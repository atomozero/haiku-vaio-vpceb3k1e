#!/bin/sh
# Run hello world media test, then IDCT media test (if hello passes)
# Must run on fresh boot (ring dies after first media hang)

cd "$(dirname "$0")"

echo "=== Step 1: Hello World (no CURBE, no surfaces) ==="
./test_media_hello
rc=$?
if [ $rc -ne 0 ]; then
	echo "STOP: Hello world failed, ring dead."
	exit 1
fi

echo ""
echo "=== Step 2: MPEG-2 Viewer (IDCT kernel + CURBE + surfaces) ==="
cd ../../intel_extreme/mpeg2_plugin
timeout 8 ./mpeg2_viewer /boot/home/Desktop/sample_1280x720.m2v 2>&1 | head -20
