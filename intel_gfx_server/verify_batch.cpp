/*
 * verify_batch - Verify GTT entries and test batch buffer execution
 *
 * Strategy: use the RING BUFFER AREA (GTT offset 0, known valid)
 * as batch buffer, placing the batch at an unused offset within
 * the ring's 64KB space. If this works, the issue is GTT entries
 * for GEM buffers. If not, MI_BATCH_BUFFER_START itself is broken.
 */
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <OS.h>
#include "accelerant.h"
#include "intel_extreme.h"
#include "GemManager.h"

static int sFd;
static intel_shared_info* sSI;
static volatile uint8* sRegs;
static uint32 rr(uint32 o) { return *(volatile uint32*)(sRegs + o); }

int main() {
	DIR* dir = opendir("/dev/graphics");
	char path[256] = {};
	struct dirent* e;
	while ((e = readdir(dir)))
		if (strncmp(e->d_name, "intel_extreme", 13) == 0)
			{ snprintf(path, sizeof(path), "/dev/graphics/%s", e->d_name); break; }
	closedir(dir);
	sFd = open(path, B_READ_WRITE);
	intel_get_private_data data;
	data.magic = INTEL_PRIVATE_DATA_MAGIC;
	ioctl(sFd, INTEL_GET_PRIVATE_DATA, &data, sizeof(data));
	clone_area("s",(void**)&sSI,B_ANY_ADDRESS,B_READ_AREA|B_WRITE_AREA,data.shared_info_area);
	clone_area("r",(void**)&sRegs,B_ANY_ADDRESS,B_READ_AREA|B_WRITE_AREA,sSI->registers_area);

	ring_buffer& ring = sSI->primary_ring_buffer;
	uint32* rm = (uint32*)ring.base;
	uint32 fbOff = sSI->frame_buffer_offset;

	printf("=== Batch Buffer Verification ===\n\n");
	printf("Ring: HEAD=0x%x TAIL=0x%x START=0x%x\n",
		rr(0x2034), rr(0x2030), rr(0x2038));
	printf("ESR=0x%x\n", rr(0x20b8));

	// ---------------------------------------------------------------
	// Test 1: Place batch in ring buffer area (GTT offset 0, known valid)
	// Use offset 0x8000 (32KB into 64KB ring) - far from HEAD/TAIL
	// ---------------------------------------------------------------
	printf("\n--- Test 1: Batch in ring area (GTT 0x8000) ---\n");

	uint32 batchGttOff = 0x8000;  // 32KB into ring buffer
	uint32* batch = (uint32*)((uint8*)ring.base + batchGttOff);

	// Write batch: MI_STORE_DATA_IMM to fb[0]
	batch[0] = 0x10400002;   // MI_STORE_DATA_IMM | GGTT | len=2
	batch[1] = 0;
	batch[2] = fbOff;
	batch[3] = 0x11111111;
	batch[4] = MI_BATCH_BUFFER_END;
	batch[5] = 0x00000000;
	__asm__ __volatile__("mfence":::"memory");

	printf("  Batch at ring+0x8000, cmds: ");
	for (int i = 0; i < 6; i++) printf("0x%08x ", batch[i]);
	printf("\n");

	// Read fb[0] before
	uint32* fb0 = (uint32*)(sSI->graphics_memory + fbOff);
	__asm__ __volatile__("clflush (%0)"::"r"(fb0));
	__asm__ __volatile__("mfence":::"memory");
	printf("  fb[0] before: 0x%08x\n", *fb0);

	// Emit MI_BATCH_BUFFER_START into ring
	acquire_lock(&ring.lock);
	uint32 pos = ring.position / 4;
	rm[pos++] = 0x02000000;         // MI_FLUSH
	rm[pos++] = 0x00000000;         // MI_NOOP
	rm[pos++] = (0x31 << 23);       // MI_BATCH_BUFFER_START
	rm[pos++] = batchGttOff;        // GTT offset of batch
	rm[pos++] = 0x02000000;         // MI_FLUSH
	rm[pos++] = 0x00000000;         // MI_NOOP
	ring.position = pos * 4;
	ring.space_left -= 24;
	*(volatile uint32*)(sRegs + ring.register_base) = ring.position;
	release_lock(&ring.lock);

	snooze(100000);

	uint32 head = rr(0x2034) & INTEL_RING_BUFFER_HEAD_MASK;
	uint32 esr = rr(0x20b8);
	printf("  HEAD=0x%x TAIL=0x%x ESR=0x%x\n", head, rr(0x2030), esr);

	__asm__ __volatile__("mfence":::"memory");
	__asm__ __volatile__("clflush (%0)"::"r"(fb0));
	__asm__ __volatile__("mfence":::"memory");
	printf("  fb[0] after: 0x%08x %s\n", *fb0,
		*fb0 == 0x11111111 ? "BATCH OK!" : "not written");

	bool test1ok = (*fb0 == 0x11111111);

	if (!test1ok) {
		printf("  IPEHR=0x%08x ACTHD=0x%08x\n", rr(0x2068), rr(0x2074));
		printf("  [FAIL] MI_BATCH_BUFFER_START doesn't work at all\n");

		// Check if HEAD advanced past the command
		if (head == ring.position)
			printf("  HEAD reached TAIL (cmd consumed but no effect)\n");
		else
			printf("  HEAD stuck (GPU hang on batch start)\n");
		return 1;
	}

	// ---------------------------------------------------------------
	// Test 2: Batch in GEM buffer (to test GTT entries)
	// ---------------------------------------------------------------
	printf("\n--- Test 2: Batch in GEM buffer ---\n");
	GemManager gem;
	gem.Init(sSI, sRegs, sFd);

	uint32 gemHandle = 0;
	gem.CreateBuffer(4096, &gemHandle);
	uint32 gemOff = gem.GetOffset(gemHandle);
	uint32* gemBatch = (uint32*)gem.MapBuffer(gemHandle);

	printf("  GEM: handle=%u offset=0x%x\n", gemHandle, gemOff);

	// Write batch
	gemBatch[0] = 0x10400002;
	gemBatch[1] = 0;
	gemBatch[2] = fbOff;
	gemBatch[3] = 0x22222222;
	gemBatch[4] = MI_BATCH_BUFFER_END;
	gemBatch[5] = 0x00000000;
	__asm__ __volatile__("mfence":::"memory");

	// Submit
	acquire_lock(&ring.lock);
	pos = ring.position / 4;
	rm[pos++] = 0x02000000;
	rm[pos++] = 0x00000000;
	rm[pos++] = (0x31 << 23);
	rm[pos++] = gemOff;
	rm[pos++] = 0x02000000;
	rm[pos++] = 0x00000000;
	ring.position = pos * 4;
	ring.space_left -= 24;
	*(volatile uint32*)(sRegs + ring.register_base) = ring.position;
	release_lock(&ring.lock);

	snooze(100000);
	head = rr(0x2034) & INTEL_RING_BUFFER_HEAD_MASK;
	esr = rr(0x20b8);
	__asm__ __volatile__("mfence":::"memory");
	__asm__ __volatile__("clflush (%0)"::"r"(fb0));
	__asm__ __volatile__("mfence":::"memory");
	printf("  HEAD=0x%x ESR=0x%x\n", head, esr);
	printf("  fb[0] after: 0x%08x %s\n", *fb0,
		*fb0 == 0x22222222 ? "GEM BATCH OK!" : "not written");

	if (*fb0 != 0x22222222) {
		printf("  IPEHR=0x%08x ACTHD=0x%08x\n", rr(0x2068), rr(0x2074));
		printf("  → GTT entries for GEM buffer NOT valid for GPU reads\n");
	}

	// ---------------------------------------------------------------
	// Test 3: BLT in batch (if test 1 worked)
	// ---------------------------------------------------------------
	if (test1ok && esr == 0) {
		printf("\n--- Test 3: BLT in batch ---\n");
		uint32 bpr = sSI->bytes_per_row;
		uint32* target = batch;  // use ring area if GEM failed
		uint32 targetOff = batchGttOff;
		if (*fb0 == 0x22222222) {
			target = gemBatch;
			targetOff = gemOff;
		}

		target[0] = 0x54300004;  // XY_COLOR_BLT | RGBA
		target[1] = (0x03 << 24) | (0xF0 << 16) | (bpr & 0xFFFF);
		target[2] = (500 << 16) | 500;
		target[3] = (520 << 16) | 520;
		target[4] = fbOff;
		target[5] = 0xFF0000FF;  // blue
		target[6] = MI_BATCH_BUFFER_END;
		target[7] = 0x00000000;
		__asm__ __volatile__("mfence":::"memory");

		acquire_lock(&ring.lock);
		pos = ring.position / 4;
		rm[pos++] = 0x02000000;
		rm[pos++] = 0x00000000;
		rm[pos++] = (0x31 << 23);
		rm[pos++] = targetOff;
		rm[pos++] = 0x02000000;
		rm[pos++] = 0x00000000;
		ring.position = pos * 4;
		ring.space_left -= 24;
		*(volatile uint32*)(sRegs + ring.register_base) = ring.position;
		release_lock(&ring.lock);

		snooze(100000);
		head = rr(0x2034) & INTEL_RING_BUFFER_HEAD_MASK;
		uint32* px500 = (uint32*)(sSI->graphics_memory + fbOff + 500*bpr + 500*4);
		__asm__ __volatile__("mfence":::"memory");
		__asm__ __volatile__("clflush (%0)"::"r"(px500));
		__asm__ __volatile__("mfence":::"memory");
		printf("  HEAD=0x%x ESR=0x%x\n", head, rr(0x20b8));
		printf("  fb[500,500] = 0x%08x %s\n", *px500,
			*px500 == 0xFF0000FF ? "BLUE! Batch BLT works!" : "not written");
	}

	gem.CloseBuffer(gemHandle);
	printf("\n=== Done ===\n");
	return 0;
}
