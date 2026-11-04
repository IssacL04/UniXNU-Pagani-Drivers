/* SPDX-License-Identifier: MIT */
#ifndef SM8750_TSENS_H
#define SM8750_TSENS_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Read-only GPU temperature of the SM8750 (A830) and the DCVS thermal cap, ARCHITECTURE 4.16 (M11).
 *
 * gpuss-0..7 (DT qcom,tzone-names of the GPU node) are sensors 1..8 of the TSENS controller tsens2. Its first register
 * range is the TM block (0x0C22A000, 0x1FF bytes; the second range, SROT at 0x0C224000, is never touched). Per sensor
 * n, TM_Sn_STATUS = TM + 0xA0 + 4n holds the last temperature in [11:0], signed, in deci-Celsius, and VALID in bit 21
 * (Linux drivers/thermal/qcom/tsens-v2.c: TM_Sn_STATUS_OFF 0xa0, LAST_TEMP_0 [11:0], VALID_0 bit 21). VALID drops for
 * a few AHB cycles while the controller updates a sensor, so an invalid read is retried a few times (Linux
 * get_temp_tsens_valid). Nothing here writes a TSENS register: thresholds, interrupts and calibration stay as the
 * firmware left them.
 *
 * The zones and their caps (ARCHITECTURE 4.16): Hot at 85 C or more (607 MHz), Critical at 105 C or more (389 MHz); a zone
 * is left only once the hottest sensor is 5 C under its threshold. When the sensors cannot be read at all (window not
 * mapped, or VALID never set) the zone is Unknown, cap 607 MHz for as long as that lasts. Since 2026-10-30 the design
 * profile's DCVS takes only Unknown (607 MHz) and Critical (its 389 MHz emergency) from here and runs a closed loop on the
 * readings around a target temperature instead of the Hot cap (A830Dcvs); the annibale profile uses the zone and the last
 * reading for its 70/60 C rule. No allocation, no logging; hardware only through `read`. */

typedef struct Sm8750TsensIo {
    void *context;
    uint32_t (*read)(void *context, uint32_t offset);   /* 32-bit read at a byte offset of the mapped TM window */
} Sm8750TsensIo;

enum {
    Sm8750TsensOk = 0,
    Sm8750TsensArgument = -22,      /* no window (not mapped) or a null argument */
    Sm8750TsensNoData = -61,        /* no GPU sensor reported VALID */
};

enum {
    Sm8750TsensTmBytes = 0x1ff,             /* DT tsens2 reg <0xc22a000 0x1ff ...> */
    Sm8750TsensStatusBase = 0xa0,           /* TM_Sn_STATUS(n) = 0xa0 + 4n */
    Sm8750TsensStatusStride = 4,
    Sm8750TsensValidBit = 1 << 21,
    Sm8750TsensTempMask = 0xfff,            /* signed 12 bits, deci-Celsius */
    Sm8750TsensGpuFirst = 1,                /* gpuss-0 */
    Sm8750TsensGpuLast = 8,                 /* gpuss-7 */
    Sm8750TsensGpuSensors = 8,
    Sm8750TsensReadTries = 3,               /* reads of one sensor while VALID is low */

    /* Thermal cap, ARCHITECTURE 4.16 (to be tuned in M11 against measured curves). */
    Sm8750ThermalHotDeci = 850,             /* >= 85.0 C: cap 607 MHz */
    Sm8750ThermalCriticalDeci = 1050,       /* >= 105.0 C: cap 389 MHz (the vendor's gpuss passive trip on the 13T) */
    Sm8750ThermalHysteresisDeci = 50,       /* a zone is left 5.0 C under its threshold */
    Sm8750ThermalHotCapKhz = 607000,
    Sm8750ThermalCriticalCapKhz = 389000,
    Sm8750ThermalUnreadableCapKhz = 607000, /* sensors unreadable */
    Sm8750ThermalInvalidLimit = 8,          /* consecutive reads without any VALID sensor before a once-valid reading
                                               counts as lost (about 130 ms of DCVS windows) */
};

/* One read of the eight GPU sensors. */
typedef struct Sm8750TsensSample {
    int32_t max_deci;                       /* hottest VALID sensor in deci-Celsius; 0 when valid == 0 */
    uint32_t valid;                         /* bit (n - 1) set: sensor n was VALID */
    uint32_t hottest;                       /* sensor number (1..8) of max_deci, 0 when none */
    uint32_t reads;                         /* register reads done, retries included */
    int16_t deci[Sm8750TsensGpuSensors];    /* per sensor, 0 when not VALID */
    uint32_t raw[Sm8750TsensGpuSensors];    /* last TM_Sn_STATUS read per sensor */
} Sm8750TsensSample;

/* Decode one TM_Sn_STATUS word: returns 1 and the sign-extended deci-Celsius value when VALID is set, else 0 (deci 0). */
int Sm8750TsensDecode(uint32_t status, int32_t *deci);

/* Read sensors 1..8 (each retried up to Sm8750TsensReadTries times while VALID is clear). Ok when at least one sensor
 * was VALID; NoData when none was; Argument without a window. `out` is always filled. */
int Sm8750TsensReadGpu(const Sm8750TsensIo *tm, Sm8750TsensSample *out);

typedef enum {
    Sm8750ThermalUnknown,                   /* no usable reading: cap Sm8750ThermalUnreadableCapKhz */
    Sm8750ThermalNormal,                    /* no cap */
    Sm8750ThermalHot,                       /* cap Sm8750ThermalHotCapKhz */
    Sm8750ThermalCritical,                  /* cap Sm8750ThermalCriticalCapKhz */
} Sm8750ThermalZone;

/* Zone tracking with hysteresis; one update per DCVS window. */
typedef struct Sm8750Thermal {
    uint32_t zone;                          /* Sm8750ThermalZone */
    uint32_t ever_valid;                    /* a reading was ever usable */
    uint32_t invalid_streak;                /* consecutive updates without a VALID sensor */
    int32_t last_deci;                      /* hottest sensor of the last usable reading */
    int32_t peak_deci;                      /* hottest reading seen */
    uint32_t samples, invalid_samples;      /* updates, updates without a usable reading */
    uint32_t transitions;                   /* zone changes */
} Sm8750Thermal;

void Sm8750ThermalInit(Sm8750Thermal *thermal);
/* Feed the result of Sm8750TsensReadGpu (Argument when there is no window) and its sample (may be NULL unless Ok).
 * Rules: Ok -> zone from the hottest sensor (enter Hot at >= 85.0 C, Critical at >= 95.0 C; leave Critical under
 * 90.0 C and Hot under 80.0 C, re-evaluated in the same update); no usable reading -> Unknown when no reading was ever
 * usable or after Sm8750ThermalInvalidLimit such updates in a row, otherwise the zone is kept. Returns the cap. */
uint32_t Sm8750ThermalUpdate(Sm8750Thermal *thermal, int read_result, const Sm8750TsensSample *sample);
/* Cap of the current zone in kHz; 0 = no cap. */
uint32_t Sm8750ThermalCapKhz(const Sm8750Thermal *thermal);

#ifdef __cplusplus
}
#endif
#endif
