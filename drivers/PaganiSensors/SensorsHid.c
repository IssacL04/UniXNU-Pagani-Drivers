/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* What iOS reads per sensor: see SensorsHid.h. */
#include "SensorsHid.h"

static const SnsHidKind kKinds[SNS_HID_KINDS] = {
    {0xff00, 0x03, "Pagani Accelerometer", 0},
    {0xff00, 0x09, "Pagani Gyroscope", 0},
    {0xff00, 0x0a, "Pagani Compass", 0},
    {0xff00, 0x04, "Pagani Ambient Light", 0},
    {0xff00, 0x08, "Pagani Proximity", 1},   /* backboardd ignores a proximity service whose Placement is not 1 (top) */
    {0x0020, 0x31, "Pagani Pressure", 0},
    {0x0020, 0x8a, "Pagani Orientation", 0},
};

const SnsHidKind *sns_hid_kind(unsigned kind)
{
    return kind < SNS_HID_KINDS ? &kKinds[kind] : 0;
}

/* Report 1: two 32-bit feature values on the Sensor page, 0x30e report interval and 0x31b report latency (us): IOHIDEventDriver's sensor
 * property parser takes them (so the driver starts) and writes iOS's report interval there. */
#define PROPERTIES \
    0x85, 0x01, 0x05, 0x20, 0x0a, 0x0e, 0x03, 0x0a, 0x1b, 0x03, 0x15, 0x00, 0x27, 0xff, 0xff, 0xff, 0x7f, 0x55, 0x00, 0x75, 0x20, \
    0x95, 0x02, 0xb1, 0x02
/* Report 2 of the motion sensors: x, y, z on the Sensor page (the original driver's accelerometer 0x453.., gyroscope 0x457..,
 * compass 0x485.. parsers know them; 16 bits, exponent -3). Never sent: the samples are events built here, this only gives the
 * driver the elements a real HID motion sensor has. */
#define AXES(x, y, z) \
    0x85, 0x02, 0x05, 0x20, 0x0a, (x), 0x04, 0x0a, (y), 0x04, 0x0a, (z), 0x04, 0x16, 0x01, 0x80, 0x26, 0xff, 0x7f, 0x55, 0x0d, 0x75, \
    0x10, 0x95, 0x03, 0x81, 0x02
static const uint8_t kAccel[] = {0x06, 0x00, 0xff, 0x09, 0x03, 0xa1, 0x01, PROPERTIES, AXES(0x53, 0x54, 0x55), 0xc0};
static const uint8_t kGyro[] = {0x06, 0x00, 0xff, 0x09, 0x09, 0xa1, 0x01, PROPERTIES, AXES(0x57, 0x58, 0x59), 0xc0};
static const uint8_t kMag[] = {0x06, 0x00, 0xff, 0x09, 0x0a, 0xa1, 0x01, PROPERTIES, AXES(0x85, 0x86, 0x87), 0xc0};
static const uint8_t kLight[] = {0x06, 0x00, 0xff, 0x09, 0x04, 0xa1, 0x01, PROPERTIES, 0xc0};
static const uint8_t kProx[] = {0x06, 0x00, 0xff, 0x09, 0x08, 0xa1, 0x01, PROPERTIES, 0xc0};
static const uint8_t kPressure[] = {0x05, 0x20, 0x09, 0x31, 0xa1, 0x01, PROPERTIES, 0xc0};
/* Report 1: one bit per state on the vendor usages 0x64..0x6a, one padding bit: the original driver's device orientation parser and
 * report handler (a bit going from 0 to 1 is an orientation event with that usage). */
static const uint8_t kOrientation[] = {0x05, 0x20, 0x09, 0x8a, 0xa1, 0x01, 0x85, 0x01, 0x06, 0x0c, 0xff, 0x19, 0x64, 0x29, 0x6a, 0x15, 0x00,
                                       0x25, 0x01, 0x75, 0x01, 0x95, 0x07, 0x81, 0x02, 0x75, 0x01, 0x95, 0x01, 0x81, 0x03, 0xc0};
#undef AXES
#undef PROPERTIES

const uint8_t *sns_hid_descriptor(unsigned kind, uint32_t *length)
{
    static const uint8_t *const tables[SNS_HID_KINDS] = {kAccel, kGyro, kMag, kLight, kProx, kPressure, kOrientation};
    static const uint32_t sizes[SNS_HID_KINDS] = {sizeof kAccel, sizeof kGyro, sizeof kMag, sizeof kLight, sizeof kProx, sizeof kPressure,
                                                  sizeof kOrientation};
    if (kind >= SNS_HID_KINDS) {
        if (length) *length = 0;
        return 0;
    }
    if (length) *length = sizes[kind];
    return tables[kind];
}

/* ---- the personalities, as XML text ---- */
typedef struct {
    char *out;
    uint32_t cap, len;
    int overflow;
} Xml;

static void put(Xml *x, const char *s)
{
    for (uint32_t i = 0; s[i]; ++i) {
        if (x->len + 1 >= x->cap) {x->overflow = 1; return;}
        x->out[x->len++] = s[i];
    }
}
static void put_uint(Xml *x, uint32_t v)
{
    char digits[11];
    unsigned n = 0;
    do {
        digits[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    char text[12];
    for (unsigned i = 0; i < n; ++i) text[i] = digits[n - 1 - i];
    text[n] = 0;
    put(x, text);
}
static void key_int(Xml *x, const char *key, uint32_t value)
{
    put(x, "<key>");
    put(x, key);
    put(x, "</key><integer>");
    put_uint(x, value);
    put(x, "</integer>");
}

uint32_t sns_hid_personalities(uint32_t mask, char *out, uint32_t capacity)
{
    Xml x = {out, capacity, 0, 0};
    if (!out || !capacity) return 0;
    put(&x, "<array>");
    for (unsigned kind = 0; kind < SNS_HID_KINDS; ++kind) {
        if (!(mask & (1u << kind))) continue;
        const SnsHidKind *k = &kKinds[kind];
        put(&x, "<dict><key>IOClass</key><string>IOHIDEventDriver</string>"
                "<key>IOProviderClass</key><string>IOHIDInterface</string>"
                "<key>CFBundleIdentifier</key><string>com.apple.iokit.IOHIDFamily</string>"
                "<key>HIDDefaultBehavior</key><string></string>");
        key_int(&x, "IOProbeScore", 6000);   /* before any generic event driver personality of the same usage */
        put(&x, "<key>IOPropertyMatch</key><dict><key>Product</key><string>");
        put(&x, k->product);
        put(&x, "</string></dict><key>DeviceUsagePairs</key><array><dict>");
        key_int(&x, "DeviceUsagePage", k->page);
        key_int(&x, "DeviceUsage", k->usage);
        put(&x, "</dict></array>");
        key_int(&x, "PrimaryUsagePage", k->page);
        key_int(&x, "PrimaryUsage", k->usage);
        put(&x, "<key>Built-In</key><true/>");
        if (k->placement) key_int(&x, "Placement", k->placement);
        put(&x, "</dict>");
    }
    put(&x, "</array>");
    if (x.overflow) {
        out[0] = 0;
        return 0;
    }
    out[x.len] = 0;
    return x.len;
}

/* ---- events and demand ---- */
int sns_hid_event(const SnsSample *s, uint32_t sequence, SnsHidEvent *out)
{
    SnsHidEvent e = {0, 0, 0, 0, 0, 0, sequence};
    switch (s->kind) {
    case PaganiSensorAccel: e.type = SNS_HID_EVENT_ACCELEROMETER; break;
    case PaganiSensorGyro: e.type = SNS_HID_EVENT_GYRO; break;
    case PaganiSensorMag: e.type = SNS_HID_EVENT_COMPASS; break;
    case PaganiSensorLight:
        e.type = SNS_HID_EVENT_AMBIENT_LIGHT;
        e.level = s->value[0] > 0 ? (uint32_t)s->value[0] : 0;
        *out = e;
        return 0;
    case PaganiSensorProximity:
        e.type = SNS_HID_EVENT_PROXIMITY;
        e.mask = s->value[0] ? SNS_HID_PROXIMITY_NEAR : 0;
        e.level = s->value[1] > 0 ? (uint32_t)s->value[1] : 0;
        *out = e;
        return 0;
    case PaganiSensorPressure:
        e.type = SNS_HID_EVENT_PRESSURE;
        e.x = s->value[0];
        *out = e;
        return 0;
    default:
        return -1;
    }
    e.x = s->value[0];
    e.y = s->value[1];
    e.z = s->value[2];
    *out = e;
    return 0;
}

int sns_hid_demand(unsigned kind, unsigned key, uint64_t value, uint32_t *demand)
{
    switch (kind) {
    case PaganiSensorAccel:
    case PaganiSensorGyro:
    case PaganiSensorMag:
    case PaganiSensorPressure:
        if (key != SNS_HID_KEY_REPORT_INTERVAL) return 0;
        *demand = value > 0xffffffffu ? 0xffffffffu : (uint32_t)value;
        return 1;
    case PaganiSensorProximity:
        if (key != SNS_HID_KEY_DETECTION_MODE) return 0;
        *demand = value ? 1u : 0u;
        return 1;
    default:
        return 0;   /* ambient light: always on */
    }
}

uint32_t sns_hid_orient_report(int state, uint8_t out[2])
{
    if (state < 0 || state > 6) return 0;
    out[0] = 1;
    out[1] = (uint8_t)(1u << state);
    return 2;
}

int sns_hid_spu_method(uint32_t selector, uint32_t scalars, uint64_t first_scalar, uint32_t *command)
{
    switch (selector) {
    case SNS_HID_SPU_OPEN:
    case SNS_HID_SPU_CLOSE:
        return 0;
    case SNS_HID_SPU_COMMAND:
        if (scalars < 1 || first_scalar > 0xffffffffu) return -1;
        if (command) *command = (uint32_t)first_scalar;
        return 0;
    default:
        return -1;
    }
}

unsigned sns_hid_nodes(unsigned mode, uint32_t published, unsigned wanted)
{
    unsigned nodes = 0;
    if (mode != 3 || !wanted) return 0;
    if (published & (1u << PaganiSensorAccel)) nodes |= SNS_HID_NODE_ACCEL;
    if (published & (1u << PaganiSensorGyro)) nodes |= SNS_HID_NODE_GYRO;
    return nodes;
}
