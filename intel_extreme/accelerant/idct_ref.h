/*
 * Copyright 2026, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 *
 * CPU reference for the Gen5 IDCT kernel (Phase 3.1).
 *
 * Implements the same 2-pass separable IDCT as idct_single.g4a, using
 * the same cosine table, rounding constants, and shift widths. Output
 * is verified bit-exact against the GPU — any mismatch means a bug in
 * either the kernel or this reference.
 *
 * The algorithm matches libva-intel-driver's idct.g4i (MIT licensed,
 * original authors: Zou Nan hai, Yan Li, Liu Xi bin at Intel).
 */

#ifndef IDCT_REF_H
#define IDCT_REF_H

#include <SupportDefs.h>

// IDCT cosine coefficients (Q15 fixed-point, scaled by 2^15).
// These are the UNIQUE 8 rows of the transform matrix. The GPU table
// stores each row twice (for SIMD16 {compr} dp4), but the CPU reference
// uses the unique values directly.

#define IDCT_C0 23170
#define IDCT_C1 22725
#define IDCT_C2 21407
#define IDCT_C3 19266
#define IDCT_C4 16383
#define IDCT_C5 12873
#define IDCT_C6  8867
#define IDCT_C7  4520

// The 8x8 cosine matrix for the IDCT. Row j contains the weights for
// computing output position j from input frequencies 0..7.
static const int32 kIdctCosine[8][8] = {
	{  IDCT_C4,  IDCT_C1,  IDCT_C2,  IDCT_C3,  IDCT_C4,  IDCT_C5,  IDCT_C6,  IDCT_C7 },
	{  IDCT_C4,  IDCT_C3,  IDCT_C6, -IDCT_C7, -IDCT_C4, -IDCT_C1, -IDCT_C2, -IDCT_C5 },
	{  IDCT_C4,  IDCT_C5, -IDCT_C6, -IDCT_C1, -IDCT_C4,  IDCT_C7,  IDCT_C2,  IDCT_C3 },
	{  IDCT_C4,  IDCT_C7, -IDCT_C2, -IDCT_C5,  IDCT_C4,  IDCT_C3, -IDCT_C6, -IDCT_C1 },
	{  IDCT_C4, -IDCT_C7, -IDCT_C2,  IDCT_C5,  IDCT_C4, -IDCT_C3, -IDCT_C6,  IDCT_C1 },
	{  IDCT_C4, -IDCT_C5, -IDCT_C6,  IDCT_C1, -IDCT_C4, -IDCT_C7,  IDCT_C2, -IDCT_C3 },
	{  IDCT_C4, -IDCT_C3,  IDCT_C6,  IDCT_C7, -IDCT_C4,  IDCT_C1, -IDCT_C2,  IDCT_C5 },
	{  IDCT_C4, -IDCT_C1,  IDCT_C2, -IDCT_C3,  IDCT_C4, -IDCT_C5,  IDCT_C6, -IDCT_C7 },
};

// The same table in the GPU's duplicated layout (128 int32 values).
// Each row is stored twice (for SIMD16 dp4 with {compr}).
// This is what gets loaded into CURBE g5-g20.
static const int32 kIdctTableGpu[128] = {
	// g5:  cosine row 0
	 IDCT_C4,  IDCT_C1,  IDCT_C2,  IDCT_C3,  IDCT_C4,  IDCT_C5,  IDCT_C6,  IDCT_C7,
	// g6:  cosine row 0 (duplicate)
	 IDCT_C4,  IDCT_C1,  IDCT_C2,  IDCT_C3,  IDCT_C4,  IDCT_C5,  IDCT_C6,  IDCT_C7,
	// g7:  cosine row 1
	 IDCT_C4,  IDCT_C3,  IDCT_C6, -IDCT_C7, -IDCT_C4, -IDCT_C1, -IDCT_C2, -IDCT_C5,
	// g8:  cosine row 1 (duplicate)
	 IDCT_C4,  IDCT_C3,  IDCT_C6, -IDCT_C7, -IDCT_C4, -IDCT_C1, -IDCT_C2, -IDCT_C5,
	// g9:  cosine row 2
	 IDCT_C4,  IDCT_C5, -IDCT_C6, -IDCT_C1, -IDCT_C4,  IDCT_C7,  IDCT_C2,  IDCT_C3,
	// g10: cosine row 2 (duplicate)
	 IDCT_C4,  IDCT_C5, -IDCT_C6, -IDCT_C1, -IDCT_C4,  IDCT_C7,  IDCT_C2,  IDCT_C3,
	// g11: cosine row 3
	 IDCT_C4,  IDCT_C7, -IDCT_C2, -IDCT_C5,  IDCT_C4,  IDCT_C3, -IDCT_C6, -IDCT_C1,
	// g12: cosine row 3 (duplicate)
	 IDCT_C4,  IDCT_C7, -IDCT_C2, -IDCT_C5,  IDCT_C4,  IDCT_C3, -IDCT_C6, -IDCT_C1,
	// g13: cosine row 4
	 IDCT_C4, -IDCT_C7, -IDCT_C2,  IDCT_C5,  IDCT_C4, -IDCT_C3, -IDCT_C6,  IDCT_C1,
	// g14: cosine row 4 (duplicate)
	 IDCT_C4, -IDCT_C7, -IDCT_C2,  IDCT_C5,  IDCT_C4, -IDCT_C3, -IDCT_C6,  IDCT_C1,
	// g15: cosine row 5
	 IDCT_C4, -IDCT_C5, -IDCT_C6,  IDCT_C1, -IDCT_C4, -IDCT_C7,  IDCT_C2, -IDCT_C3,
	// g16: cosine row 5 (duplicate)
	 IDCT_C4, -IDCT_C5, -IDCT_C6,  IDCT_C1, -IDCT_C4, -IDCT_C7,  IDCT_C2, -IDCT_C3,
	// g17: cosine row 6
	 IDCT_C4, -IDCT_C3,  IDCT_C6,  IDCT_C7, -IDCT_C4,  IDCT_C1, -IDCT_C2,  IDCT_C5,
	// g18: cosine row 6 (duplicate)
	 IDCT_C4, -IDCT_C3,  IDCT_C6,  IDCT_C7, -IDCT_C4,  IDCT_C1, -IDCT_C2,  IDCT_C5,
	// g19: cosine row 7
	 IDCT_C4, -IDCT_C1,  IDCT_C2, -IDCT_C3,  IDCT_C4, -IDCT_C5,  IDCT_C6, -IDCT_C7,
	// g20: cosine row 7 (duplicate)
	 IDCT_C4, -IDCT_C1,  IDCT_C2, -IDCT_C3,  IDCT_C4, -IDCT_C5,  IDCT_C6, -IDCT_C7,
};


// Row pass rounding and shift (matches idct.g4i ROW_ADD / ROW_SHIFT).
#define IDCT_ROW_ADD   1024
#define IDCT_ROW_SHIFT 11

// Column pass rounding and shift (matches idct.g4i COL_ADD / COL_SHIFT).
#define IDCT_COL_ADD   524288
#define IDCT_COL_SHIFT 20


/*
 * compute_idct_reference — CPU-side 2-pass IDCT matching the GPU kernel.
 *
 * The GPU data flow (traced from idct_single.g4a + idct.g4i):
 *
 *   Row pass:
 *     For each cosine row j (0..7) and each input row i (0..7):
 *       row_result[j][i] = sum(k=0..7, in[i*8+k] * cosine[j][k])
 *     Round and shift:
 *       row_result[j][i] = (row_result[j][i] + ROW_ADD) >> ROW_SHIFT
 *     Narrow to S16:
 *       narrow[j][i] = (int16)row_result[j][i]
 *
 *   Column pass:
 *     For each cosine row m (0..7) and each column j (0..7):
 *       col_result[m][j] = sum(i=0..7, narrow[j][i] * cosine[m][i])
 *     Round and shift:
 *       col_result[m][j] = (col_result[m][j] + COL_ADD) >> COL_SHIFT
 *     Narrow to S16:
 *       out[m*8+j] = (int16)col_result[m][j]
 *
 *   The row pass naturally transposes the matrix (g80[j] accumulates
 *   all rows' column-j values). The column pass un-transposes it,
 *   producing the final output in row-major order.
 */
static inline void
compute_idct_reference(const int16 in[64], int16 out[64])
{
	// Row pass: row_result[j][i] = dot8(input_row_i, cosine_row_j)
	int32 row_result[8][8];
	for (int j = 0; j < 8; j++) {
		for (int i = 0; i < 8; i++) {
			int32 sum = 0;
			for (int k = 0; k < 8; k++)
				sum += (int32)in[i * 8 + k] * kIdctCosine[j][k];
			// Arithmetic shift right (matches GPU asr instruction).
			row_result[j][i] = (sum + IDCT_ROW_ADD) >> IDCT_ROW_SHIFT;
		}
	}

	// Narrow D -> W (take low 16 bits, matching the GPU mov W <- D).
	int16 narrow[8][8];
	for (int j = 0; j < 8; j++)
		for (int i = 0; i < 8; i++)
			narrow[j][i] = (int16)row_result[j][i];

	// Column pass: col_result[m][j] = dot8(narrow_col_j, cosine_row_m)
	// narrow[j][i] represents column j, row i (transposed from row pass).
	for (int m = 0; m < 8; m++) {
		for (int j = 0; j < 8; j++) {
			int32 sum = 0;
			for (int i = 0; i < 8; i++)
				sum += (int32)narrow[j][i] * kIdctCosine[m][i];
			// Arithmetic shift right.
			int32 result = (sum + IDCT_COL_ADD) >> IDCT_COL_SHIFT;
			out[m * 8 + j] = (int16)result;
		}
	}
}


#endif // IDCT_REF_H
