// SPDX-License-Identifier: BSD-2-Clause-Patent
// The brightness side of PaganiPanel and its timer's whole tick, free of kernel headers so Test/panel-brightness.cpp runs them on a virtual clock.
//
// The panel (PKX110 live device tree, qcom,mdss_dsi_panel_AA594_P_7_A0020_dsc_cmd; Panel_AA594_P_7_A0020_dsc_cmd.xml): bl_ctrl_dcs, a 12-bit level
// in DCS 0x51, high byte first (qcom,mdss-dsi-bl-inverted-dbv, qcom,mdss-bl-high2bit, and its own tables: hbm-off 51 0d bb, hbm-on 51 0e e0,
// exit-max 51 0f fe). 1 is the least light (qcom,mdss-dsi-bl-min-level), 0xdbb the normal maximum (oplus,dsi-bl-normal-max-level), 0xffe the
// hardware's (above the normal maximum only with the HBM thermal rules, which are not here), 0x599 the boot level (35%), 0 dark.
// IODisplayParameters publishes 0..normal maximum, so CoreBrightness's level is the panel's own value, in a straight line from the slider
// (level = ceil(x * factor * max), slider floor (min + 1) / max, so its lowest level is the panel's 1 and 0 only comes with the display fading off).
//
// CoreBrightness (backboardd) has no new-backlight-architecture property here, so it takes its legacy path: it reads min, max and value from
// IODisplayParameters brightness when the display opens and then sets {brightness, FadePeriod (16.16 seconds), backlight-level} with
// IORegistryEntrySetCFProperties from its brightness thread, which sleeps for the fade period afterwards: the fade itself is the driver's.
// setProperties only packs the newest request into one word and arms the timer (a request replaces one not yet taken, and the caller never
// waits for the work loop: sheng int-277); pagani_panel_tick takes it and steps from the level shown to the goal.
//
// What PaganiPanel.cpp decides lives here too, so the host test runs the code the kernel runs: the first DCS (pagani_panel_first_dcs), what each
// bit of boot-arg pagani-panel changes, the packet mode and when a report is due.
#ifndef PAGANI_PANEL_BRIGHTNESS_H
#define PAGANI_PANEL_BRIGHTNESS_H
#include <stdint.h>
#include <PeripheralIO.h>
#include "PanelPower.h"
#include "PanelProbe.h"

// boot-arg pagani-panel, one number: bit 0 runs this driver (Stage7Probe looks at it). The others switch its behaviour off or, for a test, further on.
enum {
    PAGANI_PANEL_OPTION_PUBLISH_ALWAYS = 2,     // publish the backlight even when the first DCS failed, to test CoreBrightness's side alone
    PAGANI_PANEL_OPTION_NO_FORCE = 4,           // no forced DCS when the checks refused the first one
    PAGANI_PANEL_OPTION_LOW_POWER = 8,          // brightness and display on/off in low power instead of high speed
    PAGANI_PANEL_OPTION_NO_BACKLIGHT = 16,      // never publish the backlight (no backlight-control, so nothing changes for CoreBrightness and
                                                // CADeviceHasBacklight stays false: the way out of a render server that hangs on it, sheng int-236);
                                                // the DCS path, the display power and the reports stay
    PAGANI_PANEL_OPTION_NO_FADE = 32,           // ignore FadePeriod: the goal at once
    PAGANI_PANEL_OPTION_NO_DISPLAY_POWER = 64,  // never send 0x28/0x29, whatever the system asks: the screen goes dark on the display path's black
                                                // frames alone, as it did while no DCS got through, and the brightness stays in use. The
                                                // way out of a panel that stays dark after a real display off; pagani_screen_state still follows the
                                                // system's request, as pagani-input-script's wake and the keyboard's hooks expect
    PAGANI_PANEL_OPTION_PROBE = 128,            // the register probe's reads of low risk: P0 and P1 (PanelProbe.h)
    PAGANI_PANEL_OPTION_AHB_START = 256,        // the formal path, for after A1: start sets DISP_CC_MDSS_AHB_CLK's enable bit, behind A1's gates, before its first read of DSI,
                                                // and sends one DCS, the boot level again, with the log's line off the phone before each step. Then reads only: nothing is
                                                // published, no other command goes. Only in a first test with a timed reset. With bit 12 the slider (pagani_panel_ahb_after)
    PAGANI_PANEL_OPTION_AHB = 512,              // A1: at about 131 s set DISP_CC_MDSS_AHB_CLK's enable bit, behind its gates, and read what it clocks (PanelProbe.h).
                                                // Comes well before a timed reset of 240 s. Only in a first test with such a reset. Sends no DCS: start held them
    PAGANI_PANEL_OPTION_PROBE_DEEP = 1024,      // its reads of medium risk: P2 and P3, after P1's gates. Only in a first test with a timed reset
    PAGANI_PANEL_OPTION_GCC_AHB = 2048,         // A0: set GCC_DISP_AHB_CLK's enable bit if it is clear, before the first read of DSI (PanelProbe.h).
                                                // Such a boot only reads: no DCS, whatever the registers show, without bit 12
    PAGANI_PANEL_OPTION_A0_DCS = 4096,          // with bit 11: the commands may go, through pagani_panel_dcs_gate. With bit 8 as well: they stay open after the one DCS, the backlight
                                                // is published and the boot takes one hold of 1 s for all of bit 8's steps, for a package, once bit 8 alone went well on a device
};

enum {
    PAGANI_PANEL_DBV_MAX = 0xffe,       // qcom,mdss-dsi-bl-max-level
    PAGANI_PANEL_DBV_NORMAL = 0xdbb,    // oplus,dsi-bl-normal-max-level: the board's PAGANI_BRIGHTNESS_MAX stays within this (static_assert in PaganiPanel.cpp)
    PAGANI_BRIGHTNESS_STEP_MS = 16,     // one DCS per step at most; a step without a change of level sends nothing
    PAGANI_BRIGHTNESS_FADE_MAX_MS = 5000,
    PAGANI_PANEL_RETRY_MS = 100,        // a command not sent (-19, -16: nothing was written) goes again this much later,
    PAGANI_PANEL_RETRIES = 600,         // at most this many times in a row, unless the panel stays dark without it (pagani_panel_tick)
    PAGANI_PANEL_REPORT_QUIET = 10,     // reports in a row that may pass without a line
};

// The request word: bit 63 set, the level in bits 16..31 (clamped to max), the fade in milliseconds in bits 0..15. CoreBrightness's level is
// an int, and an OSNumber of 32 bits gives it back as such: a negative one is the dark end, not the bright one (sheng clamps it the same way).
static inline uint64_t pagani_brightness_request(uint64_t level, uint32_t fade_16_16, uint32_t max)
{
    uint64_t milliseconds = ((uint64_t)fade_16_16 * 1000) >> 16;
    if (milliseconds > PAGANI_BRIGHTNESS_FADE_MAX_MS) milliseconds = PAGANI_BRIGHTNESS_FADE_MAX_MS;
    if (level >> 31 == 1) level = 0;
    if (level > max) level = max;
    return 1ull << 63 | level << 16 | milliseconds;
}
static inline int pagani_brightness_request_valid(uint64_t word) { return (int)(word >> 63); }
static inline uint32_t pagani_brightness_request_level(uint64_t word) { return (uint32_t)(word >> 16) & 0xffff; }
static inline uint32_t pagani_brightness_request_fade(uint64_t word) { return (uint32_t)word & 0xffff; }

struct pagani_brightness {
    uint32_t shown;           // the last level the panel took
    uint32_t from, to;        // the running fade: from the level shown when it began to the goal
    uint64_t start, period;   // ns; period 0: the goal at once
};
static inline void pagani_brightness_init(struct pagani_brightness *b, uint32_t level)
{
    struct pagani_brightness z = {level, level, level, 0, 0};
    *b = z;
}
// A request taken at `now`: a fade from what the panel shows now, so a newer goal bends a running fade instead of restarting it.
static inline void pagani_brightness_begin(struct pagani_brightness *b, uint32_t goal, uint32_t milliseconds, uint64_t now)
{
    b->from = b->shown;
    b->to = goal;
    b->start = now;
    b->period = (uint64_t)milliseconds * 1000000u;
}
// The level the fade asks for at `now`.
static inline uint32_t pagani_brightness_level(const struct pagani_brightness *b, uint64_t now)
{
    const uint64_t elapsed = now - b->start;
    if (!b->period || elapsed >= b->period) return b->to;
    const uint64_t span = b->to > b->from ? b->to - b->from : b->from - b->to;
    const uint32_t moved = (uint32_t)(span * elapsed / b->period);
    return b->to > b->from ? b->from + moved : b->from - moved;
}
static inline int pagani_brightness_done(const struct pagani_brightness *b) { return b->shown == b->to; }

// The panel as the timer drives it: the power sequences of PanelPower.h (without the sleep option 0x28 and 0x29 alone) and the brightness.
struct pagani_panel_run {
    struct pagani_panel_power power;
    struct pagani_brightness bright;
    uint32_t retries;         // commands in a row that were not sent
    uint32_t retried, writes; // commands sent again, brightness commands sent
    uint8_t shown;            // the panel shows: on, no sequence running
    uint8_t stale;            // it left sleep mode: the level is written again
    uint8_t held;             // no command goes, for the whole boot (pagani_panel_dcs_gate): PAGANI_PANEL_DCS_HELD_*
    uint8_t ahb;              // bit 8: start found DISP_CC_MDSS_AHB_CLK running behind its gates (panel_probe_ahb)
    uint8_t checked;          // bit 8: every command looks at the controller first (pagani_panel_send_packet)
};
struct pagani_panel_tick_result {
    int rc;                   // the command that failed in this tick, 0: none
    uint32_t next_ms;         // the next tick this much later; 0: none until a request or the display asks for one
    uint8_t stopped;          // rc ended the panel's commands: an engine that may still own a command gets no other, the caller stops
    uint8_t publish;          // what the registry shows changed
    uint8_t finished;         // power sequences that ended
    uint8_t woke;             // the wake from sleep mode ended
};
static inline void pagani_panel_run_init(struct pagani_panel_run *r, uint32_t level, int sleep)
{
    pagani_panel_power_init(&r->power, sleep);
    pagani_brightness_init(&r->bright, level);
    r->retries = r->retried = r->writes = 0;
    r->shown = 1;
    r->stale = r->held = r->ahb = r->checked = 0;
}
static inline int pagani_brightness_send(pagani_panel_send send, void *context, uint32_t level)
{
    const uint8_t payload[3] = {0x51, (uint8_t)(level >> 8), (uint8_t)level};
    return send(context, 0x39, payload, 3);
}

// The bits of pagani-panel as the driver applies them.
static inline unsigned pagani_panel_send_mode(unsigned options) { return options & PAGANI_PANEL_OPTION_LOW_POWER ? DSI_PANEL_LP : 0; }
static inline unsigned pagani_panel_want(unsigned options, unsigned requested) { return options & PAGANI_PANEL_OPTION_NO_DISPLAY_POWER ? 1 : requested; }
static inline uint32_t pagani_panel_fade(unsigned options, uint32_t fade_16_16) { return options & PAGANI_PANEL_OPTION_NO_FADE ? 0 : fade_16_16; }
static inline int pagani_panel_backlight(unsigned options) { return !(options & PAGANI_PANEL_OPTION_NO_BACKLIGHT); }
static inline int pagani_panel_gcc_ahb(unsigned options) { return (options & PAGANI_PANEL_OPTION_GCC_AHB) != 0; }
static inline int pagani_panel_ahb_start(unsigned options) { return (options & PAGANI_PANEL_OPTION_AHB_START) != 0; }
static inline int pagani_panel_ahb_slider(unsigned options) { return pagani_panel_ahb_start(options) && (options & PAGANI_PANEL_OPTION_A0_DCS); }
// Bit 8 alone: a moment no run has seen, so each step's line goes first.
static inline int pagani_panel_ahb_careful(unsigned options) { return pagani_panel_ahb_start(options) && !(options & PAGANI_PANEL_OPTION_A0_DCS); }
// With bit 8 only a first DCS that went well and read back well publishes, and only with bit 12: bit 1 is no way past that, a command that failed stays failed.
static inline int pagani_panel_publishes(unsigned options, int first_dcs)
{
    if (pagani_panel_ahb_start(options)) return !first_dcs && pagani_panel_ahb_slider(options);
    return !first_dcs || (options & PAGANI_PANEL_OPTION_PUBLISH_ALWAYS);
}
static inline unsigned pagani_panel_probe_stages(unsigned options)
{
    return (options & PAGANI_PANEL_OPTION_PROBE ? PANEL_PROBE_P0 | PANEL_PROBE_P1 : 0u) | (options & PAGANI_PANEL_OPTION_PROBE_DEEP ? PANEL_PROBE_P2 | PANEL_PROBE_P3 : 0u) |
           (options & PAGANI_PANEL_OPTION_AHB ? PANEL_PROBE_A1 : 0u);
}
// A boot that tests the controller's state: its commands go through pagani_panel_dcs_gate. Bit 12 alone is not one; it only widens bit 11.
static inline int pagani_panel_guarded(unsigned options) { return pagani_panel_probe_stages(options) || pagani_panel_gcc_ahb(options) || pagani_panel_ahb_start(options); }
// One packet's mode, from the driver's own (send_mode, and DSI_PANEL_FORCE when that is what worked): the wake is part of the DT's on sequence
// (low power); the off sequence, display on alone and the brightness are high speed.
static inline unsigned pagani_panel_packet_mode(const struct pagani_panel_run *r, unsigned mode)
{
    return mode | (r->power.seq == pagani_panel_wake ? DSI_PANEL_LP : 0);
}

// The first DCS: the level the boot loader set again, so nothing changes on the panel, to learn whether the command path answers before the
// system is told there is a backlight. Refused by the checks (they read other than on the sibling boards, or the engine stayed busy) it goes
// once more without them, and the commands after it the same way when that worked. `version` is the controller's version register as
// read before: 0 means reads do not reach this block (AHB or power domain), nothing can complete there, and a first write could only make
// that worse, so a block that does not answer gets no forced command. `forcing` (may be null) runs before the forced send, for the log.
static inline int pagani_panel_first_dcs(struct pagani_panel_run *r, unsigned options, uint32_t version, unsigned *mode, pagani_panel_send send,
                                         void (*forcing)(void *), void *context)
{
    int rc = pagani_brightness_send(send, context, r->bright.shown);
    if ((rc == -19 || rc == -16) && !(options & PAGANI_PANEL_OPTION_NO_FORCE) && version) {
        if (forcing) forcing(context);
        *mode |= DSI_PANEL_FORCE;
        rc = pagani_brightness_send(send, context, r->bright.shown);
        if (rc) *mode &= ~DSI_PANEL_FORCE;
    }
    if (!rc) ++r->writes;
    return rc;
}

// Whether a boot may try the first DCS, before anything is written: 0, or why not, which holds that one and every command after it for the whole boot
// (a -19: nothing written, nothing read). Only for pagani_panel_guarded options: the others go on exactly as they did, DSI_PANEL_FORCE on a controller that
// reads otherwise included. A controller that came back is a state this port never saw, and a -110 there leaves the engine as it is with the frames behind
// it (PeripheralIO.h: no restore without DSI_PANEL_FORCE). So it must look as the boot loader left a working one: version 2.9, DSI_EN and CMD_MODE_EN, the
// DMA trigger on software (the send path does not look at that one). Version 0 is not that either: decided once, here, a later command cannot be the
// first write to DSI because a read of ctrl happened to answer, with the send path looking at nothing else. That is what keeps a probe with bit 1 read
// only. After A0 the boot only reads: its point is the registers, and the first write to DSI should not come as a side effect of it working. Bit 12
// lets the commands go, and then the shape above opens the way.
// Bit 8 adds three ways: AHB (the clock is not on, whatever DSI reads), LOST (the first DCS or what it left, or a later look at the controller: below) and ONE (bit 8 alone, after its DCS).
enum { PAGANI_PANEL_DCS_OPEN, PAGANI_PANEL_DCS_HELD_A0, PAGANI_PANEL_DCS_HELD_SHAPE, PAGANI_PANEL_DCS_HELD_AHB, PAGANI_PANEL_DCS_HELD_LOST, PAGANI_PANEL_DCS_HELD_ONE };
static inline const char *pagani_panel_held_name(unsigned held)
{
    static const char *const names[] = {"open", "a0", "shape", "ahb", "lost", "one"};
    return held < sizeof names / sizeof names[0] ? names[held] : "?";
}
static inline unsigned pagani_panel_dcs_gate(unsigned options, uint32_t version, uint32_t ctrl, uint32_t trigger)
{
    if (!pagani_panel_guarded(options)) return PAGANI_PANEL_DCS_OPEN;
    if (pagani_panel_gcc_ahb(options) && !(options & PAGANI_PANEL_OPTION_A0_DCS)) return PAGANI_PANEL_DCS_HELD_A0;
    return panel_probe_dsi_shape(version, ctrl, trigger) ? PAGANI_PANEL_DCS_OPEN : PAGANI_PANEL_DCS_HELD_SHAPE;
}
// The first DCS as start runs it, with the registers it just read, and one packet as sendPacket sends it: both here so that the host test runs what the kernel runs.
// Bit 8 forces nothing: a controller that refuses here says something no run has seen, and the way back is to leave it alone. Its first DCS also needs the clock start set, and a
// command after it looks at the controller first: DSI reading as anything but a working controller (the branch off again, the block gone) ends the commands for the boot, with the slider
// left where it is, before anything is written there (the -19 goes the way of pagani_panel_tick).
static inline int pagani_panel_start_dcs(struct pagani_panel_run *r, unsigned options, const struct dsi_panel_state *seen, unsigned *mode, pagani_panel_send send,
                                         void (*forcing)(void *), void *context)
{
    const int formal = pagani_panel_ahb_start(options);
    r->held = (uint8_t)pagani_panel_dcs_gate(options, seen->version, seen->ctrl, seen->trigger);
    if (formal && !r->ahb && !r->held) r->held = PAGANI_PANEL_DCS_HELD_AHB;
    r->checked = (uint8_t)formal;
    return r->held ? -19 : pagani_panel_first_dcs(r, formal ? options | PAGANI_PANEL_OPTION_NO_FORCE : options, seen->version, mode, send, forcing, context);
}
static inline int pagani_panel_send_packet(struct pagani_panel_run *r, const struct peripheral_io *io, unsigned mode, uint8_t type, const uint8_t *payload, unsigned length)
{
    if (r->held) return -19;
    if (r->checked && !panel_probe_dsi_shape(peripheral_read(io, 0x0), peripheral_read(io, 0x4), peripheral_read(io, 0x84))) {
        r->held = PAGANI_PANEL_DCS_HELD_LOST;
        return -19;
    }
    return dsi_panel_packet_mode(io, type, payload, length, pagani_panel_packet_mode(r, mode));
}

// Bit 8, right after its DCS, from the registers as start read them before and as they read now: what is wrong with what the command left, 0: nothing. The shape and the configuration
// are what one command does not change (the send path restores DMA_CTRL and the FIFO's mode). DMA busy means the engine still has it. The error bits count when they are new, as the boot
// loader may have left some: DSI_TIMEOUT_STATUS HS TX, LP RX, BTA; DSI_FIFO_STATUS the MDP and DMA FIFOs' underflows and each lane's; DSI_INT_CTRL's DSI_ERROR (facts: dsi_ctrl_hw_cmn.c).
enum { PAGANI_DSI_TIMEOUTS = 0x111, PAGANI_DSI_FIFO_ERRORS = 0xcccc0480u, PAGANI_DSI_ERROR = 1u << 24 };
static inline const char *pagani_panel_readback(const struct dsi_panel_state *before, const struct dsi_panel_state *after)
{
    if (after->version != before->version || !panel_probe_dsi_shape(after->version, after->ctrl, after->trigger)) return "shape";
    if (after->ctrl != before->ctrl || after->trigger != before->trigger || after->dma != before->dma || after->clocks != before->clocks) return "config";
    if (after->tpg) return "tpg";
    if (after->status & 2) return "busy";
    if (after->timeout & ~before->timeout & PAGANI_DSI_TIMEOUTS) return "timeout";
    if (after->fifo & ~before->fifo & PAGANI_DSI_FIFO_ERRORS) return "fifo";
    if (after->ints & ~before->ints & PAGANI_DSI_ERROR) return "error";
    return 0;
}
// Bit 8 around the first DCS, on start's thread with the writer held since panel_probe_ahb (host: log, sleep_ms, ticks, quiet and io[PANEL_BLOCK_DSI]). Before it: careful, the line
// `first DCS next` leaves the phone, and in any case a look at the frames. Nothing of this comes when the clock is not on: no command goes then.
static inline uint32_t pagani_panel_ahb_before(const struct panel_probe_host *host, const struct pagani_panel_run *r, unsigned options)
{
    const struct panel_probe p = {host, 0, 0, 0, 0, 0, 0};
    if (!r->ahb) return 0;
    if (pagani_panel_ahb_careful(options)) panel_probe_flush(&p);
    return panel_probe_frames(&p);
}
// After it, whatever it gave: the registers and the frames again (only a command that went has left anything to read), then the outcome. Anything but a clean answer holds the commands for
// the boot, a -110 first of all, the engine may still own that one. What goes back to start is what publishes: rc, or -5 for a command that went and left the controller otherwise than
// it found it. Bit 8 alone holds them after a clean answer too: one DCS, then reads only. The one way out of the writer's hold, for every path but a hang.
static inline int pagani_panel_ahb_after(uint32_t frames, const struct panel_probe_host *host, struct pagani_panel_run *r, unsigned options, int rc, const struct dsi_panel_state *seen)
{
    const struct panel_probe p = {host, 0, 0, 0, 0, 0, 0};
    uint32_t still = 0;
    const char *why = rc ? "no read" : 0;
    if (!rc) {
        struct dsi_panel_state after;
        dsi_panel_snapshot(&host->io[PANEL_BLOCK_DSI], &after);
        still = panel_probe_frames(&p);
        why = pagani_panel_readback(seen, &after);
        if (!why && frames && !still) why = "stream";
        if (why) rc = -5;
    }
    if (rc && !r->held) r->held = PAGANI_PANEL_DCS_HELD_LOST;
    if (!rc && !pagani_panel_ahb_slider(options)) r->held = PAGANI_PANEL_DCS_HELD_ONE;
    PANEL_PROBE_LOG(&p, "panel: ahb dcs rc %d %s held %s frames %u->%u", rc, why ? why : "clean", pagani_panel_held_name(r->held), frames, still);
    host->quiet(host->context, 0);
    return rc;
}

// The reporter's timer, once per period: whether a line is due and whether the registers go with it. They go once with the display on, and again
// when a quiet period passed while the commands stopped; never with the display off, which is the one state the display path may
// have left the MDP idle in (pagani-te-ar-off). A line when `seen` (the counters together) moved, and after PAGANI_PANEL_REPORT_QUIET periods anyway.
enum { PAGANI_PANEL_REPORT_COUNTERS = 1, PAGANI_PANEL_REPORT_REGISTERS = 2 };
struct pagani_panel_reports {
    uint32_t seen;
    uint32_t quiet;
    uint8_t logged;
};
static inline unsigned pagani_panel_report_due(struct pagani_panel_reports *r, uint32_t seen, unsigned display_on, unsigned stopped)
{
    const int quiet = ++r->quiet >= PAGANI_PANEL_REPORT_QUIET;
    const int registers = display_on && (!r->logged || (stopped && quiet));
    if (!registers && !quiet && seen == r->seen) return 0;
    if (registers) r->logged = 1;
    r->seen = seen;
    r->quiet = 0;
    return PAGANI_PANEL_REPORT_COUNTERS | (registers ? PAGANI_PANEL_REPORT_REGISTERS : 0);
}
// One tick at `now` (ns): the request word if one came, the display's power `want`, then the level while the panel shows; a panel off keeps
// the goal and takes it after display on. The next tick is asked for only while something runs (a sequence, a fade, a retry), so an idle panel costs no wake-up.
// A command not sent (-19, -16) goes again PAGANI_PANEL_RETRY_MS later, PAGANI_PANEL_RETRIES times in a row; past that the commands stop, with the
// panel as it is. Only not on a panel that is dark and needs this sequence to come back (a display on, a wake, the sleep after 0x28 went out):
// stopping there would leave the screen black until a reboot, so that one has no limit.
static inline void pagani_panel_tick(struct pagani_panel_run *r, uint64_t now, unsigned want, uint64_t request, pagani_panel_send send, void *context,
                                     struct pagani_panel_tick_result *out)
{
    out->rc = 0;
    out->next_ms = 0;
    out->stopped = out->publish = out->finished = out->woke = 0;
    if (pagani_brightness_request_valid(request))
        pagani_brightness_begin(&r->bright, pagani_brightness_request_level(request), pagani_brightness_request_fade(request), now);
    uint64_t wait = 0;
    unsigned finished = 0;
    int rc = pagani_panel_power_step(&r->power, now, want, send, context, &wait, &finished);
    out->finished = (uint8_t)finished;
    if (!rc && r->power.refresh) {
        r->power.refresh = 0;
        r->stale = 1;
        out->woke = 1;
    }
    const uint8_t shown = r->power.state == PAGANI_PANEL_ON && !r->power.seq;
    if (shown != r->shown) {
        r->shown = shown;
        out->publish = 1;
    }
    const uint32_t level = pagani_brightness_level(&r->bright, now);
    if (!rc && shown && (r->stale || level != r->bright.shown)) {
        rc = pagani_brightness_send(send, context, level);
        if (!rc) {
            r->bright.shown = level;
            r->stale = 0;
            ++r->writes;
            if (pagani_brightness_done(&r->bright)) out->publish = 1;
        }
    }
    out->rc = rc;
    if (rc) {
        const int dark = r->power.seq && (r->power.ends == PAGANI_PANEL_ON || r->power.next);
        if ((rc == -19 || rc == -16) && (++r->retries <= PAGANI_PANEL_RETRIES || dark)) {
            ++r->retried;
            out->next_ms = PAGANI_PANEL_RETRY_MS;
        } else {
            out->stopped = 1;
        }
        return;
    }
    r->retries = 0;
    if (r->power.seq || wait) {
        const uint64_t ms = (wait + 999999) / 1000000;
        out->next_ms = ms && ms < PAGANI_BRIGHTNESS_STEP_MS ? (uint32_t)ms : PAGANI_BRIGHTNESS_STEP_MS;
    } else if (shown && !pagani_brightness_done(&r->bright)) {
        out->next_ms = PAGANI_BRIGHTNESS_STEP_MS;
    }
}
#endif
