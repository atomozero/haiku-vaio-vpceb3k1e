/*
 * Copyright 2026, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 *
 * Pipeline-agnostic GPU instrumentation for the Gen5 driver.
 *
 * Provides register readback (ACTHD, INSTDONE, IPEHR, EIR, ring state),
 * decoded interpretation helpers, batch-marker DWORD builders, and BO
 * hex-dump helpers. Used by the media-pipeline bring-up to diagnose
 * kernel hangs and state corruption.
 *
 * All operations are read-only on the GPU side except for emitting
 * MI_STORE_DATA_IMM markers, which is a pure diagnostic write that
 * targets a caller-supplied GTT address.
 */


#ifndef GPU_DEBUG_H
#define GPU_DEBUG_H


#include <SupportDefs.h>

#include "gpu_bo.h"


// MMIO offsets of the Gen5 render-engine debug registers. Raw offsets
// against the MMIO BAR, used via the REGS_FLAT block-0 path in read32().
#define GPU_REG_INSTDONE	0x206C	// per-stage "done" bits
#define GPU_REG_IPEHR		0x2068	// instruction parser: last hazard header
#define GPU_REG_ACTHD		0x2074	// actual head (DWORD being executed)
#define GPU_REG_ESR			0x20B8	// error status register
#define GPU_REG_EIR			0x20B0	// error identity register (latched)
#define GPU_REG_EMR			0x20B4	// error mask register
#define GPU_REG_INSTPM		0x20C0	// instruction parser mode
#define GPU_REG_PGTBL_ER	0x2024	// page-table error

// Read a raw MMIO register at a given BAR offset (bypasses block encoding).
uint32 gpu_debug_read_mmio(uint32 offset);

// Dump the full set of render-engine debug registers to the syslog with
// a caller-supplied tag (e.g. "before batch submit", "after timeout").
// The tag is written on its own line for easy log grep.
void gpu_debug_dump_registers(const char* tag);

// Decode INSTDONE into a human-readable string of "stage_name" tokens for
// stages whose "done" bit is CLEARED (i.e. still busy / stalled).
// The output buffer must be at least 128 bytes. Never allocates.
void gpu_debug_decode_instdone(uint32 instdone, char* out, size_t out_size);

// Decode the IPEHR register into (pipeline, opcode, sub_opcode) and a best-
// effort command name if we know it. Output buffer at least 64 bytes.
void gpu_debug_decode_ipehr(uint32 ipehr, char* out, size_t out_size);

// Read the ring buffer HEAD, TAIL, and CTL from the primary render ring.
// Values returned by reference. Use this alongside ACTHD to tell whether
// the ring itself has advanced past a given command.
void gpu_debug_read_ring(uint32* head, uint32* tail, uint32* ctl);

// Hex-dump 'dwords' consecutive DWORDs from a BO starting at 'offset' to
// the syslog, 4 per line with the GTT offset as a prefix. Useful for
// inspecting state objects or partially-executed batch buffers post-hang.
void gpu_debug_hexdump_bo(const gpu_bo* bo, uint32 offset, uint32 dwords);

// Build the 4 DWORDs of an MI_STORE_DATA_IMM (via GGTT) command that will
// write 'tag' to the given 'gtt_offset' when executed by the ring. Writes
// into the caller-supplied array; caller is responsible for queueing the
// 4 DWORDs into whichever batch / ring command stream they maintain.
//
// Layout per existing render.cpp marker pattern:
//   DW0 = (0x20 << 23) | (1 << 22) | 2
//   DW1 = 0
//   DW2 = gtt_offset
//   DW3 = tag
void gpu_debug_marker_dwords(uint32 dwords[4], uint32 gtt_offset, uint32 tag);

// Poll the given 32-bit memory location, waiting for it to equal 'expected'
// or for 'timeout_us' microseconds to elapse. Returns true if the value was
// observed before timeout, false on timeout. Uses snooze() between reads.
bool gpu_debug_wait_value(volatile uint32* addr, uint32 expected,
	uint32 timeout_us);


#endif // GPU_DEBUG_H
