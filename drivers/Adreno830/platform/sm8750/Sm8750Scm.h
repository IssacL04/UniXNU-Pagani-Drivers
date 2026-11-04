/* SPDX-License-Identifier: MIT */
#ifndef SM8750_SCM_H
#define SM8750_SCM_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Qualcomm secure channel manager (SCM) calls made by XNU at EL1, as Linux
 * drivers/firmware/qcom/qcom_scm-smc.c and qcom_scm.c make them (SMCCC, SMC64, SiP owner):
 *   x0 = function id, x1 = arginfo, x2..x5 = arguments; SMC #0; x0 = status, x1..x3 = results.
 *
 *  - INTERRUPTED (x0 = 1): reissue with x0 = 1, x1..x5 and x7 unchanged and x6 as returned
 *    (ARM_SMCCC_QUIRK_QCOM_A6 carries the session in x6).
 *  - WAITQ_SLEEP (x0 = 2, yielding calls): x1 = wait-queue context, x2 = call context. Wait until the
 *    TZ wait-queue interrupt (SPI 962 / INTID 994, edge) reports that context through the fast call
 *    GET_WQ_CTX, then continue with WAITQ_RESUME(call context). Without the interrupt, GET_WQ_CTX is
 *    polled every millisecond; with it, polling also starts once a wait has gone Sm8750ScmWaitqIrqGraceUs
 *    without our wake (an attached interrupt that is never delivered must not turn every wait into a
 *    timeout).
 *  - EBUSY (x0 = -12): release the lock, sleep 30 ms, reissue the request; at most 20 times.
 *  - Any other non-zero x0 is the request's error; x1 is the result of calls that return one (PAS).
 *    As in Linux, INTERRUPTED/WAITQ_SLEEP/EBUSY are compared as 64-bit values, but the final error code
 *    is the low 32 bits of x0 (qcom_scm_remap_error takes an int) and a PAS result is the low 32 bits
 *    of x1 (the PAS helpers return res.result[0] as an int).
 *
 * Yielding (standard) calls are serialised by the caller's lock and may sleep; interrupts stay enabled.
 * Fast calls take no lock and never sleep (usable from the interrupt handler). Every yielding request is
 * bounded by budget_us (default 1 s) and 10000 SMC instructions. The SMC instruction itself is an
 * injectable function, so the host tests drive every sequence; Sm8750ScmSmcInstruction is the real one. */

/* Function ids: ARM_SMCCC_CALL_VAL(type, SMC_64, OWNER_SIP, svc << 8 | cmd). */
#define SM8750_SCM_FID(fast, svc, cmd) \
    ((uint32_t)(((fast) ? 0x80000000u : 0u) | 0x40000000u | 0x02000000u | (((svc) & 0xffu) << 8) | ((cmd) & 0xffu)))
/* QCOM_SCM_ARGS: argument count in [3:0], the type of argument i in bits [5 + 2i : 4 + 2i]. */
#define SM8750_SCM_ARGS(count, t0, t1, t2, t3) \
    ((uint32_t)(((count) & 0xfu) | ((t0) & 3u) << 4 | ((t1) & 3u) << 6 | ((t2) & 3u) << 8 | ((t3) & 3u) << 10))

enum {
    Sm8750ScmVal = 0, Sm8750ScmRo = 1, Sm8750ScmRw = 2, Sm8750ScmBufVal = 3,       /* argument types */
    Sm8750ScmSvcBoot = 0x01, Sm8750ScmSvcPil = 0x02, Sm8750ScmSvcInfo = 0x06, Sm8750ScmSvcMp = 0x0c,
    Sm8750ScmSvcWaitq = 0x24, Sm8750ScmSvcGpu = 0x28,
    Sm8750ScmBootSetRemoteStateCmd = 0x0a,
    Sm8750ScmPasInitImageCmd = 0x01, Sm8750ScmPasMemSetupCmd = 0x02, Sm8750ScmPasAuthAndResetCmd = 0x05,
    Sm8750ScmPasIsSupportedCmd = 0x07, Sm8750ScmIsCallAvailCmd = 0x01, Sm8750ScmCpSmmuApertureCmd = 0x1b,
    Sm8750ScmWaitqResumeCmd = 0x02, Sm8750ScmWaitqGetWqCtxCmd = 0x03, Sm8750ScmWaitqGetInfoCmd = 0x04,
    Sm8750ScmGpuInitRegsCmd = 0x01,
    Sm8750ScmGpuPasId = 13,                                   /* GPU_PAS_ID (the ZAP shader) */
    Sm8750ScmZapShaderResume = 0,                             /* SCM_GPU_ZAP_SHADER_RESUME: SET_REMOTE_STATE state */
    /* gpu_req bits of GPU_INIT_REGS (include/linux/firmware/qcom/qcom_scm.h) */
    Sm8750ScmGpuAlwaysEn = 1u << 0, Sm8750ScmGpuBclEn = 1u << 1, Sm8750ScmGpuClxEn = 1u << 2,
    Sm8750ScmGpuTsenseEn = 1u << 3,
};
#define SM8750_SCM_GPU_INIT_REGS SM8750_SCM_FID(0, Sm8750ScmSvcGpu, Sm8750ScmGpuInitRegsCmd)          /* 0x42002801 */
#define SM8750_SCM_IS_CALL_AVAIL SM8750_SCM_FID(0, Sm8750ScmSvcInfo, Sm8750ScmIsCallAvailCmd)         /* 0x42000601 */
#define SM8750_SCM_CP_SMMU_APERTURE SM8750_SCM_FID(0, Sm8750ScmSvcMp, Sm8750ScmCpSmmuApertureCmd)     /* 0x42000c1b */
#define SM8750_SCM_PAS_INIT_IMAGE SM8750_SCM_FID(0, Sm8750ScmSvcPil, Sm8750ScmPasInitImageCmd)        /* 0x42000201 */
#define SM8750_SCM_PAS_MEM_SETUP SM8750_SCM_FID(0, Sm8750ScmSvcPil, Sm8750ScmPasMemSetupCmd)          /* 0x42000202 */
#define SM8750_SCM_PAS_AUTH_AND_RESET SM8750_SCM_FID(0, Sm8750ScmSvcPil, Sm8750ScmPasAuthAndResetCmd) /* 0x42000205 */
#define SM8750_SCM_PAS_IS_SUPPORTED SM8750_SCM_FID(0, Sm8750ScmSvcPil, Sm8750ScmPasIsSupportedCmd)    /* 0x42000207 */
#define SM8750_SCM_SET_REMOTE_STATE SM8750_SCM_FID(0, Sm8750ScmSvcBoot, Sm8750ScmBootSetRemoteStateCmd) /* 0x4200010a */
#define SM8750_SCM_WAITQ_RESUME SM8750_SCM_FID(0, Sm8750ScmSvcWaitq, Sm8750ScmWaitqResumeCmd)         /* 0x42002402 */
#define SM8750_SCM_WAITQ_GET_WQ_CTX SM8750_SCM_FID(1, Sm8750ScmSvcWaitq, Sm8750ScmWaitqGetWqCtxCmd)   /* 0xc2002403 */
#define SM8750_SCM_WAITQ_GET_INFO SM8750_SCM_FID(1, Sm8750ScmSvcWaitq, Sm8750ScmWaitqGetInfoCmd)      /* 0xc2002404 */

/* x0 values of the secure world (drivers/firmware/qcom/qcom_scm.h). */
enum {
    Sm8750ScmX0Ok = 0,
    Sm8750ScmX0Interrupted = 1,
    Sm8750ScmX0WaitqSleep = 2,
    Sm8750ScmX0Error = -1,
    Sm8750ScmX0InvalidArg = -2,
    Sm8750ScmX0InvalidAddr = -3,
    Sm8750ScmX0NotSupported = -4,
    Sm8750ScmX0NoMemory = -5,
    Sm8750ScmX0Busy = -12,
};
/* GET_WQ_CTX flags (QCOM_SMC_WAITQ_FLAG_WAKE_ONE / _WAKE_ALL). */
enum { Sm8750ScmWakeOne = 1u << 0, Sm8750ScmWakeAll = 1u << 1 };

enum {
    Sm8750ScmDefaultBudgetUs = 1000000,     /* per yielding request, EBUSY sleeps included */
    Sm8750ScmBusyRetries = 20,
    Sm8750ScmBusySleepUs = 30000,
    Sm8750ScmWaitqPollUs = 1000,
    Sm8750ScmWaitqIrqGraceUs = 10000,       /* interrupt mode: poll GET_WQ_CTX too once a wait is this old */
    Sm8750ScmMaxCalls = 10000,              /* SMC instructions per request */
    Sm8750ScmFastMaxCalls = 64,             /* fast calls: INTERRUPTED resumes are not expected */
    Sm8750ScmWaitqContexts = 16,            /* DT qcom,max-queues = 2 on PKX110 */
    Sm8750ScmIrqMaxContexts = 16,           /* GET_WQ_CTX rounds per interrupt (more_pending) */
    Sm8750ScmHistoryDepth = 8,
};

typedef struct {
    void *cookie;
    /* One SMC: regs[0..7] hold x0..x7 on entry and on return. Nonzero only if the call could not be issued
     * at all (host models); the kernel passes Sm8750ScmSmcInstruction. */
    int (*smc)(void *cookie, uint64_t regs[8]);
    uint64_t (*now_us)(void *cookie);                       /* monotonic */
    void (*sleep_us)(void *cookie, uint32_t microseconds);  /* blocks; WAITQ polling calls it with the lock held */
    /* Optional mutex around yielding calls (NULL: the caller serialises all SCM use). The holder may sleep
     * (WAITQ). */
    void (*lock)(void *cookie);
    void (*unlock)(void *cookie);
    /* Optional, needed once the wait-queue interrupt is attached: block until wake(event) or timeout
     * (assert_wait_timeout + thread_block / thread_wakeup). A lost wakeup only costs one timeout slice. */
    void (*wait)(void *cookie, const void *event, uint32_t timeout_us);
    void (*wake)(void *cookie, const void *event);
} Sm8750ScmOps;

typedef enum {
    Sm8750ScmOk,
    Sm8750ScmArgument,        /* bad parameters, missing ops, wrong call type for the entry point */
    Sm8750ScmIoError,         /* the smc op refused (host models only) */
    Sm8750ScmError,           /* the secure world returned an error in x0 (see x0) */
    Sm8750ScmBusy,            /* still EBUSY after the retries or the budget */
    Sm8750ScmTimeout,         /* budget or call limit reached while INTERRUPTED or in WAITQ */
    Sm8750ScmResultError,     /* x0 == 0 but the call's result in x1 is not (PAS) */
    Sm8750ScmWaitqError,      /* WAITQ_SLEEP with a bad context, or WAITQ_SLEEP from a fast call */
} Sm8750ScmStatus;

/* Outcome of one request; kept for the status block (x0/x1/calls/wq of the S2 and S7 rows). */
typedef struct {
    uint32_t fid, arginfo;
    uint64_t args[4];
    int64_t x0;                 /* final status from the secure world */
    uint64_t x1, x2, x3;        /* final results */
    uint32_t calls;             /* SMC instructions: resumes, WAITQ resumes and GET_WQ_CTX polls included */
    uint32_t interrupted;       /* INTERRUPTED continuations */
    uint32_t waitq;             /* WAITQ_SLEEP returns */
    uint32_t busy;              /* EBUSY retries (sleeps) */
    uint32_t wq_ctx, call_ctx;  /* the last WAITQ_SLEEP's contexts */
    uint32_t elapsed_us;
    int32_t status;             /* Sm8750ScmStatus */
} Sm8750SmcResult;

/* Probe bits (Sm8750ScmProbeGpu). */
enum {
    Sm8750ScmHasGpuInitRegs = 1u << 0,      /* 0x2801 */
    Sm8750ScmHasCpAperture = 1u << 1,       /* 0x0c1b */
    Sm8750ScmHasPasIsSupported = 1u << 2,   /* 0x0207 */
    Sm8750ScmProbed = 1u << 8,              /* every probe got an answer (x0 == 0) */
};

typedef struct {
    const Sm8750ScmOps *ops;
    uint32_t budget_us;                     /* per yielding request; 0 = Sm8750ScmDefaultBudgetUs */
    uint32_t irq_attached;                  /* Sm8750ScmSetIrqAttached */
    /* Wait-queue bookkeeping, updated with atomics by the interrupt handler or by GET_WQ_CTX polls. */
    uint32_t wakes[Sm8750ScmWaitqContexts];  /* wakes reported per wait-queue context */
    uint32_t irqs;                           /* wait-queue interrupts taken */
    uint32_t wq_errors, wq_bad_flags, wq_bad_ctx;
    int64_t wq_last_x0;                      /* last failing GET_WQ_CTX status */
    /* Written with the lock held. polls: GET_WQ_CTX polls of waiting threads; irq_fallbacks: interrupt-mode
     * waits that reached Sm8750ScmWaitqIrqGraceUs and polled as well (994 did not wake them in time: either
     * a long wait or an interrupt that is not delivered; irqs tells which). */
    uint32_t requests, smcs, polls, irq_fallbacks;
    uint32_t available;                     /* Sm8750ScmHas* | Sm8750ScmProbed from the last Sm8750ScmProbeGpu */
    Sm8750SmcResult last;
    Sm8750SmcResult history[Sm8750ScmHistoryDepth];
    uint32_t history_count;                 /* entries ever recorded; history[(n - 1) % depth] is the latest */
} Sm8750Scm;

void Sm8750ScmInit(Sm8750Scm *scm, const Sm8750ScmOps *ops);
/* Call once armcpu_spi_attach(994, edge, Sm8750ScmWaitqIrq, scm, ...) succeeded. */
void Sm8750ScmSetIrqAttached(Sm8750Scm *scm, int attached);

/* A yielding (standard) call: fid bit 31 clear, at most 4 arguments. Ok means x0 == 0; out may be NULL.
 * Busy: x0[31:0] is EBUSY after the retries; Error: any other nonzero x0. */
Sm8750ScmStatus Sm8750ScmCall(Sm8750Scm *scm, uint32_t fid, uint32_t arginfo, const uint64_t args[4],
                              Sm8750SmcResult *out);
/* A fast (atomic) call: fid bit 31 set. No lock, no sleep, not recorded in the history. */
Sm8750ScmStatus Sm8750ScmFastCall(Sm8750Scm *scm, uint32_t fid, uint32_t arginfo, const uint64_t args[4],
                                  Sm8750SmcResult *out);

/* INFO_IS_CALL_AVAIL(svc, cmd): *available = x0 == 0 && x1 != 0. */
Sm8750ScmStatus Sm8750ScmIsAvailable(Sm8750Scm *scm, uint32_t svc, uint32_t cmd, int *available, Sm8750SmcResult *out);
/* Probes GPU_INIT_REGS, CP_SMMU_APERTURE_ID and PAS_IS_SUPPORTED; *mask = Sm8750ScmHas* | Sm8750ScmProbed. */
Sm8750ScmStatus Sm8750ScmProbeGpu(Sm8750Scm *scm, uint32_t *mask);
/* WAITQ_GET_INFO (fast): queue count (x1[7:0]) and the wait-queue interrupt as a GIC INTID (x2[15:0]). */
Sm8750ScmStatus Sm8750ScmWaitqInfo(Sm8750Scm *scm, uint32_t *queues, uint32_t *intid, Sm8750SmcResult *out);

/* GPU_INIT_REGS(gpu_req), PKX110: ALWAYS_EN | TSENSE_EN. Ok for x0 == 0 and, as KGSL
 * gen8_scm_gpu_init_cx_regs does, for x0[31:0] == -4 (-EOPNOTSUPP after qcom_scm_remap_error); x1 is not
 * looked at. */
Sm8750ScmStatus Sm8750ScmGpuInitRegs(Sm8750Scm *scm, uint32_t gpu_req, Sm8750SmcResult *out);
/* CP_SMMU_APERTURE_ID for a context bank (qcom_scm_set_gpu_smmu_aperture): Ok for x0 == 0. */
Sm8750ScmStatus Sm8750ScmSetGpuSmmuAperture(Sm8750Scm *scm, uint32_t context_bank, Sm8750SmcResult *out);
/* PAS_IS_SUPPORTED(pas_id): *supported = x0 == 0 && x1 != 0. */
Sm8750ScmStatus Sm8750ScmPasIsSupported(Sm8750Scm *scm, uint32_t pas_id, int *supported, Sm8750SmcResult *out);
/* Peripheral authentication service as the ZAP loader (pas_id 13) needs it: metadata (4 KiB aligned,
 * physically contiguous), the relocatable region (0x9B09A000, 0x1000), authenticate and reset.
 * Ok only for x0 == 0 && x1[31:0] == 0 (Linux "ret ? : res.result[0]", returned as an int). */
Sm8750ScmStatus Sm8750ScmPasInitImage(Sm8750Scm *scm, uint32_t pas_id, uint64_t metadata_phys, Sm8750SmcResult *out);
Sm8750ScmStatus Sm8750ScmPasMemSetup(Sm8750Scm *scm, uint32_t pas_id, uint64_t phys, uint64_t bytes,
                                     Sm8750SmcResult *out);
Sm8750ScmStatus Sm8750ScmPasAuthAndReset(Sm8750Scm *scm, uint32_t pas_id, Sm8750SmcResult *out);
/* qcom_scm_set_remote_state(state, pas_id): BOOT_SET_REMOTE_STATE, ARGS(2) = {state, pas_id}; Ok only for x0 == 0 &&
 * x1[31:0] == 0 (Linux "ret ? : res.result[0]"). State Sm8750ScmZapShaderResume (0) re-initialises an already
 * authenticated ZAP shader (Linux a5xx_zap_shader_resume); the A830 driver uses it only to resume the ZAP after a
 * power collapse that lost its state (ARCHITECTURE 4.17 P1), never for the first load. */
Sm8750ScmStatus Sm8750ScmPasSetRemoteState(Sm8750Scm *scm, uint32_t state, uint32_t pas_id, Sm8750SmcResult *out);

/* The wait-queue interrupt handler (context = the Sm8750Scm). Primary interrupt context: GET_WQ_CTX fast
 * calls until no wake is pending, then one wake(). Edge-triggered, nothing to clear. */
void Sm8750ScmWaitqIrq(void *context);

#if defined(__aarch64__)
/* SMC #0 with x0..x7 in and out; x8..x17 clobbered (SMCCC). */
int Sm8750ScmSmcInstruction(void *cookie, uint64_t regs[8]);
#endif

#ifdef __cplusplus
}
#endif
#endif
