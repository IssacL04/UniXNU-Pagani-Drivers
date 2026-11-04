// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test of StallProbe.h (the M and Q records of the ramoops log): the per-CPU dispatch counts and their second, kernel threads in a band of their
// own, what is on core, IPI latency, the watch over idle CPUs and pending IPIs, the record limits and the limit of its own of frames-only gaps, the gap
// trigger (what tells a stall from the end of an animation, the thresholds and their clock, the clock icon's 10 Hz, a tap on a screen long idle, a
// blinking cursor, a late tick), the kbt trigger (first and later samples, ends, queue overflow,
// recycling, giving up), kbd's busiest thread, the timeouts in the system
// call registers, the frame walk on simulated user memory, the lengths of all records with worst-case values, and the records through the actual
// RamoopsLog.c writer (M in the console ring, Q in the events ring, CRC checked).
//   cc -std=c11 -O2 -Wall -Wextra -Werror -fsanitize=address,undefined stall.c -o stall && ./stall
//   ./stall --dump DIR   also writes the two zone images of a run with records and expected.tsv (gpu-test-loop's decoder and stall-report.py tests)
#define RAMOOPS_LOG_TEST 1
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../RamoopsLog.c"
#include "../StallProbe.h"

// ---- RamoopsLog platform (as Test/ramoops.c, reduced) ------------------------------------------------------------------
enum { REGION = RAMOOPS_LOG_DEFAULT_CONSOLE + RAMOOPS_LOG_DEFAULT_PMSG, HZ = 19200000 };
static uint8_t *region;
static uint64_t clock_now = 1000000;
uintptr_t ramoops_platform_map(uint64_t pa, uint64_t bytes, int device)
{
    (void)pa; (void)device;
    assert(bytes <= REGION);
    memset(region, 0xa5, REGION);
    return (uintptr_t)region;
}
int ramoops_platform_arg(const char *name, void *value, unsigned bytes)
{
    if (strcmp(name, "pagani-ramoops")) return 0;
    assert(bytes == 4);
    uint32_t one = 1;
    memcpy(value, &one, 4);
    return 1;
}
const char *ramoops_platform_boot_args(void) { return "-v pagani-ramoops=1 pagani-diag=1"; }
uint64_t ramoops_platform_counter(void) { return clock_now += 1920; }
uint64_t ramoops_platform_frequency(void) { return HZ; }
uint32_t ramoops_platform_cpu_mhz(int *cpu) { *cpu = 0; return 1000; }
int64_t ramoops_platform_psci(int hvc, uint64_t function, uint64_t a, uint64_t b, uint64_t c)
{ (void)hvc; (void)function; (void)a; (void)b; (void)c; return -1; }
int ramoops_platform_psci_method(char method[4]) { (void)method; return 0; }
int ramoops_platform_arm_timer(uint64_t milliseconds) { (void)milliseconds; return 0; }
int ramoops_platform_arm_backup_timer(uint64_t milliseconds, int avoid) { (void)milliseconds; (void)avoid; return -1; }
void ramoops_platform_ram(uint64_t *base, uint64_t *bytes) { *base = 0x8c0000000ULL; *bytes = 0x100000000ULL; }
void ramoops_platform_smc(uint64_t regs[8]) { (void)regs; assert(0); }
unsigned ramoops_platform_park_others(uint32_t *parked) { (void)parked; assert(0); return 0; }
int ramoops_platform_wdog_bite(const char *reason) { (void)reason; assert(0); return 0; }
int ramoops_platform_wdog_hold(const char *reason) { (void)reason; assert(0); return 0; }
void ramoops_platform_wdog_release(const char *reason) { (void)reason; assert(0); }
void ramoops_platform_wdog_flushed(void) {}

#define MS(ms) ((uint64_t)(ms) * (HZ / 1000))

static struct stall_state s;

static void state_reset(void)
{
    memset(&s, 0, sizeof(s));
    stall_state_init(&s, HZ);
}

static void run_on(unsigned cpu, int32_t pid, const char *name, int32_t pri, uint64_t stamp, uint64_t latency, uint64_t tid)
{
    struct boot_progress_stall_run r;
    memset(&r, 0, sizeof(r));
    r.stamp = stamp;
    r.latency = latency;
    r.tid = tid;
    r.name = name;
    r.pid = pid;
    r.pri = pri;
    r.cpu = cpu;
    r.user = pid > 0;
    stall_note_run(&s, &r);
}

static void test_dispatch(void)
{
    static struct stall_seen seen;
    static struct stall_second second;
    assert(stall_band_of(127) == STALL_FG && stall_band_of(47) == STALL_FG && stall_band_of(46) == STALL_DEF);
    assert(stall_band_of(20) == STALL_DEF && stall_band_of(19) == STALL_BG && stall_band_of(4) == STALL_BG);
    state_reset();
    memset(&seen, 0, sizeof(seen));
    run_on(0, 448, "MobileNotes", 47, 100, MS(3), 0x2b5);
    run_on(1, 448, "MobileNotes", 47, 101, MS(12), 0x2b6);
    run_on(2, 38, "backboardd", 54, 102, MS(60), 0x100);
    run_on(3, 90, "kbd", 31, 103, MS(250), 0x200);
    run_on(3, 90, "kbd", 4, 104, MS(1), 0x201);
    run_on(4, 0, NULL, 80, 105, MS(500), 0x300);        // a kernel thread: its band goes by what it is, not by its priority
    run_on(6, 0, NULL, 4, 105, MS(1), 0x302);
    run_on(5, -1, NULL, 0, 106, 0, 0x301);              // the idle thread: on core, not counted
    stall_second_take(&s, &seen, &second);
    assert(second.band[STALL_FG].n == 3 && second.band[STALL_FG].over10 == 2 && second.band[STALL_FG].over50 == 1);
    assert(second.band[STALL_FG].over200 == 0 && second.band[STALL_FG].max_ticks == MS(60));
    assert(second.band[STALL_FG].tid == 0x100 && second.band[STALL_FG].pid == 38 && !strcmp(second.band[STALL_FG].name, "backboardd"));
    assert(second.band[STALL_DEF].n == 1 && second.band[STALL_DEF].over200 == 1 && !strcmp(second.band[STALL_DEF].name, "kbd"));
    assert(second.band[STALL_BG].n == 1 && second.band[STALL_BG].pri == 4);
    assert(second.band[STALL_KERNEL].n == 2 && second.band[STALL_KERNEL].over200 == 1 && second.band[STALL_KERNEL].max_ticks == MS(500));
    assert(second.band[STALL_KERNEL].tid == 0x300 && second.band[STALL_KERNEL].pri == 80 && !second.band[STALL_KERNEL].pid && !second.band[STALL_KERNEL].name[0]);
    // The next second starts from nothing, in the other half; the half read is empty when it comes back.
    stall_second_take(&s, &seen, &second);
    assert(!second.band[STALL_FG].n && !second.band[STALL_FG].max_ticks && !second.band[STALL_FG].name[0]);
    run_on(0, 448, "MobileNotes", 47, 200, MS(5), 0x2b5);
    stall_second_take(&s, &seen, &second);
    assert(second.band[STALL_FG].n == 1 && second.band[STALL_FG].max_ticks == MS(5));
    stall_second_take(&s, &seen, &second);
    assert(!second.band[STALL_FG].max_ticks);
    run_on(0, 448, "MobileNotes", 47, 300, MS(1), 0x2b5);
    stall_second_take(&s, &seen, &second);
    assert(second.band[STALL_FG].max_ticks == MS(1));    // not the 5 ms of two passes ago
    // A name of more than 16 characters keeps its first 16; a CPU reuses the name it has for the same pid.
    run_on(6, 777, "AVeryLongProcessNameIndeed", 31, 400, 0, 1);
    struct stall_oncore on;
    assert(stall_oncore_read(&s.cpu[6], &on) && !strcmp(on.name, "AVeryLongProcess") && on.tid == 1 && on.since == 400 && on.pri == 31);
    run_on(6, 777, "IgnoredSamePid", 31, 401, 0, 2);
    assert(stall_oncore_read(&s.cpu[6], &on) && !strcmp(on.name, "AVeryLongProcess") && on.tid == 2);
    run_on(6, -1, NULL, 0, 402, 0, 3);
    assert(stall_oncore_read(&s.cpu[6], &on) && on.pid == -1 && !on.name[0]);
    // A writer in the middle: the reader gives up after four tries, it never mixes.
    s.cpu[6].seq |= 1;
    assert(!stall_oncore_read(&s.cpu[6], &on));
    s.cpu[6].seq++;
    // A CPU out of range changes nothing.
    run_on(STALL_CPUS, 1, "x", 47, 500, MS(1000), 9);
    stall_note_idle(&s, STALL_CPUS);
    stall_note_ipi_send(&s, STALL_CPUS, 1);
    stall_note_ipi_recv(&s, STALL_CPUS, 2);
    stall_second_take(&s, &seen, &second);
    assert(!second.band[STALL_FG].n);
    printf("ok  dispatch: bands, per-second largest and its thread, epochs, on-core reader\n");
}

static void test_ipi(void)
{
    static struct stall_seen seen;
    static struct stall_second second;
    static struct stall_watch w;
    state_reset();
    memset(&seen, 0, sizeof(seen));
    memset(&w, 0, sizeof(w));
    stall_note_idle(&s, 2);
    stall_note_idle(&s, 2);
    stall_note_ipi_send(&s, 3, MS(100));
    stall_note_ipi_send(&s, 3, MS(101));                 // merged: the oldest stamp stays
    assert(s.cpu[3].pending == MS(100));
    stall_note_ipi_recv(&s, 3, MS(104));
    assert(!s.cpu[3].pending);
    stall_note_ipi_recv(&s, 3, MS(105));                 // a handler with no stamp: counted, no latency
    stall_second_take(&s, &seen, &second);
    assert(second.idle[2] == 2 && second.sent[3] == 2 && second.recv[3] == 2 && second.ipi_max[3] == MS(4));
    stall_second_take(&s, &seen, &second);
    assert(!second.sent[3] && !second.ipi_max[3]);
    // Pending too long: counted once, an IPI that lands clears the mark.
    assert(!stall_watch_ipi(&w, 1, 0, MS(1000), HZ));
    assert(!stall_watch_ipi(&w, 1, MS(990), MS(1000), HZ));
    assert(stall_watch_ipi(&w, 1, MS(980), MS(1000), HZ) && w.stuck[1] == 1);
    assert(!stall_watch_ipi(&w, 1, MS(980), MS(1010), HZ) && w.stuck[1] == 1);
    assert(!stall_watch_ipi(&w, 1, 0, MS(1020), HZ));
    assert(stall_watch_ipi(&w, 1, MS(1000), MS(1030), HZ) && w.stuck[1] == 2);
    assert(!stall_watch_ipi(&w, STALL_CPUS, MS(1), MS(1000), HZ));
    // Idle over a non-empty queue: from the second tick in a row, a sample at the fifth.
    struct boot_progress_stall_cpu c = {STALL_CPU_IDLE, 1, 1, 0};
    assert(!stall_watch_cpu(&w, 4, &c) && !w.hungry[4]);
    assert(!stall_watch_cpu(&w, 4, &c) && w.hungry[4] == 1);
    assert(!stall_watch_cpu(&w, 4, &c) && !stall_watch_cpu(&w, 4, &c));
    assert(stall_watch_cpu(&w, 4, &c) && w.hungry[4] == 4);
    assert(!stall_watch_cpu(&w, 4, &c) && w.hungry[4] == 5);    // once a streak
    struct boot_progress_stall_cpu busy = {STALL_CPU_RUNNING, 0, 3, 0};
    assert(!stall_watch_cpu(&w, 4, &busy) && !w.streak[4]);
    // Chosen, still on the idle thread: as hungry. Chosen and already off it: not.
    struct boot_progress_stall_cpu chosen = {STALL_CPU_DISPATCHING, 1, 1, 0}, moving = {STALL_CPU_DISPATCHING, 0, 1, 0};
    assert(!stall_watch_cpu(&w, 5, &chosen) && w.streak[5] == 1);
    assert(!stall_watch_cpu(&w, 5, &moving) && !w.streak[5]);
    // An empty queue, a realtime queue, off.
    struct boot_progress_stall_cpu empty = {STALL_CPU_IDLE, 1, 0, 0}, rt = {STALL_CPU_IDLE, 1, 0, 2}, off = {STALL_CPU_OFF, 0, 1, 0};
    assert(!stall_watch_cpu(&w, 6, &empty) && !w.streak[6]);
    assert(!stall_watch_cpu(&w, 6, &rt) && w.streak[6] == 1);
    assert(!stall_watch_cpu(&w, 6, &off) && !w.streak[6]);
    assert(!stall_watch_cpu(&w, STALL_CPUS, &c));
    for (unsigned i = 0; i < 300; i++) stall_watch_cpu(&w, 7, &c);
    assert(w.streak[7] == 255 && w.hungry[7] == 299);    // the streak saturates, it never comes back round to the sample
    printf("ok  IPIs: latency, merged SGIs, stuck; idle over a non-empty queue\n");
}

static void test_limit(void)
{
    struct stall_limit l;
    memset(&l, 0, sizeof(l));
    // Two seconds' worth at once, then 20 lines a second.
    assert(stall_limit_take(&l, 0, HZ, 20, STALL_LEVEL_HIGH));
    assert(stall_limit_take(&l, 0, HZ, 14, STALL_LEVEL_HIGH));
    assert(stall_limit_take(&l, 0, HZ, 6, STALL_LEVEL_HIGH));
    assert(!stall_limit_take(&l, 0, HZ, 1, STALL_LEVEL_HIGH) && l.over_rate == 1 && l.used == 40);
    assert(!stall_limit_take(&l, MS(300), HZ, 7, STALL_LEVEL_HIGH));
    assert(stall_limit_take(&l, MS(350), HZ, 7, STALL_LEVEL_HIGH));
    // A long quiet does not save up more than that.
    assert(stall_limit_take(&l, MS(100000), HZ, 40, STALL_LEVEL_HIGH));
    assert(!stall_limit_take(&l, MS(100000), HZ, 1, STALL_LEVEL_HIGH));
    // A stall's first three samples with their fg thread, 9 lines each at the first thresholds after a quiet, get through; so does the next one at 1 s.
    memset(&l, 0, sizeof(l));
    for (unsigned i = 0; i < 4; i++) assert(stall_limit_take(&l, MS(stall_gap_ms[i] - stall_gap_ms[0]), HZ, 9, STALL_LEVEL_HIGH));
    assert(!l.over_rate);
    // The budget: what is least sure to be a stall stops first.
    static const struct { unsigned level; uint32_t lines; } tiers[] = {{STALL_LEVEL_TAIL, STALL_LINES_TAIL}, {STALL_LEVEL_LOW, STALL_LINES_LOW},
                                                                       {STALL_LEVEL_MID, STALL_LINES_MID}, {STALL_LEVEL_HIGH, STALL_LINES_MAX}};
    uint64_t now = MS(1000);
    for (unsigned i = 0; i < 4; i++) {
        memset(&l, 0, sizeof(l));
        l.primed = 1;
        l.used = tiers[i].lines - 7;
        l.stamp = now;
        l.credit = HZ;
        assert(stall_limit_take(&l, now, HZ, 7, tiers[i].level) && l.used == tiers[i].lines);
        now += HZ;
        assert(!stall_limit_take(&l, now, HZ, 7, tiers[i].level) && l.over_budget == 1);
        if (i < 3) assert(stall_limit_take(&l, now, HZ, 7, tiers[i + 1].level));
    }
    // A level out of range gets the least.
    memset(&l, 0, sizeof(l));
    l.primed = 1;
    l.used = STALL_LINES_TAIL;
    assert(!stall_limit_take(&l, now, HZ, 1, STALL_LEVELS));
    // The keyboard's limit is a limit of its own: all of the other one used up, by lines and by rate, changes nothing for it.
    struct stall_limit gaps, keys;
    memset(&gaps, 0, sizeof(gaps));
    memset(&keys, 0, sizeof(keys));
    gaps.primed = 1;
    gaps.used = STALL_LINES_MAX;
    gaps.stamp = now;
    assert(!stall_limit_take(&gaps, now, HZ, 7, STALL_LEVEL_HIGH));
    uint32_t taken = 0;
    for (uint64_t at = now; taken < STALL_LINES_KBT / 7; at += MS(350)) taken += (uint32_t)stall_limit_take(&keys, at, HZ, 7, STALL_LEVEL_KBT);
    assert(keys.used == taken * 7 && !keys.over_rate);
    keys.credit = HZ;
    assert(!stall_limit_take(&keys, now + MS(3000000), HZ, 7, STALL_LEVEL_KBT) && keys.over_budget == 1);
    assert(gaps.used == STALL_LINES_MAX);
    printf("ok  limit: 20 lines a second, a burst of two seconds' worth, the budget by level, the keyboard's own\n");
}

static struct stall_gap_in in_at(uint64_t now, uint64_t frames, uint64_t newest)
{
    struct stall_gap_in in;
    memset(&in, 0, sizeof(in));
    in.now = now;
    in.hz = HZ;
    in.frames = frames;
    in.newest = newest;
    return in;
}

// One tick of a stall's ladder: the threshold it asks for must be `want`.
static void gap_tick(struct stall_gap *g, struct stall_gap_in *in, uint64_t at, uint32_t want)
{
    in->now = at;
    assert(stall_gap_step(g, in) == want);
}

// A run of `frames` frames, one every 8 ms, each with its submission, stamped as stall_tick sees them, then the newest frame's tick.
static struct stall_gap_in animate(struct stall_gap *g, uint64_t *count, uint32_t *gpu, uint64_t start, unsigned frames)
{
    struct stall_gap_in in = in_at(start, 0, 0);
    for (unsigned i = 0; i < frames; i++) {
        const uint64_t at = start + MS(8) * i;
        *gpu += 1;
        in = in_at(at + MS(1), ++*count, at);
        in.gpu = *gpu;
        in.gpu_at = in.now;
        in.frames_before = i > 3 ? 3 : i;
        (void)stall_gap_step(g, &in);
    }
    return in;
}

static void test_gap(void)
{
    struct stall_gap g;
    memset(&g, 0, sizeof(g));
    // Before the first frame nothing happens.
    struct stall_gap_in in = in_at(MS(1000), 0, 0);
    assert(!stall_gap_step(&g, &in) && !stall_gap_step(&g, &in));
    // A touch after the frame: from it, every threshold, and the sampler stays awake until the last.
    in = in_at(MS(1000), 10, MS(1000));
    in.frames_before = 3;
    assert(!stall_gap_step(&g, &in));
    in.touch_at = MS(1010);
    gap_tick(&g, &in, MS(1010), 0);
    assert(g.asked == MS(1010) && stall_gap_waiting(&g));
    static const struct { uint32_t age, want; unsigned level; } steps[] = {
        {10, 0, 0}, {109, 0, 0}, {110, 110, STALL_LEVEL_LOW}, {150, 0, 0}, {200, 200, STALL_LEVEL_MID}, {300, 0, 0}, {400, 400, STALL_LEVEL_HIGH}, {999, 0, 0},
        {1000, 1000, STALL_LEVEL_HIGH}, {2999, 0, 0}, {3000, 3000, STALL_LEVEL_HIGH}, {7999, 0, 0}, {8000, 8000, STALL_LEVEL_HIGH}};
    for (unsigned i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
        gap_tick(&g, &in, MS(1010 + steps[i].age), steps[i].want);
        if (steps[i].want) assert(stall_gap_level(&g, steps[i].want) == steps[i].level);
        assert(stall_gap_waiting(&g) == (steps[i].age < 8000));
    }
    assert(g.samples == 6 && g.stalls == 1 && !g.capped);
    gap_tick(&g, &in, MS(30000), 0);                    // long past the touch: the ladder is over, nothing more
    assert(g.samples == 6);
    // A new frame ends the stall; the next one has its own samples and number.
    in = in_at(MS(30001), 11, MS(30001));
    assert(!stall_gap_step(&g, &in) && !g.samples && !g.level && !g.asked);
    in.touch_at = MS(30002);
    gap_tick(&g, &in, MS(30002), 0);
    gap_tick(&g, &in, MS(30112), 110);
    assert(g.stalls == 2);
    // A tick that comes late crosses several thresholds as one sample, at the largest.
    in = in_at(MS(31000), 12, MS(31000));
    assert(!stall_gap_step(&g, &in));
    in.touch_at = MS(31005);
    gap_tick(&g, &in, MS(31005), 0);
    gap_tick(&g, &in, MS(31255), 200);
    assert(g.samples == 1);
    gap_tick(&g, &in, MS(31406), 400);
    // A submission after the frame: a stall while it is recent, of the low level, and it keeps nobody awake: it may be work that is never shown.
    in = in_at(MS(32000), 112, MS(32000));
    assert(!stall_gap_step(&g, &in));
    in.gpu = 5;
    in.gpu_at = MS(32005);
    gap_tick(&g, &in, MS(32005), 0);
    assert(g.sign == STALL_SIGN_GPU && !stall_gap_waiting(&g));
    gap_tick(&g, &in, MS(32115), 110);
    assert(stall_gap_level(&g, 110) == STALL_LEVEL_LOW);
    gap_tick(&g, &in, MS(32205), 200);
    assert(stall_gap_level(&g, 200) == STALL_LEVEL_LOW);
    gap_tick(&g, &in, MS(32405), 400);
    assert(stall_gap_level(&g, 400) == STALL_LEVEL_LOW);
    const uint32_t still = g.quiet;
    gap_tick(&g, &in, MS(33005), 0);                    // 1000 ms: the last submission is 1 s old, a stall no one is sure of ends here
    assert(g.samples == 3 && g.quiet == still + 1);
    // More of them: it goes on; and a touch makes it sure, from the next threshold, on the same clock.
    in = in_at(MS(34000), 113, MS(34000));
    assert(!stall_gap_step(&g, &in));
    in.gpu = 6;
    in.gpu_at = MS(34010);
    gap_tick(&g, &in, MS(34010), 0);
    in.gpu_at = MS(35000);
    gap_tick(&g, &in, MS(35010), 1000);                 // a tick that comes late: all four at once
    in.touch_at = MS(35100);
    gap_tick(&g, &in, MS(35100), 0);
    assert(g.sign == STALL_SIGN_SURE && stall_gap_waiting(&g) && stall_gap_from(&g) == MS(34010));
    gap_tick(&g, &in, MS(37010), 3000);
    assert(stall_gap_level(&g, 3000) == STALL_LEVEL_HIGH);
    // Idle: no touch, no GPU work.
    in = in_at(MS(40000), 13, MS(40000));
    assert(!stall_gap_step(&g, &in));
    uint32_t quiet = g.quiet;
    gap_tick(&g, &in, MS(40500), 0);
    assert(g.quiet == quiet + 1 && !stall_gap_waiting(&g));
    // A touch that is too old does not count, and neither does the touch or submission of the frame itself, seen at the same tick.
    in = in_at(MS(41000), 14, MS(41000));
    in.touch_at = MS(40000);
    assert(!stall_gap_step(&g, &in));
    gap_tick(&g, &in, MS(41110), 0);
    in = in_at(MS(42000), 15, MS(42000));
    in.touch_at = MS(42000);
    in.gpu = 9;
    in.gpu_at = MS(42000);
    assert(!stall_gap_step(&g, &in));
    gap_tick(&g, &in, MS(42110), 0);
    assert(!g.asked);
    // A tap on a screen idle for a minute: nothing while idle, then the ladder runs from the tap, not from the last frame.
    in = in_at(MS(50000), 16, MS(50000));
    in.frames_before = 3;
    assert(!stall_gap_step(&g, &in));
    for (uint32_t t = 10; t < 60000; t += 10) gap_tick(&g, &in, MS(50000 + t), 0);
    assert(g.level == STALL_GAP_LEVELS && !g.asked);
    in.touch_at = MS(110000);
    gap_tick(&g, &in, MS(110000), 0);
    assert(g.asked == MS(110000) && !g.level);
    gap_tick(&g, &in, MS(110109), 0);
    gap_tick(&g, &in, MS(110110), 110);
    assert(stall_gap_from(&g) == MS(110000));
    // One touch is enough for all of a long stall: the signs last until the next frame, no fresh one is needed.
    gap_tick(&g, &in, MS(113000), 3000);
    assert(stall_gap_waiting(&g));
    gap_tick(&g, &in, MS(118000), 8000);
    assert(!stall_gap_waiting(&g));
    // A touch past the last threshold, still without a frame: a stall of its own.
    const uint32_t stalls = g.stalls;
    in.touch_at = MS(118500);
    gap_tick(&g, &in, MS(118500), 0);
    assert(g.asked == MS(118500) && !g.samples);
    gap_tick(&g, &in, MS(118610), 110);
    assert(g.stalls == stalls + 1);
    // The clock icon on the home screen: a frame every 100 ms, its own submission, a cadence, a finger on the glass in between. No sample at all.
    uint64_t ticking = 300;
    uint32_t submitted = 50;
    const uint32_t before_clock = g.stalls;
    for (unsigned hand = 0; hand < 50; hand++) {
        const uint64_t at = MS(120000 + 103 * hand);    // the display link's 100 ms, a little late
        in = in_at(at + MS(1), ++ticking, at);
        in.gpu = ++submitted;
        in.gpu_at = at;
        in.frames_before = 3;
        assert(!stall_gap_step(&g, &in));
        for (uint32_t t = 10; t <= 100; t += 10) {
            if (hand % 2 && t == 20) in.touch_at = at + MS(t);
            gap_tick(&g, &in, at + MS(t), 0);
        }
    }
    assert(g.stalls == before_clock);
    // A blinking cursor: one frame every 500 ms, its own submission, nothing after it. No sample, however long.
    uint32_t gpu = 100;
    for (unsigned blink = 0; blink < 4; blink++) {
        const uint64_t at = MS(130000 + 500 * blink);
        gpu += 2;
        in = in_at(at + MS(1), 20 + blink, at);
        in.gpu = gpu;
        in.gpu_at = at;
        in.frames_before = 1;
        assert(!stall_gap_step(&g, &in));
        for (uint32_t t = 10; t < 490; t += 10) gap_tick(&g, &in, at + MS(t), 0);
    }
    // The end of an animation: one sample, the first threshold, of the lowest level, whatever the age, and nothing that keeps the sampler up.
    uint64_t count = 100;
    in = animate(&g, &count, &gpu, MS(140000), 40);
    const uint64_t last = in.newest;
    const uint32_t capped = g.capped;
    in.gpu_at = last;
    gap_tick(&g, &in, last + MS(109), 0);
    gap_tick(&g, &in, last + MS(110), 110);
    assert(!g.asked && !g.sign && stall_gap_level(&g, 110) == STALL_LEVEL_TAIL && !stall_gap_waiting(&g) && stall_gap_from(&g) == last);
    gap_tick(&g, &in, last + MS(200), 0);
    gap_tick(&g, &in, last + MS(400), 0);
    assert(g.samples == 1 && g.capped == capped + 2);
    // Something asks after all: the ladder, in the same stall, from then.
    const uint32_t same = g.stalls;
    in.gpu = gpu + 1;
    in.gpu_at = last + MS(450);
    gap_tick(&g, &in, last + MS(450), 0);
    gap_tick(&g, &in, last + MS(560), 110);
    assert(g.stalls == same && g.samples == 2 && stall_gap_level(&g, 110) == STALL_LEVEL_LOW);
    // Two frames after a touch that came before the last of them, no cadence: nothing to sample.
    in = in_at(MS(150000), 200, MS(150000));
    in.touch_at = MS(149980);
    in.frames_before = 1;
    assert(!stall_gap_step(&g, &in));
    for (uint32_t t = 10; t <= 700; t += 10) gap_tick(&g, &in, MS(150000 + t), 0);
    // A tick that sees a touch and the frame that answers it at once: the touch's stamp is the tick's, later than the frame's, and still the frame's own.
    in = in_at(MS(150900), 299, MS(150895));
    in.touch_at = MS(150900);
    in.frames_before = 1;
    assert(!stall_gap_step(&g, &in));
    gap_tick(&g, &in, MS(150910), 0);
    assert(!g.asked);
    // A touch stamped before the newest frame, noticed only after it, says nothing about that frame's successor either.
    in = in_at(MS(151000), 300, MS(151000));
    in.frames_before = 1;
    assert(!stall_gap_step(&g, &in));
    in.touch_at = MS(150980);
    for (uint32_t t = 10; t <= 700; t += 10) gap_tick(&g, &in, MS(151000 + t), 0);
    assert(!g.asked);
    // A kbt pair left open is a sign too, and stays one as long as it is open; the submission must be recent.
    in = in_at(MS(160000), 201, MS(160000));
    assert(!stall_gap_step(&g, &in));
    in.kbt_open = 1;
    gap_tick(&g, &in, MS(160010), 0);
    gap_tick(&g, &in, MS(160120), 110);
    in = in_at(MS(170000), 202, MS(170000));
    in.gpu = gpu;
    in.gpu_at = MS(169000);
    in.frames_before = 3;
    assert(!stall_gap_step(&g, &in));
    in.gpu = gpu + 1;
    gap_tick(&g, &in, MS(170110), 0);
    assert(!g.asked);
    // A stamp ahead of the clock (two CPUs).
    in = in_at(MS(180000), 203, MS(180100));
    assert(!stall_gap_step(&g, &in) && !stall_gap_step(&g, &in));
    printf("ok  gaps: what tells a stall, the thresholds and their clock, a tap on an idle screen, a long stall, the clock icon, a blinking cursor, a late tick\n");
}

// Animations that end, hundreds of them: what they may spend is bounded by a limit of their own, and the real stalls and the keyboard still get theirs,
// whichever of them used up theirs first.
static void test_gap_budget(void)
{
    struct stall_gap g;
    struct stall_limit gaps, tails, keys;
    memset(&g, 0, sizeof(g));
    memset(&gaps, 0, sizeof(gaps));
    memset(&tails, 0, sizeof(tails));
    memset(&keys, 0, sizeof(keys));
    uint64_t count = 0, t = MS(1000);
    uint32_t gpu = 0;
    unsigned sampled = 0, written = 0;
    for (unsigned round = 0; round < 400; round++) {
        struct stall_gap_in in = animate(&g, &count, &gpu, t, 40);
        const uint64_t last = in.newest;
        for (uint32_t ms = 10; ms <= 700; ms += 10) {
            in.now = last + MS(ms);
            const uint32_t got = stall_gap_step(&g, &in);
            if (got) {
                sampled++;
                assert(got == 110 && stall_gap_level(&g, got) == STALL_LEVEL_TAIL);
                written += (unsigned)stall_limit_take(&tails, in.now, HZ, 9, stall_gap_level(&g, got));
            }
        }
        t = last + MS(2000);
    }
    assert(sampled == 400 && written * 9 == tails.used && tails.used <= STALL_LINES_TAIL && tails.over_budget == sampled - written && !gaps.used);
    // The stalls after all that: every threshold of a long one is still written. The keyboard does not even see the difference.
    struct stall_gap_in in = animate(&g, &count, &gpu, t, 40);
    const uint64_t last = in.newest;
    in.touch_at = last + MS(5);
    in.gpu_at = last;
    unsigned ladder = 0;
    for (uint32_t ms = 10; ms <= 9000; ms += 10) {
        in.now = last + MS(ms);
        const uint32_t got = stall_gap_step(&g, &in);
        if (got) ladder += (unsigned)stall_limit_take(&gaps, in.now, HZ, 9, stall_gap_level(&g, got));
    }
    assert(ladder == STALL_GAP_LEVELS);
    assert(stall_limit_take(&keys, last, HZ, 7, STALL_LEVEL_KBT) && !keys.over_budget);
    // The other way round: the stalls' limit used up to the last line, an animation's end still has all of its own.
    gaps.used = STALL_LINES_MAX;
    struct stall_limit fresh;
    memset(&fresh, 0, sizeof(fresh));
    assert(!stall_limit_take(&gaps, last + MS(10000), HZ, 9, STALL_LEVEL_HIGH));
    assert(stall_limit_take(&fresh, last + MS(10000), HZ, 9, STALL_LEVEL_TAIL));
    // Animations again, each followed by a submission never shown: they stay under what real stalls may use.
    struct stall_limit worst;
    memset(&worst, 0, sizeof(worst));
    t = last + MS(20000);
    for (unsigned round = 0; round < 400; round++) {
        struct stall_gap_in late = animate(&g, &count, &gpu, t, 40);
        const uint64_t end = late.newest;
        late.gpu = gpu + 1;
        late.gpu_at = end + MS(5);
        for (uint32_t ms = 10; ms <= 1500; ms += 10) {
            late.now = end + MS(ms);
            const uint32_t got = stall_gap_step(&g, &late);
            if (got) (void)stall_limit_take(&worst, late.now, HZ, 9, stall_gap_level(&g, got));
        }
        gpu++;
        t = end + MS(2000);
    }
    assert(worst.used <= STALL_LINES_LOW && worst.used + 9 * STALL_GAP_LEVELS <= STALL_LINES_MAX);
    printf("ok  gaps: 400 animations cost %u lines of the %u of their limit, a stall after them all %u samples\n", tails.used, (unsigned)STALL_LINES_TAIL, ladder);
}

static void test_kbt(void)
{
    static struct stall_kbt t;
    struct stall_kbt_due due;
    memset(&t, 0, sizeof(t));
    uint64_t now = MS(1000);
    assert(!stall_kbt_any(&t) && !stall_kbt_take(&t, now, HZ, &due));
    // An end without a begin: nothing.
    stall_kbt_note(&t, 448, STALL_KBT_IN, 1, now, HZ);
    assert(!stall_kbt_any(&t));
    // A pair that ends in time is never sampled.
    stall_kbt_note(&t, 448, STALL_KBT_IN, 0, now, HZ);
    assert(stall_kbt_any(&t) && !stall_kbt_take(&t, now + MS(299), HZ, &due));
    stall_kbt_note(&t, 448, STALL_KBT_IN, 1, now + MS(100), HZ);
    assert(!stall_kbt_any(&t) && !stall_kbt_take(&t, now + MS(400), HZ, &due));
    // One that does not: at 300 ms, then every 500 ms until it ends.
    stall_kbt_note(&t, 448, STALL_KBT_DN, 0, now, HZ);
    assert(!stall_kbt_take(&t, now + MS(299), HZ, &due));
    assert(stall_kbt_take(&t, now + MS(300), HZ, &due) && due.pid == 448 && due.group == STALL_KBT_DN && due.age_ms == 300 && due.sample == 1 && due.open == 1);
    const uint32_t id = due.id;
    assert(!stall_kbt_take(&t, now + MS(799), HZ, &due));
    assert(stall_kbt_take(&t, now + MS(800), HZ, &due) && due.age_ms == 800 && due.sample == 2 && due.id == id);
    stall_kbt_note(&t, 448, STALL_KBT_DN, 1, now + MS(900), HZ);
    assert(!stall_kbt_any(&t) && !stall_kbt_take(&t, now + MS(1400), HZ, &due));
    // The longer it stays open the rarer: 500 ms four times, 1 s four times, then 2 s.
    now = MS(1500);
    stall_kbt_note(&t, 448, STALL_KBT_DN, 0, now, HZ);
    uint64_t at = now + MS(300);
    for (unsigned sample = 1; sample <= 12; sample++) {
        const uint32_t wait = sample <= 4 ? 500 : sample <= 8 ? 1000 : 2000;
        assert(stall_kbt_wait_ms(sample) == wait && stall_kbt_take(&t, at, HZ, &due) && due.sample == sample);
        assert(!stall_kbt_take(&t, at + MS(wait - 1), HZ, &due));
        at += MS(wait);
    }
    stall_kbt_note(&t, 448, STALL_KBT_DN, 1, at, HZ);
    assert(!stall_kbt_any(&t));
    // Two begins: the end of the first starts the second's clock from its own begin, under a new number.
    now = MS(2000);
    stall_kbt_note(&t, 448, STALL_KBT_UP, 0, now, HZ);
    stall_kbt_note(&t, 448, STALL_KBT_UP, 0, now + MS(200), HZ);
    assert(stall_kbt_take(&t, now + MS(300), HZ, &due) && due.open == 2);
    const uint32_t first = due.id;
    stall_kbt_note(&t, 448, STALL_KBT_UP, 1, now + MS(350), HZ);
    assert(stall_kbt_take(&t, now + MS(500), HZ, &due) && due.id != first && due.age_ms == 300 && due.sample == 1 && due.open == 1);
    stall_kbt_note(&t, 448, STALL_KBT_UP, 1, now + MS(600), HZ);
    assert(!stall_kbt_any(&t));
    // More begins than are kept: the oldest stay, every end still counts.
    now = MS(3000);
    for (unsigned i = 0; i < STALL_KBT_BEGINS + 3; i++) stall_kbt_note(&t, 1, STALL_KBT_IN, 0, now + MS(i), HZ);
    for (unsigned i = 0; i < STALL_KBT_BEGINS + 2; i++) stall_kbt_note(&t, 1, STALL_KBT_IN, 1, now + MS(10), HZ);
    assert(stall_kbt_any(&t) && stall_kbt_take(&t, now + MS(4000), HZ, &due) && due.open == 1);
    stall_kbt_note(&t, 1, STALL_KBT_IN, 1, now + MS(4001), HZ);
    assert(!stall_kbt_any(&t));
    // More pairs than slots: the oldest gives way.
    now = MS(4000);
    for (int32_t p = 1; p <= STALL_KBT_SLOTS + 1; p++) stall_kbt_note(&t, p, STALL_KBT_DN, 0, now + MS(p), HZ);
    assert(t.recycled == 1 && !stall_kbt_find(&t, 1, STALL_KBT_DN) && stall_kbt_find(&t, STALL_KBT_SLOTS + 1, STALL_KBT_DN));
    // The process is gone.
    for (int32_t p = 2; p <= STALL_KBT_SLOTS + 1; p++) stall_kbt_forget(&t, p);
    assert(!stall_kbt_any(&t));
    // An end that never comes is given up on.
    now = MS(5000);
    stall_kbt_note(&t, 9, STALL_KBT_IN, 0, now, HZ);
    unsigned samples = 0;
    for (at = now; at < now + MS(130000); at += MS(100)) samples += (unsigned)stall_kbt_take(&t, at, HZ, &due);
    assert(samples == STALL_KBT_SAMPLES && !stall_kbt_any(&t) && t.given_up == 1);
    // Two groups of one process stay apart; a bad group is ignored.
    stall_kbt_note(&t, 9, STALL_KBT_IN, 0, now, HZ);
    stall_kbt_note(&t, 9, STALL_KBT_DN, 0, now, HZ);
    stall_kbt_note(&t, 9, STALL_KBT_GROUPS, 0, now, HZ);
    stall_kbt_note(&t, 9, STALL_KBT_IN, 1, now + MS(1), HZ);
    assert(stall_kbt_find(&t, 9, STALL_KBT_DN) && !stall_kbt_find(&t, 9, STALL_KBT_IN));
    printf("ok  kbt: 300 ms, every 500 ms and rarer, ends, queue overflow, recycling, a pair given up\n");
}

static void test_busy(void)
{
    static struct stall_busy b;
    uint64_t tids[3] = {1, 2, 3}, times[3] = {100, 500, 50};
    assert(!stall_busiest(&b, tids, times, 0));
    assert(stall_busiest(&b, tids, times, 3) == 2);
    times[0] = 400; times[1] = 510; times[2] = 60;
    assert(stall_busiest(&b, tids, times, 3) == 1);      // 300 against 10
    tids[2] = 4; times[2] = 5000;
    assert(stall_busiest(&b, tids, times, 3) == 4);      // new: all of its time
    times[0] = 400; times[1] = 510; times[2] = 5000;
    assert(stall_busiest(&b, tids, times, 3) == 1);      // nobody ran: the first
    uint64_t many[STALL_BUSY_THREADS + 8], more[STALL_BUSY_THREADS + 8];
    for (unsigned i = 0; i < STALL_BUSY_THREADS + 8; i++) { many[i] = 100 + i; more[i] = i; }
    assert(stall_busiest(&b, many, more, STALL_BUSY_THREADS + 8) == 100 + STALL_BUSY_THREADS - 1 && b.count == STALL_BUSY_THREADS);
    printf("ok  busiest thread: by the time since the last look\n");
}

// In the kernel by way of an SVC, as a thread blocked in a system call is.
static void regs_for(struct boot_progress_stall_regs *r, int32_t sc)
{
    memset(r, 0, sizeof(*r));
    r->valid = 1;
    r->x16 = (uint64_t)(int64_t)sc;
    r->esr = 0x56000080;
}

static void test_timeouts(void)
{
    struct boot_progress_stall_regs r;
    uint64_t ms = 0;
    regs_for(&r, -47);
    assert(stall_syscall_of(&r, 'W') == -47 && stall_syscall_of(&r, 'U') == -47);
    assert(!stall_syscall_of(&r, 'R') && !stall_syscall_of(&r, 'C'));     // preempted, or on core in user space: x16 and esr say nothing
    r.kernel = 1;
    assert(stall_syscall_of(&r, 'C') == -47);            // on core in the kernel, in that call
    r.esr = 0x92000006;
    assert(!stall_syscall_of(&r, 'W'));                  // in a page fault
    r.x[1] = 0x2;
    r.x[7] = 3137;
    assert(!stall_wait_timeout(-47, &r, &ms));           // no MACH_RCV_TIMEOUT
    r.x[1] = 0x100;
    assert(stall_wait_timeout(-47, &r, &ms) && ms == 3137);
    regs_for(&r, -31);
    r.x[1] = 0x102;
    r.x[5] = 6281;
    assert(stall_wait_timeout(-31, &r, &ms) && ms == 6281);
    regs_for(&r, -38);
    r.x[1] = 3;
    r.x[2] = 137000000;
    assert(stall_wait_timeout(-38, &r, &ms) && ms == 3137);
    regs_for(&r, -39);
    r.x[2] = 6;
    r.x[3] = 281000000;
    assert(stall_wait_timeout(-39, &r, &ms) && ms == 6281);
    regs_for(&r, -61);
    r.x[2] = 10;
    assert(stall_wait_timeout(-61, &r, &ms) && ms == 10);
    regs_for(&r, 230);
    r.x[2] = 25;
    assert(stall_wait_timeout(230, &r, &ms) && ms == 25);
    r.x[2] = (uint32_t)-1;
    assert(!stall_wait_timeout(230, &r, &ms));
    regs_for(&r, 305);
    r.x[6] = 12;
    r.x[7] = 571000000;
    assert(stall_wait_timeout(305, &r, &ms) && ms == 12571);
    r.x[6] = r.x[7] = 0;
    assert(!stall_wait_timeout(305, &r, &ms));
    regs_for(&r, 334);
    r.x[2] = 1;
    r.x[4] = 1;
    r.x[5] = 500000000;
    assert(stall_wait_timeout(334, &r, &ms) && ms == 1500);
    r.x[2] = 0;
    assert(!stall_wait_timeout(334, &r, &ms));
    regs_for(&r, 515);
    r.x[3] = 3137000;
    assert(stall_wait_timeout(515, &r, &ms) && ms == 3137);
    r.x[3] = 0;
    assert(!stall_wait_timeout(515, &r, &ms));
    regs_for(&r, 544);
    r.x[3] = 3137000000ULL;
    assert(stall_wait_timeout(544, &r, &ms) && ms == 3137);
    regs_for(&r, 301);
    assert(!stall_wait_timeout(301, &r, &ms));           // psynch_mutexwait has none
    printf("ok  system call timeouts\n");
}

static void test_slot(void)
{
    static struct stall_slot slot;
    int thread_a, thread_b;
    memset(&slot, 0, sizeof(slot));
    assert(stall_slot_ask(&slot, &thread_a) && stall_slot_wanted(&slot) == &thread_a && !stall_slot_answered(&slot));
    // The handler has not run: no new question.
    assert(!stall_slot_ask(&slot, &thread_b) && stall_slot_wanted(&slot) == &thread_a);
    stall_slot_answer(&slot, 1);
    assert(stall_slot_answered(&slot) && stall_slot_found(&slot) == 1);
    assert(stall_slot_ask(&slot, &thread_b) && stall_slot_wanted(&slot) == &thread_b && !stall_slot_found(&slot));
    stall_slot_answer(&slot, 0);
    assert(stall_slot_answered(&slot) && !stall_slot_found(&slot));
    assert(stall_slot_ask(&slot, &thread_a));
    stall_slot_cancel(&slot);                            // the call did not go out
    assert(stall_slot_ask(&slot, &thread_b));
    printf("ok  cross-call slot: a late answer never lands in a newer question\n");
}

// ---- the frame walk on simulated user memory ---------------------------------------------------------------------------------------
enum { STACK = 0x16f000000 };
#define SLIDE 0x10678000ULL
static uint8_t stack_page[0x1000];

static int fake_read(void *context, uint64_t address, void *buffer, uint32_t bytes)
{
    assert(context == (void *)0x5eed && !(address & 7) && !(bytes & 7));
    if (address < STACK || address + bytes > STACK + sizeof(stack_page)) return -1;
    memcpy(buffer, stack_page + (address - STACK), bytes);
    return 0;
}

static void put_frame(uint64_t fp, uint64_t next, uint64_t ret)
{
    memcpy(stack_page + (fp - STACK), &next, 8);
    memcpy(stack_page + (fp - STACK) + 8, &ret, 8);
}

static void fake_input(struct boot_progress_crash *c)
{
    memset(c, 0, sizeof(*c));
    c->pc = 0x1a9463e64ULL + SLIDE;
    c->lr = 0x1b0000010ULL + SLIDE;
    c->fp = STACK + 0x100;
    c->user_limit = 0x400000000ULL;
    c->cache_start = 0x180000000ULL + SLIDE;
    c->cache_end = 0x280000000ULL + SLIDE;
    c->cache_slide = SLIDE;
    c->context = (void *)0x5eed;
    c->read = fake_read;
}

static struct crash_trace trace;

static void test_walk(void)
{
    struct boot_progress_crash c;
    static struct stall_known known;
    memset(&known, 0, sizeof(known));
    fake_input(&c);
    memset(stack_page, 0, sizeof(stack_page));
    // More frames than a sample keeps.
    for (unsigned i = 0; i < 12; i++) put_frame(STACK + 0x100 + 0x40 * i, STACK + 0x140 + 0x40 * i, 0x1b0001000ULL + SLIDE + i * 16);
    stall_trace_take(&c, &trace);
    assert(trace.frames == STALL_FRAMES && trace.stop == CRASH_STOP_MAX);
    assert(trace.pc.kind == CRASH_CACHE && trace.pc.value == 0x1a9463e64ULL && trace.frame[3].value == 0x1b0001030ULL);
    assert(!stall_trace_unknown(&trace, &known, 38));
    // A frame outside the cache stays raw, and is asked about until its mapping is known.
    put_frame(STACK + 0x100, STACK + 0x140, 0x104001234ULL);
    stall_trace_take(&c, &trace);
    assert(trace.frame[0].kind == CRASH_RAW && trace.frame[0].value == 0x104001234ULL);
    assert(stall_trace_unknown(&trace, &known, 38) == 0x104001234ULL);
    stall_known_add(&known, 38, 0x104000000ULL, 0x104200000ULL);
    assert(!stall_trace_unknown(&trace, &known, 38));
    assert(stall_trace_unknown(&trace, &known, 39) == 0x104001234ULL);   // another process's mapping
    for (unsigned i = 0; i < STALL_KNOWN; i++) stall_known_add(&known, 40, i, i + 1);
    assert(stall_trace_unknown(&trace, &known, 38) == 0x104001234ULL);   // pushed out
    // A chain into a page that is not resident ends with read, no fault; no frame pointer gives none.
    put_frame(STACK + 0x100, STACK + 0x4000, 0x1b0000010ULL + SLIDE);
    stall_trace_take(&c, &trace);
    assert(trace.frames == 1 && trace.stop == CRASH_STOP_READ && trace.stop_at == STACK + 0x4000);
    c.fp = 0;
    stall_trace_take(&c, &trace);
    assert(!trace.frames && trace.stop == CRASH_STOP_END);
    printf("ok  frame walk: 8 frames, cache and raw addresses, the mappings to look up, a page not resident\n");
}

// ---- records ----------------------------------------------------------------------------------------------------------------------------
static char line[STALL_LINE + 8];

static void check_line(void)
{
    assert(strlen(line) <= STALL_LINE && strlen(line) < RAMOOPS_LOG_TEXT_MAX);
    for (unsigned i = 0; line[i]; i++) assert(line[i] >= 0x20 && line[i] < 0x7f);
}

// One record of each kind, with the longest values; every field must survive, the last one too.
static void test_lengths(void)
{
    struct stall_sample_head h;
    memset(&h, 0, sizeof(h));
    h.id = UINT32_MAX;
    h.stall = UINT32_MAX;
    h.sample = UINT32_MAX;
    h.why = "idleq";
    h.age_ms = 999999999;
    h.touch_ms = 999999999;
    h.gpu_ms = UINT64_MAX;
    h.frames_before = 3;
    h.pid = INT32_MAX;
    h.name = "AVeryLongProcessName";
    h.open = UINT32_MAX;
    h.cpu = 7;
    stall_line_head(line, &h);
    check_line();
    assert(strstr(line, " cpu=7") && strstr(line, "gpu=-") && !strstr(line, " since="));
    h.gap_ms = 8000;
    h.since_ms = 999999999;
    h.sign = STALL_SIGN_SURE;
    h.focal = 12;
    stall_line_head(line, &h);
    check_line();
    assert(strstr(line, " why=gap8000 ") && strstr(line, " since=999999999 ask=2 fg=12 pid="));
    h.focal = -1;
    stall_line_head(line, &h);
    assert(strstr(line, " ask=2 fg=-1 "));
    static struct boot_progress_stall_thread t;
    memset(&t, 0, sizeof(t));
    t.tid = UINT64_MAX >> 8;
    t.state = 1;
    t.sched_pri = 127;
    t.base_pri = 127;
    t.cpu = -1;
    t.hint = 30;
    t.wait_event = 0xfffffe1000000000ULL;
    t.last_run = MS(1);
    t.kframes = BOOT_PROGRESS_STALL_KFRAMES;
    for (unsigned i = 0; i < BOOT_PROGRESS_STALL_KFRAMES; i++) t.kframe[i] = 0xfffffff00b8fe8b4ULL;
    regs_for(&t.user, -47);
    t.user.x[1] = 0x100000103ULL;
    t.user.x[7] = 3137;
    struct stall_thread_view v;
    v.id = 123456;
    v.tag = "app";
    v.pid = 448;
    v.name = "AVeryLongProcessName";
    v.t = &t;
    v.regs = &t.user;
    v.now = MS(1000000);
    v.hz = HZ;
    v.since = 0;
    v.xcall = NULL;
    stall_line_thread(line, &v);
    check_line();
    assert(strstr(line, " st=W ") && strstr(line, " sc=-47 to=3137 o=0x100000103 ") && strstr(line, "k:b8fe8b4,k:"));
    t.kframes = 0;
    t.continuation = 0xfffffff00a000000ULL;
    stall_line_thread(line, &v);
    assert(strstr(line, " cont=k:a000000"));
    t.continuation = 0;
    t.state = 0x04;
    t.cpu = 3;
    v.since = MS(999999);
    v.xcall = "ok";
    stall_line_thread(line, &v);
    assert(!strstr(line, " sc="));                       // on core in user space
    assert(strstr(line, " st=C ") && strstr(line, " cpu=3 ") && strstr(line, " wt=1.0 ") && strstr(line, " x=ok "));
    t.cpu = -1;
    t.made_runnable = MS(999998);
    stall_line_thread(line, &v);
    assert(strstr(line, " st=R ") && strstr(line, " wt=2.0 "));
    t.state = 0x09;
    stall_line_thread(line, &v);
    assert(strstr(line, " st=U "));
    t.state = 0x14;
    stall_line_thread(line, &v);
    assert(strstr(line, " st=T "));
    t.state = 0;
    stall_line_thread(line, &v);
    assert(strstr(line, " st=? "));
    // bt: end= must fit whatever the frames take.
    memset(&trace, 0, sizeof(trace));
    trace.frames = STALL_FRAMES;
    trace.pc.kind = trace.lr.kind = CRASH_CACHE;
    trace.pc.value = trace.lr.value = 0x1fffffffffULL;
    for (unsigned i = 0; i < STALL_FRAMES; i++) {
        trace.frame[i].kind = CRASH_RAW;
        trace.frame[i].value = 0x3ffffffffULL;
    }
    trace.stop = CRASH_STOP_READ;
    trace.stop_at = 0x3ffffffffULL;
    struct boot_progress_stall_regs r;
    regs_for(&r, 0);
    r.kernel = 1;
    r.kpc = 0xfffffff00b8fe8b4ULL;
    r.esr = 0x92000006;
    r.far = 0x3ffffffffULL;
    stall_line_bt(line, 123456, "app", INT32_MAX, &r, &trace);
    check_line();
    assert(strstr(line, " end=read@0x3ffffffff") && strstr(line, " kpc=k:b8fe8b4 "));
    static struct stall_cpu_view cpus[STALL_CPUS];
    memset(cpus, 0, sizeof(cpus));
    for (unsigned c = 0; c < STALL_CPUS; c++) {
        cpus[c].ok = 1;
        cpus[c].sched.kind = STALL_CPU_RUNNING;
        cpus[c].sched.runq = 12;
        cpus[c].sched.rt = 1;
        cpus[c].on.pid = 448;
        cpus[c].on.pri = 127;
        cpus[c].on.tid = 0xfffff;
        strcpy(cpus[c].on.name, "AVeryLongProcess");
    }
    stall_line_cpus(line, 123456, 0, 4, cpus, MS(99999), HZ);
    check_line();
    assert(strstr(line, " 3:AVeryLongPro/fffff/127/99999 rq=12 rt=1"));
    cpus[4].on.pid = -1;
    cpus[5].on.pid = -1;
    cpus[5].sched.kind = STALL_CPU_DISPATCHING;
    cpus[6].sched.kind = STALL_CPU_OFF;
    cpus[7].ok = 0;
    stall_line_cpus(line, 1, 4, 4, cpus, MS(10), HZ);
    assert(strstr(line, " 4:-/10 5:~/10 6:x 7:? "));
    cpus[0].on.pid = 0;
    stall_line_cpus(line, 1, 0, 1, cpus, MS(10), HZ);
    assert(strstr(line, " 0:k/fffff/127/10 "));
    static struct stall_second second;
    memset(&second, 0, sizeof(second));
    for (unsigned b = 0; b < STALL_BANDS; b++) {
        second.band[b].n = 99999;
        second.band[b].over10 = second.band[b].over50 = second.band[b].over200 = 9999;
        second.band[b].max_ticks = MS(9999);
        second.band[b].tid = 0xfffff;
        second.band[b].pid = 99999;
        second.band[b].pri = 127;
        strcpy(second.band[b].name, "AVeryLongProcess");
    }
    second.band[STALL_KERNEL].pid = 0;
    second.band[STALL_KERNEL].name[0] = 0;
    stall_line_lat(line, &second, HZ);
    check_line();
    // All four at their longest: the numbers of each survive, and threads only as long as they leave room for them.
    assert(strstr(line, "lat fg 99999/9999/9999/9999/9999 AVeryLongPro[99999]/fffff/127 def 99999/9999/9999/9999/9999"));
    assert(strstr(line, " bg 99999/9999/9999/9999/9999") && strstr(line, " k 99999/9999/9999/9999/9999"));
    // With three bands there is room for all of them.
    second.band[STALL_KERNEL].n = 0;
    stall_line_lat(line, &second, HZ);
    check_line();
    assert(strstr(line, "bg 99999/9999/9999/9999/9999 AVeryLongPro[99999]/fffff/127") && !strstr(line, " k "));
    second.band[STALL_KERNEL].n = 99999;
    second.band[STALL_DEF].n = 0;
    second.band[STALL_BG].max_ticks = MS(1) / 2;
    stall_line_lat(line, &second, HZ);
    check_line();
    assert(!strstr(line, " def ") && strstr(line, "bg 99999/0.5/9999/9999/9999 k 99999/9999/9999/9999/9999 fffff/127"));
    uint32_t timers[STALL_CPUS];
    for (unsigned c = 0; c < STALL_CPUS; c++) {
        second.idle[c] = second.sent[c] = second.recv[c] = 9999;
        second.ipi_max[c] = MS(999);
        timers[c] = 9999;
    }
    stall_line_cpu(line, &second, timers);
    check_line();
    assert(strstr(line, " tmr 9999,9999,9999,9999,9999,9999,9999,9999"));
    struct stall_ipi_line ipi;
    memset(&ipi, 0, sizeof(ipi));
    for (unsigned c = 0; c < STALL_CPUS; c++) ipi.hungry[c] = ipi.stuck[c] = 100;
    ipi.tick_ticks = MS(9999);
    ipi.images = 999;
    stall_line_ipi(line, &second, &ipi, HZ);
    check_line();
    assert(strstr(line, " tick 9999 img 999"));
    struct stall_io_line io;
    memset(&io, 0, sizeof(io));
    io.reads = 99999;
    io.read_ticks = io.verify_ticks = MS(9999);
    io.free_min = 99999;
    io.available = 999999;
    io.faults = 999999;
    io.pageins = io.compressions = io.decompressions = io.page_waits = 99999;
    io.throttled[0] = io.throttled[1] = io.throttled[2] = 9999;
    io.used = 2000;
    io.over_rate = io.over_budget = 9999;
    io.kbt_used = 2000;
    io.kbt_over_rate = io.kbt_over_budget = 9999;
    io.tail_used = 800;
    io.tail_over_rate = io.tail_over_budget = 9999;
    stall_line_io(line, &io, HZ);
    check_line();
    assert(strstr(line, " dcmp 99999 pw 99999 t1 ") && strstr(line, " q 2000/9999/9999 kq 2000/9999/9999 tq 800/9999/9999"));
    struct stall_mem_line mem;
    memset(&mem, 0, sizeof(mem));
    mem.mem.wired = mem.mem.internal = mem.mem.external = mem.mem.compressor = mem.mem.compressed = 2097152;
    mem.mem.throttled = mem.mem.purgeable = mem.mem.zone = 2097152;
    mem.ram_bytes = 2147483648ULL;
    mem.pool_mib = mem.pool_peak_mib = 1024;
    stall_line_mem(line, &mem);
    check_line();
    assert(!strcmp(line, "mem wire 2097152 int 2097152 ext 2097152 cmp 2097152/2097152 thr 2097152 pur 2097152 zone 2097152 ram 2147483648 gpu 1024/1024"));
    // Its turn: the first pass, then every STALL_MEM_EVERY_S, however the passes fall.
    uint64_t next = 0, at = MS(1000);
    assert(stall_mem_due(at, HZ, &next) && next == at + (uint64_t)HZ * STALL_MEM_EVERY_S);
    assert(!stall_mem_due(at + MS(1), HZ, &next) && !stall_mem_due(at + (uint64_t)HZ * STALL_MEM_EVERY_S - 1, HZ, &next));
    assert(stall_mem_due(at + (uint64_t)HZ * STALL_MEM_EVERY_S, HZ, &next));
    unsigned mems = 0;
    next = 0;
    for (unsigned second_passed = 0; second_passed < 60; second_passed++) mems += (unsigned)stall_mem_due(MS(1000 * second_passed + second_passed / 2), HZ, &next);
    assert(mems == 12);
    struct boot_progress_stall_task task;
    memset(&task, 0, sizeof(task));
    task.pid = 38;
    strcpy(task.name, "backboardd");
    task.cache_start = 1;
    task.cache_end = 2;
    task.cache_slide = SLIDE;
    task.main_header = 0x100a4c000ULL;
    stall_line_map(line, &task);
    check_line();
    assert(!strcmp(line, "map pid=38 backboardd slide=0x10678000 main=0x100a4c000"));
    struct boot_progress_crash_image image;
    memset(&image, 0, sizeof(image));
    image.start = 0x104000000ULL;
    image.end = 0x104200000ULL;
    image.offset = 0x4000;
    strcpy(image.name, "NativeAdrenoMetal");
    stall_line_img(line, 38, &image);
    check_line();
    assert(!strcmp(line, "img pid=38 start=0x104000000 end=0x104200000 off=0x4000 NativeAdrenoMetal"));
    char tail[48];
    assert(stall_fps_tail(tail, sizeof(tail), 23, 12, 1) && !strcmp(tail, " | tch 23 last 12 vl 1"));
    stall_fps_tail(tail, sizeof(tail), 0, UINT64_MAX, 0);
    assert(!strcmp(tail, " | tch 0 last - vl 0"));
    assert(!stall_fps_tail(tail, 0, 1, 1, 1));
    printf("ok  record lengths with worst-case values (all within %u characters)\n", STALL_LINE);
}

// ---- through RamoopsLog.c ----------------------------------------------------------------------------------------------------------------
static void put_record(char kind, const char *text, FILE *f)
{
    ramoops_log_line(kind, text);
    if (f) fprintf(f, "%c\t%s\n", kind, text);
}

// A run: gaps at three thresholds of backboardd waiting in mach_msg, a key with the App on core, the seconds around them.
static void scene(FILE *f)
{
    if (!region) region = calloc(1, REGION);
    assert(region);
    assert(ramoops_log_vm_ready());
    ramoops_log_line('S', "S6: kernel VM and exception vectors ready");
    static struct stall_seen seen;
    static struct stall_second second;
    state_reset();
    memset(&seen, 0, sizeof(seen));
    run_on(0, 448, "MobileNotes", 47, 100, MS(240), 0x2b5);
    run_on(2, 38, "backboardd", 54, 102, MS(3), 0x100);
    run_on(5, 0, NULL, 91, 103, MS(12), 0x300);
    stall_note_idle(&s, 1);
    stall_note_ipi_send(&s, 1, MS(1));
    stall_note_ipi_recv(&s, 1, MS(2));
    stall_second_take(&s, &seen, &second);
    stall_line_lat(line, &second, HZ);
    put_record('M', line, f);
    uint32_t timers[STALL_CPUS] = {100, 90, 80, 70, 60, 50, 40, 30};
    stall_line_cpu(line, &second, timers);
    put_record('M', line, f);
    struct stall_ipi_line ipi;
    memset(&ipi, 0, sizeof(ipi));
    ipi.tick_ticks = MS(11);
    stall_line_ipi(line, &second, &ipi, HZ);
    put_record('M', line, f);
    struct stall_io_line io;
    memset(&io, 0, sizeof(io));
    io.reads = 12;
    io.read_ticks = MS(34);
    io.verify_ticks = MS(1);
    io.free_min = 3456;
    io.available = 322773;
    io.faults = 1200;
    io.pageins = 12;
    io.page_waits = 3;
    io.used = 42;
    io.kbt_used = 21;
    io.tail_used = 9;
    stall_line_io(line, &io, HZ);
    put_record('M', line, f);
    struct stall_mem_line mem;
    memset(&mem, 0, sizeof(mem));
    mem.mem.wired = 240000;
    mem.mem.internal = 360000;
    mem.mem.external = 250000;
    mem.mem.throttled = 90000;
    mem.mem.purgeable = 1200;
    mem.mem.zone = 110000;
    mem.ram_bytes = 1234567890ULL;
    mem.pool_mib = 400;
    mem.pool_peak_mib = 921;
    stall_line_mem(line, &mem);
    put_record('M', line, f);
    struct boot_progress_stall_task task;
    memset(&task, 0, sizeof(task));
    task.pid = 38;
    strcpy(task.name, "backboardd");
    task.cache_start = 1;
    task.cache_end = 2;
    task.cache_slide = SLIDE;
    task.main_header = 0x100a4c000ULL;
    stall_line_map(line, &task);
    put_record('Q', line, f);
    struct stall_sample_head h;
    memset(&h, 0, sizeof(h));
    h.id = 1;
    h.gap_ms = 110;
    h.stall = 1;
    h.sample = 2;
    h.age_ms = 113;
    h.since_ms = 122;
    h.sign = STALL_SIGN_SURE;
    h.focal = 1;
    h.touch_ms = 20;
    h.gpu_ms = 90;
    h.frames_before = 3;
    h.cpu = -1;
    stall_line_head(line, &h);
    put_record('Q', line, f);
    static struct stall_cpu_view cpus[STALL_CPUS];
    memset(cpus, 0, sizeof(cpus));
    for (unsigned c = 0; c < STALL_CPUS; c++) {
        cpus[c].ok = 1;
        cpus[c].sched.kind = STALL_CPU_IDLE;
        cpus[c].on.pid = -1;
    }
    cpus[3].sched.kind = STALL_CPU_RUNNING;
    cpus[3].sched.runq = 2;
    cpus[3].on.pid = 448;
    cpus[3].on.pri = 47;
    cpus[3].on.tid = 0x2b5;
    strcpy(cpus[3].on.name, "MobileNotes");
    stall_line_cpus(line, 1, 0, 4, cpus, MS(100), HZ);
    put_record('Q', line, f);
    stall_line_cpus(line, 1, 4, 4, cpus, MS(100), HZ);
    put_record('Q', line, f);
    static struct boot_progress_stall_thread t;
    memset(&t, 0, sizeof(t));
    t.tid = 0x100;
    t.state = 1;
    t.sched_pri = 54;
    t.base_pri = 54;
    t.cpu = -1;
    t.hint = 2;
    t.wait_event = 0xfffffe1000001234ULL;
    t.last_run = MS(1000);
    t.kframes = 2;
    t.kframe[0] = 0xfffffff007123456ULL;
    t.kframe[1] = 0xfffffff007234568ULL;
    regs_for(&t.user, -47);
    t.user.x[1] = 0x103;
    t.user.x[7] = 3137;
    t.user.pc = 0x1a9463e64ULL + SLIDE;
    t.user.lr = 0x1b0000010ULL + SLIDE;
    t.user.fp = STACK + 0x100;
    struct stall_thread_view v;
    v.id = 1;
    v.tag = "rs";
    v.pid = 38;
    v.name = "backboardd";
    v.t = &t;
    v.regs = &t.user;
    v.now = MS(1100);
    v.hz = HZ;
    v.since = 0;
    v.xcall = NULL;
    stall_line_thread(line, &v);
    put_record('Q', line, f);
    struct boot_progress_crash c;
    fake_input(&c);
    memset(stack_page, 0, sizeof(stack_page));
    put_frame(STACK + 0x100, STACK + 0x140, 0x1b0000010ULL + SLIDE);
    put_frame(STACK + 0x140, 0, 0x104001234ULL);
    stall_trace_take(&c, &trace);
    stall_line_bt(line, 1, "rs", 38, &t.user, &trace);
    put_record('Q', line, f);
    // The App on screen, blocked in a page fault of its own while the render server waits for work.
    v.tag = "fg";
    v.pid = 448;
    v.name = "Preferences";
    stall_line_thread(line, &v);
    put_record('Q', line, f);
    stall_line_bt(line, 1, "fg", 448, &t.user, &trace);
    put_record('Q', line, f);
    memset(&h, 0, sizeof(h));
    h.id = 2;
    h.why = "in";
    h.stall = 7;
    h.sample = 1;
    h.age_ms = 300;
    h.touch_ms = 10;
    h.gpu_ms = UINT64_MAX;
    h.pid = 448;
    h.name = "MobileNotes";
    h.open = 1;
    h.cpu = -1;
    stall_line_head(line, &h);
    put_record('Q', line, f);
    struct crash_text x;
    stall_head(&x, line, 2, "no ");
    crash_put_str(&x, "kbd");
    put_record('Q', line, f);
    struct boot_progress_crash_image image;
    memset(&image, 0, sizeof(image));
    image.start = 0x104000000ULL;
    image.end = 0x104200000ULL;
    strcpy(image.name, "NativeAdrenoMetal");
    stall_line_img(line, 38, &image);
    put_record('Q', line, f);
}

static void test_ramoops(void)
{
    scene(NULL);
    // The rings hold the records; which ring is the writer's decision, read back from its own table.
    assert(area_of(RAMOOPS_KIND_METER, "lat") == AREA_CONSOLE && area_of(RAMOOPS_KIND_STALL, "s=1") == AREA_EVENTS);
    assert(!ramoops_log_resetting());
    printf("ok  records through RamoopsLog.c: M to the console ring, Q to the events ring\n");
}

static void dump(const char *directory)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/expected.tsv", directory);
    FILE *f = fopen(path, "w");
    assert(f);
    scene(f);
    assert(!fclose(f));
    for (unsigned z = 0; z < 2; z++) {
        snprintf(path, sizeof(path), "%s/%s.zone", directory, z ? "pmsg" : "console");
        f = fopen(path, "wb");
        assert(f && fwrite(region + (z ? zone_size[0] : 0), 1, zone_size[z], f) == zone_size[z] && !fclose(f));
    }
    printf("dumped\n");
}

int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "--dump")) { dump(argv[2]); return 0; }
    test_dispatch();
    test_ipi();
    test_limit();
    test_gap();
    test_gap_budget();
    test_kbt();
    test_busy();
    test_timeouts();
    test_slot();
    test_walk();
    test_lengths();
    test_ramoops();
    printf("stall probe: all tests passed\n");
    return 0;
}
