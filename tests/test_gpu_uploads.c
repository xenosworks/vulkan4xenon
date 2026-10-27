#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "harness.h"
#include "tests.h"
#include "xsdk/log.h"

// vkCmdCopyBufferToImage drawn on the GPU (milestone 67): a staging buffer in the GPU's write-combined memory, which
// the CPU reads back at about 40 MB/s, copied into a 512x512 image whole, as Flycast copies a movie frame each frame;
// then into a rectangle of it from rows longer than the rectangle; then whole again, the buffer filled with zeros after
// the copy in its command buffer; and into a 64x64 image in the other 8-bit format. Each is read back through
// vkCmdCopyImageToBuffer into cached memory: the image must hold the copy's texels bit for bit and the rest what it
// held, and the first copy must be done soon after its submit.

enum {
	FENCE_TIMEOUT_NS = 1000000000,
	TEXEL_BYTES = 4,
	SIDE = 512,
	IMAGE_BYTES = SIDE * SIDE * TEXEL_BYTES,
	// The rectangle, on the resolve's 8-pixel grid, from rows the image's width long, after the whole image's.
	RECT_X = 128,
	RECT_Y = 64,
	RECT_WIDTH = 256,
	RECT_HEIGHT = 128,
	RECT_AT = IMAGE_BYTES,
	RECT_BYTES = SIDE * RECT_HEIGHT * TEXEL_BYTES,
	// The other format's image, its rows after the rectangle's, 4 KB aligned.
	SMALL_SIDE = 64,
	SMALL_AT = RECT_AT + RECT_BYTES,
	SMALL_BYTES = SMALL_SIDE * SMALL_SIDE * TEXEL_BYTES,
	STAGING_BYTES = SMALL_AT + SMALL_BYTES,
	// The first copy's submission done within this: on the CPU, reading write-combined memory, the same copy took 21 to
	// 26 ms in Flycast on the console (2026-10-27); xsdk_gpu_wait looks between 1 ms sleeps (xsdk:include/xsdk/gpu.h).
	DONE_LIMIT_US = 5000,
	MAX_MISSES_LOGGED = 4,
	MICROSECONDS_PER_SECOND = 1000000,
	NANOSECONDS_PER_MICROSECOND = 1000,
};

typedef struct {
	VkImage image;
	VkDeviceMemory image_memory;
	VkImageView view;
	VkImage small;
	VkDeviceMemory small_memory;
	VkImageView small_view;
	VkBuffer staging;
	VkDeviceMemory staging_memory;
	uint8_t *staged;
	VkBuffer readback;
	VkDeviceMemory readback_memory;
	const uint8_t *read;
	VkCommandBuffer commands;
} Scene;

// Each channel of a texel apart from its neighbours'; the rectangle's are each channel's complement, so every channel
// differs from what the first copy left.
static void first_texel(uint32_t x, uint32_t y, uint8_t out[TEXEL_BYTES])
{
	out[0] = (uint8_t)x;
	out[1] = (uint8_t)y;
	out[2] = (uint8_t)(x >> 8 | y >> 8 << 1 | (x ^ y) << 2);
	out[3] = (uint8_t)(x * 7 + y * 13);
}

static void rect_texel(uint32_t x, uint32_t y, uint8_t out[TEXEL_BYTES])
{
	first_texel(x, y, out);
	for (uint32_t c = 0; c < TEXEL_BYTES; c++)
		out[c] = (uint8_t)~out[c];
}

// The whole image's texels, then the rectangle's rows, each the image's width long, past the rectangle a texel no copy
// takes; then the small image's.
static void fill_staging(uint8_t *staged)
{
	for (uint32_t y = 0; y < SIDE; y++) {
		for (uint32_t x = 0; x < SIDE; x++)
			first_texel(x, y, staged + (y * SIDE + x) * TEXEL_BYTES);
	}
	for (uint32_t y = 0; y < RECT_HEIGHT; y++) {
		for (uint32_t x = 0; x < SIDE; x++) {
			uint8_t *const out = staged + RECT_AT + (y * SIDE + x) * TEXEL_BYTES;
			if (x < RECT_WIDTH)
				rect_texel(RECT_X + x, RECT_Y + y, out);
			else
				memset(out, 0xAA, TEXEL_BYTES);
		}
	}
	for (uint32_t y = 0; y < SMALL_SIDE; y++) {
		for (uint32_t x = 0; x < SMALL_SIDE; x++)
			rect_texel(x, y, staged + SMALL_AT + (y * SMALL_SIDE + x) * TEXEL_BYTES);
	}
}

// A buffer bound to memory of the type its requirements list, cached or not, mapped.
static bool make_buffer(const Harness *harness, VkBufferUsageFlags usage, uint32_t size_bytes, bool cached,
	VkBuffer *buffer, VkDeviceMemory *memory, void **mapped)
{
	const VkBufferCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size_bytes, .usage = usage
	};
	if (!harness_ok("vkCreateBuffer", vkCreateBuffer(harness->device, &info, NULL, buffer)))
		return false;
	VkMemoryRequirements requirements;
	vkGetBufferMemoryRequirements(harness->device, *buffer, &requirements);
	VkPhysicalDeviceMemoryProperties properties;
	vkGetPhysicalDeviceMemoryProperties(harness->physical_device, &properties);
	uint32_t type = UINT32_MAX;
	for (uint32_t i = 0; i < properties.memoryTypeCount && type == UINT32_MAX; i++) {
		const bool is_cached = (properties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) != 0;
		if ((requirements.memoryTypeBits & 1u << i) && is_cached == cached)
			type = i;
	}
	if (type == UINT32_MAX) {
		xsdk_log_write("gpu uploads: no memory type %s cached for the buffer", cached ? "" : "un");
		return false;
	}
	const VkMemoryAllocateInfo allocate = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = requirements.size,
		.memoryTypeIndex = type,
	};
	return harness_ok("vkAllocateMemory", vkAllocateMemory(harness->device, &allocate, NULL, memory)) &&
		harness_ok("vkBindBufferMemory", vkBindBufferMemory(harness->device, *buffer, *memory, 0)) &&
		harness_ok("vkMapMemory", vkMapMemory(harness->device, *memory, 0, VK_WHOLE_SIZE, 0, mapped));
}

static bool build(const Harness *harness, Scene *scene)
{
	const VkImageUsageFlags usage =
		VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	void *staged = NULL;
	void *read = NULL;
	bool ok = harness_ok("image",
				  harness_image(harness, VK_FORMAT_R8G8B8A8_UNORM, SIDE, SIDE, usage, &scene->image,
					  &scene->image_memory, &scene->view)) &&
		harness_ok("small image",
			harness_image(harness, VK_FORMAT_B8G8R8A8_UNORM, SMALL_SIDE, SMALL_SIDE, usage, &scene->small,
				&scene->small_memory, &scene->small_view)) &&
		make_buffer(harness, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, STAGING_BYTES, false,
			&scene->staging, &scene->staging_memory, &staged) &&
		make_buffer(harness, VK_BUFFER_USAGE_TRANSFER_DST_BIT, IMAGE_BYTES, true, &scene->readback,
			&scene->readback_memory, &read);
	scene->staged = staged;
	scene->read = read;
	const VkCommandBufferAllocateInfo commands = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = harness->pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1,
	};
	ok = ok &&
		harness_ok("vkAllocateCommandBuffers", vkAllocateCommandBuffers(harness->device, &commands, &scene->commands));
	if (ok)
		fill_staging(scene->staged);
	return ok;
}

static uint64_t now_us(void)
{
	struct timespec now;
	(void)clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * MICROSECONDS_PER_SECOND + (uint64_t)now.tv_nsec / NANOSECONDS_PER_MICROSECOND;
}

static bool begin(VkCommandBuffer commands)
{
	const VkCommandBufferBeginInfo info = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	return harness_ok("vkBeginCommandBuffer", vkBeginCommandBuffer(commands, &info));
}

// Ends the commands and submits them with the GPU idle; how long until the fence after.
static bool submit(const Harness *harness, VkCommandBuffer commands, uint64_t *done_us)
{
	const VkSubmitInfo info = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.commandBufferCount = 1,
		.pCommandBuffers = &commands,
	};
	if (!harness_ok("vkEndCommandBuffer", vkEndCommandBuffer(commands)) ||
		!harness_ok("vkQueueWaitIdle", vkQueueWaitIdle(harness->queue)))
		return false;
	const uint64_t start = now_us();
	const bool done = harness_ok("vkQueueSubmit", vkQueueSubmit(harness->queue, 1, &info, harness->fence)) &&
		harness_ok("vkWaitForFences", vkWaitForFences(harness->device, 1, &harness->fence, VK_TRUE, FENCE_TIMEOUT_NS));
	*done_us = now_us() - start;
	(void)vkResetFences(harness->device, 1, &harness->fence);
	return done;
}

static void copy_in(const Scene *scene, VkImage image, uint32_t at, uint32_t row_texels, VkRect2D rect)
{
	const VkBufferImageCopy region = {
		.bufferOffset = at,
		.bufferRowLength = row_texels,
		.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
		.imageOffset = { rect.offset.x, rect.offset.y, 0 },
		.imageExtent = { rect.extent.width, rect.extent.height, 1 },
	};
	vkCmdCopyBufferToImage(scene->commands, scene->staging, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
}

// The image read back whole into the cached buffer, in a submission of its own.
static bool read_back(const Harness *harness, const Scene *scene, VkImage image, uint32_t side)
{
	if (!begin(scene->commands))
		return false;
	const VkBufferImageCopy region = {
		.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
		.imageExtent = { side, side, 1 },
	};
	vkCmdCopyImageToBuffer(scene->commands, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, scene->readback, 1, &region);
	uint64_t done_us;
	return submit(harness, scene->commands, &done_us);
}

static bool inside_rect(uint32_t x, uint32_t y)
{
	return x >= RECT_X && x < RECT_X + RECT_WIDTH && y >= RECT_Y && y < RECT_Y + RECT_HEIGHT;
}

// The texels read back that differ from what each should hold: the rectangle's inside it if asked, the first copy's
// elsewhere; the first few logged.
static uint32_t wrong_texels(const Scene *scene, const char *what, bool with_rect)
{
	uint32_t wrong = 0;
	for (uint32_t y = 0; y < SIDE; y++) {
		for (uint32_t x = 0; x < SIDE; x++) {
			uint8_t want[TEXEL_BYTES];
			if (with_rect && inside_rect(x, y))
				rect_texel(x, y, want);
			else
				first_texel(x, y, want);
			const uint8_t *const texel = scene->read + (y * SIDE + x) * TEXEL_BYTES;
			if (!memcmp(texel, want, TEXEL_BYTES))
				continue;
			if (wrong++ < MAX_MISSES_LOGGED)
				xsdk_log_write("gpu uploads: %s, (%u, %u) is %u %u %u %u, not %u %u %u %u", what, (unsigned)x,
					(unsigned)y, texel[0], texel[1], texel[2], texel[3], want[0], want[1], want[2], want[3]);
		}
	}
	return wrong;
}

static uint32_t wrong_small_texels(const Scene *scene)
{
	uint32_t wrong = 0;
	for (uint32_t y = 0; y < SMALL_SIDE; y++) {
		for (uint32_t x = 0; x < SMALL_SIDE; x++) {
			uint8_t want[TEXEL_BYTES];
			rect_texel(x, y, want);
			const uint8_t *const texel = scene->read + (y * SMALL_SIDE + x) * TEXEL_BYTES;
			if (!memcmp(texel, want, TEXEL_BYTES))
				continue;
			if (wrong++ < MAX_MISSES_LOGGED)
				xsdk_log_write("gpu uploads: the B8G8R8A8 copy, (%u, %u) is %u %u %u %u, not %u %u %u %u", (unsigned)x,
					(unsigned)y, texel[0], texel[1], texel[2], texel[3], want[0], want[1], want[2], want[3]);
		}
	}
	return wrong;
}

static void check_copies(const Harness *harness, const Scene *scene)
{
	const VkRect2D whole = { { 0, 0 }, { SIDE, SIDE } };
	const VkRect2D rect = { { RECT_X, RECT_Y }, { RECT_WIDTH, RECT_HEIGHT } };
	uint64_t whole_us = 0;
	bool ran = begin(scene->commands);
	copy_in(scene, scene->image, 0, 0, whole);
	ran = ran && submit(harness, scene->commands, &whole_us);
	xsdk_log_write("gpu uploads: the 512x512 copy from write-combined memory was done %llu us after its submit",
		(unsigned long long)whole_us);
	check("gpu uploads: a 512x512 copy from write-combined memory is done within 5 ms of its submit",
		ran && whole_us <= DONE_LIMIT_US);
	check("gpu uploads: a 512x512 copy from write-combined memory holds every texel, bit for bit",
		ran && read_back(harness, scene, scene->image, SIDE) && wrong_texels(scene, "the whole copy", false) == 0);

	uint64_t done_us;
	ran = begin(scene->commands);
	copy_in(scene, scene->image, RECT_AT, SIDE, rect);
	ran = ran && submit(harness, scene->commands, &done_us);
	check(
		"gpu uploads: a rectangle copied from rows longer than it holds its texels, the rest of the image what it held",
		ran && read_back(harness, scene, scene->image, SIDE) && wrong_texels(scene, "the rectangle", true) == 0);

	// The fill writes on the CPU at replay, so it must wait for the GPU to have read the rows.
	ran = begin(scene->commands);
	copy_in(scene, scene->image, 0, 0, whole);
	const VkMemoryBarrier barrier = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
		.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
		.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
	};
	vkCmdPipelineBarrier(scene->commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1,
		&barrier, 0, NULL, 0, NULL);
	vkCmdFillBuffer(scene->commands, scene->staging, 0, IMAGE_BYTES, 0);
	ran = ran && submit(harness, scene->commands, &done_us);
	check("gpu uploads: a fill of the rows after their copy, in its command buffer, doesn't reach the image",
		ran && read_back(harness, scene, scene->image, SIDE) &&
			wrong_texels(scene, "the copy before a fill", false) == 0);

	const VkRect2D small = { { 0, 0 }, { SMALL_SIDE, SMALL_SIDE } };
	ran = begin(scene->commands);
	copy_in(scene, scene->small, SMALL_AT, 0, small);
	ran = ran && submit(harness, scene->commands, &done_us);
	check("gpu uploads: a B8G8R8A8 image copied from write-combined memory holds every texel, bit for bit",
		ran && read_back(harness, scene, scene->small, SMALL_SIDE) && wrong_small_texels(scene) == 0);
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
	vkDestroyImageView(device, scene->small_view, NULL);
	vkDestroyImage(device, scene->small, NULL);
	vkFreeMemory(device, scene->small_memory, NULL);
	vkDestroyImageView(device, scene->view, NULL);
	vkDestroyImage(device, scene->image, NULL);
	vkFreeMemory(device, scene->image_memory, NULL);
}

void test_gpu_uploads(const Harness *harness)
{
	Scene scene = { .image = VK_NULL_HANDLE };
	const bool ok = build(harness, &scene);
	check("gpu uploads: the images and buffers made", ok);
	if (ok)
		check_copies(harness, &scene);
	destroy(harness, &scene);
}
