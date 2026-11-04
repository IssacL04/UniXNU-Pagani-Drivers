/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* The haptics module's SPMI access through the PMIC arbiter v7 (HapticsRegs.h [A]): reads through this EE's observer channels, writes
 * through the RW channel of an APID this EE owns, and nothing else.
 *
 * What may be written is fixed here, not by the caller: only the HAPTICS_CFG and HAPTICS_PATTERN peripherals of the board's slave
 * (the HBOOST and the SDAM are read only), only the registers of PaganiHaptics' sequences, with their lengths, and for some of them
 * only certain values (pgh_spmi_allowed). Before a peripheral may be written its channel must map the expected PPID and be owned by
 * this EE, and its identity must have been read (pgh_spmi_identify); before every write the channel table entry and the owner are
 * read again. A failed or refused write closes the bus: every later write is refused, except one emergency stop (SPMI_PLAY = 0).
 * All waits are bounded (1000 x 1 us per transaction, as the Linux driver).
 *
 * The arbiter also reports a transaction it dropped (status DONE | DROPPED, the 13T: one boot of four, the VMAX write of the init: "FAULT
 * write rc=-5 periph=0 reg=0x48 status=0x9"): nothing was carried out, the module is as it was. With retry_dropped (set after discovery,
 * default off) such a transaction goes again, at most PGH_ARB_DROP_RETRIES times, 100 us apart, the channel checked before each; any
 * other failure closes the bus at once, and so does a drop that does not go away. */
#ifndef PAGANI_HAPTICS_SPMI_H
#define PAGANI_HAPTICS_SPMI_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

enum { PGH_CFG = 0, PGH_PTN = 1, PGH_HBST = 2, PGH_SDAM = 3, PGH_PERIPHS = 4 };

typedef struct {
    void *ctx;
    uint32_t (*core)(void *ctx, uint32_t offset);             /* arbiter core window (version, features, channel table) */
    uint32_t (*cnfg)(void *ctx, uint32_t offset);             /* ownership table window */
    uint32_t (*obs_read)(void *ctx, uint32_t offset);         /* this EE's 0x8000 observer window */
    void (*obs_write)(void *ctx, uint32_t offset, uint32_t value);
    /* RW channel page of one APID (chnls + 0x1000 x apid) for PGH_CFG or PGH_PTN: 0 when mapped. The glue maps nothing else. */
    int (*map_channel)(void *ctx, unsigned periph, unsigned apid);
    uint32_t (*chan_read)(void *ctx, unsigned periph, uint32_t offset);
    void (*chan_write)(void *ctx, unsigned periph, uint32_t offset, uint32_t value);
    void (*delay_us)(void *ctx, unsigned us);
} PghSpmiIo;

typedef struct {
    uint16_t ppid;                  /* SID << 8 | PID expected */
    int16_t apid;                   /* the mapping this EE owns, else the first one; -1 none */
    uint8_t owner, irq, mappings;   /* write EE of that APID, its IRQ owner bit, APIDs that map the PPID */
    uint8_t identified;             /* REVISION1..4, TYPE, SUBTYPE read */
    uint8_t writable;               /* RW channel mapped after the checks (CFG, PTN only) */
    uint8_t rev[4], type, subtype;
} PghPeriph;

typedef struct {
    const PghSpmiIo *io;
    unsigned ee;
    uint32_t version, channels;
    PghPeriph p[PGH_PERIPHS];
    uint8_t vmax_code_max;          /* VMAX register values above this are refused (the hard cap / 50 mV) */
    uint8_t closed;                 /* a write failed or was refused: writes stop */
    uint8_t emergency_used;
    uint8_t retry_dropped;          /* a transaction the arbiter dropped goes again (see above) */
    uint32_t reads, writes, refused, failures, timeouts, dropped;
    uint32_t last_status;           /* the status word of the last failed transaction */
    int last_rc;
    uint8_t last_periph, last_reg;  /* where it failed */
} PghSpmi;

/* Reads the arbiter version, channel count, the channel table and owners for the four PPIDs (no SPMI transaction). 0; -19 not the v7
 * arbiter (nothing else read); -22 bad arguments. A PPID without a channel leaves apid -1. */
int pgh_spmi_discover(PghSpmi *s, const PghSpmiIo *io, unsigned ee, const uint16_t ppid[PGH_PERIPHS], unsigned vmax_code_max);
/* REVISION1..4, TYPE and SUBTYPE through the observer channel: 0, -19 no channel, or the transaction's error. */
int pgh_spmi_identify(PghSpmi *s, unsigned periph);
/* Opens writing: CFG or PTN only, identified, its APID owned by this EE (cnfg bits 2:0) and still mapping the PPID; maps the channel.
 * 0, -1 (EPERM) refused, or the glue's mapping error. */
int pgh_spmi_open(PghSpmi *s, unsigned periph);
/* 1..4 bytes from reg up (one observer transaction). 0, -19, -22, -5 (EIO: failure/denied/dropped), -110 (timeout). */
int pgh_spmi_read(PghSpmi *s, unsigned periph, uint8_t reg, uint8_t *data, unsigned len);
/* One write transaction of 1..8 bytes, if pgh_spmi_allowed: 0, -1 refused (the bus closes), -5/-110 (the bus closes). */
int pgh_spmi_write(PghSpmi *s, unsigned periph, uint8_t reg, const uint8_t *data, unsigned len);
static inline int pgh_spmi_write1(PghSpmi *s, unsigned periph, uint8_t reg, uint8_t value)
{
    return pgh_spmi_write(s, periph, reg, &value, 1);
}
/* SPMI_PLAY = 0 even after the bus closed (once), the channel checks still apply. */
int pgh_spmi_emergency_stop(PghSpmi *s);
/* The write whitelist: 1 when (periph, reg, len, data) may be written. */
int pgh_spmi_allowed(const PghSpmi *s, unsigned periph, uint8_t reg, const uint8_t *data, unsigned len);

#ifdef __cplusplus
}
#endif
#endif
