#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "harness.h"
#include "shaders.h"
#include "tests.h"
#include "xsdk/log.h"

// Milestone 27's step 1: render passes whose render area is smaller than the framebuffer clear, load and store their
// area alone. The target is read back through vkCmdCopyImageToBuffer, into cached memory, and checked texel by texel:
// each area as its pass left it, and every texel around it as it was. Milestone 67: areas off the 8-pixel grid too, as
// PPSSPP's passes take the bounds of their draws (God of War: Chains of Olympus).

enum {
	FENCE_TIMEOUT_NS = 1000000000,
	COLOR = VK_IMAGE_ASPECT_COLOR_BIT,
	DEPTH_STENCIL = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
	// Off the 8-pixel grid both ways, so an area reaching the edges ends off it.
	WIDTH = 124,
	HEIGHT = 92,
	// Another target's width, so its pass leaves eDRAM holding what the target doesn't, which the next pass doesn't
	// take as its clear.
	OTHER_WIDTH = 160,
	TEXEL_BYTES = 4,
	IMAGE_BYTES = WIDTH * HEIGHT * TEXEL_BYTES,
	// Where each read-back lands: after the colour areas, after the depth area, after the depth image's clear.
	AREAS_AT = 0,
	DEPTH_AT = AREAS_AT + IMAGE_BYTES,
	IMAGE_CLEAR_AT = DEPTH_AT + IMAGE_BYTES,
	OFF_GRID_AT = IMAGE_CLEAR_AT + IMAGE_BYTES,
	OFF_DEPTH_AT = OFF_GRID_AT + IMAGE_BYTES,
	READBACK_BYTES = OFF_DEPTH_AT + IMAGE_BYTES,
	// The memory type the CPU caches, which buffers only copied to and from may take.
	CACHED_TYPE = 1,
	MAX_MISSES_LOGGED = 4,
};

typedef enum {
	PASS_CLEAR,
	PASS_LOAD,
	// Into a half float target, of which xsdk resolves no rectangle, so a smaller area is refused.
	PASS_FLOAT,
	// Colour and depth both cleared; colour loaded and depth cleared; both loaded.
	PASS_DEPTH_CLEAR,
	PASS_DEPTH_RECLEAR,
	PASS_DEPTH_LOAD,
	PASSES,
} PassKind;

static const uint8_t blue[4] = { 0, 0, 255, 255 };
static const uint8_t green[4] = { 0, 255, 0, 255 };
static const uint8_t red[4] = { 255, 0, 0, 255 };
static const uint8_t yellow[4] = { 255, 255, 0, 255 };
static const uint8_t grey[4] = { 128, 128, 128, 255 };
static const uint8_t cyan[4] = { 0, 255, 255, 255 };

// A cleared area inside the target, with a quad in it; then a loaded one reaching its right and bottom edges, over a
// corner of the first, with a quad of its own.
static const VkRect2D clear_area = { { 32, 16 }, { 48, 40 } };
static const VkRect2D load_area = { { 64, 48 }, { WIDTH - 64, HEIGHT - 48 } };
static const HarnessQuad clear_quad = { 40.0f, 24.0f, 16.0f, 16.0f, 0.0f, { 1.0f, 0.0f, 0.0f, 1.0f }, false };
static const HarnessQuad load_quad = { 96.0f, 64.0f, 16.0f, 16.0f, 0.0f, { 1.0f, 1.0f, 0.0f, 1.0f }, false };
// Depth cleared to 1 there after 0.5 over the whole; a quad at 0.75 over the whole then shows there alone.
static const VkRect2D depth_area = { { 16, 56 }, { 32, 24 } };
static const HarnessQuad depth_quad = { 0.0f, 0.0f, WIDTH, HEIGHT, 0.75f, { 0.0f, 1.0f, 1.0f, 1.0f }, false };
// The depth image cleared to 0.25 outside a pass, then a pass of that area loading it; a quad at 0.4 over the whole
// after hides everywhere, if the image's clear reached past the area.
static const VkClearDepthStencilValue image_clear = { 0.25f, 0 };
static const HarnessQuad hidden_quad = { 0.0f, 0.0f, WIDTH, HEIGHT, 0.4f, { 1.0f, 0.5f, 0.0f, 1.0f }, false };

// Areas off the grid every way: a cleared one with a quad past it, which the scissor keeps inside; a loaded one with a
// quad over it; and one whose depth a pass clears to 1 after 0.5 over the whole, a quad at 0.75 then showing there
// alone.
static const VkRect2D off_clear_area = { { 5, 3 }, { 37, 29 } };
static const VkRect2D off_load_area = { { 70, 61 }, { 19, 13 } };
static const VkRect2D off_depth_area = { { 9, 45 }, { 23, 17 } };
static const HarnessQuad off_clear_quad = { 14.0f, 9.0f, 30.0f, 30.0f, 0.0f, { 1.0f, 0.0f, 0.0f, 1.0f }, false };
static const HarnessQuad off_load_quad = { 60.0f, 55.0f, 40.0f, 30.0f, 0.0f, { 1.0f, 1.0f, 0.0f, 1.0f }, false };

typedef struct {
	VkRenderPass passes[PASSES];
	VkImage target;
	VkDeviceMemory target_memory;
	VkImageView target_view;
	VkImage other;
	VkDeviceMemory other_memory;
	VkImageView other_view;
	VkImage half;
	VkDeviceMemory half_memory;
	VkImageView half_view;
	VkImage depth;
	VkDeviceMemory depth_memory;
	VkImageView depth_view;
	VkFramebuffer target_framebuffer;
	VkFramebuffer other_framebuffer;
	VkFramebuffer half_framebuffer;
	VkFramebuffer depth_framebuffer;
	VkPipelineLayout layout;
	VkPipeline pipeline;
	VkPipeline depth_pipeline;
	VkBuffer vertices;
	VkDeviceMemory vertex_memory;
	VkBuffer readback;
	VkDeviceMemory readback_memory;
	const uint8_t *read;
	VkCommandBuffer commands;
} Scene;

static VkResult make_render_pass(const Harness *harness, PassKind kind, VkRenderPass *render_pass)
{
	const bool loads = kind == PASS_LOAD || kind == PASS_DEPTH_RECLEAR || kind == PASS_DEPTH_LOAD;
	const bool depth = kind >= PASS_DEPTH_CLEAR;
	const VkAttachmentLoadOp depth_load =
		kind == PASS_DEPTH_LOAD ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
	const VkAttachmentDescription attachments[2] = {
		{ .format = kind == PASS_FLOAT ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM,
			.samples = VK_SAMPLE_COUNT_1_BIT,
			.loadOp = loads ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR,
			.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
			.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
			.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
			.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL },
		{ .format = VK_FORMAT_D24_UNORM_S8_UINT,
			.samples = VK_SAMPLE_COUNT_1_BIT,
			.loadOp = depth_load,
			.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
			.stencilLoadOp = depth_load,
			.stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE,
			.initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
			.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL },
	};
	const VkAttachmentReference color = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
	const VkAttachmentReference depth_reference = { 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
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

static VkResult make_framebuffer(const Harness *harness, VkRenderPass render_pass, const VkImageView *views,
	uint32_t view_count, uint32_t width, VkFramebuffer *framebuffer)
{
	const VkFramebufferCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
		.renderPass = render_pass,
		.attachmentCount = view_count,
		.pAttachments = views,
		.width = width,
		.height = HEIGHT,
		.layers = 1,
	};
	return vkCreateFramebuffer(harness->device, &info, NULL, framebuffer);
}

static bool build_images(const Harness *harness, Scene *scene)
{
	const VkImageUsageFlags color =
		VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	const VkImageUsageFlags depth = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	VkResult result = harness_image(harness, VK_FORMAT_R8G8B8A8_UNORM, WIDTH, HEIGHT, color, &scene->target,
		&scene->target_memory, &scene->target_view);
	if (result == VK_SUCCESS)
		result = harness_image(harness, VK_FORMAT_R8G8B8A8_UNORM, OTHER_WIDTH, HEIGHT,
			VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, &scene->other, &scene->other_memory, &scene->other_view);
	if (result == VK_SUCCESS)
		result = harness_image(harness, VK_FORMAT_R16G16B16A16_SFLOAT, WIDTH, HEIGHT,
			VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, &scene->half, &scene->half_memory, &scene->half_view);
	if (result == VK_SUCCESS)
		result = harness_image(harness, VK_FORMAT_D24_UNORM_S8_UINT, WIDTH, HEIGHT, depth, &scene->depth,
			&scene->depth_memory, &scene->depth_view);
	for (uint32_t i = 0; i < PASSES && result == VK_SUCCESS; i++)
		result = make_render_pass(harness, (PassKind)i, &scene->passes[i]);
	const VkRenderPass color_pass = scene->passes[PASS_CLEAR];
	if (result == VK_SUCCESS)
		result = make_framebuffer(harness, color_pass, &scene->target_view, 1, WIDTH, &scene->target_framebuffer);
	if (result == VK_SUCCESS)
		result = make_framebuffer(harness, color_pass, &scene->other_view, 1, OTHER_WIDTH, &scene->other_framebuffer);
	if (result == VK_SUCCESS)
		result =
			make_framebuffer(harness, scene->passes[PASS_FLOAT], &scene->half_view, 1, WIDTH, &scene->half_framebuffer);
	const VkImageView depth_views[2] = { scene->target_view, scene->depth_view };
	if (result == VK_SUCCESS)
		result = make_framebuffer(
			harness, scene->passes[PASS_DEPTH_CLEAR], depth_views, 2, WIDTH, &scene->depth_framebuffer);
	return harness_ok("the images, render passes and framebuffers", result);
}

static bool build_drawing(const Harness *harness, Scene *scene)
{
	const VkPipelineDepthStencilStateCreateInfo less = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
		.depthTestEnable = VK_TRUE,
		.depthWriteEnable = VK_TRUE,
		.depthCompareOp = VK_COMPARE_OP_LESS,
	};
	HarnessPipeline spec = {
		.vertex_spirv = push_vert_spirv,
		.vertex_bytes = PUSH_VERT_SPIRV_BYTES,
		.fragment_spirv = push_frag_spirv,
		.fragment_bytes = PUSH_FRAG_SPIRV_BYTES,
		.bindings = &harness_quad_binding,
		.binding_count = 1,
		.attributes = &harness_quad_attribute,
		.attribute_count = 1,
		.dynamic_viewport = true,
	};
	bool ok = harness_ok("push layout", harness_push_layout(harness, &scene->layout));
	spec.layout = scene->layout;
	ok = ok && harness_ok("pipeline", harness_pipeline(harness, &spec, scene->passes[PASS_CLEAR], &scene->pipeline));
	spec.depth_stencil = &less;
	ok = ok &&
		harness_ok("depth pipeline",
			harness_pipeline(harness, &spec, scene->passes[PASS_DEPTH_CLEAR], &scene->depth_pipeline));
	ok = ok && harness_ok("vertex buffer", harness_quad_buffer(harness, &scene->vertices, &scene->vertex_memory));
	const VkBufferCreateInfo buffer = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = READBACK_BYTES, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT
	};
	ok = ok && harness_ok("vkCreateBuffer", vkCreateBuffer(harness->device, &buffer, NULL, &scene->readback));
	if (!ok)
		return false;
	VkMemoryRequirements requirements;
	vkGetBufferMemoryRequirements(harness->device, scene->readback, &requirements);
	const VkMemoryAllocateInfo allocate = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = requirements.size,
		.memoryTypeIndex = CACHED_TYPE,
	};
	void *mapped = NULL;
	ok = harness_ok("vkAllocateMemory", vkAllocateMemory(harness->device, &allocate, NULL, &scene->readback_memory)) &&
		harness_ok(
			"vkBindBufferMemory", vkBindBufferMemory(harness->device, scene->readback, scene->readback_memory, 0)) &&
		harness_ok("vkMapMemory", vkMapMemory(harness->device, scene->readback_memory, 0, VK_WHOLE_SIZE, 0, &mapped));
	scene->read = mapped;
	const VkCommandBufferAllocateInfo commands = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = harness->pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1,
	};
	return ok &&
		harness_ok("vkAllocateCommandBuffers", vkAllocateCommandBuffers(harness->device, &commands, &scene->commands));
}

static bool begin(VkCommandBuffer commands)
{
	const VkCommandBufferBeginInfo info = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	return harness_ok("vkBeginCommandBuffer", vkBeginCommandBuffer(commands, &info));
}

static bool submit(const Harness *harness, VkCommandBuffer commands)
{
	const VkSubmitInfo info = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.commandBufferCount = 1,
		.pCommandBuffers = &commands,
	};
	const bool done = harness_ok("vkEndCommandBuffer", vkEndCommandBuffer(commands)) &&
		harness_ok("vkQueueSubmit", vkQueueSubmit(harness->queue, 1, &info, harness->fence)) &&
		harness_ok("vkWaitForFences", vkWaitForFences(harness->device, 1, &harness->fence, VK_TRUE, FENCE_TIMEOUT_NS));
	(void)vkResetFences(harness->device, 1, &harness->fence);
	return done;
}

// A pass over its area, the viewport over the whole framebuffer and the scissor at the area, as Vulkan has the app
// keep its draws inside it.
static void begin_pass(const Scene *scene, PassKind kind, VkFramebuffer framebuffer, uint32_t width,
	const VkRect2D *area, const VkClearValue *clears)
{
	const VkRenderPassBeginInfo info = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		.renderPass = scene->passes[kind],
		.framebuffer = framebuffer,
		.renderArea = *area,
		.clearValueCount = clears ? 2 : 0,
		.pClearValues = clears,
	};
	vkCmdBeginRenderPass(scene->commands, &info, VK_SUBPASS_CONTENTS_INLINE);
	const VkViewport viewport = { 0.0f, 0.0f, (float)width, HEIGHT, 0.0f, 1.0f };
	vkCmdSetViewport(scene->commands, 0, 1, &viewport);
	vkCmdSetScissor(scene->commands, 0, 1, area);
	vkCmdBindPipeline(scene->commands, VK_PIPELINE_BIND_POINT_GRAPHICS,
		kind >= PASS_DEPTH_CLEAR ? scene->depth_pipeline : scene->pipeline);
	const VkDeviceSize offset = 0;
	vkCmdBindVertexBuffers(scene->commands, 0, 1, &scene->vertices, &offset);
}

static VkClearValue color_clear(const uint8_t rgba[4])
{
	return (VkClearValue){ .color = { .float32 = {
										  rgba[0] / 255.0f, rgba[1] / 255.0f, rgba[2] / 255.0f, rgba[3] / 255.0f } } };
}

static void copy_out(const Scene *scene, uint32_t at)
{
	const VkBufferImageCopy region = {
		.bufferOffset = at,
		.imageSubresource = { COLOR, 0, 0, 1 },
		.imageExtent = { WIDTH, HEIGHT, 1 },
	};
	vkCmdCopyImageToBuffer(
		scene->commands, scene->target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, scene->readback, 1, &region);
}

// The target cleared blue on the CPU; another target's pass, which leaves eDRAM magenta; the cleared area, green with
// a red quad; and the loaded one, which finds the first's corner and blue, with a yellow quad.
static void record_areas(const Scene *scene)
{
	const VkClearColorValue blue_clear = { .float32 = { 0.0f, 0.0f, 1.0f, 1.0f } };
	const VkImageSubresourceRange range = { COLOR, 0, 1, 0, 1 };
	vkCmdClearColorImage(scene->commands, scene->target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &blue_clear, 1, &range);
	const VkRect2D other_area = { { 0, 0 }, { OTHER_WIDTH, HEIGHT } };
	const VkClearValue magenta[2] = { color_clear((const uint8_t[4]){ 255, 0, 255, 255 }) };
	begin_pass(scene, PASS_CLEAR, scene->other_framebuffer, OTHER_WIDTH, &other_area, magenta);
	vkCmdEndRenderPass(scene->commands);
	const VkClearValue green_clear[2] = { color_clear(green) };
	begin_pass(scene, PASS_CLEAR, scene->target_framebuffer, WIDTH, &clear_area, green_clear);
	harness_push_quad_in(scene->commands, scene->layout, &clear_quad, WIDTH, HEIGHT);
	vkCmdEndRenderPass(scene->commands);
	begin_pass(scene, PASS_LOAD, scene->target_framebuffer, WIDTH, &load_area, NULL);
	harness_push_quad_in(scene->commands, scene->layout, &load_quad, WIDTH, HEIGHT);
	vkCmdEndRenderPass(scene->commands);
	copy_out(scene, AREAS_AT);
}

// The target cleared blue again, then the areas off the grid: the cleared one's pass and the loaded one's; then the
// depth sequence above in the depth area off the grid.
static void record_off_grid(const Scene *scene)
{
	const VkClearColorValue blue_clear = { .float32 = { 0.0f, 0.0f, 1.0f, 1.0f } };
	const VkImageSubresourceRange range = { COLOR, 0, 1, 0, 1 };
	vkCmdClearColorImage(scene->commands, scene->target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &blue_clear, 1, &range);
	const VkClearValue green_clear[2] = { color_clear(green) };
	begin_pass(scene, PASS_CLEAR, scene->target_framebuffer, WIDTH, &off_clear_area, green_clear);
	harness_push_quad_in(scene->commands, scene->layout, &off_clear_quad, WIDTH, HEIGHT);
	vkCmdEndRenderPass(scene->commands);
	begin_pass(scene, PASS_LOAD, scene->target_framebuffer, WIDTH, &off_load_area, NULL);
	harness_push_quad_in(scene->commands, scene->layout, &off_load_quad, WIDTH, HEIGHT);
	vkCmdEndRenderPass(scene->commands);
	copy_out(scene, OFF_GRID_AT);
	const VkRect2D all = { { 0, 0 }, { WIDTH, HEIGHT } };
	const VkClearValue first[2] = { color_clear(grey), { .depthStencil = { 0.5f, 0 } } };
	const VkClearValue far[2] = { color_clear(grey), { .depthStencil = { 1.0f, 0 } } };
	begin_pass(scene, PASS_DEPTH_CLEAR, scene->depth_framebuffer, WIDTH, &all, first);
	vkCmdEndRenderPass(scene->commands);
	begin_pass(scene, PASS_DEPTH_RECLEAR, scene->depth_framebuffer, WIDTH, &off_depth_area, far);
	vkCmdEndRenderPass(scene->commands);
	begin_pass(scene, PASS_DEPTH_LOAD, scene->depth_framebuffer, WIDTH, &all, NULL);
	harness_push_quad_in(scene->commands, scene->layout, &depth_quad, WIDTH, HEIGHT);
	vkCmdEndRenderPass(scene->commands);
	copy_out(scene, OFF_DEPTH_AT);
}

// Grey colour and depth 0.5 over the whole; depth 1 in the depth area; a quad at 0.75 over the whole, testing less.
// Then the depth image cleared to 0.25, a pass of the depth area loading it, and a quad at 0.4 over the whole.
static void record_depth(const Scene *scene)
{
	const VkRect2D all = { { 0, 0 }, { WIDTH, HEIGHT } };
	const VkClearValue first[2] = { color_clear(grey), { .depthStencil = { 0.5f, 0 } } };
	const VkClearValue far[2] = { color_clear(grey), { .depthStencil = { 1.0f, 0 } } };
	begin_pass(scene, PASS_DEPTH_CLEAR, scene->depth_framebuffer, WIDTH, &all, first);
	vkCmdEndRenderPass(scene->commands);
	begin_pass(scene, PASS_DEPTH_RECLEAR, scene->depth_framebuffer, WIDTH, &depth_area, far);
	vkCmdEndRenderPass(scene->commands);
	begin_pass(scene, PASS_DEPTH_LOAD, scene->depth_framebuffer, WIDTH, &all, NULL);
	harness_push_quad_in(scene->commands, scene->layout, &depth_quad, WIDTH, HEIGHT);
	vkCmdEndRenderPass(scene->commands);
	copy_out(scene, DEPTH_AT);
	const VkImageSubresourceRange range = { DEPTH_STENCIL, 0, 1, 0, 1 };
	vkCmdClearDepthStencilImage(
		scene->commands, scene->depth, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &image_clear, 1, &range);
	begin_pass(scene, PASS_DEPTH_LOAD, scene->depth_framebuffer, WIDTH, &depth_area, NULL);
	vkCmdEndRenderPass(scene->commands);
	begin_pass(scene, PASS_DEPTH_LOAD, scene->depth_framebuffer, WIDTH, &all, NULL);
	harness_push_quad_in(scene->commands, scene->layout, &hidden_quad, WIDTH, HEIGHT);
	vkCmdEndRenderPass(scene->commands);
	copy_out(scene, IMAGE_CLEAR_AT);
}

static bool inside(const VkRect2D *rect, uint32_t x, uint32_t y)
{
	return x >= (uint32_t)rect->offset.x && x < rect->offset.x + rect->extent.width && y >= (uint32_t)rect->offset.y &&
		y < rect->offset.y + rect->extent.height;
}

static bool in_quad(const HarnessQuad *quad, uint32_t x, uint32_t y)
{
	return x >= quad->x && x < quad->x + quad->width && y >= quad->y && y < quad->y + quad->height;
}

// What the colour passes leave: the loaded area's quad, the cleared area's quad and clear, blue elsewhere.
static const uint8_t *area_texel(uint32_t x, uint32_t y)
{
	if (inside(&load_area, x, y) && in_quad(&load_quad, x, y))
		return yellow;
	if (inside(&clear_area, x, y))
		return in_quad(&clear_quad, x, y) ? red : green;
	return blue;
}

static const uint8_t *depth_texel(uint32_t x, uint32_t y)
{
	return inside(&depth_area, x, y) ? cyan : grey;
}

static const uint8_t *off_grid_texel(uint32_t x, uint32_t y)
{
	if (inside(&off_load_area, x, y))
		return yellow;
	if (inside(&off_clear_area, x, y))
		return in_quad(&off_clear_quad, x, y) ? red : green;
	return blue;
}

static const uint8_t *off_depth_texel(uint32_t x, uint32_t y)
{
	return inside(&off_depth_area, x, y) ? cyan : grey;
}

typedef const uint8_t *(*Expected)(uint32_t x, uint32_t y);

// The texels inside the rectangle, or outside it, that don't hold what they should; the first few logged.
static uint32_t count_wrong(
	const Scene *scene, uint32_t at, Expected expected, const VkRect2D *rect, bool in_rect, const char *what)
{
	uint32_t wrong = 0;
	for (uint32_t y = 0; y < HEIGHT; y++) {
		for (uint32_t x = 0; x < WIDTH; x++) {
			if (rect && inside(rect, x, y) != in_rect)
				continue;
			const uint8_t *const texel = scene->read + at + (y * WIDTH + x) * TEXEL_BYTES;
			const uint8_t *const want = expected(x, y);
			if (!memcmp(texel, want, TEXEL_BYTES))
				continue;
			if (wrong++ < MAX_MISSES_LOGGED)
				xsdk_log_write("render areas: %s: (%u, %u) is %u %u %u %u, not %u %u %u %u", what, (unsigned)x,
					(unsigned)y, texel[0], texel[1], texel[2], texel[3], want[0], want[1], want[2], want[3]);
		}
	}
	return wrong;
}

static void check_passes(const Scene *scene)
{
	const uint32_t around = count_wrong(scene, AREAS_AT, area_texel, &load_area, false, "the cleared area");
	const uint32_t loaded = count_wrong(scene, AREAS_AT, area_texel, &load_area, true, "the loaded area");
	const uint32_t depth = count_wrong(scene, DEPTH_AT, depth_texel, NULL, false, "the depth area");
	const uint32_t image = count_wrong(scene, IMAGE_CLEAR_AT, depth_texel, NULL, false, "the depth image's clear");
	xsdk_log_write("render areas: %u, %u, %u and %u texels wrong", (unsigned)around, (unsigned)loaded, (unsigned)depth,
		(unsigned)image);
	check(
		"render areas: a pass clearing an area inside the framebuffer clears and draws there alone, and stores no "
		"more",
		around == 0);
	check(
		"render areas: a pass loading an area that reaches the framebuffer's edges, off the 8-pixel grid, loads and "
		"stores it alone, the area before it in place",
		loaded == 0);
	check("render areas: a pass clearing an area's depth leaves the depth around it", depth == 0);
	check("render areas: a depth image's clear covers all of it, before a pass of a smaller area", image == 0);
	const uint32_t off_cleared = count_wrong(scene, OFF_GRID_AT, off_grid_texel, &off_load_area, false, "off the grid");
	const uint32_t off_loaded = count_wrong(scene, OFF_GRID_AT, off_grid_texel, &off_load_area, true, "loaded off it");
	const uint32_t off_depth = count_wrong(scene, OFF_DEPTH_AT, off_depth_texel, NULL, false, "depth off the grid");
	xsdk_log_write("render areas: off the grid, %u, %u and %u texels wrong", (unsigned)off_cleared,
		(unsigned)off_loaded, (unsigned)off_depth);
	check(
		"render areas: a pass clearing an area off the 8-pixel grid every way clears and draws there alone, the "
		"texels around it as they were",
		off_cleared == 0);
	check("render areas: a pass loading an area off the 8-pixel grid every way loads and stores it alone",
		off_loaded == 0);
	check("render areas: a pass clearing depth in an area off the 8-pixel grid leaves the depth around it",
		off_depth == 0);
}

// A pass of the area into the framebuffer fails its command buffer.
static bool refused_pass(const Scene *scene, PassKind kind, VkFramebuffer framebuffer, const VkRect2D *area)
{
	const bool begun = begin(scene->commands);
	begin_pass(scene, kind, framebuffer, WIDTH, area, NULL);
	vkCmdEndRenderPass(scene->commands);
	return begun && vkEndCommandBuffer(scene->commands) == VK_ERROR_FEATURE_NOT_PRESENT;
}

// An area on the grid into a half float target fails its command buffer.
static void check_refusals(const Harness *harness, const Scene *scene)
{
	VkExtent2D granularity = { 0, 0 };
	vkGetRenderAreaGranularity(harness->device, scene->passes[PASS_CLEAR], &granularity);
	check("render areas: the granularity is 8 by 8", granularity.width == 8 && granularity.height == 8);
	check("render areas: an area smaller than the framebuffer of a half float colour attachment is refused",
		refused_pass(scene, PASS_FLOAT, scene->half_framebuffer, &clear_area));
}

static void destroy(const Harness *harness, const Scene *scene)
{
	const VkDevice device = harness->device;
	if (scene->commands)
		vkFreeCommandBuffers(device, harness->pool, 1, &scene->commands);
	vkDestroyBuffer(device, scene->readback, NULL);
	vkFreeMemory(device, scene->readback_memory, NULL);
	vkDestroyBuffer(device, scene->vertices, NULL);
	vkFreeMemory(device, scene->vertex_memory, NULL);
	vkDestroyPipeline(device, scene->depth_pipeline, NULL);
	vkDestroyPipeline(device, scene->pipeline, NULL);
	vkDestroyPipelineLayout(device, scene->layout, NULL);
	vkDestroyFramebuffer(device, scene->depth_framebuffer, NULL);
	vkDestroyFramebuffer(device, scene->half_framebuffer, NULL);
	vkDestroyFramebuffer(device, scene->other_framebuffer, NULL);
	vkDestroyFramebuffer(device, scene->target_framebuffer, NULL);
	for (uint32_t i = 0; i < PASSES; i++)
		vkDestroyRenderPass(device, scene->passes[i], NULL);
	const VkImageView views[] = { scene->depth_view, scene->half_view, scene->other_view, scene->target_view };
	const VkImage images[] = { scene->depth, scene->half, scene->other, scene->target };
	const VkDeviceMemory memories[] = { scene->depth_memory, scene->half_memory, scene->other_memory,
		scene->target_memory };
	for (uint32_t i = 0; i < sizeof images / sizeof images[0]; i++) {
		vkDestroyImageView(device, views[i], NULL);
		vkDestroyImage(device, images[i], NULL);
		vkFreeMemory(device, memories[i], NULL);
	}
}

void test_render_areas(const Harness *harness)
{
	Scene scene = { .target = VK_NULL_HANDLE };
	const bool ok = build_images(harness, &scene) && build_drawing(harness, &scene);
	check("render areas: the targets, passes and buffers made", ok);
	if (ok && begin(scene.commands)) {
		record_areas(&scene);
		record_depth(&scene);
		record_off_grid(&scene);
		if (submit(harness, scene.commands))
			check_passes(&scene);
	}
	if (ok)
		check_refusals(harness, &scene);
	destroy(harness, &scene);
}
