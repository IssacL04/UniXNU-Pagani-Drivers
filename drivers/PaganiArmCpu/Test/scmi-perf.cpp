// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test of pagani-cpufreq (ScmiPerf.h, CpuFreqGovernor.h) against a fake CPUCP: a thread that waits for the doorbell, reads the message
// from the shared memory the way the platform does and answers from a model of the 13T's two performance domains (Android's level tables,
// performance values in kHz), with injectable faults: no answer, a late answer, an SCMI status, a short or oversized answer, a wrong token
// or message id, the channel error bit, a channel that never becomes free, inconsistent DESCRIBE_LEVELS counts, too many levels, and
// thermal lowering (a cap on the level the platform applies and reports). The governor (pagani-cpufreq=2): its rules on exact work rates
// (1.25 x utilization, bursts, 3-of-4 drops, touch and GPU-frame levels), the GPU's thermal state (adreno830_thermal_state's flags as
// Adreno830 publishes them: the heat caps, touches through them, the emergency), the touch hold and the launch boost (how long they hold,
// the touch gate, their end, the heat and screen-off caps over them, SCMI BUSY during them), scenarios on a workload model with XNU's
// 10 ms idle ticks (light load with taps, a burst, a steady medium load, idle, GPU frames, heat, an app's launch), random windows against
// its invariants, and extreme windows (spans up to 2^64 - 1, levels near 2^32 kHz) against a 128-bit reference of its demand.
// Build: xcrun clang++ -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
//        Test/scmi-perf.cpp -o /tmp/scmi-perf
#include "../ScmiPerf.h"
#include "../CpuFreqGovernor.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <math.h>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

static unsigned checks;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); abort(); } checks++; } while (0)

static const uint32_t kTable[2][16] = {
    {384000, 556800, 748800, 960000, 1152000, 1363200, 1555200, 1785600, 1996800, 2227200, 2400000, 2745600, 2918400, 3072000, 3321600,
     3532800},
    {1017600, 1209600, 1401600, 1689600, 1958400, 2246400, 2438400, 2649600, 2841600, 3072000, 3283200, 3513600, 3801600, 4089600, 4204800,
     4320000},
};

enum FaultKind { kNone, kSilent, kLate, kStatusCode, kCut, kWrongToken, kWrongMessage, kErrorBit, kOversize, kCountsDrift, kNoProgress };
static const uint32_t kAnyMessage = 0xff;      // a Fault on every message of its protocol
struct Fault {
    uint32_t protocol = 0, message = 0;
    unsigned skip = 0;              // let this many matching messages through first
    unsigned times = 1;             // then fault this many (0: every one)
    FaultKind kind = kNone;
    int32_t code = 0;               // kStatusCode
    unsigned lateMs = 0;            // kLate
    int keep = -1;                  // kCut: return values kept (-1: half of them, a single one dropped)
};

struct Message {
    uint32_t protocol, message, type, token, status, flags, length, doorbell;
    std::vector<uint32_t> payload;
};

struct Model {
    uint32_t baseVersion = 0x20000, perfVersion = 0x30000, domains = 2;
    unsigned chunk = 5;                         // levels per DESCRIBE_LEVELS answer
    bool v4 = false, indexing = false, unsorted = false, noSet = false;
    unsigned extraLevels = 0;                   // describe more levels than the table (too many)
    uint32_t requested[2] = {8, 4};             // level indices (boot: 1996.8 / 1958.4 MHz)
    uint32_t cap[2] = {15, 15};                 // thermal: the highest level the platform applies
    uint32_t rateLimitUs[2] = {0, 0};
};

class FakeCpucp {
public:
    std::atomic<uint32_t> mem[scmi::kShmemBytes / 4];
    Model model;
    std::vector<Fault> faults;
    std::vector<Message> seen;
    std::mutex lock;

    FakeCpucp() {
        for (auto &w : mem) w.store(0);
        mem[scmi::kChannelStatus / 4].store(scmi::kChannelFree);
        worker_ = std::thread([this] { run(); });
    }
    ~FakeCpucp() {
        { std::lock_guard<std::mutex> g(lock); quit_ = true; }
        wake_.notify_all();
        worker_.join();
    }
    std::atomic<unsigned> rung{0};              // doorbells rung: one per message the agent sent
    void ring(uint32_t value) {
        rung++;
        { std::lock_guard<std::mutex> g(lock); rings_.push_back(value); }
        wake_.notify_all();
    }
    // Replaces the faults, their match counts from zero (under the lock: a late answer may still be in flight).
    void setFaults(const std::vector<Fault> &list) {
        std::lock_guard<std::mutex> g(lock);
        faults = list;
        matched_.assign(faults.size(), 0);
    }
    // Waits until every doorbell so far has been handled (late answers included).
    void settle() {
        std::unique_lock<std::mutex> g(lock);
        idle_.wait(g, [this] { return rings_.empty() && !busy_; });
    }
    std::vector<Message> messages() { std::lock_guard<std::mutex> g(lock); return seen; }
    unsigned count(uint32_t protocol, uint32_t message) {
        std::lock_guard<std::mutex> g(lock);
        unsigned n = 0;
        for (auto &m : seen) n += m.protocol == protocol && m.message == message;
        return n;
    }
    std::vector<uint32_t> sets(unsigned domain) {
        std::lock_guard<std::mutex> g(lock);
        std::vector<uint32_t> v;
        for (auto &m : seen)
            if (m.protocol == scmi::kPerf && m.message == scmi::kLevelSet && m.payload.size() == 2 && m.payload[0] == domain)
                v.push_back(m.payload[1]);
        return v;
    }
    // PERF_LEVEL_GET messages for domain `domain`.
    unsigned gets(unsigned domain) {
        std::lock_guard<std::mutex> g(lock);
        unsigned n = 0;
        for (auto &m : seen)
            n += m.protocol == scmi::kPerf && m.message == scmi::kLevelGet && m.payload.size() == 1 && m.payload[0] == domain;
        return n;
    }
    void clearSeen() { std::lock_guard<std::mutex> g(lock); seen.clear(); }

private:
    std::thread worker_;
    std::condition_variable wake_, idle_;
    std::deque<uint32_t> rings_;
    bool quit_ = false, busy_ = false;
    std::vector<unsigned> matched_;

    uint32_t rd(uint32_t offset) { return mem[offset / 4].load(std::memory_order_acquire); }
    void wr(uint32_t offset, uint32_t value) { mem[offset / 4].store(value, std::memory_order_relaxed); }
    uint32_t levels(unsigned d) const { return 16 + model.extraLevels * (d == 0); }
    // Level i of domain d in the order the platform describes it (unsorted: odd indices first, then even).
    unsigned described(unsigned i, unsigned d) const {
        if (!model.unsorted) return i;
        const unsigned n = levels(d), odd = n / 2;
        return i < odd ? 2 * i + 1 : 2 * (i - odd);
    }
    uint32_t khz(unsigned d, unsigned i) const { return i < 16 ? kTable[d][i] : kTable[d][15] + 1000 * (i - 15); }
    uint32_t value(unsigned d, unsigned i) const { return model.indexing ? 100 + i : khz(d, i); }
    bool find(unsigned d, uint32_t v, unsigned *index) const {
        for (unsigned i = 0; i < levels(d); i++)
            if (value(d, i) == v) { *index = i; return true; }
        return false;
    }
    void run() {
        std::unique_lock<std::mutex> g(lock);
        for (;;) {
            wake_.wait(g, [this] { return quit_ || !rings_.empty(); });
            if (quit_) return;
            const uint32_t doorbell = rings_.front();
            rings_.pop_front();
            busy_ = true;
            g.unlock();
            handle(doorbell);
            g.lock();
            busy_ = false;
            if (rings_.empty()) idle_.notify_all();
        }
    }
    void handle(uint32_t doorbell) {
        Message m;
        const uint32_t header = rd(scmi::kHeader);
        m.protocol = scmi::headerProtocol(header); m.message = scmi::headerMessage(header); m.type = (header >> 8) & 3;
        m.token = scmi::headerToken(header); m.status = rd(scmi::kChannelStatus); m.flags = rd(scmi::kFlags);
        m.length = rd(scmi::kLength); m.doorbell = doorbell;
        for (uint32_t i = 0; m.length >= 4 && i < (m.length - 4) / 4 && i < scmi::kPayloadWords; i++)
            m.payload.push_back(rd(scmi::kPayload + 4 * i));
        Fault fault;
        {
            std::lock_guard<std::mutex> g(lock);
            seen.push_back(m);
            matched_.resize(faults.size());
            for (size_t i = 0; i < faults.size(); i++) {
                const Fault &f = faults[i];
                if (f.protocol != m.protocol || (f.message != kAnyMessage && f.message != m.message)) continue;
                const unsigned n = matched_[i]++;
                if (n >= f.skip && (!f.times || n < f.skip + f.times)) { fault = f; break; }
            }
        }
        if (fault.kind == kSilent) return;                         // never answers: the channel stays busy
        if (fault.kind == kLate) std::this_thread::sleep_for(std::chrono::milliseconds(fault.lateMs));
        int32_t code = 0;
        std::vector<uint32_t> ret;
        answer(m, &code, &ret, fault);
        if (fault.kind == kStatusCode) { code = fault.code; ret.clear(); }
        if (fault.kind == kCut) ret.resize(fault.keep >= 0 ? (size_t)fault.keep : ret.size() / 2);
        uint32_t token = m.token, message = m.message;
        if (fault.kind == kWrongToken) token ^= 1;
        if (fault.kind == kWrongMessage) message ^= 1;
        for (size_t i = 0; i < ret.size() && i + 1 < scmi::kPayloadWords; i++) wr(scmi::kPayload + 4 + 4 * (uint32_t)i, ret[i]);
        wr(scmi::kPayload, (uint32_t)code);
        wr(scmi::kLength, fault.kind == kOversize ? 0x300 : 8 + 4 * (uint32_t)ret.size());
        wr(scmi::kHeader, scmi::header(m.protocol, message, token));
        mem[scmi::kChannelStatus / 4].store(scmi::kChannelFree | (fault.kind == kErrorBit ? scmi::kChannelError : 0u),
                                            std::memory_order_release);
    }
    void answer(const Message &m, int32_t *code, std::vector<uint32_t> *ret, const Fault &fault) {
        const std::vector<uint32_t> &p = m.payload;
        if (m.protocol == scmi::kBase && m.message == scmi::kVersion) { ret->push_back(model.baseVersion); return; }
        if (m.protocol != scmi::kPerf) { *code = -1; return; }
        const unsigned d = p.empty() ? 99 : p[0];
        switch (m.message) {
        case scmi::kVersion: ret->push_back(model.v4 ? 0x40000 : model.perfVersion); return;
        case scmi::kAttributes: *ret = {model.domains, 0, 0, 0}; return;
        case scmi::kDomainAttributes: {
            if (d >= model.domains || d > 1) { *code = -4; return; }
            uint32_t flags = scmi::kCanSetLimits | (model.noSet ? 0u : (uint32_t)scmi::kCanSetLevel) | (model.indexing ? scmi::kLevelIndexing : 0u);
            const uint32_t sustained = d ? 3801600 : 2918400;
            *ret = {flags, model.rateLimitUs[d], sustained, sustained, 0, 0, 0, 0};
            const char *name = d ? "perf1" : "perf0";
            for (unsigned i = 0; name[i]; i++) (*ret)[4 + i / 4] |= (uint32_t)(unsigned char)name[i] << (8 * (i % 4));
            return;
        }
        case scmi::kDescribeLevels: {
            if (d > 1 || p.size() < 2) { *code = -2; return; }
            const unsigned total = levels(d), from = p[1];
            if (from > total) { *code = -5; return; }
            unsigned n = total - from < model.chunk ? total - from : model.chunk;
            unsigned remaining = total - from - n;
            if (fault.kind == kCountsDrift) remaining += 1;
            if (fault.kind == kNoProgress) { n = 0; remaining = total - from; }
            ret->push_back(n | remaining << 16);
            for (unsigned k = 0; k < n; k++) {
                const unsigned i = described(from + k, d);
                ret->push_back(model.indexing ? khz(d, i) : value(d, i));   // performance value (kHz here)
                ret->push_back(1000 + i);                                    // power
                ret->push_back(30 + i);                                      // latency us, reserved 0
                if (model.v4) { ret->push_back(khz(d, i)); ret->push_back(value(d, i)); }
            }
            return;
        }
        case scmi::kLevelSet: {
            unsigned i = 0;
            if (d > 1 || p.size() < 2) { *code = -2; return; }
            if (!find(d, p[1], &i)) { *code = -5; return; }
            model.requested[d] = i;
            return;
        }
        case scmi::kLevelGet: {
            if (d > 1) { *code = -2; return; }
            const uint32_t applied = model.requested[d] < model.cap[d] ? model.requested[d] : model.cap[d];
            ret->push_back(value(d, applied));
            return;
        }
        case scmi::kLimitsGet:
            if (d > 1) { *code = -2; return; }
            *ret = {value(d, model.cap[d]), value(d, 0)};
            return;
        default: *code = -1; return;
        }
    }
};

// The agent side: the shared memory of the fake, its doorbell, a real clock in nanoseconds.
static bool gAllowed = true;
static uint32_t ioRead(void *c, uint32_t offset) { return static_cast<FakeCpucp *>(c)->mem[offset / 4].load(std::memory_order_acquire); }
static void ioWrite(void *c, uint32_t offset, uint32_t value)
{
    static_cast<FakeCpucp *>(c)->mem[offset / 4].store(value, std::memory_order_relaxed);
}
static void ioRing(void *c, uint32_t value)
{
    std::atomic_thread_fence(std::memory_order_seq_cst);
    static_cast<FakeCpucp *>(c)->ring(value);
}
static uint64_t ioNow(void *)
{
    return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
static void ioPause(void *, uint64_t waited)
{
    if (waited < 100000) std::this_thread::yield();
    else std::this_thread::sleep_for(std::chrono::microseconds(200));
}
static bool ioAllowed(void *) { return gAllowed; }

static std::vector<std::string> gLog;
static void logLine(void *, const char *text) { gLog.push_back(text); }
static bool logged(const char *part)
{
    for (auto &line : gLog)
        if (line.find(part) != std::string::npos) return true;
    return false;
}

static scmi::Io io(FakeCpucp *fake)
{
    scmi::Io v = {};
    v.context = fake; v.read32 = ioRead; v.write32 = ioWrite; v.ring = ioRing; v.now = ioNow; v.pause = ioPause; v.allowed = ioAllowed;
    v.hz = 1000000000; v.timeoutMs = 50;
    return v;
}

// A fresh agent on `fake` (each its own: a test may run a second one inside another).
static std::vector<std::unique_ptr<cpufreq::Control>> gControls;
static cpufreq::Control *control(FakeCpucp *fake)
{
    gControls.emplace_back(new cpufreq::Control());
    cpufreq::Control *c = gControls.back().get();
    memset(c, 0, sizeof(*c));
    c->perf.channel.io = io(fake);
    c->log = logLine;
    c->touchMs = cpufreq::kTouchMs;         // as ArmCpu.cpp without pagani-cpufreq-touch-ms and pagani-cpufreq-launch-ms
    c->launchMs = cpufreq::kLaunchMs;
    gLog.clear();
    return c;
}

static std::string row(const cpufreq::Control *c)
{
    char text[100];
    memset(text, 'x', sizeof(text));
    cpufreq::controlRow(c, text, sizeof(text));
    CHECK(strlen(text) <= cpufreq::kRowWidth);
    return text;
}

static const uint64_t MS = 1000000;     // ticks of the governor's clock (1 GHz, as io.hz)

static void testEncoding()
{
    // Linux arm_scmi pack_scmi_header: id bits 0-7, type 8-9, protocol 10-17, token 18-27.
    CHECK(scmi::header(0x13, 0x7, 5) == (0x7u | 0x13u << 10 | 5u << 18));
    CHECK(scmi::header(0x10, 0x0, 0x3ff) == (0x10u << 10 | 0x3ffu << 18));
    CHECK(scmi::header(0x13, 0x4, 0x400) == (0x4u | 0x13u << 10));                 // the token keeps 10 bits
    CHECK(scmi::headerToken(0x0ffc0000) == 0x3ff && scmi::headerProtocol(0x4c00) == 0x13 && scmi::headerMessage(0x4c08) == 0x08);
    CHECK(scmi::kDoorbell == 0x104 && scmi::kPayload == 0x1c && scmi::kPayloadBytes == 484 && scmi::kLength == 0x14);
    char text[64];
    scmi::Result r = scmi::fail(scmi::kTimeout, scmi::kPerf, scmi::kLevelGet);
    scmi::describe(r, text, sizeof(text));
    CHECK(!strcmp(text, "timeout lvl get"));
    r = scmi::fail(scmi::kStatus, scmi::kPerf, scmi::kLevelSet, -3);
    scmi::describe(r, text, sizeof(text));
    CHECK(!strcmp(text, "status -3 lvl set"));
    scmi::describe(r, text, 8);
    CHECK(!strcmp(text, "status "));
    r = scmi::fail(scmi::kStatus, scmi::kPerf, scmi::kLevelSet, INT32_MIN);
    scmi::describe(r, text, sizeof(text));
    CHECK(!strcmp(text, "status -2147483648 lvl set"));
    printf("ok  header and doorbell layout, failure texts\n");
}

static void testProbe()
{
    FakeCpucp fake;
    cpufreq::Control *c = control(&fake);
    scmi::Perf *p = &c->perf;
    CHECK(scmi::ok(scmi::probe(p)));
    fake.settle();
    CHECK(p->baseVersion == 0x20000 && p->perfVersion == 0x30000 && p->domains == 2);
    for (unsigned d = 0; d < 2; d++) {
        const scmi::Domain &dom = p->domain[d];
        CHECK(dom.count == 16 && dom.multiplier == 1000 && !dom.indexing && !strcmp(dom.name, d ? "perf1" : "perf0"));
        CHECK((dom.flags & scmi::kCanSetLevel) && dom.sustainedKhz == (d ? 3801600u : 2918400u));
        for (unsigned i = 0; i < 16; i++)
            CHECK(dom.levels[i].khz == kTable[d][i] && dom.levels[i].perf == kTable[d][i] && dom.levels[i].value == kTable[d][i] &&
                  dom.levels[i].latencyUs == 30 + i);
    }
    // What went over the wire, in order, each as Linux's shmem_tx_prepare writes it: channel busy (status 0), no completion interrupt,
    // length = header + payload, a command header whose token follows the doorbell value (non-zero, one more each time).
    const std::vector<Message> m = fake.messages();
    const uint32_t order[][2] = {{0x10, 0}, {0x13, 0}, {0x13, 1}, {0x13, 3}, {0x13, 4}, {0x13, 4}, {0x13, 4}, {0x13, 4},
                                 {0x13, 3}, {0x13, 4}, {0x13, 4}, {0x13, 4}, {0x13, 4}};
    CHECK(m.size() == sizeof(order) / sizeof(order[0]));
    for (size_t i = 0; i < m.size(); i++) {
        CHECK(m[i].protocol == order[i][0] && m[i].message == order[i][1] && m[i].type == 0);
        CHECK(m[i].status == 0 && m[i].flags == 0 && m[i].length == 4 + 4 * m[i].payload.size());
        CHECK(m[i].doorbell == i + 1 && m[i].token == ((i + 1) & 0x3ff));
    }
    // DESCRIBE_LEVELS in four rounds of 5, 5, 5, 1, each from the next index.
    CHECK(m[4].payload == std::vector<uint32_t>({0, 0}) && m[5].payload == std::vector<uint32_t>({0, 5}));
    CHECK(m[6].payload == std::vector<uint32_t>({0, 10}) && m[7].payload == std::vector<uint32_t>({0, 15}));
    CHECK(m[3].payload == std::vector<uint32_t>({0}) && m[8].payload == std::vector<uint32_t>({1}));
    // Level get and set, by index; the reported value maps back to the index.
    uint32_t index = 99, raw = 0;
    CHECK(scmi::ok(scmi::levelGet(p, 0, &index, &raw)) && index == 8 && raw == 1996800);
    CHECK(scmi::ok(scmi::levelSet(p, 1, 15)) && fake.sets(1) == std::vector<uint32_t>({4320000}));
    CHECK(scmi::ok(scmi::levelGet(p, 1, &index, &raw)) && index == 15 && raw == 4320000);
    uint32_t hi = 0, lo = 99;
    CHECK(scmi::ok(scmi::limitsGet(p, 1, &hi, &lo)) && hi == 15 && lo == 0);
    CHECK(scmi::levelSet(p, 1, 16).error == scmi::kBadDomain && scmi::levelSet(p, 2, 0).error == scmi::kBadDomain);
    // A value between levels maps to the level below it; one below every level is unknown.
    CHECK(scmi::levelIndex(p->domain[0], 2000000, &index) && index == 8);
    CHECK(!scmi::levelIndex(p->domain[0], 1000, &index));
    // The other chunk sizes give the same table: one level per answer, all at once.
    for (unsigned chunk : {1u, 16u}) {
        FakeCpucp other;
        other.model.chunk = chunk;
        cpufreq::Control *o = control(&other);
        CHECK(scmi::ok(scmi::probe(&o->perf)));
        other.settle();
        CHECK(o->perf.domain[1].count == 16 && o->perf.domain[1].levels[15].khz == 4320000);
        CHECK(other.count(0x13, 4) == 2 * (chunk == 1 ? 16 : 1));
    }
    printf("ok  probe: BASE/PERF versions, attributes, both domains' attributes and levels (rounds of 5+5+5+1, 1 and 16), the "
           "Linux shmem layout of every message, level get/set/limits by index\n");
}

static void testV4Indexing()
{
    FakeCpucp fake;
    fake.model.v4 = true; fake.model.indexing = true; fake.model.unsorted = true; fake.model.chunk = 7;
    cpufreq::Control *c = control(&fake);
    scmi::Perf *p = &c->perf;
    CHECK(scmi::ok(scmi::probe(p)));
    for (unsigned d = 0; d < 2; d++) {
        const scmi::Domain &dom = p->domain[d];
        CHECK(dom.indexing && dom.count == 16);
        for (unsigned i = 0; i < 16; i++) CHECK(dom.levels[i].khz == kTable[d][i] && dom.levels[i].value == 100 + i);   // sorted
    }
    CHECK(scmi::ok(scmi::levelSet(p, 0, 15)) && fake.sets(0) == std::vector<uint32_t>({115}));
    uint32_t index = 0, raw = 0;
    CHECK(scmi::ok(scmi::levelGet(p, 0, &index, &raw)) && raw == 115 && index == 15);
    printf("ok  PERF v4: five-word levels, level indexing (set/get by index value), unsorted answers sorted\n");
}

// What ArmCpu.cpp's thread does with a start: controlStart, then controlProbe each time a transient failure left it waiting, once that
// backoff passed (the simulated clock jumps there; the row says it waits). *tries: the attempts.
static bool startLikeThread(cpufreq::Control *c, uint32_t mode, uint32_t minMhz, uint64_t *now, unsigned *tries = nullptr)
{
    bool on = cpufreq::controlStart(c, mode, minMhz, *now);
    unsigned n = 1;
    while (!on && c->retry.waiting) {
        CHECK(c->retry.at > *now && !c->on && row(c).rfind("CPU scmi retry: ", 0) == 0);
        *now = c->retry.at;
        on = cpufreq::controlProbe(c, *now);
        n++;
        CHECK(n <= cpufreq::kRetryBudget);
    }
    if (tries) *tries = n;
    return on;
}

static void expectOff(FakeCpucp *fake, uint32_t mode, const char *why, unsigned sets = 0)
{
    cpufreq::Control *c = control(fake);
    uint64_t now = 1000 * MS;
    const bool on = startLikeThread(c, mode, 0, &now);
    fake->settle();
    if (on) fprintf(stderr, "started, expected \"%s\"\n", why);
    CHECK(!on && !c->on);
    if (strcmp(c->reason, why)) fprintf(stderr, "reason \"%s\", expected \"%s\"\n", c->reason, why);
    CHECK(!strcmp(c->reason, why));
    CHECK(row(c) == std::string("CPU scmi off: ") + why);
    CHECK(logged((std::string("cpufreq: off: ") + why).c_str()));
    CHECK(fake->sets(0).size() + fake->sets(1).size() == sets);
}

static void testFailures()
{
    // A platform that stops answering (the boot level's PERF_LEVEL_GET never answered, the channel held from then on): the start is tried
    // again until kSilentBudget missing answers in a row, the reason named after the first.
    { FakeCpucp f; f.faults.push_back({0x13, 8, 0, 1, kSilent}); expectOff(&f, 1, "timeout lvl get x6"); }
    { FakeCpucp f; Fault x{0x13, 7, 0, 1, kStatusCode}; x.code = -3; f.faults.push_back(x); expectOff(&f, 1, "status -3 lvl set", 1); }
    { FakeCpucp f; f.faults.push_back({0x13, 4, 1, 1, kCut}); expectOff(&f, 2, "short describe"); }
    { FakeCpucp f; f.faults.push_back({0x13, 4, 0, 1, kCountsDrift}); expectOff(&f, 2, "bad levels describe"); }
    { FakeCpucp f; f.faults.push_back({0x13, 4, 2, 1, kCountsDrift}); expectOff(&f, 2, "bad levels describe"); }
    { FakeCpucp f; f.faults.push_back({0x13, 4, 1, 1, kNoProgress}); expectOff(&f, 2, "bad levels describe"); }
    { FakeCpucp f; f.model.extraLevels = 17; expectOff(&f, 2, "levels>32 describe"); }
    // The error bit and an answer with another token (no answer of ours in time) are the transport's: retried, off once they persist.
    { FakeCpucp f; f.faults.push_back({0x10, 0, 0, 0, kErrorBit}); expectOff(&f, 1, "chan error base ver x6"); }
    { FakeCpucp f; f.faults.push_back({0x10, 0, 0, 0, kWrongToken}); expectOff(&f, 1, "timeout base ver x6"); }
    { FakeCpucp f; f.faults.push_back({0x13, 0, 0, 1, kWrongMessage}); expectOff(&f, 1, "bad header perf ver"); }
    { FakeCpucp f; f.faults.push_back({0x13, 1, 0, 1, kOversize}); expectOff(&f, 1, "short perf attr"); }
    { FakeCpucp f; Fault x{0x13, 3, 1, 1, kCut}; x.keep = 3; f.faults.push_back(x); expectOff(&f, 1, "short dom attr"); }
    {
        // Four words (no name) are enough for the domain attributes.
        FakeCpucp f;
        Fault x{0x13, 3, 1, 1, kCut};
        x.keep = 4;
        f.faults.push_back(x);
        cpufreq::Control *c = control(&f);
        CHECK(cpufreq::controlStart(c, cpufreq::kModeMax, 0, 0) && !c->perf.domain[1].name[0] && !strcmp(c->perf.domain[0].name, "perf0"));
    }
    { FakeCpucp f; f.faults.push_back({0x10, 0, 0, 1, kCut}); expectOff(&f, 1, "short base ver"); }
    { FakeCpucp f; f.model.baseVersion = 0; expectOff(&f, 1, "version base ver"); }
    { FakeCpucp f; f.model.baseVersion = 0x40000; expectOff(&f, 1, "version base ver"); }
    { FakeCpucp f; f.model.perfVersion = 0x50000; expectOff(&f, 1, "version perf ver"); }
    { FakeCpucp f; f.model.domains = 1; expectOff(&f, 1, "domains perf attr"); }
    { FakeCpucp f; f.model.noSet = true; expectOff(&f, 1, "no lvl set dom attr"); }
    { FakeCpucp f; Fault x{0x13, 3, 1, 1, kStatusCode}; x.code = -4; f.faults.push_back(x); expectOff(&f, 2, "status -4 dom attr"); }
    // A channel the platform never frees: waited for, never written over; tried again until kSilentBudget.
    {
        FakeCpucp f;
        f.mem[scmi::kChannelStatus / 4].store(0);
        expectOff(&f, 1, "chan busy base ver x6");
        CHECK(f.messages().empty() && f.mem[scmi::kHeader / 4].load() == 0);
    }
    // In interrupt context nothing is touched.
    {
        FakeCpucp f;
        gAllowed = false;
        expectOff(&f, 1, "irq context base ver");
        gAllowed = true;
        CHECK(f.messages().empty() && f.mem[scmi::kLength / 4].load() == 0);
    }
    // An answer after the timeout: the transfer fails; the next one waits for the channel and then goes through.
    {
        FakeCpucp f;
        Fault late{0x13, 8, 0, 1, kLate};
        late.lateMs = 120;
        f.faults.push_back(late);
        cpufreq::Control *c = control(&f);
        CHECK(scmi::ok(scmi::probe(&c->perf)));
        uint32_t index = 0, raw = 0;
        CHECK(scmi::levelGet(&c->perf, 0, &index, &raw).error == scmi::kTimeout);
        CHECK(!(f.mem[scmi::kChannelStatus / 4].load() & scmi::kChannelFree));   // still the platform's
        c->perf.channel.io.timeoutMs = 400;
        CHECK(scmi::ok(scmi::levelGet(&c->perf, 1, &index, &raw)) && index == 4);
        f.settle();
        const std::vector<Message> m = f.messages();
        CHECK(m.size() >= 2 && m[m.size() - 1].payload == std::vector<uint32_t>({1}) && m[m.size() - 2].payload == std::vector<uint32_t>({0}));
    }
    // A message too long for the channel is refused before anything is written.
    {
        FakeCpucp f;
        cpufreq::Control *c = control(&f);
        uint32_t words[scmi::kPayloadWords + 1] = {};
        CHECK(scmi::transfer(&c->perf.channel, 0x13, 7, words, scmi::kPayloadWords + 1, nullptr, 0, nullptr).error == scmi::kShort);
        CHECK(f.messages().empty());
    }
    // Without the platform side (not mapped) nothing happens.
    {
        cpufreq::Control c;
        memset(&c, 0, sizeof(c));
        CHECK(!cpufreq::controlStart(&c, 1, 0, 0) && !strcmp(c.reason, "not mapped"));
    }
    printf("ok  final failures switch the feature off with their reason (refusing statuses, short/oversized/inconsistent answers, wrong "
           "message, versions, domains, no level set, interrupt context, unmapped); missing answers that persist (timeouts, wrong token, "
           "error bit, a channel never free) after kSilentBudget start attempts; late answers\n");
}

static void testMax()
{
    FakeCpucp fake;
    cpufreq::Control *c = control(&fake);
    CHECK(cpufreq::controlStart(c, cpufreq::kModeMax, 0, 1000 * MS));
    fake.settle();
    CHECK(c->on && c->boot[0] == 8 && c->boot[1] == 4 && c->level[0] == 15 && c->level[1] == 15 && c->lowered == 0);
    CHECK(fake.sets(0) == std::vector<uint32_t>({3532800}) && fake.sets(1) == std::vector<uint32_t>({4320000}));
    CHECK(fake.count(0x13, 8) == 4);                                     // boot levels, then the check right after the set
    CHECK(logged("cpufreq: scmi base 2.0 perf 3.0 domains 2, d0 perf0 16 lv 384-3532MHz at lv8, d1 perf1 16 lv 1017-4320MHz at lv4, mode 1"));
    CHECK(row(c) == "CPU max 3532/4320MHz lv15/15 ld -/-% thr 0 err 0");
    // Every 5 s: the platform reports the top level, nothing is set again.
    CHECK(cpufreq::controlCheck(c, 6000 * MS));
    CHECK(fake.sets(1).size() == 1 && c->lowered == 0);
    // Thermal lowering: domain 1 capped at level 12 (3801.6 MHz). Counted, logged once with the limits, asked again (re-asserted).
    fake.model.cap[1] = 12;
    CHECK(cpufreq::controlCheck(c, 11000 * MS));
    fake.settle();
    CHECK(c->lowered == 1 && c->seen[1] == 12 && fake.sets(1).size() == 2 && c->on);
    CHECK(logged("cpufreq: d1 at lv12 3801MHz, asked lv15 4320MHz, limits lv0-12"));
    CHECK(row(c) == "CPU max 3532/3801MHz lv15/12! ld -/-% thr 1 err 0");
    const size_t lines = gLog.size();
    CHECK(cpufreq::controlCheck(c, 16000 * MS));
    CHECK(c->lowered == 2 && gLog.size() == lines);                     // the same level again: counted, not logged again
    // The cap lifts: back at the top, logged.
    fake.model.cap[1] = 15;
    CHECK(cpufreq::controlCheck(c, 21000 * MS));
    CHECK(c->lowered == 2 && c->seen[1] == 15 && logged("cpufreq: d1 back at lv15"));
    CHECK(row(c) == "CPU max 3532/4320MHz lv15/15 ld -/-% thr 2 err 0");
    // PERF_LIMITS_GET refused (optional): counted in err, not asked again, the feature stays on.
    Fault refused{0x13, 6, 0, 0, kStatusCode};
    refused.code = -1;
    fake.faults.push_back(refused);
    fake.model.cap[0] = 13;
    CHECK(cpufreq::controlCheck(c, 26000 * MS));
    CHECK(c->on && c->errors == 1 && !c->limits && logged("cpufreq: d0 at lv13 3072MHz, asked lv15 3532MHz, limits status -1"));
    CHECK(row(c) == "CPU max 3072/4320MHz lv13!/15 ld -/-% thr 3 err 1");
    fake.model.cap[0] = 15;
    CHECK(cpufreq::controlCheck(c, 31000 * MS) && c->on);
    // Its missing answer leaves the line without the limits and is retried like any other: a platform that never answers again (the
    // channel held from then on) ends it after kSilentBudget missing answers in a row (mode 1: the top level owed again each window).
    {
        FakeCpucp f;
        f.model.cap[0] = 10;
        f.faults.push_back({0x13, 6, 0, 1, kSilent});
        cpufreq::Control *o = control(&f);
        uint64_t t = 0;
        CHECK(cpufreq::controlStart(o, cpufreq::kModeMax, 0, t) && o->on);
        CHECK(logged("cpufreq: d0 at lv10 2400MHz, asked lv15 3532MHz, limits no answer") && o->retry.pending == 1);
        CHECK(logged("cpufreq: retry: timeout lim get d0 after lvl get d0, next in 20 ms"));
        unsigned windows = 0;
        uint64_t b[2] = {0, 0}, sp[2] = {MS, MS};
        while (o->on && windows < 20) { t += 1000 * MS; cpufreq::controlWindow(o, t, b, sp, 0, 0); windows++; }
        CHECK(!o->on && windows == 5 && !strcmp(o->reason, "timeout lim get x6") && row(o) == "CPU scmi off: timeout lim get x6");
    }
    // A LEVEL_GET failing in a later check switches it off.
    fake.faults.push_back({0x13, 8, 0, 1, kCut});
    CHECK(!cpufreq::controlCheck(c, 36000 * MS) && !c->on && row(c) == "CPU scmi off: short lvl get");
    CHECK(!cpufreq::controlCheck(c, 41000 * MS));                         // stays off
    printf("ok  pagani-cpufreq=1: top level on both domains, 5 s checks, thermal lowering counted/logged/re-asserted, limits, failures\n");
}

static void loads(uint64_t *busy, uint64_t *span, uint32_t p0, uint32_t p1, uint64_t window = 20 * MS)
{
    span[0] = span[1] = window;
    busy[0] = window * p0 / 100;
    busy[1] = window * p1 / 100;
}

// A steady work rate per domain (MHz the busiest CPU needs): busy is that share of the level in effect, all of the window above it.
static void work(const cpufreq::Control *c, uint64_t *busy, uint64_t *span, uint32_t mhz0, uint32_t mhz1, uint64_t window = 20 * MS)
{
    const uint32_t want[2] = {mhz0, mhz1};
    for (unsigned d = 0; d < 2; d++) {
        const uint64_t khz = c->perf.domain[d].levels[c->level[d]].khz;
        span[d] = window;
        busy[d] = (uint64_t)want[d] * 1000 >= khz ? window : window * want[d] * 1000 / khz;
    }
}

static void testGovernor()
{
    FakeCpucp fake;
    cpufreq::Control *c = control(&fake);
    uint64_t now = 1000 * MS;
    CHECK(cpufreq::controlStart(c, cpufreq::kModeGovernor, 0, now));
    const cpufreq::Domain &g0 = c->gov.domain[0], &g1 = c->gov.domain[1];
    CHECK(g0.floor == 3 && g0.gpu == 7 && g0.touch == 9 && g0.cap == 11 && g0.top == 15);
    CHECK(g1.floor == 3 && g1.gpu == 7 && g1.touch == 8 && g1.cap == 9 && g1.top == 15);
    CHECK(logged("cpufreq: governor floor 960/1689 MHz, touch 2227/2841 MHz for 250 ms, GPU frames 1785/2649 MHz, "
                 "heat caps 2745/3072 MHz, launch 2918/3513 MHz for 1500 ms"));
    CHECK(row(c) == "CPU 1996/1958MHz lv8/4 ld -/-% boost 0 thr 0 err 0 hot 0");
    CHECK(fake.sets(0).empty() && fake.sets(1).empty());
    uint64_t busy[2], span[2];
    unsigned touches = 7, hints = 3;
    auto step = [&]() { now += 20 * MS; CHECK(cpufreq::controlWindow(c, now, busy, span, touches, hints)); };
    auto at = [&](uint32_t l0, uint32_t l1) { return c->level[0] == l0 && c->level[1] == l1; };
    // Idle: the 1st and 2nd windows hold, the 3rd (3 of the last 4 wanted less) drops straight to the floor; then nothing is sent.
    loads(busy, span, 0, 0);
    step(); CHECK(at(8, 4));
    step(); CHECK(at(8, 4));
    step(); CHECK(at(3, 3));
    for (unsigned w = 0; w < 60; w++) step();
    CHECK(fake.sets(0) == std::vector<uint32_t>({960000}) && fake.sets(1) == std::vector<uint32_t>({1689600}));
    // Utilization (1.25 x the demand): 1200 / 1500 MHz of work. Domain 0 saturates the floor after quiet windows (a burst): 2 x 960 MHz,
    // at least the touch level (lv9) at once; at lv9 it is 54% busy, and once 3 of 4 windows want less it drops to what the last 4
    // windows need, the lowest level at or above 1.25 x 1200 MHz (lv6, 1555.2 MHz). Domain 1 (89% of 1689.6 MHz, not saturated)
    // rises once the 8-window average reaches 1.25 x 1500 MHz (lv4, 1958.4 MHz). Then both hold.
    work(c, busy, span, 1200, 1500); step(); CHECK(at(9, 3));
    for (unsigned w = 2; w <= 8; w++) {
        work(c, busy, span, 1200, 1500);
        step();
        CHECK(c->level[0] == (w < 4 ? 9u : 6u) && c->level[1] == (w < 8 ? 3u : 4u));
    }
    for (unsigned w = 0; w < 100; w++) { work(c, busy, span, 1200, 1500); step(); }
    CHECK(at(6, 4) && c->gov.domain[0].demand / 100 == 11999 && c->gov.domain[1].demand / 100 == 14999);   // kHz, busy rounded down
    CHECK(fake.sets(0) == std::vector<uint32_t>({960000, 2227200, 1555200}) && fake.sets(1) == std::vector<uint32_t>({1689600, 1958400}));
    CHECK(row(c).rfind("CPU 1555/1958MHz lv6/4 ", 0) == 0);
    // Load gone: 3 windows to drop (3 of 4), straight to the floor once the last 4 windows need no more.
    loads(busy, span, 0, 0);
    step(); step(); CHECK(at(6, 4));
    step(); CHECK(at(3, 3));
    for (unsigned w = 0; w < 10; w++) step();
    // A saturating burst after quiet windows: 2 x at its 1st window (lv9 = the touch level for domain 0, 2 x 1689.6 MHz = lv11 for
    // domain 1), 2 x again at its 2nd (the top). Held 3 windows after it ends, lv9 at the 4th (what the 160 ms average still needs),
    // the floor at the 7th.
    work(c, busy, span, 9000, 9000); step(); CHECK(at(9, 11));
    work(c, busy, span, 9000, 9000); step(); CHECK(at(15, 15));
    for (unsigned w = 0; w < 8; w++) { work(c, busy, span, 9000, 9000); step(); CHECK(at(15, 15)); }
    // Levels the governor raised above what the last check read are no lowering: no '!' on the row.
    CHECK(row(c).rfind("CPU 3532/4320MHz lv15/15 ", 0) == 0);
    loads(busy, span, 0, 0);
    for (unsigned w = 1; w <= 7; w++) {
        step();
        CHECK(c->level[0] == (w < 4 ? 15u : w < 7 ? 9u : 3u));
    }
    for (unsigned w = 0; w < 10; w++) step();
    CHECK(at(3, 3));
    // A saturated window when the windows before were busy is no burst: nothing until the 3rd saturated window in a row (the top).
    for (unsigned w = 0; w < 12; w++) { work(c, busy, span, 700, 700); step(); }
    CHECK(at(3, 3) && c->gov.domain[0].saturated == 0);
    work(c, busy, span, 9000, 700); step(); CHECK(at(3, 3));
    work(c, busy, span, 9000, 700); step(); CHECK(at(3, 3));
    work(c, busy, span, 9000, 700); step(); CHECK(at(15, 3) && c->gov.domain[0].saturated == 3 && !c->gov.domain[0].burst);
    loads(busy, span, 0, 0);
    for (unsigned w = 0; w < 14; w++) step();
    CHECK(at(3, 3));
    // A touch: the touch level (2227.2 / 2841.6 MHz) in the window it is seen, at 5% load; more touch frames extend it without a new
    // boost; it holds until 250 ms after the last touch frame, then 3 windows to drop (3 of 4) to the floor.
    loads(busy, span, 5, 5);
    touches++;
    step();
    CHECK(at(9, 8) && c->gov.boosts == 1 && fake.sets(0).back() == 2227200 && fake.sets(1).back() == 2841600);
    for (unsigned w = 0; w < 4; w++) { touches++; step(); CHECK(at(9, 8)); }
    const uint64_t lastTouch = now;
    CHECK(c->gov.boosts == 1 && c->gov.touchUntil == lastTouch + 250 * MS);
    for (unsigned w = 1; w <= 15; w++) {
        step();
        CHECK(c->level[0] == (now < lastTouch + 300 * MS ? 9u : 3u) && c->level[1] == (now < lastTouch + 300 * MS ? 8u : 3u));
    }
    CHECK(row(c).find(" boost 1 ") != std::string::npos);
    // Heavy load during a touch goes past the touch level (the load rules alone): the window it began in ran at the floor (2 x 960 MHz
    // is under the touch level), the next one at the touch level: 2 x 2227.2 MHz, the top.
    touches++;
    loads(busy, span, 100, 5);
    step();
    CHECK(c->level[0] == 9 && c->level[1] == 8 && c->gov.boosts == 2);
    step();
    CHECK(c->level[0] == 15 && c->level[1] == 8);
    loads(busy, span, 5, 5);
    for (unsigned w = 0; w < 20; w++) step();
    CHECK(at(3, 3));
    // GPU frames: submissions in 4 of the last 5 windows hold the middle level (1785.6 / 2649.6 MHz) at 5% load; 3 of 5 do not; the
    // floor 3 windows after it ends.
    for (unsigned w = 0; w < 3; w++) { hints++; step(); CHECK(at(3, 3)); }
    hints++;
    step();
    CHECK(at(7, 7) && c->gov.hints == 1 && fake.sets(0).back() == 1785600 && fake.sets(1).back() == 2649600);
    for (unsigned w = 0; w < 30; w++) { hints++; step(); CHECK(at(7, 7)); }
    step(); CHECK(at(7, 7));                                    // 4 of 5
    step(); step(); CHECK(at(7, 7));                            // 2 windows wanted less
    step(); CHECK(at(3, 3) && c->gov.hints == 1);               // the 3rd: the floor
    for (unsigned w = 0; w < 40; w++) { if (w % 2 == 0) hints++; step(); CHECK(at(3, 3)); }   // every other window: 3 of 5 at most
    CHECK(c->gov.hints == 1);
    // The 5 s checks ran from the windows meanwhile: the platform applied every level asked for, nothing lowered, no "back at".
    CHECK(c->lastCheck > 1000 * MS && c->lowered == 0 && !logged("back at"));
    CHECK(!logged("cpufreq: d0 at lv") && !logged("cpufreq: d1 at lv"));
    // A 5 s check finding thermal lowering: counted and logged, never set again by the governor.
    const size_t before = fake.sets(0).size();
    fake.model.cap[0] = 2;      // below the floor even
    now = c->lastCheck + 5000 * MS;
    CHECK(cpufreq::controlWindow(c, now, busy, span, touches, hints));
    CHECK(c->lowered == 1 && logged("cpufreq: d0 at lv2 748MHz, asked lv3 960MHz") && fake.sets(0).size() == before);
    CHECK(c->below[0] && !c->below[1] && row(c).find("lv2!/3 ") != std::string::npos);
    fake.model.cap[0] = 15;
    printf("ok  pagani-cpufreq=2: idle to the floor in 3 windows, 1.25 x utilization (lv6/lv4 for 1200/1500 MHz of work), bursts 2 x per "
           "saturated window to the top, the 3rd saturated window tops any run, 3-of-4 drops never below the last 4 windows' need, touch "
           "level 250 ms, GPU frames (4 of 5 windows) at the middle level, 5 s checks with thermal lowering\n");

    // The floor follows pagani-cpufreq-min=<MHz> (the touch and GPU-frame levels never under it); a failure mid-run asks both domains
    // back to their boot levels once.
    {
        FakeCpucp f;
        cpufreq::Control *o = control(&f);
        uint64_t t = 1000 * MS;
        CHECK(cpufreq::controlStart(o, cpufreq::kModeGovernor, 1500, t));
        CHECK(o->gov.domain[0].floor == 6 && o->gov.domain[1].floor == 3 && o->gov.domain[0].touch == 9 && o->gov.domain[0].gpu == 7);
        for (unsigned w = 0; w < 60; w++) {
            t += 20 * MS;
            loads(busy, span, 0, 0);
            CHECK(cpufreq::controlWindow(o, t, busy, span, 0, 0));
        }
        CHECK(o->level[0] == 6 && o->level[1] == 3);
        // BUSY at that set: retried in the next window, the level goes through, the feature stays on.
        Fault busyStatus{0x13, 7, 0, 1, kStatusCode};
        busyStatus.code = -6;
        f.setFaults({busyStatus});
        t += 20 * MS;
        loads(busy, span, 90, 0);
        CHECK(cpufreq::controlWindow(o, t, busy, span, 0, 0) && o->on && o->level[0] == 6 && o->retry.pending == 1);
        t += 20 * MS;
        CHECK(cpufreq::controlWindow(o, t, busy, span, 0, 0) && o->on && o->level[0] == 14 && o->retry.pending == 0);   // a burst: 2 x
        CHECK(f.sets(0).size() >= 2 && f.sets(0).back() == 3321600 && row(o).find(" err 1") != std::string::npos);
        for (unsigned w = 0; w < 60; w++) {
            t += 20 * MS;
            loads(busy, span, 0, 0);
            CHECK(cpufreq::controlWindow(o, t, busy, span, 0, 0));
        }
        CHECK(o->level[0] == 6 && o->level[1] == 3);
        // A final status (GENERIC_ERROR) at a set: off, both domains asked back to their boot levels.
        Fault generic{0x13, 7, 0, 1, kStatusCode};
        generic.code = -8;
        f.setFaults({generic});
        t += 20 * MS;
        loads(busy, span, 90, 0);
        CHECK(!cpufreq::controlWindow(o, t, busy, span, 0, 0) && !o->on);
        f.settle();
        CHECK(!strcmp(o->reason, "status -8 lvl set") && row(o) == "CPU scmi off: status -8 lvl set");
        CHECK(f.sets(0).back() == 1996800 && f.sets(1).back() == 1958400);          // back to the boot levels
        CHECK(logged("cpufreq: d0 back to boot lv8: ok") && logged("cpufreq: d1 back to boot lv4: ok"));
        CHECK(o->level[0] == 8 && o->level[1] == 4);
        const size_t sent = f.sets(0).size();
        CHECK(!cpufreq::controlWindow(o, t + 20 * MS, busy, span, 0, 0) && f.sets(0).size() == sent);   // off: nothing more
    }
    {
        FakeCpucp f;
        cpufreq::Control *o = control(&f);
        CHECK(cpufreq::controlStart(o, cpufreq::kModeGovernor, 2500, 1000 * MS));
        const cpufreq::Domain &x = o->gov.domain[0], &y = o->gov.domain[1];
        CHECK(x.floor == 11 && x.touch == 11 && x.gpu == 11 && x.cap == 11 && y.floor == 7 && y.touch == 8 && y.gpu == 7 && y.cap == 9);
        CHECK(logged("cpufreq: governor floor 2745/2649 MHz, touch 2745/2841 MHz for 250 ms, GPU frames 2745/2649 MHz, "
                     "heat caps 2745/3072 MHz, launch 2918/3513 MHz for 1500 ms"));
    }
    // The domain's rate limit (rate_limit_us) defers a change; the first one goes at once.
    {
        FakeCpucp f;
        f.model.rateLimitUs[0] = 50000;
        cpufreq::Control *o = control(&f);
        uint64_t t = 1000 * MS;
        CHECK(cpufreq::controlStart(o, cpufreq::kModeGovernor, 0, t));
        CHECK(o->gov.domain[0].rateLimit == 50 * MS);
        loads(busy, span, 0, 0);
        for (unsigned w = 0; w < 3; w++) { t += 20 * MS; CHECK(cpufreq::controlWindow(o, t, busy, span, 0, 0)); }
        CHECK(o->level[0] == 3);                                // the first change: at once
        for (unsigned w = 0; w < 10; w++) { t += 20 * MS; CHECK(cpufreq::controlWindow(o, t, busy, span, 0, 0)); }
        loads(busy, span, 100, 0);
        t += 20 * MS;
        CHECK(cpufreq::controlWindow(o, t, busy, span, 0, 0) && o->level[0] == 9);       // a burst: the touch level
        t += 20 * MS;
        CHECK(cpufreq::controlWindow(o, t, busy, span, 0, 0) && o->level[0] == 9);       // the top 20 ms later: deferred
        t += 20 * MS;
        CHECK(cpufreq::controlWindow(o, t, busy, span, 0, 0) && o->level[0] == 9);       // 40 ms: deferred
        t += 20 * MS;
        CHECK(cpufreq::controlWindow(o, t, busy, span, 0, 0) && o->level[0] == 15);      // 60 ms: past the 50 ms
    }
    printf("ok  governor floor from pagani-cpufreq-min (touch and GPU-frame levels at least the floor), failure mid-run back to the boot "
           "levels, the domain's rate limit\n");
}

// The GPU's heat (adreno830_thermal_state, pagani-cpufreq=2): while the GPU's thermal loop holds it below its top, domain 0 stays at or
// below 2745.6 MHz and domain 1 at or below 3072 MHz unless a touch boosts them; in the GPU's 95 C emergency the caps hold through touches
// too; the caps apply at once (past the down rule); the GPU-frame middle level lies under them; the caps go with the flag. The transitions
// are logged and counted ("hot" on the row); pagani-cpufreq=1 ignores the flags.
static void testHeat()
{
    FakeCpucp fake;
    cpufreq::Control *c = control(&fake);
    uint64_t now = 1000 * MS;
    CHECK(cpufreq::controlStart(c, cpufreq::kModeGovernor, 0, now));
    CHECK(c->gov.domain[0].cap == 11 && c->gov.domain[1].cap == 9 && c->gov.domain[0].gpu == 7 && c->gov.domain[1].gpu == 7);
    const cpufreq::GpuHeat cool = {450, cpufreq::kGpuHeatValid};
    const cpufreq::GpuHeat hot = {853, cpufreq::kGpuHeatValid | cpufreq::kGpuHeatLimiting};
    const cpufreq::GpuHeat burning = {962, cpufreq::kGpuHeatValid | cpufreq::kGpuHeatLimiting | cpufreq::kGpuHeatEmergency};
    uint64_t busy[2], span[2];
    unsigned touches = 0, hints = 0;
    auto window = [&](uint32_t p0, uint32_t p1, const cpufreq::GpuHeat &heat) {
        now += 20 * MS;
        loads(busy, span, p0, p1);
        CHECK(cpufreq::controlWindow(c, now, busy, span, touches, hints, heat));
    };
    // Load without heat: a burst from the boot levels, the top on both domains by its 2nd window.
    window(95, 95, cool);
    window(95, 95, cool);
    CHECK(c->level[0] == 15 && c->level[1] == 15 && c->gov.heatCaps == 0 && !c->gov.capped);
    // The GPU limited: both domains down to their caps in this window, load or not; logged once, counted once.
    window(95, 95, hot);
    fake.settle();
    CHECK(c->level[0] == 11 && c->level[1] == 9 && c->gov.heatCaps == 1 && c->gov.capped);
    CHECK(fake.sets(0).back() == 2745600 && fake.sets(1).back() == 3072000);
    CHECK(logged("cpufreq: GPU limited by heat at 85.3 C: d0 <= 2745 MHz, d1 <= 3072 MHz unless touched"));
    const size_t lines = gLog.size();
    for (unsigned w = 0; w < 25; w++) window(95, 95, hot);
    CHECK(c->level[0] == 11 && c->level[1] == 9 && c->gov.heatCaps == 1 && gLog.size() == lines);
    CHECK(row(c) == "CPU 2745/3072MHz lv11/9 ld -/-% boost 0 thr 0 err 0 hot 1");
    // A touch lifts the caps (the saturated load takes the top) until 100 ms after it, then the caps again at once.
    touches++;
    window(95, 95, hot);
    CHECK(c->level[0] == 15 && c->level[1] == 15 && !c->gov.capped && c->gov.boosts == 1);
    for (unsigned w = 0; w < 4; w++) window(95, 95, hot);
    CHECK(c->level[0] == 15 && c->level[1] == 15);
    window(95, 95, hot);
    CHECK(c->level[0] == 11 && c->level[1] == 9 && c->gov.capped);
    // The touch level's own hold (250 ms) does not lift them.
    for (unsigned w = 0; w < 8; w++) { window(95, 95, hot); CHECK(c->level[0] == 11 && c->level[1] == 9 && c->gov.capped); }
    // The GPU's emergency: the caps hold through a touch as well (logged once).
    window(95, 95, burning);
    CHECK(logged("cpufreq: GPU heat emergency at 96.2 C: the caps hold through touches"));
    touches++;
    window(95, 95, burning);
    CHECK(c->level[0] == 11 && c->level[1] == 9 && c->gov.capped && c->gov.boosts == 2 && c->gov.heatCaps == 1);
    for (unsigned w = 0; w < 20; w++) window(10, 10, burning);
    CHECK(c->level[0] <= 11 && c->level[1] <= 9);
    // Under the caps the GPU-frame middle level (7) holds while the load is low.
    for (unsigned w = 0; w < 30; w++) window(0, 0, hot);
    CHECK(c->level[0] == 3 && c->level[1] == 3);
    for (unsigned w = 0; w < 4; w++) { hints++; window(0, 0, hot); }
    CHECK(c->level[0] == 7 && c->level[1] == 7);
    // The flag clears: logged, the caps go; a saturating load after quiet windows: 2 x the middle level, the top.
    window(95, 95, cool);
    CHECK(c->level[0] == 15 && c->level[1] == 15 && !c->gov.capped && logged("cpufreq: GPU no longer limited by heat (45.0 C)"));
    // Limited again: a second episode.
    window(95, 95, hot);
    CHECK(c->level[0] == 11 && c->level[1] == 9 && c->gov.heatCaps == 2 && row(c).find(" hot 2") != std::string::npos);
    printf("ok  GPU heat (adreno830_thermal_state): caps 2745.6/3072 MHz at once, a touch lifts them except in the GPU's emergency, the "
           "GPU-frame middle level under them, logged and counted episodes, lifted with the flag\n");

    // pagani-cpufreq-min above a cap: the cap is the floor then.
    {
        FakeCpucp f;
        cpufreq::Control *o = control(&f);
        CHECK(cpufreq::controlStart(o, cpufreq::kModeGovernor, 3000, 1000 * MS));
        CHECK(o->gov.domain[0].floor == 13 && o->gov.domain[0].cap == 13 && o->gov.domain[1].floor == 9 && o->gov.domain[1].cap == 9);
        uint64_t t = 1000 * MS;
        t += 20 * MS;
        loads(busy, span, 90, 90);
        CHECK(cpufreq::controlWindow(o, t, busy, span, 0, 0, hot) && o->level[0] == 13 && o->level[1] == 9);
    }
    // pagani-cpufreq=1 ignores the GPU's heat: both domains stay at the top (the 5 s checks set it again if the platform lowers it).
    {
        FakeCpucp f;
        cpufreq::Control *o = control(&f);
        CHECK(cpufreq::controlStart(o, cpufreq::kModeMax, 0, 1000 * MS));
        uint64_t t = 1000 * MS;
        for (unsigned w = 0; w < 10; w++) {
            t += 20 * MS;
            loads(busy, span, 90, 90);
            CHECK(cpufreq::controlWindow(o, t, busy, span, 1, 1, burning));
        }
        f.settle();
        CHECK(o->level[0] == 15 && o->level[1] == 15 && f.sets(0).size() == 1 && f.sets(1).size() == 1);
        CHECK(row(o) == "CPU max 3532/4320MHz lv15/15 ld -/-% thr 0 err 0" && !logged("GPU limited"));
    }
    printf("ok  heat caps: pagani-cpufreq-min above a cap raises it to the floor; pagani-cpufreq=1 unchanged\n");
}

// Scenarios on a model of what the governor really sees: each domain's busiest CPU works through a queue (a steady rate, 120 Hz frames,
// bursts) at the level in effect, and XNU's idle time reaches the governor in 10 ms ticks (processor_info, domainBusy), windows of 20 ms
// plus up to 1 ms of sleep jitter. Every window goes through controlWindow and the fake CPUCP. Light load with taps (the case measured
// on the 13T: 10%/8% load, both clusters at the top), a heavy burst after quiet, a steady medium load with 120 Hz frames, idle from the
// top, GPU frames with and without touches, and a heavy load while the GPU is limited by heat.
struct Workload {
    struct Job { double left, due; bool frame; };
    struct Core { std::deque<Job> queue; double idleNs = 0; };
    Core core[cpufreq::kCpus];                  // CPUs 0-5 domain 0, CPUs 6-7 domain 1 (MPIDR Aff2), the first of each the busiest
    cpufreq::CpuTicks before[cpufreq::kCpus];
    unsigned framesLate = 0, framesDone = 0;
    double burstDone[2] = {-1, -1};
    uint64_t rngState = 0x9e3779b97f4a7c15ull;
    double uniform()
    {
        rngState ^= rngState << 13; rngState ^= rngState >> 7; rngState ^= rngState << 17;
        return (rngState >> 11) * 0x1p-53;
    }
    void drop() { for (auto &k : core) k.queue.clear(); }   // the work left over from a scene that asked for more than the top
    static unsigned domainOf(unsigned cpu) { return cpu < 6 ? 0 : 1; }
    static unsigned first(unsigned d) { return d ? 6 : 0; }
    Workload() {
        for (unsigned i = 0; i < cpufreq::kCpus; i++) {
            core[i].idleNs = uniform() * 1e7;     // each CPU's idle ticks at its own phase
            before[i] = {true, true, domainOf(i), (uint32_t)(core[i].idleNs / 1e7)};
        }
    }
};
struct Scene {
    double rate[2] = {0, 0};            // MHz of steady work on the busiest CPU of each domain
    double others[2] = {0, 0};          // MHz of steady work on each of the domain's other CPUs
    double frameCost[2] = {0, 0};       // MHz x ms of work per 120 Hz frame on the busiest CPU (0: none)
    bool touching = false, submitting = false;
    uint32_t heat = 0;
    uint32_t procs = 0;                 // the count of process starts (LaunchInput::procs)
};

static void runScene(cpufreq::Control *c, Workload *m, uint64_t *now, double ms, const std::function<Scene(double)> &scene,
                     unsigned *touches, unsigned *hints, const std::function<void(double, const Scene &)> &after, double *t0)
{
    const double end = *t0 + ms;
    double t = *t0, windowStart = *t0, nextWindow = *t0 + 20 + m->uniform(), nextFrame = *t0, nextTouch = *t0;
    const double step = 0.05;
    while (t < end) {
        const Scene sc = scene(t - *t0);
        if (sc.frameCost[0] > 0 || sc.frameCost[1] > 0 || sc.submitting) {
            if (t >= nextFrame) {
                for (unsigned d = 0; d < 2; d++)
                    if (sc.frameCost[d] > 0) m->core[Workload::first(d)].queue.push_back({sc.frameCost[d], t + 8.333, true});
                if (sc.submitting) (*hints)++;
                nextFrame += 8.333;
            }
        } else {
            nextFrame = t;
        }
        if (sc.touching && t >= nextTouch) { (*touches)++; nextTouch = t + 8; }
        for (unsigned i = 0; i < cpufreq::kCpus; i++) {
            const unsigned d = Workload::domainOf(i);
            Workload::Core &k = m->core[i];
            const double rate = i == Workload::first(d) ? sc.rate[d] : sc.others[d];
            if (rate > 0) {
                if (!k.queue.empty() && !k.queue.back().frame && k.queue.back().due < 0) k.queue.back().left += rate * step;
                else k.queue.push_back({rate * step, -1, false});
            }
            const double mhz = c->perf.domain[d].levels[c->level[d]].khz / 1000.0;
            double room = mhz * step, used = 0;
            while (room > 1e-9 && !k.queue.empty()) {
                Workload::Job &j = k.queue.front();
                const double take = j.left < room ? j.left : room;
                j.left -= take; room -= take; used += take;
                if (j.left <= 1e-9) {
                    if (j.frame) { m->framesDone++; if (t + used / mhz > j.due) m->framesLate++; }
                    else if (j.due == -2) m->burstDone[d] = t + used / mhz;
                    k.queue.pop_front();
                }
            }
            k.idleNs += (1.0 - used / (mhz * step)) * step * 1e6;
        }
        t += step;
        if (t >= nextWindow) {
            // What ArmCpu.cpp samples: each CPU's idle time in 10 ms ticks (processor_info), then cpuWindows over the window.
            cpufreq::CpuTicks ticksNow[cpufreq::kCpus];
            for (unsigned i = 0; i < cpufreq::kCpus; i++)
                ticksNow[i] = {true, true, Workload::domainOf(i), (uint32_t)(m->core[i].idleNs / 1e7)};
            const uint64_t span = (uint64_t)((t - windowStart) * 1e6);
            cpufreq::CpuWindow cpus[cpufreq::kCpus];
            uint32_t online[2];
            cpufreq::cpuWindows(ticksNow, m->before, cpufreq::kCpus, span, 10 * MS, cpus, online);
            CHECK(online[0] == 6 && online[1] == 2);
            for (unsigned i = 0; i < cpufreq::kCpus; i++) m->before[i] = ticksNow[i];
            *now += span;
            CHECK(cpufreq::controlWindowCpus(c, *now, cpus, cpufreq::kCpus, *touches, *hints,
                                             cpufreq::GpuHeat{sc.heat ? 900 : 450, sc.heat}, cpufreq::ScreenInput{0, 0},
                                             cpufreq::LaunchInput{sc.procs, 0}));
            after(t - *t0, sc);
            windowStart = t;
            nextWindow = t + 20 + m->uniform();
        }
    }
    *t0 = end;
}

static void addBurst(Workload *m, unsigned d, double work) { m->core[Workload::first(d)].queue.push_back({work, -2, false}); }

static void testScenarios()
{
    FakeCpucp fake;
    cpufreq::Control *c = control(&fake);
    uint64_t now = 1000 * MS;
    CHECK(cpufreq::controlStart(c, cpufreq::kModeGovernor, 0, now));
    Workload m;
    unsigned touches = 0, hints = 0;
    double t0 = 0;
    auto mhz = [&](unsigned d) { return c->perf.domain[d].levels[c->level[d]].khz / 1000; };
    // 1. Idle from the top: the floor within 4 windows.
    {
        uint32_t top[2] = {15, 15};
        for (unsigned d = 0; d < 2; d++) CHECK(cpufreq::setLevel(c, d, top[d], now));
        unsigned windows = 0, atFloor = 0;
        runScene(c, &m, &now, 400, [](double) { Scene s; s.rate[0] = s.rate[1] = 10; return s; }, &touches, &hints,
                 [&](double, const Scene &) { windows++; if (c->level[0] == 3 && c->level[1] == 3 && !atFloor) atFloor = windows; }, &t0);
        CHECK(atFloor && atFloor <= 4 && c->level[0] == 3 && c->level[1] == 3);
        printf("    idle from the top: the floor after %u windows\n", atFloor);
    }
    // 2. Light load with taps (the measured case: 10%/8% at 3.5 GHz, about 350/280 MHz of work): a tap every 600 ms (80 ms of touch
    //    frames), 250 ms of 120 Hz frames after each (GPU submissions, 0.6/0.4 ms of CPU work per frame at 2 GHz). Never the top; the
    //    touch level within one window of the first touch frame; no late frame; the floor again between the taps.
    {
        double sumMhz[2] = {0, 0}, firstTouch = -1, touchLevel = -1;
        unsigned windows = 0, top = 0, floorBetween = 0;
        runScene(c, &m, &now, 3600, [](double t) {
            Scene s;
            s.rate[0] = 250; s.rate[1] = 200;
            const double phase = t < 300 ? -1 : fmod(t - 300, 600);
            s.touching = phase >= 0 && phase < 80;
            if (phase >= 10 && phase < 260) { s.frameCost[0] = 1200; s.frameCost[1] = 1040; s.submitting = true; }
            return s;
        }, &touches, &hints, [&](double t, const Scene &s) {
            windows++;
            for (unsigned d = 0; d < 2; d++) sumMhz[d] += mhz(d);
            top += c->level[0] == 15 || c->level[1] == 15;
            if (s.touching && firstTouch < 0) firstTouch = t;
            if (firstTouch >= 0 && touchLevel < 0 && c->level[0] >= 9 && c->level[1] >= 8) touchLevel = t;
            const double phase = t < 300 ? -1 : fmod(t - 300, 600);
            if (phase > 560 && c->level[0] == 3 && c->level[1] == 3) floorBetween++;
        }, &t0);
        CHECK(top == 0 && touchLevel >= 0 && touchLevel - firstTouch <= 21 && m.framesLate == 0 && floorBetween >= 5);
        CHECK(sumMhz[0] / windows < 1800 && sumMhz[1] / windows < 2500);
        printf("    light load with taps: average %.0f/%.0f MHz, never the top, %u late frames\n", sumMhz[0] / windows, sumMhz[1] / windows,
               m.framesLate);
    }
    // 3. A heavy burst after quiet: 600 Mcycles on domain 0 (170 ms at the top), 400 Mcycles on domain 1. The top by the 3rd window
    //    after it begins (the 1st holds part of it only); done within 45 ms of the time at the top; the floor within 200 ms after it.
    {
        runScene(c, &m, &now, 300, [](double) { Scene s; s.rate[0] = s.rate[1] = 10; return s; }, &touches, &hints,
                 [](double, const Scene &) {}, &t0);
        addBurst(&m, 0, 600000); addBurst(&m, 1, 400000);
        m.burstDone[0] = m.burstDone[1] = -1;
        const double start = t0;
        double topAt[2] = {-1, -1}, floorAt = -1;
        unsigned window = 0;
        runScene(c, &m, &now, 1000, [](double) { Scene s; s.rate[0] = s.rate[1] = 10; return s; }, &touches, &hints,
                 [&](double t, const Scene &) {
                     window++;
                     for (unsigned d = 0; d < 2; d++) if (topAt[d] < 0 && c->level[d] == 15) topAt[d] = window;
                     if (m.burstDone[0] >= 0 && m.burstDone[1] >= 0 && floorAt < 0 && c->level[0] == 3 && c->level[1] == 3) floorAt = t;
                 }, &t0);
        const double took0 = m.burstDone[0] - start, took1 = m.burstDone[1] - start;
        CHECK(topAt[0] > 0 && topAt[0] <= 3 && topAt[1] > 0 && topAt[1] <= 3);
        CHECK(took0 < 600000 / 3532.8 + 45 && took1 < 400000 / 4320.0 + 45);     // at most about two windows lost
        CHECK(floorAt > 0 && floorAt - (m.burstDone[0] - start) < 200);
        printf("    burst after quiet: the top in window %.0f/%.0f, done in %.0f/%.0f ms (%.0f/%.0f ms at the top), the floor %.0f ms "
               "after it\n",
               topAt[0], topAt[1], took0, took1, 600000 / 3532.8, 400000 / 4320.0, floorAt - (m.burstDone[0] - start));
    }
    // 4. Steady medium load with 120 Hz frames: 1200/1500 MHz of work (half steady, half in frames). The level for 1.25 x that is lv6/lv4
    //    (1555.2/1958.4 MHz): after 300 ms the domains stay within one level below and two above it, never at the top, at most 15 level
    //    changes a second, no late frame (the first 300 ms climb from the floor).
    {
        unsigned windows = 0, inside = 0, top = 0, changes = 0, late = 0;
        uint32_t last[2] = {c->level[0], c->level[1]};
        runScene(c, &m, &now, 3000, [](double) {
            Scene s;
            s.rate[0] = 600; s.rate[1] = 750; s.frameCost[0] = 5000; s.frameCost[1] = 6250;
            return s;
        }, &touches, &hints, [&](double t, const Scene &) {
            if (t < 300) { last[0] = c->level[0]; last[1] = c->level[1]; late = m.framesLate; return; }
            windows++;
            inside += c->level[0] >= 5 && c->level[0] <= 8 && c->level[1] >= 3 && c->level[1] <= 6;
            top += c->level[0] == 15 || c->level[1] == 15;
            for (unsigned d = 0; d < 2; d++) { changes += c->level[d] != last[d]; last[d] = c->level[d]; }
        }, &t0);
        CHECK(top == 0 && inside * 10 >= windows * 9 && changes <= 15 * 2 * 27 / 10 && m.framesLate == late);
        printf("    steady medium load: %u%% of windows within lv5-8/lv3-6, %u changes in 2.7 s over both domains, never the top, no late "
               "frame after the first 300 ms\n", inside * 100 / windows, changes);
    }
    // 5. GPU frames without touches: 120 Hz submissions with light CPU work hold the middle level (from the 5th window on) until 160 ms
    //    after the frames stop; two submissions a second never do.
    {
        runScene(c, &m, &now, 400, [](double) { Scene s; s.rate[0] = s.rate[1] = 10; return s; }, &touches, &hints,
                 [](double, const Scene &) {}, &t0);
        unsigned window = 0, held = 0, during = 0, sparseMid = 0;
        double released = -1;
        runScene(c, &m, &now, 2500, [](double t) {
            Scene s;
            s.rate[0] = s.rate[1] = 30;
            if (t < 1500) { s.submitting = true; s.frameCost[0] = 2400; s.frameCost[1] = 1560; }
            return s;
        }, &touches, &hints, [&](double t, const Scene &) {
            window++;
            if (t < 1500 && window > 5) { during++; held += c->level[0] >= 7 && c->level[1] >= 7; }
            if (t >= 1500 && released < 0 && c->level[0] < 7 && c->level[1] < 7) released = t - 1500;
        }, &t0);
        CHECK(held == during && released >= 0 && released <= 160);
        runScene(c, &m, &now, 2000, [](double t) { Scene s; s.rate[0] = s.rate[1] = 30; s.submitting = fmod(t, 500) < 8; return s; },
                 &touches, &hints, [&](double, const Scene &) { sparseMid += c->level[0] >= 7 || c->level[1] >= 7; }, &t0);
        CHECK(sparseMid == 0);
        printf("    GPU frames: the middle level held through %u windows of 120 Hz frames, released %.0f ms after them; sparse "
               "submissions never\n", during, released);
    }
    // 6. Heavy load while the GPU is limited by heat: the caps in every window that is not touched; a touch lifts them for 100 ms after
    //    it, the emergency holds them through touches.
    {
        unsigned capped = 0, over = 0, lifted = 0;
        runScene(c, &m, &now, 2000, [](double t) {
            Scene s;
            s.rate[0] = 5000; s.rate[1] = 6000;
            s.heat = cpufreq::kGpuHeatValid | cpufreq::kGpuHeatLimiting | (t >= 1200 ? (uint32_t)cpufreq::kGpuHeatEmergency : 0u);
            s.touching = (t >= 500 && t < 600) || (t >= 1500 && t < 1600);
            return s;
        }, &touches, &hints, [&](double, const Scene &) {
            if (c->gov.capped) { capped++; over += c->level[0] > 11 || c->level[1] > 9; }
            else lifted += c->level[0] == 15 && c->level[1] == 15;
        }, &t0);
        CHECK(over == 0 && capped > 60 && lifted >= 5 && lifted <= 12);
        printf("    heavy load under the GPU's heat: %u capped windows all within 2745.6/3072 MHz, %u windows lifted by a touch\n",
               capped, lifted);
    }
    // 7. A parallel medium load: every CPU of domain 0 at 1200 MHz of work and both of domain 1 at 1500 (independent idle-tick phases).
    //    Each CPU is averaged on its own: the domains stay near what one CPU needs (lv6/lv4 for 1.25 x), never at the top. (The busiest of
    //    six CPUs taken per 20 ms window would read 100% whenever any of them crossed no idle tick: about 2.3 GHz and the top 14% of the
    //    time in simulation.)
    {
        m.drop();
        unsigned windows = 0, top = 0;
        double sumMhz[2] = {0, 0};
        runScene(c, &m, &now, 3000, [](double) {
            Scene s;
            s.rate[0] = s.others[0] = 1200; s.rate[1] = s.others[1] = 1500;
            return s;
        }, &touches, &hints, [&](double t, const Scene &) {
            if (t < 300) return;
            windows++;
            top += c->level[0] == 15 || c->level[1] == 15;
            for (unsigned d = 0; d < 2; d++) sumMhz[d] += mhz(d);
        }, &t0);
        CHECK(top == 0 && sumMhz[0] / windows < 1900 && sumMhz[1] / windows < 2300);
        printf("    parallel medium load on all 8 CPUs: average %.0f/%.0f MHz (1.25 x one CPU's need: 1555/1958), never the top\n",
               sumMhz[0] / windows, sumMhz[1] / windows);
    }
    printf("ok  scenarios through the fake CPUCP with 10 ms idle ticks (cpuWindows, controlWindowCpus): idle, light load with taps, burst, "
           "steady medium load, GPU frames, heat, a parallel load\n");
}

// The screen-off policy (pagani-screenoff bit 0, Control::screen): the display off (pagani_screen_state bit 0) for kScreenOffDelayMs
// gives the lowest floor and the caps 748.8 / 1209.6 MHz (lv2 / lv1, at once); a touch lifts them for 300 ms without the touch level
// (nothing is shown), a key report lifts them with the touch level, GPU frames lift them with the GPU-frame level; the display back
// restores the floor in that window and holds the touch level for 500 ms; a short off restarts the delay; without the bit nothing
// changes. Logged once per screen-off period.
static void testScreenOff()
{
    FakeCpucp fake;
    cpufreq::Control *c = control(&fake);
    c->screen = true;
    uint64_t now = 1000 * MS;
    CHECK(cpufreq::controlStart(c, cpufreq::kModeGovernor, 0, now));
    const cpufreq::Domain &g0 = c->gov.domain[0], &g1 = c->gov.domain[1];
    CHECK(c->gov.screenPolicy && g0.offCap == 2 && g1.offCap == 1 && g0.floor == 3 && g1.floor == 3);
    CHECK(logged("cpufreq: screen-off policy: 1000 ms after the display goes off 384-748/1017-1209 MHz, lifted for 300 ms by a touch, "
                 "a key or GPU frames; the display back: touch level for 500 ms"));
    uint64_t busy[2], span[2];
    uint32_t touches = 3, hints = 5, keys = 9, state = 0;
    auto step = [&]() {
        now += 20 * MS;
        CHECK(cpufreq::controlWindow(c, now, busy, span, touches, hints, cpufreq::GpuHeat{0, 0}, cpufreq::ScreenInput{state, keys}));
    };
    auto at = [&](uint32_t l0, uint32_t l1) { return c->level[0] == l0 && c->level[1] == l1; };
    loads(busy, span, 0, 0);
    for (unsigned w = 0; w < 60; w++) step();
    CHECK(at(3, 3) && !c->gov.screenOff && c->gov.offs == 0);
    // until(end): the windows before `end` stay lifted at (l0, l1); the window at `end` is lowered: the caps at once (lv2 / lv1); then the
    // 3-of-4 down rule takes 3 more windows to the lowest level.
    auto until = [&](uint64_t end, uint32_t l0, uint32_t l1) {
        while (now + 20 * MS < end) { step(); CHECK(!c->gov.lowered && at(l0, l1)); }
        step();
        CHECK(now == end && c->gov.lowered && at(2, 1));
    };
    auto settleLow = [&]() {
        step(); CHECK(at(2, 1));
        step(); CHECK(at(2, 1));
        step(); CHECK(at(0, 0));
    };
    // Off: the screen-on governor for 1000 ms (the window that saw it and 49 more), then the caps at once and the lowest floor through
    // the 3-of-4 down rule.
    state = 1;
    step();
    const uint64_t off = now;
    CHECK(c->gov.screenOff && c->gov.offs == 1 && c->gov.offStart == off && !c->gov.lowered && at(3, 3));
    until(off + 1000 * MS, 3, 3);
    CHECK(logged("cpufreq: screen off 1.0 s: d0 384-748 MHz, d1 1017-1209 MHz"));
    settleLow();
    CHECK(fake.sets(0).back() == 384000 && fake.sets(1).back() == 1017600);
    for (unsigned w = 0; w < 20; w++) { step(); CHECK(at(0, 0) && c->gov.lowered); }
    // Heavy load while lowered: the burst rules ask for the touch level and then the top; the caps hold at once (lv2 / lv1), and once the
    // load is gone the down rule ends at the lowest level again.
    for (unsigned w = 0; w < 20; w++) {
        work(c, busy, span, 9000, 9000);
        step();
        CHECK(at(2, 1) && c->gov.lowered);
    }
    loads(busy, span, 0, 0);
    for (unsigned w = 0; w < 20; w++) { step(); CHECK(c->level[0] <= 2 && c->level[1] <= 1 && c->gov.lowered); }
    CHECK(at(0, 0));
    // A touch while off: lifted for 300 ms back to the screen-on floor, without the touch level, then lowered again.
    const uint32_t boosts = c->gov.boosts;
    touches++;
    step();
    const uint64_t touched = now;
    CHECK(!c->gov.lowered && at(3, 3) && c->gov.lifts == 1 && c->gov.periodLifts == 1 && c->gov.boosts == boosts + 1);
    until(touched + 300 * MS, 3, 3);
    settleLow();
    // A key report while off (the power key): lifted with the touch level (2227.2 / 2841.6 MHz) for 300 ms, then the caps at once.
    keys++;
    step();
    const uint64_t keyed = now;
    CHECK(!c->gov.lowered && at(9, 8) && c->gov.lifts == 2);
    until(keyed + 300 * MS, 9, 8);
    settleLow();
    // GPU frames while off (4 of the last 5 windows): lifted with the GPU-frame level (1785.6 / 2649.6 MHz) while they last and 300 ms
    // after the last window that had them; the 3-of-4 down rule takes the level to the floor meanwhile.
    for (unsigned w = 0; w < 3; w++) { hints++; step(); CHECK(at(0, 0) && c->gov.lowered); }
    hints++;
    step();
    CHECK(!c->gov.lowered && c->gov.frames && at(7, 7) && c->gov.lifts == 3);
    for (unsigned w = 0; w < 10; w++) { hints++; step(); CHECK(at(7, 7)); }
    step();
    CHECK(c->gov.frames && at(7, 7));                           // 4 of the last 5 windows: still frames
    const uint64_t lastFrames = now;
    step();
    CHECK(!c->gov.frames && !c->gov.lowered && at(7, 7));
    while (now + 20 * MS < lastFrames + 300 * MS) { step(); CHECK(!c->gov.lowered && c->level[0] >= 3 && c->level[1] >= 3); }
    CHECK(at(3, 3));
    step();
    CHECK(now == lastFrames + 300 * MS && c->gov.lowered && at(2, 1));
    settleLow();
    CHECK(c->gov.periodLifts == 3);
    // The display back: the screen-on floor and the touch level in the window that sees it, for 500 ms, then the floor.
    state = 2;
    step();
    const uint64_t on = now;
    CHECK(!c->gov.screenOff && !c->gov.lowered && at(9, 8) && c->gov.ons == 1);
    CHECK(fake.sets(0).back() == 2227200 && fake.sets(1).back() == 2841600);
    char want[160];
    const uint64_t tenths = (on - off + 50 * MS) / (100 * MS);     // to the nearest tenth, as logged
    snprintf(want, sizeof(want), "cpufreq: screen on after %llu.%llu s off: lowered ", (unsigned long long)(tenths / 10),
             (unsigned long long)(tenths % 10));
    CHECK(logged(want) && logged(" MHz on average, 3 lifts"));
    while (now < on + 500 * MS) { step(); CHECK(at(9, 8)); }
    step(); step();
    CHECK(at(3, 3));
    // A key with the display on: no screen-off rule (no touch level, no lift).
    keys++;
    step();
    CHECK(at(3, 3) && c->gov.lifts == 3);
    // An off shorter than the delay: never lowered; back on: the touch level.
    state = 3;
    for (unsigned w = 0; w < 30; w++) { step(); CHECK(!c->gov.lowered && c->level[0] >= 3 && c->level[1] >= 3); }
    state = 4;
    step();
    CHECK(at(9, 8) && c->gov.offs == 2 && c->gov.ons == 2);
    CHECK(logged("cpufreq: screen on after 0.6 s off: lowered 0.0 s, 0 lifts"));
    for (unsigned w = 0; w < 40; w++) step();
    CHECK(at(3, 3));
    // Off, then on and off again between two windows (the count moved by 2, still odd): the same period, its delay restarting there.
    state = 5;
    for (unsigned w = 0; w < 30; w++) step();
    CHECK(c->gov.offs == 3 && !c->gov.lowered);
    state = 7;
    step();
    const uint64_t again = now;
    CHECK(c->gov.offs == 3 && c->gov.ons == 2 && c->gov.screenOff && c->gov.offSince == again && !c->gov.lowered);
    until(again + 1000 * MS, 3, 3);
    settleLow();
    state = 8;
    step();
    CHECK(at(9, 8) && c->gov.ons == 3);
    printf("ok  screen-off policy (pagani-screenoff bit 0): 1000 ms after the display goes off the lowest floor and caps lv2/lv1 "
           "(748.8/1209.6 MHz) at once, a touch lifts them for 300 ms without the touch level, a key with it, GPU frames with the "
           "GPU-frame level; the display back: floor and touch level at once for 500 ms; short offs, repeated offs, logs\n");

    // Without the bit (Control::screen false) the same inputs change nothing: a twin governor fed no screen at all sets the same levels.
    {
        FakeCpucp fa, fb;
        cpufreq::Control *a = control(&fa), *b = control(&fb);
        uint64_t t = 1000 * MS;
        CHECK(cpufreq::controlStart(a, cpufreq::kModeGovernor, 0, t) && cpufreq::controlStart(b, cpufreq::kModeGovernor, 0, t));
        CHECK(!a->gov.screenPolicy && !logged("screen-off policy"));
        uint32_t s = 0, k = 0, tc = 0, h = 0;
        uint64_t state2 = 99;
        auto rnd = [&](uint64_t n) { state2 = state2 * 6364136223846793005ull + 1442695040888963407ull; return (state2 >> 33) % n; };
        for (unsigned w = 0; w < 3000; w++) {
            t += 20 * MS;
            if (rnd(50) == 0) s++;
            if (rnd(30) == 0) k++;
            if (rnd(20) == 0) tc++;
            if (rnd(3) == 0) h++;
            loads(busy, span, (uint32_t)rnd(101), (uint32_t)rnd(101));
            CHECK(cpufreq::controlWindow(a, t, busy, span, tc, h, cpufreq::GpuHeat{0, 0}, cpufreq::ScreenInput{s, k}));
            CHECK(cpufreq::controlWindow(b, t, busy, span, tc, h));
            CHECK(a->level[0] == b->level[0] && a->level[1] == b->level[1] && a->level[0] >= 3 && a->level[1] >= 3);
        }
        CHECK(!a->gov.screenOff && a->gov.offs == 0 && !logged("cpufreq: screen"));
    }
    printf("ok  screen-off policy off (no pagani-screenoff bit 0): 3000 windows with screen and key changes set the levels of a "
           "governor without them, never below the floor\n");
}

// Random windows with the screen-off policy on (screen offs and ons, touches, keys, GPU submissions, heat and loads at random): the
// target lies between the lowest level and the screen-off cap while lowered, between the floor and the top otherwise (under the heat
// cap while capped); lowered only with the display off for at least the delay and no input lifting it; the touch level while the
// display just came back (unless capped by heat); and a display off and quiet for the delay and 4 windows more ends at the lowest
// level.
static void testScreenProperties()
{
    scmi::Perf perf;
    memset(&perf, 0, sizeof(perf));
    for (unsigned d = 0; d < 2; d++) {
        perf.domain[d].count = 16;
        for (unsigned i = 0; i < 16; i++) perf.domain[d].levels[i].khz = kTable[d][i];
    }
    static cpufreq::Governor g;
    uint64_t seed = 777;
    auto next = [&](uint64_t n) { seed = seed * 6364136223846793005ull + 1442695040888963407ull; return (seed >> 33) % n; };
    unsigned checked = 0, loweredChecks = 0, quietOffs = 0;
    for (unsigned run = 0; run < 30; run++) {
        const uint32_t boot[2] = {(uint32_t)next(16), (uint32_t)next(16)};
        memset(&g, 0, sizeof(g));
        cpufreq::governorInit(&g, perf, run % 3 == 0 ? (uint32_t)next(3000) : 0, boot, 1000000000ull, 0);
        g.screenPolicy = true;
        uint64_t now = 1000 * MS, quietFrom = 0;
        uint32_t touch = 0, gpu = 0, heat = 0, state = 0, keys = 0, quiet = 0, procs = 0;
        for (unsigned w = 0; w < 6000; w++) {
            cpufreq::CpuWindow cpus[cpufreq::kCpus];
            const uint64_t span = 18 * MS + next(4 * MS);
            if (!quiet && next(300) == 0) { quiet = 80; state |= 1; state += 2; quietFrom = 0; }   // off and quiet for 80 windows
            if (!quiet && next(60) == 0) state += 1;                                             // a toggle
            for (unsigned i = 0; i < cpufreq::kCpus; i++) {
                cpus[i].domain = i < 6 ? 0 : 1;
                cpus[i].span = next(20) == 0 ? 0 : span;
                cpus[i].busy = quiet ? 0 : next(4) == 0 ? span : next(span + 1);
            }
            if (!quiet && next(8) == 0) touch++;
            if (!quiet && next(25) == 0) keys++;
            if (!quiet && next(3)) gpu++;
            if (!quiet && next(40) == 0) procs++;
            if (next(80) == 0) heat = (uint32_t)next(8);
            if (quiet) heat = 0;
            now += span;
            uint32_t target[2];
            cpufreq::governorStep(&g, now, cpus, cpufreq::kCpus, touch, gpu, heat, target, cpufreq::ScreenInput{state, keys},
                                  cpufreq::LaunchInput{procs, 0});
            CHECK(!(g.launching && g.screenOff));
            for (unsigned d = 0; d < 2; d++) {
                const cpufreq::Domain &x = g.domain[d];
                CHECK(target[d] <= x.top);
                if (g.lowered) {
                    CHECK(target[d] <= x.offCap && g.screenOff && now - g.offSince >= 1000 * MS && now >= g.inputUntil);
                    loweredChecks++;
                } else {
                    CHECK(target[d] >= x.floor);
                    if (g.capped) CHECK(target[d] <= x.cap);
                    if (!g.screenOff && now < g.onUntil && !g.capped) CHECK(target[d] >= x.touch);
                }
                if (target[d] != x.level) cpufreq::governorSet(&g, d, target[d], now);
                checked++;
            }
            if (quiet) {
                if (!quietFrom && g.screenOff) quietFrom = now;
                if (--quiet == 0 && quietFrom && now - quietFrom >= 1000 * MS + 5 * 22 * MS && g.screenOff && now >= g.inputUntil) {
                    CHECK(g.lowered && g.domain[0].level == 0 && g.domain[1].level == 0);
                    quietOffs++;
                }
            }
        }
    }
    CHECK(quietOffs > 100 && loweredChecks > 10000);
    printf("ok  %u random windows with the screen-off policy: lowered only off past the delay without input, then at most the "
           "screen-off caps; otherwise the floor, the heat caps and the screen-on touch level; quiet offs end at the lowest level (%u "
           "times)\n", checked / 2, quietOffs);
}

// Random windows straight into governorStep (eight CPUs, six in domain 0, some without a sample): whatever the load, touches, GPU
// submissions, process starts, a driver's boosts and heat, every target lies between the floor and the top, under the cap while capped, at
// least the touch level while touched and not capped (the touch level lies under the caps), at least the middle level while GPU frames
// flow and not capped, at least the launch level (under the caps while limited) during a launch boost; a boost began only on a process
// start at most 1000 ms after a touch frame or a driver's word; and 18 quiet windows end at the floor unless a hold ends too late for
// the down rule (4 windows past the end of the holds).
static void testProperties()
{
    scmi::Perf perf;
    memset(&perf, 0, sizeof(perf));
    for (unsigned d = 0; d < 2; d++) {
        perf.domain[d].count = 16;
        for (unsigned i = 0; i < 16; i++) perf.domain[d].levels[i].khz = kTable[d][i];
    }
    static cpufreq::Governor g;
    uint64_t state = 12345;
    auto next = [&](uint64_t n) { state = state * 6364136223846793005ull + 1442695040888963407ull; return (state >> 33) % n; };
    unsigned checked = 0, quietRuns = 0, launches = 0;
    for (unsigned run = 0; run < 40; run++) {
        const uint32_t boot[2] = {(uint32_t)next(16), (uint32_t)next(16)};
        const uint32_t minMhz = run % 4 == 0 ? (uint32_t)next(4000) : 0;
        memset(&g, 0, sizeof(g));
        cpufreq::governorInit(&g, perf, minMhz, boot, 1000000000ull, 0);
        uint64_t now = 1000 * MS, lastTouch = 0;
        uint32_t touch = 0, gpu = 0, heat = 0, quiet = 0, procs = 0, asks = 0, past = 0;
        for (unsigned w = 0; w < 5000; w++) {
            cpufreq::CpuWindow cpus[cpufreq::kCpus];
            const unsigned mode = (unsigned)next(10);
            const uint64_t span = mode == 0 ? next(200) * MS : 18 * MS + next(4 * MS);
            for (unsigned i = 0; i < cpufreq::kCpus; i++) {
                cpus[i].domain = i < 6 ? 0 : 1;
                cpus[i].span = next(20) == 0 ? 0 : span;
                cpus[i].busy = quiet ? 0 : mode < 4 ? span : next(span + 1);
            }
            if (!quiet && next(40) == 0) quiet = 18;
            if (!quiet && next(5) == 0) touch++;
            if (!quiet && next(3)) gpu++;
            const bool started = !quiet && next(60) == 0, asked = !quiet && next(400) == 0;
            procs += started;
            asks += asked;
            if (next(50) == 0) heat = (uint32_t)next(8);
            if (quiet) heat = 0;
            now += span;
            uint32_t target[2];
            const uint32_t touchSeen = g.touchSeen, began = g.launches;
            cpufreq::governorStep(&g, now, cpus, cpufreq::kCpus, touch, gpu, heat, target, cpufreq::ScreenInput{0, 0},
                                  cpufreq::LaunchInput{procs, asks});
            if (touch != touchSeen) lastTouch = now;
            if (g.launches != began) {
                CHECK(asked || (started && lastTouch && now - lastTouch <= 1000 * MS));
                launches++;
            }
            CHECK(g.launching == (now < g.launchUntil));
            if (heat & cpufreq::kGpuHeatEmergency) CHECK(!g.launching && g.launches == began);
            for (unsigned d = 0; d < 2; d++) {
                const cpufreq::Domain &x = g.domain[d];
                CHECK(target[d] >= x.floor && target[d] <= x.top);
                if (g.capped) CHECK(target[d] <= x.cap);
                if (now < g.touchUntil && !g.capped) CHECK(target[d] >= x.touch);
                if (g.frames && !g.capped) CHECK(target[d] >= x.gpu);
                if (g.launching) CHECK(target[d] >= cpufreq::launchLevel(x, (heat & cpufreq::kGpuHeatLimiting) != 0));
                if (target[d] != x.level) cpufreq::governorSet(&g, d, target[d], now);
                checked++;
            }
            past = now >= g.touchUntil && now >= g.launchUntil ? past + 1 : 0;
            if (quiet && --quiet == 0 && past >= 4 && !g.frames) {
                CHECK(g.domain[0].level == g.domain[0].floor && g.domain[1].level == g.domain[1].floor);
                quietRuns++;
            }
        }
    }
    CHECK(quietRuns > 100 && launches > 1000);
    printf("ok  %u random windows of 8 CPUs: floor <= level <= top, the caps while capped, the touch, GPU-frame and launch levels while "
           "they apply, %u launch boosts all on a gated process start or a driver's word, the floor after 18 quiet windows (%u times)\n",
           checked / 2, launches, quietRuns);
}

// Extreme windows straight into governorStep: spans up to 2^64 - 1 (a thread that stalled), busy above the span, CPUs of no known domain
// or without a sample, fewer CPUs than kCpus, tables of one level or of levels near 2^32 kHz. Each domain's demand equals a 128-bit
// reference of the same rules (cpuStep's scaling of long windows, then no 64-bit wrap of kHistory x busy x kHz), every target lies
// between the floor and the top.
static void testExtremes()
{
    static scmi::Perf perf;
    static cpufreq::Governor g;
    uint64_t state = 0x243f6a8885a308d3ull;
    auto next = [&](uint64_t n) { state = state * 6364136223846793005ull + 1442695040888963407ull; return n ? (state >> 11) % n : 0; };
    auto wide = [&]() { const uint64_t hi = next(1ull << 32); return hi << 32 | next(1ull << 32); };
    unsigned windows = 0, longWindows = 0;
    for (unsigned run = 0; run < 24; run++) {
        memset(&perf, 0, sizeof(perf));
        for (unsigned d = 0; d < 2; d++) {
            const uint32_t count = run % 6 == 0 ? 1 : 1 + (uint32_t)next(scmi::kMaxLevels);
            perf.domain[d].count = count;
            for (uint32_t i = 0; i < count; i++)
                perf.domain[d].levels[i].khz = run % 2 ? 0xffffffffu - (count - 1 - i) : 300000 + i * 250000;
        }
        const uint32_t boot[2] = {(uint32_t)next(perf.domain[0].count), (uint32_t)next(perf.domain[1].count)};
        memset(&g, 0, sizeof(g));
        cpufreq::governorInit(&g, perf, 0, boot, 19200000, 0);
        uint64_t refBusy[cpufreq::kCpus][cpufreq::kHistory] = {}, refSpan[cpufreq::kCpus][cpufreq::kHistory] = {};
        uint32_t refDomain[cpufreq::kCpus] = {};
        uint64_t now = 1;
        for (unsigned w = 0; w < 2000; w++) {
            cpufreq::CpuWindow cpus[cpufreq::kCpus];
            const unsigned kind = (unsigned)next(4);
            const uint64_t span = kind == 0 ? wide() : kind == 1 ? 1ull << (29 + next(35)) : 384000 + next(20000);
            longWindows += kind < 2;
            for (unsigned i = 0; i < cpufreq::kCpus; i++) {
                cpus[i].domain = next(30) == 0 ? (uint32_t)next(1u << 31) : i < 6 ? 0 : 1;
                cpus[i].span = next(10) == 0 ? 0 : span;
                cpus[i].busy = next(3) == 0 ? wide() : next(4) == 0 ? cpus[i].span : next(cpus[i].span + 1);
            }
            const unsigned n = w % 25 == 0 ? (unsigned)next(cpufreq::kCpus + 1) : cpufreq::kCpus;
            const uint32_t slot = g.slot;
            for (unsigned c = 0; c < cpufreq::kCpus; c++) {
                uint32_t dom = refDomain[c];
                uint64_t s = 0, b = 0;
                if (c < n && cpus[c].span && cpus[c].domain < scmi::kDomains) {
                    dom = cpus[c].domain; s = cpus[c].span; b = cpus[c].busy > s ? s : cpus[c].busy;
                    while (s > 0xffffffffull / cpufreq::kHistory) { s >>= 1; b >>= 1; }
                }
                if (dom != refDomain[c]) {
                    for (unsigned i = 0; i < cpufreq::kHistory; i++) refBusy[c][i] = refSpan[c][i] = 0;
                    refDomain[c] = dom;
                }
                refBusy[c][slot] = b; refSpan[c][slot] = s;
            }
            uint32_t target[2];
            now += 384000;
            cpufreq::governorStep(&g, now, cpus, n, (uint32_t)next(3), (uint32_t)next(2), (uint32_t)next(8), target,
                                  cpufreq::ScreenInput{0, 0}, cpufreq::LaunchInput{(uint32_t)next(3), (uint32_t)next(2)});
            unsigned __int128 want[2] = {0, 0};
            for (unsigned c = 0; c < cpufreq::kCpus; c++) {
                CHECK(g.cpu[c].domain == refDomain[c]);
                unsigned __int128 work = 0, total = 0;
                for (unsigned i = 0; i < cpufreq::kHistory; i++) {
                    work += (unsigned __int128)refBusy[c][i] * g.domain[refDomain[c]].ran[i];
                    total += refSpan[c][i];
                }
                const unsigned __int128 demand = total ? work / total : 0;
                if (demand > want[refDomain[c]]) want[refDomain[c]] = demand;
            }
            for (unsigned d = 0; d < 2; d++) {
                const cpufreq::Domain &x = g.domain[d];
                CHECK(x.demand == want[d]);
                CHECK(target[d] >= x.floor && target[d] <= x.top && x.top < perf.domain[d].count);
                if (target[d] != x.level) cpufreq::governorSet(&g, d, target[d], now);
            }
            windows++;
        }
    }
    printf("ok  %u extreme windows (%u of them 2^29 ticks or longer, up to 2^64 - 1; busy above the span, unknown domains, fewer CPUs, "
           "one-level and 2^32 kHz tables): the demand equals a 128-bit reference, floor <= target <= top\n", windows, longWindows);
}

static void testSampling()
{
    cpufreq::CpuTicks before[8] = {}, now[8] = {};
    for (unsigned c = 0; c < 8; c++) {
        before[c].known = now[c].known = true;
        before[c].running = now[c].running = true;
        before[c].domain = now[c].domain = c < 6 ? 0 : 1;
        before[c].idle = 0xfffffffe;                         // the tick counter wraps
    }
    const uint64_t tick = 192000, span = 20 * 19200;        // 10 ms ticks of a 19.2 MHz counter, a 20 ms window
    now[0].idle = 0xfffffffe + 2;   // idle the whole window
    now[1].idle = 0xfffffffe + 1;   // half
    now[2].idle = 0xfffffffe;       // busy
    for (unsigned c = 3; c < 6; c++) now[c].idle = 0xfffffffe + 2;
    now[6].idle = 0xfffffffe + 3;   // more idle than the window (tick rounding): 0
    now[7].idle = 0xfffffffe + 1;
    uint64_t busy[2];
    uint32_t online[2], online2[2];
    cpufreq::CpuWindow cpus[8];
    cpufreq::domainBusy(now, before, 8, span, tick, busy, online);
    CHECK(busy[0] == span && busy[1] == span / 2 && online[0] == 6 && online[1] == 2);
    // The governor's own view: every CPU's busy ticks (the same rules), no sample (span 0) for a CPU that did not run at both ends.
    cpufreq::cpuWindows(now, before, 8, span, tick, cpus, online2);
    CHECK(online2[0] == 6 && online2[1] == 2 && cpus[0].busy == 0 && cpus[1].busy == span / 2 && cpus[2].busy == span);
    CHECK(cpus[6].busy == 0 && cpus[6].domain == 1 && cpus[7].busy == span / 2 && cpus[7].span == span && cpus[3].domain == 0);
    now[2].running = false;         // the busy CPU went offline: not counted
    cpufreq::domainBusy(now, before, 8, span, tick, busy, online);
    CHECK(busy[0] == span / 2 && online[0] == 5);
    cpufreq::cpuWindows(now, before, 8, span, tick, cpus, online2);
    CHECK(online2[0] == 5 && cpus[2].span == 0 && cpus[2].busy == 0);
    before[1].running = false;      // started during the window: not counted yet
    now[7].known = false;
    cpufreq::domainBusy(now, before, 8, span, tick, busy, online);
    CHECK(busy[0] == 0 && online[0] == 4 && busy[1] == 0 && online[1] == 1);
    cpufreq::cpuWindows(now, before, 8, span, tick, cpus, online2);
    CHECK(online2[0] == 4 && online2[1] == 1 && !cpus[1].span && !cpus[7].span && cpus[6].span == span);
    now[6].domain = 7;              // a CPU of no known domain is left out
    cpufreq::domainBusy(now, before, 8, span, tick, busy, online);
    CHECK(online[1] == 0);
    cpufreq::cpuWindows(now, before, 8, span, tick, cpus, online2);
    CHECK(online2[1] == 0 && !cpus[6].span);
    // The row's loads: percent of the period, '-' without a running CPU.
    cpufreq::Control c;
    memset(&c, 0, sizeof(c));
    c.loadBusy[0] = 340; c.loadSpan[0] = 1000; c.loadBusy[1] = 5; c.loadSpan[1] = 1000;
    const uint32_t seen[2] = {6, 0};
    cpufreq::controlLoads(&c, seen);
    CHECK(c.load[0] == 34 && c.load[1] == cpufreq::kLoadUnknown && c.loadBusy[0] == 0 && c.loadSpan[0] == 0);
    c.loadBusy[0] = 2000; c.loadSpan[0] = 1000;
    cpufreq::controlLoads(&c, seen);
    CHECK(c.load[0] == 100);
    printf("ok  sampling: busiest running CPU per domain and every CPU's window from 10 ms idle ticks (wrap, offline, starting, unknown "
           "CPUs), row loads\n");
}

static void testFloorAndRow()
{
    scmi::Domain d = {};
    d.count = 16;
    for (unsigned i = 0; i < 16; i++) d.levels[i].khz = kTable[1][i];
    CHECK(cpufreq::floorIndex(d, 0) == 3 && cpufreq::floorIndex(d, 1) == 0 && cpufreq::floorIndex(d, 1018) == 1);
    CHECK(cpufreq::floorIndex(d, 4320) == 15 && cpufreq::floorIndex(d, 5000) == 15 && cpufreq::floorIndex(d, 2000) == 5);
    d.count = 2;
    CHECK(cpufreq::floorIndex(d, 0) == 1);
    d.count = 0;
    CHECK(cpufreq::floorIndex(d, 0) == 0);
    // The row: every combination of the widest values stays within 65 characters.
    cpufreq::Control c;
    memset(&c, 0, sizeof(c));
    c.on = true;
    for (unsigned d2 = 0; d2 < 2; d2++) {
        c.perf.domain[d2].count = 32;
        for (unsigned i = 0; i < 32; i++) c.perf.domain[d2].levels[i].khz = 99999999;
    }
    unsigned widest = 0;
    for (uint32_t mode : {1u, 2u})
        for (uint32_t level : {0u, 9u, 31u})
            for (uint32_t seen : {0u, 9u, 30u, 31u})
                for (uint32_t load : {0u, 7u, 100u, (uint32_t)cpufreq::kLoadUnknown})
                    for (uint32_t count : {0u, 9u, 999u, 100000u}) {
                        c.mode = mode;
                        c.level[0] = c.level[1] = level; c.seen[0] = c.seen[1] = seen; c.load[0] = c.load[1] = load;
                        c.below[0] = c.below[1] = seen < level;
                        c.gov.boosts = count; c.lowered = count; c.errors = count; c.gov.heatCaps = count; c.gov.launches = count;
                        c.retry.busy = count; c.retry.missing = count;
                        const std::string text = row(&c);
                        if (text.size() > widest) widest = (unsigned)text.size();
                    }
    CHECK(widest <= cpufreq::kRowWidth);
    c.mode = 2; c.level[0] = 31; c.level[1] = 31; c.seen[0] = 30; c.seen[1] = 30; c.load[0] = c.load[1] = 100;
    c.below[0] = c.below[1] = true;
    c.gov.boosts = c.lowered = c.errors = c.gov.heatCaps = 5000;
    c.gov.launches = 0;
    CHECK(row(&c) == "CPU 9999/9999MHz lv30!/30! ld 100/100% boost 999 thr 999 err 999");   // no room for " hot 999": left out
    // A level raised since the last check is no lowering: no '!' (the last check's level is shown only while it was found lowered).
    c.below[0] = c.below[1] = false;
    CHECK(row(&c) == "CPU 9999/9999MHz lv31/31 ld 100/100% boost 999 thr 999 err 999");   // " hot 999" would make 70: left out
    c.gov.boosts = c.lowered = c.errors = 0; c.gov.heatCaps = 7;
    c.retry.busy = c.retry.missing = 0;
    CHECK(row(&c) == "CPU 9999/9999MHz lv31/31 ld 100/100% boost 0 thr 0 err 0 hot 7");
    // "err": the refused optional messages and the retried failures (BUSY answers, missing answers), at most 999.
    c.errors = 1; c.retry.busy = 2; c.retry.missing = 3;
    CHECK(row(&c) == "CPU 9999/9999MHz lv31/31 ld 100/100% boost 0 thr 0 err 6 hot 7");
    c.errors = 0xffffffffu; c.retry.busy = 0xffffffffu; c.retry.missing = 0xffffffffu;
    CHECK(row(&c) == "CPU 9999/9999MHz lv31/31 ld 100/100% boost 0 thr 0 err 999 hot 7");
    // Launch boosts: only once there were any, only when they fit, after everything else.
    c.errors = c.retry.busy = c.retry.missing = 0;
    c.gov.launches = 3;
    CHECK(row(&c) == "CPU 9999/9999MHz lv31/31 ld 100/100% boost 0 thr 0 err 0 hot 7");
    c.load[0] = c.load[1] = 5;
    CHECK(row(&c) == "CPU 9999/9999MHz lv31/31 ld 5/5% boost 0 thr 0 err 0 hot 7 app 3");
    c.gov.launches = 12;
    CHECK(row(&c) == "CPU 9999/9999MHz lv31/31 ld 5/5% boost 0 thr 0 err 0 hot 7 app 12");
    c.gov.launches = 100000;
    CHECK(row(&c) == "CPU 9999/9999MHz lv31/31 ld 5/5% boost 0 thr 0 err 0 hot 7");
    c.gov.launches = 0; c.load[0] = c.load[1] = 100;
    c.errors = 0xffffffffu; c.retry.busy = 0xffffffffu; c.retry.missing = 0xffffffffu;
    // The off row with the longest reason stays within it as well.
    c.on = false;
    strcpy(c.reason, "status -2147483648 lvl set");
    CHECK(row(&c) == "CPU scmi off: status -2147483648 lvl set");
    // A budget's count after the reason (controlFail): the longest still fits the 40-byte reason and the row.
    {
        cpufreq::Control o;
        memset(&o, 0, sizeof(o));
        gLog.clear();
        o.log = logLine;
        cpufreq::controlFail(&o, scmi::fail(scmi::kStatus, scmi::kPerf, scmi::kDomainAttributes, INT32_MIN), 0xffffffffu);
        CHECK(!strcmp(o.reason, "status -2147483648 dom attr x4294967295") && strlen(o.reason) == sizeof(o.reason) - 1);
        CHECK(row(&o) == "CPU scmi off: status -2147483648 dom attr x4294967295" && logged("cpufreq: off: status -2147483648 dom attr x4"));
        cpufreq::controlFail(&o, scmi::fail(scmi::kTimeout, scmi::kPerf, scmi::kLevelGet), 1);
        CHECK(!strcmp(o.reason, "timeout lvl get"));
    }
    // While the start waits for its retry: the latest failure.
    c.retry.waiting = true;
    c.retry.last = scmi::fail(scmi::kStatus, scmi::kPerf, scmi::kDescribeLevels, INT32_MIN);
    CHECK(row(&c) == "CPU scmi retry: status -2147483648 describe");
    c.retry.waiting = false;
    char tiny[8];
    cpufreq::controlRow(&c, tiny, sizeof(tiny));
    CHECK(!strcmp(tiny, "CPU scm"));
    printf("ok  floor from MHz, row texts (widest %u of %u characters)\n", widest, cpufreq::kRowWidth);
}

// ---- Transient SCMI failures (CpuFreqGovernor.h, Retry): BUSY answers and missing answers retried after a backoff, the budgets, what is
// owed after a failure, log rationing. Measured 2026-10-31 (pkgD2, 41.849 s): one "status -6 lvl get" in a 5 s check ended the governor
// for the boot and left the clusters at their boot levels (about 1997 / 1018 MHz) for the rest of it.

static Fault busyOn(uint32_t message, unsigned skip, unsigned times)
{
    Fault f{scmi::kPerf, message, skip, times, kStatusCode};
    f.code = -6;
    return f;
}

static unsigned countLogged(const char *part)
{
    unsigned n = 0;
    for (auto &line : gLog) n += line.find(part) != std::string::npos;
    return n;
}

static std::string format(const char *pattern, unsigned a, unsigned b = 0, unsigned c = 0)
{
    char text[200];
    snprintf(text, sizeof(text), pattern, a, b, c);
    return text;
}

static void testRetryRules()
{
    using scmi::fail;
    CHECK(scmi::classify(fail(scmi::kStatus, scmi::kPerf, scmi::kLevelGet, -6)) == scmi::kRetryBusy);
    CHECK(scmi::classify(fail(scmi::kStatus, scmi::kPerf, scmi::kLevelGet, -7)) == scmi::kRetrySilent);
    for (int32_t status : {-1, -2, -3, -4, -5, -8, -9, -10, -11, -12, 1, 6, 7, INT32_MIN, INT32_MAX})
        CHECK(scmi::classify(fail(scmi::kStatus, scmi::kPerf, scmi::kLevelSet, status)) == scmi::kFinal);
    for (scmi::Error e : {scmi::kBusy, scmi::kTimeout, scmi::kChannelErr})
        CHECK(scmi::classify(fail(e, scmi::kPerf, scmi::kLevelSet)) == scmi::kRetrySilent);
    for (scmi::Error e : {scmi::kOk, scmi::kNotReady, scmi::kInterrupt, scmi::kShort, scmi::kBadHeader, scmi::kBadVersion,
                          scmi::kFewDomains, scmi::kNoLevelSet, scmi::kBadLevels, scmi::kTooManyLevels, scmi::kUnknownLevel,
                          scmi::kBadDomain})
        CHECK(scmi::classify(fail(e, scmi::kPerf, scmi::kLevelSet)) == scmi::kFinal);
    // The backoff: 20 ms (the next governor window) doubled per failure in a row, 1280 ms from the 7th on.
    const uint32_t expect[] = {20, 20, 40, 80, 160, 320, 640, 1280, 1280, 1280};
    for (uint32_t n = 0; n < 10; n++) CHECK(cpufreq::retryMs(n) == expect[n]);
    CHECK(cpufreq::retryMs(1000000) == 1280 && cpufreq::retryMs(UINT32_MAX) == 1280);
    uint64_t budget = 0, silent = 0;
    for (uint32_t n = 1; n < cpufreq::kRetryBudget; n++) budget += cpufreq::retryMs(n);
    for (uint32_t n = 1; n < cpufreq::kSilentBudget; n++) silent += cpufreq::retryMs(n);
    CHECK(budget == 53740 && silent == 620);
    printf("ok  transient results: BUSY (-6), COMMS_ERROR (-7), a timeout, a channel still held and the error bit are retried; every other "
           "status and every malformed answer is final; backoff 20/40/80/.../1280 ms: kRetryBudget failures in a row take %llu ms, "
           "kSilentBudget %llu ms of backoff\n", (unsigned long long)budget, (unsigned long long)silent);
}

// pkgD2 replayed: a 5 s check's PERF_LEVEL_GET answered BUSY (domain 0, domain 1, and domain 0 right after its level set in the same
// window). The governor stays on, nothing goes out for 20 ms, the check resumes with the domain it stopped at (the other one is neither
// asked nor counted again), and the levels keep following the load.
static void testBusyCheck()
{
    for (unsigned variant = 0; variant < 3; variant++) {
        const unsigned busyDomain = variant == 1 ? 1 : 0;
        FakeCpucp fake;
        cpufreq::Control *c = control(&fake);
        uint64_t now = 1000 * MS;
        CHECK(cpufreq::controlStart(c, cpufreq::kModeGovernor, 0, now));
        uint64_t busy[2], span[2];
        auto step = [&](uint32_t p0, uint32_t p1) {
            now += 20 * MS;
            loads(busy, span, p0, p1);
            return cpufreq::controlWindow(c, now, busy, span, 0, 0);
        };
        while (now + 20 * MS - c->lastCheck < 5000 * MS) CHECK(step(10, 30));     // quiet: 100% after it is a burst
        CHECK(c->level[0] == 3 && c->level[1] == 3);
        fake.settle();
        const unsigned gets0 = fake.gets(0), gets1 = fake.gets(1), rung = fake.rung;
        const size_t sets0 = fake.sets(0).size();
        fake.model.cap[1] = 2;                                  // the platform lowered domain 1: the check counts it once
        fake.setFaults({busyOn(scmi::kLevelGet, busyDomain, 1)});
        const uint64_t checkAt = now + 20 * MS;
        // Variant 2: a burst, domain 0's level set first in the window of a check that waited as long as it may (checkDue).
        if (variant == 2) c->lastCheck -= cpufreq::kCheckWaitMs * MS;
        const uint64_t lastCheck = c->lastCheck;
        CHECK(step(variant == 2 ? 100 : 10, 30) && c->on);
        fake.settle();
        CHECK(c->lastCheck == lastCheck && c->retry.at == checkAt + 20 * MS && c->retry.checked == (busyDomain ? 1u : 0u));
        CHECK(fake.rung == rung + 1 + busyDomain + (variant == 2 ? 1u : 0u));      // nothing after the BUSY answer
        CHECK(fake.sets(0).size() == sets0 + (variant == 2 ? 1u : 0u));
        const char *line = variant == 0 ? "cpufreq: retry: status -6 lvl get d0, next in 20 ms"
                         : variant == 1 ? "cpufreq: retry: status -6 lvl get d1 after lvl get d0, next in 20 ms"
                                        : "cpufreq: retry: status -6 lvl get d0 after lvl set d0, next in 20 ms";
        CHECK(logged(line) && !logged("cpufreq: off"));
        // Within the backoff nothing goes out; 20 ms later the check resumes and completes.
        CHECK(step(variant == 2 ? 100 : 10, 30) && c->on && c->lastCheck == checkAt + 20 * MS && c->retry.checked == 0);
        fake.settle();
        CHECK(fake.gets(0) == gets0 + (busyDomain ? 1u : 2u) && fake.gets(1) == gets1 + (busyDomain ? 2u : 1u));
        CHECK(c->lowered == 1 && logged("cpufreq: d1 at lv2 1401MHz, asked lv3 1689MHz, limits lv0-2"));
        const char *ok = variant == 0 ? "cpufreq: retry ok: lvl get d0 after 1 failure in 20 ms (busy 1, no answer 0)"
                       : variant == 1 ? "cpufreq: retry ok: lvl get d1 after 1 failure in 20 ms (busy 1, no answer 0)"
                                      : "cpufreq: retry ok: lvl set d0 after 1 failure in 20 ms (busy 1, no answer 0)";
        CHECK(logged(ok));
        CHECK(row(c).find(" err 1 ") != std::string::npos && c->retry.busy == 1 && c->retry.streak == 0);
        // The levels resume following the load: a burst reaches the top on both domains, the platform with them.
        fake.model.cap[1] = 15;
        for (unsigned w = 0; w < 4; w++) CHECK(step(100, 100));
        fake.settle();
        CHECK(c->level[0] == 15 && c->level[1] == 15 && fake.model.requested[0] == 15 && fake.model.requested[1] == 15);
        for (unsigned w = 0; w < 30; w++) CHECK(step(0, 0));
        fake.settle();
        CHECK(c->level[0] == 3 && c->level[1] == 3 && fake.model.requested[0] == 3 && fake.model.requested[1] == 3);
    }
    printf("ok  pkgD2 replayed: BUSY at a 5 s check's PERF_LEVEL_GET (d0, d1, d0 right after its level set) keeps the governor on, "
           "nothing for 20 ms, the check resumes where it stopped (no domain asked or counted twice), the levels follow the load\n");
}

// BUSY bursts on PERF_LEVEL_SET (1, 3, 7, 20 in a row): no message at all while each backoff lasts (20, 40, 80, ... 1280 ms after the
// failures in a row), the owed set goes out again once it passed, and when the burst ends the platform has the governor's level.
static void testBusyBursts()
{
    for (unsigned burst : {1u, 3u, 7u, 20u}) {
        FakeCpucp fake;
        cpufreq::Control *c = control(&fake);
        uint64_t now = 1000 * MS;
        CHECK(cpufreq::controlStart(c, cpufreq::kModeGovernor, 0, now));
        uint64_t busy[2], span[2];
        for (unsigned w = 0; w < 10; w++) {
            now += 20 * MS;
            loads(busy, span, 0, 0);
            CHECK(cpufreq::controlWindow(c, now, busy, span, 0, 0));
        }
        CHECK(c->level[0] == 3 && c->level[1] == 3);
        fake.settle();
        const size_t s0 = fake.sets(0).size();
        fake.setFaults({busyOn(scmi::kLevelSet, 0, burst)});
        std::vector<uint64_t> tries;
        unsigned quiet = 0;
        for (unsigned w = 0; w < 2000 && fake.sets(0).size() - s0 <= burst; w++) {
            now += 20 * MS;
            loads(busy, span, 100, 0);
            const bool backoff = now < c->retry.at;
            const size_t sent = fake.sets(0).size();
            const unsigned rung = fake.rung;
            CHECK(cpufreq::controlWindow(c, now, busy, span, 0, 0) && c->on);
            fake.settle();
            if (backoff) { CHECK(fake.rung == rung); quiet++; }
            CHECK(fake.sets(0).size() - sent <= 1);
            if (fake.sets(0).size() > sent) tries.push_back(now);
        }
        CHECK(tries.size() == burst + 1);
        uint64_t took = 0;
        for (unsigned i = 1; i <= burst; i++) {
            CHECK(tries[i] - tries[i - 1] == cpufreq::retryMs(i) * MS);
            took += cpufreq::retryMs(i);
        }
        // Resumed: nothing owed, the platform at the level the governor recorded, the top a window or two later.
        CHECK(c->retry.pending == 0 && c->level[0] >= 9 && fake.model.requested[0] == c->level[0] && c->retry.sets == 0);
        for (unsigned w = 0; w < 3; w++) {
            now += 20 * MS;
            CHECK(cpufreq::controlWindow(c, now, busy, span, 0, 0));
        }
        fake.settle();
        CHECK(c->level[0] == 15 && fake.model.requested[0] == 15);
        CHECK(logged("cpufreq: retry: status -6 lvl set d0, next in 20 ms"));
        CHECK(logged((format("cpufreq: retry ok: lvl set d0 after %u ", burst) + (burst == 1 ? "failure" : "failures") +
                      format(" in %u ms (busy %u, no answer 0)", (unsigned)took, burst)).c_str()));
        CHECK(row(c).find(format(" err %u ", burst)) != std::string::npos && !logged("cpufreq: off"));
        printf("    BUSY x%u at a level set: tries %u, %u quiet windows, the level through after %llu ms\n", burst, (unsigned)tries.size(),
               quiet, (unsigned long long)took);
    }
    printf("ok  BUSY bursts on PERF_LEVEL_SET: nothing sent while a backoff lasts, retried after 20/40/80/... ms, the level the governor "
           "wants goes through when the burst ends\n");
}

// BUSY at every level set from now on: off after kRetryBudget in a row (53.74 s of backoff), the reason named and counted, both domains
// asked back to their boot levels (BUSY too, logged), nothing more after that; one streak, one line.
static void testBusyForever()
{
    FakeCpucp fake;
    cpufreq::Control *c = control(&fake);
    uint64_t now = 1000 * MS;
    CHECK(cpufreq::controlStart(c, cpufreq::kModeGovernor, 0, now));
    uint64_t busy[2], span[2];
    for (unsigned w = 0; w < 10; w++) {
        now += 20 * MS;
        loads(busy, span, 0, 0);
        CHECK(cpufreq::controlWindow(c, now, busy, span, 0, 0));
    }
    fake.settle();
    fake.setFaults({busyOn(scmi::kLevelSet, 0, 0)});
    const size_t s0 = fake.sets(0).size() + fake.sets(1).size();
    uint64_t first = 0, off = 0;
    for (unsigned w = 0; w < 10000 && !off; w++) {
        now += 20 * MS;
        loads(busy, span, 100, 0);
        const bool backoff = now < c->retry.at;
        const unsigned rung = fake.rung;
        const bool on = cpufreq::controlWindow(c, now, busy, span, 0, 0);
        fake.settle();
        if (backoff) CHECK(fake.rung == rung && on);
        if (!first && c->retry.streak) first = now;
        if (!on) off = now;
    }
    CHECK(off && !c->on && off - first == 53740 * MS);
    CHECK(!strcmp(c->reason, "status -6 lvl set x48") && row(c) == "CPU scmi off: status -6 lvl set x48");
    CHECK(logged("cpufreq: off: status -6 lvl set x48"));
    CHECK(logged("cpufreq: d0 back to boot lv8: status -6 lvl set") && logged("cpufreq: d1 back to boot lv4: status -6 lvl set"));
    CHECK(fake.sets(0).size() + fake.sets(1).size() == s0 + cpufreq::kRetryBudget + 2);
    CHECK(countLogged("cpufreq: retry") == 1);
    const unsigned rung = fake.rung;
    for (unsigned w = 0; w < 100; w++) {
        now += 20 * MS;
        CHECK(!cpufreq::controlWindow(c, now, busy, span, 0, 0));
    }
    CHECK(fake.rung == rung);
    printf("ok  BUSY at every level set: off after %u in a row (%.2f s), \"status -6 lvl set x48\", both domains asked back to their boot "
           "levels, nothing sent after that, one log line for the streak\n", cpufreq::kRetryBudget, (double)(off - first) / 1e9);
}

// Missing answers: intermittent late answers (each set then owed and sent again), a stall the platform comes back from, a platform that
// stops answering for good (off after kSilentBudget, one message a window at most till then), and the boot levels asked back for a domain
// whose set was left owed even when the level recorded is its boot level.
static void testTimeouts()
{
    {
        FakeCpucp fake;
        cpufreq::Control *c = control(&fake);
        uint64_t now = 1000 * MS;
        CHECK(cpufreq::controlStart(c, cpufreq::kModeGovernor, 0, now));
        uint64_t busy[2], span[2];
        uint32_t maxSilent = 0;
        for (unsigned w = 0; w < 400; w++) {
            if (w % 10 == 5) {
                Fault late{scmi::kPerf, scmi::kLevelSet, 0, 1, kLate};
                late.lateMs = 70;
                fake.setFaults({late});
            }
            now += 20 * MS;
            const bool heavy = (w / 25) % 2 == 0;           // 500 ms busy, 500 ms idle: level sets both ways
            loads(busy, span, heavy ? 100 : 0, heavy ? 100 : 0);
            CHECK(cpufreq::controlWindow(c, now, busy, span, 0, 0) && c->on);
            if (c->retry.silent > maxSilent) maxSilent = c->retry.silent;
        }
        fake.setFaults({});
        for (unsigned w = 0; w < 80; w++) {
            now += 20 * MS;
            loads(busy, span, 0, 0);
            CHECK(cpufreq::controlWindow(c, now, busy, span, 0, 0));
        }
        fake.settle();
        CHECK(c->on && c->retry.missing >= 8 && maxSilent <= 2 && c->retry.pending == 0);
        CHECK(fake.model.requested[0] == c->level[0] && fake.model.requested[1] == c->level[1] && c->level[0] == 3 && c->level[1] == 3);
        CHECK(row(c).find(format(" err %u ", c->retry.missing)) != std::string::npos && !logged("cpufreq: off"));
        printf("    intermittent late answers: %u missing answers over 8 s, at most %u in a row, on throughout, the platform at the "
               "governor's levels\n", c->retry.missing, maxSilent);
    }
    {
        // A stall: the platform holds the channel 180 ms (timeout at 50 ms, then the channel still held), then answers: on.
        FakeCpucp fake;
        cpufreq::Control *c = control(&fake);
        uint64_t now = 1000 * MS;
        CHECK(cpufreq::controlStart(c, cpufreq::kModeGovernor, 0, now));
        Fault stall{scmi::kPerf, scmi::kLevelSet, 0, 1, kLate};
        stall.lateMs = 180;
        fake.setFaults({stall});
        uint64_t busy[2], span[2];
        uint32_t maxSilent = 0;
        for (unsigned w = 0; w < 200; w++) {
            now += 20 * MS;
            loads(busy, span, 100, 0);
            CHECK(cpufreq::controlWindow(c, now, busy, span, 0, 0) && c->on);
            if (c->retry.silent > maxSilent) maxSilent = c->retry.silent;
        }
        fake.settle();
        CHECK(maxSilent >= 2 && maxSilent < cpufreq::kSilentBudget && c->level[0] == 15 && fake.model.requested[0] == 15);
        CHECK(logged("cpufreq: retry: timeout lvl set d0, next in 20 ms") && logged("cpufreq: retry ok: lvl set d0 after "));
    }
    {
        // A platform that stops answering for good: off after kSilentBudget missing answers in a row, named after the first; until then
        // at most one message a window (each failed transfer the only one of its window), and the restore of both domains last.
        FakeCpucp fake;
        cpufreq::Control *c = control(&fake);
        uint64_t now = 1000 * MS;
        CHECK(cpufreq::controlStart(c, cpufreq::kModeGovernor, 0, now));
        uint64_t busy[2], span[2];
        for (unsigned w = 0; w < 10; w++) {
            now += 20 * MS;
            loads(busy, span, 0, 0);
            CHECK(cpufreq::controlWindow(c, now, busy, span, 0, 0));
        }
        fake.settle();
        fake.setFaults({Fault{scmi::kPerf, scmi::kLevelSet, 0, 1, kSilent}});
        unsigned windows = 0, attempts = 0;
        double longest = 0;
        bool on = true;
        const uint64_t began = now + 20 * MS;
        while (on && windows < 200) {
            now += 20 * MS;
            loads(busy, span, 100, 0);
            const uint64_t silentBefore = c->retry.silent;
            const auto t0 = std::chrono::steady_clock::now();
            on = cpufreq::controlWindow(c, now, busy, span, 0, 0);
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            if (ms > longest) longest = ms;
            attempts += c->retry.silent != silentBefore || !on;
            windows++;
        }
        CHECK(!on && !strcmp(c->reason, "timeout lvl set x6") && attempts == cpufreq::kSilentBudget);
        CHECK(now - began == 620 * MS);                         // 20 + 40 + 80 + 160 + 320 ms of backoff
        CHECK(logged("cpufreq: d0 back to boot lv8: chan busy lvl set") && logged("cpufreq: d1 back to boot lv4: chan busy lvl set"));
        CHECK(countLogged("cpufreq: retry") == 1 && row(c) == "CPU scmi off: timeout lvl set x6");
        printf("    a platform that stops answering: off after %u missing answers (%u windows, 0.62 s of backoff), the longest window "
               "%.0f ms (three 50 ms waits at most: the failure, then the two restores)\n", attempts, windows, longest);
        CHECK(longest < 1000);
    }
    {
        // Owed sets are asked back too: d0's level set to the top unanswered in time (the platform applied it late), its level recorded
        // still the boot one; then a final failure: d0 asked back to its boot level all the same, d1 (nothing owed, at its boot level) not.
        FakeCpucp fake;
        cpufreq::Control *c = control(&fake);
        uint64_t now = 1000 * MS;
        CHECK(cpufreq::controlStart(c, cpufreq::kModeGovernor, 0, now));
        fake.setFaults({Fault{scmi::kPerf, scmi::kLevelSet, 0, 1, kWrongToken}});
        uint64_t busy[2], span[2];
        now += 20 * MS;
        loads(busy, span, 100, 40);
        CHECK(cpufreq::controlWindow(c, now, busy, span, 0, 0) && c->on && c->retry.pending == 1 && c->level[0] == 8);
        fake.settle();
        CHECK(fake.model.requested[0] == 15 && c->level[1] == 4);
        Fault denied{scmi::kPerf, scmi::kLevelSet, 0, 1, kStatusCode};
        denied.code = -3;
        fake.setFaults({denied});
        now += 20 * MS;
        CHECK(!cpufreq::controlWindow(c, now, busy, span, 0, 0) && !strcmp(c->reason, "status -3 lvl set"));
        fake.settle();
        CHECK(logged("cpufreq: d0 back to boot lv8: ok") && !logged("cpufreq: d1 back to boot") && fake.model.requested[0] == 8);
    }
    printf("ok  missing answers: intermittent late answers retried (the set owed and sent again), a 180 ms stall recovered, a platform "
           "that stops answering off after kSilentBudget, an owed level set asked back to the boot level\n");
}

// The start: BUSY at its first message (waits of 20, 40, 80 ms, the row saying why), BUSY for good (off after kRetryBudget attempts),
// transport failures once (the next attempt runs), and a message without an answer of ours in ten attempts in a row while the others
// are answered (the platform is there: no kSilentBudget).
static void testStartRetry()
{
    {
        FakeCpucp f;
        Fault x{scmi::kBase, scmi::kVersion, 0, 3, kStatusCode};
        x.code = -6;
        f.setFaults({x});
        cpufreq::Control *c = control(&f);
        const uint64_t now = 1000 * MS;
        CHECK(!cpufreq::controlStart(c, cpufreq::kModeGovernor, 0, now) && c->retry.waiting && !c->on && !c->reason[0]);
        CHECK(row(c) == "CPU scmi retry: status -6 base ver" && c->retry.at == now + 20 * MS);
        CHECK(logged("cpufreq: retry: status -6 base ver, next in 20 ms") && !logged("cpufreq: off"));
        // Called before its backoff passed: still waiting, nothing sent.
        CHECK(!cpufreq::controlProbe(c, now + 19 * MS) && c->retry.waiting && f.count(scmi::kBase, scmi::kVersion) == 1);
        std::vector<uint64_t> at;
        while (c->retry.waiting) {
            at.push_back(c->retry.at);
            cpufreq::controlProbe(c, c->retry.at);
            CHECK(at.size() < 10);
        }
        CHECK(c->on && at.size() == 3 && at[0] == now + 20 * MS && at[1] == at[0] + 40 * MS && at[2] == at[1] + 80 * MS);
        CHECK(logged("cpufreq: retry ok: start after 3 failures in 140 ms (busy 3, no answer 0)") && c->lastCheck == at[2]);
        CHECK(logged("cpufreq: scmi base 2.0 perf 3.0 domains 2") && logged("cpufreq: governor floor"));
        CHECK(row(c) == "CPU 1996/1958MHz lv8/4 ld -/-% boost 0 thr 0 err 3 hot 0");
        CHECK(cpufreq::controlProbe(c, at[2] + MS) && f.count(scmi::kBase, scmi::kVersion) == 4);    // on already: nothing sent
    }
    {
        FakeCpucp f;
        Fault x{scmi::kPerf, scmi::kAttributes, 0, 0, kStatusCode};
        x.code = -6;
        f.setFaults({x});
        cpufreq::Control *c = control(&f);
        uint64_t now = 1000 * MS;
        const uint64_t start = now;
        unsigned tries = 0;
        CHECK(!startLikeThread(c, cpufreq::kModeGovernor, 0, &now, &tries) && !c->on && !c->retry.waiting);
        CHECK(tries == cpufreq::kRetryBudget && now - start == 53740 * MS && !strcmp(c->reason, "status -6 perf attr x48"));
        CHECK(f.count(scmi::kPerf, scmi::kAttributes) == cpufreq::kRetryBudget && f.sets(0).empty() && f.sets(1).empty());
        CHECK(!cpufreq::controlProbe(c, now + 1000 * MS) && f.count(scmi::kPerf, scmi::kAttributes) == cpufreq::kRetryBudget);
        CHECK(row(c) == "CPU scmi off: status -6 perf attr x48");
    }
    for (FaultKind kind : {kErrorBit, kWrongToken, kLate}) {
        FakeCpucp f;
        Fault x{scmi::kPerf, scmi::kDescribeLevels, 2, 1, kind};
        x.lateMs = 80;
        f.setFaults({x});
        cpufreq::Control *c = control(&f);
        uint64_t now = 1000 * MS;
        unsigned tries = 0;
        CHECK(startLikeThread(c, cpufreq::kModeGovernor, 0, &now, &tries));
        CHECK(tries >= 2 && tries <= (kind == kLate ? 4u : 2u) && c->retry.missing == tries - 1 && c->perf.domain[1].count == 16);
    }
    {
        FakeCpucp f;
        f.setFaults({Fault{scmi::kPerf, scmi::kDescribeLevels, 0, 10, kWrongToken}});
        cpufreq::Control *c = control(&f);
        uint64_t now = 1000 * MS;
        unsigned tries = 0;
        CHECK(startLikeThread(c, cpufreq::kModeGovernor, 0, &now, &tries) && tries == 11 && c->retry.missing == 10);
    }
    printf("ok  the start: BUSY retried after 20/40/80 ms (row \"CPU scmi retry: ...\"), off after kRetryBudget attempts of BUSY, "
           "transport failures once survived, unanswered messages amid answered ones never count as a silent platform\n");
}

// PERF_LIMITS_GET (optional) answered BUSY or not at all: the lowering's line says so, the queries go on (only a refusal stops them),
// the check completes with the other domain after the backoff.
static void testLimitsTransient()
{
    FakeCpucp fake;
    cpufreq::Control *c = control(&fake);
    uint64_t now = 1000 * MS;
    CHECK(cpufreq::controlStart(c, cpufreq::kModeGovernor, 0, now));
    uint64_t busy[2], span[2];
    auto step = [&]() {
        now += 20 * MS;
        loads(busy, span, 0, 0);
        return cpufreq::controlWindow(c, now, busy, span, 0, 0);
    };
    for (unsigned w = 0; w < 10; w++) CHECK(step());
    fake.model.cap[0] = 2;
    fake.setFaults({busyOn(scmi::kLimitsGet, 0, 1)});
    while (!c->lowered) CHECK(step());
    const uint64_t at = now;
    CHECK(c->on && c->limits && c->errors == 0 && c->retry.checked == 1 && c->lastCheck < at);
    CHECK(logged("cpufreq: d0 at lv2 748MHz, asked lv3 960MHz, limits busy"));
    CHECK(logged("cpufreq: retry: status -6 lim get d0 after lvl get d0, next in 20 ms"));
    CHECK(step() && c->lastCheck == at + 20 * MS && c->retry.checked == 0 && fake.gets(0) == 2 && fake.gets(1) == 2);
    fake.model.cap[0] = 1;
    uint32_t lowered = c->lowered;
    while (c->lowered == lowered) CHECK(step());
    CHECK(logged("cpufreq: d0 at lv1 556MHz, asked lv3 960MHz, limits lv0-1"));
    fake.model.cap[0] = 0;
    Fault late{scmi::kPerf, scmi::kLimitsGet, 0, 1, kLate};
    late.lateMs = 80;
    fake.setFaults({late});
    lowered = c->lowered;
    while (c->lowered == lowered) CHECK(step());
    CHECK(logged("cpufreq: d0 at lv0 384MHz, asked lv3 960MHz, limits no answer") && c->limits && c->on);
    const uint64_t lateAt = now;
    for (unsigned w = 0; w < 5; w++) CHECK(step());
    fake.settle();
    CHECK(c->retry.streak == 0 && c->lastCheck > lateAt && c->retry.checked == 0);
    // A refusal still stops them, counted in err with the retried failures.
    fake.model.cap[0] = 2;
    Fault refused{scmi::kPerf, scmi::kLimitsGet, 0, 1, kStatusCode};
    refused.code = -3;
    fake.setFaults({refused});
    lowered = c->lowered;
    while (c->lowered == lowered) CHECK(step());
    CHECK(!c->limits && c->errors == 1 && logged("cpufreq: d0 at lv2 748MHz, asked lv3 960MHz, limits status -3"));
    CHECK(row(c).find(" err 3 ") != std::string::npos && c->on);
    printf("ok  PERF_LIMITS_GET BUSY or unanswered: the line says so, still asked at the next lowering, the check completes after the "
           "backoff; a refusal stops it\n");
}

// pagani-cpufreq=1: BUSY at the start's first top level set leaves both domains' top levels owed (the second deferred by the backoff):
// the next 1 s window sends both; a 5 s check that met BUSY resumes in the next window and re-asserts a lowered top.
static void testMaxRetry()
{
    FakeCpucp fake;
    fake.setFaults({busyOn(scmi::kLevelSet, 0, 1)});
    cpufreq::Control *c = control(&fake);
    uint64_t now = 1000 * MS;
    CHECK(cpufreq::controlStart(c, cpufreq::kModeMax, 0, now) && c->on);
    CHECK(c->level[0] == 8 && c->level[1] == 4 && c->retry.pending == 3);
    fake.settle();
    CHECK(fake.sets(0).size() == 1 && fake.sets(1).empty() && fake.count(scmi::kPerf, scmi::kLevelGet) == 2);
    uint64_t b[2] = {0, 0}, sp[2] = {MS, MS};
    now += 1000 * MS;
    CHECK(cpufreq::controlWindow(c, now, b, sp, 0, 0) && c->level[0] == 15 && c->level[1] == 15 && c->retry.pending == 0);
    fake.settle();
    CHECK(fake.model.requested[0] == 15 && fake.model.requested[1] == 15);
    CHECK(row(c) == "CPU max 3532/4320MHz lv15/15 ld -/-% thr 0 err 1");
    fake.setFaults({busyOn(scmi::kLevelGet, 0, 1)});
    fake.model.cap[1] = 12;
    while (c->retry.busy < 2) {
        now += 1000 * MS;
        CHECK(cpufreq::controlWindow(c, now, b, sp, 0, 0));
    }
    CHECK(c->lastCheck == 1000 * MS && c->lowered == 0);
    const size_t sets = fake.sets(1).size();
    now += 1000 * MS;
    CHECK(cpufreq::controlWindow(c, now, b, sp, 0, 0) && c->lastCheck == now && c->lowered == 1);
    fake.settle();
    CHECK(logged("cpufreq: d1 at lv12 3801MHz, asked lv15 4320MHz, limits lv0-12") && fake.sets(1).size() == sets + 1);
    printf("ok  pagani-cpufreq=1: owed top levels sent in the next window, a check met by BUSY resumed and re-asserting the top\n");
}

// Log rationing: 100 streaks (BUSY at each 5 s check's first PERF_LEVEL_GET for 8 minutes): lines for the first kRetryLines streaks
// (start and end), then a summary at most once a minute; the row counts all of them.
static void testRetryRationing()
{
    FakeCpucp fake;
    cpufreq::Control *c = control(&fake);
    uint64_t now = 1000 * MS;
    CHECK(cpufreq::controlStart(c, cpufreq::kModeGovernor, 0, now));
    uint64_t busy[2], span[2];
    loads(busy, span, 0, 0);
    const size_t before = gLog.size();
    unsigned injected = 0;                  // (not "checks": CHECK counts in that name)
    while (injected < 100 || c->retry.streak) {
        if (injected < 100 && now + 20 * MS - c->lastCheck >= 5000 * MS && !c->retry.streak) {
            fake.setFaults({busyOn(scmi::kLevelGet, 0, 1)});
            injected++;
        }
        now += 20 * MS;
        CHECK(cpufreq::controlWindow(c, now, busy, span, 0, 0));
    }
    fake.settle();
    CHECK(c->on && c->retry.busy == 100 && c->retry.streaks == 100 && c->retry.longest == 1);
    CHECK(countLogged("cpufreq: retry: ") == cpufreq::kRetryLines && countLogged("cpufreq: retry ok: ") == cpufreq::kRetryLines);
    const unsigned summaries = countLogged("cpufreq: retries since boot: ");
    CHECK(summaries == 8 && gLog.size() - before == 2 * cpufreq::kRetryLines + summaries);
    CHECK(logged("cpufreq: retries since boot: busy 9, no answer 0, 9 streaks, longest 1, checks left out 0 (a line a minute at most)"));
    CHECK(row(c).find(" err 100 ") != std::string::npos);
    printf("ok  log rationing: 100 BUSY streaks over %.0f s: %u lines for the first %u, then %u summaries (a minute apart at least); "
           "err 100 on the row\n", (double)(now - 1000 * MS) / 1e9, 2 * cpufreq::kRetryLines, cpufreq::kRetryLines, summaries);
}

// A check that keeps meeting BUSY is left out after kCheckTries (the governor goes on, the next check kCheckMs later): bounded traffic.
static void testCheckLeftOut()
{
    FakeCpucp fake;
    cpufreq::Control *c = control(&fake);
    uint64_t now = 1000 * MS;
    CHECK(cpufreq::controlStart(c, cpufreq::kModeGovernor, 0, now));
    fake.setFaults({busyOn(scmi::kLevelGet, 0, 0)});
    uint64_t busy[2], span[2];
    unsigned windows = 0;
    for (; windows < 3000; windows++) {
        now += 20 * MS;
        loads(busy, span, (windows / 100) % 2 ? 100 : 0, 0);     // the governor keeps changing levels meanwhile
        CHECK(cpufreq::controlWindow(c, now, busy, span, 0, 0) && c->on);
    }
    fake.settle();
    const unsigned gets = fake.gets(0) + fake.gets(1);
    CHECK(c->retry.skipped >= 10 && c->retry.skipped <= 12 && gets == 2 + cpufreq::kCheckTries * c->retry.skipped + c->retry.checks);
    CHECK(logged("cpufreq: 5 s check left out after 4 failed tries (last: status -6 lvl get d0), the next in 5 s"));
    CHECK(fake.model.requested[0] == c->level[0] && c->retry.pending == 0);
    printf("ok  a check that keeps meeting BUSY: left out after %u tries each time (%u checks in 60 s, %u PERF_LEVEL_GETs), the "
           "governor's level sets go through meanwhile\n", cpufreq::kCheckTries, c->retry.skipped, gets);
}

// pagani-cpufreq-retry=0 (Control::noRetry): every failure final again, as before the retries: pkgD2's BUSY check switches it off with
// the domains asked back to their boot levels, a BUSY start is off at once.
static void testNoRetry()
{
    {
        FakeCpucp fake;
        cpufreq::Control *c = control(&fake);
        c->noRetry = true;
        uint64_t now = 1000 * MS;
        CHECK(cpufreq::controlStart(c, cpufreq::kModeGovernor, 0, now) && logged(", mode 2, no retries"));
        uint64_t busy[2], span[2];
        while (now + 20 * MS - c->lastCheck < 5000 * MS) {
            now += 20 * MS;
            loads(busy, span, 10, 30);
            CHECK(cpufreq::controlWindow(c, now, busy, span, 0, 0));
        }
        fake.setFaults({busyOn(scmi::kLevelGet, 0, 1)});
        now += 20 * MS;
        CHECK(!cpufreq::controlWindow(c, now, busy, span, 0, 0) && !c->on && !strcmp(c->reason, "status -6 lvl get"));
        fake.settle();
        CHECK(logged("cpufreq: off: status -6 lvl get") && logged("cpufreq: d0 back to boot lv8: ok") &&
              logged("cpufreq: d1 back to boot lv4: ok") && !logged("cpufreq: retry"));
    }
    {
        FakeCpucp f;
        f.setFaults({busyOn(scmi::kAttributes, 0, 1)});
        cpufreq::Control *c = control(&f);
        c->noRetry = true;
        CHECK(!cpufreq::controlStart(c, cpufreq::kModeGovernor, 0, 1000 * MS) && !c->retry.waiting);
        CHECK(!strcmp(c->reason, "status -6 perf attr"));
    }
    printf("ok  pagani-cpufreq-retry=0: every failure final as before (BUSY at a check: off and back to the boot levels; at the start: "
           "off)\n");
}

// Random faults over random windows: BUSY on any message or on level sets or gets (bursts of 1-12), late answers, the error bit, other
// tokens; never a final one, never a new fault before the last one was resolved. Invariants: on throughout (no budget runs out), no
// message while a backoff lasts, at most four messages a window, the levels between the floor and the top, and 1.4 s after the faults
// stop nothing owed and the platform at the levels the governor recorded.
static void testRetryRandom()
{
    FakeCpucp fake;
    cpufreq::Control *c = control(&fake);
    uint64_t now = 1000 * MS;
    CHECK(cpufreq::controlStart(c, cpufreq::kModeGovernor, 0, now));
    uint64_t rng = 0x243f6a8885a308d3ull;
    auto next = [&]() { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; };
    unsigned windows = 0, injected = 0, slow = 0, quiet = 0, touches = 0, hints = 0, procs = 0;
    for (unsigned round = 0; round < 60; round++) {
        const unsigned length = 50 + (unsigned)(next() % 100);
        for (unsigned w = 0; w < length; w++) {
            if (next() % 20 == 0 && !c->retry.streak && !c->retry.sets && !c->retry.silent) {
                Fault f;
                f.protocol = scmi::kPerf;
                const unsigned pick = (unsigned)(next() % 3);
                f.message = pick == 0 ? kAnyMessage : pick == 1 ? (uint32_t)scmi::kLevelSet : (uint32_t)scmi::kLevelGet;
                f.skip = (unsigned)(next() % 3);
                f.times = 1 + (unsigned)(next() % 12);
                f.kind = kStatusCode;
                f.code = -6;
                const unsigned kind = (unsigned)(next() % 10);
                if (kind >= 7 && slow < 24) {
                    slow++;
                    f.times = 1;
                    f.kind = kind == 7 ? kErrorBit : kind == 8 ? kWrongToken : kLate;
                    f.lateMs = 70;
                }
                fake.setFaults({f});
                injected++;
            }
            uint64_t busy[2], span[2];
            loads(busy, span, (uint32_t)(next() % 101), (uint32_t)(next() % 101));
            if (next() % 9 == 0) touches++;
            if (next() % 3 == 0) hints++;
            if (next() % 40 == 0) procs++;
            now += 20 * MS;
            const bool backoff = now < c->retry.at;
            const unsigned rung = fake.rung;
            CHECK(cpufreq::controlWindow(c, now, busy, span, touches, hints, cpufreq::GpuHeat{0, 0}, cpufreq::ScreenInput{0, 0},
                                         cpufreq::LaunchInput{procs, 0}) && c->on);
            CHECK(fake.rung - rung <= 4 && (!backoff || fake.rung == rung));
            quiet += backoff;
            for (unsigned d = 0; d < 2; d++) CHECK(c->level[d] >= c->gov.domain[d].floor && c->level[d] <= c->gov.domain[d].top);
            windows++;
        }
        fake.setFaults({});
        for (unsigned w = 0; w < 70; w++) {
            uint64_t busy[2], span[2];
            loads(busy, span, (uint32_t)(next() % 101), (uint32_t)(next() % 101));
            now += 20 * MS;
            CHECK(cpufreq::controlWindow(c, now, busy, span, touches, hints, cpufreq::GpuHeat{0, 0}, cpufreq::ScreenInput{0, 0},
                                         cpufreq::LaunchInput{procs, 0}));
        }
        fake.settle();
        CHECK(c->retry.pending == 0 && c->retry.sets == 0 && c->retry.silent == 0);
        CHECK(fake.model.requested[0] == c->level[0] && fake.model.requested[1] == c->level[1]);
    }
    CHECK(c->on && !logged("cpufreq: off") && injected > 100 && c->retry.busy > 100 && c->gov.launches > 20);
    printf("ok  %u random windows with %u faults (%u of them late answers, error bits or other tokens), %u launch boosts among them: on "
           "throughout, no message in %u backoff windows, at most 4 a window, levels within floor..top, the platform at the governor's "
           "levels once they stop (busy %u, no answer %u, %u streaks, longest %u, %u checks left out)\n", windows, injected, slow,
           c->gov.launches, quiet, c->retry.busy, c->retry.missing, c->retry.streaks, c->retry.longest, c->retry.skipped);
}

// The touch hold (pagani-cpufreq-touch-ms, default 250 ms): the touch level until that long after the last touch frame, then the 3-of-4
// down rule; 100 is the hold of before, 0 none, more than kTouchMaxMs is cut there. The heat caps give way for 100 ms of it at most.
// Typing at a key every 220 ms: the level stays up between the keys with the new default, and drops in every gap with 100 ms.
static void testTouchHold()
{
    for (uint32_t asked : {100u, 200u, 300u, 0u, 60000u}) {
        FakeCpucp fake;
        cpufreq::Control *c = control(&fake);
        c->touchMs = asked;
        uint64_t now = 1000 * MS;
        CHECK(cpufreq::controlStart(c, cpufreq::kModeGovernor, 0, now));
        const uint32_t hold = asked > cpufreq::kTouchMaxMs ? cpufreq::kTouchMaxMs : asked;
        CHECK(c->gov.touchMs == hold && logged(format("touch 2227/2841 MHz for %u ms,", hold).c_str()));
        uint64_t busy[2], span[2];
        uint32_t touches = 0;
        loads(busy, span, 5, 5);
        auto step = [&]() { now += 20 * MS; CHECK(cpufreq::controlWindow(c, now, busy, span, touches, 0)); };
        for (unsigned w = 0; w < 6; w++) step();
        CHECK(c->level[0] == 3 && c->level[1] == 3);
        touches++;
        step();
        const uint64_t touched = now;
        CHECK(c->gov.touchUntil == touched + hold * MS && c->gov.touchLift == touched + (hold < 100 ? hold : 100) * MS);
        CHECK(c->gov.touchAt == touched && c->gov.boosts == 1);
        // The first window at or past the end of the hold is the 1st to want less, the 3rd takes the level down.
        const uint64_t down = touched + ((hold + 19) / 20 + 2) * 20 * MS;
        while (now < down + 100 * MS) {
            const bool held = hold && now < down;
            CHECK(c->level[0] == (held ? 9u : 3u) && c->level[1] == (held ? 8u : 3u));
            step();
        }
    }
    unsigned below[2] = {0, 0};
    for (unsigned i = 0; i < 2; i++) {
        FakeCpucp fake;
        cpufreq::Control *c = control(&fake);
        c->touchMs = i ? cpufreq::kTouchMs : 100;
        uint64_t now = 1000 * MS, busy[2], span[2];
        CHECK(cpufreq::controlStart(c, cpufreq::kModeGovernor, 0, now));
        uint32_t touches = 0;
        loads(busy, span, 5, 5);
        for (unsigned w = 0; w < 300; w++) {
            if (w % 11 == 0) touches++;
            now += 20 * MS;
            CHECK(cpufreq::controlWindow(c, now, busy, span, touches, 0));
            if (w >= 11) below[i] += c->level[0] < 9 || c->level[1] < 8;        // the first window only reads the counter
        }
    }
    CHECK(below[0] > 100 && below[1] == 0);
    printf("ok  pagani-cpufreq-touch-ms: the touch level to the end of the hold (100, 200, 300 ms, none, cut at 2000 ms), then the 3-of-4 "
           "down rule; a key every 220 ms stays at the touch level (%u of 289 windows below it with 100 ms)\n", below[0]);
}

// The 5 s check of the governor: not in a window with a level set (the platform still reports the level before it), what asked for the level
// and how long ago on its line, so the firmware's limit and a level just set read apart. A heat cap of ours alone writes no line.
static void testCheckLine()
{
    FakeCpucp fake;
    cpufreq::Control *c = control(&fake);
    uint64_t now = 1000 * MS;
    CHECK(cpufreq::controlStart(c, cpufreq::kModeGovernor, 0, now));
    uint64_t busy[2], span[2];
    uint32_t touches = 0, procs = 7, hints = 0;
    cpufreq::GpuHeat heat = {0, 0};
    auto step = [&]() {
        now += 20 * MS;
        CHECK(cpufreq::controlWindow(c, now, busy, span, touches, hints, heat, cpufreq::ScreenInput{0, 0}, cpufreq::LaunchInput{procs, 0}));
    };
    auto quiet = [&]() { loads(busy, span, 5, 5); for (unsigned w = 0; w < 300 && c->level[0] != 3; w++) step(); for (unsigned w = 0; w < 20; w++) step(); };
    // A check due in a window with a level set waits, past the 3 windows a burst sets levels in, for the first without one.
    quiet();
    CHECK(c->level[0] == 3 && c->gov.domain[0].why == cpufreq::kWhyFloor);
    while (now + 20 * MS - c->lastCheck < 5000 * MS) step();
    const uint64_t lastCheck = c->lastCheck;
    const unsigned gets = fake.gets(0);
    loads(busy, span, 100, 5);
    step(); CHECK(c->level[0] == 9 && c->lastCheck == lastCheck && fake.gets(0) == gets);
    step(); CHECK(c->level[0] == 15 && c->lastCheck == lastCheck && fake.gets(0) == gets);
    CHECK(c->gov.domain[0].asked == cpufreq::kWhyBurst && c->gov.domain[0].askedRun == 2);
    step(); CHECK(c->level[0] == 15 && c->lastCheck == now && fake.gets(0) == gets + 1);
    CHECK(c->gov.domain[0].why == cpufreq::kWhySaturated && c->gov.domain[0].whyRun == 3);
    // It waits kCheckWaitMs at most, whatever the levels do; a check a failure stopped does not wait.
    c->lastCheck = now - 5000 * MS;
    cpufreq::Domain *x = &c->gov.domain[0];
    x->changed = now;
    CHECK(!cpufreq::checkDue(c, now));
    x->changed = now - 20 * MS;
    CHECK(cpufreq::checkDue(c, now));
    x->changed = now;
    c->retry.checked = 1;
    CHECK(cpufreq::checkDue(c, now));
    c->retry.checked = 0;
    c->lastCheck = now - 6000 * MS;
    CHECK(cpufreq::checkDue(c, now));
    c->lastCheck = now - 4980 * MS;
    x->changed = now - 20 * MS;
    CHECK(!cpufreq::checkDue(c, now));
    // Every reason on the line of a domain the platform holds lower.
    auto lowered = [&](unsigned d, uint32_t cap) { fake.model.cap[d] = cap; now = c->lastCheck + 5000 * MS; };
    // Demand: 85% busy climbs a level at a time on 1.25 x the demand, 80% stays there.
    quiet();
    loads(busy, span, 85, 5);
    for (unsigned w = 0; w < 12; w++) step();
    loads(busy, span, 80, 5);
    for (unsigned w = 0; w < 12; w++) step();
    CHECK(c->level[0] > 3 && c->level[0] < 15 && c->gov.domain[0].asked == cpufreq::kWhyDemand);
    lowered(0, 2);
    now -= 20 * MS;
    step();
    CHECK(c->lowered == 1 && logged(", why demand, set ") && !logged(", why demand, set 0 ms"));
    fake.model.cap[0] = 15;
    // A touch: the level it asked for 0 ms ago, the check having waited as long as it may.
    quiet();
    fake.model.cap[0] = 1;                                      // another reading: the line comes when the reading changes
    c->lastCheck = now + 20 * MS - 6000 * MS;
    touches++;
    step();
    CHECK(c->level[0] == 9 && logged("asked lv9 2227MHz, limits lv0-1, why touch, set 0 ms ago"));
    fake.model.cap[0] = 15;
    // A launch, then a level the down rule holds: both on the line.
    quiet();
    touches++;
    step();
    procs++;
    step();
    CHECK(c->level[0] == 12 && c->gov.domain[0].asked == cpufreq::kWhyLaunch);
    fake.model.cap[0] = 0;
    c->lastCheck = now - 5000 * MS;
    step();
    CHECK(logged("asked lv12 2918MHz, limits lv0-0, why launch, set 20 ms ago"));
    fake.model.cap[0] = 15;
    // The 2nd burst window, then the run's 3rd: what asked and what decides now; a level asked for by the heat cap.
    quiet();
    for (unsigned w = 0; w < 300 && c->gov.launching; w++) step();
    quiet();
    loads(busy, span, 100, 5);
    step(); step();
    fake.model.cap[0] = 2;
    c->lastCheck = now - 5000 * MS;
    step();
    CHECK(logged("asked lv15 3532MHz, limits lv0-2, why burst 2, set 20 ms ago, now sat 3"));
    fake.model.cap[0] = 15;
    heat = cpufreq::GpuHeat{860, cpufreq::kGpuHeatValid | cpufreq::kGpuHeatLimiting};
    for (unsigned w = 0; w < 8; w++) step();
    CHECK(c->level[0] == 11 && c->gov.domain[0].why == cpufreq::kWhyHeatCap && c->gov.domain[0].asked == cpufreq::kWhyHeatCap);
    // Our cap alone: the platform at the level the cap asked for is no lowering, so "why heat cap" never stands for a cap of ours.
    const uint32_t beforeCap = c->lowered;
    c->lastCheck = now - 5000 * MS;
    step();
    CHECK(c->lastCheck == now && c->lowered == beforeCap && !logged("why heat cap"));
    // The firmware under that cap: the line, with `why` saying only what asked for lv11.
    fake.model.cap[0] = 1;
    c->lastCheck = now - 5000 * MS;
    step();
    CHECK(c->lowered == beforeCap + 1 && logged("asked lv11 2745MHz, limits lv0-1, why heat cap, set 180 ms ago"));
    // The load gone: the level the down rule still holds reads "hold".
    heat = cpufreq::GpuHeat{0, 0};
    fake.model.cap[0] = 15;
    for (unsigned w = 0; w < 4; w++) step();
    CHECK(c->level[0] == 15);
    fake.model.cap[0] = 0;
    loads(busy, span, 5, 5);
    for (unsigned w = 0; w < 12 && c->gov.domain[0].why != cpufreq::kWhyHold; w++) step();
    CHECK(c->gov.domain[0].why == cpufreq::kWhyHold && c->level[0] == 15 && cpufreq::controlCheck(c, now));
    CHECK(logged(", why demand, set ") && logged(" ms ago, now hold"));
    printf("ok  5 s check: not in a window with a level set (at most 1000 ms later), what asked for the level, how long ago and what decides now on "
           "its line: demand, touch, launch, burst, saturated, the heat cap\n");
}

// The launch boost (pagani-cpufreq-launch-ms, default 1500 ms): a process start up to 1000 ms after a touch frame or a driver's word hold
// the launch level (2918.4 / 3513.6 MHz) on both domains; its end, the 3-of-4 down rule after it, the gate's edges, what it leaves alone.
static void testLaunch()
{
    FakeCpucp fake;
    cpufreq::Control *c = control(&fake);
    uint64_t now = 1000 * MS;
    CHECK(cpufreq::controlStart(c, cpufreq::kModeGovernor, 0, now));
    const cpufreq::Domain &g0 = c->gov.domain[0], &g1 = c->gov.domain[1];
    CHECK(g0.launch == 12 && g1.launch == 11 && c->gov.launchMs == 1500 && g0.floor == 3 && g1.floor == 3);
    uint64_t busy[2], span[2];
    uint32_t touches = 0, procs = 81234, asks = 5;
    auto step = [&]() {
        now += 20 * MS;
        CHECK(cpufreq::controlWindow(c, now, busy, span, touches, 0, cpufreq::GpuHeat{0, 0}, cpufreq::ScreenInput{0, 0},
                                     cpufreq::LaunchInput{procs, asks}));
    };
    auto at = [&](uint32_t l0, uint32_t l1) { return c->level[0] == l0 && c->level[1] == l1; };
    loads(busy, span, 5, 5);
    // The counters' first reading is no event; processes that start with no touch before them are launchd's own.
    for (unsigned w = 0; w < 6; w++) step();
    CHECK(at(3, 3) && c->gov.launches == 0);
    for (unsigned w = 0; w < 30; w++) { procs += 2; step(); CHECK(at(3, 3)); }
    CHECK(c->gov.launches == 0 && !c->gov.launching);
    // A tap: the touch level. The app's fork 120 ms after the touch frame: both domains at the launch level in that window, logged.
    touches++;
    step();
    CHECK(at(9, 8));
    for (unsigned w = 0; w < 5; w++) step();
    CHECK(at(9, 8) && c->gov.launches == 0);
    procs++;
    step();
    CHECK(at(12, 11) && c->gov.launches == 1 && c->gov.launching && c->gov.launchUntil == now + 1500 * MS);
    CHECK(fake.sets(0).back() == 2918400 && fake.sets(1).back() == 3513600);
    CHECK(logged("cpufreq: launch 1: an app start 120 ms after a touch, 2918/3513 MHz for 1500 ms"));
    CHECK(row(c).find(" boost 1 thr 0 err 0 hot 0 app 1") != std::string::npos);
    // Its exec a window later renews the hold: no new boost, no new line, no message.
    const size_t sets0 = fake.sets(0).size(), sets1 = fake.sets(1).size();
    procs++;
    step();
    const uint64_t renewed = now;
    CHECK(at(12, 11) && c->gov.launches == 1 && c->gov.launchUntil == renewed + 1500 * MS && countLogged("cpufreq: launch ") == 1);
    // Held through the end of the touch hold at 5% load, to the end of the boost; then 3 windows to the floor, straight, one message.
    for (unsigned w = 1; w <= 80; w++) {
        step();
        const bool held = now < renewed + 1540 * MS;
        CHECK(c->level[0] == (held ? 12u : 3u) && c->level[1] == (held ? 11u : 3u));
        CHECK(c->gov.launching == (now < renewed + 1500 * MS));
    }
    CHECK(fake.sets(0).size() == sets0 + 1 && fake.sets(1).size() == sets1 + 1);
    CHECK(fake.sets(0).back() == 960000 && fake.sets(1).back() == 1689600);
    // The touch gate's edges: a start 1000 ms after the last touch frame counts, one 1020 ms after it does not.
    touches++;
    step();
    const uint64_t second = now;
    while (now + 20 * MS < second + 1000 * MS) step();
    procs++;
    step();
    CHECK(now == second + 1000 * MS && c->gov.launches == 2 && c->gov.launchGap == 1000 * MS && at(12, 11));
    for (unsigned w = 0; w < 80; w++) step();
    CHECK(at(3, 3) && !c->gov.launching);
    touches++;
    step();
    const uint64_t third = now;
    while (now + 20 * MS < third + 1020 * MS) step();
    procs++;
    step();
    CHECK(now == third + 1020 * MS && c->gov.launches == 2 && !c->gov.launching && at(3, 3));
    // A stream of process starts after one tap: renewed only while the gate lasts, so over 2500 ms after that tap at the latest.
    touches++;
    step();
    const uint64_t fourth = now;
    while (now < fourth + 2600 * MS) {
        procs++;
        step();
        CHECK(c->gov.launching == (now < fourth + 2500 * MS));
    }
    CHECK(c->gov.launches == 3);
    for (unsigned w = 0; w < 6; w++) step();
    CHECK(at(3, 3));
    // Load goes past the launch level by the load rules (a saturated run: the top), and back to it, not below, while the boost lasts.
    touches++;
    procs++;
    step();
    CHECK(at(12, 11) && c->gov.launches == 4);
    for (unsigned w = 0; w < 4; w++) { work(c, busy, span, 9000, 9000); step(); }
    CHECK(at(15, 15));
    loads(busy, span, 5, 5);
    for (unsigned w = 0; w < 20; w++) step();
    CHECK(at(12, 11) && c->gov.launching);
    for (unsigned w = 0; w < 70; w++) step();
    CHECK(at(3, 3) && !c->gov.launching);
    // A driver's word (pagani_cpufreq_boost): no touch needed, one boost however many words in a window, a word renews it.
    now += 6000 * MS;                                           // past the log's kLaunchLineMs
    asks += 3;
    step();
    const uint64_t asked = now;
    CHECK(at(12, 11) && c->gov.launches == 5 && c->gov.launchAsked && c->gov.launchUntil == asked + 1500 * MS);
    CHECK(logged("cpufreq: launch 5: a driver asked, 2918/3513 MHz for 1500 ms"));
    for (unsigned w = 0; w < 10; w++) step();
    asks++;
    step();
    CHECK(c->gov.launches == 5 && c->gov.launchUntil == now + 1500 * MS);
    for (unsigned w = 0; w < 80; w++) step();
    CHECK(at(3, 3));
    printf("ok  pagani-cpufreq-launch-ms: the counters' first reading and starts without a touch do nothing; a start up to 1000 ms after a "
           "touch frame: 2918.4/3513.6 MHz on both domains at once, renewed by the next start, at most 2500 ms after the tap; the load rules "
           "above it; a driver's word; held to its end, then 3 windows to the floor\n");

    // One line per kLaunchLineMs at most: 20 boosts 2 s apart, a line for every third (the count shows those left out).
    {
        FakeCpucp f;
        cpufreq::Control *o = control(&f);
        uint64_t t = 1000 * MS;
        CHECK(cpufreq::controlStart(o, cpufreq::kModeGovernor, 0, t));
        uint32_t tc = 1, ps = 1;
        for (unsigned w = 0; w < 1000; w++) {
            if (w % 100 == 50) { tc++; ps++; }
            t += 20 * MS;
            CHECK(cpufreq::controlWindow(o, t, busy, span, tc, 0, cpufreq::GpuHeat{0, 0}, cpufreq::ScreenInput{0, 0},
                                         cpufreq::LaunchInput{ps, 0}));
        }
        CHECK(o->gov.launches == 10 && countLogged("cpufreq: launch ") == 4);
        CHECK(logged("cpufreq: launch 1:") && !logged("cpufreq: launch 2:") && logged("cpufreq: launch 4:") && logged("cpufreq: launch 10:"));
    }
    // pagani-cpufreq-launch-ms=0: no boost, the levels of a twin without launch inputs. 60000 is cut at kLaunchMaxMs, 100 holds 100 ms.
    {
        FakeCpucp fa, fb;
        cpufreq::Control *a = control(&fa);
        a->launchMs = 0;
        cpufreq::Control *b = control(&fb);
        uint64_t t = 1000 * MS;
        CHECK(cpufreq::controlStart(a, cpufreq::kModeGovernor, 0, t) && logged(", launch off"));
        CHECK(cpufreq::controlStart(b, cpufreq::kModeGovernor, 0, t));
        uint32_t tc = 0, ps = 0, ak = 0;
        uint64_t rng = 2024;
        auto rnd = [&](uint64_t n) { rng = rng * 6364136223846793005ull + 1442695040888963407ull; return (rng >> 33) % n; };
        for (unsigned w = 0; w < 3000; w++) {
            t += 20 * MS;
            if (rnd(15) == 0) tc++;
            if (rnd(10) == 0) ps++;
            if (rnd(50) == 0) ak++;
            loads(busy, span, (uint32_t)rnd(101), (uint32_t)rnd(101));
            CHECK(cpufreq::controlWindow(a, t, busy, span, tc, 0, cpufreq::GpuHeat{0, 0}, cpufreq::ScreenInput{0, 0},
                                         cpufreq::LaunchInput{ps, ak}));
            CHECK(cpufreq::controlWindow(b, t, busy, span, tc, 0));
            CHECK(a->level[0] == b->level[0] && a->level[1] == b->level[1]);
        }
        CHECK(a->gov.launches == 0 && !a->gov.launching);
    }
    for (uint32_t ms : {60000u, 100u}) {
        FakeCpucp f;
        cpufreq::Control *o = control(&f);
        o->launchMs = ms;
        CHECK(cpufreq::controlStart(o, cpufreq::kModeGovernor, 0, 1000 * MS));
        const uint32_t hold = ms > cpufreq::kLaunchMaxMs ? cpufreq::kLaunchMaxMs : ms;
        CHECK(o->gov.launchMs == hold && logged(format("launch 2918/3513 MHz for %u ms", hold).c_str()));
    }
    // pagani-cpufreq-min above the launch level: the floor stands; pagani-cpufreq=1 reads none of it.
    {
        FakeCpucp f;
        cpufreq::Control *o = control(&f);
        CHECK(cpufreq::controlStart(o, cpufreq::kModeGovernor, 3100, 1000 * MS));
        CHECK(o->gov.domain[0].floor == 14 && o->gov.domain[0].launch == 14 && o->gov.domain[1].floor == 10 && o->gov.domain[1].launch == 11);
    }
    {
        FakeCpucp f;
        cpufreq::Control *o = control(&f);
        uint64_t t = 1000 * MS;
        CHECK(cpufreq::controlStart(o, cpufreq::kModeMax, 0, t));
        for (uint32_t w = 0; w < 10; w++) {
            t += 20 * MS;
            CHECK(cpufreq::controlWindow(o, t, busy, span, w, 0, cpufreq::GpuHeat{0, 0}, cpufreq::ScreenInput{0, 0},
                                         cpufreq::LaunchInput{w, w}));
        }
        CHECK(o->level[0] == 15 && o->level[1] == 15 && !logged("cpufreq: launch"));
    }
    printf("ok  launch boost: one line per 5 s at most, off with pagani-cpufreq-launch-ms=0 (the levels of a governor without it), cut at "
           "5000 ms, under pagani-cpufreq-min, ignored by pagani-cpufreq=1\n");
}

// The caps over the launch boost. The GPU limited by heat (adreno830_thermal_state): the boost stops at the caps (2745.6 / 3072 MHz) in
// every window, a touch's 100 ms lift included, and comes back above them only with the flag; the caps at once when the flag comes
// during a boost; the emergency holds them through a touch and through the load rules, and raises no floor: no boost begins in it and one
// under way ends. The platform's own limit (a level the CPUCP lowers) is only read and reported: the boost does not ask again. The
// display off, with or without pagani-screenoff bit 0: no boost, and one under way ends.
// Daemons near a touch (nextpidversion moves, no app or extension bundle): no boost, the relaunches of a job that exits at once least of all;
// a real launch's own daemons renew the boost it began, near a touch, and only then.
static void testLaunchDaemons()
{
    FakeCpucp fake;
    cpufreq::Control *c = control(&fake);
    uint64_t now = 1000 * MS;
    CHECK(cpufreq::controlStart(c, cpufreq::kModeGovernor, 0, now));
    uint64_t busy[2], span[2];
    uint32_t touches = 0, apps = 41, spawns = 900;
    auto step = [&]() {
        now += 20 * MS;
        CHECK(cpufreq::controlWindow(c, now, busy, span, touches, 0, cpufreq::GpuHeat{0, 0}, cpufreq::ScreenInput{0, 0},
                                     cpufreq::LaunchInput{apps, 0, 0, spawns}));
    };
    loads(busy, span, 5, 5);
    for (unsigned w = 0; w < 6; w++) step();
    // The job that exits at once, relaunched every 10 s through a stretch of tapping: fork, exec of xpcproxy, exec of the daemon.
    for (unsigned beat = 0; beat < 25; beat++) {
        for (unsigned w = 0; w < 500; w++) {
            if (w % 50 == 0) touches++;
            if (w == 3) spawns += 3;
            step();
            CHECK(!c->gov.launching);
        }
    }
    CHECK(c->gov.launches == 0 && !logged("cpufreq: launch "));
    // The app: its exec begins the boost, even in the window of that beat.
    touches++;
    step();
    spawns += 6; apps++;
    step();
    CHECK(c->gov.launches == 1 && c->gov.launching);
    const uint64_t begun = c->gov.launchUntil;
    CHECK(begun == now + 1500 * MS);
    // A daemon it wakes renews it, near the touch; one a touch-free 1000 ms later no longer does and the boost ends on time.
    for (unsigned w = 0; w < 10; w++) step();
    spawns += 3;
    step();
    const uint64_t renewed = c->gov.launchUntil;
    CHECK(renewed == now + 1500 * MS && renewed > begun && c->gov.launches == 1);
    while (now - c->gov.touchAt <= 1000 * MS) step();
    spawns += 3;
    step();
    CHECK(c->gov.launchUntil == renewed);
    while (now < renewed) step();
    CHECK(!c->gov.launching);
    // Over: a daemon near a new touch begins nothing.
    touches++;
    step();
    spawns += 3;
    step();
    CHECK(c->gov.launches == 1 && !c->gov.launching);
    printf("ok  launch boost: 25 relaunches of a daemon through tapping begin no boost, an app's exec does in the same window; a daemon renews "
           "a boost under way near a touch, no other\n");
}

static void testLaunchCaps()
{
    FakeCpucp fake;
    cpufreq::Control *c = control(&fake);
    uint64_t now = 1000 * MS;
    CHECK(cpufreq::controlStart(c, cpufreq::kModeGovernor, 0, now));
    const cpufreq::GpuHeat cool = {450, cpufreq::kGpuHeatValid};
    const cpufreq::GpuHeat hot = {853, cpufreq::kGpuHeatValid | cpufreq::kGpuHeatLimiting};
    const cpufreq::GpuHeat burning = {962, cpufreq::kGpuHeatValid | cpufreq::kGpuHeatLimiting | cpufreq::kGpuHeatEmergency};
    cpufreq::GpuHeat heat = cool;
    uint64_t busy[2], span[2];
    uint32_t touches = 0, procs = 0;
    auto step = [&]() {
        now += 20 * MS;
        CHECK(cpufreq::controlWindow(c, now, busy, span, touches, 0, heat, cpufreq::ScreenInput{0, 0}, cpufreq::LaunchInput{procs, 0}));
    };
    auto at = [&](uint32_t l0, uint32_t l1) { return c->level[0] == l0 && c->level[1] == l1; };
    loads(busy, span, 5, 5);
    for (unsigned w = 0; w < 6; w++) step();
    CHECK(at(3, 3));
    // Limited before the tap: within the touch's lift the caps do not apply, the boost stops at them all the same.
    heat = hot;
    touches++;
    step();
    CHECK(at(9, 8) && !c->gov.capped);
    procs++;
    step();
    CHECK(at(11, 9) && c->gov.launching && !c->gov.capped);
    CHECK(logged("cpufreq: launch 1: an app start 20 ms after a touch, 2745/3072 MHz for 1500 ms"));
    for (unsigned w = 0; w < 3; w++) { step(); CHECK(at(11, 9) && c->gov.launching && !c->gov.capped); }
    for (unsigned w = 0; w < 30; w++) { step(); CHECK(at(11, 9) && c->gov.launching && c->gov.capped); }
    // The flag clears during the boost: the launch level in that window. It comes back: the caps at once, past the down rule.
    heat = cool;
    step();
    CHECK(at(12, 11) && c->gov.launching);
    heat = hot;
    step();
    CHECK(at(11, 9) && c->gov.capped);
    // The emergency: a touch does not lift the caps, nor does the load past them, and the boost under way ends.
    heat = burning;
    touches++;
    step();
    CHECK(at(11, 9) && c->gov.capped && !c->gov.launching && c->gov.launchUntil == 0);
    for (unsigned w = 0; w < 6; w++) { work(c, busy, span, 9000, 9000); step(); CHECK(at(11, 9)); }
    loads(busy, span, 5, 5);
    for (unsigned w = 0; w < 90; w++) step();
    CHECK(at(3, 3) && !c->gov.launching && c->gov.heatCaps == 2);
    // A start in the emergency begins no boost: at 5% load the touch hold, then the floor, and nothing near the caps. (With a boost, 2.3 s
    // at 2745.6 / 3072 MHz at the moment the CPU should give way.)
    const uint32_t before = c->gov.launches;
    touches++;
    procs++;
    step();
    CHECK(!c->gov.launching && c->gov.launches == before && at(9, 8));
    unsigned above = 0;
    for (unsigned w = 0; w < 120; w++) { step(); CHECK(c->level[0] <= 9 && c->level[1] <= 8 && !c->gov.launching); above += !at(3, 3); }
    CHECK(above <= 250 / 20 + 4 && at(3, 3) && c->gov.launches == before);
    // The flag gone, the next start boosts again.
    heat = cool;
    for (unsigned w = 0; w < 6; w++) step();
    touches++;
    procs++;
    step();
    CHECK(c->gov.launching && c->gov.launches == before + 1 && at(12, 11));
    for (unsigned w = 0; w < 100; w++) step();
    CHECK(at(3, 3) && !c->gov.launching);
    // Limited, with a saturated load and a boost, no touch for 100 ms: the caps in every window, and in every set of this run.
    const size_t limitedSets[2] = {fake.sets(0).size(), fake.sets(1).size()};
    heat = hot;
    touches++;
    procs++;
    step();
    for (unsigned w = 0; w < 5; w++) step();                    // past the lift
    for (unsigned w = 0; w < 40; w++) { work(c, busy, span, 9000, 9000); step(); CHECK(at(11, 9) && c->gov.capped && c->gov.launching); }
    loads(busy, span, 5, 5);
    for (unsigned w = 0; w < 60; w++) step();
    CHECK(at(3, 3));
    fake.settle();
    for (unsigned d = 0; d < 2; d++) {
        const std::vector<uint32_t> all = fake.sets(d);
        CHECK(all.size() > limitedSets[d]);
        for (size_t i = limitedSets[d]; i < all.size(); i++) CHECK(all[i] <= (d ? 3072000u : 2745600u));
    }
    heat = cool;
    for (unsigned w = 0; w < 6; w++) step();
    // The platform's limit: a level the CPUCP lowers is counted and logged at the 5 s check, never asked again. The check falls inside
    // the boost, which goes on.
    fake.model.cap[0] = 9;
    now = c->lastCheck + 4900 * MS;
    step();
    touches++;
    procs++;
    step();
    CHECK(at(12, 11) && c->gov.launching && c->lowered == 0);
    const size_t sets = fake.sets(0).size();
    for (unsigned w = 0; w < 6 && !c->lowered; w++) step();
    fake.settle();
    CHECK(c->lowered == 1 && c->gov.launching && at(12, 11));
    CHECK(logged("cpufreq: d0 at lv9 2227MHz, asked lv12 2918MHz, limits lv0-9") && fake.sets(0).size() == sets);
    for (unsigned w = 0; w < 20; w++) { step(); CHECK(c->gov.launching && fake.sets(0).size() == sets); }
    fake.model.cap[0] = 15;
    printf("ok  launch boost under the heat caps: 2745.6/3072 MHz in every window while the GPU is limited, a touch's lift included; above "
           "them only with the flag gone; the caps at once when it comes; the emergency through touches and load; the platform's limit "
           "only read\n");

    // The display off (pagani-screenoff bit 0): nothing starts a boost, a boost under way ends and the screen-off caps hold.
    {
        FakeCpucp f;
        cpufreq::Control *o = control(&f);
        o->screen = true;
        uint64_t t = 1000 * MS;
        CHECK(cpufreq::controlStart(o, cpufreq::kModeGovernor, 0, t));
        uint32_t tc = 0, ps = 0, ak = 0, state = 0;
        auto window = [&]() {
            t += 20 * MS;
            CHECK(cpufreq::controlWindow(o, t, busy, span, tc, 0, cpufreq::GpuHeat{0, 0}, cpufreq::ScreenInput{state, 0},
                                         cpufreq::LaunchInput{ps, ak}));
        };
        loads(busy, span, 5, 5);
        for (unsigned w = 0; w < 6; w++) window();
        tc++; ps++;
        window();
        CHECK(o->gov.launching && o->level[0] == 12 && o->level[1] == 11);
        state = 1;
        window();
        CHECK(!o->gov.launching && o->gov.launchUntil == 0);
        for (unsigned w = 0; w < 6; w++) window();
        CHECK(o->level[0] == 3 && o->level[1] == 3);
        for (unsigned w = 0; w < 60; w++) {
            if (w % 7 == 0) { tc++; ps++; ak++; }
            window();
            CHECK(!o->gov.launching && o->gov.launches == 1);
        }
        CHECK(o->level[0] <= 3 && o->level[1] <= 3);
        // Dark and quiet: lowered, nothing lifts it but what the policy names; the display back: boosts again.
        for (unsigned w = 0; w < 60; w++) window();
        CHECK(o->gov.lowered && o->level[0] == 0 && o->level[1] == 0);
        state = 2;
        window();
        ps++;
        window();
        CHECK(!o->gov.lowered && o->gov.launches == 1);         // the gate: the last touch was over 1000 ms ago
        tc++; ps++;
        window();
        CHECK(o->gov.launches == 2 && o->level[0] == 12 && o->level[1] == 11);
    }
    // Without the policy, as in every package: the same from pagani_screen_state alone. Touches still come with the display off, and
    // nothing of the policy runs (no caps, no count of periods).
    {
        FakeCpucp f;
        cpufreq::Control *o = control(&f);
        uint64_t t = 1000 * MS;
        CHECK(cpufreq::controlStart(o, cpufreq::kModeGovernor, 0, t));
        uint32_t tc = 0, ps = 0, ak = 0, state = 0;
        auto window = [&]() {
            t += 20 * MS;
            CHECK(cpufreq::controlWindow(o, t, busy, span, tc, 0, cpufreq::GpuHeat{0, 0}, cpufreq::ScreenInput{0, 0},
                                         cpufreq::LaunchInput{ps, ak, state}));
        };
        loads(busy, span, 5, 5);
        for (unsigned w = 0; w < 6; w++) window();
        tc++; ps++;
        window();
        CHECK(o->gov.launching && o->level[0] == 12 && o->level[1] == 11);
        state = 3;                                              // the count above bit 0 moved too
        window();
        CHECK(!o->gov.launching && o->gov.launchUntil == 0 && !o->gov.screenOff && !o->gov.offs);
        for (unsigned w = 0; w < 40; w++) {
            if (w % 7 == 0) { tc++; ps++; ak++; }
            window();
            CHECK(!o->gov.launching && o->gov.launches == 1);
        }
        for (unsigned w = 0; w < 30; w++) window();
        state = 4;
        tc++; ps++;
        window();
        CHECK(o->gov.launching && o->gov.launches == 2);
    }
    // A touch with the display off lifts no heat cap: nobody is looking (the policy on, the GPU limited by heat).
    {
        FakeCpucp f;
        cpufreq::Control *o = control(&f);
        o->screen = true;
        uint64_t t = 1000 * MS;
        CHECK(cpufreq::controlStart(o, cpufreq::kModeGovernor, 0, t));
        uint32_t tc = 0, state = 0;
        auto window = [&]() {
            t += 20 * MS;
            CHECK(cpufreq::controlWindow(o, t, busy, span, tc, 0, hot, cpufreq::ScreenInput{state, 0}, cpufreq::LaunchInput{0, 0}));
        };
        loads(busy, span, 5, 5);
        for (unsigned w = 0; w < 10; w++) window();
        CHECK(o->gov.capped);
        tc++;
        window();
        CHECK(!o->gov.capped);                                  // the lift, display on
        for (unsigned w = 0; w < 10; w++) window();
        CHECK(o->gov.capped);
        state = 1;
        window();
        tc++;
        window();
        CHECK(o->gov.capped);
    }
    // A driver's request needs no touch, not even one since boot.
    {
        FakeCpucp f;
        cpufreq::Control *o = control(&f);
        uint64_t t = 1000 * MS;
        CHECK(cpufreq::controlStart(o, cpufreq::kModeGovernor, 0, t));
        uint32_t ak = 0;
        auto window = [&]() {
            t += 20 * MS;
            CHECK(cpufreq::controlWindow(o, t, busy, span, 0, 0, cool, cpufreq::ScreenInput{0, 0}, cpufreq::LaunchInput{0, ak}));
        };
        loads(busy, span, 5, 5);
        for (unsigned w = 0; w < 6; w++) window();
        CHECK(!o->gov.touchAt && !o->gov.launching);
        ak++;
        window();
        CHECK(o->gov.launching && o->gov.launchAsked && o->gov.launches == 1 && !o->gov.touchAt);
    }
    // The row as CpuFreqGovernor.h and the README show it, 65 characters; with one more the launch count is left out.
    {
        FakeCpucp f;
        cpufreq::Control *o = control(&f);
        CHECK(cpufreq::controlStart(o, cpufreq::kModeGovernor, 0, 1000 * MS));
        o->level[0] = 15; o->level[1] = 15; o->load[0] = 4; o->load[1] = 12;
        o->gov.boosts = 3; o->gov.heatCaps = 1; o->gov.launches = 2;
        CHECK(row(o) == "CPU 3532/4320MHz lv15/15 ld 4/12% boost 3 thr 0 err 0 hot 1 app 2" && row(o).size() == cpufreq::kRowWidth);
        o->load[0] = 34;
        CHECK(row(o) == "CPU 3532/4320MHz lv15/15 ld 34/12% boost 3 thr 0 err 0 hot 1");
    }
    printf("ok  launch boost with the display off, with or without pagani-screenoff bit 0: none starts, one under way ends, back with the display; "
           "no heat lift while dark; a driver's request without a touch; the row's example\n");
}

// SCMI BUSY during a boost: a BUSY answer is retried, never the end of the governor. BUSY at the level set that starts it (1, 3, 5 in a
// row, 620 ms of backoff at most): no message while each backoff lasts, the level through when the burst ends, the end of the boost not
// moved; BUSY at the level set that ends it; a 5 s check met by BUSY in the middle of it (pkgD2): on, resumed, the boost goes on.
static void testLaunchBusy()
{
    for (unsigned burst : {1u, 3u, 5u}) {
        FakeCpucp fake;
        cpufreq::Control *c = control(&fake);
        uint64_t now = 1000 * MS;
        CHECK(cpufreq::controlStart(c, cpufreq::kModeGovernor, 0, now));
        uint64_t busy[2], span[2];
        uint32_t touches = 0, procs = 0;
        loads(busy, span, 5, 5);
        auto window = [&]() {
            now += 20 * MS;
            return cpufreq::controlWindow(c, now, busy, span, touches, 0, cpufreq::GpuHeat{0, 0}, cpufreq::ScreenInput{0, 0},
                                          cpufreq::LaunchInput{procs, 0});
        };
        for (unsigned w = 0; w < 6; w++) CHECK(window());
        CHECK(c->level[0] == 3 && c->level[1] == 3);
        fake.settle();
        const size_t s0 = fake.sets(0).size();
        fake.setFaults({busyOn(scmi::kLevelSet, 0, burst)});
        touches++;
        procs++;
        CHECK(window() && c->on && c->gov.launches == 1);
        const uint64_t until = c->gov.launchUntil;
        CHECK(until == now + 1500 * MS && c->level[0] == 3 && (c->retry.pending & 1));
        unsigned quiet = 0;
        for (unsigned w = 0; w < 200 && c->level[0] != 12; w++) {
            const bool backoff = now + 20 * MS < c->retry.at;
            const unsigned rung = fake.rung;
            CHECK(window() && c->on);
            fake.settle();
            if (backoff) { CHECK(fake.rung == rung); quiet++; }
        }
        fake.settle();
        unsigned quietWanted = 0;
        for (unsigned i = 1; i <= burst; i++) quietWanted += cpufreq::retryMs(i) / 20 - 1;
        CHECK(quiet == quietWanted);
        CHECK(c->level[0] == 12 && c->level[1] == 11 && c->gov.launchUntil == until && c->gov.launches == 1);
        CHECK(fake.model.requested[0] == 12 && fake.model.requested[1] == 11 && c->retry.pending == 0);
        CHECK(fake.sets(0).size() == s0 + burst + 1 && fake.sets(0).back() == 2918400);
        CHECK(row(c).find(format(" err %u ", burst)) != std::string::npos);
        // BUSY at the level set that ends the boost: the floor through the same way.
        while (now + 20 * MS < until) CHECK(window());
        fake.settle();
        fake.setFaults({busyOn(scmi::kLevelSet, 0, burst)});
        for (unsigned w = 0; w < 200 && c->level[0] != 3; w++) CHECK(window() && c->on);
        for (unsigned w = 0; w < 6; w++) CHECK(window() && c->on);
        fake.settle();
        CHECK(c->level[0] == 3 && c->level[1] == 3 && fake.model.requested[0] == 3 && fake.model.requested[1] == 3 && c->retry.pending == 0);
        CHECK(!logged("cpufreq: off"));
    }
    // pkgD2's BUSY at a 5 s check, with a boost under way: on, the check resumes after 20 ms, the level untouched.
    {
        FakeCpucp fake;
        cpufreq::Control *c = control(&fake);
        uint64_t now = 1000 * MS;
        CHECK(cpufreq::controlStart(c, cpufreq::kModeGovernor, 0, now));
        uint64_t busy[2], span[2];
        uint32_t touches = 0, procs = 0;
        loads(busy, span, 5, 5);
        auto window = [&]() {
            now += 20 * MS;
            return cpufreq::controlWindow(c, now, busy, span, touches, 0, cpufreq::GpuHeat{0, 0}, cpufreq::ScreenInput{0, 0},
                                          cpufreq::LaunchInput{procs, 0});
        };
        while (now + 40 * MS - c->lastCheck < 5000 * MS) CHECK(window());
        touches++;
        procs++;
        CHECK(window() && c->level[0] == 12 && c->level[1] == 11);
        fake.settle();
        fake.setFaults({busyOn(scmi::kLevelGet, 0, 1)});
        CHECK(window() && c->on && c->lastCheck == 1000 * MS && logged("cpufreq: retry: status -6 lvl get d0, next in 20 ms"));
        CHECK(window() && c->on && c->lastCheck == now);
        fake.settle();
        CHECK(c->level[0] == 12 && c->level[1] == 11 && c->gov.launching && !logged("cpufreq: off") && c->retry.busy == 1);
    }
    printf("ok  SCMI BUSY during a launch boost: nothing sent in a backoff, the level through when the burst ends, the boost's end not "
           "moved, BUSY at its end and at a 5 s check in the middle of it retried the same way\n");
}

// An app's launch on the workload model (10 ms idle ticks, windows of 20-21 ms): a tap, the process start 120 ms after the last touch
// frame, then 1000 ms of work no CPU saturates at first: the render server's 120 Hz frames (6 ms each at 3 GHz) with five CPUs at 700 MHz
// of work beside them. Without a boost the frames wait for the 160 ms average; with it they run at the launch level from the process
// start on. The boost ends on time after the work.
static void testLaunchScene()
{
    unsigned late[2] = {0, 0}, frames[2] = {0, 0};
    double average[2] = {0, 0};
    for (unsigned boost = 0; boost < 2; boost++) {
        FakeCpucp fake;
        cpufreq::Control *c = control(&fake);
        c->launchMs = boost ? cpufreq::kLaunchMs : 0;
        uint64_t now = 1000 * MS;
        CHECK(cpufreq::controlStart(c, cpufreq::kModeGovernor, 0, now));
        Workload m;
        unsigned touches = 0, hints = 0, windows = 0;
        double t0 = 0;
        runScene(c, &m, &now, 400, [](double) { Scene s; s.rate[0] = s.rate[1] = 10; return s; }, &touches, &hints,
                 [](double, const Scene &) {}, &t0);
        const unsigned lateBefore = m.framesLate, doneBefore = m.framesDone;
        runScene(c, &m, &now, 1800, [](double t) {
            Scene s;
            s.rate[0] = s.rate[1] = 10;
            s.touching = t < 60;
            s.procs = t >= 180 ? 1 : 0;
            if (t >= 180 && t < 1180) {
                s.rate[0] = 700; s.others[0] = 700; s.frameCost[0] = 18000; s.submitting = true;
            }
            return s;
        }, &touches, &hints, [&](double t, const Scene &) {
            if (t < 180 || t >= 1180) return;
            windows++;
            average[boost] += c->perf.domain[0].levels[c->level[0]].khz / 1000.0;
        }, &t0);
        late[boost] = m.framesLate - lateBefore;
        frames[boost] = m.framesDone - doneBefore;
        average[boost] /= windows;
        CHECK(c->gov.launches == boost && !c->gov.launching);
    }
    CHECK(late[1] < late[0] && frames[1] == frames[0]);
    printf("    a launch on the model: %u of %u frames late without the boost at %.0f MHz on average, %u with it at %.0f MHz\n", late[0],
           frames[0], average[0], late[1], average[1]);
    printf("ok  a launch on the workload model through controlWindowCpus: the boost starts with the process, ends with its hold\n");
}

int main()
{
    testEncoding();
    testProbe();
    testV4Indexing();
    testFailures();
    testMax();
    testGovernor();
    testHeat();
    testScreenOff();
    testScenarios();
    testProperties();
    testScreenProperties();
    testExtremes();
    testSampling();
    testFloorAndRow();
    testRetryRules();
    testBusyCheck();
    testBusyBursts();
    testBusyForever();
    testTimeouts();
    testStartRetry();
    testLimitsTransient();
    testMaxRetry();
    testRetryRationing();
    testCheckLeftOut();
    testRetryRandom();
    testNoRetry();
    testTouchHold();
    testCheckLine();
    testLaunch();
    testLaunchDaemons();
    testLaunchCaps();
    testLaunchBusy();
    testLaunchScene();
    printf("PASS: pagani-cpufreq SCMI PERF client and governor with the GPU heat caps, the screen-off policy, the touch hold, the launch boost "
           "and the retries of transient SCMI failures, %u checks against a fake CPUCP\n", checks);
    return 0;
}
