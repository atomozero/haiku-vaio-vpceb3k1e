/*
 * Copyright 2026, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 *
 * CPU reference for MPEG-2 intra macroblock inverse quantization, used
 * as the bit-exact target for the Gen5 iq_intra kernel (Phase 2.3a).
 *
 * This reference mirrors the behaviour of libva-intel-driver's
 * shaders/mpeg2/vld/iq_intra.g4i + do_iq_intra.g4i on a per-instruction
 * level, NOT the full MPEG-2 §7.4 specification. The kernel omits
 * saturation (§7.4.4) and mismatch control (§7.4.3); those happen later
 * in libva's decode pipeline. Our reference matches the kernel exactly
 * so the GPU output can be compared bit-for-bit.
 *
 * Bit layout of `flags` matches what iq_intra.g4i reads from g82.8 UW:
 *   [4:0]   q_scale_code        (1..31 per MPEG-2; 0 reserved, produces 0)
 *   [5]     q_scale_type        (0 = linear, 1 = non-linear)
 *   [14:13] intra_dc_precision  (0..3 selecting 8..11 bit DC)
 */


#ifndef IQ_INTRA_REF_H
#define IQ_INTRA_REF_H


#include <stdint.h>


static inline void
iq_intra_ref(int16_t out[6 * 64], const int16_t in[6 * 64],
	const uint8_t qmat[64], uint16_t flags)
{
	const int q_scale_code       = flags & 0x1f;
	const int q_scale_type       = (flags >> 5) & 1;
	const int intra_dc_precision = (flags >> 13) & 3;

	// q_scale computation mirrors the 4-way branch in iq_intra.g4i:
	//  - linear:       q_scale = q_scale_code * 2
	//  - non-linear 1..8:   q_scale = q_scale_code (no transform)
	//  - non-linear 9..16:  q_scale = (q-9)*2 + 10  -> 10,12,14,16,18,20,22,24
	//  - non-linear 17..24: q_scale = (q-17)*4 + 28 -> 28,32,36,40,44,48,52,56
	//  - non-linear 25..31: q_scale = (q-25)*8 + 64 -> 64,72,80,88,96,104,112
	// q_scale_code == 0 is reserved; the kernel's q<9 branch falls
	// through with q_scale = 0, zeroing every AC coefficient. We match.
	int q_scale;
	if (q_scale_type == 0)
		q_scale = q_scale_code * 2;
	else if (q_scale_code < 9)
		q_scale = q_scale_code;
	else if (q_scale_code < 17)
		q_scale = (q_scale_code - 9) * 2 + 10;
	else if (q_scale_code < 25)
		q_scale = (q_scale_code - 17) * 4 + 28;
	else
		q_scale = (q_scale_code - 25) * 8 + 64;

	// intra_dc_mult per iq_intra.g4i: 0->8, 1->4, 2->2, 3->1.
	const int intra_dc_mult = 8 >> intra_dc_precision;

	for (int b = 0; b < 6; b++) {
		for (int i = 0; i < 64; i++) {
			const int32_t c = in[b * 64 + i];
			int32_t r;
			if (i == 0) {
				// DC path: kernel does
				//   mul(1) g116.0.D = g111.W × g109.4.UW
				// 32-bit product of the 16-bit DC coeff by the small
				// multiplier; the later D->W narrowing keeps low 16 bits.
				r = c * intra_dc_mult;
			} else {
				// AC path: kernel does
				//   mul(16) g116.D = coeff.W × qmat.UW
				//   mul(16) g116.D = g116.D × q_scale.UW
				//   asr(16) g116.D = g116.D >> 4
				// Equivalent to spec (QF*W*q*2)/32 = (c*qmat*q_scale)>>4.
				// Max |c|≈2047, qmat≤255, q_scale≤112 -> product ≤ 58M,
				// fits in signed int32. Signed '>>' on gcc/x86 is arithmetic
				// shift (asr), matching the kernel exactly. We cannot use
				// '/ 16' because on negatives C rounds toward zero while
				// the kernel rounds toward -infinity.
				r = (c * (int32_t)qmat[i] * q_scale) >> 4;
			}
			// D -> W narrowing in iq_intra.g4i is done via a <16,8,2>W
			// stride-2 region that picks the low 16 bits of each D.
			// In C, that's an int16_t cast with two's-complement wrap.
			out[b * 64 + i] = (int16_t)r;
		}
	}
}


#endif // IQ_INTRA_REF_H
