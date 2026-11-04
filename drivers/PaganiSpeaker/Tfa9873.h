/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* The two NXP TFA9873 speaker amplifiers of the OnePlus 13T (I2C hub SE4: 0x34 top / earpiece, 0x35 bottom). The TFA9873 has no
 * on-chip DSP (the vendor driver marks it a "probus" device and its mute is a no-op): it takes I2S/TDM from the SoC's secondary MI2S
 * and plays its slot straight to the speaker, so no amplifier firmware is loaded. Registers are 8-bit addresses holding 16-bit values,
 * big-endian on the wire. A program is a list of steps; the runner turns each step into one bus transfer (a read-modify-write takes
 * two) or a wait and never waits itself: the owner performs the transfer and feeds the result back (the shape of sheng's CS35L43
 * runner, UniXNU-sheng package/ShengCs35l43.h, which bypasses that amplifier's DSP the same way).
 *
 * Register facts, used as facts (the GPL sources are not copied):
 *  - field codes register << 8 | position << 4 | (width - 1): NXP tfa98xx v6.14.2 inc/tfa9873_tfafieldnames.h;
 *  - the start sequence: tfa_init.c tfa9873_specific (keys 0x0F = 0x5A6B, 0xA0 = 0xFB ^ 0x5A, key2 MTPK 0xA1 = 0x5A without
 *    hiding the keys again, the per-revision register table, interrupt enables VDDS|OTDS|OCPR|UVDS, FSSYNCEN = 0), tfa_dsp.c
 *    tfa98xx_init (I2CR), tfaRunStartup (device registers, then the profile's), tfa_dev_set_state INIT_CF (MANSCONF = 1, PWDN = 0,
 *    wait for CLKS) and OPERATING (AMPE = 1 for a probus device), tfa98xx_powerdown (PWDN = 1, then AMPE = 0), tfa_dev_set_tdm_bitwidth
 *    (TDME = 0 around a TDM format change; 16-bit: NBCK 0, slot 15, size 15);
 *  - the board values: the phone's /odm/firmware/tfa98xx.cnt (5699 bytes, OPPO/TFA/9873, sha256 8574cf32...0a4e): device 0 at
 *    0x34 and device 1 at 0x35, 33 bitfield items each (container item numbers in Tfa9873.c), profile "speaker"/"speakerR"
 *    (DCA 1, LNMODE 1, TDMSPKG 6). The container's patch is TFA9873N1B0_dummy.patch (nothing to load). */
#ifndef PAGANI_TFA9873_H
#define PAGANI_TFA9873_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define TFA9873_ADDRESS_TOP 0x34u
#define TFA9873_ADDRESS_BOTTOM 0x35u
#define TFA9873_REG_STATUS 0x10u      /* VDDS DCOCPOK OTDS OCDS UVDS MANALARM CLKS MTPB NOCLK . TDMERR (sticky ones clear on read) */
#define TFA9873_REG_STATUS2 0x11u     /* DCIL DCDCA ... PLLS (6) ... SWS (13) AMPS (14) AREFS (15) */
#define TFA9873_REG_STATUS3 0x13u     /* ... OVDS (8) CLIPS (9) ... MANOPER (15) */
#define TFA9873_REG_MANAGER 0x14u     /* TDMSTAT 2:0, MANSTATE 6:3, AMPSTE 10:7, DCMODE 12:11, WAITSYNC 13 */
#define TFA9873_REG_BATTERY 0x15u     /* BATS 9:0 */
#define TFA9873_REG_TEMPERATURE 0x16u /* TEMPS 8:0 */
#define TFA9873_REG_REV 0x03u
#define TFA9873_REG_IRQ_STATUS 0x40u
#define TFA9873_REG_IRQ_CLEAR 0x44u
#define TFA9873_REG_IRQ_ENABLE 0x48u
#define TFA9873_FAMILY 0x73u          /* REV bits 7:0 */
#define TFA9873_CLKS (1u << 6)        /* REG_STATUS: clocks stable */
#define TFA9873_NOCLK (1u << 8)       /* REG_STATUS: lost clock (sticky); the other flags are logged raw */
#define TFA9873_TDMERR (1u << 10)     /* REG_STATUS: the TDM interface in error (sticky) */
#define TFA9873_PLLS (1u << 6)        /* REG_STATUS2: PLL locked */
#define TFA9873_SWS (1u << 13)        /* REG_STATUS2: the output stage switching */
#define TFA9873_AMPS (1u << 14)       /* REG_STATUS2: the amplifier enabled */
#define TFA9873_MANSTATE(v) (((v) >> 3) & 15u)
#define TFA9873_MANSTATE_OPERATING 9u /* tfa_dev_get_state: 0 power down, 9 operating */

/* Field codes (register << 8 | position << 4 | (width - 1)). */
#define TFA9873_FIELD_REG(f) ((uint8_t)((f) >> 8))
#define TFA9873_FIELD_POS(f) ((unsigned)(((f) >> 4) & 15u))
#define TFA9873_FIELD_WIDTH(f) ((unsigned)((f) & 15u) + 1u)
#define TFA9873_FIELD_MASK(f) ((uint16_t)(((1u << TFA9873_FIELD_WIDTH(f)) - 1u) << TFA9873_FIELD_POS(f)))
enum {
    Tfa9873Pwdn = 0x0000, Tfa9873I2cr = 0x0010, Tfa9873Ampe = 0x0030, Tfa9873Dca = 0x0040, Tfa9873Mansconf = 0x0120,
    Tfa9873Audfs = 0x0203, Tfa9873Fractdel = 0x0255, Tfa9873Fssyncen = 0x0480,
    Tfa9873Tdme = 0x2000, Tfa9873Tdmslots = 0x2013, Tfa9873Tdmclinv = 0x2060, Tfa9873Tdmfspol = 0x20b0, Tfa9873Tdmnbck = 0x20c3,
    Tfa9873Tdmslln = 0x2144, Tfa9873Tdmdel = 0x21e0, Tfa9873Tdmssize = 0x2224, Tfa9873Tdmdce = 0x2310, Tfa9873Tdmcse = 0x2330,
    Tfa9873Tdmvse = 0x2340, Tfa9873Tdmspks = 0x2603, Tfa9873Tdmdcs = 0x2643, Tfa9873Tdmcss = 0x26c3, Tfa9873Tdmvss = 0x2703,
    Tfa9873Tdmspkg = 0x5f63, Tfa9873Lnmode = 0x62e1, Tfa9873Tdmsrcmap = 0x6802, Tfa9873Tdmsrcas = 0x6831, Tfa9873Tdmsrcbs = 0x6851,
    Tfa9873Encurcomp = 0x6f50, Tfa9873Dcmcc = 0x7003, Tfa9873Dctrip = 0x7444, Tfa9873Dctript = 0x7584, Tfa9873Dcvos = 0x7695,
};

enum {Tfa9873Write = 1, Tfa9873Update = 2, Tfa9873Read = 3, Tfa9873DelayMs = 4, Tfa9873Key = 5, Tfa9873Poll = 6};
/* Write: reg = value. Update: reg = (reg & ~mask) | value (no write when unchanged). Read: the value goes to the run's reads. DelayMs:
 * value ms. Key: read reg, write register aux = (read ^ value). Poll: read reg until (read & mask) == value, at most aux reads 1 ms apart;
 * running out counts in polls_failed and the program goes on (as the vendor's CLKS wait). */
typedef struct {uint8_t op, reg, aux, unused; uint16_t mask, value;} Tfa9873Step;

#define TFA9873_MAX_STEPS 96u
#define TFA9873_MAX_READS 16u

typedef struct {
    uint16_t rev;        /* the REV register as read (0x0a73, 0x0b73 and 0x1a73 are known) */
    uint8_t amp;         /* 0: 0x34 (top, slot 0); 1: 0x35 (bottom, slot 1) */
    uint8_t bits;        /* slot width: 32 (the container: 2 x 32-bit slots, 64 BCK per frame) or 16 (2 x 16) */
    uint8_t feedback;    /* 0: no current/voltage sense on the amplifier's data out (TDMCSE = TDMVSE = 0); 1: the container's TDMCSE = 1 */
} Tfa9873Config;

/* Programs; each returns the step count (at most TFA9873_MAX_STEPS) or 0 on a bad argument. */
unsigned tfa9873_identify_program(Tfa9873Step *out);
/* Reset (I2CR), keys, the revision's register table, interrupt enables, FSSYNCEN = 0, the container's device fields (TDM format between
 * TDME = 0 and TDME = 1), the "speaker" profile, the feedback override, then powered down (PWDN = 1, AMPE = 0). */
unsigned tfa9873_init_program(Tfa9873Step *out, const Tfa9873Config *config);
/* Reads the registers init wrote fields of, for tfa9873_verify. */
unsigned tfa9873_verify_program(Tfa9873Step *out);
/* With the clocks running: MANSCONF = 1, PWDN = 0, wait for CLKS (50 x 1 ms), AMPE = 1. */
unsigned tfa9873_power_up_program(Tfa9873Step *out);
/* PWDN = 1, then AMPE = 0. */
unsigned tfa9873_power_down_program(Tfa9873Step *out);
/* Reads STATUS, STATUS2, STATUS3, MANAGER, BATTERY, TEMPERATURE and IRQ_STATUS, then clears the latched interrupt bits. */
unsigned tfa9873_status_program(Tfa9873Step *out);
enum {Tfa9873StStatus, Tfa9873StStatus2, Tfa9873StStatus3, Tfa9873StManager, Tfa9873StBattery, Tfa9873StTemperature, Tfa9873StIrq,
      Tfa9873StCount};

/* 1 when rev is a TFA9873 (bits 7:0 = 0x73). */
int tfa9873_known(uint16_t rev);
/* How many registers the revision's table writes (0x0a73: 18, 0x0b73: 10, others none). */
unsigned tfa9873_table_size(uint16_t rev);
/* The value one field of the container gives an amplifier (after the profile and the feedback override); -1 when init does not set it. */
int tfa9873_expected_field(const Tfa9873Config *config, uint16_t field);
/* Checks the values the verify program read: 0 when every field init set reads back, else the first register that differs (its
 * read value and the expected masked bits in *got / *want). */
int tfa9873_verify(const Tfa9873Config *config, const uint16_t *reads, unsigned count, uint16_t *got, uint16_t *want);

/* The runner. */
typedef struct {
    const Tfa9873Step *steps;
    unsigned count, index;
    uint8_t phase;                   /* 0 next step; 1 read of an update in flight; 2 write in flight; 3 read in flight; 4 the write of an
                                        update is next; 5 key read in flight; 6 key write next; 7 poll read in flight; 8 poll wait next;
                                        9 poll read next */
    uint16_t pending;                /* the write of an update or key */
    unsigned tries;                  /* poll reads so far */
    uint16_t reads[TFA9873_MAX_READS];
    unsigned read_count;
    uint16_t last;                   /* the last value read */
    uint32_t transfers, polls_failed;
} Tfa9873Run;
enum {Tfa9873Transfer = 1, Tfa9873Wait = 2, Tfa9873Finished = 3};
typedef struct {
    uint8_t bytes[3];
    uint32_t length;                 /* bytes to write: 3 (register, value high, value low) or 1 (the register, then a read) */
    uint32_t read_length;            /* 0 or 2 */
    uint32_t delay_ms;               /* Tfa9873Wait */
} Tfa9873Action;
void tfa9873_run_start(Tfa9873Run *r, const Tfa9873Step *steps, unsigned count);
/* What to do next: Tfa9873Transfer (perform a, then call tfa9873_run_result), Tfa9873Wait (wait a.delay_ms, then call next again) or
 * Tfa9873Finished. */
int tfa9873_run_next(Tfa9873Run *r, Tfa9873Action *a);
/* The transfer finished; value: the 16-bit register value read (when one was read). */
void tfa9873_run_result(Tfa9873Run *r, uint16_t value);
/* Two bytes as read off the wire (first byte in bits 7:0) as the register value. */
static inline uint16_t tfa9873_wire_value(uint32_t rx) {return (uint16_t)((rx & 0xffu) << 8 | ((rx >> 8) & 0xffu));}

#ifdef __cplusplus
}
#endif
#endif
