"""Breaks milestone 30, passes past eDRAM recorded into the queue's space, in a copy of vulkan4xenon, a set at a time:
breaks.py --list; breaks.py SET LAYER, which applies it to that copy.

Each break is a small source change that should fail the vulkan4xenon_test checks named beside it, and no others;
builds.sh builds each set into a title of its own, v4x_m30_<set>.xex.
- wait_last: each pass waits for the GPU to finish the pass before it, as each waited for its framebuffer's last one
  before milestone 30.
- no_span_wait: a pass's room taken without waiting for the passes recorded there, so the CPU writes over recordings
  the GPU may still run. The GPU may hang instead of the check failing, so run this set last, from a fresh boot.
"""
import pathlib
import sys

STACK = ("tiles: 8 passes past eDRAM at 2 samples in one submission on one framebuffer, each loading the last and "
         "adding a strip, every texel of the resolve attachment")
WRAP = ("tiles: 12 passes past eDRAM at 2 samples in one submission on one framebuffer, recorded past the queue's "
        "2.5 MB of space, so it starts again over passes the GPU may still run, every texel of the resolve attachment")
BUSY = ("tiles: 8 passes past eDRAM at 2 samples, each keeping the GPU busy, return from vkQueueSubmit in under a "
        "quarter of the time until their fence")

WAIT = "\tif (covered)\n\t\twait_for_span(span_at(queue, covered - 1));\n"

# Each set's breaks: the file, the text it changes, what it becomes, and the checks it should fail. STACK fails under
# neither: its passes are small, so no pass's room reaches another's recording, and waiting changes no pixel.
SETS = {
    "wait_last": [
        ("src/queue.c", WAIT, "\tif (queue->tile_span_count)\n\t\tcovered = queue->tile_span_count;\n" + WAIT, [BUSY]),
    ],
    "no_span_wait": [
        ("src/queue.c", WAIT, WAIT.replace("if (covered)", "if (covered && false)"), [WRAP]),
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
