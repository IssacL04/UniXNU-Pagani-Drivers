// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test of GunyahWdog.c against a fake Gunyah hypervisor that keeps the primary VM's watchdog the way
// quic/gunyah-hypervisor hyp/misc/watchdog/src/watchdog.c does (bark, then bite, both counted from the last pat; a bite
// disables the watchdog) and answers the SMCCC calls of the phone's gh_virt_wdt.ko. Checked: call IDs, arguments, units
// and order; the probe; clamping; pets only from the pet thread and the observer and their cadence; a hang bites at the
// last pet + bite; stop before an intentional reset and before a panic reset; armed after a panic without reset; the
// stop-pet test; the HVC fallback; pagani-wdog=0 and absent. The boot CPU's heartbeat (the audit's G5): started before the
// enable, ended with the pets; no pet once it or the observer has not run for 3 s (the boot CPU wedged, the observer stuck:
// the bite comes on time while the pet thread runs on), pets again when both are back; an idle boot CPU, woken by the
// heartbeat's own timer only, and late wakes under 3 s are no hang; a kernel without an observer is judged by the heartbeat.
//   xcrun clang -std=c11 -O2 -Wall -Wextra -Werror -fsanitize=address,undefined Test/gunyah-wdog.c -o gunyah-wdog
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define GUNYAH_WDOG_TEST 1
#include "../GunyahWdog.c"

#define HZ 19200000ull
#define MS(ms) ((uint64_t)(ms) * (HZ / 1000))

// Boot-args.
static struct { const char *name; uint32_t value; } args[4];
static unsigned arg_count;
static void set_args(const char *n0, uint32_t v0, const char *n1, uint32_t v1, const char *n2, uint32_t v2)
{
    arg_count = 0;
    if (n0) { args[arg_count].name = n0; args[arg_count++].value = v0; }
    if (n1) { args[arg_count].name = n1; args[arg_count++].value = v1; }
    if (n2) { args[arg_count].name = n2; args[arg_count++].value = v2; }
}
int gunyah_wdog_platform_arg(const char *name, uint32_t *value)
{
    for (unsigned i = 0; i < arg_count; i++)
        if (!strcmp(args[i].name, name)) { *value = args[i].value; return 1; }
    return 0;
}

static uint64_t now_ticks;
uint64_t gunyah_wdog_platform_counter(void) { return now_ticks; }
uint64_t gunyah_wdog_platform_frequency(void) { return HZ; }

// The fake hypervisor.
static struct {
    int absent;                 // nothing answers: every call returns -1
    int all_ones;               // -1 as 0xffffffffffffffff instead of 0x00000000ffffffff
    int enabled, barked, bitten;
    uint32_t bark, bite;        // ms; 0x8000 when never set (the clamp of an unset time)
    uint64_t last_pat, bark_at, bite_at;
    int32_t fail_set_time, fail_enable;
} hyp;
static struct call { int hvc; uint32_t function, a1, a2; uint64_t at; } calls[4096];
static unsigned ncalls;

static void hyp_reset(void)
{
    memset(&hyp, 0, sizeof(hyp));
    hyp.bark = hyp.bite = 0x8000;
    ncalls = 0;
}

// Barks and bites due by `until`, in the hypervisor's order.
static void hyp_advance(uint64_t until)
{
    if (!hyp.enabled) return;
    uint64_t bark_at = hyp.last_pat + MS(hyp.bark), bite_at = hyp.last_pat + MS(hyp.bite);
    if (!hyp.barked && hyp.bark < hyp.bite && bark_at <= until) { hyp.barked = 1; hyp.bark_at = bark_at; }
    if (bite_at <= until) { hyp.bitten = 1; hyp.bite_at = bite_at; hyp.enabled = 0; }
}

static uint32_t clamp_ms(uint32_t ms) { return ms >= 0x8000 ? 0x8000 : ms; }

void gunyah_wdog_platform_call(int hvc, uint32_t function, uint32_t a1, uint32_t a2, uint64_t result[3])
{
    assert(ncalls < sizeof(calls) / sizeof(calls[0]));
    calls[ncalls++] = (struct call){hvc, function, a1, a2, now_ticks};
    hyp_advance(now_ticks);
    const uint64_t minus_one = hyp.all_ones ? UINT64_MAX : 0xffffffffull;
    result[0] = result[1] = result[2] = 0x5a5a5a5a5a5a5a5aull;    // unspecified registers must not matter
    if (hyp.absent) { result[0] = minus_one; return; }
    switch (function) {
    case GUNYAH_WDOG_FN_STATUS:
        result[0] = 0;
        result[1] = (uint32_t)hyp.enabled | (hyp.enabled && hyp.barked ? GUNYAH_WDOG_STATUS_EXPIRED : 0);
        result[2] = (now_ticks - hyp.last_pat) / (HZ / 1000);
        return;
    case GUNYAH_WDOG_FN_PET:
        if (hyp.enabled) { hyp.last_pat = now_ticks; hyp.barked = 0; }
        result[0] = 0;
        return;
    case GUNYAH_WDOG_FN_SET_TIME:
        if (hyp.fail_set_time) { result[0] = (uint32_t)hyp.fail_set_time; return; }
        if (a1 != GUNYAH_WDOG_TIME_UNCHANGED) hyp.bark = clamp_ms(a1);
        if (a2 != GUNYAH_WDOG_TIME_UNCHANGED) hyp.bite = clamp_ms(a2);
        result[0] = 0;
        return;
    case GUNYAH_WDOG_FN_CONTROL:
        // Like SM8650: disabling a disabled watchdog is an invalid argument.
        if (a1 == GUNYAH_WDOG_CTRL_ENABLE) {
            if (hyp.fail_enable) { result[0] = (uint32_t)hyp.fail_enable; return; }
            if (!hyp.enabled) { hyp.enabled = 1; hyp.barked = 0; hyp.last_pat = now_ticks; }
            result[0] = 0;
        } else if (a1 == GUNYAH_WDOG_CTRL_DISABLE && hyp.enabled) {
            hyp.enabled = 0;
            result[0] = 0;
        } else result[0] = 1;
        return;
    default:
        result[0] = minus_one;
    }
}

static int thread_ok = 1;
static unsigned threads_started, thread_started_after_call;
int gunyah_wdog_platform_start_thread(void)
{
    threads_started++;
    thread_started_after_call = ncalls;
    return thread_ok;
}

// The boot CPU's heartbeat timer: started on beat_cpu_value (-1: it does not start); run() fires it every period from its start
// (gunyah_wdog_host_beat) while sim.beat is set, the way a local timer wakes its CPU, idle or not.
static int beat_cpu_value;
static unsigned beats_started, beat_started_after_call;
static uint64_t beat_started_at;
int gunyah_wdog_platform_start_beat(void)
{
    beats_started++;
    beat_started_after_call = ncalls;
    beat_started_at = now_ticks;
    return beat_cpu_value;
}

// The ramoops log.
static struct { char kind; char text[256]; } lines[512];
static unsigned nlines;
void ramoops_log_line(char kind, const char *text)
{
    assert(nlines < sizeof(lines) / sizeof(lines[0]));
    assert(strlen(text) < RAMOOPS_LOG_TEXT_MAX);      // one record each
    lines[nlines].kind = kind;
    snprintf(lines[nlines].text, sizeof(lines[nlines].text), "%s", text);
    nlines++;
}
static const char *panic_footer_text;
const char *ramoops_log_panic_footer(void) { return panic_footer_text; }
static int flush_pending;                   // the UFS log's last snapshot is still to come
int ufs_log_panic_pending(void) { return flush_pending; }

static int logged(char kind, const char *text)
{
    for (unsigned i = 0; i < nlines; i++)
        if (lines[i].kind == kind && strstr(lines[i].text, text)) return 1;
    return 0;
}
static unsigned logged_count(char kind, const char *text)
{
    unsigned n = 0;
    for (unsigned i = 0; i < nlines; i++)
        if (lines[i].kind == kind && strstr(lines[i].text, text)) n++;
    return n;
}

static unsigned count_calls(uint32_t function, unsigned from)
{
    unsigned n = 0;
    for (unsigned i = from; i < ncalls; i++) n += calls[i].function == function;
    return n;
}

static void expect_call(unsigned i, uint32_t function, uint32_t a1, uint32_t a2)
{
    assert(i < ncalls);
    if (calls[i].function != function || calls[i].a1 != a1 || calls[i].a2 != a2 || calls[i].hvc) {
        fprintf(stderr, "call %u: %#x(%u, %u) hvc %d, expected %#x(%u, %u)\n", i, calls[i].function, calls[i].a1, calls[i].a2,
                calls[i].hvc, function, a1, a2);
        assert(0);
    }
}

// A fresh boot: the module, the hypervisor, the log and time.
static void boot(void)
{
    wdog_started = 0;
    hyp_reset();
    nlines = 0;
    threads_started = 0;
    thread_started_after_call = 0;
    thread_ok = 1;
    beat_cpu_value = 0;
    beats_started = 0;
    beat_started_after_call = 0;
    beat_started_at = 0;
    panic_footer_text = NULL;
    flush_pending = 0;
    now_ticks = MS(7000);       // boot_progress_start comes some seconds into the boot
}

// Runs the boot CPU's heartbeat (every period from its start, while beat is set: cleared, the boot CPU takes no interrupt), the pet
// thread (rounds as wdog_thread_step asks) and the 1 Hz observer (offset by half a second) until `until` ms, with the hypervisor's
// barks and bites in between; at the same instant the heartbeat comes first, then the thread, then the observer.
struct sim { int thread, observer, beat, thread_done, beat_done; uint64_t thread_at, observer_at, beat_at; };
static void sim_start(struct sim *s)
{
    s->thread = s->observer = s->beat = 1;
    s->thread_done = 0;
    s->beat_done = !beats_started || beat_cpu_value < 0;
    s->thread_at = now_ticks + MS(1000);
    s->observer_at = now_ticks + MS(500);
    s->beat_at = beat_started_at + MS(GUNYAH_WDOG_BEAT_MS);
}
static void run(struct sim *s, uint64_t until_ms)
{
    uint64_t until = MS(until_ms);
    assert(until >= now_ticks);
    for (;;) {
        uint64_t next = until;
        if (s->beat && !s->beat_done && s->beat_at < next) next = s->beat_at;
        if (s->thread && !s->thread_done && s->thread_at < next) next = s->thread_at;
        if (s->observer && s->observer_at < next) next = s->observer_at;
        assert(next >= now_ticks);
        hyp_advance(next);
        now_ticks = next;
        if (s->beat && !s->beat_done && s->beat_at == next) {
            if (gunyah_wdog_host_beat()) s->beat_at = next + MS(GUNYAH_WDOG_BEAT_MS);
            else s->beat_done = 1;
        } else if (s->thread && !s->thread_done && s->thread_at == next) {
            uint32_t ms = gunyah_wdog_host_thread_step();
            if (ms) s->thread_at = next + MS(ms);
            else s->thread_done = 1;
        } else if (s->observer && s->observer_at == next) {
            gunyah_wdog_tick();
            s->observer_at = next + MS(1000);
        } else break;
    }
}

static void test_off(void)
{
    for (int with_zero = 0; with_zero < 2; with_zero++) {
        boot();
        if (with_zero) set_args("pagani-wdog", 0, "pagani-wdog-stop-pet", 5, NULL, 0);
        else set_args("pagani-wdog-stop-pet", 5, NULL, 0, NULL, 0);
        gunyah_wdog_start();
        struct sim s;
        sim_start(&s);
        run(&s, 60000);
        gunyah_wdog_panic();
        assert(!gunyah_wdog_panic_footer());
        gunyah_wdog_before_reset("timer");
        assert(!ncalls && !threads_started && !beats_started && !nlines && !gunyah_wdog_status() && s.thread_done && s.beat_done);
    }
}

// The arming sequence, its units and the configuration lines; then healthy pets for ten minutes.
static void test_arm_and_cadence(void)
{
    boot();
    set_args("pagani-wdog", 20, NULL, 0, NULL, 0);
    gunyah_wdog_start();
    assert(ncalls == 5);
    expect_call(0, 0x86000006, 0, 0);           // STATUS: the probe
    expect_call(1, 0x86000008, 10000, 20000);   // SET_TIME: bark = bite / 2, in ms
    expect_call(2, 0x86000005, 3, 0);           // CONTROL: enable (bits 0 and 1)
    expect_call(3, 0x86000007, 0, 0);           // PET
    expect_call(4, 0x86000006, 0, 0);           // STATUS: read back
    assert(threads_started == 1 && thread_started_after_call == 1);     // the pet thread exists before SET_TIME
    assert(beats_started == 1 && beat_started_after_call == 1);         // so does the boot CPU's heartbeat
    assert(hyp.enabled && hyp.bark == 10000 && hyp.bite == 20000);
    assert(lines[0].kind == 'T' && !strcmp(lines[0].text,
           "wdog config: pagani-wdog=20 bite 20000 ms bark 10000 ms pets every 1000 ms conduit smc"));
    assert(lines[1].kind == 'T' && !strcmp(lines[1].text,
           "wdog probe: STATUS(0x86000006)=0 enabled 0 expired 0 since_pet 7000 ms"));
    assert(lines[2].kind == 'T' && !strcmp(lines[2].text,
           "wdog heartbeat: a local timer on cpu 0 every 1000 ms; a pet needs a beat and an observer pass within the last "
           "3000 ms"));
    assert(lines[3].kind == 'T' && !strcmp(lines[3].text,
           "wdog armed: SET_TIME(0x86000008, bark 10000, bite 20000)=0 CONTROL(0x86000005, 3)=0 PET(0x86000007)=0 "
           "STATUS(0x86000006)=0 enabled 1 expired 0 since_pet 0 ms"));
    assert(nlines == 4 && !strcmp(gunyah_wdog_status(), "wdog on: bite 20 s, bark at half, Gunyah smc"));
    // Ten minutes: one pet a second from each path, nothing else, no bark, no bite.
    unsigned armed = ncalls;
    struct sim s;
    sim_start(&s);
    run(&s, 7000 + 600000);
    assert(!hyp.bitten && !hyp.barked && hyp.enabled);
    assert(count_calls(GUNYAH_WDOG_FN_PET, armed) == ncalls - armed);
    assert(wdog_pets[0] == 600 && wdog_pets[1] == 600 && !wdog_pet_failures);
    assert(wdog_ms(wdog_max_gap) <= 1000);
    assert(wdog_beats == 600 && wdog_ms(wdog_beat_max_gap) == 1000 && wdog_observer_passes == 600 && !wdog_withheld_count);
    assert(logged_count('U', "wdog ok: armed ") == 10 && !logged('T', "withheld"));
    assert(logged('U', "wdog ok: armed 600 s, pets thread 600 observer 600 failed 0, longest gap 500 ms (bark 10000), cpu 0 "
                       "beats 600 (gap <= 1000 ms), withheld 0"));
    // The observer alone keeps it quiet (the pet thread stalled): alive, it pets. (The pet thread alone no longer does: test_alive.)
    s.thread = 0;
    run(&s, 7000 + 700000);
    assert(!hyp.bitten && !hyp.barked && !wdog_withheld_count);
    // A hang: neither path runs any more (the boot CPU still beats). The bark comes bark ms after the last pet, the bite bite ms
    // after it.
    uint64_t last = hyp.last_pat;
    s.observer = 0;
    run(&s, 7000 + 800000);
    assert(hyp.barked && hyp.bark_at == last + MS(10000));
    assert(hyp.bitten && hyp.bite_at == last + MS(20000) && !hyp.enabled);
}

// A late pet thread is reported with the hypervisor's view, measured from its previous round; the observer's pets keep the
// watchdog quiet meanwhile. Late while the observer stalled as well (past the bark): the pets are withheld until the observer is
// back half a second later, and nothing bites.
static void test_late(void)
{
    boot();
    set_args("pagani-wdog", 20, NULL, 0, NULL, 0);
    gunyah_wdog_start();
    struct sim s;
    sim_start(&s);
    run(&s, 30000);
    s.thread_at += MS(6000);
    run(&s, 60000);
    assert(logged('U', "wdog late: the pet thread ran 7000 ms after its previous round; STATUS(0x86000006)=0 enabled 1 "
                       "expired 0 since_pet 500 ms"));
    assert(!wdog_withheld_count);
    s.thread_at += MS(12000);
    s.observer_at += MS(13000);
    run(&s, 90000);
    assert(logged('U', "wdog late: the pet thread ran 13000 ms after its previous round; STATUS(0x86000006)=0 enabled 1 "
                       "expired 1 since_pet 13000 ms"));
    assert(logged('T', "wdog: pets withheld: cpu 0 heartbeat 0 ms ago (beats 66), observer 13500 ms ago (passes 53); last pet "
                       "13000 ms ago, bite at 20000"));
    assert(logged('T', "wdog: pets resume after 500 ms withheld: cpu 0 heartbeat 500 ms ago (beats 66), observer 0 ms ago "
                       "(passes 54)"));
    assert(!hyp.bitten && !hyp.barked && hyp.enabled && wdog_withheld_count == 1);
}

// G5: a pet needs the boot CPU's heartbeat and the observer within the last 3 s.
static void test_alive(void)
{
    struct sim s;
    unsigned from;
    // The boot CPU wedged (interrupts off, or stuck on a bus access) after its beat at 60 s, the pet thread and the observer
    // running on other CPUs: pets while that beat is at most 3 s old (the thread's at 63 s is the last), then none. The bite
    // comes 3 s + the bite time after the last beat; one record says why; the pet thread keeps running.
    boot();
    set_args("pagani-wdog", 20, NULL, 0, NULL, 0);
    gunyah_wdog_start();
    sim_start(&s);
    run(&s, 60000);
    assert(wdog_beat_at == MS(60000) && !wdog_withheld_count);
    s.beat = 0;
    run(&s, 63000);
    from = ncalls;
    run(&s, 120000);
    assert(count_calls(GUNYAH_WDOG_FN_PET, from) == 0 && hyp.barked && hyp.bark_at == MS(73000));
    assert(hyp.bitten && hyp.bite_at == MS(60000 + 3000 + 20000) && !hyp.enabled && !s.thread_done);
    assert(logged_count('T', "wdog: pets withheld: ") == 1 && !logged('T', "resume") && wdog_withheld_count == 1);
    assert(logged('T', "wdog: pets withheld: cpu 0 heartbeat 3500 ms ago (beats 53), observer 0 ms ago (passes 57); last pet "
                       "500 ms ago, bite at 20000"));

    // The observer stuck (its thread call, or its timer on a wedged CPU) after its pass at 59.5 s, the boot CPU and the pet
    // thread alive: the thread's pet at 62 s is the last (that pass 2.5 s old), the bite follows it.
    boot();
    set_args("pagani-wdog", 20, NULL, 0, NULL, 0);
    gunyah_wdog_start();
    sim_start(&s);
    run(&s, 60000);
    s.observer = 0;
    run(&s, 62000);
    from = ncalls;
    run(&s, 120000);
    assert(count_calls(GUNYAH_WDOG_FN_PET, from) == 0 && hyp.bitten && hyp.bite_at == MS(62000 + 20000));
    assert(logged('T', "wdog: pets withheld: cpu 0 heartbeat 0 ms ago (beats 56), observer 3500 ms ago (passes 53); last pet "
                       "1000 ms ago, bite at 20000"));

    // A stall shorter than the bite: no interrupt on the boot CPU from 60 s to 70 s. Withheld from 63.5 s, the pets resume with
    // the beat at 70 s; nothing barks or bites, and the summaries count the stretch and the 10 s gap.
    boot();
    set_args("pagani-wdog", 20, NULL, 0, NULL, 0);
    gunyah_wdog_start();
    sim_start(&s);
    run(&s, 60000);
    s.beat = 0;
    run(&s, 69999);
    s.beat = 1;
    s.beat_at = MS(70000);
    run(&s, 130000);
    assert(!hyp.bitten && !hyp.barked && hyp.enabled && wdog_withheld_count == 1);
    assert(logged('T', "wdog: pets withheld: cpu 0 heartbeat 3500 ms ago (beats 53), observer 0 ms ago (passes 57)"));
    assert(logged('T', "wdog: pets resume after 6500 ms withheld: cpu 0 heartbeat 0 ms ago (beats 54), observer 500 ms ago "
                       "(passes 63)"));
    assert(logged('U', "wdog ok: armed 60 s, pets thread 56 observer 56 failed 0, longest gap 500 ms (bark 10000), cpu 0 beats 53 "
                       "(gap <= 1000 ms), withheld 1"));
    assert(logged('U', "), cpu 0 beats 111 (gap <= 10000 ms), withheld 1"));

    // An idle boot CPU is no hang: XNU is tickless, but the heartbeat's own timer wakes that CPU every period (nothing else runs
    // on it here). Wakes up to 2.9 s apart are fine too; a 3.6 s gap withholds the pets for the half second it is over 3 s.
    boot();
    set_args("pagani-wdog", 20, NULL, 0, NULL, 0);
    gunyah_wdog_start();
    sim_start(&s);
    run(&s, 7000 + 600000);
    assert(wdog_beats == 600 && wdog_ms(wdog_beat_max_gap) == 1000 && !wdog_withheld_count && !hyp.barked && !hyp.bitten);
    uint64_t at = 7000 + 600000;
    for (int i = 0; i < 20; i++) {
        s.beat_at += MS(1900);
        at += 5000;
        run(&s, at);
    }
    assert(!wdog_withheld_count && wdog_ms(wdog_beat_max_gap) == 2900 && !hyp.barked && !hyp.bitten);
    s.beat_at += MS(2600);
    at += 10000;
    run(&s, at);
    assert(wdog_ms(wdog_beat_max_gap) == 3600 && wdog_withheld_count == 1 && !hyp.barked && !hyp.bitten);
    assert(logged_count('T', "wdog: pets withheld: ") == 1 && logged('T', "wdog: pets resume after 500 ms withheld"));

    // A kernel without an observer (its thread call was never allocated): judged by the heartbeat alone, the pet thread keeps
    // the watchdog quiet; a wedged boot CPU still stops the pets.
    boot();
    set_args("pagani-wdog", 20, NULL, 0, NULL, 0);
    gunyah_wdog_start();
    sim_start(&s);
    s.observer = 0;
    run(&s, 120000);
    assert(!hyp.bitten && !hyp.barked && !wdog_withheld_count && !wdog_pets[1] && wdog_pets[0] == 113);
    s.beat = 0;
    run(&s, 180000);
    assert(hyp.bitten && hyp.bite_at == MS(123000 + 20000));
    assert(logged('T', "wdog: pets withheld: cpu 0 heartbeat 4000 ms ago (beats 113), observer not run since arming; last pet "
                       "1000 ms ago, bite at 20000"));

    // The heartbeat runs on the CPU the arming thread is bound to, named as it is.
    boot();
    beat_cpu_value = 4;
    set_args("pagani-wdog", 20, NULL, 0, NULL, 0);
    gunyah_wdog_start();
    assert(hyp.enabled && logged('T', "wdog heartbeat: a local timer on cpu 4 every 1000 ms"));
}

// Panic: no pet from the panic path; disabled when RamoopsLog resets after the panic, armed (the bite resets) otherwise.
static void test_panic(void)
{
    for (int reset_follows = 0; reset_follows < 2; reset_follows++) {
        boot();
        set_args("pagani-wdog", 20, NULL, 0, NULL, 0);
        gunyah_wdog_start();
        struct sim s;
        sim_start(&s);
        run(&s, 60000);
        uint64_t last = hyp.last_pat;
        now_ticks += MS(400);
        panic_footer_text = reset_follows ? "RESET IN 20 s; log kept in ramoops" : NULL;
        unsigned before = ncalls;
        gunyah_wdog_panic();
        gunyah_wdog_panic();                    // nested panics and the exception path call it again
        if (reset_follows) {
            assert(ncalls == before + 1);
            expect_call(before, 0x86000005, 2, 0);
            assert(!hyp.enabled && !gunyah_wdog_panic_footer());
            assert(logged('T', "wdog panic: pets stopped; RamoopsLog resets, so CONTROL(0x86000005, 2)=0 disabled the "
                               "watchdog"));
        } else {
            assert(ncalls == before && hyp.enabled);
            assert(logged('T', "wdog panic: pets stopped, no software reset follows: the bite resets the phone in <= "
                               "19600 ms (last pet 400 ms ago)"));
            assert(!strcmp(gunyah_wdog_panic_footer(), "WATCHDOG RESET IN <= 20 s (pagani-wdog)"));
        }
        // Other CPUs keep scheduling: the pet thread ends, the observer does nothing.
        run(&s, 120000);
        assert(s.thread_done && s.beat_done && count_calls(GUNYAH_WDOG_FN_PET, before) == 0);
        assert(count_calls(GUNYAH_WDOG_FN_CONTROL, before) == (unsigned)reset_follows);
        if (reset_follows) {
            assert(!hyp.bitten);
            gunyah_wdog_before_reset("panic");  // RamoopsLog's reset after the wait: already disabled, nothing more
            assert(ncalls == before + 1);
        } else assert(hyp.bitten && hyp.bite_at == last + MS(20000));
    }
}

// A flush to the UFS log ahead: the watchdog stays armed through it, so a CPU stuck in it ends in the bite; once it is over no bite races the
// reset. Without a reset to come, or without a flush, nothing changes.
static void test_panic_flush(void)
{
    boot();
    set_args("pagani-wdog", 20, NULL, 0, NULL, 0);
    gunyah_wdog_start();
    struct sim s;
    sim_start(&s);
    run(&s, 60000);
    assert(gunyah_wdog_armed());
    uint64_t last = hyp.last_pat;
    panic_footer_text = "RESET IN 20 s; log kept in ramoops";
    flush_pending = 1;
    unsigned before = ncalls;
    gunyah_wdog_panic();
    gunyah_wdog_panic();
    assert(ncalls == before && hyp.enabled && gunyah_wdog_armed());
    assert(!logged('T', "wdog panic: pets stopped; RamoopsLog resets"));
    flush_pending = 0;
    now_ticks += MS(200);
    gunyah_wdog_panic_flushed();
    expect_call(before, 0x86000005, 2, 0);
    assert(ncalls == before + 1 && !hyp.enabled && !gunyah_wdog_armed());
    gunyah_wdog_panic_flushed();                // nested panics come here again
    assert(ncalls == before + 1);
    run(&s, 120000);
    assert(!hyp.bitten);
    // The flush never comes back: the pets are off, and the bite resets the phone at the last pet + its time.
    boot();
    set_args("pagani-wdog", 20, NULL, 0, NULL, 0);
    gunyah_wdog_start();
    sim_start(&s);
    run(&s, 60000);
    last = hyp.last_pat;
    panic_footer_text = "RESET IN 20 s; log kept in ramoops";
    flush_pending = 1;
    before = ncalls;
    gunyah_wdog_panic();
    run(&s, 120000);
    assert(hyp.bitten && hyp.bite_at == last + MS(20000) && count_calls(GUNYAH_WDOG_FN_PET, before) == 0);
    // No software reset to come: flushed or not, armed, and the panic's line was there already.
    boot();
    set_args("pagani-wdog", 20, NULL, 0, NULL, 0);
    gunyah_wdog_start();
    sim_start(&s);
    run(&s, 60000);
    flush_pending = 1;
    before = ncalls;
    gunyah_wdog_panic();
    assert(logged('T', "wdog panic: pets stopped, no software reset follows"));
    gunyah_wdog_panic_flushed();
    assert(ncalls == before && hyp.enabled && gunyah_wdog_armed());
    assert(logged_count('T', "wdog panic: pets stopped, no software reset follows") == 1);
    // Nothing waiting for it: RamoopsLog's call changes nothing, whatever a reset may follow.
    boot();
    set_args("pagani-wdog", 20, NULL, 0, NULL, 0);
    gunyah_wdog_start();
    sim_start(&s);
    run(&s, 60000);
    panic_footer_text = "RESET IN 20 s; log kept in ramoops";
    before = ncalls;
    gunyah_wdog_panic_flushed();
    assert(ncalls == before && hyp.enabled && gunyah_wdog_armed());
    // Pets can be stopped without anything else: what a reset does before its flush.
    boot();
    set_args("pagani-wdog", 20, NULL, 0, NULL, 0);
    gunyah_wdog_start();
    sim_start(&s);
    run(&s, 60000);
    last = hyp.last_pat;
    before = ncalls;
    gunyah_wdog_stop_pets();
    assert(hyp.enabled && gunyah_wdog_armed());
    run(&s, 120000);
    assert(hyp.bitten && hyp.bite_at == last + MS(20000) && count_calls(GUNYAH_WDOG_FN_PET, before) == 0);
}

// An intentional reset (RamoopsLog reset_now): disabled once, never petted again.
static void test_before_reset(void)
{
    boot();
    set_args("pagani-wdog", 30, NULL, 0, NULL, 0);
    gunyah_wdog_start();
    struct sim s;
    sim_start(&s);
    run(&s, 100000);
    unsigned before = ncalls;
    gunyah_wdog_before_reset("timer");
    gunyah_wdog_before_reset("observer");
    assert(ncalls == before + 1);
    expect_call(before, 0x86000005, 2, 0);
    assert(!hyp.enabled && logged('T', "wdog: timer reset: pets stopped, CONTROL(0x86000005, 2)=0 disabled the watchdog"));
    run(&s, 200000);
    assert(ncalls == before + 1 && s.thread_done && s.beat_done && !hyp.bitten);
    gunyah_wdog_panic();                        // a panic on the way to the reset changes nothing
    assert(ncalls == before + 1 && !gunyah_wdog_panic_footer());
    // The reset returned (RamoopsLog's retry): the shortest bite, no pets, the bite comes; only once.
    gunyah_wdog_after_failed_reset();
    gunyah_wdog_after_failed_reset();
    assert(ncalls == before + 3);
    expect_call(before + 1, 0x86000008, 2000, 4000);
    expect_call(before + 2, 0x86000005, 3, 0);
    assert(hyp.enabled && logged('T', "wdog: the PSCI reset returned: SET_TIME(0x86000008, bark 2000, bite 4000)=0 "
                                      "CONTROL(0x86000005, 3)=0, no pets: the bite resets the phone"));
    run(&s, 210000);
    assert(hyp.bitten && ncalls == before + 3);
}

// pagani-reset-wdog: the timed reset by the bite. Armed: the shortest bite, pets stopped, the bite comes; once. Not ours: 0.
static void test_bite_reset(void)
{
    boot();
    set_args("pagani-wdog", 30, NULL, 0, NULL, 0);
    gunyah_wdog_start();
    struct sim s;
    sim_start(&s);
    run(&s, 100000);
    unsigned before = ncalls;
    assert(gunyah_wdog_bite_reset("timer") == 1);
    assert(gunyah_wdog_bite_reset("observer") == 1 && ncalls == before + 1);
    expect_call(before, 0x86000008, 2000, 4000);
    assert(hyp.enabled && logged('T', "wdog: timer reset by the bite: pets stopped, SET_TIME(0x86000008, bark 2000, bite 4000)=0"));
    run(&s, 110000);
    assert(hyp.bitten && ncalls == before + 1);
    boot();
    set_args("pagani-wdog", 0, NULL, 0, NULL, 0);
    gunyah_wdog_start();
    assert(gunyah_wdog_bite_reset("timer") == 0 && !ncalls && !hyp.enabled);
}

// pagani-reset-seq (RamoopsLog's reset with steps): PET, then the shortest bite, pets stopped, left armed; a second resetter and the
// plain reset's call change nothing; the steps' CPU disables it before PSCI (once); a returned PSCI re-arms as without the steps. A
// step that never returns: the bite 4 s after the hold's pet, even when the last pet is long past. Not ours, or disabled by a panic
// that RamoopsLog resets after: nothing.
static void test_hold_for_steps(void)
{
    boot();
    set_args("pagani-wdog", 30, NULL, 0, NULL, 0);
    gunyah_wdog_start();
    struct sim s;
    sim_start(&s);
    run(&s, 100000);
    unsigned before = ncalls;
    assert(gunyah_wdog_hold_for_steps("timer") == 1);
    expect_call(before, 0x86000007, 0, 0);                  // the pet first: the bite counts from now
    expect_call(before + 1, 0x86000008, 2000, 4000);
    assert(ncalls == before + 2 && hyp.enabled && hyp.last_pat == now_ticks && hyp.bark == 2000 && hyp.bite == 4000);
    assert(logged('T', "wdog: timer reset: pets stopped, PET(0x86000007)=0 SET_TIME(0x86000008, bark 2000, bite 4000)=0: armed "
                       "through the reset steps (pagani-reset-seq)"));
    run(&s, 101000);                                         // a second of steps: the pet thread, heartbeat and observer pet no more
    assert(count_calls(GUNYAH_WDOG_FN_PET, before + 1) == 0 && s.thread_done && s.beat_done && !hyp.bitten);
    assert(gunyah_wdog_hold_for_steps("backup timer") == 2);
    gunyah_wdog_before_reset("backup timer");
    assert(ncalls == before + 2 && hyp.enabled);
    gunyah_wdog_release_hold("timer");
    gunyah_wdog_release_hold("timer");
    assert(ncalls == before + 3);
    expect_call(before + 2, 0x86000005, 2, 0);
    assert(!hyp.enabled && logged('T', "wdog: timer reset steps done: CONTROL(0x86000005, 2)=0 disabled the watchdog before PSCI"));
    run(&s, 200000);
    assert(!hyp.bitten && ncalls == before + 3);
    gunyah_wdog_after_failed_reset();                        // the PSCI reset returned: the shortest bite, no pets
    expect_call(before + 3, 0x86000008, 2000, 4000);
    expect_call(before + 4, 0x86000005, 3, 0);
    run(&s, 210000);
    assert(hyp.bitten && ncalls == before + 5);

    // A step that never returns (no release). The observer is the resetting thread: it stopped 20 s before, so the pets were
    // withheld and the last one is 17 s old; the hold's pet makes the bite come 4 s after the hold, not at once.
    boot();
    set_args("pagani-wdog", 30, NULL, 0, NULL, 0);
    gunyah_wdog_start();
    sim_start(&s);
    run(&s, 100000);
    s.observer = 0;
    run(&s, 120000);
    assert(!hyp.bitten && now_ticks - hyp.last_pat > MS(15000) && logged('T', "wdog: pets withheld"));
    before = ncalls;
    assert(gunyah_wdog_hold_for_steps("observer") == 1);
    run(&s, 130000);
    assert(hyp.bitten && hyp.bite_at == MS(124000) && count_calls(GUNYAH_WDOG_FN_PET, before + 1) == 0);

    // Not ours (pagani-wdog absent): nothing.
    boot();
    set_args("pagani-wdog", 0, NULL, 0, NULL, 0);
    gunyah_wdog_start();
    assert(gunyah_wdog_hold_for_steps("timer") == 0);
    gunyah_wdog_release_hold("timer");
    assert(!ncalls && !nlines && !hyp.enabled);
    // Disabled by a panic that RamoopsLog resets after: nothing to hold or release.
    boot();
    set_args("pagani-wdog", 20, NULL, 0, NULL, 0);
    gunyah_wdog_start();
    sim_start(&s);
    run(&s, 30000);
    panic_footer_text = "RESET IN 20 s; log kept in ramoops";
    gunyah_wdog_panic();
    before = ncalls;
    assert(!hyp.enabled && gunyah_wdog_hold_for_steps("panic") == 0);
    gunyah_wdog_release_hold("panic");
    assert(ncalls == before && !hyp.enabled);
    // The pagani-reset-wdog bite settled it first: a hold changes nothing.
    boot();
    set_args("pagani-wdog", 30, NULL, 0, NULL, 0);
    gunyah_wdog_start();
    assert(gunyah_wdog_bite_reset("timer") == 1);
    before = ncalls;
    assert(gunyah_wdog_hold_for_steps("backup timer") == 2 && ncalls == before && hyp.enabled);
}

// Without our watchdog (pagani-wdog absent) a returned reset arms nothing.
static void test_failed_reset_without_wdog(void)
{
    boot();
    set_args("pagani-wdog", 0, NULL, 0, NULL, 0);
    gunyah_wdog_start();
    gunyah_wdog_before_reset("timer");
    gunyah_wdog_after_failed_reset();
    assert(!ncalls && !hyp.enabled && !nlines);
}

static void test_clamp_and_hvc(void)
{
    boot();
    set_args("pagani-wdog", 100, NULL, 0, NULL, 0);
    gunyah_wdog_start();
    expect_call(1, 0x86000008, 16000, 32000);
    assert(logged('T', "wdog config: pagani-wdog=100 (clamped to 32 s) bite 32000 ms bark 16000 ms"));
    boot();
    set_args("pagani-wdog", 1, NULL, 0, NULL, 0);
    gunyah_wdog_start();
    expect_call(1, 0x86000008, 2000, 4000);
    assert(logged('T', "wdog config: pagani-wdog=1 (clamped to 4 s) bite 4000 ms bark 2000 ms"));
    // pagani-wdog-hvc=1: the same calls through HVC.
    boot();
    set_args("pagani-wdog", 20, "pagani-wdog-hvc", 1, NULL, 0);
    gunyah_wdog_start();
    struct sim s;
    sim_start(&s);
    run(&s, 20000);
    assert(ncalls > 5 && hyp.enabled);
    for (unsigned i = 0; i < ncalls; i++) assert(calls[i].hvc == 1);
    assert(!strcmp(gunyah_wdog_status(), "wdog on: bite 20 s, bark at half, Gunyah hvc"));
}

static void test_failures(void)
{
    // Nothing answers (both encodings of -1): only the probe, no thread, nothing armed, nothing petted.
    for (int all_ones = 0; all_ones < 2; all_ones++) {
        boot();
        hyp.absent = 1;
        hyp.all_ones = all_ones;
        set_args("pagani-wdog", 20, NULL, 0, NULL, 0);
        gunyah_wdog_start();
        struct sim s;
        sim_start(&s);
        run(&s, 30000);
        gunyah_wdog_panic();
        gunyah_wdog_before_reset("timer");
        assert(ncalls == 1 && calls[0].function == 0x86000006 && !threads_started && !beats_started);
        assert(logged('T', "wdog probe: STATUS(0x86000006)=-1; off: no Gunyah watchdog answers smc"));
        assert(!strcmp(gunyah_wdog_status(), "wdog off: STATUS -1"));
    }
    // Enabled before us (firmware): disabled first, then armed as usual.
    boot();
    hyp.enabled = 1;
    hyp.last_pat = now_ticks - MS(300);
    set_args("pagani-wdog", 20, NULL, 0, NULL, 0);
    gunyah_wdog_start();
    assert(ncalls == 6);
    expect_call(0, 0x86000006, 0, 0);
    expect_call(1, 0x86000005, 2, 0);
    expect_call(2, 0x86000008, 10000, 20000);
    expect_call(3, 0x86000005, 3, 0);
    expect_call(4, 0x86000007, 0, 0);
    assert(logged('T', "wdog: found enabled before arming; CONTROL(0x86000005, 2)=0"));
    // SET_TIME refused: not enabled, the pet thread ends.
    boot();
    hyp.fail_set_time = 1;
    set_args("pagani-wdog", 20, NULL, 0, NULL, 0);
    gunyah_wdog_start();
    assert(ncalls == 2 && !hyp.enabled && !gunyah_wdog_host_thread_step() && beats_started == 1 && !gunyah_wdog_host_beat());
    assert(logged('T', "wdog off: SET_TIME(0x86000008, bark 10000, bite 20000)=1; not enabled"));
    assert(!strcmp(gunyah_wdog_status(), "wdog off: SET_TIME 1"));
    // CONTROL(enable) refused: no pets.
    boot();
    hyp.fail_enable = -2;
    set_args("pagani-wdog", 20, NULL, 0, NULL, 0);
    gunyah_wdog_start();
    gunyah_wdog_tick();
    assert(ncalls == 3 && !gunyah_wdog_host_thread_step() && !gunyah_wdog_host_beat() &&
           logged('T', "wdog off: CONTROL(0x86000005, 3)=-2"));
    // No pet thread: the watchdog is never enabled.
    boot();
    thread_ok = 0;
    set_args("pagani-wdog", 20, NULL, 0, NULL, 0);
    gunyah_wdog_start();
    assert(ncalls == 1 && !hyp.enabled && !beats_started && !strcmp(gunyah_wdog_status(), "wdog off: no pet thread"));
    // No heartbeat on the boot CPU: never enabled either (no pet could prove that CPU alive); the pet thread ends.
    boot();
    beat_cpu_value = -1;
    set_args("pagani-wdog", 20, NULL, 0, NULL, 0);
    gunyah_wdog_start();
    assert(ncalls == 1 && threads_started == 1 && beats_started == 1 && !hyp.enabled && !gunyah_wdog_host_thread_step());
    assert(logged('T', "wdog off: the boot CPU heartbeat did not start; the watchdog is not enabled"));
    assert(!strcmp(gunyah_wdog_status(), "wdog off: no CPU heartbeat"));
    // While arming, the pet thread waits and the heartbeat counts.
    boot();
    set_args("pagani-wdog", 20, NULL, 0, NULL, 0);
    wdog_state = WDOG_ARMING;
    const uint32_t beats = wdog_beats;
    assert(gunyah_wdog_host_thread_step() == GUNYAH_WDOG_PET_MS && !ncalls);
    assert(gunyah_wdog_host_beat() && wdog_beats == beats + 1 && !ncalls);
}

// pagani-wdog-stop-pet: healthy pets, then none, the hypervisor's view each second, the bark and the bite.
static void test_stop_pet(void)
{
    boot();
    set_args("pagani-wdog", 20, "pagani-wdog-stop-pet", 30, NULL, 0);
    gunyah_wdog_start();
    assert(logged('T', "conduit smc; TEST: pets stop 30 s after arming"));
    struct sim s;
    sim_start(&s);
    run(&s, 7000 + 29999);
    assert(!hyp.bitten && wdog_pets[0] == 29);
    unsigned pets = count_calls(GUNYAH_WDOG_FN_PET, 0);
    run(&s, 7000 + 120000);
    assert(count_calls(GUNYAH_WDOG_FN_PET, 0) == pets);
    assert(logged('T', "wdog test: pets stopped 30 s after arming (pagani-wdog-stop-pet); last pet 500 ms ago; bark due at "
                       "10000 ms, bite at 20000 ms after it"));
    // The observer's pet 500 ms before the stop was the last one: the bark 10 s and the bite 20 s after it.
    assert(logged('T', "wdog test: STATUS(0x86000006)=0 enabled 1 expired 0 since_pet 500 ms (bark 10000, bite 20000)"));
    assert(logged('T', "wdog test: STATUS(0x86000006)=0 enabled 1 expired 0 since_pet 9500 ms"));
    assert(logged('T', "wdog test: STATUS(0x86000006)=0 enabled 1 expired 1 since_pet 10500 ms"));
    assert(hyp.bitten && hyp.bite_at == MS(7000 + 29500 + 20000));
    assert(logged('T', "wdog test: STATUS(0x86000006)=0 enabled 1 expired 1 since_pet 19500 ms"));
    assert(logged('T', "wdog test: STATUS(0x86000006)=0 enabled 0 expired 0 since_pet 20500 ms"));
    // The STATUS lines end after a bounded number (the thread stops when the bite did not reset the phone).
    run(&s, 7000 + 400000);
    assert(s.thread_done && s.beat_done && logged_count('T', "wdog test: STATUS") == WDOG_TEST_LINES);
}

int main(void)
{
    test_off();
    test_arm_and_cadence();
    test_late();
    test_alive();
    test_panic();
    test_panic_flush();
    test_before_reset();
    test_hold_for_steps();
    test_failed_reset_without_wdog();
    test_bite_reset();
    test_clamp_and_hvc();
    test_failures();
    test_stop_pet();
    printf("PASS: gunyah watchdog: SMCCC 0x86000005..8 over smc (hvc on request), SET_TIME bark/bite in ms (bark = bite/2, "
           "4..32 s), probe/disable/SET_TIME/enable/PET/read-back order, pets only from the pet thread and the observer "
           "(1 s cadence), a hang bites at last pet + bite, disabled before a reset and before a panic reset, armed after a "
           "panic without reset, stop-pet test, off when pagani-wdog=0 or absent; boot CPU heartbeat: no pet unless it and "
           "the observer ran within 3 s (wedged boot CPU or stuck observer: the bite on time), pets resume after a short "
           "stall, an idle boot CPU and late wakes under 3 s are no hang, no observer: the heartbeat alone; pagani-reset-seq: "
           "held armed through the reset steps (PET, then the shortest bite; a hang bites 4 s after the hold), released before "
           "PSCI once, nothing when not ours or after a panic or the bite path\n");
    return 0;
}
