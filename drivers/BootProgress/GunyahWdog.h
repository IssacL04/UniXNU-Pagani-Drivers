// SPDX-License-Identifier: BSD-2-Clause-Patent
// The Gunyah hypervisor's watchdog of the primary VM on the OnePlus 13T (SM8750), so an unattended test recovers from a
// kernel that stopped scheduling (BootProgress internals, next to RamoopsLog).
//
// pagani-wdog=<bite seconds> (0 or absent: off; clamped to 4..32) arms the watchdog Android drives through its
// "qcom,gh-watchdog" node under /hypervisor (gh_virt_wdt.ko + qcom_wdt_core.ko; bark interrupt SPI 0, which XNU leaves
// disabled). A bite is handled by the hypervisor (and TZ) with no XNU code running: the phone resets.
//
// ABI: SMCCC 1.1 fast calls, SMC32 convention, owner 6 (vendor-specific hypervisor service), conduit SMC (Gunyah traps
// the VM's SMC and serves these itself). These are the calls the phone's own gh_virt_wdt.ko makes, and the ones of
// Linux drivers/watchdog/gunyah_wdt.c (tested on SM8750):
//   0x86000005 CONTROL   w1 = 3 enable, 2 disable (bit 0: enable; bit 1 is reserved and must be 1)
//   0x86000006 STATUS    w1 bit 0 enabled, bit 31 expired (the bark fired); w2 milliseconds since the last pet
//   0x86000007 PET       w1 = 0
//   0x86000008 SET_TIME  w1 bark ms, w2 bite ms, both counted from the last pet; 0xffff leaves a field as it is.
//                        16-bit fields; values from 0x8000 are reserved (Gunyah clamps them), hence at most 32 s.
// w0 returns Gunyah's error code as 32 bits: 0 OK, -1 unimplemented (also SMCCC NOT_SUPPORTED), 1 invalid argument.
//
// Arming (gunyah_wdog_start): STATUS (the probe; a watchdog left enabled is disabled first), the pet thread, the boot
// CPU's heartbeat, SET_TIME(bark = bite / 2, bite), CONTROL(3), PET, STATUS (read back and logged with the configuration).
// Pets come only from thread context: the pagani-wdog kernel thread (every second) and the 1 Hz observer of
// BootProgress. Nothing in interrupt, exception or panic context pets the watchdog, so a kernel whose threads stop
// running is not kept alive by a surviving interrupt path.
// A pet also has to show that the boot CPU and the observer are alive (the audit's G5): the boot CPU carries the timed
// reset's timer (RamoopsLog) and takes every SPI, yet the pet thread and the observer pet from any CPU. So the arming
// thread (bound to the boot CPU) starts a heartbeat there, a local CPU timer that counts and enters itself again every
// GUNYAH_WDOG_BEAT_MS (interrupt context; it never pets), and a pet goes out only while that heartbeat and the observer
// (counted by gunyah_wdog_tick, from its first pass after the arming on) both ran within GUNYAH_WDOG_ALIVE_MS. The
// heartbeat's own timer wakes an idle boot CPU, so a tickless idle is not taken for a hang. A withheld stretch logs once
// when it begins and once when it ends; past the bite time the bite resets the phone.
// A panic stops the pets. When RamoopsLog resets after it (ramoops_log_panic_footer() != NULL) the watchdog is disabled
// first, so the two resets cannot race; otherwise it is left armed and the bite resets the phone. With a flush to the UFS log ahead
// (UfsLog.h) "first" means after that flush: a bus that hangs under it must still end in the bite.
// gunyah_wdog_before_reset disables it before an intentional PSCI reset. With pagani-reset-seq the reset first holds it armed with
// the shortest bite through its steps (gunyah_wdog_hold_for_steps) and disables it after them (gunyah_wdog_release_hold).
//
// pagani-wdog-stop-pet=<seconds> (test only) stops the pets that long after arming, as a hang would, and the pet thread
// then logs STATUS once a second until the bite. pagani-wdog-hvc=1 issues the calls with HVC instead of SMC (fallback
// for experiments; Android uses SMC).
//
// Records go to the ramoops log: 'T' for the configuration, the heartbeat, arming, withheld and resumed pets, panic and
// reset decisions, 'U' for pet failures, a late pet thread and the summary every 60 s (with the heartbeat's beats and the
// withheld stretches).
#ifndef PAGANI_GUNYAH_WDOG_H
#define PAGANI_GUNYAH_WDOG_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define GUNYAH_WDOG_FN_CONTROL 0x86000005u
#define GUNYAH_WDOG_FN_STATUS 0x86000006u
#define GUNYAH_WDOG_FN_PET 0x86000007u
#define GUNYAH_WDOG_FN_SET_TIME 0x86000008u
#define GUNYAH_WDOG_CTRL_ENABLE 3u
#define GUNYAH_WDOG_CTRL_DISABLE 2u
#define GUNYAH_WDOG_STATUS_ENABLED 0x1u
#define GUNYAH_WDOG_STATUS_EXPIRED 0x80000000u
#define GUNYAH_WDOG_TIME_UNCHANGED 0xffffu
#define GUNYAH_WDOG_MIN_SECONDS 4u
#define GUNYAH_WDOG_MAX_SECONDS 32u
#define GUNYAH_WDOG_PET_MS 1000u
#define GUNYAH_WDOG_BEAT_MS 1000u           // the boot CPU's heartbeat period
#define GUNYAH_WDOG_ALIVE_MS 3000u          // a pet needs a heartbeat and an observer pass at most this long ago

// boot_progress_start (thread context bound to the boot CPU, once): boot-args, probe, the pet thread, the boot CPU's
// heartbeat and arming. A no-op while pagani-wdog is 0 or absent.
void gunyah_wdog_start(void);
// One line for the screen describing the watchdog; NULL while pagani-wdog is off.
const char *gunyah_wdog_status(void);
// The 1 Hz observer (thread_call context): counts the observer's pass, then one pet while armed and alive.
void gunyah_wdog_tick(void);
// 1 while the watchdog is ours and armed.
int gunyah_wdog_armed(void);
// Before a flush that may hang (UfsLog.h ufs_log_quiesce; any context): no more pets, the watchdog stays as it is.
void gunyah_wdog_stop_pets(void);
// Panic paths (interrupts off, any CPU, more than once): stops the pets; disables the watchdog when RamoopsLog resets
// after the panic, otherwise leaves it armed so the bite resets the phone. With ufs_log_panic still to come the disable waits for
// gunyah_wdog_panic_flushed.
void gunyah_wdog_panic(void);
void gunyah_wdog_panic_flushed(void);
// The panic screen's last line when the bite will reset the phone, NULL otherwise.
const char *gunyah_wdog_panic_footer(void);
// Before an intentional PSCI reset (any context): stops the pets and disables the watchdog.
void gunyah_wdog_before_reset(const char *reason);
// After an intentional PSCI reset that returned (RamoopsLog's retry): enables the watchdog disabled above again with the shortest
// bite and no pets, so a firmware that refuses the reset still resets the phone. Nothing when the watchdog was not ours.
void gunyah_wdog_after_failed_reset(void);
// RamoopsLog's timed and observer resets with pagani-reset-wdog=1: stops the pets and sets the shortest bite, so the hypervisor resets
// the phone with no PSCI call (a bite kept the ramoops log intact where SYSTEM_RESET left weak DRAM cells flipped, 2026-10-30).
// 1 when the watchdog is ours and armed, else 0 (the caller resets through PSCI).
int gunyah_wdog_bite_reset(const char *reason);
// RamoopsLog's reset with pagani-reset-seq, in place of gunyah_wdog_before_reset: stops the pets and, when the watchdog is ours and
// armed, PET then SET_TIME(bark 2000, bite 4000) and leaves it armed, so a reset step that never returns (an SCM call, the CPU
// park) still ends in the bite 4 s later. 1: done by this call; 2: another resetter already settled the watchdog (nothing to do);
// 0: not ours or not armed (the caller goes on with gunyah_wdog_before_reset, which then does nothing either).
int gunyah_wdog_hold_for_steps(const char *reason);
// After the pagani-reset-seq steps, right before the PSCI reset (the CPU that ran them): disables the watchdog held above, as
// gunyah_wdog_before_reset does for a reset without steps. Nothing when it is not ours and armed.
void gunyah_wdog_release_hold(const char *reason);
#ifdef __cplusplus
}
#endif
#endif
