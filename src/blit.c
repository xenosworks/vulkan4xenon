#include "blit.h"

#include <string.h>

#include <vulkan/vulkan.h>

#include "blit_shaders.h"
#include "objects.h"
#include "shader.h"
#include "xsdk/gpu.h"
#include "xsdk/log.h"
#include "xsdk/shader.h"
#include "xsdk/texture.h"

enum {
	RECTANGLE_VERTICES = 6,
	// A position, then texture coordinates.
	VERTEX_FLOATS = 6,
	VERTEX_BYTES = VERTEX_FLOATS * 4,
	RECTANGLE_BYTES = RECTANGLE_VERTICES * VERTEX_BYTES,
	UV_OFFSET_BYTES = 16,
};

// Two triangles over clip space, Vulkan's y down, so texture coordinates (0, 0) land on the top left pixel.
static const float rectangle[RECTANGLE_VERTICES][VERTEX_FLOATS] = {
	{ -1.0f, -1.0f, 0.0f, 1.0f, 0.0f, 0.0f },
	{ 1.0f, -1.0f, 0.0f, 1.0f, 1.0f, 0.0f },
	{ -1.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f },
	{ 1.0f, -1.0f, 0.0f, 1.0f, 1.0f, 0.0f },
	{ 1.0f, 1.0f, 0.0f, 1.0f, 1.0f, 1.0f },
	{ -1.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f },
};

// The layer's shaders, as the bundle has them: the image drawn back, into one target or four and as depth, the clears,
// and the drawn blit's vertex shader.
typedef struct {
	const V4xShader *vertex;
	const V4xShader *pixel;
	const V4xShader *pixel_four;
	const V4xShader *depth;
	const V4xShader *clear;
	const V4xShader *clear_four;
	const V4xShader *region;
} Shaders;

static NTSTATUS load_shader(XsdkShader *shader, const V4xShader *source)
{
	return xsdk_shader_load(shader, source->container, source->container_size_bytes);
}

static VkResult load_shaders(Blit *blit, const Shaders *shaders)
{
	NTSTATUS status = load_shader(&blit->vertex, shaders->vertex);
	if (NT_SUCCESS(status))
		status = load_shader(&blit->pixel, shaders->pixel);
	if (NT_SUCCESS(status))
		status = load_shader(&blit->pixel_four, shaders->pixel_four);
	if (NT_SUCCESS(status))
		status = load_shader(&blit->depth, shaders->depth);
	if (NT_SUCCESS(status))
		status = load_shader(&blit->clear, shaders->clear);
	if (NT_SUCCESS(status))
		status = load_shader(&blit->clear_four, shaders->clear_four);
	if (NT_SUCCESS(status))
		status = load_shader(&blit->region, shaders->region);
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("vulkan4xenon: xsdk_shader_load failed for the blit: 0x%08X", (unsigned)status);
		return VK_ERROR_OUT_OF_DEVICE_MEMORY;
	}
	// Locations 0 and 1 are the position and the colour usage to xost (xsdk:src/xsc_resolve.c:344-346).
	const XsdkVertexElement elements[] = {
		{ .usage = XSDK_USAGE_POSITION, .format = XSDK_VERTEX_FLOAT4, .stream = 0, .offset_bytes = 0 },
		{ .usage = XSDK_USAGE_COLOR, .format = XSDK_VERTEX_FLOAT2, .stream = 0, .offset_bytes = UV_OFFSET_BYTES },
	};
	const uint32_t strides[XSDK_VERTEX_STREAMS] = { VERTEX_BYTES };
	const uint32_t count = sizeof elements / sizeof elements[0];
	status = xsdk_shader_set_vertex_layout(&blit->vertex, elements, count, strides);
	if (NT_SUCCESS(status))
		status = xsdk_shader_set_vertex_layout(&blit->region, elements, count, strides);
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("vulkan4xenon: xsdk_shader_set_vertex_layout failed for the blit: 0x%08X", (unsigned)status);
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	// Each samples its one image at the slot xost gives a fragment shader's first sampler.
	blit->texture_slot = shaders->pixel->resources[0].first;
	blit->clear_constant = shaders->clear->resources[0].first;
	blit->clear_four_constant = shaders->clear_four->resources[0].first;
	blit->region_constant = shaders->region->resources[0].first;
	return VK_SUCCESS;
}

static VkResult write_rectangle(Blit *blit)
{
	blit->vertices = xsdk_gpu_alloc(RECTANGLE_BYTES);
	if (!blit->vertices) {
		xsdk_log_write("vulkan4xenon: no GPU memory for the blit's rectangle");
		return VK_ERROR_OUT_OF_DEVICE_MEMORY;
	}
	const float *const floats = &rectangle[0][0];
	for (uint32_t i = 0; i < RECTANGLE_BYTES / 4; i++) {
		uint32_t bits;
		memcpy(&bits, &floats[i], sizeof bits);
		blit->vertices[i] = bits;
	}
	blit->vertices_address = xsdk_gpu_address(blit->vertices);
	return VK_SUCCESS;
}

// Point or linear sampling of level 0 alone, edges clamped, as a blit samples; no blending, culling, depth or stencil.
static void make_state(Blit *blit)
{
	XsdkSampler sampler = { .mag_filter = XSDK_FILTER_POINT,
		.min_filter = XSDK_FILTER_POINT,
		.mip_filter = XSDK_FILTER_POINT,
		.address_u = XSDK_ADDRESS_CLAMP,
		.address_v = XSDK_ADDRESS_CLAMP,
		.level_count = 1 };
	xsdk_texture_sampler_state(&blit->sampler, &sampler);
	sampler.mag_filter = XSDK_FILTER_LINEAR;
	sampler.min_filter = XSDK_FILTER_LINEAR;
	xsdk_texture_sampler_state(&blit->linear_sampler, &sampler);
	const XsdkBlend blend = { .color_source = XSDK_BLEND_ONE,
		.color_dest = XSDK_BLEND_ZERO,
		.color_op = XSDK_BLEND_OP_ADD,
		.alpha_source = XSDK_BLEND_ONE,
		.alpha_dest = XSDK_BLEND_ZERO,
		.alpha_op = XSDK_BLEND_OP_ADD,
		.write_mask = XSDK_WRITE_ALL };
	xsdk_gpu_blend_state(&blit->blend, &blend);
	const XsdkRaster raster = { .cull = XSDK_CULL_NONE };
	xsdk_gpu_raster_state(&blit->raster, &raster);
	const XsdkDepthStencil depth_stencil = { .depth_compare = XSDK_COMPARE_ALWAYS };
	xsdk_gpu_depth_stencil_state(&blit->depth_stencil, &depth_stencil);
	const XsdkDepthStencil depth_write = {
		.depth_test = true, .depth_write = true, .depth_compare = XSDK_COMPARE_ALWAYS
	};
	xsdk_gpu_depth_stencil_state(&blit->depth_write, &depth_write);
}

VkResult v4x_blit_create(Blit *blit)
{
	*blit = (Blit){ .vertices = NULL };
	const Shaders shaders = {
		.vertex = v4x_shader_find(&v4x_blit_bundle, v4x_blit_vert_spirv, V4X_BLIT_VERT_SPIRV_BYTES),
		.pixel = v4x_shader_find(&v4x_blit_bundle, v4x_blit_frag_spirv, V4X_BLIT_FRAG_SPIRV_BYTES),
		.pixel_four = v4x_shader_find(&v4x_blit_bundle, v4x_blit_four_frag_spirv, V4X_BLIT_FOUR_FRAG_SPIRV_BYTES),
		.depth = v4x_shader_find(&v4x_blit_bundle, v4x_depth_back_frag_spirv, V4X_DEPTH_BACK_FRAG_SPIRV_BYTES),
		.clear = v4x_shader_find(&v4x_blit_bundle, v4x_clear_frag_spirv, V4X_CLEAR_FRAG_SPIRV_BYTES),
		.clear_four = v4x_shader_find(&v4x_blit_bundle, v4x_clear_four_frag_spirv, V4X_CLEAR_FOUR_FRAG_SPIRV_BYTES),
		.region = v4x_shader_find(&v4x_blit_bundle, v4x_blit_region_vert_spirv, V4X_BLIT_REGION_VERT_SPIRV_BYTES),
	};
	if (!shaders.vertex || !shaders.pixel || !shaders.pixel_four || !shaders.depth || !shaders.clear ||
		!shaders.clear_four || !shaders.region)
		return VK_ERROR_INITIALIZATION_FAILED;
	VkResult result = load_shaders(blit, &shaders);
	if (result == VK_SUCCESS)
		result = write_rectangle(blit);
	if (result != VK_SUCCESS) {
		v4x_blit_destroy(blit);
		return result;
	}
	make_state(blit);
	return VK_SUCCESS;
}

void v4x_blit_destroy(Blit *blit)
{
	xsdk_gpu_free(blit->vertices);
	xsdk_shader_free(&blit->region);
	xsdk_shader_free(&blit->clear_four);
	xsdk_shader_free(&blit->clear);
	xsdk_shader_free(&blit->depth);
	xsdk_shader_free(&blit->pixel_four);
	xsdk_shader_free(&blit->pixel);
	xsdk_shader_free(&blit->vertex);
	*blit = (Blit){ .vertices = NULL };
}

// Each of count colour targets blends nothing, and only target `target` is written, or none for target count.
static void masked_blends(const Blit *blit, uint32_t target, uint32_t count, XsdkBlendState *blends)
{
	for (uint32_t i = 0; i < count; i++) {
		blends[i] = blit->blend;
		if (i != target)
			blends[i].color_mask = 0;
	}
}

// The texture over the area, texel for texel: the viewport as the replay sets one over the whole target (src/queue.c,
// apply_viewport), the scissor at the area.
static void draw_texture(const Blit *blit, const XsdkTexture *texture, uint32_t width, uint32_t height,
	const VkRect2D *area, const XsdkBlendState *blends, uint32_t count)
{
	xsdk_gpu_set_viewport(-0.5f, (float)height - 0.5f, (float)width, -(float)height, 0.0f, 1.0f);
	xsdk_gpu_set_scissor((uint32_t)area->offset.x, (uint32_t)area->offset.y, area->extent.width, area->extent.height);
	xsdk_gpu_set_blends(blends, count);
	xsdk_gpu_set_raster(&blit->raster);
	xsdk_gpu_set_vertices(0, blit->vertices_address, RECTANGLE_BYTES);
	xsdk_gpu_set_texture(blit->texture_slot, texture, &blit->sampler);
	xsdk_gpu_draw(XSDK_PRIMITIVE_TRIANGLE_LIST, RECTANGLE_VERTICES);
}

void v4x_blit_draw(const Blit *blit, const XsdkTexture *texture, uint32_t width, uint32_t height, const VkRect2D *area,
	uint32_t target, uint32_t count)
{
	XsdkBlendState blends[V4X_MAX_COLOR_ATTACHMENTS];
	masked_blends(blit, target, count, blends);
	xsdk_gpu_set_shaders(&blit->vertex, count > 1 ? &blit->pixel_four : &blit->pixel);
	xsdk_gpu_set_depth_stencil(&blit->depth_stencil);
	draw_texture(blit, texture, width, height, area, blends, count);
}

// Xenos exports no stencil, so depth alone comes back, through the pixel shader (xost:docs/xsc.md, gl_FragDepth).
void v4x_blit_draw_depth(const Blit *blit, const XsdkTexture *texture, uint32_t width, uint32_t height, uint32_t count)
{
	XsdkBlendState blends[V4X_MAX_COLOR_ATTACHMENTS];
	masked_blends(blit, count, count, blends);
	const VkRect2D whole = { { 0, 0 }, { width, height } };
	xsdk_gpu_set_shaders(&blit->vertex, &blit->depth);
	xsdk_gpu_set_depth_stencil(&blit->depth_write);
	draw_texture(blit, texture, width, height, &whole, blends, count);
}

// The viewport over the destination's rectangle, as the replay sets one (src/queue.c, apply_viewport), and the scissor
// at it, as Xenos draws past a viewport (docs/SUPPORT.md, Pipelines).
void v4x_blit_region(const Blit *blit, const DrawnBlit *region, const XsdkTexture *texture)
{
	const VkRect2D *const rect = &region->rect;
	const float x = (float)rect->offset.x;
	const float y = (float)rect->offset.y;
	xsdk_gpu_set_shaders(&blit->region, &blit->pixel);
	xsdk_gpu_set_viewport(x - 0.5f, y + (float)rect->extent.height - 0.5f, (float)rect->extent.width,
		-(float)rect->extent.height, 0.0f, 1.0f);
	xsdk_gpu_set_scissor((uint32_t)rect->offset.x, (uint32_t)rect->offset.y, rect->extent.width, rect->extent.height);
	xsdk_gpu_set_blend(&blit->blend);
	xsdk_gpu_set_raster(&blit->raster);
	xsdk_gpu_set_depth_stencil(&blit->depth_stencil);
	xsdk_gpu_set_vertex_constants(blit->region_constant, region->uv, 1);
	xsdk_gpu_set_vertices(0, blit->vertices_address, RECTANGLE_BYTES);
	const XsdkSamplerState *const sampler = region->linear ? &blit->linear_sampler : &blit->sampler;
	xsdk_gpu_set_texture(blit->texture_slot, texture, sampler);
	xsdk_gpu_draw(XSDK_PRIMITIVE_TRIANGLE_LIST, RECTANGLE_VERTICES);
}

// Each colour target written only if it's cleared.
static void clear_blends(const ClearRect *clear, uint32_t color_count, XsdkBlendState *blends)
{
	for (uint32_t i = 0; i < color_count; i++) {
		const XsdkBlend blend = { .color_source = XSDK_BLEND_ONE,
			.color_dest = XSDK_BLEND_ZERO,
			.color_op = XSDK_BLEND_OP_ADD,
			.alpha_source = XSDK_BLEND_ONE,
			.alpha_dest = XSDK_BLEND_ZERO,
			.alpha_op = XSDK_BLEND_OP_ADD,
			.write_mask = clear->colors_cleared >> i & 1 ? XSDK_WRITE_ALL : 0 };
		xsdk_gpu_blend_state(&blends[i], &blend);
	}
}

// The viewport's depth range is the clear's depth alone, so every pixel takes it; depth always passes, stencil always
// passes and takes the reference, and each writes only if it's cleared. Several colour targets take the shader that
// writes four colours.
void v4x_blit_clear(const Blit *blit, const ClearRect *clear, uint32_t color_count, uint32_t width, uint32_t height)
{
	XsdkBlendState blends[V4X_MAX_COLOR_ATTACHMENTS];
	clear_blends(clear, color_count, blends);
	const bool four = color_count > 1;
	const XsdkDepthStencil depth_stencil = { .depth_test = clear->clears_depth,
		.depth_write = clear->clears_depth,
		.depth_compare = XSDK_COMPARE_ALWAYS,
		.stencil_test = clear->clears_stencil,
		.stencil_compare = XSDK_COMPARE_ALWAYS,
		.stencil_fail = XSDK_STENCIL_REPLACE,
		.stencil_depth_fail = XSDK_STENCIL_REPLACE,
		.stencil_pass = XSDK_STENCIL_REPLACE,
		.stencil_reference = (uint8_t)clear->stencil,
		.stencil_read_mask = 0xFF,
		.stencil_write_mask = 0xFF };
	XsdkDepthStencilState depth_stencil_state;
	xsdk_gpu_depth_stencil_state(&depth_stencil_state, &depth_stencil);
	xsdk_gpu_set_shaders(&blit->vertex, four ? &blit->clear_four : &blit->clear);
	xsdk_gpu_set_viewport(-0.5f, (float)height - 0.5f, (float)width, -(float)height, clear->depth, clear->depth);
	xsdk_gpu_set_scissor((uint32_t)clear->rect.offset.x, (uint32_t)clear->rect.offset.y, clear->rect.extent.width,
		clear->rect.extent.height);
	xsdk_gpu_set_blends(blends, color_count);
	xsdk_gpu_set_raster(&blit->raster);
	xsdk_gpu_set_depth_stencil(&depth_stencil_state);
	if (four)
		xsdk_gpu_set_pixel_constants(blit->clear_four_constant, &clear->colors[0][0], V4X_MAX_COLOR_ATTACHMENTS);
	else
		xsdk_gpu_set_pixel_constants(blit->clear_constant, clear->colors[0], 1);
	xsdk_gpu_set_vertices(0, blit->vertices_address, RECTANGLE_BYTES);
	xsdk_gpu_draw(XSDK_PRIMITIVE_TRIANGLE_LIST, RECTANGLE_VERTICES);
}
