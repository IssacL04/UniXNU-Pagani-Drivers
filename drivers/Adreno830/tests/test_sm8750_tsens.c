/* SPDX-License-Identifier: MIT */
/* host-test: platform/sm8750/Sm8750Tsens.c
 * Host model of the tsens2 TM block: eight TM_Sn_STATUS words at 0xA0 + 4n (n = 1..8, the gpuss sensors), each with
 * VALID in bit 21 and a signed 12-bit deci-Celsius value. The model counts every read, refuses any offset outside the
 * eight status words (the driver only reads those), can hold VALID low for a number of reads (the controller update
 * window), and checks the zone/cap rules of ARCHITECTURE 4.16 against temperature traces. */
#include "../platform/sm8750/Sm8750Tsens.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond, ...)                                                                                       \
    do {                                                                                                       \
        if (!(cond)) {                                                                                         \
            fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond);                                    \
            fprintf(stderr, __VA_ARGS__);                                                                      \
            fputc('\n', stderr);                                                                               \
            abort();                                                                                           \
        }                                                                                                      \
    } while (0)

typedef struct {
    uint32_t status[9];          /* index = sensor number; [0] unused (sensor 0 is not a GPU sensor) */
    unsigned invalid_reads[9];   /* this many reads of the sensor return VALID clear first */
    unsigned reads, bad_offsets;
} Model;

static uint32_t model_read(void *context, uint32_t offset)
{
    Model *m = (Model *)context;
    ++m->reads;
    if (offset < 0xa0 + 4 || offset > 0xa0 + 4 * 8 || (offset & 3)) {
        ++m->bad_offsets;
        return 0xdeadbeefu;
    }
    const unsigned n = (offset - 0xa0) / 4;
    if (m->invalid_reads[n]) {
        --m->invalid_reads[n];
        return m->status[n] & ~(1u << 21);
    }
    return m->status[n];
}

static uint32_t encode(int deci, int valid)
{
    return ((uint32_t)deci & 0xfffu) | (valid ? 1u << 21 : 0) | 0x0c000000u;   /* junk above bit 21 must be ignored */
}

static void set_all(Model *m, int deci)
{
    for (unsigned n = 1; n <= 8; ++n)
        m->status[n] = encode(deci, 1);
}

static unsigned checks;
#define EXPECT(cond, ...) do { ++checks; CHECK(cond, __VA_ARGS__); } while (0)

static void test_decode(void)
{
    int32_t deci = 1234;
    EXPECT(Sm8750TsensDecode(encode(452, 1), &deci) == 1 && deci == 452, "45.2 C decodes, got %d", deci);
    EXPECT(Sm8750TsensDecode(encode(-105, 1), &deci) == 1 && deci == -105, "-10.5 C sign-extends, got %d", deci);
    EXPECT(Sm8750TsensDecode(0x00200800u, &deci) == 1 && deci == -2048, "most negative value, got %d", deci);
    EXPECT(Sm8750TsensDecode(0x002007ffu, &deci) == 1 && deci == 2047, "most positive value, got %d", deci);
    EXPECT(Sm8750TsensDecode(encode(800, 0), &deci) == 0 && deci == 0, "VALID clear is no reading");
    EXPECT(Sm8750TsensDecode(encode(800, 1), NULL) == 1, "a null output is allowed");
}

static void test_read(void)
{
    Model m;
    memset(&m, 0, sizeof m);
    Sm8750TsensIo io = {&m, model_read};
    Sm8750TsensSample s;
    set_all(&m, 400);
    m.status[5] = encode(731, 1);
    m.status[2] = encode(-30, 1);
    EXPECT(Sm8750TsensReadGpu(&io, &s) == Sm8750TsensOk, "all valid");
    EXPECT(s.valid == 0xff && s.max_deci == 731 && s.hottest == 5, "max %d at %u valid %x", s.max_deci, s.hottest, s.valid);
    EXPECT(s.deci[1] == -30 && s.deci[4] == 731 && s.reads == 8 && m.bad_offsets == 0, "per-sensor values, 8 reads");
    for (unsigned n = 1; n <= 8; ++n)
        EXPECT(s.raw[n - 1] == m.status[n], "raw word of sensor %u", n);

    /* VALID low for two reads on sensor 3 (update window): retried, still counted. */
    memset(&m, 0, sizeof m);
    set_all(&m, 500);
    m.invalid_reads[3] = 2;
    EXPECT(Sm8750TsensReadGpu(&io, &s) == Sm8750TsensOk && s.valid == 0xff && s.reads == 10, "retry: %u reads", s.reads);
    /* VALID low for longer than the retries: that sensor is skipped, the others count. */
    memset(&m, 0, sizeof m);
    set_all(&m, 500);
    m.status[8] = encode(990, 1);
    m.invalid_reads[8] = 5;
    EXPECT(Sm8750TsensReadGpu(&io, &s) == Sm8750TsensOk && s.valid == 0x7f && s.max_deci == 500 && s.hottest == 1,
           "invalid hottest sensor ignored: valid %x max %d at %u", s.valid, s.max_deci, s.hottest);
    EXPECT(s.reads == 7 + 3 && s.deci[7] == 0, "three tries on the invalid sensor");
    /* No sensor valid. */
    memset(&m, 0, sizeof m);
    for (unsigned n = 1; n <= 8; ++n)
        m.status[n] = encode(300, 0);
    EXPECT(Sm8750TsensReadGpu(&io, &s) == Sm8750TsensNoData && s.valid == 0 && s.hottest == 0 && s.reads == 24,
           "no data after 3 tries each");
    /* Hottest negative: max must not start at 0. */
    memset(&m, 0, sizeof m);
    set_all(&m, -200);
    m.status[6] = encode(-150, 1);
    EXPECT(Sm8750TsensReadGpu(&io, &s) == Sm8750TsensOk && s.max_deci == -150 && s.hottest == 6, "cold: %d", s.max_deci);
    /* Unmapped window or missing callback. */
    EXPECT(Sm8750TsensReadGpu(NULL, &s) == Sm8750TsensArgument && s.valid == 0, "no window");
    Sm8750TsensIo broken = {&m, NULL};
    EXPECT(Sm8750TsensReadGpu(&broken, &s) == Sm8750TsensArgument, "no read callback");
    EXPECT(Sm8750TsensReadGpu(&io, NULL) == Sm8750TsensArgument, "no output");
}

/* Feed one reading at `deci` (or none) and return the cap. */
static uint32_t feed(Sm8750Thermal *t, Model *m, const Sm8750TsensIo *io, int deci, int valid)
{
    Sm8750TsensSample s;
    for (unsigned n = 1; n <= 8; ++n)
        m->status[n] = encode(n == 4 ? deci : deci - 60, valid);
    const int r = Sm8750TsensReadGpu(io, &s);
    return Sm8750ThermalUpdate(t, r, &s);
}

static void test_zones(void)
{
    Model m;
    memset(&m, 0, sizeof m);
    Sm8750TsensIo io = {&m, model_read};
    Sm8750Thermal t;
    Sm8750ThermalInit(&t);
    EXPECT(t.zone == Sm8750ThermalUnknown && Sm8750ThermalCapKhz(&t) == 607000, "before any reading: 607 MHz cap");
    EXPECT(feed(&t, &m, &io, 450, 1) == 0 && t.zone == Sm8750ThermalNormal, "45 C: no cap");
    EXPECT(feed(&t, &m, &io, 849, 1) == 0, "84.9 C: no cap");
    EXPECT(feed(&t, &m, &io, 850, 1) == 607000 && t.zone == Sm8750ThermalHot, "85.0 C: 607 MHz");
    EXPECT(feed(&t, &m, &io, 820, 1) == 607000, "82 C: still hot (hysteresis)");
    EXPECT(feed(&t, &m, &io, 800, 1) == 607000, "80.0 C: still hot");
    EXPECT(feed(&t, &m, &io, 799, 1) == 0 && t.zone == Sm8750ThermalNormal, "79.9 C: normal again");
    EXPECT(feed(&t, &m, &io, 1051, 1) == 389000 && t.zone == Sm8750ThermalCritical, "105.1 C: 389 MHz straight from normal");
    EXPECT(feed(&t, &m, &io, 1010, 1) == 389000, "101 C: still critical");
    EXPECT(feed(&t, &m, &io, 999, 1) == 607000 && t.zone == Sm8750ThermalHot, "99.9 C: down to hot");
    EXPECT(feed(&t, &m, &io, 1050, 1) == 389000, "105.0 C: critical again");
    EXPECT(feed(&t, &m, &io, 700, 1) == 0 && t.zone == Sm8750ThermalNormal, "70 C: critical straight to normal");
    EXPECT(t.peak_deci == 1051 && t.last_deci == 700, "peak %d last %d", t.peak_deci, t.last_deci);
    const uint32_t transitions = t.transitions;
    /* Transient invalid reads keep the zone; 8 in a row lose it. */
    EXPECT(feed(&t, &m, &io, 870, 1) == 607000, "hot");
    for (unsigned i = 0; i < Sm8750ThermalInvalidLimit - 1; ++i)
        EXPECT(feed(&t, &m, &io, 0, 0) == 607000 && t.zone == Sm8750ThermalHot, "transient invalid %u keeps hot", i);
    EXPECT(feed(&t, &m, &io, 0, 0) == 607000 && t.zone == Sm8750ThermalUnknown, "8th invalid: unknown");
    EXPECT(feed(&t, &m, &io, 600, 1) == 0 && t.zone == Sm8750ThermalNormal && t.invalid_streak == 0, "valid again: normal");
    EXPECT(t.transitions > transitions && t.invalid_samples == 8, "counted");
    /* Normal zone and transient invalid: still no cap. */
    EXPECT(feed(&t, &m, &io, 0, 0) == 0, "one invalid read in normal keeps no cap");
    /* VALID never set: unknown (607 MHz) from the first update on, for good. */
    Sm8750ThermalInit(&t);
    for (unsigned i = 0; i < 20; ++i)
        EXPECT(feed(&t, &m, &io, 300, 0) == 607000 && t.zone == Sm8750ThermalUnknown, "never valid %u", i);
    /* Window not mapped: unknown. */
    Sm8750ThermalInit(&t);
    Sm8750TsensSample s;
    EXPECT(Sm8750ThermalUpdate(&t, Sm8750TsensReadGpu(NULL, &s), &s) == 607000, "unmapped: 607 MHz");
    EXPECT(Sm8750ThermalUpdate(&t, Sm8750TsensArgument, NULL) == 607000, "null sample");
    EXPECT(Sm8750ThermalUpdate(NULL, Sm8750TsensOk, &s) == 607000 && Sm8750ThermalCapKhz(NULL) == 607000, "null state");
    EXPECT(m.bad_offsets == 0, "only TM_Sn_STATUS 1..8 read");
}

int main(void)
{
    test_decode();
    test_read();
    test_zones();
    printf("PASS: SM8750 tsens2 GPU sensors 1-8: VALID bit 21 and signed 12-bit deci-Celsius decoded, retries and maximum "
           "(%u checks); cap 607 MHz at >=85 C or unreadable, 389 MHz at >=105 C, 5 C hysteresis\n", checks);
    return 0;
}
