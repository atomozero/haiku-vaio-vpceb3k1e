/*
 * GPU-accelerated IDCT for standalone MPEG-2 decoder.
 *
 * Opens the Intel GPU device directly (shared ring buffer with
 * app_server's accelerant), allocates GTT memory, and dispatches
 * IDCT compute kernels on the Gen5 EU array via MEDIA_OBJECT.
 *
 * Thread-safe: uses the same benaphore lock as the accelerant's
 * ring buffer submissions.
 */

#ifndef GPU_IDCT_H
#define GPU_IDCT_H

#include <SupportDefs.h>

// Maximum blocks per batch dispatch. Higher = better throughput
// but more latency before results are available.
#define GPU_IDCT_MAX_BATCH 400

// Initialize GPU access. Opens /dev/graphics/intel_extreme/0,
// clones shared_info and register areas, allocates GTT BOs for
// kernel + CURBE + input + output. Returns B_OK on success.
status_t gpu_idct_init(void);

// Release all GPU resources and close device.
void gpu_idct_uninit(void);

// Check if GPU IDCT is available (init succeeded).
bool gpu_idct_available(void);

// Batch IDCT: process 'count' blocks of 64 S16 coefficients each.
// Output is 64 S16 IDCT results per block (caller adds MC ref + clamps).
// count must be <= GPU_IDCT_MAX_BATCH.
// Returns B_OK if all blocks were processed.
status_t gpu_idct_process(const int16 blocks[][64], int16 output[][64],
	uint32 count);

#endif // GPU_IDCT_H
