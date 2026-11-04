/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved.
 * The sequence and the client queues follow UniXNU-sheng package/AdspProbe.cpp (https://github.com/UniXNU/UniXNU-sheng, commit
 * 08cde20e); see AdspCore.h for the 13T stages. */
#include "AdspCore.h"
#include "AdspUtil.h"

static const char kLoadStateOn[] = "{class: image, res: load_state, name: adsp, val: on}";
static const char kLoadStateOff[] = "{class: image, res: load_state, name: adsp, val: off}";
static const char kEntryStop[] = "master-kernel", kEntrySleep[] = "sleepstate", kEntrySlave[] = "slave-kernel";

#define CTX (c->ops->context)

/* ---- the log: one "[adsp] ..." line at a time, built in c->line ---- */
static void line_begin(PaganiAdspCore *c, PaganiText *t, const char *what)
{
    pagani_text_start(t, c->line, sizeof c->line);
    pagani_text_str(t, "[adsp] ");
    pagani_text_str(t, what);
}
static void line_end(PaganiAdspCore *c)
{
    if (c->ops->trace) c->ops->trace(CTX, c->line);
}
void pagani_adsp_core_trace(PaganiAdspCore *c, const char *text)
{
    PaganiText t;
    line_begin(c, &t, text);
    line_end(c);
}
static void note(PaganiAdspCore *c, const char *text)
{
    pagani_strcpy(c->status.note, sizeof c->status.note, text);
}
static void stage(PaganiAdspCore *c, int32_t value)
{
    c->status.stage = value;
}
static void scm_detail(PaganiAdspCore *c, PaganiText *t)
{
    int64_t x0 = 0;
    uint64_t x1 = 0;
    if (!c->ops->pas_result) return;
    c->ops->pas_result(CTX, &x0, &x1);
    pagani_text_kd(t, "x0", x0);
    pagani_text_kx(t, "x1", x1);
}
/* The stretches around first touches (AdspCore.h): the line, its time to reach the UFS log, the writer held, a round under way given its
 * time to end. let_go ends a stretch; one that hangs never gets there. */
static void let_go(PaganiAdspCore *c)
{
    if (!c->held) return;
    c->held = 0;
    c->ops->quiet(CTX, 0);
}
static void guard(PaganiAdspCore *c, const char *what)
{
    if (!c->ops->quiet) return;
    let_go(c);
    PaganiText t;
    line_begin(c, &t, "guard ");
    pagani_text_str(&t, what);
    line_end(c);
    if (!c->config.crumb_ms) return;
    c->ops->sleep_ms(CTX, c->config.crumb_ms);
    c->held = 1;
    c->ops->quiet(CTX, 1);
    c->ops->sleep_ms(CTX, PAGANI_ADSP_QUIET_MS);
}

/* Records the failure of stage `at` and logs it with the reason. */
static int fail(PaganiAdspCore *c, int32_t at, int32_t error, const char *why)
{
    PaganiText t;
    c->status.stage = -at;
    c->status.error = error;
    line_begin(c, &t, "FAIL");
    pagani_text_kd(&t, "st", -at);
    pagani_text_kd(&t, "err", error);
    pagani_text_char(&t, ' ');
    pagani_text_str(&t, why);
    line_end(c);
    return 0;
}

/* ---- events ---- */
static void service_smp2p(PaganiAdspCore *c);
static void service_glink(PaganiAdspCore *c);

void pagani_adsp_core_service(PaganiAdspCore *c, uint32_t events)
{
    /* The wait ran out without an event: a look at everything, so that one lost doorbell delays the link instead of stalling it for good
     * (at most one idle period: a minute, a second with a send waiting). */
    if (!events) events = PaganiAdspEvPoll;
    if (events & (PaganiAdspEvSmp2p | PaganiAdspEvGlink | PaganiAdspEvPoll)) service_smp2p(c);
    if (c->glink_started && (events & (PaganiAdspEvGlink | PaganiAdspEvSend | PaganiAdspEvPoll))) service_glink(c);
    if (c->glink_check_ms && !c->glink.version_agreed && c->ops->now_ms(CTX) >= c->glink_check_ms) {
        PaganiText t;
        c->glink_check_ms = 0;
        line_begin(c, &t, "glink: no version answer after 5 s");
        pagani_text_kd(&t, "rx_fifo", c->glink.rx.data ? (int64_t)c->glink.rx.length : -1);
        pagani_text_kd(&t, "in", c->glink.rx_messages);
        pagani_text_kd(&t, "out", c->glink.tx_messages);
        line_end(c);
        note(c, "glink: no version answer");
    }
}

/* Runs the events until done(c) holds or ms pass; returns done(c). Without the interrupt the wait is a 5 ms poll. */
static int wait_until(PaganiAdspCore *c, uint32_t ms, int (*done)(PaganiAdspCore *))
{
    const uint64_t deadline = c->ops->now_ms(CTX) + ms;
    while (!done(c)) {
        const uint64_t now = c->ops->now_ms(CTX);
        if (now >= deadline) {
            pagani_adsp_core_service(c, PaganiAdspEvPoll);   /* one last look: a lost edge must not fail the step */
            return done(c);
        }
        uint32_t left = (uint32_t)(deadline - now);
        if (!c->irq_ok && left > 5) left = 5;
        uint32_t events = c->ops->wait(CTX, left);
        if (!c->irq_ok) events |= PaganiAdspEvPoll;
        pagani_adsp_core_service(c, events);
    }
    return 1;
}

/* ---- SMEM helpers ---- */
static int item(PaganiAdspCore *c, const QcomSmemPartition *part, uint16_t id, uint32_t *offset, uint32_t *size)
{
    return qcom_smem_find(&c->smem, part, id, offset, size);
}
static int allocate(PaganiAdspCore *c, uint16_t id, uint32_t size)
{
    if (!c->ops->hwlock(CTX, 1)) return PaganiAdspErrLock;
    const int rc = qcom_smem_alloc(&c->smem, &c->part, id, size);
    c->ops->hwlock(CTX, 0);
    return rc == QcomSmemExists ? 0 : rc;
}
/* The crash reason (global item 423) into the status note and the log. */
static void crash_reason(PaganiAdspCore *c)
{
    uint32_t at = 0, size = 0;
    char text[sizeof c->status.note];
    unsigned n = 0;
    if (!c->have_global || item(c, &c->global, PaganiAdspItemCrash, &at, &size) || !size) return;
    for (uint32_t w = 0; w < size && n + 1 < sizeof text; w += 4) {
        const uint32_t word = qcom_smem_read32(&c->smem, at + w);
        for (unsigned b = 0; b < 4 && w + b < size && n + 1 < sizeof text; ++b) {
            const char ch = (char)(word >> (8 * b));
            if (!ch) {w = size; break;}
            text[n++] = ch >= 32 && ch < 127 ? ch : '.';
        }
    }
    text[n] = 0;
    if (!n) return;
    PaganiText t;
    pagani_text_start(&t, c->status.note, sizeof c->status.note);
    pagani_text_str(&t, "crash: ");
    pagani_text_str(&t, text);
    line_begin(c, &t, "crash: ");
    pagani_text_str(&t, text);
    line_end(c);
}

/* ---- 1: the partition table (only its page is read) ---- */
static int table(PaganiAdspCore *c)
{
    PaganiText t;
    QcomSmem *s = &c->smem;
    if (qcom_smem_allow(s, s->size - QCOM_SMEM_TOC_BYTES, QCOM_SMEM_TOC_BYTES)) return fail(c, PaganiAdspStMapped, QcomSmemGuard, "smem window");
    const int count = qcom_smem_toc_count(s);
    if (count < 0) return fail(c, PaganiAdspStMapped, count, "no SMEM partition table");
    /* The whole table, six entries a line: hosts@offset+size. */
    unsigned listed = 0;
    line_begin(c, &t, "toc");
    pagani_text_kd(&t, "n", count);
    for (uint32_t i = 0; i < (uint32_t)count; ++i) {
        QcomSmemPartition p;
        if (qcom_smem_toc_entry(s, i, &p)) continue;
        if (listed && !(listed % 6)) {
            line_end(c);
            line_begin(c, &t, "toc+");
        }
        listed++;
        pagani_text_char(&t, ' ');
        pagani_text_hex(&t, p.host0);
        pagani_text_char(&t, '/');
        pagani_text_hex(&t, p.host1);
        pagani_text_char(&t, '@');
        pagani_text_hex(&t, p.offset);
        pagani_text_char(&t, '+');
        pagani_text_hex(&t, p.size);
    }
    line_end(c);
    c->have_global = qcom_smem_toc_find(s, QCOM_SMEM_HOST_GLOBAL, QCOM_SMEM_HOST_GLOBAL, &c->global) == QcomSmemOk;
    const int rc = qcom_smem_toc_find(s, QCOM_SMEM_HOST_APPS, PaganiAdspRemotePid, &c->part);
    if (rc) return fail(c, PaganiAdspStMapped, rc, "no apps-adsp SMEM partition");
    line_begin(c, &t, "smem part");
    pagani_text_kx(&t, "adsp", c->part.offset);
    pagani_text_char(&t, '+');
    pagani_text_hex(&t, c->part.size);
    pagani_text_kd(&t, "cl", c->part.cacheline);
    if (c->have_global) {
        pagani_text_kx(&t, "global", c->global.offset);
        pagani_text_char(&t, '+');
        pagani_text_hex(&t, c->global.size);
    }
    line_end(c);
    stage(c, PaganiAdspStMapped);
    return 1;
}

/* ---- 2: firmware (storage reads only) ---- */
static void image_line(PaganiAdspCore *c, const PaganiFwImage *image, uint64_t base, uint64_t size)
{
    PaganiText t;
    line_begin(c, &t, "fw ");
    pagani_text_str(&t, image->name);
    pagani_text_ks(&t, "hdr", image->from_b00 ? "b00" : "mdt");
    pagani_text_kd(&t, "phdrs", image->elf.count);
    pagani_text_kd(&t, "load", image->loadable);
    pagani_text_kd(&t, "files", image->pieces);
    pagani_text_kd(&t, "KiB", (int64_t)(image->file_bytes >> 10));
    pagani_text_kd(&t, "meta", (int64_t)pagani_fw_metadata_size(image));
    pagani_text_kd(&t, "reloc", image->elf.relocatable);
    pagani_text_kx(&t, "span", pagani_fw_span(image));
    pagani_text_kx(&t, "end", (image->elf.relocatable ? base : 0) + (image->elf.relocatable ? pagani_fw_span(image) : image->elf.max_address));
    pagani_text_kx(&t, "region", base);
    pagani_text_char(&t, '+');
    pagani_text_hex(&t, size);
    line_end(c);
}
static int open_image(PaganiAdspCore *c, PaganiFwImage *image, const char *name, uint64_t base, uint64_t size)
{
    PaganiText t;
    int rc = pagani_fw_open(image, c->volume, c->config.directory, name);
    if (!rc) rc = pagani_fw_check(image, base, size);
    if (rc) {
        line_begin(c, &t, "fw open failed");
        pagani_text_ks(&t, "path", image->path);
        pagani_text_kd(&t, "rc", rc);
        pagani_text_kd(&t, "detail", image->detail);
        line_end(c);
        return fail(c, PaganiAdspStFirmware, rc, name);
    }
    image_line(c, image, base, size);
    return 1;
}
static void release_firmware(PaganiAdspCore *c)
{
    if (c->dtb) {pagani_fw_close(c->dtb); c->ops->free(CTX, c->dtb, sizeof *c->dtb);}
    if (c->adsp) {pagani_fw_close(c->adsp); c->ops->free(CTX, c->adsp, sizeof *c->adsp);}
    if (c->volume) c->ops->free(CTX, c->volume, sizeof *c->volume);
    c->dtb = c->adsp = 0;
    c->volume = 0;
}
static int firmware(PaganiAdspCore *c)
{
    PaganiText t;
    c->volume = (PaganiFwVolume *)c->ops->alloc(CTX, sizeof *c->volume);
    c->dtb = (PaganiFwImage *)c->ops->alloc(CTX, sizeof *c->dtb);
    c->adsp = (PaganiFwImage *)c->ops->alloc(CTX, sizeof *c->adsp);
    if (!c->volume || !c->dtb || !c->adsp) return fail(c, PaganiAdspStFirmware, PaganiAdspErrNoMemory, "firmware memory");
    const uint64_t began = c->ops->now_ms(CTX);
    int rc;
    for (unsigned attempt = 0;; ++attempt) {
        rc = pagani_fw_mount(c->volume, &c->ops->disk, c->config.partitions, c->config.partition_count);
        if (rc != PaganiFwNoPartition || attempt >= c->config.retries || !c->config.retry_ms) break;
        if (!attempt) pagani_adsp_core_trace(c, "fw: firmware partition not found yet (storage starting?), retrying");
        c->ops->sleep_ms(CTX, c->config.retry_ms[attempt]);
    }
    if (rc) {
        line_begin(c, &t, "fw mount failed");
        pagani_text_ks(&t, "part", c->config.partitions[0]);
        pagani_text_kd(&t, "rc", rc);
        pagani_text_kd(&t, "fat", c->volume->disk_failed ? -1 : 0);
        line_end(c);
        return fail(c, PaganiAdspStFirmware, rc, "firmware partition");
    }
    line_begin(c, &t, "fw part=");
    pagani_text_str(&t, c->volume->partition);
    pagani_text_kd(&t, "lun", c->volume->lun);
    pagani_text_kd(&t, "first", (int64_t)c->volume->first);
    pagani_text_kd(&t, "blocks", (int64_t)c->volume->blocks);
    pagani_text_ks(&t, "fat", c->volume->fat.fat32 ? "32" : "16");
    pagani_text_kd(&t, "sector", c->volume->fat.sector_bytes);
    pagani_text_kd(&t, "cluster", c->volume->fat.cluster_bytes);
    line_end(c);
    if (!open_image(c, c->dtb, "adsp_dtb", PAGANI_ADSP_DTB_BASE, PAGANI_ADSP_DTB_SIZE) ||
        !open_image(c, c->adsp, "adsp", PAGANI_ADSP_REGION_BASE, PAGANI_ADSP_REGION_SIZE))
        return 0;
    c->status.firmware_kib = (uint32_t)((c->dtb->file_bytes + c->adsp->file_bytes) >> 10);
    line_begin(c, &t, "fw parsed");
    pagani_text_kd(&t, "ms", (int64_t)(c->ops->now_ms(CTX) - began));
    pagani_text_kd(&t, "reads", c->volume->reads);
    line_end(c);
    stage(c, PaganiAdspStFirmware);
    return 1;
}

/* ---- 3: survey (the first access to the apps-ADSP and global partitions) ---- */
static int survey(PaganiAdspCore *c)
{
    PaganiText t;
    QcomSmem *s = &c->smem;
    /* The line is in DRAM before the access: if this page aborts, the log says where. */
    line_begin(c, &t, "smem touch");
    pagani_text_kx(&t, "adsp", c->part.offset);
    pagani_text_kx(&t, "global", c->have_global ? c->global.offset : 0);
    line_end(c);
    if (qcom_smem_allow(s, c->part.offset, c->part.size)) return fail(c, PaganiAdspStSurveyed, PaganiAdspErrGuard, "smem guard");
    if (c->have_global && qcom_smem_allow(s, c->global.offset, c->global.size)) c->have_global = 0;
    int rc = qcom_smem_partition(s, QCOM_SMEM_HOST_APPS, PaganiAdspRemotePid, &c->part);
    if (rc) return fail(c, PaganiAdspStSurveyed, rc, "apps-adsp partition header");
    if (c->have_global && qcom_smem_partition(s, QCOM_SMEM_HOST_GLOBAL, QCOM_SMEM_HOST_GLOBAL, &c->global)) {
        c->have_global = 0;
        pagani_adsp_core_trace(c, "smem: global partition header unusable (no crash reasons)");
    }
    static const uint16_t ids[5] = {PaganiAdspItemSmp2pIn, PaganiAdspItemSmp2pOut, PaganiAdspItemGlinkDesc, PaganiAdspItemGlinkTx,
                                    PaganiAdspItemGlinkRx};
    uint32_t items = 0, at = 0, size = 0;
    for (unsigned i = 0; i < 5; ++i) {
        rc = item(c, &c->part, ids[i], &at, &size);
        if (rc == QcomSmemOk) items |= 1u << i;
        else if (rc != QcomSmemMissing) return fail(c, PaganiAdspStSurveyed, rc, "apps-adsp items");
    }
    c->status.slave = -1;
    if (items & 1) {
        QcomSmp2p view;
        pagani_zero(&view, sizeof view);
        rc = item(c, &c->part, PaganiAdspItemSmp2pIn, &at, &size);
        if (!rc && !qcom_smp2p_set_in(&view, (volatile uint32_t *)(s->base + at), size) &&
            !qcom_smp2p_check_in(view.in, 0, PaganiAdspRemotePid)) {
            const int index = qcom_smp2p_find_in(&view, kEntrySlave);
            if (index >= 0) c->status.slave = (int32_t)qcom_smp2p_in_value(&view, index);
        }
    }
    crash_reason(c);
    PaganiAdspSurvey hw;
    pagani_zero(&hw, sizeof hw);
    if (c->ops->survey) c->ops->survey(CTX, &hw);
    line_begin(c, &t, "survey");
    pagani_text_kx(&t, "items", items);
    pagani_text_kd(&t, "slave", c->status.slave);
    pagani_text_ks(&t, "qmp", hw.aoss_magic == 0x4d41494cu ? "LIAM" : "?");
    pagani_text_kx(&t, "qmp_magic", hw.aoss_magic);
    pagani_text_kd(&t, "qmp_ver", hw.aoss_version);
    pagani_text_kx(&t, "links", hw.aoss_links);
    pagani_text_kx(&t, "drv2", hw.rsc_id);
    pagani_text_kd(&t, "refused", c->refused);
    line_end(c);
    if (c->refused) return fail(c, PaganiAdspStSurveyed, PaganiAdspErrGuard, "smem access outside the readable ranges");
    if ((c->status.slave > 0) || (items & (1u << 4))) return fail(c, PaganiAdspStSurveyed, PaganiAdspErrRunning, "DSP already running");
    stage(c, PaganiAdspStSurveyed);
    return 1;
}

/* ---- 4..11: the start ---- */
/* The OnePlus sensor items of the global partition. On Android two kernel modules make them before the DSP starts (vendor_boot's
 * oplus_sensor_deviceinfo.ko and oplus_sensor_feedback.ko, build-userspace/a830-inputs/sensors/ssc-facts.md 6.2): 130, the device
 * information the sensor hub reads, all zero on the 13T but the magnetic fusion's block, and 128, zeroed, where the hub leaves its feedback
 * records. Whether the sensor PD needs them is not known; they cost nothing, and a failure here is only a line. An item that is already
 * there is not touched. */
static void oplus_sensor_items(PaganiAdspCore *c)
{
    enum {kDevinfo = 130, kDevinfoBytes = 9016, kFeedback = 128, kFeedbackBytes = 3488, kFusionType = 8764, kFusionMag = 6, kFusionDropZero = 8836};
    uint32_t at = 0, size = 0;
    PaganiText t;
    if (!c->have_global) return;
    int devinfo = PaganiAdspErrLock, feedback = PaganiAdspErrLock;
    if (c->ops->hwlock(CTX, 1)) {
        devinfo = qcom_smem_alloc(&c->smem, &c->global, kDevinfo, kDevinfoBytes);
        feedback = qcom_smem_alloc(&c->smem, &c->global, kFeedback, kFeedbackBytes);
        c->ops->hwlock(CTX, 0);
    }
    if (devinfo == QcomSmemOk && !item(c, &c->global, kDevinfo, &at, &size) && size >= kDevinfoBytes) {
        for (uint32_t i = 0; i < kDevinfoBytes; i += 4) qcom_smem_write32(&c->smem, at + i, 0);
        qcom_smem_write32(&c->smem, at + kFusionType, kFusionMag);
        qcom_smem_write32(&c->smem, at + kFusionDropZero, 1);
    }
    if (feedback == QcomSmemOk && !item(c, &c->global, kFeedback, &at, &size) && size >= kFeedbackBytes)
        for (uint32_t i = 0; i < kFeedbackBytes; i += 4) qcom_smem_write32(&c->smem, at + i, 0);
    line_begin(c, &t, "smem oplus sensor items");
    pagani_text_kd(&t, "130", devinfo);
    pagani_text_kd(&t, "128", feedback);
    line_end(c);
}

static int smp2p(PaganiAdspCore *c)
{
    uint32_t at = 0, size = 0;
    oplus_sensor_items(c);
    int rc = allocate(c, PaganiAdspItemSmp2pOut, QCOM_SMP2P_ITEM_BYTES);
    if (!rc) rc = item(c, &c->part, PaganiAdspItemSmp2pOut, &at, &size);
    if (rc) return fail(c, PaganiAdspStSmp2p, rc, "smp2p item 429");
    if (size < QCOM_SMP2P_ITEM_BYTES) return fail(c, PaganiAdspStSmp2p, PaganiAdspErrItem, "smp2p item 429 too small");
    qcom_smp2p_init_out(&c->p2p, (volatile uint32_t *)(c->smem.base + at), 0, PaganiAdspRemotePid);
    c->stop_entry = qcom_smp2p_add_entry(&c->p2p, kEntryStop);
    c->sleep_entry = qcom_smp2p_add_entry(&c->p2p, kEntrySleep);
    if (c->sleep_entry >= 0) qcom_smp2p_update(&c->p2p, c->sleep_entry, 1, 1);   /* awake, as Android's sleepstate driver */
    PaganiText t;
    line_begin(c, &t, "smp2p out");
    pagani_text_kx(&t, "item", at);
    pagani_text_kd(&t, "size", size);
    line_end(c);
    stage(c, PaganiAdspStSmp2p);
    return 1;
}
static int doorbells(PaganiAdspCore *c)
{
    const int rc = c->ops->doorbells(CTX);
    c->irq_ok = rc == 0;
    if (rc) {
        PaganiText t;
        line_begin(c, &t, "ipcc interrupt unavailable, polling");
        pagani_text_kd(&t, "rc", rc);
        line_end(c);
    }
    c->ops->ring(CTX, PaganiAdspSignalSmp2p);
    stage(c, PaganiAdspStDoorbells);
    return 1;
}
static int load_state(PaganiAdspCore *c)
{
    const int rc = c->ops->qmp(CTX, kLoadStateOn);
    if (rc) return fail(c, PaganiAdspStLoadState, rc, "AOSS load_state on");
    c->load_state_on = 1;
    pagani_adsp_core_trace(c, "qmp load_state adsp on");
    stage(c, PaganiAdspStLoadState);
    return 1;
}
static int rails(PaganiAdspCore *c)
{
    const int rc = c->ops->rails(CTX, 1);
    if (rc) return fail(c, PaganiAdspStRails, rc, "lcx/lmx proxy votes");
    c->votes_on = 1;
    stage(c, PaganiAdspStRails);
    return 1;
}
/* Every secure call's result passes here: one that ran out of its budget leaves its session parked in the secure world. */
static int scm_seen(PaganiAdspCore *c, int rc)
{
    if (rc == PaganiAdspErrScmParked) c->scm_parked = 1;
    return rc;
}
/* init_image with the metadata, mem_setup of a relocatable image, the segments into the carveout. */
static int pas_image(PaganiAdspCore *c, PaganiFwImage *image, uint32_t pas, uint64_t base, uint64_t size, int32_t at_stage)
{
    PaganiText t;
    const uint64_t began = c->ops->now_ms(CTX), bytes = pagani_fw_metadata_size(image);
    uint8_t *meta = bytes ? (uint8_t *)c->ops->alloc(CTX, (size_t)bytes) : 0;
    if (!meta) return fail(c, at_stage, PaganiAdspErrNoMemory, "metadata buffer");
    int rc = pagani_fw_metadata(image, meta, bytes);
    if (rc) {
        c->ops->free(CTX, meta, (size_t)bytes);
        return fail(c, at_stage, rc, "metadata read");
    }
    rc = scm_seen(c, c->ops->pas_init(CTX, pas, meta, bytes));
    c->ops->free(CTX, meta, (size_t)bytes);
    if (rc) {
        line_begin(c, &t, "pas init_image failed");
        pagani_text_kx(&t, "pas", pas);
        scm_detail(c, &t);
        line_end(c);
        return fail(c, at_stage, rc, "init_image");
    }
    if (image->elf.relocatable && (rc = scm_seen(c, c->ops->pas_mem_setup(CTX, pas, base, pagani_fw_span(image))))) {
        line_begin(c, &t, "pas mem_setup failed");
        pagani_text_kx(&t, "pas", pas);
        scm_detail(c, &t);
        line_end(c);
        return fail(c, at_stage, rc, "mem_setup");
    }
    uint8_t *va = c->ops->map(CTX, base, size);
    if (!va) return fail(c, at_stage, PaganiAdspErrNoMemory, "carveout mapping");
    uint64_t written = 0;
    rc = pagani_fw_load(image, va, base, size, &written);
    c->ops->unmap(CTX, va, size);
    if (rc) {
        line_begin(c, &t, "segment load failed");
        pagani_text_ks(&t, "image", image->name);
        pagani_text_kd(&t, "segment", image->detail);
        line_end(c);
        return fail(c, at_stage, rc, "segments");
    }
    line_begin(c, &t, "pas loaded ");
    pagani_text_str(&t, image->name);
    pagani_text_kx(&t, "pas", pas);
    pagani_text_kd(&t, "KiB", (int64_t)(written >> 10));
    pagani_text_kd(&t, "ms", (int64_t)(c->ops->now_ms(CTX) - began));
    line_end(c);
    return 1;
}
static int auth(PaganiAdspCore *c, uint32_t pas, int32_t at_stage, const char *what)
{
    PaganiText t;
    const uint64_t began = c->ops->now_ms(CTX);
    const int rc = scm_seen(c, c->ops->pas_auth(CTX, pas));
    line_begin(c, &t, "pas auth_and_reset");
    pagani_text_kx(&t, "pas", pas);
    pagani_text_kd(&t, "rc", rc);
    pagani_text_kd(&t, "ms", (int64_t)(c->ops->now_ms(CTX) - began));
    scm_detail(c, &t);
    line_end(c);
    return rc ? fail(c, at_stage, rc, what) : 1;
}
static int pas(PaganiAdspCore *c)
{
    if (!pas_image(c, c->dtb, PaganiAdspPasDtb, PAGANI_ADSP_DTB_BASE, PAGANI_ADSP_DTB_SIZE, PaganiAdspStDtb)) return 0;
    if (!auth(c, PaganiAdspPasDtb, PaganiAdspStDtb, "adsp_dtb auth_and_reset")) return 0;
    c->dtb_running = 1;
    stage(c, PaganiAdspStDtb);
    if (!pas_image(c, c->adsp, PaganiAdspPasAdsp, PAGANI_ADSP_REGION_BASE, PAGANI_ADSP_REGION_SIZE, PaganiAdspStLoaded)) return 0;
    stage(c, PaganiAdspStLoaded);
    c->auth_ms = c->ops->now_ms(CTX);
    if (!auth(c, PaganiAdspPasAdsp, PaganiAdspStStarted, "adsp auth_and_reset")) return 0;
    c->adsp_running = 1;
    stage(c, PaganiAdspStStarted);
    return 1;
}
static int ready_or_fatal(PaganiAdspCore *c)
{
    return (c->slave & (PaganiAdspBitReady | PaganiAdspBitFatal)) != 0;
}
static int wait_ready(PaganiAdspCore *c)
{
    PaganiText t;
    const int seen = wait_until(c, c->config.ready_ms ? c->config.ready_ms : 5000u, ready_or_fatal);
    c->status.ready_ms = (uint32_t)(c->ops->now_ms(CTX) - c->auth_ms);
    line_begin(c, &t, "ready wait");
    pagani_text_kd(&t, "ms", c->status.ready_ms);
    pagani_text_kx(&t, "slave", c->slave);
    pagani_text_kd(&t, "handover", c->handover);
    pagani_text_kd(&t, "irqs", c->status.ipcc_interrupts);
    line_end(c);
    if (c->slave & PaganiAdspBitFatal) return fail(c, PaganiAdspStReady, PaganiAdspErrFatal, "fatal before ready");   /* reason logged */
    if (!seen) return fail(c, PaganiAdspStReady, PaganiAdspErrTimeout, "no ready");
    /* Ready, and not one doorbell in all that time (the last look of the wait found it): the interrupt attached but is not delivered.
     * Polling from here on: without it nothing would serve the link. */
    if (c->irq_ok && !__atomic_load_n(&c->status.ipcc_interrupts, __ATOMIC_RELAXED)) {
        c->irq_ok = 0;
        pagani_adsp_core_trace(c, "ipcc interrupt attached but silent, polling");
    }
    c->ops->pas_release(CTX, PaganiAdspPasDtb, 1);
    c->ops->pas_release(CTX, PaganiAdspPasAdsp, 1);
    stage(c, PaganiAdspStReady);
    return 1;
}
/* After a failed start: what the DSP had been given goes back (Linux qcom_q6v5_pas error paths), as far as that is safe: an image that
 * did not shut down, or a secure call still parked in the secure world, keeps the votes and load_state (nothing is pulled from under a
 * Q6 that may run, and the parked session gets no second call: it could only wait on it). */
static void undo(PaganiAdspCore *c)
{
    PaganiText t;
    int shutdown_adsp = 0, shutdown_dtb = 0, unvote = 0, off = 0;
    const int touched = c->adsp_running || c->dtb_running || c->votes_on || c->load_state_on;
    if (c->adsp_running && !c->scm_parked && !(shutdown_adsp = scm_seen(c, c->ops->pas_shutdown(CTX, PaganiAdspPasAdsp)))) c->adsp_running = 0;
    if (c->dtb_running && !c->scm_parked && !(shutdown_dtb = scm_seen(c, c->ops->pas_shutdown(CTX, PaganiAdspPasDtb)))) c->dtb_running = 0;
    const int keep = c->adsp_running || c->dtb_running || c->scm_parked;
    if (c->votes_on && !keep) {unvote = c->ops->rails(CTX, 0); c->votes_on = 0;}
    if (c->load_state_on && !keep) {off = c->ops->qmp(CTX, kLoadStateOff); c->load_state_on = 0;}
    if (c->ops->pas_release) {
        c->ops->pas_release(CTX, PaganiAdspPasDtb, 0);
        c->ops->pas_release(CTX, PaganiAdspPasAdsp, 0);
    }
    if (!touched) return;
    line_begin(c, &t, "undo");
    pagani_text_kd(&t, "shutdown_adsp", shutdown_adsp);
    pagani_text_kd(&t, "shutdown_dtb", shutdown_dtb);
    pagani_text_kd(&t, "unvote", unvote);
    pagani_text_kd(&t, "load_state_off", off);
    if (keep) pagani_text_kd(&t, "kept_votes", c->votes_on);
    if (c->scm_parked) pagani_text_str(&t, " scm_parked=1");
    line_end(c);
}

/* ---- 12: GLINK ---- */
static void kick_glink(void *context)
{
    PaganiAdspCore *c = (PaganiAdspCore *)context;
    c->ops->ring(CTX, PaganiAdspSignalGlink);
}
static int glink_start(PaganiAdspCore *c)
{
    uint32_t desc = 0, desc_size = 0, fifo = 0, fifo_size = 0;
    int rc = allocate(c, PaganiAdspItemGlinkDesc, 32);
    if (!rc) rc = allocate(c, PaganiAdspItemGlinkTx, PaganiAdspGlinkFifo);
    if (!rc) rc = item(c, &c->part, PaganiAdspItemGlinkDesc, &desc, &desc_size);
    if (!rc) rc = item(c, &c->part, PaganiAdspItemGlinkTx, &fifo, &fifo_size);
    if (rc) return fail(c, PaganiAdspStGlink, rc, "glink items 478/479");
    if (desc_size < 16 || fifo_size < 1024) return fail(c, PaganiAdspStGlink, PaganiAdspErrItem, "glink items too small");
    volatile uint32_t *d = (volatile uint32_t *)(c->smem.base + desc);
    /* Descriptor words: our FIFO's tail and head, then the DSP's FIFO's tail and head. */
    const QcomGlinkFifo tx = {&d[1], &d[0], c->smem.base + fifo, fifo_size}, rx = {&d[3], &d[2], 0, 0};
    qcom_glink_init(&c->glink, tx, rx, kick_glink, c);
    c->glink_started = 1;
    c->glink_check_ms = c->ops->now_ms(CTX) + 5000;
    PaganiText t;
    line_begin(c, &t, "glink start");
    pagani_text_kx(&t, "desc", desc);
    pagani_text_kx(&t, "tx", fifo);
    pagani_text_char(&t, '+');
    pagani_text_hex(&t, fifo_size);
    line_end(c);
    service_glink(c);
    return 1;
}

int pagani_adsp_core_boot(PaganiAdspCore *c)
{
    PaganiText t;
    /* Stops are honoured up to the votes: from the first secure call on, the run goes to the end (or fails and undoes). */
    const uint32_t stop = c->config.stop_after < PaganiAdspStDtb ? c->config.stop_after : 0;
    line_begin(c, &t, "start");
    pagani_text_kd(&t, "stop", stop);
    pagani_text_kd(&t, "ready_ms", c->config.ready_ms ? c->config.ready_ms : 5000u);
    pagani_text_ks(&t, "part", c->config.partitions[0]);
    line_end(c);
    guard(c, "smem-table");
    if (!table(c)) {
        let_go(c);
        c->dead = 1;
        return 0;
    }
    let_go(c);
    if (stop == PaganiAdspStMapped) goto stopped;
    if (!firmware(c)) goto failed;
    if (stop == PaganiAdspStFirmware) goto stopped;
    guard(c, "smem");
    if (!survey(c)) goto failed;
    if (stop == PaganiAdspStSurveyed) goto stopped;
    if (!smp2p(c)) goto failed;
    if (stop == PaganiAdspStSmp2p) goto stopped;
    if (!doorbells(c)) goto failed;
    if (stop == PaganiAdspStDoorbells) goto stopped;
    guard(c, "lpass");
    if (!load_state(c)) goto failed;
    if (stop == PaganiAdspStLoadState) goto undo_stop;
    if (!rails(c)) goto failed;
    if (stop == PaganiAdspStRails) goto undo_stop;
    if (!pas(c)) goto failed;
    if (!wait_ready(c)) goto failed;
    let_go(c);
    release_firmware(c);
    c->booted = 1;
    if (!glink_start(c)) c->dead = 1;   /* the DSP runs (its SMP2P events are still served), but no channel ever opens */
    return 1;
stopped:
    let_go(c);
    release_firmware(c);
    c->dead = 1;
    c->status.error = PaganiAdspErrStopped;
    line_begin(c, &t, "stopped as asked (pagani-adsp-stop)");
    pagani_text_kd(&t, "st", c->status.stage);
    line_end(c);
    return 0;
undo_stop:
    undo(c);
    goto stopped;
failed:
    undo(c);
    let_go(c);
    release_firmware(c);
    c->dead = 1;
    return 0;
}

/* ---- run time ---- */
static void service_smp2p(PaganiAdspCore *c)
{
    PaganiText t;
    if (!c->p2p.out) return;
    if (!c->p2p.in) {
        uint32_t at = 0, size = 0;
        if (item(c, &c->part, PaganiAdspItemSmp2pIn, &at, &size)) return;
        if (qcom_smp2p_set_in(&c->p2p, (volatile uint32_t *)(c->smem.base + at), size)) return;
    }
    uint32_t value = 0, changed = 0;
    if (qcom_smp2p_service(&c->p2p, kEntrySlave, &value, &changed)) c->ops->ring(CTX, PaganiAdspSignalSmp2p);
    if (qcom_smp2p_find_in(&c->p2p, kEntrySlave) < 0) return;
    c->slave = value;
    c->status.slave = (int32_t)value;
    if (changed) {
        line_begin(c, &t, "slave-kernel");
        pagani_text_kx(&t, "value", value);
        pagani_text_kx(&t, "changed", changed);
        line_end(c);
    }
    if (changed & value & PaganiAdspBitFatal) {
        c->fatal = 1;
        crash_reason(c);
        if (c->booted) {
            c->status.crashed = 1;
            if (c->glink_started && !c->glink_stopped) {
                c->glink_stopped = 1;
                qcom_glink_stop(&c->glink);
            }
            pagani_adsp_core_trace(c, "DSP fatal error while running; channels closed (no restart)");
        }
    }
    if ((value & PaganiAdspBitHandover) && !c->handover) {
        c->handover = 1;
        if (c->votes_on) {
            const int rc = c->ops->rails(CTX, 0);
            c->votes_on = 0;
            line_begin(c, &t, "handover: proxy votes dropped");
            pagani_text_kd(&t, "rc", rc);
            line_end(c);
        }
    }
}

static void glue_state(void *context, QcomGlinkChannel *glink, int up)
{
    (void)glink;
    PaganiAdspChannel *ch = (PaganiAdspChannel *)context;
    PaganiAdspCore *c = ch->core;
    PaganiText t;
    __atomic_store_n(&ch->up, up ? 1u : 0u, __ATOMIC_RELEASE);
    line_begin(c, &t, "channel ");
    pagani_text_str(&t, ch->name);
    pagani_text_str(&t, up ? " up" : " down");
    line_end(c);
    if (ch->client.state) ch->client.state(ch->context, up);
}
static void glue_receive(void *context, QcomGlinkChannel *glink, const uint8_t *data, uint32_t length)
{
    (void)glink;
    PaganiAdspChannel *ch = (PaganiAdspChannel *)context;
    if (ch->client.receive) ch->client.receive(ch->context, data, length);
}
static void apply_channels(PaganiAdspCore *c)
{
    const QcomGlinkClient glue = {glue_state, glue_receive};
    uint32_t failed = 0;
    c->ops->lock(CTX);
    const uint32_t count = __atomic_load_n(&c->channel_count, __ATOMIC_ACQUIRE);
    for (uint32_t i = 0; i < count; ++i) {
        PaganiAdspChannel *ch = &c->channel[i];
        if (ch->glink) continue;
        ch->glink = qcom_glink_channel(&c->glink, ch->name, ch->intent_size, ch->intent_count, ch->storage, &glue, ch, ch->initiator);
        if (!ch->glink) failed++;
    }
    c->ops->unlock(CTX);
    if (failed) pagani_adsp_core_trace(c, "glink: no channel slot left");
}
static void pump(PaganiAdspCore *c)
{
    const uint32_t count = __atomic_load_n(&c->channel_count, __ATOMIC_ACQUIRE);
    for (uint32_t i = 0; i < count; ++i) {
        PaganiAdspChannel *ch = &c->channel[i];
        if (!ch->glink || !ch->glink->up) continue;
        for (;;) {
            uint32_t length = 0;
            c->ops->lock(CTX);
            if (!ch->count) {c->ops->unlock(CTX); break;}
            uint8_t *slot = ch->queue + (size_t)ch->head * (4u + ch->max_message);
            c->ops->unlock(CTX);
            /* Only this thread takes messages off, so the slot stays valid with the lock dropped. */
            pagani_copy(&length, slot, 4);
            const int rc = qcom_glink_send(&c->glink, ch->glink, slot + 4, length);
            if (rc == QcomGlinkAgain) {ch->deferred++; break;}
            c->ops->lock(CTX);
            ch->head = (ch->head + 1) % ch->depth;
            ch->count--;
            c->ops->unlock(CTX);
            if (rc == QcomGlinkOk) ch->sent++;
            else ch->dropped++;
        }
    }
}
static void service_glink(PaganiAdspCore *c)
{
    PaganiText t;
    if (c->glink_stopped) return;
    if (!c->glink.rx.data) {
        uint32_t at = 0, size = 0;
        if (!item(c, &c->part, PaganiAdspItemGlinkRx, &at, &size) && size >= 64) {
            qcom_glink_set_rx(&c->glink, c->smem.base + at, size);
            line_begin(c, &t, "glink rx fifo");
            pagani_text_kx(&t, "item", at);
            pagani_text_char(&t, '+');
            pagani_text_hex(&t, size);
            line_end(c);
        }
    }
    apply_channels(c);
    qcom_glink_poll(&c->glink);
    pump(c);
    c->status.glink_in = c->glink.rx_messages;
    c->status.glink_out = c->glink.tx_messages;
    if (c->glink.version_agreed && !c->version_traced) {
        c->version_traced = 1;
        line_begin(c, &t, "glink version agreed");
        pagani_text_kx(&t, "features", c->glink.features);
        line_end(c);
        if (c->status.stage > 0 && c->status.stage < PaganiAdspStGlink) stage(c, PaganiAdspStGlink);
    }
    uint32_t up = 0;
    const uint32_t count = __atomic_load_n(&c->channel_count, __ATOMIC_ACQUIRE);
    for (uint32_t i = 0; i < count; ++i) if (__atomic_load_n(&c->channel[i].up, __ATOMIC_ACQUIRE)) up |= 1u << i;
    c->status.channels_up = up;
    if (up && c->status.stage == PaganiAdspStGlink) stage(c, PaganiAdspStChannel);
    if (c->glink.unknown_opens != c->unknown_traced) {
        c->unknown_traced = c->glink.unknown_opens;
        line_begin(c, &t, "glink opens nobody serves:");
        pagani_text_char(&t, ' ');
        pagani_text_str(&t, c->glink.unknown_names);
        line_end(c);
    }
    if (c->glink.broken && !c->glink_stopped) {
        c->glink_stopped = 1;
        qcom_glink_stop(&c->glink);
        note(c, "glink stream broken");
        line_begin(c, &t, "glink stream broken; link stopped");
        pagani_text_kd(&t, "in", c->glink.rx_messages);
        line_end(c);
    }
}

uint32_t pagani_adsp_core_idle_ms(PaganiAdspCore *c)
{
    uint32_t idle = 60000;
    const uint32_t count = __atomic_load_n(&c->channel_count, __ATOMIC_ACQUIRE);
    for (uint32_t i = 0; i < count; ++i)
        if (c->channel[i].count && c->channel[i].glink && c->channel[i].glink->up) idle = 1000;   /* waiting for an intent or room */
    if (c->glink_check_ms) {
        const uint64_t now = c->ops->now_ms(CTX);
        const uint32_t left = c->glink_check_ms > now ? (uint32_t)(c->glink_check_ms - now) : 0;
        if (left < idle) idle = left;
    }
    if (!c->irq_ok && idle > 20) idle = 20;
    return idle;
}

/* ---- clients ---- */
PaganiAdspChannel *pagani_adsp_core_channel(PaganiAdspCore *c, const PaganiAdspChannelConfig *config, const PaganiAdspClient *client,
                                            void *context)
{
    if (!c || !config || !client || !config->name) return 0;
    const size_t name = pagani_strlen(config->name, PAGANI_ADSP_NAME);
    const uint32_t max = config->max_message ? config->max_message : PAGANI_ADSP_MAX_MESSAGE;
    const uint32_t depth = config->queue_depth ? config->queue_depth : PAGANI_ADSP_DEFAULT_QUEUE;
    if (!name || name >= PAGANI_ADSP_NAME || !config->intent_count || config->intent_count > PAGANI_ADSP_MAX_INTENTS ||
        !config->intent_size || config->intent_size > 0x10000u || max > PAGANI_ADSP_MAX_MESSAGE_LIMIT || depth > PAGANI_ADSP_MAX_QUEUE)
        return 0;
    const size_t storage_bytes = (size_t)config->intent_size * config->intent_count, queue_bytes = (size_t)depth * (4u + max);
    uint8_t *storage = (uint8_t *)c->ops->alloc(CTX, storage_bytes), *queue = (uint8_t *)c->ops->alloc(CTX, queue_bytes);
    PaganiAdspChannel *ch = 0;
    c->ops->lock(CTX);
    const uint32_t count = c->channel_count;
    int taken = 0;
    for (uint32_t i = 0; i < count; ++i) taken |= pagani_streq(c->channel[i].name, config->name);
    if (!taken && storage && queue && count < PaganiAdspMaxChannels) {
        ch = &c->channel[count];
        pagani_zero(ch, sizeof *ch);
        pagani_strcpy(ch->name, sizeof ch->name, config->name);
        ch->intent_size = config->intent_size;
        ch->intent_count = config->intent_count;
        ch->max_message = max;
        ch->depth = depth;
        ch->initiator = config->initiator != 0;
        ch->client = *client;
        ch->context = context;
        ch->core = c;
        ch->storage = storage;
        ch->queue = queue;
        __atomic_store_n(&c->channel_count, count + 1, __ATOMIC_RELEASE);
    }
    c->ops->unlock(CTX);
    if (!ch) {
        if (storage) c->ops->free(CTX, storage, storage_bytes);
        if (queue) c->ops->free(CTX, queue, queue_bytes);
        return 0;
    }
    if (c->ops->wake) c->ops->wake(CTX);
    return ch;
}

int pagani_adsp_core_send(PaganiAdspCore *c, PaganiAdspChannel *ch, const void *data, uint32_t length)
{
    if (!c || !ch || !data || !length) return PaganiAdspArgument;
    if (length > ch->max_message) return PaganiAdspTooBig;
    if (c->dead || c->glink_stopped || c->status.crashed) return PaganiAdspDown;
    c->ops->lock(CTX);
    if (ch->count == ch->depth) {
        c->ops->unlock(CTX);
        return PaganiAdspFull;
    }
    uint8_t *slot = ch->queue + (size_t)((ch->head + ch->count) % ch->depth) * (4u + ch->max_message);
    pagani_copy(slot, &length, 4);
    pagani_copy(slot + 4, data, length);
    ch->count++;
    c->ops->unlock(CTX);
    if (c->ops->wake) c->ops->wake(CTX);
    return PaganiAdspOk;
}

void pagani_adsp_core_status(PaganiAdspCore *c, PaganiAdspStatus *out)
{
    if (!c || !out) return;
    *out = c->status;
    uint32_t up = 0;
    const uint32_t count = __atomic_load_n(&c->channel_count, __ATOMIC_ACQUIRE);
    for (uint32_t i = 0; i < count; ++i) if (__atomic_load_n(&c->channel[i].up, __ATOMIC_ACQUIRE)) up |= 1u << i;
    out->channels_up = up;
}

void pagani_adsp_core_init(PaganiAdspCore *c, const PaganiAdspOps *ops, const PaganiAdspConfig *config, volatile uint8_t *smem,
                           uint32_t smem_size)
{
    pagani_zero(c, sizeof *c);
    c->ops = ops;
    c->config = *config;
    if (!c->config.directory) c->config.directory = "image";
    c->smem.base = smem;
    c->smem.size = smem_size;
    c->smem.refused = &c->refused;
    c->status.slave = -1;
    c->stop_entry = c->sleep_entry = -1;
    pagani_adsp_qrtr_init(c);
}
