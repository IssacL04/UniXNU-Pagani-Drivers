/* SPDX-License-Identifier: MIT */
#ifndef A830_HFI_H
#define A830_HFI_H
#include <IOKit/IOTypes.h>
#include <stdint.h>
#include "A830Regs.h"
#include "A830Votes.h"

/* Host side of the GMU HFI (ARCHITECTURE 4.7.5-4.7.6, the perf vote of 4.16, the slumber/IFPC/ACD messages of 4.17):
 *  - the queue table and the cmd/msg/dbg queues in the 16 KiB shared region at GMU VA 0x60008000 (CPU write-combined,
 *    GMU non-cached), Linux layout (queues at +0x1000/+0x2000/+0x3000) or, only as a fallback when no first ACK ever
 *    arrives, the KGSL layout (848 + i * 4096);
 *  - one command in flight at a time under the HFI mutex (ARCHITECTURE 2.3: every HFI user, the start sequence, DCVS,
 *    slumber and the 336 bottom half, goes through it);
 *  - ACK matching on the whole request header with bit 19 (RECORD) cleared on both sides (KGSL CMP_HFI_ACK_HDR);
 *    FEATURE_CTRL/GET_VALUE/SET_VALUE answers in the v5 form when the HFI minor version is above 4 (word 2 the value,
 *    word 3 the error), every other answer with word 2 the error;
 *  - waits by sleeping 100 us per round, 1 s per command (KGSL HFI_RSP_TIMEOUT); each round reads CM3_FW_INIT_RESULT
 *    (bits 0xE00: NMI, fail at once) and GMU2HOST_INTR_INFO (bit 15 CM3 fault: fatal; bit 23 only recorded);
 *  - firmware requests: F2H_MSG_ERR fatal, F2H_MSG_DEBUG recorded, F2H_MSG_MEM_ALLOC (20) and F2H_MSG_GMU_CNTR_REGISTER
 *    (110) fail at once with reason 22 (RB mode has nothing to answer them with), anything else counted and ignored;
 *  - the dbg queue is drained only by the thread holding the HFI mutex: after every command and from the 336 bottom
 *    half (drainDebug). Interrupt context never touches a queue index.
 * The class owns no hardware window: register access, sleeping, time and the mutex come from A830HfiIo.
 *
 * The annibale profile (A830Profile.h, read at run time) follows what the REDMI K90 port proved with the same GMU firmware
 * where kernel-sequences section 14 says so:
 *  - start(): no M7 (row 24: annibale sends no GET_VALUE); M5 {IFPC,0,0} must be acknowledged with data 0 and error 0
 *    (row 26); the version must also be major 2 (5.2 step 5); M9 and every later GX_BW_PERF_VOTE carry the bus level alone
 *    in bw, without KGSL's 0xFFFF0000 (row 24, 6.1: the first vote is {1, 1, 2});
 *  - the matching ACK: at least 3 dwords, FEATURE_CTRL 4 (data, error), and FEATURE_CTRL fails unless both are 0 whatever
 *    the HFI version (5.3, K/adreno8_hfi.c:193-220); a shorter one fails with reason 15, value its dwords, expect the need;
 *  - each poll clears GMU2HOST_INTR_INFO & (MSGQ | DBGQ) (K/adreno8_hfi.c:124: the GMU interrupts stay masked);
 *  - after a failed command the msg and dbg queues are drained for 3 s more before the failure is returned (drainLate,
 *    field-notes 9-1).
 * The design profile keeps ARCHITECTURE 4.7.5-4.7.6 as written. */

/* Register access in GPU dword offsets (A8XX_GMUCX_*, A8XX_GMUAO_*, A8XX_GMU_DTCM_*; inside the board's gmu window at byte
 * offset 4 * dword - 0x37000, ARCHITECTURE 0.3), and the thread services. */
struct A830HfiIo {
    void *context;
    uint32_t (*read)(void *context,uint32_t dword);
    void (*write)(void *context,uint32_t dword,uint32_t value);   /* must complete the store (str + dsb sy) before returning */
    void (*sleep)(void *context,uint32_t microseconds);           /* blocks the calling thread at least this long */
    uint64_t (*now)(void *context);                               /* monotonic microseconds */
    void (*lock)(void *context);                                  /* the HFI mutex (a sleeping lock, never taken in IRQ) */
    void (*unlock)(void *context);
    bool (*faulted)(void *context);   /* optional: the 336/337 handlers saw a fatal GMU fault (CM3 bit 15, WDOG_BITE) */
};

enum A830HfiLayout : uint32_t {A830HfiLayoutLinux,A830HfiLayoutKgsl};

/* HFI layout facts (A830Regs.h section 21): the KGSL table holds 17 queue headers (848 = ALIGN(24 + 17*48, 16)), KGSL's
 * debug queue priority is 40 (the Linux layout uses 10 for all three queues), the header size field is 8 bits (255 dwords
 * with the header), and the gmu window byte offset of a GPU dword offset is 4 * dword - 0x37000 (ARCHITECTURE 0.3). */
enum : uint32_t {A830HfiKgslQueueHeaders=A830_HFI_KGSL_QTBL_HEADERS,A830HfiKgslDbgPriority=A830_HFI_KGSL_DBG_PRIORITY,
                 A830HfiMaxPayload=A830_HFI_MAX_PAYLOAD,A830HfiGmuWindowBias=A830_GMU_WIN_DELTA};

/* start() options (ARCHITECTURE 4.7.6, 4.17). */
enum A830HfiStartFlag : uint32_t {
    A830HfiStartAcd=1u<<0,          /* M3: FEATURE_CTRL{ACD,1,0} + ACD table (pagani-gpu-acd=1, after the QMP val 1 message) */
    A830HfiStartIfpc=1u<<1,         /* M5 P2 form FEATURE_CTRL{IFPC,1,hysteresis} (fatal on error); else {IFPC,0,0} recorded */
    A830HfiStartDeferStart=1u<<2,   /* pagani-gpu-hfi bit1: M8 (START) is left to sendStart() after the OOB (KGSL order) */
    A830HfiStartQueryAb=1u<<3,      /* M7: GET_VALUE(GMU_AB_VOTE); its answer only enables AB votes, never fails */
};
inline constexpr uint32_t A830HfiStartDefault=A830HfiStartQueryAb;   /* P0: IFPC off form, no ACD, START before OOB */

struct A830HfiStartOptions {
    uint32_t flags;             /* A830HfiStartFlag */
    uint8_t level,bus;          /* M9 GX_BW_PERF_VOTE: GMU level (1..levels) and DDR level; 0xFF = no vote */
    uint32_t ifpc_hysteresis;   /* A830HfiStartIfpc: 0x1680 (300 us of XO) */
    uint32_t hfi_version;       /* DTCM HFI version read at B5; 0 = read it here */
};

enum : uint16_t {
    A830HfiStatusNone=0xffff,   /* a message not sent or not answered (= A830HfiNotSent of Adreno830Status.h) */
};
enum : uint32_t {
    A830HfiNoAb=0xffffffffu,    /* perfVote: no AB vote (bw[31:16] = 0xFFFF; annibale profile: 0) */
    A830HfiNoVote=0xffu,        /* level or bus "unchanged" (KGSL INVALID_DCVS_IDX) */
};
/* annibale's late poll after a failure (K/redwood_gpu_dispatch.c:166-178; field-notes 9-1 and its timeout table): the
 * GMU's own report of a failure often arrives after the driver stopped waiting, so the queues are read every 100 ms for 3 s
 * more (a round now and one after each of 30 steps) before the failure is reported. */
enum : uint32_t {A830HfiLateDrainUs=3000000,A830HfiLateDrainStepUs=100000};
/* Start messages M1..M9 (ARCHITECTURE 4.7.6); S5 step = 0x10 + M. */
enum A830HfiMessage : uint8_t {
    A830HfiM1Perf=1,A830HfiM2Bw=2,A830HfiM3Acd=3,A830HfiM5Ifpc=5,A830HfiM6CoreFwStart=6,A830HfiM7AbQuery=7,A830HfiM8Start=8,
    A830HfiM9Vote=9,A830HfiMessages=10,
};

/* One answer. */
struct A830HfiAck {
    uint32_t words[32];         /* the ACK as read: hdr, request hdr, results */
    uint8_t dwords;
    bool v5;                    /* FEATURE_CTRL/GET_VALUE/SET_VALUE with HFI minor > 4 */
    uint32_t error;             /* v5: word 3; else word 2 */
    uint32_t value;             /* v5: word 2; else 0 */
};

/* What failed (A830Hfi::failure()); the orchestrator copies it into adreno830_status (reg = the HFI header). */
struct A830HfiFailure {
    uint16_t step;              /* S5 step 0x11..0x19 for M1..M9, 0x10 for the transport itself, else the message id */
    uint16_t reason;            /* A830Reason: 5 timeout, 6 queue state, 13 NMI, 14 CM3 fault, 15 ACK status, 16 F2H_MSG_ERR,
                                   22 host resource request */
    IOReturn ior;
    uint32_t hdr;               /* the request header */
    uint32_t value;             /* ACK error, FW_INIT_RESULT, GMU2HOST_INTR_INFO, or the F2H message header */
    uint32_t expect;
    uint32_t f2h_id,f2h_code;   /* the firmware message that failed it */
    uint32_t f2h_data[16];
};

struct A830HfiStats {
    uint32_t sent,acks,stale_acks,timeouts,errors;
    uint32_t f2h_err,f2h_debug,f2h_log,f2h_unknown,f2h_resource,malformed;
    uint32_t dbg_messages,polls,queue_full;
    uint32_t info_bit23;        /* GMU2HOST bit 23 seen (Linux CM3 fault bit, record only) */
    uint32_t last_info;         /* last GMU2HOST_INTR_INFO read */
    uint32_t last_debug[4];     /* last F2H_MSG_DEBUG: hdr, type, timestamp, data */
    uint32_t last_unknown;      /* header of the last unexpected message */
    uint32_t last_err[18];      /* last F2H_MSG_ERR: hdr, code, data[16] */
    uint32_t votes,vote_failures;
    /* drainLate (annibale profile): drains done, rounds polled, messages found (ACKs that came too late, F2H_MSG_ERR), and
     * the last late F2H_MSG_ERR (hdr, code, data[16]). */
    uint32_t late_drains,late_rounds,late_messages,late_acks,late_errors;
    uint32_t late_err[18];
};

class A830Hfi {
public:
    /* The start options the board's knobs ask for (ARCHITECTURE 3.6): pagani-gpu-acd=1 adds M3 (the caller has sent the
     * QMP val 1 message), pagani-gpu-hfi bit1 moves M8 behind the OOB; M7 is always asked; IFPC stays off (P0). */
    static A830HfiStartOptions startOptions(const Adreno830Board &board,uint8_t level,uint8_t bus,uint32_t hfiVersion);

    /* Builds the queue table in `shared` (16 KiB at GMU VA `iova`) and clears every index. Only while the GMU is held in
     * reset (before B1), never while it runs. io must be set first (it supplies the lock). */
    IOReturn init(volatile uint32_t *shared,uint64_t iova);                               /* Linux layout */
    IOReturn init(volatile uint32_t *shared,uint64_t iova,A830HfiLayout layout);
    void setIo(const A830HfiIo &io);
    /* Before every GMU boot after the first (slumber wake, recovery, a cold-boot retry), GMU in reset: indexes to 0,
     * sequence numbers from 1 again, the started state cleared. */
    IOReturn resetQueues();
    /* The GMU is going down (forced off, slumber done): no message until the next resetQueues()/start(). */
    void stop();

    /* One command: payload excludes the header (dwords <= 254); waits for the matching ACK. ack may be null. On an ACK
     * whose status is non-zero the answer is still returned and the result is kIOReturnIOError (reason 15). An acknowledged
     * PREPARE_SLUMBER sent this way stops the HFI as prepareSlumber() does (A830PowerState sends it through here). */
    IOReturn send(uint8_t id,const uint32_t *payload,uint8_t dwords,A830HfiAck *ack);

    /* The start messages after B8 (ARCHITECTURE 4.7.6). P0 order: M1, M2, M5 {IFPC,0,0}, M6, M7, M8, M9; M3 between M2 and
     * M5 with A830HfiStartAcd; M8 left out with A830HfiStartDeferStart. Every ACK must be 0 except M5's off form and M7.
     * annibale profile: M1, M2, M5, M6, M8, M9 (A830HfiStartQueryAb is ignored, row 24), M5 strict, M9 bw = bus alone. */
    IOReturn start(const A830VoteTables &tables,uint8_t init_level,uint8_t init_bus);     /* A830HfiStartDefault */
    IOReturn start(const A830VoteTables &tables,const A830HfiStartOptions &options);
    IOReturn sendStart();                                                                  /* M8 alone */

    /* GX_BW_PERF_VOTE {ack_type 1, freq, bw} (4.16, KGSL gen8_gmu_dcvs_set): level 1..levels or 0xFF (unchanged),
     * bus 1..ddr_levels-1 or 0xFF; level 0 is refused (XO); nothing is sent when both are unchanged. While the HFI is
     * not started (slumber) the vote is only remembered (pending()) and kIOReturnSuccess returned. Without AB the bw word
     * is bus | 0xFFFF0000 (design) or the bus alone (annibale, K/adreno8_hfi.c:553,658: {1, level, bus}). */
    IOReturn perfVote(uint8_t level,uint8_t bus);
    IOReturn perfVote(uint8_t level,uint8_t bus,uint32_t ab_kbps);   /* ab_kbps: A830HfiNoAb = no AB vote */
    /* The same with the bw word built by the caller (A830Dcvs: A830Votes::bwWord): bits [7:0] the bus level (out of range =
     * unchanged), [15:8] the AB enable, [31:16] the quantized AB or 0xFFFF; AB bits are dropped unless the GMU takes AB. */
    IOReturn perfVoteBw(uint8_t level,uint32_t bw);
    /* PREPARE_SLUMBER {bw = bus | 0xFFFF0000, freq = level} (4.17 P1 step S5): level 1..levels, bus 1..ddr_levels-1; the HFI
     * counts as stopped once the GMU acknowledged it. */
    IOReturn prepareSlumber(uint8_t level,uint8_t bus);
    /* FEATURE_CTRL{feature, enable, data}, v5 answer. */
    IOReturn featureCtrl(uint32_t feature,uint32_t enable,uint32_t data,A830HfiAck *ack);
    /* M3: FEATURE_CTRL{ACD,1,0} then the ACD table (ARCHITECTURE 4.17 P3). */
    IOReturn sendAcd(const A830VoteTables &tables);
    /* GET_VALUE / SET_VALUE (v5). */
    IOReturn getValue(uint32_t type,uint32_t subtype,uint32_t *value);
    IOReturn setValue(uint32_t type,uint32_t subtype,uint32_t data);

    /* The 336 bottom half (A830Gmu::gmuWork): takes the HFI mutex and drains the dbg queue. An F2H_MSG_ERR or a host
     * resource request found there returns an error (the caller starts the recovery). */
    IOReturn drainDebug();
    /* annibale profile (field-notes 9-1 and P1 item 7, K/redwood_gpu_dispatch.c:166-178): after a failure or a hang verdict,
     * before it is reported, the msg and dbg queues are read every 100 ms for `microseconds` (default 3 s). Each round
     * clears GMU2HOST_INTR_INFO & (MSGQ | DBGQ) and stops at a CM3 fault or an NMI; every message is recorded in the late_*
     * statistics only (failure() keeps the original cause; a late ACK still proves the queue layout, firstAckSeen()); an
     * F2H_MSG_ERR (the report waited for) or a corrupt queue ends it. A failed command already drained this way, so the
     * call is a no-op until the next command or resetQueues(). Takes the HFI mutex. Design profile: nothing, 0.
     * Returns the number of messages drained. */
    uint32_t drainLate(uint32_t microseconds=A830HfiLateDrainUs);

    bool started() const {return started_;}
    bool abSupported() const {return ab_;}
    bool firstAckSeen() const {return first_ack_;}   /* false after a timeout of the very first command: try the KGSL layout */
    A830HfiLayout layout() const {return layout_;}
    uint32_t version() const {return version_;}
    uint16_t status(unsigned message) const {return message<A830HfiMessages?status_[message]:A830HfiStatusNone;}
    uint32_t header(unsigned message) const {return message<A830HfiMessages?headers_[message]:0;}
    const A830HfiFailure &failure() const {return failure_;}
    const A830HfiStats &stats() const {return stats_;}
    /* Last vote the GMU acknowledged (level 0: none yet) and the one remembered while stopped. */
    uint8_t currentLevel() const {return level_;}
    uint8_t currentBus() const {return bus_;}
    bool pending(uint8_t *level,uint8_t *bus) const;

private:
    struct Queue {uint32_t header,ring;};     /* dword index of the queue header and of the ring in the shared region */
    struct Message {uint32_t words[64];uint32_t dwords;};
    enum Outcome {Continue,Matched,Fatal};

    IOReturn transact(uint8_t id,const uint32_t *payload,uint32_t dwords,A830HfiAck *ack,uint16_t step,unsigned message,
                      bool tolerate);
    IOReturn exchange(uint8_t id,const uint32_t *payload,uint32_t dwords,A830HfiAck *ack,uint16_t step,unsigned message,
                      bool tolerate,bool *pushed);   /* transact without the annibale late drain */
    uint32_t drainLateLocked(uint32_t microseconds);
    IOReturn startLocked(const A830VoteTables &tables,const A830HfiStartOptions &options);
    IOReturn acdLocked(const A830VoteTables &tables);
    IOReturn perfVoteLocked(uint8_t level,uint32_t bw,uint16_t step,unsigned message);
    IOReturn voteLocked(uint8_t level,uint32_t bw);   /* perfVote/perfVoteBw: pending while stopped, checks, unchanged */
    IOReturn push(uint32_t header,const uint32_t *payload,uint32_t dwords);
    int pop(unsigned queue,Message *message);          /* 1 message, 0 empty, -1 corrupt */
    Outcome handle(const Message &message,uint32_t sent,A830HfiAck *ack,uint16_t step);
    IOReturn drainLocked(unsigned queue,uint16_t step);
    IOReturn fail(uint16_t step,uint16_t reason,IOReturn ior,uint32_t hdr,uint32_t value,uint32_t expect=0);
    uint32_t rd(uint32_t dword) const {return io_.read(io_.context,dword);}
    void wr(uint32_t dword,uint32_t value) const {io_.write(io_.context,dword,value);}
    bool v5(uint8_t id) const;
    void lock() const {if(io_.lock)io_.lock(io_.context);}
    void unlock() const {if(io_.unlock)io_.unlock(io_.context);}
    void layoutQueues();

    A830HfiIo io_{};
    volatile uint32_t *shared_=nullptr;
    uint64_t iova_=0;
    A830HfiLayout layout_=A830HfiLayoutLinux;
    Queue queue_[A830_HFI_QUEUE_COUNT]{};
    uint32_t seq_=0;
    uint32_t version_=0;
    bool ready_=false,started_=false,ab_=false,first_ack_=false,pending_=false;
    bool late_done_=false;    /* the last failure was drained already (drainLate) */
    uint8_t level_=0,bus_=0,pending_level_=0,pending_bus_=0;
    uint32_t gx_levels_=0;    /* GX entries of the tables start() sent (level bound of perfVote) */
    uint32_t bus_levels_=0;   /* DDR levels (bus bound) */
    uint32_t top_kbps_=0;     /* the top DDR level (AB quantization) */
    uint16_t status_[A830HfiMessages]{};
    uint32_t headers_[A830HfiMessages]{};
    A830HfiFailure failure_{};
    A830HfiStats stats_{};
};

/* read/write of io through the board's mapped gmu window (Device-nGnRE; every write followed by dsb sy). */
void A830HfiBindGmuWindow(A830HfiIo *io,volatile uint8_t *gmuWindow);

#ifndef ADRENO830_HOST_TEST
/* The kernel thread services for A830HfiIo (sleep: IODelay, which blocks above the spin threshold; now:
 * mach_absolute_time; lock/unlock: one IOLock allocated on first use, never freed). read/write/faulted stay the caller's.
 * Returns kIOReturnNoMemory when the IOLock cannot be allocated. */
IOReturn A830HfiKernelServices(A830HfiIo *io);
#endif
#endif
