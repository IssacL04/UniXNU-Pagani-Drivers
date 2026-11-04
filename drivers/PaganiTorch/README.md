# PaganiTorch

The OnePlus 13T (PKX110, SM8750) LED torch for iOS: the flash LED module of the PM8550 PMIC (SPMI slave 1, base `0xee00`, the 4-channel
module; the phone's LED is on channels 0 and 1) driven from XNU over the SPMI PMIC arbiter v7, with a kernel API, a sysctl and a path from
the iOS flashlight button that needs no camera.

Status: host checks pass; the kernel archive and the plug-in build with zero warnings; **not yet run on the device**, not wired into the
tree (diffs in `build-userspace/a830-inputs/torch/integrate`). Default off: without the boot arguments nothing runs, maps, allocates or
registers.

## Sources (facts only, no code copied)

| Tag | Source | Used for |
| --- | --- | --- |
| [L] | Linux `drivers/leds/flash/leds-qcom-flash.c` (GPL-2.0-only) | TYPE/SUBTYPE, the 4-channel register fields, the torch current formula and limits, the torch sequence, the fault bits |
| [A] | Linux `drivers/spmi/spmi-pmic-arb.c` | arbiter v7 tables, RW and observer channels, command and status (the same facts as PaganiHaptics) |
| DT | `build-userspace/a830-inputs/dt/running.dts` | `qcom,flash_led@ee00` (11361), `qcom,camera-flash1..3` (34747): `torch-source = <torch_0 torch_1>`, `qcom,max-current-ma = 500`, `qcom,thermal-derate-current = <200 500>`, the arbiter (11197) |
| iOS | 21H461: `AVFCapture`, `CMCapture`, `SpringBoardUIServices`, `ControlCenterServices`, libMobileGestalt, static reading | the flashlight chain |

Registers and their sources are in `TorchRegs.h`, the board values in `PaganiTorchBoard.h`.

## Safety

- **Writes are whitelisted in `TorchSpmi.c`, not by the caller** (PaganiHaptics' `HapticsSpmi.c`, copied at sha256 `45e83a18...`; the arbiter
  paths are byte for byte the same, only the whitelist, the interlock and the emergency stop differ): only the flash module of slave 1, only
  the torch sequence's registers of the board's channels, single bytes, and only these values: ITARGET at most the code of 200 mA, the
  safety timer 0, the software strobe, EN_CTL 0x00 or 0x80, IRES with 5 mA steps on the board's channels, CHAN_EN within them.
- **An interlock**: a channel can be switched on only after this gate itself wrote its resolution, target, strobe and timer since the
  channel was opened, so a current left behind by another system can never be switched on. Android's flash settings (12.5 mA steps, 1.6 A)
  are overwritten first; every value is read back before the channels go on.
- **Before any write**: the arbiter is v7, the PPID `0x1ee` maps through an APID owned by EE 0, TYPE 0x18, SUBTYPE 0x07, and EN_CTL has no
  bit but the enable. Anything else: nothing written. Before **every** write the channel table entry and the owner are read again.
- **Current**: per channel, default 50 mA at level 1000, `pagani-torch-max-ma` at most 200 (the DT allows 500; the cap is in the core and
  the gate refuses the code of anything above it). A level never rounds up.
- **On-time**: an "on" ends by itself after 5 minutes. Changing the level does not extend it; off and on again does.
- **Faults**: every bit of the three status registers on the board's channels, polled every 100 ms while on and at once after switching
  on: short circuit, over-temperature (OTST1, OTST2, BOB thermal), over-current (BOB limits, BCL), low VPH, safety timer. Any fault, bus
  failure, timeout, refusal or failed read-back switches the LED off (one emergency CHAN_EN = 0 and EN_CTL = 0, even after the bus closed)
  and keeps the driver off until reboot.
- **The boot never waits**: `pagani_torch_start` returns at once; all register access is on the driver's thread; every wait is bounded.
- No PMIC interrupt is enabled. The thermistor `pm8550_cam_flash_therm` is not read: at 200 mA a channel is at 40 % of its torch rating, and
  the module's own over-temperature bits end the torch.
- A panic while the LED is on leaves it on until the reset (`pagani-panic-reset`); a reset clears it.

## Sequences (`TorchCore.c`)

- **Init**: all channels off, the module off when it was on.
- **On** [L]: board channels' CHAN_EN off, module off, IRES, ITARGET, timer 0, module on, software strobe, read back, CHAN_EN on, status.
- **Off**: CHAN_EN off, module off, ITARGET to the lowest code, CHAN_EN and the module read back.
- **Level change**: the sequence again; about 20 SPMI transactions, the LED blinks off for under a millisecond.

## Boot arguments

| Argument | Meaning |
| --- | --- |
| `pagani-torch=1` | on; `2`: dry run (discovery, identity, owner and a register dump in the log through observer reads, plus one read of the module's RW channel STATUS; nothing written); absent or anything else: off |
| `pagani-torch-max-ma` | the current per channel at level 1000, default 50, at most 200, at least 5 |
| `pagani-torch-test` | bring-up: on at level 1000 for this many ms (at most 1000) after the initialization, then off (mode 1) |
| `pagani-torch-log=0/1/2` | failures only / rationed (default: the first 8 changes, then one per 5 minutes, at most 24 a boot) / every change (at most 64) |
| `pagani-torch-ios=1` | the iOS flashlight, below: only with `pagani-torch=1` |

Byte counts on the command line, with the leading space: `pagani-torch=2` 15, `pagani-torch=1 pagani-torch-test=300` 37,
`pagani-torch=1 pagani-torch-ios=1` 34.

## Interfaces

- `pagani_torch_set(level)` (`PaganiTorch.h`): 0 off, 1..1000 permille of the cap; any thread that may sleep; waits at most 200 ms for the
  driver's thread and returns what happened.
- `kern.pagani_torch` (only with `pagani-torch=1`, registered by hand, so a kernel without the argument has no such name): read the level,
  write it as root.
- `pagani_torch_status[16]`: `PaganiTorch.h`.

## The flashlight of iOS

Control Center's FlashlightModule (`UIRequiredDeviceCapabilities`: `camera-flash`), `SBUIFlashlightController` and the lock screen all end in
`+[AVFlashlight hasFlashlight]`, which is `MGGetBoolAnswer("camera-flash")`: the property `flash` of `IODeviceTree:/product/camera` exists.
`AVFlashlight` talks to `FigFlashlight` in mediaserverd (`com.apple.coremedia.flashlight`; `FigFlashlightServerStart`, not conditional on a
camera). `FigFlashlight` asks `BWFigCaptureDeviceVendor` for a device and a stream and sets the stream's `TorchLevel`; on this phone the vendor
has no capture device, because `FigCaptureGetCaptureDeviceCreateFunction` finds none of `AppleH16/13/10/9/6CamIn`.

That lookup is also the way in, without a camera and without changing an Apple file or code page:

1. The kernel publishes an IOService of class `AppleH6CamIn` with a user client, and `/product/camera` `flash`, just before launchd, only when
   the plug-in is visible and the driver runs. H6 is last in the table and no class of the pinned kernelcache (it has H9, H10, H13); besides
   CMCapture only libMobileGestalt and libZhuGeArmory name it, to read camera module serial numbers from such a service, and those
   properties are absent.
2. `/System/Library/MediaCapture/H6ISP.mediacapture`: the directory is empty on the root and only `H9ISP` is in the shared cache, so the owner
   overlay can provide the file (`phone-torch-overlay.patch`, `scripts/add-pagani-torch-data.py`). `ios/TorchPlugin.m` runs in mediaserverd
   when the vendor is first used.
3. It exports no `H6ISPCaptureDeviceCreate`, so the vendor stays without a capture device: `dlsym` fails as it fails now and no camera appears
   anywhere. It replaces six vendor methods in the process's memory; each passes everything through, except for a client registered under
   the name `FigFlashlight`, which gets a token device and a stream whose `TorchLevel` goes to the user client.

The user client takes one call, opens only for mediaserverd, and switches the LED off when the process goes away.

Known limit: the 5 minute limit is not reported back, so after an auto-off the button still shows on until it is tapped. The button's levels
are fractions of the cap (`pagani-torch-max-ma`).

## Building and testing

- `scripts/build-pagani-torch.py --tree TREE --out DIR`: the kernel archive (component recipe).
- `scripts/build-pagani-torch-plugin.py --out DIR`, `scripts/add-pagani-torch-data.py`: the plug-in and the seed.
- `Test/check-pagani-torch-host.py [--tree TREE]`, `Test/mutate-pagani-torch.py`: host checks, mutants. Host checks do not establish that the
  LED lights on the device.
