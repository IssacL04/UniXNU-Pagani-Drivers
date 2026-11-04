/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved.
 * The property mapping follows UniXNU-sheng package/ShengBattery.cpp (https://github.com/UniXNU/UniXNU-sheng, commit 08cde20e,
 * ShengBattery::publish); the keys added and left out, and why, are the 21H461 facts listed in BatteryProps.h. */
#include "BatteryProps.h"

static void put(PbatProp *out, unsigned *n, const char *key, uint32_t type, int32_t value, const char *text)
{
    if (*n >= PBAT_PROPS_MAX) return;
    out[*n].key = key;
    out[*n].type = type;
    out[*n].value = value;
    out[*n].text = text;
    (*n)++;
}

const char *pagani_battery_source_name(uint32_t sources)
{
    if (sources & PbatSrcGauge) {
        return sources & PbatSrcCharger ? "gauge+charger" : sources & PbatSrcPmic ? "gauge+pmic" : sources & PbatSrcInferred ? "gauge+inferred" :
               "gauge";
    }
    return sources & PbatSrcCharger ? "dsp+charger" : "dsp";
}

unsigned pagani_battery_props(const PbatSnapshot *s, PbatProp out[PBAT_PROPS_MAX])
{
    unsigned n = 0;
    const int32_t percent = s->percent < 0 ? 0 : s->percent > 100 ? 100 : s->percent;
    put(out, &n, "BatteryInstalled", PbpBool, s->present != 0, 0);
    put(out, &n, "ExternalConnected", PbpBool, s->external != 0, 0);
    put(out, &n, "ExternalChargeCapable", PbpBool, s->external != 0, 0);
    /* the connection itself, before any charging decision: powerd's "Raw External Connected" and "Show Charging UI" */
    put(out, &n, "AppleRawExternalConnected", PbpBool, s->external != 0, 0);
    put(out, &n, "IsCharging", PbpBool, s->charging != 0, 0);
    put(out, &n, "FullyCharged", PbpBool, s->fully_charged != 0, 0);
    put(out, &n, "AtCriticalLevel", PbpBool, s->critical != 0, 0);
    put(out, &n, "CurrentCapacity", PbpNumber, percent, 0);
    put(out, &n, "MaxCapacity", PbpNumber, 100, 0);
    put(out, &n, "Voltage", PbpNumber, s->voltage_mv > 0 ? s->voltage_mv : 0, 0);
    put(out, &n, "Amperage", PbpNumber, s->current_ma, 0);
    put(out, &n, "InstantAmperage", PbpNumber, s->instant_ma, 0);
    put(out, &n, "TimeRemaining", PbpNumber, s->time_remaining_min, 0);
    /* the health figure's inputs (powerd 0x10002b118): CycleCount even at 0, the two capacities in mAh */
    if (s->cycles_known) put(out, &n, "CycleCount", PbpNumber, s->cycles, 0);
    if (s->design_mah > 0) put(out, &n, "DesignCapacity", PbpNumber, s->design_mah, 0);
    if (s->full_mah > 0) {
        put(out, &n, "NominalChargeCapacity", PbpNumber, s->full_mah, 0);
        put(out, &n, "AppleRawMaxCapacity", PbpNumber, s->full_mah, 0);
    }
    if (s->raw_mah > 0) put(out, &n, "AppleRawCurrentCapacity", PbpNumber, s->raw_mah, 0);
    if (s->health_percent > 0) put(out, &n, "MaximumCapacityPercent", PbpNumber, s->health_percent, 0);
    if (s->temperature_valid) put(out, &n, "Temperature", PbpNumber, s->temperature_centi, 0);   /* hundredths of a degree */
    put(out, &n, "AdapterDetails", PbpAdapter, 0, 0);
    put(out, &n, "PaganiBatterySource", PbpString, 0, pagani_battery_source_name(s->sources));
    return n;
}
