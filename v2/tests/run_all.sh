#!/bin/sh
#
# Intel Gen GPU Driver — Test Suite Runner
#
# Runs all test phases in order. Each test is independent and
# reports PASS/FAIL. Stop on first failure with -x flag.
#

SCRIPT_DIR="$(dirname "$0")"
PASS=0
FAIL=0
SKIP=0
STOP_ON_FAIL=false

if [ "$1" = "-x" ]; then
	STOP_ON_FAIL=true
fi

run_test() {
	name="$1"
	binary="$2"

	if [ ! -x "$SCRIPT_DIR/$binary" ]; then
		echo "  SKIP  $name ($binary not built)"
		SKIP=$((SKIP + 1))
		return
	fi

	echo -n "  RUN   $name ... "
	output=$("$SCRIPT_DIR/$binary" 2>&1)
	rc=$?

	if [ $rc -eq 0 ]; then
		echo "PASS"
		PASS=$((PASS + 1))
	else
		echo "FAIL (exit $rc)"
		echo "$output" | tail -5 | sed 's/^/        /'
		FAIL=$((FAIL + 1))
		if $STOP_ON_FAIL; then
			echo ""
			echo "Stopped on first failure (-x)."
			exit 1
		fi
	fi
}

echo "=== Intel Gen GPU Driver — Test Suite ==="
echo ""

# Phase 0: Ring basics
echo "--- Phase 0: Ring & Basics ---"
run_test "Ring submission (MI_NOOP)"      test_ring
run_test "GEM create/close"               test_gem
run_test "Execbuffer2 basic"              test_execbuffer

# Phase 1: Media pipeline
echo "--- Phase 1: Media Pipeline ---"
run_test "Media pipeline dispatch"        test_media
run_test "BLT engine copy"               test_blt

# Phase 2: Display
echo "--- Phase 2: Display ---"
run_test "Generation detection"           test_gen_detect
run_test "Display mode query"             test_display

# Phase 3: Stress & recovery
echo "--- Phase 3: Stress & Recovery ---"
run_test "Ring stress (1000 submits)"     test_ring_stress
run_test "GPU reset recovery"             test_reset

echo ""
echo "=== Results: $PASS passed, $FAIL failed, $SKIP skipped ==="

if [ $FAIL -gt 0 ]; then
	exit 1
fi
exit 0
