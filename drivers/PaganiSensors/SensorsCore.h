/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* The sensor manager: portable C over a table of operations (PaganiSensors.cpp gives the kernel ones, Test/core.c a simulated sensor hub
 * and router), single threaded (the driver's own thread). It is a client of the sensor client service (SensorsSsc.h) on one IPC router
 * connection for the lookups (connection 0) and one per sensor (connection 1 + kind, as Android's HAL and libssc keep one per sensor):
 *
 *   service    waits for service 400 (any instance); a service that goes, or comes back with a new generation (a restarted sensor
 *              PD), starts everything over; reminders in the log at 10 s, 60 s, 5 min, then every 30 min while it is missing
 *   lookup     one SUID request per data type (register_updates, default_only, APSS, wakeup delivery; the answers may come any time
 *              later, also after the discovery-done event: the hub reports late sensors), optionally one for every data type (the list)
 *   attributes per sensor found: an attribute request (msg 1) on its connection, the attribute event read (5 s, three tries, then the
 *              sensor goes on with defaults)
 *   streams    (mode 3) what iOS asks for: the motion sensors at the rate iOS asks (the attribute rates' first at or above it) or
 *              off, ambient light always (on change), proximity while iOS has a detection mode; changes become new config requests
 *              (513 with the rate, 514 on change, 10 disable) on the sensor's connection
 *   samples    1025 (sensor event), 769/1031 (proximity) converted to iOS units (SensorsFixed.h) with the axis maps, given to the glue;
 *              with the orientation on, the accelerometer always streams (at least 25 Hz) and its samples feed SensorsOrient.h
 *
 * One request is outstanding per connection: the next goes out after the response (2 s timeout); a refused request is tried again after
 * 1, 2, 4 ... 60 s, a stream refused 8 times in a row is given up until the service restarts. Everything is logged as "[sensors] ..."
 * lines, rationed (a budget of lines per boot, then failures and an hourly summary only). */
#ifndef SENSORS_CORE_H
#define SENSORS_CORE_H
#include <stdint.h>
#include "PaganiSensors.h"
#include "SensorsFixed.h"
#include "SensorsOrient.h"
#include "SensorsSsc.h"
#ifdef __cplusplus
extern "C" {
#endif

/* Connections: 0 the SUID lookups, 1 + kind each sensor's, then the list of every data type (diagnostic, never used to pick a sensor). */
enum {SNS_CONN_LOOKUP = 0, SNS_CONN_LIST = 1 + PaganiSensorKinds, SNS_CONNS = 2 + PaganiSensorKinds};
enum {SNS_OFF = 0, SNS_ON_CHANGE = 1, SNS_STREAMING = 2};

typedef struct {
    unsigned kind;
    uint64_t timestamp;          /* the hub's (QTimer, 19.2 MHz ticks) */
    int32_t value[3];            /* iOS units: 16.16 g, rad/s, uT; 16.16 kPa in [0]; lux in [0] (raw count in [1]); proximity 1 near */
    int32_t milli[3];            /* the hub's own units x 1000 (m/s^2, rad/s, uT, hPa, lux), unmapped, for the log */
    int32_t status;              /* accuracy 0..3, -1 not given */
} SnsSample;

typedef struct {
    void *context;
    uint64_t (*now_ms)(void *context);
    /* One QMI message on connection conn to node:port. 0 when queued, nonzero when refused (the request then counts as failed). */
    int (*send)(void *context, unsigned conn, uint32_t node, uint32_t port, const uint8_t *data, uint32_t length);
    void (*trace)(void *context, const char *line);
    void (*sample)(void *context, const SnsSample *s);
    /* A sensor's SUID is known (the glue may then publish an iOS service it only publishes for present hardware, e.g. pressure). */
    void (*found)(void *context, unsigned kind);
    /* The device orientation changed (SensorsOrient.h; only with SnsConfig.orient). */
    void (*orientation)(void *context, int state);
} SnsOps;

typedef struct {
    uint32_t mode;                           /* 2: lookup and attributes; 3: also the streams */
    SfxAxes axes[3];                         /* accelerometer, gyroscope, magnetometer */
    uint32_t als_permille;                   /* 0: 1000 */
    char type[PaganiSensorKinds][SSC_TYPE_MAX];   /* the data types; empty: that kind is not asked for */
    uint32_t motion_hz;                      /* nonzero: the motion sensors stream at this rate without a request */
    uint32_t prox_always;
    uint32_t orient;                         /* compute the device orientation: the accelerometer stays on */
    uint32_t list;
    uint32_t log;                            /* 0 failures, 1 rationed, 2 more */
} SnsConfig;

typedef struct {
    uint16_t next_txn, pending_txn;
    int pending;                             /* a request is outstanding */
    uint32_t pending_msg;                    /* its sensor message id */
    uint32_t pending_tag;                    /* lookup: the kind (or PaganiSensorKinds for the list) */
    uint64_t sent_ms;
    int has_client;
    uint64_t client_id;
    uint32_t requests, responses, refused, timeouts, reports;
} SnsConn;

typedef struct {
    int found;
    SscSuid suid;
    unsigned suids;                          /* SUIDs the hub listed for the data type */
    int attr_state;                          /* 0 to ask, 1 asked, 2 read, 3 gave up */
    unsigned attr_tries;
    uint64_t attr_deadline;
    SscAttrs attrs;
    uint32_t demand_us;                      /* iOS's report interval (0: none) */
    int want, active, requested;             /* SNS_OFF / _ON_CHANGE / _STREAMING */
    uint32_t want_rate, active_rate, requested_rate;   /* float bits, Hz */
    unsigned tries;
    uint64_t retry_at;
    int failed;                              /* given up until the service restarts */
    uint32_t events, errors, bad, skipped;
    int first_logged, quiet_logged;
    uint64_t active_since, sample_ms;        /* when the stream was accepted, when the last sample came (our clock) */
    SnsSample last;
} SnsSensor;

typedef struct {
    const SnsOps *ops;
    SnsConfig cfg;
    int32_t stage, error;
    int up;
    uint32_t node, port, instance, generation;
    uint64_t wait_since;
    unsigned reminders;
    uint64_t reminder_at;
    unsigned lookup_sent;                    /* bit per kind (bit PaganiSensorKinds: the list): the request was accepted */
    unsigned lookup_tries;
    uint64_t lookup_retry_at;
    int discovery_done, discovery_logged;
    uint64_t discovery_started;
    SnsConn conn[SNS_CONNS];
    SnsSensor sensor[PaganiSensorKinds];
    unsigned budget;                         /* ordinary lines left */
    unsigned transitions;                    /* the service's ups and downs (the first 32 logged, then every 64th) */
    unsigned listed;                         /* list lines written */
    uint64_t summary_at;
    uint32_t sent, refused, responses, reports, malformed, unexpected, failures, stale;
    SnoState orient;
    char line[240];
} SnsCore;

void sns_core_init(SnsCore *c, const SnsOps *ops, const SnsConfig *config);
/* The service's state from the router (the glue reads it on every edge): up with its address and the generation of the port's server. */
void sns_core_server(SnsCore *c, int up, uint32_t node, uint32_t port, uint32_t instance, uint32_t generation);
/* One QMI message that arrived on connection conn from node:port. */
void sns_core_receive(SnsCore *c, unsigned conn, uint32_t node, uint32_t port, const uint8_t *data, uint32_t length);
/* iOS's request for a sensor: the report interval in microseconds (motion sensors), any nonzero value for on (proximity); 0 none. A
 * repeated proximity request gives the current state again (backboardd writes a detection mode for each change of mode and expects
 * the current mask each time; the hub only reports a change or a new request). */
void sns_core_demand(SnsCore *c, unsigned kind, uint32_t interval_us);
/* Timers and pending work. */
void sns_core_tick(SnsCore *c);
/* How long the thread may sleep before the next tick is due (1 .. 60000 ms). */
uint32_t sns_core_idle_ms(SnsCore *c);
/* The driver's stage for the parts before the service (Services, Ports) and their failures; logged by the glue. */
void sns_core_stage(SnsCore *c, int32_t stage, int32_t error);
/* Fills the core's parts of a status. */
void sns_core_status(const SnsCore *c, PaganiSensorsStatus *out);
const char *sns_kind_name(unsigned kind);

#ifdef __cplusplus
}
#endif
#endif
