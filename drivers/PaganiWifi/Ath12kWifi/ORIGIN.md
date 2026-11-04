# Origin

- Source: https://github.com/UniXNU/Ath12kWifi, commit `f6bf561f92cc42761bdd417fce1d53fe1db9477c` (the revision UniXNU's
  `components.toml` pins and UniXNU-sheng `08cde20e` uses), copied without `.git`, `deps/` and `out/` on 2026-10-31.
- Upstream ships no license file in this repository; the copied files keep upstream's terms. The files added here
  (`tools/peach.py`, `tools/peach-sim.sh`, the `nw_board_option` and `nw_peach_step` defaults in `src/nw_wifi.c`, `nw_mhi_reload` in
  `src/nw_mhi.c`, `lx_flush_work` in the runtime, the peach cases of `tests/stack_sim.c`, the prepare.sh hook and this file) are
  BSD-2-Clause-Patent (PaganiWifi, UniXNU contributors).
- Changed from upstream, for what wifid of 21H461 expects (PaganiWifi README.md, Settings): `src/nw_sta.c` lets a join from a joined network through, a leave
  answers with `link_lost` or the join's result, a deauthentication between messages 1 and 3 is a key error, the status follows the joined network's
  beacons; `tests/sta_sim.c` covers these, and its data path caught up with the hardware's encapsulation so that it links and runs again.
- Build-time inputs, not kept here: Linux v7.0.14 (ath12k, MHI host, QMI library, QRTR; GPL-2.0 / BSD-3-Clause-Clear) and FreeBSD
  releng/15.1 linuxkpi headers (BSD-2-Clause), fetched by `tools/fetch-sources.sh` into `deps/` (ignored by git). `tools/prepare.sh`
  and `tools/peach.py` edit generated copies of them in `out/` (ignored) at build time.
