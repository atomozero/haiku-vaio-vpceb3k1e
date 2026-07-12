#!/bin/bash
# blit2d_ab.sh — A/B the two accelerant 2D submission paths in one boot.
#
# Runs blit2d_bench once on the GEM execbuffer path (M4 default) and once
# on the legacy ring path (via the intel_2d_legacy toggle), then prints
# both results side by side. Requires the toggle-capable accelerant
# (loaded after a reboot following its install).
set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
BENCH="$HERE/blit2d_bench"
TOGGLE=/boot/home/config/settings/intel_2d_legacy

[ -x "$BENCH" ] || g++ -Wall -O2 -o "$BENCH" "$HERE/blit2d_bench.cpp" -lbe

rm -f "$TOGGLE"			# ensure we start on the GEM path
echo "=== Pass A: GEM execbuffer path (M4 default) ==="
A=$("$BENCH")
echo "$A"
echo

echo "=== Pass B: legacy ring path (toggle on) ==="
touch "$TOGGLE"
sleep 1				# let the 200ms toggle cache refresh
B=$("$BENCH")
echo "$B"
rm -f "$TOGGLE"			# restore the default GEM path
echo

echo "=== Comparison (GEM vs legacy ring) ==="
paste <(echo "$A") <(echo "$B") | awk -F'\t' '
	/throughput|latency/ {
		# extract the numeric column from each side
		ga = $1; gb = $2;
		na = ga; gsub(/[^0-9.]/, " ", na); split(na, xa, " ");
		nb = gb; gsub(/[^0-9.]/, " ", nb); split(nb, xb, " ");
		va = xa[1]; vb = xb[1];
		# label = text before the colon
		lab = $1; sub(/:.*/, "", lab); gsub(/^ +| +$/, "", lab);
		if (va > 0 && vb > 0)
			printf "  %-22s GEM %12.1f   ring %12.1f   ratio %.2fx\n",
				lab, va, vb, va / vb;
	}'
echo
echo "(throughput ratio >1 = GEM faster; latency ratio <1 = GEM lower latency)"
