/* SPDX-License-Identifier: MIT */
#ifndef NATIVE_ADRENO_TIMER_H
#define NATIVE_ADRENO_TIMER_H
#include <IOKit/IOTypes.h>
#include <stdint.h>

/* GPU always-on counter reader for the user-visible timestamp (GetTimestamp, ARCHITECTURE 1.3 #5 and 4.13.3). Two sources
 * are distinguished because their address spaces differ:
 *  - GMU window (default): byte offsets inside the bound gmu@3d37000 window. A830 reads the GMUCX AO counter, dword
 *    0x1F840 (low) and 0x1F841 (high) of the GPU space, as KGSL gen8_read_alwayson does: byte offset
 *    (0x1F840 << 2) - (0x03D37000 - 0x03D00000) = 0x47100 for the low word, 0x47104 for the high word (physical
 *    0x3D7E100/0x3D7E104). It is in CX, so it stays readable with GX off (IFPC, slumber with CX kept).
 *  - CP window: dword register indices in the GPU register window, Gen8 CP_ALWAYS_ON_COUNTER at 0x8E7 (high 0x8E8), a GX
 *    register: only under OOB_GPU, used by S8 to compare both counters (ARCHITECTURE 9.2 #5).
 * Both count at 19.2 MHz. The caller binds exactly one already powered window; this object performs no power, reset or
 * window discovery. A read is high, low, high: when the high word moved, the low word is read again and paired with the
 * second high word (a rollover then cannot tear the value). A failed access reports an I/O error and never fabricates
 * ticks. */
enum NativeAdrenoTimeSource { NativeAdrenoTimeGmu = 0, NativeAdrenoTimeCp = 1 };
struct NativeAdrenoTimeIo {
    void *cookie;
    int (*read32)(void *,uint32_t address,uint32_t *value);
};
class NativeAdrenoTimer final {
    NativeAdrenoTimeIo io_{};
    uint32_t source_=NativeAdrenoTimeGmu;
    bool attached_=false;
public:
    NativeAdrenoTimer()=default;
    ~NativeAdrenoTimer()=default;
    NativeAdrenoTimer(const NativeAdrenoTimer &)=delete;
    NativeAdrenoTimer &operator=(const NativeAdrenoTimer &)=delete;
    /* Source is a numeric ABI boundary value and is range-checked. */
    IOReturn attach(const NativeAdrenoTimeIo *,uint32_t source);
    IOReturn detach();
    IOReturn read(uint64_t *ticks) const;
    bool attached() const {return attached_;}
    uint32_t source() const {return source_;}
    static uint32_t gmuLowOffset();      /* 0x47100 */
    static uint32_t gmuHighOffset();     /* 0x47104 */
    static uint32_t cpLowRegister();     /* 0x8E7 */
    static uint32_t cpHighRegister();    /* 0x8E8 */
    static uint64_t frequencyHz() {return 19200000;}
};
#endif
