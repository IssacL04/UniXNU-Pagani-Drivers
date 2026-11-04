/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* Host tests of BatteryGauge.c against a model of the NFG8011B's bq27541 standard commands: units, the current's sign, the range checks,
 * retries, a gauge that does not answer, slow fields and the device type query. */
#include <string.h>
#include "check.h"
#include "../BatteryGauge.h"

typedef struct {
    uint16_t reg[0x40];
    int fail[0x40];             /* failures still to come for a register; -1: always */
    int absent;                 /* no answer at all */
    unsigned reads[0x40];
    uint16_t subcommand, device_type;
    int wrong_echo, xfer_error;
    unsigned sleeps, slept_ms, xfers;
} Gauge;

static int g_read16(void *ctx, uint8_t reg, uint16_t *value)
{
    Gauge *g = ctx;
    CHECK(reg < 0x40 && !(reg & 1) && pagani_gauge_transfer_allowed(&reg, 1, 2));
    g->reads[reg]++;
    if (g->absent) return -6;
    if (g->fail[reg]) {
        if (g->fail[reg] > 0) g->fail[reg]--;
        return -110;
    }
    *value = g->reg[reg];
    return 0;
}

static int g_xfer(void *ctx, const uint8_t *tx, unsigned txn, uint8_t *rx, unsigned rxn)
{
    Gauge *g = ctx;
    g->xfers++;
    CHECK(pagani_gauge_transfer_allowed(tx, txn, rxn));   /* the owner's gate lets every transfer of the reader through */
    if (g->xfer_error) return g->xfer_error;
    CHECK(txn >= 1 && tx[0] == PgRegBlock);
    if (txn == 3 && !rxn) {
        g->subcommand = (uint16_t)(tx[1] | tx[2] << 8);
        return 0;
    }
    CHECK(txn == 1 && rxn == 4);
    const uint16_t echo = g->wrong_echo ? 0x0000 : g->subcommand;
    rx[0] = (uint8_t)echo; rx[1] = (uint8_t)(echo >> 8);
    rx[2] = (uint8_t)g->device_type; rx[3] = (uint8_t)(g->device_type >> 8);
    return 0;
}

static void g_sleep(void *ctx, unsigned ms)
{
    Gauge *g = ctx;
    g->sleeps++;
    g->slept_ms += ms;
}

static void battery(Gauge *g)
{
    memset(g, 0, sizeof *g);
    g->reg[PgRegVoltage] = 3950;
    g->reg[PgRegCurrent] = (uint16_t)(int16_t)-812;
    g->reg[PgRegTemperature] = 2731 + 295;    /* 29.5 C in 0.1 K */
    g->reg[PgRegSoc] = 87;
    g->reg[PgRegFlags] = PGAUGE_FLAG_DSG;
    g->reg[PgRegRemaining] = 3200;
    g->reg[PgRegFull] = 6100;
    g->reg[PgRegAvgCurrent] = (uint16_t)(int16_t)-800;
    g->reg[PgRegTimeToEmpty] = 360;
    g->reg[PgRegTimeToFull] = 0xffff;
    g->reg[PgRegCycles] = 37;
    g->reg[PgRegSoh] = 98;
    g->device_type = PGAUGE_DEVICE_NFG8011B;
}

static unsigned total_reads(const Gauge *g)
{
    unsigned n = 0;
    for (unsigned i = 0; i < 0x40; ++i) n += g->reads[i];
    return n;
}

int main(void)
{
    Gauge g;
    PgReading r;
    const PgBus bus = {&g, g_read16, g_xfer, g_sleep};

    /* A discharging battery, every field. */
    battery(&g);
    memset(&r, 0, sizeof r);
    CHECK(pagani_gauge_read(&bus, &r, 1) == PgCount);
    CHECK(r.valid == (1u << PgCount) - 1 && r.answered == r.valid);
    CHECK(r.voltage_mv == 3950 && r.current_ma == -812 && r.avg_current_ma == -800 && r.temperature_centi == 2950);
    CHECK(r.soc == 87 && r.flags == PGAUGE_FLAG_DSG && r.remaining_mah == 3200 && r.full_mah == 6100);
    CHECK(r.time_to_empty_min == 360 && r.time_to_full_min == -1 && r.cycles == 37 && r.soh == 98);
    CHECK(r.passes == 1 && r.reads == PgCount && !r.retries && !r.failures && !r.last_error);

    /* A fast pass leaves cycles and health alone (kept from before). */
    g.reg[PgRegCurrent] = 1500;   /* now charging: the sign is the charge's */
    g.reg[PgRegCycles] = 99;
    const unsigned cycles_reads = g.reads[PgRegCycles];
    CHECK(pagani_gauge_read(&bus, &r, 0) == PgCount - 2);
    CHECK(g.reads[PgRegCycles] == cycles_reads && g.reads[PgRegSoh] == 1);
    CHECK(r.current_ma == 1500 && r.cycles == 37 && pagani_gauge_has(&r, PgCycles));

    /* The temperature's ends: -40 and 90 C read, beyond them not; below zero converts. */
    battery(&g);
    g.reg[PgRegTemperature] = 2731 - 50;
    pagani_gauge_read(&bus, &r, 0);
    CHECK(pagani_gauge_has(&r, PgTemperature) && r.temperature_centi == -500);
    g.reg[PgRegTemperature] = 2331;
    pagani_gauge_read(&bus, &r, 0);
    CHECK(pagani_gauge_has(&r, PgTemperature) && r.temperature_centi == -4000);
    g.reg[PgRegTemperature] = 2330;
    pagani_gauge_read(&bus, &r, 0);
    CHECK(!pagani_gauge_has(&r, PgTemperature));
    g.reg[PgRegTemperature] = 3631;
    pagani_gauge_read(&bus, &r, 0);
    CHECK(pagani_gauge_has(&r, PgTemperature) && r.temperature_centi == 9000);
    g.reg[PgRegTemperature] = 3632;
    pagani_gauge_read(&bus, &r, 0);
    CHECK(!pagani_gauge_has(&r, PgTemperature));
    g.reg[PgRegTemperature] = 0;   /* a gauge being reset */
    pagani_gauge_read(&bus, &r, 0);
    CHECK(!pagani_gauge_has(&r, PgTemperature));

    /* What a gauge in reset or a broken bus answers is not used. */
    battery(&g);
    g.reg[PgRegVoltage] = 0xffff;
    g.reg[PgRegSoc] = 101;
    g.reg[PgRegFull] = 0;
    g.reg[PgRegCurrent] = 0x8000;
    g.reg[PgRegAvgCurrent] = 17000;
    g.reg[PgRegTimeToEmpty] = 7000;
    g.reg[PgRegRemaining] = 0xffff;
    pagani_gauge_read(&bus, &r, 1);
    CHECK(!pagani_gauge_has(&r, PgVoltage) && !pagani_gauge_has(&r, PgSoc) && !pagani_gauge_has(&r, PgFull));
    CHECK(!pagani_gauge_has(&r, PgCurrent) && !pagani_gauge_has(&r, PgAvgCurrent) && !pagani_gauge_has(&r, PgTimeToEmpty));
    CHECK(!pagani_gauge_has(&r, PgRemaining) && pagani_gauge_has(&r, PgTemperature) && pagani_gauge_has(&r, PgFlags));
    g.reg[PgRegVoltage] = 2499;
    pagani_gauge_read(&bus, &r, 0);
    CHECK(!pagani_gauge_has(&r, PgVoltage));
    g.reg[PgRegVoltage] = 5001;
    pagani_gauge_read(&bus, &r, 0);
    CHECK(!pagani_gauge_has(&r, PgVoltage));
    g.reg[PgRegVoltage] = 2500;
    g.reg[PgRegSoc] = 0;
    pagani_gauge_read(&bus, &r, 0);
    CHECK(pagani_gauge_has(&r, PgVoltage) && pagani_gauge_has(&r, PgSoc) && r.soc == 0);

    /* Retries: two failures, then the answer (5 ms apart); three failures drop the field and the pass goes on. */
    battery(&g);
    memset(&r, 0, sizeof r);
    g.fail[PgRegSoc] = 2;
    CHECK(pagani_gauge_read(&bus, &r, 0) == PgCount - 2);
    CHECK(r.retries == 2 && g.sleeps == 2 && g.slept_ms == 2 * PGAUGE_RETRY_MS && pagani_gauge_has(&r, PgSoc) && r.soc == 87);
    g.fail[PgRegFull] = -1;
    CHECK(pagani_gauge_read(&bus, &r, 0) == PgCount - 3);
    CHECK(!pagani_gauge_has(&r, PgFull) && r.failures == 1 && r.last_error == -110 && g.reads[PgRegFull] == 1 + PGAUGE_TRIES);
    g.fail[PgRegFull] = 0;
    CHECK(pagani_gauge_read(&bus, &r, 0) == PgCount - 2 && pagani_gauge_has(&r, PgFull));

    /* No answer at all: three tries of the voltage and nothing else; what the pass would read is no longer valid. */
    battery(&g);
    memset(&r, 0, sizeof r);
    CHECK(pagani_gauge_read(&bus, &r, 1) == PgCount);
    g.absent = 1;
    memset(g.reads, 0, sizeof g.reads);
    CHECK(pagani_gauge_read(&bus, &r, 0) == -6);
    CHECK(total_reads(&g) == PGAUGE_TRIES && g.reads[PgRegVoltage] == PGAUGE_TRIES);
    CHECK(!pagani_gauge_has(&r, PgVoltage) && !pagani_gauge_has(&r, PgSoc) && !pagani_gauge_has(&r, PgCurrent));
    CHECK(pagani_gauge_has(&r, PgCycles) && pagani_gauge_has(&r, PgSoh));   /* not this pass's fields */
    CHECK(pagani_gauge_read(&bus, &r, 1) == -6 && !r.valid);
    g.absent = 0;
    CHECK(pagani_gauge_read(&bus, &r, 1) == PgCount && r.valid == (1u << PgCount) - 1);

    /* The device type. */
    battery(&g);
    uint16_t type = 0;
    CHECK(pagani_gauge_device_type(&bus, &type) == 0 && type == PGAUGE_DEVICE_NFG8011B && g.subcommand == 0x0001 && g.xfers == 2);
    g.wrong_echo = 1;
    type = 0;
    CHECK(pagani_gauge_device_type(&bus, &type) == -71 && type == 0 && g.xfers == 2 + 2 * PGAUGE_TRIES);
    g.wrong_echo = 0;
    g.xfer_error = -6;
    CHECK(pagani_gauge_device_type(&bus, &type) == -6);
    CHECK(pagani_gauge_device_type(0, &type) == -22);
    const PgBus no_xfer = {&g, g_read16, 0, g_sleep};
    CHECK(pagani_gauge_device_type(&no_xfer, &type) == -22);
    CHECK(pagani_gauge_read(0, &r, 0) == -22);

    /* What may go to the gauge: register reads (an address, then 1..4 bytes) and the DEVICE_TYPE subcommand, nothing else. Every write
     * the reader itself makes passes; the protection FET subcommands of the bq28z610 family (0x001f CHG FET, 0x0020 DSG FET, 0x0022 FET
     * control), a seal or reset (0x0030, 0x0041), any data written behind an address and any write with a read are refused. */
    static const uint8_t device_type[3] = {PgRegBlock, 0x01, 0x00};
    CHECK(pagani_gauge_transfer_allowed(device_type, 3, 0));
    for (unsigned reg = 0; reg < 0x80; ++reg) {
        const uint8_t a = (uint8_t)reg;
        CHECK(pagani_gauge_transfer_allowed(&a, 1, 2) && pagani_gauge_transfer_allowed(&a, 1, 4) && pagani_gauge_transfer_allowed(&a, 1, 1));
        CHECK(!pagani_gauge_transfer_allowed(&a, 1, 0) && !pagani_gauge_transfer_allowed(&a, 1, 5));
    }
    const uint8_t high = 0x80;
    CHECK(!pagani_gauge_transfer_allowed(&high, 1, 2));
    static const uint16_t forbidden[] = {0x001f, 0x0020, 0x0022, 0x0030, 0x0041, 0x0000, 0x0002, 0x0100, 0x0101, 0xffff};
    for (unsigned i = 0; i < sizeof forbidden / sizeof forbidden[0]; ++i) {
        const uint8_t cmd[3] = {PgRegBlock, (uint8_t)forbidden[i], (uint8_t)(forbidden[i] >> 8)};
        CHECK(!pagani_gauge_transfer_allowed(cmd, 3, 0) && !pagani_gauge_transfer_allowed(cmd, 3, 4));
    }
    for (unsigned reg = 0; reg < 0x80; ++reg) {
        const uint8_t write[3] = {(uint8_t)reg, 0x01, 0x00};
        CHECK(pagani_gauge_transfer_allowed(write, 3, 0) == (reg == PgRegBlock));
        const uint8_t word[2] = {(uint8_t)reg, 0x00};
        CHECK(!pagani_gauge_transfer_allowed(word, 2, 0) && !pagani_gauge_transfer_allowed(word, 2, 2));
    }
    CHECK(!pagani_gauge_transfer_allowed(device_type, 3, 4) && !pagani_gauge_transfer_allowed(0, 1, 2) &&
          !pagani_gauge_transfer_allowed(device_type, 0, 2));
    /* the reader's own transfers, recorded through a gate: each one allowed */
    battery(&g);
    memset(&r, 0, sizeof r);
    CHECK(pagani_gauge_read(&bus, &r, 1) == PgCount);
    for (unsigned f = 0; f < 0x40; ++f) {
        const uint8_t a = (uint8_t)f;
        if (g.reads[f]) CHECK(pagani_gauge_transfer_allowed(&a, 1, 2));
    }

    printf("PASS gauge: %u checks\n", checks);
    return 0;
}
