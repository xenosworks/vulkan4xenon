#ifndef VULKAN4XENON_H
#define VULKAN4XENON_H

#include <stdint.h>

#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

// A shader compiled offline by tools/build_shaders.py: xost's container for one SPIR-V module.

// A float constant xost pooled for a literal, which the layer writes when a pipeline using the shader is bound.
typedef struct {
	uint32_t reg;
	float values[4];
} V4xShaderLiteral;

enum {
	V4X_RESOURCE_PUSH_CONSTANTS,
	V4X_RESOURCE_UNIFORM_BUFFER,
	V4X_RESOURCE_SAMPLER,
	// A hardware loop's count, the int at byte first of the push constants or of the uniform buffer, for loop constant
	// count, its i from start by step (xost:docs/xsc.md, "Loops a block counts").
	V4X_RESOURCE_PUSH_LOOP,
	V4X_RESOURCE_UNIFORM_LOOP,
	// A sampler2DShadow's comparison: the float constant its sampler's compare op is written to.
	V4X_RESOURCE_COMPARISON,
	// A vertex shader's gl_InstanceIndex: the float constant of its instance group, which the layer writes each
	// instance's index into, in w (xost:docs/xsc.md, "Interface conventions").
	V4X_RESOURCE_INSTANCE_INDEX,
	// Loops whose count is data, the third loop kind, last so the kinds before keep their numbers: loop constant count,
	// which runs first passes, 255 (xsdk:include/xsdk/xsc.h, XSDK_XSC_FIXED_LOOP).
	V4X_RESOURCE_FIXED_LOOP,
};

// Where xost put a block, sampler, comparison or gl_InstanceIndex the shader reads (xost:docs/xsc.md, "Interface
// conventions"), or where a loop's count is.
typedef struct {
	uint8_t kind;
	// The descriptor set and binding; 0 for push constants, the instance index and a fixed loop.
	uint8_t set;
	uint8_t binding;
	// The first float constant register, holding the block's first 16 bytes or the index, the fetch constant, a loop's
	// count's byte in its block, or a fixed loop's passes.
	uint16_t first;
	// Registers the block fills; 1 for a sampler and the index; a loop's loop constant.
	uint16_t count;
	// A block loop's i: where it starts and what each pass adds, 1 or more, which the layer works its passes out from
	// (xsdk:include/xsdk/xsc.h, xsdk_xsc_loop_passes); 0 for the other kinds.
	int32_t start;
	int32_t step;
} V4xShaderResource;

// A user clip plane a vertex shader's gl_ClipDistance[plane] reads from its float constant register reg, in a block
// it reads, which the layer sets at each draw from the values bound (xost:docs/xsc.md, "Interface conventions").
typedef struct {
	uint16_t reg;
	uint8_t plane;
} V4xShaderClipPlane;

typedef struct {
	// FNV-1a over the SPIR-V's words, each taken as its four bytes low first, and the SPIR-V's size.
	uint64_t spirv_hash;
	uint32_t spirv_size_bytes;
	const uint32_t *container;
	uint32_t container_size_bytes;
	const V4xShaderLiteral *literals;
	uint32_t literal_count;
	const V4xShaderResource *resources;
	uint32_t resource_count;
	const V4xShaderClipPlane *clip_planes;
	uint32_t clip_plane_count;
} V4xShader;

typedef struct {
	const V4xShader *shaders;
	uint32_t count;
} V4xShaderBundle;

// The app's bundle, which tools/build_shaders.py writes; vkCreateShaderModule looks SPIR-V up in it, and finds
// nothing when the app links none.
extern const V4xShaderBundle v4x_shader_bundle;

// Keeps the shaders the layer compiles in this folder and reads them back from it; call before making any modules.
void v4x_set_shader_cache(const char *directory);

// Shaders the layer has compiled, and read back from its cache, since the title started.
typedef struct {
	uint32_t compiled;
	uint32_t read;
} V4xShaderCounts;

V4xShaderCounts v4x_shader_counts(void);

#ifdef __cplusplus
}
#endif

#endif
