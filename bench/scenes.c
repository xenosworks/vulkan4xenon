#include "scenes.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

#include "bench.h"
#include "vmath.h"

// vkbench's scenes, in Vulkan's clip space, y down and depth 0 to 1, for both backends.

#define ASPECT        ((float)SCREEN_WIDTH / (float)SCREEN_HEIGHT)
#define SPRITE_HALF   0.03f
#define FRAME_SECONDS (1.0f / 60.0f)

static DrawItem items[MAX_DRAW_ITEMS];
static float grid_x[MAX_DRAW_ITEMS];
static float grid_y[MAX_DRAW_ITEMS];
static float cube_scale;
static float projection[16];

static SpriteVertex sprite_vertices[MAX_SPRITES * 4];
static float sprite_px[MAX_SPRITES];
static float sprite_py[MAX_SPRITES];
static float sprite_vx[MAX_SPRITES];
static float sprite_vy[MAX_SPRITES];
static float sprite_color[MAX_SPRITES][4];

static uint32_t rng_state;

static uint32_t rng_next(void)
{
	rng_state = rng_state * 1664525u + 1013904223u;
	return rng_state;
}

static float rng_float(void)
{
	return (float)(rng_next() >> 8) * (1.0f / 16777216.0f);
}

static void setup_projection(void)
{
	// Flips y and moves depth from -1..1 to 0..1.
	static const float gl_to_vulkan[16] = { 1, 0, 0, 0, 0, -1, 0, 0, 0, 0, 0.5f, 0, 0, 0, 0.5f, 1 };
	mat4_ortho(projection, -ASPECT, ASPECT, -1.0f, 1.0f, -10.0f, 10.0f);
	mat4_mul(projection, gl_to_vulkan, projection);
}

static void setup_meshes(const SceneDef *scene)
{
	const int n = scene->count;
	const int cols = (int)ceilf(sqrtf((float)n * ASPECT));
	const int rows = (n + cols - 1) / cols;
	const float cell = fminf(2.0f * ASPECT / (float)cols, 2.0f / (float)rows);
	const float x0 = -0.5f * cell * (float)(cols - 1);
	const float y0 = 0.5f * cell * (float)(rows - 1);
	cube_scale = cell * 0.6f;
	setup_projection();
	for (int i = 0; i < n; i++) {
		DrawItem *const item = &items[i];
		grid_x[i] = x0 + (float)(i % cols) * cell;
		grid_y[i] = y0 - (float)(i / cols) * cell;
		// The texture changes every draw, the shader every second and blending every fourth.
		const bool states = scene->kind == SCENE_STATES;
		item->texture = states ? (uint8_t)(i & 3) : 0;
		item->grayscale = states ? (uint8_t)((i >> 1) & 1) : 0;
		item->blend = states ? (uint8_t)((i >> 2) & 1) : 0;
		item->tint[0] = item->tint[1] = item->tint[2] = 1.0f;
		item->tint[3] = item->blend ? 0.6f : 1.0f;
	}
}

static void update_meshes(const SceneDef *scene, int frame)
{
	const float t = (float)frame * FRAME_SECONDS;
	float rotation[16];
	float shared[16];
	// One shared rotation keeps the CPU's work tiny, so the API's cost shows.
	mat4_rotation_yx(rotation, t * 1.3f, t * 0.9f, cube_scale);
	mat4_mul(shared, projection, rotation);
	for (int i = 0; i < scene->count; i++) {
		float *const m = items[i].mvp;
		memcpy(m, shared, sizeof shared);
		m[12] = projection[0] * grid_x[i] + projection[12];
		m[13] = projection[5] * grid_y[i] + projection[13];
	}
}

// Clip space's y points down, so sprites mirror as vkbench's Vulkan build does.
static void setup_sprites(const SceneDef *scene)
{
	rng_state = 0x5EED1234u;
	for (int i = 0; i < scene->count; i++) {
		sprite_px[i] = (rng_float() * 2.0f - 1.0f) * (1.0f - SPRITE_HALF);
		sprite_py[i] = -(rng_float() * 2.0f - 1.0f) * (1.0f - SPRITE_HALF * ASPECT);
		sprite_vx[i] = (rng_float() * 2.0f - 1.0f) * 0.8f;
		sprite_vy[i] = -(rng_float() * 2.0f - 1.0f) * 0.8f;
		for (int c = 0; c < 3; c++)
			sprite_color[i][c] = (float)(64 + (rng_next() & 191)) / 255.0f;
		sprite_color[i][3] = 200.0f / 255.0f;
	}
}

static void update_sprites(const SceneDef *scene)
{
	const float hx = SPRITE_HALF;
	const float hy = SPRITE_HALF * ASPECT;
	const float dy = -hy;
	for (int i = 0; i < scene->count; i++) {
		float px = sprite_px[i] + sprite_vx[i] * FRAME_SECONDS;
		float py = sprite_py[i] + sprite_vy[i] * FRAME_SECONDS;
		if (px < -1.0f + hx || px > 1.0f - hx) {
			sprite_vx[i] = -sprite_vx[i];
			px = fminf(fmaxf(px, -1.0f + hx), 1.0f - hx);
		}
		if (py < -1.0f + hy || py > 1.0f - hy) {
			sprite_vy[i] = -sprite_vy[i];
			py = fminf(fmaxf(py, -1.0f + hy), 1.0f - hy);
		}
		sprite_px[i] = px;
		sprite_py[i] = py;
		const float *const c = sprite_color[i];
		SpriteVertex *const v = &sprite_vertices[i * 4];
		v[0] = (SpriteVertex){ px - hx, py - dy, 0.0f, 1.0f, { c[0], c[1], c[2], c[3] } };
		v[1] = (SpriteVertex){ px + hx, py - dy, 1.0f, 1.0f, { c[0], c[1], c[2], c[3] } };
		v[2] = (SpriteVertex){ px - hx, py + dy, 0.0f, 0.0f, { c[0], c[1], c[2], c[3] } };
		v[3] = (SpriteVertex){ px + hx, py + dy, 1.0f, 0.0f, { c[0], c[1], c[2], c[3] } };
	}
}

void scene_setup(const SceneDef *scene)
{
	switch (scene->kind) {
	case SCENE_DRAWS:
	case SCENE_STATES:
	case SCENE_UNIFORMS:
	case SCENE_INSTANCES:
		setup_meshes(scene);
		break;
	case SCENE_STREAM:
		setup_sprites(scene);
		break;
	case SCENE_FILL:
		break;
	}
}

void scene_update(const SceneDef *scene, int frame)
{
	switch (scene->kind) {
	case SCENE_DRAWS:
	case SCENE_STATES:
	case SCENE_UNIFORMS:
	case SCENE_INSTANCES:
		update_meshes(scene, frame);
		break;
	case SCENE_STREAM:
		update_sprites(scene);
		break;
	case SCENE_FILL:
		break;
	}
}

void scene_render(const SceneDef *scene, const BenchBackend *backend)
{
	switch (scene->kind) {
	case SCENE_DRAWS:
	case SCENE_STATES:
		backend->draw_meshes(items, scene->count, false);
		break;
	case SCENE_UNIFORMS:
		backend->draw_meshes(items, scene->count, true);
		break;
	case SCENE_INSTANCES:
		backend->draw_instances(items, scene->count);
		break;
	case SCENE_STREAM:
		backend->draw_sprites(sprite_vertices, scene->count);
		break;
	case SCENE_FILL:
		backend->draw_fullscreen(scene->count);
		break;
	}
}
