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
	uint32* rm = (uint32*)ring.base;
	uint32 bpr = sSI->bytes_per_row;
	uint32 fbOff = sSI->frame_buffer_offset;

	printf("=== BLT Fix Attempts ===\n");
	printf("fb_offset=0x%x bpr=%u\n", fbOff, bpr);

	// Target: pixel at (300,300) in framebuffer
	uint32* pixel = (uint32*)(sSI->graphics_memory + fbOff + 300*bpr + 300*4);
	__asm__ __volatile__("mfence":::"memory");
	__asm__ __volatile__("clflush (%0)"::"r"(pixel));
	__asm__ __volatile__("mfence":::"memory");
	printf("Before: pixel(300,300)=0x%08x\n\n", *pixel);

	// Attempt 1: BLT with MI_FLUSH before and after
	printf("--- Attempt 1: MI_FLUSH + BLT + MI_FLUSH ---\n");
	acquire_lock(&ring.lock);
	uint32 pos = ring.position / 4;
	rm[pos++] = 0x02000000;       // MI_FLUSH
	rm[pos++] = 0x00000000;       // MI_NOOP (pad)
	rm[pos++] = 0x54300004;       // XY_COLOR_BLT | RGBA
	rm[pos++] = (0x03 << 24) | (0xF0 << 16) | (bpr & 0xFFFF);
	rm[pos++] = (300 << 16) | 300;
	rm[pos++] = (310 << 16) | 310;
	rm[pos++] = fbOff;
	rm[pos++] = 0xFF00FF00;       // green
	rm[pos++] = 0x02000000;       // MI_FLUSH
	rm[pos++] = 0x00000000;       // MI_NOOP
	ring.position = pos * 4;
	ring.space_left -= pos * 4;
	*(volatile uint32*)(sRegs + 0x2030) = ring.position;
	release_lock(&ring.lock);
	snooze(100000);

	uint32 esr = rr(0x20b8);
	uint32 head = rr(0x2034) & INTEL_RING_BUFFER_HEAD_MASK;
	__asm__ __volatile__("mfence":::"memory");
	__asm__ __volatile__("clflush (%0)"::"r"(pixel));
	__asm__ __volatile__("mfence":::"memory");
	printf("  HEAD=0x%x ESR=0x%x pixel=0x%08x %s\n",
		head, esr, *pixel, *pixel == 0xFF00FF00 ? "GREEN!" : "no");

	// Attempt 2: Use COLOR_BLT (non-XY version, opcode 0x40)
	// COLOR_BLT = 0x40000003 (2D, opcode 0x40, length=3)
	printf("\n--- Attempt 2: COLOR_BLT (non-XY) ---\n");
	pixel = (uint32*)(sSI->graphics_memory + fbOff + 310*bpr + 310*4);
	__asm__ __volatile__("clflush (%0)"::"r"(pixel));
	__asm__ __volatile__("mfence":::"memory");
	printf("  Before: pixel(310,310)=0x%08x\n", *pixel);

	acquire_lock(&ring.lock);
	pos = ring.position / 4;
	// COLOR_BLT: DW0=opcode, DW1=raster_op|pitch, DW2=height<<16|width, DW3=addr, DW4=color
	// Actually COLOR_BLT on Gen5: 0x40000003
	rm[pos++] = 0x40000003 | 0x00300000;  // COLOR_BLT | RGBA write
	rm[pos++] = (0x03 << 24) | (0xF0 << 16) | (bpr & 0xFFFF);
	rm[pos++] = (10 << 16) | (10 * 4);    // 10 rows, 10*4=40 bytes wide
	rm[pos++] = fbOff + 310*bpr + 310*4;   // start address
	rm[pos++] = 0xFFFF0000;               // red
	rm[pos++] = 0x00000000;               // pad to even
	ring.position = pos * 4;
	ring.space_left -= 24;
	*(volatile uint32*)(sRegs + 0x2030) = ring.position;
	release_lock(&ring.lock);
	snooze(100000);

	esr = rr(0x20b8);
	head = rr(0x2034) & INTEL_RING_BUFFER_HEAD_MASK;
	__asm__ __volatile__("mfence":::"memory");
	__asm__ __volatile__("clflush (%0)"::"r"(pixel));
	__asm__ __volatile__("mfence":::"memory");
	printf("  HEAD=0x%x ESR=0x%x pixel=0x%08x %s\n",
		head, esr, *pixel, *pixel == 0xFFFF0000 ? "RED!" : "no");

	// Attempt 3: Use MI_STORE_DATA_IMM to manually fill pixels
	printf("\n--- Attempt 3: MI_STORE_DATA_IMM fill (10 pixels) ---\n");
	uint32 pixAddr = fbOff + 320*bpr + 320*4;
	uint32* pixPtr = (uint32*)(sSI->graphics_memory + pixAddr);

	acquire_lock(&ring.lock);
	pos = ring.position / 4;
	for (int p = 0; p < 10; p++) {
		rm[pos++] = 0x10400002;  // MI_STORE_DATA_IMM | GGTT | len=2
		rm[pos++] = 0;
		rm[pos++] = pixAddr + p * 4;
		rm[pos++] = 0xFF0000FF;  // blue
	}
	ring.position = pos * 4;
	ring.space_left -= 10 * 16;
	*(volatile uint32*)(sRegs + 0x2030) = ring.position;
	release_lock(&ring.lock);
	snooze(100000);

	head = rr(0x2034) & INTEL_RING_BUFFER_HEAD_MASK;
	__asm__ __volatile__("mfence":::"memory");
	for (int c = 0; c < 64; c += 64)
		__asm__ __volatile__("clflush (%0)"::"r"((uint8*)pixPtr + c));
	__asm__ __volatile__("mfence":::"memory");
	printf("  HEAD=0x%x pixel(320,320)=0x%08x %s\n",
		head, pixPtr[0], pixPtr[0] == 0xFF0000FF ? "BLUE!" : "no");
	printf("  pixel(320,321)=0x%08x %s\n",
		pixPtr[1], pixPtr[1] == 0xFF0000FF ? "BLUE!" : "no");

	return 0;
}
