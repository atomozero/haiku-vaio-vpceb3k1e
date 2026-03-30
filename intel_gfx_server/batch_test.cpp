#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <OS.h>
#include "accelerant.h"
#include "intel_extreme.h"

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
	int fd = open(path, B_READ_WRITE);
	intel_get_private_data data;
	data.magic = INTEL_PRIVATE_DATA_MAGIC;
	ioctl(fd, INTEL_GET_PRIVATE_DATA, &data, sizeof(data));
	clone_area("s",(void**)&sSI,B_ANY_ADDRESS,B_READ_AREA|B_WRITE_AREA,data.shared_info_area);
	clone_area("r",(void**)&sRegs,B_ANY_ADDRESS,B_READ_AREA|B_WRITE_AREA,sSI->registers_area);

	ring_buffer& ring = sSI->primary_ring_buffer;
	uint32* rm = (uint32*)ring.base;

	printf("=== Minimal Batch Test ===\n\n");
	printf("Ring: HEAD=0x%x TAIL=0x%x ESR=0x%x\n",
		rr(0x2034), rr(0x2030), rr(0x20b8));

	// Place batch at offset 0xF000 in ring area (near end, far from ring activity)
	// ONLY MI_NOOP + MI_BATCH_BUFFER_END — simplest possible batch
	uint32 batchOff = 0xF000;
	uint32* batch = (uint32*)((uint8*)ring.base + batchOff);

	// Fill area with known pattern first
	for (int i = 0; i < 16; i++) batch[i] = 0xDEADDEAD;
	__asm__ __volatile__("mfence":::"memory");

	// Test A: Just MI_BATCH_BUFFER_END
	printf("\n--- Test A: ONLY MI_BATCH_BUFFER_END ---\n");
	batch[0] = MI_BATCH_BUFFER_END;
	batch[1] = 0x00000000;
	__asm__ __volatile__("mfence":::"memory");

	printf("  batch[0]=0x%08x batch[1]=0x%08x\n", batch[0], batch[1]);

	acquire_lock(&ring.lock);
	uint32 pos = ring.position / 4;
	rm[pos++] = 0x02000000;         // MI_FLUSH
	rm[pos++] = 0x00000000;
	rm[pos++] = MI_BATCH_BUFFER_START;
	rm[pos++] = batchOff;
	rm[pos++] = 0x02000000;         // MI_FLUSH
	rm[pos++] = 0x00000000;
	ring.position = pos * 4;
	ring.space_left -= 24;
	*(volatile uint32*)(sRegs + ring.register_base) = ring.position;
	release_lock(&ring.lock);

	snooze(100000);
	uint32 head = rr(0x2034) & INTEL_RING_BUFFER_HEAD_MASK;
	uint32 tail = rr(0x2030);
	printf("  HEAD=0x%x TAIL=0x%x ESR=0x%x\n", head, tail, rr(0x20b8));
	printf("  ACTHD=0x%08x IPEHR=0x%08x\n", rr(0x2074), rr(0x2068));
	printf("  %s\n", head >= tail ? "OK - returned to ring!" : "FAIL - stuck in batch");

	if (head < tail) {
		// Try with address | 1 (some gens need bit 0 set?)
		printf("\n--- Test B: batch addr | 1 ---\n");
		// Need reboot... can't recover
		printf("  Skipped (GPU hung)\n");
		return 1;
	}

	// Test C: MI_STORE_DATA_IMM + MI_BATCH_BUFFER_END
	printf("\n--- Test C: MI_STORE_DATA_IMM in batch ---\n");
	uint32 fbOff = sSI->frame_buffer_offset;
	batch[0] = (0x20 << 23) | (1 << 22) | 2;  // MI_STORE_DATA_IMM | GGTT | len=2
	batch[1] = 0;
	batch[2] = fbOff;
	batch[3] = 0x12345678;
	batch[4] = MI_BATCH_BUFFER_END;
	batch[5] = 0x00000000;
	__asm__ __volatile__("mfence":::"memory");

	uint32* fb0 = (uint32*)(sSI->graphics_memory + fbOff);
	__asm__ __volatile__("clflush (%0)"::"r"(fb0));
	__asm__ __volatile__("mfence":::"memory");
	printf("  fb[0] before: 0x%08x\n", *fb0);

	acquire_lock(&ring.lock);
	pos = ring.position / 4;
	rm[pos++] = 0x02000000;
	rm[pos++] = 0x00000000;
	rm[pos++] = MI_BATCH_BUFFER_START;
	rm[pos++] = batchOff;
	rm[pos++] = 0x02000000;
	rm[pos++] = 0x00000000;
	ring.position = pos * 4;
	ring.space_left -= 24;
	*(volatile uint32*)(sRegs + ring.register_base) = ring.position;
	release_lock(&ring.lock);

	snooze(100000);
	head = rr(0x2034) & INTEL_RING_BUFFER_HEAD_MASK;
	__asm__ __volatile__("mfence":::"memory");
	__asm__ __volatile__("clflush (%0)"::"r"(fb0));
	__asm__ __volatile__("mfence":::"memory");
	printf("  HEAD=0x%x ESR=0x%x\n", head, rr(0x20b8));
	printf("  fb[0] after: 0x%08x %s\n", *fb0,
		*fb0 == 0x12345678 ? "BATCH WORKS!" : "not written");

	return 0;
}
