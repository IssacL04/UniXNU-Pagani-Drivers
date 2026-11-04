// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026, UniXNU contributors. All rights reserved.
//
// OnePlus 13T (PKX110, SM8750 "sun", board "pagani") Wi-Fi facts as tables: the PCIe0 root complex that carries the
// Qualcomm "peach" WLAN chip (PCI 17cb:110e, peach_v2), its PHY, clocks, resets, GDSCs, pins, rails, RPMh/AOSS
// messages, SMMU stream, MSI routing, and what peach needs from an ath12k-style host (MHI, QMI, firmware names,
// register windows). The fact tables of the PaganiWifi board driver; the structure follows UniXNU-sheng
// package/WifiPcieTables.h and package/WifiProbe.cpp (https://github.com/UniXNU/UniXNU-sheng, commit 08cde20e,
// BSD-2-Clause-Patent): same kinds of facts, the 13T's values. No code of any GPL or vendor source is copied; every
// number below is a hardware fact, with its source in a Src tag and in facts.md next to this file.
//
// Sources (Src):
//   kDevice    this phone: the running device tree (/sys/firmware/fdt, sha256 13b8b74a8dba343b..., decompiled as
//              build-userspace/a830-inputs/dt/running.dts) and its Android properties.
//   kVendor    OnePlusOSS sources of this exact firmware line (PKX110_16.0.10.500): android_kernel_oneplus_sm8750
//              @745702d5 (pci-msm.c, gcc-sun.c, MHI host boot.c) and android_kernel_modules_and_devicetree_oneplus_sm8750
//              @6a196f79 (cnss2, cnss_utils, fw-api/hw/peach, qca-wifi-host-cmn, wlan-devicetree
//              oplus/pagani_sun-peach-cnss-v8-overlay.dts -> sun-peach-cnss-v8.dtsi).
//   kMainline  Linux torvalds master @36aad21d (2026-10-31): sm8750.dtsi, gcc-sm8750.c, tcsrcc-sm8750.c,
//              phy-qcom-qmp-pcie.c, pinctrl-sm8750.c, interconnect/qcom/sm8750.c; Linux v7.0.14 MHI host (Ath12kWifi's input).
//   kPagani    renhiyama/pagani-linux @2635bc17 (Linux 7.1 on this phone; its peach ath12k support is a bring-up, the
//              firmware still faulted in some paths there): facts only.
//   kDerived   computed from the above (rule given in the comment).
//   kUnknown   not established tonight: build-userspace/a830-inputs/wifi/morning-facts.sh reads it on the phone.
// A fact the code depends on and that is kDerived or kUnknown must stay overridable (boot argument or table entry).
#pragma once
#include <stdint.h>

namespace pagani_wifi {

enum Src : uint8_t { kUnknown = 0, kDevice = 1, kVendor = 2, kMainline = 3, kPagani = 4, kDerived = 5 };

// ---------------------------------------------------------------------------------------------------------------------
// 1. The PCIe instance: PCIe0 (pcie@1c00000, "qcom,pci-msm" with qcom,drv-name = "cesta" on Android; mainline
//    "qcom,pcie-sm8750" + "qcom,pcie-sm8550", i.e. the SM8550 programming model). The only PCIe controller enabled on
//    this phone; cnss "qcom,wlan-rc-num = <0>". Root port BDF 00:00.0 (requester 0x0), endpoint 01:00.0 (requester 0x100).
// ---------------------------------------------------------------------------------------------------------------------
struct Window {const char *name; uint64_t base; uint32_t bytes; Src src;};
constexpr Window kPcie0Windows[] = {
    {"parf",    0x01c00000, 0x3000,    kDevice},    // reg-names "parf"
    {"phy",     0x01c06000, 0x2000,    kDevice},    // QMP gen3x2 PHY, v7 layout (section 3)
    {"dbi",     0x40000000, 0x0f1d,    kDevice},    // "dm_core" (DesignWare DBI)
    {"elbi",    0x40000f20, 0x00a8,    kDevice},
    {"atu",     0x40001000, 0x1000,    kDevice},    // "iatu", unrolled: 0x200 per outbound region
    {"config",  0x40100000, 0x100000,  kDevice},    // "conf": 1 MiB config window (iATU region 0 -> bus 1)
    {"pcie_sm", 0x01d07000, 0x7000,    kDevice},    // CESTA PCIe state manager: Android only; never touched here
    {"mhi",     0x01c03000, 0x1000,    kMainline},  // the controller's own MHI block (mainline reg "mhi"), debug only
    {"io",      0x40200000, 0x100000,  kDevice},    // ranges: I/O window
    {"mem",     0x40300000, 0x3d00000, kDevice},    // ranges: 32-bit MEM window, CPU = PCI address (mainline: 0x23d00000)
};
constexpr uint64_t kParf = 0x01c00000, kPhy = 0x01c06000, kDbi = 0x40000000, kAtu = 0x40001000, kConfig = 0x40100000;
constexpr uint64_t kMemBase = 0x40300000;
constexpr uint32_t kMemBytes = 0x3d00000;
constexpr unsigned kRcLanes = 2;                  // mainline num-lanes = <2>; the endpoint's trained width: kUnknown
constexpr unsigned kRcMaxGen = 3;                 // qcom,bw-scale has three entries (Gen1..Gen3), PHY "gen3x2"
constexpr uint16_t kRootRid = 0x0000, kEndpointRid = 0x0100;
constexpr uint32_t kEndpointVendorDevice = 0x110e17cb;   // config dword 0: device << 16 | vendor (cnss supported-ids <0x110e>)

// Per-Gen votes of the root complex (qcom,bw-scale <cx mx rate-change-clock>): RPMh cx.lvl / mx.lvl minimum levels
// (64 = LOW_SVS, 256 = NOM) and the PHY rate-change clock. Mainline's OPP table agrees (Gen3 needs NOM).
struct GenScale {unsigned gen; uint16_t cxLevel, mxLevel; uint32_t rchngHz;};
constexpr GenScale kGenScale[] = {{1, 64, 64, 19200000}, {2, 64, 64, 19200000}, {3, 256, 256, 100000000}};
constexpr uint32_t kAuxClkFreqMhz = 20;           // qcom,aux-clk-freq (19.2 MHz rounded); msm_pcie writes it to DBI 0xb40

// ---------------------------------------------------------------------------------------------------------------------
// 2. Interrupts and MSI.
//    Android: msi-map <0x0 &gic_its 0x1400 1>, <0x100 &gic_its 0x1401 1> -> the GIC ITS at 0x16040000 (LPIs); cnss asks for
//    32 MSI vectors (falls back to one). How many it got: kUnknown (morning /proc/interrupts).
//    XNU (no ITS driver): the DesignWare iMSI receiver in the DBI raises SPI 141 ("msi0", vectors 0-31) like sheng.
//    The "qcom,pci-msi" node at 0x17110040 (SPI 768-799) is status = "disabled" and its unit address disagrees with its
//    reg: do not use it.
// ---------------------------------------------------------------------------------------------------------------------
constexpr uint32_t kSpiGlobal = 140;              // "int_global_int" / mainline "global" (kDevice + kMainline)
constexpr uint32_t kSpiMsi0 = 141;                // mainline "msi0"; msi1..msi7 = 142..148 (kMainline; not in the vendor DT)
constexpr uint32_t kSpiIntA = 149;                // INTA..INTD = 149..152 (kDevice)
constexpr uint64_t kGicIts = 0x16040000;          // Android's MSI controller (kDevice); DeviceIDs 0x1400 / 0x1401
constexpr uint32_t kMsiAddrLo = 0x820, kMsiAddrHi = 0x824;            // DBI, DesignWare iMSI (same core as sheng)
constexpr uint32_t kMsiIntrEnable = 0x828, kMsiIntrMask = 0x82c, kMsiIntrStatus = 0x830, kMsiCtrlStride = 0xc;  // ctrl n at +n*0xc
// cnss2's MSI plan on this phone (pci_qcom.c msi_config, PCI_IRQ_MSI, 32 vectors); ath12k's wifi7 plan uses 16.
struct MsiUser {const char *name; uint8_t vectors, base;};
constexpr MsiUser kCnssMsiPlan[] = {{"MHI", 3, 0}, {"CE", 10, 3}, {"WAKE", 1, 13}, {"DP", 18, 14}};
constexpr MsiUser kAth12kMsiPlan[] = {{"MHI", 3, 0}, {"CE", 5, 3}, {"DP", 8, 8}};
constexpr uint32_t kMsixMatchAddr = 0x3000;       // cnss "msix-match-addr": used only if MSI-X were enabled (it is not)

// ---------------------------------------------------------------------------------------------------------------------
// 3. The QMP PCIe PHY (0x1c06000, 8 KiB), v7 layout: serdes 0x0, pcs 0x400, pcs_misc 0x800, tx 0x1000, rx 0x1200,
//    tx2 0x1800, rx2 0x1a00 (mainline qmp_pcie_offsets_v7; sheng's SM8550 PHY is v5: pcs 0x200, tx 0xe00, rx 0x1000).
// ---------------------------------------------------------------------------------------------------------------------
namespace phy {
constexpr uint32_t kSerdes = 0x0, kPcs = 0x400, kPcsMisc = 0x800, kTx = 0x1000, kRx = 0x1200, kTx2 = 0x1800, kRx2 = 0x1a00;
constexpr uint32_t kSwReset = 0x400, kPcsStatus1 = 0x414, kPowerDownControl = 0x440, kStartControl = 0x444;
constexpr uint32_t kPhyStatus = 1u << 6;          // PCS_STATUS1 bit 6 (qcom,phy-status-offset/-bit): 1 = not ready
constexpr uint32_t kPowerDownOn = 0x3;            // SW_PWRDN | REFCLK_DRV_DSBL
constexpr uint32_t kStartBoth = 0x3;              // SERDES_START | PCS_START
struct Write {uint16_t offset; uint8_t value;};
// qcom,phy-sequence of the running DT, in its order (every delay field is 0). It opens with POWER_DOWN_CONTROL = 3 and ends
// with SW_RESET = 0 then START_CONTROL = 3; between them: serdes (38, identical to sheng's kSerdes in values and order),
// rx lane 1 (29), tx2 0x1800 = 0 (1), tx lane 1 (7), rx lane 2 (29), tx lane 2 (7), pcs (7) and pcs_misc (7).
// Against mainline sm8750_qmp_gen3x2_pcie_*_tbl this phone's sequence differs in: RX_MODE_01_HIGH4 and RX_MODE_10_HIGH4
// (0x1380/0x1394 and lane 2 0x1b80/0x1b94) = 0x8f where mainline has 0x9c; 0x5e4 = 0x0f where mainline writes
// EQ_CONFIG2 at 0x5e0; and it adds 0x8f8 = 0x1f and 0x1800 = 0. The phone's own sequence is the validated one.
constexpr Write kSequence[] = {
    {0x0440, 0x03}, {0x00c0, 0x01}, {0x00cc, 0x62}, {0x00d0, 0x02}, {0x0060, 0xf8}, {0x0064, 0x01}, {0x0000, 0x93}, {0x0004, 0x01},
    {0x00e0, 0x90}, {0x00e4, 0x82}, {0x00f4, 0x07}, {0x0070, 0x02}, {0x0010, 0x02}, {0x0074, 0x16}, {0x0014, 0x16}, {0x0078, 0x36},
    {0x0018, 0x36}, {0x0110, 0x08}, {0x00bc, 0x0a}, {0x0120, 0x42}, {0x0080, 0x04}, {0x0084, 0x0d}, {0x0020, 0x0a}, {0x0024, 0x1a},
    {0x0088, 0x41}, {0x0028, 0x34}, {0x0090, 0xab}, {0x0094, 0xaa}, {0x0098, 0x01}, {0x0030, 0x55}, {0x0034, 0x55}, {0x0038, 0x01},
    {0x0140, 0x14}, {0x0164, 0x34}, {0x003c, 0x01}, {0x001c, 0x04}, {0x0174, 0x16}, {0x01bc, 0x0f}, {0x0170, 0xa0}, {0x13a4, 0x38},
    {0x12dc, 0x11}, {0x1360, 0xbf}, {0x1364, 0xbf}, {0x1368, 0xb7}, {0x136c, 0xea}, {0x135c, 0x3f}, {0x1374, 0x09}, {0x1378, 0x49},
    {0x137c, 0x1b}, {0x1380, 0x8f}, {0x1370, 0xd1}, {0x1388, 0x09}, {0x138c, 0x49}, {0x1390, 0x1b}, {0x1394, 0x8f}, {0x1384, 0xd1},
    {0x12c4, 0x3e}, {0x12c8, 0x1e}, {0x12cc, 0xd2}, {0x1208, 0x09}, {0x1214, 0x05}, {0x124c, 0x08}, {0x1250, 0x08}, {0x12d8, 0x09},
    {0x1318, 0x1c}, {0x131c, 0x60}, {0x12f8, 0x07}, {0x13f8, 0x08}, {0x1800, 0x00}, {0x1084, 0x35}, {0x108c, 0x10}, {0x1090, 0x31},
    {0x1094, 0x7f}, {0x10e4, 0x02}, {0x1040, 0x08}, {0x103c, 0x14}, {0x1ba4, 0x38}, {0x1adc, 0x11}, {0x1b60, 0xbf}, {0x1b64, 0xbf},
    {0x1b68, 0xb7}, {0x1b6c, 0xea}, {0x1b5c, 0x3f}, {0x1b74, 0x09}, {0x1b78, 0x49}, {0x1b7c, 0x1b}, {0x1b80, 0x8f}, {0x1b70, 0xd1},
    {0x1b88, 0x09}, {0x1b8c, 0x49}, {0x1b90, 0x1b}, {0x1b94, 0x8f}, {0x1b84, 0xd1}, {0x1ac4, 0x3e}, {0x1ac8, 0x1e}, {0x1acc, 0xd2},
    {0x1a08, 0x09}, {0x1a14, 0x05}, {0x1a4c, 0x08}, {0x1a50, 0x08}, {0x1ad8, 0x09}, {0x1b18, 0x1c}, {0x1b1c, 0x60}, {0x1af8, 0x07},
    {0x1bf8, 0x08}, {0x1884, 0x35}, {0x188c, 0x10}, {0x1890, 0x31}, {0x1894, 0x7f}, {0x18e4, 0x02}, {0x1840, 0x08}, {0x183c, 0x14},
    {0x04dc, 0x05}, {0x0588, 0x77}, {0x0598, 0x0b}, {0x08a4, 0x1e}, {0x08f4, 0x27}, {0x05e4, 0x0f}, {0x080c, 0x1d}, {0x0814, 0x07},
    {0x0820, 0xc1}, {0x0894, 0x00}, {0x08f8, 0x1f}, {0x05d0, 0x8c}, {0x0568, 0x17}, {0x0570, 0x2e}, {0x0400, 0x00}, {0x0444, 0x03},
};
constexpr unsigned kSequenceCount = sizeof kSequence / sizeof kSequence[0];
static_assert(kSequenceCount == 128, "the running DT's qcom,phy-sequence has 128 writes");
static_assert(kSequence[0].offset == kPowerDownControl && kSequence[0].value == kPowerDownOn, "sequence opens powered down");
static_assert(kSequence[126].offset == kSwReset && kSequence[127].offset == kStartControl, "sequence ends with reset release and start");
// PHY supplies: not in the vendor DT (CESTA owns them on Android). Mainline sm8750-mtp: vdda-phy = L1F 0.88 V, vdda-pll =
// L3G 1.2 V; on this phone those two LDOs also feed the UFS PHY (refgen and pll), so they are on whenever NativeUfs reads.
}  // namespace phy

// ---------------------------------------------------------------------------------------------------------------------
// 4. PARF (Qualcomm wrapper), DBI and iATU registers: the SM8550 model (pcie-qcom.c ops 1_9_0 in mainline, the same
//    offsets in the vendor pci-msm.c). Identical to sheng's; only the windows above differ.
// ---------------------------------------------------------------------------------------------------------------------
namespace parf {
constexpr uint32_t kSysCtrl = 0x0, kPmCtrl = 0x20, kPmStts = 0x24, kPhyCtrl = 0x40, kMhiClockResetCtrl = 0x174;
constexpr uint32_t kAxiMstrWrAddrHalt = 0x1a8, kLtssm = 0x1b0, kIntAllStatus = 0x224, kIntAllClear = 0x228, kIntAllMask = 0x22c;
constexpr uint32_t kStatus = 0x230, kClkreqOverride = 0x2b0, kDbiBase = 0x350, kDbiBaseHi = 0x354, kSlvAddrSpace = 0x358;
constexpr uint32_t kSlvAddrSpaceHi = 0x35c, kAtuBase = 0x634, kAtuBaseHi = 0x638, kDeviceType = 0x1000;
constexpr uint32_t kBdfToSidTable = 0x2000, kBdfToSidCfg = 0x2c00;   // 256 words: BDF << 16 | SID offset << 8 | next
constexpr uint32_t kDeviceTypeRc = 4, kLtssmEnable = 1u << 8, kPhyTestPwrDown = 1u << 0, kBdfToSidBypass = 1u << 0;
constexpr uint32_t kCestaConfig = 0x2c48;         // qcom,pcie-clkreq-offset: CESTA only, not used without Android
}  // namespace parf
namespace dbi {
constexpr uint32_t kCommand = 0x4, kBusNumbers = 0x18, kMemBaseLimit = 0x20, kCapPtr = 0x34, kPcieCap = 0x70;
constexpr uint32_t kLinkControlStatus = 0x80, kLinkControl2 = 0xa0, kPortDebug0 = 0x728, kPortDebug1 = 0x72c, kAuxClkFreq = 0xb40;
constexpr uint32_t kLtssmL0 = 0x11, kLtssmMask = 0x3f, kPortDebug1LinkUp = 1u << 4;
}  // namespace dbi
namespace atu {
constexpr uint32_t kRegionStride = 0x200, kCtrl1 = 0x0, kCtrl2 = 0x4, kLowerBase = 0x8, kUpperBase = 0xc, kLimit = 0x10;
constexpr uint32_t kLowerTarget = 0x14, kUpperTarget = 0x18, kUpperLimit = 0x20, kEnable = 1u << 31;
constexpr uint32_t kTypeMem = 0, kTypeIo = 2, kTypeCfg0 = 4, kTypeCfg1 = 5;
}  // namespace atu

// ---------------------------------------------------------------------------------------------------------------------
// 5. GCC (0x100000, 0x1f4200 bytes). Offsets agree between mainline gcc-sm8750.c and the vendor gcc-sun.c.
// ---------------------------------------------------------------------------------------------------------------------
constexpr uint64_t kGcc = 0x00100000;
constexpr uint32_t kGccBytes = 0x1f4200, kGccMapBytes = 0x80000;      // every register below is under 0x80000
constexpr uint32_t kGdscPcie0 = 0x6b004, kGdscPcie0Phy = 0x6c000;    // parents: cx.lvl (S1J) and mx.lvl
constexpr uint32_t kGdscCollapseVote = 0x5214c;                       // DT qcom,collapse-vote <syscon 0x15214c bit>; sheng: 0x52020
constexpr uint32_t kCollapsePcie0 = 1u << 0, kCollapsePcie0Phy = 1u << 2;   // a set bit keeps the GDSC collapsed (sheng: 0 and 3)
constexpr uint32_t kGdscPwrOn = 1u << 31;
struct Branch {const char *name; uint32_t voteReg; uint8_t voteBit; uint32_t haltReg; bool hwGated;};
// The enable votes (APCS branch vote registers) and the halt register of each branch (bit 31 CLK_OFF). Gated branches
// (hwGated) can read CLK_OFF while idle.
constexpr Branch kBranches[] = {
    {"pcie_0_slv_axi",        0x52008, 0,  0x6b020, true},
    {"pcie_0_mstr_axi",       0x52008, 1,  0x6b030, true},
    {"pcie_0_cfg_ahb",        0x52008, 2,  0x6b040, true},
    {"pcie_0_aux",            0x52008, 3,  0x6b044, false},
    {"pcie_0_pipe",           0x52008, 4,  0x6b054, false},   // runs only once the PHY drives the pipe clock
    {"pcie_0_slv_q2a_axi",    0x52008, 5,  0x6b01c, false},
    {"cnoc_pcie_sf_axi",      0x52008, 6,  0x10058, true},
    {"qmip_pcie_ahb",         0x52000, 11, 0x6b018, true},    // not voted by sheng
    {"aggre_noc_pcie_axi",    0x52000, 12, 0x10068, true},
    {"ddrss_pcie_sf_qtb",     0x52000, 19, 0x1007c, true},
    {"cfg_noc_pcie_anoc_ahb", 0x52000, 20, 0x10050, true},    // not voted by sheng
    {"pcie_0_phy_rchng",      0x52000, 22, 0x6b064, false},
};
constexpr uint32_t kBranchOff = 1u << 31;
constexpr uint32_t kVote52008Bits = 0x7f, kVote52000Bits = (1u << 11) | (1u << 12) | (1u << 19) | (1u << 20) | (1u << 22);
constexpr uint32_t kPipeMux = 0x6b080, kPipeMuxPhy = 0, kPipeMuxXo = 2;   // sheng (SM8550): 0x6b070
constexpr uint32_t kAuxRcg = 0x6b084, kRchngRcg = 0x6b068;               // CMD at +0 (bit 0 update), CFG at +4; sheng: 0x6b074, 0x6b058
constexpr uint32_t kAuxCfg19p2MHz = 0;                                 // source 0 (TCXO), divider bypass
constexpr uint32_t kRchngCfg100MHz = (6u << 8) | 5u;                   // source 6 (GPLL0_OUT_EVEN), divider 3 (field 2*3-1)
constexpr uint32_t kBcrPcie0 = 0x6b000, kBcrPcie0Phy = 0x6c01c;          // DT resets <gcc 6>, <gcc 9> (vendor numbering = mainline + 2)
constexpr uint32_t kBcrPcie0LinkDown = 0x6c014, kBcrPcie0NocsrComPhy = 0x6c020, kBcrPcie0PhyNocsrComPhy = 0x6c028;
constexpr uint64_t kTcsrPcie0ClkrefEn = 0x0f204008;  // tcsrcc base, offset 0, bit 0 (sheng: 0x1fd5000 + 0x100)
constexpr uint32_t kTcsrClkrefEnBit = 1u << 0;

// ---------------------------------------------------------------------------------------------------------------------
// 6. TLMM pins. Pin n's CTL at 0x0f100000 + 0x1000 * n, IO at +4 (the vendor node starts at 0xf000000; the pin pages start
//    0x100000 in, as PaganiSpeaker uses). CTL: pull 1:0 (1 down, 3 up), function 5:2, drive 8:6 (mA/2 - 1), OE bit 9,
//    eGPIO present bit 11 / enable bit 12. IO: bit 0 input level, bit 1 output.
// ---------------------------------------------------------------------------------------------------------------------
constexpr uint64_t kTlmmPins = 0x0f100000;
constexpr uint32_t kTlmmPinStride = 0x1000;
struct Pin {const char *name; uint16_t gpio; uint8_t func; uint8_t driveMa; int8_t pull; bool activeLow; bool egpio; Src src;};
// pull: -1 down, 0 none, 1 up (the state Android selects while WLAN is on)
constexpr Pin kPins[] = {
    {"wlan_en",       16,  0, 16,  1, false, false, kDevice},  // "wlan_en_active": output high, 16 mA, pull-up; sleep: low, 2 mA, pull-down
    {"sw_ctrl",       18,  1,  0,  0, false, false, kDevice},  // function wcn_sw_ctrl (PMU state to the SoC); MPM wake
    {"wlan_sw_ctrl",  19,  1,  0,  0, false, false, kDevice},  // function wcn_sw; MPM wake
    {"perst",         102, 0,  2, -1, true,  false, kDevice},  // PERST#, gpio, 2 mA, pull-down (sheng: 94)
    {"clkreq",        103, 1,  2,  1, true,  false, kDevice},  // pcie0_clk_req_n (function 1), pull-up (sheng: 95)
    {"wake",          104, 0,  2,  1, true,  false, kDevice},  // WAKE#, gpio input, pull-up (sheng: 96)
    {"host_sol",      202, 0,  4, -1, false, true,  kDevice},  // host sign-of-life out (cnss drives 0 after power-on)
    {"dev_sol",       203, 0,  0,  0, false, true,  kDevice},  // device sign-of-life in (falling edge = device error)
};
// BT enable is not a TLMM pin: PM8550VE "F" (SPMI SID 5) gpio3 (PPID 0x58a), not needed for WLAN.

// ---------------------------------------------------------------------------------------------------------------------
// 7. PMIC side (SPMI arbiter v7, same addresses as SM8550/sheng).
// ---------------------------------------------------------------------------------------------------------------------
constexpr uint64_t kSpmiCore = 0x0c400000, kSpmiObsrvr = 0x0c440000, kSpmiChnls = 0x0c500000, kSpmiCnfg = 0x0c42d000;
constexpr uint32_t kSleepClkPpid = 0x0ba;          // PMK8550 (SID 0) gpio3 = "alt_sleep_clk", function func1, output (= sheng)
// Regulator peripherals on these PMICs (regulator-ocp-notifier "periph-<sid><page>" of the running DT):
// SMPS n at page 0x9b + 3 * (n - 1); LDO n at page 0xc0 + n (sheng's SM8550 PMICs: LDO15 at 0x4e, a different layout).
// Readback as sheng does: STATUS 0x08 bit 7 (in regulation), EN_CTL 0x46 bit 7, voltage setpoint mV at 0x40/0x41.

// ---------------------------------------------------------------------------------------------------------------------
// 8. WLAN rails (cnss "qcom,cnss-peach" supplies; all RPMh VRM resources, voted mode + voltage + enable as sheng learned).
//    order = the order cnss2 turns them on (its vreg list order); chosenMv = cnss min_uv. The DT floors of S4D/S5F/S7I are
//    500/500/1224 mV; pagani-linux raised them to these values (its note: with S4D, the aon rail, at 0.5 V the chip's PMU
//    never released; S5F and S7I follow the peach spec). VRM register at +0 voltage (mV), +4 enable, +8 mode
//    (7 = NPM/HPM: cnss AOP enum RET 3, LPM 4, AUTO 6, NPM 7).
// ---------------------------------------------------------------------------------------------------------------------
struct Rail {
    const char *cnssSupply; const char *resource; const char *regulator; uint16_t ppid;
    uint16_t minMv, maxMv, chosenMv; uint16_t loadMa; const char *pdcSs; const char *sharedWith; uint8_t order;
};
constexpr Rail kRails[] = {
    {"vdd-wlan-io",        "ldof3", "pm_v8f_l3",  0x5c3, 1800, 1800, 1800, 30, nullptr, "BT vdd18-aon; always-on", 1},
    {"vdd-wlan-io12",      "ldof2", "pm_v8f_l2",  0x5c2, 1200, 1200, 1200, 30, nullptr, "BT vdd12-io; always-on", 2},
    {"vdd-wlan-ant-share", "ldob8", "pm_humu_l8", 0x1c8, 2800, 2800, 2800, 0,  nullptr, "sdhc_2 vdd-io (no SD slot)", 3},
    {"vdd-wlan-aon",       "smpd4", "pm_v8d_s4",  0x3a4, 876,  1036, 876,  0,  "bb",    "BT vdd-dig, wlan-aon", 4},
    {"vdd-wlan-dig",       "smpf5", "pm_v8f_s5",  0x5a7, 876,  1000, 876,  0,  "rf",    "BT rfa0p75/aon", 5},
    {"vdd-wlan-rfa1",      "smpg3", "pm_v8g_s3",  0x6a1, 1860, 2000, 1860, 0,  "rf",    "BT rfa1p8", 6},
    {"vdd-wlan-rfa2",      "smpi7", "pm_v8i_s7",  0x8ad, 1312, 1340, 1312, 0,  "rf",    "UFS vccq-parent! never lower or disable", 7},
};
// The LDOs the PCIe PHY hangs on. Neither the vendor devicetree nor this driver votes them (CESTA does on Android) and they are never
// written here: the sources disagree on which two they are (mainline sm8750-mtp: vdda-phy L1F, vdda-pll L3G; pagani-linux on this phone:
// L3I, L2I) and all four are the UFS path's (kDevice: refgen L1F, pll L3G, qref L3I, vddp-ref-clk L2I; L3G also feeds the DSI PHY), so
// NativeUfs reading the root keeps them on. Android runs them in HPM (qcom,init-mode 4). The survey reads them: state, set point, mode.
struct PhyRail {const char *resource; uint16_t ppid; const char *sharedWith;};
constexpr PhyRail kPhyRails[] = {
    {"ldof1", 0x5c1, "UFS refgen, USB, CSI"}, {"ldog3", 0x6c3, "UFS pll, DSI, USB, CSI"},
    {"ldoi3", 0x8c3, "UFS qref"},             {"ldoi2", 0x8c2, "UFS vddp-ref-clk, WCD"},
};
// The chip-side map (qcom,pmu_vreg_map): PMU pin -> SoC rail. Used to turn the firmware's PMU parameters (QMI target-cap
// ol_cpr_cfg) into AOP PDC messages.
struct PmuPin {const char *pin; const char *rail;};
constexpr PmuPin kPmuVregMap[] = {
    {"VDDD_AON_0P9", "s5f"}, {"VDDA_RFA_1P9", "s3g"}, {"VDDA_RFA_1P3", "s7i"}, {"VDDA_RFA_0P9", "s5f"}, {"VDDD_WLMX_0P9", "s4d"},
    {"VDDD_WLCX_0P9", "s5f"}, {"VDDD_BTCX_0P9", "s5f"}, {"VDDD_BTCMX_0P9", "s5f"}, {"VDDA_PCIE_1P2", "s7i"}, {"VDDA_PCIE_0P9", "s7i"},
};
// The chip's reference clock: pagani-linux's PMU node takes RPMH_RF_CLK1 (rpmhcc index 6, also BT's bt_rf_clk1); cnss's
// WLAN node lists no clock (kUnknown whether RF_CLK1 is on without the BT driver: morning-facts.sh reads clk_summary).
constexpr const char *kRefClock = "RPMH_RF_CLK1";
constexpr unsigned kPmuPowerUpDelayMs = 50;        // pwrseq-qcom-wcn (wcn7850/peach): after WLAN_EN high (kMainline/kPagani)
constexpr unsigned kWlanEnDelayUs = 1000;          // cnss2 WLAN_ENABLE_DELAY after selecting wlan_en_active

// ---------------------------------------------------------------------------------------------------------------------
// 9. AOSS QMP messages (message RAM 0xc300000, Sm8750AossQmp). cnss sends the DT's pdc_init_table at boot; after the QMI
//    target capability it sends OL-CPR votes computed from the firmware's PMU parameters:
//    wake = roundup(fw_wake_mV, 4) - 16 + 30 (TCS up, "upval"), sleep = roundup(fw_sleep_mV, 4) - 16 + 10 (down, "dwnval"),
//    message "{class: wlan_pdc, ss: <pdcSs>, res: <rail>.v, upval|dwnval: <mV>}". pagani-linux sends fixed values captured
//    from Android. Only the PDC's handling of the chip's own sleep requests depends on these; the APPS RPMh votes of
//    section 8 keep the rails up without them (sheng sent none).
// ---------------------------------------------------------------------------------------------------------------------
constexpr const char *kPdcInit[] = {
    "{class: wlan_pdc, ss: rf, res: s5f.m, enable: 1}",
    "{class: wlan_pdc, ss: rf, res: s5f.v, enable: 1}",
    "{class: wlan_pdc, ss: rf, res: s5f.v, upval: 876}",
    "{class: wlan_pdc, ss: rf, res: s5f.v, dwnval: 876}",
};
constexpr const char *kPdcOlCprPagani[] = {   // kPagani: what pagani-linux sends after the four above
    "{class: wlan_pdc, ss: rf, res: s5f.v, upval: 814}",
    "{class: wlan_pdc, ss: bb, res: s4d.v, upval: 822}",
    "{class: wlan_pdc, ss: rf, res: s7i.v, upval: 1314}",
    "{class: wlan_pdc, ss: rf, res: s3g.v, upval: 1866}",
};
constexpr const char *kPdcDisableOnChipPmic = "{class: wlan_pdc, ss: rf, res: pdc, enable: 0}";  // only for on-chip-PMIC boards

// ---------------------------------------------------------------------------------------------------------------------
// 10. RPMh (apps RSC DRV-2) and interconnect.
// ---------------------------------------------------------------------------------------------------------------------
constexpr uint64_t kRscDrv2 = 0x16520000;          // sheng (SM8550): 0x17a20000
constexpr uint32_t kRscTcsBase = 0xd00, kRscTcsStride = 0x2a0;
constexpr uint64_t kCmdDb = 0x81c60000;
constexpr uint32_t kCmdDbBytes = 0x20000;
// PCIe DMA path (mainline icc sm8750): xm_pcie3 (pcie_noc 0x16c0000) -> qns_pcie_mem_noc [BCM SN4] -> gem_noc qnm_pcie
// [BCM SH1] -> llcc -> ebi [BCM MC0 keepalive, ACV]. sheng voted SN7, the SM8550 name of the same hop.
constexpr const char *kPcieNocBcm = "SN4";
constexpr const char *kPathBcms[] = {"SN4", "SH1", "MC0", "ACV"};
// cnss qcom,bus-bw-cfg (KBps avg/peak) per level, path 0 = pcie_to_memnoc, path 1 = memnoc_to_ddr.
struct BwLevel {const char *name; uint32_t avg0, peak0, avg1, peak1;};
constexpr BwLevel kBusBw[] = {
    {"none", 0, 0, 0, 0},                    {"idle", 2250, 800000, 2250, 2188800},
    {"low", 7500, 800000, 7500, 2188800},     {"medium", 30000, 800000, 30000, 2188800},
    {"high", 100000, 800000, 100000, 2188800}, {"very-high", 175000, 3224000, 175000, 6220800},
    {"ultra-high", 312500, 3224000, 312500, 8368000}, {"super-high", 587500, 4264000, 587500, 12800000},
    {"low-latency", 7500, 1600000, 7500, 2188800},
};

// ---------------------------------------------------------------------------------------------------------------------
// 11. SMMU (apps_smmu 0x15000000, 1 MiB). The controller maps a requester to a stream through PARF's BDF-to-SID hash table
//     (CRC-8, polynomial 0x07, MSB first, over the BDF's high then low byte) once BDF_TO_SID_CFG bit 0 (bypass) is clear.
// ---------------------------------------------------------------------------------------------------------------------
constexpr uint64_t kAppsSmmu = 0x15000000;
constexpr uint32_t kAppsSmmuBytes = 0x100000;
constexpr uint16_t kSidRootPort = 0x1400, kSidEndpoint = 0x1401;      // iommu-map, and qcom,smmu-sid-base = <0x1400>
constexpr uint8_t kBdfHashPoly = 0x07;
constexpr bool kDmaCoherent = true;                // pcie0 dma-coherent (kDevice + kMainline); cache maintenance stays harmless
constexpr uint64_t kWlanIovaStart = 0x18000000, kWlanIovaEnd = 0xb0010000;   // Android's WLAN IOVA geometry (cnss_audio_iommu_group0)
constexpr uint64_t kWlanIovaIpa = 0xb0000000;      // cnss reg "smmu_iova_ipa" (0x10000): IPA/direct-link window, not needed
constexpr uint32_t kCnssCmaBytes = 0x2000000;      // cnss_wlan_region: 32 MiB reusable pool, 4 MiB aligned, below 4 GiB
// What the chip has been seen to work with: cnss sets 36 bits but sits behind stage 1 of the SMMU, IOVAs 0x18000000..0xb0000000; ath12k
// keeps its streaming mask at 36 bits and its coherent mask (the BHI buffer, the MHI contexts and rings, the CE/HAL rings) at 32;
// sheng's window is a 32-bit IOVA. No host has given this chip a coherent address above 4 GiB, and all of XNU's memory is (from
// 0x8c0000000), so the DMA window takes low memory outside XNU's window, as the speaker buffer and sheng's period buffer do: UEFI's
// conventional DXE heap (0xbe360000..0xd8000000, HLOS memory in the devicetree, kDevice), with the bypass stream bus = physical.
constexpr unsigned kDmaBitsCnss = 36, kDmaBitsAth12kStreaming = 36, kDmaBitsAth12kCoherent = 32;
constexpr uint64_t kDmaLowBase = 0xc0000000;       // 2 MiB aligned; pagani-wifi-dmapa moves it, 0: XNU memory above 4 GiB (unproven)

// ---------------------------------------------------------------------------------------------------------------------
// 12. The peach chip seen through BAR0 (ath12k-style host). Register offsets are chip addresses; those under 0x80000 are
//     direct in BAR0, higher ones go through the remap window: write 0x40000000 | ((addr >> 19) & 0x3f) to the window
//     register, then access BAR0 + 0x80000 + (addr & 0x7ffff). The MHI block (0x1e0e100..0x1e0effc) is at BAR0 + 0.
// ---------------------------------------------------------------------------------------------------------------------
namespace chip {
constexpr uint32_t kWindowReg = 0x3278;            // peach (cnss PEACH_PCIE_REMAP_BAR_CTRL_OFFSET; = QCC2072). WCN7850/sheng: 0x310c
constexpr uint32_t kWindowEnable = 0x40000000, kWindowShift = 19, kWindowValueMask = 0x3f, kWindowStart = 0x80000, kWindowRange = 0x7ffff;
constexpr uint32_t kBar0BytesExpected = 0x200000;  // kUnknown: read in the morning (sysfs resource); 2 MiB on WCN7850
// Chip address map (fw-api hw/peach/v1 msmhwiobase.h / wcss_seq_hwiobase.h; same bases as kiwi = WCN7850)
constexpr uint32_t kSramStart = 0x01400000, kSramEnd = 0x01868000;
constexpr uint32_t kTcsrSocHwVersion = 0x01b00000;  // major = bits 11:8 (2 = peach_v2), minor = bits 7:4 (read through the window)
constexpr uint32_t kCeBase = 0x01b80000, kUmacBase = 0x00a00000, kPcieWrapper = 0x01e00000, kGccChip = 0x01e40000, kWlaon = 0x01f80000;
// ath12k_pci_power_up registers (WCN7850 values, peach inherits the same PCIe-wrapper/WLAON map):
constexpr uint32_t kParfLtssm = 0x01e081b0, kParfLtssmValue = 0x111, kIntAllClear = 0x01e08228;
constexpr uint32_t kQfpromPwrCtrl = 0x01f8031c, kQfpromVdd4Blow = 1u << 2;
constexpr uint32_t kQ6Cookie = 0x01f80500, kWarmSwEntry = 0x01f80504, kResetCause = 0x01f8060c;   // reset cause: read-clear
constexpr uint32_t kSocGlobalReset = 0x3008, kSocGlobalResetBit = 1u << 0, kSocWake = 0x3004, kScratch0 = 0x4040;
// GCC_PCIE_HOT_RST (ath12k writes bit 4): WCN7850 0x1e40304; ath12k's QCC2072 table 0x1e65304, which pagani-linux uses for
// peach. Not confirmed for peach; cnss2 never writes it. Keep it optional (off by default).
constexpr uint32_t kHotResetWcn7850 = 0x01e40304, kHotResetQcc2072 = 0x01e65304, kHotResetValue = 0x10;
// Boot debug (cnss2 reg.h, peach)
constexpr uint32_t kPblBootstrapStatus = 0x01a10008, kPblLogSram = 0x01403640, kPblLogBytes = 40, kSblLogMaxBytes = 0x1000;
constexpr uint32_t kBhiErrDbg2 = 0x01e0e238, kBhiErrDbg3 = 0x01e0e23c, kMhiTimeLow = 0xa28, kMhiTimeHigh = 0xa2c;
constexpr uint32_t kWcssStatusForDebugLow = 0x01e03284, kWcssStatusForDebugHigh = 0x01e03280;
}  // namespace chip

// ---------------------------------------------------------------------------------------------------------------------
// 13. MHI. cnss2 (this phone): sbl_size 512 KiB, seg_len 512 KiB, fbc_download, tme_supported_image. The firmware file is
//     two images: the SBL in its first 512 KiB (sent by BHI) and a second ELF at 512 KiB that BHIe loads. Rule (vendor MHI
//     tme_supported_image, and Linux v7.0.14's generic check): if the bytes at sbl_size start with "\177ELF", the BHIe
//     vector table describes image[sbl_size .. end], else the whole image (WCN7850, sheng). Ath12kWifi's MHI (v7.0.14)
//     already applies the rule; sheng's own MhiBoot (stage 3/4 path) does not.
// ---------------------------------------------------------------------------------------------------------------------
namespace mhi {
constexpr uint32_t kSblBytes = 512 * 1024, kSegBytes = 512 * 1024;
constexpr uint32_t kRddmBytes = 0x780000;          // qcom,wlan-ramdump-dynamic (= ath12k WCN7850 rddm_size)
constexpr unsigned kTimeoutMsCnss = 10000, kTimeoutMsAth12k = 2000;
enum Dir : int8_t {kFromDevice = -1, kBoth = 0, kToDevice = 1};
struct Channel {uint8_t num; const char *name; uint16_t elements; uint8_t eventRing; Dir dir; bool autoQueue;};
// cnss_mhi_config_no_diag (DIAG is not supported from peach on); ath12k WCN7850 keeps only IPCR 20/21 (64 elements, ring 1).
constexpr Channel kChannelsCnss[] = {
    {0, "LOOPBACK", 32, 1, kToDevice, false}, {1, "LOOPBACK", 32, 1, kFromDevice, false},
    {20, "IPCR", 64, 1, kToDevice, false},    {21, "IPCR", 64, 1, kFromDevice, true},
    {50, "ADSP_0", 64, 3, kBoth, false},      {51, "ADSP_1", 64, 3, kBoth, false},   // satellite (DRV) channels, not needed
    {70, "ADSP_2", 64, 3, kBoth, false},      {71, "ADSP_3", 64, 3, kBoth, false},
};
struct EventRing {uint16_t elements; uint8_t msi; const char *kind; bool clientManaged;};
constexpr EventRing kEventRingsCnss[] = {{32, 1, "ctrl", false}, {256, 2, "data", false}, {32, 1, "bw-scale", false}, {256, 2, "offload", true}};
constexpr EventRing kEventRingsAth12k[] = {{32, 1, "ctrl", false}, {256, 2, "data", false}};   // = sheng's two rings
}  // namespace mhi

// ---------------------------------------------------------------------------------------------------------------------
// 14. QMI (WLFW service 0x45, instance 1 on this phone: cnss WLFW_SERVICE_INS_ID_V01 1 = ath12k's WCN7850 instance).
// ---------------------------------------------------------------------------------------------------------------------
namespace qmi {
constexpr uint32_t kService = 0x45, kInstance = 1, kClientId = 0x4b4e454c;
enum Msg : uint16_t {
    kIndRegister = 0x20, kWlanMode = 0x22, kWlanCfg = 0x23, kCap = 0x24, kBdfDownload = 0x25, kCalReport = 0x26,
    kHostCap = 0x34, kRequestMemInd = 0x35, kRespondMem = 0x36, kFwMemReadyInd = 0x37, kFwInitDoneInd = 0x38, kM3Info = 0x3c,
    kCalDoneInd = 0x3e, kQdssTraceConfigDownload = 0x44, kAuxUcInfo = 0x5a, kTmeLiteInfo = 0x5b, kSoftSkuInfo = 0x60,
};
// host_cap feature_list bits (CNSS_*_V01)
constexpr uint64_t kFeatDrv = 1ull << 1, kFeatWlanEn = 1ull << 2, kFeatQdssCfgMiss = 1ull << 3, kFeatPerstNoPull = 1ull << 4;
constexpr uint64_t kFeatAuxUc = 1ull << 6;
constexpr uint64_t kFeaturesPagani = kFeatDrv | kFeatWlanEn | kFeatAuxUc;          // pagani-linux's peach entry
constexpr uint64_t kFeaturesWcn7850 = kFeatQdssCfgMiss | kFeatPerstNoPull;        // ath12k WCN7850
enum TmeFile : uint32_t {kTmePatch = 0, kTmeOemFuse = 1, kTmeRpr = 2, kTmeDpr = 3};
// cnss2's order after FW_MEM_READY on peach: target cap -> TME patch (0x5b, type 0) -> [OEM fuse on request] ->
// soft-SKU license (0x60, if the file exists) -> [HDS] -> REGDB (0x25) -> BDF (0x25, ELF) -> M3 = phy ucode (0x3c) ->
// AUX ucode (0x5a, when the firmware reports aux support in its caps) -> QDSS trace config (0x44). FW_READY then a cold-boot
// calibration pass (cal report 0x26, wlan mode CALIBRATION, wait CAL_DONE, mode OFF) when qcom,wlan-cbc-enabled (set here),
// then mission mode. pagani-linux: TME and AUX failures non-fatal; it sends a null M3 (m3_support = 0) because loading
// phy_ucode as M3 faulted there, against cnss2 which sends phy_ucode20.elf through M3_INFO (kUnknown which one holds).
}  // namespace qmi

// ---------------------------------------------------------------------------------------------------------------------
// 15. Firmware files. cnss2 requests "peach/<name>" (qcom,same-dt-multi-dev sets the prefix) unless noted; peach_v2 picks
//     the "20"/"_v2" names. Location on this phone (kDevice, morning facts of 2026-10-31 and dmesg): every file below is on the modem_a
//     FAT (LUN 4, first 4 KiB block 4870, 89600 blocks; Android /vendor/firmware_mnt), "image/peach/<name>" or "image/<name>", read back
//     with WifiFirmware.c from a copy of the real volume, sha256 equal. But Android loads the BDF and the regulatory database from
//     /odm/etc/wifi/peach (erofs in super: out of reach here), different bytes from modem_a's, so those two are looked for in the RAM Data
//     seed first (WifiFirmware.h). tmel_peach_20.elf is loaded and refused (QMI result 1, error 83) and Android goes on; there is no
//     cnss_softsku_peach.pfm and no qdss_trace_config_perf_v2.cfg on this phone.
// ---------------------------------------------------------------------------------------------------------------------
enum Transport : uint8_t {kBhi = 1, kQmiBdf, kQmiRegdb, kQmiM3, kQmiAux, kQmiTme, kQmiSku, kQmiQdss};
struct FwFile {
    const char *ath12kName;   // what an ath12k-style stack asks fw_get for (basename)
    const char *androidName;  // what cnss2 requests (relative to the firmware search path)
    Transport transport; bool required; Src src;
};
constexpr FwFile kFirmware[] = {
    {"amss.bin",       "peach/amss20.bin",                    kBhi,      true,  kVendor},  // fallback name "amss20.bin"
    {"board.bin",      "peach/bdwlan.elf",                    kQmiBdf,   true,  kVendor},  // OPLUS: board_id 0xff -> bdwlan.elf (CN), bdwlan.b0i/b0e/b0a (IN/EU/US),
                                                                                            // GF chips bdwlang.*; board_id < 0xff -> bdwlan.e<id>. /odm's (125328 bytes) first
    {"regdb.bin",      "peach/regdb.bin",                     kQmiRegdb, true,  kVendor},
    {"m3.bin",         "peach/phy_ucode20.elf",               kQmiM3,    false, kVendor},  // see qmi note: cnss sends it, pagani-linux does not
    {"aux_ucode.bin",  "peach/aux_ucode20.elf",               kQmiAux,   false, kVendor},
    {"tmel_peach_20.elf", "tmel_peach_20.elf",                kQmiTme,   true,  kVendor},  // no "peach/" prefix
    {"qdss_trace_config_v2.cfg", "peach/qdss_trace_config_perf_v2.cfg", kQmiQdss, false, kVendor},  // fallback peach/qdss_trace_config_v2.cfg
    {"cnss_softsku_peach.pfm", "cnss_softsku_peach.pfm",      kQmiSku,   false, kVendor},  // no prefix; optional
    {"peach_sec.dat",  "peach/peach_sec.dat",                 kQmiTme,   false, kVendor},  // TME OEM fuse, only on request
    {"peach_rpr.bin",  "peach/peach_rpr.bin",                 kQmiTme,   false, kVendor},
    {"peach_dpr.bin",  "peach/peach_dpr.bin",                 kQmiTme,   false, kVendor},
};
constexpr const char *kFatDirs[] = {"image/peach", "image"};
constexpr const char *kFirmwarePartitions[] = {"modem_a", "modem_b"};

// ---------------------------------------------------------------------------------------------------------------------
// 16. ath12k-style hardware parameters for peach (kPagani, against ath12k's WCN7850 / QCC2072 entries; the register layout
//     claims are checked against fw-api hw/peach/v1: REO2SW1 ring base 0x500, SW2TCL1 0x918, REO cmd 0x2a8, MISC_CTL 0xba0,
//     TCL status 0xd50 = QCC2072's, while WCN7850's are 0x4e4/0x900/0x28c/0xb7c/0xd38).
// ---------------------------------------------------------------------------------------------------------------------
struct Ath12kParams {
    const char *name; const char *fwDir; uint32_t boardBytes, calOffset; bool auxUcode; uint32_t qmiInstance; bool internalSleepClock;
    const char *halRegs; const char *rxDesc; uint8_t ceCount; const char *ceConfig; const char *mhiConfig; uint32_t rddmBytes;
};
constexpr Ath12kParams kPeachParams = {
    "wcn7860 hw2.0", "WCN7860/hw2.0", 256 * 1024, 256 * 1024, true, 1, true,
    "qcc2072", "qcc2072", 9, "wcn7850", "wcn7850", 0x780000,
};
// qca-wifi-host-cmn on this phone: peach uses the KIWI (= WCN7850) CE config (9 CEs) and service map, shadow registers v3,
// and compact RX TLVs (rx_msdu_end_compact), consistent with the table above.

// ---------------------------------------------------------------------------------------------------------------------
// 17. Boot arguments that keep the unknowns overridable (PaganiWifi; everything off by default). The stage numbers are the ones
//     PaganiWifi.cpp runs (README.md): 0 off, 1 read-only survey, 2 power (rails, pins, GDSCs, clocks), 3 link (PHY, PARF, PERST#,
//     LTSSM, endpoint ID), 4 endpoint (SMMU stream, BAR0, MHI/BHI registers), 5 firmware + own MHI boot to mission mode, 6 the
//     Ath12kWifi stack and the network service. Each stage includes the ones before it.
// ---------------------------------------------------------------------------------------------------------------------
namespace bootarg {
constexpr const char *kStage = "pagani-wifi";            // 0..6 as above
constexpr const char *kMaxGen = "pagani-wifi-gen";       // cap the link speed (2 until cx/mx >= NOM is voted)
constexpr const char *kFwPartition = "pagani-wifi-fwpart";  // default modem_a, then modem_b
constexpr const char *kFwDir = "pagani-wifi-fwdir";      // default image/peach, then image
constexpr const char *kBdf = "pagani-wifi-bdf";          // BDF file name override
constexpr const char *kM3 = "pagani-wifi-m3";            // 0 null M3 (pagani-linux), 1 phy_ucode20.elf (cnss2)
constexpr const char *kChipReset = "pagani-wifi-chipreset";  // bit mask of the ath12k power-up writes (default 0: none, as cnss2)
constexpr const char *kPdc = "pagani-wifi-pdc";          // 1 send kPdcInit, 2 also the OL-CPR values
constexpr const char *kMsi = "pagani-wifi-msi";          // 0 poll, 1 iMSI on SPI 141
constexpr const char *kCbc = "pagani-wifi-cbc";          // cold-boot calibration pass (default 1, as cnss)
constexpr const char *kTme = "pagani-wifi-tme";          // TME-Lite patch download (default 1, as cnss)
constexpr const char *kQdss = "pagani-wifi-qdss";        // 0 advertise QDSS_CFG_MISS (ath12k), 1 download the QDSS config (cnss)
constexpr const char *kAutoJoin = "pagani-wifi-aj";      // 0: Settings joins; 1 (default): join.txt of the firmware sources
constexpr const char *kDmaMiB = "pagani-wifi-dma";       // DMA window MiB (default 48)
constexpr const char *kDmaPa = "pagani-wifi-dmapa";      // DMA window physical address (default kDmaLowBase; 0: XNU memory above 4 GiB)
constexpr const char *kSleepClk = "pagani-wifi-sleepclk"; // 0: never write the PMK8550 gpio3 sleep clock pin
constexpr const char *kStopAfter = "pagani-wifi-stop";   // stop after this step number of the stage (debugging; 0 none)
constexpr const char *kLog = "pagani-wifi-log";          // low 3 bits: deepest stack log level in the boot log (default 6, 7: debug lines,
                                                         // rate-limited); +8: a firmware crash's RAM dump to the UFS log's export area
}  // namespace bootarg

}  // namespace pagani_wifi
