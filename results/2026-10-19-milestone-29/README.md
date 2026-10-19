# Milestone 29: the submit's cost of a draw

docs/ROADMAP.md, "Milestone 29". Built in the V29 worktree pair, against xsdk `eda49f9`, and run on the
console through the console agent on 2026-10-19.

## Step 1: what the submit spends

`make V4X_PROFILE=1` times the submit's stages with the time base (src/profile.c). A lap costs 21.3 ns, taken off each
stage below. vkbench's `draws_4000`, a cube a draw, two pushes and an indexed draw each (`step1_vkbench_profile.log`):

| Stage | ns a draw |
|---|---|
| Constants, xsdk's setters | 226 |
| Pushes copied into the replay | 154 |
| Dispatch | 114 |
| The draw's checks | 86 |
| Resources | 45 |
| xsdk's draw | 31 |
| Other commands | 20 |
| Pipeline | 5 |
| All | 682 |

The plain build's submit took 2,684 us for 4,000 draws, 0.671 us a draw (`step1_vkbench.log`). In `states_1000`, setting
a pipeline's state again took 481 ns a draw.

**Dusklight's title scene** (`step1_dusklight_profile.log`): 240 to 250 draws a frame took 10.4 to 11.9 ms of submit a
frame, 40.6 us a draw of it in commands other than draws. Split by type, `END_RENDER_PASS` took 9.4 to 10.9 ms a
frame over its 27 passes. In the attract movie, `COPY_BUFFER_TO_IMAGE` took about 2.4 ms a frame over its 2 copies.
- **The passes' cost is xsdk's resolve rectangles.** xsdk keeps 8 rectangles a resolve draws, by place and size
  (xsdk:src/gpu.c, `rectangle_slot`), and one at a ninth size waits for the GPU to finish with the oldest, a 1 ms sleep
  at a time. With 64 slots in a build for this question alone, the passes took 130 us a frame and the whole submit 1.05
  to 1.09 ms (`step1_dusklight_profile_64_slots.log`). That build was reset after.
- The draws themselves cost Dusklight about 2.9 us each, 0.7 ms a frame: pipeline 1.0 us, resources 0.6, constants
  0.4, checks 0.25.

## Steps 2 and 3: a stage's push range, read where recorded

A stage's push block writes only the registers its ranges cover, and pushes of whole 16-byte quads are read where the
command buffer recorded them.
- **vkbench** (`step23_vkbench.log`): `draws_4000`'s submit took 2,340 us, 0.585 us a draw, from 0.671. The pushes
  took 44 ns a draw, from 154 (`step23_vkbench_profile.log`). Constants took 237 ns, about what xsdk's own two setters
  cost. 17 of 17.
- **vulkan4xenon_test** (`step23_vulkan4xenon_test_1.log`, `_2.log`): 492 of 492 each, `test_push_constants` with a
  quad pushed in parts.
- **Broken on purpose,** the fragment window starting a register late (`step23_vulkan4xenon_test_broken.log`): 125
  checks failed, the push constants' and every other check whose colour comes from a push.
- **Dusklight** (`step23_dusklight_profile.log`): its pushes took 47 ns a draw, from 153. Its title scene ran as
  before.
