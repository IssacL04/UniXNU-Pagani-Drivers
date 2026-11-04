// SPDX-License-Identifier: BSD-2-Clause-Patent
// Triple-buffered page flips of the scanout (ARCHITECTURE 5.3.4, D2): the buffer the DPU scans (front), the buffer flushed and waiting to
// latch at the next TE (pending), and free buffers the GPU may write. Only the display's work loop uses it, and only the work loop writes
// the flip registers. A frame that completes while the previous flip has not latched replaces it (mailbox): the newest frame is flushed
// and the replaced buffer waits in limbo until a latch is seen after that flush, because the hardware may have latched it just before.
// Pure state, host tested; NativeLegacyDisplay does the register writes (native_dpu_flip_writes) and the latch polling.
// Register access (dpu-safety audit, 2026-10-30): none here. The flips write only the SRC0/SRC1 addresses of the pipes CTL 0 names and
// CTL 0's flush, and poll that flush; native_dpu_allowed (NativeDpu.h) refuses everything else, and nothing in the flip path reads the
// apps SMMU any more (native_dpu_mdp_fsr returns 0).
#pragma once
#include <stdint.h>

struct NativeScanoutFlip {
    static constexpr uint8_t kNone=0xff;
    uint8_t buffers=1;                 // 1: flips off (everything stays on buffer 0)
    uint8_t front=0,pending=kNone,limbo=kNone;
    uint8_t taken[3]={0,0,0};          // handed to the GPU for a frame and not completed yet
    uint64_t flips=0,latches=0,mailboxes=0,refused=0;

    void enable(uint8_t count) {buffers=count>=2 && count<=3?count:1;front=0;pending=limbo=kNone;taken[0]=taken[1]=taken[2]=0;}
    bool on() const {return buffers>1;}
    bool busy(uint8_t b) const {return b==front || b==pending || b==limbo || taken[b];}
    // A buffer for the next frame: neither scanned, nor waiting to latch, nor in limbo, nor already being written. kNone: wait for a latch.
    uint8_t take() {
        if(!on())return 0;
        for(uint8_t b=0;b<buffers;++b)if(!busy(b)){taken[b]=1;return b;}
        ++refused;return kNone;
    }
    // The frame written into `b` was dropped (copy failed or the offer was refused): the buffer is free again.
    void abandon(uint8_t b) {if(b<3)taken[b]=0;}
    // The frame in `b` is complete: it becomes the pending flip (the caller writes its addresses and the flush). An older pending flip is
    // replaced (mailbox) and goes to limbo. With three buffers limbo is always empty here: front, pending and the buffer just written are
    // all three, so take() handed out nothing while a limbo existed. false: `b` was not handed out.
    bool complete(uint8_t b) {
        if(b>=buffers || !taken[b] || limbo!=kNone)return false;
        taken[b]=0;
        if(pending!=kNone){limbo=pending;++mailboxes;}
        pending=b;++flips;
        return true;
    }
    // The flush bits cleared after the latest flush: the pending buffer is scanned now; limbo and the old front are free.
    void latched() {
        if(pending==kNone)return;
        front=pending;pending=kNone;limbo=kNone;++latches;
    }
    // The CPU path writes buffer 0 only (ARCHITECTURE 5.3.4): the caller flips back to it and waits for the latch; flipping stays on.
    void home() {front=0;pending=limbo=kNone;}
    bool atHome() const {return front==0 && pending==kNone && limbo==kNone;}
    // Give up flipping (latch timeout, a failed GPU copy): everything returns to buffer 0, which the caller flips back to.
    void reset() {buffers=1;front=0;pending=limbo=kNone;taken[0]=taken[1]=taken[2]=0;}
};
