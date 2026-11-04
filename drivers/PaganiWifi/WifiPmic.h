/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved.
 * The rail readback and the sleep clock pin follow UniXNU-sheng package/WifiProbe.cpp pmicScan/pmicPrepare (commit 08cde20e); the
 * arbiter accesses follow Sm8750Platform PeripheralIO.h spmi_find/spmi_read8 and PaganiHaptics HapticsSpmi.c (the channel write). */
/* The PMIC side of the Wi-Fi power-up, through the SPMI PMIC arbiter v7 (core 0xc400000, owner table 0xc42d000, observer 0xc440000,
 * channels 0xc500000 + APID x 0x1000):
 *  - a regulator's state, read only through the apps observer channel: STATUS 0x08 (bit 7 in regulation), EN_CTL 0x46 (bit 7),
 *    MODE_CTL 0x45 (reported only), the set point in mV at 0x40/0x41 (the PPIDs are in PaganiWifiTables.h kRails and kPhyRails);
 *  - the 32 kHz sleep clock the WLAN PMU takes from PMK8550 gpio3 (PPID 0x0ba): an enabled output of func1 (MODE 0x40 = 1, the
 *    source 0x44 = 2, EN_CTL 0x46 bit 7). When it is not, and only when the apps processor owns the channel and the pin is an LV/MV
 *    GPIO (subtype 0x10..0x13), it is switched on with sheng's five writes in the pin control driver's order: pull 0x42 = 5 (none),
 *    power source 0x41 = 0, source 0x44 = 2, mode 0x40 = 1, enable 0x46 = 0x80. Nothing else is ever written. */
#ifndef PAGANI_WIFI_PMIC_H
#define PAGANI_WIFI_PMIC_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define PW_SPMI_CORE 0xc400000ull
#define PW_SPMI_CORE_BYTES 0x3000u
#define PW_SPMI_CNFG 0xc42d000ull
#define PW_SPMI_CNFG_BYTES 0x4000u
#define PW_SPMI_OBS 0xc440000ull
#define PW_SPMI_OBS_BYTES 0x8000u          /* EE 0 only: APID x 0x20 */
#define PW_SPMI_CHAN 0xc500000ull
#define PW_SPMI_CHAN_STRIDE 0x1000u
#define PW_SLEEP_CLOCK_PPID 0x0bau

enum {PwSpmiOk = 0, PwSpmiIoError = -5, PwSpmiNotFound = -19, PwSpmiTimeout = -110, PwSpmiNotOwner = -1, PwSpmiLayout = -95,
      PwSpmiArgument = -22};

typedef struct {
    void *ctx;
    uint32_t (*core)(void *ctx, uint32_t offset);
    uint32_t (*cnfg)(void *ctx, uint32_t offset);
    uint32_t (*obs_read)(void *ctx, uint32_t offset);
    void (*obs_write)(void *ctx, uint32_t offset, uint32_t value);
    /* the channel page of one APID (null: no write path) */
    uint32_t (*chan_read)(void *ctx, unsigned apid, uint32_t offset);
    void (*chan_write)(void *ctx, unsigned apid, uint32_t offset, uint32_t value);
    void (*delay_us)(void *ctx, unsigned us);
} PwSpmiIo;

/* The APID of a PPID, preferring the apps processor's (EE 0) channel; *owner gets its owner. Negative when none. */
int pw_spmi_find(const PwSpmiIo *io, unsigned ppid, int *owner);
int pw_spmi_read8(const PwSpmiIo *io, int apid, uint8_t reg, uint8_t *value);
/* A regulator's STATUS, EN_CTL, MODE_CTL (0xee when that one read failed: it is only reported) and set point. */
int pw_pmic_rail(const PwSpmiIo *io, unsigned ppid, uint8_t *status, uint8_t *enable, uint8_t *mode, uint16_t *millivolts);

typedef struct {
    int apid, owner;
    uint8_t regs[10];          /* 04 05 40 41 42 43 44 45 46 4a before, as read */
    uint8_t after[10];
    int written;               /* writes made (0..5) */
} PwSleepClock;
/* 1 when the pin already is an enabled func1 output, 2 when it was switched on now (write set), 0 when it is not (write clear),
 * negative when it cannot be read or switched (PwSpmiNotOwner: another EE owns it; PwSpmiLayout: not an LV/MV GPIO). */
int pw_pmic_sleep_clock(const PwSpmiIo *io, int write, PwSleepClock *out);

#ifdef __cplusplus
}
#endif
#endif
