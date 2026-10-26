#include "transfer.h"

#include <altivec.h>
#include <math.h>
#include <string.h>

#include <vulkan/vulkan.h>

#include "format.h"
#include "image.h"
#include "objects.h"
#include "profile.h"
#include "query.h"
#include "sync.h"
#include "xsdk/gpu.h"
#include "xsdk/log.h"
#include "xsdk/texture.h"

enum {
	IDLE_TIMEOUT_MS = 10000,
	// A row of the widest image, at its widest texels.
	ROW_BYTES = XSDK_TEXTURE_MAX_SIZE * V4X_MAX_TEXEL_BYTES,
	// The source rows a blit filters between, and the row it writes.
	ROW_ABOVE = 0,
	ROW_BELOW = 1,
	ROW_OUT = 2,
	ROWS = 3,
	// D24's largest value, depth 1.
	DEPTH_MAX = 0xFFFFFF,
	// An indexed instanced draw's copies of its indices at most, which met step 1's cost targets on the console
	// (xsdk:docs/HARDWARE.md, "Instancing on the console, milestone 47 step 1").
	INDEX_COPIES = 64,
	// The bits of an index the GPU takes (xenia:src/xenia/gpu/dxbc_shader_translator.cc:346-374).
	INDEX_MASK = 0xFFFFFF,
	QUAD_BYTES = 16,
};

// Rows of texels on their way between images; the queue is externally synchronized, so one set will do.
static uint8_t rows[ROWS][ROW_BYTES] __attribute__((aligned(16)));
// A draw's indices on their way into their copies, likewise.
static uint32_t indices[V4X_MAX_DRAW_VERTICES];

static uint32_t level_size(uint32_t size, uint32_t level)
{
	return size >> level ? size >> level : 1;
}

// The CPU reads or writes what a submission used, so the GPU must be done with it: nothing to wait for if no submission
// has; the fence the last one that did ended with; a fence now if this one did (docs/ROADMAP.md, milestones 2 and 12).
static void wait_for_serial(VkQueue queue, uint32_t serial, const char *what)
{
	if (!serial)
		return;
	const uint32_t fence = serial == queue->serial ? xsdk_gpu_fence() : v4x_sync_fence_of(queue, serial);
	if (xsdk_gpu_fence_done(fence))
		return;
	v4x_profile_wait_start();
	const NTSTATUS status = xsdk_gpu_wait(fence, IDLE_TIMEOUT_MS);
	v4x_profile_wait_end();
	if (!NT_SUCCESS(status))
		xsdk_log_write("vulkan4xenon: the GPU didn't finish with %s: 0x%08X", what, (unsigned)status);
}

static void wait_for_image(VkQueue queue, const Image *image)
{
	wait_for_serial(queue, image->used_serial, "an image for a copy");
}

// Rows of texels, or of blocks in a compressed format, in a level.
static uint32_t level_rows(const Image *image, uint32_t level)
{
	return v4x_texel_blocks(image->format, level_size(image->height, level));
}

static bool is_compressed(const Image *image)
{
	return v4x_format(image->format)->compressed;
}

// Vulkan's compressed blocks are the Khronos Data Format Specification's bytes, each 16-bit word least significant byte
// first, and xsdk's DXT formats take each word in the CPU's order (xsdk:include/xsdk/texture.h), so copies swap them,
// a store at a time as either side may be write-combined memory.
static void swap_words(volatile const uint32_t *from, volatile uint32_t *to, uint32_t size_bytes)
{
	for (uint32_t i = 0; i < size_bytes / 4; i++) {
		const uint32_t word = from[i];
		to[i] = (word & 0x00FF00FF) << 8 | (word >> 8 & 0x00FF00FF);
	}
}

// The CPU writes the buffer, so the GPU must be done with the draws before, which may read it.
// shortcut: waits for every draw; keep each memory's last use, as images keep theirs, if a port's copies stall on it.
static void wait_for_draws(VkQueue queue)
{
	if (!queue->drawn)
		return;
	v4x_profile_wait_start();
	const NTSTATUS status = xsdk_gpu_wait(xsdk_gpu_fence(), IDLE_TIMEOUT_MS);
	v4x_profile_wait_end();
	if (!NT_SUCCESS(status))
		xsdk_log_write(
			"vulkan4xenon: the GPU didn't finish its draws for a copy into a buffer: 0x%08X", (unsigned)status);
	queue->drawn = false;
}

// The GPU's caches may hold what the CPU replaced; cached memory, which the GPU never reads, is at address 0.
static void buffer_written(uint32_t address, uint32_t size_bytes)
{
	if (address)
		xsdk_gpu_invalidate(address, size_bytes);
}

// The same for an image, which eDRAM no longer holds as it was.
static void image_written(VkQueue queue, const Image *image)
{
	xsdk_gpu_invalidate(image->texture.address, image->texture.size_bytes);
	for (uint32_t i = 0; i < V4X_MAX_COLOR_ATTACHMENTS; i++) {
		if (queue->colors[i].image == image)
			queue->colors[i] = (EdramPart){ .contents = EDRAM_UNKNOWN };
	}
}

// A layer's or 3D slice's rows in the copy's buffer.
static uint8_t *slice_rows(const LevelCopy *copy, uint32_t slice)
{
	return (uint8_t *)(uintptr_t)(copy->buffer + slice * copy->slice_bytes);
}

// A compressed level's layer or 3D slice a row of blocks at a time, swapped through a row of the CPU's.
static void upload_blocks(Image *image, const LevelCopy *copy, uint32_t slice)
{
	const uint32_t row_bytes =
		v4x_texel_blocks(image->format, level_size(image->width, copy->level)) * v4x_texel_bytes(image->format);
	const uint8_t *const from = slice_rows(copy, slice);
	for (uint32_t y = 0; y < level_rows(image, copy->level); y++) {
		swap_words((const uint32_t *)(from + y * copy->row_bytes), (uint32_t *)rows[ROW_OUT], row_bytes);
		xsdk_texture_upload_slice_rows(&image->texture, copy->level, copy->first_slice + slice, y, 1, rows[ROW_OUT], 0);
	}
}

// A region's rows: whole rows as they are, part of each through a row of the level's own, read and written back.
// memcpy from and into write-combined memory is safe, as v4x_transfer_copy_buffer says.
static void upload_texels(Image *image, const LevelCopy *copy, uint32_t slice)
{
	const uint32_t layer = copy->first_slice + slice;
	const uint8_t *const from = slice_rows(copy, slice);
	if (copy->width == level_size(image->width, copy->level)) {
		xsdk_texture_upload_slice_rows(
			&image->texture, copy->level, layer, copy->y, copy->height, from, copy->row_bytes);
		return;
	}
	const uint32_t texel_bytes = v4x_texel_bytes(image->format);
	for (uint32_t y = 0; y < copy->height; y++) {
		xsdk_texture_download_slice_rows(&image->texture, copy->level, layer, copy->y + y, 1, rows[ROW_OUT], 0);
		memcpy(rows[ROW_OUT] + copy->x * texel_bytes, from + y * copy->row_bytes, copy->width * texel_bytes);
		xsdk_texture_upload_slice_rows(&image->texture, copy->level, layer, copy->y + y, 1, rows[ROW_OUT], 0);
	}
}

void v4x_transfer_buffer_to_image(VkQueue queue, const LevelCopy *copy)
{
	Image *const image = copy->image;
	wait_for_image(queue, image);
	for (uint32_t i = 0; i < copy->slice_count; i++) {
		if (is_compressed(image))
			upload_blocks(image, copy, i);
		else
			upload_texels(image, copy, i);
	}
	image_written(queue, image);
}

static void download_blocks(const Image *image, const LevelCopy *copy, uint32_t slice)
{
	const uint32_t row_bytes =
		v4x_texel_blocks(image->format, level_size(image->width, copy->level)) * v4x_texel_bytes(image->format);
	uint8_t *const to = slice_rows(copy, slice);
	for (uint32_t y = 0; y < level_rows(image, copy->level); y++) {
		xsdk_texture_download_slice_rows(
			&image->texture, copy->level, copy->first_slice + slice, y, 1, rows[ROW_OUT], 0);
		swap_words((const uint32_t *)rows[ROW_OUT], (uint32_t *)(to + y * copy->row_bytes), row_bytes);
	}
}

// A region's rows, whole rows as they are and part of each through a row of the CPU's.
static void download_texels(const Image *image, const LevelCopy *copy, uint32_t slice)
{
	const uint32_t layer = copy->first_slice + slice;
	uint8_t *const to = slice_rows(copy, slice);
	if (copy->width == level_size(image->width, copy->level)) {
		xsdk_texture_download_slice_rows(
			&image->texture, copy->level, layer, copy->y, copy->height, to, copy->row_bytes);
		return;
	}
	const uint32_t texel_bytes = v4x_texel_bytes(image->format);
	for (uint32_t y = 0; y < copy->height; y++) {
		xsdk_texture_download_slice_rows(&image->texture, copy->level, layer, copy->y + y, 1, rows[ROW_OUT], 0);
		memcpy(to + y * copy->row_bytes, rows[ROW_OUT] + copy->x * texel_bytes, copy->width * texel_bytes);
	}
}

void v4x_transfer_image_to_buffer(VkQueue queue, const LevelCopy *copy)
{
	const Image *const image = copy->image;
	const uint32_t height = is_compressed(image) ? level_rows(image, copy->level) : copy->height;
	wait_for_image(queue, image);
	wait_for_draws(queue);
	for (uint32_t i = 0; i < copy->slice_count; i++) {
		if (is_compressed(image))
			download_blocks(image, copy, i);
		else
			download_texels(image, copy, i);
	}
	buffer_written(copy->address, (copy->slice_count - 1) * copy->slice_bytes + height * copy->row_bytes);
}

// A row at a time, as the two levels' layouts may differ, each layer or 3D slice into its own.
void v4x_transfer_copy_image(VkQueue queue, const Command *command)
{
	const Image *const source = command->image_copy.source;
	Image *const destination = command->image_copy.destination;
	const uint32_t from_level = command->image_copy.source_level;
	const uint32_t to_level = command->image_copy.destination_level;
	wait_for_image(queue, source);
	wait_for_image(queue, destination);
	const uint32_t height = level_rows(source, from_level);
	for (uint32_t i = 0; i < command->image_copy.slice_count; i++) {
		const uint32_t from = command->image_copy.source_slice + i;
		const uint32_t to = command->image_copy.destination_slice + i;
		for (uint32_t y = 0; y < height; y++) {
			xsdk_texture_download_slice_rows(&source->texture, from_level, from, y, 1, rows[ROW_OUT], 0);
			xsdk_texture_upload_slice_rows(&destination->texture, to_level, to, y, 1, rows[ROW_OUT], 0);
		}
	}
	image_written(queue, destination);
}

// Where a destination texel's centre falls among the source's, as a blit samples: the nearest, or the two either side
// and how far towards the second, edges clamped.
typedef struct {
	uint32_t first;
	uint32_t second;
	float towards_second;
} Taps;

static Taps taps(uint32_t index, uint32_t to, uint32_t from, bool linear)
{
	const float centre = ((float)index + 0.5f) * (float)from / (float)to;
	if (!linear) {
		const uint32_t nearest = (uint32_t)centre < from ? (uint32_t)centre : from - 1;
		return (Taps){ nearest, nearest, 0.0f };
	}
	const int32_t below = (int32_t)floorf(centre - 0.5f);
	const uint32_t first = below < 0 ? 0 : (uint32_t)below;
	const uint32_t second = below + 1 >= (int32_t)from ? from - 1 : (uint32_t)(below + 1);
	return (Taps){ first, second, centre - 0.5f - (float)below };
}

static float mix(float a, float b, float towards_b)
{
	return a + (b - a) * towards_b;
}

typedef struct {
	const V4xFormat *format;
	uint32_t from_width;
	uint32_t to_width;
	bool linear;
} BlitRow;

static void source_texel(const BlitRow *blit, uint32_t row, uint32_t x, float rgba[4])
{
	v4x_format_unpack(blit->format, &rows[row][x * blit->format->texel_bytes], rgba);
}

// A destination row from the source rows either side of it, each texel's channels unpacked, mixed and packed again.
static void blit_row(const BlitRow *blit, float towards_below)
{
	for (uint32_t x = 0; x < blit->to_width; x++) {
		const Taps across = taps(x, blit->to_width, blit->from_width, blit->linear);
		float corners[4][4];
		source_texel(blit, ROW_ABOVE, across.first, corners[0]);
		source_texel(blit, ROW_ABOVE, across.second, corners[1]);
		source_texel(blit, ROW_BELOW, across.first, corners[2]);
		source_texel(blit, ROW_BELOW, across.second, corners[3]);
		float mixed[4];
		for (uint32_t c = 0; c < 4; c++) {
			const float above = mix(corners[0][c], corners[1][c], across.towards_second);
			const float below = mix(corners[2][c], corners[3][c], across.towards_second);
			mixed[c] = mix(above, below, towards_below);
		}
		v4x_format_pack(blit->format, mixed, &rows[ROW_OUT][x * blit->format->texel_bytes]);
	}
}

// A destination layer's or 3D slice's level from the source's.
static void blit_slice(const Command *command, const BlitRow *blit, uint32_t from, uint32_t to)
{
	const Image *const source = command->image_copy.source;
	Image *const destination = command->image_copy.destination;
	const uint32_t from_level = command->image_copy.source_level;
	const uint32_t to_level = command->image_copy.destination_level;
	const uint32_t from_height = level_size(source->height, from_level);
	const uint32_t to_height = level_size(destination->height, to_level);
	for (uint32_t y = 0; y < to_height; y++) {
		const Taps down = taps(y, to_height, from_height, blit->linear);
		xsdk_texture_download_slice_rows(&source->texture, from_level, from, down.first, 1, rows[ROW_ABOVE], 0);
		xsdk_texture_download_slice_rows(&source->texture, from_level, from, down.second, 1, rows[ROW_BELOW], 0);
		blit_row(blit, down.towards_second);
		xsdk_texture_upload_slice_rows(&destination->texture, to_level, to, y, 1, rows[ROW_OUT], 0);
	}
}

// Images of formats the CPU unpacks (vkCmdBlitImage), a destination row at a time, each layer or 3D slice into its own.
void v4x_transfer_blit(VkQueue queue, const Command *command)
{
	const Image *const source = command->image_copy.source;
	Image *const destination = command->image_copy.destination;
	const uint32_t from_level = command->image_copy.source_level;
	const uint32_t to_level = command->image_copy.destination_level;
	const BlitRow blit = { .format = v4x_format(source->format),
		.from_width = level_size(source->width, from_level),
		.to_width = level_size(destination->width, to_level),
		.linear = command->image_copy.linear };
	wait_for_image(queue, source);
	wait_for_image(queue, destination);
	for (uint32_t i = 0; i < command->image_copy.slice_count; i++)
		blit_slice(command, &blit, command->image_copy.source_slice + i, command->image_copy.destination_slice + i);
	image_written(queue, destination);
}

// Each level takes the one row of the texel over and over.
void v4x_transfer_clear(VkQueue queue, const Command *command)
{
	Image *const image = command->clear.image;
	const uint32_t texel_bytes = v4x_texel_bytes(image->format);
	const uint32_t width = level_size(image->width, command->clear.first_level);
	for (uint32_t i = 0; i < width * texel_bytes; i += texel_bytes)
		memcpy(&rows[ROW_OUT][i], command->clear.texel, texel_bytes);
	wait_for_image(queue, image);
	const uint32_t end = command->clear.first_level + command->clear.level_count;
	const uint32_t slices_end = command->clear.first_slice + command->clear.slice_count;
	for (uint32_t level = command->clear.first_level; level < end; level++) {
		for (uint32_t slice = command->clear.first_slice; slice < slices_end; slice++)
			xsdk_texture_upload_slice_rows(
				&image->texture, level, slice, 0, level_size(image->height, level), rows[ROW_OUT], 0);
	}
	image_written(queue, image);
}

// memcpy into write-combined memory is safe, as the toolchain's stores bytes or aligned words (docs/SUPPORT.md).
void v4x_transfer_copy_buffer(VkQueue queue, const Command *command, volatile const void *source)
{
	wait_for_draws(queue);
	memcpy((void *)(uintptr_t)command->buffer_copy.destination, (const void *)(uintptr_t)source,
		command->buffer_copy.size_bytes);
	buffer_written(command->buffer_copy.address, command->buffer_copy.size_bytes);
}

void v4x_transfer_fill_buffer(VkQueue queue, const Command *command)
{
	wait_for_draws(queue);
	volatile uint32_t *const words = (volatile uint32_t *)(uintptr_t)command->buffer_copy.destination;
	for (uint32_t i = 0; i < command->buffer_copy.size_bytes / 4; i++)
		words[i] = command->buffer_copy.data;
	buffer_written(command->buffer_copy.address, command->buffer_copy.size_bytes);
}

// One word for every texel, so the texture's tiled order doesn't matter.
void v4x_transfer_fill_depth(VkQueue queue, Image *image, float depth, uint8_t stencil)
{
	const uint32_t word = (uint32_t)lrintf(depth * DEPTH_MAX) << 8 | stencil;
	wait_for_image(queue, image);
	volatile uint32_t *const texels = image->texture.texels;
	for (uint32_t i = 0; i < image->texture.size_bytes / 4; i++)
		texels[i] = word;
	image_written(queue, image);
}

// The CPU marks the slots unwritten, so the GPU must be done with what wrote them.
void v4x_transfer_reset_queries(VkQueue queue, const Command *command)
{
	QueryPool *const pool = command->queries.pool;
	wait_for_serial(queue, pool->used_serial, "queries for a reset");
	for (uint32_t i = 0; i < command->queries.count; i++) {
		volatile uint32_t *const slot = &pool->slots[(command->queries.first + i) * V4X_QUERY_SLOT_WORDS];
		for (uint32_t word = 0; word < XSDK_GPU_CLOCK_WORDS; word++)
			slot[word] = V4X_QUERY_UNWRITTEN;
	}
	__asm__ volatile("sync" ::: "memory");
}

// A result's value or availability, the index-th of a query's, at 32 or 64 bits, most significant word first.
static void put_result(volatile uint32_t *out, uint32_t index, uint64_t value, bool wide)
{
	if (wide) {
		out[2 * index] = (uint32_t)(value >> 32);
		out[2 * index + 1] = (uint32_t)value;
	} else {
		out[index] = (uint32_t)value;
	}
}

// The GPU must be done writing the queries, and with the draws before, which may read the buffer; then every query
// written before is available, so waiting adds nothing.
void v4x_transfer_copy_queries(VkQueue queue, const QueryCopy *copy)
{
	wait_for_serial(queue, copy->pool->used_serial, "queries for a copy");
	wait_for_draws(queue);
	const bool wide = copy->flags & VK_QUERY_RESULT_64_BIT;
	for (uint32_t i = 0; i < copy->count; i++) {
		volatile uint32_t *const out = (volatile uint32_t *)(copy->destination + i * copy->stride_bytes);
		uint64_t value = 0;
		const bool available = v4x_query_value(copy->pool, copy->first + i, &value);
		if (available || copy->flags & VK_QUERY_RESULT_PARTIAL_BIT)
			put_result(out, 0, value, wide);
		if (copy->flags & VK_QUERY_RESULT_WITH_AVAILABILITY_BIT)
			put_result(out, 1, available, wide);
	}
	buffer_written(copy->address, copy->count * copy->stride_bytes);
}

// The app's indices as the GPU takes them, into indices. Mapped memory is write-combined, so where they're aligned
// they're read 16 bytes a load: 80 MB/s, against 21 a word at a time (xsdk:docs/HARDWARE.md, "Instancing on the
// console, milestone 47 step 1").
static void read_indices(volatile const uint8_t *from, bool is_32_bit, uint32_t count)
{
	const uint32_t index_bytes = is_32_bit ? 4 : 2;
	const uint32_t per_quad = QUAD_BYTES / index_bytes;
	uint32_t quad[QUAD_BYTES / 4] __attribute__((aligned(16)));
	for (uint32_t i = 0; i < count;) {
		volatile const uint8_t *const at = from + i * index_bytes;
		if ((uintptr_t)at % QUAD_BYTES || count - i < per_quad) {
			indices[i++] = (is_32_bit ? *(volatile const uint32_t *)at : *(volatile const uint16_t *)at) & INDEX_MASK;
			continue;
		}
		vec_st(vec_ld(0, (const vector unsigned int *)(uintptr_t)at), 0, (vector unsigned int *)quad);
		// 16-bit indices lie two to a word, the first in its high half, as the CPU stores them.
		for (uint32_t j = 0; j < per_quad; j++)
			indices[i++] = (is_32_bit ? quad[j] : quad[j / 2] >> (j % 2 ? 0 : 16) & 0xFFFF) & INDEX_MASK;
	}
}

// Room for the copies in the part being filled, else in the next once the GPU is done with what the submission that
// last filled it drew; NULL if that's this one, which can't wait for itself inside a tiled pass's recording, or if
// the memory, taken at the first such draw so other apps don't pay for it, can't be had.
static volatile uint32_t *copy_room(VkQueue queue, uint32_t size_bytes)
{
	if (!queue->index_copies)
		queue->index_copies = xsdk_gpu_alloc(V4X_INDEX_COPY_PARTS * V4X_INDEX_COPY_PART_BYTES);
	if (!queue->index_copies)
		return NULL;
	if (queue->copy_part_bytes + size_bytes > V4X_INDEX_COPY_PART_BYTES) {
		const uint32_t next = (queue->copy_part + 1) % V4X_INDEX_COPY_PARTS;
		if (queue->copy_serials[next] == queue->serial)
			return NULL;
		wait_for_serial(queue, queue->copy_serials[next], "repeated indices");
		queue->copy_part = next;
		queue->copy_part_bytes = 0;
	}
	queue->copy_serials[queue->copy_part] = queue->serial;
	volatile uint32_t *const room =
		queue->index_copies + (queue->copy_part * V4X_INDEX_COPY_PART_BYTES + queue->copy_part_bytes) / 4;
	queue->copy_part_bytes += size_bytes;
	return room;
}

bool v4x_transfer_repeat_indices(VkQueue queue, volatile const uint8_t *from, bool is_32_bit, uint32_t count,
	uint32_t instance_count, XsdkRepeatedIndices *repeated)
{
	read_indices(from, is_32_bit, count);
	const uint32_t copies = instance_count < INDEX_COPIES ? instance_count : INDEX_COPIES;
	const uint32_t size_bytes = xsdk_gpu_repeated_index_bytes(indices, count, copies);
	volatile uint32_t *const room = copy_room(queue, size_bytes);
	if (!room)
		return false;
	xsdk_gpu_repeat_indices(repeated, room, indices, count, copies);
	// The GPU's caches may hold what the part held before (xsdk:include/xsdk/gpu.h, xsdk_gpu_repeat_indices).
	xsdk_gpu_invalidate(repeated->address, size_bytes);
	return true;
}
