"""Breaks milestone 17's second part, multisampling, in copies of vulkan4xenon and xsdk, a set at a time:
breaks.py --list; breaks.py --xsdk SET, which succeeds if the set breaks xsdk; breaks.py SET LAYER XSDK, which applies
it to those copies.

Each break is a small source change that should fail the vulkan4xenon_test checks named beside it, and no others;
builds.sh builds each set into a title of its own, v4x_m17b_<set>.xex. Each set is one break, as four of them break xsdk.
- resolve0: xsdk's resolves copying sample 0, so resolve attachments do.
- offset0: xsdk's tiled pass leaving the window offset at 0, so every band draws band 0's rows.
- atoc_off: alphaToCoverageEnable ignored.
- drawback1: the load's draw-back skipped in bands after the first.
- depth_last: xsdk's band resolves run in the order given, so the depth resolve the layer lists last clears depth.
- hiz_off: a control, not a break: the hierarchical buffer off in every tiled pass, failing nothing.
- depth_sample1: xsdk's depth resolves copying sample 1 at 2 and 4 samples, so vkCmdResolveImage of depth does.
"""
import pathlib
import sys

EDGES = [f"multisample: {name} at {samples} samples: the resolve attachment's edge pixels blend in {step}, the rest as "
         "worked out"
         for name in ("R8G8B8A8_UNORM", "A2B10G10R10_UNORM_PACK32")
         for samples, step in ((2, "halves"), (4, "quarters"))]
ATOC = {samples: f"multisample: alpha to coverage at {samples} sample{'s' if samples > 1 else ''}: squares at alphas 0 "
        "to 1 in 16ths, each pixel the share of its samples covered" for samples in (1, 2, 4)}
MSAA = ("2 samples, depth whole", "4 samples, depth in bands")
MSAA_FRAMES = [f"tiles: 3 frames at {name} of passes past eDRAM storing and loading colour and depth, resolved and on "
               "screen" for name in MSAA]
MSAA_TEXELS = [f"tiles: {name} past eDRAM at 1280x720, every texel of the resolve attachment after a pass clearing "
               "colour and depth and one loading both, edges at the samples' steps" for name in MSAA]
DEPTH_RESOLVES = [f"tiles: {name} past eDRAM at 1280x720, vkCmdResolveImage of its depth after both passes, every "
                  "texel sample 0's" for name in MSAA]

# The first part's checks these breaks fail too (results/2026-10-18-milestone-17-part-1/breaks.py).
FOUR = ("tiles: four attachments past eDRAM at 1280x720, attachment {}, every texel after a pass clearing all and one "
        "loading all and depth")
FOUR_FRAMES = ("tiles: 3 frames of four attachments past eDRAM loaded and stored across two passes, then an untiled "
               "one, the first attachment on screen in every band")
ATTACHMENTS = [FOUR.format(a) for a in ("0, R8G8B8A8_UNORM", "1, A2B10G10R10_UNORM_PACK32", "2, R16G16B16A16_SFLOAT",
                                        "3, R32_SFLOAT")]
UNTILED_AFTER = "tiles: an untiled pass after them loads the depth they left whole at eDRAM's top, every texel"
BIG = ["tiles: 3 frames of a 2048x2048 shadow map in bands, then a pass loading its depth, its top left on screen",
       "tiles: a 2048x2048 shadow map past eDRAM, every texel's depth in its memory, stored a band at a time",
       "tiles: a 2048x2048 pass loading the shadow map's depth, drawn back a band at a time, every texel",
       "tiles: a 2048x2048 pass past eDRAM sampling the shadow map, every texel's red its depth"]

DRAW_BACK = """	for (uint32_t i = 0; i < count; i++) {
		if (render_pass->load_ops[i] != VK_ATTACHMENT_LOAD_OP_LOAD)
			continue;
		v4x_blit_draw(&queue->blit, &framebuffer->attachments[i]->texture, framebuffer->width, framebuffer->height,
			&replay->area, i, count);
		framebuffer->attachments[i]->used_serial = replay->serial;
	}
"""

# Each set's breaks: the tree, the file, the text it changes, what it becomes, and the checks it should fail. Sample 0
# leaves edges at no step between, and alpha to coverage at 2 and 4 samples, which covers sample 0 first
# (xsdk:docs/HARDWARE.md, "Alpha to coverage, milestone 44 step 4"), resolves to the square's colour; one sample's
# copy is sample 0 already, and the copies and draw-backs compare the same sample.
SETS = {
    "resolve0": [
        ("xsdk", "src/gpu_draw.c", "return depth ? 0 : copy_sample_selects[samples_log2(target_samples)];",
         "return depth ? 0 : copy_sample_selects[0];", EDGES + [ATOC[2], ATOC[4]] + MSAA_TEXELS),
    ],
    "offset0": [
        ("xsdk", "src/gpu_tiles.c", "const uint32_t window = ((uint32_t)-(int32_t)top & OFFSET_MASK) << Y_SHIFT;",
         "const uint32_t window = 0;",
         [FOUR_FRAMES] + ATTACHMENTS + [UNTILED_AFTER] + BIG + MSAA_FRAMES + MSAA_TEXELS),
    ],
    "atoc_off": [
        ("layer", "src/pipeline_state.c", "setting.alpha_to_coverage = i == 0 && alpha_to_coverage;",
         "setting.alpha_to_coverage = i == 0 && alpha_to_coverage && false;", list(ATOC.values())),
    ],
    "drawback1": [
        ("layer", "src/queue.c", DRAW_BACK,
         "\txsdk_gpu_set_draw_rows(0, 1);\n" + DRAW_BACK + "\txsdk_gpu_set_draw_rows(0, framebuffer->height);\n",
         [FOUR_FRAMES] + ATTACHMENTS + MSAA_FRAMES + MSAA_TEXELS),
    ],
    # Only the 4-sample pass keeps the hierarchical buffer on in its bands: 1280 wide, 256 rows times 4 samples
    # (xsdk:src/gpu_draw.c, xsdk_gpu_hiz_fits); the 2048x2048 map is too wide and the rest keep depth whole.
    "depth_last": [
        ("xsdk", "src/gpu_tiles.c",
         "return (r->target == XSDK_TILE_DEPTH) == (k < count) && !(before && r->drawn_back);",
         "return k >= count && !(before && r->drawn_back);", [MSAA_FRAMES[1], MSAA_TEXELS[1]]),
    ],
    "hiz_off": [
        ("xsdk", "src/gpu_tiles.c", "if (pass.depth && !pass.depth_whole && pass.count)", "if (false)", []),
    ],
    # The 4-sample pass's stored depth, drawn back by the next, is sample 1's too; its quads' edges lie between pixels,
    # where every sample agrees, so only the triangle's edges, which the depth resolve checks, tell.
    "depth_sample1": [
        ("xsdk", "src/gpu_draw.c", "return depth ? 0 : copy_sample_selects[samples_log2(target_samples)];",
         "return depth ? target_samples > 1 : copy_sample_selects[samples_log2(target_samples)];", DEPTH_RESOLVES),
    ],
}


def apply(name, roots):
    texts = {}
    for tree, path, old, new, checks in SETS[name]:
        file = roots[tree] / path
        text = texts.get(file, file.read_text())
        assert text.count(old) == 1, (path, old)
        texts[file] = text.replace(old, new)
        print(f"{name}: {tree} {path}: should fail {'; '.join(checks)}")
    for file, text in texts.items():
        file.write_text(text)


if sys.argv[1] == "--list":
    print(" ".join(SETS))
elif sys.argv[1] == "--xsdk":
    sys.exit(0 if any(tree == "xsdk" for tree, *_ in SETS[sys.argv[2]]) else 1)
else:
    apply(sys.argv[1], {"layer": pathlib.Path(sys.argv[2]), "xsdk": pathlib.Path(sys.argv[3])})
