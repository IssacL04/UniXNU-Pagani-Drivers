/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* Register facts of the Qualcomm PMIC haptics module HAP530_HV (the PMIH010x of the OnePlus 13T, SPMI slave 7: HAPTICS_CFG at
 * 0xf000, HAPTICS_PATTERN at 0xf100, HAPTICS_HBOOST at 0xf200) and of the SPMI PMIC arbiter v7 write path. Facts only; no code of
 * the sources below is copied.
 *
 *   [Q] OnePlus sm8750 kernel, drivers/input/misc/qcom-hv-haptics.c (OnePlusOSS/android_kernel_oneplus_sm8750, branch
 *       oneplus/sm8750_b_16.0.0_oneplus_13t, blob 16a79e99): register map, HAP530 (hw_type 5, "PMIH010X") work-arounds, init, play,
 *       FIFO and stop sequences, OPLUS additions.
 *   [U] Fenglin Wu, "[PATCH v4 3/3] input: misc: Add Qualcomm SPMI PMIC haptics driver" (LKML, 2026-07-17): the minimal HAP530
 *       DIRECT_PLAY and FIFO sequences.
 *   [E] OnePlus 13s UEFI PmicDxe (BOOT.MXF.2.5.1-00265-PAKALA, the PmicDxeLa.patched.efi that UEFI-Pagani_SM8750 runs before XNU):
 *       pm_resource_manager dispatches peripheral TYPE 0x15 SUBTYPE 9/10 to pm_haptics_cfg_driver_init, which asserts SUBTYPE 0x0a and
 *       a digital revision below 5.2; its boot-vibration routine writes the CFG registers listed in its table.
 *   [A] Linux drivers/spmi/spmi-pmic-arb.c: arbiter v7 channel table, ownership table, RW and observer channels, command format.
 * Values not in a source are marked as inferences where they are used. */
#ifndef PAGANI_HAPTICS_REGS_H
#define PAGANI_HAPTICS_REGS_H

/* ---- every PMIC peripheral ([E]: pm_get_peripheral_info reads 0x00..0x05) ---- */
#define PGH_REG_REVISION1          0x00u   /* digital minor */
#define PGH_REG_REVISION2          0x01u   /* digital major: 5 on HAP530 CFG and PTN [Q] */
#define PGH_REG_REVISION3          0x02u
#define PGH_REG_REVISION4          0x03u
#define PGH_REG_TYPE               0x04u
#define PGH_REG_SUBTYPE            0x05u

#define PGH_HAPTICS_TYPE           0x15u   /* [E] haptics class */
#define PGH_CFG_SUBTYPE_HAP530     0x0au   /* [E] asserted by pm_haptics_cfg_driver_init */
#define PGH_HAP530_MAJOR           5u      /* [Q] HAP_CFG_V5 / HAP_PTN_V5 */

/* ---- HAPTICS_CFG [Q] ---- */
#define PGH_CFG_STATUS_DATA_MSB    0x09u
#define   PGH_AUTO_RES_CAL_DONE    0x20u   /* with CAL_TLRA_CL_STS selected */
#define   PGH_FIFO_EMPTY_FLAG      0x40u   /* with FIFO_REAL_TIME_STS selected */
#define   PGH_FIFO_FULL_FLAG       0x20u
#define   PGH_FIFO_FILL_MSB_MASK   0x1fu   /* HAP530 (V5) */
#define PGH_CFG_STATUS_DATA_LSB    0x0au
#define PGH_CFG_HPWR_INTF          0x0bu   /* bits 1:0: 0 disabled, 3 ready (HAP525 and later) */
#define   PGH_HPWR_MASK            0x03u
#define   PGH_HPWR_DISABLED        0x00u
#define   PGH_HPWR_READY           0x03u
#define PGH_CFG_FAULT_STATUS       0x0cu
#define   PGH_FAULT_SC             0x04u   /* short circuit */
#define   PGH_FAULT_AUTO_RES       0x02u
#define   PGH_FAULT_HPWR_RDY       0x01u
#define PGH_CFG_HW_CONFIG          0x0du
#define   PGH_HW_HV_DRIVER         0x02u   /* HV driver: VMAX up to 10 V, else 6 V */
#define PGH_CFG_RT_LRA_IMPEDANCE   0x0eu   /* x 200 mOhm on HAP530 */
#define PGH_CFG_INT_RT_STS         0x10u
#define   PGH_INT_FIFO_EMPTY       0x02u   /* below the FIFO empty threshold; DT interrupt <7 0xf0 1 1> is this bit */
#define PGH_CFG_EN_CTL             0x46u
#define   PGH_HAPTICS_EN           0x80u
#define PGH_CFG_DRV_CTRL           0x47u
#define PGH_CFG_VMAX               0x48u   /* 50 mV steps (HV) */
#define   PGH_VMAX_STEP_MV         50u
#define PGH_CFG_DRV_WF_SEL         0x49u
#define   PGH_DRV_WF_FMT           0x10u   /* 0: FIFO samples are two's complement */
#define   PGH_DRV_WF_SEL_MASK      0x03u   /* 0 square, 1 sine, 2 no modulation */
#define PGH_CFG_AUTO_SHUTDOWN      0x4au
#define   PGH_EN_WFHOLD_FIFO       0x04u   /* kept 0: no output held when the FIFO runs dry */
#define PGH_CFG_TRIG_PRIORITY      0x4bu
#define   PGH_SWR_IGNORE           0x10u   /* HAP530: set around SPMI plays */
#define PGH_CFG_SPMI_PLAY          0x4cu
#define   PGH_PLAY_EN              0x80u
#define   PGH_BRAKE_EN             0x08u
#define   PGH_PAT_SRC_MASK         0x07u
#define   PGH_SRC_FIFO             0u
#define   PGH_SRC_DIRECT           1u
#define PGH_CFG_BRAKE_MODE_CFG     0x50u   /* 7:6 mode (0 open loop), 3:2 sine gain, 1:0 brake shape */
#define   PGH_BRAKE_CFG_MASK       0xcfu
#define PGH_CFG_TLRA_OL_HIGH       0x5cu   /* 3:0, with LOW: the open-loop LRA period in 5 us steps */
#define PGH_CFG_TLRA_OL_LOW        0x5du
#define   PGH_TLRA_STEP_US         5u
#define   PGH_TLRA_MAX_US          20475u
#define PGH_CFG_DRV_DUTY_CFG       0x60u
#define PGH_CFG_ZX_WIND_CFG        0x62u
#define PGH_CFG_AUTORES_CFG        0x63u   /* 7 enable, 6:2 enable delay (half cycles), 1:0 error window */
#define   PGH_AUTORES_EN           0x80u
#define PGH_CFG_FAULT_CLR          0x66u
#define   PGH_FAULT_CLR_ALL        0x17u   /* ZX timeout (HAP530) | short circuit | auto-resonance error | HPWR ready */
#define PGH_CFG_VMAX_HDRM          0x67u   /* 6:0, 50 mV steps */
#define   PGH_HDRM_DEFAULT_MV      1500u   /* HAP530 restores 1.5 V after every SPMI play */
#define   PGH_VMAX_PLUS_HDRM_MAX   11000u
#define PGH_CFG_VSET_CFG           0x68u
#define PGH_CFG_ZX_SYNC_CFG        0x6fu
#define   PGH_EN_PDN_ZX_TO_FAULT   0x04u
#define PGH_CFG_MOD_STATUS_SEL     0x70u
#define PGH_CFG_MOD_STATUS_XT      0x71u
#define PGH_CFG_CAL_EN             0x72u
#define   PGH_CAL_RC_CLK_MASK      0x0cu
#define   PGH_CAL_RC_CLK_DISABLED  0x00u
#define   PGH_CAL_RC_CLK_AUTO      0x04u
#define PGH_CFG_RAMP_DN_CFG2       0x86u   /* [U] */
#define PGH_CFG_DIG_SPARE          0xb8u

/* status selections: MOD_STATUS_XT, MOD_STATUS_SEL [Q] */
#define PGH_STS_CAL_TLRA_CL        0x0000u
#define PGH_STS_LAST_GOOD_TLRA_CL  0x0003u
#define PGH_STS_HAP_DRV            0x0005u
#define PGH_STS_FIFO_REAL_TIME     0x8005u

/* ---- HAPTICS_PATTERN [Q][U] ---- */
#define PGH_PTN_FIFO_READY_STS     0x08u
#define PGH_PTN_FIFO_DIN_0         0x20u   /* 0x20..0x23: four samples in one 4-byte write */
#define PGH_PTN_FIFO_PLAY_RATE     0x24u   /* 3:0 FIFO rate, 7:4 pattern memory rate */
#define   PGH_FIFO_RATE_MASK       0x0fu
#define PGH_PTN_DIRECT_PLAY        0x26u
#define PGH_PTN_FIFO_EMPTY_CFG     0x2au   /* 6:0, 64 samples per step (HAP530) */
#define   PGH_FIFO_THRESH_UNIT     64u
#define   PGH_FIFO_THRESH_MASK     0x7fu
#define PGH_PTN_FIFO_DIN_1B        0x2cu
#define PGH_PTN_MEM_OP_ACCESS      0x2du
#define   PGH_MEM_FLUSH_RELOAD     0x01u
#define PGH_PTN_VISENSE_EN         0x46u
#define PGH_PTN_VISENSE_ADC_CFG    0x47u
#define PGH_PTN_CL_VDRIVE_CTL      0x4au
#define   PGH_EN_CL_VDRIVE         0x80u
#define PGH_PTN_BRAKE_AMP          0x70u   /* 0x70..0x77 */
#define   PGH_BRAKE_SAMPLES        8u
#define PGH_PTN_MMAP_FIFO          0xa0u
#define   PGH_MMAP_FIFO_EXIST      0x80u
#define   PGH_MMAP_FIFO_LEN_MASK   0x7fu   /* length / 64 - 1 */
#define   PGH_MEM_BYTES            8192u   /* HAP530 pattern + FIFO memory */
#define   PGH_MMAP_UNIT            64u
#define PGH_PTN_PATX_PLAY_CFG      0xa2u

/* FIFO play rates (PGH_PTN_FIFO_PLAY_RATE 3:0) [Q][U] */
#define PGH_RATE_T_LRA             0u
#define PGH_RATE_F_8KHZ            8u
#define PGH_RATE_F_16KHZ           9u
#define PGH_RATE_F_24KHZ           10u
#define PGH_RATE_F_32KHZ           11u
#define PGH_RATE_F_44P1KHZ         12u
#define PGH_RATE_F_48KHZ           13u

/* ---- HAPTICS_HBOOST [Q]: read only here. HAP530 runs without the SW_CTRL_HBST work-around, so the driver of [Q] never writes it
 * (the boost follows the haptics module); PaganiHaptics never writes it either. ---- */
#define PGH_HBST_STATUS4           0x0bu   /* bit 0: DTEST1, boost busy */
#define PGH_HBST_HW_CTRL_FOLLOW    0x41u   /* bit 7: the boost follows the haptics module */
#define   PGH_FOLLOW_HW_EN         0x80u
#define PGH_HBST_VREG_EN           0x46u
#define   PGH_HBST_VREG_ON         0x80u
#define PGH_HBST_CLAMP             0x70u
#define   PGH_HBST_CLAMP_5V        0x01u
#define   PGH_CLAMPED_VMAX_MV      5000u

/* HAP530 timing [Q]: the module enable waits 500 us first (VNDRV overshoot), the VNDRV LDO discharges for 500 us after HPWR goes to
 * disabled or ready, VMAX settles within 10 x 1 ms. */
#define PGH_EN_DELAY_US            500u
#define PGH_DISCHARGE_US           500u
#define PGH_HPWR_POLLS             10u
#define PGH_HPWR_POLL_US           1000u

/* ---- SPMI PMIC arbiter v7 [A] ---- */
#define PGH_ARB_VERSION            0x0000u     /* core: >= 0x70000000 and < 0x80000000 */
#define PGH_ARB_FEATURES           0x0004u     /* core: 10:0 channels of bus 0 */
#define PGH_ARB_CHAN_MAP(n)        (0x2000u + 4u * (n))   /* core: 19:8 PPID (SID << 8 | PID), 24 IRQ owner */
#define PGH_ARB_OWNER(n)           (4u * (n))             /* cnfg: 2:0 the EE that may write the APID */
#define PGH_ARB_OBS_EE_STRIDE      0x8000u     /* obsrvr: EE x 0x8000 + APID x 0x20 */
#define PGH_ARB_OBS_STRIDE         0x20u
#define PGH_ARB_CHNL_STRIDE        0x1000u     /* chnls: APID x 0x1000 (v7; v8 uses 0x200) */
#define PGH_ARB_CMD                0x00u
#define PGH_ARB_STATUS             0x08u       /* 0 done, 1 failure, 2 denied, 3 dropped */
#define PGH_ARB_WDATA0             0x10u
#define PGH_ARB_WDATA1             0x14u
#define PGH_ARB_RDATA0             0x18u
#define PGH_ARB_OP_EXT_WRITEL      0u          /* command: opcode << 27 | register << 4 | (bytes - 1) */
#define PGH_ARB_OP_EXT_READL       1u
#define PGH_ARB_POLLS              1000u       /* 1 us each, as the Linux driver */
#define PGH_ARB_FAILED             0x0eu       /* status: failure, denied or dropped */
#define PGH_ARB_DROPPED            0x08u       /* status: dropped alone: the arbiter did not carry the transaction out */
#define PGH_ARB_DROP_RETRIES       3u          /* with PghSpmi.retry_dropped, 100 us apart */
#define PGH_ARB_DROP_PAUSE_US      100u

#endif
