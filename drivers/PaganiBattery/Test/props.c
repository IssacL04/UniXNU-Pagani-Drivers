/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* Host test of the IOPMPowerSource properties (BatteryProps.c): the keys powerd must find, their types, the ones published only when the
 * value is known (BatteryProps.h gives the 21H461 facts behind each), and what must never be published. check-pagani-battery-host.py
 * also holds every key against the strings of the system volume's powerd. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../BatteryProps.h"

static unsigned checks;
#define CHECK(x) do {if (!(x)) {fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1);} ++checks;} while (0)

static const PbatProp *find(const PbatProp *p, unsigned n, const char *key)
{
    for (unsigned i = 0; i < n; ++i) if (!strcmp(p[i].key, key)) return &p[i];
    return 0;
}
static int32_t number(const PbatProp *p, unsigned n, const char *key)
{
    const PbatProp *e = find(p, n, key);
    CHECK(e && e->type == PbpNumber);
    return e->value;
}
static int flag(const PbatProp *p, unsigned n, const char *key)
{
    const PbatProp *e = find(p, n, key);
    CHECK(e && e->type == PbpBool);
    return e->value != 0;
}

int main(void)
{
    PbatProp p[PBAT_PROPS_MAX];
    /* a full gauge reading with a charger */
    PbatSnapshot s = {0};
    s.present = s.external = s.charging = 1;
    s.percent = 87; s.voltage_mv = 3950; s.current_ma = 1200; s.instant_ma = 1300; s.temperature_centi = 2950; s.temperature_valid = 1;
    s.raw_mah = 5100; s.full_mah = 5900; s.design_mah = 6260; s.cycles = 37; s.cycles_known = 1; s.health_percent = 95;
    s.time_remaining_min = 40; s.adapter_family = 0xe000400au; s.adapter_mv = 9000; s.adapter_ma = 2000; s.adapter_watts = 18;
    s.sources = PbatSrcGauge | PbatSrcCharger;
    unsigned n = pagani_battery_props(&s, p);
    CHECK(n > 20 && n <= PBAT_PROPS_MAX);
    for (unsigned i = 0; i < n; ++i)
        for (unsigned k = i + 1; k < n; ++k) CHECK(strcmp(p[i].key, p[k].key));   /* a key once */
    /* powerd ignores the source without these two */
    CHECK(number(p, n, "CurrentCapacity") == 87 && number(p, n, "MaxCapacity") == 100);
    /* powerd counts these only as booleans */
    CHECK(flag(p, n, "BatteryInstalled") && flag(p, n, "ExternalConnected") && flag(p, n, "ExternalChargeCapable"));
    CHECK(flag(p, n, "AppleRawExternalConnected") && flag(p, n, "IsCharging"));
    CHECK(!flag(p, n, "FullyCharged") && !flag(p, n, "AtCriticalLevel"));
    CHECK(number(p, n, "Voltage") == 3950 && number(p, n, "Amperage") == 1200 && number(p, n, "InstantAmperage") == 1300);
    CHECK(number(p, n, "TimeRemaining") == 40 && number(p, n, "Temperature") == 2950);
    /* the health figure's inputs */
    CHECK(number(p, n, "CycleCount") == 37 && number(p, n, "DesignCapacity") == 6260 && number(p, n, "NominalChargeCapacity") == 5900);
    CHECK(number(p, n, "AppleRawMaxCapacity") == 5900 && number(p, n, "AppleRawCurrentCapacity") == 5100);
    CHECK(number(p, n, "MaximumCapacityPercent") == 95);
    const PbatProp *adapter = find(p, n, "AdapterDetails");
    CHECK(adapter && adapter->type == PbpAdapter);
    const PbatProp *source = find(p, n, "PaganiBatterySource");
    CHECK(source && source->type == PbpString && !strcmp(source->text, "gauge+charger"));
    /* Never: what would claim an authenticated Apple battery or steer powerd's calibration and service flags (BatteryProps.h). */
    CHECK(!find(p, n, "Serial") && !find(p, n, "BatteryData") && !find(p, n, "AppleAuthCPRelay"));
    /* A held-full phone: the bolt comes from the raw connection alone; a discharge is a negative Amperage (two's complement). */
    s.charging = 0; s.fully_charged = 1; s.current_ma = -350; s.instant_ma = -360;
    n = pagani_battery_props(&s, p);
    CHECK(flag(p, n, "AppleRawExternalConnected") && !flag(p, n, "IsCharging") && flag(p, n, "FullyCharged"));
    CHECK(number(p, n, "Amperage") == -350 && number(p, n, "InstantAmperage") == -360);
    /* The DSP alone, nothing of the gauge: what is unknown is left out, and a cycle count of 0 is known (powerd needs it). */
    PbatSnapshot d = {0};
    d.present = 1; d.percent = 150; d.time_remaining_min = 0xffff; d.sources = PbatSrcBattmgr;
    n = pagani_battery_props(&d, p);
    CHECK(number(p, n, "CurrentCapacity") == 100 && number(p, n, "TimeRemaining") == 0xffff && number(p, n, "Voltage") == 0);
    CHECK(!find(p, n, "CycleCount") && !find(p, n, "DesignCapacity") && !find(p, n, "NominalChargeCapacity") &&
          !find(p, n, "AppleRawMaxCapacity") && !find(p, n, "AppleRawCurrentCapacity") && !find(p, n, "MaximumCapacityPercent") &&
          !find(p, n, "Temperature"));
    CHECK(!strcmp(find(p, n, "PaganiBatterySource")->text, "dsp"));
    d.cycles_known = 1; d.percent = -3;
    n = pagani_battery_props(&d, p);
    CHECK(number(p, n, "CycleCount") == 0 && number(p, n, "CurrentCapacity") == 0);
    /* the names BatteryProps.h documents for the source */
    CHECK(!strcmp(pagani_battery_source_name(PbatSrcGauge), "gauge") && !strcmp(pagani_battery_source_name(PbatSrcGauge | PbatSrcInferred), "gauge+inferred"));
    CHECK(!strcmp(pagani_battery_source_name(PbatSrcCharger), "dsp+charger"));
    CHECK(!strcmp(pagani_battery_source_name(PbatSrcGauge | PbatSrcPmic), "gauge+pmic"));
    printf("PASS props: %u checks\n", checks);
    return 0;
}
