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

static void log_commands(void);

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
	logged = totals;
	logged_at = now;
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
