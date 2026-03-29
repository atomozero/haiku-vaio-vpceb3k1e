/*
 * quick_blt - Minimal BLT test with ring state tracking
 */
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
		if (strncmp(e->d_name, "intel_extreme", 13) == 0) {
			snprintf(path, sizeof(path), "/dev/graphics/%s", e->d_name);
			break;
		}
	closedir(dir);
	int fd = open(path, B_READ_WRITE);
	intel_get_private_data data;
	data.magic = INTEL_PRIVATE_DATA_MAGIC;
	ioctl(fd, INTEL_GET_PRIVATE_DATA, &data, sizeof(data));
	clone_area("si", (void**)&sSI, B_ANY_ADDRESS, B_READ_AREA|B_WRITE_AREA, data.shared_info_area);
	clone_area("rg", (void**)&sRegs, B_ANY_ADDRESS, B_READ_AREA|B_WRITE_AREA, sSI->registers_area);

	ring_buffer& ring = sSI->primary_ring_buffer;
	uint32 rb = ring.register_base;

	printf("Ring: HEAD=0x%x TAIL=0x%x CTL=0x%x\n", rr(rb+4), rr(rb), rr(rb+0xc));
	printf("ESR=0x%x EIR=0x%x\n", rr(0x20b8), rr(0x20b0));

	// Test 1: MI_NOOP — does HEAD advance?
	printf("\n--- MI_NOOP test ---\n");
	acquire_lock(&ring.lock);
	uint32* rm = (uint32*)ring.base;
	uint32 pos = ring.position / 4;
	rm[pos++] = 0x00000000;  // MI_NOOP
	rm[pos++] = 0x00000000;  // MI_NOOP
	ring.position = pos * 4;
	ring.space_left -= 8;
	// Flush: write TAIL register
	*(volatile uint32*)(sRegs + rb) = ring.position;
	release_lock(&ring.lock);

	snooze(50000);
	uint32 headAfter = rr(rb + 4) & INTEL_RING_BUFFER_HEAD_MASK;
	printf("  TAIL=0x%x HEAD=0x%x\n", rr(rb), headAfter);
	printf("  HEAD advanced: %s\n", headAfter >= ring.position ? "YES" : "NO");

	if (headAfter < ring.position) {
		printf("  [FAIL] Ring not processing\n");
		return 1;
	}

	// Test 2: MI_STORE_DATA_IMM — write to a known GTT address
	printf("\n--- MI_STORE_DATA_IMM test ---\n");
	// Write 0xCAFEBABE to framebuffer offset + 0 (first pixel)
	uint32 fbOff = sSI->frame_buffer_offset;
	uint32* fbPtr = (uint32*)(sSI->graphics_memory + fbOff);
	printf("  fb[0] before: 0x%08x (fb_off=0x%x)\n", fbPtr[0], fbOff);

	// MI_STORE_DATA_IMM: opcode 0x20, writes data to memory address
	// DW0: 0x10200002 (MI_STORE_DATA_IMM | DW_LENGTH=2)
	// Actually for Gen5: MI_STORE_DATA_IMM = (0x20 << 23) | use_ggtt | dwords
	// Format: (0x20 << 23) | (1 << 22) [use GGTT] | length
	// DW0: opcode, DW1: address low, DW2: address high, DW3: data low
	acquire_lock(&ring.lock);
	pos = ring.position / 4;
	rm[pos++] = (0x20 << 23) | (1 << 22) | 2;  // MI_STORE_DATA_IMM, GGTT, len=2
	rm[pos++] = fbOff;        // address (GTT offset)
	rm[pos++] = 0;            // address high
	rm[pos++] = 0xCAFEBABE;   // data to write
	ring.position = pos * 4;
	ring.space_left -= 16;
	*(volatile uint32*)(sRegs + rb) = ring.position;
	release_lock(&ring.lock);

	snooze(50000);
	headAfter = rr(rb + 4) & INTEL_RING_BUFFER_HEAD_MASK;
	printf("  TAIL=0x%x HEAD=0x%x\n", rr(rb), headAfter);

	// Check for errors
	uint32 esr = rr(0x20b8);
	printf("  ESR=0x%x\n", esr);

	// Read back
	__asm__ __volatile__("mfence":::"memory");
	__asm__ __volatile__("clflush (%0)"::"r"(fbPtr));
	__asm__ __volatile__("mfence":::"memory");
	printf("  fb[0] after: 0x%08x %s\n", fbPtr[0],
		fbPtr[0] == 0xCAFEBABE ? "(OK!)" : "(not written)");

	// Test 3: XY_COLOR_BLT to framebuffer
	printf("\n--- XY_COLOR_BLT to framebuffer ---\n");
	uint32 bpr = sSI->bytes_per_row;
	uint32* testPixel = (uint32*)(sSI->graphics_memory + fbOff + 200*bpr + 200*4);
	printf("  fb[200,200] before: 0x%08x\n", *testPixel);

	acquire_lock(&ring.lock);
	pos = ring.position / 4;
	rm[pos++] = 0x54000004 | 0x00300000;  // XY_COLOR_BLT | RGBA
	rm[pos++] = (0x03 << 24) | (0xF0 << 16) | (bpr & 0xFFFF);
	rm[pos++] = (200 << 16) | 200;    // y1=200, x1=200
	rm[pos++] = (210 << 16) | 210;    // y2=210, x2=210
	rm[pos++] = fbOff;
	rm[pos++] = 0xFF00FF00;           // green
	ring.position = pos * 4;
	ring.space_left -= 24;
	*(volatile uint32*)(sRegs + rb) = ring.position;
	release_lock(&ring.lock);

	snooze(100000);
	headAfter = rr(rb + 4) & INTEL_RING_BUFFER_HEAD_MASK;
	esr = rr(0x20b8);
	printf("  TAIL=0x%x HEAD=0x%x ESR=0x%x\n", rr(rb), headAfter, esr);

	__asm__ __volatile__("mfence":::"memory");
	for (int c = 0; c < 256; c += 64)
		__asm__ __volatile__("clflush (%0)"::"r"((uint8*)testPixel + c));
	__asm__ __volatile__("mfence":::"memory");
	printf("  fb[200,200] after: 0x%08x %s\n", *testPixel,
		*testPixel == 0xFF00FF00 ? "(GREEN OK!)" : "(not written)");

	return 0;
}
