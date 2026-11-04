/* SPDX-License-Identifier: MIT */
#ifndef NATIVE_ADRENO_KOPT_H
#define NATIVE_ADRENO_KOPT_H
#include <stdint.h>

/* pagani-gpu-kopt=<bits> (ARCHITECTURE 3.6): one bit per kernel-side latency change of 2026-10-31, all on unless the boot argument says
 * otherwise, so that a change can be turned back off to compare it on the device; 0 is the behaviour before them, meter line included.
 * S0 refuses other bits and stores the value (NativeAdrenoMemory::setKopt) before anything reads it; it never changes afterwards. */
enum : uint32_t {
    NativeAdrenoKoptPoolZero=1u<<0,     /* a pooled BO is zeroed after the Memory lock is dropped, not under it */
    NativeAdrenoKoptContiguous=1u<<1,   /* a BO outside the pool asks for a contiguous run only up to 128 KiB, and backs off after a failure */
    NativeAdrenoKoptAliasOnce=1u<<2,    /* a map that is not exclusive takes preflight's alias pass: the table transaction does not repeat it */
    NativeAdrenoKoptRuns=1u<<3,         /* cache maintenance through a kernel mapping pins the CPU for 64 pages at a time, not the whole range */
    NativeAdrenoKoptMeter=1u<<4,        /* the longest call and lock wait, for the meter line (ARCHITECTURE 8.8) */
    NativeAdrenoKoptVmClock=1u<<5,      /* the VM's phase counters run: native_adreno_vm_now_us is set at publication */
    NativeAdrenoKoptAll=(1u<<6)-1u,
};
extern "C" volatile uint32_t native_adreno_kopt;
static inline bool NativeAdrenoKoptOn(uint32_t bit) {return (native_adreno_kopt&bit)!=0;}
#endif
