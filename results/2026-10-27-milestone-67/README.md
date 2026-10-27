# Milestone 67: copies from the GPU's memory drawn on the GPU

docs/SUPPORT.md, "Commands" (`vkCmdCopyBufferToImage` from the GPU's memory drawn on the GPU), and docs/ROADMAP.md,
"Open" (where code lands). Built at `f780ff4` against xsdk `4c0f8da` (the M65 worktree), and run on the console on
2026-10-27.

## vulkan4xenon_test and its breaks

`vulkan4xenon_test.log`: 544 passed, 0 failed. The 512x512 copy from write-combined memory was done 1,116 us after its
submit, and every texel of each drawn copy matched, bit for bit.

Each break fails its own check alone (`break_*.log`):

| Break | What it undoes | The check that fails |
|---|---|---|
| `upcpu` | every copy on the CPU again: done after 25,939 us | the copy done within 5 ms of its submit |
| `upuv` | the rows sampled across their whole length | the rectangle from rows longer than it |
| `upwait` | a copy into the buffer after the drawn copy doesn't wait | the fill after the copy doesn't reach the image |
| `upalign` | buffers only copied to and from 64-byte aligned | the objects check of their 4 KB alignment |
| `upflags` | the cached memory type not device-local | the objects check of the memory types |

## Flycast's movie

Sonic Adventure's opening movie, unattended, through RetroArch with the layer's profile build, round 8's core either
side, so only the layer differs (`flycast_movie_before.log` at `4920c06`, `flycast_movie_after.log` at `f780ff4`). A
present, in the movie's blocks:

| | Before | After |
|---|---|---|
| Submitting | 21.4 to 25.8 ms | 128.6 to 134.6 us |
| `COPY_BUFFER_TO_IMAGE` (type 7) | 21.3 to 25.7 ms | none |
| `DRAW_UPLOAD` (type 35) | none | 30 to 36 us |
| Bytes the CPU copied into images, from write-combined memory | 858 to 1036 KB, all | 0 |
| Bytes drawn into images from buffers | 0 | 879 to 1048 KB |
| The GPU's time | 1.15 to 1.22 ms | 1.41 to 1.50 ms |

The emulator no longer waits for the render thread: its vblanks went from 152 to 172 per 10 s to 206 to 210. Both
logs start with one present that spent 10.6 to 10.8 s in 930 `vkCmdBlitImage` the GPU didn't draw (type 14), the same
either side.

## vkbench and where the code lands

`vkbench.log`: 17 passed. Against `9125a52`, `uniforms_1000` went from 663 to 714 us a frame, though no function a draw
runs through changed: `vkCmdCopyBufferToImage` grew 752 bytes and moved everything after it 768. Four builds in one
session (`vkbench_layout_*.log`), draw+submit in us, recording and submit apart for the two that moved:

| Scene | `9125a52` | `9125a52` + 768 bytes | `f780ff4`, `draw_blit` inlined | `f780ff4` |
|---|---|---|---|---|
| draws_250 | 222 | 222 | 218 | 227 |
| draws_1000 | 840 | 841 | 822 | 856 |
| draws_4000 | 3658 | 3659 | 3585 | 3715 |
| states_250 | 323 | 327 | 320 | 327 |
| states_1000 | 1242 (325 + 917) | 1259 (325 + 934) | 1229 (326 + 903) | 1259 (325 + 933) |
| uniforms_1000 | 663 (273 + 390) | 714 (324 + 390) | 703 (324 + 379) | 714 (324 + 390) |

The 768 bytes never run: a function of `.space` before `vkCmdBlitImage`, which puts `vkCmdDrawIndexed`, `prepare_draw`
and `draw` at `f780ff4`'s addresses. It gives `f780ff4`'s numbers, so placement alone moved the scenes, by -2% to +8%.
`uniforms_1000`'s 51 us is all recording. Taking the CPU's L1 instruction cache as 32 KB, two ways of 128-byte lines,
its set the address's bits 7 to 13, three lines that each of its draws runs share set 3 in the builds that moved:
vkbench's uniform loop (`vulkan_draw_meshes` at `82054180`), `record`'s first line (`8205c180`) and
`vkCmdDrawIndexed`'s entry (`82060180`). In `9125a52`, `vkCmdDrawIndexed` sits in sets 125 to 127.
