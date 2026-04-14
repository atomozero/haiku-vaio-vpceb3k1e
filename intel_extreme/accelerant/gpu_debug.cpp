/*
 * Copyright 2026, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 *
 * Pipeline-agnostic GPU instrumentation — implementation.
 *
 * All output goes directly through _sPrintf(). We deliberately do not use
 * libroot snprintf/sprintf here: an earlier revision of this file called
 * snprintf() from within intel_init_accelerant() and crashed on a PLT
 * lazy-binding failure (see analysis/PHASE_I_B_FIRST_CRASH_ANALYSIS for
 * details). Keeping formatting limited to the format strings _sPrintf
 * understands has no such issue because the existing accelerant already
 * uses _sPrintf heavily and the PLT slot is resolved the same way.
 */


#include "gpu_debug.h"

#include <string.h>

#include <OS.h>

#include "accelerant.h"
#include "intel_extreme.h"


#define LOG(x...) _sPrintf("intel_extreme gpu_debug: " x)


uint32
gpu_debug_read_mmio(uint32 offset)
{
	return *(volatile uint32*)((uint8*)gInfo->registers + offset);
}


void
gpu_debug_read_ring(uint32* head, uint32* tail, uint32* ctl)
{
	uint32 base = gInfo->shared_info->primary_ring_buffer.register_base;
	if (head != NULL)
		*head = read32(base + RING_BUFFER_HEAD) & INTEL_RING_BUFFER_HEAD_MASK;
	if (tail != NULL)
		*tail = read32(base + RING_BUFFER_TAIL);
	if (ctl != NULL)
		*ctl = read32(base + RING_BUFFER_CONTROL);
}


// Gen5 INSTDONE bit meanings. Bit CLEARED = stage not done (busy/stalled).
// Derived from i915_reg.h (Linux) and brw_defines.h (Mesa) for gen5.
struct instdone_bit {
	uint32 mask;
	const char* name;
};


static const instdone_bit kInstdoneBits[] = {
	{ 1u << 0,  "ROW_0"  },
	{ 1u << 1,  "ROW_1"  },
	{ 1u << 2,  "VF"     },
	{ 1u << 3,  "VS"     },
	{ 1u << 4,  "GS"     },
	{ 1u << 5,  "CLIP"   },
	{ 1u << 6,  "SF"     },
	{ 1u << 7,  "WIZ"    },
	{ 1u << 8,  "IS"     },
	{ 1u << 9,  "IC"     },
	{ 1u << 10, "MAP"    },
	{ 1u << 11, "MTP"    },
	{ 1u << 12, "MAS"    },
	{ 1u << 13, "PSD"    },
	{ 1u << 14, "PS"     },
	{ 1u << 15, "DM"     },
	{ 1u << 16, "GW"     },
	{ 1u << 17, "URB"    },
	{ 1u << 18, "RCC"    },
	{ 1u << 19, "RCPB"   },
	{ 1u << 20, "RCPFE"  },
	{ 1u << 21, "RCFBC"  },
	{ 1u << 22, "RCZ"    },
	{ 1u << 23, "MTL"    },
	{ 1u << 24, "MTM"    },
	{ 1u << 25, "GAM"    },
};


static void
log_instdone_stalled(uint32 instdone)
{
	if (instdone == 0xffffffffu) {
		LOG("  INSTDONE=0xffffffff  stalled: (all done)\n");
		return;
	}
	LOG("  INSTDONE=0x%08" B_PRIx32 "  stalled:\n", instdone);
	for (size_t i = 0; i < sizeof(kInstdoneBits) / sizeof(kInstdoneBits[0]); i++) {
		if ((instdone & kInstdoneBits[i].mask) == 0)
			LOG("    [bit %u] %s\n", (unsigned)i, kInstdoneBits[i].name);
	}
}


// Best-effort command name for a parser hazard header. Gen5 encoding:
//   [31:29] = type (0=MI, 2=2D/BLT, 3=GFX/3D/Media)
//   [28:27] = pipeline sub-selector (only meaningful when type=3)
//   [26:24] = op field (common) or high bits of op for 3D
//   [23:16] = sub_op
// For GFX commands the op value is (pipeline<<3 | low_op), matching the
// CMD(p, o, s) macro which stores p in bits [28:27] and o in [26:24].
static const char*
ipehr_cmd_name(uint32 type, uint32 pipeline, uint32 op, uint32 sub_op)
{
	if (type == 0) {
		// MI commands: op is bits [28:23] of the header.
		switch (op) {
			case 0x00: return (sub_op == 0) ? "MI_NOOP" : "MI_*";
			case 0x02: return "MI_USER_INTERRUPT";
			case 0x04: return "MI_FLUSH";
			case 0x05: return "MI_BATCH_BUFFER_END";
			case 0x0a: return "MI_ARB_ON_OFF";
			case 0x20: return "MI_STORE_DATA_IMM";
			case 0x21: return "MI_STORE_DATA_INDEX";
			case 0x31: return "MI_BATCH_BUFFER_START";
			default:   return "MI_?";
		}
	}
	if (type == 2) {
		switch (op) {
			case 0x40: return "COLOR_BLT";
			case 0x50: return "XY_COLOR_BLT";
			case 0x53: return "XY_SRC_COPY_BLT";
			default:   return "2D_?";
		}
	}
	if (type == 3) {
		// Pipeline 0: common gfx commands.
		if (pipeline == 0) {
			if (op == 0) {
				if (sub_op == 0x00) return "URB_FENCE";
				if (sub_op == 0x01) return "CS_URB_STATE";
				if (sub_op == 0x02) return "CONSTANT_BUFFER";
				return "COMMON_0_?";
			}
			if (op == 1) {
				if (sub_op == 0x01) return "STATE_BASE_ADDRESS";
				if (sub_op == 0x02) return "STATE_SIP";
				return "COMMON_1_?";
			}
			return "COMMON_?";
		}
		// Pipeline 1: single-DW gfx commands (PIPELINE_SELECT live qui).
		if (pipeline == 1) {
			if (op == 1 && sub_op == 0x04) return "PIPELINE_SELECT";
			return "SINGLE_DW_?";
		}
		// Pipeline 2: media commands.
		if (pipeline == 2) {
			if (op == 0) {
				if (sub_op == 0x00) return "MEDIA_STATE_POINTERS";
				return "MEDIA_0_?";
			}
			if (op == 1) {
				if (sub_op == 0x00) return "MEDIA_OBJECT";
				if (sub_op == 0x01) return "MEDIA_OBJECT_EX";
				if (sub_op == 0x03) return "MEDIA_OBJECT_WALKER";
				return "MEDIA_1_?";
			}
			return "MEDIA_?";
		}
		// Pipeline 3: 3D commands.
		if (pipeline == 3) {
			if (op == 1 && sub_op == 0x05) return "3DSTATE_DEPTH_BUFFER";
			if (op == 0 && sub_op == 0x18) return "3DPRIMITIVE";
			return "3D_?";
		}
	}
	return "UNKNOWN";
}


static void
log_ipehr(uint32 ipehr)
{
	uint32 type     = (ipehr >> 29) & 0x7;
	uint32 pipeline = (ipehr >> 27) & 0x3;
	uint32 op       = (ipehr >> 24) & 0x7;
	uint32 sub_op   = (ipehr >> 16) & 0xff;
	LOG("  IPEHR=0x%08" B_PRIx32 "  type=%u pipe=%u op=0x%02x sub=0x%02x (%s)\n",
		ipehr, (unsigned)type, (unsigned)pipeline,
		(unsigned)op, (unsigned)sub_op,
		ipehr_cmd_name(type, pipeline, op, sub_op));
}


void
gpu_debug_dump_registers(const char* tag)
{
	uint32 instdone = gpu_debug_read_mmio(GPU_REG_INSTDONE);
	uint32 ipehr    = gpu_debug_read_mmio(GPU_REG_IPEHR);
	uint32 acthd    = gpu_debug_read_mmio(GPU_REG_ACTHD);
	uint32 eir      = gpu_debug_read_mmio(GPU_REG_EIR);
	uint32 esr      = gpu_debug_read_mmio(GPU_REG_ESR);
	uint32 emr      = gpu_debug_read_mmio(GPU_REG_EMR);
	uint32 pgtbl_er = gpu_debug_read_mmio(GPU_REG_PGTBL_ER);
	uint32 head, tail, ctl;
	gpu_debug_read_ring(&head, &tail, &ctl);

	LOG("--- %s ---\n", tag ? tag : "state");
	log_instdone_stalled(instdone);
	log_ipehr(ipehr);
	LOG("  ACTHD=0x%08" B_PRIx32 "\n", acthd);
	LOG("  EIR=0x%08" B_PRIx32 "  ESR=0x%08" B_PRIx32
		"  EMR=0x%08" B_PRIx32 "\n", eir, esr, emr);
	LOG("  PGTBL_ER=0x%08" B_PRIx32 "\n", pgtbl_er);
	LOG("  RING: HEAD=0x%08" B_PRIx32 "  TAIL=0x%08" B_PRIx32
		"  CTL=0x%08" B_PRIx32 "%s\n", head, tail, ctl,
		(ctl & INTEL_RING_BUFFER_ENABLED) ? "" : "  (DISABLED)");
}


// Backwards-compatible no-snprintf versions. These no longer build a
// composite string into a buffer — they log directly. The out/out_size
// parameters are retained for ABI stability but the buffer is unused
// except to hold a fixed-width "(see log)" marker.
void
gpu_debug_decode_instdone(uint32 instdone, char* out, size_t out_size)
{
	if (out != NULL && out_size > 0) {
		const char* msg = "(see log)";
		size_t len = strlen(msg);
		if (len >= out_size)
			len = out_size - 1;
		memcpy(out, msg, len);
		out[len] = '\0';
	}
	log_instdone_stalled(instdone);
}


void
gpu_debug_decode_ipehr(uint32 ipehr, char* out, size_t out_size)
{
	if (out != NULL && out_size > 0) {
		const char* msg = "(see log)";
		size_t len = strlen(msg);
		if (len >= out_size)
			len = out_size - 1;
		memcpy(out, msg, len);
		out[len] = '\0';
	}
	log_ipehr(ipehr);
}


void
gpu_debug_hexdump_bo(const gpu_bo* bo, uint32 offset, uint32 dwords)
{
	if (bo == NULL || !bo->valid) {
		LOG("hexdump: BO is null or invalid\n");
		return;
	}
	if (offset >= bo->size) {
		LOG("hexdump: offset %u beyond BO size %u\n", offset, bo->size);
		return;
	}

	uint32 max_dwords = (bo->size - offset) / 4;
	if (dwords > max_dwords)
		dwords = max_dwords;

	LOG("hexdump %s @ gtt=0x%x+%u (%u dwords):\n",
		bo->name ? bo->name : "(unnamed)", bo->gtt_offset, offset, dwords);

	volatile uint32* p = (volatile uint32*)(bo->cpu_addr + offset);
	uint32 i = 0;
	while (i + 4 <= dwords) {
		LOG("  0x%08x: %08" B_PRIx32 " %08" B_PRIx32
			" %08" B_PRIx32 " %08" B_PRIx32 "\n",
			bo->gtt_offset + offset + i * 4,
			p[i], p[i + 1], p[i + 2], p[i + 3]);
		i += 4;
	}
	// Tail: up to 3 remaining DWORDs, one line per DWORD to keep the
	// format simple and avoid string concatenation.
	for (; i < dwords; i++) {
		LOG("  0x%08x: %08" B_PRIx32 "\n",
			bo->gtt_offset + offset + i * 4, p[i]);
	}
}


void
gpu_debug_marker_dwords(uint32 dwords[4], uint32 gtt_offset, uint32 tag)
{
	dwords[0] = (0x20u << 23) | (1u << 22) | 2u;
	dwords[1] = 0;
	dwords[2] = gtt_offset;
	dwords[3] = tag;
}


bool
gpu_debug_wait_value(volatile uint32* addr, uint32 expected, uint32 timeout_us)
{
	if (addr == NULL)
		return false;

	// Fast path: busy-wait for up to 2 ms using system_time() deltas.
	// Haiku's scheduler quantum makes snooze() unusable for sub-ms GPU
	// dispatches — a single snooze(200) easily becomes 10-60 ms wait,
	// which destroys benchmark timing resolution. For Gen5 pure-EOT
	// dispatches the GPU completes in microseconds, so busy-waiting
	// briefly is both fast and fair.
	const bigtime_t spin_budget_us = 2000;
	bigtime_t t0 = system_time();
	while ((system_time() - t0) < spin_budget_us) {
		if (*addr == expected)
			return true;
	}

	// Slow path: if the dispatch is taking more than a couple of ms,
	// fall back to snooze() to avoid burning CPU. Larger kernels (real
	// video decode, matmul) will end up here.
	bigtime_t elapsed = system_time() - t0;
	const uint32 poll_interval_us = 500;
	while (elapsed < (bigtime_t)timeout_us) {
		if (*addr == expected)
			return true;
		snooze(poll_interval_us);
		elapsed = system_time() - t0;
	}
	return *addr == expected;
}
