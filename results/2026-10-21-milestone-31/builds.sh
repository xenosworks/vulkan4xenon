#!/bin/bash
# Builds each break set of breaks.py, or the sets named, into a copy of XSDK, the xsdk beside the layer by default, and
# a copy of the layer's sources against that: xsdk_m31_<set>.xex and v4x_m31_<set>.xex in OUT, the layer's
# build/broken by default, which git ignores. Neither tree is changed. Run it in WSL.
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
	mkdir -p "$copy/Xbox-360-SDK" "$copy/vulkan4xenon"
	tar -C "$XSDK" -cf - --exclude=build --exclude=xenos-toolchain/reference --exclude=xenos-toolchain/tools/glslang \
		assets cmake imports include src tests tools xenos-toolchain Makefile | tar -C "$copy/Xbox-360-SDK" -mxf -
	cp -r "$REPO"/{src,include,tests,tools,third_party,bench,Makefile} "$copy/vulkan4xenon"
	python3 "$HERE/breaks.py" "$set" "$copy/Xbox-360-SDK"
	log=$OUT/broken_m31_$set.txt
	if make -C "$copy/Xbox-360-SDK" -j32 build/libxsdk.a build/xsdk_test.xex >"$log" 2>&1 &&
		make -C "$copy/vulkan4xenon" -j32 XSDK="$copy/Xbox-360-SDK" build/vulkan4xenon_test.xex >>"$log" 2>&1; then
		cp "$copy/Xbox-360-SDK/build/xsdk_test.xex" "$OUT/xsdk_m31_$set.xex"
		cp "$copy/vulkan4xenon/build/vulkan4xenon_test.xex" "$OUT/v4x_m31_$set.xex"
	else
		grep -E "error:|Error|No rule" "$log"
	fi
	rm -rf "$copy"
done
ls -la "$OUT"/*_m31_*.xex
