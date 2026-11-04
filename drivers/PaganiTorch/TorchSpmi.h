/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* The torch module's SPMI access through the PMIC arbiter v7 (TorchRegs.h [A]): reads through this EE's observer channels, writes
 * through the RW channel of the one APID this EE owns, and nothing else. The SPMI write gate of PaganiHaptics (HapticsSpmi.c, copied
 * at its 2026-10-31 07:13 state, sha256 45e83a18...) with this module's prefix, one peripheral and the flash module's whitelist.
 *
 * What may be written is fixed here, not by the caller: only the flash LED module of the board's slave, only the registers of the
 * torch sequences of the channels the board uses (PgtSpmi.chan_mask), single bytes, and for each register only the values of those
 * sequences (pgt_spmi_allowed): target current codes at most the hard cap's code, 5 mA current steps on every board channel, the
 * software strobe, the safety timer off, no other channel ever enabled. An interlock: a channel may be switched on only after this
 * gate itself wrote its current resolution, its target current, its strobe and its timer since the channel was opened (so a current
 * left by another system can never be switched on). Before the peripheral may be written its channel must map the expected PPID and
 * be owned by this EE, and its identity must have been read (pgt_spmi_identify); before every write the channel table entry and the
 * owner are read again. A failed or refused write closes the bus: every later write is refused, except one emergency stop (all
 * channels off, then the module off). All waits are bounded (1000 x 1 us per transaction, as the Linux driver). */
#ifndef PAGANI_TORCH_SPMI_H
#define PAGANI_TORCH_SPMI_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

enum { PGT_FLASH = 0, PGT_PERIPHS = 1 };

typedef struct {
    void *ctx;
    uint32_t (*core)(void *ctx, uint32_t offset);             /* arbiter core window (version, features, channel table) */
    uint32_t (*cnfg)(void *ctx, uint32_t offset);             /* ownership table window */
    uint32_t (*obs_read)(void *ctx, uint32_t offset);         /* this EE's 0x8000 observer window */
    void (*obs_write)(void *ctx, uint32_t offset, uint32_t value);
    /* RW channel page of one APID (chnls + 0x1000 x apid) for PGT_FLASH: 0 when mapped. The glue maps nothing else. */
    int (*map_channel)(void *ctx, unsigned periph, unsigned apid);
    uint32_t (*chan_read)(void *ctx, unsigned periph, uint32_t offset);
    void (*chan_write)(void *ctx, unsigned periph, uint32_t offset, uint32_t value);
    void (*delay_us)(void *ctx, unsigned us);
} PgtSpmiIo;

typedef struct {
    uint16_t ppid;                  /* SID << 8 | PID expected */
    int16_t apid;                   /* the mapping this EE owns, else the first one; -1 none */
    uint8_t owner, irq, mappings;   /* write EE of that APID, its IRQ owner bit, APIDs that map the PPID */
    uint8_t identified;             /* REVISION1..4, TYPE, SUBTYPE read */
    uint8_t writable;               /* RW channel mapped after the checks */
    uint8_t rev[4], type, subtype;
} PgtPeriph;

typedef struct {
    const PgtSpmiIo *io;
    unsigned ee;
    uint32_t version, channels;
    PgtPeriph p[PGT_PERIPHS];
    uint8_t chan_mask;              /* the board's channels (the only ones that may ever be on) */
    uint8_t itarget_max;            /* target current codes above this are refused (the hard cap's code) */
    uint8_t closed;                 /* a write failed or was refused: writes stop */
    uint8_t emergency_used;
    /* the interlock: what this gate itself wrote since the channel was opened */
    uint8_t ires_set;               /* IRES written with 5 mA steps on every board channel */
    uint8_t itarget_set, strobe_set, timer_set;   /* bit ch: ITARGET / CHAN_STROBE / CHAN_TIMER of channel ch written */
    uint32_t reads, writes, refused, failures, timeouts;
    uint32_t last_status;           /* the status word of the last failed transaction */
    int last_rc;
    uint8_t last_periph, last_reg;  /* where it failed */
} PgtSpmi;

/* Reads the arbiter version, channel count, the channel table and owners for the PPID (no SPMI transaction). 0; -19 not the v7 arbiter
 * (nothing else read); -22 bad arguments (an EE above 7, no channel or a channel above 3 in the mask, a zero or 7-bit-overflowing code
 * limit). A PPID without a channel leaves apid -1. */
int pgt_spmi_discover(PgtSpmi *s, const PgtSpmiIo *io, unsigned ee, const uint16_t ppid[PGT_PERIPHS], unsigned chan_mask,
                      unsigned itarget_max);
/* REVISION1..4, TYPE and SUBTYPE through the observer channel: 0, -19 no channel, or the transaction's error. */
int pgt_spmi_identify(PgtSpmi *s, unsigned periph);
/* Opens writing: PGT_FLASH, identified, its APID owned by this EE (cnfg bits 2:0) and still mapping the PPID; maps the channel and
 * clears the interlock. 0, -1 (EPERM) refused, or the glue's mapping error. */
int pgt_spmi_open(PgtSpmi *s, unsigned periph);
/* 1..4 bytes from reg up (one observer transaction). 0, -19, -22, -5 (EIO: failure/denied/dropped), -110 (timeout). */
int pgt_spmi_read(PgtSpmi *s, unsigned periph, uint8_t reg, uint8_t *data, unsigned len);
/* One write transaction, if pgt_spmi_allowed: 0, -1 refused (the bus closes), -5/-110 (the bus closes). */
int pgt_spmi_write(PgtSpmi *s, unsigned periph, uint8_t reg, const uint8_t *data, unsigned len);
static inline int pgt_spmi_write1(PgtSpmi *s, unsigned periph, uint8_t reg, uint8_t value)
{
    return pgt_spmi_write(s, periph, reg, &value, 1);
}
/* CHAN_EN = 0, then EN_CTL = 0, even after the bus closed (once); the channel checks still apply, and the bus stays closed after it.
 * 0, or the first error (both writes are tried). */
int pgt_spmi_emergency_stop(PgtSpmi *s);
/* The write whitelist and the interlock: 1 when (periph, reg, len, data) may be written now. */
int pgt_spmi_allowed(const PgtSpmi *s, unsigned periph, uint8_t reg, const uint8_t *data, unsigned len);

#ifdef __cplusplus
}
#endif
#endif
