// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test of PaganiPanel's register probe and its writes (PanelProbe.h), against a model of the display's registers on a virtual clock:
//  - no write: the probe's write callback aborts on every window but A1's, on every run; A0 and A1 get one register each, one bit, and only when it reads clear;
//  - what it reads: every address on the list below, written out here again from the plan rather than from the header; nothing else;
//  - gates: GCC off leaves the clock controller alone, the controller's own branches and the core GDSC what they clock, each read after the
//    sleep before its block; a controller that reads as zero is not one with its clocks on; CFG_GDSCR bit 16 is the core GDSC's bit, not GDSCR bit 31;
//  - breadcrumbs: on every run, a block's line and PANEL_PROBE_CRUMB_MS come before its first read;
//  - the UFS log's wire: every read comes with the writer held for PANEL_PROBE_QUIET_MS at the least, a held writer gets each block's line away first (it
//    was let go before it), and nothing is left held when the probe ends;
//  - time: the stages' moments, 20 us between DSI groups, 250 us between phase samples, the duration logged, P3 done before the 420 s reset;
//    a thread that loses the CPU keeps the samples apart, shows in the log, and more than a frame of it closes P3;
//  - lines: the longest a format can print stays under PANEL_PROBE_LINE bytes;
//  - the windows the kernel maps for a set of stages are the only ones it reads;
//  - A1, against the 13T's registers as the first run read them: each gate alone, before the sleep and after it; the order of a hang's last lines; what a
//    clock that does not start leaves unread; DSI and INTF_1 behind the clock in the model, so the probe sees what the write opens; the end before a 240 s reset;
//  - bit 8, start as the kernel runs it on that model: the read list; careful, each step's line off the phone before the step; with bit 12, one hold for all of it; every gate of A1
//    alone, each way the clock or the command can fail, the read back and the frames: no write behind any of them, and the writer let go on every way out.
//   xcrun clang++ -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
//       -I <UniXNU-sm8750-common>/kits/Sm8750Platform -I <device>/package Test/panel-probe.cpp -o /tmp/panel-probe && /tmp/panel-probe
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include "../PanelBrightness.h"

static unsigned checks;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); std::abort(); } checks++; } while (0)

enum : uint64_t { TICKS_S = 19200000, READ_COST = 4, MS = 19200 };
static const unsigned ALL = PANEL_PROBE_P0 | PANEL_PROBE_P1 | PANEL_PROBE_P2 | PANEL_PROBE_P3;

// The whole list of what may be read, by stage: the plan's table with the review's cuts (no MDSS top, no DSI1, no CTL, SSPP or SMMU).
static const std::set<uint64_t> allowed = {
    0x127004, 0x127008,                                                         // GCC_DISP_AHB_CLK, GCC_DISP_HF_AXI_CLK
    0xf156004,                                                                  // TE, GPIO 86
    0xae94000, 0xae94004, 0xae94008,                                            // DSI0 version, ctrl, status
    0xaf09000, 0xaf09004, 0xaf0b000, 0xaf0b004, 0xaf0f000,                      // GDSCs, misc
    0xaf080b0, 0xaf08360, 0xaf08364, 0xaf0c004, 0xaf0c00c, 0xaf0c008,           // AHB and its RCG, non-GDSC, RSCC
    0xaf08010, 0xaf08150, 0xaf08154, 0xaf08020, 0xaf08030, 0xaf00000, 0xaf01000,// MDP and its RCG, LUT, vsync, PLLs
    0xaf08034, 0xaf08038, 0xaf08004, 0xaf08044, 0xaf08180, 0xaf08108, 0xaf081b8,// link clocks and RCGs
    0xaf202d0,                                                                  // RSCC power status
    0xaf30000, 0xaf30024, 0xaf30040,                                            // wrapper
    0xaf31000, 0xaf31004, 0xaf31008, 0xaf31010,                                 // SCC
    0xaf2780c, 0xaf27818, 0xaf27840, 0xaf27a80, 0xaf27aa8,                      // CRMC
    0xae95000, 0xae95004, 0xae95008, 0xae9500c,                                 // PHY
    0xae36000, 0xae360ac, 0xae36298, 0xae362b4,                                 // INTF_1
};
// What A1 reads beyond them: DSI as panel: start reads it, DISP_CC_MDSS_AHB1_CLK, the first register of MDSS. Nothing else of MDSS top, its interrupt status above all.
static const std::set<uint64_t> allowedA1 = {
    0xae9400c, 0xae9403c, 0xae94084, 0xae940a8, 0xae94110, 0xae9411c, 0xae9415c, 0xae94068, 0xae940c0, 0xae942cc, 0xae94330,
    0xaf0a028, 0xae00000,
};
// All bit 8 reads, in start: A1's state and its phase, the gates of DSI, DSI as panel: start reads it, and what the send path saves. Nothing of the PHY, INTF_1 or MDSS.
static const std::set<uint64_t> allowedFormal = {
    0x127004,
    0xaf080b0, 0xaf0a028, 0xaf08360, 0xaf08364, 0xaf09000, 0xaf09004, 0xaf08010, 0xaf08034, 0xaf08004, 0xaf0c00c,
    0xae94000, 0xae94004, 0xae94008, 0xae9400c, 0xae9403c, 0xae94084, 0xae940a8, 0xae94110, 0xae9411c, 0xae9415c, 0xae94068, 0xae940c0, 0xae942cc, 0xae94330,
    0xae94390, 0xae9404c,
};

struct Event {
    enum Kind { Read, Line, Sleep, Quiet, Write } kind;
    uint64_t at;
    unsigned block = 0;
    uint64_t address = 0;
    std::string text = {};
    unsigned ms = 0;
    Event(Kind k, uint64_t t) : kind(k), at(t) {}
};

// One boot of the model. Every register reads through `value`, so a test can change any of them at any time; the defaults are a healthy display
// with its registers answering.
struct Soc {
    uint64_t now = 3 * TICKS_S / 2;          // the thread starts at about 1.5 s
    uint64_t start = now;
    std::vector<Event> events;
    std::map<uint64_t, uint32_t> regs;
    std::map<uint64_t, std::function<uint32_t(uint64_t)>> live;     // register -> value at ticks
    std::function<unsigned(uint64_t)> display = [](uint64_t) { return 1u; };
    unsigned stages = ALL;                   // what the kernel maps: a read of anything else is a read of a null window
    uint32_t reads = 0;
    size_t longest = 0;
    unsigned quiet = 0;                      // the UFS log's writer is held
    uint64_t quietSince = 0;
    std::vector<std::pair<uint64_t, uint32_t>> writes;
    std::function<void(uint32_t)> wrote = [](uint32_t) {};
    std::vector<std::pair<uint32_t, uint32_t>> dsiWrites;       // bit 8's command, the only writes to DSI anywhere here
    std::function<void(uint32_t, uint32_t)> dsiWrote = [](uint32_t, uint32_t) {};
    Soc() {
        regs[0x127004] = 1; regs[0x127008] = 0x80000001;
        regs[0xae94000] = 0x20090000; regs[0xae94004] = 0x1f5;
        regs[0xaf09000] = 0x80000000; regs[0xaf09004] = 0x10000;
        for (uint64_t branch : {0xaf080b0, 0xaf08010, 0xaf08034, 0xaf08038, 0xaf08004, 0xaf08044, 0xaf08020, 0xaf08030, 0xaf0c004, 0xaf0c008, 0xaf0c00c}) regs[branch] = 1;
        regs[0xae95000] = 0x20; regs[0xae36000] = 1; regs[0xae362b4] = 0x80000001; regs[0xaf202d0] = 0x1000;
        // Frames: 120 Hz, TE high for a tenth of the period; the frame count goes with them.
        live[0xf156004] = [](uint64_t t) { return (t % (TICKS_S / 120)) < TICKS_S / 1200 ? 1u : 0u; };
        live[0xae360ac] = [](uint64_t t) { return (uint32_t)(t / (TICKS_S / 120)); };
    }
    uint32_t value(uint64_t address) {
        auto l = live.find(address);
        if (l != live.end()) return l->second(now);
        auto r = regs.find(address);
        return r == regs.end() ? 0 : r->second;
    }
    size_t count(uint64_t address) const {
        size_t n = 0;
        for (auto &e : events) n += e.kind == Event::Read && e.address == address;
        return n;
    }
    size_t countBlock(unsigned block) const {
        size_t n = 0;
        for (auto &e : events) n += e.kind == Event::Read && e.block == block;
        return n;
    }
    bool logged(const char *text) const { return find(text) != nullptr; }
    const Event *find(const char *text) const {
        for (auto &e : events) if (e.kind == Event::Line && e.text.find(text) != std::string::npos) return &e;
        return nullptr;
    }
    std::string line(const char *text) const {
        const Event *e = find(text);
        return e ? e->text : "";
    }
    uint64_t secondsAt(const Event *e) const { return e->at / TICKS_S; }
    void print() const { for (auto &e : events) if (e.kind == Event::Line) std::printf("  %7.3f %s\n", (double)e.at / TICKS_S, e.text.c_str()); }
};

struct Port { Soc *soc; unsigned block; };
static uint32_t portRead(void *context, uint32_t reg) {
    auto &port = *static_cast<Port *>(context);
    Soc &s = *port.soc;
    const panel_probe_window &window = panel_probe_windows[port.block];
    const uint64_t address = window.base + reg;
    CHECK(window.stages & s.stages);         // mapped for these stages
    CHECK(reg < window.size);
    if (s.stages == PANEL_PROBE_AHB) CHECK(allowedFormal.count(address));
    else CHECK(allowed.count(address) || ((s.stages & PANEL_PROBE_A1) && allowedA1.count(address)));
    CHECK(port.block != PANEL_BLOCK_AHB);    // that page is for the one write
    CHECK(s.quiet && s.now >= s.quietSince + PANEL_PROBE_QUIET_MS * MS);
    s.now += READ_COST;
    ++s.reads;
    Event e{Event::Read, s.now};
    e.block = port.block;
    e.address = address;
    s.events.push_back(e);
    return s.value(address);
}
static void portWrite(void *, uint32_t reg, uint32_t value) {
    std::fprintf(stderr, "FAIL the probe wrote %x to %x\n", value, reg);
    std::abort();
}
// A1's page: mapped for A1 alone, one register, with the writer held like a read.
static void ahbWrite(void *context, uint32_t reg, uint32_t value) {
    auto &port = *static_cast<Port *>(context);
    Soc &s = *port.soc;
    CHECK(port.block == PANEL_BLOCK_AHB && (panel_probe_windows[port.block].stages & s.stages));
    CHECK(panel_probe_windows[port.block].base + reg == 0xaf080b0);
    CHECK(s.quiet && s.now >= s.quietSince + PANEL_PROBE_QUIET_MS * MS);
    s.now += READ_COST;
    s.writes.emplace_back(0xaf080b0, value);
    Event e{Event::Write, s.now};
    e.address = 0xaf080b0;
    s.events.push_back(e);
    s.wrote(value);
}
static void noDelay(void *, unsigned) { std::abort(); }      // the probe waits on the counter (README: IODelay blocks on this port)

// The longest this format can print: %x 8, %u 10 (the phase line's counts at most PANEL_PROBE_PHASES, so 2), %s its own, %% 1.
static size_t widest(const char *format, va_list arguments) {
    size_t width = 0;
    unsigned number = 0;
    const bool phase = std::strstr(format, " phase n ") != nullptr;
    for (const char *c = format; *c; ++c) {
        if (*c != '%') {++width; continue;}
        switch (*++c) {
        case 'x': (void)va_arg(arguments, unsigned); width += 8; break;
        case 'u': (void)va_arg(arguments, unsigned); width += phase && number < 7 ? 2 : 10; ++number; break;
        case 'd': (void)va_arg(arguments, int); width += 11; break;
        case 's': width += std::strlen(va_arg(arguments, const char *)); break;
        case '%': ++width; break;
        default: std::abort();
        }
    }
    return width;
}
static void record(Soc &s, const char *format, va_list arguments) {
    char line[512];
    va_list copy;
    va_copy(copy, arguments);
    const size_t worst = widest(format, copy);
    va_end(copy);
    const int printed = std::vsnprintf(line, sizeof line, format, arguments);
    CHECK(printed > 0 && (size_t)printed <= worst);
    CHECK(worst < PANEL_PROBE_LINE);         // with its terminator it fits the kernel's buffer
    if (worst > s.longest) s.longest = worst;
    Event e{Event::Line, s.now};
    e.text = line;
    s.events.push_back(e);
}
static void hostLog(void *context, const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    record(*static_cast<Soc *>(context), format, arguments);
    va_end(arguments);
}
static void hostSleep(void *context, unsigned ms) {
    auto &s = *static_cast<Soc *>(context);
    Event e{Event::Sleep, s.now};
    e.ms = ms;
    s.events.push_back(e);
    s.now += ms * MS;
}
static uint64_t hostTicks(void *context) { return ++static_cast<Soc *>(context)->now; }
static unsigned hostDisplay(void *context) { auto &s = *static_cast<Soc *>(context); return s.display(s.now); }
static void hostQuiet(void *context, unsigned on) {
    auto &s = *static_cast<Soc *>(context);
    CHECK(on <= 1);
    if (on && !s.quiet) s.quietSince = s.now;
    s.quiet = on;
    Event e{Event::Quiet, s.now};
    e.ms = on;
    s.events.push_back(e);
}

// A block's first read comes after its line and the sleep right after that line.
static void crumbsFirst(const Soc &s) {
    std::set<unsigned> seen;
    for (size_t i = 0; i < s.events.size(); ++i) {
        const Event &e = s.events[i];
        if (e.kind != Event::Read || seen.count(e.block)) continue;
        seen.insert(e.block);
        const std::string name = std::string(" ") + panel_probe_windows[e.block].name + " next";
        bool found = false;
        for (size_t j = 0; j + 1 < i && !found; ++j) {
            const Event &crumb = s.events[j], &sleep = s.events[j + 1];
            if (crumb.kind == Event::Line && crumb.text.size() >= name.size() && !crumb.text.compare(crumb.text.size() - name.size(), name.size(), name)) {
                CHECK(sleep.kind == Event::Sleep && sleep.ms == PANEL_PROBE_CRUMB_MS);
                // The writer free when the line went out, so it takes that line; held after the sleep.
                CHECK(j && s.events[j - 1].kind == Event::Quiet && !s.events[j - 1].ms);
                CHECK(s.events[j + 2].kind == Event::Quiet && s.events[j + 2].ms);
                found = true;
            }
        }
        CHECK(found);
    }
}

// The UFS log's writer runs every 3 s: a line needs more than that to be sure of leaving.
static_assert(PANEL_PROBE_CRUMB_MS == 3500, "the sleep after a breadcrumb");

static void boot(Soc &s, unsigned stages = ALL) {
    Port ports[PANEL_PROBE_BLOCKS];
    panel_probe_host host{};
    host.context = &s;
    for (unsigned i = 0; i < PANEL_PROBE_BLOCKS; ++i) {
        ports[i] = {&s, i};
        host.io[i] = {&ports[i], portRead, i == PANEL_BLOCK_AHB ? ahbWrite : portWrite, noDelay};
    }
    host.log = hostLog;
    host.sleep_ms = hostSleep;
    host.ticks = hostTicks;
    host.display_on = hostDisplay;
    host.quiet = hostQuiet;
    s.stages = stages;
    s.start = s.now;
    panel_probe_run(&host, stages);
    CHECK(!s.quiet);
    crumbsFirst(s);
    for (auto &e : s.events) CHECK(e.kind != Event::Line || e.text.rfind("panel: ", 0) == 0);
}

static void healthy() {
    Soc s;
    boot(s);
    // Each stage at its moment.
    const uint64_t origin = s.start;
    const struct {const char *line; unsigned ms;} moments[] = {{"p0 gcc next", PANEL_PROBE_P0_MS}, {"p1 dispcc next", PANEL_PROBE_P1_MS},
                                                               {"p2 rsc next", PANEL_PROBE_P2_MS}, {"p3 intf next", PANEL_PROBE_P3_MS}};
    for (auto &m : moments) {
        const Event *e = s.find(m.line);
        CHECK(e && e->at >= origin + m.ms * MS && e->at < origin + (m.ms + 1000) * MS);
    }
    const Event *done = s.find("panel: p3 done");
    CHECK(done && (done->at + TICKS_S) < 420 * TICKS_S);     // well before the timed reset, and that from the kernel's start
    CHECK((done->at - origin) / TICKS_S < 360);
    CHECK(s.logged("panel: probe end"));
    // Past pagani-input-script's run (at 240 s of PaganiInput's clock, about 241 of uptime, 18 s of keys and at most 3 s to wake the screen).
    CHECK(s.find("p0 gcc next")->at - origin >= 261 * TICKS_S);
    // A stage that ran late: each stage's blocks done before the next one's moment, on the healthy path.
    CHECK(s.find("panel: p0 te rise")->at < origin + PANEL_PROBE_P1_MS * MS && s.find("panel: p1 clk_off")->at < origin + PANEL_PROBE_P2_MS * MS);
    CHECK(s.find("panel: p2 phy 20")->at < origin + PANEL_PROBE_P3_MS * MS);

    // P0: 1024 reads of each, then DSI in groups 20 us apart at the least, 100 ms long, TE far more often; never a free-running loop on DSI.
    std::vector<uint64_t> groups;
    for (auto &e : s.events) if (e.kind == Event::Read && e.address == 0xae94008) groups.push_back(e.at);
    CHECK(groups.size() > 4500 && groups.size() <= 5001);
    for (size_t i = 1; i < groups.size(); ++i) CHECK(groups[i] - groups[i - 1] >= PANEL_PROBE_GROUP_TICKS);
    CHECK(groups.back() - groups.front() >= PANEL_PROBE_WINDOW_TICKS - PANEL_PROBE_GROUP_TICKS * 2 && groups.back() - groups.front() <= PANEL_PROBE_WINDOW_TICKS);
    CHECK(s.count(0xae94004) == groups.size());
    CHECK(s.count(0xf156004) > 20 * groups.size());
    CHECK(s.count(0x127004) >= PANEL_PROBE_LATENCY_READS + 1 && s.count(0x127008) == 1);
    char want[120];
    std::snprintf(want, sizeof want, "panel: p0 lat gcc %u te ", (unsigned)(PANEL_PROBE_LATENCY_READS * READ_COST + 1));
    CHECK(s.logged(want));
    CHECK(s.logged("panel: p0 gcc ahb 1 axi 80000001"));
    unsigned n, nz, a, b, c, first, at;
    CHECK(std::sscanf(s.line("panel: p0 dsi n ").c_str(), "panel: p0 dsi n %u nz %u or %x/%x/%x first %x@%u", &n, &nz, &a, &b, &c, &first, &at) == 7);
    CHECK(n == groups.size() && nz == n && a == 0x20090000 && b == 0x1f5 && !c && first == 0x20090000 && at == 0);
    // TE at 120 Hz: 12 rising edges in 100 ms, 8333 us apart, high a tenth of the time.
    unsigned rises, shortest, longest, high;
    CHECK(std::sscanf(s.line("panel: p0 te rise").c_str(), "panel: p0 te rise %u per %u-%u us high %u%%", &rises, &shortest, &longest, &high) == 4);
    CHECK(rises >= 11 && rises <= 13 && shortest >= 8325 && longest <= 8341 && high >= 9 && high <= 11);

    // P1: the phase, 64 samples 250 us apart, its real length in the log.
    unsigned offAhb, offMdp, offByte, offPclk, offGdsc, nonzero, reading, us, gap;
    CHECK(std::sscanf(s.line("panel: p1 phase").c_str(), "panel: p1 phase n %u off ahb %u mdp %u byte %u pclk %u gdsc %u dsi nz %u lat %u us %u gap %u",
                      &n, &offAhb, &offMdp, &offByte, &offPclk, &offGdsc, &nonzero, &reading, &us, &gap) == 10);
    CHECK(n == 64 && !offAhb && !offMdp && !offByte && !offPclk && !offGdsc && nonzero == 64 && reading >= 64 * 6 * READ_COST && reading < 64 * 8 * READ_COST);
    CHECK(us >= 15750 && us <= 16010);       // 63 gaps of 250 us and the last sample
    CHECK(gap >= 250 && gap <= 252 && s.logged("panel: p1 clk_off mdp 0 byte 0 pclk 0"));
    std::vector<uint64_t> phases;
    for (auto &e : s.events) if (e.kind == Event::Read && e.address == 0xaf08010) phases.push_back(e.at);
    CHECK(phases.size() == 1 + 64 + 64 + 64);     // the line, P1's phase, P3's two phases
    for (size_t i = 2; i < 65; ++i) CHECK(phases[i] - phases[i - 1] >= PANEL_PROBE_PHASE_TICKS - READ_COST * 6 && phases[i] - phases[i - 1] <= PANEL_PROBE_PHASE_TICKS + READ_COST * 2);

    // P2's blocks in order, the PHY last; P3 reads the four registers and the two counts twice, 1 s apart.
    size_t order[5];
    const char *crumbs[] = {"p2 rsc next", "p2 wrap next", "p2 scc next", "p2 crmc next", "p2 phy next"};
    for (unsigned i = 0; i < 5; ++i) order[i] = (size_t)(s.find(crumbs[i]) - s.events.data());
    for (unsigned i = 1; i < 5; ++i) CHECK(order[i] > order[i - 1]);
    CHECK(s.logged("panel: p2 rsc pwr 1000") && s.logged("panel: p2 wrap 0 0 0") && s.logged("panel: p2 scc 0 0 0 0") && s.logged("panel: p2 crmc 0 0 0 0 0") && s.logged("panel: p2 phy 20 0 0 0"));
    CHECK(s.count(0xae36000) == 1 && s.count(0xae360ac) == 2 && s.count(0xae36298) == 2 && s.count(0xae362b4) == 1);
    std::vector<uint64_t> frames;
    for (auto &e : s.events) if (e.kind == Event::Read && e.address == 0xae360ac) frames.push_back(e.at);
    const uint64_t apart = (PANEL_PROBE_CRUMB_MS + PANEL_PROBE_QUIET_MS) * MS;
    CHECK(frames[1] - frames[0] >= apart && frames[1] - frames[0] < apart + 100 * MS);
    // P1's values and P3's first line reach the writer before the reads that follow them: it is let go after the line and held again before the first of those.
    const auto lineAt = [&s](const char *text) { return (size_t)(s.find(text) - s.events.data()); };
    const auto freed = [&s](size_t after, uint64_t until) {
        for (size_t i = after + 1; i < s.events.size() && s.events[i].at <= until; ++i)
            if (s.events[i].kind == Event::Quiet && !s.events[i].ms) return true;
        return false;
    };
    CHECK(freed(lineAt("panel: p1 link"), phases[1]) && freed(lineAt("panel: p3 intf te 1 fc"), frames[1]));
    unsigned te, f0, f1, t0, t1, ar;
    CHECK(std::sscanf(s.line("panel: p3 intf te 1 fc").c_str(), "panel: p3 intf te %x fc %x int %x", &te, &f0, &t0) == 3);
    const std::string last = s.events[(size_t)(s.find("panel: p3 done") - s.events.data()) - 1].text;
    CHECK(std::sscanf(last.c_str(), "panel: p3 intf te %x fc %x->%x int %x->%x ar %x", &te, &f0, &f1, &t0, &t1, &ar) == 6);
    CHECK(te == 1 && f1 - f0 >= 540 && f1 - f0 <= 543 && ar == 0x80000001);   // 4.5 s and the phase before the second read
    // Every address is read once at the least in this run, so the list above is the whole of it.
    for (uint64_t address : allowed) CHECK(s.count(address));
    printf("ok  healthy: stages at their moments, %zu DSI groups 20 us apart, every listed register read, longest line %zu bytes, end at %.1f s\n",
           groups.size(), s.longest, (double)(done->at - origin) / TICKS_S + 1.5);
    if (std::getenv("PANEL_PROBE_SHOW")) s.print();
}

// The observed state: DSI reads as zero, clocks on.
static void dsiZero() {
    Soc s;
    for (auto r : {0xae94000, 0xae94004, 0xae94008}) s.regs.erase(r);
    boot(s);
    CHECK(s.line("panel: p0 dsi n ").find(" nz 0 or 0/0/0 first 0@0") != std::string::npos);
    CHECK(s.line("panel: p1 phase").find(" dsi nz 0 ") != std::string::npos);
    CHECK(s.logged("panel: p3 done"));
    // Frame related: DSI answers one group in a hundred, 3 ms in.
    Soc f;
    f.live[0xae94000] = [&](uint64_t) { return f.reads % 100 < 3 ? 0x20090000u : 0u; };
    f.regs.erase(0xae94004);
    boot(f, PANEL_PROBE_P0);
    unsigned n, nz, a, b, c, first, at;
    CHECK(std::sscanf(f.line("panel: p0 dsi n ").c_str(), "panel: p0 dsi n %u nz %u or %x/%x/%x first %x@%u", &n, &nz, &a, &b, &c, &first, &at) == 7);
    CHECK(nz > 0 && nz < n && a == 0x20090000 && first == 0x20090000);
    printf("ok  DSI reading zero: logged as such, the probe goes on; a value that comes and goes shows with its first moment\n");
}

// GCC_DISP_AHB_CLK off: not a register of the clock controller, in any stage; and nothing behind it.
static void gccOff() {
    for (uint32_t v : {0u, 2u, 0x80000000u, 0x80000001u}) {
        Soc s;
        s.regs[0x127004] = v;
        boot(s);
        CHECK(!s.countBlock(PANEL_BLOCK_DISP) && !s.countBlock(PANEL_BLOCK_RSC) && !s.countBlock(PANEL_BLOCK_WRAP) && !s.countBlock(PANEL_BLOCK_SCC));
        CHECK(!s.countBlock(PANEL_BLOCK_CRMC) && !s.countBlock(PANEL_BLOCK_PHY) && !s.countBlock(PANEL_BLOCK_INTF));
        char want[64];
        std::snprintf(want, sizeof want, "panel: p1 skipped gcc %x", v);
        CHECK(s.logged(want) && s.logged("panel: p2 skipped no p1") && s.logged("panel: p3 skipped no full phase"));
    }
    // Hardware gated: CLK_OFF on an idle clock is no reason, on GCC as on the controller's branches.
    Soc idle;
    idle.regs[0x127004] = 0x80000003;
    idle.regs[0xaf080b0] = 0x80000003;
    idle.regs[0xaf08010] = 0x80000003;
    boot(idle);
    CHECK(idle.logged("panel: p3 done") && idle.logged("panel: p1 clk_off mdp 64 byte 0 pclk 0"));
    // It goes off during the sleep before the block: the gate is read after the sleep, so nothing is read of the clock controller.
    Soc late;
    late.live[0x127004] = [&](uint64_t t) { return t > late.start + (PANEL_PROBE_P1_MS + 100) * MS ? 0u : 1u; };
    boot(late);
    CHECK(!late.countBlock(PANEL_BLOCK_DISP) && late.logged("panel: p1 skipped gcc 0"));
    // It goes off while P1's lines leave the phone: the phase looks at the gate again, and reads nothing behind it.
    Soc flushing;
    flushing.live[0x127004] = [&](uint64_t) { return flushing.logged("panel: p1 link") ? 0u : 1u; };
    boot(flushing);
    CHECK(flushing.logged("panel: p1 phase skipped gcc 0") && !flushing.logged("panel: p1 phase n") && flushing.logged("panel: p3 skipped no full phase"));
    for (auto &e : flushing.events) CHECK(e.kind != Event::Read || e.block != PANEL_BLOCK_DISP || e.at <= flushing.find("panel: p1 link")->at);
    // It goes off after P1: P2 and P3 read their gates fresh, and those start at GCC.
    Soc after;
    after.live[0x127004] = [&](uint64_t t) { return t > after.start + (PANEL_PROBE_P2_MS - 1000) * MS ? 0u : 1u; };
    boot(after);
    for (auto &e : after.events) CHECK(e.kind != Event::Read || e.block != PANEL_BLOCK_DISP || e.at < after.start + PANEL_PROBE_P2_MS * MS);
    CHECK(after.logged("panel: p2 rsc skipped gcc 0 rscc 0") && after.logged("panel: p2 phy skipped gcc 0 ahb 0") && after.logged("panel: p3 skipped gcc 0 cfg 0 display 1"));
    CHECK(!after.countBlock(PANEL_BLOCK_RSC) && !after.countBlock(PANEL_BLOCK_PHY) && !after.countBlock(PANEL_BLOCK_INTF));
    printf("ok  GCC off: nothing behind it is read, in any stage; a hardware gated clock counts as on; the gate is read after the sleep\n");
}

// The clock controller reads as zero: CLK_OFF 0 looks like a clock running, the enable bit says otherwise.
static void dispccZero() {
    Soc s;
    for (auto it = s.regs.begin(); it != s.regs.end();) it = it->first >> 16 == 0xaf0 ? s.regs.erase(it) : std::next(it);
    boot(s);
    CHECK(s.countBlock(PANEL_BLOCK_DISP) && s.logged("panel: p1 gdsc 0/0 int2 0/0 misc 0"));
    CHECK(s.line("panel: p1 phase").find("off ahb 64 mdp 64 byte 64 pclk 64 gdsc 64") != std::string::npos);
    CHECK(!s.countBlock(PANEL_BLOCK_RSC) && !s.countBlock(PANEL_BLOCK_WRAP) && !s.countBlock(PANEL_BLOCK_SCC) && !s.countBlock(PANEL_BLOCK_CRMC));
    CHECK(!s.countBlock(PANEL_BLOCK_PHY) && !s.countBlock(PANEL_BLOCK_INTF));
    CHECK(s.logged("panel: p2 rsc skipped gcc 1 rscc 0") && s.logged("panel: p3 skipped no full phase"));
    printf("ok  a clock controller reading as zero opens nothing\n");
}

// Each gate alone, with the rest on: what it closes and what stays open.
static void gates() {
    {   // dispcc's AHB branch off: CRMC, PHY and INTF stay unread; RSCC, wrapper and SCC do not need it.
        Soc s;
        s.regs[0xaf080b0] = 0x80000000;
        boot(s);
        CHECK(s.countBlock(PANEL_BLOCK_RSC) && s.countBlock(PANEL_BLOCK_WRAP) && s.countBlock(PANEL_BLOCK_SCC));
        CHECK(!s.countBlock(PANEL_BLOCK_CRMC) && !s.countBlock(PANEL_BLOCK_PHY) && !s.countBlock(PANEL_BLOCK_INTF));
        CHECK(s.logged("panel: p2 crmc skipped gcc 1 ahb 80000000 rscc 1") && s.logged("panel: p2 phy skipped gcc 1 ahb 80000000"));
    }
    {   // Enabled but stopped, no hardware gating: not running, the same.
        Soc s;
        s.regs[0xaf080b0] = 0x80000001;
        boot(s);
        CHECK(!s.countBlock(PANEL_BLOCK_CRMC) && !s.countBlock(PANEL_BLOCK_PHY) && !s.countBlock(PANEL_BLOCK_INTF));
    }
    {   // The RSCC's branch off: RSCC, wrapper, SCC and CRMC unread; the PHY needs only AHB; P3 does not look at it.
        Soc s;
        s.regs[0xaf0c00c] = 0;
        boot(s);
        CHECK(!s.countBlock(PANEL_BLOCK_RSC) && !s.countBlock(PANEL_BLOCK_WRAP) && !s.countBlock(PANEL_BLOCK_SCC) && !s.countBlock(PANEL_BLOCK_CRMC));
        CHECK(s.countBlock(PANEL_BLOCK_PHY) && s.logged("panel: p3 done"));
    }
    {   // The same for the RSCC branch: it carries register reads, so CLK_OFF counts, unless hardware gated.
        Soc s;
        s.regs[0xaf0c00c] = 0x80000001;
        boot(s);
        CHECK(!s.countBlock(PANEL_BLOCK_RSC) && !s.countBlock(PANEL_BLOCK_CRMC) && s.logged("panel: p2 rsc skipped gcc 1 rscc 80000001"));
        Soc gated;
        gated.regs[0xaf0c00c] = 0x80000003;
        boot(gated);
        CHECK(gated.countBlock(PANEL_BLOCK_RSC) && gated.countBlock(PANEL_BLOCK_CRMC));
    }
    {   // The core GDSC off: wrapper, SCC and INTF unread.
        Soc s;
        s.regs[0xaf09004] = 0;
        boot(s);
        CHECK(s.countBlock(PANEL_BLOCK_RSC) && s.countBlock(PANEL_BLOCK_CRMC) && s.countBlock(PANEL_BLOCK_PHY));
        CHECK(!s.countBlock(PANEL_BLOCK_WRAP) && !s.countBlock(PANEL_BLOCK_SCC) && !s.countBlock(PANEL_BLOCK_INTF));
        // DSI sits behind it too: without P0, nothing of it is read at all, and the phase logs the samples as zero.
        Soc d;
        d.regs[0xaf09004] = 0;
        boot(d, ALL & ~PANEL_PROBE_P0);
        CHECK(!d.countBlock(PANEL_BLOCK_DSI) && d.line("panel: p1 phase").find(" dsi nz 0 ") != std::string::npos);
        // One sample with the GDSC seen off: that one sample leaves DSI alone.
        Soc one;
        unsigned seen = 0;
        one.live[0xaf09004] = [&](uint64_t) { return one.logged("panel: p1 link") && !one.logged("panel: p1 phase") && ++seen == 20 ? 0u : 0x10000u; };
        boot(one, PANEL_PROBE_P1);
        CHECK(one.count(0xae94000) == 63 && one.line("panel: p1 phase").find(" gdsc 1 dsi nz 63 ") != std::string::npos);
    }
    {   // The right bit: GDSCR bit 31 set says nothing about this GDSC, CFG_GDSCR bit 16 clear does.
        Soc s;
        s.regs[0xaf09000] = 0x80000000;
        s.regs[0xaf09004] = 0x00008000;
        boot(s);
        CHECK(!s.countBlock(PANEL_BLOCK_WRAP) && !s.countBlock(PANEL_BLOCK_SCC) && !s.countBlock(PANEL_BLOCK_INTF));
        Soc on;
        on.regs[0xaf09000] = 0;
        on.regs[0xaf09004] = 0x10000;
        boot(on);
        CHECK(on.countBlock(PANEL_BLOCK_WRAP) && on.countBlock(PANEL_BLOCK_INTF));
    }
    {   // The RSCC's branch goes off in the sleep before the wrapper: the reading after it counts, not P1's.
        Soc s;
        s.live[0xaf0c00c] = [&](uint64_t t) { return t > s.start + (PANEL_PROBE_P2_MS + PANEL_PROBE_CRUMB_MS + PANEL_PROBE_QUIET_MS + 100) * MS ? 0u : 1u; };
        boot(s);
        CHECK(s.countBlock(PANEL_BLOCK_RSC) && !s.countBlock(PANEL_BLOCK_WRAP) && !s.countBlock(PANEL_BLOCK_SCC) && !s.countBlock(PANEL_BLOCK_CRMC));
    }
    printf("ok  gates: AHB closes CRMC, PHY and INTF, RSCC closes its own four, the GDSC by CFG_GDSCR bit 16 closes wrapper, SCC and INTF\n");
}

// P3 needs 64 samples of 64, twice, the GDSC and the display.
static void third() {
    // Exactly one sample of P1's 64 with one branch off, for each branch.
    for (uint64_t branch : {0xaf080b0, 0xaf08010, 0xaf08034, 0xaf08004, 0xaf09004}) {
        Soc s;
        const uint32_t on = s.regs[branch];
        unsigned seen = 0;
        s.live[branch] = [&s, &seen, on, branch](uint64_t) {
            if (s.count(branch) && s.logged("panel: p1 link") && !s.logged("panel: p1 phase") && ++seen == 37) return 0u;
            return on;
        };
        boot(s, ALL & ~PANEL_PROBE_P0);
        CHECK(s.line("panel: p1 phase").find("off ahb 0 mdp 0 byte 0 pclk 0 gdsc 0") == std::string::npos);
        CHECK(!s.countBlock(PANEL_BLOCK_INTF) && s.logged("panel: p3 skipped no full phase"));
    }
    {   // Off when P3 samples again, right before its reads: P1's phase is not enough.
        Soc s;
        s.live[0xaf08010] = [&](uint64_t t) { return t > s.start + PANEL_PROBE_P3_MS * MS ? 0u : 1u; };
        boot(s);
        CHECK(s.line("panel: p1 phase").find("off ahb 0 mdp 0 byte 0 pclk 0 gdsc 0") != std::string::npos);
        CHECK(!s.countBlock(PANEL_BLOCK_INTF) && s.logged("panel: p3 skipped off ahb 0 mdp 64"));
    }
    {   // The frame's clocks stopped between frames by DISP_CC_MISC_CMD, with no CBCR bit 1: enabled, so on; the log counts it. AHB does not get that.
        Soc s;
        for (uint64_t branch : {0xaf08010, 0xaf08034, 0xaf08004}) s.live[branch] = [](uint64_t t) { return t % 96000 < 48000 ? 0x80000001u : 1u; };
        boot(s);
        unsigned mdp, byte, pclk;
        CHECK(std::sscanf(s.line("panel: p1 clk_off").c_str(), "panel: p1 clk_off mdp %u byte %u pclk %u", &mdp, &byte, &pclk) == 3);
        CHECK(mdp && byte && pclk && mdp < 64 && s.logged("panel: p3 done"));
        CHECK(s.line("panel: p1 phase").find("off ahb 0 mdp 0 byte 0 pclk 0 gdsc 0") != std::string::npos);
        Soc a;
        a.live[0xaf080b0] = [](uint64_t t) { return t % 96000 < 48000 ? 0x80000001u : 1u; };
        boot(a);
        CHECK(!a.countBlock(PANEL_BLOCK_INTF) && a.logged("panel: p3 skipped no full phase"));
    }
    {   // The display off, as the system sees it.
        Soc s;
        s.display = [](uint64_t) { return 0u; };
        boot(s);
        CHECK(!s.countBlock(PANEL_BLOCK_INTF) && s.logged("panel: p3 skipped gcc 1 cfg 10000 display 0") && !s.logged("p3 done"));
        CHECK(s.countBlock(PANEL_BLOCK_RSC));    // only P3 asks for it
    }
    {   // The core GDSC gone by P3, seen first by the gate itself; or the display off in the sleep before INTF: the gate is read after that sleep.
        Soc s;
        s.live[0xaf09004] = [&](uint64_t t) { return t > s.start + (PANEL_PROBE_P3_MS - 1000) * MS ? 0u : 0x10000u; };
        boot(s);
        CHECK(!s.countBlock(PANEL_BLOCK_INTF) && s.logged("panel: p3 skipped gcc 1 cfg 0 display 1"));
        Soc d;
        d.display = [&](uint64_t t) { return t > d.start + (PANEL_PROBE_P3_MS + 1000) * MS ? 0u : 1u; };
        boot(d);
        CHECK(!d.countBlock(PANEL_BLOCK_INTF) && d.logged("panel: p3 skipped gcc 1 cfg 10000 display 0"));
    }
    {   // The display goes off during the second between the reads, or the GDSC collapses: no second read.
        Soc s;
        s.display = [&](uint64_t t) { return t > s.start + (PANEL_PROBE_P3_MS + PANEL_PROBE_CRUMB_MS + PANEL_PROBE_QUIET_MS + 500) * MS ? 0u : 1u; };
        boot(s);
        CHECK(s.count(0xae360ac) == 1 && !s.count(0xae362b4) && !s.logged("p3 done"));
        Soc g;
        g.live[0xaf09004] = [&](uint64_t t) { return t > g.start + (PANEL_PROBE_P3_MS + PANEL_PROBE_CRUMB_MS + PANEL_PROBE_QUIET_MS + 500) * MS ? 0u : 0x10000u; };
        boot(g);
        CHECK(g.count(0xae360ac) == 1 && !g.count(0xae362b4) && !g.logged("p3 done"));
    }
    printf("ok  P3: one sample of 64 with one branch off ends it, P3's own phase too; the GDSC and the display are looked at right before each read\n");
}

// The thread loses the CPU in the middle of a phase. It takes this up after the read of `sample`'s MDP branch of P1's (or, with `p3`, P3's first) phase.
struct Lost {
    Soc s;
    unsigned seen = 0;
    std::vector<uint64_t> samples;
    Lost(unsigned sample, unsigned ms, bool p3) {
        s.live[0xaf08010] = [this, sample, ms, p3](uint64_t) {
            const bool inPhase = s.logged("panel: p1 link") && (p3 ? s.logged("panel: p3 intf next") : !s.logged("panel: p1 phase"));
            if (inPhase) {
                samples.push_back(s.now);
                if (++seen == sample) s.now += ms * MS;
            }
            return 1u;
        };
        boot(s, ALL & ~PANEL_PROBE_P0);
    }
    bool apart(size_t count) const {
        for (size_t i = 1; i < count; ++i) if (samples[i] - samples[i - 1] < PANEL_PROBE_PHASE_TICKS - READ_COST * 6) return false;
        return true;
    }
};
static void preempted() {
    unsigned n, ahb, mdp, byte, pclk, gdsc, nonzero, reading, us, gap;
    const char *format = "panel: p1 phase n %u off ahb %u mdp %u byte %u pclk %u gdsc %u dsi nz %u lat %u us %u gap %u";
    // Less than a frame: the samples stay 250 us apart after it, the log has what happened, and P3 goes on; the review's two cases of 8 and 7 ms.
    const struct {unsigned sample, ms;} shorter[] = {{2, 8}, {30, 7}};
    for (auto &c : shorter) {
        Lost l{c.sample, c.ms, false};
        CHECK(l.samples.size() == 64 && l.apart(64));
        CHECK(std::sscanf(l.s.line("panel: p1 phase").c_str(), format, &n, &ahb, &mdp, &byte, &pclk, &gdsc, &nonzero, &reading, &us, &gap) == 10);
        CHECK(us >= 15500 + c.ms * 1000 && us <= 15760 + c.ms * 1000 && gap >= c.ms * 1000 && gap <= c.ms * 1000 + 2);
        CHECK(l.s.logged("panel: p3 done"));
    }
    // More than a frame, P1's phase: not seen whole, whatever the bits read; P3 reads nothing of INTF.
    Lost a{2, 14, false};
    CHECK(a.apart(64) && std::sscanf(a.s.line("panel: p1 phase").c_str(), format, &n, &ahb, &mdp, &byte, &pclk, &gdsc, &nonzero, &reading, &us, &gap) == 10);
    CHECK(!ahb && !mdp && !byte && !pclk && !gdsc && us >= 29500 && gap >= 14000 && gap <= 14002);
    CHECK(!a.s.countBlock(PANEL_BLOCK_INTF) && a.s.logged("panel: p3 skipped no full phase"));
    // Right before INTF, P3's own phase: the same, and the line says why.
    Lost b{20, 14, true};
    CHECK(b.apart(64) && !b.s.countBlock(PANEL_BLOCK_INTF) && b.s.logged("panel: p3 skipped off ahb 0 mdp 0 byte 0 pclk 0 gdsc 0 gap 1400"));
    // The limit itself: a frame passes, a frame and a sample's worth does not.
    panel_probe_phase edge = {};
    edge.gap_us = PANEL_PROBE_FRAME_US;
    CHECK(panel_probe_phase_on(&edge));
    edge.gap_us = PANEL_PROBE_FRAME_US + 1;
    CHECK(!panel_probe_phase_on(&edge));
    printf("ok  a thread that loses the CPU: samples stay 250 us apart, the length and the longest gap in the log, past a frame P3 reads nothing\n");
}

// The sets of stages the boot-arg bits make: nothing is read beyond the windows of those stages (portRead checks), and bit 10 alone proves no clock.
static void stages() {
    Soc low;
    boot(low, PANEL_PROBE_P0 | PANEL_PROBE_P1);
    CHECK(low.countBlock(PANEL_BLOCK_DISP) && !low.countBlock(PANEL_BLOCK_RSC) && !low.countBlock(PANEL_BLOCK_PHY) && !low.countBlock(PANEL_BLOCK_INTF));
    CHECK(!low.logged(" p2 ") && !low.logged(" p3 ") && low.logged("panel: probe end"));
    Soc deep;
    boot(deep, PANEL_PROBE_P2 | PANEL_PROBE_P3);
    CHECK(!deep.reads && deep.logged("panel: p2 skipped no p1") && deep.logged("panel: p3 skipped no full phase"));
    printf("ok  stages: bit 7 stops after P1, bit 10 alone reads nothing\n");
}

// A0, against its own model of GCC: the one write of all this.
struct Clock {
    uint32_t cbcr;
    explicit Clock(uint32_t v) : cbcr(v) {}
    unsigned offPolls = 0;                 // CLK_OFF clears after this many reads
    unsigned reads = 0, delays = 0, dsiReads = 0, dsiReadAtWrite = 0;
    std::vector<std::pair<uint32_t, uint32_t>> writes;
    std::vector<std::string> lines;
    uint32_t version = 0;
    static uint32_t read(void *context, uint32_t reg) {
        auto &c = *static_cast<Clock *>(context);
        CHECK(reg == PANEL_GCC_AHB);
        ++c.reads;
        if (c.writes.size() && c.offPolls && !--c.offPolls) c.cbcr &= ~0x80000000u;
        return c.cbcr;
    }
    static void write(void *context, uint32_t reg, uint32_t value) {
        auto &c = *static_cast<Clock *>(context);
        c.writes.emplace_back(reg, value);
        c.dsiReadAtWrite = c.dsiReads;
        c.cbcr = value;
    }
    static void delay(void *context, unsigned us) { CHECK(us == 1); ++static_cast<Clock *>(context)->delays; }
    static uint32_t readDsi(void *context, uint32_t reg) {
        auto &c = *static_cast<Clock *>(context);
        CHECK(!reg);
        ++c.dsiReads;
        return c.version;
    }
    static void log(void *context, const char *format, ...) {
        char line[256];
        va_list arguments;
        va_start(arguments, format);
        const int printed = std::vsnprintf(line, sizeof line, format, arguments);
        va_end(arguments);
        CHECK(printed > 0 && printed < PANEL_PROBE_LINE);
        static_cast<Clock *>(context)->lines.push_back(line);
    }
    void run() {
        const peripheral_io gcc = {this, read, write, delay}, dsi = {this, readDsi, portWrite, noDelay};
        panel_probe_a0(&gcc, &dsi, log, this);
    }
};
static void a0() {
    // Clear, CLK_OFF set: the bit, once, read back, CLK_OFF waited for, then DSI: the write comes before DSI's first read.
    Clock c{0x80000000};
    c.offPolls = 5;
    c.version = 0x20090000;
    c.run();
    CHECK(c.writes == (std::vector<std::pair<uint32_t, uint32_t>>{{PANEL_GCC_AHB, 0x80000001}}));
    CHECK(!c.dsiReadAtWrite && c.dsiReads == 1 && c.delays == 4 && c.lines.back() == "panel: a0 gcc 80000000->1 ver 20090000");
    CHECK(c.lines.front() == "panel: a0 write next gcc 80000000");
    // The other bits as they were: only bit 0 changes.
    Clock other{0x00004ff0};
    other.run();
    CHECK(other.writes == (std::vector<std::pair<uint32_t, uint32_t>>{{PANEL_GCC_AHB, 0x00004ff1}}) && !other.delays);
    // Already on: nothing written, nothing of DSI, one line. With CLK_OFF too.
    for (uint32_t v : {1u, 0x80000003u, 0xffffffffu}) {
        Clock on{v};
        on.run();
        char want[64];
        std::snprintf(want, sizeof want, "panel: a0 gcc %x already on", v);
        CHECK(on.writes.empty() && !on.dsiReads && on.lines == std::vector<std::string>{want});
    }
    // CLK_OFF stays: the poll ends, DSI's answer still reaches the log.
    Clock stuck{0x80000000};
    stuck.run();
    CHECK(stuck.writes.size() == 1 && stuck.delays == PANEL_PROBE_A0_POLLS && stuck.dsiReads == 1 && stuck.lines.back() == "panel: a0 gcc 80000000->80000001 ver 0");
    printf("ok  A0: one write, bit 0 alone, only when clear, before DSI's first read; the poll ends at %u\n", (unsigned)PANEL_PROBE_A0_POLLS);
}

// The 13T as the first run read it (merged.log of 20261101-110201-stage15-smoke-1), its AHB branch off. What the model adds is the claim under test: DSI, the PHY, INTF_1 and
// MDSS read as zero while that branch does not run, and as a working display once it does.
struct Device {
    Soc s;
    unsigned polls = 2;                      // reads of the CBCR until CLK_OFF clears
    bool ignores = false, sticks = false;    // the write does not take; it takes and the clock does not start
    uint32_t ahb = 0x88000000;
    bool runs() const { return (ahb & 1) && !(ahb >> 31); }
    Device() {
        s.regs.clear();
        const std::pair<uint64_t, uint32_t> read[] = {
            {0x127004, 0x88000003}, {0x127008, 0x8200001}, {0xaf09000, 0xfa22f800}, {0xaf09004, 0x4070000}, {0xaf0b000, 0x222f801}, {0xaf0b004, 0x4088000},
            {0xaf08364, 0x100001}, {0xaf0c004, 0x8000000}, {0xaf0c00c, 0x8000001}, {0xaf0c008, 0x8000001}, {0xaf08010, 0x8000001}, {0xaf08154, 0x100105},
            {0xaf08020, 0x8000000}, {0xaf08030, 0x8000001}, {0xaf00000, 0xe0000005}, {0xaf01000, 0x100}, {0xaf08034, 0x8000001}, {0xaf08038, 0x8000001},
            {0xaf08004, 0x8000001}, {0xaf08044, 0x8000001}, {0xaf31000, 0xf0}};
        for (auto &r : read) s.regs[r.first] = r.second;
        s.live[0xaf080b0] = [this](uint64_t) {
            if ((ahb & 1) && !sticks && polls && !--polls) ahb &= ~0x80000000u;
            return ahb;
        };
        s.wrote = [this](uint32_t value) { if (!ignores) ahb = value; };
        // TE as the run measured it: 8299 us, high 3% of the time.
        s.live[0xf156004] = [](uint64_t t) { return t % 159341 < 4780 ? 1u : 0u; };
        const std::pair<uint64_t, uint32_t> behind[] = {
            {0xae94000, 0x20090000}, {0xae94004, 0x1f5}, {0xae94084, 0x4}, {0xae9411c, 0x23f}, {0xae95000, 0x20}, {0xae36000, 1}, {0xae362b4, 0x80000001}, {0xae00000, 0xc0000000}};
        for (auto &b : behind) {
            const uint32_t value = b.second;
            s.live[b.first] = [this, value](uint64_t) { return runs() ? value : 0u; };
        }
        s.live[0xae360ac] = [this](uint64_t t) { return runs() ? (uint32_t)(t / 159341) : 0u; };
    }
};
static void inOrder(const Soc &s, std::initializer_list<const char *> lines) {
    size_t at = 0;
    for (const char *text : lines) {
        while (at < s.events.size() && (s.events[at].kind != Event::Line || s.events[at].text.find(text) == std::string::npos)) ++at;
        if (at == s.events.size()) std::fprintf(stderr, "no line %s in its place\n", text);
        CHECK(at < s.events.size());
        ++at;
    }
}
// The moment of this kind of event, the first after `after`.
static uint64_t firstAfter(const Soc &s, uint64_t after, Event::Kind kind, unsigned block = PANEL_PROBE_BLOCKS) {
    for (auto &e : s.events) if (e.kind == kind && e.at > after && (block == PANEL_PROBE_BLOCKS || e.block == block)) return e.at;
    return UINT64_MAX;
}
static const unsigned behindAhb[] = {PANEL_BLOCK_PHY, PANEL_BLOCK_INTF, PANEL_BLOCK_MDSS, PANEL_BLOCK_CRMC};

static void a1Healthy() {
    Device d;
    Soc &s = d.s;
    boot(s, PANEL_PROBE_A1);
    // One write, bit 0 alone, in the page that is writable; the rest of the CBCR as it read.
    CHECK(s.writes == (std::vector<std::pair<uint64_t, uint32_t>>{{0xaf080b0, 0x88000001}}));
    // INTF_1 before everything of P2, MDSS after it all: what a hang costs is the least it can be.
    inOrder(s, {"panel: probe stages 10", "panel: a1 te next", "panel: a1 te rise", "panel: a1 gcc next", "panel: a1 dsi next", "panel: a1 dispcc next",
                "panel: a1 state gcc 88000003 ahb 88000000 ahb1 0 rcg 0/100001 gdsc fa22f800/4070000 mdp 8000001", "panel: a1 write next ahb 88000000",
                "panel: a1 pre off ahb 64 mdp 0 byte 0 pclk 0 gdsc 0", "panel: a1 ahb 88000000->8000001 us ", "panel: a1 dsi next",
                "panel: a1 dsi ver 20090000 ctrl 1f5 st 0 fifo 0 dma 0 trig 4", "panel: a1 lanes 0 int 0 clk 23f", "panel: a1 dsi back",
                "panel: a1 phase n 64 off ahb 0 mdp 0 byte 0 pclk 0 gdsc 0 dsi nz 64 ", "panel: p3 intf next", "panel: p3 intf te 1 fc", "panel: p3 done",
                "panel: p2 rsc next", "panel: p2 rsc pwr 0", "panel: p2 wrap 0 0 0", "panel: p2 scc f0 0 0 0", "panel: p2 crmc 0 0 0 0 0", "panel: p2 phy 20 0 0 0",
                "panel: a1 mdss next", "panel: a1 mdss ver c0000000", "panel: probe end"});
    // At its moment, and done well before a timed reset of 240 s of uptime; the write early enough that a hang still leaves the watchdog's 30 s before that reset.
    const Event *first = s.find("panel: a1 te next"), *end = s.find("panel: probe end");
    CHECK(first->at >= s.start + PANEL_PROBE_A1_MS * MS && first->at < s.start + (PANEL_PROBE_A1_MS + 1000) * MS);
    CHECK(end->at + 30 * TICKS_S < 240 * TICKS_S);
    uint64_t written = 0;
    for (auto &e : s.events) if (e.kind == Event::Write) written = e.at;
    CHECK(written + 60 * TICKS_S < 240 * TICKS_S);
    // `a1 write next` left the phone before the write: the writer was free for a whole crumb after it.
    const Event *next = s.find("panel: a1 write next");
    bool freed = false;
    for (auto &e : s.events) if (e.kind == Event::Quiet && !e.ms && e.at >= next->at && e.at < written) freed = true;
    CHECK(freed && written - next->at >= (PANEL_PROBE_CRUMB_MS + PANEL_PROBE_QUIET_MS) * MS);
    // The first read of DSI behind the running clock comes after a line of its own and that line's sleep; the write's result is on the disk by then.
    uint64_t crumb = 0;
    for (auto &e : s.events) if (e.kind == Event::Line && e.text == "panel: a1 dsi next" && e.at > written) crumb = e.at;
    CHECK(crumb && firstAfter(s, written, Event::Read, PANEL_BLOCK_DSI) >= crumb + (PANEL_PROBE_CRUMB_MS + PANEL_PROBE_QUIET_MS) * MS);
    // DSI's answer leaves the phone before the phase with the clock on: that is the line a hang after it must not take along.
    const Event *answer = s.find("panel: a1 dsi back"), *phase = s.find("panel: a1 phase n");
    freed = false;
    for (auto &e : s.events) if (e.kind == Event::Quiet && !e.ms && e.at >= answer->at && e.at < phase->at) freed = true;
    CHECK(freed && phase->at - answer->at >= (PANEL_PROBE_CRUMB_MS + PANEL_PROBE_QUIET_MS) * MS);
    // What nothing else reads: the gates' registers before the write, the DSI 14, MDSS's first register once. INTF_1's counts twice.
    for (uint64_t address : allowedA1) CHECK(s.count(address));
    CHECK(s.count(0xae00000) == 1 && s.count(0xae360ac) == 2);
    CHECK(!s.countBlock(PANEL_BLOCK_AHB));
    // The period over 24 periods, to a tenth of a microsecond; the edges alone give it to 12.
    unsigned rises, shortest, longest, high, mean;
    CHECK(std::sscanf(s.line("panel: a1 te rise").c_str(), "panel: a1 te rise %u per %u-%u us high %u%% mean %u ns", &rises, &shortest, &longest, &high, &mean) == 5);
    CHECK(rises >= 24 && rises <= 25 && shortest >= 8298 && longest <= 8300 && high >= 2 && high <= 4 && mean >= 8298900 && mean <= 8299200);
    printf("ok  A1: one write of one bit after its line left the phone, DSI back behind a line of its own, INTF_1, P2 and last MDSS, end at %.1f s, longest line %zu bytes\n",
           (double)end->at / TICKS_S, s.longest);
    if (std::getenv("PANEL_PROBE_SHOW")) s.print();
}

// Each gate alone: seen before the sleep, or only after it (the word changes when `a1 write next` went out). Nothing written, nothing behind the clock read, the line says why.
static void a1Gates() {
    const struct {uint64_t reg; uint32_t bad; const char *why;} cases[] = {
        {0x127004, 0x88000000, "gcc"},              // GCC_DISP_AHB_CLK's enable bit clear
        {0x127004, 0x80000001, "gcc"},              // enabled, stopped, not hardware gated
        {0xaf080b0, 0x88000002, "hw gated"},        // AHB's own hardware gating
        {0xaf08360, 0x80000000, "root off"},        // ROOT_OFF
        {0xaf08364, 0x100401, "not xo"},            // PLL1, which is off here
        {0xaf08364, 0x100601, "not xo"},
        {0xaf09004, 0x4068000, "gdsc"},             // CFG_GDSCR bit 16 clear, GDSCR bit 31 still set
    };
    for (auto &c : cases) for (unsigned late = 0; late < 2; ++late) {
        Device d;
        Soc &s = d.s;
        auto base = s.live.count(c.reg) ? s.live[c.reg] : nullptr;
        const uint32_t good = s.regs[c.reg];
        const uint32_t bad = c.bad;
        if (late) s.live[c.reg] = [&s, base, good, bad](uint64_t t) { return s.logged("panel: a1 write next") ? bad : base ? base(t) : good; };
        else if (c.reg == 0xaf080b0) d.ahb = bad;
        else s.regs[c.reg] = bad;
        boot(s, PANEL_PROBE_A1);
        char want[64];
        std::snprintf(want, sizeof want, "panel: a1 held %s ", c.why);
        CHECK(s.writes.empty() && s.logged(want) && s.logged("panel: probe end"));
        CHECK(s.logged("panel: a1 write next") == !!late);
        for (unsigned block : behindAhb) CHECK(!s.countBlock(block));
        CHECK(!s.logged("panel: p3") && !s.logged("panel: p2") && !s.logged("panel: a1 dsi back") && !s.logged("panel: a1 dsi skipped"));
        if (c.reg == 0x127004) CHECK(!late ? !s.countBlock(PANEL_BLOCK_DISP) || s.count(0xaf080b0) == 0 : true);
    }
    {   // A clock controller that reads as zero: it shows no gate closed, and opens nothing.
        Device d;
        Soc &s = d.s;
        for (auto it = s.regs.begin(); it != s.regs.end();) it = it->first >> 16 == 0xaf0 ? s.regs.erase(it) : std::next(it);
        d.ahb = 0;
        boot(s, PANEL_PROBE_A1);
        CHECK(s.writes.empty() && s.logged("panel: a1 held zero gcc 88000003 ahb 0 rcg 0/0 cfg 0 display 1"));
    }
    {   // The display off, as the system sees it: before the sleep and in it.
        for (unsigned late = 0; late < 2; ++late) {
            Device d;
            Soc &s = d.s;
            s.display = [&s, late](uint64_t) { return late && !s.logged("panel: a1 write next") ? 1u : 0u; };
            boot(s, PANEL_PROBE_A1);
            CHECK(s.writes.empty() && s.logged("panel: a1 held display") && s.line("panel: a1 held display").find(" display 0") != std::string::npos);
        }
    }
    {   // The frame's clocks or the GDSC off in one sample of 64, right before the write.
        for (uint64_t branch : {0xaf08010, 0xaf08034, 0xaf08004, 0xaf09004}) {
            Device d;
            Soc &s = d.s;
            const uint32_t on = s.regs[branch];
            unsigned seen = 0;
            s.live[branch] = [&s, &seen, on](uint64_t) { return s.logged("panel: a1 write next") && !s.logged("panel: a1 pre") && ++seen == 37 ? 0u : on; };
            boot(s, PANEL_PROBE_A1);
            CHECK(s.writes.empty() && s.logged("panel: a1 held phase"));
            for (unsigned block : behindAhb) CHECK(!s.countBlock(block));
        }
    }
    printf("ok  A1's gates: GCC, a controller reading zero, hardware gating, ROOT_OFF, a source not XO, the GDSC, the display, the phase; before the sleep and after it\n");
}

// The thread loses the CPU in the phase before the write: `count` phases of A1's three, 14 ms each time.
static void a1Preempted() {
    for (unsigned lost = 1; lost <= 3; ++lost) {
        Device d;
        Soc &s = d.s;
        unsigned seen = 0;
        s.live[0xaf08010] = [&s, &seen, lost](uint64_t) {
            // The state's read of this branch, then the phases' samples: the second sample of a phase takes the 14 ms.
            if (s.logged("panel: a1 write next") && !s.logged("panel: a1 pre") && ++seen % 64 == 3 && seen / 64 < lost) s.now += 14 * MS;
            return 0x8000001u;
        };
        boot(s, PANEL_PROBE_A1);
        const bool whole = lost < 3;
        CHECK(seen == 1 + 64 * (lost + (whole ? 1 : 0)));
        CHECK(s.writes.size() == (whole ? 1u : 0u) && s.logged("panel: a1 held phase") == !whole);
    }
    printf("ok  A1 and a thread that loses the CPU: another look, three in all, then no write\n");
}

static void a1Outcomes() {
    {   // On already: nothing to write, the reads go on.
        Device d;
        d.ahb = 0x8000001;
        boot(d.s, PANEL_PROBE_A1);
        CHECK(d.s.writes.empty() && d.s.logged("panel: a1 ahb 8000001 already on") && d.s.logged("panel: a1 dsi back") && d.s.logged("panel: a1 mdss ver"));
    }
    {   // The write does not take: the line says so, and nothing behind the clock is read.
        Device d;
        d.ignores = true;
        boot(d.s, PANEL_PROBE_A1);
        CHECK(d.s.writes.size() == 1 && d.s.logged("panel: a1 ahb 88000000->88000000 us 1000"));
        CHECK(d.s.logged("panel: a1 failed ignored ahb 88000000 rcg 0/100001 gdsc fa22f800/4070000"));
        for (unsigned block : behindAhb) CHECK(!d.s.countBlock(block));
        uint64_t written = 0;
        for (auto &e : d.s.events) if (e.kind == Event::Write) written = e.at;
        CHECK(firstAfter(d.s, written, Event::Read, PANEL_BLOCK_DSI) == UINT64_MAX && !d.s.logged("panel: a1 dsi skipped"));
    }
    {   // Not taken with CLK_OFF clear (some vote of another keeps the branch running): the enable bit is what was asked for, and it did not come.
        Device d;
        d.ignores = true;
        d.ahb = 0x8000000;
        boot(d.s, PANEL_PROBE_A1);
        CHECK(d.s.logged("panel: a1 failed ignored ahb 8000000"));
        for (unsigned block : behindAhb) CHECK(!d.s.countBlock(block));
    }
    {   // It takes and the clock does not start: the poll ends at 1 ms, the same.
        Device d;
        d.sticks = true;
        boot(d.s, PANEL_PROBE_A1);
        CHECK(d.s.logged("panel: a1 ahb 88000000->88000001 us 1000") && d.s.logged("panel: a1 failed stuck ahb 88000001"));
        for (unsigned block : behindAhb) CHECK(!d.s.countBlock(block));
    }
    {   // The clock runs and DSI stays zero: the root cause is elsewhere. The PHY and INTF_1 still tell which way; MDSS is not worth a read.
        Device d;
        for (uint64_t r : {0xae94000, 0xae94004, 0xae94084, 0xae9411c}) d.s.live.erase(r);
        boot(d.s, PANEL_PROBE_A1);
        CHECK(d.s.logged("panel: a1 dsi ver 0 ctrl 0") && d.s.logged("panel: a1 dsi zero") && d.s.logged(" dsi nz 0 "));
        CHECK(d.s.logged("panel: p3 done") && d.s.logged("panel: p2 phy 20 0 0 0"));
        CHECK(d.s.logged("panel: a1 mdss skipped dsi 0 intf 1") && !d.s.countBlock(PANEL_BLOCK_MDSS));
    }
    {   // Something reads, not in the shape of a working controller.
        Device d;
        d.s.live[0xae94004] = [](uint64_t) { return 0u; };
        boot(d.s, PANEL_PROBE_A1);
        CHECK(d.s.logged("panel: a1 dsi other") && d.s.logged("panel: a1 mdss skipped dsi 0 intf 1"));
    }
    {   // DSI back, INTF_1 zero: MDSS stays unread.
        Device d;
        for (uint64_t r : {0xae36000, 0xae360ac, 0xae362b4}) {d.s.live.erase(r); d.s.regs.erase(r);}
        boot(d.s, PANEL_PROBE_A1);
        CHECK(d.s.logged("panel: a1 dsi back") && d.s.logged("panel: a1 mdss skipped dsi 1 intf 0") && !d.s.countBlock(PANEL_BLOCK_MDSS));
    }
    {   // The branch goes off again by itself in one sample after DSI: no full phase, so INTF_1, which needs it in every sample, stays unread.
        Device d;
        const auto base = d.s.live[0xaf080b0];
        unsigned seen = 0;
        d.s.live[0xaf080b0] = [&d, base, &seen](uint64_t t) {
            const uint32_t v = base(t);
            return d.s.logged("panel: a1 dsi back") && !d.s.logged("panel: a1 phase n") && ++seen == 37 ? 0x88000000u : v;
        };
        boot(d.s, PANEL_PROBE_A1);
        CHECK(d.s.line("panel: a1 phase n").find(" off ahb 1 ") != std::string::npos && d.s.logged("panel: p3 skipped no full phase"));
        CHECK(!d.s.countBlock(PANEL_BLOCK_INTF) && d.s.logged("panel: a1 mdss skipped dsi 1 intf 0"));
    }
    {   // Off again before DSI's first read: the sleep's gate stops it, nothing of DSI behind a dead clock.
        Device d;
        d.s.live[0xaf09004] = [&d](uint64_t) { return d.s.logged("panel: a1 ahb 88000000->") ? 0u : 0x4070000u; };
        boot(d.s, PANEL_PROBE_A1);
        uint64_t written = 0;
        for (auto &e : d.s.events) if (e.kind == Event::Write) written = e.at;
        CHECK(d.s.logged("panel: a1 dsi skipped gcc 88000003 ahb 8000001 cfg 0") && firstAfter(d.s, written, Event::Read, PANEL_BLOCK_DSI) == UINT64_MAX);
    }
    {   // The branch off again before DSI's first read, by itself, the GDSC still on: the same gate, the other half of it.
        Device d;
        const auto base = d.s.live[0xaf080b0];
        d.s.live[0xaf080b0] = [&d, base](uint64_t t) { const uint32_t v = base(t); return d.s.logged("panel: a1 ahb 88000000->") ? 0x88000000u : v; };
        d.s.live[0xae94000] = [](uint64_t) { return 0x20090000u; };   // what a read would have seen
        boot(d.s, PANEL_PROBE_A1);
        uint64_t written = 0;
        for (auto &e : d.s.events) if (e.kind == Event::Write) written = e.at;
        CHECK(d.s.logged("panel: a1 dsi skipped gcc 88000003 ahb 88000000 cfg 4070000") && firstAfter(d.s, written, Event::Read, PANEL_BLOCK_DSI) == UINT64_MAX);
    }
    {   // GCC gone before the second phase: not a read of the controller.
        Device d;
        d.s.live[0x127004] = [&d](uint64_t) { return d.s.logged("panel: a1 dsi back") ? 0u : 0x88000003u; };
        boot(d.s, PANEL_PROBE_A1);
        CHECK(d.s.logged("panel: a1 phase skipped gcc 0") && !d.s.logged("panel: a1 phase n"));
    }
    {   // MDSS's own moment: the display off in its sleep, after everything else went well.
        Device d;
        d.s.display = [&d](uint64_t) { return d.s.logged("panel: a1 mdss next") ? 0u : 1u; };
        boot(d.s, PANEL_PROBE_A1);
        CHECK(d.s.logged("panel: p3 skipped gcc 88000003 cfg 4070000 display 0") && !d.s.countBlock(PANEL_BLOCK_MDSS));
    }
    printf("ok  A1's outcomes: on already, a write that does not take, a clock that does not start, DSI still zero, DSI otherwise, INTF_1 zero, the clock gone again\n");
}

// A pulse the thread sleeps through is not a period of two, in A1's look and in P0's; and the windows the kernel maps for the stages, the moment a thread with no windows speaks.
static void a1Parts() {
    Device d;
    unsigned reads = 0;
    d.s.live[0xf156004] = [&d, &reads](uint64_t t) {
        if (++reads == 200000) d.s.now += 9 * MS;
        return t % 159341 < 4780 ? 1u : 0u;
    };
    boot(d.s, PANEL_PROBE_A1);
    unsigned rises, shortest, longest, high, mean;
    CHECK(std::sscanf(d.s.line("panel: a1 te rise").c_str(), "panel: a1 te rise %u per %u-%u us high %u%% mean %u ns", &rises, &shortest, &longest, &high, &mean) == 5);
    CHECK(longest > 16000 && longest < 16700 && shortest < 8301 && mean >= 8298900 && mean <= 8299200);
    panel_probe_samples none = {};
    CHECK(!panel_probe_te_ns(&none));
    none.rises = 1;
    CHECK(!panel_probe_te_ns(&none));
    panel_probe_samples two = {};
    two.rises = 2; two.shortest = 8299; two.span = 159341;
    CHECK(panel_probe_te_ns(&two) == 8299010);             // 159341 ticks of 19.2 MHz
    // One edge seen 3% early over 24 periods: the shortest says so, the count stays 24. A count rounded to nearest would be 25.
    two.rises = 25; two.shortest = 8050; two.span = 24 * 159341;
    CHECK(panel_probe_te_ns(&two) == 8299010);

    CHECK(panel_probe_windows[PANEL_BLOCK_AHB].base + PANEL_AHB_CBCR == 0xaf080b0 && panel_probe_windows[PANEL_BLOCK_AHB].stages == (PANEL_PROBE_A1 | PANEL_PROBE_AHB));
    CHECK(panel_probe_windows[PANEL_BLOCK_AHB].size == 0x1000);
    CHECK(panel_probe_windows[PANEL_BLOCK_MDSS].base == 0xae00000 && panel_probe_windows[PANEL_BLOCK_MDSS].stages == PANEL_PROBE_A1);
    CHECK(panel_probe_first_ms(PANEL_PROBE_A1) == PANEL_PROBE_A1_MS && panel_probe_first_ms(ALL | PANEL_PROBE_A1) == PANEL_PROBE_A1_MS && panel_probe_first_ms(ALL) == PANEL_PROBE_P0_MS);
    CHECK(PANEL_PROBE_A1_MS * MS < 200 * TICKS_S);
    printf("ok  a pulse slept through leaves the period alone; A1's windows, and the moment a thread with no windows says so\n");
}

// With the stages after it, on a boot long enough for them: each still has its line and a held writer (portRead checks), and sees the clock on.
static void a1ThenStages() {
    Device d;
    Soc &s = d.s;
    boot(s, ALL | PANEL_PROBE_A1);
    CHECK(s.writes.size() == 1);
    inOrder(s, {"panel: a1 mdss ver", "panel: p0 gcc next", "panel: p0 lat", "panel: p1 dispcc next", "panel: p1 ahb 8000001", "panel: p2 rsc next", "panel: p3 intf next", "panel: p3 done", "panel: probe end"});
    CHECK(s.line("panel: p0 dsi n ").find(" nz 0 ") == std::string::npos);
    CHECK(s.find("panel: p0 gcc next")->at >= s.start + PANEL_PROBE_P0_MS * MS);
    printf("ok  A1 and the stages after it: each with its own lines, the writer held, the clock still on\n");
}

// Bit 8's command, on the model: a write in the DSI window, the one kind this file allows only here, under the same hold as a read. The registers work as the send path expects.
static void dsiWrite(void *context, uint32_t reg, uint32_t value) {
    auto &port = *static_cast<Port *>(context);
    Soc &s = *port.soc;
    CHECK(port.block == PANEL_BLOCK_DSI && s.stages == PANEL_PROBE_AHB);
    CHECK(s.quiet && s.now >= s.quietSince + PANEL_PROBE_QUIET_MS * MS);
    s.now += READ_COST;
    s.dsiWrites.emplace_back(reg, value);
    Event e{Event::Write, s.now};
    e.block = PANEL_BLOCK_DSI;
    e.address = 0xae94000ull + reg;
    s.events.push_back(e);
    uint32_t &word = s.regs[e.address];
    word = reg == 0x110 ? (value & 0xa222aa02) | (word & 0x55555555 & ~value) : value;
    s.dsiWrote(reg, value);
}
static void dsiDelay(void *context, unsigned us) { static_cast<Port *>(context)->soc->now += us * MS / 1000; }

// One start with bit 8 as PaganiPanel.cpp runs it: setDispAhb's windows, the reads of logState, the first DCS with what comes before and after it. The DSI window of the command is the
// service's own, the only one here with a write behind it; setDispAhb's is read only. The frames: the link as the run's TE shows it, busy for 6.8 ms of each period.
struct Formal {
    Device d;
    Soc &s = d.s;
    const unsigned options;
    bool complete = true;
    bool on = false;
    unsigned mode = 0;
    int rc = 1;
    pagani_panel_run run{};
    Port ports[PANEL_PROBE_BLOCKS];
    peripheral_io dsi{};
    explicit Formal(unsigned value) : options(value) {
        s.live[0xae94008] = [this](uint64_t t) { return d.runs() && t % 159341 < 130560 ? 4u : 0u; };
        s.regs[0xae9403c] = 0x14000000; s.regs[0xae94390] = 2; s.regs[0xae9404c] = 32; s.regs[0xae94110] = 0x20010102;
        s.dsiWrote = [this](uint32_t reg, uint32_t) { if (reg == 0x90 && complete) s.regs[0xae94110] |= 1; };
    }
    static int send(void *context, uint8_t type, const uint8_t *payload, unsigned length) {
        auto &f = *static_cast<Formal *>(context);
        return pagani_panel_send_packet(&f.run, &f.dsi, f.mode, type, payload, length);
    }
    void start() {
        panel_probe_host host{};
        host.context = &s;
        for (unsigned i = 0; i < PANEL_PROBE_BLOCKS; ++i) {
            ports[i] = {&s, i};
            host.io[i] = {&ports[i], portRead, i == PANEL_BLOCK_AHB ? ahbWrite : portWrite, noDelay};
        }
        host.log = hostLog;
        host.sleep_ms = hostSleep;
        host.ticks = hostTicks;
        host.display_on = hostDisplay;
        host.quiet = hostQuiet;
        s.stages = PANEL_PROBE_AHB;
        s.start = s.now;
        panel_probe probe{};
        probe.host = &host;
        on = panel_probe_ahb(&probe, pagani_panel_ahb_careful(options)) != 0;
        hostLog(&s, "panel: first DSI read next");
        dsi = {&ports[PANEL_BLOCK_DSI], portRead, dsiWrite, dsiDelay};
        dsi_panel_state seen;
        dsi_panel_snapshot(&dsi, &seen);
        pagani_panel_run_init(&run, 0x599, 0);
        run.ahb = on;
        mode = pagani_panel_send_mode(options);
        hostLog(&s, "panel: first DCS next");
        panel_probe_host last{};
        last = host;
        last.io[PANEL_BLOCK_DSI] = dsi;
        const uint32_t frames = pagani_panel_ahb_before(&last, &run, options);
        rc = pagani_panel_start_dcs(&run, options, &seen, &mode, send, nullptr, this);
        rc = pagani_panel_ahb_after(frames, &last, &run, options, rc, &seen);
        CHECK(!s.quiet);                     // on every way out but a hang
        for (auto &e : s.events) CHECK(e.kind != Event::Line || e.text.rfind("panel: ", 0) == 0);
    }
    unsigned triggers() const {
        unsigned n = 0;
        for (auto &w : s.dsiWrites) n += w.first == 0x90;
        return n;
    }
    std::vector<uint32_t> fifo() const {
        std::vector<uint32_t> words;
        for (auto &w : s.dsiWrites) if (w.first == 0x17c) words.push_back(w.second);
        return words;
    }
    const Event *line(const char *text) const { return s.find(text); }
    uint64_t firstWrite(unsigned block) const {
        for (auto &e : s.events) if (e.kind == Event::Write && (block == PANEL_BLOCK_DSI ? e.block == PANEL_BLOCK_DSI : !e.block)) return e.at;
        return UINT64_MAX;
    }
    uint64_t lastSleep() const {
        uint64_t at = 0;
        for (auto &e : s.events) if (e.kind == Event::Sleep) at = e.at;
        return at;
    }
    // Written behind no line of its own, or not at all: nothing went to the controller or the clock.
    void nothing() const { CHECK(s.dsiWrites.empty() && run.held && !pagani_panel_publishes(options, rc)); }
};
enum { ONE = 1 | 64 | 256, SLIDER = ONE | 4096, BOTH_WAYS = 2 };

// A line that leaves the phone before what it names: the writer free when it went out and for a whole crumb after it, held for a whole QUIET_MS before `at`.
static void crumbBefore(const Formal &f, const char *text, uint64_t at) {
    const Event *line = f.line(text);
    CHECK(line && at != UINT64_MAX && at >= line->at + (PANEL_PROBE_CRUMB_MS + PANEL_PROBE_QUIET_MS) * MS);
    uint64_t freed = 0, held = 0;
    for (auto &e : f.s.events) {
        if (e.kind == Event::Quiet && !e.ms && e.at <= line->at) freed = e.at;
        if (e.kind == Event::Quiet && e.ms && e.at >= line->at && e.at < at) held = e.at;
    }
    CHECK(freed && held >= line->at + PANEL_PROBE_CRUMB_MS * MS && at >= held + PANEL_PROBE_QUIET_MS * MS);
}

static void formalCareful() {
    Formal f(ONE);
    Soc &s = f.s;
    f.start();
    CHECK(s.writes == (std::vector<std::pair<uint64_t, uint32_t>>{{0xaf080b0, 0x88000001}}));
    CHECK(!f.rc && f.on && f.run.held == PAGANI_PANEL_DCS_HELD_ONE && f.run.writes == 1 && f.triggers() == 1 && !f.mode);
    CHECK(f.fifo() == std::vector<uint32_t>({0xc0390003, 0xff990551}));
    inOrder(s, {"panel: ahb dispcc next", "panel: ahb state gcc 88000003 ahb 88000000 ahb1 0 rcg 0/100001 gdsc fa22f800/4070000 mdp 8000001", "panel: a1 write next ahb 88000000",
                "panel: a1 pre off ahb 64 mdp 0 byte 0 pclk 0 gdsc 0", "panel: a1 ahb 88000000->8000001 us ", "panel: ahb dsi next", "panel: first DSI read next",
                "panel: first DCS next", "panel: ahb dcs rc 0 clean held one frames 2->2"});
    // Each step's line off the phone before the step: the state, the clock's write, DSI behind the clock, the first write to DSI.
    uint64_t firstRead = UINT64_MAX, dsiRead = UINT64_MAX;
    const uint64_t written = f.firstWrite(PANEL_PROBE_BLOCKS);
    for (auto &e : s.events) if (e.kind == Event::Read && firstRead == UINT64_MAX) firstRead = e.at;
    for (auto &e : s.events) if (e.kind == Event::Read && e.block == PANEL_BLOCK_DSI && e.at > written && dsiRead == UINT64_MAX) dsiRead = e.at;
    crumbBefore(f, "panel: ahb dispcc next", firstRead);
    crumbBefore(f, "panel: a1 write next", written);
    crumbBefore(f, "panel: ahb dsi next", dsiRead);
    crumbBefore(f, "panel: first DCS next", f.firstWrite(PANEL_BLOCK_DSI));
    // The order that matters: the clock, then DSI behind it, then the command; no read of DSI behind a clock not running but the phase's own, which goes with the GDSC as in A1.
    CHECK(firstRead < written && written < dsiRead && dsiRead < f.firstWrite(PANEL_BLOCK_DSI));
    // What the boot pays: about 18 s, on a thread with nothing behind it yet.
    const uint64_t took = s.now - s.start;
    CHECK(took >= 18 * TICKS_S && took < 19 * TICKS_S);
    // Reads only from here on: a command that would come later goes nowhere, and reads nothing of DSI for it. Nothing published.
    const size_t before = s.dsiWrites.size();
    const uint32_t reads = s.reads;
    CHECK(!pagani_panel_publishes(ONE, f.rc) && Formal::send(&f, 0x39, reinterpret_cast<const uint8_t *>("\x51\x05\x99"), 3) == -19 && s.dsiWrites.size() == before && s.reads == reads);
    printf("ok  bit 8 alone: each step's line off the phone before it, one bit, one DCS, %.1f s of boot, then reads only, longest line %zu bytes\n", (double)took / TICKS_S, s.longest);
    if (std::getenv("PANEL_PROBE_SHOW")) s.print();
}

static void formalQuick() {
    Formal f(SLIDER);
    Soc &s = f.s;
    f.start();
    CHECK(s.writes == (std::vector<std::pair<uint64_t, uint32_t>>{{0xaf080b0, 0x88000001}}));
    CHECK(!f.rc && f.on && !f.run.held && f.run.checked && f.run.writes == 1 && f.triggers() == 1 && pagani_panel_publishes(SLIDER, f.rc));
    inOrder(s, {"panel: ahb next", "panel: ahb state", "panel: a1 write next", "panel: a1 pre", "panel: a1 ahb 88000000->8000001", "panel: first DSI read next", "panel: first DCS next",
                "panel: ahb dcs rc 0 clean held open frames 2->2"});
    // One hold, one sleep, the writer never let go in between.
    unsigned sleeps = 0, holds = 0;
    for (auto &e : s.events) {
        sleeps += e.kind == Event::Sleep;
        holds += e.kind == Event::Quiet && e.ms;
    }
    CHECK(sleeps == 1 && holds == 1 && s.now - s.start < 12 * TICKS_S / 10);
    // The slider through the same send, on the same model: the level goes, in a frame's gap as any other command. The request word is the other test's.
    pagani_panel_tick_result out;
    s.stages = PANEL_PROBE_AHB;
    hostQuiet(&s, 1);
    s.now += PANEL_PROBE_QUIET_MS * MS;  // the model's hold, for its reads: the tick does not take one
    pagani_panel_tick(&f.run, 5000000000ull, 1, pagani_brightness_request(0xdbb, 0, 0xdbb), Formal::send, &f, &out);
    hostQuiet(&s, 0);
    CHECK(!out.rc && f.fifo().back() == 0xffbb0d51 && f.triggers() == 2 && f.run.bright.shown == 0xdbb);
    printf("ok  bit 8 with bit 12: one hold of 1 s for all of it, the writer not let go in between, then the slider through the same send\n");
}

// Each of A1's gates alone, on both ways, in start: no write to anything, no command, nothing published, the writer free. Careful also with the word changing in the crumb before the write.
static void formalGates() {
    const struct {uint64_t reg; uint32_t bad; const char *why;} cases[] = {
        {0x127004, 0x88000000, "gcc"}, {0x127004, 0x80000001, "gcc"}, {0xaf080b0, 0x88000002, "hw gated"}, {0xaf08360, 0x80000000, "root off"},
        {0xaf08364, 0x100401, "not xo"}, {0xaf08364, 0x100601, "not xo"}, {0xaf09004, 0x4068000, "gdsc"},
    };
    for (unsigned options : {(unsigned)ONE, (unsigned)SLIDER}) for (auto &c : cases) for (unsigned late = 0; late < (options == ONE ? 2u : 1u); ++late) {
        Formal f(options);
        Soc &s = f.s;
        auto base = s.live.count(c.reg) ? s.live[c.reg] : nullptr;
        const uint32_t good = s.regs[c.reg], bad = c.bad;
        if (late) s.live[c.reg] = [&s, base, good, bad](uint64_t t) { return s.logged("panel: a1 write next") ? bad : base ? base(t) : good; };
        else if (c.reg == 0xaf080b0) f.d.ahb = bad;
        else s.regs[c.reg] = bad;
        f.start();
        char want[64];
        std::snprintf(want, sizeof want, "panel: a1 held %s ", c.why);
        CHECK(s.writes.empty() && s.logged(want) && !f.on && f.rc == -19);
        f.nothing();
        CHECK(s.logged("panel: ahb dcs rc -19 no read held "));
        // It ends there: no step behind the gate, no look at the frames for a command that will not come, no sleep past `first DCS next`.
        CHECK(!s.logged("panel: ahb dsi") && s.count(0xae94008) == 1 && f.lastSleep() < s.line("panel: first DCS next").size() + s.find("panel: first DCS next")->at);
    }
    {   // The clock controller reading as zero, the display off, one sample of the phase.
        Formal zero(SLIDER);
        for (auto it = zero.s.regs.begin(); it != zero.s.regs.end();) it = it->first >> 16 == 0xaf0 ? zero.s.regs.erase(it) : std::next(it);
        zero.d.ahb = 0;
        zero.start();
        CHECK(zero.s.writes.empty() && zero.s.logged("panel: a1 held zero"));
        zero.nothing();
        Formal off(SLIDER);
        off.s.display = [](uint64_t) { return 0u; };
        off.start();
        CHECK(off.s.writes.empty() && off.s.logged("panel: a1 held display"));
        off.nothing();
        Formal phase(SLIDER);
        unsigned seen = 0;
        phase.s.live[0xaf08034] = [&phase, &seen](uint64_t) { return phase.s.logged("panel: a1 write next") && ++seen == 37 ? 0u : 0x8000001u; };
        phase.start();
        CHECK(phase.s.writes.empty() && phase.s.logged("panel: a1 held phase"));
        phase.nothing();
    }
    printf("ok  bit 8 and A1's gates, each alone, both ways: nothing written anywhere, nothing published, the writer free\n");
}

static void formalOutcomes() {
    for (unsigned options : {(unsigned)ONE, (unsigned)SLIDER}) {
        const bool careful = options == ONE;
        {   // On already: nothing to write, the command goes.
            Formal f(options);
            f.d.ahb = 0x8000001;
            f.start();
            CHECK(f.s.writes.empty() && f.s.logged("panel: ahb 8000001 already on") && !f.rc && f.triggers() == 1);
        }
        {   // A write that does not take, a clock that does not start: no command, DSI as it reads, zero.
            Formal f(options);
            f.d.ignores = true;
            f.start();
            CHECK(f.s.writes.size() == 1 && f.s.logged("panel: a1 failed ignored") && f.run.held == PAGANI_PANEL_DCS_HELD_SHAPE);
            f.nothing();
            Formal g(options);
            g.d.sticks = true;
            g.start();
            CHECK(g.s.writes.size() == 1 && g.s.logged("panel: a1 failed stuck") && g.run.held == PAGANI_PANEL_DCS_HELD_SHAPE);
            g.nothing();
        }
        {   // The clock runs and DSI reads otherwise: the first read holds it.
            Formal f(options);
            for (uint64_t r : {0xae94000, 0xae94004, 0xae94084, 0xae9411c}) f.s.live.erase(r);
            f.start();
            CHECK(f.s.writes.size() == 1 && f.on && f.run.held == PAGANI_PANEL_DCS_HELD_SHAPE && f.rc == -19);
            f.nothing();
        }
        {   // The GDSC gone before DSI's gate, DSI still reading well: not the clock start asked for, whatever DSI shows.
            Formal f(options);
            f.s.live[0xaf09004] = [&f](uint64_t) { return f.s.logged("panel: a1 ahb 88000000->") ? 0u : 0x4070000u; };
            f.start();
            CHECK(!f.on && f.s.logged("panel: ahb dsi skipped gcc 88000003 ahb 8000001 cfg 0") && f.run.held == PAGANI_PANEL_DCS_HELD_AHB && f.rc == -19);
            f.nothing();
        }
        {   // The branch off again by itself before DSI's gate, the GDSC on, DSI still reading well: the same, the other half of that gate.
            Formal f(options);
            const auto base = f.s.live[0xaf080b0];
            f.s.live[0xaf080b0] = [&f, base](uint64_t t) { const uint32_t v = base(t); return f.s.logged("panel: a1 ahb 88000000->") ? 0x88000000u : v; };
            f.d.polls = 1;
            f.start();
            CHECK(!f.on && f.s.logged("panel: ahb dsi skipped gcc 88000003 ahb 88000000 cfg 4070000") && f.run.held == PAGANI_PANEL_DCS_HELD_AHB && f.rc == -19);
            f.nothing();
        }
        {   // The controller never takes the command: one trigger, -110, the engine keeps it, nothing more goes. Not published.
            Formal f(options);
            f.complete = false;
            f.start();
            CHECK(f.rc == -110 && f.triggers() == 1 && f.run.held == PAGANI_PANEL_DCS_HELD_LOST && !f.run.writes && f.s.regs[0xae9415c] == 0x30006);
            CHECK(f.s.logged("panel: ahb dcs rc -110 no read held lost frames 2->0") && !pagani_panel_publishes(options, f.rc));
            const size_t before = f.s.dsiWrites.size();
            CHECK(Formal::send(&f, 0x39, reinterpret_cast<const uint8_t *>("\x51\x05\x99"), 3) == -19 && f.s.dsiWrites.size() == before);
        }
        {   // The stream never lets go (status bit 2 for good): 20 ms, nothing written, nothing forced.
            Formal f(options);
            f.s.live[0xae94008] = [](uint64_t) { return 4u; };
            f.start();
            CHECK(f.rc == -16 && !f.mode && f.s.dsiWrites.empty() && f.run.held == PAGANI_PANEL_DCS_HELD_LOST && !pagani_panel_publishes(options, f.rc));
        }
        // A command that went and left the controller otherwise: -5, held, not published. Each change comes with the trigger, so the look before it saw nothing.
        const struct {const char *why; uint64_t reg; uint32_t value;} left[] = {
            {"shape", 0xae94000, 0}, {"shape", 0xae94084, 0}, {"config", 0xae9411c, 0x3f}, {"tpg", 0xae9415c, 0x30006}, {"busy", 0xae94008, 2}, {"timeout", 0xae940c0, 1}, {"fifo", 0xae9400c, 1u << 10},
            {"error", 0xae94110, 1u << 24 | 0x20010102},
        };
        for (auto &l : left) {
            Formal f(options);
            Soc &s = f.s;
            const uint64_t reg = l.reg;
            const uint32_t value = l.value;
            auto base = s.dsiWrote;
            s.dsiWrote = [&s, base, reg, value](uint32_t written, uint32_t v) {
                base(written, v);
                // After the restore's last write, the send's own end.
                if (written == 0x4c && s.regs[0xae9415c] == 0) {s.live.erase(reg); s.regs[reg] = value;}
            };
            f.start();
            char want[64];
            std::snprintf(want, sizeof want, "panel: ahb dcs rc -5 %s held lost", l.why);
            if (!s.logged(want)) s.print();
            CHECK(f.rc == -5 && f.triggers() == 1 && f.run.held == PAGANI_PANEL_DCS_HELD_LOST && s.logged(want) && !pagani_panel_publishes(options, f.rc));
        }
        {   // Frames seen before the command and gone after it, with every register in its place.
            Formal f(options);
            Soc &s = f.s;
            auto frames = s.live[0xae94008];
            s.live[0xae94008] = [&f, frames](uint64_t t) { return f.triggers() && !(f.s.regs[0xae9415c]) && f.s.dsiWrites.back().first == 0x4c ? 0u : frames(t); };
            f.start();
            CHECK(f.rc == -5 && f.run.held == PAGANI_PANEL_DCS_HELD_LOST && s.logged("panel: ahb dcs rc -5 stream held lost frames 2->0"));
        }
        {   // No frames on this bit before the command either: it shows nothing here, and proves nothing.
            Formal f(options);
            f.s.live[0xae94008] = [](uint64_t) { return 0u; };
            f.start();
            CHECK(!f.rc && f.triggers() == 1 && f.s.logged("panel: ahb dcs rc 0 clean held ") && f.s.logged(" frames 0->0"));
        }
        CHECK(careful == !!pagani_panel_ahb_careful(options));
    }
    // The windows of bit 8, the only stage that maps the writable page but A1; the kernel's bit 8 starts no thread for them.
    for (unsigned i = 0; i < PANEL_PROBE_BLOCKS; ++i) {
        const bool wanted = i == PANEL_BLOCK_GCC || i == PANEL_BLOCK_DSI || i == PANEL_BLOCK_DISP || i == PANEL_BLOCK_AHB;
        CHECK(!!(panel_probe_windows[i].stages & PANEL_PROBE_AHB) == wanted);
    }
    printf("ok  bit 8's outcomes, both ways: on already, a write that does not take, a clock that does not start, DSI otherwise, the GDSC gone, no answer, a busy stream, each register of the read back, frames gone\n");
}

int main() {
    healthy(); dsiZero(); gccOff(); dispccZero(); gates(); third(); preempted(); stages(); a0();
    a1Healthy(); a1Gates(); a1Preempted(); a1Outcomes(); a1Parts(); a1ThenStages();
    formalCareful(); formalQuick(); formalGates(); formalOutcomes();
    printf("PASS: PaganiPanel probe, %u checks\n", checks);
    return 0;
}
