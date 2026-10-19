#ifndef V4X_BENCH_SCENES_H
#define V4X_BENCH_SCENES_H

#include "bench.h"

typedef enum {
	// Many draws, no state changes.
	SCENE_DRAWS,
	// Many draws, with state changes between them.
	SCENE_STATES,
	// Many draws, each one's matrix and tint from dynamic uniform buffers.
	SCENE_UNIFORMS,
	// SCENE_DRAWS's cubes as the instances of one draw (xsdk:docs/scope/47-instancing.md, step 5).
	SCENE_INSTANCES,
	// Sprites moved and written out every frame.
	SCENE_STREAM,
	// Full-screen layers, bound by the GPU.
	SCENE_FILL,
} SceneKind;

typedef struct {
	const char *name;
	SceneKind kind;
	int count;
	// The scene whose picture this one draws through each backend, or NULL.
	const char *picture_of;
} SceneDef;

void scene_setup(const SceneDef *scene);
// Steps by frame number, so both backends get the same data.
void scene_update(const SceneDef *scene, int frame);
void scene_render(const SceneDef *scene, const BenchBackend *backend);

#endif
