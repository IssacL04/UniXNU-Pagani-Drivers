/* SPDX-License-Identifier: MIT */
#ifndef ADRENO830_START_H
#define ADRENO830_START_H
#include "Adreno830Board.h"
#include "Adreno830Device.h"

/* The orchestrator: adreno830_start() (declared in Adreno830Board.h) runs the stages S0..S10 of ARCHITECTURE 3.4 in
 * order, records each one in adreno830_status and stops at the first failure: no retry (except the GMU cold boot of S5,
 * at most three attempts with a forced power-off between them, 4.7.3), no rollback of hardware actions (except that a
 * failure after S3 puts the claimed streams back to FAULT, 4.5.7), and nothing is published. Read-back values the
 * hypervisor or the firmware controls (sGFSR, the CB FSRs, CP_RB_RPTR/WPTR at ring start, CP_SQE_CNTL, the GMU AO bits
 * 3/5, GMU2HOST bit 23) are only recorded (section 10); only functional bits are gates.
 *
 * Bring-up profile (A830Profile.h, boot argument pagani-gpu-profile, default 1): S0 checks it and sets
 * adreno830_profile_value before the cmd-db and vote work, so every module reads the same profile from S0 on; row 0 of the
 * overlay ends with p<profile>. The modules switch their own sequences; the orchestrator's part of the annibale profile
 * (REDMI K90 port, kernel-sequences sections 14 and 15) is:
 *  - S1: CX_AHB_{AON,GMU,CP,VBIF_SMMU,HOST}_CNTL = 0x1B12 once HUB runs at 200 MHz, read back exactly (a830CxAhbTimeout),
 *    and again after every CX power-up of a wake (row 6); the design profile writes them only with pagani-gpu-ahbto=1;
 *  - S5: the first GX_BW_PERF_VOTE (HFI M9) at the 160 MHz level, {1, 1, 2} on PKX110 (rows 2, 24); the S5 page adds the
 *    TCM words B4 read back, GENERAL_11 (gmu_general11) and RSC_STATUS0_DRV0 (Adreno830Status.h);
 *  - S6: A830Cp gets its A830CpHealth hooks (9.7 steps 5, 6, 9, 10): sGFSR read through the SMMU window and never written
 *    back, and the GMU side's fault state (A830Gmu::fatalSeen, else one read-only HFI poll of GMU2HOST bit 15 and the NMI
 *    bits); its CP_INIT criterion is bounded by 200 ms (row 33);
 *  - S6-S8 failures and hang verdicts: A830Hfi::drainLate (3 s of late GMU messages, field-notes 9-1) before the streams
 *    FAULT and the status block is written; the counts go to hfi_late ("L<messages>/<acks>/<errors>" on the pages);
 *  - S7, wakes and recoveries: the OOB_GPU of S5 stays set until the next cold boot (row 28, first round; only a P1
 *    slumber entry releases it, A830PowerState);
 *  - S8: GPU interrupt 332 is not attached and not a gate: the CP_MEM_WRITE check passes on the polled fence (16 ms slice)
 *    and the scratch read-back, the DCVS thread retires completed jobs once a second (row 36);
 *  - S10: the DCVS stays at or below 342 MHz unless pagani-gpu-dcvs-max is given (row 38, A830Dcvs).
 * Its two performance switches (Adreno830Board.h, both 0 by default, S0 checks maxlevel <= 16 and irq <= 1):
 *  - pagani-gpu-maxlevel=n: S0 builds the vote tables with A830VotesFullTable (the design's whole DT table, the first vote
 *    still {1, 1, 2}) and S10 hands n to A830Dcvs as the ceiling level (A830DcvsConfig::maxLevel);
 *  - pagani-gpu-irq=1: S8 attaches 332 (step 0x01) with A830IrqConfig::own_mask = the fence interrupt only and runs the
 *    CP_MEM_WRITE check with its interrupt criteria; a refused attach is recorded (irq_off bit 0), a fence interrupt that does
 *    not arrive or wake the waiter turns 332 off for the boot (A830Irq::fallBack); either way S8 carries on with the polled
 *    fences above, never failing for it. With 332 live the fence interrupt wakes waiters and kicks the retirement.
 * S0 hands both to the status block (adreno830_status_knobs): the RUN row ends with " m<n>" and " i1" (" i0" polled).
 * pagani-gpu-hwsched is reserved: anything but 0 stops S0 with reason 24.
 *
 * Bring-up register trace (pagani-gpu-trace, A830Trace.h, ARCHITECTURE 8.7): while adreno830_start runs, a power operation
 * re-runs a stage body or a recovery runs, every access through the tables bound here (rd/wr, the wrapped SMMU, GMU and
 * A830Irq windows, the SMC) is written to BootProgress's ramoops log before it is made, with begin/end lines per stage.
 *
 * Step numbers (ARCHITECTURE 8.3) are what a failure stores in adreno830_status.step, with reg/value/expect as listed. */

/* S0: board inputs. */
enum : uint16_t {
    A830S0StepBoard=0x01,      /* magic/version: reg 0, value = magic, expect = Adreno830BoardMagic */
    A830S0StepWindow=0x10,     /* + window index (Adreno830BoardWindows order): reg = phys, value = bytes */
    A830S0StepIdentity=0x20,   /* chip/SoC id: reg = 0 (chip) or 1 (SoC), value = id */
    A830S0StepIrq=0x30,        /* + interrupt index (Adreno830BoardIrqs order): reg = index, value = INTID */
    A830S0StepStreams=0x40,    /* SID/slot/bank layout: reg = sid gpu|lpac<<16|gmu<<32, value = slots, expect = banks */
    A830S0StepActlr=0x41,      /* reg 0, value = smmu_actlr */
    A830S0StepLevels=0x42,     /* the bins, the common table or the DDR table: reg = bin (0xff common, 0xfe DDR), value = count */
    A830S0StepPool=0x48,       /* reg 0, value = pool_mib */
    A830S0StepKnobs=0x49,      /* reg = Adreno830Knob, value = knob value, expect = limit (pagani-gpu-hwsched: reason 24) */
    A830S0StepFirmware=0x50,   /* + firmware (0 GMU, 1 SQE, 2 AQE, 3 ZAP): reg = index, value = length, expect = pinned sha256[0..7] */
    A830S0StepOptionalWindow=0x58,   /* + optional window, checked only when set: reg = phys, value = bytes */
    A830S0StepLlccWindow=0x5C,       /* + LLCC window index: reg = phys, value = bytes */
    A830S0StepMap=0x60,        /* mapping cmd-db (0), SMEM (1), qfprom (2) failed: reg = phys, value = bytes */
    A830S0StepCmdDb=0x61,      /* snapshot or parse: reg = cmd-db phys, value = Sm8750Rpmh status, expect = failed resource */
    A830S0StepSocInfo=0x62,    /* SMEM walk: reg = last offset, value = Sm8750SocInfo status */
    A830S0StepSocId=0x63,      /* reg = SMEM phys, value = id read, expect = board soc_id */
    A830S0StepLevelTable=0x64, /* level selection: value = status, expect = pagani-gpu-perf or the DCVS bounds */
    A830S0StepVotes=0x65,      /* the GX/CX/dependency, bandwidth and ACD tables (A830Votes): value = error step << 16 | value */
    A830S0StepGmuImage=0x66,   /* gen80000_gmu.bin block parse (A830GmuFirmware): value = IOReturn */
};
/* S1: power. 1..7 are Sm8750GpuPowerStep (reg = block base + offset, value = the trace value); QMP is 0 and never fails
 * the stage. */
enum : uint16_t {
    A830S1StepQmp=0x00,
    A830S1StepMap=0x08,        /* + window id: mapping failed, reg = phys, value = bytes */
    A830S1StepRails=0x10,      /* DRV-2 mx/cx vote: reg = Sm8750RpmhResult step, value = Sm8750Rpmh status, expect = tcs */
    A830S1StepCm3=0x20,        /* CM3_SYSRESET read (record only) */
    A830S1StepAhbTimeout=0x30, /* CX_AHB_*_CNTL (annibale profile, or pagani-gpu-ahbto=1): reg = phys of the register that
                                  failed, value = what it read (0xFFFFFFFF: no answer), expect = 0x1B12 */
};
/* S2: secure CX. */
enum : uint16_t {
    A830S2StepWaitq=0x01,      /* INTID 994 attach (record only) */
    A830S2StepProbe=0x02,      /* IS_CALL_AVAIL probes (record only) */
    A830S2StepInitRegs=0x03,   /* GPU_INIT_REGS: reg = function id, value = x0, expect = x1 */
    A830S2StepCn0=0x04,        /* host CN0 vote */
    A830S2StepDdr=0x05,        /* host DDR boot vote: expect = kBps */
    A830S2StepFuse=0x06,       /* SW_FUSE reads (record only) */
};
/* S3: SMMU. 4.5.3 precheck steps 1..6, 4.5.4 steps 1..16 plus the bank: CB0 +0x100, CB2 +0x200, LPAC +0x300; a bank
 * reset is 0x0F (+ bank). */
enum : uint16_t {
    A830S3StepIds=0x01,A830S3StepCr0=0x02,A830S3StepAcr=0x03,A830S3StepGfsr=0x04,A830S3StepRoutes=0x05,A830S3StepBanks=0x06,
    A830S3StepBankReset=0x0F,
    A830S3StepIrqs=0x20,       /* 710/712 attach (record only) */
    A830S3StepPool=0x30,       /* BO pool reservation: value = MiB asked, expect = MiB reserved */
    A830S3StepContext=0x31,    /* GMU (+0x200) or GPU (+0x100) context bring-up outside the 4.5.4 register steps */
    A830S3StepAperture=0x40,   /* CP_SMMU_APERTURE (record only) */
    A830S3Cb0=0x100,A830S3Cb2=0x200,A830S3Lpac=0x300,
};
/* S4: firmware memory. */
enum : uint16_t {
    A830S4StepTimer=0x01,      /* CX always-on counter seeding, once per boot */
    A830S4StepGmuFirmware=0x02,/* gen80000_gmu.bin parse and DDR blocks */
    A830S4StepSqe=0x03,A830S4StepAqe=0x04,
    A830S4StepBuffers=0x05,    /* ring, memptrs, pwrup cleared */
};
/* S5: GMU and HFI. B1..B8 = 1..8, M1..M9 = 0x11..0x19, OOB 0x20, GX confirmation 0x21. */
enum : uint16_t {
    A830S5StepHfiInit=0x10,A830S5StepM1=0x11,A830S5StepM9=0x19,
    A830S5StepDdrFloor=0x1A,   /* host DDR floor after the first GX_BW_PERF_VOTE answer */
    A830S5StepOob=0x20,A830S5StepGx=0x21,
    A830S5StepCounters=0x22,   /* GMU-side counters and AO clock gating (A830GmuStepCounters) */
    A830S5StepIrqs=0x23,       /* 336/337 attach (record only) */
    A830S5StepBind=0x24,       /* binding the GMU, its firmware memory and the HFI (once per boot) */
};
/* S6/S7: the module's own step numbers (A830CpStep*, A830ZapStep*: 4.8 and 4.11 steps written so the two hex digits read as
 * the design's step, 9a = 0x9A, SQE start 0x40, ME_INIT wait 0x41), plus the orchestrator's: */
enum : uint16_t {
    A830S6StepLlcc=0x20,       /* LLCC slices 9/11 (record only: a slice that fails is left out, step 9a then skipped) */
    A830S6StepAttach=0x21,     /* binding the CP to the ring and the SMMU check */
    A830S7StepZapInit=0x00,    /* binding the SCM and the metadata/region memory */
};
/* S8: the self-test's step numbers (A830S8Step* of A830SelfTest.h: 332 attach 0x01, CP_MEM_WRITE 0x02.., V4 0x24, V9a 0x28,
 * V9b 0x29, V9c 0x2A), plus: */
enum : uint16_t {
    A830S8StepStart=0x10,      /* A830Irq / A830Fence objects (thread calls) */
    A830S8StepCxHost=0x11,     /* 112 attach (record only) */
    A830S8StepTimer=0x12,      /* GMU AO counter attached for GetTimestamp */
};
/* S9: Metal service; S10: DCVS. */
enum : uint16_t {
    A830S9StepInfo=0x01,A830S9StepSubmit=0x02,A830S9StepPrepare=0x10,   /* + Adreno830MetalReport step */
    A830S9StepPublish=0x20,A830S9StepCommit=0x30,
    A830S10StepPower=0x01,     /* A830PowerState / A830Recovery binding */
    A830S10StepDcvs=0x02,      /* A830Dcvs::init and start (pagani-gpu-dcvs=0: one GX_BW_PERF_VOTE {perf, bus}) */
};

/* The windows, interrupts and firmware images S0 checks, in step order. */
inline constexpr Adreno830Window Adreno830Board::*Adreno830BoardWindows[]={
    &Adreno830Board::gpu,&Adreno830Board::rscc,&Adreno830Board::cx_dbgc,&Adreno830Board::cx_misc,&Adreno830Board::gmu,
    &Adreno830Board::gmu_ao,&Adreno830Board::gpucc,&Adreno830Board::gcc,&Adreno830Board::smmu,&Adreno830Board::cmd_db,
    &Adreno830Board::smem,&Adreno830Board::zap,&Adreno830Board::qfprom,&Adreno830Board::rsc_drv2};
/* Optional inputs of later milestones (AOSS QMP in M3, thermal in M11): zero means absent. */
inline constexpr Adreno830Window Adreno830Board::*Adreno830BoardOptionalWindows[]={
    &Adreno830Board::aoss_msgram,&Adreno830Board::ipcc,&Adreno830Board::tsens_tm};
enum : unsigned {Adreno830BoardIrqCount=10};
inline constexpr Adreno830Firmware Adreno830Board::*Adreno830BoardFirmware[]={
    &Adreno830Board::gmu_fw,&Adreno830Board::sqe_fw,&Adreno830Board::aqe_fw,&Adreno830Board::zap_fw};

#ifdef ADRENO830_HOST_TEST
extern "C" void adreno830_start_reset(void);   /* host tests only: forget that adreno830_start() already ran */
void adreno830_set_os(const Adreno830Os *os);  /* host tests only: the kernel services the stages use */
Adreno830Device *adreno830_device_for_test(void);
#endif
#endif
