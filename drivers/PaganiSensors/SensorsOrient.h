/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* The device orientation from the accelerometer, in the kernel. CoreMotion's motion capabilities are chosen by the product-id, which stays
 * J120's: hardware type 62 has the capability "the orientation comes from the always-on processor", so CLOrientationNotifier (in
 * backboardd, also in locationd) never computes it on the application processor and waits for orientation events (type 10, usage
 * 0x64 + state) of the HID service 0x20/0x8a. On this phone nothing else computes it: this state machine does, on the samples iOS gets
 * (16.16 g in iOS's convention: at rest the vector points down, portrait y = -1, face up z = -1).
 *
 * It is integer only (kernel code builds with -mgeneral-regs-only). A sample counts when the vector's length is 0.6 .. 1.4 g (not while
 * the phone is being shaken or falling). Face up/down: z holds at least 0.85 of the length (within about 32 degrees). Portrait/landscape: the
 * vector lies in the screen plane enough (x and y hold at least 0.4 of the length), and a change needs the leading axis to lead by 1.3
 * times (about 52 degrees: 7 degrees of hysteresis on each side of the diagonal). A new state must hold for SNO_HOLD_MS before it is
 * reported. */
#ifndef SENSORS_ORIENT_H
#define SENSORS_ORIENT_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* UIDeviceOrientation's order: the event's usage is 0x64 + state. */
enum {SNO_UNKNOWN = 0, SNO_PORTRAIT = 1, SNO_UPSIDE_DOWN = 2, SNO_LANDSCAPE_LEFT = 3, SNO_LANDSCAPE_RIGHT = 4, SNO_FACE_UP = 5,
      SNO_FACE_DOWN = 6, SNO_STATES = 7};
#define SNO_HOLD_MS 250u

typedef struct {
    int state;                  /* the state reported last (SNO_UNKNOWN before the first) */
    int candidate;              /* the state the samples show now */
    uint64_t since_ms;          /* when they began to */
} SnoState;

void sno_init(SnoState *s);
/* One sample. The state to report when it changes, else -1. */
int sno_update(SnoState *s, int32_t x, int32_t y, int32_t z, uint64_t now_ms);

#ifdef __cplusplus
}
#endif
#endif
