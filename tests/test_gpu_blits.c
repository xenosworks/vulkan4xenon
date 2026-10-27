#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "harness.h"
#include "shaders.h"
#include "tests.h"
#include "xsdk/log.h"

// Milestone 27's step 2: vkCmdBlitImage drawn on the GPU. An image the GPU drew is blitted into rectangles of
// another, scaled up and down, mirrored, nearest and linear, and both are read back through vkCmdCopyImageToBuffer,
// into cached memory: each rectangle must hold what Vulkan's blit makes of the source as read back, and the texels
// around them what they held. Then the replay is timed with a blit drawn on the GPU and with one into a mip level,
// which stays on the CPU and waits for the GPU's work before it.

enum {
	FENCE_TIMEOUT_NS = 1000000000,
	COLOR = VK_IMAGE_ASPECT_COLOR_BIT,
	TEXEL_BYTES = 4,
	// The drawn source: cells of 8 by 8 texels, each a colour of its own.
	SOURCE_SIDE = 64,
	CELL = 8,
	CELLS = SOURCE_SIDE / CELL,
	// Off the 8-pixel grid, so a rectangle can reach its edges off it.
	DESTINATION_SIDE = 124,
	// The CPU's destination: level 1 of an image twice the source's size.
	LEVELS_SIDE = 2 * SOURCE_SIDE,
	// The pass that keeps the GPU busy before each timed blit: quads over a 1280x720 target.
	BUSY_WIDTH = 1280,
	BUSY_HEIGHT = 720,
	BUSY_QUADS = 32,
	// Where each read-back lands.
	SOURCE_AT = 0,
	DESTINATION_AT = SOURCE_AT + SOURCE_SIDE * SOURCE_SIDE * TEXEL_BYTES,
	LEVEL_AT = DESTINATION_AT + DESTINATION_SIDE * DESTINATION_SIDE * TEXEL_BYTES,
	READBACK_BYTES = LEVEL_AT + SOURCE_SIDE * SOURCE_SIDE * TEXEL_BYTES,
	// The memory type the CPU caches, which buffers only copied to and from may take.
	CACHED_TYPE = 1,
	// A drawn texel's channels within 2 of what was drawn; a linear one within 3 of the CPU's working, the tolerance
	// xsdk's milestone 15 bounds the filter by (xsdk:TODO.md, "Linear filtering's precision").
	NEAREST_TOLERANCE = 2,
	LINEAR_TOLERANCE = 3,
	MAX_MISSES_LOGGED = 4,
	// The replay with the blit on the CPU must take this many times as long as with it drawn, as it waits.
	WAIT_RATIO = 4,
	MICROSECONDS_PER_SECOND = 1000000,
	NANOSECONDS_PER_MICROSECOND = 1000,
};

static const uint8_t sentinel[4] = { 255, 0, 255, 255 };

typedef struct {
	const char *check;
	VkOffset3D from[2];
	VkOffset3D to[2];
	VkFilter filter;
} Region;

// Each into a rectangle of its own on the grid, but the last two: one inside the destination off the grid, drawn over
// the grid around it (milestone 67), and one reaching the destination's edges. Scaling by 2 up
// and 3 down puts no destination texel's centre on a source texel's edge, which nearest sampling could take either way;
// the odd offset of the linear one down has its pairs of texels straddle the cells.
static const Region regions[] = {
	{ "blits: a region scaled up 2 times, nearest, fills its rectangle", { { 0, 0, 0 }, { 16, 16, 1 } },
		{ { 8, 8, 0 }, { 40, 40, 1 } }, VK_FILTER_NEAREST },
	{ "blits: a region scaled down 3 times, nearest", { { 0, 0, 0 }, { 48, 48, 1 } }, { { 48, 8, 0 }, { 64, 24, 1 } },
		VK_FILTER_NEAREST },
	{ "blits: a region mirrored across by its destination and down by its source, nearest",
		{ { 8, 40, 0 }, { 40, 8, 1 } }, { { 104, 8, 0 }, { 72, 40, 1 } }, VK_FILTER_NEAREST },
	{ "blits: a region scaled up 2 times, linear", { { 0, 0, 0 }, { 24, 16, 1 } }, { { 8, 48, 0 }, { 56, 80, 1 } },
		VK_FILTER_LINEAR },
	{ "blits: a region at an odd offset scaled down 2 times, linear", { { 3, 3, 0 }, { 51, 51, 1 } },
		{ { 64, 48, 0 }, { 88, 72, 1 } }, VK_FILTER_LINEAR },
	{ "blits: a region off the 8-pixel grid inside the destination, scaled up 2 times, nearest",
		{ { 0, 0, 0 }, { 16, 16, 1 } }, { { 13, 85, 0 }, { 45, 117, 1 } }, VK_FILTER_NEAREST },
	{ "blits: a region reaching the destination's edges, off the 8-pixel grid", { { 8, 8, 0 }, { 36, 36, 1 } },
		{ { 96, 96, 0 }, { DESTINATION_SIDE, DESTINATION_SIDE, 1 } }, VK_FILTER_NEAREST },
};

enum { REGIONS = sizeof regions / sizeof regions[0] };

// Timed: the source over the destination's top left, on the GPU, and onto level 1, which only the CPU blits.
static const Region timed = { NULL, { { 0, 0, 0 }, { SOURCE_SIDE, SOURCE_SIDE, 1 } },
	{ { 0, 0, 0 }, { SOURCE_SIDE, SOURCE_SIDE, 1 } }, VK_FILTER_NEAREST };

typedef struct {
	VkRenderPass render_pass;
	VkImage source;
	VkDeviceMemory source_memory;
	VkImageView source_view;
	VkImage busy;
	VkDeviceMemory busy_memory;
	VkImageView busy_view;
	VkImage destination;
	VkDeviceMemory destination_memory;
	VkImage levels;
	VkDeviceMemory levels_memory;
	VkFramebuffer source_framebuffer;
	VkFramebuffer busy_framebuffer;
	VkPipelineLayout layout;
	VkPipeline pipeline;
	VkBuffer vertices;
	VkDeviceMemory vertex_memory;
	VkBuffer readback;
	VkDeviceMemory readback_memory;
	const uint8_t *read;
	VkCommandBuffer commands;
} Scene;

// Each cell's colour: red and green by its column and row.
static void cell_color(uint32_t column, uint32_t row, float rgba[4])
{
	rgba[0] = (16.0f + 28.0f * column) / 255.0f;
	rgba[1] = (16.0f + 28.0f * row) / 255.0f;
	rgba[2] = 128.0f / 255.0f;
	rgba[3] = 1.0f;
}

static VkResult make_render_pass(const Harness *harness, VkRenderPass *render_pass)
{
	const VkAttachmentDescription attachment = {
		.format = VK_FORMAT_R8G8B8A8_UNORM,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
		.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
		.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
		.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	};
	const VkAttachmentReference color = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
	const VkSubpassDescription subpass = {
		.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
		.colorAttachmentCount = 1,
		.pColorAttachments = &color,
	};
	const VkRenderPassCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
		.attachmentCount = 1,
		.pAttachments = &attachment,
		.subpassCount = 1,
		.pSubpasses = &subpass,
	};
	return vkCreateRenderPass(harness->device, &info, NULL, render_pass);
}

static VkResult make_framebuffer(
	const Harness *harness, const Scene *scene, VkImageView view, uint32_t width, uint32_t height, VkFramebuffer *out)
{
	const VkFramebufferCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
		.renderPass = scene->render_pass,
		.attachmentCount = 1,
		.pAttachments = &view,
		.width = width,
		.height = height,
		.layers = 1,
	};
	return vkCreateFramebuffer(harness->device, &info, NULL, out);
}

// An image only copied and blitted to and from, of its levels.
static VkResult make_transfer_image(
	const Harness *harness, uint32_t side, uint32_t levels, VkImage *image, VkDeviceMemory *memory)
{
	const VkImageCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = VK_FORMAT_R8G8B8A8_UNORM,
		.extent = { side, side, 1 },
		.mipLevels = levels,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
	};
	VkResult result = vkCreateImage(harness->device, &info, NULL, image);
	if (result != VK_SUCCESS)
		return result;
	VkMemoryRequirements requirements;
	vkGetImageMemoryRequirements(harness->device, *image, &requirements);
	const VkMemoryAllocateInfo allocate = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = requirements.size,
	};
	result = vkAllocateMemory(harness->device, &allocate, NULL, memory);
	return result == VK_SUCCESS ? vkBindImageMemory(harness->device, *image, *memory, 0) : result;
}

static bool build_images(const Harness *harness, Scene *scene)
{
	const VkImageUsageFlags drawn = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	VkResult result = make_render_pass(harness, &scene->render_pass);
	if (result == VK_SUCCESS)
		result = harness_image(harness, VK_FORMAT_R8G8B8A8_UNORM, SOURCE_SIDE, SOURCE_SIDE, drawn, &scene->source,
			&scene->source_memory, &scene->source_view);
	if (result == VK_SUCCESS)
		result = harness_image(harness, VK_FORMAT_R8G8B8A8_UNORM, BUSY_WIDTH, BUSY_HEIGHT,
			VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, &scene->busy, &scene->busy_memory, &scene->busy_view);
	if (result == VK_SUCCESS)
		result = make_transfer_image(harness, DESTINATION_SIDE, 1, &scene->destination, &scene->destination_memory);
	if (result == VK_SUCCESS)
		result = make_transfer_image(harness, LEVELS_SIDE, 2, &scene->levels, &scene->levels_memory);
	if (result == VK_SUCCESS)
		result =
			make_framebuffer(harness, scene, scene->source_view, SOURCE_SIDE, SOURCE_SIDE, &scene->source_framebuffer);
	if (result == VK_SUCCESS)
		result = make_framebuffer(harness, scene, scene->busy_view, BUSY_WIDTH, BUSY_HEIGHT, &scene->busy_framebuffer);
	return harness_ok("the images, render pass and framebuffers", result);
}

static bool build_drawing(const Harness *harness, Scene *scene)
{
	bool ok = harness_ok("push layout", harness_push_layout(harness, &scene->layout));
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
		.dynamic_viewport = true,
	};
	ok = ok && harness_ok("pipeline", harness_pipeline(harness, &spec, scene->render_pass, &scene->pipeline));
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

static uint64_t now_us(void)
{
	struct timespec now;
	(void)clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * MICROSECONDS_PER_SECOND + (uint64_t)now.tv_nsec / NANOSECONDS_PER_MICROSECOND;
}

// How long the submit, which replays the commands, took, and how long until the fence after.
typedef struct {
	uint64_t replay_us;
	uint64_t done_us;
} Timing;

static bool submit(const Harness *harness, VkCommandBuffer commands, Timing *timing)
{
	const VkSubmitInfo info = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.commandBufferCount = 1,
		.pCommandBuffers = &commands,
	};
	if (!harness_ok("vkEndCommandBuffer", vkEndCommandBuffer(commands)))
		return false;
	const uint64_t start = now_us();
	const bool submitted = harness_ok("vkQueueSubmit", vkQueueSubmit(harness->queue, 1, &info, harness->fence));
	const uint64_t replayed = now_us();
	const bool done = submitted &&
		harness_ok("vkWaitForFences", vkWaitForFences(harness->device, 1, &harness->fence, VK_TRUE, FENCE_TIMEOUT_NS));
	*timing = (Timing){ .replay_us = replayed - start, .done_us = now_us() - start };
	(void)vkResetFences(harness->device, 1, &harness->fence);
	return done;
}

static void begin_pass(const Scene *scene, VkFramebuffer framebuffer, uint32_t width, uint32_t height)
{
	const VkClearValue black = { .color = { .float32 = { 0.0f, 0.0f, 0.0f, 1.0f } } };
	const VkRenderPassBeginInfo info = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		.renderPass = scene->render_pass,
		.framebuffer = framebuffer,
		.renderArea = { { 0, 0 }, { width, height } },
		.clearValueCount = 1,
		.pClearValues = &black,
	};
	vkCmdBeginRenderPass(scene->commands, &info, VK_SUBPASS_CONTENTS_INLINE);
	const VkViewport viewport = { 0.0f, 0.0f, (float)width, (float)height, 0.0f, 1.0f };
	const VkRect2D scissor = { { 0, 0 }, { width, height } };
	vkCmdSetViewport(scene->commands, 0, 1, &viewport);
	vkCmdSetScissor(scene->commands, 0, 1, &scissor);
	vkCmdBindPipeline(scene->commands, VK_PIPELINE_BIND_POINT_GRAPHICS, scene->pipeline);
	const VkDeviceSize offset = 0;
	vkCmdBindVertexBuffers(scene->commands, 0, 1, &scene->vertices, &offset);
}

// The source, a quad a cell, after a busy pass of quads over a big target if asked.
static void record_source(const Scene *scene, bool busy)
{
	if (busy) {
		begin_pass(scene, scene->busy_framebuffer, BUSY_WIDTH, BUSY_HEIGHT);
		for (uint32_t i = 0; i < BUSY_QUADS; i++) {
			HarnessQuad quad = { 0.0f, 0.0f, BUSY_WIDTH, BUSY_HEIGHT, 0.0f, { 0.0f, 0.0f, 0.0f, 1.0f }, false };
			quad.color[0] = (float)i / BUSY_QUADS;
			harness_push_quad_in(scene->commands, scene->layout, &quad, BUSY_WIDTH, BUSY_HEIGHT);
		}
		vkCmdEndRenderPass(scene->commands);
	}
	begin_pass(scene, scene->source_framebuffer, SOURCE_SIDE, SOURCE_SIDE);
	for (uint32_t row = 0; row < CELLS; row++) {
		for (uint32_t column = 0; column < CELLS; column++) {
			HarnessQuad quad = { (float)(column * CELL), (float)(row * CELL), CELL, CELL, 0.0f, { 0 }, false };
			cell_color(column, row, quad.color);
			harness_push_quad_in(scene->commands, scene->layout, &quad, SOURCE_SIDE, SOURCE_SIDE);
		}
	}
	vkCmdEndRenderPass(scene->commands);
}

static void blit(const Scene *scene, VkImage destination, uint32_t level, const Region *region)
{
	const VkImageBlit blit = {
		.srcSubresource = { COLOR, 0, 0, 1 },
		.srcOffsets = { region->from[0], region->from[1] },
		.dstSubresource = { COLOR, level, 0, 1 },
		.dstOffsets = { region->to[0], region->to[1] },
	};
	vkCmdBlitImage(scene->commands, scene->source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, destination,
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, region->filter);
}

static void copy_out(const Scene *scene, VkImage image, uint32_t level, uint32_t side, uint32_t at)
{
	const VkBufferImageCopy region = {
		.bufferOffset = at,
		.imageSubresource = { COLOR, level, 0, 1 },
		.imageExtent = { side, side, 1 },
	};
	vkCmdCopyImageToBuffer(scene->commands, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, scene->readback, 1, &region);
}

// The destination filled with the sentinel on the CPU, the source drawn, each region blitted, and both read back.
static void record_regions(const Scene *scene)
{
	const VkClearColorValue clear = { .float32 = { 1.0f, 0.0f, 1.0f, 1.0f } };
	const VkImageSubresourceRange range = { COLOR, 0, 1, 0, 1 };
	vkCmdClearColorImage(scene->commands, scene->destination, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
	record_source(scene, false);
	for (uint32_t i = 0; i < REGIONS; i++)
		blit(scene, scene->destination, 0, &regions[i]);
	copy_out(scene, scene->source, 0, SOURCE_SIDE, SOURCE_AT);
	copy_out(scene, scene->destination, 0, DESTINATION_SIDE, DESTINATION_AT);
}

static const uint8_t *source_texel(const Scene *scene, int32_t x, int32_t y)
{
	const int32_t last = SOURCE_SIDE - 1;
	x = x < 0 ? 0 : x > last ? last : x;
	y = y < 0 ? 0 : y > last ? last : y;
	return scene->read + SOURCE_AT + ((uint32_t)y * SOURCE_SIDE + (uint32_t)x) * TEXEL_BYTES;
}

// Vulkan's blit: a destination texel's centre mapped into the source.
static float source_coordinate(int32_t from0, int32_t from1, int32_t to0, int32_t to1, uint32_t texel)
{
	return (float)from0 + ((float)texel + 0.5f - (float)to0) * (float)(from1 - from0) / (float)(to1 - to0);
}

// What the texel at (x, y) takes from the source: the nearest texel, or the four about the centre weighed, edges
// clamped.
static void blitted(const Scene *scene, const Region *region, uint32_t x, uint32_t y, float out[4])
{
	const float u = source_coordinate(region->from[0].x, region->from[1].x, region->to[0].x, region->to[1].x, x);
	const float v = source_coordinate(region->from[0].y, region->from[1].y, region->to[0].y, region->to[1].y, y);
	if (region->filter == VK_FILTER_NEAREST) {
		const uint8_t *const texel = source_texel(scene, (int32_t)floorf(u), (int32_t)floorf(v));
		for (uint32_t c = 0; c < 4; c++)
			out[c] = texel[c];
		return;
	}
	const float s = u - 0.5f;
	const float t = v - 0.5f;
	const int32_t left = (int32_t)floorf(s);
	const int32_t top = (int32_t)floorf(t);
	const float across = s - (float)left;
	const float down = t - (float)top;
	for (uint32_t c = 0; c < 4; c++) {
		const float upper =
			source_texel(scene, left, top)[c] * (1.0f - across) + source_texel(scene, left + 1, top)[c] * across;
		const float lower = source_texel(scene, left, top + 1)[c] * (1.0f - across) +
			source_texel(scene, left + 1, top + 1)[c] * across;
		out[c] = upper * (1.0f - down) + lower * down;
	}
}

static VkRect2D rect_of(const Region *region)
{
	const int32_t left = region->to[0].x < region->to[1].x ? region->to[0].x : region->to[1].x;
	const int32_t top = region->to[0].y < region->to[1].y ? region->to[0].y : region->to[1].y;
	return (VkRect2D){ { left, top },
		{ (uint32_t)abs(region->to[1].x - region->to[0].x), (uint32_t)abs(region->to[1].y - region->to[0].y) } };
}

static bool inside(const VkRect2D *rect, uint32_t x, uint32_t y)
{
	return x >= (uint32_t)rect->offset.x && x < rect->offset.x + rect->extent.width && y >= (uint32_t)rect->offset.y &&
		y < rect->offset.y + rect->extent.height;
}

static bool near(const uint8_t *texel, const float want[4], uint32_t tolerance)
{
	for (uint32_t c = 0; c < 4; c++) {
		if (fabsf((float)texel[c] - want[c]) > (float)tolerance + 0.5f)
			return false;
	}
	return true;
}

static const uint8_t *destination_texel(const Scene *scene, uint32_t x, uint32_t y)
{
	return scene->read + DESTINATION_AT + (y * DESTINATION_SIDE + x) * TEXEL_BYTES;
}

static uint32_t region_wrong(const Scene *scene, const Region *region)
{
	const VkRect2D rect = rect_of(region);
	const uint32_t tolerance = region->filter == VK_FILTER_LINEAR ? LINEAR_TOLERANCE : NEAREST_TOLERANCE;
	uint32_t wrong = 0;
	for (uint32_t y = 0; y < DESTINATION_SIDE; y++) {
		for (uint32_t x = 0; x < DESTINATION_SIDE; x++) {
			if (!inside(&rect, x, y))
				continue;
			float want[4];
			blitted(scene, region, x, y, want);
			const uint8_t *const texel = destination_texel(scene, x, y);
			if (near(texel, want, tolerance))
				continue;
			if (wrong++ < MAX_MISSES_LOGGED)
				xsdk_log_write("%s: (%u, %u) is %u %u %u %u, not %.1f %.1f %.1f %.1f", region->check, (unsigned)x,
					(unsigned)y, texel[0], texel[1], texel[2], texel[3], (double)want[0], (double)want[1],
					(double)want[2], (double)want[3]);
		}
	}
	return wrong;
}

static bool in_any_region(uint32_t x, uint32_t y)
{
	for (uint32_t i = 0; i < REGIONS; i++) {
		const VkRect2D rect = rect_of(&regions[i]);
		if (inside(&rect, x, y))
			return true;
	}
	return false;
}

// The destination's texels outside every rectangle still the sentinel, and the source's cells as drawn.
static void check_around(const Scene *scene)
{
	uint32_t wrong = 0;
	for (uint32_t y = 0; y < DESTINATION_SIDE; y++) {
		for (uint32_t x = 0; x < DESTINATION_SIDE; x++) {
			const uint8_t *const texel = destination_texel(scene, x, y);
			if (in_any_region(x, y) || !memcmp(texel, sentinel, TEXEL_BYTES))
				continue;
			if (wrong++ < MAX_MISSES_LOGGED)
				xsdk_log_write("blits: around the rectangles, (%u, %u) is %u %u %u %u", (unsigned)x, (unsigned)y,
					texel[0], texel[1], texel[2], texel[3]);
		}
	}
	check("blits: the destination around the rectangles keeps its texels", wrong == 0);
	bool drawn = true;
	for (uint32_t y = 0; y < SOURCE_SIDE; y++) {
		for (uint32_t x = 0; x < SOURCE_SIDE; x++) {
			float want[4];
			cell_color(x / CELL, y / CELL, want);
			for (uint32_t c = 0; c < 4; c++)
				want[c] *= 255.0f;
			drawn = drawn && near(source_texel(scene, (int32_t)x, (int32_t)y), want, NEAREST_TOLERANCE);
		}
	}
	check("blits: the source the GPU drew holds its cells", drawn);
}

static void check_regions(const Scene *scene)
{
	for (uint32_t i = 0; i < REGIONS; i++)
		check(regions[i].check, region_wrong(scene, &regions[i]) == 0);
	check_around(scene);
}

// The same work before a blit drawn on the GPU and one into level 1, which the CPU blits once the GPU is done.
static void check_wait(const Harness *harness, const Scene *scene)
{
	Timing drawn = { 0 };
	Timing waited = { 0 };
	bool ran = begin(scene->commands);
	record_source(scene, true);
	blit(scene, scene->destination, 0, &timed);
	ran = ran && submit(harness, scene->commands, &drawn) && begin(scene->commands);
	record_source(scene, true);
	blit(scene, scene->levels, 1, &timed);
	ran = ran && submit(harness, scene->commands, &waited) && begin(scene->commands);
	copy_out(scene, scene->levels, 1, SOURCE_SIDE, LEVEL_AT);
	Timing copied;
	ran = ran && submit(harness, scene->commands, &copied);
	xsdk_log_write(
		"blits: the replay took %llu us with the blit drawn on the GPU, %llu us with it on the CPU; their "
		"submissions were done after %llu us and %llu us",
		(unsigned long long)drawn.replay_us, (unsigned long long)waited.replay_us, (unsigned long long)drawn.done_us,
		(unsigned long long)waited.done_us);
	check("blits: a blit drawn on the GPU doesn't wait for it, a blit on the CPU does",
		ran && waited.replay_us >= WAIT_RATIO * drawn.replay_us);
	check("blits: into a level past the first, which stays on the CPU, the source's texels",
		ran && !memcmp(scene->read + LEVEL_AT, scene->read + SOURCE_AT, SOURCE_SIDE * SOURCE_SIDE * TEXEL_BYTES));
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
	vkDestroyPipeline(device, scene->pipeline, NULL);
	vkDestroyPipelineLayout(device, scene->layout, NULL);
	vkDestroyFramebuffer(device, scene->busy_framebuffer, NULL);
	vkDestroyFramebuffer(device, scene->source_framebuffer, NULL);
	vkDestroyImage(device, scene->levels, NULL);
	vkFreeMemory(device, scene->levels_memory, NULL);
	vkDestroyImage(device, scene->destination, NULL);
	vkFreeMemory(device, scene->destination_memory, NULL);
	vkDestroyImageView(device, scene->busy_view, NULL);
	vkDestroyImage(device, scene->busy, NULL);
	vkFreeMemory(device, scene->busy_memory, NULL);
	vkDestroyImageView(device, scene->source_view, NULL);
	vkDestroyImage(device, scene->source, NULL);
	vkFreeMemory(device, scene->source_memory, NULL);
	vkDestroyRenderPass(device, scene->render_pass, NULL);
}

void test_gpu_blits(const Harness *harness)
{
	Scene scene = { .render_pass = VK_NULL_HANDLE };
	const bool ok = build_images(harness, &scene) && build_drawing(harness, &scene);
	check("blits: the images, pass and buffers made", ok);
	Timing timing;
	if (ok && begin(scene.commands)) {
		record_regions(&scene);
		if (submit(harness, scene.commands, &timing))
			check_regions(&scene);
	}
	if (ok)
		check_wait(harness, &scene);
	destroy(harness, &scene);
}
