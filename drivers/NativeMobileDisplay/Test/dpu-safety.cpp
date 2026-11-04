// SPDX-License-Identifier: BSD-2-Clause-Patent
// host-test: drivers/NativeMobileDisplay/Test/dpu-safety.cpp drivers/NativeMobileDisplay/NativeDpu.h drivers/NativeMobileDisplay/NativeDpuRows.h
// Host test of the display probe's safety (2026-10-30: the old full snapshot hung the 13T 150 s after boot): the work each
// pagani-dpu-probe value selects together with pagani-te and pagani-fb-flip, the access guard (everything the safe mode, the TE lock and
// page flips touch passes; the hung snapshot's other blocks, unaligned and out-of-range offsets are refused; the apps SMMU is not in the
// mapping at all), the pipes CTL 0 names, the per-second TE and frame rates from the INTF_1 counters (16- and 32-bit wraps, gaps), and
// the rows of every mode (exact text, at most 66 characters).
#include "../NativeDpuRows.h"
#include <assert.h>
#include <initializer_list>
#include <stdio.h>
#include <string.h>

using namespace nativedpu;

static unsigned checks;
#define CHECK(x) do { assert(x); ++checks; } while (0)

static void plans() {
    for (int te = 0; te < 2; ++te) {
        const NativeDpuProbePlan off = native_dpu_probe_plan(0, te, false);
        CHECK(!off.thread && !off.rows && off.mode == kProbeOff);          // the TE lock alone starts no thread and reads no MDP top
        const NativeDpuProbePlan safe = native_dpu_probe_plan(1, te, false);
        CHECK(safe.thread && safe.rows && safe.counters && !safe.flipPath && !safe.full && !safe.power && !safe.underrun &&
              !safe.clearUnderrun && safe.delayMs == kProbeDelayMs && safe.mode == kProbeCounters);
        const NativeDpuProbePlan path = native_dpu_probe_plan(2, te, false);
        CHECK(path.thread && path.rows && path.counters && path.flipPath && !path.full && path.underrun && !path.clearUnderrun &&
              path.delayMs == kProbeDelayMs);
        const NativeDpuProbePlan full = native_dpu_probe_plan(3, te, false);
        CHECK(full.thread && full.full && full.counters && !full.flipPath && full.underrun && full.delayMs == kProbeFullDelayMs);
        const NativeDpuProbePlan power = native_dpu_probe_plan(4, te, false);
        CHECK(power.thread && power.rows && power.power && !power.counters && !power.underrun && !power.full &&
              power.delayMs == kProbeDelayMs);
        for (unsigned bad : {5u, 9u, 0xffffffffu}) {
            const NativeDpuProbePlan p = native_dpu_probe_plan(bad, te, false);
            CHECK(!p.thread && p.mode == kProbeOff);
        }
    }
    // Page flips alone: a thread without rows that counts and clears underruns, at once, but only after the trial passed.
    const NativeDpuProbePlan flip = native_dpu_probe_plan(0, false, true);
    CHECK(flip.thread && !flip.rows && flip.underrun && flip.clearUnderrun && flip.delayMs == 0 && !flip.counters);
    CHECK(!native_dpu_sample_underrun(flip, 0) && native_dpu_sample_underrun(flip, 1) && !native_dpu_sample_underrun(flip, 9));
    const NativeDpuProbePlan safeFlip = native_dpu_probe_plan(1, true, true);
    CHECK(safeFlip.counters && safeFlip.clearUnderrun && !native_dpu_sample_underrun(safeFlip, 0) && native_dpu_sample_underrun(safeFlip, 1));
    // The safe mode alone never reads the MDP top; modes 2 and 3 always sample it (without clearing).
    CHECK(!native_dpu_sample_underrun(native_dpu_probe_plan(1, true, false), 1));
    CHECK(native_dpu_sample_underrun(native_dpu_probe_plan(2, false, false), 0) && native_dpu_sample_underrun(native_dpu_probe_plan(3, false, false), 0));
    CHECK(!native_dpu_sample_underrun(native_dpu_probe_plan(4, false, false), 1));
}

static void guard() {
    // The safe mode's three reads, and the TE lock's reads and writes (lockTe, the tick, panelTo120, autorefresh off/on).
    for (uint32_t r : {kIntfFrameCount, kTearIntCount, kTearAutorefresh, kTearLineCount, kTearSyncVsync})
        CHECK(native_dpu_allowed(kIntf1 + r, false, 0) && native_dpu_block(kIntf1 + r) == kDpuIntf1Window);
    CHECK(native_dpu_allowed(kIntf1 + kTearSyncVsync, true, 0) && native_dpu_allowed(kIntf1 + kTearAutorefresh, true, 0));
    // The flip-path snapshot's INTF_1 reads; nothing else in INTF_1 may be written.
    for (uint32_t r : {kIntfTimingEngine, kIntfLineCount, kIntfStatus, kTearMdpVsyncSel, kTearCheckEn, kTearSyncHeight, kTearVsyncInit,
                       kTearStartPos, kTearRdPtrIrq, kIntf1Window - 4})
        CHECK(native_dpu_allowed(kIntf1 + r, false, 0) && !native_dpu_allowed(kIntf1 + r, true, 0));
    // INTF_1's own interrupt registers (+0x1c0) lie inside the DSI driver's window: readable, never written.
    CHECK(native_dpu_allowed(kIntf1IntrEn, false, 0) && !native_dpu_allowed(kIntf1IntrEn, true, 0xffff));
    // INTF_1 beyond that window (the tear-interrupt block at +0x800), the other interfaces.
    for (uint32_t o : {kIntf1 + kIntf1Window, kIntf1TearIntrEn, kIntf1TearIntrStatus, kIntf1TearIntrClear, 0x35000u + kTearIntCount,
                       0x37000u + kTearIntCount})
        CHECK(!native_dpu_allowed(o, false, 0xffff) && native_dpu_block(o) == kDpuOtherMdss);
    // MDSS and MDP top: only the three interrupt registers, each in its direction.
    CHECK(!native_dpu_allowed(kHwIntrStatus, false, 0xffff) && !native_dpu_allowed(0x1000, false, 0xffff));
    CHECK(native_dpu_allowed(kMdpIntrEn, false, 0) && native_dpu_allowed(kMdpIntrStatus, false, 0) && !native_dpu_allowed(kMdpIntrClear, false, 0));
    CHECK(native_dpu_allowed(kMdpIntrClear, true, 0) && !native_dpu_allowed(kMdpIntrStatus, true, 0) && !native_dpu_allowed(kMdpIntrEn, true, 0));
    // CTL 0: reads, and only its flush is written; CTL 1..5 not at all.
    for (uint32_t r : {kCtlTop, kCtlFlush, kCtlIntfActive, kCtlFetchActive, kCtlPipeActive, kCtlLayerActive})
        CHECK(native_dpu_allowed(kCtl0 + r, false, 0) && native_dpu_block(kCtl0 + r) == kDpuCtl0);
    CHECK(native_dpu_allowed(kCtl0 + kCtlFlush, true, 0) && !native_dpu_allowed(kCtl0 + kCtlStart, true, 0) &&
          !native_dpu_allowed(kCtl0 + kCtlPipeActive, true, 0));
    for (unsigned c = 1; c < kCtlCount; ++c)
        CHECK(!native_dpu_allowed(kCtl0 + kCtlStride * c + kCtlFlush, false, 0xffff) &&
              !native_dpu_allowed(kCtl0 + kCtlStride * c + kCtlFlush, true, 0xffff));
    // Pipes: only those CTL 0 named; their addresses are the only writes.
    const uint16_t named = uint16_t(1u << 0 | 1u << 6);                    // VIG0 and DMA2
    for (unsigned p = 0; p < kPipeCount; ++p) {
        unsigned which = 99;
        CHECK(native_dpu_block(kPipeBase[p] + kSrc0Addr, &which) == kDpuPipe && which == p);
        CHECK(!native_dpu_allowed(kPipeBase[p] + kSrc0Addr, false, 0));
        const bool on = (named >> p) & 1;
        CHECK(native_dpu_allowed(kPipeBase[p] + kSrcSize, false, named) == on && native_dpu_allowed(kPipeBase[p] + kMultirectOpMode, false, named) == on);
        CHECK(native_dpu_allowed(kPipeBase[p] + kSrc0Addr, true, named) == on && native_dpu_allowed(kPipeBase[p] + kSrc1Addr, true, named) == on);
        CHECK(!native_dpu_allowed(kPipeBase[p] + kSrcOpMode, true, named) && !native_dpu_allowed(kPipeBase[p] + kSrcFormat, true, named));
        CHECK(native_dpu_allowed(kPipeBase[p] + kPipeBytes - 4, false, named) == on);
        CHECK(!native_dpu_allowed(kPipeBase[p] + kPipeBytes, false, 0xffff));     // the SSPP's clock control (+0x330 is inside, +0x344 not)
    }
    // Unaligned, past the mapping.
    for (uint32_t o : {kIntf1 + kTearIntCount + 2, uint32_t(kMdssBytes), uint32_t(kMdssBytes) + kIntf1, 0xfffffffcu})
        CHECK(!native_dpu_allowed(o, false, 0xffff) && native_dpu_block(o) == kDpuOutside);
    // The snapshot that hung: of its 3 + 14 + 30 + 70 reads only the INTF_1 window, the two interrupt registers and CTL 0 pass without
    // named pipes (the apps SMMU is a separate mapping the guard never sees: 0x15000000 is far outside kMdssBytes).
    unsigned passed = 0, total = 0;
    auto count = [&](uint32_t o) {++total; passed += native_dpu_allowed(o, false, 0);};
    count(kHwIntrStatus); count(kMdpIntrEn); count(kMdpIntrStatus);
    for (uint32_t r : {kIntfTimingEngine, kIntfFrameCount, kIntfLineCount, kIntfStatus, kTearMdpVsyncSel, kTearCheckEn, kTearSyncVsync,
                       kTearSyncHeight, kTearVsyncInit, kTearIntCount, kTearStartPos, kTearRdPtrIrq, kTearLineCount, kTearAutorefresh})
        count(kIntf1 + r);
    for (unsigned c = 0; c < kCtlCount; ++c)
        for (uint32_t r : {kCtlTop, kCtlFlush, kCtlIntfActive, kCtlPipeActive, kCtlLayerActive}) count(kCtl0 + kCtlStride * c + r);
    for (unsigned p = 0; p < kPipeCount; ++p)
        for (uint32_t r : {kSrcSize, kSrc0Addr, kSrc1Addr, kSrcYStride0, kSrcFormat, kSrcOpMode, kMultirectOpMode}) count(kPipeBase[p] + r);
    CHECK(total == 117 && passed == 2 + 14 + 5);
    CHECK(native_dpu_block(uint32_t(kAppsSmmuBase)) == kDpuOutside && native_dpu_block(uint32_t(kGdscBase)) == kDpuOutside);
    // A flip's writes on a CTL 0 scanout all pass with the pipes CTL 0 named.
    NativeDpuState s = {};
    s.valid = 1;
    s.ctl[0] = {0, 0, kCtlIntf1, kPipeActive[0], 0};
    s.fetchActive[0] = kPipeActive[6];
    s.pipe[0] = {0x0a5004c0, uint32_t(kSplashBase), uint32_t(kSplashBase) + 0x6000, 0x1300, 0x237ff, 0, kMultirectRec1};
    s.pipe[6] = {0x0a5004c0, uint32_t(kSplashBase) + 0x980, 0, 0x1300, 0x237ff, 0, 0};
    const uint16_t pipes = native_dpu_ctl0_pipes(s.ctl[0], s.fetchActive[0]);
    CHECK(pipes == named);
    NativeDpuScanout out;
    CHECK(native_dpu_scanout(s, kSplashBase, 0xc3f000, &out) && out.ctl == 0 && out.pipes == named && out.rec1 == 1);
    NativeDpuWrite w[2 * kPipeCount + 1];
    const unsigned n = native_dpu_flip_writes(out, kSplashBase + 0xc3f000, w, 2 * kPipeCount + 1);
    CHECK(n == 4 && w[n - 1].offset == kCtl0 + kCtlFlush);
    for (unsigned i = 0; i < n; ++i) {
        CHECK(native_dpu_allowed(w[i].offset, true, pipes));
        CHECK(native_dpu_allowed(w[i].offset, true, 0) == (i + 1 == n));   // without the named pipes only CTL 0's flush passes
    }
}

static void ctl0() {
    NativeDpuCtl c = {0, 0, kCtlIntf1, kPipeActive[0] | kPipeActive[5], 0};
    CHECK(native_dpu_ctl0_pipes(c, 0) == (1u << 0 | 1u << 5));
    CHECK(native_dpu_ctl0_pipes(c, kPipeActive[9]) == (1u << 0 | 1u << 5 | 1u << 9));
    c.pipeActive = 0;
    CHECK(native_dpu_ctl0_pipes(c, 0) == 0 && native_dpu_ctl0_pipes(c, kPipeActive[3]) == 1u << 3);
    c.pipeActive = 0xffffffffu;
    CHECK(native_dpu_ctl0_pipes(c, 0) == (1u << kPipeCount) - 1);
    c.intfActive = 1u << 0;                                                // CTL 0 drives INTF_0, not the panel: nothing is read
    CHECK(native_dpu_ctl0_pipes(c, 0xffffffffu) == 0);
    c.intfActive = 0;
    CHECK(native_dpu_ctl0_pipes(c, 0xffffffffu) == 0);
}

// The tear check's frame field and INTF_1's frame count at mach time t (ticks): TE at 120.04 Hz from an offset, one frame per TE.
static NativeDpuCounters reading(uint64_t t, uint64_t tps, uint32_t teBase, uint32_t frameBase, uint32_t line) {
    const uint64_t tes = t * 12004 / (100 * tps);
    return {uint32_t(frameBase + tes), uint32_t(((teBase + tes) & 0xffff) << 16 | line), 0x80000001u};
}

static void rates() {
    const uint64_t tps = 19200000;                                          // the mach timebase (CNTFRQ 19.2 MHz)
    NativeDpuRates r;
    uint64_t te = 0, lo = 0, hi = 0;
    uint32_t fps = 0, flo = 0, fhi = 0;
    CHECK(r.intervals() == 0 && r.teMillihertz(tps) == 0 && !r.interval(0, tps, &te, &fps) && !r.extremes(tps, &lo, &hi, &flo, &fhi));
    // Fifteen readings about a second apart (1.00, 1.01, 1.02 s), both counters wrapping on the way.
    uint64_t t = 7 * tps;
    for (unsigned i = 0; i < 15; ++i) {
        r.add(t, reading(t, tps, 0xffc0, 0xffffff00u, 1234), 5 * tps);
        t += tps + (i % 3) * tps / 100;
    }
    CHECK(r.intervals() == 10 && r.count == NativeDpuRates::kReadings);
    const uint64_t mhz = r.teMillihertz(tps);
    CHECK(mhz > 119900 && mhz < 120200);
    CHECK(r.interval(0, tps, &te, &fps) && te > 118000 && te < 122000 && fps >= 119 && fps <= 121);
    CHECK(r.extremes(tps, &lo, &hi, &flo, &fhi) && lo <= hi && lo > 118000 && hi < 122000 && flo >= 119 && fhi <= 121 && flo <= fhi);
    CHECK(!r.interval(10, tps, &te, &fps) && r.interval(9, tps, &te, &fps));
    // A reading more than the gap after the previous one, or not after it, starts over.
    t += 6 * tps;
    r.add(t, reading(t, tps, 0, 0, 0), 5 * tps);
    CHECK(r.intervals() == 0 && r.teMillihertz(tps) == 0);
    r.add(t + tps, reading(t + tps, tps, 0, 0, 0), 5 * tps);
    CHECK(r.intervals() == 1 && r.teMillihertz(tps) > 119000);
    r.add(t + tps, reading(t + tps, tps, 0, 0, 0), 5 * tps);
    CHECK(r.intervals() == 0);
    // A frame counter that jumps (garbage, a reset): clamped, no overflow; a TE count that stands still: 0 Hz.
    r.reset();
    r.add(tps, {0, 5u << 16, 0}, 5 * tps);
    r.add(2 * tps, {0x80000000u, 5u << 16, 0}, 5 * tps);
    CHECK(r.interval(0, tps, &te, &fps) && te == 0 && fps == 0xffffff && r.teMillihertz(tps) == 0);
    // A 24 MHz timebase gives the same rates.
    NativeDpuRates q;
    const uint64_t fast = 24000000;
    for (unsigned i = 0; i < 4; ++i) q.add((i + 1) * fast, reading((i + 1) * fast, fast, 7, 7, 0), 5 * fast);
    CHECK(q.intervals() == 3 && q.teMillihertz(fast) > 119600 && q.teMillihertz(fast) < 120400);
}

static void rows() {
    char text[5][NATIVE_DPU_ROW_BYTES];                                  // PAGANI_DPU_ROWS (BootProgress PeripheralStatus.h)
    NativeDpuSafeRowInputs in = {};
    in.common.teMillihertz = 120040; in.common.frames = 120;
    in.autorefresh = 0x80000001; in.intervals = 10;
    in.teMinMillihertz = 119980; in.teMaxMillihertz = 120060; in.framesMin = 119; in.framesMax = 121;
    in.mode = kProbeCounters; in.seconds = 37;
    CHECK(native_dpu_counter_rows(in, text) == 3);
    CHECK(!strcmp(text[0], "TE 120.04Hz FC 120/s AR 80000001 FREE RL 0"));
    CHECK(!strcmp(text[1], "10S TE 119.98-120.06Hz FC 119-121/s"));
    CHECK(!strcmp(text[2], "MODE 1 INTF_1 ONLY UR - REF 0 T 37s"));
    // TE lock, refusals, counted underruns, flips: the FLIP row is the snapshot rows' FLIP row.
    in.common.locked = 1; in.common.relocks = 2; in.refused = 2; in.refusedOffset = 0x80016018u; in.underrunsCounted = 1;
    in.common.flipping = 1; in.common.flips = 1000; in.common.latches = 990; in.common.latchMaxUs = 8400; in.common.latchSumUs = 990 * 4100;
    in.common.late = 3;
    CHECK(native_dpu_counter_rows(in, text) == 4);
    CHECK(!strcmp(text[0], "TE 120.04Hz FC 120/s AR 80000001 LOCK RL 2"));
    CHECK(!strcmp(text[2], "MODE 1 INTF_1 ONLY UR 0 REF 2@80016018 T 37s"));
    CHECK(!strcmp(text[3], "FLIP 1000 LAT 8.4/4.1ms MISS 3 UR 0 TO 0"));
    char flipRow[NATIVE_DPU_ROW_BYTES];
    native_dpu_flip_row(in.common, flipRow);
    NativeDpuState empty = {};
    char snapshot[5][NATIVE_DPU_ROW_BYTES];
    CHECK(native_dpu_rows(empty, nullptr, in.common, snapshot) == 5 && !strcmp(snapshot[4], flipRow) && !strcmp(snapshot[4], text[3]));
    CHECK(!strcmp(snapshot[3], "MDP SMMU not read"));
    // Before the first interval.
    in.intervals = 0;
    native_dpu_counter_rows(in, text);
    CHECK(!strcmp(text[1], "0S TE - FC -"));
    // Mode 2's path row: CTL 0 on INTF_1 with the pipes it read, or not.
    NativeDpuState s = {};
    s.ctl[0].intfActive = kCtlIntf1; s.readPipes = 0x010;
    in.mode = kProbeFlipPath; in.refused = 0;
    native_dpu_path_row(s, in, text[0]);
    CHECK(!strcmp(text[0], "MODE 2 CTL0 INTF_1 P 010 UR 0 REF 0 T 37s"));
    s.ctl[0].intfActive = 1; s.readPipes = 0;
    native_dpu_path_row(s, in, text[0]);
    CHECK(!strcmp(text[0], "MODE 2 CTL0 NO_INTF1 P 000 UR 0 REF 0 T 37s"));
    // Mode 4: GDSCR words and their state.
    CHECK(native_dpu_power_rows(true, 0x80000000u, 0x00000001u, 0, 37, text) == 2);
    CHECK(!strcmp(text[0], "GDSC CORE 80000000 ON INT2 00000001 OFF") && !strcmp(text[1], "MODE 4 GDSC ONLY CHG 0 T 37s"));
    native_dpu_power_rows(true, 0x80000002u, 0x00000002u, 3, 37, text);
    CHECK(!strcmp(text[0], "GDSC CORE 80000002 ON/HW INT2 00000002 OFF/HW") && !strcmp(text[1], "MODE 4 GDSC ONLY CHG 3 T 37s"));
    native_dpu_power_rows(false, 0, 0, 0, 1, text);
    CHECK(!strcmp(text[0], "GDSC not mapped"));
    // The display is off: one row, nothing read.
    CHECK(native_dpu_idle_rows(kProbeCounters, 37, text) == 1 && !strcmp(text[0], "MODE 1 INTF_1 ONLY DISPLAY OFF: NO READS T 37s"));
    CHECK(!strcmp(native_dpu_mode_name(kProbeFull), "FULL+SMMU") && !strcmp(native_dpu_mode_name(9), "?"));
    // Extremes stay within the 66 characters of a photographed row.
    NativeDpuSafeRowInputs big = {};
    big.common.teMillihertz = ~0ull; big.common.frames = ~0u; big.common.locked = 1; big.common.relocks = ~0u; big.common.flipping = 1;
    big.common.flips = big.common.latches = big.common.latchMaxUs = big.common.latchSumUs = big.common.late = big.common.timeouts = ~0ull;
    big.common.underruns = ~0u; big.autorefresh = ~0u; big.intervals = ~0u; big.teMinMillihertz = big.teMaxMillihertz = ~0ull;
    big.framesMin = big.framesMax = ~0u; big.mode = 0xff; big.underrunsCounted = 1; big.refused = ~0u; big.refusedOffset = ~0u;
    big.seconds = ~0u;
    const unsigned count = native_dpu_counter_rows(big, text);
    CHECK(count == 4);
    for (unsigned r = 0; r < count; ++r) CHECK(strlen(text[r]) <= NATIVE_DPU_ROW_CHARS);
    CHECK(!strncmp(text[0], "TE 99999.99Hz", 13));
    NativeDpuState full = {};
    for (auto &c : full.ctl) c = {~0u, ~0u, ~0u, ~0u, ~0u};
    full.readPipes = 0xffff;
    native_dpu_path_row(full, big, text[0]);
    native_dpu_power_rows(true, ~0u, ~0u, ~0u, ~0u, text + 1);
    native_dpu_idle_rows(~0u, ~0u, text + 3);
    for (unsigned r = 0; r < 4; ++r) CHECK(strlen(text[r]) <= NATIVE_DPU_ROW_CHARS);
}

int main() {
    plans();
    guard();
    ctl0();
    rates();
    rows();
    printf("PASS: display probe safety, %u checks: modes per boot argument, access guard (safe mode, TE lock, flips; the hung snapshot "
           "refused), CTL 0 pipes, per-second rates, rows of every mode\n", checks);
    return 0;
}
