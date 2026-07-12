#!/bin/bash
# glconform.sh — differential GL conformance run for the crocus/GEM stack.
#
# Renders the glconform scene battery twice — hardware path (default
# renderer) and software reference (HGL_SOFTWARE=1, llvmpipe) — then
# compares the two sets pixel by pixel. llvmpipe acts as the conformance
# oracle: a match means the GPU renders correctly.
#
# Usage: tools/glconform.sh [result-base-dir]
set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
BASE="${1:-/tmp/glconform}"
BIN="$HERE/glconform"

if [ ! -x "$BIN" ] || [ "$HERE/glconform.cpp" -nt "$BIN" ]; then
	echo "building glconform..."
	g++ -Wall -O2 -o "$BIN" "$HERE/glconform.cpp" -lbe -lGL -lGLU
fi

rm -rf "$BASE/hw" "$BASE/sw"

echo "=== hardware pass (default renderer) ==="
"$BIN" --render "$BASE/hw"

echo "=== software reference pass (llvmpipe) ==="
HGL_SOFTWARE=1 "$BIN" --render "$BASE/sw"

echo
"$BIN" --compare "$BASE/hw" "$BASE/sw"
