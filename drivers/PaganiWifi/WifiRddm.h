/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved.
 * Ported from UniXNU-sheng package/WifiProbe.cpp rddmScan, printable and looksLikeError (https://github.com/UniXNU/UniXNU-sheng, commit
 * 08cde20e, BSD-2-Clause-Patent): the wait for the upload, the search for the texts a firmware error leaves, the context ranges, the crash
 * record's words and the export's container are sheng's. Here it is portable C over callbacks and the PwText builder, so the host test
 * (Test/rddm.c) drives it against a synthetic BHIe table; every register read is bounds-checked by the owner's callback, and the table
 * and its segments must lie in the DMA window (to_va) or they are not looked at. */
/* What a crashed peach firmware left in the host's memory. The MHI core gives the device a receive vector table at power up (BHIe RX,
 * rddm_size bytes in DMA segments); on a fatal error the device goes to the RDDM execution environment and uploads its RAM dump into
 * those segments. Nothing here needs the device to answer beyond the BHIe status register: the scan reads host memory. */
#ifndef PAGANI_WIFI_RDDM_H
#define PAGANI_WIFI_RDDM_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define PW_RDDM_BHIEOFF 0x2cu                   /* MHI register: where the BHIe block is */
#define PW_RDDM_BHIE_MAX 0x1000u                /* a BHIEOFF past this is not one (all ones when the link is gone) */
#define PW_RDDM_RXVECADDR_LOW 0x60u             /* from BHIEOFF: the table the device writes through */
#define PW_RDDM_RXVECADDR_HIGH 0x64u
#define PW_RDDM_RXVECSIZE 0x68u
#define PW_RDDM_RXVECSTATUS 0x78u               /* bits 31:30: 0 in progress, 2 done, 3 error */
#define PW_RDDM_STATUS_SHIFT 30u
#define PW_RDDM_DONE 2u
#define PW_RDDM_WAIT_STEPS 400u                 /* 10 ms each */
#define PW_RDDM_TABLE_MAX (64u * 16u)           /* entries of a sane table: rddm_size / 512 KiB = 15 */
#define PW_RDDM_MESSAGES 40u
#define PW_RDDM_CONTEXT_LINES 160u
#define PW_RDDM_EXPORT_MAX (64ull << 20)
#define PW_RDDM_PACE_LINES 32u                  /* lines between two pauses, for the boot log's 32 KiB ring */
#define PW_RDDM_PACE_MS 1000u

enum {PwRddmOk = 0, PwRddmNoBhie = -1, PwRddmNotDone = -2, PwRddmNoTable = -3};

typedef struct {
    void *ctx;
    uint32_t (*read)(void *ctx, uint32_t reg);                  /* a BAR0 register; all ones outside the mapped window */
    const uint8_t *(*to_va)(void *ctx, uint64_t bus, uint64_t bytes);   /* the CPU's address of a bus range in the DMA window, or null */
    void (*msleep)(void *ctx, unsigned ms);
    void (*log)(void *ctx, const char *line);
} PwRddmIo;

typedef struct {
    uint32_t status, waited_ms, table_bytes;
    uint64_t table_bus, scanned;
    uint32_t messages, context_lines, record_lines, lines;
    const uint8_t *table;                       /* the table in the window; null when the scan stopped before it */
} PwRddmResult;

/* Waits for the upload (at most 4 s), logs its state, the error texts in the dump, the crash record's texts, the runtime log's and the
 * record's first 0x200 bytes as words. PwRddmOk, or why it stopped (logged). */
int pw_rddm_scan(const PwRddmIo *io, PwRddmResult *r);

/* The whole dump for the UFS log's export area (boot_log_export_put): the table as table.bin and each segment as segNN.bin, in the
 * container read-ufslog.py unpacks ("SHEXR001" + total + "M2VCACH1" file list). The bytes it takes (0: nothing to export, or more than
 * PW_RDDM_EXPORT_MAX), then the fill; pw_rddm_export_fill returns the bytes written, 0 when they differ from the size given. */
uint64_t pw_rddm_export_bytes(const PwRddmIo *io, const PwRddmResult *r);
uint64_t pw_rddm_export_fill(const PwRddmIo *io, const PwRddmResult *r, uint8_t *blob, uint64_t bytes);

#ifdef __cplusplus
}
#endif
#endif
