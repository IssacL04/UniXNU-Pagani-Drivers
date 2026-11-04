// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026, UniXNU contributors. All rights reserved.
// Host tests of SensorsHid.c, what iOS reads: each report descriptor walked item by item by a small HID parser written here (one
// top-level application collection with the kind's usage pair, report 1, the two Sensor page property features 0x30e and 0x31b of 32
// bits, report 2 with the x/y/z input elements of the motion sensors and no input item for the others, nothing after the end), the personalities (balanced XML; one dictionary per kind; Placement only on proximity;
// never Transport "SPU"; written to a file for the runner's plistlib check), the events each sample becomes, iOS's properties as
// demand, the orientation service (its descriptor, input reports, the plug-in's user client methods) and the MobileGestalt node plan.
#include "check.h"
#include "../SensorsHid.h"

typedef struct {
    uint32_t page, usage;            /* the top-level collection's */
    unsigned collections, features, inputs, outputs;
    uint32_t feature_usages[4], input_usages[8];
    unsigned feature_usage_count, input_usage_count, padding;
    uint32_t feature_id, feature_size, feature_count, input_id, input_size, input_count;
    int32_t feature_min, feature_max, feature_exponent, input_min, input_max, input_exponent;
} Walk;

static int32_t value_of(const uint8_t *p, unsigned n, int is_signed)
{
    uint32_t v = 0;
    for (unsigned i = 0; i < n; ++i) v |= (uint32_t)p[i] << (8 * i);
    if (is_signed && n && n < 4 && (v >> (8 * n - 1)) & 1) v |= ~0u << (8 * n);
    return (int32_t)v;
}

static void walk(const uint8_t *d, uint32_t n, Walk *w)
{
    uint32_t page = 0, local_usages[8], report_size = 0, report_count = 0, report_id = 0, range_min = 0;
    int32_t logical_min = 0, logical_max = 0, unit_exponent = 0;
    unsigned local = 0, depth = 0;
    memset(w, 0, sizeof *w);
    for (uint32_t at = 0; at < n;) {
        const uint8_t prefix = d[at];
        CHECK(prefix != 0xfe);                       /* no long items */
        const unsigned size = (prefix & 3) == 3 ? 4 : prefix & 3, type = (prefix >> 2) & 3, tag = prefix >> 4;
        CHECK(at + 1 + size <= n);
        const uint8_t *data = d + at + 1;
        const uint32_t u = (uint32_t)value_of(data, size, 0);
        if (type == 0) {                             /* main */
            if (tag == 0xa) {                        /* collection */
                CHECK(u == 1 || depth);              /* the top level is an application collection */
                if (!depth) {
                    CHECK(local == 1);
                    w->page = local_usages[0] >> 16 ? local_usages[0] >> 16 : page;
                    w->usage = local_usages[0] & 0xffff;
                }
                depth++;
                w->collections++;
            } else if (tag == 0xc) {                 /* end collection */
                CHECK(depth > 0);
                depth--;
            } else if (tag == 0xb) {                 /* feature */
                w->features++;
                CHECK(u == 2 && depth == 1);         /* data, variable, absolute */
                for (unsigned i = 0; i < local && w->feature_usage_count < 4; ++i) w->feature_usages[w->feature_usage_count++] = local_usages[i];
                w->feature_id = report_id; w->feature_size = report_size; w->feature_count = report_count;
                w->feature_min = logical_min; w->feature_max = logical_max; w->feature_exponent = unit_exponent;
            } else if (tag == 0x8) {
                CHECK(depth == 1);
                if (u == 3) {                        /* constant: padding */
                    w->padding += report_size * report_count;
                } else {
                    CHECK(u == 2);
                    w->inputs++;
                    for (unsigned i = 0; i < local && w->input_usage_count < 8; ++i) w->input_usages[w->input_usage_count++] = local_usages[i];
                    w->input_id = report_id; w->input_size = report_size; w->input_count = report_count;
                    w->input_min = logical_min; w->input_max = logical_max; w->input_exponent = unit_exponent;
                }
            } else if (tag == 0x9) {
                w->outputs++;
            } else {
                CHECK(0);
            }
            local = 0;
        } else if (type == 1) {                      /* global */
            switch (tag) {
            case 0x0: page = u; break;
            case 0x1: logical_min = value_of(data, size, 1); break;
            case 0x2: logical_max = value_of(data, size, 1); break;
            case 0x5: unit_exponent = size == 1 && (u & 8) ? (int32_t)u - 16 : value_of(data, size, 1); break;   /* a nibble */
            case 0x7: report_size = u; break;
            case 0x8: report_id = u; break;
            case 0x9: report_count = u; break;
            default: CHECK(0);
            }
        } else if (type == 2) {                      /* local */
            if (tag == 1) {                          /* a usage range */
                range_min = u;
            } else if (tag == 2) {
                CHECK(u >= range_min && u - range_min < 8 && !local);
                for (uint32_t usage = range_min; usage <= u; ++usage) local_usages[local++] = page << 16 | usage;
            } else {
                CHECK(tag == 0 && local < 8);        /* else usages only */
                local_usages[local++] = size == 4 ? u : (page << 16 | u);
            }
        } else {
            CHECK(0);
        }
        at += 1 + size;
        if (!depth && w->collections) CHECK(at == n);   /* nothing after the top-level collection */
    }
    CHECK(!depth);
}

static void descriptors(void)
{
    static const uint16_t pairs[PaganiSensorKinds][2] = {{0xff00, 3}, {0xff00, 9}, {0xff00, 0x0a}, {0xff00, 4}, {0xff00, 8}, {0x20, 0x31}};
    for (unsigned kind = 0; kind < PaganiSensorKinds; ++kind) {
        uint32_t n = 0;
        const uint8_t *d = sns_hid_descriptor(kind, &n);
        Walk w;
        CHECK(d && n > 20);
        walk(d, n, &w);
        CHECK(w.page == pairs[kind][0] && w.usage == pairs[kind][1]);
        CHECK(sns_hid_kind(kind)->page == pairs[kind][0] && sns_hid_kind(kind)->usage == pairs[kind][1]);
        CHECK(w.collections == 1 && w.features == 1 && w.outputs == 0 && w.feature_id == 1);
        CHECK(w.feature_usage_count == 2 && w.feature_usages[0] == 0x20030e && w.feature_usages[1] == 0x20031b);
        CHECK(w.feature_size == 32 && w.feature_count == 2 && w.feature_min == 0 && w.feature_max == 0x7fffffff && w.feature_exponent == 0);
        static const uint32_t axes[3] = {0x200453, 0x200457, 0x200485};   /* accelerometer, gyroscope, compass x; y and z follow */
        if (kind <= PaganiSensorMag) {
            CHECK(w.inputs == 1 && w.input_id == 2 && w.input_usage_count == 3 && w.input_size == 16 && w.input_count == 3);
            for (unsigned i = 0; i < 3; ++i) CHECK(w.input_usages[i] == axes[kind] + i);
            CHECK(w.input_min == -32767 && w.input_max == 32767 && w.input_exponent == -3);
        } else {
            CHECK(w.inputs == 0);
        }
    }
    /* the orientation service: 0x20/0x8a, no feature, report 1 with one bit per state on the usages CoreMotion's event handler wants
     * (0xff0c 0x64..0x6a: 100 + state), padded to a byte */
    uint32_t n = 0;
    Walk w;
    const uint8_t *d = sns_hid_descriptor(SNS_HID_ORIENT, &n);
    CHECK(d && n > 20);
    walk(d, n, &w);
    CHECK(w.page == 0x20 && w.usage == 0x8a && sns_hid_kind(SNS_HID_ORIENT)->page == 0x20 && sns_hid_kind(SNS_HID_ORIENT)->usage == 0x8a);
    CHECK(w.features == 0 && w.outputs == 0 && w.inputs == 1 && w.input_id == 1 && w.input_size == 1 && w.input_count == 7 && w.padding == 1);
    CHECK(w.input_min == 0 && w.input_max == 1 && w.input_usage_count == 7);
    for (unsigned i = 0; i < 7; ++i) CHECK(w.input_usages[i] == 0xff0c0064u + i);
    n = 7;
    CHECK(!sns_hid_descriptor(SNS_HID_KINDS, &n) && n == 0 && !sns_hid_kind(SNS_HID_KINDS));
}

static void personalities(const char *path)
{
    static char xml[8192];
    const uint32_t all = (1u << PaganiSensorKinds) - 1;
    const uint32_t n = sns_hid_personalities(all, xml, sizeof xml);
    CHECK(n > 0 && n == strlen(xml) && !strncmp(xml, "<array>", 7) && !strcmp(xml + n - 8, "</array>"));
    /* balanced tags */
    char stack[32][32];
    unsigned depth = 0, dicts = 0;
    for (const char *p = xml; (p = strchr(p, '<'));) {
        const char *end = strchr(p, '>');
        CHECK(end && end - p < 32);
        char tag[32];
        memcpy(tag, p + 1, (size_t)(end - p - 1));
        tag[end - p - 1] = 0;
        if (tag[0] == '/') {
            CHECK(depth && !strcmp(stack[depth - 1], tag + 1));
            depth--;
        } else if (tag[strlen(tag) - 1] != '/') {
            CHECK(depth < 32);
            strcpy(stack[depth++], tag);
            if (!strcmp(tag, "dict") && depth == 2) dicts++;
        }
        p = end;
    }
    CHECK(!depth && dicts == PaganiSensorKinds && !strstr(xml, "SPU") && !strstr(xml, "Transport"));
    for (unsigned kind = 0; kind < PaganiSensorKinds; ++kind) CHECK(strstr(xml, sns_hid_kind(kind)->product));
    const char *prox = strstr(xml, "Pagani Proximity"), *placement = strstr(xml, "<key>Placement</key><integer>1</integer>");
    CHECK(prox && placement && placement > prox && !strstr(placement + 1, "<key>Placement</key>"));
    CHECK(strstr(xml, "<key>DeviceUsagePage</key><integer>65280</integer><key>DeviceUsage</key><integer>4</integer>"));
    CHECK(strstr(xml, "<key>PrimaryUsagePage</key><integer>32</integer><key>PrimaryUsage</key><integer>49</integer>"));
    /* one kind; too small a buffer gives nothing */
    char one[1024];
    CHECK(sns_hid_personalities(1u << PaganiSensorPressure, one, sizeof one) > 0 && strstr(one, "Pagani Pressure") &&
          !strstr(one, "Pagani Proximity"));
    CHECK(sns_hid_personalities(all, one, 100) == 0 && one[0] == 0);
    if (path) {
        FILE *f = fopen(path, "w");
        CHECK(f);
        fputs(xml, f);
        fclose(f);
    }
}

/* The orientation service is never among the sensors' personalities: asking for it is asking for the trap. */
static void orientation_service(void)
{
    static char xml[8192];
    const uint32_t sensors = (1u << PaganiSensorKinds) - 1;
    CHECK(sns_hid_personalities(sensors, xml, sizeof xml) > 0 && !strstr(xml, "Orientation") && !strstr(xml, "<integer>138</integer>"));
    CHECK(sns_hid_personalities(sensors | 1u << SNS_HID_ORIENT, xml, sizeof xml) > 0 && strstr(xml, "Pagani Orientation") &&
          strstr(xml, "<key>PrimaryUsagePage</key><integer>32</integer><key>PrimaryUsage</key><integer>138</integer>"));
    uint8_t r[2];
    for (int state = 0; state < 7; ++state) CHECK(sns_hid_orient_report(state, r) == 2 && r[0] == 1 && r[1] == 1u << state);
    CHECK(sns_hid_orient_report(-1, r) == 0 && sns_hid_orient_report(7, r) == 0);
    /* AppleSPUHIDDeviceClass: Open is selector 4, SendCommand selector 6 with the command as the only scalar */
    uint32_t command = 0;
    CHECK(sns_hid_spu_method(SNS_HID_SPU_OPEN, 0, 0, 0) == 0 && sns_hid_spu_method(SNS_HID_SPU_CLOSE, 0, 0, 0) == 0);
    CHECK(sns_hid_spu_method(SNS_HID_SPU_COMMAND, 1, SNS_HID_SPU_CONFIG, &command) == 0 && command == 0x20);
    CHECK(sns_hid_spu_method(SNS_HID_SPU_COMMAND, 0, 0, &command) == -1);
    CHECK(sns_hid_spu_method(SNS_HID_SPU_COMMAND, 1, 1ull << 32, &command) == -1);
    for (uint32_t selector = 0; selector < 16; ++selector)
        if (selector < 4 || selector > 6) CHECK(sns_hid_spu_method(selector, 1, 1, 0) == -1);
    CHECK(SNS_HID_SPU_CLIENT == ('a' << 24 | 's' << 16 | 'h' << 8 | 'd'));
}

/* MobileGestalt's nodes: only with the streams asked for, only for services that are there, and the boot argument can refuse them. */
static void nodes(void)
{
    const uint32_t both = 1u << PaganiSensorAccel | 1u << PaganiSensorGyro;
    for (unsigned mode = 0; mode < 3; ++mode) CHECK(sns_hid_nodes(mode, both, 1) == 0);
    CHECK(sns_hid_nodes(3, both, 1) == (SNS_HID_NODE_ACCEL | SNS_HID_NODE_GYRO));
    CHECK(sns_hid_nodes(3, both, 0) == 0);
    CHECK(sns_hid_nodes(3, 1u << PaganiSensorGyro, 1) == SNS_HID_NODE_GYRO);
    CHECK(sns_hid_nodes(3, 1u << PaganiSensorAccel, 1) == SNS_HID_NODE_ACCEL);
    CHECK(sns_hid_nodes(3, 1u << PaganiSensorLight, 1) == 0);
}

static void events(void)
{
    SnsSample s;
    SnsHidEvent e;
    memset(&s, 0, sizeof s);
    s.kind = PaganiSensorAccel;
    s.value[0] = 1; s.value[1] = -2; s.value[2] = -65536;
    CHECK(sns_hid_event(&s, 7, &e) == 0 && e.type == 13 && e.x == 1 && e.y == -2 && e.z == -65536 && e.sequence == 7);
    s.kind = PaganiSensorGyro;
    CHECK(sns_hid_event(&s, 8, &e) == 0 && e.type == 20 && e.z == -65536);
    s.kind = PaganiSensorMag;
    CHECK(sns_hid_event(&s, 9, &e) == 0 && e.type == 21 && e.y == -2);
    s.kind = PaganiSensorLight;
    s.value[0] = 312;
    CHECK(sns_hid_event(&s, 1, &e) == 0 && e.type == 12 && e.level == 312 && !e.x);
    s.value[0] = -3;
    CHECK(sns_hid_event(&s, 1, &e) == 0 && e.level == 0);
    s.kind = PaganiSensorProximity;
    s.value[0] = 1; s.value[1] = 4000;
    CHECK(sns_hid_event(&s, 1, &e) == 0 && e.type == 14 && e.mask == 0x40 && e.level == 4000);
    s.value[0] = 0;
    CHECK(sns_hid_event(&s, 1, &e) == 0 && e.mask == 0);
    s.kind = PaganiSensorPressure;
    s.value[0] = 6640435;
    CHECK(sns_hid_event(&s, 2, &e) == 0 && e.type == 31 && e.x == 6640435 && e.sequence == 2);
    s.kind = PaganiSensorKinds;
    CHECK(sns_hid_event(&s, 1, &e) == -1);
}

static void demand(void)
{
    uint32_t d = 99;
    CHECK(sns_hid_demand(PaganiSensorAccel, SNS_HID_KEY_REPORT_INTERVAL, 10000, &d) == 1 && d == 10000);
    CHECK(sns_hid_demand(PaganiSensorGyro, SNS_HID_KEY_REPORT_INTERVAL, 0, &d) == 1 && d == 0);
    CHECK(sns_hid_demand(PaganiSensorMag, SNS_HID_KEY_REPORT_INTERVAL, 1ull << 40, &d) == 1 && d == 0xffffffffu);
    CHECK(sns_hid_demand(PaganiSensorPressure, SNS_HID_KEY_REPORT_INTERVAL, 1000000, &d) == 1 && d == 1000000);
    CHECK(sns_hid_demand(PaganiSensorAccel, SNS_HID_KEY_DETECTION_MODE, 2, &d) == 0);
    CHECK(sns_hid_demand(PaganiSensorProximity, SNS_HID_KEY_DETECTION_MODE, 2, &d) == 1 && d == 1);
    CHECK(sns_hid_demand(PaganiSensorProximity, SNS_HID_KEY_DETECTION_MODE, 0, &d) == 1 && d == 0);
    CHECK(sns_hid_demand(PaganiSensorProximity, SNS_HID_KEY_REPORT_INTERVAL, 5, &d) == 0);
    CHECK(sns_hid_demand(PaganiSensorLight, SNS_HID_KEY_REPORT_INTERVAL, 5, &d) == 0);
}

int main(int argc, char **argv)
{
    descriptors();
    personalities(argc > 1 ? argv[1] : 0);
    orientation_service();
    nodes();
    events();
    demand();
    printf("PASS HID: seven descriptors walked, the personalities, events, demand, the orientation service, the nodes: %u checks\n", checks);
    return 0;
}
