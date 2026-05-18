/*
 * gpu_ring.cpp — Generic Intel GPU ring buffer submission layer.
 *
 * Works on any Intel GPU generation where the Haiku kernel driver
 * provides INTEL_RING_WRITE_TAIL ioctl. Uses its own register pointer
 * (no dependency on accelerant's gInfo global).
 */

#include "gpu_ring.h"

#include <string.h>
#include <OS.h>

#include "intel_extreme.h"


// Read a 32-bit register via the ring's own MMIO mapping
static inline uint32
ring_reg_read(gpu_ring* ring, uint32 offset)
{
	return *(volatile uint32*)(ring->regs + ring->reg_base + offset);
}


status_t
gpu_ring_init(gpu_ring* ring, int device_fd)
{
	if (ring == NULL)
		return B_BAD_VALUE;

	memset(ring, 0, sizeof(gpu_ring));
	ring->device_fd = device_fd;

	// Get shared_info via ioctl + clone
	intel_get_private_data privdata;
	privdata.magic = INTEL_PRIVATE_DATA_MAGIC;
	if (ioctl(device_fd, INTEL_GET_PRIVATE_DATA, &privdata,
			sizeof(privdata)) != 0)
		return B_ERROR;

	intel_shared_info* shared;
	area_id shared_area = clone_area("gpu_ring shared",
		(void**)&shared, B_ANY_ADDRESS,
		B_READ_AREA | B_WRITE_AREA, privdata.shared_info_area);
	if (shared_area < B_OK)
		return B_ERROR;

	uint8* regs;
	area_id regs_area = clone_area("gpu_ring regs",
		(void**)&regs, B_ANY_ADDRESS,
		B_READ_AREA, shared->registers_area);
	if (regs_area < B_OK) {
		delete_area(shared_area);
		return B_ERROR;
	}

	ring_buffer& hw = shared->primary_ring_buffer;
	ring->base = (uint32*)hw.base;
	ring->size = hw.size;
	ring->reg_base = hw.register_base;
	ring->regs = regs;

	// Sync with hardware TAIL
	uint32 hwTail = ring_reg_read(ring, RING_BUFFER_TAIL)
		& (ring->size - 1);
	ring->pos = hwTail;

	// Also sync the shared_info for other ring users
	hw.position = hwTail;
	hw.space_left = hw.size - 64;

	// We keep the areas alive for the lifetime of the ring.
	// In a real driver these would be tracked and cleaned up.

	return B_OK;
}


status_t
gpu_ring_begin(gpu_ring* ring, uint32 dwords)
{
	uint32 bytes_needed = (dwords + 1) * sizeof(uint32);

	if (ring->pos + bytes_needed + 64 > ring->size) {
		bigtime_t deadline = system_time() + 500000;
		while (system_time() < deadline) {
			uint32 head = ring_reg_read(ring, RING_BUFFER_HEAD)
				& INTEL_RING_BUFFER_HEAD_MASK;
			uint32 tail = ring_reg_read(ring, RING_BUFFER_TAIL)
				& (ring->size - 1);
			if (head == tail)
				break;
			snooze(100);
		}
		ring->pos = 0;
	}

	ring->emit_start = ring->pos;
	ring->emit_count = 0;
	ring->emit_reserved = dwords;
	return B_OK;
}


void
gpu_ring_emit(gpu_ring* ring, uint32 value)
{
	uint32 dw_index = ring->pos / sizeof(uint32);
	uint32 mask = (ring->size / sizeof(uint32)) - 1;
	ring->base[dw_index & mask] = value;
	ring->pos = (ring->pos + sizeof(uint32)) & (ring->size - 1);
	ring->emit_count++;
}


status_t
gpu_ring_advance(gpu_ring* ring)
{
	if (ring->pos & 0x07)
		gpu_ring_emit(ring, 0);

	asm volatile("mfence" ::: "memory");

	intel_ring_tail tail;
	tail.magic = INTEL_PRIVATE_DATA_MAGIC;
	tail.tail_value = ring->pos;
	if (ioctl(ring->device_fd, INTEL_RING_WRITE_TAIL, &tail,
			sizeof(tail)) != 0)
		return B_ERROR;

	return B_OK;
}


bool
gpu_ring_wait_idle(gpu_ring* ring, uint32 timeout_us)
{
	bigtime_t deadline = system_time() + timeout_us;
	while (system_time() < deadline) {
		uint32 head = ring_reg_read(ring, RING_BUFFER_HEAD)
			& INTEL_RING_BUFFER_HEAD_MASK;
		uint32 tail = ring_reg_read(ring, RING_BUFFER_TAIL)
			& (ring->size - 1);
		if (head == tail)
			return true;
	}
	return false;
}


uint32
gpu_ring_read_head(gpu_ring* ring)
{
	return ring_reg_read(ring, RING_BUFFER_HEAD)
		& INTEL_RING_BUFFER_HEAD_MASK;
}


status_t
gpu_ring_submit(gpu_ring* ring, const uint32* dwords, uint32 count)
{
	status_t st = gpu_ring_begin(ring, count);
	if (st != B_OK)
		return st;

	for (uint32 i = 0; i < count; i++)
		gpu_ring_emit(ring, dwords[i]);

	return gpu_ring_advance(ring);
}
