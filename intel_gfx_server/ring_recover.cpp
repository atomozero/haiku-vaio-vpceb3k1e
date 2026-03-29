/*
 * ring_recover - Aggressive ring recovery for GPU hang
 *
 * 1. Clear error registers
 * 2. Stop ring
 * 3. Force HEAD=0, TAIL=0
 * 4. Re-enable ring
 * 5. Test with MI_NOOP
 */
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <OS.h>
#include "accelerant.h"
#include "intel_extreme.h"

static intel_shared_info* sSI = NULL;
static volatile uint8* sRegs = NULL;

static uint32 rr(uint32 o) { return *(volatile uint32*)(sRegs + o); }
static void wr(uint32 o, uint32 v) { *(volatile uint32*)(sRegs + o) = v; }

static bool init() {
	DIR* dir = opendir("/dev/graphics");
	if (!dir) return false;
	char path[256] = {};
	struct dirent* e;
	while ((e = readdir(dir)))
		if (strncmp(e->d_name, "intel_extreme", 13) == 0) {
			snprintf(path, sizeof(path), "/dev/graphics/%s", e->d_name);
			break;
		}
	closedir(dir);
	int fd = open(path, B_READ_WRITE);
	if (fd < 0) return false;
	intel_get_private_data data;
	data.magic = INTEL_PRIVATE_DATA_MAGIC;
	if (ioctl(fd, INTEL_GET_PRIVATE_DATA, &data, sizeof(data)) != 0)
		return false;
	area_id a1 = clone_area("rc_si", (void**)&sSI, B_ANY_ADDRESS,
		B_READ_AREA | B_WRITE_AREA, data.shared_info_area);
	if (a1 < 0) return false;
	area_id a2 = clone_area("rc_regs", (void**)&sRegs, B_ANY_ADDRESS,
		B_READ_AREA | B_WRITE_AREA, sSI->registers_area);
	return a2 >= 0;
}

int main() {
	if (!init()) { printf("init fail\n"); return 1; }

	ring_buffer& ring = sSI->primary_ring_buffer;
	uint32 rb = ring.register_base;  // 0x2030

	printf("=== Ring Recovery ===\n\n");
	printf("Before:\n");
	printf("  HEAD=0x%x TAIL=0x%x CTL=0x%x\n",
		rr(rb+4), rr(rb+0), rr(rb+8));
	printf("  ESR=0x%x EIR=0x%x IPEHR=0x%x\n",
		rr(0x20b8), rr(0x20b0), rr(0x2068));

	// Step 1: Clear error registers
	printf("\nStep 1: Clear errors\n");
	wr(0x20b0, rr(0x20b0));  // EIR: write back to clear
	wr(0x20b8, rr(0x20b8));  // ESR: write back to clear
	wr(0x2064, 0);            // IPEIR
	wr(0x2068, 0);            // IPEHR
	snooze(1000);
	printf("  ESR=0x%x EIR=0x%x\n", rr(0x20b8), rr(0x20b0));

	// Step 2: Stop ring
	printf("\nStep 2: Stop ring\n");
	wr(rb + 8, 0);  // CTL = 0 (disable)
	snooze(10000);   // wait 10ms
	printf("  CTL=0x%x\n", rr(rb+8));

	// Step 3: Zero the ring buffer memory
	printf("\nStep 3: Zero ring memory\n");
	memset((void*)ring.base, 0, ring.size);

	// Step 4: Force HEAD=0
	printf("\nStep 4: Force HEAD=0, TAIL=0\n");
	wr(rb + 4, 0);  // HEAD = 0
	snooze(1000);
	wr(rb + 0, 0);  // TAIL = 0
	snooze(1000);
	printf("  HEAD=0x%x TAIL=0x%x\n", rr(rb+4), rr(rb+0));

	// Step 5: Set ring START (physical address of ring buffer)
	// The ring buffer is at the start of graphics memory (GTT offset 0)
	printf("\nStep 5: Set START\n");
	uint32 ringPhys = 0;  // GTT offset 0 for the ring
	wr(rb + 0x0C, ringPhys);  // RING_BUFFER_START is at base+0x0C? 
	// Actually on Gen5: START=base+0, TAIL=base+0, HEAD=base+4, 
	// START_reg=base-4 ... let me use the standard offsets
	// RING_BUFFER_START (0x2038) = register_base + 8 bytes before?
	// No: standard layout is START=0x2038, HEAD=0x2034, TAIL=0x2030, CTL=0x203C
	// But our register_base = 0x2030 and we use:
	//   TAIL = register_base + 0 = 0x2030
	//   HEAD = register_base + 4 = 0x2034
	//   START= register_base + 8 = 0x2038  (or is it CTL?)
	//   CTL  = register_base + 0xC = 0x203C
	// Let me check what the actual layout is:
	printf("  reg[base+0]=0x%x (TAIL)\n", rr(rb));
	printf("  reg[base+4]=0x%x (HEAD)\n", rr(rb+4));
	printf("  reg[base+8]=0x%x (START/CTL?)\n", rr(rb+8));
	printf("  reg[base+C]=0x%x\n", rr(rb+0xc));

	// Intel Gen5 RCS ring register layout:
	// 0x2030 = TAIL
	// 0x2034 = HEAD  
	// 0x2038 = START (ring buffer physical/GTT base address)
	// 0x203C = CTL (control: ring size + enable bit)

	wr(0x2038, ringPhys);  // START = 0 (ring at GTT offset 0)
	snooze(1000);
	printf("  START after write: 0x%x\n", rr(0x2038));

	// Step 6: Re-enable ring
	printf("\nStep 6: Enable ring\n");
	// Ring size: 64KB = 0x10000, CTL value = (pages-1) | 1
	// 64KB = 16 pages of 4KB. Length bits [12:1] encode (pages-1)
	// Actually CTL bits: [20:12] = length in pages - 1, bit 0 = enable
	// 64KB / 4KB = 16 pages. (16-1) = 15 = 0xF. CTL = (0xF << 12) | 1 = 0xF001
	wr(0x203C, 0x0000F001);
	snooze(5000);
	printf("  CTL=0x%x HEAD=0x%x TAIL=0x%x\n",
		rr(0x203C), rr(0x2034), rr(0x2030));

	// Update SW state
	ring.position = 0;
	ring.space_left = ring.size - 16;

	// Step 7: Test with MI_NOOP
	printf("\nStep 7: Test MI_NOOP\n");
	acquire_lock(&ring.lock);
	uint32* ringMem = (uint32*)ring.base;
	ringMem[0] = 0x00000000;  // MI_NOOP
	ringMem[1] = 0x00000000;  // MI_NOOP
	ring.position = 8;
	ring.space_left -= 8;
	// Write TAIL
	wr(0x2030, 8);
	release_lock(&ring.lock);

	snooze(50000);  // 50ms

	uint32 newHead = rr(0x2034) & 0x1FFFF8;
	printf("  HEAD=0x%x (expect 0x8)\n", newHead);
	printf("  %s\n", newHead == 8 ? "[OK] GPU responding!" :
		"[FAIL] Still hung");

	if (newHead == 8) {
		// Bonus: try a BLT to framebuffer
		printf("\nBonus: BLT to framebuffer\n");
		uint32 fbOff = sSI->frame_buffer_offset;
		uint32 bpr = sSI->bytes_per_row;

		acquire_lock(&ring.lock);
		uint32 pos = ring.position / 4;
		ringMem[pos++] = 0x54300004;  // XY_COLOR_BLT | RGBA
		ringMem[pos++] = (0x03 << 24) | (0xF0 << 16) | (bpr & 0xFFFF);
		ringMem[pos++] = (50 << 16) | 50;     // y1=50, x1=50
		ringMem[pos++] = (60 << 16) | 60;     // y2=60, x2=60
		ringMem[pos++] = fbOff;
		ringMem[pos++] = 0xFF00FF00;           // green
		ring.position = pos * 4;
		ring.space_left -= 24;
		wr(0x2030, ring.position);
		release_lock(&ring.lock);

		snooze(50000);
		newHead = rr(0x2034) & 0x1FFFF8;
		printf("  HEAD=0x%x TAIL=0x%x\n", newHead, rr(0x2030));
		printf("  %s\n",
			newHead == ring.position ? "[OK] BLT executed!" :
			"[FAIL] BLT not processed");
	}

	printf("\nFinal state:\n");
	printf("  HEAD=0x%x TAIL=0x%x CTL=0x%x\n",
		rr(0x2034), rr(0x2030), rr(0x203C));
	printf("  ESR=0x%x EIR=0x%x\n", rr(0x20b8), rr(0x20b0));

	return 0;
}
