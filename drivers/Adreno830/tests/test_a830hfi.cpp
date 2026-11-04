/* SPDX-License-Identifier: MIT */
/* A830Hfi against a model of the gen80000 GMU firmware's HFI side (ARCHITECTURE 4.7.5-4.7.6, 4.16, 4.17): the firmware reads
 * the queue table it was given, consumes the cmd queue on each doorbell (checking the 0xFAFAFAFA padding), and answers
 * after a scripted number of 100 us sleeps with v5 or classic ACKs, stale ACKs, RECORD-bit echoes, firmware requests
 * (ERR, DEBUG, MEM_ALLOC, CNTR_REGISTER, unknown, oversized), dbg-queue traffic, NMI, CM3 fault and silence. A fake
 * clock makes the 1 s timeout exact. Every host-owned index (cmd write, msg read, dbg read) must only move while the HFI
 * mutex is held. All of that in the design profile; then the annibale profile of A830Profile.h (kernel-sequences 5.2-5.5,
 * 6.1, section 14 rows 2, 24, 26; field-notes 9-1): its 4-entry perf table, no M7, M9 {1, 1, 2}, strict FEATURE_CTRL and
 * short-ACK refusal, the MSGQ|DBGQ poll clear, bus-only votes, the major-version check and the 3 s late drain.
 * host-test: A830Hfi.cpp A830Votes.cpp platform/sm8750/Sm8750Rpmh.c platform/sm8750/Sm8750SocInfo.c */
#include "A830Hfi.h"
#include "A830Profile.h"
#include "Adreno830Status.h"
#include "Sm8750GpuLevels.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>

#define CHECK(x) do {if(!(x)) {std::fprintf(stderr,"%s:%d: CHECK failed: %s\n",__FILE__,__LINE__,#x);std::abort();} ++checks;} while(0)

static unsigned checks;
using Words=std::vector<uint32_t>;

constexpr uint64_t Iova=A830_GMU_VA_HFI;
alignas(64) static uint32_t sharedMem[A830_GMU_HFI_BYTES/4];
static volatile uint32_t *const shm=sharedMem;

/* ---- the firmware model ------------------------------------------------------------------------------------------- */

struct Script {
    bool drop=false;              /* never answer */
    unsigned delay=2;             /* sleeps before the answer */
    std::vector<Words> before;    /* messages ahead of the ACK in the msg queue */
    std::vector<Words> dbg;       /* messages into the dbg queue with the answer */
    uint32_t status=0;            /* classic: word 2; v5: word 3 */
    uint32_t value=0;             /* v5 word 2 */
    bool record=false;            /* echo the request header with RECORD (bit 19) set */
    bool staleFirst=false;        /* an ACK to another header first */
    uint32_t setInfo=0,setInit=0; /* GMU2HOST_INTR_INFO / CM3_FW_INIT_RESULT bits raised with the answer */
};

struct Pending {std::vector<Words> msg,dbg;uint32_t setInfo,setInit;unsigned countdown;};

struct Firmware {
    uint32_t info=0,init=A830_GMU_FW_INIT_READY,version=A830_GMU_HFI_VERSION;
    bool v5=true,consume=true,faulted=false;
    unsigned doorbells=0,msgqClears=0,paddingErrors=0,sizeErrors=0,sleeps=0,versionReads=0;
    unsigned dbgqClears=0,lateSleeps=0;          /* DBGQ cleared by a poll; 100 ms sleeps of the late drain */
    std::map<uint8_t,uint8_t> ackDwords;         /* answer this id with a truncated ACK of that many dwords */
    uint64_t now=0;
    std::vector<Words> received;
    std::map<uint8_t,Script> scripts;
    std::vector<Pending> pending;
};
static Firmware fw;

struct QueueAt {uint32_t header,ring;};
static QueueAt queueAt(unsigned i)
{
    const uint32_t header=shm[2]+shm[3]*i;
    return {header,(shm[header+A830_HFI_QHDR_START_ADDR]-uint32_t(Iova))/4};
}

static void fwPush(unsigned queue,const Words &message)
{
    const QueueAt q=queueAt(queue);
    uint32_t write=shm[q.header+A830_HFI_QHDR_WRITE_INDEX];
    const uint32_t aligned=(uint32_t(message.size())+3)&~3u;
    for(uint32_t i=0;i<aligned;++i) {
        shm[q.ring+write]=i<message.size()?message[i]:A830_HFI_FILL_WORD;
        write=(write+1)%A830_HFI_QUEUE_DWORDS;
    }
    shm[q.header+A830_HFI_QHDR_WRITE_INDEX]=write;
}

static void emit(const Pending &p)
{
    for(const Words &m:p.msg)fwPush(A830_HFI_QUEUE_ID_MSG,m);
    for(const Words &m:p.dbg)fwPush(A830_HFI_QUEUE_ID_DBG,m);
    if(!p.msg.empty())fw.info|=A830_GMU2HOST_MSGQ;
    if(!p.dbg.empty())fw.info|=A830_GMU2HOST_DBGQ;
    fw.info|=p.setInfo;
    fw.init|=p.setInit;
}

static void respond(const Words &command)
{
    const uint32_t hdr=command[0];
    const uint8_t id=uint8_t(hdr&0xff);
    const Script s=fw.scripts.count(id)?fw.scripts[id]:Script{};
    const bool v5=fw.v5 && (id==A830_HFI_H2F_FEATURE_CTRL || id==A830_HFI_H2F_GET_VALUE || id==A830_HFI_H2F_SET_VALUE);
    const uint32_t seq=A830_HFI_HDR_SEQ(hdr),echoed=hdr|(s.record?A830_HFI_HDR_RECORD:0u);
    Pending p{s.before,s.dbg,s.setInfo,s.setInit,s.delay};
    if(s.staleFirst)p.msg.push_back({A830_HFI_HDR(id,A830_HFI_MSG_ACK,3,seq),hdr^(1u<<20),0});
    if(!s.drop) {
        Words answer=v5?Words{A830_HFI_HDR(id,A830_HFI_MSG_ACK,4,seq),echoed,s.value,s.status}:
                        Words{A830_HFI_HDR(id,A830_HFI_MSG_ACK,3,seq),echoed,s.status};
        if(fw.ackDwords.count(id)) {
            answer.resize(fw.ackDwords[id]);
            answer[0]=A830_HFI_HDR(id,A830_HFI_MSG_ACK,uint32_t(answer.size()),seq);
        }
        p.msg.push_back(answer);
    }
    if(!p.countdown)emit(p);
    else fw.pending.push_back(p);
}

static void fwDoorbell()
{
    ++fw.doorbells;
    if(!fw.consume)return;
    const QueueAt q=queueAt(A830_HFI_QUEUE_ID_CMD);
    uint32_t read=shm[q.header+A830_HFI_QHDR_READ_INDEX];
    const uint32_t write=shm[q.header+A830_HFI_QHDR_WRITE_INDEX];
    while(read!=write) {
        const uint32_t hdr=shm[q.ring+read],size=A830_HFI_HDR_DWORDS(hdr),aligned=(size+3)&~3u;
        if(!size || A830_HFI_HDR_TYPE(hdr)!=A830_HFI_MSG_CMD)++fw.sizeErrors;
        Words message;
        for(uint32_t i=0;i<aligned;++i) {
            const uint32_t word=shm[q.ring+(read+i)%A830_HFI_QUEUE_DWORDS];
            if(i<size)message.push_back(word);
            else if(word!=A830_HFI_FILL_WORD)++fw.paddingErrors;
        }
        read=(read+aligned)%A830_HFI_QUEUE_DWORDS;
        fw.received.push_back(message);
        shm[q.header+A830_HFI_QHDR_READ_INDEX]=read;
        respond(message);
    }
}

static void fwTick()
{
    std::vector<Pending> later;
    for(Pending &p:fw.pending) {
        if(--p.countdown==0)emit(p);
        else later.push_back(p);
    }
    fw.pending.swap(later);
}

/* ---- host io and the lock discipline -------------------------------------------------------------------------------- */

struct Discipline {bool held=false;unsigned locks=0,unlocks=0,violations=0;uint32_t snapshot[3]={};};
static Discipline lockState;
static bool tracking;   /* the table exists: host-owned indexes can be watched */

static void hostIndexes(uint32_t out[3])
{
    out[0]=shm[queueAt(A830_HFI_QUEUE_ID_CMD).header+A830_HFI_QHDR_WRITE_INDEX];
    out[1]=shm[queueAt(A830_HFI_QUEUE_ID_MSG).header+A830_HFI_QHDR_READ_INDEX];
    out[2]=shm[queueAt(A830_HFI_QUEUE_ID_DBG).header+A830_HFI_QHDR_READ_INDEX];
}

static void observe()
{
    if(!tracking)return;
    uint32_t now[3];
    hostIndexes(now);
    if(!lockState.held && std::memcmp(now,lockState.snapshot,sizeof now))++lockState.violations;
    std::memcpy(lockState.snapshot,now,sizeof now);
}

static uint32_t ioRead(void *,uint32_t dword)
{
    observe();
    CHECK(lockState.held);
    switch(dword) {
    case A8XX_GMUCX_GMU2HOST_INTR_INFO:return fw.info;
    case A8XX_GMUCX_CM3_FW_INIT_RESULT:return fw.init;
    case A8XX_GMU_DTCM_HFI_VERSION:++fw.versionReads;return fw.version;
    default:std::fprintf(stderr,"unexpected read of dword %#x\n",dword);std::abort();
    }
}

static void ioWrite(void *,uint32_t dword,uint32_t value)
{
    observe();
    CHECK(lockState.held);
    switch(dword) {
    case A8XX_GMUCX_HOST2GMU_INTR_SET:CHECK(value==A830_HOST2GMU_HFI_DOORBELL);fwDoorbell();break;
    case A8XX_GMUCX_GMU2HOST_INTR_CLR:
        /* The design clears the message bit alone; annibale's poll clears messages and the debug queue (bits 0 and 2). */
        CHECK(value && !(value&~(A830_GMU2HOST_MSGQ|A830_GMU2HOST_DBGQ)) && (adreno830_annibale() || value==A830_GMU2HOST_MSGQ));
        fw.info&=~value;
        if(value&A830_GMU2HOST_MSGQ)++fw.msgqClears;
        if(value&A830_GMU2HOST_DBGQ)++fw.dbgqClears;
        break;
    default:std::fprintf(stderr,"unexpected write of dword %#x\n",dword);std::abort();
    }
}

static void ioSleep(void *,uint32_t microseconds)
{
    observe();
    CHECK(microseconds==A830_POLL_STEP_US || (adreno830_annibale() && microseconds==A830HfiLateDrainStepUs));
    fw.now+=microseconds;
    if(microseconds==A830HfiLateDrainStepUs)++fw.lateSleeps;else ++fw.sleeps;
    fwTick();
}

static uint64_t ioNow(void *) {observe();return fw.now;}
static void ioLock(void *) {observe();CHECK(!lockState.held);lockState.held=true;++lockState.locks;}
static void ioUnlock(void *) {CHECK(lockState.held);observe();lockState.held=false;++lockState.unlocks;}
static bool ioFaulted(void *) {return fw.faulted;}

static const A830HfiIo io={nullptr,ioRead,ioWrite,ioSleep,ioNow,ioLock,ioUnlock,ioFaulted};

/* ---- vote tables from the PKX110 levels and a plausible cmd-db ---------------------------------------------------- */

static A830VoteTables tables,annibaleTables;   /* the same inputs in the two profiles */

static void arc(Sm8750RpmhArc &a,uint32_t addr,const std::vector<uint16_t> &levels)
{
    a={};a.addr=addr;a.present=1;a.count=uint8_t(levels.size());
    for(size_t i=0;i<levels.size();++i)a.vlvl[i]=levels[i];
}

static void makeTables()
{
    Sm8750RpmhResources res={};
    arc(res.gfx,0x30020,{0x0,0x30,0x32,0x34,0x38,0x3c,0x40,0x50,0x80,0x90,0xc0,0xe0,0x100,0x140,0x1a0,0x1c4});
    arc(res.mx,0x30010,{0x0,0x10,0x30,0x40,0x80,0xc0,0x100,0x140,0x180,0x1a0});
    arc(res.cx,0x30000,{0x0,0x10,0x30,0x38,0x40,0x50,0x60,0x80,0x90,0xc0,0xe0,0x100,0x120,0x140,0x180,0x1a0});
    res.sh0={0x50008,4800000,64,1,1,8};
    res.mc0={0x50004,4800000,16,0,1,8};
    res.acv={0x50000,1,1,0,1,8};
    res.cn0={0x5000c,1000,4,2,1,8};
    Adreno830Board board={};
    board.ddr_kbps=Sm8750DtDdrKbps;board.ddr_levels=Sm8750DtDdrLevels;
    board.cnoc_levels[0]=0;board.cnoc_levels[1]=100;
    board.gmu_khz[0]=500000;board.gmu_khz[1]=650000;board.gmu_vlvl[0]=0x40;board.gmu_vlvl[1]=0x80;
    Sm8750GpuLevelTable levels;
    CHECK(Sm8750GpuLevelsSelect(Sm8750DtGpuBins,6,A830VotesNoFuse,0,0,&levels)==Sm8750SocInfoOk);
    adreno830_profile_value=A830ProfileDesign;
    CHECK(A830Votes::build(res,board,levels,0,&tables)==kIOReturnSuccess);
    CHECK(tables.perf_dwords==49 && tables.gx_count==12 && tables.acd_levels==8 && tables.acd_mask==0x0FF0);
    adreno830_profile_value=A830ProfileAnnibale;
    CHECK(A830Votes::build(res,board,levels,0,&annibaleTables)==kIOReturnSuccess);
    CHECK(annibaleTables.perf_dwords==25 && annibaleTables.gx_count==4 && annibaleTables.acd_levels==0 && annibaleTables.initial==1);
    adreno830_profile_value=A830ProfileDesign;
}

/* ---- helpers -------------------------------------------------------------------------------------------------------- */

static void freshFirmware()
{
    fw=Firmware{};
    lockState=Discipline{};
    tracking=false;
}

static void boot(A830Hfi &hfi,A830HfiLayout layout=A830HfiLayoutLinux)
{
    freshFirmware();
    std::memset(sharedMem,0x5a,sizeof sharedMem);
    hfi=A830Hfi{};
    hfi.setIo(io);
    CHECK(hfi.init(shm,Iova,layout)==kIOReturnSuccess);
    hostIndexes(lockState.snapshot);
    tracking=true;
}

static Words payloadOf(const uint32_t *message,uint32_t dwords) {return Words(message+1,message+dwords);}

static void expectCommand(size_t index,uint32_t header,const Words &payload)
{
    CHECK(index<fw.received.size());
    const Words &m=fw.received[index];
    CHECK(m.size()==payload.size()+1 && m[0]==header);
    for(size_t i=0;i<payload.size();++i)CHECK(m[i+1]==payload[i]);
}

/* ---- tests ---------------------------------------------------------------------------------------------------------- */

static void testInitLayouts()
{
    A830Hfi hfi;
    boot(hfi);
    CHECK(shm[0]==0 && shm[1]==168 && shm[2]==6 && shm[3]==12 && shm[4]==3 && shm[5]==3);
    const uint32_t types[3]={0xA00,0xA01,0xA02};
    for(unsigned i=0;i<3;++i) {
        const volatile uint32_t *h=shm+6+12*i;
        CHECK(h[0]==1 && h[1]==uint32_t(Iova)+0x1000*(i+1) && h[2]==types[i] && h[3]==1024 && h[4]==0 && h[5]==0);
        CHECK(h[6]==1 && h[7]==1 && h[8]==1 && h[9]==0 && h[10]==0 && h[11]==0);
    }
    for(unsigned i=6+36;i<A830_GMU_HFI_BYTES/4;++i)CHECK(shm[i]==0);   /* the rest cleared */
    CHECK(lockState.locks==lockState.unlocks && lockState.locks>=1);
    /* KGSL layout (the fallback when no first ACK arrives): queues at 848 + i * 4096, size in dwords, dbg priority 40. */
    boot(hfi,A830HfiLayoutKgsl);
    CHECK(hfi.layout()==A830HfiLayoutKgsl && shm[1]==6+17*12 && shm[2]==6 && shm[3]==12 && shm[4]==3 && shm[5]==3);
    const uint32_t kgslTypes[3]={0xA00,0xA01,0x2802};
    for(unsigned i=0;i<3;++i) {
        const volatile uint32_t *h=shm+6+12*i;
        CHECK(h[0]==1 && h[1]==uint32_t(Iova)+848+4096*i && h[2]==kgslTypes[i] && h[3]==1024);
        CHECK(h[6]==0 && h[7]==0 && h[8]==0 && h[10]==0 && h[11]==0);
    }
    /* Arguments. */
    A830Hfi bare;
    CHECK(bare.init(shm,Iova)==kIOReturnBadArgument);   /* no io */
    bare.setIo(io);
    CHECK(bare.init(nullptr,Iova)==kIOReturnBadArgument && bare.init(shm,Iova+4)==kIOReturnBadArgument);
    CHECK(bare.init(shm,0xFFFFF000ull)==kIOReturnBadArgument);
    CHECK(bare.resetQueues()==kIOReturnNotReady);
    uint32_t value;
    CHECK(bare.getValue(1,0,&value)==kIOReturnNotReady && bare.drainDebug()==kIOReturnNotReady);
}

/* P0: M1, M2, M5 {9,0,0}, M6, M7, M8, M9, headers with sequence 1..7 (ARCHITECTURE 4.7.6). */
static void testStartP0(A830HfiLayout layout)
{
    A830Hfi hfi;
    boot(hfi,layout);
    fw.scripts[A830_HFI_H2F_GET_VALUE].value=1;
    CHECK(hfi.start(tables,2,2)==kIOReturnSuccess);
    CHECK(hfi.started() && hfi.abSupported() && hfi.firstAckSeen() && hfi.version()==0x20070001 && fw.versionReads==1);
    CHECK(fw.received.size()==7 && fw.doorbells==7 && !fw.paddingErrors && !fw.sizeErrors);
    expectCommand(0,0x0010310F,payloadOf(tables.perf,49));
    expectCommand(1,0x0020A003,payloadOf(tables.bw,160));
    expectCommand(2,0x0030040B,{9,0,0});
    expectCommand(3,0x0040020E,{0});
    expectCommand(4,0x0050030C,{122,0});
    expectCommand(5,0x0060010A,{});
    expectCommand(6,0x0070041E,{1,2,0xFFFF0002u});
    const unsigned sent[]={1,2,5,6,7,8,9};
    for(unsigned m:sent)CHECK(hfi.status(m)==0);
    CHECK(hfi.status(3)==A830HfiStatusNone && hfi.status(4)==A830HfiStatusNone && hfi.header(1)==0x0010310F &&
          hfi.header(9)==0x0070041E);
    CHECK(hfi.currentLevel()==2 && hfi.currentBus()==2 && hfi.stats().votes==1 && hfi.stats().acks==7);
    CHECK(fw.msgqClears>=1 && (fw.info&A830_GMU2HOST_MSGQ)==0);
    CHECK(lockState.violations==0 && !lockState.held && lockState.locks==lockState.unlocks);
    /* Every message waited about two polls (the firmware answers after two sleeps). */
    CHECK(fw.sleeps==2*7 && fw.now==2*7*A830_POLL_STEP_US);
}

static void testStartVariants()
{
    A830Hfi hfi;
    /* pagani-gpu-hfi bit1: no START in start(); sendStart() after the OOB. */
    boot(hfi);
    A830HfiStartOptions options={A830HfiStartDefault|A830HfiStartDeferStart,2,2,0,0x20070001};
    CHECK(hfi.start(tables,options)==kIOReturnSuccess && fw.received.size()==6 && fw.versionReads==0);
    expectCommand(5,0x0060041E,{1,2,0xFFFF0002u});
    CHECK(hfi.status(8)==A830HfiStatusNone && !hfi.abSupported());   /* the model answered GET_VALUE with 0 */
    CHECK(hfi.sendStart()==kIOReturnSuccess);
    expectCommand(6,0x0070010A,{});
    CHECK(hfi.status(8)==0);
    /* ACD (P3): FEATURE_CTRL{12,1,0} then the 37-dword table, between M2 and M5. */
    boot(hfi);
    options={A830HfiStartAcd,7,6,0,0};
    CHECK(hfi.start(tables,options)==kIOReturnSuccess && fw.received.size()==8);
    expectCommand(2,0x0030040B,{12,1,0});
    expectCommand(3,0x00402507,payloadOf(tables.acd,37));
    CHECK(fw.received[3][2]==0x0FF0 && fw.received[3][1]==1 && fw.received[3][3]==1 && fw.received[3][4]==8);
    CHECK(fw.received[3][5]==0xC8285FFDu && fw.received[3][12]==0x882A5FFDu);
    expectCommand(4,0x0050040B,{9,0,0});
    expectCommand(5,0x0060020E,{0});
    expectCommand(6,0x0070010A,{});   /* no M7 without A830HfiStartQueryAb */
    expectCommand(7,0x0080041E,{1,7,0xFFFF0006u});
    CHECK(hfi.status(3)==0 && hfi.status(7)==A830HfiStatusNone && hfi.currentLevel()==7 && hfi.currentBus()==6);
}

static void testStartM9Order()
{
    A830Hfi hfi;
    boot(hfi);
    A830HfiStartOptions options={A830HfiStartAcd|A830HfiStartQueryAb,7,6,0,0};
    fw.scripts[A830_HFI_H2F_GET_VALUE].value=1;
    CHECK(hfi.start(tables,options)==kIOReturnSuccess && fw.received.size()==9);
    expectCommand(6,0x0070030C,{122,0});
    expectCommand(7,0x0080010A,{});
    expectCommand(8,0x0090041E,{1,7,0xFFFF0006u});
    CHECK(hfi.currentLevel()==7 && hfi.currentBus()==6);
    /* IFPC (P2): {9,1,0x1680}. */
    boot(hfi);
    options={A830HfiStartIfpc,2,2,0,0};
    CHECK(hfi.start(tables,options)==kIOReturnSuccess);
    expectCommand(2,0x0030040B,{9,1,0x1680});
    /* No initial vote. */
    boot(hfi);
    options={0,A830HfiNoVote,A830HfiNoVote,0,0};
    CHECK(hfi.start(tables,options)==kIOReturnSuccess && fw.received.size()==5 && hfi.status(9)==A830HfiStatusNone);
}

static void testTolerated()
{
    A830Hfi hfi;
    /* M5 off form answered with an error, M7 with an error: both only recorded. */
    boot(hfi);
    fw.scripts[A830_HFI_H2F_FEATURE_CTRL].status=A830_HFI_V5_ERROR_NO_ENTRY;
    fw.scripts[A830_HFI_H2F_GET_VALUE].status=A830_HFI_V5_ERROR_NOT_SUPPORTED;
    fw.scripts[A830_HFI_H2F_GET_VALUE].value=1;
    CHECK(hfi.start(tables,2,2)==kIOReturnSuccess && hfi.status(5)==8 && hfi.status(7)==7 && !hfi.abSupported());
    CHECK(fw.received.size()==7 && hfi.stats().errors==0);
    /* A v5 value (word 2) is a status for FEATURE_CTRL: recorded for the off form. */
    boot(hfi);
    fw.scripts[A830_HFI_H2F_FEATURE_CTRL].value=1;
    CHECK(hfi.start(tables,2,2)==kIOReturnSuccess && hfi.status(5)==1);
    /* ... and fatal for the IFPC on form. */
    boot(hfi);
    fw.scripts[A830_HFI_H2F_FEATURE_CTRL].status=A830_HFI_V5_ERROR_TIMEOUT;
    A830HfiStartOptions options={A830HfiStartIfpc,2,2,0x1680,0};
    CHECK(hfi.start(tables,options)==kIOReturnIOError);
    CHECK(hfi.failure().step==0x15 && hfi.failure().reason==A830ReasonHfiStatus && hfi.failure().value==6 &&
          hfi.failure().hdr==0x0030040B && !hfi.started() && fw.received.size()==3);
}

static void testFailures()
{
    A830Hfi hfi;
    /* M1 answered with a non-zero status: step 0x11, reason 15, the header in reg. */
    boot(hfi);
    fw.scripts[A830_HFI_H2F_TABLE].status=7;
    CHECK(hfi.start(tables,2,2)==kIOReturnIOError);
    CHECK(hfi.failure().step==0x11 && hfi.failure().reason==A830ReasonHfiStatus && hfi.failure().hdr==0x0010310F &&
          hfi.failure().value==7 && hfi.status(1)==7 && fw.received.size()==1 && hfi.firstAckSeen());
    /* No answer at all: 1 s of 100 us sleeps, reason 5, and no first ACK (try the KGSL layout next boot). */
    boot(hfi);
    fw.scripts[A830_HFI_H2F_TABLE].drop=true;
    CHECK(hfi.start(tables,2,2)==kIOReturnTimeout);
    CHECK(hfi.failure().step==0x11 && hfi.failure().reason==A830ReasonTimeout && !hfi.firstAckSeen());
    CHECK(fw.now>=A830_T_HFI_ACK_US && fw.now<=A830_T_HFI_ACK_US+A830_POLL_STEP_US && fw.sleeps==A830_T_HFI_ACK_US/100);
    CHECK(hfi.stats().timeouts==1 && hfi.status(1)==A830HfiStatusNone);
    /* NMI (CM3_FW_INIT_RESULT bits 0xE00) ends the wait at once. */
    boot(hfi);
    fw.scripts[A830_HFI_H2F_BW_VOTE_TBL]={true,5,{},{},0,0,false,false,0,0x200};
    CHECK(hfi.start(tables,2,2)==kIOReturnIOError && hfi.failure().reason==A830ReasonGmuNmi && hfi.failure().step==0x12);
    CHECK(fw.now<10000 && (hfi.failure().value&0xE00)==0x200);
    /* CM3 fault (GMU2HOST bit 15): fatal; bit 23 only recorded. */
    boot(hfi);
    fw.scripts[A830_HFI_H2F_CORE_FW_START]={true,3,{},{},0,0,false,false,A830_GMU2HOST_CM3_FAULT,0};
    CHECK(hfi.start(tables,2,2)==kIOReturnIOError && hfi.failure().reason==A830ReasonGmuFault && hfi.failure().step==0x16);
    boot(hfi);
    fw.scripts[A830_HFI_H2F_TABLE].setInfo=A830_GMU2HOST_CM3_FAULT_LINUX;
    CHECK(hfi.start(tables,2,2)==kIOReturnSuccess && hfi.stats().info_bit23>=1);
    /* The 336/337 handlers saw a fatal fault (the interrupt cleared bit 15 already). */
    boot(hfi);
    fw.scripts[A830_HFI_H2F_TABLE].drop=true;
    fw.faulted=true;
    CHECK(hfi.start(tables,2,2)==kIOReturnIOError && hfi.failure().reason==A830ReasonGmuFault && fw.now==0);
    /* F2H_MSG_ERR ahead of the ACK: reason 16 with the firmware's code and data. */
    boot(hfi);
    Words err={A830_HFI_HDR(A830_HFI_F2H_ERR,0,18,0),0x00030004};
    for(uint32_t i=0;i<16;++i)err.push_back(0x41424300+i);
    fw.scripts[A830_HFI_H2F_BW_VOTE_TBL].before={err};
    CHECK(hfi.start(tables,2,2)==kIOReturnIOError && hfi.failure().reason==A830ReasonF2hError && hfi.failure().step==0x12);
    CHECK(hfi.failure().f2h_id==100 && hfi.failure().f2h_code==0x00030004 && hfi.failure().f2h_data[15]==0x4142430F);
    CHECK(hfi.stats().f2h_err==1 && hfi.stats().last_err[1]==0x00030004);
    /* MEM_ALLOC (20) after START, the way the same firmware asked annibale: reason 22 at once, not after 1 s. */
    for(uint8_t request:{uint8_t(A830_HFI_F2H_MEM_ALLOC),uint8_t(A830_HFI_F2H_GMU_CNTR_REGISTER)}) {
        boot(hfi);
        fw.scripts[A830_HFI_H2F_START]={true,1,{{A830_HFI_HDR(request,0,4,0),1,0x1000,0x60010000}},{},0,0,false,false,0,0};
        CHECK(hfi.start(tables,2,2)==kIOReturnUnsupported);
        CHECK(hfi.failure().reason==A830ReasonGmuHostResource && hfi.failure().step==0x18 && hfi.failure().f2h_id==request);
        CHECK(fw.now<10000 && hfi.stats().f2h_resource==1 && !hfi.started());
    }
}

static void testMessages()
{
    A830Hfi hfi;
    boot(hfi);
    /* DEBUG, an unknown request and an oversized one before the ACK; a stale ACK; the RECORD bit on the echo. */
    Words big(70,0);
    big[0]=A830_HFI_HDR(150,0,70,0);
    fw.scripts[A830_HFI_H2F_TABLE].before={{A830_HFI_HDR(A830_HFI_F2H_DEBUG,0,4,0),7,1234,0x55},{A830_HFI_HDR(150,0,2,0),9},big};
    fw.scripts[A830_HFI_H2F_BW_VOTE_TBL].staleFirst=true;
    fw.scripts[A830_HFI_H2F_CORE_FW_START].record=true;
    fw.scripts[A830_HFI_H2F_START].before={{A830_HFI_HDR(A830_HFI_F2H_LOG_BLOCK,0,3,0),1,2}};
    CHECK(hfi.start(tables,2,2)==kIOReturnSuccess);
    const A830HfiStats &s=hfi.stats();
    CHECK(s.f2h_debug==1 && s.last_debug[1]==7 && s.last_debug[2]==1234 && s.last_debug[3]==0x55);
    CHECK(s.f2h_unknown==2 && s.malformed==1 && s.stale_acks==1 && s.f2h_log==1 && s.acks==8);
    /* send(): any message, the answer returned. */
    A830HfiAck ack;
    const uint32_t payload[2]={5,6};
    fw.scripts[40]={false,1,{},{},0,0,false,false,0,0};
    CHECK(hfi.send(40,payload,2,&ack)==kIOReturnSuccess && ack.dwords==3 && ack.words[1]==0x00800328u && !ack.v5);
    fw.scripts[40].status=3;
    CHECK(hfi.send(40,payload,2,&ack)==kIOReturnIOError && ack.error==3 && hfi.failure().step==40);
    CHECK(hfi.send(40,nullptr,2,&ack)==kIOReturnBadArgument);
    /* GET/SET_VALUE and FEATURE_CTRL in the v5 form. */
    fw.scripts[A830_HFI_H2F_GET_VALUE]={false,1,{},{},0,0x1234,false,false,0,0};
    uint32_t value=0;
    CHECK(hfi.getValue(122,0,&value)==kIOReturnSuccess && value==0x1234);
    fw.scripts[A830_HFI_H2F_GET_VALUE].status=8;
    CHECK(hfi.getValue(122,0,&value)==kIOReturnIOError);
    fw.scripts[A830_HFI_H2F_SET_VALUE]={false,1,{},{},0,0,false,false,0,0};
    CHECK(hfi.setValue(A830_HFI_VALUE_LOG_EVENT_ON,44,0)==kIOReturnSuccess);
    CHECK(fw.received.back()==Words({fw.received.back()[0],112,44,0}) && A830_HFI_HDR_ID(fw.received.back()[0])==13);
    CHECK(hfi.featureCtrl(9,0,0,&ack)==kIOReturnSuccess && ack.v5 && ack.error==0);
    CHECK(lockState.violations==0);
}

/* A firmware older than 2.7 answers in the classic form; older than 2.6.1 is refused before anything is sent. */
static void testVersions()
{
    A830Hfi hfi;
    /* Before start() the firmware version is unknown: answers are read in the classic form (word 2 the error). */
    boot(hfi);
    fw.v5=false;
    fw.scripts[A830_HFI_H2F_FEATURE_CTRL].status=5;
    A830HfiAck ack;
    CHECK(hfi.featureCtrl(12,1,0,&ack)==kIOReturnIOError && !ack.v5 && ack.error==5 && ack.dwords==3);
    /* HFI 2.6.1 (minor 6 > 4): v5 answers. */
    boot(hfi);
    fw.version=0x20060001;
    CHECK(hfi.start(tables,2,2)==kIOReturnSuccess && hfi.version()==0x20060001);
    CHECK(hfi.featureCtrl(12,1,0,&ack)==kIOReturnSuccess && ack.v5 && ack.dwords==4);
    boot(hfi);
    fw.version=0x20050001;
    CHECK(hfi.start(tables,2,2)==kIOReturnUnsupported && fw.received.empty() && hfi.failure().value==0x20050001);
    /* Bad start arguments. */
    boot(hfi);
    CHECK(hfi.start(tables,0,2)==kIOReturnBadArgument && hfi.failure().step==0x19 && fw.received.empty());
    CHECK(hfi.start(tables,12,2)==kIOReturnBadArgument && hfi.start(tables,2,12)==kIOReturnBadArgument);
    static A830VoteTables broken;
    broken=tables;
    broken.perf_dwords=48;
    CHECK(hfi.start(broken,2,2)==kIOReturnBadArgument && fw.received.empty());
    broken=tables;
    broken.acd_levels=0;
    const A830HfiStartOptions acd={A830HfiStartAcd,2,2,0,0};
    CHECK(hfi.start(broken,acd)==kIOReturnBadArgument && hfi.failure().step==0x13 && fw.received.size()==2);
}

static void testPerfVote()
{
    A830Hfi hfi;
    boot(hfi);
    /* Before start: remembered, not sent. */
    uint8_t level=0,bus=0;
    CHECK(hfi.perfVote(5,4)==kIOReturnSuccess && fw.received.empty() && hfi.pending(&level,&bus) && level==5 && bus==4);
    fw.scripts[A830_HFI_H2F_GET_VALUE].value=1;
    CHECK(hfi.start(tables,level,bus)==kIOReturnSuccess && !hfi.pending(nullptr,nullptr));
    expectCommand(6,0x0070041E,{1,5,0xFFFF0004u});
    size_t n=fw.received.size();
    /* {ack_type 1, level, bus | 0xFFFF0000}; bus 0 or out of range: unchanged; both unchanged: nothing sent. */
    CHECK(hfi.perfVote(9,8)==kIOReturnSuccess);
    expectCommand(n++,0x0080041E,{1,9,0xFFFF0008u});
    CHECK(hfi.perfVote(9,0)==kIOReturnSuccess);
    expectCommand(n++,0x0090041E,{1,9,0xFFFF00FFu});
    CHECK(hfi.perfVote(A830HfiNoVote,12)==kIOReturnSuccess && fw.received.size()==n);
    CHECK(hfi.perfVote(A830HfiNoVote,7)==kIOReturnSuccess);
    expectCommand(n++,0x00A0041E,{1,0xFF,0xFFFF0007u});
    CHECK(hfi.perfVote(0,2)==kIOReturnBadArgument && hfi.perfVote(12,2)==kIOReturnBadArgument && fw.received.size()==n);
    CHECK(hfi.currentLevel()==9 && hfi.currentBus()==7);
    /* AB (the GMU answered GET_VALUE(GMU_AB_VOTE) with 1): quantized << 16 | 1 << 8 | bus. */
    CHECK(hfi.abSupported() && hfi.perfVote(7,6,5000000)==kIOReturnSuccess);
    const uint32_t q=uint32_t(5000000ull*65536/(18597656ull*4));
    expectCommand(n++,0x00B0041E,{1,7,q<<16|1u<<8|6u});
    CHECK(hfi.perfVote(A830HfiNoVote,A830HfiNoVote,0)==kIOReturnSuccess);   /* AB 0 is a vote */
    expectCommand(n++,0x00C0041E,{1,0xFF,0x000001FFu});
    /* The raw bw word of A830Dcvs: bus byte normalized, AB bits kept for a GMU that takes AB. */
    CHECK(hfi.perfVoteBw(4,A830Votes::bwWord(3,true,1000000,18597656))==kIOReturnSuccess);
    expectCommand(n++,0x00D0041E,{1,4,A830Votes::bwWord(3,true,1000000,18597656)});
    CHECK(hfi.perfVoteBw(4,0xFFFF0040u)==kIOReturnSuccess);   /* bus 64: out of range, unchanged */
    expectCommand(n++,0x00E0041E,{1,4,0xFFFF00FFu});
    CHECK(hfi.perfVoteBw(A830HfiNoVote,A830_HFI_BW_UNCHANGED)==kIOReturnSuccess && fw.received.size()==n);
    CHECK(hfi.perfVoteBw(0,0xFFFF0002u)==kIOReturnBadArgument && fw.received.size()==n);
    /* A refused vote is counted and keeps the level. */
    fw.scripts[A830_HFI_H2F_GX_BW_PERF_VOTE].status=1;
    CHECK(hfi.perfVote(3,2)==kIOReturnIOError && hfi.currentLevel()==4 && hfi.stats().vote_failures==1);
    fw.scripts[A830_HFI_H2F_GX_BW_PERF_VOTE].status=0;
    n=fw.received.size();
    /* PREPARE_SLUMBER {bw, freq}: afterwards the HFI is stopped and votes are only remembered. */
    CHECK(hfi.prepareSlumber(2,2)==kIOReturnSuccess);
    expectCommand(n++,A830_HFI_HDR(33,0,3,A830_HFI_HDR_SEQ(fw.received.back()[0])),{0xFFFF0002u,2});
    CHECK(!hfi.started() && hfi.perfVote(8,6)==kIOReturnSuccess && fw.received.size()==n && hfi.pending(&level,&bus) &&
          level==8 && bus==6);
    CHECK(hfi.prepareSlumber(2,2)==kIOReturnNotReady);
    /* The next boot: queues reset, sequence from 1, the remembered vote in M9. */
    CHECK(hfi.resetQueues()==kIOReturnSuccess);
    for(unsigned i=0;i<3;++i)CHECK(shm[6+12*i+10]==0 && shm[6+12*i+11]==0);
    fw.received.clear();
    CHECK(hfi.start(tables,level,bus)==kIOReturnSuccess);
    expectCommand(0,0x0010310F,payloadOf(tables.perf,49));
    expectCommand(6,0x0070041E,{1,8,0xFFFF0006u});
    hfi.stop();
    CHECK(!hfi.started() && hfi.perfVote(9,8)==kIOReturnSuccess && hfi.pending(nullptr,nullptr));
    boot(hfi);
    CHECK(hfi.start(tables,2,2)==kIOReturnSuccess);
    CHECK(hfi.prepareSlumber(0,2)==kIOReturnBadArgument && hfi.prepareSlumber(2,0)==kIOReturnBadArgument);
    /* Without AB support the AB bits of a raw word are dropped. */
    n=fw.received.size();
    CHECK(!hfi.abSupported() && hfi.perfVoteBw(5,0x12340105u)==kIOReturnSuccess);
    expectCommand(n++,A830_HFI_HDR(30,0,4,A830_HFI_HDR_SEQ(fw.received.back()[0])),{1,5,0xFFFF0005u});
    /* PREPARE_SLUMBER through send() (A830PowerState) stops the HFI as well. */
    const uint32_t slumber[2]={0xFFFF0002u,2};
    CHECK(hfi.send(A830_HFI_H2F_PREPARE_SLUMBER,slumber,2,nullptr)==kIOReturnSuccess && !hfi.started());
    CHECK(hfi.perfVoteBw(6,0xFFFF0004u)==kIOReturnSuccess && hfi.pending(&level,&bus) && level==6 && bus==4);
    CHECK(lockState.violations==0);
}

/* Indexes wrap at 1024 dwords in both directions; a message may straddle the end. */
static void testWrap()
{
    A830Hfi hfi;
    boot(hfi);
    CHECK(hfi.start(tables,2,2)==kIOReturnSuccess);
    for(unsigned i=0;i<700;++i)CHECK(hfi.perfVote(uint8_t(1+i%11),uint8_t(1+i%11))==kIOReturnSuccess);
    CHECK(!fw.paddingErrors && !fw.sizeErrors && hfi.stats().votes==701);
    /* Both queues parked near the end, then a full start: M1 (52 dwords) and the ACKs straddle the wrap. */
    CHECK(hfi.resetQueues()==kIOReturnSuccess);
    const uint32_t cmd=6+A830_HFI_QHDR_READ_INDEX,msg=6+12+A830_HFI_QHDR_READ_INDEX;
    shm[cmd]=1016;shm[cmd+1]=1016;
    shm[msg]=1020;shm[msg+1]=1020;
    hostIndexes(lockState.snapshot);
    fw.received.clear();
    CHECK(hfi.start(tables,2,2)==kIOReturnSuccess && fw.received.size()==7 && !fw.paddingErrors);
    expectCommand(0,0x0010310F,payloadOf(tables.perf,49));
    expectCommand(1,0x0020A003,payloadOf(tables.bw,160));
    CHECK(shm[cmd+1]<1016 && shm[msg]<1020 && (shm[cmd+1]&3)==0 && (shm[msg]&3)==0);
    /* The ring words around the wrap hold M1's tail. */
    CHECK(sharedMem[0x1000/4+1016]==0x0010310F && sharedMem[0x1000/4+1023]==tables.perf[7] && sharedMem[0x1000/4]==tables.perf[8]);
    /* Full queue: refused before anything is written (reason 6, transport step). */
    CHECK(hfi.resetQueues()==kIOReturnSuccess);
    shm[cmd]=8;   /* GMU read index 8, host write index 0: 7 dwords free */
    hostIndexes(lockState.snapshot);
    fw.received.clear();
    const unsigned doorbells=fw.doorbells;
    CHECK(hfi.start(tables,2,2)==kIOReturnNoResources && hfi.stats().queue_full==1 && fw.doorbells==doorbells);
    CHECK(hfi.failure().step==0x10 && hfi.failure().reason==A830ReasonUnexpectedState && fw.received.empty());
    /* A corrupt msg queue index fails the command (reason 6). */
    CHECK(hfi.resetQueues()==kIOReturnSuccess);
    shm[msg+1]=2000;   /* the GMU's write index: no answer is ever written (the model stops consuming) */
    fw.consume=false;
    CHECK(hfi.start(tables,2,2)==kIOReturnIOError && hfi.failure().reason==A830ReasonUnexpectedState &&
          hfi.failure().value==0 && hfi.failure().expect==2000 && hfi.failure().step==0x11);
    /* A misaligned host write index is refused. */
    CHECK(hfi.resetQueues()==kIOReturnSuccess);
    shm[cmd+1]=2;
    hostIndexes(lockState.snapshot);
    CHECK(hfi.start(tables,2,2)==kIOReturnIOError && hfi.failure().step==0x10);
    CHECK(lockState.violations==0);
}

/* The dbg queue: drained after every command and by drainDebug(), always under the HFI mutex. */
static void testDebugQueue()
{
    A830Hfi hfi;
    boot(hfi);
    fw.scripts[A830_HFI_H2F_TABLE].dbg={{A830_HFI_HDR(A830_HFI_F2H_DEBUG,0,4,0),1,2,3},{A830_HFI_HDR(A830_HFI_F2H_DEBUG,0,4,0),4,5,6}};
    CHECK(hfi.start(tables,2,2)==kIOReturnSuccess);
    const uint32_t dbg=6+24;
    CHECK(shm[dbg+A830_HFI_QHDR_READ_INDEX]==shm[dbg+A830_HFI_QHDR_WRITE_INDEX] && hfi.stats().dbg_messages==2);
    CHECK(hfi.stats().last_debug[1]==4);
    /* Traffic between commands, as the 336 bottom half sees it. */
    fwPush(A830_HFI_QUEUE_ID_DBG,{A830_HFI_HDR(A830_HFI_F2H_DEBUG,0,4,0),7,8,9});
    fwPush(A830_HFI_QUEUE_ID_DBG,{A830_HFI_HDR(A830_HFI_F2H_LOG_BLOCK,0,2,0),1});
    const unsigned locks=lockState.locks;
    CHECK(hfi.drainDebug()==kIOReturnSuccess && lockState.locks==locks+1 && hfi.stats().dbg_messages==4);
    CHECK(shm[dbg+A830_HFI_QHDR_READ_INDEX]==shm[dbg+A830_HFI_QHDR_WRITE_INDEX] && hfi.stats().last_debug[3]==9);
    /* A firmware error there is reported (the caller recovers). */
    fwPush(A830_HFI_QUEUE_ID_DBG,{A830_HFI_HDR(A830_HFI_F2H_ERR,0,3,0),0x11,0x22});
    CHECK(hfi.drainDebug()==kIOReturnIOError && hfi.failure().reason==A830ReasonF2hError && hfi.failure().f2h_code==0x11);
    /* A corrupt dbg queue is skipped, never fatal. */
    shm[dbg+A830_HFI_QHDR_READ_INDEX]=5000;
    hostIndexes(lockState.snapshot);
    CHECK(hfi.drainDebug()==kIOReturnSuccess && hfi.stats().malformed>=1);
    shm[dbg+A830_HFI_QHDR_READ_INDEX]=shm[dbg+A830_HFI_QHDR_WRITE_INDEX];
    hostIndexes(lockState.snapshot);
    fwPush(A830_HFI_QUEUE_ID_DBG,{0x00000000,1,2,3});   /* size 0 */
    CHECK(hfi.drainDebug()==kIOReturnSuccess && shm[dbg+A830_HFI_QHDR_READ_INDEX]==shm[dbg+A830_HFI_QHDR_WRITE_INDEX]);
    /* An F2H_MSG_ERR left in the dbg queue by a command fails that command. */
    fw.scripts[A830_HFI_H2F_GX_BW_PERF_VOTE].dbg={{A830_HFI_HDR(A830_HFI_F2H_ERR,0,3,0),0x33,0}};
    hostIndexes(lockState.snapshot);
    CHECK(hfi.perfVote(3,3)==kIOReturnIOError && hfi.failure().f2h_code==0x33);
    CHECK(lockState.violations==0 && !lockState.held);
}

/* Knob mapping (ARCHITECTURE 3.6) and the gmu-window binding (byte offset = 4 * dword - 0x37000). */
static void testHelpers()
{
    Adreno830Board board={};
    Adreno830KnobDefaults(&board.knobs);
    A830HfiStartOptions o=A830Hfi::startOptions(board,2,2,0x20070001);
    CHECK(o.flags==A830HfiStartQueryAb && o.level==2 && o.bus==2 && o.ifpc_hysteresis==0x1680 && o.hfi_version==0x20070001);
    board.knobs.acd=1;board.knobs.hfi=2;
    o=A830Hfi::startOptions(board,7,6,0);
    CHECK(o.flags==(A830HfiStartQueryAb|A830HfiStartAcd|A830HfiStartDeferStart) && !(o.flags&A830HfiStartIfpc));
    board.knobs.hfi=1;   /* bit0 is the dependency-vote form, a votes option, not a start option */
    CHECK(!(A830Hfi::startOptions(board,2,2,0).flags&A830HfiStartDeferStart));
    static std::vector<uint32_t> window(A830_GMU_BYTES/4,0);
    A830HfiIo bound={};
    A830HfiBindGmuWindow(&bound,reinterpret_cast<volatile uint8_t *>(window.data()));
    CHECK(bound.read && bound.write && bound.context==window.data());
    bound.write(bound.context,A8XX_GMUCX_HOST2GMU_INTR_SET,1);
    CHECK(window[(4*A8XX_GMUCX_HOST2GMU_INTR_SET-0x37000)/4]==1 && (4*A8XX_GMUCX_HOST2GMU_INTR_SET-0x37000)==0x47650);
    window[(4*A8XX_GMU_DTCM_HFI_VERSION-0x37000)/4]=0x20070001;
    CHECK(bound.read(bound.context,A8XX_GMU_DTCM_HFI_VERSION)==0x20070001 && (4*A8XX_GMU_DTCM_HFI_VERSION-0x37000)==0x3DFF0);
    window[(4*A8XX_GMUAO_AO_HOST_INTERRUPT_STATUS-0x37000)/4]=0x29;
    CHECK(bound.read(bound.context,A8XX_GMUAO_AO_HOST_INTERRUPT_STATUS)==0x29);
    A830HfiBindGmuWindow(nullptr,nullptr);
}

/* ---- the annibale profile ------------------------------------------------------------------------------------------ */

/* M1 (4 entries, 24 payload dwords), M2 (x = 0 rows), M5 {9,0,0}, M6, M8, M9 {1,1,2}: no M7 although the default flags ask
 * for it, and no AB afterwards (kernel-sequences 5.4-5.5, 6.1). */
static void testAnnibaleStart(A830HfiLayout layout)
{
    A830Hfi hfi;
    boot(hfi,layout);
    fw.scripts[A830_HFI_H2F_GET_VALUE].value=1;   /* would turn AB votes on if it were asked */
    const A830HfiStartOptions options={A830HfiStartDefault,1,2,0x1680,0};
    CHECK(hfi.start(annibaleTables,options)==kIOReturnSuccess);
    CHECK(hfi.started() && !hfi.abSupported() && hfi.firstAckSeen() && hfi.version()==0x20070001 && fw.versionReads==1);
    CHECK(fw.received.size()==6 && fw.doorbells==6 && !fw.paddingErrors && !fw.sizeErrors);
    expectCommand(0,0x0010190F,payloadOf(annibaleTables.perf,25));
    expectCommand(1,0x0020A003,payloadOf(annibaleTables.bw,160));
    expectCommand(2,0x0030040B,{9,0,0});
    expectCommand(3,0x0040020E,{0});
    expectCommand(4,0x0050010A,{});
    expectCommand(5,0x0060041E,{1,1,2});
    const Words &perf=fw.received[0];
    CHECK(perf[1]==0 && perf[2]==1 && perf[3]==4 && perf[4]==3 && perf[5]==0 && perf[6]==0xFFFFFFFFu && perf[7]==0);
    CHECK(perf[10]==160000 && perf[13]==222000 && perf[16]==342000 && perf[17]==3 && perf[18]==2 && perf[24]==650000);
    for(unsigned i=1;i<12;++i)for(unsigned j=0;j<2;++j)CHECK(((fw.received[1][A830BwDdrData+i*8+j]>>14)&0x3fff)==0);
    const unsigned sent[]={1,2,5,6,8,9};
    for(unsigned m:sent)CHECK(hfi.status(m)==0);
    CHECK(hfi.status(7)==A830HfiStatusNone && hfi.header(7)==0 && hfi.header(9)==0x0060041E);
    CHECK(hfi.currentLevel()==1 && hfi.currentBus()==2 && hfi.stats().late_drains==0 && fw.lateSleeps==0);
    CHECK(lockState.violations==0 && !lockState.held && lockState.locks==lockState.unlocks);
}

/* FEATURE_CTRL needs data 0 and error 0, in 4 dwords; any ACK needs 3 (K/adreno8_hfi.c:193-220). The design takes them. */
static void testAnnibaleStrict()
{
    A830Hfi hfi;
    boot(hfi);
    fw.scripts[A830_HFI_H2F_FEATURE_CTRL].value=1;
    CHECK(hfi.start(annibaleTables,1,2)==kIOReturnIOError && !hfi.started() && fw.received.size()==3);
    CHECK(hfi.failure().step==0x15 && hfi.failure().reason==A830ReasonHfiStatus && hfi.failure().value==1 &&
          hfi.failure().hdr==0x0030040B && hfi.status(5)==1);
    CHECK(hfi.stats().late_drains==1 && hfi.stats().late_rounds==31 && fw.lateSleeps==30);   /* then the late drain */
    boot(hfi);
    fw.scripts[A830_HFI_H2F_FEATURE_CTRL].status=A830_HFI_V5_ERROR_NO_ENTRY;
    CHECK(hfi.start(annibaleTables,1,2)==kIOReturnIOError && hfi.failure().value==8 && hfi.status(5)==8);
    boot(hfi);
    fw.ackDwords[A830_HFI_H2F_FEATURE_CTRL]=3;
    CHECK(hfi.start(annibaleTables,1,2)==kIOReturnIOError && hfi.failure().step==0x15 && hfi.failure().value==3 &&
          hfi.failure().expect==4 && hfi.status(5)==A830HfiStatusNone);
    boot(hfi);
    fw.ackDwords[A830_HFI_H2F_TABLE]=2;
    CHECK(hfi.start(annibaleTables,1,2)==kIOReturnIOError && hfi.failure().step==0x11 && hfi.failure().value==2 &&
          hfi.failure().expect==3 && fw.received.size()==1);
    adreno830_profile_value=A830ProfileDesign;
    boot(hfi);
    fw.ackDwords[A830_HFI_H2F_FEATURE_CTRL]=3;
    fw.ackDwords[A830_HFI_H2F_TABLE]=2;
    fw.scripts[A830_HFI_H2F_FEATURE_CTRL].value=0;
    CHECK(hfi.start(tables,2,2)==kIOReturnSuccess && hfi.status(1)==0 && hfi.status(5)==0 && hfi.stats().late_drains==0);
    adreno830_profile_value=A830ProfileAnnibale;
    /* The version must also be major 2 (5.2 step 5); the design looks at the minimum only. Nothing is sent, nothing drained. */
    boot(hfi);
    fw.version=0x30070001;
    CHECK(hfi.start(annibaleTables,1,2)==kIOReturnUnsupported && fw.received.empty() && hfi.stats().late_drains==0);
    adreno830_profile_value=A830ProfileDesign;
    boot(hfi);
    fw.version=0x30070001;
    CHECK(hfi.start(tables,2,2)==kIOReturnSuccess);
    adreno830_profile_value=A830ProfileAnnibale;
    /* Arguments refused before anything is queued: no drain either. */
    boot(hfi);
    CHECK(hfi.start(annibaleTables,0,2)==kIOReturnBadArgument && hfi.start(annibaleTables,4,2)==kIOReturnBadArgument);
    CHECK(fw.received.empty() && hfi.stats().late_drains==0 && fw.now==0);
}

/* GX_BW_PERF_VOTE carries the bus alone in bw; the GMU levels are annibale's 1..3. */
static void testAnnibaleVotes()
{
    A830Hfi hfi;
    boot(hfi);
    CHECK(hfi.start(annibaleTables,1,2)==kIOReturnSuccess);
    uint32_t n=uint32_t(fw.received.size());
    auto vote=[&n](const Words &payload) {expectCommand(n,A830_HFI_HDR(A830_HFI_H2F_GX_BW_PERF_VOTE,0,4,n+1),payload);++n;};
    CHECK(hfi.perfVote(3,3)==kIOReturnSuccess);vote({1,3,3});
    CHECK(hfi.perfVote(2,0)==kIOReturnSuccess);vote({1,2,0xFF});
    CHECK(hfi.perfVote(A830HfiNoVote,12)==kIOReturnSuccess && fw.received.size()==n);   /* both unchanged */
    CHECK(hfi.perfVote(A830HfiNoVote,2)==kIOReturnSuccess);vote({1,0xFF,2});
    CHECK(hfi.perfVote(2,2,5000000)==kIOReturnSuccess);vote({1,2,2});                 /* no AB without M7 */
    CHECK(hfi.perfVoteBw(3,0xFFFF0002u)==kIOReturnSuccess);vote({1,3,2});               /* the KGSL high half dropped */
    CHECK(hfi.perfVoteBw(3,A830Votes::bwWord(3,false,0,0))==kIOReturnSuccess);vote({1,3,3});
    CHECK(hfi.perfVoteBw(A830HfiNoVote,A830_HFI_BW_UNCHANGED)==kIOReturnSuccess && fw.received.size()==n);
    CHECK(hfi.perfVoteBw(A830HfiNoVote,0xFFu)==kIOReturnSuccess && fw.received.size()==n);
    CHECK(hfi.perfVote(4,2)==kIOReturnBadArgument && fw.received.size()==n);
    CHECK(hfi.currentLevel()==3 && hfi.currentBus()==3 && hfi.stats().votes==7 && hfi.stats().late_drains==0);
    /* A vote the GMU refuses: counted, then the late drain (3 s). */
    fw.scripts[A830_HFI_H2F_GX_BW_PERF_VOTE].status=1;
    CHECK(hfi.perfVote(1,2)==kIOReturnIOError && hfi.stats().vote_failures==1 && hfi.stats().late_drains==1 && fw.lateSleeps==30);
    CHECK(lockState.violations==0);
}

/* The poll clears GMU2HOST bits 0 and 2 (K/adreno8_hfi.c:124); the design leaves bit 2 to the 336 handler. */
static void testAnnibaleClear()
{
    A830Hfi hfi;
    boot(hfi);
    fw.scripts[A830_HFI_H2F_TABLE].dbg={{A830_HFI_HDR(A830_HFI_F2H_DEBUG,0,4,0),1,2,3}};
    CHECK(hfi.start(annibaleTables,1,2)==kIOReturnSuccess && fw.dbgqClears>=1 && !(fw.info&A830_GMU2HOST_DBGQ));
    CHECK(hfi.stats().dbg_messages==1);
    adreno830_profile_value=A830ProfileDesign;
    boot(hfi);
    fw.scripts[A830_HFI_H2F_TABLE].dbg={{A830_HFI_HDR(A830_HFI_F2H_DEBUG,0,4,0),1,2,3}};
    CHECK(hfi.start(tables,2,2)==kIOReturnSuccess && fw.dbgqClears==0 && (fw.info&A830_GMU2HOST_DBGQ));
    adreno830_profile_value=A830ProfileAnnibale;
}

/* After a failure the queues are read every 100 ms for 3 s (field-notes 9-1); what comes is recorded, failure() stays. */
static void testLateDrain()
{
    A830Hfi hfi;
    /* M1 answered too late, an F2H_MSG_ERR ahead of the ACK: both arrive with the second 100 ms step and end the drain. */
    boot(hfi);
    fw.scripts[A830_HFI_H2F_TABLE].delay=10002;
    fw.scripts[A830_HFI_H2F_TABLE].before={{A830_HFI_HDR(A830_HFI_F2H_ERR,0,4,0),0x77,1,2}};
    CHECK(hfi.start(annibaleTables,1,2)==kIOReturnTimeout);
    CHECK(hfi.failure().reason==A830ReasonTimeout && hfi.failure().step==0x11 && hfi.failure().f2h_id==0);
    const A830HfiStats &st=hfi.stats();
    CHECK(st.late_drains==1 && st.late_rounds==3 && st.late_messages==2 && st.late_acks==1 && st.late_errors==1);
    CHECK(st.late_err[0]==A830_HFI_HDR(A830_HFI_F2H_ERR,0,4,0) && st.late_err[1]==0x77 && st.late_err[3]==2 && st.f2h_err==0);
    CHECK(fw.now==A830_T_HFI_ACK_US+2*A830HfiLateDrainStepUs && fw.lateSleeps==2 && hfi.firstAckSeen());
    CHECK(hfi.drainLate()==0 && st.late_drains==1 && st.late_rounds==3);   /* that failure is drained already */
    /* Nothing comes: the whole 3 s, 31 rounds. */
    boot(hfi);
    fw.scripts[A830_HFI_H2F_TABLE].drop=true;
    CHECK(hfi.start(annibaleTables,1,2)==kIOReturnTimeout && !hfi.firstAckSeen());
    CHECK(hfi.stats().late_rounds==31 && hfi.stats().late_messages==0 && fw.now==A830_T_HFI_ACK_US+A830HfiLateDrainUs);
    /* A CM3 fault or an NMI: the GMU is gone, one round. */
    boot(hfi);
    fw.scripts[A830_HFI_H2F_CORE_FW_START]={true,3,{},{},0,0,false,false,A830_GMU2HOST_CM3_FAULT,0};
    CHECK(hfi.start(annibaleTables,1,2)==kIOReturnIOError && hfi.failure().reason==A830ReasonGmuFault);
    CHECK(hfi.stats().late_drains==1 && hfi.stats().late_rounds==1 && fw.lateSleeps==0);
    boot(hfi);
    fw.scripts[A830_HFI_H2F_BW_VOTE_TBL]={true,5,{},{},0,0,false,false,0,0x200};
    CHECK(hfi.start(annibaleTables,1,2)==kIOReturnIOError && hfi.failure().reason==A830ReasonGmuNmi);
    CHECK(hfi.stats().late_rounds==1 && fw.lateSleeps==0);
    /* A host resource request (reason 22) fails at once and is drained as well. */
    boot(hfi);
    fw.scripts[A830_HFI_H2F_START]={true,1,{{A830_HFI_HDR(A830_HFI_F2H_MEM_ALLOC,0,4,0),1,0x1000,0x60010000}},{},0,0,false,false,0,0};
    CHECK(hfi.start(annibaleTables,1,2)==kIOReturnUnsupported && hfi.failure().reason==A830ReasonGmuHostResource);
    CHECK(hfi.stats().late_drains==1 && hfi.stats().late_rounds==31);
    /* drainLate() for the caller's own failures (a hang verdict, S6-S8): ends at an F2H_MSG_ERR, once until the next
     * command, and takes the HFI mutex. */
    boot(hfi);
    CHECK(hfi.start(annibaleTables,1,2)==kIOReturnSuccess);
    fwPush(A830_HFI_QUEUE_ID_MSG,{A830_HFI_HDR(A830_HFI_F2H_DEBUG,0,4,0),1,2,3});
    fwPush(A830_HFI_QUEUE_ID_DBG,{A830_HFI_HDR(A830_HFI_F2H_DEBUG,0,4,0),4,5,6});
    fwPush(A830_HFI_QUEUE_ID_DBG,{A830_HFI_HDR(A830_HFI_F2H_ERR,0,3,0),0x99,0});
    hostIndexes(lockState.snapshot);
    const uint64_t before=fw.now;
    const unsigned locks=lockState.locks;
    CHECK(hfi.drainLate()==3 && hfi.stats().late_rounds==1 && hfi.stats().late_errors==1 && hfi.stats().late_err[1]==0x99);
    CHECK(fw.now==before && lockState.locks==locks+1 && hfi.failure().reason==0);
    CHECK(hfi.drainLate()==0 && lockState.locks==locks+2);
    CHECK(hfi.perfVote(2,2)==kIOReturnSuccess);
    const uint64_t later=fw.now;
    CHECK(hfi.drainLate(2*A830HfiLateDrainStepUs)==0 && hfi.stats().late_rounds==4 && fw.now==later+2*A830HfiLateDrainStepUs);
    CHECK(lockState.violations==0 && !lockState.held);
    /* The design: no drain, ever. */
    adreno830_profile_value=A830ProfileDesign;
    boot(hfi);
    fw.scripts[A830_HFI_H2F_TABLE].drop=true;
    CHECK(hfi.start(tables,2,2)==kIOReturnTimeout && fw.now==A830_T_HFI_ACK_US && hfi.stats().late_drains==0);
    CHECK(hfi.drainLate()==0 && fw.now==A830_T_HFI_ACK_US && hfi.stats().late_rounds==0 && fw.lateSleeps==0);
    adreno830_profile_value=A830ProfileAnnibale;
}

int main()
{
    makeTables();
    /* The design profile (ARCHITECTURE 4.7.5-4.7.6 as written). */
    adreno830_profile_value=A830ProfileDesign;
    testHelpers();
    testInitLayouts();
    testStartP0(A830HfiLayoutLinux);
    testStartP0(A830HfiLayoutKgsl);
    testStartVariants();
    testStartM9Order();
    testTolerated();
    testFailures();
    testMessages();
    testVersions();
    testPerfVote();
    testWrap();
    testDebugQueue();
    /* The annibale profile. */
    adreno830_profile_value=A830ProfileAnnibale;
    testAnnibaleStart(A830HfiLayoutLinux);
    testAnnibaleStart(A830HfiLayoutKgsl);
    testAnnibaleStrict();
    testAnnibaleVotes();
    testAnnibaleClear();
    testLateDrain();
    std::printf("PASS: A830Hfi queues, wrap and 0xFAFAFAFA padding, whole-header ACK match, v5 answers, F2H handling (reason "
                "16/22), 1 s timeout, NMI and CM3 exits, M1-M9 encodings, perf/slumber votes, dbg drain under the lock "
                "(design); 4-entry perf table, no M7, M9 {1,1,2}, strict FEATURE_CTRL and short ACKs, bit 0/2 poll clear, "
                "bus-only votes, major version, 3 s late drain (annibale) (%u checks)\n",checks);
    return 0;
}
