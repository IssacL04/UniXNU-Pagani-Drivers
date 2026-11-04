// SPDX-License-Identifier: BSD-2-Clause-Patent
// host-test: drivers/NativeMobileDisplay/Test/scanout-flip.cpp drivers/NativeMobileDisplay/NativeScanoutFlip.h drivers/NativeMobileDisplay/NativeDpu.h drivers/NativeMobileDisplay/NativeDpuRows.h
// Host test of the page-flip side of the display path (ARCHITECTURE 5.3.4 D2, M12) and the display probe's rows (5.3.3, M12): the
// triple-buffer state machine (targets never scanned or pending, mailbox replacement and limbo, waiting for a latch, the way back to buffer
// 0), recognizing the pipes that fetch the scanout from a snapshot (multirect rect 1, the CTL that flushes them), the flip's register writes
// (flush last), the MDP stream route lookup, and the probe rows (at most 66 characters, every variant).
#include "../NativeScanoutFlip.h"
#include "../NativeDpuRows.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

using namespace nativedpu;

static NativeDpuState bootLoaderState() {
    // What the boot loader leaves: VIG0 and VIG1 scan the left and right halves of B0 (rect 1 of VIG1 too), DMA2 an unrelated buffer,
    // CTL 2 names VIG0/VIG1 active and drives INTF_1.
    NativeDpuState s = {};
    s.valid = 1;
    s.autorefresh = 0x80000001;
    s.pipe[0] = {0x0a5004c0, (uint32_t)kSplashBase, 0, 0x1300, 0x000237ff, 0, 0};
    s.pipe[1] = {0x0a5004c0, (uint32_t)kSplashBase + 0x980, (uint32_t)kSplashBase + 0x6000, 0x1300, 0x000237ff, 0, 3};
    s.pipe[6] = {0x00100010, 0x9f000000, 0, 0x40, 0x000237ff, 0, 0};
    s.ctl[2] = {0x10, 0, kCtlIntf1, kPipeActive[0] | kPipeActive[1], 0x1};
    s.ctl[0] = {0, 0, 0, kPipeActive[6], 0};
    return s;
}

int main() {
    unsigned checks = 0;
    {   // Off: every frame goes to buffer 0.
        NativeScanoutFlip f;
        assert(!f.on() && f.take() == 0 && f.atHome());
        f.enable(5);
        assert(!f.on());
        checks++;
    }
    {   // Three buffers: the GPU never gets the front or the pending buffer; latches free the old front.
        NativeScanoutFlip f; f.enable(3);
        assert(f.on() && f.front == 0);
        uint8_t a = f.take();
        assert(a == 1 && f.complete(a) && f.pending == 1 && f.flips == 1);
        uint8_t b = f.take();
        assert(b == 2);                                     // 0 is scanned, 1 waits to latch
        assert(f.take() == NativeScanoutFlip::kNone && f.refused == 1);
        f.latched();
        assert(f.front == 1 && f.pending == NativeScanoutFlip::kNone && f.latches == 1);
        assert(f.complete(b) && f.pending == 2);
        uint8_t c = f.take();
        assert(c == 0);                                     // the old front is free again
        // Mailbox: c completes while 2 has not latched: 2 goes to limbo, nothing is free until a latch.
        assert(f.complete(c) && f.pending == 0 && f.limbo == 2 && f.mailboxes == 1);
        assert(f.take() == NativeScanoutFlip::kNone);
        f.latched();
        assert(f.front == 0 && f.limbo == NativeScanoutFlip::kNone);
        uint8_t d = f.take(), e = f.take();
        assert(d == 1 && e == 2);                           // two free buffers after the latch
        f.abandon(e);
        assert(!f.complete(e) && f.take() == 2);            // a dropped frame frees its buffer; completing it again is refused
        f.abandon(2);
        assert(!f.complete(9) && f.complete(d));
        checks += 2;
    }
    {   // The CPU path: back to buffer 0 (flipping stays on); a latch timeout or failed copy resets to one buffer.
        NativeScanoutFlip f; f.enable(3);
        uint8_t a = f.take(); f.complete(a);
        f.home();
        assert(f.atHome() && f.on() && f.take() == 1);
        f.reset();
        assert(!f.on() && f.atHome() && f.take() == 0);
        checks++;
    }
    {   // Pipes fetching B0: VIG0 and VIG1 (rect 1 too), not DMA2; CTL 2 by its active pipes.
        NativeDpuState s = bootLoaderState();
        NativeDpuScanout out;
        assert(native_dpu_scanout(s, kSplashBase, 0xc3f000, &out));
        assert(out.pipes == 0x3 && out.rec1 == 0x2 && out.ctl == 2 && out.flush == (kPipeFlush[0] | kPipeFlush[1]));
        assert(out.offset0[1] == 0x980 && out.offset1[1] == 0x6000);
        // B1's writes: the three addresses then the flush, last.
        NativeDpuWrite w[2 * kPipeCount + 1];
        const unsigned n = native_dpu_flip_writes(out, kSplashBase + 0xc3f000, w, 2 * kPipeCount + 1);
        assert(n == 4);
        assert(w[0].offset == kPipeBase[0] + kSrc0Addr && w[0].value == uint32_t(kSplashBase + 0xc3f000));
        assert(w[1].offset == kPipeBase[1] + kSrc0Addr && w[1].value == uint32_t(kSplashBase + 0xc3f000 + 0x980));
        assert(w[2].offset == kPipeBase[1] + kSrc1Addr && w[2].value == uint32_t(kSplashBase + 0xc3f000 + 0x6000));
        assert(w[3].offset == kCtl0 + 2 * kCtlStride + kCtlFlush && w[3].value == out.flush);
        assert(!native_dpu_flip_writes(out, kSplashBase, w, 3));        // too small a list: nothing
        assert(!native_dpu_flip_writes(out, 0x100000000ull, w, 9));      // addresses are 32-bit
        // Without pipe-active bits the CTL driving INTF_1 is used; without either, CTL 0.
        s.ctl[2].pipeActive = 0;
        assert(native_dpu_scanout(s, kSplashBase, 0xc3f000, &out) && out.ctl == 2);
        s.ctl[2].intfActive = 0;
        assert(native_dpu_scanout(s, kSplashBase, 0xc3f000, &out) && out.ctl == 0);
        // A translated (IOVA) fetch address, a disabled pipe, or no snapshot: nothing to flip.
        NativeDpuState t = bootLoaderState();
        t.pipe[0].src0 = 0x01000000; t.pipe[1].size = 0;
        assert(!native_dpu_scanout(t, kSplashBase, 0xc3f000, &out) && !out.pipes);
        t = bootLoaderState(); t.valid = 0;
        assert(!native_dpu_scanout(t, kSplashBase, 0xc3f000, &out));
        checks += 3;
    }
    {   // MDP stream routes: SMR valid bit, ID match under the mask.
        const uint32_t smr[6] = {0x00000800, 0x80000801, 0x80030800, 0x80000804, 0x8000a000, 0};
        assert(native_dpu_stream_slot(smr, 6, 0x801) == 1);
        assert(native_dpu_stream_slot(smr, 6, 0x800) == 2 && native_dpu_stream_slot(smr, 6, 0x802) == 2);   // mask 3
        assert(native_dpu_stream_slot(smr, 6, 0x804) == 3 && native_dpu_stream_slot(smr, 6, 0x900) == -1);
        assert(native_dpu_stream_slot(smr, 1, 0x800) == -1);                                                 // invalid entry
        checks++;
    }
    {   // Probe rows: four without flips, five with; each at most 66 characters, also with extreme values.
        NativeDpuState s = bootLoaderState();
        NativeDpuScanout out;
        assert(native_dpu_scanout(s, kSplashBase, 0xc3f000, &out));
        NativeDpuRowInputs in = {};
        in.teMillihertz = 120040; in.frames = 120; in.locked = 1; in.relocks = 2;
        char rows[5][NATIVE_DPU_ROW_BYTES];
        assert(native_dpu_rows(s, &out, in, rows) == 4);
        assert(!strcmp(rows[0], "TE 120.04Hz AR 80000001 FC 120/s LOCK RL 2"));
        assert(!strncmp(rows[1], "CTL2 PIPE 00030000 FLUSH 00000000 INTF 00000002", 47));
        assert(!strcmp(rows[2], "SSPP VIG0 SRC0 fc800000 STR 00001300 FMT 000237ff MR 0 +1"));
        assert(!strcmp(rows[3], "MDP SMMU not read"));
        s.smmuRead = 1;
        for (unsigned i = 0; i < kStreamCount; ++i) {s.stream[i] = {}; s.stream[i].sid = kMdpStreams[i]; s.stream[i].slot = 0xff; s.stream[i].bank = 0xff;}
        assert(native_dpu_rows(s, &out, in, rows) == 4 && !strncmp(rows[3], "MDP SID 800..804 no route", 25));
        s.stream[1] = {0x80000801, 0x00010000, 0, 0, 0, 0, 0, 0, 0x801, 5, 0xff};
        assert(native_dpu_rows(s, &out, in, rows) == 4 && !strcmp(rows[3], "MDP SID 801 S2C 00010000 SMR 80000801 BYPASS"));
        s.stream[1] = {0x80000801, 0x00000007, 0x00000007, 0x00000067, 0x80000000, 0x44ff, 0x2, 0x9a0000000ull, 0x801, 5, 7};
        assert(native_dpu_rows(s, &out, in, rows) == 4 && !strcmp(rows[3], "MDP SID 801 S2C 00000007 CB 7 SCT 00000067 FSR 00000002"));
        in.flipping = 1; in.flips = 1000; in.latches = 990; in.latchMaxUs = 8400; in.latchSumUs = 990 * 4100; in.late = 3; in.underruns = 0;
        assert(native_dpu_rows(s, &out, in, rows) == 5 && !strcmp(rows[4], "FLIP 1000 LAT 8.4/4.1ms MISS 3 UR 0 TO 0"));
        // Without a scanout (nothing fetches the GOP buffer) the first pipe with a size is shown.
        assert(native_dpu_rows(s, nullptr, in, rows) == 5 && !strncmp(rows[2], "SSPP VIG0", 9));
        NativeDpuState empty = {};
        assert(native_dpu_rows(empty, nullptr, in, rows) == 5 && !strcmp(rows[2], "SSPP none fetching"));
        // Extremes stay within the 66 characters of a photographed row.
        in.teMillihertz = ~0ull; in.frames = ~0u; in.relocks = ~0u;
        in.flips = in.latches = in.latchMaxUs = in.latchSumUs = in.late = in.timeouts = ~0ull; in.underruns = ~0u;
        for (auto &c : s.ctl) c = {~0u, ~0u, ~0u, ~0u, ~0u};
        const unsigned count = native_dpu_rows(s, &out, in, rows);
        for (unsigned r = 0; r < count; ++r) assert(strlen(rows[r]) <= NATIVE_DPU_ROW_CHARS);
        checks += 2;
    }
    printf("PASS: scanout flips and display probe rows, %u checks: triple-buffer states, mailbox and limbo, way home, pipes and CTL from "
           "a snapshot, flip writes, stream routes, rows\n", checks);
    return 0;
}
