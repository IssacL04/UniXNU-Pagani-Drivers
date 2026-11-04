# Adreno830

A830 (Gen8, chip id `0x44050001`) kernel GPU driver for UniXNU (XNU, iPadOS 17.7.11 / 21H461) on SM8750, first board the
OnePlus 13T (PKX110). Source-level: every power, SMMU, GMU, command-processor and ZAP step is in this repository; no
closed binary is patched. User space (AdrenoMetal: metal2vulkan + Turnip) talks to it through the unchanged v2 ABI of
`NativeAdrenoABI.h`.

Design: [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) (with the revisions of section 10) and
[`docs/IMPLEMENTATION_PLAN.md`](docs/IMPLEMENTATION_PLAN.md) (milestones M0-M14, section 7); progress in
[`docs/PROGRESS.md`](docs/PROGRESS.md). Measured hardware facts win over every other source.

## Bring-up

The board (device repository, `package/PaganiGpuBoard.cpp`) fills `Adreno830Board` (`Adreno830Board.h`) from its
generated device-tree constants and the embedded gen80000 firmware, parses the `pagani-gpu-*` boot arguments
(`Adreno830ParseKnobs`) and calls `adreno830_start()` once, synchronously, from `native_legacy_display_late_probe()` when
`pagani-gpu=1`. The orchestrator (`Adreno830Start.cpp`, device object `g830`) runs the stages of ARCHITECTURE 3.4:

| Stage | Overlay | Work |
| --- | --- | --- |
| S0 | `INPUTS` | board and firmware pins, cmd-db snapshot, SMEM socinfo (SoC 618), speed-bin fuse, level table, vote tables, GMU image |
| S1 | `POWER` | optional AOSS QMP ACD message, host mx/cx votes, `Sm8750GpuPowerOn`, CX_AHB timeouts (annibale, or `pagani-gpu-ahbto=1`), `CM3_SYSRESET` recorded |
| S2 | `SECCX` | SCM wait-queue interrupt, `GPU_INIT_REGS`, host CN0 and DDR votes, software fuses recorded |
| S3 | `SMMU` | survey, sACR/sGFSR, CB2/CB0 reset, 710/712, BO pool, GMU context (CB2), GPU context (CB0, LPAC), aperture |
| S4 | `FWMEM` | CX timer seed (once per boot), GMU DDR blocks, SQE/AQE, ring/memptrs/pwrup |
| S5 | `GMU-HFI` | B1-B8 (at most 3 cold boots), HFI M1-M9, host DDR floor, OOB_GPU, GX confirmation, GMU counters |
| S6 | `CP` | optional LLCC slices, Gen8 hw_init, microcode bases, ring, `CP_SQE_CNTL`, `CP_ME_INIT` |
| S7 | `ZAP` | PAS 13, `CP_SET_SECURE_MODE 0`, GMEM_PROTECT, pwrup list, HWCG, OOB_GPU cleared (design profile only) |
| S8 | `FENCE` | interrupt 332 (design profile), fences, GMU AO timer, self-test (CP_MEM_WRITE, IRQ or polled fence, V4, V9a; V9b/V9c with `pagani-gpu-cohtest=1`) |
| S9 | `METAL` | DeviceInfo, `native_adreno_publish`, staged `AGXAccelerator` (`IOGPUFamily` loaded first) |
| S10 | `DCVS` | power state machine, recovery and the DCVS service thread (`pagani-gpu-dcvs=0`: one fixed vote) |

A failure stops at its stage with step, reason code, IOReturn and register (no retry but the S5 cold boot, no publication);
`pagani-gpu-stop=N` stops after S_N. With `pagani-gpu-trace=1` (the default while the bring-up runs on hardware;
`A830Trace.h`, ARCHITECTURE 8.7) every register access and SMC of S0-S10, of the power operations that re-run a stage body
and of a recovery is written to BootProgress's ramoops log before it is made (`g<S>.<step> <block> w|r <phys> ...`, polls
and long sequential blocks counted), so after a hang and a manual reset the last access line names the register that hung
the bus; `=2` writes every access, `=0` turns it off, and nothing is written while the ramoops log is off. While the GPU runs with a
diagnostic overlay (`pagani-gpu-status=2` or `3`, as the test images run; the default `1` logs nothing new), the DCVS thread also
writes at most one `gpu meter ...` line a minute, and only a changed one, whenever the ramoops log runs (no new boot argument;
ARCHITECTURE 8.8; `pagani-diag` adds the per-second `gpu sec` line, the `gpu noslot` refusals and the first `gpu va past 4 GiB`): the queue meter's job attribution (render-server service time, the external time ahead of each render-server
job and how often it exceeded 4 ms: preemption phase 0; the periods seen without the fence interrupt, whose figures do not count),
context entries, slot pressure, how the jobs in flight at a page fault compared with its FAR window (counted only: the FAR
window's connection is the one charged) and the CP_SMMU_APERTURE outcome (per-process page tables P0), then, with the call meter
on, the longest call of each kind, the longest wait for each lock, the imports that took the alias pass, the time of the VM's table
transactions, the zero fill of pooled BOs and the contiguous attempts outside the pool (`NativeAdrenoMeter.h`). `pagani-gpu-kopt=<bits>`
(default 63, all on; `NativeAdrenoKopt.h`, ARCHITECTURE 3.6 and 5.4; a board value is only needed to compare) has one bit per kernel
latency change of 2026-10-31: pooled BOs zeroed after the Memory lock is dropped, contiguous attempts only from 64 to 128 KiB with a
5 s backoff after a failure, one alias pass per map, cache maintenance pinning the CPU for 64 pages at a time, the call meter, and the
VM's clock. None of them has run on the device yet. The status block `adreno830_status` (`Adreno830Status.h`, ARCHITECTURE 8) is written
by the stages and modules, laid out as overlay rows (BootProgress, text rows 10-13; `pagani-gpu-status=0..3`) and, after
S9, the registry property `Adreno830Status` of `NativeAdrenoGpuPublished`. After the bring-up the power state machine
(`A830PowerState`) and the recovery (`A830Recovery`) re-run the stage bodies through the operations the orchestrator binds.
The default power policy is P0 (no slumber, no IFPC); `pagani-gpu-slumber=1|2` (P1, full collapse or keeping CX),
`pagani-gpu-ifpc=1` (P2) and `pagani-gpu-ahbto=1` (CX_AHB timeout detection) switch the M13 single-variable cases on.
The board-wide `pagani-screenoff` (default 0) adds the screen rule with its bit 1: while the system keeps the display off
(PaganiPanel's `pagani_screen_state`) and the GPU has been quiet for 1 s, the GPU slumbers (CX kept; bit 3: the full
collapse); submissions wake it as always, the display coming back wakes it at once at the touch level, touches on the dark
display are ignored, and a failed entry or wake ends the rule for the boot (ARCHITECTURE 4.17). Its bits 0 and 2 belong to
the CPU governor and the panel.
Submissions hold the GPU awake through `A830PowerState::acquire/release`; the display's `pagani_display_vsync/late/underrun`
hooks feed the DCVS.

### Bring-up profile

`pagani-gpu-profile` (`A830Profile.h`) selects how the stages bring the GPU up. S0 checks it (0 or 1) and sets
`adreno830_profile_value` before the cmd-db and vote work, so every module reads the same value from S0 on; row 0 of the
overlay then ends with `p1` or `p0` (left out only when a 64-66 character row has no room).

- `pagani-gpu-profile=1` (default), annibale: the sequences and values the REDMI K90 port (same SM8750/A830, same GMU
  firmware) proved on hardware under XNU, wherever the annibale comparison table (kernel-sequences section 14, first round of
  section 15) says to follow them. The 13T measurements still win in both profiles: the `Sm8750GpuPower` power-up and PLL,
  the host CX/MX top-level votes, the host DDR votes and SCM `GPU_INIT_REGS`. Each module switches its own part (SMMU
  context values and TLB maintenance, with one `TLBIASID` + `TLBSYNC = 1` after every VM map through the programmer's
  `sync_after_map`; GMU start registers and polled GMU interrupts, firmware read-back, HFI messages, CP hw_init and CP_INIT
  criteria); ARCHITECTURE section 11 item 14 lists every difference. The orchestrator's part:
  - S1 writes the five CX_MISC `CX_AHB_*_CNTL` = `0x1B12` once HUB runs at 200 MHz and reads each one back (S1 step `0x30`,
    reason 7 on a mismatch); a wake after a CX collapse writes them again;
  - the first `GX_BW_PERF_VOTE` (HFI M9) is the 160 MHz level, `{1, 1, 2}` on PKX110, and the DCVS starts from it;
  - S6 gives `A830Cp` its health hooks: sGFSR read through the SMMU window and never written back (new bits across the SQE
    start fail it), and the GMU side's fault state (a fatal bit already taken, else one read-only poll of GMU2HOST bit 15 and
    the CM3 NMI bits) in every round of its waits; CP_INIT is bounded by 200 ms;
  - a failure of S6-S8 and a hang verdict first read the GMU's late messages for 3 s (`A830Hfi::drainLate`, before the
    streams FAULT and the status block is written); the counts go to `hfi_late` and the S5-S8 diagnostic pages show
    `L<messages>/<acks>/<errors>`; the S5 page adds the TCM words read back, `GENERAL_11` and the RSC status;
  - OOB_GPU is set in S5 and never cleared until the next cold boot (also across wakes and recoveries; P1 slumber releases
    it on entry);
  - S8 does not attach GPU interrupt 332 and does not need it: the CP_MEM_WRITE check passes on the polled fence (16 ms
    slice) and the scratch read-back, V4/V9 run as usual, and the DCVS thread retires completed jobs once a second;
  - the DCVS stays at or below 342 MHz; `pagani-gpu-dcvs-max=<MHz>` lifts the cap (at most 734 MHz while ACD is off). Any
    GPU sensor at 70 C or more, or no usable reading, holds 160 MHz until all are below 60 C; a vote the GMU does not
    acknowledge ends the frequency changes for the boot.
  - two performance switches, both 0 (the first round) by default: `pagani-gpu-maxlevel=<n>` (0-16) hands the GMU the
    design's whole DT level table (the first vote still the 160 MHz `{1, 1, 2}`, the DDR rows still annibale's x = 0) and
    makes GMU level n the DCVS ceiling (beyond the table: its top; at most 734 MHz while ACD is off, an explicit
    `pagani-gpu-dcvs-max` lowers it, the 70/60 C rule stays); `pagani-gpu-irq=1` attaches GPU interrupt 332 in S8 with only
    the ring fence interrupt unmasked, so fence waits wake on it instead of their 16 ms slice; a refused attach or a fence
    interrupt that never arrives falls back to the polled fences for the boot, never an S8 failure. The RUN row then ends
    with `m<n>` and `i1` (`i0`: given but polled).
- `pagani-gpu-profile=0`, design: ARCHITECTURE as written (Linux/KGSL defaults), including 332 as the one hard interrupt
  of S8, OOB_GPU cleared at the end of S7, the 734 MHz DCVS ceiling (900 MHz with ACD) and the thermal loop of
  ARCHITECTURE 4.16 (2026-10-30): a ceiling that moves one level at a time around `pagani-gpu-ttarget` (95 C; down per
  500 ms above it unless falling, up per 2 s at least 5 C under it, never below 525 MHz outside the 105 C emergency, which
  takes 389 MHz at once), shown on the DCVS row as ` T95+`; `adreno830_thermal_state()` and `adreno830_cpu_hint()` feed
  PaganiArmCpu's governor (`pagani-cpufreq=2`), which then keeps the CPU clusters off their top levels.

`pagani-gpu-hwsched` is reserved for the GMU hardware scheduler, the proven fallback if the ring buffer fails after
CP_INIT on the 13T: only 0 exists in this build, and any other value stops S0 (step `0x49`, reason 24, `kIOReturnUnsupported`).

## Layout

- ABI shell: `NativeAdrenoPublish`, `NativeAdrenoAccelerator`, `NativeAdrenoUserClient`, `NativeAdrenoABI.h`.
- Generation-independent core: `NativeAdrenoVm`, `Memory`, `Submit`, `Ring`, `Timer`, `Firmware`, `Sha256`, contexts.
- Gen8: `A830Smmu`, `A830Gmu`, `A830GmuFirmware`, `A830Hfi`, `A830Votes`, `A830Cp`, `A830Zap`, `A830Irq`, `A830Fence`,
  `A830SelfTest`, `A830Dcvs`, `A830PowerState`, `A830Recovery`, `A830HwInit.h` (generated), `A830Regs.h`.
- Orchestration and diagnostics: `Adreno830Start`, `Adreno830Status`, `Adreno830Board.h`, `Adreno830Device.h`.
- SM8750 platform layer: `platform/sm8750/` (power, RPMh, SCM, SoC info, AOSS QMP, LLCC, TSENS).

## Build and tests

- Kernel archive: `scripts/build-adreno830.py` (the UniXNU component recipe, `build/adreno830/libAdreno830.a`); per file
  while developing: `python3 scripts/build-adreno830.py --tree <UniXNU tree> --out <dir> --only <source>`.
- Host tests: `python3 tests/run-host-tests.py [names]` (clang -Wall -Wextra -Werror with ASan/UBSan over the host doubles;
  `--firmware <dir>` adds the gen80000 pins).
- Board inputs and the board glue are checked in the device repository (`scripts/test-gpu-inputs.py`).

Firmware (`gen80000_gmu.bin`, `_sqe.fw`, `_aqe.fw`, `_zap.mbn`) is never committed: the device build embeds the local copies
after checking their pinned lengths and SHA-256.

License: MIT.

This copy is integrated into `UniXNU-Pagani-Drivers` from [Shad0w23333/UniXNU-OnePlus13T at 3c514bc](https://github.com/Shad0w23333/UniXNU-OnePlus13T/tree/3c514bc). Its original hardware records describe the author's builds. The IssacL04 candidate uses independently pinned PKX110 firmware and retains the existing Metal frontend and trace methods; its build and device results are recorded in the device repository.
