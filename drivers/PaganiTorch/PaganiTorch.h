/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* PaganiTorch: the OnePlus 13T's LED torch (the PM8550 flash LED module, SPMI slave 1, 0xee00, channels 0 and 1) from XNU. See README.md.
 *
 * Boot arguments (all optional):
 *   pagani-torch=N          0 or absent: off (nothing runs, maps, allocates or registers); 1 on; 2 dry run: discovery, identity, owner
 *                           and a register dump in the boot log (observer reads) and one read of the module's RW channel STATUS; nothing
 *                           is written
 *   pagani-torch-max-ma=MA  the current per channel at level 1000 (default 50, at most 200, at least 5)
 *   pagani-torch-test=MS    bring-up: on at level 1000 for MS ms (at most 1000) right after the initialization, then off (mode 1 only)
 *   pagani-torch-log=N      0 failures only, 1 rationed (default), 2 every change
 *   pagani-torch-ios=1      the flashlight of iOS (TorchIos.h): MobileGestalt's camera-flash and the capture service the plug-in of
 *                           mediaserverd talks to; only with pagani-torch=1, just before launchd; absent or 0: nothing
 *
 * An "on" ends by itself after 5 minutes; any bus failure, refusal, failed read-back or fault bit switches the LED off and keeps the
 * driver off until reboot. */
#ifndef PAGANI_TORCH_H
#define PAGANI_TORCH_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Stage7Probe's call after the vibrator start (its weak default does nothing when PaganiTorch is not linked). Returns at once. */
void pagani_torch_start(void);

/* Level 0: off; 1..1000: on at that permille of pagani-torch-max-ma (a level above 0 is at least 5 mA). Any thread that may sleep, never
 * from interrupt context: waits for the driver's thread (at most 200 ms). 0 done, -19 the driver is not running (off, dry run, refused or
 * after a fault), -22 bad level, -5 this request hit a fault (the LED is off), -110 the thread did not answer in time. The 5 minute limit
 * runs from the moment the LED went on: changing the level does not extend it. */
int pagani_torch_set(uint32_t level);
/* The same for a named caller, for the log. */
int pagani_torch_request(uint32_t level, const char *who);
/* The level now (0 off), or -19 when the driver is not running. */
int pagani_torch_level(void);

/* Diagnostics (TorchCore.h pgt_core_export): [0] stage (1 started, 2 discovered, 3 identified, 4 ready, 5 dry run done; negative: where it
 * stopped: -1 config, -2 arbiter or no channel, -3 identity, owner or dump, -4 write path, -6 thread, -7 memory, -8 windows), [1] state
 * (1 off, 2 on, 3 fault, 4 refused, 5 dry run), [2] level, [3] mA per channel, [4] ITARGET code, [5] ons, [6] offs, [7] faults, [8] status
 * bits (s1 << 16 | s2 << 8 | s3), [9] SPMI writes, [10] refused, [11] APID, [12] owner EE, [13] type << 8 | subtype, [14] auto-offs,
 * [15] last error. */
extern volatile int32_t pagani_torch_status[16];

#ifdef __cplusplus
}
#endif
#endif
