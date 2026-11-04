/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* OnePlus 13T (PKX110) torch board facts, from the running Android device tree (build-userspace/a830-inputs/dt/running.dts). At
 * integration these may move to the device repository's package/PaganiPeripherals.h (board policy stays in the device repository); the
 * driver includes this file only when that header does not define PAGANI_TORCH_SID. */
#ifndef PAGANI_TORCH_BOARD_H
#define PAGANI_TORCH_BOARD_H

#ifndef PAGANI_TORCH_SID
/* SPMI bus 0 (qcom,spmi@c42d000, DT 11197: cnfg 0xc42d000, core 0xc400000, chnls 0xc500000, obsrvr 0xc440000), apps EE 0 (qcom,ee = 0) */
#define PAGANI_TORCH_SPMI_CORE     0xc400000ull
#define PAGANI_TORCH_SPMI_CORE_SZ  0x3000u
#define PAGANI_TORCH_SPMI_CNFG     0xc42d000ull
#define PAGANI_TORCH_SPMI_CNFG_SZ  0x4000u
#define PAGANI_TORCH_SPMI_CHNLS    0xc500000ull      /* 0x400000: only the one page of the flash module's APID is mapped */
#define PAGANI_TORCH_SPMI_OBS      0xc440000ull      /* EE 0's 0x8000 of the 0x80000 window */
#define PAGANI_TORCH_SPMI_EE       0u
/* qcom,pm8550@1 / qcom,flash_led@ee00 (DT 11361), reg = <0xee00> */
#define PAGANI_TORCH_SID           1u
#define PAGANI_TORCH_PID           0xeeu
/* qcom,camera-flash1..3 (DT 34747): torch-source = <torch_0 torch_1>, flash-source = <flash_0 flash_1>: the phone's LED is on channels 0
 * and 1; led_switch_2 lists all four (qcom,led-mask 0x0f) but only those two are configured. Both are driven at the same current. */
#define PAGANI_TORCH_CHANNELS      0x03u
/* qcom,torch_0/1 qcom,max-current-ma 500; this driver's hard cap per channel is 200 (TorchRegs.h PGT_TORCH_POLICY_MA); the runtime cap is
 * pagani-torch-max-ma, default 50. */
#define PAGANI_TORCH_HARD_MA       200u
#endif

#endif
