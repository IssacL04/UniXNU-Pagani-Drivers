# PaganiBattery

The OnePlus 13T (PKX110, SM8750) battery as iOS reads it: an `IOPMPowerSource` (class `PaganiBattery`) that powerd finds, so the
status bar, the lock screen's charging state and Settings > Battery show the charge and the charger. The property mapping and the
battery manager's request cycle are ported from UniXNU-sheng (`08cde20e`, `package/ShengBattery.cpp`, `ShengBattmgr.c`,
`BatteryProbe.cpp`, `GeniI2c.c`, BSD-2-Clause-Patent; each ported file names its source).

Status: on the device since 2026-10-31 with the gauge and the inferred charger (`[battery]` lines of the A830 runs), without the DSP.
The charger PMIC's state and the USB probe pass their host checks and build with zero warnings; neither has run on the device.

## Two sources

| Source | What it gives | Needs |
| --- | --- | --- |
| NFG8011B fuel gauge, I2C hub engine 1 (`i2c@984000`, 100 kHz), address 0x55 | charge, voltage, current (charge positive), temperature, remaining/full capacity, cycles, health, time to empty/full | nothing else: works without the DSP |
| ADSP battery manager, PMIC GLINK channel `PMIC_RTR_ADSP_APPS` (charger PD `msm/adsp/charger_pd`) | USB connected, charger status, adapter type, VBUS/IBUS, input limit; battery values when the gauge does not answer | PaganiAdsp running (`pagani-adsp=1`) |

The gauge is the one Android shows on this phone: in the 13T devicetree the OPLUS virtual gauge has two members, the ADSP "gauge"
(which implements only the sub-board NTC) and `bq27541-battery@55` with `oplus,batt_nfg8011b`, which implements every gauge function.
OPLUS reads it with the bq27541 standard commands (current from 0x0c). Without the DSP, being on a charger is inferred from the gauge:
a battery taking more than 50 mA is charging; a charged battery held at 100 % by the charger stays shown as connected. The gauge's average
counts for that only from 60 s after the start on: until then it holds what came before this boot. Four runs read +57 to +94 mA at 2 s;
one of them still +72 mA at 4 s with the battery giving 124 mA, which showed a charger that was not there.

What iOS is shown is guarded: nothing is published before a charge is known (0 % would read as an empty battery and start a
shutdown), a 0 % reading above 3450 mV shows 1 %, and the critical level needs the voltage below 3500 mV when it is known.

## The battery manager's property numbers

The OPLUS charger firmware numbers the battery properties like Qualcomm's older list (temperature 12, charge counter 14, cycles 15,
design 16, full 17, time to full 19, time to empty 20, OPLUS `BATT_CHG_EN` 24); sheng's Xiaomi firmware has one more property before
the temperature, so its numbers from there on are one higher. The first cycle reads properties 12 and 13 as probes (OPLUS: a
temperature and the technology enum; Qualcomm: a current and the temperature) and decides; nothing whose number depends on the layout
is read before. A write of a vendor property is refused unless the OPLUS layout was proven; the only writes ever possible are the USB
input limit (5), the charge current limit (10) and, at policy level 2, `BATT_CHG_EN` (24).

## Charging policy

| `pagani-battery-charge` | Writes |
| --- | --- |
| 0 (default) | none (the notification request only) |
| 1 | while USB is connected: input unsuspended when the firmware has it at 0; a computer's port gets its input limit (SDP `pagani-battery-sdp-ma`, default 900 mA; CDP 1500 mA); charge current limited to `pagani-battery-fcc-ma` (default 2000 mA), 500 mA outside 5..45 C (back inside 8..42 C) |
| 2 | 1, and `BATT_CHG_EN` = 1 when the firmware has it at 0, 0 outside 0..50 C (back inside 3..47 C); only with the OPLUS layout proven |

At most three tries per value and connection; every write and its result is logged.

## The charger PMIC's state (`pagani-battery` bits 2 and 3, default off)

Without the DSP the gauge's current is all there is, so a cable whose load is larger than its input reads as no cable, and a phone at 100 %
as no charger. The charger PMIC knows better, and the apps processor can read it: SPMI slave 7's peripherals `0x726`..`0x72c` have apps (EE 0)
channels, and observer reads need nothing else. `BatterySpmi.h` lists the registers and what is known of them: readings of Android's register
dumps (`sm8450_st_dump`) that fit the SMB5 register map. No document of this PMIC is public.

- Bit 2 (`pagani-battery=5`) reads nine registers every 2 s and only logs: nothing iOS is shown changes. A line at the first sample, at each
  change of VBUS (at most 24 a boot), of the charger's stage, input limit or Rp (24), and with VBUS one every 2 min (90), each with the gauge's
  reading, so a charge that stops half way shows in the log with no other switch.
- Bit 3 (`pagani-battery=13`) also uses them, when the DSP's view is not there: `ExternalConnected` is VBUS (`0x2910` bit 4 set, bit 2
  clear, two samples in a row), `IsCharging` VBUS and a stage that pushes current (`0x2606` bits 2:0 are 1 to 4, whatever the load takes),
  `FullyCharged` VBUS, not charging, 99 % or more. Three failed samples go back to the inference, which keeps running beside it. It stays a
  separate bit until a log of bit 2 confirms the readings.

## USB probe (`pagani-battery` bit 1, default off)

What the boot loaders left of the USB path, for the choice of how to take the controller over: `BatteryUsb.h` has the order and each gate. It
lives here because this driver has a thread, the observer channels and the log, so it needs no kernel patch. Nothing is written but the
arbiter's observer read commands; a read of a block without power takes this SoC down, so what can fault comes last, opened by what the
steps before it read, and `PBU_SETTLE_MS` after the line that announces it: when a read kills the phone, the UFS log still shows how far it got.
Those reads come with no UFS write on the wire: a bite after a hang gives no warning, and a reset on a write can hurt the logical unit around it
(`UfsLog.h`). So `pagani_panel_probe_quiet`, bit 1 (bit 0 is PaganiPanel's), holds the log's writer from `PBU_QUIET_MS` before them until their
lines are written, as PaganiPanel's probe does around its reads. A hang keeps it held: the timed reset's or the panic's last snapshot still goes out.

1. At the driver's start, the GCC alone (`gcc0`). Nothing in XNU touches USB, so that is the state at XNU's entry.
2. 120 s after the thread started, one step at a time between its other work: the PHY's and the repeater's LDOs, the repeater and the charger's
   peripherals, the SMMU's table for SID 0x40, the GCC: all of that reads what this port reads anyway.
3. The DWC3 only with the block reset released, the GDSC on and finished, both roots running and all five branches running (the two NOC
   branches may sleep in hardware clock gating). The GCC offsets are SM8750's, not SM8550's. An id that is not Synopsys's stops it.
4. The glue, 5 s later. Another 5 s later TCSR's reference clock enable, a block this port has never read, only with the PHY's three LDOs
   in regulation and its reset released; another 5 s later, with the clock on, the M31 PHY.
5. `[usbp] done route=...`: `inherit` (device mode, PHY up), `host` (stop xHCI first), `cold` (no power), `mixed` or `noid`.
6. Last, 5 s after the verdict, the context bank of a translating SMMU entry: of those registers NativeUfs reads only the first three.

`scripts/decode-usb-probe.py merged.log` reads the lines out: the state, the DWC version, the charger's stage, limits and watchdog, and where the
run stopped when the log ends early.

## Gauge bus bring-up (BatteryI2c.c)

1. GCC branch votes 0x52008 bits 7 (`gcc_qupv3_i2c_s_ahb_clk`), 8 (`gcc_qupv3_i2c_core_clk`), 11 (`gcc_qupv3_i2c_s1_clk`), bits only
   ever set, each branch seen running before anything else. The register is shared: every update takes the weak lock word
   `sm8750_gcc_vote_owner`, here and in PaganiWifi (PCIe) with the protocol of Sm8750Platform's `sm8750_gcc_vote_set`, which the touch
   (bits 18-21 and 26) and the speaker amplifiers (bits 7, 8, 14) use. That helper writes anyway when a holder overstays 50 ms, so every
   transfer still reads the register first: bits found dropped are voted again and the branches seen running before the engine is touched (a transfer
   that cannot get them back fails with -16, the engine untouched; logged up to four times).
2. The engine's RCG (0x17024) must run from the XO; it is never reprogrammed.
3. The engine: I2C protocol, FIFO interface, no command left running (one is cancelled or aborted); FIFO mode, divider 7, SCL counters
   10/12/26 (100 kHz from 19.2 MHz), no interrupt line (polled).
4. gpio66/gpio67 on function 1, 2 mA, no pull (Android's `qupv3_hub_i2c1_*_active_oplus`).

## Boot arguments

| Argument | Meaning |
| --- | --- |
| `pagani-battery=N` | bit 0 on; absent, 0, an even value or one above 15: off (nothing runs, maps or allocates). `pagani-adsp=1` does not imply it: a bring-up step names what it turns on. The other bits are off by default and cost no byte of the command line: 2 the USB probe, 4 the charger PMIC's state logged, 8 also used for what iOS is shown (implies 4). `1` is the driver as it was; `7` probe and log; `15` all |
| `pagani-battery-gauge=0` | leave the I2C gauge alone |
| `pagani-battery-gauge-id=0` | skip the device type query (the one write to the gauge) |
| `pagani-battery-adsp=0` | no battery manager channel even with `pagani-adsp=1` |
| `pagani-battery-charge=0/1/2` | charging policy (above) |
| `pagani-battery-fcc-ma`, `pagani-battery-sdp-ma` | policy limits |
| `pagani-battery-design-mah` | design capacity when the DSP does not give it (default 6260) |
| `pagani-battery-gauge-ms` | gauge period (default 10 s without the DSP, 20 s with it; a silent gauge backs off to 5 min) |
| `pagani-battery-log=0/1/2` | boot log: start and failures (the first four gauge losses) / rationed (default, about 15 lines a day, under 40 with a gauge that keeps coming and going) / every pass (bring-up only) |
| `pagani-battery-pmic=0/1/2` | the thread's first step, who may drive the charger PMIC (SPMI slave 7, `BatterySpmi.h`): 0 nothing; 1 (default) the SPMI arbiter's channel table (core `0xc400000` + 0, 4, `0x2000` + 4 n) and ownership table (cnfg `0xc42d000` + 4 n), read only, the same reads PaganiInput's buttons make on every boot; 2 also each slave 7 peripheral's TYPE/SUBTYPE through the apps (EE 0) observer channel (`0xc440000` + `0x20` APID), which writes the observer's read commands: bring-up only, keep 1 (or 0) for unattended runs |

## Build and tests

```sh
python3 Test/check-pagani-battery-host.py [--tree TREE]
python3 Test/mutate-pagani-battery.py [--only NAME]
python3 scripts/build-pagani-battery.py --tree TREE [--append-to TREE/build/adreno830/libAdreno830.a] [--without-adsp]
python3 scripts/decode-usb-probe.py merged.log
```

The kernel archive uses the tree's XNU compile records (zero warnings, `-mgeneral-regs-only`). `PaganiBatteryService.cpp`'s class
initializer is taken out of `__mod_init_func` and registered late by `pagani_battery_start`. `PaganiAdsp.h` comes from `../PaganiAdsp`
(or `--adsp`); the archive carries weak defaults of PaganiAdsp's entry points. Stage7Probe calls `pagani_battery_start()` after the GPU
and ADSP starts (weak default there).

Host checks: the battery manager codec against scripted OPLUS and Qualcomm firmware; the gauge reader against an NFG8011B register
model; the IOPMPowerSource properties (every key against 21H461 powerd's strings when the system volume is at hand); the bus bring-up and transfers against a model of the GCC votes, RCG, pins and a GENI I2C engine with the gauge behind it (no
engine access with its clocks off, also after another driver's unlocked update dropped the votes); the USB probe against a model in which
a read of a block without power fails the test (every condition of the gates alone, the SM8750 offsets, the order, the waits, the SMMU
table, the longest lines); the charger's registers against Android's samples; the whole core with a simulated clock, gauge and firmware
(sources, guards, inference, the average at boot, the charger's state not read, logged and used, timeouts, the policy at each level, the
log's size over a day, also with a gauge that keeps coming and going); every boot argument the driver reads is in the table above and in
`PaganiBattery.h`; the decoder on the probe's lines. `Test/mutate-pagani-battery.py` breaks each of those rules in a copy and holds that
a test fails. Host checks do not establish that the gauge answers, that the phone charges or what the charger's registers mean.

## Boot log (ramoops U rows)

- `[battery] start gauge=1 adsp=1 charge=0 design=6260 log=1 pmic=1 mode=1`
- `[battery] spmi sid7 v=0x7... ch=... n=... ee0=... irq0=...`, then `[battery] spmi sid7 + pid:owner ...` (16 a line; `pid:owner:type/subtype`
  with `pagani-battery-pmic=2`)
- `[battery] i2c up st=5 err=0 vote=... after=... voted=1 cbcr=... rcg=... fw=... clk=... scl=... sda=... sclpin=...`
- `[battery] gauge addr=0x55 rc=0 type=0x4231 NFG8011B`
- `[battery] gauge pass=1 mv=... ma=... avg=... t=29.50 soc=87 rm=... fcc=... cyc=... soh=... flags=... valid=0xfff ...`
- `[battery] dsp channel PMIC_RTR_ADSP_APPS registered (5 x 1024)`, `[battery] dsp channel up`
- `[battery] dsp cyc=2 layout=oplus st=2 cap=8750 uv=... ua=... t=2950 lim=... en=1 usb=0 type=0 adap=0 real=0 icl=... ...`
- `[battery] state published pct=87 ext=0 chg=0 full=0 crit=0 mv=3950 ma=-800 t=29.50 rm=3200 fcc=6100 cyc=37 src=gauge+charger`
- `[battery] state changed ...` on every connect, disconnect, charging and full change; `[battery] dsp set ...` for every write.
- A gauge that comes and goes: the first failing pass of the first eight failing runs, `[battery] gauge stopped answering ... lost=N`
  and `[battery] gauge answers again` for the first four losses; after that only the counts, ` fail=` (failed reads) and ` lost=` on
  the state lines, with a `[battery] state summary ...` at most every 2 h while they grow.
- `[battery] i2c clock votes dropped by another driver, voted again n=... vote=... rc=0` (or `not back`, `rc=` the cause): another
  driver's unlocked update of 0x52008 took this driver's votes away (at most four lines).
- `[battery] pmic first|change|trend vbus=1 stage=3 rt=0x11 in=0x2 det=0xf rp=0x2 tc=0x2b st=0x83 icl=2000 imax=2000 inow=2000 pct=87 rm=3200 mv=3950
  ma=812 avg=800 t=29.50`, `[battery] pmic read failed rc=... n=...`: the charger PMIC's state (bit 2); `src=gauge+pmic` on the state lines (bit 3).
- `[usbp] ...`: the USB probe (bit 1), one line of the GCC at the start, about 20 lines at most after 120 s, under 4 KiB of the events ring and
  each within one U record: `gcc0 rail p7fd p726 p727 p728 p729 p72b p72c smmu gcc gate dwc dev [xhci] hw qs ref phy done [smmu bank]`. A closed
  gate ends the list early and says why (`closed=...`, `phy=...`, `why=...`).

## Limits

No suspend handling (the port does not sleep), no wireless charger, no OPLUS fast charging (VOOC/UFCS need the OPLUS framework).
Without the DSP nothing pets the PMIC charger's watchdog or runs its detection, so the phone may stop charging (see the device notes);
the gauge current in the log shows whether it charges, and the `[battery] pmic trend` lines of bit 2 show it over a long charge. The USB probe
reads and reports only; there is no USB driver.
