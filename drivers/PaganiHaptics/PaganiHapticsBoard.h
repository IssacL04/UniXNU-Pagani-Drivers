/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* OnePlus 13T (PKX110) haptics board facts, from the running Android device tree (build-userspace/a830-inputs/dt/running.dts,
 * node qcom,pmih010x@7/qcom,hv-haptics@f000) and the stock system properties. At integration these move to the device repository's
 * package/PaganiPeripherals.h (board policy stays in the device repository); the driver includes this file only when that header
 * does not define PAGANI_HAPTICS_SID. */
#ifndef PAGANI_HAPTICS_BOARD_H
#define PAGANI_HAPTICS_BOARD_H

#ifndef PAGANI_HAPTICS_SID
/* SPMI bus 0 (qcom,spmi@c42d000: cnfg 0xc42d000, core 0xc400000, chnls 0xc500000, obsrvr 0xc440000), apps EE 0 */
#define PAGANI_HAPTICS_SPMI_CORE     0xc400000ull
#define PAGANI_HAPTICS_SPMI_CORE_SZ  0x3000u
#define PAGANI_HAPTICS_SPMI_CNFG     0xc42d000ull
#define PAGANI_HAPTICS_SPMI_CNFG_SZ  0x4000u
#define PAGANI_HAPTICS_SPMI_CHNLS    0xc500000ull      /* 0x400000: only the pages of the written APIDs are mapped */
#define PAGANI_HAPTICS_SPMI_OBS      0xc440000ull      /* EE 0's 0x8000 of the 0x80000 window */
#define PAGANI_HAPTICS_SPMI_EE       0u
/* reg = <0xf000 0xf100 0xf200> under qcom,pmih010x@7: HAPTICS_CFG, HAPTICS_PATTERN, HAPTICS_HBOOST */
#define PAGANI_HAPTICS_SID           7u
#define PAGANI_HAPTICS_PID_CFG       0xf0u
#define PAGANI_HAPTICS_PID_PTN       0xf1u
#define PAGANI_HAPTICS_PID_HBST      0xf2u
/* nvmem hap_cfg_sdam = phandle 0x67d = sdam@9d00 of qcom,pmk8550@0 (SID 0): read only (LRA nominal ohm at 0x75) */
#define PAGANI_HAPTICS_SDAM_SID      0u
#define PAGANI_HAPTICS_PID_SDAM      0x9du
/* LRA: qcom,lra-period-us 0x16fa (5882 us, 170.0 Hz); OPLUS F0 detection target qcom,cl_lra-period-us 0x15f2 (5618 us) */
#define PAGANI_HAPTICS_T_LRA_US      5882u
#define PAGANI_HAPTICS_F0_MIN_DHZ    1600u             /* ro.oplus.haptic.lra_range_min 160 Hz */
#define PAGANI_HAPTICS_F0_MAX_DHZ    1800u             /* ro.oplus.haptic.lra_range_max 180 Hz */
/* qcom,vmax-mv 0x4ec (DIRECT_PLAY), effect qcom,wf-vmax-mv 0xe10, qcom,fifo-vmax-mv 0x1d1a (FIFO full scale, the most OPLUS
 * drives this actuator with), qcom,vbat-low-fifo-vmax-mv 0x1a90 below qcom,vbat-low-soc 5 % (20 % below -5.0 C) */
#define PAGANI_HAPTICS_VMAX_MV       1260u
#define PAGANI_HAPTICS_EFFECT_MV     3600u
#define PAGANI_HAPTICS_FIFO_MV       7450u
#define PAGANI_HAPTICS_VBAT_LOW_MV   6800u
#define PAGANI_HAPTICS_VBAT_LOW_SOC  5u
#define PAGANI_HAPTICS_VBAT_COLD_SOC 20u
#define PAGANI_HAPTICS_VBAT_COLD_DC  (-50)            /* 0.1 C */
/* qcom,fifo-empty-threshold 0x1e0: 480 samples (programmed as 7 x 64) */
#define PAGANI_HAPTICS_FIFO_THRESH   480u
/* qcom,brake-mode 0 (open loop), qcom,brake-pattern [ff 3f 1f], qcom,drv-sig-shape 1 and qcom,brake-sig-shape 1 (sine) */
#define PAGANI_HAPTICS_BRAKE_MODE    0u
#define PAGANI_HAPTICS_BRAKE_GAIN    0u
#define PAGANI_HAPTICS_DRV_SHAPE     1u
#define PAGANI_HAPTICS_BRAKE_SHAPE   1u
#define PAGANI_HAPTICS_BRAKE         {0xff, 0x3f, 0x1f, 0, 0, 0, 0, 0}
#define PAGANI_HAPTICS_BRAKE_LEN     3u
/* What the OPLUS probe writes after the init (Android runs with these): DRV_DUTY_CFG adaptive brake duty, 62.5 % drive, 75 % brake;
 * AUTORES_CFG auto resonance off, 6-cycle enable delay, 50 % error window. */
#define PAGANI_HAPTICS_DRV_DUTY      0x56u
#define PAGANI_HAPTICS_AUTORES       0x32u
/* effect_0..5 and primitive_0..8: all-0x7f FIFO data at T_LRA (one sample per period), these lengths; primitive_0 is 4 zero
 * samples (silence) in the device tree, kept as 0 cycles here */
#define PAGANI_HAPTICS_EFFECT_CYCLES {4, 6, 2, 3, 3, 6, 0, 2, 2, 2, 2, 2, 2, 2, 2}
#define PAGANI_HAPTICS_EFFECTS       15u
#endif

#endif
