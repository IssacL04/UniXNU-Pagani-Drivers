/* SPDX-License-Identifier: MIT */
#include "A830SelfTest.h"
#include "A830Regs.h"
#include "AdrenoPM4.h"
#include "Adreno830Status.h"

extern "C" A830SelfTestReport a830_selftest_last;
A830SelfTestReport a830_selftest_last;

namespace {
/* A tiny PM4 writer over a caller buffer: any packet that does not fit makes the whole stream empty (0 words). */
struct Stream {
    uint32_t *out;size_t capacity,count;bool bad;
    bool room(size_t n) {if(bad || count+n>capacity) {bad=true;return false;}return true;}
    void type7(uint32_t opcode,uint32_t payload)
    {
        uint32_t header=0;
        if(AdrenoPM4Type7(opcode,payload,&header)!=AdrenoPM4Success) {bad=true;return;}
        if(room(1))out[count++]=header;
    }
    void type4(uint32_t reg,uint32_t payload)
    {
        uint32_t header=0;
        if(AdrenoPM4Type4(reg,payload,&header)!=AdrenoPM4Success) {bad=true;return;}
        if(room(1))out[count++]=header;
    }
    void word(uint32_t value) {if(room(1))out[count++]=value;}
    void reg(uint32_t reg,uint32_t value) {type4(reg,1);word(value);}
    void event(uint32_t event) {type7(A8XX_PM4_CP_EVENT_WRITE7,1);word(event);}
    size_t done() const {return bad?0:count;}
};

void barrier() {__atomic_thread_fence(__ATOMIC_SEQ_CST);}
uint32_t v9aPattern(unsigned round,unsigned line,unsigned word) {return 0xA9000000u|round<<20|line<<8|word;}
uint32_t v9bOld(unsigned line) {return 0xB0000000u|line;}
uint32_t v9bNew(unsigned line) {return 0xB9B90000u|line;}
uint32_t v4Pattern(unsigned word) {return 0x4F4F0000u|word*0x111u;}
constexpr uint32_t kV9cValue=0xC9C9C9C9u;
constexpr size_t kPageBytes=4096,kPageWords=kPageBytes/4;
}

A830SelfTest::A830SelfTest(const A830SelfTestEnv &env):env_(env) {}

uint32_t A830SelfTest::timeoutUs() const {return env_.timeout_us?env_.timeout_us:A830_T_SELFTEST_US;}

/* ---- streams ---- */

size_t A830SelfTest::memWriteWords(uint64_t iova,uint32_t value,uint32_t *out,size_t capacity)
{
    Stream s{out,capacity,0,false};
    s.type7(A8XX_PM4_CP_MEM_WRITE,3);s.word(uint32_t(iova));s.word(uint32_t(iova>>32));s.word(value);
    return s.done();
}

/* CP_MEM_TO_MEM (0x73): { flags, DST lo/hi, SRC_A lo/hi } copies one dword; invalidate first drops UCHE lines a previous
 * round left (CACHE_INVALIDATE7, as the framed submissions do). */
size_t A830SelfTest::copyWords(const uint64_t *src,const uint64_t *dst,size_t count,bool invalidate,uint32_t *out,size_t capacity)
{
    Stream s{out,capacity,0,false};
    if(invalidate)s.event(A8XX_EVENT_CACHE_INVALIDATE7);
    for(size_t i=0;i<count;++i) {
        s.type7(A8XX_PM4_CP_MEM_TO_MEM,5);
        s.word(0);s.word(uint32_t(dst[i]));s.word(uint32_t(dst[i]>>32));s.word(uint32_t(src[i]));s.word(uint32_t(src[i]>>32));
    }
    s.type7(A8XX_PM4_CP_WAIT_MEM_WRITES,0);
    return s.done();
}

size_t A830SelfTest::fillWords(const uint64_t *dst,const uint32_t *values,size_t count,uint32_t *out,size_t capacity)
{
    Stream s{out,capacity,0,false};
    for(size_t i=0;i<count;++i) {
        s.type7(A8XX_PM4_CP_MEM_WRITE,3);s.word(uint32_t(dst[i]));s.word(uint32_t(dst[i]>>32));s.word(values[i]);
    }
    s.type7(A8XX_PM4_CP_WAIT_MEM_WRITES,0);
    return s.done();
}

/* V9c (ARCHITECTURE 4.14.2): a pass through the color path: CCU in sysmem mode, the 2D engine fills [iova, iova+bytes) with
 * value as R32_UINT (one row, Turnip's vkCmdFillBuffer), then the CCU is cleaned; the ring's fence that follows is the
 * CACHE_CLEAN that stopped the A740 on write-back inner-shareable targets. */
size_t A830SelfTest::blitWords(uint64_t iova,uint32_t bytes,uint32_t value,uint32_t *out,size_t capacity)
{
    Stream s{out,capacity,0,false};
    if((iova&63) || !bytes || (bytes&3) || bytes/4>0x4000) {s.bad=true;return 0;}
    const uint32_t texels=bytes/4;
    s.type7(A8XX_PM4_CP_THREAD_CONTROL,1);s.word(A8XX_CP_THREAD_SYNC|A8XX_CP_THREAD_BR);
    s.event(A8XX_EVENT_CCU_CLEAN_COLOR);
    s.event(A8XX_EVENT_CCU_INVALIDATE_COLOR);
    s.type7(A8XX_PM4_CP_WAIT_FOR_IDLE,0);
    s.reg(A8XX_RB_CCU_CACHE_CNTL,A830_RB_CCU_CACHE_CNTL_SYSMEM);
    s.reg(A8XX_RB_A2D_PIXEL_CNTL,0);
    s.reg(A8XX_RB_A2D_BLT_CNTL,A8XX_A2D_BLT_CNTL(A8XX_FMT6_32_UINT,A8XX_R2D_INT32));
    s.reg(A8XX_GRAS_A2D_BLT_CNTL,A8XX_A2D_BLT_CNTL(A8XX_FMT6_32_UINT,A8XX_R2D_INT32));
    s.reg(A8XX_TPL1_A2D_BLT_CNTL,A8XX_TPL1_A2D_BLT_CNTL_TYPE(A8XX_TEX_2D));
    s.reg(A8XX_SP_A2D_OUTPUT_INFO,A8XX_SP_A2D_OUTPUT_INFO_VALUE(A8XX_OUTPUT_IFMT_2D_UINT,A8XX_FMT6_32_UINT));
    s.type4(A8XX_RB_A2D_CLEAR_COLOR_DW0,4);s.word(value);s.word(0);s.word(0);s.word(0);
    s.type4(A8XX_RB_A2D_DEST_BUFFER_INFO,4);
    s.word(A8XX_FMT6_32_UINT);s.word(uint32_t(iova));s.word(uint32_t(iova>>32));s.word(0);
    s.type4(A8XX_GRAS_A2D_DEST_TL,2);s.word(A8XX_REG_XY(0,0));s.word(A8XX_REG_XY(texels-1,0));
    s.type7(A8XX_PM4_CP_WAIT_FOR_IDLE,0);
    s.type7(A8XX_PM4_CP_BLIT,1);s.word(A8XX_CP_BLIT_OP_SCALE);
    s.type7(A8XX_PM4_CP_WAIT_FOR_IDLE,0);
    s.event(A8XX_EVENT_CCU_CLEAN_COLOR);
    return s.done();
}

/* ---- helpers ---- */

IOReturn A830SelfTest::fenced(const uint32_t *words,size_t count,uint32_t *seq,A830WaitStats *stats,A830SelfTestFailure *failure,
                              uint16_t step)
{
    *seq=0;
    if(!count) {*failure={A830S8StepSubmit,A830ReasonArgument,step,0,0};return kIOReturnBadArgument;}
    uint32_t fence=0;
    const IOReturn submitted=env_.submit(env_.ring,words,count,&fence);
    if(submitted!=kIOReturnSuccess || !fence) {
        *failure={A830S8StepSubmit,A830ReasonUnexpectedState,step,uint64_t(uint32_t(submitted)),0};
        return submitted!=kIOReturnSuccess?submitted:kIOReturnIOError;
    }
    *seq=fence;
    env_.fence->submitted(fence);
    return env_.fence->waitNs(fence,uint64_t(timeoutUs())*1000,stats);
}

bool A830SelfTest::mapped(A830SelfTestPage *page,unsigned mode)
{
    *page={};
    if(env_.vm.allocate(env_.vm.cookie,page)!=kIOReturnSuccess || !page->cpu)return false;
    if(env_.vm.map(env_.vm.cookie,page,mode)!=kIOReturnSuccess || !page->iova) {env_.vm.release(env_.vm.cookie,page);return false;}
    return true;
}

void A830SelfTest::drop(A830SelfTestPage *page,bool mapped_page)
{
    if(mapped_page)env_.vm.unmap(env_.vm.cookie,page);
    env_.vm.release(env_.vm.cookie,page);
}

/* ---- the checks ---- */

IOReturn A830SelfTest::memWrite(uint64_t iova,volatile uint32_t *cpu,uint32_t value,A830SelfTestReport *report,
                                A830SelfTestFailure *failure)
{
    A830SelfTestFailure local{};
    A830SelfTestFailure &f=failure?*failure:local;
    if(!env_.irq || !env_.fence || !env_.submit || !cpu) {f={A830S8StepMemWrite,A830ReasonArgument,iova,0,0};return kIOReturnBadArgument;}
    *cpu=0;
    barrier();
    const uint64_t before=env_.irq->fenceIrqs();
    uint32_t words[8];
    const size_t count=memWriteWords(iova,value,words,8);
    uint32_t seq=0;
    A830WaitStats stats{};
    const IOReturn waited=fenced(words,count,&seq,&stats,&f,A830S8StepMemWrite);
    if(report)report->fence=seq;
    if(waited!=kIOReturnSuccess) {
        if(seq)f={A830S8StepMemWrite,uint16_t(waited==kIOReturnTimeout?A830ReasonTimeout:A830ReasonUnexpectedState),iova,
                  env_.fence->completed(),seq};
        return waited;
    }
    barrier();
    const uint32_t readback=*cpu;
    const uint64_t after=env_.irq->fenceIrqs();
    if(report) {
        report->scratch=readback;report->fence_irqs=after;report->irq_count=env_.irq->count();
        report->readback=env_.irq->readback();report->router=env_.irq->router();
        const uint64_t irq=env_.irq->lastFenceIrq();
        report->wake_us=irq && stats.returned>irq?uint32_t(a830AbsToNs(stats.returned-irq)/1000):0;
    }
    if(readback!=value) {f={A830S8StepReadback,A830ReasonUnexpectedState,iova,readback,value};return kIOReturnIOError;}
    /* ARCHITECTURE 4.12.1: the criterion of the 332 wiring is "count + 1, waiter woken", not the GIC readback. */
    if(after<=before) {f={A830S8StepIrq,A830ReasonIrqWiring,A830_IRQ_GPU,after,before+1};return kIOReturnIOError;}
    if(stats.late) {f={A830S8StepWake,A830ReasonIrqWiring,A830_IRQ_GPU,stats.slices,0};return kIOReturnIOError;}
    return kIOReturnSuccess;
}

void A830SelfTest::timestamps(uint64_t *cp,uint64_t *ao)
{
    *cp=0;*ao=0;
    if(env_.irq && env_.irq->gpuMmio().valid()) {
        const A830Mmio &gpu=env_.irq->gpuMmio();
        /* hi - lo - hi (KGSL gen8_read_alwayson): a carry between the reads re-reads lo. */
        uint32_t hi=gpu.read32(gpu.cookie,A830_GPU_REG_BYTE(A8XX_CP_ALWAYS_ON_COUNTER_HI));
        uint32_t lo=gpu.read32(gpu.cookie,A830_GPU_REG_BYTE(A8XX_CP_ALWAYS_ON_COUNTER));
        const uint32_t again=gpu.read32(gpu.cookie,A830_GPU_REG_BYTE(A8XX_CP_ALWAYS_ON_COUNTER_HI));
        if(again!=hi) {lo=gpu.read32(gpu.cookie,A830_GPU_REG_BYTE(A8XX_CP_ALWAYS_ON_COUNTER));hi=again;}
        *cp=uint64_t(hi)<<32|lo;
    }
    if(env_.ao_counter)*ao=env_.ao_counter(env_.timer);
}

/* V4: a new leaf written through the table page's cacheable view, published with dsb ishst only; the CP copies 16 dwords
 * through it. The data itself was cleaned first, so only the table walk is under test. */
uint8_t A830SelfTest::checkV4(A830CoherenceResult *result,A830SelfTestFailure *failure,bool *hung)
{
    A830SelfTestPage page;
    if(!mapped(&page,A830SelfTestMapCachedTable))return A830CohFail;
    for(unsigned i=0;i<kPageWords;++i)page.cpu[i]=i<A830SelfTestV4Words?v4Pattern(i):0;
    barrier();
    env_.vm.maintain(env_.vm.cookie,&page,A830SelfTestClean);
    uint64_t far=0;
    if(env_.vm.faults)env_.vm.faults(env_.vm.cookie,&far);   /* forget anything earlier */
    volatile uint32_t *out=env_.scratch+A830SelfTestScratchV4/4;
    uint64_t src[A830SelfTestV4Words],dst[A830SelfTestV4Words];
    for(unsigned i=0;i<A830SelfTestV4Words;++i) {
        out[i]=0;src[i]=page.iova+4*i;dst[i]=env_.scratch_iova+A830SelfTestScratchV4+4*i;
    }
    barrier();
    uint32_t words[A830SelfTestV4Words*6+4];
    const size_t count=copyWords(src,dst,A830SelfTestV4Words,false,words,sizeof words/sizeof words[0]);
    uint32_t seq=0;
    const IOReturn waited=fenced(words,count,&seq,nullptr,failure,A830S8StepV4);
    uint8_t verdict=A830CohFail;
    if(waited==kIOReturnTimeout) *hung=true;
    else if(waited==kIOReturnSuccess) {
        barrier();
        result->v4_fsr=env_.vm.faults?env_.vm.faults(env_.vm.cookie,&far):0;
        result->v4_far=far;
        for(unsigned i=0;i<A830SelfTestV4Words;++i)if(out[i]!=v4Pattern(i))++result->v4_mismatch;
        verdict=!result->v4_fsr && !result->v4_mismatch?A830CohPass:A830CohFail;
    }
    drop(&page,true);
    return verdict;
}

/* V9a: CPU writes 64 lines through the cacheable view (no clean); the GPU reads them through WB+IS; 8 rounds. */
uint8_t A830SelfTest::checkV9a(A830CoherenceResult *result,A830SelfTestFailure *failure,bool *hung)
{
    A830SelfTestPage page;
    if(!mapped(&page,A830SelfTestMapCoherentRo))return A830CohFail;
    volatile uint32_t *out=env_.scratch+A830SelfTestScratchV9a/4;
    uint64_t src[A830SelfTestV9Lines],dst[A830SelfTestV9Lines];
    for(unsigned line=0;line<A830SelfTestV9Lines;++line) {
        src[line]=page.iova+line*A830SelfTestLineBytes;dst[line]=env_.scratch_iova+A830SelfTestScratchV9a+4*line;
    }
    static uint32_t words[A830SelfTestV9Lines*6+8];
    const size_t count=copyWords(src,dst,A830SelfTestV9Lines,true,words,sizeof words/sizeof words[0]);
    uint8_t verdict=A830CohPass;
    for(unsigned round=0;round<A830SelfTestV9aRounds && verdict==A830CohPass;++round) {
        for(unsigned line=0;line<A830SelfTestV9Lines;++line)
            for(unsigned word=0;word<A830SelfTestLineBytes/4;++word)
                page.cpu[line*(A830SelfTestLineBytes/4)+word]=v9aPattern(round,line,word);
        for(unsigned line=0;line<A830SelfTestV9Lines;++line)out[line]=0;
        barrier();
        uint32_t seq=0;
        const IOReturn waited=fenced(words,count,&seq,nullptr,failure,A830S8StepV9a);
        if(waited!=kIOReturnSuccess) {if(waited==kIOReturnTimeout)*hung=true;verdict=A830CohFail;break;}
        barrier();
        uint32_t mismatches=0;
        for(unsigned line=0;line<A830SelfTestV9Lines;++line)if(out[line]!=v9aPattern(round,line,0))++mismatches;
        result->v9a_mismatch+=mismatches;
        if(mismatches)verdict=A830CohFail;
        else ++result->v9a_rounds;
    }
    drop(&page,true);
    return verdict;
}

/* V9b: the CPU holds clean lines of the page; the GPU writes new values through WB+IS; the CPU reads them without any
 * invalidate. */
uint8_t A830SelfTest::checkV9b(A830CoherenceResult *result,A830SelfTestFailure *failure,bool *hung)
{
    A830SelfTestPage page;
    if(!mapped(&page,A830SelfTestMapCoherentRw))return A830CohFail;
    const unsigned stride=A830SelfTestLineBytes/4;
    for(unsigned i=0;i<kPageWords;++i)page.cpu[i]=i%stride?0:v9bOld(i/stride);
    barrier();
    env_.vm.maintain(env_.vm.cookie,&page,A830SelfTestClean);
    uint32_t sum=0;
    for(unsigned line=0;line<A830SelfTestV9Lines;++line)sum+=page.cpu[line*stride];   /* the lines are now in the cache */
    (void)sum;
    uint64_t dst[A830SelfTestV9Lines];
    uint32_t values[A830SelfTestV9Lines];
    for(unsigned line=0;line<A830SelfTestV9Lines;++line) {dst[line]=page.iova+line*A830SelfTestLineBytes;values[line]=v9bNew(line);}
    static uint32_t words[A830SelfTestV9Lines*4+4];
    const size_t count=fillWords(dst,values,A830SelfTestV9Lines,words,sizeof words/sizeof words[0]);
    uint32_t seq=0;
    const IOReturn waited=fenced(words,count,&seq,nullptr,failure,A830S8StepV9b);
    uint8_t verdict=A830CohFail;
    if(waited==kIOReturnTimeout) *hung=true;
    else if(waited==kIOReturnSuccess) {
        barrier();
        for(unsigned line=0;line<A830SelfTestV9Lines;++line)if(page.cpu[line*stride]!=v9bNew(line))++result->v9b_mismatch;
        verdict=result->v9b_mismatch?A830CohFail:A830CohPass;
    }
    drop(&page,true);
    return verdict;
}

/* V9c: the 2D-engine pass into a WB+IS target, then the CACHE_CLEAN fence. The fill must have landed (after an invalidate)
 * for the pass to count: a blit that wrote nothing proves nothing. */
uint8_t A830SelfTest::checkV9c(A830CoherenceResult *result,A830SelfTestFailure *failure,bool *hung)
{
    A830SelfTestPage page;
    if(!mapped(&page,A830SelfTestMapCoherentRw))return A830CohFail;
    for(unsigned i=0;i<kPageWords;++i)page.cpu[i]=0;
    barrier();
    env_.vm.maintain(env_.vm.cookie,&page,A830SelfTestCleanInvalidate);
    uint32_t words[64];
    const size_t count=blitWords(page.iova,kPageBytes,kV9cValue,words,sizeof words/sizeof words[0]);
    uint32_t seq=0;
    const IOReturn waited=fenced(words,count,&seq,nullptr,failure,A830S8StepV9c);
    uint8_t verdict=A830CohFail;
    if(waited==kIOReturnTimeout) *hung=true;
    else if(waited==kIOReturnSuccess) {
        env_.vm.maintain(env_.vm.cookie,&page,A830SelfTestInvalidate);
        barrier();
        for(unsigned i=0;i<kPageWords;++i)if(page.cpu[i]!=kV9cValue)++result->v9c_mismatch;
        verdict=result->v9c_mismatch?A830CohFail:A830CohPass;
    }
    drop(&page,true);
    return verdict;
}

uint8_t A830SelfTest::limitOf(const A830CoherenceResult &result)
{
    if(result.v9a!=A830CohPass)return A830CoherenceOff;
    return result.v9b==A830CohPass && result.v9c==A830CohPass && !result.timed_out?A830CoherenceFull:A830CoherenceReadOnly;
}

uint8_t A830SelfTest::levelOf(const A830CoherenceResult &result,uint32_t coherent,bool coherent_given)
{
    const uint8_t limit=limitOf(result);
    return coherent_given && coherent<limit?uint8_t(coherent):limit;
}

IOReturn A830SelfTest::coherence(bool cohtest,A830CoherenceResult *result,A830SelfTestFailure *failure,bool *recovered)
{
    A830CoherenceResult local{};
    A830CoherenceResult &r=result?*result:local;
    A830SelfTestFailure none{};
    A830SelfTestFailure &f=failure?*failure:none;
    r={};
    if(recovered)*recovered=false;
    const A830SelfTestVm &vm=env_.vm;
    const bool usable=vm.allocate && vm.release && vm.map && vm.unmap && vm.maintain && env_.scratch && env_.submit &&
                      env_.fence && env_.irq;
    IOReturn outcome=kIOReturnSuccess;
    if(usable) {
        /* Each probe in order; a probe that hangs the GPU ends the series: recovered (M10) or S8 stops with reason 18. */
        struct Probe {uint8_t A830CoherenceResult::*field;uint16_t step;bool cohtest_only;
                      uint8_t (A830SelfTest::*check)(A830CoherenceResult *,A830SelfTestFailure *,bool *);};
        static const Probe probes[]={
            {&A830CoherenceResult::v4,A830S8StepV4,false,&A830SelfTest::checkV4},
            {&A830CoherenceResult::v9a,A830S8StepV9a,false,&A830SelfTest::checkV9a},
            {&A830CoherenceResult::v9b,A830S8StepV9b,true,&A830SelfTest::checkV9b},
            {&A830CoherenceResult::v9c,A830S8StepV9c,true,&A830SelfTest::checkV9c}};
        for(const Probe &probe:probes) {
            if(probe.cohtest_only && !cohtest)continue;
            bool hung=false;
            r.*probe.field=(this->*probe.check)(&r,&f,&hung);
            if(!hung)continue;
            r.*probe.field=A830CohFail;
            r.timed_out=true;
            /* The GPU did not finish the probe within 100 ms: the watchdog must not report it again later. */
            env_.fence->stopWatchdog();
            if(env_.recover && env_.recover(env_.recovery,A830ReasonHang)==kIOReturnSuccess) {
                if(recovered)*recovered=true;
                env_.fence->startWatchdog();
            } else {
                f={probe.step,A830ReasonHang,env_.scratch_iova,env_.fence->completed(),env_.fence->lastSubmitted()};
                outcome=kIOReturnTimeout;
            }
            break;
        }
    }
    r.tables_cacheable=r.v4==A830CohPass;
    r.limit=limitOf(r);
    r.level=levelOf(r,env_.coherent,env_.coherent_given);
    if(outcome==kIOReturnSuccess && vm.policy)vm.policy(vm.cookie,&r);
    return outcome;
}

void A830SelfTest::record(const A830SelfTestReport &report)
{
    Adreno830Status &s=adreno830_status;
    const A830CoherenceResult &c=report.coherence;
    s.aux[0]=report.readback|uint64_t(report.wake_us)<<32;
    s.aux[1]=report.irq_count;
    s.aux[2]=uint64_t(c.v4&3)|uint64_t(c.v9a&3)<<2|uint64_t(c.v9b&3)<<4|uint64_t(c.v9c&3)<<6;
    s.aux[3]=report.cp_counter;
    s.aux[4]=report.ao_counter;
    s.aux[5]=report.scratch|uint64_t(report.fence)<<32;
    s.coh_limit=c.limit;
}

IOReturn A830SelfTest::run(uint32_t gpu_intid,A830SelfTestReport *report,A830SelfTestFailure *failure)
{
    A830SelfTestReport local{};
    A830SelfTestReport &rep=report?*report:local;
    A830SelfTestFailure none{};
    A830SelfTestFailure &f=failure?*failure:none;
    rep={};f={};
    IOReturn result=kIOReturnSuccess;
    if(!env_.irq || !env_.fence || !env_.submit || !env_.scratch) {
        f={A830S8StepAttach,A830ReasonArgument,gpu_intid,0,0};
        result=kIOReturnBadArgument;
    }
    /* 1. the GPU interrupt, the only hard interrupt dependency (reason 19). */
    if(result==kIOReturnSuccess && !env_.irq->attached()) {
        const IOReturn attached=env_.irq->attach(gpu_intid);
        rep.attach_result=uint32_t(attached);
        if(attached!=kIOReturnSuccess) {f={A830S8StepAttach,A830ReasonIrqWiring,gpu_intid,uint32_t(attached),0};result=attached;}
    }
    if(env_.irq) {rep.readback=env_.irq->readback();rep.router=env_.irq->router();rep.irq_count=env_.irq->count();}
    /* 2. CP_MEM_WRITE with the IRQ fence. */
    if(result==kIOReturnSuccess)
        result=memWrite(env_.scratch_iova+A830SelfTestScratchMemWrite,env_.scratch+A830SelfTestScratchMemWrite/4,
                        A830SelfTestScratchValue,&rep,&f);
    /* 3. both always-on counters, record only (ARCHITECTURE 1.3 item 5, 9.2 item 5). */
    if(env_.irq)timestamps(&rep.cp_counter,&rep.ao_counter);
    /* 4, 5. coherence: record only, unless a probe hung the GPU. */
    if(result==kIOReturnSuccess)result=coherence(env_.cohtest,&rep.coherence,&f,&rep.recovered);
    else {rep.coherence.limit=A830CoherenceOff;rep.coherence.level=A830CoherenceOff;}
    if(env_.irq)rep.irq_count=env_.irq->count();
    record(rep);
    a830_selftest_last=rep;
    return result;
}
