/*
 * GEM-like buffer object manager for Intel Gen5 on Haiku.
 */

#include "GemManager.h"

#include <stdio.h>
#include <string.h>


GemManager::GemManager()
	:
	fSharedInfo(NULL),
	fRegs(NULL),
	fRing(NULL),
	fNextHandle(1),
	fAllocNext(GEM_ALLOC_START),
	fAllocEnd(0),
	fLastSeq(0),
	fScratch(NULL)
{
	memset(fObjects, 0, sizeof(fObjects));
}


GemManager::~GemManager()
{
}


status_t
GemManager::Init(intel_shared_info* sharedInfo, volatile uint8* regs)
{
	fSharedInfo = sharedInfo;
	fRegs = regs;
	fRing = &sharedInfo->primary_ring_buffer;

	// Determine allocatable GTT region.
	// Only GTT offsets with valid entries are usable by the GPU.
	// Known mapped regions (from kernel driver):
	//   0x00000-0x0FFFF: Ring buffer (64KB)
	//   0x10000-0x10FFF: Scratch page
	//   0x11000-0x1FFFF: Free (60KB)
	//   0x20000-0x20FFF: Render state (if allocated)
	//   0x21000-~0x428000: Framebuffer
	// GTT entries after framebuffer are NOT mapped - GPU hangs if accessed!
	//
	// For the prototype, allocate from 0x11000 to 0x20000 (60KB).
	// TODO: use intel_allocate_memory() or map new GTT entries for more space.
	fAllocNext = 0x11000;
	fAllocEnd = 0x20000;

	printf("  GEM: alloc region 0x%x - 0x%x (%.1f MB free)\n",
		fAllocNext, fAllocEnd,
		(float)(fAllocEnd - fAllocNext) / (1024 * 1024));

	// Map scratch page (for fence sequence tracking)
	fScratch = (volatile uint32*)(
		(uint8*)sharedInfo->graphics_memory + GEM_SCRATCH_GTT);

	// Clear scratch
	for (int i = 0; i < 256; i++)
		fScratch[i] = 0;

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

	// Allocate GTT space
	uint32 offset = _AllocGTT(size);
	if (offset == 0)
		return B_NO_MEMORY;

	// Fill in buffer object
	gem_buffer& bo = fObjects[slot];
	bo.handle = fNextHandle++;
	bo.gtt_offset = offset;
	bo.size = size;
	bo.cpu_addr = (addr_t)fSharedInfo->graphics_memory + offset;
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
			_FreeGTT(fObjects[i].gtt_offset, fObjects[i].size);
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
GemManager::ExecBatch(uint32 batchHandle, uint32 batchLen)
{
	uint32 batchOffset = GetOffset(batchHandle);
	if (batchOffset == 0)
		return B_BAD_VALUE;

	// Ensure batch ends with MI_BATCH_BUFFER_END
	void* batchPtr = MapBuffer(batchHandle);
	if (batchPtr == NULL)
		return B_BAD_VALUE;

	// Flush Write-Combining buffers so batch data is visible to GPU.
	// WC writes to GTT aperture aren't flushed by atomic_add on stack.
	// sfence ensures all prior stores (including WC) are globally visible.
	__asm__ __volatile__("sfence" ::: "memory");

	// Increment fence sequence
	uint32 seq = ++fLastSeq;

	// Submit via ring: MI_BATCH_BUFFER_START + fence write
	acquire_lock(&fRing->lock);

	// Check space (need 6 DWORDs: batch_start + store_dword + noop)
	uint32 needed = 8 * sizeof(uint32);
	bigtime_t start = system_time();
	while (fRing->space_left < needed) {
		uint32 head = _ReadReg(fRing->register_base + RING_BUFFER_HEAD)
			& INTEL_RING_BUFFER_HEAD_MASK;
		if (head <= fRing->position)
			head += fRing->size;
		fRing->space_left = head - fRing->position;
		if (fRing->space_left < needed) {
			if (system_time() > start + 1000000LL)
				break;
			snooze(10);
		}
	}

	// MI_BATCH_BUFFER_START (with NON_SECURE bit for Gen4/5)
	_RingWrite(0x18800000 | (1 << 8));
	_RingWrite(batchOffset);	// GTT offset of batch

	// MI_STORE_DWORD_INDEX: write sequence to scratch for fencing
	_RingWrite((0x21 << 23) | 1);	// MI_STORE_DWORD_INDEX
	_RingWrite(0);					// offset 0 in HWS/scratch
	_RingWrite(seq);				// sequence number

	// MI_FLUSH + NOOP for alignment
	_RingWrite(0x02000000);		// MI_FLUSH
	_RingWrite(0);				// MI_NOOP
	_RingWrite(0);				// MI_NOOP (QWord align)

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


// ---------------------------------------------------------------
// GTT allocator (simple bump allocator)
// ---------------------------------------------------------------

uint32
GemManager::_AllocGTT(uint32 size)
{
	if (fAllocNext + size > fAllocEnd)
		return 0;

	uint32 offset = fAllocNext;
	fAllocNext += size;
	return offset;
}


void
GemManager::_FreeGTT(uint32 offset, uint32 size)
{
	// Simple bump allocator: no real free (leak is OK for prototype).
	// A real implementation would use a free list or bitmap.
	(void)offset;
	(void)size;
}


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
