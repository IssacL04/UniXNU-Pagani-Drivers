// SPDX-License-Identifier: BSD-2-Clause-Patent
// The display probe's status rows (text rows 14-18, IMPLEMENTATION_PLAN M12 "叠加层"), laid out from the probe's readings and the display
// driver's counters, and the per-second rates the safe mode computes from the INTF_1 counters. Each row is at most 66 characters so a
// photograph of the screen reads it whole. Pure formatting and integer arithmetic (the kernel side is built without floating point), host
// tested (Test/scanout-flip.cpp, Test/dpu-safety.cpp).
#pragma once
#include <stdint.h>
#if defined(KERNEL)
#include <libkern/libkern.h>
#else
#include <stdio.h>
#endif
#include "NativeDpu.h"

enum { NATIVE_DPU_ROW_BYTES = 68, NATIVE_DPU_ROW_CHARS = 66 };
struct NativeDpuRowInputs {
    uint64_t teMillihertz;      // measured TE rate (lock or read-pointer frames per second); 0 unknown
    uint32_t frames;            // INTF_1 frame count increase over the last second
    uint8_t locked;             // TE lock in use (pagani-te=1)
    uint32_t relocks;
    uint8_t flipping;           // page flips in use: adds the FLIP row
    uint64_t flips,latches,latchMaxUs,latchSumUs,late,timeouts;
    uint32_t underruns;
};

// A rate in millihertz as whole hertz and hundredths, at most 99999.99 Hz.
static inline void native_dpu_hz(uint64_t millihertz, unsigned *hz, unsigned *hundredths) {
    if (millihertz > 99999990ull) millihertz = 99999990ull;
    *hz = unsigned(millihertz / 1000);
    *hundredths = unsigned(millihertz % 1000 / 10);
}

// The FLIP row (page flips): flips, latch time max/mean, late frames, underruns, latch timeouts.
static inline void native_dpu_flip_row(const NativeDpuRowInputs &in, char *row) {
    const uint64_t mean = in.latches ? in.latchSumUs / in.latches : 0;
    snprintf(row, NATIVE_DPU_ROW_CHARS + 1, "FLIP %llu LAT %llu.%llu/%llu.%llums MISS %llu UR %u TO %llu", (unsigned long long)in.flips,
             (unsigned long long)(in.latchMaxUs / 1000), (unsigned long long)(in.latchMaxUs % 1000 / 100),
             (unsigned long long)(mean / 1000), (unsigned long long)(mean % 1000 / 100), (unsigned long long)in.late, in.underruns,
             (unsigned long long)in.timeouts);
}

// The snapshot rows of pagani-dpu-probe=2 and 3: five rows when flipping, else four. Returns the number of rows. Mode 2 replaces row 3
// (it never reads the apps SMMU: "MDP SMMU not read") with native_dpu_path_row.
static inline unsigned native_dpu_rows(const NativeDpuState &s, const NativeDpuScanout *scanout, const NativeDpuRowInputs &in,
                                       char (*rows)[NATIVE_DPU_ROW_BYTES]) {
    using namespace nativedpu;
    unsigned hz = 0, hundredths = 0;
    native_dpu_hz(in.teMillihertz, &hz, &hundredths);
    snprintf(rows[0], NATIVE_DPU_ROW_CHARS + 1, "TE %u.%02uHz AR %08x FC %u/s %s RL %u", hz, hundredths, s.autorefresh, in.frames,
             in.locked ? "LOCK" : "FREE", in.relocks);
    unsigned c = scanout ? scanout->ctl : 0;
    if (c >= kCtlCount) c = 0;
    snprintf(rows[1], NATIVE_DPU_ROW_CHARS + 1, "CTL%u PIPE %08x FLUSH %08x INTF %08x LM %08x", c, s.ctl[c].pipeActive, s.ctl[c].flush,
             s.ctl[c].intfActive, s.ctl[c].layerActive);
    unsigned first = kPipeCount, more = 0;
    for (unsigned p = 0; p < kPipeCount; ++p) {
        const bool chosen = scanout ? (scanout->pipes >> p) & 1 : s.pipe[p].size != 0;
        if (!chosen) continue;
        if (first == kPipeCount) first = p; else ++more;
    }
    if (first == kPipeCount)
        for (unsigned p = 0; p < kPipeCount && first == kPipeCount; ++p) if (s.pipe[p].size) first = p;
    if (first == kPipeCount) snprintf(rows[2], NATIVE_DPU_ROW_CHARS + 1, "SSPP none fetching");
    else snprintf(rows[2], NATIVE_DPU_ROW_CHARS + 1, "SSPP %s SRC0 %08x STR %08x FMT %08x MR %x +%u", kPipeName[first], s.pipe[first].src0,
                  s.pipe[first].ystride0, s.pipe[first].format, s.pipe[first].multirect & 3, more);
    unsigned st = kStreamCount;
    for (unsigned i = 0; i < kStreamCount && st == kStreamCount; ++i) if (s.stream[i].slot != 0xff) st = i;
    if (!s.smmuRead) snprintf(rows[3], NATIVE_DPU_ROW_CHARS + 1, "MDP SMMU not read");
    else if (st == kStreamCount) snprintf(rows[3], NATIVE_DPU_ROW_CHARS + 1, "MDP SID %x..%x no route CR0 %08x", kMdpStreams[0],
                                          kMdpStreams[kStreamCount - 1], s.smmuCr0);
    else if (s.stream[st].bank == 0xff)
        snprintf(rows[3], NATIVE_DPU_ROW_CHARS + 1, "MDP SID %x S2C %08x SMR %08x %s", s.stream[st].sid, s.stream[st].s2cr, s.stream[st].smr,
                 ((s.stream[st].s2cr >> 16) & 3) == 1 ? "BYPASS" : "FAULT");
    else snprintf(rows[3], NATIVE_DPU_ROW_CHARS + 1, "MDP SID %x S2C %08x CB %u SCT %08x FSR %08x", s.stream[st].sid, s.stream[st].s2cr,
                  s.stream[st].bank, s.stream[st].sctlr, s.stream[st].fsr);
    if (!in.flipping) return 4;
    native_dpu_flip_row(in, rows[4]);
    return 5;
}

// The safe mode's per-second readings of the INTF_1 counters (pagani-dpu-probe=1, 2): the TE rate over the whole window (the tear check's
// frame field, +1 per TE, 16 bits) and, per interval between two readings, the TE rate and the INTF_1 frames per second (32-bit frame
// count) with their extremes. Up to eleven readings, so ten one-second intervals. A reading that comes more than `gap` ticks after the
// previous one (or not after it) starts over, so no counter can wrap unseen within an interval or the window.
struct NativeDpuRates {
    static constexpr unsigned kReadings = 11;
    uint64_t time[kReadings] = {};
    uint32_t frame[kReadings] = {};
    uint16_t te[kReadings] = {};
    unsigned count = 0, newest = 0;
    void reset() {count = 0; newest = 0;}
    void add(uint64_t now, const NativeDpuCounters &c, uint64_t gap) {
        if (count && (now <= time[newest] || now - time[newest] > gap)) reset();
        newest = count ? (newest + 1) % kReadings : 0;
        time[newest] = now; frame[newest] = c.frame; te[newest] = uint16_t(c.intCount >> 16);
        if (count < kReadings) ++count;
    }
    unsigned intervals() const {return count ? count - 1 : 0;}
    unsigned index(unsigned back) const {return (newest + kReadings - back) % kReadings;}
    // TE millihertz from the oldest reading to the newest (0 without an interval). `ticksPerSecond` at most 2^35.
    uint64_t teMillihertz(uint64_t ticksPerSecond) const {
        if (count < 2) return 0;
        const unsigned a = index(count - 1), b = newest;
        return uint64_t(uint16_t(te[b] - te[a])) * ticksPerSecond * 1000 / (time[b] - time[a]);
    }
    // Interval `back` (0: the newest): its TE in millihertz and its INTF_1 frames per second, rounded. false past the window.
    bool interval(unsigned back, uint64_t ticksPerSecond, uint64_t *teMillihertz, uint32_t *framesPerSecond) const {
        if (back + 1 >= count) return false;
        const unsigned n = index(back), o = index(back + 1);
        const uint64_t ticks = time[n] - time[o];
        uint64_t frames = uint32_t(frame[n] - frame[o]);
        if (frames > 0xffffffull) frames = 0xffffffull;             // a counter that jumped: keep the product in 64 bits
        *teMillihertz = uint64_t(uint16_t(te[n] - te[o])) * ticksPerSecond * 1000 / ticks;
        *framesPerSecond = uint32_t((frames * ticksPerSecond + ticks / 2) / ticks);
        return true;
    }
    // The extremes over every interval in the window; false without an interval.
    bool extremes(uint64_t ticksPerSecond, uint64_t *teMin, uint64_t *teMax, uint32_t *framesMin, uint32_t *framesMax) const {
        bool any = false;
        for (unsigned back = 0; back + 1 < count; ++back) {
            uint64_t t = 0; uint32_t f = 0;
            if (!interval(back, ticksPerSecond, &t, &f)) break;
            if (!any) {*teMin = *teMax = t; *framesMin = *framesMax = f; any = true; continue;}
            if (t < *teMin) *teMin = t;
            if (t > *teMax) *teMax = t;
            if (f < *framesMin) *framesMin = f;
            if (f > *framesMax) *framesMax = f;
        }
        return any;
    }
};

// What the safe-mode rows show besides the snapshot rows' inputs.
struct NativeDpuSafeRowInputs {
    NativeDpuRowInputs common;  // teMillihertz: the TE lock's or the window's; frames: the newest interval's; lock; flips; underruns
    uint32_t autorefresh;       // TEAR_AUTOREFRESH_CONFIG at the newest reading
    unsigned intervals;         // one-second intervals in the window (the "10S" row's span)
    uint64_t teMinMillihertz, teMaxMillihertz;
    uint32_t framesMin, framesMax;
    uint8_t mode;               // nativedpu::kProbe*
    uint8_t underrunsCounted;   // 0: this mode never reads MDP_INTR_STATUS ("UR -")
    uint32_t refused, refusedOffset, seconds;
};
static inline const char *native_dpu_mode_name(unsigned mode) {
    static const char *const names[] = {"OFF", "INTF_1 ONLY", "FLIP PATH", "FULL+SMMU", "GDSC ONLY"};
    return mode < sizeof(names) / sizeof(names[0]) ? names[mode] : "?";
}
// "UR -" when underruns are not counted, else the count; "REF n" with "@offset" of the first refused access when there was one.
static inline void native_dpu_counts(const NativeDpuSafeRowInputs &in, char *ur, unsigned urBytes, char *ref, unsigned refBytes) {
    if (in.underrunsCounted) snprintf(ur, urBytes, "%u", in.common.underruns); else snprintf(ur, urBytes, "-");
    if (in.refused) snprintf(ref, refBytes, "%u@%x", in.refused, in.refusedOffset); else snprintf(ref, refBytes, "0");
}

// pagani-dpu-probe=1: "TE 120.04Hz FC 120/s AR 80000001 FREE RL 0", the extremes over the window "10S TE 119.98-120.06Hz FC 119-121/s",
// the mode row "MODE 1 INTF_1 ONLY UR - REF 0 T 37s", and the FLIP row while flipping. Returns the number of rows (3 or 4).
static inline unsigned native_dpu_counter_rows(const NativeDpuSafeRowInputs &in, char (*rows)[NATIVE_DPU_ROW_BYTES]) {
    unsigned hz = 0, hundredths = 0;
    native_dpu_hz(in.common.teMillihertz, &hz, &hundredths);
    snprintf(rows[0], NATIVE_DPU_ROW_CHARS + 1, "TE %u.%02uHz FC %u/s AR %08x %s RL %u", hz, hundredths, in.common.frames, in.autorefresh,
             in.common.locked ? "LOCK" : "FREE", in.common.relocks);
    if (!in.intervals) snprintf(rows[1], NATIVE_DPU_ROW_CHARS + 1, "0S TE - FC -");
    else {
        unsigned lowHz = 0, lowHundredths = 0, highHz = 0, highHundredths = 0;
        native_dpu_hz(in.teMinMillihertz, &lowHz, &lowHundredths);
        native_dpu_hz(in.teMaxMillihertz, &highHz, &highHundredths);
        snprintf(rows[1], NATIVE_DPU_ROW_CHARS + 1, "%uS TE %u.%02u-%u.%02uHz FC %u-%u/s", in.intervals, lowHz, lowHundredths, highHz,
                 highHundredths, in.framesMin, in.framesMax);
    }
    char ur[12], ref[24];
    native_dpu_counts(in, ur, sizeof ur, ref, sizeof ref);
    snprintf(rows[2], NATIVE_DPU_ROW_CHARS + 1, "MODE %u %s UR %s REF %s T %us", unsigned(in.mode), native_dpu_mode_name(in.mode), ur, ref,
             in.seconds);
    if (!in.common.flipping) return 3;
    native_dpu_flip_row(in.common, rows[3]);
    return 4;
}

// pagani-dpu-probe=2's row 3 (instead of "MDP SMMU not read"): whether CTL 0 drives INTF_1, the pipes the snapshot read, underruns,
// refusals and seconds: "MODE 2 CTL0 INTF_1 P 010 UR 0 REF 0 T 37s".
static inline void native_dpu_path_row(const NativeDpuState &s, const NativeDpuSafeRowInputs &in, char *row) {
    char ur[12], ref[24];
    native_dpu_counts(in, ur, sizeof ur, ref, sizeof ref);
    snprintf(row, NATIVE_DPU_ROW_CHARS + 1, "MODE %u CTL0 %s P %03x UR %s REF %s T %us", unsigned(in.mode),
             (s.ctl[0].intfActive & nativedpu::kCtlIntf1) ? "INTF_1" : "NO_INTF1", unsigned(s.readPipes), ur, ref, in.seconds);
}

// pagani-dpu-probe=4: the GDSCs' raw GDSCR words with their state ("ON", "OFF", "/HW" when the hardware controls them), how often either
// word changed, and seconds: "GDSC CORE 80000000 ON INT2 00000001 OFF" and "MODE 4 GDSC ONLY CHG 0 T 37s". Two rows.
static inline const char *native_dpu_gdsc_state(uint32_t gdscr) {
    using namespace nativedpu;
    const bool on = gdscr & kGdscPwrOn, hw = gdscr & kGdscHwControl;
    return on ? (hw ? "ON/HW" : "ON") : (hw ? "OFF/HW" : "OFF");
}
static inline unsigned native_dpu_power_rows(bool read, uint32_t core, uint32_t int2, uint32_t changes, uint32_t seconds,
                                             char (*rows)[NATIVE_DPU_ROW_BYTES]) {
    if (!read) snprintf(rows[0], NATIVE_DPU_ROW_CHARS + 1, "GDSC not mapped");
    else snprintf(rows[0], NATIVE_DPU_ROW_CHARS + 1, "GDSC CORE %08x %s INT2 %08x %s", core, native_dpu_gdsc_state(core), int2,
                  native_dpu_gdsc_state(int2));
    snprintf(rows[1], NATIVE_DPU_ROW_CHARS + 1, "MODE %u %s CHG %u T %us", unsigned(nativedpu::kProbePower),
             native_dpu_mode_name(nativedpu::kProbePower), changes, seconds);
    return 2;
}

// While the display is off (native_dpu_idle) the probe reads nothing and shows only this row.
static inline unsigned native_dpu_idle_rows(unsigned mode, uint32_t seconds, char (*rows)[NATIVE_DPU_ROW_BYTES]) {
    snprintf(rows[0], NATIVE_DPU_ROW_CHARS + 1, "MODE %u %s DISPLAY OFF: NO READS T %us", mode, native_dpu_mode_name(mode), seconds);
    return 1;
}
