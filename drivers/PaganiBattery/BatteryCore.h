/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved.
 * The refresh structure (one battery manager request in flight with a 2 s timer, a refresh on every notification and every 60 s, the
 * slow fields every tenth time, publication only once a charge is known) follows UniXNU-sheng package/ShengBattery.cpp
 * (https://github.com/UniXNU/UniXNU-sheng, commit 08cde20e); the two sources, their merge and the charging policy are the 13T's. */
/* PaganiBattery's portable core: everything but the kernel's threads, mappings and IOKit objects (PaganiBattery.cpp), so the host tests
 * run it against a simulated gauge and DSP.
 *
 * Sources. The NFG8011B fuel gauge on the apps I2C (BatteryGauge.h) gives the battery itself: charge, voltage, current, temperature,
 * capacities and cycles. It is what Android shows on this phone, and it needs no DSP. The DSP's battery manager (BatteryBattmgr.h, with
 * pagani-adsp=1) adds the charger: whether USB is connected, the charger's state and the adapter; it also has battery values, used only
 * when the gauge does not answer. Without the DSP, being on a charger is inferred from the gauge current, one decision per gauge pass
 * and a change only after passes in a row agree (the second one 2 s after the first): charging when the battery takes 50 mA (instant or
 * average), still charging down to 20 mA; held full when a charge ends at 99 % or more with the current between -50 and +20 mA (the
 * charger carries the phone); unplugged when the battery gives 50 mA, or after four passes of a charge that stopped below the top (the
 * charger no longer charges: without the charger's view that is shown as on battery). The status register's bits are not used (their
 * layout is not proven on this part, BatteryGauge.h). Without the DSP a charger that does not charge cannot be seen at all. The average
 * counts only from 60 s after the start on: until then it still holds what came before this boot (its filter forgets in about 14 s; seen
 * at 2 and 4 s: +94 and +72 mA with the battery giving 124 mA, shown as a charger), and only the instant reading shows a charger.
 *
 * The charger PMIC's own state (pagani-battery bits 2 and 3; BatterySpmi.h) answers what the gauge cannot: bit 2 reads it every 2 s through
 * the SPMI observer channels and only logs it, so nothing iOS is shown changes. Bit 3 also uses it, without the DSP's view: external is
 * VBUS (two samples in a row), charging a stage that pushes current whatever the load takes, fully charged VBUS without that at 99 % or
 * more. Three failed samples go back to the inference, which keeps running. The meanings are readings of Android's register dumps,
 * which is why bit 3 is separate: it waits until a log of bit 2 confirms them.
 *
 * Safety of what iOS is shown: nothing is published before a charge is known (0 % would read as an empty battery); a gauge answering
 * 0 % above 3450 mV shows 1 %; the critical level needs the voltage to agree (below 3500 mV when it is known).
 *
 * Charging policy (pagani-battery-charge). 0 (default): nothing is written to the charger but the notification request. 1: while USB is
 * connected the input is unsuspended when the firmware has it at 0, a computer's port gets its input limit (SDP 900 mA, CDP 1500 mA, as a
 * USB stack would ask), the charge current is limited to pagani-battery-fcc-ma (default 2000 mA) and to 500 mA outside 5..45 C (back
 * above 8 C / below 42 C). 2: also BATT_CHG_EN = 1 when the firmware has it at 0, and 0 outside 0..50 C, only with the OPLUS property
 * layout proven by the probes. Every write is logged; at most three tries per value and connection. */
#ifndef PAGANI_BATTERY_CORE_H
#define PAGANI_BATTERY_CORE_H
#include <stdint.h>
#include "BatteryBattmgr.h"
#include "BatteryGauge.h"
#include "BatterySpmi.h"
#ifdef __cplusplus
extern "C" {
#endif

/* What iOS is shown (PaganiBatteryService.cpp makes it IOPMPowerSource properties). */
typedef struct {
    int32_t present, external, charging, fully_charged, critical;
    int32_t percent;                     /* 0..100 */
    int32_t voltage_mv;
    int32_t current_ma, instant_ma;      /* charge positive: the average (Amperage) and the instant reading */
    int32_t temperature_centi, temperature_valid;
    int32_t raw_mah, full_mah, design_mah, cycles;
    int32_t cycles_known;                /* cycles was read (0 is a valid count: powerd needs CycleCount for the health figure) */
    int32_t health_percent;              /* full over design rounded up as powerd's nominal capacity percentage, 1..100; 0 unknown */
    int32_t time_remaining_min;          /* to full while charging, else to empty; 0xffff unknown */
    uint32_t adapter_family;             /* kIOPSFamilyCode, 0 without an adapter */
    int32_t adapter_mv, adapter_ma, adapter_watts;
    uint32_t sources;                    /* PbatSrc* */
} PbatSnapshot;

enum {PbatSrcGauge = 1u, PbatSrcBattmgr = 2u, PbatSrcCharger = 4u, PbatSrcInferred = 8u, PbatSrcPmic = 16u};

/* The charger inferred from the gauge (no charger view). */
enum {PbatInfNone = 0, PbatInfCharging = 1, PbatInfFull = 2};

typedef struct {
    void *ctx;
    uint64_t (*now_ms)(void *ctx);
    int (*send)(void *ctx, const uint8_t *data, uint32_t length);    /* 0 queued; anything else refused */
    void (*publish)(void *ctx, const PbatSnapshot *s, int first);   /* first: register the service now (its properties are set) */
    void (*trace)(void *ctx, const char *line);
    PgBus gauge;                                                     /* read16 0: no gauge */
    PbsRead pmic;                                                    /* null: the charger PMIC is not read */
} PbatOps;

typedef struct {
    uint32_t use_gauge, use_adsp;
    uint32_t charge;            /* 0, 1, 2 (see above) */
    uint32_t fcc_ma;            /* charge >= 1: charge current limit (default 2000, at most 6000) */
    uint32_t sdp_ma;            /* charge >= 1: a computer port's input limit (default 900; 0 leaves the firmware's) */
    uint32_t design_mah;        /* the battery's design capacity when the DSP does not give it (13T: 6260) */
    uint32_t log;               /* 0 start and failures, 1 rationed (default), 2 every pass */
    uint32_t gauge_ms;          /* gauge period (0: 20 s with the DSP, 10 s without) */
    uint32_t refresh_ms;        /* battery manager refresh (0: 60 s) */
    uint32_t pmic;              /* the charger PMIC's state: 0 not read (default), 1 logged only, 2 also used (see above) */
} PbatConfig;

/* The value of boot-arg pagani-battery: bit 0 on, without it nothing runs; the others default off and cost no byte of the command line. */
enum {
    PBAT_MODE_ON = 1u,
    PBAT_MODE_USB_PROBE = 2u,   /* the USB probe once, 120 s after the start (BatteryUsb.h) */
    PBAT_MODE_PMIC_LOG = 4u,    /* the charger PMIC's state, logged only */
    PBAT_MODE_PMIC_USE = 8u,    /* ... and used for what iOS is shown; implies the log */
    PBAT_MODE_ALL = 15u,
};
/* The switches a value of pagani-battery turns on, 0 for off: bit 0 clear, or a bit this driver does not know (a typo stays off). */
uint32_t pagani_battery_mode(uint32_t value);
/* PbatConfig.pmic of those switches. */
uint32_t pagani_battery_mode_pmic(uint32_t mode);

/* Tags of the charger writes. */
enum {PbatSetUnsuspend = 1, PbatSetPortLimit = 2, PbatSetChargeLimit = 3, PbatSetChargeEnable = 4};

typedef struct {
    const PbatOps *ops;
    PbatConfig cfg;
    uint64_t start_ms;
    /* gauge */
    uint8_t gauge_available, gauge_answering;
    PgReading gauge;
    uint64_t gauge_due, gauge_ok_ms;
    uint32_t gauge_passes, gauge_fail_streak, gauge_ok_passes, gauge_lost;
    /* battery manager */
    PbmState bm;
    uint8_t channel_up, cycle_done;
    uint64_t request_deadline, refresh_due, bm_cycle_ms;
    uint32_t refreshes, send_refused, channel_ups, channel_downs, last_set_seen;
    /* charging policy (per connection) */
    uint8_t plugged, thermal, en_paused;
    uint32_t tries[5], applied_limit;
    uint32_t want_limit;
    /* inference without the charger's view (PbatInf*): the state, the change the last passes voted for and how many in a row */
    uint8_t inference, inference_next, inference_votes;
    uint32_t inference_changes;
    /* the charger PMIC's state (cfg.pmic): the last good sample, VBUS after two samples in a row, the log's rationing */
    PbsCharger pmic, pmic_logged;
    uint8_t pmic_seen, pmic_plug, pmic_votes, pmic_logged_plug;
    uint64_t pmic_due, pmic_ok_ms, pmic_log_ms;
    uint32_t pmic_failures, pmic_fail_streak;
    uint32_t pmic_fail_lines, pmic_plug_lines, pmic_change_lines, pmic_trend_lines;
    /* publication */
    PbatSnapshot shown;
    uint8_t published;
    uint32_t publishes, soc_guarded;
    /* log rationing */
    uint32_t lines, state_lines, gauge_lines, dsp_lines, fail_lines;
    uint64_t state_log_ms, gauge_log_ms, dsp_log_ms;
    int32_t logged_percent;
    uint32_t logged_failures;   /* the gauge's failed reads when the last state line was written */
    char line[224];
} PbatCore;

void pagani_battery_core_init(PbatCore *c, const PbatOps *ops, const PbatConfig *cfg);
/* The gauge's bus came up (1) or not (0); the core reads it only after 1. */
void pagani_battery_core_gauge(PbatCore *c, int available);
/* The DSP opened (1) or closed (0) the battery manager channel. */
void pagani_battery_core_channel(PbatCore *c, int up);
/* One message from the channel. */
void pagani_battery_core_message(PbatCore *c, const uint8_t *data, uint32_t length);
/* Does what is due now (requests, gauge read, policy, publication, log); returns how long to sleep at most (ms, 1..60000). */
uint32_t pagani_battery_core_run(PbatCore *c);
/* The snapshot from the current readings (exposed for the tests; run publishes it). */
void pagani_battery_core_merge(PbatCore *c, uint64_t now, PbatSnapshot *s);
/* kIOPSFamilyCode of a POWER_SUPPLY_USB_TYPE. */
uint32_t pagani_battery_family(int32_t usb_type);
/* 16 words for the diagnostics array pagani_battery_status (PaganiBattery.h). */
void pagani_battery_core_export(const PbatCore *c, int32_t out[16]);

#ifdef __cplusplus
}
#endif
#endif
