/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* PaganiSensors: the OnePlus 13T's (PKX110, SM8750) motion and environment sensors for iOS. The sensors hang off the ADSP's sensor
 * protection domain (msm/adsp/sensor_pd, Qualcomm's sensor hub); PaganiSensors is a client of its sensor client service (QMI service 400
 * over PaganiAdsp's IPC router), finds the sensors by data type (SUID lookup), reads their attributes, starts the streams iOS asks for and
 * gives iOS the samples as IOHIDEventService events: ambient light (lux), proximity (near/far), accelerometer (g), gyroscope (rad/s),
 * magnetometer (uT), and pressure (kPa) when the hub has a barometer. README.md has the whole design, the boot log and the limits.
 *
 * Boot arguments (all read once by pagani_sensors_start):
 *   pagani-sensors=N            0 (default, also absent): off, nothing allocated, registered or sent
 *                               1: the iOS sensor services only (registered at boot, no sample ever; no DSP traffic)
 *                               2: also the hub's SUID lookup and the sensors' attributes (needs pagani-adsp=1), no stream
 *                               3: also the streams iOS asks for, and MobileGestalt's accel and gyro nodes
 *                               +4: the FastRPC file service of the sensor PD (needs pagani-adsp=1 and the pack in the Data seed, without
 *                                  it every open fails and the paths are logged): 5 alone, 6 and 7 with the hub
 *                               +8 (with 3): the orientation service and its plug-in's user client (rotation): 11, or 15 with the file service
 *   pagani-sensors-nodes=0      no accel and gyro nodes even with mode 3
 *   pagani-sensors-rpc-pa=ADDR  the arena of the file service (default 0xd0100000; refused inside XNU's window or above 4 GiB)
 *   pagani-sensors-rpc-bank=N   its context bank, 3..8 (default 4, the sensors PD's on Android; its stream ids are 0x1000 + N ...)
 *   pagani-sensors-axes=MAP     axis maps in the hub's frame (SensorsFixed.h: a+x+y+z,g+x+y+z,m+x+y+z; default identity)
 *   pagani-sensors-als=TYPE     the ambient light sensor's data type (default ambient_light)
 *   pagani-sensors-prox=TYPE    the proximity sensor's data type (default proximity)
 *   pagani-sensors-als-scale=N  lux scale in permille (default 1000)
 *   pagani-sensors-motion-hz=N  stream the accelerometer, gyroscope and magnetometer at N Hz without an iOS request (default 0)
 *   pagani-sensors-prox-on=1    keep the proximity sensor on without an iOS request
 *   pagani-sensors-list=0/1     also list every data type the hub has (default 1 with pagani-sensors=2, else 0)
 *   pagani-sensors-log=0/1/2    failures only / rationed (default) / every stream change and the first samples of each start
 *
 * Linking: the kernel is linked statically and refuses an undefined weak reference; a caller that may be linked without PaganiSensors
 * defines PAGANI_SENSORS_WEAK_DEFAULTS in one file before including this header (weak definitions that report the driver as off). */
#ifndef PAGANI_SENSORS_H
#define PAGANI_SENSORS_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* The boot argument's fields. */
enum {PaganiSensorsModeMask = 3, PaganiSensorsFileService = 4, PaganiSensorsOrientation = 8};
/* Stages: PaganiSensorsStatus.stage is the last one reached, or minus the one that failed. */
enum {
    PaganiSensorsStOff = 0,
    PaganiSensorsStServices = 1,     /* the iOS services registered */
    PaganiSensorsStPorts = 2,        /* the IPC router ports open (PaganiAdsp running), waiting for the sensor client service */
    PaganiSensorsStService = 3,      /* service 400 announced by the sensor PD */
    PaganiSensorsStDiscovered = 4,   /* at least one sensor's SUID found */
    PaganiSensorsStDescribed = 5,    /* at least one sensor's attributes read */
    PaganiSensorsStStreaming = 6,    /* at least one stream accepted */
    PaganiSensorsStData = 7,         /* samples given to iOS */
};
/* Error codes beside the stage (negative errno style; the IOKit ones pass through as their own values). */
enum {
    PaganiSensorsErrOff = -19,       /* PaganiAdsp off (pagani-adsp=1 missing) or not linked */
    PaganiSensorsErrNoMemory = -12,
    PaganiSensorsErrClass = -2,      /* the HID device class could not be registered */
    PaganiSensorsErrService = -3,    /* an iOS service could not be attached or started */
    PaganiSensorsErrRefused = -5,    /* the hub refused a request every time it was tried */
    PaganiSensorsErrTimeout = -60,   /* no answer within the deadline (keeps waiting, the stage stays) */
};

/* The sensors iOS gets (the HID services, in this order). */
enum {PaganiSensorAccel = 0, PaganiSensorGyro, PaganiSensorMag, PaganiSensorLight, PaganiSensorProximity, PaganiSensorPressure,
      PaganiSensorKinds};

typedef struct {
    int32_t stage, error;
    uint32_t mode;
    uint32_t node, port, instance;     /* the sensor client service, while announced */
    uint32_t found;                    /* bit per kind: SUID known */
    uint32_t described;                /* bit per kind: attributes read */
    uint32_t streaming;                /* bit per kind: stream accepted by the hub */
    uint32_t published;                /* bit per kind: iOS service registered */
    uint32_t events[PaganiSensorKinds];
    uint32_t requests, responses, refused, reports, dropped, malformed;
    int32_t rpc_stage, rpc_error;      /* the file service: SsrSt* (negative: failed there), 0 when not asked for */
    uint32_t rpc_opens, rpc_missing;   /* files the sensor PD opened, paths it asked for that the pack does not have */
    char note[64];
} PaganiSensorsStatus;

void pagani_sensors_start(void);
/* A copy of the status; 0, or -19 when the driver is off. */
int pagani_sensors_status(PaganiSensorsStatus *out);

#ifdef PAGANI_SENSORS_WEAK_DEFAULTS
__attribute__((weak)) void pagani_sensors_start(void) {}
__attribute__((weak)) int pagani_sensors_status(PaganiSensorsStatus *out)
{
    (void)out;
    return -19;
}
#endif

#ifdef __cplusplus
}
#endif
#endif
