/* SPDX-License-Identifier: MIT */
#ifndef A830_SMMU_H
#define A830_SMMU_H
#include "NativeAdrenoSmmuContext.h"
#include "Adreno830Status.h"
#include "Sm8750Scm.h"
#include "A830Profile.h"

/* The A830 GPU SMMU (ARM MMU-500 at 0x3DA0000, ARCHITECTURE 4.5, 4.12): the read-only survey of S3, the take-over of the GMU
 * bank (CB2, slot 1, SID 5) and the GPU bank (CB0, slot 0, SID 0, LPAC SID 1 on slot 3), the context-fault interrupts, the TLB
 * maintenance the VM's table transactions issue, and release, save and restore.
 *
 * The global register space is emulated by the hypervisor on SM8750 (measured on this phone: S2CR reads 0x03000018 at boot,
 * sCR0 writes are ignored, CB registers hold random values at boot). So only functional bits gate progress: the S2CR type and
 * bank of our routes, our SMRs, SCTLR (written 0 reads 0; enabled reads back what was written, ASIDPNE dropped), the context
 * image under the tolerance rules of 4.5.4 (TCR2.TBI0/TBI1 may read 1, TCR.EPD1 may be cleared once the CP ran), CLIENTPD.
 * ACTLR, sACR, sGFSR and the context FSRs are read, written back where the profile says so, and recorded; they never fail a
 * step.
 *
 * Profiles (A830Profile.h, read once by init()):
 *  - annibale (default): what the REDMI K90 port proved on the same SoC (kernel-sequences 7.4-7.6, section 14 rows 9, 11, 12,
 *    13). Both banks get SCTLR 0x27 (no CFIE, HUPCF or ASIDPNE), TCR 0x00803519 (T0SZ 25, EPD1, no TTBR1 walk fields),
 *    TTBR1 0 and MAIR0 0xF404FF44; ACTLR is written on the GPU bank only; every TLB invalidation is a 32-bit TLBIASID of
 *    the bank's ASID followed by TLBSYNC (also for the VM's transactions, see tableIo); sACR is never written and sGFSR
 *    never written back; 710/712 are not attached (poll() reads the FSRs). The ASID stays CB + 1 (row 11).
 *  - design (0): ARCHITECTURE 4.5.3-4.5.7 as written: SCTLR 0x1167, the kit's TCR (CB0 with the TTBR1 guard fields),
 *    MAIR0 0xFF44, ACTLR on both banks, TLBIALL, sACR.CACHE_LOCK cleared, sGFSR written back, 710/712 attached.
 *
 * Register access: every access goes through the injected ArmSmmu4KIo; on the device A830SmmuBindMmio binds out-of-line
 * helpers that keep every value (zero included) in a general register (never WZR/XZR, which the emulation refuses) and use one
 * 64-bit store for a 64-bit register: TLBIVA/TLBIVAA/TLBIVAL/TLBIVAAL must be single 64-bit stores (the first half of a split
 * store would already invalidate with ASID 0), TLBIASID/TLBIALL/TLBSYNC/SCTLR/FSR are 32-bit registers (ARCHITECTURE 4.5.2). */

/* The context values of the annibale profile, the same for both banks (kernel-sequences 7.4 steps 9, 12, 14, 15). */
enum : uint32_t {
    A830SmmuAnnibaleSctlr=0x27u,        /* M | TRE | AFE | CFRE: faults are polled */
    A830SmmuAnnibaleTcr=0x00803519u,    /* T0SZ 25, IRGN0/ORGN0 WBWA, SH0 inner, TG0 4K, EPD1; T1SZ/TG1 0 */
    A830SmmuAnnibaleMair0=0xF404FF44u,  /* index 0 NC 0x44 and 1 WBRWA 0xFF as in the design, 2 device 0x04, 3 0xF4 */
    A830SmmuAnnibaleTlbSync=1u,         /* the TLBSYNC value K90 wrote */
};

struct A830SmmuStream {uint8_t slot,cb;uint16_t sid;};

/* The survey (ARCHITECTURE 4.5.3, read-only; IMPLEMENTATION_PLAN M1). */
enum : unsigned {A830SmmuMaxSmrs=24,A830SmmuSurveyBanks=4};
enum : uint32_t {
    A830SmmuWarnUsfcfg=1u<<0,          /* sCR0.USFCFG = 0: unmatched streams bypass instead of faulting (warning only) */
    A830SmmuWarnCacheLock=1u<<1,       /* sACR.CACHE_LOCK set at boot (design: releaseCacheLock clears it, ARCHITECTURE 1.3 #2) */
    A830SmmuWarnGlobalFault=1u<<2,     /* sGFSR non-zero at boot (record only) */
    A830SmmuWarnSlotMoved=1u<<3,       /* a preferred slot's SMR was valid: the stream moved to the lowest free slot */
    A830SmmuWarnBankEnabled=1u<<4,     /* CB0 or CB2 SCTLR.M was set at boot (random reset values; resetBank clears it) */
    A830SmmuWarnSmrs=1u<<5,            /* more than A830SmmuMaxSmrs stream-match groups: the rest were scanned, not stored */
};
struct A830SmmuSurvey {
    uint32_t idr0,idr1,idr2,scr0,sacr,sgfsr,smr[A830SmmuMaxSmrs],s2cr[A830SmmuMaxSmrs];
    uint32_t cb_sctlr[A830SmmuSurveyBanks],cb_actlr[A830SmmuSurveyBanks],cb_tcr[A830SmmuSurveyBanks],cb_fsr[A830SmmuSurveyBanks];
    uint64_t cb_ttbr0[A830SmmuSurveyBanks];
    /* Derived from the IDs: stream-match groups, context banks, global pages (2 << NUMPAGENDXB), output bits (IDR2.OAS, the
     * value Query reports; NativeAdrenoGpuContext caps allocations at 40). */
    uint32_t smrs,banks,global_pages,output_bits;
    uint8_t slot_gpu,slot_lpac,slot_gmu;    /* chosen per step 5: the board's preferred slots, else the lowest invalid SMR (not 2) */
    uint8_t reserved;
    uint32_t warnings;                      /* A830SmmuWarn* */
};
/* ARCHITECTURE 4.5.4 step 14, as read back (record) with the fields outside the tolerance rules in mismatch (the bits of
 * NativeAdrenoSmmuProgramReport::mismatch). */
struct A830SmmuReadback {
    uint32_t sctlr,actlr,cbar,cba2r,tcr2,tcr,mair0,mair1,fsr;
    uint32_t mismatch;
    uint64_t ttbr0,ttbr1;
};
/* A failed step (ARCHITECTURE 8.3): the step number of 4.5.3/4.5.4 (bank reset 0x0F; GPU bank + 0x100, GMU bank + 0x200,
 * LPAC + 0x300; TLB sync 0x0C, enable 0x0D), the A830Reason, the physical register and the value read / expected. */
struct A830SmmuFailure {uint16_t step,reason;uint64_t reg,value,expect;};
/* One context fault (ARCHITECTURE 4.12.3), recorded by contextIrq or poll and drained by the bottom half. */
struct A830SmmuFault {
    uint64_t far;
    uint32_t fsr,fsynr0,fsynr1;
    uint16_t sid;                     /* CBFRSYNRA[15:0] */
    uint8_t cb;
    uint8_t polled;                   /* 1: found by poll(), not by the interrupt */
    uint8_t sampled;                  /* 1: the interrupt read the ring's fences below before anything else (platform.fences) */
    uint32_t completed,issued;        /* the completed fence and the last one issued then: the jobs in flight at the fault (M14 P0) */
};
enum : unsigned {A830SmmuFaultRing=32};
/* One fault in a log line (the bottom half's sink, the first A830SmmuFaultLines of a boot of the GPU's bank and of the others', adreno830_status_diag_lines:
 * nothing in a log counts CB2's faults, so whether it has harmless ones is unknown and they must not use up the lines): the pf counts of the meter line
 * cannot tell a high address word lost from a mapping past its window or a page never mapped, FAR does, and without it a fault past 4 GiB would take
 * another boot. "gpu fault cb<n> far 0x<far> fsr 0x<fsr> syn 0x<fsynr0> 0x<fsynr1> sid 0x<sid>", at most 90 characters. */
enum : unsigned {A830SmmuFaultLines=8,A830SmmuFaultLineBytes=92};
static inline void A830SmmuFaultLine(char *line,const A830SmmuFault &fault)
{
    unsigned at=0;
    const auto put=[&](char c) {if(at+1<A830SmmuFaultLineBytes)line[at++]=c;};
    const auto text=[&](const char *s) {while(*s)put(*s++);};
    const auto hex=[&](uint64_t value) {
        text(" 0x");
        unsigned shift=60;
        while(shift && !(value>>shift))shift-=4;
        for(;;shift-=4) {
            put("0123456789abcdef"[(value>>shift)&15]);
            if(!shift)break;
        }
    };
    text("gpu fault cb");
    if(fault.cb>=100)put(char('0'+fault.cb/100));
    if(fault.cb>=10)put(char('0'+fault.cb/10%10));
    put(char('0'+fault.cb%10));
    text(" far");hex(fault.far);
    text(" fsr");hex(fault.fsr);
    text(" syn");hex(fault.fsynr0);hex(fault.fsynr1);
    text(" sid");hex(fault.sid);
    line[at]=0;
}
/* Steps of 4.5.3/4.5.4 the failure record and the status page use. */
enum : uint16_t {
    A830SmmuStepIds=1,A830SmmuStepScr0=2,A830SmmuStepSacr=3,A830SmmuStepGlobalFault=4,A830SmmuStepSlots=5,A830SmmuStepBanks=6,
    A830SmmuStepClaimS2cr=1,A830SmmuStepClaimSmr=2,A830SmmuStepSctlrOff=3,A830SmmuStepActlr=4,A830SmmuStepCbar=5,
    A830SmmuStepTcr2=6,A830SmmuStepTcr=7,A830SmmuStepTtbr0=8,A830SmmuStepTtbr1=9,A830SmmuStepMair=10,A830SmmuStepFsr=11,
    A830SmmuStepTlb=12,A830SmmuStepEnable=13,A830SmmuStepReadback=14,A830SmmuStepRoute=15,A830SmmuStepRouteFsr=16,
    A830SmmuStepReset=0x0F,A830SmmuStepGpu=0x100,A830SmmuStepGmu=0x200,A830SmmuStepLpac=0x300,
    A830SmmuStepVerify=0x20,A830SmmuStepRelease=0x30,A830SmmuStepRestore=0x40,
};
/* The S3 diagnostic page (ARCHITECTURE 8.4) from fillStatus: aux words while stage == A830SSmmu.
 *   aux[0] IDR0 | IDR1 << 32       aux[1] IDR2 | sCR0 << 32        aux[2] sACR | sGFSR (survey) << 32
 *   aux[3] S2CR of the GPU slot | (slot gpu | lpac << 8 | gmu << 16) << 32
 *   aux[4] CB0 SCTLR | CB0 ACTLR << 32                             aux[5] CB2 SCTLR | CB2 ACTLR << 32
 *   smmu_slots: gpu | lpac << 8 | gmu << 16; sticky[0] sGFSR, sticky[1] CB0 FSR, sticky[2] CB2 FSR (last non-zero);
 *   smmu_faults counts faults of enabled banks; irq_off bit 3 = 710 (CB0), bit 4 = 712 (CB2). */
enum : uint32_t {A830SmmuIrqOffGpu=1u<<3,A830SmmuIrqOffGmu=1u<<4};

/* Platform services, injected (host models drive every sequence): the kernel binding is A830SmmuKernelPlatform. */
struct A830SmmuPlatform {
    void *cookie;
    uint64_t (*now_us)(void *cookie);                  /* monotonic */
    void (*delay_us)(void *cookie,uint32_t microseconds);   /* busy wait (TLB sync back-off; interrupts may be off) */
    /* PaganiArmCpu SPI interface (ARCHITECTURE 4.12.1); any may be null: the interrupt is then not wired and the banks are polled. */
    IOReturn (*spi_attach)(uint32_t intid,bool edge,void (*handler)(void *),void *context,uint32_t *readback,uint64_t *router);
    IOReturn (*spi_disable)(uint32_t intid);
    IOReturn (*spi_enable)(uint32_t intid);
    /* Called from the interrupt handler after a fault was recorded: thread_call_enter of the fault bottom half (may be null). */
    void (*fault_notify)(void *cookie);
    /* CP_SMMU_APERTURE_ID for a bank (Sm8750ScmSetGpuSmmuAperture); x0 out. Null: grantAperture records "not run". */
    int (*aperture)(void *cookie,uint32_t context_bank,int64_t *x0);
    /* Optional (M14 P0): the ring's completed fence (memptrs + 4) and the last fence issued, read lock-free at the top of contextIrq,
     * so the bottom half can name the jobs that were in flight at the fault. Interrupt context: loads only. Null: faults carry none. */
    void (*fences)(void *cookie,uint32_t *completed,uint32_t *issued);
};
struct A830SmmuConfig {
    const ArmSmmu4KIo *io;            /* the whole 0x40000-byte window */
    uint64_t phys;                    /* board smmu.phys, for failure records */
    A830SmmuPlatform platform;
    uint32_t actlr;                   /* knobs.actlr (pagani-gpu-actlr, default 0x303) */
    uint8_t cb_gpu,cb_gmu;            /* 0, 2 */
    uint8_t slot_gpu,slot_lpac,slot_gmu;   /* preferred 0, 3, 1 */
    uint16_t sid_gpu,sid_lpac,sid_gmu;     /* 0, 1, 5 */
    uint32_t irq_cb[4],irq_global;    /* 710..713, 706 */
    Adreno830Status *status;          /* sticky, smmu_faults, irq_off; may be null (host tests) */
};

class A830Smmu final {
public:
    enum : unsigned {MaxBanks=8,MaxStreams=8,StormLimit=64,SyncSpin=10,SyncBudgetUs=10000,SyncBackoffMaxUs=1024};
    struct Bank {                     /* one context bank this driver owns; the interrupt context of contextIrq */
        A830Smmu *owner;
        uint32_t intid;
        uint8_t cb;
        bool reset,programmed,enabled,irq_attached,irq_off;
        bool actlr_written;           /* false: the annibale profile left this bank's ACTLR as found (read, recorded) */
        uint32_t no_progress,irq_readback;
        uint32_t actlr,actlr_read,fsr_boot;
        ArmSmmu4KContextImage image;  /* what programContext wrote (TTBR1/TCR included) */
        uint64_t irqs,faults;
    };
private:
    A830SmmuConfig config_{};
    ArmSmmu4KIo io_{};                /* config_.io, copied */
    ArmSmmu4KIo table_io_{};          /* the VM's TLB I/O: io_ plus the TLB sync budget */
    NativeAdrenoSmmuProgrammer programmer_{};
    A830SmmuSurvey survey_{};
    A830SmmuFailure failure_{};
    Bank banks_[MaxBanks]{};
    A830SmmuStream streams_[MaxStreams]{};
    bool claimed_[MaxStreams]{},routed_[MaxStreams]{};
    size_t stream_count_=0;
    A830SmmuFault faults_[A830SmmuFaultRing]{};
    uint32_t fault_seq_[A830SmmuFaultRing]{};   /* index + 1 once faults_[index % ring] is written (lock-free ring) */
    uint32_t fault_head_=0,fault_tail_=0,fault_lost_=0;
    mutable uint32_t writes_=0;
    /* The TLB sync budget of the VMs' transactions, per bank (the GMU and GPU VMs may synchronize at the same time). */
    uint64_t sync_started_us_[MaxBanks]{};
    uint32_t sync_polls_[MaxBanks]{};
    uint32_t sync_timeouts_=0,sync_slowest_us_=0;
    uint32_t sacr_after_=0,sgfsr_after_=0,global_faults_=0,storms_=0;
    uint32_t sgfsr_seen_=0;           /* annibale: the sGFSR last recorded (it is never written back, so it stays) */
    uint64_t sgfar_=0;uint32_t sgfsynr_[3]{};
    int64_t aperture_x0_=0;uint32_t aperture_state_=0;   /* 0 not run, 1 granted, 2 refused (record only) */
    bool initialized_=false,surveyed_=false;
    bool annibale_=false;             /* adreno830_annibale() when init() ran */
    int read32(uint32_t offset,uint32_t *value) const;
    int write32(uint32_t offset,uint32_t value) const;
    int barrier() const;
    uint32_t cbOffset(unsigned cb) const {return (survey_.global_pages+cb)*0x1000u;}
    /* The context bank a table I/O offset falls in, or -1 (global space, before the survey, beyond MaxBanks). */
    int tableBank(size_t at) const;
    /* The bank's ASID as programmed (TTBR0[63:48] of its image; 0 before programContext). */
    uint32_t bankAsid(unsigned cb) const {return cb<MaxBanks?uint32_t(banks_[cb].image.ttbr0>>48)&0xFFFFu:0;}
    /* The whole-bank invalidation of step 12 and 4.5.7 at the bank's register offset (the TLBSYNC wait follows): TLBIALL
     * (design) or TLBIASID of the bank's ASID (annibale). reg receives the register; non-zero on an I/O failure. */
    int invalidateBank(unsigned cb,uint32_t cb_offset,uint32_t *reg);
    uint16_t stepBase(unsigned cb,uint16_t sid) const;
    IOReturn fail(uint16_t step,uint16_t reason,uint32_t offset,uint64_t value,uint64_t expect,IOReturn result);
    Bank *bank(unsigned cb);
    int streamIndex(const A830SmmuStream &stream) const;
    IOReturn tlbSyncAt(uint32_t cb_offset);
    IOReturn stopBank(unsigned cb);
    void recordFault(Bank &bank,uint32_t fsr,bool polled,bool sampled=false,uint32_t completed=0,uint32_t issued=0);
    bool pushFault(const A830SmmuFault &fault);
    void noteSticky(unsigned cb,uint32_t fsr);
    void stormGuard(Bank &bank,bool progress);
    void globalCheck();
    static uint32_t tcrWithUpper(uint32_t tcr);
    static int tableRead32(void *,size_t,uint32_t *);
    static int tableRead64(void *,size_t,uint64_t *);
    static int tableWrite32(void *,size_t,uint32_t);
    static int tableWrite64(void *,size_t,uint64_t);
    static int tableBarrier(void *);
    static IOReturn programmerClaim(void *,unsigned,const ArmSmmu4KStream *);
    static IOReturn programmerProgram(void *,unsigned,const ArmSmmu4KContextImage *,uint64_t,NativeAdrenoSmmuProgramReport *);
    static IOReturn programmerRoute(void *,unsigned,const ArmSmmu4KStream *);
    static IOReturn programmerVerify(void *,unsigned,const ArmSmmu4KStream *,size_t);
    static IOReturn programmerFault(void *,unsigned,const ArmSmmu4KStream *,size_t);
    static IOReturn programmerDisable(void *,unsigned);
public:
    A830Smmu()=default;
    A830Smmu(const A830Smmu &)=delete;
    A830Smmu &operator=(const A830Smmu &)=delete;
    /* Binds the configuration and reads the profile (adreno830_annibale()) for this object's life; no register access. */
    IOReturn init(const A830SmmuConfig &config);
    /* ARCHITECTURE 4.5.3 steps 1, 2, 5, 6 (read-only): IDs (S1TS, SMS, 4K pages, >= 4 banks, 4K granule, OAS), sCR0.CLIENTPD,
     * every SMR/S2CR (refuses a valid SMR covering SID 0/1/5 or a valid route translating to CB0/CB2) and the slot choice, CB0..CB3
     * state. The result goes to out (may be null) and to fillStatus. Repeatable. */
    IOReturn survey(A830SmmuSurvey *out);
    /* Step 3: sACR read and recorded. Design: CACHE_LOCK set -> write sACR with it clear; the readback is recorded only.
     * Annibale: never written (K90 wrote ACTLR 0x303 without it). Never fails on the value. */
    IOReturn releaseCacheLock();
    /* Step 4: sGFSR non-zero -> record sGFAR/sGFSYNR0-2 and the re-read. Design: the value is written back first. Annibale:
     * never written back (kernel-sequences 14 #9). Never fails on the value. */
    IOReturn clearGlobalFault();
    /* Before any context-fault interrupt (4.5.4): SCTLR = 0 (must read 0), FSR = 0xC00001FE (re-read recorded). */
    IOReturn resetBank(unsigned cb);
    /* 710/712 for the GPU and GMU banks, only after resetBank of both; attach failures or a readback other than an enabled
     * level Group 1 SPI at 0xA0 are recorded in irq_off (the banks are then polled). Annibale: neither is attached in the first
     * round (kernel-sequences 14 #13), both are in irq_off and polled. Returns the number of interrupts wired. */
    unsigned attachIrqs();
    /* 4.5.4 steps 1-2: S2CR = 0x00020300 (read back type FAULT), SMR = 0x80000000 | sid (read back equal). */
    IOReturn claimStream(const A830SmmuStream &stream);
    /* Steps 3-14 with the tolerance rules. ttbr1: 0 (CB2), or ASID << 48 | a zeroed guard table (CB0): the design then sets
     * the TTBR1 walk fields (T1SZ = T0SZ, WBWA, inner shareable, 4K) with EPD1 kept set; the annibale profile writes TTBR1 0
     * and its own SCTLR/TCR/MAIR0 (see above) whatever the image says, ACTLR on the GPU bank only, TLBIASID at step 12. The
     * bank counts as enabled for contextIrq once step 13 wrote SCTLR. */
    IOReturn programContext(unsigned cb,const ArmSmmu4KContextImage &image,uint32_t actlr,uint64_t ttbr1,A830SmmuReadback *readback);
    /* Steps 15-16: S2CR = 0x300 | cb, read back (v & 0x030303FF) == 0x300 | cb; FSR recorded and written back if non-zero. */
    IOReturn routeStream(const A830SmmuStream &stream);
    /* 4.5.5: the LPAC stream (SID 1) on slot 3 to CB0, after CB0 is enabled and before CP_SQE_CNTL = 1. */
    IOReturn attachLpac(uint8_t slot,uint16_t sid,unsigned cb);
    /* 4.5.6: CP_SMMU_APERTURE for the bank through the platform's SCM call; the result is recorded, never a failure. */
    IOReturn grantAperture(unsigned cb);
    /* Read-only (the VM's attach, and the diagnostic patrol >= 1 s): every route claimed for cb is translate-to-cb with its exact
     * SMR, SCTLR.M is set and TTBR0 is what programContext wrote (TCR.EPD1 may have been cleared). */
    IOReturn verifyRoutes(unsigned cb);
    /* 4.5.7: the stream's S2CR to FAULT, its SMR kept valid. */
    IOReturn faultStream(const A830SmmuStream &stream);
    /* SCTLR = 0, TLBIALL (annibale: TLBIASID of the bank's ASID), TLBSYNC (the bank stops counting as enabled first). */
    IOReturn disableBank(unsigned cb);
    /* ARCHITECTURE 4.5.7 for one stream: S2CR FAULT, then SCTLR = 0 and the invalidation + sync of its bank. The caller has
     * stopped the CP and halted GBIF; only after success may the bank's tables be freed. Any failure: keep all memory until a
     * cold reboot. */
    IOReturn release(const A830SmmuStream &stream);
    /* A failure after S3 (ARCHITECTURE 3.4, the first two steps of 4.5.7): every claimed stream back to FAULT and SCTLR = 0 on
     * every programmed bank, so no live route points at tables nobody maintains; no TLB maintenance, nothing freed. Best
     * effort; the first error is returned. */
    IOReturn releaseAll();
    /* 4.12.3 (context = the Bank): FSR & 0xC00001FE == 0 returns; a bank not yet enabled only writes FSR back; otherwise FAR,
     * FSYNR0/1, the SID from CBFRSYNRA go to the fault ring, FSR is written back, RESUME = 1 if FSR.SS, smmu_faults += 1 and the
     * platform's fault_notify runs. 64 handlings in a row without progress clear SCTLR.CFIE, disable the SPI, set irq_off. Before
     * any register read it takes the ring's fences (platform.fences, when bound) for the fault record (sampled). */
    static void contextIrq(void *context);
    /* 4.12.3 (context = the A830Smmu; written, not attached in the first version): records sGFSR/sGFSYNR0-2/sGFAR, writes
     * back (design; annibale records a changed value only and never writes it). */
    static void globalIrq(void *context);
    /* Fault bottom half: hands every recorded fault to sink in order (any thread; one consumer). Returns how many. */
    unsigned drainFaults(void (*sink)(void *cookie,const A830SmmuFault &fault),void *cookie);
    /* Once a second from the DCVS thread while 710/712 are not wired or were turned off (always, in the annibale profile):
     * CB0/CB2 FSR read, recorded and written back; sGFSR as globalIrq (never on the submit path). */
    void poll();
    /* The S6 start check (ARCHITECTURE 4.9, A830CpSmmu): the gate is the LPAC route reading back translate-to-CB0
     * ((S2CR & 0x030303FF) == 0x300 | cb); *readback receives that S2CR. */
    IOReturn lpacRouted(uint32_t *readback);
    /* Record only (4.9): sGFSR and the CB0/CB2 FSR read; the FSRs written back to clear; non-zero values to the sticky words. */
    void recordSticky();
    /* The hang snapshot (4.13.4): FSR and FAR of the GPU bank [0] and the GMU bank [1]; reads only. */
    void snapshot(uint32_t fsr[2],uint64_t far[2]);
    /* The interrupts the storm guard turned off, back on after a recovery re-initialised the device side (4.12.1): SCTLR.CFIE
     * set again on enabled banks, spi_enable; returns how many. */
    unsigned restoreIrqs();
    uint32_t storms() const {return storms_;}
    /* Slumber with CX collapsed (ARCHITECTURE 4.17 P1, M13): routesIntact() tells whether the SMMU kept our state; restore()
     * replays the take-over from the recorded images (claim, program, route, LPAC, aperture) over the unchanged tables. */
    bool routesIntact();
    IOReturn save();
    IOReturn restore();
    /* The injected programmer of NativeAdrenoSmmuContext; streams map to A830SmmuStream {slot, bank, sid}. */
    const NativeAdrenoSmmuProgrammer *programmer();
    /* The TLB I/O the VM gives the kit (programmer()->io). Annibale: the kit's TLBIVA/TLBIVAA/TLBIVAL/TLBIVAAL, TLBIASID and
     * TLBIALL are not forwarded; every TLBSYNC it writes to a bank goes out as TLBIASID of that bank's ASID, then TLBSYNC = 1,
     * so the hardware sees K90's only invalidation (kernel-sequences 7.5, 14 #12) and 32-bit stores only. The annibale
     * programmer also sets sync_after_map, so a map (invalid to valid) gets that invalidation too, as on K90. */
    const ArmSmmu4KIo *tableIo() const {return &table_io_;}
    bool annibale() const {return annibale_;}
    /* Writes the S3 page words (see above) into status, the aperture outcome (A830StatusAperture of apertureState) included. */
    void fillStatus(Adreno830Status *status) const;
    const A830SmmuSurvey &surveyed() const {return survey_;}
    const A830SmmuFailure &failure() const {return failure_;}
    const Bank *bankState(unsigned cb) const;
    A830SmmuStream gpuStream() const {return {survey_.slot_gpu,config_.cb_gpu,config_.sid_gpu};}
    A830SmmuStream lpacStream() const {return {survey_.slot_lpac,config_.cb_gpu,config_.sid_lpac};}
    A830SmmuStream gmuStream() const {return {survey_.slot_gmu,config_.cb_gmu,config_.sid_gmu};}
    uint32_t outputBits() const {return survey_.output_bits;}
    uint32_t tlbTimeouts() const {return sync_timeouts_;}
    uint32_t apertureState(int64_t *x0) const {if(x0)*x0=aperture_x0_;return aperture_state_;}
    uint64_t contextOffset(unsigned cb) const {return cbOffset(cb);}
};

/* The device binding (aarch64 kernel only): out-of-line MMIO helpers over the mapped window, "r" constraints only, a single
 * 64-bit store for 64-bit registers, dsb sy as the barrier. mmio must stay valid for the io's life. */
struct A830SmmuMmio {volatile uint8_t *base;size_t bytes;};
IOReturn A830SmmuBindMmio(A830SmmuMmio *mmio,ArmSmmu4KIo *io);
/* The kernel platform: mach time, IODelay, scm for CP_SMMU_APERTURE (may be null), notify for the fault bottom half (may be
 * null; called with notify_cookie). The SPI members stay null: the orchestrator copies the PaganiArmCpu entry points it
 * resolved (Adreno830Os::spi_attach/spi_disable/spi_enable) into them. One kernel platform per boot. */
IOReturn A830SmmuKernelPlatform(A830SmmuPlatform *platform,Sm8750Scm *scm,void (*notify)(void *),void *notify_cookie);
#endif
