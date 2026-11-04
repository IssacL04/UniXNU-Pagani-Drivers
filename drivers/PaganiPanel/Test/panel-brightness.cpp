// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test of PaganiPanel's brightness side: the request word and the tick (PanelBrightness.h) on a virtual clock, and what reaches the DSI
// FIFO through dsi_panel_packet_mode: 51 high low, high speed, the checks of the handoff, DSI_PANEL_FORCE, and a link under the MDP's autorefresh:
//  - wire: the panel's own tables byte for byte, high speed for brightness and display off, low power for the wake;
//  - handoff: -19 and -16 write nothing, the forced send goes out, the timeouts keep or restore as the engine reports, the snapshot only reads;
//  - first DCS: forced only when the checks refused it, the option allows it and the block answers reads; the mode that worked stays;
//    the gate before it: what a boot with the new bits lets through, what A0 holds back, no command of that boot reaching DSI later, and every
//    other value of pagani-panel doing, register for register, what it did before the gate existed;
//  - bit 8: its gates, nothing forced, what publishes, what each register of the read back may and may not do, a later command looking at the controller first; the slider down to the
//    wire on that path; a controller that goes away ends the commands. Its timing, its writer and the clock's side are in Test/panel-probe.cpp;
//  - options: what each bit of pagani-panel changes, the reporter's timing;
//  - link: frames at the DT's three timings with the MDP busy for its transfer time: the DMA goes in the gap, and the wait costs few polls;
//  - requests and curve: clamped to the panel's normal maximum, CoreBrightness's slider formula down to the wire, level 0 kept;
//  - fades: exact ends, monotonic, a newer request bends from the level shown;
//  - ticks: idle costs no timer; display off during a fade, requests while off, on again; retries and their limit, none for a dark panel; a stop;
//    no 0x28/0x29 with bit 6; sleep mode.
//   xcrun clang++ -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
//       -I <UniXNU-sm8750-common>/kits/Sm8750Platform -I <device>/package Test/panel-brightness.cpp -o /tmp/panel-brightness && /tmp/panel-brightness
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <utility>
#include <vector>
#include <PeripheralIO.h>
#include <PaganiPeripherals.h>
#include "../PanelBrightness.h"

static unsigned checks;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); std::abort(); } checks++; } while (0)

enum { MAX_LEVEL = PAGANI_BRIGHTNESS_MAX, BOOT_LEVEL = PAGANI_BRIGHTNESS_DEFAULT };

struct MMIO {
    std::map<uint32_t, uint32_t> regs;
    std::vector<std::pair<uint32_t, uint32_t>> writes;
    bool complete = true, busyOnTrigger = false;
    unsigned waited = 0, statusReads = 0, idleAfter = 0, reads = 0;
    static uint32_t read(void *ctx, uint32_t reg) {
        auto &m = *static_cast<MMIO *>(ctx);
        ++m.reads;
        if (reg == 8 && m.idleAfter) {if (++m.statusReads >= m.idleAfter) m.regs[8] = 0;}
        return m.regs[reg];
    }
    static void write(void *ctx, uint32_t reg, uint32_t value) {
        auto &m = *static_cast<MMIO *>(ctx);
        m.writes.emplace_back(reg, value);
        if (reg == 0x110) m.regs[reg] = (value & 0xa222aa02) | (m.regs[reg] & 0x55555555 & ~value);
        else m.regs[reg] = value;
        if (reg == 0x90 && m.complete) m.regs[0x110] |= 1;
        if (reg == 0x90 && m.busyOnTrigger) m.regs[8] = 2;
    }
    static void delay(void *ctx, unsigned us) { static_cast<MMIO *>(ctx)->waited += us; }
    peripheral_io io() { return {this, read, write, delay}; }
    void boot() { regs[4] = 0x1f5; regs[0x110] = 0x20010102; regs[0x3c] = 0x14000000; regs[0x390] = 2; regs[0x4c] = 32; }
    std::vector<uint32_t> fifo() const {
        std::vector<uint32_t> v;
        for (auto &w : writes) if (w.first == 0x17c) v.push_back(w.second);
        return v;
    }
    uint32_t written(uint32_t reg) const {
        for (auto &w : writes) if (w.first == reg) return w.second;
        return 0xdeadbeef;
    }
};

static void wire() {
    // The panel's own tables: hbm-off 51 0d bb, hbm-on 51 0e e0, exit-max 51 0f fe, hbm-max 51 10 ed; the boot default 0x599 as 51 05 99.
    const struct {unsigned level; uint32_t word;} table[] = {{0xdbb, 0xffbb0d51}, {0xee0, 0xffe00e51}, {0xffe, 0xfffe0f51}, {0x10ed, 0xffed1051},
                                                             {0x599, 0xff990551}, {1, 0xff010051}, {0, 0xff000051}};
    for (auto &t : table) {
        MMIO m; m.boot(); auto io = m.io();
        CHECK(!dsi_panel_command(&io, 0x51, t.level));
        CHECK(m.fifo() == std::vector<uint32_t>({0xc0390003, t.word}) && m.written(0x4c) == 8);
        CHECK(m.written(0x3c) == 0x10000000);                 // high speed: the DT's hbm command state and the default of bl_config.lp_mode
        CHECK(m.regs[0x3c] == 0x14000000 && m.regs[0x390] == 2 && m.regs[0x4c] == 32 && !m.regs[0x15c]);
    }
    MMIO m; m.boot(); auto io = m.io();
    CHECK(!dsi_panel_command_mode(&io, 0x51, 0x599, DSI_PANEL_LP) && m.written(0x3c) == 0x14000000);
    MMIO off; off.boot(); auto o = off.io();
    CHECK(!dsi_panel_command(&o, 0x28, 0) && off.fifo() == std::vector<uint32_t>({0x80050028, 0}) && off.written(0x4c) == 4 && off.written(0x3c) == 0x10000000);
    uint8_t on = 0x29;
    MMIO lp; lp.boot(); auto l = lp.io();
    CHECK(!dsi_panel_packet(&l, 0x05, &on, 1) && lp.written(0x3c) == 0x14000000);   // the on sequence stays low power
    printf("ok  wire: 51 high low, the panel's tables byte for byte; high speed for brightness and off, low power for the wake\n");
}

static void handoff() {
    // Enable bit clear: nothing written, -19, the caller may retry; forced, the packet goes out.
    MMIO a; a.boot(); a.regs[4] = 0; auto ia = a.io();
    CHECK(dsi_panel_command(&ia, 0x51, 0x599) == -19 && a.writes.empty());
    CHECK(!dsi_panel_command_mode(&ia, 0x51, 0x599, DSI_PANEL_FORCE) && a.fifo().size() == 2);
    // A test-pattern FIFO the boot loader left on: the same.
    MMIO b; b.boot(); b.regs[0x15c] = 0x30006; auto ib = b.io();
    CHECK(dsi_panel_command(&ib, 0x51, 0x599) == -19 && b.writes.empty());
    CHECK(!dsi_panel_command_mode(&ib, 0x51, 0x599, DSI_PANEL_FORCE) && !b.regs[0x15c]);
    // The MDP stream busy for good: -16 without a write after 20 ms; forced, the DMA goes out beside it, as on Android.
    MMIO c; c.boot(); c.regs[8] = 4; auto ic = c.io();
    CHECK(dsi_panel_command(&ic, 0x51, 0x599) == -16 && c.writes.empty() && c.waited == 20000);
    CHECK(!dsi_panel_command_mode(&ic, 0x51, 0x599, DSI_PANEL_FORCE));
    // A frame ending: the command waits for the gap.
    MMIO d; d.boot(); d.regs[8] = 4; d.idleAfter = 30; auto id = d.io();
    CHECK(!dsi_panel_command(&id, 0x51, 0x599) && d.waited >= 2900 && d.waited < 3100);
    // No DMA done: the checked send leaves the command to the engine; forced, with the engine not busy, the registers go back.
    MMIO e; e.boot(); e.complete = false; auto ie = e.io();
    CHECK(dsi_panel_command(&ie, 0x51, 0x599) == -110 && e.regs[0x15c] == 0x30006 && e.regs[0x4c] == 8);
    MMIO f; f.boot(); f.complete = false; auto jf = f.io();
    CHECK(dsi_panel_command_mode(&jf, 0x51, 0x599, DSI_PANEL_FORCE) == -110 && !f.regs[0x15c] && f.regs[0x3c] == 0x14000000 && f.regs[0x4c] == 32);
    MMIO g; g.boot(); g.complete = false; g.regs[8] = 2; g.idleAfter = 100000; auto jg = g.io();
    CHECK(dsi_panel_command_mode(&jg, 0x51, 0x599, DSI_PANEL_FORCE) == -16 && g.writes.empty());
    // Forced too: an engine that took the command and reports busy keeps it, nothing goes back under it.
    MMIO h; h.boot(); h.complete = false; h.busyOnTrigger = true; auto jh = h.io();
    CHECK(dsi_panel_command_mode(&jh, 0x51, 0x599, DSI_PANEL_FORCE) == -110 && h.regs[0x15c] == 0x30006 && h.regs[0x4c] == 8 && h.regs[0x3c] == 0x10000000);
    CHECK(h.regs[0x390] == 0 && !h.regs[0x1ec]);
    // The snapshot reads and never writes.
    MMIO s; s.boot(); s.regs[0] = 0x20090000; auto is = s.io(); dsi_panel_state st; dsi_panel_snapshot(&is, &st);
    CHECK(s.writes.empty() && st.version == 0x20090000 && st.ctrl == 0x1f5 && st.dma == 0x14000000);
    printf("ok  handoff: -19 and -16 write nothing, the forced send goes out, the timeouts keep or restore as the engine reports\n");
}

// The link under the MDP's autorefresh: a frame every `period` us, the stream busy for `transfer` of it (status bit 2), the command DMA for
// `dma` us after its trigger (bit 1). Time passes in the delay callback only, and a little with every register access.
struct Link {
    uint64_t now = 0, doneAt = 0, period, transfer, dma = 30;
    std::map<uint32_t, uint32_t> regs;
    unsigned statusReads = 0, triggers = 0, inFrame = 0;
    uint64_t lastWait = 0, began = 0;
    Link(uint64_t p, uint64_t t) : period(p), transfer(t) { regs[4] = 0x1f5; regs[0x3c] = 0x14000000; regs[0x390] = 2; regs[0x4c] = 32; regs[0x110] = 0x20000000; }
    bool stream() const { return now % period < transfer; }
    static uint32_t read(void *ctx, uint32_t reg) {
        auto &l = *static_cast<Link *>(ctx);
        l.now += 1;
        if (reg == 8) {
            ++l.statusReads;
            return (l.stream() ? 4u : 0u) | (l.now < l.doneAt ? 2u : 0u);
        }
        if (reg == 0x110) return l.regs[reg] | (l.doneAt && l.now >= l.doneAt ? 1u : 0u);
        return l.regs[reg];
    }
    static void write(void *ctx, uint32_t reg, uint32_t value) {
        auto &l = *static_cast<Link *>(ctx);
        l.now += 1;
        if (reg == 0x90) {
            ++l.triggers;
            if (l.stream()) ++l.inFrame;
            l.doneAt = l.now + l.dma;
            l.lastWait = l.now - l.began;
            return;
        }
        if (reg == 0x110) { l.regs[reg] = value & ~1u; if (value & 1) l.doneAt = 0; return; }
        l.regs[reg] = value;
    }
    static void delay(void *ctx, unsigned us) { static_cast<Link *>(ctx)->now += us; }
    peripheral_io io() { return {this, read, write, delay}; }
};

// The DT's three timings: qcom,mdss-mdp-transfer-time-us 0x1a90 at 120 and 60 Hz, 0x2328 at 90 Hz.
static void link() {
    const struct {uint64_t period, transfer;} modes[] = {{8333, 6800}, {16667, 6800}, {11111, 9000}};
    for (auto &mode : modes) {
        Link l(mode.period, mode.transfer);
        auto io = l.io();
        uint64_t longest = 0;
        unsigned polls = 0, seed = 12345;
        for (unsigned i = 0; i < 2000; ++i) {
            seed = seed * 1103515245u + 12345u;
            l.now += seed % 8333 + 1;            // the fades' steps land anywhere in a frame
            l.began = l.now;
            const unsigned before = l.statusReads;
            CHECK(!dsi_panel_command(&io, 0x51, 0x400 + (i & 0x3ff)));
            if (l.lastWait > longest) longest = l.lastWait;
            if (l.statusReads - before > polls) polls = l.statusReads - before;
        }
        // A status read in the last microseconds of a gap can still see the next frame start before the trigger (about 20 register accesses
        // later): the hardware arbitrates that one, as on Android. It stays the exception here: about 20 us of a frame's 8333. This counts the
        // link alone, with no thread in it: a thread overtaken between the read and the trigger lands anywhere in a frame, so this is no upper bound.
        CHECK(l.triggers == 2000 && l.inFrame <= 2000 / 100);
        CHECK(longest < mode.transfer + 300);                    // at most the frame on its way
        CHECK(polls <= mode.transfer / DSI_PANEL_POLL_US + 3);   // about 70 polls in the worst case at 120 Hz, not 700
        CHECK(l.regs[0x3c] == 0x14000000 && l.regs[0x4c] == 32 && !l.regs[0x15c]);
        printf("ok  link at %.1f Hz: 2000 commands, %u started as a frame began (arbitrated), longest wait %.1f ms, at most %u status polls\n",
               1000000.0 / (double)mode.period, l.inFrame, (double)longest / 1000.0, polls);
    }
}

static void requests() {
    const uint32_t max = MAX_LEVEL;
    uint64_t w = pagani_brightness_request(0x400, 0x8000, max);        // 0.5 s in 16.16
    CHECK(pagani_brightness_request_valid(w) && pagani_brightness_request_level(w) == 0x400 && pagani_brightness_request_fade(w) == 500);
    CHECK(!pagani_brightness_request_valid(0));
    w = pagani_brightness_request(0x7fffffffull, 0, max);
    CHECK(pagani_brightness_request_level(w) == max && pagani_brightness_request_fade(w) == 0);
    w = pagani_brightness_request(0x100000000ull, 0, max);
    CHECK(pagani_brightness_request_level(w) == max);
    CHECK(pagani_brightness_request_level(pagani_brightness_request(0xffffffffull, 0, max)) == 0);   // -1 from CoreBrightness's int: dark, not full
    w = pagani_brightness_request(1, 0xffffffffu, max);
    CHECK(pagani_brightness_request_level(w) == 1 && pagani_brightness_request_fade(w) == PAGANI_BRIGHTNESS_FADE_MAX_MS);
    CHECK(pagani_brightness_request_level(pagani_brightness_request(0, 0, max)) == 0);
    printf("ok  requests: clamped to the panel's normal maximum and to 5 s, valid bit, level 0 kept, a negative level is the dark end\n");
}

// CoreBrightness's legacy path (__DisplaySetPhysicalBrightnessLegacyOnThread): level = ceil(x * factor * (max - min)) + min with the min and
// max of IODisplayParameters, slider floor (min + 1) / max (DisplayCreateWithID).
static void curve() {
    const uint32_t min = 0, max = MAX_LEVEL;
    // The board's numbers against the panel's: oplus,dsi-bl-normal-max-level and the 35% of the boot level.
    CHECK(max == 0xdbb && max == PAGANI_PANEL_DBV_NORMAL && max <= PAGANI_PANEL_DBV_MAX && max < 1u << 12 && BOOT_LEVEL == (35 * 4095 + 50) / 100);
    const double floor = (double)(min + 1) / max;
    uint32_t previous = 0, lowest = 0xffff, highest = 0, sent[2] = {0, 0};
    for (unsigned step = 0; step <= 100000; ++step) {
        const double x = floor + (1.0 - floor) * step / 100000;
        const uint32_t level = (uint32_t)std::ceil(x * (max - min)) + min;
        const uint64_t word = pagani_brightness_request(level, 0, max);
        const uint32_t taken = pagani_brightness_request_level(word);
        CHECK(taken == level && taken >= previous && taken >= 1 && taken <= max);
        previous = taken;
        if (taken < lowest) lowest = taken;
        if (taken > highest) highest = taken;
        if (step % 5000) continue;
        MMIO m; m.boot(); auto io = m.io();
        CHECK(!dsi_panel_command(&io, 0x51, taken));
        const uint32_t word1 = m.fifo().at(1);
        CHECK((word1 & 0xff) == 0x51 && ((word1 >> 8) & 0xff) == taken >> 8 && ((word1 >> 16) & 0xff) == (taken & 0xff) && (word1 >> 8 & 0xf0) == 0);
        sent[step ? 1 : 0] = word1;
    }
    CHECK(lowest == 1 && highest == max && sent[0] == 0xff010051 && sent[1] == 0xffbb0d51);     // the panel's 1 and hbm-off's own 51 0d bb
    // Beyond what CoreBrightness can ask: nothing reaches past the normal maximum, in 12 bits, whatever the number.
    for (uint64_t level = 0; level < 0x20000; level += 7) {
        const uint32_t taken = pagani_brightness_request_level(pagani_brightness_request(level, 0, max));
        CHECK(taken <= max && taken == (level < max ? level : max));
    }
    printf("ok  curve: the slider's floor gives level 1 (51 00 01), its top 0xdbb (51 0d bb, the panel's own hbm-off), monotonic, 12 bits, the normal maximum never passed\n");
}

static void fades() {
    pagani_brightness b; pagani_brightness_init(&b, 0x599);
    CHECK(pagani_brightness_done(&b) && pagani_brightness_level(&b, 12345) == 0x599);
    // At once.
    pagani_brightness_begin(&b, 0x800, 0, 1000);
    CHECK(pagani_brightness_level(&b, 1000) == 0x800 && !pagani_brightness_done(&b));
    b.shown = 0x800; CHECK(pagani_brightness_done(&b));
    // 100 ms up: the ends exact, monotonic on 16 ms steps.
    const uint64_t t0 = 5000000000ull;
    pagani_brightness_begin(&b, 0xc00, 100, t0);
    CHECK(pagani_brightness_level(&b, t0) == 0x800);
    uint32_t previous = 0x800; unsigned steps = 0;
    for (uint64_t t = t0; ; t += PAGANI_BRIGHTNESS_STEP_MS * 1000000ull) {
        const uint32_t next = pagani_brightness_level(&b, t);
        CHECK(next >= previous && next <= 0xc00);
        previous = next; b.shown = next; ++steps;
        if (pagani_brightness_done(&b)) break;
        CHECK(steps < 100);
    }
    CHECK(previous == 0xc00 && steps >= 6 && steps <= 8);
    // Down to 0 (CoreBrightness's display off), and a newer request in the middle bends from what is shown.
    pagani_brightness_begin(&b, 0, 1000, t0);
    b.shown = pagani_brightness_level(&b, t0 + 500000000ull);
    CHECK(b.shown == 0x600);
    pagani_brightness_begin(&b, 0xdbb, 1000, t0 + 500000000ull);
    CHECK(pagani_brightness_level(&b, t0 + 500000000ull) == 0x600);
    CHECK(pagani_brightness_level(&b, t0 + 1500000000ull) == 0xdbb);
    printf("ok  fades: goal at once, exact ends, monotonic, %u steps for 100 ms, a newer request starts from the level shown\n", steps);
}

// PaganiPanel's timer on a virtual clock: a tick when the last one asked for it, or when the display or a request arms it (1 us later).
struct Bench {
    pagani_panel_run r;
    uint64_t now = 7000000000ull;
    unsigned want = 1, options = 0;
    uint64_t word = 0;
    uint32_t armed = 0;                    // ms until the tick the last one asked for, 0: none
    unsigned ticks = 0;
    struct Sent { uint64_t at; uint8_t type; std::vector<uint8_t> bytes; };
    std::vector<Sent> sent;
    int fail = 0;                          // refuse this many sends with `error`
    int error = -16;
    size_t failAt = SIZE_MAX;              // refuse one send, the one with this many before it
    pagani_panel_tick_result last{};
    explicit Bench(int sleep) { pagani_panel_run_init(&r, BOOT_LEVEL, sleep); }
    static int send(void *ctx, uint8_t type, const uint8_t *payload, unsigned length) {
        auto &b = *static_cast<Bench *>(ctx);
        if (b.fail) {--b.fail; return b.error;}
        if (b.sent.size() == b.failAt) {b.failAt = SIZE_MAX; return b.error;}
        b.sent.push_back({b.now, type, std::vector<uint8_t>(payload, payload + length)});
        return 0;
    }
    void tick() {
        ++ticks;
        pagani_panel_tick(&r, now, pagani_panel_want(options, want), word, send, this, &last);
        word = 0;
        armed = last.next_ms;
    }
    // A request or the display arms the timer: the tick follows at once.
    void ask(uint32_t level, uint32_t fadeMs) { word = pagani_brightness_request(level, (uint32_t)(((uint64_t)fadeMs << 16) / 1000), MAX_LEVEL); tick(); }
    void power(unsigned on) { want = on; tick(); }
    // Time passes for ms, the ticks asked for run; stops at a stop.
    void run(unsigned ms) {
        const uint64_t end = now + (uint64_t)ms * 1000000;
        while (armed && !last.stopped) {
            const uint64_t at = now + (uint64_t)armed * 1000000;
            if (at > end) { armed -= (uint32_t)((end - now) / 1000000); break; }
            now = at;
            tick();
        }
        if (now < end) now = end;
    }
    unsigned count(uint8_t first) const {
        unsigned n = 0;
        for (auto &s : sent) n += s.bytes[0] == first;
        return n;
    }
    uint32_t level(size_t i) const { return (uint32_t)sent[i].bytes[1] << 8 | sent[i].bytes[2]; }
};

static void ticks() {
    // Idle: a tick with nothing to do sends nothing and asks for no other, and no timer runs for an idle panel (the old one ran 62.5 times a second).
    Bench idle(0);
    idle.tick();
    CHECK(idle.sent.empty() && !idle.armed && !idle.last.publish && !idle.last.rc);
    idle.run(10000);
    CHECK(idle.ticks == 1);

    // A goal at once: one command, published, then quiet.
    Bench once(0);
    once.ask(0x800, 0);
    CHECK(once.sent.size() == 1 && once.level(0) == 0x800 && once.sent[0].type == 0x39 && once.sent[0].bytes[0] == 0x51);
    CHECK(once.last.publish && !once.armed && once.r.bright.shown == 0x800 && once.r.writes == 1);

    // A fade: steps of 16 ms, monotonic, the exact goal, one publication, then no timer.
    Bench fade(0);
    fade.ask(BOOT_LEVEL + 800, 160);
    CHECK(fade.armed == PAGANI_BRIGHTNESS_STEP_MS && !fade.last.publish);
    unsigned published = 0;
    while (fade.armed) {
        fade.run(fade.armed);
        published += fade.last.publish;
        if (fade.ticks > 100) break;
    }
    CHECK(published == 1 && fade.r.bright.shown == BOOT_LEVEL + 800 && !fade.armed && fade.ticks <= 13 && fade.sent.size() >= 9);
    for (size_t i = 1; i < fade.sent.size(); i++) CHECK(fade.level(i) > fade.level(i - 1));

    // Slider drag: requests faster than the steps; the newest wins, nothing queues up (a word, not a queue: one replaces the other).
    Bench drag(0);
    drag.word = pagani_brightness_request(0x100, 0, MAX_LEVEL);
    drag.word = pagani_brightness_request(0x900, 0, MAX_LEVEL);
    drag.tick();
    CHECK(drag.sent.size() == 1 && drag.level(0) == 0x900);

    // Display off in the middle of a fade: 0x28, no brightness while off, what is asked meanwhile waits; on: 0x29 first, then the goal.
    Bench off(0);
    off.ask(0xc00, 1000);
    off.run(200);
    const size_t before = off.sent.size();
    const uint32_t reached = off.r.bright.shown;
    CHECK(before >= 12 && reached > BOOT_LEVEL && reached < 0xc00);
    off.power(0);
    CHECK(off.sent.size() == before + 1 && off.sent.back().bytes == std::vector<uint8_t>({0x28}) && off.last.publish && !off.r.shown);
    off.run(2000);
    CHECK(off.sent.size() == before + 1 && off.r.bright.shown == reached && !off.armed && off.r.power.offs == 1);   // the timer stops while off
    off.ask(0x300, 0);
    CHECK(off.sent.size() == before + 1 && off.r.bright.shown == reached && !off.armed);
    off.power(1);
    CHECK(off.sent.size() == before + 3 && off.sent[before + 1].bytes == std::vector<uint8_t>({0x29}) && off.level(before + 2) == 0x300);
    CHECK(off.r.shown && off.r.bright.shown == 0x300 && !off.armed && off.r.power.ons == 1);

    // Fading to 0 and display off as CoreBrightness does, then back: 0 reaches the panel, and the way up goes through a request.
    Bench dark(0);
    dark.ask(0, 32);
    dark.run(100);
    CHECK(dark.r.bright.shown == 0 && !dark.armed);
    dark.power(0);
    dark.run(100);
    dark.power(1);
    CHECK(dark.count(0x29) == 1 && dark.r.bright.shown == 0);
    dark.ask(0x700, 0);
    CHECK(dark.r.bright.shown == 0x700);

    // Off and straight on again: 0x29 only after 0x28's 20 ms, the DT's delay; nothing lost.
    Bench quick(0);
    quick.power(0);
    quick.want = 1;
    quick.tick();
    CHECK(quick.sent.size() == 1 && quick.armed > 0);
    quick.run(40);
    CHECK(quick.sent.size() == 2 && quick.sent[1].bytes[0] == 0x29 && quick.sent[1].at - quick.sent[0].at >= 20000000 && quick.r.shown);

    // Not sent (-16, -19: nothing was written): every 100 ms again, the goal kept, the count clean after success.
    Bench busy(0);
    busy.fail = 3;
    busy.ask(0x800, 0);
    CHECK(busy.last.rc == -16 && !busy.last.stopped && busy.armed == PAGANI_PANEL_RETRY_MS && busy.r.retries == 1 && busy.r.bright.shown == BOOT_LEVEL);
    busy.run(250);
    CHECK(busy.r.retries == 3 && busy.sent.empty());
    busy.run(100);
    CHECK(busy.sent.size() == 1 && busy.level(0) == 0x800 && !busy.r.retries && busy.r.retried == 3 && !busy.armed);
    // The display's power too, and a newer goal replaces the one waiting.
    Bench busyOff(0);
    busyOff.fail = 2; busyOff.error = -19;
    busyOff.power(0);
    CHECK(busyOff.last.rc == -19 && busyOff.r.power.seq == pagani_panel_off && busyOff.armed == PAGANI_PANEL_RETRY_MS);
    busyOff.run(250);
    CHECK(busyOff.sent.size() == 1 && busyOff.sent[0].bytes[0] == 0x28);
    Bench busyNew(0);
    busyNew.fail = 1;
    busyNew.ask(0x800, 0);
    busyNew.ask(0x400, 0);
    CHECK(busyNew.sent.size() == 1 && busyNew.level(0) == 0x400);

    // The limit: PAGANI_PANEL_RETRIES in a row, then the stop; one minute of a refusing controller, not a retry forever.
    Bench limit(0);
    limit.fail = 1000000;
    limit.ask(0x800, 0);
    limit.run(70000);
    CHECK(limit.last.stopped && limit.last.rc == -16 && limit.r.retried == PAGANI_PANEL_RETRIES && limit.ticks == PAGANI_PANEL_RETRIES + 1 && !limit.last.next_ms);

    // Display off refused as long: the panel still shows, so the limit holds.
    Bench stays(0);
    stays.fail = 1000000; stays.error = -19;
    stays.power(0);
    stays.run(70000);
    CHECK(stays.last.stopped && stays.last.rc == -19 && stays.r.retried == PAGANI_PANEL_RETRIES && stays.r.power.state == PAGANI_PANEL_ON && stays.sent.empty());

    // Display on refused past the limit: the panel is dark and only this brings it back, so it never stops, and it goes when the controller takes it.
    for (int error : {-16, -19}) {
        Bench back(0);
        back.power(0);
        back.run(100);
        CHECK(back.r.power.state == PAGANI_PANEL_BLANK && back.sent.size() == 1);
        back.fail = 1000000; back.error = error;
        back.power(1);
        back.run(70000);
        CHECK(!back.last.stopped && back.last.rc == error && back.armed == PAGANI_PANEL_RETRY_MS && back.r.retried > PAGANI_PANEL_RETRIES && back.r.retries == back.r.retried);
        back.fail = 0;
        back.run(100);
        CHECK(back.sent.size() == 2 && back.sent[1].bytes == std::vector<uint8_t>({0x29}) && back.r.shown && !back.armed && !back.r.retries);
    }
    // The same in the middle of the wake from sleep mode, and for the sleep after 0x28 went out: the display comes back from there.
    Bench wake(1);
    wake.power(0);
    wake.run(300);
    wake.fail = 1;                                            // 0x11 refused once, then it goes and its 120 ms run
    wake.power(1);
    wake.run(100);
    CHECK(wake.sent.size() == 3 && wake.sent[2].bytes == std::vector<uint8_t>({0x11}) && wake.r.power.seq == pagani_panel_wake);
    wake.fail = 1000000;
    wake.run(70000);
    CHECK(wake.sent.size() == 3 && wake.r.power.seq == pagani_panel_wake && !wake.last.stopped && wake.r.retried > PAGANI_PANEL_RETRIES);
    wake.fail = 0;
    wake.run(300);
    CHECK(wake.r.shown && wake.r.power.wakes == 1 && !wake.armed && !wake.r.retries);
    Bench asleep(1);
    asleep.power(0);
    asleep.fail = 1000000;
    asleep.run(70000);
    CHECK(asleep.sent.size() == 1 && !asleep.last.stopped && asleep.r.power.seq == pagani_panel_off && asleep.r.retried > PAGANI_PANEL_RETRIES);
    asleep.fail = 0;
    asleep.power(1);
    asleep.run(100);
    CHECK(asleep.r.shown && asleep.sent.back().bytes == std::vector<uint8_t>({0x29}) && !asleep.count(0x10));

    // No DMA done: a command the engine may still own gets no other, at once. On 0x29 as well: a dark panel does not change what it may still own.
    Bench lost(0);
    lost.fail = 1; lost.error = -110;
    lost.ask(0x800, 0);
    CHECK(lost.last.stopped && lost.last.rc == -110 && !lost.armed && !lost.r.retried && lost.r.bright.shown == BOOT_LEVEL);
    Bench lostOn(0);
    lostOn.power(0);
    lostOn.run(100);
    lostOn.fail = 1; lostOn.error = -110;
    lostOn.power(1);
    CHECK(lostOn.last.stopped && lostOn.last.rc == -110 && !lostOn.armed && !lostOn.r.retried);

    // Bit 6: the system's display off and on send nothing, in either mode, the brightness stays in use and the counters stay at 0.
    for (int mode : {0, 1}) {
        Bench keep(mode);
        keep.options = PAGANI_PANEL_OPTION_NO_DISPLAY_POWER;
        keep.power(0);
        CHECK(keep.sent.empty() && keep.r.shown && !keep.armed && !keep.last.publish);
        keep.ask(0x300, 0);
        CHECK(keep.sent.size() == 1 && keep.level(0) == 0x300);
        keep.power(1);
        keep.run(1000);
        CHECK(keep.sent.size() == 1 && !keep.r.power.offs && !keep.r.power.ons && !keep.r.power.sleeps && !keep.r.power.wakes && keep.r.power.state == PAGANI_PANEL_ON);
    }

    // Sleep mode: back from it the level goes again, also unchanged; a goal asked while asleep goes after the wake.
    Bench sleep(1);
    sleep.power(0);
    sleep.run(300);
    CHECK(sleep.r.power.state == PAGANI_PANEL_ASLEEP && sleep.sent.size() == 2 && !sleep.armed);
    sleep.power(1);
    sleep.run(300);
    CHECK(sleep.r.power.state == PAGANI_PANEL_ON && sleep.r.shown && !sleep.r.stale && !sleep.armed);
    CHECK(sleep.sent.size() == 3 + PAGANI_PANEL_WAKE_PACKETS - 1 + 1);
    CHECK(sleep.sent.back().bytes[0] == 0x51 && sleep.level(sleep.sent.size() - 1) == BOOT_LEVEL);
    Bench sleep2(1);
    sleep2.power(0);
    sleep2.run(300);
    sleep2.ask(0x123, 0);
    sleep2.power(1);
    sleep2.run(300);
    CHECK(sleep2.sent.back().bytes[0] == 0x51 && sleep2.level(sleep2.sent.size() - 1) == 0x123 && sleep2.count(0x51) >= 1 && !sleep2.r.stale);
    // The level refused right after the wake stays owed: it goes with the retry, also unchanged.
    Bench owed(1);
    owed.power(0);
    owed.run(300);
    owed.failAt = 2 + PAGANI_PANEL_WAKE_PACKETS;              // the packet after 0x28, 0x10 and the wake
    owed.power(1);
    owed.run(130);
    CHECK(owed.r.shown && owed.r.stale && owed.last.rc == -16 && owed.armed);
    CHECK(owed.sent.size() == 2 + PAGANI_PANEL_WAKE_PACKETS && owed.sent.back().bytes[0] == 0x29);
    owed.run(100);
    CHECK(!owed.r.stale && owed.sent.back().bytes[0] == 0x51 && owed.level(owed.sent.size() - 1) == BOOT_LEVEL && !owed.armed);
    printf("ok  ticks: idle costs no timer, fades step and end exactly, display off holds the goal, on sends 0x29 then the goal, -19/-16 retry every "
           "100 ms up to %d times and without limit on a dark panel, -110 stops at once, bit 6 sends no 0x28/0x29, sleep mode writes the level again\n", (int)PAGANI_PANEL_RETRIES);
}

// The kernel's sendPacket on the register model: the same call, the driver's mode word beside the run.
struct Wire {
    MMIO *bus;
    pagani_panel_run *run;
    unsigned mode = 0, forcings = 0;
    size_t writesAtForcing = SIZE_MAX;
    static int send(void *ctx, uint8_t type, const uint8_t *payload, unsigned length) {
        auto &w = *static_cast<Wire *>(ctx);
        auto io = w.bus->io();
        return pagani_panel_send_packet(w.run, &io, w.mode, type, payload, length);
    }
    static void forcing(void *ctx) {
        auto &w = *static_cast<Wire *>(ctx);
        ++w.forcings;
        w.writesAtForcing = w.bus->writes.size();
    }
};

static void firstDcs() {
    const uint32_t version = 0x20090000;
    struct Run { MMIO m; pagani_panel_run r; Wire w; int rc;
        Run(unsigned options, uint32_t ver, void (*setup)(MMIO &)) : w{&m, &r} {
            m.boot(); setup(m); pagani_panel_run_init(&r, BOOT_LEVEL, 0); w.mode = pagani_panel_send_mode(options);
            rc = pagani_panel_first_dcs(&r, options, ver, &w.mode, Wire::send, Wire::forcing, &w);
        }
    };
    // The handoff as the sibling boards read it: the boot level again, 51 05 99, nothing forced.
    Run good(0, version, [](MMIO &) {});
    CHECK(!good.rc && good.w.mode == 0 && !good.w.forcings && good.r.writes == 1 && good.m.fifo() == std::vector<uint32_t>({0xc0390003, 0xff990551}));
    // Build 50's reading, enable bit clear: refused, then forced, logged first, and the commands after it the same way.
    Run forced(0, version, [](MMIO &m) { m.regs[4] = 0; });
    CHECK(!forced.rc && forced.w.mode == DSI_PANEL_FORCE && forced.w.forcings == 1 && !forced.w.writesAtForcing && forced.r.writes == 1);
    pagani_panel_tick_result out;
    pagani_panel_tick(&forced.r, 1000, 1, pagani_brightness_request(0x700, 0, MAX_LEVEL), Wire::send, &forced.w, &out);
    CHECK(!out.rc && forced.r.bright.shown == 0x700 && forced.m.fifo().back() == 0xff000751);
    // The stream busy for 20 ms: forced, it goes beside it. The DMA busy for good: not forced either, and the mode goes back.
    Run stream(0, version, [](MMIO &m) { m.regs[8] = 4; });
    CHECK(!stream.rc && stream.w.mode == DSI_PANEL_FORCE && stream.w.forcings == 1);
    Run dma(0, version, [](MMIO &m) { m.regs[8] = 2; });
    CHECK(dma.rc == -16 && dma.w.mode == 0 && dma.w.forcings == 1 && !dma.r.writes && dma.m.writes.empty());
    // Forced without an answer: the error stands, no write counted, the mode goes back.
    Run lost(0, version, [](MMIO &m) { m.regs[4] = 0; m.complete = false; });
    CHECK(lost.rc == -110 && lost.w.mode == 0 && lost.w.forcings == 1 && !lost.r.writes);
    // Bit 2: the refusal stands, nothing written.
    Run refused(PAGANI_PANEL_OPTION_NO_FORCE, version, [](MMIO &m) { m.regs[4] = 0; });
    CHECK(refused.rc == -19 && refused.w.mode == 0 && !refused.w.forcings && refused.m.writes.empty() && !refused.r.writes);
    // A block that reads as zero (version 0): nothing can complete there, so no first write of this port goes to it.
    Run dead(0, 0, [](MMIO &m) { m.regs.clear(); });
    CHECK(dead.rc == -19 && dead.w.mode == 0 && !dead.w.forcings && dead.m.writes.empty() && !dead.r.writes);
    // Bit 3: the first DCS is low power too.
    Run low(PAGANI_PANEL_OPTION_LOW_POWER, version, [](MMIO &) {});
    CHECK(!low.rc && low.w.mode == DSI_PANEL_LP && low.m.written(0x3c) == 0x14000000);
    printf("ok  first DCS: forced only when refused, allowed and the block reads; the log comes first; the mode that worked stays, a failed one goes back\n");
}

// One boot as start runs it: the registers, the snapshot, the gate and the first DCS, then ticks through the kernel's send.
struct StartRun {
    MMIO m;
    pagani_panel_run r;
    Wire w;
    int rc;
    unsigned reads = 0;
    StartRun(unsigned options, uint32_t version, uint32_t ctrl, uint32_t trigger, uint32_t status, unsigned clock = 0) : w{&m, &r} {
        m.boot();
        m.regs[0] = version;
        m.regs[4] = ctrl;
        m.regs[0x84] = trigger;
        m.regs[8] = status;
        pagani_panel_run_init(&r, BOOT_LEVEL, 0);
        r.ahb = (uint8_t)clock;
        w.mode = pagani_panel_send_mode(options);
        auto io = m.io();
        dsi_panel_state seen;
        dsi_panel_snapshot(&io, &seen);
        rc = pagani_panel_start_dcs(&r, options, &seen, &w.mode, Wire::send, Wire::forcing, &w);
    }
    int tick(uint64_t request) {
        pagani_panel_tick_result out;
        pagani_panel_tick(&r, 1000, 1, request, Wire::send, &w, &out);
        return out.rc;
    }
};

static void dcsGate() {
    const uint32_t version = 0x20090000, ctrl = 0x1f5, trigger = 0x4;
    const unsigned guarded[] = {129, 193, 195, 1089, 1217, 1219, 2113 | 4096, 193 | 4096, 513, 577, 579};
    CHECK(pagani_panel_guarded(128) && pagani_panel_guarded(1024) && pagani_panel_guarded(2048) && pagani_panel_guarded(512) && !pagani_panel_guarded(4096) && !pagani_panel_guarded(127) &&
          !pagani_panel_guarded(4096 | 127) && pagani_panel_guarded(256));
    // The shape: version 2.9 with any step, DSI_EN and CMD_MODE_EN, the DMA trigger on software; other bits of ctrl and trigger do not matter.
    for (unsigned options : guarded) {
        CHECK(pagani_panel_dcs_gate(options, version, ctrl, trigger) == PAGANI_PANEL_DCS_OPEN && pagani_panel_dcs_gate(options, 0x2009ffff, 5, 0xff4) == PAGANI_PANEL_DCS_OPEN);
        for (uint32_t other : {0x20080000u, 0x200a0000u, 0x30090000u, 1u, 0u})
            CHECK(pagani_panel_dcs_gate(options, other, ctrl, trigger) == PAGANI_PANEL_DCS_HELD_SHAPE);
        for (uint32_t other : {0u, 1u, 4u, 0x1f0u, 0x1f1u, 0x1f4u}) CHECK(pagani_panel_dcs_gate(options, version, other, trigger) == PAGANI_PANEL_DCS_HELD_SHAPE);
        for (uint32_t other : {0u, 1u, 5u, 0xcu, 0xfu}) CHECK(pagani_panel_dcs_gate(options, version, ctrl, other) == PAGANI_PANEL_DCS_HELD_SHAPE);
    }
    // A0: reads only, whatever the registers show, also when they look perfect; bit 12 lets the shape decide, and version 0 stops there.
    CHECK(pagani_panel_dcs_gate(2048 | 1, version, ctrl, trigger) == PAGANI_PANEL_DCS_HELD_A0 && pagani_panel_dcs_gate(2048 | 1, 0, 0, 0) == PAGANI_PANEL_DCS_HELD_A0);
    CHECK(pagani_panel_dcs_gate(2048 | 128 | 2, version, ctrl, trigger) == PAGANI_PANEL_DCS_HELD_A0);
    CHECK(pagani_panel_dcs_gate(4096 | 2048 | 1, version, ctrl, trigger) == PAGANI_PANEL_DCS_OPEN);
    CHECK(pagani_panel_dcs_gate(4096 | 2048 | 1, version, ctrl, 0) == PAGANI_PANEL_DCS_HELD_SHAPE && pagani_panel_dcs_gate(4096 | 2048 | 1, 0, 0, 0) == PAGANI_PANEL_DCS_HELD_SHAPE);

    // Every value without the new bits, on every state of the controller, does register for register what pagani_panel_first_dcs did before the gate: =65 is
    // the package's, Build 50's forced send still goes, and bit 12 alone changes nothing.
    const struct {uint32_t version, ctrl, trigger, status;} states[] = {
        {0, 0, 0, 0}, {version, ctrl, trigger, 0}, {version, 0, trigger, 0}, {version, ctrl, 0, 0}, {0x20080000, ctrl, trigger, 0}, {1, 0, 0, 0}, {version, ctrl, trigger, 4}, {0, ctrl, trigger, 0}};
    for (unsigned bits = 0; bits < 64; ++bits) for (unsigned twelve : {0u, 4096u}) for (auto &state : states) {
        const unsigned options = 1 | bits << 1 | twelve;
        StartRun now(options, state.version, state.ctrl, state.trigger, state.status);
        MMIO m; m.boot(); m.regs[0] = state.version; m.regs[4] = state.ctrl; m.regs[0x84] = state.trigger; m.regs[8] = state.status;
        pagani_panel_run r; pagani_panel_run_init(&r, BOOT_LEVEL, 0);
        Wire w{&m, &r};
        w.mode = pagani_panel_send_mode(options);
        const int before = pagani_panel_first_dcs(&r, options, state.version, &w.mode, Wire::send, Wire::forcing, &w);
        CHECK(now.rc == before && !now.r.held && now.m.writes == m.writes && now.w.mode == w.mode && now.w.forcings == w.forcings && now.r.writes == r.writes);
    }
    StartRun build50(65, version, 0, trigger, 0);
    CHECK(!build50.rc && build50.w.mode == DSI_PANEL_FORCE && build50.w.forcings == 1 && !build50.r.held);
    StartRun sm8750(65, 0, 0, 0, 0);
    CHECK(sm8750.rc == -19 && sm8750.m.writes.empty() && !sm8750.w.forcings && !pagani_panel_publishes(65, -19) && !pagani_panel_probe_stages(65) && !pagani_panel_gcc_ahb(65));
    // =67 keeps its way: version 0 at start, ctrl answering later, and the tick sends, as it always did.
    StartRun published(67, 0, 0, 0, 0);
    CHECK(published.rc == -19 && pagani_panel_publishes(67, published.rc) && !published.r.held);
    published.m.regs[4] = 0x1f5;
    CHECK(!published.tick(pagani_brightness_request(0x400, 0, MAX_LEVEL)) && published.m.fifo().size() == 2);

    // A held boot writes nothing and reads nothing to do it, at start and in every tick, whatever DSI reads by then. =1219: version 0 at start, ctrl with
    // bit 0 at the tick, the case of a probe that publishes. =577 and =579 the same, DSI coming back with A1 at 154 s: the first write to DSI is no side effect of it.
    for (unsigned options : {1219u, 195u, 2113u, 2115u, 6209u, 2048u | 128u | 2u | 1u, 577u, 579u}) {
        StartRun held(options, 0, 0, 0, 0);
        CHECK(held.rc == -19 && held.r.held && held.m.writes.empty() && !held.w.forcings && !held.r.writes);
        held.m.regs[0] = version; held.m.regs[4] = ctrl; held.m.regs[0x84] = trigger;
        CHECK(held.tick(pagani_brightness_request(0x400, 0, MAX_LEVEL)) == -19 && held.tick(0) == -19 && held.m.writes.empty() && held.r.retried == 2);
    }
    // After A0 with the registers as they should be: still read only without bit 12; with it the first DCS goes, 51 05 99.
    StartRun a0(2113, version, ctrl, trigger, 0);
    CHECK(a0.rc == -19 && a0.r.held == PAGANI_PANEL_DCS_HELD_A0 && a0.m.writes.empty());
    StartRun a0Dcs(6209, version, ctrl, trigger, 0);
    CHECK(!a0Dcs.rc && !a0Dcs.r.held && a0Dcs.m.fifo() == std::vector<uint32_t>({0xc0390003, 0xff990551}));
    StartRun a0Wrong(6209, version, ctrl, 0, 0);
    CHECK(a0Wrong.rc == -19 && a0Wrong.r.held == PAGANI_PANEL_DCS_HELD_SHAPE && a0Wrong.m.writes.empty());
    // A probe on a working controller: the first DCS goes as it would without the probe, the ticks after it too.
    StartRun working(1217, version, ctrl, trigger, 0);
    CHECK(!working.rc && !working.r.held && working.m.fifo() == std::vector<uint32_t>({0xc0390003, 0xff990551}));
    CHECK(!working.tick(pagani_brightness_request(0x400, 0, MAX_LEVEL)));
    // Held, nothing is forced on the way: the shape that failed is the reason, not a check the force would skip.
    StartRun refused(1217, version, 0, trigger, 0);
    CHECK(refused.rc == -19 && refused.r.held == PAGANI_PANEL_DCS_HELD_SHAPE && !refused.w.forcings && !refused.w.mode && refused.m.writes.empty());
    printf("ok  DCS gate: a boot with the new bits goes only on the shape of a working controller, A0 reads only without bit 12, nothing of a held boot reaches DSI later; every other value as before\n");
}

// Bit 8, on the registers. Its clock, its timing and the writer are the other test's: this one takes what start found as given.
static void formal() {
    const uint32_t version = 0x20090000, ctrl = 0x1f5, trigger = 0x4;
    const unsigned one = 1 | 64 | 256, slider = one | 4096;
    CHECK(PAGANI_PANEL_OPTION_AHB_START == 256 && one == 321 && slider == 4417);
    CHECK(pagani_panel_ahb_start(one) && pagani_panel_ahb_careful(one) && !pagani_panel_ahb_slider(one));
    CHECK(pagani_panel_ahb_start(slider) && !pagani_panel_ahb_careful(slider) && pagani_panel_ahb_slider(slider));
    // Whatever else the word says, nothing of this without bit 8; and what publishes without it is what always did.
    for (unsigned options = 0; options < 1u << 13; ++options) {
        if (pagani_panel_ahb_start(options)) {
            CHECK(pagani_panel_guarded(options) && !pagani_panel_publishes(options, -19) && !pagani_panel_publishes(options, -16) && !pagani_panel_publishes(options, -110) && !pagani_panel_publishes(options, -5));
            CHECK(pagani_panel_publishes(options, 0) == !!(options & 4096));
        } else {
            CHECK(!pagani_panel_ahb_careful(options) && !pagani_panel_ahb_slider(options));
            for (int first : {0, -19, -16, -110}) CHECK(!!pagani_panel_publishes(options, first) == (!first || (options & 2)));
        }
    }
    CHECK(!pagani_panel_probe_stages(slider) && !pagani_panel_gcc_ahb(slider) && pagani_panel_probe_stages(slider | 128) == (PANEL_PROBE_P0 | PANEL_PROBE_P1));

    // The clock on, the controller in shape: the boot level again, 51 05 99, high speed, one command.
    for (unsigned options : {one, slider}) {
        StartRun good(options, version, ctrl, trigger, 0, 1);
        CHECK(!good.rc && !good.r.held && good.r.checked && good.w.mode == 0 && !good.w.forcings && good.r.writes == 1);
        CHECK(good.m.fifo() == std::vector<uint32_t>({0xc0390003, 0xff990551}) && good.m.written(0x3c) == 0x10000000);
    }
    // Not found on: no command whatever DSI reads, and nothing written or read for it.
    {
        StartRun off(slider, version, ctrl, trigger, 0, 0);
        const unsigned reads = off.m.reads;
        CHECK(off.rc == -19 && off.r.held == PAGANI_PANEL_DCS_HELD_AHB && off.m.writes.empty() && !off.r.writes);
        CHECK(off.tick(pagani_brightness_request(0x400, 0, MAX_LEVEL)) == -19 && off.m.writes.empty() && off.m.reads == reads);
    }
    // The clock on and the shape not that of a working controller: the same, whichever of the three reads otherwise.
    const struct {uint32_t version, ctrl, trigger;} shapes[] = {{0, 0, 0}, {0x20080000, ctrl, trigger}, {version, 0x1f4, trigger}, {version, 0x1f1, trigger}, {version, ctrl, 0}};
    for (auto &shape : shapes) {
        StartRun wrong(slider, shape.version, shape.ctrl, shape.trigger, 0, 1);
        CHECK(wrong.rc == -19 && wrong.r.held == PAGANI_PANEL_DCS_HELD_SHAPE && wrong.m.writes.empty() && !wrong.w.forcings);
    }
    // A refusal stands: nothing is forced here, for the stream, the DMA, the test-pattern FIFO or a clear enable bit. Without bit 8 the same registers go on to the forced send.
    StartRun stream(slider, version, ctrl, trigger, 4, 1);
    CHECK(stream.rc == -16 && !stream.w.forcings && !stream.w.mode && stream.m.writes.empty() && !stream.r.writes);
    StartRun dma(slider, version, ctrl, trigger, 2, 1);
    CHECK(dma.rc == -16 && !dma.w.forcings && !dma.w.mode && dma.m.writes.empty());
    StartRun plain(1, version, ctrl, trigger, 4);
    CHECK(!plain.rc && plain.w.mode == DSI_PANEL_FORCE && plain.w.forcings == 1);
    {
        MMIO m; m.boot(); m.regs[0] = version; m.regs[0x84] = trigger; m.regs[0x15c] = 0x30006;
        pagani_panel_run r; pagani_panel_run_init(&r, BOOT_LEVEL, 0); r.ahb = 1;
        Wire w{&m, &r};
        auto io = m.io();
        dsi_panel_state seen;
        dsi_panel_snapshot(&io, &seen);
        CHECK(pagani_panel_start_dcs(&r, slider, &seen, &w.mode, Wire::send, Wire::forcing, &w) == -19 && !w.forcings && m.writes.empty());
    }

    // What one command leaves, and each thing it may not leave. The registers of a healthy read first, then one change at a time.
    dsi_panel_state before{};
    before.version = version; before.ctrl = ctrl; before.trigger = trigger; before.dma = 0x14000000; before.clocks = 0x23f; before.status = 4; before.lanes = 0x1f; before.ints = 0x20010102;
    CHECK(!pagani_panel_readback(&before, &before));
    {   // What moves by itself is no matter: the frame on its way, the lanes, the done bit's neighbours.
        dsi_panel_state a = before;
        a.status = 0; a.lanes = 0; a.ints = 0x20010103; a.ack = 1; a.secure = 7;
        CHECK(!pagani_panel_readback(&before, &a));
    }
    const struct {const char *why; void (*change)(dsi_panel_state &);} changes[] = {
        {"shape", [](dsi_panel_state &a) { a.version = 0; }},                     // the block is gone
        {"shape", [](dsi_panel_state &a) { a.version = 0x20090001; }},            // no other version from one read to the next
        {"shape", [](dsi_panel_state &a) { a.ctrl = 0; }},
        {"shape", [](dsi_panel_state &a) { a.trigger = 0; }},
        {"config", [](dsi_panel_state &a) { a.ctrl = 0x1f7; }},
        {"config", [](dsi_panel_state &a) { a.trigger = 0x14; }},
        {"config", [](dsi_panel_state &a) { a.dma = 0x10000000; }},               // not restored: the next frame's DMA setup
        {"config", [](dsi_panel_state &a) { a.clocks = 0x3f; }},
        {"tpg", [](dsi_panel_state &a) { a.tpg = 0x30006; }},                     // the FIFO's mode stayed
        {"busy", [](dsi_panel_state &a) { a.status = 6; }},                       // the engine still has it
        {"timeout", [](dsi_panel_state &a) { a.timeout = 1; }},
        {"timeout", [](dsi_panel_state &a) { a.timeout = 0x10; }},
        {"timeout", [](dsi_panel_state &a) { a.timeout = 0x100; }},
        {"fifo", [](dsi_panel_state &a) { a.fifo = 1u << 7; }},
        {"fifo", [](dsi_panel_state &a) { a.fifo = 1u << 10; }},
        {"fifo", [](dsi_panel_state &a) { a.fifo = 1u << 18; }},
        {"fifo", [](dsi_panel_state &a) { a.fifo = 1u << 19; }},
        {"fifo", [](dsi_panel_state &a) { a.fifo = 1u << 22; }},
        {"fifo", [](dsi_panel_state &a) { a.fifo = 1u << 23; }},
        {"fifo", [](dsi_panel_state &a) { a.fifo = 1u << 26; }},
        {"fifo", [](dsi_panel_state &a) { a.fifo = 1u << 27; }},
        {"fifo", [](dsi_panel_state &a) { a.fifo = 1u << 30; }},
        {"fifo", [](dsi_panel_state &a) { a.fifo = 1u << 31; }},
        {"error", [](dsi_panel_state &a) { a.ints |= 1u << 24; }},
    };
    for (auto &c : changes) {
        dsi_panel_state a = before;
        c.change(a);
        const char *why = pagani_panel_readback(&before, &a);
        CHECK(why && !std::strcmp(why, c.why));
    }
    {   // An error the boot loader left is not this command's: only a new one counts.
        dsi_panel_state left = before;
        left.timeout = 0x111; left.fifo = PAGANI_DSI_FIFO_ERRORS; left.ints |= 1u << 24;
        dsi_panel_state a = left;
        CHECK(!pagani_panel_readback(&left, &a));
        // Bits that say nothing of an error, in the FIFO status and the timeouts: the empty flags that come and go.
        dsi_panel_state b = before;
        b.fifo = ~PAGANI_DSI_FIFO_ERRORS; b.timeout = ~(uint32_t)PAGANI_DSI_TIMEOUTS;
        CHECK(!pagani_panel_readback(&before, &b));
    }
    CHECK(PAGANI_DSI_FIFO_ERRORS == (1u << 7 | 1u << 10 | 3u << 18 | 3u << 22 | 3u << 26 | 3u << 30));

    // The slider on this path: the request as CoreBrightness sends it down to the wire, high byte first, the fade's steps, the panel's maximum; each command looks first.
    StartRun open(slider, version, ctrl, trigger, 0, 1);
    uint64_t now = 2000000000ull;
    pagani_panel_tick_result out;
    pagani_panel_tick(&open.r, now, pagani_panel_want(slider, 0), pagani_brightness_request(0xdbb, 0, MAX_LEVEL), Wire::send, &open.w, &out);
    CHECK(!out.rc && open.m.fifo().back() == 0xffbb0d51 && open.r.bright.shown == 0xdbb && out.publish);
    pagani_panel_tick(&open.r, now, 1, pagani_brightness_request(0x7fffffff, 0, MAX_LEVEL), Wire::send, &open.w, &out);
    CHECK(!out.rc && open.m.fifo().back() == 0xffbb0d51 && open.r.bright.shown == 0xdbb);
    pagani_panel_tick(&open.r, now, 1, pagani_brightness_request(0xdbb - 800, (1u << 16) * 160 / 1000, MAX_LEVEL), Wire::send, &open.w, &out);
    unsigned steps = 0;
    while (out.next_ms && steps < 40) {
        now += (uint64_t)out.next_ms * 1000000;
        ++steps;
        pagani_panel_tick(&open.r, now, 1, 0, Wire::send, &open.w, &out);
    }
    CHECK(!out.rc && open.r.bright.shown == 0xdbb - 800 && steps >= 9 && steps <= 12 && !open.r.held && open.m.fifo().back() == 0xff9b0a51);
    // Bit 6: the system's display off sends nothing here either.
    const size_t sent = open.m.fifo().size();
    pagani_panel_tick(&open.r, now, pagani_panel_want(slider, 0), 0, Wire::send, &open.w, &out);
    CHECK(!out.rc && open.m.fifo().size() == sent && !open.r.power.offs);
    // Without bit 6: 0x28 and 0x29 go the same way, behind the same look.
    StartRun power(1 | 256 | 4096, version, ctrl, trigger, 0, 1);
    pagani_panel_tick(&power.r, now, 0, 0, Wire::send, &power.w, &out);
    CHECK(!out.rc && power.m.fifo().back() == 0 && power.m.fifo()[power.m.fifo().size() - 2] == 0x80050028);

    // The controller goes away under a later command, each of the three looks: nothing written, -19, the same for good, without another read of it; the tick stops at its limit,
    // and the commands do not come back with the controller.
    for (uint32_t reg : {0u, 4u, 0x84u}) {
        StartRun gone(slider, version, ctrl, trigger, 0, 1);
        const uint32_t kept = gone.m.regs[reg];
        const size_t written = gone.m.writes.size();
        gone.m.regs[reg] = 0;
        CHECK(gone.tick(pagani_brightness_request(0x700, 0, MAX_LEVEL)) == -19 && gone.r.held == PAGANI_PANEL_DCS_HELD_LOST && gone.m.writes.size() == written);
        const unsigned reads = gone.m.reads;
        gone.m.regs[reg] = kept;
        CHECK(gone.tick(pagani_brightness_request(0x700, 0, MAX_LEVEL)) == -19 && gone.m.writes.size() == written && gone.m.reads == reads && gone.r.bright.shown == BOOT_LEVEL);
        for (unsigned i = 0; i < PAGANI_PANEL_RETRIES + 2 && !out.stopped; ++i) pagani_panel_tick(&gone.r, 1000, 1, 0, Wire::send, &gone.w, &out);
        CHECK(out.stopped && out.rc == -19 && gone.m.writes.size() == written);
        out.stopped = 0;
    }
    // Without bit 8 no command looks first: =67 stays what it was.
    StartRun old(65, version, ctrl, trigger, 0);
    const size_t written = old.m.writes.size();
    const unsigned reads = old.m.reads;
    old.m.regs[0] = 0; old.m.regs[0x84] = 0;
    CHECK(!old.r.checked && !old.tick(pagani_brightness_request(0x700, 0, MAX_LEVEL)) && old.m.writes.size() > written && !old.r.held && old.m.reads > reads);
    CHECK(!std::strcmp(pagani_panel_held_name(PAGANI_PANEL_DCS_HELD_A0), "a0") && !std::strcmp(pagani_panel_held_name(PAGANI_PANEL_DCS_HELD_SHAPE), "shape") &&
          !std::strcmp(pagani_panel_held_name(PAGANI_PANEL_DCS_HELD_AHB), "ahb") && !std::strcmp(pagani_panel_held_name(PAGANI_PANEL_DCS_HELD_LOST), "lost") &&
          !std::strcmp(pagani_panel_held_name(PAGANI_PANEL_DCS_HELD_ONE), "one") && !std::strcmp(pagani_panel_held_name(99), "?"));
    printf("ok  bit 8: its gates, nothing forced, what publishes, each register of the read back, the slider to the wire, a controller that goes away ends the commands, nothing of it without the bit\n");
}

static void options() {
    // The bits as README.md lists them, and =1 is all defaults.
    CHECK(PAGANI_PANEL_OPTION_PUBLISH_ALWAYS == 2 && PAGANI_PANEL_OPTION_NO_FORCE == 4 && PAGANI_PANEL_OPTION_LOW_POWER == 8 && PAGANI_PANEL_OPTION_NO_BACKLIGHT == 16 &&
          PAGANI_PANEL_OPTION_NO_FADE == 32 && PAGANI_PANEL_OPTION_NO_DISPLAY_POWER == 64 && PAGANI_PANEL_OPTION_PROBE == 128 && PAGANI_PANEL_OPTION_PROBE_DEEP == 1024 &&
          PAGANI_PANEL_OPTION_GCC_AHB == 2048 && PAGANI_PANEL_OPTION_A0_DCS == 4096 && PAGANI_PANEL_OPTION_AHB == 512);
    CHECK(!pagani_panel_probe_stages(127) && !pagani_panel_gcc_ahb(127 | 4096) && !pagani_panel_probe_stages(2048 | 4096 | 256));
    CHECK(pagani_panel_probe_stages(577) == PANEL_PROBE_A1 && pagani_panel_probe_stages(579) == PANEL_PROBE_A1 && !pagani_panel_gcc_ahb(577));
    CHECK(pagani_panel_probe_stages(1217 | 512) == (PANEL_PROBE_P0 | PANEL_PROBE_P1 | PANEL_PROBE_P2 | PANEL_PROBE_P3 | PANEL_PROBE_A1));
    CHECK(pagani_panel_probe_stages(1 | 64 | 128) == (PANEL_PROBE_P0 | PANEL_PROBE_P1) && pagani_panel_probe_stages(1 | 1024) == (PANEL_PROBE_P2 | PANEL_PROBE_P3));
    CHECK(pagani_panel_probe_stages(1217) == (PANEL_PROBE_P0 | PANEL_PROBE_P1 | PANEL_PROBE_P2 | PANEL_PROBE_P3) && pagani_panel_gcc_ahb(2049));
    CHECK(pagani_panel_send_mode(1) == 0 && pagani_panel_want(1, 0) == 0 && pagani_panel_want(1, 1) == 1 && pagani_panel_fade(1, 0x8000) == 0x8000);
    CHECK(pagani_panel_publishes(1, 0) && !pagani_panel_publishes(1, -19) && !pagani_panel_publishes(1, -110) && pagani_panel_backlight(1));
    CHECK(pagani_panel_publishes(3, -19) && pagani_panel_publishes(3, 0));
    CHECK(pagani_panel_send_mode(9) == DSI_PANEL_LP);
    CHECK(!pagani_panel_backlight(17) && pagani_panel_publishes(17, 0) && pagani_panel_want(17, 0) == 0);    // =17 keeps the DCS and the display power
    CHECK(pagani_panel_fade(33, 0x8000) == 0);
    CHECK(pagani_panel_want(65, 0) == 1 && pagani_panel_want(65, 1) == 1 && pagani_panel_backlight(65));     // =65 keeps the backlight
    CHECK(pagani_panel_want(81, 0) == 1 && !pagani_panel_backlight(81));
    // A packet's mode: the driver's own, and low power for the wake alone.
    pagani_panel_run r; pagani_panel_run_init(&r, BOOT_LEVEL, 1);
    CHECK(pagani_panel_packet_mode(&r, 0) == 0 && pagani_panel_packet_mode(&r, DSI_PANEL_FORCE) == DSI_PANEL_FORCE);
    r.power.seq = pagani_panel_off; CHECK(pagani_panel_packet_mode(&r, 0) == 0);
    r.power.seq = pagani_panel_display_on; CHECK(pagani_panel_packet_mode(&r, 0) == 0);
    r.power.seq = pagani_panel_wake; CHECK(pagani_panel_packet_mode(&r, DSI_PANEL_FORCE) == (DSI_PANEL_LP | DSI_PANEL_FORCE));

    // The reporter: the registers once with the display on, then a line when the counters move and every ten periods.
    const unsigned both = PAGANI_PANEL_REPORT_COUNTERS | PAGANI_PANEL_REPORT_REGISTERS;
    pagani_panel_reports a{};
    CHECK(pagani_panel_report_due(&a, 5, 1, 0) == both);
    for (unsigned i = 1; i < PAGANI_PANEL_REPORT_QUIET; ++i) CHECK(!pagani_panel_report_due(&a, 5, 1, 0));
    CHECK(pagani_panel_report_due(&a, 5, 1, 0) == PAGANI_PANEL_REPORT_COUNTERS);
    CHECK(pagani_panel_report_due(&a, 6, 1, 0) == PAGANI_PANEL_REPORT_COUNTERS && !pagani_panel_report_due(&a, 6, 1, 0));
    // Never the registers with the display off: they wait for it; a line still goes.
    pagani_panel_reports b{};
    CHECK(pagani_panel_report_due(&b, 1, 0, 0) == PAGANI_PANEL_REPORT_COUNTERS && !pagani_panel_report_due(&b, 1, 0, 0));
    CHECK(pagani_panel_report_due(&b, 1, 1, 0) == both);
    // Commands stopped: the registers again every ten periods, with the display on; not with it off.
    pagani_panel_reports c{};
    CHECK(pagani_panel_report_due(&c, 1, 1, 1) == both);
    for (unsigned i = 1; i < PAGANI_PANEL_REPORT_QUIET; ++i) CHECK(!pagani_panel_report_due(&c, 1, 1, 1));
    CHECK(pagani_panel_report_due(&c, 1, 1, 1) == both);
    for (unsigned i = 1; i < PAGANI_PANEL_REPORT_QUIET; ++i) CHECK(!pagani_panel_report_due(&c, 1, 0, 1));
    CHECK(pagani_panel_report_due(&c, 1, 0, 1) == PAGANI_PANEL_REPORT_COUNTERS);
    printf("ok  options: each bit of pagani-panel alone and in the combinations the images use; the wake alone is low power; the reporter's lines\n");
}

static void throughDsi() {
    MMIO m; m.boot(); m.regs[0x3c] = 0x14000000;
    pagani_panel_run r; pagani_panel_run_init(&r, BOOT_LEVEL, 1);
    Wire w{&m, &r};
    pagani_panel_tick_result out;
    uint64_t now = 5000000000ull;
    pagani_panel_tick(&r, now, 1, pagani_brightness_request(0xdbb, 0, MAX_LEVEL), Wire::send, &w, &out);
    CHECK(!out.rc && m.fifo() == std::vector<uint32_t>({0xc0390003, 0xffbb0d51}) && m.written(0x3c) == 0x10000000);
    m.writes.clear();
    pagani_panel_tick(&r, now, 0, 0, Wire::send, &w, &out);                   // 0x28 high speed, the DT's off-command-state
    CHECK(!out.rc && m.fifo() == std::vector<uint32_t>({0x80050028, 0}) && m.written(0x3c) == 0x10000000);
    for (unsigned i = 0; i < 40; ++i) { now += 16000000; pagani_panel_tick(&r, now, 0, 0, Wire::send, &w, &out); }
    CHECK(!out.rc && r.power.state == PAGANI_PANEL_ASLEEP);
    m.writes.clear();
    pagani_panel_tick(&r, now, 1, 0, Wire::send, &w, &out);                   // 0x11 low power, the DT's on-command-state
    CHECK(!out.rc && m.fifo() == std::vector<uint32_t>({0x80050011, 0}) && m.written(0x3c) == 0x14000000);
    for (unsigned i = 0; i < 16; ++i) { now += 16000000; pagani_panel_tick(&r, now, 1, 0, Wire::send, &w, &out); }
    const std::vector<uint32_t> fifo = m.fifo();
    CHECK(!out.rc && r.shown && fifo[fifo.size() - 2] == 0xc0390003 && fifo.back() == 0xffbb0d51);   // the level again, high speed
    uint32_t dma = 0, last = 0;                                               // DMA_CTRL as each trigger found it: the last one's
    for (auto &w : m.writes) {
        if (w.first == 0x3c) dma = w.second;
        if (w.first == 0x90) last = dma;
    }
    CHECK(last == 0x10000000 && m.regs[0x3c] == 0x14000000 && !m.regs[0x15c]);
    printf("ok  through the registers: brightness and off high speed, the wake low power, the level again after it, registers restored\n");
}

int main() {
    wire(); handoff(); firstDcs(); dcsGate(); formal(); options(); link(); requests(); curve(); fades(); ticks(); throughDsi();
    printf("PASS: PaganiPanel brightness, %u checks\n", checks);
    return 0;
}
