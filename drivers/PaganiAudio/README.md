# PaganiAudio

The OnePlus 13T audio path for iOS: a kernel service with shared playback and capture rings, and an audio server plug-in
(`PaganiAudio.driver`) that iOS's audio server (audiomxd) loads from `/System/Library/Audio/Plug-Ins/HAL`. Ported from UniXNU-sheng
(revision 08cde20: `package/ShengAudioService.cpp`, `package/ShengAudioAbi.h`, the service part of `package/AudioProbe.cpp`,
`hal/ShengAudioPlugIn.c`, `scripts/build-sheng-audio-hal.py`, `patches/xnu/zzyd-sheng-audio-addon-mount.patch`), BSD-2-Clause-Patent.

The rings are carried by a backend. The silent backend runs now: it takes every playback period by the clock (four periods ahead of the
play position, as a DSP queue would) and clears it, and captures zeros. iOS therefore has an output and input device with a volume
control, which on sheng's iPad identity carried playback and the volume. A DSP backend (the ADSP/AudioReach path) replaces it through
`pagani_audio_set_backend()` with the same rings, timeline and plug-in.

Status: host checks pass. On the 13T (iPhone16,1 identity on the J120 product, 2026-10-31) the audio server loads the plug-in and the
HAL has the device and its volume control, but the routing layer's default virtual device has no active output port
(`audio route ... vcsp=0/0 vc=0 cat=csav mode=imdf out=0`), so SpringBoard refuses volume changes and shows no HUD. The probe now says
why in that case (`t0 t1` below). Since `pagani-haptics-ios=2` it is worse and looks better: with a `haptics` node the routing layer's
deferred initialisation dies on the J120 product and no virtual device exists at all (`audio route ... dev=0 vad=0 vcsp=0/560947818 vc=1
cat=.... ?h?t`, `'!obj'` read as a volume control): the HUD shows, the volume goes nowhere (`sets=0`), nothing plays, and no haptic client
has a device (`audio haptics plug vad=0`, `runs=0/0`). Bit 1 of `pagani-audio-srzl` is the answer (Limits). `scripts/sound-verdict.py RUN_DIR`
says from a run's log where the path stops.

## Boot arguments

| Argument | Effect |
| --- | --- |
| `pagani-audio=1` | Starts the service and shows the plug-in directory over the HAL directory. Absent or 0: nothing runs, maps, allocates or mounts. |
| `pagani-audio-hostclock=0` | Leaves CoreAudio's 24 MHz host clock constant alone (default: corrected to the 19.2 MHz timer, as on sheng). |
| `pagani-audio-trace=1` | The plug-in traces the routing layer's failing calls and throws (VirtualAudio import rebinding) into the control page. |
| `pagani-audio-probe=0` | Leaves out the route probe (default on): the plug-in's read-only look, from a queue of its own, at what the audio server makes of the device (below, `audio route`). |
| `pagani-audio-log=0/1/2` | Boot log rows: 0 start and failures only; 1 (default) rationed summaries; 2 every 10 s while anything moves (IO too) for the boot's first 16 rounds, then every 10 min while it still moves: fine for a diagnosis run. |
| `pagani-audio-mx=1` | The plug-in also reads, read only, how far MediaExperience's start got and whether its control queue answers (`audio mx p=... acq=...`, below). 21H461 only: the code at `MXInitialize` must match or nothing is read. |
| `pagani-audio-srzl=N` | Bits, each default off. Bit 0 (1): the plug-in puts MediaExperience's own serialization listeners on the routing layer once, when its one attempt is known to have been made and lost: `'srzl'` empty, MediaExperience past that call, the routing layer initialised, and both listeners' code byte for byte 21H461's. Bit 1 (2), with the actuator offered: before the routing layer's deferred initialisation it gives VirtualAudio the actuator settings its product table lacks for J120 (Limits): the image must be 21H461's (UUID) and MGGetProductType J120's, or nothing is written (`audio mx ... va=`). Bit 2 (4): PaganiHaptics', not this driver's. |
| `pagani-haptics-ios=1` or `2`, with `pagani-haptics=1` | The haptic actuator: memory type 3, the `Actuator` device and the bridge (`PaganiAudioHaptics.c`) to the vibrator. The plug-in lists the device at `Initialize` whether or not its ring maps (with the option, or without a service when MobileGestalt says DeviceSupportsHaptics); without the ring its IO is refused. |
| `pagani-audio-haptics-rate=N` | FIFO rate of the bridge: 8000 (default), 16000, 24000 or 48000. |
| `pagani-audio-haptics-sustain-mv=N` | Drive above this level (default 1260, the board's DIRECT_PLAY voltage) draws on an 80 ms credit that comes back at a quarter of the speed, then is clipped to it. `pagani-haptics-vmax-mv` stays the cap of everything. |

## How it fits together

1. Stage7Probe calls `pagani_audio_start()` after the display and the GPU (weak default there). The service allocates the control page
   and the rings (`PaganiAudioAbi.h`), anchors the timeline on the host clock (400 ticks per frame on the 19.2 MHz timer, computed as an
   exact ratio), registers the classes of `PaganiAudioService.cpp` late, starts its thread and publishes `PaganiAudio`
   (`NativeSandboxGrantProcess = audiomxd,mediaserverd`, `IOUserClientClass = PaganiAudioUserClient`).
2. `pagani-audio-addon-mount.patch` (device repository) mounts `/private/var/ngpu/audio` read-only as a union over
   `/System/Library/Audio/Plug-Ins/HAL` (scratch offset 0xe40; only with `pagani-audio`) and logs `overlay audio stage S status E`.
3. `scripts/add-pagani-audio-data.py` puts `PaganiAudio.driver` into a Data seed copy and its CDHash into a copy of the owner trust
   bundle.
4. audiomxd loads the plug-in. The device reports the Thunderbolt transport type (sheng's finding: iOS then makes it a driver device with
   one output and one input port). StartIO/StopIO become per-client requests; each user client releases only its own when it closes,
   and a request that still reaches a closed client (another thread of a dying process) is refused, so no direction stays counted.

The engine (`PaganiAudioEngine.c`, portable C) owns the timeline and never trusts the shared page: it keeps its own copies of every
constant and only counts what the plug-in writes back. It publishes `out_taken`, and the plug-in drops frames mixed behind it (a late IO
cycle would otherwise play a ring turn, 1.37 s, later). At a start it keeps what the plug-in already mixed ahead in the last 100 ms (the
audio server mixes as soon as StartIO returns) and clears the rest. Output stays up 5 s after the last stop. Late periods, holes,
stalls longer than the ring and the measured lead are handled as in sheng, with the periods a resync skips cleared.

## Build and check

```sh
python3 scripts/check-pagani-audio-host.py [--tree TREE] [--kern-exec TREE/.cache/xnu-work/bsd/kern/kern_exec.c --patch PATCH]
python3 scripts/build-pagani-audio.py --tree TREE              # component recipe: TREE/build/pagani-audio/libPaganiAudio.a
python3 scripts/build-pagani-audio-hal.py --out DIR             # PaganiAudio.driver, ad hoc signed (4 KiB pages), trust.json
python3 scripts/add-pagani-audio-data.py BASE.dbrdisk OUT.dbrdisk  # seed copy + OUT-owner-trust.bundle + OUT.json
python3 scripts/add-pagani-audio-data.py --plugin DIR --trust SEED-owner-trust.bundle --replace SEED.dbrdisk OUT.dbrdisk
                                                                 # a rebuilt plug-in into a seed that carries one (old CDHash dropped)
python3 scripts/make-audio-mount-patch.py --kern-exec KERN_EXEC_C --out PATCH
```

`Test/mutate-pagani-audio.py` runs the routing layer's repairs and the verdict's amplifier and tone rows as mutants (24, each must fail a host
check). `scripts/sound-verdict.py RUN_DIR` reads a run's log and says, segment by segment, where the path from iOS to the speaker and the
vibrator stops; for the kernel's tone (`pagani-audio-tone`) it goes as far as the amplifier. What it cannot say is whether air moved: that takes
an ear.

The host checks cover the ABI (`Test/abi.c`), the engine (`Test/engine.c`), the plug-in against the engine (`Test/plugin.c`) and the
kernel glue `PaganiAudio.cpp` / `PaganiAudioService.cpp` on the kernel stand-ins of `Test/kstubs` (`Test/glue.cpp`: nothing happens
without `pagani-audio`, every start failure is recorded and stops there, per-client requests and close, the service thread's round, the
boot log's rationing, the host clock correction's guards), all with AddressSanitizer and UndefinedBehaviorSanitizer. The barriers of the
route probe's seqlock, which no single-threaded test sees, are checked in the arm64 code (host build at -O2, the iOS bundle, the kernel
archive): the plug-in stores the odd seq, `dmb ish`, the fields, the even seq with `stlr`; the kernel reads seq with acquire, the fields,
`dmb ishld`, seq again.

The kernel archive is compiled with the tree's own XNU compile records, zero warnings, `-mgeneral-regs-only`; only
`PaganiAudioService.cpp` carries a static initializer, taken out of `__mod_init_func` and exported for the late registration. The kernel
links the archive through `CONFIG_XNU_EXTRA_LD_ARCHIVES="-Wl,-force_load,{tree}/build/pagani-audio/libPaganiAudio.a"`.

## Boot log (ramoops U rows)

- `overlay audio stage 5 status 0`: the HAL directory shows the system's plug-ins and ours (stage 1: the seed has no
  `/private/var/ngpu/audio`).
- `audio: up mode 1 backend silent frame 400/1 ticks ...`: the service runs; `audio: failed at ...` names the step that did not.
- `audio: open N by audiomxd pid P`: the plug-in was loaded and opened the service.
- `audio svc ...`: users, requests, output and input starts/stops, periods taken and played, late/holes, lead, peaks.
- `audio plug loads=... vol=... sets=... unk=... clk=19200000/0`: the plug-in's side; `sets` grows with each volume change.
- `audio io ...`, `audio unk ...`, `audio clock ...`: IO timing, unanswered properties, the host clock correction.
- `audio route #N rR dev=D va=V vad=A ours=O vold=X run=Y vcsp=C/S vc=H cat=.... mode=.... out=P t0 t1 ports=S vol=M q=Q`: the route
  probe (20, 60, 150 and 400 s after the plug-in loads, and 2 s after volume changes and IO starts, a few times each; a row only when
  it changed). What decides the volume HUD on 21H461: SpringBoard changes the volume only while MediaExperience says the current route
  has a volume control (`+[SBVolumeControl _isVolumeChangeAllowedForState:error:]`), and MediaExperience reads that from the routing
  layer's default virtual device, `vcsp` in the output scope (`vaemCurrentRouteHasVolumeControl`). `ours`/`vold`: this device as the
  HAL lists it and whether the HAL finds `{'vold', output, main}` on it, which it translates to this plug-in's volume control
  (`HALB_Info::TranslateDeviceAddressToControlAddress`); VirtualAudio's Thunderbolt port gets a volume control only then
  (`Device_Thunderbolt_Aspen.cpp`). `vad`, `vcsp=value/status`, `vol`: the default virtual device (UID `VirtualAudioDevice_Default`),
  its raw `vcsp` read and output volume x 1000. `vc`: that read as MediaExperience takes it, the answer SpringBoard's check gets:
  `vaemCurrentRouteHasVolumeControl` returns `(status | value) != 0`, so a failed read (`vcsp=0/S`, S not 0) counts as a volume
  control too and only a read that succeeded with 0 gives `vc=0`. `va`, `cat`, `mode`, `out`, `t0 t1`, `ports`: the routing layer's
  plug-in object, its category (`rctx`) and mode (`inpm`), the default device's active output ports (`aprc`) and the first two port
  types (`ptbo` is this device's output). `run`: this device running in any process. `q`: reads of the volume control (the HAL and the
  routing layer asking).
  With `out=0` (no active output port) `t0 t1` say why instead (VirtualAudio.plugin of 21H461): `t0` is the first output port the
  routing layer has connected (`prts`, `....` none) and `t1` four letters. 1: `S` MediaExperience installed its serialization listener
  on the routing layer (`srzl`; until then the routing layer's device manager takes only the UIDs Null_Device, Actuator, Halogen,
  Hawking, Flicker and Penrose and refuses route changes), `s` not, `?` unread. 2: `H` this device is hogged by the audio server
  (`oink`: the routing layer's Device_HAL_Common takes hog mode on every device it takes), `h` free (not taken), `x` another
  process, `?` unread. 3: the connected output ports, `0`-`9`, `+` more, `?` unread. 4: `T` one of them is a Thunderbolt output
  (this device's port), `t` none. So `s...`: MediaExperience never finished setting up the routing layer; `Sh0t`: the routing layer
  did not take this device; `SH1T` with `t0=ptbo`: taken and connected, but the routing left it out of the default route.
- `audio mx p=0x0fff filt=3 acq=2 srzl=0 st=0/0 after=- filt=0>0 n=0` (with `pagani-audio-mx` or `pagani-audio-srzl`): `p` the progress word
  (`PaganiAudioAbi.h`, `PaganiAudioMx*`: bits 0..9 MediaExperience's start in order, 7 entered FigVAEndpointManagerCreate, 8 the routing
  layer's id cached, 9 its `srzl` and `rwbu` calls returned; 10 the audio control queue set, 11 it ran an empty block in 2 s, 12-13 the
  device filter; 30 no MXInitialize, 31 not 21H461's code: nothing read), `acq` that block's time in ms (-1 none, -2 not asked), `srzl` the
  repair's state (0 nothing yet, 1 not needed, 2 waiting, 3 done, 4 failed, 5 refused: another build), `st` its two HAL results, `after` the
  listener read back, `filt=a>b` the device filter before and after (2 six UIDs, 3 every device), `n` attempts (at most one per boot). Bit 1
  of `p` (MXInitialize finished) counts libdispatch's generation form too: boots before this was fixed show `p=0x27fd` with
  MediaExperience finished (it answered AVAudioSession), not stuck. With bit 1 of `pagani-audio-srzl` the row ends in ` va=N`: 0 nothing yet,
  1 the settings given before the deferred initialisation, 2 given after its device manager appeared (it may have thrown already: read
  `audio route`), 3 they were there, 4 refused: another build, 5 refused: another product.
- `audio vaerr n=2 vain@1. T+0x232ac` (with `pagani-audio-trace=1`): the routing layer's failing calls and throws, offsets in VirtualAudio.
- `audio haptics runs=1/0 io=1/0 frames=N loud=N peak=in/out streams=begun/ended/rollovers fail=n/rc sent=taken/refused clip=N lat=last/most us`
  and `audio haptics plug loads=1 vad=N hog=1 writes=N frames=N peak=N ahead=a..b drop=f/w unk=n sel@obj fmt=N stale=N skip=N fifo=Hz cap=mV
  lim=N`: the bridge and the plug-in's side of the actuator. `lat` runs from the plug-in's write of a sound's first frame to the vibrator's
  stream begin; the motor starts about 10 ms after that (the vibrator's 8 ms of samples, its 2 ms poll and the start on the bus). `vad`
  VirtualAudio's device for the actuator (0 until its port exists), `hog=1` the audio server took the device, `lim` samples clipped to the
  sustained level.

## Limits

No sound until a DSP backend exists. Under the iPhone identity the default route stays empty (above); the `t0 t1` of the next
`audio route` row with `out=0` tell at which of the three steps. The host clock correction writes CoreAudio's constant on its shared page as sheng does, but only
at an address inside the arm64 shared region (0x180000000..0x300000000) and only on a page of managed RAM (`pmap_valid_address`;
`phystokv` would panic on anything else); turn it off with `pagani-audio-hostclock=0` if the audio server misbehaves.

The J120 product and a `haptics` node do not go together in VirtualAudio of 21H461, statically: `DeviceSupportsHaptics` adds the actuator's
alternate virtual device to the routing database (`sub_306FE0`, type 12), its settings are read from ActuatorSettingsFactory_Aspen
(`sub_308DDC` case 12, `sub_2AC470`), and the factory's product table (`sub_2AC4DC`) has no row for product 64 and throws
`std::logic_error("Invalid Product Type")` inside the RoutingManager's constructor; the ExceptionBarrier at the end of the deferred
initialisation (`sub_14CC58`) swallows it, the plug-in object stays not initialised for the whole boot, audiomxd does not crash. Nothing here
has seen the throw on the phone: `pagani-audio-trace=1` shows it (`audio vaerr ... T+0x2ac...`), and it fits the boots to the letter (and
the boots without the node, `vad=72`). Bit 1 of `pagani-audio-srzl` writes what the table's row for a product without I/V sense writes (kind
1, format `dflt`) and marks the factory's guard: the actuator's virtual device gets the ports {`pact`, `pnul`}, Device_Actuator_Aspen
finishes. What the routing layer does with `pact` on J120's database after that is not verified either: the route rows will say. Without the
bit, `pagani-haptics-ios` off is the other way to a working routing layer (no haptics then). MediaExperience takes the virtual devices' ids once,
after at most 9 s, and drops its device listener: a routing layer that publishes later leaves it with id 0 for the boot (`vc=1`, volume
`sets=0` with a route that plays).

The Thunderbolt port stays what it was: not muted by the silent mode (`ptbo` flags 0x616, route-re 4.3), its volume kept under LineOut
(0.3 at first). Ring, alarm and media categories all have `'pap '->'ptbo'` in J120's database (`sub_3BA97C`, `sub_3DB020`).
