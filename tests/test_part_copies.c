#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "harness.h"
#include "shaders.h"
#include "tests.h"
#include "tiled_index.h"
#include "xsdk/log.h"

// vkCmdCopyImage of part of a level (milestone 67), as PPSSPP copies framebuffers' rectangles (God of War: Chains of
// Olympus). Colour: two rectangles at odd offsets and sizes the GPU draws over the grid around them, one on the 8-pixel
// grid, and the odd two again in a packed format the CPU copies, each into a destination filled with a pattern of its
// own, read back, every texel the source's inside the rectangles and the destination's own outside. Depth: a sampled
// D24S8 image drawn, a quad at 0.25 writing stencil 0x11 over a clear to 0.5 and 0x44, moved to be copied from by a
// barrier; then a rectangle of depth and stencil on the CPU, and depth alone on the GPU, a rectangle and the whole,
// each into an image cleared to 0.75 and 0x22, its memory read, every word's depth, and the CPU's stencil.

enum {
	WIDTH = 64,
	HEIGHT = 48,
	TEXEL_BYTES = 4,
	DEPTH_SIDE = 32,
	// The source, then destinations of depth and stencil on the CPU, and of depth alone drawn, a rectangle and whole.
	DEPTHS = 4,
	DRAWN_WIDTH = 10,
	DRAWN_STENCIL = 0x11,
	SOURCE_STENCIL = 0x44,
	DESTINATION_STENCIL = 0x22,
	DEPTH_24_MAX = 0xFFFFFF,
	// D24's rounding, as xsdk's check of the resolve allows (xsdk:tests/test_depth.c).
	DEPTH_24_TOLERANCE = 2,
	// The memory type the CPU caches, which buffers only copied to and from may take.
	CACHED_TYPE = 1,
	FENCE_TIMEOUT_NS = 1000000000,
	MAX_MISSES_LOGGED = 4,
	IMAGE_BYTES = WIDTH * HEIGHT * TEXEL_BYTES,
};

typedef enum {
	COLOR_SOURCE,
	COLOR_ODD,
	COLOR_DRAWN,
	PACKED_SOURCE,
	PACKED_CPU,
	COLORS,
} ColorImage;

// Where each colour image's texels sit in the staging and read-back buffers: a table, as GCC makes i * IMAGE_BYTES an
// rldic, which Xenia skips (CLAUDE.md), hanging the test there.
static const uint32_t image_at[COLORS] = { 0, IMAGE_BYTES, 2 * IMAGE_BYTES, 3 * IMAGE_BYTES, 4 * IMAGE_BYTES };

// Two rectangles at odd offsets and sizes; one into the grid a resolve writes.
static const VkImageCopy odd_regions[2] = {
	{ .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
		.srcOffset = { 5, 3, 0 },
		.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
		.dstOffset = { 9, 11, 0 },
		.extent = { 13, 7, 1 } },
	{ .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
		.srcOffset = { 40, 30, 0 },
		.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
		.dstOffset = { 45, 37, 0 },
		.extent = { 17, 9, 1 } },
};
static const VkImageCopy drawn_region = { .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
	.srcOffset = { 3, 5, 0 },
	.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
	.dstOffset = { 8, 16, 0 },
	.extent = { 16, 8, 1 } };

// Into each depth destination: depth and stencil, on the CPU; depth alone, drawn, a rectangle and the whole.
static const VkImageCopy depth_regions[DEPTHS] = {
	{ .extent = { 0, 0, 0 } },
	{ .srcSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 0, 1 },
		.srcOffset = { 3, 5, 0 },
		.dstSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 0, 1 },
		.dstOffset = { 10, 7, 0 },
		.extent = { 13, 9, 1 } },
	{ .srcSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 },
		.srcOffset = { 3, 20, 0 },
		.dstSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 },
		.dstOffset = { 2, 9, 0 },
		.extent = { 13, 6, 1 } },
	{ .srcSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 },
		.dstSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 },
		.extent = { DEPTH_SIDE, DEPTH_SIDE, 1 } },
};
static const char *const depth_names[DEPTHS] = { "", "depth and stencil on the CPU", "depth drawn in a rectangle",
	"depth drawn whole" };

static const float drawn_depth = 0.25f;
static const float source_depth = 0.5f;
// Static, as is the clear's range, which GCC otherwise builds with an rldic, which Xenia skips (CLAUDE.md).
static const VkClearDepthStencilValue destination_clear = { 0.75f, DESTINATION_STENCIL };

typedef struct {
	VkImage colors[COLORS];
	VkDeviceMemory color_memories[COLORS];
	VkImageView color_views[COLORS];
	VkImage pass_color;
	VkDeviceMemory pass_color_memory;
	VkImageView pass_color_view;
	VkImage depths[DEPTHS];
	VkDeviceMemory depth_memories[DEPTHS];
	VkImageView depth_views[DEPTHS];
	VkRenderPass render_pass;
	VkFramebuffer framebuffer;
	VkPipelineLayout layout;
	VkPipeline pipeline;
	VkBuffer quad;
	VkDeviceMemory quad_memory;
	VkBuffer staging;
	VkDeviceMemory staging_memory;
	VkBuffer readback;
	VkDeviceMemory readback_memory;
	const uint8_t *read;
	VkCommandBuffer commands;
} Scene;

// The source's texels and each destination's fill, every texel its own.
static void source_texel(uint32_t x, uint32_t y, uint8_t texel[4])
{
	texel[0] = (uint8_t)(x * 4), texel[1] = (uint8_t)(y * 5), texel[2] = 77, texel[3] = 255;
}

static void fill_texel(ColorImage image, uint32_t x, uint32_t y, uint8_t texel[4])
{
	texel[0] = (uint8_t)(255 - x), texel[1] = (uint8_t)(255 - y), texel[2] = (uint8_t)(100 + 20 * image),
	texel[3] = 255;
}

static uint32_t depth_24(float depth)
{
	return (uint32_t)(depth * (double)DEPTH_24_MAX + 0.5);
}

// Colour cleared; depth and stencil cleared, stored and kept an attachment, for a barrier to move.
static VkResult create_pass(const Harness *harness, VkRenderPass *render_pass)
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
			.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
			.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
			.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
			.stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE,
			.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
			.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL },
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

static VkResult build_images(const Harness *harness, Scene *scene)
{
	const VkImageUsageFlags color_usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
		VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	const VkImageUsageFlags depth_usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
		VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	// A packed format a drawn copy doesn't take, so its copies stay on the CPU.
	const VkImageUsageFlags packed_usage =
		VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	VkResult result = VK_SUCCESS;
	for (uint32_t i = 0; i < COLORS && result == VK_SUCCESS; i++) {
		const bool packed = i >= PACKED_SOURCE;
		result = harness_image(harness, packed ? VK_FORMAT_A2B10G10R10_UNORM_PACK32 : VK_FORMAT_R8G8B8A8_UNORM, WIDTH,
			HEIGHT, packed ? packed_usage : color_usage, &scene->colors[i], &scene->color_memories[i],
			&scene->color_views[i]);
	}
	for (uint32_t i = 0; i < DEPTHS && result == VK_SUCCESS; i++)
		result = harness_image(harness, VK_FORMAT_D24_UNORM_S8_UINT, DEPTH_SIDE, DEPTH_SIDE, depth_usage,
			&scene->depths[i], &scene->depth_memories[i], &scene->depth_views[i]);
	if (result == VK_SUCCESS)
		result = harness_image(harness, VK_FORMAT_B8G8R8A8_UNORM, DEPTH_SIDE, DEPTH_SIDE,
			VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, &scene->pass_color, &scene->pass_color_memory,
			&scene->pass_color_view);
	if (result == VK_SUCCESS)
		result = create_pass(harness, &scene->render_pass);
	const VkImageView views[2] = { scene->pass_color_view, scene->depth_views[0] };
	const VkFramebufferCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
		.renderPass = scene->render_pass,
		.attachmentCount = 2,
		.pAttachments = views,
		.width = DEPTH_SIDE,
		.height = DEPTH_SIDE,
		.layers = 1,
	};
	if (result == VK_SUCCESS)
		result = vkCreateFramebuffer(harness->device, &info, NULL, &scene->framebuffer);
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
		result = harness_pipeline(harness, &spec, scene->render_pass, &scene->pipeline);
	return result;
}

// A buffer of cached memory the CPU maps.
static VkResult cached_buffer(const Harness *harness, VkBufferUsageFlags usage, uint32_t size_bytes, VkBuffer *buffer,
	VkDeviceMemory *memory, void **mapped)
{
	const VkBufferCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size_bytes, .usage = usage
	};
	VkResult result = vkCreateBuffer(harness->device, &info, NULL, buffer);
	if (result != VK_SUCCESS)
		return result;
	VkMemoryRequirements requirements;
	vkGetBufferMemoryRequirements(harness->device, *buffer, &requirements);
	const VkMemoryAllocateInfo allocate = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = requirements.size,
		.memoryTypeIndex = CACHED_TYPE,
	};
	result = vkAllocateMemory(harness->device, &allocate, NULL, memory);
	if (result == VK_SUCCESS)
		result = vkBindBufferMemory(harness->device, *buffer, *memory, 0);
	if (result == VK_SUCCESS)
		result = vkMapMemory(harness->device, *memory, 0, VK_WHOLE_SIZE, 0, mapped);
	return result;
}

// The staging buffer holds the source's texels, then each destination's fill.
static VkResult build_buffers(const Harness *harness, Scene *scene)
{
	const uint32_t image_bytes = IMAGE_BYTES;
	void *staged = NULL;
	VkResult result = cached_buffer(harness, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, COLORS * image_bytes, &scene->staging,
		&scene->staging_memory, &staged);
	for (uint32_t y = 0; y < HEIGHT && result == VK_SUCCESS; y++) {
		for (uint32_t x = 0; x < WIDTH; x++) {
			uint8_t *const texels = (uint8_t *)staged + (y * WIDTH + x) * TEXEL_BYTES;
			source_texel(x, y, texels + image_at[COLOR_SOURCE]);
			source_texel(x, y, texels + image_at[PACKED_SOURCE]);
			fill_texel(COLOR_ODD, x, y, texels + image_at[COLOR_ODD]);
			fill_texel(COLOR_DRAWN, x, y, texels + image_at[COLOR_DRAWN]);
			fill_texel(PACKED_CPU, x, y, texels + image_at[PACKED_CPU]);
		}
	}
	void *read = NULL;
	if (result == VK_SUCCESS)
		result = cached_buffer(harness, VK_BUFFER_USAGE_TRANSFER_DST_BIT, COLORS * image_bytes, &scene->readback,
			&scene->readback_memory, &read);
	scene->read = read;
	if (result == VK_SUCCESS)
		result = harness_quad_buffer(harness, &scene->quad, &scene->quad_memory);
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
	return harness_ok("images", build_images(harness, scene)) &&
		harness_ok("pipeline", build_pipeline(harness, scene)) &&
		harness_ok("buffers", build_buffers(harness, scene)) &&
		harness_ok("vkAllocateCommandBuffers", vkAllocateCommandBuffers(harness->device, &commands, &scene->commands));
}

static void barrier(
	VkCommandBuffer commands, VkImage image, VkImageAspectFlags aspects, VkImageLayout from, VkImageLayout to)
{
	const VkImageMemoryBarrier info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
		.oldLayout = from,
		.newLayout = to,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = image,
		.subresourceRange = { aspects, 0, 1, 0, 1 },
	};
	vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL,
		0, NULL, 1, &info);
}

// The depth source drawn and moved to be copied from by a barrier; the destination cleared.
static void record_depth(const Scene *scene)
{
	const VkCommandBuffer commands = scene->commands;
	const VkClearValue clears[2] = { { .color = { .float32 = { 0.0f, 0.0f, 0.0f, 1.0f } } },
		{ .depthStencil = { source_depth, SOURCE_STENCIL } } };
	const VkRect2D area = { { 0, 0 }, { DEPTH_SIDE, DEPTH_SIDE } };
	const VkRenderPassBeginInfo pass = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		.renderPass = scene->render_pass,
		.framebuffer = scene->framebuffer,
		.renderArea = area,
		.clearValueCount = 2,
		.pClearValues = clears,
	};
	vkCmdBeginRenderPass(commands, &pass, VK_SUBPASS_CONTENTS_INLINE);
	const VkViewport viewport = { 0.0f, 0.0f, (float)DEPTH_SIDE, (float)DEPTH_SIDE, 0.0f, 1.0f };
	vkCmdSetViewport(commands, 0, 1, &viewport);
	vkCmdSetScissor(commands, 0, 1, &area);
	vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, scene->pipeline);
	const VkDeviceSize offset = 0;
	vkCmdBindVertexBuffers(commands, 0, 1, &scene->quad, &offset);
	const HarnessQuad quad = { 0.0f, 0.0f, DRAWN_WIDTH, DEPTH_SIDE, drawn_depth, { 1.0f, 1.0f, 1.0f, 1.0f }, false };
	harness_push_quad_in(commands, scene->layout, &quad, DEPTH_SIDE, DEPTH_SIDE);
	vkCmdEndRenderPass(commands);
	const VkImageAspectFlags both = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
	barrier(commands, scene->depths[0], both, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
		VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	static const VkImageSubresourceRange range = { VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0,
		1 };
	for (uint32_t i = 1; i < DEPTHS; i++) {
		barrier(commands, scene->depths[i], both, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		vkCmdClearDepthStencilImage(
			commands, scene->depths[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &destination_clear, 1, &range);
	}
}

// The colour images filled, the depth drawn, every copy, and the colour destinations read back; one submission.
static bool run(const Harness *harness, const Scene *scene)
{
	const VkCommandBuffer commands = scene->commands;
	const VkCommandBufferBeginInfo begin = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	if (!harness_ok("vkBeginCommandBuffer", vkBeginCommandBuffer(commands, &begin)))
		return false;
	for (uint32_t i = 0; i < COLORS; i++) {
		const VkBufferImageCopy region = { .bufferOffset = image_at[i],
			.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
			.imageExtent = { WIDTH, HEIGHT, 1 } };
		barrier(commands, scene->colors[i], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		vkCmdCopyBufferToImage(
			commands, scene->staging, scene->colors[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
	}
	record_depth(scene);
	vkCmdCopyImage(commands, scene->colors[COLOR_SOURCE], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		scene->colors[COLOR_ODD], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 2, odd_regions);
	vkCmdCopyImage(commands, scene->colors[COLOR_SOURCE], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		scene->colors[COLOR_DRAWN], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &drawn_region);
	vkCmdCopyImage(commands, scene->colors[PACKED_SOURCE], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		scene->colors[PACKED_CPU], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 2, odd_regions);
	for (uint32_t i = 1; i < DEPTHS; i++)
		vkCmdCopyImage(commands, scene->depths[0], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, scene->depths[i],
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &depth_regions[i]);
	for (uint32_t i = COLOR_ODD; i < COLORS; i++) {
		if (i == PACKED_SOURCE)
			continue;
		const VkBufferImageCopy region = { .bufferOffset = image_at[i],
			.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
			.imageExtent = { WIDTH, HEIGHT, 1 } };
		vkCmdCopyImageToBuffer(
			commands, scene->colors[i], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, scene->readback, 1, &region);
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

// The region of these that holds (x, y) of the destination, or NULL.
static const VkImageCopy *region_at(const VkImageCopy *regions, uint32_t count, uint32_t x, uint32_t y)
{
	for (uint32_t i = 0; i < count; i++) {
		const VkImageCopy *const r = &regions[i];
		if (x >= (uint32_t)r->dstOffset.x && x < (uint32_t)r->dstOffset.x + r->extent.width &&
			y >= (uint32_t)r->dstOffset.y && y < (uint32_t)r->dstOffset.y + r->extent.height)
			return r;
	}
	return NULL;
}

// A colour destination read back: the source's texel inside a region, its own fill outside.
static uint32_t colors_wrong(const Scene *scene, ColorImage image, const VkImageCopy *regions, uint32_t count)
{
	static const char *const names[COLORS] = { "", "drawn off the grid", "drawn", "", "packed, on the CPU" };
	uint32_t wrong = 0;
	for (uint32_t y = 0; y < HEIGHT; y++) {
		for (uint32_t x = 0; x < WIDTH; x++) {
			const VkImageCopy *const r = region_at(regions, count, x, y);
			uint8_t want[4];
			if (r)
				source_texel(x - (uint32_t)r->dstOffset.x + (uint32_t)r->srcOffset.x,
					y - (uint32_t)r->dstOffset.y + (uint32_t)r->srcOffset.y, want);
			else
				fill_texel(image, x, y, want);
			const uint8_t *const got = scene->read + image_at[image] + (y * WIDTH + x) * TEXEL_BYTES;
			if (!memcmp(got, want, sizeof want))
				continue;
			if (wrong++ < MAX_MISSES_LOGGED)
				xsdk_log_write("part copies: %s, (%u, %u) is %u %u %u %u, not %u %u %u %u", names[image], (unsigned)x,
					(unsigned)y, (unsigned)got[0], (unsigned)got[1], (unsigned)got[2], (unsigned)got[3],
					(unsigned)want[0], (unsigned)want[1], (unsigned)want[2], (unsigned)want[3]);
		}
	}
	xsdk_log_write("part copies: %s, %u of %u texels wrong", names[image], (unsigned)wrong, (unsigned)(WIDTH * HEIGHT));
	return wrong;
}

// The depth source's word at (x, y): the quad's on the left, the clear's on the right.
static uint32_t source_word(uint32_t x)
{
	return x < DRAWN_WIDTH ? depth_24(drawn_depth) << 8 | DRAWN_STENCIL : depth_24(source_depth) << 8 | SOURCE_STENCIL;
}

static uint32_t difference(uint32_t a, uint32_t b)
{
	return a > b ? a - b : b - a;
}

// A depth destination's words: the source's depth inside its region, the destination's clear outside; the stencil
// too, where the CPU copied both, the source's inside and the clear's outside. Xenos exports no stencil, so a drawn
// copy leaves the destination's undefined.
static uint32_t words_wrong(const Harness *harness, const Scene *scene, uint32_t i)
{
	void *mapped = NULL;
	if (!harness_ok(
			"vkMapMemory", vkMapMemory(harness->device, scene->depth_memories[i], 0, VK_WHOLE_SIZE, 0, &mapped)))
		return 1;
	volatile const uint32_t *const words = mapped;
	const bool stencil_kept = depth_regions[i].dstSubresource.aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT;
	uint32_t wrong = 0;
	for (uint32_t y = 0; y < DEPTH_SIDE; y++) {
		for (uint32_t x = 0; x < DEPTH_SIDE; x++) {
			const VkImageCopy *const r = region_at(&depth_regions[i], 1, x, y);
			const uint32_t taken = r ? source_word(x - (uint32_t)r->dstOffset.x + (uint32_t)r->srcOffset.x) : 0;
			const uint32_t depth = r ? taken >> 8 : depth_24(destination_clear.depth);
			const uint32_t stencil = r ? (taken & 0xFF) : DESTINATION_STENCIL;
			const uint32_t word = words[tiled_index(x, y, DEPTH_SIDE)];
			if (difference(word >> 8, depth) <= DEPTH_24_TOLERANCE && (!stencil_kept || (word & 0xFF) == stencil))
				continue;
			if (wrong++ < MAX_MISSES_LOGGED)
				xsdk_log_write("part copies: %s, (%u, %u) is %08X, not depth %06X above stencil %02X", depth_names[i],
					(unsigned)x, (unsigned)y, (unsigned)word, (unsigned)depth, (unsigned)stencil);
		}
	}
	vkUnmapMemory(harness->device, scene->depth_memories[i]);
	xsdk_log_write(
		"part copies: %s, %u of %u words wrong", depth_names[i], (unsigned)wrong, (unsigned)(DEPTH_SIDE * DEPTH_SIDE));
	return wrong;
}

static void destroy(const Harness *harness, const Scene *scene)
{
	const VkDevice device = harness->device;
	if (scene->commands)
		vkFreeCommandBuffers(device, harness->pool, 1, &scene->commands);
	vkDestroyBuffer(device, scene->readback, NULL);
	vkFreeMemory(device, scene->readback_memory, NULL);
	vkDestroyBuffer(device, scene->staging, NULL);
	vkFreeMemory(device, scene->staging_memory, NULL);
	vkDestroyBuffer(device, scene->quad, NULL);
	vkFreeMemory(device, scene->quad_memory, NULL);
	vkDestroyPipeline(device, scene->pipeline, NULL);
	vkDestroyPipelineLayout(device, scene->layout, NULL);
	vkDestroyFramebuffer(device, scene->framebuffer, NULL);
	vkDestroyRenderPass(device, scene->render_pass, NULL);
	vkDestroyImageView(device, scene->pass_color_view, NULL);
	vkDestroyImage(device, scene->pass_color, NULL);
	vkFreeMemory(device, scene->pass_color_memory, NULL);
	for (uint32_t i = 0; i < DEPTHS; i++) {
		vkDestroyImageView(device, scene->depth_views[i], NULL);
		vkDestroyImage(device, scene->depths[i], NULL);
		vkFreeMemory(device, scene->depth_memories[i], NULL);
	}
	for (uint32_t i = 0; i < COLORS; i++) {
		vkDestroyImageView(device, scene->color_views[i], NULL);
		vkDestroyImage(device, scene->colors[i], NULL);
		vkFreeMemory(device, scene->color_memories[i], NULL);
	}
}

void test_part_copies(const Harness *harness)
{
	static Scene scene;
	memset(&scene, 0, sizeof scene);
	const bool built = build(harness, &scene);
	check("part copies: colour and sampled depth images, a pass and buffers made", built);
	const bool ran = built && run(harness, &scene);
	check(
		"part copies: two rectangles at odd offsets and sizes drawn over the grid around them, read back, the texels "
		"around as they were",
		ran && colors_wrong(&scene, COLOR_ODD, odd_regions, 2) == 0);
	check("part copies: a rectangle on the 8-pixel grid drawn, read back, the texels around as they were",
		ran && colors_wrong(&scene, COLOR_DRAWN, &drawn_region, 1) == 0);
	check(
		"part copies: the two odd rectangles in a packed format on the CPU, read back, the texels around as they were",
		ran && colors_wrong(&scene, PACKED_CPU, odd_regions, 2) == 0);
	check("part copies: depth and stencil on the CPU, from a depth image a barrier moved to be copied from, every word",
		ran && words_wrong(harness, &scene, 1) == 0);
	check("part copies: depth alone drawn on the GPU, a rectangle at an odd offset and size, every word's depth",
		ran && words_wrong(harness, &scene, 2) == 0);
	check("part copies: depth alone drawn on the GPU, the whole image, every word's depth",
		ran && words_wrong(harness, &scene, 3) == 0);
	destroy(harness, &scene);
}
