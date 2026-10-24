// SPDX-License-Identifier: BSD-2-Clause-Patent
// SM8550 GCC registers used by UFS, transcribed as facts from the public
// Linux gcc-sm8550.c clock driver. No write is performed by NativeUfs yet.
#ifndef NATIVE_UFS_GCC_UFS_H
#define NATIVE_UFS_GCC_UFS_H

// Physical windows from the sheng board DT (clock-controller@100000, ufshc@1d84000).
#define GCC_BASE                      0x00100000ULL
#define UFS_HC_BASE                   0x01d84000ULL
#define UFS_HC_SIZE                   0x3000ULL

#define GCC_UFS_PHY_GDSCR             0x77004
#define GCC_UFS_MEM_PHY_GDSCR         0x9e000

#define GCC_UFS_PHY_AXI_ENABLE        0x77018
#if defined(NATIVE_UFS_SM8750) && NATIVE_UFS_SM8750
// PKX110 live.dtb and OnePlus gcc-sun.c (745702d5); these are read-only probes.
#define GCC_UFS_PHY_AHB_ENABLE        0x77028
#define GCC_UFS_PHY_TX_SYMBOL0_ENABLE 0x7702c
#define GCC_UFS_PHY_RX_SYMBOL0_ENABLE 0x77030
#define GCC_UFS_PHY_UNIPRO_CORE_ENABLE 0x7706c
#define GCC_UFS_PHY_ICE_CORE_ENABLE   0x7707c
#define GCC_UFS_PHY_PHY_AUX_ENABLE    0x770bc
#define GCC_UFS_PHY_RX_SYMBOL1_ENABLE 0x770d8
#define GCC_AGGRE_UFS_PHY_AXI_ENABLE  0x770f0
#define UFS_ROOT_PARTITION            "unixnu"
#else
#define GCC_UFS_PHY_AHB_ENABLE        0x77024
#define GCC_UFS_PHY_TX_SYMBOL0_ENABLE 0x77028
#define GCC_UFS_PHY_RX_SYMBOL0_ENABLE 0x7702c
#define GCC_UFS_PHY_UNIPRO_CORE_ENABLE 0x77068
#define GCC_UFS_PHY_ICE_CORE_ENABLE   0x77074
#define GCC_UFS_PHY_PHY_AUX_ENABLE    0x770b0
#define GCC_UFS_PHY_RX_SYMBOL1_ENABLE 0x770cc
// bus_aggr_clk in the board DT: gcc_aggre_ufs_phy_axi_clk, halt/enable 0x770e4, HW clock gating bit1
#define GCC_AGGRE_UFS_PHY_AXI_ENABLE  0x770e4
#define UFS_ROOT_PARTITION            "userdata"
#endif

static inline bool ufsBusClocksOn(unsigned gdsc, unsigned axi, unsigned ahb, unsigned aggre)
{
  // HW-controlled branches can report CLK_OFF while idle and wake on access.
  auto on = [](unsigned value) { return (value & 3) && (!(value & 0x80000000U) || (value & 2)); };
  return (gdsc & 0x80000000U) && on(axi) && on(ahb) && on(aggre);
}

#define GCC_UFS_BRANCH_ENABLE_BIT     0x1U

// gcc_ufs_phy_ahb_clk and gcc_aggre_ufs_phy_axi_clk set hwcg_bit=1 in gcc-sm8550.c, so CLK_OFF=1 on
// an idle branch is normal (hardware 2026-10-17 read ahb=80000003).
// The reset and RPMh/TCSR/QMP-PHY programming are not represented here: their
// semantics are not qualified and NativeUfs must not write them speculatively.
#endif
