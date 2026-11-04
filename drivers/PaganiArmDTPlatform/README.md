# ArmDTPlatform

ARM device-tree platform expert. Creates platform nubs from the ADT and matches drivers by compatible string.

Current state: used by the sheng port for its device tree.

Linked into XNU by `unixnu build kernel`.

## Boot arguments

All of them are off by default: without them the driver behaves as before.

| Argument | Effect |
| --- | --- |
| `pagani-halt=1` | `haltRestart` handles restart and power off. Restart, panic restart: BootProgress's reset (its records, watchdog handling and PSCI choice). Power off: PSCI SYSTEM_OFF on the same path. |
| `pagani-halt=2` | The same, but a power off restarts too: the phone comes back to Android and the DRAM log stays readable. |
| `pagani-rtc=1` | The calendar comes from the PMK8550 RTC, read only over the SPMI observer. `setGMTTimeOfDay` changes an offset in memory. |
| `pagani-rtc-offset=<seconds>` | With `pagani-rtc=1`: UTC minus the RTC's count, negative allowed. Measure it under Android: `date +%s` minus `/sys/class/rtc/rtc0/since_epoch`. Without it the first read starts at `/chosen/pagani-build-time`. |

`PlatformHalt.h` and `PlatformRtc.h` hold the facts and the logic, and say where they come from. The power off and restart path needs BootProgress's `ramoops_log_restart` and `ramoops_log_power_off`; without them `pagani-halt` falls back on a plain PSCI reset.

## Tests

`Test/check-platform-host.py`: the portable headers against a model of the SPMI arbiter and the RTC, with ASan and UBSan, and a list of deliberate defects the test must catch. It says nothing about the phone: not that the RTC answers, nor what the firmware does with SYSTEM_OFF.

`/chosen/pagani-build-time`, a u32 or u64 of Unix seconds, comes from the image's board configuration.
