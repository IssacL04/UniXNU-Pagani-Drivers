/* SPDX-License-Identifier: MIT */
#include "NativeAdrenoTimer.h"
#include <IOKit/IOLib.h>

/* Derivation, not a guess:
 *   KGSL gen8_reg.h / A830Regs.h  GEN8_GMUCX_AO_COUNTER_LO 0x1F840, _HI 0x1F841 (GPU-space dwords)
 *   SM8750 DT                     gpu@3d00000, gmu@3d37000 (the gmu window starts 0x37000 bytes into the GPU space)
 *   byte offset in the gmu window = (dword << 2) - 0x37000 = 0x47100 (low), 0x47104 (high)
 *   Mesa a6xx.xml A8XX            CP_ALWAYS_ON_COUNTER 0x08E7 (64-bit: low 0x8E7, high 0x8E8)
 * The read order is KGSL gen8_read_alwayson: high, low, high; when the two high words differ the low word is read again
 * and paired with the second high word. */
enum : uint32_t {
    TimerGmuLow=0x47100,
    TimerGmuHigh=0x47104,
    TimerCpLow=0x08E7,
    TimerCpHigh=0x08E8,
};
static_assert(TimerGmuLow==(0x1F840u<<2)-0x37000u && TimerGmuHigh==TimerGmuLow+4,"GMUCX AO counter in the gmu window");
uint32_t NativeAdrenoTimer::gmuLowOffset(){return TimerGmuLow;}
uint32_t NativeAdrenoTimer::gmuHighOffset(){return TimerGmuHigh;}
uint32_t NativeAdrenoTimer::cpLowRegister(){return TimerCpLow;}
uint32_t NativeAdrenoTimer::cpHighRegister(){return TimerCpHigh;}
IOReturn NativeAdrenoTimer::attach(const NativeAdrenoTimeIo *io,uint32_t source)
{
    if(attached_)return kIOReturnBusy;
    if(!io || !io->read32)return kIOReturnBadArgument;
    if(source!=NativeAdrenoTimeGmu && source!=NativeAdrenoTimeCp)return kIOReturnBadArgument;
    io_=*io;source_=source;attached_=true;
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoTimer::detach()
{
    if(!attached_)return kIOReturnNotReady;
    attached_=false;io_={};
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoTimer::read(uint64_t *ticks) const
{
    if(!ticks)return kIOReturnBadArgument;*ticks=0;
    if(!attached_)return kIOReturnNotReady;
    const uint32_t low=source_==NativeAdrenoTimeGmu?TimerGmuLow:TimerCpLow;
    const uint32_t high=source_==NativeAdrenoTimeGmu?TimerGmuHigh:TimerCpHigh;
    uint32_t first=0,low_word=0,second=0;
    if(io_.read32(io_.cookie,high,&first) || io_.read32(io_.cookie,low,&low_word) || io_.read32(io_.cookie,high,&second))
        return kIOReturnIOError;
    /* The low word wrapped between the two high reads: it belongs to the second high word only if read after it. */
    if(first!=second && io_.read32(io_.cookie,low,&low_word))return kIOReturnIOError;
    *ticks=(uint64_t(second)<<32)|low_word;
    return kIOReturnSuccess;
}
