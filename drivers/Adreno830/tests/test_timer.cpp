/* SPDX-License-Identifier: MIT */
/* GetTimestamp reader (ARCHITECTURE 1.3 #5, 4.13.3) over a software counter peer: the GMUCX AO counter at dword
 * 0x1F840/0x1F841 (gmu window +0x47100/+0x47104), Gen8 CP_ALWAYS_ON_COUNTER 0x8E7/0x8E8 for the S8 comparison, and the
 * high-low-high read of KGSL gen8_read_alwayson. No device access. */
#include "XnuMemoryFixture.h"
#include "NativeAdrenoTimer.h"
#include "A830Regs.h"
#include <cstdio>
#include <vector>
#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::exit(1);}}while(0)
/* A free-running 64-bit counter: `step` ticks pass between two reads. Every read is logged. */
struct Peer {
    uint64_t count=0,step=0;
    unsigned reads=0,fail_read=0;
    bool cp=false;
    std::vector<uint32_t> addresses;
};
static Peer peer;
static int read32(void *cookie,uint32_t address,uint32_t *value)
{
    CHECK(cookie==&peer && value);++peer.reads;
    peer.addresses.push_back(address);
    if(peer.reads==peer.fail_read)return 1;
    const uint64_t now=peer.count;
    peer.count+=peer.step;
    const uint32_t low=peer.cp?NativeAdrenoTimer::cpLowRegister():NativeAdrenoTimer::gmuLowOffset();
    const uint32_t high=peer.cp?NativeAdrenoTimer::cpHighRegister():NativeAdrenoTimer::gmuHighOffset();
    if(address==low) {*value=uint32_t(now);return 0;}
    if(address==high) {*value=uint32_t(now>>32);return 0;}
    CHECK(0);return 1;
}
static NativeAdrenoTimeIo io{&peer,read32};
int main()
{
    unsigned reads=0,refusals=0;
    /* The offsets are the A830 ones: GMU AO counter in the gmu window, CP_ALWAYS_ON_COUNTER in GPU dwords. */
    {
        CHECK(NativeAdrenoTimer::gmuLowOffset()==0x47100 && NativeAdrenoTimer::gmuHighOffset()==0x47104);
        CHECK(NativeAdrenoTimer::gmuLowOffset()==A830_GMU_REG_BYTE(A8XX_GMUCX_AO_COUNTER_LO));
        CHECK(NativeAdrenoTimer::gmuHighOffset()==A830_GMU_REG_BYTE(A8XX_GMUCX_AO_COUNTER_HI));
        CHECK(A830_GMU_PHYS+NativeAdrenoTimer::gmuLowOffset()==0x3D7E100);
        CHECK(NativeAdrenoTimer::cpLowRegister()==A8XX_CP_ALWAYS_ON_COUNTER && NativeAdrenoTimer::cpHighRegister()==0x8E8);
        CHECK(NativeAdrenoTimer::frequencyHz()==19200000);
    }
    /* A stable counter reads in three accesses, high-low-high, through both address spaces. */
    {
        peer=Peer{};peer.count=0x0123456789abcdefull;
        NativeAdrenoTimer timer;
        CHECK(!timer.attached() && timer.attach(&io,NativeAdrenoTimeGmu)==kIOReturnSuccess && timer.attached());
        CHECK(timer.source()==NativeAdrenoTimeGmu);
        uint64_t ticks=0;
        CHECK(timer.read(&ticks)==kIOReturnSuccess && ticks==0x0123456789abcdefull);++reads;
        CHECK(peer.reads==3 && peer.addresses[0]==0x47104 && peer.addresses[1]==0x47100 && peer.addresses[2]==0x47104);
        CHECK(timer.detach()==kIOReturnSuccess && timer.detach()==kIOReturnNotReady);
        peer=Peer{};peer.count=0x0123456789abcdefull;peer.cp=true;
        NativeAdrenoTimer cp;
        CHECK(cp.attach(&io,NativeAdrenoTimeCp)==kIOReturnSuccess && cp.source()==NativeAdrenoTimeCp);
        CHECK(cp.read(&ticks)==kIOReturnSuccess && ticks==0x0123456789abcdefull);++reads;
        CHECK(peer.addresses[0]==0x8E8 && peer.addresses[1]==0x8E7 && peer.addresses[2]==0x8E8);
    }
    /* A running counter: the value is between the first and the last access, never torn. */
    {
        NativeAdrenoTimer timer;CHECK(timer.attach(&io,NativeAdrenoTimeGmu)==kIOReturnSuccess);
        for(uint64_t start:{0x00000000fffffff0ull,0x00000007ffffffffull,0x12345678fffffffeull,0x0000000100000000ull}) {
            for(uint64_t step:{0ull,1ull,3ull,0x10ull}) {
                peer=Peer{};peer.count=start;peer.step=step;
                uint64_t ticks=0;
                CHECK(timer.read(&ticks)==kIOReturnSuccess);++reads;
                CHECK(ticks>=start && ticks<=peer.count);
                /* The low word is read again exactly when the high word moved between the two high reads. */
                const bool moved=uint32_t(start>>32)!=uint32_t((start+2*step)>>32);
                CHECK(peer.reads==(moved?4u:3u));
            }
        }
        /* The rollover case: high 7 → 8 while low wrapped; the result pairs 8 with the low word read after it. */
        peer=Peer{};peer.count=0x00000007fffffff0ull;peer.step=0x10;
        uint64_t ticks=0;
        CHECK(timer.read(&ticks)==kIOReturnSuccess && ticks==0x0000000800000020ull);++reads;
    }
    /* Arguments and I/O failures are errors, never fabricated ticks. */
    {
        peer=Peer{};peer.count=1;
        NativeAdrenoTimeIo missing{nullptr,nullptr};
        NativeAdrenoTimer unbound;
        CHECK(unbound.attach(nullptr,NativeAdrenoTimeCp)==kIOReturnBadArgument);++refusals;
        CHECK(unbound.attach(&missing,NativeAdrenoTimeCp)==kIOReturnBadArgument);++refusals;
        CHECK(unbound.attach(&io,7)==kIOReturnBadArgument);++refusals;
        CHECK(unbound.attach(&io,0xffffffffu)==kIOReturnBadArgument);++refusals;
        CHECK(unbound.read(nullptr)==kIOReturnBadArgument);++refusals;
        uint64_t ticks=5;
        CHECK(unbound.read(&ticks)==kIOReturnNotReady && !ticks);++refusals;
        NativeAdrenoTimer timer;CHECK(timer.attach(&io,NativeAdrenoTimeGmu)==kIOReturnSuccess);
        for(unsigned fail=1;fail<=3;++fail) {
            peer=Peer{};peer.count=0x100000000ull;peer.fail_read=fail;ticks=5;
            CHECK(timer.read(&ticks)==kIOReturnIOError && !ticks);++refusals;
        }
        peer=Peer{};peer.count=0xfffffffeull;peer.step=1;peer.fail_read=4;ticks=5;   /* the low re-read after a rollover */
        CHECK(timer.read(&ticks)==kIOReturnIOError && !ticks);++refusals;
        CHECK(timer.attach(&io,NativeAdrenoTimeCp)==kIOReturnBusy);++refusals;
        CHECK(timer.detach()==kIOReturnSuccess && timer.read(&ticks)==kIOReturnNotReady);++refusals;
    }
    std::printf("PASS: GMU AO counter at gmu +0x47100/+0x47104 and CP 0x8E7/0x8E8, %u high-low-high reads (rollovers included), "
                "%u refusals over a software counter peer\n",reads,refusals);
}
