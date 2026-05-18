/*
 * gpu_ring.h — Generic Intel GPU ring buffer submission layer.
 *
 * Provides ring buffer management via kernel ioctl for any Intel GPU
 * generation on Haiku OS. Userspace MMIO writes are silently ignored
 * on this platform, so all TAIL register writes go through the kernel
 * driver's INTEL_RING_WRITE_TAIL ioctl.
 *
 * This layer is generation-independent: it handles ring position
 * tracking, command writing, TAIL kicks, and idle waits. The caller
 * is responsible for encoding generation-specific GPU commands.
 *
 * Usage:
 *   gpu_ring ring;
 *   gpu_ring_init(&ring);              // sync with hardware
 *   gpu_ring_begin(&ring, 8);          // reserve 8 DWORDs
 *   gpu_ring_emit(&ring, cmd0);        // write commands
 *   gpu_ring_emit(&ring, cmd1);
 *   ...
 *   gpu_ring_advance(&ring);           // pad + kick TAIL
 *   gpu_ring_wait_idle(&ring, 50000);  // wait for GPU
 */

#ifndef GPU_RING_H
#define GPU_RING_H

#include <SupportDefs.h>


struct gpu_ring {
	uint32*		base;			// CPU pointer to ring memory
	uint32		size;			// ring size in bytes
	uint32		pos;			// current write position (bytes)
	uint32		emit_start;		// position at begin() (for advance)
	uint32		emit_count;		// DWORDs emitted since begin()
	uint32		emit_reserved;	// DWORDs reserved by begin()
	uint32		reg_base;		// register base for HEAD/TAIL reads
	volatile uint8*	regs;		// MMIO register mapping (for reads)
	int			device_fd;		// kernel device fd for ioctls
};


// Initialize the ring: sync position with hardware TAIL.
// Must be called once after opening the device and cloning shared_info.
// Never resets the ring — disable+re-enable kills the CS on Ironlake.
status_t	gpu_ring_init(gpu_ring* ring, int device_fd);

// Reserve space for 'dwords' DWORDs in the ring.
// Handles wrap-around by waiting for the GPU to drain.
// Returns B_OK if space is available, B_TIMED_OUT if GPU is stuck.
status_t	gpu_ring_begin(gpu_ring* ring, uint32 dwords);

// Emit a single DWORD to the ring (must be between begin/advance).
void		gpu_ring_emit(gpu_ring* ring, uint32 value);

// Finalize the command sequence: pad to QWord alignment and kick TAIL.
status_t	gpu_ring_advance(gpu_ring* ring);

// Wait for the GPU to become idle (HEAD == TAIL).
// Returns true if idle within timeout, false on timeout.
bool		gpu_ring_wait_idle(gpu_ring* ring, uint32 timeout_us);

// Read current hardware HEAD position.
uint32		gpu_ring_read_head(gpu_ring* ring);

// Submit a pre-built array of DWORDs (convenience wrapper).
// Equivalent to begin + N×emit + advance.
status_t	gpu_ring_submit(gpu_ring* ring, const uint32* dwords, uint32 count);


#endif // GPU_RING_H
