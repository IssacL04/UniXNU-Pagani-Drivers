// SPDX-License-Identifier: BSD-2-Clause-Patent
// Display timing without a vblank interrupt (ARCHITECTURE 5.3.3, D0): the panel's tearing-effect (TE) cadence is read from INTF_1's
// read-pointer frame count (INTF_TEAR_INT_COUNT_VAL [31:16], +1 per TE), locked once by dense sampling and then followed by a timer
// that wakes just after each predicted TE. The same register's line count ([15:0], loaded with VSYNC_INIT_VAL at the TE and counting the
// panel's lines; Linux dpu_encoder_phys_cmd tear-check setup) dates the TE at every tick, so the phase follows the panel both ways without
// dense resampling; the frame count alone sees only a TE that comes later than predicted. Pure arithmetic on mach times (ticks) and
// register values: host tested, no hardware here.
// Register access (dpu-safety audit, 2026-10-30): the callers' read()/edge() read only INTF_1 +0x298 (TEAR_INT_COUNT_VAL), inside the
// INTF_1 window that native_dpu_allowed (NativeDpu.h) admits; the TE lock's other accesses (+0x2b0 line count, +0x288/+0x2b4 for
// autorefresh off and on) stay in that window too. Nothing here reaches the MDP top, a CTL, a pipe or the apps SMMU.
#pragma once
#include <stdint.h>

// A locked TE cadence: the edge seen at `anchor` carried frame count `count`; `periodQ16` is the TE period in ticks with 16 fraction
// bits, so predictions stay within a tick over thousands of frames.
struct NativeTeLock {
    uint64_t anchor=0,periodQ16=0;
    uint16_t count=0;
    bool valid=false;
    // Line dating (0: the line count is not usable): ticks per line with 16 fraction bits, and the line count at the TE.
    uint64_t lineQ16=0;
    uint16_t lineInit=0;
    uint64_t period() const {return periodQ16>>16;}
    // The TE before a reading `value` taken at `now`, from its line count; false without line dating or for a count past one frame.
    bool dated(uint64_t now,uint32_t value,uint64_t *te) const {
        if(!lineQ16)return false;
        const uint64_t lines=uint16_t(uint16_t(value)-lineInit),elapsed=(lines*lineQ16)>>16;
        if(elapsed>period() || elapsed>now)return false;
        *te=now-elapsed;
        return true;
    }
    // Whole and fractional period times `frames` (64-bit only: no 128-bit helpers in the kernel). Exact for 2^40 frames.
    uint64_t span(uint64_t frames) const {return (periodQ16>>16)*frames+(((periodQ16&0xffff)*frames)>>16);}
    // The TE `frames` after (or before, negative) the anchor.
    uint64_t at(int64_t frames) const {return frames>=0?anchor+span(uint64_t(frames)):anchor-span(uint64_t(-frames));}
    // The first TE strictly after `now` and its index from the anchor (the expected frame field is count + index).
    uint64_t next(uint64_t now,uint64_t *index) const {
        uint64_t k=0;
        if(now>=anchor && periodQ16) {
            const uint64_t since=now-anchor;
            k=(since<(1ull<<47)?(since<<16)/periodQ16:since/(periodQ16>>16 ? periodQ16>>16 : 1))+1;
        }
        uint64_t t=at((int64_t)k);
        while(t<=now){++k;t=at((int64_t)k);}
        if(index)*index=k;
        return t;
    }
    // TE rate in millihertz for `ticksPerSecond` (the mach timebase, at most 2^35).
    uint64_t millihertz(uint64_t ticksPerSecond) const {return periodQ16?((ticksPerSecond*1000)<<16)/periodQ16:0;}
};

// Dense sampling: each reading of INTF_TEAR_INT_COUNT_VAL with the time it was taken. An edge is a change of the frame field; its time is
// the middle of the interval between the last reading before it and the first after it.
// The line count's rate comes from consecutive readings within a frame; its value at the TE from the first reading after each edge.
struct NativeTeSampler {
    static constexpr unsigned kStarts=32;
    uint64_t firstEdge=0,lastEdge=0,previous=0;
    uint16_t firstCount=0,lastCount=0,previousCount=0,previousLine=0;
    unsigned edges=0,samples=0,backwards=0;
    uint64_t lineTicks=0,lineSteps=0;                  // sums over readings within one frame
    uint64_t startAge[kStarts]={};uint16_t startLine[kStarts]={};unsigned starts=0;
    void sample(uint64_t now,uint32_t value) {
        const uint16_t frame=uint16_t(value>>16),line=uint16_t(value);
        if(samples++) {
            if(frame!=previousCount) {
                const uint64_t edge=previous+(now-previous)/2;
                if(!edges){firstEdge=edge;firstCount=frame;}
                lastEdge=edge;lastCount=frame;++edges;
                if(starts<kStarts){startAge[starts]=now-edge;startLine[starts]=line;++starts;}
            } else if(line>previousLine){lineTicks+=now-previous;lineSteps+=uint16_t(line-previousLine);}
            else if(line<previousLine)++backwards;
        }
        previous=now;previousCount=frame;previousLine=line;
    }
    // At least minEdges edges, a forward count and a plausible period (2.5 to 50 ms at `ticksPerSecond`). Line dating only when the count
    // ran forward within frames, gives 1000 to 20000 lines per frame and agrees on its TE value to within 32 lines.
    bool lock(NativeTeLock *out,uint64_t ticksPerSecond,unsigned minEdges=3) const {
        if(edges<minEdges || edges<2)return false;
        const uint16_t frames=uint16_t(lastCount-firstCount);
        if(!frames || frames>=0x8000 || lastEdge<=firstEdge)return false;
        const uint64_t q=((lastEdge-firstEdge)<<16)/frames;
        if((q>>16)<ticksPerSecond/400 || (q>>16)>ticksPerSecond/20)return false;
        out->anchor=lastEdge;out->periodQ16=q;out->count=lastCount;out->valid=true;
        out->lineQ16=0;out->lineInit=0;
        if(lineSteps && !backwards && starts>=2) {
            const uint64_t lineQ16=(lineTicks<<16)/lineSteps,lines=lineQ16?q/lineQ16:0;
            if(lines>=1000 && lines<=20000) {
                int64_t sum=0,lowest=0x7fffffffffffffffll,highest=-0x7fffffffffffffffll-1;
                for(unsigned i=0;i<starts;++i) {
                    const int64_t init=int64_t(startLine[i])-int64_t((startAge[i]<<16)/lineQ16);
                    sum+=init;if(init<lowest)lowest=init;if(init>highest)highest=init;
                }
                if(highest-lowest<=32){out->lineQ16=lineQ16;out->lineInit=uint16_t(int64_t(sum/int64_t(starts)));}
            }
        }
        return true;
    }
};

// A tick read the frame field `value` where `expected` was due. 0: on time; > 0: the tick came late by that many TEs (the cadence is
// still right, the caller skips ahead); < 0: the TE has not happened yet (the phase drifted: relock).
static inline int native_te_check(uint16_t expected,uint32_t value) {return (int)(int16_t)(uint16_t(value>>16)-expected);}

// A relock saw an edge at `edgeTime` with frame field `count`: the anchor moves there and, over the long baseline from the old anchor,
// the period is refined (only when the frame difference is unambiguous and the new period is within 2% of the old one).
static inline void native_te_relock(NativeTeLock *lock,uint64_t edgeTime,uint16_t count) {
    if(lock->valid && edgeTime>lock->anchor && edgeTime-lock->anchor<(1ull<<47)) {
        const uint16_t frames=uint16_t(count-lock->count);
        if(frames && frames<0x8000) {
            const uint64_t q=((edgeTime-lock->anchor)<<16)/frames;
            if(q*50>lock->periodQ16*49 && q*50<lock->periodQ16*51)lock->periodQ16=q;
        }
    }
    lock->anchor=edgeTime;lock->count=count;
}

// The refresh class reported to the render server: 120, 90 or 60 when the measured rate is within 10% of one, else the measured rate
// itself (24..240); 0 for none.
static inline unsigned native_refresh_class(uint64_t millihertz) {
    static const unsigned classes[]={120,90,60};
    for(unsigned c: classes)if(millihertz*10>=(uint64_t)c*9000 && millihertz*10<=(uint64_t)c*11000)return c;
    const uint64_t hz=(millihertz+500)/1000;
    return hz>=24 && hz<=240?unsigned(hz):0;
}
// IOMFBDisplayRefresh words are intervals in 32.32 seconds.
static inline uint64_t native_refresh_interval(unsigned hz) {return hz?(1ull<<32)/hz:0;}

// Boot-arg sheng-vsync. A rate, 24 to 240: the tick is on, at pagani-vsync (default 120), as it always was. A period in microseconds, one frame of those rates:
// the tick is on, at that period to the microsecond. For a panel whose TE is no round rate: the 13T's first TE reading is 8293-8305 us where 120 Hz is 8333, and
// a tick that much slower repeats a frame about every 2 s, where no counter of the log can see it (the panel's own period: panel: a1 te ... mean N ns). The display
// still reports the refresh class, 120 for that period, as a TE lock does: the interval the render server gets with each tick is the period itself. One
// value of one argument because the command line is full: sheng-vsync=8299 is one byte more than =120, a pagani-vsync-us=8299 would be 21. Anything else: no tick.
static constexpr unsigned kVsyncPeriodMinUs=4167,kVsyncPeriodMaxUs=41667;
static inline unsigned native_vsync_period_us(unsigned value) {return value>=kVsyncPeriodMinUs && value<=kVsyncPeriodMaxUs?value:0;}
static inline bool native_vsync_on(unsigned value) {return (value>=24 && value<=240) || native_vsync_period_us(value);}
// The tick's period in ns: sheng-vsync's period, or a frame of `hz`.
static inline uint64_t native_vsync_ns(unsigned value,unsigned hz) {const unsigned us=native_vsync_period_us(value);return us?uint64_t(us)*1000:1000000000ull/hz;}
// The refresh class of sheng-vsync's period, 0 for a rate: that comes from pagani-vsync.
static inline unsigned native_vsync_class(unsigned value) {const unsigned us=native_vsync_period_us(value);return us?native_refresh_class(1000000000ull/us):0;}

// Software cadence (no TE lock): the deadline of the tick after `previous` at `interval`, and the timestamp to report for a tick that ran
// at `now`: the deadline itself unless the tick is a whole interval or more late (then the cadence restarts at now).
static inline uint64_t native_vsync_stamp(uint64_t deadline,uint64_t now,uint64_t interval) {
    return deadline<=now && now-deadline<interval?deadline:now;
}

// A frame that arrived at `arrival` and reached the scanout at `done` aimed at the first TE after its arrival. Late by how much (0: on
// time). `te` is any TE of the cadence and `period` its length (with no lock: the software tick).
static inline uint64_t native_frame_late(uint64_t te,uint64_t period,uint64_t arrival,uint64_t done) {
    if(!period || done<=arrival)return 0;
    uint64_t target;
    if(arrival>=te)target=te+((arrival-te)/period+1)*period;
    else {const uint64_t back=(te-arrival)/period;target=te-back*period;if(target<=arrival)target+=period;}
    return done>target?done-target:0;
}

// One tick of the TE-locked cadence at `now` (woken just after the TE `*index` predicted): the time of the TE it follows. `read()` returns
// INTF_TEAR_INT_COUNT_VAL; `edge(us, &time, &count)` samples densely for at most `us` and reports the next edge. On time: the predicted TE.
// Late by whole TEs: the latest one (the index skips ahead). Early (the TE has not come, the phase drifted): relock on the next edge
// (*relocked set). No edge within relockUs: the TE is gone, lock->valid is cleared and `now` is returned.
template <class Read, class Edge>
static inline uint64_t native_te_tick(NativeTeLock *lock, uint64_t *index, uint64_t now, Read read, Edge edge, unsigned relockUs,
                                      bool *relocked) {
    *relocked = false;
    const uint32_t value = read();
    const int d = native_te_check(uint16_t(lock->count + *index), value);
    if (d >= 0 && d < 0x4000) {
        *index += uint64_t(d);
        const uint64_t predicted = lock->at(int64_t(*index));
        // Line dating: the TE as the panel's read pointer saw it. More than 20 us off the prediction (about 384 ticks at 19.2 MHz;
        // period/400 in general) moves the anchor there, which also refines the period over the baseline since the last move.
        uint64_t dated = 0;
        if (lock->dated(now, value, &dated)) {
            const uint64_t off = dated > predicted ? dated - predicted : predicted - dated;
            if (off > lock->period() / 4) return predicted;          // not this TE: keep the frame count's answer
            if (off > lock->period() / 400) {native_te_relock(lock, dated, uint16_t(lock->count + *index)); *index = 0;}
            return dated;
        }
        return predicted;
    }
    uint64_t time = 0; uint16_t count = 0;
    if (edge(relockUs, &time, &count)) {native_te_relock(lock, time, count); *index = 0; *relocked = true; return time;}
    lock->valid = false;
    return now;
}

// The whole TE-locked tick: the TE to report (never later than the time it is reported at) and, while the lock holds, the next wake-up
// `lead` after the following TE (*index then names that TE). `clock()` is read again after a relock, which samples for up to relockUs.
template <class Clock, class Read, class Edge>
static inline uint64_t native_te_cadence(NativeTeLock *lock, uint64_t *index, Clock clock, Read read, Edge edge, unsigned relockUs,
                                         uint64_t lead, uint64_t *wake, uint64_t *reportedAt, bool *relocked) {
    uint64_t now = clock();
    uint64_t stamp = native_te_tick(lock, index, now, read, edge, relockUs, relocked);
    if (*relocked || !lock->valid) now = clock();
    if (stamp > now) stamp = now;
    if (lock->valid) *wake = lock->next(now, index) + lead;
    *reportedAt = now;
    return stamp;
}

// What ends an offered (GPU) or copied (CPU, outside the gate) frame. The swap may have retired already (early retirement): then the end
// only releases the next frame; otherwise a good end retires it now. A failed GPU copy makes the next attempt use the CPU (bypass); a
// failed CPU copy leaves the swap queued for the next kick.
enum NativeSwapAction : uint8_t { kSwapReleaseNext, kSwapRetire, kSwapFallback, kSwapRetry, kSwapStopped };
struct NativeSwapEnd { NativeSwapAction action; bool reportSuccess, earlyFailure, bypassGpu; };
static inline NativeSwapEnd native_gpu_swap_end(bool retired, int status) {
    if (retired) return {kSwapReleaseNext, status == 0, status != 0, status != 0};
    if (status == 0) return {kSwapRetire, true, false, false};
    return {kSwapFallback, false, false, true};
}
static inline NativeSwapEnd native_copy_end(bool retired, bool ok, bool stopping) {
    if (stopping) return {kSwapStopped, ok, false, false};
    if (retired) return {kSwapReleaseNext, ok, false, false};
    return {ok ? kSwapRetire : kSwapRetry, ok, false, false};
}
