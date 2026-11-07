#include <assert.h>
#include <string.h>

#include <vulkan/vulkan.h>

#include "blit.h"
#include "host.h"
#include "objects.h"
#include "profile.h"
#include "record.h"
#include "sync.h"
#include "transfer.h"
#include "xsdk/gpu.h"
#include "xsdk/log.h"
#include "xsdk/texture.h"

// The submit: each command buffer's commands replayed in order, render passes and copies on the CPU and through xsdk as
// eDRAM and the GPU stand then, and the runs of packets its draws recorded copied in between (record.c).

enum {
	IDLE_TIMEOUT_MS = 10000,
	// All of physical memory, as libxenon invalidates it (libxenon:drivers/xenos/xe.c:992).
	ALL_MEMORY_BYTES = 0x20000000,
	// The visibility ids one status word holds (xsdk:include/xsdk/gpu.h).
	VISIBILITY_WORD_IDS = XSDK_VISIBILITY_IDS / 2,
	// The commands after a pass's end looked through for what reads its image before its next pass (resolve_can_wait).
	RESOLVE_SCAN_COMMANDS = 256,
};

// What one replay has begun so far.
typedef struct {
	VkQueue queue;
	// The submission it belongs to (VkQueue_T's serial).
	uint32_t serial;
	// User clip planes may be on: a run may have left them so, and a target set turns them off.
	bool clip_planes_on;
	const RenderPass *render_pass;
	const Framebuffer *framebuffer;
	// The render area, which the pass clears, loads and stores alone.
	VkRect2D area;
	// Where the pass's colour targets went (place_colors): the framebuffer's, moved up from tile 0 to color_tile.
	uint32_t color_tile;
	XsdkColorTarget targets[V4X_MAX_COLOR_ATTACHMENTS];
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
	// The occlusion query open, Vulkan's one at a time: its slot's GPU address, 0 for none, and the visibility id its
	// draws go to now, from 0 (begin_query).
	uint32_t query_address;
	uint32_t visibility_id;
	// In a tiled pass, whose commands go into its recording (begin_tiled_pass).
	bool tiling;
} Replay;

// What render passes' begins did to eDRAM, for the tests; the queue's external synchronization guards it.
static V4xEdramCounts edram_counts;

// Resolves clear eDRAM's depth and stencil to what xsdk_gpu_set_depth_clear last took.
static void set_depth_clear(VkQueue queue, float depth, uint32_t stencil)
{
	if (queue->depth_clear == depth && queue->stencil_clear == stencil)
		return;
	xsdk_gpu_set_depth_clear(depth, (uint8_t)stencil);
	queue->depth_clear = depth;
	queue->stencil_clear = stencil;
}

// The colour targets where place_colors put them, and depth where place_depth put it (docs/ROADMAP.md, milestones 4,
// 14 and 25), at the pass's samples.
static void set_target(const Replay *replay)
{
	const Framebuffer *const framebuffer = replay->framebuffer;
	xsdk_gpu_set_targets_samples(framebuffer->width, framebuffer->height, framebuffer->samples, replay->targets,
		replay->render_pass->color_count, replay->depth_tile);
}

// Colour target i alone where it sits, to draw an image back into.
static void set_one_target(const Replay *replay, uint32_t i)
{
	const Framebuffer *const framebuffer = replay->framebuffer;
	xsdk_gpu_set_targets_samples(
		framebuffer->width, framebuffer->height, framebuffer->samples, &replay->targets[i], 1, replay->depth_tile);
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

// Laid out alike: the same size and samples.
static bool same_size(const EdramPart *part, const Framebuffer *framebuffer)
{
	return part->width == framebuffer->width && part->height == framebuffer->height &&
		part->samples == framebuffer->samples;
}

static EdramPart cleared_color(const Replay *replay, uint32_t target, const float rgba[4], VkRect2D area)
{
	const Framebuffer *const framebuffer = replay->framebuffer;
	EdramPart part = { .contents = EDRAM_CLEARED,
		.width = framebuffer->width,
		.height = framebuffer->height,
		.samples = framebuffer->samples,
		.tile = replay->targets[target].tile,
		.format = replay->targets[target].format,
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
		.samples = framebuffer->samples,
		.depth = depth,
		.stencil = stencil,
		.tile = tile,
		.area = area };
}

static EdramPart holding(Image *image, uint32_t tile, VkRect2D area)
{
	return (EdramPart){ .contents = EDRAM_IMAGE,
		.width = image->width,
		.height = image->height,
		.samples = image->samples,
		.image = image,
		.tile = tile,
		.area = area };
}

static EdramPart holding_color(const Replay *replay, uint32_t target, VkRect2D area)
{
	EdramPart part = holding(replay->framebuffer->attachments[target], replay->targets[target].tile, area);
	part.format = replay->targets[target].format;
	return part;
}

// What eDRAM holds where a colour target goes: what a pass left at that tile in that format, or nothing known, as
// anything else there may be part of another target.
static EdramPart held_part(VkQueue queue, const XsdkColorTarget *wanted)
{
	for (uint32_t i = 0; i < V4X_EDRAM_COLORS; i++) {
		const EdramPart *const part = &queue->colors[i];
		if (part->contents != EDRAM_UNKNOWN && part->tile == wanted->tile && part->format == wanted->format)
			return *part;
	}
	return (EdramPart){ .contents = EDRAM_UNKNOWN };
}

static EdramPart held_color(VkQueue queue, const Replay *replay, uint32_t target)
{
	return held_part(queue, &replay->targets[target]);
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
		first < part->tile + xsdk_gpu_target_sample_tiles(part->width, part->height, part->samples);
}

static bool ranges_overlap(uint32_t first, uint32_t tiles, uint32_t other_first, uint32_t other_tiles)
{
	return first < other_first + other_tiles && other_first < first + tiles;
}

// Whether tiles first to first + tiles miss the pass's colour and every depth image eDRAM holds.
static bool depth_tiles_free(VkQueue queue, uint32_t first, uint32_t tiles, uint32_t color_first, uint32_t color_tiles)
{
	if (ranges_overlap(first, tiles, color_first, color_tiles))
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
static uint32_t new_depth_tile(VkQueue queue, uint32_t tiles, uint32_t color_first, uint32_t color_tiles)
{
	const uint32_t top = XSDK_EDRAM_TILES - tiles;
	if (depth_tiles_free(queue, top, tiles, color_first, color_tiles))
		return top;
	for (uint32_t i = 0; i < V4X_EDRAM_DEPTHS; i++) {
		const EdramPart *const part = &queue->depths[i];
		if (part->contents == EDRAM_IMAGE && part->tile >= tiles &&
			depth_tiles_free(queue, part->tile - tiles, tiles, color_first, color_tiles))
			return part->tile - tiles;
	}
	return top;
}

// The tiles a depth at the framebuffer's size takes.
static uint32_t depth_tiles(const Framebuffer *framebuffer)
{
	return xsdk_gpu_target_sample_tiles(framebuffer->width, framebuffer->height, framebuffer->samples);
}

// Where the pass's depth goes with its colour from color_first: where eDRAM holds its image's, or the clear the pass
// wants at its size, in part; else new tiles, part V4X_EDRAM_DEPTHS.
static uint32_t choose_depth(VkQueue queue, const Replay *replay, uint32_t color_first, uint32_t *part)
{
	const RenderPass *const render_pass = replay->render_pass;
	const Framebuffer *const framebuffer = replay->framebuffer;
	const uint32_t color_tiles = framebuffer->color_tiles;
	*part = V4X_EDRAM_DEPTHS;
	for (uint32_t i = 0; i < V4X_EDRAM_DEPTHS && render_pass->has_depth; i++) {
		const EdramPart *const depth = &queue->depths[i];
		// The pass's colour targets would draw over it.
		if (part_overlaps(depth, color_first, color_tiles))
			continue;
		// Another size's depth lies at another pitch, and may not fit where it is.
		if ((depth->contents == EDRAM_IMAGE && depth->image == framebuffer->depth && same_size(depth, framebuffer)) ||
			(*part == V4X_EDRAM_DEPTHS && replay->clears_depth && depth_ready(depth, replay)))
			*part = i;
	}
	if (*part < V4X_EDRAM_DEPTHS)
		return queue->depths[*part].tile;
	return new_depth_tile(queue, depth_tiles(framebuffer), color_first, render_pass->has_depth ? color_tiles : 0);
}

// Places the pass's depth as choose_depth says, its colour placed. Whatever depth the pass's colour or depth then
// reaches is lost, as colour drawn over it overwrites it.
static void place_depth(VkQueue queue, Replay *replay)
{
	const RenderPass *const render_pass = replay->render_pass;
	const Framebuffer *const framebuffer = replay->framebuffer;
	const uint32_t tiles = depth_tiles(framebuffer);
	uint32_t part;
	const uint32_t tile = choose_depth(queue, replay, replay->color_tile, &part);
	for (uint32_t i = 0; i < V4X_EDRAM_DEPTHS; i++) {
		if (i != part &&
			(part_overlaps(&queue->depths[i], replay->color_tile, framebuffer->color_tiles) ||
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

// The tiles a colour part takes, at its format's size.
static uint32_t color_part_tiles(const EdramPart *part)
{
	return xsdk_gpu_color_target_sample_tiles(part->width, part->height, part->format, part->samples);
}

static bool color_overlaps(const EdramPart *part, uint32_t first, uint32_t tiles)
{
	return part->contents != EDRAM_UNKNOWN && ranges_overlap(part->tile, color_part_tiles(part), first, tiles);
}

// Whether a part kept for target i saves its begin's work: the clear it clears to, the image it loads, or its own
// image, whose tiles another framebuffer's parts then keep.
static bool part_serves(const EdramPart *part, const Replay *replay, uint32_t i)
{
	if (replay->render_pass->load_ops[i] == VK_ATTACHMENT_LOAD_OP_CLEAR)
		return part->contents == EDRAM_CLEARED && !memcmp(part->rgba, replay->clears->colors[i], sizeof part->rgba);
	return part->contents == EDRAM_IMAGE && part->image == replay->framebuffer->attachments[i];
}

// Whether tiles first to first + tiles hold no colour or depth eDRAM keeps.
static bool tiles_unkept(VkQueue queue, uint32_t first, uint32_t tiles)
{
	for (uint32_t i = 0; i < V4X_EDRAM_COLORS; i++) {
		if (color_overlaps(&queue->colors[i], first, tiles))
			return false;
	}
	for (uint32_t i = 0; i < V4X_EDRAM_DEPTHS; i++) {
		if (part_overlaps(&queue->depths[i], first, tiles))
			return false;
	}
	return true;
}

// The lower of best and first, if the colour fits from first below room without reaching anything eDRAM keeps.
static uint32_t lower_unkept(VkQueue queue, uint32_t best, uint32_t first, uint32_t tiles, uint32_t room)
{
	return first < best && first + tiles <= room && tiles_unkept(queue, first, tiles) ? first : best;
}

// Where the pass's colour starts: where eDRAM keeps a part that serves one of its targets; else the lowest tiles,
// from 0 or the end of a part kept, that reach nothing kept and leave a new depth room at the top; else tile 0.
static uint32_t color_tile(VkQueue queue, const Replay *replay)
{
	const Framebuffer *const framebuffer = replay->framebuffer;
	const uint32_t tiles = framebuffer->color_tiles;
	for (uint32_t t = 0; t < replay->render_pass->color_count; t++) {
		const XsdkColorTarget *const target = &framebuffer->targets[t];
		for (uint32_t i = 0; i < V4X_EDRAM_COLORS; i++) {
			const EdramPart *const part = &queue->colors[i];
			if (part->format == target->format && same_size(part, framebuffer) && part->tile >= target->tile &&
				part->tile - target->tile + tiles <= XSDK_EDRAM_TILES && part_serves(part, replay, t))
				return part->tile - target->tile;
		}
	}
	const uint32_t room = XSDK_EDRAM_TILES - (replay->render_pass->has_depth ? depth_tiles(framebuffer) : 0);
	uint32_t best = lower_unkept(queue, UINT32_MAX, 0, tiles, room);
	for (uint32_t i = 0; i < V4X_EDRAM_COLORS; i++) {
		const EdramPart *const part = &queue->colors[i];
		if (part->contents != EDRAM_UNKNOWN)
			best = lower_unkept(queue, best, part->tile + color_part_tiles(part), tiles, room);
	}
	for (uint32_t i = 0; i < V4X_EDRAM_DEPTHS; i++) {
		const EdramPart *const part = &queue->depths[i];
		if (part->contents != EDRAM_UNKNOWN)
			best = lower_unkept(queue, best,
				part->tile + xsdk_gpu_target_sample_tiles(part->width, part->height, part->samples), tiles, room);
	}
	return best == UINT32_MAX ? 0 : best;
}

// Places the pass's colour targets as one, the framebuffer's layout moved up to color_tile, so a pass going back to a
// framebuffer finds eDRAM still holding it (xenia:src/xenia/gpu/registers.h:742, a target's base is any tile); where
// its depth would land on them, from tile 0, where it never does.
static void place_colors(VkQueue queue, Replay *replay)
{
	const Framebuffer *const framebuffer = replay->framebuffer;
	uint32_t first = color_tile(queue, replay);
	uint32_t part;
	const uint32_t depth = choose_depth(queue, replay, first, &part);
	if (replay->render_pass->has_depth &&
		ranges_overlap(depth, depth_tiles(framebuffer), first, framebuffer->color_tiles))
		first = 0;
	replay->color_tile = first;
	for (uint32_t i = 0; i < replay->render_pass->color_count; i++)
		replay->targets[i] =
			(XsdkColorTarget){ .format = framebuffer->targets[i].format, .tile = first + framebuffer->targets[i].tile };
}

// A colour part's contents eDRAM holds newer than its image, a pass's resolve put off, resolved into the image; the
// targets must be set again after.
static void resolve_part(EdramPart *part, uint32_t serial)
{
	static const float none[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	const XsdkColorTarget target = { .format = part->format, .tile = part->tile };
	const uint32_t depth_tile =
		XSDK_EDRAM_TILES - xsdk_gpu_target_sample_tiles(part->width, part->height, part->samples);
	xsdk_gpu_set_targets_samples(part->width, part->height, part->samples, &target, 1, depth_tile);
	xsdk_gpu_set_resolve_clears(false, false);
	xsdk_gpu_resolve_target(0, &part->image->texture, none);
	part->image->used_serial = serial;
	part->unresolved = false;
	edram_counts.late_resolves++;
}

// A colour part eDRAM is about to lose, resolved first if its resolve was put off; the targets must be set again after.
static void lose_part(EdramPart *part, uint32_t serial)
{
	if (part->unresolved)
		resolve_part(part, serial);
	*part = (EdramPart){ .contents = EDRAM_UNKNOWN };
}

static void lose_parts(VkQueue queue, uint32_t serial)
{
	for (uint32_t i = 0; i < V4X_EDRAM_COLORS; i++)
		lose_part(&queue->colors[i], serial);
}

// Whether the pass draws or resolves into an image: one of its colour attachments or their resolve attachments.
static bool pass_writes(const Replay *replay, const Image *image)
{
	for (uint32_t i = 0; i < replay->render_pass->color_count; i++) {
		if (replay->framebuffer->attachments[i] == image || replay->framebuffer->resolves[i] == image)
			return true;
	}
	return false;
}

// Whether a part is one the pass keeps as its own: its target i's image where it placed target i.
static bool own_part(const EdramPart *part, const Replay *replay)
{
	for (uint32_t i = 0; i < replay->render_pass->color_count; i++) {
		if (part->contents == EDRAM_IMAGE && part->image == replay->framebuffer->attachments[i] &&
			part->tile == replay->targets[i].tile && part->format == replay->targets[i].format)
			return true;
	}
	return false;
}

// The colour parts the pass's colour targets or depth reach are lost, as it draws over them, and so are those of the
// images it writes, which it holds afresh where it placed them; each resolved first if its resolve was put off, but
// for the pass's own, which begin_render_pass keeps.
static void forget_colors(VkQueue queue, const Replay *replay)
{
	const Framebuffer *const framebuffer = replay->framebuffer;
	const bool has_depth = replay->render_pass->has_depth;
	for (uint32_t i = 0; i < V4X_EDRAM_COLORS; i++) {
		EdramPart *const part = &queue->colors[i];
		if (!color_overlaps(part, replay->color_tile, framebuffer->color_tiles) &&
			!(has_depth && color_overlaps(part, replay->depth_tile, depth_tiles(framebuffer))) &&
			!(part->contents == EDRAM_IMAGE && pass_writes(replay, part->image)))
			continue;
		if (own_part(part, replay))
			*part = (EdramPart){ .contents = EDRAM_UNKNOWN };
		else
			lose_part(part, replay->serial);
	}
}

// The pass's parts first, target i's at i for its end; then the others kept, newest first, as many as fit, those that
// don't resolved if their resolves were put off. Returns whether one was, so the targets must be set again.
static bool keep_colors(VkQueue queue, const EdramPart *parts, uint32_t count, uint32_t serial)
{
	EdramPart kept[V4X_EDRAM_COLORS];
	uint32_t n = 0;
	bool resolved = false;
	for (; n < count; n++)
		kept[n] = parts[n];
	for (uint32_t i = 0; i < V4X_EDRAM_COLORS; i++) {
		EdramPart *const part = &queue->colors[i];
		if (part->contents == EDRAM_UNKNOWN)
			continue;
		if (n < V4X_EDRAM_COLORS) {
			kept[n++] = *part;
		} else {
			resolved = resolved || part->unresolved;
			lose_part(part, serial);
		}
	}
	for (; n < V4X_EDRAM_COLORS; n++)
		kept[n] = (EdramPart){ .contents = EDRAM_UNKNOWN };
	memcpy(queue->colors, kept, sizeof kept);
	return resolved;
}

// Depth eDRAM lost, which its image's texture doesn't hold either: its contents are undefined.
static void depth_undefined(void)
{
	static bool logged;
	if (!logged)
		xsdk_log_write("vulkan4xenon: a depth attachment was loaded after eDRAM lost it; its contents are undefined");
	logged = true;
}

// Whether eDRAM holds the pass's depth image's depth where the pass's depth goes.
static bool depth_kept(VkQueue queue, const Replay *replay)
{
	const EdramPart *const part = &queue->depths[replay->depth_part];
	return part->contents == EDRAM_IMAGE && part->image == replay->framebuffer->depth;
}

// A pass loading depth eDRAM lost draws it back from its image's texture, where the texture holds it, without its
// stencil, as Xenos exports none from a pixel shader; otherwise its contents are undefined.
static void load_depth(VkQueue queue, Replay *replay)
{
	static bool stencil_logged;
	const Framebuffer *const framebuffer = replay->framebuffer;
	Image *const depth = framebuffer->depth;
	if (depth_kept(queue, replay))
		return;
	if (!depth->is_sampled_depth || !depth->depth_in_texture) {
		depth_undefined();
		return;
	}
	if (!stencil_logged)
		xsdk_log_write("vulkan4xenon: depth drawn back from its texture comes back without its stencil");
	stencil_logged = true;
	v4x_blit_draw_depth(
		&queue->blit, &depth->texture, framebuffer->width, framebuffer->height, replay->render_pass->color_count);
	depth->used_serial = replay->serial;
	queue->depths[replay->depth_part] = holding(depth, replay->depth_tile, whole(framebuffer));
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

// A pass that clears depth and loads stencil, or the reverse, clears its part over the area after the load, the other's
// writes off, as vkCmdClearAttachments clears depth or stencil alone (blit.c, v4x_blit_clear).
static void clear_depth_part(VkQueue queue, const Replay *replay)
{
	const RenderPass *const render_pass = replay->render_pass;
	if (!render_pass->clears_depth_alone && !render_pass->clears_stencil_alone)
		return;
	const ClearRect clear = { .rect = replay->area,
		.depth = replay->clears->depth,
		.stencil = replay->clears->stencil,
		.clears_depth = render_pass->clears_depth_alone,
		.clears_stencil = render_pass->clears_stencil_alone };
	v4x_blit_clear(
		&queue->blit, &clear, render_pass->color_count, replay->framebuffer->width, replay->framebuffer->height);
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
		// Its resolve was put off only for a whole pass, whose area holds any load's.
		assert(!colors[i].unresolved);
		if (render_pass->color_count > 1)
			set_one_target(replay, i);
		v4x_profile_pass_event(V4X_PROFILE_DRAWN_BACK);
		edram_counts.drawn_back++;
		v4x_blit_draw(&queue->blit, &framebuffer->attachments[i]->texture, framebuffer->width, framebuffer->height,
			&replay->area, 0, 1);
		framebuffer->attachments[i]->used_serial = replay->serial;
		colors[i] = holding_color(replay, i, replay->area);
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
		v4x_profile_pass_event(V4X_PROFILE_CLEAR_RESOLVE);
		edram_counts.clear_resolves++;
		resolve(framebuffer->attachments[i], i, replay, true, depth_left, replay->clears->colors[i]);
		colors[i] = cleared_color(replay, i, replay->clears->colors[i], replay->area);
		depth_left = false;
		resolved = true;
	}
	if (depth_left) {
		v4x_profile_pass_event(V4X_PROFILE_CLEAR_RESOLVE);
		edram_counts.clear_resolves++;
		resolve(framebuffer->attachments[0], 0, replay, false, true, replay->clears->colors[0]);
	}
	if (resolved || depth_left)
		set_target(replay);
	if (clears_depth)
		*depth =
			cleared_depth(framebuffer, replay->depth_tile, replay->clear_depth, replay->clear_stencil, replay->area);
}

static void begin_render_pass(VkQueue queue, Replay *replay)
{
	const RenderPass *const render_pass = replay->render_pass;
	v4x_profile_pass_mark(true);
	place_colors(queue, replay);
	place_depth(queue, replay);
	EdramPart colors[V4X_MAX_COLOR_ATTACHMENTS];
	for (uint32_t i = 0; i < render_pass->color_count; i++) {
		colors[i] = held_color(queue, replay, i);
		if (colors[i].contents == EDRAM_IMAGE && colors[i].image != replay->framebuffer->attachments[i])
			colors[i] = (EdramPart){ .contents = EDRAM_UNKNOWN };
	}
	forget_colors(queue, replay);
	v4x_profile_pass_lap(V4X_PROFILE_PASS_PLACE);
	set_target(replay);
	v4x_profile_pass_lap(V4X_PROFILE_PASS_TARGET);
	if (render_pass->has_depth && render_pass->loads_depth && !replay->clears_depth && !replay->draws_image_clear)
		load_depth(queue, replay);
	if (replay->draws_image_clear)
		clear_image_depth(queue, replay, &queue->depths[replay->depth_part]);
	draw_back(queue, replay, colors);
	clear_targets(queue, replay, colors);
	clear_depth_part(queue, replay);
	v4x_profile_pass_lap(V4X_PROFILE_PASS_FILL);
	if (keep_colors(queue, colors, render_pass->color_count, replay->serial))
		set_target(replay);
	replay->clip_planes_on = false;
	v4x_profile_pass_lap(V4X_PROFILE_PASS_KEEP);
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
		framebuffer->samples != replay->framebuffer->samples || !contains(&replay->area, next_area))
		return false;
	for (uint32_t i = 0; i < pass->color_count; i++) {
		if (framebuffer->targets[i].tile != own->tile || framebuffer->targets[i].format != own->format)
			continue;
		*rgba = next_clears->colors[i];
		return pass->load_ops[i] == VK_ATTACHMENT_LOAD_OP_CLEAR;
	}
	return false;
}

// Whether a barrier after index, before the next pass, leaves the depth image to be read (resolve_depth_to_read), which
// takes it from eDRAM.
static bool barrier_reads_depth(VkCommandBuffer buffer, uint32_t index, const Image *depth)
{
	for (uint32_t c = index + 1; c < buffer->count && buffer->commands[c].type != COMMAND_BEGIN_RENDER_PASS; c++) {
		if (buffer->commands[c].type == COMMAND_RESOLVE_DEPTH && buffer->commands[c].depth_to_read == depth)
			return true;
	}
	return false;
}

// The same for depth, over the same area alone, as eDRAM holds a depth image's depth around a pass's area; a pass
// without depth never clears it, as a later pass may load an earlier one's, and nor does one whose depth a barrier
// after it leaves to be read.
static bool end_depth_clear(VkCommandBuffer buffer, uint32_t index, const Replay *replay, const Command *next,
	const PassClears *next_clears, const VkRect2D *next_area, float *depth, uint32_t *stencil)
{
	if (!replay->render_pass->has_depth || barrier_reads_depth(buffer, index, replay->framebuffer->depth))
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
		framebuffer->height == replay->framebuffer->height && framebuffer->samples == replay->framebuffer->samples &&
		contains(&replay->area, next_area) && contains(next_area, &replay->area);
}

// A sampled depth attachment the pass leaves to be read goes into its texture first, clearing nothing, so the colour's
// resolve clears as it would; so does a vkCmdResolveImage's destination, sample 0 at 2 and 4 samples.
static void resolve_depth(Image *depth, uint32_t serial)
{
	v4x_profile_pass_event(V4X_PROFILE_END_RESOLVE);
	xsdk_gpu_set_resolve_clears(false, false);
	xsdk_gpu_resolve_depth_texture(&depth->texture);
	depth->used_serial = serial;
	depth->depth_in_texture = true;
}

// A barrier leaving a sampled depth image to be read, which a pass left as an attachment: its depth goes into its
// texture from where eDRAM holds it, as at a pass's end, the targets set over it there. Depth eDRAM no longer holds is
// undefined (docs/SUPPORT.md, Render passes), so the texture keeps what it held.
static void resolve_depth_to_read(VkQueue queue, Replay *replay, Image *image)
{
	for (uint32_t i = 0; i < V4X_EDRAM_DEPTHS; i++) {
		const EdramPart *const part = &queue->depths[i];
		if (part->contents != EDRAM_IMAGE || part->image != image)
			continue;
		// A colour target as a resolve needs one set; it copies depth alone and clears nothing.
		const XsdkColorTarget target = { .format = XSDK_TARGET_8_8_8_8, .tile = 0 };
		xsdk_gpu_set_targets_samples(part->width, part->height, part->samples, &target, 1, part->tile);
		// The target set turned the clip planes off.
		replay->clip_planes_on = false;
		resolve_depth(image, replay->serial);
		return;
	}
}

// Where eDRAM holds an image once a rectangle of it is resolved: where it held it, if over the rectangle, else there.
static VkRect2D stored_area(const EdramPart *color, const Image *image, const VkRect2D *rect)
{
	const bool held = color->contents == EDRAM_IMAGE && color->image == image && contains(&color->area, rect);
	return held ? color->area : *rect;
}

// Whether a pass draws into the image as a colour attachment, so its own resolve may wait for it; one that resolves
// into it or takes it as depth reads or writes its memory first.
static bool pass_draws_into(const Command *begin, const Image *image, bool *touches)
{
	const Framebuffer *const framebuffer = begin->begin.framebuffer;
	*touches = framebuffer->depth == image;
	for (uint32_t i = 0; i < begin->begin.render_pass->color_count; i++) {
		*touches = *touches || framebuffer->resolves[i] == image;
		if (framebuffer->attachments[i] == image)
			return true;
	}
	return false;
}

// Whether a draw with these sets bound may sample the image, through any combined image sampler viewing it.
static bool sets_view(const DescriptorSet *const *sets, const Image *image)
{
	for (uint32_t s = 0; s < V4X_MAX_DESCRIPTOR_SETS; s++) {
		for (uint32_t b = 0; sets[s] && b < V4X_MAX_DESCRIPTOR_BINDINGS; b++) {
			if (sets[s]->layout->bindings[b].type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER &&
				sets[s]->descriptors[b].image == image)
				return true;
		}
	}
	return false;
}

// Whether a run's draws sample the image.
static bool run_samples(const Recorder *recorder, uint32_t index, const Image *image)
{
	const Run *const run = &recorder->runs[index];
	for (uint32_t i = run->first_image; i < run->end_image; i++) {
		if (recorder->images[i] == image)
			return true;
	}
	return false;
}

// Whether an image a whole pass stored can stay in eDRAM unresolved: within the scan's reach after the pass's end, a
// pass draws into it again before any draw may sample it or a command but those below runs; replay is in order, as
// vkCmdPipelineBarrier records nothing. The rest of the command buffer may read it, so its end resolves what's left.
static bool resolve_can_wait(VkCommandBuffer buffer, uint32_t end, const Image *image)
{
	const uint32_t last = buffer->count - end > RESOLVE_SCAN_COMMANDS ? end + RESOLVE_SCAN_COMMANDS : buffer->count;
	for (uint32_t c = end + 1; c < last; c++) {
		const Command *const command = &buffer->commands[c];
		bool touches = false;
		switch (command->type) {
		case COMMAND_BEGIN_RENDER_PASS:
			if (pass_draws_into(command, image, &touches))
				return true;
			if (touches)
				return false;
			break;
		case COMMAND_RUN:
			if (run_samples(buffer->recorder, command->run, image))
				return false;
			break;
		case COMMAND_DRAW_SNAPSHOT:
			if (sets_view(((const DrawSnapshot *)(buffer->data + command->state_at))->bound.sets, image))
				return false;
			break;
		case COMMAND_END_RENDER_PASS:
		case COMMAND_CLEAR_ATTACHMENTS:
		case COMMAND_COPY_BUFFER:
		case COMMAND_UPDATE_BUFFER:
		case COMMAND_FILL_BUFFER:
		case COMMAND_BEGIN_QUERY:
		case COMMAND_END_QUERY:
		case COMMAND_WRITE_TIMESTAMP:
		// A depth image's resolve reads only depth.
		case COMMAND_RESOLVE_DEPTH:
			break;
		// Copies, blits, image clears, events and secondary command buffers may read it.
		default:
			return false;
		}
	}
	return false;
}

// Whether target i's resolve can wait: a whole pass at one sample, of its image's size, stored with nothing cleared, as
// resolve_part resolves it.
static bool put_off(VkCommandBuffer buffer, uint32_t index, const Replay *replay, uint32_t i)
{
	const Framebuffer *const framebuffer = replay->framebuffer;
	const Image *const image = framebuffer->attachments[i];
	return whole_area(replay) && framebuffer->samples == 1 && image->width == framebuffer->width &&
		image->height == framebuffer->height && !framebuffer->resolves[i] && resolve_can_wait(buffer, index, image);
}

// Each stored target's area is resolved into its attachment, the first of them clearing depth for the next pass too, or
// left unresolved where nothing reads it before its next pass (put_off); a sampled depth image takes the whole of
// eDRAM's depth, which holds it all. A multisampled target's average goes into its resolve attachment first, clearing
// nothing if its own image takes it too.
static void end_render_pass(VkQueue queue, VkCommandBuffer buffer, uint32_t index, Replay *replay,
	Image *resolved_depth, const Command *next, const PassClears *next_clears, const VkRect2D *next_area)
{
	const RenderPass *const render_pass = replay->render_pass;
	const Framebuffer *const framebuffer = replay->framebuffer;
	EdramPart *const depth_part = &queue->depths[replay->depth_part];
	v4x_profile_pass_mark(false);
	if (render_pass->has_depth)
		*depth_part = holding(framebuffer->depth, replay->depth_tile, whole(framebuffer));
	if (render_pass->has_depth && render_pass->leaves_depth_to_read && framebuffer->depth->is_sampled_depth)
		resolve_depth(framebuffer->depth, replay->serial);
	else if (render_pass->has_depth)
		framebuffer->depth->depth_in_texture = false;
	if (resolved_depth)
		resolve_depth(resolved_depth, replay->serial);
	float depth = 0.0f;
	uint32_t stencil = 0;
	bool clear_depth = end_depth_clear(buffer, index, replay, next, next_clears, next_area, &depth, &stencil);
	for (uint32_t i = 0; i < render_pass->color_count; i++) {
		Image *const resolved = framebuffer->resolves[i];
		const bool stores = render_pass->store_ops[i] == VK_ATTACHMENT_STORE_OP_STORE;
		// What a whole pass left unresolved goes in first, as a pass of a smaller area keeps the image around it.
		const bool whole_put_off = queue->colors[i].unresolved && (!stores || !whole_area(replay));
		if (whole_put_off)
			resolve_part(&queue->colors[i], replay->serial);
		if (!stores && !resolved) {
			queue->colors[i] = (EdramPart){ .contents = EDRAM_UNKNOWN };
			continue;
		}
		if (whole_put_off)
			set_target(replay);
		v4x_profile_pass_lap(V4X_PROFILE_PASS_REST);
		const float *rgba;
		const bool clear_color = end_color_clear(replay, i, next, next_clears, next_area, &rgba);
		if (clear_depth)
			set_depth_clear(queue, depth, stencil);
		const VkRect2D stored = stored_area(&queue->colors[i], framebuffer->attachments[i], &replay->area);
		const bool waits = stores && !clear_color && !clear_depth && put_off(buffer, index, replay, i);
		v4x_profile_pass_lap(V4X_PROFILE_PASS_DECIDE);
		if (resolved)
			resolve(resolved, i, replay, clear_color && !stores, clear_depth && !stores, rgba);
		if (stores && !waits)
			resolve(framebuffer->attachments[i], i, replay, clear_color, clear_depth, rgba);
		v4x_profile_pass_lap(V4X_PROFILE_PASS_RESOLVE);
		if (resolved || !waits)
			v4x_profile_pass_event(V4X_PROFILE_END_RESOLVE);
		edram_counts.resolves_put_off += waits;
		queue->colors[i] =
			clear_color ? cleared_color(replay, i, rgba, replay->area) : holding_color(replay, i, stored);
		queue->colors[i].unresolved = waits;
		if (clear_depth)
			*depth_part = cleared_depth(framebuffer, replay->depth_tile, depth, stencil, replay->area);
		clear_depth = false;
		v4x_profile_pass_lap(V4X_PROFILE_PASS_REST);
	}
}

// Where a tiled pass keeps a whole depth: ending at eDRAM's top (render_pass.c, tile_targets).
static uint32_t top_depth_tile(const Framebuffer *framebuffer)
{
	return XSDK_EDRAM_TILES -
		xsdk_gpu_target_sample_tiles(framebuffer->width, framebuffer->height, framebuffer->samples);
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
	lose_parts(queue, replay->serial);
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
	} else if (render_pass->has_depth && render_pass->loads_depth && !replay->clears_depth &&
		!depth_kept(queue, replay)) {
		depth_undefined();
	}
	clear_depth_part(queue, replay);
	for (uint32_t i = 0; i < count; i++) {
		if (render_pass->load_ops[i] != VK_ATTACHMENT_LOAD_OP_LOAD)
			continue;
		v4x_blit_draw(&queue->blit, &framebuffer->attachments[i]->texture, framebuffer->width, framebuffer->height,
			&replay->area, i, count);
		framebuffer->attachments[i]->used_serial = replay->serial;
	}
}

// The GPU done with a pass recorded in the queue's space, as the CPU is about to write over it.
static void wait_for_span(const TileSpan *span)
{
	if (xsdk_gpu_fence_done(span->fence))
		return;
	v4x_profile_wait_start();
	const NTSTATUS status = xsdk_gpu_wait(span->fence, IDLE_TIMEOUT_MS);
	v4x_profile_wait_end(V4X_PROFILE_WAIT_SPAN);
	if (!NT_SUCCESS(status))
		xsdk_log_write("vulkan4xenon: a pass past eDRAM's recording didn't finish: 0x%08X", (unsigned)status);
}

// The span i after the oldest the queue keeps.
static const TileSpan *span_at(VkQueue queue, uint32_t i)
{
	return &queue->tile_spans[(queue->tile_span_first + i) % V4X_TILE_SPANS];
}

// Room for a pass past eDRAM's recording: V4X_TILE_RECORDING_DWORDS from the space's next 128-byte line, or from its
// start where they don't fit, once the GPU is done with the passes recorded there. Their fences land in order, so the
// newest of them is the one to wait for, and the spans up to it go.
static XsdkCommandBuffer *tile_room(VkQueue queue)
{
	uint32_t first = (queue->tile_write + V4X_TILE_ALIGN_DWORDS - 1) & ~(uint32_t)(V4X_TILE_ALIGN_DWORDS - 1);
	if (first + V4X_TILE_RECORDING_DWORDS > V4X_TILE_SPACE_DWORDS)
		first = 0;
	uint32_t covered = 0;
	for (uint32_t i = 0; i < queue->tile_span_count; i++) {
		const TileSpan *const span = span_at(queue, i);
		if (span->first < first + V4X_TILE_RECORDING_DWORDS && first < span->end)
			covered = i + 1;
	}
	// With every span kept, the oldest goes too, so this pass's has a place.
	if (!covered && queue->tile_span_count == V4X_TILE_SPANS)
		covered = 1;
	if (covered)
		wait_for_span(span_at(queue, covered - 1));
	queue->tile_span_first = (queue->tile_span_first + covered) % V4X_TILE_SPANS;
	queue->tile_span_count -= covered;
	queue->tile_recording = (XsdkCommandBuffer){ .dwords = queue->tile_space + first,
		.address = xsdk_gpu_address(queue->tile_space) + first * 4,
		.capacity_dwords = V4X_TILE_RECORDING_DWORDS };
	return &queue->tile_recording;
}

// The pass just ended kept as a span of the queue's space, which the GPU runs until the fence after its bands; one that
// recorded nothing, or outgrew its room, queued nothing.
static void keep_span(VkQueue queue, uint32_t fence)
{
	const XsdkCommandBuffer *const recording = &queue->tile_recording;
	const uint32_t first = (uint32_t)(recording->dwords - queue->tile_space);
	queue->tile_write = first + recording->count;
	if (!recording->count)
		return;
	queue->tile_spans[(queue->tile_span_first + queue->tile_span_count) % V4X_TILE_SPANS] =
		(TileSpan){ .first = first, .end = first + recording->count, .fence = fence };
	queue->tile_span_count++;
}

// A subpass past eDRAM's tiles is a tiled pass (xsdk:include/xsdk/gpu.h): its commands are replayed once into a
// recording, in the queue's space, that each band of rows runs.
static void begin_tiled_pass(VkQueue queue, Replay *replay)
{
	const Framebuffer *const framebuffer = replay->framebuffer;
	v4x_profile_tiles_begin();
	XsdkCommandBuffer *const recording = tile_room(queue);
	keep_tiled_depth(queue, replay);
	// Resolves clear depth in bands to it, and the hierarchical buffer takes it as each band's.
	if (replay->clears_depth && !framebuffer->tiling.depth_whole)
		set_depth_clear(queue, replay->clear_depth, replay->clear_stencil);
	const NTSTATUS status = xsdk_gpu_begin_tiles(&framebuffer->tiling, recording);
	if (!NT_SUCCESS(status))
		xsdk_log_write("vulkan4xenon: xsdk_gpu_begin_tiles failed: 0x%08X", (unsigned)status);
	replay->tiling = true;
	load_tiles(queue, replay);
	replay->clip_planes_on = false;
	v4x_profile_tiles_begun();
}

// Colour target i's band into an image, clearing the target to its clear for the next band; one drawn back is left out
// of band 0's clear.
static void add_band_resolve(const Replay *replay, uint32_t i, Image *image, XsdkTileResolve *resolve)
{
	*resolve = (XsdkTileResolve){ .target = i,
		.texture = &image->texture,
		.drawn_back = replay->render_pass->load_ops[i] == VK_ATTACHMENT_LOAD_OP_LOAD };
	memcpy(resolve->clear_rgba, replay->clears->colors[i], sizeof resolve->clear_rgba);
	image->used_serial = replay->serial;
}

// Each colour attachment is resolved a band at a time, into its image whatever its store op, as the resolve clears the
// band, but for one not stored whose resolve attachment takes it; into its resolve attachment too, which xsdk lets
// clear it alone (xsdk:include/xsdk/gpu.h, xsdk_gpu_end_tiles). So is depth, as resolves_depth says. eDRAM then holds
// the last band, which no untiled pass uses, and a whole depth at the top.
static void end_tiled_pass(VkQueue queue, Replay *replay, Image *resolved_depth)
{
	const RenderPass *const render_pass = replay->render_pass;
	const Framebuffer *const framebuffer = replay->framebuffer;
	v4x_profile_tiles_end();
	XsdkTileResolve resolves[2 * V4X_MAX_COLOR_ATTACHMENTS + 2];
	uint32_t count = 0;
	for (uint32_t i = 0; i < render_pass->color_count; i++) {
		Image *const resolved = framebuffer->resolves[i];
		if (render_pass->store_ops[i] == VK_ATTACHMENT_STORE_OP_STORE || !resolved)
			add_band_resolve(replay, i, framebuffer->attachments[i], &resolves[count++]);
		if (resolved)
			add_band_resolve(replay, i, resolved, &resolves[count++]);
	}
	Image *const depth = framebuffer->depth;
	if (resolves_depth(render_pass, framebuffer)) {
		resolves[count++] = (XsdkTileResolve){
			.target = XSDK_TILE_DEPTH, .texture = &depth->texture, .drawn_back = draws_depth_back(replay)
		};
		depth->used_serial = replay->serial;
	}
	if (render_pass->has_depth)
		depth->depth_in_texture = resolves_depth(render_pass, framebuffer);
	// Left out of band 0's clear, which would copy junk into it and could clear depth on a depth resolve
	// (xsdk:docs/HARDWARE.md, "A band's depth clear goes on a colour resolve, milestone 44 step 5").
	if (resolved_depth) {
		resolves[count++] =
			(XsdkTileResolve){ .target = XSDK_TILE_DEPTH, .texture = &resolved_depth->texture, .drawn_back = true };
		resolved_depth->used_serial = replay->serial;
	}
	replay->tiling = false;
	if (!NT_SUCCESS(xsdk_gpu_end_tiles(resolves, count)))
		(void)v4x_host_unsupported("a pass past eDRAM whose commands outgrow its recording's 1 MB");
	keep_span(queue, xsdk_gpu_fence());
	queue->depths[0] = render_pass->has_depth && framebuffer->tiling.depth_whole
		? holding(depth, replay->depth_tile, whole(framebuffer))
		: (EdramPart){ .contents = EDRAM_UNKNOWN };
	replay->clip_planes_on = false;
	v4x_profile_tiles_ended(queue->tile_recording.count);
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

// The blit is drawn from the texture into eDRAM at its destination's size, a target in its format from tile 0, outside
// any render pass, so the next one finds that target holding the destination over the rectangle, and the colour targets
// and depths it reached lost. The replay doesn't wait for it.
// A rectangle's sides out to the 8-pixel grid a resolve copies on, or the image's edges.
static VkRect2D grid_around(const VkRect2D *rect, const Image *image)
{
	const uint32_t align = XSDK_RESOLVE_ALIGNMENT;
	const uint32_t left = (uint32_t)rect->offset.x & ~(align - 1);
	const uint32_t top = (uint32_t)rect->offset.y & ~(align - 1);
	uint32_t right = ((uint32_t)rect->offset.x + rect->extent.width + align - 1) & ~(align - 1);
	uint32_t bottom = ((uint32_t)rect->offset.y + rect->extent.height + align - 1) & ~(align - 1);
	right = right < image->width ? right : image->width;
	bottom = bottom < image->height ? bottom : image->height;
	return (VkRect2D){ { (int32_t)left, (int32_t)top }, { right - left, bottom - top } };
}

// A rectangle off the grid is drawn inside the grid around it, which is resolved: the destination drawn back there
// first, unless eDRAM holds it there.
static void draw_blit(VkQueue queue, Replay *replay, const DrawnBlit *blit, const XsdkTexture *texture)
{
	Image *const destination = blit->destination;
	const VkRect2D grid = grid_around(&blit->rect, destination);
	const VkRect2D *const rect = &grid;
	const XsdkColorTarget target = { .format = XSDK_TARGET_8_8_8_8, .tile = 0 };
	const uint32_t tiles = xsdk_gpu_target_tiles(destination->width, destination->height);
	for (uint32_t i = 0; i < V4X_EDRAM_DEPTHS; i++) {
		if (part_overlaps(&queue->depths[i], 0, tiles))
			queue->depths[i] = (EdramPart){ .contents = EDRAM_UNKNOWN };
	}
	const EdramPart held = held_part(queue, &target);
	const VkRect2D stored = stored_area(&held, destination, rect);
	const bool held_grid = held.contents == EDRAM_IMAGE && held.image == destination && contains(&held.area, &grid);
	for (uint32_t i = 0; i < V4X_EDRAM_COLORS; i++) {
		if (color_overlaps(&queue->colors[i], 0, tiles) || queue->colors[i].image == destination)
			lose_part(&queue->colors[i], replay->serial);
	}
	const uint32_t depth_tile = XSDK_EDRAM_TILES - xsdk_gpu_target_tiles(destination->width, destination->height);
	xsdk_gpu_set_targets(destination->width, destination->height, &target, 1, depth_tile);
	// The target set turned the clip planes off.
	replay->clip_planes_on = false;
	if (!contains(&blit->rect, &grid) && !held_grid)
		v4x_blit_draw(&queue->blit, &destination->texture, destination->width, destination->height, &grid, 0, 1);
	v4x_blit_region(&queue->blit, blit, texture);
	xsdk_gpu_set_resolve_clears(false, false);
	xsdk_gpu_resolve_texture_rect(&destination->texture, (uint32_t)rect->offset.x, (uint32_t)rect->offset.y,
		rect->extent.width, rect->extent.height, 0);
	destination->used_serial = replay->serial;
	EdramPart part = holding(destination, 0, stored);
	part.format = XSDK_TARGET_8_8_8_8;
	(void)keep_colors(queue, &part, 1, replay->serial);
}

// A sampled depth image's depth copied on the GPU, with no wait: drawn from the source's texture into eDRAM's bottom
// tiles over the destination's own, drawn back there first unless the copy covers it, then resolved into the
// destination's texture, which a pass loading it draws back (load_depth). Xenos exports no stencil, so the
// destination's stencil comes back undefined, as a draw-back leaves it anyway; the colour target set is written
// nowhere.
static void draw_depth_copy(VkQueue queue, Replay *replay, const DrawnBlit *blit)
{
	Image *const destination = blit->destination;
	const uint32_t width = destination->width;
	const uint32_t height = destination->height;
	const uint32_t tiles = xsdk_gpu_target_tiles(width, height);
	for (uint32_t i = 0; i < V4X_EDRAM_DEPTHS; i++) {
		if (part_overlaps(&queue->depths[i], 0, tiles) || queue->depths[i].image == destination)
			queue->depths[i] = (EdramPart){ .contents = EDRAM_UNKNOWN };
	}
	for (uint32_t i = 0; i < V4X_EDRAM_COLORS; i++) {
		if (color_overlaps(&queue->colors[i], 0, tiles))
			lose_part(&queue->colors[i], replay->serial);
	}
	const XsdkColorTarget target = { .format = XSDK_TARGET_8_8_8_8, .tile = tiles };
	xsdk_gpu_set_targets(width, height, &target, 1, 0);
	replay->clip_planes_on = false;
	const VkRect2D all = { { 0, 0 }, { width, height } };
	if (!contains(&blit->rect, &all))
		v4x_blit_draw_depth(&queue->blit, &destination->texture, width, height, 1);
	v4x_blit_region_depth(&queue->blit, blit, &blit->source->texture);
	xsdk_gpu_set_resolve_clears(false, false);
	xsdk_gpu_resolve_depth_texture(&destination->texture);
	destination->used_serial = replay->serial;
	blit->source->used_serial = replay->serial;
	destination->depth_in_texture = true;
	destination->clear_pending = false;
}

// A render pass's area, in its command buffer's data.
static const VkRect2D *area_of(VkCommandBuffer buffer, const Command *begin)
{
	return (const VkRect2D *)(buffer->data + begin->begin.area_at);
}

// The pass ends before the next one in the command buffer begins, which its resolves clear for.
static void end_pass_command(VkQueue queue, VkCommandBuffer buffer, Replay *replay, uint32_t index)
{
	Image *const resolved_depth = buffer->commands[index].resolved_depth;
	v4x_profile_pass_end();
	if (replay->framebuffer->tiled) {
		end_tiled_pass(queue, replay, resolved_depth);
	} else {
		const Command *const next = next_render_pass(buffer, index);
		const PassClears *const next_clears = next ? (const PassClears *)(buffer->data + next->begin.clears_at) : NULL;
		end_render_pass(
			queue, buffer, index, replay, resolved_depth, next, next_clears, next ? area_of(buffer, next) : NULL);
	}
	v4x_profile_pass_ended();
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
	v4x_profile_pass_begin(replay->framebuffer, replay->render_pass, &replay->area);
	replay->color_tile = 0;
	if (replay->framebuffer->tiled)
		begin_tiled_pass(queue, replay);
	else
		begin_render_pass(queue, replay);
	v4x_profile_pass_begun(replay->framebuffer->tiled ? 0 : replay->color_tile);
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

// A run of the command buffer's packets into the ring, as eDRAM and xsdk stand now.
static void copy_run(VkQueue queue, Replay *replay, const Recorder *recorder, uint32_t run)
{
	queue->drawn |= v4x_record_copy(recorder, &recorder->runs[run], replay->serial) != 0;
	replay->clip_planes_on = true;
}

// A draw known at the submit alone, recorded now with the state bound for it and copied as a run: an indirect one's
// parameters are read now, in order with the replay, as the host wrote them before the submit or a copy in it did, a
// VkDrawIndirectCommand (vertexCount, instanceCount, firstVertex, firstInstance) or a VkDrawIndexedIndirectCommand
// (indexCount, instanceCount, firstIndex, vertexOffset, firstInstance); one of repeated indices copies them now.
static void draw_snapshot(VkQueue queue, Replay *replay, const DrawSnapshot *snapshot)
{
	Recorder *const scratch = queue->scratch;
	const Bound *const bound = &snapshot->bound;
	volatile const uint32_t *const p = snapshot->parameters;
	v4x_record_reset(scratch);
	if (p && !snapshot->indexed)
		v4x_record_draw(scratch, bound, p[0], p[2], p[1], p[3]);
	else if (p)
		v4x_record_draw_indexed(scratch, bound, queue, p[0], p[2], (int32_t)p[3], p[1], p[4]);
	else
		v4x_record_draw_indexed(scratch, bound, queue, snapshot->count, snapshot->first, snapshot->vertex_offset,
			snapshot->instance_count, snapshot->first_instance);
	uint32_t run;
	if (v4x_record_close(scratch, &run))
		copy_run(queue, replay, scratch, run);
}

// The command after index in its command buffer, or NULL.
static const Command *next_command(VkCommandBuffer buffer, uint32_t index)
{
	return index + 1 < buffer->count ? &buffer->commands[index + 1] : NULL;
}

static void replay_commands(VkQueue queue, VkCommandBuffer buffer, Replay *replay);

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
	case COMMAND_RUN:
		v4x_profile_lap(V4X_PROFILE_COMMANDS);
		copy_run(queue, replay, buffer->recorder, command->run);
		v4x_profile_lap(V4X_PROFILE_RUNS);
		return;
	case COMMAND_DRAW_SNAPSHOT:
		v4x_profile_lap(V4X_PROFILE_COMMANDS);
		draw_snapshot(queue, replay, (const DrawSnapshot *)(buffer->data + command->state_at));
		v4x_profile_lap(V4X_PROFILE_RUNS);
		return;
	case COMMAND_COPY_BUFFER_TO_IMAGE:
		v4x_transfer_buffer_to_image(
			queue, (const LevelCopy *)(buffer->data + command->state_at), next_command(buffer, index));
		break;
	case COMMAND_COPY_IMAGE_TO_BUFFER:
		v4x_transfer_image_to_buffer(queue, (const LevelCopy *)(buffer->data + command->state_at));
		break;
	case COMMAND_COPY_IMAGE:
		v4x_transfer_copy_image(queue, command);
		break;
	case COMMAND_COPY_IMAGE_PART:
		v4x_transfer_copy_part(queue, (const PartCopy *)(buffer->data + command->state_at));
		break;
	case COMMAND_DRAW_DEPTH_COPY:
		draw_depth_copy(queue, replay, (const DrawnBlit *)(buffer->data + command->state_at));
		break;
	case COMMAND_BLIT_IMAGE:
		v4x_transfer_blit(queue, command, next_command(buffer, index));
		break;
	case COMMAND_DRAW_BLIT: {
		const DrawnBlit *const blit = (const DrawnBlit *)(buffer->data + command->state_at);
		draw_blit(queue, replay, blit, &blit->source->texture);
		blit->source->used_serial = replay->serial;
		break;
	}
	case COMMAND_DRAW_UPLOAD: {
		const DrawnUpload *const upload = (const DrawnUpload *)(buffer->data + command->state_at);
		// The submit's invalidate of all memory left a texture fetch the rows drawn in from there before
		// (upload_check).
		xsdk_gpu_invalidate(upload->texture.address, upload->texture.size_bytes);
		draw_blit(queue, replay, &upload->blit, &upload->texture);
		// Of 4-byte texels, as an 8_8_8_8 target's formats are.
		v4x_profile_count(
			V4X_PROFILE_UPLOAD_DRAWN_BYTES, upload->blit.rect.extent.width * upload->blit.rect.extent.height * 4);
		// The GPU reads the buffer, so a copy into it waits (transfer.c, wait_for_draws).
		queue->drawn = true;
		break;
	}
	case COMMAND_RESOLVE_DEPTH:
		resolve_depth_to_read(queue, replay, command->depth_to_read);
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
	case COMMAND_CLEAR_ATTACHMENTS:
		clear_attachments(queue, replay, (const ClearRect *)(buffer->data + command->state_at));
		break;
	case COMMAND_CLEAR_DEPTH_IMAGE:
		clear_depth_image(queue, command);
		break;
	// In place; a secondary command buffer begins and ends no render pass.
	case COMMAND_EXECUTE_COMMANDS:
		replay_commands(queue, command->secondary, replay);
		break;
	case COMMAND_WRITE_EVENT:
		xsdk_gpu_write_word(command->event.address, command->event.value);
		break;
	// What the host wrote before it set an event, uniform buffers among it, may differ from what groups loaded, which
	// the run after it knows nothing of.
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
	// What the runs didn't charge to their own stage: the other commands, and their dispatch.
	v4x_profile_command(command->type);
}

// A command buffer's commands in order; kept out of line, so replay_command is inlined into it and a command costs no
// call.
static __attribute__((noinline)) void replay_commands(VkQueue queue, VkCommandBuffer buffer, Replay *replay)
{
	v4x_profile_commands(buffer->count);
	for (uint32_t c = 0; c < buffer->count; c++)
		replay_command(queue, buffer, replay, c);
}

V4xEdramCounts v4x_edram_counts(void)
{
	return edram_counts;
}

VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit(
	VkQueue queue, uint32_t submitCount, const VkSubmitInfo *pSubmits, VkFence fence)
{
	const uint32_t began = v4x_profile_call_start();
	v4x_profile_submit();
	// Drain the CPU's write-combining, so the GPU reads what the app wrote to mapped memory.
	__asm__ volatile("sync" ::: "memory");
	// Mapped memory is coherent, so the app may have written any of it; one invalidate covers it all, as xsdk's set-up
	// covers memory (xsdk:src/gpu_setup.c).
	xsdk_gpu_invalidate(0, ALL_MEMORY_BYTES);
	v4x_profile_gpu_begin();
	queue->serial = queue->serial + 1 ? queue->serial + 1 : 1;
	v4x_profile_lap(V4X_PROFILE_SUBMIT);
	for (uint32_t i = 0; i < submitCount; i++) {
		for (uint32_t j = 0; j < pSubmits[i].commandBufferCount; j++) {
			const VkCommandBuffer buffer = pSubmits[i].pCommandBuffers[j];
			Replay replay = { .queue = queue, .serial = queue->serial, .clip_planes_on = true };
			replay_commands(queue, buffer, &replay);
			// Whatever comes after may read what's left unresolved (resolve_can_wait).
			for (uint32_t c = 0; c < V4X_EDRAM_COLORS; c++) {
				if (queue->colors[c].unresolved)
					resolve_part(&queue->colors[c], replay.serial);
			}
		}
	}
	// One queue replays in order, so semaphores need nothing; the fence submits the ring either way.
	v4x_profile_gpu_end();
	const uint32_t submitted = xsdk_gpu_fence();
	queue->submit_fences[queue->serial % V4X_SUBMIT_FENCES] = submitted;
	v4x_sync_submitted(v4x_object(fence), submitted);
	v4x_profile_lap(V4X_PROFILE_SUBMIT);
	v4x_profile_call_end(V4X_PROFILE_CALL_SUBMIT, began);
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
	const uint32_t began = v4x_profile_fence_wait_start();
	const NTSTATUS status = xsdk_gpu_wait(xsdk_gpu_fence(), IDLE_TIMEOUT_MS);
	v4x_profile_fence_wait_end(began);
	v4x_profile_call_end(V4X_PROFILE_CALL_IDLE_WAIT, began);
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("vulkan4xenon: the GPU didn't go idle: 0x%08X", (unsigned)status);
		return VK_ERROR_DEVICE_LOST;
	}
	queue->drawn = false;
	return VK_SUCCESS;
}
