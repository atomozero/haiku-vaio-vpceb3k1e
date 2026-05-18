#!/bin/bash
# Ring Health Monitor — Intel Ironlake Gen5
# Diagnoses GPU ring buffer state and reports health status.
# Run after reboot or when GPU tests fail.

PROJECT="$HOME/Desktop/Sony Vaio VPCEB3K1E"
TESTS="$PROJECT/intel_extreme/accelerant/tests"

echo "=== Ring Health Report ==="
echo "Date: $(date '+%Y-%m-%d %H:%M:%S')"
echo ""

# Step 1: Check if device exists
if [ ! -e /dev/graphics/intel_extreme_000200 ]; then
    echo "Status: DRIVER NOT LOADED"
    echo "Device /dev/graphics/intel_extreme_000200 not found!"
    echo ""
    echo "Checking syslog..."
    grep -i "intel_extreme.*Could not load\|intel_extreme.*Symbol not\|loaded driver.*intel" \
        /boot/system/var/log/syslog 2>/dev/null | tail -5
    echo ""
    echo "Checking blacklist..."
    cat /boot/system/settings/packages 2>/dev/null | grep intel
    echo ""
    echo "Checking non-packaged driver..."
    ls -la /boot/system/non-packaged/add-ons/kernel/drivers/bin/intel_extreme 2>/dev/null
    ls -la /boot/system/non-packaged/add-ons/kernel/drivers/dev/graphics/intel_extreme 2>/dev/null
    echo ""
    echo "Action: Check syslog for symbol errors. Rebuild kernel driver if needed."
    exit 1
fi

# Step 2: Check syslog for errors
echo "Syslog GPU errors:"
ERRORS=$(grep -i "intel_extreme.*stall\|intel_extreme.*error\|intel_extreme.*RING\|Could not load.*intel" \
    /boot/system/var/log/syslog 2>/dev/null | grep -v "^KERN: intel_extreme: CALLED" | tail -5)
if [ -z "$ERRORS" ]; then
    echo "  (none)"
else
    echo "$ERRORS" | sed 's/^/  /'
fi
echo ""

# Step 3: Run test_ring_ioctl if available
if [ -x "$TESTS/test_ring_ioctl" ]; then
    echo "Running test_ring_ioctl..."
    OUTPUT=$("$TESTS/test_ring_ioctl" 2>&1)
    echo "$OUTPUT" | sed 's/^/  /'
    echo ""

    # Parse results
    HEAD_BEFORE=$(echo "$OUTPUT" | grep "Before reset:" | sed 's/.*HEAD=\(0x[0-9a-f]*\).*/\1/')
    TAIL_BEFORE=$(echo "$OUTPUT" | grep "Before reset:" | sed 's/.*TAIL=\(0x[0-9a-f]*\).*/\1/')
    CTL=$(echo "$OUTPUT" | grep "Before reset:" | sed 's/.*CTL=\(0x[0-9a-f]*\).*/\1/')

    if echo "$OUTPUT" | grep -q "GPU EXECUTED"; then
        echo "Status: RING HEALTHY"
        echo "HEAD: $HEAD_BEFORE  TAIL: $TAIL_BEFORE  CTL: $CTL"
        echo "Verdict: GPU ring is active and executing commands."
        echo "Action: All GPU tests should work."
    elif echo "$OUTPUT" | grep -q "Cannot open device"; then
        echo "Status: DRIVER NOT LOADED"
        echo "Verdict: Cannot open GPU device."
        echo "Action: Check kernel driver installation."
    elif echo "$OUTPUT" | grep -q "did NOT execute"; then
        HEAD_AFTER=$(echo "$OUTPUT" | grep "After TAIL kick:" | sed 's/.*HEAD=\(0x[0-9a-f]*\).*/\1/')
        TAIL_AFTER=$(echo "$OUTPUT" | grep "After TAIL kick:" | sed 's/.*TAIL=\(0x[0-9a-f]*\).*/\1/')

        if [ "$HEAD_AFTER" = "0x0" ] && echo "$OUTPUT" | grep -q "ioctl returned: 0"; then
            echo "Status: RING DEAD (RESET DAMAGE)"
            echo "HEAD: $HEAD_AFTER  TAIL: $TAIL_AFTER  CTL: $CTL"
            echo "Verdict: RING_RESET was called and killed the CS."
            echo "Action: REBOOT REQUIRED. After reboot, do NOT call RING_RESET."
        else
            echo "Status: RING DEAD"
            echo "HEAD: $HEAD_BEFORE  TAIL: $TAIL_BEFORE  CTL: $CTL"
            echo "Verdict: HEAD is stuck, GPU not processing ring commands."
            echo "Action: REBOOT REQUIRED."
        fi
    else
        echo "Status: UNKNOWN"
        echo "Verdict: Could not parse test output."
        echo "Action: Run test_ring_ioctl manually and inspect output."
    fi
else
    echo "test_ring_ioctl not found at $TESTS/test_ring_ioctl"
    echo "Status: UNTESTABLE"
    echo "Action: Rebuild test tools (cd accelerant/tests && make test tools)"
fi
