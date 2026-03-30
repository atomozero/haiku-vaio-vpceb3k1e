/*
 * batch_test - Test MI_BATCH_BUFFER_START on Gen5
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
	clone_area("s", (void**)&sSI, B_ANY_ADDRESS, B_READ_AREA|B_WRITE_AREA, data.shared_info_area);
	clone_area("r", (void**)&sRegs, B_ANY_ADDRESS, B_READ_AREA|B_WRITE_AREA, sSI->registers_area);

	ring_buffer& ring = sSI->primary_ring_buffer;
	uint32* rm = (uint32*)ring.base;
	uint32 fbOff = sSI->frame_buffer_offset;
	uint32 bpr = sSI->bytes_per_row;

	printf("=== MI_BATCH_BUFFER_START Test ===\n\n");

	// Allocate a GEM buffer for the batch
	GemManager gem;
	gem.Init(sSI, sRegs, sFd);

	uint32 batchHandle = 0;
	gem.CreateBuffer(4096, &batchHandle);
	uint32 batchOff = gem.GetOffset(batchHandle);
	uint32* batch = (uint32*)gem.MapBuffer(batchHandle);

	printf("Batch: handle=%u offset=0x%x ptr=%p\n", batchHandle, batchOff, batch);

	// Test 1: Simple MI_STORE_DATA_IMM in batch buffer
	printf("\n--- Test 1: Batch with MI_STORE_DATA_IMM ---\n");

	// Write a simple batch: MI_STORE_DATA_IMM + MI_BATCH_BUFFER_END
	batch[0] = 0x10400002;   // MI_STORE_DATA_IMM | GGTT | len=2
	batch[1] = 0;            // reserved
	batch[2] = fbOff;        // write to first fb pixel
	batch[3] = 0xABCD1234;   // unique value
	batch[4] = 0x0A000000;   // MI_BATCH_BUFFER_END
	batch[5] = 0x00000000;   // MI_NOOP (pad)

	// Read fb[0] before
	uint32* fb0 = (uint32*)(sSI->graphics_memory + fbOff);
	__asm__ __volatile__("mfence":::"memory");
	__asm__ __volatile__("clflush (%0)"::"r"(fb0));
	__asm__ __volatile__("mfence":::"memory");
	printf("  fb[0] before: 0x%08x\n", *fb0);

	// Memory barrier for batch
	__asm__ __volatile__("mfence":::"memory");

	// Emit MI_BATCH_BUFFER_START into ring
	printf("  MI_BATCH_BUFFER_START → 0x%x\n", batchOff);
	acquire_lock(&ring.lock);
	uint32 pos = ring.position / 4;
	rm[pos++] = 0x02000000;                    // MI_FLUSH
	rm[pos++] = 0x00000000;                    // MI_NOOP
	rm[pos++] = (0x31 << 23);                  // MI_BATCH_BUFFER_START
	rm[pos++] = batchOff;                      // batch GTT address
	rm[pos++] = 0x02000000;                    // MI_FLUSH
	rm[pos++] = 0x00000000;                    // MI_NOOP
	ring.position = pos * 4;
	ring.space_left -= 24;
	*(volatile uint32*)(sRegs + ring.register_base) = ring.position;
	release_lock(&ring.lock);

	snooze(100000);

	uint32 head = rr(0x2034) & INTEL_RING_BUFFER_HEAD_MASK;
	uint32 esr = rr(0x20b8);
	uint32 ipehr = rr(0x2068);
	printf("  HEAD=0x%x TAIL=0x%x ESR=0x%x IPEHR=0x%08x\n",
		head, rr(0x2030), esr, ipehr);

	// Check if batch was executed
	__asm__ __volatile__("mfence":::"memory");
	__asm__ __volatile__("clflush (%0)"::"r"(fb0));
	__asm__ __volatile__("mfence":::"memory");
	printf("  fb[0] after: 0x%08x %s\n", *fb0,
		*fb0 == 0xABCD1234 ? "BATCH EXECUTED!" : "not written");

	if (*fb0 != 0xABCD1234) {
		// Try alternate encoding: bit 8 = address space select
		printf("\n--- Test 2: Alternate MI_BATCH_BUFFER_START encoding ---\n");

		// Reset fb[0]
		batch[3] = 0xDEAD5678;
		__asm__ __volatile__("mfence":::"memory");

		acquire_lock(&ring.lock);
		pos = ring.position / 4;
		rm[pos++] = 0x02000000;
		rm[pos++] = 0x00000000;
		// Try with address space bit: (0x31 << 23) | (1 << 8)
		rm[pos++] = (0x31 << 23) | (1 << 8);
		rm[pos++] = batchOff;
		rm[pos++] = 0x02000000;
		rm[pos++] = 0x00000000;
		ring.position = pos * 4;
		ring.space_left -= 24;
		*(volatile uint32*)(sRegs + ring.register_base) = ring.position;
		release_lock(&ring.lock);

		snooze(100000);
		head = rr(0x2034) & INTEL_RING_BUFFER_HEAD_MASK;
		esr = rr(0x20b8);
		ipehr = rr(0x2068);
		printf("  HEAD=0x%x ESR=0x%x IPEHR=0x%08x\n", head, esr, ipehr);

		__asm__ __volatile__("mfence":::"memory");
		__asm__ __volatile__("clflush (%0)"::"r"(fb0));
		__asm__ __volatile__("mfence":::"memory");
		printf("  fb[0] after: 0x%08x %s\n", *fb0,
			*fb0 == 0xDEAD5678 ? "BATCH EXECUTED!" : "not written");
	}

	if (*fb0 != 0xABCD1234 && *fb0 != 0xDEAD5678) {
		// Test 3: Use MI_BATCH_BUFFER (opcode 0x30) instead
		printf("\n--- Test 3: MI_BATCH_BUFFER (0x30) ---\n");
		batch[3] = 0xCAFE9999;
		__asm__ __volatile__("mfence":::"memory");

		acquire_lock(&ring.lock);
		pos = ring.position / 4;
		rm[pos++] = 0x02000000;
		rm[pos++] = 0x00000000;
		rm[pos++] = (0x30 << 23) | 1;  // MI_BATCH_BUFFER, len=1
		rm[pos++] = batchOff;           // start
		rm[pos++] = batchOff + 20;      // end (5 DWORDs = 20 bytes)
		rm[pos++] = 0x00000000;         // MI_NOOP
		rm[pos++] = 0x02000000;
		rm[pos++] = 0x00000000;
		ring.position = pos * 4;
		ring.space_left -= 32;
		*(volatile uint32*)(sRegs + ring.register_base) = ring.position;
		release_lock(&ring.lock);

		snooze(100000);
		head = rr(0x2034) & INTEL_RING_BUFFER_HEAD_MASK;
		esr = rr(0x20b8);
		ipehr = rr(0x2068);
		printf("  HEAD=0x%x ESR=0x%x IPEHR=0x%08x\n", head, esr, ipehr);

		__asm__ __volatile__("mfence":::"memory");
		__asm__ __volatile__("clflush (%0)"::"r"(fb0));
		__asm__ __volatile__("mfence":::"memory");
		printf("  fb[0] after: 0x%08x %s\n", *fb0,
			*fb0 == 0xCAFE9999 ? "BATCH EXECUTED!" : "not written");
	}

	// Test 4: BLT in batch (if batch execution works)
	if (*fb0 == 0xABCD1234 || *fb0 == 0xDEAD5678 || *fb0 == 0xCAFE9999) {
		printf("\n--- Test 4: BLT in batch buffer ---\n");
		batch[0] = 0x54300004;  // XY_COLOR_BLT | RGBA
		batch[1] = (0x03 << 24) | (0xF0 << 16) | (bpr & 0xFFFF);
		batch[2] = (400 << 16) | 400;
		batch[3] = (420 << 16) | 420;
		batch[4] = fbOff;
		batch[5] = 0xFF0000FF;  // blue
		batch[6] = 0x0A000000;  // MI_BATCH_BUFFER_END
		batch[7] = 0x00000000;
		__asm__ __volatile__("mfence":::"memory");

		gem.ExecBatch(batchHandle, 28);
		gem.WaitIdle(500000);

		uint32* px400 = (uint32*)(sSI->graphics_memory + fbOff + 400*bpr + 400*4);
		__asm__ __volatile__("mfence":::"memory");
		__asm__ __volatile__("clflush (%0)"::"r"(px400));
		__asm__ __volatile__("mfence":::"memory");
		printf("  fb[400,400] = 0x%08x %s\n", *px400,
			*px400 == 0xFF0000FF ? "BLUE!" : "not written");
	}

	gem.CloseBuffer(batchHandle);
	printf("\n=== Done ===\n");
	return 0;
}
