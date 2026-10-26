#include "profile.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

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
};

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
};
// When the pass past eDRAM's begin or end being timed started, and whether a wait now falls in its begin.
static uint32_t tiles_began;
static bool in_tiled_begin;

static void log_commands(void);
static void log_waits(void);
static void log_counts(void);

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
	const time_t now = seconds();
	if (now - logged_at < LOG_SECONDS)
		return;
	v4x_profile_log("the last 10 s", &logged, &totals);
	log_commands();
	log_waits();
	log_counts();
	logged = totals;
	logged_at = now;
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

// Since the last log, each count a present.
static void log_counts(void)
{
	const uint32_t presents = totals.presents - logged.presents;
	for (uint32_t c = 0; c < V4X_PROFILE_COUNTS; c++) {
		const uint32_t tenths = each((counts[c] - logged_counts[c]) * 10, presents);
		xsdk_log_write("vulkan4xenon: profile, the last 10 s: %s, %u.%u a present", count_names[c],
			(unsigned)(tenths / 10), (unsigned)(tenths % 10));
		logged_counts[c] = counts[c];
	}
}
