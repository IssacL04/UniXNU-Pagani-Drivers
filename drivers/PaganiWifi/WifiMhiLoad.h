/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved.
 * Ported from UniXNU-sheng package/WifiProbe.cpp mhiBring, programContexts and eventAck (https://github.com/UniXNU/UniXNU-sheng,
 * commit 08cde20e): the buffers, the context layout and the phase loop are sheng's; the firmware file is cut by mhi_fw_layout (peach:
 * the boot loader, then a second ELF that BHIe carries alone), the segments are cnss2's 512 KiB, and everything runs over callbacks
 * and the PwDma window, so the host model (Test/mhi_load.c) drives it. */
/* Stage 5 of PaganiWifi: the firmware into the endpoint without the Ath12kWifi stack, to see the chip reach mission mode (or where
 * it stops). The DMA window holds the boot loader (one buffer, BHI), the BHIe segments and their vector table, and the MHI context
 * arrays (128 channel contexts left disabled, two event rings: control 32 elements on MSI vector 1, data 256 on vector 2, and the
 * primary command ring), which the device is given before M0 and again after the boot loader resets its registers. Each phase has
 * phase_ms; the device's event rings are given back as it fills them. Nothing is started on the device once it reaches mission
 * mode: the run ends there (QMI and the WLAN start belong to the stack, stage 6). */
#ifndef PAGANI_WIFI_MHI_LOAD_H
#define PAGANI_WIFI_MHI_LOAD_H
#include <stddef.h>
#include <stdint.h>
#include "MhiBoot.h"
#include "WifiDma.h"
#ifdef __cplusplus
extern "C" {
#endif

#define PW_MHI_MAX_SEGMENTS 64u
#define PW_MHI_SBL_BYTES (512u * 1024u)
#define PW_MHI_SEG_BYTES (512u * 1024u)
#define PW_MHI_CTX_BYTES (32u * 1024u)
#define PW_MHI_DMA_LIMIT ((1ull << 36) - 1)
#define PW_MHI_LINK_STEPS 100u

enum {
    PwMhiOk = 0, PwMhiNoImage = -1, PwMhiTooLarge = -2, PwMhiNoDma = -3, PwMhiLayout = -4, PwMhiOpen = -5, PwMhiFailed = -6,
    PwMhiTimeout = -7,
};

typedef struct {
    void *ctx;
    uint32_t (*read)(void *ctx, uint32_t reg);                 /* BAR0 register (MHI space at BAR0 + 0) */
    void (*write)(void *ctx, uint32_t reg, uint32_t value);    /* completes with a barrier */
    void (*msleep)(void *ctx, unsigned ms);
    uint64_t (*now_ms)(void *ctx);
    void (*clean)(void *ctx, const void *va, size_t bytes);    /* the CPU's writes reach memory (dc civac) */
    void (*log)(void *ctx, const char *line);
    /* Called at most every PW_MHI_LINK_STEPS steps while MHI status reads all ones: puts back what a restart of the endpoint's PCIe
     * core cleared (BAR0, the decode bits, MSI); 1 when it did. May be null. */
    unsigned (*link_check)(void *ctx);
} PwMhiIo;

typedef struct {
    const uint8_t *image;
    uint32_t image_bytes;
    uint32_t bar_bytes;          /* the mapped BAR0 bytes the MHI registers may lie in */
    uint32_t msi_vector;         /* BHI interrupt vector (0) */
    uint32_t phase_ms;           /* deadline per phase (cnss: 10000; sheng 20000) */
    uint32_t step_ms;            /* time between steps (5) */
    uint32_t seq;                /* the doorbell sequence seed */
} PwMhiConfig;

typedef struct {
    MhiBoot boot;
    MhiFwLayout layout;
    uint32_t segments, table_entries;
    uint64_t sbl_bus, table_bus, ctx_bus, seg_bus[PW_MHI_MAX_SEGMENTS];
    uint8_t *ctx_va, *ev_va[2];
    uint64_t ev_bus[2], acked[2];
    uint32_t ev_len[2];
    uint32_t context_writes, event_acks, steps, elapsed_ms, phase_ms[10], link_restores;
    int result;
} PwMhiLoad;

/* Allocates the buffers in the window, copies the image, programs the contexts, runs the boot to mission mode or to its failure.
 * PwMhiOk with l->boot.phase == MhiDone, or an error (l->boot says where the device stopped). */
int pw_mhi_load(PwMhiLoad *l, PwDma *dma, const PwMhiIo *io, const PwMhiConfig *config);
/* The context arrays and rings into the window and the registers that point the device at them (exposed for the tests). */
void pw_mhi_contexts(PwMhiLoad *l, const PwMhiIo *io);
/* Gives the device back the event ring entries it posted; returns how many rings moved. */
unsigned pw_mhi_event_ack(PwMhiLoad *l, const PwMhiIo *io);

#ifdef __cplusplus
}
#endif
#endif
