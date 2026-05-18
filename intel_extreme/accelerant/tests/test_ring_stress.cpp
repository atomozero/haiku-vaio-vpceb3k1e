/*
 * test_ring_stress.cpp -- Comprehensive ring buffer stress test for Gen5.
 *
 * Tests that the ring works without reset (approach B), covering:
 *   1. Baseline: ring alive after boot (MI_NOOP, HEAD chases TAIL)
 *   2. Sequential: submit, wait idle, submit again (10 rounds)
 *   3. Ring wrap: fill near capacity, verify wrap-around works
 *   4. Interleaved tools: MI_STORE_DATA_IMM marker, then BLT, then marker
 *   5. (reserved for hang recovery -- not applicable without reset)
 *   6. Stress: 1000 back-to-back MI_NOOP+MI_FLUSH submissions
 *
 * Build (from intel_extreme/accelerant):
 *   make
 *   cd tests
 *   g++ -Wall -O2 -I.. \
 *       -I/boot/system/develop/headers/os/add-ons/graphics \
 *       -I/boot/system/develop/headers/os/drivers \
 *       -I/boot/system/develop/headers/private/graphics \
 *       -I/boot/system/develop/headers/private/graphics/intel_extreme \
 *       -I/boot/system/develop/headers/private/graphics/common \
 *       -I/boot/system/develop/headers/private/shared \
 *       -I/boot/system/develop/headers/private/system \
 *       -I/boot/system/develop/headers/private/kernel/boot \
 *       -I/boot/system/develop/headers/os \
 *       -I/boot/system/develop/headers/os/support \
 *       -I/boot/system/develop/headers/os/interface \
 *       -I/boot/system/develop/headers/os/kernel \
 *       -I/boot/system/develop/headers/os/storage \
 *       -I/boot/system/develop/headers/os/app \
 *       -I/boot/system/develop/headers/posix \
 *       -o test_ring_stress test_ring_stress.cpp \
 *       ../gpu_ring.o -lbe -lstdc++
 *
 * Run: ./test_ring_stress
 * No reboot needed. Safe to run multiple times.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <OS.h>

#include "intel_extreme.h"
#include "lock.h"
#include "accelerant.h"
#include "gpu_ring.h"


// Global required by accelerant headers
accelerant_info* gInfo;

// MI command encodings
#define MI_NOOP_CMD				0x00000000
#define MI_FLUSH_CMD			(0x04 << 23)
// MI_STORE_DATA_IMM (GGTT): opcode 0x20, bit 22 = use GGTT, DW count = 2
#define MI_STORE_DATA_IMM_GGTT	((0x20u << 23) | (1u << 22) | 2u)


static int sPass = 0;
static int sFail = 0;


static void
check(bool ok, const char* name, const char* detail = NULL)
{
	if (ok) {
		sPass++;
		printf("  PASS: %s", name);
	} else {
		sFail++;
		printf("  FAIL: %s", name);
	}
	if (detail)
		printf(" (%s)", detail);
	printf("\n");
}


// Detect dead ring: HEAD stuck, does not chase TAIL after submission.
static bool
ring_is_alive(gpu_ring* ring)
{
	uint32 head_before = gpu_ring_read_head(ring);

	// Submit 2 MI_NOOPs
	uint32 cmds[2] = { MI_NOOP_CMD, MI_NOOP_CMD };
	gpu_ring_submit(ring, cmds, 2);

	// Wait up to 100ms for HEAD to advance
	bigtime_t deadline = system_time() + 100000;
	while (system_time() < deadline) {
		uint32 head_after = gpu_ring_read_head(ring);
		if (head_after != head_before)
			return true;
		snooze(100);
	}
	return false;
}


// Dump ring diagnostic state
static void
dump_ring_state(gpu_ring* ring, const char* label)
{
	uint32 head = gpu_ring_read_head(ring);
	printf("  [%s] HEAD=0x%04x pos=0x%04x size=0x%x\n",
		label, head, ring->pos, ring->size);
}


// ==================================================================
// Test 1: Baseline -- ring alive after boot
// ==================================================================
static void
test_baseline(gpu_ring* ring)
{
	printf("\n--- Test 1: Baseline (ring alive after boot) ---\n");
	dump_ring_state(ring, "before");

	bool alive = ring_is_alive(ring);
	check(alive, "ring is alive (HEAD advances after MI_NOOP)");

	if (alive) {
		bool idle = gpu_ring_wait_idle(ring, 50000);
		check(idle, "ring reaches idle (HEAD == TAIL)");
	}
	dump_ring_state(ring, "after");
}


// ==================================================================
// Test 2: Sequential submissions -- submit, wait, submit again
// ==================================================================
static void
test_sequential(gpu_ring* ring)
{
	printf("\n--- Test 2: Sequential (10 rounds of submit+wait) ---\n");
	dump_ring_state(ring, "before");

	bool all_ok = true;
	for (int i = 0; i < 10; i++) {
		// Submit MI_FLUSH + MI_NOOP (2 DWORDs, QWord aligned)
		uint32 cmds[2] = { MI_FLUSH_CMD, MI_NOOP_CMD };
		status_t st = gpu_ring_submit(ring, cmds, 2);
		if (st != B_OK) {
			printf("  round %d: submit failed (status %d)\n", i, (int)st);
			all_ok = false;
			break;
		}

		bool idle = gpu_ring_wait_idle(ring, 100000);
		if (!idle) {
			printf("  round %d: TIMEOUT -- HEAD stuck\n", i);
			dump_ring_state(ring, "stuck");
			all_ok = false;
			break;
		}
	}
	check(all_ok, "10 sequential submit+wait rounds completed");
	dump_ring_state(ring, "after");
}


// ==================================================================
// Test 3: Ring wrap -- fill until position wraps around
// ==================================================================
static void
test_ring_wrap(gpu_ring* ring)
{
	printf("\n--- Test 3: Ring wrap (fill near capacity) ---\n");
	dump_ring_state(ring, "before");

	// Each submission is 2 DWORDs = 8 bytes. We submit enough to
	// exceed ring size and force at least one wrap. Ring size is
	// typically 16KB-128KB.
	uint32 submissions_needed = (ring->size / 8) + 16;
	bool wrap_seen = false;
	uint32 prev_pos = ring->pos;
	bool all_ok = true;

	for (uint32 i = 0; i < submissions_needed; i++) {
		uint32 cmds[2] = { MI_NOOP_CMD, MI_NOOP_CMD };
		status_t st = gpu_ring_submit(ring, cmds, 2);
		if (st != B_OK) {
			printf("  submission %u: failed (status %d)\n", i, (int)st);
			all_ok = false;
			break;
		}

		if (ring->pos < prev_pos)
			wrap_seen = true;
		prev_pos = ring->pos;

		// Every 256 submissions, wait for GPU to drain so we don't
		// overrun the ring (gpu_ring_begin handles this, but be safe)
		if ((i & 0xFF) == 0xFF) {
			if (!gpu_ring_wait_idle(ring, 200000)) {
				printf("  drain timeout at submission %u\n", i);
				dump_ring_state(ring, "drain");
				all_ok = false;
				break;
			}
		}
	}

	// Final drain
	if (all_ok) {
		bool idle = gpu_ring_wait_idle(ring, 500000);
		if (!idle) {
			printf("  final drain TIMEOUT\n");
			dump_ring_state(ring, "final");
			all_ok = false;
		}
	}

	check(all_ok, "all wrap submissions completed");
	check(wrap_seen, "ring position wrapped around at least once");
	dump_ring_state(ring, "after");
}


// ==================================================================
// Test 4: Interleaved tools -- marker write, then BLT, then marker
//
// Uses MI_STORE_DATA_IMM to write a known tag to a GTT address,
// verifying both the MI engine and the command streamer pipeline.
// We do NOT issue an actual BLT (no render buffer allocated), but
// we submit MI_FLUSH between markers to exercise pipeline drains.
// ==================================================================
static void
test_interleaved(gpu_ring* ring)
{
	printf("\n--- Test 4: Interleaved (marker + flush + marker) ---\n");
	dump_ring_state(ring, "before");

	// We cannot easily allocate a GTT buffer from userspace for the
	// marker write destination, so we test the command sequence with
	// MI_FLUSH (simulates a pipeline drain between different command
	// types) and verify HEAD advances correctly.

	// Sequence: MI_FLUSH, MI_NOOP, MI_FLUSH, MI_NOOP (simulates
	// tool-A drain, tool-B drain)
	bool all_ok = true;
	for (int round = 0; round < 5; round++) {
		// "Tool A" -- flush
		gpu_ring_begin(ring, 2);
		gpu_ring_emit(ring, MI_FLUSH_CMD);
		gpu_ring_emit(ring, MI_NOOP_CMD);
		gpu_ring_advance(ring);

		if (!gpu_ring_wait_idle(ring, 100000)) {
			printf("  round %d tool-A: TIMEOUT\n", round);
			all_ok = false;
			break;
		}

		// "Tool B" -- different flush pattern
		gpu_ring_begin(ring, 4);
		gpu_ring_emit(ring, MI_FLUSH_CMD);
		gpu_ring_emit(ring, MI_NOOP_CMD);
		gpu_ring_emit(ring, MI_NOOP_CMD);
		gpu_ring_emit(ring, MI_NOOP_CMD);
		gpu_ring_advance(ring);

		if (!gpu_ring_wait_idle(ring, 100000)) {
			printf("  round %d tool-B: TIMEOUT\n", round);
			all_ok = false;
			break;
		}
	}

	check(all_ok, "5 rounds of interleaved tool-A/tool-B");
	dump_ring_state(ring, "after");
}


// ==================================================================
// Test 6: Stress -- 1000 back-to-back submissions
// ==================================================================
static void
test_stress(gpu_ring* ring)
{
	printf("\n--- Test 6: Stress (1000 submissions) ---\n");
	dump_ring_state(ring, "before");

	const uint32 kIterations = 1000;
	uint32 completed = 0;
	bool hung = false;
	bigtime_t t0 = system_time();

	for (uint32 i = 0; i < kIterations; i++) {
		// Alternate between MI_NOOP pairs and MI_FLUSH+NOOP to
		// exercise different command parser paths.
		uint32 cmds[2];
		if (i & 1) {
			cmds[0] = MI_FLUSH_CMD;
			cmds[1] = MI_NOOP_CMD;
		} else {
			cmds[0] = MI_NOOP_CMD;
			cmds[1] = MI_NOOP_CMD;
		}

		status_t st = gpu_ring_submit(ring, cmds, 2);
		if (st != B_OK) {
			printf("  submit failed at iteration %u (status %d)\n",
				i, (int)st);
			hung = true;
			break;
		}
		completed++;

		// Drain every 64 submissions to prevent ring overflow
		if ((i & 0x3F) == 0x3F) {
			if (!gpu_ring_wait_idle(ring, 200000)) {
				printf("  GPU hung at iteration %u -- HEAD stuck\n", i);
				dump_ring_state(ring, "hung");
				hung = true;
				break;
			}
		}
	}

	// Final drain
	if (!hung) {
		if (!gpu_ring_wait_idle(ring, 500000)) {
			printf("  final drain TIMEOUT\n");
			dump_ring_state(ring, "timeout");
			hung = true;
		}
	}

	bigtime_t elapsed = system_time() - t0;

	check(!hung, "no GPU hang during 1000 submissions");
	check(completed == kIterations,
		"all 1000 submissions accepted by ring API");

	printf("  completed: %u/%u in %.1f ms (%.0f submissions/sec)\n",
		completed, kIterations, elapsed / 1000.0,
		completed * 1e6 / (double)elapsed);

	// Verify ring is still alive after stress
	bool alive = ring_is_alive(ring);
	check(alive, "ring still alive after stress test");

	dump_ring_state(ring, "after");
}


// ==================================================================
// Main
// ==================================================================
int
main(int, char**)
{
	printf("=== Ring Buffer Stress Test (Gen5 Ironlake) ===\n");
	printf("Tests ring operation WITHOUT reset (approach B).\n");
	printf("Safe to run multiple times without reboot.\n\n");

	// Open device
	int fd = open("/dev/graphics/intel_extreme_000200", B_READ_WRITE);
	if (fd < 0) {
		printf("Cannot open /dev/graphics/intel_extreme_000200\n");
		return 1;
	}

	// Initialize gpu_ring (syncs with hardware, no reset)
	gpu_ring ring;
	status_t st = gpu_ring_init(&ring, fd);
	if (st != B_OK) {
		printf("gpu_ring_init failed: %d\n", (int)st);
		close(fd);
		return 1;
	}

	printf("Ring initialized: size=0x%x, pos=0x%x, HEAD=0x%x\n",
		ring.size, ring.pos, gpu_ring_read_head(&ring));

	// Run all tests in order
	test_baseline(&ring);
	test_sequential(&ring);
	test_ring_wrap(&ring);
	test_interleaved(&ring);
	// Test 5 (hang recovery) is N/A -- we never reset the ring.
	test_stress(&ring);

	// Summary
	printf("\n=== RESULTS: %d passed, %d failed ===\n", sPass, sFail);
	if (sFail == 0)
		printf("ALL TESTS PASSED -- ring works without reset.\n");
	else
		printf("FAILURES DETECTED -- check output above.\n");

	close(fd);
	return sFail > 0 ? 1 : 0;
}
