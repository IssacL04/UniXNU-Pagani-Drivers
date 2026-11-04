// SPDX-License-Identifier: BSD-2-Clause-Patent
// Persistent boot log in the Android ramoops region and the timed warm reset of the photo-free test loop (RamoopsLog.h).
// Linux side (fs/pstore/ram.c, ram_core.c): with record_size 0 and ftrace_size 0 the console zone starts the region and
// the pmsg zone follows it; persistent_ram_post_init keeps a zone whose sig is 0x43474244 and whose header satisfies
// size <= zone bytes - 12 and start <= size, and exposes data[start..size) + data[0..start) as the zone's pstore file.
// Each zone header is written once with start 0 and size the whole zone, so both files show every copy at a fixed offset.
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include "RamoopsLog.h"
#include "GunyahWdog.h"
#include "CpuClock.h"
#include "UfsLog.h"

#if defined(BOOT_PROGRESS_TEST) || defined(RAMOOPS_LOG_TEST)
#define RAMOOPS_HOST 1
#include <stdio.h>
// The host tests supply the platform: the mapping, boot-args, the counter, PSCI, the two reset timers (each returns the CPU it
// is armed on; the second one -1 when the calling CPU is `avoid`) and the CPU clock measurement; they fire the timers themselves.
uintptr_t ramoops_platform_map(uint64_t pa, uint64_t bytes, int device);
int ramoops_platform_arg(const char *name, void *value, unsigned bytes);
const char *ramoops_platform_boot_args(void);
uint64_t ramoops_platform_counter(void);
uint64_t ramoops_platform_frequency(void);
uint32_t ramoops_platform_cpu_mhz(int *cpu);
int64_t ramoops_platform_psci(int hvc, uint64_t function, uint64_t a, uint64_t b, uint64_t c);
int ramoops_platform_psci_method(char method[4]);
int ramoops_platform_arm_timer(uint64_t milliseconds);
int ramoops_platform_arm_backup_timer(uint64_t milliseconds, int avoid);
void ramoops_platform_ram(uint64_t *base, uint64_t *bytes);
void ramoops_log_timer_fired(void);
void ramoops_log_backup_timer_fired(void);
// pagani-reset-seq: the SCM conduit (SMC #0, x0..x7 in and out) and the parking of the other CPUs (returns how many were
// signalled; each one that parks adds 1 to *parked).
void ramoops_platform_smc(uint64_t regs[8]);
unsigned ramoops_platform_park_others(uint32_t *parked);
#else
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>
#include <machine/machine_routines.h>
#include <kern/clock.h>
#include <kern/cpu_number.h>
#include <kern/timer_call.h>
// osfmk/arm/cpu_data_internal.h (not included: its scheduler headers warn under this file's -Weverything recipe): NULL for a CPU
// number without cpu_data. pagani-reset-seq 0x008 only.
extern struct cpu_data *cpu_datap(int cpu);
extern unsigned long gPhysBase, gPhysSize;
extern int vsnprintf(char *, size_t, const char *, va_list);
static void reset_now(const char *reason);

// Normal non-cacheable (write-combined) like Linux's ramoops mem_type 0, or Device-nGnRE (pagani-ramoops bit 2): either
// way the stores bypass the CPU caches and a dsb completes them in DRAM. Nothing in XNU maps this range cacheable.
static uintptr_t ramoops_platform_map(uint64_t pa, uint64_t bytes, int device)
{
    if (device) return (uintptr_t)ml_io_map((vm_offset_t)pa, (vm_size_t)bytes);
    return (uintptr_t)ml_io_map_wcomb((vm_offset_t)pa, (vm_size_t)bytes);
}

static int ramoops_platform_arg(const char *name, void *value, unsigned bytes)
{
    return PE_parse_boot_argn(name, value, (int)bytes) ? 1 : 0;
}

static const char *ramoops_platform_boot_args(void) { return PE_boot_args(); }

static uint64_t ramoops_platform_counter(void)
{
    uint64_t value;
    __asm__ volatile("isb\n\tmrs %0, cntvct_el0" : "=r"(value) :: "memory");
    return value;
}

static uint64_t ramoops_platform_frequency(void)
{
    uint64_t value;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(value));
    return value;
}

// The heartbeat's CPU clock (CpuClock.h, about 1 ms of adds) on the observer's CPU; *cpu is its number, -1 when the thread moved to
// another CPU meanwhile.
static uint32_t ramoops_platform_cpu_mhz(int *cpu)
{
    const int first = cpu_number();
    const uint32_t mhz = cpu_clock_measure(ramoops_platform_counter, ramoops_platform_frequency());
    *cpu = cpu_number() == first ? first : -1;
    return mhz;
}

// The PSCI call of PaganiArmCpu (psciCall): the conduit named by /psci "method"; x1-x3 may come back changed.
static int64_t ramoops_platform_psci(int hvc, uint64_t function, uint64_t a, uint64_t b, uint64_t c)
{
    register uint64_t x0 __asm__("x0") = function;
    register uint64_t x1 __asm__("x1") = a;
    register uint64_t x2 __asm__("x2") = b;
    register uint64_t x3 __asm__("x3") = c;
    if (hvc) {
        __asm__ volatile("hvc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) :
                         : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
    } else {
        __asm__ volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) :
                         : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
    }
    return (int64_t)x0;
}

static int ramoops_platform_psci_method(char method[4])
{
    DTEntry entry = NULL;
    void const *value = NULL;
    unsigned int bytes = 0;
    if (SecureDTLookupEntry(NULL, "/psci", &entry) != kSuccess ||
        SecureDTGetProperty(entry, "method", &value, &bytes) != kSuccess || value == NULL || bytes < 3) return 0;
    const char *text = (const char *)value;
    for (unsigned i = 0; i < 3; i++) method[i] = text[i];
    method[3] = 0;
    return 1;
}

static timer_call_data_t reset_timer, reset_backup_timer;
static void reset_timer_fired(timer_call_param_t param0, timer_call_param_t param1)
{
    (void)param0; (void)param1;
    reset_now("timer");
}

static void reset_backup_fired(timer_call_param_t param0, timer_call_param_t param1)
{
    (void)param0; (void)param1;
    reset_now("backup timer");
}

// A CPU timer (interrupt context, not migrated: TIMER_CALL_LOCAL stays on the entering CPU's queue), so a wedged scheduler does
// not hold the reset back. Interrupts stay off between reading the CPU number and entering the timer; that CPU is returned.
static int reset_timer_enter(timer_call_data_t *timer, timer_call_func_t fired, uint64_t milliseconds)
{
    uint64_t deadline = 0;
    if (milliseconds > UINT32_MAX) milliseconds = UINT32_MAX;
    timer_call_setup(timer, fired, NULL);
    clock_interval_to_deadline((uint32_t)milliseconds, 1000000, &deadline);
    timer_call_enter(timer, deadline, TIMER_CALL_SYS_CRITICAL | TIMER_CALL_LOCAL);
    return cpu_number();
}

// The first timer, from boot_progress_start (bound to the boot CPU).
static int ramoops_platform_arm_timer(uint64_t milliseconds)
{
    const boolean_t enabled = ml_set_interrupts_enabled(FALSE);
    const int cpu = reset_timer_enter(&reset_timer, reset_timer_fired, milliseconds);
    (void)ml_set_interrupts_enabled(enabled);
    return cpu;
}

// The second timer (the audit's G4), from the observer: on the CPU it runs on, unless that is `avoid` (the first timer's CPU).
static int ramoops_platform_arm_backup_timer(uint64_t milliseconds, int avoid)
{
    const boolean_t enabled = ml_set_interrupts_enabled(FALSE);
    const int cpu = cpu_number() == avoid ? -1 : reset_timer_enter(&reset_backup_timer, reset_backup_fired, milliseconds);
    (void)ml_set_interrupts_enabled(enabled);
    return cpu;
}

static void ramoops_platform_ram(uint64_t *base, uint64_t *bytes) { *base = gPhysBase; *bytes = gPhysSize; }

// pagani-reset-seq: SMC #0 with x0..x7 in and out (SMCCC: x8..x17 are not preserved), the conduit the 13T's qcom_scm uses
// (arm_smccc_smc), as Adreno830's Sm8750ScmSmcInstruction. Under Gunyah the HLOS VM's yielding SiP calls reach TZ (the ZAP load works
// on the 13T); that the fast SiP and OEM calls do too is what the 0x100 probe and the U records show.
static void ramoops_platform_smc(uint64_t regs[8])
{
    register uint64_t x0 __asm__("x0") = regs[0];
    register uint64_t x1 __asm__("x1") = regs[1];
    register uint64_t x2 __asm__("x2") = regs[2];
    register uint64_t x3 __asm__("x3") = regs[3];
    register uint64_t x4 __asm__("x4") = regs[4];
    register uint64_t x5 __asm__("x5") = regs[5];
    register uint64_t x6 __asm__("x6") = regs[6];
    register uint64_t x7 __asm__("x7") = regs[7];
    __asm__ volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4), "+r"(x5), "+r"(x6), "+r"(x7) :
                     : "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
    regs[0] = x0; regs[1] = x1; regs[2] = x2; regs[3] = x3;
    regs[4] = x4; regs[5] = x5; regs[6] = x6; regs[7] = x7;
}

// pagani-reset-seq 0x008: what Linux's local_cpu_stop does on the other CPUs after smp_send_stop (interrupts masked, wfe/wfi
// forever; no PSCI CPU_OFF), reached through an XNU cross-call (interrupt context on that CPU; it never returns).
__attribute__((noreturn)) static void reset_park_cpu(void *param)
{
    __asm__ volatile("msr daifset, #15" ::: "memory");
    __atomic_add_fetch((uint32_t *)param, 1, __ATOMIC_RELEASE);
    for (;;) __asm__ volatile("wfe\n\twfi" ::: "memory");
}

// The immediate cross-call (SIGPxcallImm) fails at once when one is already pending on that CPU instead of spinning towards
// cpu_signal_internal's timeout panic, and an offline CPU fails at once too (SIGPdisabled); a CPU without cpu_data (never
// registered) is skipped, since cpu_xcall_internal panics on it. Nothing here waits.
static unsigned ramoops_platform_park_others(uint32_t *parked)
{
    const int self = cpu_number(), max = ml_get_max_cpu_number();
    unsigned signalled = 0;
    for (unsigned n = 0; max >= 0 && n <= (unsigned)max; n++) {     // unsigned: no overflow check (UBSan trap) in a reset path
        const int cpu = (int)n;
        if (cpu == self || !cpu_datap(cpu)) continue;
        if (cpu_immediate_xcall(cpu, reset_park_cpu, parked) == KERN_SUCCESS) signalled++;
    }
    return signalled;
}
#endif

#ifdef RAMOOPS_LOG_TEST
// Test/ramoops.c and Test/crash.c, where GunyahWdog.c is not linked: the watchdog's side of a reset (pagani-reset-wdog's bite, and
// the hold through pagani-reset-seq's steps with its release).
int ramoops_platform_wdog_bite(const char *reason);
int ramoops_platform_wdog_hold(const char *reason);
void ramoops_platform_wdog_release(const char *reason);
void ramoops_platform_wdog_flushed(void);
#else
static int ramoops_platform_wdog_bite(const char *reason) { return gunyah_wdog_bite_reset(reason); }
static int ramoops_platform_wdog_hold(const char *reason) { return gunyah_wdog_hold_for_steps(reason); }
static void ramoops_platform_wdog_release(const char *reason) { gunyah_wdog_release_hold(reason); }
static void ramoops_platform_wdog_flushed(void) { gunyah_wdog_panic_flushed(); }
#endif

#define PSCI_VERSION 0x84000000u
#define PSCI_SYSTEM_OFF 0x84000008u
#define PSCI_SYSTEM_RESET 0x84000009u
#define PSCI_FEATURES 0x8400000au
#define PSCI_SYSTEM_RESET2_64 0xc4000012u   // reset_type 0: SYSTEM_WARM_RESET (architectural), cookie 0
enum {
    LOCK_SPINS = 1u << 18,                  // then drop the record (a CPU stopped inside the lock)
    LOCK_SPINS_SUSPECT = 1u << 10,          // after a drop, until an acquisition succeeds again
    LOCK_SPINS_FORCED = 1u << 21,           // panic and reset: then write anyway
    HEARTBEAT_SECONDS = 10,
    DPU_SECONDS = 10,
    PANIC_TEXT = 2048,
};

// The areas of every copy: the boot area (log header and boot-args, filled once), the events ring (stages, status rows,
// exceptions, panic, resets: low volume, so a console flood cannot push them out) and the console ring.
enum { AREA_BOOT, AREA_EVENTS, AREA_CONSOLE, AREAS };
struct area {
    uint32_t offset, bytes, position;       // within a copy; position: the next write offset inside the area
    int ring;                               // 0: the boot area, whose overflow goes to the events ring
    uint64_t total;                         // bytes ever written here, counted once they are in DRAM (read without the lock)
};

struct line {
    char text[RAMOOPS_LOG_TEXT_MAX];
    unsigned area;
    unsigned length;
    int continued;                          // the next chunk of this line is a '+' record
    uint64_t stamp;                         // counter at the last character
};

struct rowset {
    int valid;
    uint32_t crc, sets, count;
    uint64_t stamp;
};

static volatile int log_enabled;            // read with the MMU off too: plain loads and stores only
static unsigned log_mask;
static volatile uint8_t *copies[RAMOOPS_LOG_MAX_COPIES];
static unsigned copy_count, zone_copies[2];
static uint32_t copy_bytes, zone_size[2];
static uintptr_t zone_base[2];              // the two zones' mapped headers (0 while the log is off)
static struct area areas[AREAS];
static uint8_t copy_header[RAMOOPS_LOG_COPY_HEADER];
static uint32_t sequence, run_id, lock_word, drops, drops_reported, lock_suspect;
static uint64_t start_counter, counter_hz, heartbeat_at;
static uint32_t reset_after_seconds, panic_reset_seconds, reset_cold, reset_wdog;
static int reset_cold_skipped;              // pagani-reset-seq: the download mode did not read back 0, so not without SYSTEM_RESET2
static int reset_power_off;                 // ramoops_log_power_off: SYSTEM_OFF first
static uint64_t reset_deadline;             // counter value; 0: no timed reset
static int reset_armed, reset_started, headless_panic, reset_parked;
static uint64_t reset_started_at;          // counter value when reset_now began (the observer's late retry)
static int reset_begun;                     // reset_now was entered: the stall probe stops (reset_started comes after the UFS log's quiet)
static uint32_t reset_observer_retries;
// The second reset timer (the audit's G4): armed once by the observer on a CPU other than the first timer's, due RESET_BACKUP_SECONDS
// after the deadline, so a wedged boot CPU no longer takes both timers with it. CPU numbers, -1 while unknown or not armed.
enum { RESET_BACKUP_SECONDS = 3 };
static int reset_timer_cpu = -1, reset_backup_cpu = -1, reset_backup_armed;
static char reset_backup_text[160];        // the observer's only
// The last seconds before a timed reset: the GPU drops to its lowest level and bus (the Adreno830 driver's hook; empty without it), so
// the SoC and the DRAM stacked on it cool down first. A hot DRAM lost far more of the log across the reset (23% of the 1 bits after a
// GPU desktop at 734 MHz against about 8% otherwise), and Linux drops a zone whose header lost a single bit.
enum { RESET_PARK_SECONDS = 15 };
void adreno830_dcvs_park(void);
__attribute__((weak)) void adreno830_dcvs_park(void) {}
// The UFS log (UfsLog.c): empty without it, as in the host tests of this file alone.
#ifndef UFS_LOG_TEST
__attribute__((weak)) void ufs_log_start(void) {}
__attribute__((weak)) void ufs_log_panic(uint64_t limit_ms) { (void)limit_ms; }
__attribute__((weak)) void ufs_log_quiesce(const char *reason) { (void)reason; }
#endif
static int psci_resolved, psci_hvc, psci_method_found;
static uint32_t psci_version;
static int32_t psci_reset2_features = -1;
static struct line console_line, panic_line;
static struct rowset rowsets[2];
static uint8_t record[RAMOOPS_LOG_HEADER_BYTES + RAMOOPS_LOG_TEXT_MAX + 1];
static char scratch[512], status[160], footer[80];
static char panic_text[PANIC_TEXT];

static const uint32_t crc_nibble[16] = {
    0x00000000, 0x1db71064, 0x3b6e20c8, 0x26d930ac, 0x76dc4190, 0x6b6b51f4, 0x4db26158, 0x5005713c,
    0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c, 0x9b64c2b0, 0x86d3d2d4, 0xa00ae278, 0xbdbdf21c,
};

// zlib CRC32 when started from 0xffffffff and inverted at the end.
static uint32_t crc_update(uint32_t crc, const uint8_t *bytes, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        crc ^= bytes[i];
        crc = (crc >> 4) ^ crc_nibble[crc & 15];
        crc = (crc >> 4) ^ crc_nibble[crc & 15];
    }
    return crc;
}

#ifdef RAMOOPS_HOST
// The host tests' stand-in for PSTATE.DAIF, per thread: irq_off masks, irq_restore_flags puts the saved state back.
static _Thread_local int host_daif_masked;
#endif

static uint64_t irq_off(void)
{
    uint64_t flags = 0;
#ifndef RAMOOPS_HOST
    __asm__ volatile("mrs %0, daif\n\tmsr daifset, #15" : "=r"(flags) :: "memory");
#else
    flags = (uint64_t)host_daif_masked;
    host_daif_masked = 1;
#endif
    return flags;
}

static void irq_restore_flags(uint64_t flags)
{
#ifndef RAMOOPS_HOST
    __asm__ volatile("msr daif, %0" :: "r"(flags) : "memory");
#else
    host_daif_masked = (int)flags;
#endif
}

// Completes the write-combined stores: a record is in DRAM before the caller goes on (and before a reset).
static void complete_stores(void)
{
#ifndef RAMOOPS_HOST
    __asm__ volatile("dsb sy" ::: "memory");
#else
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
#endif
}

// Orders the record's bytes before the header that publishes them (write-combined stores may otherwise be reordered).
static void order_stores(void)
{
#ifndef RAMOOPS_HOST
    __asm__ volatile("dmb st" ::: "memory");
#else
    __atomic_thread_fence(__ATOMIC_RELEASE);
#endif
}

static void relax(void)
{
#ifndef RAMOOPS_HOST
    __asm__ volatile("yield" ::: "memory");
#endif
}

// Parks the CPU until the reset lands. Like BootProgress's panic_halt it waits on a flag nothing sets instead of being
// noreturn: the host tests return from here.
static volatile int halt_released;
static void halt(void)
{
#ifndef RAMOOPS_HOST
    while (!halt_released) __asm__ volatile("wfe");
#endif
}

static int log_on(void) { return log_enabled; }

static uint64_t log_counter(void);
// After SYSTEM_RESET returned: the Gunyah watchdog, disabled before the reset, is enabled again with its shortest bite and no pets,
// and SYSTEM_RESET is retried once a second meanwhile, so a refused reset no longer parks the phone for good (2026-10-30 18:35). The
// observer's own fallback cannot step in any more (reset_started is set); other CPUs still come in through the observer's late
// retry (G3) and the backup timer (G4), whose reset_now ends here too.
static void halt(void);
static void reset_retry(void)
{
#ifndef RAMOOPS_HOST
    gunyah_wdog_after_failed_reset();
    while (counter_hz) {
        const uint64_t until = log_counter() + counter_hz;
        while (log_counter() < until) __asm__ volatile("yield");
        ramoops_platform_psci(psci_hvc, PSCI_SYSTEM_RESET, 0, 0, 0);
    }
#endif
    halt();                             // no counter to pace the retries (or the host tests): park as before
}

static uint64_t log_counter(void) { return ramoops_platform_counter(); }

#define LOCK_FAILED UINT64_MAX
static uint64_t lock_log(int forced)
{
    uint64_t flags = irq_off();
    uint32_t limit = forced ? (uint32_t)LOCK_SPINS_FORCED :
                     __atomic_load_n(&lock_suspect, __ATOMIC_RELAXED) ? (uint32_t)LOCK_SPINS_SUSPECT : (uint32_t)LOCK_SPINS;
    for (uint32_t spins = 0;; spins++) {
        if (!__atomic_exchange_n(&lock_word, 1, __ATOMIC_ACQUIRE)) {
            __atomic_store_n(&lock_suspect, 0, __ATOMIC_RELAXED);
            return flags;
        }
        if (spins >= limit) {
            if (forced) return flags;       // a stopped holder must not hold back a panic or a reset
            irq_restore_flags(flags);
            __atomic_fetch_add(&drops, 1, __ATOMIC_RELAXED);
            __atomic_store_n(&lock_suspect, 1, __ATOMIC_RELAXED);   // later writers give up quickly
            return LOCK_FAILED;
        }
        relax();
    }
}

static void unlock_log(uint64_t flags)
{
    __atomic_store_n(&lock_word, 0, __ATOMIC_RELEASE);
    irq_restore_flags(flags);
}

static void hex_digits(uint8_t *to, uint64_t value, unsigned digits)
{
    while (digits) {
        to[--digits] = (uint8_t)"0123456789abcdef"[value & 15];
        value >>= 4;
    }
}

static uint8_t printable(char c)
{
    unsigned char u = (unsigned char)c;
    if (u == '\t') return ' ';
    return u >= 0x20 && u <= 0x7e ? u : '?';
}

static uint64_t elapsed_ms(void)
{
    uint64_t now = log_counter();
    return counter_hz && now > start_counter ? (now - start_counter) * 1000 / counter_hz : 0;
}

// A zone: cleared, then the Linux header. zones_stamp writes the same three words again (the values never change, so a reset in
// the middle cannot tear them).
static void zone_init(uintptr_t base, uint32_t bytes)
{
    volatile uint64_t *words = (volatile uint64_t *)base;
    for (uint32_t i = 0; i < bytes / 8; i++) words[i] = 0;
    volatile uint32_t *header = (volatile uint32_t *)base;
    header[1] = 0;
    header[2] = bytes - RAMOOPS_LOG_ZONE_HEADER;
    order_stores();
    header[0] = RAMOOPS_LOG_SIG;
}

// The Linux headers again, on every heartbeat and before a reset (harmless; it did not help): on the 13T Linux keeps finding the
// pmsg zone's size word (0x880040008, written as 0x001ffff4) as 18807764 or 18283476 — bit 24 set, bits 5, 10 and 16 clear — in
// most runs since 2026-10-30 11:49 whatever the kernel: weak DRAM cells losing their charge across the reset, after the last
// write here (audit/judge.md). The console zone usually keeps a valid header (its weak bits only shrink the size).
static void zones_stamp(void)
{
    for (unsigned z = 0; z < 2; z++) {
        if (!zone_base[z]) continue;
        volatile uint32_t *header = (volatile uint32_t *)zone_base[z];
        header[1] = 0;
        header[2] = zone_size[z] - RAMOOPS_LOG_ZONE_HEADER;
        order_stores();
        header[0] = RAMOOPS_LOG_SIG;
    }
    complete_stores();
}

static void store32(uint8_t *to, uint32_t value)
{
    for (unsigned i = 0; i < 4; i++) to[i] = (uint8_t)(value >> (8 * i));
}

// Our own header at the start of every copy: magic, version and copy number, so a reader aligns and checks the copies
// without Linux's start/size. The CRC leaves out the copy number, so the copies' headers can be merged bit by bit.
static void copy_headers(uint64_t pa, uint32_t console, uint32_t pmsg)
{
    uint8_t *h = copy_header;
    for (unsigned i = 0; i < RAMOOPS_LOG_COPY_HEADER; i++) h[i] = 0;
    store32(h + 0, RAMOOPS_LOG_MAGIC0);
    store32(h + 4, RAMOOPS_LOG_MAGIC1);
    store32(h + 8, RAMOOPS_LOG_VERSION);
    store32(h + 16, copy_count);
    store32(h + 20, run_id);
    store32(h + 24, copy_bytes);
    store32(h + 28, areas[AREA_EVENTS].offset);
    store32(h + 32, areas[AREA_EVENTS].bytes);
    store32(h + 36, areas[AREA_CONSOLE].bytes);
    store32(h + 40, console);
    store32(h + 44, pmsg);
    store32(h + 48, (uint32_t)pa);
    store32(h + 52, (uint32_t)(pa >> 32));
    store32(h + 56, zone_copies[0] | zone_copies[1] << 16);
    store32(h + 60, ~crc_update(crc_update(0xffffffffu, h, 12), h + 16, 44));
    for (unsigned c = 0; c < copy_count; c++) {
        store32(h + 12, c);
        for (unsigned i = 0; i < RAMOOPS_LOG_COPY_HEADER; i++) copies[c][i] = h[i];
    }
    store32(h + 12, 0);
}

// Where a record goes: the log header to the boot area, console lines, the FPS lines and the per-process exec/exit stages
// (bulk) to the console ring, everything else to the events ring.
static unsigned area_of(char kind, const char *text)
{
    if (kind == RAMOOPS_KIND_BOOT || kind == RAMOOPS_KIND_ARGS) return AREA_BOOT;
    // The GPU status rows come four a second: in the events ring (32 KiB) they pushed the boot's one-time U lines (the staged apps
    // copy, the overlays, the audio service) out after about 80 s (2026-10-30 21:33 run), so they share the console ring.
    if (kind == RAMOOPS_KIND_CONSOLE || kind == RAMOOPS_KIND_FPS || kind == RAMOOPS_KIND_GPU || kind == RAMOOPS_KIND_METER) return AREA_CONSOLE;
    if (kind == RAMOOPS_KIND_STAGE && text) {
        const char *p = text[0] == 'e' && text[1] == 'x' && text[2] == 'i' && text[3] == 't' && text[4] == ' ' ? text + 5 : text;
        if (p[0] == 'p' && p[1] == 'i' && p[2] == 'd' && p[3] == '=') return AREA_CONSOLE;
    }
    return AREA_EVENTS;
}

// One record into the same offset of every copy. Under the lock (or forced).
static void emit(unsigned which, char kind, const char *text, unsigned length)
{
    if (length > RAMOOPS_LOG_TEXT_MAX) length = RAMOOPS_LOG_TEXT_MAX;
    uint8_t *r = record;
    r[0] = '#';
    hex_digits(r + 1, ++sequence, 8);
    r[9] = ' ';
    hex_digits(r + 10, elapsed_ms(), 8);
    r[18] = ' ';
    hex_digits(r + 19, length, 2);
    r[21] = ' ';
    r[30] = ' ';
    r[31] = printable(kind);
    r[32] = ' ';
    for (unsigned i = 0; i < length; i++) r[RAMOOPS_LOG_HEADER_BYTES + i] = printable(text[i]);
    r[RAMOOPS_LOG_HEADER_BYTES + length] = '\n';
    uint32_t crc = crc_update(0xffffffffu, r, 22);
    crc = ~crc_update(crc, r + 30, 3 + length);
    hex_digits(r + 22, crc, 8);
    uint32_t n = RAMOOPS_LOG_HEADER_BYTES + length + 1;
    struct area *a = &areas[which];
    if (!a->ring && a->position + n > a->bytes) a = &areas[AREA_EVENTS];
    for (unsigned c = 0; c < copy_count; c++) {
        volatile uint8_t *base = copies[c] + a->offset;
        uint32_t at = a->position;
        for (uint32_t i = 0; i < n; i++) {
            base[at] = r[i];
            if (++at == a->bytes) at = 0;
        }
    }
    a->position = a->ring ? (a->position + n) % a->bytes : a->position + n;
    complete_stores();
    __atomic_store_n(&a->total, a->total + n, __ATOMIC_RELEASE);
}

// Text with newlines: one record per line, long lines continued in '+' records.
static void emit_text(char kind, const char *text)
{
    const char *p = text ? text : "";
    unsigned which = area_of(kind, p);
    do {
        unsigned n = 0, offset = 0;
        char k = kind;
        while (p[n] && p[n] != '\n') n++;
        do {
            unsigned chunk = n - offset > RAMOOPS_LOG_TEXT_MAX ? RAMOOPS_LOG_TEXT_MAX : n - offset;
            emit(which, k, p + offset, chunk);
            offset += chunk;
            k = RAMOOPS_KIND_MORE;
        } while (offset < n);
        p += n;
        if (*p == '\n') p++;
    } while (*p);
}

static void line_put(struct line *l, char kind, char c)
{
    if (c == '\r') return;
    if (c == '\n') {
        if (l->length) emit(l->area, l->continued ? RAMOOPS_KIND_MORE : kind, l->text, l->length);
        l->length = 0;
        l->continued = 0;
        return;
    }
    if (l->length == RAMOOPS_LOG_TEXT_MAX) {
        emit(l->area, l->continued ? RAMOOPS_KIND_MORE : kind, l->text, l->length);
        l->length = 0;
        l->continued = 1;
    }
    l->text[l->length++] = c;
    l->stamp = log_counter();
}

// A partial line goes out now (a hang may follow); the rest of it continues in '+' records.
static void line_flush(struct line *l, char kind)
{
    if (!l->length) return;
    emit(l->area, l->continued ? RAMOOPS_KIND_MORE : kind, l->text, l->length);
    l->length = 0;
    l->continued = 1;
}

struct text {
    char *at;
    unsigned used, room;
};

static void text_start(struct text *t, char *buffer, unsigned room)
{
    t->at = buffer;
    t->used = 0;
    t->room = room;
    buffer[0] = 0;
}

static void put_char(struct text *t, char c)
{
    if (t->used + 1 >= t->room) return;
    t->at[t->used++] = c;
    t->at[t->used] = 0;
}

static void put_str(struct text *t, const char *s)
{
    while (s && *s) put_char(t, *s++);
}

static void put_hex(struct text *t, uint64_t value)
{
    uint8_t digits[16];
    unsigned n = 0;
    put_str(t, "0x");
    do { digits[n++] = (uint8_t)"0123456789abcdef"[value & 15]; value >>= 4; } while (value);
    while (n) put_char(t, (char)digits[--n]);
}

static void put_dec(struct text *t, uint64_t value)
{
    uint8_t digits[20];
    unsigned n = 0;
    do { digits[n++] = (uint8_t)('0' + value % 10); value /= 10; } while (value);
    while (n) put_char(t, (char)digits[--n]);
}

static unsigned bounded_length(const char *s, unsigned limit)
{
    unsigned n = 0;
    while (s && n < limit && s[n]) n++;
    return n;
}

static uint32_t floor_power_of_two(uint32_t value)
{
    uint32_t result = 1;
    if (!value) return 0;
    while (value >>= 1) result <<= 1;
    return result;
}

// PSCI facts, once: the conduit from /psci "method" (the DT ArmCpu reads; SMC when absent, as ArmDTPlatform's stage reset),
// the version, and PSCI_FEATURES(SYSTEM_RESET2) the way Linux probes it (PSCI 1.0 and later).
static void psci_resolve(void)
{
    if (__atomic_load_n(&psci_resolved, __ATOMIC_ACQUIRE)) return;
    char method[4] = {0, 0, 0, 0};
    psci_method_found = ramoops_platform_psci_method(method);
    psci_hvc = psci_method_found && method[0] == 'h' && method[1] == 'v' && method[2] == 'c';
    psci_version = (uint32_t)ramoops_platform_psci(psci_hvc, PSCI_VERSION, 0, 0, 0);
    psci_reset2_features = -1;
    if ((psci_version >> 16) >= 1 && psci_version != UINT32_MAX)
        psci_reset2_features = (int32_t)(uint32_t)ramoops_platform_psci(psci_hvc, PSCI_FEATURES, PSCI_SYSTEM_RESET2_64, 0, 0);
    __atomic_store_n(&psci_resolved, 1, __ATOMIC_RELEASE);
}

static int psci_reset2(void)
{
    return (!reset_cold || reset_cold_skipped) && (psci_version >> 16) >= 1 && psci_version != UINT32_MAX &&
           psci_reset2_features >= 0;
}

static void put_psci(struct text *t)
{
    put_str(t, "psci=");
    if (psci_version == UINT32_MAX) put_str(t, "none");
    else { put_dec(t, psci_version >> 16); put_char(t, '.'); put_dec(t, psci_version & 0xffff); }
    put_str(t, psci_hvc ? " conduit=hvc" : " conduit=smc");
    put_str(t, psci_method_found ? "(/psci)" : "(default)");
    put_str(t, " SYSTEM_RESET2 features=");
    if (psci_reset2_features < 0) { put_char(t, '-'); put_dec(t, (uint64_t)(-(int64_t)psci_reset2_features)); }
    else put_hex(t, (uint32_t)psci_reset2_features);
    put_str(t, psci_reset2() ? " path=SYSTEM_RESET2 warm (0xc4000012 type 0)" : " path=SYSTEM_RESET (0x84000009)");
    if (reset_cold) put_str(t, " (pagani-reset-cold)");
}

static void put_result(struct text *t, int64_t result)
{
    int32_t value = (int32_t)(uint32_t)result;
    if (value < 0) { put_char(t, '-'); put_dec(t, (uint64_t)(-(int64_t)value)); }
    else put_dec(t, (uint64_t)value);
}

// ---- pagani-reset-seq (RamoopsLog.h) ---------------------------------------------------------------------------------------
// The calls as the 13T's own kernel makes them (OnePlus sm8750 6.6: drivers/firmware/qcom_scm.c, qcom_scm-smc.c): fast calls,
// SMC64, x0 the function id, x1 the arginfo (QCOM_SCM_ARGS(n) = n for value arguments), x2.. the arguments. IS_CALL_AVAIL is a
// standard (yielding) call; only the probe at boot makes it (thread context, interrupts enabled).
#define RESET_SEQ_FN_SDI_OFF 0xc2000109u        // SiP BOOT 0x01 / WDOG_DEBUG_PART 0x09 (qcom_scm_disable_sdi): (1, 0)
#define RESET_SEQ_FN_IO_READ 0xc2000501u        // SiP IO 0x05 / READ 0x01 (qcom_scm_io_readl): (address), the value in x1
#define RESET_SEQ_FN_IO_WRITE 0xc2000502u       // SiP IO 0x05 / WRITE 0x02 (qcom_scm_io_writel): (address, value)
#define RESET_SEQ_FN_ARB_HALT 0xc2000901u       // SiP PWR 0x09 / IO_DISABLE_PMIC_ARBITER 0x01 (qcom_scm_halt_spmi_pmic_arbiter): (0)
#define RESET_SEQ_FN_OEM_REBOOT 0xc3000922u     // OEM OEM_POWER 0x09 / OEM_POWER_REBOOT 0x22 (qcom_scm_reboot): no arguments
#define RESET_SEQ_FN_IS_AVAIL 0x42000601u       // SiP INFO 0x06 / IS_CALL_AVAIL 0x01, standard: (owner << 24 | svc << 8 | cmd)
#define RESET_SEQ_DLOAD_PA 0x1fd9000u           // the download mode: TCSR 0x1fc0000 + 0x19000 (sun.dtsi qcom_scm qcom,dload-mode)
enum {
    RESET_SEQ_IDLE, RESET_SEQ_RUNNING, RESET_SEQ_DONE,
    RESET_SEQ_RESUMES = 64,                     // SMCs per call, INTERRUPTED continuations included
    RESET_SEQ_PARK_MS = 1000,                   // the wait for the parked CPUs (smp_send_stop waits 1 s too)
    RESET_SEQ_WAIT_SECONDS = 2,                 // a second resetter's wait for the steps
    RESET_SEQ_PROBES = 6,
};
static uint32_t reset_seq_mask, reset_cpus_parked;
static uint32_t reset_seq_refused;          // the parsed value of a refused pagani-reset-seq (S5 below); its mask stays 0
static int reset_seq_state, reset_seq_probed;
static char reset_seq_text[RAMOOPS_LOG_TEXT_MAX + 1], reset_seq_screen[160];
// What the probe asks IS_CALL_AVAIL about: the SiP calls above, SET_DLOAD_MODE (0x0110, Linux's fallback without a dload
// address) and the OEM reboot (the owner field says OEM; Linux never asks about it).
static const uint32_t reset_seq_probe_ids[RESET_SEQ_PROBES] = {0x02000109, 0x02000110, 0x02000501, 0x02000502, 0x02000901, 0x03000922};
static const char reset_seq_probe_names[RESET_SEQ_PROBES][5] = {"sdi", "dl", "rd", "wr", "spmi", "oem"};

struct scm_answer {
    int64_t x0;
    uint64_t x1;
    uint32_t smcs;
};

// One call as Linux's __scm_smc_do_quirk makes it: while x0 comes back INTERRUPTED (1) the same registers go out again with x0 = 1
// and the returned x6 (at most RESET_SEQ_RESUMES SMCs in all).
static void reset_seq_scm(uint64_t function, uint64_t arginfo, uint64_t a, uint64_t b, struct scm_answer *answer)
{
    uint64_t x0 = function, x6 = 0;
    answer->smcs = 0;
    for (;;) {
        uint64_t regs[8];
        regs[0] = x0; regs[1] = arginfo; regs[2] = a; regs[3] = b;
        regs[4] = 0; regs[5] = 0; regs[6] = x6; regs[7] = 0;
        ramoops_platform_smc(regs);
        answer->smcs++;
        answer->x0 = (int64_t)regs[0];
        answer->x1 = regs[1];
        if (regs[0] != 1 || answer->smcs >= RESET_SEQ_RESUMES) return;
        x0 = 1;
        x6 = regs[6];
    }
}

// The reset path's records take the lock by force (a stopped holder must not hold back a reset).
static void reset_seq_record(char kind, const char *text)
{
    if (!log_on()) return;
    uint64_t flags = lock_log(1);
    emit_text(kind, text);
    unlock_log(flags);
}

// One call and its U record "<label> SCM 0x<function>(<arguments>) = <x0>[ x1 0x<x1>]" (x0 as Linux reads it: its low 32 bits,
// signed); returns x0, *x1 the returned x1.
static int64_t reset_seq_call(const char *label, uint64_t function, unsigned args, uint64_t a, uint64_t b, int show_x1,
                              uint64_t *x1)
{
    struct scm_answer answer;
    struct text t;
    reset_seq_scm(function, args, a, b, &answer);
    text_start(&t, reset_seq_text, sizeof(reset_seq_text));
    put_str(&t, label);
    put_str(&t, " SCM ");
    put_hex(&t, function);
    put_char(&t, '(');
    if (args > 0) put_hex(&t, a);
    if (args > 1) { put_str(&t, ", "); put_hex(&t, b); }
    put_str(&t, ") = ");
    put_result(&t, answer.x0);
    if (answer.x0 == 1) { put_str(&t, " (still INTERRUPTED after "); put_dec(&t, answer.smcs); put_str(&t, " SMCs)"); }
    if (show_x1 && !answer.x0) { put_str(&t, " x1 "); put_hex(&t, answer.x1); }
    reset_seq_record(RAMOOPS_KIND_TRACE, reset_seq_text);
    if (x1) *x1 = answer.x1;
    return answer.x0;
}

// 0x100 at boot (ramoops_log_start: thread context, interrupts enabled, before any driver makes SCM calls): IS_CALL_AVAIL for each
// call and the download mode, one U record each, and the screen line. Changes nothing.
static void reset_seq_probe_boot(void)
{
    struct text s;
    uint64_t x1 = 0;
    text_start(&s, reset_seq_screen, sizeof(reset_seq_screen));
    put_str(&s, "reset seq ");                  // at most 79 characters for -1 answers (a BootProgress stage line)
    put_hex(&s, reset_seq_mask);
    put_str(&s, " avail");
    for (unsigned i = 0; i < RESET_SEQ_PROBES; i++) {
        int64_t x0 = reset_seq_call("reset seq probe: avail", RESET_SEQ_FN_IS_AVAIL, 1, reset_seq_probe_ids[i], 0, 1, &x1);
        put_char(&s, ' ');
        put_str(&s, reset_seq_probe_names[i]);
        put_char(&s, ' ');
        if (x0) { put_char(&s, 'e'); put_result(&s, x0); }
        else put_dec(&s, x1 != 0);
    }
    int64_t x0 = reset_seq_call("reset seq probe: dload read", RESET_SEQ_FN_IO_READ, 1, RESET_SEQ_DLOAD_PA, 0, 1, &x1);
    put_str(&s, " dload ");
    if (x0) { put_char(&s, 'e'); put_result(&s, x0); }
    else put_hex(&s, (uint32_t)x1);
}

const char *ramoops_reset_seq_status(void) { return reset_seq_screen[0] ? reset_seq_screen : NULL; }

// 0x008: park the other CPUs, then wait at most RESET_SEQ_PARK_MS for them; one U record.
static void reset_seq_park(void)
{
    struct text t;
    const uint64_t start = log_counter(), limit = (uint64_t)RESET_SEQ_PARK_MS * counter_hz / 1000;
    const unsigned signalled = ramoops_platform_park_others(&reset_cpus_parked);
    uint32_t parked = __atomic_load_n(&reset_cpus_parked, __ATOMIC_ACQUIRE);
    while (parked < signalled && log_counter() - start < limit) {
        relax();
        parked = __atomic_load_n(&reset_cpus_parked, __ATOMIC_ACQUIRE);
    }
    text_start(&t, reset_seq_text, sizeof(reset_seq_text));
    put_str(&t, "reset seq: park CPUs: ");
    put_dec(&t, parked);
    put_str(&t, " of ");
    put_dec(&t, signalled);
    put_str(&t, " signalled CPUs parked in ");
    put_dec(&t, counter_hz ? (log_counter() - start) * 1000 / counter_hz : 0);
    put_str(&t, parked < signalled ? " ms (gave up after 1 s)" : " ms");
    reset_seq_record(RAMOOPS_KIND_TRACE, reset_seq_text);
}

// The names of the steps in mask (and pagani-reset-cold when cold is set), each after a space.
static void put_seq_steps(struct text *t, uint32_t mask, int cold)
{
    if (mask & RAMOOPS_RESET_SEQ_SDI) put_str(t, " sdi-off");
    if (mask & RAMOOPS_RESET_SEQ_SPMI) put_str(t, " spmi-halt");
    if (mask & RAMOOPS_RESET_SEQ_CPUS) put_str(t, " park-cpus");
    if (mask & RAMOOPS_RESET_SEQ_OEM) put_str(t, " oem-warm-reboot");
    if (cold) put_str(t, " pagani-reset-cold");
}

// The steps, in Linux's order: the download mode (reboot notifier), SDI and the SPMI arbiter (qcom_scm_shutdown), the other
// CPUs (machine_restart's smp_send_stop), the OEM warm reboot (qcom_scm_do_restart for reboot_mode warm). Interrupts stay off
// on this CPU meanwhile, as in machine_restart; the caller's state comes back for the PSCI reset unless 0x008 parked the other CPUs.
static void reset_seq_run(const char *reason)
{
    // A power off takes the steps Android takes before it too, but for the OEM warm reboot, which would restart the phone instead.
    const int power_off = __atomic_load_n(&reset_power_off, __ATOMIC_ACQUIRE);
    const uint32_t mask = power_off ? reset_seq_mask & ~(uint32_t)RAMOOPS_RESET_SEQ_OEM : reset_seq_mask;
    const uint64_t began = log_counter();
    // F1 of the 2026-10-31 review: a warm reset with the download mode set ends in the crash dump (XBL waits for a USB host, or a
    // mini dump goes to the phone's storage), and Android clears the mode before every reboot (13T qcom_dload_reboot: NODUMP first,
    // whatever the command). So a step that may make the reset warm, and pagani-reset-cold (the reset without the failing
    // SYSTEM_RESET2, as Android's), come with the clear and run only when the mode then reads back 0.
    const int warm = (mask & RAMOOPS_RESET_SEQ_WARM) || reset_cold;
    const int implied = warm && !(mask & RAMOOPS_RESET_SEQ_DLOAD);
    struct text t;
    text_start(&t, reset_seq_text, sizeof(reset_seq_text));
    put_str(&t, "reset seq ");
    put_hex(&t, mask);
    put_str(&t, " (pagani-reset-seq) reason=");
    put_str(&t, reason);
    put_char(&t, ':');
    if (mask & RAMOOPS_RESET_SEQ_PROBE) put_str(&t, " probe");
    if (mask & RAMOOPS_RESET_SEQ_DLOAD) put_str(&t, " dload-clear");
    if (implied) put_str(&t, " dload-clear(implied)");
    put_seq_steps(&t, mask, 0);
    reset_seq_record(RAMOOPS_KIND_RESET, reset_seq_text);
    const uint64_t flags = irq_off();
    uint32_t steps = mask;
    if ((mask & RAMOOPS_RESET_SEQ_PROBE) && !(mask & RAMOOPS_RESET_SEQ_DLOAD) && !implied)
        (void)reset_seq_call("reset seq: dload read", RESET_SEQ_FN_IO_READ, 1, RESET_SEQ_DLOAD_PA, 0, 1, NULL);
    if ((mask & RAMOOPS_RESET_SEQ_DLOAD) || implied) {
        uint64_t x1 = 0;
        (void)reset_seq_call(implied ? "reset seq: dload read (implied)" : "reset seq: dload read", RESET_SEQ_FN_IO_READ, 1,
                             RESET_SEQ_DLOAD_PA, 0, 1, NULL);
        (void)reset_seq_call(implied ? "reset seq: dload clear (implied)" : "reset seq: dload clear", RESET_SEQ_FN_IO_WRITE, 2,
                             RESET_SEQ_DLOAD_PA, 0, 0, NULL);
        const int64_t x0 = reset_seq_call(implied ? "reset seq: dload read back (implied)" : "reset seq: dload read back",
                                          RESET_SEQ_FN_IO_READ, 1, RESET_SEQ_DLOAD_PA, 0, 1, &x1);
        // Judged as Linux reads them (qcom_scm_io_readl): x0 on its low 32 bits, the register's 32 bits in x1.
        if (warm && ((uint32_t)x0 || (uint32_t)x1)) {
            // Not cleared (the write refused, the read failed or INTERRUPTED to the end): the reset goes as without these.
            steps &= ~(uint32_t)RAMOOPS_RESET_SEQ_WARM;
            reset_cold_skipped = reset_cold != 0;
            text_start(&t, reset_seq_text, sizeof(reset_seq_text));
            put_str(&t, "reset seq: the download mode reads back ");
            if ((uint32_t)x0) { put_str(&t, "x0 "); put_result(&t, x0); }
            else put_hex(&t, (uint32_t)x1);
            put_str(&t, ", not 0:");
            put_seq_steps(&t, mask, reset_cold != 0);
            put_str(&t, " skipped (a warm reset would enter the crash dump)");
            reset_seq_record(RAMOOPS_KIND_RESET, reset_seq_text);
        }
    }
    if (steps & RAMOOPS_RESET_SEQ_SDI) (void)reset_seq_call("reset seq: SDI off", RESET_SEQ_FN_SDI_OFF, 2, 1, 0, 0, NULL);
    if (steps & RAMOOPS_RESET_SEQ_SPMI)
        (void)reset_seq_call("reset seq: SPMI arbiter halt", RESET_SEQ_FN_ARB_HALT, 1, 0, 0, 0, NULL);
    if (steps & RAMOOPS_RESET_SEQ_CPUS) reset_seq_park();
    if (steps & RAMOOPS_RESET_SEQ_OEM) {
        // Expected not to return: the record before it shows the call went out.
        reset_seq_record(RAMOOPS_KIND_TRACE, "reset seq: OEM warm reboot SCM 0xc3000922() next");
        (void)reset_seq_call("reset seq: OEM warm reboot returned:", RESET_SEQ_FN_OEM_REBOOT, 0, 0, 0, 0, NULL);
    }
    // S4 of the review: with the other CPUs parked, interrupts stay masked here up to the reset (and through reset_retry), as Linux's
    // machine_restart keeps them from local_irq_disable on; the observer's thread, preempted here, could hand this CPU to a thread
    // that spins on a lock a parked CPU holds.
    if (!(steps & RAMOOPS_RESET_SEQ_CPUS)) irq_restore_flags(flags);
    text_start(&t, reset_seq_text, sizeof(reset_seq_text));
    put_str(&t, "reset seq done in ");
    put_dec(&t, counter_hz ? (log_counter() - began) * 1000 / counter_hz : 0);
    put_str(&t, power_off ? " ms; powering off through PSCI" : " ms; resetting through PSCI");
    reset_seq_record(RAMOOPS_KIND_RESET, reset_seq_text);
}

// From reset_now, with pagani-reset-seq set: the first resetter runs the steps (it claimed them right after reset_started), a
// later one waits at most RESET_SEQ_WAIT_SECONDS while they run; the watchdog-bite path applies none.
static void reset_seq(const char *reason, int first, int bite)
{
    if (!first) {
        const uint64_t start = log_counter(), limit = (uint64_t)RESET_SEQ_WAIT_SECONDS * counter_hz;
        while (__atomic_load_n(&reset_seq_state, __ATOMIC_ACQUIRE) == RESET_SEQ_RUNNING && log_counter() - start < limit) relax();
        return;
    }
    if (bite) {
        struct text t;
        text_start(&t, reset_seq_text, sizeof(reset_seq_text));
        put_str(&t, "reset seq ");
        put_hex(&t, reset_seq_mask);
        put_str(&t, " not applied: the watchdog bite resets (pagani-reset-wdog)");
        reset_seq_record(RAMOOPS_KIND_RESET, reset_seq_text);
        return;
    }
    reset_seq_run(reason);
    // The watchdog held through the steps (reset_now) goes off now, right before PSCI, as for a reset without them; a power off keeps
    // it armed through SYSTEM_OFF.
    if (!__atomic_load_n(&reset_power_off, __ATOMIC_ACQUIRE)) ramoops_platform_wdog_release(reason);
    __atomic_store_n(&reset_seq_state, RESET_SEQ_DONE, __ATOMIC_RELEASE);
}

// Only the first resetting CPU builds text here, so it needs no lock of its own.
static char reset_text[200];

static void log_reset_text(void)
{
    if (!log_on()) return;
    uint64_t flags = lock_log(1);
    emit_text(RAMOOPS_KIND_RESET, reset_text);
    unlock_log(flags);
}

// Flushes, records the path, resets. Never returns on the device. A second caller (another CPU, or a fault on the way)
// goes straight to PSCI, once it is past the UFS log's quiet.
static void reset_now(const char *reason)
{
    const int power_off = __atomic_load_n(&reset_power_off, __ATOMIC_ACQUIRE);
    __atomic_store_n(&reset_begun, 1, __ATOMIC_RELEASE);
    // sheng-log=1: first of all no log command stays on the wire (UfsLog.h), before anything below starts a clock towards the reset. Bounded,
    // and a second resetter waits for it here; interrupts stay off for it, as in a panic.
    const uint64_t irqs = irq_off();
    ufs_log_quiesce(reason);
    irq_restore_flags(irqs);
    // pagani-reset-wdog=1 with pagani-wdog: the watchdog's bite resets (it kept the log where SYSTEM_RESET flipped weak DRAM cells);
    // PSCI only if no bite came within 10 s. Otherwise no watchdog bite races the PSCI reset (GunyahWdog.h). A power off never takes
    // the bite, which would restart the phone.
    int bite = 0, held = 0;
    if (reset_wdog && !power_off) bite = ramoops_platform_wdog_bite(reason);
    // pagani-reset-seq: the watchdog stays armed through the steps instead, with its shortest bite from a pet now, so a step that
    // never returns still ends in a bite (S3 of the 2026-10-31 review: disabled here first, such a step left only a manual reset);
    // reset_seq disables it after them. A power off holds it the same way, through SYSTEM_OFF too: a firmware that neither powers off
    // nor answers still ends in the bite.
    if (!bite && (reset_seq_mask || power_off)) held = ramoops_platform_wdog_hold(reason);
#ifndef RAMOOPS_LOG_TEST
    if (!bite && !held) gunyah_wdog_before_reset(reason);
#else
    (void)held;
#endif
    int first = !__atomic_exchange_n(&reset_started, 1, __ATOMIC_ACQ_REL);
    if (first) __atomic_store_n(&reset_started_at, log_counter(), __ATOMIC_RELEASE);
    // pagani-reset-seq: the first resetter claims the steps at once, so a second one waits for them (reset_seq).
    if (first && reset_seq_mask && !bite) __atomic_store_n(&reset_seq_state, RESET_SEQ_RUNNING, __ATOMIC_RELEASE);
    struct text t;
    psci_resolve();
    if (first && log_on()) {
        uint64_t flags = lock_log(1);
        line_flush(&console_line, RAMOOPS_KIND_CONSOLE);
        line_flush(&panic_line, RAMOOPS_KIND_PANIC);
        text_start(&t, reset_text, sizeof(reset_text));
        put_str(&t, "reset reason=");
        put_str(&t, reason);
        put_str(&t, " up=");
        put_dec(&t, elapsed_ms());
        put_str(&t, "ms ");
        put_psci(&t);
        if (bite) put_str(&t, "; first the watchdog bite (pagani-reset-wdog)");
        if (power_off) put_str(&t, "; first SYSTEM_OFF (0x84000008)");
        emit_text(RAMOOPS_KIND_RESET, reset_text);
        zones_stamp();
        unlock_log(flags);
    }
    if (reset_seq_mask) reset_seq(reason, first, bite);
#ifndef RAMOOPS_HOST
    // The bite comes within the shortest bite time (4 s) of the last pet: PSCI only if it has not come 10 s later.
    if (bite && counter_hz) {
        const uint64_t until = log_counter() + 10 * counter_hz;
        while (log_counter() < until) __asm__ volatile("yield");
        if (first) {
            text_start(&t, reset_text, sizeof(reset_text));
            put_str(&t, "no watchdog bite within 10 s; resetting through PSCI");
            log_reset_text();
        }
    }
#endif
    // Gunyah passes the HLOS VM's SYSTEM_OFF to the firmware and does not come back; one that does comes back refused, and a phone that
    // would not power off restarts as for any other reset. Only the first resetter powers off: a later one is here because of a panic
    // or a timer.
    if (first && power_off) {
        int64_t result = ramoops_platform_psci(psci_hvc, PSCI_SYSTEM_OFF, 0, 0, 0);
        text_start(&t, reset_text, sizeof(reset_text));
        put_str(&t, "SYSTEM_OFF returned ");
        put_result(&t, result);
        put_str(&t, "; resetting instead");
        log_reset_text();
        ramoops_platform_wdog_release(reason);
    }
    if (psci_reset2()) {
        int64_t result = ramoops_platform_psci(psci_hvc, PSCI_SYSTEM_RESET2_64, 0, 0, 0);
        if (first) {
            text_start(&t, reset_text, sizeof(reset_text));
            put_str(&t, "SYSTEM_RESET2 returned ");
            put_result(&t, result);
            put_str(&t, "; trying SYSTEM_RESET");
            log_reset_text();
        }
    }
    int64_t result = ramoops_platform_psci(psci_hvc, PSCI_SYSTEM_RESET, 0, 0, 0);
    if (first) {
        text_start(&t, reset_text, sizeof(reset_text));
        put_str(&t, "SYSTEM_RESET returned ");
        put_result(&t, result);
        put_str(&t, "; retrying once a second, watchdog armed");
        log_reset_text();
    }
    reset_retry();
}

#ifdef RAMOOPS_HOST
void ramoops_log_timer_fired(void) { reset_now("timer"); }
void ramoops_log_backup_timer_fired(void) { reset_now("backup timer"); }
#endif

void ramoops_log_suspend(void) { log_enabled = 0; }

int ramoops_log_resetting(void) { return __atomic_load_n(&reset_begun, __ATOMIC_ACQUIRE); }

void ramoops_log_restart(const char *reason) { reset_now(reason); }

void ramoops_log_power_off(const char *reason)
{
    __atomic_store_n(&reset_power_off, 1, __ATOMIC_RELEASE);
    reset_now(reason);
}

const char *ramoops_log_status(void) { return status[0] ? status : NULL; }

unsigned ramoops_log_snapshot(void *dst, unsigned cap, struct ramoops_snapshot *info)
{
    info->events_total = info->console_total = 0;
    info->events_bytes = info->console_bytes = 0;
    info->sequence = info->run = 0;
    if (!log_on()) return 0;
    info->events_total = __atomic_load_n(&areas[AREA_EVENTS].total, __ATOMIC_ACQUIRE);
    info->console_total = __atomic_load_n(&areas[AREA_CONSOLE].total, __ATOMIC_ACQUIRE);
    info->events_bytes = areas[AREA_EVENTS].bytes;
    info->console_bytes = areas[AREA_CONSOLE].bytes;
    info->sequence = __atomic_load_n(&sequence, __ATOMIC_RELAXED);
    info->run = run_id;
    if (dst) {
        const volatile uint32_t *from = (const volatile uint32_t *)(uintptr_t)copies[0];
        uint32_t *to = dst;
        const unsigned words = (copy_bytes < cap ? copy_bytes : cap) / 4;
        for (unsigned i = 0; i < words; i++) to[i] = from[i];
    }
    return copy_bytes;
}

uint64_t ramoops_log_reset_in_ms(void)
{
    if (!reset_deadline || !counter_hz) return 0;
    const uint64_t now = log_counter();
    const uint64_t ms = now < reset_deadline ? (reset_deadline - now) * 1000 / counter_hz : 0;
    return ms ? ms : 1;
}

static void put_seconds_config(struct text *t)
{
    if (reset_after_seconds) { put_str(t, "; reset after "); put_dec(t, reset_after_seconds); put_str(t, " s"); }
    if (panic_reset_seconds) { put_str(t, "; panic reset "); put_dec(t, panic_reset_seconds); put_str(t, " s"); }
    if (reset_seq_mask) { put_str(t, "; reset seq "); put_hex(t, reset_seq_mask); }
    else if (reset_seq_refused) { put_str(t, "; reset seq REFUSED "); put_hex(t, reset_seq_refused); }
}

// S5 of the 2026-10-31 review: XNU's parser (pexpert/gen/bootargs.c getval, argstrcpy2, argnumcpy) reads "0x" + hex digits, "0b"
// binary, a leading 0 as octal and decimal; anything else ("0X10F", a word) comes back as the bytes of its first three characters
// (0X10F: 0x315830, OEM bit set), a bare name as 1, and only the low 32 bits of a longer number. The parse goes on after a match,
// so the last occurrence counts. The value text of that occurrence (NULL with *bare set for a bare name; NULL alone: not found).
static const char *reset_seq_written(const char *args, unsigned *length, int *bare)
{
    static const char name[] = "pagani-reset-seq";
    const char *value = NULL;
    *length = 0;
    *bare = 0;
    for (const char *p = args; p && *p;) {
        while (*p == ' ' || *p == '\t') p++;
        const char *token = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        unsigned n = 0, same = 0;
        while (token + n < p && token[n] != '=') n++;
        if (n != sizeof(name) - 1) continue;
        while (same < n && token[same] == name[same]) same++;
        if (same != n) continue;
        value = token + n < p ? token + n + 1 : NULL;
        *bare = value == NULL;
        *length = value ? (unsigned)(p - value) : 0;
    }
    return value;
}

// Why the parsed pagani-reset-seq value may not run (NULL: it may): it must be written "0x" and 1 to 8 hex digits, read as that
// value, with known bits only.
static const char *reset_seq_refusal(uint32_t value, const char *text, unsigned length)
{
    if (value & ~(uint32_t)RAMOOPS_RESET_SEQ_KNOWN) return "unknown bits";
    if (!text || length < 3 || length > 10 || text[0] != '0' || text[1] != 'x') return "not written as 0x and 1 to 8 hex digits";
    uint32_t written = 0;
    for (unsigned i = 2; i < length; i++) {
        const char c = text[i];
        const uint32_t digit = c >= '0' && c <= '9' ? (uint32_t)(c - '0') : c >= 'a' && c <= 'f' ? (uint32_t)(c - 'a' + 10) :
                               c >= 'A' && c <= 'F' ? (uint32_t)(c - 'A' + 10) : 16u;
        if (digit > 15) return "not written as 0x and 1 to 8 hex digits";
        written = written << 4 | digit;
    }
    return written == value ? NULL : "parsed as another value";
}

static int refuse(const char *why, uint64_t a, uint64_t b)
{
    struct text t;
    text_start(&t, status, sizeof(status));
    put_str(&t, "ramoops refused: ");
    put_str(&t, why);
    if (a || b) { put_char(&t, ' '); put_hex(&t, a); put_char(&t, '-'); put_hex(&t, b); }
    put_seconds_config(&t);
    return 0;
}

int ramoops_log_vm_ready(void)
{
    struct text t;
    uint32_t value = 0;
    log_enabled = 0;
    log_mask = 0; sequence = 0; drops = 0; drops_reported = 0; heartbeat_at = 0; lock_word = 0; lock_suspect = 0;
    zone_base[0] = zone_base[1] = 0;
    reset_after_seconds = 0; panic_reset_seconds = 0; reset_deadline = 0; reset_cold = 0; reset_wdog = 0;
    reset_armed = 0; reset_started = 0; reset_begun = 0; headless_panic = 0; reset_parked = 0; psci_resolved = 0;
    reset_started_at = 0; reset_observer_retries = 0;
    reset_timer_cpu = -1; reset_backup_cpu = -1; reset_backup_armed = 0;
    reset_seq_mask = 0; reset_cpus_parked = 0; reset_seq_state = RESET_SEQ_IDLE; reset_seq_probed = 0; reset_seq_screen[0] = 0;
    reset_seq_refused = 0; reset_cold_skipped = 0; reset_power_off = 0;
    console_line.length = 0; console_line.continued = 0; panic_line.length = 0; panic_line.continued = 0;
    console_line.area = AREA_CONSOLE; panic_line.area = AREA_EVENTS;
    rowsets[0].valid = 0; rowsets[1].valid = 0; rowsets[0].sets = 0; rowsets[1].sets = 0;
    status[0] = 0;
    start_counter = log_counter();
    counter_hz = ramoops_platform_frequency();
    if (!counter_hz) counter_hz = 19200000;
    if (ramoops_platform_arg("pagani-reset-after", &value, sizeof(value))) reset_after_seconds = value;
    value = 0;
    if (ramoops_platform_arg("pagani-panic-reset", &value, sizeof(value))) panic_reset_seconds = value;
    value = 0;
    if (ramoops_platform_arg("pagani-reset-cold", &value, sizeof(value))) reset_cold = value != 0;
    if (ramoops_platform_arg("pagani-reset-wdog", &value, sizeof(value))) reset_wdog = value != 0;
    value = 0;
    // pagani-reset-seq: taken only as written and with known bits (S5 above); otherwise none of it runs.
    const char *seq_text = NULL, *seq_why = NULL;
    unsigned seq_length = 0;
    int seq_bare = 0;
    if (ramoops_platform_arg("pagani-reset-seq", &value, sizeof(value)) && value) {
        seq_text = reset_seq_written(ramoops_platform_boot_args(), &seq_length, &seq_bare);
        seq_why = reset_seq_refusal(value, seq_text, seq_length);
        if (seq_why) reset_seq_refused = value;
        else reset_seq_mask = value;
    }
    if (reset_after_seconds) reset_deadline = start_counter + (uint64_t)reset_after_seconds * counter_hz;
    value = 0;
    if (ramoops_platform_arg("pagani-ramoops", &value, sizeof(value))) log_mask = value;
    if (!(log_mask & RAMOOPS_LOG_MASK_LOG)) {
        if (reset_after_seconds || panic_reset_seconds || reset_seq_refused) {
            text_start(&t, status, sizeof(status));
            put_str(&t, "ramoops off");
            put_seconds_config(&t);
        }
        return 0;
    }
    uint64_t pa = RAMOOPS_LOG_DEFAULT_PA;
    uint32_t console = RAMOOPS_LOG_DEFAULT_CONSOLE, pmsg = RAMOOPS_LOG_DEFAULT_PMSG, limit = RAMOOPS_LOG_COPY_LIMIT;
    (void)ramoops_platform_arg("pagani-ramoops-pa", &pa, sizeof(pa));
    (void)ramoops_platform_arg("pagani-ramoops-console", &console, sizeof(console));
    (void)ramoops_platform_arg("pagani-ramoops-pmsg", &pmsg, sizeof(pmsg));
    (void)ramoops_platform_arg("pagani-ramoops-copy", &limit, sizeof(limit));
    // Linux rounds both zone sizes down to powers of two.
    console = floor_power_of_two(console);
    pmsg = floor_power_of_two(pmsg);
    uint64_t total = (uint64_t)console + pmsg;
    if ((pa & 4095) || console < 4096 || pmsg < 4096 || pa + total < pa) return refuse("bad region", pa, pa + total);
    uint64_t ram = 0, ram_bytes = 0;
    ramoops_platform_ram(&ram, &ram_bytes);
    if (pa < ram + ram_bytes && ram < pa + total) return refuse("region overlaps XNU RAM", ram, ram + ram_bytes);
    // Copies: the largest size of at most the limit (64 KiB, or pagani-ramoops-copy) that tiles each zone (on the 13T
    // 0xfffc: 4 in the console zone, 32 in pmsg; with 0x8000, 0x7ffc: 8 + 56), so a zone Linux drops still leaves the
    // other one enough copies to vote.
    uint32_t space[2] = {console - RAMOOPS_LOG_ZONE_HEADER, pmsg - RAMOOPS_LOG_ZONE_HEADER};
    if (!limit) limit = RAMOOPS_LOG_COPY_LIMIT;
    copy_bytes = limit;
    for (unsigned z = 0; z < 2; z++) {
        uint32_t tiles = (uint32_t)(((uint64_t)space[z] + limit - 1) / limit);
        uint32_t size = (space[z] / tiles) & ~3u;
        if (size < copy_bytes) copy_bytes = size;
    }
    if (copy_bytes < RAMOOPS_LOG_BOOT_END + 3 * 4096) return refuse("zones too small for a copy", pa, pa + total);
    zone_copies[0] = space[0] / copy_bytes;
    zone_copies[1] = space[1] / copy_bytes;
    // At most RAMOOPS_LOG_MAX_COPIES in all; each zone keeps up to half of them, or more when the other needs fewer.
    if (zone_copies[0] + zone_copies[1] > RAMOOPS_LOG_MAX_COPIES) {
        unsigned half = RAMOOPS_LOG_MAX_COPIES / 2;
        if (zone_copies[0] <= half) zone_copies[1] = RAMOOPS_LOG_MAX_COPIES - zone_copies[0];
        else if (zone_copies[1] <= half) zone_copies[0] = RAMOOPS_LOG_MAX_COPIES - zone_copies[1];
        else zone_copies[0] = zone_copies[1] = half;
    }
    // The events ring: 32 KiB while the console ring keeps at least 16 KiB, else half of what follows the boot area.
    uint32_t rest = copy_bytes - RAMOOPS_LOG_BOOT_END;
    uint32_t events = rest >= RAMOOPS_LOG_EVENTS_BYTES + RAMOOPS_LOG_CONSOLE_MIN ? RAMOOPS_LOG_EVENTS_BYTES : (rest / 2) & ~3u;
    areas[AREA_BOOT] = (struct area){RAMOOPS_LOG_COPY_HEADER, RAMOOPS_LOG_BOOT_END - RAMOOPS_LOG_COPY_HEADER, 0, 0, 0};
    areas[AREA_EVENTS] = (struct area){RAMOOPS_LOG_BOOT_END, events, 0, 1, 0};
    areas[AREA_CONSOLE] = (struct area){RAMOOPS_LOG_BOOT_END + events, copy_bytes - RAMOOPS_LOG_BOOT_END - events, 0, 1, 0};
    uintptr_t base = ramoops_platform_map(pa, total, (log_mask & RAMOOPS_LOG_MASK_DEVICE) != 0);
    if (!base) return refuse("map failed", pa, pa + total);
    zone_size[0] = console;
    zone_size[1] = pmsg;
    zone_init(base, console);
    zone_init(base + console, pmsg);
    zone_base[0] = base;
    zone_base[1] = base + console;
    copy_count = 0;
    for (unsigned z = 0; z < 2; z++)
        for (unsigned k = 0; k < zone_copies[z]; k++)
            copies[copy_count++] = (volatile uint8_t *)(base + (z ? console : 0) + RAMOOPS_LOG_ZONE_HEADER) + (uintptr_t)k * copy_bytes;
    run_id = ~crc_update(0xffffffffu, (const uint8_t *)&start_counter, sizeof(start_counter));
    copy_headers(pa, console, pmsg);
    complete_stores();
    log_enabled = 1;

    uint64_t flags = lock_log(1);
    text_start(&t, scratch, sizeof(scratch));
    put_str(&t, "UniXNU ramoops log v3 run=");
    put_hex(&t, run_id);
    put_str(&t, " mask=");
    put_dec(&t, log_mask);
    emit_text(RAMOOPS_KIND_BOOT, scratch);
    text_start(&t, scratch, sizeof(scratch));
    put_str(&t, "zones console=");
    put_hex(&t, pa); put_char(&t, '+'); put_hex(&t, console);
    put_str(&t, " pmsg=");
    put_hex(&t, pa + console); put_char(&t, '+'); put_hex(&t, pmsg);
    put_str(&t, " copies ");
    put_dec(&t, zone_copies[0]); put_char(&t, '+'); put_dec(&t, zone_copies[1]);
    put_str(&t, " x ");
    put_hex(&t, copy_bytes);
    emit_text(RAMOOPS_KIND_BOOT, scratch);
    text_start(&t, scratch, sizeof(scratch));
    put_str(&t, "areas boot ");
    put_hex(&t, areas[AREA_BOOT].offset); put_char(&t, '+'); put_hex(&t, areas[AREA_BOOT].bytes);
    put_str(&t, " events ");
    put_hex(&t, areas[AREA_EVENTS].offset); put_char(&t, '+'); put_hex(&t, areas[AREA_EVENTS].bytes);
    put_str(&t, " console ");
    put_hex(&t, areas[AREA_CONSOLE].offset); put_char(&t, '+'); put_hex(&t, areas[AREA_CONSOLE].bytes);
    emit_text(RAMOOPS_KIND_BOOT, scratch);
    text_start(&t, scratch, sizeof(scratch));
    put_str(&t, "xnu ram=");
    put_hex(&t, ram); put_char(&t, '+'); put_hex(&t, ram_bytes);
    put_str(&t, " counter=");
    put_hex(&t, start_counter);
    put_str(&t, " hz=");
    put_dec(&t, counter_hz);
    emit_text(RAMOOPS_KIND_BOOT, scratch);
    text_start(&t, scratch, sizeof(scratch));
    put_str(&t, "reset-after=");
    put_dec(&t, reset_after_seconds);
    put_str(&t, "s panic-reset=");
    put_dec(&t, panic_reset_seconds);
    put_char(&t, 's');
    if (reset_seq_mask) { put_str(&t, " reset-seq="); put_hex(&t, reset_seq_mask); }
    emit_text(RAMOOPS_KIND_BOOT, scratch);
    emit_text(RAMOOPS_KIND_ARGS, ramoops_platform_boot_args());
    if (reset_seq_refused) {
        text_start(&t, scratch, sizeof(scratch));
        put_str(&t, "reset seq refused: pagani-reset-seq");
        if (seq_text) {
            put_char(&t, '=');
            for (unsigned i = 0; i < seq_length && i < 24; i++) put_char(&t, seq_text[i]);
        } else put_str(&t, seq_bare ? " (no value)" : " (not in the boot-args)");
        put_str(&t, " read as ");
        put_hex(&t, reset_seq_refused);
        put_str(&t, ": ");
        put_str(&t, seq_why);
        put_str(&t, "; none of it runs (write the mask as 0x and 1 to 8 hex digits)");
        emit_text(RAMOOPS_KIND_RESET, scratch);
    }
    unlock_log(flags);

    text_start(&t, status, sizeof(status));
    put_str(&t, "ramoops on ");
    put_hex(&t, pa);
    put_str(&t, " copies ");
    put_dec(&t, copy_count);
    put_str(&t, " x ");
    put_hex(&t, copy_bytes);
    if (log_mask & RAMOOPS_LOG_MASK_DEVICE) put_str(&t, " device");
    put_seconds_config(&t);
    return 1;
}

void ramoops_log_start(void)
{
    if ((reset_seq_mask & RAMOOPS_RESET_SEQ_PROBE) && !reset_seq_probed) {
        reset_seq_probed = 1;
        reset_seq_probe_boot();
    }
    if (!log_on() && !reset_after_seconds && !panic_reset_seconds) return;
    psci_resolve();
    uint64_t ms = 0;
    if (reset_deadline && !reset_armed) {
        uint64_t now = log_counter();
        ms = now < reset_deadline ? (reset_deadline - now) * 1000 / counter_hz : 0;
        if (!ms) ms = 1;
    }
    if (log_on()) {
        uint64_t flags = lock_log(1);
        struct text t;
        text_start(&t, scratch, sizeof(scratch));
        put_psci(&t);
        emit_text(RAMOOPS_KIND_RESET, scratch);
        if (ms) {
            text_start(&t, scratch, sizeof(scratch));
            put_str(&t, "reset armed in ");
            put_dec(&t, ms);
            put_str(&t, " ms (pagani-reset-after=");
            put_dec(&t, reset_after_seconds);
            put_str(&t, ")");
            emit_text(RAMOOPS_KIND_RESET, scratch);
        }
        unlock_log(flags);
    }
    if (ms) {
        reset_timer_cpu = ramoops_platform_arm_timer(ms);
        __atomic_store_n(&reset_armed, 1, __ATOMIC_RELEASE);
    }
}

// G4, from the observer: the second reset timer on the observer's CPU the first time that is not the first timer's (normally the
// first timer reset the phone 3 s before it fires). Its reset_now is the same as the first timer's: a second caller of reset_now
// goes straight to PSCI and then retries (re-arming the watchdog when the first caller never got back from PSCI).
static void arm_backup_timer(uint64_t now)
{
    const uint64_t due = reset_deadline + (uint64_t)RESET_BACKUP_SECONDS * counter_hz;
    uint64_t ms = now < due ? (due - now) * 1000 / counter_hz : 0;
    if (!ms) ms = 1;
    const int cpu = ramoops_platform_arm_backup_timer(ms, reset_timer_cpu);
    if (cpu < 0) return;                    // still the first timer's CPU: the next pass tries again
    reset_backup_armed = 1;
    reset_backup_cpu = cpu;
    if (!log_on()) return;
    struct text t;
    text_start(&t, reset_backup_text, sizeof(reset_backup_text));
    put_str(&t, "reset backup timer armed on cpu ");
    put_dec(&t, (uint64_t)cpu);
    put_str(&t, " in ");
    put_dec(&t, ms);
    put_str(&t, " ms (deadline + ");
    put_dec(&t, RESET_BACKUP_SECONDS);
    put_str(&t, " s; the first timer is on cpu ");
    if (reset_timer_cpu >= 0) put_dec(&t, (uint64_t)reset_timer_cpu);
    else put_char(&t, '?');
    put_char(&t, ')');
    ramoops_log_line(RAMOOPS_KIND_RESET, reset_backup_text);
}

void ramoops_log_putc(char c)
{
    if (!log_on()) return;
    uint64_t flags = lock_log(0);
    if (flags == LOCK_FAILED) return;
    line_put(&console_line, RAMOOPS_KIND_CONSOLE, c);
    unlock_log(flags);
}

void ramoops_log_line(char kind, const char *text)
{
    if (!log_on()) return;
    uint64_t flags = lock_log(0);
    if (flags == LOCK_FAILED) return;
    emit_text(kind, text);
    unlock_log(flags);
}

void ramoops_log_root(const char *stage, const char *detail, uint64_t value)
{
    if (!log_on()) return;
    uint64_t flags = lock_log(0);
    if (flags == LOCK_FAILED) return;
    struct text t;
    text_start(&t, scratch, sizeof(scratch));
    put_str(&t, stage ? stage : "?");
    put_str(&t, " value=");
    put_hex(&t, value);
    if (detail && detail[0]) { put_str(&t, ": "); put_str(&t, detail); }
    for (unsigned i = 0; i < t.used; i++) if (scratch[i] == '\n') scratch[i] = ' ';
    emit_text(RAMOOPS_KIND_ROOT, scratch);
    unlock_log(flags);
}

void ramoops_log_rows(char kind, const char (*rows)[PAGANI_GPU_ROW_BYTES], unsigned count)
{
    if (!log_on()) return;
    int dpu = kind == RAMOOPS_KIND_DPU;
    if (dpu && !(log_mask & RAMOOPS_LOG_MASK_DPU)) return;
    unsigned limit = dpu ? (unsigned)PAGANI_DPU_ROWS : (unsigned)PAGANI_GPU_ROWS;
    if (!rows) count = 0;
    if (count > limit) count = limit;
    uint32_t crc = crc_update(0xffffffffu, (const uint8_t *)&count, sizeof(count));
    for (unsigned r = 0; r < count; r++)
        crc = crc_update(crc, (const uint8_t *)rows[r], bounded_length(rows[r], PAGANI_GPU_ROW_BYTES - 1));
    uint64_t now = log_counter();
    uint64_t flags = lock_log(0);
    if (flags == LOCK_FAILED) return;
    struct rowset *set = &rowsets[dpu];
    if (set->valid && set->crc == crc && set->count == count) { unlock_log(flags); return; }
    if (dpu && set->valid && now - set->stamp < (uint64_t)DPU_SECONDS * counter_hz) { unlock_log(flags); return; }
    set->valid = 1;
    set->crc = crc;
    set->count = count;
    set->stamp = now;
    set->sets++;
    struct text t;
    for (unsigned r = 0; r < (count ? count : 1); r++) {
        text_start(&t, scratch, sizeof(scratch));
        put_dec(&t, set->sets);
        put_char(&t, '.');
        put_dec(&t, r);
        put_char(&t, '/');
        put_dec(&t, count);
        put_char(&t, ' ');
        if (count) {
            unsigned n = bounded_length(rows[r], PAGANI_GPU_ROW_BYTES - 1);
            for (unsigned i = 0; i < n; i++) put_char(&t, rows[r][i] == '\n' ? ' ' : rows[r][i]);
        } else put_str(&t, "(no rows)");
        // area_of: the GPU rows share the console ring (they came four a second into the events ring and pushed the boot's
        // one-time U lines out after about 80 s; this call wrote AREA_EVENTS outright until 2026-10-31).
        emit(area_of(kind, scratch), kind, scratch, t.used);
    }
    unlock_log(flags);
}

void ramoops_log_exception(uint64_t esr, uint64_t elr, uint64_t far)
{
    if (!log_on()) return;
    uint64_t flags = lock_log(1);
    struct text t;
    text_start(&t, scratch, sizeof(scratch));
    put_str(&t, "kernel exception ESR ");
    put_hex(&t, esr);
    put_str(&t, " ELR ");
    put_hex(&t, elr);
    put_str(&t, " FAR ");
    put_hex(&t, far);
    emit_text(RAMOOPS_KIND_EXCEPTION, scratch);
    unlock_log(flags);
}

void ramoops_log_panic_putc(char c)
{
    if (!log_on()) return;
    uint64_t flags = lock_log(1);
    line_put(&panic_line, RAMOOPS_KIND_PANIC, c);
    unlock_log(flags);
}

// Counter ticks from a panic to its reset; 0: the panic halts.
static uint64_t panic_wait(uint64_t now)
{
    if (panic_reset_seconds) return (uint64_t)panic_reset_seconds * counter_hz;
    if (reset_deadline) {
        uint64_t least = 5 * counter_hz;
        return reset_deadline > now + least ? reset_deadline - now : least;
    }
    return 0;
}

const char *ramoops_log_panic_footer(void)
{
    uint64_t wait = panic_wait(log_counter());
    if (!wait || !counter_hz) return NULL;
    struct text t;
    text_start(&t, footer, sizeof(footer));
    put_str(&t, "RESET IN ");
    put_dec(&t, (wait + counter_hz - 1) / counter_hz);
    put_str(&t, log_on() ? " s; log kept in ramoops" : " s");
    return footer;
}

void ramoops_log_panic_reset(void)
{
    uint64_t now = log_counter(), wait = panic_wait(now);
    if (log_on()) {
        uint64_t flags = lock_log(1);
        line_flush(&console_line, RAMOOPS_KIND_CONSOLE);
        line_flush(&panic_line, RAMOOPS_KIND_PANIC);
        if (wait) {
            struct text t;
            text_start(&t, scratch, sizeof(scratch));
            put_str(&t, "panic: reset in ");
            put_dec(&t, wait * 1000 / counter_hz);
            put_str(&t, " ms");
            emit_text(RAMOOPS_KIND_RESET, scratch);
        }
        unlock_log(flags);
    }
    // sheng-log=1: the last snapshot goes to UFS now that the records above are in it. It is bounded, and inside `wait`: the reset keeps
    // its time. Interrupts stay off for it (a panic without a screen comes here with them on).
    const uint64_t irqs = irq_off();
    ufs_log_panic(wait && counter_hz ? wait * 1000 / counter_hz : 0);
    irq_restore_flags(irqs);
    ramoops_platform_wdog_flushed();        // what stayed armed through the flush goes off now: no bite may race the reset below
    if (!wait) return;
    while (log_counter() - now < wait) relax();
    reset_now("panic");
}

void ramoops_log_panic_headless(const char *format, va_list *args, unsigned long caller, unsigned reason, uint64_t options)
{
    if (!log_on() && !panic_wait(log_counter())) return;
    if (__atomic_exchange_n(&headless_panic, 1, __ATOMIC_ACQ_REL)) return;
    if (format && args) {
        va_list copy;
        va_copy(copy, *args);
        if (vsnprintf(panic_text, sizeof(panic_text), format, copy) < 0) panic_text[0] = 0;
        va_end(copy);
    } else {
        unsigned n = bounded_length(format, PANIC_TEXT - 1);
        for (unsigned i = 0; i < n; i++) panic_text[i] = format[i];
        panic_text[n] = 0;
    }
    if (log_on()) {
        uint64_t flags = lock_log(1);
        struct text t;
        text_start(&t, scratch, sizeof(scratch));
        put_str(&t, "PANIC without a mapped screen caller ");
        put_hex(&t, caller);
        put_str(&t, " reason ");
        put_hex(&t, reason);
        put_str(&t, " options ");
        put_hex(&t, options);
        emit_text(RAMOOPS_KIND_PANIC, scratch);
        emit_text(RAMOOPS_KIND_PANIC, panic_text);
        unlock_log(flags);
    }
    ramoops_log_panic_reset();
}

void ramoops_log_tick(uint64_t polls, uint64_t timer_interrupts, uint64_t free_pages, uint64_t available_pages)
{
    uint64_t now = log_counter();
    // The CPU timer did not fire (its CPU wedged with interrupts off): the observer resets instead.
    if (reset_deadline && reset_armed && now >= reset_deadline + 2 * counter_hz &&
        !__atomic_load_n(&reset_started, __ATOMIC_ACQUIRE)) reset_now("observer");
    // A reset began 5 s ago and this observer still runs: the resetting CPU is stuck (in the firmware call, or parked) while this
    // one is alive. SYSTEM_RESET again from here, once a second (the audit's G3; reset_now's own retries need its CPU).
    const uint64_t began = __atomic_load_n(&reset_started_at, __ATOMIC_ACQUIRE);
    if (__atomic_load_n(&reset_started, __ATOMIC_ACQUIRE) && began && counter_hz && now >= began + 5 * counter_hz) {
        if (!reset_observer_retries++ && log_on()) ramoops_log_line(RAMOOPS_KIND_RESET, "reset: still running 5 s after the reset "
                                                                    "began; the observer sends SYSTEM_RESET once a second");
        psci_resolve();
        ramoops_platform_psci(psci_hvc, PSCI_SYSTEM_RESET, 0, 0, 0);
    }
    if (reset_deadline && __atomic_load_n(&reset_armed, __ATOMIC_ACQUIRE) && !reset_backup_armed &&
        !__atomic_load_n(&reset_started, __ATOMIC_ACQUIRE)) arm_backup_timer(now);
    if (reset_deadline && !reset_parked && now + RESET_PARK_SECONDS * counter_hz >= reset_deadline) {
        reset_parked = 1;
        adreno830_dcvs_park();
    }
    if (!log_on()) return;
    // The heartbeat's CPU clock is measured before the log lock (about 1 ms of adds; only this observer writes heartbeat_at).
    const int beat = !heartbeat_at || now - heartbeat_at >= (uint64_t)HEARTBEAT_SECONDS * counter_hz;
    int cpu = -1;
    const uint32_t mhz = beat ? ramoops_platform_cpu_mhz(&cpu) : 0;
    uint64_t flags = lock_log(0);
    if (flags == LOCK_FAILED) return;
    if (console_line.length && now - console_line.stamp >= counter_hz) line_flush(&console_line, RAMOOPS_KIND_CONSOLE);
    uint32_t dropped = __atomic_load_n(&drops, __ATOMIC_RELAXED);
    if (dropped != drops_reported) {
        struct text t;
        text_start(&t, scratch, sizeof(scratch));
        put_str(&t, "dropped ");
        put_dec(&t, dropped - drops_reported);
        put_str(&t, " writes: the log lock was held too long (a stopped CPU?)");
        emit_text(RAMOOPS_KIND_WARN, scratch);
        drops_reported = dropped;
    }
    if (beat) {
        struct text t;
        heartbeat_at = now;
        zones_stamp();
        text_start(&t, scratch, sizeof(scratch));
        put_str(&t, "up ");
        put_dec(&t, elapsed_ms() / 1000);
        put_str(&t, " s polls ");
        put_dec(&t, polls);
        put_str(&t, " timer ");
        put_dec(&t, timer_interrupts);
        put_str(&t, " free ");
        put_dec(&t, free_pages);
        put_str(&t, " avail ");
        put_dec(&t, available_pages);
        put_str(&t, " drops ");
        put_dec(&t, __atomic_load_n(&drops, __ATOMIC_RELAXED));
        put_str(&t, " cpu ");
        put_dec(&t, mhz);
        put_str(&t, "MHz on ");
        if (cpu >= 0) put_dec(&t, (uint64_t)cpu);
        else put_char(&t, '?');
        emit_text(RAMOOPS_KIND_HEARTBEAT, scratch);
    }
    unlock_log(flags);
}

void ramoops_log_fps(const char *row, uint32_t jitters, uint64_t lost, const char *extra)
{
    if (!log_on()) return;
    uint64_t flags = lock_log(0);
    if (flags == LOCK_FAILED) return;
    struct text t;
    text_start(&t, scratch, sizeof(scratch));
    put_str(&t, row ? row : "?");
    put_str(&t, " | jit ");
    put_dec(&t, jitters);
    if (lost) { put_str(&t, " lost "); put_dec(&t, lost); }
    put_str(&t, extra);
    for (unsigned i = 0; i < t.used; i++) if (scratch[i] == '\n') scratch[i] = ' ';
    emit_text(RAMOOPS_KIND_FPS, scratch);
    unlock_log(flags);
}

void ramoops_log_jitter(uint32_t interval_us, unsigned path, uint64_t age_us, uint32_t skipped, const char *detail)
{
    if (!log_on()) return;
    uint64_t flags = lock_log(0);
    if (flags == LOCK_FAILED) return;
    const uint64_t now = elapsed_ms(), age = age_us / 1000, tenths = ((uint64_t)interval_us + 50) / 100;
    struct text t;
    text_start(&t, scratch, sizeof(scratch));
    put_str(&t, "jitter ");
    put_dec(&t, tenths / 10);
    put_char(&t, '.');
    put_dec(&t, tenths % 10);
    put_str(&t, path ? "ms gpu at " : "ms cpu at ");
    put_dec(&t, now > age ? now - age : 0);
    put_str(&t, "ms");
    if (detail && detail[0]) { put_char(&t, ' '); put_str(&t, detail); }
    if (skipped) { put_str(&t, " skipped "); put_dec(&t, skipped); }
    for (unsigned i = 0; i < t.used; i++) if (scratch[i] == '\n') scratch[i] = ' ';
    emit_text(RAMOOPS_KIND_JITTER, scratch);
    unlock_log(flags);
}

void boot_progress_trace(const char *text) { ramoops_log_line(RAMOOPS_KIND_TRACE, text); }

void boot_progress_trace_values(const char *label, uint64_t a, uint64_t b)
{
    if (!log_on()) return;
    uint64_t flags = lock_log(0);
    if (flags == LOCK_FAILED) return;
    struct text t;
    text_start(&t, scratch, sizeof(scratch));
    put_str(&t, label ? label : "?");
    put_char(&t, ' ');
    put_hex(&t, a);
    put_char(&t, ' ');
    put_hex(&t, b);
    emit_text(RAMOOPS_KIND_TRACE, scratch);
    unlock_log(flags);
}

int boot_progress_trace_enabled(void) { return log_on(); }
