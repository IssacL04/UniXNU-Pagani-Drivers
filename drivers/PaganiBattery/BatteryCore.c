/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved.
 * Refresh structure after UniXNU-sheng package/ShengBattery.cpp (commit 08cde20e); see BatteryCore.h. The adapter family mapping is
 * sheng's (ShengBattmgr.c). */
#include "BatteryCore.h"
#include "BatteryText.h"

enum {
    kRequestMs = 2000,            /* an unanswered battery manager request is skipped after this */
    kRefreshMs = 60000,
    kSlowEvery = 10,              /* every tenth refresh and gauge pass reads the fields that hardly change */
    kGaugeWithDspMs = 20000,
    kGaugeAloneMs = 10000,
    kGaugeSoonMs = 1500,          /* after a notification or a change of the charging state: the current has settled */
    kGaugeBackoffMs = 300000,     /* the longest pause between passes of a gauge that does not answer */
    kChargingMa = 50,             /* a battery taking more is charging */
    kAverageSettleMs = 60000,     /* the gauge's average counts from this long after the start on (BatteryCore.h) */
    kChargeHoldMa = 20,           /* ... and stays charging down to this (inferred charger) */
    kDrainMa = 50,                /* a battery giving more runs the phone: no charger carries it (inferred charger) */
    kTopPercent = 99,             /* a charge ending at this or above is held full by the charger (inferred charger) */
    kFullLeavePercent = 95,       /* held full, a charge drifting below this is not held by anything (inferred charger) */
    kInferVotes = 2,              /* passes in a row for a change of the inferred charger */
    kInferStopVotes = 4,          /* ... for a charge that stopped below the top without the battery giving current */
    kConfirmMs = 2000,            /* the pass that confirms a change comes this soon */
    kEmptyGuardMv = 3450,         /* a 0 % reading above this shows 1 % */
    kCriticalMv = 3500,
    kLimitTries = 3,
    kReducedUa = 500000,
    kHotReduce = 4500, kHotReduceBack = 4200, kColdReduce = 500, kColdReduceBack = 800,
    kHotPause = 5000, kHotPauseBack = 4700, kColdPause = 0, kColdPauseBack = 300,
    kStateLines = 24,             /* charging state changes logged per boot (log 1) */
    kFailLines = 8,               /* failing gauge runs whose first pass is logged per boot (log 1) */
    kLostLines = 4,               /* gauge losses and recoveries logged per boot (log 0, 1); later ones are counted (lost=) */
    kQuietMs = 2 * 3600 * 1000,   /* otherwise a summary every 2 h at most (log 1: the ramoops ring is shared by every driver) */
    kPmicMs = 2000,               /* the charger PMIC's state is read this often */
    kPmicVotes = 2,               /* samples in a row for a change of VBUS */
    kPmicStaleMs = 3 * kPmicMs + 1000,   /* a state this old is not used */
    kPmicFailLines = 4,           /* failing runs of samples logged per boot */
    kPmicPlugLines = 24,          /* changes of VBUS logged per boot */
    kPmicChangeLines = 24,        /* other changes (stage, input limit, Rp) logged per boot: a charger hunting at the top uses these up, not those */
    kPmicTrendMs = 120000,        /* with VBUS a line this often: a charge over hours is what the log has to show */
    kPmicTrendLines = 90,         /* 3 h of them per boot */
};

static uint64_t now_ms(const PbatCore *c) {return c->ops->now_ms(c->ops->ctx);}

static void trace(PbatCore *c, const char *line)
{
    c->lines++;
    if (c->ops->trace) c->ops->trace(c->ops->ctx, line);
}

void pagani_battery_core_init(PbatCore *c, const PbatOps *ops, const PbatConfig *cfg)
{
    pbat_zero(c, sizeof *c);
    c->ops = ops;
    c->cfg = *cfg;
    if (!c->cfg.design_mah) c->cfg.design_mah = 6260;
    if (c->cfg.charge > 2) c->cfg.charge = 2;
    if (c->cfg.charge && !c->cfg.fcc_ma) c->cfg.fcc_ma = 2000;
    if (c->cfg.fcc_ma > 6000) c->cfg.fcc_ma = 6000;
    if (c->cfg.fcc_ma && c->cfg.fcc_ma < 500) c->cfg.fcc_ma = 500;
    if (c->cfg.sdp_ma > 3000) c->cfg.sdp_ma = 3000;
    if (c->cfg.pmic > 2) c->cfg.pmic = 2;
    pagani_bm_init(&c->bm);
    c->start_ms = now_ms(c);
    c->gauge_due = c->start_ms;
    c->pmic_due = c->start_ms;
    c->logged_percent = -1;
}

uint32_t pagani_battery_mode(uint32_t value)
{
    return (value & PBAT_MODE_ON) && value <= PBAT_MODE_ALL ? value : 0;
}

uint32_t pagani_battery_mode_pmic(uint32_t mode)
{
    return mode & PBAT_MODE_PMIC_USE ? 2 : mode & PBAT_MODE_PMIC_LOG ? 1 : 0;
}

void pagani_battery_core_gauge(PbatCore *c, int available)
{
    c->gauge_available = (uint8_t)(available && c->cfg.use_gauge && c->ops->gauge.read16);
    c->gauge_due = now_ms(c);
}

void pagani_battery_core_channel(PbatCore *c, int up)
{
    const uint64_t now = now_ms(c);
    if (up && !c->channel_up) {
        c->channel_up = 1;
        c->channel_ups++;
        trace(c, "[battery] dsp channel up");
        uint8_t enable[PBM_NOTIFY_REQUEST_BYTES];
        const uint32_t n = pagani_bm_enable_request(enable);
        if (c->ops->send(c->ops->ctx, enable, n)) c->send_refused++;
        pagani_bm_refresh(&c->bm, 1);
        c->refresh_due = now + (c->cfg.refresh_ms ? c->cfg.refresh_ms : kRefreshMs);
    } else if (!up && c->channel_up) {
        c->channel_up = 0;
        c->channel_downs++;
        trace(c, "[battery] dsp channel down: the charger's view is gone, the gauge stays");
        /* The values become stale; the layout found stays known (the same firmware comes back, if ever). */
        const uint8_t layout = c->bm.layout, assumed = c->bm.layout_assumed;
        pagani_bm_init(&c->bm);
        c->bm.layout = layout;
        c->bm.layout_assumed = assumed;
        c->plugged = 0;
        c->applied_limit = 0;
    }
}

static void log_set(PbatCore *c)
{
    const PbmState *b = &c->bm;
    static const char *const names[] = {"?", "unsuspend", "port-limit", "charge-limit", "charge-enable"};
    PbatText t;
    pbat_text_start(&t, c->line, sizeof c->line);
    pbat_text_str(&t, "[battery] dsp set ");
    pbat_text_str(&t, b->last_set.tag < 5 ? names[b->last_set.tag] : "?");
    pbat_text_kx(&t, "op", b->last_set.opcode);
    pbat_text_kd(&t, "prop", b->last_set.property);
    pbat_text_kx(&t, "value", b->last_set.value);
    pbat_text_kd(&t, "rc", b->last_set_result);
    pbat_text_kd(&t, "try", c->tries[b->last_set.tag < 5 ? b->last_set.tag : 0]);
    trace(c, c->line);
}

void pagani_battery_core_message(PbatCore *c, const uint8_t *data, uint32_t length)
{
    if (!c->channel_up) return;
    const uint32_t notifications = c->bm.notifications;
    const int r = pagani_bm_receive(&c->bm, data, length);
    if (r == PbmAnswer || r == PbmCycleDone) c->request_deadline = 0;
    if (r == PbmCycleDone) {
        c->cycle_done = 1;
        c->bm_cycle_ms = now_ms(c);
    }
    if (r == PbmSetDone) {
        c->request_deadline = 0;
        if (c->bm.last_set.tag == PbatSetChargeLimit && !c->bm.last_set_result) c->applied_limit = c->bm.last_set.value;
        c->last_set_seen = c->bm.last_set_done;
        log_set(c);
    }
    if (c->bm.notifications != notifications) {
        /* a plug, a status change: read the gauge too once the current settled */
        const uint64_t soon = now_ms(c) + kGaugeSoonMs;
        if (c->gauge_due > soon) c->gauge_due = soon;
    }
}

uint32_t pagani_battery_family(int32_t type)
{
    /* POWER_SUPPLY_USB_TYPE (unknown 0, SDP 1, DCP 2, CDP 3, ACA 4, C 5, PD 6, PD_DRP 7, PD_PPS 8) to kIOPSFamilyCode (the USB family's
     * error space 0xe0004000 + n: host 0, dedicated charging port 4, downstream charging port 5, unknown 7, Type-C 9, PD 10). */
    switch (type) {
    case 1: return 0xe0004000u;
    case 2: return 0xe0004004u;
    case 3: return 0xe0004005u;
    case 5: return 0xe0004009u;
    case 6: case 7: case 8: return 0xe000400au;
    default: return 0xe0004007u;
    }
}

static int gauge_fresh(const PbatCore *c, uint64_t now)
{
    const uint32_t period = c->cfg.gauge_ms ? c->cfg.gauge_ms : kGaugeAloneMs;
    return c->gauge_available && c->gauge_answering && pagani_gauge_has(&c->gauge, PgSoc) && pagani_gauge_has(&c->gauge, PgVoltage) &&
           now - c->gauge_ok_ms <= 3ull * (period > kGaugeWithDspMs ? period : kGaugeWithDspMs) + 5000;
}

static int bm_fresh(const PbatCore *c, uint64_t now)
{
    const uint32_t period = c->cfg.refresh_ms ? c->cfg.refresh_ms : kRefreshMs;
    return c->channel_up && c->bm.cycles_done && now - c->bm_cycle_ms <= 3ull * period;
}

static int pmic_fresh(const PbatCore *c, uint64_t now) {return c->pmic_seen && now - c->pmic_ok_ms <= kPmicStaleMs;}

static int32_t clamp(int32_t v, int32_t lo, int32_t hi) {return v < lo ? lo : v > hi ? hi : v;}

void pagani_battery_core_merge(PbatCore *c, uint64_t now, PbatSnapshot *s)
{
    pbat_zero(s, sizeof *s);
    const PgReading *g = &c->gauge;
    const PbmState *b = &c->bm;
#define GH(f) pagani_gauge_has(g, (f))
#define BH(f) pagani_bm_has(b, (f))
#define BV(f) (b->value[(f)])
    const int gf = gauge_fresh(c, now), bf = bm_fresh(c, now);
    const int layout = bf && b->layout != PbmLayoutUnknown;
    int known = 0;
    if (gf) {
        s->percent = g->soc;
        known = 1;
        s->sources |= PbatSrcGauge;
    } else if (bf && BH(PbmCapacity) && BV(PbmCapacity) >= 0 && BV(PbmCapacity) <= 10000) {
        s->percent = (BV(PbmCapacity) + 50) / 100;
        known = 1;
        s->sources |= PbatSrcBattmgr;
    }
    s->voltage_mv = gf && GH(PgVoltage) ? g->voltage_mv : bf && BH(PbmVoltage) ? BV(PbmVoltage) / 1000 : 0;
    if (gf && GH(PgCurrent)) {
        s->instant_ma = g->current_ma;
        s->current_ma = GH(PgAvgCurrent) ? g->avg_current_ma : g->current_ma;
    } else if (bf && BH(PbmCurrent)) {
        s->instant_ma = s->current_ma = -(BV(PbmCurrent) / 1000);   /* the firmware's current is discharge positive */
    }
    if (gf && GH(PgTemperature)) {
        s->temperature_centi = g->temperature_centi;
        s->temperature_valid = 1;
    } else if (layout && BH(PbmTemperature) && BV(PbmTemperature) >= -4000 && BV(PbmTemperature) <= 9000) {
        s->temperature_centi = BV(PbmTemperature);
        s->temperature_valid = 1;
    }
    s->raw_mah = gf && GH(PgRemaining) ? g->remaining_mah : layout && BH(PbmChargeCounter) && BV(PbmChargeCounter) > 0 ?
                 BV(PbmChargeCounter) / 1000 : 0;
    s->full_mah = gf && GH(PgFull) ? g->full_mah : layout && BH(PbmFull) && BV(PbmFull) > 0 ? BV(PbmFull) / 1000 : 0;
    s->design_mah = layout && BH(PbmDesign) && BV(PbmDesign) >= 1000000 && BV(PbmDesign) <= 20000000 ? BV(PbmDesign) / 1000 :
                    (int32_t)c->cfg.design_mah;
    if (gf && GH(PgCycles)) {
        s->cycles = g->cycles;
        s->cycles_known = 1;
    } else if (layout && BH(PbmCycles) && BV(PbmCycles) >= 0 && BV(PbmCycles) < 20000) {
        s->cycles = BV(PbmCycles);
        s->cycles_known = 1;
    }
    /* powerd's nominal charge capacity percentage, rounded up (21H461 powerd: ceil(100 * NominalChargeCapacity / DesignCapacity)) */
    if (s->full_mah > 0 && s->design_mah > 0)
        s->health_percent = clamp((int32_t)(((int64_t)s->full_mah * 100 + s->design_mah - 1) / s->design_mah), 1, 100);
    s->present = gf ? 1 : bf && BH(PbmPresent) ? BV(PbmPresent) != 0 : known;

    /* Charger. */
    const int gauge_charging = gf && ((GH(PgCurrent) && g->current_ma >= kChargingMa) || (GH(PgAvgCurrent) && g->avg_current_ma >= kChargingMa));
    if (bf && BH(PbmUsbOnline)) {
        s->sources |= PbatSrcCharger;
        const int32_t status = BH(PbmStatus) ? BV(PbmStatus) : 0;   /* POWER_SUPPLY_STATUS: 1 charging, 2 discharging, 3 not, 4 full */
        s->external = BV(PbmUsbOnline) != 0;
        const int signal = status == 1 || gauge_charging || (!gf && BH(PbmCurrent) && BV(PbmCurrent) <= -kChargingMa * 1000);
        s->charging = s->external && signal && status != 4;
        s->fully_charged = s->external && (status == 4 || (known && s->percent >= 100 && !s->charging));
    } else if (gf && c->cfg.pmic >= 2 && pmic_fresh(c, now)) {
        s->sources |= PbatSrcPmic;
        s->external = c->pmic_plug;
        s->charging = s->external && pagani_spmi_stage_charging(pagani_spmi_stage(&c->pmic));
        s->fully_charged = s->external && !s->charging && known && s->percent >= kTopPercent;
    } else if (gf) {
        /* the gauge's passes decided (infer below); a gauge that is not fresh claims no charger */
        s->sources |= PbatSrcInferred;
        s->external = c->inference != PbatInfNone;
        s->charging = c->inference == PbatInfCharging;
        s->fully_charged = c->inference == PbatInfFull && known && s->percent >= kTopPercent;
    }

    /* What iOS must never be shown by mistake. */
    if (known) {
        s->percent = clamp(s->percent, 0, 100);
        if (s->percent == 0 && s->voltage_mv >= kEmptyGuardMv) {
            s->percent = 1;
            c->soc_guarded++;
        }
    }
    s->critical = s->present && !s->external && known && s->percent <= 2 && (!s->voltage_mv || s->voltage_mv < kCriticalMv);

    /* Time remaining (minutes). */
    int32_t minutes = -1;
    if (s->charging) {
        if (gf && GH(PgTimeToFull) && g->time_to_full_min >= 0) minutes = g->time_to_full_min;
        else if (layout && BH(PbmTimeToFull) && BV(PbmTimeToFull) > 0 && BV(PbmTimeToFull) < 100 * 3600) minutes = BV(PbmTimeToFull) / 60;
    } else if (!s->external) {
        if (gf && GH(PgTimeToEmpty) && g->time_to_empty_min >= 0) minutes = g->time_to_empty_min;
        else if (layout && BH(PbmTimeToEmpty) && BV(PbmTimeToEmpty) > 0 && BV(PbmTimeToEmpty) < 100 * 3600) minutes = BV(PbmTimeToEmpty) / 60;
    }
    s->time_remaining_min = minutes >= 0 && minutes < 0xffff ? minutes : 0xffff;

    /* Adapter. */
    if (s->external) {
        const int32_t type = bf && BH(PbmUsbAdapter) ? BV(PbmUsbAdapter) : bf && BH(PbmUsbType) ? BV(PbmUsbType) : 0;
        s->adapter_family = pagani_battery_family(type);
        if (bf) {
            s->adapter_mv = BH(PbmUsbVoltage) && BV(PbmUsbVoltage) > 0 ? BV(PbmUsbVoltage) / 1000 : 0;
            s->adapter_ma = BH(PbmUsbCurrent) ? BV(PbmUsbCurrent) / 1000 : 0;
            if (s->adapter_ma < 0) s->adapter_ma = -s->adapter_ma;
            s->adapter_watts = (int32_t)(((int64_t)s->adapter_mv * s->adapter_ma + 500000) / 1000000);
        }
    }
    if (!known) s->percent = -1;
#undef GH
#undef BH
#undef BV
}

/* ---- the charging policy (see BatteryCore.h) ---- */
static void thermal_state(PbatCore *c, const PbatSnapshot *s)
{
    if (!s->temperature_valid) return;
    const int32_t t = s->temperature_centi;
    const int pause = c->cfg.charge >= 2;
    switch (c->thermal) {
    case 0:
        if (pause && (t >= kHotPause || t < kColdPause)) c->thermal = 2;
        else if (t >= kHotReduce || t < kColdReduce) c->thermal = 1;
        break;
    case 1:
        if (pause && (t >= kHotPause || t < kColdPause)) c->thermal = 2;
        else if (t <= kHotReduceBack && t >= kColdReduceBack) c->thermal = 0;
        break;
    default:
        if (t <= kHotPauseBack && t >= kColdPauseBack) c->thermal = 1;
        break;
    }
}

static int try_set(PbatCore *c, int tag, uint32_t opcode, uint32_t property, uint32_t value)
{
    if (c->tries[tag] >= kLimitTries) return 0;
    if (pagani_bm_set(&c->bm, opcode, property, value, (uint32_t)tag)) return 0;
    c->tries[tag]++;
    return 1;
}

static void policy(PbatCore *c, const PbatSnapshot *s)
{
    PbmState *b = &c->bm;
    if (!c->cfg.charge || !c->channel_up || !pagani_bm_has(b, PbmUsbOnline)) return;
    const int online = b->value[PbmUsbOnline] != 0;
    if (online != c->plugged) {
        c->plugged = (uint8_t)online;
        pbat_zero(c->tries, sizeof c->tries);
        c->applied_limit = 0;
    }
    thermal_state(c, s);
    if (!online || b->queued || b->set_in_flight) return;
    /* 1 the input: suspended (0) -> lifted */
    if (pagani_bm_has(b, PbmUsbInputLimit) && b->value[PbmUsbInputLimit] == 0) {
        try_set(c, PbatSetUnsuspend, PBM_OP_USB_SET, PbmPropUsbInputLimit, 0xffffffffu);
        return;
    }
    /* 2 a computer's port: the limit its USB stack would ask for */
    const int32_t port = pagani_bm_has(b, PbmUsbAdapter) ? b->value[PbmUsbAdapter] : pagani_bm_has(b, PbmUsbType) ? b->value[PbmUsbType] : 0;
    const uint32_t port_ua = port == 1 ? c->cfg.sdp_ma * 1000u : port == 3 ? 1500000u : 0u;
    if (port_ua && pagani_bm_has(b, PbmUsbInputLimit) && (uint64_t)(uint32_t)b->value[PbmUsbInputLimit] + 100000u < port_ua) {
        if (try_set(c, PbatSetPortLimit, PBM_OP_USB_SET, PbmPropUsbInputLimit, port_ua)) return;
    }
    /* 3 the charge current: the configured limit, 500 mA outside 5..45 C */
    const uint32_t want = c->thermal ? (uint32_t)kReducedUa : c->cfg.fcc_ma * 1000u;
    if (want != c->want_limit) {
        c->want_limit = want;
        c->tries[PbatSetChargeLimit] = 0;
    }
    if (c->applied_limit != want && try_set(c, PbatSetChargeLimit, PBM_OP_BATTERY_SET, PbmPropChargeControlLimit, want)) return;
    /* 4 charging itself (2 only, OPLUS layout proven): on, off outside 0..50 C */
    if (c->cfg.charge >= 2 && b->layout == PbmLayoutOplus && !b->layout_assumed && pagani_bm_has(b, PbmChargeEnable)) {
        const uint32_t en = c->thermal == 2 ? 0u : 1u;
        if ((uint32_t)b->value[PbmChargeEnable] != en) {
            if (c->en_paused != (uint8_t)(en == 0)) {c->en_paused = (uint8_t)(en == 0); c->tries[PbatSetChargeEnable] = 0;}
            try_set(c, PbatSetChargeEnable, PBM_OP_BATTERY_SET, PbmPropOplusChargeEnable, en);
        }
    }
}

/* ---- log lines ---- */
static void line_gauge(PbatCore *c)
{
    const PgReading *g = &c->gauge;
    PbatText t;
    pbat_text_start(&t, c->line, sizeof c->line);
    pbat_text_str(&t, "[battery] gauge");
    pbat_text_kd(&t, "pass", g->passes);
    pbat_text_kd(&t, "mv", g->voltage_mv);
    pbat_text_kd(&t, "ma", g->current_ma);
    pbat_text_kd(&t, "avg", g->avg_current_ma);
    pbat_text_centi(&t, "t", g->temperature_centi);
    pbat_text_kd(&t, "soc", g->soc);
    pbat_text_kd(&t, "rm", g->remaining_mah);
    pbat_text_kd(&t, "fcc", g->full_mah);
    pbat_text_kd(&t, "cyc", g->cycles);
    pbat_text_kd(&t, "soh", g->soh);
    pbat_text_kx(&t, "flags", g->flags);
    pbat_text_kd(&t, "tte", g->time_to_empty_min);
    pbat_text_kd(&t, "ttf", g->time_to_full_min);
    pbat_text_kx(&t, "valid", g->valid);
    pbat_text_kd(&t, "retry", g->retries);
    pbat_text_kd(&t, "fail", g->failures);
    if (g->last_error) pbat_text_kd(&t, "err", g->last_error);
    trace(c, c->line);
}

static void line_dsp(PbatCore *c)
{
    const PbmState *b = &c->bm;
    static const char *const layouts[] = {"?", "oplus", "qti"};
    PbatText t;
    pbat_text_start(&t, c->line, sizeof c->line);
    pbat_text_str(&t, "[battery] dsp");
    pbat_text_kd(&t, "cyc", b->cycles_done);
    pbat_text_str(&t, " layout=");
    pbat_text_str(&t, layouts[b->layout < 3 ? b->layout : 0]);
    if (b->layout_assumed) pbat_text_char(&t, '?');
#define F(key, f) do {if (pagani_bm_has(b, (f))) pbat_text_kd(&t, key, b->value[(f)]); else {pbat_text_char(&t, ' '); pbat_text_str(&t, key); pbat_text_str(&t, "=-");}} while (0)
    F("st", PbmStatus);
    F("cap", PbmCapacity);
    F("uv", PbmVoltage);
    F("ua", PbmCurrent);
    F("t", PbmTemperature);
    F("lim", PbmChargeLimit);
    F("en", PbmChargeEnable);
    F("usb", PbmUsbOnline);
    F("type", PbmUsbType);
    F("adap", PbmUsbAdapter);
    F("real", PbmUsbRealType);
    F("icl", PbmUsbInputLimit);
    F("vbus", PbmUsbVoltage);
    F("ibus", PbmUsbCurrent);
#undef F
    pbat_text_kd(&t, "ans", b->responses);
    pbat_text_kd(&t, "err", b->errors);
    pbat_text_kd(&t, "to", b->timeouts);
    pbat_text_kd(&t, "note", b->notifications);
    pbat_text_kd(&t, "ign", b->ignored);
    trace(c, c->line);
}

static void line_state(PbatCore *c, const PbatSnapshot *s, const char *why)
{
    PbatText t;
    pbat_text_start(&t, c->line, sizeof c->line);
    pbat_text_str(&t, "[battery] state ");
    pbat_text_str(&t, why);
    pbat_text_kd(&t, "pct", s->percent);
    pbat_text_kd(&t, "ext", s->external);
    pbat_text_kd(&t, "chg", s->charging);
    pbat_text_kd(&t, "full", s->fully_charged);
    pbat_text_kd(&t, "crit", s->critical);
    pbat_text_kd(&t, "mv", s->voltage_mv);
    pbat_text_kd(&t, "ma", s->current_ma);
    if (s->temperature_valid) pbat_text_centi(&t, "t", s->temperature_centi);
    pbat_text_kd(&t, "rm", s->raw_mah);
    pbat_text_kd(&t, "fcc", s->full_mah);
    if (s->cycles_known) pbat_text_kd(&t, "cyc", s->cycles);
    else pbat_text_str(&t, " cyc=-");
    if (s->health_percent) pbat_text_kd(&t, "mcp", s->health_percent);
    if (s->adapter_family) pbat_text_kx(&t, "adapter", s->adapter_family);
    if (s->adapter_watts) pbat_text_kd(&t, "w", s->adapter_watts);
    pbat_text_str(&t, " src=");
    pbat_text_str(&t, s->sources & PbatSrcGauge ? "gauge" : s->sources & PbatSrcBattmgr ? "dsp" : "none");
    pbat_text_str(&t, s->sources & PbatSrcCharger ? "+charger" : s->sources & PbatSrcPmic ? "+pmic" : s->sources & PbatSrcInferred ? "+inferred" : "");
    if (c->thermal) pbat_text_kd(&t, "thermal", c->thermal);
    /* the gauge's failed reads and losses: past the first few lines of their own, a gauge that comes and goes shows only here */
    if (c->gauge.failures) pbat_text_kd(&t, "fail", c->gauge.failures);
    if (c->gauge_lost) pbat_text_kd(&t, "lost", c->gauge_lost);
    trace(c, c->line);
}

static void line_lost(PbatCore *c)
{
    PbatText t;
    pbat_text_start(&t, c->line, sizeof c->line);
    pbat_text_str(&t, "[battery] gauge stopped answering (3 passes): the DSP's values or the last ones stay");
    pbat_text_kd(&t, "lost", c->gauge_lost);
    if (c->cfg.log < 2 && c->gauge_lost == kLostLines) pbat_text_str(&t, " (the last such line this boot)");
    trace(c, c->line);
}

static void line_pmic(PbatCore *c, const char *why)
{
    const PgReading *g = &c->gauge;
    PbatText t;
    pbat_text_start(&t, c->line, sizeof c->line);
    pbat_text_str(&t, "[battery] pmic ");
    pbat_text_str(&t, why);
    pbat_text_kd(&t, "vbus", c->pmic_plug);
    pagani_spmi_charger_text(&t, &c->pmic);
    if (c->gauge_answering) {
        pbat_text_kd(&t, "pct", g->soc);
        pbat_text_kd(&t, "rm", g->remaining_mah);
        pbat_text_kd(&t, "mv", g->voltage_mv);
        pbat_text_kd(&t, "ma", g->current_ma);
        pbat_text_kd(&t, "avg", g->avg_current_ma);
        pbat_text_centi(&t, "t", g->temperature_centi);
    }
    if (c->pmic_failures) pbat_text_kd(&t, "fail", c->pmic_failures);
    trace(c, c->line);
}

static int quiet_over(uint64_t now, uint64_t last) {return !last || now - last >= kQuietMs;}

/* ---- the charger inferred from the gauge (no charger view; BatteryCore.h) ---- */
static int charger_view(const PbatCore *c, uint64_t now) {return bm_fresh(c, now) && pagani_bm_has(&c->bm, PbmUsbOnline);}

/* One decision per answering gauge pass: what the readings show, then the state moves only when enough passes in a row agree. */
static void infer(PbatCore *c, uint64_t now)
{
    const PgReading *g = &c->gauge;
    const int i_ok = pagani_gauge_has(g, PgCurrent), a_ok = pagani_gauge_has(g, PgAvgCurrent);
    if (!i_ok && !a_ok) return;   /* no current: no evidence either way */
    const int32_t i = g->current_ma, a = g->avg_current_ma;
    const int32_t percent = pagani_gauge_has(g, PgSoc) ? g->soc : -1;
    const int settled = now - c->start_ms >= kAverageSettleMs;
    const int taking = (i_ok && i >= kChargingMa) || (settled && a_ok && a >= kChargingMa);
    const int still = (i_ok && i >= kChargeHoldMa) || (a_ok && a >= kChargeHoldMa);
    const int giving = i_ok ? i <= -kDrainMa : a <= -kDrainMa;   /* the instant reading first: the average lags a plug-out */
    uint8_t want = c->inference;
    unsigned need = kInferVotes;
    switch (c->inference) {
    case PbatInfCharging:
        if (still) want = PbatInfCharging;
        else if (!giving && percent >= kTopPercent) want = PbatInfFull;
        else {
            want = PbatInfNone;
            if (!giving) need = kInferStopVotes;   /* stopped below the top: the charger's own pause or no charge at all */
        }
        break;
    case PbatInfFull:
        if (taking) want = PbatInfCharging;      /* topping up */
        else if (giving || (percent >= 0 && percent < kFullLeavePercent)) want = PbatInfNone;
        else want = PbatInfFull;
        break;
    default:
        want = taking ? PbatInfCharging : PbatInfNone;
        break;
    }
    if (want == c->inference) {
        c->inference_votes = 0;
        return;
    }
    if (want != c->inference_next || !c->inference_votes) {
        c->inference_next = want;
        c->inference_votes = 1;
    } else {
        c->inference_votes++;
    }
    if (c->inference_votes >= need) {
        c->inference = want;
        c->inference_votes = 0;
        c->inference_changes++;
    }
}

/* ---- the run ---- */
static void gauge_pass(PbatCore *c, uint64_t now)
{
    const int slow = c->gauge_passes % kSlowEvery == 0;
    c->gauge_passes++;
    const int rc = pagani_gauge_read(&c->ops->gauge, &c->gauge, slow);
    const int was = c->gauge_answering, ok = rc > 0 && pagani_gauge_has(&c->gauge, PgVoltage);
    if (ok) {
        c->gauge_answering = 1;
        c->gauge_ok_ms = now;
        c->gauge_fail_streak = 0;
        c->gauge_ok_passes++;
        if (charger_view(c, now)) {
            /* the charger says it: a later loss of its view starts the inference afresh */
            c->inference = PbatInfNone;
            c->inference_votes = 0;
        } else {
            infer(c, now);
        }
    } else if (++c->gauge_fail_streak >= 3) {
        c->gauge_answering = 0;
        c->inference_votes = 0;
    }
    /* log 2: every pass; 1: the first two readings and the first failure of the first kFailLines failing runs; 0: losing it. Losses
     * and recoveries are logged kLostLines times a boot (log 0, 1): a gauge that keeps coming and going is otherwise only counted, the
     * state lines carry fail= and lost= (the ramoops event ring is 32 KiB for every driver). */
    int log = c->cfg.log >= 2;
    if (c->cfg.log == 1) log = ok ? c->gauge_ok_passes <= 2 : c->gauge_fail_streak == 1 && c->fail_lines < kFailLines;
    if (log) {
        if (!ok) c->fail_lines++;
        c->gauge_log_ms = now;
        c->gauge_lines++;
        line_gauge(c);
    }
    if (was && !c->gauge_answering) {
        c->gauge_lost++;
        if (c->cfg.log >= 2 || c->gauge_lost <= kLostLines) {
            if (!c->cfg.log) line_gauge(c);
            line_lost(c);
        }
    } else if (!was && c->gauge_answering && c->gauge_ok_passes > 1 && (c->cfg.log >= 2 || c->gauge_lost <= kLostLines)) {
        trace(c, "[battery] gauge answers again");
    }
}

/* One sample of the charger's state: the state only moves on a good one, and three bad ones in a row make it stale (pmic_fresh). The log
 * has the first sample, every change of VBUS, of the stage, the input limit and the Rp, and with VBUS a trend line, each with the gauge's
 * reading: a charge that stops half way shows in them without any other switch. */
static void pmic_pass(PbatCore *c, uint64_t now)
{
    PbsCharger s;
    const int rc = pagani_spmi_charger(c->ops->pmic, c->ops->ctx, &s);
    if (rc) {
        c->pmic_failures++;
        if (++c->pmic_fail_streak == 1 && c->pmic_fail_lines < kPmicFailLines && c->cfg.log) {
            c->pmic_fail_lines++;
            PbatText t;
            pbat_text_start(&t, c->line, sizeof c->line);
            pbat_text_str(&t, "[battery] pmic read failed");
            pbat_text_kd(&t, "rc", rc);
            pbat_text_kd(&t, "n", c->pmic_failures);
            trace(c, c->line);
        }
        return;
    }
    const int first = !c->pmic_seen;
    const uint8_t plug = (uint8_t)pagani_spmi_plugged(&s);
    c->pmic = s;
    c->pmic_ok_ms = now;
    c->pmic_fail_streak = 0;
    c->pmic_seen = 1;
    if (first || plug == c->pmic_plug) {
        c->pmic_plug = plug;
        c->pmic_votes = 0;
    } else if (++c->pmic_votes >= kPmicVotes) {
        c->pmic_plug = plug;
        c->pmic_votes = 0;
    }
    if (!c->cfg.log) return;
    const char *why = 0;
    if (first) {
        why = "first";
    } else if (c->pmic_plug != c->pmic_logged_plug) {
        if (c->pmic_plug_lines < kPmicPlugLines) {
            c->pmic_plug_lines++;
            why = "change";
        }
    } else if (pagani_spmi_stage(&s) != pagani_spmi_stage(&c->pmic_logged) || s.reg[PbsUsbinLimit] != c->pmic_logged.reg[PbsUsbinLimit] ||
               s.reg[PbsTypecSink] != c->pmic_logged.reg[PbsTypecSink]) {
        if (c->pmic_change_lines < kPmicChangeLines) {
            c->pmic_change_lines++;
            why = "change";
        }
    }
    if (!why && c->pmic_plug && now - c->pmic_log_ms >= kPmicTrendMs && c->pmic_trend_lines < kPmicTrendLines) {
        c->pmic_trend_lines++;
        why = "trend";
    }
    if (why) {
        c->pmic_logged = s;
        c->pmic_logged_plug = c->pmic_plug;
        c->pmic_log_ms = now;
        line_pmic(c, why);
    }
}

static void publish(PbatCore *c, uint64_t now, int policy_due)
{
    PbatSnapshot s;
    pagani_battery_core_merge(c, now, &s);
    /* the policy only on a fresh battery manager cycle: a write's effect is read back before the next decision */
    if (policy_due) policy(c, &s);
    if (s.percent < 0) return;   /* no charge known yet: nothing is shown */
    const PbatSnapshot old = c->shown;
    const int first = !c->published;
    const int state = !first && (old.external != s.external || old.charging != s.charging || old.fully_charged != s.fully_charged ||
                                 old.critical != s.critical);
    int differs = first;
    const uint8_t *a = (const uint8_t *)&old, *b = (const uint8_t *)&s;
    for (unsigned i = 0; i < sizeof s && !differs; ++i) differs = a[i] != b[i];
    if (!differs) return;
    c->shown = s;
    c->published = 1;
    c->publishes++;
    if (c->ops->publish) c->ops->publish(c->ops->ctx, &s, first);
    if (state && (s.external != old.external || s.charging != old.charging)) {
        /* the current moves with the charging state: read the gauge again soon */
        const uint64_t soon = now + 2u * kGaugeSoonMs;
        if (c->gauge_due > soon) c->gauge_due = soon;
    }
    const char *why = 0;
    if (first) why = "published";
    else if (state && c->state_lines < kStateLines) why = "changed";
    else if (c->cfg.log >= 2) why = "pass";
    else if ((s.percent != c->logged_percent || c->gauge.failures != c->logged_failures) && quiet_over(now, c->state_log_ms))
        why = "summary";
    if (why && c->cfg.log) {
        c->state_lines++;
        c->state_log_ms = now;
        c->logged_percent = s.percent;
        c->logged_failures = c->gauge.failures;
        line_state(c, &s, why);
    }
}

uint32_t pagani_battery_core_run(PbatCore *c)
{
    const uint64_t now = now_ms(c);
    PbmState *b = &c->bm;
    if (c->channel_up) {
        if (pagani_bm_busy(b) && c->request_deadline && now >= c->request_deadline) {
            const uint32_t done = b->cycles_done, sets = b->last_set_done;
            pagani_bm_timeout(b);
            c->request_deadline = 0;
            if (b->cycles_done != done) {c->cycle_done = 1; c->bm_cycle_ms = now;}
            if (b->last_set_done != sets) log_set(c);
        }
        if (now >= c->refresh_due) {
            pagani_bm_refresh(b, ++c->refreshes % kSlowEvery == 0);
            c->refresh_due = now + (c->cfg.refresh_ms ? c->cfg.refresh_ms : kRefreshMs);
        }
    }
    if (c->gauge_available && now >= c->gauge_due) {
        gauge_pass(c, now);
        uint64_t period = c->cfg.gauge_ms ? c->cfg.gauge_ms : c->channel_up ? kGaugeWithDspMs : kGaugeAloneMs;
        /* a gauge that stopped answering is asked less and less often (each failing pass may spend its retries), up to 5 min */
        if (!c->gauge_answering && c->gauge_fail_streak >= 3) {
            const uint32_t shift = c->gauge_fail_streak - 2 < 6 ? c->gauge_fail_streak - 2 : 6;
            period <<= shift;
            if (period > kGaugeBackoffMs) period = kGaugeBackoffMs;
        }
        /* a first vote for a change of the inferred charger: the confirming pass comes soon (a plug-in shows within seconds) */
        if (c->gauge_answering && c->inference_votes == 1 && period > kConfirmMs) period = kConfirmMs;
        c->gauge_due = now + period;
    }
    const int pmic = c->cfg.pmic && c->ops->pmic;
    if (pmic && now >= c->pmic_due) {
        pmic_pass(c, now);
        c->pmic_due = now + kPmicMs;
    }
    const int policy_due = c->cycle_done;
    if (c->cycle_done) {
        c->cycle_done = 0;
        const int layout_line = b->cycles_done >= 1 && b->layout != PbmLayoutUnknown && !c->dsp_lines;
        if (c->cfg.log >= 2 || (c->cfg.log && (b->cycles_done <= 2 || layout_line || quiet_over(now, c->dsp_log_ms)))) {
            c->dsp_log_ms = now;
            c->dsp_lines++;
            line_dsp(c);
        }
    }
    publish(c, now, policy_due);
    /* the next request (after the policy queued its write, if any) */
    if (c->channel_up && !pagani_bm_busy(b)) {
        uint8_t out[PBM_REQUEST_BYTES];
        const uint32_t n = pagani_bm_next(b, out);
        if (n) {
            if (c->ops->send(c->ops->ctx, out, n)) c->send_refused++;   /* the timer skips it */
            c->request_deadline = now + kRequestMs;
        }
    }
    uint64_t next = now + 60000;
    if (c->gauge_available && c->gauge_due < next) next = c->gauge_due;
    if (pmic && c->pmic_due < next) next = c->pmic_due;
    if (c->channel_up && c->refresh_due < next) next = c->refresh_due;
    if (c->channel_up && pagani_bm_busy(b) && c->request_deadline && c->request_deadline < next) next = c->request_deadline;
    return next <= now ? 1u : (uint32_t)(next - now);
}

void pagani_battery_core_export(const PbatCore *c, int32_t out[16])
{
    const PbatSnapshot *s = &c->shown;
    const PbmState *b = &c->bm;
    out[1] = c->published ? s->percent : -1;
    out[2] = s->voltage_mv;
    out[3] = s->current_ma;
    out[4] = s->temperature_centi;
    out[5] = s->present | s->external << 1 | s->charging << 2 | s->fully_charged << 3 | s->critical << 4 | c->gauge_answering << 5 |
             c->channel_up << 6 | c->published << 7 | (int32_t)b->layout << 8 | b->layout_assumed << 10 | b->enabled << 11 |
             (c->inference != PbatInfNone) << 12 | (int32_t)c->thermal << 13 | (int32_t)c->inference << 15 |
             (int32_t)(c->inference_changes > 255 ? 255 : c->inference_changes) << 17;
    out[6] = (int32_t)c->gauge.passes;
    out[7] = (int32_t)c->gauge.failures;
    out[8] = (int32_t)b->cycles_done;
    out[9] = (int32_t)b->responses;
    out[10] = (int32_t)(b->timeouts | b->errors << 16);
    out[11] = (int32_t)b->notifications;
    out[12] = (int32_t)(b->sets_ok | b->sets_failed << 16);
    out[13] = (int32_t)c->gauge_ok_passes;
    out[14] = (int32_t)c->publishes;
    out[15] = (int32_t)(c->soc_guarded | c->send_refused << 16);
}
