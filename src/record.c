#include "record.h"

#include <altivec.h>
#include <assert.h>
#include <math.h>
#include <string.h>

#include <vulkan/vulkan.h>

#include "host.h"
#include "objects.h"
#include "pipeline_state.h"
#include "profile.h"
#include "transfer.h"
#include "xsdk/gpu.h"
#include "xsdk/log.h"
#include "xsdk/xsc.h"

// The draws' work the replay did at vkQueueSubmit, done as they're recorded, through xsdk's builders into the command
// buffer's packets (docs/COMMAND_BUFFERS.md). What a run writes rests only on what it wrote itself: at its start the
// GPU may hold anything, as the commands between runs, and other command buffers, set state of their own.

enum {
	// The GPU loads constants from 64-byte aligned memory (xsdk:include/xsdk/gpu.h), in groups (V4X_CONSTANT_GROUP).
	LOAD_ALIGNMENT_BYTES = 64,
	GROUP = V4X_CONSTANT_GROUP,
	// A one-dword packet that does nothing, which xsdk pads with (libxenon:drivers/xenos/xe.c:1217-1221).
	TYPE2_NOP = 0x80000000,
	// The most a piece holds: xsdk_gpu_reserve's most at once (xsdk:src/gpu.c, OVERFLOW_SCRATCH_DWORDS), less the
	// padding that keeps its 16-byte phase (copy_piece).
	PIECE_MOST_DWORDS = 8192 - 3,
	// A piece is cut at the first draw past this. A reservation that doesn't fit what's left of the stream's 64 KB
	// block pads the rest of it (xsdk:src/gpu.c, next_block), so smaller pieces waste less: vkbench's draws_4000 took
	// 32.6 dwords a draw at 4,096 and 26.6 at 1,024, for the 25.1 its draws write (in Xenia, smoke only).
	PIECE_DWORDS = 1024,
	// One register's float constants, alignment and header with its group (xsdk_gpu_float_constant_dwords).
	REGISTER_DWORDS = 3 + 1 + 4 * GROUP,
	// What a draw's next piece or instance writes at most: its streams, its vertex base and instance group, its draw.
	PIECE_STEP_DWORDS =
		XSDK_VERTEX_STREAMS * XSDK_GPU_VERTICES_DWORDS + 2 * REGISTER_DWORDS + XSDK_GPU_DRAW_INDEXED_DWORDS,
	FIRST_LIST_ITEMS = 64,
};

// A block's registers before the submit reads them in.
static const float zeros[V4X_FLOAT_CONSTANTS * 4] __attribute__((aligned(16)));

// A list's room doubled till it holds needed items; NULL with the error kept where there's no memory.
static __attribute__((noinline)) void *grow_list(
	Recorder *r, void *items, uint32_t *capacity, uint32_t needed, size_t item_bytes)
{
	uint32_t grown = *capacity ? *capacity : FIRST_LIST_ITEMS;
	while (grown < needed)
		grown *= 2;
	void *const resized = v4x_host_realloc(r->allocator, items, grown * item_bytes, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
	if (!resized) {
		r->result = VK_ERROR_OUT_OF_HOST_MEMORY;
		return NULL;
	}
	*capacity = grown;
	return resized;
}

// The packets' room doubled, 16-byte aligned, for the copy's VMX loads and the constants' VMX stores (copy_piece).
static __attribute__((noinline)) bool grow_packets(Recorder *r, uint32_t needed)
{
	uint32_t capacity = r->packets.capacity ? r->packets.capacity : V4X_FIRST_PACKET_DWORDS;
	while (capacity - r->packets.count < needed)
		capacity *= 2;
	void *const memory = v4x_host_alloc(r->allocator, capacity * 4 + 16, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
	if (!memory) {
		r->result = VK_ERROR_OUT_OF_HOST_MEMORY;
		return false;
	}
	uint32_t *const dwords = (uint32_t *)(((uintptr_t)memory + 15) & ~(uintptr_t)15);
	if (r->packets.count)
		memcpy(dwords, (const void *)(uintptr_t)r->packets.dwords, r->packets.count * 4);
	v4x_host_free(r->allocator, r->packet_memory);
	r->packet_memory = memory;
	r->packets.dwords = dwords;
	r->packets.capacity = capacity;
	return true;
}

static void cut(Recorder *r)
{
	if (r->cut_count == r->cut_capacity) {
		uint32_t *const grown = grow_list(r, r->cuts, &r->cut_capacity, r->cut_count + 1, sizeof *r->cuts);
		if (!grown)
			return;
		r->cuts = grown;
	}
	r->cuts[r->cut_count++] = r->packets.count;
	r->piece_first = r->packets.count;
}

// Room for dwords of packets, in the piece being recorded, else in a piece of their own.
static inline bool make_room(Recorder *r, uint32_t dwords)
{
	if (r->packets.count - r->piece_first + dwords > PIECE_MOST_DWORDS)
		cut(r);
	return r->packets.capacity - r->packets.count >= dwords || grow_packets(r, dwords);
}

static inline void draw_done(Recorder *r)
{
	if (r->packets.count - r->piece_first >= PIECE_DWORDS)
		cut(r);
}

static void add_site(Recorder *r, volatile uint32_t *at, bool depth, uint32_t depth_control, bool pixel_writes_depth)
{
	if (r->site_count == r->site_capacity) {
		HizSite *const grown = grow_list(r, r->sites, &r->site_capacity, r->site_count + 1, sizeof *r->sites);
		if (!grown)
			return;
		r->sites = grown;
	}
	r->sites[r->site_count++] = (HizSite){ .offset = (uint32_t)(at - r->packets.dwords),
		.depth_control = depth_control,
		.depth = depth,
		.pixel_writes_depth = pixel_writes_depth };
}

static void add_patch(Recorder *r, const ReadPatch *patch)
{
	if (r->patch_count == r->patch_capacity) {
		ReadPatch *const grown = grow_list(r, r->patches, &r->patch_capacity, r->patch_count + 1, sizeof *r->patches);
		if (!grown)
			return;
		r->patches = grown;
	}
	r->patches[r->patch_count++] = *patch;
}

static void add_image(Recorder *r, Image *image)
{
	if (r->image_count == r->image_capacity) {
		Image **const grown = grow_list(r, r->images, &r->image_capacity, r->image_count + 1, sizeof *r->images);
		if (!grown)
			return;
		r->images = grown;
	}
	r->images[r->image_count++] = image;
}

static void begin_run(Recorder *r)
{
	r->run = (Run){ .first = r->packets.count,
		.first_cut = r->cut_count,
		.first_site = r->site_count,
		.first_patch = r->patch_count,
		.first_image = r->image_count };
	r->piece_first = r->packets.count;
}

// Nothing known of what the GPU holds, but the boolean words, which nothing the layer runs sets.
static void forget(Recorder *r)
{
	r->pipeline_dirty = true;
	r->viewport_dirty = false;
	r->dynamic_dirty = false;
	memset(r->known, 0, sizeof r->known);
	memset(r->loaded, 0, sizeof r->loaded);
	memset(r->read_groups, 0, sizeof r->read_groups);
	r->loops_known = false;
	memset(r->pointed_streams, 0, sizeof r->pointed_streams);
	memset(r->pointed_textures, 0, sizeof r->pointed_textures);
	r->clip = (XsdkClipControl){ .value = XSDK_GPU_CLIP_CONTROL_TARGET, .stale = true };
	r->clip_planes_on = true;
	r->loaded_vertex = 0;
	r->loaded_pixel = 0;
	r->pixel_writes_depth = true;
}

Recorder *v4x_record_new(const VkAllocationCallbacks *allocator)
{
	void *const memory = v4x_host_alloc(allocator, sizeof(Recorder) + 15, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
	if (!memory)
		return NULL;
	Recorder *const r = (Recorder *)(((uintptr_t)memory + 15) & ~(uintptr_t)15);
	r->memory = memory;
	r->allocator = allocator;
	if (!grow_packets(r, V4X_FIRST_PACKET_DWORDS)) {
		v4x_host_free(allocator, memory);
		return NULL;
	}
	v4x_record_reset(r);
	return r;
}

void v4x_record_delete(Recorder *r)
{
	if (!r)
		return;
	v4x_host_free(r->allocator, r->packet_memory);
	v4x_host_free(r->allocator, r->runs);
	v4x_host_free(r->allocator, r->cuts);
	v4x_host_free(r->allocator, r->sites);
	v4x_host_free(r->allocator, r->patches);
	v4x_host_free(r->allocator, r->images);
	v4x_host_free(r->allocator, r->memory);
}

void v4x_record_reset(Recorder *r)
{
	r->result = VK_SUCCESS;
	r->packets.count = 0;
	r->run_count = 0;
	r->cut_count = 0;
	r->site_count = 0;
	r->patch_count = 0;
	r->image_count = 0;
	begin_run(r);
	forget(r);
}

bool v4x_record_close(Recorder *r, uint32_t *index)
{
	bool closed = r->packets.count > r->run.first;
	if (closed && r->run_count == r->run_capacity) {
		Run *const grown = grow_list(r, r->runs, &r->run_capacity, r->run_count + 1, sizeof *r->runs);
		closed = grown != NULL;
		if (grown)
			r->runs = grown;
	}
	if (closed) {
		Run *const run = &r->runs[r->run_count];
		*run = r->run;
		run->end = r->packets.count;
		run->end_cut = r->cut_count;
		run->end_site = r->site_count;
		run->end_patch = r->patch_count;
		run->end_image = r->image_count;
		*index = r->run_count++;
	}
	begin_run(r);
	forget(r);
	return closed;
}

// A register's four floats from anywhere 4-byte aligned, as bits, so values compare equal only when they're the same.
static inline vector unsigned int register_bits(const float *values)
{
	return (vector unsigned int)vec_perm(vec_ld(0, values), vec_ld(15, values), vec_lvsl(0, values));
}

// Whether count registers from first each hold their value already.
static inline bool holds(const Recorder *r, uint32_t stage, uint32_t first, const float *values, uint32_t count)
{
	for (uint32_t g = first / GROUP; g <= (first + count - 1) / GROUP; g++) {
		if (r->loaded[stage][g])
			return false;
	}
	const uint8_t *const known = &r->known[stage][first];
	const float *const kept = &r->kept[stage][4 * first];
	for (uint32_t i = 0; i < count; i++) {
		if (!known[i] || !vec_all_eq(register_bits(&values[4 * i]), (vector unsigned int)vec_ld(0, &kept[4 * i])))
			return false;
	}
	return true;
}

// Writes count registers from first, and the rest of each group they touch from the kept values, as xsdk writes whole
// groups (xsdk:include/xsdk/gpu.h); where one of the rest holds a uniform buffer's words the submit reads, it reads
// them in there too. read, where not NULL, is a block's words from first, present of them in the descriptor's range,
// which the submit reads in for values. Groups loaded from memory hold what's written after.
static __attribute__((noinline)) void put_floats(Recorder *r, uint32_t stage, uint32_t first, const float *values,
	uint32_t count, volatile const uint32_t *read, uint32_t present)
{
	volatile uint32_t *const at = stage ? xsdk_gpu_put_pixel_constants(&r->packets, r->kept[1], first, values, count)
										: xsdk_gpu_put_vertex_constants(&r->packets, r->kept[0], first, values, count);
	// Register reg's values are 4 dwords a register from register first's.
	const uint32_t first_offset = (uint32_t)(at - r->packets.dwords);
	const uint32_t end = first + count;
	for (uint32_t g = first / GROUP; g <= (end - 1) / GROUP; g++) {
		r->loaded[stage][g] = 0;
		uint32_t bits = r->read_groups[stage][g];
		if (!bits && !read)
			continue;
		for (uint32_t reg = g * GROUP; reg < (g + 1) * GROUP; reg++) {
			const uint32_t bit = 1u << (reg % GROUP);
			const bool written = reg >= first && reg < end;
			if (reg == first && read) {
				add_patch(r,
					&(ReadPatch){ .offset = first_offset,
						.words = read,
						.present = present < 4 * count ? present : 4 * count,
						.count = 4 * count,
						.kind = PATCH_WORDS });
			}
			if (written && read) {
				const uint32_t skipped = 4 * (reg - first);
				r->read_words[stage][reg] = read + skipped;
				r->read_present[stage][reg] = present > skipped ? (present - skipped < 4 ? present - skipped : 4) : 0;
				bits |= bit;
			} else if (written) {
				bits &= ~bit;
			} else if (bits & bit) {
				add_patch(r,
					&(ReadPatch){ .offset = first_offset + 4 * reg - 4 * first,
						.words = r->read_words[stage][reg],
						.present = r->read_present[stage][reg],
						.count = 4,
						.kind = PATCH_WORDS });
			}
		}
		r->read_groups[stage][g] = (uint8_t)bits;
	}
	memset(&r->known[stage][first], read ? 0 : 1, count);
}

// Sets count registers from first unless each holds its value already.
static inline void write_constants(Recorder *r, bool vertex, uint32_t first, const float *values, uint32_t count)
{
	assert(count);
	const uint32_t stage = vertex ? 0 : 1;
	if (!holds(r, stage, first, values, count))
		put_floats(r, stage, first, values, count, NULL, 0);
}

// Has the GPU load count registers from first, whole groups, from memory at address, unless each group holds that
// memory already: nothing the GPU runs writes memory, and what changes it between passes is a command, before which a
// run ends.
static void load_constants(Recorder *r, bool vertex, uint32_t first, uint32_t address, uint32_t count)
{
	const uint32_t stage = vertex ? 0 : 1;
	uint32_t *const loaded = &r->loaded[stage][first / GROUP];
	const uint32_t groups = count / GROUP;
	uint32_t same = 0;
	while (same < groups && loaded[same] == ((address + same * LOAD_ALIGNMENT_BYTES) | 1))
		same++;
	if (same == groups)
		return;
	v4x_profile_count(V4X_PROFILE_BLOCKS_LOADED, 1);
	if (vertex)
		xsdk_gpu_put_load_vertex_constants(&r->packets, first, address, count);
	else
		xsdk_gpu_put_load_pixel_constants(&r->packets, first, address, count);
	for (uint32_t g = 0; g < groups; g++) {
		loaded[g] = (address + g * LOAD_ALIGNMENT_BYTES) | 1;
		r->read_groups[stage][first / GROUP + g] = 0;
	}
}

// A register xsdk's builder set itself, an instanced draw's group, holds what the recorder doesn't know; xost puts that
// group past the blocks (xost:docs/xsc.md, "Interface conventions"), so no block read at the submit shares it.
static void lose_constant(Recorder *r, uint32_t reg)
{
	r->known[0][reg] = 0;
	r->loaded[0][reg / GROUP] = 0;
	if (r->read_groups[0][reg / GROUP]) {
		static bool logged;
		if (!logged)
			(void)v4x_host_unsupported("an instance group sharing its group of 4 with a uniform block read on the CPU");
		logged = true;
	}
}

// A shader's literals, each run of them in registers one after another set as one, as xsdk writes whole groups.
static void write_literals(Recorder *r, const V4xShader *shader, bool vertex)
{
	v4x_profile_count(V4X_PROFILE_LITERALS, shader->literal_count);
	uint32_t i = 0;
	while (i < shader->literal_count) {
		const uint32_t first = shader->literals[i].reg;
		uint32_t count = 0;
		do {
			memcpy(&r->block[4 * count], shader->literals[i].values, sizeof shader->literals[i].values);
			count++;
			i++;
		} while (i < shader->literal_count && shader->literals[i].reg == first + count);
		write_constants(r, vertex, first, r->block, count);
	}
}

// The first pixel whose centre a viewport edge at coordinate covers, or the one past the last.
static int32_t first_pixel(float coordinate)
{
	return (int32_t)ceilf(coordinate - 0.5f);
}

static int32_t larger(int32_t a, int32_t b)
{
	return a > b ? a : b;
}

static int32_t smaller(int32_t a, int32_t b)
{
	return a < b ? a : b;
}

// Whether a draw writes a state: the run hasn't written it, or the value differs from what it last wrote, which then
// takes it. Values the same bit for bit write the same registers.
static bool changed(Recorder *r, void *applied, const void *value, size_t size_bytes)
{
	if (!r->pipeline_dirty && !memcmp(applied, value, size_bytes))
		return false;
	memcpy(applied, value, size_bytes);
	return true;
}

// Vulkan's y points down and its pixel centres sit on halves; xsdk's viewport is y-up with centres on whole numbers.
// Vulkan clips to the viewport, and Xenos draws past it as far as the scissor (docs/SUPPORT.md, Pipelines), so the
// scissor stops at the viewport's pixels too.
static void apply_viewport(Recorder *r, const Bound *b, const Pipeline *pipeline)
{
	const VkViewport *const v = pipeline->dynamic_viewport ? &b->viewport : &pipeline->viewport;
	const VkRect2D *const s = pipeline->dynamic_scissor ? &b->scissor : &pipeline->scissor;
	const bool viewport_changed = changed(r, &r->applied.viewport, v, sizeof *v);
	if (viewport_changed)
		xsdk_gpu_put_viewport(
			&r->packets, v->x - 0.5f, v->y + v->height - 0.5f, v->width, -v->height, v->minDepth, v->maxDepth);
	if (!changed(r, &r->applied.scissor, s, sizeof *s) && !viewport_changed)
		return;
	const int32_t left = larger(s->offset.x, first_pixel(v->x));
	const int32_t top = larger(s->offset.y, first_pixel(v->y));
	const int32_t right = smaller(s->offset.x + (int32_t)s->extent.width, first_pixel(v->x + v->width));
	const int32_t bottom = smaller(s->offset.y + (int32_t)s->extent.height, first_pixel(v->y + v->height));
	xsdk_gpu_put_scissor(&r->packets, (uint32_t)left, (uint32_t)top, right > left ? (uint32_t)(right - left) : 0,
		bottom > top ? (uint32_t)(bottom - top) : 0);
}

// The state blocks dynamic state may change: the pipeline's own, or worked out with what the commands set. The blend
// constants aren't compared, as few pipelines take them. The depth and stencil's RB_HIZCONTROL is the submit's to work
// out (v4x_record_copy).
static void apply_dynamic(Recorder *r, const Bound *b, const Pipeline *pipeline)
{
	XsdkRasterState raster_scratch;
	XsdkDepthStencilState depth_stencil_scratch;
	const XsdkRasterState *const raster = v4x_pipeline_raster(pipeline, &b->dynamic, &raster_scratch);
	if (changed(r, &r->applied.raster, raster, sizeof *raster))
		xsdk_gpu_put_raster(&r->packets, raster, b->samples, &r->clip);
	const XsdkDepthStencilState *const depth_stencil =
		v4x_pipeline_depth_stencil(pipeline, &b->dynamic, &depth_stencil_scratch);
	if (changed(r, &r->applied.depth_stencil, depth_stencil, sizeof *depth_stencil))
		add_site(
			r, xsdk_gpu_put_depth_stencil(&r->packets, depth_stencil, 0), true, depth_stencil->depth_control, false);
	const float *const constants = v4x_pipeline_blend_constants(pipeline, &b->dynamic);
	if (constants)
		xsdk_gpu_put_blend_constants(&r->packets, constants);
}

// Each shader only if it isn't the one loaded, as xsdk_gpu_set_shaders loads them, and the hierarchical buffer's
// control after them where it puts one: a pixel shader loaded that writes depth, or after one that may have.
static void put_shaders(Recorder *r, const Pipeline *pipeline)
{
	const XsdkShader *const vertex = &pipeline->vertex;
	const XsdkShader *const pixel = &pipeline->pixel;
	const bool load_vertex = vertex->address != r->loaded_vertex;
	const bool load_pixel = pixel->address != r->loaded_pixel;
	if (!load_vertex && !load_pixel)
		return;
	const bool writes_depth = xsdk_gpu_writes_depth(pixel);
	const bool site = load_pixel && (writes_depth || r->pixel_writes_depth);
	r->pixel_writes_depth = writes_depth;
	xsdk_gpu_put_shaders(&r->packets, vertex, pixel, load_vertex, load_pixel);
	if (site)
		add_site(r, xsdk_gpu_put_hiz_control(&r->packets, 0), false, 0, writes_depth);
	r->loaded_vertex = vertex->address;
	r->loaded_pixel = pixel->address;
}

// Each pipeline has shaders of its own. A shader's literals stay in their registers while the pipelines drawing share
// that shader, as its blocks never reach them; another shader's may.
static __attribute__((noinline)) void apply_pipeline(Recorder *r, const Bound *b, const Pipeline *pipeline)
{
	AppliedState *const applied = &r->applied;
	v4x_profile_count(V4X_PROFILE_PIPELINES, 1);
	v4x_profile_count(V4X_PROFILE_PIPELINES_AGAIN, applied->pipeline == pipeline);
	applied->pipeline = pipeline;
	put_shaders(r, pipeline);
	if (changed(r, &applied->vertex_literals, &pipeline->vertex_source, sizeof pipeline->vertex_source))
		write_literals(r, pipeline->vertex_source, true);
	if (changed(r, &applied->pixel_literals, &pipeline->pixel_source, sizeof pipeline->pixel_source))
		write_literals(r, pipeline->pixel_source, false);
	apply_viewport(r, b, pipeline);
	const uint32_t count = pipeline->blend_count;
	const bool count_changed = changed(r, &applied->blend_count, &count, sizeof count);
	if (changed(r, applied->blends, pipeline->blends, count * sizeof *pipeline->blends) || count_changed)
		xsdk_gpu_put_blends(&r->packets, pipeline->blends, count);
	apply_dynamic(r, b, pipeline);
}

// A uniform buffer's words as the resource's set is bound, from its dynamic offset; where its descriptor's range
// starts, and the bytes the range covers. NULL where the set isn't bound.
static volatile const uint32_t *buffer_words(
	const Bound *b, const V4xShaderResource *resource, uint32_t *address, uint32_t *range_bytes)
{
	const DescriptorSet *const set = b->sets[resource->set];
	if (!set)
		return NULL;
	const Descriptor *const descriptor = &set->descriptors[resource->binding];
	const LayoutBinding *const binding = &set->layout->bindings[resource->binding];
	const uint32_t offset = binding->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC
		? b->dynamic_offsets[resource->set][binding->dynamic_index]
		: 0;
	*address = descriptor->buffer_address + offset;
	*range_bytes = descriptor->range_bytes;
	return descriptor->buffer + offset / 4;
}

// A uniform block the submit reads into its registers, zero past the descriptor's range.
static void read_block(Recorder *r, const StageResource *stage_resource, volatile const uint32_t *words, uint32_t range)
{
	const V4xShaderResource *const resource = &stage_resource->resource;
	v4x_profile_count(V4X_PROFILE_BLOCKS_READ, 1);
	v4x_profile_count(V4X_PROFILE_QUADS_READ, resource->count);
	put_floats(r, stage_resource->vertex ? 0 : 1, resource->first, zeros, resource->count, words, range / 4);
}

// Has the GPU load a block that starts a group (pipeline.c, plan_group) straight from the buffer, when the buffer is
// aligned and covers it, the padding past the block taking whatever follows it, which nothing reads; else the submit
// reads it.
static void block_from_buffer(Recorder *r, const Bound *b, const StageResource *stage_resource)
{
	const V4xShaderResource *const resource = &stage_resource->resource;
	uint32_t address;
	uint32_t range;
	volatile const uint32_t *const words = buffer_words(b, resource, &address, &range);
	if (!words)
		return;
	if (address % LOAD_ALIGNMENT_BYTES || range < resource->count * 16u || resource->first % GROUP) {
		read_block(r, stage_resource, words, range);
		return;
	}
	const uint32_t count = (resource->count + GROUP - 1) / GROUP * GROUP;
	load_constants(r, stage_resource->vertex, resource->first, address, count);
}

// Points a slot at the texture and sampler unless it already is; the submit marks the image as its submission's.
static void point_texture(Recorder *r, uint32_t slot, const Descriptor *descriptor)
{
	assert(slot < V4X_TEXTURE_SLOTS);
	PointedTexture *const pointed = &r->pointed_textures[slot];
	if (pointed->texture == descriptor->texture && pointed->sampler == descriptor->sampler)
		return;
	v4x_profile_count(V4X_PROFILE_TEXTURES, 1);
	xsdk_gpu_put_texture(&r->packets, slot, descriptor->texture, &descriptor->sampler->state);
	add_image(r, descriptor->image);
	*pointed = (PointedTexture){ .texture = descriptor->texture, .sampler = descriptor->sampler };
}

// A loop's count read where the submit reads it: its word in the block, where the descriptor's range reaches it.
typedef struct {
	uint32_t loop;
	volatile const uint32_t *word;
	int32_t start;
	int32_t step;
} LoopRead;

// The loop constants a pipeline's hardware loops read, each from its count, or its fixed passes for loops whose count
// is data; a loop counted in a uniform buffer from the count the submit reads. xsdk writes every boolean and loop word
// together (xsdk:include/xsdk/gpu.h), so they're written when one of the loops changes or is read.
static __attribute__((noinline)) void apply_loops(Recorder *r, const Bound *b, const Pipeline *pipeline)
{
	uint32_t values[XSDK_LOOP_CONSTANTS];
	LoopRead reads[V4X_MAX_LOOPS * 2];
	uint32_t read_count = 0;
	memcpy(values, &r->integer[XSDK_BOOL_CONSTANT_WORDS], sizeof values);
	for (uint32_t i = pipeline->plan[PLAN_LOOP]; i < pipeline->plan[PLAN_LOOP + 1]; i++) {
		const V4xShaderResource *const resource = &pipeline->resources[i].resource;
		uint32_t passes = 0;
		if (resource->kind == V4X_RESOURCE_FIXED_LOOP) {
			passes = resource->first;
		} else if (resource->kind == V4X_RESOURCE_PUSH_LOOP) {
			// An int's offset is a multiple of 4, so it lies in one quad.
			int32_t count;
			memcpy(&count, (const uint8_t *)b->push + resource->first, sizeof count);
			passes = xsdk_xsc_loop_passes(resource->start, resource->step, count);
		} else {
			uint32_t address;
			uint32_t range;
			volatile const uint32_t *const words = buffer_words(b, resource, &address, &range);
			if (words && resource->first + sizeof(int32_t) <= range && read_count < V4X_MAX_LOOPS * 2) {
				reads[read_count++] = (LoopRead){ .loop = resource->count,
					.word = words + resource->first / 4,
					.start = resource->start,
					.step = resource->step };
				continue;
			}
		}
		values[resource->count] = xsdk_gpu_loop_constant(passes, 0, 1);
	}
	if (!read_count && r->loops_known && !memcmp(values, &r->integer[XSDK_BOOL_CONSTANT_WORDS], sizeof values))
		return;
	volatile uint32_t *const at = xsdk_gpu_put_loop_constants(&r->packets, r->integer, 0, values, XSDK_LOOP_CONSTANTS);
	for (uint32_t i = 0; i < read_count; i++) {
		add_patch(r,
			&(ReadPatch){ .offset = (uint32_t)(at - r->packets.dwords) + reads[i].loop,
				.words = reads[i].word,
				.present = 1,
				.count = 1,
				.kind = PATCH_LOOP,
				.start = reads[i].start,
				.step = reads[i].step });
	}
	r->loops_known = !read_count;
}

// The draw plan's groups in turn (Pipeline, plan); what a set that isn't bound holds is left as it was.
static void apply_resources(Recorder *r, const Bound *b, const Pipeline *pipeline)
{
	const StageResource *const resources = pipeline->resources;
	const uint16_t *const plan = pipeline->plan;
	if (plan[PLAN_LOOP] < plan[PLAN_LOOP + 1])
		apply_loops(r, b, pipeline);
	for (uint32_t i = plan[PLAN_PUSH]; i < plan[PLAN_PUSH + 1]; i++) {
		const StageResource *const s = &resources[i];
		v4x_profile_count(V4X_PROFILE_QUADS_PUSHED, s->push_count);
		write_constants(r, s->vertex, s->resource.first + s->push_first, &b->push[4 * s->push_first], s->push_count);
	}
	for (uint32_t i = plan[PLAN_LOAD]; i < plan[PLAN_LOAD + 1]; i++)
		block_from_buffer(r, b, &resources[i]);
	for (uint32_t i = plan[PLAN_READ]; i < plan[PLAN_READ + 1]; i++) {
		uint32_t address;
		uint32_t range;
		volatile const uint32_t *const words = buffer_words(b, &resources[i].resource, &address, &range);
		if (words)
			read_block(r, &resources[i], words, range);
	}
	for (uint32_t i = plan[PLAN_COMPARISON]; i < plan[PLAN_COMPARISON + 1]; i++) {
		const V4xShaderResource *const resource = &resources[i].resource;
		const DescriptorSet *const set = b->sets[resource->set];
		if (set)
			write_constants(r, resources[i].vertex, resource->first,
				set->descriptors[resource->binding].sampler->compare_weights, 1);
	}
	for (uint32_t i = plan[PLAN_TEXTURE]; i < plan[PLAN_TEXTURE + 1]; i++) {
		const V4xShaderResource *const resource = &resources[i].resource;
		const DescriptorSet *const set = b->sets[resource->set];
		if (set)
			point_texture(r, resource->first, &set->descriptors[resource->binding]);
	}
}

// Xenos clips against the planes itself, so each draw of a pipeline with clip distances sets them from the values
// bound, which may have changed since, the submit reading those in uniform buffers; one without turns off what may be
// on. Past a descriptor's range a plane reads 0, as the block's registers do.
static void apply_clip_planes(Recorder *r, const Bound *b, const Pipeline *pipeline)
{
	if (!pipeline->clip_plane_count && !r->clip_planes_on)
		return;
	float planes[XSDK_CLIP_PLANES][4] = { { 0 } };
	ReadPatch reads[XSDK_CLIP_PLANES];
	uint32_t read_count = 0;
	for (uint32_t i = 0; i < pipeline->clip_plane_count; i++) {
		const ClipPlane *const clip = &pipeline->clip_planes[i];
		const V4xShaderResource *const resource = &pipeline->resources[clip->resource].resource;
		if (resource->kind == V4X_RESOURCE_PUSH_CONSTANTS) {
			memcpy(planes[clip->plane], &b->push[clip->offset_bytes / 4], sizeof planes[clip->plane]);
			continue;
		}
		uint32_t address;
		uint32_t range;
		volatile const uint32_t *const words = buffer_words(b, resource, &address, &range);
		const uint32_t skipped = clip->offset_bytes / 4;
		if (words) {
			reads[read_count++] = (ReadPatch){ .offset = 4 * clip->plane,
				.words = words + skipped,
				.present = range / 4 > skipped ? range / 4 - skipped : 0,
				.count = 4,
				.kind = PATCH_WORDS };
		}
	}
	volatile uint32_t *const at = xsdk_gpu_put_clip_planes(&r->packets, pipeline->clip_plane_mask, planes, &r->clip);
	for (uint32_t i = 0; i < read_count; i++) {
		reads[i].offset += (uint32_t)(at - r->packets.dwords);
		reads[i].present = reads[i].present < 4 ? reads[i].present : 4;
		add_patch(r, &reads[i]);
	}
	r->clip_planes_on = pipeline->clip_plane_count != 0;
}

// Whether the draw can be written: room for its state, a pipeline bound in a render pass.
static bool prepare_draw(Recorder *r, const Bound *b)
{
	const Pipeline *const pipeline = b->pipeline;
	if (!make_room(r, pipeline->draw_dwords))
		return false;
	r->run.draws++;
	if (r->pipeline_dirty || pipeline != r->applied.pipeline) {
		apply_pipeline(r, b, pipeline);
	} else {
		if (r->viewport_dirty)
			apply_viewport(r, b, pipeline);
		if (r->dynamic_dirty)
			apply_dynamic(r, b, pipeline);
	}
	r->pipeline_dirty = false;
	r->viewport_dirty = false;
	r->dynamic_dirty = false;
	apply_resources(r, b, pipeline);
	apply_clip_planes(r, b, pipeline);
	return true;
}

// xsdk draws from vertex 0, so each stream at the vertex's rate starts at the draw's first vertex, which may come
// before the binding's; one at the instance's rate stays at the binding's start, the shader fetching each instance's
// element (pipeline.c). A vertex shader reading gl_VertexIndex adds the first vertex back to the GPU's index as its
// vertex base (xost:docs/xsc.md, "Interface conventions"); a float holds it exactly up to 2^24.
static void point_streams(Recorder *r, const Bound *b, int32_t first_vertex)
{
	const Pipeline *const pipeline = b->pipeline;
	if (pipeline->reads_vertex_index) {
		const float base[4] = { (float)first_vertex, 0.0f, 0.0f, 0.0f };
		write_constants(r, true, pipeline->vertex_base, base, 1);
	}
	for (uint32_t s = 0; s < pipeline->stream_count; s++) {
		const VertexStream *const source = &pipeline->streams[s];
		const VertexBinding *const binding = &b->bindings[source->binding];
		const uint32_t skipped = source->per_instance ? 0 : (uint32_t)(first_vertex * (int32_t)source->stride_bytes);
		const VertexBinding stream = { .address = binding->address + skipped,
			.size_bytes = binding->size_bytes - skipped };
		if (r->pointed_streams[s].address == stream.address && r->pointed_streams[s].size_bytes == stream.size_bytes)
			continue;
		v4x_profile_count(V4X_PROFILE_STREAMS, 1);
		xsdk_gpu_put_vertices(&r->packets, r->groups, s, stream.address, stream.size_bytes);
		r->pointed_streams[s] = stream;
	}
}

// An instanced vertex shader's group for a draw of one instance: 1/S, 0.5/S and S 0, so its prologue keeps the vertex's
// index and takes the instance from w (xost:docs/xsc.md, "Interface conventions"); a float holds it exactly up to 2^24.
static void write_instance(Recorder *r, const Pipeline *pipeline, uint32_t instance)
{
	if (!pipeline->vertex.instanced)
		return;
	const float values[4] = { 0.0f, 0.0f, 0.0f, (float)instance };
	write_constants(r, true, pipeline->vertex.instance_constant, values, 1);
}

// One instance, in the pieces its pipeline splits a draw into (Pipeline, piece_vertices), each with room for its
// instance group, streams and draw.
static void draw_instance(Recorder *r, const Bound *b, uint32_t vertex_count, uint32_t first_vertex, uint32_t instance)
{
	const Pipeline *const pipeline = b->pipeline;
	if (!make_room(r, PIECE_STEP_DWORDS))
		return;
	write_instance(r, pipeline, instance);
	for (uint32_t start = 0;; start += pipeline->piece_step) {
		if (start && !make_room(r, PIECE_STEP_DWORDS))
			return;
		point_streams(r, b, (int32_t)(first_vertex + start));
		const uint32_t left = vertex_count - start;
		xsdk_gpu_put_draw(
			&r->packets, pipeline->primitive, left < pipeline->piece_vertices ? left : pipeline->piece_vertices);
		if (left <= pipeline->piece_vertices)
			break;
	}
}

static void draw_indexed_instance(
	Recorder *r, const Bound *b, uint32_t count, uint32_t first_index, int32_t vertex_offset, uint32_t instance)
{
	const Pipeline *const pipeline = b->pipeline;
	if (!make_room(r, PIECE_STEP_DWORDS))
		return;
	write_instance(r, pipeline, instance);
	point_streams(r, b, vertex_offset);
	const uint32_t index_bytes = b->index_32_bit ? 4 : 2;
	for (uint32_t start = 0;; start += pipeline->piece_step) {
		if (start && !make_room(r, PIECE_STEP_DWORDS))
			return;
		const uint32_t address = b->index_address + (first_index + start) * index_bytes;
		const uint32_t left = count - start;
		const uint32_t indices = left < pipeline->piece_vertices ? left : pipeline->piece_vertices;
		xsdk_gpu_put_draw_indexed(&r->packets, pipeline->primitive, address, indices, b->index_32_bit);
		if (left <= pipeline->piece_vertices)
			break;
	}
}

// What a draw refuses as it's recorded, an indirect one as the submit draws it: no pipeline bound outside a render
// pass, and a fan past what a draw packet carries.
static bool drawable(const Bound *b, uint32_t count, uint32_t instances)
{
	if (!count || !instances)
		return false;
	if (!b->pipeline || !b->samples) {
		static bool logged;
		if (!logged)
			xsdk_log_write("vulkan4xenon: a draw without a pipeline bound, or outside a render pass, draws nothing");
		logged = true;
		return false;
	}
	if (count > V4X_MAX_DRAW_VERTICES && !b->pipeline->piece_step) {
		(void)v4x_host_unsupported("triangle fans of more than 65535 vertices");
		return false;
	}
	return true;
}

// An instanced pipeline's list draws its instances together, each count vertices or indices taken down to whole
// primitives, as Vulkan draws no partial one; 0 where they go an instance at a time: a pipeline that isn't instanced, a
// strip, a fan, or an instance past what a draw packet holds (docs/SUPPORT.md, Commands).
static uint32_t instanced_list(const Pipeline *pipeline, uint32_t count)
{
	if (!pipeline->vertex.instanced || !pipeline->list_vertices || count > V4X_MAX_DRAW_VERTICES)
		return 0;
	return count - count % pipeline->list_vertices;
}

// A draw of one instance in one piece under a vertex shader that isn't instanced: drawn as it is, with no instance
// group, copies or pieces.
static bool plain(const Pipeline *pipeline, uint32_t count, uint32_t instances)
{
	return instances == 1 && count <= pipeline->piece_vertices && !pipeline->vertex.instanced;
}

// Instances or pieces, kept out of the draw so a plain draw saves no more registers than it uses.
static __attribute__((noinline)) void draw_instances(Recorder *r, const Bound *b, uint32_t vertex_count,
	uint32_t first_vertex, uint32_t instance_count, uint32_t first_instance)
{
	const Pipeline *const pipeline = b->pipeline;
	if (!prepare_draw(r, b))
		return;
	const uint32_t whole = instanced_list(pipeline, vertex_count);
	// Together in pieces that fit a piece of the run, else an instance at a time.
	const uint32_t dwords = whole ? xsdk_gpu_draw_instanced_dwords(pipeline->primitive, whole, instance_count) : 0;
	if (whole && dwords <= PIECE_MOST_DWORDS) {
		point_streams(r, b, (int32_t)first_vertex);
		if (!make_room(r, dwords))
			return;
		xsdk_gpu_put_draw_instanced(&r->packets, r->kept[0], pipeline->vertex.instance_constant, pipeline->primitive,
			whole, first_instance, instance_count);
		lose_constant(r, pipeline->vertex.instance_constant);
		return;
	}
	for (uint32_t i = 0; i < instance_count; i++)
		draw_instance(r, b, vertex_count, first_vertex, first_instance + i);
}

void v4x_record_draw(Recorder *r, const Bound *b, uint32_t vertex_count, uint32_t first_vertex, uint32_t instance_count,
	uint32_t first_instance)
{
	if (!drawable(b, vertex_count, instance_count))
		return;
	const Pipeline *const pipeline = b->pipeline;
	if (!plain(pipeline, vertex_count, instance_count)) {
		draw_instances(r, b, vertex_count, first_vertex, instance_count, first_instance);
	} else if (prepare_draw(r, b)) {
		point_streams(r, b, (int32_t)first_vertex);
		xsdk_gpu_put_draw(&r->packets, pipeline->primitive, vertex_count);
	}
	draw_done(r);
}

bool v4x_record_repeats_indices(const Bound *b, uint32_t index_count, uint32_t instance_count)
{
	return b->pipeline && instance_count > 1 && instanced_list(b->pipeline, index_count);
}

// A list's indices are repeated, a copy an instance, unless the copies' memory is full (transfer.c); one instance draws
// from the app's own, which a copy would only read back first, from write-combined memory at 80 MB/s
// (xsdk:docs/HARDWARE.md, "Instancing on the console, milestone 47 step 1"). The copies are made at the submit, from
// the indices then.
static __attribute__((noinline)) void draw_indexed_instances(Recorder *r, const Bound *b, VkQueue queue, uint32_t count,
	uint32_t first_index, int32_t vertex_offset, uint32_t instance_count, uint32_t first_instance)
{
	const Pipeline *const pipeline = b->pipeline;
	const uint32_t index_bytes = b->index_32_bit ? 4 : 2;
	if (!prepare_draw(r, b))
		return;
	const uint32_t whole = instanced_list(pipeline, count);
	XsdkRepeatedIndices repeated;
	if (whole && instance_count > 1 && queue &&
		v4x_transfer_repeat_indices(
			queue, b->index_bytes + first_index * index_bytes, b->index_32_bit, whole, instance_count, &repeated) &&
		xsdk_gpu_draw_indexed_instanced_dwords(pipeline->primitive, &repeated, instance_count) <= PIECE_MOST_DWORDS) {
		point_streams(r, b, vertex_offset);
		if (!make_room(r, xsdk_gpu_draw_indexed_instanced_dwords(pipeline->primitive, &repeated, instance_count)))
			return;
		xsdk_gpu_put_draw_indexed_instanced(&r->packets, r->kept[0], pipeline->vertex.instance_constant,
			pipeline->primitive, &repeated, first_instance, instance_count);
		lose_constant(r, pipeline->vertex.instance_constant);
		return;
	}
	for (uint32_t i = 0; i < instance_count; i++)
		draw_indexed_instance(r, b, count, first_index, vertex_offset, first_instance + i);
}

void v4x_record_draw_indexed(Recorder *r, const Bound *b, VkQueue queue, uint32_t count, uint32_t first_index,
	int32_t vertex_offset, uint32_t instance_count, uint32_t first_instance)
{
	if (!drawable(b, count, instance_count))
		return;
	const Pipeline *const pipeline = b->pipeline;
	if (!plain(pipeline, count, instance_count)) {
		draw_indexed_instances(r, b, queue, count, first_index, vertex_offset, instance_count, first_instance);
	} else if (prepare_draw(r, b)) {
		point_streams(r, b, vertex_offset);
		const uint32_t address = b->index_address + first_index * (b->index_32_bit ? 4 : 2);
		xsdk_gpu_put_draw_indexed(&r->packets, pipeline->primitive, address, count, b->index_32_bit);
	}
	draw_done(r);
}

// The most each part of a draw writes: every state a draw puts when the run knows nothing, each literal and resource as
// its own write, every stream, the vertex base and instance group, and the draw; 0 past what a piece holds.
uint32_t v4x_record_pipeline_dwords(const Pipeline *pipeline)
{
	uint32_t dwords = XSDK_GPU_SHADERS_DWORDS + XSDK_GPU_HIZ_CONTROL_DWORDS + XSDK_GPU_VIEWPORT_DWORDS +
		XSDK_GPU_SCISSOR_DWORDS + XSDK_GPU_BLENDS_DWORDS + XSDK_GPU_RASTER_DWORDS + XSDK_GPU_DEPTH_STENCIL_DWORDS +
		XSDK_GPU_BLEND_CONSTANTS_DWORDS + XSDK_GPU_CLIP_PLANES_DWORDS + XSDK_GPU_LOOP_CONSTANTS_DWORDS +
		pipeline->stream_count * XSDK_GPU_VERTICES_DWORDS + 2 * REGISTER_DWORDS + XSDK_GPU_DRAW_INDEXED_DWORDS;
	const V4xShader *const sources[2] = { pipeline->vertex_source, pipeline->pixel_source };
	for (uint32_t s = 0; s < 2; s++) {
		for (uint32_t i = 0; i < sources[s]->literal_count; i++)
			dwords += xsdk_gpu_float_constant_dwords(sources[s]->literals[i].reg, 1);
	}
	for (uint32_t i = 0; i < pipeline->resource_count; i++) {
		const StageResource *const s = &pipeline->resources[i];
		switch (s->resource.kind) {
		case V4X_RESOURCE_PUSH_CONSTANTS:
			dwords += xsdk_gpu_float_constant_dwords(s->resource.first + s->push_first, s->push_count);
			break;
		case V4X_RESOURCE_UNIFORM_BUFFER:
			dwords +=
				XSDK_GPU_LOAD_CONSTANTS_DWORDS + xsdk_gpu_float_constant_dwords(s->resource.first, s->resource.count);
			break;
		case V4X_RESOURCE_COMPARISON:
			dwords += REGISTER_DWORDS;
			break;
		case V4X_RESOURCE_SAMPLER:
			dwords += XSDK_GPU_TEXTURE_DWORDS;
			break;
		default:
			break;
		}
	}
	return dwords <= PIECE_MOST_DWORDS ? dwords : 0;
}

// A piece's dwords into the stream at the 16-byte phase they have in the recorder's packets, so its constants' values
// stay on 16 bytes, padded with NOPs either side; returns where its first dword went. Both share their 16-byte
// boundaries, so the middle goes in VMX stores (xsdk:docs/HARDWARE.md, "Copies into write-combined and cached memory").
static volatile uint32_t *copy_piece(volatile const uint32_t *source, uint32_t count)
{
	assert(count <= PIECE_MOST_DWORDS);
	volatile uint32_t *const space = xsdk_gpu_reserve(count + 3);
	const uint32_t pad = ((uint32_t)((uintptr_t)source >> 2) - (uint32_t)((uintptr_t)space >> 2)) & 3;
	for (uint32_t i = 0; i < pad; i++)
		space[i] = TYPE2_NOP;
	volatile uint32_t *const out = space + pad;
	uint32_t i = 0;
	for (; i < count && ((uintptr_t)&out[i] & 15); i++)
		out[i] = source[i];
	for (; i + 4 <= count; i += 4)
		vec_st(vec_ld(0, (const vector unsigned int *)(uintptr_t)&source[i]), 0, (vector unsigned int *)&out[i]);
	for (; i < count; i++)
		out[i] = source[i];
	for (uint32_t j = pad; j < 3; j++)
		out[count + j - pad] = TYPE2_NOP;
	return out;
}

// A uniform buffer's words into the packet as it holds them now; mapped memory is write-combined, so each read goes to
// memory, and a quad at a time is a quarter of the reads.
static void read_patch(volatile uint32_t *out, const ReadPatch *patch)
{
	if (patch->kind == PATCH_LOOP) {
		const int32_t count = patch->present ? (int32_t)patch->words[0] : 0;
		*out = xsdk_gpu_loop_constant(xsdk_xsc_loop_passes(patch->start, patch->step, count), 0, 1);
		return;
	}
	uint32_t i = 0;
	if ((((uintptr_t)patch->words | (uintptr_t)out) & 15) == 0) {
		for (; i + 4 <= patch->present; i += 4)
			vec_st(
				vec_ld(0, (const vector unsigned int *)(uintptr_t)&patch->words[i]), 0, (vector unsigned int *)&out[i]);
	}
	for (; i < patch->count; i++)
		out[i] = i < patch->present ? patch->words[i] : 0;
}

uint32_t v4x_record_copy(const Recorder *r, const Run *run, uint32_t serial)
{
	XsdkHiz hiz;
	xsdk_gpu_hiz_state(&hiz);
	const HizSite *site = &r->sites[run->first_site];
	const HizSite *const sites_end = &r->sites[run->end_site];
	const ReadPatch *patch = &r->patches[run->first_patch];
	const ReadPatch *const patches_end = &r->patches[run->end_patch];
	uint32_t piece_first = run->first;
	for (uint32_t c = run->first_cut; c <= run->end_cut; c++) {
		const uint32_t piece_end = c < run->end_cut ? r->cuts[c] : run->end;
		if (piece_end == piece_first)
			continue;
		volatile uint32_t *const out = copy_piece(&r->packets.dwords[piece_first], piece_end - piece_first);
		// The hierarchical buffer's control as xsdk's setters would work it out, from what it holds now.
		for (; site < sites_end && site->offset < piece_end; site++) {
			if (site->depth)
				hiz.depth_control = site->depth_control;
			else
				hiz.pixel_writes_depth = site->pixel_writes_depth;
			out[site->offset - piece_first] = xsdk_gpu_hiz_control(&hiz);
		}
		for (; patch < patches_end && patch->offset < piece_end; patch++)
			read_patch(&out[patch->offset - piece_first], patch);
		piece_first = piece_end;
	}
	xsdk_gpu_packets_written(&hiz);
	for (uint32_t i = run->first_image; i < run->end_image; i++) {
		r->images[i]->used_serial = serial;
		v4x_profile_pass_texture(r->images[i]->texture.address);
	}
	v4x_profile_draws(run->draws);
	return run->draws;
}
