# Command buffers that hold the GPU's packets

A design for review, 2026-11-06, proposed as milestone 35. Nothing here is built. xsdk's part needs the user's OK
first.

## Why

A cube in vkbench's `draws_4000`, two `vkCmdPushConstants` and a `vkCmdDrawIndexed`, costs the CPU 849 ns through the
layer on the percall branch (`f04e5b8`), against 285 to 290 through xsdk. Recording takes 248 ns and the submit 601.
Of the submit, xsdk's constant setters and draw take about 261 ns, by milestone 29's profile split, and the replay's
own work about 340: the dispatch, checks, resources, pushes and pipeline. The figures come from the coordinator's
console run (results/console/before and after, uncommitted). The goal is 1.3 to 1.5 times xsdk's cost.

The replay keeps the commands, then works each draw out again at the submit. Milestones 29 and 30 left translating at
record time out, for four reasons (ROADMAP.md, each milestone's "Left out"):
- a pass's begin and end depend on what eDRAM holds at the submit;
- uniform buffers the GPU can't load are read on the CPU at the submit;
- indirect draws are known only at the submit;
- recordings can't nest inside a tiled pass's.

The design below keeps each of these at the submit, or answers it.

## The design in short

- **Recording builds the packets.** Each command buffer has a recorder that does a draw's work at the `vkCmd*` call:
  today's `prepare_draw`, `apply_resources`, `point_streams` and the draw and its pieces. It writes the packets with
  xsdk's builders into the command buffer's own packet buffer, in cached host memory.
- **What depends on the submit stays a command,** replayed as now:
  - render passes' begins and ends;
  - copies, blits, uploads and image clears;
  - a depth image's resolve at a barrier;
  - query resets and result copies;
  - secondary command buffers;
  - in step 1, also `vkCmdClearAttachments`, queries, time stamps and events.

  The packets between two such commands form a **run**.
- **The submit copies each run into xsdk's stream** through `xsdk_gpu_reserve`, the same stream xsdk's own setters
  write. On the way it fills in the few dwords that depend on the submit: the hierarchical depth control, and constants
  read from uniform buffers.

## What a command buffer holds

- **The packet buffer.** Host memory, 16-byte aligned, that doubles when full and keeps its size through resets, as the
  command list does now. A run starts on 16 bytes.
- **The command list,** shorter than now. Binds, pushes, draws, viewports, scissors and dynamic state no longer become
  commands. A run is one command, `COMMAND_RUN`, which holds:
  - its offset and size in the packet buffer, cut into pieces of at most 4,096 dwords at draw boundaries;
  - its HiZ sites and read patches (both below);
  - the images its draws sampled and the query pools it wrote;
  - whether it drew or waited on an event.
- **Snapshots,** in the data arena. A draw that needs the submit (indirect parameters, repeated indices) becomes a
  command holding the recorder's bound state.

## The recorder

It lives in the command buffer, so command buffers can record on several threads at once, as Vulkan allows. The
`static` scratch arrays in queue.c (`block`, `held_values`) move into it. It holds:
- **The Vulkan state bound:** the pipeline, the sets with their dynamic offsets as values, a 128-byte copy of the push
  constants with a mask of the quads pushed, the vertex and index bindings, the viewport, scissor and dynamic state.
  A push copies its bytes in. It no longer takes an arena slot and a command.
- **What the GPU holds since the run began,** as the replay tracks it now:
  - `AppliedState`, `HeldConstants`, and the streams and texture slots pointed;
  - the loop words, `PA_CL_CLIP_CNTL`, the shaders loaded, and the HiZ inputs.
- **Its own copies** of the float constants, the streams' fetch groups and the boolean and loop block. xsdk's builders
  fill each group's other registers from these, as xsdk's setters fill from xsdk's copies now
  (xsdk:src/gpu_constants.c:77-103, src/gpu_draw.c:587-608).

**Deduplication runs within a run only.** At a run's start the recorder knows nothing, so the first draw writes all
the state it reads, as the replay does after each pass's begin now. Nothing carries across runs or command buffers.
Their order, and the begins, blits and other command buffers that run between them, are known only at the submit.

**Group filling stays correct, for the reason f04e5b8 gave.** xost packs the push constants, then the blocks, then the
literals in consecutive registers, so groups of 4 are shared between resources (xost:docs/xsc.md:196-204, 432-442).
Every register a draw reads is written within the run or is known to hold its value. So a register a group fill takes
from the recorder's copy is either known, written later in the same draw, or read by nothing. That is the replay's
case with xsdk's copy.

**No templates per pipeline.** Building a pipeline's state from parts costs a compare per part and keeps the dedupe.
A template would have to carry the literals' shared groups and the HiZ sites, and couldn't drop the parts that didn't
change. If `states_1000` shows pipeline application dominating after step 1, templates are a later measure. Dynamic
offsets need no patching: `vkCmdBindDescriptorSets` gives them, so loads carry their final addresses.

## The submit

`vkQueueSubmit` keeps its `sync`, its invalidation of all memory and its fence, and walks the commands as now. For each
run it does this:
1. **Copies the pieces.** Each piece goes through `xsdk_gpu_reserve`, padded first with type-2 NOPs, so the stream's
   16-byte phase matches the buffer's. Constant payloads then stay on 16 bytes, and the copy can use VMX stores into
   write-combined memory. A reservation takes up to 8,192 dwords (xsdk:src/gpu.c, `OVERFLOW_SCRATCH_DWORDS`).
2. **Writes the HiZ sites.** `RB_HIZCONTROL` follows whether a resolve cleared the surface's depth, and from which
   side (xsdk:src/gpu_draw.c:679-695, src/gpu_resolve.c:384-391). The pass's begin decides that at the submit. The
   recorder notes each `RB_HIZCONTROL` dword it writes, with the depth control and whether the pixel shader writes
   depth there. It writes one for each depth-stencil set, and for each shader set where xsdk's `xsdk_gpu_set_shaders`
   would put one, or wherever a run's start leaves that unknown. The copier:
   - takes xsdk's HiZ state;
   - steps through the sites with `xsdk_gpu_hiz_control` and writes each value;
   - hands the end state back.

   The values are the ones the replay's own calls would have made.
3. **Applies the read patches.** Some values are known at recording by where they're read from, but not by value:
   - a uniform block that doesn't start a group;
   - a block whose address or range the GPU can't load (`PLAN_READ`, and `load_uniform_buffer`'s fallback);
   - a loop count read from a uniform buffer;
   - a clip plane read from one.

   The recorder writes the packet with placeholder values in those registers and notes where they go. The copier
   reads the buffer then, as the replay does now, and writes the values in. The recorder counts those registers as
   unknown afterwards, so the next draw writes them again.
4. **Marks what the run used:** `used_serial` for each image it sampled and each query pool it wrote, and
   `queue->drawn` if it drew.

A snapshot draw goes through a scratch recorder the queue keeps, starting from nothing, with the parameters or
indices read at the submit. Its packets are copied like a run's. One implementation, the recorder, serves both the
recording and the submit, so the replay's draw code leaves queue.c.

## Copy, or indirect buffers (decision 1)

The other way to send a run is to record into GPU memory and run it from the ring with `PM4_INDIRECT_BUFFER`
(`xsdk_gpu_execute`). Recommended: the copy.

| | Copy into the stream | Indirect buffer |
|---|---|---|
| A draw's 25 dwords | Into cached memory, then copied: from the cache at 5.5 GB/s, from memory at 1.5 (results/console/after/vkbench.log:6-8), so about 20 to 100 ns more than writing in place | Written into write-combined memory at 3.8 GB/s, about 26 ns; a kick of 1.1 to 1.3 us a run (xsdk:docs/HARDWARE.md, "The cost of a draw") |
| Tiled passes | The copy lands in the pass's recording, which each band runs | Can't nest: the console has run no buffer from inside another (xsdk:docs/HARDWARE.md, "Recorded command buffers, milestone 18"). xsdk would need a way for a band to run several buffers. |
| HiZ | Worked out exactly at each copy | A guess at recording, patched in GPU memory at the submit where it's wrong, and a buffer pending on the GPU can't be patched back on |
| Uniform reads | Written into the copy | Patched in GPU memory, waiting for a pending submission first |
| Resets, simultaneous use, a secondary run twice | Host memory only, never waits | Memory the GPU may still read: resets and patches wait on fences |
| xsdk changes | Builders, and the HiZ state calls | The same, plus the tiles' runs, and an execute without a kick to keep runs cheap |

The copy costs more a draw and is much simpler. Step 1 measures what it costs. Indirect buffers are the next measure
if step 1 shows the copy and the cached stores together above about 60 ns a draw.

## How the replay's work maps

| What the replay does at the submit | After |
|---|---|
| Draws and their state: pipeline, viewport and scissor, dynamic state, push constants, uniform loads, textures, streams, loops, clip planes, instances, pieces | Packets at recording |
| Uniform blocks, loop counts and clip planes read from uniform buffers | Packets at recording, values filled in at the submit |
| The hierarchical depth control | Packets at recording, values worked out at the submit |
| A render pass's begin and end: placement, draw-backs, clear resolves, end resolves, put-off resolves | Commands at the submit, unchanged. `resolve_can_wait` looks at a run's sampled images where it now follows set binds and draws, and a run with an event in it stops the look, as an event command does now. |
| Tiled passes (milestone 30) | Begin and end unchanged, the load part recorded through xsdk into the queue's space. A run inside the pass is copied into that recording, so every band runs it and nothing nests. The 1 MB a pass is unchanged. |
| Drawn blits, drawn uploads with their page invalidation (73d0d61), depth copies, and the CPU's copies, blits, image clears, buffer copies, updates and fills | Commands, unchanged |
| Barriers | Nothing, but a sampled depth image's resolve, a command as now |
| Events | Commands in step 1. Packets from step 2: the write, the wait, the invalidation. The recorder forgets its loads after a wait. |
| Occlusion queries and time stamps | Commands in step 1, packets from step 2. The query's visibility id is counted at recording. Resets and result copies stay commands. A secondary that doesn't know its framebuffer keeps its queries as commands, so they are still refused in a tiled pass. |
| `vkCmdClearAttachments` | A command in step 1, with the query pause. Packets from step 3. |
| Indirect draws, and instanced indexed lists of repeated indices | Snapshot commands, drawn at the submit by the scratch recorder |
| Secondary command buffers | Their own runs and commands, walked in place at the submit. Each starts with no state bound, as Vulkan promises. The primary forgets what the GPU holds after one and keeps what it bound. |
| Resets and reuse | Nothing for the GPU lives in a command buffer, so a reset never waits |
| Multiple queues | One queue, as now |

## What xsdk would change (needs the user's OK)

1. **A public packet cursor,** `XsdkPackets`: today's private `GpuPackets` (dwords, count, capacity). Builders write
   into it and assert there's room. Each builder's most dwords is public, so a caller can make room first.
2. **Builders for the draw path, step 0.** Each writes exactly the packets its setter queues today. It takes the state
   the setter reads from xsdk's own copies as arguments, so it touches no xsdk state and is safe on any thread. The
   setters become wrappers that reserve from the stream, so xsdk's packets don't change.
   - `xsdk_gpu_put_shaders(p, vertex, pixel, load_vertex, load_pixel)`: the IM_LOADs and program registers. The caller
     tracks what's loaded and puts `RB_HIZCONTROL` itself.
   - `xsdk_gpu_put_hiz_control(p, value)` and `xsdk_gpu_put_depth_stencil(p, state, hiz_control)`: each returns
     where the `RB_HIZCONTROL` value went.
   - `xsdk_gpu_put_blends`, `xsdk_gpu_put_blend_constants`, `xsdk_gpu_put_scissor`, `xsdk_gpu_put_viewport`.
   - `xsdk_gpu_put_raster(p, state, samples, clip_control)` and `xsdk_gpu_put_clip_planes(p, mask, planes,
     clip_control)`. `clip_control` is the caller's `PA_CL_CLIP_CNTL`, or a value meaning unknown, which the builders
     update. `msaa_enable` comes from the samples passed in, not the last target set.
   - `xsdk_gpu_put_vertex_constants(p, kept, first, values, count)` and the pixel shader's version. `kept` is the
     caller's 16-byte aligned copy of the stage's registers. Each returns where register `first` went, for a read
     patch.
   - `xsdk_gpu_put_load_vertex_constants` and `xsdk_gpu_put_load_pixel_constants`.
   - `xsdk_gpu_put_loop_constants(p, kept, first, values, count)`: `kept` is the caller's booleans and loops, 40
     words. It returns where loop `first` went.
   - `xsdk_gpu_put_vertices(p, groups, stream, address, size_bytes)`: `groups` is the caller's copy of the streams'
     fetch groups, with their count made public.
   - `xsdk_gpu_put_texture`, `xsdk_gpu_put_draw` and `xsdk_gpu_put_draw_indexed`, and the instanced pair, which take
     the caller's copy and the shader's instance constant.
   - `xsdk_shader_writes_depth(shader)`: `PS_EXPORT_DEPTH` is private now.
3. **HiZ state across packets xsdk didn't write.**
   - `XsdkHiz`: kept, greater, the pixel shader writes depth, the depth control.
   - `xsdk_gpu_hiz_state(&hiz)`.
   - `xsdk_gpu_hiz_control(&hiz, depth_control)`: today's static `hiz_control` as a pure step, which drops kept on a
     write it can't follow.
   - `xsdk_gpu_packets_written(&hiz)`: after a title wrote packets through `xsdk_gpu_reserve`, it forgets the shaders
     loaded and the clip control, as `xsdk_gpu_execute` does, and takes the HiZ state they left.

   xsdk's float, fetch-group and loop copies then hold what xsdk last set, not what the GPU holds. That is stated in
   the header. It is harmless for the layer: after a run, everything the GPU draws sets what it reads (the layer's
   blits, xsdk's resolves).
4. **Step 2's builders:** `xsdk_gpu_put_begin_visibility`, `put_end_visibility`, `put_write_word`, `put_wait_word`,
   `put_write_clock` and `put_invalidate`.

Not needed: indirect buffer or tiling changes, a set-if-changed path, or access to xsdk's copies. The recorder keeps
its own.

## Expected cost of a draw

These are estimates from xsdk's per-call clocks (xsdk:docs/HARDWARE.md, "The cost of a draw, milestone 38": vertex
constants 339 clocks, the indexed draw 154, at 3.2 GHz) and vkbench's call timings. Step 1 measures them.

| `draws_4000`, per draw | Now (ns) | Expected (ns) |
|---|---|---|
| `vkCmdPushConstants`, 64 bytes | 113 | 25-40: a copy into the push copy, no arena or command |
| `vkCmdPushConstants`, 16 bytes | 77 | 20-30 |
| Draw dispatch and checks, pipeline unchanged | 15 at recording, plus the replay | 15-25 |
| The vertex push block: compared, then 4 registers in 20 dwords | the replay's, plus about 106 in xsdk | 85-130 |
| The pixel push block: compared, held, left out | the replay's | 10-15 |
| Streams unchanged, then the indexed draw's 5 dwords | the replay's, plus about 48 in xsdk | 30-60 |
| The submit: the run's 25 dwords copied | 601 for the whole submit | 20-70 |
| **A draw** | **849** | **205-370, 0.7 to 1.3 times xsdk's** |

The layer writes 25.1 dwords a draw against xsdk's scene's 45.1, as it leaves out the unchanged tint
(results/console/after/vkbench.log:22, 54). `states_1000` (a pipeline a draw) and `uniforms_1000` (dynamic offsets a
draw) do the same work moved to recording. Their gain is measured in step 1, not estimated here. The GPU runs the same
packets, so its time shouldn't move.

## Risks

- **The estimate is a guess.** The replay's 340 ns may partly move with its work. Step 1 ends at a gate.
- **Cached stores.** A packet buffer that leaves the 1 MB L2 costs a read for each line stored (cached stores 1.3 GB/s,
  vkbench.log:6). Zeroing new lines with `dcbz` is the cheap fix, and decision 1's indirect buffers the larger one.
- **HiZ.** A missed site leaves the buffer rejecting what it shouldn't. The sites mirror xsdk's two writers, and a break
  build checks them.
- **xsdk's setters rewritten** touch every title. Their packets stay byte for byte as now, and xsdk_test checks it on
  the console.
- **Behaviour changes,** in docs/SUPPORT.md:
  - descriptor sets are read at recording, as Vulkan requires;
  - a secondary starts with no state;
  - a draw with no pipeline bound is refused at recording;
  - events' shortcut is unchanged, as CPU reads stay at the submit.
- **Memory.** About 100 bytes of host memory a draw for each command buffer, kept through resets.
- **The profile build.** The per-draw stages move to recording, the copy becomes a stage of the submit, and the counts
  are kept per command buffer and added up at the submit. Profile builds already accept racing totals.

## Order of work

Each step ends with vulkan4xenon_test and vkbench passing on the console. Each has break builds that fail what they
name.
0. **xsdk** (with the user's OK): the cursor, step 0's builders and the HiZ calls, the setters as wrappers.
   - Checks: xsdk_test on the console, all passing; vkbench through xsdk with the same pictures, its times within
     noise.
   - Break: the float builder fills groups with zeros instead of the kept copy. It should fail xsdk_test's constant
     checks.
   - Size: about 400 lines of xsdk, mostly moved, and about 150 lines of header.
1. **The recorder** (src/record.c). queue.c's draw code moves there, on the builders. command_buffer.c records into it;
   runs, the copy, HiZ sites, read patches, snapshots and secondaries come with it. Clears, queries, time stamps and
   events stay commands. The profile build is adapted, and SUPPORT.md updated.
   - Checks: vulkan4xenon_test 570 of 570; vkbench 17 of 17 with every picture as xsdk's; `draws_4000`'s recording
     and submit a draw.
   - Breaks:
     - the HiZ sites copied as recorded: the partial depth clear's quad at 0.875 should fail (milestone 27);
     - read patches skipped: the uniform buffers' CPU-read check;
     - the recorder keeping what it knew across a command: f04e5b8's clear between quads.
   - Gate: if `draws_4000` costs over 1.5 times xsdk's draw, the measurement goes back before step 2.
2. **Events, queries and time stamps as packets,** with xsdk's step 2 builders.
   - Checks: test_events, test_occlusion and the time stamps.
   - Break: the visibility id not counted past a clear.
3. **`vkCmdClearAttachments` as packets:** the blit's clear through the recorder.
   - Checks: test_clears and f04e5b8's check.
4. **Dusklight.** The title scene and the logo shot in the diagnostics build, as milestones 29 and 30 measured them:
   recording and submit a frame, and the frame rate. PPSSPP too, if the coordinator wants it.

## Decisions

1. **The copy, or indirect buffers.** Recommended: the copy, with indirect buffers if step 1's gate or cost says so.
2. **Builders in xsdk, or the layer writing PM4 itself.** Recommended: builders in xsdk. The other way needs no xsdk
   change but copies xsdk's register knowledge into the layer.
3. **Step 1 in one piece.** Splitting it leaves a stretch where the draw code exists twice. Recommended: one step, with
   its gate before the rest.

## Left out

- Indirect buffers (decision 1).
- Pipeline templates.
- Deduplication across runs or command buffers.
- Pass begins and ends built at recording.
- Command buffers kept across submissions as GPU packets.

## What step 1 measured, and why it isn't shipped

Step 1 was built as designed and reverted on 2026-11-06, having missed its gate twice on the console; the user had
allowed one round past the first miss. Its commits stay in percall's history (`9616dce`, `e2cc1f8`, `912eeec`) with
their reverts after them. Step 0 stays on xsdk's branch `v35` (`03edf7a`, `b640e45`, `74ffae9`), unmerged: the builders
and the hierarchical buffer's calls, with xsdk_test's packet checks, and `set_floats` back on 3960a1d's fast path.

- **Round 1** (results/console/v35s1*): the recorder, runs, the copy, hierarchical depth at the copy, uniform reads,
  snapshots and secondaries. vulkan4xenon_test passed 570 of 570; its read break failed its three named checks alone,
  and its hierarchical depth break 20 depth checks. `draws_4000` took 3,396 us through the layer against 1,170 through
  xsdk, 2.90 times, recording 3,160 and the submit 234: a submit cost 57 ns a draw, but a `vkCmdDrawIndexed` that
  changed nothing cost 396 ns and a 64-byte push 80. The replay's work had moved into the draw without shrinking.
- **Round 2** (results/console/v35r2*): a draw wrote only what changed since the last one, pushes compared as they
  were made, and the profile build timed the recording's parts. vulkan4xenon_test passed 570 of 570 and vkbench 17 of
  17. `draws_4000` took 2,701 us against 1,157, 2.33 times; `draws_1000` 659 us, 2.23 times. `states_1000` took 1,323
  us and `uniforms_1000` 774, 2.51 and 2.63 times, worse than percall's 1,275 and 684; `states_250` 345 against 328.
  The recording probe put a `draws_4000` draw, two pushes and the draw, at 298 ns, the draw alone at 128. The profile
  put `draws_4000`'s recording at 469 ns a draw: push constants written 241, pushes compared 126, the draw packet 36,
  streams 21, state 20, checks 18, resources 7; the submit 57. A pipeline change in `states_1000` cost 401 ns of
  resources and 255 of state, and a set bound in `uniforms_1000` 286 of resources.

So the copy and the submit did what the design said, 57 ns a draw against the replay's 601, but recording cost more
than the replay saved, and the scenes that change pipelines or sets every draw got slower. The gate wanted 1.5 times
xsdk's cost.

Leads, should this be tried again:
- **The push write:** 241 ns for one 4-register write through xsdk's builder, where xsdk's own setters write a whole
  draw into its stream in about 290. Most likely the packets' fresh lines: each draw stores into cached lines the core
  hasn't touched, and each store's line is fetched first. Zeroing a line ahead with `dcbz` before the builders write
  it would show.
- **A pipeline or set change:** 255 to 401 ns working out state and resources at each change. Their packets could be
  built once, when a pipeline is made or a set is written or bound, and copied at the draw.
- **The scene against the probe:** the scene's recording took about twice the warm probe's, so the cache, more than
  the instructions, may be the cost: the packets, the recorder and the pipeline's spread-out fields.
