/*
 * Copyright 2026, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 *
 * Minimal benchmarking primitives for the Gen5 compute-path work.
 *
 * Wraps Haiku's monotonic system_time() (microsecond resolution) and
 * logs formatted durations to the syslog. Intended to be used around
 * GPU dispatch sites to track baseline latencies and catch regressions
 * as the pipeline grows in Phase 2+.
 */


#ifndef BENCH_H
#define BENCH_H


#include <SupportDefs.h>


// Current monotonic time in microseconds. Equivalent to system_time()
// but named to match the bench_* family for readability.
bigtime_t bench_now_us(void);

// Log a named duration: "bench tag: 1234 us (5.03 ms, 198.4 Hz)".
// The Hz column is only shown when us > 0.
void bench_log(const char* tag, bigtime_t duration_us);

// Convenience: log a named duration AND a per-unit rate
// (e.g. "per thread", "per instruction"). Prints total + amortized.
void bench_log_per_unit(const char* tag, bigtime_t duration_us,
	uint32 unit_count, const char* unit_name);


#endif // BENCH_H
