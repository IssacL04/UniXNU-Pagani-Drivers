# PaganiHaptics

The OnePlus 13T (PKX110, SM8750) vibrator for iOS: the haptics module of the PMIH010x PMIC (Qualcomm HAP530_HV, SPMI slave 7:
HAPTICS_CFG `0xf000`, HAPTICS_PATTERN `0xf100`, HAPTICS_HBOOST `0xf200`; X-axis LRA "0809", 170 Hz) driven from XNU over the SPMI PMIC
arbiter v7, with a kernel API for the audio side (constant vibration, the board's short effects, PCM-like streaming into the hardware
FIFO, stop).

Status: host checks pass; the kernel archive builds with the tree's XNU compile records, zero warnings; on the device (four boots of
2026-10-31/11-01, `pagani-haptics=1 pagani-haptics-ios=2`) discovery, identity and the init ran (`ready`, one boot `FAULT write`), the
actuator has never been driven: no `[haptics] stream/constant` line, because iOS never reached the actuator (PaganiAudio's README, Limits).

## Sources (facts only, no code copied)

| Tag | Source | Used for |
| --- | --- | --- |
| [Q] | OnePlus sm8750 kernel `drivers/input/misc/qcom-hv-haptics.c`, branch `oneplus/sm8750_b_16.0.0_oneplus_13t` (blob `16a79e99`) | register map, HAP530 (`hw_type` 5, "PMIH010X") work-arounds, init/play/FIFO/stop sequences, OPLUS values |
| [U] | Fenglin Wu, "[PATCH v4 3/3] input: misc: Add Qualcomm SPMI PMIC haptics driver" (LKML, 2026-07-17, HAP530/PMIH0108) | the minimal DIRECT_PLAY/FIFO sequences, all 8 KiB as FIFO |
| [E] | OnePlus 13s UEFI `PmicDxeLa.patched.efi` (BOOT.MXF.2.5.1-00265-PAKALA), the one UEFI-Pagani_SM8750 runs before XNU | TYPE 0x15, SUBTYPE 0x0a, revision < 5.2 of HAPTICS_CFG |
| [A] | Linux `drivers/spmi/spmi-pmic-arb.c` | arbiter v7 channel/owner tables, RW and observer channels, command and status |
| DT | `build-userspace/a830-inputs/dt/running.dts`, `qcom,hv-haptics@f000` | every board value (`PaganiHapticsBoard.h`) |

Registers and their sources are in `HapticsRegs.h`; the board values (LRA period 5882 us, DIRECT_PLAY 1260 mV, effects 3600 mV, FIFO
full scale 7450 mV, brake `ff 3f 1f`, sine drive and brake, the 15 effects) in `PaganiHapticsBoard.h`.

## Safety

- **Writes are whitelisted in `HapticsSpmi.c`, not by the caller**: only HAPTICS_CFG and HAPTICS_PATTERN of slave 7 (HBOOST and the
  SDAM `0x9d00` of slave 0 are read only), only the registers of the sequences below with their lengths, and for some only certain
  values (SPMI_PLAY: FIFO or DIRECT_PLAY sources only; VMAX at most the hard cap; MEM_OP never the pattern-memory bit; the memory map
  only "all FIFO"). The kernel maps only two RW channel pages, those of the two APIDs, after the checks.
- **Before any write**: the arbiter is v7; CFG and PTN map through APIDs owned by EE 0 (cnfg bits 2:0); HAPTICS_CFG is TYPE 0x15
  SUBTYPE 0x0a revision 5.0/5.1 [E][Q]; PATTERN revision 5 [Q]; PATTERN and HBOOST present (and equal to `PAGANI_HAPTICS_PTN_ID` /
  `PAGANI_HAPTICS_HBST_ID` once pinned). Anything else: nothing written. Before **every** write the channel table entry and the owner
  are read again.
- **Drive voltage**: hard cap 7450 mV (the board's FIFO full scale, the most OPLUS ever drives this actuator with), checked again by
  the bus layer; runtime cap `pagani-haptics-vmax-mv` (default 3600); the boost's 5 V clamp; the board's low-battery rule (6800 mV
  below 5 %, below 20 % under -5 C, from PaganiBattery's status when it runs); VMAX + headroom never above 11 V.
- **On-time**: one constant play or effect at most `pagani-haptics-play-ms` (2 s), one stream at most 30 s, and a bucket of 5 s of
  drive refilled at half speed (sustained 50 % duty); a stream nobody feeds stops 100 ms after the FIFO runs dry.
- **Errors**: any bus failure, timeout or refusal, or a short circuit, stops the play (one emergency SPMI_PLAY = 0 even after the bus
  closed) and keeps the driver off until reboot. An HPWR that does not settle (toggled as [Q] does) or a boost enabled in open loop by
  someone else refuses only that play. With bit 2 of `pagani-audio-srzl` a transaction the arbiter only dropped goes again (bounded) first.
- **The boot never waits**: `pagani_haptics_start` returns at once; everything runs on the driver's thread; every wait is bounded
  (1000 x 1 us per SPMI transaction, 10 x 1 ms for HPWR, the brake length plus a period).
- No PMIC interrupt is enabled: the FIFO is polled (every 4 ms while streaming) and its empty threshold stays 0.

## Sequences (HAP530; `HapticsCore.c`)

- **Init** [Q][U]: a play left running stopped; faults cleared (`0x66 = 0x17`); VMAX headroom 1.5 V (`0x67 = 30`); sine drive and two's
  complement FIFO data (`0x49`); open-loop brake, sine (`0x50`); RC clock calibration auto (`0x72`); TLRA_OL 5882 us (`0x5c/0x5d =
  0x498`); DRV_DUTY `0x56` and AUTORES `0x32` (the OPLUS probe's values); no waveform hold; SWR not ignored; FIFO threshold 0; FIFO rate
  T_LRA; all 8 KiB as FIFO (`0xf1a0 = 0xff`); brake samples (`0xf170..77`); VMAX with the HAP530 sequence (zero-cross timeout fault
  powered down and closed-loop VDRIVE off around it, faults cleared, HPWR settles, 500 us discharge). HAPTICS_EN is left as found and
  set by the first play (after 500 us).
- **DIRECT_PLAY**: boost free; SWR ignored; VMAX; amplitude (`0xf126`); auto resonance on (closed loop; `pagani-haptics-autores=0`: open
  loop at TLRA_OL); faults cleared; enable; `SPMI_PLAY = 0x89` last. After a closed-loop play of 120 ms or more the measured period
  (status selections CAL_TLRA_CL / LAST_GOOD) is accepted within 160..180 Hz and becomes TLRA_OL.
- **Effects** (`effect_0..5`, `primitive_0..8`): N samples of 0x7f at the LRA period, 3600 mV x strength, no brake (the device tree's
  `wf-brake-disable`).
- **Streams**: 8 ms of samples first (64 at 8 kHz; they count as latency), rate 8/16/24/32/44.1/48 kHz (`0xf124`), auto resonance off, RC
  clock calibration off (as OPLUS streams), FIFO flushed (`0xf12d` 1 then 0), up to 320 samples, `SPMI_PLAY = 0x80`; then each poll reads the fill level (selection `0x80/0x05`) and writes what
  fits in 4-byte writes (`0xf120..23`; the last 1..3 samples through `0xf12c`), at most 1024 samples a poll.
- **Stop** [Q]: `SPMI_PLAY` without PLAY_EN (with BRAKE_EN after DIRECT_PLAY), HPWR polled every half period until disabled, 500 us;
  after FIFO plays the rate back to T_LRA and RC calibration auto; headroom back to 1.5 V; SWR no longer ignored.

## Boot arguments

| Argument | Meaning |
| --- | --- |
| `pagani-haptics=1` | on; `2`: dry run (discovery, identity, owners and a register dump in the log through observer reads, plus one read of the HAPTICS_CFG RW channel's STATUS to show the chnls page is mapped for the HLOS VM; nothing written); absent or anything else: off (nothing runs, maps or allocates) |
| `pagani-haptics-vmax-mv` | runtime drive cap in mV (default 3600; at most 7450) |
| `pagani-haptics-play-ms` | one constant play or effect at most this long (default 2000, at most 10000) |
| `pagani-haptics-autores=0` | DIRECT_PLAY in open loop (default 1: closed loop) |
| `pagani-haptics-log=0/1/2` | failures only / rationed (default) / every play |
| `pagani-haptics-test` | bring-up: one constant vibration of this many ms (at most 500) after the init (default 0) |
| `pagani-audio-srzl` | bit 2 (value 4; default off): an SPMI transaction the arbiter dropped (status `DONE \| DROPPED`: nothing carried out) goes again, at most 3 times, 100 us apart, the channel checked before each, and the `ready` line says `dropped=N`. Without it such a transaction ends the driver until reboot: the 13T's boot of four, `FAULT write rc=-5 periph=0 reg=0x48 status=0x9`. The argument's other bits are PaganiAudio's |
| `pagani-haptics-ios` | the iOS haptics switch (`HapticsIos.h`, `PaganiHapticsIos.cpp`): `1` creates the device tree node `/product/haptics` with `supports-3rd-party-haptics` = 1 (stage A: MobileGestalt's DeviceSupportsHaptics, vibrations through the legacy vibe engine), `2` also `closed-loop` = 1 (stage B: CoreHaptics, UIKit feedback, keyboard clicks). Planned in `pagani_haptics_start`, made just before launchd by `pagani_haptics_ios_commit` (`phone-haptics-overlays.patch`): only with `pagani-haptics=1`, `pagani-audio=1`, the service offering the actuator and the plug-in on the root (the node makes iOS wait for the `Actuator` device); `closed-loop` only with `hapticengineconfig.plist` visible (without the J120 tunings overlay stage 2 falls back to stage A). Otherwise refused in the log. Absent or 0: no node |

## API (`PaganiHaptics.h`)

`pagani_haptics_constant(permille, ms, mv)`, `pagani_haptics_effect(id, permille)`, `pagani_haptics_stream_begin(rate_hz, mv)`,
`pagani_haptics_stream_write(samples, n)` (signed 8-bit, returns how many were taken; 16384-sample ring),
`pagani_haptics_stream_end(drain)`, `pagani_haptics_stop()`, `pagani_haptics_info(&info)` (stage, state, measured f0, period, caps, FIFO,
ring space). Calls queue a command and return at once (-19 when the driver is not running). A client that must link without the driver
defines `PAGANI_HAPTICS_WEAK_DEFAULTS` in one file.

## Boot log (ramoops U rows)

- `[haptics] start mode=1 cap_mv=3600 hard_mv=7450 play_ms=2000 autores=1 log=1 test_ms=0`
- `[haptics] spmi rc=0 v=0x7... ch=N ee=0 cfg=0x7f0:APID:eeO:irqI:xM ptn=... hbst=... sdam=...`: the channel, its write owner, its IRQ
  owner bit and how many APIDs map the PPID. **This line answers whether EE 0 owns SID 7 0xf0/0xf1/0xf2.**
- `[haptics] id cfg=TT/SS:rMAJ.MIN.AMAJ.AMIN ptn=... hbst=... sdam=... verdict=ok|<problem>`
- `[haptics] reg cfg 08=.. 09=.. ...`, `reg ptn ...`, `reg hbst ...`, `reg sdam ...` (the dump, in both modes)
- `[haptics] chan cfg apid=.. map=0 status=0x.. (read only)` (dry run, when it would not refuse): the RW channel page is reachable
- `[haptics] ready fifo=8192 hw_max_mv=10000 cap_mv=3600 hard_mv=7450 vmax=1250 en=0 writes=16`, or `[haptics] refused: <problem>`
- `[haptics] constant/effect/stream a=.. b=.. mv=.. n=.. budget_ms=..`, `[haptics] stop <why> ...`,
  `[haptics] closed-loop period us=.. cal=.. cal_en=.. f0_dhz=..`, `[haptics] FAULT <what> rc=.. periph=.. reg=.. status=..`

## Build and tests

```sh
python3 Test/check-pagani-haptics-host.py [--tree TREE]
python3 Test/mutate-pagani-haptics.py          # 37 mutants of the safety rules, each must fail a test
python3 scripts/build-pagani-haptics.py --tree TREE [--out DIR] [--append-to TREE/build/adreno830/libAdreno830.a]
```

Host checks: `Test/spmi.c` (9047 checks) runs the SPMI layer against a model of the arbiter v7; `Test/core.c` (91832 checks) runs the
core against the arbiter plus a HAP530 model (register files, FIFO with memory map, flush and fill status, playback on a simulated clock,
HPWR around plays and brakes, closed-loop status, fault injection): dry run writes nothing; each identity/revision/owner/arbiter problem
refuses before any write; the init's exact write sequence; DIRECT_PLAY start and stop order; effects; streams (order, never an
overflow, 4-byte writes until the 1-byte tail, drain, abort, underrun, samples from before a stream dropped); the caps, the clamp and low-battery rules,
the length cap and the budget; faults (failure, timeout, short circuit, auto-resonance fault, owner change, HPWR timeout, boost in use).
Host checks do not establish that the actuator moves.

## Integration (not done; drafts in `integration/`)

1. Copy this directory to `UniXNU-Pagani-Drivers/drivers/PaganiHaptics`; move the board values to the device repository's
   `package/PaganiPeripherals.h` (`integration/PaganiPeripherals.h.snippet`).
2. `integration/Stage7Probe.diff`: weak `pagani_haptics_start` and its call after `pagani_audio_start`.
3. `integration/components.toml.snippet` (UniXNU) and `integration/board.toml.diff` (device repository): the component, appended to
   `libAdreno830.a` like PaganiBattery.
4. On the device, one boot each, the user present: `pagani-haptics=2` (read the `[haptics] spmi/id/reg` lines; pin
   `PAGANI_HAPTICS_PTN_ID`/`HBST_ID`), then `pagani-haptics=1 pagani-haptics-test=150`, then the audio side's calls.

## Limits

No PMIC interrupt, no pattern memory (PAT_MEM) effects, no SWR haptics (the board marks it unsupported), no VI-sense, no auto-brake
calibration, no OPLUS factory calibration restore (the closed-loop measurement replaces it), no suspend handling (the port does not
sleep). The PATTERN and HBOOST TYPE/SUBTYPE are not known yet (logged by the dry run).
