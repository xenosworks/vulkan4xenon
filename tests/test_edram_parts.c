#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "harness.h"
#include "shaders.h"
#include "tests.h"
#include "tiled_index.h"
#include "xsdk/log.h"

// Render passes going back and forth between framebuffers (milestone 66): three of 1280x720, 720 tiles each, more than
// eDRAM's 2048 hold, so going back to them draws one back, every texel kept; a main one with depth and a small one
// drawn between its passes, as an emulator's palette blits go, kept in eDRAM side by side so neither is drawn back;
// then part of the main one blitted into the small one on the GPU while eDRAM holds the small one past the blit's
// tiles, so the pass loading it after draws it back with the blit.

enum {
	TOLERANCE = 2,
	MAIN_WIDTH = 320,
	MAIN_HEIGHT = 192,
	SMALL_WIDTH = 64,
	SMALL_HEIGHT = 64,
	ROUNDS = 6,
	BIG_WIDTH = 1280,
	BIG_HEIGHT = 720,
	BIGS = 3,
	FENCE_TIMEOUT_NS = 1000000000,
	// The main framebuffer's texels probed, and each big one's.
	MAIN_PROBES = ROUNDS + 4,
	BIG_PROBES = 3,
	BLIT_PROBES = 3,
	// The main framebuffer's square blitted into the whole small one, inside its first quad.
	BLIT_X = 16,
	BLIT_Y = 16,
};

typedef enum {
	IMAGE_MAIN,
	IMAGE_SMALL,
	IMAGE_BIG,
	IMAGES = IMAGE_BIG + BIGS,
} ImageKind;

typedef enum {
	PASS_MAIN_CLEAR,
	PASS_MAIN_LOAD,
	PASS_SMALL,
	PASS_SMALL_LOAD,
	PASS_BIG_CLEAR,
	PASS_BIG_LOAD,
	PASSES,
} PassKind;

typedef struct {
	VkImage images[IMAGES];
	VkDeviceMemory memory[IMAGES];
	VkImageView views[IMAGES];
	VkImage depth;
	VkDeviceMemory depth_memory;
	VkImageView depth_view;
	VkRenderPass passes[PASSES];
	VkFramebuffer main;
	VkFramebuffer small;
	VkFramebuffer bigs[BIGS];
	VkPipelineLayout layout;
	VkPipeline depth_pipeline;
	VkPipeline flat_pipeline;
	VkBuffer quad;
	VkDeviceMemory quad_memory;
} Parts;

typedef struct {
	const char *name;
	uint32_t x;
	uint32_t y;
	Rgb want;
} Probe;

#define RGBA(r, g, b) { (r) / 255.0f, (g) / 255.0f, (b) / 255.0f, 1.0f }

static const VkClearValue main_clears[2] = {
	{ .color = { .float32 = RGBA(0, 0, 96) } },
	{ .depthStencil = { 1.0f, 0 } },
};
// The main framebuffer's first quad, at middle depth; each round's after a small pass; then one behind the first.
static const HarnessQuad first_quad = { 16, 16, 96, 64, 0.5f, RGBA(220, 40, 40), false };
static const float round_colors[ROUNDS][4] = { RGBA(40, 200, 40), RGBA(230, 210, 30), RGBA(40, 200, 200),
	RGBA(200, 40, 200), RGBA(250, 250, 250), RGBA(240, 128, 0) };
static const HarnessQuad behind_quad = { 0, 0, 160, 192, 0.7f, RGBA(128, 128, 128), false };
static const Probe main_probes[MAIN_PROBES - ROUNDS] = {
	{ "the first quad, in front of the last", 50, 40, { 220, 40, 40 } },
	{ "the last quad, behind nothing", 8, 150, { 128, 128, 128 } },
	{ "the clear colour", 300, 180, { 0, 0, 96 } },
	{ "the clear colour between the rounds' quads", 300, 60, { 0, 0, 96 } },
};

static const VkClearValue big_clears[BIGS] = {
	{ .color = { .float32 = RGBA(96, 0, 0) } },
	{ .color = { .float32 = RGBA(0, 96, 0) } },
	{ .color = { .float32 = RGBA(0, 0, 96) } },
};
static const float big_colors[BIGS][4] = { RGBA(220, 40, 40), RGBA(40, 200, 40), RGBA(40, 40, 220) };
// Each loaded again takes a yellow quad of its own.
static const float loaded_color[4] = RGBA(230, 210, 30);
// Drawn into the small framebuffer once the blit is in.
static const HarnessQuad after_blit_quad = { 40, 40, 16, 16, 0.0f, RGBA(40, 200, 200), false };
static const Probe blit_probes[BLIT_PROBES] = {
	{ "the main framebuffer's first quad blitted in", 8, 8, { 220, 40, 40 } },
	{ "more of it", 20, 50, { 220, 40, 40 } },
	{ "the quad drawn after the blit", 48, 48, { 40, 200, 200 } },
};

static HarnessQuad round_quad(uint32_t round)
{
	HarnessQuad quad = { 128 + 32 * (float)round, 16, 24, 24, 0.3f, { 0 }, false };
	memcpy(quad.color, round_colors[round], sizeof quad.color);
	return quad;
}

static HarnessQuad big_quad(uint32_t big)
{
	HarnessQuad quad = { 100 + 50 * (float)big, 100, 200, 150, 0.0f, { 0 }, false };
	memcpy(quad.color, big_colors[big], sizeof quad.color);
	return quad;
}

static HarnessQuad loaded_quad(uint32_t big)
{
	HarnessQuad quad = { 600 + 120 * (float)big, 400, 100, 100, 0.0f, { 0 }, false };
	memcpy(quad.color, loaded_color, sizeof quad.color);
	return quad;
}

static Rgb rgb(const float color[4])
{
	return (Rgb){ (uint32_t)(color[0] * 255.0f + 0.5f), (uint32_t)(color[1] * 255.0f + 0.5f),
		(uint32_t)(color[2] * 255.0f + 0.5f) };
}

static VkResult create_pass(const Harness *harness, VkAttachmentLoadOp load, bool depth, VkAttachmentLoadOp depth_load,
	VkRenderPass *render_pass)
{
	const VkImageLayout after = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	const VkImageLayout depth_layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	const VkAttachmentDescription attachments[2] = {
		{ .format = VK_FORMAT_B8G8R8A8_UNORM,
			.samples = VK_SAMPLE_COUNT_1_BIT,
			.loadOp = load,
			.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
			.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
			.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
			.initialLayout = load == VK_ATTACHMENT_LOAD_OP_LOAD ? after : VK_IMAGE_LAYOUT_UNDEFINED,
			.finalLayout = after },
		{ .format = VK_FORMAT_D24_UNORM_S8_UINT,
			.samples = VK_SAMPLE_COUNT_1_BIT,
			.loadOp = depth_load,
			.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
			.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
			.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
			.initialLayout = depth_load == VK_ATTACHMENT_LOAD_OP_LOAD ? depth_layout : VK_IMAGE_LAYOUT_UNDEFINED,
			.finalLayout = depth_layout },
	};
	const VkAttachmentReference color = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
	const VkAttachmentReference depth_reference = { 1, depth_layout };
	const VkSubpassDescription subpass = {
		.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
		.colorAttachmentCount = 1,
		.pColorAttachments = &color,
		.pDepthStencilAttachment = depth ? &depth_reference : NULL,
	};
	const VkRenderPassCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
		.attachmentCount = depth ? 2 : 1,
		.pAttachments = attachments,
		.subpassCount = 1,
		.pSubpasses = &subpass,
	};
	return vkCreateRenderPass(harness->device, &info, NULL, render_pass);
}

static VkResult create_framebuffer(const Harness *harness, VkRenderPass render_pass, const VkImageView *views,
	uint32_t view_count, uint32_t width, uint32_t height, VkFramebuffer *framebuffer)
{
	const VkFramebufferCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
		.renderPass = render_pass,
		.attachmentCount = view_count,
		.pAttachments = views,
		.width = width,
		.height = height,
		.layers = 1,
	};
	return vkCreateFramebuffer(harness->device, &info, NULL, framebuffer);
}

static VkResult create_targets(const Harness *harness, Parts *parts)
{
	static const uint32_t widths[IMAGES] = { MAIN_WIDTH, SMALL_WIDTH, BIG_WIDTH, BIG_WIDTH, BIG_WIDTH };
	static const uint32_t heights[IMAGES] = { MAIN_HEIGHT, SMALL_HEIGHT, BIG_HEIGHT, BIG_HEIGHT, BIG_HEIGHT };
	const VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
		VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	VkResult result = VK_SUCCESS;
	for (uint32_t i = 0; i < IMAGES && result == VK_SUCCESS; i++)
		result = harness_image(harness, VK_FORMAT_B8G8R8A8_UNORM, widths[i], heights[i], usage, &parts->images[i],
			&parts->memory[i], &parts->views[i]);
	if (result == VK_SUCCESS)
		result = harness_image(harness, VK_FORMAT_D24_UNORM_S8_UINT, MAIN_WIDTH, MAIN_HEIGHT,
			VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, &parts->depth, &parts->depth_memory, &parts->depth_view);
	const VkAttachmentLoadOp clear = VK_ATTACHMENT_LOAD_OP_CLEAR;
	const VkAttachmentLoadOp load = VK_ATTACHMENT_LOAD_OP_LOAD;
	const VkAttachmentLoadOp dont_care = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	if (result == VK_SUCCESS)
		result = create_pass(harness, clear, true, clear, &parts->passes[PASS_MAIN_CLEAR]);
	if (result == VK_SUCCESS)
		result = create_pass(harness, load, true, load, &parts->passes[PASS_MAIN_LOAD]);
	if (result == VK_SUCCESS)
		result = create_pass(harness, dont_care, false, dont_care, &parts->passes[PASS_SMALL]);
	if (result == VK_SUCCESS)
		result = create_pass(harness, load, false, dont_care, &parts->passes[PASS_SMALL_LOAD]);
	if (result == VK_SUCCESS)
		result = create_pass(harness, clear, false, dont_care, &parts->passes[PASS_BIG_CLEAR]);
	if (result == VK_SUCCESS)
		result = create_pass(harness, load, false, dont_care, &parts->passes[PASS_BIG_LOAD]);
	const VkImageView main_views[2] = { parts->views[IMAGE_MAIN], parts->depth_view };
	if (result == VK_SUCCESS)
		result = create_framebuffer(
			harness, parts->passes[PASS_MAIN_CLEAR], main_views, 2, MAIN_WIDTH, MAIN_HEIGHT, &parts->main);
	if (result == VK_SUCCESS)
		result = create_framebuffer(harness, parts->passes[PASS_SMALL], &parts->views[IMAGE_SMALL], 1, SMALL_WIDTH,
			SMALL_HEIGHT, &parts->small);
	for (uint32_t i = 0; i < BIGS && result == VK_SUCCESS; i++)
		result = create_framebuffer(harness, parts->passes[PASS_BIG_CLEAR], &parts->views[IMAGE_BIG + i], 1, BIG_WIDTH,
			BIG_HEIGHT, &parts->bigs[i]);
	return result;
}

static VkResult create_pipeline(const Harness *harness, const Parts *parts, bool depth, VkPipeline *pipeline)
{
	const VkPipelineDepthStencilStateCreateInfo depth_state = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
		.depthTestEnable = VK_TRUE,
		.depthWriteEnable = VK_TRUE,
		.depthCompareOp = VK_COMPARE_OP_LESS,
	};
	const HarnessPipeline spec = {
		.vertex_spirv = push_vert_spirv,
		.vertex_bytes = PUSH_VERT_SPIRV_BYTES,
		.fragment_spirv = push_frag_spirv,
		.fragment_bytes = PUSH_FRAG_SPIRV_BYTES,
		.bindings = &harness_quad_binding,
		.binding_count = 1,
		.attributes = &harness_quad_attribute,
		.attribute_count = 1,
		.layout = parts->layout,
		.depth_stencil = depth ? &depth_state : NULL,
		.dynamic_viewport = true,
	};
	return harness_pipeline(harness, &spec, parts->passes[depth ? PASS_MAIN_CLEAR : PASS_SMALL], pipeline);
}

static bool build(const Harness *harness, Parts *parts)
{
	bool ok = harness_ok("edram parts targets", create_targets(harness, parts));
	ok = ok && harness_ok("edram parts layout", harness_push_layout(harness, &parts->layout));
	ok = ok && harness_ok("edram parts quad", harness_quad_buffer(harness, &parts->quad, &parts->quad_memory));
	ok = ok && harness_ok("edram parts pipeline", create_pipeline(harness, parts, true, &parts->depth_pipeline));
	return ok && harness_ok("edram parts pipeline", create_pipeline(harness, parts, false, &parts->flat_pipeline));
}

// A pass over the whole framebuffer, its viewport and scissor over it, drawing with the pipeline.
static void begin(VkCommandBuffer commands, const Parts *parts, VkRenderPass render_pass, VkFramebuffer framebuffer,
	uint32_t width, uint32_t height, const VkClearValue *clears, uint32_t clear_count, VkPipeline pipeline)
{
	const VkRect2D whole = { { 0, 0 }, { width, height } };
	const VkRenderPassBeginInfo info = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		.renderPass = render_pass,
		.framebuffer = framebuffer,
		.renderArea = whole,
		.clearValueCount = clear_count,
		.pClearValues = clears,
	};
	vkCmdBeginRenderPass(commands, &info, VK_SUBPASS_CONTENTS_INLINE);
	const VkViewport viewport = { 0.0f, 0.0f, (float)width, (float)height, 0.0f, 1.0f };
	vkCmdSetViewport(commands, 0, 1, &viewport);
	vkCmdSetScissor(commands, 0, 1, &whole);
	const VkDeviceSize offset = 0;
	vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
	vkCmdBindVertexBuffers(commands, 0, 1, &parts->quad, &offset);
}

// The main framebuffer, then each round a small pass and the main loaded again, then a quad behind the first.
static void record_main(VkCommandBuffer commands, const Parts *parts)
{
	const VkRenderPass load = parts->passes[PASS_MAIN_LOAD];
	begin(commands, parts, parts->passes[PASS_MAIN_CLEAR], parts->main, MAIN_WIDTH, MAIN_HEIGHT, main_clears, 2,
		parts->depth_pipeline);
	harness_push_quad_in(commands, parts->layout, &first_quad, MAIN_WIDTH, MAIN_HEIGHT);
	vkCmdEndRenderPass(commands);
	for (uint32_t r = 0; r < ROUNDS; r++) {
		begin(commands, parts, parts->passes[PASS_SMALL], parts->small, SMALL_WIDTH, SMALL_HEIGHT, NULL, 0,
			parts->flat_pipeline);
		HarnessQuad fill = { 0, 0, SMALL_WIDTH, SMALL_HEIGHT, 0.0f, { 0 }, false };
		memcpy(fill.color, round_colors[r], sizeof fill.color);
		harness_push_quad_in(commands, parts->layout, &fill, SMALL_WIDTH, SMALL_HEIGHT);
		vkCmdEndRenderPass(commands);
		begin(commands, parts, load, parts->main, MAIN_WIDTH, MAIN_HEIGHT, NULL, 0, parts->depth_pipeline);
		const HarnessQuad quad = round_quad(r);
		harness_push_quad_in(commands, parts->layout, &quad, MAIN_WIDTH, MAIN_HEIGHT);
		vkCmdEndRenderPass(commands);
	}
	begin(commands, parts, load, parts->main, MAIN_WIDTH, MAIN_HEIGHT, NULL, 0, parts->depth_pipeline);
	harness_push_quad_in(commands, parts->layout, &behind_quad, MAIN_WIDTH, MAIN_HEIGHT);
	vkCmdEndRenderPass(commands);
}

// The three big framebuffers, then each loaded again with a quad of its own.
static void record_bigs(VkCommandBuffer commands, const Parts *parts)
{
	for (uint32_t b = 0; b < BIGS; b++) {
		begin(commands, parts, parts->passes[PASS_BIG_CLEAR], parts->bigs[b], BIG_WIDTH, BIG_HEIGHT, &big_clears[b], 1,
			parts->flat_pipeline);
		const HarnessQuad quad = big_quad(b);
		harness_push_quad_in(commands, parts->layout, &quad, BIG_WIDTH, BIG_HEIGHT);
		vkCmdEndRenderPass(commands);
	}
	for (uint32_t b = 0; b < BIGS; b++) {
		begin(commands, parts, parts->passes[PASS_BIG_LOAD], parts->bigs[b], BIG_WIDTH, BIG_HEIGHT, NULL, 0,
			parts->flat_pipeline);
		const HarnessQuad quad = loaded_quad(b);
		harness_push_quad_in(commands, parts->layout, &quad, BIG_WIDTH, BIG_HEIGHT);
		vkCmdEndRenderPass(commands);
	}
}

// A square of the main framebuffer blitted over the whole small one, drawn on the GPU, then the small one loaded with a
// quad.
static void record_blit(VkCommandBuffer commands, const Parts *parts)
{
	const VkImageSubresourceLayers layers = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
	const VkImageBlit region = {
		.srcSubresource = layers,
		.srcOffsets = { { BLIT_X, BLIT_Y, 0 }, { BLIT_X + SMALL_WIDTH, BLIT_Y + SMALL_HEIGHT, 1 } },
		.dstSubresource = layers,
		.dstOffsets = { { 0, 0, 0 }, { SMALL_WIDTH, SMALL_HEIGHT, 1 } },
	};
	vkCmdBlitImage(commands, parts->images[IMAGE_MAIN], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		parts->images[IMAGE_SMALL], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, VK_FILTER_NEAREST);
	begin(commands, parts, parts->passes[PASS_SMALL_LOAD], parts->small, SMALL_WIDTH, SMALL_HEIGHT, NULL, 0,
		parts->flat_pipeline);
	harness_push_quad_in(commands, parts->layout, &after_blit_quad, SMALL_WIDTH, SMALL_HEIGHT);
	vkCmdEndRenderPass(commands);
}

// Records with record into a command buffer of its own, runs it and waits for it.
static bool run(const Harness *harness, const Parts *parts, void (*record)(VkCommandBuffer, const Parts *))
{
	const VkCommandBufferAllocateInfo allocate_info = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = harness->pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1,
	};
	VkCommandBuffer commands = VK_NULL_HANDLE;
	if (!harness_ok("edram parts command buffer", vkAllocateCommandBuffers(harness->device, &allocate_info, &commands)))
		return false;
	const VkCommandBufferBeginInfo begin_info = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	(void)vkBeginCommandBuffer(commands, &begin_info);
	record(commands, parts);
	bool ok = harness_ok("edram parts recording", vkEndCommandBuffer(commands));
	const VkSubmitInfo submit = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &commands
	};
	ok = ok && harness_ok("edram parts submit", vkQueueSubmit(harness->queue, 1, &submit, harness->fence));
	ok = ok &&
		harness_ok("edram parts wait", vkWaitForFences(harness->device, 1, &harness->fence, VK_TRUE, FENCE_TIMEOUT_NS));
	(void)vkResetFences(harness->device, 1, &harness->fence);
	vkFreeCommandBuffers(harness->device, harness->pool, 1, &commands);
	return ok;
}

// The probes an image misses, read back where the tiled layout puts each texel (B8G8R8A8: blue first); the first few
// logged.
static uint32_t misses(
	const Harness *harness, const Parts *parts, ImageKind image, uint32_t width, const Probe *probes, uint32_t count)
{
	void *mapped = NULL;
	if (!harness_ok(
			"edram parts map", vkMapMemory(harness->device, parts->memory[image], 0, VK_WHOLE_SIZE, 0, &mapped)))
		return count;
	uint32_t missed = 0;
	for (uint32_t i = 0; i < count; i++) {
		const uint8_t *const bytes = (const uint8_t *)mapped + tiled_index(probes[i].x, probes[i].y, width) * 4;
		const Rgb texel = { bytes[2], bytes[1], bytes[0] };
		if (harness_near(texel, probes[i].want, TOLERANCE))
			continue;
		if (missed++ < 4)
			xsdk_log_write("edram parts: image %u, %s at (%u, %u) is %u %u %u, not %u %u %u", (unsigned)image,
				probes[i].name, (unsigned)probes[i].x, (unsigned)probes[i].y, (unsigned)texel.red,
				(unsigned)texel.green, (unsigned)texel.blue, (unsigned)probes[i].want.red,
				(unsigned)probes[i].want.green, (unsigned)probes[i].want.blue);
	}
	vkUnmapMemory(harness->device, parts->memory[image]);
	return missed;
}

static uint32_t main_misses(const Harness *harness, const Parts *parts)
{
	Probe probes[MAIN_PROBES];
	for (uint32_t r = 0; r < ROUNDS; r++) {
		const HarnessQuad quad = round_quad(r);
		probes[r] = (Probe){ "a round's quad", (uint32_t)quad.x + 12, 28, rgb(round_colors[r]) };
	}
	memcpy(&probes[ROUNDS], main_probes, sizeof main_probes);
	const Probe small = { "the small framebuffer's last fill", 32, 32, rgb(round_colors[ROUNDS - 1]) };
	return misses(harness, parts, IMAGE_MAIN, MAIN_WIDTH, probes, MAIN_PROBES) +
		misses(harness, parts, IMAGE_SMALL, SMALL_WIDTH, &small, 1);
}

static uint32_t big_misses(const Harness *harness, const Parts *parts)
{
	uint32_t missed = 0;
	for (uint32_t b = 0; b < BIGS; b++) {
		const HarnessQuad quad = big_quad(b);
		const HarnessQuad loaded = loaded_quad(b);
		const Probe probes[BIG_PROBES] = {
			{ "the clear colour", 20, 20, rgb(big_clears[b].color.float32) },
			{ "the first quad", (uint32_t)quad.x + 40, 200, rgb(big_colors[b]) },
			{ "the quad drawn once loaded again", (uint32_t)loaded.x + 50, 450, rgb(loaded_color) },
		};
		missed += misses(harness, parts, (ImageKind)(IMAGE_BIG + b), BIG_WIDTH, probes, BIG_PROBES);
	}
	return missed;
}

static void destroy(const Harness *harness, Parts *parts)
{
	const VkDevice device = harness->device;
	vkDestroyBuffer(device, parts->quad, NULL);
	vkFreeMemory(device, parts->quad_memory, NULL);
	vkDestroyPipeline(device, parts->flat_pipeline, NULL);
	vkDestroyPipeline(device, parts->depth_pipeline, NULL);
	vkDestroyPipelineLayout(device, parts->layout, NULL);
	for (uint32_t i = 0; i < BIGS; i++)
		vkDestroyFramebuffer(device, parts->bigs[i], NULL);
	vkDestroyFramebuffer(device, parts->small, NULL);
	vkDestroyFramebuffer(device, parts->main, NULL);
	for (uint32_t i = 0; i < PASSES; i++)
		vkDestroyRenderPass(device, parts->passes[i], NULL);
	vkDestroyImageView(device, parts->depth_view, NULL);
	vkDestroyImage(device, parts->depth, NULL);
	vkFreeMemory(device, parts->depth_memory, NULL);
	for (uint32_t i = 0; i < IMAGES; i++) {
		vkDestroyImageView(device, parts->views[i], NULL);
		vkDestroyImage(device, parts->images[i], NULL);
		vkFreeMemory(device, parts->memory[i], NULL);
	}
}

void test_edram_parts(const Harness *harness)
{
	static Parts parts;
	memset(&parts, 0, sizeof parts);
	const bool built = build(harness, &parts);
	check("edram parts: five images, six render passes and two pipelines made", built);
	// The big ones first, so one holds tile 0 and the main one goes past the blit's tiles.
	const uint32_t before = v4x_edram_counts().drawn_back;
	const bool bigs_ran = built && run(harness, &parts, record_bigs);
	const uint32_t big_drawn = v4x_edram_counts().drawn_back - before;
	check(
		"edram parts: three 1280x720 framebuffers loaded again, eDRAM too small for them all, one drawn back at least",
		bigs_ran && big_drawn >= 1);
	check("edram parts: each holds its clear colour, its quad and the quad drawn once loaded again",
		bigs_ran && big_misses(harness, &parts) == 0);
	const uint32_t between = v4x_edram_counts().drawn_back;
	const bool main_ran = built && run(harness, &parts, record_main);
	const uint32_t main_drawn = v4x_edram_counts().drawn_back - between;
	check("edram parts: a main framebuffer with depth loaded after each of 6 small passes, none drawn back",
		main_ran && main_drawn == 0);
	check("edram parts: the main framebuffer holds every quad, its depth kept, and the small one its last",
		main_ran && main_misses(harness, &parts) == 0);
	const bool blit_ran = main_ran && run(harness, &parts, record_blit);
	check("edram parts: the small framebuffer loaded after a blit into it on the GPU holds the blit, drawn back",
		blit_ran && misses(harness, &parts, IMAGE_SMALL, SMALL_WIDTH, blit_probes, BLIT_PROBES) == 0);
	xsdk_log_write("edram parts: drawn back %u with the small passes, %u going back to the big ones",
		(unsigned)main_drawn, (unsigned)big_drawn);
	destroy(harness, &parts);
}
