// SPDX-License-Identifier: BSD-2-Clause-Patent
// The clock of the CPU running this code (RamoopsLog's heartbeat, "cpu <MHz>MHz on <cpu>"), measured like the UEFI CpuPerfDxe's
// LoopMhz: a chain of adds where every add waits for the one before it (one add per cycle on this SoC's cores), timed by the architected
// counter, so adds per microsecond is the clock in MHz. CPU_CLOCK_WINDOWS windows of at least 1/CPU_CLOCK_PER_SECOND s each (125 us) and
// the best of them: an interrupt or a preemption only spoils its own window; about 1 ms of work in all. A throttled or parked-low core
// shows a lower number. Integer only (the kernel builds this with -mgeneral-regs-only), no memset or memcpy. The counter is the caller's
// (cntvct_el0 after an isb in the kernel; the host tests pass a fake one, or the real counter on an arm64 host).
#ifndef PAGANI_CPU_CLOCK_H
#define PAGANI_CPU_CLOCK_H
#include <stdint.h>

enum {
    CPU_CLOCK_WINDOWS = 8,          // windows per measurement; the fastest counts
    CPU_CLOCK_PER_SECOND = 8000,    // a window lasts at least 1/8000 s of the counter
    CPU_CLOCK_CHUNK = 512,          // iterations of eight dependent adds between two counter reads (4096 adds)
    CPU_CLOCK_CHUNKS_MAX = 1024,    // at most this many chunks per window (a counter that does not move ends it)
    CPU_CLOCK_ADDS = 8 * CPU_CLOCK_CHUNK,
};

// Adds per microsecond from `adds` adds over `ticks` of a counter at `hz`, rounded to the nearest; 0 without time. The callers keep
// adds below 2^32 and hz is a 32-bit register: the product fits.
static inline uint32_t cpu_clock_rate(uint64_t adds, uint64_t ticks, uint64_t hz)
{
    if (!ticks || !hz) return 0;
    const uint64_t rate = (adds * hz + ticks * 500000) / (ticks * 1000000);
    return rate > UINT32_MAX ? UINT32_MAX : (uint32_t)rate;
}

// One chunk: CPU_CLOCK_CHUNK iterations of eight adds, each on the result of the one before.
static inline uint64_t cpu_clock_chunk(uint64_t value)
{
    for (unsigned i = 0; i < CPU_CLOCK_CHUNK; i++) {
#if defined(__aarch64__)
        __asm__ volatile("add %0, %0, #1\n\tadd %0, %0, #1\n\tadd %0, %0, #1\n\tadd %0, %0, #1\n\t"
                         "add %0, %0, #1\n\tadd %0, %0, #1\n\tadd %0, %0, #1\n\tadd %0, %0, #1" : "+r"(value));
#else
        __asm__ volatile("" : "+r"(value));
        value += 8;
#endif
    }
    return value;
}

// The best window's rate in MHz on this CPU; `counter` reads a counter running at `hz`. 0 without a counter frequency.
static inline uint32_t cpu_clock_measure(uint64_t (*counter)(void), uint64_t hz)
{
    uint32_t best = 0;
    uint64_t value = 0;
    if (!hz || !counter) return 0;
    const uint64_t span = hz / CPU_CLOCK_PER_SECOND ? hz / CPU_CLOCK_PER_SECOND : 1;
    for (unsigned w = 0; w < CPU_CLOCK_WINDOWS; w++) {
        const uint64_t start = counter();
        uint64_t end = start, chunks = 0;
        do {
            value = cpu_clock_chunk(value);
            chunks++;
            end = counter();
        } while (end - start < span && chunks < CPU_CLOCK_CHUNKS_MAX);
        const uint32_t rate = cpu_clock_rate(chunks * CPU_CLOCK_ADDS, end - start, hz);
        if (rate > best) best = rate;
    }
    return best;
}
#endif
