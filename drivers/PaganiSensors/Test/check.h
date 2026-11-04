// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026, UniXNU contributors. All rights reserved.
#ifndef SENSORS_TEST_CHECK_H
#define SENSORS_TEST_CHECK_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static unsigned checks;
#define CHECK(x) do {if (!(x)) {fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1);} ++checks;} while (0)

/* "0a 05 61" -> bytes; returns the count */
static inline uint32_t hex(const char *text, uint8_t *out, uint32_t cap)
{
    uint32_t n = 0;
    int high = -1;
    for (const char *p = text; *p; ++p) {
        int v = *p >= '0' && *p <= '9' ? *p - '0' : *p >= 'a' && *p <= 'f' ? *p - 'a' + 10 : *p >= 'A' && *p <= 'F' ? *p - 'A' + 10 : -1;
        if (v < 0) continue;
        if (high < 0) {high = v; continue;}
        if (n >= cap) {fprintf(stderr, "hex vector too long\n"); exit(1);}
        out[n++] = (uint8_t)(high << 4 | v);
        high = -1;
    }
    if (high >= 0) {fprintf(stderr, "odd hex vector\n"); exit(1);}
    return n;
}

static inline int same_bytes(const uint8_t *a, uint32_t an, const uint8_t *b, uint32_t bn)
{
    if (an != bn || memcmp(a, b, an)) {
        fprintf(stderr, "got (%u):     ", an);
        for (uint32_t i = 0; i < an; ++i) fprintf(stderr, "%02x ", a[i]);
        fprintf(stderr, "\nexpected (%u): ", bn);
        for (uint32_t i = 0; i < bn; ++i) fprintf(stderr, "%02x ", b[i]);
        fprintf(stderr, "\n");
        return 0;
    }
    return 1;
}

static inline uint32_t float_bits(float f)
{
    uint32_t b;
    memcpy(&b, &f, 4);
    return b;
}
static inline float bits_float(uint32_t b)
{
    float f;
    memcpy(&f, &b, 4);
    return f;
}
#endif
