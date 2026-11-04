/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* The calendar behind pagani-rtc=1: the PMK8550 real time clock of the OnePlus 13T, read only, plus an offset kept in memory.
 *
 * The kernel has no RTC driver: a BCM2837 XNU skips the wait for IORTC in IOKitInitializeTime, IOPlatformExpert::getGMTTimeOfDay returns 0
 * and the calendar starts at 1970. timed gives no floor here either: its filesystem timestamp is off without kern.monotonicclock_usecs.
 *
 * The clock (rtc@6100, compatible qcom,pmk8350-rtc; 13T rtc-pm8xxx.c pmk8350_regs): SPMI slave 0, peripheral 0x61, RTC_CTRL at 0x46 (bit 7:
 * the counter runs), RDATA0-3 at 0x48-0x4b, seconds, little endian, not latched: a read can straddle a carry, so a value counts once two
 * whole reads in a row agree. Reads go through the apps (EE 0) observer channel of the SPMI PMIC arbiter v7, as in PaganiInput and
 * PaganiBattery BatterySpmi.c (core + 0x2000 + 4 n: channel n's peripheral id in bits 19:8; cnfg + 4 n bits 2:0: its owner; observer
 * + 0x20 n: the command, status at +8, data at +0x18). Nothing is ever written to the PMIC: the RTC is not set, enabled or disturbed.
 *
 * utc = raw + offset. Android's DT gives rtc@6100 neither allow-set-time nor an offset cell, so raw is a free-running count and the
 * offset has to come from outside: boot-arg pagani-rtc-offset=<seconds> (Android's `date +%s` minus its /sys/class/rtc/rtc0/since_epoch;
 * negative allowed), else the first read starts at the floor, /chosen/pagani-build-time, the time the image was built, and the clock
 * runs from there. The floor also stops an offset gone stale (the count restarts when the battery runs flat). A time set from iOS (setGMTTimeOfDay) replaces the offset in memory only, until the next boot.
 * With the RTC unreadable the clock runs on the uptime from the last time given out, or from the floor. pagani-rtc absent or 0: none of
 * this runs and getGMTTimeOfDay stays the inherited 0.
 *
 * Portable and free of IOKit: ArmDTPlatform.cpp supplies the accessors, and the host test a model of the arbiter and the clock. */
#ifndef PAGANI_PLATFORM_RTC_H
#define PAGANI_PLATFORM_RTC_H
#include <stddef.h>
#include <stdint.h>

#define PRTC_CORE_BASE 0xc400000ull
#define PRTC_CORE_BYTES 0x3000u
#define PRTC_CNFG_BASE 0xc42d000ull
#define PRTC_CNFG_BYTES 0x4000u
#define PRTC_OBS_BASE 0xc440000ull
#define PRTC_OBS_BYTES 0x8000u          /* EE 0's channels only: APID x 0x20 */
#define PRTC_PPID 0x061u                /* slave 0 (PMK8550), peripheral 0x61 */
#define PRTC_REG_CTRL 0x46u
#define PRTC_CTRL_ENABLE 0x80u
#define PRTC_REG_RDATA0 0x48u
#define PRTC_READS 4u                   /* whole reads at most: one straddled carry costs one more */
#define PRTC_POLLS 1000u                /* a command's status polls, 1 us apart */
#define PRTC_LIMIT (1ll << 40)          /* seconds: anything at or beyond it is not a time (an unparsable boot-arg gives its bytes) */
#define PRTC_LATE_SECONDS 5             /* a calendar this far from the clock missed it at boot */

enum {
    PRTC_OK = 0,
    PRTC_EIO = -5,                      /* the arbiter refused or dropped a read */
    PRTC_ENODEV = -19,                  /* no v7 arbiter, or no channel for the peripheral */
    PRTC_EDISABLED = -61,               /* the counter does not run */
    PRTC_ETIMEDOUT = -110,              /* a read never finished */
    PRTC_EUNSTABLE = -85                /* no two whole reads agreed */
};

/* Where the offset came from, for the log. */
enum { PRTC_FROM_NONE, PRTC_FROM_RAW, PRTC_FROM_ARG, PRTC_FROM_FLOOR, PRTC_FROM_SET };

typedef struct {
    void *ctx;
    uint32_t (*core)(void *ctx, uint32_t offset);
    uint32_t (*cnfg)(void *ctx, uint32_t offset);
    uint32_t (*obs_read)(void *ctx, uint32_t offset);
    void (*obs_write)(void *ctx, uint32_t offset, uint32_t value);
    void (*delay_us)(void *ctx, unsigned us);
} PrtcIo;

typedef struct {
    int64_t arg;                        /* pagani-rtc-offset, with have_arg */
    int64_t offset;                     /* utc - raw, with known */
    int64_t floor;                      /* no earlier than this until an offset is known; 0: none */
    int64_t anchor_utc;                 /* the last time given out and the uptime then, with anchored */
    uint64_t anchor_uptime;
    uint8_t have_arg, known, anchored, from;
} PrtcClock;

/* The arbiter's channel of `ppid` (the apps processor's if several claim it), or PRTC_ENODEV. */
static inline int prtc_find(const PrtcIo *io, uint32_t ppid)
{
    const uint32_t version = io->core(io->ctx, 0), count = io->core(io->ctx, 4) & 0x7ffu;
    if (version < 0x70000000u || version >= 0x80000000u || count > 1024u) return PRTC_ENODEV;
    int found = PRTC_ENODEV;
    for (uint32_t n = 0; n < count; ++n) {
        const uint32_t map = io->core(io->ctx, 0x2000u + 4u * n);
        if (!map || ((map >> 8) & 0xfffu) != ppid) continue;
        if (found < 0 || !(io->cnfg(io->ctx, 4u * n) & 7u)) found = (int)n;
    }
    return found;
}

/* One register of the channel, through the observer: the read command (opcode 1, one byte), then status bit 0 done, bits 3:1 errors. */
static inline int prtc_read8(const PrtcIo *io, int apid, uint8_t reg, uint8_t *value)
{
    if (apid < 0 || (uint32_t)apid * 0x20u + 0x1cu > PRTC_OBS_BYTES) return PRTC_ENODEV;
    const uint32_t channel = (uint32_t)apid * 0x20u;
    io->obs_write(io->ctx, channel, 1u << 27 | (uint32_t)reg << 4);
    for (unsigned n = 0; n < PRTC_POLLS; ++n) {
        const uint32_t status = io->obs_read(io->ctx, channel + 8u);
        if (status & 0xeu) return PRTC_EIO;
        if (status & 1u) {
            *value = (uint8_t)io->obs_read(io->ctx, channel + 0x18u);
            return PRTC_OK;
        }
        io->delay_us(io->ctx, 1);
    }
    return PRTC_ETIMEDOUT;
}

static inline uint32_t prtc_decode(const uint8_t rdata[4])
{
    return (uint32_t)rdata[0] | (uint32_t)rdata[1] << 8 | (uint32_t)rdata[2] << 16 | (uint32_t)rdata[3] << 24;
}

/* The counter's seconds in *raw, RTC_CTRL in *control (also when the counter is off, for the log). */
static inline int prtc_read_raw(const PrtcIo *io, int apid, uint32_t *raw, uint8_t *control)
{
    int rc = prtc_read8(io, apid, PRTC_REG_CTRL, control);
    if (rc) return rc;
    if (!(*control & PRTC_CTRL_ENABLE)) return PRTC_EDISABLED;
    uint32_t previous = 0;
    for (unsigned attempt = 0; attempt < PRTC_READS; ++attempt) {
        uint8_t rdata[4] = {0, 0, 0, 0};
        for (uint8_t k = 0; k < 4; ++k) {
            rc = prtc_read8(io, apid, (uint8_t)(PRTC_REG_RDATA0 + k), &rdata[k]);
            if (rc) return rc;
        }
        const uint32_t value = prtc_decode(rdata);
        if (attempt && value == previous) {
            *raw = value;
            return PRTC_OK;
        }
        previous = value;
    }
    return PRTC_EUNSTABLE;
}

/* A time or an offset this code can add up without overflow. */
static inline int prtc_usable(int64_t seconds)
{
    return seconds > -PRTC_LIMIT && seconds < PRTC_LIMIT;
}

/* /chosen/pagani-build-time: a little-endian u32 or u64 of Unix seconds; 0 (no floor) for anything else. */
static inline int64_t prtc_floor(const void *bytes, size_t length)
{
    const uint8_t *in = (const uint8_t *)bytes;
    if (!in || (length != 4 && length != 8)) return 0;
    uint64_t value = 0;
    for (size_t k = 0; k < length; ++k) value |= (uint64_t)in[k] << (8 * k);
    return value < (uint64_t)PRTC_LIMIT ? (int64_t)value : 0;
}

/* have_offset: pagani-rtc-offset was given; an unusable value counts as not given. */
static inline void prtc_clock_start(PrtcClock *clock, int have_offset, int64_t offset, int64_t floor)
{
    const PrtcClock empty = {0, 0, 0, 0, 0, 0, 0, 0, PRTC_FROM_NONE};
    *clock = empty;
    clock->have_arg = have_offset && prtc_usable(offset);
    clock->arg = clock->have_arg ? offset : 0;
    clock->floor = floor;
}

/* The seconds for the calendar. ok: raw is a fresh read of the RTC; uptime in seconds. The first good read fixes the offset. */
static inline int64_t prtc_clock_get(PrtcClock *clock, int ok, uint32_t raw, uint64_t uptime)
{
    int64_t utc;
    if (ok) {
        if (!clock->known) {
            int64_t offset = clock->have_arg ? clock->arg : 0;
            uint8_t from = clock->have_arg ? PRTC_FROM_ARG : PRTC_FROM_RAW;
            if ((int64_t)raw + offset < clock->floor) {
                offset = clock->floor - (int64_t)raw;
                from = PRTC_FROM_FLOOR;
            }
            clock->offset = offset;
            clock->from = from;
            clock->known = 1;
        }
        utc = (int64_t)raw + clock->offset;
    } else if (clock->anchored) {
        utc = clock->anchor_utc + (int64_t)(uptime - clock->anchor_uptime);
    } else {
        utc = clock->floor + (int64_t)uptime;
    }
    if (utc < 0) utc = 0;
    clock->anchor_utc = utc;
    clock->anchor_uptime = uptime;
    clock->anchored = 1;
    return utc;
}

/* A time set from iOS: the offset in memory, no write to the PMIC. With the RTC unreadable it becomes the floor of its next good read. */
static inline void prtc_clock_set(PrtcClock *clock, int ok, uint32_t raw, uint64_t uptime, int64_t utc)
{
    if (!prtc_usable(utc) || utc < 0) return;
    if (ok) {
        clock->offset = utc - (int64_t)raw;
        clock->from = PRTC_FROM_SET;
        clock->known = 1;
    } else {
        clock->floor = utc;
        clock->have_arg = 0;
        clock->known = 0;
    }
    clock->anchor_utc = utc;
    clock->anchor_uptime = uptime;
    clock->anchored = 1;
}

/* After the boot's calendar init: does the running calendar still need this clock's time? (A BCM2837 XNU reads it without waiting for the
 * platform expert, so that read can have found none and left the calendar at the uptime.) */
static inline int prtc_calendar_stale(uint64_t calendar, int64_t utc)
{
    const int64_t difference = (int64_t)calendar - utc;
    return difference > PRTC_LATE_SECONDS || difference < -PRTC_LATE_SECONDS;
}

#endif
