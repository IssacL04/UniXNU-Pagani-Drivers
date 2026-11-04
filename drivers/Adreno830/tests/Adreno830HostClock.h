/* SPDX-License-Identifier: MIT */
/* Host double of <kern/clock.h> for the orchestrator tests: the test owns the clock. */
#ifndef ADRENO830_HOST_CLOCK_H
#define ADRENO830_HOST_CLOCK_H
#include <cstdint>
extern "C" uint64_t mach_absolute_time(void);
extern "C" void absolutetime_to_nanoseconds(uint64_t abstime,uint64_t *result);
#endif
