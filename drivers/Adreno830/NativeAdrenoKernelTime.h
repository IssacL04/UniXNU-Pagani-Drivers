/* SPDX-License-Identifier: MIT */
#ifndef NATIVE_ADRENO_KERNEL_TIME_H
#define NATIVE_ADRENO_KERNEL_TIME_H
#include <IOKit/IOTypes.h>
#include <stdint.h>

/* Real kernel time and CPU services for the GMU sequences (A830GmuKernelOs): monotonic microseconds from
 * mach_absolute_time, waits, the CPU generic counter and interrupt masking. This is the only production implementation of
 * these callbacks; host tests inject their own. Every function ignores the cookie except the int-returning pair, which
 * keeps the older callback contract (nonzero cookie or null output → 1). */
struct NativeAdrenoKernelTime {
    static int now_us(void *,uint64_t *);
    /* IODelay: spins with interrupts or preemption off, and blocks the thread (timer wakeup) for long intervals otherwise. */
    static int wait_us(void *,unsigned);
    /* Thread context only: gives the CPU away for about `microseconds` (delay_for_interval). */
    static void sleep_us(unsigned microseconds);
    /* Busy wait, any context (machine-level delay; no blocking). */
    static void spin_us(unsigned microseconds);
    /* Monotonic microseconds. */
    static uint64_t monotonic_us();
    /* CPU generic timer count, the base of mach_absolute_time (19.2 MHz on SM8750): the CX timer seed source. */
    static uint64_t counter();
    /* Interrupts off on this CPU; returns the previous state for interruptsRestore. */
    static uintptr_t interruptsOff();
    static void interruptsRestore(uintptr_t state);
};
#endif
