/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved.
 * The scripted-firmware structure follows UniXNU-sheng package/tests/battmgr_host.c (commit 08cde20e). */
/* Host tests of BatteryBattmgr.c against two scripted charger firmwares: the OPLUS numbering (OnePlus 13T) and the newer Qualcomm one
 * (sheng's Xiaomi firmware), the layout probes, the queued writes and their refusals, notifications, timeouts and stray messages. */
#include <string.h>
#include "check.h"
#include "../BatteryBattmgr.h"
#include "../BatteryText.h"

typedef struct {
    int layout;                     /* PbmLayoutOplus or PbmLayoutQti */
    int32_t battery[32], usb[16];
    uint32_t battery_fail;          /* bit per battery property answered with an error */
    uint32_t set_result;
    unsigned asked_battery[32], asked_usb[16];
    uint32_t set_op[16], set_prop[16], set_value[16];
    unsigned sets;
} Fw;

/* A battery at 29.5 C, 87.5 %, 3.95 V, discharging 812 mA, 3200 of 6100 mAh, design 6260, 37 cycles, 6 h left. */
static void fw_battery(Fw *f, int layout)
{
    memset(f, 0, sizeof *f);
    f->layout = layout;
    f->battery[0] = 2; f->battery[1] = 1; f->battery[2] = 1; f->battery[3] = 1; f->battery[4] = 8750;
    f->battery[7] = 3950000; f->battery[9] = 812000; f->battery[10] = 3000000;
    const int o = layout == PbmLayoutQti;   /* the Qualcomm list has the constant charge current at 12 */
    if (o) f->battery[12] = 2000000;
    f->battery[12 + o] = 2950;              /* temperature */
    f->battery[13 + o] = 2;                 /* technology: Li-ion */
    f->battery[14 + o] = 3200000;           /* charge counter */
    f->battery[15 + o] = 37;                /* cycles */
    f->battery[16 + o] = 6260000;           /* design */
    f->battery[17 + o] = 6100000;           /* full */
    f->battery[19 + o] = -1;                /* time to full */
    f->battery[20 + o] = 6 * 3600;          /* time to empty */
    if (!o) f->battery[24] = 1;             /* BATT_CHG_EN */
    f->usb[0] = 0; f->usb[5] = 1500000; f->usb[6] = 0; f->usb[7] = 0; f->usb[11] = 0;
}

static uint32_t fw_answer(Fw *f, const uint8_t *request, uint8_t *out)
{
    CHECK(pbat_le32(request) == PBM_OWNER && pbat_le32(request + 4) == PBM_TYPE_REQUEST && pbat_le32(request + 12) == 0);
    const uint32_t opcode = pbat_le32(request + 8), property = pbat_le32(request + 16), value = pbat_le32(request + 20);
    pbat_put32(out, PBM_OWNER);
    pbat_put32(out + 4, PBM_TYPE_REQUEST);
    pbat_put32(out + 8, opcode);
    pbat_put32(out + 12, property);
    uint32_t answer = 0, result = 0;
    if (opcode == PBM_OP_BATTERY_GET) {
        CHECK(property < 32);
        f->asked_battery[property]++;
        answer = (uint32_t)f->battery[property];
        result = (f->battery_fail >> property) & 1u;
    } else if (opcode == PBM_OP_USB_GET) {
        CHECK(property < 16);
        f->asked_usb[property]++;
        answer = (uint32_t)f->usb[property];
    } else {
        CHECK(opcode == PBM_OP_BATTERY_SET || opcode == PBM_OP_USB_SET);
        CHECK(f->sets < 16);
        f->set_op[f->sets] = opcode; f->set_prop[f->sets] = property; f->set_value[f->sets] = value; f->sets++;
        result = f->set_result;
        if (!result) {
            if (opcode == PBM_OP_USB_SET) f->usb[property & 15] = (int32_t)value;
            else f->battery[property & 31] = (int32_t)value;
        }
        answer = value;
    }
    pbat_put32(out + 16, answer);
    pbat_put32(out + 20, result);
    return 24;
}

/* Runs requests until a cycle ends (a write in between is answered too); returns the number of requests. */
static unsigned run_cycle(PbmState *b, Fw *f)
{
    uint8_t request[PBM_REQUEST_BYTES], reply[24], spare[PBM_REQUEST_BYTES];
    unsigned n = 0;
    for (;;) {
        const uint32_t length = pagani_bm_next(b, request);
        if (!length) break;
        CHECK(length == 24);
        CHECK(pagani_bm_next(b, spare) == 0);  /* one in flight */
        CHECK(pagani_bm_busy(b));
        fw_answer(f, request, reply);
        const int r = pagani_bm_receive(b, reply, 24);
        ++n;
        if (r == PbmCycleDone) break;
        CHECK(r == PbmAnswer || r == PbmSetDone);
        CHECK(n < 200);
    }
    return n;
}

static void clear_asked(Fw *f) {memset(f->asked_battery, 0, sizeof f->asked_battery); memset(f->asked_usb, 0, sizeof f->asked_usb);}

static void test_probes(void)
{
    CHECK(pagani_bm_layout_from_probes(1, 2950, 1, 2) == PbmLayoutOplus);
    CHECK(pagani_bm_layout_from_probes(1, -500, 1, 1) == PbmLayoutOplus);     /* a cold battery */
    CHECK(pagani_bm_layout_from_probes(1, 2000000, 1, 2950) == PbmLayoutQti);
    CHECK(pagani_bm_layout_from_probes(1, 0, 1, 2950) == PbmLayoutQti);       /* no constant current set */
    CHECK(pagani_bm_layout_from_probes(1, 2950, 1, 99999) == PbmLayoutUnknown);
    CHECK(pagani_bm_layout_from_probes(1, 50000, 1, 3) == PbmLayoutUnknown);  /* 12 is no temperature */
    CHECK(pagani_bm_layout_from_probes(0, 2950, 1, 2) == PbmLayoutUnknown);
    CHECK(pagani_bm_layout_from_probes(1, 2950, 0, 2) == PbmLayoutUnknown);
    CHECK(pagani_bm_layout_from_probes(1, 2950, 1, -3) == PbmLayoutQti);      /* -0.03 C reads as a temperature */
}

static void test_enable(void)
{
    uint8_t enable[PBM_NOTIFY_REQUEST_BYTES];
    memset(enable, 0xaa, sizeof enable);
    CHECK(pagani_bm_enable_request(enable) == 28);
    CHECK(pbat_le32(enable) == 32778 && pbat_le32(enable + 4) == PBM_TYPE_NOTIFY && pbat_le32(enable + 8) == 4);
    for (unsigned i = 12; i < 28; i += 4) CHECK(pbat_le32(enable + i) == 0);
}

static void test_oplus(void)
{
    Fw f;
    PbmState b;
    fw_battery(&f, PbmLayoutOplus);
    pagani_bm_init(&b);
    uint8_t request[PBM_REQUEST_BYTES];
    CHECK(pagani_bm_next(&b, request) == 0);   /* no cycle yet */
    pagani_bm_refresh(&b, 0);
    /* First cycle: the common fields, the slow ones (nothing read yet) and the probes; nothing whose number depends on the layout. */
    const unsigned first = run_cycle(&b, &f);
    CHECK(first == 18);
    static const unsigned common[] = {0, 1, 2, 3, 4, 7, 9, 10};
    for (unsigned i = 0; i < sizeof common / sizeof common[0]; ++i) CHECK(f.asked_battery[common[i]] == 1);
    CHECK(f.asked_battery[12] == 1 && f.asked_battery[13] == 1);   /* the probes */
    static const unsigned dependent[] = {14, 15, 16, 17, 19, 20, 24};
    for (unsigned i = 0; i < sizeof dependent / sizeof dependent[0]; ++i) CHECK(f.asked_battery[dependent[i]] == 0);
    CHECK(f.asked_usb[0] && f.asked_usb[1] && f.asked_usb[3] && f.asked_usb[4] && f.asked_usb[5] && f.asked_usb[6] && f.asked_usb[7]);
    CHECK(f.asked_usb[11] == 1);
    CHECK(b.layout == PbmLayoutOplus && !b.layout_assumed && b.cycles_done == 1);
    CHECK(!pagani_bm_has(&b, PbmTemperature));
    /* The layout found, a slow cycle starts at once with the OPLUS numbers and without the probes. */
    clear_asked(&f);
    CHECK(pagani_bm_next(&b, request) == 24);
    uint8_t reply[24];
    fw_answer(&f, request, reply);
    CHECK(pagani_bm_receive(&b, reply, 24) == PbmAnswer);
    run_cycle(&b, &f);
    CHECK(b.cycles_done == 2);
    CHECK(f.asked_battery[12] == 1 && f.asked_battery[13] == 0);
    CHECK(f.asked_battery[14] && f.asked_battery[15] && f.asked_battery[16] && f.asked_battery[17] && f.asked_battery[19]);
    CHECK(f.asked_battery[20] && f.asked_battery[24]);
    CHECK(f.asked_battery[18] == 0 && f.asked_battery[21] == 0);
    CHECK(b.value[PbmTemperature] == 2950 && b.value[PbmChargeCounter] == 3200000 && b.value[PbmCycles] == 37);
    CHECK(b.value[PbmDesign] == 6260000 && b.value[PbmFull] == 6100000 && b.value[PbmTimeToEmpty] == 21600);
    CHECK(b.value[PbmTimeToFull] == -1 && b.value[PbmChargeEnable] == 1 && b.value[PbmCapacity] == 8750);
    CHECK(b.value[PbmVoltage] == 3950000 && b.value[PbmCurrent] == 812000 && b.value[PbmChargeLimit] == 3000000);
    CHECK(pagani_bm_property(&b, PbmTemperature) == 12 && pagani_bm_property(&b, PbmChargeEnable) == 24);
    CHECK(pagani_bm_next(&b, request) == 0);
    /* A plain refresh skips health, cycles, design, full and the real USB type. */
    clear_asked(&f);
    pagani_bm_refresh(&b, 0);
    CHECK(run_cycle(&b, &f) == 19);   /* 12 battery and 7 USB fields */
    CHECK(!f.asked_battery[1] && !f.asked_battery[15] && !f.asked_battery[16] && !f.asked_battery[17] && !f.asked_usb[11]);
    CHECK(pagani_bm_has(&b, PbmFull) && pagani_bm_has(&b, PbmDesign));   /* kept from the slow cycle */
    pagani_bm_refresh(&b, 1);
    CHECK(run_cycle(&b, &f) == 24);   /* and health, cycles, design, full, the real USB type */
}

static void test_qti(void)
{
    Fw f;
    PbmState b;
    fw_battery(&f, PbmLayoutQti);
    pagani_bm_init(&b);
    pagani_bm_refresh(&b, 1);
    run_cycle(&b, &f);
    CHECK(b.layout == PbmLayoutQti && !b.layout_assumed);
    clear_asked(&f);
    run_cycle(&b, &f);   /* the slow cycle the layout started */
    CHECK(f.asked_battery[13] == 1 && f.asked_battery[12] == 0 && f.asked_battery[24] == 0);
    CHECK(f.asked_battery[15] && f.asked_battery[16] && f.asked_battery[17] && f.asked_battery[18] && f.asked_battery[20] && f.asked_battery[21]);
    CHECK(b.value[PbmTemperature] == 2950 && b.value[PbmChargeCounter] == 3200000 && b.value[PbmCycles] == 37);
    CHECK(b.value[PbmDesign] == 6260000 && b.value[PbmFull] == 6100000 && b.value[PbmTimeToEmpty] == 21600);
    CHECK(!pagani_bm_has(&b, PbmChargeEnable));
    CHECK(pagani_bm_property(&b, PbmChargeEnable) == 0xff);
    /* No vendor write with the Qualcomm layout. */
    CHECK(pagani_bm_set(&b, PBM_OP_BATTERY_SET, PbmPropOplusChargeEnable, 1, 9) == -2);
}

static void test_assumed(void)
{
    Fw f;
    PbmState b;
    fw_battery(&f, PbmLayoutOplus);
    f.battery_fail = 1u << 12 | 1u << 13;   /* the probes answer with errors */
    pagani_bm_init(&b);
    for (unsigned i = 0; i < 3; ++i) {
        CHECK(b.layout == PbmLayoutUnknown);
        pagani_bm_refresh(&b, 0);
        run_cycle(&b, &f);
    }
    CHECK(b.layout == PbmLayoutOplus && b.layout_assumed && b.undecided_cycles == 3);
    CHECK(pagani_bm_set(&b, PBM_OP_BATTERY_SET, PbmPropOplusChargeEnable, 1, 9) == -2);   /* never on an assumed layout */
    CHECK(pagani_bm_set(&b, PBM_OP_BATTERY_SET, PbmPropChargeControlLimit, 2000000, 9) == 0);
    /* the assumed layout is read (the write first), the probes stay in the cycles */
    clear_asked(&f);
    run_cycle(&b, &f);
    CHECK(f.sets == 1 && f.set_prop[0] == 10);
    CHECK(f.asked_battery[14] == 1 && f.asked_battery[12] >= 1 && f.asked_battery[13] == 1);
    CHECK(b.value[PbmChargeCounter] == 3200000 && !pagani_bm_has(&b, PbmTemperature));   /* 12 fails here */
    /* The firmware turns out to be Qualcomm's after all: the probes decide, what was read with the assumed numbers is dropped. */
    fw_battery(&f, PbmLayoutQti);
    pagani_bm_refresh(&b, 0);
    run_cycle(&b, &f);
    CHECK(b.layout == PbmLayoutQti && !b.layout_assumed);
    CHECK(!pagani_bm_has(&b, PbmChargeCounter) && !pagani_bm_has(&b, PbmCycles));
    run_cycle(&b, &f);
    CHECK(b.value[PbmChargeCounter] == 3200000 && b.value[PbmTemperature] == 2950);
}

static void test_sets(void)
{
    Fw f;
    PbmState b;
    uint8_t request[PBM_REQUEST_BYTES], reply[24];
    fw_battery(&f, PbmLayoutOplus);
    pagani_bm_init(&b);
    /* refused before anything is known: a vendor property, the ship mode, other properties and opcodes */
    CHECK(pagani_bm_set(&b, PBM_OP_BATTERY_SET, PbmPropOplusChargeEnable, 1, 1) == -2);
    CHECK(pagani_bm_set(&b, PBM_OP_BATTERY_SET, 27, 1, 1) == -2);
    CHECK(pagani_bm_set(&b, PBM_OP_BATTERY_SET, 12, 1, 1) == -2);
    CHECK(pagani_bm_set(&b, PBM_OP_USB_SET, 7, 1, 1) == -2);
    CHECK(pagani_bm_set(&b, PBM_OP_BATTERY_GET, 10, 1, 1) == -2);
    CHECK(pagani_bm_set(&b, 0x36, 0, 1, 1) == -2);   /* BC_SHIP_MODE_REQ_SET */
    pagani_bm_refresh(&b, 0);
    run_cycle(&b, &f);
    run_cycle(&b, &f);
    CHECK(b.layout == PbmLayoutOplus && !b.layout_assumed);
    CHECK(pagani_bm_set(&b, PBM_OP_BATTERY_SET, 27, 1, 1) == -2);
    /* a write waits for the field in flight, then goes before the next field */
    pagani_bm_refresh(&b, 0);
    CHECK(pagani_bm_next(&b, request) == 24 && pbat_le32(request + 8) == PBM_OP_BATTERY_GET);
    CHECK(pagani_bm_set(&b, PBM_OP_USB_SET, PbmPropUsbInputLimit, 0xffffffffu, 7) == 0);
    CHECK(pagani_bm_next(&b, reply) == 0);
    fw_answer(&f, request, reply);
    CHECK(pagani_bm_receive(&b, reply, 24) == PbmAnswer);
    CHECK(pagani_bm_next(&b, request) == 24);
    CHECK(pbat_le32(request + 4) == PBM_TYPE_REQUEST && pbat_le32(request + 8) == PBM_OP_USB_SET && pbat_le32(request + 16) == 5);
    CHECK(pbat_le32(request + 20) == 0xffffffffu && b.set_in_flight && pagani_bm_busy(&b));
    fw_answer(&f, request, reply);
    const uint32_t done = b.last_set_done;
    CHECK(pagani_bm_receive(&b, reply, 24) == PbmSetDone);
    CHECK(b.last_set_done == done + 1 && b.last_set.tag == 7 && b.last_set_result == 0 && b.sets_ok == 1 && b.refresh_wanted);
    run_cycle(&b, &f);
    CHECK(b.value[PbmUsbInputLimit] == -1);   /* read back */
    /* the firmware refuses: its code is the result */
    f.set_result = 7;
    CHECK(pagani_bm_set(&b, PBM_OP_BATTERY_SET, PbmPropChargeControlLimit, 2000000, 3) == 0);
    pagani_bm_refresh(&b, 0);
    run_cycle(&b, &f);
    CHECK(b.last_set_result == 7 && b.sets_failed == 1 && b.last_set.property == 10 && b.last_set.value == 2000000);
    /* a write nobody answers times out; its late answer is ignored */
    f.set_result = 0;
    CHECK(pagani_bm_set(&b, PBM_OP_BATTERY_SET, PbmPropOplusChargeEnable, 0, 4) == 0);
    CHECK(pagani_bm_next(&b, request) == 24 && pbat_le32(request + 16) == 24 && pbat_le32(request + 20) == 0);
    const uint32_t timeouts = b.timeouts, ignored = b.ignored;
    pagani_bm_timeout(&b);
    CHECK(b.last_set_result == -60 && b.timeouts == timeouts + 1 && !b.set_in_flight);
    fw_answer(&f, request, reply);
    CHECK(pagani_bm_receive(&b, reply, 24) == PbmIgnored && b.ignored == ignored + 1);
    /* the queue holds four */
    for (unsigned i = 0; i < PBM_PENDING_SETS; ++i) CHECK(pagani_bm_set(&b, PBM_OP_BATTERY_SET, 10, 1000000 + i, 3) == 0);
    CHECK(pagani_bm_set(&b, PBM_OP_BATTERY_SET, 10, 5, 3) == -1 && b.sets_dropped == 1);
    for (unsigned i = 0; i < PBM_PENDING_SETS; ++i) {
        CHECK(pagani_bm_next(&b, request) == 24 && pbat_le32(request + 20) == 1000000 + i);   /* in order */
        fw_answer(&f, request, reply);
        CHECK(pagani_bm_receive(&b, reply, 24) == PbmSetDone);
    }
    run_cycle(&b, &f);   /* the read-back cycle the first write started */
    CHECK(b.value[PbmChargeLimit] == 1000003 && b.refresh_wanted == 0 && b.cursor != 0xff);
    run_cycle(&b, &f);   /* and the one the later writes asked for while it ran */
    CHECK(pagani_bm_next(&b, request) == 0);
    /* a write's answer with another opcode or too short is not its answer */
    CHECK(pagani_bm_set(&b, PBM_OP_USB_SET, 5, 900000, 2) == 0);
    CHECK(pagani_bm_next(&b, request) == 24 && pbat_le32(request + 8) == PBM_OP_USB_SET);
    fw_answer(&f, request, reply);
    pbat_put32(reply + 8, PBM_OP_BATTERY_SET);
    CHECK(pagani_bm_receive(&b, reply, 24) == PbmIgnored && b.set_in_flight);
    pbat_put32(reply + 8, PBM_OP_USB_SET);
    CHECK(pagani_bm_receive(&b, reply, 20) == PbmIgnored && b.set_in_flight);
    CHECK(pagani_bm_receive(&b, reply, 24) == PbmSetDone && !b.set_in_flight && b.last_set.tag == 2);
    CHECK(f.set_op[f.sets - 1] == PBM_OP_USB_SET && f.set_value[f.sets - 1] == 900000 && f.usb[5] == 900000);
}

static void test_messages(void)
{
    Fw f;
    PbmState b;
    uint8_t request[PBM_REQUEST_BYTES], reply[24];
    fw_battery(&f, PbmLayoutOplus);
    pagani_bm_init(&b);
    pagani_bm_refresh(&b, 0);
    CHECK(pagani_bm_next(&b, request) == 24 && pbat_le32(request + 16) == 0);
    /* stray answers leave the request in flight */
    const uint32_t ignored = b.ignored;
    fw_answer(&f, request, reply);
    pbat_put32(reply + 12, 9);
    CHECK(pagani_bm_receive(&b, reply, 24) == PbmIgnored);   /* another property */
    CHECK(pagani_bm_receive(&b, reply, 11) == PbmIgnored);   /* short */
    CHECK(pagani_bm_receive(&b, 0, 24) == PbmIgnored);
    pbat_put32(reply, 32779);
    CHECK(pagani_bm_receive(&b, reply, 24) == PbmIgnored);   /* the port manager */
    fw_answer(&f, request, reply);
    pbat_put32(reply + 8, PBM_OP_USB_GET);
    CHECK(pagani_bm_receive(&b, reply, 24) == PbmIgnored);   /* another opcode */
    CHECK(b.ignored == ignored + 5 && b.in_flight);
    fw_answer(&f, request, reply);
    CHECK(pagani_bm_receive(&b, reply, 20) == PbmIgnored);   /* a property answer is 24 bytes */
    CHECK(pagani_bm_receive(&b, reply, 24) == PbmAnswer);
    /* a notification during the cycle runs another one right after it */
    uint8_t note[16];
    pbat_put32(note, PBM_OWNER); pbat_put32(note + 4, PBM_TYPE_NOTIFY); pbat_put32(note + 8, PBM_OP_NOTIFY); pbat_put32(note + 12, 0x57);
    CHECK(pagani_bm_receive(&b, note, 16) == PbmNotified && b.refresh_wanted == 1 && b.last_notification == 0x57);
    const uint32_t done = b.cycles_done;
    run_cycle(&b, &f);
    CHECK(b.cycles_done == done + 1 && b.cursor != 0xff);   /* the layout's slow cycle or the notified one runs */
    run_cycle(&b, &f);
    if (b.cursor != 0xff) run_cycle(&b, &f);
    CHECK(pagani_bm_next(&b, request) == 0);
    /* idle: a notification starts a cycle at once; a 12-byte one too */
    CHECK(pagani_bm_receive(&b, note, 12) == PbmNotified && b.last_notification == 0 && pagani_bm_next(&b, request) == 24);
    /* the notification enable answer */
    uint8_t enabled[PBM_NOTIFY_REQUEST_BYTES];
    pagani_bm_enable_request(enabled);
    CHECK(pagani_bm_receive(&b, enabled, 24) == PbmEnabled && b.enabled);
    /* an answer when nothing waits */
    pagani_bm_init(&b);
    fw_answer(&f, request, reply);
    CHECK(pagani_bm_receive(&b, reply, 24) == PbmIgnored);
}

static void test_timeouts(void)
{
    Fw f;
    PbmState b;
    uint8_t request[PBM_REQUEST_BYTES];
    fw_battery(&f, PbmLayoutOplus);
    pagani_bm_init(&b);
    pagani_bm_refresh(&b, 1);
    unsigned n = 0;
    while (pagani_bm_next(&b, request) == 24) {
        pagani_bm_timeout(&b);
        CHECK(++n < 64);
        if (b.cursor == 0xff) break;
    }
    CHECK(b.cycles_done == 1 && b.timeouts == n && !b.valid && b.last_timeout >= 0);
    CHECK(b.layout == PbmLayoutUnknown && b.undecided_cycles == 1);
    pagani_bm_timeout(&b);   /* nothing in flight: no effect */
    CHECK(b.timeouts == n);
    /* an error answer is an error, the field stays invalid, the cycle goes on */
    f.battery_fail = 1u << 4;
    pagani_bm_refresh(&b, 0);
    run_cycle(&b, &f);
    CHECK(b.errors == 1 && !pagani_bm_has(&b, PbmCapacity) && pagani_bm_has(&b, PbmVoltage));
}

int main(void)
{
    test_probes();
    test_enable();
    test_oplus();
    test_qti();
    test_assumed();
    test_sets();
    test_messages();
    test_timeouts();
    printf("PASS battery manager: %u checks\n", checks);
    return 0;
}
