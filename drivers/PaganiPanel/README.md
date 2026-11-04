# PaganiPanel

DSI brightness and display blanking for the OnePlus 13T AA594_P_7_A0020 panel. Enable with `pagani-panel=1`. Stage7Probe loads AppleARMPlatform and registers an owned subclass with genuine `AppleARMBacklight` ancestry; its constructor, storage size, destructor and 169-slot class extent are checked against the pinned 21H461 image. The subclass uses the ordinary IOService lifecycle and guards the unqualified extra virtual method.

`setProperties` takes an integer `brightness` and a `FadePeriod` (16.16 seconds, at most 5 s), clamps the level to 0..0xdbb, the board's normal maximum, packs both into one word and arms the timer; the caller never waits for the work loop. The tick sends DCS 0x51 high byte first (`51 05 99` for the boot level: the panel's own tables, `qcom,mdss-dsi-bl-inverted-dbv` and the vendor UEFI agree) in high speed, stepping every 16 ms from the level shown to the goal. Values are published when a fade ends. The display PM callback requests DCS 0x28/0x29 (bit 6 below keeps them from the panel). These commands retain the existing clocks, PHY, DSC configuration and GOP scanout; they do not power down panel rails.

The panel's level is 12 bits: 1 the least light, 0xdbb the normal maximum, 0xffe the hardware's (for HBM, which is not here), 0x599 the boot level, 0 dark. `IODisplayParameters` publishes 0..0xdbb, so CoreBrightness's level (`ceil(x * factor * max)`, slider floor 1/max) is the panel's own value, in a straight line. OLED mid levels look bright on it; a curve belongs after the path is proven on the device.

## Boot argument `pagani-panel`

One number, so the command line (998 of 1020 bytes in the full package) grows by a byte or two at most. Bit 0 runs the driver: without it Stage7Probe loads nothing and the system has no backlight, the way back from everything below.

| Bit | Value | Effect |
| --- | --- | --- |
| 0 | 1 | Run the driver: first DCS, then the backlight (the default of the packages) |
| 1 | 2 | Publish even when the first DCS failed: tests CoreBrightness's side alone |
| 2 | 4 | No forced DCS when the checks refused the first one |
| 3 | 8 | Brightness and display on/off in low power instead of high speed |
| 4 | 16 | Never publish the backlight: no `backlight-control`, no registered service, `CADeviceHasBacklight` stays false, so the render server cannot hang on it (sheng int-236). The DCS path, display power and the reports stay: `pagani-panel=17` tells a hang of the publication from one of DCS |
| 5 | 32 | Ignore `FadePeriod`: the goal at once |
| 6 | 64 | Never send 0x28/0x29: the tick takes the display as on whatever the system asks, so the screen goes dark on the display path's black frames alone, as it did while no DCS got through. `pagani_screen_state` and the system's view stay as they were; the brightness stays in use, also while the display is off (the DSI link runs as with the display on). The way out of a panel that stays dark after a real display off, without losing the brightness |
| 7 | 128 | The register probe's low-risk stages, P0 and P1 ("Register probe" below) |
| 8 | 256 | The formal path, for after A1 ("The formal path" below): `start` sets `DISP_CC_MDSS_AHB_CLK`'s enable bit behind A1's gates, before its first read of DSI, and sends one DCS. Then reads only: nothing published. Only in a first test with a timed reset |
| 9 | 512 | A1: at about 131 s set `DISP_CC_MDSS_AHB_CLK`'s enable bit, behind its gates, and read what that clock opens ("A1" below): only in a first test with a timed reset of 240 s, never in a package someone uses |
| 10 | 1024 | Its medium-risk stages, P2 and P3, behind P1's gates: only in a first test with a timed reset, never in a package someone uses. Alone it reads nothing |
| 11 | 2048 | A0: set `GCC_DISP_AHB_CLK`'s enable bit if it reads clear, before the first read of DSI, and read DSI's version. Such a boot sends no DCS without bit 12 |
| 12 | 4096 | With bit 11: the commands may go, through the gate below. With bit 8: the slider, the backlight published and the commands open after the first DCS. Alone it changes nothing |

With bit 7, 8, 9, 10 or 11 set a boot's commands go through the gate below; without them nothing of this is in the way, and `pagani-panel=65` and `=67` do what they did. Three digits cost a byte of the command line and four digits two (`577` = 1 + 64 + 512, `1217` = 1 + 64 + 128 + 1024, `321` = 1 + 64 + 256, `4417` = 321 + 4096).

Images, narrowing down a failure: `pagani-panel=65` for a panel that stays dark after display off, `pagani-panel=17` for a render server that hangs, `pagani-panel=81` for both, and `pagani-panel` in `A830_DROP_ARGS` for no driver at all. `make-a830-layout.sh` takes `A830_EXTRA_ARGS` and `A830_DROP_ARGS` as a whole, so add to the values the package is built with, never in their place: the full package's carry `pagani-haptics` and the like, and `startup_debug`. An extra argument replaces the board's of the same name, so added to them `=17`, `=65` or `=81` grows the line by one byte.

## Start

`start` logs the controller (`panel: start ...`, two U records), then sends the first DCS, the level the boot loader set again, so nothing changes on the panel. Refused by the checks (-19, -16: nothing was written), it goes once more with `DSI_PANEL_FORCE`, without them, and the commands after it the same way when that worked: Build 50 read `DSI_CTRL` bit 0 clear while the screen updated, which only the engine's answer can settle. Not on a controller whose version register reads 0, though: reads do not reach that block, no completion could be seen there, and the forced send would be this port's first write to it. A first DCS that does not complete leaves the service unregistered with its reports running, and the system runs as it does without this driver.

A controller that answers is a state this port has not seen, and the first write to it is the risky one: a -110 on the unforced path leaves the engine's registers as they are (`PeripheralIO.h`), with the frames behind them. So a boot with bit 7, 8, 9, 10 or 11 goes through `pagani_panel_dcs_gate`, once, with the registers `start` just read. It wants what a working controller shows: a version of the form `0x2009xxxx`, `DSI_CTRL` with bits 0 and 2, the low four bits of `TRIG_CTRL` at 4 (the send path does not look at that one). Anything else is held, version 0 included: `panel: dcs held shape ver V ctrl C trig T`, a -19 for the first DCS and for every command of that boot, which then writes nothing and reads nothing of DSI on the way (`pagani_panel_send_packet`), so a probe with bit 1 stays read only however a later read turns out. Without those bits the gate is open and nothing changes, the forced send above included. With bit 11 the boot only reads (`panel: dcs held a0 ...`), however well A0 went, so that the first write to DSI never comes as a side effect of the clock coming back; with bit 12 as well the shape decides.

On success `IODisplayParameters` comes first: QuartzCore waits for the libEDR server CoreBrightness starts while opening that display as soon as `backlight-control` makes `CADeviceHasBacklight` true, and CoreBrightness refuses a display without the parameters. Then `NativeSandboxGrantProcess=backboardd`, `backlight-control`, `Built-In`, `registerService`.

The grant is needed, genuine ancestry or not: CoreBrightness sets `{brightness, FadePeriod, backlight-level}` in one dictionary, and no sandbox profile of the 21H461 kernelcache has a `FadePeriod` literal (it appears only in AppleARMPlatform's own strings; the profiles' fragments around `AppleARMBacklight` hold `brightness-nits` and the like). That is a string search, which a pattern rule would pass; what settles it is sheng int-276, which needed the grant with the same ancestry: without it `is_io_registry_entry_set_properties` returns `kIOReturnNotPermitted` before `setProperties` runs (`patches/xnu/zzwb-native-sandbox-grant-set-properties.patch`).

## Concurrency

- All DSI traffic is one tick at a time on the service's work loop (`PanelBrightness.h` `pagani_panel_tick`); the first DCS in `start` comes before the timer can fire. The tick asks for the next one only while a sequence, a fade or a retry runs: an idle panel and a panel that is off cost no wake-up (the old timer ran 62.5 times a second).
- Three threads arm the timer (CoreBrightness's in `setProperties`, the display PM's in `pagani_panel_set_power`, the tick). `wakeAtTime` increments its generation and reads it back without a lock, and a thread overtaken in between makes `timeoutAndRelease` drop the tick, possibly the one that sends 0x29: every arm takes `panelTimerLock`.
- The timers run in the thread-call group `user` (priority 31), not the default `kernel-high` (92) of the display's vsync timer: a command polls for the frame gap with `IODelay`, which blocks on this port (no `cpu_idle_latency`) and would return every 100 us ahead of what runs on its CPU. A fade step late under load shows nowhere; the price is that 0x29 too waits for a free CPU.
- The MDP's autorefresh sends every frame by itself, 6.8 of the 8.3 ms at 120 Hz, and stays on: only the command DMA is touched, never CTL or INTF. A command waits for the gap (status bits 1 and 2 clear, polled every 100 us, at most 20 ms; the hardware arbitrates the few that meet the next frame, as on Android). Nothing else in this port writes DSI: `pagani_panel_dcs_write`, which `panelTo120` would use, is not provided (a provider must go through this tick).
- With the display off no command goes except the 0x28 itself and what was refused before it, and a goal asked meanwhile is written after 0x29; without bit 7, nothing reads DSI either. The probe's P0 and P1 do not look at the display: they read DSI at their moments, on or off. `pagani-te-ar-off=1` (autorefresh off with the screen, in no package) is not qualified with this driver or with the probe: 0x28 and 0x29 are not ordered against the display path's autorefresh writes, as before.
- A command not sent (-19, -16: nothing was written) goes again 100 ms later, 600 times in a row at most; anything else stops the requests, as a command the engine may still own must not be followed by another. `PeripheralIO.h` leaves it to the engine then, unless forced with the DMA engine not busy. The limit does not hold for a dark panel that needs the running sequence to come back (a display on, a wake, the sleep after 0x28 went out): stopping there would leave the screen black until a reboot, where the system's black frames used to bring it back by themselves, so those go on until the controller takes them. A -110 on them still stops: that screen stays dark, which is what bit 6 is for.

## Logs

The ramoops events ring keeps the last 32 KiB, so the boot's records are gone by the time a long run is read: `panel: start ...` and `panel: dcs ok` or `panel: no dcs` (with the registers) at boot, the registers again 45 s later, then one `panel: requests N writes N retried N shown N goal N rc N mode N power N/N off N on N` a minute while the counters move and every ten minutes when they do not (with the registers again while the commands do not work). `panel: DSI failed -110 at packet 0 of the wake sequence; requests disabled` when they stop. Stage7Probe logs `panel: stage7 AppleARMPlatform load 0, class registered, service started` and, when no service came up, repeats that line after 45 s and every ten minutes, since nothing else would.

The portable tests cover packet byte order, register preservation, busy engines, completion timeouts, the first DCS with every way it can go and the gate before it, what each option bit changes, the reporter's timing, the frame gap on a modelled link (no thread in it, so its count of commands that meet a frame is no upper bound), the slider's formula down to the wire and the whole tick on a virtual clock; `scripts/test-peripherals.py` of the device repository runs them and checks the order of publication in `start`; `Test/panel-start.py` runs this whole `PaganiPanel.cpp` against stubs of the kernel, and `Test/mutate-pagani-panel.py` turns each criterion of bit 8 into a source change one of these must catch. They do not verify physical luminance, CoreBrightness sandbox access, display-off/on behavior or coexistence with the firmware scanout. Host test: `xcrun clang++ -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -I <UniXNU-sm8750-common>/kits/Sm8750Platform -I <device>/package Test/panel-brightness.cpp -o /tmp/panel-brightness && /tmp/panel-brightness`.

## Register probe (`PanelProbe.h`)

The 13T reads DSI0 as all zero, version register included, with no abort and no hang, while the screen updates; XNU has never read anything but zero from MDSS or INTF, and never written to either. The probe asks why, read only, on a thread of its own (the work loop stays free), started in `start` after the reporter is set. P0, P1 and P2 ran once (below); P3, A0 and A1 never did.

| Stage | Moment | Reads | Gate |
| --- | --- | --- | --- |
| P0, bit 7 | 266 s | `0x127004/8` (`GCC_DISP_AHB_CLK`, which Linux sets at boot and this port never did), TE GPIO 86, DSI `0, 4, 8`: 1024 reads of each for the latency, then 100 ms of DSI groups 20 us apart with TE in every pass | none |
| P1, bit 7 | 286 s | dispcc: GDSCs, AHB, MDP, link clocks; then 64 samples 250 us apart of AHB, MDP, byte, pclk, the core GDSC and, when that sample saw the GDSC on, DSI's version | `0x127004` on: bit 0, and CLK_OFF clear or bit 1 (hardware gating) |
| P2, bit 10 | 306 s | RSCC power status; wrapper; SCC; CRMC; last the PHY | RSCC: `rscc`. Wrapper, SCC: `rscc` and the GDSC. CRMC: `ahb` and `rscc`. PHY: `ahb` |
| P3, bit 10 | 346 s | INTF_1 `0, 0xac, 0x298, 0x2b4`, frame and tear counts twice 4.5 s apart. No MDSS top, DSI1, CTL, SSPP or SMMU: the snapshot that hung the phone on 2026-10-30 began with those | P1's 64 samples and 64 more right before the reads with everything on and no gap of more than a frame, the core GDSC on, the display on |

- Every window is mapped read only (`Mapping.h`, `kIOMapReadOnly`): a stray write faults. A stage's windows only.
- A block's first read comes after `panel: pN block next` and 3.5 s, so the UFS log's 3 s snapshot takes the line off the phone before a read that may hang. Values go to the log as soon as they are read and to the disk with the next round: at the next block, and before P1's phase and P3's second look, which give the writer a round of their own.
- No read comes with a UFS write on the wire: past that sleep `pagani_panel_probe_quiet` (bit 0; bit 1 is PaganiBattery's USB probe, whose reads fall in A1's seconds) holds the log's writer as the Power key does, 1 s before the read, until the next block's line or the end of the stage. A read that hangs ends in a reset nothing sees coming, and a logical unit without `bDataReliability` is the one thing a reset can hurt. The word stays set after a hang; the last snapshot of a timed reset or a panic still goes out. Every stage comes after `pagani-input-script`'s keys (241 to 262 s): a read that hangs costs the run what follows it.
- A gate is read right before its block, after that sleep. A branch of dispcc counts as on when its enable bit is set and its group of reads is not all zero: a controller that reads as zero is no controller with its clocks on. AHB and the RSCC's branch, which carry the register reads, also need CLK_OFF clear or hardware gating. MDP, byte and pixel do not: `DISP_CC_MISC_CMD` can stop those between frames with CBCR bit 1 clear, so their CLK_OFF on a healthy display says nothing (`panel: p1 clk_off ...` counts it). The core GDSC's power state is `CFG_GDSCR` (`0xaf09004`) bit 16, for its `POLL_CFG_GDSCR`, not GDSCR bit 31.
- The pacing is `CNTPCT_EL0`, not `IODelay`, which blocks on this port, and each sample counts from the one before: a thread that lost the CPU does not catch up in a burst. The phase line gives its real length and its longest gap; more than a frame (8.3 ms) and it does not count as all on.
- Bit 10 needs P1's results, so alone it logs `panel: p2 skipped no p1` and reads nothing.

Lines, about 30 U records, all from 266 s on (the events ring keeps those): `panel: p0 gcc ahb A axi B`, `panel: p0 lat gcc N te N dsi N` (ticks of 19.2 MHz for 1024 reads; DSI far above GCC would mean a trap to EL2), `panel: p0 dsi n N nz N or A/B/C first V@US`, `panel: p0 te rise N per MIN-MAX us high P% mean N ns` (the period over the whole sampling, what `sheng-vsync` takes in microseconds), `panel: p1 gdsc`, `p1 ahb`, `p1 mdp`, `p1 link`, `panel: p1 phase n 64 off ahb N mdp N byte N pclk N gdsc N dsi nz N lat TICKS us US gap US`, `panel: p1 clk_off mdp N byte N pclk N`, `panel: p2 rsc`, `p2 wrap`, `p2 scc`, `p2 crmc`, `p2 phy`, `panel: p3 intf te T fc A->B int C->D ar R`, `panel: p3 done`, `panel: probe end`. Each gate that closes says so: `panel: p1 skipped gcc V`, `panel: p2 crmc skipped ...`, `panel: p3 skipped ...`.

A0 (bit 11) is the one write: in `start`, before the first read of DSI, through a window that lives for that call. When `0x127004` bit 0 reads clear it sets that bit and nothing else, reads back, waits for CLK_OFF for at most 1000 polls and reads DSI's version: `panel: a0 gcc B->A ver V`. With the bit set it writes nothing and reads no DSI: `panel: a0 gcc V already on`. That moment's lines may fall out of the events ring (the GPU's trace at boot), so the last one, a held first DCS and a thread that did not start go again with the first report, at 46 s, whose `panel: report ver ...` shows the controller as well. Windows the thread could not map say so at the first stage's moment, for the same reason.

### What the first run read, and A1

Run `20261101-110201-stage15-smoke-1`, `pagani-panel=1217`. CBCR: bit 31 CLK_OFF, bit 1 hardware gating, bit 0 enable; bit 27, set in every CBCR read here, has no public meaning.

| Read | Says |
| --- | --- |
| `p1 ahb 88000000`, `off ahb 64` of 64 | `DISP_CC_MDSS_AHB_CLK`, `mdss_mdp`'s `iface_clk` and `disp_crm`'s clock in the device tree: enable and hardware gating clear, CLK_OFF set. Off, and nothing else in this table is |
| `p0 gcc ahb 88000003` | `GCC_DISP_AHB_CLK` enabled, hardware gated, stopped while idle, as a read of GCC itself finds it. The clock controller behind it answers: A0 has nothing to do |
| `rcg 0/100001`, `pll e0000005 100` | AHB's RCG runs on XO (source 0, 19.2 MHz, HW_CLK_CTRL), root not off; PLL0 locked, PLL1 off and not needed |
| `nongdsc 8000000`, `lut 8000000` | Voted branches: bit 0 is only this CPU's vote, CLK_OFF clear says they run |
| `gdsc fa22f800/4070000` | Core GDSC on (CFG bit 16), its wait fields those of the Linux driver, HW_CTL clear: software's, no hardware collapse. INT2 off, nothing here touches it |
| `mdp`, `byte`, `pclk`, `esc`, `clk_off` 0 of 64, `misc 0` | The frame's clocks run through two frames, ungated |
| `rsc pwr 0`, `scc f0 0 0 0`, `wrap 0 0 0` | CESTA is not at work: 0xf0 is what SCC_CTRL is given disabled, the GDSC never collapsed. The CRM drives only the MDP's RCG and PLL0, not AHB |
| `p0 dsi n 4930 nz 0`, `p0 lat gcc 11038 te 9984 dsi 17927` | Not a matter of the frame's phase, and no trap to EL2: under a microsecond a read |
| `p0 te rise 13 per 8293-8305 us` | The panel is a little faster than 120 Hz (the software tick: `sheng-vsync`, NativeMobileDisplay) |

So a register space with its interface clock off reads as zero here, which fits everything above and proves nothing: only that clock on can. A1 is that experiment, one bit, at about 154 s, in a boot that resets at 240 s (`A830_RESET_AFTER=240`, `A830_PANEL=577`; `pagani-input-script`'s keys then never start). The moment is its own, before the stages above: those start after 240 s.

| Time | Step | A hang there leaves as last line |
| --- | --- | --- |
| 131.5 s | TE alone, 200 ms, for its mean period: `panel: a1 te rise ...` | `a1 te next` |
| 136 to 150 s | GCC, DSI's and dispcc's lines; the state: `panel: a1 state gcc G ahb A ahb1 B rcg C/F gdsc R/C mdp M` | `a1 dispcc next` |
| 150 s | `panel: a1 write next ahb A`, 4.5 s for it to leave the phone | |
| 154 s | The state again and the gates (below), the phase, `panel: a1 pre ...`; the write through the thread's one writable page, `0xaf08000`; up to 1 ms for CLK_OFF: `panel: a1 ahb A->B us N` | `a1 write next` |
| 154 to 159 s | `panel: a1 dsi next`, 4.5 s, DSI's 14 registers: `panel: a1 dsi ver V ...`, `a1 lanes ...`, `a1 dsi back`, `zero` or `other` | `a1 dsi next` |
| 159 to 163 s | The phase with the clock on: `panel: a1 phase n 64 off ahb N ... dsi nz N` | `a1 dsi back` and no phase |
| 163 to 172 s | P3 (INTF_1), when that phase was whole | `p3 intf next` |
| 172 to 195 s | P2 with the clock on: RSCC, wrapper, SCC, CRMC, PHY | that block's `next` |
| 195 s | `panel: a1 mdss next`, `0xae00000` only (`0xae00010` began the snapshot that hung), and only after `a1 dsi back` and INTF_1 reading something: `panel: a1 mdss ver V` | `a1 mdss next` |
| 199 s | `panel: probe end`. The timed reset undoes the bit: nothing else does | |

The gates, read after the sleep they follow, each by a line of its own, `panel: a1 held WHY gcc G ahb A rcg C/F cfg C display D`, and no write: `gcc` (`GCC_DISP_AHB_CLK` not on), `zero` (dispcc reads as zero), `hw gated` (AHB's bit 1), `root off` (the RCG's ROOT_OFF), `not xo` (its source is PLL1, which is off), `gdsc`, `display` (the system's), `phase` (MDP, byte, pixel or the GDSC off in a sample of 64, or no phase without a gap of more than a frame in three). Bit 0 set already is no refusal: `panel: a1 ahb A already on`, and the reads go on.

| Lines | Means | Next |
| --- | --- | --- |
| `a1 held ...` | Not this boot's state: nothing changed | Read WHY |
| Log ends at `a1 write next`; or a panic with this page as FAR | The write hangs or is refused | No A; the slider needs route B |
| `a1 failed ignored` | The bit does not stay | Who owns the page: no A |
| `a1 failed stuck ahb A rcg C/F gdsc R/C` | Enable set, CLK_OFF stays: the branch has no clock | Read the line; do not touch the RCG blind |
| `a1 dsi back`, `phase ... off ahb 0 ... dsi nz 64` | The cause. The panel's registers read as on any working board | Next round: the bit in `start`, before `logState`, then the first DCS by the gate above, with a timed reset. `fc` growing by about 542 in P3 also says the scanout never noticed |
| `a1 dsi zero` with `p2 phy` or `p3 intf` not zero | The clock was not all of it, DSI's own | `ahb1`, then route B |
| `a1 dsi zero`, PHY and INTF zero | All of MDSS reads zero with its clock on | CESTA, firmware: route B |
| `phase ... off ahb` above 0 | Something else turns the branch off | The word to look for is on the same line |
| `a1 dsi other` | An answer not of a working controller: the commands stay held | Read the two lines |

The bit does not touch the frames' clocks, CTL, INTF or DSI, and the commands stay held (`panel: dcs held shape`, at 1.5 s, for the whole boot): no DCS comes of it, with bit 1 or without. What it may do to a running scanout is the thing only a device says; INTF_1's frame count is what the log has to say it with.

Host test (the model's write callback aborts on every window but A1's and, for bit 8's first DCS, the service's own; every address read is on a list written out in the test; each gate alone and read after its sleep; breadcrumbs on every run; the timing on a virtual clock, with the thread losing the CPU; the longest line each format can print): `xcrun clang++ -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -I <UniXNU-sm8750-common>/kits/Sm8750Platform -I <device>/package Test/panel-probe.cpp -o /tmp/panel-probe && /tmp/panel-probe`. The test cannot say whether a gate is a safe one on the hardware: that is what the first run is for.

## The formal path (bit 8, after A1)

What comes of A1 if its table ends in `a1 dsi back`. Written with A1 still unrun, so everything below that says what the hardware does is the model's: default off, and every value without bit 8 runs, read for read, as before.

`start` calls `panel_probe_ahb` before `panel: first DSI read next`, on the windows of a stage of their own (`PANEL_PROBE_AHB`: GCC, DSI and dispcc read only, A1's one page writable), gone with the call. It is A1's code, `panel_probe_a1_write`: the same gates, each read right before the write, the same 64 samples, the same one bit. Then the gates of DSI, `logState` as ever, and the first DCS: the boot level again, through `pagani_panel_dcs_gate`, which now also wants the clock found on (`held ahb`). Nothing is forced on this path, and bit 1 is no way past it. The moment is about 1.5 s, on the thread that goes on to the GPU and the root mount, so a sleep here moves all of those; the writer has been up since 1.2 s.

Right after the command, read only (`pagani_panel_ahb_after`): `dsi_panel_snapshot` against what `start` read before it, and the frames. Any of these holds the commands for the boot and publishes nothing, `panel: ahb dcs rc -5 WHY held lost frames A->B`:

| WHY | Means |
| --- | --- |
| `shape` | Version, `DSI_EN`, `CMD_MODE_EN` or the DMA trigger no longer those of a working controller |
| `config` | `DSI_CTRL`, `TRIG_CTRL`, `DMA_CTRL` or `DSI_CLK_CTRL` changed: one command changes none of them |
| `tpg`, `busy` | The FIFO's mode stayed, or the engine still has the command |
| `timeout`, `fifo`, `error` | New in `DSI_TIMEOUT_STATUS` (HS TX, LP RX, BTA), `DSI_FIFO_STATUS` (the underflows and overflows) or `DSI_INT_CTRL`'s `DSI_ERROR`. What the boot loader left there does not count |
| `stream` | `DSI_STATUS` bit 2 rose in the 64 samples before the command and not in those after it. That bit was never read on this phone: a boot on which it shows nothing proves nothing and fails nothing |

A refused command (-19, -16) and a -110 hold them too, `panel: ahb dcs rc RC no read held lost`, and a -110 leaves the engine as it is, as everywhere (`PeripheralIO.h`): the timed reset of a test image is what ends that. After the first DCS each command reads the shape first (`pagani_panel_send_packet`): DSI reading otherwise, the branch gone for instance, is a -19 with nothing written, for good, the 600 retries of the tick and `panel: DSI failed -19`. The slider stays where it was and the system goes on.

The writer is held from before the first read on, by bit 0 of `pagani_panel_probe_quiet`, through `logState` and the first DCS. `pagani_panel_ahb_after` lets it go on every way out, and only a hang does not get there.

Two speeds. Bit 8 alone is careful: a moment no run has seen, so every step has its line off the phone first, about 18 s of boot in all. With bit 12, for a package and only after that went well on a device, one hold of 1 s covers all of it: the steps are the same, the writer's one round at the start of a hold takes the log up to `panel: ahb next` off the phone (`UfsLog.h`), and a hang leaves that line last with nothing on the wire.

| Time, careful | Step | A hang leaves as last line |
| --- | --- | --- |
| 1.5 s | The state of GCC and dispcc: `panel: ahb state ...`, the gates | `ahb dispcc next` |
| 6 s | `panel: a1 write next`, 4.5 s, the gates again, the phase, the write: `panel: a1 ahb A->B us N` | `a1 write next` |
| 10.5 s | `panel: ahb dsi next`, 4.5 s, the gates of DSI, `panel: start ver ...` | `ahb dsi next` |
| 15 s | `panel: first DCS next`, 4.5 s, the frames, the command, the read back, the frames | `first DCS next` |
| 19.5 s | `panel: ahb dcs rc 0 clean held one frames 2->2`, `panel: dcs held one ...`, `panel: dcs ok ver ...` | |

The first report, at about 65 s of such a boot, says the outcome again: the GPU's trace comes right after `start` and takes its lines out of the events ring.

| Image | `A830_PANEL` | Reset | Does | Goes on when |
| --- | --- | --- | --- | --- |
| A1 | 577 | 240 s | A1's table above | `a1 dsi back`, `phase ... off ahb 0 ... dsi nz 64`, `fc` growing in P3 |
| One DCS | 321 | 240 s | This path, careful. No system side: nothing published | `panel: ahb dcs rc 0 clean held one frames N->N`, `panel: report ver 2009... ` with `writes 1`, the system up, the screen updating |
| Slider | 4417 | 420 s | This path, the backlight published, no 0x28/0x29 | `requests N` growing with the slider, `writes` with it, `rc 0`, the light following; backboardd up (no `CADeviceHasBacklight` hang) |
| Display power | 4353 | 420 s | With 0x28/0x29 (bit 6 clear) | Screen off and on under a person's eye |
| Package | 4417, or 4353 if that went well | none | | |

Back from any of them: `A830_PANEL=65`, a value and no code. `4433` is the slider's image without the publication (bit 4), for a render server that hangs on it. Bit 8 with bit 7 runs P0 and P1 behind it as ever, which is worth it once: P1's `ahb` and the phase with the branch on.

What the "DCS" of the second image is: the one command the slider sends all day, at a value that changes nothing, and after it only reads. A real DCS read, the panel's own answer, is the DT's ESD check, `qcom,mdss-dsi-panel-status-command` `06 00 00 00 00 00 01 0a` in low power, 0x9c expected after a page 0 write. That needs a bus turnaround on a link with the frames on it, the read-back registers and a way out of a turnaround without an answer, which this port has none of (no soft reset), and the slider does not need it. Not here.

A boot that reads otherwise here than A1's did at 154 s shows it by the two runs' lines side by side, `a1 dsi ver ...` and `panel: start ver ...`. A `tpg` other than 0 at A1 means -19 here, as nothing is forced: decide then, with the value at hand.

### `pagani-te=1` with this path (evaluation only)

`NativeLegacyDisplay::start` measures the TE first thing (`lockTe`, 1.116 s, `GOP display start enter`), and `startPeripherals` runs after it: this driver's `start` comes about 0.4 s later, behind the touch. So the lock's 100 ms read INTF_1 with the branch off, read zero as far as the first run can tell, and find no TE: `te.valid` stays clear and the tick stays software, nothing worse if that reading is right. For the lock to work the order has to change one of two ways:

- The bit before `display->start(nub)` in Stage7Probe's `native_legacy_display_late_probe`, at about 1.1 s. That is before the UFS log opens (1.214 s), so before the crumbs of a careful boot could leave; and before the class of this driver is registered, so it would be a plain function.
- The lock after the bit: a call of the display's `lockTe` on its work loop behind `startPeripherals`. The better one: the refresh class the display published stays 120 for a TE of 120.5 Hz, so nothing has to be published again. One line in Stage7Probe, a small function in NativeLegacyDisplay.

Neither is done. A TE lock also reads INTF_1 at every tick for the rest of the boot, 120 times a second, and P3 has not run yet: `p3 intf ... fc`, `int` growing, is what it would stand on. `sheng-vsync=8299` already gives the period without touching MDSS.

### Route B, when A1 fails (plan only, nothing of it is written)

For `a1 held` that stays, a write that hangs or does not take, `a1 failed stuck`, and `a1 dsi zero`. It dims in the GPU's copy, so it can only go down from what the boot loader left (0x599), and it needs no register of the display. The smallest version:

1. `A830_PANEL=67` first, no code: whether a published backlight hangs the render server. B publishes too; if it hangs, B is off the table.
2. PaganiPanel: a new bit, 13 (8192; bit 8 is taken), so `8257`, 1000 bytes. It publishes with the first DCS held, `max` and the clamp at 0x599, so the slider's top is the boot level. The tick stays and the fade with it; `sendPacket` takes 0x51 as a level, q8 = 40 + 216 * level / 0x599, 256 at the top, hands it to the display, returns 0, and takes 0x28/0x29 as done. No DSI.
3. Wire: `NGP_METHOD_WAIT_SWAP3` (6), `NGPSwap3 {NGPSwap swap; uint64_t target_offset; uint32_t dim_q8, reserved;}`, in both copies of `NativeGOPWire.h` (here and UniXNU `drivers/GopFramebuffer`); `NativeGOPPresenter` takes q8 from the panel; Adreno830's tunnel to 0x7006 (`NativeAdrenoHandoffLast`, its method list, `tests/test_userclient.cpp`).
4. Plugin (AdrenoMetal): `GpuScanoutClient` tries SWAP3, then SWAP2; `Frame.dimQ8`; below 256 a compute kernel in place of the blit, one dispatch per `copyRuns` run with `targetOffset`, so the status band stays. Its pipeline built when the first frame below 256 comes, never in `Client::open`: the plugin is in the seed whatever `pagani-panel` says. The first failure drops it for the boot and goes back to the blit.
5. Seed: `deploy-findmy-plugin.py --seed --trust`, not `build-userspace-a830.sh`.
6. Accepted when: `requests` grows with a person's slider, the light follows, colours as before, median `copy` at most 4 ms, 100 fps or more while it scrolls. Known limits: never brighter than the boot level, some banding near the bottom; a CPU-copied frame is not dimmed; without a redraw of the last frame a change shows with the next frame, so a still screen's fade does not (the kernel's redraw of `build-userspace/a830-inputs/brightness2/plan.md`, with the CPU path clearing what it remembers, is the step after this one).

## Display power for the other drivers (`pagani_screen_state`)

The display PM callback (`pagani_panel_set_power`, called by NativeLegacyDisplay's `setPowerState`) also publishes the system's display power in the weak word `pagani_screen_state`: bit 0 set while the display is off, the number of changes above it, so a reader that missed an off and an on still sees the count move. It is written whatever `pagani-panel` says. PaganiArmCpu's governor (`pagani-screenoff` bit 0) and Adreno830's screen rule (bits 1 and 3) read it; each carries the same weak zero definition, so the kernel link keeps one word and a build without this driver reads the display as on.

## Sleep mode while the display is off (`pagani-screenoff` bit 2)

Default off: the display PM callback then sends 0x28 and 0x29 alone, through the same sequence engine with one packet each. With bit 2, `PanelPower.h` runs the panel's own sequences from the PKX110 live device tree (`qcom,mdss_dsi_panel_AA594_P_7_A0020_dsc_cmd`, the same in all four timings), one at a time, each to its end, honouring the DT's delays (the timer is armed for the delay that is due):

- off: display off 0x28, 20 ms, enter sleep 0x10, 120 ms (the DT's `qcom,mdss-dsi-off-command`, high speed). Android then turns the rails off; here the rails, reset, DSI, PHY, DSC and the scanout stay as the boot loader left them.
- back: exit sleep 0x11, 120 ms, then the 51 writes the vendor sends after exit sleep in `qcom,mdss-dsi-on-command` (page 0x51/0x52 tables, identical in every timing, unlike the part before exit sleep, which the boot loader wrote and the panel keeps), page 0, display on 0x29, low power as the DT says; the brightness is written again right after.
- a display asked back on between 0x28 and 0x10 skips the sleep; an on during the 120 ms after 0x10 waits for them (MIPI DCS); an off during the wake runs the wake to its end first.

Packets go through `dsi_panel_packet_mode` (`Sm8750Platform/PeripheralIO.h`: DCS short 0x05/0x15 and long 0x39 writes up to 16 bytes; `dsi_panel_command` is a wrapper of it). A packet that was not sent goes again like any command; a failed one disables further requests, and the panel then stays as it was (asleep: dark until reboot). Each finished sequence and a failure go to the ramoops log (`U` records: `panel: display off, sleep in in 141 ms (asleep 1, woken 0, packets 2)`, `panel: sleep out, vendor writes, display on in 137 ms ...`); the registry shows `PanelSleepMode` and `PanelAsleep`. Whether the MDP's autorefresh pauses while the panel sleeps (its TE stops) and whether exit sleep with these writes restores the picture are hardware questions; the option stays off until a device test.

Host test (sequences on a virtual 16 ms tick, the wake table pinned by CRC-32 to the DT, `dsi_panel_packet` against the DSI register model, including its equivalence with `dsi_panel_command` for 0x28/0x29/0x51): `xcrun clang++ -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -I <UniXNU-sm8750-common>/kits/Sm8750Platform Test/panel-power.cpp -o /tmp/panel-power && /tmp/panel-power`.
