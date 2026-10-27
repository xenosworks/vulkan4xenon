#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "harness.h"
#include "shaders.h"
#include "tests.h"
#include "tiled_index.h"
#include "xsdk/log.h"

// A D24S8 attachment that clears depth and loads stencil, and one that loads depth and clears stencil, as PPSSPP's
// render passes do (God of War: Chains of Olympus). Each is first drawn by a pass that clears both, a quad on its left
// half at depth 0.25 writing stencil 0x11, and kept in eDRAM; then a pass with the mixed load ops draws nothing and
// leaves it to be sampled, so its memory holds both parts: the cleared one at the pass's clear, the loaded one as
// drawn.

enum {
	SIDE = 32,
	HALF = SIDE / 2,
	CASES = 2,
	DRAWN_STENCIL = 0x11,
	DEPTH_24_MAX = 0xFFFFFF,
	// D24's rounding, as xsdk's check of the resolve allows (xsdk:tests/test_depth.c).
	DEPTH_24_TOLERANCE = 2,
	FENCE_TIMEOUT_NS = 1000000000,
	MAX_MISSES_LOGGED = 4,
};

typedef struct {
	const char *name;
	VkAttachmentLoadOp depth_load;
	VkAttachmentLoadOp stencil_load;
	VkClearDepthStencilValue clear;
} Case;

static const Case cases[CASES] = {
	{ "depth cleared, stencil loaded", VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_LOAD_OP_LOAD, { 0.5f, 0x33 } },
	{ "stencil cleared, depth loaded", VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_LOAD_OP_CLEAR, { 0.625f, 0x22 } },
};

static const float drawn_depth = 0.25f;

typedef struct {
	VkImage colors[CASES];
	VkDeviceMemory color_memories[CASES];
	VkImageView color_views[CASES];
	VkImage depths[CASES];
	VkDeviceMemory depth_memories[CASES];
	VkImageView depth_views[CASES];
	VkRenderPass draw_pass;
	VkRenderPass part_passes[CASES];
	VkFramebuffer framebuffers[CASES];
	VkPipelineLayout layout;
	VkPipeline pipeline;
	VkBuffer quad;
	VkDeviceMemory quad_memory;
	VkCommandBuffer commands;
} Scene;

static uint32_t depth_24(float depth)
{
	return (uint32_t)(depth * (double)DEPTH_24_MAX + 0.5);
}

// Colour cleared; depth and stencil each loaded or cleared, stored, from one layout into another.
static VkResult create_pass(const Harness *harness, VkAttachmentLoadOp depth_load, VkAttachmentLoadOp stencil_load,
	VkImageLayout initial, VkImageLayout final, VkRenderPass *render_pass)
{
	const VkAttachmentDescription attachments[2] = {
		{ .format = VK_FORMAT_B8G8R8A8_UNORM,
			.samples = VK_SAMPLE_COUNT_1_BIT,
			.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
			.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
			.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
			.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
			.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
			.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL },
		{ .format = VK_FORMAT_D24_UNORM_S8_UINT,
			.samples = VK_SAMPLE_COUNT_1_BIT,
			.loadOp = depth_load,
			.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
			.stencilLoadOp = stencil_load,
			.stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE,
			.initialLayout = initial,
			.finalLayout = final },
	};
	const VkAttachmentReference color = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
	const VkAttachmentReference depth = { 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
	const VkSubpassDescription subpass = {
		.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
		.colorAttachmentCount = 1,
		.pColorAttachments = &color,
		.pDepthStencilAttachment = &depth,
	};
	const VkRenderPassCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
		.attachmentCount = 2,
		.pAttachments = attachments,
		.subpassCount = 1,
		.pSubpasses = &subpass,
	};
	return vkCreateRenderPass(harness->device, &info, NULL, render_pass);
}

static VkResult build_targets(const Harness *harness, Scene *scene)
{
	VkResult result = create_pass(harness, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_LOAD_OP_CLEAR,
		VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, &scene->draw_pass);
	for (uint32_t i = 0; i < CASES && result == VK_SUCCESS; i++) {
		result = create_pass(harness, cases[i].depth_load, cases[i].stencil_load,
			VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			&scene->part_passes[i]);
		if (result == VK_SUCCESS)
			result = harness_image(harness, VK_FORMAT_B8G8R8A8_UNORM, SIDE, SIDE, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
				&scene->colors[i], &scene->color_memories[i], &scene->color_views[i]);
		if (result == VK_SUCCESS)
			result = harness_image(harness, VK_FORMAT_D24_UNORM_S8_UINT, SIDE, SIDE,
				VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, &scene->depths[i],
				&scene->depth_memories[i], &scene->depth_views[i]);
		const VkImageView views[2] = { scene->color_views[i], scene->depth_views[i] };
		const VkFramebufferCreateInfo info = {
			.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
			.renderPass = scene->draw_pass,
			.attachmentCount = 2,
			.pAttachments = views,
			.width = SIDE,
			.height = SIDE,
			.layers = 1,
		};
		if (result == VK_SUCCESS)
			result = vkCreateFramebuffer(harness->device, &info, NULL, &scene->framebuffers[i]);
	}
	return result;
}

// Depth tests less and writes; stencil always passes and takes 0x11.
static VkResult build_pipeline(const Harness *harness, Scene *scene)
{
	const VkStencilOpState stencil = { .failOp = VK_STENCIL_OP_KEEP,
		.passOp = VK_STENCIL_OP_REPLACE,
		.depthFailOp = VK_STENCIL_OP_KEEP,
		.compareOp = VK_COMPARE_OP_ALWAYS,
		.compareMask = 0xFF,
		.writeMask = 0xFF,
		.reference = DRAWN_STENCIL };
	const VkPipelineDepthStencilStateCreateInfo depth_stencil = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
		.depthTestEnable = VK_TRUE,
		.depthWriteEnable = VK_TRUE,
		.depthCompareOp = VK_COMPARE_OP_LESS,
		.stencilTestEnable = VK_TRUE,
		.front = stencil,
		.back = stencil,
	};
	VkResult result = harness_push_layout(harness, &scene->layout);
	const HarnessPipeline spec = {
		.vertex_spirv = push_vert_spirv,
		.vertex_bytes = PUSH_VERT_SPIRV_BYTES,
		.fragment_spirv = push_frag_spirv,
		.fragment_bytes = PUSH_FRAG_SPIRV_BYTES,
		.bindings = &harness_quad_binding,
		.binding_count = 1,
		.attributes = &harness_quad_attribute,
		.attribute_count = 1,
		.layout = scene->layout,
		.depth_stencil = &depth_stencil,
		.dynamic_viewport = true,
	};
	if (result == VK_SUCCESS)
		result = harness_pipeline(harness, &spec, scene->draw_pass, &scene->pipeline);
	return result;
}

static bool build(const Harness *harness, Scene *scene)
{
	const VkCommandBufferAllocateInfo commands = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = harness->pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1,
	};
	return harness_ok("targets", build_targets(harness, scene)) &&
		harness_ok("pipeline", build_pipeline(harness, scene)) &&
		harness_ok("quad", harness_quad_buffer(harness, &scene->quad, &scene->quad_memory)) &&
		harness_ok("vkAllocateCommandBuffers", vkAllocateCommandBuffers(harness->device, &commands, &scene->commands));
}

static void begin_pass(const Scene *scene, VkRenderPass render_pass, uint32_t i, VkClearDepthStencilValue depth)
{
	const VkClearValue clears[2] = { { .color = { .float32 = { 0.0f, 0.0f, 0.0f, 1.0f } } },
		{ .depthStencil = depth } };
	const VkRect2D area = { { 0, 0 }, { SIDE, SIDE } };
	const VkRenderPassBeginInfo info = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		.renderPass = render_pass,
		.framebuffer = scene->framebuffers[i],
		.renderArea = area,
		.clearValueCount = 2,
		.pClearValues = clears,
	};
	vkCmdBeginRenderPass(scene->commands, &info, VK_SUBPASS_CONTENTS_INLINE);
	const VkViewport viewport = { 0.0f, 0.0f, (float)SIDE, (float)SIDE, 0.0f, 1.0f };
	vkCmdSetViewport(scene->commands, 0, 1, &viewport);
	vkCmdSetScissor(scene->commands, 0, 1, &area);
}

// Each case's draw and its mixed pass one after the other, so eDRAM still holds the attachment; one submission.
static bool run(const Harness *harness, const Scene *scene)
{
	const VkCommandBuffer commands = scene->commands;
	const VkCommandBufferBeginInfo begin = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	if (!harness_ok("vkBeginCommandBuffer", vkBeginCommandBuffer(commands, &begin)))
		return false;
	const HarnessQuad left = { 0.0f, 0.0f, HALF, SIDE, drawn_depth, { 1.0f, 1.0f, 1.0f, 1.0f }, false };
	const VkDeviceSize offset = 0;
	for (uint32_t i = 0; i < CASES; i++) {
		begin_pass(scene, scene->draw_pass, i, (VkClearDepthStencilValue){ 1.0f, 0 });
		vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, scene->pipeline);
		vkCmdBindVertexBuffers(commands, 0, 1, &scene->quad, &offset);
		harness_push_quad_in(commands, scene->layout, &left, SIDE, SIDE);
		vkCmdEndRenderPass(commands);
		begin_pass(scene, scene->part_passes[i], i, cases[i].clear);
		vkCmdEndRenderPass(commands);
	}
	const VkSubmitInfo info = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.commandBufferCount = 1,
		.pCommandBuffers = &scene->commands,
	};
	const bool done = harness_ok("vkEndCommandBuffer", vkEndCommandBuffer(commands)) &&
		harness_ok("vkResetFences", vkResetFences(harness->device, 1, &harness->fence)) &&
		harness_ok("vkQueueSubmit", vkQueueSubmit(harness->queue, 1, &info, harness->fence)) &&
		harness_ok("vkWaitForFences", vkWaitForFences(harness->device, 1, &harness->fence, VK_TRUE, FENCE_TIMEOUT_NS));
	(void)vkResetFences(harness->device, 1, &harness->fence);
	return done;
}

static uint32_t difference(uint32_t a, uint32_t b)
{
	return a > b ? a - b : b - a;
}

// A case's words, depth above stencil, against the cleared part at the pass's clear and the loaded one as drawn: the
// left half at 0.25 and stencil 0x11, the right at the first pass's clear, 1.0 and 0.
static uint32_t words_wrong(const Harness *harness, const Scene *scene, uint32_t i)
{
	void *mapped = NULL;
	if (!harness_ok(
			"vkMapMemory", vkMapMemory(harness->device, scene->depth_memories[i], 0, VK_WHOLE_SIZE, 0, &mapped)))
		return 1;
	volatile const uint32_t *const words = mapped;
	const Case *const c = &cases[i];
	uint32_t wrong = 0;
	for (uint32_t y = 0; y < SIDE; y++) {
		for (uint32_t x = 0; x < SIDE; x++) {
			const bool drawn = x < HALF;
			const uint32_t depth = c->depth_load == VK_ATTACHMENT_LOAD_OP_CLEAR ? depth_24(c->clear.depth)
				: drawn                                                         ? depth_24(drawn_depth)
																				: DEPTH_24_MAX;
			const uint32_t stencil = c->stencil_load == VK_ATTACHMENT_LOAD_OP_CLEAR ? c->clear.stencil
				: drawn                                                             ? DRAWN_STENCIL
																					: 0;
			const uint32_t word = words[tiled_index(x, y, SIDE)];
			if (difference(word >> 8, depth) <= DEPTH_24_TOLERANCE && (word & 0xFF) == stencil)
				continue;
			if (wrong++ < MAX_MISSES_LOGGED)
				xsdk_log_write("depth parts: %s, (%u, %u) is %08X, not depth %06X above stencil %02X", c->name,
					(unsigned)x, (unsigned)y, (unsigned)word, (unsigned)depth, (unsigned)stencil);
		}
	}
	vkUnmapMemory(harness->device, scene->depth_memories[i]);
	xsdk_log_write("depth parts: %s, %u of %u words wrong", c->name, (unsigned)wrong, (unsigned)(SIDE * SIDE));
	return wrong;
}

static void destroy(const Harness *harness, const Scene *scene)
{
	const VkDevice device = harness->device;
	if (scene->commands)
		vkFreeCommandBuffers(device, harness->pool, 1, &scene->commands);
	vkDestroyBuffer(device, scene->quad, NULL);
	vkFreeMemory(device, scene->quad_memory, NULL);
	vkDestroyPipeline(device, scene->pipeline, NULL);
	vkDestroyPipelineLayout(device, scene->layout, NULL);
	for (uint32_t i = 0; i < CASES; i++) {
		vkDestroyFramebuffer(device, scene->framebuffers[i], NULL);
		vkDestroyRenderPass(device, scene->part_passes[i], NULL);
		vkDestroyImageView(device, scene->depth_views[i], NULL);
		vkDestroyImage(device, scene->depths[i], NULL);
		vkFreeMemory(device, scene->depth_memories[i], NULL);
		vkDestroyImageView(device, scene->color_views[i], NULL);
		vkDestroyImage(device, scene->colors[i], NULL);
		vkFreeMemory(device, scene->color_memories[i], NULL);
	}
	vkDestroyRenderPass(device, scene->draw_pass, NULL);
}

void test_depth_parts(const Harness *harness)
{
	static Scene scene;
	memset(&scene, 0, sizeof scene);
	const bool built = build(harness, &scene);
	check("depth parts: render passes that clear depth and load stencil, and the reverse, made", built);
	const bool ran = built && run(harness, &scene);
	check("depth parts: depth cleared and stencil loaded, after a pass that drew both, every word's depth and stencil",
		ran && words_wrong(harness, &scene, 0) == 0);
	check("depth parts: stencil cleared and depth loaded, after a pass that drew both, every word's depth and stencil",
		ran && words_wrong(harness, &scene, 1) == 0);
	destroy(harness, &scene);
}
