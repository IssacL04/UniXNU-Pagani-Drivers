/* SPDX-License-Identifier: MIT */
/* Host doubles shared by test_a830powerstate, test_a830recovery and test_a830dcvs: a register model of the GPU, GMU,
 * RSCC and CX_MISC windows (with the acknowledgements and idle states the power code polls), a virtual clock behind
 * A830PowerEnv (sleeps advance it, the mutex checks it is never taken twice), operation tables that log every call and
 * can fail any of them, and recorders for the status block calls (Adreno830Status.cpp is not linked). Include it from
 * exactly one translation unit per test. */
#ifndef A830_POWER_FAKES_H
#define A830_POWER_FAKES_H
#include "XnuMemoryFixture.h"
#include "A830Dcvs.h"
#include "A830PowerState.h"
#include "A830Recovery.h"
#include "A830Regs.h"
#include "Adreno830Status.h"
#include "Sm8750GpuPower.h"
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

static unsigned gChecks;
#define CHECK(x) do {++gChecks;if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::exit(1);}}while(0)

/* ---- status block recorders -------------------------------------------------------------------------------------- */
Adreno830Status adreno830_status;
struct StatusLog {
    unsigned running=0,published=0;
    std::vector<std::vector<uint32_t>> lost,recovering,slumber,dcvs,memory;
    std::vector<uint16_t> residency;
    std::vector<int32_t> temperature;
    std::vector<std::vector<int32_t>> thermal;   /* adreno830_status_thermal: target, trend, flags */
};
static StatusLog gStatus;
void adreno830_status_running(void) {++gStatus.running;adreno830_status.state=A830Done;adreno830_status.stage=A830SRunning;}
void adreno830_status_lost(uint8_t stage,uint16_t step,uint16_t reason,IOReturn ior)
{
    gStatus.lost.push_back({stage,step,reason,uint32_t(ior)});
    adreno830_status.state=A830Lost;adreno830_status.stage=stage;adreno830_status.step=step;adreno830_status.reason=reason;
}
void adreno830_status_recovering(uint8_t stage,uint32_t elapsed_ms)
{
    gStatus.recovering.push_back({stage,elapsed_ms});
    adreno830_status.state=A830Recovering;adreno830_status.stage=stage;
}
void adreno830_status_slumber(uint32_t seconds,uint32_t wakes,uint32_t last_wake_ms)
{
    gStatus.slumber.push_back({seconds,wakes,last_wake_ms});
    adreno830_status.state=A830Slumber;
}
void adreno830_status_sticky(unsigned index,uint32_t value) {if(value && index<4)adreno830_status.sticky[index]=value;}
void adreno830_status_dcvs(uint32_t mhz,uint32_t busy_permille,uint32_t gmu_level,uint32_t bus_level,uint32_t min_mhz,
                           uint32_t max_mhz,unsigned first,unsigned count,const uint16_t *residency_permille)
{
    gStatus.dcvs.push_back({mhz,busy_permille,gmu_level,bus_level,min_mhz,max_mhz,first,count});
    gStatus.residency.assign(residency_permille,residency_permille+count);
    adreno830_status.mhz=mhz;adreno830_status.busy_permille=busy_permille;
    adreno830_status.gmu_level=gmu_level;adreno830_status.bus_level=bus_level;
}
void adreno830_status_memory(uint32_t pool_mib,uint32_t used_mib,uint32_t imports,uint32_t coherent)
{
    gStatus.memory.push_back({pool_mib,used_mib,imports,coherent});
}
void adreno830_status_temperature(int32_t deci) {gStatus.temperature.push_back(deci);}
void adreno830_status_thermal(int32_t target_deci,uint32_t trend,uint32_t flags)
{
    gStatus.thermal.push_back({target_deci,int32_t(trend),int32_t(flags)});
}

/* ---- registers --------------------------------------------------------------------------------------------------- */
struct Access {bool write;uint8_t window;uint32_t offset,value;};
static constexpr uint32_t G(uint32_t dword) {return A830_GPU_REG_BYTE(dword);}
static constexpr uint32_t M(uint32_t dword) {return A830_GMU_REG_BYTE(dword);}
static constexpr uint32_t R(uint32_t dword) {return A830_RSCC_REG_BYTE(dword);}
static constexpr uint32_t X(uint32_t dword) {return A830_CX_MISC_REG_BYTE(dword);}

struct FakeHw {
    std::map<std::pair<uint8_t,uint32_t>,uint32_t> regs;
    std::vector<Access> log;
    unsigned barriers=0;
    uint64_t busyTicks=0;            /* GMU XO power counter 0 */
    bool gbifAck=true,rbbmGbifAck=true;
    std::function<bool(uint8_t,uint32_t,uint32_t *)> readHook;   /* return true to answer a read */
    void reset()
    {
        regs.clear();log.clear();barriers=0;busyTicks=0;gbifAck=rbbmGbifAck=true;readHook=nullptr;
        for(uint32_t tcs=0;tcs<A8XX_RSCC_TCS_COUNT;++tcs)regs[{A830PowerWinRscc,R(A8XX_RSCC_TCS_DRV0_STATUS(tcs))}]=1;
    }
    uint32_t &at(uint8_t window,uint32_t offset) {return regs[{window,offset}];}
    uint32_t read(uint8_t window,uint32_t offset)
    {
        uint32_t value=0;
        if(readHook && readHook(window,offset,&value)) {log.push_back({false,window,offset,value});return value;}
        if(window==A830PowerWinGmu && offset==M(A8XX_GMUCX_POWER_COUNTER_XOCLK_L_0))value=uint32_t(busyTicks);
        else if(window==A830PowerWinGmu && offset==M(A8XX_GMUCX_POWER_COUNTER_XOCLK_H_0))value=uint32_t(busyTicks>>32);
        else if(window==A830PowerWinGpu && offset==G(A8XX_GBIF_HALT_ACK))value=gbifAck?at(window,G(A8XX_GBIF_HALT)):0;
        else if(window==A830PowerWinGpu && offset==G(A8XX_RBBM_GBIF_HALT_ACK))value=rbbmGbifAck?at(window,G(A8XX_RBBM_GBIF_HALT)):0;
        else value=at(window,offset);
        log.push_back({false,window,offset,value});
        return value;
    }
    void write(uint8_t window,uint32_t offset,uint32_t value) {at(window,offset)=value;log.push_back({true,window,offset,value});}
    /* Index of the first write of `value` to (window, offset) at or after `from`; -1 when none. */
    long writeAt(uint8_t window,uint32_t offset,uint32_t value,size_t from=0) const
    {
        for(size_t i=from;i<log.size();++i)
            if(log[i].write && log[i].window==window && log[i].offset==offset && log[i].value==value)return long(i);
        return -1;
    }
    bool wrote(uint8_t window,uint32_t offset) const
    {
        for(const auto &a:log)if(a.write && a.window==window && a.offset==offset)return true;
        return false;
    }
    bool readAny(uint8_t window,uint32_t offset) const
    {
        for(const auto &a:log)if(!a.write && a.window==window && a.offset==offset)return true;
        return false;
    }
    /* No register of the GPU (GX) window was read or written since log index `from`. */
    bool gpuQuietSince(size_t from) const
    {
        for(size_t i=from;i<log.size();++i)if(log[i].window==A830PowerWinGpu)return false;
        return true;
    }
};
static FakeHw gHw;
static uint32_t ioRead(void *,uint8_t window,uint32_t offset) {return gHw.read(window,offset);}
static void ioWrite(void *,uint8_t window,uint32_t offset,uint32_t value) {gHw.write(window,offset,value);}
static void ioBarrier(void *) {++gHw.barriers;}
static const A830PowerIo gIo={nullptr,ioRead,ioWrite,ioBarrier};

/* ---- clock, mutex, thread event ---------------------------------------------------------------------------------- */
struct FakeEnv {uint64_t now=1000000000ull;int locked=0;unsigned locks=0,wakes=0,waits=0,threads=0;uint64_t slept=0;};
static FakeEnv gEnv;
static uint64_t envNow(void *) {return gEnv.now;}
static void envSleep(void *,uint32_t us) {gEnv.now+=uint64_t(us)*1000;gEnv.slept+=us;}
static void envDelay(void *,uint32_t us) {gEnv.now+=uint64_t(us)*1000;}
static void envLock(void *) {if(gEnv.locked){std::fprintf(stderr,"power mutex taken twice\n");std::exit(1);}gEnv.locked=1;++gEnv.locks;}
static void envUnlock(void *) {if(!gEnv.locked){std::fprintf(stderr,"power mutex released twice\n");std::exit(1);}gEnv.locked=0;}
static void envWait(void *,uint64_t deadline) {++gEnv.waits;if(deadline>gEnv.now)gEnv.now=deadline;}
static void envWake(void *) {++gEnv.wakes;}
static IOReturn envThread(void *,void (*)(void *),void *) {++gEnv.threads;return kIOReturnSuccess;}
static const A830PowerEnv gPowerEnv={nullptr,envNow,envSleep,envDelay,envLock,envUnlock,envWait,envWake,nullptr};
static const A830PowerEnv gThreadEnv={nullptr,envNow,envSleep,envDelay,envLock,envUnlock,envWait,envWake,envThread};

/* ---- operations of the other modules ----------------------------------------------------------------------------- */
struct FakeOps {
    std::vector<std::string> calls;
    std::map<std::string,IOReturn> fail;          /* op name -> result to return (once per entry, unless sticky) */
    std::map<std::string,bool> sticky;
    uint32_t lastSubmitted=0,retired=0;
    uint32_t warnings=0;                          /* cxPowerOff */
    uint16_t failStep=0;                          /* *step reported by a failing gmuStart/cpStart/secureResume */
    A830GmuStartArgs lastArgs{};
    uint32_t fence=0,jobs=0;
    std::vector<uint8_t> gates,gateCauses;   /* recovery gate: each gate and the fault cause it came with */
    std::vector<std::pair<uint8_t,uint32_t>> votes;   /* perfVote level, bw */
    std::vector<uint32_t> hostDdr;
    unsigned polls=0,memoryRows=0,queueRows=0;
    std::vector<bool> queueAwake;               /* queueRow: was the GPU active (A830PowerActive) at each call */
    std::vector<std::pair<size_t,size_t>> queueOrder;   /* queueRow: memory and DCVS rows already stored at each call */
    int oobDepth=0;
    std::function<void(const std::string &)> during;  /* runs inside every op call */
    void reset() {*this=FakeOps();}
    IOReturn hit(const std::string &name)
    {
        calls.push_back(name);
        if(during)during(name);
        auto f=fail.find(name);
        if(f==fail.end())return kIOReturnSuccess;
        const IOReturn result=f->second;
        if(!sticky[name])fail.erase(f);
        return result;
    }
    size_t index(const std::string &name,size_t from=0) const
    {
        for(size_t i=from;i<calls.size();++i)if(calls[i]==name)return i;
        return size_t(-1);
    }
    unsigned count(const std::string &name) const
    {
        unsigned n=0;
        for(const auto &c:calls)if(c==name)++n;
        return n;
    }
};
static FakeOps gOps;
static IOReturn opPowerOn(void *) {return gOps.hit("cxPowerOn");}
static IOReturn opPowerOff(void *,uint32_t *warnings) {*warnings=gOps.warnings;return gOps.hit("cxPowerOff");}
static IOReturn opSmmu(void *) {return gOps.hit("smmuRestore");}
static void opTimer(void *) {gOps.hit("cxTimerSeed");}
static IOReturn opGmuStart(void *,const A830GmuStartArgs *args,uint16_t *step)
{
    gOps.lastArgs=*args;
    const IOReturn r=gOps.hit("gmuStart");
    if(r)*step=gOps.failStep;
    return r;
}
static IOReturn opForceOff(void *) {return gOps.hit("gmuForceOff");}
static IOReturn opRscc(void *) {return gOps.hit("rsccSleep");}
static IOReturn opOobSet(void *)
{
    const IOReturn r=gOps.hit("oobSet");
    if(!r)++gOps.oobDepth;
    return r;
}
static void opOobClear(void *) {gOps.hit("oobClear");--gOps.oobDepth;}
static IOReturn opHfi(void *,uint8_t id,const uint32_t *payload,uint8_t dwords)
{
    std::string name="hfi:"+std::to_string(id);
    for(unsigned i=0;i<dwords;++i) {char word[16];std::snprintf(word,sizeof word,":%08x",payload[i]);name+=word;}
    return gOps.hit(name);
}
static IOReturn opCpStart(void *,uint32_t fence,uint16_t *step)
{
    gOps.fence=fence;
    const IOReturn r=gOps.hit("cpStart");
    if(r)*step=gOps.failStep;
    return r;
}
static IOReturn opSecure(void *,uint16_t *step)
{
    const IOReturn r=gOps.hit("secureResume");
    if(r)*step=gOps.failStep;
    return r;
}
static void opIrq(void *,uint32_t lines,bool enable)
{
    char name[32];std::snprintf(name,sizeof name,"irq:%02x:%s",lines,enable?"on":"off");
    gOps.hit(name);
}
static uint32_t opLastSubmitted(void *) {return gOps.lastSubmitted;}
static uint32_t opRetired(void *) {return gOps.retired;}
static const A830PowerOps gPowerOps={nullptr,opPowerOn,opPowerOff,opSmmu,opTimer,opGmuStart,opForceOff,opRscc,opOobSet,
    opOobClear,opHfi,opCpStart,opSecure,opIrq,opLastSubmitted,opRetired};

/* recovery */
static uint32_t opFailInflight(void *) {gOps.hit("failInflight");return gOps.jobs;}
static void opGate(void *,uint8_t gate,uint8_t cause)
{
    gOps.gates.push_back(gate);gOps.gateCauses.push_back(cause);gOps.hit("gate:"+std::to_string(gate));
}
static void opFenceSignal(void *) {gOps.hit("fenceSignal");}
static void opIrqRestore(void *) {gOps.hit("irqRestore");}
static std::map<uint32_t,uint32_t> gPipeRegs;   /* BR pipe registers */
static IOReturn opReadPipe(void *,uint8_t pipe,uint32_t reg,uint32_t *value)
{
    gOps.calls.push_back("readPipe");
    if(pipe!=A8XX_PIPE_BR)return kIOReturnBadArgument;
    *value=gPipeRegs[reg];
    return kIOReturnSuccess;
}
static void opSnapshot(void *,A830HangSnapshot *s) {gOps.hit("snapshot");s->smmuFsr[0]=0x2;s->smmuFar[0]=0x20001000;}
static const A830RecoveryOps gRecoveryOps={nullptr,opFailInflight,opGate,opFenceSignal,opIrqRestore,opReadPipe,opSnapshot};

/* DCVS */
static IOReturn opVote(void *,uint8_t level,uint32_t bw)
{
    const IOReturn r=gOps.hit("perfVote");
    if(!r)gOps.votes.push_back({level,bw});
    return r;
}
static IOReturn opHostDdr(void *,uint32_t kbps) {gOps.hostDdr.push_back(kbps);return gOps.hit("hostDdr");}
static void opPoll(void *) {++gOps.polls;}
static void opMemory(void *,uint32_t *pool,uint32_t *used,uint32_t *imports,uint32_t *coherent)
{
    ++gOps.memoryRows;*pool=512;*used=96;*imports=7;*coherent=1;
}
/* The ring's queue period (A830Fence::takeQueue + adreno830_status_queue in the orchestrator): counted, with the phase the
 * power state machine was in (the op runs asleep too). */
static A830PowerState *gQueuePower;
static void opQueue(void *)
{
    ++gOps.queueRows;
    gOps.queueAwake.push_back(gQueuePower && gQueuePower->phase()==A830PowerActive);
    gOps.queueOrder.emplace_back(gStatus.memory.size(),gStatus.dcvs.size());
}
static const A830DcvsOps gDcvsOps={nullptr,opVote,opHostDdr,opPoll,opMemory,opQueue};

static void resetAll()
{
    gHw.reset();gEnv=FakeEnv();gOps.reset();gStatus=StatusLog();gPipeRegs.clear();
    adreno830_status=Adreno830Status{};
}

/* The common 11-level table of ARCHITECTURE 4.1.4 and the 12-level DDR table. */
static const Adreno830Level kLevels[11]={
    {160000,0x32,2,2,2,0,0},{222000,0x34,2,2,2,0,0},{342000,0x38,2,3,3,0,0},{389000,0x3c,2,4,6,0,0xc8285ffd},
    {443000,0x40,2,4,6,0,0xc02e5ffd},{525000,0x50,2,4,6,0,0xc02e5ffd},{607000,0x80,4,6,8,0,0xa82e5ffd},
    {660000,0x90,4,6,8,0,0x882c5ffd},{734000,0xc0,6,8,10,0,0xa82a5ffd},{832000,0xe0,7,10,10,0,0x882a5ffd},
    {900000,0x100,7,10,10,0,0x882a5ffd}};
static const uint32_t kDdr[12]={0,781250,2136718,5285156,6074218,6671875,8171875,10687500,12449218,14398437,16500000,18597656};
static const A830DcvsTable kTable={kLevels,11,kDdr,12};

#endif
