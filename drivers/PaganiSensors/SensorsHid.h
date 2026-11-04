/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* What iOS 17.7 (21H461) reads, per sensor: the HID service each kind is published as, and the event each sample becomes. Portable C
 * (the kernel glue PaganiSensorsHid.cpp and the host tests use the same tables). Facts from the static reverse engineering of the
 * consumers (build-userspace/a830-inputs/sensors/ios-sensors.md sections 2 to 4):
 *
 *   kind           usage pair      extra properties       event                                   consumer
 *   accelerometer  0xff00/0x03     Built-In               13 accelerometer x y z (16.16 g)        locationd, CoreMotion, backboardd
 *   gyroscope      0xff00/0x09     Built-In               20 gyro x y z (16.16 rad/s)             locationd (CLGyro)
 *   magnetometer   0xff00/0x0a     Built-In               21 compass x y z (16.16 uT)             locationd (CLMagnetometer)
 *   ambient light  0xff00/0x04     Built-In               12 ambient light level (integer lux)    CoreBrightness (auto-brightness)
 *   proximity      0xff00/0x08     Built-In, Placement 1  14 proximity mask (0x40 near, 0 far)    backboardd (screen off in calls)
 *   pressure       0x0020/0x31     Built-In               31 atmospheric pressure (16.16 kPa)     locationd (CLPressure)
 *   orientation    0x0020/0x8a     Built-In, 'ashd' above 10 orientation, usage 0x64 + state      CoreMotion in backboardd and locationd
 *
 * The services are the original IOHIDEventDriver on our IOHIDDevice: its descriptor holds one top-level application collection with the
 * kind's usage pair, the sensor property feature elements (Sensor page 0x30e report interval, 0x31b report latency), which
 * IOHIDEventDriver's parser accepts (so it starts) and which carry iOS's report interval, and for the motion sensors their x/y/z input
 * elements (never sent, the shape of a real HID motion sensor for the driver's own parsers); the events are built with the original
 * IOHIDEvent constructors and handed to the driver's dispatchEvent (the original driver has no ambient light or pressure parsing, and its
 * proximity path only knows the masks 0 and 1, so no sensor goes through input reports). No "Transport" = "SPU": that is the real
 * always-on processor's, and AppleSPU's plug-ins would match.
 *
 * The orientation service is the exception, and it is a trap. Its descriptor has the seven one-bit inputs of the 0xff0c usages
 * 0x64..0x6a; a report with a bit going from 0 to 1 makes the original driver send the event itself. But the moment the service exists,
 * CoreMotion's CLOrientationNotifier (in backboardd, every boot) takes the service's grandparent, opens the AppleSPULib plug-in on it and
 * sends a configuration through the interface without checking that it opened: without that plug-in's user client, a null dereference
 * (ios-sensors.md, conclusion 4). So this service is published only together with what makes the open succeed: on the same device
 * object (the grandparent of the event driver) IOCFPlugInTypes naming the plug-in, the user client of type 'ashd' with the methods
 * SNS_HID_SPU_* below, and NativeSandboxGrantAll (XNU patch zzwc-native-adreno-sandbox-grant-all: backboardd and locationd are
 * sandboxed, and their profiles know Apple's user client class, not ours). PaganiSensorsHid.cpp sets all of them before the device
 * starts: there is no moment when the service is visible and the user client is not. */
#ifndef SENSORS_HID_H
#define SENSORS_HID_H
#include <stdint.h>
#include "SensorsCore.h"
#ifdef __cplusplus
extern "C" {
#endif

/* The HID kinds: the sensors of PaganiSensors.h, then the orientation service (no sensor of the hub: computed here, SensorsOrient.h). */
enum {SNS_HID_ORIENT = PaganiSensorKinds, SNS_HID_KINDS = PaganiSensorKinds + 1};

enum {
    SNS_HID_EVENT_AMBIENT_LIGHT = 12, SNS_HID_EVENT_ACCELEROMETER = 13, SNS_HID_EVENT_PROXIMITY = 14, SNS_HID_EVENT_GYRO = 20,
    SNS_HID_EVENT_COMPASS = 21, SNS_HID_EVENT_PRESSURE = 31,
};
#define SNS_HID_PROXIMITY_NEAR 0x40u    /* kIOHIDProximityDetectionReceiver: backboardd's "object present" in its active modes */

typedef struct {
    uint16_t page, usage;
    const char *product;               /* the device's Product: the personality matches it */
    uint32_t placement;                /* 0: not set */
} SnsHidKind;
const SnsHidKind *sns_hid_kind(unsigned kind);

/* The report descriptor of a kind (its length in *length). */
const uint8_t *sns_hid_descriptor(unsigned kind, uint32_t *length);
/* The IOKit personalities (an XML array, one dictionary per kind in mask) matching the original IOHIDEventDriver to our devices; the
 * length written (terminated), 0 when it did not fit. */
uint32_t sns_hid_personalities(uint32_t mask, char *out, uint32_t capacity);

/* One event: the original constructor's arguments for a sample. */
typedef struct {
    uint32_t type;                     /* SNS_HID_EVENT_* */
    int32_t x, y, z;                   /* motion: 16.16; pressure: level 16.16 in x */
    uint32_t level;                    /* ambient light: lux; proximity: the hub's raw count */
    uint16_t mask;                     /* proximity */
    uint32_t sequence;
} SnsHidEvent;
/* 0 and the event of a sample, -1 for a kind without one. */
int sns_hid_event(const SnsSample *s, uint32_t sequence, SnsHidEvent *out);

/* iOS's properties on a service, as the manager's demand: the report interval (us) of the motion services and pressure ("ReportInterval";
 * 0 stops), the detection mode of proximity ("DetectionMode": 0 off, else on). */
enum {SNS_HID_KEY_REPORT_INTERVAL = 1, SNS_HID_KEY_DETECTION_MODE = 2};
/* 1 and the demand when the key concerns the kind, else 0. */
int sns_hid_demand(unsigned kind, unsigned key, uint64_t value, uint32_t *demand);

/* The orientation service's input report for a state (SensorsOrient.h): report id and the state's bit. 2, or 0 for a bad state. */
uint32_t sns_hid_orient_report(int state, uint8_t out[2]);

/* The AppleSPULib plug-in's side of the orientation service (ios-sensors.md 2.7, AppleSPUHIDDeviceClass): the user client type and the
 * selectors CoreMotion uses; the rest of the interface is never called on this path. */
#define SNS_HID_SPU_CLIENT 0x61736864u   /* 'ashd' */
#define SNS_HID_SPU_PLUGIN_TYPE "7ACF5332-1A35-4893-87CB-BA64E1887FAE"
#define SNS_HID_SPU_PLUGIN_PATH "AppleSPU.kext/PlugIns/AppleSPULib.plugin"
enum {SNS_HID_SPU_OPEN = 4, SNS_HID_SPU_CLOSE = 5, SNS_HID_SPU_COMMAND = 6, SNS_HID_SPU_CONFIG = 0x20};
/* 0 for a selector that succeeds (with no output), -1 unsupported. A command is recorded in *command when given. */
int sns_hid_spu_method(uint32_t selector, uint32_t scalars, uint64_t first_scalar, uint32_t *command);

/* The IODeviceTree nodes MobileGestalt's accelerometer and gyroscope answers look for, by name through the whole plane. Only a boot that
 * asks for the streams (mode 3) claims them, and only for a service that is really there; never in the board's static device tree (a
 * daily image would then answer "has an accelerometer" with no data source). The nodes to create: bit 0 accel, bit 1 gyro. */
enum {SNS_HID_NODE_ACCEL = 1, SNS_HID_NODE_GYRO = 2};
unsigned sns_hid_nodes(unsigned mode, uint32_t published, unsigned wanted);

#ifdef __cplusplus
}
#endif
#endif
