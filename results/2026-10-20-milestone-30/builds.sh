#!/bin/bash
# Builds each break set of breaks.py, or the sets named, into a copy of the layer's sources against XSDK, the xsdk
# beside the layer by default, as v4x_m30_<set>.xex in OUT, the layer's build/broken by default, which git ignores. The
# tree itself is never changed. Build xsdk first; run it in WSL.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
XSDK=${XSDK:-$(cd "$REPO/../Xbox-360-SDK" && pwd)}
OUT=${OUT:-$REPO/build/broken}
SETS=${*:-$(python3 "$HERE/breaks.py" --list)}
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
for set in $SETS; do
	copy=$(mktemp -d "$OUT/copy.XXXXXX")
	cp -r "$REPO"/{src,include,tests,tools,third_party,bench,Makefile} "$copy"
	python3 "$HERE/breaks.py" "$set" "$copy"
	log=$OUT/broken_$set.txt
	if make -C "$copy" -j32 XSDK="$XSDK" build/vulkan4xenon_test.xex >"$log" 2>&1; then
		cp "$copy/build/vulkan4xenon_test.xex" "$OUT/v4x_m30_$set.xex"
	else
		grep -E "error:|Error|No rule" "$log"
	fi
	rm -rf "$copy"
done
ls -la "$OUT"/v4x_m30_*.xex
