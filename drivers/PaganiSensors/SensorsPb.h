/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* Protocol buffers wire format (proto2), our own encoder and decoder for the Qualcomm sensor client messages (SensorsSsc.c). Written from
 * the format's public definition: tag = field << 3 | wire type; varint (7 bits a byte, low first, the top bit continues), 64-bit and
 * 32-bit little endian, length-delimited (a varint length, then the bytes). Kernel code: no allocation, no floating point (floats travel
 * as their IEEE-754 bit patterns, SensorsFixed.h converts them), every write and read bounded.
 *
 * Encoder: an output buffer that records an overflow instead of writing past its end; nested messages are opened and closed in place
 * (the length is written minimal, the content moved when it needs more than one byte). Decoder: a cursor over one message; unknown fields
 * are skipped by their wire type, groups (wire types 3 and 4) are refused, a varint longer than ten bytes or anything past the end is an
 * error. Repeated scalars are read packed and unpacked alike. */
#ifndef SENSORS_PB_H
#define SENSORS_PB_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

enum {SPB_VARINT = 0, SPB_I64 = 1, SPB_LEN = 2, SPB_I32 = 5};

typedef struct {
    uint8_t *buf;
    uint32_t cap, len;
    int overflow;
} SpbOut;

void spb_out(SpbOut *o, uint8_t *buf, uint32_t cap);
void spb_varint(SpbOut *o, uint64_t value);
void spb_tag(SpbOut *o, uint32_t field, uint32_t wire);
void spb_put_varint(SpbOut *o, uint32_t field, uint64_t value);            /* uint32/uint64/bool/enum >= 0 */
void spb_put_int(SpbOut *o, uint32_t field, int64_t value);                /* int32/int64/enum: negative as a ten-byte varint */
void spb_put_fixed32(SpbOut *o, uint32_t field, uint32_t value);           /* fixed32, float bits */
void spb_put_fixed64(SpbOut *o, uint32_t field, uint64_t value);           /* fixed64, sfixed64 */
void spb_put_bytes(SpbOut *o, uint32_t field, const void *data, uint32_t length);
void spb_put_string(SpbOut *o, uint32_t field, const char *text);          /* the bytes up to the terminator (may be empty) */
/* A nested message: spb_open writes the tag and returns a mark for spb_close, which writes the length. */
uint32_t spb_open(SpbOut *o, uint32_t field);
void spb_close(SpbOut *o, uint32_t mark);
/* The encoded length, 0 after an overflow. */
uint32_t spb_done(const SpbOut *o);

typedef struct {
    const uint8_t *p, *end;
    int error;
} SpbIn;

void spb_in(SpbIn *in, const uint8_t *data, uint32_t length);
/* The next field: 1 with its number and wire type, 0 at the end, -1 on an error (also recorded in in->error). */
int spb_next(SpbIn *in, uint32_t *field, uint32_t *wire);
/* The value of the field just announced by spb_next, by its wire type. 0 when read, -1 on an error or a wrong wire type. */
int spb_varint_value(SpbIn *in, uint32_t wire, uint64_t *value);
int spb_fixed32_value(SpbIn *in, uint32_t wire, uint32_t *value);
int spb_fixed64_value(SpbIn *in, uint32_t wire, uint64_t *value);
int spb_bytes_value(SpbIn *in, uint32_t wire, const uint8_t **data, uint32_t *length);
int spb_skip(SpbIn *in, uint32_t wire);
/* A repeated fixed32/float field, packed (wire 2, n x 4 bytes) or one element (wire 5): appended to out while count < max (the rest is
 * counted in *more). 0 when read, -1 on an error. */
int spb_fixed32_values(SpbIn *in, uint32_t wire, uint32_t *out, unsigned max, unsigned *count, unsigned *more);
/* A repeated varint field, packed or one element, the same way. */
int spb_varint_values(SpbIn *in, uint32_t wire, int64_t *out, unsigned max, unsigned *count, unsigned *more);

#ifdef __cplusplus
}
#endif
#endif
