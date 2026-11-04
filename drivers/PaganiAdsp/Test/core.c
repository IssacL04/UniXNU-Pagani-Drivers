// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026, UniXNU contributors. All rights reserved.
// Host test of the whole ADSP bring-up and run time (AdspCore.c, AdspQrtr.c, AdspFirmware.c and the ported cores) against a simulated
// SoC and DSP:
//   core VOLUME.img EXPECT_DIR
// - SMEM: 2 MiB with the partition table, the global partition, the apps-ADSP partition and partitions of other hosts. The pages the
//   driver must never touch (header page, the other hosts' partitions) are mprotect(PROT_NONE): one stray access ends the test.
// - The DSP: after adsp authenticates it makes SMP2P item 443 ("slave-kernel"), signals ready and handover, answers SMP2P feature
//   negotiation, makes its GLINK FIFO (480) once ours exists, opens IPCRTR, PMIC_RTR_ADSP_APPS, adsp_apps and PMIC_LOGS_ADSP_APPS,
//   says hello on the IPC router, announces services and asks the locator for "avs/audio" and "tms/servreg" (checking the answers),
//   and echoes on the two client channels. Fault switches cover every stage's failure, timeouts, a crash and a broken stream.
// - VOLUME.img: modem_a (FAT, mtools) with image/adsp* and image/adsp_dtb*; EXPECT_DIR: the metadata and carveout bytes the script
//   computed. Time is simulated.
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include "../AdspCore.h"

static unsigned checks;
#define CHECK(x) do {if (!(x)) {fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); dump_log(); exit(1);} ++checks;} while (0)

/* ---- the log ---- */
static char logbuf[1 << 18];
static size_t logused;
static int verbose;
static void dump_log(void) {fprintf(stderr, "---- log ----\n%s", logbuf);}
static int logged(const char *text) {return strstr(logbuf, text) != 0;}

/* ---- the scenario ---- */
typedef struct {
    int toc_bad, no_partition, fw_not_ready, fw_missing, running_already, hwlock_busy, irq_fail, qmp_rc, rails_rc;
    int dtb_init_rc, dtb_auth_rc, adsp_init_rc, adsp_setup_rc, adsp_auth_rc;
    int never_ready, fatal_at_boot, no_handover, handover_first, fatal_later, corrupt, no_fifo;
    int shutdown_rc;                  /* the PAS shutdown fails */
    int irq_silent;                   /* the interrupt attaches, but no doorbell is ever delivered */
    int lose_doorbells;               /* doorbells are lost (a lost edge) */
    uint32_t ready_ms, stop;
} Scenario;
static Scenario sc;

/* ---- time and events ---- */
static uint64_t now_ms;
static uint32_t pending;          /* events for the core */
static unsigned dsp_rings[3];     /* doorbells the core rang: [0] GLINK, [2] SMP2P */
static unsigned dsp_todo;         /* bit 0: GLINK kick, bit 2: SMP2P kick */
static void dsp_step(void);

/* ---- SMEM ---- */
#define SMEM_SIZE 0x200000u
enum {kGlobal = 0x4000, kGlobalSize = 0x98000, kOther = 0x9c000, kPart = 0x100000, kPartSize = 0x80000, kRest = 0x180000,
      kTocPage = 0x1fc000};
static uint8_t *smem;
static void w32(uint32_t at, uint32_t v) {memcpy(smem + at, &v, 4);}
static void partition(uint32_t index, uint32_t offset, uint32_t size, uint16_t h0, uint16_t h1)
{
    const uint32_t e = SMEM_SIZE - 0x1000 + 32 + 48 * index;
    w32(e, offset); w32(e + 4, size); w32(e + 12, h0 | (uint32_t)h1 << 16); w32(e + 16, 64);
    w32(offset, 0x54525024u); w32(offset + 4, h0 | (uint32_t)h1 << 16); w32(offset + 8, size);
    w32(offset + 12, 32); w32(offset + 16, size);
}
static void make_smem(void)
{
    if (smem) munmap(smem, SMEM_SIZE);
    smem = mmap(0, SMEM_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    CHECK(smem != MAP_FAILED && !((uintptr_t)smem & 0x3fff));
    memset(smem, 0, SMEM_SIZE);
    const uint32_t toc = SMEM_SIZE - 0x1000;
    w32(toc, sc.toc_bad ? 0 : 0x434f5424u); w32(toc + 4, 1); w32(toc + 8, 5);
    partition(0, kGlobal, kGlobalSize, 0xfffe, 0xfffe);
    partition(1, kOther, 0x64000, 1, 2);                     /* modem-adsp */
    if (!sc.no_partition) partition(2, kPart, kPartSize, 0, 2);
    partition(3, kRest, 0x40000, 0, 1);                       /* apps-modem: ours, but this driver has no business there */
    partition(4, kRest + 0x40000, 0x3c000, 5, 2);             /* cdsp-adsp */
    /* What the driver must never touch is made inaccessible. */
    CHECK(!mprotect(smem, kGlobal, PROT_NONE));
    CHECK(!mprotect(smem + kOther, kPart - kOther, PROT_NONE));
    CHECK(!mprotect(smem + kRest, kTocPage - kRest, PROT_NONE));
}

/* ---- the DSP ---- */
typedef struct {unsigned ups, downs, messages; uint8_t last[4096]; uint32_t length;} Seen;
static struct {
    QcomSmem view;
    QcomSmemPartition part, global;
    QcomSmp2p p2p;
    int slave;
    int booted, ready, handover, fatal;
    uint64_t ready_at, handover_at, fatal_at;
    QcomGlink glink;
    int glink_on;
    uint32_t desc, rx;
    QcomGlinkChannel *ipcrtr, *pmic, *apps, *logs;
    uint8_t store[4][8 * 4096];
    Seen seen_pmic, seen_apps;
    QcomQrtr qrtr;
    int asked, locator_ok, servreg_ok, services_sent;
} dsp;

static void dsp_kick_glink(void *c) {(void)c; pending |= PaganiAdspEvGlink;}
static int dsp_qrtr_send(void *c, const uint8_t *packet, uint32_t length)
{
    (void)c;
    return qcom_glink_send(&dsp.glink, dsp.ipcrtr, packet, length) == QcomGlinkOk ? 0 : -1;
}
static uint32_t le32(const uint8_t *p) {return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;}
static uint16_t le16(const uint8_t *p) {return (uint16_t)(p[0] | p[1] << 8);}
/* The answers to its locator requests (a client port of the DSP's router). */
static void dsp_locator_answer(void *c, QcomQrtr *q, uint32_t port, uint32_t node, uint32_t from, const uint8_t *data, uint32_t length)
{
    (void)c; (void)q; (void)port;
    CHECK(node == 1 && from == 1 && length > 19 && data[0] == 2 && le16(data + 3) == 0x21);
    const unsigned total = le16(data + 17), txn = le16(data + 1);
    if (txn == 1) {
        CHECK(total == 1 && data[28] == 17 && !memcmp(data + 29, "msm/adsp/audio_pd", 17) && le32(data + 46) == 74);
        dsp.locator_ok = 1;
    } else {
        CHECK(txn == 2 && total == 6);
        dsp.servreg_ok = 1;
    }
}
static void dsp_ask(uint16_t txn, const char *name)
{
    uint8_t m[64];
    const uint32_t n = (uint32_t)strlen(name);
    m[0] = 0; m[1] = (uint8_t)txn; m[2] = (uint8_t)(txn >> 8); m[3] = 0x21; m[4] = 0; m[5] = (uint8_t)(3 + n); m[6] = 0;
    m[7] = 1; m[8] = (uint8_t)n; m[9] = 0;
    memcpy(m + 10, name, n);
    CHECK(qcom_qrtr_send_data(&dsp.qrtr, 0x20, 1, 1, m, 10 + n) == 0);
}
static void dsp_state(void *c, QcomGlinkChannel *ch, int up)
{
    Seen *s = c;
    if (ch == dsp.ipcrtr) {
        if (up) qcom_qrtr_start(&dsp.qrtr);
        return;
    }
    if (s) {if (up) s->ups++; else s->downs++;}
}
static void dsp_receive(void *c, QcomGlinkChannel *ch, const uint8_t *data, uint32_t length)
{
    if (ch == dsp.ipcrtr) {
        CHECK(qcom_qrtr_receive(&dsp.qrtr, data, length) == 0);
        /* Our hello came back and the locator was announced: ask it, once. */
        if (dsp.qrtr.hello_received && qcom_qrtr_find_remote(&dsp.qrtr, 0x40) >= 0 && !dsp.asked) {
            dsp.asked = 1;
            CHECK(dsp.qrtr.remote[qcom_qrtr_find_remote(&dsp.qrtr, 0x40)].node == 1);
            dsp_ask(1, "avs/audio");
            dsp_ask(2, "tms/servreg");
        }
        return;
    }
    Seen *s = c;
    s->messages++;
    s->length = length;
    memcpy(s->last, data, length);
    /* echo with the first byte flipped; a long message gets its first 16 bytes back (the DSP's replies fit 512-byte intents) */
    uint8_t reply[4096];
    const uint32_t n = length > 512 ? 16 : length;
    memcpy(reply, data, n);
    reply[0] ^= 0xff;
    CHECK(qcom_glink_send(&dsp.glink, ch, reply, n) == QcomGlinkOk);
}

static void dsp_boot(void)
{
    dsp.booted = 1;
    dsp.ready_at = now_ms + (sc.ready_ms ? sc.ready_ms : 40);
    dsp.handover_at = dsp.ready_at + (sc.handover_first ? -10 : 5);
}
static void dsp_set(uint32_t bits)
{
    qcom_smp2p_update(&dsp.p2p, dsp.slave, bits, bits);
    pending |= PaganiAdspEvSmp2p;
}
static void dsp_crash_reason(const char *text)
{
    uint32_t at = 0, size = 0;
    CHECK(qcom_smem_alloc(&dsp.view, &dsp.global, 423, 64) == 0);
    CHECK(qcom_smem_find(&dsp.view, &dsp.global, 423, &at, &size) == 0);
    memcpy(smem + at, text, strlen(text) + 1);
}
static void dsp_step(void)
{
    if (!dsp.booted) return;
    /* the DSP's own SMP2P item, made as it starts */
    if (dsp.slave < 0 && now_ms + 30 >= dsp.ready_at) {
        uint32_t at = 0, size = 0;
        CHECK(qcom_smem_alloc(&dsp.view, &dsp.part, 443, QCOM_SMP2P_ITEM_BYTES) == 0);
        CHECK(qcom_smem_find(&dsp.view, &dsp.part, 443, &at, &size) == 0);
        qcom_smp2p_init_out(&dsp.p2p, (volatile uint32_t *)(smem + at), 2, 0);
        dsp.slave = qcom_smp2p_add_entry(&dsp.p2p, "slave-kernel");
        /* our item 429 as the DSP finds it */
        CHECK(qcom_smem_find(&dsp.view, &dsp.part, 429, &at, &size) == 0 && size >= QCOM_SMP2P_ITEM_BYTES);
        dsp.p2p.in = (volatile uint32_t *)(smem + at);
        CHECK(qcom_smp2p_check_in(dsp.p2p.in, 2, 0) == 0);
        CHECK(qcom_smp2p_find_in(&dsp.p2p, "master-kernel") == 0 && qcom_smp2p_find_in(&dsp.p2p, "sleepstate") == 1);
        CHECK(qcom_smp2p_in_value(&dsp.p2p, 1) == 1);
        pending |= PaganiAdspEvSmp2p;
    }
    if (dsp.slave >= 0 && (dsp_todo & 4)) {
        uint32_t value, changed;
        if (qcom_smp2p_service(&dsp.p2p, "master-kernel", &value, &changed)) pending |= PaganiAdspEvSmp2p;
        CHECK(!(value & 1));   /* nobody asks the DSP to stop */
    }
    if (dsp.slave >= 0 && sc.fatal_at_boot && !dsp.fatal && now_ms >= dsp.ready_at) {
        dsp_crash_reason("audio_pd: err_fatal at boot");
        dsp.fatal = 1;
        dsp_set(PaganiAdspBitFatal);
    }
    if (dsp.slave >= 0 && !sc.never_ready && !sc.fatal_at_boot && !dsp.ready && now_ms >= dsp.ready_at) {dsp.ready = 1; dsp_set(PaganiAdspBitReady);}
    if (dsp.slave >= 0 && !sc.no_handover && !sc.fatal_at_boot && !dsp.handover && now_ms >= dsp.handover_at) {
        dsp.handover = 1;
        dsp_set(PaganiAdspBitHandover);
    }
    /* GLINK, once our descriptor and FIFO exist */
    uint32_t desc = 0, dsize = 0, tx = 0, tsize = 0;
    if (dsp.ready && !dsp.glink_on && !sc.no_fifo && !qcom_smem_find(&dsp.view, &dsp.part, 478, &desc, &dsize) &&
        !qcom_smem_find(&dsp.view, &dsp.part, 479, &tx, &tsize)) {
        uint32_t rx = 0, rsize = 0;
        CHECK(dsize >= 16 && tsize == 0x4000);
        CHECK(qcom_smem_alloc(&dsp.view, &dsp.part, 480, 0x4000) == 0 && !qcom_smem_find(&dsp.view, &dsp.part, 480, &rx, &rsize));
        dsp.desc = desc;
        dsp.rx = rx;
        volatile uint32_t *d = (volatile uint32_t *)(smem + desc);
        const QcomGlinkFifo mine = {&d[3], &d[2], smem + rx, rsize}, theirs = {&d[1], &d[0], smem + tx, tsize};
        qcom_glink_init(&dsp.glink, mine, theirs, dsp_kick_glink, 0);
        static const QcomGlinkClient client = {dsp_state, dsp_receive};
        dsp.ipcrtr = qcom_glink_channel(&dsp.glink, "IPCRTR", 2048, 8, dsp.store[0], &client, 0, 1);
        dsp.pmic = qcom_glink_channel(&dsp.glink, "PMIC_RTR_ADSP_APPS", 1024, 4, dsp.store[1], &client, &dsp.seen_pmic, 1);
        dsp.apps = qcom_glink_channel(&dsp.glink, "adsp_apps", 4096, 6, dsp.store[2], &client, &dsp.seen_apps, 1);
        dsp.logs = qcom_glink_channel(&dsp.glink, "PMIC_LOGS_ADSP_APPS", 1024, 2, dsp.store[3], &client, 0, 1);
        CHECK(dsp.ipcrtr && dsp.pmic && dsp.apps && dsp.logs);
        qcom_qrtr_init(&dsp.qrtr, 5, dsp_qrtr_send, 0);
        qcom_qrtr_add_service(&dsp.qrtr, 0x777, 1, 0x20, dsp_locator_answer, 0);   /* the client port of its locator requests */
        dsp.glink_on = 1;
        dsp_todo |= 1;
        dsp.fatal_at = now_ms + 200;
    }
    if (dsp.glink_on) {
        if (sc.corrupt && dsp.asked) {
            /* a command no GLINK speaks */
            const uint8_t bad[8] = {0x55, 0, 0, 0, 0, 0, 0, 0};
            volatile uint32_t *d = (volatile uint32_t *)(smem + dsp.desc);
            uint32_t head = d[3];
            for (unsigned i = 0; i < 8; ++i) {smem[dsp.rx + head] = bad[i]; head = (head + 1) % 0x4000;}
            d[3] = head;
            sc.corrupt = 0;
            pending |= PaganiAdspEvGlink;
        }
        qcom_glink_poll(&dsp.glink);
        if (dsp.qrtr.hello_received && !dsp.services_sent) {
            /* the DSP's own services, as its name service announces them */
            static const uint32_t services[3][2] = {{0x190, 0x1}, {0x42, 0x4a01}, {0x2b, 0x14}};
            for (unsigned i = 0; i < 3; ++i) {
                uint8_t pkt[20];
                memset(pkt, 0, sizeof pkt);
                pkt[0] = QCOM_QRTR_NEW_SERVER;
                memcpy(pkt + 4, &services[i][0], 4);
                memcpy(pkt + 8, &services[i][1], 4);
                const uint32_t node = 5, port = 0x30 + i;
                memcpy(pkt + 12, &node, 4);
                memcpy(pkt + 16, &port, 4);
                uint8_t m[52];
                const uint32_t words[8] = {1, QCOM_QRTR_NEW_SERVER, 5, QCOM_QRTR_PORT_CTRL, 0, 20, 1, QCOM_QRTR_PORT_CTRL};
                memcpy(m, words, 32);
                memcpy(m + 32, pkt, 20);
                CHECK(qcom_glink_send(&dsp.glink, dsp.ipcrtr, m, sizeof m) == QcomGlinkOk);
            }
            dsp.services_sent = 1;
        }
        if (sc.fatal_later && !dsp.fatal && now_ms >= dsp.fatal_at) {
            dsp_crash_reason("adsp: wdog bite");
            dsp.fatal = 1;
            dsp_set(PaganiAdspBitFatal);
        }
    }
    dsp_todo = 0;
}

/* ---- the platform operations ---- */
static char sequence[4096];
static void seq(const char *what) {strcat(sequence, what); strcat(sequence, " ");}
static size_t allocated;
static unsigned locks, rings_total;
static int lock_depth;
static int64_t pas_x0;
static uint64_t pas_x1;
static uint8_t *expect_meta[2], *expect_region[2];
static size_t expect_meta_size[2], expect_region_size[2];
static uint8_t *region_dtb, *region_adsp;
static int mapped;
static unsigned interrupts;

static PaganiAdspCore core;

static uint64_t op_now(void *c) {(void)c; return now_ms;}
static uint32_t op_wait(void *c, uint32_t ms)
{
    (void)c;
    dsp_step();
    if (!pending) {now_ms += ms < 7 ? ms : 7; dsp_step();}
    else now_ms += 1;
    const uint32_t events = pending;
    pending = 0;
    if (sc.irq_fail || sc.irq_silent || sc.lose_doorbells) return 0;   /* no interrupt: the core polls */
    if (events & (PaganiAdspEvSmp2p | PaganiAdspEvGlink)) {interrupts++; core.status.ipcc_interrupts++;}
    return events;
}
static void op_wake(void *c) {(void)c; pending |= PaganiAdspEvSend;}
static void op_sleep(void *c, uint32_t ms) {(void)c; now_ms += ms;}
/* The UFS log's writer as the stretches hold it: a watched run checks each first touch below. */
static int quiet_watch, quiet_held;
static unsigned quiet_holds;
static void op_quiet(void *c, unsigned on)
{
    (void)c;
    CHECK(quiet_held != (int)on);
    quiet_held = (int)on;
    quiet_holds += on;
}
static void quiet_touch(int wanted) {CHECK(!quiet_watch || quiet_held == wanted);}
static void op_trace(void *c, const char *line)
{
    if (strstr(line, "[adsp] toc ") || strstr(line, "[adsp] smem touch") || strstr(line, "[adsp] rails on") || strstr(line, "[adsp] pas loaded")) quiet_touch(1);
    if (strstr(line, "[adsp] fw part=") || strstr(line, "[adsp] glink start")) quiet_touch(0);
    (void)c;
    CHECK(strlen(line) < 256 && !strncmp(line, "[adsp] ", 7));
    if (verbose) printf("%" PRIu64 " %s\n", now_ms, line);
    const size_t n = strlen(line);
    if (logused + n + 2 < sizeof logbuf) {memcpy(logbuf + logused, line, n); logused += n; logbuf[logused++] = '\n'; logbuf[logused] = 0;}
}
static void op_lock(void *c) {(void)c; CHECK(lock_depth == 0); lock_depth++; locks++;}
static void op_unlock(void *c) {(void)c; CHECK(lock_depth == 1); lock_depth--;}
static void *op_alloc(void *c, size_t n) {(void)c; allocated += n; return calloc(1, n);}
static void op_free(void *c, void *p, size_t n) {(void)c; CHECK(allocated >= n); allocated -= n; free(p);}
static int hwlock_held;
static unsigned hwlock_takes;
static int op_hwlock(void *c, int take)
{
    if (core.status.stage < PaganiAdspStReady) quiet_touch(1);
    (void)c;
    if (!take) {CHECK(hwlock_held); hwlock_held = 0; return 1;}
    CHECK(!hwlock_held);
    hwlock_takes++;
    if (sc.hwlock_busy) return 0;
    hwlock_held = 1;
    return 1;
}
static void op_ring(void *c, uint32_t signal)
{
    if (core.status.stage < PaganiAdspStReady) quiet_touch(1);
    (void)c;
    CHECK(signal == 0 || signal == 2);
    dsp_rings[signal]++;
    rings_total++;
    dsp_todo |= 1u << signal;
}
static int op_doorbells(void *c) {(void)c; seq("doorbells"); return sc.irq_fail ? -19 : 0;}
static void op_survey(void *c, PaganiAdspSurvey *out) {(void)c; out->aoss_magic = 0x4d41494c; out->aoss_version = 1; out->aoss_links = 0xf; out->rsc_id = 0x30000;}
static int op_qmp(void *c, const char *message)
{
    (void)c;
    if (strstr(message, "val: on}")) {CHECK(!strcmp(message, "{class: image, res: load_state, name: adsp, val: on}")); seq("qmp:on"); return sc.qmp_rc;}
    CHECK(!strcmp(message, "{class: image, res: load_state, name: adsp, val: off}"));
    seq("qmp:off");
    return 0;
}
static int op_rails(void *c, int on) {(void)c; seq(on ? "rails:on" : "rails:off"); return on ? sc.rails_rc : 0;}
static int op_pas_init(void *c, uint32_t pas, const uint8_t *meta, uint64_t bytes)
{
    quiet_touch(1);
    (void)c;
    const int i = pas == 1;
    seq(i ? "init:1" : "init:24");
    CHECK(pas == 1 || pas == 0x24);
    CHECK(bytes == expect_meta_size[i] && !memcmp(meta, expect_meta[i], (size_t)bytes));
    pas_x0 = 0; pas_x1 = 0;
    const int rc = i ? sc.adsp_init_rc : sc.dtb_init_rc;
    if (rc) pas_x0 = -2;
    return rc;
}
static int op_pas_setup(void *c, uint32_t pas, uint64_t base, uint64_t bytes)
{
    (void)c;
    const int i = pas == 1;
    seq(i ? "setup:1" : "setup:24");
    CHECK(base == (i ? PAGANI_ADSP_REGION_BASE : PAGANI_ADSP_DTB_BASE) && bytes == expect_region_size[i] && !(bytes & 0xfff));
    return i ? sc.adsp_setup_rc : 0;
}
static int op_pas_auth(void *c, uint32_t pas)
{
    (void)c;
    const int i = pas == 1;
    seq(i ? "auth:1" : "auth:24");
    CHECK(!mapped);   /* the carveout is unmapped before the secure world locks it */
    const uint8_t *region = i ? region_adsp : region_dtb;
    CHECK(!memcmp(region, expect_region[i], expect_region_size[i]));
    const int rc = i ? sc.adsp_auth_rc : sc.dtb_auth_rc;
    pas_x0 = 0;
    pas_x1 = rc ? 0x7 : 0;
    if (!rc && i) dsp_boot();
    return rc;
}
static int op_pas_shutdown(void *c, uint32_t pas) {(void)c; seq(pas == 1 ? "shutdown:1" : "shutdown:24"); return sc.shutdown_rc;}
static unsigned released[2], kept[2];
static void op_pas_release(void *c, uint32_t pas, int authenticated) {(void)c; if (authenticated) released[pas == 1]++; else kept[pas == 1]++;}
static void op_pas_result(void *c, int64_t *x0, uint64_t *x1) {(void)c; *x0 = pas_x0; *x1 = pas_x1;}
static uint8_t *op_map(void *c, uint64_t base, uint64_t bytes)
{
    (void)c;
    CHECK(!mapped);
    seq("map");
    if (base == PAGANI_ADSP_DTB_BASE) {CHECK(bytes == PAGANI_ADSP_DTB_SIZE); mapped = 1; return region_dtb;}
    CHECK(base == PAGANI_ADSP_REGION_BASE && bytes == PAGANI_ADSP_REGION_SIZE);
    mapped = 1;
    return region_adsp;
}
static void op_unmap(void *c, uint8_t *va, uint64_t bytes) {(void)c; (void)va; (void)bytes; CHECK(mapped); mapped = 0; seq("unmap");}

/* the firmware partition */
static uint8_t *volume;
static size_t volume_size;
static unsigned finds;
static int disk_find(void *c, const char *name, uint32_t *lun, uint64_t *first, uint64_t *blocks)
{
    (void)c;
    finds++;
    if (sc.fw_not_ready) {sc.fw_not_ready--; return 0;}
    if (strcmp(name, "modem_a")) return 0;
    *lun = 4; *first = 4870; *blocks = volume_size / 4096;
    return 1;
}
static int disk_read(void *c, uint32_t lun, uint64_t lba, uint32_t blocks, void *dst)
{
    (void)c;
    CHECK(lun == 4 && lba >= 4870 && (lba - 4870 + blocks) * 4096 <= volume_size);
    memcpy(dst, volume + (lba - 4870) * 4096, (size_t)blocks * 4096);
    return 1;
}
static void *disk_alloc(void *c, size_t n) {return op_alloc(c, n);}
static void disk_free(void *c, void *p, size_t n) {op_free(c, p, n);}

static const PaganiAdspOps ops = {
    0, op_now, op_wait, op_wake, op_sleep, op_trace, op_lock, op_unlock, op_alloc, op_free, op_hwlock, op_ring, op_doorbells,
    op_survey, op_qmp, op_rails, op_pas_init, op_pas_setup, op_pas_auth, op_pas_shutdown, op_pas_release, op_pas_result, op_map,
    op_unmap, {0, disk_find, disk_read, disk_alloc, disk_free}, 0,
};
static const uint32_t retry[] = {1000, 2000, 4000};

/* ---- clients ---- */
static struct {unsigned ups, downs, messages; uint8_t last[4096]; uint32_t length;} battery, audio;
static void battery_state(void *c, int up) {(void)c; if (up) battery.ups++; else battery.downs++;}
static void battery_receive(void *c, const uint8_t *d, uint32_t n) {(void)c; battery.messages++; battery.length = n; memcpy(battery.last, d, n);}
static void audio_state(void *c, int up) {(void)c; if (up) audio.ups++; else audio.downs++;}
static void audio_receive(void *c, const uint8_t *d, uint32_t n) {(void)c; audio.messages++; audio.length = n; memcpy(audio.last, d, n);}

static void reset(void)
{
    memset(&dsp, 0, sizeof dsp);
    dsp.slave = -1;
    make_smem();
    dsp.view = (QcomSmem){(volatile uint8_t *)smem, SMEM_SIZE, 0, {{0, 0}}, 0};
    if (!sc.no_partition && !sc.toc_bad) CHECK(qcom_smem_partition(&dsp.view, 0, 2, &dsp.part) == 0);
    if (!sc.toc_bad) CHECK(qcom_smem_partition(&dsp.view, 0xfffe, 0xfffe, &dsp.global) == 0);
    if (sc.running_already) {
        uint32_t at = 0, size = 0;
        CHECK(qcom_smem_alloc(&dsp.view, &dsp.part, 443, QCOM_SMP2P_ITEM_BYTES) == 0);
        CHECK(qcom_smem_find(&dsp.view, &dsp.part, 443, &at, &size) == 0);
        qcom_smp2p_init_out(&dsp.p2p, (volatile uint32_t *)(smem + at), 2, 0);
        qcom_smp2p_update(&dsp.p2p, qcom_smp2p_add_entry(&dsp.p2p, "slave-kernel"), 2, 2);
    }
    sequence[0] = 0;
    logused = 0;
    logbuf[0] = 0;
    pending = 0;
    dsp_todo = 0;
    memset(dsp_rings, 0, sizeof dsp_rings);
    memset(&battery, 0, sizeof battery);
    memset(&audio, 0, sizeof audio);
    memset(released, 0, sizeof released);
    memset(kept, 0, sizeof kept);
    memset(region_dtb, 0xa5, PAGANI_ADSP_DTB_SIZE);
    memset(region_adsp, 0xa5, expect_region_size[1] + 0x10000);
    mapped = 0;
    interrupts = 0;
    hwlock_held = 0;
}

static const char *const partitions[2] = {"modem_a", "modem_b"};
static int boot(void)
{
    const PaganiAdspConfig config = {sc.stop, 0, {partitions[0], partitions[1]}, 2, "image", retry, 3, 0};
    pagani_adsp_core_init(&core, &ops, &config, (volatile uint8_t *)smem, SMEM_SIZE);
    return pagani_adsp_core_boot(&core);
}
static void run_for(uint32_t ms)
{
    const uint64_t end = now_ms + ms;
    while (now_ms < end) {
        uint32_t idle = pagani_adsp_core_idle_ms(&core);
        if (idle > end - now_ms) idle = (uint32_t)(end - now_ms);
        uint32_t ev = op_wait(0, idle ? idle : 1);
        if (sc.irq_fail) ev |= PaganiAdspEvPoll;
        pagani_adsp_core_service(&core, ev);
    }
}
static void release_all(void)
{
    /* the channel buffers of this core (a kernel core lives until reboot) */
    for (uint32_t i = 0; i < core.channel_count; ++i) {
        PaganiAdspChannel *ch = &core.channel[i];
        op_free(0, ch->storage, (size_t)ch->intent_size * ch->intent_count);
        op_free(0, ch->queue, (size_t)ch->depth * (4u + ch->max_message));
    }
    CHECK(allocated == 0);
}

static PaganiAdspChannel *battery_channel, *audio_channel;
static void register_clients(void)
{
    static const PaganiAdspClient bc = {battery_state, battery_receive}, ac = {audio_state, audio_receive};
    battery_channel = pagani_adsp_core_channel(&core, &(PaganiAdspChannelConfig){"PMIC_RTR_ADSP_APPS", 1024, 5, 0, 0, 0}, &bc, 0);
    audio_channel = pagani_adsp_core_channel(&core, &(PaganiAdspChannelConfig){"adsp_apps", 512, 20, 0, 4096, 4}, &ac, 0);
    CHECK(battery_channel && audio_channel);
}

/* ---- the scenarios ---- */
static void happy(int irq)
{
    memset(&sc, 0, sizeof sc);
    sc.irq_fail = !irq;
    reset();
    const uint64_t began = now_ms;
    /* clients may register before the boot (Stage7Probe order) */
    pagani_adsp_core_init(&core, &ops, &(PaganiAdspConfig){0, 0, {"modem_a", "modem_b"}, 2, "image", retry, 3, 0}, (volatile uint8_t *)smem, SMEM_SIZE);
    register_clients();
    CHECK(!pagani_adsp_core_channel(&core, &(PaganiAdspChannelConfig){"adsp_apps", 512, 20, 0, 0, 0},
                                    &(PaganiAdspClient){audio_state, audio_receive}, 0));             /* a name once */
    CHECK(pagani_adsp_core_send(&core, battery_channel, "early", 5) == PaganiAdspOk);           /* queued until the DSP opens it */
    CHECK(pagani_adsp_core_boot(&core) == 1);
    CHECK(core.status.stage == PaganiAdspStReady || core.status.stage == PaganiAdspStGlink || core.status.stage == PaganiAdspStChannel);
    CHECK(strstr(sequence, "doorbells qmp:on rails:on init:24 setup:24 map unmap auth:24 init:1 setup:1 map unmap auth:1") == sequence);
    CHECK(released[0] == 1 && released[1] == 1 && !kept[0] && !kept[1]);
    CHECK(core.status.ready_ms >= 40 && core.status.ready_ms < 200);
    run_for(3000);
    /* handover dropped the votes, after ready */
    CHECK(strstr(sequence, "auth:1 rails:off") != 0 && core.handover && !core.votes_on);
    CHECK(core.status.stage == PaganiAdspStChannel && core.glink.version_agreed && core.glink.features == 1);
    /* the OnePlus sensor items in the global partition, as Android's modules leave them */
    {
        uint32_t at = 0, size = 0;
        CHECK(qcom_smem_find(&dsp.view, &dsp.global, 130, &at, &size) == 0 && size >= 9016);
        for (uint32_t i = 0; i < 9016; i += 4) {
            uint32_t word;
            memcpy(&word, smem + at + i, 4);
            CHECK(word == (i == 8764 ? 6u : i == 8836 ? 1u : 0u));
        }
        CHECK(qcom_smem_find(&dsp.view, &dsp.global, 128, &at, &size) == 0 && size >= 3488);
    }
    CHECK(core.status.channels_up == 7);    /* IPCRTR, PMIC_RTR_ADSP_APPS, adsp_apps */
    CHECK(battery.ups == 1 && audio.ups == 1 && dsp.seen_pmic.messages == 1 && !memcmp(dsp.seen_pmic.last, "early", 5));
    CHECK(battery.messages == 1 && battery.last[0] == ('e' ^ 0xff));
    /* the IPC router: hello both ways, our locator announced, the DSP's services seen, both questions answered */
    CHECK(dsp.locator_ok && dsp.servreg_ok && core.status.locator_requests == 2 && core.status.locator_answers == 2);
    CHECK(core.status.qrtr_node == 5 && core.status.qrtr_services == 4);   /* its locator client port and three services */
    CHECK(logged("glink opens nobody serves: PMIC_LOGS_ADSP_APPS") && logged("locator ask=avs/audio domains=1 sent=1"));
    /* messages both ways, a long one (more than the 512-byte default), many so the FIFOs wrap */
    uint8_t big[3000];
    for (unsigned i = 0; i < sizeof big; ++i) big[i] = (uint8_t)i;
    CHECK(pagani_adsp_core_send(&core, audio_channel, big, sizeof big) == PaganiAdspOk);
    run_for(50);
    CHECK(dsp.seen_apps.messages == 1 && dsp.seen_apps.length == sizeof big && !memcmp(dsp.seen_apps.last, big, sizeof big));
    CHECK(audio.messages == 1 && audio.length == 16 && audio.last[0] == 0xff && !memcmp(audio.last + 1, big + 1, 15));
    for (unsigned i = 0; i < 300; ++i) {
        uint8_t m[100];
        memset(m, (int)i, sizeof m);
        CHECK(pagani_adsp_core_send(&core, battery_channel, m, 20 + i % 80) == PaganiAdspOk);
        run_for(3);
    }
    CHECK(dsp.seen_pmic.messages == 301 && battery.messages == 301 && core.glink.dropped == 0);
    /* the queue: full, too big, bad arguments */
    unsigned queued = 0;
    while (pagani_adsp_core_send(&core, audio_channel, "q", 1) == PaganiAdspOk) queued++;
    CHECK(queued == 4);
    CHECK(pagani_adsp_core_send(&core, audio_channel, big, 4097) == PaganiAdspTooBig);
    CHECK(pagani_adsp_core_send(&core, audio_channel, 0, 4) == PaganiAdspArgument && pagani_adsp_core_send(&core, 0, "x", 1) == PaganiAdspArgument);
    run_for(50);
    CHECK(audio.messages == 5);
    /* the SMEM accesses all stayed inside the readable ranges (and no protected page was touched: we are still here) */
    CHECK(core.refused == 0);
    if (irq) CHECK(interrupts > 0 && !logged("attached but silent"));
    else CHECK(logged("ipcc interrupt unavailable, polling"));
    /* the DSP crashes while running: the reason is read, the channels go down, sends refused */
    printf("happy path (%s): ready in %u ms, %u GLINK messages in, %u out, %" PRIu64 " ms simulated\n", irq ? "interrupt" : "polling",
           core.status.ready_ms, core.status.glink_in, core.status.glink_out, now_ms - began);
    release_all();
}

/* The interrupt attaches, but no doorbell is ever delivered: ready is found by the wait's last look, and from there on the core polls. */
static void silent_interrupt(void)
{
    memset(&sc, 0, sizeof sc);
    sc.irq_silent = 1;
    reset();
    pagani_adsp_core_init(&core, &ops, &(PaganiAdspConfig){0, 0, {"modem_a", "modem_b"}, 2, "image", retry, 3, 0}, (volatile uint8_t *)smem, SMEM_SIZE);
    register_clients();
    CHECK(pagani_adsp_core_boot(&core) == 1);
    CHECK(logged("ipcc interrupt attached but silent, polling") && !core.irq_ok);
    CHECK(pagani_adsp_core_idle_ms(&core) <= 20);
    run_for(3000);
    CHECK(core.status.stage == PaganiAdspStChannel && core.status.channels_up == 7 && battery.ups == 1 && audio.ups == 1);
    release_all();
}

/* Doorbells lost after a good start (the interrupt stays attached): the idle wait's end looks at everything, the link goes on. */
static void lost_doorbells(void)
{
    memset(&sc, 0, sizeof sc);
    reset();
    pagani_adsp_core_init(&core, &ops, &(PaganiAdspConfig){0, 0, {"modem_a", "modem_b"}, 2, "image", retry, 3, 0}, (volatile uint8_t *)smem, SMEM_SIZE);
    register_clients();
    CHECK(pagani_adsp_core_boot(&core) == 1);
    run_for(100);
    CHECK(core.irq_ok);
    const unsigned before = core.status.glink_in;
    sc.lose_doorbells = 1;
    CHECK(pagani_adsp_core_send(&core, battery_channel, "lost", 4) == PaganiAdspOk);
    run_for(180000);
    CHECK(battery.messages >= 1 && core.status.glink_in > before && core.irq_ok);
    sc.lose_doorbells = 0;
    release_all();
}

static void crash_while_running(void)
{
    memset(&sc, 0, sizeof sc);
    sc.fatal_later = 1;
    reset();
    pagani_adsp_core_init(&core, &ops, &(PaganiAdspConfig){0, 0, {"modem_a", "modem_b"}, 2, "image", retry, 3, 0}, (volatile uint8_t *)smem, SMEM_SIZE);
    register_clients();
    CHECK(pagani_adsp_core_boot(&core) == 1);
    run_for(1000);
    CHECK(core.status.crashed && battery.downs == 1 && audio.downs == 1 && !strcmp(core.status.note, "crash: adsp: wdog bite"));
    CHECK(pagani_adsp_core_send(&core, battery_channel, "x", 1) == PaganiAdspDown);
    CHECK(logged("crash: adsp: wdog bite") && logged("DSP fatal error while running"));
    PaganiAdspStatus st;
    pagani_adsp_core_status(&core, &st);
    CHECK(st.crashed && st.channels_up == 0);
    release_all();
}

static void broken_stream(void)
{
    memset(&sc, 0, sizeof sc);
    sc.corrupt = 1;
    reset();
    pagani_adsp_core_init(&core, &ops, &(PaganiAdspConfig){0, 0, {"modem_a", "modem_b"}, 2, "image", retry, 3, 0}, (volatile uint8_t *)smem, SMEM_SIZE);
    register_clients();
    CHECK(pagani_adsp_core_boot(&core) == 1);
    run_for(2000);
    CHECK(core.glink_stopped && battery.downs == 1 && logged("glink stream broken") && !strcmp(core.status.note, "glink stream broken"));
    CHECK(pagani_adsp_core_send(&core, audio_channel, "x", 1) == PaganiAdspDown);
    release_all();
}

/* Everything of SMEM the core may see (all but the PROT_NONE pages), for "stages 1 to 3 write nothing". */
static uint64_t smem_fingerprint(void)
{
    static const uint32_t ranges[3][2] = {{kGlobal, kOther}, {kPart, kRest}, {kTocPage, SMEM_SIZE}};
    uint64_t h = 1469598103934665603ull;
    for (unsigned r = 0; r < 3; ++r)
        for (uint32_t i = ranges[r][0]; i < ranges[r][1]; ++i) {h ^= smem[i]; h *= 1099511628211ull;}
    return h;
}

/* One failing or stopping scenario: the stage, the error and what was undone. */
static void expect_fail(const char *what, Scenario s, int32_t stage, int32_t error, const char *undone)
{
    sc = s;
    reset();
    const uint64_t smem_before = smem_fingerprint();
    const unsigned rings_before = rings_total, takes_before = hwlock_takes;
    CHECK(boot() == 0);
    /* Up to the survey (stopped there or failed there) only reads: SMEM unchanged, no TCSR mutex, no doorbell, and no IPCC, AOSS,
     * RPMh, secure-world or carveout operation at all (the sequence records each of those). */
    if (stage >= -PaganiAdspStSurveyed && stage <= PaganiAdspStSurveyed)
        CHECK(smem_fingerprint() == smem_before && rings_total == rings_before && hwlock_takes == takes_before && !sequence[0]);
    if (core.status.stage != stage || core.status.error != error) {
        fprintf(stderr, "%s: stage %d error %d, expected %d %d\n", what, core.status.stage, core.status.error, stage, error);
        dump_log();
        exit(1);
    }
    if (undone && !strstr(sequence, undone)) {fprintf(stderr, "%s: sequence \"%s\" lacks \"%s\"\n", what, sequence, undone); exit(1);}
    CHECK(!mapped && !hwlock_held && core.refused == 0 && core.dead);
    CHECK(pagani_adsp_core_send(&core, core.qrtr.channel, "x", 1) == PaganiAdspDown);   /* nothing queues after a failed bring-up */
    release_all();
    printf("  %-28s st=%d err=%d\n", what, stage, error);
}

/* The stretches around first touches (AdspCore.h): each opened by its line before the touch, the writer held at the touch, let go after ready,
 * at a stop and after a failure; without the UFS log (crumb 0) nothing is held; no quiet op, not even the lines. */
static void stretches(void)
{
    PaganiAdspOps watched = ops;
    watched.quiet = op_quiet;
    memset(&sc, 0, sizeof sc);
    reset();
    quiet_watch = 1;
    quiet_holds = 0;
    pagani_adsp_core_init(&core, &watched, &(PaganiAdspConfig){0, 0, {"modem_a", "modem_b"}, 2, "image", retry, 3, .crumb_ms = 3500}, (volatile uint8_t *)smem, SMEM_SIZE);
    const uint64_t began = now_ms;
    CHECK(pagani_adsp_core_boot(&core) == 1 && !quiet_held && quiet_holds == 3 && !core.held);
    CHECK(now_ms - began >= 3 * (3500 + PAGANI_ADSP_QUIET_MS));
    CHECK(strstr(logbuf, "[adsp] guard smem-table") && strstr(logbuf, "[adsp] guard smem\n") && strstr(logbuf, "[adsp] guard lpass"));
    CHECK(strstr(logbuf, "[adsp] guard smem-table") < strstr(logbuf, "[adsp] toc "));
    CHECK(strstr(logbuf, "[adsp] guard smem\n") < strstr(logbuf, "[adsp] smem touch"));
    CHECK(strstr(logbuf, "[adsp] guard lpass") < strstr(logbuf, "qmp load_state adsp on"));
    release_all();
    /* a stop inside each stretch lets go; so does a failure in the last one (no ready) */
    const uint32_t stops[] = {PaganiAdspStMapped, PaganiAdspStSurveyed, PaganiAdspStDoorbells, PaganiAdspStRails};
    for (unsigned i = 0; i < 4; ++i) {
        memset(&sc, 0, sizeof sc);
        reset();
        pagani_adsp_core_init(&core, &watched, &(PaganiAdspConfig){stops[i], 0, {"modem_a", "modem_b"}, 2, "image", retry, 3, .crumb_ms = 3500}, (volatile uint8_t *)smem, SMEM_SIZE);
        CHECK(pagani_adsp_core_boot(&core) == 0 && !quiet_held && !core.held);
        release_all();
    }
    quiet_watch = 0;
    memset(&sc, 0, sizeof sc);
    sc.never_ready = 1;
    reset();
    pagani_adsp_core_init(&core, &watched, &(PaganiAdspConfig){0, 0, {"modem_a", "modem_b"}, 2, "image", retry, 3, .crumb_ms = 3500}, (volatile uint8_t *)smem, SMEM_SIZE);
    CHECK(pagani_adsp_core_boot(&core) == 0 && !quiet_held && !core.held);
    release_all();
    memset(&sc, 0, sizeof sc);
    reset();
    quiet_holds = 0;
    pagani_adsp_core_init(&core, &watched, &(PaganiAdspConfig){0, 0, {"modem_a", "modem_b"}, 2, "image", retry, 3, 0}, (volatile uint8_t *)smem, SMEM_SIZE);
    const uint64_t quick = now_ms;
    CHECK(pagani_adsp_core_boot(&core) == 1 && quiet_holds == 0 && now_ms - quick < 3500 && strstr(logbuf, "[adsp] guard lpass"));
    release_all();
    memset(&sc, 0, sizeof sc);
    reset();
    CHECK(boot() == 1 && !strstr(logbuf, "[adsp] guard"));
    release_all();
    printf("  stretches: 3 holds, let go at each stop and failure, none without the log\n");
}

int main(int argc, char **argv)
{
    CHECK(argc >= 3);
    verbose = argc > 3;
    FILE *f = fopen(argv[1], "rb");
    CHECK(f);
    fseek(f, 0, SEEK_END);
    volume_size = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    volume = malloc(volume_size);
    CHECK(fread(volume, 1, volume_size, f) == volume_size);
    fclose(f);
    const char *names[2] = {"adsp_dtb", "adsp"};
    for (int i = 0; i < 2; ++i) {
        char path[512];
        snprintf(path, sizeof path, "%s/%s.meta", argv[2], names[i]);
        f = fopen(path, "rb");
        CHECK(f);
        fseek(f, 0, SEEK_END);
        expect_meta_size[i] = (size_t)ftell(f);
        fseek(f, 0, SEEK_SET);
        expect_meta[i] = malloc(expect_meta_size[i]);
        CHECK(fread(expect_meta[i], 1, expect_meta_size[i], f) == expect_meta_size[i]);
        fclose(f);
        snprintf(path, sizeof path, "%s/%s.region", argv[2], names[i]);
        f = fopen(path, "rb");
        CHECK(f);
        fseek(f, 0, SEEK_END);
        expect_region_size[i] = (size_t)ftell(f);
        fseek(f, 0, SEEK_SET);
        expect_region[i] = malloc(expect_region_size[i]);
        CHECK(fread(expect_region[i], 1, expect_region_size[i], f) == expect_region_size[i]);
        fclose(f);
    }
    region_dtb = malloc(PAGANI_ADSP_DTB_SIZE);
    region_adsp = malloc(PAGANI_ADSP_REGION_SIZE);
    CHECK(region_dtb && region_adsp);

    happy(1);
    happy(0);
    stretches();
    silent_interrupt();
    lost_doorbells();
    crash_while_running();
    broken_stream();

    printf("failures and stops:\n");
    const Scenario none = {0};
    Scenario s;
    s = none; s.toc_bad = 1;          expect_fail("no partition table", s, -1, QcomSmemNoTable, 0);
    s = none; s.no_partition = 1;     expect_fail("no apps-adsp partition", s, -1, QcomSmemNoPartition, 0);
    s = none; s.stop = 1;             expect_fail("stop after the table", s, 1, PaganiAdspErrStopped, 0);
    s = none; s.fw_not_ready = 2;     s.stop = 2;
    expect_fail("storage late, firmware read", s, 2, PaganiAdspErrStopped, 0);
    CHECK(logged("firmware partition not found yet") && logged("fw part=modem_a lun=4 first=4870"));
    s = none; s.fw_not_ready = 99;    expect_fail("no firmware partition", s, -2, PaganiFwNoPartition, 0);
    s = none; s.stop = 3;             expect_fail("stop after the survey", s, 3, PaganiAdspErrStopped, 0);
    CHECK(logged("survey items=0x0 slave=-1 qmp=LIAM") && logged("smem touch adsp=0x100000"));
    s = none; s.running_already = 1;  expect_fail("DSP already running", s, -3, PaganiAdspErrRunning, 0);
    s = none; s.hwlock_busy = 1;      expect_fail("TCSR mutex taken", s, -4, PaganiAdspErrLock, 0);
    s = none; s.stop = 5;             expect_fail("stop after the doorbells", s, 5, PaganiAdspErrStopped, 0);
    s = none; s.qmp_rc = -110;        expect_fail("AOP silent", s, -6, -110, "qmp:on");
    CHECK(!strstr(sequence, "rails") && !strstr(sequence, "qmp:off"));
    s = none; s.stop = 7;             expect_fail("stop after the votes", s, 7, PaganiAdspErrStopped, "rails:on rails:off qmp:off");
    s = none; s.rails_rc = -16;       expect_fail("DRV-2 busy", s, -7, -16, "qmp:on rails:on qmp:off");
    s = none; s.dtb_init_rc = -1002;  expect_fail("dtb init_image refused", s, -8, -1002, "init:24 rails:off qmp:off");
    CHECK(!strstr(sequence, "shutdown") && logged("pas init_image failed pas=0x24 x0=-2"));
    s = none; s.dtb_auth_rc = -1006;  expect_fail("dtb auth refused", s, -8, -1006, "auth:24 rails:off qmp:off");
    CHECK(!strstr(sequence, "shutdown"));
    s = none; s.adsp_init_rc = -1002; expect_fail("adsp init_image refused", s, -9, -1002, "init:1 shutdown:24 rails:off qmp:off");
    s = none; s.adsp_setup_rc = -1003; expect_fail("adsp mem_setup refused", s, -9, -1003, "setup:1 shutdown:24 rails:off qmp:off");
    s = none; s.adsp_auth_rc = -1006; expect_fail("adsp auth refused", s, -10, -1006, "auth:1 shutdown:24 rails:off qmp:off");
    CHECK(kept[0] && kept[1] && !released[0]);
    /* A secure call that ran out of its budget: its session stays parked, nothing more is asked of the secure world, and what may run
     * keeps its power. */
    s = none; s.adsp_auth_rc = PaganiAdspErrScmParked;
    expect_fail("adsp auth out of budget", s, -10, PaganiAdspErrScmParked, "auth:1");
    CHECK(!strstr(sequence, "shutdown") && !strstr(sequence, "rails:off") && !strstr(sequence, "qmp:off") && logged("scm_parked=1"));
    s = none; s.dtb_init_rc = PaganiAdspErrScmParked;
    expect_fail("dtb init out of budget", s, -8, PaganiAdspErrScmParked, "init:24");
    CHECK(!strstr(sequence, "shutdown") && !strstr(sequence, "rails:off") && !strstr(sequence, "qmp:off"));
    /* A shutdown that fails: the votes and load_state stay (no rail pulled from under a Q6 that may run). */
    s = none; s.never_ready = 1; s.no_handover = 1; s.shutdown_rc = -1006;
    expect_fail("never ready, shutdown fails", s, -11, PaganiAdspErrTimeout, "shutdown:1 shutdown:24");
    CHECK(!strstr(sequence, "rails:off") && !strstr(sequence, "qmp:off") && logged("kept_votes=1") && core.votes_on && core.load_state_on);
    s = none; s.never_ready = 1;      expect_fail("never ready", s, -11, PaganiAdspErrTimeout, "auth:1 rails:off shutdown:1 shutdown:24 qmp:off");
    CHECK(logged("handover: proxy votes dropped") && logged("FAIL st=-11 err=-60 no ready"));
    s = none; s.never_ready = 1; s.no_handover = 1;
    expect_fail("never ready, no handover", s, -11, PaganiAdspErrTimeout, "shutdown:1 shutdown:24 rails:off qmp:off");
    s = none; s.fatal_at_boot = 1;    expect_fail("fatal at boot", s, -11, PaganiAdspErrFatal, "shutdown:1 shutdown:24 rails:off qmp:off");
    CHECK(!strcmp(core.status.note, "crash: audio_pd: err_fatal at boot") && logged("crash: audio_pd: err_fatal at boot"));
    s = none; s.no_fifo = 1;
    {
        sc = s;
        reset();
        CHECK(boot() == 1);
        run_for(6000);
        CHECK(logged("glink: no version answer after 5 s rx_fifo=-1"));
        release_all();
        printf("  %-28s logged\n", "DSP never makes its FIFO");
    }
    s = none; s.handover_first = 1;
    {
        sc = s;
        reset();
        CHECK(boot() == 1);
        CHECK(strstr(sequence, "auth:1 rails:off") != 0);   /* dropped once, before ready */
        run_for(500);
        CHECK(core.status.stage == PaganiAdspStChannel);
        release_all();
        printf("  %-28s ok\n", "handover before ready");
    }
    munmap(smem, SMEM_SIZE);
    free(region_dtb); free(region_adsp); free(volume);
    for (int i = 0; i < 2; ++i) {free(expect_meta[i]); free(expect_region[i]);}
    printf("PASS adsp core: bring-up, run time, IPC router and %u failure paths: %u checks\n", 26u, checks);
    return 0;
}
