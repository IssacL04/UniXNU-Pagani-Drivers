/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* What the boot loaders left of the USB path, read once on a running system and logged: the probe that decides how the controller is
 * taken over later (README.md "USB probe"). Off unless pagani-battery has bit 1 set. It lives in this driver because this driver has a
 * thread, the SPMI observer channel and the log already, and it needs no kernel patch.
 *
 * Nothing is written but the arbiter's observer read commands. The danger is in the reads: on this SoC a read of a block without power or
 * clocks does not fail, it takes the phone down. So the run goes from what cannot fault to what can, every step that can is opened by
 * what the steps before it read, and it comes PBU_SETTLE_MS after the line that announces it, one UFS log period and a margin: when a read
 * kills the phone, the log still says how far the run got. No such read comes with a UFS write on the wire either: the bite that ends a hang
 * gives no warning, and a reset that lands on a write is the one way the log can hurt the logical unit around it (UfsLog.h). So PbuIo.quiet
 * holds the log's writer, PBU_QUIET_MS for a round under way to end, for the reads of steps 5 to 8 and 10, and lets go with their lines
 * written, the way PaganiPanel's register probe does around its reads (PanelProbe.h). Only the always-on GCC is read at the driver's start
 * (pagani_usb_entry, the one line of the boot's first seconds: nothing in XNU touches USB, so that is how the boot loaders left it, and the
 * last line says whether it is still so). The rest in this order, once, PBU_DELAY_MS after the thread started:
 *
 *   1 rails    PMIC, observer channel: STATUS 0x08 (bit 7 in regulation) and EN_CTL 0x46 of the USB PHY's LDOs (vdd L2D, vdda12 L3G, refgen
 *              L1F) and the repeater's (L15B, L5B). PPIDs: the running devicetree's regulator-ocp-notifier periph-<ppid>-supply.
 *   2 pmic     the repeater (0x7fd: 08 RPTR_OK, 46 enable, 51-57 tune) and the charger's peripherals 0x726-0x72c, offsets 04-10 and 40-7f of
 *              each: charger stage, input limit, watchdog. The meanings are read off line by line later (scripts/decode-usb-probe.py).
 *   3 smmu     the apps SMMU's stream-match table for SID 0x40: the registers NativeUfs reads on every boot, and nothing else yet.
 *   4 gcc      always-on, and the gate of everything after it: the block reset 0x39000 released, the GDSC 0x39004 on (PWR_ON set, SW_COLLAPSE
 *              clear) and finished (CFG_GDSCR 0x39008: power-up complete, power-down complete clear), both roots running (CMD_RCGR
 *              0x39030 and 0x39048: ROOT_OFF clear) and all five branches running: master 0x39018, mock UTMI 0x3902c, sleep 0x39028 enabled
 *              with CLK_OFF clear, and the two NOC branches, aggre 0x39090 and cfg 0x3908c, the same or in hardware clock gating, where
 *              CLK_OFF reads 1 while idle (enabled with HW_CTL set: GccUfs.h, ufsBusClocksOn, the same two kinds on the UFS side).
 *              The offsets are SM8750's: mainline gcc-sm8750.c, and they differ from SM8550's in three places (aggre 0x39088, mock UTMI
 *              0x39028, sleep 0x39024), which is why nothing is shared with the SM8550 USB code. A closed gate ends the run here.
 *   5 dwc      GSNPSID first: anything that is not a Synopsys id (U3, 31, 32) ends the run. Then the globals, the device registers, the
 *              first event buffer and GHWPARAMS0-8; the xHCI command and status registers only in host mode.
 *   6 qscratch the glue, 0xa6f8800: general config, HS PHY control, clock gating, SS PHY control, power events.
 *   7 ref      TCSR_USB2_CLKREF_EN (0xf206008: tcsrcc 0xf204008 + 0x2000, the devicetree's <tcsrcc 2>), only with the PHY's LDOs in regulation (a
 *              failed read counts as not) and its block reset 0x12000 released. Android reads this block, this port never has, so it
 *              comes after everything the controller can answer.
 *   8 phy      the M31 eUSB2 PHY 0x88e3000 (phy-qcom-m31-eusb2.c offsets), only with that gate and the reference clock on. Nothing near the EUD
 *              (0x88e0000-0x88e2fff, 0xc278000) is mapped or read.
 *   9 done     one line with the verdict: route=inherit (device mode, PHY up: take the controller over), host (stop xHCI first),
 *              cold (no power: bring it up from nothing), mixed (anything else) or noid.
 *  10 banks    the context bank of a translating stream entry: CBAR, CBA2R, SCTLR, TTBR0, TCR, FSR. Last, after the verdict: NativeUfs has read
 *              only the first three, of its own bank.
 *
 * Portable (the host test Test/usb.c runs it against a model in which a read of a block without power fails the test); PaganiBattery.cpp
 * maps the windows and calls pagani_usb_step from its thread. Register facts: mainline Linux gcc-sm8750.c, tcsrcc-sm8750.c, dwc3 core.h,
 * dwc3-qcom.c, phy-qcom-m31-eusb2.c, the ARM SMMUv2 architecture and the 13T running devicetree; no code of them is used. */
#ifndef PAGANI_BATTERY_USB_H
#define PAGANI_BATTERY_USB_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define PBU_DELAY_MS 120000u        /* after the thread started: the desktop is up and everything else has started */
#define PBU_SETTLE_MS 4000u         /* the UFS log writes every 3 s */
#define PBU_QUIET_MS 1000u          /* the writer looks at the hold every 500 ms, a round takes at most some ms: PanelProbe.h's PANEL_PROBE_QUIET_MS */
#define PBU_QUIET_BIT 2u            /* this probe's bit of pagani_panel_probe_quiet: its bit 0 is PaganiPanel's, whose probe runs at the same time of the boot */
#define PBU_LINE 200u               /* one U record (RamoopsLog.h RAMOOPS_LOG_TEXT_MAX) with its terminator: no line continues in a + record */
#define PBU_SMMU_SHOWN 2u           /* the stream's entries shown */

/* The register windows; the owner maps each one and frees them when the run is over. */
enum {PbuWinGcc, PbuWinTcsr, PbuWinXhci, PbuWinDwc, PbuWinQscratch, PbuWinPhy, PbuWinSmmu, PbuWinCount};
typedef struct {
    uint64_t base;
    uint32_t bytes;
} PbuWindow;
extern const PbuWindow pagani_usb_windows[PbuWinCount];

typedef struct {
    void *ctx;
    uint32_t (*read)(void *ctx, unsigned window, uint32_t offset);
    int (*pmic)(void *ctx, unsigned ppid, uint8_t offset, uint8_t *value);   /* one PMIC register (0, or negative) */
    void (*trace)(void *ctx, const char *line);
    void (*quiet)(void *ctx, unsigned on);                                   /* holds the UFS log's writer (1) or lets it go (0) */
} PbuIo;

typedef struct {
    uint32_t bcr, gdsc, cfg, master, aggre, noc, utmi, sleep, master_rcg, utmi_rcg;
} PbuGcc;

typedef struct {
    const PbuIo *io;
    uint32_t stage;
    PbuGcc entry;                   /* as the driver started */
    uint8_t entry_read, gcc_same;
    uint8_t held;                   /* the writer is held for the next step */
    uint8_t rails_ok;               /* every PHY LDO read and in regulation */
    uint8_t core_open;              /* step 4's gate */
    uint8_t phy_read, id_ok;
    uint32_t phy_bcr;               /* the PHY's block reset */
    uint32_t gsnpsid, gctl, dctl, dsts;
    uint32_t phy_ctrl5, phy_common0;
    uint32_t smmu_pages, smmu_banks, smmu_bank[PBU_SMMU_SHOWN];
    uint32_t pmic_failed;           /* PMIC bytes that could not be read */
    uint32_t lines;
    const char *why;                /* what closed a gate: "-" none */
    char line[PBU_LINE];
} PbuRun;

void pagani_usb_init(PbuRun *r, const PbuIo *io);
/* The GCC as the driver started: reads and logs that window alone. */
void pagani_usb_entry(PbuRun *r);
/* Does the next step; returns how many milliseconds to wait before the next call, 0 when the run is over. */
uint32_t pagani_usb_step(PbuRun *r);
/* The verdict: "inherit", "host", "cold", "mixed" or "noid"; valid when the run is over. */
const char *pagani_usb_route(const PbuRun *r);

#ifdef __cplusplus
}
#endif
#endif
