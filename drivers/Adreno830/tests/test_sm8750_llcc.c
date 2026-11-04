/* SPDX-License-Identifier: MIT */
/* host-test: platform/sm8750/Sm8750Llcc.c */
/* Model of the SM8750 LLCC v6 as Linux llcc-qcom.c drives it: four banks behind three windows. A write to the
 * broadcast OR window reaches every bank, a read returns the OR of the banks; a read of the broadcast AND window
 * returns the AND; channel 0 answers the common status (bank count). Slice (de)activation starts on ACT_CTRL written
 * with TRIG and then without it, completes after a configurable number of status polls (ACT_COMPLETE on each bank,
 * the state bits flip), and ACT_CLEAR drops ACT_COMPLETE. Any access outside the registers the sequence may touch
 * (the ALGO registers, writes to the AND window or channel 0, other slices) aborts the test. Faults: failed reads,
 * writes the hypervisor ignores, banks that never complete, a state bit that never clears. The constants are checked
 * against A830Regs.h. */
#include "../platform/sm8750/Sm8750Llcc.h"
#include "../A830Regs.h"
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

enum { Banks = 4, Slices = 64, MaxLog = 4096 };

typedef struct {
    unsigned block;
    uint32_t offset, value;
    int write;
} Access;

typedef struct {
    uint32_t attr[Banks][Slices][4];
    uint32_t wrs[Banks][2], wrs_cacheable[Banks][2];
    uint32_t state[Banks][Slices];            /* bit0 active, bit1 deactive, bit4 act_complete */
    uint32_t ctrl[Slices];                    /* last ACT_CTRL value */
    int armed[Slices];                        /* TRIG seen, waiting for the TRIG-clear write */
    int pending[Slices];                      /* polls left until the operation completes, -1 none */
    uint32_t pending_opcode[Slices];
    uint32_t version, banks;
    int latency;                              /* status polls (AND view) before completion */
    int never_complete_bank;                  /* bank that never sets ACT_COMPLETE, -1 none */
    int stuck_state_slice;                    /* slice whose old state bit never clears, -1 none */
    uint32_t ignore_offset;                   /* OR writes to this offset are dropped by the hypervisor, 0 none */
    unsigned ignore_bank;                     /* ... on this bank only (Banks = every bank) */
    uint32_t fail_read_block, fail_read_offset;   /* read that fails, offset 0 = none */
    unsigned delays;
    Access log[MaxLog];
    unsigned count;
} Model;

static Model g;
static Sm8750Llcc llcc;   /* the board's object: static storage, zero until the first probe */

static void log_access(unsigned block, uint32_t offset, uint32_t value, int write)
{
    CHECK(g.count < MaxLog, "access log full");
    g.log[g.count++] = (Access){block, offset, value, write};
}

/* Which register an OR/AND offset names: kind 0..3 ATTR0..3, 4 WRS_EN, 5 WRS_CACHEABLE_EN, 6 ACT_CTRL, 7 STATUS,
 * 8 ACT_CLEAR, 9 COMMON_HW_INFO; -1 anything else. */
static int decode(uint32_t offset, uint32_t *slice)
{
    *slice = 0;
    if (offset == Sm8750LlccCommonHwInfo)
        return 9;
    if (offset >= Sm8750LlccAttr0Base && offset < Sm8750LlccAttr0Base + Slices * Sm8750LlccAttrStride) {
        const uint32_t rel = offset - Sm8750LlccAttr0Base;
        *slice = rel / Sm8750LlccAttrStride;
        switch (rel % Sm8750LlccAttrStride) {
        case 0x0: return 0;
        case 0x8: return 1;
        case 0x10: return 2;
        case 0x14: return 3;
        default: return -1;
        }
    }
    if (offset == Sm8750LlccWrsEn || offset == Sm8750LlccWrsEn + 4) {
        *slice = (offset - Sm8750LlccWrsEn) / 4;
        return 4;
    }
    if (offset == Sm8750LlccWrsCacheableEn || offset == Sm8750LlccWrsCacheableEn + 4) {
        *slice = (offset - Sm8750LlccWrsCacheableEn) / 4;
        return 5;
    }
    if (offset < Slices * Sm8750LlccActCtrlStride) {
        *slice = offset / Sm8750LlccActCtrlStride;
        switch (offset % Sm8750LlccActCtrlStride) {
        case 0: return 6;
        case 4: return 7;
        case 8: return 8;
        default: return -1;
        }
    }
    return -1;
}

static uint32_t bank_value(unsigned b, int kind, uint32_t slice)
{
    switch (kind) {
    case 0: case 1: case 2: case 3: return g.attr[b][slice][kind];
    case 4: return g.wrs[b][slice];
    case 5: return g.wrs_cacheable[b][slice];
    case 7: return g.state[b][slice];
    case 9: return g.version;
    default: return 0;
    }
}

/* An operation in flight completes once the AND view has been polled `latency` times. */
static void advance(uint32_t s)
{
    if (g.pending[s] < 0)
        return;
    if (g.pending[s] > 0) {
        g.pending[s]--;
        return;
    }
    g.pending[s] = -1;
    const int activate = g.pending_opcode[s] == Sm8750LlccOpcodeActivate;
    for (unsigned b = 0; b < g.banks; b++) {
        if ((int)b != g.never_complete_bank)
            g.state[b][s] |= Sm8750LlccStatusActComplete;
        if ((int)s == g.stuck_state_slice)
            continue;
        g.state[b][s] = (g.state[b][s] & Sm8750LlccStatusActComplete) |
                        (activate ? Sm8750LlccStatusActive : Sm8750LlccStatusDeactive);
    }
}

static int model_read(void *cookie, unsigned block, uint32_t offset, uint32_t *value)
{
    (void)cookie;
    CHECK(block < Sm8750LlccBlocks && !(offset & 3), "read block %u offset %#x", block, offset);
    log_access(block, offset, 0, 0);
    if (g.fail_read_offset && block == g.fail_read_block && offset == g.fail_read_offset)
        return -1;
    if (block == Sm8750LlccChannel0) {
        CHECK(offset == Sm8750LlccCommonStatus0, "channel 0 read of %#x", offset);
        *value = g.banks << 28 | 0x00a5;
        return 0;
    }
    uint32_t slice;
    const int kind = decode(offset, &slice);
    CHECK(kind >= 0 && kind != 6 && kind != 8, "read of %#x", offset);
    if (block == Sm8750LlccBroadcastAnd) {
        CHECK(kind == 7, "AND window read of %#x (only TRP_STATUS)", offset);
        advance(slice);
        uint32_t v = 0xffffffffu;
        for (unsigned b = 0; b < g.banks; b++)
            v &= bank_value(b, kind, slice);
        *value = v;
        return 0;
    }
    if (kind == 9) {                  /* common register: one value, whatever the bank count says */
        *value = g.version;
        return 0;
    }
    uint32_t v = 0;
    for (unsigned b = 0; b < g.banks; b++)
        v |= bank_value(b, kind, slice);
    *value = v;
    return 0;
}

static int model_write(void *cookie, unsigned block, uint32_t offset, uint32_t value)
{
    (void)cookie;
    CHECK(block == Sm8750LlccBroadcastOr, "write to block %u offset %#x (only the broadcast OR window)", block, offset);
    log_access(block, offset, value, 1);
    uint32_t slice;
    const int kind = decode(offset, &slice);
    CHECK(kind >= 0 && kind != 7 && kind != 9, "write of %#x", offset);
    if (kind == 6) {
        CHECK(slice == Sm8750LlccSliceGpu || slice == Sm8750LlccSliceGpuHtw, "ACT_CTRL of slice %u", slice);
        if (value & Sm8750LlccActTrig) {
            g.armed[slice] = 1;
        } else {
            CHECK(g.armed[slice] && (g.ctrl[slice] & ~(uint32_t)Sm8750LlccActTrig) == value, "trigger sequence of slice %u",
                  slice);
            g.armed[slice] = 0;
            g.pending[slice] = g.latency;
            g.pending_opcode[slice] = value;
        }
        g.ctrl[slice] = value;
        return 0;
    }
    if (kind == 8) {
        CHECK(value == Sm8750LlccActClear, "ACT_CLEAR value %#x", value);
        for (unsigned b = 0; b < g.banks; b++)
            g.state[b][slice] &= ~(uint32_t)Sm8750LlccStatusActComplete;
        return 0;
    }
    if (kind <= 3)
        CHECK(slice == Sm8750LlccSliceGpu || slice == Sm8750LlccSliceGpuHtw, "attribute write to slice %u", slice);
    for (unsigned b = 0; b < g.banks; b++) {
        if (offset == g.ignore_offset && (g.ignore_bank == Banks || g.ignore_bank == b))
            continue;
        switch (kind) {
        case 0: case 1: case 2: case 3: g.attr[b][slice][kind] = value; break;
        case 4: g.wrs[b][slice] = value; break;
        case 5: g.wrs_cacheable[b][slice] = value; break;
        default: break;
        }
    }
    return 0;
}

static int model_delay(void *cookie, unsigned us)
{
    (void)cookie;
    CHECK(us == 1, "delay of %u us", us);
    g.delays++;
    return 0;
}

static const Sm8750LlccIo io = {NULL, model_read, model_write, model_delay};

static void reset_model(void)
{
    memset(&g, 0, sizeof(g));
    memset(&llcc, 0, sizeof(llcc));
    g.version = 0x06010000;
    g.banks = Banks;
    g.latency = 2;
    g.never_complete_bank = -1;
    g.stuck_state_slice = -1;
    for (unsigned s = 0; s < Slices; s++) {
        g.pending[s] = -1;
        for (unsigned b = 0; b < Banks; b++)
            g.state[b][s] = Sm8750LlccStatusDeactive;
    }
}

static unsigned writes_to(uint32_t offset)
{
    unsigned n = 0;
    for (unsigned i = 0; i < g.count; i++)
        n += g.log[i].write && g.log[i].offset == offset;
    return n;
}

static int first_index(unsigned block, uint32_t offset, int write)
{
    for (unsigned i = 0; i < g.count; i++)
        if (g.log[i].block == block && g.log[i].offset == offset && g.log[i].write == write)
            return (int)i;
    return -1;
}

static void check_slice_attrs(uint32_t s, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3)
{
    for (unsigned b = 0; b < g.banks; b++)
        CHECK(g.attr[b][s][0] == a0 && g.attr[b][s][1] == a1 && g.attr[b][s][2] == a2 && g.attr[b][s][3] == a3,
              "slice %u bank %u attributes %#x %#x %#x %#x", s, b, g.attr[b][s][0], g.attr[b][s][1], g.attr[b][s][2],
              g.attr[b][s][3]);
}

int main(void)
{
    Sm8750LlccStatus s;
    const uint32_t ctrl9 = Sm8750LlccSliceGpu * 0x1000, ctrl11 = Sm8750LlccSliceGpuHtw * 0x1000;

    /* Constants: the same facts as A830Regs.h (ARCHITECTURE 4.15). */
    CHECK(Sm8750LlccChannel0Phys == SM8750_LLCC0_PHYS && Sm8750LlccBroadcastOrPhys == SM8750_LLCC_BCAST_OR_PHYS &&
              Sm8750LlccBroadcastAndPhys == SM8750_LLCC_BCAST_AND_PHYS && Sm8750LlccWindowBytes == SM8750_LLCC_WINDOW_BYTES &&
              Sm8750LlccCommonHwInfo == SM8750_LLCC_V6_COMMON_HW_INFO &&
              Sm8750LlccCommonStatus0 == SM8750_LLCC_V6_COMMON_STATUS0 &&
              Sm8750LlccAttr0Base + 64u * 9 == SM8750_LLCC_V6_ATTR0(9) && Sm8750LlccAttr1Base + 64u * 9 == SM8750_LLCC_V6_ATTR1(9) &&
              Sm8750LlccAttr2Base + 64u * 11 == SM8750_LLCC_V6_ATTR2(11) &&
              Sm8750LlccAttr3Base + 64u * 11 == SM8750_LLCC_V6_ATTR3(11) && Sm8750LlccWrsEn == SM8750_LLCC_V6_WRS_EN &&
              Sm8750LlccWrsCacheableEn == SM8750_LLCC_V6_WRS_CACHEABLE_EN &&
              Sm8750LlccVersion6 == SM8750_LLCC_VERSION_6 && Sm8750LlccSliceGpu == SM8750_LLCC_SCID_GPU &&
              Sm8750LlccSliceGpuHtw == SM8750_LLCC_SCID_GPUHTW && Sm8750LlccPollUs == A830_T_LLCC_US,
          "constants against A830Regs.h");
    unsigned n = 0;
    const Sm8750LlccSliceConfig *cfg = Sm8750LlccGpuSlices(&n);
    CHECK(n == 2 && cfg[0].slice == 9 && cfg[1].slice == 11 && Sm8750LlccAttr2(&cfg[0]) == SM8750_LLCC_ATTR2_GPU_VALUE &&
              Sm8750LlccAttr3(&cfg[0], 4) == SM8750_LLCC_ATTR3_GPU_VALUE &&
              Sm8750LlccAttr3(&cfg[1], 4) == SM8750_LLCC_ATTR3_GPUHTW_VALUE && Sm8750LlccAttr3(&cfg[0], 0) == 0 &&
              Sm8750LlccAttr2(NULL) == 0,
          "slice values");

    /* 1. Fresh LLCC (nobody configured the GPU slices): probe, configure and activate 9 and 11. */
    reset_model();
    g.state[0][1] = g.state[1][1] = g.state[2][1] = g.state[3][1] = Sm8750LlccStatusActive;   /* CPUSS by firmware */
    s = Sm8750LlccEnableGpu(&io, &llcc);
    CHECK(s == Sm8750LlccOk && llcc.probed && llcc.version == 0x06010000 && llcc.banks == 4, "enable: %d", s);
    CHECK(llcc.survey[0] == Sm8750LlccStatusActive && llcc.survey[1] == Sm8750LlccStatusDeactive &&
              llcc.survey[2] == Sm8750LlccStatusDeactive && llcc.survey[3] == Sm8750LlccStatusDeactive,
          "survey of slices 1, 31, 9, 11");
    check_slice_attrs(9, 0, 0xffffffffu, 0x1100, 0x5800);
    check_slice_attrs(11, 0, 0xffffffffu, 0x1100, 0x800);
    for (unsigned b = 0; b < Banks; b++)
        CHECK(g.wrs[b][0] == 1u << 9 && g.wrs_cacheable[b][0] == 1u << 9, "write-scid bits of bank %u", b);
    CHECK(llcc.configured == ((1ull << 9) | (1ull << 11)) && llcc.active == ((1ull << 9) | (1ull << 11)) && !llcc.kept &&
              !llcc.abandoned && Sm8750LlccGpuScid(&llcc) == 9 && llcc.trace.step == Sm8750LlccStepDone,
          "masks");
    for (unsigned b = 0; b < Banks; b++)
        CHECK((g.state[b][9] & 0x13) == Sm8750LlccStatusActive && (g.state[b][11] & 0x13) == Sm8750LlccStatusActive,
              "active and ACT_COMPLETE cleared on bank %u", b);
    /* Order: version first, banks second; nothing written before the attributes were read; ATTR0 (res_ways 0) never
     * written; ACT_CTRL 0x3 then 0x2, then the AND completion poll, the OR state poll, ACT_CLEAR. */
    CHECK(g.log[0].block == Sm8750LlccBroadcastOr && g.log[0].offset == Sm8750LlccCommonHwInfo && !g.log[0].write &&
              g.log[1].block == Sm8750LlccChannel0 && g.log[1].offset == Sm8750LlccCommonStatus0,
          "probe order");
    CHECK(writes_to(Sm8750LlccAttr0Base + 64 * 9) == 0 && writes_to(Sm8750LlccAttr0Base + 64 * 11) == 0, "ATTR0 untouched");
    CHECK(writes_to(Sm8750LlccWrsEn) == 1 && writes_to(Sm8750LlccWrsCacheableEn) == 1, "WRS bits written once (slice 9)");
    {
        const int trig = first_index(Sm8750LlccBroadcastOr, ctrl9, 1);
        CHECK(trig >= 0 && g.log[trig].value == 0x3 && g.log[trig + 1].offset == ctrl9 && g.log[trig + 1].value == 0x2 &&
                  g.log[trig + 2].block == Sm8750LlccBroadcastAnd && g.log[trig + 2].offset == ctrl9 + 4,
              "slice 9 trigger");
        const int clear = first_index(Sm8750LlccBroadcastOr, ctrl9 + 8, 1);
        const int state = first_index(Sm8750LlccBroadcastOr, ctrl9 + 4, 0);
        CHECK(clear > trig && g.log[clear].value == 1 && state > 0 && clear > state, "slice 9 completion order");
        const int attr = first_index(Sm8750LlccBroadcastOr, Sm8750LlccAttr1Base + 64 * 9, 1);
        const int attr_read = first_index(Sm8750LlccBroadcastOr, Sm8750LlccAttr1Base + 64 * 9, 0);
        CHECK(attr_read >= 0 && attr > attr_read && trig > attr, "read before write, attributes before activation");
        CHECK(first_index(Sm8750LlccBroadcastOr, ctrl11, 1) > clear, "slice 11 after slice 9");
    }
    /* 1a. Again (the next GX power-up): nothing to write. */
    unsigned before = g.count;
    s = Sm8750LlccEnableGpu(&io, &llcc);
    CHECK(s == Sm8750LlccOk && g.count == before, "second enable touches nothing (%u accesses)", g.count - before);
    /* 1b. Slumber: deactivate both (ACT_CTRL 0x5 then 0x4), then a wake re-activates without reconfiguring. */
    before = g.count;
    s = Sm8750LlccDisableGpu(&io, &llcc);
    CHECK(s == Sm8750LlccOk && !llcc.active && Sm8750LlccGpuScid(&llcc) == -1, "disable: %d", s);
    {
        const int trig = first_index(Sm8750LlccBroadcastOr, ctrl9, 1);
        int last = -1;
        for (unsigned i = before; i < g.count; i++)
            if (g.log[i].write && g.log[i].offset == ctrl9)
                last = (int)i;
        CHECK(trig >= 0 && last > 0 && g.log[last].value == 0x4 && g.log[last - 1].value == 0x5, "deactivate trigger");
    }
    for (unsigned b = 0; b < Banks; b++)
        CHECK((g.state[b][9] & 3) == Sm8750LlccStatusDeactive && (g.state[b][11] & 3) == Sm8750LlccStatusDeactive,
              "deactivated bank %u", b);
    const unsigned attr_writes = writes_to(Sm8750LlccAttr1Base + 64 * 9);
    s = Sm8750LlccEnableGpu(&io, &llcc);
    CHECK(s == Sm8750LlccOk && llcc.active == ((1ull << 9) | (1ull << 11)) &&
              writes_to(Sm8750LlccAttr1Base + 64 * 9) == attr_writes,
          "re-activation without reconfiguration");
    CHECK(Sm8750LlccDisableGpu(&io, &llcc) == Sm8750LlccOk && Sm8750LlccDisableGpu(&io, &llcc) == Sm8750LlccOk,
          "disable twice");

    /* 2. Firmware configured slice 9 (other values) and set other slices' write-scid bits: kept, merged. */
    reset_model();
    for (unsigned b = 0; b < Banks; b++) {
        g.attr[b][9][1] = 0x0000ffff;
        g.attr[b][9][2] = 0x3100;
        g.wrs[b][0] = 1u << 1;
        g.wrs_cacheable[b][0] = (1u << 9) | (1u << 2);
    }
    s = Sm8750LlccEnableGpu(&io, &llcc);
    CHECK(s == Sm8750LlccOk && llcc.kept == (1ull << 9) && llcc.active == ((1ull << 9) | (1ull << 11)), "kept: %d", s);
    check_slice_attrs(9, 0, 0x0000ffff, 0x3100, 0x5800);   /* ATTR3 was zero: ours */
    for (unsigned b = 0; b < Banks; b++)
        CHECK(g.wrs[b][0] == ((1u << 1) | (1u << 9)) && g.wrs_cacheable[b][0] == ((1u << 9) | (1u << 2)),
              "write-scid merge on bank %u", b);
    CHECK(writes_to(Sm8750LlccWrsCacheableEn) == 0 && writes_to(Sm8750LlccAttr1Base + 64 * 9) == 0,
          "set bits and non-zero attributes are not rewritten");

    /* 3. The hypervisor drops writes to slice 9's ATTR3: slice 9 abandoned (never triggered), slice 11 still active. */
    reset_model();
    g.ignore_offset = Sm8750LlccAttr3Base + 64 * 9;
    g.ignore_bank = Banks;
    s = Sm8750LlccEnableGpu(&io, &llcc);
    CHECK(s == Sm8750LlccReadback && llcc.abandoned == (1ull << 9) && llcc.active == (1ull << 11) &&
              Sm8750LlccGpuScid(&llcc) == -1 && writes_to(ctrl9) == 0,
          "dropped write: %d", s);
    CHECK(llcc.trace.step == Sm8750LlccStepAttributes && llcc.trace.slice == 9 &&
              llcc.trace.offset == Sm8750LlccAttr3Base + 64 * 9 && llcc.trace.value == 0 && llcc.trace.expect == 0x5800 &&
              llcc.trace.status == Sm8750LlccReadback && llcc.trace.block == Sm8750LlccBroadcastOr,
          "dropped write trace");
    CHECK(Sm8750LlccActivate(&io, &llcc, 9) == Sm8750LlccReadback && Sm8750LlccEnableGpu(&io, &llcc) == Sm8750LlccReadback &&
              writes_to(ctrl9) == 0,
          "an abandoned slice stays abandoned");
    /* 3a. A dropped write of the write-scid bit. */
    reset_model();
    g.ignore_offset = Sm8750LlccWrsEn;
    g.ignore_bank = Banks;
    CHECK(Sm8750LlccEnableGpu(&io, &llcc) == Sm8750LlccReadback && llcc.abandoned == (1ull << 9) &&
              llcc.trace.step == Sm8750LlccStepWriteScid && llcc.active == (1ull << 11),
          "dropped write-scid bit");

    /* 4. Activation that never completes on one bank: Timeout after 100 one-microsecond polls, no ACT_CLEAR. */
    reset_model();
    g.never_complete_bank = 2;
    s = Sm8750LlccEnableGpu(&io, &llcc);
    CHECK(s == Sm8750LlccTimeout && !llcc.active && llcc.abandoned == ((1ull << 9) | (1ull << 11)) &&
              llcc.trace.step == Sm8750LlccStepComplete && llcc.trace.block == Sm8750LlccBroadcastAnd &&
              llcc.trace.expect == Sm8750LlccStatusActComplete && writes_to(ctrl9 + 8) == 0 && writes_to(ctrl11 + 8) == 0,
          "never complete: %d step %u", s, llcc.trace.step);
    CHECK(g.delays == 2 * Sm8750LlccPollUs, "100 us per slice: %u delays", g.delays);
    /* 4a. The DEACTIVE bit stays set: Timeout in the state poll. */
    reset_model();
    g.stuck_state_slice = 11;
    s = Sm8750LlccEnableGpu(&io, &llcc);
    CHECK(s == Sm8750LlccTimeout && llcc.active == (1ull << 9) && llcc.trace.step == Sm8750LlccStepState &&
              llcc.trace.slice == 11 && llcc.trace.block == Sm8750LlccBroadcastOr,
          "stuck state: %d", s);
    /* 4b. Completion right away (latency 0) and slow completion within the budget. */
    reset_model();
    g.latency = 0;
    CHECK(Sm8750LlccEnableGpu(&io, &llcc) == Sm8750LlccOk && g.delays == 0, "immediate completion");
    reset_model();
    g.latency = 99;
    CHECK(Sm8750LlccEnableGpu(&io, &llcc) == Sm8750LlccOk && g.delays == 2 * 99, "completion at 99 us: %u", g.delays);
    reset_model();
    g.latency = 100;
    CHECK(Sm8750LlccEnableGpu(&io, &llcc) == Sm8750LlccOk, "completion at the 100 us limit");
    reset_model();
    g.latency = 101;
    CHECK(Sm8750LlccEnableGpu(&io, &llcc) == Sm8750LlccTimeout, "completion after the limit");

    /* 5. Already active on every bank (firmware): no trigger, counted as active. */
    reset_model();
    for (unsigned b = 0; b < Banks; b++)
        g.state[b][9] = Sm8750LlccStatusActive;
    CHECK(Sm8750LlccEnableGpu(&io, &llcc) == Sm8750LlccOk && writes_to(ctrl9) == 0 && writes_to(ctrl11) == 2 &&
              Sm8750LlccGpuScid(&llcc) == 9,
          "already active");
    /* Active on some banks only: triggered. */
    reset_model();
    g.state[0][9] = Sm8750LlccStatusActive;
    CHECK(Sm8750LlccEnableGpu(&io, &llcc) == Sm8750LlccOk && writes_to(ctrl9) == 2, "partly active is triggered");

    /* 6. Probe refusals: nothing is ever written. */
    reset_model();
    g.version = 0x05000000;
    CHECK(Sm8750LlccEnableGpu(&io, &llcc) == Sm8750LlccUnsupported && !llcc.probed && g.count == 1 &&
              llcc.trace.step == Sm8750LlccStepVersion && llcc.trace.value == 0x05000000,
          "v5 refused");
    reset_model();
    g.banks = 0;
    CHECK(Sm8750LlccEnableGpu(&io, &llcc) == Sm8750LlccUnsupported && llcc.trace.step == Sm8750LlccStepBanks && g.count == 2,
          "no banks");
    reset_model();
    g.fail_read_block = Sm8750LlccBroadcastOr;
    g.fail_read_offset = Sm8750LlccCommonHwInfo;
    CHECK(Sm8750LlccEnableGpu(&io, &llcc) == Sm8750LlccIoError && llcc.trace.value == 0xffffffffu && g.count == 1,
          "failed version read");
    reset_model();
    g.fail_read_block = Sm8750LlccBroadcastOr;
    g.fail_read_offset = Sm8750LlccSliceWrcache * 0x1000 + 4;
    CHECK(Sm8750LlccProbe(&io, &llcc) == Sm8750LlccIoError && !llcc.probed && llcc.trace.step == Sm8750LlccStepSurvey,
          "failed survey read");
    /* A failed attribute read abandons only that slice. */
    reset_model();
    g.fail_read_block = Sm8750LlccBroadcastOr;
    g.fail_read_offset = Sm8750LlccAttr2Base + 64 * 11;
    CHECK(Sm8750LlccEnableGpu(&io, &llcc) == Sm8750LlccIoError && llcc.active == (1ull << 9) &&
              llcc.abandoned == (1ull << 11),
          "failed attribute read");

    /* 7. Two banks: ATTR3 follows the bank count (MAX_CAP_TO_BYTES / num_banks >> 6). */
    reset_model();
    g.banks = 2;
    CHECK(Sm8750LlccEnableGpu(&io, &llcc) == Sm8750LlccOk && llcc.banks == 2, "two banks");
    check_slice_attrs(9, 0, 0xffffffffu, 0x1100, 0xB000);
    check_slice_attrs(11, 0, 0xffffffffu, 0x1100, 0x1000);

    /* 8. Arguments and order. */
    reset_model();
    CHECK(Sm8750LlccProbe(NULL, &llcc) == Sm8750LlccArgument && Sm8750LlccProbe(&io, NULL) == Sm8750LlccArgument,
          "probe arguments");
    Sm8750LlccIo broken = io;
    broken.delay_us = NULL;
    CHECK(Sm8750LlccEnableGpu(&broken, &llcc) == Sm8750LlccArgument && g.count == 0, "missing delay");
    memset(&llcc, 0, sizeof(llcc));
    CHECK(Sm8750LlccConfigure(&io, &llcc, &cfg[0]) == Sm8750LlccNotProbed && Sm8750LlccActivate(&io, &llcc, 9) ==
              Sm8750LlccNotProbed && g.count == 0,
          "nothing before the probe");
    CHECK(Sm8750LlccProbe(&io, &llcc) == Sm8750LlccOk, "probe");
    CHECK(Sm8750LlccActivate(&io, &llcc, 9) == Sm8750LlccArgument && writes_to(ctrl9) == 0, "activation before configuration");
    Sm8750LlccSliceConfig wild = cfg[0];
    wild.slice = 64;
    CHECK(Sm8750LlccConfigure(&io, &llcc, &wild) == Sm8750LlccArgument && Sm8750LlccActivate(&io, &llcc, 64) ==
              Sm8750LlccArgument && Sm8750LlccDeactivate(&io, &llcc, 70) == Sm8750LlccArgument,
          "slice range");
    CHECK(Sm8750LlccDeactivate(&io, &llcc, 9) == Sm8750LlccOk && writes_to(ctrl9) == 0, "deactivating an inactive slice");
    CHECK(Sm8750LlccGpuScid(NULL) == -1 && Sm8750LlccEnableGpu(&io, NULL) == Sm8750LlccArgument &&
              Sm8750LlccDisableGpu(NULL, &llcc) == Sm8750LlccArgument,
          "NULL");

    puts("PASS: SM8750 LLCC v6 slices 9/11: read before write, firmware values kept, broadcast OR writes with exact "
         "readback, ACT_CTRL trigger with AND completion and OR state polls (100 us), slices abandoned independently");
    return 0;
}
