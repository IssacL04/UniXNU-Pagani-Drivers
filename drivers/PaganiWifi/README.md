# PaganiWifi

The OnePlus 13T (PKX110, SM8750) Wi-Fi: the Qualcomm peach chip (PCIe `17cb:110e`, peach_v2, WCN7880 family) on PCIe0, brought up
by the board code, booted, run by the Ath12kWifi stack (a copy with a peach entry, `Ath12kWifi/`) and offered to iPadOS 17.7.11
(21H461) as an Apple80211 interface `en0`. A port of UniXNU-sheng's working Wi-Fi ([UniXNU-sheng](https://github.com/UniXNU/UniXNU-sheng)
`08cde20e`: `package/WifiProbe.cpp`, `WifiPcieTables.h`, `MhiBoot.c`, `WifiNet.cpp`; Ath12kWifi `f6bf561`; BSD-2-Clause-Patent,
each ported file names its source) to the 13T's facts (`PaganiWifiTables.h`, from `build-userspace/a830-inputs/wifi/facts.md`).

Off by default. Without `pagani-wifi=N` (N = 1..6) `pagani_wifi_start` (Stage7Probe) returns at once: nothing is mapped, allocated or
started. Nothing panics: every failure is a step number and an error in `pagani_wifi_status` and a `[wifi]` line in the BootProgress
ramoops log (U records, `pagani-ramoops=1`); the thread then parks. Nothing is ever written to the phone's storage. What may hang runs with
the UFS log's writer held (below).

**Hardware status: nothing here has run on the phone yet.** Host model tests and kernel-flag builds pass (below); every stage needs
a device round.

## Stages (`pagani-wifi=N`, each includes the ones before)

| N | Stage | What it does | 13T facts it relies on |
| --- | --- | --- | --- |
| 1 | survey (reads only) | GCC votes, GDSCs, the 12 branch halts, the 8 WLAN pins, the 7 rails and the 4 LDOs the PCIe PHY may hang on through the SPMI observer (state, set point, mode), the PMK8550 gpio3 sleep clock pin; PARF/DBI only when the GDSC is on and the controller's clocks run (a read of an unclocked block stalls the bus) | windows of `PaganiWifiTables.h` |
| 2 | power | sleep clock pin (owner-checked, sheng's 5 writes) → the 7 RPMh rails in cnss2's order, each mode 7, voltage, enable (S7I and the rules: see below) → PCIe NoC BCM `SN4` (cnss "high") → AOSS PDC messages (`pagani-wifi-pdc`) → PERST# low, CLKREQ# func 1, WLAN_EN low 150 ms then high, 60 ms → GDSC collapse votes `0x5214c` bits 0/2 cleared → aux/rchng RCGs, pipe mux on TCXO, branch votes `0x52008`/`0x52000` under the lock word `sm8750_gcc_vote_owner`, TCSR `0xf204008` clkref → BCR pulses | GCC/TLMM/TCSR offsets, rails, pins |
| 3 | link | PARF (RC, DBI 0x40000000, iATU 0x40001000, slave space) → the running devicetree's 128-write `qcom,phy-sequence` (QMP v7), pipe mux to the PHY before the reset release/start, PHYSTATUS → BDF-to-SID (endpoint 01:00.0 → hash 0x15 → stream 0x1401) → root port bus numbers, command, Gen cap (`pagani-wifi-gen`, default 2) → PERST# high after ≥ 100 ms → LTSSM → L0 within 1 s → iATU region 0 as the config window → up to 1 s for the endpoint to answer (the vendor driver's wait) → endpoint ID must be `17cb:110e` | PHY sequence, windows moved to 0x4000_0000 |
| 4 | endpoint | DMA window (`pagani-wifi-dma` MiB, default 48, at `pagani-wifi-dmapa`, default 0xc0000000: low memory outside XNU's window, write-combined) → stream 0x1401 as a VA64 bypass bank of apps_smmu (Sm8750Platform `SmmuTakeover.h`, the planner NativeUfs uses) → iATU region 1 MEM window, root port base/limit → BAR0 sized and placed (2 MiB → 0x40400000) → BAR0 mapped → MHI registers (all ones: the stage fails there), BHI EE, then TCSR_SOC_HW_VERSION through the 0x3278 window | SMMU, BAR, low memory |
| 5 | firmware + own MHI boot | waits (≤ 300 s) for `amss.bin`, the chip power-up writes if asked (`pagani-wifi-chipreset`, default none), the iMSI receiver (SPI 141) or polling, then WifiMhiLoad: boot loader by BHI, the second ELF of `amss20.bin` (from 512 KiB) by BHIe in 512 KiB segments, contexts before the loader and after its reset, BAR0 and the decode put back if the endpoint's PCIe core restarts, to mission mode. Diagnostic only: the stack is not started | firmware layout |
| 6 | stack + network | the component recipe builds with the stack; a kernel without it fails at once, `[wifi] FAIL step=601 err=-78`, nothing mapped or powered. `/chosen/mac-address-wifi0` (written synchronously in `pagani_wifi_start`, before launchd and MobileGestalt look), the firmware files, `nw_wifi_start` (the stack does its own MHI/QMI), the loop on MSI (or 1 ms polling), the host's restart of the firmware after its calibration, and once the station exists the late registration of `PaganiWifiNet.cpp`'s classes and `en0` | QMI hooks of `Ath12kWifi/tools/peach.py` |

Steps in `pagani_wifi_status[0]`: 101–104 survey, 201–208 power, 301–307 link, 401–406 endpoint, 501–506 MHI, 601–603 stack/net
(`WifiBringup.h`, `PaganiWifi.h`); negative = the step that failed, `[1]` its error. The other words are listed in `PaganiWifi.h`.

### The UFS log's writer

A read that hangs ends in a reset nothing sees coming, and a reset on a UFS write is a power cut in the middle of it (`UfsLog.h`): this flash can
lose a whole logical unit to that (`bDataReliability` is 0 on LUN 0-5). So no write is on the wire while the driver touches a block for the first
time or moves a rail under the UFS path. Each such stretch starts with a line, `[wifi] guard <name>`, then `pagani-ufslog-ms` + 500 ms (3.5 s by
default, 1.5 s with `pagani-ufslog-ms=1000`) for that line to reach the log, then `pagani_panel_probe_quiet` bit 3 (`fetch_or`, `fetch_and`:
bits 0 to 2 are PaganiPanel's, PaganiBattery's and PaganiBt's) and 1 s for a round under way to end. The stretch ends the writer's hold, and a
failure or a stop ends it too. A stretch that hangs never does: the writer stays held, the timed reset's or the panic's last snapshot still goes
out, and the last `[wifi]` line read back names the stretch.

| Stretch | Stage | What it covers |
| --- | --- | --- |
| `controller` | 1 | only when the boot loaders left PCIe0 on: the survey's reads of PARF and the DBI |
| `rails` | 2 | the sleep clock pin, the rail votes (S7I carries the UFS rail), the NoC vote, the PDC messages: steps 201 to 204 |
| `wlan_en` | 2 | WLAN_EN (the chip's load lands on S7I), the GDSCs, the clocks and the first read of the TCSR page, the block resets: steps 205 to 208 |
| `parf` | 3 | the first accesses to PARF and the PHY, the PHY sequence, the BDF-to-SID table: steps 301 to 303 |
| `dbi` | 3 | the first accesses to the controller's registers, PERST#, the LTSSM, L0: steps 304 to 306 |
| `config` | 3 | the iATU, the first request that leaves the SoC: step 307 |
| `dma` | 4 | the first touch of the DMA window (low memory outside XNU's), the SMMU stream, BAR0's sizing and mapping: steps 401 to 405 |
| `bar0` | 4 | the first reads through BAR0 and through the chip's window: step 406 |
| `chip` | 5 | the chip registers `chipPowerUp` reads and writes |
| `mhi` | 5 | the whole of the own MHI boot (the endpoint's PCIe core restarts in it) |
| `stack` | 6 | `nw_wifi_start`, with the stack's chip resets, and at most 2 s after it, or until the firmware answers (stage 3): the QMI lines rounds C are for must not overrun the 32 KiB ring |

Without `sheng-log=1` nothing writes, so the lines stay and the holds and their waits go. With it every stretch costs 2.5 to 4.5 s, all before the
Wi-Fi stack does anything: a stage 6 boot gets `en0` 20 to 36 s later.

### Rail rules

Seven RPMh VRM rails (`kRails`): L3F 1.8 V, L2F 1.2 V, L8B 2.8 V, S4D 0.876 V, S5F 0.876 V, S3G 1.86 V, S7I 1.312 V, in cnss2's
order, through the Adreno830 platform layer (`Sm8750CmdDbVrm`, `Sm8750RpmhWriteActive`, DRV-2 lock word). No disable, ever. Never a
lower voltage than the PMIC reports: a higher set point inside the cnss range is kept, one above it gets no voltage command at all.

**S7I is also the UFS vccq parent** (and, in pagani-linux's description, the input of L3B and L11B). Before any rail is voted its state is read; when it cannot
be read nothing is voted (step 202, `kErrRailRead`) unless `pagani-wifi-s7i=1`. It gets its voltage only when below 1312 mV and its
enable only when off, but always the high power mode: the devicetree leaves it in its low mode until a load of 200 mA is voted, only
UFS's own vote (210 mA) reaches that, cnss2 votes none, and this kernel votes no load at all. After the votes it must read no lower and
no less enabled than before, or WLAN_EN stays low.

The PCIe PHY's LDOs (`kPhyRails`: mainline says L1F/L3G, pagani-linux L3I/L2I; all four are the UFS path's, L3G the display's too) are
never voted: only read in the survey, with their mode. On Android they run in HPM.

The two branch vote registers are read-modify-writes under the lock word, with interrupts off, and written only when a bit is
missing: 0x52000 also carries the GPU's GPLL0 votes, whose driver takes the same lock word (`Sm8750GpuPower`).

## Boot arguments (all default off)

| Argument | Meaning |
| --- | --- |
| `pagani-wifi=N` | stage 1..6 (table above); 0 or absent: off |
| `pagani-wifi-stop=S` | return after step S, any of 101..502 (e.g. 205: power and WLAN_EN, no GDSC; 405: BAR0 mapped, nothing read through it) |
| `pagani-wifi-gen=G` | link speed cap 1..3 (default 2; Gen3 needs cx/mx at NOM, not voted here) |
| `pagani-wifi-dma=MiB` | DMA window size, 8..128 (default 48) |
| `pagani-wifi-dmapa=ADDR` | DMA window physical address, 2 MiB aligned, below 4 GiB, outside XNU's window (default 0xc0000000); 0: XNU memory above 4 GiB, which no host has given this chip (falls back to 32/24/16 MiB) |
| `pagani-wifi-msi=0` | no interrupt: poll every millisecond (default 1: iMSI on SPI 141) |
| `pagani-wifi-sleepclk=0` | never write the PMK8550 gpio3 sleep clock pin |
| `pagani-wifi-s7i=1` | go on and vote S7I when its state cannot be read (default: nothing is voted then) |
| `pagani-wifi-pdc=1/2` | send the devicetree's 4 WLAN PDC messages (2: also pagani-linux's OL-CPR values) |
| `pagani-wifi-fwpart=NAME` | firmware partition (default `modem_a`, then `modem_b`; `none`: Data seed only) |
| `pagani-wifi-fwdir=DIR` | FAT directory (default `image/peach`, then `image`) |
| `pagani-wifi-bdf=NAME` | board data file in `peach/` (default `bdwlan.elf`; e.g. `bdwlan.b0e`) |
| `pagani-wifi-chipreset=MASK` | stage 5 chip power-up writes: 1 LTSSM, 2 hot reset at WCN7850's address, 4 at QCC2072's, 8 interrupt clear, 0x10 QFPROM, 0x20 warm entry, 0x40 global reset (default 0, as cnss2 on this phone; 0x79 is ath12k's set without its hot reset, what the stack does at stage 6) |
| `pagani-wifi-m3=0/1` | stack: 1 (default, cnss2) `m3.bin` = `peach/phy_ucode20.elf`; 0 (pagani-linux) null M3 |
| `pagani-wifi-tme=0` | stack: skip the TME-Lite patch (default 1) |
| `pagani-wifi-aux=0` | stack: skip the AUX microcode (default 1) |
| `pagani-wifi-qdss=1` | stack: clear QDSS_CFG_MISS and download the QDSS trace configuration (default 0) |
| `pagani-wifi-cbc=0` | stack: skip the cold-boot calibration pass (default 1, as cnss2 on this phone; after it the firmware gets 2 s to restart by itself, then the host powers it down and up, once) |
| `pagani-wifi-features=HEX` | stack: the host capability's feature list as given (e.g. 0x46, pagani-linux's) |
| `pagani-wifi-boardkib=K` | stack: board data sent up to K KiB (default 256) |
| `pagani-wifi-aj=0` | no join from `join.txt`, Settings joins; 1 (default): the network in `join.txt` of the Data seed is joined by itself, and again whenever the link is gone (WPA2-PSK or open), so nobody has to type a key |
| `pagani-wifi-log=N` | low 3 bits: the deepest level of the stack's lines in the boot log (default 6: notices, at most 600 lines, errors always; 7: the debug lines too, QMI's among them, a bucket of 200 lines refilled by one every 16 ms, at most 3000, the rest only counted in `held`). +8: a firmware crash's whole RAM dump goes to the UFS log's export area (`sheng-log=1`), read back as `export/table.bin`, `segNN.bin`. `pagani-wifi-log=7`: 18 bytes of the command line, `=15`: 19 |

## Firmware

Read in place, never in the image (`WifiFirmware.c`): first the FAT volume of `modem_a` (then `modem_b`) through NativeUfs
`native_ufs_part_find/read` (`image/peach/<name>`, then `image/<name>`), then the RAM Data seed `/private/var/ngpu/firmware/wlan/`
(`<Android name>`, then its basename), read through the VFS once `/private/var/ngpu` exists (Data mounted). Names: `amss.bin` →
`peach/amss20.bin` (`amss20.bin`), `board.bin` → `peach/bdwlan.elf` or the `pagani-wifi-bdf` override, `regdb.bin` →
`peach/regdb.bin`, `m3.bin` → `peach/phy_ucode20.elf` (m3=1), `aux_ucode.bin` → `peach/aux_ucode20.elf`, `tmel_peach_20.elf`,
`qdss_trace_config_v2.cfg` → `peach/qdss_trace_config_perf_v2.cfg` (`…_v2.cfg`); `join.txt` from the seed only; anything else is
absent.

Every one of these files is on `modem_a`, FAT16 with 4 KiB sectors, LUN 4, block 4870 (this reader over a copy of the phone's real volume:
7 files, sha256 equal; NativeUfs's own part of the path has not run on the phone yet). But Android takes the board data and the regulatory
database from `/odm/etc/wifi/peach`, not from `modem_a` (bdwlan.elf 125328 bytes against 125420, regdb.bin same size, different bytes), and
`/odm` is erofs in super, out of reach here. So for `board.bin` and `regdb.bin` the order is reversed: the seed first. The thread waits up
to 60 s for the Data volume (it comes back about 3 s after the thread starts), takes the seed's copy if there is one and `modem_a`'s
otherwise, or when the volume does not come. A kernel running on `modem_a`'s copies logs `from=modem_a:…` on those two lines. How the seed gets
them: `build-userspace/a830-inputs/wifi/make-wifi-firmware-seed.md`.

## Ath12kWifi copy (`Ath12kWifi/`)

Ath12kWifi `f6bf561` (the component sheng runs) with `tools/peach.py`, run by `tools/prepare.sh` on the generated copies of the fetched
Linux v7.0.14 sources (no ath12k code is kept in the repository, only the marks it edits at, as prepare.sh already does):
`0x110e` is probed as QCC2072, whose v7.0.14 entry already has peach's window register 0x3278, HAL registers, compact RX descriptors,
WCN7850 CE/MHI/WMI, QMI instance 1 and AUX microcode; for `0x110e` only, qmi.c gains the M3 switch, the TME-Lite patch download, the
QDSS configuration download, the feature list and board size options, and the cold-boot calibration pass (CAL report, mode 7, wait for
CAL_DONE 0x3e, mode OFF, second FW_READY), each behind `nw_board_option` (boot arguments above). Android powers the chip off after the mode
OFF and loads the firmware again, so the firmware gets 2 s to come back by itself (10 s from the moment its service is back, if it is);
otherwise `nw_peach_step`, called by `nw_wifi_step` outside every work item, does `ath12k_hif_power_down` and `ath12k_hif_power_up`, once,
with `cal_done` and the firmware's memory kept. That took three things the stack never needed before: `ATH12K_PCI_FLAG_INIT_DONE` set by
hand (the power down returns at once without it, and it only comes with mission mode), a real `flush_work` in the shim (it used to cancel
the item, which is `mhi_power_down`'s shutdown) and the pre-load step of `nw_mhi.c` armed again for the second boot. `tools/peach-sim.sh`
runs the whole stack under ASan against the simulated device in six cases. The WCN7850 path is unchanged (its whole-stack simulation still
ends where it did).

## Build

`scripts/build-pagani-wifi.py` (component recipe, cwd = the tree) replays the tree's XNU compile records, no warning accepted, ARM64
iOS 17.0 objects, `build/pagani-wifi/libPaganiWifi.a`; `--with-ath12k [--fetch-deps]`, which the recipe in `UniXNU/components.toml` passes,
adds the stack (sheng's flags, `-w`, symbols renamed `_nwk…` except the entry points and `nw_board_option`) and refuses an object of it with
a static initializer, so nothing of it runs without `pagani-wifi=6`; `--append-to build/adreno830/libAdreno830.a` joins the force-loaded
archive after checking the platform symbols and that no strong symbol is defined twice (the FAT reader is built as
`pagani_wifi_fat_*`, because PaganiAdsp's copy is in the same archive). `PaganiWifiNet.cpp`'s static initializer is taken out of
`__mod_init_func` and registered late by `netStart`.

The stack needs `Ath12kWifi/deps` (Linux v7.0.14, FreeBSD releng/15.1). On this machine it is a link to a copy outside every repository,
`build-userspace/a830-inputs/wifi/ath12k-deps` (387 MB), and `.gitignore` says `deps` without the slash, because a link is not a directory to
git. Elsewhere `tools/fetch-sources.sh` fills it. It costs the kernel 0.57 MB (424 KB of code, 74 KB of strings, 38 KB of constants, 33 KB of
bss), all of it idle without `pagani-wifi=6`.

Integration is applied: the `components.toml` entry, the `board.toml` component and archive line, Stage7Probe's weak `pagani_wifi_start` and
its call after the haptics start, `make-phone-seed.py --wifi-firmware`, `make-a830-layout.sh A830_WIFI_FW`, and the GPU's use of the lock
words. No defconfig change and no XNU patch is needed (sheng's same network code ran on stock 21H461 XNU; the objects only import kernel
symbols the static link has).

## Settings

wifid's traffic with this driver, from `/usr/sbin/wifid`, IO80211.framework, CoreWiFi and WiFiKit of 21H461 (static analysis, none of it has run on this phone):

| Settings | wifid, IO80211 | Here |
| --- | --- | --- |
| list | scan request type 10, 5520 bytes, SSID at 16; SCAN_DONE; type 11 until `EIO`. `_wifi_device_scan_async` keeps only channels of SUPPORTED_CHANNELS | `nw_wifi_scan`; `kListedMhz` with the DFS channels (flag 0x100, not 0x80). A real scan offers what it heard (`kBssStaleMs`, the module's list keeps everything); joined, the results come from the cache after 2 s |
| join | `Apple80211AssociateAsync`, type 20, 856 bytes: cipher 6 with wifid's PMK (no SAE capability), cipher 10 with the passphrase. ASSOC_DONE, then type 21 in `req_val` | as sheng. The private address the request carries (bit 3 at 0x1d0, address at 0x1fc) is logged and not used: the firmware's station and en0 keep the board's address, so Settings shows an address that is not on the air |
| another network | no leave first (`__WiFiDeviceAssociateAsyncQueued`, 30 s timer) | the module used to drop the request. It now ends the link first, as a leave does |
| wrong password | `_errorForAssociateResult` 21 is -3924, WiFiKit shows Incorrect Password for -3924 and -3925 | `kAssocBadKey`, also when the access point sends a deauthentication between messages 1 and 3 (it used to be a refusal: Unknown Error) |
| joined | LINK_CHANGED up needs CURRENT_NETWORK (else "Link up event with no current network detected. Dropping") | from the list, else the record kept at the join. RSSI follows the joined network's beacons; the scaled value wifid shows moves only on link events (RSSI_CHANGED is not posted) |
| disconnect, forget | CoreWiFi `disassociateWithReason`, `Apple80211DisassociateExt`, type 22; wifid's state moves only on LINK_CHANGED down | the link's end now posts it, voluntary, then BSSID and SSID changed, which refreshes `State:/Network/Interface/en0/AirPort`. A leave during a join answers ASSOC_DONE with a failure |
| switch off | `Apple80211SetPowerState`, type 19 | as a leave, with POWER_CHANGED |
| known networks | wifid and securityd, on the RAM Data volume | nothing here: forgotten at every boot, no UFS write added |

Requests answered EOPNOTSUPP are listed in `工具/gpu-test-loop/wifi-rounds.py` `REFUSED_KNOWN`, each one read for what wifid does without it; a type outside
that list in a round's log is a path this reading missed.

No ADT node `wlan` is needed for wifid: it already runs on this kernel (pid 43 at 14.956 s in the run of 2026-11-01, no such node in the
ADT). launchd's `_LimitLoadToDeviceTree` asks `IODTFindMatchingEntries`, and the `gop-display` nub Stage7Probe makes has IOResources for
a provider, so `IOPlatformDevice::compareName` calls `compareNubName` through a vtable slot IOResources does not have and every name
matches. That is a bug this driver happens to profit from: should somebody fix that nub, wifid stops starting, and
`build-userspace/a830-inputs/wifi/integrate/board-adt-wlan.diff` is the way back.

## Tests

`python3 Test/check-pagani-wifi-host.py` (ASan/UBSan, `-Werror`; mtools for the FAT images): tables, MHI boot (sheng's test + peach
layout), MHI load (boot ROM/loader model), DMA window, PMIC (SPMI arbiter model), firmware sources (FAT16 4 KiB sectors/FAT32 + seed, the
seed-first order of the two `/odm` files), bring-up (simulated PCIe0 corner: order, values, rail rules, every failure path, a survey that
writes nothing), Apple80211 layouts, the network service (`PaganiWifiNet.cpp` itself against stub kernel headers and a model of the module: Settings
request by request), SMMU stream (the real `SmmuTakeover.h`), RDDM reader (a synthetic BHIe table; its export unpacked by
the UFS log's reader), the log gate, the writer's holds (the model hangs in each stretch; the kernel glue's own bit and stretches by text), and
with `Ath12kWifi/deps` the station code (`Ath12kWifi/tools/sta-sim.sh`: join, leave, a join from a joined network, a wrong key) and the whole
stack (`Ath12kWifi/tools/peach-sim.sh`: WCN7850, and peach's calibration pass in six cases). `python3 Test/mutate-pagani-wifi.py`: each
criterion of the holds and of the Settings paths is a source change one of those tests must catch. Host tests do not establish that anything works
on the phone.

The rounds on the phone, what each must show and what to do when it does not: `build-userspace/a830-inputs/round-1101/wifi.md` section 5,
`build-userspace/a830-inputs/fullrun/make-wifi-rounds.sh` for the images, `工具/gpu-test-loop/wifi-rounds.py` to run them.

## What needs a device round

In order: stage 2 stopped at 204 (do the rails answer and does S7I stay, with UFS on it), stage 5 (GCC votes under CESTA, PHY, L0,
`17cb:110e`, Gen/width, SMMU bank, BAR0 size, chip version, the first real `native_ufs_part_*` reads, the low DMA window, does the PBL take
the loader, the split image, MSI), stage 6 (does the firmware run under this stack at all, calibration and the host's restart, then `en0`,
scan, join, DHCP, then Settings: `工具/gpu-test-loop/wifi-rounds.py` `WALK`). When the firmware crashes the log has the root port's AER registers, the stream's SMMU state and what the RAM dump says
(`rddm` lines); `pagani-wifi-m3`, `-qdss`, `-features`, `-pdc`, `-cbc` are the variables after that. RF_CLK1 is not voted here; if peach
needs it, a BCM/VRM entry.

Known limits: no power-down path (the rails and the link stay up until reboot), no suspend, no 6 GHz, WPA2-PSK/CCMP or open networks
only (no SAE, no required PMF), nothing joined from Settings stays remembered across a boot (the keychain and the known networks live on
the RAM Data volume, the software keybag's class keys are per boot), DIAG/QDSS traces are not read.
`build-userspace/a830-inputs/wifi/review.md` has the hardware review of stages 1 to 5.
