# Agent instructions

## Responsibility

Maintain the Adreno830 kernel GPU driver: the A830 (Gen8) of SM8750 under UniXNU (iPadOS 17.7.11 / 21H461), first board the
OnePlus 13T (PKX110). The driver is written from source in this repository; never patch the author's closed build.

## Required reading

1. `README.md`, then `docs/ARCHITECTURE.md` (including section 10) and `docs/IMPLEMENTATION_PLAN.md` (including section 7).
   They are authoritative; where they are silent follow Linux/KGSL; measured hardware facts win.
2. `docs/PROGRESS.md` for the current state and the hardware checklist.
3. The module's header and host test before editing it; `A830Regs.h` for every register fact.
4. The UniXNU component catalog (`components.toml`, entry `Adreno830`) and the device repository
   (`UniXNU-Pagani_SM8750-gpu`, branch `a830-driver`) for board inputs and image assembly.

## Rules

- Board inputs reach the driver only through `Adreno830Board` (`Adreno830Board.h`); the driver reads no ADT and no device
  repository header. Boot arguments are parsed by the board with `Adreno830ParseKnobs`.
- Read-back values the firmware or the hypervisor controls (sGFSR, CB FSRs, CP pointers at start, `CP_SQE_CNTL`, GMU AO
  bits 3/5, GMU2HOST bit 23) are recorded, never gates (ARCHITECTURE 10). Every stage failure is a step plus a reason code
  (ARCHITECTURE 8.3), without retry except where the design retries.
- No logging, formatting or cache maintenance on hot paths; diagnostics go to `adreno830_status`. The logs are the
  bring-up register trace (`A830Trace.h`, `pagani-gpu-trace`): lines only inside its scopes (bring-up, power operations,
  recovery), never from interrupt context; a new table bound for a module must go through the orchestrator's traced paths;
  and the run-time meter line (`adreno830_status_meter`, ARCHITECTURE 8.8): the DCVS thread, only on the diagnostic overlay
  (`pagani-gpu-status` 2 or 3), at most one line a minute and only a changed one; it goes to the 32 KiB events ring, which the
  boot's one-time lines must survive in. The one exception is the per-second line `gpu sec` of the same function (ARCHITECTURE 8.8,
  2026-10-31): only under `pagani-diag` as well (`adreno830_status_diag_lines`, the images with the UFS log, `sheng-log=1`, which keeps
  what the ring turns over) and only a second with GPU work; a one-minute window cannot tell a spike from its minute. No other new per-second log line: numbers go into the existing rows
  or those two.
- Keep `NativeAdrenoABI.h` byte-identical (AdrenoMetal and the Turnip darwin backend include it).
- Firmware stays out of Git; GPL sources (Linux, KGSL) are references only.

## Build and verification

- Host tests: `python3 tests/run-host-tests.py [names]`; new tests are `tests/test_<name>.cpp` or
  `tests/test_sm8750_<name>.c` with a `host-test:` line listing their sources.
- Kernel objects: `python3 scripts/build-adreno830.py --tree <UniXNU build tree> [--only <source>]` with zero warnings;
  the full archive is built by `./unixnu kernel` through the component recipe.
- Report commands, results and source revisions; report hardware observations separately, with the device and OS profile.
  Temporary boots (`fastboot boot`) only with the user's approval; never flash partitions.

## Documentation

Keep `README.md`, the design documents and `docs/PROGRESS.md` current with the interfaces and behavior; record hardware
results in the device repository's `porting.toml`.
