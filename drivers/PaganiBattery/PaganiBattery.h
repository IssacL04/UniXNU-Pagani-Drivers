/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* PaganiBattery: the OnePlus 13T battery as iOS reads it, an IOPMPowerSource (class PaganiBattery) fed by the NFG8011B fuel gauge on
 * the apps I2C and, with the audio DSP running (PaganiAdsp, pagani-adsp=1), by the DSP's battery manager over PMIC GLINK. See README.md.
 *
 * Boot arguments (all optional):
 *   pagani-battery=N            bit 0 on; absent, 0, an even value or one above 15: off (nothing runs, maps or allocates). pagani-adsp=1
 *                               does not imply it. The other bits add what is off by default, without one more byte of the command line:
 *                               2 the USB probe, once, 120 s after the start (read only; BatteryUsb.h); 4 the charger PMIC's state, read
 *                               every 2 s and logged only; 8 that state also decides ExternalConnected, IsCharging and FullyCharged
 *                               (implies 4; BatteryCore.h). 1 as before; 3, 5, 7, 13, 15 for the combinations
 *   pagani-battery-gauge=0      leave the I2C gauge alone (the DSP's values only)
 *   pagani-battery-gauge-id=0   skip the gauge's device type query at start (the one write to the gauge)
 *   pagani-battery-adsp=0       do not open the battery manager channel even with pagani-adsp=1
 *   pagani-battery-charge=N     0 read only (default); 1 input unsuspend, computer port limit, charge current limit and thermal limit;
 *                               2 also BATT_CHG_EN (OPLUS layout proven) - see BatteryCore.h
 *   pagani-battery-fcc-ma=MA    charge >= 1: charge current limit (default 2000, 500..6000)
 *   pagani-battery-sdp-ma=MA    charge >= 1: a computer port's input limit (default 900; 0: the firmware's)
 *   pagani-battery-design-mah=N design capacity when the DSP does not report it (default 6260, the 13T's battery)
 *   pagani-battery-gauge-ms=MS  gauge period (default 10000 without the DSP, 20000 with it)
 *   pagani-battery-log=N        0 start and failures, 1 rationed (default), 2 every pass (bring-up only: the log ring is shared)
 *   pagani-battery-pmic=N       first thing on the thread, who may drive the charger PMIC (SPMI slave 7; BatterySpmi.h): 0 nothing;
 *                               1 (default) the SPMI arbiter's channel and ownership tables, read only (the reads PaganiInput's
 *                               buttons make on every boot); 2 also each peripheral's TYPE/SUBTYPE, which writes read commands to
 *                               the apps (EE 0) observer channels (bring-up only) */
#ifndef PAGANI_BATTERY_H
#define PAGANI_BATTERY_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Stage7Probe's call after the GPU and ADSP starts (its weak default does nothing when PaganiBattery is not linked). Returns at once:
 * the work is a kernel thread's. */
void pagani_battery_start(void);

/* Diagnostics: [0] stage (1 starting, 2 classes, 3 service, 4 thread, 5 gauge up, 6 sources ready; negative: the step that failed),
 * [1] percent shown (-1 none yet), [2] mV, [3] mA (charge positive), [4] temperature (hundredths of a degree), [5] flags (bit 0 present,
 * 1 external, 2 charging, 3 full, 4 critical, 5 gauge answering, 6 DSP channel up, 7 published, 9:8 layout, 10 layout assumed,
 * 11 notifications enabled, 12 charger inferred from the gauge, 14:13 thermal limit), [6] gauge passes, [7] gauge read failures,
 * [8] battery manager cycles, [9] answers, [10] timeouts | errors << 16, [11] notifications, [12] writes taken | refused << 16,
 * [13] gauge passes answered, [14] publications, [15] 0 % readings shown as 1 % | refused sends << 16. */
extern volatile int32_t pagani_battery_status[16];

#ifdef __cplusplus
}
#endif
#endif
