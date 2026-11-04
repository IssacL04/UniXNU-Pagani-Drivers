/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* The NFG8011B reader (BatteryGauge.h). Register numbers, units and the current's sign are facts of TI's bq27541 and bq28z610 data
 * sheets and of the OPLUS gauge driver's use of this part (oplus_hal_bq27541.c/oplus_hal_nfg8011b.h: the standard command set with the
 * current at 0x0c, three retries 5 ms apart, the device type through block command 0x3e); no code of that driver is used. */
#include "BatteryGauge.h"

static const uint8_t kRegister[PgCount] = {
    [PgVoltage] = PgRegVoltage, [PgCurrent] = PgRegCurrent, [PgTemperature] = PgRegTemperature, [PgSoc] = PgRegSoc,
    [PgFlags] = PgRegFlags, [PgRemaining] = PgRegRemaining, [PgFull] = PgRegFull, [PgAvgCurrent] = PgRegAvgCurrent,
    [PgTimeToEmpty] = PgRegTimeToEmpty, [PgTimeToFull] = PgRegTimeToFull, [PgCycles] = PgRegCycles, [PgSoh] = PgRegSoh,
};
#define SLOW_FIELDS ((1u << PgCycles) | (1u << PgSoh))
#define ALL_FIELDS ((1u << PgCount) - 1u)
#define ZERO_C_DECIKELVIN 2731

static int32_t signed16(uint16_t v) {return v & 0x8000u ? (int32_t)v - 0x10000 : (int32_t)v;}

static void field(PgReading *r, int f, int ok)
{
    if (ok) r->valid |= 1u << f;
    else r->valid &= ~(1u << f);
}

void pagani_gauge_decode(PgReading *r, uint32_t fields)
{
    for (int f = 0; f < PgCount; ++f) {
        if (!((fields >> f) & 1u)) continue;
        if (!((r->answered >> f) & 1u)) {field(r, f, 0); continue;}
        const uint16_t raw = r->raw[f];
        switch (f) {
        case PgVoltage:
            r->voltage_mv = raw;
            field(r, f, raw >= 2500 && raw <= 5000);
            break;
        case PgCurrent:
        case PgAvgCurrent: {
            const int32_t ma = signed16(raw);
            if (f == PgCurrent) r->current_ma = ma;
            else r->avg_current_ma = ma;
            field(r, f, raw != 0x8000u && ma >= -16000 && ma <= 16000);
            break;
        }
        case PgTemperature:
            /* -40 .. 90 C; a gauge without its thermistor answers far outside */
            r->temperature_centi = ((int32_t)raw - ZERO_C_DECIKELVIN) * 10;
            field(r, f, raw >= ZERO_C_DECIKELVIN - 400 && raw <= ZERO_C_DECIKELVIN + 900);
            break;
        case PgSoc:
            r->soc = raw;
            field(r, f, raw <= 100);
            break;
        case PgFlags:
            r->flags = raw;
            field(r, f, 1);
            break;
        case PgRemaining:
            r->remaining_mah = raw;
            field(r, f, raw <= 20000);
            break;
        case PgFull:
            r->full_mah = raw;
            field(r, f, raw >= 500 && raw <= 20000);
            break;
        case PgTimeToEmpty:
        case PgTimeToFull: {
            const int32_t minutes = raw == 0xffffu ? -1 : (int32_t)raw;
            if (f == PgTimeToEmpty) r->time_to_empty_min = minutes;
            else r->time_to_full_min = minutes;
            field(r, f, minutes <= 100 * 60);
            break;
        }
        case PgCycles:
            r->cycles = raw;
            field(r, f, raw <= 20000);
            break;
        case PgSoh:
            r->soh = raw;
            field(r, f, raw <= 100);
            break;
        default:
            break;
        }
    }
}

static int read_one(const PgBus *bus, PgReading *r, int f)
{
    int rc = -5;
    for (unsigned attempt = 0; attempt < PGAUGE_TRIES; ++attempt) {
        if (attempt) {
            r->retries++;
            if (bus->sleep_ms) bus->sleep_ms(bus->ctx, PGAUGE_RETRY_MS);
        }
        uint16_t value = 0;
        r->reads++;
        rc = bus->read16(bus->ctx, kRegister[f], &value);
        if (!rc) {
            r->raw[f] = value;
            r->answered |= 1u << f;
            return 0;
        }
    }
    r->answered &= ~(1u << f);
    r->failures++;
    r->last_error = rc;
    return rc;
}

int pagani_gauge_read(const PgBus *bus, PgReading *r, int slow)
{
    if (!bus || !bus->read16 || !r) return -22;
    r->passes++;
    const uint32_t fields = slow ? ALL_FIELDS : ALL_FIELDS & ~SLOW_FIELDS;
    int answered = 0;
    for (int f = 0; f < PgCount; ++f) {
        if (!((fields >> f) & 1u)) continue;
        const int rc = read_one(bus, r, f);
        if (rc && f == PgVoltage) {
            /* The gauge does not answer at all: everything this pass would read is stale now. */
            r->answered &= ~fields;
            pagani_gauge_decode(r, fields);
            return rc;
        }
        if (!rc) answered++;
    }
    pagani_gauge_decode(r, fields);
    return answered;
}

int pagani_gauge_transfer_allowed(const uint8_t *tx, unsigned txn, unsigned rxn)
{
    if (!tx || !txn) return 0;
    if (txn == 1) return rxn >= 1 && rxn <= 4 && tx[0] < 0x80;           /* a register address, then the read */
    return txn == 3 && !rxn && tx[0] == PgRegBlock && tx[1] == 0x01 && tx[2] == 0x00;   /* DEVICE_TYPE */
}

int pagani_gauge_device_type(const PgBus *bus, uint16_t *type)
{
    if (!bus || !bus->xfer || !type) return -22;
    int rc = -71;
    for (unsigned attempt = 0; attempt < PGAUGE_TRIES; ++attempt) {
        static const uint8_t subcommand[3] = {PgRegBlock, 0x01, 0x00};   /* DEVICE_TYPE, little endian */
        rc = bus->xfer(bus->ctx, subcommand, 3, 0, 0);
        if (rc) continue;
        if (bus->sleep_ms) bus->sleep_ms(bus->ctx, 5);
        const uint8_t reg = PgRegBlock;
        uint8_t block[4] = {0};
        rc = bus->xfer(bus->ctx, &reg, 1, block, 4);
        if (rc) continue;
        if ((block[0] | block[1] << 8) != 0x0001) {rc = -71; continue;}
        *type = (uint16_t)(block[2] | block[3] << 8);
        return 0;
    }
    return rc;
}
