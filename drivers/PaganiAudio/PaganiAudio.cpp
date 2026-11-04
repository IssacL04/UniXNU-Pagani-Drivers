// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026, UniXNU contributors. All rights reserved.
// The OnePlus 13T audio service (boot argument pagani-audio=1). Stage7Probe calls pagani_audio_start() after the display and the GPU
// have started; without the boot argument it returns at once and nothing here runs, maps or allocates. With it:
//   1  the control page and the two rings of PaganiAudioAbi.h (IOBufferMemoryDescriptor, shared with user space)
//   2  the engine (PaganiAudioEngine.c): the timeline anchored on the host clock now, the silent backend
//   3  the classes of PaganiAudioService.cpp, registered late with the kernel's module (as Stage7Probe does for the peripherals)
//   4  the service thread, then the service PaganiAudio, which iOS's audio server plug-in (hal/PaganiAudioPlugIn.c) opens
// The thread serves the plug-in until reboot: it starts and stops each direction as the clients ask, runs the backend and writes a
// rationed summary to the boot log (BootProgress's ramoops U lines) when anything changed. The silent backend takes every playback
// period by the clock and clears it and captures zeros: iOS sees a running output and input device with a volume control, apps play
// (without sound). A DSP backend replaces it later through pagani_audio_set_backend(): same rings, same timeline, same plug-in.
// No failure stops the boot: a step that fails is recorded in pagani_audio_status[0] and the boot log, and the service is not published.
// The thread-and-requests structure follows UniXNU-sheng package/AudioProbe.cpp (revision 08cde20: setupShared, publishService, serve,
// sheng_audio_memory, sheng_audio_request); the hardware path of that file is not part of this one.
// ABI revision 2 (PaganiAudioAbi.h), each part default off:
//   pagani-audio-mx=1     the plug-in also writes MediaExperience's start progress ("audio mx" rows)
//   pagani-audio-srzl     bit 0: the plug-in puts MediaExperience's lost serialization listener on the routing layer once (row "audio mx
//                         ... srzl="); bit 1: with the actuator, it gives VirtualAudio the actuator settings its table lacks for J120 ("va=")
//   pagani-haptics-ios=1 or 2 with pagani-haptics=1: the haptic actuator. A fourth memory (memory type 3), the plug-in's "Actuator" device
//                         and the bridge (PaganiAudioHaptics.c) on this service's thread, which feeds the vibrator's stream API
//                         (PaganiHaptics.h; weak defaults below when PaganiHaptics is not linked). pagani-audio-haptics-rate: the FIFO
//                         rate (8000 default, 16000, 24000, 48000); the cap is the vibrator's (pagani-haptics-vmax-mv, default 3600);
//                         pagani-audio-haptics-sustain-mv: the sustained level (default 1260, PaganiAudioHaptics.h).
// With pagani-audio-trace=1 the failing calls and throws of the routing layer that the plug-in traces are also printed ("audio vaerr").
#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOService.h>
#include <libkern/OSKextLib.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <mach/kmod.h>
#include <pexpert/pexpert.h>
#include <stdarg.h>
#include "PaganiAudioEngine.h"
#include "PaganiAudioHaptics.h"

extern "C" kmod_info_t g_kernel_kmod_info;
extern "C" void pagani_audio_service_static_init(void) __asm__("__GLOBAL__sub_I_PaganiAudioService.cpp");
extern "C" int pagani_audio_service_publish(const char *backend);
extern "C" volatile uint64_t pagani_audio_clock[4];

// BootProgress's ramoops log (drivers/BootProgress RamoopsLog.c). The kernel is linked statically, where an undefined weak reference is an
// error, so these weak defaults stand in when it is absent; BootProgress's definitions replace them.
extern "C" __attribute__((weak)) void boot_progress_trace(const char *) {}
extern "C" __attribute__((weak)) int boot_progress_trace_enabled(void) {return 0;}
// PaganiHaptics' stream API (drivers/PaganiHaptics PaganiHaptics.h, the same prototypes): these report the vibrator as absent (-19) in a
// kernel without it; its strong definitions replace them in the link (both archives are force-loaded).
extern "C" __attribute__((weak)) int pagani_haptics_stream_begin(uint32_t, uint32_t) {return -19;}
extern "C" __attribute__((weak)) int pagani_haptics_stream_write(const int8_t *, uint32_t) {return -19;}
extern "C" __attribute__((weak)) int pagani_haptics_stream_end(int) {return -19;}

extern "C" {
// [0] stage: 1 starting, 2 memory, 3 engine, 4 classes, 5 thread, 6 serving; negative: the step that failed.
// [1] opens, [2] closes, [3] memory maps, [4] pid of the last opener, [5] backend id, [6] mode, [7] options.
volatile int32_t pagani_audio_status[8];
}

namespace {
// The boot log is BootProgress's ramoops events ring (32 KiB, shared with every driver and the crash records), so the audio rows are
// rationed: pagani-audio-log=1 (default) writes a summary round when something an observer cares about changed (opens, loads,
// starts, late or dropped audio, unknown properties, the volume, the clock), at most every 30 s for the first 12 rounds and every
// 10 min after; 2 also counts the moving counters and allows a round every 10 s for the first 16 rounds, then 1's 10 min (while audio
// IO runs those counters move in every round, and a round takes over 500 bytes of the ring: unbounded, level 2 pushed the boot lines
// out within minutes of IO; bounded, it can stay on for a diagnosis run); 0 writes the start line and failures.
constexpr uint32_t kOpenLines = 16;      // opens and closes that get a line of their own
constexpr uint32_t kEarlyRounds = 12;
constexpr uint32_t kVerboseRounds = 16;  // rounds pagani-audio-log=2 may write 10 s apart

PaganiAudioEngine gEngine;
IOBufferMemoryDescriptor *gMemory[4];   // control, playback, capture; the actuator (revision 2) only when it is offered
PaganiAudioHaptics gHaptics;
uint32_t gActuator, gHapticsRate = PAGANI_AUDIO_HAPTICS_RATE, gHapticsCapMv = PAGANI_AUDIO_HAPTICS_CAP_MV, gHapticsSustainMv;
IOLock *gLock;
volatile uint32_t gStarted, gServing, gEvents;
uint32_t gHostClock = 1, gOptions, gLogLevel = 1, gRounds;
char gLastOpener[17];

uint64_t msAt(uint64_t now) {
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(now, &ns);
    return ns / 1000000ULL;
}

void trace(bool console, const char *format, ...) __attribute__((format(printf, 2, 3)));
void trace(bool console, const char *format, ...) {
    char line[192];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(line, sizeof line, format, arguments);
    va_end(arguments);
    boot_progress_trace(line);
    if (console) IOLog("%s\n", line);
}

void fail(int step, const char *what) {
    pagani_audio_status[0] = -step;
    trace(true, "audio: failed at %s (step %d)", what, step);
}

void wake() {
    IOLockLock(gLock);
    gEvents = gEvents + 1;
    IOLockWakeup(gLock, (event_t)&gEvents, false);
    IOLockUnlock(gLock);
}

const char *fourcc(uint32_t v, char text[5]) {
    for (int i = 0; i < 4; ++i) {
        const char c = (char)(v >> (24 - 8 * i));
        text[i] = c >= 0x20 && c < 0x7f ? c : '.';
    }
    text[4] = 0;
    return text;
}

// The summary rows of the boot log: the service, the plug-in's view (the diagnostic half of the control page, written by the plug-in,
// read here only to be printed) and the properties the plug-in could not answer.
struct Summary {uint64_t key; uint64_t lastMs; uint64_t clock;};
Summary gSummary;

// The plug-in's route probe (PaganiAudioAbi.h, PaganiAudioProbe): what the audio server's routing layer and MediaExperience make of the
// device, which decides whether SpringBoard shows the volume HUD. A row whenever it changed (the probe count, its trigger and the volume
// control's read count aside), 5 s apart at least and 24 rows per boot at most; a probe the plug-in is still writing (odd seq) waits.
// The probe is a seqlock (the writer is pagani_audio_probe_route): its fields count only when seq held the same even value before and
// after them, and the acquire fence (dmb ishld) keeps the field reads from completing after the second read of seq.
constexpr uint32_t kRouteRows = 24;
constexpr uint64_t kRouteSpacingMs = 5000;
struct RouteLog {uint64_t hash; uint64_t lastMs; uint32_t rows;};
RouteLog gRoute;

void routeRow(uint64_t ms) {
    const PaganiAudioProbe *p =
        reinterpret_cast<const PaganiAudioProbe *>(reinterpret_cast<const uint8_t *>(gEngine.ctl) + PAGANI_AUDIO_PROBE_OFFSET);
    const uint32_t seq = __atomic_load_n(&p->seq, __ATOMIC_ACQUIRE);
    if (!seq || (seq & 1u)) return;
    const uint32_t v[15] = {p->devices, p->va_plugin, p->vad, p->ours, (uint32_t)p->ours_vold, (uint32_t)p->ours_running,
                            (uint32_t)p->vcsp_status, p->vcsp, p->category, p->mode, (uint32_t)p->ports_status, p->out_ports,
                            p->port_type[0], p->port_type[1], p->vad_volume_milli};
    const uint32_t reason = p->reason, queries = p->vol_queries;
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if (__atomic_load_n(&p->seq, __ATOMIC_RELAXED) != seq) return;
    uint64_t hash = 1469598103934665603ULL;
    for (uint32_t x : v) hash = (hash ^ x) * 1099511628211ULL;
    if (hash == gRoute.hash || gRoute.rows >= kRouteRows || (gRoute.lastMs && ms - gRoute.lastMs < kRouteSpacingMs)) return;
    gRoute.hash = hash;
    gRoute.lastMs = ms;
    gRoute.rows++;
    // vc: 'vcsp' as MediaExperience takes it (vaemCurrentRouteHasVolumeControl returns (status | value) != 0, so a failed read counts
    // as a volume control). Derived from the raw value and status the row keeps, so it changes no rationing.
    const uint32_t control = (v[6] | v[7]) != 0;
    char category[5], mode[5], port0[5], port1[5];
    trace(false, "audio route #%u r%u dev=%u va=%u vad=%u ours=%u vold=%d run=%d vcsp=%u/%d vc=%u cat=%s mode=%s out=%u %s %s ports=%d "
          "vol=%u q=%u", seq / 2, reason, v[0], v[1], v[2], v[3], (int32_t)v[4], (int32_t)v[5], v[7], (int32_t)v[6], control,
          fourcc(v[8], category), fourcc(v[9], mode), v[11], fourcc(v[12], port0), fourcc(v[13], port1), (int32_t)v[10], v[14], queries);
}

// MediaExperience's progress and the 'srzl' repair (PaganiAudioAbi.h, PaganiAudioMx; options MxProbe / SrzlRepair): a row when the block
// changed and then held still for one round (the plug-in writes it in one burst; no fence needed for a diagnostic), 5 s apart, 24 rows.
// Not inlined: the route probe's reader keeps the glue's only acquire fence (scripts/check-pagani-audio-host.py checks it).
struct MxLog {uint32_t gen, seen, rows; uint64_t hash, lastMs;};
MxLog gMx;
__attribute__((noinline)) void mxRow(uint64_t ms) {
    if (!(gOptions & (PaganiAudioOptionMxProbe | PaganiAudioOptionSrzlRepair | PaganiAudioOptionVaSettings))) return;
    const volatile PaganiAudioMx *m =
        reinterpret_cast<const volatile PaganiAudioMx *>(reinterpret_cast<const uint8_t *>(gEngine.ctl) + PAGANI_AUDIO_MX_OFFSET);
    const uint32_t gen = m->gen;
    if (!gen || gen != gMx.seen) {   // nothing yet, or still moving: look again next round
        gMx.seen = gen;
        return;
    }
    const uint32_t v[10] = {m->progress, (uint32_t)m->acq_ms, m->srzl_state, (uint32_t)m->srzl_status, (uint32_t)m->rwbu_status,
                            m->serial_after, m->filter_before, m->filter_after, m->attempts, m->va_settings};
    uint64_t hash = 1469598103934665603ULL;
    for (uint32_t x : v) hash = (hash ^ x) * 1099511628211ULL;
    if (hash == gMx.hash || gMx.rows >= kRouteRows || (gMx.lastMs && ms - gMx.lastMs < kRouteSpacingMs)) return;
    gMx.hash = hash;
    gMx.lastMs = ms;
    gMx.rows++;
    const char after = v[5] >= 0x20 && v[5] < 0x7f ? (char)v[5] : '-';
    char settings[12] = "";
    if (gOptions & PaganiAudioOptionVaSettings) snprintf(settings, sizeof settings, " va=%u", v[9]);
    trace(false, "audio mx p=0x%04x filt=%u acq=%d srzl=%u st=%d/%d after=%c filt=%u>%u n=%u%s", v[0], (v[0] >> PaganiAudioMxFilterShift) & 3u,
          (int32_t)v[1], v[2], (int32_t)v[3], (int32_t)v[4], after, v[6], v[7], v[8], settings);
}

// R3 (route-re 6): the routing layer's failing calls (kind 4) and throws (kind 8, the thrower's offset in VirtualAudio in place of the
// selector) that the plug-in traces with pagani-audio-trace=1: a row when their number grew, 10 s apart, 8 rows per boot. Offsets read
// against route-re's addresses: 0x232ac "not initialized yet", 0x9f530 no serialization context, inside sub_1BF4E0 hog mode.
struct VaLog {uint32_t count, rows; uint64_t lastMs;};
VaLog gVa;
__attribute__((noinline)) void vaRow(uint64_t ms) {
    if (!(gOptions & PaganiAudioOptionTraceRouting)) return;
    const volatile PaganiAudioTrace *tr =
        reinterpret_cast<const volatile PaganiAudioTrace *>(reinterpret_cast<const uint8_t *>(gEngine.ctl) + PAGANI_AUDIO_TRACE_OFFSET);
    const uint32_t n = tr->count < PAGANI_AUDIO_TRACE_SLOTS ? tr->count : PAGANI_AUDIO_TRACE_SLOTS;
    uint32_t va = 0;
    for (uint32_t i = 0; i < n; ++i) va += (tr->entry[i][1] & 0xcu) != 0;
    if (va == gVa.count || gVa.rows >= 8 || (gVa.lastMs && ms - gVa.lastMs < 10000)) return;
    gVa.count = va;
    gVa.lastMs = ms;
    gVa.rows++;
    char row[180];
    int at = snprintf(row, sizeof row, "audio vaerr n=%u", va);
    for (uint32_t i = 0, shown = 0; i < n && shown < 8 && at > 0 && at < (int)sizeof row; ++i) {
        const uint32_t selector = tr->entry[i][0], info = tr->entry[i][1];
        if (!(info & 0xcu)) continue;
        char text[5];
        if (info & 0x8u) at += snprintf(row + at, sizeof row - (size_t)at, " T+0x%x", selector);
        else at += snprintf(row + at, sizeof row - (size_t)at, " %s@%u%c", fourcc(selector, text), info >> 24,
                            (char)((info >> 16) & 0xff) >= 0x20 && (char)((info >> 16) & 0xff) < 0x7f ? (char)((info >> 16) & 0xff) : '.');
        shown++;
    }
    trace(false, "%s", row);
}

// The haptic actuator (revision 2): the bridge's counters and the plug-in's, two rows when something an observer cares about changed
// (runs, IO, streams, refusals, peaks, the latency, clips, the plug-in's drops and unknown writes; not the moving frame counts), 10 s apart
// for the first 24 rounds, then 10 min.
struct HapticsLog {uint64_t key, lastMs; uint32_t rounds;};
HapticsLog gHapticsLog;
__attribute__((noinline)) void hapticsRow(uint64_t ms) {
    if (!gActuator) return;
    const PaganiAudioHapticsStats &s = gHaptics.stats;
    const volatile PaganiAudioActuatorControl *a = gHaptics.ctl;
    const uint32_t w[19] = {s.runs, s.run_ends, s.streams, s.stream_ends, s.rollovers, s.begin_failed, (uint32_t)s.last_rc, s.peak_in,
                            s.peak_out, s.clips, (uint32_t)s.latency_us, a->io_starts, a->io_stops, a->dropped_writes, a->unknown_sets,
                            a->loads, a->va_device, (uint32_t)a->hog, s.limited != 0};
    uint64_t key = 1469598103934665603ULL;
    for (uint32_t x : w) key = (key ^ x) * 1099511628211ULL;
    const uint64_t spacing = gHapticsLog.rounds < 24 ? 10000 : 600000;
    if (key == gHapticsLog.key || (gHapticsLog.lastMs && ms - gHapticsLog.lastMs < spacing)) return;
    gHapticsLog.key = key;
    gHapticsLog.lastMs = ms;
    gHapticsLog.rounds++;
    trace(false, "audio haptics runs=%u/%u io=%u/%u frames=%llu loud=%llu peak=%u/%u streams=%u/%u/%u fail=%u/%d sent=%llu/%llu "
          "clip=%u lat=%d/%dus", s.runs, s.run_ends, a->io_starts, a->io_stops, s.frames, s.loud_frames, s.peak_in, s.peak_out,
          s.streams, s.stream_ends, s.rollovers, s.begin_failed, s.last_rc, s.samples, s.refused, s.clips, s.latency_us, s.latency_max_us);
    char text[5];
    trace(false, "audio haptics plug loads=%u vad=%u hog=%d writes=%u frames=%llu peak=%u ahead=%d..%d drop=%llu/%u unk=%u %s@%u fmt=%u "
          "stale=%u skip=%llu fifo=%u cap=%u lim=%u", a->loads, a->va_device, a->hog, a->writes, (unsigned long long)a->frames, a->peak,
          a->ahead_min, a->ahead_max, (unsigned long long)a->dropped_frames, a->dropped_writes, a->unknown_sets,
          fourcc(a->unknown_selector, text), a->unknown_where >> 24, a->format_sets, s.stale, s.skipped, gHaptics.rate,
          s.cap_mv ? s.cap_mv : gHapticsCapMv, s.limited);
}

void summarize(uint64_t ms) {
    if (!gLogLevel) return;
    routeRow(ms);
    mxRow(ms);
    vaRow(ms);
    hapticsRow(ms);
    const PaganiAudioEngine &e = gEngine;
    const PaganiAudioEngineStats &s = e.stats;
    const PaganiAudioDiag *d = reinterpret_cast<const PaganiAudioDiag *>(reinterpret_cast<const uint8_t *>(e.ctl) + PAGANI_AUDIO_DIAG_OFFSET);
    uint64_t key = (uint64_t)s.out_starts + (uint64_t)s.in_starts * 3 + (uint64_t)s.out_refused * 5 + (uint64_t)s.in_refused * 7 +
                   (uint64_t)s.late * 11 + (uint64_t)s.holes * 13 + (uint64_t)s.switches * 17 + (uint64_t)s.refused_requests * 19 +
                   (uint64_t)d->loads * 23 + (uint64_t)d->count * 29 + (uint64_t)d->volume_sets * 31 + (uint64_t)d->dropped_writes * 37 +
                   (uint64_t)d->in_misses * 41 + (uint64_t)(uint32_t)d->host_clock_result * 43 + (uint64_t)pagani_audio_status[1] * 47 +
                   (uint64_t)pagani_audio_status[2] * 53;
    if (gLogLevel >= 2) key += s.taken * 59 + s.captured * 61 + (uint64_t)d->writes * 67;
    const uint64_t spacing = gLogLevel >= 2 && gRounds < kVerboseRounds ? 10000 : gRounds < kEarlyRounds ? 30000 : 600000;
    if (key == gSummary.key || (gSummary.lastMs && ms - gSummary.lastMs < spacing)) return;
    gSummary.key = key;
    gSummary.lastMs = ms;
    gRounds++;
    trace(false, "audio svc st=%x users=%u/%u req=%u bad=%u out=%u/%u/%u in=%u/%u taken=%llu played=%llu late=%u holes=%u skip=%u "
          "lead=%u peak=%u/%u", e.ctl->state, e.out_users, e.in_users, s.requests, s.refused_requests, s.out_starts, s.out_stops,
          s.out_refused, s.in_starts, s.in_stops, s.taken, s.played, s.late, s.holes, s.skipped, e.lead, s.peak_out, s.peak_in);
    trace(false, "audio plug loads=%u opens=%d/%d maps=%d by %s vol=%u sets=%u writes=%u miss=%u unk=%u clk=%u/%d", d->loads,
          pagani_audio_status[1], pagani_audio_status[2], pagani_audio_status[3], gLastOpener[0] ? gLastOpener : "-", d->volume_milli,
          d->volume_sets, d->writes, d->in_misses, d->count, d->host_clock, d->host_clock_result);
    // How the audio server's IO runs against the timeline (frames): write-ahead and read-behind, skew, late frames dropped, mixes into
    // frames already written (several IO contexts), skipped stretches, write intervals <15/25/40/80/160/more ms, its IO thread.
    if (d->writes)
        trace(false, "audio io frames=%u ahead=%d..%d behind=%d..%d skew=%d..%d drop=%u/%u overlap=%u/%u gaps=%u int=%u/%u/%u/%u/%u/%u "
              "pri=%d..%d tc=%d", d->frames_total, d->ahead_min, d->ahead_max, d->behind_min, d->behind_max, d->skew_min, d->skew_max,
              d->dropped_frames, d->dropped_writes, d->overlaps, d->overlap_max, d->gaps, d->interval[0], d->interval[1], d->interval[2],
              d->interval[3], d->interval[4], d->interval[5], d->io_pri_min, d->io_pri, d->io_tc);
    if (d->count) {
        char row[160];
        int n = snprintf(row, sizeof row, "audio unk n=%u", d->count);
        for (uint32_t i = 0; i < d->count && i < 8 && n > 0 && n < (int)sizeof row; ++i) {
            char text[5];
            n += snprintf(row + n, sizeof row - (size_t)n, " %s@%u%c", fourcc(d->selector[i], text), d->where[i] >> 24,
                          (char)(d->where[i] & 0xff) >= 0x20 ? (char)(d->where[i] & 0xff) : '.');
        }
        trace(false, "%s", row);
    }
    if (pagani_audio_clock[1] && pagani_audio_clock[1] != gSummary.clock) {
        gSummary.clock = pagani_audio_clock[1];
        trace(false, "audio clock step=%llu va=%llx pa=%llx value=%llx", pagani_audio_clock[0], pagani_audio_clock[1],
              pagani_audio_clock[2], pagani_audio_clock[3]);
    }
}

void serviceThread(void *, wait_result_t) {
    thread_set_thread_name(current_thread(), "pagani-audio");
    uint32_t seen = 0;
    for (;;) {
        const uint64_t now = mach_absolute_time(), ms = msAt(now);
        uint32_t wait = pagani_audio_engine_poll(&gEngine, now, ms);
        if (gActuator) {
            const uint32_t haptics = pagani_audio_haptics_poll(&gHaptics, &gEngine, now);
            if (haptics < wait) wait = haptics;
        }
        summarize(ms);
        IOLockLock(gLock);
        if (gEvents == seen) {
            uint64_t deadline = 0;
            clock_interval_to_deadline(wait, kMillisecondScale, &deadline);
            IOLockSleepDeadline(gLock, (event_t)&gEvents, deadline, THREAD_UNINT);
        }
        seen = gEvents;
        IOLockUnlock(gLock);
    }
}

bool setupMemory() {
    const IOOptionBits shared = static_cast<IOOptionBits>(kIODirectionInOut) | static_cast<IOOptionBits>(kIOMemoryKernelUserShared);
    const vm_size_t sizes[3] = {PAGANI_AUDIO_CONTROL_BYTES, PAGANI_AUDIO_RING_FRAMES * PAGANI_AUDIO_OUT_CHANNELS * sizeof(int16_t),
                                PAGANI_AUDIO_RING_FRAMES * PAGANI_AUDIO_IN_CHANNELS * sizeof(int16_t)};
    for (unsigned i = 0; i < 3; ++i) {
        gMemory[i] = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, shared, sizes[i], 4096);
        if (!gMemory[i] || !gMemory[i]->getBytesNoCopy()) return false;
    }
    if (gActuator) {
        gMemory[3] = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, shared, PAGANI_AUDIO_ACT_BYTES, 4096);
        if (!gMemory[3] || !gMemory[3]->getBytesNoCopy()) return false;
    }
    return true;
}

// The vibrator's stream API for the bridge.
int hapticsBegin(void *, uint32_t rate, uint32_t mv) {return pagani_haptics_stream_begin(rate, mv);}
int hapticsWrite(void *, const int8_t *samples, uint32_t count) {return pagani_haptics_stream_write(samples, count);}
int hapticsEnd(void *, int drain) {return pagani_haptics_stream_end(drain);}
uint32_t hapticsCap(void *) {return gHapticsCapMv;}
const PaganiAudioHapticsOps kHapticsOps = {hapticsBegin, hapticsWrite, hapticsEnd, hapticsCap, nullptr};

bool registerClasses() {
    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!handle) return false;
    pagani_audio_service_static_init();
    return OSMetaClass::postModLoad(handle) == kIOReturnSuccess;
}
}  // namespace

extern "C" {
// The memory a user client maps (PaganiAudioMemory*), retained for the caller; null until the service is up.
IOBufferMemoryDescriptor *pagani_audio_memory(uint32_t type) {
    if (!__atomic_load_n(&gServing, __ATOMIC_ACQUIRE) || type > PaganiAudioMemoryActuator) return nullptr;
    if (type == PaganiAudioMemoryActuator && !gActuator) return nullptr;
    IOBufferMemoryDescriptor *m = gMemory[type];
    if (m) {
        m->retain();
        __atomic_add_fetch(&pagani_audio_status[3], 1, __ATOMIC_RELAXED);
    }
    return m;
}

int pagani_audio_request(PaganiAudioClientState *client, uint32_t selector) {
    if (!__atomic_load_n(&gServing, __ATOMIC_ACQUIRE)) return -1;
    if ((selector == PaganiAudioStartActuator || selector == PaganiAudioStopActuator) && !gActuator) return -1;
    const int r = pagani_audio_engine_request(&gEngine, client, selector);
    if (!r) wake();
    return r;
}

void pagani_audio_client_opened(const char *name, int pid) {
    const int32_t n = __atomic_add_fetch(&pagani_audio_status[1], 1, __ATOMIC_RELAXED);
    pagani_audio_status[4] = pid;
    strlcpy(gLastOpener, name, sizeof gLastOpener);
    if (gLogLevel && (uint32_t)n <= kOpenLines) trace(false, "audio: open %d by %s pid %d", n, name, pid);
}

void pagani_audio_client_closed(PaganiAudioClientState *client, const char *name) {
    const int32_t n = __atomic_add_fetch(&pagani_audio_status[2], 1, __ATOMIC_RELAXED);
    if (__atomic_load_n(&gServing, __ATOMIC_ACQUIRE)) {
        pagani_audio_engine_release(&gEngine, client);
        wake();
    }
    if (gLogLevel && (uint32_t)n <= kOpenLines) trace(false, "audio: close %d by %s", n, name);
}

int pagani_audio_host_clock_allowed(void) {return gHostClock != 0;}

// Whether the haptic actuator is offered (the user client passes its selectors only then).
int pagani_audio_actuator_enabled(void) {return __atomic_load_n(&gServing, __ATOMIC_ACQUIRE) && gActuator;}

// For a DSP backend (later): its answer callback (on the DSP thread) wakes the service thread, whose round runs the backend's pump.
void pagani_audio_wake(void) {
    if (__atomic_load_n(&gServing, __ATOMIC_ACQUIRE)) wake();
}

// For a DSP backend (later): the service switches to it at its next round; the running directions restart on it. 0 accepted.
int pagani_audio_set_backend(const PaganiAudioBackend *backend) {
    if (!backend || !__atomic_load_n(&gServing, __ATOMIC_ACQUIRE)) return -1;
    pagani_audio_engine_set_backend(&gEngine, backend);
    pagani_audio_status[5] = (int32_t)backend->id;
    wake();
    return 0;
}

// Stage7Probe's call (its weak default does nothing). Once per boot; returns at once without pagani-audio.
void pagani_audio_start(void) {
    uint32_t mode = 0, value = 0;
    if (!PE_parse_boot_argn("pagani-audio", &mode, sizeof mode) || !mode) return;
    if (__atomic_exchange_n(&gStarted, 1u, __ATOMIC_ACQ_REL)) return;
    if (PE_parse_boot_argn("pagani-audio-hostclock", &value, sizeof value)) gHostClock = value != 0;
    if (PE_parse_boot_argn("pagani-audio-trace", &value, sizeof value) && value) gOptions |= PaganiAudioOptionTraceRouting;
    gOptions |= PaganiAudioOptionProbeRoute;   // the plug-in's read-only route probe; pagani-audio-probe=0 leaves it out
    if (PE_parse_boot_argn("pagani-audio-probe", &value, sizeof value) && !value) gOptions &= ~(uint32_t)PaganiAudioOptionProbeRoute;
    if (PE_parse_boot_argn("pagani-audio-log", &value, sizeof value)) gLogLevel = value;
    // Revision 2 (each default off).
    if (PE_parse_boot_argn("pagani-audio-mx", &value, sizeof value) && value) gOptions |= PaganiAudioOptionMxProbe;
    if (PE_parse_boot_argn("pagani-audio-srzl", &value, sizeof value)) {
        if (value & 1u) gOptions |= PaganiAudioOptionSrzlRepair;
        if (value & 2u) gOptions |= PaganiAudioOptionVaSettings;
    }
    uint32_t ios = 0, haptics = 0;
    if (PE_parse_boot_argn("pagani-haptics-ios", &ios, sizeof ios) && ios) {
        // The actuator only with stage 1 or 2 and the vibrator running (pagani-haptics=1; 2 is its dry run, which writes nothing): the same
        // conditions under which PaganiHaptics plans /product/haptics (pgh_ios_plan), so iOS never waits for an actuator device that does
        // not come (route-re R4).
        if ((ios == 1 || ios == 2) && PE_parse_boot_argn("pagani-haptics", &haptics, sizeof haptics) && haptics == 1) {
            gActuator = 1;
            gOptions |= PaganiAudioOptionActuator;
            if (PE_parse_boot_argn("pagani-audio-haptics-rate", &value, sizeof value)) gHapticsRate = value;
            if (PE_parse_boot_argn("pagani-audio-haptics-sustain-mv", &value, sizeof value)) gHapticsSustainMv = value;
            if (PE_parse_boot_argn("pagani-haptics-vmax-mv", &value, sizeof value) && value)
                gHapticsCapMv = value < PAGANI_AUDIO_HAPTICS_FULL_MV ? value : PAGANI_AUDIO_HAPTICS_FULL_MV;
        } else {
            trace(true, "audio: haptics actuator not offered: pagani-haptics-ios=%u needs 1 or 2 with pagani-haptics=1 (it is %u)", ios, haptics);
        }
    }
    pagani_audio_status[6] = (int32_t)mode;
    pagani_audio_status[7] = (int32_t)gOptions;
    pagani_audio_status[0] = 1;
    gLock = IOLockAlloc();
    if (!gLock || !setupMemory()) return fail(2, "memory");
    pagani_audio_status[0] = 2;
    mach_timebase_info_data_t tb;
    clock_timebase_info(&tb);
    // Later backends are chosen here by mode; every mode serves through the silent backend until one is switched in.
    const PaganiAudioBackend *backend = &pagani_audio_silent_backend;
    if (pagani_audio_engine_init(&gEngine, gMemory[0]->getBytesNoCopy(), static_cast<int16_t *>(gMemory[1]->getBytesNoCopy()),
                                 static_cast<int16_t *>(gMemory[2]->getBytesNoCopy()), mach_absolute_time(), tb.numer, tb.denom, backend,
                                 gOptions)) return fail(3, "timebase");
    pagani_audio_status[5] = (int32_t)backend->id;
    if (gActuator &&
        pagani_audio_haptics_init(&gHaptics, &gEngine, gMemory[3]->getBytesNoCopy(), &kHapticsOps, gHapticsRate, 0, gHapticsSustainMv)) {
        // Cannot happen with the engine up; if it did, the actuator is not offered and audio goes on.
        gActuator = 0;
        gOptions &= ~(uint32_t)PaganiAudioOptionActuator;
        gEngine.ctl->options = gOptions;
        trace(true, "audio: haptics actuator not offered: bridge init failed");
    }
    // The revision 2 block (PaganiAudioAbi.h): a plug-in uses an addition only when it is here; the actuator's geometry only when offered.
    PaganiAudioRevision *revision =
        reinterpret_cast<PaganiAudioRevision *>(reinterpret_cast<uint8_t *>(gEngine.ctl) + PAGANI_AUDIO_REVISION_OFFSET);
    revision->actuator_frames = gActuator ? PAGANI_AUDIO_ACT_RING_FRAMES : 0;
    revision->actuator_rate = gActuator ? PAGANI_AUDIO_ACT_RATE : 0;
    revision->revision = PAGANI_AUDIO_REVISION;
    __atomic_store_n(&revision->magic, PAGANI_AUDIO_REVISION_MAGIC, __ATOMIC_RELEASE);
    pagani_audio_status[0] = 3;
    if (!registerClasses()) return fail(4, "class registration");
    pagani_audio_status[0] = 4;
    thread_t thread;
    if (kernel_thread_start(serviceThread, nullptr, &thread) != KERN_SUCCESS) return fail(5, "thread");
    thread_deallocate(thread);
    pagani_audio_status[0] = 5;
    // Serving before the service is visible: a client that opens it at once finds its memory.
    __atomic_store_n(&gServing, 1u, __ATOMIC_RELEASE);
    __atomic_or_fetch(&gEngine.ctl->state, PaganiAudioOutputReady | PaganiAudioInputReady, __ATOMIC_ACQ_REL);
    const int published = pagani_audio_service_publish(backend->name);
    if (published != 1) {
        __atomic_store_n(&gServing, 0u, __ATOMIC_RELEASE);
        return fail(6, published == -1 ? "service attach" : "service start");
    }
    pagani_audio_status[0] = 6;
    trace(true, "audio: up mode %u backend %s frame %llu/%llu ticks (q32 %llx) ring %u period %u lead %u hostclock %u options %x log %u",
          mode, backend->name, gEngine.tick_num, gEngine.tick_den, gEngine.ctl->ticks_per_frame_q32, gEngine.ring, gEngine.period,
          gEngine.lead, gHostClock, gOptions, gLogLevel);
    if (gActuator)
        trace(true, "audio: haptics actuator offered: ring %u frames, fifo %u Hz, full scale %u mV, cap %u mV, sustained %u mV, silence %u ms",
              PAGANI_AUDIO_ACT_RING_FRAMES, gHaptics.rate, gHaptics.full_mv, gHapticsCapMv, gHaptics.sustain_mv,
              (uint32_t)PAGANI_AUDIO_HAPTICS_SILENCE_MS);
}
}
