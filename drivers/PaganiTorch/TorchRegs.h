/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* Register facts of the Qualcomm PMIC flash LED module (the PM8550 of the OnePlus 13T, SPMI slave 1, base 0xee00: the "4-channel"
 * MV flash module) in torch mode, and of the SPMI PMIC arbiter v7 write path. Facts only; no code of the sources below is copied.
 *
 *   [L] Linux drivers/leds/flash/leds-qcom-flash.c (Qualcomm, GPL-2.0-only; the copy in the xring-research o1src common-ogki tree):
 *       TYPE/SUBTYPE values, the 4-channel register fields (status, safety timer, target current, module enable, current resolution,
 *       strobe, channel enable), the torch current formula and limits, the torch sequence (strobe off, module off, current, timer 0,
 *       module on, strobe on), the fault bits of the three status registers.
 *   [A] Linux drivers/spmi/spmi-pmic-arb.c: arbiter v7 channel/ownership tables, observer and RW channels, command format (the
 *       same facts as PaganiHaptics' HapticsRegs.h, copied with this module's prefix).
 *   DT  build-userspace/a830-inputs/dt/running.dts, node qcom,pm8550@1/qcom,flash_led@ee00 (DT 11361) and the camera's
 *       qcom,camera-flash1..3 (DT 34747): which channels the phone's LED is on and their limits (PaganiTorchBoard.h).
 * Values not in a source are marked as inferences where they are used. */
#ifndef PAGANI_TORCH_REGS_H
#define PAGANI_TORCH_REGS_H

/* ---- every PMIC peripheral ---- */
#define PGT_REG_REVISION1          0x00u   /* digital minor */
#define PGT_REG_REVISION2          0x01u   /* digital major */
#define PGT_REG_REVISION3          0x02u
#define PGT_REG_REVISION4          0x03u
#define PGT_REG_TYPE               0x04u
#define PGT_REG_SUBTYPE            0x05u

#define PGT_FLASH_TYPE             0x18u   /* [L] FLASH_TYPE_VAL */
#define PGT_FLASH_SUBTYPE_4CH      0x07u   /* [L] FLASH_SUBTYPE_4CH_VAL (PM8350C/PM8550 class); 0x03/0x04 are the 3-channel modules */

/* ---- the 4-channel module [L] (mvflash_4ch_regs) ---- */
#define PGT_FLASH_STATUS1          0x06u   /* bit 2 x ch: short circuit of channel ch */
#define PGT_FLASH_STATUS2          0x07u   /* bits 6:0, below */
#define   PGT_STS2_VPH_LOW         0x01u
#define   PGT_STS2_BCL_IBAT        0x02u
#define   PGT_STS2_BOB_ILIM_S1     0x04u
#define   PGT_STS2_BOB_ILIM_S2     0x08u
#define   PGT_STS2_OTST2           0x10u
#define   PGT_STS2_OTST1           0x20u
#define   PGT_STS2_BOB_THM_OVLD    0x40u
#define   PGT_STS2_MASK            0x7fu   /* the field is bits 6:0 */
#define PGT_FLASH_STATUS3          0x09u   /* bit 2 x ch: safety timer expired on channel ch */
#define PGT_FLASH_CHAN_TIMER(ch)   (0x3eu + (ch))   /* 7 enable, 6:0 x 10 ms; 0 (disabled) in torch mode */
#define   PGT_TIMER_EN             0x80u
#define PGT_FLASH_ITARGET(ch)      (0x42u + (ch))   /* 6:0: target current code; bit 7 outside the field (kept as read) */
#define   PGT_ITARGET_MASK         0x7fu
#define PGT_FLASH_EN_CTL           0x46u   /* bit 7: module enable (the field is that one bit) */
#define   PGT_MODULE_EN            0x80u
#define PGT_FLASH_IRES             0x49u   /* 3:0, bit ch: 1 = 5 mA steps (torch), 0 = 12.5 mA steps (flash) */
#define PGT_FLASH_CHAN_STROBE(ch)  (0x4au + (ch))   /* 6:0; bit 7 outside the field (kept as read) */
#define   PGT_STROBE_HW_SEL        0x04u   /* 1: hardware strobe pin, 0: software (CHAN_EN) */
#define   PGT_STROBE_EDGE          0x02u   /* 1: edge trigger, 0: level */
#define   PGT_STROBE_ACTIVE_HIGH   0x01u
#define   PGT_STROBE_FIELD         0x7fu
#define   PGT_STROBE_SW_TORCH      (PGT_STROBE_ACTIVE_HIGH)   /* SW strobe, level trigger, active high: what [L] writes */
#define PGT_FLASH_CHAN_EN          0x4eu   /* 3:0, bit ch: channel ch on (with the module enabled and a SW strobe) */
#define PGT_CHANNELS               4u

/* torch current [L]: per channel, IRES 5 mA in torch mode; code = (uA + 1) / 5000 - 1 (0 below one step), so the channel drives
 * (code + 1) x 5 mA; TORCH_CURRENT_MAX_UA 500 mA per channel (the device tree's torch_0..3 qcom,max-current-ma = 500 too). */
#define PGT_TORCH_STEP_UA          5000u
#define PGT_TORCH_MAX_MA           500u
#define PGT_TORCH_MIN_MA           5u
/* This driver's limit, a tenth of the phone's flash current and well under the 500 mA above: whatever a board header or a boot argument
 * says, no channel is ever driven above this, and the gate refuses the code of anything above it (TorchCore.c, TorchSpmi.c). */
#define PGT_TORCH_POLICY_MA        200u

/* ---- SPMI PMIC arbiter v7 [A] (as HapticsRegs.h) ---- */
#define PGT_ARB_VERSION            0x0000u     /* core: >= 0x70000000 and < 0x80000000 */
#define PGT_ARB_FEATURES           0x0004u     /* core: 10:0 channels of bus 0 */
#define PGT_ARB_CHAN_MAP(n)        (0x2000u + 4u * (n))   /* core: 19:8 PPID (SID << 8 | PID), 24 IRQ owner */
#define PGT_ARB_OWNER(n)           (4u * (n))             /* cnfg: 2:0 the EE that may write the APID */
#define PGT_ARB_OBS_EE_STRIDE      0x8000u     /* obsrvr: EE x 0x8000 + APID x 0x20 */
#define PGT_ARB_OBS_STRIDE         0x20u
#define PGT_ARB_CHNL_STRIDE        0x1000u     /* chnls: APID x 0x1000 (v7; v8 uses 0x200) */
#define PGT_ARB_CMD                0x00u
#define PGT_ARB_STATUS             0x08u       /* bit 0 done; bits 3:1 failure, denied, dropped */
#define PGT_ARB_WDATA0             0x10u
#define PGT_ARB_WDATA1             0x14u
#define PGT_ARB_RDATA0             0x18u
#define PGT_ARB_OP_EXT_WRITEL      0u          /* command: opcode << 27 | register << 4 | (bytes - 1) */
#define PGT_ARB_OP_EXT_READL       1u
#define PGT_ARB_POLLS              1000u       /* 1 us each, as the Linux driver */

#endif
