/* SPDX-License-Identifier: MIT */
#include "Sm8750Tsens.h"
#include <stddef.h>

int Sm8750TsensDecode(uint32_t status, int32_t *deci)
{
    int32_t value = (int32_t)(status & Sm8750TsensTempMask);
    if (value & 0x800)
        value -= 0x1000;    /* sign-extend bit 11 (Linux sign_extend32(temp, 11)) */
    if (!(status & Sm8750TsensValidBit)) {
        if (deci)
            *deci = 0;
        return 0;
    }
    if (deci)
        *deci = value;
    return 1;
}

int Sm8750TsensReadGpu(const Sm8750TsensIo *tm, Sm8750TsensSample *out)
{
    if (!out)
        return Sm8750TsensArgument;
    Sm8750TsensSample sample = {0};
    if (!tm || !tm->read) {
        *out = sample;
        return Sm8750TsensArgument;
    }
    for (unsigned n = Sm8750TsensGpuFirst; n <= Sm8750TsensGpuLast; ++n) {
        const unsigned i = n - Sm8750TsensGpuFirst;
        const uint32_t offset = Sm8750TsensStatusBase + Sm8750TsensStatusStride * n;
        int32_t deci = 0;
        int valid = 0;
        for (unsigned attempt = 0; attempt < Sm8750TsensReadTries && !valid; ++attempt) {
            sample.raw[i] = tm->read(tm->context, offset);
            ++sample.reads;
            valid = Sm8750TsensDecode(sample.raw[i], &deci);
        }
        if (!valid)
            continue;
        sample.deci[i] = (int16_t)deci;
        if (!sample.valid || deci > sample.max_deci) {
            sample.max_deci = deci;
            sample.hottest = n;
        }
        sample.valid |= 1u << i;
    }
    *out = sample;
    return sample.valid ? Sm8750TsensOk : Sm8750TsensNoData;
}

void Sm8750ThermalInit(Sm8750Thermal *thermal)
{
    if (!thermal)
        return;
    const Sm8750Thermal initial = {Sm8750ThermalUnknown, 0, 0, 0, 0, 0, 0, 0};
    *thermal = initial;
}

static uint32_t zoneFor(uint32_t zone, int32_t deci)
{
    /* Leave a zone only 5 C under its threshold; enter a hotter one at its threshold. */
    if (zone == Sm8750ThermalCritical && deci >= Sm8750ThermalCriticalDeci - Sm8750ThermalHysteresisDeci)
        return Sm8750ThermalCritical;
    if (deci >= Sm8750ThermalCriticalDeci)
        return Sm8750ThermalCritical;
    if ((zone == Sm8750ThermalHot || zone == Sm8750ThermalCritical) &&
        deci >= Sm8750ThermalHotDeci - Sm8750ThermalHysteresisDeci)
        return Sm8750ThermalHot;
    if (deci >= Sm8750ThermalHotDeci)
        return Sm8750ThermalHot;
    return Sm8750ThermalNormal;
}

uint32_t Sm8750ThermalUpdate(Sm8750Thermal *thermal, int read_result, const Sm8750TsensSample *sample)
{
    if (!thermal)
        return Sm8750ThermalUnreadableCapKhz;
    ++thermal->samples;
    const uint32_t before = thermal->zone;
    if (read_result == Sm8750TsensOk && sample && sample->valid) {
        thermal->invalid_streak = 0;
        if (!thermal->ever_valid || sample->max_deci > thermal->peak_deci)
            thermal->peak_deci = sample->max_deci;
        thermal->ever_valid = 1;
        thermal->last_deci = sample->max_deci;
        /* From Unknown the reading decides directly (no hysteresis to honour). */
        thermal->zone = zoneFor(before == Sm8750ThermalUnknown ? Sm8750ThermalNormal : before, sample->max_deci);
    } else {
        ++thermal->invalid_samples;
        if (thermal->invalid_streak < 0xffffffffu)
            ++thermal->invalid_streak;
        if (!thermal->ever_valid || thermal->invalid_streak >= Sm8750ThermalInvalidLimit)
            thermal->zone = Sm8750ThermalUnknown;
    }
    if (thermal->zone != before)
        ++thermal->transitions;
    return Sm8750ThermalCapKhz(thermal);
}

uint32_t Sm8750ThermalCapKhz(const Sm8750Thermal *thermal)
{
    if (!thermal)
        return Sm8750ThermalUnreadableCapKhz;
    switch (thermal->zone) {
    case Sm8750ThermalNormal:
        return 0;
    case Sm8750ThermalHot:
        return Sm8750ThermalHotCapKhz;
    case Sm8750ThermalCritical:
        return Sm8750ThermalCriticalCapKhz;
    default:
        return Sm8750ThermalUnreadableCapKhz;
    }
}
