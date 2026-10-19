#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../tests/tiled_index.h"
#include "bench.h"
#include "probes.h"
#include "scenes.h"
#include "xsdk/gpu.h"
#include "xsdk/log.h"

// Runs every scene through each backend in turn, timing each frame's stages, and compares the two
// (docs/ROADMAP.md, milestone 5).

enum {
	WARMUP_FRAMES = 60,
	MEASURE_FRAMES = 600,
	BACKENDS = 2,
	// The pictures are compared at every 20th pixel across and down, from the 10th.
	GRID_COLUMNS = 64,
	GRID_ROWS = 36,
	GRID_STEP = 20,
	GRID_POINTS = GRID_COLUMNS * GRID_ROWS,
	PICTURE_TOLERANCE = 2,
	MAX_DIFFERENCES_LOGGED = 3,
	NANOSECONDS_PER_MICROSECOND = 1000,
};

typedef enum {
	METRIC_FRAME,
	METRIC_SIM,
	METRIC_DRAW,
	METRIC_SUBMIT,
	METRIC_GPU,
	METRIC_PRESENT,
	// The API's CPU cost, the number to compare (docs/ROADMAP.md, milestone 5).
	METRIC_DRAW_SUBMIT,
	METRICS,
} Metric;

static const char *const metric_names[METRICS] = { "frame", "sim", "draw", "submit", "gpu", "present", "draw_submit" };

static const SceneDef scenes[] = {
	{ "draws_250", SCENE_DRAWS, 250, NULL },
	{ "draws_1000", SCENE_DRAWS, 1000, NULL },
	{ "draws_4000", SCENE_DRAWS, 4000, NULL },
	{ "states_250", SCENE_STATES, 250, NULL },
	{ "states_1000", SCENE_STATES, 1000, NULL },
	{ "uniforms_1000", SCENE_UNIFORMS, 1000, NULL },
	{ "stream_1000", SCENE_STREAM, 1000, NULL },
	{ "stream_8000", SCENE_STREAM, 8000, NULL },
	{ "fill_4", SCENE_FILL, 4, NULL },
	{ "fill_16", SCENE_FILL, 16, NULL },
	{ "instances_4000", SCENE_INSTANCES, 4000, "draws_4000" },
};

enum {
	SCENES = sizeof scenes / sizeof scenes[0],
};

static const BenchBackend *const backends[BACKENDS] = { &bench_xsdk_backend, &bench_vulkan_backend };

typedef struct {
	uint32_t mean;
	uint32_t median;
	uint32_t p99;
} Stat;

static uint32_t samples[METRICS][MEASURE_FRAMES];
static uint32_t sorted[MEASURE_FRAMES];
static Stat results[BACKENDS][SCENES][METRICS];
static bool ran[BACKENDS][SCENES];
static uint32_t pictures[BACKENDS][SCENES][GRID_POINTS];
// What the ring did over the last scene's measured frames (xsdk:SCOPE.md, milestone 19, step 1).
static XsdkGpuStats ring_use;
static FILE *frames_csv;
static FILE *summary_csv;
static int passed_count;
static bool gpu_stopped;
static int failed_count;

static void check(const char *name, bool passed)
{
	xsdk_log_write("%s: %s", passed ? "PASS" : "FAIL", name);
	if (passed)
		passed_count++;
	else
		failed_count++;
}

uint64_t bench_now_ns(void)
{
	struct timespec now;
	(void)clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * 1000000000u + (uint64_t)now.tv_nsec;
}

static uint32_t microseconds(uint64_t nanoseconds)
{
	return (uint32_t)((nanoseconds + NANOSECONDS_PER_MICROSECOND / 2) / NANOSECONDS_PER_MICROSECOND);
}

static int compare_u32(const void *a, const void *b)
{
	const uint32_t x = *(const uint32_t *)a;
	const uint32_t y = *(const uint32_t *)b;
	return (x > y) - (x < y);
}

static Stat compute(const uint32_t *values)
{
	uint64_t sum = 0;
	memcpy(sorted, values, sizeof sorted);
	qsort(sorted, MEASURE_FRAMES, sizeof sorted[0], compare_u32);
	for (int i = 0; i < MEASURE_FRAMES; i++)
		sum += sorted[i];
	return (Stat){ .mean = (uint32_t)((sum + MEASURE_FRAMES / 2) / MEASURE_FRAMES),
		.median = sorted[MEASURE_FRAMES / 2],
		.p99 = sorted[MEASURE_FRAMES * 99 / 100] };
}

// Each frame: the scene's own work, the backend's draw calls, the submit, the GPU, then the present and the wait for
// the next image. False if the GPU stopped.
static bool measure(const BenchBackend *backend, const SceneDef *scene)
{
	uint64_t previous = bench_now_ns();
	XsdkGpuStats ring_start = { 0 };
	for (int f = 0; f < WARMUP_FRAMES + MEASURE_FRAMES; f++) {
		if (f == WARMUP_FRAMES)
			xsdk_gpu_stats(&ring_start);
		const uint64_t t0 = bench_now_ns();
		scene_update(scene, f);
		const uint64_t t1 = bench_now_ns();
		backend->begin_frame();
		scene_render(scene, backend);
		const uint64_t t2 = bench_now_ns();
		backend->submit_frame();
		const uint64_t t3 = bench_now_ns();
		if (!backend->wait_frame()) {
			xsdk_log_write("vkbench: %s %s: frame %d didn't finish within %d ms, so the GPU has stopped", backend->name,
				scene->name, f, FRAME_TIMEOUT_MS);
			return false;
		}
		const uint64_t t4 = bench_now_ns();
		backend->present_frame();
		const uint64_t t5 = bench_now_ns();
		if (f >= WARMUP_FRAMES) {
			const int i = f - WARMUP_FRAMES;
			samples[METRIC_FRAME][i] = microseconds(t0 - previous);
			samples[METRIC_SIM][i] = microseconds(t1 - t0);
			samples[METRIC_DRAW][i] = microseconds(t2 - t1);
			samples[METRIC_SUBMIT][i] = microseconds(t3 - t2);
			samples[METRIC_GPU][i] = microseconds(t4 - t3);
			samples[METRIC_PRESENT][i] = microseconds(t5 - t4);
			samples[METRIC_DRAW_SUBMIT][i] = microseconds(t3 - t1);
		}
		previous = t0;
	}
	xsdk_gpu_stats(&ring_use);
	ring_use.dwords -= ring_start.dwords;
	ring_use.kicks -= ring_start.kicks;
	ring_use.waits -= ring_start.waits;
	ring_use.waited_us -= ring_start.waited_us;
	return true;
}

static void write_frames(const BenchBackend *backend, const SceneDef *scene)
{
	if (!frames_csv)
		return;
	for (int i = 0; i < MEASURE_FRAMES; i++)
		fprintf(frames_csv, "%s,%s,%d,%u,%u,%u,%u,%u,%u\n", backend->name, scene->name, i,
			(unsigned)samples[METRIC_FRAME][i], (unsigned)samples[METRIC_SIM][i], (unsigned)samples[METRIC_DRAW][i],
			(unsigned)samples[METRIC_SUBMIT][i], (unsigned)samples[METRIC_GPU][i],
			(unsigned)samples[METRIC_PRESENT][i]);
}

static void report(uint32_t b, uint32_t s)
{
	const Stat *const r = results[b][s];
	const uint32_t fps_tenths = r[METRIC_FRAME].mean ? 10000000u / r[METRIC_FRAME].mean : 0;
	xsdk_log_write(
		"vkbench: %s %s: draw+submit %u us (mean %u, p99 %u): draw %u, submit %u; gpu %u (p99 %u); present "
		"%u; frame %u (%u.%u fps)",
		backends[b]->name, scenes[s].name, (unsigned)r[METRIC_DRAW_SUBMIT].median, (unsigned)r[METRIC_DRAW_SUBMIT].mean,
		(unsigned)r[METRIC_DRAW_SUBMIT].p99, (unsigned)r[METRIC_DRAW].median, (unsigned)r[METRIC_SUBMIT].median,
		(unsigned)r[METRIC_GPU].median, (unsigned)r[METRIC_GPU].p99, (unsigned)r[METRIC_PRESENT].median,
		(unsigned)r[METRIC_FRAME].median, (unsigned)(fps_tenths / 10), (unsigned)(fps_tenths % 10));
	if (!summary_csv)
		return;
	fprintf(summary_csv, "%s,%s,%d,%d", backends[b]->name, scenes[s].name, scenes[s].count, MEASURE_FRAMES);
	for (int m = 0; m < METRICS; m++)
		fprintf(summary_csv, ",%u,%u,%u", (unsigned)r[m].mean, (unsigned)r[m].median, (unsigned)r[m].p99);
	fprintf(summary_csv, ",%llu,%u,%u,%llu\n", (unsigned long long)ring_use.dwords, (unsigned)ring_use.kicks,
		(unsigned)ring_use.waits, (unsigned long long)ring_use.waited_us);
}

// Tenths of a frame's share of a count over the measured frames, so the log needs no floats.
static uint32_t tenths_a_frame(uint64_t total)
{
	return (uint32_t)((total * 10 + MEASURE_FRAMES / 2) / MEASURE_FRAMES);
}

static void report_ring(uint32_t b, uint32_t s)
{
	const uint32_t count = (uint32_t)scenes[s].count;
	const uint32_t dwords_tenths = (uint32_t)((ring_use.dwords * 10 / MEASURE_FRAMES + count / 2) / count);
	const uint32_t kicks = tenths_a_frame(ring_use.kicks);
	const uint32_t waits = tenths_a_frame(ring_use.waits);
	xsdk_log_write("vkbench: %s %s: a frame: %u dwords, %u.%u for each of %u; %u.%u kicks; %u.%u waits, %u us waiting",
		backends[b]->name, scenes[s].name, (unsigned)(ring_use.dwords / MEASURE_FRAMES), (unsigned)(dwords_tenths / 10),
		(unsigned)(dwords_tenths % 10), (unsigned)count, (unsigned)(kicks / 10), (unsigned)(kicks % 10),
		(unsigned)(waits / 10), (unsigned)(waits % 10), (unsigned)(ring_use.waited_us / MEASURE_FRAMES));
}

// The last frame's picture, at the grid's points, as front buffer words: bytes B, G, R, A.
static void sample_picture(const BenchBackend *backend, uint32_t *picture)
{
	volatile const uint32_t *const pixels = backend->presented_pixels();
	for (uint32_t y = 0; y < GRID_ROWS; y++) {
		for (uint32_t x = 0; x < GRID_COLUMNS; x++)
			picture[y * GRID_COLUMNS + x] =
				pixels[tiled_index(x * GRID_STEP + GRID_STEP / 2, y * GRID_STEP + GRID_STEP / 2, SCREEN_WIDTH)];
	}
}

static bool run_scene(uint32_t b, uint32_t s)
{
	const BenchBackend *const backend = backends[b];
	const SceneDef *const scene = &scenes[s];
	scene_setup(scene);
	(void)backend->finish();
	if (!measure(backend, scene)) {
		gpu_stopped = true;
		return false;
	}
	const uint32_t errors = backend->finish();
	if (errors)
		xsdk_log_write("vkbench: %s %s: %u calls failed", backend->name, scene->name, (unsigned)errors);
	for (int m = 0; m < METRICS; m++)
		results[b][s][m] = compute(samples[m]);
	report(b, s);
	report_ring(b, s);
	write_frames(backend, scene);
	sample_picture(backend, pictures[b][s]);
	ran[b][s] = true;
	return errors == 0;
}

static void run_backend(uint32_t b)
{
	const BenchBackend *const backend = backends[b];
	char name[96];
	const bool opened = backend->init();
	(void)snprintf(name, sizeof name, "vkbench: %s: opened", backend->name);
	check(name, opened);
	bool clean = opened;
	for (uint32_t s = 0; s < SCENES && opened && !gpu_stopped; s++)
		clean = run_scene(b, s) && clean;
	backend->shutdown();
	(void)snprintf(name, sizeof name, "vkbench: %s: every scene ran its %d frames with no call failing", backend->name,
		WARMUP_FRAMES + MEASURE_FRAMES);
	check(name, clean);
}

static bool near(uint32_t a, uint32_t b)
{
	for (uint32_t shift = 8; shift < 32; shift += 8) {
		const int difference = (int)(a >> shift & 0xFF) - (int)(b >> shift & 0xFF);
		if (difference > PICTURE_TOLERANCE || difference < -PICTURE_TOLERANCE)
			return false;
	}
	return true;
}

// Hundredths of a ratio, so the log needs no floats.
static uint32_t ratio_hundredths(uint32_t numerator, uint32_t denominator)
{
	return denominator ? (uint32_t)(((uint64_t)numerator * 100 + denominator / 2) / denominator) : 0;
}

// Run b's picture and medians against run a's: the first differing points, then b's times against a's; how many points
// differ.
static uint32_t compare_runs(const char *what, uint32_t a_backend, uint32_t a_scene, const char *a_name,
	uint32_t b_backend, uint32_t b_scene, const char *b_name)
{
	const uint32_t *const a_picture = pictures[a_backend][a_scene];
	const uint32_t *const b_picture = pictures[b_backend][b_scene];
	uint32_t differing = 0;
	for (uint32_t i = 0; i < GRID_POINTS; i++) {
		if (near(a_picture[i], b_picture[i]))
			continue;
		if (differing++ < MAX_DIFFERENCES_LOGGED)
			xsdk_log_write("vkbench: %s: at (%u, %u) %s drew %08X, %s %08X", what,
				(unsigned)(i % GRID_COLUMNS * GRID_STEP + GRID_STEP / 2),
				(unsigned)(i / GRID_COLUMNS * GRID_STEP + GRID_STEP / 2), a_name, (unsigned)a_picture[i], b_name,
				(unsigned)b_picture[i]);
	}
	const Stat *const a = results[a_backend][a_scene];
	const Stat *const b = results[b_backend][b_scene];
	const uint32_t cpu = ratio_hundredths(b[METRIC_DRAW_SUBMIT].median, a[METRIC_DRAW_SUBMIT].median);
	const uint32_t gpu = ratio_hundredths(b[METRIC_GPU].median, a[METRIC_GPU].median);
	xsdk_log_write(
		"vkbench: %s: draw+submit %u us against %u, %u.%02ux; gpu %u us against %u, %u.%02ux; %u of %u "
		"points differ",
		what, (unsigned)b[METRIC_DRAW_SUBMIT].median, (unsigned)a[METRIC_DRAW_SUBMIT].median, (unsigned)(cpu / 100),
		(unsigned)(cpu % 100), (unsigned)b[METRIC_GPU].median, (unsigned)a[METRIC_GPU].median, (unsigned)(gpu / 100),
		(unsigned)(gpu % 100), (unsigned)differing, (unsigned)GRID_POINTS);
	return differing;
}

static void compare(uint32_t s)
{
	if (!ran[0][s] || !ran[1][s])
		return;
	const uint32_t differing = compare_runs(scenes[s].name, 0, s, "xsdk", 1, s, "vulkan4xenon");
	char name[96];
	(void)snprintf(name, sizeof name, "vkbench: %s: both backends drew the same picture", scenes[s].name);
	check(name, differing == 0);
}

// Through each backend, scene s against scene t, the one whose picture it draws.
static void compare_scenes(uint32_t s, uint32_t t)
{
	for (uint32_t b = 0; b < BACKENDS; b++) {
		if (!ran[b][s] || !ran[b][t])
			continue;
		char what[96];
		(void)snprintf(what, sizeof what, "%s: %s against %s", backends[b]->name, scenes[s].name, scenes[t].name);
		const uint32_t differing = compare_runs(what, b, t, scenes[t].name, b, s, scenes[s].name);
		char name[96];
		(void)snprintf(
			name, sizeof name, "vkbench: %s: %s drew %s's picture", backends[b]->name, scenes[s].name, scenes[t].name);
		check(name, differing == 0);
	}
}

static void open_results(void)
{
	frames_csv = fopen("game:\\vkbench_frames.csv", "w");
	summary_csv = fopen("game:\\vkbench_summary.csv", "w");
	if (!frames_csv || !summary_csv)
		xsdk_log_write("vkbench: the CSVs can't be written; the log has the summary");
	if (frames_csv)
		fprintf(frames_csv, "backend,scene,frame,frame_us,sim_us,draw_us,submit_us,gpu_us,present_us\n");
	if (!summary_csv)
		return;
	fprintf(summary_csv, "backend,scene,count,frames");
	for (int m = 0; m < METRICS; m++)
		fprintf(summary_csv, ",%s_mean_us,%s_median_us,%s_p99_us", metric_names[m], metric_names[m], metric_names[m]);
	// The ring's counts over all the measured frames.
	fprintf(summary_csv, ",ring_dwords,ring_kicks,ring_waits,ring_waited_us\n");
}

int main(void)
{
	xsdk_log_write("vkbench: %u scenes, each %d warm-up and %d measured frames, through xsdk then vulkan4xenon",
		(unsigned)SCENES, WARMUP_FRAMES, MEASURE_FRAMES);
	open_results();
	probes_run();
	for (uint32_t b = 0; b < BACKENDS && !gpu_stopped; b++)
		run_backend(b);
	if (gpu_stopped)
		xsdk_log_write("vkbench: the GPU stopped, so the rest didn't run");
	xsdk_log_write("vkbench: vulkan4xenon against xsdk, medians");
	for (uint32_t s = 0; s < SCENES; s++)
		compare(s);
	xsdk_log_write("vkbench: scenes against the scene whose picture they draw, through each backend, medians");
	for (uint32_t s = 0; s < SCENES; s++) {
		for (uint32_t t = 0; scenes[s].picture_of && t < SCENES; t++) {
			if (!strcmp(scenes[t].name, scenes[s].picture_of))
				compare_scenes(s, t);
		}
	}
	if (frames_csv)
		fclose(frames_csv);
	if (summary_csv)
		fclose(summary_csv);
	xsdk_log_write("tests: %d passed, %d failed", passed_count, failed_count);
	return failed_count;
}
