# PaganiSpeaker

The OnePlus 13T (PKX110, SM8750) speakers for XNU: two NXP TFA9873 amplifiers (I2C hub serial engine 4 at `0x990000`, `0x34` and
`0x35`, resets gpio2 and gpio164, supply L15B) fed by the secondary MI2S of the audio DSP, which plays an AudioReach graph built here
over PaganiAdsp's GLINK channel `adsp_apps`. It gives the kernel a test tone of its own (no iOS involved) and the PaganiAudio service an
`adsp` backend, so the iOS audio server's output reaches the speakers once its route picks the device.

Status: host checks pass; the kernel archive builds with the tree's XNU compile records, zero warnings; linked into the GPU line kernel
(Stage7Probe, component catalog, board); **never run on the device**. Default off (`pagani-speaker` absent: nothing is allocated, mapped
or started).

## Stages (`SpeakerCore.h`)

| Stage | Name | What happens | Needs |
| --- | --- | --- | --- |
| 1 | Pins | gpio72/73 on `i2chub0_se4` (2 mA, pull-up, strong I2C pull), the MI2S pins gpio121-124 in their sleep state | - |
| 2 | Rail | L15B (`ldob15`) 1.8 V through apps RSC DRV-2 under the DRV-2 lock word (logged, not fatal) | - |
| 3 | Bus | I2C hub AHB/core/SE4 votes in GCC `0x52008` through the kit's shared locked update, SE4 RCG on the XO, branches running, the GENI engine at 400 kHz (FIFO, polled) | - |
| 4 | Amps | both resets pulsed (1 ms), REV read: `0x..73` is a TFA9873 | - |
| 5 | AmpConfig | I2C reset, keys, the revision's table, interrupt enables, the container's 33 device fields (2 x 32-bit TDM slots, 0x34 slot 0, 0x35 slot 1, 48 kHz), the "speaker" profile, no I/V sense output; read back and verified; **powered down** | - |
| 6 | Dsp | `adsp_apps` registered with PaganiAdsp and opened by the DSP (up to `pagani-speaker-dsp-ms`) | `pagani-adsp=1` |
| 7 | Apm | the audio framework (APM) answers ready | the DSP's audio PD |
| 8 | Smmu | audio stream ids `0x1001` (required), `0x1081`, `0x1041`, `0x1061` in pass-through on the bank the storage stream (`0x60`) runs on: one S2CR and one SMR each, no context bank written (`SpeakerSmmu.h`) | - |
| 9 | Buffer | 64 KiB at `0xd0000000` (outside XNU's window) mapped write-combined for the period slots | - |
| 10 | Graphs | device graph (I2S sink, LPAIF secondary MI2S, SD line, 48 kHz x 2 x 32 bit), stream graph (WR_SHMEM -> PCM_DEC -> PCM_CNV), media format, MEM_MAP | - |
| 11 | Tone | the kernel's test tone (`pagani-audio-tone`) | - |
| 12 | Serving | the `adsp` backend handed to the PaganiAudio service | `pagani-audio=1` |

A playback run (the tone or the iOS ring): MI2S pins active, PRM bit clock (secondary MI2S IBIT, 3.072 MHz), PREPARE/START device then
stream, amplifiers powered up once the bit clock runs (PLL lock), four 10 ms periods in flight; stop in reverse. Every period passes the
**digital ceiling** (below) on its way into the DSP buffer.

Morning order: `build-userspace/a830-inputs/adsp-review.md` has the whole staged list. For this driver, one boot each, no ADSP needed for
the first two: `pagani-speaker=1 pagani-speaker-stop=4` (both REV lines), `stop=5` (config ok); then with `pagani-adsp=1`: `stop=8` (the
SMMU writes alone), `stop=10` (buffer and graphs), `pagani-audio-tone=1` (440 Hz, 500 ms, -30 dBFS: should be heard), `pagani-audio-tone=2`
(the walk: both, then 0x34 alone, then 0x35 alone: tells which amplifier is which speaker), then `pagani-audio=1` for iOS (needs its route).

## Boot arguments

| Argument | Meaning |
| --- | --- |
| `pagani-speaker=1` | on; absent (default) or any other value: off, nothing runs, maps or allocates (one log line when it or `pagani-audio-tone` is set to something else) |
| `pagani-speaker-stop` | stop after this stage (1..10, `0` all): `4` = amplifier identity only, `5` = amplifier configuration, neither touches the DSP |
| `pagani-audio-tone` | the kernel's test tone after stage 10, only with `pagani-speaker=1`: `1` one tone on both speakers; `2` the walk (both, then 0x34 alone, then 0x35 alone, 1 s apart); `0` (default) none |
| `pagani-audio-tone-hz` | tone frequency (default 440, 20..20000) |
| `pagani-audio-tone-ms` | tone length (default 500, 1..10000) |
| `pagani-audio-tone-atten` | tone level in dB below full scale (default 30, 0..90; the ceiling applies after it) |
| `pagani-speaker-ceiling` | the digital ceiling in dB below full scale (default 12; clamped to 3..40; it cannot be switched off) |
| `pagani-audio` | read only to decide whether to hand the `adsp` backend to the PaganiAudio service (its own argument) |
| `pagani-speaker-amps` | amplifier mask: bit 0 `0x34`, bit 1 `0x35` (default 3) |
| `pagani-speaker-sd` | MI2S data line: `1` SD0 / gpio122 (default), `2` SD1 / gpio124 (see Limits before trying it) |
| `pagani-speaker-bits` | slot width 32 (default, `backend_conf.xml`) or 16 |
| `pagani-speaker-prm-port` | GPR port PRM commands go from: `2` (default, sheng's finding) or `7` (downstream client) |
| `pagani-speaker-clock` | `1` (default) request the MI2S bit clock from the PRM around each run, `0` do not |
| `pagani-speaker-feedback` | `1` keep the container's current-sense output on (TDMCSE), `0` (default) no sense output |
| `pagani-speaker-i2c-pull` | `1` (default) the strong I2C pull-up on gpio72/73 |
| `pagani-speaker-rail` | `1` (default) vote L15B |
| `pagani-speaker-sids` | stream id mask (default `0xf`; bit 0, `0x1001`, always; `1` for a first run: only the stream the buffer addresses select) |
| `pagani-speaker-dsp-ms` | how long stage 6 waits for the DSP (default 120000) |
| `pagani-speaker-pa` | the period buffer's physical address (default `0xd0000000`; refused inside XNU's window or above 4 GiB) |

Command-line budget (`pagani-...` with its leading space): `pagani-speaker=1` 17, `pagani-speaker-stop=4` 22, `pagani-audio-tone=1` 20,
`pagani-adsp=1` 14.

## Safety

- **Digital ceiling** (`SpeakerLimit.c`): no SpeakerBoost runs (Android's speaker protection lives in its own DSP graph), so every
  period is limited before the DSP sees it: a stereo-linked peak limiter with one 1 ms block of look-ahead; unity up to the knee (3 dB
  below the ceiling), above it the output peak follows a line to the ceiling at full scale (about 11:1 at the default); the gain ramps
  down inside the block before a loud one (no clipping, no step) and recovers at about 24 dB/s. Below the knee the signal passes bit for
  bit, 1 ms later. Integer only; a final clamp counts any sample above the ceiling (none in any host check). Default ceiling -12 dBFS:
  it limits peaks, not power or excursion, and with no model watching the coil the level has to be safe for a held sine (`SpeakerLimit.h`
  has the arithmetic: 6 ohm speakers, about 0.6 W at -12 dBFS, about 2.5 W at -6 dBFS). Raise it only after listening, with music.
- **Shared GCC vote register** `0x52008` (touch, fuel gauge, speaker, PCIe): updated only through Sm8750Platform's
  `sm8750_gcc_vote_set` (`PeripheralIO.h`) under the lock word `sm8750_gcc_vote_owner` (tag `'SPKR'`), the protocol PaganiBattery
  already uses; a lock held elsewhere for 50 ms gives one counted unlocked update, never a hang. The touch's `sm8750_touch_clock` uses
  the same update.
- **Amplifiers**: configured and verified while powered down; powered up only after the DSP's bit clock runs; powered down at every
  stop, stall, DSP loss or refused start. No I/V sense output by default (nothing driven back onto the data lines).
- **The boot never waits**: `pagani_speaker_start` only starts a thread; every wait is bounded (I2C 3 x 10 ms per transfer, branch
  polls 20 ms, DSP 120 s, commands 2 s, the hand-over 30 s); nothing panics; every failure is one `[spk]` line.
- **No UFS write under a first touch** (a reset nothing sees coming is a power cut in the middle of a write, and this flash can lose a whole
  logical unit to that, `UfsLog.h`): three stretches, `[spk] guard i2c` (stages 1..5), `guard smmu` (8..10, the step that may reset the
  board), `guard tone` (the first tone's bit clock and DMA). Each line gets its time to reach the UFS log, then the writer is held by bit 5
  (32) of `pagani_panel_probe_quiet` and a round under way gets a second to end; about 4.5 s each. A stretch that hangs stays held and its line
  is the last one read back; a failure and a stop let go; without the UFS log (`sheng-log`) nothing is held. iOS's first run gets none: it
  runs on the audio service's thread, and the tone rounds have been through that path first.

## Boot log (ramoops U rows)

- `[spk] start amps=0x3 bits=32 sd=sd0 prm_port=2 clock=1 feedback=0 rail=1 sids=0xf pa=0xd0000000 stop=N ceil_db=-12 ceil=8231`
- `[spk] guard i2c`, `[spk] guard smmu`, `[spk] guard tone`: the stretches above
- `[spk] pins sda=.. scl=.. sck=.. d0=.. ws=.. d1=..`, `[spk] rail ldob15 mv=1800 rc=.. detail=..`
- `[spk] bus clk=0 vote=0x.. lock=1|0|- waits=N ahb=.. core=.. s4=.. rcg=.. rcg_fixed=.. fw=.. if=.. clkcfg=.. scl=.. set=.. abort=.. rc=0`
  (`lock=1` written under the lock word, `0` written without it after 50 ms, `-` nothing to write)
- `[spk] amp 0x34 rev=0x..73 type=tfa9873 table=N` (stage 4), `[spk] amp 0x34 config ok transfers=N bits=32 slot=0 tdm=.. en=.. slots=..`
- `[spk] dsp up waited_ms=N`, `[spk] apm state=ready ...`, `[spk] smmu 0x1001=ok/..`, `[spk] buffer pa=.. dsp=.. bytes=65536`,
  `[spk] graphs handle=.. bits=32 sd=sd0 prm_port=2 clock=3072000`
- `[spk] play start run=N amps=0x3 ...`, `[spk] amp 0x34 state=9 clks=1 plls=1 sws=1 ampen=1 noclk=0 tdmerr=0 ...` (status 100 ms into a run: the
  manager operating, clocks and PLL, the output stage switching, the amplifier enabled, no clock loss or TDM error since the last read),
  `[spk] play stop run=N sent=.. played=.. ... lim=<blocks limited> gmin=<lowest gain, per mille> in=<peak in> out=<peak out> clamp=0`
- `[spk] tone walk: ...`, `[spk] tone rc=0 hz=440 ms=500 atten=30 gain=1036 periods=50 sent=.. played=.. amps=0x3 clks_failed=0 ch=0x3 out=.. ceil=8231`
- `[spk] backend adsp handed to the PaganiAudio service (st=12)`, or why not
- failures: `[spk] FAIL st=-N err=E <what>` (with `op=`/`dsp=` from stage 6 on), `[spk] stopped as asked (pagani-speaker-stop) st=N`

## Build and tests

```sh
python3 Test/check-pagani-speaker-host.py [--tree TREE]
python3 scripts/build-pagani-speaker.py --tree TREE [--out DIR] [--append-to TREE/build/adreno830/libAdreno830.a]
```

The kernel archive uses the tree's XNU compile records (zero warnings, `-mgeneral-regs-only`, no static initializer) and carries weak
defaults of PaganiAdsp's and PaganiAudio's entry points and the shared lock words; every strong symbol is checked against the other
archives of the tree. Headers: `../PaganiAdsp`, `../PaganiAudio`, `../PaganiInput` (`Drv2Lock.h`) and the Sm8750Platform kit
(`PeripheralIO.h`, `Rpmh.h`). Stage7Probe calls `pagani_speaker_start()` after `pagani_audio_start()`.

Host checks (`Test/`): `tone` (SpeakerTone against libm), `protocol` (GPR and every AudioReach/PRM field), `amp` (GENI and TFA9873
programs against register models), `hw` (pins, clocks with the lock word, engine, transfers), `smmu` (the pass-through plan against the
13T's tables: two writes on success, none on any refusal, no context bank, TLB or global register touched), `limit` (the ceiling against libm: curve
at every ceiling, random signals of every level, delay, attack, release, linking), `gccvote` (the vote register under the touch, this
driver, the gauge's and PCIe's lock protocols on thousands of interleavings: nothing lost with the lock, votes lost without it), `core`
(the whole path over a DSP model and PaganiAudio's real engine: stages, tone, walk, iOS ring, full-scale input, every failure path),
`args`. Host checks do not establish that the amplifiers answer, that the DSP takes the graphs or that sound comes out.

## Limits

- Never run on the device; the ADSP (PaganiAdsp) has never run on the 13T either.
- No speaker protection model (SpeakerBoost), no calibration (Re/F0), no OPLUS tuning (EQ, MBDRC): only the fixed container values and
  the digital ceiling. The container `/odm/firmware/tfa98xx.cnt` (5699 bytes, sha256 `8574cf32...0a4e`) was read on 2026-10-30; its
  device fields are written out in `Tfa9873.c`, the file itself is not kept.
- The data line is an inference: SD0 (gpio122) carries the samples; SD1 (gpio124, pulled down in its active state) is most likely the
  amplifiers' sense output towards the SoC. `pagani-speaker-sd=2` drives SD1 from the SoC: only try it with the sense output off (the
  default) and if SD0 stays silent with every other stage fine.
- Which amplifier is the top (earpiece) speaker is an inference (0x34, `tfa_channel 0`): the tone walk settles it by ear.
- The period buffer at `0xd0000000` is HLOS memory per the Android devicetree, `EfiConventionalMemory` in the 13T's UEFI map (row
  `DXE_Heap`, `SpeakerBoard.h`) and outside XNU's window; that nothing of the UEFI boot stays in use there is an inference, not verified
  on the device.
- The audio streams' pass-through is sheng's sequence (shared bypass bank), proven on SM8550, not yet on the 13T: sheng's board reset on
  every translating context for these streams, so stage 8 gets a run of its own (`pagani-speaker-stop=8`).
- No microphone (WCD939x on the VA SoundWire: a separate design, `build-userspace/a830-inputs/speaker/mic-design.md`), no earpiece
  routing, no idle power-down of the graphs (the PaganiAudio service stops output 5 s after the last client).
- About 60-80 ms output latency (four 10 ms periods in flight plus the DSP's lead plus the 1 ms look-ahead).

## Sources (facts only, no code copied)

UniXNU-sheng (BSD-2-Clause-Patent, commit `08cde20e`): the structure of `package/AudioProbe.cpp`, `GeniI2c`, `ShengGpr`,
`ShengAudioReach`, `ShengCs35l43`, `SmmuBypass.h` (ported, see each file's header). Register and protocol facts: upstream Linux `gcc-sm8750.c`,
`pinctrl-sm8750.c`, `i2c-qcom-geni.c`, `sound/soc/qcom/qdsp6/audioreach.h`; the 13T's GPL audio-kernel (`audio_prm.h`, `msm_common.c`,
`sun.c`) and NXP's tfa98xx v6.14.2 (field codes and start sequence) as facts only; the phone's running devicetree, `backend_conf.xml`
and `tfa98xx.cnt`. Study: `build-userspace/a830-inputs/haptics/sound/FEASIBILITY.md`.
