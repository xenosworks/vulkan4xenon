#ifndef V4X_TESTS_H
#define V4X_TESTS_H

#include <stdint.h>

#include "harness.h"

// The logo shows from vkCreateDevice until the first present; called with the device made and no swapchain yet.
void test_boot_logo(Harness *harness, uint32_t shown_before_device);
// The proc-address table, memory types, buffer requirements and refusals.
void test_objects(const Harness *harness);
// Milestone 19's commands that answer without drawing, and those refused, step 1.
void test_commands(const Harness *harness);
// Copies, clears and blits on the CPU, indirect draws and linear images, each read back texel by texel (milestone 19).
void test_copies(const Harness *harness);
// Stencil masks and references, depth bias and blend constants set by commands (milestone 19).
void test_dynamic_state(const Harness *harness);
// Front and back faces' own stencil, from the pipeline and from commands for one face (milestone 26).
void test_two_sided_stencil(const Harness *harness);
// Quads cut by clip distances from pushed planes and a uniform buffer's, and nothing cut after them (milestone 26).
void test_clip_distances(const Harness *harness);
// Point sprites of gl_PointSize coloured by gl_PointCoord, and a point without a size, 1 pixel (milestone 26).
void test_point_sprites(const Harness *harness);
// Quads drawn as their triangles' edges and corners, and a back face culled before its edges (milestone 26).
void test_fill_modes(const Harness *harness);
// A depth image cleared outside a pass, and rectangles of colour, depth and stencil cleared inside one (milestone 19).
void test_clears(const Harness *harness);
// A render pass of three subpasses, the second drawn from secondary command buffers (milestone 19).
void test_subpasses(const Harness *harness);
// Colour attachments in the float and 10-bit formats, four at once, each blended on its own (milestone 25).
void test_targets(const Harness *harness);
// Render passes whose area is smaller than the framebuffer, each clearing, loading and storing it alone (milestone 27).
void test_render_areas(const Harness *harness);
// Render passes past eDRAM drawn a band of rows at a time: four attachments with depth at 1280x720 loaded and stored, a
// 2048x2048 shadow map stored, loaded and sampled, and what such a pass refuses (milestone 17).
void test_tiles(const Harness *harness);
// Blits drawn on the GPU into rectangles of an image, and the replay's time with one and with a blit on the CPU
// (milestone 27).
void test_gpu_blits(const Harness *harness);
// Occlusion queries around quads, nothing, hidden quads and clears, reused, many at once and copied (milestone 28).
void test_occlusion(const Harness *harness);
// Events the GPU sets, resets and waits on, and time stamps of its clock (milestone 19).
void test_events(const Harness *harness);
// A triangle through Vulkan, checked on the image the display shows.
void test_triangle(const Harness *harness);
// The triangle again, its shaders read back from the layer's cache by a second device (milestone 13).
void test_triangle_from_cache(const Harness *harness);
// The triangle with 8-bit colours packed in a word, as Aurora decodes vertex colours (milestone 10).
void test_packed_colour(const Harness *harness);
// Colours and positions from vertex attributes of half floats, bytes and shorts, normalized or scaled (milestone 20).
void test_vertex_formats(const Harness *harness);
// Images copied in from a buffer and sampled through descriptor sets (milestone 2).
void test_texture(const Harness *harness);
// Every image format on xsdk's sampled, filtered, copied, blitted and cleared, each texel checked (milestone 20).
void test_formats(const Harness *harness);
// A cube and a 3D image copied in face by face and slice by slice, sampled, copied and cleared (milestone 23).
void test_cube_3d(const Harness *harness);
// Every address mode past 0 and 1, borders in white and in black blended over the clear colour (milestone 23).
void test_addressing(const Harness *harness);
// A D24S8 image compared against a reference by comparison samplers, and filtered linearly (milestone 23).
void test_shadow(const Harness *harness);
// Quads placed and coloured by push constants (milestone 2).
void test_push_constants(const Harness *harness);
// Quads placed by a matrix and coloured from uniform buffers, one dynamic, in two sets (milestone 2).
void test_uniform_buffers(const Harness *harness);
// Index buffers and every topology (milestone 3).
void test_indexed(const Harness *harness);
// Instanced draws, indexed and not, direct and indirect, each instance placed and coloured on its own (milestone 24).
void test_instancing(const Harness *harness);
// Strips past 65535 vertices, drawn in pieces sharing the vertices where they meet; long fans refused (milestone 24).
void test_strips(const Harness *harness);
// A depth and stencil attachment: depth tests, a stencil mask and a depth bias (milestone 3).
void test_depth_stencil(const Harness *harness);
// Culling, blending, a write mask, dynamic viewport and scissor, and pixel centres (milestone 3).
void test_raster(const Harness *harness);
// Images rendered, copied, loaded and sampled in the passes after, depth kept in eDRAM (milestone 4).
void test_render_to_texture(const Harness *harness);
// Uniform arrays read at run-time indices: matrices picked per vertex, colours by pixel column (milestone 11).
void test_run_time_index(const Harness *harness);
// Copies from cached memory, R8 and R8G8 through swizzled views, R8 filtered, and copies around a draw (milestone 12).
void test_uploads(const Harness *harness);
// Images one texel high, as palettes are, every texel drawn where it should be (milestone 14).
void test_thin_textures(const Harness *harness);
// A D24S8 attachment stored by one pass, its memory read and its depth sampled by the next (milestone 14).
void test_sampled_depth(const Harness *harness);
// Stripes squeezed 8 to 1, sampled without anisotropy and with 16 (milestone 14).
void test_anisotropy(const Harness *harness);
// Aurora's GLSL for GX configs, each drawn as a point and checked against its WGSL's colour (milestone 18).
void test_aurora_points(const Harness *harness);
// Samplers' LOD bias and clamps, and an image of one level, by the colour of the level each cell shows (milestone 18).
void test_lod(const Harness *harness);
// If/else in vertex shaders, on data and a push constant, in unrolled loops' bodies and around gl_FragDepth, each arm's
// output where it alone puts it (milestone 21).
void test_branches(const Harness *harness);
// Hardware loops counted by push constants and uniform buffers, in either stage and both, with an if/else, an array at
// the pass's index and a break (milestone 21).
void test_loops(const Harness *harness);
// LODs, gradients and biases fragment shaders work out, by the colour of the level each band shows (milestone 22).
void test_shader_lod(const Harness *harness);
// Quads of both windings coloured by gl_FrontFacing under each frontFace, and back faces discarded (milestone 22).
void test_facing(const Harness *harness);
// Columns moved by heights and coloured by levels a vertex shader samples, and the fetch constants' limit (milestone
// 22).
void test_vertex_textures(const Harness *harness);
// Names the layer's cache, a folder emptied first; called before the device is made (milestone 13).
void test_shader_cache_start(void);
// The files the layer kept, and a second device reading the triangle's shaders back; called after the first goes.
void test_shader_cache_finish(void);

#endif
