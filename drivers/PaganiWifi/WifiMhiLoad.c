/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved.
 * Ported from UniXNU-sheng package/WifiProbe.cpp mhiBring, programContexts and eventAck (commit 08cde20e); see WifiMhiLoad.h. */
#include "WifiMhiLoad.h"
#include "WifiUtil.h"

enum {kCtx = 44, kChannels = 128, kEvents = 2, kCmdElements = 8, kElement = 16};
/* MHI registers (mhi_init_mmio): context array pointers, the address ranges the device may reach, the event ring doorbells. */
enum {kMhiCfg = 0x10, kErdbOff = 0x20, kCcabap = 0x58, kEcabap = 0x60, kCrcbap = 0x68, kCtrlBase = 0x80, kCtrlLimit = 0x88,
      kDataBase = 0x98, kDataLimit = 0xa0};

static void put32(uint8_t *at, uint32_t v) {for (unsigned i = 0; i < 4; ++i) at[i] = (uint8_t)(v >> (8 * i));}
static void put64(uint8_t *at, uint64_t v) {put32(at, (uint32_t)v); put32(at + 4, (uint32_t)(v >> 32));}
static uint64_t get64(const uint8_t *at)
{
    uint64_t v = 0;
    for (unsigned i = 0; i < 8; ++i) v |= (uint64_t)at[i] << (8 * i);
    return v;
}
static void reg64(const PwMhiIo *io, uint32_t low, uint64_t v)
{
    io->write(io->ctx, low, (uint32_t)v);
    io->write(io->ctx, low + 4, (uint32_t)(v >> 32));
}
static void say(const PwMhiIo *io, const char *what, int64_t a, int64_t b)
{
    if (!io->log) return;
    char text[120];
    PwText t;
    pw_text_start(&t, text, sizeof text);
    pw_text_str(&t, "[wifi] mhi ");
    pw_text_str(&t, what);
    pw_text_kx(&t, "a", (uint64_t)a);
    pw_text_kx(&t, "b", (uint64_t)b);
    io->log(io->ctx, text);
}

void pw_mhi_contexts(PwMhiLoad *l, const PwMhiIo *io)
{
    uint8_t *va = l->ctx_va;
    const uint64_t pa = l->ctx_bus;
    const uint32_t evOff = kChannels * kCtx, cmdOff = evOff + kEvents * kCtx;
    uint32_t cur = (cmdOff + kCtx + 4095u) & ~4095u;
    l->ev_len[0] = 32 * kElement;
    l->ev_len[1] = 256 * kElement;
    for (uint32_t i = 0; i < kEvents; ++i) {
        l->ev_bus[i] = pa + cur;
        l->ev_va[i] = va + cur;
        uint8_t *c = va + evOff + i * kCtx;
        put32(c, i == 0 ? 0 : 1u << 16);                    /* interrupt moderation 0 / 1 ms */
        put32(c + 4, 1);                                    /* MHI_ER_TYPE_VALID */
        put32(c + 8, i + 1);                                /* MSI vector */
        put64(c + 12, l->ev_bus[i]);
        put64(c + 20, l->ev_len[i]);
        put64(c + 28, l->ev_bus[i]);                        /* read pointer */
        put64(c + 36, l->ev_bus[i] + l->ev_len[i] - kElement);   /* write pointer: the whole ring is the device's */
        l->acked[i] = l->ev_bus[i];
        cur += l->ev_len[i];
    }
    const uint64_t cmd = pa + cur;
    put64(va + cmdOff + 12, cmd);
    put64(va + cmdOff + 20, kCmdElements * kElement);
    put64(va + cmdOff + 28, cmd);
    put64(va + cmdOff + 36, cmd);
    if (io->clean) io->clean(io->ctx, va, PW_MHI_CTX_BYTES);
    reg64(io, kCcabap, pa);
    reg64(io, kEcabap, pa + evOff);
    reg64(io, kCrcbap, pa + cmdOff);
    /* The host ranges the device may reach: 0 .. 2^36 - 1, what the stack programs too (ath12k's 36-bit streaming mask; cnss2 uses 36
     * bits for peach). The window itself is low memory by default, and above 4 GiB with pagani-wifi-dmapa=0. */
    reg64(io, kCtrlBase, 0);
    reg64(io, kCtrlLimit, PW_MHI_DMA_LIMIT);
    reg64(io, kDataBase, 0);
    reg64(io, kDataLimit, PW_MHI_DMA_LIMIT);
    io->write(io->ctx, kMhiCfg, (io->read(io->ctx, kMhiCfg) & ~0xffff0000u) | (uint32_t)kEvents << 16);   /* NER 2, NHWER 0 */
    const uint32_t erdb = io->read(io->ctx, kErdbOff);
    if (erdb >= 0x100 && erdb + 8 * kEvents <= l->boot.reglen && !(erdb & 3))
        for (uint32_t i = 0; i < kEvents; ++i) reg64(io, erdb + 8 * i, l->ev_bus[i] + l->ev_len[i] - kElement);
    l->context_writes++;
}

unsigned pw_mhi_event_ack(PwMhiLoad *l, const PwMhiIo *io)
{
    unsigned moved = 0;
    const uint32_t erdb = io->read(io->ctx, kErdbOff);
    if (!l->ctx_va || erdb < 0x100 || erdb + 8 * kEvents > l->boot.reglen || (erdb & 3)) return 0;
    for (unsigned r = 0; r < kEvents; ++r) {
        uint8_t *c = l->ctx_va + kChannels * kCtx + r * kCtx;
        const uint64_t base = get64(c + 12), len = get64(c + 20), rp = get64(c + 28);
        if (rp == l->acked[r] || rp < base || rp >= base + len) continue;
        l->acked[r] = rp;
        const uint64_t wp = rp == base ? base + len - kElement : rp - kElement;
        put64(c + 36, wp);
        if (io->clean) io->clean(io->ctx, c + 36, 8);
        reg64(io, erdb + 8 * r, wp);
        ++moved;
    }
    l->event_acks += moved;
    return moved;
}

int pw_mhi_load(PwMhiLoad *l, PwDma *dma, const PwMhiIo *io, const PwMhiConfig *config)
{
    if (!l || !dma || !io || !io->read || !io->write || !io->msleep || !io->now_ms || !config) return PwMhiNoImage;
    pw_zero(l, sizeof *l);
    if (!config->image || !config->image_bytes) return l->result = PwMhiNoImage;
    if (mhi_fw_layout(config->image, config->image_bytes, config->image_bytes, PW_MHI_SBL_BYTES, &l->layout))
        return l->result = PwMhiNoImage;
    say(io, "image", l->layout.split ? (int64_t)l->layout.image_offset : 0, l->layout.image_size);
    l->segments = (l->layout.image_size + PW_MHI_SEG_BYTES - 1) / PW_MHI_SEG_BYTES;
    if (l->segments > PW_MHI_MAX_SEGMENTS) return l->result = PwMhiTooLarge;

    /* The buffers: the boot loader, each segment (512 KiB, aligned to it), the vector table, the contexts. */
    uint8_t *sbl = (uint8_t *)pw_dma_alloc(dma, PW_MHI_SBL_BYTES, &l->sbl_bus);
    if (!sbl) return l->result = PwMhiNoDma;
    uint64_t addresses[PW_MHI_MAX_SEGMENTS];
    uint8_t *seg[PW_MHI_MAX_SEGMENTS];
    for (uint32_t i = 0; i < l->segments; ++i) {
        seg[i] = (uint8_t *)pw_dma_alloc(dma, PW_MHI_SEG_BYTES, &l->seg_bus[i]);
        if (!seg[i]) return l->result = PwMhiNoDma;
        addresses[i] = l->seg_bus[i];
    }
    MhiBhieEntry *table = (MhiBhieEntry *)pw_dma_alloc(dma, 4096, &l->table_bus);
    l->ctx_va = (uint8_t *)pw_dma_alloc(dma, PW_MHI_CTX_BYTES, &l->ctx_bus);
    if (!table || !l->ctx_va) return l->result = PwMhiNoDma;
    pw_copy(sbl, config->image + l->layout.sbl_offset, l->layout.sbl_size);
    for (uint32_t i = 0; i < l->segments; ++i) {
        const uint32_t done = i * PW_MHI_SEG_BYTES, left = l->layout.image_size - done;
        pw_copy(seg[i], config->image + l->layout.image_offset + done, left < PW_MHI_SEG_BYTES ? left : PW_MHI_SEG_BYTES);
    }
    l->table_entries = mhi_bhie_table(table, 4096 / sizeof(MhiBhieEntry), addresses, l->layout.image_size, PW_MHI_SEG_BYTES);
    if (!l->table_entries) return l->result = PwMhiLayout;
    if (io->clean) {
        io->clean(io->ctx, sbl, l->layout.sbl_size);
        for (uint32_t i = 0; i < l->segments; ++i) io->clean(io->ctx, seg[i], PW_MHI_SEG_BYTES);
        io->clean(io->ctx, table, 4096);
    }

    const MhiIo mio = {io->read, io->write, io->ctx, config->bar_bytes};
    if (mhi_boot_open(&l->boot, &mio)) return l->result = PwMhiOpen;
    say(io, "open", l->boot.bhi, l->boot.bhie);
    /* The driver of this chip family gives the device its contexts before the boot loader starts (sheng saw the loader refuse an
     * image otherwise), and again once the loader reset the registers (MhiNeedContexts). */
    pw_mhi_contexts(l, io);
    const uint64_t t0 = io->now_ms(io->ctx);
    uint32_t phase = mhi_boot_begin(&l->boot, config->msi_vector, l->sbl_bus, l->layout.sbl_size, l->table_bus,
                                    l->table_entries * (uint32_t)sizeof(MhiBhieEntry), config->seq);
    const uint32_t step = config->step_ms ? config->step_ms : 5;
    const uint32_t limit = config->phase_ms ? config->phase_ms : 20000;
    uint32_t last = phase;
    uint64_t since = io->now_ms(io->ctx);
    while (phase != MhiDone && phase != MhiFailed) {
        if (phase == MhiNeedContexts) {
            pw_mhi_contexts(l, io);
            phase = mhi_boot_contexts_done(&l->boot);
        } else {
            if (io->link_check && !(l->steps % PW_MHI_LINK_STEPS) && io->read(io->ctx, MHI_STATUS) == 0xffffffffu)
                l->link_restores += io->link_check(io->ctx);
            phase = mhi_boot_step(&l->boot);
            if (phase != MhiNeedContexts && phase != MhiDone && phase != MhiFailed) pw_mhi_event_ack(l, io);
        }
        l->steps++;
        const uint64_t now = io->now_ms(io->ctx);
        if (phase != last) {
            if (last < 10) l->phase_ms[last] = (uint32_t)(now - since);
            say(io, "phase", phase, (int64_t)(now - t0));
            last = phase;
            since = now;
            continue;
        }
        if (now - since > limit) {
            phase = mhi_boot_timeout(&l->boot);
            break;
        }
        io->msleep(io->ctx, step);
    }
    l->elapsed_ms = (uint32_t)(io->now_ms(io->ctx) - t0);
    say(io, phase == MhiDone ? "mission mode" : "stopped", (int64_t)(l->boot.phase | l->boot.error << 8 | l->boot.failed_phase << 16),
        (int64_t)mhi_boot_ee(&l->boot));
    if (phase == MhiDone) return l->result = PwMhiOk;
    return l->result = l->boot.error == MhiErrTimeout ? PwMhiTimeout : PwMhiFailed;
}
