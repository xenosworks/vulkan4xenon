# vulkan4xenon roadmap (agreed 2026-10-11)

**Goal.** A Vulkan 1.0 subset for the Xbox 360: a static library on the xsdk GPU layer, for ports that run on the
retail kernel. The goal it's measured against is **Dusklight**, a reimplementation of Twilight Princess, running on
the console through it.

**Status.** Agreed on 2026-10-11. Milestones 1 to 7, 9 to 15 and 17 to 31 are done, 18 with its fourth step put off
and 29 closed short of its targets; milestone 8 was tried and reverted. Milestone 34, the FIFO worker, closed short of
its target, and milestone 16, Dusklight's menus, shipped in Dusklight's beta 1 without being closed; both are pushed.
Milestone 33, graphics options, is agreed.
Section 5 has the Dusklight track's order, agreed the same day; section 7 lists what's still open.

**Constraints.** xsdk's, unchanged:
- No Microsoft tooling, no leaked SDK material, no OpenXeChain.
- Every claim about Xenos cites Xenia, libxenon, xost or a hardware result.
- The console decides: a milestone is done only on a clean hardware run. Xenia is for smoke checks.

**Sources.**
- **xsdk** (Z:\Xbox-360-SDK), cited as `xsdk:<path>`. What the console has settled is in `xsdk:docs/HARDWARE.md`.
- **Dusklight** (TwilitRealm/dusklight at `2c4960d`), cited as `dusklight:<path>:<line>`. The port's fork is
  Z:\dusklight-xenon, from that commit.
- **encounter's Aurora** (encounter/aurora at `3227d76`, the commit Dusklight pins), cited as `aurora:<path>:<line>`.
  This is the GameCube and Wii compatibility layer Dusklight is built on, not the Aurora dashboard titles launch from.
- **Aurora's fork** (xenosworks/aurora-xenon, branch `xenon`, on `3227d76`), with the backend interface, the null
  backend and the vulkan4xenon backend, cited as `aurora-xenon:<path>`.
- **Aurora for Melee** (xenosworks/aurora-xenon, branch `melee-xenon`: encounter's Aurora at `40cf48f`, melee-pc's
  vendored Aurora and the `xenon` branch merged), where milestone 18 is built, cited as `aurora-melee:<path>`.
- **xost** (xsdk's `xenos-toolchain/`), cited as `xost:<path>:<line>`.

## 1. How the layer is built

- **A static library.**
  - Apps call `vk*` directly; `vkGetInstanceProcAddr` and `vkGetDeviceProcAddr` work too.
  - It reports Vulkan 1.0, refuses 1.1 and up, and claims no conformance.
- **Khronos headers, vendored** in `third_party/vulkan-headers/`. They're Apache-2.0.
- **docs/SUPPORT.md** is the one list of what exists, every limit below the spec's minimum, and every deviation, each
  with a reason. It changes in the same commit as the code (CODE_GUIDELINES.md).
- **Milestones written one way:**
  - **Done when:** criteria a hardware run can check.
  - **Passed on hardware** on a date, at a commit, with the results' folder.
  - **Decisions**, **How it maps onto Xenos**, **New functions**, **New tests** and **Not in milestone N**.
- **Feature tests** in one title, each checking pixels of the presented image. They're broken on purpose before a
  hardware run, as xsdk's are.
- **A benchmark against the raw GPU layer,** xsdk's, scene for scene.
- **The boot logo:** the vulkan4xenon logo shows from `vkCreateDevice` until the app's first present, fading in from
  black. The image and its generator are xsdk's (`xsdk:assets/vulkan4xenon.png`,
  `xsdk:tools/gen_splash.py`). The Vulkan logo is a registered trademark of the Khronos Group, used at the user's
  choice.
- **Rules of thumb:**
  - Where the GPU's own ordering isn't proven, wait on the CPU.
  - Measure fast paths before keeping them.
  - Check the accounting before the ratio: a miscounted present skews every benchmark.
  - A passing test can hide an app that hangs, so the boot logo's present chain is checked through to the screen.

## 2. Decided (2026-10-11)

- **Its own repository,** this one. It links xsdk's `libxsdk.a` and can join the combined xsdk and xost later.
- **Shaders are compiled offline and looked up by their SPIR-V.**
  - Apps hand `vkCreateShaderModule` standard SPIR-V.
  - A build step runs xost over each module and writes a bundle of Xenos containers keyed by the SPIR-V; xost goes
    SPIR-V → Xenos (xost:docs/xsc.md:1-14).
  - `vkCreateShaderModule` hashes the code and finds its container. A module missing from the bundle fails with
    `VK_ERROR_FEATURE_NOT_PRESENT`.
  - **Python only in that offline step.** xost moving off Python is a later item (xsdk:TODO.md, "Toolchain").
- **Uniform buffers are copied into float constants** when a command buffer replays. Loading them from memory with
  `PM4_LOAD_ALU_CONSTANT` (xenia:src/xenia/gpu/command_processor.cc:1477-1500) is a later optimisation. It needs a
  probe first, since partial constant writes hang the GPU (xsdk:docs/HARDWARE.md, milestone 11).
- **The target is Dusklight,** through encounter's Aurora. It's demanding enough to show what's missing.
- **Vulkan 1.0 stays the version.** Nothing on the path needs 1.1: Aurora's backend asks for 1.0
  (aurora-xenon:lib/vulkan4xenon/device.cpp:303), most of what 1.1 promoted is optional, and its one new requirement,
  subgroup operations, is for compute shaders, which Xenos lacks. A single 1.1 piece a target needs comes as its
  extension on 1.0, such as `VK_KHR_maintenance1` for a negative viewport height.
- **Aurora's side is a fork,** aurora-xenon, with a backend interface and a design for a GPU without storage buffers
  (section 5).
- **The shader subset is xost's,** and its limits become the layer's, listed in SUPPORT.md:
  - no bitwise operations;
  - data-dependent branches inside loops raise an error;
  - 16 registers a pixel shader;
  - vertex shaders lower less than fragment shaders.

  See xost:docs/xsc.md:124-131, 176-195.

## 3. How Vulkan maps onto Xenos

| Vulkan | On xsdk's GPU layer |
|---|---|
| Instance, physical device, device, queue | One each. The queue is the ring. |
| Memory | One heap of physical memory, write-combined, from `xsdk_gpu_alloc`. Device-local, host-visible and coherent. Mapping returns the pointer. `vkQueueSubmit` drains the CPU's write-combining. |
| Buffers | Vertex buffers are streams (`xsdk_gpu_set_vertices`, 16 of them). Index buffers go to `xsdk_gpu_draw_indexed`. |
| Uniform buffers, dynamic uniform buffers | Copied into float constants at replay, at their dynamic offsets. |
| Push constants | Float constants, set at replay. |
| Images | `XsdkTexture`: optimal tiling is Xenos's tiled layout; linear tiling is linear with one level. Formats and mips as xsdk milestones 15 and 16 settled. |
| Image views, samplers | The texture's and the sampler state's halves of a fetch constant (`xsdk_gpu_set_texture`). |
| Descriptor sets | Combined image samplers fill fetch-constant slots 0 up, the fragment shader's, then the vertex shader's (milestone 22); uniform buffers fill constant blocks. Bindings are numbered in order across the pipeline layout. |
| Graphics pipelines | Built once at creation from xsdk's depth and stencil, blend and raster state blocks. The vertex input state gives a patched copy of the vertex shader (xsdk milestone 17). |
| Render passes, framebuffers | One subpass. A render pass instance draws into eDRAM, and each attachment's store op is a resolve into its image. A clear load op is the resolve's clear; a load op draws the image back in. 1280x720 colour and depth fit eDRAM (xsdk:SCOPE.md 3.4). |
| Command buffers | Recorded into the layer's own list and replayed into the ring at `vkQueueSubmit`. xsdk's recorded command buffers (milestone 18) can take over where a buffer's state is all known when it's recorded. |
| Fences | xsdk's fences. |
| Semaphores, barriers | One queue, so semaphores do nothing. Barriers order copies against draws. |
| Swapchain | `VK_KHR_surface` and `VK_KHR_display` over `xsdk_present`: 2 or 3 images at 1280x720. |
| Copies | `vkCmdCopyBufferToImage` and the like run on the CPU at replay; blits between images a resolve writes are drawn and resolved (milestone 27). |

## 4. The layer's milestones

1. **A triangle through Vulkan.** Done when a feature-test title draws a triangle on the console with only Vulkan
   calls, presented through the swapchain, with its pixels checked.
   - **Builds:** instance, device and queue; memory and buffers; one graphics pipeline; a render pass with one colour
     attachment; command buffers replayed at submit; a fence; the boot logo.
   - **The shader bundle:** glslang compiles the test's GLSL to Vulkan SPIR-V, and xost turns that into containers.
     xost lowers OpenGL-flavoured SPIR-V today (`glslang -G`, xost:docs/xsc.md:13); taking Vulkan's is xost work in
     this milestone.
2. **Textures, descriptors and push constants.** Done when the feature tests sample images through descriptor sets,
   read push constants and uniform buffers (dynamic offsets included), and copy a buffer into an image, each checked
   by pixel.
3. **Pipeline state.** Done when the feature tests cover, through Vulkan, what xsdk milestones 12 to 14 cover through
   the GPU layer: depth and stencil, blending, culling, dynamic viewport and scissor, indexed draws and topologies.
4. **Render to texture.** Done when an image rendered in one render pass is sampled in the next, with depth in eDRAM,
   checked by pixel. Encounter's Aurora copies its framebuffer this way.
5. **The benchmark.** Done when the same scenes run through the raw GPU layer and through Vulkan, with the gap
   measured on the console and its causes named.
6. **The replay's cost,** agreed on 2026-10-11 after milestone 5 and xsdk milestones 19 and 20 measured it. Done when
   the replay points streams and textures only when they change, and uniform buffers no longer cost the CPU an
   uncached read a word.
7. **Uniform blocks the GPU loads,** agreed on 2026-10-11 after milestone 6. Done when the GPU loads each uniform block
   straight from its buffer, and reading uniform buffers costs a draw next to nothing.
8. **Push constants the GPU loads,** agreed on 2026-10-11 after milestone 7. Done when each draw's push constants go
   through a ring the GPU loads them from, and push draws cost less for it. They cost the same, so it was reverted.
9. **Dusklight on Aurora's null backend,** agreed on 2026-10-11, the first of the Dusklight track (section 5). Done
   when Dusklight runs its title scene on the console for a minute without drawing.

### Milestone 1: a triangle through Vulkan (done)

**Done when** `build/vulkan4xenon_test.xex`, on the console:
- shows the boot logo from `vkCreateDevice` until its first present;
- draws a triangle with only Vulkan calls, presented through the swapchain for 120 frames;
- checks the image the display shows: each corner's colour, and the clear colour outside the triangle;
- passes every check, and returns to the Aurora dashboard.

**Passed on hardware** on 2026-10-11, at `aba67e7` with xsdk at `7420d94`
(`results/2026-10-11-milestone-1/xsdk.log`). All 20 checks passed, and the console went back to the Aurora dashboard.
- **The boot logo:**
  - the display moved from XAM's surface at 0 to the logo's buffer;
  - it stayed there once the swapchain existed;
  - the first present replaced it with the app's dark green, read back exactly (00 60 00).
- **The triangle:** every one of the 120 frames had its corners and clear colour.
- **What was on screen at the end:** the corners, the clear colour, and 259,200 drawn pixels, the triangle's area
  (960 x 540 / 2).
- **Presenting:** all 121 frames were shown, 121 flips over 132 vblanks. The 10 missed vblanks came from the test,
  which waits for each frame and reads it back before presenting it.
- **Seen on screen,** the user reported: the logo faded in, held for a second or two, the dark green flashed, then the
  triangle showed. The triangle's capture is `results/2026-10-11-milestone-1/triangle.png`.

**Decisions:**
- **The library and its handles.**
  - Apps link `libvulkan4xenon.a` and call `vk*` directly; `vkGetInstanceProcAddr` and `vkGetDeviceProcAddr` look
    names up in one table.
  - Dispatchable handles point at the layer's structs. Non-dispatchable ones are those pointers as 64-bit integers.
- **Replay.**
  - `vkQueueSubmit` replays on the caller's thread into xsdk's ring.
  - Each command buffer gets a fixed list of commands when it's allocated. Recording past its end makes
    `vkEndCommandBuffer` return `VK_ERROR_OUT_OF_HOST_MEMORY`.
- **Memory.**
  - One heap with one type: device-local, host-visible and coherent. Each allocation is `xsdk_gpu_alloc`'s
    write-combined memory, in 64 KB pages.
  - `memcpy` into it is safe: the toolchain's `memcpy` stores bytes or aligned words. Its disassembly has no 64-bit
    stores.
  - A plain 8-byte store needs an 8-byte-aligned address (xsdk:CLAUDE.md).
- **Shaders.**
  - An app's GLSL becomes Vulkan SPIR-V (`glslang -V`) and then a container through xost, in `tools/build_shaders.py`.
  - The app links the generated bundle: each container keyed by a 64-bit FNV-1a hash of its SPIR-V and the SPIR-V's
    size, with the literal constants xost pools (xost:docs/xsc.md, "Literal constant pool").
  - `vkCreateShaderModule` finds the entry, and a pipeline writes its literals when it's bound.
  - xost's instruction templates come from Xenia's definitions, not from XDK-compiled blobs (see the xost changes
    below).
- **Vertex input.**
  - Location n feeds the usage xost gives it: 0 position, 1 colour, then texture coordinates from 2
    (xost:xsc/resolve.py:158-162).
  - Each binding is one xsdk stream. `firstVertex` moves the stream's address forward by that many strides, since
    xsdk draws from index 0.
- **Clip space.**
  - Vulkan's y points down, and xsdk's viewport follows Direct3D's y-up (xsdk:src/gpu_draw.c:298). So the layer sets
    a viewport of negative height from the bottom edge.
  - Xenos centres pixels on whole coordinates under xsdk's `PIX_CENTER_D3D` (xenia:src/xenia/gpu/xenos.h:861-866,
    xsdk:src/gpu_setup.c:263), and Vulkan centres them on halves, so the viewport also moves up and left by half a
    pixel. Checked at the pixel level from milestone 3, which tests edges.
- **Render passes.**
  - Beginning a pass sets the eDRAM target, and ending it resolves into the attachment.
  - A resolve also clears eDRAM, to the colour it's given (xsdk:src/gpu_resolve.c:112-128). Each pass's resolve
    clears to that pass's clear colour, so a frame that clears to the same colour as the last needs nothing more.
  - When eDRAM holds anything else, such as at the start, beginning the pass resolves into the attachment once to
    clear it. That copies junk the pass then overwrites.
- **The swapchain** is `xsdk_present`'s front buffers: 2 or 3 images at 1280x720 in `VK_FORMAT_B8G8R8A8_UNORM`,
  presented in FIFO mode.
  - `vkAcquireNextImageKHR` waits on the CPU for the next buffer to leave the screen.
  - One image can be acquired at a time; SUPPORT.md lists that deviation.
- **The boot logo** is xsdk's splash.
  - `vkCreateDevice` fades it in over half a second and leaves it up.
  - The swapchain takes over the display without showing anything of its own, so the logo stays until the first
    present flips.
  - Its buffers are freed only after that flip and one more vblank, as freeing the logo's frame too early can hang.

**How it maps onto Xenos:** as section 3, less descriptors, uniform buffers, images other than the swapchain's, and
copies.

**New in xsdk:**
- `xsdk_splash_begin` and `xsdk_splash_end`, with `xsdk_present_open` taking over a splash's display;
- `xsdk_present_buffer_address`;
- `xsdk_display_shown_address`, so a test can read the image actually on screen.

**New in xost:**
- instruction templates built from Xenia's definitions, checked against the corpus as test vectors;
- a regression test lowering Vulkan SPIR-V.

**Functions:**

| Group | Functions |
|---|---|
| Instance | `vkCreateInstance`, `vkDestroyInstance`, `vkEnumerateInstanceExtensionProperties`, `vkEnumerateInstanceLayerProperties`, `vkGetInstanceProcAddr`, `vkEnumeratePhysicalDevices` |
| Physical device | `vkGetPhysicalDeviceProperties`, `vkGetPhysicalDeviceFeatures`, `vkGetPhysicalDeviceQueueFamilyProperties`, `vkGetPhysicalDeviceMemoryProperties`, `vkGetPhysicalDeviceFormatProperties`, `vkEnumerateDeviceExtensionProperties`, `vkEnumerateDeviceLayerProperties` |
| Device | `vkCreateDevice`, `vkDestroyDevice`, `vkGetDeviceProcAddr`, `vkGetDeviceQueue`, `vkDeviceWaitIdle` |
| Memory and buffers | `vkAllocateMemory`, `vkFreeMemory`, `vkMapMemory`, `vkUnmapMemory`, `vkFlushMappedMemoryRanges`, `vkInvalidateMappedMemoryRanges`, `vkCreateBuffer`, `vkDestroyBuffer`, `vkGetBufferMemoryRequirements`, `vkBindBufferMemory` |
| Pipelines | `vkCreateShaderModule`, `vkDestroyShaderModule`, `vkCreatePipelineLayout`, `vkDestroyPipelineLayout`, `vkCreateGraphicsPipelines`, `vkDestroyPipeline` |
| Render passes | `vkCreateImageView`, `vkDestroyImageView`, `vkCreateRenderPass`, `vkDestroyRenderPass`, `vkCreateFramebuffer`, `vkDestroyFramebuffer` |
| Commands | `vkCreateCommandPool`, `vkDestroyCommandPool`, `vkResetCommandPool`, `vkAllocateCommandBuffers`, `vkFreeCommandBuffers`, `vkBeginCommandBuffer`, `vkEndCommandBuffer`, `vkResetCommandBuffer`, `vkCmdBeginRenderPass`, `vkCmdEndRenderPass`, `vkCmdBindPipeline`, `vkCmdBindVertexBuffers`, `vkCmdDraw` |
| Sync and submit | `vkQueueSubmit`, `vkQueueWaitIdle`, `vkCreateFence`, `vkDestroyFence`, `vkResetFences`, `vkGetFenceStatus`, `vkWaitForFences`, `vkCreateSemaphore`, `vkDestroySemaphore` |
| Display | `vkGetPhysicalDeviceDisplayPropertiesKHR`, `vkGetPhysicalDeviceDisplayPlanePropertiesKHR`, `vkGetDisplayPlaneSupportedDisplaysKHR`, `vkGetDisplayModePropertiesKHR`, `vkCreateDisplayModeKHR`, `vkGetDisplayPlaneCapabilitiesKHR`, `vkCreateDisplayPlaneSurfaceKHR`, `vkDestroySurfaceKHR`, `vkGetPhysicalDeviceSurfaceSupportKHR`, `vkGetPhysicalDeviceSurfaceCapabilitiesKHR`, `vkGetPhysicalDeviceSurfaceFormatsKHR`, `vkGetPhysicalDeviceSurfacePresentModesKHR` |
| Swapchain | `vkCreateSwapchainKHR`, `vkDestroySwapchainKHR`, `vkGetSwapchainImagesKHR`, `vkAcquireNextImageKHR`, `vkQueuePresentKHR` |

Within them:
- 32-bit float vertex formats;
- triangle lists without culling;
- one viewport and scissor, set in the pipeline;
- no depth, stencil or blending;
- one subpass with one colour attachment, cleared or left on load and stored or dropped at the end;
- one instance a draw.

**New tests,** in one title (`tests/`):
- `boot_logo`: the logo is on screen after `vkCreateDevice`, and after the first present the screen shows the app's
  image;
- `objects`: the proc-address table, memory types and buffer requirements;
- `triangle`: 120 frames, each corner's colour and the clear colour checked on the image the display shows.

**Not in milestone 1:**
- images and samplers other than the swapchain's, descriptor sets, uniform buffers and push constants (milestone 2);
- depth, stencil, blending, culling, dynamic state, index buffers and other topologies (milestone 3);
- vertex formats other than 32-bit floats, which need xsdk to set a stream's byte swap;
- loading an attachment's old contents, and render areas smaller than the framebuffer (milestone 4);
- copies, queries, compute, secondary command buffers and multisampling.

### Milestone 2: textures, descriptors and push constants (done)

**Done when** `build/vulkan4xenon_test.xex`, on the console, passes milestone 1's checks and these, each checked by
pixel on the image the display shows:
- `texture`: images copied in from buffers with `vkCmdCopyBufferToImage`, in `R8G8B8A8_UNORM` and `B8G8R8A8_UNORM`,
  sampled through combined image sampler descriptors;
- `push_constants`: quads placed and coloured by push constants, a vertex range and a fragment range;
- `uniform_buffers`: quads placed by a matrix and coloured by vectors from uniform buffers, one of them dynamic and read
  at a different offset each draw, in two descriptor sets.

**Passed on hardware** on 2026-10-11, at `e24c820` with xsdk at `7ff6c4c` and xost at `b09a2d8`
(`results/2026-10-11-milestone-2/`). All 33 checks passed, milestone 1's 20 among them, and the console went back to
the Aurora dashboard.
- **Textures:** every texel of both 4x4 images showed in every frame, read through combined image sampler
  descriptors. Red and blue held their places in both byte orders, the second image copied from padded rows.
- **Push constants:** each quad took its place from the vertex range and its colour from the fragment range, both
  pushed before each draw.
- **Uniform buffers:**
  - each quad landed where its column-major matrix put it, read at three dynamic offsets;
  - its colour came through the same block;
  - set 1's tint halved green, from a second descriptor set and the fragment shader's own registers.
- **New paths in it that ran for the first time:** blocks copied into constants at replay; the two byte-order
  formats; xost's column-major products; a constant written straight to a varying.
- **Captures:** `texture.png`, `push_constants.png` and `uniform_buffers.png` in the results folder.

**Decisions:**
- **Where shader resources land is xost's choice, recorded in the bundle.**
  - Each shader gets its own registers. Push constants take the first ones, then each uniform block in
    (set, binding) order, each from its first byte. Combined image samplers take fetch constants from 0 in (set,
    binding) order. xost works this out from the SPIR-V alone, as `resolve_interface`.
  - `tools/build_shaders.py` writes the result into the bundle as each shader's resources: a kind, set, binding,
    first register or fetch constant, and register count.
  - `vkCreateGraphicsPipelines` matches them against the pipeline layout and refuses what the layout lacks.
- **Uniform blocks and push constants are copied in as raw bytes.**
  - A member at byte offset o of a block sits in register first + o / 16, from component (o % 16) / 4.
  - A matrix stays column-major, one column a register, as std140 lays it out. So xost lowers `M * v` as a
    multiply-add over columns and `v * M` as dot products, the reverse of what it does for OpenGL's row-uploaded
    uniforms.
  - At replay, each draw copies every block its shaders read into float constants
    (`xsdk_gpu_set_vertex_constants`, `xsdk_gpu_set_pixel_constants`), from the buffer at its offset plus any
    dynamic offset, as section 2 decided. Mapped memory is uncached, so each read is slow; milestone 5 measures it.
- **Images.**
  - 2D, optimal tiling only, which is Xenos's tiled layout with mips as xsdk milestones 15 and 16 settled.
  - Formats: `R8G8B8A8_UNORM` and `B8G8R8A8_UNORM`. Vulkan lays their bytes out in component order, so the texture
    fetch reads them unswapped. xsdk gains these two byte-order formats beside its 0xAARRGGBB one.
  - xsdk splits working out a texture from placing it, so an image's texels live in the `VkDeviceMemory` it's bound
    to.
- **Copies run on the CPU at replay.** Replay first
  waits for the GPU to finish what came before, since nothing yet orders the GPU's reads of the image against the
  CPU's writes. `vkCmdCopyBufferToImage` copies whole mip levels.
- **Samplers** become xsdk's sampler state: point or linear filtering, nearest or linear mips, repeat, mirrored repeat
  or clamp to edge, and a LOD bias. Border colours, anisotropy, comparison and LOD clamps return
  `VK_ERROR_FEATURE_NOT_PRESENT`; they're xsdk TODO items.
- **Descriptor sets** hold uniform buffers, dynamic uniform buffers and combined image samplers, one descriptor a
  binding. Fragment shaders sample; vertex shaders don't yet, since both stages share the fetch constants.
- **`vkCmdPipelineBarrier`** records nothing: replay is in order, and copies wait for the GPU.

**New in xost:** uniform blocks by set and binding, push constant blocks, members at any std140 offset, column-major
matrices, and samplers by set and binding, each with a regression test.

**New in xsdk:** `xsdk_texture_init` and `xsdk_texture_attach`, splitting `xsdk_texture_create`; the two byte-order
formats.

**New functions:** `vkCreateImage`, `vkDestroyImage`, `vkGetImageMemoryRequirements`, `vkBindImageMemory`,
`vkCreateSampler`, `vkDestroySampler`, `vkCreateDescriptorSetLayout`, `vkDestroyDescriptorSetLayout`,
`vkCreateDescriptorPool`, `vkDestroyDescriptorPool`, `vkResetDescriptorPool`, `vkAllocateDescriptorSets`,
`vkFreeDescriptorSets`, `vkUpdateDescriptorSets`, `vkCmdBindDescriptorSets`, `vkCmdPushConstants`,
`vkCmdCopyBufferToImage`, `vkCmdPipelineBarrier`.

**Not in milestone 2:**
- linear images, other formats, partial copies, image arrays and views of part of an image;
- separate images and samplers, descriptor arrays, storage buffers and texel buffers;
- sampling in vertex shaders;
- dynamic indexing into uniform arrays;
- other copies and blits.

### Milestone 3: pipeline state (done)

**Done when** `build/vulkan4xenon_test.xex`, on the console, passes milestones 1 and 2's checks and these, each checked
by pixel on the image the display shows:
- `indexed`:
  - triangle lists, strips and fans, line lists and strips, and points, from 16- and 32-bit index buffers;
  - a first index and a vertex offset;
  - a list of more than 65535 indices whose values pass 65535.
- `depth_stencil`:
  - a D24S8 attachment cleared with the render pass;
  - depth tested and written in both draw orders, and tested but not written;
  - the clear depth hiding what's behind it;
  - a stencil mask with no colour written, and a draw tested against it;
  - a constant depth bias.
- `raster`:
  - culling of clockwise and counter-clockwise quads under both front faces;
  - blending: alpha, additive, subtract, reverse subtract, min and max;
  - a colour write mask;
  - dynamic viewport and scissor;
  - edges a quarter pixel off the pixel grid, which tell Vulkan's pixel centres from Direct3D 9's.

**Passed on hardware** on 2026-10-11, at `376e2da` with xsdk at `604bdfb` (`results/2026-10-11-milestone-3/`). All 45
checks passed, milestones 1 and 2's 33 among them, and the console went back to the Aurora dashboard.
- **Three things this settled that were assumptions before:**
  - **Pixel centres.** Edges a quarter pixel off the grid covered exactly the pixels Vulkan's centres on halves say,
    600 to 699 and 500 to 599. So the half-pixel viewport shift is right, and Xenos centres pixels on whole
    coordinates under `PIX_CENTER_D3D`, as Xenia says.
  - **Front faces.** Every culling case matched with `frontFace` mapped straight to `front_clockwise`, under the
    flipped viewport. Xenos decides facing from where vertices land on screen.
  - **Triangle fans,** first drawn on the console.
- **Indexed:**
  - triangle lists, strips and fans, line lists and strips, and points, from 16- and 32-bit indices;
  - a first index and vertex offsets;
  - a list of 65541 indices split in two, its quad at indices past 65535.
- **Depth and stencil:**
  - depth cleared to 0.5 with the render pass;
  - nearer won in both draw orders, and a draw that wrote no depth was covered by a farther one;
  - nothing showed behind the clear;
  - stencil drew only over a mask written without colour;
  - a constant bias of -0.01, given in 2^-24 units, pulled an equal-depth quad in front.
- **Raster:**
  - culling under both front faces;
  - alpha, additive, subtract, reverse subtract, min and max blends within 2 per channel;
  - a red-and-blue write mask;
  - a dynamic viewport filled and a scissor clipped, the scissor's right and bottom edges exclusive.
- **Captures:** `indexed.png`, `depth_stencil.png` and `raster.png` in the results folder.

**Decisions:**
- **State maps onto what xsdk milestones 12 to 14 proved**, worked out when a pipeline is created
  (`xsdk_gpu_depth_stencil_state`, `xsdk_gpu_blend_state`, `xsdk_gpu_raster_state`) and set when it's bound.
  - Vulkan's compare and stencil ops are xsdk's, value for value.
  - Blend factors and ops map one to one, except constant colours, which return `VK_ERROR_FEATURE_NOT_PRESENT` (an
    xsdk TODO). Min and max ignore their factors in Xenia (xenia:src/xenia/gpu/spirv_shader_translator_rb.cc:3324), as
    Vulkan says; the layer sets them to one anyway, so the result holds either way.
- **Front faces.** Vulkan's clockwise is clockwise as the triangle looks in its y-down framebuffer. xsdk's culling test
  measured it the same way, as it looks on screen (xsdk:tests/test_raster.c:207). The viewport flip keeps every
  vertex where Vulkan puts it, so `frontFace` maps straight to xsdk's `front_clockwise`.
- **Depth bias.**
  - The constant factor is in units of the depth format's resolution, 2^-24 for D24. The hardware's offset is in depth
    units: Xenia scales it by 2^24 - 1 (xenia:src/xenia/gpu/draw_util.h:165-172). So the layer divides by 2^24.
  - The slope factor passes through as xsdk's per-pixel slope. Unverified, as in xsdk (xsdk:docs/HARDWARE.md,
    milestone 14).
- **Depth attachments** are `VK_FORMAT_D24_UNORM_S8_UINT`, living in eDRAM beside the colour.
  - A clear load op is the resolve's depth and stencil clear (`xsdk_gpu_set_depth_clear`). eDRAM's known contents now
    cover depth and stencil, and a pass that clears to what it holds does nothing more.
  - Depth isn't resolved out (an xsdk TODO), so a store op keeps nothing. That can't be seen: nothing that could read
    the image afterwards exists, as loading it returns `VK_ERROR_FEATURE_NOT_PRESENT` and depth images can't be
    sampled or copied.
  - Depth images take a page of memory they never use.
- **Topologies:** point lists, line lists and strips, triangle lists, strips and fans. Fans are new to xsdk:
  `XSDK_PRIMITIVE_TRIANGLE_FAN` is Xenos's `kTriangleFan` (xenia:src/xenia/gpu/xenos.h:50), first proven here.
  Primitive restart returns `VK_ERROR_FEATURE_NOT_PRESENT` (an xsdk TODO).
- **Index buffers** go to `xsdk_gpu_draw_indexed`. A first index moves the address, and a vertex offset moves each
  stream as a first vertex does. Lists of more than 65535 indices are split; strips and fans that long return
  `VK_ERROR_FEATURE_NOT_PRESENT`. Index values reach 2^24 - 1, xsdk's `VGT_MAX_VTX_INDX`.
- **Dynamic state:** viewport and scissor, one of each. Other dynamic state returns `VK_ERROR_FEATURE_NOT_PRESENT`.

**New in xsdk:** `XSDK_PRIMITIVE_TRIANGLE_FAN`.

**New functions:** `vkCmdSetViewport`, `vkCmdSetScissor`, `vkCmdBindIndexBuffer`, `vkCmdDrawIndexed`.

**Not in milestone 3:**
- two-sided stencil, constant blend colours, primitive restart, fill modes, wide lines and points, and depth bounds;
- depth resolves, so loading or sampling depth;
- dynamic state beyond viewport and scissor;
- instancing.

### Milestone 4: render to texture (done)

**Done when** `build/vulkan4xenon_test.xex`, on the console, passes milestones 1 to 3's checks and these, each checked
by pixel on the image the display shows:
- `render_to_texture`: eight render passes a frame for 60 frames, in the order encounter's Aurora draws a frame:
  1. a scene drawn into a 200x120 `R8G8B8A8_UNORM` image with a D24S8 attachment, both cleared, depth tested;
  2. that image drawn into a 96x64 `B8G8R8A8_UNORM` image, as encounter's Aurora copies its framebuffer;
  3. the scene loaded again, colour and depth, with a quad that shows only where it's nearer than pass 1's depth;
  4. the scene loaded straight after, in a pass without depth, with one more quad;
  5. both images sampled onto the swapchain image;
  6. a 64x32 `B8G8R8A8_UNORM` image cleared and drawn;
  7. the swapchain image loaded after it, with that image drawn over it;
  8. the swapchain image loaded straight after, with one more quad.
- So the checks cover:
  - depth tested in an image of another size than the screen, its last column intact;
  - red and blue in place in both byte orders;
  - colour loaded after another pass (3 and 7), where the image is drawn back in, and straight after (4 and 8),
    where eDRAM still holds it;
  - depth loaded after a pass without depth (3).

**Passed on hardware** on 2026-10-11, at `91612ec` with xsdk at `c5d87ed` (`results/2026-10-11-milestone-4/`). All 58
checks passed, milestones 1 to 3's 45 among them, and the console went back to the Aurora dashboard. The images were
read back after pass 1 and again after pass 4, and every frame of 60 was checked on screen.
- **The scene:**
  - depth tested at 200x120, with depth at the top of eDRAM;
  - its last column held the red drawn there, past the width's last whole tile of 32, written by resolving the
    texture's 224-texel pitch;
  - red and blue in place in R8G8B8A8, resolved without the red-blue swap, and in B8G8R8A8, with it.
- **The copy** held the scene as pass 1 left it, without passes 3 and 4's quads.
- **Loads after another pass,** where eDRAM held something else, so the layer drew the image back in:
  - pass 3 drew the scene back, and its blue showed only where it was nearer than pass 1's depth. So depth stayed in
    eDRAM through the copy pass, which had none;
  - pass 7 drew the swapchain image back from its front buffer, sampled as a tiled B8G8R8A8 texture.
- **Loads straight after:** passes 4 and 8 found eDRAM holding their images, so the layer drew nothing back. The
  resolves before them, which cleared nothing, left eDRAM's colour as it was.
- **Xenia disagreed:** there, a draw sampling an image resolved into got its earlier contents (CLAUDE.md). The
  console decides.
- **Capture:** `render_to_texture.png` in the results folder.

**Decisions:**
- **Where attachments go in eDRAM.** Colour from tile 0, and depth from the top, ending at tile 2048. xsdk put depth
  at 0 and colour after it (libxenon:drivers/xenos/xe.c:998-1036), so a smaller colour target landed in a larger
  one's depth. Now a pass without depth leaves an earlier pass's depth alone.
- **What eDRAM holds.** The layer tracks eDRAM's colour and its depth apart: unknown, cleared to values at a
  framebuffer's size, or an image's contents.
  - A clear finds its values there, or a resolve into the attachment sets them, as milestone 1 decided.
  - A load finds its image there, or the image is drawn back in.
  - A pass's resolve clears what the next pass in the command buffer clears, and keeps what it loads. The last pass
    in a command buffer clears to its own clear values, as milestones 1 to 3 did, so the next frame finds them.
  - Copying into an image eDRAM holds makes eDRAM's copy unknown.
- **Drawing an image back in.** eDRAM's modes draw or copy out; none loads memory in
  (xenia:src/xenia/gpu/xenos.h:886-910). So a load draws the image over the framebuffer with the layer's own shaders,
  point-sampled texel for texel, with no blending, depth or stencil. Those shaders are built offline like an app's,
  into `src/blit_shaders.c`.
- **Depth stays in eDRAM.** Depth resolves are an xsdk TODO, so depth can be loaded only while eDRAM still holds it:
  no other depth attachment and no colour reaching its tiles since. Otherwise its contents are undefined, and the
  layer logs that once.
  - Depth and stencil clear together, as one register holds both (xsdk:src/gpu_resolve.c), so a pass that clears one
    and loads the other returns `VK_ERROR_FEATURE_NOT_PRESENT`.
- **Resolves into images.** xsdk resolves into tiled textures of the byte-order formats now, swapping red and blue for
  `B8G8R8A8` and leaving the bytes in place (xenia:src/xenia/gpu/shaders/resolve.xesli:768-795). It resolves a
  texture's whole pitch, a multiple of 32, so widths needn't be one.
- **Colour attachment images:**
  - `R8G8B8A8_UNORM` and `B8G8R8A8_UNORM`, one level, both sides over 16 texels. A smaller base level sits in the
    packed mip tail, where a resolve can't write (xsdk:src/gpu_resolve.c:180-181).
  - A framebuffer's colour and depth fit eDRAM's 2048 tiles together.
- **Swapchain images load the same way.** A front buffer holds what a resolve into a tiled 1280x720 `B8G8R8A8`
  texture writes, at the same size (xsdk:src/gpu_resolve.c:171-174), so the layer samples it as one.

**New in xsdk:** `xsdk_gpu_set_target_at` and `xsdk_gpu_target_tiles`, to place targets in eDRAM;
`xsdk_gpu_set_resolve_clears`, as a resolve cleared both colour and depth before; byte-order formats in
`xsdk_gpu_resolve_texture`.

**New functions:** none. Colour attachment images, `VK_ATTACHMENT_LOAD_OP_LOAD` and `R8G8B8A8_UNORM` colour
attachments are new.

**Not in milestone 4:**
- render areas smaller than the framebuffer. Milestone 1 put them here, but they need a resolve of part of eDRAM or
  clears by drawing, and encounter's Aurora always renders whole targets. They wait for an app that needs them.
- depth resolves, so depth loaded after eDRAM lost it, sampled or copied;
- colour attachments 16 texels or less on a side, with mips, or more than one in a subpass.

### Milestone 5: the benchmark (done)

**Done when** `build/vkbench.xex`, on the console, runs each scene through xsdk's GPU layer and then through
vulkan4xenon, in two runs of the title, and this section records each scene's draw + submit and GPU time through the
layer against xsdk's, with the gap's causes named from measurements.

- **vkbench:** scenes, data and per-frame metrics, with a backend for xsdk's GPU layer and one for vulkan4xenon.
  - Both backends are in one title and run one after the other, so code they share is laid out the same for both.
  - Both draw with the same xost-compiled shaders: the xsdk backend loads the containers of the bundle vulkan4xenon
    looks SPIR-V up in, and sets their constants where the bundle says.
  - Each opens the GPU, the boot logo and the display as `vkCreateDevice` and the swapchain do, so the logo shows
    before each half.
- **Scenes:**
  - `draws_250`, `draws_1000` and `draws_4000`: that many cubes, one draw each, with depth, no state changes;
  - `states_250` and `states_1000`: the texture changes every draw, the shader every second and blending every fourth;
  - `uniforms_1000`: `draws_1000` with each draw's matrix and tint in a dynamic uniform buffer, as encounter's Aurora
    passes its uniforms, where the other scenes push them. Milestone 2 left the cost of
    reading uniform buffers at replay for this one to measure;
  - `stream_1000` and `stream_8000`: that many sprites, moved on the CPU and written out every frame, one draw;
  - `fill_4` and `fill_16`: alpha-blended full-screen layers, bound by the GPU, so both should tie.
  - `instances_4000`, added for xsdk's milestone 47 (xsdk:docs/scope/47-instancing.md, step 5): `draws_4000`'s cubes
    in one indexed draw of 4,000 instances, each cube's matrix and tint at the instance's rate, through
    `xsdk_gpu_draw_indexed_instanced` and `vkCmdDrawIndexed`; through each backend its picture must be `draws_4000`'s.
  - vkbench's minify scenes are left out: they compare linear and swizzled images, and images here are always tiled.
- **Per frame, in microseconds,** as vkbench measures them: sim, draw, submit, present and frame, and also gpu: from
  the submit's kick to its fence landing, with the CPU spinning on it. 60 warm-up and 600 measured frames a scene.
  Medians, means and 99th percentiles go in the log and in `game:\vkbench_summary.csv`, every frame in
  `game:\vkbench_frames.csv`.
- **Accounting,** checked before any ratio:
  - xsdk's draw calls write the ring as they go and the layer writes it in `vkQueueSubmit`, so draw + submit is the
    number to compare, not either alone;
  - both kick the ring only at their fence, or when it fills (xsdk:src/gpu.c), so the GPU starts at submit in both;
  - both wait for the next image in present: `xsdk_present_begin` and `vkAcquireNextImageKHR` move there;
  - both present in FIFO at every vblank, the only mode the swapchain has, so frame times step in vblanks; gpu shows
    what the GPU took.
- **The same picture:** each scene's last frame is sampled on a grid in both backends, and the two must match.
- **Differences from vkbench:** 1280x720; sprite colours are four floats, as the layer takes only 32-bit float vertex
  formats; v-sync stays on.

**Passed on hardware** on 2026-10-11, in two whole runs. Each passed all 14 checks, both backends drew the same
picture in every scene, and the console went back to the Aurora dashboard.
- **Run 1**, at `ff0f2c7` with xsdk at `a441b04` (`results/2026-10-11-milestone-5/`: `vkbench_run1.log` and its
  CSVs), on xsdk's 32 KB ring.
- **Run 2**, at `077d2c4` with xsdk at `b5933a0` (xsdk:results/2026-10-11-milestone-19-stream-3/), after xsdk
  milestone 19 put commands into a stream the ring runs every 1024 dwords as they're written, and with the ring's
  counts. So the GPU starts during xsdk's draw calls and the layer's submit, and gpu counts only what's left after it.
- **The test title** passed all 58 checks after each (`results/2026-10-11-milestone-5/vulkan4xenon_test.log`,
  xsdk:results/2026-10-11-milestone-19-regression/).

Draw and submit, median, in microseconds:

| Scene | xsdk, run 1 | vulkan4xenon, run 1 | xsdk, run 2 | vulkan4xenon, run 2 |
|---|---|---|---|---|
| `draws_250` | 8,049 | 11,684 | 307 | 724 |
| `draws_1000` | 37,590 | 47,515 | 1,213 | 2,867 |
| `draws_4000` | 151,190 | 195,871 | 4,836 | 11,703 |
| `states_250` | 13,987 | 15,686 | 545 | 907 |
| `states_1000` | 55,496 | 65,512 | 2,167 | 3,607 |
| `uniforms_1000` | 37,590 | 48,514 | 1,214 | 6,852 |
| `stream_1000` | 700 | 704 | 698 | 702 |
| `stream_8000` | 5,553 | 5,565 | 5,554 | 5,564 |
| `fill_4` | 12 | 20 | 10 | 16 |
| `fill_16` | 22 | 49 | 17 | 39 |

GPU time tied: within 1% in the sprite and fill scenes in both runs, and in the cube scenes both finished 0.3 to
0.5 ms after the submit, about the frame's resolve.

**The gap's causes,** from the counts:
- **Run 1: both waited for xsdk's ring.** A cube wrote 156.4 dwords through xsdk and 200.3 through the layer (run 2's
  counts; what a draw writes didn't change between the runs). The 32 KB ring filled every 52 cubes through xsdk, and
  each fill slept about 2 ms, a 1 ms sleep having measured 1,993 us. So the layer's 1.28 times the dwords cost it
  1.26 and 1.30 times the time in `draws_1000` and `draws_4000`.
- **Run 2: nothing waited, so the CPU's work shows.** A cube cost xsdk 1.21 us and the layer 2.93: 0.23 recording and
  2.69 at submit, replaying it into xsdk's calls (`src/queue.c`). The layer's 44 extra dwords point the vertex stream
  and the texture again every draw, each behind a coherency request, though neither changed.
- **Uniform buffers:** `uniforms_1000` cost the layer 4.0 us a draw more than `draws_1000`, for reading two blocks, 20
  words in all, from mapped memory at submit. That memory is write-combined, and reads from it are slow. Run 1 showed
  1.0 us.
- **Sprites tie:** both write vertices with `memcpy` into write-combined memory, which runs at 185 MB/s on the console
  (xsdk:docs/HARDWARE.md), so `stream_8000`'s 1 MB takes 5.5 ms either way.
- **Fill ties on the GPU:** each blended full-screen layer took 462 us, about 2.0 Gpixels a second, in both.

**Decisions:**
- **Command buffers grow.** `draws_4000` records 12,000 commands a frame, and 320 KB of push constants, past the 4096
  commands and 16 KB of data each command buffer had. Each starts at that size now and doubles when it fills, keeping
  its size through resets, so recording stops allocating after the first frames.
- **xost fetches into r0 last** (xost `098f17a`). Vertex input n is fetched into rn, and every fetch reads the vertex
  index from r0, so fetching input 0 first overwrote the index the rest read. In Xenia, which reads the index as each
  fetch runs (xenia:src/xenia/gpu/dxbc_shader_translator_fetch.cc:80-88), sprites took their uv and colour from the
  vertex their x position named, and the two backends' pictures differed until the fix. Milestone 2 passed on the
  console with the old order, so the hardware may not show it. Shaders with one input are unchanged; the test titles'
  and the blit shaders' containers are rebuilt.
- **The GPU's time is spun on.** `xsdk_gpu_wait` sleeps a millisecond between polls, which would round gpu up to
  whole milliseconds, so the xsdk backend spins on `xsdk_gpu_fence_done` and the Vulkan one on `vkGetFenceStatus`.
- **No display, no swapchain.** Where the display won't open, as in Xenia, each backend draws into an image of the
  screen's size and samples that, and logs that it did. So the bench runs and compares pictures in Xenia, whose
  timings mean nothing.
- **Comparing pictures:** at every 20th pixel across and down, from the 10th, 64 by 36 points, each channel within 2.
  The first 3 points that differ are logged with both colours.
- **Counting the ring, after run 1.** Run 1 left the causes inferred from code, so vkbench now also logs, for each
  scene, what xsdk's ring did a frame: dwords, kicks, waits for room and the time waited
  (`xsdk_gpu_stats`). It also times a 1 ms sleep, and 1 MB written into write-combined memory and into cached memory
  that is then flushed. Run 2 carries them; they are xsdk milestone 19's first step (xsdk:SCOPE.md).

**New functions:** none. Every call vkbench makes was there after milestone 4.

### Milestone 6: the replay's cost (done)

**Why.** After xsdk milestones 19 and 20, a cube drawn through the layer cost 1.50 us to xsdk's 0.63, and one taking
its matrix and tint from uniform buffers 3.8 us more (xsdk:docs/HARDWARE.md, "Fewer dwords a draw"):
- the replay points the vertex stream and the texture again for every draw, 22 dwords and the calls behind them,
  though neither changed (`src/queue.c`);
- it reads each uniform block from mapped memory a word at a time, and mapped memory is write-combined, so each read
  goes to memory: 20 words, about 190 ns each.

**In two steps,** each run on the console:
1. **Point what changed.** The replay remembers each stream's address and size and each slot's texture and sampler,
   and points them only when they differ, forgetting them where something else points them: a resolve, drawing an
   image back in, and the start of each submit.
2. **Uniform buffers the GPU loads.** `PM4_LOAD_ALU_CONSTANT` has the command processor copy constants from memory
   (xenia:src/xenia/gpu/command_processor.cc:1477-1515), so the CPU wouldn't read the buffer at all. A probe on the
   console first, at the sizes blocks take, as a lone float constant written as a register hung the GPU
   (xsdk:docs/HARDWARE.md). If it takes them, each block loads straight from its buffer; if not, the layer reads blocks
   16 bytes at a time with VMX, a quarter of the reads.

**Done when,** on the console:
- vkbench's `draws_1000` through the layer draws and submits faster than its 1,503 us, in fewer dwords a draw;
- `uniforms_1000` costs at most a third of its 3.8 us a draw more than `draws_1000`;
- vulkan4xenon_test passes in full, and vkbench's backends still draw the same pictures.

**Passed on hardware** on 2026-10-11, at `ef044d8` with xsdk at `051bffd` (`results/2026-10-11-milestone-6/`: vkbench's
log and CSVs, and the test title's tail).
- **vkbench passed all 14 checks,** both backends drawing the same picture in every scene, with no waits. Through the
  layer, drawing and submitting took:
  - `draws_1000` 1,221 us, from 1,503, in 65.3 dwords a cube, from 87.1; `draws_4000` 5,062 us, from 6,218;
    `states_1000` 2,006 us, from 2,111;
  - `uniforms_1000` 2,091 us, from 5,309, so its uniform buffers cost 0.87 us a draw more than `draws_1000`'s push
    constants, from 3.8: five 16-byte reads of write-combined memory, about 175 ns each.
  - A cube costs 1.97 times xsdk's, from 2.40.
- **The test title passed all 58 checks,** its render to texture among them: eight passes a frame, with resolves and
  images drawn back between them, where the replay forgets what it pointed.
- **VMX loads from write-combined memory work,** the first on the console.

**Decisions:**
- **`LOAD_ALU_CONSTANT` waits.** The console loads float constants from memory only in whole groups of 4 from a
  multiple of 4, and hung loading a group of 3 (xsdk:docs/HARDWARE.md). Blocks aren't placed on those groups, so a load
  would overwrite their neighbours. xost placing each block on its own groups would let the GPU load it with no CPU
  reads at all.
- **Pointing is forgotten at each render pass's start,** not tracked through the resolves and the image drawn back:
  draws happen only inside passes, and the first draw of each points what it needs.

### Milestone 7: uniform blocks the GPU loads (done)

**Why.** Milestone 6 left uniform buffers costing 0.87 us a draw: the CPU reads each block from write-combined memory,
16 bytes at a time. The console loads float constants from memory with `LOAD_ALU_CONSTANT`, but only in whole groups
of 4 from a multiple of 4 (xsdk:docs/HARDWARE.md), and xost placed blocks wherever the last one ended, so a load would
overwrite whatever shared a block's groups.

**In three parts,** run on the console together:
1. **xost gives each uniform block whole groups of 4 to itself** (xost `b9b38e9`): it starts on a multiple of 4, and
   whatever follows it, another block or the literal pool, starts on the next. Push constant blocks stay packed, as the
   CPU writes them.
2. **xsdk loads float constants from memory** (xsdk `5d4bda6`): `xsdk_gpu_load_vertex_constants` and
   `xsdk_gpu_load_pixel_constants`, whole groups of 4 from a multiple of 4, from 64-byte aligned memory, as the console
   loaded them.
3. **The replay has the GPU load each uniform block** from its buffer, its padding taking whatever follows the block,
   which nothing reads. A block whose buffer isn't 64-byte aligned, or whose descriptor's range doesn't cover it, still
   goes through the CPU. Buffers are 64-byte aligned now, and uniform offsets go in 256s.

**Done when,** on the console:
- vkbench's `uniforms_1000` through the layer costs at most 0.2 us a draw more than `draws_1000`, from 0.87;
- xsdk_test and vulkan4xenon_test pass in full, and vkbench's backends still draw the same pictures;
- xost's tests pass.

**Passed on hardware** on 2026-10-11, at `02f593b` with xsdk at `5d4bda6` and xost at `b9b38e9`
(`results/2026-10-11-milestone-7/`).
- **vkbench passed all 14 checks,** both backends drawing the same picture in every scene. Through the layer,
  `uniforms_1000` drew and submitted in 717 us, from 2,091, in 17.3 dwords a draw, from 49.3: 0.50 us a draw less than
  `draws_1000`'s push constants, where milestone 6 left it 0.87 more. It costs 1.16 times xsdk's own, from 3.4.
  - The GPU had 0.49 ms left at the submit, from 0.30: the CPU finishes sooner, and each load is the GPU's work now.
    The scene's draw, submit and GPU time still fell, 1.2 ms from 2.4.
- **The test title passed all 58 checks** on the second run (`vulkan4xenon_test-tail.log`), its uniform buffers loaded
  by the GPU at the registers xost gave them. The first (`vulkan4xenon_test-alignment-check.log`) failed only the
  objects check that still expected 16-byte buffer alignment; it drew everything right.
- **xsdk_test passed all 203 checks** (xsdk:results/2026-10-11-constant-loading/), test_constants's quads drawn right
  with their constants loaded from memory.
- **xost's tests pass,** and its block test fails with the groups of 4 turned off.

**Decisions:**
- **Push constant blocks stay packed.** The CPU writes them in groups of 4 from its copy, so they can share groups with
  literals; only a uniform block, which the GPU loads, needs groups to itself.
- **Buffers are 64-byte aligned,** as the console loaded constants from; with uniform offsets in 256s, every uniform
  block in a buffer bound at a 64-byte offset can be loaded.

### Milestone 8: push constants the GPU loads (reverted)

**Why.** After milestone 7 a draw taking its matrix and tint from uniform buffers cost 0.72 us through the layer, and
one pushing them 1.22 us: the CPU writes push blocks into constants itself, through xsdk's setters and their copy, 65
dwords a cube against a uniform draw's 17.

**In two parts,** run on the console together:
1. **xost gives push blocks whole groups of 4 to themselves,** as uniform blocks have, so loading one can't overwrite
   anything else.
2. **A ring the GPU loads push constants from.** A 4 MB ring of write-combined memory: each draw copies its push blocks
   in, padded to groups of 4, and has the GPU load them. A submit's bytes are reused once the GPU passes its fence, and
   the invalidate each submit starts with keeps the GPU's caches from holding an earlier lap. A submit that would lap
   the ring by itself, or a block that doesn't start a group, goes through the CPU as now.

The bytes the CPU stores barely change, about 48 into the ring against 56 written into constants; what goes is the
setters' bookkeeping.

**Done when,** on the console:
- vkbench's `draws_1000` through the layer draws and submits faster than its 1,215 us, measurably; otherwise the
  change is reverted (xsdk:CODE_GUIDELINES.md, "Measure, don't guess");
- vulkan4xenon_test passes in full, and vkbench's backends still draw the same pictures;
- xost's tests pass.

**Not faster on hardware,** so reverted on 2026-10-11. vkbench ran at `2e94c05` with xsdk at `a7151aa` and xost at
`af5bb8b` (`results/2026-10-11-milestone-8/`), passing all 14 checks with no waits. Through the layer, against
milestone 7's run:
- **`draws_1000` drew and submitted in 1,217 us, from 1,215,** its submit 974 us both times, though a pushed cube took
  17.3 dwords, from 65.3, and a frame 18 kicks, from 64.
- **`states_1000` took 2,021 us, from 1,975,** and `draws_4000` 5,078, from 5,043; `uniforms_1000` was unchanged at
  718.
- **The GPU took about 24 us longer after each cube scene's submit:** 325 us in `draws_1000`, from 301, and 512 in
  `uniforms_1000`, from 488, though the build changed neither that scene's path nor its shaders. Why is unmeasured.

**What it showed:** a pushed cube and a uniform one now wrote the same 17.3 dwords and 18 kicks, yet the pushed one's
submit cost 0.97 us a draw against 0.50. Writing three quarters fewer dwords saved nothing, so the push path's cost
isn't the command stream. What the push path does beyond the uniform path is copy each `vkCmdPushConstants` into the
replay's push array, then copy each block back out, into constants before, into the ring here. Where the time goes in
that is unmeasured.

The layer is back at milestone 7's code. xost packs push blocks again; its `vk_block_literals` test stays, its
uniform block still on c4 and its literals from c8.

### Milestone 9: Dusklight on Aurora's null backend

**Why.** Every later milestone needs the game running on the console, and running it settles the platform, the disc,
memory and byte order before anything draws. Read on 2026-10-11, what stands in the way:
- **Dusklight quits on the null backend:** with `BACKEND_NULL` it runs only its launcher's UI loop and returns
  (dusklight:src/m_Do/m_Do_main.cpp:737-751).
- **SDL3 is under everything:** Aurora's events, controller, file streams, paths, ticks and delays, threads and audio,
  and Dusklight's frame limiter (dusklight:src/dusk/time.h:31-72), with no port to the 360. Aurora pins release
  3.4.10 (aurora:cmake/AuroraDependencyVersions.cmake:20).
- **nod, Aurora's disc reader, is Rust** (aurora:cmake/AuroraNodProvider.cmake:41-80), with no target here.
- **RmlUi,** Dusklight's own menus, builds only with Dawn, as it draws with WebGPU.
- **Byte order:** the console is big-endian like the GameCube, so the PC build's swaps must turn off. Dusklight's data
  is mostly typed `BE()` fields behind one switch, `TARGET_LITTLE_ENDIAN`, but its big-endian branch has no `BSWAP64`
  and swaps constants regardless (dusklight:include/helpers/endian.h:9-29, 63-98), and Aurora's command stream writer
  swaps every word (aurora:lib/gx/fifo.hpp), which breaks even the null backend.
- **Types:** PowerPC's `char` is unsigned, and Dusklight adds `-fsigned-char` only on ARM
  (dusklight:CMakeLists.txt:256-260); xsdk's `uint32_t` is `unsigned long`, the PC build's `u32` `unsigned int`
  (dusklight:libs/dolphin/include/dolphin/types.h:15-17).
- **Memory:** the game asks for 256 MB of GameCube memory and 24 MB of ARAM (dusklight:src/m_Do/m_Do_main.cpp:705-706),
  and Aurora's null backend about 63 MB of pools; xsdk's heap reserves 256 MB (xsdk:src/sbrk.c). What the kernel
  leaves a title is unmeasured.

**Decided with the user:** SDL3 is ported, an xsdk platform added to it in a fork of its own, so Aurora, Dusklight and
borealis call it unchanged; the disc is a plain ISO on the console's drive; the forks are Z:\dusklight-xenon,
Z:\aurora-xenon and Z:\SDL-xenon (from release 3.4.10). Switched off: code mods, which load code
at run time; Dawn and nod; borealis's HTTP, network, crash, Discord, update and Sentry parts. Dusklight's menus, RmlUi,
run with a render interface that draws nothing: their header uses RmlUi's types in 23 of the game's files, so turning
them off would touch all of those, and the vulkan4xenon backend needs RmlUi's renderer later anyway (agreed with the
user after the scope above). Their settings come from `config.json` until that renderer brings them back.

**In three steps,** each run on the console:
1. **It builds and starts.** A CMake toolchain file in xsdk; the forks wired together; the libraries the build keeps
   built with xsdk's toolchain. The `.xex` shows the splash, logs its size and the memory the kernel left free, and
   exits.
2. **The platform.** SDL3's xsdk backends: the controller on `XamInputGetState`, audio on xsdk's audio client, the
   timer, threads on xsdk's pthreads and paths on `game:\`; in xsdk, what they need beyond it today, as xsdk
   milestone 21 (xsdk:SCOPE.md). Dusklight runs the game on the null backend up to the disc. The game reads the
   disc's ID before `main01` (dusklight:src/dusk/version.cpp:13-24, called at dusklight:src/m_Do/m_Do_main.cpp:903),
   so with no reader yet the `.xex` reports the disc missing and ends; reaching `main01` moved to step 3, agreed with
   the user on 2026-10-12.
3. **The disc, byte order and memory,** as scoped with the user on 2026-10-12:
   - **The disc:** a reader for plain GameCube ISOs behind nod's C API, in Aurora's fork. It reads the disc header,
     holds boot.bin, bi2.bin, the apploader, main.dol and the FST in memory, since Dusklight reads data out of
     main.dol (dusklight:src/dusk/dvd_asset.cpp:39), walks the FST as nod does and reads files by offset. Wii and
     compressed images are refused. A GameCube disc holds 1,459,978,240 bytes, so 32-bit offsets reach all of it.
   - **Byte order, as far as the null backend runs it:**
     - Aurora's command-stream writer swaps every value (aurora:lib/gx/fifo.hpp), while its reader swaps only data
       not in the host's order (aurora:lib/internal.hpp); the writer will swap only on a
       little-endian host.
     - Dusklight swaps the `BE<>` values the compiler sets at build time whatever the host
       (dusklight:include/helpers/endian.h:63-98). About 10 are hit, among them the message files' signature
       (dusklight:libs/JSystem/src/JMessage/data.cpp:5) and the audio envelope tables.
   - **Left for milestone 11,** when drawing shows whether they're right: Aurora's texture conversion swaps; the
     vertex and matrix arrays Dusklight flags little-endian in 58 places; and the memory card and controller binding
     files, which read back on the console but differ from a PC's.
   - **Memory:** the PC's sizes stay, 256 MB of main memory and 24 MB of ARAM
     (dusklight:src/m_Do/m_Do_main.cpp:705-706), inside the 442 MB the title has free.
   - **The run ends itself** a minute into the title scene, with status 0, after logging the frame rate and each
     heap's peak.

**Done when,** on the console:
- Dusklight's `.xex` starts with the splash, opens the ISO from the console's drive and runs the title scene (stage
  F_SP102, dusklight:src/d/d_com_inf_game.cpp:1221) for a minute on the null backend, with no call failing;
- its log names the scene, its frame rate and each heap's peak;
- xsdk_test passes in full.

**All three steps passed on hardware** on 2026-10-12, xsdk_test passing in full with each (xsdk:docs/HARDWARE.md, from
"What the kernel leaves a title"):
- **Step 1,** Dusklight `455b3f5`: the `.xex` showed the splash, logged its image, 32 MB on the console's calling
  convention, and the 442 MB of the title's 480 MB left free, and exited. xsdk's heap now reserves the title's free
  memory at its first growth, 443 MB here (xsdk:results/2026-10-12-memory/, 2026-10-12-heap-reservation/ and
  2026-10-12-dusklight-mcall-xsdk/).
- **Step 2,** Dusklight `13316c3` with Aurora `c7f5905`, SDL-xenon `c86389c89` and xsdk `f770193`: the game's own
  startup ran on the null backend, reading its config, starting Aurora with no window and loading RmlUi's fonts, and
  ended with status 1 at the missing `game:\dusklight.iso` (xsdk:results/2026-10-12-dusklight-step-2/). It took two
  xsdk changes: `main` gets argc 1, since Dusklight reads its options from `argv[1]`, and a path `std::filesystem`
  makes absolute, `game:\` in front of `game:\...`, starts over at its second drive.
- **Step 3,** Dusklight `0069f0d` with Aurora `94c1353`, SDL-xenon `c86389c89` and xsdk `c98ceb8`, the GZ2E01 disc
  beside it: the title scene ran its minute on the null backend at 30 fps, after 19 fps over the 10 seconds it
  loaded in, and the title exited with status 0 (xsdk:results/2026-10-12-dusklight-step-3/). At their peaks the
  game's root heap held 137 MB of its 256 and newlib's heap 377 MB of the 443 it reserves. Two console crashes
  Xenia never showed came first: a float load from Aurora's byte-packed command stream raised an alignment exception
  (GCC had merged an integer load and a cast into it), and Dusklight's reset freed message queues whose woken threads
  then relocked their mutexes, corrupting newlib's heap. The calls that still fail don't stop it: the memory card has
  no save yet, the pipeline cache has no database (sqlite has no file system here), and there's no window to title.

With that, milestone 9 is done: Dusklight runs on the console, from its disc, up to the title scene's game loop.

**The revert passed on hardware** on 2026-10-11, at `0579b93` with xsdk at `a7151aa` and xost at `77c7605`
(`results/2026-10-11-milestone-8-reverted/`): vulkan4xenon_test passed all 58 checks, and vkbench all 14. Through
the layer it ran milestone 7's figures to within 5 us: `draws_1000` 1,215 us in 65.3 dwords a cube, its submit 974;
`states_1000` 1,975; `uniforms_1000` 719; the GPU 301 us after `draws_1000`'s submit. The same code runs that close
from one run to the next, so the ring build's differences above were its own: it was a little slower, not as fast.
xsdk's TODO.md lists the ring, for if a run shows the cost it removes.

### Milestone 10: the boot screens drawn

**Why.** The game's own frames on the console, first what it shows while it loads. Read on 2026-10-12:
- **The PC build draws nothing before the title.** It skips the GameCube's warning, Nintendo and Dolby screens
  (dusklight:src/d/d_s_logo.cpp:1227-1229), so the archives load behind black and the title scene fades in.
- **Those screens are simple:** each is one textured 2D quad, under the fader's full-screen one. They take position,
  colour and one texture coordinate, one to three TEV stages, alpha blending and an alpha compare, with no depth and no
  framebuffer copies (dusklight:libs/JSystem/src/J2DGraph/J2DPicture.cpp:704-786,
  libs/JSystem/src/J2DGraph/J2DGrafContext.cpp:33-65). Their textures, in the disc's LogoUs.arc, are I4, IA4 and IA8.
  That is about five shader configurations. The title scene's are several dozen at least, with fog, matrices picked
  per vertex, palettes and framebuffer copies.
- **A Vulkan backend fits the fork's seam** (aurora-xenon:lib/gfx/backend.hpp). Beyond what the layer had, it would
  want 8-bit vertex colours (vulkan4xenon refused them, src/pipeline.c:40-58), a second memory type, an RGBA
  swapchain, linear textures and a shader compiler at run time.
- **xost compiles a restricted GLSL** (xost:docs/xsc.md, xsc/lower.py): one store per local, no function calls and no
  unary minus, and one comparison per discard. Indices into blocks must be constants. The boot screens' shaders fit
  that; the title scene's matrices picked per vertex, fragment position and depth export don't.
- **A PC's recorded configurations don't carry over:** Aurora's pipeline cache keeps each configuration as its raw
  struct, bitfields included, with 32-bit hashes on a 32-bit host (aurora:lib/gx/pipeline.hpp, lib/hash.hpp).

**Decided with the user (2026-10-12):** the first frames are the GameCube's boot screens, restored on the console
under `__XSDK__` with the PC's code kept; the title scene moves to milestone 11. Shader configurations are recorded by
the 360's own null backend, in Xenia or on the console, instead of on a PC.

**In two steps,** each run on the console:
1. **The boot screens and their shaders.** Dusklight shows the warning, Nintendo and Dolby screens on the null
   backend. A GLSL generator for the boot screens' configurations, modelled on Aurora's WGSL
   (aurora:lib/gx/shader.cpp) and written in the form xost compiles, runs in the null backend and writes each
   configuration's GLSL to a file on `game:\`. Each recorded program compiles with glslang and xost.
2. **The backend.** Aurora's vulkan4xenon backend:
   - with one memory type, a `B8G8R8A8` swapchain, every texture tiled and uniforms at 256-byte offsets;
   - with stores that are safe for write-combined memory;
   - with its shaders from a bundle an offline step builds out of the recorded GLSL, leaving out any program xost
     can't fit in the pixel shader's 16 registers.

   The layer takes 8-bit RGBA vertex colours, and Aurora's texture conversion reads IA8 on a big-endian host.

**Done when,** on the console:
- the warning, Nintendo and Dolby screens show with their fades while the game loads;
- the title scene then runs, its configurations missing from the bundle left undrawn;
- every recorded boot-screen configuration compiles with xost;
- xsdk_test and vulkan4xenon_test pass in full.

**Step 1 passed on hardware** on 2026-10-12, Dusklight `8089c2f` with Aurora `d263186`
(`results/2026-10-12-milestone-10-step-1/`):
- the logo scene went through the warning, Nintendo and Dolby screens on the null backend at 60 fps, the warning
  holding its minute with no button pressed;
- the title scene then ran its minute at 30 fps, and the title exited with status 0;
- the console recorded 36 programs, byte for byte the ones Xenia recorded from the same build;
- glslang and xost compiled all 8 of the boot screens' and 27 of the title scene's 28. The last needs 18 registers,
  over the pixel shader's 16 (`compile.txt`), and xost only warns that it would hang.

**Step 2 passed on hardware** on 2026-10-12, Dusklight `43b8f65` with Aurora `037ac39` and this repository at
`6be3389` (`results/2026-10-12-milestone-10-step-2/`; Dusklight's log names `54e3f14`, now `b03a1f6`, the commit at
its last configure):
- **The boot screens drawn:** Aurora's vulkan4xenon backend drew the warning, Nintendo and Dolby screens, which faded
  in and out on the user's screen, at 50 to 60 fps. The title scene then ran its minute, at 19 fps over the 10 seconds
  it loaded in and 30 after, its configurations missing from the pack left undrawn, and the title exited with status 0
  (`dusklight.log`). Its other errors are the title scene's, left for milestone 11: GLSL not yet written for fog,
  lighting, matrices picked per vertex and indirect texturing, depth copies, `GXPeekZ`, and a 20x11 copy target, under
  the layer's smallest colour attachment.
- **The pack** holds 68 of the 69 programs recorded: the 36 the console recorded in step 1, and the backend's 33 clears,
  copies and present, which Xenia recorded from the same build. The title scene's 18-register program is left out, so
  its draws are skipped. Aurora's `tools/build_glsl_pack.py` builds it, through this repository's
  `tools/build_shaders.py`.
- **The tests:** vulkan4xenon_test passed all 64 checks, 8-bit packed vertex colours among them
  (`vulkan4xenon_test-tail.log`). Broken on purpose, the colours packed the other way round, it failed only the two
  packed-colour pixel checks: the yellow corner read as magenta, cyan as white and magenta as yellow
  (`vulkan4xenon_test-broken.log`). xsdk_test passed all 230 (`xsdk_test-tail.log`).
- **Memory is tight.** The title scene started with 46 MB free and ended with 7: as it loaded, the GPU's physical pages
  went from 54 to 72 MB and newlib's heap from 330 to 347 MB. Aurora's frame pools and texture caches are sized for the
  console from runs in Xenia, at about 4 times a frame's peak for the pools and 16, 16 and 8 MiB for the texture
  caches. The game's root heap keeps the PC's 256 MB, 137 of it used, until gameplay can be measured (decided with
  the user). Dusklight's run log now prints the kernel's count of the title's memory every 10 seconds; Xenia's
  `MmQueryStatistics` doesn't count as the console's does.
- **Two runs before it ran out of memory** soon after the title scene loaded. On a first run Dusklight opens a preset
  window, which the backend doesn't draw, since Aurora records RmlUi's draws only on Dawn. The press that skipped the
  warning screen chose the Dusklight preset there unseen, turning on frame interpolation and 4x shadow maps, whose
  1536x1536 targets are over the layer's 1280x720 (`dusklight-preset.log`). The window isn't opened on the console now.
  Xenia, where nothing presses a button, never showed it. Saving the settings fails on the console ("Function not
  implemented"), so the preset didn't stay.

With that, milestone 10 is done: the console draws the game's boot screens while it loads.

Since then: pixel shaders get 64 registers under xsdk's split, so the pack holds all 69 programs, and the title scene's
18-register program draws (xsdk:docs/HARDWARE.md, milestone 24).

### Milestone 11: the title screen drawn

**Why.** The title scene, stage F_SP102 (dusklight:src/d/d_com_inf_game.cpp:1221), is the first scene the player
sees, and what it draws today is only the configurations the pack holds. Read on 2026-10-12, from the run with all 69
programs (xsdk:results/2026-10-12-milestone-24/dusklight.log):
- **The configurations the generator refuses,** each logged once with its first reason (Aurora
  lib/vulkan4xenon/glsl.cpp, `make_plan`): fog 42, matrices picked per vertex 15, lighting 4, indirect texturing 3.
  A configuration needing several is counted once, so how many need each isn't known.
- **What each needs:**
  - **Lighting:** the WGSL loops over a channel's lights, testing its mask's bits (aurora:lib/gx/shader.cpp). Unrolled
    over the lights the config's mask names, the lights are read at constant indices, with `normalize`, `dot`, `max`
    and division, which xost compiles (xost:docs/xsc.md). Its vertices also carry normals.
  - **Fog:** the WGSL reads the fragment's depth (aurora:lib/gx/shader.cpp); clip z and w can reach the fragment
    program in an interpolant instead, with `exp2` for the exponential kinds. Range fog adds a factor by the pixel's
    column: the fragment's position, xost's generated parameter (xenia:src/xenia/gpu/registers.h:140, 234),
    and a constant array indexed at run time. The game turns range fog on per environment
    (dusklight:src/d/d_kankyo.cpp:9459).
  - **Matrices picked per vertex:** the position, normal and texture matrices come from arrays indexed by a vertex
    attribute (aurora:lib/gx/shader.cpp). Xenos reads constants at `c[a0 + n]`, `a0` set by `MOVA`
    (xenia:src/xenia/gpu/ucode.h:196-202, 1140-1148); xost reads arrays only by a loop counter.
  - **Indirect texturing:** the WGSL masks texel values with an integer `&` (aurora:lib/gx/shader.cpp); the masks are a
    power of two less one, so `mod` of the floor does it without the bit operations Xenos lacks.
- **Memory:** each pipeline's two shaders take two 64 KiB pages at least, as xsdk rounds every GPU allocation up to a
  page (xsdk:src/gpu.c, `allocate`; xsdk:TODO.md, "Small GPU allocations"). The refused configurations' pipelines would
  take about 8 MB more, and the title scene ends with 9 to 11 MB free.
- **Copy targets:** the scene copies into a 20x11 target, which the layer refuses as a colour attachment 16 texels or
  less on a side (src/image.c:54-56), the size at which xsdk packs a texture.
- **Byte order,** left by milestone 9 until drawing shows it: Aurora's texture conversion swaps, the vertex and matrix
  arrays Dusklight flags little-endian in 58 places, and the memory card and controller binding files.
- **From encounter's upstream** (aurora:`3227d76..c8406f2`, read 2026-10-12): 23 commits, none of them a speed-up,
  and Dusklight's upstream still pins `3227d76`. Five apply onto the fork without conflict and fix something:
  `3094e04`, packed TEV alpha compares taking the colour operands from before the stage; `afa388d`, `SET_REG_FIELD`
  masking the value to its field; `c8406f2`, the DVD keeping its handle out of `userData`; `0857ad8`, texture
  coordinates as a texgen source, a fatal error before; and `77997d4`, `CARDProbeEx` with null outputs. `b922153`
  bounds the texture content, copy and offscreen caches with an LRU, and conflicts in 9 files with the fork's
  backend-neutral types. The rest are left: `d664382`'s input rework drops `aurora/lib/input.hpp`, which Dusklight
  includes (dusklight:src/dusk/action_bindings.cpp:6), and the others are churn, or already in the fork.
- **The fork's tuning,** read the same day:
  - a shader config's key holds its channels' light masks (aurora-xenon:lib/gx/gx.hpp:185), so each mask makes a
    pipeline of its own, and a draw is skipped until its pipeline is built (aurora-xenon:lib/vulkan4xenon/encoding.cpp,
    `usable_pipeline`);
  - pipelines compile on as many threads as cores less two, 4 on xsdk
    (aurora-xenon:lib/gfx/pipeline_cache.cpp:1149-1155), but the backend holds one lock across the pack lookup and
    the pipeline's creation (aurora-xenon:lib/vulkan4xenon/pipeline.cpp:503);
  - quads, strips and fans take their indices from prebuilt patterns only when `AURORA_INDEX_PATTERN_VERTICES` is
    set (aurora-xenon:CMakeLists.txt), and Dusklight doesn't set it.
- **Left for milestone 12,** depth out of eDRAM: the Z16 depth copy and `GXPeekZ`.

**Agreed with the user on 2026-10-12,** with Dusklight's PC build as the reference, and the audit's items added the same
day. **In six steps,** each run on the console:
1. **What the scene needs, measured.** A recording build logs, once each, every refused configuration with all it
   needs, not only the first, and each copy target's size and format; a run in Xenia counts them, says whether the
   scene turns range fog on, and counts the configurations that differ only in their light masks. The counts decide
   whether range fog and small copy targets are in this milestone.
2. **Aurora from upstream, and the 360's settings:** `3094e04`, `afa388d`, `c8406f2`, `0857ad8` and `77997d4`
   picked, with `3094e04`'s packed alpha compares mirrored in the fork's GLSL generator, which today compares the
   alpha operands (aurora-xenon:lib/vulkan4xenon/glsl.cpp, `alpha_op`): checked by a config made to hit it, its GLSL
   reading the colour operands as the WGSL does and compiling with glslang and xost. Dusklight takes Aurora's index
   patterns, `AURORA_INDEX_PATTERN_VERTICES` 4096, under the patterns' 16-bit indices, and 2 pipeline compile
   threads. The title scene runs as at milestone 24, its frame rate and free memory compared.
3. **xost: uniform arrays indexed at run time,** in vertex shaders (`m[int(i)]` lowered to `MOVA` and `c[a0 + n]`),
   and, if step 1 finds range fog, in pixel shaders with the fragment's position. Checked through xost's oracle, and
   on the console by vulkan4xenon_test: a vertex shader that picks each quad's matrix by a vertex attribute, broken on
   purpose with the index ignored.
4. **Memory:** shaders' microcode shares pages, xsdk's milestone 25 (xsdk:SCOPE.md): 256-byte blocks of shared
   64 KiB pages, with an xsdk_test check that 200 small shaders fit a few pages where they took 200 and draw right,
   broken on purpose by overlapping two. Aurora's texture content, copy and offscreen caches take upstream's LRU
   bounds (`b922153`), ported by hand to the fork's backend-neutral types, the fork's byte cap on converted textures
   their budget, checked by Aurora's host tests. The title scene runs on the console, its free memory compared.

**Step 4 scoped with the user on 2026-10-12,** from a count in Xenia (xsdk:results/2026-10-12-gpu-allocations/):
Dusklight's boot screens and a minute of its title scene made 87 GPU allocations, 73 under a page and 70 of those 24
to 1,440 bytes, shaders' microcode, about 85 KB in 73 pages, 4.6 MB. Only microcode shares pages, decided with the
user, as the other small allocations would save a few. Microcode had a page each, not shared pages as step 4 said
first. `b922153` conflicts in 9 files, 18 places; it frees nothing in the title scene, whose 8 copy sizes and 2
offscreen targets are each used every frame, but bounds the caches for gameplay and keeps a static copy texture that
upstream cleared on a resize.
5. **The generator and the pack:** fog, lighting, matrices picked per vertex and indirect texturing, and range fog if
   step 1 finds it, as gx/shader.cpp's WGSL writes them, in the GLSL xost compiles. Lighting's light masks are
   either kept in the key, each mask's lights unrolled, or left out of it by a backend limit, all eight lights
   unrolled and each one switched by a constant: decided with the user from step 1's counts. The scene is recorded
   again in Xenia, since every program's name is the hash of its text, and the pack rebuilt.
6. **What drawing shows:** the user captures the title screen on the console, and the same moment from Dusklight's
   PC build, the reference agreed with the user, at the settings the console runs: a 1280x720 window, Dusk bloom and
   depth of field, the internal resolution automatic, 1x shadows and no frame interpolation. The byte order above is
   fixed where the captures show it wrong, and the copy targets step 1 finds are drawn, if the layer must take small
   colour attachments.

**Changed with the user on 2026-10-12,** after step 1: the captures moved from step 1 to step 6, where they're first
compared. Step 1's counts settled all it decides, and a capture of what the console draws before step 5 would show
nothing step 6 can't.

**Done when,** on the console:
- the title scene runs its minute and exits with status 0, refusing no configuration for fog, lighting, matrices picked
  per vertex or indirect texturing, and missing no program from the pack; the depth copies and `GXPeekZ` are the only
  unsupported features it reports;
- the title screen, captured, matches the reference capture of the same moment, as the user judges it, both filed in
  `results/`;
- it ends with no less free memory than milestone 10's 7 MB;
- the fork carries upstream's `3094e04`, `afa388d`, `c8406f2`, `0857ad8` and `77997d4` and `b922153`'s bounded
  caches, and Dusklight runs with index patterns and 2 pipeline compile threads;
- vulkan4xenon_test, with its matrix-per-vertex check, vkbench and xsdk_test pass in full, vkbench within the noise of
  its last run.

**Step 1 passed on hardware** on 2026-10-12, Dusklight `8f99ef3` with Aurora `265dcda`
(`results/2026-10-12-milestone-11-step-1/`; Dusklight's log names `1961c31`, the commit at its last configure). The
generator logs each configuration it refuses once, under its hash, with all it needs; a recording build logs each
target it creates; `tools/count_refusals.py` counts both (`counts-xenia.txt`, from the recording build in Xenia):
- **62 configurations refused,** the same 62 needing the same in the recording build and the drawing build in Xenia
  (`dusklight-record-xenia.log`, `dusklight-xenia.log`) and the drawing build on the console (`dusklight.log`):
  - **fog 50, lighting 46, matrices picked per vertex 15, indirect texturing 5,** and nothing else;
  - **most need more than one:** fog and lighting 31, those and matrices picked per vertex 10, fog alone 7, matrices
    alone 5, lighting alone 4, indirect texturing alone 3, and indirect texturing with fog 2, one of them lit too;
  - **by the first each needs** they are 40, 15, 4 and 3. Milestone 24's 64 lines, 42 of them fog, were one for each
    pipeline asking, so two configurations were logged twice.
- **Range fog is on:** 43 of the 50 fog configurations adjust it by range. All are perspective: 47 linear and 3 reverse
  exponential, 2 of those squared. Range fog is in this milestone.
- **Lighting:** every lit channel is COLOR0, with clamped diffuse and spot attenuation, its ambient from the register
  in all but one. 30 sum all eight lights, 12 seven, 2 six and 2 one. No two configurations differ only in their light
  masks: the 46 stay 46 with every mask cleared, though each one's hash changes. Keeping the masks in the key costs no
  pipeline here, and leaving them out saves none.
- **Matrices picked per vertex:** by the position matrix's index alone in 7, and by it and texture matrix 1's in 8.
- **Indirect texturing:** one indirect stage in each of the 5.
- **Targets:** copies at 1280x720 and each half of it down to 20x11, and one at 192x192, all RGBA8 (GX format 6); and
  offscreen targets with depth at 384x384 and 960x360. Only the 20x11 copy is 16 texels or less on a side, so small
  copy targets are in this milestone too, at step 6.
- **The programs the generator writes didn't change:** the recording wrote the pack's 69, byte for byte.
- **The console run** went as at milestone 24: the title scene at 29.1 to 30.0 fps once loaded, 8 MB free at its end
  and status 0, with milestone 24's other errors.

**Step 2 passed on hardware** on 2026-10-12, Dusklight `3d1853a` with Aurora `da395ea`
(`results/2026-10-12-milestone-11-step-2/`; Dusklight's log names `8f99ef3`, the commit at its last configure):
- **Upstream's five fixes** applied to the fork without a conflict, each keeping its author and naming the commit it
  came from: `77997d4` as `d6b542b`, `3094e04` as `f8bb045`, `0857ad8` as `95613f0`, `afa388d` as `2676de6` and
  `c8406f2` as `d2617ff`. Aurora's DVD tests passed all 39 on the host (`dvd_tests.txt`), among them `c8406f2`'s new
  one and the 12 that read a disc, through the fork's ISO reader from the GameCube disc Dusklight reads.
- **The packed alpha compares in the GLSL** (`da395ea`): an alpha op's R8, GR16 and BGR24 compares take their keys from
  the colour operands a and b, as the WGSL now does, while A8 still compares the alpha operands. Aurora's new
  `vulkan4xenon_glsl_tests` checks each on a config made to hit it, and passed all 7 (`vulkan4xenon_glsl_tests.txt`).
  Broken on purpose, comparing the alpha operands again, it failed the 6 packed compares and passed A8. glslang and xost
  compiled all 42 programs its configs and the backend's own make. None of the title scene's programs compares in its
  alpha op: the recording build in Xenia wrote the pack's 69 as before, byte for byte, and refused step 1's 62
  (`dusklight-record-xenia.log`, `counts-xenia.txt`).
- **Dusklight's settings:** pipelines compile on 2 threads (`Pipeline compilation: 2 at a time`, from 4), and draws of
  up to 4096 vertices take their indices from Aurora's patterns. A frame's indices peaked at 222 KB, from 223:
  patterns serve only draws not merged into the one before, and a merged draw's indices still go into the frame's pool
  (aurora-xenon:lib/gx/command_processor.cpp, `draw_prim`).
- **The console run** (`dusklight.log`) went as at milestone 24: the title scene at 29.2 to 30.0 fps once loaded,
  against 29.4 to 30.0, and 9 MB free at its end, as then; status 0, with milestone 24's other errors and step 1's 62
  refused. The 10 seconds it loads in ran at 14.8 fps, against 19.2 at milestone 24 and 15.9 at step 1, both on 4
  threads: the count's 10-second windows take the boot screens' end and the loading in other parts each run.

**Step 3 passed on hardware** on 2026-10-12, this repository at `a39ad55` with xost at `7a5e42e`
(`results/2026-10-12-milestone-11-step-3/`; xsdk:docs/HARDWARE.md, "Reads at a0, and the pixel position"):
- **xost reads a block's array at an index known only at run time** (`4d7c297`): `MOVA` sets `a0` from the index, the
  element at `c[a0 + n]` is copied into a temp, and a constant added to the index goes in `n`, so a matrix's rows share
  one `MOVA`. A pixel shader's `gl_FragCoord.xy` comes from the pixel parameters, the register past the
  interpolators, plus 0.5; its `zw` are refused, and before, it lowered silently as the first interpolator. xost's
  `tests/test_dynamic_index.py` runs each shader through a simulator, `MOVA` and `a0` included, and the oracle. Every
  other test shader lowers as before, byte for byte, and so do Dusklight's 69 programs and this repository's other
  shaders.
- **vulkan4xenon_test passed 72 of 72** (`vulkan4xenon_test.log`), with two checks of its own (`test_run_time_index.c`):
  four quads each placed and coloured by the matrix their vertices name, three rows a matrix, and, beside the scope,
  the screen coloured in 128-pixel bands picked by `gl_FragCoord.x`, probed either side of band edges, as range fog
  needs both and only the console shows the pixel parameters. Broken on purpose, both shaders ignoring their index,
  the quads stacked in one corner and the bands took one colour, failing those 4 checks alone
  (`vulkan4xenon_test_broken.log`).
- **The first run failed the bands** (`vulkan4xenon_test-first.log`): every column read band 0. A diagnostic title
  (`vulkan4xenon_diag.log`, its sources in `diagnostic/`) showed a pixel shader's reads at `a0` work, but the pixel
  parameters' x reads 0 without `SQ_CONTEXT_MISC.SC_OUTPUT_SCREEN_XY`, which xost now sets with them (`7a5e42e`), and
  that the execs must keep addressing mode 0 for reads at `a0`.

**Step 4's first part, xsdk's milestone 25, passed on hardware** on 2026-10-12, xsdk at `4bcc93f`
(`results/2026-10-12-milestone-25/`; xsdk:docs/HARDWARE.md, "Shaders' microcode in shared pages"): shaders'
microcode takes 256-byte blocks of shared pages, where each shader took a page. vulkan4xenon_test passed 72 of 72 and
vkbench 14 of 14, with milestone 24's command stream and its times within noise. Dusklight's title scene ended with
13 MB free, from 9 at step 2, the title holding 69 MB of physical pages from 73. Aurora's LRU caches are next.

**Step 4 passed on hardware** on 2026-10-12, Dusklight `6b3d713` with Aurora `64066c4`
(`results/2026-10-12-milestone-11-step-4/`; Dusklight's log names `3d1853a`, the commit at its last configure):
- **Upstream's LRU caches** (`b922153`), ported by hand to the fork's backend-neutral types: the converted-texture,
  copy and offscreen caches share Aurora's `LruCache`. Converted textures keep the fork's byte cap, 16 MiB in
  Dusklight; copies and offscreen targets take upstream's 64 MiB, more than the console has free, so what bounds them
  there is upstream's other limit: a copy size or offscreen target unused for 32 frames goes. Offscreen targets are
  keyed by their formats too, as upstream's are, and the caches are no longer cleared on a window resize.
- **Aurora's host tests,** on Dawn, which the cache tests need: lru_cache_tests 7 of 7, gx_texture_cache_tests 38 of
  38 (upstream's 14 new), gfx_recording_tests 18 of 18 (upstream's 6 new, one varying the colour format alone, as the
  fork's formats name one depth format); on the null backend, dvd_tests 39 of 39 and lru_cache_tests 7 of 7. Broken on
  purpose three ways (`breaks.txt`), an LRU that doesn't reorder on use, a destination's superseded copy left outside
  the budget and an offscreen key without its formats, they failed 5, 12 and 1 tests.
- **gx_fifo_tests' two byte goldens** had failed since step 2's `2676de6`, upstream's `afa388d`, which masks a value to
  its register field; step 2 ran only the DVD and GLSL tests. Aurora `dbc0cf8` records them again from the per-value
  writes they came from, `bfaa5f3`, with the masking applied: built as it was, it gives the old goldens, all five
  (`goldens-e3908bb.txt`). gx_fifo_tests passes 236 of 236, and with the masking undone fails those two alone.
- **The console run** (`dusklight.log`): the title scene at 29.3 to 30.0 fps once loaded and 14 MB free at its end,
  against 13 at milestone 25, the title holding 69 MB of physical pages as then; status 0, with milestone 25's errors.
  It refused 61 configurations: the 62nd, `4b9dad0c2751e69f`, is drawn near the minute's end, and this run drew 45
  frames fewer after its slower load (14.5 fps against 19.6). The same `.xex` in Xenia refused all 62 and logged step
  1's Xenia errors (`dusklight-xenia.log`). The log came back whole.

**Step 5 passed on hardware** on 2026-10-13, Dusklight `962860e` with Aurora `470e2de` and xost `a22f915`
(`results/2026-10-13-milestone-11-step-5/`):
- **The generator** (Aurora `46e7a2b`, with the view's depth range from `f58b2da`) writes fog with its range
  adjustment, lighting with each mask's lights unrolled, the masks kept in the key as decided with the user, matrices
  picked per vertex through `a0`, and indirect texturing, as gx/shader.cpp's WGSL writes them.
  vulkan4xenon_glsl_tests passed 33 of 33. tools/check_glsl_math.py runs 14 named configurations' programs, compiled
  as the pack is, on xost's simulator against its own reference of the WGSL: all 14 agreed (`check_glsl_math.txt`).
  Broken on purpose seven ways, each break failed the cases it touches (`glsl_breaks.txt`).
- **xost** (`3d46e0c`, `a22f915`): temporaries run past r63 until the allocator packs them, vertex programs are
  allocated past 64 registers, dependent texture reads gather their coordinates first, and a vertex program's
  branches are refused, where they had been miscompiled without a word. Its 15 test files pass, 164 checks
  (`xost-tests.txt`). Of 536 lowerings, 470 came out as before, none that compiled changed, 24 more compile, and the
  18 now refused are the earlier generator's vertex programs with a `?:`, which it no longer writes
  (`xost-compare.txt`).
- **The scene recorded again** in Xenia refused no configuration: the pack's 69 programs came out byte for byte as
  before, and 71 more, 140 in all.
- **The first console run** (`dusklight-first.log`) drew every configuration and exited with status 0, but ran the
  title scene at 0.3 fps. Xenia shows none of the layer's frames, so Aurora `470e2de` has the backend write frame
  captures, the EFB untiled into a PPM, and Dusklight writes the title scene's 20 seconds in. In Xenia
  (`xenia-before-fixes.png`) the scene's models drew as flat triangles through the screen's centre, as they had on
  the console all along: the user had only ever seen the boot screens draw right. Two of the PC port's byte-order
  assumptions, step 6's to fix, were brought forward:
  - **Arrays the host writes,** which games mark `le`: J3D's matrices
    (dusklight:libs/JSystem/include/JSystem/J3DGraphBase/J3DSys.h:154) and its skinned vertices
    (dusklight:libs/JSystem/src/J3DGraphBase/J3DShape.cpp:158), read swapped on the big-endian console. The command
    processor reads them in the host's order now (Aurora `08900a8`; `xenia-geometry-fixed.png`).
  - **Textures' big-endian words,** RGB565, RGB5A3, C14X2 and CMPR, swapped unconditionally, which drew the models'
    textures as noise (Aurora `e34bef3`; `xenia-title.png`, `xenia-castle-gate.png`).

  With both fixed the console ran at 17 to 22 fps, so the 0.3 fps was the broken geometry: triangles across much of
  the screen, drawn with step 5's programs.
- **The console run** (`dusklight.log`): the title scene drew correctly, as the user saw on the screen; status 0,
  nothing refused, step 4's errors only. It ran at 16.9 to 22.1 fps once loaded, against step 4's 29.3 to 30.0 with
  61 configurations undrawn, and ended with 10 MB free, from 14, with more textures drawn. Xenia ran the same `.xex`
  at 30.0 fps (`dusklight-xenia.log`). Aurora's host tests passed on Dawn, gx_fifo 236, gx_texture_cache 38,
  gfx_recording 18 and lru_cache 7, and on the null backend dvd 39, vulkan4xenon_glsl 33 and lru_cache 7; the
  byte-order fixes change nothing on a little-endian host.
- **Where the frame goes,** from a diagnostics build (`dusklight_diag.log`; its sources in `diagnostic/`, against
  Aurora `470e2de` and Dusklight `32e17a7`). Every 10 seconds it logged each worker's time and the GPU's, the latter
  from submit to fence and so with the layer's replay of the command buffer, and drew the next period without one
  feature's draws. In the title's last, still shot, at 18.2 to 18.9 fps, a frame took the GPU 32 to 36 ms, the
  render worker 32 to 34 ms, 5 of them waiting for the display, and the FIFO worker 10 ms. Without the lit draws,
  nearly the whole 3D scene, the title scene ran at 28.5 fps. Without the indirect draws, 16 to 19 a frame with 1,000
  to 1,300 vertices in the periods before, it ran at 28.1, the GPU's time down to 30 ms from 48. With every feature's
  draws gone, 25 draws left, it ran at 28.0 fps, the GPU still at 20 ms a frame. The periods are different moments of
  a moving scene, so they compare neighbours, not the same frames. Reaching 30 fps is open.

**After step 5, xsdk's milestone 26** (2026-10-13; `results/2026-10-13-milestone-26/`; xsdk:docs/HARDWARE.md, "Threads
on all six hardware threads"): every thread had run on hardware thread 0, as the kernel keeps a thread on its
creator's, and now each of Aurora's and the game's threads runs on the one its list gives. vulkan4xenon_test passed 72
of 72 and vkbench 14 of 14, its frame times as at milestone 25. The title scene ran at 16.2 to 27.6 fps, from 16.9 to
22.1: most frames still take more than the 33 ms of two vblanks.

**Then the GPU's pixel shading, and xost's optimiser** (2026-10-13; `results/2026-10-13-shader-optimizer/`, the
diagnostics' sources in `diagnostic/`). A diagnostics build timed the replay's parts and the GPU in the title's still
logo shot, and drew phases of it differently:
- **The replay's 19 ms was a wait.** Per frame, the 246 draws' pipelines, uniforms, push constants, textures, streams
  and packets took 0.6 ms together; every uniform block loaded on the GPU. The rest was in `end_render_pass`, in one
  wait for the GPU: a frame resolves 9 sizes, and xsdk keeps a resolve's rectangle for 8 (xsdk:src/gpu.c,
  `rectangle_slot`), so reusing one waits for the GPU to finish the resolve that last used it
  (`dusklight_diag_rectangles.log`). With 32 slots the wait went, the render worker dropping from 22 to 4.5 ms a frame,
  and the frame rate didn't move (`dusklight_diag_32_rectangles.log`): the GPU is what holds it. xsdk keeps 8 for now.
- **The GPU's time is pixel shading.** The logo shot took 25.9 ms of it a frame. Scissored to one pixel, every draw's
  vertex work and the resolves still done, it took 7.9 ms; scissored to 3/4 of each side, 15.0 ms, and to 1/2, 11.0
  (`dusklight_diag_scissor.log`, `dusklight_diag_before.log`). Lowering the resolution was ruled out with the user:
  the work goes into the shaders.
- **xost's programs carried waste.** The heaviest pixel program's 156 ALU instructions held 36 moves, 19 reciprocals
  mostly of uniforms, scalar ops in instructions of their own, and multiplies and adds unfused (`pack_stats.txt`).
  xost's optimiser (xost `fed0752`, xasm/optimize.py) rewrites a program's instructions before allocation: 3,232 of the
  pack's pixel ALU instructions became 2,184 and 5,612 vertex ones 4,883; all 137 programs it can run matched before
  and after, as instructions and as microcode, and the 3 with relative addressing are left as they were
  (`check_optimize.txt`, `test_optimize.txt`). Some instructions now name two constants, 224 in the pack, which the
  console takes. tools/build_shaders.py (`0e7889e`) and Aurora's tools/build_glsl_pack.py (Aurora `89b5c5f`) take
  `--optimize`, and Dusklight's pack is built with it (Dusklight `c6ea25a`).
- **On the console** the optimised pack drew the logo shot as before, but for the logo's moving shine, which differs
  between two unoptimised runs too (`shot_before.png`, `shot_after.png`, `shot_diff.png`,
  `shot_diff_two_unoptimised_runs.png`), and took 22.0 ms of GPU time a frame, from 25.9 (`dusklight_diag_after.log`).
- **Five programs had been missing** since at least milestone 26, their draws skipped: the title's attract demo uses
  them, about 230 s into the title scene, which the recording never reached. A recording build run that long on the
  console recorded all five (`dusklight_record.log`, the log read as the title wrote it), and the pack holds 145.
  That run then **ran out of memory** as the demo's movie stage loaded, `sbrk` refused with 0xC0000017 and 51 MB free,
  and crashed; the drawing build has less free there, so the title screen left alone will crash when its demo starts.
  That's open, apart from the shaders.
- **The drawing build** (`dusklight.log`, `title.png`; xsdk, the layer and Dusklight cleaned and built in full): the
  pack's 145 programs, none missing, status 0, 8 MB free at the end. The title scene ran at 26.3, 28.7, 27.7, 30.0
  and 29.7 fps over its 10-second windows, from milestone 26's 16.2 to 27.6.

**Then range fog read from a table** (2026-10-13; `results/2026-10-13-range-fog-table/`), the next step of the shader
work agreed with the user. Range fog's factor depends only on the pixel's column, and the programs worked it out per
pixel, about 20 ALU instructions and a square root. Drawn without it, the logo shot took 19.5 ms of GPU time a frame,
from 21.9; the texture sizes' reciprocals, also worked out per pixel, cost nothing measurable, the optimiser pairing
them with vector instructions.
- **32-bit float images:** xsdk's milestone 28 added `XSDK_FORMAT_R32F` (xsdk:docs/HARDWARE.md, "32-bit float
  textures"), and the layer samples `VK_FORMAT_R32_SFLOAT` images as it (`e0462aa`; docs/SUPPORT.md, Formats): optimal
  tiling, nearest only, not colour attachments. vulkan4xenon_test's texture check draws one, its red from floats, one
  under 0 and one over 1, and passed 72 of 72 (`vulkan4xenon_test.log`); broken on purpose, the format taken as
  R8G8B8A8, only that check's two failed, its texels the floats' bytes (`vulkan4xenon_test_r32f_as_bytes.log`).
  vkbench passed 14 of 14.
- **The table** (Aurora `3a53fee`): the FIFO worker already builds the factor for each column into the storage pool,
  as the WGSL reads it. The backend copies it into a 64x32 R32_SFLOAT texture, column x at texel (x % 64, x / 64), 8
  KiB where a single row would take 32 rows of tiles, written by the CPU before any frame draws with it, so with no
  copy and no wait for the GPU; finds tables seen before by their factors, keeping 8; and binds it at set 2 for
  pipelines whose program has range fog. The generator samples it nearest, u repeating, at the middle of the column's
  texel. vulkan4xenon_glsl_tests passed 33 of 33, and tools/check_glsl_math.py 14 of 14, its range fog cases now also
  checking that the sample lands on the middle of its texel (`check_glsl_math.txt`); broken on purpose, the table's
  width taken as 32, or the sample off the middle of its row, each failed (`glsl_breaks.txt`).
- **The pack** (Dusklight `d44654aa97`): the title scene recorded again in Xenia gave the 25 range-fog fragment
  programs anew, each the old one with the generator's change and nothing else (`recorded_pairs.txt`); the other 120
  came out byte for byte as before. Optimised, the 25 take 896 ALU instructions, from 1,485, the heaviest 70 from 94
  (`alu_pairs.txt`); xost's optimiser check passed on all 25 (`check_optimize.txt`).
- **Drawn the same:** in Xenia the diagnostics build's logo shot came out pixel for pixel as step 1's, and with the
  table zeroed on purpose 14.7% of it changed, the castle and the far hills (`captures.txt`,
  `xenia_zero_table_diff.png`). On the console it differed from step 1's only in the logo's moving shine, as two of
  step 1's runs do (`shot_diff.png`).
- **On the console** (`dusklight_diag.log`, against `results/2026-10-13-shader-optimizer/dusklight_diag_after.log`):
  the logo shot took 20.6 ms of GPU time a frame, from 21.9, and 19.6 from 21.4 in the phase that skips the few draws
  picking matrices per vertex. The drawing build (`dusklight.log`, `title.png`; everything cleaned and built in full)
  drew the title scene with nothing missing at 29.8, 28.9, 29.9, 29.8 and 30.0 fps once loaded, and exited with status
  0, 8 MB free.

**Then the memory for the attract demo** (2026-10-13; `results/2026-10-13-attract-demo-memory/`), the out of memory
step 1 left open, three fixes agreed with the user:
- **Movie frames kept for nothing** (Aurora `b9ede7a`): Aurora's pre-conversion worker parked its result for content
  the FIFO thread had already converted inline and uploaded, at the draw it reached first. A movie's frames come once
  each, so nothing took them until the ready budget or their 120 s ran out, and the recording build's budget was
  Aurora's 128 MiB. The worker now checks the content wasn't uploaded before it parks. Aurora's Dawn tests passed, the
  texture cache's 39 with a new one that holds the worker in its conversion while the draw uploads the same content
  (`aurora-dawn-tests.txt`); with the check removed only that test failed, 64 bytes parked
  (`aurora-dawn-tests-not-parked-check-removed.txt`).
- **MEM1, the game's root heap, at 144 MiB** (Dusklight `24404604ed`, the Xbox 360 only), from the PC's 256: it parents
  the system, game, archive and smaller heaps at fixed sizes, 140,176 KB in all, and past start-up only a debug build's
  camera editor and previewer take from it, so 119 MiB were never handed out.
- **The recording build takes the drawing build's frame pools, index patterns and texture caps** (the same commit), so
  its memory is the drawing build's.
- **On the console,** with the title scene held 300 s rather than 60 for the run: the drawing build (`dusklight.log`) ran
  the title scene with 120 MB free, from 8, at 28.7 to 29.9 fps, then played the movie to the end with 35 to 44 MB
  free; the root heap peaked at 140,176 KB of 147,439. The recording build (`dusklight_record.log`), which had run out
  of memory as the movie began, played it at 30 fps with 197 MB free, flat. Both exited with status 0.
- **Open: the movie plays at 7.5 fps** in the drawing build, as the user saw on the screen and a second run logged
  (`dusklight_rerun.log`), and the title's physical pages rise from 69 to about 145 MB while it plays. On the null
  backend it plays at 30, so the cost is the backend's: each movie frame is three new textures, made and uploaded, and
  each upload waits for the GPU (docs/SUPPORT.md, Commands).

### Milestone 12: texture uploads at the CPU's speed (done)

**Why.** A diagnostics build timed the attract demo's movie on the console (2026-10-13): a frame takes 133 ms, 114 of
them in the replay's copies of its three textures, 1.6 MB, and 5 more waiting for the GPU before them; the GPU needs 3
after the replay, and the game thread waits 115 ms a frame for a frame slot. The copies read Aurora's upload buffer,
in write-combined memory the CPU reads uncached, and xsdk_texture_upload wrote a texel at a time. The movie's I8
planes are expanded to RGBA8 first, four times the bytes.

**Decided with the user (2026-10-13), all three, with xsdk's milestone 29:**
1. **Copies read cached memory.** A second memory type, `HOST_VISIBLE`, `HOST_COHERENT` and `HOST_CACHED`, the CPU's
   cached memory, which only buffers whose sole usage is `TRANSFER_SRC` take: copies run on the CPU, so the GPU never
   reads it. xsdk_texture_upload writes runs of whole words (xsdk's milestone 29).
2. **8-bit textures stay 8-bit.** `VK_FORMAT_R8_UNORM` and `VK_FORMAT_R8G8_UNORM` images, sampled, filtered linearly
   and copied into, xsdk's R8 and R8G8; image views take a component mapping, which the fetch constant's swizzle
   applies (xsdk_texture_swizzle). Aurora uploads I8 as R8, sampled as red in every channel.
3. **A copy waits only for what used its image.** Each image keeps the submission that last drew with it, sampled it,
   resolved into it or drew it back; each submission keeps the fence it ended with. A copy into an image no submission
   used doesn't wait, one used by an earlier submission waits for that fence, and one used earlier in its own
   submission waits as before.

**Done when,** on the console:
- vulkan4xenon_test draws R8 and R8G8 images through swizzled views, as intensity and as intensity and alpha, and an
  R8 image filtered linearly between two texels; broken on purpose, a view's mapping ignored, the check fails;
- vulkan4xenon_test copies into an image, draws with it, copies other texels into it and draws again in one command
  buffer, each draw showing its own texels; broken on purpose, a copy never waiting, the check fails;
- vulkan4xenon_test copies from a buffer in the cached memory type, and its objects check finds the type offered to
  transfer-source buffers alone;
- Dusklight's movie plays at its 30 fps, measured by the diagnostics build, and the title scene draws as before;
- xsdk_test, vulkan4xenon_test and vkbench pass.

**On the console** (2026-10-13; `results/2026-10-13-milestone-12/`; xsdk's milestone 29, xsdk:docs/HARDWARE.md,
"Uploads in runs, and channel swizzles"; Aurora `1844958`, Dusklight `9001ae09e6`):
- **vulkan4xenon_test passed 80 of 80** (`vulkan4xenon_test.log`). R8 drew as intensity, and R8G8 through a view of
  green, red, 1 and 0, so that both its channels show on screen, where alpha doesn't; the 2x1 R8 image filtered
  linearly between its two texels; two copies into one image around a draw in one command buffer, each draw showing its
  own texels; the copies came from a staging buffer in the cached memory type, which the objects check found offered to
  transfer-source buffers alone.
- **Broken on purpose,** a view's mapping ignored, the swizzled and filtered checks failed, the filtered image's view
  being swizzled too (`vulkan4xenon_test_mapping_ignored.log`); a copy never waiting, only the copies check failed
  (`vulkan4xenon_test_copies_never_wait.log`). Each break failed the check of the image on screen as well.
- **Found on the way:** the first run's filtered image read neither of its texels
  (`vulkan4xenon_test_2x1_as_wide.log`). xsdk had put a 2x1 texture's base where the console doesn't read it, fixed in
  xsdk (xsdk:docs/HARDWARE.md, "A 2x1 texture's tail").
- **vkbench** passed 14 of 14.
- **The movie** (the diagnostics build, `dusklight_movie.log`, against `dusklight_movie_before.log`): 29.9 to 30.0 fps
  over its four windows, from 7.5. A frame's uploads came to 224 to 266 KB, textures of 133 KB, from three of 532 KB,
  the movie's I8 planes now 8-bit; the copies took 2.4 to 3.1 ms a frame, from 114, with no wait for the GPU, from
  5.3 ms; the game thread no longer waited for a frame slot, from 115 ms. Drawn right (`movie.png`;
  `movie_before.png`, a different moment).
- **The title scene** (the drawing build, `dusklight.log`, `title.png`; xsdk, the layer and Dusklight cleaned and built
  in full): 29.1 to 30.0 fps once loaded, with 118 MB free, and status 0 at the exit.
- **The logo shot** (the diagnostics build, `dusklight_diag.log`, `shot.png`): drawn as in the range fog step's run but
  for the logo's moving shine, 0.05% of its pixels (`shot_diff.png`), in 20.6 ms of GPU time a frame, as then.

### Milestone 13: compiling shaders on the console (done)

**Agreed with the user (2026-10-14):** programs a pack doesn't hold are compiled on the console the first time the
game asks for them, and kept on the hard disk, so no one has to play the game drawn badly to fill a pack
(xsdk:docs/scope/shader-compiling.md). glslang on the console and xost's compiler in C are done; this milestone is
that scope's step 3, in five parts: xsdk's `rename` (done), the layer compiling SPIR-V its bundle doesn't hold
(done), the layer keeping what it compiles in a folder the app names (done), Aurora's backend compiling GLSL its pack
doesn't hold through glslang (done), and Dusklight built without its pack for the run that starts with nothing (done).

**On the console:**
- **Part 2** (`results/2026-10-14-milestone-13-part-2/`): vulkan4xenon_test draws the triangle, the per-vertex matrices
  and the bands again from SPIR-V the bundle can't match, its generator word changed. 89 passed, 0 failed; the four
  shaders compiled in 0.08 to 0.34 ms, and the triangle's image is the bundle's to the pixel.
- **Part 3** (`results/2026-10-14-milestone-13-part-3/`): the test names a folder for the layer's cache, emptied
  first. The four shaders compiled are kept in four files, and a second device, made after the first goes, reads the
  triangle's two back, compiles none and draws the bundle's image. 94 passed, 0 failed. With the renames left out on
  purpose, 92 passed and 2 failed: no file kept and the second device compiled both; with the reads left out, 93 passed
  and 1 failed.
- **Parts 4 and 5** (xsdk:docs/HARDWARE.md, "Shaders compiled as the game asks", its logs in
  xsdk:results/2026-10-14-shader-compiling-step3/; Aurora `953d207`, Dusklight `828f1f561d`): Aurora's backend reads a
  program the pack doesn't hold from the disk, else compiles its GLSL through glslang-xenon, and names the layer's
  folder beside its own. Without its pack (`DUSK_XSDK_GLSL_PACK=OFF`), played by the user from nothing kept through the
  title, the opening cutscenes and Ordon, Dusklight compiled 189 programs, each once: through glslang the first in
  919 ms with glslang starting and the rest in 23.9 ms on average, through xost 17.0 ms on average. The second time
  through compiled none and read 159 programs back. The 111 programs the title scene's runs kept are the pack's SPIR-V
  and containers, byte for byte. With the pack nothing was compiled, and the title scene and logo shot ran as before.
  vulkan4xenon_test passed 94 of 94 and vkbench 14 of 14.
- **Found on the way:** Aurora named an atomic write's temporary file in 44 characters for the memory card's, where
  FATX holds 42, so the card was never made on the console; Aurora `2ba57e8` names them in 21, and the second played
  run made it. Ordon's minimap drew as noise, and memory ran out there in the first played run: milestones 14 and 15.

### Milestone 14: the minimap, palette conversions of EFB copies

**Why.** The first play past the title on the console (2026-10-14, step 3's first run without the pack,
xsdk:docs/scope/shader-compiling.md) showed Ordon's minimap as noise, flickering in and out. Twilight Princess draws
its map into the EFB, copies it out as 8-bit indices, and draws the copy through a palette. Aurora turns such a copy
into an RGBA8 texture, `GX Dynamic Palette Texture`, by a pass through the palette (aurora-xenon:lib/gx/texture.cpp,
resolve_dynamic_palette_texture), and the vulkan4xenon backend skips that pass, logging "Palette conversions of EFB
copies aren't supported" once (aurora-xenon:lib/vulkan4xenon/encoding.cpp, encode_pass). The texture is never written,
and the GPU samples whatever its memory held. The game changes the map's palette often, and each change makes Aurora
drop the texture and make another (texture.cpp, get_tlut_texture), on other memory, so the noise changes; once the
console's memory ran out (milestone 15), 204 of them weren't made, and the map wasn't drawn on those frames.

**Agreed with the user (2026-10-14),** after milestone 15's measurement:
1. **The programs.** Aurora's GLSL generator writes the conversion's fragment programs, one for 8-bit indices and one
   for 4-bit, as the WebGPU backend's shaders do it (aurora-xenon:lib/gfx/tex_palette_conv.cpp): the copy's texel,
   sampled nearest, scaled to its index, then the palette's entry at that index. They join the fixed programs a
   recording writes (glsl::fixed_programs), so Dusklight's pack holds them, and the console compiles them otherwise
   (milestone 13). WebGPU's third variant, 16-bit integer indices, comes from no EFB copy (texture.cpp makes only the
   other two) and stays out.
2. **The backend.** A pipeline layout with one set of two samplers, the copy and the palette; a pipeline for each
   variant, drawing the full-screen triangle into the RGBA8 texture as the EFB copies' format conversions draw
   (encoding.cpp, draw_texture); and encode_pass running a pass's conversions before the pass, where the WebGPU
   backend runs them (aurora-xenon:lib/gfx/encoding.cpp, render).
3. **Getting there.** The diagnostics build's warp to Ordon (milestone 15) shows the map without anyone at the pad,
   and captures the frame.

**Found on the way (2026-10-14).** With the conversion in, the map still came out wrong, so the PC's Dawn build ran the
same warp beside the console's, logging each step. The copy's indices, the palette, the TEV registers and the uniforms
the GPU read all matched the PC's. What didn't: the map's areas, the draws with C0 at index 188, are GX lines, and the
GLSL generator refuses lines and points ("no GLSL yet for config ...: lines and points", three configs), so those
draws get no pipeline and are skipped. The map showed only what's drawn as triangles.

**Agreed with the user (2026-10-14), added:**

4. **Lines and points, as the WebGPU backend draws them** (aurora-xenon:lib/gx/shader.cpp:1042-1125): each segment
   and each point a quad in screen space, as wide as GX's line width or point size, in sixths of a pixel, scaled from
   the game's viewport to the one drawn into, its texture coordinates offset across it as `GXSetLineWidth`'s and
   `GXSetPointSize`'s texture offsets say (gx/shader_info.cpp:355-380). The layer draws lines and points 1 pixel wide
   (docs/SUPPORT.md, Pipelines) and xost has no instancing or storage buffers, so the backend's vertex decode writes
   each segment's or point's corners as two triangles, each corner with its end, the other end and which corner it is,
   and the GLSL vertex program moves the corners out from the ends' clip positions. The game draws them for the minimap
   and its paths (d_map.cpp, d_map_path.cpp, with `GX_TO_ONE` offsets), the effect lines (d_drawlist.cpp,
   `dDlst_effectLine_c`), 2D points (`dDlst_2DPoint_c`) and debug packets (m_Do_ext.cpp).

**Agreed with the user (2026-10-14), after lines and points:** the rest of what the game draws that the backend
refuses. Two more errors show in every console run past the title, and both need depth read out of eDRAM, which
neither xsdk nor the layer can do yet (xsdk:TODO.md, "Depth resolves"; docs/SUPPORT.md, "Depth stays in eDRAM"):
- "Depth copies (format 19) aren't supported": the depth of field copies the scene's depth as `GX_TF_Z16`
  (dusklight-xenon:src/m_Do/m_Do_graphic.cpp:1134), so the blur reads a texture never written;
- "GXPeekZ isn't supported": the sun's lens flare and the insects test their visibility against the depth
  (d_kankyo_rain.cpp:357, d_insect.cpp:96), and read nothing.

5. **Depth out of eDRAM.**
   - xsdk resolves depth into a texture, as a resolve with `RB_COPY_CONTROL`'s source 4 does
     (xenia:src/xenia/gpu/registers.h:856; xenos.h:925-933), checked by xsdk_test against depth it drew, and what a
     fetch of the resolved texture returns measured on the console and recorded in docs/HARDWARE.md.
   - The layer resolves a depth attachment into its image when the image is made to be sampled and a pass stores it
     and leaves it to be read, samples it, and leaves its words in the image's memory for the CPU, checked by
     vulkan4xenon_test against known depths.
   - Aurora's backend makes the `GX_TF_Z8` and `GX_TF_Z16` copies as the WebGPU backend's conversions do
     (aurora-xenon:lib/gfx/tex_copy_conv.cpp, FragZ8 and FragZ16), and answers `GXPeekZ` from a snapshot of the
     frame's depth, as gfx/depth_peek.cpp does.

**Agreed with the user (2026-10-14), after depth:**

6. **Anisotropic filtering.** Aurora asks for 16 times by default (aurora-xenon:lib/aurora.cpp:75-76) and the backend
   asks for none, as the layer has none (`maxSamplerAnisotropy` 1, src/sampler.c:46-47), though xsdk's samplers have
   had it since its milestone 33 (xsdk:SCOPE.md). The layer offers the `samplerAnisotropy` feature, up to 16, and
   passes a sampler's `maxAnisotropy` to xsdk's; the backend asks for Aurora's setting. vulkan4xenon_test checks it as
   xsdk_test does, stripes squeezed 8 to 1 keeping their contrast, and Dusklight's frame time in the title scene and
   Ordon is measured with it and without, for the user to settle the default.

**Changed on the way (2026-10-15),** steps 5 and 6 built together, as the user asked:
- **No copy into a buffer.** The layer's copies run on the CPU at replay and wait for the GPU (docs/SUPPORT.md,
  Commands), so one for each snapshot would stall the frame. The snapshot is drawn on the GPU instead, as
  gfx/depth_peek.cpp's compute pass makes it: GX's z of the depth under each texel of the GX framebuffer, packed into
  the red, green and blue of an RGBA8 image. Of two such slots, one is drawn only once the other's frame is done, and
  `GXPeekZ` reads its few words from the newest done, uncached memory a word at a time.
- **The resolve keys on the final layout, not the store op.** Aurora's passes store the EFB's depth for the next pass
  to load; resolving each would add a resolve the size of the colour's to every pass. So a pass resolves a sampled
  depth image only when it leaves it in a layout a shader samples in, and one that keeps it as an attachment leaves
  it in eDRAM alone (docs/SUPPORT.md, Render passes). Aurora's backend lays the EFB's depth out so: the pass before a
  depth copy, and the frame's last when a snapshot is due, leave it to be read, and the rest keep it.
- **A pass with depth of its own took the EFB's.** Milestone 4 placed every depth ending at the top of eDRAM, so
  Dusklight's bloom, a 960x360 pass with its own depth, took the 276 tiles holding the EFB depth's rows from 432 down,
  every frame from the title scene on; the passes after it loaded cleared depth there, and GXPeekZ read it, the lower
  half of the GX framebuffer far (`FFFFFF`) on the console and in Xenia alike. The layer now keeps up to four depths
  in eDRAM, a new one placed under one it holds when both fit above the pass's colour (docs/SUPPORT.md, Render
  passes); vulkan4xenon_test's sampled depth draws a smaller pass with depth of its own between the pass that draws
  the image's depth and the one that loads it.
- **xost lowers no component taken out of a vector into arithmetic,** only into a vector built of them
  (xsdk:src/xsc_lower.c, value_operand), so the depth programs work in vectors, `texture(...).rrrr`, and pick
  components only to build what they write.

**What else the backend refuses, and why it stays out.** Everything the backend refuses was checked against the game's
code and against every model, 2D screen and texture on its disc (a scan of its 2,095 archives: 3,626 J3D models with
13,371 materials, 132 2D screens, 19,493 textures). None of these is used:
- emboss texgens and binormal or tangent sources, and vertex formats with binormals: none on the disc, none in the code;
- more than xost's 10 interpolants: the most texgens a material has is 7, 10 with both colour channels and fog;
- a TEV stage sampling a texture with no coordinates, and logic ops: none in any material. The code's one logic op,
  the shadows' OR (d_drawlist.cpp:1574), is blending on the PC;
- destination alpha: only `JFWDisplay::clearEfb` sets it, and it returns at once on the PC
  (libs/JSystem/src/JFramework/JFWDisplay.cpp:486);
- polygon offset: only `GX2SetPolygonOffset` sets it, and the game never calls it;
- 16-bit palette indices (`GX_TF_C14X2` and the conversion's third variant): no texture on the disc, and no EFB copy;
- a sampler's minimum LOD above 0, its own anisotropy, or a maximum LOD short of the texture's last level: no texture
  on the disc sets one.

Two gaps outside GX were found on the way, each agreed (2026-10-14) as a milestone of its own after milestones 14 and
15: Dusklight's menus, milestone 16, and multisampling, milestone 17.

**Done when,** on the console:
- Ordon's minimap draws as on the PC's Dawn build, steady, and the log has no "Palette conversions" or "lines and
  points" line; broken on purpose, the index read one entry off, the map's colours come out wrong, and with the line
  width left out, its lines vanish, their quads having no area;
- with step 5: the log has no "Depth copies" or "GXPeekZ" line, Ordon's depth of field matches the PC's at the same
  warp, and the peeks the diagnostics build logs match the PC's; broken on purpose, the copy reading colour, the blur
  comes out wrong. vulkan4xenon_test finds each texel's depth above its stencil in the memory of a sampled depth image
  a pass left to be read, to its last texel, samples its depth in red, and finds the memory left alone by a pass that
  keeps it as an attachment; broken on purpose, resolving on the store op alone, that last check fails;
- with step 6: vulkan4xenon_test's stripes keep their contrast with anisotropy, and grey without it, and the frame
  times with and without are reported;
- vulkan4xenon_glsl_tests compile the new programs on the PC through glslang and xost, as the others;
- Dusklight's title scene and logo shot run as before, and vulkan4xenon_test, vkbench and xsdk_test pass.

**Left out:** palettes that don't come from EFB copies, which Aurora converts on the CPU; the 16-bit index variant;
the menus and multisampling, milestones 16 and 17.

**Steps 1 to 4 on the console** (2026-10-15; `results/2026-10-15-milestone-14/`; Aurora `a41820d`, Dusklight
`e5632c4f1a`, xsdk `2f8351d` and `0e8af8f`). Ordon's minimap draws as on the PC's Dawn build, the same warp from the
title scene (`minimap_pc_console.png`; `ordon_console.png`, `ordon_pc.png`), steady over five captures 20 s apart
(`minimap_5_captures.png`), and the log has no "Palette conversions" or "lines and points" line, only step 5's two
(`dusklight_ordon.log`). Broken on purpose (`breaks.png`): with the line width left out, the map's outlines are gone;
with each index read one entry off, Link's marker turns cyan, where the palette's entries, four alike in a row,
change. Three things were found on the way:
- **The game's map draws its areas' outlines as GX lines,** which the generator refused. Found by building Dusklight
  for Windows on Dawn and logging the map's draws there and on the console side by side: the copy's indices, the
  palette and the uniforms matched, and the draws at index 188 had no pipeline on the console. Hence step 4.
- **The corners' first decode froze the console:** they were written into the vertex cache, write-combined memory,
  through `memcpy`s of 8 and 12 bytes, which GCC makes 8-byte stores, and one at an odd word raised an alignment
  exception there. Xenia ran it. They're written in cached memory and copied over a word at a time now, as the decode
  writes (Aurora's lib/vulkan4xenon/decode.cpp).
- **The conversion came out as noise on the console and right in Xenia,** from the same containers: xost never set
  the serialize bit, and the ALU read the fetched index in its fetch's exec before the fetch had written it. xost now
  serializes what an exec reads across the ALU and its fetches (xsdk:docs/HARDWARE.md, "Serialized fetches"). It also
  lowered a vec2's `dot()` as a 4-wide one, which tools/check_glsl_math.py caught in the lines' corners.

On the PC, vulkan4xenon_glsl_tests passed 39 of 39 (`vulkan4xenon_glsl_tests.txt`), the corners' order broken on
purpose failing one (`vulkan4xenon_glsl_tests_corner_break.txt`), and their programs, 109, compiled through glslang and
xost; tools/check_glsl_math.py passed its 17 cases (`check_glsl_math.txt`), three new for lines and points, which fail
with the corners' sides swapped, the width unscaled or the mask ignored (`check_glsl_math_breaks.txt`). On the console
the title scene ran at 28 to 30 fps and the logo shot took 17.6 ms of GPU time a frame as before, its end frame the
same byte for byte; xsdk_test passed 300 of 300, vulkan4xenon_test 100 of 100 (with `test_thin_textures`, four strips
16 to 256 by 1, written while the noise was chased) and vkbench 14 of 14 (xsdk:results/2026-10-15-serialized-fetches/).
`disc_scan.txt` is the disc's scan behind "What else the backend refuses". Steps 5 and 6 are next.

**Steps 5 and 6 on the console** (2026-10-15; `results/2026-10-15-milestone-14-steps-5-6/`). The log has no "Depth
copies" or "GXPeekZ" line any more (`dusklight_aniso16.log`).
- **vulkan4xenon_test** passed 112 of 112 (`vulkan4xenon_test.log`). A sampled depth image's memory held each point's
  depth above its stencil word for word, to its last texel, with a smaller pass's depth drawn between the pass that
  drew it and the one that loaded it; its fetch returned the depth in red; and a pass keeping it as an attachment left
  its memory alone. The stripes kept a contrast of 1000 per mille with anisotropy 16 and greyed to 0 without. Broken on
  purpose in one build (`vulkan4xenon_test_breaks.log`), each break failed its own checks: every depth placed at the
  top again, the smaller pass's words (`0CCCCD5A`) in the image's last rows; the resolve keyed on the store op, the
  kept pass's memory overwritten; anisotropy left out of xsdk's sampler, the stripes greyed.
- **The depth of field** draws only while the player targets or an event focuses (dusklight-xenon:
  src/m_Do/m_Do_graphic.cpp, drawDepth2), so the diagnostics build forced it on at -200, within targeting's range, on
  the console and the PC alike (`diagnostic/`). The far houses blur and Link and the ground before him stay sharp on
  both (`depth_of_field_console_pc.png`): the top fifth's mean step in green fell from 5.17 to 3.21 on the console and
  6.52 to 3.55 on the PC, and the lower fifths didn't change (`diagnostic/sharpness.py`). Broken on purpose, the copy
  reading the pass's colour, the houses stayed sharp, 5.17 (`depth_of_field_break.png`).
- **GXPeekZ's** answers at a 5 by 4 grid over the GX framebuffer, 20, 40 and 60 s into Ordon, matched the PC's to
  within the frames' different framing, 1280x720 on the console and the PC's 1216x896 window: the grid's bottom row
  `FEEFB3 FEF642 FEFD14 FF036C FF09FB` on the console, `FEF338 FEF7DE FEFCAC FF0128 FF05CE` on the PC (`peeks.txt`).
  Before the placement fix (Changed on the way) the grid's lower half read `FFFFFF` on the console and in Xenia alike.
- **Anisotropy** changed nothing in Dusklight's frame times (`anisotropy_frame_times.txt`): the GPU took 18.18 to
  19.01 ms a frame in Ordon with Aurora's default of 16, and 18.08 to 18.78 with 1. Twilight Princess asks for none:
  no texture on its disc sets its own (step 4's `disc_scan.txt`), and Aurora only scales what a game asks for (aurora:
  lib/gfx/texture.cpp, anisotropy). Its default stays at 16, for games that ask.
- **The logo shot** took 17.9 ms of GPU time a frame, 0.3 ms more than before, for the depth copy the game makes every
  frame and the depth resolve before it: a copy more (11.0 to 12.0), a draw more and two resolves more. Its end frame
  was the same byte for byte (`dusklight_logo.log`).
- **On the PC,** vulkan4xenon_glsl_tests passed 40 of 40 (`vulkan4xenon_glsl_tests.txt`); tools/check_glsl_math.py
  passed its 20 cases (`check_glsl_math.txt`), three new for the depth copies and the snapshot, against WebGPU's
  gx_z24 in f32, which fail with the depth not reversed or a byte taken wrong (`check_glsl_math_breaks.txt`).
  Dusklight's pack holds the three new programs, 150 in all (`pack_dusklight.txt`); the 147 before are unchanged.
- **Elsewhere:** vkbench passed 14 of 14 (`vkbench.log`), its timings within about 1% of the run before. xsdk_test is
  recorded in xsdk:results/2026-10-15-depth-resolves/: it passed 305 of 305, its replay and pacing tests now reading
  their stats at the last flip, as a vblank after it had counted as missed in 2 of 3 runs (xsdk:docs/HARDWARE.md,
  "Depth resolves").

### Milestone 15: GPU memory in Ordon

**Why.** In the same run the console ran out of memory in Ordon (`dusklight_play.log`): free memory fell from 104 MB
in the title scene to 0, as the title's physical pages, its GPU memory, grew from 53 MB to 143 MB, the last 74 MB in
about 20 s of stages F_SP103 and F_SP104, while newlib's heap held at 280 MB. From then on GPU allocations failed.
What fills it isn't known, as the log counts pages, not what they hold. Candidates from the code, none measured: the
minimap's palette textures, 2.2 MB a pair, made anew at each palette change and freed only once the GPU is done with
the old ones (texture.cpp; aurora-xenon:lib/vulkan4xenon/device.cpp, retire); a pool's blocks, which go back to the
kernel only when empty (device.cpp, free_allocation), so a few live allocations can hold many; and Ordon's own
textures and EFB copies. Milestone 13's glslang keeps 13.5 MB of newlib's heap for the run, which counts against the
same memory.

**Agreed with the user (2026-10-14), in two steps,** the first before milestone 14:
1. **Measure.** The diagnostics build logs GPU memory by what holds it, every 10 s beside the memory line: each pool's
   blocks, bytes in use and largest free range; textures and buffers by label (EFB copies, converted and palette
   textures, render targets, the vertex cache, the frame pools); and what waits to be freed. It gets to Ordon by
   itself, warping to F_SP103 a minute into the title scene as Dusklight's `warp` command does
   (dusklight-xenon:src/dusk/commands.cpp), and stays two minutes, so a run needs no one at the pad; if a warp from
   the title doesn't hold, the user plays there instead. Only the diagnostics build warps. Done when a run in Ordon
   accounts for the physical pages the memory line counts, to within a few MB.
2. **Fix what the numbers show,** scoped with the user once they're in, without cutting resolution or texture quality.

**Left out:** memory past Ordon, until a run gets there.

**Step 1 on the console** (2026-10-14; `results/2026-10-14-milestone-15-step-1/`). The diagnostics build, Dusklight
with its pack and Aurora's diagnostics, counted every `xsdk_gpu_alloc` by caller through the linker's `--wrap`
(`callers.txt`), and Aurora's GPU memory by label and pool. It warped a minute into the title scene to F_SP103's room
1, outside Link's house, and stayed two minutes (`dusklight_mem.log`). It landed in a dialogue, so the minimap was
hidden, though the game still drew it offscreen (`ordon.png`). The physical pages went from 53 MB in the title scene
to 93 MB in Ordon, flat for the last 100 s, with 61 MB free. At the end they held:
- **78.1 MB through `vkAllocateMemory`, Aurora's pools:** eight 8 MB blocks, with 12 to 20 MB free inside them, and
  dedicated buffers: the vertex cache 8 MB, the frames' uniforms 4 MB and indices 2 MB. In the blocks: the minimap's
  palette textures, `GX Dynamic Palette Texture`, 35 to 49 live at once, 16 to 23 MB, none ever written (milestone
  14); the game's textures, 169 of them, 14.4 MB; 12 EFB copies, 6.3 MB; the EFB, 3.6 MB; offscreen targets, 2.4 MB.
- **11.7 MB of xsdk's:** the two display buffers, 7.25 MB (xsdk:src/present.c, xsdk_present_open); the ring, writeback
  and stream, 4.1 MB (xsdk:src/gpu.c); 0.3 MB of shader microcode pages.
- **3 to 4 MB outside xsdk's GPU allocations,** the same from boot on.

Aurora's texture uploads, 16 MB, sit in the cached memory type, which is newlib's heap, not physical pages. So the
physical pages are accounted for to within the 3 to 4 MB there from boot, and step 1 is done. What the run doesn't
show is the played run's 143 MB: here Link stood still in a dialogue. Aurora keeps a palette texture for each EFB
copy it converts until 600 frames go by unused (aurora-xenon:lib/gx/texture.cpp, sweep_object_caches), so 35 to 49 at
once means the copies behind the map keep changing, and a moving player may well change them faster.

### Milestone 16: Dusklight's menus and overlays

**Agreed with the user (2026-10-14)** as a milestone after 14 and 15. **Its scope, below, agreed on 2026-10-21**
with every recommendation in it. It takes in ImGui through vulkan4xenon too, which xsdk milestone 57 left to this
layer (xsdk:docs/scope/57-text-and-ui.md, Left out).

**Why.**
- **The console's backend draws neither of Aurora's two UIs.** It skips every RmlUi draw
  (aurora-xenon:lib/vulkan4xenon/encoding.cpp:1082-1083, "recorded only on Dawn") and drops ImGui's frozen draw data
  (encoding.cpp:1281-1282, `(void)overlay`). RmlUi is drawn only when Aurora is built with Dawn
  (aurora-xenon:CMakeLists.txt:50-55). On the console a null render interface hands out handles
  (aurora-xenon:lib/rmlui/RmlUi_Backend_Aurora.cpp:14-33), and the documents get `Context::Update` every frame but
  never `Render` (aurora-xenon:lib/rmlui.cpp:518-525; lib/gfx/frame.cpp:229-235).
- **Back opens a menu nobody can see, and it takes the controller.** Back maps to F1 unless the menu has a binding of
  its own (dusklight-xenon:src/dusk/ui/input.cpp:345-349). F1 is the menu command (src/dusk/ui/ui.cpp:646-647), and
  that command toggles the menu bar (src/dusk/ui/document.cpp:215-224). The preset window had the same problem and is
  kept closed on the console (dusklight-xenon `43b8f65635`; src/m_Do/m_Do_main.cpp:1014-1022; milestone 10).
- **The beta's testers need the settings and the overlays.** The FPS counter is an element of RmlUi's overlay
  document (src/dusk/ui/overlay.cpp:41, 273-310) and is set under Video, "Show FPS Counter"
  (src/dusk/ui/settings.cpp:721-770), so it arrives with RmlUi. Dusklight's ImGui tools (src/dusk/imgui/) already
  run every frame on the console, since `TARGET_PC=1` is set there too (src/m_Do/m_Do_graphic.cpp:2188-2190;
  cmake/GameABIConfig.cmake:7). Aurora renders and clones their lists each frame (aurora-xenon:lib/aurora.cpp:191), and
  then nothing draws them.
- **No keyboard reaches them.** ImGui's menu bar opens with Shift+F1 (src/dusk/imgui/ImGuiConsole.cpp:283-299), and
  the overlays open with F2 to F10 (src/dusk/imgui/ImGuiMenuTools.cpp:139-145; ImGuiHeapOverlay.cpp:27-28;
  ImGuiCameraOverlay.cpp:16-17). The console's backend passes ImGui no input at all
  (aurora-xenon:lib/vulkan4xenon/backend.cpp:556), so a player with only a controller needs an option in the menu.

**What Dusklight's menus ask of RmlUi's render interface,** from res/rml/*.rcss and the documents in
src/dusk/ui/*.cpp, checked against encounter's RmlUi at `f00a0fe` (aurora-xenon:extern/CMakeLists.txt:350-351) and
Dawn's renderer (aurora-xenon:lib/rmlui/WebGPURenderInterface.cpp, 1,868 lines, and pipeline.cpp, 1,054):

| Feature | Where Dusklight uses it | Dawn | Here |
|---|---|---|---|
| Geometry, coloured and textured | Everything. Text, the `text()` icons (global.rcss:70-119) and `horizontal-`/`vertical-gradient`, which RmlUi builds as vertex colours (rmlui:Source/Core/DecoratorGradient.cpp:143-166; tabbing.rcss:24-44) | `DrawGeometry`, :418-454, pushes vertices and 32-bit indices on every draw | Step 2 |
| Textures | Font atlases, up to 1024x1024 each (rmlui:Source/Core/FontEngineDefault/FontFaceLayer.cpp:100), PNGs and the game's item icons (src/dusk/ui/icon_provider.cpp:753-755) | :517-600, premultiplied RGBA8 | Step 2 |
| Scissor | Every scrolled pane and list | :602-643 | Step 2 |
| Transforms | Windows and toasts scale in (window.rcss:30, 52; overlay.rcss:43, 52), the menu bar slides in (popup.rcss:27-32), `translateX(-50%)` (overlay.rcss:187-208), rotations (controls.rcss:118-122) | :700-706 | Step 2 |
| Clip masks | `overflow: hidden` with `border-radius`, or under a transform, which turns the scissor off (rmlui:Source/Core/ElementUtilities.cpp:196-218): every window (window.rcss:22-32). Every box-shadow's cut-out too (below) | Stencil, :645-698 | Step 2 |
| Layers, saved as textures | Every `box-shadow`. RmlUi draws the element's background and border into the shadow's texture, which it makes in a layer (rmlui:Source/Core/ElementBackgroundBorder.cpp:166-172; GeometryBoxShadow.cpp:157-243). That covers every button and select button (controls.rcss:1-36), and windows, toasts and popovers. Without layers they have no background | :1312-1486 | Step 2 |
| `filter: opacity` | The 0.2 s fades of windows, toasts and popovers (window.rcss:29-32; overlay.rcss:42-52; popover.rcss:22-30). Values of 0 and 1 push no layer (rmlui:Source/Core/FilterBasic.cpp:40-50) | Colour matrix, :1539-1548 | Step 2 (Decisions, 4) |
| Blur | `backdrop-filter` on windows, the menu bar, popovers, toasts and a close button (window.rcss:26; popup.rcss:26; popover.rcss:20; overlay.rcss:40; tabbing.rcss:78). Box-shadows with a blur radius (window.rcss:27, 25dp; overlay.rcss:41; popover.rcss:21, 87) | `RenderBlur`, :941-1061 | Left out (Decisions, 3) |
| `drop-shadow`, `grayscale`, `brightness` | The autosave icon (overlay.rcss:333), the mods' package rows (window.rcss:451) and the touch controls (touch_controls.rcss:59) | :1062-1239 | Left out |
| Gradient shaders | An empty colour swatch and the hue slider (global.rcss:221; popover.rcss:76; src/dusk/ui/color_input.cpp:279) | :1642-1747 | Left out |
| `image-effects` | The mods' images (src/dusk/ui/mods_window.cpp:126, 228). Registered on Dawn alone (aurora-xenon:lib/rmlui.cpp:393) | ImageEffects.cpp | Left out |
| Glass, mask images | Not used: no match in res/rml or src/dusk/ui | GlassFilter.cpp | Nothing |

So the core is a short list: geometry, textures, scissor, transforms, clip masks and layers. Layers are needed
because buttons draw their backgrounds through them, not only for effects.

**What vulkan4xenon gives them, all in place already:**
- **Stencil** with a `D24_UNORM_S8_UINT` attachment, covering every op, reference and mask (docs/SUPPORT.md,
  Pipelines; milestone 26).
- **Render to texture** into `R8G8B8A8_UNORM` images, which can be sampled, blended into and used as colour
  attachments (Formats; milestone 4).
- **A 1280x720 colour target with a D24S8 depth target at one sample fits eDRAM untiled.** Depth and stencil stay in
  eDRAM from one pass to the next unless another target reaches their tiles (Render passes).
- **Dynamic scissor; every blend factor but the dual-source ones; colour write masks** (Pipelines).
- **The vertex formats both UIs need:** `R32G32_SFLOAT`; `R8G8B8A8_UNORM`, read by address; and
  `A8B8G8R8_UNORM_PACK32`, read as a packed word (Formats). Indices of 16 or 32 bits (Commands). Push constants up to
  128 bytes (Pipelines).
- **Shaders:** `vkCreateShaderModule` finds a container in the app's bundle by its SPIR-V. When the bundle lacks one,
  the layer compiles it on the console once and keeps it (Shaders; xsdk:docs/scope/shader-compiling.md).
- **About 1 us of CPU a draw through the layer:** 0.966 us before milestone 29, which brought the submit down to 0.575
  (milestone 29).

**How a 2D program joins Aurora's backend.**
- **How programs are found now.** GX pipelines get GLSL from the generator (aurora-xenon:lib/vulkan4xenon/
  pipeline.cpp, `make_pipeline`). `find_program` then looks for that GLSL's SPIR-V in three places
  (lib/vulkan4xenon/program.cpp:146-194): first the pack, keyed by XXH3 of the source (glsl.cpp:1648-1650); then
  SPIR-V kept on the disk; else glslang compiles it on the console and keeps it.
- **The backend's own programs** are listed in `glsl::fixed_programs()`: clears, copies, the present, palettes and
  GXPeekZ (glsl.hpp:113-130; glsl.cpp:1627-1646). The null backend's recorder writes them out with the first config
  (glsl.hpp:136-138). tools/build_glsl_pack.py compiles the recording into dusklight-xenon:platforms/xenon/glsl_pack.c
  (dusklight-xenon:CMakeLists.txt:76-81).
- **The UI's two programs join that list.** The next pack then holds them beside the 150 before, unchanged, as
  `66e5621140` did for its three. Until that pack is built, the console compiles them once.

**ImGui on the console today.**
- Aurora builds ImGui 1.91.9b (aurora-xenon:extern/CMakeLists.txt:229-251). The backend builds its font atlas and
  sets only the display size (backend.cpp:558-566), so `io.DeltaTime` keeps ImGui's default of 1/60
  (imgui:imgui.cpp:1460). As a result, the Debug Overlay's "FPS" would always read 60.
- `aurora_imgui_add_texture` returns no texture (backend.cpp:593-598). Dusklight loads its logos through it
  (src/dusk/imgui/ImGuiEngine.cpp:44, 221-228).
- Dusklight's ImGui fonts are Inter at 18, 26 and 40 px and Noto Mono at 16 (ImGuiEngine.cpp:90-104). With ImGui's
  Latin-1 glyphs that is likely a 512x1024 atlas, 2 MB as RGBA32. This is an estimate, and step 1 logs the real size.
- On the PC, the tools open from ImGui's menu bar or with F keys (ImGuiMenuTools.cpp:117-125), and only with
  "Enable Advanced Settings" on (settings.cpp:1608-1617; ImGuiMenuTools.cpp:140-141). The Input Viewer is ImGui with
  an option of its own, "Show Input Viewer" (settings.cpp:1618-1622; ImGuiControllerOverlay.cpp:13).
- **Which windows work without input.** The Debug Overlay, Player Info and Camera Debug show information only, with
  `ImGuiWindowFlags_NoNav` (ImGuiMenuTools.cpp:149-152, 214-217; ImGuiCameraOverlay.cpp:30). Heaps is readable but
  has buttons. The rest are tools that need input to use: Processes, Audio Debug, the Save Editor, State Share, the
  Actor Spawner, Bloom and the Stub Log.
- **Byte order.** ImGui writes its colours and atlas texels as words, which IM_COL32 builds as 0xAABBGGRR
  (imgui:imgui.h:2882-2889). On this big-endian CPU their bytes are therefore A, B, G, R. imgui_impl_vulkan reads both
  as `R8G8B8A8_UNORM` (imgui:backends/imgui_impl_vulkan.cpp:712-721, 936), so used unchanged it would take alpha for
  red. That corrects milestone 57's "needs no change". RmlUi's colours are bytes in R, G, B, A order
  (rmlui:Include/RmlUi/Core/Vertex.h:12-18), so they read correctly as `R8G8B8A8_UNORM`.

**The design.** RmlUi and ImGui share one program pair: the position goes through a matrix, and the fragment colour
is the vertex colour times the texture. They use different pipeline states over that pair. Both draw into a
1280x720 UI image, which is composited premultiplied over the EFB at the present, as Dawn's present composites
RmlUi's target (aurora-xenon:lib/webgpu/present.cpp:209-221). RmlUi records on the game thread into a list. That
list goes to the render worker with the frame, as ImGui's frozen data does. It doesn't take Dawn's route, which
drains the FIFO on the game thread before RmlUi records (lib/aurora.cpp:197-200), because the game thread is what
holds Ordon at 28 fps.

**What it takes.**
1. **Measurement first.** In the diagnostics build, a counting render interface replaces the null one, and
   `Context::Render` runs after `Update`, still drawing nothing. A hook in dusklight-diag.patch, like milestone 44's,
   steps through the title scene's counted frames:
   1. menu closed, nothing shown;
   2. FPS counter on;
   3. a toast;
   4. the menu open on Settings, at the Interface tab;
   5. every tab of the menu once.

   For each stage the log gives:
   - `Update` and `Render` time a frame on the game thread;
   - draws, vertex bytes and index bytes a frame;
   - scissor, transform and clip-mask calls;
   - layers pushed and the deepest stack;
   - textures saved from layers, and textures generated, with their sizes;
   - filters and shaders asked for, by name;
   - ImGui's atlas size, and its lists' vertices with the Debug Overlay on.

   If `Render` costs more than 0.1 ms a frame with the menu closed, or the memory sum passes the budget
   (Decisions, 6), the numbers come back to you before step 2.
2. **The shared 2D path in Aurora's backend** (aurora-xenon, lib/vulkan4xenon and a console render interface in
   lib/rmlui):
   - **Programs:** `ui_vertex` and `ui_fragment` in glsl.cpp, added to `fixed_programs()`. There is no gamma, as
     Dawn applies it only to sRGB targets (WebGPURenderInterface.cpp:1752-1780). Push constants hold a 64-byte matrix
     and the translation.
   - **Pipelines over that pair**, from pipeline.cpp's `create_pipeline` with stencil state added to `PipelineDesc`:
     - RmlUi's draw, with stencil off and with stencil equal. Vertices are 20 bytes: position, then colour as
       `R8G8B8A8_UNORM` at 8, then UV at 12. Blending is premultiplied (one, one minus source alpha).
     - Clip replace and clip intersect, with colour writes off and the references Dawn uses (:652-698).
     - Layer composite, blended or replacing (`Rml::BlendMode`).
     - ImGui's draw. Colour is `A8B8G8R8_UNORM_PACK32` at 16. Colour blends as source alpha over one minus source
       alpha, and alpha as one over one minus source alpha (imgui_impl_vulkan.cpp:968-972), which leaves
       premultiplied colour in the UI image.
     - The present's composite, from the existing copy programs (pipeline.cpp:398-405) with premultiplied blending.
   - **Targets.** The UI image is `R8G8B8A8_UNORM`, with a D24S8 for clip masks and layer images of the same size.
     Each is made at first use and kept. One render pass covers colour plus D24S8, to load or clear.
   - **RmlUi's interface, on the game thread, makes no Vulkan calls.**
     - It records draws, scissors, transforms, clip operations, layer pushes, composites, pops and saves into a list.
     - Geometry stays on the CPU from `CompileGeometry` and is held by the frame's list until the render worker has
       copied it into a ring for the frame slot, as Dawn copies it on every draw.
     - New textures' bytes go through `stage_upload` (lib/gfx/backend.hpp:81-83). `LoadTexture` works as Dawn's does:
       SDL's PNG reader, premultiplied, and runtime textures.
     - `SaveLayerAsTexture` copies the scissor region with the existing copy draw (encoding.cpp:404-410).
     - `CompileFilter` handles "opacity", folded into the composite's colour. "blur" returns no filter, so
       box-shadows are drawn without blur (GeometryBoxShadow.cpp:194-200).
     - The "blur" and "drop-shadow" filter instancers are replaced by identity ones on the console, so they push no
       layer (rmlui:Source/Core/Factory.cpp:471-475).
     - `CompileShader` returns no shader.
     - The list's vectors are cleared between frames rather than freed (CODE_GUIDELINES.md 9).
   - **ImGui:**
     - `new_frame` sets `DeltaTime` from the frame clock.
     - The atlas is uploaded once, its words written out as bytes in R, G, B, A order, and the CPU's copy is freed.
     - `aurora_imgui_add_texture` makes a texture.
     - The lists are copied into the ring, each command's indices starting on 4 bytes, the only alignment the console
       has drawn index lists from (xsdk:docs/GUIDE.md:93-95). Each command's clip rectangle becomes the scissor.
   - **`end_frame`:**
     - If anything was drawn: the uploads, then RmlUi's passes, then ImGui into the base pass. The present then draws
       the EFB and the UI image over it.
     - If nothing was drawn: no pass and no composite.
     - The UI is drawn whether or not a swapchain image was acquired, so Xenia's captures show it.
     - `backend::end_frame` takes RmlUi's list beside ImGui's (backend.hpp:160), and the Dawn and null backends'
       signatures change to match.
   - **Captures:** `write_capture` (encoding.cpp:197-221) blends the UI image over the EFB on the CPU whenever the UI
     drew. A diagnostics call also writes the UI image alone, with alpha.
3. **Dusklight's side** (dusklight-xenon):
   - **A "Debug Overlays" option** in the Interface tab, under `#ifdef __XSDK__`, after "Show Input Viewer":
     - It is disabled unless advanced settings are on, as the tools are.
     - It has toggles for Debug Overlay, Player Info, Camera Debug and Heaps, through four accessors on
       ImGuiMenuTools' private flags (ImGuiMenuTools.hpp:28-55).
     - The toggles aren't saved, as on the PC (ImGuiMenuTools.cpp:197).
   - **Help text.** Under the same `#if`, the help text for "Enable Advanced Settings" names that option instead of
     Shift+F1.
   - **The pack is rebuilt** with the two new programs.
   - **The FPS counter and Input Viewer options work as they are.**

**xsdk and vulkan4xenon need nothing new.** If step 1 or the runs find a gap in the layer, it's scoped with you then.

**New checks.**
- **Aurora's host tests** (local/m44c/aurora_host_tests.sh's way: the null backend on the PC):
  - the recorder:
    - clip Set, SetInverse and Intersect, with the stencil references and pipelines Dawn uses, and the limit of 255
      nested clips;
    - the scissor clamped to the target;
    - a transformed and translated point landing where Dawn puts it;
    - push, composite, pop and save, each touching the right images;
    - opacity in the composite's colour;
    - blur and drop-shadow pushing nothing;
  - ImGui:
    - the atlas's words becoming FF FF FF a on any host;
    - a command at an odd index offset copied onto 4 bytes;
    - an empty clip rectangle skipped;
  - the two programs present in `fixed_programs()`, checked by text as `DepthPrograms` checks its programs;
  - the capture's arithmetic: a premultiplied texel over an EFB texel.
- **On the console, in the diagnostics build, run by the agent.** At the title scene's counted frames the hook turns
  on the FPS counter, advanced settings, the Debug Overlay and a toast. It shows the menu notification, then opens
  the menu as Back does and goes to Settings, Interface. It captures the composite and the UI image a second after
  each change, then closes the menu. Each check reads element boxes from RmlUi and ImGui, and probes only flat
  interiors, as Xenos's linear filter isn't exact at texel centres (xsdk:docs/HARDWARE.md, "Milestone 57 on the
  console"):
  - `ui_fps`: inside the FPS box, its background is `rgba(surface, 80%)` premultiplied, within 2/255, and the text
    box holds glyph pixels.
  - `ui_focus_ring`: one pixel outside the focused button is the accent colour.
  - `ui_focus_inside`: inside it, alpha is the button's own, as `opacity: 0.9` gives it (controls.rcss:5), and not 1.
  - `ui_transform`: the notification's centre is within a pixel of x = 640.
  - `ui_scissor`: with the settings pane scrolled 200 px, the window rows above it are unchanged.
  - `imgui_bg`: inside the Debug Overlay, the window background at 0.65 alpha, premultiplied.
  - `imgui_text`: 10% to 90% of the overlay's first text line is glyph.
  - `composite`: across the FPS box, composite = EFB x (1 - a) + UI.
  - `ui_idle`: with the menu closed and nothing shown, the profile's passes a present equal the control build's.
- Every check is a log line and takes under a second.

**Broken on purpose** (`results/<date>-milestone-32/breaks.py` and `builds.sh`, as milestone 30's), each failing the
checks named:
- `no_clip_mask`: `ui_focus_inside`.
- `no_save_layer` (`SaveLayerAsTexture` saves nothing): `ui_focus_ring` and `ui_focus_inside`.
- `no_transform`: `ui_transform`.
- `no_scissor`: `ui_scissor`.
- `imgui_byte_colour` (ImGui's colour as `R8G8B8A8_UNORM`): `imgui_bg`.
- `imgui_atlas_words` (the atlas uploaded unswapped): `imgui_text`.
- `straight_composite`: `composite`.
- `always_draw` (the UI pass and composite run every frame): `ui_idle`.

Each host check is also broken once in its own test.

**Verification:**
- Aurora's host tests pass, and each of their breaks fails.
- The diagnostics build passes every check twice on the console, and each break fails the checks it names and no
  others. The log gives:
  - CPU time a frame on the game thread (`Update`, `Render` and the recording) and on the render worker (the UI's
    replay), in each state;
  - the first menu open's frame time;
  - the UI's GPU memory by kind (image, D24S8, layers, textures, ring), after every tab has opened once.
- Plain builds with and without this milestone, against the same xsdk and layer, in the same session:
  - the title scene in five or more 10 s windows, menu closed, nothing shown;
  - Ordon's village (F_SP103) with the same, warped to by the agent as milestone 15's build does, or played by you if
    you prefer your own route;
  - an option changed in the menu still set after a relaunch.
- A Xenia smoke run of the diagnostics build with `--readback_resolve=full`: no crash, and its captures written.

**Done when:**
- The console's captures show the menu, a toast, the FPS counter, the four ImGui overlays and the Input Viewer.
- Every check passes twice, each break fails what it names, and the host tests pass.
- With the menu closed and nothing shown:
  - no UI pass is recorded;
  - RmlUi costs the game thread at most 0.1 ms a frame more than `Update` alone;
  - the title scene and Ordon run within the control build's noise.
- With only the FPS counter on, the overlay costs at most 0.2 ms of CPU a frame, game thread and render worker
  together.
- The CPU cost of an open menu, and the UI's memory, are recorded, and the memory is within the budget (Decisions, 6).

**Left out:**
- **Blur, whether backdrop, filter or a box-shadow's.** Shadows come out hard-edged: a window's
  `0 0 25dp 5dp` (window.rcss:27) draws a 5dp ring, opaque black, as RmlUi's default shadow colour is. The 80% to 90%
  backgrounds show the scene behind them unblurred.
- **Drop shadows, colour-matrix filters, gradient shaders, image effects, glass and mask images** (the table above).
  Each of these is drawn as nothing, or as identity.
- **ImGui's interactive tools and its menu bar.** Navigating them would share the controller with the game
  (Decisions, 5).
- **The TV's safe area, touch controls, and the prelaunch screen**, which the console skips
  (src/m_Do/m_Do_main.cpp:804-808).

**Decisions,** each with a recommendation:
1. **Measure first, or build straight away.** Step 1 is about 150 lines and one run by the agent, and it answers the
   question that would sink the design: whether `Render` costs a CPU-bound game too much with the menu closed.
   Recommended: measure first.
2. **Where the UI draws.** Into its own image, composited at the present (recommended): layers stay uniform, captures
   and Xenia show the UI, and the present pass keeps no stencil. The cost is 3.6 MB, and when anything is shown,
   about 0.3 ms of GPU for the image's resolve and the composite (a 720p resolve takes 270 us,
   xsdk:docs/HARDWARE.md, "Multisampled formats and targets, milestone 44 step 3"). The alternative draws straight
   into the swapchain image. That saves those, but a layer pushed mid-frame would need the swapchain image loaded
   back, and captures would need another path.
3. **Blur.** Recommended: none for the beta, with the hard shadows above. Next, if you want it: blur for box-shadow
   textures alone, made once a texture, for about 150 lines and a program. Dawn's full blur, backdrops included, runs
   several passes every frame a window is open and isn't planned.
4. **Fades.** Layers (recommended): the same code box-shadows need, used 0.2 s at a time, at the cost of one more
   3.6 MB image. The alternative is an identity "opacity" instancer, which turns fades into cuts and saves that
   image.
5. **ImGui's tools from a controller.** The four display-only windows, toggled from the menu (recommended). The
   alternative is gamepad navigation of ImGui's menu bar, which needs a mode that keeps the controller from the game:
   a milestone of its own.
6. **Memory.** Targets come to about 14 MB with fades (image, D24S8, two layers), or 11 MB without, plus the textures
   step 1 counts, against about 50 MB free in Ordon (your figure). Recommended: a 20 MB budget, everything kept once
   made (stability over latency). The alternatives are freeing layers and shadow textures some frames after the UI
   goes idle, or reusing the EFB's one-sample read depth (encoding.cpp, `efbDepthRead`) as the stencil, which saves
   3.6 MB but ties the two together.
7. **The preset window** stays closed on the console (recommended): its presets ask for 1536x1536 shadow targets,
   past the layer's 1280x720 (milestone 10).

**Size:** medium.
- **Aurora:** about 1,300 lines. The recorder is about 450, the backend's passes, pipelines, textures, ring,
  composite and capture about 450, the programs 40, ImGui 120 and the host tests 250. Dawn's renderer is 2,900.
- **Dusklight:** about 80 lines committed and the pack rebuilt; the diagnostics hook, about 250, stays in local/.
- **Console:** step 1's run, then two runs with their breaks and the Ordon comparison, all by the agent.

**Risks:**
- **`Render` is new work on the critical thread.** Step 1 prices it before any GPU work.
- **Each focus move makes a box-shadow texture.** The background changes with the shadow, so each move costs a layer
  pass and a copy, about two 720p resolves of GPU. The first open makes every one at once, which may hitch a frame or
  two. If the logged first-open time is long, the layer pass can resolve only the scissor region, as render areas
  allow (docs/SUPPORT.md, Render passes; milestone 27).
- **Big-endian data.** ImGui's words against RmlUi's bytes (above), and the item icons converted from game textures
  (src/dusk/ui/icon_provider.cpp). The byte-order checks catch the first two; the icons show only in advanced tabs.
- **A clip mask lost between passes.** That would happen if another target took the stencil's tiles, and it would
  show as `ui_focus_inside` failing.
- **The first open rasterises glyphs** with FreeType, which may hitch.

**Shipped in Dusklight's beta 1 on 2026-10-22, by the user's choice, but not closed** (the runs' logs are in
`xsdk:local/beta/v16`, outside the repository):
- Run 3, before the review's fixes, passed every check twice, each break failed what it names, and the title used the
  memory the control build did.
- After the review's fixes and the memory the user chose (aurora-xenon `ca4e8c2`: RmlUi's textures kept, the UI's
  image, layers and rings released once nothing shows), run 5's first pass passed 10 checks of 11. Its first `ui_idle`
  failed because the config saved on the console had the FPS counter on from the start, so the UI drew from the boot.
  `ui_released` passed: the image, layers and rings went from 11.6 MB to 0 once nothing showed.
- Run 5's second pass stopped 17.7 s into the title scene, after the toast's check, with nothing in the log. That
  isn't explained. The user then played the beta with a freeze logger in it and saw no freeze. The freeze the user
  hit before came with Unlock Framerate on (milestone 34's closing note).
- Pushed: aurora-xenon `a526001`, dusklight-xenon `e3a303e56a`.

To close it: both passes clean, from a config with the defaults.

### Milestone 17: multisampling

**Why.** Aurora draws with Dusklight's anti-aliasing setting on the PC, and the backend draws with one sample whatever
it's set to (aurora-xenon:lib/vulkan4xenon/backend.cpp:401-402). xsdk has had 2- and 4-sample targets since its
milestone 32 (xsdk:SCOPE.md), but at 1280x720 a 2-sample colour and depth target needs 14 MB, past eDRAM's 10 MB, so
the frame has to be drawn in tiles, as Xenos's predicated tiling does (xsdk:SCOPE.md, "predicated tiling").

**Agreed with the user (2026-10-14)** as a milestone after 14 and 15; its scope is settled when it starts.

**Scope (agreed 2026-10-17):** xsdk:docs/scope/44-tiling-and-msaa.md, its vulkan4xenon part, in two: render passes past
eDRAM at one sample once xsdk milestone 44's step 2 is done, then multisampling after its steps 3 and 4.

**The first part, render passes past eDRAM at one sample** (built 2026-10-18; on the console 2026-10-18, `c33a664`,
`results/2026-10-18-milestone-17-part-1/`: vulkan4xenon_test 439 of 439 twice, each of five break sets failing the
checks it names and no others; xsdk:docs/HARDWARE.md, "Render passes past eDRAM in the layer, milestone 44"):
- A subpass past eDRAM's tiles is xsdk's tiled pass: its commands are replayed once into a recording each band runs,
  its loads drawn back at the recording's start, its stores each band's resolves, its depth whole at eDRAM's top where
  it fits, else in bands, stored into a sampled depth image's texture and drawn back through the pixel shader, its
  stencil lost (docs/SUPPORT.md, Render passes). Framebuffers go to 4096x4096. Queries, time stamps and render areas
  smaller than the framebuffer are refused in such a pass.
- A fragment shader's constants stay below c252, which xsdk keeps for `gl_FragCoord`: such a pipeline is refused, and
  `maxUniformBufferRange` is 4032.
- xsdk takes three additions for it (xsdk:include/xsdk/gpu.h, tiled passes): a resolve the recording draws back is left
  out of band 0's clear (`drawn_back`); depth whole sits at eDRAM's top, where the layer's untiled passes keep it
  (`depth_top`); and band 0's clear is skipped only where the last pass cleared each target to the same.
- **New test:** tests/test_tiles.c. **Broken on purpose:** `results/2026-10-18-milestone-17-part-1/breaks.py` names
  each break and the checks it should fail, and `builds.sh` builds them.
- **Not in it:** multisampled images, resolve attachments, alpha to coverage, Aurora and Dusklight, the second part;
  subpasses without colour attachments, still refused.

**The second part, multisampling** (the layer's part of xsdk milestone 44's step 5; built and run on the console
2026-10-20, `results/2026-10-20-milestone-17-part-2/`, and xsdk:results/2026-10-20-milestone-44-step-5/:
vulkan4xenon_test 517 of 517 twice, each break failing the checks it names; done with xsdk's milestone 44):
- **Images:** `R8G8B8A8_UNORM`, `B8G8R8A8_UNORM`, `A2B10G10R10_UNORM_PACK32` and D24S8 at 2 and 4 samples, the
  formats whose targets averaged on resolve on the console; the half and 32-bit floats take one, as their `sampleCounts`
  say (xsdk:docs/HARDWARE.md, "Multisampled formats and targets, milestone 44 step 3"). The framebuffer sample counts
  are 1, 2 and 4, the sampled ones 1, as Xenos samples no multisampled texture, and `standardSampleLocations` is false.
  A multisampled image is drawn into and resolved from, never sampled; its memory holds a texture at one sample.
- **Pipelines:** `rasterizationSamples` at the subpass's count; `alphaToCoverageEnable` through
  `XsdkBlend.alpha_to_coverage`, n of N samples from alpha (4n - 2) / 4N (xsdk:docs/HARDWARE.md, "Alpha to coverage,
  milestone 44 step 4"); sample masks other than all ones, alpha to one and sample shading refused.
- **Render passes:** a resolve attachment takes its colour's average at the subpass's end; storing a multisampled
  attachment stores the average into its image's memory and loading it draws that average back into every sample, a
  deviation (docs/SUPPORT.md, Render passes); `vkCmdResolveImage` copies that stored average. Render areas smaller than
  the framebuffer stay at one sample.
- **Past eDRAM at 2 and 4 samples** as xsdk's tiled passes at that count: at 1280x720 one colour attachment and depth
  take 3 bands, depth whole at 2 samples and in bands at 4, where a multisampled depth image keeps sample 0's depth in
  its texture and a pass loading it draws it back as one depth a pixel. A stored attachment with a resolve attachment is
  two resolves of one target a band, so xsdk clears a target resolved twice at its last resolve
  (xsdk:include/xsdk/gpu.h, `xsdk_gpu_end_tiles`).
- **New test:** tests/test_multisample.c: a triangle's edges at 2 and 4 samples in both colour formats that take them,
  through a resolve attachment, counted as xsdk_test counts them; `vkCmdResolveImage` of each stored attachment; a pass
  loading each, drawn back; alpha to coverage's steps at 1, 2 and 4 samples; the formats' sample counts, the limits
  and the refusals. **Extended:** tests/test_tiles.c, passes at 1280x720 at 2 samples, depth whole, and at 4, depth in
  bands, each clearing colour and depth and a second loading both, every texel of the resolve attachment checked.
- **Broken on purpose:** `results/2026-10-20-milestone-17-part-2/breaks.py` names each break and the checks it should
  fail, and `builds.sh` builds them: resolves copying sample 0, the tiled pass's window offset left at 0,
  `alphaToCoverageEnable` ignored, and the load's draw-back skipped in bands after the first.
- **What the console settled:** exported depth reaches every sample, a draw-back writes every sample, and a band's
  resolve that doesn't clear leaves the target for the next. The 4-sample pass with depth in bands went wrong from
  band 1 on until xsdk resolved a band's depth before its colour, so the band's clear, and the hierarchical buffer's,
  went on a colour resolve (xsdk:docs/HARDWARE.md, "A band's depth clear goes on a colour resolve, milestone 44
  step 5").
- **Depth resolved for Aurora:** `vkCmdResolveImage` of a multisampled depth image right after the pass that drew it
  takes sample 0 on the GPU, as xsdk's depth resolves do, a deviation (docs/SUPPORT.md, Commands); `make
  V4X_FORCE_BANDS=n` forces every pass with depth into n bands, for measuring.
- **Aurora and Dusklight:** Aurora's backend draws the EFB at its configured count (aurora-xenon `acbb442`); Dusklight
  takes `DUSK_XSDK_MSAA` and stays at 1, as 2 samples dips under 30 fps in the title scene and Ordon
  (xsdk:docs/HARDWARE.md, "Multisampling's cost, and Dusklight at 2 and 4 samples, milestone 44 step 5").
- **Not in it:** samples stored apart,
  sampled multisampled images, sample masks, sample shading, alpha to one and multisampled render areas
  (xsdk:docs/scope/44-tiling-and-msaa.md, "Not in it").

### Milestone 18: every GX pipeline in GLSL

**Why.** The backend draws a GX pipeline only once the GLSL generator (aurora-melee:lib/vulkan4xenon/glsl.cpp) has
written programs for it, and each port finds the generator's gaps one config at a time: Melee's attract mode and menus
refused 38 configs in twelve minutes on the console, every one an emboss texgen ("no GLSL yet ... an emboss texgen
(TEXCOORD2)", 360-ports melee run of 2026-10-16). Aurora's WGSL generator for Dawn draws every GX pipeline its games
use (aurora-melee:lib/gx/shader.cpp, build_shader_source, 972-2195), so the gaps are known, and closing all of them
now ends those surprises for every port. They fall in three kinds:
- **Refused, and logged:** emboss texgens (glsl.cpp:116-117; WGSL 1286-1298), binormal and tangent loads and sources
  (123-124; WGSL 762-786, 1100-1117; decode.cpp:219-233 decodes them already), a prior texcoord as a texgen's source
  (125-127; WGSL 1302-1303), more than 10 interpolants (27, 140-142; GX can need 11, two colours, eight texcoords and
  fog, and Xenos has 16, xenia:src/xenia/gpu/xenos.h:802), and destination alpha, refused per pipeline
  (pipeline.cpp:334-336; WGSL 1724-1761).
- **Wrong without a word:** a texgen whose texcoord the vertex lacks stops the title (glsl.cpp:242, `FATAL`) where
  melee-pc's WGSL reads 0, as it does for a missing position or matrix index (WGSL 560-583); texture LOD bias is
  dropped (WGSL 1630, 1434; `gfx::SamplerConfig`, lib/gfx/texture.hpp:152-161, has no bias, and the samplers set none,
  backend.cpp:241-254); single-level textures aren't sampled at level 0 (WGSL 1619-1632); Z textures are ignored, so
  those draws keep polygon depth (WGSL 1743-1801; glsl.cpp never reads `zTexOp`, `zTexFmt`, `zTexBias` or
  `zCompLocBeforeTex`); `normalize` has no guard for a zero vector, and an MTX3x4 texgen none for q near 0 (WGSL
  1347-1350, 1358-1364, 1384-1387; glsl.cpp:498-500); TEV registers no stage has written yet start unwrapped, so a
  `GXSetTevColorS10` value past 0..1 passes as it is (glsl.cpp:686, 688; WGSL 1170-1171).
- **Done, differently, and kept:** vertex formats (decoded on the CPU), matrix indices, MTX2x4/MTX3x4/SRTG texgens,
  lighting, TEV, indirect texturing, alpha compare, fog, and lines and points as quads.

**Agreed with the user (2026-10-16), in four steps,** each done on the console before the next:
1. **Vertex inputs and texgens.** Missing attributes read 0 as WGSL's do, and no valid config reaches a `FATAL`;
   binormals and tangents in the vertex layout and as texgen sources; emboss texgens BUMP0-7 from the light and the
   binormal and tangent through the normal matrix; a prior texcoord as a source; the normalize and q guards; the
   interpolant cap raised to what xost allocates. This is the step Melee needs now.
2. **TEV and sampling.** Unwritten TEV registers wrap as WGSL's do; texture and indirect LOD bias, and the texture's
   LOD clamps, which the layer refuses past a `minLod` of 0 and ignores the most of (aurora-melee:
   lib/vulkan4xenon/backend.cpp:235), in Xenos's texture fetch constant, which holds a LOD bias and the levels sampled
   (xenia:src/xenia/gpu/xenos.h:1265-1270), rather than in the program as WGSL applies them; single-level textures
   sampled at level 0. Widened to the clamps with the user (2026-10-16).
3. **Z textures.** First in xost: `gl_FragDepth`, which it doesn't lower today (src/xsc_resolve.c knows position,
   vertex and instance IDs and FragCoord only), exported as Xenos's pixel shaders export depth; then ADD and REPLACE,
   U8 and U24, with the bias, gated on `zCompLocBeforeTex` as WGSL is. A draw that writes depth can't be tested early,
   so xsdk's HiZ and early Z have to be off for it; how xsdk's layer tells is part of this step.
4. **Destination alpha,** as Dawn's backend draws it: the constant written as alpha, and, when the blend reads source
   alpha, a second pass for the alpha where Xenos has no dual-source blending. Through Vulkan's blend constants, which
   need whatever xsdk's GPU layer lacks for them; the largest step.

**Done when,** for each step, with a deliberate break each check catches:
- `tools/check_glsl_math.py` has a named config for each new feature, run on xost's simulator against the WGSL
  reference's maths, as milestone 11's step 5 set up;
- a coverage check on the PC goes through every value of every `ShaderConfig` field WGSL reads, one at a time on a
  plain config, and a few thousand random valid configs, and every one gets a plan, compiles in glslang and lowers in
  xost inside its register budget; a refusal anywhere fails it and names the field;
- vulkan4xenon_test draws a scene for the step on the console, pixels checked;
- Melee's attract mode and a played match log no "no GLSL yet", and Dusklight's title scene draws as it did.
At the end, the refusal path is left only for configs that aren't valid GX.

**Left out:** what WGSL doesn't do either: `indTexUseOrigLOD` (WGSL only logs it, 1017), half-aspect lines
(`line_aspect_y` is declared and never used), and the unit spot and distance terms (`unitSpotLights`,
`unitDistanceLights`, set only when `limits().unitLightTerms` is on, off by default, backend.hpp:143). Palette and
format conversions are passes of their own, done already, and the normal attachment has no target here.

**Which Aurora.** Z textures, the guards, the attribute defaults and the level-0 sampling are melee-pc's, merged into
aurora-melee's melee-xenon branch (upstream 40cf48f with melee-pc and the xenon work); aurora-xenon, Dusklight's, is
older and lacks them (its `ShaderConfig` has no `zTexOp`). Agreed with the user (2026-10-16): built in aurora-melee,
the tree that has all of the WGSL reference; aurora-xenon comes up to it when Dusklight moves to that upstream, a step
of its own.

**Step 1 on the console** (2026-10-16; `results/2026-10-16-milestone-18-step-1/`). The generator
(aurora-melee:lib/vulkan4xenon/glsl.cpp) writes emboss texgens, binormals and tangents in the vertex layout and as
sources, a prior texcoord as a source, and the attribute defaults, with no `FATAL` left for a valid config; the
normalize and q guards; and up to 15 interpolants. decode.cpp writes zeros for an attribute the vertex lacks.
- **On the PC:** vulkan4xenon_glsl_tests passed 46 of 46. `tools/check_glsl_math.py` passed 28 of 28, 8 of them step
  1's, and caught 12 of 12 breaks of them. The coverage check (aurora-melee:tests/vulkan4xenon_glsl_coverage.cpp and
  tools/check_glsl_coverage.py) swept 4,074 configs one field at a time and 2,500 at random: every one got programs,
  but the first time 2,244 of their 6,644 programs didn't lower in xost (`check_glsl_coverage_before.txt`); now all
  6,644 do (`check_glsl_coverage.txt`).
- **Found by the coverage check, and fixed:** an emboss texgen of an emboss texgen read the first one's vertex output,
  which xost doesn't load back (1,067 programs); xost lowered a float extracted from a vector value only where a scalar
  was wanted, so the TEV's compares of a splatted alpha were refused (1,175 programs; xsdk:src/xsc_lower.c,
  `scalar_value`); and seven programs of 16 stages needed past xost's 64 registers once milestone 36's fetch grouping
  laid their 10 to 15 fetches out together, and stopped in the encoder (`dest=64 does not fit in 6 unsigned bits`)
  before the budget's check could refuse them. xost now refuses a shader past its budget before encoding, and
  `xsdk_xsc_compile` compiles a pixel shader refused so again with its fetches apart: one took 72 registers together
  and 22 apart (xenos-toolchain:tests/test_register_fallback.py). The generator now samples each stage's texture just
  before the stage too, rather than all first. xost's suite passed, two tests new, and caught 3 of 3 breaks.
- **On the console:** vulkan4xenon_test's new `test_aurora_points.c` draws each of step 1's 8 cases as a GX point,
  whose six corners come from one vertex, so every interpolant holds that vertex's value across it, and checks its
  middle against the colour `tools/gen_aurora_points.py` works out from check_glsl_math.py's reference, map 0 a
  triangle wave sampled bilinear, continuous across the wrap, so no coordinate lands on a texel's edge.
  vulkan4xenon_test passed 121 of 121, twice; broken on purpose in three of the cases, those three failed and the
  others passed (`vulkan4xenon_test_broken.log`). xsdk_test passed 315 of 315 with the user at the pad, its new
  `open` and `sysconf` checks among them (`xsdk_test_pad.log`; 313 before that, with no one at the pad,
  `xsdk_test.log`); broken on purpose, both new checks failed (`xsdk_test_broken.log`). Melee's attract mode logged no "no GLSL yet", where it had refused 38 emboss configs,
  and the console compiled all 78 programs it lacked (`melee_attract.log`).
- **Changed on the way:** texgen_tangent's matrix is texture matrix 4, as matrix 2's first row is at right angles to
  the tangent. Programs for an alpha compare that fails every pixel are taken as right, as they discard every pixel.
- **Not affected:** Dusklight runs aurora-xenon, which hasn't the generator's changes; xost's are taken only by
  programs it refused before.

Step 1 is done.

**Step 2 on the console** (2026-10-16; `results/2026-10-16-milestone-18-step-2/`). The generator
starts each TEV register the game set as not yet wrapped, so a stage reading one as a, b or c takes its low 8 bits, as
the WGSL does; seven of vulkan4xenon_glsl_tests' compare tests expected the operands unwrapped and now read the
wrapped locals. Aurora's `SamplerConfig` carries the texture's LOD bias (gfx/texture.hpp), which the backend hands the
layer with its `minLod` and `maxLod` as they are. The layer takes both clamps, to the nearest whole level, where it
refused a `minLod` past 0 and ignored `maxLod` (docs/SUPPORT.md, Samplers); xsdk's sampler has a lowest level and a
count of levels (xsdk:include/xsdk/texture.h), and `xsdk_gpu_set_texture` takes the lower last level of the texture's
and the sampler's (xsdk:src/texture.c, `xsdk_texture_levels`). Single-level textures needed nothing: melee-pc's
level-0 sampling (its c213063) works round Intel's Vulkan driver, and Xenos samples an image of one level at level 0
under any bias, as the console shows.
- **On the PC:** vulkan4xenon_glsl_tests passed 46 of 46; check_glsl_math.py 29 of 29, `tev_reg_wrap` new, with 14 of
  14 breaks caught; the coverage check's 6,644 programs all lower.
- **On the console:** vulkan4xenon_test's new `test_lod.c` draws a 256x256 image of a colour a level at LOD 2 through
  nine samplers, its bias up and down a level and half a level with linear mips, `minLod` 4, `maxLod` 1 and 0, both
  clamps at 2 with a bias of 3, and an image of one level biased up 3; each showed its level. vulkan4xenon_test passed
  123 of 123, twice. Broken on purpose, the layer dropping the bias and `minLod` failed those four cells, and xsdk
  ORing the levels as before failed the four clamped ones (`vulkan4xenon_test_layer_broken.log`,
  `vulkan4xenon_test_levels_broken.log`). xsdk_test's sampling check has two cells new, a lowest level and a count of
  levels; it passed 315 of 315 with the user at the pad, and broken so, both cells failed (`xsdk_test_levels_broken.log`).
  Dusklight, built against this xsdk, ran its title scene at 29.8 to 30 fps, with nothing refused (`dusklight.log`).

Step 2 is done.

**Step 3 on the console** (2026-10-16; `results/2026-10-16-milestone-18-step-3/`). xost exports `gl_FragDepth`,
stored once outside any if/else, to export 61's x, and gives the shader's record ps_export_mode 3: a probe found that
with libxenon's 2 such a shader drew nothing (xsdk:docs/HARDWARE.md, "A pixel shader's depth"). xsdk tells a draw that
writes depth by that bit and leaves hierarchical depth off for it, as the buffer tests a polygon's depth: with it on,
921,438 of 921,600 pixels hid a draw writing nearer depth. The layer needed nothing for it but its docs and tests. The
generator (aurora-melee:lib/vulkan4xenon/glsl.cpp, `z_texture`) writes the WGSL's Z texture after the colour: the last
stage's texel through its swap table, U24's three bytes or U8's first as GX's 24-bit z, replacing the polygon's depth
or added to it, with the bias, wrapped to 24 bits, reversed. In floats, which hold the sum's integers exactly only to
2^24, so a sum past it may be off by its lowest bits. The polygon's depth comes from the clip z and w fog already
passed, now `clip_zw`; the stage's texture is sampled where its colour reads none; compare-before-texture and U16 get
no depth, as the WGSL's `ztex_source_stage` leaves them.
- **On the PC:** vulkan4xenon_glsl_tests passed 47 of 47; check_glsl_math.py 33 of 33, four Z texture cases new, U24
  and U8, replace and add, one wrapping, with 20 of 20 breaks caught, 6 new; the coverage sweep's 6,648 programs all
  lower, 411 writing depth; xost's suite passed 24 of 24, with the fix below, xcomb's parity test run on Windows as
  it needs MSVC (`xost_tests_full.txt`), its new `test_frag_depth.py` catching 4 of 4 breaks.
- **On the console:** vulkan4xenon_test's points draw the four Z textures, map 0 sampled at a texel's middle so its
  bytes are whole, each writing its depth always; over its middle a quad 32 of D24's steps below that depth tests
  greater-than and one above less-than, so either shows only if the point wrote a depth outside them. vulkan4xenon_test
  passed 127 of 127, twice. Broken on purpose, the bundle built by an xost leaving the export mode at 2, or the programs
  writing depth unreversed, the four Z textures failed and the other 123 passed. xsdk_test passed 316 of 316 with the
  user at the pad (xsdk:results/2026-10-16-depth-export/). Melee's attract mode and matches logged no "no GLSL yet"
  (`melee.log`).
- **Found on the way, in Melee:** its fighters drew with spikes, as they had before this milestone. Two frames dumped
  from a diagnostic build, replayed through the layer alone, put it on vertex shaders that fetch five attributes in one
  exec: xost marked only four fetches in the exec's vertex cache bits, and an unmarked fifth fetched wrong. Marked,
  every draw of both frames stayed in place and Melee's fighters drew whole (xsdk:docs/HARDWARE.md, "A fifth vertex
  fetch"; its `results/2026-10-16-fifth-vertex-fetch/`). The replay also showed Xenos drawing past a viewport as far as
  the scissor, which the layer now stops at the viewport (docs/SUPPORT.md, Pipelines). Two of Dusklight's 47 vertex
  programs fetched five attributes; its pack, rebuilt, changed by that one bit in each. vulkan4xenon_test passed 127 of
  127 twice with both fixes (`vulkan4xenon_test_fixes.log`, `vulkan4xenon_test_fixes_2.log`), xsdk_test 316 of 316,
  and Dusklight's title scene ran at 29.3 to 30 fps, with nothing refused (`dusklight.log`).

Step 3 is done.

**Step 4 is put off,** at the user's word (2026-10-16), to xsdk:TODO.md, "Destination alpha".

### The Guide, through xsdk (2026-10-15)

xsdk's milestone 37 shows the Guide: `xsdk_present_end` hands each frame to the kernel's swap, and while the Guide is
open XAM's commands run between the title's frames and draw in eDRAM (xsdk:docs/HARDWARE.md, "The Guide shows through
VdSwap"). So `vkQueuePresentKHR` forgets what the queue keeps in eDRAM, its colour and depths, whenever xsdk's count of
those frames moves (src/swapchain.c). On the console vulkan4xenon_test passed 112 of 112, and vkbench 14 of 14,
this morning's build and the new one twice each in turn: the draw scenes record 0.7 to 2.7% slower and the uniform
scene 3.4% faster, the differences growing with the draws though the swap comes once a frame, so from where the
rebuilt code lies (`results/2026-10-15-guide/`; xsdk:docs/HARDWARE.md, "The Guide, milestone 37"). Dusklight shows the
Guide over the whole frame and leaves through its Xbox Home cleanly.

### Milestone 19: the rest of Vulkan 1.0's commands

**Why.** The layer exports 85 of Vulkan 1.0's 137 commands (src/proc_addr.c, against the vendored headers' version
363), and `vkGetDeviceProcAddr` finds nothing for the other 52, so a port that calls one calls a null pointer. Aurora's
backend calls none of them (aurora-xenon:lib/vulkan4xenon), so they're for the ports and homebrew past Dusklight and
Melee.

**Agreed with the user (2026-10-16), in six steps,** each done on the console before the next. Every command is
exported; one the layer can't do yet logs and returns `VK_ERROR_FEATURE_NOT_PRESENT`, as its other refusals do
(CODE_GUIDELINES.md, "Errors").
1. **What needs nothing of Xenos** (22 commands):
   - what the spec allows without more behind it: the pipeline cache's four, its data a header alone;
     `vkGetRenderAreaGranularity`; `vkGetDeviceMemoryCommitment`; the two sparse queries, which find nothing; and the
     host's five event commands;
   - refused, as the features they need aren't reported: `vkQueueBindSparse`, `vkCreateComputePipelines`,
     `vkCmdDispatch`, `vkCmdDispatchIndirect`, `vkCmdResolveImage`, `vkCreateBufferView`, `vkDestroyBufferView` and
     `vkCmdSetDepthBounds`;
   - `vkCmdSetLineWidth`, with the line width dynamic: 1 is the only width without `wideLines`.
2. **Copies on the CPU at replay** (10), as `vkCmdCopyBufferToImage`'s are (section 3): `vkCmdCopyBuffer`,
   `vkCmdFillBuffer`, `vkCmdUpdateBuffer`, `vkCmdClearColorImage`, `vkCmdCopyImage`, `vkCmdCopyImageToBuffer` and
   `vkCmdBlitImage`, of whole levels; `vkCmdDrawIndirect` and `vkCmdDrawIndexedIndirect`, one draw each, their
   parameters read at replay, since the host or a copy at replay writes every buffer; linear images, and
   `vkGetImageSubresourceLayout` for them. xsdk reads a tiled texture back for it, where it has only
   `xsdk_texture_upload`, and images take `TRANSFER_SRC`.
3. **Dynamic state** (5): the stencil compare mask, write mask and reference, the depth bias, and blend constants, for
   which xsdk gains the constant-colour factors and `RB_BLEND_RED` to `ALPHA`
   (xenia:src/xenia/gpu/register_table.inc:506).
4. **Clears** (2): `vkCmdClearAttachments`, a rectangle the layer draws, as it draws an image back (src/blit.c); and
   `vkCmdClearDepthStencilImage`, kept until the next pass that loads the image, since depth lives in eDRAM.
5. **Secondary command buffers and subpasses** (2): `vkCmdExecuteCommands` replays secondary command buffers in place;
   `vkCmdNextSubpass` moves through render passes of several subpasses, each replayed as a pass of its own, without
   input attachments.
6. **Events, queries and time stamps the GPU writes** (11): `vkCmdSetEvent`, `vkCmdResetEvent` and `vkCmdWaitEvents` on
   xsdk's fences, with `PM4_WAIT_REG_MEM` (xsdk:src/gpu_setup.c:173) for an event the host sets after the submit;
   occlusion queries' seven on `EVENT_WRITE_ZPD` (xenia:src/xenia/gpu/command_processor.cc:1249), after a console
   probe; and `vkCmdWriteTimestamp` on the GPU clock the console showed (xsdk:docs/HARDWARE.md, "A clock the GPU and
   the CPU both read"), 64 bits if `RBBM_PERFCOUNTER1_HI` (xenia:src/xenia/gpu/register_table.inc:52) counts.

   Changed with the user (2026-10-16), after the probe: the high word counts and the command processor waits on memory
   the CPU writes, but `ZPASS_DONE` hung the GPU in six runs and wrote no counts (xsdk:docs/HARDWARE.md, "Queries and
   events"). So step 6 does events and time stamps, and the query pool's commands for time stamp pools; occlusion query
   pools are refused, and with them `vkCmdBeginQuery` and `vkCmdEndQuery`, until a sequence that writes the counts is
   found (xsdk:TODO.md, "Occlusion queries").

**Done when,** for each step, vulkan4xenon_test checks each command the step adds, by pixel where it draws or copies
and by value where it answers, every check failing with its break; and vulkan4xenon_test passes in full on the
console.

**Not in milestone 19,** each a milestone of its own, and refused until then:
- `vkCmdResolveImage`, once the layer has multisampled images (milestone 17): Xenos multisamples in eDRAM and its
  resolve averages the samples (xsdk:include/xsdk/gpu.h, `xsdk_gpu_set_target_samples`).
- Buffer views: uniform texel buffers, read through a vertex fetch or a linear texture, with xost lowering the reads;
  storage texel buffers need memexport.
- Compute, emulated: a draw of points whose vertex shader writes through memexport (xenia:src/xenia/gpu/shader.h:59-62),
  for shaders without shared memory or barriers.
- Occlusion queries, once a probe finds how Xenos writes its sample counts (step 6).

Two stay refused: depth bounds, as Xenia's registers have no depth bounds test and a pixel shader can't read eDRAM's
depth; and sparse binding, as the GPU addresses physical memory and Xenia's registers have no GPU MMU. Neither is a
console result.

**Step 1 on the console** (2026-10-16; `results/2026-10-16-milestone-19-step-1/`). The 22 commands are exported and
found by name. A pipeline cache's data is its 32-byte header, least significant byte first as the spec has it on a
big-endian CPU; events are a flag the host sets, read atomically; the sparse queries find nothing; a pipeline may take
its line width from `vkCmdSetLineWidth`, which takes 1 alone. The eight refusals log and return
`VK_ERROR_FEATURE_NOT_PRESENT`. vulkan4xenon_test passed 138 of 138, twice, its 11 checks new in `test_commands.c`.
Broken on purpose, one break a check (`breaks.py`), the 11 failed and the other 127 passed
(`vulkan4xenon_test_broken.log`).

Step 1 is done.

**Step 2 on the console** (2026-10-16; `results/2026-10-16-milestone-19-step-2/`). The copies, clears and blits run on
the CPU at replay, in src/transfer.c, beside `vkCmdCopyBufferToImage`'s, which moved there. xsdk reads a level's rows
back (`xsdk_texture_download_rows`) and writes some of them (`xsdk_texture_upload_rows`), the upload's runs of whole
words going either way (xsdk:results/2026-10-16-texture-read-back/). Image copies go a row at a time through one row
of the CPU's, as two levels' layouts may differ; blits filter 8-bit channels on the CPU, a destination row from the
two source rows about it; clears upload one row of the texel over and over. Indirect draws read their parameters at
replay, one draw each. Linear images are one level, copied to and from, their rows 256 bytes apart. A copy into a
buffer waits for the draws before it, which may read it, and, as images' last uses are kept and buffers' aren't, for
all of them (docs/SUPPORT.md, "Commands"). Not in the plan, as small: a buffer only copied to and from is offered the
cached memory type, so one an image is read back into maps fast.
- **The checks:** vulkan4xenon_test's `test_copies.c` draws into a target the CPU cleared, a quad directly and one
  through each indirect command, the plain one's count from a `vkCmdUpdateBuffer` before it; halves the vertices
  between two passes; clears the target in a pass that draws nothing; blits a 64x64 chain down to 1x1 and a 2x2 up
  to 4x4; clears an R8 image's tail; copies between optimal and linear images, the host's at its row pitch; fills,
  updates and copies buffers at unaligned offsets. Each image comes back through `vkCmdCopyImageToBuffer` into cached
  memory and is checked texel by texel.
- **On the console:** vulkan4xenon_test passed 154 of 154, twice, its 16 checks new. Broken on purpose, in four
  builds as some breaks fail several draw checks at once (`breaks.py`), each new check failed with its break and no
  older one did but the memory check this step changed: 13 failed with twelve breaks of copies, clears, blits,
  indirect draws, linear pitches, refusals, limits and names (`vulkan4xenon_test_broken_a1.log`); the clear-only
  pass failed with the read-back not waiting for its image (`_a2`); the quads drawn before the halving failed with
  the copy into a buffer not waiting for the draws (`_a3`); and the buffers' making and the objects' memory check
  failed with the cached type kept from a buffer copied into (`_b`). The read-back's wait for its image was caught only once the clear-only pass came in,
  as a draw's wait covers it otherwise (`before_clear_pass/`). The first run's triangle check failed on frame 0
  because the new test left the harness's fence signaled; it now resets it, as the other tests do
  (`vulkan4xenon_test_fence_left_signaled.log`). xsdk_test passed 318 of 318 with the user at the pad; broken on
  purpose, rows written past the first landing on the first and words read back off by one, its two new checks
  failed and the rest passed. Dusklight, built clean against both, ran its title scene at 29.4 to 30 fps, refusing
  only the 20x11 copy, 16 texels or less on a side, as since milestone 14, until the user left it through the Guide's
  Xbox Home (`dusklight_m19.log`).

Step 2 is done.

**Step 3 on the console** (2026-10-16; `results/2026-10-16-milestone-19-step-3/`). A pipeline keeps the xsdk settings
its raster and depth and stencil blocks came from, and a bit for each dynamic state it takes; the replay keeps what the
commands last set, and works the blocks out again for a pipeline that takes them, once a pipeline is bound or a command
sets one. The stencil commands set both faces at once, as both share one setting, and a face alone is refused, as a
depth bias clamp is. xsdk gained the constant blend factors and `xsdk_gpu_set_blend_constants`, which writes
`RB_BLEND_RED` to `ALPHA`; the layer sets the colour with each pipeline whose blend reads it, its own or
`vkCmdSetBlendConstants`'s (xsdk:docs/HARDWARE.md, "Blend constants"; xsdk:results/2026-10-16-blend-constants/).
- **The checks:** vulkan4xenon_test's `test_dynamic_state.c` draws four stencil cells, a mask then a quad over it
  through a write mask, a reference, a compare mask, and a static mask after a dynamic reference, each pipeline's own
  values set to fail; a quad pulled nearer and one pushed farther by `vkCmdSetDepthBias` on a base at their depth;
  and three blends, two by `vkCmdSetBlendConstants`'s colours set between them and one by a static pipeline's alpha.
  xsdk_test's raster test draws two cells by the constant factors, a colour's channels and an alpha.
- **On the console:** vulkan4xenon_test passed 161 of 161, twice, its 7 checks new. Broken on purpose, the write mask
  a command set ignored, the dynamic bias ignored, the pipeline's own blend constants kept, a single face's stencil
  recorded and `vkCmdSetBlendConstants` missing from the names, the five checks they reach failed and the rest passed
  (`vulkan4xenon_test_broken_a.log`); dynamic blend constants refused, the pipelines' making and the four checks after
  it failed (`_b`). xsdk_test passed 318 of 318 with the user at the pad; the constants written alpha first, its
  raster check failed on the two constant cells alone.

Step 3 is done.

**Step 4 on the console** (2026-10-16; `results/2026-10-16-milestone-19-step-4/`). `vkCmdClearAttachments` draws each
rectangle with the layer's own shaders, as it draws an image back: the rectangle it has for that over the whole target,
the scissor at the clear's rectangle, a new `clear.frag` writing the colour from a push constant, and a viewport whose
depth range is the clear's depth alone, so every pixel takes it; depth and stencil always pass, the stencil taking the
clear's as its reference, and each of colour, depth and stencil is written only if it's cleared. The pass's pipeline is
set again for the next draw. `vkCmdClearDepthStencilImage` keeps its values on the image, and the next pass with it as
its depth attachment clears to them unless it clears to its own; a sampled image's memory takes the clear on the CPU at
once. Depth images take `TRANSFER_DST` for it, and a range of depth or stencil alone is refused.
- **The checks:** vulkan4xenon_test's `test_clears.c` clears a depth image to 0.2 and stencil 1 each frame, then draws
  in a pass loading it: a quad behind 0.2 hides and one in front shows, a quad testing stencil equal to 1 shows, and
  the pass undoes both at its end, so each frame finds them by the image's clear alone. In the same pass it clears a
  rectangle of colour inside a red quad, then draws with the pipeline from before the clear; a rectangle of depth to
  0.9 between a near quad and a farther one; and a rectangle of stencil to 2 under a quad testing equal to it. A
  sampled depth image cleared outside a pass is read back through its memory.
- **On the console:** vulkan4xenon_test passed 170 of 170, twice, its 9 checks new. Broken on purpose, in four builds
  (`breaks.py`): the colour, depth and stencil clears each writing nothing, the sampled image left unfilled, a depth
  range alone recorded and `vkCmdClearAttachments` missing from the names failed those six checks
  (`vulkan4xenon_test_broken_a.log`); the image's clear ignored failed its check and the depth cell's, which counts
  on it (`_b`); the pipeline left unset after a clear failed the colour cell and the depth cell's next draw (`_c`);
  depth images refused `TRANSFER_DST` failed the scene's making and the checks after it (`_d`).

Step 4 is done.

**Step 5 on the console** (2026-10-16; `results/2026-10-16-milestone-19-step-5/`). A render pass keeps a pass for each
of up to 8 subpasses, over up to 8 attachments: an attachment takes its own load op in the first subpass that uses it
and its own store op in the last, and the subpasses between load and store it; depth stays in eDRAM from one subpass to
the next unless a subpass gives it up. A framebuffer keeps each subpass's colour and depth images. `vkCmdNextSubpass`
ends one subpass's pass and begins the next's, with the clears `vkCmdBeginRenderPass` was given; a pipeline takes the
settings of its own subpass. `vkCmdExecuteCommands` replays each secondary command buffer's commands in place, and a
pass may take its contents from them. Input and resolve attachments, a subpass with other than one colour attachment
and a render area short of the framebuffer are refused.
- **The checks:** vulkan4xenon_test's `test_subpasses.c` draws a render pass of three subpasses, an offscreen RGBA8
  image, then the swapchain image, then the offscreen image again, through one D24S8 depth: the first draws a red quad
  at depth 0.2, the second, from two secondary command buffers, a green quad at 0.5 that the first's depth hides and a
  yellow one, and the third a blue quad into the offscreen image. Five probes check the swapchain image each frame, and
  the offscreen image, copied into cached memory, is checked for the first's quad, the third's over it, the clear and
  nothing of the second's. An input attachment is refused.
- **On the console:** vulkan4xenon_test passed 176 of 176, twice, its 6 checks new. Broken on purpose, in three builds
  (`breaks.py`): secondary command buffers skipped, a later subpass taking its attachment's own load op, input
  attachments taken and `vkCmdNextSubpass` missing from the names failed those four checks
  (`vulkan4xenon_test_broken_a.log`); depth cleared again between subpasses failed the second subpass's check alone
  (`_b`); secondary command buffers refused failed the scene's making and the checks after it (`_c`). Dusklight, built
  clean against it, ran its warning screen at 59.6 to 60 fps and its title scene at 28.7 to 30 between stage loads,
  refusing only the 20x11 copy, 16 texels or less on a side, as since milestone 14, until the user left it through the
  Guide's Xbox Home (`dusklight_m19.log`).

Step 5 is done.

**Step 6 on the console** (2026-10-16; `results/2026-10-16-milestone-19-step-6/`, and xsdk's probe in
xsdk:results/2026-10-16-queries-probe/). The probe found what events and time stamps need, and that `ZPASS_DONE` hangs
the GPU, so occlusion queries were put off with the user (the plan, above). xsdk gained `xsdk_gpu_write_word`, a word
written once the work before is done, as fences are; `xsdk_gpu_wait_word`, the command processor waiting on a word
in memory; and `xsdk_gpu_write_clock`, `xsdk_gpu_clock_value` and `xsdk_gpu_clock`, the clock's 64 bits as the GPU
copies them and as the CPU reads them, the counter on from set-up (xsdk:docs/HARDWARE.md, "Queries and events").
- **Events:** each a word in GPU memory, 1 when set. `vkCmdSetEvent` and `vkCmdResetEvent` write it after the work
  before; `vkCmdWaitEvents` has the GPU wait on it, then takes memory out of its caches. What the replay does on the CPU
  at the submit doesn't wait for an event the host sets later (docs/SUPPORT.md, Sync).
- **Time stamps:** a pool's queries are slots the clock is copied into, at once at the top of the pipe and once the GPU
  is idle at any other stage; `timestampPeriod` 2.005 ns and 64 valid bits. Resets and copies of results run on the CPU
  at replay, after the pool's last submission, as the copies do; occlusion and pipeline statistics pools are refused,
  and with them `vkCmdBeginQuery` and `vkCmdEndQuery`.
- **The checks:** vulkan4xenon_test's `test_events.c`, run last so the clock is past its first 2^32 ticks, sets and
  resets events from a command buffer; waits on an event the GPU set and on one the host sets 20 ms after the submit,
  the fence after held till then; brackets two time stamps by the CPU's reads of the clock, 50 ms apart by the CPU's
  clock; takes a time stamp at the top of the pipe and one at the bottom after a draw over the screen; copies results at
  64 bits with availability, at 32, and for an unwritten query; and checks a pool's queries unavailable when made and
  again once reset, the period and bits, and the refusals.
- **On the console:** vulkan4xenon_test passed 187 of 187, twice, its 11 checks new. The two time stamps' high word was
  2, and they were 52345.1 us apart by the period against 52346.0 by the CPU's clock; the draw's top time stamp came
  664 clocks after it, the bottom one 134574. The first run had the time stamps around a render pass's end, and the top
  one waited for the pass's resolve as the bottom did (`vulkan4xenon_test_after_a_pass.log`): the command processor
  waits for it itself. The period was 2.004 ns at first, from milestone 34's 498.9 a microsecond; xsdk_test measured
  498.75, so it's 2.005 (`vulkan4xenon_test_period_2.004.log` and `_2`, 52190.5 us against 52207.0).
- **Broken on purpose,** in three builds (`breaks.py`): nine breaks, one a check, failed those nine
  (`vulkan4xenon_test_broken_a.log`); the period at 1 ns failed the period check and the 50 ms one, which reads it
  (`_b`); `vkCreateEvent` failing failed the scene's making and step 1's host event check, and the checks after didn't
  run (`_c`). xsdk_test passed 321 of 321 with the user at the pad, and its three new checks failed with their breaks
  (xsdk:docs/HARDWARE.md, "Queries and events").
- **Dusklight,** built clean against both before the period's correction, which it never reads, ran its title scene's
  attract loop, the title and the opening movie over and over, for an hour at 29.7 to 30 fps between stage loads, 69
  to 72 MB free, refusing only the 20x11 copy, as since milestone 14. The console agent's run ended it at its 3600 s
  limit. `dusklight_m19.log` is the copy taken while it ran, 59 minutes in: the copy fetched at its end was lost, written
  over by a second fetch to the same path.

Step 6 is done, and with it milestone 19: the layer exports all 137 of Vulkan 1.0's commands, and docs/SUPPORT.md
(Functions) lists the ones that refuse and why. Occlusion queries wait for a sequence that makes Xenos write its sample
counts (xsdk:TODO.md).

### Direct3D 9 parity: milestones 20 to 28 (proposed 2026-10-16)

**Why.** Ports will come from any system up to the GameCube and Wii, and from PC games of the same years, not only
through Aurora (the user, 2026-10-16). docs/PARITY.md measures the layer against Direct3D 9's Shader Model 3, the class
all of them were written for: of its 47 rows the layer lacks 17 and has 12 in part, and Xenos has the hardware for all
but occlusion queries. These milestones close the gaps, cheapest first, as a track of their own beside the Dusklight
track's milestones 14 to 17. Each is a proposal till agreed, and its scope is settled when it starts. Each is done on
the console, its new checks in vulkan4xenon_test passing and failing when broken on purpose, with Dusklight run after
as a regression check. What docs/PARITY.md lists past Direct3D 9 is decided when a port needs it.

### Milestone 20: the formats xsdk already has

**Why.** Most PC ports ship DXT textures and pack their vertices. xsdk samples 17 texture formats and fetches 18 vertex
formats (xsdk:include/xsdk/texture.h, shader.h), and the layer takes 6 and 5 of them (docs/SUPPORT.md, Formats).
1. **Images:** DXT1, 3 and 5 as BC1, BC2 and BC3, reported a format at a time, as `textureCompressionBC` promises BC4
   to BC7 too; 565, 1555 and 4444; 16-bit floats in one, two and four channels; and 10-10-10-2. Where the bits of
   Vulkan's packed formats land against xsdk's is settled on the console, as xsdk milestone 17 did for its vertex
   formats.
2. **Vertex attributes:** half floats, and bytes and shorts, normalized or scaled, on xsdk's formats.
3. **The formats xsdk lacks,** xsdk first (xsdk:TODO.md, "More texture formats"): DXN and DXT5A as BC5 and BC4, 16-bit
   fixed point, 10-11-11, the signed formats, and gamma as sRGB, its piecewise-linear curve a deviation
   (xenia:src/xenia/gpu/xenos.cc:27).

Done when vulkan4xenon_test samples every new image format and draws from every new vertex format, checked by pixel.

**Step 1, built** (2026-10-16). A table of the layer's image formats, src/format.c, puts each on the xsdk format that
holds its bits as Vulkan does: a packed unit in the CPU's byte order, as xsdk's uploads take it and as Vulkan has a
packed unit on a big-endian host, and each channel where xsdk's own format puts it, as xsdk milestone 16 settled. Where
Vulkan orders the channels otherwise, the fetch constant's swizzle picks them, and a view's component mapping picks
among those (docs/SUPPORT.md, Formats).
- **The formats:** BC1 with and without alpha, BC2 and BC3 on DXT1, 3 and 5, BC1 without alpha reading 1 for alpha;
  `R5G6B5` and `B5G6R5`; `A1R5G5B5`; `R4G4B4A4` and `B4G4R4A4` on xsdk's 4-4-4-4; `A2B10G10R10` and `A2R10G10B10`;
  and 16-bit floats in one, two and four channels. Each samples with linear filtering. `R5G5B5A1` and `B5G5R5A1` are
  left out, as Xenos's 1-5-5-5 has its alpha in the top bit.
- **BC:** reported a format at a time, `textureCompressionBC` false, as Vulkan allows (docs/SUPPORT.md, Formats).
  Vulkan's blocks are the Khronos Data Format Specification's little-endian 16-bit words and xsdk's DXT formats take
  each word in the CPU's order, so copies in and out swap each word's bytes, a row of blocks at a time
  (src/transfer.c). Compressed images are optimal-tiled; clearing one and blitting one are refused, and so is a copy
  between compressed and uncompressed images, as their words lie in different orders.
- **Copies, clears and blits** of the new formats run on the CPU as the old ones' do: copies move the bits as they
  are, a clear packs its colour as the format holds it, and a blit unpacks each texel to floats, mixes and packs again,
  so every uncompressed format but `R32_SFLOAT` blits. Rows the CPU copies through grew to 8 bytes a texel.
- **The checks:** vulkan4xenon_test's `test_formats.c` makes a 4x4 image of each uncompressed format and an 8x8 one of
  each BC format, every texel's red, green, blue and alpha different, and copies them in from a buffer as Vulkan lays
  them out: packed units in the CPU's order, BC blocks in the Khronos Data Format Specification's. BC1's last two
  blocks are three colours and transparent black, and BC2 and BC3's last block has its first endpoint below the
  second. Each frame draws each image three times, 48 pixels a side: sampled nearest, through a view that shows its
  alpha in red, green and blue, and filtered linearly, a pixel's centre at its own corner's coordinates so texels'
  centres and the midpoints between them land on pixels' centres. Probes check every texel nearest and in alpha, a
  check a format, and halfway between neighbouring texels along the filtered quad's edges, a check for all; BC2 and
  BC3's last block has a check of its own. The images are copied back out and checked byte for byte; each
  uncompressed one is blitted linearly to 2x2, then cleared to one colour, and both read back against the test's own
  packing of the means and the colour. The formats' features, `textureCompressionBC` false and the refusals have a
  check each.

**Step 2, built** (2026-10-16). Vertex attributes of half floats, and of bytes and shorts in `UNORM`, `SNORM`,
`USCALED` and `SSCALED`, two or four to an attribute, on xsdk's vertex formats (src/format.c). xsdk fetches 32-bit
words, each component after the first in the bits above it, and swaps a stream's words 8 in 32, so Vulkan's packed
`A8B8G8R8` words read as they are. Its other formats lay their components out by address, so in each word the bytes or
the halves come in reverse, which xsdk can't fetch as it stood.
- **xsdk:** a vertex element names the component of its format each of x, y, z and w reads (`XsdkVertexElement`'s
  `components`, 0 for its own), put into the fetch's destination swizzle, whose components are absolute
  (xenia:src/xenia/gpu/ucode.h:677-688), before the padding of what the format lacks (xsdk:src/shader.c).
- **The layer:** `R8G8B8A8` picks w, z, y, x; `B8G8R8A8_UNORM`, which Direct3D's `D3DCOLOR` is, y, z, w, x; `R16G16`
  y, x; `R16G16B16A16` y, x, w, z (docs/SUPPORT.md, Formats). `_UINT` and `_SINT` stay refused, as xost takes floats
  alone, and so do formats of 1, 2, 3 or 6 bytes, as xsdk fetches whole words.
- **The checks:** xsdk_test's vertex test draws four cells more, `UBYTE4N`, `HALF2`, `SHORT4N` and `UBYTE4`, each
  colour's components stored in another order and picked back into place, a check of their own. vulkan4xenon_test's
  `test_vertex_formats.c` draws a cell of four squares for each of the 19 byte, short and half formats, colours from
  an attribute 4 bytes into its vertex, each square's components apart and a signed format's most negative in one,
  then the cell again with alpha in red, green and blue; the vertex shader scales and moves each format's range to 0
  to 1, a check a format. Ten formats place a triangle below its square's diagonal too, which x and y swapped would
  mirror and z and w swapped would lose, a check for all; and every format reports `VERTEX_BUFFER_BIT`, and the
  integer formats and those under a word don't, a check.

**Step 3, built** (2026-10-16). The formats xsdk lacked, xsdk first.
- **xsdk:** `XSDK_FORMAT_DXN` and `DXT5A`, which Xenia reads as BC5 and BC4
  (xenia:src/xenia/gpu/vulkan/vulkan_texture_cache.cc:333-337, 384-388); `R16`, `R16G16` and `R16G16B16A16`, 16-bit
  fixed point; `B10G11R11` and `B11G11R10`, Xenos's fixed-point 10-11-11 and 11-11-10; and `xsdk_texture_signs`, which
  reads each of a format's channels unsigned, signed or through gamma, by the fetch constant's TextureSign for the
  component the channel comes from (xenia:src/xenia/gpu/xenos.h:121-129, 1195-1205).
- **The layer:** `BC4_UNORM` and `BC5_UNORM`; `R16`, `R16G16` and `R16G16B16A16` in `UNORM` and `SNORM`; `R8`,
  `R8G8`, `R8G8B8A8` and `B8G8R8A8` in `SNORM` and `SRGB`; `A2B10G10R10` and `A2R10G10B10` in `SNORM`; and BC1 to
  BC3 in `SRGB`. A signed format reads every channel signed, Xenos's two's complement from -1 to 1, the most negative
  read as -1, as Vulkan's `SNORM` has it (xenia:src/xenia/gpu/xenos.h:761-767); an sRGB format reads its colours
  through Xenos's gamma, its piecewise-linear curve a deviation (xenia:src/xenia/gpu/xenos.cc:27; docs/SUPPORT.md,
  Formats). Blits and clears of sRGB images take sRGB's own curve on the CPU, as Vulkan has them.
- **Left out:** `B10G11R11_UFLOAT`, as Xenos's 10-11-11 is fixed point; BC4 and BC5 `SNORM`, which Xenia doesn't read
  signed, so nothing shows how Xenos signs them; sRGB colour attachments, with the render target formats
  (milestone 25).
- **The checks:** xsdk_test's formats test puts every format, the new ones a check of their own, in rows of a pixel a
  texel, and draws eleven cells read signed or through gamma, a check for each kind, decoded by Xenia's curve.
  vulkan4xenon_test's `test_formats.c` takes the 22 new formats as it took step 1's, an sRGB format's texels expected
  through Xenos's curve as sampled and through sRGB's as blitted and cleared, a signed format's shown from -1 to 1 as
  0 to 1; a signed texel holds the most negative value. sRGB images' filtered probes have a check of their own, as a
  blend of texels after the curve, which is Vulkan's order and not Xenia's.

**On the console** (2026-10-16, the zero-fuse console; `results/2026-10-16-milestone-20/`):
- **vulkan4xenon_test passed 257 of 257, twice** (`vulkan4xenon_test.log`, `_2`). Every format held its channels
  sampled nearest, BC's word swap, the signed mode's -1 and Xenia's gamma curve among them; BC2 and BC3 colour blocks
  whose first endpoint is below the second took four colours; and filtered sRGB images blended their texels after the
  curve, as Vulkan's do, so neither is a deviation (xsdk:TODO.md had both as questions).
- **Linear filtering of the half floats and 10-10-10-2 is wrong.** The first run passed 255 of 257: halfway between two
  texels, `A2B10G10R10` and `A2R10G10B10`, unsigned and signed, missed all 360 of their probes over 30 frames, and
  the half floats 270 to 360, while every 8-bit, 16-bit fixed point and BC format gave the mean
  (`vulkan4xenon_test_filtering_wrong.log`, and `_counts`, after the test logged each format's misses). Those seven
  now sample nearest alone and blit nearest, a deviation (docs/SUPPORT.md, Formats; xsdk:docs/HARDWARE.md, "Texture
  and vertex formats, vulkan4xenon milestone 20"); Xenos's formats for filtering at more precision are untried
  (xsdk:TODO.md).
- **Broken on purpose,** each of the five sets failed the checks `breaks.py` names for it, and only those but what
  follows from them (`vulkan4xenon_test_broken_1a.log` to `_3b`); xsdk's two did the same
  (xsdk:results/2026-10-16-vulkan4xenon-milestone-20/).
- **xsdk_test passed 323 of 325,** the two left the controller's, as no pad was in, its four new checks among those
  passed.
- **Dusklight,** built clean against both, ran its title scene's attract loop for five minutes at 29.6 to 29.9 fps,
  94 MB free, refusing only the 20x11 copy, as since milestone 14 (`dusklight_m20.log`).

Milestone 20 is done.

### Milestone 21: Shader Model 3's control flow

**Why.** xost turned `if` into selects in pixel shaders and refused it in vertex shaders, unrolled loops of fixed
counts, and refused an `if` on data inside a loop. The layer refused a shader that needs a hardware loop's constants.
Shader Model 3 has all of it, and Xenos runs it with conditional execs, predicates and loops
(xenia:src/xenia/gpu/ucode.h:101-132).
- **xost:** `if` and `else` in vertex shaders, on data and on uniforms; `if` on data inside a loop's body;
  `gl_FragDepth` written under an `if`. Real control flow where it's needed and selects where the arms are cheap and
  have no side effects, chosen on the evidence and said in xost:docs/xsc.md; exports under control flow as Xenia has
  them.
- **The layer:** the integer constants a hardware loop reads, so a shader whose loop a uniform counts isn't refused,
  the replay kept cheap.

Done when GLSL with each runs through the layer on the console, checked by pixel, and xost's tests cover it.

**Built in three steps** (2026-10-16), each an xsdk commit for xost and a layer commit for its checks:
1. **An if/else on data, by selects** (xsdk `9ef5b51`, `59ae63a`). xost's unroller runs both arms of an if/else on
   data in a vertex shader, in a loop's body or around `gl_FragDepth`, each from the locals and outputs as they were,
   and merges each value they leave apart with a select where they meet, glslang's Phis for `&&` and `||` too.
   Outputs are kept as locals are and exported once at the end, so every path writes them; `gl_FragDepth` written on
   one path alone is refused, as nothing else gives the pixel a depth (xenia:src/xenia/gpu/draw_util.cc:498-502).
   Selects, not execs on the predicate, as such an exec runs for every vertex or pixel of 64 when any of them needs it
   (ucode.h:114-124), so arms that differ within a group run both either way (xost:docs/xsc.md, "Control flow").
2. **An if/else every vertex or pixel agrees on, in execs on the predicate** (xsdk `a850896`). Where the condition
   reads only push constants, uniform blocks and literals, and an arm works something out of its own, a `setp_*` sets
   p0 (ucode.h:1162-1204) and the arms go in `(p0)` and `(!p0)` execs, every instruction in them predicated as its
   exec is (ucode.h:114-124), so the arm not taken runs no instructions. An if/else inside such an arm is selects, as
   there's one predicate.
   Changed after the console run: in a vertex shader, such execs hung the GPU, so a vertex shader's if/else on a
   uniform is selects too, and pixel shaders alone take execs on the predicate (xsdk `37d35de`).
3. **Hardware loops a block counts** (xsdk `3148015`). Vulkan's GLSL has no bare uniforms, and the hardware loops xost
   lowered took their counts from one, so it now keeps `for (int i = 0; i < b.n; i++)`, `n` an int in a push constant or
   uniform block, as a hardware loop: what the loop carries in registers, `i` a float counter, an if/else on data in the
   pass as selects, and `if (c) break;` ending a pass as a predicated LOOP_END, on a value every vertex or pixel shares,
   as LOOP_END breaks only when all 64 agree (ucode.h:372-373). Its manifest names the int's block and offset and the
   loop constant: a vertex shader's from 0, a pixel shader's from 16, as a pixel shader's loop ran 0, 1 and 3 passes on
   the console as loop constant 0 said (xsdk:docs/HARDWARE.md, "Shader constants: partial writes hang the GPU,
   libxenon's whole blocks don't"), so the stages share the 32. The bundle carries each as a resource,
   `V4X_RESOURCE_PUSH_LOOP` or `V4X_RESOURCE_UNIFORM_LOOP`, beside the blocks and samplers, so `V4xShader` keeps its
   layout; the layer takes them from the console's compiler too. At each draw of a pipeline with loops the layer reads
   each int, from the push constants or the bound buffer at its dynamic offset, clamps it to the 0 to 255 passes a loop
   constant counts (xenia:src/xenia/gpu/xenos.h:1086-1097), a deviation past 255, and writes the loop constants, all 32
   with the booleans as xsdk always does, only when one changed.

**Changed from the plan:** the loop constants are written at each draw that changes one, not when a pipeline is
bound, as the count is in a block that changes without a bind: a push constant, or a buffer at another dynamic offset.
A draw whose pipeline has no loop costs one test.

- **The checks:** vulkan4xenon_test's `test_branches.c` and `test_loops.c`, each scene 30 frames, checked by pixel on
  every frame and on the screen after, a check line of its own for each:
  - `branch_data.vert`: four quads whose colour's w picks each arm, moving the quad right or down and picking its
    colour, one arm with an `if` of its own, then an `&&` greening the quad that ends at the bottom right; each quad
    probed where its arm puts it, and the clear where the other would.
  - `branch_uniform.vert`: four draws of one quad, a push constant picking the arm, right in its colour's swizzle or
    down in its colour, probed the same way.
  - `loop_if.frag`: an if/else on the pixel's u in an unrolled loop of four, in bands across the screen, both sides of
    a threshold; `loop_uniform_if.frag`: an if/else on a push constant in an unrolled loop of three, a half of the
    screen for each value.
  - `depth_if.frag` and `depth_before.frag`: `gl_FragDepth` in both arms, and before an if/else and again in an arm,
    then a green quad between the two depths, showing where the arms wrote the far depth alone.
  - `loop_count.frag`: six quads counted 0, 1, 3, 6, 300 and -5 by the push constants, 300 run as 255 and -5 as none.
  - `loop_count.vert`: three rows counted 1, 2 and 4 by a dynamic uniform buffer, each quad moved and coloured a pass,
    probed where its count puts it and clear a pass either side; then the same with `loop_count.frag` colouring the
    rows, counted 3, 5 and 6, both stages' loops in one draw.
  - `loop_count_if.frag`: a uniform buffer's count of 7 over the screen, an if/else on u, the block's colour at `i`
    and `float(i)` in each pass, and a break once the colours' alpha sums to 5, so eight bands show the five passes
    before it.
  - `branch_data.vert`, `branch_uniform.vert`, `loop_count.frag` and `loop_count_if.frag` again, compiled on the
    console from SPIR-V the bundle doesn't hold.
  - xsdk_test's compiler check takes `vk_vertex_if.vert`, `vk_vertex_uniform_if.vert` and `vk_loop_count_if.frag`, the
    console's containers and needs against the PC's.
- **Broken on purpose:** `results/2026-10-16-milestone-21/breaks.py` and `builds.sh`, and xsdk's in
  `results/2026-10-16-vulkan4xenon-milestone-21/`, build a `.xex` for each break from copies of the trees: six of xost's
  (an if/else on data's select taking the other arm, the arms on the predicate swapped, a loop's counter stuck at 0,
  what a loop carries kept as it came in, pixel shaders' loops at loop constant 0, and a Phi's arms swapped) and four of
  the layer's (a dynamic offset ignored, a count unclamped, the loop constants written once, and a push constant's count
  read a word off). `breaks.py fails` names the checks each should fail; on the PC, xost's change the containers of only
  the shaders those checks draw.

**On the console** (2026-10-16, the zero-fuse console; `results/2026-10-16-milestone-21/`):
- **Execs on the predicate hung the GPU in a vertex shader.** The first run's push-constant branch in a vertex shader,
  compiled on the console, placed its first frame's quads wrong and the GPU didn't go idle; every check after failed
  and the title hung as it ended, 262 passed and 46 failed (`vulkan4xenon_test_vertex_predicate_hang.log`). The same
  build with a vertex shader's if/else on a uniform as selects passed everything else, the pixel shader's execs on the
  predicate in a loop's body and every hardware loop among them (`vulkan4xenon_test_vertex_selects.log`), so xost
  keeps it so (xsdk:docs/HARDWARE.md, "Branches, vulkan4xenon milestone 21"; xsdk:TODO.md). That run's one miss was
  the shader cache's count, which the four shaders the branch and loop scenes compile on the console raise to eight.
- **vulkan4xenon_test passed 309 of 309, twice** (`vulkan4xenon_test.log`, `_2`): if/else on data in vertex shaders,
  loops' bodies and around `gl_FragDepth`; hardware loops counted by push constants, 300 run as 255 and -5 as none, by
  a uniform buffer with a break, by a dynamic uniform buffer in a vertex shader, and in both stages at once.
- **Broken on purpose,** each of the ten builds failed the checks `breaks.py fails` names for it and no other
  (`vulkan4xenon_test_broken_<break>.log`); the select swap now fails the vertex shader's push-constant scenes too, as
  they're selects. xsdk's five did the same for xsdk_test's compiler checks
  (xsdk:results/2026-10-16-vulkan4xenon-milestone-21/).
- **xsdk_test passed 326 of 328,** the two left the controller's, as no pad was in.
- **Dusklight,** built clean against both, ran its title scene's attract loop for five minutes at 29.7 to 29.9 fps,
  98 MB free, refusing only the 20x11 copy, as since milestone 14 (`dusklight_m21.log`).

Milestone 21 is done.

### Milestone 22: Shader Model 3's fetches and inputs

**Why.** Ported shaders sample at a LOD they work out, take gradients, read which face they're on, and sample textures
in vertex shaders for terrain. xost takes a constant LOD alone (xost:docs/xsc.md:82) and `gl_FragCoord.xy` alone (72),
and the layer refuses samplers in vertex shaders, as both stages share the fetch constants with the vertex streams
(docs/SUPPORT.md, Pipelines; xsdk:include/xsdk/gpu.h:150-152).
- **xost:** a computed LOD, gradients and `gl_FrontFacing`; samplers in vertex shaders.
- **The layer:** the fetch constants shared out between the stages and the vertex streams.
- **The register budget:** Shader Model 3 promises 32 temporaries, and the layer's subset gives a pixel shader 16
  (section 2). Settled by measuring what a bigger share costs.

Done when vulkan4xenon_test checks each by pixel.

**Built in steps** (2026-10-16).

**Step 1: a LOD and gradients the shader works out.** A Xenos fetch samples at the sum of the fetch constant's LOD
bias, a register LOD `setTexLOD` sets if it reads it, and its own bias field, and works a LOD out from its coordinate's
gradients, or from gradients `setGradientH` and `setGradientV` set, only where it's told to compute one
(xenia:src/xenia/gpu/spirv_shader_translator_fetch.cc:1498-1536). So xost lowers `textureLod` with the computed LOD off,
its LOD in the bias field when it's a constant the field holds and through `setTexLOD` otherwise; `texture()`'s bias the
same way on top of the computed LOD; and `textureGrad`'s gradients through the two setters (xost:docs/xsc.md, "LOD and
gradients"). Before, a constant LOD was added to the computed one, one past 4 levels didn't encode, any other LOD was
refused, and gradients were dropped without a word, as offsets were: xost now refuses offsets, a sample index and a
minimum LOD. The builder keeps each fetch in its order with the setters, and the setters write no temp.
- **The checks:** vulkan4xenon_test's `test_shader_lod.c` draws four strips of eight bands over a 256x256 image whose
  nine levels are each a colour, the image once a band at 64 pixels a side, so at LOD 2. Each band's number comes from
  an interpolant, and every pixel works its LOD out from it: textureLod at the number and a quarter; textureGrad at
  gradients giving that LOD; texture() biased from LOD 2 by the difference; and, in the last strip, textureLod at 1.25,
  in the bias field, and at 6.25, through the register. Each band should show its level's colour whichever way the GPU
  rounds a point-filtered LOD; each strip is a check of its own.

**Step 2: `gl_FrontFacing`.** The pixel parameters' x is the pixel's column with its sign set on a back face
(xenia:src/xenia/gpu/registers.h:224-227; XenosRecomp:XenosRecomp/shader_recompiler.cpp:1465-1467), so xost turns the
parameters on for it as for `gl_FragCoord`, and makes it 1.0 or 0.0 by `rcpc` of x, which keeps column 0's -0.0
negative, then `sge` against its negation (xost:docs/xsc.md, "Facing"). xost took the variable for an interpolator
before. As GLSL takes it as a bool, a pixel shader's if/else or discard on a bool value rather than a comparison now
lowers on the value: `cndgt` for the if/else, `kill_gt` or `kill_eq` against its negation for the discard. Which face
the sign marks is the console's to show; Xenia gives it the host's front face, set from the same PA_SU_SC_MODE_CNTL
face that culling reads (xenia:src/xenia/gpu/vulkan/vulkan_pipeline_cache.cc:625).
- **The checks:** vulkan4xenon_test's `test_facing.c` draws six quads, clockwise and counter-clockwise as they look,
  nothing culled: two under a counter-clockwise frontFace and two under a clockwise one, each the push constants' colour
  where gl_FrontFacing holds and blue where it doesn't, the pixel parameters past an interpolator; and two through a
  discard on !gl_FrontFacing, with no interpolator, so the parameters are in r0. A check for each pair: the front face
  shows its colour and the back face blue, or the clear colour where it's discarded.

**Step 3: samplers in vertex shaders, the fetch constants shared out.** xost lowers a vertex shader's samples after its
vertex fetches, at the LOD it gives, as a vertex shader works none out
(xenia:src/xenia/gpu/spirv_shader_translator_fetch.cc:639-641): glslang gives `texture()` there LOD 0. Each fetches for
every vertex, and two precautions the console has to judge go with it: an exec's vertex cache bits mark its vertex
fetches and none of its texture fetches, and a fetch reading what another fetch wrote in its exec is serialized
(xost:docs/xsc.md, "Vertex textures"). Both stages' samplers take fetch constants from 0, so xsdk gains
`xsdk_shader_move_textures`, which moves a shader's texture fetches up by a base: the layer moves the vertex shader's
past the fragment shader's when a pipeline is made, and the streams keep a slot for every three bindings from 31 down,
31 always, as the layer's blits and clears draw from stream 0. A pipeline that needs more is refused with a log line of
the counts; the per-stage limit stays 16, so two stages of 16 are within the limits and refused, a deviation
(docs/SUPPORT.md, Pipelines).
- **The checks:** vulkan4xenon_test's `test_vertex_textures.c` draws two rows of eight columns. Its vertex shader
  samples a height map, the first row's heights 48 to 188 pixels, the second's 0, and moves each vertex up by its
  height; it colours the second row by textureLod of a texture whose eight levels are each a colour, at each column's
  level a quarter past, the first row by its own colour; the fragment shader multiplies by a white texture of its own.
  So the vertex shader's two samplers sit at slots 1 and 2, the fragment shader's at 0, and a stage reading another's
  texture moves or colours a column wrong. The checks: each column of the first row stands its height above where it's
  drawn, which stays the clear colour; each column of the second row shows its level; and a pipeline of 16 samplers in
  each stage is refused, one of 15 and 16 made. xsdk_test's texture test moves its pixel shader's fetch to slot 5 and
  back, finding that dword alone changed, leaves its vertex shader as it was and refuses a move past slot 31.

**Step 4: the register budget.** The milestone's premise was out of date: xsdk's set-up has given pixel and vertex
shaders 64 registers each since xsdk milestone 24, and the layer's shader builds and its compiler on the console both
take that split (xsdk:docs/HARDWARE.md, "Pixel shaders past 16 registers, milestone 24"; tools/build_shaders.py), so
Shader Model 3's 32 temporaries fit either stage. docs/SUPPORT.md and PARITY.md now say so, and the split stays. What a
bigger pixel share would cost is left to measure. The registers a shader takes hold threads in flight: four samples ran
12.2 texels a clock with 12 registers and 7.5 with 17 under xsdk's split, and 16.6 with 12 under 112 pixel registers
(xsdk:docs/HARDWARE.md, "The texel rate, milestone 36"). So xsdk's texel-rate probe now also declares 32 registers, and
steps the split to 96 pixel and 32 vertex registers as well as 112 and 16, for that curve; a split past xsdk's leaves
vertex shaders fewer, and none of the 150 programs in Dusklight's pack takes more than 15 vertex registers or 17 pixel
ones (their SQ_PROGRAM_CNTL counts), so any of those splits would hold it. Vertex shaders' cost under a smaller share,
with as many as 32 registers, isn't measured.
- **The checks:** none in vulkan4xenon_test; xsdk's `build/probe_texel_rate.xex` logs a line a measurement, such as
  `texel rate: <layout>, 32 registers declared, split 96 pixel: bilinear: ... a clock`, for the reviewer to run.

**Broken on purpose,** for the console's run: `results/2026-10-16-milestone-22/builds.sh` builds each set of `breaks.py`
in a copy of xsdk and the layer, the test's shaders built again by the copy's xost, into
`vulkan4xenon_test_m22_broken_a.xex` to `_c`, and `changed.py` names the shaders each set reaches. Set a, a check each:
textureGrad's gradients unread (shader_lod's gradients), a bias's register LOD unread (its bias), the bias field written
0 (its constant LODs), a discard on a bool taken where the bool doesn't hold (facing's discard), and the fetch
constants' limit one past 32 (vertex_textures' refusal). Set b: a pixel shader's explicit LOD added to the computed one
(shader_lod's LOD per pixel and constant LODs), gl_FrontFacing inverted (all three of facing's), and the vertex shader's
samplers bound at xost's slots though its fetches moved (vertex_textures' heights and levels). Set c: a vertex shader's
register LOD unread (vertex_textures' levels alone). xsdk's `results/2026-10-16-vulkan4xenon-milestone-22/` breaks
`xsdk_shader_move_textures` to write each constant back unmoved, which xsdk_test's texture test should catch.

**On the console** (2026-10-16, the zero-fuse console; `results/2026-10-16-milestone-22/`):
- **vulkan4xenon_test passed 322 of 322,** in four runs (`vulkan4xenon_test.log`, `_2`): a LOD, gradients and a bias
  each pixel works out took the level Xenia's model says in all eight bands, and `textureLod` at 1.25 and 6.25 levels
  1 and 6; `gl_FrontFacing` held on the face each front face makes front, and a `discard` on it with no interpolator
  kept the front alone; a vertex shader's two textures moved each column and picked its level, `setTexLOD`'s value
  holding across execs; and 16 samplers in each stage were refused, past the fetch constants, where 15 and 16 were
  made (xsdk:docs/HARDWARE.md, "LODs, facing and vertex textures, vulkan4xenon milestone 22").
- **One launch froze the console,** right after a clean run, before the title opened its log; three runs in a row
  after a reboot passed, so what froze it isn't known.
- **Broken on purpose,** each set failed the checks `breaks.py` names for it and only those
  (`vulkan4xenon_test_broken_a.log` to `_c`); xsdk's move written back unmoved failed its texture check alone
  (xsdk:results/2026-10-16-vulkan4xenon-milestone-22/).
- **xsdk_test passed 329 of 329,** the user's pad in.
- **Dusklight,** built clean against both, ran its title scene's attract loop for five minutes at 29.6 to 29.9 fps,
  95 MB free, refusing only the 20x11 copy, as since milestone 14 (`dusklight_m22.log`).

Milestone 22 is done.

### Milestone 23: cube and volume textures, and the rest of sampling

**Why.** Skies and reflections sample cube maps, and a port's samplers clamp to a border, mirror once, or compare
depth for shadows. The layer takes 2D images and none of those (docs/SUPPORT.md, Images and Samplers), and xsdk lacks
cube and 3D textures and border and mirror-once addressing (xsdk:TODO.md). xost samples cube maps already
(xost:docs/xsc.md:40).
- **xsdk:** cube and 3D textures, their faces and slices laid out as Xenia has them
  (xenia:src/xenia/gpu/texture_util.cc:209-449); border and mirror-once addressing, its border black or white
  (xenia:src/xenia/gpu/xenos.h:104-162).
- **xost:** `sampler3D`, and the comparison of a `sampler2DShadow`, after the fetch unless a probe finds Xenos compares
  in it.
- **The layer:** cube and 3D images and views; clamp to border, in Vulkan's black and white borders; mirror clamp to
  edge, which in Vulkan 1.0 is the extension `VK_KHR_sampler_mirror_clamp_to_edge`; comparison samplers; and linear
  filtering of depth, once it's been tried on the console.

Done when vulkan4xenon_test samples each face and slice, each address mode and a depth comparison, checked by pixel.

**Decided (2026-10-16),** in three steps, each with xsdk's and xost's part first:
- cube and 3D textures in xsdk, laid out as Xenia has them, with uploads into each face and slice; xost's `sampler3D`;
  and the layer's cube images (2D, six layers, `CUBE_COMPATIBLE`, viewed as a cube) and 3D images, with the copies at
  replay taking layers and slices. Arrays of 2D images and cubes, and drawing into either, stay refused;
- border and mirror-once addressing in xsdk; the layer's clamp to border, in the black the evidence favours and white,
  with a check that tells black's alpha and the other black refused till the console settles it; and mirror clamp to
  edge as `VK_KHR_sampler_mirror_clamp_to_edge`;
- the comparison of a `sampler2DShadow` in xost, after the fetch, which works whatever the hardware does; whether
  Xenos compares in the fetch is a question for a later probe. Comparison samplers on depth images through it, and
  linear filtering of D24S8 reported, with a check that tries it: the console decides.

**Step 1, built** (2026-10-16; not yet run on the console). xsdk lays out cubes of six faces with their mips and 3D
textures of one level, tiled (xsdk:include/xsdk/texture.h): each cube level holds the faces in turn, +x, -x, +y, -y, +z
then -z, a level's size apart rounded to a page, and a 3D texture's slices go in Xenia's 3D tiles, 4 slices deep
(xenia:src/xenia/gpu/texture_util.cc:209-449, texture_address.h:213-238). `xsdk_texture_upload_slice_rows` and
`xsdk_texture_download_slice_rows` copy a face's or slice's rows. xost samples a `sampler3D` with `tfetch3D`, and its
simulator now runs the cube op, so `samplerCube`'s lowering is checked by its numbers (xost:tests/test_cube_and_3d.py).
The layer makes cubes and 3D images in the colour formats, milestone 20's compressed ones too, sampled and copied,
viewed whole as a cube or 3D; the copies, image copies, blits and colour clears at replay take a cube's layers and a 3D
image's slices, a compressed image's blocks swapped in each as in a 2D image's, a blit never scaling depth; a copy
between a buffer and an image keeps its region in the command buffer's data, so a command stays 24 bytes.
`maxImageDimension3D` is 1024, `maxImageDimensionCube` 8192 and `maxImageArrayLayers` a cube's 6; image format
properties give 2D images one layer. 3D mips, arrays and linear cubes and 3D images are refused (docs/SUPPORT.md,
Images).
- **The checks:** vulkan4xenon_test's `test_cube_3d.c` copies a buffer into a 16x16 cube's six faces at each of two
  levels in one region a level, and into a 16x16x8 3D image's slices in two halves, the second from `imageOffset.z`,
  each face and level and each slice its own colour, its quadrants apart in blue; draws each face at level 0 and at
  level 1, by the sampler's LOD clamps, over a quad whose directions span the face, and each slice at its w, each
  quadrant probed every frame; copies level 1's face 3 and slices 5 and 6 back out; copies face 1 into another cube's
  face 4 and clears that cube's faces 0 and 1, reading them back; and checks the image format properties, the limits,
  and the refusals of 2D arrays, cube arrays, cubes drawn into and a 2D view of a face. xsdk_test's `test_cube_volume.c`
  draws each face of a 32x32 cube at levels 0 and 1, and of an 8x8 one at levels 0 and 2, from its packed tails, and
  each slice of a 32x32x8 3D texture and an 8x8x3 one, one texel to a pixel, through xost's shaders.

**Step 2, built** (2026-10-16; not yet run on the console). xsdk's sampler takes the rest of Xenos's address modes,
`ClampMode` 3 to 7, a black or white border, W's mode, and filters between 3D slices as within them
(xsdk:include/xsdk/texture.h; xenia:src/xenia/gpu/xenos.h:104-162). The layer maps clamp to border to Xenos's border, in
transparent black, the black Xenia's backends take (xenia:src/xenia/gpu/vulkan/vulkan_texture_cache.cc:817-824), and
opaque white; opaque black and the integer borders are refused where a mode reads the border, till the console tells
black's alpha. Mirror clamp to edge is Xenos's mirror once, on a device made with `VK_KHR_sampler_mirror_clamp_to_edge`,
which the device lists; `addressModeW` is taken too.
- **The checks:** vulkan4xenon_test's `test_addressing.c` draws a 4x4 image from -1.5 to 2.5 each way, eight pixels a
  texel, in repeat, mirrored repeat, clamp to edge, mirror clamp to edge and an opaque white border, probing texels two
  past each edge, in the mirror image and inside; draws a transparent black border blended by its alpha over the clear
  colour, a check of its own, which passes where the alpha is 0 and shows black where it's 1; and checks the extension
  is listed and opaque black and the integer borders refused. xsdk_test's sampling check draws mirror once, halfway and
  to a border, a black and a white border, filtered and not, and reads the black border's alpha; its cube and 3D check
  draws a 3D texture's w wrapped, clamped and filtered between two slices.

**Step 3, built** (2026-10-16; not yet run on the console). xost compares a `sampler2DShadow`'s depth after the fetch:
the reference clamped to 0 to 1, below, equal to and above the depth, 1 or 0 each, dotted with a register past the
blocks, its resource `XSDK_XSC_COMPARISON` (xost:docs/xsc.md, "Interface conventions";
xost:tests/test_shadow_compare.py). The layer takes comparison samplers, nearest, writes the bound sampler's compare op
into that register at each draw, VkCompareOp's bits 0 to 2 as the weights, and reports D24S8 filtered linearly. A
comparison sampler that filters linearly is refused, as comparing after the fetch would blend depths, not results.
- **The checks:** vulkan4xenon_test's `test_shadow.c` draws a 64x32 D24S8 image once, cleared to 0.25 and its right
  half drawn at 0.75, and every frame compares a reference of 0.5 with it by less, greater, never and always, probing
  each half; samples it linearly at four times its size, probing either side of the edge between the depths, where the
  filter weighs 3/8 and 5/8 of the far texel, a check of its own that the console decides; and checks the format
  reports the filter and a comparison sampler that filters linearly is refused.

**Not in milestone 23,** each refused until it's built:
- a 3D image's mips, which Xenia sizes and packs in ways the console hasn't been asked about, and linear cubes and 3D
  images (xsdk:TODO.md, "Cube and 3D textures, the rest");
- arrays of 2D images or of cubes;
- drawing into a cube's face or a 3D image's slice, with milestone 25's render targets;
- seamless filtering across a cube's faces, which Xenos likely lacks (docs/SUPPORT.md, Images);
- comparisons filtered linearly, which would need four fetches compared apart; `samplerCubeShadow`; and comparing in
  the fetch, for a probe to try;
- opaque black borders, till the console tells black's alpha, and the integer borders.

**On the console** (2026-10-16, the zero-fuse console; `results/2026-10-16-milestone-23/`):
- **Merged onto milestones 20 to 22** by rebasing its four commits: copies, clears and blits run a face or slice at a
  time with milestone 20's block swap and format packing inside each, and xost's comparison went into milestone 22's
  `src/xsc_lower_sample.c` (xsdk 9f9fe62).
- **The first runs failed the shadow scene's making:** `test_shadow.c` built its depth pipeline's spec from the push
  constant layout before making it, so the pipeline had none (`vulkan4xenon_test_null_layer.log`). With the layout
  made first, **vulkan4xenon_test passed 345 of 345, twice** (`vulkan4xenon_test.log`, `_2`): every cube face and 3D
  slice where it belongs, copied in and back out a face or slice at a time; repeat, mirrored repeat, clamp to edge,
  mirror clamp to edge and an opaque white border; a transparent black border's alpha 0; comparisons by less,
  greater, never and always either side of the reference; and D24S8 filtered linearly, which stays reported.
- **Broken on purpose,** each set failed the checks `breaks.py` names for it and only those, once set a's read-back
  break read the first slice for each rather than one past the last, which tripped xsdk's assert and left the title
  waiting in xbWatson (`vulkan4xenon_test_broken_a.log` to `_c`); xsdk's three did the same
  (xsdk:results/2026-10-16-vulkan4xenon-milestone-23/).
- **xsdk_test passed 343 of 345,** the two left the controller's, no button pressed; its cube, 3D and addressing
  checks are xsdk:docs/HARDWARE.md's "Cube and 3D textures and the rest of addressing".
- **Dusklight,** built clean against both, ran its title scene's attract loop for five minutes at 29.6 to 29.9 fps,
  98 MB free, refusing only the 20x11 copy, as since milestone 14 (`dusklight_m23.log`).

Milestone 23 is done.

### Milestone 24: instancing and long strips

**Why.** Foliage, crowds and particles are drawn instanced, and the layer draws one instance a draw (docs/SUPPORT.md,
Commands). It also refuses strips and fans past 65,535 vertices, as a draw packet counts in 16 bits.
- **Instancing,** with `gl_InstanceIndex`, `firstInstance` and bindings at the instance's rate: a draw an instance, or
  one draw whose vertex shader works out its instance and fetches what it needs at an index it computes, since a
  vertex fetch takes its index from a register (xenia:src/xenia/gpu/ucode.h:695-715). Which, and the xost work it
  takes, is settled when it starts.
- **Strips and fans past 65,535,** split as lists are, sharing the vertices where the pieces meet.

Done when vulkan4xenon_test draws instanced, indexed and not, with per-instance attributes, and a strip past 65,535
vertices, checked by pixel.

**Settled when it started (2026-10-16),** in two steps:
1. **Instancing, a draw an instance,** the simple and correct way first: the replay loops over the instances. A binding
   at the instance's rate is fetched with a stride of 0, its stream pointed at the binding's address plus
   (firstInstance + i) times its stride for instance i, and xost lowers `gl_InstanceIndex` to a float constant the
   layer writes before each instance's draw. Indirect draws read instanceCount and firstInstance at replay, as they
   read the rest, so `drawIndirectFirstInstance` is reported.
2. **Strips past 65,535 vertices or indices,** split as lists are, as a draw packet counts in 16 bits
   (xenia:src/xenia/gpu/registers.h:327), a triangle strip's pieces sharing two vertices at an even offset so the
   winding holds, and a line strip's one. Fans only if they split cleanly without an index buffer made at replay, as the
   first vertex has to repeat in each piece; otherwise refused as before.

**Left for later: all the instances in one draw.** A vertex fetch takes its index from a register
(xenia:src/xenia/gpu/ucode.h:695-715), so one draw of every instance's vertices could have its vertex shader work out
each vertex's instance and fetch the instance's attributes at an index it computes. It waits for a port whose small
instances show the cost of a draw each on the console, as it takes more than the simple way:
- the vertex shader has the vertex's index alone (xost:docs/xsc.md, "Register budget"), so the instance is the index
  over the vertices an instance, and the vertex within it the remainder, both worked out in floats, as Xenos's ALU
  has float instructions alone (xenia:src/xenia/gpu/ucode.h:1006, 1359);
- an indexed draw would need its indices repeated for each instance, moved on by the vertices an instance, in an index
  buffer the layer makes at replay;
- every instanced shader's fetches change: xost would fetch the streams at the instance's rate at the computed index,
  and the layer would pass the vertices an instance as a constant.

**Step 1: instancing.** xost gives a vertex shader's `gl_InstanceIndex` the float constant register past its blocks, so
no uniform block the GPU loads shares its group of 4, before the literals, and lists it among the shader's resources;
`float()` of it, integer arithmetic on it and a block array's index at it lower as xost's integers do (xost:docs/xsc.md,
"Interface conventions"). A vertex shader's other built-ins, `gl_VertexIndex` among them, are no longer fetched as
vertex input 0 but refused when read. The bundle records the register as a `V4X_RESOURCE_INSTANCE_INDEX` resource,
tools/build_shaders.py taking xost's resource list as the compiler gives it. A pipeline takes bindings at the instance's
rate, each fetched with a stride of 0 (xenia:src/xenia/gpu/shader_interpreter.cc:1013-1014); the replay draws each
instance on its own, those bindings' streams moved to the instance's element and its index written into the constant
first, `firstInstance` included. `vkCmdDrawIndirect` and `vkCmdDrawIndexedIndirect` take the instance count and first
instance they read, and the device reports `drawIndirectFirstInstance`.
- **The checks:** vulkan4xenon_test's `test_instancing.c` draws five rows of a grid, each instance a quad in a cell
  160 by 120, from an instance buffer bound at each row's own 8 entries: `vkCmdDraw` of 4 instances, `vkCmdDrawIndexed`
  of 3 from instance 2 at a vertex offset of 2, `vkCmdDrawIndirect` of 4 from instance 3 and `vkCmdDrawIndexedIndirect`
  of 2 from instance 5, the parameters the host wrote, each instance in the column `gl_InstanceIndex` gives and the
  row and colour its own attributes give; and a shader without `gl_InstanceIndex` placing 4 from instance 1 by their
  attributes alone, column 7 less the entry. Each cell's middle holds its instance's colour or the clear colour, each
  row a check of its own, and the device reports `drawIndirectFirstInstance` and is made with it. xost's
  `tests/test_instance_index.py` checks the register past a push and a uniform block and alone, each of 8 instances'
  position and colour on xost's simulator, and `gl_VertexIndex` refused.
- **Broken on purpose** (`results/2026-10-16-milestone-24/breaks.py` and `builds.sh`), three builds: `a` breaks each
  row's draw and the feature on its own, `vkCmdDraw` dropping all but one instance from instance 0, `vkCmdDrawIndexed`
  recording a first instance of 0, `vkCmdDrawIndirect` reading none, `vkCmdDrawIndexedIndirect` reading its vertex
  offset as it, streams at the instance's rate moved only where the shader reads `gl_InstanceIndex`, and the feature
  unreported, so each of those six checks fails alone; `b` fetches the instance's bindings at their stride, failing all
  five rows; `c` never writes the index, failing the four rows that read it and not the fifth.

**Step 2: strips past 65,535.** A pipeline keeps the pieces its topology splits a long draw into: lists on whole
primitives, as before; a line strip's pieces 65,535 vertices long, each starting at the last one's last vertex; and a
triangle strip's 65,534 long, each starting at the last one's last two, 65,532 on, an even number, so every triangle
keeps the parity its facing goes by (Vulkan 1.0 spec, "Triangle Strips"). Draws and indexed draws go through the same
pieces, an indexed one's moving the index buffer's address. Fans stay refused past 65,535: every piece would need the
fan's first vertex before its own, which a draw from vertex 0 or from a run of the index buffer can't give it, and
indices made at replay would be the only way (docs/SUPPORT.md, Commands). The replay now checks every draw for that,
not only indirect ones, so a draw recorded in a secondary command buffer without its pipeline can't draw a fan in
pieces.
- **The checks:** vulkan4xenon_test's `test_strips.c` draws a triangle strip of 70,000 vertices with back faces culled:
  its first quad, a quad over the split whose lower left triangle is the first piece's last and whose upper right is the
  second's first, and its last quad, every vertex between them a quad's corner again so no triangle there has any area.
  Both triangles of each quad are probed, and nothing between them. The same strip draws again below from 70,000 32-bit
  indices. A line strip of 70,000 vertices, seven of them placing its lines and the rest off the screen, draws a row
  across the screen in three lines, the first piece's last, the second's first and the one after, and its last two lines
  on another row, each line probed. A triangle fan of 70,000 vertices still fails its command buffer, and one of 65,535
  doesn't.
- **Broken on purpose,** three builds more: `d` steps a triangle strip's pieces 65,533 on, failing both triangle strip
  checks, steps a line strip's 65,535 on, sharing nothing, failing the line strip's, and takes long fans, failing the
  fan's; `e` has an indexed draw's pieces share nothing, failing the indexed strip's check alone; `f` draws a strip's
  first piece alone, failing the three strips' checks and no list's.

**On the console** (2026-10-16, the zero-fuse console; `results/2026-10-16-milestone-24/`):
- **vulkan4xenon_test passed 361 of 361, twice** (`vulkan4xenon_test.log`, `_2`): every instance of `vkCmdDraw`,
  `vkCmdDrawIndexed` and both indirect draws in its own column and colour, `firstInstance` included, a stride of 0
  reading one element for every vertex and each instance's constants taking effect on its draw; a triangle strip of
  70,000 vertices, back faces culled, right over its split and to its end, from 32-bit indices too, and a line
  strip's; and a long fan still refused (xsdk:docs/HARDWARE.md, "Instances and long strips, vulkan4xenon milestone
  24"). What a draw an instance costs is still unmeasured.
- **Broken on purpose,** each of the six sets failed the checks `breaks.py` names for it and the screen's, and only
  those (`vulkan4xenon_test_broken_a.log` to `_f`).
- **xsdk_test passed 343 of 345,** the two left the controller's, no button pressed (`xsdk_test.log`).
- **Dusklight,** built clean against both, ran its title scene's attract loop for five minutes at 29.5 to 29.9 fps,
  99 MB free, refusing only the 20x11 copy, as since milestone 14 (`dusklight_m24.log`).

Milestone 24 is done.

### Milestone 25: render target formats, and four targets at once

**Why.** HDR and deferred renderers draw into float and 10-bit targets, several at once. The layer draws into one RGBA8
or BGRA8 colour attachment a subpass (docs/SUPPORT.md, Render passes); xsdk draws 32-bit RGBA targets alone, and
several targets are unproven in xost (xsdk:TODO.md). Xenos has the formats (xenia:src/xenia/gpu/xenos.h:299-313) and
four targets, each blended on its own (xenia:src/xenia/gpu/register_table.inc:481, 483-485, 537-547).
- **xsdk:** float, 10-bit and 64-bit targets and their resolves; four targets at once.
- **xost:** fragment outputs at locations 1 to 3.
- **The layer:** those formats as colour attachments, four colour attachments a subpass, and a blend for each.

eDRAM's 10 MB holds one 32-bit colour target and depth at 1280x720, so at that size a second target needs predicated
tiling: this milestone takes the sizes eDRAM holds.

Done when vulkan4xenon_test draws four targets in one pass in the new formats and samples them back, checked by pixel.

**Built** (2026-10-16), in three steps, a commit each; the console has run none of it yet.
1. **xsdk's target formats and their resolves.** `xsdk_gpu_set_targets` places a target of 2_10_10_10, its 7e3 float,
   16_16_16_16 fixed point from -32 to 32, 16_16_FLOAT, 16_16_16_16_FLOAT or 32_FLOAT, its format in `RB_COLOR_INFO`
   (xenia:src/xenia/gpu/registers.h:736-750), and `xsdk_gpu_color_target_tiles` counts its tiles, twice a 32-bit
   one's for the 64-bit formats (xenos.h:321-326, 427-437). `xsdk_gpu_resolve_target` copies a target into a texture
   of the format it converts to, as Xenia models the resolve (shaders/resolve.xesli): 2_10_10_10 into A2B10G10R10,
   16_16_FLOAT into R16G16F, 32_FLOAT into R32F, and the rest into R16G16B16A16F, the destination swapped into the
   CPU's order and its number format float for the floats. Its clear goes into `RB_COLOR_CLEAR` packed as the target
   holds it, a 64-bit target's blue and alpha in `RB_COLOR_CLEAR_LO`, as Xenia writes them (draw_util.h:565-567). The
   old target calls write the same registers as before.
2. **Four targets at once, in xsdk and xost.** `xsdk_gpu_set_targets` takes up to four, the rest in `RB_COLOR1_INFO`
   to `RB_COLOR3_INFO` (register_table.inc:481-485); `xsdk_gpu_set_blends` writes `RB_BLENDCONTROL0` to 3 and each
   target's four bits of `RB_COLOR_MASK`; a resolve of target n puts it where target 0 goes. xost lowers fragment
   outputs at locations 0 to 3 to `oC0` to `oC3` (xost:docs/xsc.md), each stored once outside any `if`/`else` when
   there are several, and its record's outputs word names them; `ps_export_mode` stays 2. One output lowers byte for
   byte as before, so no shader bundle changed but for the new shaders.
3. **The layer.** Colour attachments of `A2B10G10R10_UNORM_PACK32`, `R16G16_SFLOAT`, `R16G16B16A16_SFLOAT` and
   `R32_SFLOAT`, beside `R8G8B8A8_UNORM` and `B8G8R8A8_UNORM`, sampled nearest; one to four a subpass, placed one
   after another from tile 0, each a target of its own, with a blend state each and `independentBlend`. The replay
   tracks what each of the last pass's targets holds at its tile and in its format, draws a loaded image back into its
   target alone, and resolves and clears each target as before; `vkCmdClearAttachments` clears any of them, drawn by
   a shader writing four colours. A subpass whose attachments pass eDRAM's 2048 tiles is refused at
   `vkCreateFramebuffer` with a line saying how many it'd take, and blending into a float attachment is refused at
   `vkCreateGraphicsPipelines`, as those formats report no blending until the console shows they blend
   (docs/SUPPORT.md).

**The checks:**
- **xsdk_test's targets test** (xsdk:tests/test_targets.c): each new format alone at 64x32, a resolve's clear and a
  quad over the left half, past 1 in the floats and below 0 in the fixed point and half floats, every channel apart
  from the others, every texel read back by value; a quad blended onto itself, a check for 2_10_10_10 and a log line
  for the others; then four targets of four formats at once, 8_8_8_8, 2_10_10_10, 16_16_16_16_FLOAT and 32_FLOAT, one
  draw writing each its own colour from a hand-assembled shader writing `oC0` to `oC3`
  (xsdk:tools/gen_target_shaders.py), and a second blending the first by adding, the second by its alpha with alpha
  masked, writing red and alpha alone of the third and nothing of the fourth. Fourteen checks, five broken builds
  (xsdk:results/2026-10-16-vulkan4xenon-milestone-25/).
- **vulkan4xenon_test's `test_targets.c`:** a pass of four colour attachments, `R8G8B8A8_UNORM`,
  `A2B10G10R10_UNORM_PACK32`, `R16G16B16A16_SFLOAT` and `R32_SFLOAT`, each cleared to its own values, one draw
  writing each a colour of its own from GLSL with four outputs, the last a float, and a second blending and masking
  each its own way, as xsdk_test does, then `vkCmdClearAttachments` clearing the second and third in a corner, below
  0 and past 1; a second pass drawing the half-float image back as its second attachment, beside an `R16G16_SFLOAT`
  one, and a quad over both; a third sampling all five onto the screen, scaled down so values past 1 show, checked by
  pixel each of 10 frames. Each image is read back and checked by value, and the limits, features, formats' features
  and refusals are checked. Ten checks, three broken builds (`results/2026-10-16-milestone-25/`).

**What the console must settle,** each with the check that would show it wrong:
- whether a pixel shader exporting `oC1` to `oC3` needs a `ps_export_mode` other than 2: the four-target checks of
  both titles would find targets 1 to 3 holding their clears;
- whether `RB_COLOR_CLEAR` holds a 64-bit target's red and green, as Xenia writes them, or its blue and alpha: the
  16_16_16_16 and 16_16_16_16_FLOAT clears would come out with those halves swapped;
- whether resolves convert 7e3 and 16_16_16_16 into half floats and keep 16_16_16_16's sign, and whether
  `RB_COPY_DEST_INFO`'s float number format suits the float destinations: each format's own check;
- whether the float targets, 7e3 and 16_16_16_16 blend: xsdk_test's log line for each. If they do, their formats can
  report `VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT`, and the pipeline refusal goes;
- whether 2_10_10_10 blends as 8-bit channels do: xsdk_test's blend check and the layer's second attachment.

**Not in milestone 25:** 32_32_FLOAT, 16_16 and gamma targets, which the decisions left out; targets past eDRAM and
multisampled, milestone 17's; and drawing into a cube's face or a 3D image's slice (milestone 23). Milestone 20 clears
the new formats' images and samples them nearest, as the console filtered them wrong.

**On the console** (2026-10-16, the zero-fuse console; `results/2026-10-16-milestone-25/`):
- **The first runs failed every target check** (`vulkan4xenon_test_mode_2.log`; xsdk's `xsdk_test_mode_2.log`), and
  settled the first two questions (xsdk:docs/HARDWARE.md, "Float, 10-bit and 64-bit targets and four at once"):
  `ps_export_mode` counts the colours exported from its bit 1, as R600's does, so at 2 targets 1 to 3 kept their
  clears; and a 64-bit target's clear takes red and green from `RB_COLOR_CLEAR_LO`, so those halves came out swapped.
  xost now gives a shader writing `oC0` to `oC3` mode 8, one colour keeping 2, and xsdk swaps the clear's words.
- **A 10-bit attachment rounds the source to its own bits before it blends:** the second attachment, blended by an
  alpha of 0.25, came out as at 1/3, which is 0.25 in 2 bits. Both titles' checks now blend at 1/3.
- **Whether the targets blend:** 2_10_10_10, 7e3 and 16_16_16_16 do; the 16- and 32-bit floats don't, so their
  formats keep reporting no blending and the pipeline refusal stays (docs/SUPPORT.md). Resolves convert 7e3 and
  16_16_16_16 into half floats, the latter signed, and the float destinations take `RB_COPY_DEST_INFO`'s float format.
- **vulkan4xenon_test passed 371 of 371, twice** (`vulkan4xenon_test.log`, `_2`): four attachments of four formats
  each cleared, written, blended and masked its own way and cleared in a corner; the half-float image drawn back as a
  second pass's second attachment; all five sampled onto the screen each frame; and the limits, features and
  refusals.
- **Broken on purpose,** each of the three sets failed the checks `breaks.py` names for it and only those
  (`vulkan4xenon_test_broken_a.log` to `_c`), as did xsdk's five (xsdk:results/2026-10-16-vulkan4xenon-milestone-25/).
- **xsdk_test passed 359 of 359,** a button pressed (`xsdk_test.log`).
- **Dusklight,** built clean against both, ran its title scene's attract loop for five minutes at 29.6 to 30.0 fps,
  98 MB free, refusing only the 20x11 copy, as since milestone 14 (`dusklight_m25.log`).

Milestone 25 is done.

**Then milestone 17, widened.** Milestone 17, multisampling, needs predicated tiling at 1280x720. Proposed: it comes
after milestone 25 and takes every target past eDRAM, multisampled, several at once, or past 1280x720, such as a big
shadow map; and alpha to coverage (xenia:src/xenia/gpu/registers.h:687).

### Milestone 26: raster state

**Why.** Stencil shadows set each face's stencil apart, ports clip against planes, particles draw sized point sprites,
and debug views draw wireframes. The layer does none of them (docs/SUPPORT.md, Pipelines), nor does xsdk, though Xenos
has each.
- **Two-sided stencil** (xsdk:TODO.md, "Two-sided stencil").
- **Clip distances.** Xenos clips against six planes (xenia:src/xenia/gpu/registers.h:539-544), and Vulkan's distances
  are any value the vertex shader writes. So a distance from a plane might take one of Xenos's, and any other be
  interpolated for the pixel shader to discard by. Settled when it starts.
- **Point sprites:** `gl_PointSize` and `gl_PointCoord` (xenia:src/xenia/gpu/registers.h:184-212), with `largePoints`.
- **Wireframe and point fill,** with `fillModeNonSolid` (xsdk:TODO.md, "Wireframe and point fill").

Done when vulkan4xenon_test checks each by pixel.

**Settled when it started (2026-10-16), in four steps,** each built with its checks and its breaks
(`results/2026-10-16-milestone-26/breaks.py` and `builds.sh`, and xsdk's in
xsdk:results/2026-10-16-vulkan4xenon-milestone-26/), and run on the console one step at a time:
1. **Two-sided stencil:** xsdk sets `backface_enable` and the `_bf` fields of `RB_DEPTHCONTROL` and
   `RB_STENCILREFMASK_BF` (xenia:src/xenia/gpu/registers.h:799-834); the layer takes each face's stencil from the
   pipeline, and `vkCmdSetStencilCompareMask`, `WriteMask` and `Reference` for each face alone.
2. **Clip distances, in the form Direct3D 9's clip planes take:** a distance written as `dot` of the vertex's
   clip-space position and a plane from a uniform block or push constants goes to Xenos's user clip planes, which
   xost recognises and the layer writes from the bound values at each draw. Any other form is refused.
3. **Point sprites:** `gl_PointSize` from the vertex shader and `gl_PointCoord` from the pixel parameters, with
   `largePoints` and the hardware's point size range; without a size written, points stay 1 pixel.
4. **Wireframe and point fill:** `polygonMode` `LINE` and `POINT` through `PA_SU_SC_MODE_CNTL`'s `poly_mode`, with
   `fillModeNonSolid`. Line width stays 1.

**Step 1, two-sided stencil** (built 2026-10-16, not yet run on the console). `XsdkDepthStencil` gained `two_sided`
and a `back` face, its compare, ops, reference and masks: `two_sided` sets `backface_enable` and the `_bf` fields, and
`xsdk_gpu_set_depth_stencil` writes `RB_STENCILREFMASK_BF` beside `RB_STENCILREFMASK` in one packet, the front's in
both when one-sided. The hierarchical buffer counts the back face's fail ops among those it can't skip. A pipeline
takes each face's stencil and sets two-sided where the faces differ, so one-sided stencil writes what it did; the
stencil commands set the faces their mask names, a face alone no longer refused, and the replay works the faces out
again, two-sided where they now differ.
- **The checks:** vulkan4xenon_test's `test_two_sided_stencil.c` draws three cells over stencil cleared to 0x80, each
  marked by a front face on its left and a back face on its right, then a front face over all of it in one colour and
  a back face in another, which must each show over their own face's mark alone: the pipeline's own faces, the front
  replacing with 0x11 and the back inverting through the write mask 0x2F, then each testing through its own compare
  mask against its own reference; references and write masks set by commands for each face alone, the back's 0xFA
  through 0x0F; and compare masks set for each face alone, 0xF0 and 0x0F. `test_dynamic_state.c` no longer counts a
  face alone among the refusals. xsdk_test's depth test draws a two-sided cell, a front face writing 0x11 and a back
  face 0xF2 through 0x2F, then each face over both testing its own reference through its own mask, and reads both
  values back from the resolved depth.
- **Broken on purpose** (`breaks.py`): `s1a`, a back face's dynamic compare and write masks taken from the front's;
  `s1b`, a pipeline's own two-sided stencil dropped; `s1c`, a command for one face setting both. xsdk's `s1a` leaves
  `backface_enable` clear, and `s1b` writes the front's reference and masks into `RB_STENCILREFMASK_BF`.

**Step 2, clip distances** (built 2026-10-16, not yet run on the console). Xenos clips against six planes in clip
space, keeping where dot(the exported position, plane) >= 0, as Xenia applies them
(xenia:src/xenia/gpu/dxbc_shader_translator.cc:1033-1053): Direct3D 9's clip planes. xsdk gained
`xsdk_gpu_set_clip_planes`, `PA_CL_CLIP_CNTL`'s `ucp_ena` bits beside the clip space and `PA_CL_UCP_0_X` to
`PA_CL_UCP_5_W` (xenia:src/xenia/gpu/register_table.inc:623-646), which targets and resolves turn off. xost takes
`gl_ClipDistance[n]` written as `dot(gl_Position, plane)`, either way round, the position read back or the value
stored, the plane a whole `vec4` of a uniform block or push constants at a fixed index: it emits nothing for it and
names the plane's register in a need, `XSDK_XSC_CLIP_PLANE` (`("clip", n, register)` in Python's manifest); any other
form, and `gl_CullDistance`, is refused. On the way, gl_PerVertex's other members no longer pass for `gl_Position`,
which the last of their stores did. The bundle and the layer's shader cache carry the planes (`V4xShaderClipPlane`,
cache layout 2); a pipeline finds each in the vertex shader's push constants or uniform block, and each draw reads it
as bound and sets the planes, which a pipeline without them and `vkCmdClearAttachments` turn off. `shaderClipDistance`
is reported, `maxClipDistances` and `maxCombinedClipAndCullDistances` 6, under Vulkan's 8, and no cull distances;
docs/SUPPORT.md has the form as a deviation.
- **The checks:** vulkan4xenon_test's `test_clip_distances.c` draws quads cut by `clip.vert`'s pushed plane, at a
  column and then, pushed again, at a row, probed 4 pixels either side of each line; by `clip_planes.vert`'s
  distances 1 and 4 from a dynamic uniform buffer, to a quarter at offset 0 and the opposite quarter at 256; then a
  whole quad from a pipeline without clip distances, a `vkCmdClearAttachments` rectangle under a pipeline whose plane
  cuts everything, and the draw after it cut again; and checks the feature and limits. xsdk_test's raster test draws a
  row of quads cut by plane 0 at a column and planes 3 and 5 to a quarter, then one drawn with every plane off again;
  its compiler check compiles xost's `vk_clip_distance.vert` on the console into the PC's container and clip planes.
  xost's `tests/test_clip_distance.py` checks the planes' registers, the position exported alone, and the refusals.
- **Broken on purpose** (`breaks.py`): `s2a`, a pushed plane read from the push constants' start; `s2b`, the dynamic
  offset ignored, `vkCmdClearAttachments` leaving the planes on and `shaderClipDistance` unreported; `s2c`, a pipeline
  without clip distances leaving the last one's planes on. xsdk's `s2a` drops the `ucp_ena` bits and has xost name the
  next plane, and `s2b` writes the planes a plane's registers on.

**Step 3, point sprites** (built 2026-10-16, not yet run on the console). A Xenos vertex shader exports a point's
size in `oPts.x`, its full width in pixels, clamped to `PA_SU_POINT_MINMAX`'s radii in sixteenths of a pixel
(xenia:src/xenia/gpu/ucode.h:1813-1834, registers.h:433-453), and a pixel shader's parameters hold a point sprite's
coordinates in their zw (registers.h:184-212), as Xenia fills them for a point list
(xenia:src/xenia/gpu/dxbc_shader_translator.cc:735-748). xost exports `gl_PointSize` to `oPts.x` alone, setting the
record's `vs_export_mode` to 2, a sprite's second position vector (xenia:src/xenia/gpu/xenos.h:790-800), and lowers
`gl_PointCoord` to the parameters' |zw|, sharing their register with `gl_FragCoord`. xsdk's set-up leaves
`PA_SU_POINT_MINMAX` at 0 to 0xFFFF, unclamped, and keeps `PA_SU_POINT_SIZE` at a pixel, which a point takes without a
size; the register goes back as XAM had it at close. The layer reports `largePoints`, `pointSizeRange` 0 to
8191.875 and `pointSizeGranularity` 0.125, and needs nothing more at draw time.
- **The checks:** vulkan4xenon_test's `test_point_sprites.c` draws sprites of 16 and 48 pixels from `point.vert`,
  coloured by `point_coord.frag`, each probed 3 pixels inside its corners for its coordinates, within 32 a channel as
  where in a pixel Xenos takes them isn't known, and 3 pixels outside its edges for nothing; a point from `push.vert`,
  which writes no size, must be its one pixel alone; and checks the feature and limits. xsdk_test's raster test
  compiles xost's `vk_point_sprite.vert` and `vk_point_coord.frag` on the console, as the layer compiles what its
  bundle lacks, and draws sprites of 24 and 64 pixels with them, probed the same way; its compiler check compiles both
  into the PC's containers. xost's `tests/test_point_sprite.py` checks the size exported to x alone, the export mode,
  and the coordinates' |zw| on the simulator.
- **Broken on purpose** (`breaks.py`): `s3a`, xsdk's set-up clamping a vertex's size to a pixel and making a point
  without one two, and the layer's granularity 1. xsdk's `s3a` clamps the size alone, `s3b` has xost read the
  coordinates from the parameters' |xy|, and `s3c` leaves `vs_export_mode` 0, which fails the compiler check, and the
  sprite check too if the console needs the mode.

**Step 4, wireframe and point fill** (built 2026-10-16, not yet run on the console). `XsdkRaster` gained `fill`:
wireframe or points set `PA_SU_SC_MODE_CNTL`'s `poly_mode` to dual mode and both faces' polygon types to lines or
points (xenia:src/xenia/gpu/registers.h:463-469, xenos.h:846-859), as Xenia reads them
(xenia:src/xenia/gpu/vulkan/vulkan_pipeline_cache.cc:590-623); a target set, a resolve and the layer's own draws fill
again. A pipeline's `polygonMode` `LINE` and `POINT` take them, and `fillModeNonSolid` is reported; lines stay 1 pixel
wide.
- **The checks:** vulkan4xenon_test's `test_fill_modes.c` draws quads, their corners on pixels' centres, with
  `polygonMode` `LINE`, whose four edges and shared diagonal must show within a pixel and nothing inside, and `POINT`,
  whose four corners must show and nothing along an edge or the diagonal; and, culling back faces, a back-facing
  `LINE` quad, which must draw nothing, beside a front-facing one, which draws its edges; and checks the feature.
  xsdk_test's raster test draws a row of the same, a wireframe quad and a point-filled one.
- **Broken on purpose** (`breaks.py`): `s4a`, `LINE` and `POINT` each drawn filled and `fillModeNonSolid` unreported;
  `s4b`, against a copy of xsdk, back faces culled only when filled. xsdk's `s4a` drops dual mode, and `s4b` swaps the
  polygon types.

**On the console** (2026-10-16, the zero-fuse console; `results/2026-10-16-milestone-26/`):
- **vulkan4xenon_test passed 393 of 393, twice** (`vulkan4xenon_test.log`, `_2`): each face's stencil from the
  pipeline and from commands naming a face; quads cut by planes pushed before each draw and from a dynamic uniform
  buffer, nothing cut of a pipeline without them nor of `vkCmdClearAttachments`'s rectangle; sprites of 16 and 48
  pixels with `gl_PointCoord` across them, and a point without a size 1 pixel; `LINE` and `POINT` quads, and a
  back-facing `LINE` quad culled before its edges are drawn (xsdk:docs/HARDWARE.md, "Raster state, vulkan4xenon
  milestone 26").
- **What it settled:** back faces take `RB_DEPTHCONTROL`'s `_bf` fields and `RB_STENCILREFMASK_BF`; Xenos's user
  planes are in clip space, as Xenia has them; a point's size is its full width in pixels, unclamped by set-up; and the
  console needs `vs_export_mode` 2 for a shader's size, as xsdk's `s3c` failed the sprite check without it.
- **xsdk_test's first run failed its fill row,** a bug in the test: it negated an unsigned enum constant, placing the
  quad's bottom corners off screen. Fixed, xsdk_test passed 364 of 366, the two left the controller's, no button
  pressed (`xsdk_test.log`).
- **Broken on purpose,** each of the nine sets failed the checks `breaks.py` names for it and only those
  (`vulkan4xenon_test_broken_s1a.log` to `_s4b`), as did xsdk's nine
  (xsdk:results/2026-10-16-vulkan4xenon-milestone-26/).
- **Dusklight,** built clean against both with its committed shader pack, ran its title scene's attract loop for five
  minutes at 29.4 to 30.0 fps, 98 MB free, refusing only the 20x11 copy, as since milestone 14 (`dusklight_m26.log`).
  A run the user played went on into later areas at 29.6 to 30.0 fps between loads, down to 10.8 while several loaded
  together, and 49 MB free (`dusklight_m26_played.log`).

Milestone 26 is done.

### Milestone 27: copies on the GPU

**Why.** `vkCmdBlitImage` runs on the CPU once the GPU is done (docs/SUPPORT.md, Commands), which stalls a port that
scales a target every frame, as `StretchRect` does. And a render area must cover the whole framebuffer, so the copies
of part of a target that GameCube and N64 framebuffer effects make can't be drawn.
- **Blits of an image the GPU drew,** drawn as the layer draws images back (src/blit.c).
- **Render areas smaller than the framebuffer,** resolving part of a target (xsdk:TODO.md, "Resolving part of a
  target" and "Render areas smaller than the framebuffer").

Done when vulkan4xenon_test's blits and smaller render areas run without the wait, checked by pixel, the time saved
measured on the console.

**Decided (2026-10-16), in two steps,** one colour target and the formats the layer has, as milestones 20 and 25 take
the rest:
1. **Partial resolves and render areas smaller than the framebuffer.** xsdk resolves a rectangle of a target into the
   same texels, with the alignment the resolve hardware takes, and the layer clears, loads and stores a render area
   alone, `vkGetRenderAreaGranularity` giving that alignment.
2. **Blits on the GPU.** `vkCmdBlitImage` of a colour image a draw can sample draws its region into eDRAM at the
   destination region's size with the layer's own shaders, nearest or linear, then resolves that region into the
   destination; the replay no longer waits for the GPU for it. Blits the GPU can't take keep the CPU path; depth blits
   stay refused.

**Step 1, built** (2026-10-16; checks and breaks in `results/2026-10-16-milestone-27/`, the console's run to come).
- **xsdk** resolves a rectangle with `xsdk_gpu_resolve_texture_rect`, into the same texels of a texture of the target's
  size, clearing only it. Xenos copies the pixels its drawn rectangle covers, into the destination at the same
  coordinates, and clears what it copies (xenia:src/xenia/gpu/xenos.h:911-932, 941-942); it does so 8 by 8 pixels at a
  time (978-984). So the resolve's three corners, libxenon's (libxenon:drivers/xenos/xe.c:1074-1081), move to the
  rectangle's, and the window scissor, which a resolve keeps to (xenos.h:969-976), is set to it. A rectangle's offset
  is a multiple of 8, and its width and height too unless it reaches the texture's edge, past which it can write only
  the texture's padding: one reaching the right edge is resolved to the pitch, as a whole resolve is; any other is a
  caller's bug, caught by an assert. Clearing part of the depth writes nothing of hierarchical depth and leaves it off
  till the whole is cleared, as the rest of the buffer may no longer bound the depth under it.
- **The layer** takes render areas on that grid, refusing others, and `vkGetRenderAreaGranularity` gives 8x8, which is
  Vulkan's own rule for an optimal area. It tracks over which rectangle eDRAM holds its cleared values or an image:
  a clear resolves the area, a load draws the image back over the area alone, and the store resolves the area alone.
  Depth clears in the area alone, while a depth image's own clear covers all of it, drawn first as
  `vkCmdClearAttachments` draws when the area is smaller; a pass's resolve clears depth for the next pass only when
  their areas are the same, as eDRAM holds a depth image's depth around an area. A sampled depth image left to be read
  still takes all of eDRAM's depth.
- **Merged onto milestones 20 to 26,** a pass resolves each of its colour targets, in its own format, as milestone 25
  has it, and a whole area still does. xsdk resolves a rectangle of the first target alone, and in 8_8_8_8 alone, so a
  render pass of a smaller area takes one colour attachment a subpass, `R8G8B8A8_UNORM` or `B8G8R8A8_UNORM`, beside
  its depth; `vkCmdBeginRenderPass` refuses any other with a smaller area.
- **The checks:** xsdk_test's `test_resolve_rect.c` draws four coloured quarters meeting off the grid into a 204x116
  target, off the grid both ways, and resolves two rectangles into a texture of sentinels: one inside, one reaching the
  right and bottom edges. Every texel must hold the quarters inside the rectangles and the sentinel outside, and a
  resolve of the whole after them each rectangle's clear alone. Then depth cleared to 0.75 over the whole and to 1 in
  a rectangle must let a quad at 0.875 show in the rectangle alone, which a hierarchical buffer left on would reject.
  vulkan4xenon_test's `test_render_areas.c` reads a 124x92 target back texel by texel: a pass clearing an area inside
  it to green, with a red quad, after another target's pass left eDRAM magenta; a pass loading an area reaching its
  edges, over a corner of the first, with a yellow quad, the rest blue as the CPU cleared it; depth cleared to 1 in an
  area after 0.5 over the whole, a quad at 0.75 showing there alone; a depth image cleared to 0.25 outside a pass,
  then a pass of a smaller area loading it, a quad at 0.4 after hiding everywhere. The granularity is checked, and
  three areas off the grid are refused, as is a smaller area of a half float attachment.
- **Broken on purpose:** xsdk's `xsdk_test_breaks.py` and `builds.sh` in
  xsdk:results/2026-10-16-vulkan4xenon-milestone-27/, three titles: the rectangle at the corner, the whole target
  resolved, and hierarchical depth left on, which only the console fails, Xenia having none. The layer's `breaks.py`
  and `builds.sh` in `results/2026-10-16-milestone-27/`, three titles for step 1: six breaks a check each; every
  resolve of the whole framebuffer; and the image drawn back from the corner.

**Step 2, built** (2026-10-16; the console's run to come).
- **What the GPU draws:** a blit between two optimal images of one format a resolve writes, R8G8B8A8 or B8G8R8A8,
  from level 0 into level 0 of another image over 16 texels a side that fits eDRAM, its destination region on step
  1's grid. `vkCmdBlitImage` records the destination's rectangle and the texture coordinates at its top left and
  across and down it, from the source's corner that maps to each of the destination's, so a region mirrored either way
  mirrors the blit. The replay sets a target at the destination's size, colour from tile 0, and draws the rectangle
  with a new vertex shader, `blit_region.vert`, which takes those coordinates from a push constant, and `blit.frag`,
  through a sampler of level 0 alone, nearest or linear, edges clamped; then it resolves the rectangle into the
  destination, clearing nothing, and marks both images used by the submission. It waits for nothing: a later copy on
  the CPU that reads or writes either image waits for the submission's fence, as before. eDRAM's colour then holds the
  destination over the rectangle, and a depth its colour reached is lost.
- **What stays on the CPU:** every other blit the CPU took before, whole levels to whole levels of one format it
  unpacks, such as a chain of mips or R8 images; anything else is refused, as before. Encounter's Aurora blits nothing
  (aurora-xenon:lib/vulkan4xenon), so Dusklight's frame is the same.
- **Merged onto milestones 20 to 26,** the GPU takes 2D images of one layer in `R8G8B8A8_UNORM` and
  `B8G8R8A8_UNORM`, a region any on the grid, nearest or linear, as xsdk resolves a rectangle of 8_8_8_8 alone; the
  other formats milestone 25 draws keep milestone 20's blits on the CPU, which the console has run, as no check here
  draws theirs. The colour targets and depths its target reaches are lost. Its target set turns milestone 26's clip planes off, and the replay sets the pipeline, streams and textures
  again after it, as after a pass's begin. Everything else goes to milestones 20 and 23's blits on the CPU, a layer or
  slice at a time.
- **The checks:** vulkan4xenon_test's `test_gpu_blits.c` draws a 64x64 source of 8x8 cells, each its own colour, and
  blits it into rectangles of a 124x124 image the CPU filled with a sentinel: up 2 times and down 3 nearest, where no
  texel's centre falls on a source texel's edge; mirrored across by the destination and down by the source; up 2 times
  linear, and down 2 linear from an odd offset, so the filtered pairs straddle the cells; and a rectangle reaching the
  image's edges off the grid. Both images come back through `vkCmdCopyImageToBuffer`, and each rectangle's texels are
  checked against Vulkan's blit worked on the CPU from the source as read back, within 2 a channel nearest and 3
  linear, and the texels around them against the sentinel. Then the same work, 32 quads over a 1280x720 target and the
  source drawn again, comes before a blit drawn on the GPU and, in a submission of its own, before a blit into level 1
  of an image, which the CPU takes: the log gives each replay's time and when each submission was done, and the check
  wants the CPU's replay 4 times the drawn one's at least, as it waits for the GPU. The level is read back against the
  source, and a region off the grid inside the destination is refused.
- **Broken on purpose:** three titles for step 2 from `breaks.py`: linear blits sampled nearest, each blit's resolve of
  the whole destination, the drawn blit waiting for the GPU and regions off the grid taken, failing a check or two
  each; the source region's corner dropped; and the destination's mirror dropped. Step 1's areas off the grid, in set
  a, take blits' regions off it too.

**Not in milestone 27:** render areas smaller than the framebuffer in a subpass of several colour attachments, or of a
float or 10-bit one, which need xsdk to resolve a rectangle of any target in any format, past the agreed scope; blits
drawn in those formats, which stay on the CPU; areas off the 8-pixel grid inside the framebuffer; and blits drawn
from or into a level past the first, a cube's face or a 3D image's slice, or between formats, which the CPU takes or
refuses as before (docs/SUPPORT.md, Render passes and Commands).

**On the console** (2026-10-17, the zero-fuse console; `results/2026-10-16-milestone-27/`):
- **vulkan4xenon_test passed 413 of 413, twice** (`vulkan4xenon_test.log`, `_2`): passes over areas inside the
  framebuffer and reaching its edges off the grid cleared, drew, loaded and stored their areas alone, a depth image's
  clear covered all of it, and a smaller area of a half-float attachment was refused; blits drawn on the GPU matched
  Vulkan's blit worked on the CPU within 2 a channel nearest and 3 linear, mirrored, scaled up and down, and at the
  destination's edges, the texels around them kept. **The replay took 87 us of the CPU with a blit drawn, against
  24816 us with it on the CPU,** its submission done after 9.8 ms against 25.9 ms.
- **The first run failed one check** (`vulkan4xenon_test_granularity_1x1.log`): `test_commands.c` still wanted the
  granularity 1x1 from before this milestone, which reports the resolve's 8x8. It wants 8x8 now.
- **Broken on purpose,** each of the six sets failed the checks `breaks.py` names for it and only those
  (`vulkan4xenon_test_broken_a.log` to `_f`); set a's lists the commands check of the granularity too, and set b's the
  depth image's clear, which its whole resolves spoil.
- **xsdk_test passed 373 of 373** (`xsdk_test.log`), its rectangles of a target right (xsdk:docs/HARDWARE.md,
  "Resolving part of a target, vulkan4xenon milestone 27").
- **Dusklight,** built clean against both, ran its title scene's attract loop for five minutes at 29.6 to 30.0 fps,
  97 MB free, refusing only the 20x11 copy, as since milestone 14 (`dusklight_m27.log`).

Milestone 27 is done.

### Milestone 28: occlusion queries

**Why.** Some engines cull with them, though most can draw without. `ZPASS_DONE` hung the GPU in all six runs of
xsdk's probe (xsdk:docs/HARDWARE.md, "Queries and events").
- **xsdk:** the tries the probe has left (xsdk:TODO.md, "Occlusion queries").
- **The layer:** occlusion query pools, `vkCmdBeginQuery` and `vkCmdEndQuery`, on a sequence that writes the counts.

Done when a query counts a draw's samples on the console, or the probe's runs show none of the tries does, recorded in
xsdk:docs/HARDWARE.md.

**The probe's runs, planned (2026-10-16), none run yet.** xsdk's `tests/probe_occlusion.c` holds the tries, a build
each: `bash results/2026-10-16-vulkan4xenon-milestone-28/builds.sh` in xsdk builds `build/probe_occlusion_<try>.xex` for
all of them. Each runs from a fresh boot, as a try that hangs leaves the GPU hung once the title exits, and the group
tries must find the dashboard's state, not a title's. A log starts with the registers xsdk's set-up writes, as found and
as set up; then the probe draws a 256x128 quad and a quad hidden behind it and counts their pixels, 32768 of the quad's
colour and none of the hidden quad's if the draws are right; then the try counts across the same two draws. Its last
line says it finished, or the step it hung at, and how many of its events wrote their counts. In order, with what each
outcome means for the layer:
1. **`registers`:** Yamato's sample counters at 0x0F0F to 0x0F12, read by the CPU and copied by the command processor,
   without `ZPASS_DONE`; the one try that can't hang. If `RB_ZPASS_SAMPLES` rises by 32768 across the quad and not
   across the hidden quad, in the copies too, a query is the difference of two copies, at `vkCmdBeginQuery` and
   `vkCmdEndQuery`, each once the GPU is idle as a time stamp after work is, and the layer reports
   `occlusionQueryPrecise`. If the registers don't move, or move only as the CPU reads them, on to the next.
2. **`viz_query`:** D3D's visibility query. If the quad's bit is set and the hidden quad's and the empty query's are
   clear, the layer takes occlusion queries without `occlusionQueryPrecise`, under which a query that passed any sample
   may return any non-zero value (the Vulkan spec, "Occlusion Queries"): 1 here, the status register copied at
   `vkCmdEndQuery`, at once if the log's copy at the query's end already holds the bit and once idle if not, with up to
   64 queries in flight, their bits in two status registers (xenia:src/xenia/gpu/command_processor.cc:1624-1630). If
   the hidden quad's bit is set too, the answer is conservative, and whether the layer takes it, as a deviation in
   docs/SUPPORT.md, is the user's call.
3. **`reset`, `reset_copy`, `reset_event`, `flush` and `resolve`:** `ZPASS_DONE` with what the six runs didn't try
   around it. The first whose slots are written, with 32768 z passes across the quad and none across the hidden quad,
   is the layer's sequence: two records a query, the end's z passes less the begin's, A and B summed as D3D sums them
   (xenia:src/xenia/gpu/xenos.h:1525-1537), in the byte order the log's swapped and stored sums show; precise. What a
   flush or a resolve after each query costs is measured then.
4. **`no_counters`:** if its events don't hang, the hang is the counts' write to memory, and the flush, the resolve and
   the groups are where to look; if they do, the event alone wedges the render backend.
5. **`group_all`, then the seven groups:** if `group_all` writes the counts, xsdk's set-up leaves something that breaks
   them, the group runs find which group, a probe inside it which register, and the set-up keeps the console's value or
   puts it back around a query. If `group_all` hangs too, a group run could only differ where one group's state undoes
   another's, so they run last, if the user wants them.

The first try that works settles the layer's path, and the runs after it are for the record. If none does, the
milestone ends as above: xsdk:docs/HARDWARE.md records the runs, the layer keeps refusing occlusion query pools, and
docs/PARITY.md keeps their row.

**The runs** (2026-10-17, the zero-fuse console; xsdk c08f41d's probe, recorded in xsdk 5b4d6d4 and
xsdk:results/2026-10-16-vulkan4xenon-milestone-28/). None of the tries got sample counts: Yamato's registers held the
same values across the draws, and every try that sent `ZPASS_DONE` hung at its first one. The visibility query set its
bit for the quad and for the quad hidden behind it too, so it's conservative: it answers whether draws rasterized
anything, not whether a sample passed the depth test (xsdk:docs/HARDWARE.md, "Occlusion queries, vulkan4xenon milestone
28"). The user decided the layer takes it all the same, outcome 2's deviation without `occlusionQueryPrecise`.

**Step 1, built** (2026-10-17; checks and breaks in `results/2026-10-16-milestone-28/`, the console's run to come).
- **xsdk** begins and ends D3D's visibility query (xsdk 9c734eb): `xsdk_gpu_begin_visibility` sets `PA_SC_VIZ_QUERY`
  on with the id, 0 to 63, and sends `PM4_VIZ_QUERY`; `xsdk_gpu_end_visibility` sends the query's end, turns the
  register off, waits for the GPU to be idle, and copies the status word holding the id's bit, `_STATUS_0` for ids 0 to
  31 and `_STATUS_1` for 32 to 63, to memory, as the probe found it must: copied at the end, the word held the bits
  from before. Its end comes before anything after it can begin the id again.
- **The layer** makes occlusion pools, a slot a query: the status word, then the last visibility id the query took,
  written after it in the word a time stamp writes last, so availability, resets and copies are the time stamps'.
  `vkCmdBeginQuery` takes a query inside a render pass, or in a secondary command buffer continuing one, where the
  replay draws nothing of its own but `vkCmdClearAttachments`'s rectangles. Each query's draws go to id 0; a clear
  inside it ends the id and goes on under the next, in the same status word, so the clear doesn't count; past the
  31st clear, the ids are used up and the clears count. A query is 1 if any of its ids' bits is set. Every query
  waits for the GPU at its end, so a submission takes any number of them. `VK_QUERY_CONTROL_PRECISE_BIT` and queries
  begun outside a render pass, where the layer's resolves and blits would count, are refused.
- **The checks:** xsdk_test's `test_visibility.c` copies each query's status word: a quad's bit set, no draw's clear,
  a quad hidden behind it set, the conservative answer, id 1 begun again around no draw after its quad clear, and id
  63's quad set and 32's no draw clear in the second word. vulkan4xenon_test's `test_occlusion.c`: queries around a
  quad, nothing, a quad off the screen's right edge and one culled as a back face, the quad's hidden twin, a clear
  alone, a quad then a clear and a clear then a quad; query 0 reset and reused in three submissions, around nothing, a
  quad and nothing; 80 queries in one submission, quads and no draws in turn, read with `VK_QUERY_RESULT_WAIT_BIT`
  before the fence's wait; results copied at 64 bits with availability and at 32, and read with availability, beside
  a query left reset; a query in a secondary command buffer; and the refusals and features. `test_events.c` now wants
  only pipeline statistics pools refused.
- **Broken on purpose:** xsdk's `xsdk_test_breaks.py` and `xsdk_test_builds.sh` in
  xsdk:results/2026-10-16-vulkan4xenon-milestone-28/, beside the probe's `builds.sh`, three titles: the copy without
  the wait, every id read from the first word, and the id unshifted in the register. The layer's `breaks.py` and
  `builds.sh` in `results/2026-10-16-milestone-28/`, four titles: clears inside a query counted, and precise queries
  taken; the last id's bit read alone, and queries outside a render pass taken; resets leaving occlusion queries as
  they were, so one written before stays available; and a query's pool not marked used, so a wait for results doesn't
  wait.
- **What the console must settle,** each with the checks a wrong answer fails:
  - whether an id begun again around draws that rasterize nothing comes back clear, which every query after the first
    relies on: xsdk_test's id 1 begun again; vulkan4xenon_test's no draw, off screen, culled, clear alone, reused and
    80 queries checks;
  - whether a draw off the screen or culled rasterizes nothing to the query: the off screen or culled check;
  - whether ending an id around a clear keeps it out, and the id after counts: the clears check;
  - whether the hidden quad still sets the bit here, the conservative answer the probe found: xsdk_test's and
    vulkan4xenon_test's hidden quad checks, which a clear bit fails, a less conservative answer, to record;
  - whether ids 32 to 63 answer in the second word: xsdk_test's ids 32 to 63 check, which the layer doesn't use;
  - set a's and c's breaks of xsdk, which Xenia can't show: whether a copy without the wait holds the bits from before,
    as in the probe, and whether the register's id or the packet's picks the query.

**Not in milestone 28:** sample counts and `occlusionQueryPrecise`, which nothing public makes Xenos write; queries
begun outside a render pass; `inheritedQueries`; a query's clears past its 31st kept out; one wait for the GPU over
several queries' copies, which needs the ids to rotate and a measured need; and pipeline statistics.

**On the console** (2026-10-17, the zero-fuse console; `results/2026-10-16-milestone-28/`):
- **vulkan4xenon_test passed 426 of 426, twice** (`vulkan4xenon_test.log`, `_2`): a quad's query 1, no draw 0, a quad
  off screen 0 and one culled 0, a quad hidden behind another 1, the documented deviation; a clear alone 0 and a draw
  either side of it 1; query 0 reused over three submissions 0, 1 and 0; 80 queries in one submission right, which a
  wait for results waits for; copies and reads at 32 and 64 bits with availability; a query in a secondary command
  buffer; and the refusals.
- **What it settled** (xsdk:docs/HARDWARE.md, "Occlusion queries, vulkan4xenon milestone 28"): a query's end writes its
  bit, set or clear, so an id is reused once its word is copied; a draw off the screen or culled sets nothing; ending
  the id around a clear keeps the clear out; ids 32 to 63 answer in the second word; and `PA_SC_VIZ_QUERY`'s id, not
  the packet's, picks the bit the draws set. The status words outlast a title, so xsdk's break sets list only checks a
  bit left from before can't pass.
- **Broken on purpose,** each of the four sets failed the checks `breaks.py` names for it and only those
  (`vulkan4xenon_test_broken_a.log` to `_d`), as did xsdk's three (xsdk:results/2026-10-16-vulkan4xenon-milestone-28/).
- **xsdk_test passed 380 of 382,** the two left the controller's, no button pressed (`xsdk_test.log`).
- **Dusklight,** built clean against both, ran its title scene's attract loop for five minutes at 29.8 to 30.0 fps,
  99 MB free, refusing only the 20x11 copy, as since milestone 14 (`dusklight_m28.log`); neither it nor Aurora uses
  queries.

Milestone 28 is done.

### Milestone 29: the submit's cost of a draw

Agreed on 2026-10-19.

**Why.** Measured with vkbench on the console on 2026-10-19 (vulkan4xenon `d55706f`, xsdk `b150d87`), a cube costs the
CPU 0.966 us a draw through the layer and 0.289 through xsdk. Of the layer's cost, recording is 0.295 us and the submit
0.670. In `draws_4000` the submit takes 2,684 us a frame, and in `states_1000`, 1,268 us.

Each cube records two `vkCmdPushConstants` and a `vkCmdDrawIndexed`. At submit, for each draw:
- `replay_command` takes each command through its switch.
- The pushed bytes are copied into the replay's push array (src/queue.c:1314-1317), then read back out of it into
  constant groups.
- `apply_resources` loops over the pipeline's resources, working out each one's kind (src/queue.c:309-339).
- The fragment shader's push block is written whole, constants 0 to 4 (bench/shaders.c:57), the matrix among them,
  though mesh.frag reads only its tint at 4.
- The instance, piece and stream checks run before xsdk's draw.

When a pipeline is bound, `apply_pipeline` sets viewport, scissor, blend, raster and depth-stencil again, though they
haven't changed (src/queue.c:149-157, 1297-1300).

Milestone 8 found that a pushed draw writing three quarters fewer dwords cost the submit no less, so the dwords aren't
the cost (section 7). So the time is measured first.

1. **Measurement.**
   - A probe build of vkbench times each stage of the submit with the time base, in `draws_4000`, `states_1000` and
     `uniforms_1000`: the dispatch, the push copies, the resources, the constants, the draw's checks and xsdk's calls.
   - Dusklight's title scene logs its draws and submit time a frame, to weigh what a real title gains.
   - vkbench's xsdk backend waits as `xsdk_gpu_wait` does, a look every 5 us, instead of reading its fence without a
     break. Reading it without a break made a resolve 4.2% longer on the GPU (xsdk:docs/HARDWARE.md, "Looking at a
     fence costs the GPU").
   - The steps below are done in the order the measurement ranks them.
2. **A shader's push block covers only its stage's range.** Pipeline layouts keep their push constant ranges, so
   mesh.frag's block becomes its tint alone. The Vulkan spec lets a stage read only the push constants in its own
   range.
3. **Pushed constants read where they were recorded.** Pushes are 16-byte aligned in the command buffer's data and
   read from there, not copied into the replay first. Unaligned or partial pushes keep the copy, and so do the readers
   that need the values on the CPU (clip planes, loop counts).
4. **A draw plan made with the pipeline.**
   - At pipeline creation, flat lists of what each draw writes replace working out each resource's kind every draw.
   - A draw of one instance in one piece skips the instance, piece and stream checks.
5. **Pipeline state applied only when it changed.** Binding the pipeline that's bound already does nothing.
   Viewport, scissor, blend, raster and depth-stencil are compared with what was last applied. Render passes, blits
   and clears still force them again, as `pipeline_dirty` does now.
6. **Uploads.** Added on 2026-10-19 after step 1 as uploads that wait for the GPU. In Dusklight's attract movie,
   `vkCmdCopyBufferToImage` takes about 2.4 ms a frame over 2 copies (results/2026-10-19-milestone-29/). The
   profile's count of the replay's waits for the GPU then found that the copies wait for nothing: Aurora uploads each
   video frame into a new image. Their time is the CPU writing the texels into Xenos's tiled layout,
   `xsdk_texture_upload` at 117 MB/s for R8 (xsdk:docs/HARDWARE.md). Agreed the same day in its place: faster tiled
   uploads in xsdk, measured with xsdk_test's upload timing and Dusklight's movie.

Step 1 also found Dusklight's passes waiting about 10 ms a frame in xsdk, on its resolve rectangles: 8 sizes kept, a
9th waiting for the GPU in 1 ms sleeps. That's fixed in xsdk as its own change, agreed the same day.

Left out: translating a pass into an xsdk recording when it's recorded. A pass's begin and end depend on what eDRAM
holds at submit, the CPU's reads of uniform buffers happen at submit, indirect draws are known only at submit, and
recordings can't nest inside the tiled passes'.

Done when:
- `draws_4000`'s submit costs at most 0.40 us a draw, from 0.670, and `states_1000`'s at most 0.90, from 1.27;
- every vkbench picture is as before, and vulkan4xenon_test passes twice;
- Dusklight's title scene runs as before.

If step 1 shows these can't be reached by steps 2 to 5, the targets come back to the user with the measurement.

**The profile build,** step 1's measurement: `make clean && make V4X_PROFILE=1` builds the layer, the test title and
vkbench with the submit timed (src/profile.c); a normal build compiles its hooks to nothing. Each hook is a lap: it reads
the time base and charges the time since the last lap to one stage (`V4xProfileStage`, include/vulkan4xenon.h), so the
stages add up to the submit. The first submit times 10,000 laps back to back, to learn what one costs.
- **vkbench** logs `vulkan4xenon: profile, <scene>:` lines after each scene's measured frames through the layer: the
  counts, with the draws and microseconds a submit and a present, and what a lap costs; then each stage's nanoseconds a
  draw, as timed and less what its laps cost, with its laps a draw; then `all`, their sum.
- **Any title,** Dusklight among them, logs the same lines as `the last 10 s` at its first present after each 10
  seconds, so its frames are presents. Then each command type that took 2% of the submit or more, and the replay's
  waits for the GPU (step 6), all and at each place the replay waits, each in microseconds and times a present; a wait
  is counted in its stage as well. Since milestone 66's measurement of PPSSPP, also the app's own waits for fences
  outside the submit, and counts a present: uniform blocks the GPU loaded and the CPU read, push quads, pipelines
  applied, textures and streams pointed, and the bytes the CPU copied into and out of images and into buffers; the
  GPU's time on the submits by its clock, each submit's from when the GPU finished the one before; and once, after the
  third log of 300 draws a present or more, a line for each render pass of the next present: its image, size, ops
  and area, what its begin and end did, its draws, the textures it pointed, and its begin's and end's time.

**Closed on 2026-10-19 short of its targets, by the user's choice** (results/2026-10-19-milestone-29/):
- `draws_4000`'s submit costs 0.575 us a draw and `states_1000`'s 0.93 to 0.97. Most of what's left is xsdk's own
  constant setters, which cost the same through xsdk itself.
- vkbench's pictures and vulkan4xenon_test pass, and Dusklight's title scene runs as before.
- The two waits step 1 found outside the draw are gone:
  - xsdk keeps 64 resolve rectangles, which took Dusklight's submit from 10.5 to 11.9 ms a frame to about 1 ms;
  - the movie's uploads are faster, 460 to 544 us a frame from 2.35 ms.

Milestone 29 is done.

### Milestone 30: tiled passes that don't wait for the GPU

Agreed on 2026-10-20, with each decision below taken as recommended: measure first; the 2 MB space in the layer, 1 MB a
pass; Dusklight at 2 samples in this milestone if both scenes hold the bar; if the bar is missed, the GPU's part a
milestone of its own, a probe first; 4 samples measured, not a default.

**Why.** xsdk milestone 44 step 5 left Dusklight at 1 sample: at 2 samples its title scene ran 28.8 to 29.9 fps and
Ordon's village 27 to 28, under the 29.5 fps bar, where 1 sample runs 29.7 to 29.9 and 29 to 30 (xsdk:docs/HARDWARE.md,
"Multisampling's cost, and Dusklight at 2 and 4 samples, milestone 44 step 5"). The cost is the CPU's, in the replay at
`vkQueueSubmit`: in the logo shot's phase 0 it took 1.1 ms a frame at 1 sample, 11.2 at 2 and 12.8 at 4, and 29.0 at 1
sample with every pass with depth forced into 2 bands (xsdk:results/2026-10-20-milestone-44-step-5/, `dusk_ms*.log`).
The profile build's lines in those logs (milestone 29) say where:
- **Not in translating the draws.** A tiled pass's commands are already replayed once, into a recording each band runs
  (docs/SUPPORT.md, Render passes), and the stages a draw goes through cost the same in phase 0: dispatch, push,
  pipeline, constants, resources, checks and draws took 2.26 us a draw at 1 sample and 2.35 at 2, 249 draws a frame.
- **In the commands that aren't draws:** 0.39 ms a frame at 1 sample, 10.7 at 2. `vkCmdBeginRenderPass`'s replay took
  138 us a present over 27 passes in the title scene's last 10 s before the shot at 1 sample, 9,283 at 2, 10,315 at 4,
  and 25,566 to 27,061 forced into 2 bands; a pass's end 130 us at 1 sample and 258 at 2. In Ordon the begins took 127
  to 173 us a present at 1 sample and 9.1 to 17.2 ms at 2, where the submit took 13.0 to 19.3 ms a present against 1.0
  to 2.3, and Aurora's vertex cache waited 57 to 68 times for frames in flight, against none at 1 sample
  (`ordon_dusk_ms*.log`).
- **What a tiled pass's begin does that an untiled one's doesn't** is wait for the GPU to finish the recording its
  framebuffer's subpass made last, as each subpass has one recording (src/queue.c, `begin_tiled_pass`;
  src/render_pass.c, `make_recordings`), the shortcut docs/SUPPORT.md names. Aurora draws the EFB through one
  framebuffer it keeps (aurora-xenon:lib/vulkan4xenon/encoding.cpp, `target_framebuffer`) and breaks its pass at each
  EFB copy (section 5), so at 2 samples each EFB pass waits for the GPU to finish the one before. A wait that finds its
  fence not yet passed sleeps 1 ms at a time (xsdk:include/xsdk/gpu.h, `xsdk_gpu_set_wait_spin`), about 2 ms a sleep
  (xsdk:docs/HARDWARE.md, "Sleeps, write speeds, and a set-up that wouldn't run from a cached stream"). The profile
  counts only the copies' waits for the GPU (src/transfer.c), so it showed none. The waits are the likely cost, but
  haven't been timed apart.
- **The GPU's part isn't known yet.** Its time from submit rose from 13.2 to 17.0 ms a frame at 2 samples, but while
  the replay waits, the GPU can stand idle through each sleep's overshoot and the next pass's translation: it finished
  12.1 ms after the replay at 1 sample and 5.8 at 2.

So step 5's idea, a tiled pass translated once and run once a band, is how the layer works already; this milestone takes
out the wait.

**What it takes.**
1. **Measurement first,** in the profile build (`V4X_PROFILE=1`) and Dusklight's diagnostics builds at 1 and 2 samples,
   made as step 5's were, the logo shot run by the agent:
   - `begin_tiled_pass`'s wait for its recording counted with the replay's waits for the GPU, so the log gives its
     count and time a present;
   - the tiled passes a present, and the rest of their begins and ends, timed apart from the wait;
   - each tiled pass's recording in dwords, the largest and the sum a present, to size step 2's space.

   If the waits aren't most of the begins' 9.3 ms, the measurement comes back to the user before step 2.
2. **Recordings in one space the queue keeps,** in place of one a framebuffer subpass:
   - 2 MB of xsdk's GPU memory, taken at the first tiled pass, as the index copies' memory is (src/transfer.c,
     `copy_room`), and freed with the device. A framebuffer past eDRAM no longer takes 1 MB.
   - A tiled pass records from the space's next 128-byte line, where xsdk starts each segment of its stream, which the
     ring runs as indirect buffers (xsdk:src/gpu.c, `submit_segment`), with 1 MB of room, today's limit; the
     `XsdkCommandBuffer` it records into points there (xsdk:include/xsdk/gpu.h). Where 1 MB doesn't fit before the
     space's end, it starts again at the beginning.
   - Each pass's span is kept with its fence, in order. A pass whose room covers spans the GPU may still run waits for
     the last of their fences; with 2 MB and recordings of Dusklight's size (step 1), no pass should wait.
   - docs/SUPPORT.md's shortcut keeps 1 MB a pass and drops "a framebuffer used twice in a submission costs a wait".
3. **Dusklight at 2 samples by default,** once the runs below show it holds the bar: `DUSK_XSDK_MSAA`'s default in
   dusklight-xenon goes to 2.

**xsdk** needs nothing new. The bands' resolves go on the ring between runs of the recording, where `xsdk_gpu_end_tiles`
queues them (xsdk:src/gpu_tiles.c), so the change needs neither resolves nor fences inside a recording, which xsdk
doesn't have (xsdk:docs/PARITY.md, "Command buffers recorded once and run many times"). If step 1 puts the cost in
xsdk's calls instead, that's an xsdk change, scoped with the user then.

**New checks** in vulkan4xenon_test (tests/test_tiles.c), each well under a second:
- 8 passes in one submission on one 1280x720 framebuffer at 2 samples, with depth and a resolve attachment as Aurora's
  EFB has, each loading the last and adding a quad of its own, every texel of the resolve attachment checked: several
  recordings in flight at once.
- Passes whose recordings together pass the space's size, in one submission, so it starts again over spans the GPU may
  still run, every texel checked.
- The submission's time: the 8 passes, each keeping the GPU busy a few milliseconds, return from `vkQueueSubmit` in
  under a quarter of the GPU's time for them, where waiting for each pass takes seven eighths of it.

**Broken on purpose** (`results/<date>-milestone-30/breaks.py` and `builds.sh`):
- each tiled pass waiting for the last one's fence, as now: the submission's time check alone fails;
- room taken without waiting for the spans in it: the wrap check fails, or the GPU hangs, so that title runs last, from
  a fresh boot.

**Verification,** on the console:
- vulkan4xenon_test twice, every check passing, the tiles and multisample checks among them, and each break failing the
  checks it names and no others; by the agent.
- vkbench's 17 checks, its times within 3% of main's, as none of its scenes tiles; by the agent.
- xsdk_test and probe_tiles only if xsdk changes, which as scoped it doesn't.
- Dusklight at 1 and 2 samples, against the same xsdk and Aurora: the diagnostics builds' logo shot (the replay, its
  waits, the render worker, the GPU from submit) and the plain builds' title scene over five or more 10 s windows, by
  the agent; Ordon's village (F_SP103) and F_SP104 in the plain builds, played by the user, as in step 5. Nothing tiles
  at 1 sample, so its runs are the control. The logo shot's capture at 2 samples has its edges blended, as in step 5.
- 4 samples, the logo shot only, for the record.

**Done when:**
- the logo shot's replay at 2 samples costs at most 2 ms a frame, from 11.2 (1.1 at 1 sample);
- at 2 samples the title scene holds 29.5 fps or more in every 10 s window, and Ordon's village once settled; F_SP104,
  29.0 to 29.4 at 1 sample in step 5, runs no slower than 1 sample does in the same session;
- vulkan4xenon_test passes twice, each break fails what it names, and vkbench is as before;
- Dusklight defaults to 2 samples (step 3).

If the replay comes down and the frame rate still misses the bar, the milestone closes with the replay's cost and the
GPU's time a frame at 1 and 2 samples recorded, Dusklight stays at 1 sample, and the GPU's part comes back to the user
(Decisions, 4).

**Left out:**
- Translating a tiled pass when it's recorded, or keeping its recording from one submission to the next: the draws'
  translation costs the same at 1 and 2 samples, and milestone 29's reasons hold (a pass's begin and end depend on what
  eDRAM holds at submit, and uniform reads and indirect draws are known only then).
- Resolves and fences inside xsdk's recordings.
- Spinning in the wait instead (`xsdk_gpu_set_wait_spin`): the CPU would still keep step with the GPU pass by pass, and
  spinning slowed the thread sharing the waiter's core in Dusklight (xsdk:docs/HARDWARE.md, "Milestones 30 to 35
  together, and fence waits that sleep again").
- The GPU's extra work at 2 samples (Decisions, 4), and 4 samples by default.

**Decisions,** each taken as recommended on 2026-10-20:
1. **Measure first, or build step 2 straight away.** Step 1 is a few lines of the profile build and one run by the
   agent, and the waits are inferred from the begins' cost, not timed. Recommended: measure first.
2. **Where the space lives.** In the layer, 2 MB with 1 MB a pass (recommended: no xsdk change, the limit as it is, 1 MB
   more than today for Dusklight's one tiled framebuffer and less for a title with several); or 1 MB with 512 KB a
   pass, today's memory and half the limit; or xsdk recording a tiled pass into its own stream, an xsdk change, as a
   pass would need room past the stream's 64 KB blocks (xsdk:src/gpu.c, `STREAM_BLOCKS`).
3. **Dusklight's default:** 2 samples in this milestone if both scenes hold the bar (recommended), else it stays 1.
4. **The GPU, if the bar is missed.** At 2 samples each tiled EFB pass draws its colour back in every band and resolves
   it twice a band, into the multisampled image and into the resolve attachment, the same average both times
   (docs/SUPPORT.md, Render passes), where an untiled pass at 1 sample skips a load eDRAM still holds and resolves only
   its area. Recommended: a milestone of its own once this one's runs give the GPU's time without the idle gaps, a probe
   of those costs its first step.
5. **4 samples:** measured for the record, not a default (recommended), as its depth is in bands and drawn back by
   every pass that loads it, and it replayed 12.8 ms a frame in step 5.

**Size:** small. Step 1 is about 20 lines and one run by the agent; step 2 about 100 lines in src/queue.c,
src/render_pass.c and src/objects.h, the framebuffers' recordings taken out; the checks and breaks about 150; then the
runs above, of which only Ordon needs the user.

**Step 1's lines** (built 2026-10-20, run below): in a profile build, every `vulkan4xenon: profile,
<what>:` group, the 10 s ones and a title's own through `v4x_profile_log`, ends with a line on passes past eDRAM when
there were any: how many a present; their begins' waits for the GPU to finish their recording's last run, in us and
waits a present, and the rest of their begins; their ends, the bands' runs and resolves queued; and the dwords they
recorded, a present, a pass, and the most one pass recorded since the first submit. The waits count in `the replay's
waits for the GPU` too, and only those that found the GPU still running the recording.

**Step 1 on the console** (2026-10-20, the diagnostics builds of `6d301c6` against xsdk `abcfaa5`, run by the agent;
`results/2026-10-20-milestone-30/step1_dusk_ms1.log` and `step1_dusk_ms2.log`): the wait is the cost.
- **At 2 samples, the logo shot's phase 0:** 3.0 passes past eDRAM a present, the EFB's; their begins waited 10,014 us
  a present for the GPU, in 2.0 waits, and took 36 us besides; their ends 143 us. Of the submit's 11,293 us a present,
  the waits are 89%. The replay took 11.3 ms a frame, the GPU 17.1 ms from submit and 5.8 after the replay, 29.1 fps.
  Phase 1, lit draws skipped: 4,091 us waiting in 2.0 waits, 48 us besides, the GPU 10.2 ms from submit and 5.1 after
  the replay.
- **The title scene's 10 s windows at 2 samples:** 9.6 to 14.9 ms a present waiting, in 1.7 to 2.0 waits, of
  `vkCmdBeginRenderPass`'s 9.7 to 15.0 ms; the submit 10.7 to 15.9 ms a present.
- **The recordings are small:** 13,928 dwords a present in phase 0, 4,643 a pass, and 20,642 at most since the first
  submit, 81 KB, a thirteenth of a pass's 1 MB.
- **At 1 sample** no pass is past eDRAM, so no such line; phase 0's replay took 1.0 ms, the GPU 13.2 ms from submit,
  29.0 fps.

**Step 2, built** (2026-10-20; the console's run to come):
- **The space is 2.5 MB, not 2.** A pass keeps 1 MB of room, today's limit. Where the room doesn't fit before the
  space's end, it's taken from the start again, and in 2 MB that room would cover the pass just recorded, which the GPU
  is likely still running, at every lap. In 2.5 MB the passes the room covers then were followed by at least half a
  room of recordings, about 6 to 9 of Dusklight's frames at 14,000 to 21,000 dwords a present (step 1).
- **Taken with the first framebuffer past eDRAM,** not at the first such pass as the scope had it, so a lack of memory
  fails `vkCreateFramebuffer` with `VK_ERROR_OUT_OF_DEVICE_MEMORY`, as a framebuffer's own recording did, rather than a
  submit; framebuffers made on two threads at once keep one space. It's freed with the device. A framebuffer no longer
  takes 1 MB of its own.
- **Each pass records from the space's next 128-byte line** into an `XsdkCommandBuffer` pointing there, with 1 MB of
  room (src/queue.c, `tile_room`); after its bands it's kept as a span with its fence, up to 64 of them, oldest first.
  A pass whose room covers spans waits for the newest of them, as the fences land in order, and drops them; with all 64
  kept it waits for the oldest. The profile's lines count these waits as before.
- **New checks** (tests/test_tiles.c), on the 2-sample framebuffer with depth and a resolve attachment:
  - 8 passes in one submission, each loading the last and drawing a full-height strip of its own, nearer than the last,
    every texel of the resolve attachment checked;
  - 12 such passes, each with 3,000 quads off screen after its strip, the first with 300 full-screen quads in the
    clear's colour before it so the GPU lags: at 15 to 87 dwords a recorded quad, each pass stays under 1 MB and the
    space starts again over passes this submission recorded; every texel checked, and the submit's time and the fence's
    logged;
  - 8 passes of 16 full-screen quads each: `vkQueueSubmit` returns in under a quarter of the time until their fence.
- **Broken on purpose** (`results/2026-10-20-milestone-30/breaks.py` and `builds.sh`): `wait_last`, each pass waiting
  for the one before, should fail the submit's time check alone; `no_span_wait`, rooms taken without waiting for the
  spans in them, the wrap check, or hang the GPU, so it runs last, from a fresh boot.

**Step 2 on the console** (2026-10-20 and 21, `e3c5a6e` against xsdk `06e13ff`; `results/2026-10-20-milestone-30/`,
the `step2_*` and `ordon_*` logs):
- **vulkan4xenon_test** passed 520 of 520 twice. The 8 busy passes returned from `vkQueueSubmit` in 390 and 391 us,
  their fence 42.5 and 42.3 ms after; the 12 passes recorded past the 2.5 MB space came out with every texel right.
- **The breaks:** `wait_last` failed the submit's time check alone, 42,522 us of 48,475; `no_span_wait` hung the GPU,
  as warned. vkbench passed its 17 checks; its times weren't measured against main's in the same session.
- **The logo shot's phase 0, at 2 samples:** the replay took 1.2 ms a frame, from 11.2, against 1.0 at 1 sample; the
  passes' begins waited 0 us for the GPU; the GPU took 16.0 ms from submit, 13.1 at 1 sample; 29.0 and 29.1 fps. At 4
  samples the shot landed on a lighter frame, 1757 against 1921 to 1926 (72 draws a present against 249), so its
  phases don't compare; its replay took 0.5 to 1.0 ms a frame without waits.
- **The title scene,** plain builds by the agent, 32 windows: 1,151.6 frames a second summed at 1 sample and 1,149.9
  at 2, each with four of its last 25 windows under 29.5, the lowest 28.8 and 28.7.
- **Ordon,** plain builds played by the user: the village (F_SP103) averaged 27.25 fps at 1 sample, 27.23 at 2 and
  27.80 at 4, and F_SP104 29.10, 29.05 and 28.82, each route the user's own.

**Closed on 2026-10-21, with Dusklight at 4 samples by the user's choice:**
- The replay at 2 samples costs 1.2 ms a frame, under the 2 ms bar.
- The 29.5 fps bar is missed at 1 sample as at 2 and 4, in the title scene and Ordon's village alike, so the shortfall
  isn't multisampling's; the user left the frame rate for later. F_SP104 runs as at 1 sample within a route's noise.
- Decision 5 had 4 samples measured, not a default; the user chose 4, as Ordon ran as fast at 4 as at 1 and 2, and
  `DUSK_XSDK_MSAA`'s default in dusklight-xenon is 4. The title scene at 4 samples isn't recorded: the agent's run of
  it lost its log.

Milestone 30 is done.

### Milestone 31: colour attachments 16 texels or less on a side

Agreed with the user on 2026-10-21, with every recommendation below.

**Why.** Dusklight's bloom loses its smallest copy on the console. In Ordon the layer refused one image 6 times, and
Aurora logged it each time (xsdk:local/m30/ordon/ms1/dusk_ms1.log:741-747, 1305-1307, 2693-2695, 4630-4632,
5512-5514; the other four logs beside it, 5 to 7 times each):

```
vulkan4xenon: not supported: colour attachments with mips, or 16 texels or less on a side
[ERROR | aurora::vulkan4xenon] vkCreateImage: -8
[ERROR | aurora::vulkan4xenon] No image for Resolved Texture (20x11)
```

It's the copy target milestone 11's step 1 found, the only one 16 texels or less on a side
(results/2026-10-12-milestone-11-step-1/counts-xenia.txt:53-65), and every run since logs it ("refusing only the 20x11
copy, as since milestone 14", milestones 19 to 28 above).

- **Who asks.** Dusklight's own bloom, `BloomMode::Dusk`, the default (dusklight:src/dusk/settings.cpp:78;
  dusklight:src/m_Do/m_Do_graphic.cpp:1701-1705). It halves the 1280x720 render size six times, down to 20x11 at
  level 6 (m_Do_graphic.cpp:1451-1452, 1478-1523, 1634-1635), and copies with `CopyToTexObj`: RGBA8, no mipmap, clamped
  and linear (m_Do_graphic.cpp:923-928). Two copies a frame are 20x11:
  - going down, level 5's 40x22 into a 20x11 texture, which the blur at level 6 then samples
    (m_Do_graphic.cpp:1647-1660);
  - going up, level 6 into another 20x11 texture, blended into level 5 with `GX_BL_SRCALPHA, GX_BL_ONE` at an alpha of
    0.25^(1/5), 0.76 at the default multiplier (m_Do_graphic.cpp:1666-1674; settings.cpp:79).

  Depth of field's chain stops at 80x45 (m_Do_graphic.cpp:930-1018), so nothing else is that small at 1280x720.
- **What Aurora makes of it.** A GX copy becomes `resolve_copy_texture`
  (aurora-xenon:lib/dolphin/gx/GXFrameBuffer.cpp:46-56), which makes a "Resolved Texture" through `new_render_texture`
  for an RGBA8 copy (aurora-xenon:lib/gx/texture.cpp:794-796; lib/gfx/texture.cpp:141-145). The backend makes that an
  `R8G8B8A8_UNORM` image, optimal, one level, one sample, `COLOR_ATTACHMENT | SAMPLED`
  (aurora-xenon:lib/vulkan4xenon/backend.cpp:288-314, 327-329), and draws the copy into it as a render pass
  (encoding.cpp:918-953). Refused, the texture has no image: the copy returns before drawing (encoding.cpp:931-934)
  and draws that sample it get the empty texture, all zeros (backend.cpp:112, 159-160). So the blur at level 6 reads
  black and the up chain adds nothing from it: bloom's widest level is missing. Carried up through alphas 0.76, 0.71,
  0.63 and 0.5 (m_Do_graphic.cpp:1668), it would reach the quarter-size bloom at about 0.17 of its own brightness.
- **Why the layer refuses.** xsdk lays out every texture with the fetch constant's packed mips on
  (xsdk:src/texture.c:89-90, 279), as libxenon does (libxenon:libxenon/drivers/xenos/xe.c:1207, dword 5's 0xA00). A
  base 16 texels or less on a side then sits in the packed tail (xsdk:src/texture_layout.c:55-59), at (0, 16) for a
  wide one like 20x11 and at (16, 0) for a square or tall one (xsdk:src/texture_layout.c:79-98), where the console
  reads it (xsdk:docs/HARDWARE.md, "A small texture's mips share its base's tail" and "A 2x1 texture's tail"). A
  resolve copies the target's rectangle into the same texels of the texture, from its corner
  (xsdk:src/gpu_resolve.c:451-466; the destination's offset is the source's, xenia:src/xenia/gpu/draw_util.cc:1015-1031;
  on the console, xsdk:docs/HARDWARE.md, "Resolving part of a target, vulkan4xenon milestone 27"). It can't reach
  (0, 16) from a 20x11 target at (0, 0), so xsdk asserts the base isn't in the tail
  (xsdk:src/gpu_resolve.c:456-457, 494, 510), and the layer refuses such attachments
  (src/image.c:154-156; docs/SUPPORT.md:467-469). Let through, the resolve would write rows 0 to 10 and the GPU would
  sample rows 16 to 26.
- **Every case seen.** In the 1,131 logs under xsdk:local, xsdk:results and results/ that hold either line, the "No
  image for" lines are 1,149, every one `Resolved Texture (20x11)`. A log's refusals equal its 20x11 lines (504 logs),
  or exceed them by exactly 2 (610 logs): vulkan4xenon_test's own check, which makes a 16x64 attachment and a 64x64 one
  with 2 levels to see them refused (tests/test_objects.c:78-102). 17 logs fall 2 short: Dusklight's own log files
  from Xenia recordings, which carry Aurora's line and not the layer's. So Dusklight asks for one size, 20x11, and
  never for mips: the backend makes every copy, conversion and render target with one level and gives `desc.mips`
  only to sampled images (backend.cpp:305-314).
- **What isn't known.** The fetch constant's packed mips is a bit of its own (xenia:src/xenia/gpu/xenos.h:1281). In
  Xenia's model a texture without it has no tail and its base sits at the corner, whatever its size
  (xenia:src/xenia/gpu/texture_util.cc:251-253; texture_util.h:76-77). Nothing has shown that on the console: xsdk and
  libxenon always set it, and Xenia was wrong about small textures' tails twice before (both HARDWARE.md sections
  above). Xenia can't settle it, as it reads the corner by its own model either way.

So the fix is to give a small colour attachment a texture with packed mips off, its base at the corner where a resolve
writes, once a probe shows the console reads it there.

**What it takes.**
1. **The probe first** (xsdk: tests/probe_mip_tail.c, about 30 lines). It already draws each level of small textures
   one texel to a pixel from four pages whose every texel names its page and place, under two setups
   (xsdk:results/2026-10-10-probe-mip-tail/xsdk.log; xsdk:results/2026-10-13-milestone-29/probe_mip_tail_thin.log).
   A third setup clears packed mips in the fetch constant it builds (its `make_texture`, which already rewrites dwords
   1 and 5, xsdk:tests/probe_mip_tail.c:110) for one-level shapes that cover each of the tail's placements: 20x11
   (wide, Dusklight's), 11x20 (tall), 16x16 (square), 32x16 (wide, the long side over 16), 2x1 (thin, placed as a
   square), 4x1 (thin, placed as wide) and 1x1. The two setups it has run unchanged, as the control.
   - **Expected:** every pixel of the third setup reads its own texel (x, y) on the base page, from the corner.
   - **One launch, by the agent.** A fetch only reads, and the probe ran six shapes, then eleven thin ones, under both
     setups in one launch each; the four coded pages catch a read past them.
   - **If the console reads anywhere else,** the probe's codes say where, and the milestone comes back to the user
     (Decisions, 5) before step 2.
2. **xsdk: a texture's base at its corner** (about 25 lines):
   - `xsdk_texture_base_at_corner(XsdkTexture *texture)` in include/xsdk/texture.h, beside `xsdk_texture_swizzle`: for a
     one-level 2D texture whose base would sit in the packed tail, packed mips off in its fetch constant; for any other,
     nothing (asserted one level and 2D).
   - The layout follows the bit: `TextureLayout` gets `packed`, from the fetch constant in `layout_of`
     (xsdk:src/texture.c:204-214), and `xsdk_texture_place` puts level 0 in the tail only when it's set
     (xsdk:src/texture_layout.c:123), so uploads, downloads and the layer's CPU copies read and write the corner.
   - The resolves' four asserts take a private `xsdk_texture_base_in_tail(texture)`, false with packed mips off
     (xsdk:src/gpu_resolve.c:456-457, 494, 510).
   - docs/HARDWARE.md records the probe; include/xsdk/texture.h says why in one line.
3. **The layer** (about 10 lines):
   - src/image.c:154-156 refuses colour attachments with mips, and multisampled ones 16 texels or less on a side
     (Decisions, 2); `vkCreateImage` calls `xsdk_texture_base_at_corner` for every colour attachment, after
     `init_texture`, which changes only those 16 texels or less on a side.
   - src/image.h:27-29's comment says what `V4X_PACKED_SIDE` still limits: sampled depth images, multisampled colour
     images and drawn blits (src/image.c:102-105; src/command_buffer.c:612-619).
   - docs/SUPPORT.md:467-469: colour attachments of one level at any size, one 16 texels or less on a side with its
     base at the corner, packed mips off, as the console reads it (xsdk:docs/HARDWARE.md, the probe's section).
4. **Dusklight:** no change to Aurora or Dusklight. Its builds take the new xsdk and layer, and the runs below show the
   copies made and drawn.

**New checks.**
- **xsdk_test** (tests/test_sampling.c): a 20x11 and an 11x20 texture of one level with their bases at the corner,
  uploaded with coded texels and drawn one texel to a pixel, every pixel its texel.
- **vulkan4xenon_test** (new tests/test_small_targets.c, each check well under a second), on 20x11, 11x20, 16x16 and 2x1
  `R8G8B8A8_UNORM` images of one level, each drawn texel for texel from a coded source of its own, as Aurora draws a
  copy, so a texel from the tail or from another image shows:
  - read back by `vkCmdCopyImageToBuffer`, every texel right: the resolve wrote where xsdk's layout reads;
  - sampled nearest, 4 pixels a texel, into a 128x64 attachment that's then read back, every pixel its texel: the GPU
    reads where the resolve wrote.
- **tests/test_objects.c:78-102** changes: a 16x64 attachment at one sample is made; a 64x64 one with 2 levels, and a
  16x64 one at 2 samples, are still refused.

**Broken on purpose** (`results/<date>-milestone-31/breaks.py` and `builds.sh`, building an xsdk copy as milestone 21's
`builds.sh` does):
- `fetch_packed`: `xsdk_gpu_set_texture` ORs packed mips back into every fetch constant it writes
  (xsdk:src/gpu_draw.c:612-615), so the GPU reads the tail while the resolves and the CPU use the corner:
  vulkan4xenon_test's sampled check fails, its read-back passes; xsdk_test's new check fails.
- `place_tail`: `xsdk_texture_place` puts level 0 in the tail whatever the bit: the read-back check fails, the sampled
  one passes.

Each touches only textures with packed mips off, so no other check should fail. Neither can hang the GPU.

**Verification,** on the console, all by the agent:
- the probe, once (step 1);
- xsdk_test, every check passing, and `fetch_packed` failing its new check alone;
- vulkan4xenon_test twice, every check passing, each break failing the check it names and no others;
- vkbench's 17 checks, nothing it draws being that small;
- Dusklight's diagnostics build, built as local/m44c's are, at the default 4 samples, against the same build without
  the fix at the same xsdk, layer and Aurora commits otherwise:
  - no `colour attachments with mips` and no `No image for` line anywhere in the log;
  - phase 0's `copies` 2.0 a frame more than without the fix, at the same shot frame (12.0 in
    results/2026-10-20-milestone-30/step2_dusk_ms1.log:986-993): the two 20x11 copies drawn
    (xsdk:local/m44c/aurora-diag.patch:214-218 counts a copy only once it's drawn);
  - `dusklight_shot_start.ppm` from each build, and their difference filed in results: a wide, soft glow around the
    bright parts, and the logo's moving shine, which differs between two runs of one build too
    (docs/ROADMAP.md:1184-1186);
  - phase 0's fps and GPU ms a frame within the noise of the build without the fix: two 20x11 passes a frame;
- Dusklight's plain build, the title scene's 10 s windows, with neither line.

No Xenia run is needed; Xenia would pass the new checks by its own model and can't tell (Why).

**Done when:**
- the probe shows every one-level shape read from its corner with packed mips off;
- xsdk_test and vulkan4xenon_test pass in full, vulkan4xenon_test twice, each break failing what it names alone, and
  vkbench passes;
- Dusklight's runs refuse no image, draw two more copies a frame, run as fast as before, and the shot shows the glow;
- docs/HARDWARE.md, include/xsdk/texture.h and docs/SUPPORT.md say what changed.

**Left out:**
- Colour attachments with mips: resolving into a level other than 0 is new xsdk work, and Dusklight never asks.
- Sampled depth images 16 texels or less on a side (src/image.c:102-105): Aurora makes them unsampled
  (backend.cpp:316-319), and no log refuses one.
- Drawn blits to or from small images (src/command_buffer.c:612-619), which stay on the CPU: Dusklight's copies are
  draws, not blits.
- Unpacking every one-level texture (Decisions, 1).

**Decisions,** each with a recommendation:
1. **Which textures lose packed mips.** Only colour attachments whose base would sit in the tail (recommended: the
   change reaches nothing else, and the small sampled textures Dusklight uploads keep the layout the console has run
   since xsdk milestone 15); or every one-level texture, which drops the tail rules for them, Xenia's 2x1 difference
   with them, but changes every small texture Dusklight draws ahead of the beta.
2. **Small multisampled colour images.** Still refused (recommended: Aurora's copies are one sample,
   aurora-xenon:lib/vulkan4xenon/backend.hpp:189-191, and no check would cover them); or let through, as the same
   resolve writes them, with a check of their own.
3. **The xsdk call.** A setter after `xsdk_texture_init`, as `xsdk_texture_swizzle` and `xsdk_texture_signs` are
   (recommended: the layer's `init_texture` stays as it is); or an `xsdk_texture_init` for targets.
4. **Ordon.** Not run again (recommended: the title scene makes the same two copies every frame,
   results/2026-10-20-milestone-30/step2_dusk_ms1.log:843-846, so no one needs to play it); or the user plays the
   village once on the beta build.
5. **If the probe shows the console reads a small base from the tail anyway.** Back to the user with the probe's
   result (recommended). The likely way then is to draw such an attachment into eDRAM at its base's place in the tail
   and resolve that rectangle, as a resolve writes the same texels it copies (Why): every draw, clear and load into it
   offset, in the layer and in xsdk, a larger change. Leaving out bloom's level 6 in Dusklight (`divNum` 5) would
   change the look on the PC too, and isn't recommended.

**Size:** small. The probe about 30 lines and one launch; xsdk about 25 lines and a check of about 60; the layer about
10 lines, its checks about 150 and the breaks about 60; no change to Aurora or Dusklight. Every run is the agent's,
nothing needs the user.

**On the console** (2026-10-21, `0b64e7d` against xsdk `f62488e`; xsdk's
`results/2026-10-21-vulkan4xenon-milestone-31/` and `results/2026-10-21-milestone-31/`):
- **The probe:** all seven one-level shapes, packed mips off, read from their corner as one rectangle on the base
  page, 20x11 from page 0 (0, 0) to (19, 10); the 62 control lines are those of milestone 29's run, line for line.
- **vulkan4xenon_test** passed 523 of 523 twice: the four small targets read back with no texel wrong and sampled
  with 0 of 8,192 pixels wrong; vkbench passed its 17 checks.
- **xsdk_test** passed 634 of 636, its new sampling check among them; the two failures are the controller's, with
  none answering its poll.
- **The breaks** failed what they name and nothing else: `fetch_packed` the sampled check in vulkan4xenon_test
  (2,792 of 8,192 pixels wrong) and xsdk_test's new check; `place_tail` the read-back (every texel wrong) and xsdk_test's
  new check, which uploads through the same layout.
- **Dusklight's title scene,** plain builds against main and against this milestone from one Dusklight tree: the
  layer's refusal and Aurora's `No image for Resolved Texture (20x11)` are gone (2 of each against none), and the
  frame rate matches window for window (59.4 to 60.0 fps before the scene, 48.6, 29.8, 28.8 and 30.0 in it). The
  600th frame's captures don't show the glow: the scene's camera moves in real time, so the two runs' frames sit
  apart, and that, not bloom's level 6, is most of their difference. The copies being made and drawn rest on the
  refusals gone and on vulkan4xenon_test's checks of such targets.

Milestone 31 is done.

### Milestone 33: graphics options that spend Dusklight's idle GPU

Agreed with the user on 2026-10-21, with every recommendation below; the higher internal resolution stays out, as
decision 3 recommends and the user chose again the same day.

Three options in Dusklight's own menu, each defaulting to what Dusklight draws today: anisotropic filtering forced
for the game's textures, sharper shadows, and a higher internal resolution. Players reach them through the menu, which
milestone 16 makes draw on the console; nothing here waits for it, as the measurement build sets the options itself.

**Why.** Dusklight is held by the CPU, and the GPU has time to spare:
- **The frame rate doesn't follow the GPU.** Ordon's village averaged 27.25 fps at 1 sample, 27.23 at 2 and 27.80 at
  4, the same routes played by the user (milestone 30's record).
- **The main thread is busy the whole frame.** In the user's run of 2026-10-21 at 4 samples, through the village, the
  ranch, the spring and Faron Woods (`Z:\Xbox-360-SDK\local\beta\timing\run1\dusk_timing.log`, a line a 10 s window),
  the main thread's update took 8.6 to 21.9 ms a frame and its draw 11.1 to 23.5, their sum with
  `aurora_begin_frame` and the rest the whole frame, at 27.3 to 30.0 fps. Work added to it costs frame rate directly.
  The FIFO worker took 11.0 to 32.4 ms a frame, the render worker 4.2 to 22.3.
- **The GPU took 12.1 to 23.9 ms a frame,** about 18 on average: 9.4 to 21 ms short of a 33.3 ms frame. The village's
  windows took 14.8 to 23.9, the heaviest at 27.4 fps (`dusk_timing.log:5208-5211`). At 1 and 2 samples the village
  took 9.7 to 15.6 and 13.8 to 20.1 ms (`Z:\Xbox-360-SDK\local\m30\ordon\ms1`, `ms2`, the diagnostics builds).
- **But the GPU can hold the main thread before it fills the frame.** `aurora_begin_frame` waits for a frame slot
  (aurora-xenon:lib/vulkan4xenon/encoding.cpp:1219-1223); there are two (aurora-xenon:CMakeLists.txt:65), and a slot
  comes free only when the GPU finishes the frame two back (aurora-xenon:lib/vulkan4xenon/device.cpp:600-621). The
  run's `begin` took up to 5.3 ms a frame in the spring (`dusk_timing.log:3059-3061`), 4.5 in the village
  (`:4271-4273`) and 1.7 in the village's heaviest window, while the GPU stayed under 24 ms. Where `begin` already
  waits, GPU time added to a frame likely lengthens the main thread's frame one for one. How much of `begin` is the slot's
  wait isn't timed apart; step 4 does.
- **Memory is tight.** The run had 42 to 73 MB free while played (`dusk_timing.log`, the `Memory:` lines); milestone
  30's plain 4-sample build fell to 28 and 20 MB free on a second visit to the village, newlib's heap at 292 MB
  (results/2026-10-20-milestone-30/ordon_dusk_ms4_plain.log:4282, 5182). Memory, not the GPU, bounds the shadows' top
  value.

So each option must cost the GPU little, the main thread nothing, and memory a known amount, and keep today's look
unless a player picks otherwise.

**The options as Dusklight has them.**
- **Anisotropic filtering:** none. Aurora's `maxTextureAnisotropy`, 16 unless set (aurora-xenon:lib/aurora.cpp:75-76),
  only scales a texture's own GX_ANISO_2 or GX_ANISO_4 (aurora-xenon:lib/gfx/texture.cpp:49-60), and Twilight
  Princess asks for none: no texture on its disc sets its own, and Ordon's GPU time was the same with 16 and 1
  (milestone 14's record; results/2026-10-15-milestone-14-steps-5-6/anisotropy_frame_times.txt). The PC's WebGPU path
  draws it unfiltered too, and upstream Dusklight has no such setting (dusklight-xenon:src/dusk/settings.h; upstream
  `77705f8bb9` the same).
- **Shadows:** `game.shadowResolutionMultiplier`, 1 by default (dusklight-xenon:src/dusk/settings.cpp:85), 1 to 8 in the
  menu (dusklight-xenon:src/dusk/ui/graphics_tuner.cpp:148-150).
- **Internal resolution:** `game.internalResolutionScale`, 0 by default (settings.cpp:84), 0 to 12 in the menu
  (graphics_tuner.cpp:145-147).

**What it takes.**

1. **Anisotropic filtering, forced: Off, 2x, 4x, 8x, 16x.**
   - **Aurora,** in its shared code, so every backend takes it: `aurora_set_anisotropy(uint16_t)` in
     include/aurora/aurora.h beside `aurora_set_resampler`. `TextureBind::sampler_config` (lib/gfx/texture.cpp:200-234)
     takes the larger of it and the game's own for a texture with mips, filtered linearly within a level
     (`GX_LIN_MIP_NEAR`, `GX_LIN_MIP_LIN`), whose levels are its own (not `hasArbitraryMips`); every other texture keeps
     the game's, so nothing filtered nearest or without mips changes. Aurora's rule then filters those textures
     trilinearly (texture.cpp:219-223), as forced filtering does on the PC. The value is an atomic, since the setting's
     callback runs on the main thread and `sampler_config` on the FIFO worker (lib/gx/command_processor.cpp:688), and
     the setter calls `gx::texture::invalidate_bindings` (lib/gx/texture.cpp:859) so the FIFO worker's cached bind
     groups are rebuilt (command_processor.cpp:683-695).
   - **The backend** clamps it to the layer's 16 already (aurora-xenon:lib/vulkan4xenon/backend.cpp:240).
   - **Dusklight:** `game.anisotropicFiltering`, 0 to 4 for Off to 16x, 0 by default, registered with a callback that
     clamps it and calls `aurora_set_anisotropy(1 << value)`, applied at start beside
     `applyInternalResolutionScale` (src/m_Do/m_Do_main.cpp:766); a `GraphicsOption` (src/dusk/ui/graphics_tuner.hpp,
     .cpp) and a "Texture Filtering" entry in the menu's Resolution section (src/dusk/ui/settings.cpp:793-811).
   - **Values:** the powers of two, as xsdk rounds the rest down (xsdk:src/texture.c:675-678).
   - **Applies at once,** from the next frame's draws. New samplers and sets are made once; old sets expire
     (backend.cpp, `expire_texture_sets`).
   - **Memory:** none past a sampler a new configuration, a few dozen in all.
   - **CPU:** the main thread nothing; the FIFO worker a comparison a texture when it rebuilds a bind group, and every
     bind group once when the value changes.
   - **GPU:** no number yet. About a third of the game's textures have mips (6,396 of 19,493 on the disc,
     results/2026-10-15-milestone-14/disc_scan.txt), and Xenos takes more samples only where a texture is squeezed, as
     the village's path and roofs are at a glancing angle; trilinear filtering of the `GX_LIN_MIP_NEAR` ones adds its
     own. 2x and 4x are likely small; 8x and 16x are the values that may risk 30 fps, and step 4 measures each.

2. **Shadows: 1x, 2x, 3x.**
   - **What the multiplier does:** the shadow maps are 192 and 64 texels square times the multiplier, drawn into
     framebuffers twice that and copied down by half (dusklight-xenon:src/d/d_drawlist.cpp:1496-1526, 1587-1623): at
     1x the console resolves 384x384 and 192x192 (results/2026-10-13-milestone-12/dusklight_diag.log:810).
     `reset()` re-runs `init()` when the setting changes (:1541-1544), so it **applies at once**. Aurora makes the
     framebuffers' colour and depth at that size from an LRU of 64 MB that drops what goes 32 frames unused
     (aurora-xenon:lib/gfx/recording.cpp:260-276; lib/gfx/lru_cache.hpp:21-25), so a lowered value gives its memory back
     within about a second.
   - **The 1280x720 limit of 2026-10-12 is gone.** dusklight-xenon `43b8f65635` found the Dusk preset's 4x maps asking
     for 1536x1536 framebuffers past the layer's 1280x720. The layer has drawn framebuffers up to 4096x4096 past eDRAM
     since milestone 17 (docs/SUPPORT.md, Render passes), a 2048x2048 shadow map with depth stored a band at a time
     among its checks (xsdk:docs/HARDWARE.md, "Render passes past eDRAM in the layer, milestone 44"). The backend
     makes a target of any size (backend.cpp:268-365). On the console the preset window stays closed
     (m_Do_main.cpp, `43b8f65635`).
   - **Each multiplier,** the maps' passes at one sample, as Aurora's offscreen passes are
     (aurora-xenon:lib/gfx/pipeline_cache.cpp:1109-1112), tiled by the layer's rules (src/render_pass.c,
     `tile_targets`; xsdk:src/gpu_tiles.c, `rows_of`), memory for both maps' framebuffers, colour and depth, and their
     copies, at 4 bytes a texel:

     | Multiplier | Framebuffers | eDRAM, colour and depth | Memory | More than 1x | GPU, estimated |
     |---|---|---|---|---|---|
     | 1x | 384x384, 128x128 | 144 + 144 tiles, untiled | 1.4 MiB | | about 0.1 ms |
     | 2x | 768x768, 256x256 | 480 + 480, untiled | 5.6 MiB | 4.2 MiB | +0.3 to 0.5 ms |
     | 3x | 1152x1152, 384x384 | 1152 + 1152: 2 bands, depth whole | 12.7 MiB | 11.2 MiB | +0.9 to 1.3 ms |
     | 4x | 1536x1536, 512x512 | 1920 + 1920: 16 bands of 96 rows | 22.5 MiB | 21.1 MiB | +1.7 to 2.5 ms, and the casters' vertices 16 times |

     The GPU estimate is the clear and the store, each about a resolve, and the copy down, at a 1280x720 resolve's 270
     us and 3.4 Gpixels a second (xsdk:docs/HARDWARE.md, "Multisampled formats and targets, milestone 44 step 3"),
     with the casters' fill at most the whole map; the second map adds a ninth when more than four shadows are drawn.
     Each band runs the pass's draws again (xsdk:docs/HARDWARE.md, "Multisampling's cost, and Dusklight at 2 and 4
     samples, milestone 44 step 5"), few here.
   - **4x is left out** (decision 2): its 21 MiB is as much as the 20 MB milestone 30's run had free in the village,
     and its 16 bands come from the layer keeping a depth whole that the pass never loads.
   - **Only 4 samples keep the EFB's depth through a bigger map.** The game draws the shadow maps after the frame's
     first EFB pass (dusklight-xenon:src/m_Do/m_Do_graphic.cpp:2200-2243). At 4 samples the EFB's depth is in bands,
     stored to its texture and drawn back by the next pass, whatever the shadow pass did to eDRAM (docs/SUPPORT.md,
     Render passes). At 1 sample it sits whole at tiles 1328 to 2047: 2x's depth fits under it, while 3x's tiled pass
     keeps its own depth at eDRAM's top and the layer forgets the EFB's (docs/SUPPORT.md, Render passes, "a tiled
     pass's bands reach every tile but a whole depth's"). At 2 samples it sits at tiles 608 to 2047 and 2x's depth
     already lands on it (src/queue.c, `place_depth`). Losing it leaves the rest of the frame testing against what the
     shadow pass left, which the layer logs once (src/queue.c, `check_depth_kept`). So under `__XSDK__` the menu's top
     value follows `DUSK_XSDK_MSAA`: 3x at 4 samples, 2x at 1, 1x at 2, clamped where `init()` reads it, so a config
     file or a preset can't pass it. The 1- and 2-sample caps come from these rules; no run checks them, as Dusklight
     ships at 4.
   - **CPU:** the game draws the same; at 3x the render worker records the map's pass once into milestone 30's space and
     queues its 2 bands, tens of microseconds.
   - **Risk to 30 fps:** 2x unlikely; 3x's 1 ms or so lands on the main thread in windows where `begin` already
     waits, the village's heaviest among them (Why).

3. **Internal resolution: what it would take, not built** (decision 3).
   - **Where 720p is fixed:** Dusklight sets the console's window to 1280x720 (m_Do_main.cpp:708-711); the EFB follows
     the window's size, or with a scale of N, the GameCube's 608x448 (src/m_Do/m_Do_machine.cpp:94-98) times N at the
     window's aspect (aurora-xenon:lib/window.cpp:90-100, 493-502): 796x448 at 1, 1593x896 at 2, 2389x1344 at 3. The
     backend then clamps it to the swapchain's 1280x720 (encoding.cpp:223-235), the layer's one display mode
     (docs/SUPPORT.md, Display and swapchain), and logs "The EFB is 1280x720, not the window's". So scale 1 draws at
     448 rows today, and 2 and up at 720p.
   - **What it would need:**
     - the backend's clamp at the layer's framebuffer limit, 4096 (src/physical_device.c:103-104), not the screen;
     - an EFB that can't be made keeping the last one, with a line in the log: today `begin_frame` fails, and so the
       frame, every frame after (encoding.cpp:1227-1230);
     - the present as it is: it draws the EFB into the 1280x720 image at its aspect through a linear sampler
       (encoding.cpp:1296-1305), so every texel counts at 1.25x and 1.5x, unevenly, and at 2x it's a 2x2 average. The
       console ignores Dusklight's "Output Resampling" (backend.cpp:574), so there's no area filter;
     - a menu range under `__XSDK__`: the GameCube multiples land on 896 rows (2) and 1344 (3); 900p and 1080p, 1.25
       and 1.5 times 720p, take a fractional scale through `VISetFrameBufferScale`, which takes a float
       (dusklight-xenon:src/dusk/settings.cpp:236-238);
     - the EFB's samples as a player option, read at start: Aurora picks them once (aurora-xenon:lib/vulkan4xenon/
       device.cpp:330-356; m_Do_main.cpp:716). The resolution itself applies at once (window.cpp:624-633; the EFB
       remade at the next `begin_frame`, encoding.cpp:1227), with the old and the new EFB both held until the frames
       in flight finish.
   - **What it would cost.** EFB memory is its colour and depth at 1 sample, and at 2 and 4 the multisampled colour
     and the one-sample depth beside them (encoding.cpp:254-260), each a one-sample texture in memory (docs/SUPPORT.md,
     Images); EFB copies grow with the pixels. Hierarchical depth stays off past 1280 wide and past 1024 rows times
     samples (xsdk:src/gpu_draw.c, `HIZ_MOST_WIDTH`; xsdk:docs/HARDWARE.md, "A band's depth clear goes on a colour
     resolve, milestone 44 step 5": not run again since). The GPU estimate scales the village's measured time by the
     pixels and adds 1 to 1.5 ms a band past today's (milestone 30 step 2: 3 bands at 2 samples took 2.9 ms more than
     one untiled sample):

     | EFB | Samples | eDRAM tiles | Bands | EFB memory | Village GPU a frame |
     |---|---|---|---|---|---|
     | 1280x720 | 1 | 1440 | untiled | 7.2 MiB | 9.7 to 15.6 ms, measured |
     | 1280x720 | 2 | 2880 | 3, depth whole | 14.4 MiB | 13.8 to 20.1, measured |
     | 1280x720 | 4 | 5760 | 3, depth in bands | 14.4 MiB | 14.8 to 23.9, measured |
     | 1600x900 | 1 | 2280 | 2, depth whole | 11.3 MiB | 16 to 26 |
     | 1600x900 | 2 | 4520 | 3, depth in bands | 22.7 MiB | 22 to 32 |
     | 1600x900 | 4 | 9040 | 5, depth in bands | 22.7 MiB | 25 to 40 |
     | 1920x1080 | 1 | 3264 | 5, depth whole | 15.9 MiB | 26 to 41 |
     | 1920x1080 | 2 | 6480 | 4, depth in bands | 31.9 MiB | 32 to 47 |
     | 1920x1080 | 4 | 12960 | 7, depth in bands | 31.9 MiB | 37 to 60 |

     The CPU's part is the render worker's: an EFB pass past eDRAM records once into milestone 30's space and queues a
     resolve a band, 1.2 ms of replay a frame at 2 samples against 1.0 at 1 (milestone 30 step 2); the main thread and
     the FIFO worker do the same work at any size.
   - **Not worth offering at 4 samples.** 1.25x would take 25 to 40 ms of a 33.3 ms frame, and with `begin` already
     waiting in the heaviest windows, every millisecond of it lands on the main thread; 1.5x is past the frame, and its
     32 MiB EFB is 17.5 more than today's, with the copies 2.25 times theirs, against 42 MB free. Only 1.25x at 1
     sample is likely to fit, with 1.25x at 2 samples borderline and 1.5x at 1 sample past the frame in the busier
     windows, its depth at risk too: at 1080p the EFB's whole depth starts at tile 416, and a copy of 960x360 at
     720p, among the game's resolve sizes (results/2026-10-13-milestone-12/dusklight_diag.log:810), would take 612
     tiles at 1.5x and overwrite it. Each of those trades 4 samples' edges for a little texture and foliage
     smoothing, at more GPU than options 1 and 2 together.

4. **The measurement: one build, one run in Ordon, by the agent.** The user doesn't play: the build warps from the
   title scene to the village, as milestone 14's diagnostics did (results/2026-10-15-milestone-14-steps-5-6/
   diagnostic/step5-diag-dusklight.patch, `F_SP103` point 0 room 0), where Link stands on the path to the houses with
   his shadow at his feet (that folder's depth_of_field_console_pc.png).
   - **The build:** `local/beta/timing`'s, at 4 samples with Aurora's diagnostics, its main-thread parts, workers and
     GPU a frame logged each window, without its moves of SDL's audio thread every other window. Added to its overlay
     (`src/dusk/xenon_run.cpp`): the warp; a schedule setting the values through the settings themselves, as the menu
     will; each window's frame-slot wait (`aurora_take_frame_slot_wait_ns`, milestone 30's diagnostics), the
     backend's GPU memory peaks (device.cpp, the pools' `takenPeak`), the offscreen passes' sizes drawn, and a capture
     (`aurora_capture_frame`) at each window's end; then it ends itself.
   - **The schedule,** 20 s to settle in the village, then windows of 5 s settling and 10 s measured, each value twice,
     up then down, so the day's light and the villagers' wandering cancel:
     - anisotropy Off, 2x, 4x, 8x, 16x, 16x, 8x, 4x, 2x, Off, shadows at 1x;
     - shadows 1x, 2x, 3x, 4x, 4x, 3x, 2x, 1x, anisotropy Off; 4x for the record, last of its block;
     - anisotropy 16x with shadows at 3x, then both at their defaults.

     20 windows, 5 minutes after the title scene's minute.
   - **A break build** (below) runs the same schedule; and the plain build at the defaults runs the title scene for a
     minute, as a smoke run.

**xsdk and vulkan4xenon** need nothing new: the layer has anisotropy up to 16 (src/sampler.c:85-89,
src/physical_device.c:98) on xsdk's fetch constant (xsdk:docs/HARDWARE.md, "Anisotropic filtering, milestone 33"), and
draws framebuffers to 4096 past eDRAM. No vulkan4xenon_test, vkbench or xsdk_test runs.

**New checks.**
- **On the PC,** an Aurora host test (aurora-xenon:tests/, run as `local/m44c/aurora_host_tests.sh` runs them):
  `sampler_config` with the forced value gives it to a mipped linear texture, keeps the game's own when higher, and
  gives 1 to a texture without mips, one filtered nearest and one with arbitrary mips.
- **On the console,** a script on the PC (`compare.py`, as milestone 14's `diagnostic/sharpness.py`) reads the run's
  log and captures and prints each check:
  - **Anisotropy in effect:** in a band of the path between Link and the fence, clear of the HUD, picked from the
    first default capture and fixed in the script before the break build runs, each value's capture differs from
    Off's by more than three times Off's two windows differ from each other, and its mean step in green (sharpness.py's
    measure) is higher than Off's, 16x's the most.
  - **Shadows in effect:** each window logs the offscreen passes it drew, 384x384 times the multiplier; in a box
    around Link's shadow, each multiplier's capture differs from 1x's by more than three times 1x's two windows do.
  - **The costs:** each value's GPU time a frame, frame rate, `begin` and frame-slot wait, workers' time, frames past
    34 ms and GPU memory peak, against the default windows beside it, in one table, with the captures' crops in one
    picture for the record.

**Broken on purpose** (`results/<date>-milestone-33/breaks.py` and `builds.sh`, one build with both):
- `aniso_ignored`: `sampler_config` leaves the forced value out. The anisotropy check fails, every capture alike, and
  so does the host test; nothing else.
- `shadow_init_kept`: `reset()` no longer re-runs `init()` on a change. The offscreen passes stay 384x384 and the
  shadows check fails; nothing else.

**Verification,** on the console, by the agent: the measurement build's run, the break build's, the plain build's
smoke run; on the PC, Aurora's host tests with and without the break.

**Done when:**
- the host tests pass, and fail with `aniso_ignored`;
- in the measurement run, both checks pass; the break build fails each check it names and no other;
- each value offered (Off to 16x, 1x to 3x) changes the frame rate by no more than 0.5 fps, and `begin` and the
  frame-slot wait by no more than 0.3 ms a frame, against the default windows beside it, with no more frames past
  34 ms; and adds at most 3 ms of GPU a frame, alone and with the other's top value, so the user's heaviest village
  window, 23.9 ms, would stay under 27. A value that doesn't is left out of the menu and recorded;
- the GPU memory each shadow value adds is within 10% of the table's;
- at the defaults nothing changes: the host test gives today's sampler at Off, the default windows' shadow passes are
  384x384, and the plain build's smoke run passes the title scene's minute with no new error line;
- the record says, value by value, what each costs and which may lower the heaviest windows' frame rate.

**Left out:**
- Internal resolution as a player option (decision 3), and the EFB's samples as one.
- Shadows at 4x, and the layer choosing depth in bands for a tiled pass that neither loads nor stores its depth, which
  would take 4x's map from 16 bands to 2.
- A third frame slot: `AURORA_FRAME_SLOT_COUNT` 3, a CMake setting Aurora has, would let the GPU run a frame further
  behind before the main thread waits, for a frame more of latency and a third frame's pools. If step 4 shows the
  options' cost landing on `begin`, it's a measurement of its own.
- Alpha to coverage for the game's alpha-tested foliage, which the layer has (docs/SUPPORT.md, Pipelines) and Aurora
  doesn't ask for: the foliage smoothing a higher resolution would bring, for less. A milestone of its own if wanted.
- Re-checking hierarchical depth past 1280 wide (xsdk:docs/HARDWARE.md, "A band's depth clear goes on a colour
  resolve, milestone 44 step 5"), which only option 3 needs.
- An area filter for the present, and a 1080p front buffer.

**Decisions:**
1. **Anisotropy on every target or the console alone.** The rule is in Aurora's shared code, so the PC's WebGPU path
   would take it with no more code; under `__XSDK__` alone it's the same diff plus `#if`s. Recommended: every target.
2. **Shadows' top value.** 3x (recommended: 11 MiB more, about 1 ms of GPU); 2x (4 MiB, under half a millisecond, if
   step 4's 3x misses the bar); or 4x as well (21 MiB, as much as the village had free late in milestone 30's run).
3. **Internal resolution.** (a) Left out of this milestone, with the estimates above recorded (recommended: not worth
   it at 4 samples, and the combinations that fit trade edges for texture sharpness at more GPU than options 1 and 2
   together); (b) the backend's clamp lifted and 1.25x and 1.5x measured at 1 and 2 samples, two more builds and runs;
   (c) offered now, with the EFB's samples as a player option read at start and the resolution offered only at 1 and
   2 samples.
4. **Measure the defaults' slack first, or in the same run.** The run's default windows give `begin` and the
   frame-slot wait at the spawn, beside each value; the user's play run gives the heaviest windows. Recommended: the
   same run, as above, no user play.

**Size:** small. Aurora about 30 lines and a 60-line host test; Dusklight about 40 lines (the setting, the menu entry,
the shadows' cap); the measurement overlay about 150 lines over `local/beta/timing`'s, `compare.py` about 120; then
three runs by the agent and none by the user.

### Milestone 34: the FIFO worker

Agreed with the user on 2026-10-21, with every recommendation below.

Aurora's FIFO worker turns the game's GX stream into recorded passes, alone on hardware thread 4, and the game waits
for it every frame. It takes 19 to 22 ms a frame standing in Ordon and up to 31 in play. This milestone measures it
without the zones' cost, then takes out work it doesn't need to do on this CPU, until Ordon holds 30 fps with room to
spare. The per-frame wait stays.

**Why.**
- **The game's frame is the worker's time.** The game writes a frame's GX first, after the 30 fps limiter
  (dusklight:src/f_pc/f_pc_manager.cpp:82; libs/JSystem/src/JFramework/JFWDisplay.cpp:250-261), then runs every
  actor's update and draw (f_pc_manager.cpp:104, 108), and the draw's end waits for the worker:
  `cAPIGph_AfterOfDraw` (src/f_pc/f_pc_draw.cpp:46) reaches JFWDisplay's `endFrame` (src/m_Do/m_Do_graphic.cpp:905;
  JFWDisplay.cpp:331-345), whose `JFWDrawDoneAlarm` calls `GXDrawDone` (JFWDisplay.cpp:572-584), and Aurora's waits
  until the worker has passed the token (aurora-xenon:lib/dolphin/gx/GXManage.cpp:264-278; lib/gx/fifo.cpp:541-552).
  Standing outside Link's house (F_SP103, point 0, room 1; run 4, windows 15-23) a frame was 11.6-12.1 ms of the
  limiter's wait, 12.1-12.7 of the game's own work, and 7.8-8.6 of the draw-done wait, the worker taking 19.4-19.9.
  The wait is what's left of the worker after the game's work before it, about 11.5 ms (19.6 less 11.5 is 8.1), so
  the worker starts with the frame's first command and doesn't run dry, and the frame ends about 1 ms after it does
  (audio, `aurora_end_frame`, the next begin). Every millisecond off the worker is one off the frame, until the worker
  is under the game's ~11.5 ms. The GPU took 18.5-23.1 ms a frame there (runs 3 and 4).
- **In play it decides the frame rate.** In the user's run 5 the worker took 12.0-24.7 ms a frame in settled windows,
  and 31.1 in window 22 (the village, 26.0 fps), where the limiter's wait was down to 3.4 ms. The ranch's windows
  (23-26) took 18.9-23.6.
- **Where its time goes,** run 3 and run 4 at the village spot, own time a frame (windows 14-23 and 15-23):

  | Zone | ms own | calls | us a call |
  |---|---|---|---|
  | `process` (command_processor.cpp:447): decoding, BP/CP/XF registers | 7.53 / 6.65 | 1,091 / 966 | 6.9 |
  | `push_verts_hashed` (recording.cpp:887): a draw's vertices copied, hashed | 3.32 / 2.65 | 409 / 365 | 7.3-8.1 |
  | `handle_draw_unmerged` (command_processor.cpp:762), most of it `push_gx_draw` | 2.18 / 1.78 | 195 / 161 | 11.1 |
  | `DRAW_INDEXED` (command_processor.cpp:1056), most of it `push_gx_draw` | 2.10 / 2.05 | 213 / 204 | 10.0 |
  | `push_uniforms` (shader_info.cpp:488) | 1.49 / 1.35 | 389 / 353 | 3.8 |
  | `push_array` (recording.cpp:934): an array copied and hashed, whole | 1.31 / 1.29 | 75 / 67 | 17-19 |
  | `handle_aurora` (command_processor.cpp:858) | 0.68 / 0.66 | 3,626 / 3,391 | 0.19 |
  | `populate_pipeline_config` (gx.cpp:182) | 0.60 / 0.54 | 153 / 136 | 3.9 |
  | `draw_prim`, `push_indices`, `build_bind_groups`, the rest | 2.1 / 1.9 | | |

  The worker took 22.2 and 19.6 ms a frame on average, about 0.6 of it outside `process`. In run 5's window 22 the
  same rows were 9.64, 4.66, 2.61, 3.42, 2.00 and 2.29 ms: more of everything, not one new cost.
- **What costs more on this CPU,** reading the path end to end:
  - **Hashing.** Every unmerged and indexed draw's vertex bytes and every array a frame reads, whole, are hashed with
    XXH3 as they're copied (aurora-xenon:lib/gfx/recording.cpp:886-896, 933-956), for the console backend's vertex
    cache (lib/vulkan4xenon/backend.cpp:52-57; lib/vulkan4xenon/decode.cpp:828-928). So is the 2.7 KB pipeline config
    at every change of the state it's built from (lib/gx/command_processor.cpp:655-666), 136-207 times a frame, and
    each texture set, about 230 bytes, at each bind group built (lib/gfx/texture.cpp:240-262). XXH3 is defined
    little-endian: xxHash 0.8.3 swaps every 64-bit word it reads, the input's and its secret's, on a big-endian host
    (xxhash.h:2680-2691, 3423-3426); it takes its scalar path here, having no VMX one (xxhash.h:3972-3997); and
    without `__int128` its 64-by-64-bit multiplies, which inputs of 240 bytes or less use, are four 32-bit ones each
    (xxhash.h:4432-4520). A title runs in 32-bit mode (xsdk:docs/HARDWARE.md:4398). None of these hashes leaves the
    run.
  - **Copies of the pipeline config.** `populate_pipeline_config` resets the 2.7 KB config, fills it, then builds a
    second one around it and copies it back (aurora-xenon:lib/gx/gx.cpp:185, 290-298): about 8 KB of stores at each
    change, 3.9 us.
  - **Byte compares.** `update_pipeline_inputs` compares about 1 KB of state for every draw after a register write
    that dirties the pipeline, with 16-byte vector loads from fields at any alignment
    (lib/gx/command_processor.cpp:270-299, 323-360). How GCC loads those on this CPU, and what that costs, isn't
    known; it isn't zoned.
  - **Atomics.** After each `process` call on the stream, the worker stores its position and loads the one the game
    waits for, sequentially consistent (lib/gx/fifo.cpp:124-130, 171-186), about 125 ns each on the console
    (xsdk:docs/HARDWARE.md:4409-4413). The 966-1,526 `process` calls a frame count the called lists' too, so these are
    fewer: about 0.1-0.2 ms a frame. A called list ends a call on the stream (command_processor.cpp:530-532), and the
    game never waits there.
  - **Not costs here:** the stream's byte order, written and read big-endian, is a no-op on this host
    (aurora-xenon:lib/internal.hpp:86-94, 111-121); registers dispatch through a function-pointer table, one call per
    write that changes state (lib/gx/regs.cpp:14-19, 952-975), with no `std::function` or virtual call per command;
    no allocation per command (the pools are preallocated, the tables kept: command_processor.cpp:1097-1103); one
    mutex per bind group built (lib/vulkan4xenon/backend.cpp:516-527), 136-210 a frame; the bounds check on every
    read (lib/internal.hpp:184-186, 418-424), a compare. Uniform pieces are zeroed before they're written
    (lib/gfx/recording.cpp:200-213), but `map_uniform`, zeroing included, took 0.06-0.07 ms a frame.
- **The zones carry their own cost.** Each zone scans the thread table with an acquire load a slot until it finds
  its own, reads the time base twice, updates three 64-bit counters and its parent's, and does an atomic exchange on
  every close (xsdk:local/beta/zones/tracy/xenon_zone.hpp:44-92). The exchange alone is about 55 ns
  (xsdk:docs/HARDWARE.md:4409, a relaxed `fetch_add`), an acquire load's `isync` about 12. The worker ran 7,700 and
  8,830 zones a frame at the spot, 11,700 in run 5's window 22, so the zones likely cost it 1 to 2 ms a frame, most of
  it counted in `process`'s own time, where the children's opening and closing fall. Step 1 measures it.
- **The scene isn't the same from run to run.** Within a run the worker's windows at the spot varied by 0.15 ms
  (run 4) and 0.38 ms (run 3, standard deviations), but run 3 drew 15% more (8,832 zones a frame against 7,701; 1,091
  `process` calls against 966) and took 2.6 ms more, the worker's time per 1,000 zones within 1.3% (2.51 and 2.54
  ms). Two runs can't be compared without counting what they drew.
- **Why the per-frame wait stays.** The worker reads the game's memory by pointer, when it gets there: called display
  lists (aurora-xenon:lib/dolphin/gx/GXDispList.cpp:65-71; lib/gx/fifo.cpp:101-116), every array a draw indexes,
  copied at the draw (lib/gx/command_processor.cpp:629-643), matrices loaded by index (:485-510), textures and
  palettes. J3D keeps its display lists, skinned vertices and draw matrices in two buffers it swaps each frame
  (dusklight:libs/JSystem/include/JSystem/J3DGraphBase/J3DPacket.h:104-131, J3DVertex.h:133-150;
  libs/JSystem/include/JSystem/J3DGraphAnimator/J3DMtxBuffer.h:53), which is safe only because the GPU, and here the
  worker, is done with a frame before the game's next calc writes the buffer it read. Waiting a frame later, frame
  N+1's calc writes what frame N's commands still point at. And it would gain little: the worker is busy from the
  frame's first command to the wait already, so deferring adds the ~1 ms after the wait and lets a heavy frame borrow
  from the next, not more.

**What it takes.** Code in Aurora only (Z:\aurora-xenon, branch `xenon`, then Dusklight's extern/aurora fast-forwarded),
in Aurora's style (xsdk:CODE_GUIDELINES.md, section 14); the measurement code in `xsdk:local/beta/v34`, never
committed. Each step after the first is measured on its own and kept only for a measured gain.

1. **Measurement first,** the v34 build (`local/beta/v34/build.sh`, zones/build.sh's copy, its overlay over
   Dusklight and Aurora), built twice, `ZONES=1` and `ZONES=0`, run by the agent:
   - **Aurora without zones** in `ZONES=0`: Aurora's `ZoneScoped` empty, the worker timed only by Aurora's own busy
     time (aurora-xenon:lib/gx/fifo.cpp:191-211); Dusklight's main-thread zones kept. The target is measured in this
     build.
   - **The zones' cost,** two ways: both builds at the same spots, and at start the worker opens and closes 100,000
     nested zones and logs the cost of one. The shim's exchange becomes a load first (xenon_zone.hpp:87).
   - **Counters a frame,** plain 32-bit counts the logger reads every 10 s: stream bytes; display list calls and bytes;
     BP, CP and XF writes, and how many changed state; Aurora's commands by kind; draws merged, unmerged and indexed;
     pipeline-input changes and memo misses; bytes hashed for vertices, arrays, configs and texture sets; uniform bytes.
     They're what an A/B pair is checked against.
   - **Finer zones** in `ZONES=1`: `process` inside a display list apart from the stream's; inside `push_gx_draw`,
     `update_pipeline_inputs`, the memo's key and lookup, the array lookups and `push_draw_command`; each uniform
     piece. Tables for hardware threads 3 and 5 too, beside 0, 2 and 4, so the decode helper's share of the worker's
     core shows.
   - **A hash probe** on hardware thread 4 before the warp: XXH3 as built, XXH3 reading words in the host's order,
     XXH32 run twice, XXH64, and `memcpy`, over 64 B, 256 B, 1 KB, 4 KB and 64 KB, MB/s each; and whether GCC
     offers `__int128` here.
   - **Two spots,** 4 minutes each: F_SP103 point 0 room 1, as now, then the ranch, F_SP00 room 0 (a room the game
     names: dusklight:src/d/actor/d_a_demo_item.cpp:241), point 0, which the first run checks by its `Stage` line and
     a capture 300 frames in.
   - **Alternation:** a variant flag the logger flips at every 10 s window and the worker reads at each frame's begin.
     Each later step's code sits behind it in this build, so its A and B windows alternate in one run. A verify mode
     runs the old computation beside the new and counts disagreements.

   If the worker without zones is already at 15 ms a frame or less at the village spot, the milestone goes straight
   to the user's session. If hashing is under a quarter of the worker's time without zones, steps 3 and 4's estimates
   are redone with step 1's numbers and come back to the user first.
2. **The worker on its own core.** Today the decode helper, on hardware thread 5, shares core 2 with the worker
   (aurora-xenon:lib/thread.cpp:297-299), and the render worker hands it decodes and hashes
   (lib/vulkan4xenon/decode.cpp:696-786). Milestone 26's probe found two busy threads on one core's two hardware
   threads at 51% each, one alone on hardware thread 4 at 87% of one on 0, and one on 2 at 93%
   (xsdk:docs/HARDWARE.md:2051-2053). The list becomes `Aurora render worker=4,Aurora FIFO processor=2`, the decode
   helper staying on 5, now beside its render worker. One line.
   - **SDL's audio thread moves too,** off the main thread's hardware thread 0, where it starts as its creator's
     (xsdk:include/xsdk/thread.h:19) and preempts the main thread for the audio DSP, 0.8 ms a frame standing still and
     4.8 in run 5's worst village window. It goes to hardware thread 5, beside the render worker, not beside the FIFO
     worker: on hardware thread 5 beside the worker on 4 it gained nothing (local/beta/timing/run1, 28.7 against 29.3
     fps, the worker 11-32 ms a frame). Dusklight places it (aurora::thread's list, or SDL-xenon's driver taking a
     hardware thread from a hint), judged in the same A/B with the main thread's own time and the DSP's.
   - **Saving:** up to 1.3 ms (7% of 19.6) from the core, plus whatever step 1 finds the helper costs the worker.
   - **Risk:** no change to what's drawn. The render worker runs on core 2 at 87% (it took 6.9-13.4 ms a frame at the
     spot, 5-21.5 in play), and shader compiles and texture preconversion, on hardware thread 3, run in bursts beside
     the worker.
   - **Check:** each thread's placement line; the render worker's time and `begin` not rising in the A/B. In the
     A/B build each worker moves itself at a window's first frame, as `place` does (lib/thread.cpp:320-330).
3. **The pipeline memo's key.** At each pipeline-input change the worker hashes the whole config
   (aurora-xenon:lib/gx/command_processor.cpp:657), about 2.7 KB counted from its members (lib/gx/pipeline.hpp:27-41;
   lib/gx/gx.hpp:143-169, 554-585), 2.1 KB of it the 16 TEV stages, 132 bytes each with GX's enums at 4 bytes.
   `populate_pipeline_config` fills only the stages in use and leaves the rest as its reset made them
   (lib/gx/gx.cpp:185, 240-247). Changes:
   - the key hashes the config up to its last stage in use and from `tevStageCount` on: 0.9 to 1.3 KB for 1 to 4
     stages;
   - the memo is keyed by 64 bits on every host. Today it's `HashType`, 32-bit on a 32-bit host, and so is the layout
     key given as XXH3's seed (lib/gfx/hash.hpp:11-25; command_processor.cpp:226-230, 657), so two configs could
     already share a memo entry here;
   - `populate_pipeline_config` assigns the config's fields in place, without the second 2.7 KB config
     (gx.cpp:290-298).
   - **Saving:** 0.7-1.1 ms, a guess until step 1 gives XXH3's rate here. The hash is part of `DRAW_INDEXED` and
     `handle_draw_unmerged`'s 3.8-4.3 ms own time. `push_array` took 17-19 us an array, its copy about 3 of them, the
     arrays averaging 6 KB at most (the storage pool's peak, 415-448 KB a frame, over 67-75 arrays): XXH3 at about
     400 MB/s or less, so 6.5 us or more for 2.7 KB, 0.9-1.3 ms a frame at 136-207 changes. Hashing a third to a half
     of the bytes saves 0.5-0.9 of it, and populating in place about 0.2 of `populate_pipeline_config`'s 0.54-0.60.
   - **Risk:** a key shared by two different configs draws one with the other's pipeline. Every byte that can differ
     must be in the key: the stages past `tevStageCount` are always the reset's, as are the indirect stages past
     `numIndStages`, and both counts are in it.
   - **Check:** verify mode keeps each key's first config and compares every later hit with it, counting mismatches;
     a new test in gx_fifo_tests gives configs differing in one byte of a used stage, in a count, or in any field after
     the stages, and expects different keys, and equal keys for equal configs.
4. **A hash for what stays in the run.** One function for the vertex bytes and arrays
   (aurora-xenon:lib/gfx/recording.cpp:886-896, 933-956), their counterpart on the render worker for draws the worker
   didn't hash (lib/vulkan4xenon/decode.cpp:672-683, 779-786), the texture set's key (lib/gfx/texture.cpp:240-262)
   and step 3's memo key. Keys that outlive a run, the shader and pipeline caches' names on disk
   (lib/gfx/pipeline_cache.cpp:445-552; lib/vulkan4xenon/glsl.cpp:1649), stay XXH3. Step 1's probe picks the fastest
   that gives 64 bits at Dusklight's sizes. The likely ones: XXH3 compiled a second time, in one file under its own
   `XXH_NAMESPACE`, with `XXH_CPU_LITTLE_ENDIAN` set to 1, which xxhash.h lets a build choose (xxhash.h:2680), so it
   reads words as the host holds them: the same mixing without the swaps; or XXH32 run twice with two seeds, 32-bit
   multiplies only.
   - **Saving:** the copies run through xsdk's `memcpy`, 1.8 GB/s from memory into cached memory
     (xsdk:docs/HARDWARE.md:1527), so hashing is most of `push_verts_hashed` and `push_array`'s 4.0-4.6 ms, and part
     of `build_bind_groups`' 0.43-0.48. A function twice as fast saves about 1.8-2.2 ms, three times 2.4-2.9.
   - **Risk:** a weaker function lets two different vertex blocks share a key, and the vertex cache draws one's
     decoded vertices for the other.
   - **Check:** verify mode hashes the same bytes with XXH3 too and counts, each frame, pairs the new function calls
     equal and XXH3 doesn't; a new test in gfx_recording_tests: equal bytes at any alignment hash equal, and every
     single-bit change in 1 to 4,096 bytes changes the hash; the test `TextureSetHash.AsTheStreamedFieldsGave`
     (tests/gfx_recording_test.cpp:359-388) streams through the new function; the vertex cache's line keeps its 96-98%
     of draws found at the spot.
5. **The display lists' reads,** if step 1 finds hundreds of KB a frame. The worker reads a called list from the
   game's memory (aurora-xenon:lib/gx/fifo.cpp:101-116), perhaps from memory rather than the cache. `process` touches
   the list ahead of where it reads (`dcbt`, within the list's bytes): newlib's `memcpy` read 1 MB from memory at 410
   MB/s, and the SDK's, with `dcbt` 8 lines ahead, at 1,814 (xsdk:docs/HARDWARE.md:1511, 1527). About 10 lines.
   - **Saving:** unknown until step 1: none to about 1.5 ms.
   - **Risk:** none to what's drawn; a touch is a hint, kept inside the list.
   - **Check:** the A/B; gx_dl_tests and the stream's byte goldens unchanged.
6. **The rest, after step 1's split,** measured together and kept only together:
   - the worker stores its position and wakes the game only at a draw-done, a frame's end and the end of what was
     published, not after each display list's return (lib/gx/fifo.cpp:171-186): about 0.1-0.2 ms. A wake missed
     hangs the game at its draw-done, where JFWDisplay's alarm aborts the frame after 0.5 s
     (dusklight:libs/JSystem/src/JFramework/JFWDisplay.cpp:572-584), so gx_fifo_tests' waits check it, and its break
     runs last;
   - word compares in `update_pipeline_inputs` (lib/gx/command_processor.cpp:270-299), if step 1 times it above 1 us
     a draw;
   - a uniform piece that step 1 times at 0.5 ms a frame or more, if one does.

**Ranked by saving for the effort** (the zone build at the village spot, before the zones' cost comes out):

| Step | Change | Lines | Saving, ms a frame | Risk |
|---|---|---|---|---|
| 2 | The worker and render worker trade hardware threads | 1 | 0.5-1.3 | none to output |
| 3 | The memo's key: used stages, 64 bits, no second config | ~30 | 0.7-1.1 | wrong pipeline |
| 4 | A hash for what stays in the run | ~50, test ~40 | 1.8-2.9 | false vertex cache hit |
| 5 | Display lists touched ahead | ~10 | 0-1.5 | none to output |
| 6 | Position stores, input compares, a uniform piece | ~30 | 0.1-1.0 | a missed wake |

Together about 3 to 8 ms, against a worker estimated at 18 to 21 ms at the spot without zones.

**How a step is judged,** with no one at the console:
- A run is the title scene's minute, the warp, the first window there dropped, then 10 A/B pairs (200 s) at each
  spot: about 9 minutes, launched and fetched by the agent.
- A step is kept if, at the village spot, the mean of its 10 paired differences is 0.3 ms or more and three standard
  errors or more (runs 3 and 4's spreads give 0.07-0.17 ms for 10 pairs), and the ranch shows no loss of 0.3 ms or
  more. A pair whose counters differ by more than 2% is left out, and said. Otherwise the step is reverted to HEAD
  (xsdk:CODE_GUIDELINES.md, 1.4).
- Verify mode once a step, untimed, in its own run: no disagreement.
- No Xenia run: it schedules the worker on a host thread and can't time any of this (xsdk's CLAUDE.md).

**New checks** (Aurora's host tests: gx_fifo_tests and gfx_recording_tests on Dawn through `~/build/host_dawn_tests.sh`,
the rest on the null backend through `~/build/aurora_host_tests.sh`; the source a fresh copy, cleaned first):
- gx_fifo_tests: the memo's key (step 3), as above.
- gfx_recording_tests: the run's hash (step 4), as above, and the texture set test through it.
- Every existing test passes at each step, the stream's byte goldens (tests/gx_fifo_bytes_test.cpp) among them,
  unchanged: no step touches what the game thread writes.

**Broken on purpose** (`results/<date>-milestone-34/breaks.py`, the A/B build with one change each):
- `memo_prefix`: the memo's key over the config's first 256 bytes: verify mode counts mismatches, the key's test fails,
  and the capture draws materials with other materials' pipelines.
- `hash_prefix`: the run's hash over each range's first 16 bytes: verify mode counts disagreements, the hash's test
  fails, and the capture shows geometry from other draws' vertices.
- `wake_missed` (if step 6 keeps its position change): no store at a draw-done return. gx_fifo_tests' waits fail; on
  the console the game hangs at its first draw-done, so it runs last, from a fresh boot.

**Verification,** on the console:
- By the agent: step 1's two builds at both spots; each step's A/B run and verify run; the breaks; then the `ZONES=0`
  build with every kept step and no alternation, twice, for the target.
- Plain Dusklight builds, without diagnostics, before and after, built as `xsdk:local/dusk_current/build.sh` builds
  but into the milestone's own directory: `dusklight_title.ppm`, the title scene's 600th frame, counted, from each,
  and their difference, equal but for the logo's moving shine, which differs between two runs of one build too
  (vulkan4xenon:docs/ROADMAP.md:1184-1186). The steps change no GPU input, so anything else is a bug.
- By the user, once at the end: the `ZONES=0` build with every kept step, played by hand through the village, the
  spring and the ranch, as in run 5, about 10 minutes, with `zones_play.txt` beside it.

**Done when:**
- At the village spot, in the `ZONES=0` build, the worker takes 15 ms a frame or less: the mean of the settled windows
  in each of two runs (19.6 and 22.2 with zones in runs 4 and 3; step 1 gives the start without them). In play the
  worker took up to 1.26 times the spot's time in a settled window (24.7 ms in run 5's window 16, against run 4's
  19.6) and 1.59 times in the heaviest (31.1, window 22). At 15 ms here that's 19 to 24 ms there, and the frame, the
  worker plus about 1 ms after the wait, 25 ms or less: 8 ms or more under 33.3.
- Verify mode found no disagreement for any kept step; the captures match but for the shine; Aurora's host tests pass,
  each break failing what it names.
- In the user's session every 10 s window without a stage change logs 29.5 fps or more, milestone 30's bar (a window
  at the limiter's 30 logs 29.7 to 30.0), and the worker 24 ms or less.

A window that misses 29.5 with the worker under 24 ms is the main thread's: in run 5's window 22 the game's own work
was 29.9 ms a frame, and at the ranch 18.5-24.7. The audio DSP's 4.8 ms there (`DspRender`) isn't the main thread's
work: it runs in SDL's audio callback (dusklight:src/dusk/audio/DuskAudioSystem.cpp:155-260), on SDL's device thread,
which runs on hardware thread 0 with the main thread and above it, so it preempts it (the zones count per hardware
thread). Step 2 moves it off. Such windows go back to the
user as their own scope, and this milestone closes on the worker's two targets.

**Left out:**
- Waiting for the draw-done a frame later (Decisions, 1), and a third frame slot (Decisions, 2).
- Splitting the worker into a decoding thread and a recording thread on two cores: the most it could give, and a
  rewrite of the command processor's state handling, not for the beta.
- The main thread's own work (above); the audio DSP isn't its own and moves in step 2.
- Shrinking the config's TEV stages to bytes: the pipeline cache remembers configs on disk by their bytes
  (aurora-xenon:lib/gfx/pipeline_cache.cpp:443-445), so every cached one would be lost once, and step 3 takes most of
  the gain without it.
- Uniform pieces written without zeroing first: 0.06-0.07 ms a frame.
- The game thread's publish after every draw (aurora-xenon:lib/gx/fifo.cpp:40, 567-574) and every display list call
  (lib/dolphin/gx/GXDispList.cpp:71): the main thread's cost, not the worker's.

**Decisions,** each with a recommendation:
1. **The per-frame wait.** Keep it (recommended: J3D's two buffers and every pointer the worker reads assume it, and
   waiting a frame later would gain about 1 ms and smoothing); or wait a frame later, which needs a third buffer in
   each of J3D's double buffers, every other pointer the worker reads checked, and the memory for both.
2. **A third frame slot.** Not in this milestone (recommended: it shortens `begin`'s wait for the GPU, 0.2 ms a frame
   at the spot and up to 5.1 in one window, not the worker, and costs about 15 MB, Dusklight's pools being 2, 2, 1, 2
   and 8 MB a slot, dusklight:CMakeLists.txt:112-116, with 37 to 98 MB free in Ordon in runs 4 and 5); or add it.
3. **Where the hashing goes.** A faster hash on the worker, where the bytes are warm (recommended); or
   `contentHashes` off (aurora-xenon:lib/vulkan4xenon/backend.cpp:56), one line, the hashing moving to the render
   worker and its helper, both on core 2 after step 2, reading the pools cold, as the fallback if step 1's probe finds
   no function twice as fast; or keep XXH3 everywhere.
4. **How steps are compared.** Alternating windows in one run (recommended: runs 3 and 4 drew 15% apart at the same
   spot); or separate runs set side by side by their counters; or a tool that records the worker's input and replays
   it, the fairest and the most work.
5. **The target.** 15 ms at the village spot without zones (recommended: 8 ms or more of margin in play); or 12 ms,
   the game's own work before the wait, below which the still scene gains nothing; or the user's session alone.
6. **The memo's 64-bit key.** In step 3 (recommended: the 32-bit key can already pick another config's pipeline,
   and it costs nothing); or left as it is.
7. **If the target is missed after step 6.** Back to the user with the remaining buckets (recommended), the decoding
   and recording split among the options; or a hash helper on another core straight away.

**Size:** medium. Step 1 about 200 lines of measurement code in `local/beta/v34`, two builds and two runs by the
agent; steps 2-6 about 120 lines in Aurora and 80 of host tests; one A/B run and one verify run a step, the breaks, two
final runs and the captures, about 15 runs of 9 minutes by the agent; one session of about 10 minutes by the user.

**Closed on 2026-10-22 short of its target, by the user's choice** (the runs' logs are in `xsdk:local/beta/v34`,
outside the repository):
- Steps 2 to 4 are kept, each measured against the step before at the village spot:
  - the FIFO processor on hardware thread 2 and the render worker on 4, with SDL's audio thread on 5
    (`SDL_HINT_AUDIO_XSDK_HARDWARE_THREAD`, SDL-xenon `adb87a744`): 1.44 ms a frame;
  - the pipeline memo keyed in 64 bits by the stages in use: 1.05 ms;
  - XXH64 for the run's vertices and arrays: 0.45 ms.
- Step 5, the decoder's early-outs, saved nothing, and was taken out.
- The worker went from 18.90 to 15.89 ms a frame at the village spot, against the 15 ms target, and from 22.42 to
  19.23 at the ranch.
- Verify mode found no disagreement for a kept step, and it caught the decoder's break. Aurora's host tests passed on
  the tree with milestone 16: 108 of 108 on the null backend, and 413 of 413 on Dawn, which doesn't build the UI's test
  target.
- The user's session was the beta, milestones 16 and 34 together, in Ordon. It was fine with Unlock Framerate off.
  With it on, the game draws up to 60 frames a second, about twice the worker's time at 30, and the frame rate fell
  into the teens on Epona. The beta's release notes tell testers to leave it off.
- Pushed: aurora-xenon `fbaf27f`, SDL-xenon `adb87a744`, dusklight-xenon `c366f1156c`.

## 5. The Dusklight track

An outline, made precise one milestone at a time. Each starts only when agreed.

- **What encounter's Aurora needs from Vulkan,** through a backend on the fork's seam:
  - six dynamic uniform buffers, eight combined image samplers and 16 bytes of push constants;
  - dynamic viewport and scissor only;
  - triangle lists with 16-bit indices;
  - RGBA8 colour, D24S8 depth and BC1 textures;
  - render passes with load, clear and don't-care, and framebuffer copies drawn into sampled images.

  The layer's milestones 1 to 4 cover that.
- **What Twilight Princess adds,** as read on 2026-10-11:
  - **Depth copied into a texture every frame:** a half-size `GX_TF_Z16` copy for depth of field
    (dusklight:src/m_Do/m_Do_graphic.cpp:1130-1144), made before the PC build's "depth of field off" return (:1171),
    so it runs either way. The backend logs depth copies and skips them
    (aurora-xenon:lib/vulkan4xenon/encoding.cpp:599).
  - **Shadows in an offscreen target with its own depth** (dusklight:src/d/d_drawlist.cpp:1593-1626), after which
    the frame goes on with the EFB's depth (aurora:lib/gfx/recording.cpp). The layer keeps depth only in
    eDRAM, and loses it when another depth attachment comes between (SUPPORT.md, "Render passes").
  - **15 to 20 framebuffer copies a frame:** bloom's sub-rectangles, depth of field, the half-size frame and shadows
    (dusklight:src/m_Do/m_Do_graphic.cpp:943-1015, 1523-1678). Each breaks the render pass.
  - **`GXPeekZ`,** the game's only CPU read of the framebuffer (dusklight:src/d/d_drawlist.cpp:1848): up to 64 points a
    frame for occlusion tests, the sun's lens flare, the lantern's glow and golden bugs' sparkles
    (dusklight:src/d/d_kankyo_rain.cpp:337-362). The backend returns 0
    (aurora-xenon:lib/vulkan4xenon/backend.cpp:489-497), which only hides them.
  - **Fog range adjustment on every 3D draw** (dusklight:src/d/d_kankyo.cpp:1257, 9457-9460), a factor for each pixel
    column.

  Not needed after all: the Z texture with destination alpha (dusklight:libs/JSystem/src/JFramework/JFWDisplay.cpp:522-550)
  is in `clearEfb`, which returns at once on PC (:486); with it go the game's only destination alpha and any need for
  a constant blend colour, as the backend clears by drawing with write masks. Every GX texture format, palettes too,
  is decoded to RGBA8 on the CPU (aurora:lib/gfx/texture_convert.cpp), so more formats would save memory
  but block nothing.
- **The backend.** The fork adds a backend interface to encounter's Aurora, `lib/gfx/backend.hpp`, and the
  vulkan4xenon backend goes behind it.
  - **Why a fork and not upstream:** upstream draws only through Dawn, and its shaders read the game's vertex
    arrays from storage buffers and unpack big-endian bytes with integer shifts and masks
    (aurora:lib/gx/shader.cpp:1717-1764). Xenos has no storage buffers and only float ALU instructions
    (xenia:src/xenia/gpu/ucode.h:1006, 1359), and WebGPU requires compute shaders. So the backend decodes vertices
    on the CPU into one stream and keeps uniforms in uniform buffers (aurora-xenon:lib/vulkan4xenon/decode.hpp).
  - **Shaders:** the 360 has no shader compiler on the console, so:
    - a generator modelled on the WGSL's (aurora:lib/gx/shader.cpp) writes Vulkan GLSL, without its integer
      operations;
    - the 360's null backend runs it on every configuration the game asks for and writes the GLSL out, in Xenia or
      on the console (decided 2026-10-12, milestone 10). Aurora's pipeline cache holds configurations as raw structs
      (aurora:lib/gfx/pipeline_cache.cpp), bitfields included, so one recorded on a PC doesn't carry
      over;
    - glslang turns each into SPIR-V and xost into a container, and the pack ships both;
    - on the console the backend looks up the SPIR-V by configuration and hands it to `vkCreateShaderModule`, which
      finds the container by that SPIR-V. A configuration missing from the pack isn't drawn.
  - **Vertices:** encounter's Aurora fetches the game's raw big-endian vertex arrays in its shaders; the backend
    decodes them on the CPU, as the layer has no storage buffers. Xenos fetches big-endian data natively under
    an 8-in-16 swap. Fetching the raw arrays directly may avoid the decode; it needs xsdk to set a stream's endian
    swap, which is 8-in-32 for every stream today. Unverified until tried.
- **The rest of the port:**
  - **The game:** C11 and C++20 (dusklight:CMakeLists.txt:31-33), which xsdk's GCC 15 builds.
  - **The disc:** encounter's Aurora reads disc images through nod (aurora:CMakeLists.txt:11, 46), a Rust library;
    the port needs a reader in C.
  - **The platform:** encounter's Aurora takes threads, input, audio and windows from SDL3
    (aurora:CMakeLists.txt:39-44). Here they come from xsdk.
  - **Memory:** a budget within the console's 512 MB, which the CPU and GPU share.
  - **Byte order:** the console is big-endian like the GameCube, so the PC build's swaps at load must turn off
    (dusklight:include/helpers/endian.h).
- **What the shaders need from xost,** beyond the GLSL generator:
  - uniform arrays indexed at run time: the position, normal and texture matrices are picked by an index per vertex
    (aurora:lib/gx/shader.cpp). Xenos reads constants at `c[a0 + n]`, `a0` set by `MOVA`
    (xenia:src/xenia/gpu/ucode.h:196-202, 1140-1148); xost reads arrays only by a loop counter (xost:docs/xsc.md);
  - the fragment's position, for range fog: the pixel shader's generated parameter
    (xenia:src/xenia/gpu/registers.h:140, 234);
  - depth written by a pixel shader, to draw depth back into eDRAM (xenia:src/xenia/gpu/shader.h:972);
  - vertex inputs past position, colour and texture coordinates, for the matrix indices;
  - Twilight Princess's longest TEV chains within xost's 16 registers a pixel shader, unchecked.
- **What the layer needs at start-up:** 8-bit normalized vertex attributes, which xsdk has (`XSDK_VERTEX_UBYTE4N`,
  xsdk:include/xsdk/shader.h). The rest is the backend's to fit: one memory type, a `B8G8R8A8` swapchain,
  256-byte uniform offsets, and textures of any size tiled rather than linear.
- **The order, agreed on 2026-10-11:**
  1. **Milestone 9: Dusklight on Aurora's null backend,** running on the console without drawing up to the title
     scene's game loop: the build, the platform from xsdk, the disc, memory and byte order.
  2. **Milestone 10: the boot screens drawn** (section 4), changed on 2026-10-12 from "the shaders, from a pipeline
     cache recorded on a PC": the GLSL generator for the boot screens' configurations, recorded on the 360, and the
     backend that draws them.
  3. **Milestone 11: the title screen drawn** (stage F_SP102, dusklight:src/d/d_com_inf_game.cpp:1221), with the rest
     of the generator and what xost needs above, and its
     colour effects, through render to texture as it stands; depth copies skipped. It takes the
     byte order milestone 9 leaves: Aurora's texture conversion and the arrays flagged little-endian.
  4. **Depth out of eDRAM:** xsdk resolves depth to memory and samples it, after console probes of what a depth
     resolve writes (xenia:src/xenia/gpu/xenos.h:917-924); then the layer stores, samples and reloads depth
     attachments. That brings the Z16 copy, shadows over the EFB's depth, and `GXPeekZ` read from resolved depth.
  5. **Copies at the frame's pace,** if the frame needs it: sub-rectangle resolves, which libxenon says can't be done
     (libxenon:drivers/xenos/xe.c:1076) and Xenia finds games doing (xenia:src/xenia/gpu/xenos.h:944-976), and small
     passes kept out of the EFB's eDRAM.

  Later: logic ops, lines and points (the minimap), native texture formats, uploads that don't stall, the sampler's
  maximum LOD.
- **Where the code lives (decided 2026-10-11):** Dusklight's changes in a fork of its own, Z:\dusklight-xenon, from
  TwilitRealm/dusklight at `2c4960d` (remote `upstream`). Aurora's are a fork of encounter's Aurora,
  aurora-xenon, on `3227d76`, the commit Dusklight pins.

## 6. Left out

- Compute, queries, secondary command buffers, more than one queue, multisampling, multiple subpasses and dynamic
  rendering.
- Vulkan 1.1 and up (single pieces of 1.1 as extensions, section 2).
- Geometry and tessellation shaders.
- xsdk's TODO.md items, each until a milestone or Dusklight needs it.

## 7. Open

- **What milestone 5 found slow,** for when it's taken on:
  - reading uniform buffers at submit, 4.0 us a draw for two blocks, as mapped memory is write-combined. Milestone 6
    brought it to 0.87 us with 16-byte reads, and milestone 7 to none: the GPU loads each block. Encounter's Aurora
    passes its uniforms this way;
  - push constants are now the dearer path, 0.97 us a draw at submit against a uniform draw's 0.50. Milestone 8 had
    the GPU load them, 17 dwords a cube from 65, and the submit didn't move, so the cost lies elsewhere in the push
    path;
  - the replay pointing streams and textures again every draw though they didn't change. Milestone 6 points only what
    changed, and a cube costs 1.22 us through the layer against xsdk's 0.62, 1.97 times;
  - apps writing mapped memory with `memcpy` ran at 185 MB/s there. Since xsdk milestone 20, `memcpy` is xsdk's own
    and copies 5.6 GB/s from the cache and 1.5 GB/s from memory into write-combined memory (xsdk:docs/HARDWARE.md),
    so this one is done.
