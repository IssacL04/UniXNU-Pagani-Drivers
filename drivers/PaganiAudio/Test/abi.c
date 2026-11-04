// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test of the shared memory layout (PaganiAudioAbi.h) that the kernel service, the iOS plug-in and the tests compile against. The
// header asserts its own layout in every compile (kernel C++, plug-in C for iOS, host C/C++); this test prints it and, with
// -DSHENG_ABI_H="<path>/ShengAudioAbi.h", checks that every field sheng's protocol has sits at the same offset (the port renamed it and
// only added fields where sheng had padding or nothing).
// Build: cc -std=c11 -Wall -Wextra -Werror -I.. abi.c -o abi  [-DSHENG_ABI_H='"/path/ShengAudioAbi.h"']
#include "PaganiAudioAbi.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef SHENG_ABI_H
#include SHENG_ABI_H
#endif

static unsigned checks;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #x); abort(); } checks++; } while (0)
#define SAME(a, b, f) CHECK(offsetof(a, f) == offsetof(b, f))

int main(void)
{
    CHECK(PAGANI_AUDIO_MAGIC == (('P' << 24) | ('G' << 16) | ('A' << 8) | 'U'));
    CHECK(sizeof(PaganiAudioControl) == 136 && offsetof(PaganiAudioControl, out_taken) == 128);
    CHECK(offsetof(PaganiAudioControl, lead_frames) == 28 && offsetof(PaganiAudioControl, ticks_per_frame_q32) == 40);
    CHECK(offsetof(PaganiAudioControl, out_frames) == 64 && offsetof(PaganiAudioControl, out_late) == 80 && offsetof(PaganiAudioControl, out_seq) == 88);
    CHECK(sizeof(PaganiAudioDiag) == 424 && PAGANI_AUDIO_DIAG_OFFSET + sizeof(PaganiAudioDiag) == 2472);
    CHECK(offsetof(PaganiAudioDiag, selector) == 8 && offsetof(PaganiAudioDiag, where) == 136 && offsetof(PaganiAudioDiag, volume_milli) == 264);
    CHECK(offsetof(PaganiAudioDiag, interval) == 324 && offsetof(PaganiAudioDiag, io_pri) == 376 && offsetof(PaganiAudioDiag, host_clock) == 408);
    CHECK(sizeof(PaganiAudioTrace) == 4 + 8 * PAGANI_AUDIO_TRACE_SLOTS && PAGANI_AUDIO_TRACE_OFFSET + sizeof(PaganiAudioTrace) == 3588);
    // The route probe: after the trace, inside the page, and the options bit that enables it next to the trace's.
    CHECK(PAGANI_AUDIO_PROBE_OFFSET >= 3588 && PAGANI_AUDIO_PROBE_OFFSET + sizeof(PaganiAudioProbe) <= PAGANI_AUDIO_CONTROL_BYTES);
    CHECK(sizeof(PaganiAudioProbe) == 72 && offsetof(PaganiAudioProbe, reason) == 4 && offsetof(PaganiAudioProbe, ours_vold) == 24);
    CHECK(offsetof(PaganiAudioProbe, vcsp_status) == 32 && offsetof(PaganiAudioProbe, category) == 40 && offsetof(PaganiAudioProbe, mode) == 44);
    CHECK(offsetof(PaganiAudioProbe, out_ports) == 52 && offsetof(PaganiAudioProbe, vad_volume_milli) == 64);
    CHECK(PaganiAudioOptionTraceRouting == 1 && PaganiAudioOptionProbeRoute == 2 && PaganiAudioProbeStart == 3);
    CHECK(PAGANI_AUDIO_RING_FRAMES * PAGANI_AUDIO_OUT_CHANNELS * 2 == 262144 && PAGANI_AUDIO_RING_FRAMES * PAGANI_AUDIO_IN_CHANNELS * 2 == 131072);
    CHECK(PAGANI_AUDIO_LEAD_FRAMES == 14400 && PAGANI_AUDIO_RATE / PAGANI_AUDIO_PERIOD_FRAMES == 100);
    CHECK(PaganiAudioSetHostClock == 4 && PaganiAudioStopInput == 3 && PaganiAudioMemoryInput == 2);
    // Revision 2: additions in space revision 1 left free, numbering that continues revision 1's.
    CHECK(PAGANI_AUDIO_VERSION == 1 && PAGANI_AUDIO_REVISION == 2 && PAGANI_AUDIO_REVISION_MAGIC == (('P' << 24) | ('G' << 16) | ('A' << 8) | 'R'));
    CHECK(PAGANI_AUDIO_REVISION_OFFSET == 1024 && sizeof(PaganiAudioRevision) == 16 && offsetof(PaganiAudioRevision, actuator_frames) == 8);
    CHECK(PAGANI_AUDIO_REVISION_OFFSET >= sizeof(PaganiAudioControl) && PAGANI_AUDIO_REVISION_OFFSET + 16 <= PAGANI_AUDIO_DIAG_OFFSET);
    CHECK(PaganiAudioOptionMxProbe == 4 && PaganiAudioOptionSrzlRepair == 8 && PaganiAudioOptionActuator == 16 &&
          PaganiAudioOptionVaSettings == 32);
    CHECK(PAGANI_AUDIO_MX_OFFSET == 3728 && sizeof(PaganiAudioMx) == 44 && offsetof(PaganiAudioMx, progress) == 4 &&
          offsetof(PaganiAudioMx, attempts) == 36 && offsetof(PaganiAudioMx, va_settings) == 40 &&
          PAGANI_AUDIO_MX_OFFSET >= PAGANI_AUDIO_PROBE_OFFSET + sizeof(PaganiAudioProbe));
    CHECK(PAGANI_AUDIO_MX_OFFSET + sizeof(PaganiAudioMx) <= PAGANI_AUDIO_CONTROL_BYTES && PaganiAudioVaSettingsGiven == 1 &&
          PaganiAudioVaSettingsOtherProduct == 5);
    CHECK(PaganiAudioMxSerial == 1u << 9 && PaganiAudioMxQueueAlive == 1u << 11 && PaganiAudioMxFilterShift == 12 &&
          PaganiAudioMxUnverified == 1u << 31);
    CHECK(PaganiAudioMemoryActuator == 3 && PaganiAudioStartActuator == 5 && PaganiAudioStopActuator == 6 && PaganiAudioActuatorRunning == 16);
    CHECK(PAGANI_AUDIO_ACT_MAGIC == (('P' << 24) | ('G' << 16) | ('A' << 8) | 'H') && PAGANI_AUDIO_ACT_RATE == 48000);
    CHECK(PAGANI_AUDIO_ACT_RING_FRAMES == 16384 && PAGANI_AUDIO_ACT_IO_FRAMES == 256 && PAGANI_AUDIO_ACT_SAFETY_FRAMES == 128 &&
          PAGANI_AUDIO_ACT_TAKE_FRAMES == 64 && PAGANI_AUDIO_ACT_BYTES == 4096 + 32768);
    CHECK(sizeof(PaganiAudioActuatorControl) == 168 && offsetof(PaganiAudioActuatorControl, va_device) == 160 &&
          offsetof(PaganiAudioActuatorControl, ticks_per_frame_q32) == 32 &&
          offsetof(PaganiAudioActuatorControl, state) == 48 && offsetof(PaganiAudioActuatorControl, written) == 56 &&
          offsetof(PaganiAudioActuatorControl, taken) == 72 && offsetof(PaganiAudioActuatorControl, loads) == 80 &&
          offsetof(PaganiAudioActuatorControl, frames) == 96 && offsetof(PaganiAudioActuatorControl, peak) == 116 &&
          offsetof(PaganiAudioActuatorControl, unknown_sets) == 128 && offsetof(PaganiAudioActuatorControl, sound_frame) == 144);
#ifdef SHENG_ABI_H
    // sheng's protocol, field by field.
    SAME(PaganiAudioControl, ShengAudioControl, magic); SAME(PaganiAudioControl, ShengAudioControl, version);
    SAME(PaganiAudioControl, ShengAudioControl, rate); SAME(PaganiAudioControl, ShengAudioControl, lead_frames);
    SAME(PaganiAudioControl, ShengAudioControl, state); CHECK(offsetof(PaganiAudioControl, backend) == offsetof(ShengAudioControl, reserved0));
    SAME(PaganiAudioControl, ShengAudioControl, ticks_per_frame_q32); SAME(PaganiAudioControl, ShengAudioControl, out_anchor_host);
    SAME(PaganiAudioControl, ShengAudioControl, in_anchor_host); SAME(PaganiAudioControl, ShengAudioControl, out_frames);
    SAME(PaganiAudioControl, ShengAudioControl, in_frames); SAME(PaganiAudioControl, ShengAudioControl, out_late);
    SAME(PaganiAudioControl, ShengAudioControl, in_late); SAME(PaganiAudioControl, ShengAudioControl, out_seq);
    SAME(PaganiAudioControl, ShengAudioControl, in_seq); SAME(PaganiAudioControl, ShengAudioControl, out_written);
    SAME(PaganiAudioControl, ShengAudioControl, in_written); SAME(PaganiAudioControl, ShengAudioControl, out_written_host);
    SAME(PaganiAudioControl, ShengAudioControl, out_holes);
    CHECK(sizeof(ShengAudioControl) == 128 && offsetof(PaganiAudioControl, options) == offsetof(ShengAudioControl, out_holes) + 4);
    SAME(PaganiAudioDiag, ShengAudioDiag, count); SAME(PaganiAudioDiag, ShengAudioDiag, loads); SAME(PaganiAudioDiag, ShengAudioDiag, selector);
    SAME(PaganiAudioDiag, ShengAudioDiag, where); SAME(PaganiAudioDiag, ShengAudioDiag, volume_milli);
    SAME(PaganiAudioDiag, ShengAudioDiag, volume_sets); SAME(PaganiAudioDiag, ShengAudioDiag, mix_peak);
    SAME(PaganiAudioDiag, ShengAudioDiag, in_misses); SAME(PaganiAudioDiag, ShengAudioDiag, ahead_min);
    SAME(PaganiAudioDiag, ShengAudioDiag, behind_max); SAME(PaganiAudioDiag, ShengAudioDiag, io_frames);
    SAME(PaganiAudioDiag, ShengAudioDiag, writes); SAME(PaganiAudioDiag, ShengAudioDiag, overlaps);
    SAME(PaganiAudioDiag, ShengAudioDiag, overlap_max); SAME(PaganiAudioDiag, ShengAudioDiag, frames_total);
    SAME(PaganiAudioDiag, ShengAudioDiag, gaps); SAME(PaganiAudioDiag, ShengAudioDiag, gap_frames);
    SAME(PaganiAudioDiag, ShengAudioDiag, interval); SAME(PaganiAudioDiag, ShengAudioDiag, last_end);
    SAME(PaganiAudioDiag, ShengAudioDiag, last_host); SAME(PaganiAudioDiag, ShengAudioDiag, skew_min);
    SAME(PaganiAudioDiag, ShengAudioDiag, skew_max); SAME(PaganiAudioDiag, ShengAudioDiag, io_pri);
    SAME(PaganiAudioDiag, ShengAudioDiag, io_pri_min); SAME(PaganiAudioDiag, ShengAudioDiag, io_constraint);
    SAME(PaganiAudioDiag, ShengAudioDiag, host_clock); SAME(PaganiAudioDiag, ShengAudioDiag, host_clock_result);
    CHECK(sizeof(ShengAudioDiag) == offsetof(PaganiAudioDiag, dropped_frames));
    CHECK(SHENG_AUDIO_DIAG_OFFSET == PAGANI_AUDIO_DIAG_OFFSET && SHENG_AUDIO_TRACE_OFFSET == PAGANI_AUDIO_TRACE_OFFSET);
    CHECK(SHENG_AUDIO_RING_FRAMES == PAGANI_AUDIO_RING_FRAMES && SHENG_AUDIO_PERIOD_FRAMES == PAGANI_AUDIO_PERIOD_FRAMES);
    CHECK(ShengAudioSetHostClock == PaganiAudioSetHostClock && ShengAudioMemoryInput == PaganiAudioMemoryInput);
    printf("PASS abi: %u checks (control %zu bytes, diagnostics %zu at %u, trace at %u, route probe at %u; revision 2 at %u, mx at %u, "
           "actuator header %zu; sheng's fields at the same offsets)\n", checks, sizeof(PaganiAudioControl), sizeof(PaganiAudioDiag),
           PAGANI_AUDIO_DIAG_OFFSET, PAGANI_AUDIO_TRACE_OFFSET, PAGANI_AUDIO_PROBE_OFFSET, PAGANI_AUDIO_REVISION_OFFSET, PAGANI_AUDIO_MX_OFFSET,
           sizeof(PaganiAudioActuatorControl));
#else
    printf("PASS abi: %u checks (control %zu bytes, diagnostics %zu at %u, trace at %u, route probe at %u; revision 2 at %u, mx at %u, "
           "actuator header %zu)\n", checks, sizeof(PaganiAudioControl), sizeof(PaganiAudioDiag), PAGANI_AUDIO_DIAG_OFFSET,
           PAGANI_AUDIO_TRACE_OFFSET, PAGANI_AUDIO_PROBE_OFFSET, PAGANI_AUDIO_REVISION_OFFSET, PAGANI_AUDIO_MX_OFFSET,
           sizeof(PaganiAudioActuatorControl));
#endif
    return 0;
}
