/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* A read-only look at who may drive the charger PMIC (PMIH010x, SPMI slave 7): the SPMI PMIC arbiter's channel table (v7: core + 0x2000
 * + 4 n holds each channel's peripheral id in bits 19:8) and its ownership table (cnfg + 4 n, bits 2:0: the execution environment that
 * may write the peripheral; 0 is the apps processor). With pagani-battery-pmic=2 also each peripheral's TYPE and SUBTYPE registers
 * (offsets 4 and 5) through the apps observer channel. Arbiter facts as in Sm8750Platform PeripheralIO.h (spmi_find, spmi_read8) and
 * upstream spmi-pmic-arb.c; nothing is written but the observer channel's read command.
 *
 * Why: whether the apps processor could drive the charger itself (pet its watchdog, set its limits) or only the DSP's charger PD can;
 * the log line answers it on the device.
 *
 * The charger's state (pagani-battery bits 2 and 3, BatteryCore.h), through the same observer channels: nothing here writes the PMIC, and
 * the DSP is not needed. No document of this PMIC is public. Android's OPLUS driver dumps these registers (dmesg sm8450_st_dump: hundreds
 * of samples with VBUS, one without), and the meanings below come from comparing those samples with its own log and with the SMB5 charger
 * register map, which they fit; they are readings of those samples, proven on this phone only once a log confirms them:
 *
 *   0x2910  USBIN real-time status: 0x11 with VBUS, 0x04 without. Bit 4 plug-in, bit 2 under-voltage (SMB5 USBIN_PLUGIN_RT_STS, UV).
 *   0x2906, 0x2908  USBIN input and detection status: 0x02 and 0x0f with VBUS, 0 without. Logged only.
 *   0x2b06  Type-C sink status: the source's Rp, 0x02 3 A, 0x04 1.5 A, 0x08 default; 0 without VBUS.
 *   0x2b0b  Type-C misc status: 0x2b, 0xab with VBUS, 0x43 without. Logged only.
 *   0x2606  charger status, bits 2:0 the stage (below); seen: 3, then 4, 7 with OPLUS's chg_en=0, 7 without VBUS.
 *   0x2952  USBIN current limit, 50 mA steps: 0x1e, 0x0a, 0x28 are the 1500, 500, 2000 mA of OPLUS's log.
 *   0x2706, 0x2709  the input limit the charger settled on, the same steps (0x28 with 0x2952 at 0x28). */
#ifndef PAGANI_BATTERY_SPMI_H
#define PAGANI_BATTERY_SPMI_H
#include <stddef.h>
#include <stdint.h>
#include "BatteryText.h"
#ifdef __cplusplus
extern "C" {
#endif

#define PBS_CORE_BASE 0xc400000ull
#define PBS_CORE_SIZE 0x3000u
#define PBS_CNFG_BASE 0xc42d000ull
#define PBS_CNFG_SIZE 0x4000u
#define PBS_OBS_BASE 0xc440000ull
#define PBS_OBS_SIZE 0x80000u
#define PBS_MAX 96u                 /* peripherals of one slave kept */

typedef struct {
    void *ctx;
    uint32_t (*core)(void *ctx, uint32_t offset);
    uint32_t (*cnfg)(void *ctx, uint32_t offset);
    /* observer read of one register of a channel (0, or negative); null: no TYPE/SUBTYPE */
    int (*read8)(void *ctx, int apid, uint8_t offset, uint8_t *value);
} PbsIo;

typedef struct {
    uint32_t version, channels;
    uint32_t count;                 /* peripherals of the slave found (at most PBS_MAX kept) */
    uint16_t ppid[PBS_MAX];
    uint16_t apid[PBS_MAX];
    uint8_t owner[PBS_MAX];
    uint8_t type[PBS_MAX], subtype[PBS_MAX], typed[PBS_MAX];
    uint32_t per_owner[8];
    uint32_t irq_owned;             /* channels whose interrupts are the apps processor's (map bit 24) */
} PbsSurvey;

/* 0, or -19 when the arbiter is not the v7 one this reads (nothing else is read then). */
int pagani_spmi_survey(const PbsIo *io, unsigned sid, int types, PbsSurvey *out);
/* One log line from item *next on ("[battery] spmi sid7 ..."); *next moves on, 0 when everything was written. */
size_t pagani_spmi_line(const PbsSurvey *s, unsigned sid, unsigned *next, char *line, size_t size);

/* The peripherals the charger's state and the USB probe read (PPID: slave << 8 | page): the charger PMIC's CHGR, DCDC, BATIF, USBIN, TYPEC,
 * MISC, its eUSB2 repeater, then the LDOs of the USB PHY (L2D, L3G, L1F) and of the repeater (L15B, L5B). */
#define PBS_PERIPHERALS 12u
extern const uint16_t pagani_spmi_peripherals[PBS_PERIPHERALS];
/* The channel of each peripheral through the two tables, preferring the apps processor's (EE 0) as PeripheralIO.h spmi_find does:
 * apid[i] for pagani_spmi_peripherals[i], -19 without one. 0, or -19 when the arbiter is not the v7 one (nothing else is read then). */
int pagani_spmi_map(const PbsIo *io, int apid[PBS_PERIPHERALS]);
/* The index of a PPID in the list above, -19 for any other: nothing but the list can be read. */
int pagani_spmi_index(unsigned ppid);

/* One register of one peripheral through its observer channel (0, or negative). */
typedef int (*PbsRead)(void *ctx, unsigned ppid, uint8_t offset, uint8_t *value);

enum {PbsUsbinRt, PbsUsbinStatus, PbsUsbinDetect, PbsTypecSink, PbsTypecMisc, PbsChgrStatus, PbsUsbinLimit, PbsAiclMax, PbsAiclNow,
      PbsChargerRegs};
typedef struct {
    uint8_t reg[PbsChargerRegs];
} PbsCharger;

/* The charger's stage, 0x2606 bits 2:0 (SMB5 BATTERY_CHARGER_STATUS). */
enum {PbsStageInhibit = 0, PbsStageTrickle = 1, PbsStagePre = 2, PbsStageFull = 3, PbsStageTaper = 4, PbsStageTerminate = 5, PbsStagePause = 6,
      PbsStageDisabled = 7};

/* Reads all of them; 0, or the first read's error (the sample is then not to be used). */
int pagani_spmi_charger(PbsRead read, void *ctx, PbsCharger *out);
/* VBUS is there: the plug-in bit without the under-voltage bit. */
static inline int pagani_spmi_plugged(const PbsCharger *c) {return (c->reg[PbsUsbinRt] & 0x14u) == 0x10u;}
static inline unsigned pagani_spmi_stage(const PbsCharger *c) {return c->reg[PbsChgrStatus] & 7u;}
/* The charger is pushing current into the battery (the load may still take more than it gives). */
static inline int pagani_spmi_stage_charging(unsigned stage) {return stage >= PbsStageTrickle && stage <= PbsStageTaper;}
/* The fields of a log line: " stage=... rt=... ... icl=... aicl=...". */
void pagani_spmi_charger_text(PbatText *t, const PbsCharger *c);

#ifdef __cplusplus
}
#endif
#endif
