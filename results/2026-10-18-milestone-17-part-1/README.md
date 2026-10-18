# Milestone 17's first part: render passes past eDRAM at one sample

xsdk:docs/HARDWARE.md, "Render passes past eDRAM in the layer, milestone 44". Built clean from the M44 branches of both
repositories on 2026-10-18 and run on the console through the console agent.

| Log | What ran | Result |
|---|---|---|
| `vulkan4xenon_test_1.log`, `_2.log` | the layer's tests | 439 of 439, twice |
| `vulkan4xenon_test_broken_a.log` to `_e.log` | the sets `breaks.py` names, built by `builds.sh` | each failed the checks it names and no others: 5, 10, 4, 5 and 2 |
| `vkbench.log` | vkbench | 14 of 14 |
| `xsdk_test.log` | xsdk's tests | 409 passed, 2 failed: the controller checks, no pad connected |
| `probe_tiles.log` | xsdk's tiling probe, cases 1 to 4 and 11 | 11 PASS |
| `probe_shader_cost.log` | Dusklight's 150 programs, constants now below c252 | all 103 pictures as before |
| `probe_a0_predicated.log` | the a0 probe, its mode row moved from c255 to c240 | case for case as its last run, but for one quad of a case that already failed now and then |
| `dusklight.log` | Dusklight against both branches | its title scene at the frame rates before, its capture written |

Two runs on the way found what the branches then fixed:
- `vulkan4xenon_test_hiz_on.log`: with the hierarchical buffer on at a 2048 pitch, the 2048x2048 shadow map in 4 bands of
  512 rows had 1,652,496 of 4,194,304 texels' depth left at the clear, from row 812 on.
- `vulkan4xenon_test_depth_reuse.log`, a build logging the placement: once the shadow map drew right, render to texture
  asserted in xsdk's `put_surface`, a 200x120 framebuffer's depth placed at tile 2036, where eDRAM held its depth
  image's depth for a smaller framebuffer.
