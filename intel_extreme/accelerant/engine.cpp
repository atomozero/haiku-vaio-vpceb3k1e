/*
 * Copyright 2006-2007, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Axel Dörfler, axeld@pinc-software.de
 */


#include <Debug.h>
#include <string.h>
#include <unistd.h>

#include "intel_extreme.h"
#include "accelerant.h"
#include "accelerant_protos.h"
#include "commands.h"
#include "gem2d.h"
#include "render.h"


#undef TRACE
//#define TRACE_ENGINE
#ifdef TRACE_ENGINE
#	define TRACE(x...) _sPrintf("intel_extreme: " x)
#else
#	define TRACE(x...)
#endif

#define ERROR(x...) _sPrintf("intel_extreme: " x)
#define CALLED(x...) TRACE("CALLED %s\n", __PRETTY_FUNCTION__)


static engine_token sEngineToken = {1, 0 /*B_2D_ACCELERATION*/, NULL};
static int32 sLastSubmittedSeq = 0;

// Batch buffer for BLT commands - avoids per-command ring overhead
#define BATCH_BUFFER_SIZE	(64 * 1024)

static addr_t sBatchBase = 0;		// virtual address (WC mapped)
static uint32 sBatchOffset = 0;	// GTT offset for GPU
static uint32 sBatchSize = 0;


QueueCommands::QueueCommands(ring_buffer &ring)
	:
	fRingBuffer(ring)
{
	acquire_lock(&fRingBuffer.lock);
}


QueueCommands::~QueueCommands()
{
	// Emit MI_STORE_DWORD_INDEX to update the sequence number in the
	// Hardware Status Page. Only emit every 8th submission to balance
	// per-command overhead (4 DWORDs) vs sync accuracy.
	uint32 seq = atomic_add(&sLastSubmittedSeq, 1) + 1;
	if ((seq & 0x07) == 0) {
		MakeSpace(4);
		Write(MI_STORE_DWORD_INDEX);
		Write(HWS_SYNC_SEQUENCE_INDEX);
		Write(seq);
		Write(MI_NOOP);
	}

	if (fRingBuffer.position & 0x07) {
		// make sure the command is properly aligned
		Write(COMMAND_NOOP);
	}

	// We must make sure memory is written back in case the ring buffer
	// is in write combining mode - releasing the lock does this, as the
	// buffer is flushed on a locked memory operation (which is what this
	// benaphore does), but it must happen before writing the new tail...
	int32 flush = 0;
	atomic_add(&flush, 1);

	// Write TAIL register: try kernel ioctl first (userspace MMIO is
	// read-only on Ironlake), fall back to direct MMIO for other gens.
	if (gInfo != NULL && gInfo->device >= 0
		&& gInfo->shared_info->device_type.InGroup(INTEL_GROUP_ILK)) {
		intel_ring_tail tailData;
		tailData.magic = INTEL_PRIVATE_DATA_MAGIC;
		tailData.tail_value = fRingBuffer.position;
		ioctl(gInfo->device, INTEL_RING_WRITE_TAIL, &tailData,
			sizeof(tailData));
	} else {
		write32(fRingBuffer.register_base + RING_BUFFER_TAIL,
			fRingBuffer.position);
	}

	release_lock(&fRingBuffer.lock);
}


void
QueueCommands::Put(struct command &command, size_t size)
{
	uint32 count = size / sizeof(uint32);
	uint32 *data = command.Data();

	MakeSpace(count);

	// Fast path: if the command fits without wrapping, use memcpy
	uint32 bytesLeft = fRingBuffer.size - fRingBuffer.position;
	if (bytesLeft >= size) {
		memcpy((uint8*)fRingBuffer.base + fRingBuffer.position, data, size);
		fRingBuffer.position = (fRingBuffer.position + size)
			& (fRingBuffer.size - 1);
	} else {
		// Slow path: write DWORD by DWORD across the wrap boundary
		for (uint32 i = 0; i < count; i++) {
			Write(data[i]);
		}
	}
}


void
QueueCommands::PutFlush()
{
	MakeSpace(2);

	Write(COMMAND_FLUSH);
	Write(COMMAND_NOOP);
}


void
QueueCommands::PutWaitFor(uint32 event)
{
	MakeSpace(2);

	Write(COMMAND_WAIT_FOR_EVENT | event);
	Write(COMMAND_NOOP);
}


void
QueueCommands::PutOverlayFlip(uint32 mode, bool updateCoefficients)
{
	MakeSpace(2);

	Write(COMMAND_OVERLAY_FLIP | mode);

	uint32 registers;
	// G33 does not need a physical address for the overlay registers
	if (intel_uses_physical_overlay(*gInfo->shared_info))
		registers = gInfo->shared_info->physical_overlay_registers;
	else
		registers = gInfo->shared_info->overlay_offset;

	Write(registers | (updateCoefficients ? OVERLAY_UPDATE_COEFFICIENTS : 0));
}


void
QueueCommands::MakeSpace(uint32 size)
{
	ASSERT((size & 1) == 0);

	size *= sizeof(uint32);
	bigtime_t start = system_time();

	while (fRingBuffer.space_left < size) {
		// wait until more space is free
		uint32 head = read32(fRingBuffer.register_base + RING_BUFFER_HEAD)
			& INTEL_RING_BUFFER_HEAD_MASK;

		if (head <= fRingBuffer.position)
			head += fRingBuffer.size;

		fRingBuffer.space_left = head - fRingBuffer.position;

		if (fRingBuffer.space_left < size) {
			if (system_time() > start + 1000000LL) {
				ERROR("engine stalled, head %" B_PRIx32 "\n", head);
				break;
			}
			spin(10);
		}
	}

	fRingBuffer.space_left -= size;
}


void
QueueCommands::Write(uint32 data)
{
	uint32 *target = (uint32 *)(fRingBuffer.base + fRingBuffer.position);
	*target = data;

	fRingBuffer.position = (fRingBuffer.position + sizeof(uint32))
		& (fRingBuffer.size - 1);
}


//	#pragma mark -


void
uninit_ring_buffer(ring_buffer &ringBuffer)
{
	uninit_lock(&ringBuffer.lock);
	write32(ringBuffer.register_base + RING_BUFFER_CONTROL, 0);
}


void
setup_ring_buffer(ring_buffer &ringBuffer, const char* name)
{
	TRACE("Setup ring buffer %s, offset %lx, size %lx\n", name,
		ringBuffer.offset, ringBuffer.size);

	if (init_lock(&ringBuffer.lock, name) < B_OK) {
		// disable ring buffer
		ringBuffer.size = 0;
		return;
	}

	uint32 ring = ringBuffer.register_base;
	ringBuffer.position = 0;
	ringBuffer.space_left = ringBuffer.size;

	write32(ring + RING_BUFFER_TAIL, 0);
	write32(ring + RING_BUFFER_START, ringBuffer.offset);
	write32(ring + RING_BUFFER_CONTROL,
		((ringBuffer.size - B_PAGE_SIZE) & INTEL_RING_BUFFER_SIZE_MASK)
		| INTEL_RING_BUFFER_ENABLED);
}


status_t
init_batch_buffer()
{
	addr_t base;
	if (intel_allocate_memory(BATCH_BUFFER_SIZE, 0, base) != B_OK) {
		ERROR("Failed to allocate batch buffer\n");
		return B_NO_MEMORY;
	}

	sBatchBase = base;
	sBatchOffset = base - (addr_t)gInfo->shared_info->graphics_memory;
	sBatchSize = BATCH_BUFFER_SIZE;

	memset((void*)base, 0, BATCH_BUFFER_SIZE);
	TRACE("Batch buffer: base %p, GTT offset 0x%" B_PRIx32 ", size 0x%"
		B_PRIx32 "\n", (void*)base, sBatchOffset, sBatchSize);
	return B_OK;
}


void
uninit_batch_buffer()
{
	if (sBatchBase != 0) {
		intel_free_memory(sBatchBase);
		sBatchBase = 0;
	}
}


//	#pragma mark - BatchCommands


BatchCommands::BatchCommands(ring_buffer &ring)
	:
	fRing(ring),
	fPosition(0)
{
}


BatchCommands::~BatchCommands()
{
	if (sBatchBase == 0 || fPosition == 0)
		return;

	// Write sequence number to HWS page
	uint32 seq = atomic_add(&sLastSubmittedSeq, 1) + 1;
	WriteBatch(MI_STORE_DWORD_INDEX);
	WriteBatch(HWS_SYNC_SEQUENCE_INDEX);
	WriteBatch(seq);
	WriteBatch(MI_NOOP);

	// End the batch
	WriteBatch(MI_BATCH_BUFFER_END);
	if (fPosition & 0x07)
		WriteBatch(MI_NOOP);

	// Memory barrier: ensure batch writes are visible to GPU
	int32 flush = 0;
	atomic_add(&flush, 1);

	// Submit batch via ring: MI_BATCH_BUFFER_START + GTT address
	QueueCommands queue(fRing);
	queue.MakeSpace(2);
	queue.Write(MI_BATCH_BUFFER_START);
	queue.Write(sBatchOffset);
}


void
BatchCommands::Put(struct command &command, size_t size)
{
	// Check if batch has enough space (leave room for trailer)
	if (fPosition + size + 32 > sBatchSize) {
		// Batch full - shouldn't happen with 64KB buffer
		ERROR("Batch buffer full!\n");
		return;
	}

	memcpy((uint8*)sBatchBase + fPosition, command.Data(), size);
	fPosition += size;
}


void
BatchCommands::WriteBatch(uint32 data)
{
	*(uint32*)((uint8*)sBatchBase + fPosition) = data;
	fPosition += sizeof(uint32);
}


bool
batch_buffer_available()
{
	return sBatchBase != 0;
}


//	#pragma mark - engine management


/*! Return number of hardware engines */
uint32
intel_accelerant_engine_count(void)
{
	CALLED();
	return 1;
}


status_t
intel_acquire_engine(uint32 capabilities, uint32 maxWait, sync_token* syncToken,
	engine_token** _engineToken)
{
	CALLED();
	static bool sFirstAcquire = true;
	if (sFirstAcquire) {
		_sPrintf("intel_extreme: intel_acquire_engine CALLED\n");
		sFirstAcquire = false;
	}
	*_engineToken = &sEngineToken;

	if (acquire_lock(&gInfo->shared_info->engine_lock) != B_OK)
		return B_ERROR;

	if (syncToken)
		intel_sync_to_token(syncToken);

	return B_OK;
}


status_t
intel_release_engine(engine_token* engineToken, sync_token* syncToken)
{
	CALLED();
	if (syncToken != NULL)
		syncToken->engine_id = engineToken->engine_id;

	release_lock(&gInfo->shared_info->engine_lock);
	return B_OK;
}


void
intel_wait_engine_idle(void)
{
	CALLED();

	// Skylake acc engine not yet functional (stalls)
	if (gInfo->shared_info->device_type.InFamily(INTEL_FAMILY_LAKE)
			|| gInfo->shared_info->device_type.InFamily(INTEL_FAMILY_SOC0)) {
		return;
	}

	// M4: 2D work goes through GEM fences; wait those first. The legacy
	// checks below still cover ring users left on the bridge (overlay).
	if (gem2d_available())
		gem2d_wait_idle();

	// Fast path: check sequence number in Hardware Status Page.
	// The seq is only updated every 8th submission to reduce overhead,
	// so we round target down to the nearest multiple of 8.
	uint32 target = (uint32)sLastSubmittedSeq & ~0x07;
	hardware_status* hws =
		(hardware_status*)gInfo->shared_info->status_page;

	if (hws != NULL && target > 0
		&& hws->store[HWS_SYNC_SEQUENCE_INDEX] >= target) {
		// GPU already past our target - no wait needed
		return;
	}

	// Slow path: poll ring HEAD == TAIL
	{
		QueueCommands queue(gInfo->shared_info->primary_ring_buffer);
		queue.PutFlush();
	}

	bigtime_t start = system_time();

	ring_buffer &ring = gInfo->shared_info->primary_ring_buffer;
	uint32 head, tail;
	while (true) {
		head = read32(ring.register_base + RING_BUFFER_HEAD)
			& INTEL_RING_BUFFER_HEAD_MASK;
		tail = read32(ring.register_base + RING_BUFFER_TAIL)
			& INTEL_RING_BUFFER_HEAD_MASK;

		if (head == tail)
			break;

		if (system_time() > start + 1000000LL) {
			ERROR("engine locked up, head %" B_PRIx32 "!\n", head);
			break;
		}

		spin(10);
	}
}


status_t
intel_get_sync_token(engine_token* engineToken, sync_token* syncToken)
{
	CALLED();
	return B_OK;
}


status_t
intel_sync_to_token(sync_token* syncToken)
{
	CALLED();
	intel_wait_engine_idle();
	return B_OK;
}


//	#pragma mark - engine acceleration


void
intel_screen_to_screen_blit(engine_token* token, blit_params* params,
	uint32 count)
{
	if (gem2d_available()) {
		// M4: submit through the kernel GEM execbuffer instead of the ring
		gem2d_begin();
		xy_source_blit_command blit;
		for (uint32 i = 0; i < count; i++) {
			blit.source_left = params[i].src_left;
			blit.source_top = params[i].src_top;
			blit.dest_left = params[i].dest_left;
			blit.dest_top = params[i].dest_top;
			blit.dest_right = params[i].dest_left + params[i].width + 1;
			blit.dest_bottom = params[i].dest_top + params[i].height + 1;
			gem2d_add(blit.Data(), sizeof(blit));
		}
		gem2d_end();
		return;
	}

	QueueCommands queue(gInfo->shared_info->primary_ring_buffer);
	xy_source_blit_command blit;
	for (uint32 i = 0; i < count; i++) {
		blit.source_left = params[i].src_left;
		blit.source_top = params[i].src_top;
		blit.dest_left = params[i].dest_left;
		blit.dest_top = params[i].dest_top;
		blit.dest_right = params[i].dest_left + params[i].width + 1;
		blit.dest_bottom = params[i].dest_top + params[i].height + 1;

		// For overlapping regions where dest is below/right of source,
		// the GPU handles this correctly with XY_SRC_COPY_BLT as long
		// as source and dest use the same base address and pitch.
		// The hardware processes scanlines top-to-bottom, so vertical
		// overlap downward needs no special handling on Intel BLT.

		queue.Put(blit, sizeof(blit));
	}
}


// Render engine toggle: checked once, enabled by trigger file.
// If the 3D pipeline hangs the GPU, reboot and delete the file.
static int sRenderMode = -1;  // -1 = unchecked, 0 = BLT, 1 = render

static void
check_render_mode()
{
	if (sRenderMode != -1)
		return;

	bool isILK = gInfo->shared_info->device_type.InGroup(INTEL_GROUP_ILK);
	int fileOK = access("/boot/home/config/settings/render_test", F_OK);
	_sPrintf("intel_extreme: check_render_mode: ILK=%d, file=%d\n",
		isILK, fileOK);

	if (isILK && fileOK == 0) {
		sRenderMode = 1;
		_sPrintf("intel_extreme: 3D render engine ENABLED for fills "
			"(delete /boot/home/config/settings/render_test to disable)\n");
	} else {
		sRenderMode = 0;
	}
}


void
intel_fill_rectangle(engine_token* token, uint32 color,
	fill_rect_params* params, uint32 count)
{
	static bool sFirstCall = true;
	if (sFirstCall) {
		_sPrintf("intel_extreme: intel_fill_rectangle CALLED "
			"(count=%" B_PRIu32 ", color=0x%08" B_PRIx32 ")\n",
			count, color);
		sFirstCall = false;
	}

	check_render_mode();

	if (sRenderMode == 1) {
		for (uint32 i = 0; i < count; i++) {
			render_fill_rect(color,
				params[i].left, params[i].top,
				params[i].right + 1, params[i].bottom + 1);
		}
		return;
	}

	if (gem2d_available()) {
		gem2d_begin();
		xy_color_blit_command blit(false);
		blit.color = color;
		for (uint32 i = 0; i < count; i++) {
			blit.dest_left = params[i].left;
			blit.dest_top = params[i].top;
			blit.dest_right = params[i].right + 1;
			blit.dest_bottom = params[i].bottom + 1;
			gem2d_add(blit.Data(), sizeof(blit));
		}
		gem2d_end();
		return;
	}

	QueueCommands queue(gInfo->shared_info->primary_ring_buffer);
	xy_color_blit_command blit(false);
	blit.color = color;
	for (uint32 i = 0; i < count; i++) {
		blit.dest_left = params[i].left;
		blit.dest_top = params[i].top;
		blit.dest_right = params[i].right + 1;
		blit.dest_bottom = params[i].bottom + 1;
		queue.Put(blit, sizeof(blit));
	}
}


void
intel_invert_rectangle(engine_token* token, fill_rect_params* params,
	uint32 count)
{
	if (gem2d_available()) {
		gem2d_begin();
		xy_color_blit_command blit(true);
		blit.color = 0xffffffff;
		for (uint32 i = 0; i < count; i++) {
			blit.dest_left = params[i].left;
			blit.dest_top = params[i].top;
			blit.dest_right = params[i].right + 1;
			blit.dest_bottom = params[i].bottom + 1;
			gem2d_add(blit.Data(), sizeof(blit));
		}
		gem2d_end();
		return;
	}

	QueueCommands queue(gInfo->shared_info->primary_ring_buffer);
	xy_color_blit_command blit(true);
	blit.color = 0xffffffff;
	for (uint32 i = 0; i < count; i++) {
		blit.dest_left = params[i].left;
		blit.dest_top = params[i].top;
		blit.dest_right = params[i].right + 1;
		blit.dest_bottom = params[i].bottom + 1;
		queue.Put(blit, sizeof(blit));
	}
}


void
intel_fill_span(engine_token* token, uint32 color, uint16* _params,
	uint32 count)
{
	struct params {
		uint16	top;
		uint16	left;
		uint16	right;
	} *params = (struct params*)_params;

	if (gem2d_available()) {
		// The scanline blits depend on the preceding setup command, so
		// emit them in chunks that are guaranteed to share one batch.
		xy_setup_mono_pattern_command setup;
		setup.background_color = color;
		setup.pattern = 0;

		const uint32 kChunk = 1024;
		for (uint32 base = 0; base < count; base += kChunk) {
			uint32 chunkEnd = base + kChunk < count ? base + kChunk : count;
			gem2d_begin();
			gem2d_add(setup.Data(), sizeof(setup));
			xy_scanline_blit_command blit;
			for (uint32 i = base; i < chunkEnd; i++) {
				blit.dest_left = params[i].left;
				blit.dest_top = params[i].top;
				blit.dest_right = params[i].right;
				blit.dest_bottom = params[i].top;
				gem2d_add(blit.Data(), sizeof(blit));
			}
			gem2d_end();
		}
		return;
	}

	QueueCommands queue(gInfo->shared_info->primary_ring_buffer);

	xy_setup_mono_pattern_command setup;
	setup.background_color = color;
	setup.pattern = 0;
	queue.Put(setup, sizeof(setup));

	xy_scanline_blit_command blit;

	for (uint32 i = 0; i < count; i++) {
		blit.dest_left = params[i].left;
		blit.dest_top = params[i].top;
		blit.dest_right = params[i].right;
		blit.dest_bottom = params[i].top;

		queue.Put(blit, sizeof(blit));
	}
}
