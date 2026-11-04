/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved.
 * Ported from UniXNU-sheng package/ShengBattmgr.c (https://github.com/UniXNU/UniXNU-sheng, commit 08cde20e): the request cycle, the
 * timeout handling and the notification refresh are sheng's; the property table, the layout check and the queued writes are the 13T's
 * (BatteryBattmgr.h). The property numbers are facts of the OPLUS kernel's battery_property_id/usb_property_id lists and of Qualcomm's
 * charger firmware interface; no code of those drivers is used. */
#include "BatteryBattmgr.h"
#include "BatteryText.h"

#define NA 0xffu
#define F_SLOW 1u        /* only in slow cycles (values that hardly change) */
#define F_PROBE 2u       /* only while the layout is not known for certain */
#define F_LAYOUT 4u      /* the number depends on the layout: not read while it is unknown */
#define NO_CYCLE 0xffu
#define UNDECIDED_LIMIT 3u

/* Battery: status 0, health 1, present 2, charge type 3, capacity 4 (hundredths of a percent), voltage now 7 (uV), current now 9 (uA,
 * discharge positive), charge control limit 10 (uA), then by layout: temperature (hundredths of a degree), charge counter (uAh), cycles,
 * design and full capacity (uAh), time to full and to empty (s); OPLUS BATT_CHG_EN 24. USB: online 0, voltage now 1 (uV), current now
 * 3 (uA), current max 4, input current limit 5 (uA), type 6, adapter type 7 (POWER_SUPPLY_USB_TYPE), real type 11. */
static const struct {uint8_t opcode, oplus, qti, flags;} kFields[PbmFields] = {
    [PbmStatus] = {PBM_OP_BATTERY_GET, 0, 0, 0},
    [PbmPresent] = {PBM_OP_BATTERY_GET, 2, 2, 0},
    [PbmCapacity] = {PBM_OP_BATTERY_GET, 4, 4, 0},
    [PbmVoltage] = {PBM_OP_BATTERY_GET, 7, 7, 0},
    [PbmCurrent] = {PBM_OP_BATTERY_GET, 9, 9, 0},
    [PbmChargeType] = {PBM_OP_BATTERY_GET, 3, 3, 0},
    [PbmChargeLimit] = {PBM_OP_BATTERY_GET, 10, 10, 0},
    [PbmHealth] = {PBM_OP_BATTERY_GET, 1, 1, F_SLOW},
    [PbmTemperature] = {PBM_OP_BATTERY_GET, 12, 13, F_LAYOUT},
    [PbmChargeCounter] = {PBM_OP_BATTERY_GET, 14, 15, F_LAYOUT},
    [PbmTimeToFull] = {PBM_OP_BATTERY_GET, 19, 20, F_LAYOUT},
    [PbmTimeToEmpty] = {PBM_OP_BATTERY_GET, 20, 21, F_LAYOUT},
    [PbmChargeEnable] = {PBM_OP_BATTERY_GET, 24, NA, F_LAYOUT},
    [PbmCycles] = {PBM_OP_BATTERY_GET, 15, 16, F_LAYOUT | F_SLOW},
    [PbmDesign] = {PBM_OP_BATTERY_GET, 16, 17, F_LAYOUT | F_SLOW},
    [PbmFull] = {PBM_OP_BATTERY_GET, 17, 18, F_LAYOUT | F_SLOW},
    [PbmUsbOnline] = {PBM_OP_USB_GET, 0, 0, 0},
    [PbmUsbVoltage] = {PBM_OP_USB_GET, 1, 1, 0},
    [PbmUsbCurrent] = {PBM_OP_USB_GET, 3, 3, 0},
    [PbmUsbCurrentMax] = {PBM_OP_USB_GET, 4, 4, 0},
    [PbmUsbInputLimit] = {PBM_OP_USB_GET, 5, 5, 0},
    [PbmUsbType] = {PBM_OP_USB_GET, 6, 6, 0},
    [PbmUsbAdapter] = {PBM_OP_USB_GET, 7, 7, 0},
    [PbmUsbRealType] = {PBM_OP_USB_GET, 11, 11, F_SLOW},
    [PbmProbe12] = {PBM_OP_BATTERY_GET, 12, 12, F_PROBE},
    [PbmProbe13] = {PBM_OP_BATTERY_GET, 13, 13, F_PROBE},
};

static void header(uint8_t *out, uint32_t type, uint32_t opcode)
{
    pbat_put32(out, PBM_OWNER);
    pbat_put32(out + 4, type);
    pbat_put32(out + 8, opcode);
}

void pagani_bm_init(PbmState *b)
{
    pbat_zero(b, sizeof *b);
    b->cursor = NO_CYCLE;
    b->last_timeout = -1;
    b->layout = PbmLayoutUnknown;
}

uint32_t pagani_bm_enable_request(uint8_t out[PBM_NOTIFY_REQUEST_BYTES])
{
    header(out, PBM_TYPE_NOTIFY, PBM_OP_SET_NOTIFY);
    pbat_put32(out + 12, 0);   /* battery 0 */
    pbat_put32(out + 16, 0);   /* power state */
    pbat_put32(out + 20, 0);   /* low capacity threshold: the firmware's own */
    pbat_put32(out + 24, 0);   /* high */
    return PBM_NOTIFY_REQUEST_BYTES;
}

static int layout_known(const PbmState *b) {return b->layout != PbmLayoutUnknown;}

uint32_t pagani_bm_property(const PbmState *b, int field)
{
    if (field < 0 || field >= PbmFields) return NA;
    return b->layout == PbmLayoutQti ? kFields[field].qti : kFields[field].oplus;
}

static int wanted(const PbmState *b, unsigned field)
{
    const unsigned flags = kFields[field].flags;
    if ((flags & F_SLOW) && !b->slow_due) return 0;
    if ((flags & F_PROBE) && layout_known(b) && !b->layout_assumed) return 0;
    if ((flags & F_LAYOUT) && !layout_known(b)) return 0;
    return pagani_bm_property(b, (int)field) != NA;
}

static void skip_to_wanted(PbmState *b)
{
    while (b->cursor < PbmFields && !wanted(b, b->cursor)) b->cursor++;
}

static void advance(PbmState *b)
{
    b->in_flight = 0;
    b->cursor++;
    skip_to_wanted(b);
}

void pagani_bm_refresh(PbmState *b, int slow)
{
    if (b->cursor != NO_CYCLE) {
        const uint32_t want = 1u + (slow != 0);
        if (want > b->refresh_wanted) b->refresh_wanted = want;
        return;
    }
    b->slow_due = slow || !pagani_bm_has(b, PbmFull) || !layout_known(b);
    b->cursor = 0;
    b->in_flight = 0;
    skip_to_wanted(b);
}

int pagani_bm_layout_from_probes(int has12, int32_t p12, int has13, int32_t p13)
{
    if (!has12 || !has13) return PbmLayoutUnknown;
    /* OPLUS: 12 the temperature (hundredths of a degree, -40..90 C), 13 the technology (POWER_SUPPLY_TECHNOLOGY_*, a small enum).
     * Qualcomm: 13 the temperature; a reading of 0..0.15 C is taken for an enum, not a temperature. */
    if (p13 >= 0 && p13 <= 15 && p12 >= -4000 && p12 <= 9000) return PbmLayoutOplus;
    if (p13 >= -4000 && p13 <= 9000 && !(p13 >= 0 && p13 <= 15)) return PbmLayoutQti;
    return PbmLayoutUnknown;
}

static void clear_layout_fields(PbmState *b)
{
    for (unsigned f = 0; f < PbmFields; ++f)
        if (kFields[f].flags & F_LAYOUT) b->valid &= ~(1u << f);
}

/* At the end of a cycle that read the probes: the layout, and a slow cycle at once to read what depends on it. */
static void decide_layout(PbmState *b)
{
    if (layout_known(b) && !b->layout_assumed) return;
    const int verdict = pagani_bm_layout_from_probes(pagani_bm_has(b, PbmProbe12), b->value[PbmProbe12],
                                                     pagani_bm_has(b, PbmProbe13), b->value[PbmProbe13]);
    if (verdict != PbmLayoutUnknown) {
        if (verdict != b->layout) clear_layout_fields(b);
        b->layout = (uint8_t)verdict;
        b->layout_assumed = 0;
        b->refresh_wanted = 2;
        return;
    }
    if (b->layout_assumed) return;
    if (++b->undecided_cycles >= UNDECIDED_LIMIT) {
        /* The probes never answer usefully: read with the OPLUS numbers (the 13T's), but writes of vendor properties stay refused. */
        b->layout = PbmLayoutOplus;
        b->layout_assumed = 1;
        b->refresh_wanted = 2;
    }
}

/* Ends the cycle when the cursor ran past the last field; starts the remembered one. Returns 1 when a cycle ended. */
static int finish(PbmState *b)
{
    if (b->cursor == NO_CYCLE || b->cursor < PbmFields) return 0;
    b->cursor = NO_CYCLE;
    b->cycles_done++;
    decide_layout(b);
    if (b->refresh_wanted) {
        const int slow = b->refresh_wanted == 2;
        b->refresh_wanted = 0;
        pagani_bm_refresh(b, slow);
    }
    return 1;
}

static int set_allowed(const PbmState *b, uint32_t opcode, uint32_t property)
{
    if (opcode == PBM_OP_USB_SET) return property == PbmPropUsbInputLimit;
    if (opcode != PBM_OP_BATTERY_SET) return 0;
    if (property == PbmPropChargeControlLimit) return 1;
    /* Vendor properties only with the OPLUS layout established by the probes: a wrong number could hit, for instance, the ship mode. */
    return property == PbmPropOplusChargeEnable && b->layout == PbmLayoutOplus && !b->layout_assumed;
}

int pagani_bm_set(PbmState *b, uint32_t opcode, uint32_t property, uint32_t value, uint32_t tag)
{
    if (!set_allowed(b, opcode, property)) return -2;
    if (b->queued >= PBM_PENDING_SETS) {b->sets_dropped++; return -1;}
    PbmSet *s = &b->queue[b->queued++];
    s->opcode = opcode;
    s->property = property;
    s->value = value;
    s->tag = tag;
    return 0;
}

int pagani_bm_busy(const PbmState *b) {return b->in_flight || b->set_in_flight;}

uint32_t pagani_bm_next(PbmState *b, uint8_t out[PBM_REQUEST_BYTES])
{
    if (b->in_flight || b->set_in_flight) return 0;
    if (b->queued) {
        b->flight = b->queue[0];
        for (uint32_t i = 1; i < b->queued; ++i) b->queue[i - 1] = b->queue[i];
        b->queued--;
        header(out, PBM_TYPE_REQUEST, b->flight.opcode);
        pbat_put32(out + 12, 0);
        pbat_put32(out + 16, b->flight.property);
        pbat_put32(out + 20, b->flight.value);
        b->set_in_flight = 1;
        b->sets_sent++;
        return PBM_REQUEST_BYTES;
    }
    if (b->cursor == NO_CYCLE || b->cursor >= PbmFields) return 0;
    header(out, PBM_TYPE_REQUEST, kFields[b->cursor].opcode);
    pbat_put32(out + 12, 0);
    pbat_put32(out + 16, pagani_bm_property(b, b->cursor));
    pbat_put32(out + 20, 0);
    b->in_flight = 1;
    return PBM_REQUEST_BYTES;
}

static void set_finished(PbmState *b, int32_t result)
{
    b->set_in_flight = 0;
    b->last_set = b->flight;
    b->last_set_result = result;
    b->last_set_done++;
    if (!result) b->sets_ok++;
    else b->sets_failed++;
}

void pagani_bm_timeout(PbmState *b)
{
    if (b->set_in_flight) {
        b->timeouts++;
        set_finished(b, -60);
        return;
    }
    if (b->cursor == NO_CYCLE || !b->in_flight) return;
    b->timeouts++;
    b->last_timeout = b->cursor;
    b->valid &= ~(1u << b->cursor);
    advance(b);
    finish(b);
}

int pagani_bm_receive(PbmState *b, const uint8_t *data, uint32_t length)
{
    if (!data || length < 12 || pbat_le32(data) != PBM_OWNER) {b->ignored++; return PbmIgnored;}
    const uint32_t opcode = pbat_le32(data + 8);
    if (opcode == PBM_OP_NOTIFY) {
        b->notifications++;
        b->last_notification = length >= 16 ? pbat_le32(data + 12) : 0;
        pagani_bm_refresh(b, 0);
        return PbmNotified;
    }
    if (opcode == PBM_OP_SET_NOTIFY) {b->enabled = 1; return PbmEnabled;}
    if (opcode == PBM_OP_BATTERY_SET || opcode == PBM_OP_USB_SET) {
        /* One write in flight: its answer is the one with its opcode (the firmware need not echo the property). */
        if (!b->set_in_flight || length < 24 || opcode != b->flight.opcode) {b->ignored++; return PbmIgnored;}
        set_finished(b, (int32_t)pbat_le32(data + 20));
        pagani_bm_refresh(b, 0);   /* read the effect back */
        return PbmSetDone;
    }
    if (b->cursor == NO_CYCLE || b->cursor >= PbmFields || !b->in_flight || length < 24) {b->ignored++; return PbmIgnored;}
    const uint32_t property = pbat_le32(data + 12), value = pbat_le32(data + 16), result = pbat_le32(data + 20);
    if (opcode != kFields[b->cursor].opcode || property != pagani_bm_property(b, b->cursor)) {b->ignored++; return PbmIgnored;}
    b->responses++;
    if (result) {
        b->errors++;
        b->valid &= ~(1u << b->cursor);
    } else {
        b->value[b->cursor] = (int32_t)value;
        b->valid |= 1u << b->cursor;
    }
    advance(b);
    return finish(b) ? PbmCycleDone : PbmAnswer;
}
