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

	// Check for fatal GPU error (bit 0 = instruction error / ring hang).
	// Non-fatal errors (e.g. bit 4 from bad 2D command) don't block.
	uint32 esr = _ReadReg(0x20b8);	// ESR - error status
	if (esr & 0x01) {
		printf("GEM: GPU fatal error (ESR=0x%x), ring may be hung\n", esr);
		return B_DEV_NOT_READY;
	}

	// Verify ring is enabled
	uint32 ringCtl = _ReadReg(fRing->register_base + RING_BUFFER_CONTROL);
	if (!(ringCtl & 1)) {
		printf("GEM: Ring not enabled (CTL=0x%x)\n", ringCtl);
		return B_DEV_NOT_READY;
	}

	// Add MI_FLUSH before and after commands to serialize the
	// pipeline. Without this, XY_COLOR_BLT and other 2D commands
	// are silently dropped on Gen5.
	uint32 totalDW = count + 4;	// +2 MI_FLUSH/NOOP before, +2 after
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

	// MI_FLUSH before commands (serializes pipeline for 2D BLT)
	_RingWrite(0x02000000);	// MI_FLUSH
	_RingWrite(0x00000000);	// MI_NOOP (pad to qword)

	// Write commands to ring (with wrap-around handling)
	for (uint32 i = 0; i < count; i++)
		_RingWrite(cmds[i]);

	// MI_FLUSH after commands
	_RingWrite(0x02000000);	// MI_FLUSH
	_RingWrite(0x00000000);	// MI_NOOP

	// Pad to QWord alignment
	if (totalDW & 1)
		_RingWrite(0);  // MI_NOOP

	_RingFlush();

	release_lock(&fRing->lock);

	return B_OK;
}


// Gen5 MI_BATCH_BUFFER_START: tells GPU to execute commands
// directly from a GEM buffer. The batch must end with
// MI_BATCH_BUFFER_END (0x0A000000).
//
// DW0: (0x31 << 23) | 0  — opcode, GGTT address space, length=0
// DW1: GTT offset of batch buffer (QWORD aligned)
//
// MI_BATCH_BUFFER_START/END from intel_extreme.h
// MI_BATCH_BUFFER_END = (0x0A << 23) = 0x05000000, NOT 0x0A000000!

status_t
GemManager::ExecBatch(uint32 handle, uint32 usedBytes)
{
	if (usedBytes == 0)
		return B_BAD_VALUE;

	// Find the buffer
	gem_buffer* bo = NULL;
	for (int i = 0; i < GEM_MAX_OBJECTS; i++) {
		if (fObjects[i].in_use && fObjects[i].handle == handle) {
			bo = &fObjects[i];
			break;
		}
	}
	if (bo == NULL)
		return B_BAD_VALUE;

	// Batch address must be QWORD aligned
	uint32 batchAddr = bo->gtt_offset;
	if (batchAddr & 0x7) {
		printf("GEM: batch address 0x%x not QWORD aligned\n", batchAddr);
		return B_NOT_ALLOWED;
	}

	// Check for fatal GPU error
	uint32 esr = _ReadReg(0x20b8);
	if (esr & 0x01) {
		printf("GEM: GPU fatal error (ESR=0x%x)\n", esr);
		return B_DEV_NOT_READY;
	}

	// Verify ring is enabled
	uint32 ringCtl = _ReadReg(fRing->register_base + RING_BUFFER_CONTROL);
	if (!(ringCtl & 1))
		return B_DEV_NOT_READY;

	// Ensure batch ends with MI_BATCH_BUFFER_END
	uint32* batchMem = (uint32*)bo->cpu_addr;
	uint32 lastDW = usedBytes / sizeof(uint32);
	if (lastDW > 0 && batchMem[lastDW - 1] != MI_BATCH_BUFFER_END) {
		// Append MI_BATCH_BUFFER_END
		batchMem[lastDW] = MI_BATCH_BUFFER_END;
		usedBytes += sizeof(uint32);
		// Pad to QWORD
		if ((usedBytes / sizeof(uint32)) & 1) {
			batchMem[lastDW + 1] = 0;	// MI_NOOP
			usedBytes += sizeof(uint32);
		}
	}

	// Memory barrier: ensure batch contents are visible to GPU
	__asm__ __volatile__("mfence" ::: "memory");

	// Emit into ring: MI_FLUSH + MI_BATCH_BUFFER_START + MI_FLUSH
	// Total: 6 DWORDs
	uint32 needed = 6 * sizeof(uint32);

	acquire_lock(&fRing->lock);

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

	_RingWrite(0x02000000);				// MI_FLUSH
	_RingWrite(0x00000000);				// MI_NOOP
	_RingWrite(MI_BATCH_BUFFER_START);	// start batch
	_RingWrite(batchAddr);				// GTT offset
	_RingWrite(0x02000000);				// MI_FLUSH
	_RingWrite(0x00000000);				// MI_NOOP

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
