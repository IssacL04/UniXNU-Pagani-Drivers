// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test of RamoopsLog.c (the actual writer): record encoding and CRC32, the two zones' Linux persistent_ram_buffer headers
// as Linux's persistent_ram_post_init/save_old read them, the 36 copies of layout 3 (0xfffc bytes: 4 in the console zone,
// 32 in pmsg; identical but their copy number) with their own headers, the copy size and count rules for other zone sizes,
// the boot area and the events and console rings (routing, wrap, overflow), console line assembly, status rows, the region
// guards, the PSCI reset path (PSCI_FEATURES -> SYSTEM_RESET2 warm, else SYSTEM_RESET), the reset timer, its observer
// fallback and the second reset timer the observer arms on another CPU, the panic reset and lock contention, the frame-rate F/J records with a slow frame's breakdown, and the heartbeat's
// CPU clock (CpuClock.h: the rate arithmetic, the best of the windows, the window bounds, one real measurement on arm64 hosts).
//   cc -std=c11 -O2 -Wall -Wextra -Werror -fsanitize=address,undefined ramoops.c -o ramoops && ./ramoops
//   ./ramoops --dump DIR   also writes one run's two zone images and the records it wrote (gpu-test-loop decoder test)
#define RAMOOPS_LOG_TEST 1
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../RamoopsLog.c"

// REGION: the 13T's two zones; REGION_MAX: the largest region a test maps (two 4 MiB zones, for the copy limit).
enum { REGION = RAMOOPS_LOG_DEFAULT_CONSOLE + RAMOOPS_LOG_DEFAULT_PMSG, REGION_MAX = 0x800000, GUARD = 64, HZ = 19200000 };

// ---- platform -------------------------------------------------------------------------------------------------------------
static uint8_t *region;
static uint64_t mapped_pa, mapped_bytes;
static unsigned maps;
static int mapped_device;
static uint64_t clock_now = 1000000, clock_step = 1;
static struct { const char *name; uint64_t value; } table[16];
static unsigned table_count;
static const char *boot_args_text = "-v serial=0 pagani-ramoops=1";
static uint32_t psci_version_value = 0x10001;
static int32_t reset2_features_value = 0;
static int64_t reset2_result = -2, reset_result = -1, off_result = -1;
static char method_value[4] = "smc";
static int method_present = 1;
static struct { int hvc, masked; uint64_t function, a, b, c; } calls[64];   // masked: RamoopsLog's DAIF stand-in at the call
static unsigned call_count;
static uint64_t timer_ms;
static unsigned timer_arms;
static uint64_t ram_base = 0x8c0000000ULL, ram_bytes = 0x100000000ULL;

uintptr_t ramoops_platform_map(uint64_t pa, uint64_t bytes, int device)
{
    maps++;
    mapped_device = device;
    mapped_pa = pa;
    mapped_bytes = bytes;
    assert(bytes <= REGION_MAX);
    memset(region, 0xa5, REGION_MAX + 2 * GUARD);   // stale contents: the writer clears both zones
    return (uintptr_t)(region + GUARD);
}

int ramoops_platform_arg(const char *name, void *value, unsigned bytes)
{
    for (unsigned i = 0; i < table_count; i++) {
        if (strcmp(table[i].name, name)) continue;
        if (bytes == 8) { uint64_t v = table[i].value; memcpy(value, &v, 8); }
        else { assert(bytes == 4); uint32_t v = (uint32_t)table[i].value; memcpy(value, &v, 4); }
        return 1;
    }
    return 0;
}

const char *ramoops_platform_boot_args(void) { return boot_args_text; }
// The pagani-reset-seq CPU park answers late when park_due is set: the parked count appears once the counter passes it.
static uint32_t *park_target, park_arrive;
static uint64_t park_due;
uint64_t ramoops_platform_counter(void)
{
    uint64_t now = __atomic_add_fetch(&clock_now, clock_step, __ATOMIC_RELAXED);
    if (park_target && now >= park_due) { __atomic_store_n(park_target, park_arrive, __ATOMIC_RELEASE); park_target = NULL; }
    return now;
}
uint64_t ramoops_platform_frequency(void) { return HZ; }

// Every firmware call in one order (PSCI, SCM, the CPU park as TRACE_PARK, the watchdog's hold for the steps and its release as
// TRACE_HOLD and TRACE_RELEASE), so a test checks the sequence across them. The fakes take one lock: the concurrent-reset test
// calls them from two threads.
enum { TRACE_PARK = 0xa5a5, TRACE_HOLD = 0xb01d, TRACE_RELEASE = 0x0ff, TRACE_BITE = 0xb17e, TRACE_MAX = 512 };
static pthread_mutex_t platform_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t trace[TRACE_MAX];
static unsigned trace_count;
static void trace_add(uint64_t what) { assert(trace_count < TRACE_MAX); trace[trace_count++] = what; }

int64_t ramoops_platform_psci(int hvc, uint64_t function, uint64_t a, uint64_t b, uint64_t c)
{
    pthread_mutex_lock(&platform_lock);
    assert(call_count < 64);
    calls[call_count].hvc = hvc; calls[call_count].masked = host_daif_masked; calls[call_count].function = function;
    calls[call_count].a = a; calls[call_count].b = b; calls[call_count].c = c;
    call_count++;
    trace_add(function);
    pthread_mutex_unlock(&platform_lock);
    switch (function) {
    case 0x84000000: return (int64_t)psci_version_value;
    case 0x8400000a: return a == 0xc4000012 ? (int64_t)(uint32_t)reset2_features_value : -1;
    case 0xc4000012: return reset2_result;
    case 0x84000009: return reset_result;
    case 0x84000008: return off_result;
    default: assert(0); return -1;
    }
}

// ---- the SCM conduit and the CPU park of pagani-reset-seq ---------------------------------------------------------------
// The firmware as far as the steps see it: the download mode register at 0x1fd9000 (IO_READ answers it, IO_WRITE stores into
// it), SDI and the SPMI arbiter (each call recorded), IS_CALL_AVAIL (x1 from avail_x1 by the queried id), the OEM reboot
// (returns oem_x0: on the phone it would not return). scm_fail sets x0 for one function id. scm_interrupts INTERRUPTED answers
// come before each final one, each with a new x6 session that the continuation must send back with x0 = 1 and the request's
// x1..x3 unchanged. smc_hook runs after the SMC of function smc_hook_function is answered (outside the lock).
static struct { uint64_t x0, x1, x2, x3, x6; } smcs[TRACE_MAX];
static unsigned smc_count, scm_interrupts, park_calls, park_signalled;
static uint64_t dload_value, sdi_calls, arbiter_calls, oem_x0;
static uint32_t scm_fail_function, smc_hook_function;
static int64_t scm_fail_x0;
static uint32_t avail_ids[8];
static uint64_t avail_x1[8];
static void (*smc_hook)(void);
static uint64_t interrupted_function, interrupted_args[3], interrupted_session, session_counter;
static unsigned interrupted_left;
static uint32_t interrupt_only;            // scm_interrupts for this function id only (0: every call)
static uint64_t read_x0_upper, read_x1_upper;   // IO_READ answers with these upper halves (Linux reads both as 32 bits)
static int seq_expected;                   // a test that sets pagani-reset-seq: fresh() skips the zero-calls check

void ramoops_platform_smc(uint64_t regs[8])
{
    pthread_mutex_lock(&platform_lock);
    assert(smc_count < TRACE_MAX);
    smcs[smc_count].x0 = regs[0]; smcs[smc_count].x1 = regs[1]; smcs[smc_count].x2 = regs[2];
    smcs[smc_count].x3 = regs[3]; smcs[smc_count].x6 = regs[6];
    smc_count++;
    trace_add(regs[0]);
    assert(!regs[4] && !regs[5] && !regs[7]);
    uint64_t function = regs[0];
    if (regs[0] == 1) {
        // A continuation: the interrupted request's x1..x3 and the session it got back.
        assert(interrupted_function && regs[1] == interrupted_args[0] && regs[2] == interrupted_args[1] &&
               regs[3] == interrupted_args[2] && regs[6] == interrupted_session);
        function = interrupted_function;
    } else {
        assert(!regs[6]);                  // a new request sends x6 = 0
        interrupted_function = function; interrupted_left = !interrupt_only || interrupt_only == function ? scm_interrupts : 0;
        interrupted_args[0] = regs[1]; interrupted_args[1] = regs[2]; interrupted_args[2] = regs[3];
    }
    if (interrupted_left) {
        interrupted_left--;
        regs[0] = 1;
        regs[6] = interrupted_session = 0x5e55000 + ++session_counter;
        pthread_mutex_unlock(&platform_lock);
        return;
    }
    interrupted_function = 0;
    int64_t x0 = 0;
    uint64_t x1 = 0x77;                    // x1 of a call that returns nothing: never logged
    const int fail = function == scm_fail_function;
    switch (function) {
    case 0xc2000501:
        assert(regs[1] == 1);
        if (regs[2] == 0x1fd9000) { x1 = dload_value | read_x1_upper; x0 = (int64_t)read_x0_upper; }
        else x0 = -3;
        break;
    case 0xc2000502: assert(regs[1] == 2 && regs[2] == 0x1fd9000); if (!fail) dload_value = regs[3]; break;
    case 0xc2000109: assert(regs[1] == 2 && regs[2] == 1 && regs[3] == 0); sdi_calls++; break;
    case 0xc2000901: assert(regs[1] == 1 && regs[2] == 0); arbiter_calls++; break;
    case 0xc3000922: assert(regs[1] == 0 && regs[2] == 0 && regs[3] == 0); x0 = (int64_t)oem_x0; break;
    case 0x42000601:
        assert(regs[1] == 1);
        x1 = 0;
        for (unsigned i = 0; i < 8; i++) if (avail_ids[i] == regs[2]) x1 = avail_x1[i];
        break;
    default: assert(0);
    }
    if (fail) x0 = scm_fail_x0;
    regs[0] = (uint64_t)x0;
    regs[1] = x1;
    regs[2] = 0x2222; regs[3] = 0x3333;    // x2/x3 come back changed: never read
    void (*hook)(void) = function == smc_hook_function ? smc_hook : NULL;
    pthread_mutex_unlock(&platform_lock);
    if (hook) hook();
}

unsigned ramoops_platform_park_others(uint32_t *parked)
{
    pthread_mutex_lock(&platform_lock);
    park_calls++;
    trace_add(TRACE_PARK);
    pthread_mutex_unlock(&platform_lock);
    if (!park_due) __atomic_store_n(parked, park_arrive, __ATOMIC_RELEASE);
    else park_target = parked;
    return park_signalled;
}

// The watchdog's side of a reset (GunyahWdog.c in the kernel, Test/gunyah-wdog.c tests it): pagani-reset-wdog's bite (answers
// bite_answer: 1 when our watchdog is armed and bites), and with pagani-reset-seq each resetter's hold before anything else and the
// release by the steps' CPU after them. hold_masked: the DAIF stand-in at the hold.
static unsigned hold_calls, release_calls, bite_calls;
static int hold_masked = -1, bite_answer;
int ramoops_platform_wdog_bite(const char *reason)
{
    assert(reason);
    pthread_mutex_lock(&platform_lock);
    bite_calls++;
    trace_add(TRACE_BITE);
    pthread_mutex_unlock(&platform_lock);
    return bite_answer;
}
int ramoops_platform_wdog_hold(const char *reason)
{
    assert(reason);
    pthread_mutex_lock(&platform_lock);
    hold_calls++;
    hold_masked = host_daif_masked;
    trace_add(TRACE_HOLD);
    pthread_mutex_unlock(&platform_lock);
    return 1;
}
void ramoops_platform_wdog_release(const char *reason)
{
    assert(reason);
    pthread_mutex_lock(&platform_lock);
    release_calls++;
    trace_add(TRACE_RELEASE);
    pthread_mutex_unlock(&platform_lock);
}
void ramoops_platform_wdog_flushed(void) {}

int ramoops_platform_psci_method(char method[4])
{
    if (!method_present) return 0;
    memcpy(method, method_value, 4);
    return 1;
}

// The two reset timers: the first one lands on timer_cpu_value (the boot CPU), the second on the observer's CPU (observer_cpu)
// unless that is the CPU it must avoid.
static int timer_cpu_value, observer_cpu, backup_cpu_armed = -1, backup_avoided = -2;
static uint64_t backup_ms;
static unsigned backup_calls, backup_arms;
int ramoops_platform_arm_timer(uint64_t milliseconds) { timer_ms = milliseconds; timer_arms++; return timer_cpu_value; }
int ramoops_platform_arm_backup_timer(uint64_t milliseconds, int avoid)
{
    backup_calls++;
    backup_avoided = avoid;
    if (observer_cpu == avoid) return -1;
    backup_ms = milliseconds;
    backup_arms++;
    backup_cpu_armed = observer_cpu;
    return observer_cpu;
}
void ramoops_platform_ram(uint64_t *base, uint64_t *bytes) { *base = ram_base; *bytes = ram_bytes; }
// The heartbeat's CPU clock (in the kernel CpuClock.h on the observer's CPU): a fixed answer here, its calls counted.
static uint32_t cpu_mhz_value = 4320;
static int cpu_number_value = 3;
static unsigned cpu_mhz_calls;
uint32_t ramoops_platform_cpu_mhz(int *cpu) { cpu_mhz_calls++; *cpu = cpu_number_value; return cpu_mhz_value; }

static void args_reset(void) { table_count = 0; }
static void arg(const char *name, uint64_t value)
{
    assert(table_count < 16);
    table[table_count].name = name;
    table[table_count].value = value;
    table_count++;
}

static void fresh(void)
{
    // pagani-reset-seq absent (every test but the reset-seq ones): not a single SCM call, CPU park or watchdog hold happened since
    // the last fresh(), whatever the scenario (timer, observer, backup timer, panic, headless panic, retries).
    assert(seq_expected || (!smc_count && !park_calls && !hold_calls && !release_calls));
    seq_expected = 0;
    smc_count = 0; scm_interrupts = 0; park_calls = 0; park_signalled = 0; park_arrive = 0; park_due = 0; park_target = NULL;
    interrupt_only = 0; read_x0_upper = 0; read_x1_upper = 0; hold_calls = 0; release_calls = 0; hold_masked = -1; host_daif_masked = 0; bite_calls = 0; bite_answer = 0;
    dload_value = 0x20; sdi_calls = 0; arbiter_calls = 0; oem_x0 = (uint64_t)-1;
    scm_fail_function = 0; scm_fail_x0 = 0; smc_hook = NULL; smc_hook_function = 0;
    interrupted_function = 0; interrupted_left = 0; session_counter = 0;
    for (unsigned i = 0; i < 8; i++) { avail_ids[i] = 0; avail_x1[i] = 0; }
    trace_count = 0;
    ramoops_log_suspend();
    call_count = 0; timer_arms = 0; timer_ms = 0; maps = 0; clock_step = 1;
    timer_cpu_value = 0; observer_cpu = 0; backup_cpu_armed = -1; backup_avoided = -2; backup_ms = 0; backup_calls = 0; backup_arms = 0;
    psci_version_value = 0x10001; reset2_features_value = 0; reset2_result = -2; reset_result = -1; off_result = -1;
    memcpy(method_value, "smc", 4); method_present = 1;
    ram_base = 0x8c0000000ULL; ram_bytes = 0x100000000ULL;
    boot_args_text = "-v serial=0 pagani-ramoops=1";
    args_reset();
}

// ---- Linux and a reader ---------------------------------------------------------------------------------------------------
// fs/pstore/ram_core.c for a zone with sig 0 and no ECC: persistent_ram_post_init's checks, then persistent_ram_save_old.
// Returns the pstore file's length (0: no file), or -1 when Linux zaps the zone.
static long linux_read(const uint8_t *zone, uint32_t bytes, uint8_t *out)
{
    uint32_t sig, start, size, capacity = bytes - RAMOOPS_LOG_ZONE_HEADER;
    memcpy(&sig, zone, 4); memcpy(&start, zone + 4, 4); memcpy(&size, zone + 8, 4);
    if (sig != 0x43474244) return -1;
    if (size == 0 && start == 0) return 0;
    if (size > capacity || start > size) return -1;
    memcpy(out, zone + RAMOOPS_LOG_ZONE_HEADER + start, size - start);
    memcpy(out + size - start, zone + RAMOOPS_LOG_ZONE_HEADER, start);
    return (long)size;
}

static int header_valid(const uint8_t *zone, uint32_t bytes)
{
    uint32_t sig, start, size;
    memcpy(&sig, zone, 4); memcpy(&start, zone + 4, 4); memcpy(&size, zone + 8, 4);
    return sig == 0x43474244 && size <= bytes - RAMOOPS_LOG_ZONE_HEADER && start <= size;
}

struct rec { uint32_t seq, ms; char kind; char text[RAMOOPS_LOG_TEXT_MAX + 1]; };

static unsigned hexval(const uint8_t *p, unsigned n, uint32_t *out)
{
    uint32_t v = 0;
    for (unsigned i = 0; i < n; i++) {
        uint8_t c = p[i];
        if (c >= '0' && c <= '9') v = v * 16 + (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') v = v * 16 + (uint32_t)(c - 'a' + 10);
        else return 0;
    }
    *out = v;
    return 1;
}

// Strict reader of one area: every '#' that starts a structurally valid record with a matching CRC. A ring is read with its
// first bytes appended, so a record that wraps around the end is found too.
static unsigned parse_area(const uint8_t *area, uint32_t bytes, int ring, struct rec *out, unsigned n, unsigned room)
{
    static uint8_t data[0x40000 + 512];
    uint32_t length = bytes + (ring ? 512 : 0);
    assert(length <= sizeof(data));
    for (uint32_t i = 0; i < length; i++) data[i] = area[i % bytes];
    for (uint32_t at = 0; at < bytes && at + RAMOOPS_LOG_HEADER_BYTES + 1 <= length; at++) {
        const uint8_t *r = data + at;
        uint32_t seq, ms, len, crc;
        if (r[0] != '#' || r[9] != ' ' || r[18] != ' ' || r[21] != ' ' || r[30] != ' ' || r[32] != ' ') continue;
        if (!hexval(r + 1, 8, &seq) || !hexval(r + 10, 8, &ms) || !hexval(r + 19, 2, &len) || !hexval(r + 22, 8, &crc)) continue;
        if (len > RAMOOPS_LOG_TEXT_MAX || at + RAMOOPS_LOG_HEADER_BYTES + len + 1 > length) continue;
        uint32_t check = ~crc_update(crc_update(0xffffffffu, r, 22), r + 30, 3 + len);
        if (check != crc || r[RAMOOPS_LOG_HEADER_BYTES + len] != '\n') continue;
        assert(n < room);
        out[n].seq = seq; out[n].ms = ms; out[n].kind = (char)r[31];
        memcpy(out[n].text, r + RAMOOPS_LOG_HEADER_BYTES, len);
        out[n].text[len] = 0;
        n++;
        at += RAMOOPS_LOG_HEADER_BYTES + len;
    }
    return n;
}

static int by_seq(const void *a, const void *b)
{
    uint32_t x = ((const struct rec *)a)->seq, y = ((const struct rec *)b)->seq;
    return x < y ? -1 : x > y;
}

// Zone z of the mapped region (0: console, 1: pmsg).
static uint8_t *zone_bytes(unsigned z) { return region + GUARD + (z ? zone_size[0] : 0); }

// The two zones as Linux exposes them (persistent_ram_post_init + save_old), cut into copies at the fixed offsets. Every
// copy must equal copy 0 but for its copy number; copy 0's three areas give the records, sorted by sequence number.
static struct rec *records;
static unsigned read_log(unsigned *areas_found)
{
    static uint8_t view[2][REGION_MAX];
    for (unsigned z = 0; z < 2; z++) {
        assert(header_valid(zone_bytes(z), zone_size[z]));
        long length = linux_read(zone_bytes(z), zone_size[z], view[z]);
        assert(length == (long)(zone_size[z] - RAMOOPS_LOG_ZONE_HEADER));   // start 0, size the whole zone
    }
    const uint8_t *first = view[0];
    unsigned index = 0;
    for (unsigned z = 0; z < 2; z++) {
        for (unsigned k = 0; k < zone_copies[z]; k++, index++) {
            const uint8_t *copy = view[z] + (size_t)k * copy_bytes;
            uint32_t magic0, magic1, version, number, count, bytes, per_zone, crc;
            memcpy(&magic0, copy, 4); memcpy(&magic1, copy + 4, 4); memcpy(&version, copy + 8, 4);
            memcpy(&number, copy + 12, 4); memcpy(&count, copy + 16, 4); memcpy(&bytes, copy + 24, 4);
            memcpy(&per_zone, copy + 56, 4); memcpy(&crc, copy + 60, 4);
            assert(!memcmp(copy, "UXNULOG3", 8) && magic0 == RAMOOPS_LOG_MAGIC0 && magic1 == RAMOOPS_LOG_MAGIC1 && version == 3 &&
                   number == index && count == copy_count && bytes == copy_bytes &&
                   per_zone == (zone_copies[0] | zone_copies[1] << 16));
            assert(crc == ~crc_update(crc_update(0xffffffffu, copy, 12), copy + 16, 44));
            assert(!memcmp(copy, first, 12) && !memcmp(copy + 16, first + 16, copy_bytes - 16));
        }
    }
    assert(index == copy_count);
    unsigned n = 0;
    n = parse_area(first + areas[AREA_BOOT].offset, areas[AREA_BOOT].bytes, 0, records, n, 40000);
    if (areas_found) areas_found[0] = n;
    unsigned before = n;
    n = parse_area(first + areas[AREA_EVENTS].offset, areas[AREA_EVENTS].bytes, 1, records, n, 40000);
    if (areas_found) areas_found[1] = n - before;
    before = n;
    n = parse_area(first + areas[AREA_CONSOLE].offset, areas[AREA_CONSOLE].bytes, 1, records, n, 40000);
    if (areas_found) areas_found[2] = n - before;
    qsort(records, n, sizeof(*records), by_seq);
    for (unsigned i = 1; i < n; i++) assert(records[i].seq > records[i - 1].seq);   // no record twice
    return n;
}

// No record lost: the sequence runs 1..n without a gap.
static unsigned read_all(void)
{
    unsigned n = read_log(NULL);
    for (unsigned i = 0; i < n; i++) assert(records[i].seq == i + 1);
    return n;
}

static const struct rec *find(unsigned n, char kind, const char *text)
{
    for (unsigned i = 0; i < n; i++)
        if (records[i].kind == kind && strstr(records[i].text, text)) return &records[i];
    return NULL;
}

// ---- tests ----------------------------------------------------------------------------------------------------------------
static void test_off(void)
{
    fresh();
    assert(!ramoops_log_vm_ready() && maps == 0 && ramoops_log_status() == NULL && !boot_progress_trace_enabled());
    ramoops_log_putc('x'); ramoops_log_line('S', "stage"); ramoops_log_root("r", "d", 1); ramoops_log_exception(1, 2, 3);
    ramoops_log_rows('G', NULL, 0); ramoops_log_panic_putc('p'); boot_progress_trace("t"); boot_progress_trace_values("v", 1, 2);
    observer_cpu = 3;                                // the observer on another CPU: no second timer either
    ramoops_log_start(); ramoops_log_tick(1, 2, 3, 4);
    assert(call_count == 0 && timer_arms == 0 && ramoops_log_panic_footer() == NULL && !cpu_mhz_calls);   // no log: no measurement
    assert(!backup_calls);
    ramoops_log_panic_reset();
    va_list *none = NULL;
    ramoops_log_panic_headless("no screen", none, 1, 2, 3);
    assert(call_count == 0 && !headless_panic);
}

static void test_log(void)
{
    fresh();
    arg("pagani-ramoops", 1);
    assert(ramoops_log_vm_ready() && maps == 1 && mapped_pa == 0x880000000ULL && mapped_bytes == 0x240000 && !mapped_device);
    assert(!strcmp(ramoops_log_status(), "ramoops on 0x880000000 copies 36 x 0xfffc"));
    // Four copies of 0xfffc bytes tile the console zone, 32 the pmsg zone; the areas of a copy: boot area to 4 KiB, events
    // ring 32 KiB, console ring the rest.
    assert(copy_count == 36 && zone_copies[0] == 4 && zone_copies[1] == 32 && copy_bytes == 0xfffc);
    assert(zone_size[0] == 0x40000 && zone_size[1] == 0x200000);
    assert(areas[AREA_BOOT].offset == 64 && areas[AREA_BOOT].bytes == 4096 - 64 && !areas[AREA_BOOT].ring);
    assert(areas[AREA_EVENTS].offset == 4096 && areas[AREA_EVENTS].bytes == 0x8000 && areas[AREA_EVENTS].ring);
    assert(areas[AREA_CONSOLE].offset == 4096 + 0x8000 && areas[AREA_CONSOLE].bytes == 0x6ffc && areas[AREA_CONSOLE].ring);
    ramoops_log_line('S', "S1: XNU entry, MMU off");
    ramoops_log_line('S', "pid=1 launchd exec");            // per-process stages go to the console ring
    const char *text = "IOGPU: hello\r\n\tindented \x01 byte\nsecond line without newline";
    for (const char *p = text; *p; p++) ramoops_log_putc(*p);
    ramoops_log_root("stage7", "GPU board inputs", 0xe00002bc);
    static const char rows[PAGANI_GPU_ROWS][PAGANI_GPU_ROW_BYTES] = {
        "GPU S3 GMU fail 0x1a reg 1a8f8 v 0 e 1", "row one", "row two", "row three"};
    ramoops_log_rows('G', rows, 4);
    ramoops_log_rows('G', rows, 4);                  // unchanged: not logged again
    ramoops_log_rows('G', rows, 2);
    ramoops_log_rows('D', rows, 2);                  // display probe rows need pagani-ramoops bit 1
    ramoops_log_exception(0x96000010, 0xfffffff00bfd9e58, 0xfffffff852055000);
    boot_progress_trace("about to write CP_INIT");
    boot_progress_trace_values("gmu reg", 0x1a8f8, 0x1);
    assert(boot_progress_trace_enabled());
    // No byte outside the region changed; the bytes after the last copy of each zone (4 and 116) stay zero.
    for (unsigned i = 0; i < GUARD; i++) assert(region[i] == 0xa5 && region[GUARD + REGION + i] == 0xa5);
    for (unsigned z = 0; z < 2; z++)
        for (uint32_t i = 12 + zone_copies[z] * copy_bytes; i < zone_size[z]; i++) assert(zone_bytes(z)[i] == 0);
    unsigned found[3];
    unsigned n = read_log(found);
    for (unsigned i = 0; i < n; i++) assert(records[i].seq == i + 1);
    assert(found[0] == 6 && found[1] == 5 && found[2] == 9);    // boot: 5 B + A; events: S R E U U; console: S C C G4 G2
    assert(records[0].kind == 'B' && !strncmp(records[0].text, "UniXNU ramoops log v3 run=0x", 28));
    assert(find(n, 'B', "zones console=0x880000000+0x40000 pmsg=0x880040000+0x200000 copies 4+32 x 0xfffc"));
    assert(find(n, 'B', "areas boot 0x40+0xfc0 events 0x1000+0x8000 console 0x9000+0x6ffc"));
    assert(find(n, 'B', "xnu ram=0x8c0000000+0x100000000"));
    assert(find(n, 'B', "reset-after=0s panic-reset=0s"));
    assert(find(n, 'A', "-v serial=0 pagani-ramoops=1"));
    assert(find(n, 'S', "S1: XNU entry, MMU off") && find(n, 'S', "pid=1 launchd exec"));
    assert(find(n, 'C', "IOGPU: hello") && !strcmp(find(n, 'C', "IOGPU: hello")->text, "IOGPU: hello"));
    assert(!strcmp(find(n, 'C', "indented")->text, " indented ? byte"));
    assert(!find(n, 'C', "second line"));            // still in the line buffer
    assert(!strcmp(find(n, 'R', "stage7")->text, "stage7 value=0xe00002bc: GPU board inputs"));
    assert(!strcmp(find(n, 'G', "1.0/4")->text, "1.0/4 GPU S3 GMU fail 0x1a reg 1a8f8 v 0 e 1"));
    assert(find(n, 'G', "1.3/4 row three") && find(n, 'G', "2.1/2 row one") && !find(n, 'G', "3.") && !find(n, 'D', ""));
    assert(find(n, 'E', "kernel exception ESR 0x96000010 ELR 0xfffffff00bfd9e58 FAR 0xfffffff852055000"));
    assert(find(n, 'U', "about to write CP_INIT") && find(n, 'U', "gmu reg 0x1a8f8 0x1"));
    // A partial line goes out at the first tick a second after its last character, then continues in '+' records.
    clock_now += 2 * HZ;
    ramoops_log_tick(7, 8, 9, 10);
    for (const char *p = " and its end\n"; *p; p++) ramoops_log_putc(*p);
    n = read_all();
    const struct rec *partial = find(n, 'C', "second line without newline");
    assert(partial && partial[1].kind == 'H' &&
           !strcmp(partial[1].text, "up 2 s polls 7 timer 8 free 9 avail 10 drops 0 cpu 4320MHz on 3"));
    assert(partial[2].kind == '+' && !strcmp(partial[2].text, " and its end"));
    // Timestamps: milliseconds since the log started.
    assert(partial[2].ms >= 2000 && partial[2].ms < 2100 && records[0].ms == 0);
}

static void test_long_lines(void)
{
    fresh();
    arg("pagani-ramoops", 3);
    assert(ramoops_log_vm_ready());
    char line[451];
    for (unsigned i = 0; i < 450; i++) line[i] = (char)('a' + i % 26);
    line[450] = 0;
    for (unsigned i = 0; i < 450; i++) ramoops_log_putc(line[i]);
    ramoops_log_putc('\n');
    ramoops_log_putc('\n');                          // empty lines are not logged
    ramoops_log_line('P', "first\nsecond\n");        // multi-line text: one record per line, no empty tail
    char big[700];
    memset(big, 'z', sizeof(big) - 1); big[sizeof(big) - 1] = 0;
    ramoops_log_line('S', big);
    unsigned n = read_all();
    const struct rec *c = find(n, 'C', "abcdef");
    assert(c && strlen(c[0].text) == 200 && c[1].kind == '+' && strlen(c[1].text) == 200 && c[2].kind == '+' &&
           strlen(c[2].text) == 50 && c[3].kind == 'P' && !strcmp(c[3].text, "first") && !strcmp(c[4].text, "second"));
    char joined[451];
    snprintf(joined, sizeof(joined), "%s%s%s", c[0].text, c[1].text, c[2].text);
    assert(!strcmp(joined, line));
    assert(c[5].kind == 'S' && c[6].kind == '+' && c[7].kind == '+' && c[8].kind == '+' && strlen(c[8].text) == 99);
    // Display probe rows with bit 1: logged, then at most every 10 s.
    static const char dpu[PAGANI_DPU_ROWS][PAGANI_GPU_ROW_BYTES] = {"TE 120.00Hz", "CTL0", "SSPP", "MDP", "FLIP 3"};
    static const char dpu2[PAGANI_DPU_ROWS][PAGANI_GPU_ROW_BYTES] = {"TE 119.98Hz", "CTL0", "SSPP", "MDP", "FLIP 4"};
    ramoops_log_rows('D', dpu, 9);                    // clamped to five rows
    ramoops_log_rows('D', dpu2, 5);                   // changed within 10 s: skipped
    clock_now += 11 * (uint64_t)HZ;
    ramoops_log_rows('D', dpu2, 5);
    ramoops_log_rows('G', NULL, 3);                  // no rows
    n = read_all();
    assert(find(n, 'D', "1.4/5 FLIP 3") && find(n, 'D', "2.0/5 TE 119.98Hz") && !find(n, 'D', "3."));
    assert(find(n, 'G', "1.0/0 (no rows)"));
}

// The frame-rate records (pagani-fps, BootProgress FrameRate.h): F lines go to the console ring (minutes of them leave the
// events ring alone), J records to the events ring, dated in the log's own milliseconds; both strict records with their CRC.
static void test_frame_rate(void)
{
    fresh();
    arg("pagani-ramoops", 1);
    assert(ramoops_log_vm_ready());
    ramoops_log_line('S', "GPU stage before the frames");
    clock_now += 100 * (uint64_t)HZ;                 // the log has run 100 s
    // 88.449 ms on the GPU path, 10 ms ago, with the late frame's breakdown (BootProgress FrameRate.h frame_jitter_detail)
    ramoops_log_jitter(88449, 1, 10000, 0, "gap 70.1 off 0.2 take 0.3 copy 17.0 done 0.1 fb0 to0");
    ramoops_log_jitter(51000, 0, 200000000, 3, NULL);    // dated before the log started: at 0; three left out before it; no breakdown
    ramoops_log_jitter(62000, 1, 0, 1, "gap -\noff 1.0 fb2 to0");  // a newline in the breakdown stays on the record's line
    const char *slow = " | SLOW 88.0ms gap 70.1 off 0.2 take 0.3 copy 17.0 done 0.1 fb1 to0 | ref 1 fail 0 early 0";
    for (unsigned i = 0; i < 400; i++)
        ramoops_log_fps("FPS 58 (31-60) | ft 17.2 p99 41.5 max 88.0ms | gpu 45 cpu 13", i % 3, i == 399 ? 7 : 0, i == 398 ? slow : NULL);
    unsigned found[3];
    unsigned n = read_log(found);
    assert(found[1] == 4 && found[2] > 200 && found[2] < 300);     // events: S J J J; the console ring wrapped (~108-byte lines)
    assert(find(n, 'S', "GPU stage before the frames"));
    assert(!strcmp(find(n, 'J', "88.4ms")->text, "jitter 88.4ms gpu at 99990ms gap 70.1 off 0.2 take 0.3 copy 17.0 done 0.1 fb0 to0"));
    assert(!strcmp(find(n, 'J', "51.0ms")->text, "jitter 51.0ms cpu at 0ms skipped 3"));
    assert(!strcmp(find(n, 'J', "62.0ms")->text, "jitter 62.0ms gpu at 100000ms gap - off 1.0 fb2 to0 skipped 1"));
    assert(records[n - 1].kind == 'F' &&
           !strcmp(records[n - 1].text, "FPS 58 (31-60) | ft 17.2 p99 41.5 max 88.0ms | gpu 45 cpu 13 | jit 0 lost 7"));
    assert(!strcmp(records[n - 2].text, "FPS 58 (31-60) | ft 17.2 p99 41.5 max 88.0ms | gpu 45 cpu 13 | jit 2 | SLOW 88.0ms gap 70.1 "
                                        "off 0.2 take 0.3 copy 17.0 done 0.1 fb1 to0 | ref 1 fail 0 early 0"));
    assert(!strcmp(records[n - 3].text, "FPS 58 (31-60) | ft 17.2 p99 41.5 max 88.0ms | gpu 45 cpu 13 | jit 1"));
    ramoops_log_suspend();
    ramoops_log_fps("off", 1, 1, slow);              // the log off: nothing
    ramoops_log_jitter(60000, 1, 0, 0, "gap 1.0");
    assert(read_log(NULL) == n);
}

// The heartbeat's CPU clock: measured only for a heartbeat (every 10 s, outside the log lock), "on ?" when the observer changed CPUs.
static void test_heartbeat_cpu(void)
{
    fresh();
    arg("pagani-ramoops", 1);
    assert(ramoops_log_vm_ready());
    cpu_mhz_calls = 0;
    ramoops_log_tick(1, 2, 3, 4);                    // the first tick is a heartbeat
    clock_now += 5 * (uint64_t)HZ;
    ramoops_log_tick(2, 2, 3, 4);                    // 5 s later: none, and no measurement
    assert(cpu_mhz_calls == 1);
    cpu_mhz_value = 1094; cpu_number_value = -1;     // throttled, and the thread moved meanwhile
    clock_now += 6 * (uint64_t)HZ;
    ramoops_log_tick(3, 2, 3, 4);
    assert(cpu_mhz_calls == 2);
    unsigned n = read_all();
    assert(find(n, 'H', "polls 1 timer 2 free 3 avail 4 drops 0 cpu 4320MHz on 3"));
    assert(find(n, 'H', "polls 3 timer 2 free 3 avail 4 drops 0 cpu 1094MHz on ?") && !find(n, 'H', "polls 2 "));
    cpu_mhz_value = 4320; cpu_number_value = 3;
}

// CpuClock.h itself: adds per microsecond from adds and counter ticks (rounded), the best of CPU_CLOCK_WINDOWS windows of at least
// 1/CPU_CLOCK_PER_SECOND s, a window ended by CPU_CLOCK_CHUNKS_MAX when the counter stands still, and on an arm64 host the real thing.
static uint64_t fake_now, fake_step, fake_spike;
static unsigned fake_reads, fake_spikes;
static uint64_t fake_counter(void)
{
    fake_reads++;
    fake_now += fake_step;
    if (fake_spikes && fake_reads % 2 == 0) { fake_spikes--; fake_now += fake_spike; }   // an interrupt inside the window
    return fake_now;
}
#if defined(__aarch64__)
static uint64_t host_counter(void)
{
    uint64_t value;
    __asm__ volatile("isb\n\tmrs %0, cntvct_el0" : "=r"(value) :: "memory");
    return value;
}
static uint64_t host_frequency(void)
{
    uint64_t value;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(value));
    return value;
}
#endif
static void test_cpu_clock(void)
{
    assert(cpu_clock_rate(4096, 0, HZ) == 0 && cpu_clock_rate(4096, 10, 0) == 0);
    assert(cpu_clock_rate(4320ULL * 1000, HZ / 1000, HZ) == 4320);           // 1 ms of adds at 4320 MHz over 1 ms of ticks
    assert(cpu_clock_rate(4320ULL * 125, HZ / CPU_CLOCK_PER_SECOND, HZ) == 4320);    // one 125 us window
    assert(cpu_clock_rate(3532ULL * 125 + 62, 2400, HZ) == 3532 && cpu_clock_rate(3532ULL * 125 + 63, 2400, HZ) == 3533);
    assert(cpu_clock_rate(1, HZ, HZ) == 0 && cpu_clock_rate(500000, HZ, HZ) == 1);   // to the nearest MHz
    assert(cpu_clock_rate(CPU_CLOCK_CHUNKS_MAX * (uint64_t)CPU_CLOCK_ADDS, 1, 24000000) == 100663296);  // no overflow at the limits
    // One chunk (4096 adds) per 125 us window: 32.768 MHz.
    fake_now = 0; fake_step = HZ / CPU_CLOCK_PER_SECOND; fake_reads = 0; fake_spikes = 0;
    assert(cpu_clock_measure(fake_counter, HZ) == 33 && fake_reads == 2 * CPU_CLOCK_WINDOWS);
    // Smaller steps: 3 chunks per window (2400 ticks reached at the third read after the start).
    fake_step = 800; fake_reads = 0;
    assert(cpu_clock_measure(fake_counter, HZ) == 98 && fake_reads == 4 * CPU_CLOCK_WINDOWS);
    // Interrupts spoil all but one window: the best window counts.
    fake_step = HZ / CPU_CLOCK_PER_SECOND; fake_spike = 10 * fake_step; fake_spikes = CPU_CLOCK_WINDOWS - 1; fake_reads = 0;
    assert(cpu_clock_measure(fake_counter, HZ) == 33 && !fake_spikes);
    fake_spikes = CPU_CLOCK_WINDOWS; fake_reads = 0;                          // every window: 4096 adds over 11 x 125 us
    assert(cpu_clock_measure(fake_counter, HZ) == 3);
    // A counter that stands still: each window ends after CPU_CLOCK_CHUNKS_MAX chunks without time; no rate.
    fake_step = 0; fake_spikes = 0; fake_reads = 0;
    assert(cpu_clock_measure(fake_counter, HZ) == 0 && fake_reads == CPU_CLOCK_WINDOWS * (1 + CPU_CLOCK_CHUNKS_MAX));
    assert(cpu_clock_measure(fake_counter, 0) == 0 && cpu_clock_measure(NULL, HZ) == 0);
#if defined(__aarch64__)
    // The real counter and this host's CPU: a dependent add a cycle, so a plausible clock (best of eight 125 us windows).
    const uint32_t mhz = cpu_clock_measure(host_counter, host_frequency());
    printf("host CPU clock by dependent adds: %u MHz\n", mhz);
    assert(mhz > 100 && mhz < 20000);
#endif
}

static void test_wrap(void)
{
    fresh();
    arg("pagani-ramoops", 1);
    assert(ramoops_log_vm_ready());
    ramoops_log_line('S', "GPU stage before the flood");
    char line[128];
    unsigned found[3];
    for (unsigned i = 0; i < 4000; i++) {
        int length = snprintf(line, sizeof(line), "console line %u %.*s\n", i, (int)(i % 90), "................................................"
                              "...........................................................");
        for (int k = 0; k < length; k++) ramoops_log_putc(line[k]);
    }
    // The console ring wrapped and keeps the newest lines; the events ring and the boot area are untouched by it.
    unsigned n = read_log(found);
    assert(found[0] == 6 && found[1] == 1 && found[2] > 250 && found[2] < 400);   // ~96-byte records in 0x6ffc bytes
    assert(find(n, 'S', "GPU stage before the flood") && records[6].seq == 7);
    assert(!strncmp(records[n - 1].text, "console line 3999 ", 18) && records[n - 1].seq == 4007);
    for (unsigned i = 8; i < n; i++) assert(records[i].seq == records[i - 1].seq + 1);   // the newest lines, no gap
    assert(records[7].seq > 8);                                                          // the oldest lines are gone
    // A flood of status rows wraps the console ring they share; the events ring keeps its stage record (the GPU rows went to
    // the events ring until 2026-10-31 and pushed the boot's one-time U lines out), the boot area its log header.
    static char rows[PAGANI_GPU_ROWS][PAGANI_GPU_ROW_BYTES];
    for (unsigned i = 0; i < 400; i++) {
        for (unsigned r = 0; r < 4; r++) snprintf(rows[r], PAGANI_GPU_ROW_BYTES, "GPU RUN %u MHz busy %u row %u", 300 + i, i % 100, r);
        ramoops_log_rows('G', (const char (*)[PAGANI_GPU_ROW_BYTES])rows, 4);
    }
    n = read_log(found);
    assert(found[0] == 6 && find(n, 'B', "UniXNU ramoops log v3") && find(n, 'S', "GPU stage before the flood"));
    assert(!find(n, 'C', "console line 3999 "));                                         // the console lines are gone
    assert(find(n, 'G', "400.3/4 GPU RUN 699 MHz busy 99 row 3") && !find(n, 'G', "GPU RUN 300 MHz busy 0 row 0"));
    // A full boot area sends further log-header records to the events ring.
    for (unsigned i = 0; i < 40; i++) ramoops_log_line('B', "boot area filler: a log header record that keeps the boot area busy....");
    n = read_log(found);
    assert(found[0] < 6 + 40 && find(n, 'B', "boot area filler") && records[n - 1].kind == 'B');
    unsigned fillers = 0;
    for (unsigned i = 0; i < n; i++) fillers += records[i].kind == 'B' && strstr(records[i].text, "filler") != NULL;
    assert(fillers == 40);
}

static void test_guards(void)
{
    fresh();
    arg("pagani-ramoops", 1);
    ram_base = 0x880000000ULL - 0x1000; ram_bytes = 0x2000;      // XNU RAM covers the region's first page
    assert(!ramoops_log_vm_ready() && maps == 0 && !boot_progress_trace_enabled());
    assert(!strcmp(ramoops_log_status(), "ramoops refused: region overlaps XNU RAM 0x87ffff000-0x880001000"));
    fresh();
    arg("pagani-ramoops", 1); arg("pagani-ramoops-pa", 0x880000800ULL);
    assert(!ramoops_log_vm_ready() && maps == 0 && !strncmp(ramoops_log_status(), "ramoops refused: bad region", 27));
    fresh();
    arg("pagani-ramoops", 1); arg("pagani-ramoops-console", 0x2000); arg("pagani-ramoops-pmsg", 0x2000);
    assert(!ramoops_log_vm_ready() && maps == 0 && !strncmp(ramoops_log_status(), "ramoops refused: zones too small", 32));
    fresh();
    // A smaller console zone: rounded down to a power of two like Linux does; the copies shrink to 0xfff8 so that two
    // still tile it (two of 0xfffc would not fit beside the zone header), 32 in pmsg.
    arg("pagani-ramoops", 1); arg("pagani-ramoops-console", 0x30000); arg("pagani-reset-after", 90);
    assert(ramoops_log_vm_ready() && mapped_bytes == 0x20000 + 0x200000 && zone_size[0] == 0x20000);
    assert(copy_bytes == 0xfff8 && zone_copies[0] == 2 && zone_copies[1] == 32 && copy_count == 34);
    assert(areas[AREA_EVENTS].bytes == 0x8000 && areas[AREA_CONSOLE].bytes == 0xfff8 - 0x9000);
    assert(!strcmp(ramoops_log_status(), "ramoops on 0x880000000 copies 34 x 0xfff8; reset after 90 s"));
    ramoops_log_line('S', "small copies");
    assert(find(read_all(), 'S', "small copies"));
    fresh();
    // A 32 KiB console zone: one copy of its size; pmsg would take 64 of them, so it keeps 63 (at most 64 in all), and the
    // events ring takes half of what follows the boot area.
    arg("pagani-ramoops", 1); arg("pagani-ramoops-console", 0x8000);
    assert(ramoops_log_vm_ready() && copy_bytes == 0x7ff4 && zone_copies[0] == 1 && zone_copies[1] == 63 && copy_count == 64);
    assert(areas[AREA_EVENTS].bytes == 0x37f8 && areas[AREA_CONSOLE].offset == 0x1000 + 0x37f8 &&
           areas[AREA_CONSOLE].bytes == 0x37fc);
    ramoops_log_line('S', "copy limit");
    assert(find(read_all(), 'S', "copy limit"));
    fresh();
    // Two 4 MiB zones would hold 64 copies each: 32 in each zone.
    arg("pagani-ramoops", 1); arg("pagani-ramoops-console", 0x400000); arg("pagani-ramoops-pmsg", 0x400000);
    assert(ramoops_log_vm_ready() && mapped_bytes == 0x800000 && copy_bytes == 0xfffc);
    assert(zone_copies[0] == 32 && zone_copies[1] == 32 && copy_count == 64);
    ramoops_log_line('S', "two large zones");
    assert(find(read_all(), 'S', "two large zones"));
    fresh();
    // pagani-ramoops-copy=0x8000: eight copies of 0x7ffc in the console zone, 56 in pmsg (64 in all), shorter rings.
    arg("pagani-ramoops", 1); arg("pagani-ramoops-copy", 0x8000);
    assert(ramoops_log_vm_ready() && copy_bytes == 0x7ffc && zone_copies[0] == 8 && zone_copies[1] == 56 && copy_count == 64);
    assert(areas[AREA_EVENTS].bytes == 0x37fc && areas[AREA_CONSOLE].offset == 0x1000 + 0x37fc &&
           areas[AREA_CONSOLE].bytes == 0x3800);
    assert(!strcmp(ramoops_log_status(), "ramoops on 0x880000000 copies 64 x 0x7ffc"));
    ramoops_log_line('S', "copy limit 0x8000");
    assert(find(read_all(), 'S', "copy limit 0x8000"));
    fresh();
    // A limit beyond the zones: one copy fills the console zone (0x3fff4), eight more pmsg, as layout 2 did.
    arg("pagani-ramoops", 1); arg("pagani-ramoops-copy", 0x100000);
    assert(ramoops_log_vm_ready() && copy_bytes == 0x3fff4 && zone_copies[0] == 1 && zone_copies[1] == 8);
    assert(areas[AREA_EVENTS].bytes == 0x8000 && areas[AREA_CONSOLE].bytes == 0x3fff4 - 0x9000);
    fresh();
    arg("pagani-ramoops", 1); arg("pagani-ramoops-copy", 0x3000);        // below 16 KiB: refused
    assert(!ramoops_log_vm_ready() && maps == 0 && !strncmp(ramoops_log_status(), "ramoops refused: zones too small", 32));
    fresh();
    arg("pagani-reset-after", 30); arg("pagani-panic-reset", 12);
    assert(!ramoops_log_vm_ready() && !strcmp(ramoops_log_status(), "ramoops off; reset after 30 s; panic reset 12 s"));
    fresh();
    arg("pagani-ramoops", 5);                        // bit 2: the Device-nGnRE mapping
    assert(ramoops_log_vm_ready() && mapped_device && strstr(ramoops_log_status(), " device"));
}

static void expect_call(unsigned i, int hvc, uint64_t function, uint64_t a)
{
    assert(i < call_count && calls[i].hvc == hvc && calls[i].function == function && calls[i].a == a &&
           calls[i].b == 0 && calls[i].c == 0);
}

// The Linux headers stamped again: a size word overwritten during the run (18807764 in the 13T phone-layout runs of 2026-10-30,
// which made Linux drop both zones) is right again after the next heartbeat, and before a reset even without a heartbeat since.
static void test_header_stamp(void)
{
    const uint32_t bad = 18807764u;
    fresh();
    arg("pagani-ramoops", 1); arg("pagani-reset-after", 60);
    assert(ramoops_log_vm_ready());
    memcpy(zone_bytes(0) + 8, &bad, 4);
    memcpy(zone_bytes(1) + 8, &bad, 4);
    assert(!header_valid(zone_bytes(0), zone_size[0]) && !header_valid(zone_bytes(1), zone_size[1]));
    ramoops_log_tick(1, 2, 3, 4);                    // the first tick is a heartbeat
    assert(header_valid(zone_bytes(0), zone_size[0]) && header_valid(zone_bytes(1), zone_size[1]));
    unsigned n = read_all();
    assert(find(n, 'H', "polls 1 timer 2"));
    clock_now += 3 * (uint64_t)HZ;
    ramoops_log_start();
    memcpy(zone_bytes(0) + 8, &bad, 4);              // overwritten again after the heartbeat
    ramoops_log_timer_fired();
    assert(header_valid(zone_bytes(0), zone_size[0]) && header_valid(zone_bytes(1), zone_size[1]));
    n = read_all();
    assert(find(n, 'T', "reset reason=timer"));
    // The log off: nothing is mapped, nothing is stamped.
    fresh();
    assert(!ramoops_log_vm_ready());
    ramoops_log_tick(1, 2, 3, 4);
    assert(zone_base[0] == 0 && zone_base[1] == 0);
}

// The observer's late retry (the audit's G3): a reset that began 5 s ago while the observer still runs is sent again from the
// observer's CPU, once a second, with one record.
static void test_observer_retry(void)
{
    fresh();
    arg("pagani-ramoops", 1); arg("pagani-reset-after", 60);
    assert(ramoops_log_vm_ready());
    clock_now += 3 * (uint64_t)HZ;
    ramoops_log_start();
    ramoops_log_timer_fired();                       // both PSCI resets return (the fakes)
    const unsigned calls = call_count;
    clock_now += 4 * (uint64_t)HZ;
    ramoops_log_tick(1, 2, 3, 4);
    assert(call_count == calls);                     // 4 s: not yet
    clock_now += 2 * (uint64_t)HZ;
    ramoops_log_tick(2, 2, 3, 4);
    assert(call_count == calls + 1);
    expect_call(calls, 0, 0x84000009, 0);
    clock_now += (uint64_t)HZ;
    ramoops_log_tick(3, 2, 3, 4);
    assert(call_count == calls + 2);
    expect_call(calls + 1, 0, 0x84000009, 0);
    unsigned n = read_all(), lines = 0;
    for (unsigned i = 0; i < n; i++) lines += records[i].kind == 'T' && strstr(records[i].text, "still running 5 s after the reset began");
    assert(lines == 1);
}

// The second reset timer (the audit's G4): armed once, by the observer, the first time it runs on a CPU other than the first
// timer's, due 3 s after the deadline; it resets like the first timer, and after the first timer it goes straight to PSCI.
static void test_backup_timer(void)
{
    const char *armed_on_3 = "reset backup timer armed on cpu 3 in ";
    fresh();
    arg("pagani-ramoops", 1); arg("pagani-reset-after", 60);
    assert(ramoops_log_vm_ready());
    clock_now += 3 * (uint64_t)HZ;
    ramoops_log_start();
    assert(timer_arms == 1 && reset_timer_cpu == 0 && !backup_calls);
    // The observer on the boot CPU (where the first timer is): asked each pass, nothing armed.
    ramoops_log_tick(1, 2, 3, 4);
    clock_now += HZ;
    ramoops_log_tick(2, 2, 3, 4);
    assert(backup_calls == 2 && backup_avoided == 0 && !backup_arms && reset_backup_cpu == -1);
    // On cpu 3 (5 s into the log): armed there for the deadline + 3 s, i.e. 58 s later, with one record; never again.
    observer_cpu = 3;
    clock_now += HZ;
    ramoops_log_tick(3, 2, 3, 4);
    assert(backup_arms == 1 && backup_cpu_armed == 3 && reset_backup_cpu == 3 && backup_ms > 57900 && backup_ms <= 58000);
    observer_cpu = 5;
    clock_now += HZ;
    ramoops_log_tick(4, 2, 3, 4);
    assert(backup_arms == 1 && backup_calls == 3);
    unsigned n = read_all();
    const struct rec *armed = find(n, 'T', armed_on_3);
    assert(armed && strstr(armed->text, " ms (deadline + 3 s; the first timer is on cpu 0)"));
    char expected[96];
    snprintf(expected, sizeof(expected), "%s%llu ms", armed_on_3, (unsigned long long)backup_ms);
    assert(!strncmp(armed->text, expected, strlen(expected)));
    // Normally never due: before it, the observer fell back 2 s after the deadline. Here neither came: it fires and resets.
    unsigned calls = call_count;
    ramoops_log_backup_timer_fired();
    expect_call(calls, 0, 0xc4000012, 0);
    expect_call(calls + 1, 0, 0x84000009, 0);
    assert(call_count == calls + 2);
    n = read_all();
    const struct rec *reset = find(n, 'T', "reset reason=backup timer up=");
    assert(reset && !strcmp(reset[1].text, "SYSTEM_RESET2 returned -2; trying SYSTEM_RESET") &&
           !strcmp(reset[2].text, "SYSTEM_RESET returned -1; retrying once a second, watchdog armed"));
    // A late first timer after it: straight to PSCI, no more records (reset_started).
    ramoops_log_timer_fired();
    assert(call_count == calls + 4 && read_all() == n);

    // The first timer fired (its PSCI never returned, say): the backup timer still fires on its CPU, straight to PSCI.
    fresh();
    arg("pagani-ramoops", 1); arg("pagani-reset-after", 60);
    assert(ramoops_log_vm_ready());
    ramoops_log_start();
    observer_cpu = 2;
    ramoops_log_tick(1, 2, 3, 4);
    assert(backup_arms == 1 && backup_cpu_armed == 2);
    ramoops_log_timer_fired();
    n = read_all();
    assert(find(n, 'T', "reset reason=timer up="));
    calls = call_count;
    ramoops_log_backup_timer_fired();
    assert(call_count == calls + 2 && read_all() == n);
    expect_call(calls + 1, 0, 0x84000009, 0);

    // A reset already under way (the first timer fired before the observer ever left the boot CPU): no second timer.
    fresh();
    arg("pagani-ramoops", 1); arg("pagani-reset-after", 60);
    assert(ramoops_log_vm_ready());
    ramoops_log_start();
    ramoops_log_timer_fired();
    observer_cpu = 4;
    ramoops_log_tick(1, 2, 3, 4);
    assert(!backup_calls && !backup_arms);

    // The observer leaves the boot CPU 1 s after the deadline (the first timer late): due 2 s later. One more second: the
    // observer's own fallback (deadline + 2 s) resets first, the backup timer stays armed behind it.
    fresh();
    arg("pagani-ramoops", 1); arg("pagani-reset-after", 10);
    assert(ramoops_log_vm_ready());
    ramoops_log_start();
    clock_now += 11 * (uint64_t)HZ;
    observer_cpu = 6;
    ramoops_log_tick(1, 2, 3, 4);
    assert(backup_arms == 1 && backup_ms > 1990 && backup_ms <= 2000 && call_count == 2);
    clock_now += (uint64_t)HZ + HZ / 10;
    ramoops_log_tick(2, 2, 3, 4);
    n = read_all();
    assert(find(n, 'T', "reset reason=observer") && backup_arms == 1);

    // A first timer on another CPU than 0: the second one goes anywhere else, cpu 0 included.
    fresh();
    timer_cpu_value = 2;
    arg("pagani-ramoops", 1); arg("pagani-reset-after", 60);
    assert(ramoops_log_vm_ready());
    ramoops_log_start();
    observer_cpu = 2;
    ramoops_log_tick(1, 2, 3, 4);
    assert(backup_avoided == 2 && !backup_arms);
    observer_cpu = 0;
    clock_now += HZ;
    ramoops_log_tick(2, 2, 3, 4);
    assert(backup_arms == 1 && backup_cpu_armed == 0);
    assert(find(read_all(), 'T', "reset backup timer armed on cpu 0 in "));
    assert(find(read_all(), 'T', "(deadline + 3 s; the first timer is on cpu 2)"));

    // The reset configured without the log: armed all the same, nothing recorded (nothing is mapped).
    fresh();
    arg("pagani-reset-after", 30);
    assert(!ramoops_log_vm_ready());
    ramoops_log_start();
    observer_cpu = 1;
    ramoops_log_tick(1, 2, 3, 4);
    assert(timer_arms == 1 && backup_arms == 1 && backup_cpu_armed == 1 && maps == 0);

    // No pagani-reset-after (the log alone, or a panic reset only): never asked, whatever CPU the observer is on.
    for (int panic_reset = 0; panic_reset < 2; panic_reset++) {
        fresh();
        arg("pagani-ramoops", 1);
        if (panic_reset) arg("pagani-panic-reset", 5);
        assert(ramoops_log_vm_ready());
        ramoops_log_start();
        for (int cpu = 0; cpu < 8; cpu++) {
            observer_cpu = cpu;
            clock_now += HZ;
            ramoops_log_tick((uint64_t)cpu, 2, 3, 4);
        }
        assert(!timer_arms && !backup_calls && !find(read_all(), 'T', "backup"));
    }
}

static void test_reset_paths(void)
{
    // PSCI 1.1 advertises SYSTEM_RESET2: the warm reset first, SYSTEM_RESET only if it returns. The path is logged before.
    fresh();
    arg("pagani-ramoops", 1); arg("pagani-reset-after", 60);
    assert(ramoops_log_vm_ready());
    clock_now += 3 * (uint64_t)HZ;
    ramoops_log_start();
    expect_call(0, 0, 0x84000000, 0);
    expect_call(1, 0, 0x8400000a, 0xc4000012);
    assert(call_count == 2 && timer_arms == 1 && timer_ms > 56900 && timer_ms <= 57000);
    ramoops_log_start();                             // armed once
    assert(timer_arms == 1 && call_count == 2);
    ramoops_log_putc('h'); ramoops_log_putc('i');    // a partial line is flushed before the reset
    ramoops_log_timer_fired();
    expect_call(2, 0, 0xc4000012, 0);
    expect_call(3, 0, 0x84000009, 0);
    unsigned n = read_all();
    assert(find(n, 'T', "psci=1.1 conduit=smc(/psci) SYSTEM_RESET2 features=0x0 path=SYSTEM_RESET2 warm (0xc4000012 type 0)"));
    assert(find(n, 'T', "reset armed in 5"));
    const struct rec *partial = find(n, 'C', "hi");
    assert(partial && partial[1].kind == 'T' && !strncmp(partial[1].text, "reset reason=timer up=", 22));
    assert(strstr(partial[1].text, "path=SYSTEM_RESET2 warm"));
    assert(!strcmp(partial[2].text, "SYSTEM_RESET2 returned -2; trying SYSTEM_RESET"));
    assert(!strcmp(partial[3].text, "SYSTEM_RESET returned -1; retrying once a second, watchdog armed") && partial[3].seq == n);
    ramoops_log_timer_fired();                       // a second trigger goes straight to PSCI, no more records
    assert(call_count == 6 && read_all() == n);

    // PSCI 1.0 without SYSTEM_RESET2: SYSTEM_RESET. The observer resets when the timer did not fire.
    fresh();
    arg("pagani-ramoops", 1); arg("pagani-reset-after", 10);
    psci_version_value = 0x10000; reset2_features_value = -1;
    assert(ramoops_log_vm_ready());
    ramoops_log_start();
    assert(call_count == 2 && timer_arms == 1);
    clock_now += 11 * (uint64_t)HZ;
    ramoops_log_tick(1, 1, 1, 1);                    // 11 s: inside the 2 s grace, the timer may still fire
    assert(call_count == 2);
    clock_now += 2 * (uint64_t)HZ;
    ramoops_log_tick(1, 1, 1, 1);
    expect_call(2, 0, 0x84000009, 0);
    assert(call_count == 3);
    n = read_all();
    assert(find(n, 'T', "psci=1.0 conduit=smc(/psci) SYSTEM_RESET2 features=-1 path=SYSTEM_RESET (0x84000009)"));
    assert(find(n, 'T', "reset reason=observer"));
    ramoops_log_tick(1, 1, 1, 1);
    assert(call_count == 3);

    // PSCI 0.2 has no PSCI_FEATURES; an hvc conduit from /psci; reset configured without the log.
    fresh();
    arg("pagani-reset-after", 5);
    psci_version_value = 0x2;
    memcpy(method_value, "hvc", 4);
    assert(!ramoops_log_vm_ready());
    ramoops_log_start();
    assert(call_count == 1 && calls[0].hvc == 1 && timer_arms == 1);
    ramoops_log_timer_fired();
    expect_call(1, 1, 0x84000009, 0);

    // pagani-reset-cold=1: SYSTEM_RESET although SYSTEM_RESET2 is advertised.
    fresh();
    arg("pagani-ramoops", 1); arg("pagani-reset-after", 5); arg("pagani-reset-cold", 1);
    assert(ramoops_log_vm_ready());
    ramoops_log_start();
    ramoops_log_timer_fired();
    expect_call(2, 0, 0x84000009, 0);
    assert(call_count == 3);
    n = read_all();
    assert(find(n, 'T', "features=0x0 path=SYSTEM_RESET (0x84000009) (pagani-reset-cold)"));

    // No /psci method: SMC, as ArmDTPlatform's stage reset; the 32-bit result of PSCI_FEATURES counts, not x0's upper half.
    fresh();
    arg("pagani-ramoops", 1); arg("pagani-panic-reset", 1);
    method_present = 0;
    assert(ramoops_log_vm_ready());
    ramoops_log_start();
    assert(call_count == 2 && timer_arms == 0 && calls[0].hvc == 0);
    n = read_all();
    assert(find(n, 'T', "conduit=smc(default)"));
}

static void test_panic(void)
{
    // pagani-panic-reset: the footer names the delay, the screen text goes into the log line by line, then the reset.
    fresh();
    arg("pagani-ramoops", 1); arg("pagani-panic-reset", 7);
    assert(ramoops_log_vm_ready());
    assert(!strcmp(ramoops_log_panic_footer(), "RESET IN 7 s; log kept in ramoops"));
    for (const char *p = "OnePlus 13T / XNU PANIC\nMESSAGE\nKernel data abort."; *p; p++) ramoops_log_panic_putc(*p);
    clock_step = 1000000;
    uint64_t before = clock_now;
    ramoops_log_panic_reset();
    assert(clock_now - before >= 7ULL * HZ);
    expect_call(2, 0, 0xc4000012, 0);
    unsigned n = read_all();
    const struct rec *p = find(n, 'P', "OnePlus 13T / XNU PANIC");
    assert(p && !strcmp(p[1].text, "MESSAGE") && !strcmp(p[2].text, "Kernel data abort.") && p[2].kind == 'P');
    assert(p[3].kind == 'T' && !strcmp(p[3].text, "panic: reset in 7000 ms"));
    assert(p[4].kind == 'T' && !strncmp(p[4].text, "reset reason=panic", 18));

    // No pagani-panic-reset: a panic waits for the pagani-reset-after deadline (at least 5 s).
    fresh();
    arg("pagani-ramoops", 1); arg("pagani-reset-after", 100);
    assert(ramoops_log_vm_ready());
    clock_now += 98 * (uint64_t)HZ;
    assert(!strcmp(ramoops_log_panic_footer(), "RESET IN 5 s; log kept in ramoops"));
    clock_step = 1000000;
    before = clock_now;
    ramoops_log_panic_reset();
    assert(clock_now - before >= 5ULL * HZ && clock_now - before < 6ULL * HZ && call_count == 4);

    // Neither: the panic screen halts as before (no footer, no PSCI).
    fresh();
    arg("pagani-ramoops", 1);
    assert(ramoops_log_vm_ready());
    assert(ramoops_log_panic_footer() == NULL);
    ramoops_log_panic_reset();
    assert(call_count == 0);
}

static void headless(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    ramoops_log_panic_headless(format, &args, 0xfffffff007123456, 9, 0x140);
    va_end(args);
}

static void test_headless(void)
{
    fresh();
    arg("pagani-ramoops", 1); arg("pagani-panic-reset", 2);
    assert(ramoops_log_vm_ready());
    clock_step = 1000000;
    headless("Failed to mount %s: %u\nsecond line", "root device", 60U);
    unsigned n = read_all();
    assert(find(n, 'P', "PANIC without a mapped screen caller 0xfffffff007123456 reason 0x9 options 0x140"));
    const struct rec *m = find(n, 'P', "Failed to mount root device: 60");
    assert(m && !strcmp(m[1].text, "second line") && m[2].kind == 'T');
    assert(call_count == 4);                         // VERSION, FEATURES, SYSTEM_RESET2 (fails here), SYSTEM_RESET
    expect_call(2, 0, 0xc4000012, 0);
    expect_call(3, 0, 0x84000009, 0);
    headless("second panic on another CPU");
    assert(call_count == 4 && !find(read_all(), 'P', "second panic"));
}

static void test_contention(void)
{
    fresh();
    arg("pagani-ramoops", 1);
    assert(ramoops_log_vm_ready());
    lock_word = 1;                                   // a CPU stopped inside the lock
    ramoops_log_putc('x');
    assert(drops == 1 && lock_suspect);              // the next writers spin briefly
    ramoops_log_line('S', "dropped");
    assert(drops == 2);
    ramoops_log_panic_putc('P');                     // the panic path writes anyway
    ramoops_log_panic_putc('\n');
    assert(lock_word == 0 && !lock_suspect);
    clock_now += 20 * (uint64_t)HZ;
    ramoops_log_tick(1, 2, 3, 4);
    unsigned n = read_all();
    assert(!find(n, 'S', "dropped") && find(n, 'P', "P") && find(n, 'H', "drops 2"));
    assert(find(n, 'W', "dropped 2 writes: the log lock was held too long"));
}

// ---- pagani-reset-seq -------------------------------------------------------------------------------------------------------
enum { FN_IO_READ = 0xc2000501, FN_IO_WRITE = 0xc2000502, FN_SDI = 0xc2000109, FN_ARB = 0xc2000901, FN_OEM = 0xc3000922,
       FN_AVAIL = 0x42000601, FN_VERSION = 0x84000000, FN_FEATURES = 0x8400000a, FN_RESET2 = 0xc4000012, FN_RESET = 0x84000009,
       FN_OFF = 0x84000008 };

static void expect_trace(unsigned from, const uint64_t *ids, unsigned n)
{
    assert(from + n <= trace_count);
    for (unsigned i = 0; i < n; i++) {
        if (trace[from + i] == ids[i]) continue;
        fprintf(stderr, "trace[%u] = %#llx, expected %#llx\n", from + i, (unsigned long long)trace[from + i],
                (unsigned long long)ids[i]);
        assert(0);
    }
}

static unsigned trace_how_many(uint64_t id)
{
    unsigned n = 0;
    for (unsigned i = 0; i < trace_count; i++) n += trace[i] == id;
    return n;
}

// The boot-args the way the kernel command line carries the mask (taken only as written "0x" and hex digits).
static char seq_args_text[192];
static void seq_args(uint32_t mask)
{
    snprintf(seq_args_text, sizeof(seq_args_text), "-v serial=0 pagani-ramoops=1 pagani-reset-seq=%#x", mask);
    boot_args_text = seq_args_text;
}

static void seq_setup(uint32_t mask, unsigned reset_after)
{
    fresh();
    seq_expected = 1;
    arg("pagani-ramoops", 1);
    if (reset_after) arg("pagani-reset-after", reset_after);
    arg("pagani-reset-seq", mask);
    seq_args(mask);
    assert(ramoops_log_vm_ready());
}

// The record after `r` (by sequence) has this kind and exactly this text.
static const struct rec *next_is(const struct rec *r, char kind, const char *text)
{
    const struct rec *n = r + 1;
    if (n->kind != kind || strcmp(n->text, text)) {
        fprintf(stderr, "after \"%s\": %c \"%s\", expected %c \"%s\"\n", r->text, n->kind, n->text, kind, text);
        assert(0);
    }
    return n;
}

static const uint32_t probe_ids[6] = {0x2000109, 0x2000110, 0x2000501, 0x2000502, 0x2000901, 0x3000922};

// Every bit: the boot probe before the PSCI facts; at the timer the watchdog's hold first, then the steps in Linux's order between
// the "reset reason" record and the PSCI reset, one U record each with the function id and x0, the hold's release right before
// PSCI; a later trigger and the observer's late retry go straight to PSCI (the steps run once per boot).
static void test_reset_seq_all(void)
{
    seq_setup(0x11f, 60);
    park_signalled = 7; park_arrive = 7;
    for (unsigned i = 0; i < 6; i++) { avail_ids[i] = probe_ids[i]; avail_x1[i] = i == 5 ? 0 : 1; }
    assert(!strcmp(ramoops_log_status(), "ramoops on 0x880000000 copies 36 x 0xfffc; reset after 60 s; reset seq 0x11f"));
    clock_now += 3 * (uint64_t)HZ;
    ramoops_log_start();
    static const uint64_t boot[] = {FN_AVAIL, FN_AVAIL, FN_AVAIL, FN_AVAIL, FN_AVAIL, FN_AVAIL, FN_IO_READ, FN_VERSION, FN_FEATURES};
    expect_trace(0, boot, 9);
    assert(trace_count == 9 && smc_count == 7 && timer_arms == 1 && !hold_calls);
    for (unsigned i = 0; i < 6; i++) assert(smcs[i].x1 == 1 && smcs[i].x2 == probe_ids[i] && smcs[i].x3 == 0 && smcs[i].x6 == 0);
    assert(smcs[6].x1 == 1 && smcs[6].x2 == 0x1fd9000);
    assert(!strcmp(ramoops_reset_seq_status(), "reset seq 0x11f avail sdi 1 dl 1 rd 1 wr 1 spmi 1 oem 0 dload 0x20"));
    ramoops_log_start();                             // started twice: one probe
    assert(trace_count == 9);
    unsigned n = read_all();
    const struct rec *r = find(n, 'U', "reset seq probe: avail SCM 0x42000601(0x2000109) = 0 x1 0x1");
    assert(r);
    r = next_is(r, 'U', "reset seq probe: avail SCM 0x42000601(0x2000110) = 0 x1 0x1");
    r = next_is(r, 'U', "reset seq probe: avail SCM 0x42000601(0x2000501) = 0 x1 0x1");
    r = next_is(r, 'U', "reset seq probe: avail SCM 0x42000601(0x2000502) = 0 x1 0x1");
    r = next_is(r, 'U', "reset seq probe: avail SCM 0x42000601(0x2000901) = 0 x1 0x1");
    r = next_is(r, 'U', "reset seq probe: avail SCM 0x42000601(0x3000922) = 0 x1 0x0");
    r = next_is(r, 'U', "reset seq probe: dload read SCM 0xc2000501(0x1fd9000) = 0 x1 0x20");
    assert(find(n, 'B', "reset-after=60s panic-reset=0s reset-seq=0x11f"));
    ramoops_log_putc('h'); ramoops_log_putc('i');    // the partial line still goes out before the reset
    unsigned before = trace_count;
    ramoops_log_timer_fired();
    static const uint64_t steps[] = {TRACE_HOLD, FN_IO_READ, FN_IO_WRITE, FN_IO_READ, FN_SDI, FN_ARB, TRACE_PARK, FN_OEM,
                                     TRACE_RELEASE, FN_RESET2, FN_RESET};
    expect_trace(before, steps, 11);
    assert(trace_count == before + 11 && dload_value == 0 && sdi_calls == 1 && arbiter_calls == 1 && park_calls == 1);
    assert(hold_calls == 1 && release_calls == 1);
    assert(smcs[8].x1 == 2 && smcs[8].x2 == 0x1fd9000 && smcs[8].x3 == 0);   // IO_WRITE(0x1fd9000, 0)
    n = read_all();
    r = find(n, 'C', "hi");
    assert(r && r[1].kind == 'T' && !strncmp(r[1].text, "reset reason=timer up=", 22));
    r = next_is(r + 1, 'T', "reset seq 0x11f (pagani-reset-seq) reason=timer: probe dload-clear sdi-off spmi-halt park-cpus "
                            "oem-warm-reboot");
    r = next_is(r, 'U', "reset seq: dload read SCM 0xc2000501(0x1fd9000) = 0 x1 0x20");
    r = next_is(r, 'U', "reset seq: dload clear SCM 0xc2000502(0x1fd9000, 0x0) = 0");
    r = next_is(r, 'U', "reset seq: dload read back SCM 0xc2000501(0x1fd9000) = 0 x1 0x0");
    r = next_is(r, 'U', "reset seq: SDI off SCM 0xc2000109(0x1, 0x0) = 0");
    r = next_is(r, 'U', "reset seq: SPMI arbiter halt SCM 0xc2000901(0x0) = 0");
    r = next_is(r, 'U', "reset seq: park CPUs: 7 of 7 signalled CPUs parked in 0 ms");
    r = next_is(r, 'U', "reset seq: OEM warm reboot SCM 0xc3000922() next");
    r = next_is(r, 'U', "reset seq: OEM warm reboot returned: SCM 0xc3000922() = -1");
    r = next_is(r, 'T', "reset seq done in 0 ms; resetting through PSCI");
    r = next_is(r, 'T', "SYSTEM_RESET2 returned -2; trying SYSTEM_RESET");
    r = next_is(r, 'T', "SYSTEM_RESET returned -1; retrying once a second, watchdog armed");
    assert(r->seq == n);
    // A second trigger (the backup timer, a late first timer, a panic): its hold (in GunyahWdog a no-op by then), PSCI; the
    // observer's late retry: PSCI. Neither releases the watchdog.
    before = trace_count;
    ramoops_log_backup_timer_fired();
    static const uint64_t again[] = {TRACE_HOLD, FN_RESET2, FN_RESET};
    expect_trace(before, again, 3);
    assert(trace_count == before + 3 && read_all() == n);
    clock_now += 6 * (uint64_t)HZ;
    ramoops_log_tick(1, 2, 3, 4);
    assert(trace_count == before + 4 && trace[before + 3] == FN_RESET && smc_count == 13 && park_calls == 1);
    assert(hold_calls == 2 && release_calls == 1);
}

// Each bit alone does its own calls and nothing else, after the clear the steps that may make the reset warm bring with them (F1:
// "implied" in the T and U records); the watchdog's hold comes first and its release right before PSCI.
static void test_reset_seq_single(void)
{
    static const struct { uint32_t mask; unsigned calls; uint64_t ids[4]; const char *names; uint64_t dload; } cases[] = {
        {0x1, 4, {FN_IO_READ, FN_IO_WRITE, FN_IO_READ, FN_SDI}, "dload-clear(implied) sdi-off", 0},
        {0x2, 3, {FN_IO_READ, FN_IO_WRITE, FN_IO_READ}, "dload-clear", 0},
        {0x4, 4, {FN_IO_READ, FN_IO_WRITE, FN_IO_READ, FN_ARB}, "dload-clear(implied) spmi-halt", 0},
        {0x8, 4, {FN_IO_READ, FN_IO_WRITE, FN_IO_READ, TRACE_PARK}, "dload-clear(implied) park-cpus", 0},
        {0x10, 4, {FN_IO_READ, FN_IO_WRITE, FN_IO_READ, FN_OEM}, "dload-clear(implied) oem-warm-reboot", 0},
        {0x100, 1, {FN_IO_READ}, "probe", 0x20},
    };
    for (unsigned c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        seq_setup(cases[c].mask, 60);
        park_signalled = 3; park_arrive = 3;
        ramoops_log_start();
        const unsigned probe = cases[c].mask == 0x100 ? 7 : 0;     // the boot probe's calls come first
        assert(trace_count == probe + 2);
        unsigned before = trace_count;
        ramoops_log_timer_fired();
        static const uint64_t hold[] = {TRACE_HOLD}, psci[] = {TRACE_RELEASE, FN_RESET2, FN_RESET};
        expect_trace(before, hold, 1);
        expect_trace(before + 1, cases[c].ids, cases[c].calls);
        expect_trace(before + 1 + cases[c].calls, psci, 3);
        assert(trace_count == before + cases[c].calls + 4 && hold_calls == 1 && release_calls == 1);
        assert(sdi_calls == (cases[c].mask == 0x1) && arbiter_calls == (cases[c].mask == 0x4) && park_calls == (cases[c].mask == 0x8));
        assert(dload_value == cases[c].dload);
        char expected[128];
        snprintf(expected, sizeof(expected), "reset seq %#x (pagani-reset-seq) reason=timer: %s", cases[c].mask, cases[c].names);
        unsigned n = read_all();
        const struct rec *r = find(n, 'T', expected);
        assert(r && !strcmp(r->text, expected) && !strncmp(r[-1].text, "reset reason=timer up=", 22));
        if (strstr(cases[c].names, "(implied)")) {
            r = next_is(r, 'U', "reset seq: dload read (implied) SCM 0xc2000501(0x1fd9000) = 0 x1 0x20");
            r = next_is(r, 'U', "reset seq: dload clear (implied) SCM 0xc2000502(0x1fd9000, 0x0) = 0");
            next_is(r, 'U', "reset seq: dload read back (implied) SCM 0xc2000501(0x1fd9000) = 0 x1 0x0");
        } else assert(!find(n, 'U', "(implied)"));
        assert(find(n, 'T', "reset seq done in ") && !find(n, 'T', "not 0:"));
    }
    // pagani-reset-seq=0 is the same as absent: no call, no hold, no record, no status or boot record change.
    fresh();
    arg("pagani-ramoops", 1); arg("pagani-reset-after", 60); arg("pagani-reset-seq", 0);
    seq_args(0);
    assert(ramoops_log_vm_ready() && !strcmp(ramoops_log_status(), "ramoops on 0x880000000 copies 36 x 0xfffc; reset after 60 s"));
    ramoops_log_start();
    ramoops_log_timer_fired();
    unsigned n = read_all();
    assert(!smc_count && !park_calls && !hold_calls && find(n, 'B', "reset-after=60s panic-reset=0s") && !find(n, 'B', "reset-seq"));
    assert(!find(n, 'T', "reset seq") && !find(n, 'U', "reset seq") && ramoops_reset_seq_status() == NULL);
}

// A call that fails, or stays INTERRUPTED, or CPUs that do not park: logged, and the next step and the PSCI reset still come; a
// download mode that does not read back 0 skips the steps that may make the reset warm (F1).
static void test_reset_seq_failures(void)
{
    // INTERRUPTED twice per call: continuations with x0 = 1, the request's x1..x3 and the returned x6; the final x0 is logged.
    seq_setup(0x17, 60);
    scm_interrupts = 2;
    ramoops_log_start();
    unsigned before = trace_count;
    ramoops_log_timer_fired();
    assert(smc_count == 6 * 3 && trace_count == before + 6 * 3 + 4);     // and the hold, the release, RESET2, RESET
    for (unsigned k = 0; k < 6; k++) {
        assert(smcs[3 * k].x0 != 1 && smcs[3 * k].x6 == 0);
        for (unsigned j = 1; j < 3; j++) {
            assert(smcs[3 * k + j].x0 == 1 && smcs[3 * k + j].x1 == smcs[3 * k].x1 && smcs[3 * k + j].x2 == smcs[3 * k].x2 &&
                   smcs[3 * k + j].x3 == smcs[3 * k].x3 && smcs[3 * k + j].x6 == 0x5e55000 + 2 * k + j);
        }
    }
    assert(smcs[0].x0 == FN_IO_READ && smcs[3].x0 == FN_IO_WRITE && smcs[9].x0 == FN_SDI && smcs[12].x0 == FN_ARB &&
           smcs[15].x0 == FN_OEM && dload_value == 0 && sdi_calls == 1 && arbiter_calls == 1);
    unsigned n = read_all();
    assert(find(n, 'U', "reset seq: SDI off SCM 0xc2000109(0x1, 0x0) = 0") && find(n, 'U', "reset seq: dload read back SCM "
                                                                                       "0xc2000501(0x1fd9000) = 0 x1 0x0"));

    // Failures: SDI says NOT_SUPPORTED as a 32-bit -1 (x0's upper half clear), and the rest go on.
    seq_setup(0x17, 60);
    scm_fail_function = FN_SDI; scm_fail_x0 = (int64_t)0xffffffffLL;
    ramoops_log_start();
    before = trace_count;
    ramoops_log_timer_fired();
    static const uint64_t all[] = {TRACE_HOLD, FN_IO_READ, FN_IO_WRITE, FN_IO_READ, FN_SDI, FN_ARB, FN_OEM, TRACE_RELEASE, FN_RESET2,
                                   FN_RESET};
    expect_trace(before, all, 10);
    n = read_all();
    const struct rec *r = find(n, 'U', "reset seq: SDI off SCM 0xc2000109(0x1, 0x0) = -1");
    assert(r && !strcmp(r->text, "reset seq: SDI off SCM 0xc2000109(0x1, 0x0) = -1"));
    next_is(r, 'U', "reset seq: SPMI arbiter halt SCM 0xc2000901(0x0) = 0");
    assert(find(n, 'T', "SYSTEM_RESET returned -1; retrying once a second, watchdog armed"));
    // The download mode write refused (-4) while the mode is 0x20: the read back shows it, so SDI and the arbiter halt are skipped
    // with one T record, and the reset goes on as without them.
    seq_setup(0x7, 60);
    scm_fail_function = FN_IO_WRITE; scm_fail_x0 = -4;
    ramoops_log_start();
    before = trace_count;
    ramoops_log_timer_fired();
    n = read_all();
    r = find(n, 'U', "reset seq: dload clear SCM 0xc2000502(0x1fd9000, 0x0) = -4");
    assert(r && dload_value == 0x20);
    r = next_is(r, 'U', "reset seq: dload read back SCM 0xc2000501(0x1fd9000) = 0 x1 0x20");
    r = next_is(r, 'T', "reset seq: the download mode reads back 0x20, not 0: sdi-off spmi-halt skipped (a warm reset would enter "
                        "the crash dump)");
    next_is(r, 'T', "reset seq done in 0 ms; resetting through PSCI");
    static const uint64_t gated[] = {TRACE_HOLD, FN_IO_READ, FN_IO_WRITE, FN_IO_READ, TRACE_RELEASE, FN_RESET2, FN_RESET};
    expect_trace(before, gated, 7);
    assert(!sdi_calls && !arbiter_calls && call_count == 4 && trace_count == before + 7);
    // The arbiter halt and the OEM reboot refused (the clear implied): logged, PSCI follows.
    seq_setup(0x14, 60);
    scm_fail_function = FN_ARB; scm_fail_x0 = -2; oem_x0 = (uint64_t)-4;
    ramoops_log_start();
    ramoops_log_timer_fired();
    n = read_all();
    assert(find(n, 'U', "reset seq: SPMI arbiter halt SCM 0xc2000901(0x0) = -2"));
    assert(find(n, 'U', "reset seq: OEM warm reboot returned: SCM 0xc3000922() = -4") && call_count == 4 && dload_value == 0);
    // SDI never stops being INTERRUPTED: 64 SMCs, logged as such, then the next step.
    seq_setup(0x7, 60);
    scm_interrupts = 1000; interrupt_only = FN_SDI;
    ramoops_log_start();
    ramoops_log_timer_fired();
    assert(smc_count == 3 + 64 + 1 && trace_how_many(FN_RESET) == 1 && arbiter_calls == 1);
    n = read_all();
    r = find(n, 'U', "reset seq: SDI off SCM 0xc2000109(0x1, 0x0) = 1 (still INTERRUPTED after 64 SMCs)");
    assert(r);
    next_is(r, 'U', "reset seq: SPMI arbiter halt SCM 0xc2000901(0x0) = 0");
    // Every call INTERRUPTED to the end: the implied clear never reads back 0, so SDI and the arbiter halt do not run at all.
    seq_setup(0x5, 60);
    scm_interrupts = 1000;
    ramoops_log_start();
    ramoops_log_timer_fired();
    assert(smc_count == 3 * 64 && trace_how_many(FN_RESET) == 1 && !sdi_calls && !arbiter_calls);
    n = read_all();
    assert(find(n, 'U', "reset seq: dload read back (implied) SCM 0xc2000501(0x1fd9000) = 1 (still INTERRUPTED after 64 SMCs)"));
    assert(find(n, 'T', "reset seq: the download mode reads back x0 1, not 0: sdi-off spmi-halt skipped"));
    // CPUs: 5 of 7 park (two never answer): the wait ends after 1 s and the reset goes on.
    seq_setup(0x18, 60);
    park_signalled = 7; park_arrive = 5; clock_step = 1000;
    ramoops_log_start();
    uint64_t start = clock_now;
    ramoops_log_timer_fired();
    assert(clock_now - start >= (uint64_t)HZ && clock_now - start < 2ULL * HZ && trace_how_many(FN_OEM) == 1);
    n = read_all();
    r = find(n, 'U', "reset seq: park CPUs: 5 of 7 signalled CPUs parked in 1000 ms (gave up after 1 s)");
    assert(r);
    next_is(r, 'U', "reset seq: OEM warm reboot SCM 0xc3000922() next");
    // They answer 200 ms late: the wait ends then. No other CPU at all: nothing to wait for.
    seq_setup(0x8, 60);
    park_signalled = 7; park_arrive = 7; clock_step = 1000;
    ramoops_log_start();
    park_due = clock_now + (uint64_t)HZ / 5;
    ramoops_log_timer_fired();
    n = read_all();
    r = find(n, 'U', "reset seq: park CPUs: 7 of 7 signalled CPUs parked in ");
    unsigned ms = 0;
    assert(r && sscanf(r->text, "reset seq: park CPUs: 7 of 7 signalled CPUs parked in %u ms", &ms) == 1 && !strstr(r->text, "gave up"));
    assert(ms >= 190 && ms <= 200 && !park_target);
    seq_setup(0x8, 60);
    ramoops_log_start();
    ramoops_log_timer_fired();
    assert(find(read_all(), 'U', "reset seq: park CPUs: 0 of 0 signalled CPUs parked in 0 ms"));
    // The probe's failures on the screen line: e<x0>.
    seq_setup(0x100, 0);
    scm_fail_function = FN_AVAIL; scm_fail_x0 = -1;
    ramoops_log_start();
    assert(!strcmp(ramoops_reset_seq_status(),
                   "reset seq 0x100 avail sdi e-1 dl e-1 rd e-1 wr e-1 spmi e-1 oem e-1 dload 0x20"));
    assert(strlen(ramoops_reset_seq_status()) <= 79);   // a BootProgress stage line keeps 79 characters
    seq_setup(0x100, 0);
    scm_fail_function = FN_IO_READ; scm_fail_x0 = -3;
    avail_ids[0] = 0x2000109; avail_x1[0] = 1;
    ramoops_log_start();
    assert(!strcmp(ramoops_reset_seq_status(), "reset seq 0x100 avail sdi 1 dl 0 rd 0 wr 0 spmi 0 oem 0 dload e-3"));
    assert(find(read_all(), 'U', "reset seq probe: dload read SCM 0xc2000501(0x1fd9000) = -3"));
}

// F1: the steps that may make the reset warm, and pagani-reset-cold together with any bit, bring the download mode's clear with
// them and run only when it reads back 0; the explicit clear (0x002) gates them the same way.
static void cold_setup(uint32_t mask)
{
    fresh();
    seq_expected = 1;
    arg("pagani-ramoops", 1); arg("pagani-reset-after", 60); arg("pagani-reset-cold", 1); arg("pagani-reset-seq", mask);
    seq_args(mask);
    assert(ramoops_log_vm_ready());
    ramoops_log_start();
}

static void test_reset_seq_interlock(void)
{
    // The mode was 0 already and the write is refused: it reads back 0, so SDI goes out.
    seq_setup(0x1, 60);
    dload_value = 0; scm_fail_function = FN_IO_WRITE; scm_fail_x0 = -4;
    ramoops_log_start();
    ramoops_log_timer_fired();
    unsigned n = read_all();
    assert(sdi_calls == 1 && find(n, 'U', "reset seq: dload read back (implied) SCM 0xc2000501(0x1fd9000) = 0 x1 0x0"));
    assert(find(n, 'U', "reset seq: dload clear (implied) SCM 0xc2000502(0x1fd9000, 0x0) = -4") && !find(n, 'T', "not 0:"));
    // The read fails (-3): the mode is not known to be 0 (the write went through, but nothing shows it), so no SDI.
    seq_setup(0x1, 60);
    scm_fail_function = FN_IO_READ; scm_fail_x0 = -3;
    ramoops_log_start();
    unsigned before = trace_count;
    ramoops_log_timer_fired();
    static const uint64_t unread[] = {TRACE_HOLD, FN_IO_READ, FN_IO_WRITE, FN_IO_READ, TRACE_RELEASE, FN_RESET2, FN_RESET};
    expect_trace(before, unread, 7);
    assert(!sdi_calls && dload_value == 0 && trace_count == before + 7);
    n = read_all();
    const struct rec *r = find(n, 'U', "reset seq: dload read back (implied) SCM 0xc2000501(0x1fd9000) = -3");
    assert(r);
    next_is(r, 'T', "reset seq: the download mode reads back x0 -3, not 0: sdi-off skipped (a warm reset would enter the crash dump)");
    // x0 and the value are judged on their low 32 bits, as Linux reads them: upper halves that are not 0 change nothing.
    seq_setup(0x1, 60);
    read_x0_upper = 0x100000000ULL; read_x1_upper = 0xdead00000000ULL;
    ramoops_log_start();
    ramoops_log_timer_fired();
    n = read_all();
    assert(sdi_calls == 1 && dload_value == 0 && !find(n, 'T', "not 0:"));
    assert(find(n, 'U', "reset seq: dload read back (implied) SCM 0xc2000501(0x1fd9000) = 0") && find(n, 'U', "reset seq: SDI off"));
    // The explicit clear gates the same way: 0x003 with the write refused runs no SDI.
    seq_setup(0x3, 60);
    scm_fail_function = FN_IO_WRITE; scm_fail_x0 = -4;
    ramoops_log_start();
    ramoops_log_timer_fired();
    n = read_all();
    assert(!sdi_calls && find(n, 'T', "reset seq 0x3 (pagani-reset-seq) reason=timer: dload-clear sdi-off"));
    assert(find(n, 'T', "reset seq: the download mode reads back 0x20, not 0: sdi-off skipped") && !find(n, 'U', "(implied)"));
    // pagani-reset-cold with the probe alone: the clear is implied and the reset is SYSTEM_RESET only, Android's single call.
    cold_setup(0x100);
    before = trace_count;
    ramoops_log_timer_fired();
    static const uint64_t cold[] = {TRACE_HOLD, FN_IO_READ, FN_IO_WRITE, FN_IO_READ, TRACE_RELEASE, FN_RESET};
    expect_trace(before, cold, 6);
    assert(trace_count == before + 6 && dload_value == 0 && !trace_how_many(FN_RESET2));
    n = read_all();
    r = find(n, 'T', "reset seq 0x100 (pagani-reset-seq) reason=timer: probe dload-clear(implied)");
    assert(r && !strcmp(r->text, "reset seq 0x100 (pagani-reset-seq) reason=timer: probe dload-clear(implied)"));
    assert(strstr(r[-1].text, "path=SYSTEM_RESET (0x84000009) (pagani-reset-cold)"));
    r = next_is(r, 'U', "reset seq: dload read (implied) SCM 0xc2000501(0x1fd9000) = 0 x1 0x20");
    r = next_is(r, 'U', "reset seq: dload clear (implied) SCM 0xc2000502(0x1fd9000, 0x0) = 0");
    r = next_is(r, 'U', "reset seq: dload read back (implied) SCM 0xc2000501(0x1fd9000) = 0 x1 0x0");
    r = next_is(r, 'T', "reset seq done in 0 ms; resetting through PSCI");
    next_is(r, 'T', "SYSTEM_RESET returned -1; retrying once a second, watchdog armed");
    // The same with the write refused: the mode reads back 0x20, so pagani-reset-cold is not applied: SYSTEM_RESET2 first as
    // without it, for this resetter and a later one.
    cold_setup(0x100);
    scm_fail_function = FN_IO_WRITE; scm_fail_x0 = -4;
    before = trace_count;
    ramoops_log_timer_fired();
    static const uint64_t warm[] = {TRACE_HOLD, FN_IO_READ, FN_IO_WRITE, FN_IO_READ, TRACE_RELEASE, FN_RESET2, FN_RESET};
    expect_trace(before, warm, 7);
    n = read_all();
    r = find(n, 'T', "reset seq: the download mode reads back 0x20, not 0:");
    assert(r && !strcmp(r->text, "reset seq: the download mode reads back 0x20, not 0: pagani-reset-cold skipped (a warm reset would "
                                 "enter the crash dump)"));
    assert(find(n, 'T', "SYSTEM_RESET2 returned -2; trying SYSTEM_RESET"));
    before = trace_count;
    ramoops_log_backup_timer_fired();
    static const uint64_t later[] = {TRACE_HOLD, FN_RESET2, FN_RESET};
    expect_trace(before, later, 3);
    // Steps and pagani-reset-cold both skipped, named in one record.
    cold_setup(0x11c);
    scm_fail_function = FN_IO_WRITE; scm_fail_x0 = -4;
    park_signalled = 3; park_arrive = 3;
    ramoops_log_timer_fired();
    assert(find(read_all(), 'T', "reads back 0x20, not 0: spmi-halt park-cpus oem-warm-reboot pagani-reset-cold skipped"));
    assert(!arbiter_calls && !park_calls && !trace_how_many(FN_OEM) && trace_how_many(FN_RESET2) == 1);
    // pagani-reset-cold without pagani-reset-seq: as before (SYSTEM_RESET only, not a single SCM call or hold).
    fresh();
    arg("pagani-ramoops", 1); arg("pagani-reset-after", 60); arg("pagani-reset-cold", 1);
    assert(ramoops_log_vm_ready());
    ramoops_log_start();
    before = trace_count;
    ramoops_log_timer_fired();
    assert(trace_count == before + 1 && trace[before] == FN_RESET && !smc_count && !hold_calls);
}

// S4: after 0x008 parked the other CPUs, interrupts stay masked up to the PSCI call (and after it: reset_retry), as Linux's
// machine_restart; otherwise the resetting thread's own state comes back. The observer resets from thread context (interrupts on).
static void test_reset_seq_irqs(void)
{
    static const struct { uint32_t mask; int masked; } cases[] = {{0x8, 1}, {0x1f, 1}, {0x1, 0}, {0x7, 0}, {0x100, 0}};
    for (unsigned c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        seq_setup(cases[c].mask, 60);
        park_signalled = 3; park_arrive = 3;
        ramoops_log_start();
        const unsigned from = call_count;
        clock_now += 63 * (uint64_t)HZ;                      // past the deadline + 2 s, the timer never fired: the observer resets
        assert(!host_daif_masked);
        ramoops_log_tick(1, 2, 3, 4);
        assert(reset_started && hold_masked == 0 && release_calls == 1 && host_daif_masked == cases[c].masked);
        unsigned resets = 0;
        for (unsigned i = from; i < call_count; i++) {
            if (calls[i].function != FN_RESET2 && calls[i].function != FN_RESET) continue;
            assert(calls[i].masked == cases[c].masked);
            resets++;
        }
        assert(resets == 2 && find(read_all(), 'T', "reset reason=observer"));
    }
    // A park the download mode skipped (it reads back 0x20): nothing parked, the state comes back.
    seq_setup(0x8, 60);
    scm_fail_function = FN_IO_WRITE; scm_fail_x0 = -4;
    ramoops_log_start();
    clock_now += 63 * (uint64_t)HZ;
    ramoops_log_tick(1, 2, 3, 4);
    assert(!park_calls && !host_daif_masked && !calls[call_count - 1].masked);
    // Without pagani-reset-seq: the observer's reset as before (interrupts as the thread had them).
    fresh();
    arg("pagani-ramoops", 1); arg("pagani-reset-after", 60);
    assert(ramoops_log_vm_ready());
    ramoops_log_start();
    clock_now += 63 * (uint64_t)HZ;
    ramoops_log_tick(1, 2, 3, 4);
    assert(reset_started && calls[call_count - 1].function == FN_RESET && !calls[call_count - 1].masked && !host_daif_masked);
}

// S5: the mask runs only as written "0x" and 1 to 8 hex digits (the last occurrence, the one XNU's parser keeps), read as that value,
// with known bits only. Anything else refuses all of it: no probe, no hold, no step, the reset as without the boot-arg (fresh()
// checks that no SCM call, park or hold happened); one T record at boot and the status line say so.
static void test_reset_seq_refused(void)
{
    static const struct { const char *args; uint32_t parsed; const char *record; } refused[] = {
        // XNU: "0X10F" is no number, so its first three characters come back as bytes ('0', 'X', '1', NUL).
        {"-v pagani-reset-seq=0X10F pagani-ramoops=1", 0x00315830, "pagani-reset-seq=0X10F read as 0x315830: unknown bits"},
        {"-v pagani-ramoops=1 pagani-reset-seq=all", 0x006c6c61, "pagani-reset-seq=all read as 0x6c6c61: unknown bits"},
        {"-v pagani-ramoops=1 pagani-reset-seq=0101", 0x41, "pagani-reset-seq=0101 read as 0x41: unknown bits"},   // octal
        {"-v pagani-ramoops=1 pagani-reset-seq=010", 0x8,
         "pagani-reset-seq=010 read as 0x8: not written as 0x and 1 to 8 hex digits"},
        {"-v pagani-ramoops=1 pagani-reset-seq=271", 0x10f,
         "pagani-reset-seq=271 read as 0x10f: not written as 0x and 1 to 8 hex digits"},
        {"-v pagani-ramoops=1 pagani-reset-seq=0b1", 0x1, "pagani-reset-seq=0b1 read as 0x1: not written as 0x and 1 to 8 hex digits"},
        {"-v pagani-ramoops=1 pagani-reset-seq", 0x1,
         "pagani-reset-seq (no value) read as 0x1: not written as 0x and 1 to 8 hex digits"},
        {"-v pagani-ramoops=1 pagani-reset-seq=0x100000001", 0x1,
         "pagani-reset-seq=0x100000001 read as 0x1: not written as 0x and 1 to 8 hex digits"},
        {"-v pagani-ramoops=1 pagani-reset-seq=0x00000001f", 0x1f,
         "pagani-reset-seq=0x00000001f read as 0x1f: not written as 0x and 1 to 8 hex digits"},
        {"-v pagani-ramoops=1 pagani-reset-seq=0x1k", 0x400, "pagani-reset-seq=0x1k read as 0x400: unknown bits"},
        {"-v pagani-ramoops=1 pagani-reset-seq=0x80000001", 0x80000001, "pagani-reset-seq=0x80000001 read as 0x80000001: unknown bits"},
        // The last occurrence counts (XNU's too): a good first one does not help a bad last one.
        {"-v pagani-reset-seq=0x1 pagani-ramoops=1 pagani-reset-seq=0X1", 0x00315830,
         "pagani-reset-seq=0X1 read as 0x315830: unknown bits"},
        // The text and the parsed value disagree (not from XNU's parser; refused all the same).
        {"-v pagani-ramoops=1 pagani-reset-seq=0x2", 0x1, "pagani-reset-seq=0x2 read as 0x1: parsed as another value"},
        {"-v pagani-ramoops=1", 0x1, "pagani-reset-seq (not in the boot-args) read as 0x1: not written as 0x and 1 to 8 hex digits"},
    };
    for (unsigned c = 0; c < sizeof(refused) / sizeof(refused[0]); c++) {
        fresh();
        arg("pagani-ramoops", 1); arg("pagani-reset-after", 60); arg("pagani-reset-seq", refused[c].parsed);
        boot_args_text = refused[c].args;
        assert(ramoops_log_vm_ready());
        char status[160];
        snprintf(status, sizeof(status), "ramoops on 0x880000000 copies 36 x 0xfffc; reset after 60 s; reset seq REFUSED %#x",
                 refused[c].parsed);
        assert(!strcmp(ramoops_log_status(), status) && !reset_seq_mask && reset_seq_refused == refused[c].parsed);
        ramoops_log_start();
        unsigned before = trace_count;
        ramoops_log_timer_fired();
        static const uint64_t plain[] = {FN_RESET2, FN_RESET};
        expect_trace(before, plain, 2);
        assert(trace_count == before + 2 && ramoops_reset_seq_status() == NULL);
        unsigned n = read_all();
        char record[320];
        snprintf(record, sizeof(record), "reset seq refused: %s; none of it runs (write the mask as 0x and 1 to 8 hex digits)",
                 refused[c].record);
        const struct rec *r = find(n, 'T', "reset seq refused: ");
        assert(r && !strcmp(r->text, record));
        assert(!find(n, 'T', "reset seq 0x") && !find(n, 'U', "reset seq") && !find(n, 'B', "reset-seq"));
    }
    // Written right: hex digits in either case after a lowercase 0x, leading zeros, the last of several occurrences.
    static const struct { const char *args; uint32_t mask; } accepted[] = {
        {"-v pagani-ramoops=1 pagani-reset-seq=0x10F", 0x10f},
        {"-v pagani-ramoops=1 pagani-reset-seq=0x00000001", 0x1},
        {"-v pagani-reset-seq=0X1 pagani-ramoops=1 pagani-reset-seq=0x1", 0x1},
        {"-v pagani-ramoops=1\tpagani-reset-seq=0x100", 0x100},
    };
    for (unsigned c = 0; c < sizeof(accepted) / sizeof(accepted[0]); c++) {
        fresh();
        seq_expected = 1;
        arg("pagani-ramoops", 1); arg("pagani-reset-after", 60); arg("pagani-reset-seq", accepted[c].mask);
        boot_args_text = accepted[c].args;
        assert(ramoops_log_vm_ready() && reset_seq_mask == accepted[c].mask && !reset_seq_refused);
        ramoops_log_start();
        ramoops_log_timer_fired();
        assert(hold_calls == 1 && release_calls == 1 && !find(read_all(), 'T', "refused"));
    }
    // The log off: nothing runs, the status line says why; also when nothing else is configured.
    fresh();
    arg("pagani-reset-after", 30); arg("pagani-reset-seq", 0x00315830);
    boot_args_text = "-v pagani-reset-seq=0X10F";
    assert(!ramoops_log_vm_ready() && !strcmp(ramoops_log_status(), "ramoops off; reset after 30 s; reset seq REFUSED 0x315830"));
    ramoops_log_start();
    ramoops_log_timer_fired();
    assert(maps == 0 && trace_how_many(FN_RESET) == 1);
    fresh();
    arg("pagani-reset-seq", 0x41);
    boot_args_text = "-v pagani-reset-seq=0101";
    assert(!ramoops_log_vm_ready() && !strcmp(ramoops_log_status(), "ramoops off; reset seq REFUSED 0x41"));
    ramoops_log_start();
    assert(!timer_arms && !call_count);
}

// Once per boot with resetters that overlap: a second resetter that comes while the steps run waits for them (at most 2 s), then
// resets through PSCI without running them; two threads resetting at once run them exactly once between them. Each resetter asks
// for the watchdog's hold; only the one that ran the steps releases it.
static uint64_t nested_waited;
static void nested_reset(void)
{
    uint64_t start = clock_now;
    ramoops_log_backup_timer_fired();
    nested_waited = clock_now - start;
}

static volatile int race_go;
static void *race_reset(void *param)
{
    const int which = *(const int *)param;
    while (!__atomic_load_n(&race_go, __ATOMIC_ACQUIRE)) {}
    if (which) ramoops_log_backup_timer_fired();
    else ramoops_log_timer_fired();
    return NULL;
}

static void test_reset_seq_once(void)
{
    // The backup timer fires on another CPU while the first timer's SDI call is out: it waits (the steps stay RUNNING
    // until the first resetter returns here, so the full 2 s), resets through PSCI, and the first resetter goes on.
    seq_setup(0x7, 60);
    clock_step = 1000;
    smc_hook_function = FN_SDI; smc_hook = nested_reset;
    ramoops_log_start();
    unsigned before = trace_count;
    ramoops_log_timer_fired();
    static const uint64_t order[] = {TRACE_HOLD, FN_IO_READ, FN_IO_WRITE, FN_IO_READ, FN_SDI, TRACE_HOLD, FN_RESET2, FN_RESET, FN_ARB,
                                     TRACE_RELEASE, FN_RESET2, FN_RESET};
    expect_trace(before, order, 12);
    assert(trace_count == before + 12 && smc_count == 5 && nested_waited >= 2ULL * HZ && nested_waited < 3ULL * HZ);
    assert(reset_seq_state == RESET_SEQ_DONE && hold_calls == 2 && release_calls == 1);
    unsigned n = read_all();
    assert(find(n, 'T', "reset seq done in 2") && find(n, 'U', "reset seq: SPMI arbiter halt SCM 0xc2000901(0x0) = 0"));
    // A panic after the timed reset ran the steps: none again.
    before = trace_count;
    ramoops_log_panic_reset();                       // no panic wait configured beyond the deadline: 5 s at least
    assert(trace_count == before + 3 && trace[before] == TRACE_HOLD && trace[before + 1] == FN_RESET2 && smc_count == 5);
    assert(release_calls == 1);
    // Two threads: the timer and the backup timer at the same moment. Exactly one runs the steps.
    for (int round = 0; round < 64; round++) {
        seq_setup(0x1f, 60);
        park_signalled = 2; park_arrive = 2;
        ramoops_log_start();
        before = trace_count;
        pthread_t threads[2];
        int which[2] = {0, 1};
        race_go = 0;
        for (int i = 0; i < 2; i++) assert(!pthread_create(&threads[i], NULL, race_reset, &which[i]));
        __atomic_store_n(&race_go, 1, __ATOMIC_RELEASE);
        for (int i = 0; i < 2; i++) assert(!pthread_join(threads[i], NULL));
        assert(sdi_calls == 1 && arbiter_calls == 1 && park_calls == 1 && smc_count == 6);
        assert(trace_how_many(FN_IO_WRITE) == 1 && trace_how_many(FN_OEM) == 1 && hold_calls == 2 && release_calls == 1);
        assert(trace_how_many(FN_RESET2) == 2 && trace_how_many(FN_RESET) == 2 && trace_count == before + 14);
        n = read_all();
        unsigned starts = 0;
        for (unsigned i = 0; i < n; i++) starts += records[i].kind == 'T' && !strncmp(records[i].text, "reset seq 0x1f ", 15);
        assert(starts == 1 && reset_seq_state == RESET_SEQ_DONE);
    }
}

// The panic paths reset through the same steps (reason=panic); the watchdog-bite path applies none of them (and releases nothing);
// with the log off the steps still run (no records).
static void test_reset_seq_paths(void)
{
    fresh();
    seq_expected = 1;
    arg("pagani-ramoops", 1); arg("pagani-panic-reset", 2); arg("pagani-reset-seq", 0x5);
    seq_args(0x5);
    assert(ramoops_log_vm_ready());
    ramoops_log_start();
    for (const char *p = "OnePlus 13T / XNU PANIC\nMESSAGE\nKernel data abort."; *p; p++) ramoops_log_panic_putc(*p);
    clock_step = 100000;
    unsigned before = trace_count;
    ramoops_log_panic_reset();
    static const uint64_t panic_steps[] = {TRACE_HOLD, FN_IO_READ, FN_IO_WRITE, FN_IO_READ, FN_SDI, FN_ARB, TRACE_RELEASE, FN_RESET2,
                                           FN_RESET};
    expect_trace(before, panic_steps, 9);
    unsigned n = read_all();
    const struct rec *r = find(n, 'T', "panic: reset in 2000 ms");
    assert(r && !strncmp(r[1].text, "reset reason=panic", 18));
    next_is(r + 1, 'T', "reset seq 0x5 (pagani-reset-seq) reason=panic: dload-clear(implied) sdi-off spmi-halt");
    // A panic without a screen: the same.
    fresh();
    seq_expected = 1;
    arg("pagani-ramoops", 1); arg("pagani-panic-reset", 1); arg("pagani-reset-seq", 0x1);
    seq_args(0x1);
    assert(ramoops_log_vm_ready());
    ramoops_log_start();
    clock_step = 100000;
    headless("no screen %u", 7U);
    assert(sdi_calls == 1 && trace_how_many(FN_RESET) == 1 && release_calls == 1);
    assert(find(read_all(), 'T', "reset seq 0x1 (pagani-reset-seq) reason=panic: dload-clear(implied) sdi-off"));
    // The watchdog-bite path (pagani-reset-wdog=1 and our watchdog armed: the bite resets): no hold, no step, no release, one T
    // record; PSCI comes only 10 s later in the kernel (at once on the host). Not armed (the bite call answers 0): the steps as usual.
    for (int armed = 1; armed >= 0; armed--) {
        fresh();
        seq_expected = 1;
        arg("pagani-ramoops", 1); arg("pagani-reset-after", 60); arg("pagani-reset-wdog", 1); arg("pagani-reset-seq", 0x7);
        seq_args(0x7);
        assert(ramoops_log_vm_ready());
        ramoops_log_start();
        bite_answer = armed;
        before = trace_count;
        ramoops_log_timer_fired();
        static const uint64_t bitten[] = {TRACE_BITE, FN_RESET2, FN_RESET};
        static const uint64_t steps[] = {TRACE_BITE, TRACE_HOLD, FN_IO_READ, FN_IO_WRITE, FN_IO_READ, FN_SDI, FN_ARB, TRACE_RELEASE,
                                         FN_RESET2, FN_RESET};
        if (armed) expect_trace(before, bitten, 3);
        else expect_trace(before, steps, 10);
        assert(trace_count == before + (armed ? 3 : 10) && bite_calls == 1);
        assert(armed ? !smc_count && !hold_calls && !release_calls && reset_seq_state == RESET_SEQ_IDLE : sdi_calls == 1);
        n = read_all();
        assert(!find(n, 'T', "; first the watchdog bite (pagani-reset-wdog)") == !armed);
        assert(!find(n, 'T', "reset seq 0x7 not applied: the watchdog bite resets (pagani-reset-wdog)") == !armed);
    }
    // The same through reset_seq itself: a later resetter does not wait.
    seq_setup(0x7, 60);
    ramoops_log_start();
    reset_seq("timer", 1, 1);
    assert(!smc_count && !release_calls && reset_seq_state == RESET_SEQ_IDLE);
    assert(find(read_all(), 'T', "reset seq 0x7 not applied: the watchdog bite resets (pagani-reset-wdog)"));
    uint64_t start = clock_now;
    reset_seq("backup timer", 0, 1);
    assert(clock_now - start < 10 && !smc_count);
    // The reset configured without the log: the steps run all the same, nothing is recorded (nothing is mapped).
    fresh();
    seq_expected = 1;
    arg("pagani-reset-after", 30); arg("pagani-reset-seq", 0x3);
    seq_args(0x3);
    assert(!ramoops_log_vm_ready() && !strcmp(ramoops_log_status(), "ramoops off; reset after 30 s; reset seq 0x3"));
    ramoops_log_start();
    ramoops_log_timer_fired();
    assert(maps == 0 && smc_count == 4 && sdi_calls == 1 && dload_value == 0 && trace_how_many(FN_RESET) == 1);
    assert(hold_calls == 1 && release_calls == 1);
    // The probe alone, nothing else configured: it still asks at boot (the screen line), and nothing resets.
    fresh();
    seq_expected = 1;
    arg("pagani-reset-seq", 0x100);
    seq_args(0x100);
    assert(!ramoops_log_vm_ready() && ramoops_log_status() == NULL);
    ramoops_log_start();
    assert(smc_count == 7 && call_count == 0 && timer_arms == 0 && ramoops_reset_seq_status() != NULL && !hold_calls);
}

// One realistic run for the decoder test: boot records, a few thousand console lines (the console ring wraps), process
// stages, GPU rows, a panic screen and the reset path. expected.tsv lists the records the copies still hold.
static void dump(const char *directory)
{
    fresh();
    arg("pagani-ramoops", 1); arg("pagani-reset-after", 120); arg("pagani-panic-reset", 15);
    boot_args_text = "-v serial=0 debug=0x14e cpus=8 pagani-gpu=1 pagani-ramoops=1 pagani-reset-after=120 pagani-panic-reset=15";
    clock_step = 997;
    assert(ramoops_log_vm_ready());
    const char *early[] = {"S1: XNU entry, MMU off", "S2: bootstrap maps ready", "S3: MMU enabled", "S4: arm_init entered",
                           "S5: PE_init_platform ready", "arm_vm_init enter"};
    for (unsigned i = 0; i < 6; i++) ramoops_log_line('S', early[i]);
    ramoops_log_line('S', "S6: kernel VM and exception vectors ready");
    ramoops_log_start();
    char line[160];
    static char rows[PAGANI_GPU_ROWS][PAGANI_GPU_ROW_BYTES];
    for (unsigned i = 0; i < 3000; i++) {
        int length = snprintf(line, sizeof(line), "[%5u.%03u] adreno830: step %u reg 0x%05x value 0x%08x%s\n", i / 10,
                              (i % 10) * 100, i, 0x1a000 + (i * 4) % 0x1000, i * 2654435761u, i % 7 ? "" : " (slow path)");
        for (int k = 0; k < length; k++) ramoops_log_putc(line[k]);
        if (i % 250 == 0) {
            snprintf(rows[0], PAGANI_GPU_ROW_BYTES, "GPU S%u %s ok %ums", i / 250, i < 2750 ? "GMU" : "CP", i / 3);
            snprintf(rows[1], PAGANI_GPU_ROW_BYTES, "SQE 0x01500081 AQE 0x01500022 step %u", i);
            snprintf(rows[2], PAGANI_GPU_ROW_BYTES, "HFI q0 %u q1 %u", i % 17, i % 5);
            snprintf(rows[3], PAGANI_GPU_ROW_BYTES, "SMMU CB0 FSR 0x%x", i % 3 ? 0 : 0x402);
            ramoops_log_rows('G', (const char (*)[PAGANI_GPU_ROW_BYTES])rows, 4);
            ramoops_log_root("stage7", "GPU stage", i / 250);
        }
        if (i % 600 == 0) ramoops_log_tick(i / 10, i * 3, 100000 - i, 90000 - i);
        if (i % 40 == 0) {
            snprintf(line, sizeof(line), "pid=%u launchd exec /usr/libexec/daemon%u", 100 + i / 40, i / 40);
            ramoops_log_line('S', line);
        }
    }
    ramoops_log_exception(0x96000010, 0xfffffff00bfd9e58, 0xfffffff852055000);
    const char *screen = "OnePlus 13T / XNU PANIC\npanic entry reached; values below are hexadecimal\nMESSAGE\n"
                         "Kernel data abort. at pc 0xfffffff00bfd9e58, lr 0xfffffff00bfd9e40\nRESET IN 15 s; log kept in ramoops";
    for (const char *p = screen; *p; p++) ramoops_log_panic_putc(*p);
    clock_step = 10000000;
    ramoops_log_panic_reset();
    char path[1024];
    for (unsigned z = 0; z < 2; z++) {
        snprintf(path, sizeof(path), "%s/%s.zone", directory, z ? "pmsg" : "console");
        FILE *f = fopen(path, "wb");
        assert(f && fwrite(zone_bytes(z), 1, zone_size[z], f) == zone_size[z] && !fclose(f));
    }
    unsigned found[3];
    unsigned n = read_log(found);
    snprintf(path, sizeof(path), "%s/expected.tsv", directory);
    FILE *f = fopen(path, "w");
    assert(f);
    for (unsigned i = 0; i < n; i++) fprintf(f, "%u\t%u\t%c\t%s\n", records[i].seq, records[i].ms, records[i].kind, records[i].text);
    assert(!fclose(f));
    printf("dumped %u records (seq %u..%u; boot %u, events %u, console %u) in %u copies of 0x%x bytes\n", n, records[0].seq,
           records[n - 1].seq, found[0], found[1], found[2], copy_count, copy_bytes);
}

// The platform expert's restart and power off (PaganiArmDTPlatform haltRestart, pagani-halt). A restart is any other reset with its reason
// in the record. A power off calls SYSTEM_OFF after the same record and steps, once, with the watchdog held through the call instead of
// taking its bite, and resets when the call comes back.
static void test_platform_halt(void)
{
    fresh();
    arg("pagani-ramoops", 1);
    assert(ramoops_log_vm_ready());
    ramoops_log_start();
    ramoops_log_putc('h'); ramoops_log_putc('i');    // a partial line is flushed first, as for any reset
    ramoops_log_restart("restart");
    static const uint64_t restart[] = {FN_VERSION, FN_FEATURES, FN_RESET2, FN_RESET};
    expect_trace(0, restart, 4);
    assert(trace_count == 4 && !hold_calls && !bite_calls);
    unsigned n = read_all();
    const struct rec *r = find(n, 'C', "hi");
    assert(r && r[1].kind == 'T' && !strncmp(r[1].text, "reset reason=restart up=", 24) && !strstr(r[1].text, "SYSTEM_OFF"));
    r = next_is(r + 1, 'T', "SYSTEM_RESET2 returned -2; trying SYSTEM_RESET");
    r = next_is(r, 'T', "SYSTEM_RESET returned -1; retrying once a second, watchdog armed");

    // pagani-reset-wdog=1 restarts through the bite, as the timed reset does.
    fresh();
    arg("pagani-ramoops", 1); arg("pagani-reset-wdog", 1);
    bite_answer = 1;
    assert(ramoops_log_vm_ready());
    ramoops_log_restart("restart");
    assert(bite_calls == 1 && !hold_calls);
    assert(find(read_all(), 'T', "first the watchdog bite (pagani-reset-wdog)"));

    // The power off, no steps: the hold, SYSTEM_OFF (it may come back on a firmware that refuses), the release, the reset as usual.
    fresh();
    seq_expected = 1;
    arg("pagani-ramoops", 1);
    assert(ramoops_log_vm_ready());
    ramoops_log_start();
    ramoops_log_power_off("power off");
    static const uint64_t power_off[] = {FN_VERSION, FN_FEATURES, TRACE_HOLD, FN_OFF, TRACE_RELEASE, FN_RESET2, FN_RESET};
    expect_trace(0, power_off, 7);
    assert(trace_count == 7 && hold_calls == 1 && release_calls == 1 && !smc_count);
    n = read_all();
    r = find(n, 'T', "reset reason=power off up=");
    assert(r && strstr(r->text, "; first SYSTEM_OFF (0x84000008)"));
    r = next_is(r, 'T', "SYSTEM_OFF returned -1; resetting instead");
    r = next_is(r, 'T', "SYSTEM_RESET2 returned -2; trying SYSTEM_RESET");
    r = next_is(r, 'T', "SYSTEM_RESET returned -1; retrying once a second, watchdog armed");
    ramoops_log_power_off("power off");              // a second caller, or the timer: straight to the reset, no second SYSTEM_OFF
    ramoops_log_timer_fired();
    assert(trace_how_many(FN_OFF) == 1 && trace_how_many(TRACE_RELEASE) == 1 && read_all() == n);

    // pagani-reset-wdog=1: never the bite on a power off, which would restart the phone.
    fresh();
    seq_expected = 1;
    arg("pagani-ramoops", 1); arg("pagani-reset-wdog", 1);
    bite_answer = 1;
    assert(ramoops_log_vm_ready());
    ramoops_log_power_off("power off");
    assert(!bite_calls && hold_calls == 1 && trace_how_many(FN_OFF) == 1);
    n = read_all();
    assert(!find(n, 'T', "watchdog bite") && find(n, 'T', "; first SYSTEM_OFF (0x84000008)"));

    // pagani-reset-seq: the steps Android takes first, but for the OEM warm reboot; the watchdog stays held until SYSTEM_OFF came back.
    seq_setup(0x1f, 0);
    park_signalled = 7; park_arrive = 7;
    ramoops_log_start();
    unsigned before = trace_count;
    ramoops_log_power_off("power off");
    static const uint64_t steps[] = {TRACE_HOLD, FN_IO_READ, FN_IO_WRITE, FN_IO_READ, FN_SDI, FN_ARB, TRACE_PARK, FN_OFF, TRACE_RELEASE,
                                     FN_RESET2, FN_RESET};
    expect_trace(before, steps, 11);
    assert(!trace_how_many(FN_OEM) && release_calls == 1);
    n = read_all();
    assert(find(n, 'T', "reset seq 0xf (pagani-reset-seq) reason=power off: dload-clear sdi-off spmi-halt park-cpus"));
    assert(find(n, 'T', "ms; powering off through PSCI") && !find(n, 'U', "OEM warm reboot"));

    // The log off: the same calls, no records.
    fresh();
    seq_expected = 1;
    assert(!ramoops_log_vm_ready());
    ramoops_log_power_off("power off");
    expect_trace(0, power_off + 2, 1);
    assert(trace_how_many(FN_OFF) == 1 && trace_how_many(FN_RESET) == 1 && !maps);
}

int main(int argc, char **argv)
{
    region = malloc(REGION_MAX + 2 * GUARD);
    records = malloc(40000 * sizeof(*records));
    assert(region && records);
    // The record layout and CRC: a fixed record checked against zlib's CRC32 (python3 -c 'import zlib; ...').
    assert(~crc_update(0xffffffffu, (const uint8_t *)"123456789", 9) == 0xcbf43926u);
    test_off();
    test_log();
    test_long_lines();
    test_wrap();
    test_frame_rate();
    test_heartbeat_cpu();
    test_cpu_clock();
    test_guards();
    test_header_stamp();
    test_observer_retry();
    test_backup_timer();
    test_reset_paths();
    test_panic();
    test_headless();
    test_contention();
    // The decoder test's run before the pagani-reset-seq tests: its zone images (run id included: it comes from the counter)
    // stay byte for byte those of the code before pagani-reset-seq.
    if (argc == 3 && !strcmp(argv[1], "--dump")) dump(argv[2]);
    test_reset_seq_all();
    test_reset_seq_single();
    test_reset_seq_failures();
    test_reset_seq_interlock();
    test_reset_seq_irqs();
    test_reset_seq_refused();
    test_reset_seq_once();
    test_reset_seq_paths();
    test_platform_halt();
    fresh();
    free(region); free(records);
    puts("PASS: ramoops records and CRC32, Linux-valid zone headers (start 0, whole zone), 36 identical copies of 0xfffc "
         "(4 + 32) with their layout 3 headers, copy size and limit rules, boot area / events ring / console ring routing, "
         "wrap and overflow, line assembly, status rows, region guards, PSCI SYSTEM_RESET2/SYSTEM_RESET paths, reset timer "
         "and observer fallback, panic and headless panic reset, lock contention, frame-rate F/J records with the slow frame's "
         "breakdown, heartbeat CPU clock (rate arithmetic, best window, window bounds), zone headers stamped again on the heartbeat "
         "and before a reset, the second reset timer (armed once on the observer's first CPU other than the first timer's, "
         "deadline + 3 s, straight to PSCI after the first timer, none without pagani-reset-after); pagani-reset-seq: no SCM "
         "call or CPU park in any scenario without it (or =0), the boot probe (IS_CALL_AVAIL x6, the download mode) and its "
         "screen line, each bit alone, all bits in Linux's order between the reset reason and PSCI with one U record per call "
         "(function id, arguments, x0), INTERRUPTED continuations (x0 = 1, the returned x6), failures and endless INTERRUPTED "
         "logged and passed over, CPUs that park late or never (1 s), the download mode's clear implied by every step that may "
         "make the reset warm and by pagani-reset-cold, those skipped (pagani-reset-cold too) unless it reads back 0, the "
         "watchdog's hold before the steps and its release right before PSCI by the steps' CPU only, interrupts kept masked "
         "to the reset after the CPU park only, masks refused unless written 0x and 1 to 8 hex digits with known bits (XNU's "
         "0X, word, octal, decimal, binary, bare and overlong forms; the last occurrence), once per boot (later triggers, the "
         "observer's retry, a nested resetter that waits 2 s, two racing threads x64), the panic and headless panic paths, the "
         "watchdog-bite path (none), the log off; the platform's restart (its reason in the record, the bite with pagani-reset-wdog) "
         "and power off (SYSTEM_OFF once, after the steps but the OEM reboot, the watchdog held through it, no bite, the reset if it "
         "comes back)");
    return 0;
}
