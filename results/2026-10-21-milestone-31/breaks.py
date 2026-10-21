"""Breaks milestone 31, colour attachments 16 texels or less on a side, in a copy of xsdk, a set at a time:
breaks.py --list; breaks.py SET XSDK, which applies it to that copy.

Each break is a small change to xsdk that should fail the checks named beside it, in xsdk_test and vulkan4xenon_test,
and no others; builds.sh builds both titles against each, xsdk_m31_<set>.xex and v4x_m31_<set>.xex. Each touches only
textures with packed mips off, which only those checks make, and neither can hang the GPU.
- fetch_packed: xsdk_gpu_set_texture sets packed mips in every fetch constant it writes, so the GPU reads a small base
  from the tail while the resolves and the CPU use the corner.
- place_tail: xsdk_texture_place puts level 0 in the tail whatever the bit, so uploads and read-backs use the tail while
  the resolves and the GPU use the corner.
"""
import pathlib
import sys

SAMPLING = ("sampling: 20x11 and 11x20 textures of one level with their bases at the corner, packed mips off, one "
            "texel to a pixel")
READ_BACK = ("small targets: each drawn from a source of its own, read back by vkCmdCopyImageToBuffer, every texel its "
             "own")
SAMPLED = "small targets: each sampled 2 by 2 pixels a texel into a 128x64 attachment, read back, every pixel its texel"

# Each set's breaks: the file in xsdk, the text it changes, what it becomes, and the checks it should fail.
SETS = {
    "fetch_packed": [
        ("src/gpu_draw.c", ": texture->fetch[i] | sampler->fetch[i]);",
         ": texture->fetch[i] | sampler->fetch[i] | (i == 5 ? 1u << 11 : 0));", [SAMPLING, SAMPLED]),
    ],
    "place_tail": [
        ("src/texture_layout.c", "if (layout->packed && level >= packed_level) {", "if (level >= packed_level) {",
         [SAMPLING, READ_BACK]),
    ],
}


def apply(name, root):
    texts = {}
    for path, old, new, checks in SETS[name]:
        file = root / path
        text = texts.get(file, file.read_text())
        assert text.count(old) == 1, (path, old)
        texts[file] = text.replace(old, new)
        print(f"{name}: {path}: should fail {'; '.join(checks)}")
    for file, text in texts.items():
        file.write_text(text)


if sys.argv[1] == "--list":
    print(" ".join(SETS))
else:
    apply(sys.argv[1], pathlib.Path(sys.argv[2]))
