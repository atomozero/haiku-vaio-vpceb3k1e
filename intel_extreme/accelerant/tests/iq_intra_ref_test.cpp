/*
 * Copyright 2026, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 *
 * Standalone CPU-only test for iq_intra_ref(). Validates that the
 * reference function produces hand-computed expected outputs for a
 * series of small, easy-to-verify inputs. Run before using the
 * reference as the bit-exact oracle for the Gen5 iq_intra kernel
 * (Phase 2.3a).
 *
 * Build and run:
 *   cd intel_extreme/accelerant
 *   g++ -std=c++17 -O2 -Wall -I. -o tests/iq_intra_ref_test \
 *       tests/iq_intra_ref_test.cpp
 *   ./tests/iq_intra_ref_test
 *
 * Exits 0 on full pass, 1 on any mismatch (with details).
 */


#include "../iq_intra_ref.h"

#include <cstdio>
#include <cstdint>
#include <cstring>


namespace {

struct test_case {
	const char*		name;
	int16_t			in[6 * 64];
	uint8_t			qmat[64];
	uint16_t		flags;
	// Sparse expected-output list: each entry says "at index idx,
	// element should be val". Untouched indices are expected to be 0.
	struct { int idx; int16_t val; } expected[16];
	int				expected_count;
};


// Helper to build a flags word from its three fields.
static uint16_t
make_flags(int q_scale_code, int q_scale_type, int intra_dc_precision)
{
	return (uint16_t)((q_scale_code & 0x1f)
		| ((q_scale_type & 1) << 5)
		| ((intra_dc_precision & 3) << 13));
}


// A qmat filled with a single constant value.
static void
fill_qmat(uint8_t qmat[64], uint8_t v)
{
	for (int i = 0; i < 64; i++)
		qmat[i] = v;
}


// Clear 384 input coefficients.
static void
clear_in(int16_t in[6 * 64])
{
	memset(in, 0, 6 * 64 * sizeof(int16_t));
}


// Run one test case. Returns true on pass, prints and returns false
// on the first mismatch encountered.
static bool
run_one(const test_case& tc)
{
	int16_t out[6 * 64];
	memset(out, 0xAA, sizeof(out));   // sentinel so unfilled slots show

	iq_intra_ref(out, tc.in, tc.qmat, tc.flags);

	// Build a full expected-output buffer from the sparse list.
	int16_t expected[6 * 64];
	memset(expected, 0, sizeof(expected));
	for (int k = 0; k < tc.expected_count; k++)
		expected[tc.expected[k].idx] = tc.expected[k].val;

	for (int i = 0; i < 6 * 64; i++) {
		if (out[i] != expected[i]) {
			printf("  FAIL '%s' at idx %d: got %d, expected %d\n",
				tc.name, i, (int)out[i], (int)expected[i]);
			return false;
		}
	}
	printf("  PASS '%s'\n", tc.name);
	return true;
}

} // anonymous namespace


int
main(void)
{
	int pass = 0;
	int fail = 0;

	// --------------------------------------------------------------
	// Test 1: all-zero input, any qmat, any flags -> all-zero output
	// --------------------------------------------------------------
	{
		test_case tc = {};
		tc.name = "all zeros";
		clear_in(tc.in);
		fill_qmat(tc.qmat, 16);
		tc.flags = make_flags(/*q*/1, /*type*/0, /*prec*/0);
		tc.expected_count = 0;   // all zero expected
		(run_one(tc) ? pass : fail)++;
	}

	// --------------------------------------------------------------
	// Test 2: DC only, precision 0 -> DC * 8
	// in[0]=100 -> out[0]=800
	// --------------------------------------------------------------
	{
		test_case tc = {};
		tc.name = "DC only precision=0";
		clear_in(tc.in);
		tc.in[0] = 100;
		fill_qmat(tc.qmat, 16);
		tc.flags = make_flags(/*q*/1, /*type*/0, /*prec*/0);
		tc.expected[0] = { 0, 800 };
		tc.expected_count = 1;
		(run_one(tc) ? pass : fail)++;
	}

	// --------------------------------------------------------------
	// Test 3: DC only, precision 3 -> DC * 1 (unchanged)
	// --------------------------------------------------------------
	{
		test_case tc = {};
		tc.name = "DC only precision=3";
		clear_in(tc.in);
		tc.in[0] = 100;
		fill_qmat(tc.qmat, 16);
		tc.flags = make_flags(/*q*/1, /*type*/0, /*prec*/3);
		tc.expected[0] = { 0, 100 };
		tc.expected_count = 1;
		(run_one(tc) ? pass : fail)++;
	}

	// --------------------------------------------------------------
	// Test 4: single AC coeff, linear q_scale
	// in[1]=100, qmat[1]=10, q_scale_code=1 linear (q_scale=2)
	// out[1] = (100 * 10 * 2) >> 4 = 2000 >> 4 = 125
	// --------------------------------------------------------------
	{
		test_case tc = {};
		tc.name = "AC single linear q=2";
		clear_in(tc.in);
		tc.in[1] = 100;
		fill_qmat(tc.qmat, 10);
		tc.flags = make_flags(/*q*/1, /*type*/0, /*prec*/0);
		tc.expected[0] = { 1, 125 };
		tc.expected_count = 1;
		(run_one(tc) ? pass : fail)++;
	}

	// --------------------------------------------------------------
	// Test 5: negative AC coeff -> arithmetic shift right (rounds
	// toward -infinity, NOT toward 0 like C integer divide).
	// in[1]=-5, qmat[1]=1, q_scale_code=1 linear (q_scale=2)
	// out[1] = (-5 * 1 * 2) >> 4 = -10 >> 4 = -1   (asr)
	//         (-10 / 16 = 0 in C — different! This test catches it.)
	// --------------------------------------------------------------
	{
		test_case tc = {};
		tc.name = "AC negative asr not divide";
		clear_in(tc.in);
		tc.in[1] = -5;
		fill_qmat(tc.qmat, 1);
		tc.flags = make_flags(/*q*/1, /*type*/0, /*prec*/0);
		tc.expected[0] = { 1, -1 };
		tc.expected_count = 1;
		(run_one(tc) ? pass : fail)++;
	}

	// --------------------------------------------------------------
	// Test 6: non-linear q_scale, range 9..16
	// q_scale_code=10, type=1 -> q_scale = (10-9)*2 + 10 = 12
	// in[1]=100, qmat[1]=8
	// out[1] = (100 * 8 * 12) >> 4 = 9600 >> 4 = 600
	// --------------------------------------------------------------
	{
		test_case tc = {};
		tc.name = "non-linear range 9..16 q=10";
		clear_in(tc.in);
		tc.in[1] = 100;
		fill_qmat(tc.qmat, 8);
		tc.flags = make_flags(/*q*/10, /*type*/1, /*prec*/0);
		tc.expected[0] = { 1, 600 };
		tc.expected_count = 1;
		(run_one(tc) ? pass : fail)++;
	}

	// --------------------------------------------------------------
	// Test 7: non-linear q_scale, range 17..24
	// q_scale_code=20, type=1 -> q_scale = (20-17)*4 + 28 = 40
	// in[1]=10, qmat[1]=4
	// out[1] = (10 * 4 * 40) >> 4 = 1600 >> 4 = 100
	// --------------------------------------------------------------
	{
		test_case tc = {};
		tc.name = "non-linear range 17..24 q=20";
		clear_in(tc.in);
		tc.in[1] = 10;
		fill_qmat(tc.qmat, 4);
		tc.flags = make_flags(/*q*/20, /*type*/1, /*prec*/0);
		tc.expected[0] = { 1, 100 };
		tc.expected_count = 1;
		(run_one(tc) ? pass : fail)++;
	}

	// --------------------------------------------------------------
	// Test 8: non-linear q_scale, range 25..31
	// q_scale_code=28, type=1 -> q_scale = (28-25)*8 + 64 = 88
	// in[1]=10, qmat[1]=16
	// out[1] = (10 * 16 * 88) >> 4 = 14080 >> 4 = 880
	// --------------------------------------------------------------
	{
		test_case tc = {};
		tc.name = "non-linear range 25..31 q=28";
		clear_in(tc.in);
		tc.in[1] = 10;
		fill_qmat(tc.qmat, 16);
		tc.flags = make_flags(/*q*/28, /*type*/1, /*prec*/0);
		tc.expected[0] = { 1, 880 };
		tc.expected_count = 1;
		(run_one(tc) ? pass : fail)++;
	}

	// --------------------------------------------------------------
	// Test 9: q_scale_code=0, linear. Kernel produces q_scale=0 which
	// zeroes every AC coefficient. Input AC values come out as 0.
	// --------------------------------------------------------------
	{
		test_case tc = {};
		tc.name = "q_scale_code=0 zeroes AC";
		clear_in(tc.in);
		tc.in[1] = 1000;          // big value to make sure zeroing happens
		tc.in[63] = 2000;
		fill_qmat(tc.qmat, 255);
		tc.flags = make_flags(/*q*/0, /*type*/0, /*prec*/0);
		tc.expected_count = 0;    // all zero
		(run_one(tc) ? pass : fail)++;
	}

	// --------------------------------------------------------------
	// Test 10: DC wrap on overflow
	// in[0]=10000, precision=0 -> 10000 * 8 = 80000
	// 80000 in hex = 0x13880, low 16 bits = 0x3880 = 14464
	// int16(14464) = 14464 (fits in int16 range)
	// --------------------------------------------------------------
	{
		test_case tc = {};
		tc.name = "DC wrap positive";
		clear_in(tc.in);
		tc.in[0] = 10000;
		fill_qmat(tc.qmat, 16);
		tc.flags = make_flags(/*q*/1, /*type*/0, /*prec*/0);
		tc.expected[0] = { 0, 14464 };
		tc.expected_count = 1;
		(run_one(tc) ? pass : fail)++;
	}

	// --------------------------------------------------------------
	// Test 11: DC wrap negative
	// in[0]=-30000, precision=0 -> -30000 * 8 = -240000
	// -240000 mod 65536 = 22144, int16(22144) = 22144
	// --------------------------------------------------------------
	{
		test_case tc = {};
		tc.name = "DC wrap negative input";
		clear_in(tc.in);
		tc.in[0] = -30000;
		fill_qmat(tc.qmat, 16);
		tc.flags = make_flags(/*q*/1, /*type*/0, /*prec*/0);
		tc.expected[0] = { 0, 22144 };
		tc.expected_count = 1;
		(run_one(tc) ? pass : fail)++;
	}

	// --------------------------------------------------------------
	// Test 12: multiple blocks DC populated
	// in[b*64] = b*10 for b = 0..5, precision=0
	// out[b*64] = b*10 * 8 = b*80
	// --------------------------------------------------------------
	{
		test_case tc = {};
		tc.name = "6 blocks DC populated";
		clear_in(tc.in);
		for (int b = 0; b < 6; b++)
			tc.in[b * 64] = (int16_t)(b * 10);
		fill_qmat(tc.qmat, 16);
		tc.flags = make_flags(/*q*/1, /*type*/0, /*prec*/0);
		for (int b = 0; b < 6; b++)
			tc.expected[b] = { b * 64, (int16_t)(b * 80) };
		tc.expected_count = 6;
		(run_one(tc) ? pass : fail)++;
	}

	// --------------------------------------------------------------
	// Test 13: interaction of DC and AC in same block
	// in[0]=50, in[1]=100, qmat[1]=16, q_scale_code=1 linear (q=2)
	// out[0] = 50 * 8 = 400          (DC path, no qmat, no q_scale)
	// out[1] = (100*16*2) >> 4 = 3200 >> 4 = 200
	// --------------------------------------------------------------
	{
		test_case tc = {};
		tc.name = "DC and AC in same block";
		clear_in(tc.in);
		tc.in[0] = 50;
		tc.in[1] = 100;
		fill_qmat(tc.qmat, 16);
		tc.flags = make_flags(/*q*/1, /*type*/0, /*prec*/0);
		tc.expected[0] = { 0, 400 };
		tc.expected[1] = { 1, 200 };
		tc.expected_count = 2;
		(run_one(tc) ? pass : fail)++;
	}

	printf("\n%d passed, %d failed\n", pass, fail);
	return (fail == 0) ? 0 : 1;
}
