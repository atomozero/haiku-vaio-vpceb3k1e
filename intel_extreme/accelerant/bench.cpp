/*
 * Copyright 2026, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 *
 * Minimal benchmarking primitives — implementation.
 */


#include "bench.h"

#include <OS.h>

#include "accelerant.h"


#define LOG(x...) _sPrintf("intel_extreme bench: " x)


bigtime_t
bench_now_us(void)
{
	return system_time();
}


void
bench_log(const char* tag, bigtime_t duration_us)
{
	if (tag == NULL)
		tag = "(unnamed)";

	// Compute milliseconds with one decimal place without floating point.
	bigtime_t ms_whole = duration_us / 1000;
	bigtime_t ms_tenth = (duration_us / 100) % 10;

	if (duration_us > 0) {
		// Frequency in Hz = 1,000,000 / duration_us. We present it as
		// whole number with one decimal, avoiding libm's cos/sin pull-in.
		bigtime_t hz_whole = 1000000 / duration_us;
		bigtime_t rem = 1000000 - hz_whole * duration_us;
		bigtime_t hz_tenth = (rem * 10) / duration_us;
		LOG("%s: %lld us (%lld.%lld ms, %lld.%lld Hz)\n",
			tag, (long long)duration_us,
			(long long)ms_whole, (long long)ms_tenth,
			(long long)hz_whole, (long long)hz_tenth);
	} else {
		LOG("%s: %lld us\n", tag, (long long)duration_us);
	}
}


void
bench_log_per_unit(const char* tag, bigtime_t duration_us,
	uint32 unit_count, const char* unit_name)
{
	if (tag == NULL)
		tag = "(unnamed)";
	if (unit_name == NULL)
		unit_name = "unit";

	if (unit_count == 0) {
		bench_log(tag, duration_us);
		return;
	}

	bigtime_t per_unit = duration_us / unit_count;
	LOG("%s: %lld us total, %u %ss, %lld us/%s\n",
		tag, (long long)duration_us, (unsigned)unit_count, unit_name,
		(long long)per_unit, unit_name);
}
