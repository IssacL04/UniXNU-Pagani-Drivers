#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Copyright (c) 2026, UniXNU contributors. All rights reserved.
"""The peach (PCI 17cb:110e, peach_v2, the WCN7880 family on the OnePlus 13T) entry of the generated ath12k copy.

Run by tools/prepare.sh after its own edits, on the generated tree (out/ath12k-src): like prepare.sh it edits copies made at build
time from the fetched Linux v7.0.14 sources and checks that each mark it edits at occurs exactly once, so the repository holds no
ath12k code, only the marks and the code added here (written for this component from facts: PaganiWifi's PaganiWifiTables.h and
build-userspace/a830-inputs/wifi/facts.md; no code of cnss2, pagani-linux or another driver is copied).

What peach is to ath12k v7.0.14 (facts.md section 7): the QCC2072 entry already has peach's remap window register (0x3278), its UMAC
register layout and compact RX descriptors, the WCN7850 CE/MHI/WMI configuration, QMI instance 1, the internal sleep clock and the
AUX microcode download. So:
  1 wifi7/pci.c: 0x110e is listed and probed as QCC2072 (the switch falls into its case). GUESS: everything else of that entry fits.
  2 qmi.c, all only for device 0x110e and each switchable by a board option (nw_board_option, PaganiWifi: pagani-wifi-<name>):
    m3       1 (default, cnss2): m3.bin (the board serves peach/phy_ucode20.elf); 0 (pagani-linux): a null M3 and no m3_support
    tme      1 (default): after the target capability, the TME-Lite patch (QMI 0x5b, file type 0, tmel_peach_20.elf); a failure is
             only logged (cnss2 and pagani-linux go on too)
    aux      1 (default): the AUX microcode as QCC2072 does; a failure is only logged for peach
    qdss     0 (default, the QCC2072 entry): the host capability says QDSS_CFG_MISS; 1 (cnss2): the bit is cleared and the QDSS trace
             configuration (QMI 0x44, 6144-byte segments) is sent after the AUX microcode. GUESS: which one peach needs (pagani-linux
             says the firmware waits for the configuration when the bit is clear).
    cbc      1 (default, cnss2 qcom,wlan-cbc-enabled on this phone): at the first FW_READY, the CALDB size report (QMI 0x26), the
             wlan mode CALIBRATION (7), the wait for COLD_BOOT_CAL_DONE (0x3e, 60 s), then the mode OFF. The next session (cal_done set in
             the host capability) ends in a second FW_READY, which starts mission mode. On this phone that session is the host's to start:
             Android powers the chip off after the mode OFF and loads the firmware again (dmesg of 2026-10-31: PERST# low 120 ms after the
             calibration, power on 111 ms later, amss20.bin again), as ath11k does with hif_power_down and hif_power_up for chips that
             need it; pagani-linux instead waits for the firmware to restart by itself. So a firmware that shows nothing within 2 s of the
             mode OFF (10 s from the moment its service is back, if it is) is restarted by the host, once: nw_peach_step, which
             nw_wifi_step calls between dispatcher rounds because a power down and a boot wait, and a QMI work item must not hold those
             waits. cal_done and the memory the firmware was given stay. A failure before the mode OFF falls through to mission mode.
    features nonzero: the host capability's feature list as given (cnss2 bit numbers), e.g. pagani-linux's 0x46 (DRV, WLAN_EN,
             AUX_UC); 0 (default): the entry's list (minus QDSS_CFG_MISS with qdss=1)
    boardkib nonzero: the most board data bytes sent (KiB); 0 (default): the entry's 256 KiB. GUESS: peach's bdwlan.elf fits.
  The message layouts are the WLFW v01 TLVs (facts): TME-Lite 0x01 file type (enum), 0x02 address (u64), 0x03 size (u32); QDSS
  download 0x10 total size, 0x11 segment, 0x12 data (u16 length + up to 6144 bytes), 0x13 end; CAL report 0x01 meta data (u8 count of
  enums, up to 5), 0x10 XO calibration, 0x11 removal supported, 0x12 calibration file size (u64); CAL_DONE carries nothing.

  peach.py ATH12K_SOURCE_DIR GENERATED_DIR
"""
import os
import sys
from pathlib import Path

PEACH = 0x110e


def edit(path, changes, source=None):
    """Applies (old, new) pairs, each old exactly once; writes a real file in place of a link."""
    text = Path(source or path).read_text()
    for old, new in changes:
        count = text.count(old)
        if count != 1:
            sys.exit(f'peach.py: {path}: mark found {count} times: {old[:70]!r}')
        text = text.replace(old, new, 1)
    if os.path.lexists(path):
        os.remove(path)
    Path(path).write_text(text)


PCI = [
    ('#define QCC2072_DEVICE_ID\t\t0x1112\n',
     '#define QCC2072_DEVICE_ID\t\t0x1112\n'
     '/* peach (OnePlus 13T, PaganiWifi tools/peach.py): probed as QCC2072, whose entry has peach\'s window register and layouts */\n'
     '#define NW_PEACH_DEVICE_ID\t\t0x%x\n' % PEACH),
    ('\t{ PCI_VDEVICE(QCOM, QCC2072_DEVICE_ID) },\n',
     '\t{ PCI_VDEVICE(QCOM, QCC2072_DEVICE_ID) },\n\t{ PCI_VDEVICE(QCOM, NW_PEACH_DEVICE_ID) },\n'),
    ('\tcase QCC2072_DEVICE_ID:\n', '\tcase NW_PEACH_DEVICE_ID:\n\tcase QCC2072_DEVICE_ID:\n'),
]

PROTOTYPES = r'''
/* ---- peach (PCI 17cb:110e): tools/peach.py of PaganiWifi's Ath12kWifi copy ---- */
#include "hif.h"
#include "pci.h"
int nw_board_option(const char *name, int fallback);
void nw_mhi_reload(void);
static void nw_restart_cancel(void);
static void nw_restart_service_back(void);
static bool nw_peach(struct ath12k_base *ab);
static bool nw_m3_driver(struct ath12k_base *ab);
static u64 nw_features(struct ath12k_base *ab, u64 features);
static u32 nw_board_size(struct ath12k_base *ab);
static void nw_peach_tme(struct ath12k_base *ab);
static void nw_peach_qdss(struct ath12k_base *ab);
static bool nw_peach_aux(struct ath12k_base *ab);
'''

CODE = r'''
/* ---- peach (PCI 17cb:110e): the code tools/peach.py adds; facts in its header and in PaganiWifi's PaganiWifiTables.h ---- */
#define NW_PEACH 0x110e
#define NW_QMI_TME_LITE_INFO 0x005b
#define NW_QMI_TME_LITE_INFO_LEN 25
#define NW_QMI_QDSS_DOWNLOAD 0x0044
#define NW_QMI_QDSS_DOWNLOAD_LEN 6167
#define NW_QMI_CAL_REPORT 0x0026
#define NW_QMI_CAL_REPORT_LEN 43
#define NW_QMI_CAL_DONE_IND 0x003e
#define NW_WLAN_MODE_CALIBRATION 7
#define NW_CAL_MAX 5
#define NW_COLD_BOOT_MS 60000
#define NW_RESTART_WAIT_MS 2000         /* mode OFF to the host's restart */
#define NW_RESTART_SESSION_MS 10000     /* the service came back by itself: to its FW_READY */
#define NW_RESTART_OFF_MS 150           /* power down to power up (Android: 111 ms) */

static bool nw_peach(struct ath12k_base *ab) { return ab->id.device == NW_PEACH; }
static int nw_option(struct ath12k_base *ab, const char *name, int fallback)
{
	return nw_peach(ab) ? nw_board_option(name, fallback) : fallback;
}
static bool nw_m3_driver(struct ath12k_base *ab)
{
	return ab->hw_params->fw.m3_loader == ath12k_m3_fw_loader_driver && nw_option(ab, "m3", 1);
}
static u64 nw_features(struct ath12k_base *ab, u64 features)
{
	int given = nw_option(ab, "features", 0);

	if (given)
		return (u64)(u32)given;
	if (nw_option(ab, "qdss", 0))
		features &= ~BIT(CNSS_QDSS_CFG_MISS_V01);
	return features;
}
static u32 nw_board_size(struct ath12k_base *ab)
{
	int kib = nw_option(ab, "boardkib", 0);

	return kib > 0 ? (u32)kib * 1024 : ab->hw_params->fw.board_size;
}
static bool nw_peach_aux(struct ath12k_base *ab) { return nw_option(ab, "aux", 1) != 0; }

struct nw_qmi_resp {
	struct qmi_response_type_v01 resp;
};
static const struct qmi_elem_info nw_qmi_resp_ei[] = {
	{
		.data_type	= QMI_STRUCT,
		.elem_len	= 1,
		.elem_size	= sizeof(struct qmi_response_type_v01),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x02,
		.offset		= offsetof(struct nw_qmi_resp, resp),
		.ei_array	= qmi_response_type_v01_ei,
	},
	{
		.data_type	= QMI_EOTI,
		.array_type	= NO_ARRAY,
		.tlv_type	= QMI_COMMON_TLV_TYPE,
	},
};

/* One request whose answer is only the result: 0, or an error (logged). */
static int nw_qmi_send(struct ath12k_base *ab, int msg_id, size_t len, const struct qmi_elem_info *ei, const void *req,
		       const char *what)
{
	struct nw_qmi_resp resp = {};
	struct qmi_txn txn;
	int ret;

	ret = qmi_txn_init(&ab->qmi.handle, &txn, nw_qmi_resp_ei, &resp);
	if (ret < 0)
		return ret;
	ret = qmi_send_request(&ab->qmi.handle, NULL, &txn, msg_id, len, ei, req);
	if (ret < 0) {
		qmi_txn_cancel(&txn);
		ath12k_warn(ab, "peach: qmi %s not sent: %d\n", what, ret);
		return ret;
	}
	ret = qmi_txn_wait(&txn, msecs_to_jiffies(ATH12K_QMI_WLANFW_TIMEOUT_MS));
	if (ret < 0) {
		ath12k_warn(ab, "peach: qmi %s: no answer: %d\n", what, ret);
		return ret;
	}
	if (resp.resp.result != QMI_RESULT_SUCCESS_V01) {
		ath12k_warn(ab, "peach: qmi %s refused: result %d error %d\n", what, resp.resp.result, resp.resp.error);
		return -EINVAL;
	}
	ath12k_info(ab, "peach: qmi %s done\n", what);
	return 0;
}

/* TME-Lite: the patch file, in DMA memory the firmware reads by address (kept: it may read it again). */
struct nw_tme_req {
	u32 file;
	u64 addr;
	u32 size;
};
static const struct qmi_elem_info nw_tme_req_ei[] = {
	{ .data_type = QMI_SIGNED_4_BYTE_ENUM, .elem_len = 1, .elem_size = sizeof(u32), .array_type = NO_ARRAY, .tlv_type = 0x01,
	  .offset = offsetof(struct nw_tme_req, file) },
	{ .data_type = QMI_UNSIGNED_8_BYTE, .elem_len = 1, .elem_size = sizeof(u64), .array_type = NO_ARRAY, .tlv_type = 0x02,
	  .offset = offsetof(struct nw_tme_req, addr) },
	{ .data_type = QMI_UNSIGNED_4_BYTE, .elem_len = 1, .elem_size = sizeof(u32), .array_type = NO_ARRAY, .tlv_type = 0x03,
	  .offset = offsetof(struct nw_tme_req, size) },
	{ .data_type = QMI_EOTI, .array_type = NO_ARRAY, .tlv_type = QMI_COMMON_TLV_TYPE },
};
static void nw_peach_tme(struct ath12k_base *ab)
{
	struct nw_tme_req req = {};
	const struct firmware *fw;
	static dma_addr_t paddr;
	static void *vaddr;     /* one buffer for both sessions of the calibration pass */

	if (!nw_peach(ab) || !nw_board_option("tme", 1))
		return;
	fw = ath12k_core_firmware_request(ab, "tmel_peach_20.elf");
	if (IS_ERR(fw)) {
		ath12k_warn(ab, "peach: tmel_peach_20.elf not found: %ld\n", PTR_ERR(fw));
		return;
	}
	if (!vaddr)
		vaddr = dma_alloc_coherent(ab->dev, fw->size, &paddr, GFP_KERNEL);
	if (!vaddr) {
		ath12k_warn(ab, "peach: no DMA memory for the TME-Lite patch (%zu bytes)\n", fw->size);
		release_firmware(fw);
		return;
	}
	memcpy(vaddr, fw->data, fw->size);
	req.file = 0;
	req.addr = paddr;
	req.size = fw->size;
	nw_qmi_send(ab, NW_QMI_TME_LITE_INFO, NW_QMI_TME_LITE_INFO_LEN, nw_tme_req_ei, &req, "tme-lite patch");
	release_firmware(fw);
}

/* The QDSS trace configuration, in segments of up to QMI_WLANFW_MAX_DATA_SIZE_V01 bytes (total size = what is left, as the BDF). */
struct nw_qdss_req {
	u8 total_size_valid;
	u32 total_size;
	u8 seg_id_valid;
	u32 seg_id;
	u8 data_valid;
	u32 data_len;
	u8 data[QMI_WLANFW_MAX_DATA_SIZE_V01];
	u8 end_valid;
	u8 end;
};
static const struct qmi_elem_info nw_qdss_req_ei[] = {
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(u8), .array_type = NO_ARRAY, .tlv_type = 0x10,
	  .offset = offsetof(struct nw_qdss_req, total_size_valid) },
	{ .data_type = QMI_UNSIGNED_4_BYTE, .elem_len = 1, .elem_size = sizeof(u32), .array_type = NO_ARRAY, .tlv_type = 0x10,
	  .offset = offsetof(struct nw_qdss_req, total_size) },
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(u8), .array_type = NO_ARRAY, .tlv_type = 0x11,
	  .offset = offsetof(struct nw_qdss_req, seg_id_valid) },
	{ .data_type = QMI_UNSIGNED_4_BYTE, .elem_len = 1, .elem_size = sizeof(u32), .array_type = NO_ARRAY, .tlv_type = 0x11,
	  .offset = offsetof(struct nw_qdss_req, seg_id) },
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(u8), .array_type = NO_ARRAY, .tlv_type = 0x12,
	  .offset = offsetof(struct nw_qdss_req, data_valid) },
	{ .data_type = QMI_DATA_LEN, .elem_len = 1, .elem_size = sizeof(u16), .array_type = NO_ARRAY, .tlv_type = 0x12,
	  .offset = offsetof(struct nw_qdss_req, data_len) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = QMI_WLANFW_MAX_DATA_SIZE_V01, .elem_size = sizeof(u8),
	  .array_type = VAR_LEN_ARRAY, .tlv_type = 0x12, .offset = offsetof(struct nw_qdss_req, data) },
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(u8), .array_type = NO_ARRAY, .tlv_type = 0x13,
	  .offset = offsetof(struct nw_qdss_req, end_valid) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(u8), .array_type = NO_ARRAY, .tlv_type = 0x13,
	  .offset = offsetof(struct nw_qdss_req, end) },
	{ .data_type = QMI_EOTI, .array_type = NO_ARRAY, .tlv_type = QMI_COMMON_TLV_TYPE },
};
static void nw_peach_qdss(struct ath12k_base *ab)
{
	const struct firmware *fw;
	struct nw_qdss_req *req;
	const u8 *at;
	size_t left;
	int ret = 0;

	if (!nw_peach(ab) || !nw_board_option("qdss", 0))
		return;
	fw = ath12k_core_firmware_request(ab, "qdss_trace_config_v2.cfg");
	if (IS_ERR(fw)) {
		ath12k_warn(ab, "peach: qdss trace configuration not found: %ld\n", PTR_ERR(fw));
		return;
	}
	req = kzalloc(sizeof(*req), GFP_KERNEL);
	if (!req) {
		release_firmware(fw);
		return;
	}
	at = fw->data;
	left = fw->size;
	while (left && !ret) {
		req->total_size_valid = 1;
		req->total_size = (u32)left;
		req->seg_id_valid = 1;
		req->data_valid = 1;
		req->end_valid = 1;
		req->data_len = left > QMI_WLANFW_MAX_DATA_SIZE_V01 ? QMI_WLANFW_MAX_DATA_SIZE_V01 : (u32)left;
		req->end = left <= QMI_WLANFW_MAX_DATA_SIZE_V01;
		memcpy(req->data, at, req->data_len);
		ret = nw_qmi_send(ab, NW_QMI_QDSS_DOWNLOAD, NW_QMI_QDSS_DOWNLOAD_LEN, nw_qdss_req_ei, req, "qdss trace configuration");
		at += req->data_len;
		left -= req->data_len;
		req->seg_id++;
	}
	kfree(req);
	release_firmware(fw);
}

/* Cold-boot calibration (first FW_READY only). */
struct nw_cal_report_req {
	u32 meta_data_len;
	u32 meta_data[NW_CAL_MAX];
	u8 xo_cal_data_valid;
	u8 xo_cal_data;
	u8 cal_remove_supported_valid;
	u8 cal_remove_supported;
	u8 cal_file_download_size_valid;
	u64 cal_file_download_size;
};
static const struct qmi_elem_info nw_cal_report_req_ei[] = {
	{ .data_type = QMI_DATA_LEN, .elem_len = 1, .elem_size = sizeof(u8), .array_type = NO_ARRAY, .tlv_type = 0x01,
	  .offset = offsetof(struct nw_cal_report_req, meta_data_len) },
	{ .data_type = QMI_SIGNED_4_BYTE_ENUM, .elem_len = NW_CAL_MAX, .elem_size = sizeof(u32), .array_type = VAR_LEN_ARRAY,
	  .tlv_type = 0x01, .offset = offsetof(struct nw_cal_report_req, meta_data) },
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(u8), .array_type = NO_ARRAY, .tlv_type = 0x10,
	  .offset = offsetof(struct nw_cal_report_req, xo_cal_data_valid) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(u8), .array_type = NO_ARRAY, .tlv_type = 0x10,
	  .offset = offsetof(struct nw_cal_report_req, xo_cal_data) },
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(u8), .array_type = NO_ARRAY, .tlv_type = 0x11,
	  .offset = offsetof(struct nw_cal_report_req, cal_remove_supported_valid) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(u8), .array_type = NO_ARRAY, .tlv_type = 0x11,
	  .offset = offsetof(struct nw_cal_report_req, cal_remove_supported) },
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(u8), .array_type = NO_ARRAY, .tlv_type = 0x12,
	  .offset = offsetof(struct nw_cal_report_req, cal_file_download_size_valid) },
	{ .data_type = QMI_UNSIGNED_8_BYTE, .elem_len = 1, .elem_size = sizeof(u64), .array_type = NO_ARRAY, .tlv_type = 0x12,
	  .offset = offsetof(struct nw_cal_report_req, cal_file_download_size) },
	{ .data_type = QMI_EOTI, .array_type = NO_ARRAY, .tlv_type = QMI_COMMON_TLV_TYPE },
};
struct nw_cal_done_ind {
	char placeholder;
};
static const struct qmi_elem_info nw_cal_done_ind_ei[] = {
	{ .data_type = QMI_EOTI, .array_type = NO_ARRAY, .tlv_type = QMI_COMMON_TLV_TYPE },
};
static void nw_cal_done_cb(struct qmi_handle *qmi_hdl, struct sockaddr_qrtr *sq, struct qmi_txn *txn, const void *decoded)
{
	struct ath12k_qmi *qmi = container_of(qmi_hdl, struct ath12k_qmi, handle);

	(void)sq;
	(void)txn;
	(void)decoded;
	qmi->ab->qmi.cal_done = 1;
	ath12k_info(qmi->ab, "peach: cold-boot calibration done\n");
}
static bool nw_cbc_tried;
static bool nw_peach_cbc_pending(struct ath12k_base *ab)
{
	return nw_peach(ab) && nw_board_option("cbc", 1) && !ab->qmi.cal_done && !nw_cbc_tried;
}

/* The restart after the calibration. Armed by the mode OFF, disarmed by the next FW_READY; the service coming back first gives that session
 * time to get there. It runs once: a firmware that does not come back from it stays where it is, in the log. */
static struct ath12k_base *nw_restart_ab;
static u64 nw_restart_due;
static void nw_restart_cancel(void)
{
	if (nw_restart_due)
		ath12k_info(nw_restart_ab, "peach: FW_READY again, no restart from the host\n");
	nw_restart_due = 0;
}
static void nw_restart_service_back(void)
{
	if (!nw_restart_due)
		return;
	nw_restart_due = lx_now_ms() + NW_RESTART_SESSION_MS;
	ath12k_info(nw_restart_ab, "peach: the firmware's service is back by itself: %d ms for its FW_READY\n", NW_RESTART_SESSION_MS);
}
int nw_peach_step(void)
{
	struct ath12k_base *ab = nw_restart_ab;
	int ret;

	if (!nw_restart_due || lx_now_ms() < nw_restart_due)
		return 0;
	nw_restart_due = 0;
	ath12k_warn(ab, "peach: no FW_READY after the mode OFF: the host powers the firmware down and up\n");
	/* ath12k_pci_power_down does nothing until ath12k_pci_start has run, and that only comes with mission mode */
	set_bit(ATH12K_PCI_FLAG_INIT_DONE, &ath12k_pci_priv(ab)->flags);
	ath12k_hif_power_down(ab, false);
	msleep(NW_RESTART_OFF_MS);
	nw_mhi_reload();
	ret = ath12k_hif_power_up(ab);
	if (ret)
		ath12k_err(ab, "peach: the restart after the calibration failed: %d\n", ret);
	else
		ath12k_info(ab, "peach: firmware loaded again, mission mode of MHI\n");
	return 1;
}

/* 0 when the firmware calibrated and got the mode OFF (a second session ends in FW_READY, by itself or by nw_peach_step); otherwise
 * mission mode is tried at once (logged). */
static int nw_peach_cold_boot(struct ath12k_base *ab)
{
	struct nw_cal_report_req req = {};
	wait_queue_head_t unused;
	long left;
	int ret, i;

	nw_cbc_tried = true;
	init_waitqueue_head(&unused);
	for (i = 0; i < ab->qmi.mem_seg_count; i++)
		if (ab->qmi.target_mem[i].type == CALDB_MEM_REGION_TYPE) {
			req.cal_file_download_size = ab->qmi.target_mem[i].size;
			break;
		}
	req.cal_file_download_size_valid = 1;
	nw_qmi_send(ab, NW_QMI_CAL_REPORT, NW_QMI_CAL_REPORT_LEN, nw_cal_report_req_ei, &req, "calibration report");
	ret = ath12k_qmi_wlanfw_mode_send(ab, NW_WLAN_MODE_CALIBRATION);
	if (ret < 0) {
		ath12k_warn(ab, "peach: calibration mode refused: %d, trying mission mode\n", ret);
		return ret;
	}
	left = wait_event_timeout(unused, ab->qmi.cal_done == 1, msecs_to_jiffies(NW_COLD_BOOT_MS));
	if (left <= 0) {
		ath12k_warn(ab, "peach: no calibration done within %d ms, trying mission mode\n", NW_COLD_BOOT_MS);
		return -ETIMEDOUT;
	}
	ret = ath12k_qmi_wlanfw_mode_send(ab, ATH12K_FIRMWARE_MODE_OFF);
	if (ret < 0) {
		ath12k_warn(ab, "peach: mode off after calibration: %d\n", ret);
		return ret;
	}
	nw_restart_ab = ab;
	nw_restart_due = lx_now_ms() + NW_RESTART_WAIT_MS;
	ath12k_info(ab, "peach: calibrated; %d ms for the firmware to restart by itself\n", NW_RESTART_WAIT_MS);
	return 0;
}
'''

QMI = [
    ('#include "qmi.h"\n', '#include "qmi.h"\n' + PROTOTYPES),
    # host capability: the M3 switch and the feature list
    ('\tif (ab->hw_params->fw.m3_loader == ath12k_m3_fw_loader_driver) {\n\t\treq.m3_support_valid = 1;\n',
     '\tif (nw_m3_driver(ab)) {\n\t\treq.m3_support_valid = 1;\n'),
    ('\t\treq.feature_list = ab->hw_params->qmi_cnss_feature_bitmap;\n',
     '\t\treq.feature_list = nw_features(ab, ab->hw_params->qmi_cnss_feature_bitmap);\n'),
    # M3 information: a null region in the null-M3 mode
    ('\tif (ab->hw_params->fw.m3_loader == ath12k_m3_fw_loader_driver) {\n\t\tret = ath12k_qmi_m3_load(ab);\n',
     '\tif (nw_m3_driver(ab)) {\n\t\tret = ath12k_qmi_m3_load(ab);\n'),
    # the board data limit
    ('\tfw_size = min_t(u32, ab->hw_params->fw.board_size, bd.len);\n', '\tfw_size = min_t(u32, nw_board_size(ab), bd.len);\n'),
    # the load sequence: TME-Lite after the target capability, AUX optional (and non-fatal) for peach, QDSS after it
    ('\t\tath12k_warn(ab, "qmi failed to req target capabilities:%d\\n", ret);\n\t\treturn ret;\n\t}\n',
     '\t\tath12k_warn(ab, "qmi failed to req target capabilities:%d\\n", ret);\n\t\treturn ret;\n\t}\n\n\tnw_peach_tme(ab);\n'),
    ('\tif (hw_params->fw.download_aux_ucode) {\n\t\tret = ath12k_qmi_wlanfw_aux_uc_info_send(ab);\n'
     '\t\tif (ret < 0) {\n\t\t\tath12k_warn(ab, "qmi failed to send aux_uc info req: %d\\n", ret);\n\t\t\treturn ret;\n\t\t}\n\t}\n',
     '\tif (hw_params->fw.download_aux_ucode && nw_peach_aux(ab)) {\n\t\tret = ath12k_qmi_wlanfw_aux_uc_info_send(ab);\n'
     '\t\tif (ret < 0) {\n\t\t\tath12k_warn(ab, "qmi failed to send aux_uc info req: %d\\n", ret);\n'
     '\t\t\tif (!nw_peach(ab))\n\t\t\t\treturn ret;\n\t\t\tret = 0;\n\t\t}\n\t}\n\n\tnw_peach_qdss(ab);\n'),
    # the code, before the indication table; the calibration indication in it
    ('static const struct qmi_msg_handler ath12k_qmi_msg_handlers[] = {\n',
     CODE + '\nstatic const struct qmi_msg_handler ath12k_qmi_msg_handlers[] = {\n'),
    ('\t\t.fn = ath12k_qmi_msg_fw_ready_cb,\n\t},\n',
     '\t\t.fn = ath12k_qmi_msg_fw_ready_cb,\n\t},\n\t{\n\t\t.type = QMI_INDICATION,\n\t\t.msg_id = NW_QMI_CAL_DONE_IND,\n'
     '\t\t.ei = nw_cal_done_ind_ei,\n\t\t.decoded_size = sizeof(struct nw_cal_done_ind),\n\t\t.fn = nw_cal_done_cb,\n\t},\n'),
    # the first FW_READY of peach runs the calibration pass, the next one ends the wait for the restart
    ('\t\t\tclear_bit(ATH12K_FLAG_CRASH_FLUSH,\n\t\t\t\t  &ab->dev_flags);\n\t\t\tret = ath12k_core_qmi_firmware_ready(ab);\n',
     '\t\t\tif (nw_peach_cbc_pending(ab) && !nw_peach_cold_boot(ab))\n\t\t\t\tbreak;\n\n\t\t\tnw_restart_cancel();\n'
     '\t\t\tclear_bit(ATH12K_FLAG_CRASH_FLUSH,\n\t\t\t\t  &ab->dev_flags);\n\t\t\tret = ath12k_core_qmi_firmware_ready(ab);\n'),
    # the firmware's service back while the restart is armed: it restarted by itself
    ('\t\t\tret = ath12k_qmi_event_server_arrive(qmi);\n', '\t\t\tnw_restart_service_back();\n\t\t\tret = ath12k_qmi_event_server_arrive(qmi);\n'),
]


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    source, generated = Path(sys.argv[1]), Path(sys.argv[2])
    edit(generated / 'wifi7/pci.c', PCI, source / 'wifi7/pci.c')
    qmi = generated / 'qmi.c'
    edit(qmi, QMI, qmi if not qmi.is_symlink() else source / 'qmi.c')
    print('peach: 0x%x probed as QCC2072; qmi.c: M3, TME-Lite, AUX, QDSS, features, board size, calibration and restart hooks' % PEACH)


if __name__ == '__main__':
    main()
