/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* Host tests of PlatformRtc.h and PlatformHalt.h: the channel lookup against a fake SPMI PMIC arbiter v7, the RTC read against a model that
 * ticks at any point of a read (a carry through every byte), every failure of the arbiter, the offset and the floor, a time set from iOS,
 * and the table of haltRestart's requests. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../PlatformHalt.h"
#include "../PlatformRtc.h"

static unsigned checks;
#define CHECK(x) do {if (!(x)) {fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #x); exit(1);} ++checks;} while (0)

/* The arbiter: channel table, owners, and the observer of channel `chan_apid` with the RTC behind it. */
static uint32_t version, channels, map_table[1024], owner_table[1024];
static int chan_apid;
static uint32_t rtc_seconds;
static uint8_t rtc_ctrl, rtc_type;
static unsigned commands;             /* read commands so far */
static unsigned tick_at, tick_every;  /* the counter moves on before command number tick_at, or before every tick_every-th */
static uint32_t status_now;           /* status of the last command */
static uint8_t data_now;
static uint32_t force_status;         /* nonzero: every command ends with this status */
static uint8_t force_reg;             /* with force_status: only for this register (0: all) */
static int never_done;
static unsigned delays;
static uint8_t reads_by_reg[256];

static uint32_t core(void *ctx, uint32_t at)
{
    CHECK(ctx == &version);
    if (at == 0) return version;
    if (at == 4) return 0xf800u | channels;   /* the bits above the count are not the count */
    CHECK(at >= 0x2000 && at < 0x2000 + 4 * 1024 && !(at & 3));
    return map_table[(at - 0x2000) / 4];
}

static uint32_t cnfg(void *ctx, uint32_t at)
{
    (void)ctx;
    CHECK(at < 4 * 1024 && !(at & 3));
    return owner_table[at / 4] | 0xfff0u;     /* only bits 2:0 are the owner */
}

static void obs_write(void *ctx, uint32_t at, uint32_t value)
{
    (void)ctx;
    /* The one write there is: a one-byte read command (opcode 1) to the channel's command register. Nothing else may ever go out. */
    CHECK(at == (uint32_t)chan_apid * 0x20u);
    CHECK((value >> 27) == 1 && !(value & 7u) && !(value & 0x07fff000u));
    const uint8_t reg = (uint8_t)(value >> 4);
    CHECK(reg == 4 || reg == PRTC_REG_CTRL || (reg >= 0x48 && reg <= 0x4b));
    reads_by_reg[reg]++;
    commands++;
    if (commands == tick_at || (tick_every && !(commands % tick_every))) rtc_seconds++;
    const unsigned byte = (unsigned)(reg - 0x48) * 8;
    data_now = reg == PRTC_REG_CTRL ? rtc_ctrl : reg == 4 ? rtc_type : (uint8_t)(rtc_seconds >> byte);
    status_now = 1;
    if (force_status && (!force_reg || force_reg == reg)) status_now = force_status;
    if (never_done) status_now = 0;
}

static uint32_t obs_read(void *ctx, uint32_t at)
{
    (void)ctx;
    const uint32_t channel = (uint32_t)chan_apid * 0x20u;
    if (at == channel + 8) return status_now;
    CHECK(at == channel + 0x18);
    return 0xffffff00u | data_now;            /* only the low byte is the register */
}

static void delay_us(void *ctx, unsigned us)
{
    (void)ctx;
    CHECK(us == 1);
    delays++;
}

static const PrtcIo io = {&version, core, cnfg, obs_read, obs_write, delay_us};

static void model(int apid)
{
    memset(reads_by_reg, 0, sizeof reads_by_reg);
    chan_apid = apid;
    rtc_ctrl = 0x80; rtc_seconds = 0;
    commands = 0; tick_at = 0; tick_every = 0; force_status = 0; force_reg = 0; never_done = 0; delays = 0;
    status_now = 0; data_now = 0;
}

static void test_decode(void)
{
    const uint8_t rdata[4] = {0x78, 0x56, 0x34, 0x92};
    CHECK(prtc_decode(rdata) == 0x92345678u);
    const uint8_t top[4] = {0, 0, 0, 0xff};
    CHECK(prtc_decode(top) == 0xff000000u);       /* no sign extension from the top byte */
}

static void test_find(void)
{
    memset(map_table, 0, sizeof map_table);
    memset(owner_table, 0, sizeof owner_table);
    version = 0x70060000u; channels = 300;
    for (uint32_t n = 0; n < channels; ++n) {
        map_table[n] = (0x700u | (n & 0xffu)) << 8 | 1u << 24;    /* slave 7's peripherals: never the RTC's */
        owner_table[n] = 3;
    }
    CHECK(prtc_find(&io, PRTC_PPID) == PRTC_ENODEV);
    map_table[17] = PRTC_PPID << 8;
    owner_table[17] = 2;
    CHECK(prtc_find(&io, PRTC_PPID) == 17);       /* someone else's channel: the observer reads it all the same */
    map_table[40] = PRTC_PPID << 8 | 1u << 24;
    owner_table[40] = 0;
    CHECK(prtc_find(&io, PRTC_PPID) == 40);       /* the apps processor's wins, whatever the order */
    map_table[60] = PRTC_PPID << 8;
    owner_table[60] = 4;
    CHECK(prtc_find(&io, PRTC_PPID) == 40);
    owner_table[40] = 5;
    CHECK(prtc_find(&io, PRTC_PPID) == 60 || prtc_find(&io, PRTC_PPID) == 17);
    map_table[300] = PRTC_PPID << 8;              /* past the count */
    owner_table[300] = 0;
    CHECK(prtc_find(&io, PRTC_PPID) != 300);
    map_table[299] = 0;
    CHECK(prtc_find(&io, 0xfffu) == PRTC_ENODEV);
    version = 0x60000000u;
    CHECK(prtc_find(&io, PRTC_PPID) == PRTC_ENODEV);
    version = 0x80000000u;
    CHECK(prtc_find(&io, PRTC_PPID) == PRTC_ENODEV);
    version = 0x7fffffffu; channels = 0;
    CHECK(prtc_find(&io, PRTC_PPID) == PRTC_ENODEV);
    channels = 1025;                              /* the 11-bit field can say more than the table holds */
    CHECK(prtc_find(&io, PRTC_PPID) == PRTC_ENODEV);
    version = 0x70000000u; channels = 1024;
    CHECK(prtc_find(&io, PRTC_PPID) >= 0);
}

static void test_read(void)
{
    uint32_t raw = 0;
    uint8_t ctrl = 0;
    model(40);
    rtc_seconds = 0x12345678u;
    CHECK(prtc_read_raw(&io, 40, &raw, &ctrl) == PRTC_OK);
    CHECK(raw == 0x12345678u && ctrl == 0x80 && commands == 9);
    CHECK(reads_by_reg[0x48] == 2 && reads_by_reg[0x49] == 2 && reads_by_reg[0x4a] == 2 && reads_by_reg[0x4b] == 2);

    /* Zero is a count like any other: two passes as well. */
    model(40);
    CHECK(prtc_read_raw(&io, 40, &raw, &ctrl) == PRTC_OK && raw == 0 && commands == 9);

    /* Other bits of the control register do not matter. */
    model(40);
    rtc_ctrl = 0xff; rtc_seconds = 7;
    CHECK(prtc_read_raw(&io, 40, &raw, &ctrl) == PRTC_OK && raw == 7 && ctrl == 0xff);

    /* A stopped counter: its control byte for the log, and not one data read. */
    model(40);
    rtc_ctrl = 0x7f; ctrl = 0;
    CHECK(prtc_read_raw(&io, 40, &raw, &ctrl) == PRTC_EDISABLED && ctrl == 0x7f && commands == 1);

    /* The largest count stays unsigned. */
    model(40);
    rtc_seconds = 0xffffffffu;
    CHECK(prtc_read_raw(&io, 40, &raw, &ctrl) == PRTC_OK && raw == 0xffffffffu);

    /* A carry through every byte, at every point of the read: never a torn value, whichever side of the tick it lands. */
    static const uint32_t carries[] = {0x000000ffu, 0x0000ffffu, 0x00ffffffu, 0xffffffffu, 0x12ffffffu, 0x1234ffffu};
    for (unsigned c = 0; c < sizeof carries / sizeof carries[0]; ++c) {
        for (unsigned at = 1; at <= 21; ++at) {
            model(40);
            rtc_seconds = carries[c]; tick_at = at;
            CHECK(prtc_read_raw(&io, 40, &raw, &ctrl) == PRTC_OK);
            CHECK(raw == carries[c] || raw == carries[c] + 1);
            /* Commands: 1 the control register, 2-5 the first pass, 6-9 the second. Before the second pass ended only its value may stand;
             * after it, the first two passes agreed on the old one. */
            if (at <= 5) CHECK(raw == carries[c] + 1);
            if (at >= 10) CHECK(raw == carries[c] && commands == 9);
        }
    }

    /* A tick in every pass: no two whole reads ever agree. */
    model(40);
    rtc_seconds = 1000; tick_every = 4;
    CHECK(prtc_read_raw(&io, 40, &raw, &ctrl) == PRTC_EUNSTABLE);
    CHECK(commands == 1 + 4 * 4);

    /* The arbiter's errors, on every register. */
    static const uint8_t regs[] = {PRTC_REG_CTRL, 0x48, 0x49, 0x4a, 0x4b};
    static const uint32_t errors[] = {0x2, 0x4, 0x8, 0x3, 0x5, 0x9};   /* failure, denied, dropped; alone or with done */
    for (unsigned r = 0; r < sizeof regs; ++r) {
        for (unsigned e = 0; e < sizeof errors / sizeof errors[0]; ++e) {
            model(40);
            force_status = errors[e]; force_reg = regs[r];
            raw = 0x55;
            CHECK(prtc_read_raw(&io, 40, &raw, &ctrl) == PRTC_EIO && raw == 0x55);
        }
    }

    /* A read that never finishes gives up after its polls. */
    model(40);
    never_done = 1;
    CHECK(prtc_read_raw(&io, 40, &raw, &ctrl) == PRTC_ETIMEDOUT && delays == PRTC_POLLS && commands == 1);

    /* A channel outside the window. */
    model(40);
    uint8_t value = 0;
    CHECK(prtc_read8(&io, -1, 0x46, &value) == PRTC_ENODEV && commands == 0);
    CHECK(prtc_read8(&io, 1024, 0x46, &value) == PRTC_ENODEV && commands == 0);
    CHECK(prtc_read8(&io, (int)0x80000000u, 0x46, &value) == PRTC_ENODEV && commands == 0);   /* its offset would wrap to channel 0 */
    model(1023);
    rtc_ctrl = 0x80;
    CHECK(prtc_read8(&io, 1023, 0x46, &value) == PRTC_OK && value == 0x80);
    model(40);
    rtc_type = 0xab;
    CHECK(prtc_read8(&io, 40, 4, &value) == PRTC_OK && value == 0xab);
}

static void test_floor(void)
{
    const uint8_t u32[4] = {0x80, 0x5c, 0x33, 0x6b};
    const uint8_t u64[8] = {0x80, 0x5c, 0x33, 0x6b, 0, 0, 0, 0};
    const uint8_t huge[8] = {0, 0, 0, 0, 0, 0, 0, 0x80};
    const uint8_t limit[8] = {0, 0, 0, 0, 0, 1, 0, 0};
    const uint8_t below[8] = {0xff, 0xff, 0xff, 0xff, 0xff, 0, 0, 0};
    CHECK(prtc_floor(u32, 4) == 0x6b335c80ll);
    CHECK(prtc_floor(u64, 8) == 0x6b335c80ll);
    CHECK(prtc_floor(u32, 3) == 0 && prtc_floor(u64, 5) == 0 && prtc_floor(u64, 0) == 0 && prtc_floor(NULL, 8) == 0);
    CHECK(prtc_floor(huge, 8) == 0 && prtc_floor(limit, 8) == 0);
    CHECK(prtc_floor(below, 8) == PRTC_LIMIT - 1);
}

#define NOW 1793400000ll    /* a time of about the phone's real calendar */

static void test_clock(void)
{
    PrtcClock c;
    /* pagani-rtc-offset: raw + offset, and it runs with the counter. */
    prtc_clock_start(&c, 1, NOW - 5000, 0);
    CHECK(prtc_clock_get(&c, 1, 5000, 3) == NOW && c.from == PRTC_FROM_ARG);
    CHECK(prtc_clock_get(&c, 1, 5010, 13) == NOW + 10);
    prtc_clock_start(&c, 1, -100, 0);             /* a counter ahead of the real time */
    CHECK(prtc_clock_get(&c, 1, 1000, 0) == 900);
    CHECK(prtc_clock_get(&c, 1, 50, 1) == 0);     /* the counter restarted: never below 0, the calendar's seconds are unsigned */
    prtc_clock_start(&c, 1, -5000, 0);
    CHECK(prtc_clock_get(&c, 1, 1000, 0) == 0);

    /* No offset: the first read starts at the floor, and the clock runs from there. */
    prtc_clock_start(&c, 0, 12345, NOW);
    CHECK(prtc_clock_get(&c, 1, 777, 2) == NOW && c.from == PRTC_FROM_FLOOR && c.offset == NOW - 777);
    CHECK(prtc_clock_get(&c, 1, 800, 25) == NOW + 23);

    /* No offset and a counter past the floor (a clock that holds Unix time): as it is. */
    prtc_clock_start(&c, 0, 0, NOW);
    CHECK(prtc_clock_get(&c, 1, (uint32_t)(NOW + 9), 2) == NOW + 9 && c.from == PRTC_FROM_RAW);

    /* Neither: the raw count. */
    prtc_clock_start(&c, 0, 0, 0);
    CHECK(prtc_clock_get(&c, 1, 4242, 2) == 4242 && c.from == PRTC_FROM_RAW);

    /* A stale offset (the counter restarted when the battery ran flat) cannot go below the floor either. */
    prtc_clock_start(&c, 1, 1000, NOW);
    CHECK(prtc_clock_get(&c, 1, 50, 2) == NOW && c.from == PRTC_FROM_FLOOR);
    CHECK(prtc_clock_get(&c, 1, 60, 12) == NOW + 10);

    /* A value XNU's parser made of a word's bytes is not an offset. */
    prtc_clock_start(&c, 1, 0x4142434445464748ll, NOW);
    CHECK(!c.have_arg);
    CHECK(prtc_clock_get(&c, 1, 10, 0) == NOW && c.from == PRTC_FROM_FLOOR);
    prtc_clock_start(&c, 1, -PRTC_LIMIT, 0);
    CHECK(!c.have_arg);
    prtc_clock_start(&c, 1, PRTC_LIMIT - 1, 0);
    CHECK(c.have_arg);

    /* The RTC unreadable from the start: the floor plus the uptime, an offset cannot apply. */
    prtc_clock_start(&c, 1, 7777, NOW);
    CHECK(prtc_clock_get(&c, 0, 0, 30) == NOW + 30 && c.from == PRTC_FROM_NONE);
    CHECK(prtc_clock_get(&c, 0, 0, 90) == NOW + 90);
    /* It comes back: the offset is fixed then. */
    CHECK(prtc_clock_get(&c, 1, 1, 91) == NOW);

    /* Without a floor as well: the uptime, as the calendar has it today. */
    prtc_clock_start(&c, 0, 0, 0);
    CHECK(prtc_clock_get(&c, 0, 0, 30) == 30);

    /* It goes away after a good read: the uptime carries on from the last time given out. */
    prtc_clock_start(&c, 1, 100, 0);
    CHECK(prtc_clock_get(&c, 1, 1000, 50) == 1100);
    CHECK(prtc_clock_get(&c, 0, 0, 60) == 1110);
    CHECK(prtc_clock_get(&c, 0, 0, 65) == 1115);
    CHECK(prtc_clock_get(&c, 1, 1020, 70) == 1120);

    /* A time set from iOS replaces the offset, for the boot-arg's and the floor's alike, and may lie below the floor. */
    prtc_clock_start(&c, 1, 100, NOW);
    CHECK(prtc_clock_get(&c, 1, 1000, 1) == NOW);
    prtc_clock_set(&c, 1, 1000, 1, 2000);
    CHECK(c.from == PRTC_FROM_SET && c.offset == 1000);
    CHECK(prtc_clock_get(&c, 1, 1010, 11) == 2010);

    /* Set before the first get. */
    prtc_clock_start(&c, 0, 0, NOW);
    prtc_clock_set(&c, 1, 10, 0, NOW + 500);
    CHECK(prtc_clock_get(&c, 1, 15, 5) == NOW + 505 && c.from == PRTC_FROM_SET);

    /* Set while the RTC cannot be read: the uptime keeps it, and the next good read goes on from it. */
    prtc_clock_start(&c, 1, 100, 0);
    prtc_clock_set(&c, 0, 0, 40, NOW);
    CHECK(prtc_clock_get(&c, 0, 0, 50) == NOW + 10);
    CHECK(prtc_clock_get(&c, 1, 9, 60) == NOW && !c.have_arg && c.from == PRTC_FROM_FLOOR);

    /* The same after a good read: the offset that was known is gone. */
    prtc_clock_start(&c, 1, 100, 0);
    CHECK(prtc_clock_get(&c, 1, 1000, 1) == 1100);
    prtc_clock_set(&c, 0, 0, 40, NOW);
    CHECK(!c.known && prtc_clock_get(&c, 1, 1040, 41) == NOW && c.from == PRTC_FROM_FLOOR);

    /* What cannot be a time changes nothing. */
    prtc_clock_start(&c, 1, 100, 0);
    CHECK(prtc_clock_get(&c, 1, 1000, 1) == 1100);
    prtc_clock_set(&c, 1, 1000, 1, -1);
    prtc_clock_set(&c, 1, 1000, 1, PRTC_LIMIT);
    prtc_clock_set(&c, 0, 0, 1, -1);
    CHECK(c.offset == 100 && c.known && c.from == PRTC_FROM_ARG && prtc_clock_get(&c, 1, 1001, 2) == 1101);

    /* A set at the epoch is a time. */
    prtc_clock_start(&c, 0, 0, 0);
    prtc_clock_set(&c, 1, 5, 0, 0);
    CHECK(c.known && c.offset == -5 && prtc_clock_get(&c, 1, 5, 0) == 0);
}

static void test_calendar(void)
{
    CHECK(!prtc_calendar_stale((uint64_t)NOW, NOW));
    CHECK(!prtc_calendar_stale((uint64_t)NOW + PRTC_LATE_SECONDS, NOW));
    CHECK(!prtc_calendar_stale((uint64_t)NOW - PRTC_LATE_SECONDS, NOW));
    CHECK(prtc_calendar_stale((uint64_t)NOW + PRTC_LATE_SECONDS + 1, NOW));
    CHECK(prtc_calendar_stale((uint64_t)NOW - PRTC_LATE_SECONDS - 1, NOW));
    CHECK(prtc_calendar_stale(3, NOW));           /* the calendar is the uptime: the boot's read found no platform expert */
    CHECK(!prtc_calendar_stale(3, 5));            /* no clock to speak of: nothing to fix */
}

/* Every request in every mode: the only ones that change are the three of pagani-halt, and only in modes 1 and 2. */
static void test_halt(void)
{
    /* IOPlatformExpert.h's numbers, which ArmDTPlatform.cpp checks against the header at compile time. */
    CHECK(PHALT_TYPE_HALT == 0 && PHALT_TYPE_RESTART == 1 && PHALT_TYPE_UPS_HALT == 3 && PHALT_TYPE_PANIC_RESTART == 4);
    static const unsigned modes[] = {0, 1, 2, 3, 4, 0x736579u, 0xffffffffu};
    for (unsigned m = 0; m < sizeof modes / sizeof modes[0]; ++m) {
        const unsigned mode = modes[m];
        for (unsigned type = 0; type < 64; ++type) {
            const char *reason = "untouched";
            const int action = pagani_halt_action(mode, type, &reason);
            const int on = mode == 1 || mode == 2;
            if (on && type == PHALT_TYPE_RESTART) {
                CHECK(action == PHALT_RESTART && !strcmp(reason, "restart"));
            } else if (on && type == PHALT_TYPE_PANIC_RESTART) {
                CHECK(action == PHALT_RESTART && !strcmp(reason, "panic restart"));
            } else if (mode == 1 && (type == PHALT_TYPE_HALT || type == PHALT_TYPE_UPS_HALT)) {
                CHECK(action == PHALT_POWER_OFF && !strcmp(reason, "power off"));
            } else if (mode == 2 && (type == PHALT_TYPE_HALT || type == PHALT_TYPE_UPS_HALT)) {
                CHECK(action == PHALT_RESTART && !strcmp(reason, "power off as restart"));
            } else {
                CHECK(action == PHALT_INHERIT && !strcmp(reason, "untouched"));
            }
        }
    }
}

int main(void)
{
    test_decode();
    test_find();
    test_read();
    test_floor();
    test_clock();
    test_calendar();
    test_halt();
    printf("PASS: %u checks: RTC decode, the channel lookup, reads with a carry at every point, every arbiter failure, the offset and the floor, "
           "a time set from iOS, the late calendar check, haltRestart's table\n", checks);
    return 0;
}
