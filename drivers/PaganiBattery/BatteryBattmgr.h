/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved.
 * Ported from UniXNU-sheng package/ShengBattmgr.h (https://github.com/UniXNU/UniXNU-sheng, commit 08cde20e). 13T changes: the property
 * numbers of the OnePlus (OPLUS) charger firmware with a check of the layout the firmware really uses, the notification request in the
 * form the OPLUS/Qualcomm kernel sends, generic property writes (queued, one in flight) instead of the one USB limit, no Xiaomi board
 * properties. */
/* The battery manager the audio DSP runs (PMIC GLINK, channel "PMIC_RTR_ADSP_APPS", charger PD "msm/adsp/charger_pd"): messages start
 * with a 12-byte header (owner 32778, type, opcode); a property read is a request {battery 0, property, 0} answered by {property, value,
 * result}; a write is the same request with the value; the DSP sends a 16-byte notification {header, what} when something changed. This
 * is the codec and the refresh cycle (one request in flight, the next one only when it is answered or the owner's timer gives up on it).
 * No I/O, no waits: the owner sends the bytes and feeds back what arrives.
 *
 * Property numbers. Qualcomm's charger firmware numbers the battery properties in one list; vendors insert their own. The OPLUS list
 * (OnePlus 13T kernel facts: oplus_hal_adsp.h battery_property_id) has temperature at 12, charge counter 14, cycles 15, design 16, full
 * 17, time to full 19, time to empty 20 and BATT_CHG_EN at 24; the newer Qualcomm list (sheng's Xiaomi firmware) has one more before the
 * temperature (constant charge current, 12), so everything from the temperature on is one higher. Both agree on 0..11 and on the USB
 * properties. The first cycle reads properties 12 and 13 as probes: an OPLUS firmware answers 12 with a temperature and 13 with the
 * battery technology (a small enum), a Qualcomm one 13 with the temperature. Until the layout is known the fields that depend on it are
 * not read, and a write of a vendor property is never sent before it is known to be OPLUS. */
#ifndef PAGANI_BATTERY_BATTMGR_H
#define PAGANI_BATTERY_BATTMGR_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define PBM_OWNER 32778u            /* the battery manager; 32779 is the USB-C port manager on the same channel */
#define PBM_TYPE_REQUEST 1u
#define PBM_TYPE_NOTIFY 2u
#define PBM_OP_SET_NOTIFY 0x04u
#define PBM_OP_NOTIFY 0x07u
#define PBM_OP_BATTERY_GET 0x30u
#define PBM_OP_BATTERY_SET 0x31u
#define PBM_OP_USB_GET 0x32u
#define PBM_OP_USB_SET 0x33u
#define PBM_REQUEST_BYTES 24u
#define PBM_NOTIFY_REQUEST_BYTES 28u
#define PBM_MESSAGE_MAX 64u         /* the longest message this codec reads (answers are 24 bytes, notifications 16) */
#define PBM_PENDING_SETS 4u

/* Property layouts. */
enum {PbmLayoutUnknown = 0, PbmLayoutOplus = 1, PbmLayoutQti = 2};

/* Battery property numbers this driver writes (stable in both layouts unless noted). */
enum {
    PbmPropChargeControlLimit = 10, /* battery: charge current limit, uA (the kernel's "charge control limit" in microamperes) */
    PbmPropOplusChargeEnable = 24,  /* battery, OPLUS layout only: BATT_CHG_EN 0/1 */
    PbmPropUsbInputLimit = 5,       /* usb: input current limit, uA; 0 suspends the input, 0xffffffff lifts the host's limit */
};

/* The fields a refresh reads, in order (at most 32: valid has a bit per field). */
enum {
    PbmStatus, PbmPresent, PbmCapacity, PbmVoltage, PbmCurrent, PbmChargeType, PbmChargeLimit, PbmHealth,
    PbmTemperature, PbmChargeCounter, PbmTimeToFull, PbmTimeToEmpty, PbmChargeEnable, PbmCycles, PbmDesign, PbmFull,
    PbmUsbOnline, PbmUsbVoltage, PbmUsbCurrent, PbmUsbCurrentMax, PbmUsbInputLimit, PbmUsbType, PbmUsbAdapter, PbmUsbRealType,
    PbmProbe12, PbmProbe13,
    PbmFields
};

typedef struct {
    uint32_t opcode, property, value;
    uint32_t tag;                   /* the owner's name for it, given back with the result */
} PbmSet;

typedef struct {
    int32_t value[PbmFields];
    uint32_t valid;                 /* bit per field: answered without error in the last cycle that read it */
    uint8_t cursor, in_flight, slow_due, layout, layout_assumed, enabled;
    uint32_t refresh_wanted;        /* a notification or a set asked for a new cycle while one ran (2: a slow one) */
    uint32_t cycles_done, responses, notifications, errors, timeouts, ignored, undecided_cycles;
    int last_timeout;               /* the field the last timeout skipped, -1 none */
    uint32_t last_notification;
    /* writes: queued, then one in flight (fields wait meanwhile) */
    PbmSet queue[PBM_PENDING_SETS];
    uint32_t queued;
    PbmSet flight;
    uint8_t set_in_flight;
    uint32_t sets_sent, sets_ok, sets_failed, sets_dropped;
    PbmSet last_set;                /* the last answered or timed out write */
    int32_t last_set_result;        /* its result: 0 taken, the firmware's code, -60 timed out */
    uint32_t last_set_done;         /* counts every finished write (the owner watches it) */
} PbmState;

/* Results of pagani_bm_receive. */
enum {PbmIgnored = 0, PbmAnswer = 1, PbmCycleDone = 2, PbmNotified = 3, PbmEnabled = 4, PbmSetDone = 5};

void pagani_bm_init(PbmState *b);
/* The notification enable request (sent once the channel is up): type notify, all fields 0, as the Qualcomm and OPLUS kernels send it. */
uint32_t pagani_bm_enable_request(uint8_t out[PBM_NOTIFY_REQUEST_BYTES]);
/* Starts a refresh cycle (no effect while one runs: it is remembered and runs after). slow: also the fields that hardly change. */
void pagani_bm_refresh(PbmState *b, int slow);
/* Queues a write. 0 queued; -1 full; -2 refused (a vendor property before the OPLUS layout is known, or an unknown opcode). */
int pagani_bm_set(PbmState *b, uint32_t opcode, uint32_t property, uint32_t value, uint32_t tag);
/* The next request: a queued write first, else the running cycle's next field; 0 when nothing is to be sent now. */
uint32_t pagani_bm_next(PbmState *b, uint8_t out[PBM_REQUEST_BYTES]);
/* The owner's request timer ran out: the field is skipped, or the write finishes as timed out. */
void pagani_bm_timeout(PbmState *b);
/* 1 while a request waits for its answer. */
int pagani_bm_busy(const PbmState *b);
/* A message from the channel; returns one of the Pbm* results. */
int pagani_bm_receive(PbmState *b, const uint8_t *data, uint32_t length);
/* The property number of a field in the current layout (0xff: not read in this layout). */
uint32_t pagani_bm_property(const PbmState *b, int field);
/* The layout from the two probe answers (exposed for the tests). */
int pagani_bm_layout_from_probes(int has12, int32_t p12, int has13, int32_t p13);
static inline int pagani_bm_has(const PbmState *b, int field) {return (int)((b->valid >> field) & 1u);}

#ifdef __cplusplus
}
#endif
#endif
