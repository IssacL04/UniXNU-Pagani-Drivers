/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* The interface of PaganiWifi (README.md): the start Stage7Probe calls, the status, and what the network side (PaganiWifiNet.cpp) and
 * the Ath12kWifi stack take from the board. Off by default: without pagani-wifi=N (1..6) pagani_wifi_start returns at once and
 * nothing is mapped, allocated or started. */
#ifndef PAGANI_WIFI_H
#define PAGANI_WIFI_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Stage7Probe: after the ADSP and battery starts; starts the "pagani-wifi" kernel thread and returns at once. */
void pagani_wifi_start(void);

/* The status words (also in the ramoops log as [wifi] lines):
 *  [0] the last step reached (stage * 100 + n, WifiBringup.h kStep*, 5xx/6xx below), negative: the step that failed
 *  [1] the error code of that failure (0 none)   [2] the stage asked for (pagani-wifi)   [3] seconds since the thread started
 *  [4] endpoint vendor/device   [5] link status (DBI 0x80)   [6] link ms   [7] BAR0 bus address   [8] BAR0 bytes
 *  [9] SMMU slot | bank << 16 (bit 31: mapped)   [10] DMA window MiB   [11] DMA bus address >> 12
 *  [12] MSI: 1 attached, 0 polling, negative the attach error   [13] MSIs counted   [14] MHI phase | error << 8 | failed phase << 16
 *  [15] MHI execution environment   [16] amss bytes   [17] firmware source (1 partition, 2 seed)   [18] nw_wifi_stats.stage
 *  [19] scans   [20] networks seen   [21] frames   [22] the stack's last error   [23] network service up (1)
 *  [24] chip TCSR_SOC_HW_VERSION   [25] BHI execution environment at stage 4   [26] MHI status register at stage 4
 *  [27] firmware crash: 1 + the messages found in its RAM dump, negative the reader's error (WifiRddm.h), 0 no crash seen */
extern volatile int32_t pagani_wifi_status[32];

/* Steps of stages 4 to 6 beyond WifiBringup.h's. */
enum {
    PaganiWifiStepThread = 1, PaganiWifiStepMapped = 2,
    PaganiWifiStepDma = 403, PaganiWifiStepSmmu = 404, PaganiWifiStepBarMapped = 405, PaganiWifiStepMhiRegs = 406,
    PaganiWifiStepFirmware = 501, PaganiWifiStepChipReset = 502, PaganiWifiStepMhiOpen = 503, PaganiWifiStepSbl = 504,
    PaganiWifiStepContexts = 505, PaganiWifiStepMission = 506,
    PaganiWifiStepStack = 601, PaganiWifiStepStackUp = 602, PaganiWifiStepNet = 603,
};

/* A file of the firmware sources by name (the partition, then the Data seed): 0 and the cached bytes, or negative;
 * PaganiWifiFileNotYet while the seed's volume is not mounted (the seed's files can be asked again). */
enum {PaganiWifiFileNotYet = -10};
int pagani_wifi_file(const char *name, const uint8_t **data, size_t *size);
/* The network side queued a frame: the Wi-Fi thread sends it now instead of at its next wake. */
void pagani_wifi_wake(void);
/* A line in the Wi-Fi log (level as syslog: 3 error ... 7 debug). */
void pagani_wifi_logf(int level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
/* PaganiWifiNet.cpp: the interface en0, the registry entries and the Apple80211 requests, once the stack has a station interface. */
void pagani_wifi_net_start(const uint8_t mac[6]);
/* The board options the Ath12kWifi peach entry asks for ("m3", "tme", "qdss", "cbc"): pagani-wifi-<name>, else fallback. */
int nw_board_option(const char *name, int fallback);

#ifdef __cplusplus
}
#endif
#endif
