/*
 * blt_diag - Minimal BLT diagnostic for GEM buffers
 *
 * Tests whether the GPU BLT engine can write to GEM-allocated
 * buffers, and compares with framebuffer BLT (known to work).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>

#include <OS.h>

#include "accelerant.h"
#include "intel_extreme.h"
#include "GemManager.h"


#define XY_COMMAND_COLOR_BLIT	0x54000004
#define COMMAND_BLIT_RGBA		0x00300000
#define COMMAND_MODE_RGB32		0x03

static int				sFd = -1;
static intel_shared_info* sSI = NULL;
static volatile uint8*	sRegs = NULL;
static area_id			sInfoArea = -1;
static area_id			sRegsArea = -1;


static bool
init_gpu()
{
	DIR* dir = opendir("/dev/graphics");
	if (!dir) return false;
	char path[256] = {};
	struct dirent* e;
	while ((e = readdir(dir)) != NULL) {
		if (strncmp(e->d_name, "intel_extreme", 13) == 0) {
			snprintf(path, sizeof(path), "/dev/graphics/%s", e->d_name);
			break;
		}
	}
	closedir(dir);
	if (!path[0]) return false;

	sFd = open(path, B_READ_WRITE);
	if (sFd < 0) return false;

	intel_get_private_data data;
	data.magic = INTEL_PRIVATE_DATA_MAGIC;
	if (ioctl(sFd, INTEL_GET_PRIVATE_DATA, &data, sizeof(data)) != 0)
		return false;

	sInfoArea = clone_area("diag_si", (void**)&sSI, B_ANY_ADDRESS,
		B_READ_AREA | B_WRITE_AREA, data.shared_info_area);
	if (sInfoArea < 0) return false;

	sRegsArea = clone_area("diag_regs", (void**)&sRegs, B_ANY_ADDRESS,
		B_READ_AREA | B_WRITE_AREA, sSI->registers_area);
	if (sRegsArea < 0) return false;

	printf("Device: %s\n", path);
	printf("  DevType: 0x%08x Gen%d\n",
		sSI->device_type.type, sSI->device_type.Generation());
	printf("  graphics_memory: %p\n", sSI->graphics_memory);
	printf("  graphics_memory_size: %u MB\n",
		sSI->graphics_memory_size / (1024*1024));
	printf("  frame_buffer_offset: 0x%x\n", sSI->frame_buffer_offset);
	printf("  bytes_per_row: %u\n", sSI->bytes_per_row);
	printf("  display: %ux%u\n",
		sSI->current_mode.timing.h_display,
		sSI->current_mode.timing.v_display);
	return true;
}


static inline uint32 read_reg(uint32 off)
{ return *(volatile uint32*)(sRegs + off); }


static void
ring_emit(ring_buffer& ring, uint32 val)
{
	*(uint32*)(ring.base + ring.position) = val;
	ring.position = (ring.position + sizeof(uint32)) & (ring.size - 1);
	ring.space_left -= sizeof(uint32);
}


static void
ring_flush(ring_buffer& ring)
{
	*(volatile uint32*)(sRegs + ring.register_base + RING_BUFFER_TAIL)
		= ring.position;
}


static bool
wait_idle(bigtime_t timeout = 500000)
{
	ring_buffer& ring = sSI->primary_ring_buffer;
	bigtime_t start = system_time();
	while (true) {
		uint32 head = read_reg(ring.register_base + RING_BUFFER_HEAD)
			& INTEL_RING_BUFFER_HEAD_MASK;
		uint32 tail = read_reg(ring.register_base + RING_BUFFER_TAIL);
		if (head == tail) return true;
		if (system_time() > start + timeout) return false;
		snooze(100);
	}
}


int
main()
{
	printf("=== BLT Diagnostic ===\n\n");

	if (!init_gpu()) {
		printf("[FAIL] GPU init\n");
		return 1;
	}

	ring_buffer& ring = sSI->primary_ring_buffer;

	// Ring state
	printf("\n--- Ring Status ---\n");
	uint32 ringCtl = read_reg(ring.register_base + RING_BUFFER_CONTROL);
	uint32 ringHead = read_reg(ring.register_base + RING_BUFFER_HEAD);
	uint32 ringTail = read_reg(ring.register_base + RING_BUFFER_TAIL);
	printf("  CTL=0x%08x (enabled=%s)\n", ringCtl,
		(ringCtl & 1) ? "yes" : "NO");
	printf("  HEAD=0x%08x TAIL=0x%08x\n", ringHead, ringTail);
	printf("  ring.position=%u ring.size=%u ring.space_left=%u\n",
		ring.position, ring.size, ring.space_left);
	printf("  ring.base=%p ring.register_base=0x%x\n",
		(void*)ring.base, ring.register_base);

	if (!(ringCtl & 1)) {
		printf("  [WARN] Ring NOT enabled! BLT will not work.\n");
		printf("  This may mean app_server hasn't initialized the ring,\n");
		printf("  or the ring was disabled after a GPU hang.\n");
	}

	bool headTailMatch = ((ringHead & INTEL_RING_BUFFER_HEAD_MASK)
		== ringTail);
	printf("  HEAD==TAIL: %s\n", headTailMatch ? "yes" : "NO (ring busy)");

	// Test 1: BLT to framebuffer (known to work from bench_gpu)
	printf("\n--- Test 1: BLT to framebuffer ---\n");
	uint32 fbOff = sSI->frame_buffer_offset;
	uint32 bpr = sSI->bytes_per_row;
	printf("  fbOffset=0x%x pitch=%u\n", fbOff, bpr);

	// Read a pixel from framebuffer before BLT
	uint32* fb = (uint32*)(sSI->graphics_memory + fbOff);
	printf("  fb pixel(100,100) before: 0x%08x\n", fb[100 * (bpr/4) + 100]);

	acquire_lock(&ring.lock);
	ring_emit(ring, XY_COMMAND_COLOR_BLIT | COMMAND_BLIT_RGBA);
	ring_emit(ring, (COMMAND_MODE_RGB32 << 24) | (0xF0 << 16)
		| (bpr & 0xFFFF));
	ring_emit(ring, (100 << 16) | 100);		// y1=100, x1=100
	ring_emit(ring, (104 << 16) | 104);		// y2=104, x2=104
	ring_emit(ring, fbOff);
	ring_emit(ring, 0xFF00FF00);			// green
	ring_flush(ring);
	release_lock(&ring.lock);

	wait_idle();

	// Invalidate CPU cache for the read-back region
	__asm__ __volatile__("mfence" ::: "memory");
	uint8* flush = (uint8*)&fb[100 * (bpr/4) + 100];
	for (int c = 0; c < 256; c += 64)
		__asm__ __volatile__("clflush (%0)" :: "r"(flush + c));
	__asm__ __volatile__("mfence" ::: "memory");

	printf("  fb pixel(100,100) after:  0x%08x %s\n",
		fb[100 * (bpr/4) + 100],
		fb[100 * (bpr/4) + 100] == 0xFF00FF00 ? "(green OK)" : "(unexpected)");

	// Test 2: BLT to GEM buffer
	printf("\n--- Test 2: BLT to GEM buffer ---\n");
	GemManager gem;
	gem.Init(sSI, sRegs, sFd);

	uint32 handle = 0;
	gem.CreateBuffer(4096, &handle);
	uint32 gemOff = gem.GetOffset(handle);
	void* gemPtr = gem.MapBuffer(handle);

	printf("  GEM handle=%u offset=0x%x ptr=%p\n",
		handle, gemOff, gemPtr);

	// Zero it first
	memset(gemPtr, 0, 4096);
	printf("  Before BLT: pixel[0]=0x%08x\n", ((uint32*)gemPtr)[0]);

	// BLT fill: 4x4 pixels at (0,0), pitch=16 bytes (4px * 4)
	uint32 gemPitch = 16;
	acquire_lock(&ring.lock);
	ring_emit(ring, XY_COMMAND_COLOR_BLIT | COMMAND_BLIT_RGBA);
	ring_emit(ring, (COMMAND_MODE_RGB32 << 24) | (0xF0 << 16)
		| (gemPitch & 0xFFFF));
	ring_emit(ring, (0 << 16) | 0);		// y1=0, x1=0
	ring_emit(ring, (4 << 16) | 4);		// y2=4, x2=4
	ring_emit(ring, gemOff);
	ring_emit(ring, 0xFFFF0000);			// red
	ring_flush(ring);
	release_lock(&ring.lock);

	bool idle = wait_idle();
	printf("  GPU idle: %s\n", idle ? "yes" : "TIMEOUT");

	// Flush CPU cache
	__asm__ __volatile__("mfence" ::: "memory");
	for (uint32 c = 0; c < 4096; c += 64)
		__asm__ __volatile__("clflush (%0)" :: "r"((uint8*)gemPtr + c));
	__asm__ __volatile__("mfence" ::: "memory");

	printf("  After BLT:  pixel[0]=0x%08x %s\n",
		((uint32*)gemPtr)[0],
		((uint32*)gemPtr)[0] == 0xFFFF0000 ? "(red OK)" : "(unexpected)");

	// Also try reading directly from graphics_memory + offset
	uint32* directRead = (uint32*)(sSI->graphics_memory + gemOff);
	__asm__ __volatile__("clflush (%0)" :: "r"(directRead));
	__asm__ __volatile__("mfence" ::: "memory");
	printf("  Direct read: pixel[0]=0x%08x\n", directRead[0]);

	// Diagnostic: are gemPtr and directRead the same address?
	printf("  gemPtr=%p directRead=%p same=%s\n",
		gemPtr, directRead,
		gemPtr == (void*)directRead ? "yes" : "NO");

	gem.CloseBuffer(handle);
	printf("\n=== Done ===\n");
	return 0;
}
