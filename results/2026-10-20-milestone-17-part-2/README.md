# Milestone 17, second part: multisampling in the layer

xsdk:docs/scope/44-tiling-and-msaa.md, step 5's layer part; docs/ROADMAP.md, milestone 17. Built on the M44 pair
(xsdk M44, this repository M44b) and run on the console through the console agent on 2026-10-20 (`console/`).

## The first run

At `7e7509a`, xsdk at `6e461d6` (`console/first_*.log`): xsdk_test 519 of 521, the controller's two failing with the
pad idle; vulkan4xenon_test 513 of 515 twice. Every check passed but the two of 720p tiled at 4 samples with depth in
bands, "tiles: 3 frames at 4 samples, depth in bands ..." and "tiles: 4 samples, depth in bands past eDRAM at
1280x720 ...": band 0 right, from band 1 on the first pass's draws missing, 81,200 pixels off the edges wrong.

## The cause and the fix

That pass is the layer's only one with the hierarchical buffer on in its bands (1280 wide, 256 rows times 4 samples,
xsdk's `xsdk_gpu_hiz_fits`), and the layer listed the stored depth's resolve last, so each band's depth clear and the
buffer's with it went on a resolve of depth. xsdk `e0e0921` resolves a band's depth first, so the last colour resolve
clears it, as xsdk_test's bands always had. The run at `38c0278`, xsdk `e0e0921` (`console/*.log`):
- xsdk_test 520 of 522, the controller's two; vulkan4xenon_test 515 of 515 twice; vkbench 17 of 17.
- `depth_last`, the depth resolved last again: the two 4-sample checks alone fail.
- `hiz_off`, the buffer off in every tiled pass (a control): nothing fails.
- `resolve0`, `offset0`, `atoc_off`, `drawback1`: each fails what breaks.py names.

So a band's clear on a depth resolve leaves the hierarchical buffer wrong for the next band's draws
(xsdk:docs/HARDWARE.md, "A band's depth clear goes on a colour resolve, milestone 44 step 5").

## Added after the run

The layer's resolve of a multisampled depth image, `vkCmdResolveImage` taking sample 0 for Aurora's depth copies
(xsdk:docs/scope/44-tiling-and-msaa.md, step 5), with two vulkan4xenon_test checks, "tiles: ... vkCmdResolveImage of
its depth after both passes, every texel sample 0's" at 2 and 4 samples, and the break set `depth_sample1`, which
should fail those two alone. Neither has run on the console yet.
