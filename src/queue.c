#include <altivec.h>
#include <assert.h>
#include <math.h>
#include <string.h>

#include <vulkan/vulkan.h>

#include "blit.h"
#include "host.h"
#include "objects.h"
#include "pipeline_state.h"
#include "sync.h"
#include "transfer.h"
#include "xsdk/gpu.h"
#include "xsdk/log.h"
#include "xsdk/texture.h"

enum {
	IDLE_TIMEOUT_MS = 10000,
	// All of physical memory, as libxenon invalidates it (libxenon:drivers/xenos/xe.c:992).
	ALL_MEMORY_BYTES = 0x20000000,
	// The GPU loads constants in whole groups of 4 from a multiple of 4, from 64-byte aligned memory
	// (xsdk:include/xsdk/gpu.h); xost gives each uniform block such groups to itself.
	CONSTANT_GROUP = 4,
	LOAD_ALIGNMENT_BYTES = 64,
	// The visibility ids one status word holds (xsdk:include/xsdk/gpu.h).
	VISIBILITY_WORD_IDS = XSDK_VISIBILITY_IDS / 2,
};

typedef struct {
	uint32_t address;
	uint32_t size_bytes;
} VertexBinding;

typedef struct {
	const XsdkTexture *texture;
	const Sampler *sampler;
} PointedTexture;

// What one replay has bound so far.
typedef struct {
	VkQueue queue;
	// The submission it belongs to (VkQueue_T's serial).
	uint32_t serial;
	const Pipeline *pipeline;
	// The pipeline's registers must be set again: a resolve replaces the vertex shader, the viewport and the state.
	bool pipeline_dirty;
	// Only the viewport and scissor must, after vkCmdSetViewport or vkCmdSetScissor.
	bool viewport_dirty;
	VkViewport viewport;
	VkRect2D scissor;
	// And the raster, depth and stencil, and blend constants, after a command set the state they may take.
	bool dynamic_dirty;
	DynamicState dynamic;
	// User clip planes are on, as the last draw's pipeline set them; a target set turns them off.
	bool clip_planes_on;
	const RenderPass *render_pass;
	const Framebuffer *framebuffer;
	// The render area, which the pass clears, loads and stores alone.
	VkRect2D area;
	// The depth image's own clear covers all of it, past a smaller area, so it's drawn (clear_image_depth).
	bool draws_image_clear;
	// Where the pass's depth is, in queue->depths and in eDRAM (place_depth).
	uint32_t depth_part;
	uint32_t depth_tile;
	// What the pass's attachments clear to, in the command buffer's data.
	const PassClears *clears;
	// The pass clears its depth: its load op says so, or its image's vkCmdClearDepthStencilImage waits (Image).
	bool clears_depth;
	float clear_depth;
	uint32_t clear_stencil;
	VertexBinding bindings[V4X_MAX_VERTEX_BINDINGS];
	uint32_t index_address;
	bool index_32_bit;
	const DescriptorSet *sets[V4X_MAX_DESCRIPTOR_SETS];
	const uint32_t *dynamic_offsets[V4X_MAX_DESCRIPTOR_SETS];
	float push[V4X_MAX_PUSH_CONSTANT_BYTES / 4];
	// What each stream and texture slot points at, so a draw points only what changed; zero where it isn't known.
	VertexBinding pointed_streams[XSDK_VERTEX_STREAMS];
	PointedTexture pointed_textures[V4X_TEXTURE_SLOTS];
	// The occlusion query open, Vulkan's one at a time: its slot's GPU address, 0 for none, and the visibility id its
	// draws go to now, from 0 (begin_query).
	uint32_t query_address;
	uint32_t visibility_id;
	// In a tiled pass, whose commands go into its recording (begin_tiled_pass).
	bool tiling;
} Replay;

// A block's registers on their way into the constants; the queue is externally synchronized, so one copy will do.
static float block[V4X_FLOAT_CONSTANTS * 4] __attribute__((aligned(16)));

static void write_literals(const V4xShader *shader, bool vertex)
{
	for (uint32_t i = 0; i < shader->literal_count; i++) {
		const V4xShaderLiteral *const literal = &shader->literals[i];
		if (vertex)
			xsdk_gpu_set_vertex_constants(literal->reg, literal->values, 1);
		else
			xsdk_gpu_set_pixel_constants(literal->reg, literal->values, 1);
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

// Vulkan's y points down and its pixel centres sit on halves; xsdk's viewport is y-up with centres on whole numbers.
// Vulkan clips to the viewport, and Xenos draws past it as far as the scissor (docs/SUPPORT.md, Pipelines), so the
// scissor stops at the viewport's pixels too.
static void apply_viewport(const Pipeline *pipeline, const Replay *replay)
{
	const VkViewport *const v = pipeline->dynamic_viewport ? &replay->viewport : &pipeline->viewport;
	xsdk_gpu_set_viewport(v->x - 0.5f, v->y + v->height - 0.5f, v->width, -v->height, v->minDepth, v->maxDepth);
	const VkRect2D *const s = pipeline->dynamic_scissor ? &replay->scissor : &pipeline->scissor;
	const int32_t left = larger(s->offset.x, first_pixel(v->x));
	const int32_t top = larger(s->offset.y, first_pixel(v->y));
	const int32_t right = smaller(s->offset.x + (int32_t)s->extent.width, first_pixel(v->x + v->width));
	const int32_t bottom = smaller(s->offset.y + (int32_t)s->extent.height, first_pixel(v->y + v->height));
	xsdk_gpu_set_scissor((uint32_t)left, (uint32_t)top, right > left ? (uint32_t)(right - left) : 0,
		bottom > top ? (uint32_t)(bottom - top) : 0);
}

// The state blocks dynamic state may change: the pipeline's own, or worked out with what the commands set.
static void apply_dynamic(const Pipeline *pipeline, const Replay *replay)
{
	XsdkRasterState raster;
	XsdkDepthStencilState depth_stencil;
	xsdk_gpu_set_raster(v4x_pipeline_raster(pipeline, &replay->dynamic, &raster));
	xsdk_gpu_set_depth_stencil(v4x_pipeline_depth_stencil(pipeline, &replay->dynamic, &depth_stencil));
	const float *const constants = v4x_pipeline_blend_constants(pipeline, &replay->dynamic);
	if (constants)
		xsdk_gpu_set_blend_constants(constants);
}

static void apply_pipeline(const Pipeline *pipeline, const Replay *replay)
{
	xsdk_gpu_set_shaders(&pipeline->vertex, &pipeline->pixel);
	apply_viewport(pipeline, replay);
	xsdk_gpu_set_blends(pipeline->blends, pipeline->blend_count);
	apply_dynamic(pipeline, replay);
	write_literals(pipeline->vertex_source, true);
	write_literals(pipeline->pixel_source, false);
}

static void write_constants(bool vertex, uint32_t first, const float *values, uint32_t count)
{
	if (vertex)
		xsdk_gpu_set_vertex_constants(first, values, count);
	else
		xsdk_gpu_set_pixel_constants(first, values, count);
}

// A uniform buffer's bytes as the block's registers, zero past the descriptor's range.
static void read_uniform_buffer(const Replay *replay, const V4xShaderResource *resource)
{
	const DescriptorSet *const set = replay->sets[resource->set];
	const Descriptor *const descriptor = &set->descriptors[resource->binding];
	const LayoutBinding *const binding = &set->layout->bindings[resource->binding];
	volatile const uint32_t *words = descriptor->buffer;
	if (binding->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC)
		words += replay->dynamic_offsets[resource->set][binding->dynamic_index] / 4;
	const uint32_t wanted = resource->count * 4;
	const uint32_t present = descriptor->range_bytes / 4 < wanted ? descriptor->range_bytes / 4 : wanted;
	uint32_t i = 0;
	// Mapped memory is write-combined, so each read goes to memory: a quad at a time is a quarter of the reads.
	if (((uintptr_t)words & 15) == 0) {
		for (; i + 4 <= present; i += 4)
			vec_st(vec_ld(0, (const vector unsigned int *)(uintptr_t)(words + i)), 0, (vector unsigned int *)&block[i]);
	}
	for (; i < wanted; i++) {
		const uint32_t word = i < present ? words[i] : 0;
		memcpy(&block[i], &word, sizeof word);
	}
}

// Each draw writes every block its shaders read into constants, and points their samplers' fetch constants.
// Has the GPU load the block straight from the buffer, when the block starts a group and the buffer covers it; the
// padding past the block takes whatever follows it, which nothing reads.
static bool load_uniform_buffer(const Replay *replay, const V4xShaderResource *resource, bool vertex)
{
	const DescriptorSet *const set = replay->sets[resource->set];
	const Descriptor *const descriptor = &set->descriptors[resource->binding];
	const LayoutBinding *const binding = &set->layout->bindings[resource->binding];
	uint32_t address = descriptor->buffer_address;
	if (binding->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC)
		address += replay->dynamic_offsets[resource->set][binding->dynamic_index];
	if (resource->first % CONSTANT_GROUP || address % LOAD_ALIGNMENT_BYTES ||
		descriptor->range_bytes < resource->count * 16)
		return false;
	const uint32_t count = (resource->count + CONSTANT_GROUP - 1) / CONSTANT_GROUP * CONSTANT_GROUP;
	if (vertex)
		xsdk_gpu_load_vertex_constants(resource->first, address, count);
	else
		xsdk_gpu_load_pixel_constants(resource->first, address, count);
	return true;
}

// A clip plane's four floats from the push constants or uniform buffer the vertex shader reads it from, as bound now;
// past the descriptor's range they read 0, as the block's registers do.
static void read_plane(const Replay *replay, const V4xShaderResource *resource, uint32_t offset_bytes, float plane[4])
{
	if (resource->kind == V4X_RESOURCE_PUSH_CONSTANTS) {
		memcpy(plane, (const uint8_t *)replay->push + offset_bytes, 4 * sizeof(float));
		return;
	}
	const DescriptorSet *const set = replay->sets[resource->set];
	const Descriptor *const descriptor = set ? &set->descriptors[resource->binding] : NULL;
	volatile const uint32_t *words = descriptor ? descriptor->buffer : NULL;
	const uint32_t present = descriptor ? descriptor->range_bytes / 4 : 0;
	if (descriptor && set->layout->bindings[resource->binding].type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC)
		words += replay->dynamic_offsets[resource->set][set->layout->bindings[resource->binding].dynamic_index] / 4;
	for (uint32_t c = 0; c < 4; c++) {
		const uint32_t at = offset_bytes / 4 + c;
		const uint32_t word = at < present ? words[at] : 0;
		memcpy(&plane[c], &word, sizeof word);
	}
}

// Xenos clips against the planes itself, so each draw of a pipeline with clip distances sets them from the values
// bound, which may have changed since; one without turns off what the last left on.
static void apply_clip_planes(const Pipeline *pipeline, Replay *replay)
{
	if (!pipeline->clip_plane_count && !replay->clip_planes_on)
		return;
	float planes[XSDK_CLIP_PLANES][4];
	for (uint32_t i = 0; i < pipeline->clip_plane_count; i++) {
		const ClipPlane *const clip = &pipeline->clip_planes[i];
		read_plane(replay, &pipeline->resources[clip->resource].resource, clip->offset_bytes, planes[clip->plane]);
	}
	xsdk_gpu_set_clip_planes(pipeline->clip_plane_mask, planes);
	replay->clip_planes_on = pipeline->clip_plane_count != 0;
}

// Points a slot at the texture and sampler unless it already is. A replay points every slot afresh, so each submission
// that samples an image marks it.
static void point_texture(Replay *replay, uint32_t slot, const Descriptor *descriptor)
{
	assert(slot < V4X_TEXTURE_SLOTS);
	PointedTexture *const pointed = &replay->pointed_textures[slot];
	if (pointed->texture == descriptor->texture && pointed->sampler == descriptor->sampler)
		return;
	xsdk_gpu_set_texture(slot, descriptor->texture, &descriptor->sampler->state);
	descriptor->image->used_serial = replay->serial;
	*pointed = (PointedTexture){ .texture = descriptor->texture, .sampler = descriptor->sampler };
}

// A loop's count as the draw has its block: the int at the resource's byte offset, none where the block isn't bound or
// its range doesn't reach it. A loop constant runs 0 to 255 passes (xenia:src/xenia/gpu/xenos.h:1086-1097).
static uint32_t loop_count(const Replay *replay, const V4xShaderResource *resource)
{
	int32_t count = 0;
	if (resource->kind == V4X_RESOURCE_PUSH_LOOP) {
		memcpy(&count, (const uint8_t *)replay->push + resource->first, sizeof count);
	} else if (replay->sets[resource->set]) {
		const DescriptorSet *const set = replay->sets[resource->set];
		const Descriptor *const descriptor = &set->descriptors[resource->binding];
		const LayoutBinding *const binding = &set->layout->bindings[resource->binding];
		uint32_t offset = resource->first;
		if (binding->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC)
			offset += replay->dynamic_offsets[resource->set][binding->dynamic_index];
		if (resource->first + sizeof count <= descriptor->range_bytes)
			count = (int32_t)descriptor->buffer[offset / 4];
	}
	return count < 0 ? 0 : count > V4X_MAX_LOOP_PASSES ? V4X_MAX_LOOP_PASSES : (uint32_t)count;
}

// xsdk rewrites every boolean and loop word together (xsdk:include/xsdk/gpu.h), so they're written only when one of
// the loop constants changes.
static void write_loops(VkQueue queue, const uint32_t *values)
{
	if (queue->loops_written && !memcmp(values, queue->loop_constants, sizeof queue->loop_constants))
		return;
	xsdk_gpu_set_loop_constants(0, values, XSDK_LOOP_CONSTANTS);
	memcpy(queue->loop_constants, values, sizeof queue->loop_constants);
	queue->loops_written = true;
}

// The loop constants a pipeline's hardware loops read, each from its count, or its fixed passes for loops whose count
// is data.
static void apply_loops(const Pipeline *pipeline, const Replay *replay)
{
	uint32_t values[XSDK_LOOP_CONSTANTS];
	memcpy(values, replay->queue->loop_constants, sizeof values);
	for (uint32_t i = 0; i < pipeline->resource_count; i++) {
		const V4xShaderResource *const resource = &pipeline->resources[i].resource;
		if (resource->kind == V4X_RESOURCE_PUSH_LOOP || resource->kind == V4X_RESOURCE_UNIFORM_LOOP)
			values[resource->count] = xsdk_gpu_loop_constant(loop_count(replay, resource), 0, 1);
		else if (resource->kind == V4X_RESOURCE_FIXED_LOOP)
			values[resource->count] = xsdk_gpu_loop_constant(resource->first, 0, 1);
	}
	write_loops(replay->queue, values);
}

static void apply_resources(const Pipeline *pipeline, Replay *replay)
{
	if (pipeline->loop_count)
		apply_loops(pipeline, replay);
	for (uint32_t i = 0; i < pipeline->resource_count; i++) {
		const V4xShaderResource *const resource = &pipeline->resources[i].resource;
		const bool vertex = pipeline->resources[i].vertex;
		if (resource->kind == V4X_RESOURCE_PUSH_CONSTANTS) {
			write_constants(vertex, resource->first, replay->push, resource->count);
			continue;
		}
		if (resource->kind == V4X_RESOURCE_PUSH_LOOP || resource->kind == V4X_RESOURCE_UNIFORM_LOOP ||
			resource->kind == V4X_RESOURCE_FIXED_LOOP)
			continue;
		if (!replay->sets[resource->set])
			continue;
		if (resource->kind == V4X_RESOURCE_UNIFORM_BUFFER) {
			if (!load_uniform_buffer(replay, resource, vertex)) {
				read_uniform_buffer(replay, resource);
				write_constants(vertex, resource->first, block, resource->count);
			}
			continue;
		}
		if (resource->kind == V4X_RESOURCE_COMPARISON) {
			const Sampler *const sampler = replay->sets[resource->set]->descriptors[resource->binding].sampler;
			write_constants(vertex, resource->first, sampler->compare_weights, 1);
			continue;
		}
		point_texture(replay, resource->first, &replay->sets[resource->set]->descriptors[resource->binding]);
	}
}

// Resolves clear eDRAM's depth and stencil to what xsdk_gpu_set_depth_clear last took.
static void set_depth_clear(VkQueue queue, float depth, uint32_t stencil)
{
	if (queue->depth_clear == depth && queue->stencil_clear == stencil)
		return;
	xsdk_gpu_set_depth_clear(depth, (uint8_t)stencil);
	queue->depth_clear = depth;
	queue->stencil_clear = stencil;
}

// The colour targets one after another from tile 0 (render_pass.c), and depth where place_depth put it
// (docs/ROADMAP.md, milestones 4, 14 and 25).
static void set_target(const Replay *replay)
{
	const Framebuffer *const framebuffer = replay->framebuffer;
	xsdk_gpu_set_targets(framebuffer->width, framebuffer->height, framebuffer->targets,
		replay->render_pass->color_count, replay->depth_tile);
}

// Colour target i alone where it sits, to draw an image back into.
static void set_one_target(const Replay *replay, uint32_t i)
{
	const Framebuffer *const framebuffer = replay->framebuffer;
	xsdk_gpu_set_targets(framebuffer->width, framebuffer->height, &framebuffer->targets[i], 1, replay->depth_tile);
}

static VkRect2D whole(const Framebuffer *framebuffer)
{
	return (VkRect2D){ { 0, 0 }, { framebuffer->width, framebuffer->height } };
}

static bool contains(const VkRect2D *outer, const VkRect2D *inner)
{
	return inner->offset.x >= outer->offset.x && inner->offset.y >= outer->offset.y &&
		inner->offset.x + inner->extent.width <= outer->offset.x + outer->extent.width &&
		inner->offset.y + inner->extent.height <= outer->offset.y + outer->extent.height;
}

static bool whole_area(const Replay *replay)
{
	const VkRect2D all = whole(replay->framebuffer);
	return contains(&replay->area, &all);
}

// A channel of the clear as a rectangle's resolve takes it, rounded as a whole one rounds (xsdk:src/gpu_resolve.c).
static uint32_t unorm8(float value)
{
	const float clamped = value < 0.0f ? 0.0f : value > 1.0f ? 1.0f : value;
	return (uint32_t)(clamped * 255.0f + 0.5f);
}

// Copies eDRAM's colour target i in the area into its attachment, then clears what's asked there; the targets must be
// set again after. xsdk resolves a smaller area of target 0 alone, in 8_8_8_8, as vkCmdBeginRenderPass makes sure.
static void resolve(
	Image *attachment, uint32_t target, const Replay *replay, bool clear_color, bool clear_depth, const float rgba[4])
{
	const VkRect2D *const area = &replay->area;
	xsdk_gpu_set_resolve_clears(clear_color, clear_depth);
	if (whole_area(replay)) {
		xsdk_gpu_resolve_target(target, &attachment->texture, rgba);
	} else {
		assert(target == 0);
		const uint32_t argb = unorm8(rgba[3]) << 24 | unorm8(rgba[0]) << 16 | unorm8(rgba[1]) << 8 | unorm8(rgba[2]);
		xsdk_gpu_resolve_texture_rect(&attachment->texture, (uint32_t)area->offset.x, (uint32_t)area->offset.y,
			area->extent.width, area->extent.height, argb);
	}
	attachment->used_serial = replay->serial;
}

static bool same_size(const EdramPart *part, const Framebuffer *framebuffer)
{
	return part->width == framebuffer->width && part->height == framebuffer->height;
}

static EdramPart cleared_color(const Framebuffer *framebuffer, uint32_t target, const float rgba[4], VkRect2D area)
{
	EdramPart part = { .contents = EDRAM_CLEARED,
		.width = framebuffer->width,
		.height = framebuffer->height,
		.tile = framebuffer->targets[target].tile,
		.format = framebuffer->targets[target].format,
		.area = area };
	memcpy(part.rgba, rgba, sizeof part.rgba);
	return part;
}

static EdramPart cleared_depth(
	const Framebuffer *framebuffer, uint32_t tile, float depth, uint32_t stencil, VkRect2D area)
{
	return (EdramPart){ .contents = EDRAM_CLEARED,
		.width = framebuffer->width,
		.height = framebuffer->height,
		.depth = depth,
		.stencil = stencil,
		.tile = tile,
		.area = area };
}

static EdramPart holding(const Image *image, uint32_t tile, VkRect2D area)
{
	return (EdramPart){ .contents = EDRAM_IMAGE,
		.width = image->width,
		.height = image->height,
		.image = image,
		.tile = tile,
		.area = area };
}

static EdramPart holding_color(const Framebuffer *framebuffer, uint32_t target, VkRect2D area)
{
	EdramPart part = holding(framebuffer->attachments[target], framebuffer->targets[target].tile, area);
	part.format = framebuffer->targets[target].format;
	return part;
}

// What eDRAM holds where colour target i goes: what the last pass left at that tile in that format, or nothing known,
// as anything else there may be part of another target.
static EdramPart held_color(VkQueue queue, const Framebuffer *framebuffer, uint32_t target)
{
	const XsdkColorTarget *const wanted = &framebuffer->targets[target];
	for (uint32_t i = 0; i < V4X_MAX_COLOR_ATTACHMENTS; i++) {
		const EdramPart *const part = &queue->colors[i];
		if (part->contents != EDRAM_UNKNOWN && part->tile == wanted->tile && part->format == wanted->format)
			return *part;
	}
	return (EdramPart){ .contents = EDRAM_UNKNOWN };
}

// Whether eDRAM's colour target i is what its load op wants over the area.
static bool color_ready(const EdramPart *color, const Replay *replay, uint32_t target)
{
	const Framebuffer *const framebuffer = replay->framebuffer;
	switch (replay->render_pass->load_ops[target]) {
	case VK_ATTACHMENT_LOAD_OP_CLEAR:
		return color->contents == EDRAM_CLEARED && same_size(color, framebuffer) &&
			!memcmp(color->rgba, replay->clears->colors[target], sizeof color->rgba) &&
			contains(&color->area, &replay->area);
	case VK_ATTACHMENT_LOAD_OP_LOAD:
		return color->contents == EDRAM_IMAGE && color->image == framebuffer->attachments[target] &&
			contains(&color->area, &replay->area);
	default:
		return true;
	}
}

static bool depth_ready(const EdramPart *depth, const Replay *replay)
{
	return depth->contents == EDRAM_CLEARED && same_size(depth, replay->framebuffer) &&
		depth->depth == replay->clear_depth && depth->stencil == replay->clear_stencil &&
		contains(&depth->area, &replay->area);
}

// Whether a depth eDRAM holds reaches into tiles first to first + tiles.
static bool part_overlaps(const EdramPart *part, uint32_t first, uint32_t tiles)
{
	return part->contents != EDRAM_UNKNOWN && part->tile < first + tiles &&
		first < part->tile + xsdk_gpu_target_tiles(part->width, part->height);
}

// Whether tiles first to first + tiles stay above the pass's colour and miss every depth image eDRAM holds.
static bool depth_tiles_free(VkQueue queue, uint32_t first, uint32_t tiles, uint32_t color_tiles)
{
	if (first < color_tiles)
		return false;
	for (uint32_t i = 0; i < V4X_EDRAM_DEPTHS; i++) {
		if (queue->depths[i].contents == EDRAM_IMAGE && part_overlaps(&queue->depths[i], first, tiles))
			return false;
	}
	return true;
}

// Where a depth eDRAM doesn't hold goes: ending at the top, as milestone 4 placed every depth, or else just under a
// depth image eDRAM holds, so that image keeps its depth for a later pass to load; at the top all the same when
// neither fits.
static uint32_t new_depth_tile(VkQueue queue, uint32_t tiles, uint32_t color_tiles)
{
	const uint32_t top = XSDK_EDRAM_TILES - tiles;
	if (depth_tiles_free(queue, top, tiles, color_tiles))
		return top;
	for (uint32_t i = 0; i < V4X_EDRAM_DEPTHS; i++) {
		const EdramPart *const part = &queue->depths[i];
		if (part->contents == EDRAM_IMAGE && part->tile >= tiles &&
			depth_tiles_free(queue, part->tile - tiles, tiles, color_tiles))
			return part->tile - tiles;
	}
	return top;
}

// Places the pass's depth: where eDRAM holds its image's, or the clear the pass wants at its size, or at new tiles.
// Whatever depth the pass's colour or depth then reaches is lost, as colour drawn over it overwrites it.
static void place_depth(VkQueue queue, Replay *replay)
{
	const RenderPass *const render_pass = replay->render_pass;
	const Framebuffer *const framebuffer = replay->framebuffer;
	const uint32_t tiles = xsdk_gpu_target_tiles(framebuffer->width, framebuffer->height);
	const uint32_t color_tiles = framebuffer->color_tiles;
	uint32_t part = V4X_EDRAM_DEPTHS;
	for (uint32_t i = 0; i < V4X_EDRAM_DEPTHS && render_pass->has_depth; i++) {
		const EdramPart *const depth = &queue->depths[i];
		// The pass's colour targets would draw over a depth below their end.
		if (depth->tile < color_tiles)
			continue;
		// Another size's depth lies at another pitch, and may not fit where it is.
		if ((depth->contents == EDRAM_IMAGE && depth->image == framebuffer->depth && same_size(depth, framebuffer)) ||
			(part == V4X_EDRAM_DEPTHS && replay->clears_depth && depth_ready(depth, replay)))
			part = i;
	}
	const uint32_t tile = part < V4X_EDRAM_DEPTHS
		? queue->depths[part].tile
		: new_depth_tile(queue, tiles, render_pass->has_depth ? color_tiles : 0);
	for (uint32_t i = 0; i < V4X_EDRAM_DEPTHS; i++) {
		if (i != part &&
			(part_overlaps(&queue->depths[i], 0, color_tiles) ||
				(render_pass->has_depth && part_overlaps(&queue->depths[i], tile, tiles))))
			queue->depths[i] = (EdramPart){ .contents = EDRAM_UNKNOWN };
	}
	// A new depth takes a part eDRAM no longer holds, or else the last.
	for (uint32_t i = 0; i < V4X_EDRAM_DEPTHS && part == V4X_EDRAM_DEPTHS; i++) {
		if (queue->depths[i].contents == EDRAM_UNKNOWN)
			part = i;
	}
	if (part == V4X_EDRAM_DEPTHS)
		part = V4X_EDRAM_DEPTHS - 1;
	replay->depth_part = part;
	replay->depth_tile = render_pass->has_depth ? tile : XSDK_EDRAM_TILES - tiles;
}

// Depth never leaves eDRAM, so loading it after eDRAM lost it finds undefined contents.
static void check_depth_kept(VkQueue queue, const Replay *replay)
{
	static bool logged;
	const EdramPart *const depth = &queue->depths[replay->depth_part];
	if (logged || (depth->contents == EDRAM_IMAGE && depth->image == replay->framebuffer->depth))
		return;
	xsdk_log_write("vulkan4xenon: a depth attachment was loaded after eDRAM lost it; its contents are undefined");
	logged = true;
}

// vkCmdClearDepthStencilImage clears all of the image, past the pass's area, which a resolve would copy past too.
static void clear_image_depth(VkQueue queue, const Replay *replay, EdramPart *depth)
{
	const Framebuffer *const framebuffer = replay->framebuffer;
	const Image *const image = framebuffer->depth;
	const ClearRect clear = { .rect = whole(framebuffer),
		.depth = image->pending_depth,
		.stencil = image->pending_stencil,
		.clears_depth = true,
		.clears_stencil = true };
	v4x_blit_clear(&queue->blit, &clear, replay->render_pass->color_count, framebuffer->width, framebuffer->height);
	*depth = cleared_depth(
		framebuffer, replay->depth_tile, image->pending_depth, image->pending_stencil, whole(framebuffer));
}

// A load draws each image back over the area unless eDRAM holds it there, into its target alone where there are
// several.
static void draw_back(VkQueue queue, const Replay *replay, EdramPart *colors)
{
	const RenderPass *const render_pass = replay->render_pass;
	const Framebuffer *const framebuffer = replay->framebuffer;
	bool drawn = false;
	for (uint32_t i = 0; i < render_pass->color_count; i++) {
		if (render_pass->load_ops[i] != VK_ATTACHMENT_LOAD_OP_LOAD || color_ready(&colors[i], replay, i))
			continue;
		if (render_pass->color_count > 1)
			set_one_target(replay, i);
		v4x_blit_draw(&queue->blit, &framebuffer->attachments[i]->texture, framebuffer->width, framebuffer->height,
			&replay->area, 0, 1);
		framebuffer->attachments[i]->used_serial = replay->serial;
		colors[i] = holding_color(framebuffer, i, replay->area);
		drawn = true;
	}
	if (drawn && render_pass->color_count > 1)
		set_target(replay);
}

// A clear needs eDRAM at the clear values over the area, which a resolve of the area into the attachment sets, copying
// junk the pass overwrites, or the image itself after a load; depth clears with the first resolve, or the first
// target's if no colour clears.
static void clear_targets(VkQueue queue, const Replay *replay, EdramPart *colors)
{
	const RenderPass *const render_pass = replay->render_pass;
	const Framebuffer *const framebuffer = replay->framebuffer;
	EdramPart *const depth = &queue->depths[replay->depth_part];
	const bool clears_depth = replay->clears_depth && !depth_ready(depth, replay);
	bool depth_left = clears_depth;
	bool resolved = false;
	if (clears_depth)
		set_depth_clear(queue, replay->clear_depth, replay->clear_stencil);
	for (uint32_t i = 0; i < render_pass->color_count; i++) {
		if (color_ready(&colors[i], replay, i))
			continue;
		resolve(framebuffer->attachments[i], i, replay, true, depth_left, replay->clears->colors[i]);
		colors[i] = cleared_color(framebuffer, i, replay->clears->colors[i], replay->area);
		depth_left = false;
		resolved = true;
	}
	if (depth_left)
		resolve(framebuffer->attachments[0], 0, replay, false, true, replay->clears->colors[0]);
	if (resolved || depth_left)
		set_target(replay);
	if (clears_depth)
		*depth =
			cleared_depth(framebuffer, replay->depth_tile, replay->clear_depth, replay->clear_stencil, replay->area);
}

static void begin_render_pass(VkQueue queue, Replay *replay)
{
	const RenderPass *const render_pass = replay->render_pass;
	const Framebuffer *const framebuffer = replay->framebuffer;
	place_depth(queue, replay);
	EdramPart colors[V4X_MAX_COLOR_ATTACHMENTS];
	for (uint32_t i = 0; i < V4X_MAX_COLOR_ATTACHMENTS; i++)
		colors[i] =
			i < render_pass->color_count ? held_color(queue, framebuffer, i) : (EdramPart){ .contents = EDRAM_UNKNOWN };
	set_target(replay);
	if (render_pass->has_depth && render_pass->loads_depth && !replay->clears_depth && !replay->draws_image_clear)
		check_depth_kept(queue, replay);
	if (replay->draws_image_clear)
		clear_image_depth(queue, replay, &queue->depths[replay->depth_part]);
	draw_back(queue, replay, colors);
	clear_targets(queue, replay, colors);
	memcpy(queue->colors, colors, sizeof queue->colors);
	replay->pipeline_dirty = true;
	replay->clip_planes_on = false;
	// Drawing an image back and resolves point streams and slots of their own.
	memset(replay->pointed_streams, 0, sizeof replay->pointed_streams);
	memset(replay->pointed_textures, 0, sizeof replay->pointed_textures);
}

// The resolve clears target i over the area as the next pass in the command buffer clears the target at its tile, in
// its format, at this size and within this area, and keeps it otherwise; the last pass clears to its own colour, so the
// next frame finds it ready.
static bool end_color_clear(const Replay *replay, uint32_t target, const Command *next, const PassClears *next_clears,
	const VkRect2D *next_area, const float **rgba)
{
	*rgba = replay->clears->colors[target];
	if (!next)
		return replay->render_pass->load_ops[target] == VK_ATTACHMENT_LOAD_OP_CLEAR;
	const RenderPass *const pass = next->begin.render_pass;
	const Framebuffer *const framebuffer = next->begin.framebuffer;
	const XsdkColorTarget *const own = &replay->framebuffer->targets[target];
	if (framebuffer->width != replay->framebuffer->width || framebuffer->height != replay->framebuffer->height ||
		!contains(&replay->area, next_area))
		return false;
	for (uint32_t i = 0; i < pass->color_count; i++) {
		if (framebuffer->targets[i].tile != own->tile || framebuffer->targets[i].format != own->format)
			continue;
		*rgba = next_clears->colors[i];
		return pass->load_ops[i] == VK_ATTACHMENT_LOAD_OP_CLEAR;
	}
	return false;
}

// The same for depth, over the same area alone, as eDRAM holds a depth image's depth around a pass's area; a pass
// without depth never clears it, as a later pass may load an earlier one's.
static bool end_depth_clear(const Replay *replay, const Command *next, const PassClears *next_clears,
	const VkRect2D *next_area, float *depth, uint32_t *stencil)
{
	if (!replay->render_pass->has_depth)
		return false;
	if (!next) {
		*depth = replay->clear_depth;
		*stencil = replay->clear_stencil;
		return replay->render_pass->clears_depth;
	}
	const Framebuffer *const framebuffer = next->begin.framebuffer;
	*depth = next_clears->depth;
	*stencil = next_clears->stencil;
	return next->begin.render_pass->clears_depth && framebuffer->width == replay->framebuffer->width &&
		framebuffer->height == replay->framebuffer->height && contains(&replay->area, next_area) &&
		contains(next_area, &replay->area);
}

// A sampled depth attachment the pass leaves to be read goes into its texture first, clearing nothing, so the colour's
// resolve clears as it would.
static void resolve_depth(Image *depth, uint32_t serial)
{
	xsdk_gpu_set_resolve_clears(false, false);
	xsdk_gpu_resolve_depth_texture(&depth->texture);
	depth->used_serial = serial;
}

// Where eDRAM holds an image once a rectangle of it is resolved: where it held it, if over the rectangle, else there.
static VkRect2D stored_area(const EdramPart *color, const Image *image, const VkRect2D *rect)
{
	const bool held = color->contents == EDRAM_IMAGE && color->image == image && contains(&color->area, rect);
	return held ? color->area : *rect;
}

// Each stored target's area is resolved into its attachment, the first of them clearing depth for the next pass too; a
// sampled depth image takes the whole of eDRAM's depth, which holds it all.
static void end_render_pass(
	VkQueue queue, Replay *replay, const Command *next, const PassClears *next_clears, const VkRect2D *next_area)
{
	const RenderPass *const render_pass = replay->render_pass;
	const Framebuffer *const framebuffer = replay->framebuffer;
	EdramPart *const depth_part = &queue->depths[replay->depth_part];
	if (render_pass->has_depth)
		*depth_part = holding(framebuffer->depth, replay->depth_tile, whole(framebuffer));
	if (render_pass->has_depth && render_pass->leaves_depth_to_read && framebuffer->depth->is_sampled_depth)
		resolve_depth(framebuffer->depth, replay->serial);
	float depth = 0.0f;
	uint32_t stencil = 0;
	bool clear_depth = end_depth_clear(replay, next, next_clears, next_area, &depth, &stencil);
	for (uint32_t i = 0; i < render_pass->color_count; i++) {
		if (render_pass->store_ops[i] != VK_ATTACHMENT_STORE_OP_STORE) {
			queue->colors[i] = (EdramPart){ .contents = EDRAM_UNKNOWN };
			continue;
		}
		const float *rgba;
		const bool clear_color = end_color_clear(replay, i, next, next_clears, next_area, &rgba);
		if (clear_depth)
			set_depth_clear(queue, depth, stencil);
		const VkRect2D stored = stored_area(&queue->colors[i], framebuffer->attachments[i], &replay->area);
		resolve(framebuffer->attachments[i], i, replay, clear_color, clear_depth, rgba);
		queue->colors[i] =
			clear_color ? cleared_color(framebuffer, i, rgba, replay->area) : holding_color(framebuffer, i, stored);
		if (clear_depth)
			*depth_part = cleared_depth(framebuffer, replay->depth_tile, depth, stencil, replay->area);
		clear_depth = false;
	}
}

// Where a tiled pass keeps a whole depth: ending at eDRAM's top (render_pass.c, tile_targets).
static uint32_t top_depth_tile(const Framebuffer *framebuffer)
{
	return XSDK_EDRAM_TILES - xsdk_gpu_target_tiles(framebuffer->width, framebuffer->height);
}

// A tiled pass's bands reach every tile but its whole depth's, so eDRAM keeps the depth part holding the pass's own
// depth there, or the clear it wants, as part 0, and loses every other part and the colour targets.
static void keep_tiled_depth(VkQueue queue, Replay *replay)
{
	const Framebuffer *const framebuffer = replay->framebuffer;
	const uint32_t tile = top_depth_tile(framebuffer);
	EdramPart kept = { .contents = EDRAM_UNKNOWN };
	for (uint32_t i = 0; i < V4X_EDRAM_DEPTHS && framebuffer->tiling.depth_whole; i++) {
		const EdramPart *const depth = &queue->depths[i];
		if (depth->tile == tile &&
			((depth->contents == EDRAM_IMAGE && depth->image == framebuffer->depth && same_size(depth, framebuffer)) ||
				(replay->clears_depth && depth_ready(depth, replay))))
			kept = *depth;
	}
	for (uint32_t i = 0; i < V4X_EDRAM_DEPTHS; i++)
		queue->depths[i] = i ? (EdramPart){ .contents = EDRAM_UNKNOWN } : kept;
	for (uint32_t i = 0; i < V4X_MAX_COLOR_ATTACHMENTS; i++)
		queue->colors[i] = (EdramPart){ .contents = EDRAM_UNKNOWN };
	replay->depth_part = 0;
	replay->depth_tile = tile;
}

// Whether the depth image's texture takes the pass's depth a band at a time: in bands, stored and sampled; whole, left
// to be read, as an untiled pass leaves it.
static bool resolves_depth(const RenderPass *render_pass, const Framebuffer *framebuffer)
{
	if (!render_pass->has_depth || !framebuffer->depth->is_sampled_depth)
		return false;
	return framebuffer->tiling.depth_whole ? render_pass->leaves_depth_to_read : render_pass->stores_depth;
}

// Depth in bands that loads comes back from its image's texture, which the last pass to store it filled.
static bool draws_depth_back(const Replay *replay)
{
	const RenderPass *const render_pass = replay->render_pass;
	return render_pass->has_depth && !replay->framebuffer->tiling.depth_whole && render_pass->loads_depth &&
		!replay->clears_depth;
}

// Resolves never clear a whole depth, so one eDRAM doesn't hold cleared is drawn; one in bands is cleared with band 0's
// clear by the last resolve that isn't drawn back, or drawn if every resolve is.
static bool draws_depth_clear(const Replay *replay)
{
	const RenderPass *const render_pass = replay->render_pass;
	const Framebuffer *const framebuffer = replay->framebuffer;
	if (!render_pass->has_depth || !replay->clears_depth)
		return false;
	if (framebuffer->tiling.depth_whole)
		return !depth_ready(&replay->queue->depths[0], replay);
	bool resolved = resolves_depth(render_pass, framebuffer);
	for (uint32_t i = 0; i < render_pass->color_count; i++)
		resolved = resolved || render_pass->load_ops[i] != VK_ATTACHMENT_LOAD_OP_LOAD;
	return !resolved;
}

// Into the recording first, so each band draws its own rows: the depth clear no resolve makes, depth drawn back, and
// each colour attachment that loads, drawn into its target alone by the write masks.
static void load_tiles(VkQueue queue, const Replay *replay)
{
	static bool stencil_logged;
	const RenderPass *const render_pass = replay->render_pass;
	const Framebuffer *const framebuffer = replay->framebuffer;
	const uint32_t count = render_pass->color_count;
	if (draws_depth_clear(replay)) {
		const ClearRect clear = { .rect = whole(framebuffer),
			.depth = replay->clear_depth,
			.stencil = replay->clear_stencil,
			.clears_depth = true,
			.clears_stencil = true };
		v4x_blit_clear(&queue->blit, &clear, count, framebuffer->width, framebuffer->height);
	}
	if (draws_depth_back(replay)) {
		if (!stencil_logged)
			xsdk_log_write("vulkan4xenon: depth past eDRAM in bands comes back without its stencil");
		stencil_logged = true;
		v4x_blit_draw_depth(&queue->blit, &framebuffer->depth->texture, framebuffer->width, framebuffer->height, count);
		framebuffer->depth->used_serial = replay->serial;
	} else if (render_pass->has_depth && render_pass->loads_depth && !replay->clears_depth) {
		check_depth_kept(queue, replay);
	}
	for (uint32_t i = 0; i < count; i++) {
		if (render_pass->load_ops[i] != VK_ATTACHMENT_LOAD_OP_LOAD)
			continue;
		v4x_blit_draw(&queue->blit, &framebuffer->attachments[i]->texture, framebuffer->width, framebuffer->height,
			&replay->area, i, count);
		framebuffer->attachments[i]->used_serial = replay->serial;
	}
}

// A subpass past eDRAM's tiles is a tiled pass (xsdk:include/xsdk/gpu.h): its commands are replayed once into a
// recording that each band of rows runs.
static void begin_tiled_pass(VkQueue queue, Replay *replay)
{
	const Framebuffer *const framebuffer = replay->framebuffer;
	TileRecording *const recording = framebuffer->recording;
	// The GPU may still be running what the subpass recorded last.
	const NTSTATUS waited = recording->fence ? xsdk_gpu_wait(recording->fence, IDLE_TIMEOUT_MS) : STATUS_SUCCESS;
	if (!NT_SUCCESS(waited))
		xsdk_log_write("vulkan4xenon: a pass past eDRAM's last recording didn't finish: 0x%08X", (unsigned)waited);
	keep_tiled_depth(queue, replay);
	// Resolves clear depth in bands to it, and the hierarchical buffer takes it as each band's.
	if (replay->clears_depth && !framebuffer->tiling.depth_whole)
		set_depth_clear(queue, replay->clear_depth, replay->clear_stencil);
	const NTSTATUS status = xsdk_gpu_begin_tiles(&framebuffer->tiling, &recording->buffer);
	if (!NT_SUCCESS(status))
		xsdk_log_write("vulkan4xenon: xsdk_gpu_begin_tiles failed: 0x%08X", (unsigned)status);
	replay->tiling = true;
	load_tiles(queue, replay);
	replay->pipeline_dirty = true;
	replay->clip_planes_on = false;
	memset(replay->pointed_streams, 0, sizeof replay->pointed_streams);
	memset(replay->pointed_textures, 0, sizeof replay->pointed_textures);
	// A band begins with the loop constants the last band left, so the recording writes its own.
	queue->loops_written = false;
}

// Each colour attachment is resolved a band at a time, which clears it to its clear for the next band, one drawn back
// left out of band 0's clear; so is depth, as resolves_depth says. eDRAM then holds the last band, which no untiled
// pass uses, and a whole depth at the top.
static void end_tiled_pass(VkQueue queue, Replay *replay)
{
	const RenderPass *const render_pass = replay->render_pass;
	const Framebuffer *const framebuffer = replay->framebuffer;
	XsdkTileResolve resolves[V4X_MAX_COLOR_ATTACHMENTS + 1];
	uint32_t count = 0;
	for (uint32_t i = 0; i < render_pass->color_count; i++) {
		Image *const image = framebuffer->attachments[i];
		resolves[count] = (XsdkTileResolve){ .target = i,
			.texture = &image->texture,
			.drawn_back = render_pass->load_ops[i] == VK_ATTACHMENT_LOAD_OP_LOAD };
		memcpy(resolves[count++].clear_rgba, replay->clears->colors[i], sizeof resolves[0].clear_rgba);
		image->used_serial = replay->serial;
	}
	Image *const depth = framebuffer->depth;
	if (resolves_depth(render_pass, framebuffer)) {
		resolves[count++] = (XsdkTileResolve){
			.target = XSDK_TILE_DEPTH, .texture = &depth->texture, .drawn_back = draws_depth_back(replay)
		};
		depth->used_serial = replay->serial;
	}
	replay->tiling = false;
	if (!NT_SUCCESS(xsdk_gpu_end_tiles(resolves, count)))
		(void)v4x_host_unsupported("a pass past eDRAM whose commands outgrow its recording's 1 MB");
	framebuffer->recording->fence = xsdk_gpu_fence();
	queue->depths[0] = render_pass->has_depth && framebuffer->tiling.depth_whole
		? holding(depth, replay->depth_tile, whole(framebuffer))
		: (EdramPart){ .contents = EDRAM_UNKNOWN };
	replay->pipeline_dirty = true;
	replay->clip_planes_on = false;
	memset(replay->pointed_streams, 0, sizeof replay->pointed_streams);
	memset(replay->pointed_textures, 0, sizeof replay->pointed_textures);
}

// The next render pass in the command buffer after this command, if there is one.
static const Command *next_render_pass(VkCommandBuffer buffer, uint32_t after)
{
	for (uint32_t c = after + 1; c < buffer->count; c++) {
		if (buffer->commands[c].type == COMMAND_BEGIN_RENDER_PASS)
			return &buffer->commands[c];
	}
	return NULL;
}

// The blit is drawn into eDRAM at its destination's size, a target in its format from tile 0, outside any render pass,
// so the next one finds that target holding the destination over the rectangle, and the colour targets and depths it
// reached lost. The replay doesn't wait for it.
static void draw_blit(VkQueue queue, Replay *replay, const DrawnBlit *blit)
{
	Image *const destination = blit->destination;
	const VkRect2D *const rect = &blit->rect;
	const XsdkColorTarget target = { .format = XSDK_TARGET_8_8_8_8, .tile = 0 };
	const uint32_t tiles = xsdk_gpu_target_tiles(destination->width, destination->height);
	for (uint32_t i = 0; i < V4X_EDRAM_DEPTHS; i++) {
		if (part_overlaps(&queue->depths[i], 0, tiles))
			queue->depths[i] = (EdramPart){ .contents = EDRAM_UNKNOWN };
	}
	const VkRect2D stored = stored_area(&queue->colors[0], destination, rect);
	for (uint32_t i = 0; i < V4X_MAX_COLOR_ATTACHMENTS; i++) {
		if (queue->colors[i].tile < tiles)
			queue->colors[i] = (EdramPart){ .contents = EDRAM_UNKNOWN };
	}
	const uint32_t depth_tile = XSDK_EDRAM_TILES - xsdk_gpu_target_tiles(destination->width, destination->height);
	xsdk_gpu_set_targets(destination->width, destination->height, &target, 1, depth_tile);
	// The target set turned the clip planes off.
	replay->clip_planes_on = false;
	v4x_blit_region(&queue->blit, blit);
	xsdk_gpu_set_resolve_clears(false, false);
	xsdk_gpu_resolve_texture_rect(&destination->texture, (uint32_t)rect->offset.x, (uint32_t)rect->offset.y,
		rect->extent.width, rect->extent.height, 0);
	blit->source->used_serial = replay->serial;
	destination->used_serial = replay->serial;
	queue->colors[0] = holding(destination, 0, stored);
	queue->colors[0].format = XSDK_TARGET_8_8_8_8;
	// The draw and the resolve point streams, slots and shaders of their own, as a pass's begin does.
	replay->pipeline_dirty = true;
	memset(replay->pointed_streams, 0, sizeof replay->pointed_streams);
	memset(replay->pointed_textures, 0, sizeof replay->pointed_textures);
}

// A render pass's area, in its command buffer's data.
static const VkRect2D *area_of(VkCommandBuffer buffer, const Command *begin)
{
	return (const VkRect2D *)(buffer->data + begin->begin.area_at);
}

// The pass ends before the next one in the command buffer begins, which its resolves clear for.
static void end_pass_command(VkQueue queue, VkCommandBuffer buffer, Replay *replay, uint32_t index)
{
	if (replay->framebuffer->tiled) {
		end_tiled_pass(queue, replay);
		return;
	}
	const Command *const next = next_render_pass(buffer, index);
	const PassClears *const next_clears = next ? (const PassClears *)(buffer->data + next->begin.clears_at) : NULL;
	end_render_pass(queue, replay, next, next_clears, next ? area_of(buffer, next) : NULL);
}

static void prepare_draw(Replay *replay)
{
	const Pipeline *const pipeline = replay->pipeline;
	replay->queue->drawn = true;
	if (replay->pipeline_dirty) {
		apply_pipeline(pipeline, replay);
	} else {
		if (replay->viewport_dirty)
			apply_viewport(pipeline, replay);
		if (replay->dynamic_dirty)
			apply_dynamic(pipeline, replay);
	}
	replay->pipeline_dirty = false;
	replay->viewport_dirty = false;
	replay->dynamic_dirty = false;
	apply_resources(pipeline, replay);
	apply_clip_planes(pipeline, replay);
}

// xsdk draws from vertex 0, so each stream at the vertex's rate starts at the draw's first vertex, which may come
// before the binding's, and each at the instance's rate at the instance's element.
static void point_streams(Replay *replay, int32_t first_vertex, uint32_t instance)
{
	const Pipeline *const pipeline = replay->pipeline;
	for (uint32_t s = 0; s < pipeline->stream_count; s++) {
		const VertexStream *const source = &pipeline->streams[s];
		const VertexBinding *const binding = &replay->bindings[source->binding];
		const uint32_t skipped = source->per_instance ? instance * source->stride_bytes
													  : (uint32_t)(first_vertex * (int32_t)source->stride_bytes);
		const VertexBinding stream = { .address = binding->address + skipped,
			.size_bytes = binding->size_bytes - skipped };
		if (replay->pointed_streams[s].address == stream.address &&
			replay->pointed_streams[s].size_bytes == stream.size_bytes)
			continue;
		xsdk_gpu_set_vertices(s, stream.address, stream.size_bytes);
		replay->pointed_streams[s] = stream;
	}
}

// The instance's index in every component, for a vertex shader that reads gl_InstanceIndex (xost:docs/xsc.md,
// "Interface conventions"); a float holds it exactly up to 2^24.
static void write_instance_index(const Pipeline *pipeline, uint32_t instance)
{
	if (!pipeline->reads_instance_index)
		return;
	const float index = (float)instance;
	const float values[4] = { index, index, index, index };
	xsdk_gpu_set_vertex_constants(pipeline->instance_index_constant, values, 1);
}

// One instance, in the pieces its pipeline splits a draw into (Pipeline, piece_vertices).
static void draw_instance(Replay *replay, uint32_t vertex_count, uint32_t first_vertex, uint32_t instance)
{
	const Pipeline *const pipeline = replay->pipeline;
	write_instance_index(pipeline, instance);
	for (uint32_t start = 0;; start += pipeline->piece_step) {
		point_streams(replay, (int32_t)(first_vertex + start), instance);
		const uint32_t left = vertex_count - start;
		xsdk_gpu_draw(pipeline->primitive, left < pipeline->piece_vertices ? left : pipeline->piece_vertices);
		if (left <= pipeline->piece_vertices)
			break;
	}
}

static void draw_indexed_instance(
	Replay *replay, uint32_t count, uint32_t first_index, int32_t vertex_offset, uint32_t instance)
{
	const Pipeline *const pipeline = replay->pipeline;
	write_instance_index(pipeline, instance);
	point_streams(replay, vertex_offset, instance);
	const uint32_t index_bytes = replay->index_32_bit ? 4 : 2;
	for (uint32_t start = 0;; start += pipeline->piece_step) {
		const uint32_t address = replay->index_address + (first_index + start) * index_bytes;
		const uint32_t left = count - start;
		const uint32_t indices = left < pipeline->piece_vertices ? left : pipeline->piece_vertices;
		xsdk_gpu_draw_indexed(pipeline->primitive, address, indices, replay->index_32_bit);
		if (left <= pipeline->piece_vertices)
			break;
	}
}

// What a draw refuses as it's recorded, one recorded where its pipeline wasn't bound, or an indirect one, refuses as
// it's replayed, drawing nothing.
static bool drawable(const Replay *replay, uint32_t count, uint32_t instances)
{
	if (!count || !instances)
		return false;
	if (count > V4X_MAX_DRAW_VERTICES && !replay->pipeline->piece_step) {
		(void)v4x_host_unsupported("triangle fans of more than 65535 vertices");
		return false;
	}
	return true;
}

// A draw an instance: the simple way, before a shader that fetches at an index it works out (docs/ROADMAP.md,
// milestone 24).
static void draw(
	Replay *replay, uint32_t vertex_count, uint32_t first_vertex, uint32_t instance_count, uint32_t first_instance)
{
	if (!drawable(replay, vertex_count, instance_count))
		return;
	prepare_draw(replay);
	for (uint32_t i = 0; i < instance_count; i++)
		draw_instance(replay, vertex_count, first_vertex, first_instance + i);
}

static void draw_indexed(Replay *replay, uint32_t count, uint32_t first_index, int32_t vertex_offset,
	uint32_t instance_count, uint32_t first_instance)
{
	if (!drawable(replay, count, instance_count))
		return;
	prepare_draw(replay);
	for (uint32_t i = 0; i < instance_count; i++)
		draw_indexed_instance(replay, count, first_index, vertex_offset, first_instance + i);
}

// Its parameters are read now, in order with the replay: the host wrote them before the submit, or a copy in it did.
// They're a VkDrawIndirectCommand: vertexCount, instanceCount, firstVertex, firstInstance.
static void draw_indirect(Replay *replay, volatile const uint32_t *parameters)
{
	draw(replay, parameters[0], parameters[2], parameters[1], parameters[3]);
}

// The same with a VkDrawIndexedIndirectCommand: indexCount, instanceCount, firstIndex, vertexOffset, firstInstance.
static void draw_indexed_indirect(Replay *replay, volatile const uint32_t *parameters)
{
	draw_indexed(replay, parameters[0], parameters[2], (int32_t)parameters[3], parameters[1], parameters[4]);
}

// A stencil mask or reference, for each face the command names.
static void set_stencil(DynamicState *dynamic, const Command *command)
{
	uint8_t *values = dynamic->stencil_reference;
	if (command->stencil.state == VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK)
		values = dynamic->stencil_compare_mask;
	else if (command->stencil.state == VK_DYNAMIC_STATE_STENCIL_WRITE_MASK)
		values = dynamic->stencil_write_mask;
	for (uint32_t face = 0; face < V4X_STENCIL_FACES; face++) {
		if (command->stencil.faces & 1u << face)
			values[face] = command->stencil.value;
	}
}

static void set_dynamic(Replay *replay, const Command *command)
{
	DynamicState *const dynamic = &replay->dynamic;
	if (command->type == COMMAND_SET_DEPTH_BIAS) {
		dynamic->depth_bias_constant = command->depth_bias.constant;
		dynamic->depth_bias_slope = command->depth_bias.slope;
	} else if (command->type == COMMAND_SET_BLEND_CONSTANTS) {
		memcpy(dynamic->blend_constants, command->blend_constants, sizeof dynamic->blend_constants);
	} else {
		set_stencil(dynamic, command);
	}
	replay->dynamic_dirty = true;
}

// A pass over the whole framebuffer clears to its depth image's own clear unless it clears to its own; a smaller one
// draws the image's clear over the whole first, then clears its area to its own.
static void begin_command(VkQueue queue, VkCommandBuffer buffer, Replay *replay, const Command *command)
{
	replay->render_pass = command->begin.render_pass;
	replay->framebuffer = command->begin.framebuffer;
	replay->area = *area_of(buffer, command);
	replay->clears = (const PassClears *)(buffer->data + command->begin.clears_at);
	replay->clear_depth = replay->clears->depth;
	replay->clear_stencil = replay->clears->stencil;
	replay->clears_depth = replay->render_pass->clears_depth;
	Image *const depth = replay->render_pass->has_depth ? replay->framebuffer->depth : NULL;
	replay->draws_image_clear = depth && depth->clear_pending && !whole_area(replay);
	if (depth && depth->clear_pending && !replay->clears_depth && whole_area(replay)) {
		replay->clears_depth = true;
		replay->clear_depth = depth->pending_depth;
		replay->clear_stencil = depth->pending_stencil;
	}
	if (depth)
		depth->clear_pending = false;
	if (replay->framebuffer->tiled)
		begin_tiled_pass(queue, replay);
	else
		begin_render_pass(queue, replay);
}

// A depth image's depth lives in eDRAM, so its clear waits for the next pass to load it; a sampled one's texture
// takes the clear now too, for a shader to sample.
static void clear_depth_image(VkQueue queue, const Command *command)
{
	Image *const image = command->depth_clear.image;
	image->clear_pending = true;
	image->pending_depth = command->depth_clear.depth;
	image->pending_stencil = command->depth_clear.stencil;
	if (image->is_sampled_depth)
		v4x_transfer_fill_depth(queue, image, command->depth_clear.depth, command->depth_clear.stencil);
}

// The clear draws with the layer's own shaders and state, and its rectangle in stream 0, so they're set again after;
// no clip plane cuts it, and an occlusion query open doesn't count it: the query's id ends before it, and its next id
// begins after, in the same status word.
// shortcut: a query's clears past its 31st count toward it, its ids used up; give it more words if a port does that.
static void clear_attachments(VkQueue queue, Replay *replay, const ClearRect *clear)
{
	const bool pauses_query = replay->query_address && replay->visibility_id + 1 < VISIBILITY_WORD_IDS;
	if (pauses_query)
		xsdk_gpu_end_visibility(replay->visibility_id, replay->query_address);
	if (replay->clip_planes_on)
		xsdk_gpu_set_clip_planes(0, NULL);
	replay->clip_planes_on = false;
	v4x_blit_clear(
		&queue->blit, clear, replay->render_pass->color_count, replay->framebuffer->width, replay->framebuffer->height);
	replay->pipeline_dirty = true;
	replay->pointed_streams[0] = (VertexBinding){ 0 };
	if (pauses_query)
		xsdk_gpu_begin_visibility(++replay->visibility_id);
}

// The GPU waits for each event's word, then its caches take what the host wrote before it set one.
// shortcut: what the replay reads on the CPU at vkQueueSubmit (copies, indirect draws' parameters, uniform buffers the
// GPU can't load) doesn't wait for an event the host sets later; hold that work back for the event if a port needs it.
static void wait_events(VkCommandBuffer buffer, const Command *command)
{
	for (uint32_t i = 0; i < command->wait_events.count; i++) {
		VkEvent event;
		memcpy(&event, buffer->data + command->wait_events.events_at + i * sizeof event, sizeof event);
		xsdk_gpu_wait_word(((const Event *)v4x_object(event))->address, 1);
	}
	xsdk_gpu_invalidate(0, ALL_MEMORY_BYTES);
}

// A tiled pass refuses queries and time stamps as they're recorded (command_buffer.c), but a secondary command buffer
// doesn't know its pass, so its are refused as they're replayed.
static bool refused_in_tiles(const Replay *replay, const char *what)
{
	if (replay->tiling)
		(void)v4x_host_unsupported(what);
	return replay->tiling;
}

static void write_timestamp(const Replay *replay, const Command *command)
{
	if (refused_in_tiles(replay, "time stamps in a render pass past eDRAM"))
		return;
	QueryPool *const pool = command->timestamp.pool;
	const uint32_t slot_bytes = V4X_QUERY_SLOT_WORDS * sizeof(uint32_t);
	xsdk_gpu_write_clock(pool->address + command->timestamp.query * slot_bytes, command->timestamp.after_work);
	pool->used_serial = replay->serial;
}

// Occlusion queries begin and end inside a render pass (vkCmdBeginQuery), where the layer's only draws are clears,
// which clear_attachments keeps out of them. Each takes visibility ids from 0: an id's end copies its status word once
// the GPU is idle, before anything after it can begin the id again (xsdk:include/xsdk/gpu.h), so a submission takes
// any number of queries, a wait for the GPU each.
static void begin_query(Replay *replay, const Command *command)
{
	if (refused_in_tiles(replay, "queries in a render pass past eDRAM"))
		return;
	QueryPool *const pool = command->queries.pool;
	replay->query_address = pool->address + command->queries.first * V4X_QUERY_SLOT_WORDS * sizeof(uint32_t);
	replay->visibility_id = 0;
	xsdk_gpu_begin_visibility(0);
	pool->used_serial = replay->serial;
}

// The status word, then the last id the query took, written after it as a fence is (query.c). An end whose begin was
// refused as it was recorded has nothing to end.
static void end_query(Replay *replay)
{
	if (!replay->query_address)
		return;
	xsdk_gpu_end_visibility(replay->visibility_id, replay->query_address);
	xsdk_gpu_write_word(replay->query_address + V4X_QUERY_LAST_WORD * sizeof(uint32_t), replay->visibility_id);
	replay->query_address = 0;
}

static void replay_command(VkQueue queue, VkCommandBuffer buffer, Replay *replay, uint32_t index)
{
	const Command *const command = &buffer->commands[index];
	switch (command->type) {
	case COMMAND_BEGIN_RENDER_PASS:
		begin_command(queue, buffer, replay, command);
		break;
	case COMMAND_END_RENDER_PASS:
		end_pass_command(queue, buffer, replay, index);
		break;
	case COMMAND_BIND_PIPELINE:
		replay->pipeline = command->pipeline;
		replay->pipeline_dirty = true;
		break;
	case COMMAND_BIND_VERTEX_BUFFER:
		replay->bindings[command->vertex_buffer.binding] = (VertexBinding){ .address = command->vertex_buffer.address,
			.size_bytes = command->vertex_buffer.size_bytes };
		break;
	case COMMAND_DRAW:
		draw(replay, command->draw.vertex_count, command->draw.first_vertex, command->draw.instance_count,
			command->draw.first_instance);
		break;
	case COMMAND_BIND_DESCRIPTOR_SET:
		replay->sets[command->descriptor_set.index] = command->descriptor_set.set;
		replay->dynamic_offsets[command->descriptor_set.index] =
			(const uint32_t *)(buffer->data + command->descriptor_set.offsets_at);
		break;
	case COMMAND_PUSH_CONSTANTS:
		memcpy((uint8_t *)replay->push + command->push_constants.offset_bytes,
			buffer->data + command->push_constants.data_at, command->push_constants.size_bytes);
		break;
	case COMMAND_COPY_BUFFER_TO_IMAGE:
		v4x_transfer_buffer_to_image(queue, (const LevelCopy *)(buffer->data + command->state_at));
		break;
	case COMMAND_COPY_IMAGE_TO_BUFFER:
		v4x_transfer_image_to_buffer(queue, (const LevelCopy *)(buffer->data + command->state_at));
		break;
	case COMMAND_COPY_IMAGE:
		v4x_transfer_copy_image(queue, command);
		break;
	case COMMAND_BLIT_IMAGE:
		v4x_transfer_blit(queue, command);
		break;
	case COMMAND_DRAW_BLIT:
		draw_blit(queue, replay, (const DrawnBlit *)(buffer->data + command->state_at));
		break;
	case COMMAND_CLEAR_COLOR_IMAGE:
		v4x_transfer_clear(queue, command);
		break;
	case COMMAND_COPY_BUFFER:
		v4x_transfer_copy_buffer(queue, command, command->buffer_copy.source);
		break;
	case COMMAND_UPDATE_BUFFER:
		v4x_transfer_copy_buffer(queue, command, buffer->data + command->buffer_copy.data);
		break;
	case COMMAND_FILL_BUFFER:
		v4x_transfer_fill_buffer(queue, command);
		break;
	case COMMAND_SET_VIEWPORT:
		memcpy(&replay->viewport, buffer->data + command->state_at, sizeof replay->viewport);
		replay->viewport_dirty = true;
		break;
	case COMMAND_SET_SCISSOR:
		memcpy(&replay->scissor, buffer->data + command->state_at, sizeof replay->scissor);
		replay->viewport_dirty = true;
		break;
	case COMMAND_SET_STENCIL:
	case COMMAND_SET_DEPTH_BIAS:
	case COMMAND_SET_BLEND_CONSTANTS:
		set_dynamic(replay, command);
		break;
	case COMMAND_CLEAR_ATTACHMENTS:
		clear_attachments(queue, replay, (const ClearRect *)(buffer->data + command->state_at));
		break;
	case COMMAND_CLEAR_DEPTH_IMAGE:
		clear_depth_image(queue, command);
		break;
	// In place, with the state the replay has bound; a secondary command buffer begins and ends no render pass.
	case COMMAND_EXECUTE_COMMANDS:
		for (uint32_t c = 0; c < command->secondary->count; c++)
			replay_command(queue, command->secondary, replay, c);
		break;
	case COMMAND_BIND_INDEX_BUFFER:
		replay->index_address = command->index_buffer.address;
		replay->index_32_bit = command->index_buffer.is_32_bit;
		break;
	case COMMAND_DRAW_INDEXED:
		draw_indexed(replay, command->draw_indexed.index_count, command->draw_indexed.first_index,
			command->draw_indexed.vertex_offset, command->draw_indexed.instance_count,
			command->draw_indexed.first_instance);
		break;
	case COMMAND_DRAW_INDIRECT:
		draw_indirect(replay, command->parameters);
		break;
	case COMMAND_DRAW_INDEXED_INDIRECT:
		draw_indexed_indirect(replay, command->parameters);
		break;
	case COMMAND_WRITE_EVENT:
		xsdk_gpu_write_word(command->event.address, command->event.value);
		break;
	case COMMAND_WAIT_EVENTS:
		wait_events(buffer, command);
		break;
	case COMMAND_RESET_QUERIES:
		v4x_transfer_reset_queries(queue, command);
		break;
	case COMMAND_WRITE_TIMESTAMP:
		write_timestamp(replay, command);
		break;
	case COMMAND_COPY_QUERY_RESULTS:
		v4x_transfer_copy_queries(queue, (const QueryCopy *)(buffer->data + command->state_at));
		break;
	case COMMAND_BEGIN_QUERY:
		begin_query(replay, command);
		break;
	case COMMAND_END_QUERY:
		end_query(replay);
		break;
	}
}

VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit(
	VkQueue queue, uint32_t submitCount, const VkSubmitInfo *pSubmits, VkFence fence)
{
	// Drain the CPU's write-combining, so the GPU reads what the app wrote to mapped memory.
	__asm__ volatile("sync" ::: "memory");
	// Mapped memory is coherent, so the app may have written any of it; one invalidate covers it all, as xsdk's set-up
	// covers memory (xsdk:src/gpu_setup.c).
	xsdk_gpu_invalidate(0, ALL_MEMORY_BYTES);
	queue->serial = queue->serial + 1 ? queue->serial + 1 : 1;
	for (uint32_t i = 0; i < submitCount; i++) {
		for (uint32_t j = 0; j < pSubmits[i].commandBufferCount; j++) {
			const VkCommandBuffer buffer = pSubmits[i].pCommandBuffers[j];
			Replay replay = { .queue = queue, .serial = queue->serial, .pipeline_dirty = true };
			for (uint32_t c = 0; c < buffer->count; c++)
				replay_command(queue, buffer, &replay, c);
		}
	}
	// One queue replays in order, so semaphores need nothing; the fence submits the ring either way.
	const uint32_t submitted = xsdk_gpu_fence();
	queue->submit_fences[queue->serial % V4X_SUBMIT_FENCES] = submitted;
	v4x_sync_submitted(v4x_object(fence), submitted);
	return VK_SUCCESS;
}

// The queue family has no sparse binding, as the GPU addresses physical memory (docs/SUPPORT.md, Device).
VKAPI_ATTR VkResult VKAPI_CALL vkQueueBindSparse(
	VkQueue queue, uint32_t bindInfoCount, const VkBindSparseInfo *pBindInfo, VkFence fence)
{
	(void)queue;
	(void)bindInfoCount;
	(void)pBindInfo;
	(void)fence;
	return v4x_host_unsupported("sparse binding");
}

VKAPI_ATTR VkResult VKAPI_CALL vkQueueWaitIdle(VkQueue queue)
{
	const NTSTATUS status = xsdk_gpu_wait(xsdk_gpu_fence(), IDLE_TIMEOUT_MS);
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("vulkan4xenon: the GPU didn't go idle: 0x%08X", (unsigned)status);
		return VK_ERROR_DEVICE_LOST;
	}
	queue->drawn = false;
	return VK_SUCCESS;
}
