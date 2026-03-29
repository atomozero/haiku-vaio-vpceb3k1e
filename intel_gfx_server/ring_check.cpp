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
	clone_area("s", (void**)&sSI, B_ANY_ADDRESS, B_READ_AREA|B_WRITE_AREA, data.shared_info_area);
	clone_area("r", (void**)&sRegs, B_ANY_ADDRESS, B_READ_AREA|B_WRITE_AREA, sSI->registers_area);

	ring_buffer& ring = sSI->primary_ring_buffer;

	printf("=== Ring Deep Check ===\n");
	printf("RING_START=0x%08x (phys addr of ring)\n", rr(0x2038));
	printf("RING_HEAD =0x%08x\n", rr(0x2034));
	printf("RING_TAIL =0x%08x\n", rr(0x2030));
	printf("RING_CTL  =0x%08x\n", rr(0x203c));
	printf("ring.base =%p (virt)\n", (void*)ring.base);
	printf("ring.size =%u\n", ring.size);
	printf("graphics_memory=%p\n", sSI->graphics_memory);
	printf("fb_offset =0x%x\n", sSI->frame_buffer_offset);

	// Check: what physical address does the ring START point to?
	// If RING_START=0, it means GTT offset 0. The ring contents
	// are at graphics_memory+0 for CPU, and GPU reads from GTT[0].
	// This should work IF GTT[0] entries map to the same physical pages.

	// Try writing a unique pattern to ring at current position,
	// then read it back via both CPU and check INSTDONE changes.
	printf("\n--- Verify ring is being read ---\n");
	uint32 pos = ring.position / 4;
	uint32* rm = (uint32*)ring.base;

	// Put MI_FLUSH + MI_NOOP, check INSTDONE before/after
	uint32 instBefore = rr(0x206c);
	printf("INSTDONE before: 0x%08x\n", instBefore);

	acquire_lock(&ring.lock);
	rm[pos++] = 0x02000000;  // MI_FLUSH
	rm[pos++] = 0x00000000;  // MI_NOOP
	ring.position = pos * 4;
	ring.space_left -= 8;
	*(volatile uint32*)(sRegs + 0x2030) = ring.position;
	release_lock(&ring.lock);

	snooze(50000);
	uint32 instAfter = rr(0x206c);
	printf("INSTDONE after:  0x%08x\n", instAfter);
	printf("HEAD=0x%x TAIL=0x%x\n", rr(0x2034), rr(0x2030));

	// Try: read what the GPU sees at ring offset
	// by reading the ring's own GTT-mapped memory
	printf("\n--- GTT sanity ---\n");
	printf("Ring[0..3]  via CPU: 0x%08x 0x%08x 0x%08x 0x%08x\n",
		rm[0], rm[1], rm[2], rm[3]);

	// Check MI_STORE_DATA_IMM to scratch (offset 0x10000 in GTT)
	// This is the standard scratch page used by intel_extreme
	printf("\n--- MI_STORE_DATA_IMM to various offsets ---\n");

	// Test targets
	struct { const char* name; uint32 offset; } targets[] = {
		{ "scratch (0x10000)", 0x10000 },
		{ "fb+0 (0x21000)", 0x21000 },
		{ "ring+256 (0x100)", 0x100 },    // write INTO the ring buffer area
	};

	for (int t = 0; t < 3; t++) {
		uint32 addr = targets[t].offset;
		uint8* cpuPtr = sSI->graphics_memory + addr;

		// Read before
		__asm__ __volatile__("mfence":::"memory");
		__asm__ __volatile__("clflush (%0)"::"r"(cpuPtr));
		__asm__ __volatile__("mfence":::"memory");
		uint32 before = *(volatile uint32*)cpuPtr;

		// Emit MI_STORE_DATA_IMM
		acquire_lock(&ring.lock);
		pos = ring.position / 4;
		rm[pos++] = 0x10400002;  // MI_STORE_DATA_IMM | GGTT | len=2
		rm[pos++] = 0;           // reserved
		rm[pos++] = addr;        // GTT address
		rm[pos++] = 0xBAAD0000 | t;  // unique data per target
		ring.position = pos * 4;
		ring.space_left -= 16;
		*(volatile uint32*)(sRegs + 0x2030) = ring.position;
		release_lock(&ring.lock);

		snooze(50000);

		// Check errors
		uint32 esr = rr(0x20b8);
		uint32 head = rr(0x2034) & INTEL_RING_BUFFER_HEAD_MASK;

		// Read after
		__asm__ __volatile__("mfence":::"memory");
		__asm__ __volatile__("clflush (%0)"::"r"(cpuPtr));
		__asm__ __volatile__("mfence":::"memory");
		uint32 after = *(volatile uint32*)cpuPtr;

		printf("  %s: before=0x%08x after=0x%08x ESR=0x%x HEAD=0x%x %s\n",
			targets[t].name, before, after, esr, head,
			after == (0xBAAD0000 | (uint32)t) ? "WRITTEN!" : "unchanged");
	}

	// Last resort: check if HWS (Hardware Status Page) is set up
	printf("\n--- HWS (Hardware Status Page) ---\n");
	printf("HWS_PGA=0x%08x\n", rr(0x2080));  // Hardware Status Page address

	return 0;
}
