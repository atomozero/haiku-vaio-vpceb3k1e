#!/bin/bash
# GPU Test & Regression Runner — Intel Ironlake Gen5
# Builds and runs the complete GPU test suite, reports pass/fail vs baselines.

PROJECT="$HOME/Desktop/Sony Vaio VPCEB3K1E"
ACCEL="$PROJECT/intel_extreme/accelerant"
TESTS="$ACCEL/tests"
DRM="$PROJECT/intel_extreme/drm_shim"

# Include flags for building test tools
IFLAGS="-I$ACCEL \
    -I/boot/system/develop/headers/os/add-ons/graphics \
    -I/boot/system/develop/headers/os/drivers \
    -I/boot/system/develop/headers/private/graphics \
    -I/boot/system/develop/headers/private/graphics/intel_extreme \
    -I/boot/system/develop/headers/private/graphics/common \
    -I/boot/system/develop/headers/private/shared \
    -I/boot/system/develop/headers/private/system \
    -I/boot/system/develop/headers/os \
    -I/boot/system/develop/headers/os/support \
    -I/boot/system/develop/headers/os/interface \
    -I/boot/system/develop/headers/os/kernel \
    -I/boot/system/develop/headers/os/storage \
    -I/boot/system/develop/headers/os/app \
    -I/boot/system/develop/headers/posix"

PASS=0
FAIL=0
SKIP=0
CRITICAL_FAIL=0
RESULTS=""

result() {
    local name="$1" status="$2" detail="$3" critical="$4"
    RESULTS="$RESULTS\n| $name | $status | $detail |"
    if [ "$status" = "PASS" ]; then
        PASS=$((PASS + 1))
    elif [ "$status" = "FAIL" ]; then
        FAIL=$((FAIL + 1))
        [ "$critical" = "yes" ] && CRITICAL_FAIL=$((CRITICAL_FAIL + 1))
    else
        SKIP=$((SKIP + 1))
    fi
}

echo "=== GPU Test Suite ==="
echo "Date: $(date '+%Y-%m-%d %H:%M:%S')"
echo ""

# ---- TEST 1: Build accelerant ----
echo "[1/6] Building accelerant..."
cd "$ACCEL"
BUILD_OUT=$(make 2>&1)
BUILD_RC=$?
if [ $BUILD_RC -eq 0 ]; then
    result "Build" "PASS" "accelerant compiled OK" "yes"
    echo "  PASS"
else
    result "Build" "FAIL" "make failed (exit $BUILD_RC)" "yes"
    echo "  FAIL: $BUILD_OUT" | tail -3
    echo ""
    echo "CRITICAL: Build failed. Cannot continue."
    CRITICAL_FAIL=1
fi

# ---- TEST 2: Ring ioctl ----
echo "[2/6] Ring ioctl test..."
if [ $CRITICAL_FAIL -gt 0 ]; then
    result "Ring ioctl" "SKIP" "build failed" ""
    echo "  SKIP (build failed)"
elif [ ! -x "$TESTS/test_ring_ioctl" ]; then
    # Rebuild it
    cd "$TESTS"
    g++ -Wall -O2 $IFLAGS -o test_ring_ioctl test_ring_ioctl.cpp \
        $ACCEL/*.o $ACCEL/../libaccelerantscommon.a -lbe -lstdc++ 2>/dev/null
fi

if [ -x "$TESTS/test_ring_ioctl" ] && [ $CRITICAL_FAIL -eq 0 ]; then
    RING_OUT=$("$TESTS/test_ring_ioctl" 2>&1)
    if echo "$RING_OUT" | grep -q "GPU EXECUTED"; then
        result "Ring ioctl" "PASS" "HEAD advances after TAIL kick" "yes"
        echo "  PASS: GPU executes commands"
    elif echo "$RING_OUT" | grep -q "Cannot open"; then
        result "Ring ioctl" "FAIL" "device not found" "yes"
        echo "  FAIL: Cannot open device"
        CRITICAL_FAIL=1
    else
        result "Ring ioctl" "FAIL" "HEAD stuck" "yes"
        echo "  FAIL: GPU not executing"
        echo "$RING_OUT" | grep -E "HEAD|TAIL|EXECUTED|NOT" | sed 's/^/    /'
        CRITICAL_FAIL=1
    fi
fi

# ---- TEST 3: IDCT benchmark ----
echo "[3/6] IDCT benchmark..."
if [ $CRITICAL_FAIL -gt 0 ]; then
    result "IDCT bench" "SKIP" "ring not working" ""
    echo "  SKIP (ring not working)"
else
    # Rebuild if needed
    if [ ! -x "$TESTS/gpu_idct_bench" ] || [ "$ACCEL/media_pipeline.o" -nt "$TESTS/gpu_idct_bench" ]; then
        cd "$TESTS"
        g++ -Wall -O2 $IFLAGS -o gpu_idct_bench gpu_idct_bench.cpp \
            $ACCEL/*.o $ACCEL/../libaccelerantscommon.a -lbe -lstdc++ 2>/dev/null
    fi

    if [ -x "$TESTS/gpu_idct_bench" ]; then
        IDCT_OUT=$("$TESTS/gpu_idct_bench" 2>&1)
        SPEEDUP=$(echo "$IDCT_OUT" | grep "Speedup:" | sed 's/.*Speedup: \([0-9.]*\)x.*/\1/')
        GPU_US=$(echo "$IDCT_OUT" | grep "GPU:" | sed 's/.*GPU: \([0-9]*\) us.*/\1/')
        CPU_US=$(echo "$IDCT_OUT" | grep "CPU:" | sed 's/.*CPU: \([0-9]*\) us.*/\1/')

        if [ -n "$SPEEDUP" ]; then
            # Check speedup >= 2.0
            OK=$(echo "$SPEEDUP" | awk '{print ($1 >= 2.0) ? "yes" : "no"}')
            if [ "$OK" = "yes" ]; then
                result "IDCT bench" "PASS" "GPU=${GPU_US}us CPU=${CPU_US}us ${SPEEDUP}x" "yes"
                echo "  PASS: GPU ${GPU_US}us, CPU ${CPU_US}us, ${SPEEDUP}x speedup"
            else
                result "IDCT bench" "FAIL" "speedup ${SPEEDUP}x < 2.0x baseline" "yes"
                echo "  FAIL: Speedup only ${SPEEDUP}x (expected >= 2.0x)"
            fi
        elif echo "$IDCT_OUT" | grep -q "TIMEOUT\|TIMED_OUT"; then
            result "IDCT bench" "FAIL" "GPU timeout — ring dead?" "yes"
            echo "  FAIL: GPU timeout"
        else
            result "IDCT bench" "FAIL" "unexpected output" "yes"
            echo "  FAIL: $IDCT_OUT" | tail -3
        fi
    else
        result "IDCT bench" "SKIP" "binary not found" ""
        echo "  SKIP: gpu_idct_bench not built"
    fi
fi

# ---- TEST 4: 3D cube ----
echo "[4/6] 3D cube (5 seconds)..."
if [ $CRITICAL_FAIL -gt 0 ]; then
    result "3D cube" "SKIP" "ring not working" ""
    echo "  SKIP"
elif [ -x "$TESTS/gpu_triangle" ]; then
    CUBE_OUT=$(timeout 5 "$TESTS/gpu_triangle" 2>&1)
    FPS=$(echo "$CUBE_OUT" | grep "FPS$" | head -1 | sed 's/.*: \([0-9.]*\) FPS/\1/')
    if [ -n "$FPS" ]; then
        OK=$(echo "$FPS" | awk '{print ($1 >= 30.0) ? "yes" : "no"}')
        if [ "$OK" = "yes" ]; then
            result "3D cube" "PASS" "${FPS} FPS" ""
            echo "  PASS: ${FPS} FPS"
        else
            result "3D cube" "FAIL" "${FPS} FPS < 30 baseline" ""
            echo "  FAIL: ${FPS} FPS (expected >= 30)"
        fi
    else
        result "3D cube" "FAIL" "no FPS output" ""
        echo "  FAIL: no FPS output"
        echo "$CUBE_OUT" | tail -3 | sed 's/^/    /'
    fi
else
    result "3D cube" "SKIP" "binary not found" ""
    echo "  SKIP"
fi

# ---- TEST 5: DRM shim ----
echo "[5/6] DRM shim..."
if [ -x "$DRM/test_drm_shim" ]; then
    DRM_OUT=$("$DRM/test_drm_shim" 2>&1)
    if echo "$DRM_OUT" | grep -q "All tests passed\|PASS"; then
        result "DRM shim" "PASS" "GEM ops OK" ""
        echo "  PASS"
    else
        result "DRM shim" "FAIL" "$(echo "$DRM_OUT" | tail -1)" ""
        echo "  FAIL"
    fi
else
    result "DRM shim" "SKIP" "binary not found" ""
    echo "  SKIP"
fi

# ---- TEST 6: EXECBUFFER2 ----
echo "[6/6] EXECBUFFER2..."
if [ $CRITICAL_FAIL -gt 0 ]; then
    result "EXECBUFFER2" "SKIP" "ring not working" ""
    echo "  SKIP"
elif [ -x "$DRM/test_execbuf" ]; then
    EXEC_OUT=$("$DRM/test_execbuf" 2>&1)
    if echo "$EXEC_OUT" | grep -q "GPU WROTE CORRECTLY"; then
        result "EXECBUFFER2" "PASS" "batch executed, GPU wrote target BO" ""
        echo "  PASS: GPU wrote correctly"
    elif echo "$EXEC_OUT" | grep -q "TIMEOUT"; then
        result "EXECBUFFER2" "FAIL" "timeout — batch or marker hang" ""
        echo "  FAIL: timeout"
        echo "$EXEC_OUT" | grep "TIMEOUT\|HEAD\|TAIL" | sed 's/^/    /'
    else
        result "EXECBUFFER2" "FAIL" "$(echo "$EXEC_OUT" | tail -1)" ""
        echo "  FAIL"
    fi
else
    result "EXECBUFFER2" "SKIP" "binary not found" ""
    echo "  SKIP"
fi

# ---- Summary ----
echo ""
echo "=== Summary ==="
echo -e "| Test | Result | Detail |"
echo -e "|----|--------|--------|"
echo -e "$RESULTS"
echo ""
echo "Passed: $PASS  Failed: $FAIL  Skipped: $SKIP"
if [ $CRITICAL_FAIL -gt 0 ]; then
    echo "BLOCKERS: $CRITICAL_FAIL critical failure(s) — fix before proceeding"
elif [ $FAIL -gt 0 ]; then
    echo "REGRESSIONS: $FAIL non-critical failure(s)"
else
    echo "ALL CLEAR: No regressions detected"
fi
