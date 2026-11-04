/* SPDX-License-Identifier: MIT */
#ifndef SM8750_LLCC_H
#define SM8750_LLCC_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* The GPU's slices of the SM8750 last-level cache controller (LLCC v6), ARCHITECTURE 4.15 (milestone M11, knob
 * pagani-gpu-llcc, default 0). XNU has no LLCC driver, so nobody has configured or activated the GPU slices:
 *   slice 9  (LLCC_GPU):    max_cap 5632 KiB, priority 1, fixed size, every bonus way, write_scid_en and
 *                           write_scid_cacheable_en; the GPU tags its traffic with it through GBIF_SCACHE_CNTL0/1
 *                           (written by A830Cp, ARCHITECTURE 4.8 step 9a, on every GX power-up);
 *   slice 11 (LLCC_GPUHTW): max_cap 512 KiB, priority 1, fixed size, every bonus way; the SMMU's table walks hit it
 *                           through their outer write-back attribute, no GPU register is involved.
 * Register semantics follow Linux drivers/soc/qcom/llcc-qcom.c (sm8750_cfg: llcc_v6_reg_offset, skip_llcc_cfg = false):
 * _qcom_llcc_cfg_program_v6 for the slice attributes and llcc_update_act_ctrl for (de)activation.
 *
 * Windows (DT cache-controller@24800000, all Device-nGnRE): channel 0 at 0x24800000 (the common status with the bank
 * count is read there), broadcast OR at 0x26800000 (writes reach every bank; a read returns the OR of the banks) and
 * broadcast AND at 0x26C00000 (a read returns the AND of the banks: ACT_COMPLETE on all of them).
 *
 * Policy (ARCHITECTURE 4.15): every register is read before it is written; an attribute that already holds a non-zero
 * (firmware) value is kept as it is; a write that does not read back abandons that slice only, and nothing here ever
 * makes the GPU start fail: a slice that cannot be activated only costs the acceleration. Under Gunyah, an EL1 access
 * the hypervisor refuses is a synchronous external abort, which is why the knob defaults to 0 until M11 qualifies it. */

enum {Sm8750LlccChannel0, Sm8750LlccBroadcastOr, Sm8750LlccBroadcastAnd, Sm8750LlccBlocks};

enum {
    Sm8750LlccChannel0Phys = 0x24800000,
    Sm8750LlccBroadcastOrPhys = 0x26800000,
    Sm8750LlccBroadcastAndPhys = 0x26C00000,
    Sm8750LlccWindowBytes = 0x200000,

    /* llcc_v6_reg_offset */
    Sm8750LlccCommonHwInfo = 0x64000,           /* broadcast OR: version, >= 0x06000000 */
    Sm8750LlccCommonStatus0 = 0x6400C,          /* channel 0: banks in [31:28] */
    Sm8750LlccAttr0Base = 0x41000,              /* + 64 * slice: res_ways */
    Sm8750LlccAttr1Base = 0x41008,              /* bonus_ways */
    Sm8750LlccAttr2Base = 0x41010,              /* cache_mode | probe_target_ways << 4 | fixed_size << 8 | priority << 12 */
    Sm8750LlccAttr3Base = 0x41014,              /* max_cap bytes / banks >> 6 */
    Sm8750LlccAttrStride = 64,
    Sm8750LlccWrsEn = 0x42080,                  /* + (slice / 32) * 4, bit slice % 32 */
    Sm8750LlccWrsCacheableEn = 0x42088,
    Sm8750LlccActCtrlStride = 0x1000,           /* TRP_ACT_CTRL(s) = s * 4K, TRP_STATUS(s) + 4, TRP_ACT_CLEAR(s) + 8 */
    Sm8750LlccStatusOffset = 4,
    Sm8750LlccActClearOffset = 8,

    Sm8750LlccVersion6 = 0x06000000,
    Sm8750LlccActTrig = 1u << 0,                /* ACT_CTRL_ACT_TRIG */
    Sm8750LlccOpcodeActivate = 1u << 1,         /* ACT_CTRL_OPCODE_ACTIVATE << ACT_CTRL_OPCODE_SHIFT */
    Sm8750LlccOpcodeDeactivate = 2u << 1,       /* ACT_CTRL_OPCODE_DEACTIVATE << ACT_CTRL_OPCODE_SHIFT */
    Sm8750LlccStatusActive = 1u << 0,           /* TRP_STATUS: the slice is active */
    Sm8750LlccStatusDeactive = 1u << 1,         /* TRP_STATUS: the slice is inactive */
    Sm8750LlccStatusActComplete = 1u << 4,      /* TRP_STATUS on broadcast AND: every bank finished */
    Sm8750LlccActClear = 1u << 0,
    Sm8750LlccAttr2FixedSize = 1u << 8,
    Sm8750LlccAttr2PriorityShift = 12,
    Sm8750LlccPollUs = 100,                     /* LLCC_STATUS_READ_DELAY: each poll gives up after 100 us */

    Sm8750LlccSliceCpuss = 1,                   /* firmware activate_on_init: only looked at */
    Sm8750LlccSliceGpu = 9,
    Sm8750LlccSliceGpuHtw = 11,
    Sm8750LlccSliceWrcache = 31,
    Sm8750LlccMaxSlice = 63,
    Sm8750LlccGpuMaxCapKib = 5632,
    Sm8750LlccGpuHtwMaxCapKib = 512,
};

/* 32-bit accesses at a byte offset inside one of the three windows; nonzero return = the access failed (host
 * models). write32 must not return before the store completed (str + dsb on the Device mapping). */
typedef struct {
    void *cookie;
    int (*read32)(void *cookie, unsigned block, uint32_t offset, uint32_t *value);
    int (*write32)(void *cookie, unsigned block, uint32_t offset, uint32_t value);
    int (*delay_us)(void *cookie, unsigned microseconds);
} Sm8750LlccIo;

typedef enum {
    Sm8750LlccOk,
    Sm8750LlccArgument,
    Sm8750LlccIoError,          /* an access reported failure */
    Sm8750LlccUnsupported,      /* version below 6.0, or a bank count outside 1..15 */
    Sm8750LlccReadback,         /* a written attribute did not read back: the slice is abandoned */
    Sm8750LlccTimeout,          /* ACT_COMPLETE did not rise, or the opposite state bit did not clear, in 100 us */
    Sm8750LlccNotProbed,        /* Sm8750LlccProbe has not succeeded */
} Sm8750LlccStatus;

/* One slice's static configuration (struct llcc_slice_config, the members sm8750_data uses for 9 and 11). */
typedef struct {
    uint32_t slice;
    uint32_t max_cap_kib;
    uint32_t priority;
    uint32_t fixed_size;
    uint32_t bonus_ways;
    uint32_t res_ways;
    uint32_t write_scid_en;
    uint32_t write_scid_cacheable_en;
} Sm8750LlccSliceConfig;

/* Steps of the trace (where a sequence stopped). */
typedef enum {
    Sm8750LlccStepNone,
    Sm8750LlccStepVersion,
    Sm8750LlccStepBanks,
    Sm8750LlccStepSurvey,
    Sm8750LlccStepAttributes,
    Sm8750LlccStepWriteScid,
    Sm8750LlccStepTrigger,
    Sm8750LlccStepComplete,
    Sm8750LlccStepState,
    Sm8750LlccStepClear,
    Sm8750LlccStepDone,
} Sm8750LlccStep;

/* State and evidence; one object per boot in static storage (zero until the first probe: Sm8750LlccEnableGpu probes
 * an object whose `probed` is 0), zeroed again by every Sm8750LlccProbe. */
typedef struct {
    uint32_t probed;                /* 1 once version and bank count were read and accepted */
    uint32_t version;               /* COMMON_HW_INFO */
    uint32_t status0;               /* COMMON_STATUS0 */
    uint32_t banks;                 /* status0 [31:28] (4 on SM8750) */
    uint32_t survey[4];             /* TRP_STATUS (OR view) at probe of slices 1, 31, 9, 11: what firmware activated */
    uint64_t configured;            /* bit = slice: attributes in place (written or kept) */
    uint64_t kept;                  /* bit = slice: at least one non-zero firmware attribute kept as found */
    uint64_t active;                /* bit = slice: activated by us (or found active) and not deactivated since */
    uint64_t abandoned;             /* bit = slice: given up after a readback mismatch, timeout or access failure */
    uint32_t reads, writes, polls;
    struct {
        uint32_t step;              /* Sm8750LlccStep of the first failure (Done when none) */
        uint32_t slice;
        uint32_t block;
        uint32_t offset;
        uint32_t value;             /* value read (0xffffffff when the read itself failed) */
        uint32_t expect;            /* value written or awaited */
        int32_t status;             /* Sm8750LlccStatus of the first failure */
    } trace;
} Sm8750Llcc;

/* The two GPU slices of sm8750_data (9, then 11). */
const Sm8750LlccSliceConfig *Sm8750LlccGpuSlices(unsigned *count);

/* ATTR2 and ATTR3 of a configuration for `banks` banks (pure). */
uint32_t Sm8750LlccAttr2(const Sm8750LlccSliceConfig *config);
uint32_t Sm8750LlccAttr3(const Sm8750LlccSliceConfig *config, uint32_t banks);

/* Step 0: COMMON_HW_INFO (broadcast OR) >= 6.0 and the bank count from COMMON_STATUS0 (channel 0), then a read-only
 * survey of TRP_STATUS of slices 1, 31, 9 and 11. Zeroes *llcc first. */
Sm8750LlccStatus Sm8750LlccProbe(const Sm8750LlccIo *io, Sm8750Llcc *llcc);

/* Step 1 for one slice: ATTR0..ATTR3 (+ WRS_EN/WRS_CACHEABLE_EN bits when the slice asks for them) through broadcast
 * OR. Each register is read first: a non-zero value is firmware's and is kept; a zero register that should not be zero
 * is written and must read back exactly. A mismatch or failed access abandons the slice (its later activation is
 * refused). The WRS bits are read-modify-write, other slices' bits untouched; the ALGO registers are never written. */
Sm8750LlccStatus Sm8750LlccConfigure(const Sm8750LlccIo *io, Sm8750Llcc *llcc, const Sm8750LlccSliceConfig *config);

/* Step 2 (llcc_update_act_ctrl): ACT_CTRL = opcode | TRIG, then opcode; ACT_COMPLETE on broadcast AND within 100 us;
 * the opposite state bit (DEACTIVE for activation, ACTIVE for deactivation) clear on broadcast OR within 100 us;
 * ACT_CLEAR = 1. Activation of a slice already active on every bank (AND view ACTIVE, OR view not DEACTIVE) writes
 * nothing; deactivation of a slice not active writes nothing. */
Sm8750LlccStatus Sm8750LlccActivate(const Sm8750LlccIo *io, Sm8750Llcc *llcc, uint32_t slice);
Sm8750LlccStatus Sm8750LlccDeactivate(const Sm8750LlccIo *io, Sm8750Llcc *llcc, uint32_t slice);

/* The M11 sequence for S6 (KGSL _llc_configure_gpu_scid + _llc_gpuhtw_slice_activate): probe when not probed yet,
 * then configure and activate slice 9 and slice 11, each independently. Ok only when both are active; otherwise the
 * first failure, with the other slice still attempted. Calling it again (every GX power-up) re-activates only what
 * is not active. Never a reason to fail a stage. */
Sm8750LlccStatus Sm8750LlccEnableGpu(const Sm8750LlccIo *io, Sm8750Llcc *llcc);
/* The slumber counterpart (KGSL adreno_llcc_slice_deactivate on GPU power-off): deactivate 9 and 11 if active. */
Sm8750LlccStatus Sm8750LlccDisableGpu(const Sm8750LlccIo *io, Sm8750Llcc *llcc);

/* The sub-cache id the GPU should tag its traffic with (GBIF_SCACHE_CNTL0/1, ARCHITECTURE 4.8 step 9a): 9 while the
 * GPU slice is active, -1 otherwise (then step 9a is skipped). */
int Sm8750LlccGpuScid(const Sm8750Llcc *llcc);

#ifdef __cplusplus
}
#endif
#endif
