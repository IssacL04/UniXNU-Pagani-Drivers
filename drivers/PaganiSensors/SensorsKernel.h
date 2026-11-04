/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* Between the kernel files of PaganiSensors: PaganiSensors.cpp (start, thread, IPC router, manager), PaganiSensorsHid.cpp (the HID device
 * class, registered late, the event drivers' hooks and the events) and PaganiSensorsRpc.cpp (the FastRPC file service). Kernel only. */
#ifndef SENSORS_KERNEL_H
#define SENSORS_KERNEL_H
#include <stdint.h>
#include "SensorsCore.h"
#include "SensorsRpc.h"
#ifdef __cplusplus
extern "C" {
#endif

/* iOS asked something of a service (a report interval, a detection mode), as the manager's demand (SensorsHid.h); the orientation
 * service's kind when its plug-in opened (it wants the current state). Any thread. */
typedef void (*PaganiSensorsDemandFn)(unsigned kind, uint32_t demand);

/* The class (its initializer taken out of __mod_init_func by the build) and the original event driver personalities of the kinds in
 * mask. 0, or the stage error (PaganiSensorsErrClass, an IOReturn). */
int pagani_sensors_hid_setup(PaganiSensorsDemandFn demand, uint32_t mask);
/* One kind's device on the resource service, on the shared work loop: 0, or an IOReturn. */
int pagani_sensors_hid_publish(unsigned kind);
/* An empty node of that name under /product in IODeviceTree (MobileGestalt's accelerometer and gyroscope): 0 created, 1 already there,
 * negative failed. Synchronous: launchd and MobileGestalt must find it. */
int pagani_sensors_hid_node(const char *name);
/* The orientation service's report for a state; the replay sends the last state again (its bit from 0 to 1, or the driver stays silent). */
void pagani_sensors_hid_orientation(int state);
void pagani_sensors_hid_orientation_replay(void);
/* Finds the kind's IOHIDEventDriver and installs the hooks (report interval, detection mode, copies of the last light and proximity
 * events): 1 bound (now or before), 0 not there yet, negative refused (a driver table that is not the qualified original). */
int pagani_sensors_hid_bind(unsigned kind, char *line, uint32_t size);
/* A sample as an event through the bound driver (dropped while unbound). Any one thread at a time (the manager's). */
void pagani_sensors_hid_dispatch(const SnsSample *sample);
/* Counters for the status line: events given to iOS, dropped while unbound, timestamps replaced by the arrival time. */
void pagani_sensors_hid_counts(uint32_t *dispatched, uint32_t *unbound, uint32_t *restamped);

/* The FastRPC reverse file service (PaganiSensorsRpc.cpp): 0 started, negative not (-19 PaganiAdsp off, -22 a bad arena address). */
int pagani_sensors_rpc_start(unsigned log);
int pagani_sensors_rpc_status(SsrStatus *out);

#ifdef __cplusplus
}
#endif
#endif
