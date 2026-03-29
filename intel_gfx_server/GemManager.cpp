/*
 * GEM-like buffer object manager for Intel Gen5 on Haiku.
 */

#include "GemManager.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>


GemManager::GemManager()
	:
	fSharedInfo(NULL),
	fRegs(NULL),
	fRing(NULL),
	fNextHandle(1),
	fDeviceFd(-1),
	fLastSeq(0),
	fScratch(NULL)
{
	memset(fObjects, 0, sizeof(fObjects));
}


GemManager::~GemManager()
{
}


status_t
GemManager::Init(intel_shared_info* sharedInfo, volatile uint8* regs,
	int deviceFd)
{
	fSharedInfo = sharedInfo;
	fRegs = regs;
	fRing = &sharedInfo->primary_ring_buffer;
	fDeviceFd = deviceFd;

	printf("  GEM: using kernel ioctl INTEL_ALLOCATE_GRAPHICS_MEMORY\n");
	printf("  GEM: graphics_memory_size = %" B_PRIu32 " MB\n",
		sharedInfo->graphics_memory_size / (1024 * 1024));

	return B_OK;
}


// ---------------------------------------------------------------
// Buffer object management
// ---------------------------------------------------------------

status_t
GemManager::CreateBuffer(uint32 size, uint32* outHandle)
{
	// Page-align size
	size = (size + 0xFFF) & ~0xFFF;

	// Find free handle slot
	int slot = -1;
	for (int i = 0; i < GEM_MAX_OBJECTS; i++) {
		if (!fObjects[i].in_use) {
			slot = i;
			break;
		}
	}
	if (slot < 0)
		return B_NO_MEMORY;

	// Allocate via kernel driver ioctl (programs GTT entries properly)
	intel_allocate_graphics_memory allocMem;
	allocMem.magic = INTEL_PRIVATE_DATA_MAGIC;
	allocMem.size = size;
	allocMem.alignment = 0;
	allocMem.flags = 0;

	if (ioctl(fDeviceFd, INTEL_ALLOCATE_GRAPHICS_MEMORY, &allocMem,
			sizeof(allocMem)) < 0) {
		printf("  GEM: INTEL_ALLOCATE_GRAPHICS_MEMORY failed (errno=%d)\n",
			errno);
		return B_NO_MEMORY;
	}

	// Compute GTT offset from virtual address
	addr_t base = allocMem.buffer_base;
	uint32 offset = base - (addr_t)fSharedInfo->graphics_memory;

	// Fill in buffer object
	gem_buffer& bo = fObjects[slot];
	bo.handle = fNextHandle++;
	bo.gtt_offset = offset;
	bo.size = size;
	bo.cpu_addr = base;
	bo.in_use = true;

	// Zero the buffer
	memset((void*)bo.cpu_addr, 0, size);

	*outHandle = bo.handle;
	return B_OK;
}


void*
GemManager::MapBuffer(uint32 handle)
{
	for (int i = 0; i < GEM_MAX_OBJECTS; i++) {
		if (fObjects[i].in_use && fObjects[i].handle == handle)
			return (void*)fObjects[i].cpu_addr;
	}
	return NULL;
}


uint32
GemManager::GetOffset(uint32 handle)
{
	for (int i = 0; i < GEM_MAX_OBJECTS; i++) {
		if (fObjects[i].in_use && fObjects[i].handle == handle)
			return fObjects[i].gtt_offset;
	}
	return 0;
}


status_t
GemManager::CloseBuffer(uint32 handle)
{
	for (int i = 0; i < GEM_MAX_OBJECTS; i++) {
		if (fObjects[i].in_use && fObjects[i].handle == handle) {
			// Free via kernel ioctl
			intel_free_graphics_memory freeMem;
			freeMem.magic = INTEL_PRIVATE_DATA_MAGIC;
			freeMem.buffer_base = fObjects[i].cpu_addr;
			ioctl(fDeviceFd, INTEL_FREE_GRAPHICS_MEMORY, &freeMem,
				sizeof(freeMem));
			fObjects[i].in_use = false;
			return B_OK;
		}
	}
	return B_BAD_VALUE;
}


// ---------------------------------------------------------------
// Batch buffer execution
// ---------------------------------------------------------------

status_t
GemManager::ExecCommands(const uint32* cmds, uint32 count)
{
	if (cmds == NULL || count == 0)
		return B_BAD_VALUE;

	// Check GPU error state before submitting
	uint32 esr = _ReadReg(0x20b8);	// ESR - error status
	if (esr != 0) {
		printf("GEM: GPU error state (ESR=0x%x), cannot exec\n", esr);
		return B_DEV_NOT_READY;
	}

	// Verify ring is enabled
	uint32 ringCtl = _ReadReg(fRing->register_base + RING_BUFFER_CONTROL);
	if (!(ringCtl & 1)) {
		printf("GEM: Ring not enabled (CTL=0x%x)\n", ringCtl);
		return B_DEV_NOT_READY;
	}

	// Align to even number of DWORDs (QWord alignment)
	uint32 totalDW = count;
	if (totalDW & 1)
		totalDW++;

	uint32 needed = totalDW * sizeof(uint32);

	acquire_lock(&fRing->lock);

	// MakeSpace: poll HEAD until enough room (matching QueueCommands)
	bigtime_t start = system_time();
	while (fRing->space_left < needed) {
		uint32 head = _ReadReg(fRing->register_base + RING_BUFFER_HEAD)
			& INTEL_RING_BUFFER_HEAD_MASK;
		if (head <= fRing->position)
			head += fRing->size;
		fRing->space_left = head - fRing->position;
		if (fRing->space_left < needed) {
			if (system_time() > start + 1000000LL) {
				release_lock(&fRing->lock);
				return B_TIMED_OUT;
			}
			snooze(10);
		}
	}

	// Write commands to ring (with wrap-around handling)
	for (uint32 i = 0; i < count; i++)
		_RingWrite(cmds[i]);

	// Pad to QWord alignment
	if (count & 1)
		_RingWrite(0);  // MI_NOOP

	_RingFlush();

	release_lock(&fRing->lock);

	return B_OK;
}


status_t
GemManager::WaitIdle(bigtime_t timeout)
{
	// Wait for ring HEAD to catch up to TAIL
	bigtime_t start = system_time();
	while (true) {
		uint32 head = _ReadReg(fRing->register_base + RING_BUFFER_HEAD)
			& INTEL_RING_BUFFER_HEAD_MASK;
		uint32 tail = _ReadReg(fRing->register_base + RING_BUFFER_TAIL);

		if (head == tail)
			return B_OK;

		if (system_time() > start + timeout)
			return B_TIMED_OUT;

		snooze(100);
	}
}


// GTT allocation is handled by the kernel driver via ioctl.
// No userspace allocator needed.


// ---------------------------------------------------------------
// Ring buffer helpers
// ---------------------------------------------------------------

void
GemManager::_RingWrite(uint32 value)
{
	*(uint32*)(fRing->base + fRing->position) = value;
	fRing->position = (fRing->position + sizeof(uint32))
		& (fRing->size - 1);
	fRing->space_left -= sizeof(uint32);
}


void
GemManager::_RingFlush()
{
	// Memory barrier
	int32 flush = 0;
	atomic_add(&flush, 1);

	// Write TAIL register to submit
	_WriteReg(fRing->register_base + RING_BUFFER_TAIL, fRing->position);
}
