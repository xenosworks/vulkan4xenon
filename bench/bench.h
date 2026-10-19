#ifndef V4X_BENCH_H
#define V4X_BENCH_H

#include <stdbool.h>
#include <stdint.h>

// vkbench: the same scenes through xsdk's GPU layer and through vulkan4xenon (docs/ROADMAP.md, milestone 5).

enum {
	SCREEN_WIDTH = 1280,
	SCREEN_HEIGHT = 720,
	// A frame the GPU hasn't finished by then never will: it has stopped.
	FRAME_TIMEOUT_MS = 1000,
	MAX_DRAW_ITEMS = 4096,
	// 4 vertices each; 6 indices each stay within a draw's 65535.
	MAX_SPRITES = 8192,
	TEXTURE_COUNT = 4,
	TEXTURE_SIZE = 64,
	CUBE_VERTEX_COUNT = 24,
	CUBE_INDEX_COUNT = 36,
	MVP_FLOATS = 16,
	TINT_FLOATS = 4,
};

typedef struct {
	float x, y, z;
	float u, v;
} MeshVertex;

// vkbench's colour is a byte each; vulkan4xenon takes only 32-bit float vertex formats (docs/SUPPORT.md).
typedef struct {
	float x, y;
	float u, v;
	float color[4];
} SpriteVertex;

typedef struct {
	// Column-major.
	float mvp[MVP_FLOATS];
	float tint[TINT_FLOATS];
	uint8_t texture;
	uint8_t grayscale;
	uint8_t blend;
} DrawItem;

// Backends skip state that hasn't changed, like a typical engine.
typedef struct {
	const char *name;
	// Opens the GPU, the boot logo and the display, and waits for the first image to draw into. Without a display, as
	// in Xenia, it draws into an image of the screen's size and shows nothing.
	bool (*init)(void);
	// Starts a frame, cleared to the background and depth 1.
	void (*begin_frame)(void);
	// One draw each; with uniform buffers, each draw's matrix and tint come from a dynamic uniform buffer.
	void (*draw_meshes)(const DrawItem *items, int count, bool uniform_buffers);
	// One indexed draw of count instances, each item an instance's element, its matrix and tint at the instance's rate.
	void (*draw_instances)(const DrawItem *items, int count);
	void (*draw_sprites)(const SpriteVertex *vertices, int sprite_count);
	void (*draw_fullscreen)(int layers);
	// Hands the frame to the GPU and kicks it.
	void (*submit_frame)(void);
	// Spins until the GPU finishes the frame; false if it hasn't within FRAME_TIMEOUT_MS.
	bool (*wait_frame)(void);
	// Queues the frame to show and waits for the next image.
	void (*present_frame)(void);
	// The CPU's view of the image the last frame was drawn into: tiled, 1280 pixels across, bytes B, G, R, A.
	volatile const uint32_t *(*presented_pixels)(void);
	// Waits for the GPU, and returns how many calls failed since the last time.
	uint32_t (*finish)(void);
	void (*shutdown)(void);
} BenchBackend;

extern const BenchBackend bench_xsdk_backend;

// The monotonic clock, in nanoseconds.
uint64_t bench_now_ns(void);
extern const BenchBackend bench_vulkan_backend;

#endif
