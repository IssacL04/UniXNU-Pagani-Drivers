/* SPDX-License-Identifier: MIT */
#ifndef SM8750_GPU_POWER_H
#define SM8750_GPU_POWER_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Cold power-up of the SM8750 (A830) GPU clock tree up to the point where the GMU
 * can be loaded: GCC GPU clocks, GPU_CC CX GDSC, GPU_CC_PLL0, the GMU and hub
 * RCGs and the GMU-side branches. GX power is left to the GMU.
 *
 * Register semantics follow Linux drivers/clk/qcom/gpucc-sm8750.c and
 * gcc-sm8750.c, as observed on PKX110 hardware:
 *  - critical CBCRs are only set, never read back (GPU_CC_CXO_AON_CLK reads
 *    0x80000000 after its enable bit is written);
 *  - a branch counts as on when CLK_OFF clears, when its NOC FSM reports ON
 *    (bits 30:28 == 2), or while it is in hardware clock gating (HW_CTL), e.g.
 *    gcc_gpu_gemnoc_gfx_clk reads 0x80000003 (clk_branch2_check_halt);
 *  - PLL and RCG state is read, but no step requires a write to read back
 *    except where Linux waits for the same bit.
 * The caller must already hold the cx.lvl and mx.lvl votes (>= LOW_SVS, the gpucc
 * required-opps) and map both blocks; GX belongs to the GMU. write32 must not
 * return before the store has completed (str + dsb sy on the Device mapping): the
 * sequence relies on it when it moves between GCC and GPU_CC. */

enum {Sm8750GpuGcc, Sm8750GpuGpuCc, Sm8750GpuBlocks};

typedef struct {
    void *cookie;
    int (*read32)(void *cookie, unsigned block, uint32_t offset, uint32_t *value);
    int (*write32)(void *cookie, unsigned block, uint32_t offset, uint32_t value);
    int (*delay_us)(void *cookie, unsigned microseconds);
} Sm8750GpuPowerIo;

typedef enum {
    Sm8750GpuPowerOk,
    Sm8750GpuPowerArgument,
    Sm8750GpuPowerIoError,
    Sm8750GpuPowerTimeout,
    Sm8750GpuPowerUnexpectedState,
} Sm8750GpuPowerStatus;

/* Steps, in order. The trace keeps the step and the last register touched, so a
 * failure can be shown as "step/block/offset/value" without any logging: on
 * UnexpectedState that is the first register found wrong, on IoError value is
 * 0xffffffff for a failed read. */
typedef enum {
    Sm8750GpuStepNone,
    Sm8750GpuStepGccAhb,
    Sm8750GpuStepResets,
    Sm8750GpuStepCxGdsc,
    Sm8750GpuStepGpll0,
    Sm8750GpuStepPll0,
    Sm8750GpuStepRcg,
    Sm8750GpuStepBranches,
    Sm8750GpuStepDone,
    Sm8750GpuStepOffBranches,
    Sm8750GpuStepOffRcg,
    Sm8750GpuStepOffPll0,
    Sm8750GpuStepOffGpll0,
    Sm8750GpuStepOffGdsc,
    Sm8750GpuStepOffDone,
} Sm8750GpuPowerStep;

/* Sm8750GpuPowerOff findings that do not stop the sequence (Linux only warns). */
enum {
    Sm8750GpuOffCxoStillOn = 1u << 0,       /* gpu_cc_cxo_clk kept CLK_OFF clear for 200 us */
    Sm8750GpuOffGmuRcgNotParked = 1u << 1,  /* UPDATE to XO did not latch within 500 us */
    Sm8750GpuOffHubRcgNotParked = 1u << 2,
    Sm8750GpuOffCxStillOn = 1u << 3,        /* CFG_GDSCR POWER_DOWN_COMPLETE clear: another voter keeps CX */
};

typedef struct {
    uint32_t step;
    uint32_t block;
    uint32_t offset;
    uint32_t value;
    uint32_t reads;
    uint32_t writes;
    uint32_t warnings;                      /* Sm8750GpuOff* bits */
} Sm8750GpuPowerTrace;

Sm8750GpuPowerStatus Sm8750GpuPowerOn(const Sm8750GpuPowerIo *io, Sm8750GpuPowerTrace *trace);

/* Reverse of Sm8750GpuPowerOn for slumber (design 4.2.3): branches off, both RCGs
 * parked on XO, PLL0 in standby with its configuration kept, the GPLL0 votes
 * dropped and the CX GDSC collapse requested. Votes are left to the caller, who
 * keeps them. Only register access failures stop it; see trace->warnings. A later
 * Sm8750GpuPowerOn brings everything back. */
Sm8750GpuPowerStatus Sm8750GpuPowerOff(const Sm8750GpuPowerIo *io, Sm8750GpuPowerTrace *trace);

#ifdef __cplusplus
}
#endif
#endif
