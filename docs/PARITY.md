# Direct3D 9 parity

How far the layer is from what a port needs, and where each gap lies. Ports are expected from any system up to the
GameCube and Wii, and from PC games of the same years. All of them were written for GPUs of Direct3D 9's class or
less, so the yardstick is Shader Model 3 as a typical card of the time had it, not Vulkan 1.0's conformance. What
Vulkan 1.0 requires and Xenos can't do, compute and integer and bitwise maths in shaders, Direct3D 9 doesn't have
either.

docs/SUPPORT.md says exactly what the layer does; this page sums it up against Direct3D 9. Milestones 20 to 28 in
docs/ROADMAP.md close the gaps. Each row says whether Xenos has it, whether xsdk drives it (xost, for shaders), and
whether the layer exposes it. As of 2026-10-19.

Of the 47 rows, the layer has 38, part of 8 and none of 1.
Xenos has all but three: its occlusion queries tell only whether draws rasterized anything, its gamma only approximates
sRGB, and whether it compares depth in a fetch hasn't been checked, which xost's comparison after the fetch doesn't
need. Every other gap is software.

## Shaders

| Feature | Xenos | xsdk and xost | vulkan4xenon |
|---|---|---|---|
| Vertex and pixel shaders from a high-level language | Yes | Yes: GLSL through xost, offline or on the console | Yes |
| 256 vertex and 224 pixel float constants | Yes | Yes | Yes, 256 a stage |
| Constant arrays at a run-time index, as skinning palettes are | Yes | Yes (xost:docs/xsc.md:72) | Yes |
| `if` and `else` | Yes: conditional execs and predicates (xenia:src/xenia/gpu/ucode.h:101-130) | Yes: selects on data, execs on the predicate where every pixel agrees; a `switch`, and a `return` in an arm, refused (xost:docs/xsc.md, "Control flow") | Yes |
| Loops | Yes (xenia:src/xenia/gpu/ucode.h:132) | Yes: constant counts unrolled; counts in a block, from a start by a step, and counts from data as hardware loops; breaks and continues on data in either, loops nested 4 deep and in an `if` on data, and samples and derivatives in a pass, confirmed on the console (xost:docs/xsc.md, "Loops" and "Loops a block counts") | Yes; a loop runs at most 255 passes, as one of Shader Model 3's does (SUPPORT.md, Shaders) |
| `discard` | Yes | Yes, confirmed on the console (xost:docs/xsc.md:64) | Yes |
| Derivatives | Yes | Yes, confirmed on the console (xost:docs/xsc.md:373) | Yes |
| A computed LOD and gradients (`texldl`, `texldd`) | Yes | Yes, confirmed on the console (xost:docs/xsc.md, "LOD and gradients") | Yes; the LOD from gradients is one a 2x2 quad, from its first pixel, as the console takes it (SUPPORT.md, Shaders) |
| The pixel's position and facing (`vPos`, `vFace`) | Yes | Yes: `gl_FragCoord.xy`, and `gl_FrontFacing`, confirmed on the console (xost:docs/xsc.md:72, "Facing") | Yes |
| Depth output | Yes | Yes: outside any `if`, or in both arms of one; written on one path alone, refused, as nothing else gives the pixel a depth (xost:docs/xsc.md, "Control flow") | Yes |
| Vertex texture fetch | Yes | Yes, confirmed on the console (xost:docs/xsc.md, "Vertex textures") | Yes, sharing the fetch constants with the fragment shader and the vertex streams (SUPPORT.md, Pipelines) |
| 32 temporary registers | Yes | Yes: 64 a shader of each kind under xsdk's split, confirmed on the console (xsdk:docs/HARDWARE.md, "Pixel shaders past 16 registers, milestone 24") | Yes |

## Vertex input and draws

| Feature | Xenos | xsdk and xost | vulkan4xenon |
|---|---|---|---|
| Vertex formats: half floats, bytes and shorts, normalized or not, 10-bit types | Yes (xenia:src/xenia/gpu/xenos.h:602-618) | Yes but the 10-bit types: 18 formats (xsdk:include/xsdk/shader.h) | Partial: half floats, and bytes and shorts normalized or scaled, two or four to an attribute (milestone 20); no 10-bit types |
| 16 vertex streams | Yes | Yes | Yes |
| Points, lines, line strips, triangle lists, strips and fans | Yes | Yes | Yes |
| 32-bit indices, draws past 65,535 | Yes, though a draw packet counts in 16 bits (xenia:src/xenia/gpu/registers.h:327) | Up to 65,535 a draw | Partial: lists and strips are split; fans that long are refused (SUPPORT.md, Commands) |
| Instancing | Yes: a vertex fetch takes its index from a register (xenia:src/xenia/gpu/ucode.h:695-715) | Yes but divisors past 1: xost lowers a vertex shader instanced, its instance worked out from the vertex's index (xost:docs/xsc.md, "Interface conventions"); xsdk draws a list's instances in one call, indexed from repeated indices, a strip's or a fan's a packet each (xsdk:docs/HARDWARE.md, "Instancing on the console, milestone 47 step 1" and "Instanced draws in xsdk, milestone 47 step 3") | Yes: one draw for lists, indexed or not; strips and fans a draw an instance (SUPPORT.md, Commands) |
| Dynamic vertex buffers | n/a | Yes | Yes |

## Textures and samplers

| Feature | Xenos | xsdk and xost | vulkan4xenon |
|---|---|---|---|
| 2D textures, mips, any size | Yes, up to 8192 | Yes | Yes |
| Cube maps | Yes (xenia:src/xenia/gpu/xenos.h:97-102) | Yes, with mips (xsdk:tests/test_cube_volume.c), on the console; xost samples them (xost:docs/xsc.md:41) | Yes, with mips, on the console (milestone 23) |
| Volume textures | Yes | Partial: one level, on the console; xost samples them (xost:docs/xsc.md:42) | Partial: one level, on the console (milestone 23) |
| 16-bit colour, L8, A8, A8L8 | Yes | Yes | Yes: 565, 1555 and 4444 (milestone 20), and R8 and RG8, whose swizzles stand in for L8, A8 and A8L8 |
| DXT1 to 5 | Yes (xenia:src/xenia/gpu/xenos.h:494-536) | Yes | Yes: BC1 to BC3, and BC4 and BC5 from DXT5A and DXN, each reported a format at a time, as `textureCompressionBC` promises BC6H and BC7 too (SUPPORT.md, Formats) |
| Float textures | Yes, though the console filtered 16-bit floats wrong (SUPPORT.md, Formats) | Partial: 16-bit floats and R32F | Partial: 16-bit floats in one, two and four channels, and R32F, nearest filtering only |
| 10-10-10-2, 16-bit fixed point, signed formats for bump maps | Yes | Yes (xsdk_texture_signs for the signed) | Yes: and the signed 8-, 10- and 16-bit formats, not BC4 and BC5's (milestone 20); 10-10-10-2 nearest filtering only, as the console filtered it wrong |
| sRGB | Approximate: a piecewise-linear gamma (xenia:src/xenia/gpu/xenos.cc:27) | Yes, Xenos's gamma | Partial: sampled through Xenos's curve, a deviation; not colour attachments (milestone 20) |
| Point, linear and anisotropic filtering, LOD bias, mip clamps | Yes | Yes | Yes, clamped to whole levels |
| Wrap, mirror and clamp | Yes | Yes | Yes |
| Border and mirror-once addressing | Yes, a black or white border only (xenia:src/xenia/gpu/xenos.h:104-162) | Yes, on the console (xsdk:tests/test_sampling.c) | Yes: clamp to border in transparent black and opaque white, mirror clamp to edge by its extension, on the console (milestone 23) |
| Depth textures and shadow comparison | Depth sampling yes; comparison in the fetch not checked | Depth resolves into textures; xost compares a `sampler2DShadow` after the fetch (xost:docs/xsc.md:43) | Yes: comparison samplers, nearest, and D24S8 filtered linearly, on the console (milestone 23) |

## Render targets and output

| Feature | Xenos | xsdk and xost | vulkan4xenon |
|---|---|---|---|
| Render to texture | Yes | Yes | Yes |
| Float, 10-bit and 64-bit targets | Yes (xenia:src/xenia/gpu/xenos.h:299-313) | Yes but 16_16, 32_32_FLOAT and gamma, at 1 sample; the float targets don't blend (xsdk:TODO.md, "More target formats") | Partial: `A2B10G10R10_UNORM_PACK32`, `R16G16_SFLOAT`, `R16G16B16A16_SFLOAT` and `R32_SFLOAT` attachments, the floats unblended, as Xenos doesn't blend them; 7e3 and 16-bit fixed point have no Vulkan format (SUPPORT.md, Formats) |
| Four render targets at once | Yes, each blended on its own (xenia:src/xenia/gpu/register_table.inc:481, 483-485, 537-547) | Yes, xost's outputs at locations 0 to 3, not yet on the console | Yes, with `independentBlend` (milestone 25) |
| 2x and 4x multisampling, alpha to coverage | Yes (xenia:src/xenia/gpu/registers.h:687) | Multisampling only | No (milestone 17) |
| Targets past eDRAM: 720p multisampled, a big shadow map | Yes, by predicated tiling | Partial: tiled passes at one sample, on the console (xsdk:docs/HARDWARE.md, "Tiled passes at one sample, milestone 44 step 2"); multisampled ones still to come | Partial: yes at one sample, a render pass past eDRAM drawn as a tiled pass, framebuffers to 4096x4096, not yet confirmed on the console (milestone 17's first part; SUPPORT.md, Render passes); multisampled still to come |
| D24S8, depth bias, one-sided stencil | Yes | Yes | Yes |
| Two-sided stencil | Yes (xenia:src/xenia/gpu/registers.h:808) | Yes, not yet confirmed on the console (xsdk:include/xsdk/gpu.h, `two_sided`) | Yes, not yet confirmed on the console (milestone 26) |
| Alpha test | Yes (xenia:src/xenia/gpu/registers.h:685-686) | No | Yes, through `discard`, as Vulkan has it |
| Blending: separate alpha, a constant colour, min and max, write masks | Yes | Yes | Yes |
| User clip planes | Yes (xenia:src/xenia/gpu/registers.h:539-544) | Yes, not yet confirmed on the console: `xsdk_gpu_set_clip_planes`, and xost takes `gl_ClipDistance` as `dot(gl_Position, a plane)` (xost:docs/xsc.md) | Yes, not yet confirmed on the console, in Direct3D 9's form: six planes from blocks (milestone 26) |
| Point sprites with a size | Yes (xenia:src/xenia/gpu/registers.h:184-212) | Yes, not yet confirmed on the console: xost exports `gl_PointSize` and reads `gl_PointCoord` (xost:docs/xsc.md), and xsdk leaves the size unclamped | Yes, not yet confirmed on the console, with `largePoints` (milestone 26) |
| Wireframe and point fill | Yes (xenia:src/xenia/gpu/registers.h:463-469) | Yes, not yet confirmed on the console (xsdk:TODO.md, "Wireframe and point fill") | Yes, not yet confirmed on the console, with `fillModeNonSolid` (milestone 26) |
| Scissor and viewport | Yes | Yes | Yes, one of each |

## Queries and copies

| Feature | Xenos | xsdk and xost | vulkan4xenon |
|---|---|---|---|
| Event queries and time stamps | Yes | Yes | Yes |
| Occlusion queries | Partial: sample counts hang it; its visibility query tells whether draws rasterized anything (xsdk:docs/HARDWARE.md, "Occlusion queries, vulkan4xenon milestone 28") | Partial: the visibility query (xsdk:include/xsdk/gpu.h, `xsdk_gpu_begin_visibility`) | Partial: 1 if a query's draws rasterized anything, a draw hidden behind others too, a deviation without `occlusionQueryPrecise`; not yet confirmed on the console (SUPPORT.md, Queries) |
| `StretchRect` and blits | Yes, through resolves | Resolves, of part of a target too | Yes: drawn on the GPU between images of a format a target takes, into regions on an 8-pixel grid in 8-bit RGBA and BGRA and whole levels in the others; the rest on the CPU, after the GPU is done (SUPPORT.md, Commands) |
| Reading targets back | n/a | Yes | Yes, after the GPU is done |

## Where the gaps lie

- **The layer alone,** as xsdk has it: multisampling. DXT, which most PC ports ship, the 16-bit and half-float texture
  formats, and the vertex formats came with milestone 20.
- **xost:** a `discard` or `return` inside a loop, still refused (xost:docs/xsc.md, "Loops a block counts"). Its loops
  otherwise came with xsdk milestone 46, one running at most 255 passes as one of Shader Model 3's does.
- **xsdk, then the layer:** targets past eDRAM multisampled, and a volume texture's mips. Float targets blending
  waits on the console.

## Past Direct3D 9, for older consoles' ports

Open, each for a decision when a port needs it:
- **Paletted textures** (N64, PS1, PS2, GameCube): Direct3D 9's P8 was rarely supported, so ports expand palettes on
  the CPU or look them up in a shader, which xost's dependent reads allow (xost:docs/xsc.md:87).
- **Quads and rectangles:** Xenos draws both (xenia:src/xenia/gpu/xenos.h:53-56); Vulkan has neither, so a port makes
  triangles.
- **Copies of part of a target,** as GameCube framebuffer copies and N64 framebuffer effects make: xsdk resolves part
  of an 8-bit target into the same texels, on an 8-pixel grid, and the layer takes render areas on it, of one 8-bit
  colour attachment a subpass, and draws a blit's region into another place of another image (milestone 27;
  SUPPORT.md, Render passes and Commands). Areas and blits off the grid inside the destination, areas of other
  targets, and resolves into another place, are left (xsdk:TODO.md, "Resolving part of a target, past vulkan4xenon
  milestone 27").
- **PS2's `(A-B)*C+D` blending:** most of it maps to blend factors; the rest needs dual-source blending, which Xenos
  lacks (xsdk:TODO.md, "Destination alpha"), or work in the shader.
- **Logic ops** (GameCube): Xenia's register definitions show none.

## Not counted

Direct3D 9 doesn't have them: compute, integer and bitwise maths in shaders, atomics, geometry shaders. Bitwise
operations still turn up in GLSL written since; xost refuses them, and shifts by a constant could be multiplies but
aren't yet (xost:docs/xsc.md:364-366).
