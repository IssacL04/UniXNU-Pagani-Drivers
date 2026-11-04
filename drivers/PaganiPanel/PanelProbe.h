// SPDX-License-Identifier: BSD-2-Clause-Patent
// Why DSI0 reads as zero on the 13T while the screen updates: PaganiPanel's read-only register probe (boot-arg pagani-panel bits 7 and 10)
// and its one-bit clock writes (bit 11, A0; bit 9, A1), free of kernel headers so Test/panel-probe.cpp runs them against a register model on a virtual clock.
// Bit 8 is what comes of A1 if it works: the same write in `start` (panel_probe_ahb).
//
// P0, P1 and P2 ran on a device once (2026-11-01, stage15-smoke-1); P3, A0 and A1 never did. What a block not clocked does on this SoC is known only in
// part: DSI0 reads 0 and does not hang, with DISP_CC_MDSS_AHB_CLK off as that run saw it; the old full DPU snapshot of 2026-10-30 12:15 hung the phone, in
// the same state as far as anyone knows; and a read that hangs takes the log's last lines with it. So:
//  - every block is read from its own window, mapped read only (PaganiPanel.cpp); what writes is one page of the clock controller for A1, one of GCC for A0,
//    and panel_probe_enable, the header's only write, sets an enable bit and nothing else;
//  - the first read of a block comes after a line naming it and a sleep the UFS log's 3 s snapshot needs to take that line away
//    (panel_probe_enter), and each block's values go out as soon as they are read: a hang leaves the block as the last line;
//  - no read comes with a UFS write on the wire. A read that hangs ends in a reset nothing sees coming, and a reset on a write is the one way
//    the log could hurt the logical unit around it (UfsLog.h): from panel_probe_enter on, past the sleep and PANEL_PROBE_QUIET_MS for a round
//    under way to end, pagani_panel_probe_quiet (its bit 0; PaganiBattery's USB probe has bit 1) holds the writer as the Power key does, until the next block's line or the end of the stage.
//    A hang keeps it held, and the timed reset's last snapshot or the panic's still goes out; what a held writer has not taken only goes with those,
//    so before the reads of a block that see more of it than the last ones did (P1's phase, P3's second look) the writer gets a round (panel_probe_flush);
//  - a gate is read right before the block it opens, after that sleep: GCC_DISP_AHB_CLK before the clock controller, the controller's own
//    branches and the core GDSC before what they clock. CBCR: bit 31 CLK_OFF, bit 1 hardware gating (CLK_OFF may then read 1 on an idle clock,
//    the 13T read 0x80000003 at GCC 0x71010), bit 0 enable. The core GDSC has POLL_CFG_GDSCR, so its power state is CFG_GDSCR bit 16, not
//    GDSCR bit 31 as for the others (Linux gdsc.c, dispcc-sun.c), and the bit in NativeDpu.h's kGdscPwrOn is not the one to look at here.
//    A register space that reads as zero shows CLK_OFF 0: only the enable bit and a group that does not read all zero count as running. The
//    frame's own clocks, MDP, byte and pixel, are held to that alone: DISP_CC_MISC_CMD can gate them between frames with CBCR bit 1 clear, so their
//    CLK_OFF on a healthy display says nothing (P1 logs how often it read set). AHB and the RSCC's clock carry the register reads and get no such room;
//  - HW_CTRL lets the hardware collapse the core domain, so one reading proves little: P1 samples 64 times across two frames, P3 again right
//    before it reads, and any sample with anything off ends it. So does a gap of more than a frame between two samples: a phase that did not see
//    every part of a frame says nothing about the part it missed. DSI sits in that domain: a sample reads its version only when the same sample
//    saw the GDSC on.
//
// The stages, from the start of the thread at about 1.5 s of uptime (the moment is PANEL_PROBE_*_MS; a block's first read comes
// PANEL_PROBE_CRUMB_MS + PANEL_PROBE_QUIET_MS later). The P stages all after pagani-input-script's run (241 to about 262 s): a read that hangs costs
// the run what comes after it, and those keys are what the run is for. A package with bit 10 runs on a timed reset of 420 s and P3 ends well before it.
//  P0 (bit 7, about 266 s): gcc, TE and dsi, each read 1024 times in a row for their latency (ticks of 19.2 MHz: DSI far above GCC would mean a
//     trap to EL2), then 100 ms of DSI 0x0, 0x4, 0x8 in groups 20 us apart (a read filter may count every refused read: no free-running loop on
//     DSI) with TE, GPIO 86, sampled in every pass. GPIO 86 is not one of qcom,gpios-reserved (36-39, 74).
//  P1 (bit 7, about 286 s): GCC_DISP_AHB_CLK gates it. Then the display clock controller, then the phase: 64 groups 250 us apart.
//  P2 (bit 10, about 306 s): RSCC, wrapper, SCC, CRMC and last the PHY, each a block of its own with its own gate.
//  P3 (bit 10, about 346 s): INTF_1's four registers, frame and tear counts twice 4.5 s apart. Nothing of MDSS top, DSI1, CTL, SSPP or the SMMU:
//     the hung snapshot began with those, and INTF_1 alone tells whether MDSS answers.
// Bit 10 alone proves no clock, so it reads nothing.
//  A1 (bit 9, about 131 s): see panel_probe_a1 below. It comes first and on its own clock: a package that tests it resets at 240 s.
#ifndef PAGANI_PANEL_PROBE_H
#define PAGANI_PANEL_PROBE_H
#include <stdint.h>
#include <PeripheralIO.h>

enum {
    PANEL_PROBE_P0 = 1, PANEL_PROBE_P1 = 2, PANEL_PROBE_P2 = 4, PANEL_PROBE_P3 = 8, PANEL_PROBE_A1 = 16, PANEL_PROBE_AHB = 32,
    PANEL_PROBE_A1_MS = 130000, PANEL_PROBE_P0_MS = 264500, PANEL_PROBE_P1_MS = 284500, PANEL_PROBE_P2_MS = 304500, PANEL_PROBE_P3_MS = 344500,
    PANEL_PROBE_CRUMB_MS = 3500,            // the UFS log takes a new record every 3 s
    PANEL_PROBE_QUIET_MS = 1000,            // the writer looks at the word every 500 ms, a round takes at most some ms
    PANEL_PROBE_LINE = 120,                 // bytes of a line with its terminator
    PANEL_PROBE_LATENCY_READS = 1024,
    PANEL_PROBE_PHASES = 64,
    PANEL_PROBE_TICKS_MS = 19200,           // the counter runs at 19.2 MHz
    PANEL_PROBE_GROUP_TICKS = 384,          // 20 us: P0's DSI groups
    PANEL_PROBE_WINDOW_TICKS = 1920000,     // 100 ms: P0's sampling
    PANEL_PROBE_TE_TICKS = 3840000,         // 200 ms: A1's look at TE alone, 24 periods
    PANEL_PROBE_PHASE_TICKS = 4800,         // 250 us: P1's samples
    PANEL_PROBE_FRAME_US = 8333,            // a frame at the DT's 120 Hz: the most a phase may leave between two samples
    PANEL_PROBE_A0_POLLS = 1000,
    PANEL_PROBE_A1_POLL_TICKS = 19200,      // 1 ms for CLK_OFF to clear
    PANEL_PROBE_A1_TRIES = 3,               // phases the thread may lose the CPU in, before A1 gives up on seeing a whole one
};

enum panel_probe_block {
    PANEL_BLOCK_GCC, PANEL_BLOCK_TE, PANEL_BLOCK_DSI, PANEL_BLOCK_DISP, PANEL_BLOCK_RSC, PANEL_BLOCK_WRAP, PANEL_BLOCK_SCC, PANEL_BLOCK_CRMC,
    PANEL_BLOCK_PHY, PANEL_BLOCK_INTF, PANEL_BLOCK_MDSS, PANEL_BLOCK_AHB, PANEL_PROBE_BLOCKS
};
// One read-only window each, and the stages that read it; the last one is no such window: it is the one page A1 and bit 8 write to, and nothing reads through it.
// The registers below are offsets into these.
static const struct panel_probe_window {uint64_t base; uint32_t size; unsigned stages; const char *name;} panel_probe_windows[PANEL_PROBE_BLOCKS] = {
    {0x00127000, 0x1000, PANEL_PROBE_P0 | PANEL_PROBE_P1 | PANEL_PROBE_P2 | PANEL_PROBE_P3 | PANEL_PROBE_A1 | PANEL_PROBE_AHB, "gcc"},
    {0x0f156000, 0x1000, PANEL_PROBE_P0 | PANEL_PROBE_A1, "te"},                 // TLMM GPIO 86
    {0x0ae94000, 0x1000, PANEL_PROBE_P0 | PANEL_PROBE_P1 | PANEL_PROBE_P3 | PANEL_PROBE_A1 | PANEL_PROBE_AHB, "dsi"},
    {0x0af00000, 0x10000, PANEL_PROBE_P1 | PANEL_PROBE_P2 | PANEL_PROBE_P3 | PANEL_PROBE_A1 | PANEL_PROBE_AHB, "dispcc"},
    {0x0af20000, 0x1000, PANEL_PROBE_P2 | PANEL_PROBE_A1, "rsc"},
    {0x0af30000, 0x1000, PANEL_PROBE_P2 | PANEL_PROBE_A1, "wrap"},
    {0x0af31000, 0x1000, PANEL_PROBE_P2 | PANEL_PROBE_A1, "scc"},
    {0x0af27000, 0x1000, PANEL_PROBE_P2 | PANEL_PROBE_A1, "crmc"},
    {0x0ae95000, 0x1000, PANEL_PROBE_P2 | PANEL_PROBE_A1, "phy"},
    {0x0ae36000, 0x1000, PANEL_PROBE_P3 | PANEL_PROBE_A1, "intf"},
    {0x0ae00000, 0x1000, PANEL_PROBE_A1, "mdss"},               // its first register, last of all
    {0x0af08000, 0x1000, PANEL_PROBE_A1 | PANEL_PROBE_AHB, "ahb"},
};
enum {
    PANEL_GCC_AHB = 0x4, PANEL_GCC_AXI = 0x8,                   // GCC_DISP_AHB_CLK (Linux sets its bit 0 at boot, always), GCC_DISP_HF_AXI_CLK
    PANEL_TE_IO = 0x4,                                          // GPIO_IN_OUT: bit 0 the pin
    PANEL_DISP_MDP = 0x8010, PANEL_DISP_MDP_LUT = 0x8020, PANEL_DISP_VSYNC = 0x8030, PANEL_DISP_AHB = 0x80b0, PANEL_DISP_AHB_RCG = 0x8360, PANEL_DISP_AHB1 = 0xa028,
    PANEL_AHB_CBCR = PANEL_DISP_AHB & 0xfff,                    // the same register in A1's page
    PANEL_DISP_BYTE = 0x8034, PANEL_DISP_BYTE_INTF = 0x8038, PANEL_DISP_PCLK = 0x8004, PANEL_DISP_ESC = 0x8044,
    PANEL_DISP_GDSCR = 0x9000, PANEL_DISP_CFG = 0x9004, PANEL_DISP_INT2 = 0xb000, PANEL_DISP_INT2_CFG = 0xb004, PANEL_DISP_MISC = 0xf000,
    PANEL_DISP_NON_GDSC = 0xc004, PANEL_DISP_RSCC_VSYNC = 0xc008, PANEL_DISP_RSCC = 0xc00c,
    PANEL_INTF_TE = 0x0, PANEL_INTF_FRAME = 0xac, PANEL_INTF_TEAR_INT = 0x298, PANEL_INTF_AUTOREFRESH = 0x2b4,
};

typedef void (*panel_probe_log)(void *context, const char *format, ...) __attribute__((format(printf, 2, 3)));
// What the probe needs of the kernel. io: the windows of the stages asked for; ticks: the 19.2 MHz counter; display_on: the system's display power;
// quiet: 1 holds the UFS log's writer, 0 lets it go.
struct panel_probe_host {
    void *context;
    struct peripheral_io io[PANEL_PROBE_BLOCKS];
    panel_probe_log log;
    void (*sleep_ms)(void *context, unsigned ms);
    uint64_t (*ticks)(void *context);
    unsigned (*display_on)(void *context);
    void (*quiet)(void *context, unsigned on);
};
struct panel_probe {
    const struct panel_probe_host *host;
    uint64_t began;           // ticks: the thread's start
    uint32_t entered;         // blocks whose line went out
    uint8_t dispcc;           // P1 read the clock controller
    uint8_t phase;            // and every one of its samples had everything on
    uint8_t intf;             // P3's first look read something
    uint8_t dsi;              // A1 read DSI in the shape of a working controller
};

static inline int panel_probe_gcc_on(uint32_t v) { return (v & 1) && (!(v >> 31) || (v & 2)); }
// A branch of the clock controller: as above, in a group that read something. A controller that reads as zero is not a controller with its clocks on.
static inline int panel_probe_running(uint32_t v, uint32_t group) { return group && panel_probe_gcc_on(v); }
// One of the frame's clocks: the enable bit in a group that read something. CLK_OFF may be set between frames (the top of this file).
static inline int panel_probe_enabled(uint32_t v, uint32_t group) { return group && (v & 1); }
static inline int panel_probe_gdsc_on(uint32_t cfg, uint32_t group) { return group && (cfg >> 16 & 1); }
// DSI as the boot loader leaves a working controller: version 2.9, DSI_EN and CMD_MODE_EN, the DMA trigger on software.
static inline int panel_probe_dsi_shape(uint32_t version, uint32_t ctrl, uint32_t trigger) { return version >> 16 == 0x2009 && (ctrl & 5) == 5 && (trigger & 0xf) == 4; }

static inline uint32_t panel_probe_read(const struct panel_probe *p, unsigned block, uint32_t reg) { return peripheral_read(&p->host->io[block], reg); }
static inline uint64_t panel_probe_ticks(const struct panel_probe *p) { return p->host->ticks(p->host->context); }
#define PANEL_PROBE_LOG(p, ...) (p)->host->log((p)->host->context, __VA_ARGS__)

// The wire goes quiet: a round under way ends.
static inline void panel_probe_hold(const struct panel_probe *p)
{
    p->host->quiet(p->host->context, 1);
    p->host->sleep_ms(p->host->context, PANEL_PROBE_QUIET_MS);
}
// The time for what was logged to leave the phone, then the wire goes quiet for what comes next. The writer has to be free when this starts.
static inline void panel_probe_settle(const struct panel_probe *p)
{
    p->host->sleep_ms(p->host->context, PANEL_PROBE_CRUMB_MS);
    panel_probe_hold(p);
}
// The line before the first read of a block.
static inline void panel_probe_enter(struct panel_probe *p, const char *stage, unsigned block)
{
    if (p->entered & 1u << block) return;
    p->entered |= 1u << block;
    p->host->quiet(p->host->context, 0);
    PANEL_PROBE_LOG(p, "panel: %s %s next", stage, panel_probe_windows[block].name);
    panel_probe_settle(p);
}
// A block's line once more: what comes next is another thing to it than its first read was (DSI with its clock on).
static inline void panel_probe_again(struct panel_probe *p, const char *stage, unsigned block)
{
    p->entered &= ~(1u << block);
    panel_probe_enter(p, stage, block);
}
// What the stage logged so far, off the phone before it goes on. A gate read before this is old after it.
static inline void panel_probe_flush(const struct panel_probe *p)
{
    p->host->quiet(p->host->context, 0);
    panel_probe_settle(p);
}

// Read after the sleep of the block they open. Nothing of the clock controller without GCC_DISP_AHB_CLK.
struct panel_probe_gates {
    uint32_t gcc, ahb, rscc, cfg;
    uint8_t dispcc, ahb_on, rscc_on, gdsc_on;
};
static inline void panel_probe_read_gates(const struct panel_probe *p, struct panel_probe_gates *g)
{
    g->gcc = panel_probe_read(p, PANEL_BLOCK_GCC, PANEL_GCC_AHB);
    g->ahb = g->rscc = g->cfg = 0;
    g->dispcc = (uint8_t)panel_probe_gcc_on(g->gcc);
    g->ahb_on = g->rscc_on = g->gdsc_on = 0;
    if (!g->dispcc) return;
    g->ahb = panel_probe_read(p, PANEL_BLOCK_DISP, PANEL_DISP_AHB);
    g->rscc = panel_probe_read(p, PANEL_BLOCK_DISP, PANEL_DISP_RSCC);
    g->cfg = panel_probe_read(p, PANEL_BLOCK_DISP, PANEL_DISP_CFG);
    const uint32_t group = g->ahb | g->rscc | g->cfg;
    g->ahb_on = (uint8_t)panel_probe_running(g->ahb, group);
    g->rscc_on = (uint8_t)panel_probe_running(g->rscc, group);
    g->gdsc_on = (uint8_t)panel_probe_gdsc_on(g->cfg, group);
}

static inline uint32_t panel_probe_latency(const struct panel_probe *p, unsigned block, uint32_t reg)
{
    const uint64_t start = panel_probe_ticks(p);
    for (unsigned i = 0; i < PANEL_PROBE_LATENCY_READS; ++i) panel_probe_read(p, block, reg);
    return (uint32_t)(panel_probe_ticks(p) - start);
}

struct panel_probe_samples {
    uint32_t groups, nonzero;         // DSI groups, those not all zero
    uint32_t any[3];                  // each register's values ORed
    uint32_t first, first_us;         // the first value not zero and when it came
    uint32_t reads, high, rises;      // TE: samples, samples high, rising edges
    uint32_t shortest, longest;       // us between rising edges
    uint64_t span;                    // ticks from the first rising edge to the last
};
// `window` ticks on the counter: IODelay blocks on this port (README). A group late for any reason does not make the next ones closer. dsi 0: TE alone.
static inline void panel_probe_sample(const struct panel_probe *p, struct panel_probe_samples *s, uint32_t window, unsigned dsi)
{
    const struct panel_probe_samples zero = {0, 0, {0, 0, 0}, 0, 0, 0, 0, 0, 0, 0, 0};
    *s = zero;
    const uint64_t start = panel_probe_ticks(p);
    uint64_t next = start, rose = 0, first = 0;
    uint32_t last = 1;                // no edge from the first sample
    for (;;) {
        const uint64_t now = panel_probe_ticks(p);
        if (now - start >= window) break;
        const uint32_t level = panel_probe_read(p, PANEL_BLOCK_TE, PANEL_TE_IO) & 1;
        ++s->reads;
        s->high += level;
        if (level && !last) {
            if (s->rises) {
                const uint32_t us = (uint32_t)((now - rose) * 5 / 96);
                if (s->rises == 1 || us < s->shortest) s->shortest = us;
                if (us > s->longest) s->longest = us;
            } else {
                first = now;
            }
            ++s->rises;
            rose = now;
        }
        last = level;
        if (!dsi || now < next) continue;
        const uint32_t v[3] = {panel_probe_read(p, PANEL_BLOCK_DSI, 0x0), panel_probe_read(p, PANEL_BLOCK_DSI, 0x4), panel_probe_read(p, PANEL_BLOCK_DSI, 0x8)};
        const uint32_t seen = v[0] | v[1] | v[2];
        for (unsigned i = 0; i < 3; ++i) s->any[i] |= v[i];
        ++s->groups;
        if (seen) {
            if (!s->nonzero) {
                s->first = v[0] ? v[0] : v[1] ? v[1] : v[2];
                s->first_us = (uint32_t)((now - start) * 5 / 96);
            }
            ++s->nonzero;
        }
        next = now + PANEL_PROBE_GROUP_TICKS;
    }
    s->span = rose - first;
}
// The TE period in ns over all of a sampling, far finer than its shortest and longest (the software tick is set to this: sheng-vsync, NativeDisplayTiming.h).
// A pulse the thread slept through makes no period of two: the count comes from the shortest period seen, which is no longer than the true one, so the division
// rounds down, and only an edge seen 4% early could add a period to 24.
static inline uint32_t panel_probe_te_ns(const struct panel_probe_samples *s)
{
    if (!s->shortest) return 0;
    const uint64_t shortest = (uint64_t)s->shortest * 96 / 5, periods = s->span / shortest;
    return periods ? (uint32_t)(s->span * 625 / 12 / periods) : 0;
}

static inline void panel_probe_p0(struct panel_probe *p)
{
    panel_probe_enter(p, "p0", PANEL_BLOCK_GCC);
    const uint32_t gcc = panel_probe_latency(p, PANEL_BLOCK_GCC, PANEL_GCC_AHB);
    const uint32_t ahb = panel_probe_read(p, PANEL_BLOCK_GCC, PANEL_GCC_AHB), axi = panel_probe_read(p, PANEL_BLOCK_GCC, PANEL_GCC_AXI);
    PANEL_PROBE_LOG(p, "panel: p0 gcc ahb %x axi %x", ahb, axi);
    panel_probe_enter(p, "p0", PANEL_BLOCK_TE);
    const uint32_t te = panel_probe_latency(p, PANEL_BLOCK_TE, PANEL_TE_IO);
    panel_probe_enter(p, "p0", PANEL_BLOCK_DSI);
    const uint32_t dsi = panel_probe_latency(p, PANEL_BLOCK_DSI, 0x0);
    PANEL_PROBE_LOG(p, "panel: p0 lat gcc %u te %u dsi %u", gcc, te, dsi);
    struct panel_probe_samples s;
    panel_probe_sample(p, &s, PANEL_PROBE_WINDOW_TICKS, 1);
    PANEL_PROBE_LOG(p, "panel: p0 dsi n %u nz %u or %x/%x/%x first %x@%u", s.groups, s.nonzero, s.any[0], s.any[1], s.any[2], s.first, s.first_us);
    PANEL_PROBE_LOG(p, "panel: p0 te rise %u per %u-%u us high %u%% mean %u ns", s.rises, s.shortest, s.longest,
                    s.reads ? (uint32_t)((uint64_t)s.high * 100 / s.reads) : 0, panel_probe_te_ns(&s));
}

struct panel_probe_phase {
    uint32_t off_ahb, off_mdp, off_byte, off_pclk, off_gdsc;  // samples with that one not on
    uint32_t stop_mdp, stop_byte, stop_pclk;                  // samples with its CLK_OFF set
    uint32_t dsi_nonzero;             // samples of the DSI version not zero
    uint32_t read_ticks, total_us;    // the reads of all samples together; the whole of it, as the counter saw it
    uint32_t gap_us;                  // the longest between two samples
};
// The clock controller may only be read here once its gate held. The counter again, not IODelay. The next sample counts from this one, as P0's groups
// do: on a fixed grid a thread that lost the CPU would catch up with its samples in a burst, seeing no more of the frame, with the same length in the log.
static inline void panel_probe_sample_phase(const struct panel_probe *p, struct panel_probe_phase *s)
{
    const struct panel_probe_phase zero = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    *s = zero;
    const uint64_t start = panel_probe_ticks(p);
    uint64_t next = start, last = start, gap = 0, reading = 0;
    for (unsigned i = 0; i < PANEL_PROBE_PHASES; ++i) {
        uint64_t now;
        do now = panel_probe_ticks(p); while (now < next);
        if (now - last > gap) gap = now - last;
        last = now;
        next = now + PANEL_PROBE_PHASE_TICKS;
        const uint32_t ahb = panel_probe_read(p, PANEL_BLOCK_DISP, PANEL_DISP_AHB), mdp = panel_probe_read(p, PANEL_BLOCK_DISP, PANEL_DISP_MDP);
        const uint32_t byte = panel_probe_read(p, PANEL_BLOCK_DISP, PANEL_DISP_BYTE), pclk = panel_probe_read(p, PANEL_BLOCK_DISP, PANEL_DISP_PCLK);
        const uint32_t cfg = panel_probe_read(p, PANEL_BLOCK_DISP, PANEL_DISP_CFG), group = ahb | mdp | byte | pclk | cfg;
        const uint32_t version = panel_probe_gdsc_on(cfg, group) ? panel_probe_read(p, PANEL_BLOCK_DSI, 0x0) : 0;
        reading += panel_probe_ticks(p) - now;
        s->off_ahb += !panel_probe_running(ahb, group);
        s->off_mdp += !panel_probe_enabled(mdp, group);
        s->off_byte += !panel_probe_enabled(byte, group);
        s->off_pclk += !panel_probe_enabled(pclk, group);
        s->off_gdsc += !panel_probe_gdsc_on(cfg, group);
        s->stop_mdp += mdp >> 31;
        s->stop_byte += byte >> 31;
        s->stop_pclk += pclk >> 31;
        s->dsi_nonzero += version != 0;
    }
    s->read_ticks = (uint32_t)reading;
    s->total_us = (uint32_t)((panel_probe_ticks(p) - start) * 5 / 96);
    s->gap_us = (uint32_t)(gap * 5 / 96);
}
static inline int panel_probe_phase_on(const struct panel_probe_phase *s)
{
    return !(s->off_ahb | s->off_mdp | s->off_byte | s->off_pclk | s->off_gdsc) && s->gap_us <= PANEL_PROBE_FRAME_US;
}

static inline void panel_probe_p1(struct panel_probe *p)
{
    panel_probe_enter(p, "p1", PANEL_BLOCK_GCC);
    panel_probe_enter(p, "p1", PANEL_BLOCK_DSI);
    panel_probe_enter(p, "p1", PANEL_BLOCK_DISP);
    const uint32_t gcc = panel_probe_read(p, PANEL_BLOCK_GCC, PANEL_GCC_AHB);
    if (!panel_probe_gcc_on(gcc)) {
        PANEL_PROBE_LOG(p, "panel: p1 skipped gcc %x", gcc);
        return;
    }
    p->dispcc = 1;
#define R(reg) panel_probe_read(p, PANEL_BLOCK_DISP, (reg))
    {
        const uint32_t a = R(PANEL_DISP_GDSCR), b = R(PANEL_DISP_CFG), c = R(PANEL_DISP_INT2), d = R(PANEL_DISP_INT2_CFG), e = R(PANEL_DISP_MISC);
        PANEL_PROBE_LOG(p, "panel: p1 gdsc %x/%x int2 %x/%x misc %x", a, b, c, d, e);
    }
    {
        const uint32_t a = R(PANEL_DISP_AHB), b = R(0x8360), c = R(0x8364), d = R(PANEL_DISP_NON_GDSC), e = R(PANEL_DISP_RSCC), f = R(PANEL_DISP_RSCC_VSYNC);
        PANEL_PROBE_LOG(p, "panel: p1 ahb %x rcg %x/%x nongdsc %x rscc %x %x", a, b, c, d, e, f);
    }
    {
        const uint32_t a = R(PANEL_DISP_MDP), b = R(0x8150), c = R(0x8154), d = R(PANEL_DISP_MDP_LUT), e = R(PANEL_DISP_VSYNC), f = R(0x0000), g = R(0x1000);
        PANEL_PROBE_LOG(p, "panel: p1 mdp %x rcg %x/%x lut %x vsync %x pll %x %x", a, b, c, d, e, f, g);
    }
    {
        const uint32_t a = R(PANEL_DISP_BYTE), b = R(PANEL_DISP_BYTE_INTF), c = R(PANEL_DISP_PCLK), d = R(PANEL_DISP_ESC), e = R(0x8180), f = R(0x8108), g = R(0x81b8);
        PANEL_PROBE_LOG(p, "panel: p1 link byte %x intf %x pclk %x esc %x rcg %x %x %x", a, b, c, d, e, f, g);
    }
#undef R
    panel_probe_flush(p);
    const uint32_t again = panel_probe_read(p, PANEL_BLOCK_GCC, PANEL_GCC_AHB);
    if (!panel_probe_gcc_on(again)) {
        PANEL_PROBE_LOG(p, "panel: p1 phase skipped gcc %x", again);
        return;
    }
    struct panel_probe_phase s;
    panel_probe_sample_phase(p, &s);
    PANEL_PROBE_LOG(p, "panel: p1 phase n %u off ahb %u mdp %u byte %u pclk %u gdsc %u dsi nz %u lat %u us %u gap %u", (unsigned)PANEL_PROBE_PHASES, s.off_ahb, s.off_mdp,
                    s.off_byte, s.off_pclk, s.off_gdsc, s.dsi_nonzero, s.read_ticks, s.total_us, s.gap_us);
    PANEL_PROBE_LOG(p, "panel: p1 clk_off mdp %u byte %u pclk %u", s.stop_mdp, s.stop_byte, s.stop_pclk);
    p->phase = (uint8_t)panel_probe_phase_on(&s);
}

static inline void panel_probe_p2(struct panel_probe *p)
{
    if (!p->dispcc) {
        PANEL_PROBE_LOG(p, "panel: p2 skipped no p1");
        return;
    }
    struct panel_probe_gates g;
    panel_probe_enter(p, "p2", PANEL_BLOCK_RSC);
    panel_probe_read_gates(p, &g);
    if (g.rscc_on) {
        const uint32_t power = panel_probe_read(p, PANEL_BLOCK_RSC, 0x2d0);     // bit 12: the core GDSC has collapsed
        PANEL_PROBE_LOG(p, "panel: p2 rsc pwr %x", power);
    } else {
        PANEL_PROBE_LOG(p, "panel: p2 rsc skipped gcc %x rscc %x", g.gcc, g.rscc);
    }
    // The wrapper and the SCC sit in the core domain: Android powers it before it touches either.
    panel_probe_enter(p, "p2", PANEL_BLOCK_WRAP);
    panel_probe_read_gates(p, &g);
    if (g.rscc_on && g.gdsc_on) {
        const uint32_t a = panel_probe_read(p, PANEL_BLOCK_WRAP, 0x0), b = panel_probe_read(p, PANEL_BLOCK_WRAP, 0x24), c = panel_probe_read(p, PANEL_BLOCK_WRAP, 0x40);
        PANEL_PROBE_LOG(p, "panel: p2 wrap %x %x %x", a, b, c);
    } else {
        PANEL_PROBE_LOG(p, "panel: p2 wrap skipped gcc %x rscc %x cfg %x", g.gcc, g.rscc, g.cfg);
    }
    panel_probe_enter(p, "p2", PANEL_BLOCK_SCC);
    panel_probe_read_gates(p, &g);
    if (g.rscc_on && g.gdsc_on) {
        const uint32_t a = panel_probe_read(p, PANEL_BLOCK_SCC, 0x0), b = panel_probe_read(p, PANEL_BLOCK_SCC, 0x4), c = panel_probe_read(p, PANEL_BLOCK_SCC, 0x8),
                       d = panel_probe_read(p, PANEL_BLOCK_SCC, 0x10);
        PANEL_PROBE_LOG(p, "panel: p2 scc %x %x %x %x", a, b, c, d);
    } else {
        PANEL_PROBE_LOG(p, "panel: p2 scc skipped gcc %x rscc %x cfg %x", g.gcc, g.rscc, g.cfg);
    }
    // The CRM's clock is dispcc's AHB branch (the device tree's disp_crm); it takes that one and the RSCC's.
    panel_probe_enter(p, "p2", PANEL_BLOCK_CRMC);
    panel_probe_read_gates(p, &g);
    if (g.ahb_on && g.rscc_on) {
        const uint32_t a = panel_probe_read(p, PANEL_BLOCK_CRMC, 0x80c), b = panel_probe_read(p, PANEL_BLOCK_CRMC, 0x818), c = panel_probe_read(p, PANEL_BLOCK_CRMC, 0x840),
                       d = panel_probe_read(p, PANEL_BLOCK_CRMC, 0xa80), e = panel_probe_read(p, PANEL_BLOCK_CRMC, 0xaa8);
        PANEL_PROBE_LOG(p, "panel: p2 crmc %x %x %x %x %x", a, b, c, d, e);
    } else {
        PANEL_PROBE_LOG(p, "panel: p2 crmc skipped gcc %x ahb %x rscc %x", g.gcc, g.ahb, g.rscc);
    }
    panel_probe_enter(p, "p2", PANEL_BLOCK_PHY);
    panel_probe_read_gates(p, &g);
    if (g.ahb_on) {
        const uint32_t a = panel_probe_read(p, PANEL_BLOCK_PHY, 0x0), b = panel_probe_read(p, PANEL_BLOCK_PHY, 0x4), c = panel_probe_read(p, PANEL_BLOCK_PHY, 0x8),
                       d = panel_probe_read(p, PANEL_BLOCK_PHY, 0xc);
        PANEL_PROBE_LOG(p, "panel: p2 phy %x %x %x %x", a, b, c, d);
    } else {
        PANEL_PROBE_LOG(p, "panel: p2 phy skipped gcc %x ahb %x", g.gcc, g.ahb);
    }
}

// What P3 needs right before a read: everything on across the phase, and the display on (P1's phase only tells it may try).
static inline int panel_probe_p3_open(const struct panel_probe *p)
{
    struct panel_probe_gates g;
    struct panel_probe_phase s;
    const unsigned on = p->host->display_on(p->host->context);
    panel_probe_read_gates(p, &g);
    if (!g.dispcc || !g.gdsc_on || !on) {
        PANEL_PROBE_LOG(p, "panel: p3 skipped gcc %x cfg %x display %u", g.gcc, g.cfg, on);
        return 0;
    }
    panel_probe_sample_phase(p, &s);
    if (!panel_probe_phase_on(&s)) {
        PANEL_PROBE_LOG(p, "panel: p3 skipped off ahb %u mdp %u byte %u pclk %u gdsc %u gap %u", s.off_ahb, s.off_mdp, s.off_byte, s.off_pclk, s.off_gdsc, s.gap_us);
        return 0;
    }
    return 1;
}
static inline void panel_probe_p3(struct panel_probe *p)
{
    if (!p->phase) {
        PANEL_PROBE_LOG(p, "panel: p3 skipped no full phase");
        return;
    }
    panel_probe_enter(p, "p3", PANEL_BLOCK_INTF);
    if (!panel_probe_p3_open(p)) return;
    const uint32_t te = panel_probe_read(p, PANEL_BLOCK_INTF, PANEL_INTF_TE), frame = panel_probe_read(p, PANEL_BLOCK_INTF, PANEL_INTF_FRAME),
                   tear = panel_probe_read(p, PANEL_BLOCK_INTF, PANEL_INTF_TEAR_INT);
    PANEL_PROBE_LOG(p, "panel: p3 intf te %x fc %x int %x", te, frame, tear);
    p->intf = (te | frame | tear) != 0;
    panel_probe_flush(p);
    if (!panel_probe_p3_open(p)) return;
    const uint32_t frame2 = panel_probe_read(p, PANEL_BLOCK_INTF, PANEL_INTF_FRAME), tear2 = panel_probe_read(p, PANEL_BLOCK_INTF, PANEL_INTF_TEAR_INT),
                   refresh = panel_probe_read(p, PANEL_BLOCK_INTF, PANEL_INTF_AUTOREFRESH);
    PANEL_PROBE_LOG(p, "panel: p3 intf te %x fc %x->%x int %x->%x ar %x", te, frame, frame2, tear, tear2, refresh);
    PANEL_PROBE_LOG(p, "panel: p3 done");
}

// The one thing this header writes, A0's and A1's: a branch's enable bit, the rest of the CBCR as it read.
static inline void panel_probe_enable(const struct peripheral_io *io, uint32_t reg, uint32_t seen) { peripheral_write(io, reg, seen | 1); }

// A1, pagani-panel bit 9. What P1 found on the 13T: DISP_CC_MDSS_AHB_CLK 0x88000000, enable and hardware gating clear and CLK_OFF set, in all 64 samples, with
// MDP, byte, pixel and the core GDSC on, while DSI read zero in all of P0's 4930 groups. That branch is mdss_mdp's iface_clk and disp_crm's clock in the device
// tree, and nothing in this port ever set it. So the one experiment left that needs a write: set its enable bit, alone, and see what comes back. What the
// write leans on, each read right before it and refused otherwise (panel_probe_a1_refused):
//  - its parent, disp_cc_mdss_ahb_clk_src, runs on XO (RCG CFG 0x100001: source 0, HW_CLK_CTRL), the 19.2 MHz entry of its table, with no PLL (PLL1 is off) and
//    no corner to ask for; ROOT_OFF clear: that root runs already, the non-GDSC branch on it reads CLK_OFF clear. Not CRM's: only the MDP's RCG and PLL0 are,
//    and SCC reads 0xf0, the value it is given disabled, with the RSCC's power status 0 and GDSCR HW_CTL clear: no hardware collapses anything under this write;
//  - its own bit 1 clear: a hardware gated branch is the hardware's. Bit 0 set already: nothing to write, the reads go on;
//  - the core GDSC on, and 64 samples across two frames with the frame's clocks on and the thread on the CPU (PANEL_PROBE_A1_TRIES phases for that).
// In this order, each step's line off the phone before the next one, a hang leaving the last line as its address:
//   TE alone (read only, a block P0 read): the period the software tick wants, sheng-vsync.
//   gcc, dsi, dispcc: the state. `panel: a1 write next`, then 4.5 s, then the state again, the phase, and the write through the only writable window of the
//   thread. A hang here: `a1 write next` is the last line. A refusal or a clock that did not start says why and ends A1: nothing behind that clock is read.
//   DSI, all of panel: start's 14 registers, after `a1 dsi next`: `a1 dsi back`, `zero` or `other`. Then the phase again, DSI's version in every sample.
//   INTF_1 (P3) and PHY, CRMC and the rest (P2), which go on after `zero`: the tables of README tell a controller of its own from all of MDSS by them.
//   The first register of MDSS last, and only after DSI came back and INTF_1 answered: it has no decision of its own, and the hung snapshot began next to it.
// The branch stays on: no write undoes it, the timed reset does. No DSI write comes of this: `start` held the commands at 1.5 s and stays so for the boot.
struct panel_probe_a1_state {
    uint32_t gcc, ahb, ahb1, root, source, gdscr, cfg, mdp;
};
static inline void panel_probe_a1_read(const struct panel_probe *p, struct panel_probe_a1_state *s)
{
    const struct panel_probe_a1_state zero = {0, 0, 0, 0, 0, 0, 0, 0};
    *s = zero;
    s->gcc = panel_probe_read(p, PANEL_BLOCK_GCC, PANEL_GCC_AHB);
    if (!panel_probe_gcc_on(s->gcc)) return;
    s->ahb = panel_probe_read(p, PANEL_BLOCK_DISP, PANEL_DISP_AHB);
    s->ahb1 = panel_probe_read(p, PANEL_BLOCK_DISP, PANEL_DISP_AHB1);
    s->root = panel_probe_read(p, PANEL_BLOCK_DISP, PANEL_DISP_AHB_RCG);
    s->source = panel_probe_read(p, PANEL_BLOCK_DISP, PANEL_DISP_AHB_RCG + 4);
    s->gdscr = panel_probe_read(p, PANEL_BLOCK_DISP, PANEL_DISP_GDSCR);
    s->cfg = panel_probe_read(p, PANEL_BLOCK_DISP, PANEL_DISP_CFG);
    s->mdp = panel_probe_read(p, PANEL_BLOCK_DISP, PANEL_DISP_MDP);
}
// Why the bit stays as it is; 0: it may be set.
static inline const char *panel_probe_a1_refused(const struct panel_probe_a1_state *s, unsigned display)
{
    const uint32_t group = s->ahb | s->ahb1 | s->root | s->source | s->gdscr | s->cfg | s->mdp;
    if (!panel_probe_gcc_on(s->gcc)) return "gcc";
    if (!group) return "zero";
    if (s->ahb & 2) return "hw gated";
    if (s->root >> 31) return "root off";
    if (s->source >> 8 & 7) return "not xo";
    if (!panel_probe_gdsc_on(s->cfg, group)) return "gdsc";
    if (!display) return "display";
    return 0;
}
static inline void panel_probe_a1_held(const struct panel_probe *p, const char *why, const struct panel_probe_a1_state *s, unsigned display)
{
    PANEL_PROBE_LOG(p, "panel: a1 held %s gcc %x ahb %x rcg %x/%x cfg %x display %u", why, s->gcc, s->ahb, s->root, s->source, s->cfg, display);
}
// The frame's clocks and the GDSC, with AHB off as it should be by now.
static inline int panel_probe_a1_phase_on(const struct panel_probe_phase *s)
{
    return !(s->off_mdp | s->off_byte | s->off_pclk | s->off_gdsc) && s->gap_us <= PANEL_PROBE_FRAME_US;
}
// The write. 1: the branch runs. careful: its line leaves the phone first and the gates are read again after that; bit 8 once a careful boot went well goes on at once.
static inline int panel_probe_a1_write(struct panel_probe *p, unsigned careful)
{
    struct panel_probe_a1_state s;
    panel_probe_a1_read(p, &s);
    unsigned display = p->host->display_on(p->host->context);
    const char *why = panel_probe_a1_refused(&s, display);
    if (why) {
        panel_probe_a1_held(p, why, &s, display);
        return 0;
    }
    PANEL_PROBE_LOG(p, "panel: a1 write next ahb %x", s.ahb);
    if (careful) {
        panel_probe_flush(p);
        panel_probe_a1_read(p, &s);
        display = p->host->display_on(p->host->context);
        why = panel_probe_a1_refused(&s, display);
        if (why) {
            panel_probe_a1_held(p, why, &s, display);
            return 0;
        }
    }
    struct panel_probe_phase before;
    for (unsigned i = 0; i < PANEL_PROBE_A1_TRIES; ++i) {
        panel_probe_sample_phase(p, &before);
        if (before.gap_us <= PANEL_PROBE_FRAME_US) break;
    }
    PANEL_PROBE_LOG(p, "panel: a1 pre off ahb %u mdp %u byte %u pclk %u gdsc %u gap %u", before.off_ahb, before.off_mdp, before.off_byte, before.off_pclk, before.off_gdsc, before.gap_us);
    if (!panel_probe_a1_phase_on(&before)) {
        panel_probe_a1_held(p, "phase", &s, display);
        return 0;
    }
    const uint64_t start = panel_probe_ticks(p);
    panel_probe_enable(&p->host->io[PANEL_BLOCK_AHB], PANEL_AHB_CBCR, s.ahb);
    uint32_t after = panel_probe_read(p, PANEL_BLOCK_DISP, PANEL_DISP_AHB);
    while (after >> 31 && panel_probe_ticks(p) - start < PANEL_PROBE_A1_POLL_TICKS) after = panel_probe_read(p, PANEL_BLOCK_DISP, PANEL_DISP_AHB);
    PANEL_PROBE_LOG(p, "panel: a1 ahb %x->%x us %u", s.ahb, after, (uint32_t)((panel_probe_ticks(p) - start) * 5 / 96));
    if (!(after & 1) || after >> 31) {
        panel_probe_a1_read(p, &s);
        PANEL_PROBE_LOG(p, "panel: a1 failed %s ahb %x rcg %x/%x gdsc %x/%x", after & 1 ? "stuck" : "ignored", s.ahb, s.root, s.source, s.gdscr, s.cfg);
        return 0;
    }
    return 1;
}
// 1: what the clock opens may be read.
static inline int panel_probe_a1(struct panel_probe *p)
{
    panel_probe_enter(p, "a1", PANEL_BLOCK_TE);
    struct panel_probe_samples te;
    panel_probe_sample(p, &te, PANEL_PROBE_TE_TICKS, 0);
    PANEL_PROBE_LOG(p, "panel: a1 te rise %u per %u-%u us high %u%% mean %u ns", te.rises, te.shortest, te.longest,
                    te.reads ? (uint32_t)((uint64_t)te.high * 100 / te.reads) : 0, panel_probe_te_ns(&te));
    panel_probe_enter(p, "a1", PANEL_BLOCK_GCC);
    panel_probe_enter(p, "a1", PANEL_BLOCK_DSI);
    panel_probe_enter(p, "a1", PANEL_BLOCK_DISP);
    struct panel_probe_a1_state s;
    panel_probe_a1_read(p, &s);
    PANEL_PROBE_LOG(p, "panel: a1 state gcc %x ahb %x ahb1 %x rcg %x/%x gdsc %x/%x mdp %x", s.gcc, s.ahb, s.ahb1, s.root, s.source, s.gdscr, s.cfg, s.mdp);
    if (s.ahb & 1) PANEL_PROBE_LOG(p, "panel: a1 ahb %x already on", s.ahb);
    else if (!panel_probe_a1_write(p, 1)) return 0;
    p->dispcc = 1;
    panel_probe_again(p, "a1", PANEL_BLOCK_DSI);
    struct panel_probe_gates g;
    panel_probe_read_gates(p, &g);
    if (!g.ahb_on || !g.gdsc_on) {
        PANEL_PROBE_LOG(p, "panel: a1 dsi skipped gcc %x ahb %x cfg %x", g.gcc, g.ahb, g.cfg);
        return 0;
    }
    struct dsi_panel_state d;
    dsi_panel_snapshot(&p->host->io[PANEL_BLOCK_DSI], &d);
    PANEL_PROBE_LOG(p, "panel: a1 dsi ver %x ctrl %x st %x fifo %x dma %x trig %x", d.version, d.ctrl, d.status, d.fifo, d.dma, d.trigger);
    PANEL_PROBE_LOG(p, "panel: a1 lanes %x int %x clk %x tpg %x ack %x to %x sec %x split %x", d.lanes, d.ints, d.clocks, d.tpg, d.ack, d.timeout, d.secure, d.split);
    p->dsi = (uint8_t)panel_probe_dsi_shape(d.version, d.ctrl, d.trigger);
    PANEL_PROBE_LOG(p, "panel: a1 dsi %s", p->dsi ? "back" : d.version ? "other" : "zero");
    panel_probe_flush(p);
    const uint32_t gcc = panel_probe_read(p, PANEL_BLOCK_GCC, PANEL_GCC_AHB);
    if (!panel_probe_gcc_on(gcc)) {
        PANEL_PROBE_LOG(p, "panel: a1 phase skipped gcc %x", gcc);
        return 0;
    }
    struct panel_probe_phase after;
    panel_probe_sample_phase(p, &after);
    PANEL_PROBE_LOG(p, "panel: a1 phase n %u off ahb %u mdp %u byte %u pclk %u gdsc %u dsi nz %u lat %u us %u gap %u", (unsigned)PANEL_PROBE_PHASES, after.off_ahb, after.off_mdp,
                    after.off_byte, after.off_pclk, after.off_gdsc, after.dsi_nonzero, after.read_ticks, after.total_us, after.gap_us);
    p->phase = (uint8_t)panel_probe_phase_on(&after);
    return 1;
}
static inline void panel_probe_a1_mdss(struct panel_probe *p)
{
    if (!p->dsi || !p->intf) {
        PANEL_PROBE_LOG(p, "panel: a1 mdss skipped dsi %u intf %u", p->dsi, p->intf);
        return;
    }
    panel_probe_enter(p, "a1", PANEL_BLOCK_MDSS);
    if (!panel_probe_p3_open(p)) return;
    const uint32_t version = panel_probe_read(p, PANEL_BLOCK_MDSS, 0x0);
    PANEL_PROBE_LOG(p, "panel: a1 mdss ver %x", version);
}

// The frames on the link as the controller shows them: DSI_STATUS bit 2 (PeripheralIO.h: the frame on its way), PANEL_PROBE_PHASES samples PANEL_PROBE_PHASE_TICKS apart, two frames
// at 120 Hz, each counted from the one before as in panel_probe_sample_phase. What this bit does on the 13T no run has seen yet: a boot on which it shows nothing proves nothing,
// so only frames seen before a command and gone after it count against that command (pagani_panel_ahb_after). The frames that began in those samples.
static inline uint32_t panel_probe_frames(const struct panel_probe *p)
{
    uint32_t rises = 0, last = 1;     // no edge from the first sample
    uint64_t next = panel_probe_ticks(p);
    for (unsigned i = 0; i < PANEL_PROBE_PHASES; ++i) {
        uint64_t now;
        do now = panel_probe_ticks(p); while (now < next);
        next = now + PANEL_PROBE_PHASE_TICKS;
        const uint32_t level = panel_probe_read(p, PANEL_BLOCK_DSI, 0x8) >> 2 & 1;
        rises += level && !last;
        last = level;
    }
    return rises;
}

// The formal path, pagani-panel bit 8: what A1 proved at 154 s, in `start`, before its first read of DSI and before anything of the system looks. The branch is set by A1's own gates and
// write, so a boot that reads otherwise than A1's boot did stays as it was. What differs is the moment: about 1.5 s, on the thread that goes on to the GPU and the root mount (Stage7Probe),
// so a sleep here moves all of that, and nothing else of the display runs yet. 1: the branch runs with the core GDSC on. The writer stays held, also on 0: `start` reads DSI next and the
// first DCS follows; pagani_panel_ahb_after lets it go, on every way out. A hang never gets there, as it should not. The bit stays set on the way out too, as after A1: nothing undoes it.
// careful (bit 8 alone): this moment was never run, so every step's line leaves the phone before the step, 4.5 s each, and a hang leaves that line last. Not careful (with bit 12, for a
// package, once a careful boot went well): one hold of 1 s for all of it, in which the writer's one round at the start of a hold takes the log up to `panel: ahb next` off the phone
// (UfsLog.h), and the boot goes on 1 s later instead of about 18 s.
static inline int panel_probe_ahb(struct panel_probe *p, unsigned careful)
{
    if (careful) {
        panel_probe_enter(p, "ahb", PANEL_BLOCK_DISP);
    } else {
        PANEL_PROBE_LOG(p, "panel: ahb next");
        panel_probe_hold(p);
    }
    struct panel_probe_a1_state s;
    panel_probe_a1_read(p, &s);
    PANEL_PROBE_LOG(p, "panel: ahb state gcc %x ahb %x ahb1 %x rcg %x/%x gdsc %x/%x mdp %x", s.gcc, s.ahb, s.ahb1, s.root, s.source, s.gdscr, s.cfg, s.mdp);
    if (s.ahb & 1) PANEL_PROBE_LOG(p, "panel: ahb %x already on", s.ahb);
    else if (!panel_probe_a1_write(p, careful)) return 0;
    if (careful) panel_probe_again(p, "ahb", PANEL_BLOCK_DSI);
    struct panel_probe_gates g;
    panel_probe_read_gates(p, &g);
    if (g.ahb_on && g.gdsc_on) return 1;
    PANEL_PROBE_LOG(p, "panel: ahb dsi skipped gcc %x ahb %x cfg %x", g.gcc, g.ahb, g.cfg);
    return 0;
}

static inline void panel_probe_wait(const struct panel_probe *p, unsigned ms)
{
    const uint64_t at = p->began + (uint64_t)ms * PANEL_PROBE_TICKS_MS, now = panel_probe_ticks(p);
    if (now < at) p->host->sleep_ms(p->host->context, (unsigned)((at - now + PANEL_PROBE_TICKS_MS - 1) / PANEL_PROBE_TICKS_MS));
}
// When the thread has something to say at the earliest: a line that waits for that moment is out of the events ring's way, and before a timed reset of 240 s.
static inline unsigned panel_probe_first_ms(unsigned stages) { return stages & PANEL_PROBE_A1 ? PANEL_PROBE_A1_MS : PANEL_PROBE_P0_MS; }

// The thread's whole work: stages, PANEL_PROBE_P*. Each runs at its moment; one that ran late takes the next one's sleep from it.
static inline void panel_probe_run(const struct panel_probe_host *host, unsigned stages)
{
    struct panel_probe p = {host, 0, 0, 0, 0, 0, 0};
    p.began = panel_probe_ticks(&p);
    PANEL_PROBE_LOG(&p, "panel: probe stages %x", stages);
    if (stages & PANEL_PROBE_A1) {
        panel_probe_wait(&p, PANEL_PROBE_A1_MS);
        if (panel_probe_a1(&p)) {
            panel_probe_p3(&p);
            panel_probe_p2(&p);
            panel_probe_a1_mdss(&p);
        }
        host->quiet(host->context, 0);
        p.entered = 0;                // the stages to come have their lines and their held writer of their own
    }
    if (stages & PANEL_PROBE_P0) {
        panel_probe_wait(&p, PANEL_PROBE_P0_MS);
        panel_probe_p0(&p);
        host->quiet(host->context, 0);
    }
    if (stages & PANEL_PROBE_P1) {
        panel_probe_wait(&p, PANEL_PROBE_P1_MS);
        panel_probe_p1(&p);
        host->quiet(host->context, 0);
    }
    if (stages & PANEL_PROBE_P2) {
        panel_probe_wait(&p, PANEL_PROBE_P2_MS);
        panel_probe_p2(&p);
        host->quiet(host->context, 0);
    }
    if (stages & PANEL_PROBE_P3) {
        panel_probe_wait(&p, PANEL_PROBE_P3_MS);
        panel_probe_p3(&p);
        host->quiet(host->context, 0);
    }
    PANEL_PROBE_LOG(&p, "panel: probe end");
}

// A0, pagani-panel bit 11: the cheapest fix there is, tried only when asked. GCC_DISP_AHB_CLK is dispcc's interface clock; Linux sets its bit 0 at
// boot, always, and nothing here ever did. gcc: a window at panel_probe_windows[PANEL_BLOCK_GCC], the one writable mapping of all this; it gets at
// most one write, that bit, and only when the bit reads clear. Comes before start's first read of DSI (start then reads it again for its own log):
// the next line of the log tells a block that came back from one that did not.
static inline void panel_probe_a0(const struct peripheral_io *gcc, const struct peripheral_io *dsi, panel_probe_log log, void *context)
{
    const uint32_t before = peripheral_read(gcc, PANEL_GCC_AHB);
    if (before & 1) {
        log(context, "panel: a0 gcc %x already on", before);
        return;
    }
    log(context, "panel: a0 write next gcc %x", before);
    panel_probe_enable(gcc, PANEL_GCC_AHB, before);
    uint32_t after = peripheral_read(gcc, PANEL_GCC_AHB);
    for (unsigned n = 0; n < PANEL_PROBE_A0_POLLS && after >> 31; ++n) {
        gcc->delay(gcc->context, 1);
        after = peripheral_read(gcc, PANEL_GCC_AHB);
    }
    log(context, "panel: a0 first DSI read next gcc %x", after);
    const uint32_t version = peripheral_read(dsi, 0x0);
    log(context, "panel: a0 gcc %x->%x ver %x", before, after, version);
}
#endif
