# NativeMobileDisplay

Mobile display driver and GPU scanout hand-off. Validates swaps, retires frames
after scanout transfer and keeps the IOMobileFramebuffer path live.

Current state: the CPU and GPU scanout paths work. There is no display vblank interrupt; the refresh tick is a work-loop timer, locked
to the panel's TE through the DPU's read-pointer counter with `pagani-te=1`.

Linked into XNU by `unixnu build kernel` (`scripts/build-stage7-display.py` builds `NativeLegacyDisplay.late.o`,
`NativeGOPPresenter.late.o` and `DpuProbe.o` into `libNativeLegacyDisplay.late.a`).

## Scanout copy (CPU path)

The firmware framebuffer is mapped write-combined and each swap is copied with whole-row `memcpy`
(`native_legacy_copy_rows`); the kernel mappings of the surfaces CoreAnimation rotates through are cached
(`NativeLegacySourceCache.h`: released after 16 swaps without use, and all of them when the display turns off).
The copy runs on a kernel thread call outside the family's command gate and the swap retires before the copy, so the
compositor renders the next frame meanwhile (the next swap still waits for this copy). Rows the boot status overlay keeps
(`boot_progress_gpu_band`, BootProgress text rows 8-18) are left out of every copy and fill.

## GPU hand-off (`sheng-fb-gpu=1`)

Stage7Probe creates `NativeGOPPresenter` on the resource service; the display offers each frame to it
(`native_legacy_gpu_offer_frame`) and retires the swap when the GPU client takes it. The client keeps its mappings of
the compositor's surfaces across frames (six, `NativeGOPMappingCache.h`; the id is in `NGPSwap.reserved`). Wire additions
in `NativeGOPWire.h` (version 1 unchanged, identical copy in UniXNU `drivers/GopFramebuffer`): `NGP_METHOD_BAND` (4) and
`NGP_METHOD_WAIT_SWAP2` (5).

## Display processor (`DpuProbe.cpp`, `NativeDpu.h`)

Every access goes through one MDSS mapping behind a guard (`native_dpu_allowed`): the INTF_1 window (+0x000..+0x2ff, the part
Android's DSI driver maps), MDP_INTR_EN/STATUS/CLEAR, CTL 0 and the pipes CTL 0 names while it drives INTF_1; writes only to
TEAR_SYNC_CONFIG_VSYNC, TEAR_AUTOREFRESH_CONFIG, MDP_INTR_CLEAR, CTL 0's flush and those pipes' SRC0/SRC1. Nothing outside
`pagani-dpu-probe=3` reads the apps SMMU (the Gunyah hypervisor owns it). The old full snapshot hung the 13T 150 s after boot
(2026-10-30 12:15: log cut at 150.8 s, no panic, cold reset).

- `pagani-dpu-probe=1` (safe mode): 10 s after the display started, once a second INTF_1's FRAME_COUNT, TEAR_INT_COUNT_VAL and
  TEAR_AUTOREFRESH_CONFIG only; rows 14-16: `TE 120.04Hz FC 120/s AR 80000001 FREE RL 0` (TE rate over up to ten seconds, or the
  TE lock's; INTF_1 frames in the last second; autorefresh), `10S TE 119.98-120.06Hz FC 119-121/s` (per-second extremes) and
  `MODE 1 INTF_1 ONLY UR - REF 0 T 37s` (guard refusals with the first offset, seconds); a FLIP row while flipping.
- `pagani-dpu-probe=2`: also CTL 0, its pipes and MDP_INTR_STATUS, read only (what `pagani-fb-flip=1` touches); the old snapshot
  rows with `MODE 2 CTL0 INTF_1 P 010 UR 0 REF 0 T 37s` in place of the SMMU row. Try it before `pagani-fb-flip=1`.
- `pagani-dpu-probe=3`: the old full snapshot (MDSS top, all CTLs and pipes, the apps SMMU) 150 s after the display started,
  guard off. It hung the 13T; only to reproduce that.
- `pagani-dpu-probe=4`: no MDSS access; the GDSCR words of MDSS_CORE (0xaf09000) and MDSS_CORE_INT2 (0xaf0b000):
  `GDSC CORE 80000000 ON INT2 00000001 OFF` (`/HW`: hardware-controlled).
- While the display is off (`native_dpu_idle`) the probe reads nothing.
- `pagani-te=1`: the tick follows the panel's TE (100 ms lock, then line-count dating at every tick, 5 ms relock when the TE
  comes late); the reported refresh is the measured rate; a panel at 60 Hz is moved to its 120 Hz timing when the panel
  driver provides `pagani_panel_dcs_write`; with `pagani-te-ar-off=1` autorefresh is turned off with the screen and restored
  with it (default: it stays on, the panel is blanked by DCS 0x28). Only INTF_1's tear check is touched. On the 13T nothing of this
  has run: the lock comes at 1.1 s, before PaganiPanel can have turned `DISP_CC_MDSS_AHB_CLK` on (`pagani-panel` bit 8, about 1.5 s),
  so it finds no TE and the tick stays software (PaganiPanel README, "`pagani-te=1` with this path").
- `pagani-fb-flip=1` (with `sheng-fb-gpu=1`): three buffers in the splash region and DPU page flips after a first-flip trial;
  the GPU writes a buffer that is neither scanned nor waiting to latch; the CPU path flips back to buffer 0 first.

## Frame rate (`pagani-fps`)

Every frame that reached the panel is reported once through Stage7Probe's `native_legacy_frame_presented` to BootProgress's
frame-rate ring (`boot_progress_frame`, `BootProgress/FrameRate.h`: two atomic adds and a store, no lock), with its path and
the mach time it got there: `PAGANI_FRAME_CPU` when a CPU copy (or the blank fill) into the scanout finished, `PAGANI_FRAME_GPU`
when the GPU presenter's copy finished or, with page flips, when the flip latched (a frame replaced in the mailbox is not counted).
BootProgress shows `FPS 58 (31-60) | ft 17.2 p99 41.5 max 88.0ms | gpu 45 cpu 13` on text row 9 (frames in the last second and
their range over the last ten seconds, the last second's frame times: average, 99th percentile, largest, and its frames per path);
the row is part of the band the copy and the GPU client leave out. With the ramoops log the same line goes to the log once a
second while frames come (kind `F`), and every frame interval above 50 ms (below 1 s, which is idle) as a jitter record
(kind `J`, at most 4 at once, then one per 5 s). `pagani-fps=2` logs without the row; the default is 1 with the GPU status rows
(`pagani-gpu=1`) or `pagani-platform-status=1`.

### Where a slow frame's time went

Each frame is reported with its stages (`native_legacy_frame_timed`, `PeripheralStatus.h` `pagani_frame_timing`, mach ticks):
**arrival** — the first kick that sees the swap at the head of the queue, also while the previous frame is still on its way — and
the **gap** since the previous arrival (the compositor's cadence); **offer** — offered to the GPU client, or queued for the CPU
copy thread (the copy in the gate: its start); **taken** — the client's `waitSwap` returned it (`NativeGOPPresenter`), or the CPU
copy started; **copied** — the client's `completeSwap`, or the CPU copy ended; **presented** — `frameDone` (with page flips the
latch). BootProgress keeps each frame's breakdown beside its slot in the frame-rate ring (three words checked by generation, no
lock) and shows on text row 8, right above the FPS row and part of the band, the slowest frame interval of the last second with
the breakdown of the frame that ended it:

```
SLOW 88.0ms gap 70.1 off 0.2 take 0.3 copy 17.0 done 0.1 fb0 to0
```

`gap` the compositor's inter-arrival, `off` arrival to offer (waiting for the previous frame, a free flip buffer or the kick),
`take` offer to taken (the client's wake-up and mapping), `copy` taken to copied (the blit, queued behind rendering on the one GPU
ring), `done` copied to presented (the work loop, the latch; shown only while the row fits 65 characters); `fb` CPU fallbacks this
second (refused offers and failed GPU copies), `to` the GPU client's empty waits this second. Without a frame interval in the last
second the row says what the path waits for and since when: `SLOW - copy 2.3s fb0 to0` (`off`, `take`, `copy`, `done` as above,
`idle`: the compositor's next frame). The stage (`native_legacy_frame_stage`) is the in-flight frame's while one is on its way.
Display events (`native_legacy_display_event`): `PAGANI_DISPLAY_REFUSED` (an offer refused with `sheng-fb-gpu=1`), `_FAILED`
(a GPU copy failed: CPU fallback, or the frame lost after early retirement), `_EARLY_FAILED` (that after early retirement),
`_TIMEOUT` (an empty `waitSwap`); frames that went through the CPU copy are the FPS row's `cpu` count.

The log carries the same: each `J` record ends with the late frame's breakdown and flags
(`jitter 88.4ms gpu at 123456ms gap 70.1 off 0.2 take 0.3 copy 17.0 done 0.1[ refused][ gpu-failed][ bypass][ deferred][ flip] fb0 to0`),
the `F` line adds ` | <SLOW row>[ | ref N fail N early N]` while the slowest interval is above 50 ms, a frame waits in one stage
for over 50 ms, or an offer was refused or a GPU copy failed, and the 10 s heartbeat (`H`) ends with `cpu <MHz>MHz on <cpu>`, the
observer's CPU clock measured by about 1 ms of dependent adds (`BootProgress/CpuClock.h`), so throttling shows in the log.
With the row shown the peripheral overlay (`pagani-peripheral-status`) keeps rows 4-7.

## Boot arguments

- `pagani-fb-wc` (default 1): 0 maps the framebuffer Device-nGnRnE again and copies with aligned scalar stores
  (`native_legacy_copy_pixels`, or the differential copy with `sheng-fb-diff=1`). It restores only the mapping and
  the copy: the source cache stays on, and `sheng-fb-diff` is ignored while write-combined.
- `pagani-fb-async` (default 1): 0 copies inside the command gate again.
- `pagani-fb-scan` (default 0): number of first frames whose content is scanned for the diagnostics (32 matches the
  earlier behaviour).
- `pagani-vsync` (default 120): the tick's rate without a TE lock (the AA594 panel's default timing is 120 Hz); `sheng-vsync`
  as a rate (24 to 240) only switches the tick on, `sheng-vsync-fixed=1` still reports a fixed interval.
- `sheng-vsync=<us>` (4167 to 41667): the tick on, at that period to the microsecond, for a panel whose TE is no round rate: the
  13T's reads 8293-8305 us (`panel: p0 te rise`, a finer `mean` from `pagani-panel` bit 7 or 9), and a tick 34 us slower repeats a
  frame about every 2 s. The display keeps reporting the refresh class (120 for that period), as a TE lock does; the interval sent
  with each tick is the period. A `pagani-vsync-us` would cost 21 bytes of a full command line, this costs one.
  `sheng-vsync=120` is the way back. Without a TE lock the tick's phase is arbitrary and only stays put: a tear line that
  rolled every 2 s stands still somewhere, and the GPU copy is not moved into the gap between two frames.
- `sheng-early-retire` (default 1): retire an offered swap when the GPU client takes it.
- `pagani-dpu-probe`, `pagani-te`, `pagani-fb-flip` (default 0): see above.
- `pagani-dot-pitch` (default 460): what `getDotPitch` answers when the provider has no `dot-pitch` property (no board here has one):
  MobileGestalt's `main-screen-pitch`, which UIKit's keyboard Dock needs for its millimetres. Slot 189 divides the panel by it for
  `IOMobileFramebufferGetDisplayArea`, so `CADisplay.physicalSize` is no longer zero. 0 restores the old answer, `kIOReturnNotFound`.

A full rollback is a build of the previous revision, not a boot argument.

## Host tests

```sh
cd drivers/NativeMobileDisplay/Test
xcrun clang -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined request-state.c -o /tmp/request-state && /tmp/request-state
xcrun clang++ -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined source-cache.cpp -o /tmp/source-cache && /tmp/source-cache
xcrun clang++ -std=c++17 -O2 -Wall -Wextra -Werror -fsanitize=address,undefined display-timing.cpp -o /tmp/display-timing && /tmp/display-timing
python3 vsync-boot-arg.py
xcrun clang++ -std=c++17 -O2 -Wall -Wextra -Werror -fsanitize=address,undefined scanout-flip.cpp -o /tmp/scanout-flip && /tmp/scanout-flip
xcrun clang++ -std=c++17 -O2 -Wall -Wextra -Werror -fsanitize=address,undefined dpu-safety.cpp -o /tmp/dpu-safety && /tmp/dpu-safety
xcrun clang++ -std=c++17 -O2 -Wall -Wextra -Werror -fsanitize=address,undefined gop-handoff.cpp -o /tmp/gop-handoff && /tmp/gop-handoff
xcrun clang++ -std=c++17 -O2 -Wall -Wextra -Werror -fsanitize=address,undefined frame-rate.cpp -o /tmp/frame-rate && /tmp/frame-rate
xcrun clang++ -std=c++17 -O2 -Wall -Wextra -Werror -fsanitize=address,undefined frame-timing.cpp -o /tmp/frame-timing && /tmp/frame-timing
```

The BootProgress side (rows 8-9, F/J records, the heartbeat's CPU clock) is tested by `BootProgress/Test/console.c` and
`BootProgress/Test/ramoops.c`.
