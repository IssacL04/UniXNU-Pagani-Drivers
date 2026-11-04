// SPDX-License-Identifier: BSD-2-Clause-Patent
// Persistent boot log for the photo-free test loop and the timed warm reset (BootProgress internals; drivers use the
// boot_progress_trace calls of PeripheralStatus.h).
//
// pagani-ramoops=1 writes the log into both zones of the Android ramoops region of the OnePlus 13T (qcom,ramoops at
// 0x880000000: record_size 0, console 0x40000, ftrace 0, pmsg 0x200000, ecc 0), so the next Android boot exposes it as
// /sys/fs/pstore/console-ramoops-0 and pmsg-ramoops-0. Each zone gets Linux's struct persistent_ram_buffer
// {u32 sig; u32 start; u32 size; u8 data[];} once, with start 0 and size the whole zone (a size that loses bits only
// shortens the file Linux shows, a start that gains one rotates it; a damaged sig, or a size that gains a bit, makes Linux
// drop the zone), and holds whole copies of the log: copies of at most 64 KiB that tile each zone, on the 13T 0xfffc
// bytes, 4 in the console zone and 32 in pmsg, so either zone alone still holds enough copies to vote. Every record goes
// to the same offset of every copy. DRAM loses bits across the reset (measured on this phone: about 8% of the 1 bits,
// nearly always 1 -> 0), so a reader votes the copies bit by bit and checks the result with the per-record CRC. A copy
// starts with our 64-byte header (magic "UXNULOG3", version 3, copy number, copies, run, layout, copies per zone, CRC),
// then the boot area (to 4 KiB: log header, boot-args; filled once), the events ring (32 KiB: stages, status rows,
// traces, exceptions, panic, resets) and the console ring (the rest: console lines, per-process stages). Every record is
// one text line:
//
//   #SSSSSSSS MMMMMMMM LL CCCCCCCC K text\n
//
// S the sequence number, M milliseconds since the log started, L the text length, C the CRC32 (zlib) of bytes [0, 22) and
// [30, 33 + L) (everything but the CRC itself and the newline), K the kind; numbers are lowercase hex. All copies hold
// the same bytes except their copy number. (Layout version 2 wrote one copy of 0x3fff4 bytes into the console zone and
// eight into pmsg; the reader still decodes it.)
//
// pagani-ramoops-copy=<bytes> lowers (or raises) the 64 KiB limit of a copy: 0x8000 gives 8 copies of 0x7ffc in the
// console zone (56 in pmsg), for when Linux keeps dropping the pmsg zone, at the price of rings half as long.
//
// pagani-reset-after=<seconds> resets that long after the log started (a CPU timer on the boot CPU, with the 1 Hz observer as
// fallback 2 s after the deadline, and a second CPU timer 3 s after it, armed by the observer the first time it runs on another
// CPU, so one wedged CPU cannot hold both timers);
// pagani-panic-reset=<seconds> resets that long after a panic screen (without it a panic waits for the pagani-reset-after
// deadline when one is set). The reset is PSCI SYSTEM_RESET2 with the architectural warm type when PSCI_FEATURES
// advertises it, otherwise SYSTEM_RESET (pagani-reset-cold=1 always takes SYSTEM_RESET); the chosen path is logged first.
//
// pagani-reset-seq=<mask> (default 0: nothing changes, not a single extra call) runs, once per boot, the steps the 13T's own
// Android kernel takes before its PSCI reset (OnePlus sm8750 6.6 tree: qcom_dload_reboot, qcom_scm_shutdown, machine_restart,
// qcom_scm_do_restart), because Android's reboots reach the PMIC as WARM_RESET (DRAM kept) and ours as HARD_RESET. They run in
// reset_now after the "reset reason" record and before the PSCI call, in Linux's order; every SCM call is a fast SiP (or OEM)
// call over SMC #0 and logs one U record with its function id and x0 before the next step; a failure never stops the reset:
//   0x002 clear the download mode as qcom_dload_reboot does: SCM IO_WRITE 0xc2000502(0x1fd9000, 0) (TCSR 0x1fc0000 + 0x19000,
//         sun.dtsi qcom,dload-mode), with IO_READ 0xc2000501(0x1fd9000) before and after (read only);
//   0x001 disable SDI as qcom_scm_shutdown does: SCM 0xc2000109(1, 0) (BOOT / WDOG_DEBUG_PART);
//   0x004 halt the SPMI PMIC arbiter as qcom_scm_shutdown does: SCM 0xc2000901(0) (PWR / IO_DISABLE_PMIC_ARBITER). Our own
//         SPMI readers (PaganiInput's 8 ms PON poll, pagani-battery-pmic) are not stopped first: they only read, a read in
//         flight or issued later fails (-5 or -110) and counts as a missing sample, and the halt precedes the PS_HOLD drop;
//   0x008 park the other CPUs as smp_send_stop does (an XNU cross-call each: interrupts masked, wfe/wfi forever; waits at
//         most 1 s for them). It also stops the observer's late retry and the backup timer, which run on other CPUs;
//   0x010 (beyond Linux's cold path) the OEM warm reboot Linux uses when reboot_mode is warm: SCM 0xc3000922() (OEM_POWER /
//         OEM_POWER_REBOOT, owner OEM); if it returns, the PSCI reset follows as usual;
//   0x100 read only: at boot (ramoops_log_start, thread context) SCM IS_CALL_AVAIL 0x42000601 for each call above and the
//         download mode (IO_READ), one U record each and one screen line (ramoops_reset_seq_status); at the reset the download
//         mode is read once more. Combine with the other bits freely.
// A warm reset with the download mode set ends in the crash dump (XBL waits for a USB host, or writes a mini dump to the phone's
// storage), and Android clears the mode before every reboot. So 0x001, 0x004, 0x008, 0x010, and pagani-reset-cold=1 together
// with any bit, bring the clear of 0x002 with them (its U records say "implied"), and they run only when the download mode then
// reads back 0: otherwise one T record names them, they are skipped and the reset takes the path it takes without them
// (pagani-reset-cold included).
// The resetting CPU masks interrupts for the steps, and keeps them masked up to the reset once 0x008 parked the other CPUs (as
// Linux's machine_restart). A second resetter (another CPU, a panic) skips them and waits at most 2 s while they run. An armed
// pagani-wdog stays armed through them with its shortest bite counted from a pet at the start (no pets after it), so a step that
// never returns still ends in a bite (GunyahWdog.h); the steps' CPU disables it after them, right before the PSCI call, as the
// reset does without the steps. The watchdog-bite path (pagani-reset-wdog=1 with an armed pagani-wdog) applies none of them:
// one T record says so.
// The mask runs only as written "pagani-reset-seq=0x" and 1 to 8 hex digits (the last occurrence in the boot-args, the one XNU's
// parser keeps) and with no bit beyond RAMOOPS_RESET_SEQ_KNOWN: XNU's parser turns "0X10F" or a word into the bytes of its first
// three characters, "010" into octal 8, a bare "pagani-reset-seq" into 1 and more than 8 hex digits into their low 32 bits.
// Anything else refuses all of it (nothing runs, as without the boot-arg; one T record and the status line say so).
//
// ramoops_log_restart and ramoops_log_power_off are the platform expert's restart and power off (PaganiArmDTPlatform haltRestart,
// pagani-halt): the same reset, from thread or panic context, with `reason` in its "reset reason" record, pagani-reset-seq's steps, the
// watchdog handling and pagani-reset-wdog's bite. A power off then differs in three things: the first resetter calls PSCI SYSTEM_OFF
// (0x84000008) where the reset would start, which reaches the firmware for the HLOS VM (Gunyah psci_system_off) and is the call Android
// powers off with (13T psci_sys_poweroff); pagani-reset-seq skips its OEM warm reboot and pagani-reset-wdog its bite, either of which
// would restart the phone; and an armed pagani-wdog stays armed through the call, with its shortest bite from a pet right before, so a
// firmware that neither powers off nor answers ends in the bite. Should SYSTEM_OFF return, one T record says so and the reset follows.
// DRAM does not survive a power off, so these records are only readable when it did not happen.
//
// A reset that reaches the PMIC as HARD_RESET can take this log with it (Linux drops both zones); with sheng-log=1 the UFS log (UfsLog.h)
// keeps snapshots of copy 0 on the unixnu partition, taken with ramoops_log_snapshot, so what a run logged survives any reset. Every reset
// here starts with ufs_log_quiesce, which leaves no log command on the wire and may take up to UFS_LOG_QUIESCE_BUDGET_US; without
// sheng-log=1 it does nothing.
#ifndef PAGANI_RAMOOPS_LOG_H
#define PAGANI_RAMOOPS_LOG_H
#include <stdint.h>
#include <stdarg.h>
#include "PeripheralStatus.h"
#ifdef __cplusplus
extern "C" {
#endif
#define RAMOOPS_LOG_DEFAULT_PA 0x880000000ULL
enum {
    RAMOOPS_LOG_DEFAULT_CONSOLE = 0x40000,
    RAMOOPS_LOG_DEFAULT_PMSG = 0x200000,
    RAMOOPS_LOG_SIG = 0x43474244,           // PERSISTENT_RAM_SIG ("DBGC"); console and pmsg zones xor it with 0
    RAMOOPS_LOG_ZONE_HEADER = 12,           // sizeof(struct persistent_ram_buffer)
    RAMOOPS_LOG_HEADER_BYTES = 33,          // "#SSSSSSSS MMMMMMMM LL CCCCCCCC K "
    RAMOOPS_LOG_VERSION = 3,                // 2: nine 0x3fff4-byte copies (1 + 8), header CRC at 56
    RAMOOPS_LOG_MAGIC0 = 0x554e5855,        // "UXNU"
    RAMOOPS_LOG_MAGIC1 = 0x33474f4c,        // "LOG3"
    RAMOOPS_LOG_COPY_HEADER = 64,           // magic, version, copy, copies, run, copy bytes, events offset/bytes, console
                                            // bytes, zone bytes x2, region pa, copies per zone (console | pmsg << 16),
                                            // CRC32 of [0,12)+[16,60)
    RAMOOPS_LOG_COPY_LIMIT = 0x10000,       // copies are at most this (or pagani-ramoops-copy) and tile each zone
    RAMOOPS_LOG_BOOT_END = 4096,            // the boot area is [64, 4096) of a copy; the events ring follows
    RAMOOPS_LOG_EVENTS_BYTES = 0x8000,      // when the console ring keeps at least RAMOOPS_LOG_CONSOLE_MIN
    RAMOOPS_LOG_CONSOLE_MIN = 0x4000,
    RAMOOPS_LOG_MAX_COPIES = 64,
    RAMOOPS_LOG_TEXT_MAX = 200,             // longer lines continue in '+' records
    RAMOOPS_LOG_MASK_LOG = 1,               // pagani-ramoops bit 0: the log
    RAMOOPS_LOG_MASK_DPU = 2,               // bit 1: display probe rows too (at most every 10 s)
    RAMOOPS_LOG_MASK_DEVICE = 4,            // bit 2: map Device-nGnRE (ml_io_map) instead of write-combined
};
// pagani-reset-seq bits.
enum {
    RAMOOPS_RESET_SEQ_SDI = 0x1,            // SCM 0xc2000109(1, 0)
    RAMOOPS_RESET_SEQ_DLOAD = 0x2,          // SCM 0xc2000502(0x1fd9000, 0), read before and after
    RAMOOPS_RESET_SEQ_SPMI = 0x4,           // SCM 0xc2000901(0)
    RAMOOPS_RESET_SEQ_CPUS = 0x8,           // park the other CPUs
    RAMOOPS_RESET_SEQ_OEM = 0x10,           // SCM 0xc3000922()
    RAMOOPS_RESET_SEQ_PROBE = 0x100,        // read only: IS_CALL_AVAIL at boot, the download mode at boot and at the reset
    RAMOOPS_RESET_SEQ_KNOWN = 0x11f,
    RAMOOPS_RESET_SEQ_WARM = 0x1d,          // the steps that may make the reset warm: only with the download mode read back 0
};
// Record kinds.
enum {
    RAMOOPS_KIND_BOOT = 'B',        // log header: region, XNU RAM, configuration
    RAMOOPS_KIND_ARGS = 'A',        // boot-args
    RAMOOPS_KIND_STAGE = 'S',       // boot_progress_stage (the stage history; early stages replayed at log start)
    RAMOOPS_KIND_ROOT = 'R',        // boot_progress_root and display/GPU status stages
    RAMOOPS_KIND_CONSOLE = 'C',     // console text, one line
    RAMOOPS_KIND_MORE = '+',        // continues the previous record's line
    RAMOOPS_KIND_GPU = 'G',         // boot_progress_gpu_rows: "<set>.<row>/<rows> text"
    RAMOOPS_KIND_DPU = 'D',         // boot_progress_dpu_rows, same form
    RAMOOPS_KIND_EXCEPTION = 'E',   // boot_progress_kernel_exception
    RAMOOPS_KIND_PANIC = 'P',       // the panic screen text, line by line
    RAMOOPS_KIND_HEARTBEAT = 'H',   // every 10 s from the 1 Hz observer, ending "cpu <MHz>MHz on <cpu>": the observer's CPU clock
                                    // right then (CpuClock.h, about 1 ms of dependent adds; "on ?" when the thread changed CPUs)
    RAMOOPS_KIND_RESET = 'T',       // PSCI facts, reset arming and the reset path
    RAMOOPS_KIND_TRACE = 'U',       // boot_progress_trace from drivers
    RAMOOPS_KIND_WARN = 'W',        // dropped characters
    RAMOOPS_KIND_FPS = 'F',         // pagani-fps: the FPS row once a second while frames come, "<row> | jit N[ lost N]" (console ring:
                                    // periodic bulk that must not push the events out), then " | <SLOW row>[ | ref N fail N early N]"
                                    // while the last second's slowest interval was above 50 ms, a frame has waited in one stage for
                                    // over 50 ms, or a GPU offer was refused or a GPU copy failed (BootProgress fps_observe)
    RAMOOPS_KIND_JITTER = 'J',      // a frame interval above 50 ms: "jitter 88.4ms gpu at 123456ms gap 70.1 off 0.2 take 0.3 copy 17.0
                                    // done 0.1[ refused...] fb0 to0[ skipped N]" (events ring; at most 4 at once, then one per 5 s;
                                    // skipped: those left out since the previous one; the breakdown only for frames that have one)
    RAMOOPS_KIND_CRASH = 'X',       // a process killed by a fatal signal: its registers and frame chain (CrashTrace.h; events ring;
                                    // a name at most once in 10 s, a repeated trace as one "same" record; pagani-crash)
    RAMOOPS_KIND_METER = 'M',       // the stall probe's second (StallProbe.h, pagani-diag): scheduler latency, CPUs, IPIs, root volume and VM
                                    // (console ring: periodic bulk, like F)
    RAMOOPS_KIND_STALL = 'Q',       // the stall probe's samples of a hitch or a slow key (StallProbe.h; events ring, rate-limited there)
};

// early_boot_progress_init (MMU off; also the early fatal path): only clears the enabled flag.
void ramoops_log_suspend(void);
// early_boot_progress_vm_ready, after the normal exception vectors: reads the boot-args, checks that the region is not
// XNU RAM, maps it write-combined, clears both zones and writes the header records. 1 when the log runs.
int ramoops_log_vm_ready(void);
// One line for the screen describing the log and the reset configuration; NULL when all of it is off.
const char *ramoops_log_status(void);
// boot_progress_start (thread context): PSCI facts and the pagani-reset-after timer; the pagani-reset-seq probe (0x100).
void ramoops_log_start(void);
// After ramoops_log_start: one line for the screen with the pagani-reset-seq probe's answers; NULL without the probe.
const char *ramoops_reset_seq_status(void);
// Hooks; each is a cheap no-op while the log is off.
void ramoops_log_putc(char c);
void ramoops_log_line(char kind, const char *text);
void ramoops_log_root(const char *stage, const char *detail, uint64_t value);
void ramoops_log_rows(char kind, const char (*rows)[PAGANI_GPU_ROW_BYTES], unsigned count);
void ramoops_log_exception(uint64_t esr, uint64_t elr, uint64_t far);
// What the UFS log needs (UfsLog.h). A snapshot takes no lock: the counters are read first and every byte they count is in copy 0 by then, so
// a record being written meanwhile is at worst torn in `dst` (its CRC tells), and the next snapshot has it whole. *info is always filled;
// the result is the bytes of a whole copy, 0 while the log is off; with dst, up to cap of them (rounded down to 4) are copied, aligned
// 32-bit loads, as the Device mapping needs.
struct ramoops_snapshot {
    uint64_t events_total, console_total;   // bytes ever written to each ring: never go back
    uint32_t events_bytes, console_bytes;   // the rings' sizes
    uint32_t sequence;                      // the newest record's number
    uint32_t run;                           // the copies' run id
};
unsigned ramoops_log_snapshot(void *dst, unsigned cap, struct ramoops_snapshot *info);
// Milliseconds to the timed reset (pagani-reset-after), at least 1; 0 without one.
uint64_t ramoops_log_reset_in_ms(void);
// 1 once a reset has begun (from the first resetter's first line on).
int ramoops_log_resetting(void);
// The panic screen's text, as the panic owner draws it (not clipped like the screen).
void ramoops_log_panic_putc(char c);
// A panic without a mapped screen: logs it and, when a reset is configured, resets; returns otherwise.
void ramoops_log_panic_headless(const char *format, va_list *args, unsigned long caller, unsigned reason, uint64_t options);
// The panic screen's last line when a reset follows, NULL otherwise.
const char *ramoops_log_panic_footer(void);
// After the panic screen: flushes, waits pagani-panic-reset seconds (or for the pagani-reset-after deadline) and resets;
// returns at once when neither is set.
void ramoops_log_panic_reset(void);
// The platform's restart and power off (the header above). Neither returns on the device.
void ramoops_log_restart(const char *reason);
void ramoops_log_power_off(const char *reason);
// The 1 Hz observer: stale partial console lines, the heartbeat, the reset-timer fallback, the late retry of a reset that did not
// land and the arming of the second reset timer.
void ramoops_log_tick(uint64_t polls, uint64_t timer_interrupts, uint64_t free_pages, uint64_t available_pages);
// The 1 Hz observer's frame rate (BootProgress FrameRate.h, pagani-fps): the FPS row with the jitter intervals and the frames the scan
// could not see since its previous pass, then `extra` as it is (NULL: nothing); one jitter (interval in us, path 0 CPU / 1 GPU, how long
// before now it happened, how many jitters the rate limit left out since the previous record, and `detail`, the late frame's breakdown
// after a space; NULL or empty: none), its time given in the log's milliseconds.
void ramoops_log_fps(const char *row, uint32_t jitters, uint64_t lost, const char *extra);
void ramoops_log_jitter(uint32_t interval_us, unsigned path, uint64_t age_us, uint32_t skipped, const char *detail);
#ifdef __cplusplus
}
#endif
#endif
