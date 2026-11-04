/* SPDX-License-Identifier: MIT */
#include "A830Hfi.h"
#include "A830Profile.h"
#include "Adreno830Status.h"
#ifndef ADRENO830_HOST_TEST
#include <IOKit/IOLib.h>
#include <kern/clock.h>
#endif

namespace {
/* The shared region is Normal non-cacheable on both sides (CPU write-combined, GMU NC): queue data and indexes are
 * ordered with outer-shareable barriers, the doorbell (a Device write) after a dsb (4.7.5). */
inline void storeBarrier()
{
#if defined(__aarch64__)
    __asm__ volatile("dmb oshst" ::: "memory");
#else
    __atomic_thread_fence(__ATOMIC_RELEASE);
#endif
}
inline void loadBarrier()
{
#if defined(__aarch64__)
    __asm__ volatile("dmb oshld" ::: "memory");
#else
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
#endif
}
inline void fullBarrier()
{
#if defined(__aarch64__)
    __asm__ volatile("dmb osh" ::: "memory");
#else
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
#endif
}
inline void doorbellBarrier()
{
#if defined(__aarch64__)
    __asm__ volatile("dsb st" ::: "memory");
#else
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
#endif
}

constexpr uint32_t QueueDwords=A830_HFI_QUEUE_DWORDS;
constexpr uint32_t MaxPayload=A830HfiMaxPayload;   /* header + 254 = 255 dwords, the header's size field */
constexpr uint32_t KgslQueueHeaders=A830HfiKgslQueueHeaders;   /* 3 default + 14 dispatch queue headers */
constexpr uint32_t KgslDbgPriority=A830HfiKgslDbgPriority;
constexpr uint16_t StepTransport=0x10;             /* S5: the HFI itself (queue state, not a start message) */

uint32_t windowRead(void *context,uint32_t dword)
{
    return *reinterpret_cast<volatile const uint32_t *>(static_cast<uint8_t *>(context)+(4u*dword-A830HfiGmuWindowBias));
}
void windowWrite(void *context,uint32_t dword,uint32_t value)
{
    *reinterpret_cast<volatile uint32_t *>(static_cast<uint8_t *>(context)+(4u*dword-A830HfiGmuWindowBias))=value;
#if defined(__aarch64__)
    __asm__ volatile("dsb sy" ::: "memory");
#else
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
#endif
}

uint16_t clampStatus(uint32_t status) {return status>0xfffe?uint16_t(0xfffe):uint16_t(status);}

/* The bw bits of a vote without AB: KGSL's 0xFFFF0000 (design), nothing for annibale, whose votes are {1, level, bus}
 * (kernel-sequences 6.1 and section 14 row 24, K/adreno8_hfi.c:553,658). */
uint32_t noAbBits() {return adreno830_annibale()?0u:uint32_t(A830_HFI_BW_NO_AB);}

/* annibale's ACK contract (K/adreno8_hfi.c:193-220, kernel-sequences 5.3): 3 dwords at least (hdr, echo, status), 4 for
 * FEATURE_CTRL (hdr, echo, data, error). */
constexpr uint32_t AnnibaleAckDwords=3,AnnibaleFeatureAckDwords=4;
/* annibale's GMU2HOST bits cleared by every HFI poll: messages and debug queue (K/adreno8_hfi.c:124). */
constexpr uint32_t PollClear=A830_GMU2HOST_MSGQ|A830_GMU2HOST_DBGQ;
}

/* ---- set-up ---------------------------------------------------------------------------------------------------------- */

void A830HfiBindGmuWindow(A830HfiIo *io,volatile uint8_t *gmuWindow)
{
    if(!io || !gmuWindow)return;
    io->context=const_cast<uint8_t *>(gmuWindow);
    io->read=windowRead;
    io->write=windowWrite;
}

A830HfiStartOptions A830Hfi::startOptions(const Adreno830Board &board,uint8_t level,uint8_t bus,uint32_t hfiVersion)
{
    uint32_t flags=A830HfiStartDefault;
    if(board.knobs.acd)flags|=A830HfiStartAcd;
    if(board.knobs.hfi&2u)flags|=A830HfiStartDeferStart;
    return {flags,level,bus,A830_HFI_IFPC_HYSTERESIS,hfiVersion};
}

void A830Hfi::setIo(const A830HfiIo &io) {io_=io;}

void A830Hfi::layoutQueues()
{
    for(unsigned i=0;i<A830_HFI_QUEUE_COUNT;++i) {
        queue_[i].header=A830_HFI_QTBL_HDR_DWORDS+A830_HFI_QHDR_DWORDS*i;
        queue_[i].ring=(layout_==A830HfiLayoutKgsl?A830_HFI_KGSL_QUEUE_OFFSET(i):A830_HFI_QUEUE_OFFSET(i))/4;
    }
}

IOReturn A830Hfi::init(volatile uint32_t *shared,uint64_t iova) {return init(shared,iova,A830HfiLayoutLinux);}

IOReturn A830Hfi::init(volatile uint32_t *shared,uint64_t iova,A830HfiLayout layout)
{
    if(!shared || !io_.read || !io_.write || !io_.sleep || !io_.now || (iova&0xfff) ||
       iova+A830_GMU_HFI_BYTES>0x100000000ull || (layout!=A830HfiLayoutLinux && layout!=A830HfiLayoutKgsl))
        return kIOReturnBadArgument;
    lock();
    shared_=shared;iova_=iova;layout_=layout;
    layoutQueues();
    for(uint32_t i=0;i<A830_GMU_HFI_BYTES/4;++i)shared_[i]=0;
    /* Table header (Linux a6xx_hfi_init writes the size in bytes, KGSL init_queues in dwords; the firmware reads neither). */
    const bool kgsl=layout_==A830HfiLayoutKgsl;
    shared_[0]=0;
    shared_[1]=kgsl?A830_HFI_QTBL_HDR_DWORDS+KgslQueueHeaders*A830_HFI_QHDR_DWORDS:A830_HFI_QTBL_SIZE_BYTES;
    shared_[2]=A830_HFI_QTBL_HDR_DWORDS;
    shared_[3]=A830_HFI_QHDR_DWORDS;
    shared_[4]=A830_HFI_QUEUE_COUNT;
    shared_[5]=A830_HFI_QUEUE_COUNT;
    for(unsigned i=0;i<A830_HFI_QUEUE_COUNT;++i) {
        volatile uint32_t *h=shared_+queue_[i].header;
        const uint32_t priority=kgsl && i==A830_HFI_QUEUE_ID_DBG?KgslDbgPriority:A830_HFI_QUEUE_PRIORITY;
        h[A830_HFI_QHDR_STATUS]=A830_HFI_QUEUE_ENABLED;
        h[A830_HFI_QHDR_START_ADDR]=uint32_t(iova_)+queue_[i].ring*4;
        h[A830_HFI_QHDR_TYPE]=A830_HFI_QUEUE_TYPE(priority,i);
        h[A830_HFI_QHDR_QUEUE_SIZE]=QueueDwords;
        if(!kgsl) {   /* Linux a6xx_hfi_queue_init; KGSL leaves the watermarks and requests 0 */
            h[A830_HFI_QHDR_RX_WATERMARK]=1;
            h[A830_HFI_QHDR_TX_WATERMARK]=1;
            h[A830_HFI_QHDR_RX_REQUEST]=1;
        }
    }
    fullBarrier();
    ready_=true;
    first_ack_=false;
    unlock();
    return resetQueues();
}

IOReturn A830Hfi::resetQueues()
{
    if(!shared_)return kIOReturnNotReady;
    lock();
    for(unsigned i=0;i<A830_HFI_QUEUE_COUNT;++i) {
        volatile uint32_t *h=shared_+queue_[i].header;
        h[A830_HFI_QHDR_READ_INDEX]=0;
        h[A830_HFI_QHDR_WRITE_INDEX]=0;
    }
    fullBarrier();
    seq_=0;started_=false;ab_=false;level_=0;bus_=0;late_done_=false;
    for(unsigned m=0;m<A830HfiMessages;++m) {status_[m]=A830HfiStatusNone;headers_[m]=0;}
    failure_={};
    ready_=true;
    unlock();
    return kIOReturnSuccess;
}

void A830Hfi::stop()
{
    lock();
    started_=false;
    unlock();
}

bool A830Hfi::v5(uint8_t id) const
{
    return (id==A830_HFI_H2F_FEATURE_CTRL || id==A830_HFI_H2F_GET_VALUE || id==A830_HFI_H2F_SET_VALUE) &&
           A830_GMU_VER_MINOR(version_)>A830_GMU_HFI_V5_MINOR_ABOVE;
}

bool A830Hfi::pending(uint8_t *level,uint8_t *bus) const
{
    if(level)*level=pending_level_;
    if(bus)*bus=pending_bus_;
    return pending_;
}

IOReturn A830Hfi::fail(uint16_t step,uint16_t reason,IOReturn ior,uint32_t hdr,uint32_t value,uint32_t expect)
{
    failure_.step=step;failure_.reason=reason;failure_.ior=ior;
    failure_.hdr=hdr;failure_.value=value;failure_.expect=expect;
    ++stats_.errors;
    return ior;
}

/* ---- queues -------------------------------------------------------------------------------------------------------- */

/* One command into the cmd queue, padded to a multiple of 4 dwords with 0xFAFAFAFA, then the write index, then the doorbell
 * (KGSL gen8_hfi_queue_write / gen8_hfi_cmdq_write). Free space is (read - write - 1) mod size. */
IOReturn A830Hfi::push(uint32_t header,const uint32_t *payload,uint32_t dwords)
{
    volatile uint32_t *h=shared_+queue_[A830_HFI_QUEUE_ID_CMD].header;
    volatile uint32_t *ring=shared_+queue_[A830_HFI_QUEUE_ID_CMD].ring;
    uint32_t write=h[A830_HFI_QHDR_WRITE_INDEX];
    const uint32_t read=h[A830_HFI_QHDR_READ_INDEX];
    if(write>=QueueDwords || read>=QueueDwords || (write&3))
        return fail(StepTransport,A830ReasonUnexpectedState,kIOReturnIOError,header,write,read);
    const uint32_t total=dwords+1,aligned=(total+3)&~3u;
    if((read+QueueDwords-write-1)%QueueDwords<aligned) {
        ++stats_.queue_full;
        return fail(StepTransport,A830ReasonUnexpectedState,kIOReturnNoResources,header,write,read);
    }
    ring[write]=header;
    write=(write+1)%QueueDwords;
    for(uint32_t i=0;i<dwords;++i) {ring[write]=payload[i];write=(write+1)%QueueDwords;}
    for(uint32_t i=total;i<aligned;++i) {ring[write]=A830_HFI_FILL_WORD;write=(write+1)%QueueDwords;}
    storeBarrier();
    h[A830_HFI_QHDR_WRITE_INDEX]=write;
    doorbellBarrier();
    wr(A8XX_GMUCX_HOST2GMU_INTR_SET,A830_HOST2GMU_HFI_DOORBELL);
    ++stats_.sent;
    return kIOReturnSuccess;
}

/* One message out of a GMU-written queue: size from header [15:8], copied across the wrap, then
 * read = ALIGN(read + size, 4) mod size (KGSL gen8_hfi_queue_read). Messages longer than the buffer are skipped whole. */
int A830Hfi::pop(unsigned queue,Message *message)
{
    volatile uint32_t *h=shared_+queue_[queue].header;
    volatile const uint32_t *ring=shared_+queue_[queue].ring;
    uint32_t read=h[A830_HFI_QHDR_READ_INDEX];
    const uint32_t write=h[A830_HFI_QHDR_WRITE_INDEX];
    if(read==write)return 0;
    if(read>=QueueDwords || write>=QueueDwords)return -1;
    loadBarrier();
    const uint32_t first=ring[read],size=A830_HFI_HDR_DWORDS(first);
    const uint32_t available=(write+QueueDwords-read)%QueueDwords;
    if(!size || size>available)return -1;
    message->dwords=size<64?size:64;
    if(size>64)++stats_.malformed;
    for(uint32_t i=0;i<size;++i) {
        if(i<64)message->words[i]=ring[read];
        read=(read+1)%QueueDwords;
    }
    read=((read+3)&~3u)%QueueDwords;
    fullBarrier();
    h[A830_HFI_QHDR_READ_INDEX]=read;
    return 1;
}

/* ACKs and firmware requests (4.7.5). sent = the header waited for (0: none, a drain). */
A830Hfi::Outcome A830Hfi::handle(const Message &message,uint32_t sent,A830HfiAck *ack,uint16_t step)
{
    const uint32_t hdr=message.words[0];
    if(A830_HFI_HDR_TYPE(hdr)==A830_HFI_MSG_ACK) {
        ++stats_.acks;
        /* KGSL CMP_HFI_ACK_HDR: the whole request header, RECORD (bit 19) cleared on both sides. */
        if(sent && message.dwords>=2 && (message.words[1]&~A830_HFI_HDR_RECORD)==(sent&~A830_HFI_HDR_RECORD)) {
            if(ack) {
                *ack={};
                ack->dwords=uint8_t(message.dwords<32?message.dwords:32);
                for(unsigned i=0;i<ack->dwords;++i)ack->words[i]=message.words[i];
            }
            first_ack_=true;
            return Matched;
        }
        /* An answer to something else (a command that timed out earlier): recorded, the wait goes on. */
        ++stats_.stale_acks;
        stats_.last_unknown=message.dwords>=2?message.words[1]:hdr;
        return Continue;
    }
    switch(A830_HFI_HDR_ID(hdr)) {
    case A830_HFI_F2H_ERR:
        ++stats_.f2h_err;
        for(unsigned i=0;i<18;++i)stats_.last_err[i]=i<message.dwords?message.words[i]:0;
        failure_.f2h_id=A830_HFI_F2H_ERR;
        failure_.f2h_code=message.dwords>1?message.words[1]:0;
        for(unsigned i=0;i<16;++i)failure_.f2h_data[i]=i+2<message.dwords?message.words[i+2]:0;
        fail(step,A830ReasonF2hError,kIOReturnIOError,sent,hdr);
        return Fatal;
    case A830_HFI_F2H_DEBUG:
        ++stats_.f2h_debug;
        for(unsigned i=0;i<4;++i)stats_.last_debug[i]=i<message.dwords?message.words[i]:0;
        return Continue;
    case A830_HFI_F2H_LOG_BLOCK:
        ++stats_.f2h_log;
        return Continue;
    case A830_HFI_F2H_MEM_ALLOC:
    case A830_HFI_F2H_GMU_CNTR_REGISTER:
        /* Requests that need a host answer RB mode does not have: fail now instead of after the 1 s timeout. */
        ++stats_.f2h_resource;
        failure_.f2h_id=A830_HFI_HDR_ID(hdr);
        failure_.f2h_code=message.dwords>1?message.words[1]:0;
        for(unsigned i=0;i<16;++i)failure_.f2h_data[i]=i+2<message.dwords?message.words[i+2]:0;
        fail(step,A830ReasonGmuHostResource,kIOReturnUnsupported,sent,hdr);
        return Fatal;
    default:
        ++stats_.f2h_unknown;
        stats_.last_unknown=hdr;
        return Continue;
    }
}

/* Drains a GMU-written queue outside a command. A corrupt queue is skipped (read = write) and counted: it carries no
 * answer anyone waits for. */
IOReturn A830Hfi::drainLocked(unsigned queue,uint16_t step)
{
    IOReturn result=kIOReturnSuccess;
    Message message;
    int popped;
    while((popped=pop(queue,&message))>0) {
        if(queue==A830_HFI_QUEUE_ID_DBG)++stats_.dbg_messages;
        if(handle(message,0,nullptr,step)==Fatal && result==kIOReturnSuccess)result=failure_.ior;
    }
    if(popped<0) {
        ++stats_.malformed;
        volatile uint32_t *h=shared_+queue_[queue].header;
        const uint32_t write=h[A830_HFI_QHDR_WRITE_INDEX];
        if(write<QueueDwords)h[A830_HFI_QHDR_READ_INDEX]=write;
    }
    return result;
}

/* ---- one command ---------------------------------------------------------------------------------------------------- */

IOReturn A830Hfi::transact(uint8_t id,const uint32_t *payload,uint32_t dwords,A830HfiAck *ack,uint16_t step,
                           unsigned message,bool tolerate)
{
    bool pushed=false;
    const IOReturn result=exchange(id,payload,dwords,ack,step,message,tolerate,&pushed);
    /* annibale: a command that reached the queue and failed is followed by the late drain before the caller reports it
     * (field-notes 9-1). failure() is the command's own. */
    if(result!=kIOReturnSuccess && pushed && adreno830_annibale())(void)drainLateLocked(A830HfiLateDrainUs);
    return result;
}

IOReturn A830Hfi::exchange(uint8_t id,const uint32_t *payload,uint32_t dwords,A830HfiAck *ack,uint16_t step,
                           unsigned message,bool tolerate,bool *pushed)
{
    late_done_=false;
    if(!ready_ || !shared_)return fail(step,A830ReasonUnexpectedState,kIOReturnNotReady,0,0);
    if(dwords>MaxPayload || (dwords && !payload))return fail(step,A830ReasonArgument,kIOReturnBadArgument,id,dwords);
    const bool annibale=adreno830_annibale();
    seq_=(seq_+1)&0xfffu;
    const uint32_t header=A830_HFI_HDR(id,A830_HFI_MSG_CMD,dwords+1,seq_);
    if(message && message<A830HfiMessages)headers_[message]=header;
    *pushed=true;
    IOReturn result=push(header,payload,dwords);
    if(result!=kIOReturnSuccess)return result;
    A830HfiAck local;
    A830HfiAck *answer=ack?ack:&local;
    const uint64_t deadline=io_.now(io_.context)+A830_T_HFI_ACK_US;
    bool matched=false,bit23=false;
    for(;;) {
        const uint32_t info=rd(A8XX_GMUCX_GMU2HOST_INTR_INFO);
        stats_.last_info=info;
        if((info&A830_GMU2HOST_CM3_FAULT_LINUX) && !bit23) {bit23=true;++stats_.info_bit23;}   /* record only */
        if(info&A830_GMU2HOST_CM3_FAULT)return fail(step,A830ReasonGmuFault,kIOReturnIOError,header,info,0);
        if(io_.faulted && io_.faulted(io_.context))return fail(step,A830ReasonGmuFault,kIOReturnIOError,header,info,0);
        /* annibale polls with the GMU interrupts masked and clears the message and debug bits each round. */
        const uint32_t clear=info&(annibale?PollClear:uint32_t(A830_GMU2HOST_MSGQ));
        if(clear)wr(A8XX_GMUCX_GMU2HOST_INTR_CLR,clear);
        Message incoming;
        int popped;
        while((popped=pop(A830_HFI_QUEUE_ID_MSG,&incoming))>0) {
            const Outcome outcome=handle(incoming,header,answer,step);
            if(outcome==Fatal)return failure_.ior;
            if(outcome==Matched)matched=true;
        }
        if(popped<0) {
            volatile uint32_t *h=shared_+queue_[A830_HFI_QUEUE_ID_MSG].header;
            return fail(step,A830ReasonUnexpectedState,kIOReturnIOError,header,h[A830_HFI_QHDR_READ_INDEX],
                        h[A830_HFI_QHDR_WRITE_INDEX]);
        }
        if(matched)break;
        const uint32_t init=rd(A8XX_GMUCX_CM3_FW_INIT_RESULT);
        if(init&A830_GMU_FW_INIT_NMI_MASK)return fail(step,A830ReasonGmuNmi,kIOReturnIOError,header,init,0);
        if(io_.now(io_.context)>=deadline) {
            ++stats_.timeouts;
            return fail(step,A830ReasonTimeout,kIOReturnTimeout,header,info,0);
        }
        io_.sleep(io_.context,A830_POLL_STEP_US);
        ++stats_.polls;
    }
    /* v5 (FEATURE_CTRL/GET_VALUE/SET_VALUE, HFI minor > 4): word 2 the value, word 3 the error; else word 2 the error. */
    answer->v5=v5(id);
    if(answer->v5) {
        answer->value=answer->dwords>2?answer->words[2]:0;
        answer->error=answer->dwords>3?answer->words[3]:0;
    } else {
        answer->value=0;
        answer->error=answer->dwords>2?answer->words[2]:0;
    }
    /* KGSL treats a non-zero FEATURE_CTRL/SET_VALUE value as a failure too; GET_VALUE's value is the answer. */
    uint32_t status=answer->error?answer->error:(answer->v5 && id!=A830_HFI_H2F_GET_VALUE?answer->value:0);
    if(annibale) {
        /* annibale (K/adreno8_hfi.c:193-220, section 14 row 26): a short ACK fails; FEATURE_CTRL is read as data, error
         * whatever the HFI version and passes only with both 0 (K90: all five FEATURE_CTRL answered 0/0). */
        const uint32_t need=id==A830_HFI_H2F_FEATURE_CTRL?AnnibaleFeatureAckDwords:AnnibaleAckDwords;
        if(answer->dwords<need)return fail(step,A830ReasonHfiStatus,kIOReturnIOError,header,answer->dwords,need);
        if(id==A830_HFI_H2F_FEATURE_CTRL) {
            answer->v5=true;answer->value=answer->words[2];answer->error=answer->words[3];
            status=answer->error?answer->error:answer->value;
        }
    }
    if(message && message<A830HfiMessages)status_[message]=clampStatus(status);
    /* The dbg queue, once per command, in the thread that holds the HFI mutex. */
    const IOReturn drained=drainLocked(A830_HFI_QUEUE_ID_DBG,step);
    if(status && !tolerate)return fail(step,A830ReasonHfiStatus,kIOReturnIOError,header,status,0);
    return drained;
}

IOReturn A830Hfi::send(uint8_t id,const uint32_t *payload,uint8_t dwords,A830HfiAck *ack)
{
    lock();
    const IOReturn result=transact(id,payload,dwords,ack,id,0,false);
    if(result==kIOReturnSuccess && id==A830_HFI_H2F_PREPARE_SLUMBER)started_=false;
    unlock();
    return result;
}

/* ---- start (ARCHITECTURE 4.7.6) ----------------------------------------------------------------------------------- */

IOReturn A830Hfi::acdLocked(const A830VoteTables &tables)
{
    constexpr uint16_t step=StepTransport+A830HfiM3Acd;
    if(!tables.acd_levels || A830_HFI_HDR_ID(tables.acd[0])!=A830_HFI_H2F_ACD_TBL ||
       A830_HFI_HDR_DWORDS(tables.acd[0])!=A830_HFI_ACD_TABLE_DWORDS)
        return fail(step,A830ReasonArgument,kIOReturnBadArgument,tables.acd[0],tables.acd_levels);
    const uint32_t feature[3]={A830_HFI_FEATURE_ACD,1,0};
    IOReturn result=transact(A830_HFI_H2F_FEATURE_CTRL,feature,3,nullptr,step,0,false);
    if(result!=kIOReturnSuccess)return result;
    return transact(A830_HFI_H2F_ACD_TBL,tables.acd+1,A830_HFI_ACD_TABLE_DWORDS-1,nullptr,step,A830HfiM3Acd,false);
}

IOReturn A830Hfi::perfVoteLocked(uint8_t level,uint32_t bw,uint16_t step,unsigned message)
{
    const uint32_t payload[3]={A830_HFI_GX_BW_PERF_ACK_BLOCK,level,bw};
    const IOReturn result=transact(A830_HFI_H2F_GX_BW_PERF_VOTE,payload,3,nullptr,step,message,false);
    if(result!=kIOReturnSuccess) {++stats_.vote_failures;return result;}
    ++stats_.votes;
    if(level!=A830HfiNoVote)level_=level;
    if((bw&0xffu)!=A830HfiNoVote)bus_=uint8_t(bw&0xffu);
    pending_=false;
    return kIOReturnSuccess;
}

IOReturn A830Hfi::startLocked(const A830VoteTables &tables,const A830HfiStartOptions &options)
{
    if(!ready_ || !shared_)return fail(StepTransport,A830ReasonUnexpectedState,kIOReturnNotReady,0,0);
    for(unsigned m=0;m<A830HfiMessages;++m) {status_[m]=A830HfiStatusNone;headers_[m]=0;}
    failure_={};
    started_=false;ab_=false;
    /* The tables as A830Votes built them: right ids and sizes, an off level plus at least one GPU level. */
    if(tables.perf_dwords<5 || A830_HFI_HDR_ID(tables.perf[0])!=A830_HFI_H2F_TABLE ||
       A830_HFI_HDR_DWORDS(tables.perf[0])!=tables.perf_dwords || tables.gx_count<2 || tables.gx_count>A830VotesMaxGx ||
       A830_HFI_HDR_ID(tables.bw[0])!=A830_HFI_H2F_BW_VOTE_TBL ||
       A830_HFI_HDR_DWORDS(tables.bw[0])!=A830_HFI_BW_TABLE_DWORDS || tables.ddr_levels<2 ||
       tables.ddr_levels>A830VotesMaxDdr)
        return fail(StepTransport,A830ReasonArgument,kIOReturnBadArgument,tables.perf[0],tables.gx_count,tables.ddr_levels);
    if(options.level!=A830HfiNoVote && (!options.level || options.level>=tables.gx_count))
        return fail(StepTransport+A830HfiM9Vote,A830ReasonArgument,kIOReturnBadArgument,0,options.level,tables.gx_count);
    if(options.bus!=A830HfiNoVote && options.bus>=tables.ddr_levels)
        return fail(StepTransport+A830HfiM9Vote,A830ReasonArgument,kIOReturnBadArgument,0,options.bus,tables.ddr_levels);
    version_=options.hfi_version?options.hfi_version:rd(A8XX_GMU_DTCM_HFI_VERSION);
    const bool annibale=adreno830_annibale();
    /* HFI 2.6.1 and later take H2F_MSG_TABLE; older firmware would need PERF_TBL, which this driver never sends. annibale
     * also wants major version 2 (K/adreno8_hfi.c:355: "requires v2.6.1 or later v2"). */
    if(version_<A830_GMU_HFI_TABLE_FORMAT_MIN || (annibale && A830_GMU_VER_MAJOR(version_)!=A830_GMU_VER_MAJOR(A830_GMU_HFI_VERSION)))
        return fail(StepTransport,A830ReasonUnexpectedState,kIOReturnUnsupported,0,version_,A830_GMU_HFI_TABLE_FORMAT_MIN);
    gx_levels_=tables.gx_count;
    bus_levels_=tables.ddr_levels;
    top_kbps_=tables.ddr_kbps[tables.ddr_levels-1];

    IOReturn result;
    /* M1 perf table, M2 bandwidth table. */
    if((result=transact(A830_HFI_H2F_TABLE,tables.perf+1,tables.perf_dwords-1u,nullptr,StepTransport+A830HfiM1Perf,
                        A830HfiM1Perf,false))!=kIOReturnSuccess)
        return result;
    if((result=transact(A830_HFI_H2F_BW_VOTE_TBL,tables.bw+1,A830_HFI_BW_TABLE_DWORDS-1,nullptr,StepTransport+A830HfiM2Bw,
                        A830HfiM2Bw,false))!=kIOReturnSuccess)
        return result;
    /* M3 (P3): ACD, only after the AOP was told ACD is on (QMP val 1). */
    if((options.flags&A830HfiStartAcd) && (result=acdLocked(tables))!=kIOReturnSuccess)return result;
    /* M5: IFPC. P0 sends the off form explicitly (annibale ordinary mode); KGSL never sends it with IFPC off, so the
     * firmware default is off and a non-zero answer is only recorded. P2: on with the hysteresis, fatal on error.
     * annibale: the off form must be answered 0/0 as well (K/adreno8_hfi.c:717, section 14 row 26). */
    const bool ifpc=options.flags&A830HfiStartIfpc;
    const uint32_t feature[3]={A830_HFI_FEATURE_IFPC,ifpc?1u:0u,
                               ifpc?(options.ifpc_hysteresis?options.ifpc_hysteresis:A830_HFI_IFPC_HYSTERESIS):0u};
    if((result=transact(A830_HFI_H2F_FEATURE_CTRL,feature,3,nullptr,StepTransport+A830HfiM5Ifpc,A830HfiM5Ifpc,
                        !ifpc && !annibale))!=kIOReturnSuccess)
        return result;
    /* M6 CORE_FW_START {handle 0}. */
    const uint32_t handle=0;
    if((result=transact(A830_HFI_H2F_CORE_FW_START,&handle,1,nullptr,StepTransport+A830HfiM6CoreFwStart,
                        A830HfiM6CoreFwStart,false))!=kIOReturnSuccess)
        return result;
    started_=true;
    /* M7 (optional): does the GMU take AB votes? Its answer only enables them (KGSL first boot). annibale never asks
     * (kernel-sequences 5.5: no GET_VALUE; section 14 row 24: the first round drops M7), so its votes carry no AB. */
    if((options.flags&A830HfiStartQueryAb) && !annibale) {
        const uint32_t query[2]={A830_HFI_VALUE_GMU_AB_VOTE,0};
        A830HfiAck answer;
        result=transact(A830_HFI_H2F_GET_VALUE,query,2,&answer,StepTransport+A830HfiM7AbQuery,A830HfiM7AbQuery,true);
        if(result!=kIOReturnSuccess) {started_=false;return result;}
        ab_=answer.v5 && !answer.error && answer.value==1;
    }
    /* M8 START before the OOB (1.3 item 10), unless pagani-gpu-hfi bit1 moves it behind the OOB (sendStart). */
    if(!(options.flags&A830HfiStartDeferStart) &&
       (result=transact(A830_HFI_H2F_START,nullptr,0,nullptr,StepTransport+A830HfiM8Start,A830HfiM8Start,false))!=
       kIOReturnSuccess) {
        started_=false;
        return result;
    }
    /* M9 the initial GX/bus vote (no AB: KGSL's first default vote precedes the AB query). annibale: {1, 1, 2}, the bus
     * level alone in bw (kernel-sequences 6.1, K/adreno8_hfi.c:553; the level and bus come from its 160 MHz table row). */
    if(options.level!=A830HfiNoVote || options.bus!=A830HfiNoVote) {
        const uint8_t bus=options.bus!=A830HfiNoVote && options.bus>0?options.bus:uint8_t(A830HfiNoVote);
        result=perfVoteLocked(options.level,noAbBits()|bus,StepTransport+A830HfiM9Vote,A830HfiM9Vote);
        if(result!=kIOReturnSuccess) {started_=false;return result;}
    }
    return kIOReturnSuccess;
}

IOReturn A830Hfi::start(const A830VoteTables &tables,uint8_t init_level,uint8_t init_bus)
{
    const A830HfiStartOptions options={A830HfiStartDefault,init_level,init_bus,A830_HFI_IFPC_HYSTERESIS,0};
    return start(tables,options);
}

IOReturn A830Hfi::start(const A830VoteTables &tables,const A830HfiStartOptions &options)
{
    lock();
    const IOReturn result=startLocked(tables,options);
    unlock();
    return result;
}

IOReturn A830Hfi::sendStart()
{
    lock();
    const IOReturn result=transact(A830_HFI_H2F_START,nullptr,0,nullptr,StepTransport+A830HfiM8Start,A830HfiM8Start,false);
    unlock();
    return result;
}

/* ---- run time ------------------------------------------------------------------------------------------------------ */

IOReturn A830Hfi::voteLocked(uint8_t level,uint32_t bw)
{
    const uint8_t bus=uint8_t(bw&0xffu);
    if(!ready_ || !started_) {
        /* GMU in slumber (or not started yet): remember the target, the next start() sends it in M9 (4.16). */
        pending_=true;pending_level_=level;pending_bus_=bus;
        return kIOReturnSuccess;
    }
    /* GMU level 0 (XO) is never voted (KGSL gen8_gmu_dcvs_set). */
    if(!level || (level!=A830HfiNoVote && level>=gx_levels_))
        return fail(A830_HFI_H2F_GX_BW_PERF_VOTE,A830ReasonArgument,kIOReturnBadArgument,0,level,gx_levels_);
    /* A bus level outside (0, levels) means "unchanged", as KGSL does; AB bits only for a GMU that takes AB votes (never in
     * the annibale profile, whose bw is the bus level alone). */
    bw=(bw&~0xffu)|(bus>0 && bus<bus_levels_?bus:uint32_t(A830HfiNoVote));
    if(!ab_)bw=noAbBits()|(bw&0xffu);
    if(level==A830HfiNoVote && bw==(noAbBits()|A830HfiNoVote))return kIOReturnSuccess;   /* nothing to change */
    return perfVoteLocked(level,bw,A830_HFI_H2F_GX_BW_PERF_VOTE,0);
}

IOReturn A830Hfi::perfVote(uint8_t level,uint8_t bus) {return perfVote(level,bus,A830HfiNoAb);}

IOReturn A830Hfi::perfVote(uint8_t level,uint8_t bus,uint32_t ab_kbps)
{
    lock();
    const bool ab=ab_ && ab_kbps!=A830HfiNoAb;
    const IOReturn result=voteLocked(level,A830Votes::bwWord(bus,ab,ab?ab_kbps:0,top_kbps_));
    unlock();
    return result;
}

IOReturn A830Hfi::perfVoteBw(uint8_t level,uint32_t bw)
{
    lock();
    const IOReturn result=voteLocked(level,bw);
    unlock();
    return result;
}

IOReturn A830Hfi::prepareSlumber(uint8_t level,uint8_t bus)
{
    lock();
    IOReturn result;
    if(!ready_ || !started_) {
        result=fail(A830_HFI_H2F_PREPARE_SLUMBER,A830ReasonUnexpectedState,kIOReturnNotReady,0,0);
    } else if(!level || level>=gx_levels_ || !bus || bus>=bus_levels_) {
        result=fail(A830_HFI_H2F_PREPARE_SLUMBER,A830ReasonArgument,kIOReturnBadArgument,0,level|uint32_t(bus)<<8);
    } else {
        /* {hdr, bw, freq} at the default levels (KGSL gen8_gmu_notify_slumber); bw = bus | 0xFFFF0000 as ARCHITECTURE 4.17 P1
         * writes it: the slumber sequence drops every GMU vote, an AB release adds nothing. */
        const uint32_t payload[2]={A830_HFI_BW_NO_AB|bus,level};
        result=transact(A830_HFI_H2F_PREPARE_SLUMBER,payload,2,nullptr,A830_HFI_H2F_PREPARE_SLUMBER,0,false);
        if(result==kIOReturnSuccess)started_=false;
    }
    unlock();
    return result;
}

IOReturn A830Hfi::featureCtrl(uint32_t feature,uint32_t enable,uint32_t data,A830HfiAck *ack)
{
    const uint32_t payload[3]={feature,enable,data};
    lock();
    const IOReturn result=transact(A830_HFI_H2F_FEATURE_CTRL,payload,3,ack,A830_HFI_H2F_FEATURE_CTRL,0,false);
    unlock();
    return result;
}

IOReturn A830Hfi::sendAcd(const A830VoteTables &tables)
{
    lock();
    const IOReturn result=acdLocked(tables);
    unlock();
    return result;
}

IOReturn A830Hfi::getValue(uint32_t type,uint32_t subtype,uint32_t *value)
{
    const uint32_t payload[2]={type,subtype};
    A830HfiAck ack;
    lock();
    const IOReturn result=transact(A830_HFI_H2F_GET_VALUE,payload,2,&ack,A830_HFI_H2F_GET_VALUE,0,false);
    unlock();
    if(result==kIOReturnSuccess && value)*value=ack.v5?ack.value:(ack.dwords>2?ack.words[2]:0);
    return result;
}

IOReturn A830Hfi::setValue(uint32_t type,uint32_t subtype,uint32_t data)
{
    const uint32_t payload[3]={type,subtype,data};
    lock();
    const IOReturn result=transact(A830_HFI_H2F_SET_VALUE,payload,3,nullptr,A830_HFI_H2F_SET_VALUE,0,false);
    unlock();
    return result;
}

IOReturn A830Hfi::drainDebug()
{
    lock();
    const IOReturn result=ready_ && shared_?drainLocked(A830_HFI_QUEUE_ID_DBG,0):kIOReturnNotReady;
    unlock();
    return result;
}

/* ---- late drain (annibale) ------------------------------------------------------------------------------------------ */

/* annibale's late poll (K/redwood_gpu_dispatch.c:166-178 around adreno8_hfi_poll): rounds 100 ms apart; each one clears
 * GMU2HOST_INTR_INFO & (MSGQ | DBGQ), ends the drain at a CM3 fault or an NMI (nothing more will come), and empties the msg
 * and dbg queues. What it finds is recorded only; an F2H_MSG_ERR (the report waited for) or a corrupt queue ends it. */
uint32_t A830Hfi::drainLateLocked(uint32_t microseconds)
{
    if(!ready_ || !shared_ || late_done_)return 0;
    late_done_=true;
    ++stats_.late_drains;
    uint32_t drained=0;
    /* A round now and one after each 100 ms step: 31 rounds cover the 3 s. */
    const uint32_t steps=microseconds/A830HfiLateDrainStepUs;
    for(uint32_t round=0;;++round) {
        ++stats_.late_rounds;
        const uint32_t info=rd(A8XX_GMUCX_GMU2HOST_INTR_INFO);
        stats_.last_info=info;
        if(info&PollClear)wr(A8XX_GMUCX_GMU2HOST_INTR_CLR,info&PollClear);
        if((info&A830_GMU2HOST_CM3_FAULT) || (rd(A8XX_GMUCX_CM3_FW_INIT_RESULT)&A830_GMU_FW_INIT_NMI_MASK))break;
        bool stop=false;
        const unsigned queues[2]={A830_HFI_QUEUE_ID_MSG,A830_HFI_QUEUE_ID_DBG};
        for(unsigned queue:queues) {
            Message message;
            int popped;
            while((popped=pop(queue,&message))>0) {
                ++drained;++stats_.late_messages;
                const uint32_t hdr=message.words[0];
                if(A830_HFI_HDR_TYPE(hdr)==A830_HFI_MSG_ACK) {
                    /* An answer after the wait ended: the GMU reads these queues, it was only late. */
                    ++stats_.late_acks;first_ack_=true;
                } else if(A830_HFI_HDR_ID(hdr)==A830_HFI_F2H_ERR) {
                    ++stats_.late_errors;
                    for(unsigned i=0;i<18;++i)stats_.late_err[i]=i<message.dwords?message.words[i]:0;
                    stop=true;
                }
            }
            if(popped<0) {++stats_.malformed;stop=true;}
        }
        if(stop || round>=steps)break;
        io_.sleep(io_.context,A830HfiLateDrainStepUs);
    }
    return drained;
}

uint32_t A830Hfi::drainLate(uint32_t microseconds)
{
    if(!adreno830_annibale())return 0;
    lock();
    const uint32_t drained=drainLateLocked(microseconds);
    unlock();
    return drained;
}

/* ---- kernel services ------------------------------------------------------------------------------------------------ */

#ifndef ADRENO830_HOST_TEST
namespace {
IOLock *hfiLock;
void kernelSleep(void *,uint32_t microseconds) {IODelay(microseconds);}   /* blocks above the platform spin threshold */
uint64_t kernelNow(void *)
{
    uint64_t nanoseconds=0;
    absolutetime_to_nanoseconds(mach_absolute_time(),&nanoseconds);
    return nanoseconds/1000;
}
void kernelLock(void *) {IOLockLock(hfiLock);}
void kernelUnlock(void *) {IOLockUnlock(hfiLock);}
}

IOReturn A830HfiKernelServices(A830HfiIo *io)
{
    if(!io)return kIOReturnBadArgument;
    if(!__atomic_load_n(&hfiLock,__ATOMIC_ACQUIRE)) {
        IOLock *lock=IOLockAlloc();
        if(!lock)return kIOReturnNoMemory;
        IOLock *expected=nullptr;
        if(!__atomic_compare_exchange_n(&hfiLock,&expected,lock,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))IOLockFree(lock);
    }
    io->sleep=kernelSleep;
    io->now=kernelNow;
    io->lock=kernelLock;
    io->unlock=kernelUnlock;
    return kIOReturnSuccess;
}
#endif
