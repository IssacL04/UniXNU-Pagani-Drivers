// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026, UniXNU contributors. All rights reserved.
// Ported from UniXNU-sheng package/WifiProbe.cpp (https://github.com/UniXNU/UniXNU-sheng, commit 08cde20e): wifiThread (stage 1),
// bringUp (power, clocks, resets, PARF, PHY, BDF-to-SID, PERST#, LTSSM, the configuration window) and endpointProbe (the MEM window,
// BAR0) with the OnePlus 13T's facts (PaganiWifiTables.h) in place of the SM8550/WCN7850 constants, and as a portable sequence over
// register callbacks instead of mapped windows, so that the order and the values are checked by the host model (Test/bringup.cpp).
//
// Stages (README.md): 1 read-only survey; 2 power (sleep clock, the seven RPMh rails, the PCIe NoC vote, the AOSS PDC messages when
// asked, WLAN_EN, the GDSCs, the clocks, the block resets); 3 link (PARF, the PHY sequence of the running devicetree, BDF-to-SID,
// the root port, PERST#, LTSSM, L0, the configuration window and the endpoint's ID 17cb:110e); 4 endpoint (the MEM window, BAR0).
// Every step has a number (stage * 100 + n, kStep*); a failure leaves the negative step and an error code, and a log line, never
// anything else. Nothing here maps memory, sleeps without the callbacks or touches a register outside the windows of kWin*.
//
// No UFS write is on the wire while a block is touched for the first time or a rail moves under the UFS path (guard, letGo below): a read that
// hangs ends in a reset nothing sees coming, a reset on a write is a power cut in the middle of it (UfsLog.h), and this flash can lose a whole
// logical unit to that. Each such stretch starts with a line naming it, "[wifi] guard <name>", which has time to reach the UFS log; the writer is
// then held (pagani_panel_probe_quiet, bit 3) until the stretch is over. A stretch that hangs keeps it held, and that line is the last one read back.
#pragma once
#include <stdint.h>
#include "PaganiWifiTables.h"

namespace pagani_wifi {

enum Win : unsigned {kWinGcc, kWinTlmm, kWinParf, kWinPhy, kWinDbi, kWinAtu, kWinCfg, kWinTcsr, kWinCount};
// The bytes each window spans (the kernel maps these; the model checks every access against them).
constexpr uint32_t kWinBytes[kWinCount] = {kGccMapBytes, 0xcc000, 0x3000, 0x2000, 0x1000, 0x1000, 0x1000, 0x1000};
constexpr uint64_t kWinBase[kWinCount] = {kGcc, kTlmmPins, kParf, kPhy, kDbi, kAtu, kConfig, kTcsrPcie0ClkrefEn & ~0xfffull};

enum Step : int {
    kStepSurveyClocks = 101, kStepSurveyPins = 102, kStepSurveyRails = 103, kStepSurveyController = 104,
    kStepSleepClock = 201, kStepRails = 202, kStepBus = 203, kStepPdc = 204, kStepWlanEn = 205, kStepGdsc = 206, kStepClocks = 207,
    kStepResets = 208,
    kStepParf = 301, kStepPhy = 302, kStepSid = 303, kStepRootPort = 304, kStepPerst = 305, kStepLinkUp = 306, kStepEndpointId = 307,
    kStepMemWindow = 401, kStepBar = 402,
};
enum Error : int {
    kErrNone = 0, kErrStopped = -1, kErrRail = -2, kErrRailRead = -3, kErrBus = -4, kErrPdc = -5, kErrLock = -6, kErrGdsc = -7,
    kErrRcg = -8, kErrBranch = -9, kErrPhy = -10, kErrLink = -11, kErrAtu = -12, kErrEndpoint = -13, kErrBar = -14, kErrSleepClock = -15,
    kErrArgument = -22,
};

struct Hw {
    void *ctx;
    uint32_t (*read)(void *ctx, unsigned win, uint32_t offset);
    void (*write)(void *ctx, unsigned win, uint32_t offset, uint32_t value);   // completes with a barrier
    void (*udelay)(void *ctx, unsigned us);
    void (*msleep)(void *ctx, unsigned ms);
    uint64_t (*now_ms)(void *ctx);
    // The GCC branch vote registers 0x52000/0x52008 are shared (touch, speaker, battery I2C): the weak lock word
    // sm8750_gcc_vote_owner held around each read-modify-write. 0 taken, negative not.
    int (*vote_lock)(void *ctx, int take);
    // One RPMh VRM command on apps DRV-2: the cmd-db resource ("ldof3"), the register (+0 voltage mV, +4 enable, +8 mode), the
    // value. 0 or negative; *address gets the resource's RPMh address.
    int (*vrm)(void *ctx, const char *resource, uint32_t reg, uint32_t value, uint32_t *address);
    // A BCM bandwidth vote (avg/peak kBps over the node's bus width); 0 or negative; *vote gets the command's data word.
    int (*bcm)(void *ctx, const char *name, uint32_t avg_kbps, uint32_t peak_kbps, uint32_t node_buswidth, uint32_t *vote);
    // PMIC readback of a regulator through the SPMI observer (never written): STATUS 0x08, EN_CTL 0x46, MODE_CTL 0x45, set point mV
    // 0x40/0x41.
    int (*pmic)(void *ctx, uint16_t ppid, uint8_t *status, uint8_t *enable, uint8_t *mode, uint16_t *millivolts);
    // The PMK8550 gpio3 sleep clock pin: write 0 only reads it (1: an enabled output of func1, 0: not), write 1 also switches it
    // on when it is not (owner-checked). Negative on failure.
    int (*sleep_clock)(void *ctx, int write);
    // One AOSS QMP message (optional; null: the PDC step is skipped).
    int (*pdc)(void *ctx, const char *message);
    void (*log)(void *ctx, const char *line);
    // Holds the UFS log's writer (1) or lets it go (0), by this driver's bit of pagani_panel_probe_quiet. Optional (null: no holds, no waits for them).
    void (*quiet)(void *ctx, unsigned on);
};

// pagani_panel_probe_quiet is shared bit by bit (0 PaganiPanel, 1 PaganiBattery's USB probe, 2 PaganiBt). The writer looks at it every 500 ms at the
// longest and a round takes a few ms: PanelProbe.h's PANEL_PROBE_QUIET_MS, the same number.
constexpr uint32_t kQuietBit = 1u << 3;
constexpr unsigned kQuietMs = 1000;

struct Options {
    unsigned stage = 0;            // pagani-wifi
    unsigned maxGen = 2;           // pagani-wifi-gen (1..3)
    unsigned pdc = 0;              // pagani-wifi-pdc (0, 1, 2)
    unsigned sleepClock = 1;       // pagani-wifi-sleepclk
    unsigned s7iVote = 0;          // pagani-wifi-s7i=1: go on and vote S7I even when its set point cannot be read
    int stopAfter = 0;             // pagani-wifi-stop: return after this step (0: never)
    unsigned crumbMs = 0;          // how long a line takes to reach the UFS log (PaganiWifi.cpp crumbMs); 0: no UFS log, so nothing to hold
};

// voted: the voltage sent (0: no voltage command); skipped: 1 when the rail was left at the voltage it had.
struct RailResult {uint32_t address; int rc; uint8_t status, enable, mode; uint16_t millivolts, voted; uint8_t read, skipped;};

struct Bringup {
    const Hw *hw = nullptr;
    Options opt;
    int step = 0;                  // last step done; negative: the step that failed
    int error = 0;
    bool held = false;             // this driver's bit of the UFS log's hold is set
    // stage 1 (and the reads of later stages)
    uint32_t vote52008 = 0, vote52000 = 0, gdsc = 0, phyGdsc = 0, collapse = 0, halts = 0;   // halts: bit i = kBranches[i] reads CLK_OFF
    uint32_t pinCtl[sizeof kPins / sizeof kPins[0]] = {}, pinIo[sizeof kPins / sizeof kPins[0]] = {};
    uint32_t parfLtssm = 0, parfPhyCtrl = 0, rootId = 0, rootLink = 0;
    int sleepClock = -1;           // 1 on, 0 off, negative unknown
    RailResult rails[sizeof kRails / sizeof kRails[0]] = {};
    RailResult phyRails[sizeof kPhyRails / sizeof kPhyRails[0]] = {};   // read only, in the survey
    // stage 2
    uint32_t busVote = 0;
    int pdcSent = 0;
    // stage 3
    uint32_t pcsStatus = 0, phyPolls = 0, sidHash = 0, sidCfg = 0, portDebug0 = 0, portDebug1 = 0, linkStatus = 0, linkMs = 0;
    uint32_t endpointId = 0, perstLowMs = 0;
    uint64_t perstAssertedMs = 0;
    // stage 4
    uint32_t barMask = 0, barValue = 0, command = 0;
    uint64_t barAddress = 0, barBytes = 0;
};

// Each runs its stage and returns 0, or the negative error (b.step is then minus the failed step).
int survey(Bringup &b);        // stage 1: reads only; the controller only when its GDSC and clocks run
int powerOn(Bringup &b);       // stage 2
int linkUp(Bringup &b);        // stage 3
int endpoint(Bringup &b);      // stage 4: the MEM window and BAR0 (the kernel maps the BAR and maps the SMMU stream first)
// Lets go of a hold in force, then logs "[wifi] guard <what>", gives the line crumbMs to reach the UFS log, holds the writer and gives a round under
// way kQuietMs to end. letGo ends the stretch. A stretch that hangs never gets there. A failure and a stop let go by themselves.
void guard(Bringup &b, const char *what);
void letGo(Bringup &b);
// The BDF-to-SID hash of a requester ID (CRC-8, polynomial 7, MSB first, high byte first): 0x15 for 01:00.0.
uint8_t bdfHash(uint16_t bdf);
// The TLMM control word of a pin state: pull (-1 down, 0 none, 1 up), function, drive in mA (2..16), output enable.
uint32_t pinConfig(int pull, unsigned function, unsigned driveMa, bool output);
// One bandwidth of a BCM vote (Linux bcm-voter's rule, facts only): ceil(ceil(kbps * aux width / node bus width) * 1000 / aux
// unit), at least 1 for a nonzero bandwidth, at most the 14-bit field.
uint32_t bcmVote(uint32_t kbps, uint32_t width, uint32_t buswidth, uint32_t unit);

}  // namespace pagani_wifi
