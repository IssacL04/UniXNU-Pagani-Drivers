# Ath12kWifi

ath12k Wi-Fi transport with MHI and QRTR.

Platform callbacks provide PCI configuration, BARs, MSI, and DMA. The source includes qcc2072, qcn9274, and wcn7850 HAL build inputs.

Build and interface details: [documentation](https://unixnu.com/drivers/ath12k-wifi). Component sources and dependencies: [catalog](https://unixnu.com/reference/components).

## PaganiWifi copy: the peach entry

This directory is a copy of [UniXNU/Ath12kWifi](https://github.com/UniXNU/Ath12kWifi) at `f6bf561f92cc42761bdd417fce1d53fe1db9477c`
(see `ORIGIN.md`), kept by PaganiWifi (OnePlus 13T). Changes: `tools/peach.py` (new, run at the end of `tools/prepare.sh`), weak
`nw_board_option` and `nw_peach_step` in `src/nw_wifi.c`, `nw_mhi_reload` in `src/nw_mhi.c`, a real `flush_work` in the runtime (it cancelled
the item; the one caller is `mhi_power_down`), and in `tests/stack_sim.c` the peach cases that `tools/peach-sim.sh` runs under ASan. On the
generated copies of the fetched Linux v7.0.14 sources it adds PCI `17cb:110e` (peach, WCN7880 family) probed as QCC2072, and for that device
only the QMI steps peach needs (M3 choice, TME-Lite patch, QDSS configuration, feature list, board size, cold-boot calibration and the
host's restart after it), each behind a board option (`nw_board_option(name, default)`; PaganiWifi maps it to the boot argument
`pagani-wifi-<name>`). The rest of the stack is unchanged; the WCN7850 path behaves as upstream's. The upstream `tools/sim.sh` regenerates
`out/ath12k-src` without these edits and does not test them: use `tools/peach-sim.sh`.
