/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* Host tests of BatteryCore.c: the whole run against a simulated clock, an NFG8011B gauge and a scripted OPLUS (or Qualcomm) battery
 * manager behind the channel. Covered: the gauge alone (no DSP) with the charger inferred from the current, the guards of what iOS is
 * shown, the gauge lost, the DSP alone, both together, notifications, timeouts, the channel going down, the charging policy at its three
 * levels (no write at all at 0), the thermal limits and the log's size over a day, also with a gauge that comes and goes; the average the
 * gauge carries over a boot (the four runs that showed it); the charger PMIC's state: not read by default, logged only, used. */
#include <string.h>
#include "check.h"
#include "../BatteryCore.h"
#include "../BatteryText.h"

/* ---- the simulated phone ---- */
typedef struct {
    uint64_t now;
    /* gauge */
    uint16_t reg[0x40];
    int gauge_absent;
    unsigned gauge_reads;
    /* battery manager firmware */
    int layout;
    int32_t battery[32], usb[16];
    uint32_t battery_fail, set_result;
    int silent, connected;
    uint8_t replies[64][28];
    uint32_t reply_len[64];
    unsigned nreplies;
    unsigned sent, sets, set_op[64], set_prop[64], set_value[64];
    unsigned enables;
    /* the charger PMIC */
    uint8_t pmic[0x10000];          /* slave 7: page << 8 | offset, the addresses of Android's dumps */
    int pmic_broken;
    unsigned pmic_reads;
    /* iOS */
    PbatSnapshot shown;
    unsigned publishes, firsts;
    /* log */
    unsigned lines, failure_lines, lost_lines, again_lines, summaries, counted;
    char last_line[256], last_state[256];
} Sim;

static Sim sim;

static uint64_t s_now(void *ctx) {(void)ctx; return sim.now;}

static void queue_reply(const uint8_t *data, uint32_t length)
{
    CHECK(sim.nreplies < 64 && length <= 28);
    memcpy(sim.replies[sim.nreplies], data, length);
    sim.reply_len[sim.nreplies++] = length;
}

static int s_send(void *ctx, const uint8_t *data, uint32_t length)
{
    (void)ctx;
    sim.sent++;
    if (!sim.connected) return -4;
    CHECK(length >= 12 && pbat_le32(data) == PBM_OWNER);
    const uint32_t type = pbat_le32(data + 4), opcode = pbat_le32(data + 8);
    if (opcode == PBM_OP_SET_NOTIFY) {
        CHECK(type == PBM_TYPE_NOTIFY && length == PBM_NOTIFY_REQUEST_BYTES);
        sim.enables++;
        uint8_t ack[24] = {0};
        pbat_put32(ack, PBM_OWNER); pbat_put32(ack + 4, PBM_TYPE_REQUEST); pbat_put32(ack + 8, PBM_OP_SET_NOTIFY);
        if (!sim.silent) queue_reply(ack, 24);
        return 0;
    }
    CHECK(type == PBM_TYPE_REQUEST && length == PBM_REQUEST_BYTES && pbat_le32(data + 12) == 0);
    const uint32_t property = pbat_le32(data + 16), value = pbat_le32(data + 20);
    if (opcode == PBM_OP_BATTERY_SET || opcode == PBM_OP_USB_SET) {
        CHECK(sim.sets < 64);
        sim.set_op[sim.sets] = opcode; sim.set_prop[sim.sets] = property; sim.set_value[sim.sets] = value; sim.sets++;
        /* the writes this driver may ever send */
        CHECK((opcode == PBM_OP_USB_SET && property == 5) || (opcode == PBM_OP_BATTERY_SET && (property == 10 || property == 24)));
    }
    if (sim.silent) return 0;
    uint8_t out[24];
    pbat_put32(out, PBM_OWNER); pbat_put32(out + 4, PBM_TYPE_REQUEST); pbat_put32(out + 8, opcode); pbat_put32(out + 12, property);
    uint32_t answer = 0, result = 0;
    if (opcode == PBM_OP_BATTERY_GET) {
        CHECK(property < 32);
        answer = (uint32_t)sim.battery[property];
        result = (sim.battery_fail >> property) & 1u;
    } else if (opcode == PBM_OP_USB_GET) {
        CHECK(property < 16);
        answer = (uint32_t)sim.usb[property];
    } else {
        result = sim.set_result;
        answer = value;
        if (!result) {
            if (opcode == PBM_OP_USB_SET) sim.usb[5] = value == 0xffffffffu ? 1500000 : (int32_t)value;   /* the firmware's own limit */
            else sim.battery[property] = (int32_t)value;
        }
    }
    pbat_put32(out + 16, answer);
    pbat_put32(out + 20, result);
    queue_reply(out, 24);
    return 0;
}

static void s_publish(void *ctx, const PbatSnapshot *s, int first)
{
    (void)ctx;
    sim.shown = *s;
    sim.publishes++;
    if (first) sim.firsts++;
    CHECK(s->percent >= 0 && s->percent <= 100);
}

static void s_trace(void *ctx, const char *line)
{
    (void)ctx;
    sim.lines++;
    CHECK(!strncmp(line, "[battery] ", 10) && strlen(line) < 224);
    if (strstr(line, "FAIL") || strstr(line, "stopped")) sim.failure_lines++;
    if (strstr(line, "stopped answering")) sim.lost_lines++;
    if (strstr(line, "answers again")) sim.again_lines++;
    if (!strncmp(line, "[battery] state summary", 23)) sim.summaries++;
    if (!strncmp(line, "[battery] state ", 16) && strstr(line, " fail=")) sim.counted++;
    if (!strncmp(line, "[battery] state ", 16)) strncpy(sim.last_state, line, sizeof sim.last_state - 1);
    strncpy(sim.last_line, line, sizeof sim.last_line - 1);
}

static int g_read16(void *ctx, uint8_t reg, uint16_t *value)
{
    (void)ctx;
    sim.gauge_reads++;
    if (sim.gauge_absent) return -6;
    *value = sim.reg[reg & 0x3f];
    return 0;
}

static int s_pmic(void *ctx, unsigned ppid, uint8_t offset, uint8_t *value)
{
    (void)ctx;
    sim.pmic_reads++;
    if (sim.pmic_broken) return -110;
    CHECK(ppid == 0x726 || ppid == 0x727 || ppid == 0x729 || ppid == 0x72b);   /* the charger's peripherals, and nothing else */
    *value = sim.pmic[(ppid & 0xffu) << 8 | offset];
    return 0;
}

static PbatOps ops = {0, s_now, s_send, s_publish, s_trace, {0, g_read16, 0, 0}, 0};
static PbatOps ops_pmic = {0, s_now, s_send, s_publish, s_trace, {0, g_read16, 0, 0}, s_pmic};

/* The battery: 29.5 C, 87 %, 3.95 V, -812 mA, 3200 of 6100 mAh, 37 cycles, 6 h left; the firmware sees the same. */
static void sim_reset(int layout)
{
    memset(&sim, 0, sizeof sim);
    sim.now = 1000;
    sim.reg[0x08] = 3950; sim.reg[0x0c] = (uint16_t)(int16_t)-812; sim.reg[0x14] = (uint16_t)(int16_t)-800;
    sim.reg[0x06] = 2731 + 295; sim.reg[0x2c] = 87; sim.reg[0x0a] = 1; sim.reg[0x10] = 3200; sim.reg[0x12] = 6100;
    sim.reg[0x16] = 360; sim.reg[0x18] = 0xffff; sim.reg[0x2a] = 37; sim.reg[0x2e] = 98;
    sim.layout = layout;
    const int o = layout == PbmLayoutQti;
    sim.battery[0] = 2; sim.battery[1] = 1; sim.battery[2] = 1; sim.battery[3] = 1; sim.battery[4] = 8750;
    sim.battery[7] = 3950000; sim.battery[9] = 812000; sim.battery[10] = 3000000;
    if (o) sim.battery[12] = 2000000;
    sim.battery[12 + o] = 2950; sim.battery[13 + o] = 2; sim.battery[14 + o] = 3200000; sim.battery[15 + o] = 37;
    sim.battery[16 + o] = 6260000; sim.battery[17 + o] = 6100000; sim.battery[19 + o] = -1; sim.battery[20 + o] = 21600;
    if (!o) sim.battery[24] = 1;
    sim.usb[5] = 1500000;
    sim.connected = 1;
}

static void set_temperature(int32_t centi) {sim.reg[0x06] = (uint16_t)(2731 + centi / 10); sim.battery[sim.layout == PbmLayoutQti ? 13 : 12] = centi;}
static void set_current(int32_t ma) {sim.reg[0x0c] = (uint16_t)(int16_t)ma; sim.reg[0x14] = (uint16_t)(int16_t)ma; sim.battery[9] = -ma * 1000;}

/* Runs the core until `ms` of simulated time passed, delivering the firmware's answers as they come. */
static void run_for(PbatCore *c, uint64_t ms)
{
    const uint64_t until = sim.now + ms;
    unsigned guard = 0;
    for (;;) {
        CHECK(++guard < 1000000);
        while (sim.nreplies) {
            uint8_t message[28];
            const uint32_t length = sim.reply_len[0];
            memcpy(message, sim.replies[0], length);
            memmove(sim.replies, sim.replies + 1, --sim.nreplies * sizeof sim.replies[0]);
            memmove(sim.reply_len, sim.reply_len + 1, sim.nreplies * sizeof sim.reply_len[0]);
            pagani_battery_core_message(c, message, length);
        }
        const uint32_t sleep = pagani_battery_core_run(c);
        CHECK(sleep >= 1 && sleep <= 60000);
        if (sim.nreplies) continue;
        if (sim.now >= until) break;
        sim.now += sleep < until - sim.now ? sleep : until - sim.now;
    }
}

static void notify(PbatCore *c, uint32_t what)
{
    uint8_t note[16];
    pbat_put32(note, PBM_OWNER); pbat_put32(note + 4, PBM_TYPE_NOTIFY); pbat_put32(note + 8, PBM_OP_NOTIFY); pbat_put32(note + 12, what);
    pagani_battery_core_message(c, note, 16);
}

static PbatConfig config(int gauge, int adsp, int charge)
{
    PbatConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.use_gauge = (uint32_t)gauge;
    cfg.use_adsp = (uint32_t)adsp;
    cfg.charge = (uint32_t)charge;
    cfg.sdp_ma = 900;
    cfg.design_mah = 6260;
    cfg.log = 1;
    return cfg;
}

static unsigned writes_of(uint32_t opcode, uint32_t property, uint32_t value)
{
    unsigned n = 0;
    for (unsigned i = 0; i < sim.sets; ++i)
        if (sim.set_op[i] == opcode && sim.set_prop[i] == property && sim.set_value[i] == value) n++;
    return n;
}

/* ---- the gauge alone ---- */
static void test_gauge_alone(void)
{
    static PbatCore c;
    sim_reset(PbmLayoutOplus);
    PbatConfig cfg = config(1, 0, 0);
    pagani_battery_core_init(&c, &ops, &cfg);
    CHECK(pagani_battery_core_run(&c) == 60000 && !sim.publishes);   /* no source yet */
    pagani_battery_core_gauge(&c, 1);
    run_for(&c, 1);
    CHECK(sim.firsts == 1 && sim.publishes == 1);
    const PbatSnapshot *s = &sim.shown;
    CHECK(s->percent == 87 && s->present && !s->external && !s->charging && !s->fully_charged && !s->critical);
    CHECK(s->voltage_mv == 3950 && s->current_ma == -800 && s->instant_ma == -812 && s->temperature_centi == 2950 && s->temperature_valid);
    /* 6100 of 6260 mAh: 97.4 %, rounded up as powerd rounds its nominal capacity percentage */
    CHECK(s->raw_mah == 3200 && s->full_mah == 6100 && s->design_mah == 6260 && s->cycles == 37 && s->cycles_known && s->health_percent == 98);
    CHECK(s->time_remaining_min == 360 && s->adapter_family == 0 && s->sources == (PbatSrcGauge | PbatSrcInferred));
    CHECK(!sim.sent);   /* no DSP: nothing is sent */
    /* the gauge period without the DSP is 10 s */
    const unsigned reads = sim.gauge_reads;
    run_for(&c, 10000);
    CHECK(sim.gauge_reads > reads);
    /* a charger: the battery takes 1.5 A (two passes agree, the second 2 s after the first: within 12 s) */
    set_current(1500);
    sim.reg[0x18] = 45;
    run_for(&c, 12500);
    CHECK(s->external && s->charging && !s->fully_charged && s->current_ma == 1500 && s->time_remaining_min == 45);
    CHECK(s->adapter_family == 0xe0004007u && s->adapter_watts == 0 && c.inference == PbatInfCharging);
    /* charged: the charger holds it at 100 % (about no current; the status bits are not used, whatever they say) */
    sim.reg[0x2c] = 100;
    sim.reg[0x0a] = 0;
    set_current(5);
    run_for(&c, 12500);
    CHECK(s->external && !s->charging && s->fully_charged && s->percent == 100 && c.inference == PbatInfFull);
    /* unplugged: the phone runs from the battery again */
    set_current(-400);
    run_for(&c, 12500);
    CHECK(!s->external && !s->charging && !s->fully_charged && c.inference == PbatInfNone);
    /* an idle phone at 100 % without a charger stays unplugged (no charging seen) */
    set_current(-10);
    sim.reg[0x0a] = PGAUGE_FLAG_FC | PGAUGE_STATUS_FC;
    run_for(&c, 30000);
    CHECK(!s->external && c.inference == PbatInfNone);

    /* the guards: 0 % above 3450 mV shows 1 %; the critical level needs the voltage to agree */
    sim.reg[0x0a] = 1;
    sim.reg[0x2c] = 0;
    sim.reg[0x08] = 3700;
    run_for(&c, 10000);
    CHECK(s->percent == 1 && !s->critical && c.soc_guarded >= 1);
    sim.reg[0x2c] = 2;
    sim.reg[0x08] = 3600;
    run_for(&c, 10000);
    CHECK(s->percent == 2 && !s->critical);
    sim.reg[0x08] = 3400;
    run_for(&c, 10000);
    CHECK(s->percent == 2 && s->critical);
    sim.reg[0x2c] = 0;
    sim.reg[0x08] = 3300;
    run_for(&c, 10000);
    CHECK(s->percent == 0 && s->critical);
    /* a charger at 0 %: not critical */
    set_current(800);
    run_for(&c, 12500);
    CHECK(s->external && s->charging && !s->critical);
    set_current(-812);
    sim.reg[0x2c] = 87;
    sim.reg[0x08] = 3950;
    run_for(&c, 12500);
    CHECK(!s->external && c.inference == PbatInfNone);

    /* the gauge stops answering: after three passes it is lost, iOS keeps the last values, one line says so */
    const unsigned published = sim.publishes, failures = sim.failure_lines;
    sim.gauge_absent = 1;
    run_for(&c, 35000);
    CHECK(!c.gauge_answering && c.gauge_lost == 1 && sim.failure_lines == failures + 1);
    CHECK(sim.publishes == published && sim.shown.percent == 87);
    int32_t words[16];
    pagani_battery_core_export(&c, words);
    CHECK(!(words[5] & 1 << 5) && words[1] == 87 && words[7] > 0);
    /* asked less often while it stays silent: 20, 40, 80 ... s, at most 5 min */
    const unsigned silent_reads = sim.gauge_reads;
    run_for(&c, 600000);
    CHECK(sim.gauge_reads - silent_reads <= 6 * PGAUGE_TRIES && c.gauge_due - sim.now <= 300000);   /* about five passes, three tries each */
    sim.gauge_absent = 0;
    run_for(&c, 300000);
    CHECK(c.gauge_answering && strstr(sim.last_line, "answers again"));
    run_for(&c, 10000);
    CHECK(c.gauge_due - sim.now <= 10000);   /* back to its period */

    /* a gauge that never answers: nothing is ever shown */
    static PbatCore d;
    sim_reset(PbmLayoutOplus);
    sim.gauge_absent = 1;
    pagani_battery_core_init(&d, &ops, &cfg);
    pagani_battery_core_gauge(&d, 1);
    run_for(&d, 120000);
    CHECK(!sim.publishes && !d.published);
    pagani_battery_core_export(&d, words);
    CHECK(words[1] == -1 && !(words[5] & 1 << 7));
}

/* ---- the DSP and the gauge ---- */
static void test_dsp(void)
{
    static PbatCore c;
    sim_reset(PbmLayoutOplus);
    PbatConfig cfg = config(1, 1, 0);
    pagani_battery_core_init(&c, &ops, &cfg);
    pagani_battery_core_gauge(&c, 1);
    run_for(&c, 1);
    CHECK(sim.firsts == 1 && !(sim.shown.sources & PbatSrcCharger));
    pagani_battery_core_channel(&c, 1);
    CHECK(sim.enables == 1);
    run_for(&c, 100);
    CHECK(c.bm.layout == PbmLayoutOplus && !c.bm.layout_assumed && c.bm.cycles_done >= 2 && c.bm.enabled);
    const PbatSnapshot *s = &sim.shown;
    CHECK(s->sources == (PbatSrcGauge | PbatSrcCharger) && !s->external && s->percent == 87);
    /* a PD charger: 9 V, 2.6 A in, the battery charging */
    sim.usb[0] = 1; sim.usb[6] = 0; sim.usb[7] = 6; sim.usb[1] = 9000000; sim.usb[3] = -2600000;
    sim.battery[0] = 1;
    set_current(2400);
    notify(&c, 0x57);
    run_for(&c, 2000);
    CHECK(s->external && s->charging && !s->fully_charged && s->current_ma == 2400);
    CHECK(s->adapter_family == 0xe000400au && s->adapter_mv == 9000 && s->adapter_ma == 2600 && s->adapter_watts == 23);
    /* full */
    sim.battery[0] = 4;
    set_current(0);
    sim.reg[0x2c] = 100;
    notify(&c, 0x30);
    run_for(&c, 2000);
    CHECK(s->external && !s->charging && s->fully_charged && s->percent == 100);
    /* a dedicated charging port */
    sim.usb[7] = 2;
    sim.battery[0] = 1;
    set_current(1000);
    notify(&c, 0x32);
    run_for(&c, 2000);
    CHECK(s->adapter_family == 0xe0004004u && s->charging);
    /* unplugged */
    sim.usb[0] = 0;
    sim.battery[0] = 2;
    set_current(-500);
    notify(&c, 0x32);
    run_for(&c, 2000);
    CHECK(!s->external && !s->charging && s->adapter_family == 0);
    /* charge 0: in all of this not one write */
    CHECK(sim.sets == 0);
    /* the channel goes down: the gauge carries on, the charger inferred from it */
    pagani_battery_core_channel(&c, 0);
    run_for(&c, 25000);
    CHECK(!(s->sources & PbatSrcCharger) && (s->sources & PbatSrcInferred) && s->percent == 100);
    set_current(1200);
    run_for(&c, 25000);
    CHECK(s->external && s->charging);
    /* back up: the enable request again, a fresh cycle, the charger's view again */
    sim.usb[0] = 1;
    sim.battery[0] = 1;
    pagani_battery_core_channel(&c, 1);
    run_for(&c, 100);
    CHECK(sim.enables == 2 && (s->sources & PbatSrcCharger) && s->external && c.bm.layout == PbmLayoutOplus);
    int32_t words[16];
    pagani_battery_core_export(&c, words);
    CHECK((words[5] & 1 << 6) && ((words[5] >> 8) & 3) == PbmLayoutOplus && (words[5] & 1 << 11) && words[8] > 0 && words[9] > 0);
}

static void test_dsp_alone(void)
{
    static PbatCore c;
    sim_reset(PbmLayoutOplus);
    PbatConfig cfg = config(0, 1, 0);
    pagani_battery_core_init(&c, &ops, &cfg);
    pagani_battery_core_gauge(&c, 1);   /* refused: the gauge is off by configuration */
    CHECK(!c.gauge_available);
    pagani_battery_core_channel(&c, 1);
    run_for(&c, 100);
    const PbatSnapshot *s = &sim.shown;
    CHECK(sim.firsts == 1 && s->sources == (PbatSrcBattmgr | PbatSrcCharger) && !sim.gauge_reads);
    CHECK(s->percent == 88 && s->voltage_mv == 3950 && s->current_ma == -812 && s->temperature_centi == 2950);
    CHECK(s->raw_mah == 3200 && s->full_mah == 6100 && s->design_mah == 6260 && s->cycles == 37 && s->time_remaining_min == 360);
    /* the Qualcomm numbering: the same values from other properties */
    static PbatCore q;
    sim_reset(PbmLayoutQti);
    pagani_battery_core_init(&q, &ops, &cfg);
    pagani_battery_core_channel(&q, 1);
    run_for(&q, 100);
    CHECK(q.bm.layout == PbmLayoutQti && s->temperature_centi == 2950 && s->raw_mah == 3200 && s->cycles == 37 && s->full_mah == 6100);
    /* before the layout is known, nothing that depends on it is used */
    static PbatCore u;
    sim_reset(PbmLayoutOplus);
    sim.battery_fail = 1u << 12 | 1u << 13;
    pagani_battery_core_init(&u, &ops, &cfg);
    pagani_battery_core_channel(&u, 1);
    run_for(&u, 30);
    CHECK(sim.firsts == 1 && s->percent == 88 && !s->temperature_valid && s->design_mah == 6260 && s->raw_mah == 0);
    /* a silent firmware: every field times out after 2 s, the core keeps going and never waits longer */
    static PbatCore t;
    sim_reset(PbmLayoutOplus);
    sim.silent = 1;
    pagani_battery_core_init(&t, &ops, &cfg);
    pagani_battery_core_channel(&t, 1);
    unsigned rounds = 0;
    uint32_t sleep = 0;
    for (;;) {
        sleep = pagani_battery_core_run(&t);
        if (t.bm.cycles_done) break;
        CHECK(sleep <= 2000 && pagani_bm_busy(&t.bm) && ++rounds < 100);   /* a request waits, so does the core: 2 s at most */
        sim.now += sleep;
    }
    CHECK(t.bm.timeouts == 18 && rounds == 18 && !sim.publishes && !pagani_bm_busy(&t.bm));   /* the 18 fields of a first cycle */
    CHECK(sleep > 2000 && sleep <= 60000);   /* then the refresh period */
}

/* ---- the charging policy ---- */
static void plug(PbatCore *c, int port, int32_t limit)
{
    sim.usb[0] = 1; sim.usb[7] = port; sim.usb[6] = port; sim.usb[5] = limit;
    sim.battery[0] = 1;
    set_current(1000);
    notify(c, 0x57);
}

static void test_policy(void)
{
    static PbatCore c;
    sim_reset(PbmLayoutOplus);
    PbatConfig cfg = config(1, 1, 1);
    pagani_battery_core_init(&c, &ops, &cfg);
    CHECK(c.cfg.fcc_ma == 2000);
    pagani_battery_core_gauge(&c, 1);
    pagani_battery_core_channel(&c, 1);
    run_for(&c, 100);
    CHECK(sim.sets == 0);   /* nothing while unplugged */
    /* a charger that left the input suspended: lifted once; then the charge current limit */
    plug(&c, 6, 0);
    run_for(&c, 3000);
    CHECK(writes_of(PBM_OP_USB_SET, 5, 0xffffffffu) == 1 && sim.usb[5] == 1500000);
    CHECK(writes_of(PBM_OP_BATTERY_SET, 10, 2000000) == 1 && c.applied_limit == 2000000);
    CHECK(writes_of(PBM_OP_BATTERY_SET, 24, 1) == 0 && writes_of(PBM_OP_BATTERY_SET, 24, 0) == 0);
    run_for(&c, 120000);
    CHECK(sim.sets == 2);   /* settled */
    /* hot: 500 mA from 45 C; 43 C keeps it (hysteresis); 41 C goes back */
    set_temperature(4600);
    run_for(&c, 61000);
    CHECK(writes_of(PBM_OP_BATTERY_SET, 10, 500000) == 1 && c.thermal == 1 && c.applied_limit == 500000);
    set_temperature(4300);
    run_for(&c, 61000);
    CHECK(c.thermal == 1 && writes_of(PBM_OP_BATTERY_SET, 10, 2000000) == 1);
    set_temperature(4100);
    run_for(&c, 61000);
    CHECK(c.thermal == 0 && writes_of(PBM_OP_BATTERY_SET, 10, 2000000) == 2);
    /* cold below 5 C too */
    set_temperature(300);
    run_for(&c, 61000);
    CHECK(c.thermal == 1 && writes_of(PBM_OP_BATTERY_SET, 10, 500000) == 2);
    set_temperature(2500);
    run_for(&c, 61000);
    CHECK(c.thermal == 0);
    /* charge 1 never pauses charging itself, even at 52 C */
    set_temperature(5200);
    run_for(&c, 61000);
    CHECK(c.thermal == 1 && writes_of(PBM_OP_BATTERY_SET, 24, 0) == 0);
    set_temperature(2500);
    run_for(&c, 61000);
    /* a computer's port at 100 mA: 900 mA asked for; a charging port: 1.5 A */
    sim.usb[0] = 0;
    notify(&c, 0x57);
    run_for(&c, 2000);
    plug(&c, 1, 100000);
    run_for(&c, 3000);
    CHECK(writes_of(PBM_OP_USB_SET, 5, 900000) == 1 && sim.usb[5] == 900000);
    sim.usb[0] = 0;
    notify(&c, 0x57);
    run_for(&c, 2000);
    plug(&c, 3, 500000);
    run_for(&c, 3000);
    CHECK(writes_of(PBM_OP_USB_SET, 5, 1500000) == 1);
    /* a firmware that refuses: three tries per connection, no more */
    sim.usb[0] = 0;
    notify(&c, 0x57);
    run_for(&c, 2000);
    sim.set_result = 3;
    const unsigned before = writes_of(PBM_OP_BATTERY_SET, 10, 2000000);
    plug(&c, 6, 1500000);
    run_for(&c, 600000);
    CHECK(writes_of(PBM_OP_BATTERY_SET, 10, 2000000) == before + 3);
    sim.set_result = 0;
    CHECK(writes_of(PBM_OP_BATTERY_SET, 24, 1) == 0);
}

static void test_policy_enable(void)
{
    static PbatCore c;
    sim_reset(PbmLayoutOplus);
    sim.battery[24] = 0;   /* the firmware has charging off */
    PbatConfig cfg = config(1, 1, 2);
    cfg.fcc_ma = 3000;
    pagani_battery_core_init(&c, &ops, &cfg);
    pagani_battery_core_gauge(&c, 1);
    pagani_battery_core_channel(&c, 1);
    run_for(&c, 100);
    plug(&c, 6, 1500000);
    run_for(&c, 3000);
    CHECK(writes_of(PBM_OP_BATTERY_SET, 10, 3000000) == 1 && writes_of(PBM_OP_BATTERY_SET, 24, 1) == 1 && sim.battery[24] == 1);
    /* 51 C: charging off and the limit down; 46 C: on again, the limit still reduced; 41 C: the limit back */
    set_temperature(5100);
    run_for(&c, 61000);
    run_for(&c, 61000);
    CHECK(c.thermal == 2 && writes_of(PBM_OP_BATTERY_SET, 24, 0) == 1 && sim.battery[24] == 0 && c.applied_limit == 500000);
    set_temperature(4600);
    run_for(&c, 61000);
    run_for(&c, 61000);
    CHECK(c.thermal == 1 && writes_of(PBM_OP_BATTERY_SET, 24, 1) == 2 && sim.battery[24] == 1);
    set_temperature(4100);
    run_for(&c, 61000);
    CHECK(c.thermal == 0 && writes_of(PBM_OP_BATTERY_SET, 10, 3000000) == 2);
    /* below 0 C: off as well */
    set_temperature(-200);
    run_for(&c, 61000);
    run_for(&c, 61000);
    CHECK(c.thermal == 2 && sim.battery[24] == 0);

    /* the layout only assumed: never BATT_CHG_EN */
    static PbatCore a;
    sim_reset(PbmLayoutOplus);
    sim.battery[24] = 0;
    sim.battery_fail = 1u << 12 | 1u << 13;
    pagani_battery_core_init(&a, &ops, &cfg);
    pagani_battery_core_gauge(&a, 1);
    pagani_battery_core_channel(&a, 1);
    run_for(&a, 1000);
    plug(&a, 6, 1500000);
    run_for(&a, 600000);
    CHECK(a.bm.layout_assumed && writes_of(PBM_OP_BATTERY_SET, 24, 1) == 0 && writes_of(PBM_OP_BATTERY_SET, 10, 3000000) == 1);
    /* the Qualcomm layout: never BATT_CHG_EN either */
    static PbatCore q;
    sim_reset(PbmLayoutQti);
    pagani_battery_core_init(&q, &ops, &cfg);
    pagani_battery_core_gauge(&q, 1);
    pagani_battery_core_channel(&q, 1);
    run_for(&q, 1000);
    plug(&q, 6, 1500000);
    run_for(&q, 600000);
    CHECK(q.bm.layout == PbmLayoutQti && writes_of(PBM_OP_BATTERY_SET, 24, 1) == 0 && writes_of(PBM_OP_BATTERY_SET, 24, 0) == 0);
}

/* ---- the log over a day ---- */
static void test_log(void)
{
    static PbatCore c;
    sim_reset(PbmLayoutOplus);
    PbatConfig cfg = config(1, 0, 0);
    pagani_battery_core_init(&c, &ops, &cfg);
    pagani_battery_core_gauge(&c, 1);
    /* a day of discharge from 100 % to 5 %, with a charge in the middle */
    for (int hour = 0; hour < 24; ++hour) {
        sim.reg[0x2c] = (uint16_t)(100 - hour * 4);
        set_current(hour >= 10 && hour < 12 ? 1500 : -500);
        run_for(&c, 3600000);
    }
    CHECK(sim.lines < 40);   /* the ramoops ring (32 KiB) is every driver's: a day stays a few KiB */
    const unsigned day = sim.lines;
    /* log 0: the start and failures only */
    static PbatCore q;
    sim_reset(PbmLayoutOplus);
    cfg.log = 0;
    pagani_battery_core_init(&q, &ops, &cfg);
    pagani_battery_core_gauge(&q, 1);
    run_for(&q, 3600000);
    CHECK(sim.lines == 0);
    sim.gauge_absent = 1;
    run_for(&q, 60000);
    CHECK(sim.lines == 2 && sim.failure_lines == 1);
    printf("log lines in a simulated day (log 1): %u\n", day);
}

/* A day with a gauge that comes and goes (a marginal bus, a gauge busy with its own updates): every other pass fails (pattern 1), or
 * three passes fail and one answers (pattern 3, lost and found again each time). Each failing run used to log its first pass and each
 * loss and recovery a line of its own, 4000 to 5000 lines a day, which would push every other driver's lines out of the 32 KiB event
 * ring. Runs at the given log level; returns the lines logged. */
static unsigned flapping_day(PbatCore *c, uint32_t log, int pattern)
{
    sim_reset(PbmLayoutOplus);
    PbatConfig cfg = config(1, 0, 0);
    cfg.log = log;
    pagani_battery_core_init(c, &ops, &cfg);
    pagani_battery_core_gauge(c, 1);
    const uint64_t end = sim.now + 24ull * 3600 * 1000;
    while (sim.now < end) {
        const uint32_t pass = c->gauge_passes;   /* the next one */
        sim.gauge_absent = pattern == 1 ? pass % 2 == 1 : pass % 4 != 3;
        set_current(-800 - (int32_t)(pass % 7));   /* a real gauge's current is never quite the same */
        const uint32_t sleep = pagani_battery_core_run(c);
        CHECK(sleep >= 1 && sleep <= 60000);
        sim.now += sleep;
    }
    return sim.lines;
}

static void test_log_flapping(void)
{
    static PbatCore c;
    unsigned lines = flapping_day(&c, 1, 1);
    CHECK(c.gauge_passes > 8000 && c.gauge_passes - c.gauge_ok_passes > 4000 && !c.gauge_lost);   /* never three in a row */
    CHECK(lines < 40 && c.fail_lines == 8 && sim.summaries >= 10 && sim.counted == sim.summaries);
    CHECK(strstr(sim.last_line, "[battery] state summary") && strstr(sim.last_line, " fail="));
    const unsigned every_other = lines;
    lines = flapping_day(&c, 1, 3);
    CHECK(c.gauge_lost > 1000 && c.fail_lines == 8 && sim.lost_lines == 4 && sim.again_lines == 4);
    CHECK(lines < 40 && sim.summaries >= 10 && strstr(sim.last_line, " lost="));
    const unsigned three_one = lines;
    /* log 0: the first four losses (each with its gauge line) and recoveries, nothing after */
    lines = flapping_day(&c, 0, 3);
    CHECK(c.gauge_lost > 1000 && lines == 3 * 4 && sim.lost_lines == 4 && sim.again_lines == 4 && !sim.summaries);
    /* log 2 (bring-up only) still has every pass */
    lines = flapping_day(&c, 2, 3);
    CHECK(lines > c.gauge_passes && sim.lost_lines == c.gauge_lost);
    printf("log lines in a simulated day with a gauge that comes and goes (log 1): %u every other pass, %u three of four\n", every_other,
           three_one);
}

/* ---- what the gauge's average carries over a boot ---- */
static void boot_with(PbatCore *c, int32_t i, int32_t a)
{
    sim_reset(PbmLayoutOplus);
    sim.reg[0x0c] = (uint16_t)(int16_t)i;
    sim.reg[0x14] = (uint16_t)(int16_t)a;
    PbatConfig cfg = config(1, 0, 0);
    pagani_battery_core_init(c, &ops, &cfg);
    pagani_battery_core_gauge(c, 1);
}
static void gauge_now(int32_t i, int32_t a)
{
    sim.reg[0x0c] = (uint16_t)(int16_t)i;
    sim.reg[0x14] = (uint16_t)(int16_t)a;
}

static void test_residual(void)
{
    static PbatCore c;
    /* the four runs of round 1101: (instant, average) of the passes at about 2 s and 4 s; the third is the one that showed a charger */
    static const int32_t runs[4][4] = {{0, 68, -294, 27}, {0, 87, -256, 43}, {0, 94, -124, 72}, {0, 57, -367, -367}};
    for (unsigned r = 0; r < 4; ++r) {
        boot_with(&c, runs[r][0], runs[r][1]);
        run_for(&c, 2000);
        gauge_now(runs[r][2], runs[r][3]);
        run_for(&c, 2000);
        CHECK(sim.firsts == 1 && !sim.shown.external && !sim.shown.charging && c.inference == PbatInfNone && !c.inference_votes);
        /* and on to the end of the settling, the average falling as the filter forgets */
        for (int32_t a = runs[r][3]; a > -300; a -= 20) {
            gauge_now(runs[r][2], a);
            run_for(&c, 2000);
            CHECK(!sim.shown.external && c.inference == PbatInfNone);
        }
    }
    /* the worst of it: a 2 A charge just before the boot, about 14 s of filter, an idle phone. Never a charger */
    boot_with(&c, 0, 2000);
    for (unsigned s = 0; s < 120; ++s) {
        int32_t a = 2000;
        for (unsigned k = 0; k < s; ++k) a = a * 13 / 14;
        gauge_now(-10, a);
        run_for(&c, 1000);
        CHECK(!sim.shown.external);
    }
    /* a real charger at boot still shows within seconds: the instant reading is enough (the first reading of a boot has been 0 on every
     * run so far, and the average no longer asks for the quick second pass, so the gauge's own 10 s and the 2 s confirmation) */
    boot_with(&c, 0, 90);
    run_for(&c, 2000);
    gauge_now(800, 300);
    run_for(&c, 10500);
    CHECK(sim.shown.external && sim.shown.charging && c.inference == PbatInfCharging);
    /* past the settling the average counts as it always did: the instant reading between bursts of a charging phone */
    boot_with(&c, -400, -400);
    run_for(&c, 61000);
    gauge_now(-30, 120);
    run_for(&c, 25000);
    CHECK(sim.shown.external && sim.shown.charging && c.inference == PbatInfCharging);
    /* ... and within it the same readings do not, until it ends */
    boot_with(&c, -30, 120);
    run_for(&c, 55000);
    CHECK(!sim.shown.external && c.inference == PbatInfNone);
    run_for(&c, 20000);
    CHECK(sim.shown.external && sim.shown.charging);
}

/* ---- the charger PMIC's state ---- */
static void pmic_set(uint8_t rt, uint8_t status, uint8_t limit)
{
    sim.pmic[0x2910] = rt;
    sim.pmic[0x2606] = status;
    sim.pmic[0x2952] = limit;
}
static void pmic_samples(void)
{
    /* with VBUS as Android dumps it (0x2906 0x02, 0x2908 0x0f, 0x2b06 0x02, 0x2b0b 0x2b, 0x2706 and 0x2709 at the limit) */
    sim.pmic[0x2906] = 0x02; sim.pmic[0x2908] = 0x0f; sim.pmic[0x2b06] = 0x02; sim.pmic[0x2b0b] = 0x2b;
    sim.pmic[0x2706] = 0x28; sim.pmic[0x2709] = 0x28;
}

static void test_pmic_off(void)
{
    /* the default: not one read, the same lines as before, also with a provider at hand */
    static PbatCore c;
    sim_reset(PbmLayoutOplus);
    PbatConfig cfg = config(1, 0, 0);
    pagani_battery_core_init(&c, &ops_pmic, &cfg);
    pagani_battery_core_gauge(&c, 1);
    run_for(&c, 600000);
    CHECK(!sim.pmic_reads && !strstr(sim.last_line, "pmic"));
    /* no provider (the channels were not found): the switch does nothing */
    cfg.pmic = 2;
    sim_reset(PbmLayoutOplus);
    pagani_battery_core_init(&c, &ops, &cfg);
    pagani_battery_core_gauge(&c, 1);
    run_for(&c, 30000);
    CHECK(sim.shown.sources == (PbatSrcGauge | PbatSrcInferred));
    /* the switches of the boot argument */
    CHECK(pagani_battery_mode(0) == 0 && pagani_battery_mode(1) == 1 && pagani_battery_mode(3) == 3 && pagani_battery_mode(15) == 15);
    CHECK(pagani_battery_mode(2) == 0 && pagani_battery_mode(4) == 0 && pagani_battery_mode(8) == 0 && pagani_battery_mode(14) == 0);
    CHECK(pagani_battery_mode(16) == 0 && pagani_battery_mode(17) == 0 && pagani_battery_mode(0xffffffffu) == 0);
    CHECK(pagani_battery_mode_pmic(1) == 0 && pagani_battery_mode_pmic(3) == 0 && pagani_battery_mode_pmic(5) == 1 && pagani_battery_mode_pmic(7) == 1);
    CHECK(pagani_battery_mode_pmic(9) == 2 && pagani_battery_mode_pmic(13) == 2 && pagani_battery_mode_pmic(15) == 2);
}

static void test_pmic_log(void)
{
    /* logged only: what iOS is shown is what the gauge says, whatever the charger reads */
    static PbatCore c, d;
    sim_reset(PbmLayoutOplus);
    pmic_samples();
    pmic_set(0x11, 0x83, 0x28);
    PbatConfig cfg = config(1, 0, 0);
    PbatConfig logged = cfg;
    logged.pmic = 1;
    pagani_battery_core_init(&c, &ops_pmic, &logged);
    pagani_battery_core_gauge(&c, 1);
    run_for(&c, 1);
    CHECK(sim.pmic_reads == PbsChargerRegs && c.pmic_seen && c.pmic_plug);
    CHECK(strstr(sim.last_line, "[battery] state published") || strstr(sim.last_line, "[battery] pmic first"));
    run_for(&c, 30000);
    CHECK(!sim.shown.external && !sim.shown.charging && sim.shown.sources == (PbatSrcGauge | PbatSrcInferred));
    /* the same run without it shows exactly the same */
    PbatSnapshot with = sim.shown;
    sim_reset(PbmLayoutOplus);
    pagani_battery_core_init(&d, &ops, &cfg);
    pagani_battery_core_gauge(&d, 1);
    run_for(&d, 1);
    run_for(&d, 30000);
    CHECK(!memcmp(&with, &sim.shown, sizeof with));
    /* the cadence: every 2 s, the thread woken for it */
    sim_reset(PbmLayoutOplus);
    pmic_samples();
    pmic_set(0x04, 0x07, 0);
    pagani_battery_core_init(&c, &ops_pmic, &logged);
    pagani_battery_core_gauge(&c, 1);
    run_for(&c, 1);
    const unsigned at = sim.pmic_reads;
    run_for(&c, 20000);
    CHECK(sim.pmic_reads - at == 10 * PbsChargerRegs);
    /* the lines: first, then each change; nothing while nothing moves, without VBUS no trend */
    const unsigned quiet = sim.lines;
    run_for(&c, 600000);
    CHECK(sim.lines == quiet);
    pmic_set(0x11, 0x83, 0x0a);
    run_for(&c, 4200);
    CHECK(strstr(sim.last_line, "[battery] pmic change vbus=1 stage=3 rt=0x11 in=0x2 det=0xf rp=0x2 tc=0x2b st=0x83 icl=500 imax=2000 inow=2000 pct=87"));
    CHECK(strstr(sim.last_line, " mv=3950 ma=-812 avg=-800 t=29.50") && strlen(sim.last_line) < 200);
    pmic_set(0x11, 0x84, 0x0a);
    run_for(&c, 2100);
    CHECK(strstr(sim.last_line, "stage=4"));
    /* with VBUS a trend line every 2 min, whatever else happens */
    const unsigned before = sim.lines;
    run_for(&c, 601000);
    CHECK(sim.lines == before + 5 && strstr(sim.last_line, "[battery] pmic trend vbus=1"));
    /* a failing read: no state moved, one line for a failing run */
    sim.pmic_broken = 1;
    run_for(&c, 20000);
    CHECK(c.pmic_failures == 10 && c.pmic_plug && strstr(sim.last_line, "[battery] pmic read failed rc=-110 n=1"));
    sim.pmic_broken = 0;
    run_for(&c, 2100);
    CHECK(!c.pmic_fail_streak);
    /* log 0: no lines of its own */
    sim_reset(PbmLayoutOplus);
    logged.log = 0;
    pagani_battery_core_init(&c, &ops_pmic, &logged);
    pagani_battery_core_gauge(&c, 1);
    pmic_set(0x11, 0x83, 0x28);
    run_for(&c, 600000);
    CHECK(sim.lines == 0 && sim.pmic_reads > 100);
}

/* The charger hunting at the top of the charge for a day, and a cable that comes and goes: the budget holds, and a plug change still shows
 * after the stage changes used theirs up. */
static void test_pmic_budget(void)
{
    static PbatCore c;
    sim_reset(PbmLayoutOplus);
    pmic_samples();
    pmic_set(0x11, 0x83, 0x28);
    PbatConfig cfg = config(1, 0, 0);
    cfg.pmic = 1;
    pagani_battery_core_init(&c, &ops_pmic, &cfg);
    pagani_battery_core_gauge(&c, 1);
    for (unsigned n = 0; n < 2000; ++n) {
        sim.pmic[0x2606] = n & 1 ? 0x83 : 0x84;
        run_for(&c, 2000);
    }
    CHECK(c.pmic_change_lines == 24 && !c.pmic_plug_lines);
    pmic_set(0x04, 0x07, 0x28);
    run_for(&c, 4100);
    CHECK(c.pmic_plug_lines == 1 && strstr(sim.last_line, "[battery] pmic change vbus=0"));
    const uint64_t day = sim.now + 24ull * 3600 * 1000;
    while (sim.now < day) {
        const int on = (sim.now / 6000) & 1;
        pmic_set(on ? 0x11 : 0x04, on ? 0x83 : 0x07, 0x28);
        run_for(&c, 6000);
    }
    CHECK(c.pmic_plug_lines == 24 && c.pmic_change_lines == 24 && c.pmic_trend_lines <= 90 && sim.lines < 160);
    printf("pmic lines at most in a boot: %u\n", sim.lines);
}

static void test_pmic_use(void)
{
    static PbatCore c;
    const PbatSnapshot *s = &sim.shown;
    sim_reset(PbmLayoutOplus);
    pmic_samples();
    pmic_set(0x04, 0x07, 0);
    PbatConfig cfg = config(1, 0, 0);
    cfg.pmic = 2;
    pagani_battery_core_init(&c, &ops_pmic, &cfg);
    pagani_battery_core_gauge(&c, 1);
    run_for(&c, 3000);
    CHECK(!s->external && !s->charging && !s->fully_charged && s->sources == (PbatSrcGauge | PbatSrcPmic));
    /* a cable with a load larger than the input: the gauge's current says unplugged, the charger says otherwise (two samples, 2 to 4 s) */
    set_current(-400);
    pmic_set(0x11, 0x83, 0x0a);
    run_for(&c, 1900);
    CHECK(!s->external);
    run_for(&c, 2200);
    CHECK(s->external && s->charging && !s->fully_charged && s->adapter_family == 0xe0004007u && !s->critical);
    /* a change of the state brings the next gauge pass forward */
    run_for(&c, 3100);
    CHECK(s->external && s->charging && s->current_ma == -400);
    /* the stages that push current */
    for (uint8_t stage = 1; stage <= 4; ++stage) {
        sim.pmic[0x2606] = (uint8_t)(0x80 | stage);
        run_for(&c, 2100);
        CHECK(s->external && s->charging);
    }
    /* the top of the charge, the charger still tapering: charging, not yet full */
    sim.reg[0x2c] = 100;
    set_current(30);
    sim.pmic[0x2606] = 0x84;
    run_for(&c, 12500);
    CHECK(s->external && s->charging && !s->fully_charged && s->percent == 100);
    /* full at the top: not charging, connected, fully charged */
    set_current(5);
    sim.pmic[0x2606] = 0x85;
    run_for(&c, 12500);
    CHECK(s->external && !s->charging && s->fully_charged && s->percent == 100);
    /* inhibited at the top the same; a stage that does not charge below it is connected and no more */
    sim.pmic[0x2606] = 0x80;
    run_for(&c, 2100);
    CHECK(s->external && !s->charging && s->fully_charged);
    sim.reg[0x2c] = 80;
    run_for(&c, 12500);
    CHECK(s->external && !s->charging && !s->fully_charged);
    sim.pmic[0x2606] = 0x87;
    run_for(&c, 2100);
    CHECK(s->external && !s->charging && !s->fully_charged);
    sim.pmic[0x2606] = 0x86;
    run_for(&c, 2100);
    CHECK(s->external && !s->charging);
    /* one sample of VBUS gone is not a plug-out */
    pmic_set(0x04, 0x07, 0);
    run_for(&c, 1900);
    pmic_set(0x11, 0x83, 0x28);
    run_for(&c, 2200);
    CHECK(s->external && s->charging && c.pmic_plug);
    /* plug-in and under-voltage at once: not VBUS */
    pmic_set(0x14, 0x83, 0x28);
    run_for(&c, 4100);
    CHECK(!s->external);
    pmic_set(0x11, 0x83, 0x28);
    run_for(&c, 4100);
    CHECK(s->external);
    /* the charger's stage says nothing without VBUS */
    pmic_set(0x04, 0x83, 0x28);
    run_for(&c, 4100);
    CHECK(!s->external && !s->charging && !s->fully_charged);
    /* no battery current while plugged at 100 %: the case the gauge alone misses */
    sim.reg[0x2c] = 100;
    set_current(-10);
    pmic_set(0x11, 0x85, 0x28);
    run_for(&c, 14100);
    CHECK(s->external && s->fully_charged);
    /* the charger's reads stop: within 7 s back to the inference, which sees the same gauge and has run all along */
    sim.pmic_broken = 1;
    run_for(&c, 5000);
    CHECK(s->sources & PbatSrcPmic);
    run_for(&c, 12000);
    CHECK(!(s->sources & PbatSrcPmic) && (s->sources & PbatSrcInferred) && !s->external);
    /* they come back */
    sim.pmic_broken = 0;
    run_for(&c, 12500);
    CHECK((s->sources & PbatSrcPmic) && s->external);
    /* a line says where the state came from */
    CHECK(strstr(sim.last_state, "src=gauge+pmic"));
    /* the DSP's view goes first */
    static PbatCore d;
    sim_reset(PbmLayoutOplus);
    pmic_samples();
    pmic_set(0x11, 0x83, 0x28);
    sim.usb[0] = 0;
    cfg = config(1, 1, 0);
    cfg.pmic = 2;
    pagani_battery_core_init(&d, &ops_pmic, &cfg);
    pagani_battery_core_gauge(&d, 1);
    pagani_battery_core_channel(&d, 1);
    run_for(&d, 3000);
    CHECK(!s->external && s->sources == (PbatSrcGauge | PbatSrcCharger));
    /* a gauge that does not answer shows nothing, whatever the charger says */
    static PbatCore e;
    sim_reset(PbmLayoutOplus);
    pmic_samples();
    pmic_set(0x11, 0x83, 0x28);
    sim.gauge_absent = 1;
    cfg = config(1, 0, 0);
    cfg.pmic = 2;
    pagani_battery_core_init(&e, &ops_pmic, &cfg);
    pagani_battery_core_gauge(&e, 1);
    run_for(&e, 60000);
    CHECK(!sim.publishes && sim.pmic_reads > 100);
}

int main(void)
{
    CHECK(pagani_battery_family(1) == 0xe0004000u && pagani_battery_family(2) == 0xe0004004u && pagani_battery_family(3) == 0xe0004005u);
    CHECK(pagani_battery_family(5) == 0xe0004009u && pagani_battery_family(8) == 0xe000400au && pagani_battery_family(0) == 0xe0004007u);
    test_gauge_alone();
    test_dsp();
    test_dsp_alone();
    test_policy();
    test_policy_enable();
    test_log();
    test_log_flapping();
    test_residual();
    test_pmic_off();
    test_pmic_log();
    test_pmic_budget();
    test_pmic_use();
    printf("PASS core: %u checks\n", checks);
    return 0;
}
