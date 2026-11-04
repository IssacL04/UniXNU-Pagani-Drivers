/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved.
 * The bus bring-up follows UniXNU-sheng package/AudioProbe.cpp (commit 08cde20e: setupPins, setupClocks, setupBus, transfer,
 * runProgram), moved into portable C over register callbacks so the host tests run it against models of the TLMM, the GCC and the
 * serial engine; the 13T pins, clocks and engine are those of SpeakerBoard.h. */
/* The register side of the 13T speaker path: TLMM pin states (the I2C hub pins, the secondary MI2S pins, the amplifier resets), the
 * I2C hub clocks (GCC votes, branch halts, the serial engine's 19.2 MHz RCG), the GENI serial engine at 400 kHz, and register transfers
 * with the TFA9873 programs on it. Every wait is bounded and polled through the delay callback; nothing here allocates or logs. */
#ifndef PAGANI_SPEAKER_HW_H
#define PAGANI_SPEAKER_HW_H
#include <stdint.h>
#include "GeniI2c.h"
#include "Tfa9873.h"
#ifdef __cplusplus
extern "C" {
#endif

/* Register windows: the GCC (SPEAKER_GCC_BASE), the serial engine (SPEAKER_SE_BASE) and one 4 KiB TLMM page per pin. */
enum {SpeakerWinGcc = 1, SpeakerWinSe = 2};
#define SPEAKER_WIN_PIN(gpio) (0x100u + (unsigned)(gpio))
typedef struct SpeakerIo {
    void *context;
    uint32_t (*read)(void *context, unsigned window, uint32_t offset);
    /* Returns once the write reached the device (a barrier). */
    void (*write)(void *context, unsigned window, uint32_t offset, uint32_t value);
    /* Waits at least us microseconds (the kernel sleeps for whole milliseconds). */
    void (*delay_us)(void *context, uint32_t us);
} SpeakerIo;

/* ---- pins ---- */
typedef struct {
    uint16_t gpio;
    uint8_t function, drive_ma, pull;           /* drive 2..16 mA; pull SpeakerCtlPull* */
    uint8_t output, value, i2c_pull;            /* output: OE with the level written first (no glitch); i2c_pull: CTL bit 13 */
} SpeakerPin;
/* Applies one pin state, keeping the bits it does not own; claims an eGPIO-capable pin for the apps TLMM (bit 12 when bit 11 reads
 * set, as Linux msm_pinmux_set_mux does). Returns the CTL read back; *ok is 1 when the owned bits read back as written. */
uint32_t speaker_pin_apply(const SpeakerIo *io, const SpeakerPin *pin, int *ok);
/* gpio72/73: function 1 (i2chub0_se4), 2 mA, pull-up, and the strong I2C pull-up when strong_pull (qcom,i2c_pull). 0, or -5 when a
 * readback differs (ctl[0..1] hold the readbacks). */
int speaker_pins_i2c(const SpeakerIo *io, int strong_pull, uint32_t ctl[2]);
/* gpio121..124 in the devicetree's active state (function 1, 4 mA, no pull; data1 pulled down) or sleep state (gpio, 2 mA, pull-down,
 * input). ctl[0..3] hold the readbacks; 0 or -5. */
int speaker_pins_i2s(const SpeakerIo *io, int active, uint32_t ctl[4]);
/* The amplifiers in mask (bit 0: gpio2 for 0x34, bit 1: gpio164 for 0x35) through their active-high reset: the pin an output at low,
 * high for 1 ms, low again and 2 ms for the device to come up (the vendor's tfa98xx_ext_reset holds 1 ms each). ctl[0..1] hold the
 * readbacks; 0 or -5. */
int speaker_reset_amps(const SpeakerIo *io, unsigned mask, uint32_t ctl[2]);

/* ---- clocks ---- */
typedef struct {
    uint32_t vote;                              /* the vote register after the update */
    uint32_t sahb, core, s4;                    /* the three branch registers at the end */
    uint32_t rcg_cmd, rcg_cfg;                  /* the serial engine RCG as found */
    uint32_t polls;                             /* 10 us polls until the branches ran */
    uint32_t vote_waits;                        /* 10 us steps spent waiting for the shared lock word */
    uint8_t rcg_fixed;                          /* 1: the RCG was moved to the XO */
    uint8_t vote_wrote, vote_locked;            /* the vote register was written; under the lock (0 with wrote: without, counted) */
} SpeakerClocks;
/* The I2C hub's AHB, core and SE4 branches voted on, the SE4 RCG on the 19.2 MHz XO (moved there when it is not), then every branch
 * running (CLK_OFF clear; an AHB branch under hardware gating may idle). The vote register 0x52008 is shared with the touch, the fuel
 * gauge and PCIe: its update is Sm8750Platform's sm8750_gcc_vote_set (PeripheralIO.h) under the lock word sm8750_gcc_vote_owner, tag
 * SPEAKER_GCC_OWNER, never a bare read-modify-write. 0; -110 when a branch stays off for 20 ms (nothing behind it is touched then);
 * -5 when the vote does not read back or the RCG update does not complete. */
int speaker_clocks_on(const SpeakerIo *io, SpeakerClocks *out);

/* ---- the serial engine ---- */
typedef struct {
    uint32_t fw, if_disable, clk, scl, status;  /* as found */
    uint8_t configured;                         /* 1: set to 400 kHz here; 0: the boot loader's setting kept */
    uint8_t aborted;                            /* 1: a command was still active and was aborted */
} SpeakerSe;
/* Checks the engine runs the I2C firmware with the FIFO interface on (the I2C hub has no DMA); an engine without clock or timing gets
 * the 400 kHz setting (19.2 MHz / 2, SCL counters 5/11/22, as sheng's setupBus and i2c-qcom-geni's table); a command still running is
 * aborted. 0, or -19 when the engine cannot be used. */
int speaker_se_setup(const SpeakerIo *io, SpeakerSe *out);

/* ---- transfers ---- */
typedef struct {
    GeniI2c geni;
    const SpeakerIo *io;
    uint32_t transfers, failures, timeouts;
    uint32_t last_error, last_status;           /* GeniErr* and the M_IRQ status of the last failure */
    uint8_t last_address;
} SpeakerBus;
/* 0, or -19 when the engine is not usable; *config: protocol << 24 | clock configuration. */
int speaker_bus_init(SpeakerBus *b, const SpeakerIo *io, uint32_t *config);
/* One transfer to a 7-bit address: write length bytes (1..8), then read read_length bytes (0..4, first byte in bits 7:0 of *rx).
 * Polls every 10 us; a stage that does not end within 10 ms is cancelled, then aborted. 0, -5 (bus error or NACK) or -110. */
int speaker_bus_transfer(SpeakerBus *b, uint8_t address, const uint8_t *tx, uint32_t length, uint32_t read_length, uint32_t *rx);
/* Runs one TFA9873 program on the amplifier at address; 0, or the failing transfer's code (the run shows how far it got). */
int speaker_amp_run(SpeakerBus *b, uint8_t address, const Tfa9873Step *steps, unsigned count, Tfa9873Run *run);

#ifdef __cplusplus
}
#endif
#endif
