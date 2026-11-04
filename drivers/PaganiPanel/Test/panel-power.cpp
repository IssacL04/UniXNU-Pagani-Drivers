// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test of PaganiPanel's power sequences (PanelPower.h, boot-arg pagani-screenoff bit 2) and of the DSI packet writer they go
// through (Sm8750Platform PeripheralIO.h dsi_panel_packet), against the DSI register model of the peripheral tests and a virtual
// clock ticking like the panel's 16 ms work-loop timer:
//  - dsi_panel_packet: exact FIFO words, lengths and register restore for short (0x05, 0x15) and long (0x39, 1-16 bytes) packets;
//    the register traffic of dsi_panel_command for 0x28, 0x29 and 0x51; refusals (types, lengths, no payload, DSI off, TPG busy,
//    engine busy) without a write; a completion timeout leaves the engine's command untouched;
//  - the wake table: 53 packets pinned by CRC-32 to the live DT's on-command from exit sleep on; the off table;
//  - the sequences: off with sleep (0x28, at least 20 ms, 0x10, at least 120 ms), back (0x11, at least 120 ms, the 51 writes in one
//    tick, 0x29, the brightness refresh), without sleep (0x28 / 0x29 only, what PaganiPanel always sent), requests that change
//    in the middle (back on before 0x10 skips the sleep; on during the sleep wait waits for it; off during the wake runs it to its
//    end first), a send failure stops where it failed, and the whole wake through dsi_panel_packet and the register model.
// Build: xcrun clang++ -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
//        -I <UniXNU-sm8750-common>/kits/Sm8750Platform Test/panel-power.cpp -o /tmp/panel-power
#include "../PanelPower.h"
#include <PeripheralIO.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

static unsigned checks;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); std::abort(); } checks++; } while (0)

// The DSI controller model of the peripheral tests (PaganiInput/Test/peripherals.cpp): INT_CTRL status bits are write-one-to-clear,
// a software trigger completes the command DMA at once unless `complete` is false.
struct MMIO {
    std::map<uint32_t, uint32_t> regs;
    std::vector<std::pair<uint32_t, uint32_t>> writes;
    bool complete = true;
    unsigned waited = 0;
    static uint32_t read(void *ctx, uint32_t reg) { return static_cast<MMIO *>(ctx)->regs[reg]; }
    static void write(void *ctx, uint32_t reg, uint32_t value)
    {
        auto &m = *static_cast<MMIO *>(ctx);
        m.writes.emplace_back(reg, value);
        if (reg == 0x110) m.regs[reg] = (value & 0xa222aa02) | (m.regs[reg] & 0x55555555 & ~value);
        else m.regs[reg] = value;
        if (reg == 0x90 && m.complete) m.regs[0x110] |= 1;
    }
    static void delay(void *ctx, unsigned us) { static_cast<MMIO *>(ctx)->waited += us; }
    peripheral_io io() { return {this, read, write, delay}; }
    void boot() { regs[4] = 1; regs[0x110] = 0x20010102; regs[0x3c] = 0xa5; regs[0x390] = 2; regs[0x4c] = 32; }
    std::vector<uint32_t> fifo() const
    {
        std::vector<uint32_t> v;
        for (auto &w : writes) if (w.first == 0x17c) v.push_back(w.second);
        return v;
    }
    uint32_t length() const
    {
        uint32_t n = 0;
        for (auto &w : writes) if (w.first == 0x4c && w.second != 32) n = w.second;
        return n;
    }
};

static uint32_t crc32(const uint8_t *p, size_t n)
{
    uint32_t c = 0xffffffffu;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (unsigned k = 0; k < 8; k++) c = c & 1 ? (c >> 1) ^ 0xedb88320u : c >> 1;
    }
    return ~c;
}

static void testPacketWriter()
{
    // Short packets: header (last-command flag, type, data bytes) and the zero word of the odd count; 4 bytes long.
    MMIO m;
    m.boot();
    auto io = m.io();
    const uint8_t off = 0x28, sleepIn = 0x10, param[2] = {0x60, 0x01};
    CHECK(!dsi_panel_packet(&io, 0x05, &off, 1));
    CHECK(m.fifo() == std::vector<uint32_t>({0x80050028, 0}) && m.length() == 4);
    CHECK(m.regs[0x3c] == 0xa5 && m.regs[0x390] == 2 && m.regs[0x4c] == 32 && !m.regs[0x15c]);
    CHECK((m.regs[0x110] & 0x10100) == 0x10100);         // unrelated completions are not acknowledged
    m.writes.clear();
    CHECK(!dsi_panel_packet(&io, 0x05, &sleepIn, 1) && m.fifo() == std::vector<uint32_t>({0x80050010, 0}));
    m.writes.clear();
    CHECK(!dsi_panel_packet(&io, 0x15, param, 2) && m.fifo() == std::vector<uint32_t>({0x80150160, 0}) && m.length() == 4);
    // Long packets: header with the long flag and the word count, payload little-endian padded with 0xff, a zero word when the count
    // is odd; length = 4 + the payload rounded up to a word.
    const uint8_t page[4] = {0xff, 0x5a, 0xa5, 0x51}, table[7] = {0x80, 0xd4, 0xd4, 0xd4, 0xd4, 0x22, 0x22};
    m.writes.clear();
    CHECK(!dsi_panel_packet(&io, 0x39, page, 4) && m.fifo() == std::vector<uint32_t>({0xc0390004, 0x51a55aff}) && m.length() == 8);
    m.writes.clear();
    CHECK(!dsi_panel_packet(&io, 0x39, table, 7));
    CHECK(m.fifo() == std::vector<uint32_t>({0xc0390007, 0xd4d4d480, 0xff2222d4, 0}) && m.length() == 12);
    uint8_t big[16];
    for (unsigned i = 0; i < 16; i++) big[i] = (uint8_t)(i + 1);
    m.writes.clear();
    CHECK(!dsi_panel_packet(&io, 0x39, big, 16));
    CHECK(m.fifo() == std::vector<uint32_t>({0xc0390010, 0x04030201, 0x08070605, 0x0c0b0a09, 0x100f0e0d, 0}) && m.length() == 20);
    m.writes.clear();
    CHECK(!dsi_panel_packet(&io, 0x39, big, 1) && m.fifo() == std::vector<uint32_t>({0xc0390001, 0xffffff01}) && m.length() == 8);
    // The same register traffic as dsi_panel_command for its three commands.
    for (unsigned k = 0; k < 3; k++) {
        MMIO a, b;
        a.boot(); b.boot();
        auto ia = a.io(), ib = b.io();
        const uint8_t command = k == 0 ? 0x28 : k == 1 ? 0x29 : 0x51;
        const uint8_t level[3] = {0x51, 0x05, 0x99};
        CHECK(!dsi_panel_command(&ia, command, k == 2 ? 0x599 : 0));
        CHECK(!(k == 2 ? dsi_panel_packet_mode(&ib, 0x39, level, 3, 0) : dsi_panel_packet_mode(&ib, 0x05, &command, 1, 0)));
        CHECK(a.writes == b.writes && a.waited == b.waited);
    }
    // Refusals before any write: unknown type, wrong lengths, no payload, DSI off or TPG busy (-19), the engine busy for 20 ms (-16).
    m.writes.clear();
    CHECK(dsi_panel_packet(&io, 0x29, &off, 1) == -22 && dsi_panel_packet(&io, 0x05, param, 2) == -22);
    CHECK(dsi_panel_packet(&io, 0x15, param, 1) == -22 && dsi_panel_packet(&io, 0x39, big, 0) == -22);
    CHECK(dsi_panel_packet(&io, 0x39, big, 17) == -22 && dsi_panel_packet(&io, 0x05, nullptr, 1) == -22 && m.writes.empty());
    m.regs[4] = 0;
    CHECK(dsi_panel_packet(&io, 0x05, &off, 1) == -19 && m.writes.empty());
    m.regs[4] = 1; m.regs[0x15c] = 0x30006;
    CHECK(dsi_panel_packet(&io, 0x05, &off, 1) == -19 && m.writes.empty());
    m.regs[0x15c] = 0; m.regs[8] = 2;
    CHECK(dsi_panel_packet(&io, 0x05, &off, 1) == -16 && m.writes.empty() && m.waited >= 20000);
    // A completion that never comes: -110, and the engine keeps its command (no TPG reset, no restore).
    m.regs[8] = 0; m.complete = false;
    CHECK(dsi_panel_packet(&io, 0x39, page, 4) == -110 && m.regs[0x15c] == 0x30006 && m.regs[0x4c] == 8);
    printf("ok  dsi_panel_packet: short 0x05/0x15 and long 0x39 (1-16 bytes) FIFO words, lengths and restore; the register traffic of "
           "dsi_panel_command for 0x28/0x29/0x51; refusals without a write; a timeout leaves the command to the engine\n");
}

static void testTables()
{
    // The off sequence and the wake sequence as the DT gives them; the wake pinned by CRC-32 to the DT's on-command from exit sleep.
    CHECK(pagani_panel_off[0].type == 0x05 && pagani_panel_off[0].payload[0] == 0x28 && pagani_panel_off[0].waitMs == 20);
    CHECK(pagani_panel_off[1].type == 0x05 && pagani_panel_off[1].payload[0] == 0x10 && pagani_panel_off[1].waitMs == 120);
    std::vector<uint8_t> blob;
    unsigned pages = 0, tables = 0;
    for (unsigned i = 0; i < PAGANI_PANEL_WAKE_PACKETS; i++) {
        const pagani_panel_packet &p = pagani_panel_wake[i];
        blob.push_back(p.type); blob.push_back(p.length); blob.push_back(p.waitMs);
        for (unsigned b = 0; b < p.length; b++) blob.push_back(p.payload[b]);
        CHECK(p.length >= 1 && p.length <= 7 && (p.type == 0x05 ? p.length == 1 : p.type == 0x39));
        if (p.type == 0x39 && p.length == 4 && p.payload[0] == 0xff && p.payload[1] == 0x5a && p.payload[2] == 0xa5) pages++;
        if (p.type == 0x39 && p.length == 7) tables++;
        CHECK(i == 0 ? p.waitMs == 120 : p.waitMs == 0);
    }
    CHECK(blob.size() == 509 && crc32(blob.data(), blob.size()) == 0xb8454479u);
    CHECK(pagani_panel_wake[0].payload[0] == 0x11 && pagani_panel_wake[PAGANI_PANEL_WAKE_PACKETS - 1].payload[0] == 0x29);
    CHECK(pages == 3 && tables == 48 && pagani_panel_wake[51].payload[3] == 0x00);   // pages 0x51, 0x52, then page 0
    printf("ok  tables: off 0x28 (20 ms) 0x10 (120 ms); wake 0x11 (120 ms), pages 0x51/0x52 with 48 table writes, page 0, 0x29 "
           "(53 packets, crc32 0xb8454479 of the DT's on-command from exit sleep)\n");
}

// The panel's tick on a virtual clock: one step every 16 ms (or earlier when the step asks for less, as PaganiPanel arms its timer).
struct Bench {
    pagani_panel_power p;
    uint64_t now = 1000000000ull;
    struct Sent { uint64_t at; uint8_t type, length; uint8_t payload[7]; };
    std::vector<Sent> sent;
    int fail = 0;            // fail the send of packet number `failAt` with this
    unsigned failAt = ~0u;
    static int send(void *ctx, uint8_t type, const uint8_t *payload, unsigned length)
    {
        auto &b = *static_cast<Bench *>(ctx);
        if (b.sent.size() == b.failAt) return b.fail;
        Sent s{b.now, type, (uint8_t)length, {}};
        memcpy(s.payload, payload, length);
        b.sent.push_back(s);
        return 0;
    }
    explicit Bench(bool sleep) { pagani_panel_power_init(&p, sleep); }
    // Ticks for `ms` with the display's request `want`; returns the first error.
    int run(unsigned want, unsigned ms)
    {
        const uint64_t end = now + (uint64_t)ms * 1000000;
        while (now < end) {
            uint64_t wait = 0;
            unsigned done = 0;
            const int rc = pagani_panel_power_step(&p, now, want, send, this, &wait, &done);
            if (rc) return rc;
            uint64_t next = 16000000;
            if (wait && wait < next) next = wait;
            now += next;
        }
        return 0;
    }
    uint64_t gap(size_t i) const { return sent[i].at - sent[i - 1].at; }
    uint8_t cmd(size_t i) const { return sent[i].payload[0]; }
};

static void testSequences()
{
    // Off with sleep: 0x28, 20 ms, 0x10, the panel asleep 120 ms later.
    Bench b(true);
    CHECK(!b.run(0, 100) && b.sent.size() == 2 && b.cmd(0) == 0x28 && b.cmd(1) == 0x10 && b.gap(1) >= 20000000);
    CHECK(b.p.state == PAGANI_PANEL_ON && b.p.seq);                      // the 120 ms after 0x10 still run
    CHECK(!b.run(0, 200) && b.p.state == PAGANI_PANEL_ASLEEP && !b.p.seq && b.p.sleeps == 1 && b.sent.size() == 2);
    CHECK(b.p.lastNs >= 140000000 && b.p.lastNs < 160000000);
    const uint64_t offNs = b.p.lastNs;
    // Back: 0x11, 120 ms, the 51 writes and 0x29 in one tick, the brightness refresh asked for.
    const size_t first = b.sent.size();
    CHECK(!b.run(1, 300) && b.p.state == PAGANI_PANEL_ON && b.p.wakes == 1 && b.p.refresh);
    CHECK(b.sent.size() == first + PAGANI_PANEL_WAKE_PACKETS && b.cmd(first) == 0x11);
    CHECK(b.sent[first + 1].at - b.sent[first].at >= 120000000);
    for (size_t i = first + 1; i < b.sent.size(); i++) {
        const pagani_panel_packet &want = pagani_panel_wake[i - first];
        CHECK(b.sent[i].at == b.sent[first + 1].at && b.sent[i].type == want.type && b.sent[i].length == want.length &&
              !memcmp(b.sent[i].payload, want.payload, want.length));
    }
    CHECK(b.cmd(b.sent.size() - 1) == 0x29 && b.p.packets == 2 + PAGANI_PANEL_WAKE_PACKETS);
    // Nothing more while the request holds.
    const size_t settled = b.sent.size();
    CHECK(!b.run(1, 500) && b.sent.size() == settled && !b.p.seq);
    printf("ok  sequences: off with sleep 0x28 >=20 ms 0x10, asleep after >=120 ms (%llu ms in all on 16 ms ticks); back 0x11 >=120 "
           "ms, the 51 writes and 0x29 in one tick (%llu ms), brightness refresh\n", (unsigned long long)(offNs / 1000000),
           (unsigned long long)(b.p.lastNs / 1000000));

    // Without the sleep option: 0x28 and 0x29 alone, as PaganiPanel always sent them; no refresh.
    Bench plain(false);
    CHECK(!plain.run(0, 100) && plain.sent.size() == 1 && plain.cmd(0) == 0x28 && plain.p.state == PAGANI_PANEL_BLANK && plain.p.offs == 1);
    CHECK(!plain.run(1, 100) && plain.sent.size() == 2 && plain.cmd(1) == 0x29 && plain.p.state == PAGANI_PANEL_ON && !plain.p.refresh);
    CHECK(plain.p.ons == 1 && plain.p.sleeps == 0 && plain.p.wakes == 0);

    // Back on between 0x28 and 0x10: no sleep, display on at once after the 20 ms.
    Bench quick(true);
    CHECK(!quick.run(0, 10) && quick.sent.size() == 1 && quick.cmd(0) == 0x28);
    CHECK(!quick.run(1, 200) && quick.sent.size() == 2 && quick.cmd(1) == 0x29 && quick.gap(1) >= 20000000);
    CHECK(quick.p.state == PAGANI_PANEL_ON && quick.p.offs == 1 && quick.p.sleeps == 0 && quick.p.ons == 1 && !quick.p.refresh);

    // Back on while the 120 ms after 0x10 run: the wake waits for them (DCS), then runs whole.
    Bench late(true);
    CHECK(!late.run(0, 40) && late.sent.size() == 2 && late.cmd(1) == 0x10);
    CHECK(!late.run(1, 400) && late.sent.size() == 2 + PAGANI_PANEL_WAKE_PACKETS && late.cmd(2) == 0x11);
    CHECK(late.sent[2].at - late.sent[1].at >= 120000000 && late.p.state == PAGANI_PANEL_ON && late.p.refresh);

    // Off while the wake runs: the wake ends first (0x29 sent), then the off sequence at least 120 ms after 0x11.
    Bench flip(true);
    CHECK(!flip.run(0, 300) && flip.p.state == PAGANI_PANEL_ASLEEP);
    CHECK(!flip.run(1, 40) && flip.sent.size() == 3 && flip.cmd(2) == 0x11);
    CHECK(!flip.run(0, 400));
    const size_t wakeEnd = 2 + PAGANI_PANEL_WAKE_PACKETS;
    CHECK(flip.sent.size() == wakeEnd + 2 && flip.cmd(wakeEnd - 1) == 0x29 && flip.cmd(wakeEnd) == 0x28 && flip.cmd(wakeEnd + 1) == 0x10);
    CHECK(flip.sent[wakeEnd].at - flip.sent[2].at >= 120000000 && flip.p.state == PAGANI_PANEL_ASLEEP && flip.p.wakes == 1);

    // A failed send stops there with its error; the sequence stays where it failed (PaganiPanel disables further requests).
    Bench broken(true);
    broken.failAt = 1; broken.fail = -110;
    CHECK(!broken.run(0, 10) && broken.sent.size() == 1);
    CHECK(broken.run(0, 100) == -110 && broken.sent.size() == 1 && broken.p.seq == pagani_panel_off && broken.p.next == 1);
    CHECK(broken.p.state == PAGANI_PANEL_ON && broken.p.sleeps == 0);
    Bench wakeFail(true);
    CHECK(!wakeFail.run(0, 300));
    wakeFail.failAt = 5; wakeFail.fail = -16;
    CHECK(wakeFail.run(1, 300) == -16 && wakeFail.sent.size() == 5 && wakeFail.p.state == PAGANI_PANEL_ASLEEP && !wakeFail.p.refresh);
    printf("ok  requests that change: back on before 0x10 skips the sleep, on during the sleep wait waits >=120 ms, off during the wake "
           "ends it first; without sleep 0x28/0x29 only; a failed send stops where it failed\n");
}

// The whole off and wake through dsi_panel_packet and the register model: every packet goes out as one FIFO command.
static int sendDsi(void *ctx, uint8_t type, const uint8_t *payload, unsigned length)
{
    auto io = static_cast<MMIO *>(ctx)->io();
    return dsi_panel_packet(&io, type, payload, length);
}

static void testThroughDsi()
{
    MMIO m;
    m.boot();
    pagani_panel_power p;
    pagani_panel_power_init(&p, 1);
    uint64_t now = 5000000000ull, wait = 0;
    unsigned done = 0, triggers = 0;
    for (unsigned t = 0; t < 30; t++, now += 16000000) CHECK(!pagani_panel_power_step(&p, now, 0, sendDsi, &m, &wait, &done));
    for (unsigned t = 0; t < 30; t++, now += 16000000) CHECK(!pagani_panel_power_step(&p, now, 1, sendDsi, &m, &wait, &done));
    for (auto &w : m.writes) triggers += w.first == 0x90;
    CHECK(triggers == 2 + PAGANI_PANEL_WAKE_PACKETS && p.state == PAGANI_PANEL_ON && p.refresh);
    const std::vector<uint32_t> fifo = m.fifo();
    CHECK(fifo[0] == 0x80050028 && fifo[2] == 0x80050010 && fifo[4] == 0x80050011 && fifo[6] == 0xc0390004 && fifo[7] == 0x51a55aff);
    CHECK(fifo[fifo.size() - 2] == 0x80050029 && m.regs[0x3c] == 0xa5 && m.regs[0x390] == 2 && m.regs[0x4c] == 32 && !m.regs[0x15c]);
    // The engine stops answering in the middle of the wake: -110 from that packet on, the engine left with it.
    MMIO n;
    n.boot();
    pagani_panel_power_init(&p, 1);
    for (unsigned t = 0; t < 30; t++, now += 16000000) CHECK(!pagani_panel_power_step(&p, now, 0, sendDsi, &n, &wait, &done));
    CHECK(!pagani_panel_power_step(&p, now, 1, sendDsi, &n, &wait, &done) && p.next == 1);   // 0x11 sent, 120 ms to wait
    n.complete = false;
    now += 200000000;
    CHECK(pagani_panel_power_step(&p, now, 1, sendDsi, &n, &wait, &done) == -110 && p.next == 1 && n.regs[0x15c] == 0x30006);
    printf("ok  through dsi_panel_packet: off and wake as %u FIFO commands with the registers restored; an engine that stops "
           "answering fails the packet with -110 and keeps it\n", triggers);
}

int main()
{
    testPacketWriter();
    testTables();
    testSequences();
    testThroughDsi();
    printf("PASS: PaganiPanel power sequences (pagani-screenoff bit 2) and dsi_panel_packet, %u checks\n", checks);
    return 0;
}
