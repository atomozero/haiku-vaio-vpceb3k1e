/*
 * gem2d — 2D acceleration through the kernel GEM execbuffer path
 * (RFC section 7.4, milestone M4).
 *
 * When the kernel driver runs with "gem enable", the ring belongs to
 * the kernel and userland ring writes go through a compatibility
 * benaphore bridge. This layer removes the accelerant from the ring
 * entirely: BLT commands are written into a small GEM batch BO and
 * submitted via INTEL_GEM_EXECBUFFER, with fences (seqnos) providing
 * the sync-token semantics.
 *
 * The BLT commands embed absolute GTT addresses of the framebuffer
 * (accelerant-managed graphics memory is pinned at stable offsets
 * below the GEM allocator's range), so submissions carry no
 * relocations.
 */
#ifndef GEM2D_H
#define GEM2D_H


#include <SupportDefs.h>


status_t gem2d_init();
void gem2d_uninit();
bool gem2d_available();

// Batch construction: begin() readies a batch BO for CPU writes,
// add() appends command DWORDs (flushing and restarting transparently
// when the batch fills up), end() submits. Must be called with the
// engine lock held (the accelerant hook context).
void gem2d_begin();
void gem2d_add(const void* commands, size_t size);
void gem2d_end();

// Wait until every gem2d submission has retired.
void gem2d_wait_idle();


#endif	// GEM2D_H
