# PaganiSensors

The OnePlus 13T (PKX110, SM8750) motion and environment sensors for iOS 17.7 (21H461). Every sensor hangs off the ADSP's sensor protection
domain (`msm/adsp/sensor_pd`, Qualcomm's sensor hub): nothing is reachable from the application processor. PaganiSensors is three things:

- a client of the hub's sensor client service (QMI service 400 over PaganiAdsp's IPC router): SUID lookup, attributes, streams;
- the iOS side: one `IOHIDDevice` per sensor with the original `IOHIDEventDriver` on it, events built with the original constructors;
- the FastRPC reverse file service the sensor PD needs before it has any sensor to offer (on Android `/vendor/bin/sensors.qti`).

Default off. Nothing here has met the hardware yet: all of it is host-tested only.

## Boot arguments

`pagani-sensors=N`, a mode plus flags, read once by `pagani_sensors_start`:

| N | what runs |
| --- | --- |
| absent, 0 | nothing: no allocation, no registration, no traffic |
| 1 | the iOS services only, no sample ever, no DSP traffic |
| 2 | also the hub's SUID lookups and attributes (needs `pagani-adsp=1`), no stream; lists every data type |
| 3 | also the streams iOS asks for, and MobileGestalt's `accel` and `gyro` nodes |
| +4 | the FastRPC file service (needs `pagani-adsp=1`): 5 alone, 6 and 7 with the hub |
| +8 | with mode 3: the orientation service and its plug-in's user client: 11, or 15 with the file service |

Others: `pagani-sensors-axes=a+x+y+z,g+x+y+z,m+x+y+z` (axis maps, default identity), `pagani-sensors-als=TYPE`, `-prox=TYPE`, `-accel`, `-gyro`,
`-mag`, `-pressure` (data types; a type the hub does not have turns a sensor off), `pagani-sensors-als-scale=N` (permille), `pagani-sensors-motion-hz=N`
(stream without an iOS request), `pagani-sensors-prox-on=1`, `pagani-sensors-list=0/1`, `pagani-sensors-log=0/1/2`, `pagani-sensors-nodes=0`,
`pagani-sensors-rpc-pa=ADDR`, `pagani-sensors-rpc-bank=N` (3..8, default 4).

## What iOS gets

| kind | usage pair | event | consumer |
| --- | --- | --- | --- |
| accelerometer | `0xff00/0x03` | 13, g, iOS's sign (face up z = -1) | backboardd, CoreMotion, locationd |
| gyroscope | `0xff00/0x09` | 20, rad/s | locationd |
| magnetometer | `0xff00/0x0a` | 21, uT | locationd |
| ambient light | `0xff00/0x04` | 12, integer lux | CoreBrightness (inside backboardd) |
| proximity | `0xff00/0x08`, `Placement=1` | 14, mask `0x40` near | backboardd |
| pressure | `0x0020/0x31`, only if the hub has one | 31, kPa | locationd |
| orientation | `0x0020/0x8a`, default off | 10, usage `0x64 + state` | CoreMotion in backboardd and locationd |

The identity decides a lot: CoreMotion picks its machine table by `product-id`, which stays J120 (type 62). So the orientation does not come from the
accelerometer on the application processor but from an always-on processor over `{0x20,0x8a}`.

**The orientation trap.** The moment that service exists, `CLOrientationNotifier` opens the AppleSPULib plug-in on the service's grandparent and sends a
configuration without checking that the open worked: backboardd dereferences null at every boot. So the service is never published alone. Its device
object carries, before it starts, `IOCFPlugInTypes`, the user client of type `'ashd'` (selectors 4 and 6 succeed) and `NativeSandboxGrantAll` (the XNU patch
`zzwc-native-adreno-sandbox-grant-all`: backboardd and locationd are sandboxed and their profiles name Apple's class). The state comes from
`SensorsOrient.c`; the original driver makes the events from one-bit input reports.

**MobileGestalt.** `accelerometer` and `gyroscope` are nodes named `accel` and `gyro` anywhere in IODeviceTree. They are created inside
`pagani_sensors_start`, before launchd exists, only with mode 3, only for services that were published. Never in the board's static device tree: a daily
image would then claim the sensors with no data source. `ambient-light-sensor` is the light service itself, which is why the services are registered at
boot whatever the hub does.

Proximity: each `DetectionMode` write gets the current state again (backboardd expects it; the hub only reports changes and new requests).

## The file service

The sensor PD reads `sns_reg_config`, the JSON configuration, the persist registry with this phone's calibration and `/sys/devices/soc0` from the
application processor. `SensorsRpc.c` speaks FastRPC on the GLINK channel `fastrpcglink-apps-dsp`: attach to the sensors PD, open and register
`adsp_default_listener`, `init2`, then `next2` in a loop serving the DSP's calls: `apps_remotectl`, a read-only `apps_std`, `apps_mem` for heap pages.

- Files: one pack, `/private/var/ngpu/sensors/pack.bin` on the RAM Data volume, made on the host from the Android dump by `scripts/make-sensor-pack.py`
  and put in the seed with `make-phone-seed.py --sensor-pack`. With no pack the service runs anyway: every open fails and the paths the DSP asks for are
  logged, which is how the first run says what the pack must hold. Writes are refused (the pack holds the registry Android generated).
- Memory: an 8 MiB arena at `0xd0100000`, conventional memory below 4 GiB outside XNU's window, write-combined. The DSP takes 32-bit addresses.
- Streams: `0x1004` (required), `0x1084`, `0x1044`, `0x1064` in pass-through through the SoC kit's planner, just before the first attach. The context bank
  (4, `compute-cb@2` of the devicetree) goes in bits 39:32 of every address.

## Boot log

`[sensors] ...` U records (`pagani-ramoops=1`). In order, on a good run: `start mode=.. rpc=.. orient=..`, `services published=..`, `nodes accel=0 gyro=0`,
`hid <kind> ... bound`, `ports ...`, `service up ...`, `rpc channel up`, `rpc pack entries=..`, `rpc smmu sid=0x1004 rc=0 ...`, `rpc attached to the sensors PD`,
`rpc listening`, `rpc first call of the DSP`, `rpc dsp opens apps_std`, `rpc open <path> bytes=..`, `rpc missing <path>`, `rpc quiet calls=.. opens=.. missing=..`,
`suid <kind> ...`, `attr <kind> name=.. vendor=..`, `discovery done found=.. missing=..`, `stream <kind> on ...`, `first <kind> ...`, `spu selector=4`,
`spu selector=6 command=0x20`, `orientation state=..`. Failures are `FAIL st=-N err=..`. Lines are rationed per kind: the event ring is 32 KiB and shared.

## Limits

- Under-display light: the hub's algorithm wants the display state from the application processor (`/dev/ssc_interactive` on Android) and nothing sends it
  yet; the message is unknown until the Android HAL is captured. Until then readings can follow the screen's own light. `pagani-sensors-als=-` turns the
  sensor off, the service stays.
- `CMMotionManager.deviceMotion`, compass heading: the J120 table wants the always-on processor's fusion reports (`{0xff0c,1}`). Not built.
- Pedometer, lift to wake: not in the J120 table. No service is faked.
- The file service has never met a DSP. `build-userspace/a830-inputs/sensors/build-report.md` has the staged order and what each failure means.

## Files

`SensorsPb.c` protobuf wire format, `SensorsSsc.c` QMI and the hub's messages, `SensorsFixed.c` integer float conversions and axis maps, `SensorsCore.c` the
manager, `SensorsHid.c` what iOS reads, `SensorsOrient.c` the orientation, `SensorsPack.c` the file pack, `SensorsRpc.c` the file service: portable C, no
floating point, no C library. `PaganiSensors.cpp`, `PaganiSensorsHid.cpp`, `PaganiSensorsRpc.cpp`: the kernel glue.

## Build and test

`scripts/build-pagani-sensors.py --kernelcache KC [--tree TREE] [--out DIR]`: the kernel recipe; qualifies the constructors, the driver table and the class's
vtable against the pinned 21H461 image, zero warnings; `--out` keeps the tree as it is.

`Test/check-pagani-sensors-host.py` (`--only wire|fixed|orient|core|hid|rpc|stack|qrtr|weak|adsp`, `--kernel TREE --kernelcache KC`): ASan and UBSan, a simulated
hub, a simulated DSP written from the public layouts on its own, PaganiAdsp's real router ports. Host tests do not establish that the hardware answers.

Facts come from Qualcomm's BSD-3 sources (`qualcomm/sensinghub`, `qualcomm/fastrpc`); Linux and hexagonrpc were read for facts only, no code taken.
