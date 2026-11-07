#ifndef V4X_PROFILE_H
#define V4X_PROFILE_H

#include <stdbool.h>
#include <stdint.h>

#include <vulkan/vulkan.h>

#include "objects.h"
#include "vulkan4xenon.h"

// The submit's timing, in a build made with make V4X_PROFILE=1; without it every hook is empty, and compiles to
// nothing. A lap charges the time since the last lap to a stage, so the stages add up to the submit.

// Where the replay waits for the GPU, for the 10 s log.
typedef enum {
	// A copy into a buffer, for every draw since the CPU last waited (transfer.c, wait_for_draws).
	V4X_PROFILE_WAIT_DRAWS,
	// What a submission used: an image, queries or repeated indices (transfer.c, wait_for_serial).
	V4X_PROFILE_WAIT_USED,
	// Room for a pass past eDRAM's recording (queue.c, wait_for_span).
	V4X_PROFILE_WAIT_SPAN,
	V4X_PROFILE_WAIT_SITES,
} V4xProfileWait;

// The calls the 10 s log times and puts in its timeline, whichever thread makes them.
typedef enum {
	V4X_PROFILE_CALL_SUBMIT,
	V4X_PROFILE_CALL_FENCE_WAIT,
	V4X_PROFILE_CALL_ACQUIRE,
	V4X_PROFILE_CALL_PRESENT,
	V4X_PROFILE_CALL_IDLE_WAIT,
	V4X_PROFILE_CALLS,
} V4xProfileCall;

// What the 10 s log counts a present.
typedef enum {
	// Uniform blocks the GPU loaded, and those the CPU read, and their quads.
	V4X_PROFILE_BLOCKS_LOADED,
	V4X_PROFILE_BLOCKS_READ,
	V4X_PROFILE_QUADS_READ,
	// Quads of push constants written.
	V4X_PROFILE_QUADS_PUSHED,
	// Pipelines applied whole, textures and streams pointed.
	V4X_PROFILE_PIPELINES,
	V4X_PROFILE_TEXTURES,
	V4X_PROFILE_STREAMS,
	// Bytes copied on the CPU: into images, rows of them read back to copy part of a row, out of images, between
	// images, and into buffers.
	V4X_PROFILE_UPLOAD_BYTES,
	// Of those, the bytes read from the GPU's write-combined memory, where the CPU's reads aren't cached.
	V4X_PROFILE_UPLOAD_UNCACHED_BYTES,
	// Bytes the GPU drew into images from buffers in its memory, which the CPU didn't copy (command_buffer.c,
	// drawn_upload).
	V4X_PROFILE_UPLOAD_DRAWN_BYTES,
	V4X_PROFILE_PARTIAL_ROWS,
	V4X_PROFILE_DOWNLOAD_BYTES,
	V4X_PROFILE_IMAGE_COPY_BYTES,
	V4X_PROFILE_BUFFER_BYTES,
	// Literal registers written, and pipelines applied whole though the pipeline was the one applied.
	V4X_PROFILE_LITERALS,
	V4X_PROFILE_PIPELINES_AGAIN,
	V4X_PROFILE_COUNTS,
} V4xProfileCount;

// What a render pass did, for the trace of one present's passes the 10 s log gives once.
typedef enum {
	// Drew an image back into eDRAM, resolved to clear at its begin, resolved at its end.
	V4X_PROFILE_DRAWN_BACK,
	V4X_PROFILE_CLEAR_RESOLVE,
	V4X_PROFILE_END_RESOLVE,
	V4X_PROFILE_PASS_EVENTS,
} V4xProfilePassEvent;

// The parts of an untiled pass's begin and end the 10 s log times: placing its targets and forgetting what they reach,
// setting them, drawing back and clearing, keeping its parts; then deciding each store, resolving, and the rest.
typedef enum {
	V4X_PROFILE_PASS_PLACE,
	V4X_PROFILE_PASS_TARGET,
	V4X_PROFILE_PASS_FILL,
	V4X_PROFILE_PASS_KEEP,
	V4X_PROFILE_PASS_DECIDE,
	V4X_PROFILE_PASS_RESOLVE,
	V4X_PROFILE_PASS_REST,
	V4X_PROFILE_PASS_STAGES,
} V4xProfilePassStage;

#ifdef V4X_PROFILE
// Counts a submit and starts its laps; the first also learns what a lap costs.
void v4x_profile_submit(void);
void v4x_profile_lap(V4xProfileStage stage);
void v4x_profile_commands(uint32_t count);
// Laps V4X_PROFILE_COMMANDS at a command's end, charging the time to its CommandType as well (src/objects.h).
void v4x_profile_command(uint32_t type);
// Counts a draw, then laps V4X_PROFILE_CHECKS, the checks it took to get there.
void v4x_profile_draw(void);
// Counts a present, and logs the frames since the last log once 10 seconds have passed.
void v4x_profile_present(void);
// Time a wait for the GPU in the replay, for the 10 s log; the stage the wait falls in still counts it too.
void v4x_profile_wait_start(void);
void v4x_profile_wait_end(V4xProfileWait site);
void v4x_profile_count(V4xProfileCount what, uint32_t amount);
// Time a wait for a fence outside the submit, on any thread (vkWaitForFences, vkQueueWaitIdle); start returns when it
// began, for end. Threads waiting at once race on the totals, which a log can bear.
uint32_t v4x_profile_fence_wait_start(void);
void v4x_profile_fence_wait_end(uint32_t began);
// Time a call that may block, from start to end, on any thread, for the 10 s log and its timeline of the last calls.
uint32_t v4x_profile_call_start(void);
void v4x_profile_call_end(V4xProfileCall call, uint32_t began);
// Time a pass past eDRAM's begin and end (src/queue.c), a wait for the GPU between begin and begun counted as its too.
void v4x_profile_tiles_begin(void);
void v4x_profile_tiles_begun(void);
void v4x_profile_tiles_end(void);
void v4x_profile_tiles_ended(uint32_t dwords);
// Time the GPU takes on a submit, from the first hook to the second, by its clock.
void v4x_profile_gpu_begin(void);
void v4x_profile_gpu_end(void);
// A render pass for the trace: its begin and end, each timed from the first hook to the second, where its colour went,
// what it did, and the textures it pointed.
void v4x_profile_pass_begin(const Framebuffer *framebuffer, const RenderPass *render_pass, const VkRect2D *area);
void v4x_profile_pass_begun(uint32_t color_tile);
void v4x_profile_pass_end(void);
void v4x_profile_pass_ended(void);
void v4x_profile_pass_event(V4xProfilePassEvent event);
// Starts timing an untiled pass's begin or end, then charges the time since to a stage at each lap.
void v4x_profile_pass_mark(bool begin);
void v4x_profile_pass_lap(V4xProfilePassStage stage);
void v4x_profile_pass_texture(uint32_t address);
#else
static inline void v4x_profile_submit(void)
{
}

static inline void v4x_profile_lap(V4xProfileStage stage)
{
	(void)stage;
}

static inline void v4x_profile_commands(uint32_t count)
{
	(void)count;
}

static inline void v4x_profile_command(uint32_t type)
{
	(void)type;
}

static inline void v4x_profile_draw(void)
{
}

static inline void v4x_profile_present(void)
{
}

static inline void v4x_profile_wait_start(void)
{
}

static inline void v4x_profile_wait_end(V4xProfileWait site)
{
	(void)site;
}

static inline void v4x_profile_count(V4xProfileCount what, uint32_t amount)
{
	(void)what;
	(void)amount;
}

static inline uint32_t v4x_profile_fence_wait_start(void)
{
	return 0;
}

static inline void v4x_profile_fence_wait_end(uint32_t began)
{
	(void)began;
}

static inline uint32_t v4x_profile_call_start(void)
{
	return 0;
}

static inline void v4x_profile_call_end(V4xProfileCall call, uint32_t began)
{
	(void)call;
	(void)began;
}

static inline void v4x_profile_tiles_begin(void)
{
}

static inline void v4x_profile_tiles_begun(void)
{
}

static inline void v4x_profile_tiles_end(void)
{
}

static inline void v4x_profile_tiles_ended(uint32_t dwords)
{
	(void)dwords;
}

static inline void v4x_profile_gpu_begin(void)
{
}

static inline void v4x_profile_gpu_end(void)
{
}

static inline void v4x_profile_pass_begin(
	const Framebuffer *framebuffer, const RenderPass *render_pass, const VkRect2D *area)
{
	(void)framebuffer;
	(void)render_pass;
	(void)area;
}

static inline void v4x_profile_pass_begun(uint32_t color_tile)
{
	(void)color_tile;
}

static inline void v4x_profile_pass_end(void)
{
}

static inline void v4x_profile_pass_ended(void)
{
}

static inline void v4x_profile_pass_event(V4xProfilePassEvent event)
{
	(void)event;
}

static inline void v4x_profile_pass_mark(bool begin)
{
	(void)begin;
}

static inline void v4x_profile_pass_lap(V4xProfilePassStage stage)
{
	(void)stage;
}

static inline void v4x_profile_pass_texture(uint32_t address)
{
	(void)address;
}
#endif

#endif
