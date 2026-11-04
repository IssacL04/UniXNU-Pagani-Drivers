/* SPDX-License-Identifier: MIT */
/* Test controls of tests/A830HostSched.cpp, the thread-based host double of A830Sched.h (authored, not XNU code). */
#ifndef A830_HOST_SCHED_H
#define A830_HOST_SCHED_H
#include "A830Sched.h"
#include <cstdint>
#include <thread>

/* thread_abort_safely() on a thread: a wait it is blocked in (or its next assert_wait) ends with THREAD_INTERRUPTED. */
void a830HostAbort(std::thread::id thread);
/* Threads currently waiting (asserted, not yet woken) on event. */
unsigned a830HostWaiting(void *event);
/* Thread calls: manual mode runs nothing by itself; a830HostRunCalls runs every pending call whose deadline passed (or all
 * pending ones with ignore_deadline) once, in the caller's thread, and returns how many ran. */
void a830HostManualCalls(bool manual);
unsigned a830HostRunCalls(bool ignore_deadline);
/* Pending/started state and run count of one call. */
bool a830HostCallPending(A830Call *call);
uint64_t a830HostCallRuns(A830Call *call);
/* Wait until no threaded call is pending with an expired deadline or running (bounded by timeout_ms); true when idle. */
bool a830HostCallsIdle(unsigned timeout_ms);
/* Stop and join every call worker (end of a test program). */
void a830HostShutdown(void);
/* Total a830DelayUs microseconds requested (the top half's AHB fence poll). */
uint64_t a830HostDelayed(void);
#endif
