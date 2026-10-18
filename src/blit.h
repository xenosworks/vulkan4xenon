#ifndef V4X_BLIT_H
#define V4X_BLIT_H

#include <stdint.h>

#include <vulkan/vulkan.h>

#include "objects.h"
#include "xsdk/texture.h"

// Drawing an image back into eDRAM, for a render pass that loads it (docs/ROADMAP.md, milestone 4), clearing
// rectangles of a pass's attachments (milestone 19), and drawing blits' regions (milestone 27).

// Loads the layer's shaders and the rectangle they draw; needs the GPU open.
VkResult v4x_blit_create(Blit *blit);
void v4x_blit_destroy(Blit *blit);
// Draws the texture over the area of colour target `target` of count, texel for texel, the others kept by their write
// masks, as a tiled pass can't set a target alone; leaves shaders, state and fetch constant 0 changed.
void v4x_blit_draw(const Blit *blit, const XsdkTexture *texture, uint32_t width, uint32_t height, const VkRect2D *area,
	uint32_t target, uint32_t count);
// Draws a D24S8 texture's depth over the whole target, texel for texel, its count colour targets kept and stencil left
// as it was; leaves the same changed.
void v4x_blit_draw_depth(const Blit *blit, const XsdkTexture *texture, uint32_t width, uint32_t height, uint32_t count);
// Draws the region's source over its rectangle, nearest or linear; leaves shaders, state, vertex stream 0, fetch
// constant 0 and the region's float constant changed.
void v4x_blit_region(const Blit *blit, const DrawnBlit *region);
// Clears the rectangle's colours, depth or stencil, as a draw over the pass's color_count targets that the scissor cuts
// to it; leaves shaders, state, vertex stream 0 and the clear's float constants changed.
void v4x_blit_clear(const Blit *blit, const ClearRect *clear, uint32_t color_count, uint32_t width, uint32_t height);

#endif
