#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "assets.h"
#include "bench.h"
#include "shaders.h"
#include "vulkan4xenon.h"
#include "xsdk/gpu.h"
#include "xsdk/log.h"
#include "xsdk/present.h"
#include "xsdk/shader.h"
#include "xsdk/splash.h"
#include "xsdk/texture.h"

// The scenes straight on xsdk's GPU layer, as an xsdk title would draw them, with the shaders vulkan4xenon uses.

enum {
	BUFFERS = 3,
	SWAP_INTERVAL = 1,
	TIMEOUT_MS = 10000,
	// The background, 26 26 31, as the Vulkan build clears to it.
	CLEAR_ARGB = 0xFF1A1A1F,
	MESH_STRIDE_BYTES = sizeof(MeshVertex),
	SPRITE_STRIDE_BYTES = sizeof(SpriteVertex),
	STREAM_BYTES = MAX_SPRITES * 4 * sizeof(SpriteVertex),
	QUAD_INDEX_BYTES = MAX_SPRITES * QUAD_INDICES * 2,
	FNV_OFFSET_BASIS_HIGH = 0xCBF29CE4,
	FNV_OFFSET_BASIS_LOW = 0x84222325,
	FNV_PRIME_HIGH = 0x00000100,
	FNV_PRIME_LOW = 0x000001B3,
};

typedef enum {
	SHADER_MESH_VERTEX,
	SHADER_MESH_PIXEL,
	SHADER_MESH_GRAY_PIXEL,
	SHADER_UNIFORM_VERTEX,
	SHADER_UNIFORM_PIXEL,
	SHADER_SPRITE_VERTEX,
	SHADER_SPRITE_PIXEL,
	SHADER_INSTANCED_VERTEX,
	SHADERS,
} ShaderKind;

typedef enum {
	PROGRAM_MESH,
	PROGRAM_MESH_GRAY,
	PROGRAM_UNIFORM,
	PROGRAM_SPRITE,
	PROGRAM_INSTANCED,
	PROGRAMS,
} ProgramKind;

typedef struct {
	const uint32_t *spirv;
	uint32_t size_bytes;
	// The bundle's variant compiled with xost's instanced option.
	bool instanced;
} ShaderSpirv;

static const ShaderSpirv spirv[SHADERS] = {
	[SHADER_MESH_VERTEX] = { mesh_vert_spirv, MESH_VERT_SPIRV_BYTES, false },
	[SHADER_MESH_PIXEL] = { mesh_frag_spirv, MESH_FRAG_SPIRV_BYTES, false },
	[SHADER_MESH_GRAY_PIXEL] = { mesh_gray_frag_spirv, MESH_GRAY_FRAG_SPIRV_BYTES, false },
	[SHADER_UNIFORM_VERTEX] = { mesh_uniform_vert_spirv, MESH_UNIFORM_VERT_SPIRV_BYTES, false },
	[SHADER_UNIFORM_PIXEL] = { mesh_uniform_frag_spirv, MESH_UNIFORM_FRAG_SPIRV_BYTES, false },
	[SHADER_SPRITE_VERTEX] = { sprite_vert_spirv, SPRITE_VERT_SPIRV_BYTES, false },
	[SHADER_SPRITE_PIXEL] = { sprite_frag_spirv, SPRITE_FRAG_SPIRV_BYTES, false },
	[SHADER_INSTANCED_VERTEX] = { mesh_instanced_vert_spirv, MESH_INSTANCED_VERT_SPIRV_BYTES, true },
};

// The instanced cubes take sprite.frag: the texture times the colour, here each instance's tint.
static const ShaderKind program_shaders[PROGRAMS][2] = {
	[PROGRAM_MESH] = { SHADER_MESH_VERTEX, SHADER_MESH_PIXEL },
	[PROGRAM_MESH_GRAY] = { SHADER_MESH_VERTEX, SHADER_MESH_GRAY_PIXEL },
	[PROGRAM_UNIFORM] = { SHADER_UNIFORM_VERTEX, SHADER_UNIFORM_PIXEL },
	[PROGRAM_SPRITE] = { SHADER_SPRITE_VERTEX, SHADER_SPRITE_PIXEL },
	[PROGRAM_INSTANCED] = { SHADER_INSTANCED_VERTEX, SHADER_SPRITE_PIXEL },
};

typedef struct {
	const V4xShader *sources[SHADERS];
	XsdkShader shaders[SHADERS];
	volatile uint32_t *cube_vertices;
	volatile uint32_t *cube_indices;
	volatile uint32_t *quad_indices;
	volatile uint32_t *fullscreen;
	volatile uint32_t *stream;
	// instances_4000's: each instance's DrawItem, and the cube's indices in copies an instance each.
	volatile uint32_t *instances;
	volatile uint32_t *cube_copies_memory;
	XsdkRepeatedIndices cube_copies;
	XsdkTexture textures[TEXTURE_COUNT];
	XsdkSamplerState sampler;
	XsdkBlendState opaque;
	XsdkBlendState alpha;
	XsdkDepthStencilState depth_test;
	XsdkDepthStencilState no_depth;
	XsdkRasterState raster;
	uint32_t front_address;
	uint32_t presented_address;
	// Without a display, frames go into this instead of a front buffer.
	volatile uint32_t *offscreen;
	uint32_t fence;
	// What's set, so draws skip what hasn't changed; reset each frame, as setting the target resets the state.
	int program;
	int texture;
	int blend;
	int depth;
	int vertices;
	bool opened;
} State;

static State state;

static bool headless(void)
{
	return state.offscreen != NULL;
}

// The layer's key for a shader, an FNV-1a of its SPIR-V's bytes, low first (src/shader.c).
static uint64_t spirv_hash(const uint32_t *code, uint32_t size_bytes)
{
	const uint64_t prime = (uint64_t)FNV_PRIME_HIGH << 32 | FNV_PRIME_LOW;
	uint64_t hash = (uint64_t)FNV_OFFSET_BASIS_HIGH << 32 | FNV_OFFSET_BASIS_LOW;
	for (uint32_t i = 0; i < size_bytes / 4; i++) {
		for (uint32_t shift = 0; shift < 32; shift += 8) {
			hash ^= code[i] >> shift & 0xFF;
			hash *= prime;
		}
	}
	return hash;
}

static const V4xShader *find_shader(const ShaderSpirv *code)
{
	const uint64_t hash = spirv_hash(code->spirv, code->size_bytes);
	for (uint32_t i = 0; i < v4x_shader_bundle.count; i++) {
		const V4xShader *const shader = &v4x_shader_bundle.shaders[i];
		if (shader->spirv_hash == hash && shader->spirv_size_bytes == code->size_bytes &&
			shader->instanced == code->instanced)
			return shader;
	}
	return NULL;
}

// The first register of a shader's push constants or uniform block, which xost placed (docs/SUPPORT.md).
static uint32_t block_register(const V4xShader *shader)
{
	for (uint32_t i = 0; i < shader->resource_count; i++) {
		if (shader->resources[i].kind != V4X_RESOURCE_SAMPLER)
			return shader->resources[i].first;
	}
	return 0;
}

// Locations 0, 1 and 2 are the position, colour and first texture coordinate usages to xost
// (xost:xsc/resolve.py:158-162).
static bool lay_out(XsdkShader *shader, bool sprite)
{
	const XsdkVertexElement mesh[] = {
		{ .usage = XSDK_USAGE_POSITION, .format = XSDK_VERTEX_FLOAT3, .offset_bytes = 0 },
		{ .usage = XSDK_USAGE_COLOR, .format = XSDK_VERTEX_FLOAT2, .offset_bytes = 12 },
	};
	const XsdkVertexElement sprites[] = {
		{ .usage = XSDK_USAGE_POSITION, .format = XSDK_VERTEX_FLOAT2, .offset_bytes = 0 },
		{ .usage = XSDK_USAGE_COLOR, .format = XSDK_VERTEX_FLOAT2, .offset_bytes = 8 },
		{ .usage = XSDK_USAGE_TEXCOORD, .format = XSDK_VERTEX_FLOAT4, .offset_bytes = 16 },
	};
	const uint32_t strides[XSDK_VERTEX_STREAMS] = { sprite ? SPRITE_STRIDE_BYTES : MESH_STRIDE_BYTES };
	const NTSTATUS status = sprite ? xsdk_shader_set_vertex_layout(shader, sprites, 3, strides)
								   : xsdk_shader_set_vertex_layout(shader, mesh, 2, strides);
	if (!NT_SUCCESS(status))
		xsdk_log_write("vkbench: xsdk_shader_set_vertex_layout failed: 0x%08X", (unsigned)status);
	return NT_SUCCESS(status);
}

// The mesh's two at the vertex's rate, then stream 1's DrawItems at the instance's: the matrix's columns and the tint,
// locations 2 to 6, the texture coordinates 0 to 4 to xost (src/pipeline.c, location_usage).
static bool lay_out_instances(XsdkShader *shader)
{
	_Static_assert(offsetof(DrawItem, tint) == MVP_FLOATS * 4, "the tint follows the matrix");
	enum { COLUMN_BYTES = 16, PER_INSTANCE = 5 };
	XsdkVertexElement elements[2 + PER_INSTANCE] = {
		{ .usage = XSDK_USAGE_POSITION, .format = XSDK_VERTEX_FLOAT3, .offset_bytes = 0 },
		{ .usage = XSDK_USAGE_COLOR, .format = XSDK_VERTEX_FLOAT2, .offset_bytes = 12 },
	};
	for (uint32_t i = 0; i < PER_INSTANCE; i++)
		elements[2 + i] = (XsdkVertexElement){ .usage = XSDK_USAGE_TEXCOORD,
			.usage_index = i,
			.format = XSDK_VERTEX_FLOAT4,
			.stream = 1,
			.offset_bytes = i * COLUMN_BYTES,
			.per_instance = true };
	const uint32_t strides[XSDK_VERTEX_STREAMS] = { MESH_STRIDE_BYTES, sizeof(DrawItem) };
	const NTSTATUS status = xsdk_shader_set_vertex_layout(shader, elements, 2 + PER_INSTANCE, strides);
	if (!NT_SUCCESS(status))
		xsdk_log_write("vkbench: xsdk_shader_set_vertex_layout failed for the instances: 0x%08X", (unsigned)status);
	return NT_SUCCESS(status);
}

static bool load_shaders(void)
{
	for (uint32_t i = 0; i < SHADERS; i++) {
		state.sources[i] = find_shader(&spirv[i]);
		if (!state.sources[i]) {
			xsdk_log_write("vkbench: shader %u isn't in the bundle", (unsigned)i);
			return false;
		}
		const NTSTATUS status =
			xsdk_shader_load(&state.shaders[i], state.sources[i]->container, state.sources[i]->container_size_bytes);
		if (!NT_SUCCESS(status)) {
			xsdk_log_write("vkbench: xsdk_shader_load failed: 0x%08X", (unsigned)status);
			return false;
		}
	}
	return lay_out(&state.shaders[SHADER_MESH_VERTEX], false) &&
		lay_out(&state.shaders[SHADER_UNIFORM_VERTEX], false) && lay_out(&state.shaders[SHADER_SPRITE_VERTEX], true) &&
		lay_out_instances(&state.shaders[SHADER_INSTANCED_VERTEX]);
}

// The cube's indices in copies of 64, the count the layer repeats an app's indices in, which met milestone 47's cost
// targets (xsdk:docs/HARDWARE.md, "Instancing on the console, milestone 47 step 1").
static bool make_cube_copies(const uint16_t indices[CUBE_INDEX_COUNT])
{
	enum { COPIES = 64 };
	uint32_t wide[CUBE_INDEX_COUNT];
	for (uint32_t i = 0; i < CUBE_INDEX_COUNT; i++)
		wide[i] = indices[i];
	const uint32_t size_bytes = xsdk_gpu_repeated_index_bytes(wide, CUBE_INDEX_COUNT, COPIES);
	state.cube_copies_memory = xsdk_gpu_alloc(size_bytes);
	if (!state.cube_copies_memory)
		return false;
	xsdk_gpu_repeat_indices(&state.cube_copies, state.cube_copies_memory, wide, CUBE_INDEX_COUNT, COPIES);
	__asm__ volatile("sync" ::: "memory");
	xsdk_gpu_invalidate(state.cube_copies.address, size_bytes);
	return true;
}

static volatile uint32_t *gpu_copy(const void *data, uint32_t size_bytes)
{
	volatile uint32_t *const memory = xsdk_gpu_alloc(size_bytes);
	if (!memory)
		return NULL;
	memcpy((void *)memory, data, size_bytes);
	__asm__ volatile("sync" ::: "memory");
	xsdk_gpu_invalidate(xsdk_gpu_address(memory), size_bytes);
	return memory;
}

static bool make_buffers(void)
{
	MeshVertex cube[CUBE_VERTEX_COUNT];
	uint16_t cube_indices[CUBE_INDEX_COUNT];
	static uint16_t quad_indices[MAX_SPRITES * QUAD_INDICES];
	SpriteVertex fullscreen[FULLSCREEN_VERTICES];
	assets_build_cube(cube, cube_indices);
	assets_build_quad_indices(quad_indices, MAX_SPRITES);
	assets_build_fullscreen_quad(fullscreen);
	state.cube_vertices = gpu_copy(cube, sizeof cube);
	state.cube_indices = gpu_copy(cube_indices, sizeof cube_indices);
	state.quad_indices = gpu_copy(quad_indices, QUAD_INDEX_BYTES);
	state.fullscreen = gpu_copy(fullscreen, sizeof fullscreen);
	state.stream = xsdk_gpu_alloc(STREAM_BYTES);
	state.instances = xsdk_gpu_alloc(MAX_DRAW_ITEMS * sizeof(DrawItem));
	return state.cube_vertices && state.cube_indices && state.quad_indices && state.fullscreen && state.stream &&
		state.instances && make_cube_copies(cube_indices);
}

static bool make_textures(void)
{
	static uint8_t pixels[TEXTURE_BYTES];
	for (int i = 0; i < TEXTURE_COUNT; i++) {
		const NTSTATUS status =
			xsdk_texture_create(&state.textures[i], XSDK_FORMAT_R8G8B8A8, TEXTURE_SIZE, TEXTURE_SIZE, 1, true);
		if (!NT_SUCCESS(status)) {
			xsdk_log_write("vkbench: xsdk_texture_create failed: 0x%08X", (unsigned)status);
			return false;
		}
		assets_build_texture(pixels, i);
		xsdk_texture_upload(&state.textures[i], 0, pixels, TEXTURE_SIZE * 4);
		xsdk_gpu_invalidate(state.textures[i].address, state.textures[i].size_bytes);
	}
	return true;
}

// The Vulkan build's sampler, blending, depth and culling, in xsdk's terms.
static void make_state(void)
{
	const XsdkSampler sampler = { .mag_filter = XSDK_FILTER_LINEAR,
		.min_filter = XSDK_FILTER_LINEAR,
		.mip_filter = XSDK_FILTER_POINT,
		.address_u = XSDK_ADDRESS_WRAP,
		.address_v = XSDK_ADDRESS_WRAP };
	xsdk_texture_sampler_state(&state.sampler, &sampler);
	const XsdkBlend opaque = { XSDK_BLEND_ONE, XSDK_BLEND_ZERO, XSDK_BLEND_OP_ADD, XSDK_BLEND_ONE, XSDK_BLEND_ZERO,
		XSDK_BLEND_OP_ADD, XSDK_WRITE_ALL };
	const XsdkBlend alpha = { XSDK_BLEND_SRC_ALPHA, XSDK_BLEND_ONE_MINUS_SRC_ALPHA, XSDK_BLEND_OP_ADD, XSDK_BLEND_ONE,
		XSDK_BLEND_ONE_MINUS_SRC_ALPHA, XSDK_BLEND_OP_ADD, XSDK_WRITE_ALL };
	xsdk_gpu_blend_state(&state.opaque, &opaque);
	xsdk_gpu_blend_state(&state.alpha, &alpha);
	const XsdkDepthStencil depth_test = { .depth_test = true, .depth_write = true, .depth_compare = XSDK_COMPARE_LESS };
	const XsdkDepthStencil no_depth = { .depth_compare = XSDK_COMPARE_ALWAYS };
	xsdk_gpu_depth_stencil_state(&state.depth_test, &depth_test);
	xsdk_gpu_depth_stencil_state(&state.no_depth, &no_depth);
	const XsdkRaster raster = { .cull = XSDK_CULL_NONE };
	xsdk_gpu_raster_state(&state.raster, &raster);
}

// Clears eDRAM for the first frame; each frame's resolve clears it for the next.
static bool clear_first(void)
{
	xsdk_gpu_set_target(SCREEN_WIDTH, SCREEN_HEIGHT);
	xsdk_gpu_resolve(state.front_address, SCREEN_WIDTH, SCREEN_HEIGHT, CLEAR_ARGB);
	return NT_SUCCESS(xsdk_gpu_wait(xsdk_gpu_fence(), TIMEOUT_MS));
}

static bool xsdk_init(void)
{
	state = (State){ .opened = false };
	NTSTATUS status = xsdk_gpu_open();
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("vkbench: xsdk_gpu_open failed: 0x%08X", (unsigned)status);
		return false;
	}
	state.opened = true;
	// The boot logo, as vkCreateDevice shows it.
	status = xsdk_splash_begin();
	if (!NT_SUCCESS(status))
		xsdk_log_write("vkbench: no boot logo: 0x%08X", (unsigned)status);
	uint32_t width = 0;
	uint32_t height = 0;
	status = xsdk_present_open(XSDK_DISPLAY_720P, BUFFERS, SWAP_INTERVAL, &width, &height);
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("vkbench: no display (0x%08X), so frames go into an image and show nowhere", (unsigned)status);
		state.offscreen = xsdk_gpu_alloc(xsdk_gpu_resolve_bytes(SCREEN_WIDTH, SCREEN_HEIGHT));
		if (!state.offscreen)
			return false;
	}
	if (!load_shaders() || !make_buffers() || !make_textures())
		return false;
	make_state();
	xsdk_gpu_set_depth_clear(1.0f, 0);
	if (headless()) {
		state.front_address = xsdk_gpu_address(state.offscreen);
		return clear_first();
	}
	status = xsdk_present_begin(&state.front_address, TIMEOUT_MS);
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("vkbench: xsdk_present_begin failed: 0x%08X", (unsigned)status);
		return false;
	}
	return clear_first();
}

// The viewport as vulkan4xenon sets Vulkan's over the whole screen (src/queue.c, apply_viewport).
static void xsdk_begin_frame(void)
{
	xsdk_gpu_set_target(SCREEN_WIDTH, SCREEN_HEIGHT);
	xsdk_gpu_set_viewport(-0.5f, SCREEN_HEIGHT - 0.5f, SCREEN_WIDTH, -(float)SCREEN_HEIGHT, 0.0f, 1.0f);
	xsdk_gpu_set_scissor(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
	xsdk_gpu_set_raster(&state.raster);
	state.program = state.texture = state.blend = state.depth = state.vertices = -1;
}

static void write_literals(const V4xShader *shader, bool vertex)
{
	for (uint32_t i = 0; i < shader->literal_count; i++) {
		const V4xShaderLiteral *const literal = &shader->literals[i];
		if (vertex)
			xsdk_gpu_set_vertex_constants(literal->reg, literal->values, 1);
		else
			xsdk_gpu_set_pixel_constants(literal->reg, literal->values, 1);
	}
}

static void use_program(ProgramKind program)
{
	if (state.program == (int)program)
		return;
	const ShaderKind vertex = program_shaders[program][0];
	const ShaderKind pixel = program_shaders[program][1];
	xsdk_gpu_set_shaders(&state.shaders[vertex], &state.shaders[pixel]);
	write_literals(state.sources[vertex], true);
	write_literals(state.sources[pixel], false);
	state.program = (int)program;
}

static void use_texture(int texture)
{
	if (state.texture == texture)
		return;
	xsdk_gpu_set_texture(0, &state.textures[texture], &state.sampler);
	state.texture = texture;
}

static void use_blend(bool alpha)
{
	if (state.blend == (int)alpha)
		return;
	xsdk_gpu_set_blend(alpha ? &state.alpha : &state.opaque);
	state.blend = (int)alpha;
}

static void use_depth(bool tested)
{
	if (state.depth == (int)tested)
		return;
	xsdk_gpu_set_depth_stencil(tested ? &state.depth_test : &state.no_depth);
	state.depth = (int)tested;
}

static void use_vertices(volatile uint32_t *vertices, uint32_t size_bytes)
{
	const uint32_t address = xsdk_gpu_address(vertices);
	if (state.vertices == (int)address)
		return;
	xsdk_gpu_set_vertices(0, address, size_bytes);
	state.vertices = (int)address;
}

// The tint sits 64 bytes into the pixel shader's push constants, or alone in its uniform block.
static void xsdk_draw_meshes(const DrawItem *items, int count, bool uniform_buffers)
{
	use_depth(true);
	use_vertices(state.cube_vertices, CUBE_VERTEX_COUNT * MESH_STRIDE_BYTES);
	const uint32_t cube_indices = xsdk_gpu_address(state.cube_indices);
	for (int i = 0; i < count; i++) {
		const DrawItem *const item = &items[i];
		const ProgramKind program = uniform_buffers ? PROGRAM_UNIFORM
			: item->grayscale                       ? PROGRAM_MESH_GRAY
													: PROGRAM_MESH;
		use_program(program);
		use_texture(item->texture);
		use_blend(item->blend);
		const V4xShader *const vertex = state.sources[program_shaders[program][0]];
		const V4xShader *const pixel = state.sources[program_shaders[program][1]];
		const uint32_t tint_register = block_register(pixel) + (uniform_buffers ? 0 : MVP_FLOATS / 4);
		xsdk_gpu_set_vertex_constants(block_register(vertex), item->mvp, MVP_FLOATS / 4);
		xsdk_gpu_set_pixel_constants(tint_register, item->tint, 1);
		xsdk_gpu_draw_indexed(XSDK_PRIMITIVE_TRIANGLE_LIST, cube_indices, CUBE_INDEX_COUNT, false);
	}
}

// draws_4000's state, each item copied whole as its instance's element, then one indexed instanced draw.
static void xsdk_draw_instances(const DrawItem *items, int count)
{
	const uint32_t size_bytes = (uint32_t)count * sizeof(DrawItem);
	// The GPU is done with the instances: each frame waits for its fence.
	memcpy((void *)state.instances, items, size_bytes);
	xsdk_gpu_invalidate(xsdk_gpu_address(state.instances), size_bytes);
	use_program(PROGRAM_INSTANCED);
	use_texture(0);
	use_blend(false);
	use_depth(true);
	use_vertices(state.cube_vertices, CUBE_VERTEX_COUNT * MESH_STRIDE_BYTES);
	xsdk_gpu_set_vertices(1, xsdk_gpu_address(state.instances), size_bytes);
	xsdk_gpu_draw_indexed_instanced(XSDK_PRIMITIVE_TRIANGLE_LIST, &state.cube_copies, 0, (uint32_t)count);
}

static void xsdk_draw_sprites(const SpriteVertex *vertices, int sprite_count)
{
	// The GPU is done with the stream: each frame waits for its fence.
	memcpy((void *)state.stream, vertices, (size_t)sprite_count * 4 * sizeof(SpriteVertex));
	xsdk_gpu_invalidate(xsdk_gpu_address(state.stream), (uint32_t)sprite_count * 4 * SPRITE_STRIDE_BYTES);
	use_program(PROGRAM_SPRITE);
	use_texture(0);
	use_blend(true);
	use_depth(false);
	state.vertices = -1;
	use_vertices(state.stream, (uint32_t)sprite_count * 4 * SPRITE_STRIDE_BYTES);
	xsdk_gpu_draw_indexed(XSDK_PRIMITIVE_TRIANGLE_LIST, xsdk_gpu_address(state.quad_indices),
		(uint32_t)sprite_count * QUAD_INDICES, false);
}

static void xsdk_draw_fullscreen(int layers)
{
	use_program(PROGRAM_SPRITE);
	use_texture(1);
	use_blend(true);
	use_depth(false);
	use_vertices(state.fullscreen, FULLSCREEN_VERTICES * SPRITE_STRIDE_BYTES);
	for (int i = 0; i < layers; i++)
		xsdk_gpu_draw_indexed(XSDK_PRIMITIVE_TRIANGLE_LIST, xsdk_gpu_address(state.quad_indices), QUAD_INDICES, false);
}

// The resolve clears colour and depth for the next frame, as vulkan4xenon's last pass in a command buffer does.
static void xsdk_submit_frame(void)
{
	xsdk_gpu_resolve(state.front_address, SCREEN_WIDTH, SCREEN_HEIGHT, CLEAR_ARGB);
	state.fence = xsdk_gpu_fence();
}

static bool xsdk_wait_frame(void)
{
	const uint64_t start = bench_now_ns();
	while (!xsdk_gpu_fence_done(state.fence)) {
		if (bench_now_ns() - start > (uint64_t)FRAME_TIMEOUT_MS * 1000000)
			return false;
	}
	return true;
}

static void xsdk_present_frame(void)
{
	state.presented_address = state.front_address;
	if (headless())
		return;
	xsdk_present_end();
	const NTSTATUS status = xsdk_present_begin(&state.front_address, TIMEOUT_MS);
	if (!NT_SUCCESS(status))
		xsdk_log_write("vkbench: xsdk_present_begin failed: 0x%08X", (unsigned)status);
}

static volatile const uint32_t *xsdk_presented_pixels(void)
{
	return headless() ? state.offscreen : xsdk_present_pixels(state.presented_address);
}

static uint32_t xsdk_finish(void)
{
	return NT_SUCCESS(xsdk_gpu_wait(xsdk_gpu_fence(), TIMEOUT_MS)) ? 0 : 1;
}

static void xsdk_shutdown(void)
{
	if (!state.opened)
		return;
	(void)xsdk_finish();
	xsdk_present_close();
	xsdk_splash_end();
	xsdk_gpu_free(state.offscreen);
	for (int i = 0; i < TEXTURE_COUNT; i++)
		xsdk_texture_free(&state.textures[i]);
	xsdk_gpu_free(state.cube_copies_memory);
	xsdk_gpu_free(state.instances);
	xsdk_gpu_free(state.stream);
	xsdk_gpu_free(state.fullscreen);
	xsdk_gpu_free(state.quad_indices);
	xsdk_gpu_free(state.cube_indices);
	xsdk_gpu_free(state.cube_vertices);
	for (int i = 0; i < SHADERS; i++)
		xsdk_shader_free(&state.shaders[i]);
	const NTSTATUS status = xsdk_gpu_close();
	if (!NT_SUCCESS(status))
		xsdk_log_write("vkbench: xsdk_gpu_close failed: 0x%08X", (unsigned)status);
	state.opened = false;
}

const BenchBackend bench_xsdk_backend = {
	.name = "xsdk",
	.init = xsdk_init,
	.begin_frame = xsdk_begin_frame,
	.draw_meshes = xsdk_draw_meshes,
	.draw_instances = xsdk_draw_instances,
	.draw_sprites = xsdk_draw_sprites,
	.draw_fullscreen = xsdk_draw_fullscreen,
	.submit_frame = xsdk_submit_frame,
	.wait_frame = xsdk_wait_frame,
	.present_frame = xsdk_present_frame,
	.presented_pixels = xsdk_presented_pixels,
	.finish = xsdk_finish,
	.shutdown = xsdk_shutdown,
};
