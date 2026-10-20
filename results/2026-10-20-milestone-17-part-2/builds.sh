#!/bin/bash
# Builds each break set of breaks.py, or the sets named, into a copy of the layer's sources, and of xsdk's for a set
# that breaks xsdk, against XSDK, the xsdk beside the layer by default, as v4x_m17b_<set>.xex in OUT, the layer's
# build/broken by default, which git ignores. The trees themselves are never changed. Build xsdk first; run it in WSL.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
XSDK=${XSDK:-$(cd "$REPO/../Xbox-360-SDK" && pwd)}
OUT=${OUT:-$REPO/build/broken}
SETS=${*:-$(python3 "$HERE/breaks.py" --list)}
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
for set in $SETS; do
	copy=$(mktemp -d)
	mkdir -p "$copy/vulkan4xenon" "$copy/Xbox-360-SDK"
	cp -r "$REPO"/{src,include,tests,tools,third_party,bench,Makefile} "$copy/vulkan4xenon"
	xsdk=$XSDK
	if python3 "$HERE/breaks.py" --xsdk "$set"; then
		# The library's build checks tests/doloop_check.c too (xsdk:Makefile).
		cp -r "$XSDK"/{src,include,imports,tools,tests,Makefile} "$copy/Xbox-360-SDK"
		xsdk=$copy/Xbox-360-SDK
	fi
	python3 "$HERE/breaks.py" "$set" "$copy/vulkan4xenon" "$copy/Xbox-360-SDK"
	log=$OUT/broken_$set.txt
	if { [ "$xsdk" = "$XSDK" ] || make -C "$xsdk" -j32 build/libxsdk.a; } >"$log" 2>&1 &&
		make -C "$copy/vulkan4xenon" -j32 XSDK="$xsdk" build/vulkan4xenon_test.xex >>"$log" 2>&1; then
		cp "$copy/vulkan4xenon/build/vulkan4xenon_test.xex" "$OUT/v4x_m17b_$set.xex"
	else
		grep -E "error:|Error|No rule" "$log"
	fi
	rm -rf "$copy"
done
ls -la "$OUT"/v4x_m17b_*.xex
