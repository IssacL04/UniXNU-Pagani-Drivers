// SPDX-License-Identifier: BSD-2-Clause-Patent
// The Gunyah primary-VM watchdog for unattended tests (GunyahWdog.h).
// ABI sources: the phone's own vendor_boot /lib/modules/gh_virt_wdt.ko (6.6.89-android15: smc #0 with 0x86000005..8,
// SET_TIME(bark, 0xffff) and SET_TIME(0xffff, bite), CONTROL 3/2, STATUS bit 0 / bit 31 / w2) and qcom_wdt_core.ko
// (bark 20000 ms, bite 23000 ms, pet every 9360 ms); Linux drivers/watchdog/gunyah_wdt.c (SMC32, 16-bit ms, values from
// 0x8000 reserved); quic/gunyah-hypervisor hyp/misc/watchdog/src/watchdog.c (bark, then bite, both counted from the last
// pat; a bite time not above the bark time is queued directly; set_times clamps reserved values; the bite of a critical
// watchdog aborts the hypervisor, which then makes the hardware watchdog bite, and that bite belongs to TZ).
// Every static name starts with wdog_: the BootProgress console test compiles this file into one unit with
// BootProgress.c and RamoopsLog.c.
#include <stdint.h>
#include <stddef.h>
#include "GunyahWdog.h"
#include "RamoopsLog.h"
#include "UfsLog.h"

#if defined(BOOT_PROGRESS_TEST) || defined(GUNYAH_WDOG_TEST)
#define GUNYAH_WDOG_HOST 1
// The host tests supply the platform: boot-args, the counter, the hypervisor call, the pet thread (which the tests
// drive through wdog_thread_step) and the boot CPU's heartbeat timer (driven through wdog_beat_step; its CPU, -1 when it
// did not start).
int gunyah_wdog_platform_arg(const char *name, uint32_t *value);
uint64_t gunyah_wdog_platform_counter(void);
uint64_t gunyah_wdog_platform_frequency(void);
void gunyah_wdog_platform_call(int hvc, uint32_t function, uint32_t a1, uint32_t a2, uint64_t result[3]);
int gunyah_wdog_platform_start_thread(void);
int gunyah_wdog_platform_start_beat(void);
#else
#include <pexpert/pexpert.h>
#include <machine/machine_routines.h>
#include <kern/clock.h>
#include <kern/cpu_number.h>
#include <kern/timer_call.h>
// The scheduler headers drag in task, IPC and kalloc internals whose inline code warns under this file's -Weverything
// recipe (the kernel's own objects get the same warnings); none of it is ours.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunknown-warning-option"
#pragma clang diagnostic ignored "-Wcast-function-type-strict"
#pragma clang diagnostic ignored "-Wallocator-wrappers"
#pragma clang diagnostic ignored "-Wenum-enum-conversion"
#include <kern/sched.h>
#include <kern/sched_prim.h>
#include <kern/thread.h>
#pragma clang diagnostic pop

static int gunyah_wdog_platform_arg(const char *name, uint32_t *value)
{
    return PE_parse_boot_argn(name, value, (int)sizeof(*value)) ? 1 : 0;
}

static uint64_t gunyah_wdog_platform_counter(void)
{
    uint64_t value;
    __asm__ volatile("isb\n\tmrs %0, cntvct_el0" : "=r"(value) :: "memory");
    return value;
}

static uint64_t gunyah_wdog_platform_frequency(void)
{
    uint64_t value;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(value));
    return value;
}

// SMCCC 1.1 fast call, SMC32 convention: w1 and w2 in, w0..w2 out; x4..x17 are not relied upon.
static void gunyah_wdog_platform_call(int hvc, uint32_t function, uint32_t a1, uint32_t a2, uint64_t result[3])
{
    register uint64_t x0 __asm__("x0") = function;
    register uint64_t x1 __asm__("x1") = a1;
    register uint64_t x2 __asm__("x2") = a2;
    register uint64_t x3 __asm__("x3") = 0;
    if (hvc) {
        __asm__ volatile("hvc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) :
                         : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
    } else {
        __asm__ volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) :
                         : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
    }
    result[0] = x0;
    result[1] = x1;
    result[2] = x2;
}

static uint32_t wdog_thread_step(void);
static int wdog_sleep_event;                // the pet thread sleeps on it with a timeout; nothing posts it

// The pet thread: one pet per second while armed. It needs the scheduler and the CPU timer interrupt to wake up, so it
// stops petting exactly when the kernel stops scheduling.
static void wdog_pet_thread(void *parameter, wait_result_t result)
{
    (void)parameter; (void)result;
    for (;;) {
        uint32_t ms = wdog_thread_step();
        if (!ms) break;
        assert_wait_timeout((event_t)&wdog_sleep_event, THREAD_UNINT, ms, NSEC_PER_MSEC);
        thread_block(THREAD_CONTINUE_NULL);
    }
    thread_terminate(current_thread());
}

static int gunyah_wdog_platform_start_thread(void)
{
    thread_t thread = THREAD_NULL;
    if (kernel_thread_start_priority(wdog_pet_thread, NULL, MAXPRI_KERNEL, &thread) != KERN_SUCCESS) return 0;
    thread_set_thread_name(thread, "pagani-wdog");
    thread_deallocate(thread);
    return 1;
}

static int wdog_beat_step(void);
static timer_call_data_t wdog_beat_timer;

// TIMER_CALL_LOCAL: on the entering CPU's own queue, never migrated (xnu timer_call.c skips the longterm queue for it);
// SYS_CRITICAL: no coalescing leeway. A due local timer wakes its CPU from idle like any other.
static void wdog_beat_enter(void)
{
    uint64_t deadline = 0;
    clock_interval_to_deadline(GUNYAH_WDOG_BEAT_MS, NSEC_PER_MSEC, &deadline);
    timer_call_enter(&wdog_beat_timer, deadline, TIMER_CALL_SYS_CRITICAL | TIMER_CALL_LOCAL);
}

// The heartbeat (interrupt context, on the CPU that started it): one beat, then entered again on the same CPU until the pets end.
static void wdog_beat_fired(timer_call_param_t param0, timer_call_param_t param1)
{
    (void)param0; (void)param1;
    if (wdog_beat_step()) wdog_beat_enter();
}

// The arming thread is bound to the boot CPU (startup.c: thread_bind before boot_progress_start), so the heartbeat lands on the CPU
// of the timed reset's timer; interrupts stay off between reading the CPU number and entering the timer.
static int gunyah_wdog_platform_start_beat(void)
{
    timer_call_setup(&wdog_beat_timer, wdog_beat_fired, NULL);
    const boolean_t enabled = ml_set_interrupts_enabled(FALSE);
    const int cpu = cpu_number();
    wdog_beat_enter();
    (void)ml_set_interrupts_enabled(enabled);
    return cpu;
}
#endif

enum {
    WDOG_OFF,                   // pagani-wdog absent or 0
    WDOG_ARMING,                // probe done, enabling (the pet thread waits)
    WDOG_ARMED,                 // enabled by us; pets go out until a panic, a reset or the stop-pet test
    WDOG_STOPPED,               // disabled by us before a reset
    WDOG_FAILED,                // the probe or the arming failed; the watchdog is not enabled by us
};
enum { WDOG_SOURCE_THREAD, WDOG_SOURCE_OBSERVER };
enum {
    WDOG_TEXT = 200,            // one ramoops record (RAMOOPS_LOG_TEXT_MAX)
    WDOG_LATE_PERIODS = 3,      // a thread pet this many periods after the previous one is reported
    WDOG_LATE_REPORT_SECONDS = 10,
    WDOG_SUMMARY_SECONDS = 60,
    WDOG_TEST_LINES = 2 * GUNYAH_WDOG_MAX_SECONDS + 8,  // STATUS lines after the stop-pet test, then the thread ends
};

struct wdog_result {
    int32_t error;              // Gunyah's error code (w0)
    uint32_t a1, a2;
};

struct wdog_text {
    char *at;
    uint32_t used, room;
};

static int wdog_started, wdog_hvc, wdog_state;
static int wdog_pets_stopped, wdog_test_stopped, wdog_panicked, wdog_panic_waits, wdog_resetting;
static uint32_t wdog_requested, wdog_bite_ms, wdog_bark_ms, wdog_stop_pet_seconds, wdog_test_lines;
static uint32_t wdog_pets[2], wdog_pet_failures;
static uint64_t wdog_hz, wdog_armed_at, wdog_last_pet, wdog_max_gap;
static uint64_t wdog_last_thread_round, wdog_last_late_report, wdog_last_summary;
// The liveness behind a pet (G5): the boot CPU's heartbeat (its CPU, beats, the last one and the longest gap between two) and the
// observer's passes since the arming (the last one; 0: none yet, so a kernel without an observer is judged by the heartbeat
// alone); a withheld stretch (whether one is on, since when, how many began).
static int wdog_beat_cpu, wdog_withheld;
static uint32_t wdog_beats, wdog_observer_passes, wdog_withheld_count;
static uint64_t wdog_beat_at, wdog_beat_max_gap, wdog_observer_at, wdog_withheld_at;
// One text buffer per context that may write at the same time: arming, the pet thread, the observer, the panic CPU and
// the resetting CPU.
static char wdog_status_line[96], wdog_footer[64];
static char wdog_start_text[WDOG_TEXT], wdog_thread_text[WDOG_TEXT], wdog_tick_text[WDOG_TEXT];
static char wdog_panic_text[WDOG_TEXT], wdog_reset_text[WDOG_TEXT], wdog_retry_text[WDOG_TEXT], wdog_bite_text[WDOG_TEXT];
static char wdog_hold_text[WDOG_TEXT], wdog_release_text[WDOG_TEXT];   // pagani-reset-seq: the holding and the steps' CPU

static void wdog_text_start(struct wdog_text *t, char *buffer, uint32_t room)
{
    t->at = buffer;
    t->used = 0;
    t->room = room;
    buffer[0] = 0;
}

static void wdog_put_char(struct wdog_text *t, char c)
{
    if (t->used + 1 >= t->room) return;
    t->at[t->used++] = c;
    t->at[t->used] = 0;
}

static void wdog_put_str(struct wdog_text *t, const char *s)
{
    while (s && *s) wdog_put_char(t, *s++);
}

static void wdog_put_dec(struct wdog_text *t, uint64_t value)
{
    char digits[20];
    uint32_t n = 0;
    do { digits[n++] = (char)('0' + value % 10); value /= 10; } while (value);
    while (n) wdog_put_char(t, digits[--n]);
}

static void wdog_put_hex(struct wdog_text *t, uint64_t value)
{
    char digits[16];
    uint32_t n = 0;
    wdog_put_str(t, "0x");
    do { digits[n++] = "0123456789abcdef"[value & 15]; value >>= 4; } while (value);
    while (n) wdog_put_char(t, digits[--n]);
}

static void wdog_put_error(struct wdog_text *t, int32_t error)
{
    if (error < 0) { wdog_put_char(t, '-'); wdog_put_dec(t, (uint64_t)(-(int64_t)error)); }
    else wdog_put_dec(t, (uint64_t)error);
}

static uint64_t wdog_now(void) { return gunyah_wdog_platform_counter(); }

static uint64_t wdog_ms(uint64_t ticks) { return wdog_hz ? ticks * 1000 / wdog_hz : 0; }

static uint64_t wdog_ms_since(uint64_t from, uint64_t now) { return now > from ? wdog_ms(now - from) : 0; }

// w0 is Gunyah's 32-bit error code: -1 comes back as 0xffffffff or as all ones, depending on who answered.
static struct wdog_result wdog_call(uint32_t function, uint32_t a1, uint32_t a2)
{
    uint64_t out[3] = {0, 0, 0};
    gunyah_wdog_platform_call(wdog_hvc, function, a1, a2, out);
    struct wdog_result r;
    r.error = (int32_t)(uint32_t)out[0];
    r.a1 = (uint32_t)out[1];
    r.a2 = (uint32_t)out[2];
    return r;
}

static void wdog_put_status(struct wdog_text *t, struct wdog_result r)
{
    wdog_put_str(t, "STATUS(0x86000006)=");
    wdog_put_error(t, r.error);
    if (r.error) return;
    wdog_put_str(t, " enabled ");
    wdog_put_dec(t, r.a1 & GUNYAH_WDOG_STATUS_ENABLED);
    wdog_put_str(t, " expired ");
    wdog_put_dec(t, (r.a1 & GUNYAH_WDOG_STATUS_EXPIRED) ? 1 : 0);
    wdog_put_str(t, " since_pet ");
    wdog_put_dec(t, r.a2);
    wdog_put_str(t, " ms");
    if (r.a1 & ~(GUNYAH_WDOG_STATUS_ENABLED | GUNYAH_WDOG_STATUS_EXPIRED)) { wdog_put_str(t, " w1 "); wdog_put_hex(t, r.a1); }
}

// Arming failed: the log line in t, and the screen line. The watchdog is not enabled by us (the pet thread then ends).
static void wdog_fail(struct wdog_text *t, const char *screen, int with_error, int32_t error)
{
    __atomic_store_n(&wdog_state, WDOG_FAILED, __ATOMIC_RELEASE);
    ramoops_log_line(RAMOOPS_KIND_RESET, t->at);
    struct wdog_text s;
    wdog_text_start(&s, wdog_status_line, sizeof(wdog_status_line));
    wdog_put_str(&s, screen);
    if (with_error) wdog_put_error(&s, error);
}

static void wdog_note_gap(uint64_t gap)
{
    uint64_t seen = __atomic_load_n(&wdog_max_gap, __ATOMIC_RELAXED);
    while (gap > seen && !__atomic_compare_exchange_n(&wdog_max_gap, &seen, gap, 1, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
}

// Whether a pet may go out now; the stop-pet test ends the pets here (once, whichever pet path sees it first).
static int wdog_may_pet(uint64_t now, char *buffer)
{
    if (__atomic_load_n(&wdog_state, __ATOMIC_ACQUIRE) != WDOG_ARMED) return 0;
    if (__atomic_load_n(&wdog_pets_stopped, __ATOMIC_ACQUIRE)) return 0;
    if (wdog_stop_pet_seconds && now - wdog_armed_at >= (uint64_t)wdog_stop_pet_seconds * wdog_hz) {
        __atomic_store_n(&wdog_pets_stopped, 1, __ATOMIC_RELEASE);
        if (!__atomic_exchange_n(&wdog_test_stopped, 1, __ATOMIC_ACQ_REL)) {
            struct wdog_text t;
            wdog_text_start(&t, buffer, WDOG_TEXT);
            wdog_put_str(&t, "wdog test: pets stopped ");
            wdog_put_dec(&t, wdog_stop_pet_seconds);
            wdog_put_str(&t, " s after arming (pagani-wdog-stop-pet); last pet ");
            wdog_put_dec(&t, wdog_ms_since(__atomic_load_n(&wdog_last_pet, __ATOMIC_ACQUIRE), now));
            wdog_put_str(&t, " ms ago; bark due at ");
            wdog_put_dec(&t, wdog_bark_ms);
            wdog_put_str(&t, " ms, bite at ");
            wdog_put_dec(&t, wdog_bite_ms);
            wdog_put_str(&t, " ms after it");
            ramoops_log_line(RAMOOPS_KIND_RESET, t.at);
        }
        return 0;
    }
    return 1;
}

// One beat of the boot CPU's heartbeat (interrupt context on that CPU; it only counts). 0 ends the heartbeat: the pets ended
// (panic, reset, stop-pet test) or the watchdog is not armed by us.
static int wdog_beat_step(void)
{
    const int state = __atomic_load_n(&wdog_state, __ATOMIC_ACQUIRE);
    if ((state != WDOG_ARMING && state != WDOG_ARMED) || __atomic_load_n(&wdog_pets_stopped, __ATOMIC_ACQUIRE)) return 0;
    const uint64_t now = wdog_now();
    const uint64_t previous = __atomic_exchange_n(&wdog_beat_at, now, __ATOMIC_ACQ_REL);
    if (previous && now > previous && now - previous > __atomic_load_n(&wdog_beat_max_gap, __ATOMIC_RELAXED))
        __atomic_store_n(&wdog_beat_max_gap, now - previous, __ATOMIC_RELAXED);
    __atomic_add_fetch(&wdog_beats, 1, __ATOMIC_RELAXED);
    return 1;
}

// "cpu 0 heartbeat 3500 ms ago (beats 120), observer 400 ms ago (passes 118)"
static void wdog_put_alive(struct wdog_text *t, uint64_t beat_age, uint64_t seen, uint64_t seen_age)
{
    wdog_put_str(t, "cpu ");
    wdog_put_dec(t, (uint64_t)(uint32_t)wdog_beat_cpu);
    wdog_put_str(t, " heartbeat ");
    wdog_put_dec(t, wdog_ms(beat_age));
    wdog_put_str(t, " ms ago (beats ");
    wdog_put_dec(t, __atomic_load_n(&wdog_beats, __ATOMIC_RELAXED));
    if (!seen) { wdog_put_str(t, "), observer not run since arming"); return; }
    wdog_put_str(t, "), observer ");
    wdog_put_dec(t, wdog_ms(seen_age));
    wdog_put_str(t, " ms ago (passes ");
    wdog_put_dec(t, __atomic_load_n(&wdog_observer_passes, __ATOMIC_RELAXED));
    wdog_put_char(t, ')');
}

// G5: whether the boot CPU's heartbeat and the observer (once it has run since the arming) both ran at most GUNYAH_WDOG_ALIVE_MS
// ago. Called by both pet paths, each with its own text buffer; the start and the end of a withheld stretch are logged once.
static int wdog_alive(uint64_t now, char *buffer)
{
    const uint64_t window = (uint64_t)GUNYAH_WDOG_ALIVE_MS * wdog_hz / 1000;
    const uint64_t beat = __atomic_load_n(&wdog_beat_at, __ATOMIC_ACQUIRE);
    const uint64_t seen = __atomic_load_n(&wdog_observer_at, __ATOMIC_ACQUIRE);
    const uint64_t beat_age = now > beat ? now - beat : 0, seen_age = seen && now > seen ? now - seen : 0;
    const int alive = beat_age <= window && seen_age <= window;
    struct wdog_text t;
    if (alive) {
        if (!__atomic_load_n(&wdog_withheld, __ATOMIC_ACQUIRE) || !__atomic_exchange_n(&wdog_withheld, 0, __ATOMIC_ACQ_REL)) return 1;
        wdog_text_start(&t, buffer, WDOG_TEXT);
        wdog_put_str(&t, "wdog: pets resume after ");
        wdog_put_dec(&t, wdog_ms_since(__atomic_load_n(&wdog_withheld_at, __ATOMIC_ACQUIRE), now));
        wdog_put_str(&t, " ms withheld: ");
        wdog_put_alive(&t, beat_age, seen, seen_age);
        ramoops_log_line(RAMOOPS_KIND_RESET, t.at);
        return 1;
    }
    if (__atomic_exchange_n(&wdog_withheld, 1, __ATOMIC_ACQ_REL)) return 0;
    __atomic_store_n(&wdog_withheld_at, now, __ATOMIC_RELEASE);
    __atomic_add_fetch(&wdog_withheld_count, 1, __ATOMIC_RELAXED);
    wdog_text_start(&t, buffer, WDOG_TEXT);
    wdog_put_str(&t, "wdog: pets withheld: ");
    wdog_put_alive(&t, beat_age, seen, seen_age);
    wdog_put_str(&t, "; last pet ");
    wdog_put_dec(&t, wdog_ms_since(__atomic_load_n(&wdog_last_pet, __ATOMIC_ACQUIRE), now));
    wdog_put_str(&t, " ms ago, bite at ");
    wdog_put_dec(&t, wdog_bite_ms);
    ramoops_log_line(RAMOOPS_KIND_RESET, t.at);
    return 0;
}

static void wdog_pet(unsigned source, uint64_t now, char *buffer)
{
    struct wdog_result r = wdog_call(GUNYAH_WDOG_FN_PET, 0, 0);
    if (r.error) {
        uint32_t n = __atomic_add_fetch(&wdog_pet_failures, 1, __ATOMIC_RELAXED);
        if (n & (n - 1)) return;        // the 1st, 2nd, 4th, 8th ... failure is logged
        struct wdog_text t;
        wdog_text_start(&t, buffer, WDOG_TEXT);
        wdog_put_str(&t, "wdog: PET(0x86000007)=");
        wdog_put_error(&t, r.error);
        wdog_put_str(&t, source == WDOG_SOURCE_THREAD ? " from the pet thread, failure " : " from the observer, failure ");
        wdog_put_dec(&t, n);
        ramoops_log_line(RAMOOPS_KIND_TRACE, t.at);
        return;
    }
    uint64_t previous = __atomic_exchange_n(&wdog_last_pet, now, __ATOMIC_ACQ_REL);
    if (now > previous) wdog_note_gap(now - previous);
    __atomic_add_fetch(&wdog_pets[source == WDOG_SOURCE_THREAD ? 0 : 1], 1, __ATOMIC_RELAXED);
}

// One round of the pet thread; the milliseconds until the next one, 0 when the thread ends (the watchdog is not armed by
// us any more, a panic stopped the pets, or the stop-pet test has logged enough). A round whose pet is withheld (G5) goes on
// as usual: the thread pets again as soon as the boot CPU and the observer are seen alive.
static uint32_t wdog_thread_step(void)
{
    int state = __atomic_load_n(&wdog_state, __ATOMIC_ACQUIRE);
    if (state == WDOG_ARMING) return GUNYAH_WDOG_PET_MS;
    if (state != WDOG_ARMED || __atomic_load_n(&wdog_panicked, __ATOMIC_ACQUIRE)) return 0;
    uint64_t now = wdog_now();
    struct wdog_text t;
    if (wdog_may_pet(now, wdog_thread_text)) {
        // The thread's own lateness, from its previous round (a withheld round counts: it ran).
        uint64_t gap = wdog_last_thread_round && now > wdog_last_thread_round ? now - wdog_last_thread_round : 0;
        wdog_last_thread_round = now;
        if (gap > (uint64_t)WDOG_LATE_PERIODS * GUNYAH_WDOG_PET_MS * wdog_hz / 1000 &&
            (!wdog_last_late_report || now - wdog_last_late_report >= (uint64_t)WDOG_LATE_REPORT_SECONDS * wdog_hz)) {
            wdog_last_late_report = now;
            struct wdog_result r = wdog_call(GUNYAH_WDOG_FN_STATUS, 0, 0);
            wdog_text_start(&t, wdog_thread_text, WDOG_TEXT);
            wdog_put_str(&t, "wdog late: the pet thread ran ");
            wdog_put_dec(&t, wdog_ms(gap));
            wdog_put_str(&t, " ms after its previous round; ");
            wdog_put_status(&t, r);
            ramoops_log_line(RAMOOPS_KIND_TRACE, t.at);
        }
        if (wdog_alive(now, wdog_thread_text)) wdog_pet(WDOG_SOURCE_THREAD, now, wdog_thread_text);
        if (now - wdog_last_summary >= (uint64_t)WDOG_SUMMARY_SECONDS * wdog_hz) {
            wdog_last_summary = now;
            wdog_text_start(&t, wdog_thread_text, WDOG_TEXT);
            wdog_put_str(&t, "wdog ok: armed ");
            wdog_put_dec(&t, wdog_ms_since(wdog_armed_at, now) / 1000);
            wdog_put_str(&t, " s, pets thread ");
            wdog_put_dec(&t, __atomic_load_n(&wdog_pets[0], __ATOMIC_RELAXED));
            wdog_put_str(&t, " observer ");
            wdog_put_dec(&t, __atomic_load_n(&wdog_pets[1], __ATOMIC_RELAXED));
            wdog_put_str(&t, " failed ");
            wdog_put_dec(&t, __atomic_load_n(&wdog_pet_failures, __ATOMIC_RELAXED));
            wdog_put_str(&t, ", longest gap ");
            wdog_put_dec(&t, wdog_ms(__atomic_load_n(&wdog_max_gap, __ATOMIC_RELAXED)));
            wdog_put_str(&t, " ms (bark ");
            wdog_put_dec(&t, wdog_bark_ms);
            wdog_put_str(&t, "), cpu ");
            wdog_put_dec(&t, (uint64_t)(uint32_t)wdog_beat_cpu);
            wdog_put_str(&t, " beats ");
            wdog_put_dec(&t, __atomic_load_n(&wdog_beats, __ATOMIC_RELAXED));
            wdog_put_str(&t, " (gap <= ");
            wdog_put_dec(&t, wdog_ms(__atomic_load_n(&wdog_beat_max_gap, __ATOMIC_RELAXED)));
            wdog_put_str(&t, " ms), withheld ");
            wdog_put_dec(&t, __atomic_load_n(&wdog_withheld_count, __ATOMIC_RELAXED));
            ramoops_log_line(RAMOOPS_KIND_TRACE, t.at);
        }
        return GUNYAH_WDOG_PET_MS;
    }
    // The stop-pet test: what the hypervisor reports while the bark and the bite come.
    if (__atomic_load_n(&wdog_test_stopped, __ATOMIC_ACQUIRE) && wdog_test_lines < WDOG_TEST_LINES) {
        wdog_test_lines++;
        struct wdog_result r = wdog_call(GUNYAH_WDOG_FN_STATUS, 0, 0);
        wdog_text_start(&t, wdog_thread_text, WDOG_TEXT);
        wdog_put_str(&t, "wdog test: ");
        wdog_put_status(&t, r);
        wdog_put_str(&t, " (bark ");
        wdog_put_dec(&t, wdog_bark_ms);
        wdog_put_str(&t, ", bite ");
        wdog_put_dec(&t, wdog_bite_ms);
        wdog_put_str(&t, ")");
        ramoops_log_line(RAMOOPS_KIND_RESET, t.at);
        return GUNYAH_WDOG_PET_MS;
    }
    return 0;
}

void gunyah_wdog_start(void)
{
    if (__atomic_exchange_n(&wdog_started, 1, __ATOMIC_ACQ_REL)) return;
    wdog_hvc = 0; wdog_pets_stopped = 0; wdog_test_stopped = 0; wdog_panicked = 0; wdog_panic_waits = 0; wdog_resetting = 0;
    wdog_requested = 0; wdog_bite_ms = 0; wdog_bark_ms = 0; wdog_stop_pet_seconds = 0; wdog_test_lines = 0;
    wdog_pets[0] = 0; wdog_pets[1] = 0; wdog_pet_failures = 0;
    wdog_armed_at = 0; wdog_last_pet = 0; wdog_max_gap = 0;
    wdog_last_thread_round = 0; wdog_last_late_report = 0; wdog_last_summary = 0;
    wdog_beat_cpu = -1; wdog_withheld = 0; wdog_beats = 0; wdog_observer_passes = 0; wdog_withheld_count = 0;
    wdog_beat_at = 0; wdog_beat_max_gap = 0; wdog_observer_at = 0; wdog_withheld_at = 0;
    wdog_status_line[0] = 0;
    __atomic_store_n(&wdog_state, WDOG_OFF, __ATOMIC_RELEASE);
    uint32_t value = 0;
    if (!gunyah_wdog_platform_arg("pagani-wdog", &value) || !value) return;
    wdog_requested = value;
    uint32_t seconds = value < GUNYAH_WDOG_MIN_SECONDS ? GUNYAH_WDOG_MIN_SECONDS :
                       value > GUNYAH_WDOG_MAX_SECONDS ? GUNYAH_WDOG_MAX_SECONDS : value;
    wdog_bite_ms = seconds * 1000;
    wdog_bark_ms = wdog_bite_ms / 2;
    value = 0;
    if (gunyah_wdog_platform_arg("pagani-wdog-stop-pet", &value)) wdog_stop_pet_seconds = value;
    value = 0;
    if (gunyah_wdog_platform_arg("pagani-wdog-hvc", &value)) wdog_hvc = value != 0;
    wdog_hz = gunyah_wdog_platform_frequency();
    if (!wdog_hz) wdog_hz = 19200000;
    __atomic_store_n(&wdog_state, WDOG_ARMING, __ATOMIC_RELEASE);

    struct wdog_text t;
    wdog_text_start(&t, wdog_start_text, WDOG_TEXT);
    wdog_put_str(&t, "wdog config: pagani-wdog=");
    wdog_put_dec(&t, wdog_requested);
    if (seconds != wdog_requested) { wdog_put_str(&t, " (clamped to "); wdog_put_dec(&t, seconds); wdog_put_str(&t, " s)"); }
    wdog_put_str(&t, " bite ");
    wdog_put_dec(&t, wdog_bite_ms);
    wdog_put_str(&t, " ms bark ");
    wdog_put_dec(&t, wdog_bark_ms);
    wdog_put_str(&t, " ms pets every ");
    wdog_put_dec(&t, GUNYAH_WDOG_PET_MS);
    wdog_put_str(&t, wdog_hvc ? " ms conduit hvc (pagani-wdog-hvc)" : " ms conduit smc");
    if (wdog_stop_pet_seconds) {
        wdog_put_str(&t, "; TEST: pets stop ");
        wdog_put_dec(&t, wdog_stop_pet_seconds);
        wdog_put_str(&t, " s after arming");
    }
    ramoops_log_line(RAMOOPS_KIND_RESET, t.at);

    // The probe: an error here (-1: nothing answers the call) leaves the watchdog alone.
    struct wdog_result probe = wdog_call(GUNYAH_WDOG_FN_STATUS, 0, 0);
    wdog_text_start(&t, wdog_start_text, WDOG_TEXT);
    wdog_put_str(&t, "wdog probe: ");
    wdog_put_status(&t, probe);
    if (probe.error) {
        wdog_put_str(&t, wdog_hvc ? "; off: no Gunyah watchdog answers hvc" : "; off: no Gunyah watchdog answers smc");
        wdog_fail(&t, "wdog off: STATUS ", 1, probe.error);
        return;
    }
    ramoops_log_line(RAMOOPS_KIND_RESET, t.at);
    if (probe.a1 & GUNYAH_WDOG_STATUS_ENABLED) {
        struct wdog_result off = wdog_call(GUNYAH_WDOG_FN_CONTROL, GUNYAH_WDOG_CTRL_DISABLE, 0);
        wdog_text_start(&t, wdog_start_text, WDOG_TEXT);
        wdog_put_str(&t, "wdog: found enabled before arming; CONTROL(0x86000005, 2)=");
        wdog_put_error(&t, off.error);
        ramoops_log_line(RAMOOPS_KIND_RESET, t.at);
    }
    // The pet thread before the enable: it waits while arming, so the first pets follow the enable at once.
    if (!gunyah_wdog_platform_start_thread()) {
        wdog_text_start(&t, wdog_start_text, WDOG_TEXT);
        wdog_put_str(&t, "wdog off: the pet thread did not start; the watchdog is not enabled");
        wdog_fail(&t, "wdog off: no pet thread", 0, 0);
        return;
    }
    // The boot CPU's heartbeat before the enable as well (this thread runs bound to the boot CPU): no pet without it (G5).
    wdog_beat_cpu = gunyah_wdog_platform_start_beat();
    if (wdog_beat_cpu < 0) {
        wdog_text_start(&t, wdog_start_text, WDOG_TEXT);
        wdog_put_str(&t, "wdog off: the boot CPU heartbeat did not start; the watchdog is not enabled");
        wdog_fail(&t, "wdog off: no CPU heartbeat", 0, 0);
        return;
    }
    wdog_text_start(&t, wdog_start_text, WDOG_TEXT);
    wdog_put_str(&t, "wdog heartbeat: a local timer on cpu ");
    wdog_put_dec(&t, (uint64_t)(uint32_t)wdog_beat_cpu);
    wdog_put_str(&t, " every ");
    wdog_put_dec(&t, GUNYAH_WDOG_BEAT_MS);
    wdog_put_str(&t, " ms; a pet needs a beat and an observer pass within the last ");
    wdog_put_dec(&t, GUNYAH_WDOG_ALIVE_MS);
    wdog_put_str(&t, " ms");
    ramoops_log_line(RAMOOPS_KIND_RESET, t.at);
    struct wdog_result times = wdog_call(GUNYAH_WDOG_FN_SET_TIME, wdog_bark_ms, wdog_bite_ms);
    if (times.error) {
        wdog_text_start(&t, wdog_start_text, WDOG_TEXT);
        wdog_put_str(&t, "wdog off: SET_TIME(0x86000008, bark ");
        wdog_put_dec(&t, wdog_bark_ms);
        wdog_put_str(&t, ", bite ");
        wdog_put_dec(&t, wdog_bite_ms);
        wdog_put_str(&t, ")=");
        wdog_put_error(&t, times.error);
        wdog_put_str(&t, "; not enabled");
        wdog_fail(&t, "wdog off: SET_TIME ", 1, times.error);
        return;
    }
    struct wdog_result on = wdog_call(GUNYAH_WDOG_FN_CONTROL, GUNYAH_WDOG_CTRL_ENABLE, 0);
    if (on.error) {
        wdog_text_start(&t, wdog_start_text, WDOG_TEXT);
        wdog_put_str(&t, "wdog off: CONTROL(0x86000005, 3)=");
        wdog_put_error(&t, on.error);
        wdog_fail(&t, "wdog off: CONTROL ", 1, on.error);
        return;
    }
    uint64_t now = wdog_now();
    wdog_armed_at = now;
    wdog_last_summary = now;
    __atomic_store_n(&wdog_last_pet, now, __ATOMIC_RELEASE);
    // The heartbeat's first beat is still due (one period after its start): until then it counts from the arming.
    uint64_t unbeaten = 0;
    (void)__atomic_compare_exchange_n(&wdog_beat_at, &unbeaten, now, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    __atomic_store_n(&wdog_state, WDOG_ARMED, __ATOMIC_RELEASE);
    struct wdog_result pet = wdog_call(GUNYAH_WDOG_FN_PET, 0, 0);
    struct wdog_result readback = wdog_call(GUNYAH_WDOG_FN_STATUS, 0, 0);
    // The bite configuration as the hypervisor took it.
    wdog_text_start(&t, wdog_start_text, WDOG_TEXT);
    wdog_put_str(&t, "wdog armed: SET_TIME(0x86000008, bark ");
    wdog_put_dec(&t, wdog_bark_ms);
    wdog_put_str(&t, ", bite ");
    wdog_put_dec(&t, wdog_bite_ms);
    wdog_put_str(&t, ")=0 CONTROL(0x86000005, 3)=0 PET(0x86000007)=");
    wdog_put_error(&t, pet.error);
    wdog_put_char(&t, ' ');
    wdog_put_status(&t, readback);
    if (!readback.error && !(readback.a1 & GUNYAH_WDOG_STATUS_ENABLED)) wdog_put_str(&t, " (reads back disabled)");
    ramoops_log_line(RAMOOPS_KIND_RESET, t.at);
    wdog_text_start(&t, wdog_status_line, sizeof(wdog_status_line));
    wdog_put_str(&t, "wdog on: bite ");
    wdog_put_dec(&t, seconds);
    wdog_put_str(&t, wdog_hvc ? " s, bark at half, Gunyah hvc" : " s, bark at half, Gunyah smc");
    if (wdog_stop_pet_seconds) { wdog_put_str(&t, "; TEST stop-pet "); wdog_put_dec(&t, wdog_stop_pet_seconds); wdog_put_str(&t, " s"); }
}

#ifdef GUNYAH_WDOG_HOST
// The host tests run the pet thread's rounds and the heartbeat's beats themselves.
uint32_t gunyah_wdog_host_thread_step(void);
uint32_t gunyah_wdog_host_thread_step(void) { return wdog_thread_step(); }
int gunyah_wdog_host_beat(void);
int gunyah_wdog_host_beat(void) { return wdog_beat_step(); }
#endif

const char *gunyah_wdog_status(void) { return wdog_status_line[0] ? wdog_status_line : NULL; }

void gunyah_wdog_tick(void)
{
    if (__atomic_load_n(&wdog_state, __ATOMIC_ACQUIRE) != WDOG_ARMED) return;
    uint64_t now = wdog_now();
    __atomic_store_n(&wdog_observer_at, now, __ATOMIC_RELEASE);     // this pass shows the observer alive (G5)
    __atomic_add_fetch(&wdog_observer_passes, 1, __ATOMIC_RELAXED);
    if (wdog_may_pet(now, wdog_tick_text) && wdog_alive(now, wdog_tick_text)) wdog_pet(WDOG_SOURCE_OBSERVER, now, wdog_tick_text);
}

// Disables the watchdog armed by us; the caller logs the result. 0 when it was not armed by us.
static int wdog_disable(struct wdog_result *r)
{
    int expected = WDOG_ARMED;
    if (!__atomic_compare_exchange_n(&wdog_state, &expected, WDOG_STOPPED, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return 0;
    *r = wdog_call(GUNYAH_WDOG_FN_CONTROL, GUNYAH_WDOG_CTRL_DISABLE, 0);
    return 1;
}

// The UFS log (UfsLog.c): nothing to wait for without it, as in BootProgress's host tests.
#ifndef GUNYAH_WDOG_TEST
__attribute__((weak)) int ufs_log_panic_pending(void) { return 0; }
#endif

int gunyah_wdog_armed(void) { return __atomic_load_n(&wdog_state, __ATOMIC_ACQUIRE) == WDOG_ARMED; }

void gunyah_wdog_stop_pets(void) { __atomic_store_n(&wdog_pets_stopped, 1, __ATOMIC_RELEASE); }

// What a panic does to an armed watchdog, once any flush to the UFS log is over.
static void wdog_panic_settle(void)
{
    struct wdog_text t;
    wdog_text_start(&t, wdog_panic_text, WDOG_TEXT);
    if (ramoops_log_panic_footer()) {
        // RamoopsLog resets after its wait: no bite may race that reset.
        struct wdog_result r = {0, 0, 0};
        if (!wdog_disable(&r)) return;
        wdog_put_str(&t, "wdog panic: pets stopped; RamoopsLog resets, so CONTROL(0x86000005, 2)=");
        wdog_put_error(&t, r.error);
        wdog_put_str(&t, r.error ? " (still armed: the bite may come first)" : " disabled the watchdog");
    } else {
        uint64_t since = wdog_ms_since(__atomic_load_n(&wdog_last_pet, __ATOMIC_ACQUIRE), wdog_now());
        wdog_put_str(&t, "wdog panic: pets stopped, no software reset follows: the bite resets the phone in <= ");
        wdog_put_dec(&t, since < wdog_bite_ms ? wdog_bite_ms - since : 0);
        wdog_put_str(&t, " ms (last pet ");
        wdog_put_dec(&t, since);
        wdog_put_str(&t, " ms ago)");
    }
    ramoops_log_line(RAMOOPS_KIND_RESET, t.at);
}

void gunyah_wdog_panic(void)
{
    gunyah_wdog_stop_pets();
    if (__atomic_exchange_n(&wdog_panicked, 1, __ATOMIC_ACQ_REL)) return;
    if (__atomic_load_n(&wdog_state, __ATOMIC_ACQUIRE) != WDOG_ARMED) return;
    // The UFS log's last snapshot runs with interrupts masked on a bus that may hang: disabled before it, nothing would be left to reset
    // the phone. The bite races nothing yet; RamoopsLog settles this right after the flush (gunyah_wdog_panic_flushed).
    if (ramoops_log_panic_footer() && ufs_log_panic_pending()) {
        __atomic_store_n(&wdog_panic_waits, 1, __ATOMIC_RELEASE);
        return;
    }
    wdog_panic_settle();
}

void gunyah_wdog_panic_flushed(void)
{
    if (__atomic_exchange_n(&wdog_panic_waits, 0, __ATOMIC_ACQ_REL) && __atomic_load_n(&wdog_state, __ATOMIC_ACQUIRE) == WDOG_ARMED) wdog_panic_settle();
}

const char *gunyah_wdog_panic_footer(void)
{
    if (!__atomic_load_n(&wdog_panicked, __ATOMIC_ACQUIRE) || __atomic_load_n(&wdog_state, __ATOMIC_ACQUIRE) != WDOG_ARMED)
        return NULL;
    uint64_t since = wdog_ms_since(__atomic_load_n(&wdog_last_pet, __ATOMIC_ACQUIRE), wdog_now());
    uint64_t left = since < wdog_bite_ms ? wdog_bite_ms - since : 0;
    struct wdog_text t;
    wdog_text_start(&t, wdog_footer, sizeof(wdog_footer));
    wdog_put_str(&t, "WATCHDOG RESET IN <= ");
    wdog_put_dec(&t, (left + 999) / 1000);
    wdog_put_str(&t, " s (pagani-wdog)");
    return wdog_footer;
}

void gunyah_wdog_before_reset(const char *reason)
{
    __atomic_store_n(&wdog_pets_stopped, 1, __ATOMIC_RELEASE);
    if (__atomic_exchange_n(&wdog_resetting, 1, __ATOMIC_ACQ_REL)) return;
    struct wdog_result r = {0, 0, 0};
    if (!wdog_disable(&r)) return;
    struct wdog_text t;
    wdog_text_start(&t, wdog_reset_text, WDOG_TEXT);
    wdog_put_str(&t, "wdog: ");
    wdog_put_str(&t, reason ? reason : "?");
    wdog_put_str(&t, " reset: pets stopped, CONTROL(0x86000005, 2)=");
    wdog_put_error(&t, r.error);
    wdog_put_str(&t, r.error ? " (still armed)" : " disabled the watchdog");
    ramoops_log_line(RAMOOPS_KIND_RESET, t.at);
}

int gunyah_wdog_bite_reset(const char *reason)
{
    __atomic_store_n(&wdog_pets_stopped, 1, __ATOMIC_RELEASE);
    if (__atomic_load_n(&wdog_state, __ATOMIC_ACQUIRE) != WDOG_ARMED) return 0;
    if (__atomic_exchange_n(&wdog_resetting, 1, __ATOMIC_ACQ_REL)) return 1;
    const uint32_t bite = GUNYAH_WDOG_MIN_SECONDS * 1000, bark = bite / 2;
    struct wdog_result times = wdog_call(GUNYAH_WDOG_FN_SET_TIME, bark, bite);
    struct wdog_text t;
    wdog_text_start(&t, wdog_bite_text, WDOG_TEXT);
    wdog_put_str(&t, "wdog: ");
    wdog_put_str(&t, reason ? reason : "?");
    wdog_put_str(&t, " reset by the bite: pets stopped, SET_TIME(0x86000008, bark ");
    wdog_put_dec(&t, bark);
    wdog_put_str(&t, ", bite ");
    wdog_put_dec(&t, bite);
    wdog_put_str(&t, ")=");
    wdog_put_error(&t, times.error);
    ramoops_log_line(RAMOOPS_KIND_RESET, t.at);
    return 1;
}

// pagani-reset-seq (the 2026-10-31 review's S3): gunyah_wdog_before_reset disabled the watchdog before the reset steps, so an SCM
// call that never returned (or a CPU park that wedged) left nothing but a manual reset. The pet comes first, so the bite is 4 s from
// now even when the last pet is older than that (pets withheld before the reset); the steps take about a second at most.
int gunyah_wdog_hold_for_steps(const char *reason)
{
    __atomic_store_n(&wdog_pets_stopped, 1, __ATOMIC_RELEASE);
    if (__atomic_load_n(&wdog_state, __ATOMIC_ACQUIRE) != WDOG_ARMED) return 0;
    if (__atomic_exchange_n(&wdog_resetting, 1, __ATOMIC_ACQ_REL)) return 2;
    const uint32_t bite = GUNYAH_WDOG_MIN_SECONDS * 1000, bark = bite / 2;
    struct wdog_result pet = wdog_call(GUNYAH_WDOG_FN_PET, 0, 0);
    if (!pet.error) __atomic_store_n(&wdog_last_pet, wdog_now(), __ATOMIC_RELEASE);
    struct wdog_result times = wdog_call(GUNYAH_WDOG_FN_SET_TIME, bark, bite);
    struct wdog_text t;
    wdog_text_start(&t, wdog_hold_text, WDOG_TEXT);
    wdog_put_str(&t, "wdog: ");
    wdog_put_str(&t, reason ? reason : "?");
    wdog_put_str(&t, " reset: pets stopped, PET(0x86000007)=");
    wdog_put_error(&t, pet.error);
    wdog_put_str(&t, " SET_TIME(0x86000008, bark ");
    wdog_put_dec(&t, bark);
    wdog_put_str(&t, ", bite ");
    wdog_put_dec(&t, bite);
    wdog_put_str(&t, ")=");
    wdog_put_error(&t, times.error);
    wdog_put_str(&t, ": armed through the reset steps (pagani-reset-seq)");
    ramoops_log_line(RAMOOPS_KIND_RESET, t.at);
    return 1;
}

// Whoever held it: the steps' CPU calls this once they are done (a second resetter that only waited for them does not).
void gunyah_wdog_release_hold(const char *reason)
{
    struct wdog_result r = {0, 0, 0};
    if (!wdog_disable(&r)) return;
    struct wdog_text t;
    wdog_text_start(&t, wdog_release_text, WDOG_TEXT);
    wdog_put_str(&t, "wdog: ");
    wdog_put_str(&t, reason ? reason : "?");
    wdog_put_str(&t, " reset steps done: CONTROL(0x86000005, 2)=");
    wdog_put_error(&t, r.error);
    wdog_put_str(&t, r.error ? " (still armed)" : " disabled the watchdog before PSCI");
    ramoops_log_line(RAMOOPS_KIND_RESET, t.at);
}

// A reset that returned left the phone in RamoopsLog's retry loop with the watchdog disabled (2026-10-30 18:35: a run that never
// came back needed a manual power + volume-down reset): the shortest bite, counted from the last pet before the reset, ends it.
void gunyah_wdog_after_failed_reset(void)
{
    int expected = WDOG_STOPPED;
    if (!__atomic_compare_exchange_n(&wdog_state, &expected, WDOG_ARMED, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return;
    const uint32_t bite = GUNYAH_WDOG_MIN_SECONDS * 1000, bark = bite / 2;
    struct wdog_result times = wdog_call(GUNYAH_WDOG_FN_SET_TIME, bark, bite);
    struct wdog_result on = wdog_call(GUNYAH_WDOG_FN_CONTROL, GUNYAH_WDOG_CTRL_ENABLE, 0);
    struct wdog_text t;
    wdog_text_start(&t, wdog_retry_text, WDOG_TEXT);
    wdog_put_str(&t, "wdog: the PSCI reset returned: SET_TIME(0x86000008, bark ");
    wdog_put_dec(&t, bark);
    wdog_put_str(&t, ", bite ");
    wdog_put_dec(&t, bite);
    wdog_put_str(&t, ")=");
    wdog_put_error(&t, times.error);
    wdog_put_str(&t, " CONTROL(0x86000005, 3)=");
    wdog_put_error(&t, on.error);
    wdog_put_str(&t, ", no pets: the bite resets the phone");
    ramoops_log_line(RAMOOPS_KIND_RESET, t.at);
}
