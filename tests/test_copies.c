#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "harness.h"
#include "shaders.h"
#include "tests.h"
#include "xsdk/log.h"

// Milestone 19's step 2: the copies, clears and blits the CPU runs at replay, indirect draws and linear images. What
// each writes into an image is read back through vkCmdCopyImageToBuffer, into a buffer of cached memory, and checked
// texel by texel; the drawn target shows the GPU took what the CPU wrote.

enum {
	FENCE_TIMEOUT_NS = 1000000000,
	COLOR = VK_IMAGE_ASPECT_COLOR_BIT,
	TRANSFER_USAGE = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
	// The drawn target, and its quads: one drawn directly, one by each indirect command, and one after its vertices
	// were halved.
	TARGET_WIDTH = 128,
	TARGET_HEIGHT = 64,
	QUAD_SIDE = 24,
	QUAD_VERTICES = 6,
	// The indexed indirect command's parameters, which the host writes, after the plain one's, which a copy writes.
	INDEXED_AT = 16,
	INDIRECT_BYTES = INDEXED_AT + 20,
	// An RGBA8 chain blitted down to 1x1, its levels of 16 texels and under in the packed tail.
	CHAIN_SIDE = 64,
	CHAIN_LEVELS = 7,
	// An R8 image of 2x2 blitted to 4x4, nearest.
	SMALL_SIDE = 2,
	BIG_SIDE = 4,
	// An R8 image whose levels from 1 on are cleared, all in the packed tail.
	CLEARED_SIDE = 32,
	CLEARED_LEVELS = 6,
	CLEARED_VALUE = 128,
	// Linear images whose 160-byte rows start 256 bytes apart.
	LINEAR_WIDTH = 40,
	LINEAR_HEIGHT = 24,
	LINEAR_ROW_PITCH = 256,
	// Where each copy lands in the read-back buffer, every offset a multiple of 4.
	TARGET_AT = 0,
	CHAIN_AT = TARGET_AT + TARGET_WIDTH * TARGET_HEIGHT * 4,
	BIG_AT = CHAIN_AT + 4 * (4096 + 1024 + 256 + 64 + 16 + 4 + 1),
	CLEARED_AT = BIG_AT + BIG_SIDE * BIG_SIDE,
	COPIED_AT = CLEARED_AT + 1368,
	HOST_COPIED_AT = COPIED_AT + CHAIN_SIDE * CHAIN_SIDE * 4,
	FILL_AT = HOST_COPIED_AT + LINEAR_WIDTH * LINEAR_HEIGHT * 4,
	FILL_BYTES = 16,
	FILL_WORD = 0xA1B2C3D4,
	UPDATE_AT = FILL_AT + FILL_BYTES,
	UPDATE_BYTES = 8,
	// A copy of 11 bytes from 3 bytes in to 5 bytes in, the bytes around it left as they were.
	BYTES_AT = UPDATE_AT + UPDATE_BYTES,
	BYTES_FROM = 3,
	BYTES_TO = 5,
	BYTES_COPIED = 11,
	BYTES_AROUND = 0xEE,
	BYTES_AREA = 32,
	// The target again, after a pass that only clears it, read back with no draw between to wait for.
	CLEARED_TARGET_AT = BYTES_AT + BYTES_AREA,
	// Part of the drawn target from (2, 2), its corner the clear's and the direct quad's corner at (18, 18) in it.
	TARGET_PART_X = 2,
	TARGET_PART_Y = 2,
	TARGET_PART_SIDE = 20,
	TARGET_PART_AT = CLEARED_TARGET_AT + TARGET_WIDTH * TARGET_HEIGHT * 4,
	// An RGBA8 image of the chain's base's top left, a part of its bottom right quadrant copied over it at (5, 3).
	PART_WIDTH = 16,
	PART_HEIGHT = 8,
	PART_X = 5,
	PART_Y = 3,
	PART_REGION_WIDTH = 4,
	PART_REGION_HEIGHT = 2,
	PART_AT = TARGET_PART_AT + TARGET_PART_SIDE * TARGET_PART_SIDE * 4,
	READBACK_BYTES = PART_AT + PART_WIDTH * PART_HEIGHT * 4,
	// Where each image's texels sit in the staging buffer, for the copies in.
	CHAIN_SOURCE_AT = 0,
	CLEARED_SOURCE_AT = CHAIN_SOURCE_AT + CHAIN_SIDE * CHAIN_SIDE * 4,
	SMALL_SOURCE_AT = CLEARED_SOURCE_AT + CLEARED_SIDE * CLEARED_SIDE,
	BYTES_SOURCE_AT = SMALL_SOURCE_AT + SMALL_SIDE * SMALL_SIDE,
	STAGING_BYTES = BYTES_SOURCE_AT + 16,
	// The memory type the CPU caches, which buffers only copied to and from may take.
	CACHED_TYPE = 1,
};

enum {
	IMAGE_CHAIN,
	IMAGE_SMALL,
	IMAGE_BIG,
	IMAGE_CLEARED,
	IMAGE_COPIED,
	IMAGE_HOST_LINEAR,
	IMAGE_HOST_COPIED,
	IMAGE_LINEAR,
	IMAGE_PART,
	IMAGES,
};

typedef struct {
	VkFormat format;
	uint32_t width;
	uint32_t height;
	uint32_t levels;
	VkImageTiling tiling;
} ImageSpec;

static const ImageSpec image_specs[IMAGES] = {
	[IMAGE_CHAIN] = { VK_FORMAT_R8G8B8A8_UNORM, CHAIN_SIDE, CHAIN_SIDE, CHAIN_LEVELS, VK_IMAGE_TILING_OPTIMAL },
	[IMAGE_SMALL] = { VK_FORMAT_R8_UNORM, SMALL_SIDE, SMALL_SIDE, 1, VK_IMAGE_TILING_OPTIMAL },
	[IMAGE_BIG] = { VK_FORMAT_R8_UNORM, BIG_SIDE, BIG_SIDE, 1, VK_IMAGE_TILING_OPTIMAL },
	[IMAGE_CLEARED] = { VK_FORMAT_R8_UNORM, CLEARED_SIDE, CLEARED_SIDE, CLEARED_LEVELS, VK_IMAGE_TILING_OPTIMAL },
	[IMAGE_COPIED] = { VK_FORMAT_R8G8B8A8_UNORM, CHAIN_SIDE, CHAIN_SIDE, 1, VK_IMAGE_TILING_OPTIMAL },
	[IMAGE_HOST_LINEAR] = { VK_FORMAT_R8G8B8A8_UNORM, LINEAR_WIDTH, LINEAR_HEIGHT, 1, VK_IMAGE_TILING_LINEAR },
	[IMAGE_HOST_COPIED] = { VK_FORMAT_R8G8B8A8_UNORM, LINEAR_WIDTH, LINEAR_HEIGHT, 1, VK_IMAGE_TILING_OPTIMAL },
	[IMAGE_LINEAR] = { VK_FORMAT_R8G8B8A8_UNORM, LINEAR_WIDTH, LINEAR_HEIGHT, 1, VK_IMAGE_TILING_LINEAR },
	[IMAGE_PART] = { VK_FORMAT_R8G8B8A8_UNORM, PART_WIDTH, PART_HEIGHT, 1, VK_IMAGE_TILING_OPTIMAL },
};

enum {
	QUAD_DIRECT,
	QUAD_INDIRECT,
	QUAD_INDEXED,
	QUAD_HALVED,
	QUADS,
};

static const HarnessQuad quads[QUADS] = {
	[QUAD_DIRECT] = { 8.0f, 8.0f, QUAD_SIDE, QUAD_SIDE, 0.0f, { 1.0f, 0.0f, 0.0f, 1.0f }, false },
	[QUAD_INDIRECT] = { 40.0f, 8.0f, QUAD_SIDE, QUAD_SIDE, 0.0f, { 0.0f, 1.0f, 0.0f, 1.0f }, false },
	[QUAD_INDEXED] = { 72.0f, 8.0f, QUAD_SIDE, QUAD_SIDE, 0.0f, { 1.0f, 1.0f, 0.0f, 1.0f }, false },
	[QUAD_HALVED] = { 100.0f, 8.0f, QUAD_SIDE, QUAD_SIDE, 0.0f, { 1.0f, 1.0f, 1.0f, 1.0f }, false },
};

// The unit quad push.vert scales, as harness_quad_buffer has it, and the same at half its size.
static const float unit_quad[QUAD_VERTICES][4] = {
	{ 0.0f, 0.0f, 0.0f, 1.0f },
	{ 1.0f, 0.0f, 0.0f, 1.0f },
	{ 0.0f, 1.0f, 0.0f, 1.0f },
	{ 1.0f, 0.0f, 0.0f, 1.0f },
	{ 1.0f, 1.0f, 0.0f, 1.0f },
	{ 0.0f, 1.0f, 0.0f, 1.0f },
};

static const float half_quad[QUAD_VERTICES][4] = {
	{ 0.0f, 0.0f, 0.0f, 1.0f },
	{ 0.5f, 0.0f, 0.0f, 1.0f },
	{ 0.0f, 0.5f, 0.0f, 1.0f },
	{ 0.5f, 0.0f, 0.0f, 1.0f },
	{ 0.5f, 0.5f, 0.0f, 1.0f },
	{ 0.0f, 0.5f, 0.0f, 1.0f },
};

static const uint8_t red[4] = { 255, 0, 0, 255 };
static const uint8_t green[4] = { 0, 255, 0, 255 };
static const uint8_t yellow[4] = { 255, 255, 0, 255 };
static const uint8_t white[4] = { 255, 255, 255, 255 };
static const uint8_t blue[4] = { 0, 0, 255, 255 };
static const uint8_t cyan[4] = { 0, 255, 255, 255 };

// The chain's base: four quadrants, each a colour of its own.
static const uint8_t quadrants[4][4] = {
	{ 200, 40, 10, 255 },
	{ 10, 220, 30, 255 },
	{ 30, 60, 240, 255 },
	{ 250, 250, 50, 128 },
};

static const uint8_t small_texels[SMALL_SIDE * SMALL_SIDE] = { 10, 20, 30, 40 };

static const char *const step_2_commands[] = {
	"vkCmdCopyImageToBuffer",
	"vkCmdCopyImage",
	"vkCmdBlitImage",
	"vkCmdClearColorImage",
	"vkCmdCopyBuffer",
	"vkCmdUpdateBuffer",
	"vkCmdFillBuffer",
	"vkCmdDrawIndirect",
	"vkCmdDrawIndexedIndirect",
	"vkGetImageSubresourceLayout",
};

typedef struct {
	VkRenderPass render_pass;
	VkRenderPass clear_pass;
	VkImage target;
	VkDeviceMemory target_memory;
	VkImageView target_view;
	VkFramebuffer framebuffer;
	VkPipelineLayout layout;
	VkPipeline pipeline;
	VkBuffer vertices;
	VkDeviceMemory vertex_memory;
	VkBuffer indices;
	VkDeviceMemory index_memory;
	VkBuffer indirect;
	VkDeviceMemory indirect_memory;
	VkBuffer staging;
	VkDeviceMemory staging_memory;
	VkBuffer readback;
	VkDeviceMemory readback_memory;
	uint8_t *staged;
	const uint8_t *read;
	VkImage images[IMAGES];
	VkDeviceMemory image_memory[IMAGES];
	VkCommandBuffer commands;
} Scene;

static uint32_t side(uint32_t size, uint32_t level)
{
	return size >> level ? size >> level : 1;
}

static const uint8_t *chain_texel(uint32_t level, uint32_t x, uint32_t y)
{
	const uint32_t half = side(CHAIN_SIDE, level) / 2;
	return quadrants[(y >= half) * 2 + (x >= half)];
}

static uint8_t cleared_base(uint32_t x, uint32_t y)
{
	return (uint8_t)(x * 7 + y * 3);
}

static void host_texel(uint32_t x, uint32_t y, uint8_t texel[4])
{
	texel[0] = (uint8_t)(x * 6);
	texel[1] = (uint8_t)(y * 10);
	texel[2] = (uint8_t)((x + y) * 3);
	texel[3] = 255;
}

// Bytes from the read-back buffer's level at at, of a width.
static const uint8_t *read_texel(
	const Scene *scene, uint32_t at, uint32_t width, uint32_t bytes, uint32_t x, uint32_t y)
{
	return scene->read + at + (y * width + x) * bytes;
}

// The chain's level in the read-back buffer, the levels in order.
static uint32_t chain_at(uint32_t level)
{
	uint32_t at = CHAIN_AT;
	for (uint32_t l = 0; l < level; l++)
		at += side(CHAIN_SIDE, l) * side(CHAIN_SIDE, l) * 4;
	return at;
}

static uint32_t cleared_at(uint32_t level)
{
	uint32_t at = CLEARED_AT;
	for (uint32_t l = 0; l < level; l++)
		at += side(CLEARED_SIDE, l) * side(CLEARED_SIDE, l);
	return at;
}

// A buffer only copied to and from, in cached memory, mapped.
static bool cached_buffer(const Harness *harness, VkBufferUsageFlags usage, uint32_t size_bytes, VkBuffer *buffer,
	VkDeviceMemory *memory, void **mapped)
{
	const VkBufferCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size_bytes, .usage = usage
	};
	if (!harness_ok("vkCreateBuffer", vkCreateBuffer(harness->device, &info, NULL, buffer)))
		return false;
	VkMemoryRequirements requirements;
	vkGetBufferMemoryRequirements(harness->device, *buffer, &requirements);
	const VkMemoryAllocateInfo allocate = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = requirements.size,
		.memoryTypeIndex = CACHED_TYPE,
	};
	return harness_ok("vkAllocateMemory", vkAllocateMemory(harness->device, &allocate, NULL, memory)) &&
		harness_ok("vkBindBufferMemory", vkBindBufferMemory(harness->device, *buffer, *memory, 0)) &&
		harness_ok("vkMapMemory", vkMapMemory(harness->device, *memory, 0, VK_WHOLE_SIZE, 0, mapped));
}

static bool make_image(const Harness *harness, const ImageSpec *spec, VkImage *image, VkDeviceMemory *memory)
{
	const VkImageCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = spec->format,
		.extent = { spec->width, spec->height, 1 },
		.mipLevels = spec->levels,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = spec->tiling,
		.usage = TRANSFER_USAGE,
	};
	if (!harness_ok("vkCreateImage", vkCreateImage(harness->device, &info, NULL, image)))
		return false;
	VkMemoryRequirements requirements;
	vkGetImageMemoryRequirements(harness->device, *image, &requirements);
	const VkMemoryAllocateInfo allocate = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = requirements.size,
	};
	return harness_ok("vkAllocateMemory", vkAllocateMemory(harness->device, &allocate, NULL, memory)) &&
		harness_ok("vkBindImageMemory", vkBindImageMemory(harness->device, *image, *memory, 0));
}

// One pass that loads or clears the target and leaves it to be copied from.
static VkResult make_render_pass(const Harness *harness, VkAttachmentLoadOp load, VkRenderPass *render_pass)
{
	const VkAttachmentDescription attachment = {
		.format = VK_FORMAT_R8G8B8A8_UNORM,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.loadOp = load,
		.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
		.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
		.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
		.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
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

static bool build_target(const Harness *harness, Scene *scene)
{
	const VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | TRANSFER_USAGE;
	bool ok = harness_ok("render pass", make_render_pass(harness, VK_ATTACHMENT_LOAD_OP_LOAD, &scene->render_pass)) &&
		harness_ok("clear pass", make_render_pass(harness, VK_ATTACHMENT_LOAD_OP_CLEAR, &scene->clear_pass)) &&
		harness_ok("target",
			harness_image(harness, VK_FORMAT_R8G8B8A8_UNORM, TARGET_WIDTH, TARGET_HEIGHT, usage, &scene->target,
				&scene->target_memory, &scene->target_view));
	const VkFramebufferCreateInfo framebuffer = {
		.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
		.renderPass = scene->render_pass,
		.attachmentCount = 1,
		.pAttachments = &scene->target_view,
		.width = TARGET_WIDTH,
		.height = TARGET_HEIGHT,
		.layers = 1,
	};
	ok = ok && harness_ok("framebuffer", vkCreateFramebuffer(harness->device, &framebuffer, NULL, &scene->framebuffer));
	ok = ok && harness_ok("push layout", harness_push_layout(harness, &scene->layout));
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
	return ok && harness_ok("pipeline", harness_pipeline(harness, &spec, scene->render_pass, &scene->pipeline));
}

static bool build_buffers(const Harness *harness, Scene *scene)
{
	static const uint16_t indices[QUAD_VERTICES] = { 0, 1, 2, 3, 4, 5 };
	// The plain command draws nothing until a copy in the submission writes its count.
	uint32_t indirect[INDIRECT_BYTES / 4] = { 0 };
	const uint32_t indexed[5] = { QUAD_VERTICES, 1, 0, 0, 0 };
	memcpy((uint8_t *)indirect + INDEXED_AT, indexed, sizeof indexed);
	void *staged = NULL;
	void *read = NULL;
	const bool ok = harness_ok("vertices",
						harness_buffer(harness, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
							unit_quad, sizeof unit_quad, &scene->vertices, &scene->vertex_memory)) &&
		harness_ok("indices",
			harness_buffer(harness, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, indices, sizeof indices, &scene->indices,
				&scene->index_memory)) &&
		harness_ok("indirect",
			harness_buffer(harness, VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, indirect,
				sizeof indirect, &scene->indirect, &scene->indirect_memory)) &&
		cached_buffer(harness, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, STAGING_BYTES, &scene->staging, &scene->staging_memory,
			&staged) &&
		cached_buffer(harness, VK_BUFFER_USAGE_TRANSFER_DST_BIT, READBACK_BYTES, &scene->readback,
			&scene->readback_memory, &read);
	scene->staged = staged;
	scene->read = read;
	return ok;
}

// The level 0s copied in, the bytes copied between buffers, and the host's linear image, its rows at its pitch.
static bool stage(const Harness *harness, const Scene *scene, VkSubresourceLayout *host_layout)
{
	for (uint32_t y = 0; y < CHAIN_SIDE; y++)
		for (uint32_t x = 0; x < CHAIN_SIDE; x++)
			memcpy(scene->staged + CHAIN_SOURCE_AT + (y * CHAIN_SIDE + x) * 4, chain_texel(0, x, y), 4);
	for (uint32_t y = 0; y < CLEARED_SIDE; y++)
		for (uint32_t x = 0; x < CLEARED_SIDE; x++)
			scene->staged[CLEARED_SOURCE_AT + y * CLEARED_SIDE + x] = cleared_base(x, y);
	memcpy(scene->staged + SMALL_SOURCE_AT, small_texels, sizeof small_texels);
	for (uint32_t i = 0; i < 16; i++)
		scene->staged[BYTES_SOURCE_AT + i] = (uint8_t)(0x40 + i);
	memset((uint8_t *)(uintptr_t)scene->read + BYTES_AT, BYTES_AROUND, BYTES_AREA);
	const VkImageSubresource subresource = { COLOR, 0, 0 };
	vkGetImageSubresourceLayout(harness->device, scene->images[IMAGE_HOST_LINEAR], &subresource, host_layout);
	void *mapped = NULL;
	if (!harness_ok("vkMapMemory",
			vkMapMemory(harness->device, scene->image_memory[IMAGE_HOST_LINEAR], 0, VK_WHOLE_SIZE, 0, &mapped)))
		return false;
	// Write-combined memory: a row at a time through memcpy, never a plain 8-byte store.
	uint8_t row[LINEAR_WIDTH * 4];
	for (uint32_t y = 0; y < LINEAR_HEIGHT; y++) {
		for (uint32_t x = 0; x < LINEAR_WIDTH; x++)
			host_texel(x, y, &row[x * 4]);
		memcpy((uint8_t *)mapped + host_layout->offset + y * host_layout->rowPitch, row, sizeof row);
	}
	vkUnmapMemory(harness->device, scene->image_memory[IMAGE_HOST_LINEAR]);
	return true;
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
	// The tests after submit with the fence and wait on it, so it's left unsignaled, as they leave it.
	(void)vkResetFences(harness->device, 1, &harness->fence);
	return done;
}

static void begin_pass(const Scene *scene)
{
	const VkRenderPassBeginInfo info = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		.renderPass = scene->render_pass,
		.framebuffer = scene->framebuffer,
		.renderArea = { { 0, 0 }, { TARGET_WIDTH, TARGET_HEIGHT } },
	};
	vkCmdBeginRenderPass(scene->commands, &info, VK_SUBPASS_CONTENTS_INLINE);
	const VkViewport viewport = { 0.0f, 0.0f, TARGET_WIDTH, TARGET_HEIGHT, 0.0f, 1.0f };
	const VkRect2D scissor = { { 0, 0 }, { TARGET_WIDTH, TARGET_HEIGHT } };
	vkCmdSetViewport(scene->commands, 0, 1, &viewport);
	vkCmdSetScissor(scene->commands, 0, 1, &scissor);
	vkCmdBindPipeline(scene->commands, VK_PIPELINE_BIND_POINT_GRAPHICS, scene->pipeline);
	const VkDeviceSize offset = 0;
	vkCmdBindVertexBuffers(scene->commands, 0, 1, &scene->vertices, &offset);
}

static VkBufferImageCopy level_region(uint32_t at, uint32_t level, uint32_t width, uint32_t height)
{
	return (VkBufferImageCopy){
		.bufferOffset = at,
		.imageSubresource = { COLOR, level, 0, 1 },
		.imageExtent = { side(width, level), side(height, level), 1 },
	};
}

// The target cleared on the CPU, then two passes that load it: the first draws a quad directly and one through each
// indirect command, the plain one's count from a copy before it; the second, after the vertices are halved, draws one
// more. Then the target is copied out, cleared by a pass with no draws and copied out again.
static void record_draws(const Scene *scene)
{
	VkCommandBuffer commands = scene->commands;
	const uint32_t parameters[4] = { QUAD_VERTICES, 1, 0, 0 };
	vkCmdUpdateBuffer(commands, scene->indirect, 0, sizeof parameters, parameters);
	const VkClearColorValue clear = { .float32 = { 0.0f, 0.0f, 1.0f, 1.0f } };
	const VkImageSubresourceRange range = { COLOR, 0, 1, 0, 1 };
	vkCmdClearColorImage(commands, scene->target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
	begin_pass(scene);
	harness_push_quad_in(commands, scene->layout, &quads[QUAD_DIRECT], TARGET_WIDTH, TARGET_HEIGHT);
	harness_push_place_in(commands, scene->layout, &quads[QUAD_INDIRECT], TARGET_WIDTH, TARGET_HEIGHT);
	vkCmdDrawIndirect(commands, scene->indirect, 0, 1, sizeof parameters);
	vkCmdBindIndexBuffer(commands, scene->indices, 0, VK_INDEX_TYPE_UINT16);
	harness_push_place_in(commands, scene->layout, &quads[QUAD_INDEXED], TARGET_WIDTH, TARGET_HEIGHT);
	vkCmdDrawIndexedIndirect(commands, scene->indirect, INDEXED_AT, 1, INDIRECT_BYTES - INDEXED_AT);
	vkCmdEndRenderPass(commands);
	vkCmdUpdateBuffer(commands, scene->vertices, 0, sizeof half_quad, half_quad);
	begin_pass(scene);
	harness_push_quad_in(commands, scene->layout, &quads[QUAD_HALVED], TARGET_WIDTH, TARGET_HEIGHT);
	vkCmdEndRenderPass(commands);
	const VkBufferImageCopy region = level_region(TARGET_AT, 0, TARGET_WIDTH, TARGET_HEIGHT);
	vkCmdCopyImageToBuffer(commands, scene->target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, scene->readback, 1, &region);
	const VkBufferImageCopy part = {
		.bufferOffset = TARGET_PART_AT,
		.imageSubresource = { COLOR, 0, 0, 1 },
		.imageOffset = { TARGET_PART_X, TARGET_PART_Y, 0 },
		.imageExtent = { TARGET_PART_SIDE, TARGET_PART_SIDE, 1 },
	};
	vkCmdCopyImageToBuffer(commands, scene->target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, scene->readback, 1, &part);
	// A pass that only clears: its resolves are the GPU's one work before the copy after, so no draw is waited for.
	const VkClearValue cyan_clear = { .color = { .float32 = { 0.0f, 1.0f, 1.0f, 1.0f } } };
	const VkRenderPassBeginInfo clear_pass = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		.renderPass = scene->clear_pass,
		.framebuffer = scene->framebuffer,
		.renderArea = { { 0, 0 }, { TARGET_WIDTH, TARGET_HEIGHT } },
		.clearValueCount = 1,
		.pClearValues = &cyan_clear,
	};
	vkCmdBeginRenderPass(commands, &clear_pass, VK_SUBPASS_CONTENTS_INLINE);
	vkCmdEndRenderPass(commands);
	const VkBufferImageCopy again = level_region(CLEARED_TARGET_AT, 0, TARGET_WIDTH, TARGET_HEIGHT);
	vkCmdCopyImageToBuffer(commands, scene->target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, scene->readback, 1, &again);
}

static bool cleared_target_holds(const Scene *scene)
{
	for (uint32_t y = 0; y < TARGET_HEIGHT; y++) {
		for (uint32_t x = 0; x < TARGET_WIDTH; x++) {
			const uint8_t *const texel = read_texel(scene, CLEARED_TARGET_AT, TARGET_WIDTH, 4, x, y);
			if (memcmp(texel, cyan, 4)) {
				xsdk_log_write("copies: the cleared target's (%u, %u) is %u %u %u %u", (unsigned)x, (unsigned)y,
					texel[0], texel[1], texel[2], texel[3]);
				return false;
			}
		}
	}
	return true;
}

static bool target_holds(const Scene *scene, uint32_t x, uint32_t y, const uint8_t want[4])
{
	const uint8_t *const texel = read_texel(scene, TARGET_AT, TARGET_WIDTH, 4, x, y);
	if (!memcmp(texel, want, 4))
		return true;
	xsdk_log_write("copies: the target's (%u, %u) is %u %u %u %u, not %u %u %u %u", (unsigned)x, (unsigned)y, texel[0],
		texel[1], texel[2], texel[3], want[0], want[1], want[2], want[3]);
	return false;
}

// Part of the target read back as PPSSPP reads part of a framebuffer: each texel the whole target's at its offset.
static bool target_part_holds(const Scene *scene)
{
	for (uint32_t y = 0; y < TARGET_PART_SIDE; y++) {
		for (uint32_t x = 0; x < TARGET_PART_SIDE; x++) {
			const uint8_t *const texel = read_texel(scene, TARGET_PART_AT, TARGET_PART_SIDE, 4, x, y);
			if (memcmp(texel, read_texel(scene, TARGET_AT, TARGET_WIDTH, 4, x + TARGET_PART_X, y + TARGET_PART_Y), 4))
				return false;
		}
	}
	const uint8_t *const corner = read_texel(scene, TARGET_PART_AT, TARGET_PART_SIDE, 4, 0, 0);
	const uint8_t *const quad = read_texel(scene, TARGET_PART_AT, TARGET_PART_SIDE, 4, 18, 18);
	return !memcmp(corner, blue, 4) && !memcmp(quad, red, 4);
}

static void check_draws(const Scene *scene)
{
	check("copies: part of an image the GPU drew reads back from an offset, its clear and its quad in place",
		target_part_holds(scene));
	check("copies: an image the GPU drew reads back through vkCmdCopyImageToBuffer, its quad in place",
		target_holds(scene, 20, 20, red));
	check("copies: a colour cleared on the CPU is what the next pass loads, around its quads",
		target_holds(scene, 2, 2, blue) && target_holds(scene, 64, 50, blue) &&
			target_holds(scene, TARGET_WIDTH - 2, TARGET_HEIGHT - 2, blue));
	check("copies: vkCmdDrawIndirect draws with the count a vkCmdUpdateBuffer before it wrote",
		target_holds(scene, 52, 20, green));
	check("copies: vkCmdDrawIndexedIndirect draws with parameters the host wrote", target_holds(scene, 84, 20, yellow));
	check("copies: vertices updated between two passes, the first pass's draw has the old and the second's the new",
		target_holds(scene, 30, 30, red) && target_holds(scene, 102, 10, white) && target_holds(scene, 120, 28, blue));
	check("copies: a pass that only clears, its image read back straight after, holds the clear",
		cleared_target_holds(scene));
}

static void blit(VkCommandBuffer commands, VkImage source, uint32_t source_level, uint32_t source_side,
	VkImage destination, uint32_t destination_level, uint32_t destination_side, VkFilter filter)
{
	const VkImageBlit region = {
		.srcSubresource = { COLOR, source_level, 0, 1 },
		.srcOffsets = { { 0, 0, 0 }, { (int32_t)source_side, (int32_t)source_side, 1 } },
		.dstSubresource = { COLOR, destination_level, 0, 1 },
		.dstOffsets = { { 0, 0, 0 }, { (int32_t)destination_side, (int32_t)destination_side, 1 } },
	};
	vkCmdBlitImage(commands, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, destination,
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, filter);
}

static void copy_image(VkCommandBuffer commands, VkImage source, VkImage destination, uint32_t width, uint32_t height)
{
	const VkImageCopy region = {
		.srcSubresource = { COLOR, 0, 0, 1 },
		.dstSubresource = { COLOR, 0, 0, 1 },
		.extent = { width, height, 1 },
	};
	vkCmdCopyImage(commands, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, destination,
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
}

static void copy_in(const Scene *scene, uint32_t image, uint32_t at)
{
	const ImageSpec *const spec = &image_specs[image];
	const VkBufferImageCopy region = level_region(at, 0, spec->width, spec->height);
	vkCmdCopyBufferToImage(
		scene->commands, scene->staging, scene->images[image], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
}

static void copy_out(const Scene *scene, uint32_t image, uint32_t level, uint32_t at)
{
	const ImageSpec *const spec = &image_specs[image];
	const VkBufferImageCopy region = level_region(at, level, spec->width, spec->height);
	vkCmdCopyImageToBuffer(
		scene->commands, scene->images[image], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, scene->readback, 1, &region);
}

// The chain blitted down, the small image blitted up, the cleared image's levels from 1 on cleared, the chain's base
// copied and the host's linear image copied to an optimal one and on to another linear one; then every level out,
// and the buffers filled, updated and copied.
static void record_images(const Scene *scene)
{
	VkCommandBuffer commands = scene->commands;
	const VkImage *const images = scene->images;
	copy_in(scene, IMAGE_CHAIN, CHAIN_SOURCE_AT);
	copy_in(scene, IMAGE_CLEARED, CLEARED_SOURCE_AT);
	copy_in(scene, IMAGE_SMALL, SMALL_SOURCE_AT);
	for (uint32_t level = 0; level + 1 < CHAIN_LEVELS; level++)
		blit(commands, images[IMAGE_CHAIN], level, side(CHAIN_SIDE, level), images[IMAGE_CHAIN], level + 1,
			side(CHAIN_SIDE, level + 1), VK_FILTER_LINEAR);
	blit(commands, images[IMAGE_SMALL], 0, SMALL_SIDE, images[IMAGE_BIG], 0, BIG_SIDE, VK_FILTER_NEAREST);
	const VkClearColorValue clear = { .float32 = { CLEARED_VALUE / 255.0f, 0.0f, 0.0f, 1.0f } };
	const VkImageSubresourceRange range = { COLOR, 1, VK_REMAINING_MIP_LEVELS, 0, 1 };
	vkCmdClearColorImage(commands, images[IMAGE_CLEARED], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
	copy_image(commands, images[IMAGE_CHAIN], images[IMAGE_COPIED], CHAIN_SIDE, CHAIN_SIDE);
	copy_image(commands, images[IMAGE_HOST_LINEAR], images[IMAGE_HOST_COPIED], LINEAR_WIDTH, LINEAR_HEIGHT);
	copy_image(commands, images[IMAGE_HOST_COPIED], images[IMAGE_LINEAR], LINEAR_WIDTH, LINEAR_HEIGHT);
	for (uint32_t level = 0; level < CHAIN_LEVELS; level++)
		copy_out(scene, IMAGE_CHAIN, level, chain_at(level));
	copy_out(scene, IMAGE_BIG, 0, BIG_AT);
	for (uint32_t level = 0; level < CLEARED_LEVELS; level++)
		copy_out(scene, IMAGE_CLEARED, level, cleared_at(level));
	copy_out(scene, IMAGE_COPIED, 0, COPIED_AT);
	copy_out(scene, IMAGE_HOST_COPIED, 0, HOST_COPIED_AT);
	// The chain's base's top left, its rows 64 texels long, then part of its bottom right over it.
	VkBufferImageCopy part = level_region(CHAIN_SOURCE_AT, 0, PART_WIDTH, PART_HEIGHT);
	part.bufferRowLength = CHAIN_SIDE;
	vkCmdCopyBufferToImage(
		commands, scene->staging, images[IMAGE_PART], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &part);
	part.bufferOffset = CHAIN_SOURCE_AT + (CHAIN_SIDE / 2 * CHAIN_SIDE + CHAIN_SIDE / 2) * 4;
	part.imageOffset = (VkOffset3D){ PART_X, PART_Y, 0 };
	part.imageExtent = (VkExtent3D){ PART_REGION_WIDTH, PART_REGION_HEIGHT, 1 };
	vkCmdCopyBufferToImage(
		commands, scene->staging, images[IMAGE_PART], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &part);
	copy_out(scene, IMAGE_PART, 0, PART_AT);
	vkCmdFillBuffer(commands, scene->readback, FILL_AT, FILL_BYTES, FILL_WORD);
	const uint8_t update[UPDATE_BYTES] = { 1, 2, 3, 4, 5, 6, 7, 8 };
	vkCmdUpdateBuffer(commands, scene->readback, UPDATE_AT, sizeof update, update);
	const VkBufferCopy bytes = { BYTES_SOURCE_AT + BYTES_FROM, BYTES_AT + BYTES_TO, BYTES_COPIED };
	vkCmdCopyBuffer(commands, scene->staging, scene->readback, 1, &bytes);
}

static bool same_texel(const uint8_t *got, const uint8_t *want, uint32_t bytes, uint32_t tolerance)
{
	for (uint32_t i = 0; i < bytes; i++) {
		const int32_t difference = (int32_t)got[i] - (int32_t)want[i];
		if (difference > (int32_t)tolerance || difference < -(int32_t)tolerance)
			return false;
	}
	return true;
}

// Levels 1 to 5 keep their quadrants, as each texel averages four of one; the 1x1 is the four's mean.
static bool chain_holds(const Scene *scene)
{
	bool holds = true;
	for (uint32_t level = 0; level + 1 < CHAIN_LEVELS; level++) {
		const uint32_t size = side(CHAIN_SIDE, level);
		for (uint32_t y = 0; y < size; y++)
			for (uint32_t x = 0; x < size; x++)
				holds = holds &&
					same_texel(read_texel(scene, chain_at(level), size, 4, x, y), chain_texel(level, x, y), 4, 0);
	}
	uint8_t mean[4];
	for (uint32_t c = 0; c < 4; c++)
		mean[c] = (uint8_t)((quadrants[0][c] + quadrants[1][c] + quadrants[2][c] + quadrants[3][c] + 2) / 4);
	const uint8_t *const last = read_texel(scene, chain_at(CHAIN_LEVELS - 1), 1, 4, 0, 0);
	if (!same_texel(last, mean, 4, 1))
		xsdk_log_write("copies: the chain's 1x1 is %u %u %u %u, not %u %u %u %u", last[0], last[1], last[2], last[3],
			mean[0], mean[1], mean[2], mean[3]);
	return holds && same_texel(last, mean, 4, 1);
}

static bool big_holds(const Scene *scene)
{
	bool holds = true;
	for (uint32_t y = 0; y < BIG_SIDE; y++)
		for (uint32_t x = 0; x < BIG_SIDE; x++)
			holds = holds && scene->read[BIG_AT + y * BIG_SIDE + x] == small_texels[y / 2 * SMALL_SIDE + x / 2];
	return holds;
}

static bool cleared_holds(const Scene *scene)
{
	bool holds = true;
	for (uint32_t level = 0; level < CLEARED_LEVELS; level++) {
		const uint32_t size = side(CLEARED_SIDE, level);
		for (uint32_t y = 0; y < size; y++)
			for (uint32_t x = 0; x < size; x++)
				holds = holds &&
					scene->read[cleared_at(level) + y * size + x] == (level ? CLEARED_VALUE : cleared_base(x, y));
	}
	return holds;
}

// The bottom right quadrant's colour in the part copied, the top left's around it.
static bool part_holds(const Scene *scene)
{
	bool holds = true;
	for (uint32_t y = 0; y < PART_HEIGHT; y++) {
		for (uint32_t x = 0; x < PART_WIDTH; x++) {
			const bool in =
				x >= PART_X && x < PART_X + PART_REGION_WIDTH && y >= PART_Y && y < PART_Y + PART_REGION_HEIGHT;
			holds = holds && same_texel(read_texel(scene, PART_AT, PART_WIDTH, 4, x, y), quadrants[in ? 3 : 0], 4, 0);
		}
	}
	return holds;
}

static bool copied_holds(const Scene *scene)
{
	bool holds = true;
	for (uint32_t y = 0; y < CHAIN_SIDE; y++)
		for (uint32_t x = 0; x < CHAIN_SIDE; x++)
			holds = holds && same_texel(read_texel(scene, COPIED_AT, CHAIN_SIDE, 4, x, y), chain_texel(0, x, y), 4, 0);
	return holds;
}

// Through the read-back buffer for the optimal image, and the linear one's own memory at its pitch.
static bool linear_holds(const Harness *harness, const Scene *scene, const VkSubresourceLayout *host_layout)
{
	VkSubresourceLayout layout;
	const VkImageSubresource subresource = { COLOR, 0, 0 };
	vkGetImageSubresourceLayout(harness->device, scene->images[IMAGE_LINEAR], &subresource, &layout);
	void *mapped = NULL;
	if (!harness_ok("vkMapMemory",
			vkMapMemory(harness->device, scene->image_memory[IMAGE_LINEAR], 0, VK_WHOLE_SIZE, 0, &mapped)))
		return false;
	bool holds = host_layout->rowPitch == LINEAR_ROW_PITCH && layout.rowPitch == LINEAR_ROW_PITCH;
	for (uint32_t y = 0; y < LINEAR_HEIGHT; y++) {
		for (uint32_t x = 0; x < LINEAR_WIDTH; x++) {
			uint8_t want[4];
			host_texel(x, y, want);
			const uint8_t *const linear = (const uint8_t *)mapped + layout.offset + y * layout.rowPitch + x * 4;
			holds = holds && same_texel(read_texel(scene, HOST_COPIED_AT, LINEAR_WIDTH, 4, x, y), want, 4, 0) &&
				same_texel(linear, want, 4, 0);
		}
	}
	vkUnmapMemory(harness->device, scene->image_memory[IMAGE_LINEAR]);
	return holds;
}

static bool buffers_hold(const Scene *scene)
{
	bool holds = true;
	for (uint32_t i = 0; i < FILL_BYTES / 4; i++) {
		uint32_t word;
		memcpy(&word, scene->read + FILL_AT + i * 4, sizeof word);
		holds = holds && word == FILL_WORD;
	}
	for (uint32_t i = 0; i < UPDATE_BYTES; i++)
		holds = holds && scene->read[UPDATE_AT + i] == i + 1;
	for (uint32_t i = 0; i < BYTES_AREA; i++) {
		const bool copied = i >= BYTES_TO && i < BYTES_TO + BYTES_COPIED;
		const uint8_t want = copied ? (uint8_t)(0x40 + BYTES_FROM + i - BYTES_TO) : BYTES_AROUND;
		holds = holds && scene->read[BYTES_AT + i] == want;
	}
	return holds;
}

static void check_images(const Harness *harness, const Scene *scene, const VkSubresourceLayout *host_layout)
{
	check("copies: a chain blitted down linearly keeps its quadrants to 2x2, the 1x1 their mean", chain_holds(scene));
	check("copies: a nearest blit to twice the size repeats each texel two by two", big_holds(scene));
	check("copies: a clear fills the levels asked, in the packed tail, and leaves level 0", cleared_holds(scene));
	check("copies: a level copied between optimal images reads back the same", copied_holds(scene));
	check("copies: part of a level written from a buffer at an offset, in rows longer than it, and the rest kept",
		part_holds(scene));
	check("copies: a linear image the host wrote at its row pitch copies to an optimal image and on to a linear one",
		linear_holds(harness, scene, host_layout));
	check("copies: buffers filled, updated and copied at unaligned offsets hold what was written, and no more",
		buffers_hold(scene));
}

// Each fails its command buffer, recorded alone.
static void check_refusals(const Scene *scene)
{
	bool refused = true;
	for (uint32_t i = 0; i < 3 && refused; i++) {
		refused = begin(scene->commands);
		if (i == 0)
			vkCmdDrawIndirect(scene->commands, scene->indirect, 0, 2, 16);
		else if (i == 1)
			blit(scene->commands, scene->images[IMAGE_CHAIN], 0, CHAIN_SIDE, scene->images[IMAGE_BIG], 0, BIG_SIDE,
				VK_FILTER_NEAREST);
		else
			copy_image(scene->commands, scene->images[IMAGE_CHAIN], scene->images[IMAGE_COPIED], 1, 1);
		refused = refused && vkEndCommandBuffer(scene->commands) == VK_ERROR_FEATURE_NOT_PRESENT;
	}
	check(
		"copies: two draws an indirect command, blits between formats and image copies of part of a level are refused",
		refused);
}

static void check_properties(const Harness *harness)
{
	const VkFormatFeatureFlags blits = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
	const VkFormat bytes[] = { VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8B8A8_UNORM,
		VK_FORMAT_B8G8R8A8_UNORM };
	bool formats = true;
	for (uint32_t i = 0; i < sizeof bytes / sizeof bytes[0]; i++) {
		VkFormatProperties properties;
		vkGetPhysicalDeviceFormatProperties(harness->physical_device, bytes[i], &properties);
		formats = formats && (properties.optimalTilingFeatures & blits) == blits;
	}
	VkFormatProperties floats;
	vkGetPhysicalDeviceFormatProperties(harness->physical_device, VK_FORMAT_R32_SFLOAT, &floats);
	VkImageFormatProperties linear;
	const bool copied = vkGetPhysicalDeviceImageFormatProperties(harness->physical_device, VK_FORMAT_R8G8B8A8_UNORM,
							VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_LINEAR, TRANSFER_USAGE, 0, &linear) == VK_SUCCESS &&
		linear.maxMipLevels == 1;
	const bool not_sampled =
		vkGetPhysicalDeviceImageFormatProperties(harness->physical_device, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_2D,
			VK_IMAGE_TILING_LINEAR, VK_IMAGE_USAGE_SAMPLED_BIT, 0, &linear) == VK_ERROR_FORMAT_NOT_SUPPORTED;
	VkPhysicalDeviceProperties device;
	vkGetPhysicalDeviceProperties(harness->physical_device, &device);
	check(
		"copies: formats of 8-bit channels blit and R32_SFLOAT doesn't; linear images of one level are copied, not "
		"sampled; an indirect command draws once",
		formats && !(floats.optimalTilingFeatures & blits) && copied && not_sampled &&
			device.limits.maxDrawIndirectCount == 1);
	bool found = true;
	for (uint32_t i = 0; i < sizeof step_2_commands / sizeof step_2_commands[0]; i++)
		found = found && vkGetDeviceProcAddr(harness->device, step_2_commands[i]);
	check("copies: step 2's 10 commands are found by name", found);
}

static void destroy(const Harness *harness, const Scene *scene)
{
	const VkDevice device = harness->device;
	if (scene->commands)
		vkFreeCommandBuffers(device, harness->pool, 1, &scene->commands);
	for (uint32_t i = 0; i < IMAGES; i++) {
		vkDestroyImage(device, scene->images[i], NULL);
		vkFreeMemory(device, scene->image_memory[i], NULL);
	}
	const VkBuffer buffers[] = { scene->vertices, scene->indices, scene->indirect, scene->staging, scene->readback };
	const VkDeviceMemory memories[] = { scene->vertex_memory, scene->index_memory, scene->indirect_memory,
		scene->staging_memory, scene->readback_memory };
	for (uint32_t i = 0; i < sizeof buffers / sizeof buffers[0]; i++) {
		vkDestroyBuffer(device, buffers[i], NULL);
		vkFreeMemory(device, memories[i], NULL);
	}
	vkDestroyPipeline(device, scene->pipeline, NULL);
	vkDestroyPipelineLayout(device, scene->layout, NULL);
	vkDestroyFramebuffer(device, scene->framebuffer, NULL);
	vkDestroyImageView(device, scene->target_view, NULL);
	vkDestroyImage(device, scene->target, NULL);
	vkFreeMemory(device, scene->target_memory, NULL);
	vkDestroyRenderPass(device, scene->render_pass, NULL);
	vkDestroyRenderPass(device, scene->clear_pass, NULL);
}

void test_copies(const Harness *harness)
{
	Scene scene = { .render_pass = VK_NULL_HANDLE };
	const VkCommandBufferAllocateInfo allocate = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = harness->pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1,
	};
	bool ok = build_target(harness, &scene) && build_buffers(harness, &scene) &&
		harness_ok("vkAllocateCommandBuffers", vkAllocateCommandBuffers(harness->device, &allocate, &scene.commands));
	for (uint32_t i = 0; i < IMAGES && ok; i++)
		ok = make_image(harness, &image_specs[i], &scene.images[i], &scene.image_memory[i]);
	VkSubresourceLayout host_layout = { 0 };
	ok = ok && stage(harness, &scene, &host_layout);
	check("copies: the target, buffers and images made", ok);
	if (ok && begin(scene.commands)) {
		record_draws(&scene);
		if (submit(harness, scene.commands))
			check_draws(&scene);
	}
	if (ok && begin(scene.commands)) {
		record_images(&scene);
		if (submit(harness, scene.commands))
			check_images(harness, &scene, &host_layout);
	}
	if (ok)
		check_refusals(&scene);
	check_properties(harness);
	destroy(harness, &scene);
}
