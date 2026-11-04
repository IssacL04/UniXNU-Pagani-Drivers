/* SPDX-License-Identifier: MIT */
#include "Sm8750Llcc.h"
#include <stddef.h>

/* sm8750_data entries of the two GPU slices (Linux llcc-qcom.c). */
static const Sm8750LlccSliceConfig gpu_slices[] = {
    {.slice = Sm8750LlccSliceGpu, .max_cap_kib = Sm8750LlccGpuMaxCapKib, .priority = 1, .fixed_size = 1,
     .bonus_ways = 0xffffffffu, .res_ways = 0, .write_scid_en = 1, .write_scid_cacheable_en = 1},
    {.slice = Sm8750LlccSliceGpuHtw, .max_cap_kib = Sm8750LlccGpuHtwMaxCapKib, .priority = 1, .fixed_size = 1,
     .bonus_ways = 0xffffffffu, .res_ways = 0, .write_scid_en = 0, .write_scid_cacheable_en = 0},
};

static const uint32_t survey_slices[4] = {Sm8750LlccSliceCpuss, Sm8750LlccSliceWrcache, Sm8750LlccSliceGpu,
                                          Sm8750LlccSliceGpuHtw};

static int io_valid(const Sm8750LlccIo *io)
{
    return io && io->read32 && io->write32 && io->delay_us;
}

static uint64_t bit(uint32_t slice)
{
    return (uint64_t)1 << slice;
}

/* The first failure is the one the status page shows; later ones only count. */
static Sm8750LlccStatus failed(Sm8750Llcc *llcc, Sm8750LlccStatus status, Sm8750LlccStep step, uint32_t slice,
                               unsigned block, uint32_t offset, uint32_t value, uint32_t expect)
{
    if (llcc->trace.step == Sm8750LlccStepNone || llcc->trace.step == Sm8750LlccStepDone) {
        llcc->trace.step = step;
        llcc->trace.slice = slice;
        llcc->trace.block = block;
        llcc->trace.offset = offset;
        llcc->trace.value = value;
        llcc->trace.expect = expect;
        llcc->trace.status = status;
    }
    return status;
}

static int rd(const Sm8750LlccIo *io, Sm8750Llcc *llcc, unsigned block, uint32_t offset, uint32_t *value)
{
    llcc->reads++;
    *value = 0xffffffffu;
    return io->read32(io->cookie, block, offset, value);
}

static int wr(const Sm8750LlccIo *io, Sm8750Llcc *llcc, unsigned block, uint32_t offset, uint32_t value)
{
    llcc->writes++;
    return io->write32(io->cookie, block, offset, value);
}

uint32_t Sm8750LlccAttr2(const Sm8750LlccSliceConfig *config)
{
    /* cache_mode 0 (normal) and probe_target_ways 0 for both GPU slices. */
    if (!config)
        return 0;
    return (config->fixed_size ? (uint32_t)Sm8750LlccAttr2FixedSize : 0u) |
           (config->priority & 7u) << Sm8750LlccAttr2PriorityShift;
}

uint32_t Sm8750LlccAttr3(const Sm8750LlccSliceConfig *config, uint32_t banks)
{
    /* MAX_CAP_TO_BYTES(max_cap) / num_banks >> CACHE_LINE_SIZE_SHIFT */
    if (!config || !banks)
        return 0;
    return (uint32_t)(((uint64_t)config->max_cap_kib * 1024u / banks) >> 6);
}

const Sm8750LlccSliceConfig *Sm8750LlccGpuSlices(unsigned *count)
{
    if (count)
        *count = (unsigned)(sizeof(gpu_slices) / sizeof(gpu_slices[0]));
    return gpu_slices;
}

Sm8750LlccStatus Sm8750LlccProbe(const Sm8750LlccIo *io, Sm8750Llcc *llcc)
{
    if (!llcc)
        return Sm8750LlccArgument;
    *llcc = (Sm8750Llcc){0};
    if (!io_valid(io))
        return failed(llcc, Sm8750LlccArgument, Sm8750LlccStepVersion, 0, 0, 0, 0, 0);
    uint32_t value;
    if (rd(io, llcc, Sm8750LlccBroadcastOr, Sm8750LlccCommonHwInfo, &value))
        return failed(llcc, Sm8750LlccIoError, Sm8750LlccStepVersion, 0, Sm8750LlccBroadcastOr, Sm8750LlccCommonHwInfo,
                      value, Sm8750LlccVersion6);
    llcc->version = value;
    if (value < (uint32_t)Sm8750LlccVersion6)
        return failed(llcc, Sm8750LlccUnsupported, Sm8750LlccStepVersion, 0, Sm8750LlccBroadcastOr,
                      Sm8750LlccCommonHwInfo, value, Sm8750LlccVersion6);
    if (rd(io, llcc, Sm8750LlccChannel0, Sm8750LlccCommonStatus0, &value))
        return failed(llcc, Sm8750LlccIoError, Sm8750LlccStepBanks, 0, Sm8750LlccChannel0, Sm8750LlccCommonStatus0, value,
                      4);
    llcc->status0 = value;
    llcc->banks = value >> 28;     /* LLCC_LB_CNT_MASK GENMASK(31, 28) */
    if (!llcc->banks)
        return failed(llcc, Sm8750LlccUnsupported, Sm8750LlccStepBanks, 0, Sm8750LlccChannel0, Sm8750LlccCommonStatus0,
                      value, 4);
    /* Read-only survey (ARCHITECTURE 4.15 "risk"): what firmware left active. A failed read is only recorded. */
    for (unsigned i = 0; i < 4; i++) {
        const uint32_t offset = survey_slices[i] * Sm8750LlccActCtrlStride + Sm8750LlccStatusOffset;
        if (rd(io, llcc, Sm8750LlccBroadcastOr, offset, &value))
            return failed(llcc, Sm8750LlccIoError, Sm8750LlccStepSurvey, survey_slices[i], Sm8750LlccBroadcastOr, offset,
                          value, 0);
        llcc->survey[i] = value;
    }
    llcc->probed = 1;
    llcc->trace.step = Sm8750LlccStepDone;
    return Sm8750LlccOk;
}

/* One attribute register: kept when firmware set it, written (and read back exactly) when it is zero and should not
 * be; *kept is set for a kept non-zero value. */
static Sm8750LlccStatus attribute(const Sm8750LlccIo *io, Sm8750Llcc *llcc, uint32_t slice, uint32_t offset,
                                  uint32_t want, int *kept)
{
    uint32_t value;
    if (rd(io, llcc, Sm8750LlccBroadcastOr, offset, &value))
        return failed(llcc, Sm8750LlccIoError, Sm8750LlccStepAttributes, slice, Sm8750LlccBroadcastOr, offset, value, want);
    if (value) {
        *kept = 1;
        return Sm8750LlccOk;
    }
    if (!want)
        return Sm8750LlccOk;
    if (wr(io, llcc, Sm8750LlccBroadcastOr, offset, want))
        return failed(llcc, Sm8750LlccIoError, Sm8750LlccStepAttributes, slice, Sm8750LlccBroadcastOr, offset, 0, want);
    if (rd(io, llcc, Sm8750LlccBroadcastOr, offset, &value))
        return failed(llcc, Sm8750LlccIoError, Sm8750LlccStepAttributes, slice, Sm8750LlccBroadcastOr, offset, value, want);
    if (value != want)
        return failed(llcc, Sm8750LlccReadback, Sm8750LlccStepAttributes, slice, Sm8750LlccBroadcastOr, offset, value, want);
    return Sm8750LlccOk;
}

/* regmap_update_bits(WRS*_EN + slice / 32 * 4, BIT(slice % 32), BIT(slice % 32)): the other slices' bits stay. */
static Sm8750LlccStatus write_scid_bit(const Sm8750LlccIo *io, Sm8750Llcc *llcc, uint32_t slice, uint32_t base)
{
    const uint32_t offset = base + slice / 32 * 4, mask = 1u << (slice % 32);
    uint32_t value;
    if (rd(io, llcc, Sm8750LlccBroadcastOr, offset, &value))
        return failed(llcc, Sm8750LlccIoError, Sm8750LlccStepWriteScid, slice, Sm8750LlccBroadcastOr, offset, value, mask);
    if (value & mask)
        return Sm8750LlccOk;
    const uint32_t want = value | mask;
    if (wr(io, llcc, Sm8750LlccBroadcastOr, offset, want))
        return failed(llcc, Sm8750LlccIoError, Sm8750LlccStepWriteScid, slice, Sm8750LlccBroadcastOr, offset, 0, want);
    if (rd(io, llcc, Sm8750LlccBroadcastOr, offset, &value))
        return failed(llcc, Sm8750LlccIoError, Sm8750LlccStepWriteScid, slice, Sm8750LlccBroadcastOr, offset, value, want);
    if (value != want)
        return failed(llcc, Sm8750LlccReadback, Sm8750LlccStepWriteScid, slice, Sm8750LlccBroadcastOr, offset, value, want);
    return Sm8750LlccOk;
}

Sm8750LlccStatus Sm8750LlccConfigure(const Sm8750LlccIo *io, Sm8750Llcc *llcc, const Sm8750LlccSliceConfig *config)
{
    if (!llcc || !config || config->slice > Sm8750LlccMaxSlice || !io_valid(io))
        return llcc ? failed(llcc, Sm8750LlccArgument, Sm8750LlccStepAttributes, config ? config->slice : 0, 0, 0, 0, 0)
                    : Sm8750LlccArgument;
    if (!llcc->probed)
        return failed(llcc, Sm8750LlccNotProbed, Sm8750LlccStepAttributes, config->slice, 0, 0, 0, 0);
    const uint32_t s = config->slice;
    if (llcc->abandoned & bit(s))
        return Sm8750LlccReadback;
    if (llcc->configured & bit(s))
        return Sm8750LlccOk;
    const struct {
        uint32_t offset, want;
    } attrs[4] = {
        {Sm8750LlccAttr0Base + Sm8750LlccAttrStride * s, config->res_ways},
        {Sm8750LlccAttr1Base + Sm8750LlccAttrStride * s, config->bonus_ways},
        {Sm8750LlccAttr2Base + Sm8750LlccAttrStride * s, Sm8750LlccAttr2(config)},
        {Sm8750LlccAttr3Base + Sm8750LlccAttrStride * s, Sm8750LlccAttr3(config, llcc->banks)},
    };
    int kept = 0;
    Sm8750LlccStatus status = Sm8750LlccOk;
    for (unsigned i = 0; i < 4 && status == Sm8750LlccOk; i++)
        status = attribute(io, llcc, s, attrs[i].offset, attrs[i].want, &kept);
    if (status == Sm8750LlccOk && config->write_scid_en)
        status = write_scid_bit(io, llcc, s, Sm8750LlccWrsEn);
    if (status == Sm8750LlccOk && config->write_scid_cacheable_en)
        status = write_scid_bit(io, llcc, s, Sm8750LlccWrsCacheableEn);
    if (status != Sm8750LlccOk) {
        llcc->abandoned |= bit(s);
        return status;
    }
    if (kept)
        llcc->kept |= bit(s);
    llcc->configured |= bit(s);
    return Sm8750LlccOk;
}

/* Poll one TRP_STATUS view every microsecond for at most 100 us (regmap_read_poll_timeout(..., 0, 100)) until
 * (value & mask) == want. */
static Sm8750LlccStatus poll(const Sm8750LlccIo *io, Sm8750Llcc *llcc, uint32_t slice, unsigned block, uint32_t offset,
                             uint32_t mask, uint32_t want, Sm8750LlccStep step)
{
    uint32_t value = 0;
    for (unsigned waited = 0;; waited++) {
        if (rd(io, llcc, block, offset, &value))
            return failed(llcc, Sm8750LlccIoError, step, slice, block, offset, value, want);
        if ((value & mask) == want)
            return Sm8750LlccOk;
        if (waited >= Sm8750LlccPollUs)
            return failed(llcc, Sm8750LlccTimeout, step, slice, block, offset, value, want);
        llcc->polls++;
        if (io->delay_us(io->cookie, 1))
            return failed(llcc, Sm8750LlccIoError, step, slice, block, offset, value, want);
    }
}

/* llcc_update_act_ctrl(slice, opcode, opposite state bit). */
static Sm8750LlccStatus act_ctrl(const Sm8750LlccIo *io, Sm8750Llcc *llcc, uint32_t s, uint32_t opcode, uint32_t opposite)
{
    const uint32_t ctrl = s * Sm8750LlccActCtrlStride, status = ctrl + Sm8750LlccStatusOffset,
                   clear = ctrl + Sm8750LlccActClearOffset;
    if (wr(io, llcc, Sm8750LlccBroadcastOr, ctrl, opcode | Sm8750LlccActTrig))
        return failed(llcc, Sm8750LlccIoError, Sm8750LlccStepTrigger, s, Sm8750LlccBroadcastOr, ctrl, 0,
                      opcode | Sm8750LlccActTrig);
    if (wr(io, llcc, Sm8750LlccBroadcastOr, ctrl, opcode))
        return failed(llcc, Sm8750LlccIoError, Sm8750LlccStepTrigger, s, Sm8750LlccBroadcastOr, ctrl, 0, opcode);
    /* v4.1+: every bank reports completion (AND view), then no bank is left in the opposite state (OR view). */
    Sm8750LlccStatus result = poll(io, llcc, s, Sm8750LlccBroadcastAnd, status, Sm8750LlccStatusActComplete,
                                   Sm8750LlccStatusActComplete, Sm8750LlccStepComplete);
    if (result == Sm8750LlccOk)
        result = poll(io, llcc, s, Sm8750LlccBroadcastOr, status, opposite, 0, Sm8750LlccStepState);
    if (result != Sm8750LlccOk)
        return result;
    if (wr(io, llcc, Sm8750LlccBroadcastOr, clear, Sm8750LlccActClear))
        return failed(llcc, Sm8750LlccIoError, Sm8750LlccStepClear, s, Sm8750LlccBroadcastOr, clear, 0, Sm8750LlccActClear);
    return Sm8750LlccOk;
}

Sm8750LlccStatus Sm8750LlccActivate(const Sm8750LlccIo *io, Sm8750Llcc *llcc, uint32_t slice)
{
    if (!llcc || slice > Sm8750LlccMaxSlice || !io_valid(io))
        return llcc ? failed(llcc, Sm8750LlccArgument, Sm8750LlccStepTrigger, slice, 0, 0, 0, 0) : Sm8750LlccArgument;
    if (!llcc->probed)
        return failed(llcc, Sm8750LlccNotProbed, Sm8750LlccStepTrigger, slice, 0, 0, 0, 0);
    if (llcc->abandoned & bit(slice))
        return Sm8750LlccReadback;
    if (!(llcc->configured & bit(slice)))
        return failed(llcc, Sm8750LlccArgument, Sm8750LlccStepTrigger, slice, 0, 0, 0, 0);
    if (llcc->active & bit(slice))
        return Sm8750LlccOk;
    /* Active on every bank already (firmware, or an earlier boot stage): nothing to trigger. */
    const uint32_t status = slice * Sm8750LlccActCtrlStride + Sm8750LlccStatusOffset;
    uint32_t all, any;
    if (rd(io, llcc, Sm8750LlccBroadcastAnd, status, &all))
        return failed(llcc, Sm8750LlccIoError, Sm8750LlccStepState, slice, Sm8750LlccBroadcastAnd, status, all, 0);
    if (rd(io, llcc, Sm8750LlccBroadcastOr, status, &any))
        return failed(llcc, Sm8750LlccIoError, Sm8750LlccStepState, slice, Sm8750LlccBroadcastOr, status, any, 0);
    Sm8750LlccStatus result = Sm8750LlccOk;
    if (!((all & Sm8750LlccStatusActive) && !(any & Sm8750LlccStatusDeactive)))
        result = act_ctrl(io, llcc, slice, Sm8750LlccOpcodeActivate, Sm8750LlccStatusDeactive);
    if (result != Sm8750LlccOk) {
        llcc->abandoned |= bit(slice);
        return result;
    }
    llcc->active |= bit(slice);
    return Sm8750LlccOk;
}

Sm8750LlccStatus Sm8750LlccDeactivate(const Sm8750LlccIo *io, Sm8750Llcc *llcc, uint32_t slice)
{
    if (!llcc || slice > Sm8750LlccMaxSlice || !io_valid(io))
        return llcc ? failed(llcc, Sm8750LlccArgument, Sm8750LlccStepTrigger, slice, 0, 0, 0, 0) : Sm8750LlccArgument;
    if (!llcc->probed)
        return failed(llcc, Sm8750LlccNotProbed, Sm8750LlccStepTrigger, slice, 0, 0, 0, 0);
    if (!(llcc->active & bit(slice)))
        return Sm8750LlccOk;
    /* Taken out of the active set first: a slice whose deactivation timed out is not trusted as active either. */
    llcc->active &= ~bit(slice);
    return act_ctrl(io, llcc, slice, Sm8750LlccOpcodeDeactivate, Sm8750LlccStatusActive);
}

Sm8750LlccStatus Sm8750LlccEnableGpu(const Sm8750LlccIo *io, Sm8750Llcc *llcc)
{
    if (!llcc || !io_valid(io))
        return Sm8750LlccArgument;
    Sm8750LlccStatus first = Sm8750LlccOk;
    if (!llcc->probed) {
        first = Sm8750LlccProbe(io, llcc);
        if (first != Sm8750LlccOk)
            return first;
    }
    for (unsigned i = 0; i < sizeof(gpu_slices) / sizeof(gpu_slices[0]); i++) {
        const Sm8750LlccSliceConfig *config = &gpu_slices[i];
        Sm8750LlccStatus status = Sm8750LlccConfigure(io, llcc, config);
        if (status == Sm8750LlccOk)
            status = Sm8750LlccActivate(io, llcc, config->slice);
        if (status != Sm8750LlccOk && first == Sm8750LlccOk)
            first = status;
    }
    return first;
}

Sm8750LlccStatus Sm8750LlccDisableGpu(const Sm8750LlccIo *io, Sm8750Llcc *llcc)
{
    if (!llcc || !io_valid(io))
        return Sm8750LlccArgument;
    Sm8750LlccStatus first = Sm8750LlccOk;
    for (unsigned i = 0; i < sizeof(gpu_slices) / sizeof(gpu_slices[0]); i++) {
        const Sm8750LlccStatus status = Sm8750LlccDeactivate(io, llcc, gpu_slices[i].slice);
        if (status != Sm8750LlccOk && first == Sm8750LlccOk)
            first = status;
    }
    return first;
}

int Sm8750LlccGpuScid(const Sm8750Llcc *llcc)
{
    return llcc && (llcc->active & bit(Sm8750LlccSliceGpu)) ? (int)Sm8750LlccSliceGpu : -1;
}
