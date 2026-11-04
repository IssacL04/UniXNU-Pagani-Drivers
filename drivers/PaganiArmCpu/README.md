# ArmCpu

ARM64 CPU, GIC and timer driver for UniXNU. Publishes the CPU platform device, the interrupt controller and the architected timer.

Current state: starts the CPU, GIC and timer on sheng and on the QEMU virt machine.

Linked into XNU by `unixnu build kernel`.

Shared peripheral interrupts for drivers (C exports): `armcpu_spi_attach` (Group 1, priority 0xA0, routed to the boot CPU,
enabled; the handler runs before the EOI and clears its level source), `armcpu_spi_count`, and `armcpu_spi_disable` /
`armcpu_spi_enable` (GICD_ICENABLER with RWP / GICD_ISENABLER) for a line the caller attached; the slot stays, so a storm
guard can switch its line off from the handler and a recovery on again. Host test of the gate: `Test/spi-gate.cpp`.

## CPU performance levels (`pagani-cpufreq`)

XNU has no CPU frequency driver here; under XNU the clusters stay at the level the firmware left (measured: about 1986 MHz on
every core, the 1996.8 MHz boot level). With `pagani-cpufreq` ArmCpu asks the CPUCP firmware for levels over SCMI PERF, as
Android's scmi-cpufreq does: the CPUCP mailbox (`qcom,cpucp-v2`: TX 0x17830000, doorbell at +0x104 for channel 0; RX
0x16430000 mapped but not touched) and the SCMI shared memory at 0x17b4e000 (0x200 bytes), completion polled (no interrupt).

- `pagani-cpufreq=0` or absent: nothing runs, nothing is mapped (as before).
- `pagani-cpufreq=1`: both domains (CPUs 0-5: 384-3532.8 MHz; CPUs 6-7: 1017.6-4320 MHz) to their top level once the levels
  are described; every 5 s PERF_LEVEL_GET checks them and asks again when the firmware reports another level (thermal limits:
  counted as `thr`, logged with PERF_LIMITS_GET). Measured on the 13T (2026-10-30, M8 desktop with ACD): the pinned clusters
  heat the SoC until the GPU sensors read 93-98 C with the GPU itself at 389 MHz, and the CPUCP firmware lowers the levels
  anyway (domain 0 to lv9-10, domain 1 mostly to lv13). For comparisons only.
- `pagani-cpufreq=2` (**the recommended setting**): governor, every 20 ms per domain on its busiest running CPU (XNU's
  per-processor idle time, `processor_info` PROCESSOR_CPU_LOAD_INFO, in 10 ms ticks: a single 20 ms window reads 0, 50 or
  100% busy). Utilization, as Linux schedutil maps it: the lowest level at or above 1.25 x the demand, the demand being the
  busiest CPU's busy time of the last 8 windows (160 ms) weighted by the kHz each window ran at (frequency invariant; each CPU
  is averaged on its own before the busiest is taken, so six CPUs at 55% do not read as 75%). Up fast on a CPU's saturated
  windows (at least 90% busy, not one idle tick): a burst (that CPU's 8 windows before averaged below 25% busy) at least
  doubles the level at its 1st and 2nd window and takes at least the touch level (from the floor: 2227.2 / 3513.6 MHz, then
  the top); any 3rd saturated window in a row takes the top (three in a row prove more than 83% busy). Down once 3 of the
  last 4 windows wanted a lower level, straight to the target (several levels at once), never below what the last 4 windows
  need; idle ends at the floor (`pagani-cpufreq-min=<MHz>`: the lowest level at or above it; default the 4th level, 960 / 1689.6
  MHz). Touch (PaganiInput's `pagani_input_touch_events`): at least the touch level, the level nearest 2.2 / 2.8 GHz (2227.2
  / 2841.6 MHz), until `pagani-cpufreq-touch-ms` after the last touch frame (default 250, at most 2000, 0 none); only the load
  rules go higher. What a tap sets off starts when the finger lifts (an app's launch, the keyboard daemon's answer), which is
  about where the 100 ms held before this ended; `pagani-cpufreq-touch-ms=100` gives that back. GPU frames: while
  `adreno830_cpu_hint()` (Adreno830's count of accepted GPU submissions; the weak default here returns 0) grew in at least 4
  of the last 5 windows (about 40 frames a second or more), at least the middle level (1785.6 / 2649.6 MHz). GPU heat: while
  `adreno830_thermal_state()` says the GPU's thermal loop holds the GPU below its top (Adreno830 ARCHITECTURE 4.16), domain 0
  stays at or below 2745.6 MHz and domain 1 at or below 3072 MHz, except for 100 ms after a touch frame whatever
  `pagani-cpufreq-touch-ms` says (in the GPU's 95 C emergency the caps hold through touches too), at once; the SoC heats as
  one, and the frames need the GPU more than the top CPU levels. The touch, GPU-frame, launch and cap levels never go under the
  floor. At start the governor logs its levels (`cpufreq: governor floor 960/1689 MHz, touch 2227/2841 MHz for 250 ms, GPU
  frames 1785/2649 MHz, heat caps 2745/3072 MHz, launch 2918/3513 MHz for 1500 ms`); heat transitions are logged (`cpufreq:
  GPU limited by heat at 85.3 C: d0 <= 2745 MHz, d1 <= 3072 MHz unless touched`, `cpufreq: GPU no longer limited by heat
  (74.8 C)`, `cpufreq: GPU heat emergency at 96.2 C: ...`). Simulated against the previous governor
  (above 60% the top at once, the top until 300 ms after a touch, one level down per 200 ms): light load with taps 1.6 / 2.3
  GHz on average instead of 3.4 / 4.1 GHz, a steady 1200 MHz load at 1.6 GHz instead of the top; a burst after quiet reaches
  the top one window later (40 ms instead of 20 ms).
- Launch boost (`pagani-cpufreq-launch-ms`, with `pagani-cpufreq=2`; default 1500, at most 5000, 0 off): both domains hold
  at least the launch level, the level nearest 2.9 / 3.5 GHz (2918.4 / 3513.6 MHz), for that long when an app starts. XNU
  schedules the eight CPUs as one set, so the launch's threads land on either domain, and work spread over CPUs that each
  look half busy reaches the demand average only over 160 ms. An app starts when an app or extension bundle is exec'd within
  1000 ms of a touch frame: the governor reads XNU's `pagani_app_execs` every window (`phone-app-exec-count.patch`: one more at
  every exec of a path with `.app/` or `.appex/`, one load). launchd is the parent of daemons and apps alike, so nothing cheaper
  tells them apart, and every process start used to count (`nextpidversion`): in run 20261031-223924 a daemon stood behind 18 of
  the 39 logged boosts, 11 of them the relaunches of a job exiting at once. The touch gate leaves out what launchd
  starts on its own: in the logs of 2026-10-30/31 about 2.5 groups of starts a minute over all, with runs that hold a user's own
  launches among them, up to 20 a minute in the short stretches the events ring kept. Any process start near a touch during
  a boost renews it (`nextpidversion`: the daemons a launch wakes), so a boost ends at most 2500 ms after the last touch frame.
  A daemon inside a bundle (`identityservicesd.app`) still counts; an app already running starts no process and gets only the touch hold.
  Drivers that know better can ask with `pagani_cpufreq_boost()` (`ArmCpu.cpp`; any context, it only counts; nothing calls
  it yet), which needs no touch. The boost is only a floor: the load rules go higher. The heat caps always hold over it, a
  touch's 100 ms included (it stops at 2745.6 / 3072 MHz while the GPU is limited by heat), and the platform's own limits stay
  as reported. No boost begins, and one under way ends, in the GPU's 95 C emergency, where a floor at the caps would keep the CPU
  hot when it should give way, and with the display off, which the governor reads from `pagani_screen_state` for this
  with or without `pagani-screenoff` (touches still come with the display off). Every level goes out through the same
  retried level sets as any other. A boost is logged when it begins, at most one line per 5 s (`cpufreq: launch 3: an app
  start 180 ms after a touch, 2918/3513 MHz for 1500 ms`, the count showing those left out; the time, in 20 ms windows, runs
  from the last touch frame to the exec that began the boost, so only boosts that began inside the gate have one),
  and counted on the row (`app`).
- Screen off (`pagani-screenoff` bit 0, with `pagani-cpufreq=2`; default off): the governor reads `pagani_screen_state`, the
  display's power as PaganiPanel publishes it from the display's power callback (bit 0: the system turned the display off;
  the count of changes above it). 1000 ms after the display goes off the floor drops to the lowest level and the domains are
  capped at 748.8 / 1209.6 MHz (the 3rd and 2nd levels), the caps at once; a touch frame, a key report (PaganiInput's
  `pagani_input_key_events`) or GPU frames lift both for 300 ms after the last one (the rules above then; a touch while dark
  gets no touch level, a key, typically the power key, gets it). The display coming back restores the floor in the next 20 ms
  window and holds the touch level for 500 ms. Logged once per period (`cpufreq: screen off 1.0 s: d0 384-748 MHz, d1
  1017-1209 MHz`, `cpufreq: screen on after 63.2 s off: lowered 61.9 s at 401/1017 MHz on average, 3 lifts`); at start
  `cpufreq: screen-off policy: ...`. Without the bit neither this policy nor the key counter is read; the launch boost alone
  looks at the display's state.
- Status: BootProgress row 14 (19 while the display probe rows are shown), redrawn when it changes:
  `CPU 3532/4320MHz lv15/15 ld 4/12% boost 3 thr 0 err 0 hot 1 app 2` (mode 2), `CPU max 3532/4320MHz lv15/15 ld 2/0% thr 0 err 0`
  (mode 1): MHz and level index per domain (a level the last check found lowered by the firmware with `!`, while the request is
  still above it; a level the governor raised since that check is no lowering), the busiest CPU's load per domain over the
  last second, touch boosts, lowered checks, `err` (refused PERF_LIMITS_GET plus the transient SCMI failures that were retried)
  and (mode 2, each when it fits) how often the GPU began to be limited by heat and, once there were any, the launch boosts.
  Off: `CPU scmi off: timeout lvl get x6` (what
  failed, in which message, and how many times in a row when a retry budget ran out); while a start waits for its retry:
  `CPU scmi retry: status -6 base ver`. Events go to the ramoops log (`U` records, `cpufreq: ...`).
- Reading a lowering line (`cpufreq: d0 at lv8 1996MHz, asked lv11 2745MHz, limits status -3, why heat cap, set 1840 ms ago, now
  hold`): it is only written when the level in effect is below the level asked for, so it stands for the firmware holding the
  domain there or, with a small `set`, for a level set not applied yet (the check waits for a window without a set, at most
  1000 ms; `set 0 ms ago` when it could not). `why` and `now` explain the number asked for and nothing else: the heat caps lower
  that number, never the level under it, so `why heat cap` on this line is the firmware going under our cap, and a cap of ours
  alone writes no line at all.
- Transient SCMI failures are retried, never a reason to stop at once (measured 2026-10-31, pkgD2 at 41.849 s: a single
  `status -6 lvl get`, SCMI BUSY, in a 5 s check switched the governor off for the boot and left the clusters at their boot
  levels, about 1997 / 1018 MHz). Transient: a BUSY answer (-6), and no usable answer (no answer within 30 ms, the channel
  still held from an earlier message, the channel error bit, COMMS_ERROR -7). After one, that call sends nothing more and no
  SCMI message goes out for a backoff: 20 ms (the next governor window), doubled per failure in a row up to 1280 ms; then
  what is still owed goes out again: a level set that failed is sent again (the domain's level counts as unknown until then,
  so it is sent even when the governor's level did not change), a 5 s check resumes with the domain it stopped at (the other
  is neither asked nor counted again), a start that failed probes again (the thread sleeps meanwhile; the row says `CPU scmi
  retry: ...`). No call sends more messages than before, so nothing waits longer than before. Budgets, each ending the feature
  like a final failure: 48 level sets failed in a row without one going through (about 54 s of BUSY), 48 start attempts in a
  row, or 6 missing answers in a row without any answer between them (a CPUCP that stopped answering: about 1 s). A 5 s check
  is informational: after 4 failed tries it is left out (the governor goes on, the next check 5 s later). PERF_LIMITS_GET
  BUSY or unanswered only leaves that lowering's line without the limits (`, limits busy`); a status refusing it stops these
  queries (counted in `err`). Logged with rationing: a line at the start and one at the end of each of the first 8 streaks
  (`cpufreq: retry: status -6 lvl get d1 after lvl set d1, next in 20 ms`, `cpufreq: retry ok: lvl get d1 after 1 failure in
  20 ms (busy 1, no answer 0)`), a line when a check is left out, then a summary at most once a minute (`cpufreq: retries
  since boot: busy 57, no answer 2, 30 streaks, longest 5, checks left out 1 ...`). `pagani-cpufreq-retry=0` (default 1)
  makes every failure final again, as before (the start line then ends with `, no retries`).
- Every other failure is final (any other SCMI status, a short or malformed answer, a value outside what was described): the
  feature goes off for the boot and shows why; with the governor both domains are first asked back to their boot levels (once
  each, also a domain whose level set was left owed); none of these panics. The RX mailbox window is mapped but never touched
  (completion is polled), so a failure to map it is only logged. The one thing software cannot catch: a window the
  hypervisor (Gunyah) does not map for the VM faults on its first access (logged just before it: `cpufreq: probing CPUCP SCMI
  ...`). `armcpu_cpufreq_state`: 0 not started, 1 probing (also while a start waits for its retry), 2 running, 3 off.

Code: `ScmiPerf.h` (transport, PERF messages, which failures are transient), `CpuFreqGovernor.h` (modes, governor, touch hold,
launch boost, GPU heat caps, retries and their budgets, row), the thread in `ArmCpu.cpp`. Host test with a fake CPUCP thread (both
level tables, timeouts, statuses, short answers, thermal lowering, the GPU heat caps, the governor's rules, the touch hold, the
launch boost with its gate and its end, the heat and screen-off caps over it and SCMI BUSY during it, the screen-off policy with
its lifts, the display coming back and random screen windows, scenarios on a workload model of eight CPUs with 10 ms idle ticks,
random windows against its invariants, extreme windows against a 128-bit reference of the demand; the retries: pkgD2's BUSY
check replayed, BUSY bursts on level sets with the exact backoff and no message during it, BUSY for good, intermittent late
answers, a stall, a CPUCP that stops answering, start retries, PERF_LIMITS_GET BUSY, mode 1, log rationing, checks left out,
random faults against the invariants): `Test/scmi-perf.cpp`,
`xcrun clang++ -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all
Test/scmi-perf.cpp -o /tmp/scmi-perf`.

## CPU idle (`pagani-idle`)

Without it an idle CPU only executes XNU's WFI: ArmCpu registers no `processor_idle` hook, so no core ever asks the platform
(Gunyah's PSCI 1.1, SMC) for a retention state, while Android on the same phone spends most idle time in one (`psci_idle` with
`qcom-cpu-lpm`: CPU0 108.7 s in "ret" against 61.2 s in WFI). With `pagani-idle` every CPU registers a hook that XNU's `cpu_idle`
calls with IRQ/FIQ masked right before its WFI; when the next timer (CNTV_CVAL_EL0) is far enough away the hook asks for a
retention state with PSCI `CPU_SUSPEND` (0xc4000001, entry point 0, as Linux does for these states). The call returns once an
interrupt is pending, and that interrupt also ends XNU's WFI at once. The states come from the PKX110 runtime DT; all are of
retention type and none stops the CPU's own timer:

- `pagani-idle=0` or absent: nothing (no hook, no PSCI call).
- `pagani-idle=1`: `c4` = "ret" `0x4` when the next timer is at least `pagani-idle-c4-us` away (default residency + exit latency:
  689 us on CPUs 0-5, 816 us on CPUs 6-7).
- `pagani-idle=2`: also `cl5` = "ret-pll-off" `0x1000054` (the cluster) from `pagani-idle-cl-us` (default 11127 us). A CPU only
  votes; in platform-coordinated mode (asked for with `SET_SUSPEND_MODE` when the firmware also offers OS-initiated mode) the
  cluster enters it once all its CPUs vote it.
- Never requested: "apps-pc" `0x200c354` (APSS power collapse: Android flushes the apps RSC sleep/wake sets before it, and its
  wake sources then go through the PDC).
- `pagani-idle-log=<s>`: period of the count lines (default 30, at most 3600; 0: only the start lines).
- History: after two idles in a row that another interrupt ended before their timer and in less than the c4 threshold (disk
  I/O, IPIs), a CPU stays in WFI until one of its idles lasts longer again, so such short waits do not pay c4's exit latency.

At start (boot CPU, before the secondary CPUs run; ramoops `U` records): the PSCI facts (`idle: psci 1.1 CPU_SUSPEND 0x2 (ext-id,
no osi) SET_SUSPEND_MODE -1 SYSTEM_SUSPEND -1 ...`), one self-test entry per state with interrupts masked and the timer brought
forward (2 ms for c4, 12 ms for cl5: `idle: self-test c4 rc 0 in 2.03ms (timer 2.00ms) ok`), then `idle: mode ...` and the hook
goes on. Every period: `idle 30.0s c4 2345x 2.91s cl5 12x 0.30s short 1234 hist 56 fail 0/0 late 0/0 cpu% 95 93 97 90 99 98 99
99 post 3us max 41us[ plat 4567/12][ off cl5 errors]`: entries and time per state summed over the CPUs, idles left to WFI (timer
too close; history), errors and late wakes since boot, each CPU's share of the period in CPU_SUSPEND, the time XNU takes from a
wake to the end of `cpu_idle` (mostly its idle-entry L1 flush, `platform_cache_idle_enter`), and the platform's own counts when
`PSCI_STAT_COUNT` is offered.

Safety: nothing panics. No `/psci` method, PSCI before 1.0, no CPU_SUSPEND, a self-test error or a wake more than 50 ms after the
self-test's timer keeps it off (`idle: off: ...`). At run time a state that returns an error 4 times in a row on one CPU, or wakes
more than 50 ms after its timer 3 times in a row, is switched off for the boot (`off cl5 errors`); c4 off ends PSCI idle and only
WFI remains. `armcpu-skip-gic=1` keeps it off. `armcpu_idle_state`: 0 not configured, 1 probing, 2 on, 3 off. Not yet run on
the 13T: a hang inside the self-test stops right after its log line, so test it with `pagani-wdog=<s>` (Gunyah watchdog).

Code: `PsciIdle.h` (states, choice, accounting, switches, log text), the hook, self-test and log thread in `ArmCpu.cpp`. Host
test against a fake PSCI (configuration and thresholds, CPU table, probe and SET_SUSPEND_MODE, self-test verdicts, the choice on
both clusters, accounting and the switches, the history, exact log text and every truncation, random sequences against the
invariants, eight CPU threads with a log reader, an idle model of the 13T's periodic wakeups): `Test/psci-idle.cpp`,
`xcrun clang++ -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all
Test/psci-idle.cpp -o /tmp/psci-idle` (also with `-fsanitize=thread`).
