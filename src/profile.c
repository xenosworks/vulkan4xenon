#include "profile.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "xsdk/gpu.h"
#include "xsdk/log.h"

// The submit's stages timed with the CPU's time base (docs/ROADMAP.md, milestone 29). Only the queue's calls change the
// totals, so the queue's external synchronization guards them.

enum {
	// Laps timed back to back at the first submit, to learn what one costs.
	CALIBRATION_LAPS = 10000,
	LOG_SECONDS = 10,
	// Past src/objects.h's CommandType.
	COMMAND_TYPES = 64,
	// A command type's line in the 10 s log once it takes this share of the submits, in hundredths.
	COMMAND_LOG_HUNDREDTHS = 2,
	// Submits whose GPU clocks the ring keeps, more than a 10 s log reads; each takes a begin and an end slot of 4
	// words, the clock's 3 and one to keep slots 16 bytes apart.
	GPU_SUBMITS = 1024,
	GPU_SLOT_WORDS = 4,
	// The newest submits the log leaves for the next, as the GPU may not have reached them.
	GPU_PENDING = 8,
	// What a slot holds until the GPU writes its clock.
	GPU_UNWRITTEN = 0xFFFFFFFF,
	// The passes the trace keeps, from the present after it's armed, and the textures each; it logs them at the present
	// after they're all kept, or after TRACE_PRESENTS presents.
	TRACE_PASSES = 1024,
	TRACE_PRESENTS = 8,
	TRACE_TEXTURES = 4,
	// The next present's passes are traced after the third 10 s log of this many draws a present or more, so a game
	// has settled past its menus and loading.
	TRACE_AT_LOG = 3,
	TRACE_DRAWS = 300,
};

// A render pass the trace kept.
typedef struct {
	uint32_t image;
	uint32_t width;
	uint32_t height;
	uint32_t samples;
	uint32_t color_count;
	uint32_t tile;
	uint32_t load_op;
	uint32_t store_op;
	bool has_depth;
	bool loads_depth;
	bool clears_depth;
	bool tiled;
	VkRect2D area;
	uint32_t events[V4X_PROFILE_PASS_EVENTS];
	uint32_t draws;
	uint32_t textures[TRACE_TEXTURES];
	uint32_t texture_count;
	uint32_t begin_ticks;
	uint32_t end_ticks;
} PassTrace;

typedef enum {
	TRACE_WAITING,
	TRACE_ARMED,
	TRACE_RECORDING,
	TRACE_DONE,
} TraceState;

static const char *const stage_names[V4X_PROFILE_STAGES] = {
	"dispatch",
	"push",
	"pipeline",
	"constants",
	"resources",
	"checks",
	"draws",
	"commands",
	"submit",
};

static V4xProfile totals;
static uint32_t last_lap;
static bool calibrated;
// What CALIBRATION_LAPS laps took back to back, in ticks.
static uint32_t calibration_ticks;
// The totals at the last log, and its second.
static V4xProfile logged;
static time_t logged_at;
// V4X_PROFILE_COMMANDS's ticks and laps by command type, and theirs at the last log.
static uint64_t command_ticks[COMMAND_TYPES];
static uint32_t command_laps[COMMAND_TYPES];
static uint64_t logged_command_ticks[COMMAND_TYPES];
static uint32_t logged_command_laps[COMMAND_TYPES];
// The replay's waits for the GPU by site: their ticks and count, both at the last log, and when the one going on began.
static uint64_t wait_ticks[V4X_PROFILE_WAIT_SITES];
static uint32_t waits[V4X_PROFILE_WAIT_SITES];
static uint64_t logged_wait_ticks[V4X_PROFILE_WAIT_SITES];
static uint32_t logged_waits[V4X_PROFILE_WAIT_SITES];
static uint32_t wait_began;
// The app's waits for fences, likewise.
static uint64_t fence_wait_ticks;
static uint32_t fence_waits;
static uint64_t logged_fence_wait_ticks;
static uint32_t logged_fence_waits;
// The layer's eDRAM counts and xsdk's, at the last log.
static V4xEdramCounts logged_edram;
static XsdkGpuStats logged_gpu_stats;
// V4xProfileCount's counts, and theirs at the last log.
static uint64_t counts[V4X_PROFILE_COUNTS];
static uint64_t logged_counts[V4X_PROFILE_COUNTS];
static const char *const count_names[V4X_PROFILE_COUNTS] = {
	"uniform blocks the GPU loaded",
	"uniform blocks the CPU read",
	"quads the CPU read from uniform blocks",
	"push constant quads written",
	"pipelines applied whole",
	"textures pointed",
	"streams pointed",
	"bytes copied into images",
	"rows read back to copy part of a row",
	"bytes copied out of images",
	"bytes copied between images",
	"bytes copied into buffers",
	"literal registers written",
	"pipelines applied whole again",
};
// When the pass past eDRAM's begin or end being timed started, and whether a wait now falls in its begin.
static uint32_t tiles_began;
static bool in_tiled_begin;

// The GPU's clocks at each submit's begin and end, in a ring of GPU memory taken at the first submit; the submits so
// far, and the first the log hasn't read.
static volatile uint32_t *gpu_clocks;
static uint32_t gpu_submits;
static uint32_t gpu_read;
// The GPU's time on the submits the logs read, in its clock's ticks, and how many it read; both at the last log.
static uint64_t gpu_ticks;
static uint32_t gpu_counted;
static uint64_t logged_gpu_ticks;
static uint32_t logged_gpu_counted;
// The trace of one present's passes, and the pass being traced, if any, with the draws before it and when its begin
// or end started.
static PassTrace trace[TRACE_PASSES];
static uint32_t trace_count;
static TraceState trace_state;
static uint32_t trace_presents;
// The 10 s logs of TRACE_DRAWS a present or more so far.
static uint32_t logs;
static PassTrace *pass;
static uint32_t pass_draws;
static uint32_t pass_started;
// Untiled passes' begins and ends by part (V4xProfilePassStage), the begins counted, both at the last log too, and
// when the last lap was.
static uint64_t pass_stage_ticks[V4X_PROFILE_PASS_STAGES];
static uint64_t logged_pass_stage_ticks[V4X_PROFILE_PASS_STAGES];
static uint32_t pass_begins;
static uint32_t logged_pass_begins;
static uint32_t pass_lap_last;

static void log_commands(void);
static void log_waits(void);
static void log_counts(void);
static void log_gpu(void);
static void log_trace(void);

// The low word alone: a lap is far shorter than its 86 s wrap.
static uint32_t time_base(void)
{
	uint32_t ticks;
	__asm__ volatile("mftb %0" : "=r"(ticks));
	return ticks;
}

static time_t seconds(void)
{
	struct timespec now;
	(void)clock_gettime(CLOCK_MONOTONIC, &now);
	return now.tv_sec;
}

static void charge(V4xProfileStage stage)
{
	const uint32_t now = time_base();
	totals.ticks[stage] += now - last_lap;
	totals.laps[stage]++;
	last_lap = now;
}

// Never inlined or cloned, so the calibration times the same call the hooks make.
__attribute__((noipa)) void v4x_profile_lap(V4xProfileStage stage)
{
	charge(stage);
}

// Each lap back to back costs what a lap adds to the stage it charges; the totals start after.
static void calibrate(void)
{
	last_lap = time_base();
	const uint32_t start = last_lap;
	for (uint32_t i = 0; i < CALIBRATION_LAPS; i++)
		v4x_profile_lap(V4X_PROFILE_DISPATCH);
	calibration_ticks = last_lap - start;
	totals = (V4xProfile){ 0 };
	logged_at = seconds();
	calibrated = true;
}

void v4x_profile_submit(void)
{
	if (!calibrated)
		calibrate();
	totals.submits++;
	last_lap = time_base();
}

void v4x_profile_commands(uint32_t count)
{
	totals.commands += count;
}

void v4x_profile_command(uint32_t type)
{
	assert(type < COMMAND_TYPES);
	const uint64_t before = totals.ticks[V4X_PROFILE_COMMANDS];
	charge(V4X_PROFILE_COMMANDS);
	command_ticks[type] += totals.ticks[V4X_PROFILE_COMMANDS] - before;
	command_laps[type]++;
}

void v4x_profile_draw(void)
{
	totals.draws++;
	charge(V4X_PROFILE_CHECKS);
}

void v4x_profile_present(void)
{
	if (!calibrated)
		return;
	totals.presents++;
	if (trace_state == TRACE_RECORDING && (trace_count == TRACE_PASSES || ++trace_presents == TRACE_PRESENTS)) {
		log_trace();
		trace_state = TRACE_DONE;
	} else if (trace_state == TRACE_ARMED) {
		trace_state = TRACE_RECORDING;
	}
	const time_t now = seconds();
	if (now - logged_at < LOG_SECONDS)
		return;
	v4x_profile_log("the last 10 s", &logged, &totals);
	log_commands();
	log_waits();
	log_counts();
	log_gpu();
	const bool busy = totals.draws - logged.draws >= TRACE_DRAWS * (totals.presents - logged.presents);
	logged = totals;
	logged_at = now;
	if (busy && ++logs == TRACE_AT_LOG && trace_state == TRACE_WAITING)
		trace_state = TRACE_ARMED;
}

static volatile uint32_t *gpu_slot(uint32_t submit, uint32_t end)
{
	return &gpu_clocks[((submit % GPU_SUBMITS) * 2 + end) * GPU_SLOT_WORDS];
}

static void write_gpu_clock(uint32_t end)
{
	volatile uint32_t *const slot = gpu_slot(gpu_submits, end);
	for (uint32_t i = 0; i < GPU_SLOT_WORDS; i++)
		slot[i] = GPU_UNWRITTEN;
	// Out of the CPU's write-combining before the GPU writes over it.
	__asm__ volatile("sync" ::: "memory");
	xsdk_gpu_write_clock(xsdk_gpu_address(slot), true);
}

// Once the GPU is done with what came before, so submits' times don't overlap.
void v4x_profile_gpu_begin(void)
{
	if (!gpu_clocks)
		gpu_clocks = xsdk_gpu_alloc(GPU_SUBMITS * 2 * GPU_SLOT_WORDS * sizeof(uint32_t));
	if (gpu_clocks)
		write_gpu_clock(0);
}

void v4x_profile_gpu_end(void)
{
	if (!gpu_clocks)
		return;
	write_gpu_clock(1);
	gpu_submits++;
}

static bool gpu_written(volatile const uint32_t *slot)
{
	return slot[0] != GPU_UNWRITTEN || slot[2] != GPU_UNWRITTEN;
}

// The submits the GPU has passed since the last log, but the newest; one the ring lost is skipped.
static void read_gpu_clocks(void)
{
	if (gpu_submits - gpu_read > GPU_SUBMITS)
		gpu_read = gpu_submits - GPU_SUBMITS;
	for (; gpu_read + GPU_PENDING < gpu_submits; gpu_read++) {
		volatile const uint32_t *const begin = gpu_slot(gpu_read, 0);
		volatile const uint32_t *const end = gpu_slot(gpu_read, 1);
		if (!gpu_written(begin) || !gpu_written(end))
			continue;
		gpu_ticks += xsdk_gpu_clock_value(end) - xsdk_gpu_clock_value(begin);
		gpu_counted++;
	}
}

void v4x_profile_pass_begin(const Framebuffer *framebuffer, const RenderPass *render_pass, const VkRect2D *area)
{
	pass = NULL;
	if (trace_state != TRACE_RECORDING || trace_count == TRACE_PASSES)
		return;
	pass = &trace[trace_count++];
	const bool colors = render_pass->color_count != 0;
	const Image *const image = colors ? framebuffer->attachments[0] : framebuffer->depth;
	*pass = (PassTrace){ .image = image ? image->texture.address : 0,
		.width = framebuffer->width,
		.height = framebuffer->height,
		.samples = framebuffer->samples,
		.color_count = render_pass->color_count,
		.load_op = colors ? render_pass->load_ops[0] : 0,
		.store_op = colors ? render_pass->store_ops[0] : 0,
		.has_depth = render_pass->has_depth,
		.loads_depth = render_pass->loads_depth,
		.clears_depth = render_pass->clears_depth,
		.tiled = framebuffer->tiled,
		.area = *area };
	pass_started = time_base();
}

void v4x_profile_pass_begun(uint32_t color_tile)
{
	if (!pass)
		return;
	pass->begin_ticks = time_base() - pass_started;
	pass->tile = color_tile;
	pass_draws = totals.draws;
}

void v4x_profile_pass_end(void)
{
	if (!pass)
		return;
	pass->draws = totals.draws - pass_draws;
	pass_started = time_base();
}

void v4x_profile_pass_ended(void)
{
	if (!pass)
		return;
	pass->end_ticks = time_base() - pass_started;
	pass = NULL;
}

void v4x_profile_pass_mark(bool begin)
{
	pass_begins += begin;
	pass_lap_last = time_base();
}

void v4x_profile_pass_lap(V4xProfilePassStage stage)
{
	const uint32_t now = time_base();
	pass_stage_ticks[stage] += now - pass_lap_last;
	pass_lap_last = now;
}

void v4x_profile_pass_event(V4xProfilePassEvent event)
{
	if (pass)
		pass->events[event]++;
}

void v4x_profile_pass_texture(uint32_t address)
{
	if (!pass)
		return;
	for (uint32_t i = 0; i < pass->texture_count; i++) {
		if (pass->textures[i] == address)
			return;
	}
	if (pass->texture_count < TRACE_TEXTURES)
		pass->textures[pass->texture_count++] = address;
}

void v4x_profile_wait_start(void)
{
	wait_began = time_base();
}

void v4x_profile_wait_end(V4xProfileWait site)
{
	const uint32_t ticks = time_base() - wait_began;
	wait_ticks[site] += ticks;
	waits[site]++;
	if (!in_tiled_begin)
		return;
	totals.tiled_wait_ticks += ticks;
	totals.tiled_waits++;
}

void v4x_profile_count(V4xProfileCount what, uint32_t amount)
{
	counts[what] += amount;
}

uint32_t v4x_profile_fence_wait_start(void)
{
	return time_base();
}

void v4x_profile_fence_wait_end(uint32_t began)
{
	fence_wait_ticks += time_base() - began;
	fence_waits++;
}

void v4x_profile_tiles_begin(void)
{
	tiles_began = time_base();
	in_tiled_begin = true;
}

void v4x_profile_tiles_begun(void)
{
	totals.tiled_begin_ticks += time_base() - tiles_began;
	totals.tiled_passes++;
	in_tiled_begin = false;
}

void v4x_profile_tiles_end(void)
{
	tiles_began = time_base();
}

void v4x_profile_tiles_ended(uint32_t dwords)
{
	totals.tiled_end_ticks += time_base() - tiles_began;
	totals.tiled_dwords += dwords;
	if (dwords > totals.tiled_most_dwords)
		totals.tiled_most_dwords = dwords;
}

V4xProfile v4x_profile_totals(void)
{
	return totals;
}

// The time base ticks at 49.875 MHz (xsdk:src/time.c), so a tick is 10^9 / 49,875,000 ns, 8000 / 399.
static uint64_t nanoseconds(uint64_t ticks)
{
	return ticks * 8000 / 399;
}

// The total over the count, rounded; 0 over nothing.
static uint32_t each(uint64_t total, uint32_t count)
{
	return count ? (uint32_t)((total + count / 2) / count) : 0;
}

// A stage's time a draw, as timed and less what its laps cost.
static void log_stage(const char *what, const char *stage, uint64_t ticks, uint32_t laps, uint32_t draws)
{
	const uint64_t lap_ticks = (uint64_t)laps * calibration_ticks / CALIBRATION_LAPS;
	const uint64_t less = ticks > lap_ticks ? ticks - lap_ticks : 0;
	const uint32_t laps_tenths = each((uint64_t)laps * 10, draws);
	xsdk_log_write("vulkan4xenon: profile, %s: %s %u ns a draw, %u less its %u.%u laps", what, stage,
		(unsigned)each(nanoseconds(ticks), draws), (unsigned)each(nanoseconds(less), draws),
		(unsigned)(laps_tenths / 10), (unsigned)(laps_tenths % 10));
}

// Passes past eDRAM, if any: their begins' waits for the GPU and the rest of their begins, their ends, and what they
// recorded (docs/ROADMAP.md, milestone 30).
static void log_tiles(const char *what, const V4xProfile *from, const V4xProfile *to, uint32_t presents)
{
	const uint32_t passes = to->tiled_passes - from->tiled_passes;
	if (!passes)
		return;
	const uint64_t waited = to->tiled_wait_ticks - from->tiled_wait_ticks;
	const uint64_t begun = to->tiled_begin_ticks - from->tiled_begin_ticks;
	const uint64_t dwords = to->tiled_dwords - from->tiled_dwords;
	const uint32_t pass_tenths = each((uint64_t)passes * 10, presents);
	const uint32_t wait_tenths = each((uint64_t)(to->tiled_waits - from->tiled_waits) * 10, presents);
	xsdk_log_write(
		"vulkan4xenon: profile, %s: passes past eDRAM, %u.%u a present: their begins %u us a present waiting for the "
		"GPU, in %u.%u waits, and %u us besides; their ends %u us; %u dwords recorded a present, %u a pass, %u at "
		"most since the first submit",
		what, (unsigned)(pass_tenths / 10), (unsigned)(pass_tenths % 10),
		(unsigned)each(nanoseconds(waited) / 1000, presents), (unsigned)(wait_tenths / 10),
		(unsigned)(wait_tenths % 10), (unsigned)each(nanoseconds(begun - waited) / 1000, presents),
		(unsigned)each(nanoseconds(to->tiled_end_ticks - from->tiled_end_ticks) / 1000, presents),
		(unsigned)each(dwords, presents), (unsigned)each(dwords, passes), (unsigned)to->tiled_most_dwords);
}

void v4x_profile_log(const char *what, const V4xProfile *from, const V4xProfile *to)
{
	const uint32_t draws = to->draws - from->draws;
	const uint32_t submits = to->submits - from->submits;
	const uint32_t presents = to->presents - from->presents;
	uint64_t ticks = 0;
	uint32_t laps = 0;
	for (uint32_t s = 0; s < V4X_PROFILE_STAGES; s++) {
		ticks += to->ticks[s] - from->ticks[s];
		laps += to->laps[s] - from->laps[s];
	}
	// Tenths, so the log needs no floats.
	const uint32_t present_draws = each((uint64_t)draws * 10, presents);
	const uint32_t present_us = each(nanoseconds(ticks) / 100, presents);
	const uint32_t submit_draws = each((uint64_t)draws * 10, submits);
	const uint32_t submit_us = each(nanoseconds(ticks) / 100, submits);
	const uint32_t lap_ns = each(nanoseconds(calibration_ticks) * 10, CALIBRATION_LAPS);
	xsdk_log_write(
		"vulkan4xenon: profile, %s: %u draws, %u commands, %u submits, %u presents; a present %u.%u draws, "
		"%u.%u us submitting; a submit %u.%u draws, %u.%u us; a lap %u.%u ns",
		what, (unsigned)draws, (unsigned)(to->commands - from->commands), (unsigned)submits, (unsigned)presents,
		(unsigned)(present_draws / 10), (unsigned)(present_draws % 10), (unsigned)(present_us / 10),
		(unsigned)(present_us % 10), (unsigned)(submit_draws / 10), (unsigned)(submit_draws % 10),
		(unsigned)(submit_us / 10), (unsigned)(submit_us % 10), (unsigned)(lap_ns / 10), (unsigned)(lap_ns % 10));
	for (uint32_t s = 0; s < V4X_PROFILE_STAGES; s++)
		log_stage(what, stage_names[s], to->ticks[s] - from->ticks[s], to->laps[s] - from->laps[s], draws);
	log_stage(what, "all", ticks, laps, draws);
	log_tiles(what, from, to, presents);
}

// Since the last log, each command type taking a share of the submits: its time a present, as timed, and how often.
static void log_commands(void)
{
	uint64_t ticks = 0;
	for (uint32_t s = 0; s < V4X_PROFILE_STAGES; s++)
		ticks += totals.ticks[s] - logged.ticks[s];
	const uint32_t presents = totals.presents - logged.presents;
	for (uint32_t type = 0; type < COMMAND_TYPES; type++) {
		const uint64_t taken = command_ticks[type] - logged_command_ticks[type];
		const uint32_t times = command_laps[type] - logged_command_laps[type];
		logged_command_ticks[type] = command_ticks[type];
		logged_command_laps[type] = command_laps[type];
		if (taken * 100 < ticks * COMMAND_LOG_HUNDREDTHS)
			continue;
		xsdk_log_write(
			"vulkan4xenon: profile, the last 10 s: command type %u (src/objects.h), %u us a present, %u "
			"a present",
			(unsigned)type, (unsigned)each(nanoseconds(taken) / 1000, presents), (unsigned)each(times, presents));
	}
}

// A wait's line: its time a present, and how many, in tenths.
static void log_wait(const char *what, uint64_t ticks, uint32_t count, uint32_t presents)
{
	const uint32_t tenths = each((uint64_t)count * 10, presents);
	xsdk_log_write("vulkan4xenon: profile, the last 10 s: %s, %u us a present, %u.%u a present", what,
		(unsigned)each(nanoseconds(ticks) / 1000, presents), (unsigned)(tenths / 10), (unsigned)(tenths % 10));
}

// Since the last log, the replay's waits for the GPU, all and at each site, then the app's waits for fences.
static void log_waits(void)
{
	static const char *const sites[V4X_PROFILE_WAIT_SITES] = {
		"of them for draws before a copy into a buffer",
		"for what a submission used",
		"for room for a pass past eDRAM",
	};
	const uint32_t presents = totals.presents - logged.presents;
	uint64_t ticks = 0;
	uint32_t count = 0;
	for (uint32_t s = 0; s < V4X_PROFILE_WAIT_SITES; s++) {
		ticks += wait_ticks[s] - logged_wait_ticks[s];
		count += waits[s] - logged_waits[s];
	}
	log_wait("the replay's waits for the GPU", ticks, count, presents);
	for (uint32_t s = 0; s < V4X_PROFILE_WAIT_SITES; s++) {
		log_wait(sites[s], wait_ticks[s] - logged_wait_ticks[s], waits[s] - logged_waits[s], presents);
		logged_wait_ticks[s] = wait_ticks[s];
		logged_waits[s] = waits[s];
	}
	log_wait("the app's waits for fences, outside the submit", fence_wait_ticks - logged_fence_wait_ticks,
		fence_waits - logged_fence_waits, presents);
	logged_fence_wait_ticks = fence_wait_ticks;
	logged_fence_waits = fence_waits;
}

// Since the last log, the GPU's time on the submits a present, by its clock at 498.75 ticks a microsecond.
static void log_gpu(void)
{
	read_gpu_clocks();
	const uint32_t presents = totals.presents - logged.presents;
	xsdk_log_write(
		"vulkan4xenon: profile, the last 10 s: the GPU's time on the submits, %u us a present, over %u "
		"submits it finished",
		(unsigned)each((gpu_ticks - logged_gpu_ticks) * 4 / 1995, presents),
		(unsigned)(gpu_counted - logged_gpu_counted));
	logged_gpu_ticks = gpu_ticks;
	logged_gpu_counted = gpu_counted;
}

// The passes traced, a line each: the image of its first attachment and its size, samples, colour attachments and
// the first tile it placed them at; its first attachment's load and store ops; its depth's; its area; what it did; the
// textures it pointed.
static void log_trace(void)
{
	xsdk_log_write(
		"vulkan4xenon: profile, %u passes over %u presents", (unsigned)trace_count, (unsigned)trace_presents);
	for (uint32_t i = 0; i < trace_count; i++) {
		const PassTrace *const p = &trace[i];
		xsdk_log_write(
			"vulkan4xenon: profile, pass %u: image %08X %ux%u s%u c%u tile %u%s; load %u store %u; depth "
			"%u load %u clear %u; area %d,%d %ux%u; drew back %u, clear resolves %u, %u draws, end resolves "
			"%u; textures %08X %08X %08X %08X; begin %u ns, end %u ns",
			(unsigned)i, (unsigned)p->image, (unsigned)p->width, (unsigned)p->height, (unsigned)p->samples,
			(unsigned)p->color_count, (unsigned)p->tile, p->tiled ? " tiled" : "", (unsigned)p->load_op,
			(unsigned)p->store_op, p->has_depth, p->loads_depth, p->clears_depth, (int)p->area.offset.x,
			(int)p->area.offset.y, (unsigned)p->area.extent.width, (unsigned)p->area.extent.height,
			(unsigned)p->events[V4X_PROFILE_DRAWN_BACK], (unsigned)p->events[V4X_PROFILE_CLEAR_RESOLVE],
			(unsigned)p->draws, (unsigned)p->events[V4X_PROFILE_END_RESOLVE], (unsigned)p->textures[0],
			(unsigned)p->textures[1], (unsigned)p->textures[2], (unsigned)p->textures[3],
			(unsigned)nanoseconds(p->begin_ticks), (unsigned)nanoseconds(p->end_ticks));
	}
}

// A count's line, in tenths a present.
static void log_count(const char *what, uint64_t count, uint32_t presents)
{
	const uint32_t tenths = each(count * 10, presents);
	xsdk_log_write("vulkan4xenon: profile, the last 10 s: %s, %u.%u a present", what, (unsigned)(tenths / 10),
		(unsigned)(tenths % 10));
}

// Since the last log: what passes did to eDRAM, and xsdk's waits for room in its ring, each a present.
static void log_edram(uint32_t presents)
{
	const V4xEdramCounts edram = v4x_edram_counts();
	XsdkGpuStats stats;
	xsdk_gpu_stats(&stats);
	log_count("images drawn back", edram.drawn_back - logged_edram.drawn_back, presents);
	log_count("resolves made to clear", edram.clear_resolves - logged_edram.clear_resolves, presents);
	log_count("resolves put off", edram.resolves_put_off - logged_edram.resolves_put_off, presents);
	log_count("put-off resolves made later", edram.late_resolves - logged_edram.late_resolves, presents);
	log_count("xsdk's waits for room in its ring", stats.waits - logged_gpu_stats.waits, presents);
	log_count("microseconds of them", stats.waited_us - logged_gpu_stats.waited_us, presents);
	logged_edram = edram;
	logged_gpu_stats = stats;
}

// Since the last log, untiled passes' begins and ends by part: microseconds a present and nanoseconds a pass.
static void log_pass_stages(uint32_t presents)
{
	static const char *const names[V4X_PROFILE_PASS_STAGES] = {
		"begins placing targets and forgetting what they reach",
		"begins setting targets",
		"begins drawing back and clearing",
		"begins keeping parts",
		"ends deciding stores",
		"ends resolving",
		"ends' rest",
	};
	const uint32_t begins = pass_begins - logged_pass_begins;
	for (uint32_t s = 0; s < V4X_PROFILE_PASS_STAGES; s++) {
		const uint64_t ns = nanoseconds(pass_stage_ticks[s] - logged_pass_stage_ticks[s]);
		xsdk_log_write("vulkan4xenon: profile, the last 10 s: passes' %s, %u us a present, %u ns a pass", names[s],
			(unsigned)each(ns / 1000, presents), (unsigned)each(ns, begins));
		logged_pass_stage_ticks[s] = pass_stage_ticks[s];
	}
	logged_pass_begins = pass_begins;
}

// Since the last log, each count a present.
static void log_counts(void)
{
	const uint32_t presents = totals.presents - logged.presents;
	log_edram(presents);
	log_pass_stages(presents);
	for (uint32_t c = 0; c < V4X_PROFILE_COUNTS; c++) {
		log_count(count_names[c], counts[c] - logged_counts[c], presents);
		logged_counts[c] = counts[c];
	}
}
