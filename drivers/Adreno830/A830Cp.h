/* SPDX-License-Identifier: MIT */
#ifndef A830_CP_H
#define A830_CP_H
#include <IOKit/IOTypes.h>
#include <stddef.h>
#include <stdint.h>
#include "NativeAdrenoRing.h"
struct Adreno830Status;

/* The Gen8 (A830) command processor (ARCHITECTURE 4.8-4.11, 4.13.4, 4.15; PLAN M4, M5, M10, M11, M13): hardware init (S6
 * steps 1-17 with 9a), the SQE/AQE bases, the ring registers, the SQE start and CP_ME_INIT, the idle criterion, the stall dump,
 * and after the ZAP authentication (S7) the secure exit, the GMEM protect rewrite, the power-up list and the clock gating.
 * The recovery (A830Recovery, M10) reuses every step: haltSqe/haltGbif before the GMU is forced off, then hwInit, startRing,
 * startCp and finishSecure again (the ZAP stays resident, so only CP_SET_SECURE_MODE is sent).
 *
 * Preconditions are the stage machine's (ARCHITECTURE 3.4): OOB_GPU set and GX confirmed on (S5) before any call that touches
 * a GX register; CB0 and the LPAC route enabled (S3) before startCp. Register values are the generated tables (A830HwInit.h)
 * and A830Regs.h; the order is Linux hw_init/a8xx_cp_init with KGSL gen8_start values. Pipe registers are written with
 * CP_APERTURE_CNTL_HOST = pipe << 12 and the aperture is back at 0 when a step ends; hwInit, the pipe reads and the dumps hold
 * the device MMIO lock (io.lock) so nothing else moves the aperture (the GPU interrupt top half never touches it).
 * Read-back rule of the design profile (ARCHITECTURE 10 #1): CP_SQE_CNTL, CP_RB_RPTR/WPTR at start, sGFSR/FSR,
 * RBBM_CGC_P2S_STATUS and GPU_CX_BUSY_STATUS are recorded, never gates. The gates are the LPAC route, the idle criterion
 * and the fenced WPTR write.
 *
 * Bring-up profile (A830Profile.h, read at every call). The annibale profile (the default) does what the REDMI K90 port
 * proved on the same SoC (annibale kernel-sequences 9.2, 9.4, 9.7; section 14 rows 29, 32, 33, 35):
 *  - hwInit writes annibale's list in annibale's order: GBIF halts released, RBBM_INT_0_MASK = 0 (no GPU interrupt, every
 *    wait polls), the design steps 3-15 without the CCHE trap/write-through bases, the RBBM/slice/CP perfcounter enables,
 *    bicubic TABLE_0 and the GMEM save/restore enable, CP_INTERRUPT_STATUS_MASK_GLOBAL, the SECVID range last, then a
 *    barrier and annibale's status check (RBBM_INT_0_STATUS & 0x33C002C2, CP_INTERRUPT_STATUS_GLOBAL & 0x007F007F, both 0).
 *    No RBBM_INT_CLEAR_CMD. Every CP_APERTURE_CNTL_HOST write and both halt releases are read back and must match.
 *  - loadMicrocode and startRing read every base and ring register back (must match); startRing writes base, control,
 *    BR and BV shadow addresses in that order and requires CP_RB_RPTR = CP_RB_WPTR = 0 first; startCp requires CP_SQE_CNTL
 *    to read back 1 and, with the A830CpHealth hook, fails on sGFSR bits that appear across the SQE start.
 *  - Every idle wait uses annibale's criterion (see waitIdle) and CP_ME_INIT is bounded by 2000 polls of 100 us (200 ms).
 *  - finishSecure leaves the clock gating as inherited; an explicit enableHwcg polls P2S 3 x 1 us and fails without TXDONE.
 * The design profile (A830ProfileDesign) is ARCHITECTURE 4.8-4.11 as written above. */

/* reg: GPU-space dword index as in A830Regs.h (physical = 0x03D00000 + 4 * reg). The board's implementation maps the GX/GBIF
 * block (reg < 0x10000) through the GPU window and the GMUCX_/GMUAO_ blocks (0x1B400..0x27BFF) through the GMU window
 * (byte offset 4 * reg - 0x37000). */
struct A830CpIo {
    void *cookie;
    int (*write32)(void *cookie,uint32_t reg,uint32_t value);   /* zero on a completed access */
    int (*read32)(void *cookie,uint32_t reg,uint32_t *value);
    int (*barrier)(void *cookie);                                 /* dsb sy */
    void (*sleep_us)(void *cookie,uint32_t us);                   /* may block: the 100 us steps of the bring-up waits */
    void (*delay_us)(void *cookie,uint32_t us);                   /* spins: the 1 us steps of the P2S poll */
    uint64_t (*now_us)(void *cookie);                             /* monotonic; bounds every wait */
    void (*lock)(void *cookie);                                   /* optional: the device MMIO (aperture) lock */
    void (*unlock)(void *cookie);
};
/* The SMMU side of the start check (A830Smmu, ARCHITECTURE 4.5.5, 4.9). */
struct A830CpSmmu {
    void *cookie;
    /* Gate: the LPAC stream (SID1 -> slot 3 -> CB0) reads back routed ((S2CR & 0x030303FF) == 0x300 | cb). readback receives
     * the S2CR value. Anything but kIOReturnSuccess stops S6 before the SQE starts. */
    IOReturn (*lpacRoute)(void *cookie,uint32_t *readback);
    /* Record only: read sGFSR and the CB0/CB2 FSR, write the FSRs back to clear them, store into the status sticky words. */
    void (*recordSticky)(void *cookie);
};
/* Optional checks of the annibale profile that need other modules (A830Cp::setHealth; annibale 9.7 steps 5, 6 and 9). Each
 * hook may be null (that check is then skipped); the design profile never calls them. Neither is called with the device
 * MMIO lock held. */
struct A830CpHealth {
    void *cookie;
    /* The GPU SMMU's global fault status sGFSR (0x03DA0048), read only, never written back; zero on a completed read.
     * Sampled before and after the SQE start: bits set before are recorded (a stale value under Gunyah), bits that appear
     * across the start are fatal. Also sampled into the stall dump. */
    int (*global_fault)(void *cookie,uint32_t *sgfsr);
    /* Nonzero when the GMU side has seen a fault (annibale polls HFI in every round of its waits): checked by the status
     * check that closes hwInit and in every round of the idle wait; nonzero is fatal (A830ReasonGmuFault). */
    int (*gmu_fault)(void *cookie);
};
/* S6 inputs (board values and the M11 LLCC result). */
struct A830HwInitConfig {
    uint32_t ubwc_mode;          /* qcom,ubwc-mode: 5 (UBWC 5.0); 3 and 4 are encoded as KGSL does */
    uint32_t highest_bank_bit;   /* 16 (13..17) */
    uint32_t min_access_length;  /* qcom,min-access-length: 32 (64 sets mal) */
    uint64_t gmem_base;          /* UCHE_CCHE/SP_HLSQ GC GMEM range minimum: A830_GMEM_BASE (64 MiB) */
    uint64_t uche_trap_base;     /* UCHE trap and write-through bases: A830_UCHE_TRAP_ADDR */
    uint32_t llcc_scid;          /* step 9a: the active LLCC GPU slice id (9), 0 = LLCC off (pagani-gpu-llcc=0) */
};
/* The board defaults of ARCHITECTURE 4.8/4.14.3 (LLCC off). */
A830HwInitConfig A830HwInitDefaults();

/* Step numbers in the status block (ARCHITECTURE 8.3), written so the overlay's two hex digits read as the design's step:
 * S6 step n of 4.8 is 0x0n/0x1n (step 10 = 0x10, 9a = 0x9A), 0x18 the annibale status check that closes hwInit, 0x30-0x32
 * microcode/ring/LPAC check, 0x40 SQE start, 0x41 the ME_INIT idle wait; S7 step n of 4.11 likewise (8 = 0x08, 11 = 0x11).
 * Recovery steps 0x50-0x52. */
enum : uint16_t {
    A830CpStepClearHalt=0x01,A830CpStepSecvid=0x02,A830CpStepBusyMask=0x03,A830CpStepGmemRange=0x04,A830CpStepTrapBase=0x05,
    A830CpStepCounters=0x06,A830CpStepUbwc=0x07,A830CpStepBicubic=0x08,A830CpStepClientPf=0x09,A830CpStepScache=0x9A,
    A830CpStepNonctxt=0x10,A830CpStepHangDetect=0x11,A830CpStepGmuCounters=0x12,A830CpStepProtect=0x13,
    A830CpStepGmemSaveRestore=0x14,A830CpStepPipeMasks=0x15,A830CpStepIrqMasks=0x16,A830CpStepHealth=0x18,
    A830CpStepMicrocode=0x30,A830CpStepRing=0x31,A830CpStepLpacRoute=0x32,A830CpStepSqeStart=0x40,A830CpStepMeInit=0x41,
    A830CpStepSecureMode=0x08,A830CpStepGmemProtect=0x09,A830CpStepPwrup=0x10,A830CpStepHwcg=0x11,
    A830CpStepHaltSqe=0x50,A830CpStepHaltGbif=0x51,A830CpStepHaltGbifGx=0x52,
};
/* What a failed call reports for adreno830_status_fail (reg is the physical address of the last register touched). */
struct A830CpFailure {
    uint16_t step,reason;   /* A830Cp step, A830Reason */
    uint64_t reg,value,expect;
};
/* The 4.9 timeout dump, also used for the S6/S7 diagnostic page and the recovery snapshot. The annibale profile adds the
 * rest of annibale's timeout dump (kernel-sequences 9.7 step 10); those fields stay 0 in the design profile. */
struct A830CpStallPipe {
    uint32_t hw_fault,interrupt,pipe_status,sqe_status,protect_status;   /* 0x8B3, 0x8B7, 0x8B9, 0x8BB, 0x84F */
    uint32_t sqe_stat[16];                                               /* CP_SQE_STAT 0..15; [0] is the SQE PC */
    uint32_t icache_cntl;                                                /* annibale: CP_SQE_ICACHE_CNTL_PIPE 0x8B0 */
};
struct A830CpStall {
    uint32_t rbbm_status,rbbm_status1,int0_status,int0_unmasked;   /* 0x12, 0x13, 0x6A, GMUAO 0x23B15 (unmasked shadow) */
    uint32_t rptr,wptr,bv_rptr,shadow_rptr,shadow_bv_rptr;          /* registers and the memptrs shadows */
    uint32_t rl_error[2],cp2gmu,global_status,cx_busy;              /* 0x840/0x841, 0x822, 0x8B5, GMUAO 0x23B0C */
    A830CpStallPipe pipe[2];                                         /* BR, BV */
    /* annibale: RBBM_GFX_STATUS 0x15, GFX_BR/BV_STATUS 0x1A/0x1C, RBBM_MISC_STATUS 0x1E (bit 0: SMMU stalled on a fault),
     * CP_RB_CNTL, CP_SQE_INSTR_BASE, CP_RB_BASE, and sGFSR through the A830CpHealth hook. */
    uint32_t gfx_status,gfx_br_status,gfx_bv_status,misc_status,rb_cntl,sgfsr;
    uint64_t sqe_base,rb_base;
    bool valid;
};
/* The software watchdog's progress sample (ARCHITECTURE 4.13.4): any change of any field is progress. */
struct A830CpProgress {
    uint32_t fence;          /* memptrs completed fence */
    uint32_t rptr,bv_rptr;   /* CP_RB_RPTR, CP_RB_RPTR_BV */
    uint64_t ib1_base;       /* BR CP_IB1_BASE (aperture BR) */
    uint32_t ib1_rem;        /* BR CP_IB1_REM_SIZE */
};
/* CP_HW_ERROR detail (Linux a8xx_cp_hw_err_irq), read in the interrupt bottom half under the MMIO lock. */
struct A830CpFault {
    uint32_t global;                  /* CP_INTERRUPT_STATUS_GLOBAL */
    uint8_t pipes;                    /* bit p: pipe p reported a hardware or software fault */
    uint32_t hw[8],sw[8];             /* CP_HW_FAULT_STATUS_PIPE, CP_INTERRUPT_STATUS_PIPE of each reported pipe */
    uint32_t opcode[8],protect[8];    /* CP_SQE_STAT[1] when sw bit 8 (opcode error), CP_PROTECT_STATUS_PIPE when sw bit 10 */
};
/* Per-boot counters and records (never gates). */
struct A830CpRecord {
    uint32_t rptr_at_start,wptr_at_start;   /* CP_RB_RPTR/WPTR read in startRing */
    uint32_t sqe_cntl_readback;             /* CP_SQE_CNTL after the start write */
    uint32_t lpac_route;                    /* the LPAC S2CR the SMMU callback read */
    uint32_t gbif_halt,rbbm_gbif_halt;      /* the read-back of the halt clears in step 1 */
    uint32_t cx_busy_idle;                  /* GMUAO GPU_CX_BUSY_STATUS at the last idle */
    uint32_t p2s_status,hwcg_timeouts;      /* RBBM_CGC_P2S_STATUS, P2S polls that timed out */
    uint32_t idle_us;                       /* the last waitIdle's duration */
    uint32_t gbif_ack[3];                   /* recovery halt acknowledges: RBBM GX, client, arbiter */
    uint32_t hw_inits,cp_starts,secure_exits;
    uint32_t sgfsr_before,sgfsr_after;      /* annibale with the A830CpHealth hook: sGFSR around the SQE start */
};

class A830Cp final {
    A830CpIo io_{};
    A830CpSmmu smmu_{};
    A830CpHealth health_{};
    NativeAdrenoRing *ring_=nullptr;
    A830CpFailure failure_{};
    A830CpStall stall_{};
    A830CpRecord record_{};
    uint32_t aperture_=0xffffffffu;   /* cached CP_APERTURE_CNTL_HOST; unknown until the first write */
    uint32_t gmem_protect_=0;
    bool attached_=false,pwrup_patched_=false,io_error_=false,secure_=true;
    IOReturn fail(uint16_t step,uint16_t reason,uint32_t reg,uint64_t value,uint64_t expect,IOReturn result);
    IOReturn write(uint16_t step,uint32_t reg,uint32_t value);
    IOReturn read(uint16_t step,uint32_t reg,uint32_t *value);
    IOReturn write64(uint16_t step,uint32_t reg,uint64_t value);
    IOReturn writeChecked(uint16_t step,uint32_t reg,uint32_t value);
    IOReturn aperture(uint16_t step,uint32_t pipe);
    IOReturn pipeWrite(uint16_t step,uint32_t pipe,uint32_t reg,uint32_t value);
    IOReturn pipeRead(uint16_t step,uint32_t pipe,uint32_t reg,uint32_t *value);
    IOReturn barrier(uint16_t step);
    void lock();
    void unlock();
    uint64_t now() const;
    IOReturn initLocked(const A830HwInitConfig &);
    IOReturn checkStatus(uint16_t step);
    IOReturn globalFault(uint16_t step,uint32_t *sgfsr);
    IOReturn idleState(uint32_t target,bool *idle);
    IOReturn idle(uint32_t timeout_us,uint16_t step);
    IOReturn idleAnnibale(uint32_t polls,uint16_t step);
    void dumpLocked();
public:
    A830Cp()=default;
    A830Cp(const A830Cp &)=delete;
    A830Cp &operator=(const A830Cp &)=delete;
    /* Binds the MMIO callbacks, the ring this CP executes (attached or attached later, before startRing) and the SMMU check
     * (may be null: startCp then has no LPAC gate, for host tests only). No register access. */
    IOReturn attach(const A830CpIo *,NativeAdrenoRing *,const A830CpSmmu *smmu=nullptr);
    /* The annibale profile's optional hooks (copied; null clears); independent of attach. */
    void setHealth(const A830CpHealth *);
    /* S6 steps 1-17 (ARCHITECTURE 4.8) under the MMIO lock; the aperture is 0 at the end. Re-runnable (recovery). The
     * annibale profile writes annibale's list instead (see the top of this file) and ends with its status check (step 0x18,
     * outside the lock): an error bit is A830ReasonCpFault (A830ReasonHang for HANG_DETECT), a read-back that differs is
     * A830ReasonReadback at the register concerned. */
    IOReturn hwInit(const A830HwInitConfig &);
    /* One pipe register: aperture = pipe << 12, the write, aperture back to 0, under the MMIO lock. An access error is kept
     * in failure() and fails the next step that checks it. */
    void writePipe(uint8_t pipe,uint32_t reg,uint32_t value);
    IOReturn readPipe(uint8_t pipe,uint32_t reg,uint32_t *value);
    /* S4 helper: the CP image of a pinned SQE/AQE file (ARCHITECTURE 4.9): bytes [4, fw_bytes) into dst, the rest of dst
     * zeroed, then the caller's barrier. dst_bytes must hold the image (SQE 0x1B510 into 0x1C000, AQE 0x62F8 into 0x7000). */
    static IOReturn stageMicrocode(const uint8_t *fw,uint64_t fw_bytes,void *dst,size_t dst_bytes,size_t *copied);
    /* CP_SQE_INSTR_BASE = sqe_iova (0x100000) and, when aqe_iova != 0, CP_AQE_INSTR_BASE_0 = aqe_iova (0x180000); AQE_1 is
     * never written (no LPAC). 4 KiB aligned, below 2^48. annibale: each base followed by a barrier and read back equal. */
    IOReturn loadMicrocode(uint64_t sqe_iova,uint64_t aqe_iova);
    /* KGSL gen8_rb_start on a fresh ring (write index 0: NativeAdrenoRing::attach or ::recover cleared the ring words and
     * the memptrs page; anything else is kIOReturnNotReady): CP_RB_RPTR_ADDR = memptrs + 0, CP_RB_RPTR_ADDR_BV (0x808) =
     * memptrs + 8, CP_RB_CNTL = BUFSZ | BLKSZ 2 (0x20C for 32 KiB), CP_RB_BASE = ring; then reads CP_RB_RPTR/WPTR into
     * record() (no criterion). The IOVAs must be the attached ring's. Writes no memory.
     * annibale (9.7 steps 3-4): CP_RB_RPTR and CP_RB_WPTR are read first and must both be 0 (else kIOReturnNotReady,
     * A830ReasonUnexpectedState), then CP_RB_BASE, CP_RB_CNTL, CP_RB_RPTR_ADDR, CP_RB_RPTR_ADDR_BV, each half written,
     * fenced by a barrier and read back equal. */
    IOReturn startRing(uint64_t ring_iova,uint64_t ring_bytes,uint64_t memptrs_iova);
    template<class Buffer> IOReturn startRing(const Buffer &ring,const Buffer &memptrs)   /* NativeAdrenoGpuBuffer */
    {return startRing(ring.iova,ring.bytes,memptrs.iova);}
    /* The LPAC route gate and the sticky record (SMMU callback), CP_SQE_CNTL = 1 (read back only recorded), then
     * CP_THREAD_CONTROL(CONCURRENT_BIN_DISABLE) + CP_ME_INIT {0x14B, 3, 0x20000000, 2, pwrup lo, hi, BIT(31)} as 10 ring
     * words with a fenced WPTR write, then waitIdle(2 s). fence receives the ring index the CP must reach (10 on a fresh
     * ring). pwrup_iova (0x301000) may point at a zero page: the list is filled after the ZAP (patchPwrup).
     * annibale (9.7 steps 5-9): sGFSR sampled before the start and after its barrier (A830CpHealth; new bits are
     * A830ReasonSmmuFault), CP_SQE_CNTL must read back 1 (A830ReasonReadback, nothing written to the ring), and the wait is
     * annibale's criterion for at most 2000 polls of 100 us (200 ms). */
    IOReturn startCp(uint64_t pwrup_iova,uint32_t *fence);
    /* The 4.9 criterion, 100 us sleeps up to timeout_us: CP_RB_RPTR and the memptrs RPTR shadow equal the ring's write
     * index, RBBM_STATUS has no bit but bit 0, RBBM_INT_0_STATUS has no HANG_DETECT (bit 23). GPU_CX_BUSY_STATUS is only
     * recorded. kIOReturnTimeout leaves the reason in failure() (dumpStall gives the detail).
     * annibale (9.7 step 9), ceil(timeout_us / 100) polls with a 100 us sleep after each: every poll first runs the status
     * check (the A830CpHealth GMU hook, RBBM_INT_0_STATUS & 0x33C002C2, CP_INTERRUPT_STATUS_GLOBAL & 0x007F007F; any is fatal
     * at once, kIOReturnIOError), CP_RB_RPTR, CP_RB_RPTR_BV and both memptrs shadows must lie inside the ring (else fatal),
     * and the GPU is idle when CP_RB_RPTR and the BR shadow equal the write index, GPU_CX_BUSY_STATUS bit 23 is clear and
     * RBBM_STATUS & ~1 is 0, confirmed by one more status check. */
    IOReturn waitIdle(uint32_t timeout_us);
    /* The 4.9 timeout dump into stall() (MMIO lock, aperture BR/BV, back to 0) and, when status is not null, the S6/S7 page:
     * aux[0] = CP_RB_RPTR | CP_RB_WPTR << 32, aux[1] = RBBM_STATUS | BR CP_HW_FAULT_STATUS_PIPE << 32,
     * aux[2] = BR SQE PC | BV SQE PC << 32, aux[3] = BR CP_PROTECT_STATUS_PIPE | RBBM_INT unmasked shadow << 32.
     * (aux[4..5] stay for the ZAP row: x0/x1, step and WAITQ count.) The annibale profile also fills the annibale fields. */
    void dumpStall(Adreno830Status *status);
    /* S7 step 8: CP_SET_SECURE_MODE 0 through the ring (fenced WPTR), then waitIdle(timeout_us) (annibale: its criterion). */
    IOReturn exitSecure(uint32_t timeout_us);
    /* S7 step 9: RB_GC_GMEM_PROTECT (BR) = the non-context table's value (0x0C000000) once the GPU is non-secure; aperture 0. */
    IOReturn programGmemProtect();
    /* S7 step 10 (a8xx_patch_pwrup_reglist): reads the IFPC and pwrup lists (aperture 0) and the dynamic list for every pipe
     * BR..DDE_BV in its mask (aperture = pipe), then packs page (4096 bytes, the CPU view of GPU VA 0x301000) with
     * NativeAdrenoPwrup::buildGen8 and a barrier. */
    IOReturn patchPwrup(void *page);
    /* S7 step 11 (a8xx_set_hwcg on): RBBM_CGC_0_PC = 0x702, the AO HWCG table (GMU), RBBM_CLOCK_CNTL_GLOBAL = 1,
     * RBBM_CGC_GLOBAL_LOAD_CMD = 1, RBBM_CGC_P2S_TRIG_CMD = 1, RBBM_CGC_P2S_STATUS bit 0 polled 10 x 1 us, then
     * RBBM_CLOCK_CNTL_GLOBAL = 0. A P2S timeout is only counted (record().hwcg_timeouts) and, as in Linux and KGSL, leaves the
     * global clock override on (kIOReturnSuccess). Access errors are returned and kept in failure().
     * annibale (9.4): a barrier after the trigger, P2S polled 3 times with 1 us before each; without TXDONE the override stays
     * on and the result is kIOReturnTimeout (A830ReasonTimeout at RBBM_CGC_P2S_STATUS), as annibale treats it as fatal. */
    IOReturn enableHwcg();
    /* Clock gating off (a8xx_set_hwcg off): AO HWCG 0, RBBM_CLOCK_CNTL_GLOBAL = 1, RBBM_CGC_GLOBAL_LOAD_CMD = 0, and the GBIF
     * CX clock gating bit (GBIF_CX_CONFIG bit 0) cleared; for the recovery's diagnostics and the M13 experiments. */
    IOReturn disableHwcg();
    /* S7 steps 8-11 in order: exitSecure, programGmemProtect, patchPwrup (first time only; pwrup_page may be null to skip it),
     * enableHwcg. The OOB_GPU clear (step 12) is the GMU's. annibale (section 14 row 35): steps 8-10 only, the clock gating
     * stays as inherited in the first round. */
    IOReturn finishSecure(void *pwrup_page,uint32_t timeout_us);
    /* Recovery (ARCHITECTURE 4.13.4): CP_SQE_CNTL = 3 halts the SQE (Linux a8xx_recover when GX is on). */
    IOReturn haltSqe();
    /* Recovery: a8xx_bus_clear_pending_transactions with the GX side: RBBM_GBIF_HALT = 1 until RBBM_GBIF_HALT_ACK bit 0,
     * GBIF_HALT = 1 until GBIF_HALT_ACK bit 0, GBIF_HALT = 2 until bit 1, then GBIF_HALT = 0; each wait timeout_us at most
     * (100 ms, A830_T_GBIF_HALT_US). The acknowledges are recorded; a timeout is returned (the caller forces the GMU off anyway). */
    IOReturn haltGbif(uint32_t timeout_us);
    /* The watchdog sample (MMIO lock for the BR aperture). */
    IOReturn readProgress(A830CpProgress *);
    /* The CP_HW_ERROR detail (MMIO lock, apertures of the reported pipes, back to 0). */
    IOReturn readFault(A830CpFault *);
    bool attached() const {return attached_;}
    bool secure() const {return secure_;}                 /* true until exitSecure succeeded once */
    bool pwrupPatched() const {return pwrup_patched_;}
    uint32_t gmemProtect() const {return gmem_protect_;}
    const A830CpFailure &failure() const {return failure_;}
    const A830CpStall &stall() const {return stall_;}
    const A830CpRecord &record() const {return record_;}
    /* CP_ME_INIT payload word 0 (ARCHITECTURE 4.9): multiple contexts, error detection, default reset, no perfcounter
     * save/restore, power-up list with spinlock. */
    static uint32_t meInitMask() {return 0x0000014Bu;}
};
#endif
