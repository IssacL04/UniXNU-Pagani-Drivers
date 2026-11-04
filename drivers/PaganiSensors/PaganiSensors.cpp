// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026, UniXNU contributors. All rights reserved.
// Kernel side of PaganiSensors (PaganiSensors.h, README.md). pagani_sensors_start (Stage7Probe, after pagani_adsp_start) reads its boot
// arguments and returns at once with pagani-sensors absent or 0: nothing allocated, registered or sent. Otherwise it registers the HID
// class and the personalities, publishes the iOS services (ambient light, proximity, accelerometer, gyroscope, magnetometer; the
// barometer only once the hub has one; the orientation service with its plug-in's user client when asked), with the streams asked for
// creates MobileGestalt's accel and gyro nodes before it returns (launchd and MobileGestalt run later and never look again), starts the
// FastRPC file service when asked (PaganiSensorsRpc.cpp), and from mode 2 opens PaganiAdsp IPC router ports for the sensor client
// service, then starts one kernel thread, "pagani-sensors", which binds the services' event drivers, runs the manager (SensorsCore.c)
// and hands its samples to iOS.
//
// Threads: the router's callbacks run on PaganiAdsp's DSP thread: a received message is copied into a single-producer ring and the
// thread is woken, a server change only wakes it (it reads the server's level and generation itself). iOS's property writes (report
// interval, detection mode) arrive on the writers' threads through the event drivers' hooks: one word per kind and a wake. Everything else
// is the thread's. Nothing panics, nothing waits on the boot: every failure is a stage and a code in pagani_sensors_status and a
// "[sensors] FAIL st=.. err=.." line in the BootProgress ramoops log (U records, pagani-ramoops=1).
#include <IOKit/IOLib.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
#define PAGANI_ADSP_WEAK_DEFAULTS 1
extern "C" {
#include "PaganiAdsp.h"
#include "PaganiSensors.h"
#include "SensorsCore.h"
#include "SensorsHid.h"
#include "SensorsKernel.h"
#include "SensorsText.h"
}

// The statically linked kernel refuses undefined weak references: BootProgress may be absent.
extern "C" __attribute__((weak)) void boot_progress_trace(const char *) {}

namespace {
enum : uint32_t {
    kRingSlots = 48,
    kSlotBytes = 1088,            // a whole report: 7 + 11 + 5 + 1000 bytes, and some
    kEvData = 1u << 0,
    kEvServer = 1u << 1,
    kEvDemand = 1u << 2,
    kBindFastMs = 100,            // the event drivers attach within milliseconds of the devices' start
    kBindSlowMs = 1000,
    kBindGiveUpMs = 120000,
    kStatusMs = 600000,           // a status line every 10 minutes while samples flow (rationed by the core's budget otherwise)
};

struct Slot {
    uint32_t conn, node, port, length;
    uint8_t data[kSlotBytes];
};

volatile uint32_t gStarted, gPending, gDemand[SNS_HID_KINDS], gDemandDirty;
uint32_t gMode, gLog, gRpcStart;
int gRpcResult;
Slot *gRing;
uint32_t gHead, gTail;            // atomic: the DSP thread moves the head, this thread the tail
volatile uint32_t gDropped, gTooLong;
PaganiAdspQrtrPort *gPort[SNS_CONNS];
SnsCore *gCore;
SnsConfig gConfig;
IOSimpleLock *gStatusLock;
PaganiSensorsStatus gStatus;
int32_t gStage, gError;           // before the core exists (and for mode 1)
uint32_t gPublished, gBound, gRefused;
uint64_t gBindStart;
char gLine[240];

uint64_t nowMs()
{
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    return ns / 1000000u;
}

void trace(const char *line)
{
    boot_progress_trace(line);
    IOLog("%s\n", line);
}

unsigned argument(const char *name, unsigned fallback)
{
    unsigned value = fallback;
    return PE_parse_boot_argn(name, &value, sizeof value) ? value : fallback;
}

void wake(uint32_t bits)
{
    __atomic_fetch_or(&gPending, bits, __ATOMIC_RELEASE);
    thread_wakeup((event_t)&gPending);
}

uint32_t waitEvents(uint32_t ms)
{
    uint32_t bits = __atomic_exchange_n(&gPending, 0u, __ATOMIC_ACQ_REL);
    if (bits) return bits;
    assert_wait_timeout((event_t)&gPending, THREAD_UNINT, ms ? ms : 1, kMillisecondScale);
    bits = __atomic_exchange_n(&gPending, 0u, __ATOMIC_ACQ_REL);
    if (bits) {
        clear_wait(current_thread(), THREAD_AWAKENED);
        return bits;
    }
    thread_block(THREAD_CONTINUE_NULL);
    return __atomic_exchange_n(&gPending, 0u, __ATOMIC_ACQ_REL);
}

void fail(int32_t stage, int32_t error, const char *why)
{
    SntText t;
    gStage = -stage;
    gError = error;
    if (gCore) sns_core_stage(gCore, -stage, error);
    snt_start(&t, gLine, sizeof gLine);
    snt_str(&t, "[sensors] FAIL");
    snt_kd(&t, "st", -stage);
    snt_kd(&t, "err", error);
    snt_char(&t, ' ');
    snt_str(&t, why);
    trace(gLine);
}

// ---- iOS's demand (any thread, through the event drivers' hooks) ----
void demand(unsigned kind, uint32_t value)
{
    if (kind >= SNS_HID_KINDS) return;
    __atomic_store_n(&gDemand[kind], value, __ATOMIC_RELEASE);
    __atomic_fetch_or(&gDemandDirty, 1u << kind, __ATOMIC_RELEASE);
    wake(kEvDemand);
}

// ---- the IPC router's callbacks (PaganiAdsp's DSP thread) ----
void onServer(void *, int, uint32_t, uint32_t, uint32_t)
{
    wake(kEvServer);
}

void onReceive(void *context, uint32_t node, uint32_t port, const uint8_t *data, uint32_t length)
{
    const uint32_t head = __atomic_load_n(&gHead, __ATOMIC_RELAXED), tail = __atomic_load_n(&gTail, __ATOMIC_ACQUIRE);
    if (length > kSlotBytes) {
        __atomic_fetch_add(&gTooLong, 1u, __ATOMIC_RELAXED);
        return;
    }
    if (head - tail >= kRingSlots) {
        __atomic_fetch_add(&gDropped, 1u, __ATOMIC_RELAXED);
        wake(kEvData);
        return;
    }
    Slot &slot = gRing[head % kRingSlots];
    slot.conn = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(context));
    slot.node = node;
    slot.port = port;
    slot.length = length;
    memcpy(slot.data, data, length);
    __atomic_store_n(&gHead, head + 1, __ATOMIC_RELEASE);
    wake(kEvData);
}

// ---- the manager's operations ----
uint64_t opNow(void *) {return nowMs();}
int opSend(void *, unsigned conn, uint32_t node, uint32_t port, const uint8_t *data, uint32_t length)
{
    if (conn >= SNS_CONNS || !gPort[conn]) return -1;
    return pagani_adsp_qrtr_send(gPort[conn], node, port, data, length) == PaganiAdspOk ? 0 : -1;
}
void opTrace(void *, const char *line) {trace(line);}
void opSample(void *, const SnsSample *s) {pagani_sensors_hid_dispatch(s);}
void opFound(void *, unsigned kind)
{
    // A barometer is published only when the hub has one (the 13T most likely has none: no service, no MobileGestalt answer).
    if (kind != PaganiSensorPressure || (gPublished & (1u << kind))) return;
    const int rc = pagani_sensors_hid_publish(kind);
    if (!rc) {
        gPublished |= 1u << kind;
        trace("[sensors] hid pressure published (the hub has a barometer)");
    } else {
        fail(PaganiSensorsStServices, rc, "pressure service not published");
    }
}
void opOrientation(void *, int state) {pagani_sensors_hid_orientation(state);}
const SnsOps kOps = {nullptr, opNow, opSend, opTrace, opSample, opFound, opOrientation};

void snapshot()
{
    PaganiSensorsStatus s = {};
    if (gCore) sns_core_status(gCore, &s);
    if (!gCore || gStage < 0) {s.stage = gStage; s.error = gError;}
    s.mode = gMode;
    s.published = gPublished;
    s.dropped = __atomic_load_n(&gDropped, __ATOMIC_RELAXED) + __atomic_load_n(&gTooLong, __ATOMIC_RELAXED);
    uint32_t dispatched = 0, unbound = 0, restamped = 0;
    pagani_sensors_hid_counts(&dispatched, &unbound, &restamped);
    SntText t;
    snt_start(&t, s.note, sizeof s.note);
    snt_str(&t, "bound=");
    snt_hex(&t, gBound);
    snt_kd(&t, "ios", dispatched);
    snt_kd(&t, "unbound", unbound);
    snt_kd(&t, "restamped", restamped);
    SsrStatus rpc;
    if (!pagani_sensors_rpc_status(&rpc)) {
        s.rpc_stage = rpc.stage;
        s.rpc_error = rpc.error;
        s.rpc_opens = rpc.opens;
        s.rpc_missing = rpc.missing;
    } else if (gRpcResult) {
        s.rpc_error = gRpcResult;
    }
    IOSimpleLockLock(gStatusLock);
    gStatus = s;
    IOSimpleLockUnlock(gStatusLock);
}

void statusLine()
{
    uint32_t dispatched = 0, unbound = 0, restamped = 0;
    pagani_sensors_hid_counts(&dispatched, &unbound, &restamped);
    SntText t;
    snt_start(&t, gLine, sizeof gLine);
    snt_str(&t, "[sensors] status");
    snt_kd(&t, "st", gStatus.stage);
    snt_kx(&t, "found", gStatus.found);
    snt_kx(&t, "streaming", gStatus.streaming);
    snt_kx(&t, "bound", gBound);
    snt_kd(&t, "ios", dispatched);
    snt_kd(&t, "unbound", unbound);
    snt_kd(&t, "restamped", restamped);
    snt_kd(&t, "dropped", gStatus.dropped);
    if (gRpcStart) {
        snt_kd(&t, "rpc", gStatus.rpc_stage);
        snt_kd(&t, "opens", gStatus.rpc_opens);
        snt_kd(&t, "missing", gStatus.rpc_missing);
    }
    trace(gLine);
}

// The event drivers attach asynchronously after each device's start: bound as they appear (hooks installed), given up after 2 min.
void bindAll(uint64_t now)
{
    for (unsigned kind = 0; kind < SNS_HID_KINDS; ++kind) {
        const uint32_t bit = 1u << kind;
        if (!(gPublished & bit) || (gBound & bit) || (gRefused & bit)) continue;
        gLine[0] = 0;
        const int rc = pagani_sensors_hid_bind(kind, gLine, sizeof gLine);
        if (rc > 0) {
            gBound |= bit;
            if (gLine[0]) trace(gLine);
            if (gStage >= 0 && gStage < PaganiSensorsStServices) gStage = PaganiSensorsStServices;
        } else if (rc < 0) {
            gRefused |= bit;
            if (gLine[0]) trace(gLine);
            fail(PaganiSensorsStServices, rc, "event driver hooks refused (see the line above)");
        }
    }
    const uint32_t waiting = gPublished & ~gBound & ~gRefused;
    if (waiting && now - gBindStart >= kBindGiveUpMs) {
        SntText t;
        gRefused |= waiting;
        snt_start(&t, gLine, sizeof gLine);
        snt_str(&t, "[sensors] FAIL st=-1 err=-60 no event driver after 120 s for");
        for (unsigned kind = 0; kind < SNS_HID_KINDS; ++kind)
            if (waiting & (1u << kind)) {snt_char(&t, ' '); snt_str(&t, kind == SNS_HID_ORIENT ? "orientation" : sns_kind_name(kind));}
        trace(gLine);
        gStage = -PaganiSensorsStServices;
        gError = PaganiSensorsErrTimeout;
    }
}

void readServer()
{
    PaganiAdspQrtrServer server;
    const int up = pagani_adsp_qrtr_server(gPort[SNS_CONN_LOOKUP], &server);
    sns_core_server(gCore, up, server.node, server.port, server.instance, server.generation);
}

void drain()
{
    uint32_t tail = __atomic_load_n(&gTail, __ATOMIC_RELAXED);
    const uint32_t head = __atomic_load_n(&gHead, __ATOMIC_ACQUIRE);
    while (tail != head) {
        const Slot &slot = gRing[tail % kRingSlots];
        sns_core_receive(gCore, slot.conn, slot.node, slot.port, slot.data, slot.length);
        ++tail;
        __atomic_store_n(&gTail, tail, __ATOMIC_RELEASE);
    }
}

void applyDemand()
{
    const uint32_t dirty = __atomic_exchange_n(&gDemandDirty, 0u, __ATOMIC_ACQ_REL);
    for (unsigned kind = 0; kind < PaganiSensorKinds; ++kind)
        if (dirty & (1u << kind)) sns_core_demand(gCore, kind, __atomic_load_n(&gDemand[kind], __ATOMIC_ACQUIRE));
    if (dirty & (1u << SNS_HID_ORIENT)) pagani_sensors_hid_orientation_replay();
}

void sensorsThread(void *, wait_result_t)
{
    thread_set_thread_name(current_thread(), "pagani-sensors");
    gBindStart = nowMs();
    uint64_t statusAt = gBindStart + kStatusMs;
    if (gCore) readServer();
    for (;;) {
        const uint64_t now = nowMs();
        const bool binding = (gPublished & ~gBound & ~gRefused) != 0;
        uint32_t idle = gCore ? sns_core_idle_ms(gCore) : 60000u;
        if (binding) {
            const uint32_t step = now - gBindStart < 10000 ? kBindFastMs : kBindSlowMs;
            if (idle > step) idle = step;
        }
        const uint32_t events = waitEvents(idle);
        if (binding) bindAll(nowMs());
        if (gCore) {
            if (events & kEvServer) readServer();
            drain();
            applyDemand();
            sns_core_tick(gCore);
        }
        snapshot();
        if (nowMs() >= statusAt) {
            statusAt = nowMs() + kStatusMs;
            if (gLog >= 1 && (gStatus.reports || gBound)) statusLine();
        }
    }
}

void copyType(char *to, const char *argument, const char *fallback)
{
    char value[SSC_TYPE_MAX] = {};
    const char *from = PE_parse_boot_argn(argument, value, sizeof value) && value[0] ? value : fallback;
    unsigned i = 0;
    for (; from[i] && i + 1 < SSC_TYPE_MAX; ++i) to[i] = from[i];
    to[i] = 0;
}

bool openPorts()
{
    static const PaganiAdspQrtrClient client = {onServer, onReceive};
    const PaganiAdspQrtrConfig config = {SSC_QMI_SERVICE, PAGANI_ADSP_QRTR_ANY, 0, 0};
    for (unsigned conn = 0; conn < SNS_CONNS; ++conn) {
        if (conn == SNS_CONN_LIST && !gConfig.list) continue;
        gPort[conn] = pagani_adsp_qrtr_open(&config, &client, reinterpret_cast<void *>(static_cast<uintptr_t>(conn)));
        if (gPort[conn]) continue;
        for (unsigned k = 0; k < conn; ++k) {
            pagani_adsp_qrtr_close(gPort[k]);
            gPort[k] = nullptr;
        }
        return false;
    }
    return true;
}
}  // namespace

extern "C" void pagani_sensors_start(void)
{
    uint32_t expected = 0;
    if (!__atomic_compare_exchange_n(&gStarted, &expected, 1u, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return;
    const unsigned value = argument("pagani-sensors", 0);
    if (!value) return;   // off: nothing allocated, registered or sent
    const unsigned mode = value & PaganiSensorsModeMask;
    if (value > 15 || !mode) {
        trace("[sensors] off: pagani-sensors takes a mode 1, 2 or 3, plus 4 (file service), plus 8 (orientation, with mode 3)");
        return;
    }
    const bool rpc = value & PaganiSensorsFileService, orient = (value & PaganiSensorsOrientation) && mode == 3;
    gMode = mode;
    gLog = argument("pagani-sensors-log", 1);
    gStatusLock = IOSimpleLockAlloc();
    gRing = static_cast<Slot *>(IOMallocData(sizeof(Slot) * kRingSlots));
    if (!gStatusLock || !gRing) {
        trace("[sensors] FAIL st=-1 err=-12 no memory");
        return;
    }
    // The manager's configuration.
    gConfig.mode = mode;
    gConfig.orient = orient;
    for (unsigned i = 0; i < 3; ++i) sfx_axes_identity(&gConfig.axes[i]);
    char axes[48] = {};
    const bool axesGiven = PE_parse_boot_argn("pagani-sensors-axes", axes, sizeof axes) && axes[0];
    const bool axesOk = !axesGiven || sfx_axes_parse(axes, &gConfig.axes[0], &gConfig.axes[1], &gConfig.axes[2]) == 0;
    copyType(gConfig.type[PaganiSensorAccel], "pagani-sensors-accel", "accel");
    copyType(gConfig.type[PaganiSensorGyro], "pagani-sensors-gyro", "gyro");
    copyType(gConfig.type[PaganiSensorMag], "pagani-sensors-mag", "mag");
    copyType(gConfig.type[PaganiSensorLight], "pagani-sensors-als", "ambient_light");
    copyType(gConfig.type[PaganiSensorProximity], "pagani-sensors-prox", "proximity");
    copyType(gConfig.type[PaganiSensorPressure], "pagani-sensors-pressure", "pressure");
    gConfig.als_permille = argument("pagani-sensors-als-scale", 1000);
    if (!gConfig.als_permille || gConfig.als_permille > 100000) gConfig.als_permille = 1000;
    gConfig.motion_hz = argument("pagani-sensors-motion-hz", 0);
    if (gConfig.motion_hz > 500) gConfig.motion_hz = 500;
    gConfig.prox_always = argument("pagani-sensors-prox-on", 0) == 1;
    gConfig.list = argument("pagani-sensors-list", mode == 2 ? 1 : 0) == 1;
    gConfig.log = gLog > 2 ? 2 : gLog;

    SntText t;
    char text[7];
    snt_start(&t, gLine, sizeof gLine);
    snt_str(&t, "[sensors] start");
    snt_kd(&t, "mode", mode);
    snt_kd(&t, "rpc", rpc);
    snt_kd(&t, "orient", orient);
    snt_str(&t, " axes=a");
    sfx_axes_text(&gConfig.axes[0], text);
    snt_str(&t, text);
    snt_str(&t, ",g");
    sfx_axes_text(&gConfig.axes[1], text);
    snt_str(&t, text);
    snt_str(&t, ",m");
    sfx_axes_text(&gConfig.axes[2], text);
    snt_str(&t, text);
    if (!axesOk) snt_str(&t, " (pagani-sensors-axes refused: identity)");
    snt_ks(&t, "als", gConfig.type[PaganiSensorLight]);
    snt_ks(&t, "prox", gConfig.type[PaganiSensorProximity]);
    snt_kd(&t, "scale", gConfig.als_permille);
    snt_kd(&t, "motion_hz", gConfig.motion_hz);
    snt_kd(&t, "prox_on", gConfig.prox_always);
    snt_kd(&t, "list", gConfig.list);
    snt_kd(&t, "log", gConfig.log);
    trace(gLine);

    // The iOS services, registered at boot whatever the hub does (MobileGestalt's ambient-light-sensor looks for the light service
    // once, early): no samples until the hub gives some.
    const int setup = pagani_sensors_hid_setup(demand, ((1u << PaganiSensorKinds) - 1) | (orient ? 1u << SNS_HID_ORIENT : 0u));
    if (setup) {
        fail(PaganiSensorsStServices, setup, "HID class or personalities");
        return;
    }
    for (unsigned kind = 0; kind < PaganiSensorPressure; ++kind) {
        const int rc = pagani_sensors_hid_publish(kind);
        if (!rc) {
            gPublished |= 1u << kind;
            continue;
        }
        snt_start(&t, gLine, sizeof gLine);
        snt_str(&t, "[sensors] FAIL st=-1");
        snt_kx(&t, "err", static_cast<uint32_t>(rc));
        snt_ks(&t, "sensor", sns_kind_name(kind));
        snt_str(&t, " service not started");
        trace(gLine);
        gError = rc;
    }
    gStage = gPublished ? PaganiSensorsStServices : -PaganiSensorsStServices;
    // The orientation service last and only with the accelerometer: its device carries the plug-in's user client from before its start
    // (SensorsHid.h: the service alone would crash backboardd at every boot).
    if (orient && (gPublished & (1u << PaganiSensorAccel))) {
        const int rc = pagani_sensors_hid_publish(SNS_HID_ORIENT);
        if (!rc) gPublished |= 1u << SNS_HID_ORIENT;
        else fail(PaganiSensorsStServices, rc, "orientation service not published");
    }
    snt_start(&t, gLine, sizeof gLine);
    snt_str(&t, "[sensors] services");
    snt_kx(&t, "published", gPublished);
    trace(gLine);
    if (!gPublished) return;

    // MobileGestalt's accelerometer and gyroscope: nodes found by name through IODeviceTree. Here, before this returns, or launchd's and
    // MobileGestalt's first looks (which are cached for the boot) miss them; only when the streams are asked for, never in the board's
    // static device tree.
    const unsigned nodes = sns_hid_nodes(mode, gPublished, argument("pagani-sensors-nodes", 1));
    if (nodes) {
        static const struct {unsigned bit; const char *name;} table[] = {{SNS_HID_NODE_ACCEL, "accel"}, {SNS_HID_NODE_GYRO, "gyro"}};
        snt_start(&t, gLine, sizeof gLine);
        snt_str(&t, "[sensors] nodes");
        for (const auto &node : table) {
            if (!(nodes & node.bit)) continue;
            snt_char(&t, ' ');
            snt_str(&t, node.name);
            snt_char(&t, '=');
            snt_dec(&t, pagani_sensors_hid_node(node.name));
        }
        trace(gLine);
    }

    // The FastRPC file service: on its own thread, independent of the router ports below (5 runs it with no sensor client at all).
    if (rpc) {
        gRpcStart = 1;
        gRpcResult = pagani_sensors_rpc_start(gConfig.log);
        if (gRpcResult) {
            snt_start(&t, gLine, sizeof gLine);
            snt_str(&t, "[sensors] rpc not started");
            snt_kd(&t, "err", gRpcResult);
            snt_str(&t, gRpcResult == -19 ? " (pagani-adsp=1?)" : gRpcResult == -22 ? " (pagani-sensors-rpc-pa)" : "");
            trace(gLine);
        }
    }

    // The sensor hub (mode 2 and 3): one port per connection of the manager.
    if (mode >= 2) {
        gCore = static_cast<SnsCore *>(IOMallocZero(sizeof(SnsCore)));
        if (!gCore) {
            fail(PaganiSensorsStPorts, PaganiSensorsErrNoMemory, "no memory for the manager");
        } else if (!openPorts()) {
            IOFree(gCore, sizeof(SnsCore));
            gCore = nullptr;
            PaganiAdspStatus adsp;
            const int off = pagani_adsp_status(&adsp) == PaganiAdspOff;
            fail(PaganiSensorsStPorts, PaganiSensorsErrOff,
                 off ? "PaganiAdsp is off (pagani-adsp=1?) or not linked: services without samples"
                     : "no IPC router port left: services without samples");
        } else {
            sns_core_init(gCore, &kOps, &gConfig);
            gStage = PaganiSensorsStPorts;
            snt_start(&t, gLine, sizeof gLine);
            snt_str(&t, "[sensors] ports");
            snt_kx(&t, "first", pagani_adsp_qrtr_port_number(gPort[0]));
            snt_kd(&t, "n", gConfig.list ? SNS_CONNS : SNS_CONNS - 1);
            snt_str(&t, " following service 0x190 (any instance)");
            trace(gLine);
        }
    }
    snapshot();
    thread_t thread = nullptr;
    if (kernel_thread_start(sensorsThread, nullptr, &thread) != KERN_SUCCESS) {
        fail(PaganiSensorsStServices, -7, "no thread");
        return;
    }
    thread_deallocate(thread);
}

extern "C" int pagani_sensors_status(PaganiSensorsStatus *out)
{
    if (!out || !gStatusLock) return -19;
    IOSimpleLockLock(gStatusLock);
    *out = gStatus;
    IOSimpleLockUnlock(gStatusLock);
    return 0;
}
