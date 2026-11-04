/* SPDX-License-Identifier: MIT */
#ifndef A830_GMU_H
#define A830_GMU_H
#include "A830GmuFirmware.h"
#include "A830Regs.h"
#include "Sm8750Rpmh.h"
#include <IOKit/IOTypes.h>
#include <stddef.h>
#include <stdint.h>

/* The A830 GMU, host side (ARCHITECTURE 4.7, 4.12.3, 4.13.4, 4.17; KGSL adreno_gen8_gmu.c in RB mode): cold boot B1-B8,
 * OOB set/clear with the GX power confirmation, the GMU-side counters, the HFI (336) and AO (337) interrupt top halves and
 * their bottom half, the force-off of a failed or hung GMU, RSCC sleep/wake, the GMU side of slumber (P1) and the IFPC
 * helpers (P2). The host never programs the RSCC sequencer, the PDC or CX_MISC_TCM_RET_CNTL (AOP owns them on A830).
 *
 * Fatal conditions are only the ones ARCHITECTURE 10 keeps: CM3_FW_INIT_RESULT & 0xE00 or a boot timeout, GMU2HOST_INTR
 * bit15 (CM3 fault), AO bit0 (watchdog bite), and the hang conditions of the callers. Everything else read here (B2 residue,
 * ITCM/version read-back, B6 in diag mode, GMU2HOST bit23, AO bit3/bit5, RPMH_POWER_STATE, CX_MISC_GFX_PWR_CLK_STATUS) is
 * recorded in A830GmuReport and never decides a step. No register owned by firmware or the hypervisor is read back as a
 * condition of progress.
 *
 * Profiles (A830Profile.h, read at run time): the annibale profile follows the sequences the REDMI K90 port proved on the
 * same SoC and GMU firmware wherever kernel-sequences section 14 says so (the default config, coldBoot, rsccWake,
 * attachInterrupts, a830CxTimerSeed, a830CxAhbTimeout; each is described where it is declared); the design profile keeps
 * ARCHITECTURE 4.7 as written.
 *
 * Concurrency: one owner thread at a time calls the sequences (the start sequence, recovery, slumber, the DCVS thread), all
 * under the HFI mutex of ARCHITECTURE 2.3. hfiIrq/aoIrq run in primary interrupt context and only touch the interrupt
 * registers and atomics; gmuWork runs in a kernel thread and takes the HFI mutex through the hooks. */

class A830Gmu;

/* Time and CPU services. The kernel set is A830GmuKernelOs() (NativeAdrenoKernelTime); host tests inject a model. */
struct A830GmuOs {
    void *cookie;
    uint64_t (*now_us)(void *cookie);              /* monotonic microseconds */
    void (*sleep_us)(void *cookie,uint32_t us);    /* thread context: blocks the calling thread (100 us poll steps) */
    void (*delay_us)(void *cookie,uint32_t us);    /* short wait, legal in any context (1 and 10 us steps) */
    uint64_t (*counter)(void *cookie);             /* CPU generic timer count, the source of mach_absolute_time */
    uintptr_t (*irq_disable)(void *cookie);        /* interrupts off on this CPU; returns what irq_restore needs */
    void (*irq_restore)(void *cookie,uintptr_t state);
};

/* Bounds-checked MMIO over mapped windows (kernel: Device-nGnRE mappings of the board windows). A null base is an absent
 * window; bytes is a multiple of 4. write32 is a plain 32-bit store; barrier is dsb sy. The object must outlive the io. */
struct A830GmuMmio {volatile uint8_t *base[A830GmuWindows];uint32_t bytes[A830GmuWindows];};
IOReturn A830GmuBindMmio(A830GmuMmio *mmio,A830GmuIo *io);

/* CX timer (ARCHITECTURE 4.4.2, KGSL gen8_cx_timer_init): CX_MISC AO_COUNTER_LO/HI (0x3D9E200/204) seeded with the CPU
 * counter plus the measured cost of the write, interrupts off, so GPU timestamps and mach_absolute_time share one time
 * base. Once per boot, at S4 step 1, before the GMU firmware is loaded and CM3 released: a830CxTimerSeed is a no-op while
 * `seeded` holds, so recovery (CX stayed up) never reseeds a counter the GMU runs on. Only a wake after CX lost power (P1
 * full power-off) calls a830CxTimerLost first, and the next S4 seeds again before the firmware is reloaded.
 * annibale profile (A830Profile.h): annibale never seeds the counter (kernel-sequences section 14 row 21: the first round
 * goes without the seed, GetTimestamp alone needs it), so a830CxTimerSeed writes nothing, leaves `seeded` false and counts
 * the call in `skips`. */
struct A830CxMisc {
    const A830GmuIo *io;       /* CxMisc window */
    const A830GmuOs *os;       /* counter, irq_disable, irq_restore */
    bool seeded;
    uint64_t seed;             /* counter value written (counter + skew) */
    uint64_t skew;             /* averaged cost of the write, counter ticks */
    uint32_t seeds;            /* seeds done since boot */
    uint32_t skips;            /* annibale profile: seeds left out */
    uint64_t ahb_reg;          /* annibale a830CxAhbTimeout: physical address of the register that failed, 0 when none */
    uint32_t ahb_read;         /*   and the value read there (0xFFFFFFFF: the access failed) */
};
enum : uint16_t {A830S4StepCxTimer=1};   /* S4 step of the seed (ARCHITECTURE 8.3) */
enum : uint32_t {A830CxTimerSamples=16};
/* kIOReturnSuccess (also when already seeded, and in the annibale profile), kIOReturnBadArgument (missing io/os callbacks),
 * kIOReturnIOError (a write failed; not seeded). */
IOReturn a830CxTimerSeed(A830CxMisc &cx);
void a830CxTimerLost(A830CxMisc &cx);
/* M13 c (ARCHITECTURE 4.4.2, KGSL gen8_enable_ahb_timeout_detection): CX_AHB_{AON,GMU,CP,VBIF_SMMU,HOST}_CNTL
 * (0x3D9E040..0x3D9E050) = 0x1B12, so a stuck AHB access ends in an AHB error before the NOC timeout. KGSL writes it right
 * after the clocks, before the CX timer, on every GMU boot. Design profile: written, no read back.
 * annibale profile (kernel-sequences section 14 row 6, K/adreno8_cx.c:647-662, field-notes 1-8): each register is read
 * first (0xFFFFFFFF or a failed read is an access failure), left alone when it already holds the value, else written,
 * completed (dsb) and read back strictly; the first register that fails is in cx.ahb_reg/ahb_read and the result is
 * kIOReturnIOError. Calling it at the end of S1 (HUB at 200 MHz) is the caller's part of row 6. */
IOReturn a830CxAhbTimeout(A830CxMisc &cx,uint32_t value=A830_CX_AHB_CNTL_VALUE);

/* What a failed sequence reports, in the orchestrator's terms (Adreno830Start Failure, ARCHITECTURE 8.3): step, A830Reason,
 * the physical address touched (or the INTID), the value read and the value expected. */
struct A830GmuFailure {uint16_t step,reason;uint64_t reg,value,expect;};
/* Steps. S5: B1..B8 = 1..8, OOB 0x20, GX confirmation 0x21 (ARCHITECTURE 8.3); the counters written after it 0x22. The
 * others belong to recovery (M10/M13) and slumber (P1): 0x30 RSCC sleep, 0x31 RSCC wake, 0x38 force-off, 0x50 + n slumber
 * step Sn. */
enum : uint16_t {
    A830GmuStepB1=1,A830GmuStepB2,A830GmuStepB3,A830GmuStepB4,A830GmuStepB5,A830GmuStepB6,A830GmuStepB7,A830GmuStepB8,
    A830GmuStepOob=0x20,A830GmuStepGxOn=0x21,A830GmuStepCounters=0x22,
    A830GmuStepRsccSleep=0x30,A830GmuStepRsccWake=0x31,A830GmuStepForceOff=0x38,A830GmuStepSlumber=0x50,
};
/* A830GmuReport::warnings: recorded, never a failure. */
enum : uint32_t {
    A830GmuWarnItcm=1u<<0,           /* B4: ITCM words read back differ from the image */
    A830GmuWarnVersion=1u<<1,        /* B5: DTCM version words differ from the image's */
    A830GmuWarnB6Readback=1u<<2,     /* B6, diag mode only: a configuration register read back differently */
    A830GmuWarnCm3FaultLinux=1u<<3,  /* GMU2HOST bit23 (the Linux CM3 fault bit) seen */
    A830GmuWarnFenceErr=1u<<4,       /* AO bit3 FENCE_ERR seen; fence_status recorded */
    A830GmuWarnAhbBusErr=1u<<5,      /* AO bit5 HOST_AHB_BUS_ERR seen */
    A830GmuWarnTcsBusy=1u<<6,        /* force-off/slumber: a GPU RSCC TCS stayed busy */
    A830GmuWarnGbifHalt=1u<<7,       /* force-off: GBIF halt not acknowledged within 100 ms */
    A830GmuWarnGxStuck=1u<<8,        /* force-off: GX GDSC did not report power-down within 2 ms */
    A830GmuWarnGxVote=1u<<9,         /* force-off: no temporary host GX vote (no voter, or it failed) */
    A830GmuWarnRsccSleep=1u<<10,     /* force-off: the RSCC sleep did not complete */
    A830GmuWarnGxStillOn=1u<<11,     /* force-off: GFX_PWR_CLK_STATUS still shows GX on at the end */
    A830GmuWarnIrqReadback=1u<<12,   /* an SPI attach read back other than level, Group1, enabled, priority 0xA0 */
    A830GmuWarnStorm=1u<<13,         /* a GMU interrupt was turned off by the storm guard (reason 23) */
    A830GmuWarnOobResidue=1u<<14,    /* the OOB ack bit was already set before the request */
    A830GmuWarnRpmhState=1u<<15,     /* after the GX confirmation RPMH_POWER_STATE was not ACTIVE */
    A830GmuWarnGeneral11=1u<<16,     /* annibale profile: GENERAL_11 read nonzero (or unreadable) before CM3 left reset */
};
struct A830GmuReport {
    uint32_t info_before,ao_before;      /* B2: GMU2HOST_INTR_INFO and AO_HOST_INTERRUPT_STATUS before the clear */
    uint32_t rsc_status;                 /* annibale B3: RSC_STATUS0_DRV0 (0x3D50010) at the cold boot (bit16: asleep) */
    uint32_t general11;                  /* annibale, before B6: GENERAL_11 (0x3D7E72C), expected 0 (record only) */
    uint32_t tcm_verified,ddr_verified;  /* annibale B4: TCM words read back and DDR bytes compared (A830GmuLoadCheck) */
    uint32_t polled;                     /* annibale: A830GmuIrq* lines left masked and polled instead of attached */
    uint32_t itcm[4];                    /* B4 read back */
    uint32_t core,pwr,hfi;               /* B5: DTCM version words read */
    uint32_t init_result,init_us;        /* B7: last CM3_FW_INIT_RESULT, time to it */
    uint32_t hfi_status,hfi_us;          /* B8: last HFI_CTRL_STATUS, time to it */
    uint32_t b6_mismatches,b6_first;     /* diag: B6 registers that read back differently, dword of the first */
    uint32_t oob_us,gx_us;               /* OOB_GPU acknowledgement and GX confirmation times */
    uint32_t gx_pwrclk,rpmh_state,cx_gx_status;   /* GMUCX GFX_PWR_CLK_STATUS, RPMH_POWER_STATE, CX_MISC GFX_PWR_CLK_STATUS */
    uint32_t info_last,ao_last,fence_status;      /* last interrupt words seen; AHB_FENCE_STATUS at the last FENCE_ERR */
    uint32_t log_wptr;                   /* GENERAL_9 saved by the last RSCC sleep, written back by B6 */
    uint32_t warnings;
    uint32_t boots,boot_attempts,force_offs,rscc_sleeps,rscc_wakes,slumbers,slumber_aborts,faults,storms;
    uint32_t hfi_irqs,ao_irqs,works,polls;
    uint32_t irq_readback[2];            /* attach read back of 336, 337 */
    uint32_t fenced_retries;             /* fencedWrite rewrites since boot */
};

/* Callbacks into the HFI layer and the recovery owner. Any may be null (then that part is skipped). */
struct A830GmuHooks {
    void *cookie;
    int (*reset_queues)(void *cookie);   /* A830Hfi::resetQueues: indices to 0 while CM3 is held in reset (between B1 and
                                            B7); nonzero fails B6 with reason 1 */
    void (*lock)(void *cookie);          /* the HFI mutex (ARCHITECTURE 2.3): gmuWork holds it while it handles the fatal bits */
    void (*unlock)(void *cookie);
    int (*drain_debug)(void *cookie);    /* A830Hfi::drainDebug: empties the debug queue taking the HFI mutex itself (so it is
                                            called outside lock/unlock); nonzero = it found F2H_MSG_ERR or a host resource
                                            request: a global fault with reason 16 */
    void (*fault)(void *cookie,uint16_t reason,uint32_t info,uint32_t ao);   /* global GMU fault (reason 14 or 16): device
                                            lost and recovery (4.13.4). Called with the lock held: it must only schedule. */
    int (*gx_vote)(void *gx_cookie,int on);   /* force-off step 6: temporary host vote of gfx(/gmxc) at its lowest nonzero
                                            level (on) and its removal (off); 0 = done. A830GmuGxVote is the DRV-2 one. */
    void *gx_cookie;
};

/* Interrupt plumbing (ARCHITECTURE 4.12.1). The kernel set is A830GmuKernelIrqOps(): armcpu_spi_attach/disable/enable of
 * PaganiArmCpu and a kernel-high, once-only thread_call. disable/enable may be null (device-side masking only). */
struct A830GmuIrqOps {
    IOReturn (*attach)(uint32_t intid,bool edge,void (*handler)(void *),void *context,uint32_t *readback,uint64_t *router);
    IOReturn (*disable)(uint32_t intid);
    IOReturn (*enable)(uint32_t intid);
    void *(*work_allocate)(A830Gmu *gmu);    /* a deferred call that runs gmu->gmuWork(); null on failure */
    void (*work_kick)(void *work);           /* from interrupt context */
};

/* Slumber S2 and S5 need the GPU interrupt controller and the HFI (ARCHITECTURE 4.17 P1). */
struct A830GmuSlumberOps {
    void *cookie;
    bool (*gpu_irq_pending)(void *cookie);   /* S2, GX on under OOB_GPU: an opened RBBM_INT_0_STATUS bit is set */
    void (*gpu_irq_off)(void *cookie);       /* S2: RBBM_INT_0_MASK = 0 (the GPU interrupt is masked for the slumber) */
    int (*prepare_slumber)(void *cookie);    /* S5: HFI PREPARE_SLUMBER {bw = default bus | 0xFFFF0000, freq = default level};
                                                0 = acknowledged */
};

struct A830GmuConfig {
    uint32_t chip_id;            /* GENERAL_10: A830_GMU_CHIPID (0x08000100) */
    uint32_t ahb_fence_range;    /* AHB_FENCE_RANGE_0. Design default A830_AHB_FENCE_RANGE_KGSL (lower bound 0x8A0, 1.3 #12;
                                    the Linux 0x8C0 form is the P2 alternative). annibale default A830_AHB_FENCE_RANGE_LINUX
                                    0x80C808C0, the value annibale's direct-ring (RB) mode proved on K90 (kernel-sequences 4.6
                                    step 15, section 14 row 17, K/adreno8_gmu.c:607) */
    uint32_t idle_level;         /* RPMH_POWER_STATE the GMU idles at: A830_GMU_RPMH_POWER_STATE_ACTIVE (IFPC off, P0/P1)
                                    or _IFPC (P2) */
    uint32_t diag;               /* pagani-gpu-diag: B6 read back and recorded */
    uint32_t hfi_irq,gmu_irq;    /* INTIDs 336 and 337 */
    uint32_t log_bytes;          /* GMU log size for GENERAL_8 (A830_GMU_LOG_BYTES) */
    uint32_t cx_busy_mask;       /* slumber S6 busy bit of GPU_CX_BUSY_STATUS: A830_GMU_CX_BUSY_KGSL (bit30, default; 0 means
                                    it) or the Linux bit23 (ARCHITECTURE 9.2 #4, a single-variable experiment) */
};
/* The defaults of the running profile (A830Profile.h): only ahb_fence_range differs. */
void A830GmuDefaultConfig(A830GmuConfig *config);
/* CM3_FW_INIT_RESULT criterion of B7 (Linux a6xx_gmu_start): core > 0x20010004 → (v & 0x1FF) == 0x100, else
 * v == 0xBABEFACE. */
bool A830GmuInitDone(uint32_t core,uint32_t value);
/* B8 limit for HFI_CTRL_STATUS bit0: the design waits A830_T_HFI_INIT_US (100 ms, ARCHITECTURE 4.7.3 B8); annibale waits
 * A830_T_HFI_INIT_ANNIBALE_US, 500 ms (kernel-sequences 5.2 step 4 and section 14 row 22, K/adreno8_hfi.c:345; field-notes
 * 5-7). A830GmuHfiInitUs() is the limit of the running profile; coldBoot uses the same pair. */
enum : uint32_t {A830GmuHfiInitAnnibaleUs=A830_T_HFI_INIT_ANNIBALE_US};
uint32_t A830GmuHfiInitUs();

/* Temporary GX vote through apps DRV-2 for force-off step 6 (A830GmuHooks::gx_vote, cookie = this object): on votes
 * gfx.lvl (and gmxc.lvl when present) at the lowest nonzero level, off votes level 0. Not one of the host votes of
 * ARCHITECTURE 4.3.3. Returns 0 or a negative Sm8750Rpmh error. */
struct A830GmuGxVoter {
    const Sm8750RpmhIo *drv2;
    const Sm8750RpmhResources *res;
    Sm8750RpmhResult result;
    uint32_t gfx_hlvl,gmxc_hlvl;
    int status,held;
};
int A830GmuGxVote(void *voter,int on);

class A830Gmu final {
    A830GmuIo io_{};
    A830GmuOs os_{};
    A830GmuConfig config_{};
    A830GmuHooks hooks_{};
    const A830GmuBlocks *firmware_=nullptr;
    NativeAdrenoGmuMemory *memory_=nullptr;
    const A830GmuIrqOps *irq_=nullptr;
    void *work_=nullptr;
    A830GmuFailure failure_{};
    A830GmuReport report_{};
    uint32_t oob_refs_[2]{};
    uint32_t info_pending_=0,ao_pending_=0,events_=0,fatal_=0;   /* atomics shared with the top halves */
    uint32_t hfi_idle_=0,ao_idle_=0;                    /* consecutive top-half entries without progress */
    uint32_t irq_wired_=0,irq_off_=0,storm_off_=0;      /* bit0 336, bit1 337 */
    uint32_t log_wptr_=0;                               /* GENERAL_9 kept across an RSCC sleep */
    bool bound_=false,booted_=false,first_boot_done_=false,rscc_asleep_=false;
    IOReturn fail(uint16_t step,uint16_t reason,uint64_t reg,uint64_t value,uint64_t expect,IOReturn result);
    void warn(uint32_t bits);
    bool rd(unsigned window,uint32_t offset,uint32_t *value);
    bool wr(unsigned window,uint32_t offset,uint32_t value);
    void barrier();
    int pollMask(unsigned window,uint32_t offset,uint32_t mask,uint32_t want,uint32_t timeout_us,uint32_t step_us,
                 bool sleep,uint32_t *value,uint32_t *elapsed_us);   /* 0 matched, 1 timeout, 2 access failed */
    bool checkFatal(uint32_t *info,uint32_t *ao,uint16_t *reason);
    void recordInterrupts(uint32_t info,uint32_t ao);
    void stormOff(unsigned which);
    void kick();
    IOReturn waitRsccTcs(uint32_t timeout_us);
    IOReturn haltGbif(uint32_t mask,bool release);
    IOReturn wakeRscc();   /* the B3 handshake, whatever made the RSC sleep */
public:
    A830Gmu()=default;
    A830Gmu(const A830Gmu &)=delete;
    A830Gmu &operator=(const A830Gmu &)=delete;
    /* Binds the windows, services, firmware (parsed) and its memory (allocated and mapped by the GMU SMMU context). The
     * hooks are copied. kIOReturnBusy once bound. */
    IOReturn bind(const A830GmuIo &io,const A830GmuOs &os,const A830GmuConfig &config,const A830GmuBlocks *firmware,
                  NativeAdrenoGmuMemory *memory,const A830GmuHooks *hooks);
    /* B1-B8 (ARCHITECTURE 4.7.3), preconditions as there (S1 power, CB2 mapping every region, host votes held). On failure
     * failure() holds the step (1..8) and reason (5 timeout, 13 NMI, 14 CM3 fault or watchdog, 2 firmware or a layout that
     * does not hold it at B4, 1 the queue-reset hook at B6, 7 a register access); the caller then runs forceOff() and may
     * boot again, at most 3 attempts in S5. *init_result (may be null)
     * gets the last CM3_FW_INIT_RESULT. B3 wakes the RSCC only after an RSCC sleep; B6 writes back the saved GENERAL_9.
     * The annibale profile (A830Profile.h; kernel-sequences 4.6 and section 14 rows 16-22) differs in:
     *  - B2: no GMU interrupt is opened: AO_HOST_INTERRUPT_MASK and GMU2HOST_INTR_MASK = 0xFFFFFFFF, then both CLR =
     *    0xFFFFFFFF, in that order (4.6 steps 3-6); the fatal bits are polled (attachInterrupts, pollFatal);
     *  - B3: also when RSC_STATUS0_DRV0 (0x3D50010) bit16 says the RSC is asleep at the cold boot (row 19), see rsccWake;
     *  - B4: every TCM word read back and every DDR block compared (row 16); a difference fails B4 with reason 7, reg the
     *    physical TCM address (or the GMU address of a DDR word), value read, expect the image's word;
     *  - before B6: GENERAL_11 (0x3D7E72C) read and recorded, expected 0 (row 20; annibale refuses to release CM3 otherwise,
     *    this driver only records it: report().general11, A830GmuWarnGeneral11);
     *  - B6: AHB_FENCE_RANGE_0 from the annibale default config, 0x80C808C0 (row 17);
     *  - B8: GMU2HOST_INTR_CLR and GMU2HOST_INTR_MASK = 0xFFFFFFFF before HFI_CTRL_INIT, and 500 ms for HFI_CTRL_STATUS
     *    bit0 (5.2 steps 2-4, row 22; A830GmuHfiInitUs). */
    IOReturn coldBoot(uint32_t *init_result);
    /* OOB (ARCHITECTURE 4.7.8): set BIT(30 - 2 req) in HOST2GMU_INTR_SET, poll GMU2HOST_INTR_INFO BIT(31 - req) every 100 us
     * for 100 ms, clear it. Reference counted: only the first set and the last clear reach the GMU. req 0 = GPU, 1 =
     * perfcounter (gen8 refuses the others). Step 0x20, reason 5 on timeout (the reference is not taken). */
    IOReturn oobSet(uint32_t *ack_us,uint32_t request=A830_OOB_REQ_GPU);
    void oobClear(uint32_t request=A830_OOB_REQ_GPU);
    /* After OOB_GPU: GMUCX GFX_PWR_CLK_STATUS[1:0] == 0 within timeout_us (10 us steps). Otherwise step 0x21, reason 6, and
     * no GX register was touched. RPMH_POWER_STATE and CX_MISC_GFX_PWR_CLK_STATUS are only recorded. */
    IOReturn waitGxOn(uint32_t timeout_us=A830_T_GX_ON_US);
    /* GMU-side registers rewritten after every GMU start, under OOB_GPU (4.7.8): GPU_CX_BUSY_MASK, the XOCLK power counters
     * (busy select 0x20, IFPC select 0x04, enable) and the AO clock gating (CGC_MODE/DELAY/HYST). */
    IOReturn writeCounters();
    /* DCVS busy ticks (POWER_COUNTER_XOCLK_0, 19.2 MHz): high, low, high; the low word is read again when high moved. */
    IOReturn readBusy(uint64_t *ticks);
    /* 4.7.9 in KGSL gen8_gmu_pwrctrl_suspend order: mask the GMU interrupts, FALNEXT veto, keepalive off; wait the GPU
     * RSCC TCSes (1 ms each, record only); clear the AHB fence; CM3 reset; GBIF arbiter halt (100 ms, record only); GX GDSC
     * collapse with a temporary host GX vote when GX is still on; RSCC sleep (record only); GBIF halt released. Always
     * completes; warnings say what did not. The GMU is then off and the OOB references are dropped. */
    IOReturn forceOff();
    /* 4.7.4: CM3 reset, GENERAL_9 saved, RSCC_CONTROL_REQ BIT0, RSC_STATUS0_DRV0 bit16 within 100 ms, request cleared.
     * Skipped before the first successful boot and when already asleep. Step 0x30, reason 5. */
    IOReturn rsccSleep();
    /* B3: RSCC_CONTROL_REQ BIT1, ACK bit1 and SEQ_BUSY_DRV0 == 0 within 100 ms each, request cleared. Only after a sleep.
     * Step 0x31, reason 5. annibale profile (K/adreno8_gmu.c:582-597): the request is read first and BIT1 ORed into it,
     * and afterwards the request is written back to what it was only when this driver set BIT1 (with nothing else
     * pending that is the same 2-then-0 pair); a failed read of the request is step 0x31, reason 7. */
    IOReturn rsccWake();
    /* GMU side of slumber (4.17 P1) S2-S9: OOB_GPU and the GPU interrupt check (kIOReturnBusy, nothing changed, when one is
     * pending), GPU interrupt off and OOB clear; RPMH_POWER_STATE at the idle level within 100 ms; GPU RSCC TCSes complete
     * (2 ms each); power counters off, PREPARE_SLUMBER, AHB fence allow, FALNEXT veto; GPU_CX_BUSY_STATUS bit30 clear within
     * 100 ms; RSCC sleep; GBIF client and arbiter halt; GMU interrupts masked and cleared. S10 (4.2.3 power-off) and the wake
     * (4.2.1, SMMU, coldBoot with RSCC wake) belong to the caller. On failure step 0x50 + n; the caller runs forceOff(). */
    IOReturn slumber(const A830GmuSlumberOps &ops);
    /* P2 (IFPC): the RPMH idle level slumber waits for (A830_GMU_RPMH_POWER_STATE_ACTIVE or _IFPC). */
    void setIdleLevel(uint32_t level) {config_.idle_level=level;}
    /* P2: GMUCX PWR_COL_KEEPALIVE (the GPU interrupt service holds it while it reads GX registers). */
    IOReturn keepalive(bool on);
    /* P2: AHB_FENCE_CTRL == 0 (allow), polled every 1 us at most `polls` times (KGSL: 100). */
    IOReturn waitFenceAllow(uint32_t polls=100);
    /* KGSL gen8_fenced_write (4.13.2): write, dsb, AHB_FENCE_STATUS bit0 (WRITEDROPPED0) set → 10 us and write again, at
     * most 200 times; kIOReturnTimeout after that. *retries (may be null) gets the rewrites. */
    IOReturn fencedWrite(unsigned window,uint32_t offset,uint32_t value,uint32_t *retries);
    /* Attach 336 (HFI) and 337 (AO), level triggered (4.12.2: optional, after B8). A missing attach, an error or an
     * unexpected read back is recorded in irq_off (bits 1 and 2 of adreno830_status.irq_off) and the GMU fatal bits are then
     * polled (pollFatal). kIOReturnSuccess even then; kIOReturnBusy when already attached.
     * annibale profile (section 14 row 18, K/adreno8_gmu.c:576-580, field-notes 10-1): nothing is attached and the work
     * call is not allocated; both lines count as off (interruptsOff() == 3, so pollFatal reads them) and report().polled
     * says so. That is the profile's choice, not a failure: adreno830_status.irq_off is left alone. */
    IOReturn attachInterrupts(const A830GmuIrqOps &ops);
    /* After recovery re-initialized the device side (B2): re-enable what the storm guard turned off. */
    IOReturn reenableInterrupts();
    /* Top halves (primary interrupt context, no lock, interrupt registers only). hfiIrq: read GMU2HOST_INTR_INFO into an
     * atomic, unconditionally write CLR = 0x00808006, schedule gmuWork. aoIrq: read AO_HOST_INTERRUPT_STATUS, write the
     * same value to CLR, mask bit0 at once on a watchdog bite, read AHB_FENCE_STATUS on FENCE_ERR, schedule gmuWork. Both
     * count entries without progress (status 0 on entry, or the same bits after the clear): 64 in a row mask the source on
     * the device side, disable the SPI and set irq_off (reason 23). */
    static void hfiIrq(void *context);
    static void aoIrq(void *context);
    /* Bottom half (kernel thread): under the HFI lock, drain the debug queue and handle the fatal bits (bit15, watchdog:
     * gmu_faults + 1 and the fault hook); bit23, FENCE_ERR and AHB errors are recorded only. */
    void gmuWork();
    /* With 336 or 337 off: what the top halves would have read, cleared and handed to gmuWork (DCVS thread, once a second). */
    IOReturn pollFatal();
    const A830GmuFailure &failure() const {return failure_;}
    const A830GmuReport &report() const {return report_;}
    bool bound() const {return bound_;}
    bool booted() const {return booted_;}
    bool rsccAsleep() const {return rscc_asleep_;}
    uint32_t oobReferences(uint32_t request=A830_OOB_REQ_GPU) const {return request<2?oob_refs_[request]:0;}
    /* A fatal GMU bit (CM3 fault bit15, watchdog bite) was seen by a top half or a poll since the last boot (A830HfiIo::
     * faulted). */
    bool fatalSeen() const {return __atomic_load_n(&fatal_,__ATOMIC_ACQUIRE)!=0;}
    uint32_t interruptsOff() const {return irq_off_;}
    uint32_t interruptsWired() const {return irq_wired_;}
};
enum : uint32_t {A830GmuStormLimit=64,A830GmuIrqHfi=1u<<0,A830GmuIrqAo=1u<<1};
/* adreno830_status.irq_off bits of the two GMU interrupts (ARCHITECTURE 8.2). */
enum : uint32_t {A830StatusIrqOffHfi=1u<<1,A830StatusIrqOffGmu=1u<<2};

/* A GPU-space dword offset (A8XX_GMUCX_*, A8XX_GMUAO_*, A8XX_GMU_CM3_*TCM*, A8XX_CX_MISC_*, A8XX_RSCC_* is not one: it is
 * RSCC-relative) as the window and byte offset of A830GmuIo: GMU dwords to Gmu, CX_MISC dwords to CxMisc, the rest of the
 * first 0x40000 bytes to Gpu. false when the dword is in none (for adapters such as A830HfiIo, which speaks dwords). */
bool A830GmuDwordWindow(uint32_t dword,unsigned *window,uint32_t *offset);
int A830GmuReadDword(const A830GmuIo &io,uint32_t dword,uint32_t *value);
int A830GmuWriteDword(const A830GmuIo &io,uint32_t dword,uint32_t value);

/* Kernel services (defined in kernel builds only). */
const A830GmuOs *A830GmuKernelOs();
const A830GmuIrqOps *A830GmuKernelIrqOps();
#endif
