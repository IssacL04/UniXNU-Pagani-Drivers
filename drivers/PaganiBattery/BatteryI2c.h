/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved.
 * The transfer phases (a write that holds the stop condition, then a read; data in on the TX watermark; a failed command ends with
 * CMD_DONE and its error bits; cancel, then abort, on a timeout) follow UniXNU-sheng package/GeniI2c.c (https://github.com/UniXNU/
 * UniXNU-sheng, commit 08cde20e); here they run polled and blocking, and the engine is brought up from cold. */
/* The fuel gauge's bus: I2C hub serial engine 1 of the SM8750 (qupv3_hub_i2c1, i2c@984000), 100 kHz, owned by the apps processor
 * (Android's gauge driver uses it; nothing else is on it). Nothing has to be left running by the boot loader:
 *   1 clocks   GCC branch votes 0x52008 bits 7 (gcc_qupv3_i2c_s_ahb_clk), 8 (gcc_qupv3_i2c_core_clk) and 11 (gcc_qupv3_i2c_s1_clk),
 *              each branch seen running (CBCR 0x23000, 0x23004, 0x17020; a branch in hardware clock gating counts as running); no
 *              bit is ever cleared. Bits 7 and 8 also serve the other hub engines (the speaker amplifiers on engine 4). The register
 *              is shared: every update takes the weak lock word sm8750_gcc_vote_owner, here and in PaganiWifi (PCIe) with the protocol
 *              Sm8750Platform's sm8750_gcc_vote_set uses for the touch (bits 18-21 and 26) and the speaker amplifiers (bits 7, 8, 14).
 *              That update writes anyway when a holder overstays 50 ms, so every transfer still reads the register first: bits found
 *              dropped are voted again and the branches seen running before the engine is touched (pagani_bi2c_xfer).
 *   2 root     the engine's RCG (0x17024/0x17028) runs from the 19.2 MHz XO, the only source it has; it is never reprogrammed.
 *   3 engine   it runs the I2C protocol (FW_REVISION bits 15:8 = 3), FIFO interface on, no command active (one left active is
 *              aborted); FIFO mode, 8-bit packing, divider 7 and SCL counters 10/12/26 (100 kHz from 19.2 MHz), no interrupt line
 *              (the status registers are polled).
 *   4 pins     gpio66 (SDA) and gpio67 (SCL) on function 1 (i2chub0_se1), 2 mA, no pull (the board has its own pull-ups), as the
 *              Android pin state "qupv3_hub_i2c1_*_active_oplus".
 * Register facts: upstream Linux gcc-sm8750.c, pinctrl-sm8750.c and i2c-qcom-geni.c, UniXNU's QcomGeniSpi (GENI common registers) and
 * the 13T running devicetree; no code of the Linux drivers is used. The owner maps the windows and gives the delay. */
#ifndef PAGANI_BATTERY_I2C_H
#define PAGANI_BATTERY_I2C_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* The register windows (the owner maps each; the offsets below are inside them). */
enum {PbiWinGccVote, PbiWinGccSe, PbiWinGccAhb, PbiWinSda, PbiWinScl, PbiWinSe, PbiWinCount};
#define PBI_GCC_VOTE_BASE 0x152000ull   /* GCC 0x100000 + 0x52000 */
#define PBI_GCC_SE_BASE 0x117000ull     /* GCC + 0x17000 */
#define PBI_GCC_AHB_BASE 0x123000ull    /* GCC + 0x23000 */
#define PBI_GCC_PAGE 0x1000u
#define PBI_TLMM_SDA 0xf142000ull       /* TLMM gpio66 (0xf100000 + 0x1000 * n) */
#define PBI_TLMM_SCL 0xf143000ull       /* TLMM gpio67 */
#define PBI_TLMM_PAGE 0x1000u
#define PBI_SE_BASE 0x984000ull
#define PBI_SE_SIZE 0x4000u

/* GCC */
#define PBI_VOTE 0x008u                 /* in PbiWinGccVote */
#define PBI_VOTE_S_AHB (1u << 7)
#define PBI_VOTE_CORE (1u << 8)
#define PBI_VOTE_S1 (1u << 11)
#define PBI_VOTE_BITS (PBI_VOTE_S_AHB | PBI_VOTE_CORE | PBI_VOTE_S1)
#define PBI_CBCR_S1 0x020u              /* in PbiWinGccSe */
#define PBI_RCG_CMD 0x024u
#define PBI_RCG_CFG 0x028u
#define PBI_CBCR_S_AHB 0x000u           /* in PbiWinGccAhb */
#define PBI_CBCR_CORE 0x004u
#define PBI_CBCR_OFF (1u << 31)
#define PBI_CBCR_HW_CTL (1u << 1)
#define PBI_RCG_ROOT_OFF (1u << 31)
/* TLMM CTL (offset 0 of each pin's page): pull 1:0, function 5:2, drive 8:6 (mA / 2 - 1) */
#define PBI_PIN_MASK 0x1ffu
#define PBI_PIN_I2C (1u << 2)           /* function 1, 2 mA, no pull */

/* GENI serial engine */
#define PBI_FORCE_DEFAULT 0x20u
#define PBI_OUTPUT_CTRL 0x24u
#define PBI_CGC_CTRL 0x28u
#define PBI_GENI_STATUS 0x40u
#define PBI_M_CLK_CFG 0x48u
#define PBI_IF_DISABLE 0x64u
#define PBI_FW_REVISION 0x68u
#define PBI_CLK_SEL 0x7cu
#define PBI_BYTE_GRAN 0x254u
#define PBI_DMA_MODE 0x258u
#define PBI_TX_PACK0 0x260u
#define PBI_TX_PACK1 0x264u
#define PBI_TX_LEN 0x26cu
#define PBI_RX_LEN 0x270u
#define PBI_SCL_COUNTERS 0x278u
#define PBI_RX_PACK0 0x284u
#define PBI_RX_PACK1 0x288u
#define PBI_M_CMD0 0x600u
#define PBI_M_CMD_CTRL 0x604u
#define PBI_M_IRQ_STATUS 0x610u
#define PBI_M_IRQ_EN 0x614u
#define PBI_M_IRQ_CLEAR 0x618u
#define PBI_S_IRQ_CLEAR 0x648u
#define PBI_TX_FIFO 0x700u
#define PBI_RX_FIFO 0x780u
#define PBI_RX_FIFO_STATUS 0x804u
#define PBI_TX_WATERMARK 0x80cu
#define PBI_RX_WATERMARK 0x810u
#define PBI_RX_RFR 0x814u
#define PBI_DMA_TX_IRQ_CLR 0xc44u
#define PBI_DMA_RX_IRQ_CLR 0xd44u
#define PBI_GSI_EVENT_EN 0xe18u
#define PBI_SE_IRQ_EN 0xe1cu
#define PBI_DMA_GENERAL_CFG 0xe30u

#define PBI_M_CMD_DONE (1u << 0)
#define PBI_M_CMD_OVERRUN (1u << 1)
#define PBI_M_ILLEGAL_CMD (1u << 2)
#define PBI_M_CMD_FAILURE (1u << 3)
#define PBI_M_CMD_CANCEL (1u << 4)
#define PBI_M_CMD_ABORT (1u << 5)
#define PBI_M_GP_NACK (1u << 10)        /* GP_IRQ1: address or data not acknowledged */
#define PBI_M_GP_BUS (1u << 12)         /* GP_IRQ3: bus protocol error */
#define PBI_M_GP_ARB (1u << 13)         /* GP_IRQ4: arbitration lost */
#define PBI_M_GP_ALL (0x3fu << 9)
#define PBI_M_TX_WATERMARK (1u << 30)
#define PBI_M_ERRORS (PBI_M_CMD_OVERRUN | PBI_M_ILLEGAL_CMD | PBI_M_CMD_FAILURE | PBI_M_GP_ALL)
#define PBI_M_WANTED (PBI_M_CMD_DONE | PBI_M_ERRORS | PBI_M_CMD_CANCEL | PBI_M_CMD_ABORT | PBI_M_TX_WATERMARK)
#define PBI_CTRL_ABORT (1u << 1)
#define PBI_CTRL_CANCEL (1u << 2)
#define PBI_PROTO_I2C 3u
#define PBI_OP_WRITE 1u
#define PBI_OP_READ 2u
#define PBI_STOP_STRETCH (1u << 2)
#define PBI_CLK_100K ((7u << 4) | 1u)                       /* divider 7, serial clock on */
#define PBI_SCL_100K ((10u << 20) | (12u << 10) | 26u)      /* high 10, low 12, cycle 26 */
#define PBI_PACK0 0x7f8feu                                   /* 8-bit words, 4 per FIFO word, first byte in bits 7:0 */
#define PBI_PACK1 0xffefeu

typedef struct {
    void *ctx;
    uint32_t (*read)(void *ctx, unsigned window, uint32_t offset);
    void (*write)(void *ctx, unsigned window, uint32_t offset, uint32_t value);
    void (*delay_us)(void *ctx, unsigned us);
    int (*vote_lock)(void *ctx, int take);    /* take: 0 when held (else busy); release: return ignored */
} PbiIo;

/* Bring-up stages: report.stage is the last one reached, report.error the failing step's code (0 when ready). */
enum {PbiStNone = 0, PbiStClocks = 1, PbiStRoot = 2, PbiStEngine = 3, PbiStPins = 4, PbiStReady = 5};

typedef struct {
    int stage, error;
    uint32_t vote_before, vote_after, voted;  /* voted: 1 when this driver wrote the vote register */
    uint32_t cbcr[3];                          /* S_AHB, CORE, S1 after the votes */
    uint32_t rcg_cmd, rcg_cfg;
    uint32_t fw, if_disable, geni_status;
    uint32_t clk_before, scl_before, clk_after, scl_after;
    uint32_t pin_before[2], pin_after[2];
} PbiReport;

typedef struct {
    PbiIo io;
    uint8_t ready;
    uint32_t transfers, failures, nacks, timeouts, bus_errors;
    uint32_t last_status;                     /* M_IRQ status bits of the last failure */
    int last_error;
    uint32_t revotes, dropped_vote;           /* transfers that found the clock votes dropped; the vote register they found */
    int revote_error;                         /* the last new vote: 0 back, -16 lock word owned, -5 not kept, -110 a branch stays off */
} PbiBus;

int pagani_bi2c_bringup(PbiBus *b, const PbiIo *io, PbiReport *report);
/* Writes txn (1..8) bytes to the 7-bit address, then, when rxn (1..4) is not 0, reads rxn bytes with a repeated start. 0 done;
 * -6 not acknowledged, -71 bus protocol error, -11 arbitration lost, -5 engine error, -61 no data, -110 timed out (the command was
 * cancelled or aborted), -16 the clock votes were found dropped and are not back (revote_error; the engine was not touched), -19 not
 * brought up, -22 bad arguments. */
int pagani_bi2c_xfer(PbiBus *b, uint8_t address, const uint8_t *tx, unsigned txn, uint8_t *rx, unsigned rxn);

#ifdef __cplusplus
}
#endif
#endif
