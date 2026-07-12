#!/bin/bash
# gpu_multiclient_stress.sh — hammer the GEM stack with concurrent 2D and
# 3D clients to shake out contention that only shows up under load.
#
# With M4, the accelerant's 2D path (gem2d) and every crocus GL app are
# simultaneous GEM clients: they share the kernel ring, the GTT aperture
# + eviction, the fence registers, the seqno/fence engine and the
# interrupt handler. This runs several of each at once and watches for
# hangs, wedges, lost interrupts (dead vblank) and dead clients.
#
#   tools/gpu_multiclient_stress.sh [duration_seconds]   (default 90)
#
# WARNING: this currently EXPOSES a real GEM contention hang under
# extreme concurrent load (2026-07-12): the engine wedges (ILK reset is
# inert) and stays wedged until reboot. Treat it like intel_gem_hang —
# run it last and reboot afterwards. It is NOT part of gpu_stack_suite
# until the concurrency hang is fixed. See the memory note
# "MULTI-CLIENT STRESS FOUND A REAL HANG" for the signature + hypothesis.
#
# SAFETY: no intel_gem_hang, no legacy ring tools, no T7; GUI apps are
# quit with `hey ... quit`, never signals.

HERE="$(cd "$(dirname "$0")" && pwd)"
CW="${CROCUS_WINDOW:-$HOME/Desktop/m3-mesa/mesa-22.0.5/build-crocus/src/gallium/targets/haiku-softpipe/crocus_window}"
MG="${MINIMAL_GL:-$HOME/Desktop/m3-mesa/minimal_gl}"
BLIT="$HERE/blit2d_bench"
DURATION="${1:-90}"

PASS=0
FAIL=0

hangs() { /bin/grep -c "GPU hang\|wedg" /boot/system/var/log/syslog 2>/dev/null; }
alive() { ps -a 2>/dev/null | /bin/grep -q "$1"; }

HANGS0=$(hangs)
echo "multi-client GEM stress — ${DURATION}s — $(date)"
echo "start: hangs=$HANGS0, 2D=gem2d + 3D=crocus concurrent"

# --- launch the 3D fleet (each self-terminates via timeout) ---
timeout $((DURATION + 5)) "$CW" > /tmp/stress_cw1.log 2>&1 &
timeout $((DURATION + 5)) "$CW" > /tmp/stress_cw2.log 2>&1 &
/boot/system/demos/GLTeapot > /dev/null 2>&1 &
/boot/system/demos/Haiku3d > /dev/null 2>&1 &

# --- 3D: minimal_gl relaunched in a loop (present readback stress) ---
(
	END=$(($(date +%s 2>/dev/null || echo 0) + DURATION))
	while true; do
		timeout 12 "$MG" > /dev/null 2>&1
		ps -a 2>/dev/null | /bin/grep -q "gpu_multiclient" || break
		[ -f /tmp/stress_stop ] && break
	done
) &

# --- 2D: blit2d_bench relaunched in a loop (gem2d fill/blit stress) ---
rm -f /tmp/stress_stop
(
	while [ ! -f /tmp/stress_stop ]; do
		"$BLIT" > /tmp/stress_blit.log 2>&1
	done
) &
BLIT_LOOP=$!

# --- monitor loop: sample health while the fleet runs ---
BAD=0
SAMPLES=0
STEP=6
ELAPSED=0
while [ $ELAPSED -lt $DURATION ]; do
	sleep $STEP
	ELAPSED=$((ELAPSED + STEP))
	SAMPLES=$((SAMPLES + 1))

	H=$(hangs)
	if [ "$H" != "$HANGS0" ]; then
		echo "  t=${ELAPSED}s: HANG/WEDGE detected in syslog!"
		BAD=1
		break
	fi
	if ! "$HERE/irq_health" > /dev/null 2>&1; then
		echo "  t=${ELAPSED}s: interrupt registers show stuck bits (edge loss)!"
		BAD=1
		break
	fi
	printf "  t=%2ds: hangs=%s irq=ok\n" "$ELAPSED" "$H"
done

# --- stop the loops + GUI apps cleanly ---
touch /tmp/stress_stop
hey GLTeapot quit > /dev/null 2>&1
hey Haiku3d quit > /dev/null 2>&1
sleep 3
kill $BLIT_LOOP 2>/dev/null
rm -f /tmp/stress_stop

# --- verdict ---
echo
result() { if [ "$1" -eq 0 ]; then PASS=$((PASS+1)); echo "  [PASS] $2"; \
	else FAIL=$((FAIL+1)); echo "  [FAIL] $2"; fi; }

[ $BAD -eq 0 ]
result $? "no hang/wedge and no interrupt loss across ${SAMPLES} samples"

# vblank must still be delivered after the load
"$HERE/retrace_test" > /dev/null 2>&1
result $? "VBlank still delivered after the stress run"

# 2D still produces valid output (desktop composites through gem2d)
rm -f /tmp/stress_shot.png
screenshot -s /tmp/stress_shot.png 2>/dev/null
SZ=$(stat -c %s /tmp/stress_shot.png 2>/dev/null || echo 0)
[ "$SZ" -gt 20000 ]
result $? "desktop 2D output valid after stress (${SZ}-byte screenshot)"

# a fresh crocus client still renders correctly (3D path intact)
HGL_PIXCHECK=1 timeout 15 "$MG" 2>&1 | /bin/grep -q "nonblack=90601/90601"
result $? "fresh crocus client renders full frame after stress"

echo
echo "==================================================="
if [ $FAIL -eq 0 ]; then
	echo "MULTI-CLIENT STRESS PASSED ($PASS/$((PASS+FAIL))) — 2D+3D coexist under load."
else
	echo "STRESS FAILED ($FAIL failures) — see /tmp/stress_*.log"
fi
echo "==================================================="
exit $FAIL
