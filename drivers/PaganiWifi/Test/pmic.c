// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026, UniXNU contributors. All rights reserved.
// Host test of WifiPmic.c against a model of the SPMI PMIC arbiter v7: the channel table (core + 0x2000), the owner table, the apps
// observer channels (EE 0: APID x 0x20) and the per-APID write channels. The seven WLAN rails are read back (STATUS, EN_CTL, set
// point) through the observer only; the sleep clock pin (PMK8550 gpio3, PPID 0x0ba) is left alone when it already runs, switched on
// with exactly sheng's five writes in order when the apps processor owns it and it is an LV/MV GPIO, and refused otherwise (another
// owner, another subtype, an arbiter that is not v7) with no write at all.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../WifiPmic.h"

static unsigned checks;
#define CHECK(x) do {if (!(x)) {fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1);} ++checks;} while (0)

enum {APIDS = 300};
static struct {
    uint32_t version;
    uint16_t ppid[APIDS];
    uint8_t owner[APIDS];
    uint8_t regs[APIDS][256];
    uint32_t obs_cmd[APIDS], chan_wdata[APIDS], chan_status[APIDS];
    int fail_status, deny;
    unsigned writes;
    struct {unsigned apid; uint8_t reg, value;} log[16];
} A;

static uint32_t core(void *ctx, uint32_t off)
{
    (void)ctx;
    if (off == 0) return A.version;
    if (off == 4) return APIDS;
    if (off >= 0x2000 && off < 0x2000 + 4 * APIDS) {
        const unsigned i = (off - 0x2000) / 4;
        return A.ppid[i] ? (uint32_t)A.ppid[i] << 8 : 0;
    }
    return 0;
}
static uint32_t cnfg(void *ctx, uint32_t off) {(void)ctx; CHECK(off / 4 < APIDS); return A.owner[off / 4];}
static uint32_t obs_read(void *ctx, uint32_t off)
{
    (void)ctx;
    const unsigned apid = off / 0x20, r = off % 0x20;
    CHECK(apid < APIDS);
    if (r == 0x08) return A.fail_status ? 2u : 1u;
    if (r == 0x18) return A.regs[apid][(A.obs_cmd[apid] >> 4) & 0xff];
    return 0;
}
static void obs_write(void *ctx, uint32_t off, uint32_t value)
{
    (void)ctx;
    const unsigned apid = off / 0x20;
    CHECK(apid < APIDS && off % 0x20 == 0 && (value >> 27) == 1 && (value & 0xf) == 0);   /* a one-byte read command */
    A.obs_cmd[apid] = value;
}
static uint32_t chan_read(void *ctx, unsigned apid, uint32_t off) {(void)ctx; CHECK(apid < APIDS && off == 0x08); return A.chan_status[apid];}
static void chan_write(void *ctx, unsigned apid, uint32_t off, uint32_t value)
{
    (void)ctx;
    CHECK(apid < APIDS && A.owner[apid] == 0);
    if (off == 0x10) {A.chan_wdata[apid] = value; return;}
    CHECK(off == 0x00 && (value >> 27) == 0 && (value & 0xf) == 0);   /* a one-byte write command */
    if (A.deny) {A.chan_status[apid] = 4; return;}                     /* denied: nothing changes */
    const uint8_t reg = (uint8_t)(value >> 4);
    A.regs[apid][reg] = (uint8_t)A.chan_wdata[apid];
    CHECK(A.writes < 16);
    A.log[A.writes].apid = apid;
    A.log[A.writes].reg = reg;
    A.log[A.writes].value = (uint8_t)A.chan_wdata[apid];
    A.writes++;
    A.chan_status[apid] = 1;
}
static void delay(void *ctx, unsigned us) {(void)ctx; (void)us;}
static const PwSpmiIo io = {NULL, core, cnfg, obs_read, obs_write, chan_read, chan_write, delay};

static void reset(void)
{
    memset(&A, 0, sizeof A);
    A.version = 0x70020000;
    /* the seven rails (PaganiWifiTables.h kRails) and the sleep clock pin; one PPID twice (another EE's channel first) */
    const uint16_t rails[7] = {0x5c3, 0x5c2, 0x1c8, 0x3a4, 0x5a7, 0x6a1, 0x8ad};
    const uint16_t mv[7] = {1800, 1200, 2800, 876, 876, 1860, 1312};
    for (unsigned i = 0; i < 7; ++i) {
        const unsigned apid = 40 + 13 * i;
        A.ppid[apid] = rails[i];
        A.owner[apid] = 0;
        A.regs[apid][0x08] = 0x80;
        A.regs[apid][0x46] = 0x80;
        A.regs[apid][0x45] = 6;
        A.regs[apid][0x40] = (uint8_t)mv[i];
        A.regs[apid][0x41] = (uint8_t)(mv[i] >> 8);
    }
    A.ppid[200] = 0x8ad; A.owner[200] = 3;                    /* the same rail through the modem's channel: not ours */
    A.ppid[150] = PW_SLEEP_CLOCK_PPID;
    A.owner[150] = 0;
    A.regs[150][0x04] = 0x10;                                  /* GPIO */
    A.regs[150][0x05] = 0x11;                                  /* LV/MV subtype */
}

int main(void)
{
    reset();
    int owner = -1;
    CHECK(pw_spmi_find(&io, 0x8ad, &owner) == 40 + 13 * 6 && owner == 0);   /* the apps channel wins over the modem's */
    CHECK(pw_spmi_find(&io, 0x123, &owner) == PwSpmiNotFound);
    uint8_t st = 0, en = 0, md = 0;
    uint16_t mv = 0;
    CHECK(pw_pmic_rail(&io, 0x8ad, &st, &en, &md, &mv) == 0 && st == 0x80 && en == 0x80 && md == 6 && mv == 1312);
    CHECK(pw_pmic_rail(&io, 0x1c8, &st, &en, &md, &mv) == 0 && mv == 2800);
    A.fail_status = 1;
    CHECK(pw_pmic_rail(&io, 0x5c3, &st, &en, &md, &mv) == PwSpmiIoError);
    A.fail_status = 0;
    CHECK(A.writes == 0);

    /* the sleep clock: off; a read-only look writes nothing; then switched on with sheng's five writes, in order */
    PwSleepClock s;
    CHECK(pw_pmic_sleep_clock(&io, 0, &s) == 0 && s.apid == 150 && s.owner == 0 && A.writes == 0);
    CHECK(pw_pmic_sleep_clock(&io, 1, &s) == 2 && A.writes == 5 && s.written == 5);
    const uint8_t want[5][2] = {{0x42, 5}, {0x41, 0}, {0x44, 2}, {0x40, 1}, {0x46, 0x80}};
    for (unsigned i = 0; i < 5; ++i) CHECK(A.log[i].apid == 150 && A.log[i].reg == want[i][0] && A.log[i].value == want[i][1]);
    CHECK(s.after[2] == 1 && s.after[6] == 2 && s.after[8] == 0x80);
    /* already running: nothing written */
    CHECK(pw_pmic_sleep_clock(&io, 1, &s) == 1 && A.writes == 5);
    /* another EE owns the pin; another subtype; an arbiter that is not v7: refused, no write */
    reset();
    A.owner[150] = 2;
    CHECK(pw_pmic_sleep_clock(&io, 1, &s) == PwSpmiNotOwner && A.writes == 0);
    reset();
    A.regs[150][0x05] = 0x09;
    CHECK(pw_pmic_sleep_clock(&io, 1, &s) == PwSpmiLayout && A.writes == 0);
    reset();
    A.version = 0x80000000;
    CHECK(pw_pmic_sleep_clock(&io, 1, &s) == PwSpmiLayout && pw_pmic_rail(&io, 0x8ad, &st, &en, &md, &mv) == PwSpmiLayout && A.writes == 0);
    /* a write the arbiter refuses stops the sequence there */
    reset();
    A.deny = 1;
    CHECK(pw_pmic_sleep_clock(&io, 1, &s) == PwSpmiIoError && A.writes == 0 && s.written == 0);
    printf("PASS PMIC: %u checks\n", checks);
    return 0;
}
