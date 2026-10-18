"""Breaks milestone 17's first part, render passes past eDRAM at one sample, in copies of vulkan4xenon and xsdk, a set
at a time: breaks.py --list; breaks.py --xsdk SET, which succeeds if the set breaks xsdk; breaks.py SET LAYER XSDK,
which applies it to those copies.

Each break is a small source change that should fail the vulkan4xenon_test checks named beside it, and no others;
builds.sh builds each set into a title of its own. Sets whose breaks would fail the same checks are kept apart.
- a: the load's draw-back skipped in bands after the first.
- b: xsdk's tiled pass leaving the window offset at 0, so every band draws band 0's rows.
- c: depth stored in bands not drawn back; the layer's guard of c252 gone; depth whole asked for at tile 0, not at
  eDRAM's top, where the untiled pass after looks for it.
- d: no attachment marked drawn back, so band 0's clear resolves eDRAM's junk into the images the pass loads.
- e: xsdk's band 0 clear skipped after a pass laid out the same that cleared to other colours.
"""
import pathlib
import sys

FOUR = ("tiles: four attachments past eDRAM at 1280x720, attachment {}, every texel after a pass clearing all and one "
        "loading all and depth")
FOUR_FRAMES = ("tiles: 3 frames of four attachments past eDRAM loaded and stored across two passes, then an untiled "
               "one, the first attachment on screen in every band")
ATTACHMENTS = [FOUR.format(a) for a in ("0, R8G8B8A8_UNORM", "1, A2B10G10R10_UNORM_PACK32", "2, R16G16B16A16_SFLOAT",
                                        "3, R32_SFLOAT")]
UNTILED_AFTER = "tiles: an untiled pass after them loads the depth they left whole at eDRAM's top, every texel"
BIG_FRAMES = ("tiles: 3 frames of a 2048x2048 shadow map in bands, then a pass loading its depth, its top left on "
              "screen")
BIG_DEPTH = "tiles: a 2048x2048 shadow map past eDRAM, every texel's depth in its memory, stored a band at a time"
BIG_LOADED = "tiles: a 2048x2048 pass loading the shadow map's depth, drawn back a band at a time, every texel"
BIG_SAMPLED = "tiles: a 2048x2048 pass past eDRAM sampling the shadow map, every texel's red its depth"
HIGH_BLOCK = ("tiles: a fragment shader's uniform block reaching c252, which xsdk keeps, refused, and "
              "maxUniformBufferRange 4032")

DRAW_BACK = """	for (uint32_t i = 0; i < count; i++) {
		if (render_pass->load_ops[i] != VK_ATTACHMENT_LOAD_OP_LOAD)
			continue;
		v4x_blit_draw(&queue->blit, &framebuffer->attachments[i]->texture, framebuffer->width, framebuffer->height,
			&replay->area, i, count);
		framebuffer->attachments[i]->used_serial = replay->serial;
	}
"""

# Each set's breaks: the tree, the file, the text it changes, what it becomes, and the checks it should fail.
SETS = {
    "a": [
        ("layer", "src/queue.c", DRAW_BACK,
         "\txsdk_gpu_set_draw_rows(0, 1);\n" + DRAW_BACK + "\txsdk_gpu_set_draw_rows(0, framebuffer->height);\n",
         [FOUR_FRAMES] + ATTACHMENTS),
    ],
    "b": [
        ("xsdk", "src/gpu_tiles.c", "const uint32_t window = ((uint32_t)-(int32_t)top & OFFSET_MASK) << Y_SHIFT;",
         "const uint32_t window = 0;",
         [FOUR_FRAMES] + ATTACHMENTS + [UNTILED_AFTER, BIG_FRAMES, BIG_DEPTH, BIG_LOADED, BIG_SAMPLED]),
    ],
    "c": [
        ("layer", "src/queue.c", "\tif (draws_depth_back(replay)) {\n", "\tif (false && draws_depth_back(replay)) {\n",
         [BIG_FRAMES, BIG_LOADED]),
        ("layer", "src/pipeline.c", "const uint32_t constants = vertex ? V4X_FLOAT_CONSTANTS : XSDK_PIXEL_CONSTANTS;",
         "const uint32_t constants = V4X_FLOAT_CONSTANTS;", [HIGH_BLOCK]),
        ("layer", "src/render_pass.c", "\t\t.depth_top = pass->has_depth };", "\t\t.depth_top = false };",
         [UNTILED_AFTER]),
    ],
    "d": [
        ("layer", "src/queue.c", ".drawn_back = render_pass->load_ops[i] == VK_ATTACHMENT_LOAD_OP_LOAD };",
         ".drawn_back = false };", [FOUR_FRAMES] + ATTACHMENTS),
    ],
    "e": [
        ("xsdk", "src/gpu_tiles.c",
         "same = resolves[i].drawn_back || resolves[i].target == XSDK_TILE_DEPTH || cleared_to(&resolves[i]);",
         "same = cleared_to(&resolves[i]) || true;", [BIG_FRAMES, BIG_LOADED]),
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
