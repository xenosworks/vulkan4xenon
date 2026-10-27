#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "harness.h"
#include "tests.h"
#include "xsdk/log.h"

// Mip chains made by linear blits on the CPU (milestone 67), as RetroArch makes its menu's: each level of a format of
// 8-bit channels the mean of the texels under it, byte by byte. A 64x32 R8G8B8A8 level blitted to half, and a 32x2
// B8G8R8A8 chain to 16x1 and 8x1, where the height stays, are read back texel by texel; and a 256x256 chain blitted
// down linearly must take at most a third of the time the same chain takes nearest, which still unpacks and packs each
// texel.

enum {
	FENCE_TIMEOUT_NS = 1000000000,
	TEXEL_BYTES = 4,
	// The memory type the CPU caches, which buffers only copied to and from may take.
	CACHED_TYPE = 1,
	WIDE_WIDTH = 64,
	WIDE_HEIGHT = 32,
	FLAT_WIDTH = 32,
	FLAT_HEIGHT = 2,
	FLAT_LEVELS = 3,
	TIMED_SIDE = 256,
	TIMED_LEVELS = 9,
	// A mean within 1 of the exact one, which the float path's rounding to even may give at a tie; a wrong texel pair
	// is 2 or more away in a channel, as texels step by 4 or more in one channel across and in one down.
	TOLERANCE = 1,
	// The linear chain against the nearest one.
	TIME_RATIO = 3,
	// Staging: the wide base, the flat base, the timed base.
	WIDE_AT = 0,
	FLAT_AT = WIDE_AT + WIDE_WIDTH * WIDE_HEIGHT * TEXEL_BYTES,
	TIMED_AT = FLAT_AT + FLAT_WIDTH * FLAT_HEIGHT * TEXEL_BYTES,
	STAGING_BYTES = TIMED_AT + TIMED_SIDE * TIMED_SIDE * TEXEL_BYTES,
	// Read back: the wide level 1, then the flat level 2.
	WIDE_READ_AT = 0,
	FLAT_READ_AT = WIDE_READ_AT + WIDE_WIDTH / 2 * WIDE_HEIGHT / 2 * TEXEL_BYTES,
	READBACK_BYTES = FLAT_READ_AT + FLAT_WIDTH / 4 * TEXEL_BYTES,
	MAX_MISSES_LOGGED = 4,
	MICROSECONDS_PER_SECOND = 1000000,
	NANOSECONDS_PER_MICROSECOND = 1000,
};

typedef struct {
	VkImage wide;
	VkDeviceMemory wide_memory;
	VkImage flat;
	VkDeviceMemory flat_memory;
	VkImage timed;
	VkDeviceMemory timed_memory;
	VkBuffer staging;
	VkDeviceMemory staging_memory;
	VkBuffer readback;
	VkDeviceMemory readback_memory;
	const uint8_t *read;
	VkCommandBuffer commands;
} Scene;

// Red steps by 4 across, green by 8 down, blue by 12 across and 4 down, alpha down by 4 across and 8 down.
static void texel(uint32_t x, uint32_t y, uint8_t out[TEXEL_BYTES])
{
	out[0] = (uint8_t)(4 * x + 1);
	out[1] = (uint8_t)(8 * y + 3);
	out[2] = (uint8_t)(12 * x + 4 * y);
	out[3] = (uint8_t)(255 - 4 * x - 8 * y);
}

static bool make_buffer(const Harness *harness, VkBufferUsageFlags usage, uint32_t size_bytes, VkBuffer *buffer,
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

static bool make_image(const Harness *harness, VkFormat format, uint32_t width, uint32_t height, uint32_t levels,
	VkImage *image, VkDeviceMemory *memory)
{
	const VkImageCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = format,
		.extent = { width, height, 1 },
		.mipLevels = levels,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
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

static void fill_base(uint8_t *at, uint32_t width, uint32_t height)
{
	for (uint32_t y = 0; y < height; y++) {
		for (uint32_t x = 0; x < width; x++)
			texel(x, y, at + (y * width + x) * TEXEL_BYTES);
	}
}

static bool build(const Harness *harness, Scene *scene)
{
	void *staged = NULL;
	void *read = NULL;
	bool ok =
		make_image(harness, VK_FORMAT_R8G8B8A8_UNORM, WIDE_WIDTH, WIDE_HEIGHT, 2, &scene->wide, &scene->wide_memory) &&
		make_image(harness, VK_FORMAT_B8G8R8A8_UNORM, FLAT_WIDTH, FLAT_HEIGHT, FLAT_LEVELS, &scene->flat,
			&scene->flat_memory) &&
		make_image(harness, VK_FORMAT_R8G8B8A8_UNORM, TIMED_SIDE, TIMED_SIDE, TIMED_LEVELS, &scene->timed,
			&scene->timed_memory) &&
		make_buffer(harness, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, STAGING_BYTES, &scene->staging, &scene->staging_memory,
			&staged) &&
		make_buffer(harness, VK_BUFFER_USAGE_TRANSFER_DST_BIT, READBACK_BYTES, &scene->readback,
			&scene->readback_memory, &read);
	scene->read = read;
	if (ok) {
		fill_base((uint8_t *)staged + WIDE_AT, WIDE_WIDTH, WIDE_HEIGHT);
		fill_base((uint8_t *)staged + FLAT_AT, FLAT_WIDTH, FLAT_HEIGHT);
		fill_base((uint8_t *)staged + TIMED_AT, TIMED_SIDE, TIMED_SIDE);
	}
	const VkCommandBufferAllocateInfo commands = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = harness->pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1,
	};
	return ok &&
		harness_ok("vkAllocateCommandBuffers", vkAllocateCommandBuffers(harness->device, &commands, &scene->commands));
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

// Ends the commands and submits them with the GPU idle; how long the submit, which replays them, took.
static bool submit(const Harness *harness, VkCommandBuffer commands, uint64_t *replay_us)
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
	const bool submitted = harness_ok("vkQueueSubmit", vkQueueSubmit(harness->queue, 1, &info, harness->fence));
	*replay_us = now_us() - start;
	const bool done = submitted &&
		harness_ok("vkWaitForFences", vkWaitForFences(harness->device, 1, &harness->fence, VK_TRUE, FENCE_TIMEOUT_NS));
	(void)vkResetFences(harness->device, 1, &harness->fence);
	return done;
}

static void copy_in(const Scene *scene, VkImage image, uint32_t at, uint32_t width, uint32_t height)
{
	const VkBufferImageCopy region = {
		.bufferOffset = at,
		.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
		.imageExtent = { width, height, 1 },
	};
	vkCmdCopyBufferToImage(scene->commands, scene->staging, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
}

static void copy_out(const Scene *scene, VkImage image, uint32_t level, uint32_t width, uint32_t height, uint32_t at)
{
	const VkBufferImageCopy region = {
		.bufferOffset = at,
		.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1 },
		.imageExtent = { width, height, 1 },
	};
	vkCmdCopyImageToBuffer(scene->commands, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, scene->readback, 1, &region);
}

static uint32_t side(uint32_t base, uint32_t level)
{
	return base >> level ? base >> level : 1;
}

// Level level from level - 1, as RetroArch blits its chains.
static void blit_down(
	const Scene *scene, VkImage image, uint32_t width, uint32_t height, uint32_t level, VkFilter filter)
{
	const VkImageBlit region = {
		.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, level - 1, 0, 1 },
		.srcOffsets = { { 0, 0, 0 }, { (int32_t)side(width, level - 1), (int32_t)side(height, level - 1), 1 } },
		.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1 },
		.dstOffsets = { { 0, 0, 0 }, { (int32_t)side(width, level), (int32_t)side(height, level), 1 } },
	};
	vkCmdBlitImage(scene->commands, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image,
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, filter);
}

// The mean of the texels under destination texel (x, y) of a level from a source level of these texels, rounded half
// up.
static void box_mean(const uint8_t *source, uint32_t from_width, uint32_t from_height, uint32_t to_width,
	uint32_t to_height, uint32_t x, uint32_t y, uint8_t out[TEXEL_BYTES])
{
	const uint32_t x0 = from_width == to_width ? x : 2 * x;
	const uint32_t x1 = from_width == to_width ? x : 2 * x + 1;
	const uint32_t y0 = from_height == to_height ? y : 2 * y;
	const uint32_t y1 = from_height == to_height ? y : 2 * y + 1;
	for (uint32_t c = 0; c < TEXEL_BYTES; c++) {
		const uint32_t sum = source[(y0 * from_width + x0) * TEXEL_BYTES + c] +
			source[(y0 * from_width + x1) * TEXEL_BYTES + c] + source[(y1 * from_width + x0) * TEXEL_BYTES + c] +
			source[(y1 * from_width + x1) * TEXEL_BYTES + c];
		out[c] = (uint8_t)((sum + 2) / 4);
	}
}

// A level's texels, as read back from at, against the means of the source's, each channel within TOLERANCE.
static uint32_t wrong_texels(const uint8_t *got, const uint8_t *source, uint32_t from_width, uint32_t from_height,
	uint32_t to_width, uint32_t to_height, const char *what)
{
	uint32_t wrong = 0;
	for (uint32_t y = 0; y < to_height; y++) {
		for (uint32_t x = 0; x < to_width; x++) {
			uint8_t want[TEXEL_BYTES];
			box_mean(source, from_width, from_height, to_width, to_height, x, y, want);
			const uint8_t *const have = got + (y * to_width + x) * TEXEL_BYTES;
			bool near = true;
			for (uint32_t c = 0; c < TEXEL_BYTES; c++)
				near = near && (have[c] > want[c] ? have[c] - want[c] : want[c] - have[c]) <= TOLERANCE;
			if (near)
				continue;
			if (wrong++ < MAX_MISSES_LOGGED)
				xsdk_log_write("cpu mips: %s, (%u, %u) is %u %u %u %u, not %u %u %u %u", what, (unsigned)x, (unsigned)y,
					have[0], have[1], have[2], have[3], want[0], want[1], want[2], want[3]);
		}
	}
	return wrong;
}

static void check_levels(const Harness *harness, const Scene *scene)
{
	uint64_t replay_us;
	bool ran = begin(scene->commands);
	copy_in(scene, scene->wide, WIDE_AT, WIDE_WIDTH, WIDE_HEIGHT);
	copy_in(scene, scene->flat, FLAT_AT, FLAT_WIDTH, FLAT_HEIGHT);
	blit_down(scene, scene->wide, WIDE_WIDTH, WIDE_HEIGHT, 1, VK_FILTER_LINEAR);
	for (uint32_t level = 1; level < FLAT_LEVELS; level++)
		blit_down(scene, scene->flat, FLAT_WIDTH, FLAT_HEIGHT, level, VK_FILTER_LINEAR);
	copy_out(scene, scene->wide, 1, WIDE_WIDTH / 2, WIDE_HEIGHT / 2, WIDE_READ_AT);
	copy_out(scene, scene->flat, 2, FLAT_WIDTH / 4, 1, FLAT_READ_AT);
	ran = ran && submit(harness, scene->commands, &replay_us);
	uint8_t wide[WIDE_WIDTH * WIDE_HEIGHT * TEXEL_BYTES];
	fill_base(wide, WIDE_WIDTH, WIDE_HEIGHT);
	check("cpu mips: a 64x32 level blitted to half linearly holds the mean of each 2x2 texels",
		ran &&
			wrong_texels(scene->read + WIDE_READ_AT, wide, WIDE_WIDTH, WIDE_HEIGHT, WIDE_WIDTH / 2, WIDE_HEIGHT / 2,
				"the 64x32 level's half") == 0);
	// The flat chain's 16x1 level as the test works it out, then its 8x1 from that, the height staying 1.
	uint8_t flat[FLAT_WIDTH * FLAT_HEIGHT * TEXEL_BYTES];
	uint8_t middle[FLAT_WIDTH / 2 * TEXEL_BYTES];
	fill_base(flat, FLAT_WIDTH, FLAT_HEIGHT);
	for (uint32_t x = 0; x < FLAT_WIDTH / 2; x++)
		box_mean(flat, FLAT_WIDTH, FLAT_HEIGHT, FLAT_WIDTH / 2, 1, x, 0, middle + x * TEXEL_BYTES);
	check("cpu mips: a B8G8R8A8 chain from 32x2 holds at 8x1 the means across of its 16x1",
		ran &&
			wrong_texels(scene->read + FLAT_READ_AT, middle, FLAT_WIDTH / 2, 1, FLAT_WIDTH / 4, 1, "the 8x1 level") ==
				0);
}

// The 256x256 chain blitted down once linearly and once nearest, from the same base, each replay timed.
static void check_time(const Harness *harness, const Scene *scene)
{
	uint64_t times[2] = { 0, 0 };
	bool ran = begin(scene->commands);
	copy_in(scene, scene->timed, TIMED_AT, TIMED_SIDE, TIMED_SIDE);
	uint64_t copy_us;
	ran = ran && submit(harness, scene->commands, &copy_us);
	static const VkFilter filters[2] = { VK_FILTER_LINEAR, VK_FILTER_NEAREST };
	for (uint32_t i = 0; i < 2 && ran; i++) {
		ran = begin(scene->commands);
		for (uint32_t level = 1; level < TIMED_LEVELS; level++)
			blit_down(scene, scene->timed, TIMED_SIDE, TIMED_SIDE, level, filters[i]);
		ran = ran && submit(harness, scene->commands, &times[i]);
	}
	xsdk_log_write("cpu mips: a 256x256 chain blitted down took %llu us linearly and %llu us nearest",
		(unsigned long long)times[0], (unsigned long long)times[1]);
	check("cpu mips: a 256x256 chain blitted down linearly takes at most a third of the time nearest does",
		ran && TIME_RATIO * times[0] <= times[1]);
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
	vkDestroyImage(device, scene->timed, NULL);
	vkFreeMemory(device, scene->timed_memory, NULL);
	vkDestroyImage(device, scene->flat, NULL);
	vkFreeMemory(device, scene->flat_memory, NULL);
	vkDestroyImage(device, scene->wide, NULL);
	vkFreeMemory(device, scene->wide_memory, NULL);
}

void test_cpu_mips(const Harness *harness)
{
	Scene scene = { .wide = VK_NULL_HANDLE };
	const bool ok = build(harness, &scene);
	check("cpu mips: the images and buffers made", ok);
	if (ok) {
		check_levels(harness, &scene);
		check_time(harness, &scene);
	}
	destroy(harness, &scene);
}
