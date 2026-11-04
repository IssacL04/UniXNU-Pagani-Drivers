/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* PaganiHaptics: the OnePlus 13T's vibrator (the PMIH010x HAP530 haptics module, SPMI slave 7, 0xf000/0xf100/0xf200) for the iOS side:
 * a kernel API for the audio service and its plug-in (constant vibration, the board's short effects, PCM-like streaming into the
 * hardware FIFO, stop). See README.md.
 *
 * Boot arguments (all optional):
 *   pagani-haptics=N            0 or absent: off (nothing runs, maps or allocates); 1 on; 2 dry run: discovery, identity, owners and
 *                               a register dump in the boot log (observer reads) and one read of the CFG RW channel's STATUS (the
 *                               chnls page reachable); nothing written to the haptics module
 *   pagani-haptics-vmax-mv=MV   runtime drive-voltage cap (default 3600, the board's effect voltage; never above the hard cap 7450,
 *                               the board's FIFO full scale)
 *   pagani-haptics-play-ms=MS   one constant play or effect at most this long (default 2000, at most 10000)
 *   pagani-haptics-autores=0    DIRECT_PLAY in open loop at the LRA period (default 1: closed loop, auto resonance)
 *   pagani-haptics-log=N        0 failures only, 1 rationed (default), 2 every play
 *   pagani-haptics-test=MS      bring-up: one constant vibration of MS ms (at most 500) right after the initialization (default 0)
 *   pagani-haptics-ios=N        the iOS haptics switch (HapticsIos.h): 1 creates IODeviceTree:/product/haptics with
 *                               supports-3rd-party-haptics (stage A), 2 also closed-loop (stage B, stage A without the tunings
 *                               overlay); only with pagani-haptics=1, pagani-audio=1 and the plug-in on the root (its Actuator
 *                               device), just before launchd; absent or 0: no node (MobileGestalt: no haptics)
 *   pagani-audio-srzl=N         bit 2 (value 4): an SPMI transaction the arbiter dropped goes again, at most three times (HapticsSpmi.h);
 *                               default off: a dropped transaction ends the driver until reboot. The argument's other bits are PaganiAudio's
 *
 * Calls (any thread that may block briefly; never from interrupt context). They queue a command for the driver's thread and return at
 * once: 0 queued, -19 the driver is not running (off, dry run, refused or faulted), -22 bad argument, -16 queue full. What happened is
 * in pagani_haptics_status and the boot log. */
#ifndef PAGANI_HAPTICS_H
#define PAGANI_HAPTICS_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Stage7Probe's call after the battery start (its weak default does nothing when PaganiHaptics is not linked). Returns at once. */
void pagani_haptics_start(void);

/* DIRECT_PLAY: amplitude 1..1000 permille of full scale for duration_ms; vmax_mv 0 = the board's 1260 mV (caps apply). */
int pagani_haptics_constant(uint32_t amplitude_permille, uint32_t duration_ms, uint32_t vmax_mv);
/* One of the board's FIFO effects (0..14: effect_0..5, primitive_0..8 of the device tree) at strength 1..1000 permille of 3600 mV. */
int pagani_haptics_effect(uint32_t effect, uint32_t strength_permille);
/* Streaming: signed 8-bit samples (the drive waveform, full scale = vmax_mv; 0 = the cap) at 8000, 16000, 24000, 32000, 44100 or 48000
 * Hz. begin, then write (returns how many samples were taken, 0 when the 16384-sample ring is full), then end (drain 1: play what was
 * written; 0: stop now). One producer at a time. A stream that stops being fed is stopped 100 ms after the FIFO runs dry. */
int pagani_haptics_stream_begin(uint32_t rate_hz, uint32_t vmax_mv);
int pagani_haptics_stream_write(const int8_t *samples, uint32_t count);
int pagani_haptics_stream_end(int drain);
/* Stops whatever plays (with the board's brake after DIRECT_PLAY). */
int pagani_haptics_stop(void);

typedef struct {
    int32_t stage, state;          /* pagani_haptics_status[0], [1] */
    uint32_t f0_dhz;               /* measured resonance in 0.1 Hz (0 until a closed-loop play of 120 ms or more) */
    uint32_t t_lra_us;             /* the LRA period in use (measured, else the board's 5882) */
    uint32_t cap_mv, hard_mv;      /* drive caps */
    uint32_t fifo_len;             /* hardware FIFO (8192 after the init) */
    uint32_t ring_free;            /* samples stream_write can take now */
} PaganiHapticsInfo;
int pagani_haptics_info(PaganiHapticsInfo *out);

/* Diagnostics (HapticsCore.h pgh_core_export): [0] stage (1 init, 2 dry run done, 3 ready; negative: -1 config, -2 not the v7
 * arbiter, -3 identity/owner refused, -4 write channel, -5 init failed, -9 fault), [1] state, [2] plays, [3] streams, [4] SPMI writes,
 * [5] reads, [6] refused | failures << 8 | timeouts << 16, [7] f0 (0.1 Hz), [8] VMAX code (x 50 mV), [9] fault bits seen, [10] stream
 * underruns, [11] on-time budget ms, [12] measured LRA period us, [13] last error, [14] clips, [15] stream samples. */
extern volatile int32_t pagani_haptics_status[16];

#ifdef PAGANI_HAPTICS_WEAK_DEFAULTS
/* A client that must link without PaganiHaptics defines PAGANI_HAPTICS_WEAK_DEFAULTS in exactly one file before including this header:
 * these report the driver as absent; PaganiHaptics' strong definitions replace them whenever it is linked. */
__attribute__((weak)) int pagani_haptics_constant(uint32_t a, uint32_t b, uint32_t c) {(void)a; (void)b; (void)c; return -19;}
__attribute__((weak)) int pagani_haptics_effect(uint32_t a, uint32_t b) {(void)a; (void)b; return -19;}
__attribute__((weak)) int pagani_haptics_stream_begin(uint32_t a, uint32_t b) {(void)a; (void)b; return -19;}
__attribute__((weak)) int pagani_haptics_stream_write(const int8_t *s, uint32_t n) {(void)s; (void)n; return -19;}
__attribute__((weak)) int pagani_haptics_stream_end(int d) {(void)d; return -19;}
__attribute__((weak)) int pagani_haptics_stop(void) {return -19;}
__attribute__((weak)) int pagani_haptics_info(PaganiHapticsInfo *o) {(void)o; return -19;}
#endif

#ifdef __cplusplus
}
#endif
#endif
