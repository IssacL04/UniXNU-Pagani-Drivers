/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* OnePlus 13T (PKX110, SM8750) speaker path facts.
 *  - The phone's running devicetree (Android 15): qupv3_hub_i2c4 = i2c@990000 under the I2C hub wrapper @9c0000 ("qcom,i2c-geni",
 *    qcom,i2c-hub, SPI 468, qcom,clk-freq-out 400000), pins gpio72 "i2chub0_se4_l0" and gpio73 "i2chub0_se4_l1" (2 mA, bias-pull-up,
 *    qcom,i2c_pull); tfa98xx@34 reset-gpio <tlmm 2 0>, tfa98xx@35 reset-gpio <tlmm 164 0>, supply L15B (rpmh ldob15, 1.8 V); the
 *    i2s1 pin states (active: function i2s1_*, 4 mA, no pull, data1 pulled down; sleep: gpio, 2 mA, pull-down, input);
 *    msm_audio_ion iommus <apps_smmu 0x1001 0x80>, <apps_smmu 0x1041 0x20> with qcom,smmu-sid-mask <0 0xf>.
 *  - Upstream Linux (facts): pinctrl-sm8750.c (gpio72/73 function 1 i2chub0_se4, gpio121..124 function 1 i2s1_*, the CTL layout with
 *    eGPIO bits 11/12; pinctrl-sm8550/8650.c: the strong I2C pull-up is CTL bit 13), gcc-sm8750.c (gcc_qupv3_i2c_s_ahb_clk BIT(7),
 *    gcc_qupv3_i2c_core_clk BIT(8), gcc_qupv3_i2c_s4_clk BIT(14) of the vote register 0x52008, halt registers 0x23000 / 0x23004 /
 *    0x17074, gcc_qupv3_i2c_s4_clk_src RCG 0x17078 fed only by the 19.2 MHz XO), i2c-qcom-geni.c (400 kHz from 19.2 MHz: divider 2,
 *    high 5, low 11, cycle 22).
 *  - /odm/etc/backend_conf.xml: MI2S-LPAIF-RX-SECONDARY 48000 Hz, 2 channels, 32 bits. Downstream audio_prm.h: CLOCK_ID_SEC_MI2S_IBIT.
 *  - The period buffer: 0xD0000000 lies in the 13T's UEFI memory map row "DXE_Heap" (UEFI-Pagani_SM8750 oneplus13tPkg MemoryMapLib.c:
 *    0xBE360000 + 0x16AC3000, EfiConventionalMemory, which is also the Android memory node's range, so the hypervisor maps it for the
 *    HLOS), 0x4E23000 below the reserved HW_Fence row; nothing the DT reserves is near it. XNU's window is DarwinBoot's allocation from
 *    0x8c0000000 and nothing of XNU reaches below it. What UEFI itself allocated there ended at ExitBootServices, except runtime
 *    services, which XNU never calls; that no allocation of that boot stays in use is an inference, not a measurement (stage 9 zeroes the
 *    64 KiB and logs; pagani-speaker-pa moves it). sheng's gap 0xCCF80000..0xD8000000 is another device's map: its number proves
 *    nothing here, the row above does. */
#ifndef PAGANI_SPEAKER_BOARD_H
#define PAGANI_SPEAKER_BOARD_H

#define SPEAKER_TLMM_BASE 0x0f100000ull
#define SPEAKER_TLMM_PIN(n) (SPEAKER_TLMM_BASE + 0x1000ull * (n))
#define SPEAKER_GCC_BASE 0x00100000ull
#define SPEAKER_GCC_SIZE 0x60000u
#define SPEAKER_SE_BASE 0x00990000ull          /* I2C hub serial engine 4 */
#define SPEAKER_SE_SIZE 0x4000u
#define SPEAKER_SMMU_BASE 0x15000000ull        /* apps SMMU (PKX110 live DT apps-smmu@15000000) */
#define SPEAKER_SMMU_SIZE 0x100000u
#define SPEAKER_RSC_DRV2 0x16520000ull         /* apps RSC DRV-2 */
#define SPEAKER_RSC_SIZE 0x10000u
#define SPEAKER_CMD_DB 0x81c60000ull
#define SPEAKER_CMD_DB_SIZE 0x20000u
#define SPEAKER_RAIL "ldob15"                   /* L15B: the amplifiers' VDD (and the codec's) */
#define SPEAKER_RAIL_MV 1800u
#define SPEAKER_BUFFER_PA 0xd0000000ull
#define SPEAKER_BUFFER_BYTES 0x10000u

enum {
    /* TLMM pins */
    SpeakerPinSda = 72, SpeakerPinScl = 73,
    SpeakerPinSck = 121, SpeakerPinData0 = 122, SpeakerPinWs = 123, SpeakerPinData1 = 124,
    SpeakerPinResetTop = 2, SpeakerPinResetBottom = 164,
    /* TLMM CTL fields */
    SpeakerCtlPullNone = 0, SpeakerCtlPullDown = 1, SpeakerCtlPullUp = 3,
    SpeakerCtlFuncShift = 2, SpeakerCtlDriveShift = 6, SpeakerCtlOe = 1u << 9,
    SpeakerCtlEgpioPresent = 1u << 11, SpeakerCtlEgpioEnable = 1u << 12, SpeakerCtlI2cPull = 1u << 13,
    SpeakerCtlOwned = 0x3fffu,                 /* the bits a configuration sets (pull, function, drive, OE, eGPIO, I2C pull) */
    SpeakerIoOut = 1u << 1,
    /* GCC (the vote register is shared: updated only through Sm8750Platform's sm8750_gcc_vote_set, tag 'SPKR') */
    SpeakerGccVote = 0x52008, SpeakerGccVoteBits = (1u << 7) | (1u << 8) | (1u << 14),
    SpeakerGccHaltSahb = 0x23000, SpeakerGccHaltCore = 0x23004, SpeakerGccHaltS4 = 0x17074,
    SpeakerGccRcgCmd = 0x17078, SpeakerGccRcgCfg = 0x1707c,
    SpeakerCbcrHwCtl = 1u << 1,                /* the branch is under hardware clock gating (may read CLK_OFF while idle) */
    SpeakerRcgUpdate = 1u << 0, SpeakerRcgRootEn = 1u << 1,
    /* GENI I2C at 400 kHz from the 19.2 MHz XO */
    SpeakerI2cClkCfg = 2u << 4 | 1u,
    SpeakerI2cScl = 5u << 20 | 11u << 10 | 22u,
    /* the DSP side */
    SpeakerGprPort = 0x2001,                   /* our GPR port: every APM answer comes back to it */
    SpeakerRate = 48000,
    SpeakerChannels = 2,
    SpeakerPeriodFrames = 480,                 /* 10 ms, as PaganiAudio's ring */
    SpeakerPeriodBytes = SpeakerPeriodFrames * SpeakerChannels * 2,
    SpeakerSlots = 8,                          /* period slots in the DSP buffer (4 in flight) */
    SpeakerInFlight = 4,
    SpeakerAmps = 2,
};
#define SPEAKER_CBCR_CLK_OFF 0x80000000u
#define SPEAKER_QUIET_BIT 32u                   /* pagani_panel_probe_quiet: 1 panel, 2 USB, 4 Bluetooth, 8 Wi-Fi, 16 camera, this, 64 ADSP */
#define SPEAKER_QUIET_MS 1000u                  /* the writer looks every 500 ms at the longest, a round takes a few ms (PanelProbe.h) */
#define SPEAKER_GCC_OWNER 0x53504b52u          /* 'SPKR' in the shared lock words (the GCC vote word and DRV-2) */
/* The audio stream ids (msm_audio_ion): 0x1001 is the one the DSP's buffer addresses select (address | (0x1001 & 0xf) << 32). */
#define SPEAKER_SIDS {0x1001u, 0x1081u, 0x1041u, 0x1061u}
#define SPEAKER_SID_COUNT 4u

#endif
