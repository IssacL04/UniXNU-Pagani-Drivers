/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* The OnePlus 13T fuel gauge: an NFG8011B in the battery pack, on the apps processor's I2C hub serial engine 1 (i2c@984000, 100 kHz),
 * address 0x55 (13T running devicetree: qupv3_hub_i2c1 / bq27541-battery@55, "oplus,batt_nfg8011b", oplus,ic_index 1 of the virtual
 * gauge; the ADSP "gauge" of the same virtual gauge only provides the sub-board NTC). It is the gauge Android shows: OPLUS reads it with
 * the bq27541 standard commands (16-bit little-endian registers, SMBus word reads), the current from register 0x0c. Units (TI bq27541
 * data sheet): temperature 0.1 K, voltage mV, current mA signed with charge positive, capacities mAh, times minutes (0xffff: none),
 * state of charge percent. Nothing here writes the gauge except the read-only device type query of pagani_gauge_device_type, and the
 * owner refuses any other write (pagani_gauge_transfer_allowed).
 *
 * Every value is checked against the range a phone battery can have; a reading outside it is not used (a gauge being reset answers
 * 0 or 0xffff for a while). No I/O of its own: the bus operations come from the owner. */
#ifndef PAGANI_BATTERY_GAUGE_H
#define PAGANI_BATTERY_GAUGE_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define PGAUGE_ADDRESS 0x55u
#define PGAUGE_DEVICE_NFG8011B 0x4231u
/* Register 0x0a, two layouts. bq27541 Flags(): DSG bit 0, FC bit 9. The command set this part answers (current at 0x0c, average current
 * at 0x14, cycles 0x2a, RSOC 0x2c, SOH 0x2e, ManufacturerAccess subcommands through 0x3e) is the bq28z610/bq27z561 one, where 0x0a is
 * BatteryStatus() with the SBS bits: FC bit 5, DSG bit 6 and bit 9 the remaining capacity alarm (set when the battery is LOW). Which one
 * the NFG8011B follows is not proven on the device, so the core takes no decision from these bits (BatteryCore.c); every gauge log line
 * carries the raw word, and one run with a charger settles it. */
#define PGAUGE_FLAG_DSG (1u << 0)     /* bq27541 layout: discharging */
#define PGAUGE_FLAG_FC (1u << 9)      /* bq27541 layout: fully charged (bq28z610 layout: remaining capacity alarm) */
#define PGAUGE_STATUS_FC (1u << 5)    /* bq28z610 layout: fully charged */
#define PGAUGE_STATUS_DSG (1u << 6)   /* bq28z610 layout: discharge or relax mode (0: charge mode) */
#define PGAUGE_TRIES 3u
#define PGAUGE_RETRY_MS 5u

enum {
    PgRegTemperature = 0x06, PgRegVoltage = 0x08, PgRegFlags = 0x0a, PgRegCurrent = 0x0c, PgRegRemaining = 0x10, PgRegFull = 0x12,
    PgRegAvgCurrent = 0x14, PgRegTimeToEmpty = 0x16, PgRegTimeToFull = 0x18, PgRegCycles = 0x2a, PgRegSoc = 0x2c, PgRegSoh = 0x2e,
    PgRegBlock = 0x3e,
};

/* Fields, in reading order; the voltage first (it decides whether the gauge answers at all). */
enum {
    PgVoltage, PgCurrent, PgTemperature, PgSoc, PgFlags, PgRemaining, PgFull, PgAvgCurrent, PgTimeToEmpty, PgTimeToFull, PgCycles,
    PgSoh, PgCount
};

typedef struct {
    void *ctx;
    int (*read16)(void *ctx, uint8_t reg, uint16_t *value);    /* 0, or a negative bus error */
    int (*xfer)(void *ctx, const uint8_t *tx, unsigned txn, uint8_t *rx, unsigned rxn);   /* write tx, then read rxn (0: none) */
    void (*sleep_ms)(void *ctx, unsigned ms);
} PgBus;

typedef struct {
    uint16_t raw[PgCount];
    uint32_t answered;              /* bit per field: delivered by the bus in the last pass that read it */
    uint32_t valid;                 /* bit per field: answered and in range */
    int32_t voltage_mv, current_ma, avg_current_ma;   /* current: charge positive */
    int32_t temperature_centi;      /* hundredths of a degree Celsius */
    int32_t soc, remaining_mah, full_mah, cycles, soh;
    int32_t time_to_empty_min, time_to_full_min;      /* -1: the gauge has no estimate */
    uint32_t flags;
    int last_error;                 /* the last bus error (0 none) */
    uint32_t passes, reads, retries, failures;
} PgReading;

/* One pass over the fields (slow: also cycles and health; otherwise those keep their last values). Returns the number of fields answered,
 * or the bus error when the voltage register stays unanswered after its retries (then nothing else is tried). */
int pagani_gauge_read(const PgBus *bus, PgReading *r, int slow);
/* Converts the raw words of the given fields (bit per field) and sets their valid bits. */
void pagani_gauge_decode(PgReading *r, uint32_t fields);
/* The device type (block command 0x3e, subcommand 0x0001; 0x4231 for the NFG8011B). 0, or a negative error (-71: no echo). */
int pagani_gauge_device_type(const PgBus *bus, uint16_t *type);
/* The gauge sits in the battery pack and may drive the pack's protection switches through other ManufacturerAccess subcommands (the
 * bq28z610 family has FET toggles at 0x001f/0x0020/0x0022): the owner passes every transfer to the gauge through this check first.
 * Allowed: a register address followed by a read of 1..4 bytes, and the one write this driver needs, the DEVICE_TYPE subcommand
 * {0x3e, 0x01, 0x00} without a read (Android's gauge driver sends the same at probe). Anything else returns 0 (refused). */
int pagani_gauge_transfer_allowed(const uint8_t *tx, unsigned txn, unsigned rxn);
static inline int pagani_gauge_has(const PgReading *r, int field) {return (int)((r->valid >> field) & 1u);}

#ifdef __cplusplus
}
#endif
#endif
