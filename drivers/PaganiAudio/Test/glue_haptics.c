// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026, UniXNU contributors. All rights reserved.
// The vibrator for Test/glue.cpp: strong definitions of PaganiHaptics' stream API (PaganiHaptics.h), which replace the weak defaults of
// PaganiAudio.cpp at link time exactly as drivers/PaganiHaptics does in the kernel link (so this file stays a translation unit of its own).
// Every call is recorded; the answers can be set.
#include <stdint.h>

int kstub_vib_begins, kstub_vib_ends, kstub_vib_writes, kstub_vib_begin_rc, kstub_vib_last_drain;
uint32_t kstub_vib_rate, kstub_vib_mv, kstub_vib_samples;
int kstub_vib_peak;

int pagani_haptics_stream_begin(uint32_t rate_hz, uint32_t vmax_mv)
{
    kstub_vib_begins++;
    kstub_vib_rate = rate_hz;
    kstub_vib_mv = vmax_mv;
    return kstub_vib_begin_rc;
}

int pagani_haptics_stream_write(const int8_t *samples, uint32_t count)
{
    kstub_vib_writes++;
    for (uint32_t i = 0; i < count; ++i) {
        const int m = samples[i] < 0 ? -samples[i] : samples[i];
        if (m > kstub_vib_peak) kstub_vib_peak = m;
    }
    kstub_vib_samples += count;
    return (int)count;
}

int pagani_haptics_stream_end(int drain)
{
    kstub_vib_ends++;
    kstub_vib_last_drain = drain;
    return 0;
}
