# What vulkan4xenon supports

What the layer implements today. Anything not listed here isn't supported. Each limit below the Vulkan spec's minimum
and each deviation gives its reason (CODE_GUIDELINES.md, "Vulkan behavior").

## Version

Vulkan 1.0. `vkCreateInstance` refuses an `apiVersion` above 1.0 with `VK_ERROR_INCOMPATIBLE_DRIVER`, as a 1.0
implementation must. vulkan4xenon claims no conformance: it's a subset, and the deviations below break the spec's
required minimums.

## Extensions

- Instance: `VK_KHR_surface` and `VK_KHR_display`.
- Device: `VK_KHR_swapchain`, and `VK_KHR_sampler_mirror_clamp_to_edge` for mirror clamp to edge (Samplers, milestone
  23).
- No layers.

## Functions

| Group | Functions |
|---|---|
| Instance | `vkCreateInstance`, `vkDestroyInstance`, `vkEnumerateInstanceExtensionProperties`, `vkEnumerateInstanceLayerProperties`, `vkGetInstanceProcAddr`, `vkEnumeratePhysicalDevices` |
| Physical device | `vkGetPhysicalDeviceProperties`, `vkGetPhysicalDeviceFeatures`, `vkGetPhysicalDeviceQueueFamilyProperties`, `vkGetPhysicalDeviceMemoryProperties`, `vkGetPhysicalDeviceFormatProperties`, `vkGetPhysicalDeviceImageFormatProperties`, `vkGetPhysicalDeviceSparseImageFormatProperties`, `vkEnumerateDeviceExtensionProperties`, `vkEnumerateDeviceLayerProperties` |
| Device | `vkCreateDevice`, `vkDestroyDevice`, `vkGetDeviceProcAddr`, `vkGetDeviceQueue`, `vkDeviceWaitIdle` |
| Memory and buffers | `vkAllocateMemory`, `vkFreeMemory`, `vkMapMemory`, `vkUnmapMemory`, `vkFlushMappedMemoryRanges`, `vkInvalidateMappedMemoryRanges`, `vkGetDeviceMemoryCommitment`, `vkCreateBuffer`, `vkDestroyBuffer`, `vkGetBufferMemoryRequirements`, `vkBindBufferMemory`, `vkCreateBufferView`, `vkDestroyBufferView` |
| Pipelines | `vkCreateShaderModule`, `vkDestroyShaderModule`, `vkCreatePipelineLayout`, `vkDestroyPipelineLayout`, `vkCreateGraphicsPipelines`, `vkCreateComputePipelines`, `vkDestroyPipeline`, `vkCreatePipelineCache`, `vkDestroyPipelineCache`, `vkGetPipelineCacheData`, `vkMergePipelineCaches` |
| Images and samplers | `vkCreateImage`, `vkDestroyImage`, `vkGetImageMemoryRequirements`, `vkGetImageSparseMemoryRequirements`, `vkGetImageSubresourceLayout`, `vkBindImageMemory`, `vkCreateImageView`, `vkDestroyImageView`, `vkCreateSampler`, `vkDestroySampler` |
| Descriptor sets | `vkCreateDescriptorSetLayout`, `vkDestroyDescriptorSetLayout`, `vkCreateDescriptorPool`, `vkDestroyDescriptorPool`, `vkResetDescriptorPool`, `vkAllocateDescriptorSets`, `vkFreeDescriptorSets`, `vkUpdateDescriptorSets` |
| Render passes | `vkCreateRenderPass`, `vkDestroyRenderPass`, `vkCreateFramebuffer`, `vkDestroyFramebuffer`, `vkGetRenderAreaGranularity` |
| Commands | `vkCreateCommandPool`, `vkDestroyCommandPool`, `vkResetCommandPool`, `vkAllocateCommandBuffers`, `vkFreeCommandBuffers`, `vkBeginCommandBuffer`, `vkEndCommandBuffer`, `vkResetCommandBuffer`, `vkCmdBeginRenderPass`, `vkCmdNextSubpass`, `vkCmdEndRenderPass`, `vkCmdExecuteCommands`, `vkCmdBindPipeline`, `vkCmdBindVertexBuffers`, `vkCmdBindIndexBuffer`, `vkCmdBindDescriptorSets`, `vkCmdPushConstants`, `vkCmdSetViewport`, `vkCmdSetScissor`, `vkCmdSetLineWidth`, `vkCmdSetDepthBias`, `vkCmdSetBlendConstants`, `vkCmdSetDepthBounds`, `vkCmdSetStencilCompareMask`, `vkCmdSetStencilWriteMask`, `vkCmdSetStencilReference`, `vkCmdDraw`, `vkCmdDrawIndexed`, `vkCmdDrawIndirect`, `vkCmdDrawIndexedIndirect`, `vkCmdDispatch`, `vkCmdDispatchIndirect`, `vkCmdCopyBufferToImage`, `vkCmdCopyImageToBuffer`, `vkCmdCopyImage`, `vkCmdBlitImage`, `vkCmdClearColorImage`, `vkCmdClearDepthStencilImage`, `vkCmdClearAttachments`, `vkCmdCopyBuffer`, `vkCmdUpdateBuffer`, `vkCmdFillBuffer`, `vkCmdResolveImage`, `vkCmdPipelineBarrier`, `vkCmdSetEvent`, `vkCmdResetEvent`, `vkCmdWaitEvents`, `vkCmdResetQueryPool`, `vkCmdBeginQuery`, `vkCmdEndQuery`, `vkCmdWriteTimestamp`, `vkCmdCopyQueryPoolResults` |
| Sync and submit | `vkQueueSubmit`, `vkQueueBindSparse`, `vkQueueWaitIdle`, `vkCreateFence`, `vkDestroyFence`, `vkResetFences`, `vkGetFenceStatus`, `vkWaitForFences`, `vkCreateSemaphore`, `vkDestroySemaphore`, `vkCreateEvent`, `vkDestroyEvent`, `vkGetEventStatus`, `vkSetEvent`, `vkResetEvent` |
| Queries | `vkCreateQueryPool`, `vkDestroyQueryPool`, `vkGetQueryPoolResults` |
| Display | `vkGetPhysicalDeviceDisplayPropertiesKHR`, `vkGetPhysicalDeviceDisplayPlanePropertiesKHR`, `vkGetDisplayPlaneSupportedDisplaysKHR`, `vkGetDisplayModePropertiesKHR`, `vkCreateDisplayModeKHR`, `vkGetDisplayPlaneCapabilitiesKHR`, `vkCreateDisplayPlaneSurfaceKHR`, `vkDestroySurfaceKHR`, `vkGetPhysicalDeviceSurfaceSupportKHR`, `vkGetPhysicalDeviceSurfaceCapabilitiesKHR`, `vkGetPhysicalDeviceSurfaceFormatsKHR`, `vkGetPhysicalDeviceSurfacePresentModesKHR` |
| Swapchain | `vkCreateSwapchainKHR`, `vkDestroySwapchainKHR`, `vkGetSwapchainImagesKHR`, `vkAcquireNextImageKHR`, `vkQueuePresentKHR` |

Apps call them directly. `vkGetInstanceProcAddr` finds global functions with a `NULL` instance and every function
with one; `vkGetDeviceProcAddr` finds device functions only.

Some are there to refuse, as the features they need aren't reported. Each logs what it refuses and returns
`VK_ERROR_FEATURE_NOT_PRESENT`, or for a command fails its command buffer, so `vkEndCommandBuffer` returns it
(milestone 19):
- compute: `vkCreateComputePipelines`, which sets each pipeline to `VK_NULL_HANDLE`, `vkCmdDispatch` and
  `vkCmdDispatchIndirect` (Device);
- `vkCreateBufferView`, as no format has texel buffer features (Formats), so `vkDestroyBufferView` has nothing to do;
- `vkCmdResolveImage`, as images have one sample;
- `vkCmdSetDepthBounds`, with no depth bounds test (Pipelines);
- `vkCreateQueryPool` for pipeline statistics queries, and `vkCmdBeginQuery` with `VK_QUERY_CONTROL_PRECISE_BIT` or
  outside a render pass (Queries);
- `vkQueueBindSparse`, with no sparse binding (Device).

## Device

- One physical device, and one device at a time: a second `vkCreateDevice` returns `VK_ERROR_TOO_MANY_OBJECTS`, since
  the GPU has one owner. `vkCreateDevice` takes the GPU from XAM (`xsdk_gpu_open`) and shows the boot logo.
- Features: `drawIndirectFirstInstance` (Commands), `independentBlend`, `fillModeNonSolid` and `largePoints`
  (Pipelines), `samplerAnisotropy` (Samplers) and `shaderClipDistance` (Shaders). Requesting any other returns
  `VK_ERROR_FEATURE_NOT_PRESENT`. Vulkan 1.0 requires `robustBufferAccess`; that's a deviation.
- One queue family with one queue, `VK_QUEUE_GRAPHICS_BIT` only. Vulkan 1.0 requires a family with graphics and
  compute together; Xenos has no compute stage, so that's a deviation. There's no sparse binding either: the GPU
  addresses physical memory, and Xenia's registers show no GPU MMU (xenia:src/xenia/gpu/register_table.inc).

## Memory and buffers

- One heap of 256 MB, device-local. That's a budget, half the console's 512 MB that the CPU and GPU share, not a
  measurement.
- Two memory types, both in that heap:
  - 0: device-local, host-visible and coherent. Each allocation is `xsdk_gpu_alloc`'s write-combined physical memory,
    in 64 KB pages, so `minMemoryMapAlignment` is 64 KB.
  - 1: host-visible, coherent and cached: the CPU's own memory, from newlib's heap, 128-byte aligned. The GPU never
    reads it, so only buffers whose usages are `VK_BUFFER_USAGE_TRANSFER_SRC_BIT` and `VK_BUFFER_USAGE_TRANSFER_DST_BIT`
    alone list it in their `memoryTypeBits`: copies run on the CPU, which reads and writes it at the cache's speed
    (milestones 12 and 19), so a buffer an image is read back into is quick to map and read. Another buffer, or an
    image, bound to it makes the bind return `VK_ERROR_OUT_OF_DEVICE_MEMORY`.
- Memory of type 0 is write-combined:
  - `memcpy` into it is safe, since the toolchain's `memcpy` stores bytes or aligned words;
  - a plain 8-byte store needs an 8-byte-aligned address, or it raises an alignment exception (xsdk:CLAUDE.md);
  - reading it back is slow, as it isn't cached.
- Coherence: `vkQueueSubmit` drains the CPU's write-combining (`sync`) before replaying, so flushing ranges does
  nothing and isn't needed.
- No memory type is lazily allocated, so `vkGetDeviceMemoryCommitment` gives an allocation's whole size.
- Buffers: 64-byte alignment, sizes rounded up to it, so the GPU can load uniform blocks straight from them. Sparse
  buffers return `VK_ERROR_FEATURE_NOT_PRESENT`.

## Shaders

- `vkCreateShaderModule` takes standard SPIR-V and looks it up in the app's `v4x_shader_bundle`, which
  `tools/build_shaders.py` writes: GLSL → `glslang -V` → xost → a Xenos container. The key is a 64-bit FNV-1a of the
  SPIR-V and its size (`include/vulkan4xenon.h`), and whether the container is a vertex shader's instanced variant
  (below).
- SPIR-V the bundle doesn't hold is compiled on the console the first time a module is made from it, through xost's
  compiler in xsdk (`xsdk_xsc_compile`), at xsdk's register split with the optimiser on, and kept till the device goes
  (xsdk:docs/scope/shader-compiling.md, step 3); so is an instanced variant the bundle lacks, the first time a pipeline
  wants it, from the module's SPIR-V, which the module keeps a copy of. The layer logs each shader it compiles and the
  time it took.
- An app that names a folder with `v4x_set_shader_cache`, before it makes shader modules, has what the layer compiles
  kept there, in a folder named after the compiler's version (`xsdk_xsc_version()`), one file a shader named after its
  SPIR-V's hash and size, `-i` after them for an instanced variant. Each file is written beside its name and renamed
  into place, so a crash leaves none half written. A later device reads a shader back rather than compile it; a file
  that doesn't match its name, or whose size doesn't add up, is logged and the shader compiled again.
  `v4x_shader_counts` says how many the layer compiled and read back. With no folder named, nothing is kept.
- SPIR-V xost refuses returns `VK_ERROR_FEATURE_NOT_PRESENT`, logged with xost's reason, and so does a shader whose
  hardware loop a bare uniform counts, which Vulkan's GLSL has none of. `vkCreateShaderModule` doesn't list that
  result: a deviation.
- The shader subset is xost's (xost:docs/xsc.md): no bitwise operations. A shader of each kind has 64 registers,
  xsdk's split, which xost builds for (xsdk:docs/HARDWARE.md, "Pixel shaders past 16 registers, milestone 24").
- `if`/`else` in either stage, nested, in an unrolled loop's body and around `gl_FragDepth` (milestone 21;
  xost:docs/xsc.md, "Control flow"). In a pixel shader, where the condition reads only push constants, uniform blocks
  and literals, so every pixel takes the same arm, and an arm has work of its own, the arms go in execs on the predicate
  and the arm not taken runs no instructions (xenia:src/xenia/gpu/ucode.h:114-124); a vertex shader's arms in such execs
  hung the GPU on the console (xsdk:docs/HARDWARE.md, "Branches, vulkan4xenon milestone 21"), so a vertex shader's stay
  selects. Otherwise both arms run and a select picks each value, as a group of 64 that splits runs both arms on Xenos
  anyway. A `break` or `continue` in an arm is taken as loops take one (below). Refused, with xost's reason: a `return`
  in an arm, a `discard` in an arm xost's unroller merges, a `switch`, and a matrix, array or struct an `if`/`else`
  chooses. An output stored on one path alone takes that value on the other, where GLSL leaves it undefined.
- A fragment shader may write `gl_FragDepth` outside any `if`/`else`, in both arms of one, or before one and again in
  an arm; written on one path alone, it's refused, as nothing else gives the pixel a depth (xost:docs/xsc.md,
  "Control flow"). Its draws skip no hidden pixels early, as xsdk turns hierarchical depth off for them
  (xsdk:docs/HARDWARE.md, "A pixel shader's depth").
- Fragment outputs at locations 0 to 3 go to the subpass's colour attachments 0 to 3, xost's `oC0` to `oC3`, a float
  output in every component; with more than one, each is stored once, outside any `if`/`else` (xost:docs/xsc.md,
  milestone 25).
- **Loops,** in every shape xost takes (milestone 21 and xsdk milestone 46; xost:docs/xsc.md, "Loops" and "Loops a
  block counts"):
  - a constant count, which xost unrolls;
  - `for (int i = s; i < b.n; i += k)`, counted by an int in a push constant or uniform block from a constant start by
    a constant step;
  - a loop whose count is data: a `for` to `int()` of a float or an interpolant, a `while` or a `do`/`while`;
  - breaks and continues on data in either kind of loop, each vertex or pixel leaving at a pass of its own;
  - loops nested 4 deep, and loops in an `if`/`else` on data;
  - samples and derivatives in a pass.

  All but the unrolled loops are hardware loops. The bundle names where a block's int is, with the loop's start and
  step; each draw the layer reads it, from the push constants or the bound buffer at its dynamic offset, and sets the
  loop constant xost gave the loop to `xsdk_xsc_loop_passes(start, step, n)` passes, ceil((n - start) / step) as
  Shader Model 3's `loop` counts them (`V4X_RESOURCE_PUSH_LOOP`, `V4X_RESOURCE_UNIFORM_LOOP`). A loop whose count is
  data runs from a loop constant of 255 passes (`V4X_RESOURCE_FIXED_LOOP`), left on its condition. A vertex shader's
  loop constants are from 0, a fragment shader's from 16, and they're written only when one changes.
  **Deviation:** a loop runs at most 255 passes, the most a loop constant counts
  (xenia:src/xenia/gpu/xenos.h:1086-1097), as one of Shader Model 3's does: a count past 255 runs 255, and a loop
  whose count is data stops at 255 passes too. 0 or less runs none, as Vulkan does. An implicit LOD or a derivative in
  a pass reads the quad's pixels in that pass, which mixes passes where they left at different ones; GLSL leaves both
  undefined there. Refused, with xost's reason: a `discard` or `return` in a loop, a store into part of an output in a
  hardware loop, a fifth nested hardware loop, a loop's condition on a block's int other than `i < b.n`, and a matrix,
  array or struct a hardware loop carries. A pipeline whose loop's int lies past the push constants, or whose block
  loop has no step, from a bundle built before xost gave one, returns `VK_ERROR_FEATURE_NOT_PRESENT`.
- Samples at a LOD, with gradients or with a bias the shader gives, constant or worked out: `textureLod`, `textureGrad`
  and `texture()`'s bias, and their projective forms (milestone 22; xost:docs/xsc.md, "LOD and gradients"). The
  sampler's LOD bias and clamps apply on top, as Vulkan's do. Gradients on a cube map, offsets (`textureOffset` and the
  like), a sample index and a minimum LOD are refused, as xost refuses them. **Deviation:** a LOD from `textureGrad`'s
  gradients is one a 2x2 quad, from its first pixel inside the primitive, top left, top right, bottom left then bottom
  right, not each pixel's own (xsdk:docs/HARDWARE.md, "Samples in a loop's pass, milestone 46 step 6"). Gradients the
  same across a quad, as derivatives of a value linear across it are, take the same LOD either way; a LOD
  `textureLod` gives is each pixel's own.
- A fragment shader may read `gl_FrontFacing`, from the pixel parameters' sign, in an expression, an `if`/`else` or a
  discard (milestone 22; xost:docs/xsc.md, "Facing"). It holds on the face the pipeline's `frontFace` makes front,
  as culling takes it, as it did on the console for both windings (milestone 22).
- Vertex input location n feeds the usage xost gives it: 0 position, 1 colour, 2 and up texture coordinates
  (xsdk:src/xsc_resolve.c:344-346).
- **Instanced vertex shaders** (xsdk milestone 47; xost:docs/xsc.md, "Interface conventions"). A vertex shader that
  reads `gl_InstanceIndex`, `firstInstance` included, is lowered instanced: a prologue works out the instance and the
  vertex from the vertex's index and the instance group, a float constant past the shader's blocks, which the bundle
  records as a `V4X_RESOURCE_INSTANCE` resource and xsdk writes before each piece of a draw (Commands). A float holds
  the instance exactly up to 2^24. A pipeline with a binding at the instance's rate takes its vertex shader's
  instanced variant, compiled with xost's `instanced` option: `tools/build_shaders.py` builds one for every vertex
  shader that doesn't read `gl_InstanceIndex`, the bundle's entry marked `instanced`, and the layer compiles one on
  the console where the bundle lacks it (above). A bundle written before the variants holds plain shaders alone, which
  the layer takes as before. The variant's prologue costs a vertex four ALU instructions, which pipelines without
  instancing don't pay. xost refuses `gl_VertexIndex`.
- **Point sprites** (milestone 26): `gl_PointSize` goes to the point size Xenos's vertex shaders export, `oPts.x`, the
  point's width in pixels (xenia:src/xenia/gpu/ucode.h:1813-1834), and `gl_PointCoord` reads the pixel parameters'
  |zw|, a point sprite's coordinates from its top left (xenia:src/xenia/gpu/registers.h:184-212); xost:docs/xsc.md
  has both.
- **Clip distances,** with `shaderClipDistance` (milestone 26), in the form Direct3D 9's clip planes take: Xenos clips
  against six planes in clip space, keeping where dot(the exported position, plane) >= 0
  (xenia:src/xenia/gpu/registers.h:539-544, dxbc_shader_translator.cc:1033-1053). So `gl_ClipDistance[n]` written as
  `dot(gl_Position, plane)`, either way round, the plane a whole `vec4` of a uniform block or push constants at a fixed
  index, is user clip plane n: xost records the plane's register (`V4xShaderClipPlane`), and each draw sets the plane
  from the values bound then (Pipelines). **Deviation:** a distance in any other form, from another point, a plane
  the shader works out, an index past 5, or a cull distance, is refused when the module is made: xost logs why, and
  `vkCreateShaderModule` returns `VK_ERROR_FEATURE_NOT_PRESENT`, or `tools/build_shaders.py` stops. No cull distances:
  Xenos has none.
- The literal constants xost pools are written when a pipeline is bound, at the registers after the shader's
  uniforms.
- **A fragment shader's constants stay below c252.** xsdk keeps c252 to c255, the group xost's `gl_FragCoord` reads its
  offset from, which xsdk moves for each band of a tiled pass (xsdk:include/xsdk/gpu.h, `XSDK_PIXEL_CONSTANTS`), so a
  fragment shader whose uniform blocks, push constants, comparisons or literals reach c252 makes
  `vkCreateGraphicsPipelines` return `VK_ERROR_FEATURE_NOT_PRESENT`; written, they'd move `gl_FragCoord` for the shaders
  after. `maxUniformBufferRange` is 4032 to match (Physical device).
- Where a shader's resources land is xost's choice, recorded in the bundle as its `V4xShaderResource`s
  (xost:docs/xsc.md, "Interface conventions"):
  - push constants take the shader's first float constant registers, then each uniform block in (set, binding) order,
    each holding the block's std140 bytes from its first;
  - combined image samplers take fetch constants from 0 in (set, binding) order, in each stage; the layer moves the
    vertex shader's past the fragment shader's (Pipelines);
  - a `sampler2DShadow`'s comparison takes a float constant past the blocks, which the layer writes at each draw with
    the bound sampler's compare op (Samplers).

## Pipelines

- Pipeline layouts: up to 4 descriptor set layouts, and push constant ranges within 128 bytes.
- A pipeline's shaders read only what its layout has, for their stage. A resource the layout lacks makes
  `vkCreateGraphicsPipelines` log it and return `VK_ERROR_INITIALIZATION_FAILED`, which it doesn't list: that's invalid
  use, refused rather than drawn wrong.
- Both stages sample, up to 16 samplers each, and each stage's blocks must fit its float constants: a vertex shader's
  256, a fragment shader's 252 (Shaders). A vertex shader samples at the LOD it gives: `texture()` there samples level
  0, as glslang gives it LOD 0 (milestone 22; xost:docs/xsc.md, "Vertex textures").
- **The fetch constants shared out:** both stages and the vertex streams draw on the same 32
  (xsdk:include/xsdk/gpu.h, `xsdk_gpu_set_texture` and `xsdk_gpu_set_vertices`). The fragment shader's samplers keep
  xost's slots from 0, the vertex shader's take the slots after them, its microcode moved to match when the pipeline is
  made (`xsdk_shader_move_textures`), and the bindings' streams take a slot for every three from 31 down, slot 31
  always, as the layer's own blits and clears draw from stream 0. **Deviation:** a pipeline whose samplers and
  bindings need more returns `VK_ERROR_FEATURE_NOT_PRESENT` and logs the counts, though within
  `maxPerStageDescriptorSamplers`: 16 samplers in each stage never fit, and with all 16 bindings the two stages share
  26.
- Graphics pipelines, otherwise `VK_ERROR_FEATURE_NOT_PRESENT`:
  - a vertex and a fragment stage, entry point `main`, no specialization constants;
  - bindings at the vertex's rate or the instance's, with strides a multiple of 4, up to 1020 bytes. An attribute at
    the instance's rate is fetched at its instance's element, at its binding's stride, by the instanced vertex shader
    such a pipeline takes (Shaders; xsdk:include/xsdk/shader.h, `per_instance`), its stream at the binding's start;
  - attributes of the vertex buffer formats (Formats) at offsets a multiple of 4;
  - point lists, line lists and strips, and triangle lists, strips and fans, without primitive restart (an xsdk
    TODO);
  - one viewport and scissor, set in the pipeline or dynamic;
  - every other dynamic state of Vulkan 1.0's but depth bounds, the pipeline's own value then ignored (Dynamic
    state);
  - polygons filled, or as lines or points with `fillModeNonSolid`; line width 1; no depth clamp;
  - one sample;
  - a blend state for each of the subpass's colour attachments, up to four.
- **Rasterization:**
  - culling front or back faces, or none; culling both returns `VK_ERROR_FEATURE_NOT_PRESENT`;
  - `polygonMode` `LINE` and `POINT` (milestone 26): xsdk's fill, `PA_SU_SC_MODE_CNTL`'s `poly_mode` in dual mode with
    both faces' polygon types lines or points (xenia:src/xenia/gpu/registers.h:463-469, xenos.h:846-859), each
    triangle's edges or corners, after culling, as Vulkan has it. Lines are 1 pixel wide and points as a point list's
    (Points, below); a depth bias biases them as it does a filled polygon, as Xenia takes it
    (xenia:src/xenia/gpu/draw_util.cc:93-121);
  - `frontFace` maps straight to xsdk's `front_clockwise`. Both mean clockwise as the triangle looks in the y-down
    framebuffer (xsdk:tests/test_raster.c:207), and the viewport flip keeps every vertex where Vulkan puts it;
  - depth bias: the constant factor is in units of 2^-24, D24's resolution, so the layer divides it by 2^24 into the
    hardware's depth units (xenia:src/xenia/gpu/draw_util.h:165-172). The slope factor passes through as xsdk's
    per-pixel slope, unverified as in xsdk. A bias clamp returns `VK_ERROR_FEATURE_NOT_PRESENT`;
  - clip distances (Shaders): a pipeline finds each plane in the vertex shader's push constants or uniform block,
    and each draw reads it from them as bound, a dynamic offset and all, and sets the planes
    (`xsdk_gpu_set_clip_planes`). The CPU reads a uniform buffer's plane from mapped memory, 16 bytes a plane a
    draw. A draw of a pipeline without clip distances turns off what the last left on, and so does
    `vkCmdClearAttachments`, whose rectangle no plane cuts.
- **Depth and stencil,** with a depth attachment:
  - depth test, write and compare op;
  - stencil test, ops, compare op, masks and reference, each face its own (milestone 26). Where the faces differ,
    the pipeline sets xsdk's two-sided stencil, `RB_DEPTHCONTROL`'s `backface_enable` with the back face's fields and
    `RB_STENCILREFMASK_BF` (xenia:src/xenia/gpu/registers.h:799-834); where they're the same, one-sided stencil serves
    both, as before. Back faces are the ones culling takes for back, by `frontFace`; lines and points are front faces,
    as Vulkan says, and as Xenia reads them (xenia:src/xenia/gpu/vulkan/vulkan_pipeline_cache.cc:650-668).
  - Vulkan's compare and stencil ops are xsdk's, value for value. No depth bounds test.
- **Blending,** each colour attachment by its own state, as each of Xenos's colour targets blends by its own
  `RB_BLENDCONTROL` and masks by its own four bits of `RB_COLOR_MASK` (`xsdk_gpu_set_blends`;
  xenia:src/xenia/gpu/register_table.inc:537-547), so the device has `independentBlend` (milestone 25):
  - blending into a float attachment, `R16G16_SFLOAT`, `R16G16B16A16_SFLOAT` or `R32_SFLOAT`, returns
    `VK_ERROR_FEATURE_NOT_PRESENT`: Xenos's float targets don't blend, a quad added onto itself coming out wrong on
    the console (xsdk:docs/HARDWARE.md, "Float, 10-bit and 64-bit targets and four at once"), so their formats report
    no `VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT`; their write masks work;
  - every factor but the dual-source ones, which return `VK_ERROR_FEATURE_NOT_PRESENT`. The constant ones take Xenos's
    constant colour, `RB_BLEND_RED` to `ALPHA`, which the layer sets with each pipeline whose blend reads it
    (`xsdk_gpu_set_blend_constants`; xsdk:docs/HARDWARE.md, "Blend constants");
  - every op, with min and max ignoring their factors as Vulkan says. Xenia ignores them too
    (xenia:src/xenia/gpu/spirv_shader_translator_rb.cc:3324), and the layer sets them to one, so the result holds either
    way;
  - the colour write mask. No logic ops.
- **Dynamic state:** the pipeline keeps the xsdk settings its raster and depth and stencil blocks came from, and a
  draw whose pipeline takes state from commands works them out again with what the commands last set, once a
  pipeline is bound or a command sets it:
  - `vkCmdSetLineWidth` takes 1 alone, as without `wideLines`;
  - `vkCmdSetDepthBias` gives the constant and slope factors, which bias only if the pipeline's `depthBiasEnable` is
    set; a clamp fails the command buffer, as there's no `depthBiasClamp`;
  - `vkCmdSetStencilCompareMask`, `vkCmdSetStencilWriteMask` and `vkCmdSetStencilReference` set the faces their face
    mask names, each its own (milestone 26), and the faces' settings are worked out again, two-sided where they
    differ;
  - `vkCmdSetBlendConstants` gives the constant colour;
  - a pipeline that doesn't take a state has its own, whatever the commands set before it.
- **Points,** with `largePoints` (milestone 26): a point is as wide as the vertex shader's `gl_PointSize`, which xsdk
  leaves unclamped but by `PA_SU_POINT_MINMAX`'s range, radii of 0 to 0xFFFF sixteenths of a pixel
  (xenia:src/xenia/gpu/registers.h:433-442): `pointSizeRange` 0 to 8191.875, `pointSizeGranularity` 0.125. Without
  `gl_PointSize` written, a point is 1 pixel, xsdk's `PA_SU_POINT_SIZE`, as Vulkan says.
- Pipeline caches hold nothing, since pipelines come from the shader bundle and the layer's own cache of compiled
  shaders (milestone 13). The data a cache is made with is ignored, `vkMergePipelineCaches` does nothing, and
  `vkGetPipelineCacheData` gives the 32-byte header alone, its words least significant byte first as the spec says,
  vendor, device and UUID 0.
- Clip space is Vulkan's:
  - y points down, so the viewport is set from its bottom edge with a negative height, against xsdk's y-up
    (xsdk:src/gpu_draw.c:298);
  - pixel centres sit on halves. Xenos puts them on whole coordinates under xsdk's `PIX_CENTER_D3D`
    (xenia:src/xenia/gpu/xenos.h:861-866), so the viewport moves half a pixel up and left. On the console, edges a
    quarter pixel off the grid covered exactly what Vulkan's centres say (milestone 3);
  - nothing is drawn past the viewport. Xenos draws past it as far as the scissor: on the console, a draw over a
    viewport 1168 pixels wide in a target 1280 wide covered all 1280 (xsdk:results/2026-10-16-fifth-vertex-fetch/). So
    the scissor the layer sets is the app's within the pixels the viewport covers.

## Render passes

- Up to 8 subpasses over up to 8 attachments. Each subpass has one to four colour attachments, of the formats that
  are colour attachments (Formats), one sample, and optionally a depth attachment, `VK_FORMAT_D24_UNORM_S8_UINT`;
  none, unused colour attachments, and input and resolve attachments return `VK_ERROR_FEATURE_NOT_PRESENT`.
- **A subpass past eDRAM is a tiled pass** (milestone 17's first part): one whose colour targets and depth pass eDRAM's
  2048 tiles at the framebuffer's size, a 64-bit target taking twice a 32-bit one's tiles
  (`xsdk_gpu_color_target_tiles`), is drawn as xsdk's tiled pass (xsdk:include/xsdk/gpu.h, `xsdk_gpu_begin_tiles`). At
  1280x720 one 32-bit colour attachment and depth fit, or one 64-bit one without, or two 32-bit ones without; more
  tiles. Past eDRAM, at one sample:
  - **Its commands are replayed once,** at `vkQueueSubmit`, into a recording that each band of rows runs, as few bands
    as fit, each moved to eDRAM's top (xsdk:docs/HARDWARE.md, "Tiled passes at one sample, milestone 44 step 2").
  - **Stores:** each colour attachment is resolved a band at a time into its image, whatever its store op, which clears
    it to its clear value for the next band.
  - **Loads:** each load op's image is drawn back at the recording's start, so each band draws its own rows, into its
    target alone by the write masks, as a band's targets can't be set one at a time. The resolve that clears band 0
    before the first band leaves those images alone (`XsdkTileResolve`'s `drawn_back`). A clear needs nothing more.
  - **Depth stays whole** in eDRAM, ending at its top, wherever it fits beside a band of colour, so it lasts between
    passes as an untiled pass keeps it, tiled and untiled passes taking it from each other (`XsdkTiling`'s
    `depth_top`). Every subpass up to 1280x720 fits. Resolves don't clear it, so a pass that clears it draws its clear
    first, unless eDRAM holds that clear already; one left to be read is resolved into its texture a band at a time.
  - **Otherwise depth is in bands:** each band's is cleared with it and, if the pass stores depth, resolved into the
    depth image's texture a band at a time. A pass that loads it draws that texture's depth back first, through the
    pixel shader, which takes a depth image made with `VK_IMAGE_USAGE_SAMPLED_BIT`: with any other,
    `vkCreateFramebuffer` returns `VK_ERROR_FEATURE_NOT_PRESENT`. The texture holds what the last pass that stored the
    depth in bands, or left it to be read, put there. **Deviation:** stencil doesn't come back, as Xenos exports none
    from a pixel shader, and the layer logs that once; the depth comes back through a texture fetch, which xsdk_test
    finds within 2 in 2^24 of what was stored (xsdk:tests/test_tiles.c, `DEPTH_24_TOLERANCE`).
  - **Refused inside it:** queries and time stamps, as each band would run them; `vkEndCommandBuffer` returns
    `VK_ERROR_FEATURE_NOT_PRESENT`, and one in a secondary command buffer is dropped at replay and logged. A render area
    smaller than the framebuffer fails the command buffer the same way, as each band resolves all its rows.
  - **Shortcut:** each subpass past eDRAM records into 1 MB of packets its framebuffer holds, some thousands of the
    layer's draws; a pass that outgrows it draws nothing, and the layer logs it. A pass waits for the GPU to finish the
    last one its framebuffer's subpass recorded, so a framebuffer used twice in a submission costs a wait.
  - Up to 4096x4096, any subpass fits xsdk's 64 bands.
- **Subpasses, each a pass of its own** (milestone 19): `vkCmdBeginRenderPass` and `vkCmdNextSubpass` begin each
  subpass as the replay begins a render pass, and `vkCmdNextSubpass` and `vkCmdEndRenderPass` end it. An attachment
  takes its own load op in the first subpass that uses it and its store op in the last; the subpasses between load and
  store it, so a colour attachment is resolved at each subpass that draws it and found in eDRAM, or drawn back, by the
  next, and depth stays in eDRAM throughout. Preserved attachments need nothing, as every attachment a later subpass
  uses is kept, and replay in order makes the dependencies nothing.
- Every load op: clear, load or don't-care. Store op store or don't-care.
- Depth and stencil clear together, as one register holds both (xsdk:src/gpu_resolve.c). A depth attachment that
  clears one and loads the other returns `VK_ERROR_FEATURE_NOT_PRESENT`.
- **Depth stays in eDRAM:** a depth attachment is stored in eDRAM. Loading it finds what the last pass left there, as
  long as no colour and no other depth reached its tiles in between: a pass with another depth places it under the
  first where both fit above the pass's colour (How it maps). **Deviation:** otherwise its contents are undefined, and
  the layer logs that once.
- **A depth image's clear waits for its pass:** `vkCmdClearDepthStencilImage` can't reach eDRAM, so it keeps its depth
  and stencil on the image, and the next pass with the image as its depth attachment clears to them, if it doesn't
  clear to its own (milestone 19).
- **Depth to sample:** a depth image made with `VK_IMAGE_USAGE_SAMPLED_BIT` is resolved out of eDRAM into its texture
  (`xsdk_gpu_resolve_depth_texture`) when a pass stores its depth or stencil and leaves it in a layout a shader samples
  in: `finalLayout` `VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL`, `VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL` or
  `VK_IMAGE_LAYOUT_GENERAL`. A pass that leaves it `VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL` keeps it in eDRAM
  alone, so the passes a later one loads cost no resolve (milestone 14). The resolve comes before the colour's and
  clears nothing, so the colour's clears as it would.
- A barrier moving a sampled depth image from `VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL` to one of those
  layouts fails the command buffer: `vkEndCommandBuffer` returns `VK_ERROR_FEATURE_NOT_PRESENT`, as only a pass's end
  resolves depth. Depth images can't be copied.
- Framebuffers: the render pass's attachments, their full size, up to 4096x4096, one layer. Colour attachments are
  swapchain images or images made with `VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT`.
- **Render areas** (milestone 27): a pass clears, loads and stores its render area alone, as Vulkan says, by resolving
  that part of the target (`xsdk_gpu_resolve_texture_rect`). A resolve copies and clears 8 by 8 pixels at a time
  (xsdk:include/xsdk/gpu.h), so `vkGetRenderAreaGranularity` gives 8x8, and an area's offset is a multiple of 8, and so
  are its width and height unless it reaches the framebuffer's right or bottom edge: what Vulkan calls an optimal area.
  **Deviation:** an area off that grid inside the framebuffer fails the command buffer, though Vulkan takes any area,
  the granularity being only for speed.
  **Deviation:** xsdk resolves a rectangle of the first colour target alone, in 8_8_8_8, and none of the float or
  10-bit targets (xsdk:include/xsdk/gpu.h), so an area smaller than the framebuffer takes a render pass whose subpasses
  each have one colour attachment, `R8G8B8A8_UNORM` or `B8G8R8A8_UNORM` (a swapchain image's), and a depth attachment
  if they have one. Any other render pass with a smaller area fails the command buffer; with the whole framebuffer as
  its area it takes every attachment the layer does.
  - A clear resolves the area into the attachment, clearing eDRAM there, as a whole pass's clear does; a load draws
    the image back over the area alone; the store resolves the area alone, so the image around it keeps what it held.
  - Depth clears in the area alone. A depth image's own clear, `vkCmdClearDepthStencilImage`'s, covers all of it, so a
    pass of a smaller area draws it over the whole framebuffer first, as `vkCmdClearAttachments` draws.
  - A sampled depth image left to be read takes all of eDRAM's depth, which holds the image's depth around the area.
  - A pass's resolve clears colour for the next pass only when the next pass's area is inside its own, and depth only
    when the two areas are the same, as eDRAM holds a depth image's depth around an area.
  - Clearing part of the depth leaves hierarchical depth off till a pass clears all of it (xsdk:include/xsdk/gpu.h),
    so the draws after it skip no hidden pixels early.
- How an untiled pass maps (docs/ROADMAP.md, milestones 4, 25 and 27):
  - the pass draws into eDRAM, each colour attachment a colour target of its own in its format, one after another
    from tile 0 (`xsdk_gpu_set_targets`), and depth where eDRAM holds its image's, or the clear it wants at its size;
    a depth eDRAM doesn't hold goes ending at tile 2048, or just under a depth image eDRAM holds there when that stays
    above the pass's colour, so a pass with a smaller depth of its own leaves the earlier one alone, as a pass without
    depth does (milestone 14). Up to four depths are tracked, each lost when a pass's colour or depth reaches its
    tiles, and a depth below the end of a pass's colour targets is lost to them;
  - its end resolves each colour target's area into its attachment's image, converted to the image's format
    (`xsdk_gpu_resolve_target`, or `xsdk_gpu_resolve_texture_rect` for a smaller area);
  - the layer tracks what each of the last pass's colour targets holds, at its tile and in its format, and what each
    depth holds, and over which rectangle: values a resolve cleared it to, at a framebuffer's size, or an image's
    contents;
  - a clear finds its values there over its area, or beginning the pass resolves into the attachment to set them,
    copying junk the pass then overwrites, a resolve for each colour attachment that clears;
  - a load finds its image there over its area, or the layer draws the image back in, texel for texel, with its own
    shaders, into that target alone where the subpass has several;
  - a pass's resolve clears a colour target as the next pass in the command buffer clears the target at its tile, in
    its format and at its size, and keeps it otherwise; depth the same. The last pass in a command buffer clears to its
    own values, so the next frame's usually finds them;
  - a tiled pass's bands reach every tile but a whole depth's at the top, so after one eDRAM holds its last band,
    cleared by its resolves, which no untiled pass takes, and its whole depth: the layer keeps that depth and forgets
    the other colour targets and depths.

## Images

- 2D, one layer, one sample, optimal tiling, used for sampling, copying to and from and as colour attachments; cubes and
  3D images, below; anything else returns `VK_ERROR_FEATURE_NOT_PRESENT`. Mip levels down to 1x1. None is sparse, so
  `vkGetImageSparseMemoryRequirements` and `vkGetPhysicalDeviceSparseImageFormatProperties` find nothing.
- Formats: each on the xsdk format that holds its bits as Vulkan does (Formats, "On xsdk").
- Compressed images, BC1 to BC5, are optimal-tiled. Vulkan's blocks are the Khronos Data Format Specification's
  bytes, each 16-bit word least significant byte first, and xsdk's DXT formats take each word in the CPU's order
  (xsdk:include/xsdk/texture.h), so copies into and out of a compressed image swap each word's bytes, a row of blocks
  at a time (milestone 20). The image's memory holds xsdk's order, which is opaque to the app, as Vulkan has it.
- Cubes (milestone 23): `VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT`, six layers, square, one sample, optimal tiling, in the
  colour formats (Formats), compressed or not, but not D24S8, with mips, sampled and copied to and from. Layers 0 to 5
  are xsdk's faces, +x, -x, +y, -y, +z then -z, Vulkan's order too (xsdk:include/xsdk/texture.h,
  `xsdk_texture_init_cube`). A cube clamps to each face's edge whatever the sampler's address modes, as Vulkan has it
  (xenia:src/xenia/gpu/texture_util.cc:556-577).
- 3D images (milestone 23): one level and one layer, one sample, optimal tiling, in the colour formats, compressed or
  not, but not D24S8, sampled and copied to and from, up to `maxImageDimension3D`, 1024, on each side
  (xsdk:include/xsdk/texture.h, `xsdk_texture_init_3d`). A 3D image of more levels returns
  `VK_ERROR_FEATURE_NOT_PRESENT`, as xsdk lays out a 3D texture's base alone (xsdk:TODO.md, "Cube and 3D textures, the
  rest").
- Arrays of 2D images or of cubes, and cubes or 3D images used as attachments, return `VK_ERROR_FEATURE_NOT_PRESENT`.
  `vkGetPhysicalDeviceImageFormatProperties` gives a 2D image one layer, a cube six and 14 levels, and a 3D image 1024
  on each side and one level, so `maxImageArrayLayers` is a cube's 6.
- **Deviation:** a linear filter at a cube face's edge stays inside the face, where Vulkan blends in the face beside it:
  Xenos likely has no seamless cube filtering (xenia:src/xenia/gpu/ucode.h:592-593). Not checked on the console.
- Linear images: one level of an uncompressed format, only copied to and from, which they're for, as xsdk's linear
  textures are one level (xsdk:src/texture.c). Their rows start a whole 256 bytes apart, as Xenos's linear textures need
  (xenia:src/xenia/gpu/xenos.h:1179-1181), and `vkGetImageSubresourceLayout` gives that pitch, offset 0 and the rows'
  size; for an optimal image, which is invalid use, it gives zeros. `vkGetPhysicalDeviceImageFormatProperties` takes
  linear tiling for those usages alone, one level; the formats report no linear tiling features, which copies don't
  need.
- Colour attachment images are of the formats Formats lists as colour attachments, with one level, and both sides
  over 16 texels: a smaller base level sits in the packed mip tail, where a resolve can't write
  (xsdk:src/gpu_resolve.c). Others return `VK_ERROR_FEATURE_NOT_PRESENT`. Each draws into the xsdk target of its
  format and resolves into its texture (`xsdk_gpu_resolve_target`, milestone 25): `R8G8B8A8_UNORM` and
  `B8G8R8A8_UNORM` into 8_8_8_8, `A2B10G10R10_UNORM_PACK32` into 2_10_10_10, `R16G16_SFLOAT` into 16_16_FLOAT,
  `R16G16B16A16_SFLOAT` into 16_16_16_16_FLOAT and `R32_SFLOAT` into 32_FLOAT (xenia:src/xenia/gpu/xenos.h:299-313).
  Xenos's 7e3 float, 2_10_10_10_FLOAT, and its 16_16 and 16_16_16_16 fixed point from -32 to 32 have no Vulkan
  format, so they aren't offered.
- Optimal tiling is Xenos's tiled layout, with mips as xsdk milestones 15 and 16 settled.
- Memory requirements: the texture's size, 4 KB aligned, as a fetch constant holds its address in 4 KB units. Binding
  places the texels at the memory (`xsdk_texture_attach`).
- Depth images: `VK_FORMAT_D24_UNORM_S8_UINT`, one level, a depth attachment, sampled if they're made to be, and
  cleared by `vkCmdClearDepthStencilImage` if they're made a transfer destination. One that isn't sampled has a
  memory requirement of one page, which nothing uses. A sampled one is a tiled D24S8 texture (`XSDK_FORMAT_D24S8`),
  both sides over 16 texels as a resolve needs, and needs its texture's memory:
  - after a pass leaves it to be read (Render passes), each texel's word in its memory holds depth's 24 bits above
    stencil's 8, as the CPU reads it, at `tiled_index` (tests/tiled_index.h) of the texel and the 32-texel pitch.
    **Deviation:** Vulkan leaves an optimal-tiled image's memory opaque; a mapped read of it is how an app reads depth
    back without a copy that waits for the GPU;
  - sampled, it returns the depth from 0 to 1 in red, 0 in green and blue and 1 in alpha, whatever the view's aspect
    mask, filtered linearly or not, and compared through a comparison sampler (Samplers);
  - cleared by `vkCmdClearDepthStencilImage`, every word of its memory takes the clear on the CPU at replay, for a
    shader to sample, as well as the next pass to load it.
- Image views: the whole of an image in its format, a 2D image's as 2D, a cube's as a cube and a 3D image's as 3D; a 2D
  view of a face, or an array view, returns `VK_ERROR_FEATURE_NOT_PRESENT`. A component mapping sets the fetch
  constant's swizzle (`xsdk_texture_swizzle`), so a view samples any of the format's channels, 0 or 1 as each of red,
  green, blue and alpha, the format's own picked as Formats ("On xsdk") has them; a swizzled view of a depth image
  returns `VK_ERROR_FEATURE_NOT_PRESENT`.

## Samplers

- Nearest or linear filtering and mips; a LOD bias within 15.
- Repeat, mirrored repeat, clamp to edge and clamp to border, and mirror clamp to edge on a device made with
  `VK_KHR_sampler_mirror_clamp_to_edge`, which `vkCreateSampler` refuses without it: each is one of Xenos's modes,
  mirror clamp to edge its mirror once (xsdk:include/xsdk/texture.h). W's mode is a 3D image's, across its slices
  (milestone 23).
- Clamp to border in transparent black or opaque white, Xenos's two borders (xenia:src/xenia/gpu/xenos.h:150-156).
  **Deviation:** opaque black returns `VK_ERROR_FEATURE_NOT_PRESENT` where a mode reads the border. Xenos's black has
  an alpha no one has checked, which Xenia's backends take as 0
  (xenia:src/xenia/gpu/vulkan/vulkan_texture_cache.cc:817-824), so it stands for transparent black till
  vulkan4xenon_test's border check settles it on the console. The integer borders return it too, as no image format is
  an integer one.
- Anisotropy, on a device made with `samplerAnisotropy`: `maxAnisotropy` from 1 to 16, which xsdk's sampler rounds
  down to a power of two (xsdk:include/xsdk/texture.h). Without the feature, or past 16, it returns
  `VK_ERROR_FEATURE_NOT_PRESENT`.
- `minLod` and `maxLod` from 0 to 15, a `maxLod` past 15 keeping every level; past that, or a `maxLod` below the
  `minLod`, `VK_ERROR_FEATURE_NOT_PRESENT` (milestone 18).
- Comparison samplers (`compareEnable`), for D24S8 images sampled by a `sampler2DShadow` (milestone 23). xost fetches
  the depth and compares it in the shader with the reference, clamped to 0 to 1, by the compare op the layer writes
  into the shader's constants at each draw (xost:docs/xsc.md, "Interface conventions"); whether Xenos compares in the
  fetch itself hasn't been checked. A comparison sampler that filters linearly returns `VK_ERROR_FEATURE_NOT_PRESENT`,
  as comparing after the fetch would blend depths, not results. `samplerCubeShadow` and `textureProj`'s comparison
  aren't lowered (Shaders).
- Unnormalized coordinates return `VK_ERROR_FEATURE_NOT_PRESENT`; they're an xsdk TODO item.
- **Deviation:** Xenos clamps to whole levels (xenia:src/xenia/gpu/xenos.h:1265-1266), so `minLod` and `maxLod` go to
  the nearest; a fraction of a level past either isn't sampled.

## Descriptor sets

- Host memory: a descriptor a binding, read when a draw replays. Bindings run from 0 to 31, one descriptor each.
- Types: uniform buffers, dynamic uniform buffers and combined image samplers. Anything else, arrays and immutable
  samplers return `VK_ERROR_FEATURE_NOT_PRESENT`.
- A write of several descriptors fills consecutive bindings.
- Pools hold any number of sets; their sizes and limits are ignored.
- **How draws read them:** each draw puts every uniform block its shaders read into float constants, from the
  buffer at the descriptor's offset plus any dynamic offset, as section 2 of the roadmap decided.
  - The GPU loads a block straight from the buffer when its address is 64-byte aligned and the descriptor's range
    covers it, which buffers and offsets of 256 make the usual case (milestone 7).
  - Otherwise the CPU reads it, 16 bytes at a time where it can, and bytes past the descriptor's range read as 0.
    Mapped memory isn't cached, so each read goes to memory.
- Push constants are copied in the same way, from what `vkCmdPushConstants` recorded. A stage's push block writes only
  the registers its stage's ranges cover, from the lowest range's offset to the highest's end, as a stage may read only
  the push constants in its own ranges (milestone 29).

## Commands

- Each command buffer starts with room for 4096 commands, and 16 KB of data for push constants, dynamic offsets and
  copy regions; each doubles when it fills, and keeps its size through resets. If host memory runs out,
  `vkEndCommandBuffer` returns `VK_ERROR_OUT_OF_HOST_MEMORY`.
- `vkQueueSubmit` replays them on the caller's thread into xsdk's ring.
- Secondary command buffers record the same commands; `vkCmdExecuteCommands` replays each in place, at the submit, with
  the state the replay has bound, which is more than Vulkan promises a secondary command buffer, as it begins with
  none (milestone 19).
- **Copies, clears and the blits the GPU doesn't draw run on the CPU at replay** (src/transfer.c), in order with the
  draws around them, each waiting first for the GPU to be done with what it reads or writes:
  - an image: the submission that last sampled it, resolved into it or drew it back into eDRAM, for the fence that
    submission ended with; earlier in its own, for the GPU to finish what came before. An image no submission used
    isn't waited for (milestone 12);
  - a buffer it writes: every draw since the CPU last waited for the GPU, as draws read vertex, index and uniform
    buffers. **Shortcut:** a copy into a buffer after a draw waits for the GPU to finish all of it, a millisecond at
    least, as `xsdk_gpu_wait` looks between 1 ms sleeps (xsdk:include/xsdk/gpu.h); keeping each memory's last use, as
    images keep theirs, would cut that if a port's copies stall on it.

  What they write the GPU reads afresh: the range goes out of its vertex and texture caches, and an image written is
  no longer what eDRAM holds. Copies of depth images return `VK_ERROR_FEATURE_NOT_PRESENT`.
- `vkCmdCopyBufferToImage` and `vkCmdCopyImageToBuffer` copy whole mip levels, to or from tightly packed rows or rows
  `bufferRowLength` texels long, at 4-byte-aligned offsets, in runs of whole words where the rows are word-aligned
  (`xsdk_texture_upload_slice_rows`, `xsdk_texture_download_slice_rows`). A compressed level goes a row of blocks at a
  time, each 16-bit word's bytes swapped (Images). A copy out reads the GPU's write-combined memory, so it's slow. A
  region may cover any of a cube's layers or a 3D image's slices, each one's rows `bufferImageHeight` rows, or the
  level's height, after the last's (milestone 23).
- `vkCmdCopyImage` copies whole levels of one size between formats of one texel size, the bits as they are, a row at a
  time, as many of a cube's layers or a 3D image's slices as the other gives. A copy between a compressed and an
  uncompressed format, which Vulkan allows, fails the command buffer, as their 16-bit words lie in different orders.
- **`vkCmdBlitImage` drawn on the GPU** (milestone 27), so the replay doesn't wait for it: between two 2D images of
  `VK_FORMAT_R8G8B8A8_UNORM` or `B8G8R8A8_UNORM`, one format both, from level 0 of an optimal source to level 0 of
  another optimal image, over 16 texels a side and within eDRAM's 2048 tiles, its destination region any on the
  resolve's 8-pixel grid, as a render area is (Render passes), nearest or linear. xsdk resolves a rectangle of an 8-bit
  target alone (xsdk:include/xsdk/gpu.h), so the other formats' blits stay on the CPU.

  The layer draws the source region over the destination's rectangle with its own shaders, into eDRAM at the
  destination's size, sampling level 0 alone, edges clamped, mirrored as either region's corners are; then it
  resolves that rectangle into the destination, clearing nothing. eDRAM's colour then
  holds the destination over the rectangle, and the colour targets and depths its target reached are lost, as a pass's
  colour loses them. Linear filtering is Xenos's own, whose precision isn't measured (xsdk:TODO.md, "Linear filtering's
  precision").
- Any other `vkCmdBlitImage` runs on the CPU, as milestones 20 and 23 have it: whole levels to whole levels, unflipped,
  between images of one format reporting `VK_FORMAT_FEATURE_BLIT_SRC_BIT` and `BLIT_DST_BIT` (Formats): every
  uncompressed format but `R32_SFLOAT`, the half floats and 10-10-10-2 nearest alone, as they don't report linear
  filtering. Each destination texel samples the source at its centre, nearest or between the four texels around it,
  edges clamped: the CPU unpacks each texel's channels to floats, an sRGB format's colours through sRGB's curve, mixes
  them, and packs them again rounded to the nearest (src/format.c). A chain of levels blitted down from the base makes
  an image's mips, a cube's a layer at a time. Each layer or 3D slice goes into one of its own; a blit that scales a 3D
  image's depth fails the command buffer, as does a blit neither path takes, such as one of part of a level the GPU
  doesn't draw.
- `vkCmdClearColorImage` fills whole levels with the colour, packed as the format holds it, an sRGB format's colours
  through sRGB's curve, each channel rounded to the nearest; the range's layers of a cube, or every slice of a 3D
  image. A compressed image's clear, which Vulkan doesn't allow, fails the command buffer.
- `vkCmdClearDepthStencilImage` clears depth and stencil together, as one register holds both; a range of one alone
  fails the command buffer. Its clear waits for the next pass to use the image (Render passes), and a sampled image's
  memory takes it at once (Images).
- `vkCmdClearAttachments` draws each rectangle with the layer's own shaders: the scissor at the rectangle, a viewport
  whose depth range is the clear's depth alone, so every pixel takes it, depth and stencil always passing, the
  stencil replaced by the clear's, and each colour attachment, depth and stencil written only if it's cleared; a
  subpass of several colour attachments takes a shader writing four colours, one for each (milestone 25). The pass's
  pipeline, viewport, scissor and state are set again for the draw after. Rectangles of one layer; more fail the
  command buffer.
- `vkCmdCopyBuffer` copies bytes at any offsets; `vkCmdUpdateBuffer` copies the bytes it recorded; `vkCmdFillBuffer`
  writes its word in the CPU's byte order, as the spec has it, a store a word.
- `vkCmdPipelineBarrier` records nothing: replay is in order, and copies wait for what used what they copy. One that
  would have a sampled depth image read from an attachment fails the command buffer (Render passes).
- `vkCmdDraw` and `vkCmdDrawIndexed` draw any number of instances from any `firstInstance` (milestone 24; xsdk
  milestone 47, xsdk:docs/HARDWARE.md, "Instancing on the console, milestone 47 step 1" and "Instanced draws in
  xsdk, milestone 47 step 3"). A pipeline is instanced if a binding is at the instance's rate or its vertex shader
  reads `gl_InstanceIndex` (Shaders), and an instanced pipeline's list draws its instances together:
  - `vkCmdDraw` of a list, through `xsdk_gpu_draw_instanced`: its vertex count taken down to whole primitives, as
    Vulkan draws no partial one, in pieces of whole instances up to 65,535 vertices, one packet and one group a piece.
  - `vkCmdDrawIndexed` of a list of more than one instance, through `xsdk_gpu_draw_indexed_instanced`: the app's
    indices from `firstIndex`, read from its mapped buffer 16 bytes a load where aligned, are repeated in up to 64
    copies, each moved on by the power of two above their largest (`xsdk_gpu_repeat_indices`), into GPU memory the
    queue fills in four parts of 256 KB, each reused once the submission that last wrote it is done; then a packet and
    a group a piece of as many instances as there are copies. **Shortcut:** each such draw reads the app's indices and
    writes its copies again, and drops the copies' range from the GPU's caches; a draw that finds every part written
    by its own submission draws an instance at a time, as a tiled pass's recording can't wait for itself. Keep the
    copies across draws if a port's draws spend their time there.
  - Strips, fans, instances past 65,535 vertices or indices, an indexed draw of one instance, and pipelines that aren't
    instanced draw an instance at a time, in the pieces below, an instanced pipeline's group written for each, so its
    prologue keeps the vertex's index and takes the instance. **Shortcut:** such an instance costs a draw packet and
    a group, and a constant written waits for the draw before it (xsdk:TODO.md, "Constants written between draws hold
    the GPU"); strips and fans in one draw would need a restart or degenerate triangles an instance
    (xsdk:docs/scope/47-instancing.md, "Not in it").
  - `firstVertex` and `vertexOffset` move the address of each stream at the vertex's rate, since xsdk draws from
    vertex 0.
  - `firstIndex` moves the index buffer's address. Indices are 16 or 32 bits, with values up to 2^24 - 1.
  - A draw of more than 65535 vertices or indices goes in pieces, as the draw packets count in 16 bits
    (xenia:src/xenia/gpu/registers.h:327): lists split on whole primitives; a line strip's pieces share the vertex
    where they meet, and a triangle strip's the two, each piece an even number of vertices on from the last, so every
    triangle keeps the parity its facing goes by (milestone 24). A triangle fan that long returns
    `VK_ERROR_FEATURE_NOT_PRESENT`: each piece would need the fan's first vertex again before its own, which only
    indices made at replay could give it.
- `vkCmdDrawIndirect` and `vkCmdDrawIndexedIndirect`: one draw each, as there's no `multiDrawIndirect` and
  `maxDrawIndirectCount` is 1; more fail the command buffer. The parameters are read at replay, in order with it:
  the host wrote them before the submit, or a copy earlier in the replay did, as nothing else writes buffers. They
  take any instance count and first instance, as `drawIndirectFirstInstance` promises, and draw as a direct draw of
  what they read does, an instanced list in one draw too. **Deviation:** what a direct draw refuses as it's recorded,
  an indirect one can only refuse as it's replayed, after the command buffer ended: a triangle fan past 65535. It logs
  that and draws nothing, as does a direct draw recorded where its pipeline wasn't bound, in a secondary command
  buffer.

## Sync

- Fences are xsdk's fences: `vkQueueSubmit` always ends with one, and a `VkFence` passed to it signals when the GPU
  passes it.
- Semaphores do nothing: there's one queue, and it replays in order.
- An event is a word in GPU memory, 1 when set, which the host and the GPU both write and the GPU can wait on
  (milestone 19). `vkSetEvent` writes it after draining the CPU's write-combining, so a GPU waiting on it sees what the
  host wrote before. **Shortcut:** each event takes a 64 KB page of its own, as `xsdk_gpu_alloc` hands out no less.
- `vkCmdSetEvent` and `vkCmdResetEvent` write it once the GPU is done with the work before, whatever the stage mask
  (`xsdk_gpu_write_word`).
- `vkCmdWaitEvents` has the command processor wait on each event's word (`xsdk_gpu_wait_word`), then takes the whole of
  memory out of the GPU's caches, for what the host wrote before setting it; its barriers are a pipeline barrier's.
  **Shortcut:** an event the host sets after the submit holds the GPU's work alone. What the replay does on the CPU at
  `vkQueueSubmit` (copies, indirect draws' parameters, uniform buffers the GPU can't load) doesn't wait for it, so it
  sees only what the host wrote before the submit. A GPU waiting on an event no one sets waits for good, as Vulkan
  allows.
- `vkWaitForFences` with `waitAll` false waits on the earliest pending fence, since fences signal in submission order.
  A fence never submitted never signals, so waiting on it returns `VK_TIMEOUT` at once.

## Queries

- Time stamp and occlusion queries (milestones 19 and 28). Pipeline statistics need a feature that isn't reported, so
  `vkCreateQueryPool` refuses them.
- A time stamp query's slot in GPU memory takes the GPU's clock (`xsdk_gpu_write_clock`): 64 bits, from 0 when the
  device takes the GPU, 498.75 ticks a microsecond, so `timestampPeriod` is 2.005 ns and the queue family's
  `timestampValidBits` 64 (xsdk:docs/HARDWARE.md, "Queries and events").
- `vkCmdWriteTimestamp` at `VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT` copies the clock as the command processor reaches it:
  once the draws before it are issued, which may still be running, but only once a render pass before it has ended, as
  the command processor waits for the pass's resolve. At any other stage it copies the clock once the GPU is idle, the
  latest any stage finishes.
- **Deviation: occlusion queries don't count samples.** A query is 1 if any draw between `vkCmdBeginQuery` and
  `vkCmdEndQuery` rasterized anything, and 0 if none did, so a draw hidden behind others everywhere still makes it 1.
  It's D3D's visibility query (`xsdk_gpu_begin_visibility`), the one answer Xenos gives: every try at its sample counts
  hung the GPU (xsdk:docs/HARDWARE.md, "Occlusion queries, vulkan4xenon milestone 28"). Vulkan lets a query without
  `VK_QUERY_CONTROL_PRECISE_BIT` return any non-zero value where samples passed, but 0 where none did, which the hidden
  draw's 1 breaks. `occlusionQueryPrecise` isn't reported, and the precise bit is refused (milestone 28).
- An occlusion query begins and ends inside one subpass, as Vulkan asks of one begun in a render pass; one begun outside
  a render pass is refused, as the layer's resolves and blits there would count. Its draws go to visibility id 0, and
  each `vkCmdClearAttachments` inside it ends that id and goes on under the next, so the clear's own draw doesn't count.
  **Shortcut:** past a query's 31st clear, its clears count, its ids used up. `inheritedQueries` isn't reported.
- A query's end waits for the GPU to be idle, then copies the status word holding its bits into its slot, and the last
  id it took after it, which makes it available (`xsdk_gpu_end_visibility`). So each query costs a wait for the GPU, and
  a submission takes any number of them, each from id 0 again.
- The console hasn't run occlusion queries yet. It settles whether an id begun again around draws that rasterize nothing
  comes back 0, which every query after the first relies on (docs/ROADMAP.md, milestone 28).
- For both kinds, `vkCmdResetQueryPool` marks the slots unwritten on the CPU at replay, after waiting for the submission
  that last wrote the pool, as the copies do (Commands). A query is available once its slot is written.
- `vkCmdCopyQueryPoolResults` runs on the CPU at replay too: it waits for the pool's last submission and the draws
  before, so every query written before it is available and `VK_QUERY_RESULT_WAIT_BIT` adds nothing.
- `vkGetQueryPoolResults` with `VK_QUERY_RESULT_WAIT_BIT` waits for the fence of the submission that last wrote the
  pool; a query still unwritten then returns `VK_NOT_READY`, rather than waiting for good.

## Display and swapchain

- One display with one plane and one mode: 1280x720, which the console scales to its output. The mode's refresh rate
  is the output's (`VdQueryVideoMode`). `vkCreateDisplayModeKHR` returns `VK_ERROR_INITIALIZATION_FAILED`.
- Surfaces must be full-screen, opaque and untransformed.
- Surface formats: `VK_FORMAT_B8G8R8A8_UNORM` in `VK_COLOR_SPACE_SRGB_NONLINEAR_KHR`; a resolve stores B, G, R, A in
  byte order, as the display reads them (xsdk:docs/HARDWARE.md). Present mode: FIFO.
- One swapchain at a time, of 2 or 3 images: `xsdk_present`'s front buffers, colour attachments only. A second, or
  `oldSwapchain`, returns `VK_ERROR_NATIVE_WINDOW_IN_USE_KHR`.
- `vkAcquireNextImageKHR` waits on the CPU for the next buffer to leave the screen, so its semaphore and fence are
  signalled when it returns.
- **Deviation:** one image can be acquired at a time. A second acquire before presenting returns `VK_NOT_READY` with
  a zero timeout and `VK_TIMEOUT` otherwise, though with 3 images Vulkan allows holding 2.
- `vkQueuePresentKHR` queues the acquired image; the vblank callback shows it once the GPU has finished it. Presenting
  any other image returns `VK_ERROR_OUT_OF_DATE_KHR`.
- Without a display controller, as in Xenia, `vkCreateSwapchainKHR` returns `VK_ERROR_INITIALIZATION_FAILED`.

## Boot logo

- `vkCreateDevice` fades the vulkan4xenon logo in over half a second (`xsdk_splash_begin`) and leaves it up.
- The swapchain takes the display over without showing anything of its own, so the logo stays until the first
  present flips. Its buffers are freed a vblank after that flip.
- Without a 1280x720 display, the device works without the logo.

## Formats

| Format | Features |
|---|---|
| `VK_FORMAT_R8G8B8A8_UNORM`, `VK_FORMAT_B8G8R8A8_UNORM` | optimal tiling: sampled, linear filtering, colour attachment and blending, blit source and destination; buffer: vertex buffer (milestone 20) |
| `VK_FORMAT_D24_UNORM_S8_UINT` | optimal tiling: depth and stencil attachment; sampled, its depth in red, with linear filtering, which vulkan4xenon_test's shadow check tries on the console (milestone 23) |
| `VK_FORMAT_R32_SFLOAT` | optimal tiling: sampled, nearest filtering only, as filtering 32-bit floats hasn't been tried on the console; colour attachment, unblended, as Xenos's float targets don't blend (milestone 25); buffer: vertex buffer |
| `VK_FORMAT_R8_UNORM`, `VK_FORMAT_R8G8_UNORM` | optimal tiling: sampled, linear filtering, blit source and destination |
| `VK_FORMAT_R5G6B5_UNORM_PACK16`, `B5G6R5_UNORM_PACK16`, `A1R5G5B5_UNORM_PACK16`, `R4G4B4A4_UNORM_PACK16`, `B4G4R4A4_UNORM_PACK16`, `R16_UNORM`, `R16_SNORM`, `R8_SNORM`, `R8G8_SNORM`, `B8G8R8A8_SNORM`, `R8_SRGB`, `R8G8_SRGB`, `R8G8B8A8_SRGB`, `B8G8R8A8_SRGB` | optimal tiling: sampled, linear filtering, blit source and destination (milestone 20) |
| `VK_FORMAT_R16G16_UNORM`, `R16G16B16A16_UNORM`, `R16G16_SNORM`, `R16G16B16A16_SNORM`, `R8G8B8A8_SNORM` | optimal tiling: sampled, linear filtering, blit source and destination; buffer: vertex buffer (milestone 20) |
| `VK_FORMAT_A2R10G10B10_UNORM_PACK32`, `A2B10G10R10_SNORM_PACK32`, `A2R10G10B10_SNORM_PACK32`, `R16_SFLOAT` | optimal tiling: sampled, nearest filtering only, as Xenos filtered them wrong on the console, far from the mean of two texels halfway between them (milestone 20); blit source and destination |
| `VK_FORMAT_R16G16_SFLOAT`, `R16G16B16A16_SFLOAT` | optimal tiling: sampled, nearest filtering only, as above; blit source and destination; buffer: vertex buffer (milestone 20); colour attachment, unblended, as Xenos's float targets don't blend (milestone 25) |
| `VK_FORMAT_A2B10G10R10_UNORM_PACK32` | optimal tiling: sampled, nearest filtering only, as the other 10-10-10-2 formats; blit source and destination (milestone 20); colour attachment and blending, as fixed point blends like 8-bit channels (xenia:src/xenia/gpu/render_target_cache.h:122-129; milestone 25) |
| `VK_FORMAT_BC1_RGB_UNORM_BLOCK`, `BC1_RGBA_UNORM_BLOCK`, `BC2_UNORM_BLOCK`, `BC3_UNORM_BLOCK`, `BC4_UNORM_BLOCK`, `BC5_UNORM_BLOCK`, `BC1_RGB_SRGB_BLOCK`, `BC1_RGBA_SRGB_BLOCK`, `BC2_SRGB_BLOCK`, `BC3_SRGB_BLOCK` | optimal tiling: sampled, linear filtering (milestone 20) |
| `VK_FORMAT_R32G32_SFLOAT`, `R32G32B32_SFLOAT`, `R32G32B32A32_SFLOAT`, `A8B8G8R8_UNORM_PACK32` | buffer: vertex buffer |
| `VK_FORMAT_A8B8G8R8_SNORM_PACK32`, `A8B8G8R8_USCALED_PACK32`, `A8B8G8R8_SSCALED_PACK32`, `R8G8B8A8_USCALED`, `R8G8B8A8_SSCALED`, `R16G16_USCALED`, `R16G16_SSCALED`, `R16G16B16A16_USCALED`, `R16G16B16A16_SSCALED` | buffer: vertex buffer (milestone 20) |

**On xsdk:** each image format is the xsdk format that holds its bits as Vulkan does: a packed unit in the CPU's byte
order, as xsdk's uploads take it and as Vulkan has a packed format's unit on a big-endian host, each channel where
xsdk's own format puts it (xsdk:docs/HARDWARE.md, "Texture formats, milestone 16"). Where Vulkan orders the channels
otherwise, the fetch constant's swizzle picks them, which reads the same bits (src/format.c):

| Vulkan | xsdk | Vulkan's red, green, blue and alpha are xsdk's |
|---|---|---|
| `R8G8B8A8_UNORM`, `B8G8R8A8_UNORM`, `R8_UNORM`, `R8G8_UNORM`, `R32_SFLOAT` | `XSDK_FORMAT_R8G8B8A8`, `B8G8R8A8`, `R8`, `R8G8`, `R32F` | the same |
| `R5G6B5_UNORM_PACK16`; `B5G6R5_UNORM_PACK16` | `XSDK_FORMAT_R5G6B5` | the same; blue, green, red, alpha |
| `A1R5G5B5_UNORM_PACK16` | `XSDK_FORMAT_A1R5G5B5` | the same |
| `R4G4B4A4_UNORM_PACK16`; `B4G4R4A4_UNORM_PACK16` | `XSDK_FORMAT_A4R4G4B4` | alpha, red, green, blue; green, red, alpha, blue |
| `A2B10G10R10_UNORM_PACK32`; `A2R10G10B10_UNORM_PACK32` | `XSDK_FORMAT_A2B10G10R10` | the same; blue, green, red, alpha |
| `R16_SFLOAT`, `R16G16_SFLOAT`, `R16G16B16A16_SFLOAT` | `XSDK_FORMAT_R16F`, `R16G16F`, `R16G16B16A16F` | the same |
| `BC1_RGB_UNORM_BLOCK`; `BC1_RGBA_UNORM_BLOCK` | `XSDK_FORMAT_DXT1` | red, green, blue and 1, as BC1 without alpha reads 1 where DXT1 has transparent black; the same |
| `BC2_UNORM_BLOCK`, `BC3_UNORM_BLOCK` | `XSDK_FORMAT_DXT3`, `DXT5` | the same |
| `BC4_UNORM_BLOCK`, `BC5_UNORM_BLOCK` | `XSDK_FORMAT_DXT5A`, `DXN` | the same |
| `R16_UNORM`, `R16G16_UNORM`, `R16G16B16A16_UNORM`, and each one's `SNORM` | `XSDK_FORMAT_R16`, `R16G16`, `R16G16B16A16` | the same |
| `R8_SNORM`, `R8G8_SNORM`, `R8G8B8A8_SNORM`, `B8G8R8A8_SNORM`; and each one's `SRGB` | `XSDK_FORMAT_R8`, `R8G8`, `R8G8B8A8`, `B8G8R8A8` | the same |
| `A2B10G10R10_SNORM_PACK32`; `A2R10G10B10_SNORM_PACK32` | `XSDK_FORMAT_A2B10G10R10` | the same; blue, green, red, alpha |
| `BC1_RGB_SRGB_BLOCK`; `BC1_RGBA_SRGB_BLOCK`, `BC2_SRGB_BLOCK`, `BC3_SRGB_BLOCK` | `XSDK_FORMAT_DXT1`, `DXT3`, `DXT5` | red, green, blue and 1; the same |

A signed format reads every channel signed (`xsdk_texture_signs`), two's complement from -1 to 1, the most negative
read as -1: Vulkan's `SNORM`, and Xenos's only signed mode (xenia:src/xenia/gpu/xenos.h:761-767, 1197-1201). An sRGB
format reads its colours through Xenos's gamma, and its alpha as it is (Deviations).

Left out, as no Xenos format holds their bits as Vulkan has them:
- `R5G5B5A1_UNORM_PACK16` and `B5G5R5A1_UNORM_PACK16`, whose alpha is the lowest bit, where Xenos's 1-5-5-5 has it
  highest (xenia:src/xenia/gpu/shaders/pixel_formats.xesli, `XeR5G5B5A1ToB5G5R5A1`);
- `B10G11R11_UFLOAT_PACK32`, as Xenos's 10-11-11 and 11-11-10 are fixed point
  (xenia:src/xenia/gpu/shaders/pixel_formats.xesli:450-484), which xsdk has as `XSDK_FORMAT_B10G11R11` and
  `B11G11R10`;
- `BC4_SNORM_BLOCK` and `BC5_SNORM_BLOCK`: Xenia reads DXT5A and DXN unsigned alone
  (xenia:src/xenia/gpu/vulkan/vulkan_texture_cache.cc:333-337, 384-388), so nothing shows how Xenos signs them.

Each vertex format is the xsdk vertex format of its components' size, number and count (xsdk:include/xsdk/shader.h):
xsdk fetches 32-bit words, each component after the first in the bits above it, as xsdk milestone 17 settled, and a
stream's words are swapped 8 in 32 (xsdk:src/gpu_draw.c), so a packed word reads in order. Vulkan's other vertex
formats lay their components out by address, each in the CPU's byte order, so in each word the bytes or the halves
read in reverse, and the vertex element picks them back (`XsdkVertexElement`'s `components`, which the fetch's
destination swizzle takes; xenia:src/xenia/gpu/ucode.h:677-688):

| Vulkan | xsdk | x, y, z and w read xsdk's |
|---|---|---|
| `R32_SFLOAT` to `R32G32B32A32_SFLOAT` | `XSDK_VERTEX_FLOAT1` to `FLOAT4` | the same |
| `A8B8G8R8_UNORM_PACK32`, `SNORM_PACK32`, `USCALED_PACK32`, `SSCALED_PACK32` | `XSDK_VERTEX_UBYTE4N`, `BYTE4N`, `UBYTE4`, `BYTE4` | the same |
| `R8G8B8A8_UNORM`, `SNORM`, `USCALED`, `SSCALED` | `XSDK_VERTEX_UBYTE4N`, `BYTE4N`, `UBYTE4`, `BYTE4` | w, z, y, x |
| `B8G8R8A8_UNORM` | `XSDK_VERTEX_UBYTE4N` | y, z, w, x |
| `R16G16_UNORM`, `SNORM`, `USCALED`, `SSCALED`, `SFLOAT` | `XSDK_VERTEX_USHORT2N`, `SHORT2N`, `USHORT2`, `SHORT2`, `HALF2` | y, x |
| `R16G16B16A16_UNORM`, `SNORM`, `USCALED`, `SSCALED`, `SFLOAT` | `XSDK_VERTEX_USHORT4N`, `SHORT4N`, `USHORT4`, `SHORT4`, `HALF4` | y, x, w, z |

Signed normalized components read as Vulkan has them, the most negative and the next both -1, as xsdk's fetch takes
them (xenia:src/xenia/gpu/xenos.h:761-767). Left out: the `_UINT` and `_SINT` formats, as xost's shaders take floats
alone, no integer reaching the GPU (xost:docs/xsc.md:239); formats of 1, 2, 3 or 6 bytes, such as `R8G8_UNORM` and
`R16G16B16_SFLOAT`, as xsdk fetches whole words; and the 10-bit types, which xsdk lacks.

**Deviations:**
- `textureCompressionBC` is false, as it promises BC6H and BC7 too, which Xenos lacks, and BC4 and BC5 `SNORM`
  (xenia:src/xenia/gpu/xenos.h:450-532). Vulkan has an app that doesn't find the feature read each BC format's features
  from `vkGetPhysicalDeviceFormatProperties`, which reports them (Vulkan spec, `VkPhysicalDeviceFeatures`). Vulkan also
  requires one of the BC, ETC2 and ASTC LDR suites in full; the layer has none in full.
- BC2 and BC3 colour blocks whose first endpoint isn't above the second take four colours, as Vulkan has them, and as
  Xenos decoded them on the console (milestone 20), not as DXT1's three and transparent black.
- sRGB images sample through Xenos's gamma, a piecewise-linear curve near sRGB's (xenia:src/xenia/gpu/xenos.cc:27): at
  128 it gives 0.251, where sRGB's gives 0.216. Blits and clears of them, on the CPU, take sRGB's own curve, as Vulkan
  has them, so what they write samples through Xenos's too. Xenos linearises a texel before filtering, as Vulkan
  does, not after, as Xenia does (xenia:src/xenia/gpu/dxbc_shader_translator_fetch.cc:2076-2078): on the console,
  filtered sRGB images blended their texels after the curve (milestone 20). sRGB images aren't colour attachments
  (milestone 25).
- The half floats and 10-10-10-2 sample nearest alone: Xenos filtered them wrong on the console, far from the mean of
  two texels halfway between them, though it sampled them right (milestone 20; xsdk:docs/HARDWARE.md, "Texture and
  vertex formats, vulkan4xenon milestone 20").

Vulkan 1.0 requires many more formats and features; each missing one is a deviation.

## Physical device

Only these limits are set. Every other limit is 0: what it measures isn't implemented yet, such as 1D images, storage
and texel buffers, input attachments, cull distances, and compute. Where Vulkan 1.0 has a minimum for one of those, the
0 is below it. `mipmapPrecisionBits` and `subTexelPrecisionBits` are 0 too, under the minimum of 4: filtering precision
hasn't been measured (xsdk:TODO.md).

| Limit | Value |
|---|---|
| `maxImageDimension2D`, `maxImageDimensionCube` | 8192 |
| `maxImageDimension3D` | 1024 |
| `maxImageArrayLayers` | 6 |
| `maxUniformBufferRange` | 4032 |
| `maxPushConstantsSize` | 128 |
| `maxMemoryAllocationCount` | 4096 |
| `maxSamplerAllocationCount` | 4000 |
| `bufferImageGranularity` | 1 |
| `maxDrawIndexedIndexValue` | 2^24 - 1 |
| `maxDrawIndirectCount` | 1 |
| `maxBoundDescriptorSets` | 4 |
| `maxPerStageDescriptorSamplers`, `maxPerStageDescriptorSampledImages` | 16 |
| `maxPerStageDescriptorUniformBuffers` | 12 |
| `maxPerStageResources` | 28 |
| `maxDescriptorSetSamplers`, `maxDescriptorSetSampledImages` | 96 |
| `maxDescriptorSetUniformBuffers` | 72 |
| `maxDescriptorSetUniformBuffersDynamic` | 8 |
| `maxVertexInputAttributes`, `maxVertexInputBindings` | 16 |
| `maxVertexInputAttributeOffset` | 1016 |
| `maxVertexInputBindingStride` | 1020 |
| `maxVertexOutputComponents`, `maxFragmentInputComponents` | 64 |
| `maxFragmentOutputAttachments`, `maxColorAttachments`, `maxFragmentCombinedOutputResources` | 4 |
| `maxViewports` | 1 |
| `maxViewportDimensions` | 4096, 4096 |
| `viewportBoundsRange` | -8192, 8191 |
| `subPixelPrecisionBits` | 4 |
| `maxSamplerLodBias` | 15 |
| `maxSamplerAnisotropy` | 16 |
| `minMemoryMapAlignment` | 65536 |
| `minTexelBufferOffsetAlignment`, `minUniformBufferOffsetAlignment`, `minStorageBufferOffsetAlignment` | 256 |
| `maxFramebufferWidth`, `maxFramebufferHeight` | 4096 |
| `maxFramebufferLayers` | 1 |
| `framebufferColorSampleCounts`, `framebufferDepthSampleCounts`, `framebufferStencilSampleCounts`, `sampledImageColorSampleCounts` | 1 |
| `discreteQueuePriorities` | 2 |
| `maxClipDistances`, `maxCombinedClipAndCullDistances` | 6 |
| `pointSizeRange` | 0, 8191.875 |
| `pointSizeGranularity` | 0.125 |
| `lineWidthRange` | 1, 1 |
| `standardSampleLocations` | true |
| `optimalBufferCopyOffsetAlignment`, `optimalBufferCopyRowPitchAlignment` | 1 |
| `nonCoherentAtomSize` | 128 |
| `timestampComputeAndGraphics` | true |
| `timestampPeriod` | 2.005 ns |

Set limits below Vulkan 1.0's minimum:

| Limit | Why |
|---|---|
| `maxVertexInputAttributeOffset` 1016, `maxVertexInputBindingStride` 1020 (2047, 2048) | A fetch's stride is 8 bits of dwords (xsdk:src/shader.c). |
| `maxFramebufferLayers` 1 (256) | No layered rendering. |
| `maxImageArrayLayers` 6 (256) | A cube's six layers; arrays of images aren't built yet. |
| `maxUniformBufferRange` 4032 (16384) | A block is copied into a stage's float constants of 16 bytes, a fragment shader's 252 below the 4 xsdk keeps (Shaders). |
| `maxPerStageResources` 28 (128) | 12 uniform buffers and 16 samplers: a stage's constants, and the fetch constants the two stages share with the vertex streams (Pipelines). |
| `maxClipDistances`, `maxCombinedClipAndCullDistances` 6 (8) | Xenos clips against six planes (xenia:src/xenia/gpu/registers.h:539-544). |

`subPixelPrecisionBits` is 4 because vertices land on 1/16 pixel under xsdk's `PA_SU_VTX_CNTL`
(xenia:src/xenia/gpu/xenos.h:877-884). The device is `Xenos (vulkan4xenon)`, an integrated GPU, vendor and device ID
0.
