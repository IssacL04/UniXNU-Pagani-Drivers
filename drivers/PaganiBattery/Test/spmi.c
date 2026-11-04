/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* Host tests of BatterySpmi.c against a fake SPMI PMIC arbiter v7: the slave's channels and their owners, interrupt ownership, the
 * TYPE/SUBTYPE reads, a foreign arbiter version, the log lines (every entry once, progress with a short buffer); the channels of the
 * peripherals the charger's state and the USB probe read, and what those registers say, against the samples Android dumped. */
#include <string.h>
#include "check.h"
#include "../BatterySpmi.h"

static uint32_t version = 0x70060000u, channels = 300;
static uint32_t map[1024], owner[1024];
static unsigned reads8, core_reads;
static int read_fail;

static uint32_t core(void *ctx, uint32_t at)
{
    (void)ctx;
    core_reads++;
    if (at == 0) return version;
    if (at == 4) return 0xf800u | channels;   /* the bits above the count are not the count */
    CHECK(at >= 0x2000 && at < 0x2000 + 4 * 1024 && !(at & 3));
    return map[(at - 0x2000) / 4];
}
static uint32_t cnfg(void *ctx, uint32_t at)
{
    (void)ctx;
    CHECK(at < 4 * 1024 && !(at & 3));
    return owner[at / 4] | 0xfff0u;           /* only bits 2:0 are the owner */
}
static int read8(void *ctx, int apid, uint8_t offset, uint8_t *value)
{
    (void)ctx;
    reads8++;
    if (read_fail) return -110;
    CHECK(offset == 4 || offset == 5);
    *value = (uint8_t)(offset == 4 ? 0x20 + (apid & 0xf) : apid >> 4);
    return 0;
}

static uint8_t charger[0x1000][256];
static int charger_read(void *ctx, unsigned ppid, uint8_t offset, uint8_t *value)
{
    unsigned *left = ctx;   /* null: every read works; else the reads that still work */
    if (left && !(*left)--) return -5;
    CHECK(pagani_spmi_index(ppid) >= 0);
    *value = charger[ppid][offset];
    return 0;
}

static void test_channels(void)
{
    /* the channels of the listed peripherals, as the survey of round 1101 found them: two mappings of most, EE 3 before EE 0 */
    memset(map, 0, sizeof map);
    memset(owner, 0, sizeof owner);
    channels = 896;
    version = 0x70020000u;
    uint32_t n = 10;
    for (unsigned i = 0; i < PBS_PERIPHERALS; ++i) {
        map[n] = (uint32_t)pagani_spmi_peripherals[i] << 8;
        owner[n] = 3;
        n += 7;
    }
    for (unsigned i = 0; i < PBS_PERIPHERALS; ++i) {
        if (i == 5) continue;       /* one without an apps mapping: the other owner's channel still reads */
        map[n] = (uint32_t)pagani_spmi_peripherals[i] << 8 | 1u << 24;
        owner[n] = 0;
        n += 3;
    }
    map[n] = 0x72600;               /* a later foreign owner does not take it back */
    owner[n] = 2;
    PbsIo io = {0, core, cnfg, 0};
    int apid[PBS_PERIPHERALS];
    CHECK(pagani_spmi_map(&io, apid) == 0);
    CHECK(apid[0] == (int)(10 + 7 * PBS_PERIPHERALS) && apid[5] == 10 + 7 * 5);
    for (unsigned i = 1; i < PBS_PERIPHERALS; ++i) if (i != 5) CHECK(apid[i] == (int)(10 + 7 * PBS_PERIPHERALS + 3 * (i < 5 ? i : i - 1)));
    /* a peripheral with no channel at all */
    map[10 + 7 * 5] = 0;
    CHECK(pagani_spmi_map(&io, apid) == 0 && apid[5] == -19);
    /* not the v7 arbiter: nothing is found */
    version = 0x60000000u;
    CHECK(pagani_spmi_map(&io, apid) == -19 && apid[0] == -19);
    version = 0x70020000u;
    CHECK(pagani_spmi_map(0, apid) == -22 && apid[0] == -19);
    /* the list: the charger's peripherals, the repeater, the LDOs of the PHY and the repeater by their devicetree names; no others */
    CHECK(pagani_spmi_index(0x729) >= 0 && pagani_spmi_index(0x7fd) >= 0 && pagani_spmi_index(0x3c2) >= 0 && pagani_spmi_index(0x6c3) >= 0);
    CHECK(pagani_spmi_index(0x5c1) >= 0 && pagani_spmi_index(0x1cf) >= 0 && pagani_spmi_index(0x1c5) >= 0);
    CHECK(pagani_spmi_index(0x72a) == -19 && pagani_spmi_index(0x000) == -19 && pagani_spmi_index(0x7f0) == -19);
}

static void test_charger(void)
{
    /* with VBUS, as sm8450_st_dump has it (794 samples of one boot) */
    charger[0x729][0x10] = 0x11; charger[0x729][0x06] = 0x02; charger[0x729][0x08] = 0x0f; charger[0x72b][0x06] = 0x02;
    charger[0x72b][0x0b] = 0xab; charger[0x726][0x06] = 0x87; charger[0x729][0x52] = 0x28; charger[0x727][0x06] = 0x28; charger[0x727][0x09] = 0x28;
    PbsCharger c;
    CHECK(pagani_spmi_charger(charger_read, 0, &c) == 0);
    CHECK(pagani_spmi_plugged(&c) && pagani_spmi_stage(&c) == PbsStageDisabled && !pagani_spmi_stage_charging(pagani_spmi_stage(&c)));
    CHECK(c.reg[PbsUsbinLimit] == 0x28 && c.reg[PbsAiclMax] == 0x28 && c.reg[PbsAiclNow] == 0x28 && c.reg[PbsTypecSink] == 2);
    char line[160];
    PbatText t;
    pbat_text_start(&t, line, sizeof line);
    pagani_spmi_charger_text(&t, &c);
    CHECK(!strcmp(line, " stage=7 rt=0x11 in=0x2 det=0xf rp=0x2 tc=0xab st=0x87 icl=2000 imax=2000 inow=2000"));
    /* the one sample of VBUS gone */
    charger[0x729][0x10] = 0x04;
    CHECK(pagani_spmi_charger(charger_read, 0, &c) == 0 && !pagani_spmi_plugged(&c));
    /* the stages seen and the rest of the list */
    for (uint8_t v = 0; v < 8; ++v) {
        charger[0x726][0x06] = (uint8_t)(0x80 | v);
        CHECK(pagani_spmi_charger(charger_read, 0, &c) == 0 && pagani_spmi_stage(&c) == v);
        CHECK(pagani_spmi_stage_charging(v) == (v >= 1 && v <= 4));
    }
    /* any one read that fails fails the sample */
    for (unsigned k = 0; k < PbsChargerRegs; ++k) {
        unsigned left = k;
        CHECK(pagani_spmi_charger(charger_read, &left, &c) == -5);
    }
    unsigned left = PbsChargerRegs;
    CHECK(pagani_spmi_charger(charger_read, &left, &c) == 0);
    CHECK(pagani_spmi_charger(0, 0, &c) == -22);
}

int main(void)
{
    /* slave 0 and slave 7 peripherals interleaved; slave 7's charger range owned by EE 3, a few by the apps (EE 0) */
    for (uint32_t n = 0; n < channels; ++n) {
        const uint32_t sid = n % 3 == 0 ? 7 : 0;
        map[n] = (sid << 8 | (n & 0xff)) << 8 | (n % 5 == 0 ? 1u << 24 : 0);
        owner[n] = sid == 7 && n % 2 ? 3 : 0;
    }
    map[3] = 0;   /* an unused channel */
    PbsIo io = {0, core, cnfg, read8};
    PbsSurvey s;
    CHECK(pagani_spmi_survey(&io, 7, 0, &s) == 0);
    CHECK(s.version == 0x70060000u && s.channels == 300 && s.count == 99 && !reads8);
    CHECK(s.per_owner[3] + s.per_owner[0] == 99 && s.per_owner[3] == 49 && s.per_owner[0] == 50);
    CHECK(s.ppid[0] == 0x700 && s.owner[0] == 0 && s.apid[1] == 6 && s.ppid[1] == 0x706 && !s.typed[0]);
    CHECK(s.irq_owned == 20);
    /* the types too */
    CHECK(pagani_spmi_survey(&io, 7, 1, &s) == 0 && s.typed[5] && reads8 == 2 * PBS_MAX);
    CHECK(s.type[5] == 0x20 + (s.apid[5] & 0xf) && s.subtype[5] == s.apid[5] >> 4);
    read_fail = 1;
    CHECK(pagani_spmi_survey(&io, 7, 1, &s) == 0 && !s.typed[0]);
    read_fail = 0;
    /* lines: the summary, then every kept entry exactly once */
    char line[200];
    unsigned next = 0, lines = 0, entries = 0;
    CHECK(pagani_spmi_survey(&io, 7, 1, &s) == 0);
    pagani_spmi_line(&s, 7, &next, line, sizeof line);
    CHECK(!strncmp(line, "[battery] spmi sid7 v=0x70060000 ch=300 n=99 ee0=50 ee3=49 irq0=", 64) && next == 1);
    do {
        const size_t n = pagani_spmi_line(&s, 7, &next, line, sizeof line);
        CHECK(n < sizeof line && !strncmp(line, "[battery] spmi sid7 +", 21));
        for (const char *p = line + 21; *p; ++p) if (*p == ' ') entries++;   /* one space before each entry */
        CHECK(++lines < 40);
    } while (next);
    CHECK(s.count == 99 && entries == PBS_MAX && lines == 6);   /* 99 found, the first 96 kept, 16 to a line */
    /* a buffer too short for one entry still moves on */
    next = 1;
    unsigned rounds = 0;
    do {pagani_spmi_line(&s, 7, &next, line, 30); CHECK(++rounds <= PBS_MAX + 1);} while (next);
    /* not the v7 arbiter: nothing but its two id registers is read */
    version = 0x50000000u;
    core_reads = 0;
    CHECK(pagani_spmi_survey(&io, 7, 1, &s) == -19 && core_reads == 2);
    version = 0x70060000u;
    channels = 0;
    CHECK(pagani_spmi_survey(&io, 7, 1, &s) == -19);
    channels = 300;
    CHECK(pagani_spmi_survey(&io, 16, 1, &s) == -22 && pagani_spmi_survey(0, 7, 1, &s) == -22);
    /* a slave with nothing: one summary line */
    CHECK(pagani_spmi_survey(&io, 9, 0, &s) == 0 && s.count == 0);
    next = 0;
    pagani_spmi_line(&s, 9, &next, line, sizeof line);
    CHECK(next == 0 && strstr(line, "n=0"));
    test_channels();
    test_charger();
    printf("PASS spmi: %u checks\n", checks);
    return 0;
}
