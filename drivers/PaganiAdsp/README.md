# PaganiAdsp

The OnePlus 13T (PKX110, SM8750) audio DSP: ADSP/LPASS bring-up, GLINK and the IPC router, for kernel clients such as the battery
manager (PMIC GLINK) and audio (GPR/AudioReach). A port of the UniXNU-sheng ADSP core
([UniXNU-sheng](https://github.com/UniXNU/UniXNU-sheng) `08cde20e`, `package/AdspProbe.cpp` and the Qcom* cores, BSD-2-Clause-Patent;
each ported file names its source) to the 13T's SoC facts and its SM8750 platform layer.

Off by default. `pagani-adsp=1` starts one kernel thread from Stage7Probe (`pagani_adsp_start`, after the GPU start); without it nothing
is allocated, mapped or started, and every client call reports `PaganiAdspOff`. Nothing panics: every failure is a stage and a code in
the status and a `[adsp]` line in the BootProgress ramoops log (U records, `pagani-ramoops=1`).

## Bring-up

| Stage | Step | 13T facts |
| --- | --- | --- |
| 1 Mapped | SMEM partition table only; locate the apps↔ADSP (hosts 0/2) and global partitions | SMEM 0x81d00000+0x200000; the table is the last 4 KiB |
| 2 Firmware | `image/adsp_dtb.mdt`+`.bNN` and `image/adsp.mdt`+`.bNN` on modem_a, read in place and parsed | modem_a: LUN 4, block 4870, 89600 blocks, FAT (NativeUfs `native_ufs_part_find/read`) |
| 3 Surveyed | first access to the two partitions: items 443/429/478–480, slave-kernel bits, crash reason 423; AOSS descriptor, RSC id | refuses a DSP that already runs |
| 4 Smp2p | item 429 under TCSR mutex 3: `master-kernel`, `sleepstate` (awake, as Android) | mutex 0x1f43000 |
| 5 Doorbells | IPCC interrupt and the LPASS SMP2P (2) and GLINK (0) receive signals; the AOP signal stays off | IPCC 0x406000, SPI 229 level; polling if the interrupt is not available |
| 6 LoadState | AOSS QMP `{class: image, res: load_state, name: adsp, val: on}` | message RAM 0xc300000 |
| 7 Rails | lcx.lvl/lmx.lvl proxy votes, apps RSC DRV-2 | vlvl 384 (`cx-uV-uA`/`mx-uV-uA` of the 13T devicetree), `pagani-adsp-rail` |
| 8 Dtb | PAS 0x24: init_image, mem_setup, segments, auth_and_reset | carveout 0x9ed80000+0x80000 |
| 9 Loaded | PAS 1: init_image, mem_setup, segments | carveout 0x9ee00000+0x5080000 |
| 10 Started | PAS 1 auth_and_reset | |
| 11 Ready | SMP2P ready within `pagani-adsp-ready-ms` (5000); handover drops the proxy votes whenever it comes | |
| 12 Glink | items 478 (descriptor) and 479 (16 KiB FIFO), the DSP's 480, version agreed | |
| 13 Channel | a channel open | |

A failure from stage 6 on undoes what the DSP was given, in the Linux order: PAS shutdown of what runs, votes dropped (unless handed
over), `load_state ... val: off`, and the doorbells go quiet again (receive signals disabled, line off). What may still run keeps its
power: a shutdown that failed leaves the votes and `load_state` (`kept_votes=` in the `undo` line), and a secure call that ran out of its
20 s budget (`scm_parked=1`) stays parked: no further secure call goes after it. PAS metadata memory is freed only after a successful
authentication; otherwise it stays allocated.

No UFS write is on the wire while something is touched for the first time (a reset nothing sees coming is a power cut in the middle of a
write, and this flash can lose a whole logical unit to that, `UfsLog.h`): three stretches, the SMEM table (`[adsp] guard smem-table`), the
partitions, TCSR mutex and IPCC (`guard smem`), AOSS, the rails, PAS and the Q6 until ready (`guard lpass`). Each line gets its time to reach
the UFS log, then the writer is held by bit 6 (64) of `pagani_panel_probe_quiet` and a round under way gets a second to end; about 4.5 s each.
A stretch that hangs stays held and its line is the last one read back; a failure and a stop let go; without the UFS log (`sheng-log`) nothing
is held.

An interrupt that attaches but never rings cannot stall the link: ready found only by the wait's last look (no doorbell counted) switches
the thread to polling (`ipcc interrupt attached but silent, polling`), and any idle wait that runs out looks at everything once, so a lost
doorbell costs one idle period at most.

Run time: the thread serves SMP2P (feature negotiation, restart ack, handover, a fatal error report), GLINK and the IPC router. The router
says hello, announces the service locator (service 0x40, port 1) and answers the protection domains' queries from the SM8550/SM8750
table: `avs/audio` → `msm/adsp/audio_pd`, and the root, charger and sensor PDs (instance 74), CDSP (76), modem (180, `gps/gps_service`);
every domain provides `tms/servreg`. A protection domain's crash report to the locator (`SERVREG_LOC_PFR_REQ`, 0x24) is answered
with success, logged (`[adsp] pd crash report <service> reason=...`, the first eight) and named in the status note. A fatal error of
the running DSP reads the crash reason, closes every channel and refuses later sends; there is no restart.

## Boot arguments

| Argument | Meaning |
| --- | --- |
| `pagani-adsp=1` | enable (any other value: off) |
| `pagani-adsp-stop=N` | stop after stage N (1–7): 1–3 write nothing, 6–7 undo their step before stopping |
| `pagani-adsp-part=NAME` | firmware partition (default `modem_a`, then `modem_b`) |
| `pagani-adsp-ready-ms=MS` | ready deadline (default 5000) |
| `pagani-adsp-rail=VLVL` | proxy vote level (default 384; 0: the table's top) |

## Clients

`PaganiAdsp.h` is the interface: `pagani_adsp_channel()` / `pagani_adsp_channel_config()` register a GLINK channel by name (any time,
before or after the DSP is up, also after it opened the channel: the remote's OPEN waits for the registration),
`pagani_adsp_send()` queues a copy (never waits), callbacks report the channel state and each message on
the DSP thread. Android's intents: `PMIC_RTR_ADSP_APPS` 5 × 1 KiB, `adsp_apps` 20 × 512 bytes. A client that may be linked without
PaganiAdsp defines `PAGANI_ADSP_WEAK_DEFAULTS` in one file before including the header. `pagani_adsp_status()` returns the stage, the
error, the SMP2P bits, GLINK and IPC router counters and the crash note.

IPC router clients (QMI clients of a DSP service, e.g. PaganiSensors for the sensor client service 400): `pagani_adsp_qrtr_open()`
opens a port of our node (from 0x4000, not announced; with `announce_service` also our server of a service, announced after each
hello of the DSP) that follows the DSP's server of one service (any instance, or one); `pagani_adsp_qrtr_server()` reads that server
(level, with a generation that moves on every change), the `server` callback is the edge (also for DEL_SERVER, BYE and the link going
down); `pagani_adsp_qrtr_send()` queues one data message of at most 480 bytes (the IPCRTR channel's 512-byte messages) from any
thread, also from inside a callback; `pagani_adsp_qrtr_close()` sends DEL_CLIENT. Twelve ports at a time. Callbacks run on the DSP
thread with no PaganiAdsp lock held. `Test/qrtr_client.c` (`clients`) checks it against the real channel queue.

## Sharing with the A830 GPU driver

The SCM, AOSS QMP and RPMh calls go through the Adreno830 SM8750 platform layer (linked from `libAdreno830.a`, not compiled again).
DRV-2 is serialized by the existing `sm8750_rpmh_drv2_owner`. Two more weak lock words serialize SCM calls (`sm8750_scm_owner`) and
QMP messages (`sm8750_aoss_qmp_owner`, also PaganiWifi's); the GPU driver does not take them: all its SCM and QMP calls are in the
synchronous bring-up (`pagani_gpu_start`, stages S1 to S7), which ends before this thread starts, and its run time makes none (the ZAP
resume issues nothing through TZ by default). `pagani-gpu-async=1` would overlap them: never with this driver. The TZ wait-queue
interrupt stays the GPU driver's; PaganiAdsp polls its wait-queue wakes. GET_WQ_CTX gives a wake to one caller only, so a wake the GPU's
handler fetched must reach this driver's waiter: `build-userspace/a830-inputs/adsp-review/integrate/Sm8750Scm-shared-wakes.diff` makes the
two `Sm8750Scm` share them (until it is in, a WAITQ_SLEEP here runs into the 20 s budget and ends as `scm_parked=1`).

## Build and tests

`scripts/build-pagani-adsp.py` is the component recipe: the tree's XNU compile records, no warning accepted, ARM64 iOS 17.0 objects,
`build/pagani-adsp/libPaganiAdsp.a`; with `--append-to build/adreno830/libAdreno830.a` the objects also join that force-loaded
archive (the platform symbols are checked there, and no strong symbol twice).

`python3 Test/check-pagani-adsp-host.py [--gpt-dir DIR]` runs the host tests with ASan/UBSan: the ported cores (MDT, SMEM with the
access guard, SMP2P, GLINK), the IPC router and locator table, the firmware reader on FAT16 (4 KiB sectors) and FAT32 volumes made by
mtools, the PAS/QMP/RPMh adapters over the real Adreno830 platform sources, the whole bring-up and run time against a simulated SoC and DSP
(the SMEM pages the 13T cannot read are `PROT_NONE`; every failure stage and timeout), and NativeUfs's GPT lookup (with `--gpt-dir`,
also the phone's own GPT dumps). Host tests do not establish that the DSP starts on the phone.

## Limits

No subsystem restart, no wdog interrupt, no interconnect votes (Android votes `rproc_ddr`/`crypto_ddr`; sheng's SM8550 started
without them). An intent request from the DSP larger than a channel's intents is refused (fixed buffers; IPCRTR offers the devicetree's
largest, 0x4400 bytes). The apps↔ADSP SMEM partition has not been read on this phone yet: stage 3 logs `smem touch` before its first
access. SoCCP: whether the DSP needs it from the HLOS is not known (the devicetree's `pong` interrupt points at a processor the boot
chain starts); a failure would show as `FAIL st=-11` and is undone.
