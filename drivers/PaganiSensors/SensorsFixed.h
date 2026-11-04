/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* Integer arithmetic on IEEE-754 single precision bit patterns, for kernel code built without floating point registers
 * (-mgeneral-regs-only): the sensor client service sends its samples, rates and attributes as floats, iOS wants integers and 16.16 fixed
 * point (IOFixed). Also the axis maps of the 13T's sensors (identity by default: the sensor hub reports in the phone's frame, x right,
 * y up, z out of the screen, which is iOS's frame too) and the unit conversions to the HID events iOS reads:
 *   accelerometer  m/s^2 (gravity reads +9.81 on z lying face up)  ->  g, sign reversed (face up z = -1 g), IOFixed
 *   gyroscope      rad/s                                           ->  rad/s, IOFixed
 *   magnetometer   uT                                              ->  uT, IOFixed
 *   pressure       hPa                                             ->  kPa, IOFixed
 *   ambient light  lux                                             ->  lux, integer (times a scale in permille) */
#ifndef SENSORS_FIXED_H
#define SENSORS_FIXED_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* round(value(bits) * num / den * 2^shift) (half away from zero), saturated to [lo, hi]; NaN gives 0, infinities saturate.
 * |num| < 2^39, 0 < den < 2^31. */
int64_t sfx_scale(uint32_t bits, int64_t num, int64_t den, int shift, int64_t lo, int64_t hi);
/* The float nearest num / den (ties to even); num, den < 2^32, 0 when either is 0. */
uint32_t sfx_float_from_ratio(uint64_t num, uint64_t den);
/* value * 1000, rounded and saturated to int32: the log's milli-units. */
int32_t sfx_milli(uint32_t bits);
/* 1 for a positive finite float (rates), comparable as unsigned integers. */
int sfx_positive(uint32_t bits);

#define SFX_STANDARD_GRAVITY_X100000 980665   /* 9.80665 m/s^2 */
int32_t sfx_accel_ios(uint32_t bits);        /* -v / g in 16.16 */
int32_t sfx_fixed(uint32_t bits);            /* v in 16.16 */
int32_t sfx_pressure_kpa(uint32_t bits);     /* hPa / 10 in 16.16 */
uint32_t sfx_lux(uint32_t bits, uint32_t permille);   /* lux * permille / 1000, at least 0 */

/* out[i] = (neg[i] ? -1 : 1) * in[src[i]] */
typedef struct {
    uint8_t src[3], neg[3];
} SfxAxes;
void sfx_axes_identity(SfxAxes *a);
int sfx_axes_valid(const SfxAxes *a);
void sfx_axes_apply(const SfxAxes *a, const int32_t in[3], int32_t out[3]);
/* "a-y+x+z,g+x+y+z,m+y-x+z": per sensor (a accelerometer, g gyroscope, m magnetometer) the three output axes, each a sign and the input
 * axis it takes; groups separated by ',' or '.', any subset, each input axis once per group. 0 and the maps set; -1 (nothing changed)
 * on any error. */
int sfx_axes_parse(const char *text, SfxAxes *accel, SfxAxes *gyro, SfxAxes *mag);
/* The text form of one map ("+x+y+z"), 7 bytes with the terminator. */
void sfx_axes_text(const SfxAxes *a, char out[7]);

#ifdef __cplusplus
}
#endif
#endif
