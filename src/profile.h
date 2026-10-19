#ifndef V4X_PROFILE_H
#define V4X_PROFILE_H

#include <stdint.h>

#include "vulkan4xenon.h"

// The submit's timing, in a build made with make V4X_PROFILE=1; without it every hook is empty, and compiles to
// nothing. A lap charges the time since the last lap to a stage, so the stages add up to the submit.

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
void v4x_profile_wait_end(void);
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

static inline void v4x_profile_wait_end(void)
{
}
#endif

#endif
