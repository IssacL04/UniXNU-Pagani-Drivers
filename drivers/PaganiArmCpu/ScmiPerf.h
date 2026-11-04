// SPDX-License-Identifier: BSD-2-Clause-Patent
// The SCMI PERF client of PaganiArmCpu (pagani-cpufreq, ArmCpu.cpp): the CPUCP firmware of the SM8750 changes the CPU clusters'
// performance levels on request over SCMI, as Android's scmi-cpufreq does. Kept apart so the host test (Test/scmi-perf.cpp) runs the
// same code against a fake CPUCP. Integer only, no library calls, no constructors (the kernel keeps these as zeroed globals).
//
// Transport (Linux drivers/firmware/arm_scmi shmem.c + mailbox.c over drivers/mailbox/qcom-cpucp-mbox.c, "qcom,cpucp-v2"):
//   shared memory (arm,scmi-shmem, 0x200 bytes): +0x00 reserved, +0x04 channel_status (bit 0 free, bit 1 error), +0x08 reserved x2,
//   +0x10 flags (bit 0 completion interrupt), +0x14 length (header + payload bytes), +0x18 message header, +0x1c payload;
//   the response replaces the message: length = 4 (header) + 4 (status) + return values, payload[0] = int32 status.
//   message header = message id | type 0 (command) << 8 | protocol << 10 | token << 18 (token 10 bits).
//   doorbell: a 32-bit write of a non-zero sequence number (Linux writes the transfer id) at tx + 0x100 + 8 * channel + 4.
//   Completion is polled (flags 0: no completion interrupt): channel_status free or error with the header's token ours, bounded by a
//   timeout. Nothing is read from or written to the RX window (only needed for interrupts).
// Every failure (the channel never free, no answer in time, the error bit, a status other than 0, a short or malformed answer, a value
// outside what was described) comes back as a Result; classify() says which ones are worth another try later (an SCMI BUSY answer; no
// usable answer: the channel still held, no answer in time, the channel error bit, COMMS_ERROR) and which are final (any other status:
// the platform refused the message; a malformed answer; no mapping; interrupt context). The caller (CpuFreqGovernor.h) retries the
// first kind later with a backoff and within a budget, and switches the feature off for the boot on the second, showing why. A transfer
// never waits longer than the timeout for the channel and the timeout for the answer: retries are the caller's, never in here.
#ifndef ARMCPU_SCMI_PERF_H
#define ARMCPU_SCMI_PERF_H
#include <stdint.h>
#include <stddef.h>

namespace scmi {

// Board facts of the OnePlus 13T (Android device tree, /sys/firmware/devicetree/base): soc/qcom,cpucp@0x17830000 (qcom,cpucp-v2:
// reg rx 0x16430000 + 0x4c08, tx 0x17830000 + 0x300, GIC SPI 26), soc/mmio-sram@0x17b4e000 scmi-shmem@0 (tx channel), soc/qcom,scmi
// (mboxes <&cpucp 0> tx, <&cpucp 2> rx; protocol 0x13 PERF: CPUs 0-5 domain 0, CPUs 6-7 (MPIDR 0x10000, 0x10100) domain 1).
enum : uint64_t {
    kRxBase = 0x16430000, kRxBytes = 0x4c08,
    kTxBase = 0x17830000, kTxBytes = 0x300,
    kShmemBase = 0x17b4e000, kShmemBytes = 0x200,
};
enum : uint32_t {
    kTxChannel = 0,
    kDoorbell = 0x100 + 8 * kTxChannel + 4,         // APSS_CPUCP_TX_MBOX_CMD(chan) + APSS_CPUCP_MBOX_CMD_OFF
    kChannelStatus = 0x04, kFlags = 0x10, kLength = 0x14, kHeader = 0x18, kPayload = 0x1c,
    kChannelFree = 1, kChannelError = 2,
    kPayloadBytes = (uint32_t)kShmemBytes - kPayload,   // 484: what one message or answer can carry after the header
    kPayloadWords = kPayloadBytes / 4,
    // Protocols and messages
    kBase = 0x10, kPerf = 0x13,
    kVersion = 0x0, kAttributes = 0x1,
    kDomainAttributes = 0x3, kDescribeLevels = 0x4, kLimitsSet = 0x5, kLimitsGet = 0x6, kLevelSet = 0x7, kLevelGet = 0x8,
    // PERF_DOMAIN_ATTRIBUTES flags
    kCanSetLimits = 1u << 31, kCanSetLevel = 1u << 30, kLevelIndexing = 1u << 25,
    kMaxLevels = 32,
    kDomains = 2,
    kTimeoutMs = 30,                                // Linux's default max-rx-timeout-ms
};

static inline uint32_t header(uint32_t protocol, uint32_t message, uint32_t token)
{
    return (message & 0xff) | (0u << 8) | ((protocol & 0xff) << 10) | ((token & 0x3ff) << 18);
}
static inline uint32_t headerToken(uint32_t value) { return (value >> 18) & 0x3ff; }
static inline uint32_t headerMessage(uint32_t value) { return value & 0xff; }
static inline uint32_t headerProtocol(uint32_t value) { return (value >> 10) & 0xff; }

// The platform side, supplied by the caller: the shared memory words, the doorbell, a counter and what to do between polls.
struct Io {
    void *context;
    uint32_t (*read32)(void *context, uint32_t offset);
    void (*write32)(void *context, uint32_t offset, uint32_t value);
    void (*ring)(void *context, uint32_t value);        // barrier, then the doorbell write
    uint64_t (*now)(void *context);                     // counter ticks
    void (*pause)(void *context, uint64_t waited);      // between two polls; waited: ticks since the poll began
    bool (*allowed)(void *context);                     // false in interrupt context: refused before any access
    uint64_t hz;                                        // counter frequency
    uint32_t timeoutMs;                                 // 0: kTimeoutMs
};

enum Error : uint8_t {
    kOk, kNotReady, kInterrupt, kBusy, kTimeout, kChannelErr, kStatus, kShort, kBadHeader, kBadVersion, kFewDomains, kNoLevelSet,
    kBadLevels, kTooManyLevels, kUnknownLevel, kBadDomain,
};
struct Result {
    Error error;
    uint8_t protocol, message;
    int32_t status;                 // kStatus: the SCMI status (Status below)
};
static inline bool ok(const Result &r) { return r.error == kOk; }

// The SCMI status codes (Arm DEN0056, "Status codes"; Linux arm_scmi common.h names them SCMI_ERR_*).
enum Status : int32_t {
    kSuccess = 0, kNotSupported = -1, kInvalidParameters = -2, kDenied = -3, kNotFound = -4, kOutOfRange = -5, kBusyStatus = -6,
    kCommsError = -7, kGenericError = -8, kHardwareError = -9, kProtocolError = -10, kInUse = -11,
};

// What a failed transfer means for the caller.
enum Class : uint8_t {
    kFinal,                         // try no more: a refusal (any status but BUSY and COMMS_ERROR), a malformed answer (short, wrong
                                    // header, a value outside what was described), no mapping, interrupt context
    kRetryBusy,                     // the platform answered BUSY (-6): it is there, busy now, and asks to be asked again
    kRetrySilent,                   // no usable answer: the channel still held from an earlier message (kBusy), no answer in time
                                    // (kTimeout), the channel error bit (kChannelErr) or COMMS_ERROR (-7), the transport's failures
};
static inline Class classify(const Result &r)
{
    switch (r.error) {
    case kStatus:
        if (r.status == kBusyStatus) return kRetryBusy;
        return r.status == kCommsError ? kRetrySilent : kFinal;
    case kBusy: case kTimeout: case kChannelErr: return kRetrySilent;
    case kOk: case kNotReady: case kInterrupt: case kShort: case kBadHeader: case kBadVersion: case kFewDomains: case kNoLevelSet:
    case kBadLevels: case kTooManyLevels: case kUnknownLevel: case kBadDomain:
    default: return kFinal;
    }
}

// "timeout lvl get", "status -3 lvl set", "short describe": what failed and in which message (at most 31 characters).
struct Text {
    char *at;
    size_t used, room;
};
static inline void put(Text *t, const char *s)
{
    for (; s && *s && t->used + 1 < t->room; s++) t->at[t->used++] = *s;
    if (t->room) t->at[t->used < t->room ? t->used : t->room - 1] = 0;
}
static inline void putUnsigned(Text *t, uint64_t value)
{
    char digits[24];
    unsigned n = 0;
    do { digits[n++] = (char)('0' + value % 10); value /= 10; } while (value && n < sizeof(digits));
    char one[2] = {0, 0};
    while (n) { one[0] = digits[--n]; put(t, one); }
}
static inline void putSigned(Text *t, int64_t value)
{
    if (value < 0) { put(t, "-"); putUnsigned(t, (uint64_t)(-(value + 1)) + 1); }
    else putUnsigned(t, (uint64_t)value);
}
static inline const char *messageName(uint32_t protocol, uint32_t message)
{
    if (protocol == kBase) return message == kVersion ? "base ver" : "base";
    if (protocol != kPerf) return "?";
    switch (message) {
    case kVersion: return "perf ver";
    case kAttributes: return "perf attr";
    case kDomainAttributes: return "dom attr";
    case kDescribeLevels: return "describe";
    case kLimitsSet: return "lim set";
    case kLimitsGet: return "lim get";
    case kLevelSet: return "lvl set";
    case kLevelGet: return "lvl get";
    default: return "perf";
    }
}
static inline void describe(const Result &r, char *out, size_t size)
{
    Text t = {out, 0, size};
    if (size) out[0] = 0;
    switch (r.error) {
    case kOk: put(&t, "ok"); return;
    case kNotReady: put(&t, "not mapped"); return;
    case kInterrupt: put(&t, "irq context"); break;
    case kBusy: put(&t, "chan busy"); break;
    case kTimeout: put(&t, "timeout"); break;
    case kChannelErr: put(&t, "chan error"); break;
    case kStatus: put(&t, "status "); putSigned(&t, r.status); break;
    case kShort: put(&t, "short"); break;
    case kBadHeader: put(&t, "bad header"); break;
    case kBadVersion: put(&t, "version"); break;
    case kFewDomains: put(&t, "domains"); break;
    case kNoLevelSet: put(&t, "no lvl set"); break;
    case kBadLevels: put(&t, "bad levels"); break;
    case kTooManyLevels: put(&t, "levels>32"); break;
    case kUnknownLevel: put(&t, "unknown lvl"); break;
    case kBadDomain: put(&t, "domain"); break;
    default: put(&t, "error"); break;
    }
    put(&t, " ");
    put(&t, messageName(r.protocol, r.message));
}

// One polled transfer at a time (the caller serializes): the sequence number feeds the token and the doorbell value.
struct Channel {
    Io io;
    uint32_t sequence;
    uint64_t transfers, polls;
};

static inline Result fail(Error e, uint32_t protocol, uint32_t message, int32_t status = 0)
{
    Result r;
    r.error = e; r.protocol = (uint8_t)protocol; r.message = (uint8_t)message; r.status = status;
    return r;
}

static inline uint64_t timeoutTicks(const Io &io)
{
    const uint64_t ms = io.timeoutMs ? io.timeoutMs : kTimeoutMs;
    return io.hz / 1000 * ms + 1;
}

// Sends `protocol`/`message` with txWords words and polls for the answer; up to rxWords words of its return values (after the status)
// go to rx, *rxBytes says how many bytes the platform returned (it may be more than rx holds).
static inline Result transfer(Channel *c, uint32_t protocol, uint32_t message, const uint32_t *tx, unsigned txWords, uint32_t *rx,
                              unsigned rxWords, uint32_t *rxBytes)
{
    const Io &io = c->io;
    if (rxBytes) *rxBytes = 0;
    if (!io.read32 || !io.write32 || !io.ring || !io.now || !io.hz) return fail(kNotReady, protocol, message);
    if (io.allowed && !io.allowed(io.context)) return fail(kInterrupt, protocol, message);
    if (txWords > kPayloadWords) return fail(kShort, protocol, message);
    const uint64_t limit = timeoutTicks(io);
    // A channel the platform still holds (a late answer to an earlier message) is waited for, never written over.
    uint64_t start = io.now(io.context);
    while (!(io.read32(io.context, kChannelStatus) & kChannelFree)) {
        const uint64_t waited = io.now(io.context) - start;
        if (waited > limit) return fail(kBusy, protocol, message);
        if (io.pause) io.pause(io.context, waited);
    }
    c->sequence = c->sequence + 1 ? c->sequence + 1 : 1;
    const uint32_t token = c->sequence & 0x3ff, word = header(protocol, message, token);
    io.write32(io.context, kChannelStatus, 0);              // busy, error cleared
    io.write32(io.context, kFlags, 0);                      // polled: no completion interrupt
    io.write32(io.context, kLength, 4 + 4 * txWords);
    io.write32(io.context, kHeader, word);
    for (unsigned i = 0; i < txWords; i++) io.write32(io.context, kPayload + 4 * i, tx[i]);
    io.ring(io.context, c->sequence);
    c->transfers++;
    start = io.now(io.context);
    uint32_t status;
    for (;;) {
        status = io.read32(io.context, kChannelStatus);
        c->polls++;
        if ((status & (kChannelFree | kChannelError)) && headerToken(io.read32(io.context, kHeader)) == token) break;
        const uint64_t waited = io.now(io.context) - start;
        if (waited > limit) return fail(kTimeout, protocol, message);
        if (io.pause) io.pause(io.context, waited);
    }
    if (status & kChannelError) return fail(kChannelErr, protocol, message);
    const uint32_t answer = io.read32(io.context, kHeader);
    if (headerMessage(answer) != message || headerProtocol(answer) != protocol) return fail(kBadHeader, protocol, message);
    const uint32_t length = io.read32(io.context, kLength);
    if (length < 8 || length > 4 + kPayloadBytes) return fail(kShort, protocol, message);
    const int32_t code = (int32_t)io.read32(io.context, kPayload);
    if (code != 0) return fail(kStatus, protocol, message, code);
    const uint32_t bytes = length - 8;
    if (rxBytes) *rxBytes = bytes;
    const unsigned words = (bytes + 3) / 4 < rxWords ? (bytes + 3) / 4 : rxWords;
    for (unsigned i = 0; i < words; i++) rx[i] = io.read32(io.context, kPayload + 4 + 4 * i);
    return fail(kOk, protocol, message);
}

// One performance level as described: its value for PERF_LEVEL_SET/GET (the performance value, or with level indexing the index), its
// performance value and the clock in kHz.
struct Level {
    uint32_t value, perf, khz;
    uint16_t latencyUs, reserved;
};
struct Domain {
    uint32_t flags, rateLimitUs, sustainedKhz, sustainedPerf;
    char name[17];
    bool indexing;
    uint32_t count;
    uint64_t multiplier;            // Hz per performance unit (Linux perf.c mult_factor)
    Level levels[kMaxLevels];       // ascending
};
struct Perf {
    Channel channel;
    uint32_t baseVersion, perfVersion, domains;
    Domain domain[kDomains];
};

static inline Result baseVersion(Perf *p)
{
    uint32_t rx[1] = {0}, bytes = 0;
    Result r = transfer(&p->channel, kBase, kVersion, nullptr, 0, rx, 1, &bytes);
    if (ok(r) && bytes < 4) r = fail(kShort, kBase, kVersion);
    if (ok(r)) {
        p->baseVersion = rx[0];
        if ((rx[0] >> 16) < 1 || (rx[0] >> 16) > 3) r = fail(kBadVersion, kBase, kVersion);
    }
    return r;
}

static inline Result perfVersion(Perf *p)
{
    uint32_t rx[1] = {0}, bytes = 0;
    Result r = transfer(&p->channel, kPerf, kVersion, nullptr, 0, rx, 1, &bytes);
    if (ok(r) && bytes < 4) r = fail(kShort, kPerf, kVersion);
    if (ok(r)) {
        p->perfVersion = rx[0];
        if ((rx[0] >> 16) < 1 || (rx[0] >> 16) > 4) r = fail(kBadVersion, kPerf, kVersion);
    }
    return r;
}

static inline Result perfAttributes(Perf *p)
{
    uint32_t rx[4] = {0, 0, 0, 0}, bytes = 0;
    Result r = transfer(&p->channel, kPerf, kAttributes, nullptr, 0, rx, 4, &bytes);
    if (ok(r) && bytes < 4) r = fail(kShort, kPerf, kAttributes);
    if (ok(r)) {
        p->domains = rx[0] & 0xffff;
        if (p->domains < kDomains) r = fail(kFewDomains, kPerf, kAttributes);
    }
    return r;
}

static inline Result domainAttributes(Perf *p, unsigned d)
{
    if (d >= kDomains) return fail(kBadDomain, kPerf, kDomainAttributes);
    Domain *dom = &p->domain[d];
    const uint32_t tx[1] = {d};
    uint32_t rx[8] = {0, 0, 0, 0, 0, 0, 0, 0}, bytes = 0;
    Result r = transfer(&p->channel, kPerf, kDomainAttributes, tx, 1, rx, 8, &bytes);
    if (ok(r) && bytes < 16) r = fail(kShort, kPerf, kDomainAttributes);
    if (!ok(r)) return r;
    dom->flags = rx[0]; dom->rateLimitUs = rx[1]; dom->sustainedKhz = rx[2]; dom->sustainedPerf = rx[3];
    for (unsigned i = 0; i < 17; i++) dom->name[i] = 0;
    for (unsigned i = 0; bytes >= 32 && i < 16; i++) {
        const char c = (char)(rx[4 + i / 4] >> (8 * (i % 4)));
        if (!c) break;
        dom->name[i] = c >= 32 && c <= 126 ? c : '?';
    }
    dom->indexing = (p->perfVersion >> 16) >= 4 && (dom->flags & kLevelIndexing);
    // sustained_freq_khz = mult_factor * sustained_perf_level / 1000 (perf.c): a performance value in kHz gives 1000 Hz per unit.
    if (!dom->sustainedKhz || dom->sustainedPerf > dom->sustainedKhz || !dom->sustainedPerf) dom->multiplier = 1;
    else dom->multiplier = (uint64_t)dom->sustainedKhz * 1000 / dom->sustainedPerf;
    if (!(dom->flags & kCanSetLevel)) r = fail(kNoLevelSet, kPerf, kDomainAttributes);
    return r;
}

// PERF_DESCRIBE_LEVELS, as many rounds as the platform needs (num_returned now, num_remaining after them): v1-v3 entries are three words
// (performance value, power, latency in the low half), v4 entries five (also the indicative kHz and the level index). The total must stay
// what the first answer announced; the levels are sorted ascending and must be distinct and non-zero.
static inline Result describeLevels(Perf *p, unsigned d)
{
    if (d >= kDomains) return fail(kBadDomain, kPerf, kDescribeLevels);
    Domain *dom = &p->domain[d];
    const bool v4 = (p->perfVersion >> 16) >= 4;
    const uint32_t entry = v4 ? 5 : 3;
    uint32_t got = 0, total = 0;
    dom->count = 0;
    for (unsigned round = 0; round <= kMaxLevels; round++) {
        const uint32_t tx[2] = {d, got};
        uint32_t rx[kPayloadWords], bytes = 0;
        rx[0] = 0;
        const Result r = transfer(&p->channel, kPerf, kDescribeLevels, tx, 2, rx, kPayloadWords, &bytes);
        if (!ok(r)) return fail(r.error, kPerf, kDescribeLevels, r.status);
        if (bytes < 4) return fail(kShort, kPerf, kDescribeLevels);
        const uint32_t returned = rx[0] & 0xffff, remaining = rx[0] >> 16;
        if (bytes < 4 + returned * entry * 4 || 1 + returned * entry > kPayloadWords) return fail(kShort, kPerf, kDescribeLevels);
        if (!round) total = returned + remaining;
        if (total > kMaxLevels) return fail(kTooManyLevels, kPerf, kDescribeLevels);
        if (got + returned + remaining != total || (!returned && remaining)) return fail(kBadLevels, kPerf, kDescribeLevels);
        for (uint32_t i = 0; i < returned; i++) {
            const uint32_t *e = rx + 1 + i * entry;
            Level *l = &dom->levels[got + i];
            l->perf = e[0];
            l->latencyUs = (uint16_t)(e[2] & 0xffff);
            l->reserved = 0;
            const uint32_t indicative = v4 ? e[3] : 0;
            l->value = dom->indexing ? e[4] : e[0];
            const uint64_t khz = indicative ? indicative : (uint64_t)e[0] * dom->multiplier / 1000;
            l->khz = khz > 0xffffffffu ? 0xffffffffu : (uint32_t)khz;
        }
        got += returned;
        if (!remaining) break;
    }
    if (got != total || !got) return fail(kBadLevels, kPerf, kDescribeLevels);
    // Firmware may answer in any order (perf.c sorts too).
    for (uint32_t i = 1; i < got; i++) {
        const Level key = dom->levels[i];
        uint32_t j = i;
        for (; j > 0 && dom->levels[j - 1].perf > key.perf; j--) dom->levels[j] = dom->levels[j - 1];
        dom->levels[j] = key;
    }
    for (uint32_t i = 0; i < got; i++)
        if (!dom->levels[i].perf || (i && dom->levels[i].perf == dom->levels[i - 1].perf)) return fail(kBadLevels, kPerf, kDescribeLevels);
    dom->count = got;
    return fail(kOk, kPerf, kDescribeLevels);
}

// The index of the level whose value the platform reported: exact, else (performance values) the highest level not above it.
static inline bool levelIndex(const Domain &dom, uint32_t value, uint32_t *index)
{
    for (uint32_t i = 0; i < dom.count; i++)
        if (dom.levels[i].value == value) { *index = i; return true; }
    if (dom.indexing || !dom.count || value < dom.levels[0].perf) return false;
    uint32_t best = 0;
    for (uint32_t i = 0; i < dom.count; i++)
        if (dom.levels[i].perf <= value) best = i;
    *index = best;
    return true;
}

static inline Result levelGet(Perf *p, unsigned d, uint32_t *index, uint32_t *raw)
{
    if (d >= kDomains || !p->domain[d].count) return fail(kBadDomain, kPerf, kLevelGet);
    const uint32_t tx[1] = {d};
    uint32_t rx[1] = {0}, bytes = 0;
    Result r = transfer(&p->channel, kPerf, kLevelGet, tx, 1, rx, 1, &bytes);
    if (ok(r) && bytes < 4) r = fail(kShort, kPerf, kLevelGet);
    if (ok(r)) {
        if (raw) *raw = rx[0];
        if (!levelIndex(p->domain[d], rx[0], index)) r = fail(kUnknownLevel, kPerf, kLevelGet);
    }
    return r;
}

static inline Result levelSet(Perf *p, unsigned d, uint32_t index)
{
    if (d >= kDomains || index >= p->domain[d].count) return fail(kBadDomain, kPerf, kLevelSet);
    const uint32_t tx[2] = {d, p->domain[d].levels[index].value};
    uint32_t bytes = 0;
    return transfer(&p->channel, kPerf, kLevelSet, tx, 2, nullptr, 0, &bytes);
}

// PERF_LIMITS_GET as level indices (max, min); the limits may lie between levels, then the levels inside them.
static inline Result limitsGet(Perf *p, unsigned d, uint32_t *maxIndex, uint32_t *minIndex)
{
    if (d >= kDomains || !p->domain[d].count) return fail(kBadDomain, kPerf, kLimitsGet);
    const uint32_t tx[1] = {d};
    uint32_t rx[2] = {0, 0}, bytes = 0;
    Result r = transfer(&p->channel, kPerf, kLimitsGet, tx, 1, rx, 2, &bytes);
    if (ok(r) && bytes < 8) r = fail(kShort, kPerf, kLimitsGet);
    if (ok(r)) {
        const Domain &dom = p->domain[d];
        uint32_t hi = dom.count - 1, lo = 0;
        if (!levelIndex(dom, rx[0], &hi)) hi = 0;
        if (!levelIndex(dom, rx[1], &lo)) lo = 0;
        else if (!dom.indexing && dom.levels[lo].perf < rx[1] && lo + 1 < dom.count) lo++;
        *maxIndex = hi; *minIndex = lo;
    }
    return r;
}

// Everything the client needs before it changes a level: the BASE version, the PERF version and attributes, both domains' attributes
// and levels.
static inline Result probe(Perf *p)
{
    Result r = baseVersion(p);
    if (ok(r)) r = perfVersion(p);
    if (ok(r)) r = perfAttributes(p);
    for (unsigned d = 0; ok(r) && d < kDomains; d++) {
        r = domainAttributes(p, d);
        if (ok(r)) r = describeLevels(p, d);
    }
    return r;
}

}  // namespace scmi
#endif
