# PaganiDiag

Diagnostics for the OnePlus 13T acceptance runs: which features of the iOS side work on this boot, read back from the BootProgress
ramoops log after the reset, the only channel out of iOS here. Off unless boot-arg `pagani-diag` is not 0.

Two parts: the kernel's own lines (`diag: k<s> ...`: the daemon's start, the process table, key and touch counters, the clock), which
need no user space, and a root daemon (`diag: r<round>...`) that asks the system's frameworks at 60, 180 and 600 s. With `migration=1`
the daemon also explains the system app migration (below).

Status: host tests pass (`Test/run.sh`, which also runs the daemon on the Mac), the kernel patch compiles with the build's own compile
record, the daemon is built and signed for iOS 17 and every import is in 21H461's shared cache. **Not yet run on the device.**

## How it starts

launchd of 21H461 takes every job below `/System/Library` from its signed cache `/System/Library/xpc/launchd.plist` on a normal boot, so
an overlay of `/System/Library/LaunchDaemons` adds no job and shadows none (adrenoprobed never ran there). The kernel patch starts the
daemon itself: the first exec of `/usr/libexec/DumpPanic` by xpcproxy or launchd runs `/private/var/factory_mount/paganidiag` instead,
when boot-arg `pagani-diag` is not 0 and the file exists. `com.apple.DumpPanic` is root, RunAtLoad, without MachServices, restarted only
after a crash; it does run on this board (pid 49 at 17.9 s in run 20261030-105430), and it only files the previous boot's panic report,
which this board does not keep. A later exec (the restart after a crash) runs DumpPanic itself.

That exec has two gates, and the first is why the 2026-10-31 A package logged no `diag:` line at all:

1. **The Sandbox platform policy.** `exec_check_permissions` calls `mac_vnode_check_exec` with xpcproxy as the caller. The platform
   policy's `process-exec*` of 21H461 refuses any program below `/private/var` unless the caller has `no-container` or `no-sandbox`
   (xpcproxy has neither) or the program is in a short list of trees, among them `/private/var/containers/Bundle`,
   `/private/var/personalized_*` and `/private/var/factory_mount`. The A package had the daemon in `/private/var/ngpu/bin`, so by this
   policy the exec failed and DumpPanic did not run either. That is read from the policy, not seen on the phone: the kernel's two lines
   about it, at about 18 s and 108 s, were long out of the events ring (the `kbt` lines of `pagani-kbtrace=3` left nothing before about
   250 s), and the old kernel had no later line. The daemon now lives in
   `/private/var/factory_mount`, which nothing in 21H461 refers to: not one reference in the shared cache or in `/usr/libexec`,
   `/usr/sbin`, `/sbin` (the `personalized_*` trees are MobileStorageMounter's). `Test/check-exec-policy.py` reads the policy out of the
   built kernel and walks it, old path and new, with xpcproxy's real entitlements; `process-exec-update-label` and
   `file-map-executable` allow the new path as well.
2. **Launch constraints.** launchd creates every job of its cache with launch type 1 (system service), and the original AMFI's
   system-service policy refuses an owner-trusted binary on the Data volume ("Launch Constraint Violation"). `sheng-no-lc=1` skips that
   check for every process of the system, which would make a diag run differ from a daily one in more than the daemon. Instead
   `pagani_diag_launch_constraints` passes the redirected exec's image alone, and the probes it starts (the same vnode, held by the
   running daemon). The image needs `pagani-diag=1` and nothing else.

Everything between the exec and the daemon's first line is now visible without user space: `act` and `sig` in the `k` lines give
`exec_activate_image`'s result and `process_signature`'s verdict.

Not gates, checked: `sysctl-write` has no rule in the platform policy (the operation's entry is the default allow node), so
`kern.pagani_diag_line` is not refused for a root daemon outside a sandbox; a refused write would fall back to the spool file anyway.
`/private/var/tmp` exists in the seeds and a failed lock no longer stops the daemon. All 191 imports of the daemon are in the shared cache
(`check-paganidiag-host.py imports`). The job's jetsam limit is 25 MB (J120), and the heavy frameworks load only in the probes.

## The kernel's lines

From a thread at the lowest priority (`MAXPRI_THROTTLE`), which also reads the spool file every 5 s and ends after 1800 s. Each is one U
record of at most 200 characters.

| Line | When | Meaning |
| --- | --- | --- |
| `diag: on (pagani-diag=N): daemon <path> in place of DumpPanic, checks at 60 180 600 s` | first exec | the boot-arg was read |
| `diag: launch pid N DumpPanic -> paganidiag` | about 18 s | the exec was redirected |
| `diag: launch pid N: daemon absent/unusable, DumpPanic runs` | about 18 s | the seed has no daemon: DumpPanic runs |
| `diag: launch pid N refused: exec error E` | about 18 s | `exec_activate_image` failed (1 EPERM: a MAC policy) |
| `diag: launch pid N refused: signature error E reason NS/0xC` | about 18 s | `process_signature` refused it |
| `diag: no line from pid N in 90 s` | 90 s later | the daemon wrote nothing |
| `diag: k<s> up U d pid P@T act A sig S now NAME cs 0xF ln L sp P dr D dp H key K tch T ax X/W vg C/R` | 60 180 600 900 1200 1500 1800 s | see below |
| `diag: k<s> ps name:pid ...` | 60 180 600 s, when it changed | the process table |
| `diag: k<s> time utc <now> boot <boot>` | 60 180 600 s | the calendar clock as the kernel has it |
| `diag: k<s> again r<N> sum ...` | 900 s and later | the daemon's last summary, so it survives until a late reset |
| `diag: kernel budget of this boot used, later lines dropped` | once | over the budget; later refusals only counted (`kern.pagani_diag_dropped`) |
| `diag: spool <path> refused: error E` | once | the spool file was not root's regular file |
| `diag: kernel watch done` | 1800 s | |

Status line: `d` is `wait` (no exec of DumpPanic yet), `nofile`, `err E`, `fail E` or `pid P@T` (redirected, at T s); then `act` the exec's
result and `sig` the signature verdict (`-`: not reached; with ` ns NS c 0xC` when refused), `now NAME cs 0xF` the process as it is now
(or `gone`), `ln` lines through the sysctl, `sp` through the spool (`eE`: why refused), `dr` refused lines, `dp` execs of DumpPanic, `key`
PaganiInput's accepted key reports (press and release of Power, Volume, and ring/silent changes: the evidence on the volume keys, however
the HID side treats them), `tch` its touch frames, `ax` execs of app extensions since boot / of those with `Widget` and no `Intent` in their
name below `PlugIns/` (`pagani_diag_extension`: 33 of the 35 WidgetKit extensions of `/Applications`, and not the six intents services named
after a widget, which only a widget's configuration runs), `vg` the file IDs asked of bindfs and those it
resolved (`phone-bindfs-vget.patch`; 0/0 without it).

`ps`: `wifid bluetoothd locationd CommCenter audiomxd mediaserverd thermalmonitord powerd backboardd kbd WidgetRenderer chronod PosterBoard
SpringBoard iconservicesagent securityd PerfPowerServices FindMy`, each `name:pid` or `name:-`; `*N` when N processes have the name, `/N`
when it was executed N times and that is not just once for the one live process (a restart, or a start that did not stay).

## The daemon's lines

At the rounds (`rounds=60,180,600`), at background priority (`PRIO_DARWIN_BG`, throttled I/O, the probes too). A round writes a summary
and the details; on the later rounds only the details that changed, so a steady phone costs one line a round.

| Line | Meaning |
| --- | --- |
| `start v2 pid N @S uid U argv0 /usr/libexec/DumpPanic cfg r3/44/5120 mig0[ bad KEYS]` | how it was started: rounds, budget, migration, unknown configuration keys |
| `r1 sum @61 mg 13/14 pwr 83 c0 hid 2/7 io 8/9 kc 0/0/0 aud Speaker sen 3/4 hap 1 net 2 data 1422/2047M wk 35/35` | everything in one line: the kernel keeps the last one |
| `r1@61 probe <name> timeout/sig N/exit N/spawn E` | a probe that did not end well; its answers show as `-` |
| `r1@61 wk n35 fs 25/10 reg 25/10 ok 25/10 ep 25/10 pf 2:35 oth 0 err 0 ls 35 pt 2` | the widget census, below |
| `r1@61 wkv cap 4001 fg 0/0 fid 1 chr 1 ext 3 wid 1` | the file IDs of `/Applications` and the live processes, below |
| `r1@61 kc add 0 get 0 del 0 match 1` | a test item through `SecItemAdd`, `SecItemCopyMatching`, `SecItemDelete`: the OSStatus of each (-25308 locked, -34018 no entitlement) and whether it read back |
| `r1@61 pwr n1 InternalBattery 83/100 chg0 Battery_Power src Battery_Power` | `IOPSCopyPowerSourcesInfo`: sources, the first one's type, capacity, charging, state; what supplies power |
| `r1@61 hid ff00/3:1 ff00/4:0 ff00/8:0 ff00/9:0 ff00/a:0 ff0c/3:0 b/2e:1 all 5` | `IOHIDEventService` by PrimaryUsagePage/PrimaryUsage (these decide MobileGestalt's sensor answers), all of them |
| `r1@61 io in:2 pan:1 aud:1 bat:0 sen:0 pwr:1 wl:0 aks:1 hid:5` | services by class: PaganiInput, PaganiPanel, PaganiAudio, PaganiBattery, PaganiSensorHid, IOPMPowerSource, IO80211Interface, AppleKeyStore, IOHIDEventService (-1: the lookup failed) |
| `r1@61 mg bat1 hap1 clh0 wifi1 bt1 acc1 gyr1 mag1 als1 prx1 mic1 fla1 cam1 gps1` | MobileGestalt: HasBattery, DeviceSupportsHaptics, DeviceSupportsClosedLoopHaptics, wifi, bluetooth, accelerometer, gyroscope, magnetometer, ambient-light-sensor, proximity-sensor, microphone, camera-flash, still-camera, gps; 1, 0 or `-` (no answer) |
| `r1@61 aud out Speaker in MicrophoneBuiltIn vol 0.50` | `AVAudioSession`: the port types of the current route, `none` when empty, and the output volume |
| `r1@61 sen acc1 gyr1 mag0 dm1 hap 1/1` | `CMMotionManager`: accelerometer, gyro, magnetometer, device motion available (no sensor is started); `CHHapticEngine capabilitiesForHardware`: haptics, audio (nothing plays) |
| `r1@61 net lo0+ en0-` | interfaces: `+` up, `-` down; `net N` of the summary counts those up other than lo0 |
| `r1@61 time 2026-10-31 19:02:03 +0800 tz Asia/Shanghai` | local time and zone; again only when the zone changed (the kernel logs the clock itself) |
| `end r3 p0 @601 log 0 sup 0 dup 0 sent 13 kd 0 cut 0[ spool]` | totals: rounds, passes, log lines, suppressed, repeats, sent, kernel refusals, details cut for the budget; `spool` when the sysctl was refused |

Every probe runs in a child (`paganidiag --probe <name>`), killed after 20 s, so a framework that hangs or crashes costs one answer.
Frameworks are loaded with `dlopen` in those children only. What the daemon needs no foreign code for (clock, Data volume, interfaces) it
does itself. The keychain probe is the only one that changes anything: one test item, added and deleted.

### The widget census (`wk`, `wkv`, probe `wid`)

One count for each link of what a widget needs, so a boot's log says where the chain breaks. It reads the app bundles of `/Applications`
(both layers of the union), takes those of their `PlugIns` whose extension point is `com.apple.widgetkit-extension` (35 on 21H461: 25 in
the system apps view, 10 on the root volume) and asks LaunchServices for each the way chronod does, with
`-[LSApplicationExtensionRecord initWithBundleIdentifier:requireValid:error:]`. The first number of a pair is the view (a bundle on a
bind mount), the second the root volume.

| Field | Meaning |
| --- | --- |
| `n` `fs` | the extensions on disk |
| `reg` | a record exists (`requireValid` NO) |
| `ok` | a valid record (`requireValid` YES): what chronod lists; a record it does not get is `Ignoring restricted extension` |
| `ep` | the record's extension point is WidgetKit's |
| `pf` | the records by platform, `<platform>:<count>` (2 iOS, 0 none: `_LSAddBundlePlugins` could not read the plug-in's executable) |
| `oth` | records whose URL is not this bundle: LaunchServices has another copy (`/System/Library/AppPlaceholders` holds placeholders with the same identifiers) |
| `err` | the most common error of the valid look-up, `<code>x<count>`; 0 none |
| `ls` `pt` | the records LaunchServices lists under the extension point, also outside `/Applications`; the extension point record's platform |
| `cap` | the two format capabilities of the volume of `/Applications`, hex: 4001 is `PATH_FROM_ID` and `PERSISTENTOBJECTIDS`, 0 is a bind mount without them |
| `fg` | `fsgetpath` of an extension of the view / of `/Applications` itself: 0 the same path, `x` another, `e45` ENOTSUP |
| `fid` | CoreFoundation's file ID (`_NSURLFileIDKey`) of that extension: 1 or 0 |
| `chr` `ext` `wid` | live processes at the round: chronod, app extensions, widget extensions by the same rule as `ax` (`ax` of the status line counts the execs) |

How to read it: `ok 25/10` and `pf 2:35`: registration is sound, look at chronod, `ax` and WidgetRenderer next. `ok 0/10` with `pf 0:25 2:10`
and `cap 0`, `fid 0`: the file ID chain (`phone-bindfs-vget.patch` missing or `pagani-bindfs-vget=0`). `cap 4001` and `fid 1` with `ok 0/10`:
another cause; `err` and `oth` narrow it. `fg e45` with `cap 4001`: the capability without `vfs_vget`, which must not happen.

The line is one or two of a round and comes back only when it changed; it needs `com.apple.private.coreservices.canmaplsdatabase`, as chronod
has it, to read the database itself. Replacing the daemon in a seed is `scripts/replace-diag-in-seed.py` as ever; the kernel's part (`chronod` in
the process table, `ax`, `vg`) comes with `phone-diag-sysctl.patch`.

## Budget

The 32 KiB events ring is shared with every driver: stages, status rows, crash traces, resets; a record costs 34 bytes beside its text.
The kernel's own lines are fixed: at most 5 lines a full round, 2 a later one, about 3.5 KiB a boot. For the daemon's lines the kernel
refuses a write over 48 lines or 5632 bytes a boot (`pagani_diag_take`, `pagani-diag=2`: 300 lines, 16 KiB) or over 20 lines in 10 s; the
daemon itself keeps to 44 lines and 5120 bytes (`budget_lines`, `budget_bytes`), leaves 0.55 s between lines and retries a refused line
twice after 2 s. `Rounds` keeps room for every later summary and the end line, so details are what is cut (`cut N` in the end line), in
this order of importance: probe trouble, widget census, keychain, power, HID services, IOKit services, MobileGestalt, audio, sensors,
interfaces, clock.
A steady boot costs about 3 KiB of the daemon's and 3.5 KiB of the kernel's, about 20 % of the ring. `Test/test_core.cpp` runs the worst
case (every line at its longest, every line changing each round) through the kernel's own limit.

## Configuration and turning it off

Off: no boot-arg `pagani-diag` (or 0). The kernel then creates no sysctl and no thread, and an exec costs one atomic load; the daemon in
the seed is inert, so the daily seed can carry it. Without the daemon in the seed the kernel's lines still come (`d nofile`). Other
values: `pagani-diag=1` the budget above, `pagani-diag=2` the long budget for the migration passes.

`/private/var/factory_mount/paganidiag.conf` (optional; copied from the build directory when it is there), `key=value` lines, `#` comments:
`rounds` (`60,180,600`, ascending seconds, at most 8, or `none`), `skip` (probes left out: `mg pwr hid kc aud sen hap wid`, comma separated, or
`none`), `budget_lines` (44), `budget_bytes` (5120), `gap_ms` (550), `migration` (0). Unknown keys and bad values are named in the start
line. A manual run (as root on the phone, or built for a Mac) prints instead of writing to the kernel:
`PAGANIDIAG_CONFIG=<file> paganidiag --print` (the variable is read only with `--print`).

## The migration passes (`migration=1`)

What this daemon was first written for: why the deletable system apps do not install. Off by default: they read the unified log every
30 s, which is not for a run that measures smoothness, and they need `pagani-diag=2`. Keys: `minutes` (10), `period` (30 s), `pass_lines`
(40), `boot_lines` (200), `notice_lines` (80), `info_lines` (40), `facts_every` (8), `log` (1), `logpref` (1). Every 30 s a line
`pN @S st <bytes>b n<count> <state>:<count>... lk n<count> <state>:<count>... mig M bun B stg G sen E lg <seen>/<kept>/<ms>ms[/np][ kd K]`
(`st` the state file `/private/var/installd/Library/MobileInstallation/BackedUpState/SystemAppInstallState.plist`: `-` absent, `unreadable`,
`bad`; `lk` installd's `MobileInstallationLookupSystemAppState`; `mig` `MobileInstallationGetSystemAppMigrationStatus`; `bun` entries of
`/private/var/containers/Bundle/Application`; `stg` `.app` entries in `/private/var/staged_system_apps`; `sen` the migration sentinel; `lg`
the previous log pass) and `pN not1 <state>:<id>,...` for the apps in a state other than 1; on passes 1, 9 and 17 the `mg fam ...`, `hw product
...`, `caps match=...` and `force arr ...` lines with what the migration's checks get; and the unified-log entries of
com.apple.migrationpluginwrapper (`mpw`), com.apple.datamigrator (`dm`), installd (`ind`) and installcoordinationd (`icd`), the
com.apple.migration subsystem and messages with `SystemApp`, as `<tag> <L>@<S> <message>`, shortened, once a boot, rationed by class
(`DiagCore.cpp`, `classify`). Info lines count because DataMigration logs every level as `OS_LOG_TYPE_INFO`; the daemon writes
`/private/var/preferences/Logging/Subsystems/com.apple.migration.plist` to keep them (`logpref made`). The log store: `+[OSLogStore
storeWithScope:error:]` of iOS 17 answers only the current process, so the probe wraps LoggingSupport's local store in an `OSLogStore`.

## Files

- `DiagCore.h`, `DiagCore.cpp`: what the lines say and how many (rounds, budget, change detection; filter, compaction, rationing, pacing);
  no device APIs.
- `PaganiDiag.mm`: the daemon: the probes, the schedule, the sender (sysctl, then the spool).
- `paganidiag.entitlements`: `com.apple.private.security.no-container`, `keychain-access-groups` (the test item),
  `com.apple.private.coreservices.canmaplsdatabase` (the widget census), and for the passes
  `com.apple.private.logging.admin`/`.diagnostic`/`.stream`, `com.apple.private.mobileinstall.allowedSPI`.
- `kernel/pagani_diag.h` (the lines and the limits, pure C), `kernel/kern_exec-diag.inc` (sysctls, spool, watch thread, start),
  `kernel/phone-diag-sysctl.patch` (made by `scripts/make-diag-patch.py` from the device series; after phone-keyboard-trace.patch).
- `scripts/build-paganidiag.py [--out DIR]`: build, sign (4 KiB pages), check.
- `scripts/make-diag-trust.py [--drop CDHASH]... BASE OUT DIR`: an owner trust bundle with the daemon's CDHash added.
- `scripts/replace-diag-in-seed.py --build DIR --trust BUNDLE SEED OUTPUT`: the daemon into a copy of a seed that exists already, the trust
  bundle merged beside it.
- `integration/*.diff`, for 工具/: `make-phone-seed.diff` (`--diag-daemon DIR`), `make-a830-layout.diff` (`A830_DIAG=<dir>`, or `=1` with a
  prebuilt seed; adds `pagani-diag=1`), `make-a830-final-seed.diff` (the `s4-diag` step).
- `Test/run.sh`: the core and kernel C tests with ASan and UBSan at -O0 and -O2, then `Test/check-paganidiag-host.py`: patch, build,
  imports, flow (the daemon run on the Mac), policy (`Test/check-exec-policy.py`), trust, replace, seed, layout.
