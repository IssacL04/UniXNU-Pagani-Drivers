/* SPDX-License-Identifier: MIT */
/* A830Regs.h 的主机测试（C11 与 C++17 同一份源码）。
 *  1. 第一个包含 A830Regs.h：证明它不依赖任何前置头文件；头内的编译期断言随之全部生效。
 *  2. 再包含平台层头文件：证明宏名互不冲突，并核对两边重复出现的事实（SCM 功能号、RPMh 票、档位表）。
 *  3. 运行期把约 150 个寄存器偏移与 docs/inputs/regs/ 里的 Mesa 导出逐项比对（名字、偏移、reg64、数组长度与步长），
 *     并检查同一地址空间里没有两个寄存器重叠、每个寄存器都落在自己的窗口里。
 * run-host-tests.py 以 C11 编译本文件；C++ 编译命令：
 *   xcrun clang++ -x c++ -std=c++17 -O2 -Wall -Wextra -Werror -fsanitize=address,undefined tests/test_a830regs.c -o <out>
 * 可选参数：docs/inputs/regs 目录（默认按 __FILE__ 推算）。 */
#include "../A830Regs.h"
#include "../platform/sm8750/Sm8750GpuPower.h"
#include "../platform/sm8750/Sm8750Rpmh.h"
#include "../platform/sm8750/Sm8750Scm.h"
#include "../platform/sm8750/Sm8750SocInfo.h"
#include "../platform/sm8750/Sm8750GpuLevels.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- 与平台层的约定一致（编译期） ---- */
A830_STATIC_ASSERT(SM8750_SCM_GPU_INIT_REGS == 0x42002801u && SM8750_SCM_CP_SMMU_APERTURE == 0x42000C1Bu &&
                   SM8750_SCM_PAS_INIT_IMAGE == 0x42000201u && SM8750_SCM_PAS_MEM_SETUP == 0x42000202u &&
                   SM8750_SCM_PAS_AUTH_AND_RESET == 0x42000205u && SM8750_SCM_IS_CALL_AVAIL == 0x42000601u &&
                   SM8750_SCM_WAITQ_RESUME == 0x42002402u && SM8750_SCM_WAITQ_GET_WQ_CTX == 0xC2002403u,
                   "Sm8750Scm.h function ids are the ARCH 4.4.1 values A830Regs.h defers to");
A830_STATIC_ASSERT(A830_SCM_GPU_INIT_REQ == ((uint32_t)Sm8750ScmGpuAlwaysEn | (uint32_t)Sm8750ScmGpuTsenseEn) &&
                   A830_ZAP_PAS_ID == (uint32_t)Sm8750ScmGpuPasId, "SCM request bits and PAS id");
A830_STATIC_ASSERT(A830_CN0_VOTE_ON == (uint32_t)Sm8750RpmhCn0On && A830_CN0_VOTE_OFF == (uint32_t)Sm8750RpmhCn0Off &&
                   A830_SH0_BUSWIDTH == (uint32_t)Sm8750RpmhSh0BusWidth && A830_MC0_BUSWIDTH == (uint32_t)Sm8750RpmhMc0BusWidth &&
                   A830_ACV_GPU_PERFMODE == (uint32_t)Sm8750RpmhAcvGpuPerfmode &&
                   A830_ACV_PERFMODE_KBPS == (uint32_t)Sm8750RpmhAcvPerfmodeKbps &&
                   A830_ACV_HLOS_ENABLE == (uint32_t)Sm8750RpmhAcvHlosEnable && A830_BCM_VOTE_MAX == (uint32_t)Sm8750RpmhBcmVoteMask &&
                   A830_DDR_BOOT_LEVEL == (uint32_t)Sm8750RpmhDdrDefaultLevel && A830_ARC_LOW_SVS == (uint32_t)Sm8750RpmhLowSvs &&
                   SM8750_CMD_DB_BYTES == (uint32_t)Sm8750CmdDbBytes, "RPMh constants agree with Sm8750Rpmh.h");
A830_STATIC_ASSERT(SM8750_QFPROM_GPU_SPEED_BIN == (uint32_t)Sm8750SpeedBinOffset &&
                   SM8750_GPU_SPEED_BIN_MASK == (1u << Sm8750SpeedBinBits) - 1u && SM8750_SMEM_BYTES == (uint32_t)Sm8750SmemBytes &&
                   SM8750_QFPROM_BYTES == (uint32_t)Sm8750QfpromBytes, "SoC info windows agree with Sm8750SocInfo.h");
A830_STATIC_ASSERT(SM8750_DT_SMEM_PHYS == SM8750_SMEM_PHYS && SM8750_DT_QFPROM_PHYS == SM8750_QFPROM_PHYS &&
                   SM8750_DT_SMEM_BYTES == SM8750_SMEM_BYTES && (uint32_t)Sm8750DtDdrLevels == A830_DDR_LEVELS &&
                   (uint32_t)Sm8750DtGpuCommonCount == A830_GMU_COMMON_LEVELS && (uint32_t)Sm8750DtScmWaitqIntid == A830_IRQ_SCM_WAITQ &&
                   (uint32_t)Sm8750DtGpuCommonInitial + 1u == A830_GMU_INITIAL_LEVEL, "generated board tables agree");
/* Mesa A6XX_CX_MISC 域（窗口内 dword）对照 KGSL 的 GPU 空间偏移 */
A830_STATIC_ASSERT(A830_CX_MISC_REG_BYTE(A8XX_CX_MISC_TCM_RET_CNTL) == 4u * 0x039u &&
                   A830_CX_MISC_REG_BYTE(A8XX_CX_MISC_GFX_PWR_CLK_STATUS) == 4u * 0x083u &&
                   A830_CX_MISC_REG_BYTE(A8XX_CX_MISC_SW_FUSE_VALUE) == 4u * 0x400u &&
                   A830_CX_MISC_REG_BYTE(A8XX_CX_MISC_SW_FUSE_FREQ_LIMIT_STATUS) == 4u * 0x405u, "Mesa CX_MISC offsets");

enum { SpaceGpu, SpaceGmu, SpaceRscc };
static const char *const kFiles[2] = {"a8xx_regs.txt", "a8xx_gmu_regs.txt"};

typedef struct {
    int space;              /* SpaceGpu: a8xx_regs.txt；SpaceGmu、SpaceRscc: a8xx_gmu_regs.txt */
    const char *mesa;       /* Mesa 导出里的名字 */
    uint32_t value;         /* 本头文件的偏移 */
    uint32_t delta;         /* 本头文件偏移 − Mesa 偏移（只有 RSCC TCS 状态为 4，见 A8XX_RSCC_MESA_TCS_DELTA） */
    uint32_t count, stride; /* 数组：Mesa 的 [count x stride s]；0 = 单个寄存器 */
    uint32_t span;          /* 占用的 dword 数（reg64 为 2） */
    int is64;               /* Mesa 标为 reg64 */
    const char *macro;
} RegCheck;

#define R(n)                {SpaceGpu, #n, A8XX_##n, 0u, 0u, 0u, 1u, 0, "A8XX_" #n}
#define R64(n)              {SpaceGpu, #n, A8XX_##n, 0u, 0u, 0u, 2u, 1, "A8XX_" #n}
#define RA(n, c, s)         {SpaceGpu, #n, A8XX_##n(0), 0u, c, s, (c) * (s), 0, "A8XX_" #n "(0)"}
#define G(m, ours)          {SpaceGmu, m, ours, 0u, 0u, 0u, 1u, 0, #ours}
#define RS(m, ours, d)      {SpaceRscc, m, ours, d, 0u, 0u, 1u, 0, #ours}

static const RegCheck kChecks[] = {
    /* RBBM */
    R(RBBM_GBIF_HALT), R(RBBM_GBIF_HALT_ACK), R(RBBM_STATUS), R(RBBM_STATUS1), R(RBBM_GFX_STATUS),
    R(RBBM_INTERFACE_HANG_INT_CNTL), R(RBBM_INT_CLEAR_CMD), R(RBBM_INT_0_MASK), R(RBBM_INT_0_STATUS),
    R(RBBM_SW_FUSE_INT_STATUS), R(RBBM_SW_FUSE_INT_MASK), R(RBBM_CLOCK_CNTL_GLOBAL), R(RBBM_CGC_GLOBAL_LOAD_CMD),
    R(RBBM_CGC_P2S_TRIG_CMD), R(RBBM_CGC_P2S_STATUS), R(RBBM_CGC_0_PC), R(RBBM_PERFCTR_GPU_BUSY_MASKED),
    R(RBBM_PERFCTR_SRAM_INIT_STATUS), RA(RBBM_PERFCTR_CP, A8XX_RBBM_PERFCTR_CP_COUNT, 2u), R(RBBM_PERFCTR_SRAM_INIT_CMD),
    R(RBBM_PERFCTR_CNTL), R(RBBM_SLICE_PERFCTR_CNTL), R(RBBM_SLICE_INTERFACE_HANG_INT_CNTL),
    R(RBBM_SLICE_PERFCTR_SRAM_INIT_CMD), R(RBBM_SECVID_TRUST_CNTL), R64(RBBM_SECVID_TSB_TRUSTED_BASE),
    R(RBBM_SECVID_TSB_TRUSTED_SIZE), R(RBBM_SECVID_TSB_CNTL),
    /* section 21 (merged module additions) */
    R(RBBM_GFX_STATUS1), R(RBBM_LPAC_STATUS), R(RBBM_GFX_BR_STATUS), R(RBBM_GFX_BV_STATUS), R(RBBM_MISC_STATUS),
    R64(RBBM_SECVID_TSB_STATUS), R(GRAS_A2D_BLT_CNTL), R(GRAS_A2D_DEST_TL), R(GRAS_A2D_DEST_BR), R(RB_CCU_CACHE_CNTL),
    R(RB_A2D_BLT_CNTL), R(RB_A2D_PIXEL_CNTL), R(RB_A2D_DEST_BUFFER_INFO), R64(RB_A2D_DEST_BUFFER_BASE),
    R(RB_A2D_DEST_BUFFER_PITCH), R(RB_A2D_CLEAR_COLOR_DW0), R(SP_A2D_OUTPUT_INFO), R(TPL1_A2D_BLT_CNTL),
    /* CP */
    R64(CP_RB_BASE), R(CP_RB_CNTL), R(CP_RB_RPTR_WR), R64(CP_RB_RPTR_ADDR), R(CP_RB_RPTR), R(CP_RB_WPTR),
    /* Mesa 把 CP_RB_RPTR_ADDR_BV 标成 reg32，KGSL 与 Linux gpu_write64 用 0x808/0x809 两个字：按 64 位占用检查重叠 */
    {SpaceGpu, "CP_RB_RPTR_ADDR_BV", A8XX_CP_RB_RPTR_ADDR_BV, 0u, 0u, 0u, 2u, 0, "A8XX_CP_RB_RPTR_ADDR_BV"},
    R(CP_RB_RPTR_BV), R(CP_SMMU_STREAM_ID_LPAC), R(CP_SQE_CNTL), R64(CP_SQE_INSTR_BASE), R64(CP_AQE_INSTR_BASE_0),
    R64(CP_AQE_INSTR_BASE_1), R(CP_APERTURE_CNTL_HOST), R(CP_APERTURE_CNTL_GMU), R(CP_APERTURE_CNTL_CD),
    R(CP_CP2GMU_STATUS), RA(CP_SCRATCH_GLOBAL, A8XX_CP_SCRATCH_GLOBAL_COUNT, 1u),
    RA(CP_SCRATCH_PIPE, A8XX_CP_SCRATCH_PIPE_COUNT, 1u), R(CP_RL_ERROR_DETAILS_0), R(CP_RL_ERROR_DETAILS_1),
    R(CP_APRIV_CNTL_PIPE), R(CP_PROTECT_CNTL_PIPE), R(CP_PROTECT_STATUS_PIPE),
    RA(CP_PROTECT_GLOBAL, A8XX_CP_PROTECT_GLOBAL_COUNT, 1u), RA(CP_PROTECT_PIPE, A8XX_CP_PROTECT_PIPE_COUNT, 1u),
    R(CP_HW_FAULT_STATUS_PIPE), R(CP_HW_FAULT_STATUS_MASK_PIPE), R(CP_INTERRUPT_STATUS_GLOBAL),
    R(CP_INTERRUPT_STATUS_MASK_GLOBAL), R(CP_INTERRUPT_STATUS_PIPE), R(CP_INTERRUPT_STATUS_MASK_PIPE), R(CP_PIPE_STATUS_PIPE),
    R(CP_SQE_STATUS_PIPE), RA(CP_PERFCTR_CP_SEL, A8XX_CP_PERFCTR_CP_SEL_COUNT, 1u), R64(CP_ALWAYS_ON_COUNTER),
    R(CP_SQE_STAT_ADDR_PIPE), R(CP_SQE_STAT_DATA_PIPE), R64(CP_IB1_BASE), R(CP_IB1_REM_SIZE), R64(CP_IB2_BASE),
    R(CP_IB2_REM_SIZE),
    /* UCHE、GMEM、UBWC */
    R64(UCHE_WRITE_THRU_BASE), R64(UCHE_TRAP_BASE), R(UCHE_CLIENT_PF), R64(UCHE_CCHE_WRITE_THRU_BASE), R64(UCHE_CCHE_TRAP_BASE),
    R64(UCHE_CCHE_GC_GMEM_RANGE_MIN), R64(SP_HLSQ_GC_GMEM_RANGE_MIN), R(GRAS_NC_MODE_CNTL), R(RB_CCU_NC_MODE_CNTL),
    R(RB_GC_GMEM_PROTECT), R(RB_CONTEXT_SWITCH_GMEM_SAVE_RESTORE_ENABLE), R(RB_CMP_NC_MODE_CNTL), R(SP_NC_MODE_CNTL),
    R(TPL1_NC_MODE_CNTL), RA(TPL1_BICUBIC_WEIGHTS_TABLE, A8XX_TPL1_BICUBIC_WEIGHTS_TABLE_COUNT, 1u),
    /* GBIF */
    R(GBIF_CX_CONFIG), R(GBIF_SCACHE_CNTL0), R(GBIF_SCACHE_CNTL1), R(GBIF_QSB_SIDE0),
    {SpaceGpu, "GBIF_QSB_SIDE1", A8XX_GBIF_QSB_SIDE(1), 0u, 0u, 0u, 1u, 0, "A8XX_GBIF_QSB_SIDE(1)"},
    {SpaceGpu, "GBIF_QSB_SIDE2", A8XX_GBIF_QSB_SIDE(2), 0u, 0u, 0u, 1u, 0, "A8XX_GBIF_QSB_SIDE(2)"},
    R(GBIF_QSB_SIDE3), R(GBIF_HALT), R(GBIF_HALT_ACK),
    /* GMU（Mesa 的 GMU 名字按代际混用，这里写出对应关系） */
    G("GMU_CM3_ITCM_START", A8XX_GMU_CM3_ITCM_START), G("GMU_CM3_DTCM_START", A8XX_GMU_CM3_DTCM_START),
    G("GMU_CORE_FW_VERSION", A8XX_GMU_DTCM_CORE_FW_VERSION), G("GMU_ICACHE_CONFIG", A8XX_GMUCX_ICACHE_CONFIG),
    G("GMU_DCACHE_CONFIG", A8XX_GMUCX_DCACHE_CONFIG), G("GMU_SYS_BUS_CONFIG", A8XX_GMUCX_SYS_BUS_CONFIG),
    G("GMU_MRC_GBIF_QOS_CTRL", A8XX_GMUCX_MRC_GBIF_QOS_CTRL), G("GMU_GMU_PWR_COL_KEEPALIVE", A8XX_GMUCX_PWR_COL_KEEPALIVE),
    G("GMU_PWR_COL_PREEMPT_KEEPALIVE", A8XX_GMUCX_PWR_COL_PREEMPTION_KEEPALIVE),
    G("GMU_PWR_CLK_STATUS", A8XX_GMUCX_GFX_PWR_CLK_STATUS), G("GPU_GMU_CX_GMU_RPMH_POWER_STATE", A8XX_GMUCX_RPMH_POWER_STATE),
    G("GPU_GMU_CX_GMU_CX_FAL_INTF", A8XX_GMUCX_CX_FAL_INTF), G("GPU_GMU_CX_GMU_CX_FALNEXT_INTF", A8XX_GMUCX_CX_FALNEXT_INTF),
    G("GMU_CM3_SYSRESET", A8XX_GMUCX_CM3_SYSRESET), G("GMU_CM3_BOOT_CONFIG", A8XX_GMUCX_CM3_BOOT_CONFIG),
    G("GMU_CM3_FW_INIT_RESULT", A8XX_GMUCX_CM3_FW_INIT_RESULT), G("GMU_CM3_CFG", A8XX_GMUCX_CM3_CFG),
    G("GMU_ALWAYS_ON_COUNTER_L", A8XX_GMUCX_AO_COUNTER_LO), G("GMU_ALWAYS_ON_COUNTER_H", A8XX_GMUCX_AO_COUNTER_HI),
    G("GMU_HFI_CTRL_STATUS", A8XX_GMUCX_HFI_CTRL_STATUS), G("GMU_HFI_QTBL_INFO", A8XX_GMUCX_HFI_QTBL_INFO),
    G("GMU_HFI_QTBL_ADDR", A8XX_GMUCX_HFI_QTBL_ADDR), G("GMU_HFI_CTRL_INIT", A8XX_GMUCX_HFI_CTRL_INIT),
    G("GMU_GMU2HOST_INTR_SET", A8XX_GMUCX_GMU2HOST_INTR_SET), G("GMU_GMU2HOST_INTR_CLR", A8XX_GMUCX_GMU2HOST_INTR_CLR),
    G("GMU_GMU2HOST_INTR_INFO", A8XX_GMUCX_GMU2HOST_INTR_INFO), G("GMU_GMU2HOST_INTR_MASK", A8XX_GMUCX_GMU2HOST_INTR_MASK),
    G("GMU_HOST2GMU_INTR_SET", A8XX_GMUCX_HOST2GMU_INTR_SET), G("GMU_HOST2GMU_INTR_CLR", A8XX_GMUCX_HOST2GMU_INTR_CLR),
    G("GMU_HOST2GMU_INTR_RAW_INFO", A8XX_GMUCX_HOST2GMU_INTR_RAW_INFO), G("GMU_GENERAL_8", A8XX_GMUCX_GENERAL_8),
    G("GMU_GENERAL_9", A8XX_GMUCX_GENERAL_9), G("GMU_GENERAL_10", A8XX_GMUCX_GENERAL_10), G("GMU_GENERAL_11", A8XX_GMUCX_GENERAL_11),
    G("GMU_CX_GMU_POWER_COUNTER_ENABLE", A8XX_GMUCX_POWER_COUNTER_ENABLE),
    G("GMU_CX_GMU_POWER_COUNTER_SELECT_XOCLK_0", A8XX_GMUCX_POWER_COUNTER_SELECT_XOCLK_0),
    G("GMU_CX_GMU_POWER_COUNTER_SELECT_XOCLK_1", A8XX_GMUCX_POWER_COUNTER_SELECT_XOCLK_1),
    G("GMU_CX_GMU_POWER_COUNTER_XOCLK_0_L", A8XX_GMUCX_POWER_COUNTER_XOCLK_L_0),
    G("GMU_CX_GMU_POWER_COUNTER_XOCLK_0_H", A8XX_GMUCX_POWER_COUNTER_XOCLK_H_0),
    G("GMU_AO_INTERRUPT_EN", A8XX_GMUAO_AO_INTERRUPT_EN), G("GMU_AO_HOST_INTERRUPT_CLR", A8XX_GMUAO_AO_HOST_INTERRUPT_CLR),
    G("GMU_AO_HOST_INTERRUPT_STATUS", A8XX_GMUAO_AO_HOST_INTERRUPT_STATUS),
    G("GMU_AO_HOST_INTERRUPT_MASK", A8XX_GMUAO_AO_HOST_INTERRUPT_MASK), G("GMU_RSCC_CONTROL_REQ", A8XX_GMUAO_RSCC_CONTROL_REQ),
    G("GMU_RSCC_CONTROL_ACK", A8XX_GMUAO_RSCC_CONTROL_ACK), G("GPU_GMU_AO_GMU_CGC_MODE_CNTL", A8XX_GMUAO_CGC_MODE_CNTL),
    G("GPU_GMU_AO_GMU_CGC_DELAY_CNTL", A8XX_GMUAO_CGC_DELAY_CNTL), G("GPU_GMU_AO_GMU_CGC_HYST_CNTL", A8XX_GMUAO_CGC_HYST_CNTL),
    G("GPU_GMU_AO_GPU_CX_BUSY_STATUS", A8XX_GMUAO_GPU_CX_BUSY_STATUS),
    G("GPU_GMU_AO_GPU_CX_BUSY_STATUS2", A8XX_GMUAO_GPU_CX_BUSY_STATUS2),
    G("GPU_GMU_AO_GPU_CX_BUSY_MASK", A8XX_GMUAO_GPU_CX_BUSY_MASK), G("GMU_AO_AHB_FENCE_CTRL", A8XX_GMUAO_AHB_FENCE_CTRL),
    G("GMU_AHB_FENCE_RANGE_0", A8XX_GMUAO_AHB_FENCE_RANGE_0), G("GMU_AHB_FENCE_STATUS", A8XX_GMUAO_AHB_FENCE_STATUS),
    G("GMU_AHB_FENCE_STATUS_CLR", A8XX_GMUAO_AHB_FENCE_STATUS_CLR),
    G("GMU_RBBM_INT_UNMASKED_STATUS", A8XX_GMUAO_RBBM_INT_UNMASKED_STATUS_SHADOW),
    /* GPU RSCC */
    RS("GPU_RSCC_RSC_STATUS0_DRV0", A8XX_RSCC_RSC_STATUS0_DRV0, 0u), RS("RSCC_SEQ_BUSY_DRV0", A8XX_RSCC_SEQ_BUSY_DRV0, 0u),
    RS("RSCC_TCS0_DRV0_STATUS", A8XX_RSCC_TCS_DRV0_STATUS(0), A8XX_RSCC_MESA_TCS_DELTA),
    RS("RSCC_TCS1_DRV0_STATUS", A8XX_RSCC_TCS_DRV0_STATUS(1), A8XX_RSCC_MESA_TCS_DELTA),
    RS("RSCC_TCS2_DRV0_STATUS", A8XX_RSCC_TCS_DRV0_STATUS(2), A8XX_RSCC_MESA_TCS_DELTA),
    RS("RSCC_TCS3_DRV0_STATUS", A8XX_RSCC_TCS_DRV0_STATUS(3), A8XX_RSCC_MESA_TCS_DELTA),
    RS("RSCC_TCS4_DRV0_STATUS", A8XX_RSCC_TCS_DRV0_STATUS(4), A8XX_RSCC_MESA_TCS_DELTA),
    RS("RSCC_TCS5_DRV0_STATUS", A8XX_RSCC_TCS_DRV0_STATUS(5), A8XX_RSCC_MESA_TCS_DELTA),
    RS("RSCC_TCS6_DRV0_STATUS", A8XX_RSCC_TCS_DRV0_STATUS(6), A8XX_RSCC_MESA_TCS_DELTA),
    RS("RSCC_TCS7_DRV0_STATUS", A8XX_RSCC_TCS_DRV0_STATUS(7), A8XX_RSCC_MESA_TCS_DELTA),
    RS("RSCC_TCS8_DRV0_STATUS", A8XX_RSCC_TCS_DRV0_STATUS(8), A8XX_RSCC_MESA_TCS_DELTA),
    RS("RSCC_TCS9_DRV0_STATUS", A8XX_RSCC_TCS_DRV0_STATUS(9), A8XX_RSCC_MESA_TCS_DELTA),
};
#define CHECK_COUNT (sizeof(kChecks) / sizeof(kChecks[0]))

/* 设计输入 docs/inputs/power-gmu/A830PowerGmuRegs.h 的 RSCC_TCS_DRV0_STATUS 表（KGSL gen8 的值） */
static const uint32_t kInputTcs[A8XX_RSCC_TCS_COUNT] = {
    0x034Au, 0x03F2u, 0x049Au, 0x0542u, 0x05EAu, 0x0692u, 0x073Au, 0x07E2u, 0x088Au, 0x0932u};

typedef struct {
    uint32_t offset, count, stride;
    int is64;
    char name[96];
} Entry;

static int failures;
static void fail(const char *what, const char *detail)
{
    printf("FAIL %s: %s\n", what, detail);
    ++failures;
}

/* "0x00815 CP_SQE_CNTL reg32"、"0x0082a CP_SCRATCH_GLOBAL[4 x stride 1] array"；数组元素行（"[0].REG"）跳过 */
static int parse_line(const char *line, Entry *e)
{
    char *end = NULL;
    unsigned long offset = strtoul(line, &end, 16);
    if (end == line || *end != ' ')
        return 0;
    const char *p = end + 1;
    size_t n = strcspn(p, " [");
    if (n == 0 || n >= sizeof(e->name))
        return 0;
    memcpy(e->name, p, n);
    e->name[n] = '\0';
    e->offset = (uint32_t)offset;
    e->count = e->stride = 0;
    p += n;
    if (*p == '[') {
        unsigned count = 0, stride = 0;
        if (sscanf(p, "[%u x stride %u]", &count, &stride) != 2)
            return 0;
        e->count = count;
        e->stride = stride;
    }
    e->is64 = strstr(p, " reg64") != NULL;
    return 1;
}

static Entry *load(const char *dir, const char *file, size_t *count)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", dir, file);
    FILE *f = fopen(path, "r");
    if (!f) {
        fail("open", path);
        return NULL;
    }
    size_t capacity = 2048, n = 0;
    Entry *entries = (Entry *)calloc(capacity, sizeof(Entry));
    char line[512];
    while (entries && fgets(line, sizeof(line), f)) {
        if (n == capacity)
            break;
        if (parse_line(line, &entries[n]))
            ++n;
    }
    fclose(f);
    *count = n;
    return entries;
}

static void default_dir(char *out, size_t bytes)
{
    const char *file = __FILE__;
    const char *slash = strrchr(file, '/');
    if (!slash)
        snprintf(out, bytes, "../docs/inputs/regs");
    else
        snprintf(out, bytes, "%.*s/../docs/inputs/regs", (int)(slash - file), file);
}

static unsigned check_against_mesa(const char *dir)
{
    Entry *tables[2] = {NULL, NULL};
    size_t counts[2] = {0, 0};
    unsigned matched = 0;
    for (int i = 0; i < 2; ++i) {
        tables[i] = load(dir, kFiles[i], &counts[i]);
        if (tables[i] && counts[i] < 100)
            fail(kFiles[i], "too few register lines");
    }
    for (size_t c = 0; c < CHECK_COUNT; ++c) {
        const RegCheck *k = &kChecks[c];
        int t = k->space == SpaceGpu ? 0 : 1;
        int seen = 0, ok = 0;
        char detail[256];
        detail[0] = '\0';
        for (size_t i = 0; tables[t] && i < counts[t]; ++i) {
            const Entry *e = &tables[t][i];
            if (strcmp(e->name, k->mesa) != 0 || (k->count != 0) != (e->count != 0))
                continue;
            seen = 1;
            if (e->offset + k->delta == k->value && e->count == k->count && e->stride == k->stride && (!k->is64 || e->is64))
                ok = 1;
            else
                snprintf(detail, sizeof(detail), "%s = 0x%05X, Mesa %s = 0x%05X%s [%u x %u]", k->macro, (unsigned)k->value,
                         k->mesa, (unsigned)e->offset, e->is64 ? " reg64" : "", (unsigned)e->count, (unsigned)e->stride);
        }
        if (!seen)
            fail(k->macro, "name not in the Mesa export");
        else if (!ok)
            fail(k->macro, detail);
        else
            ++matched;
    }
    free(tables[0]);
    free(tables[1]);
    return matched;
}

static void check_layout(void)
{
    /* 同一地址空间里互不重叠；每个寄存器落在自己的窗口里；reg64 的 _HI 在下一字 */
    for (size_t a = 0; a < CHECK_COUNT; ++a) {
        const RegCheck *x = &kChecks[a];
        uint32_t last = x->value + x->span - 1u;
        int inside = x->space == SpaceGpu ? A830_GPU_REG_IN_WINDOW(last) :
                     x->space == SpaceGmu ? (A830_GMU_REG_IN_WINDOW(x->value) && A830_GMU_REG_IN_WINDOW(last)) :
                     A830_RSCC_REG_BYTE(last) < A830_RSCC_BYTES;
        if (!inside)
            fail(x->macro, "outside its window");
        for (size_t b = a + 1; b < CHECK_COUNT; ++b) {
            const RegCheck *y = &kChecks[b];
            if (x->space == y->space && x->value < y->value + y->span && y->value < x->value + x->span) {
                char detail[160];
                snprintf(detail, sizeof(detail), "overlaps %s", y->macro);
                fail(x->macro, detail);
            }
        }
    }
    const uint32_t hi_pairs[][2] = {
        {A8XX_CP_RB_BASE, A8XX_CP_RB_BASE_HI}, {A8XX_CP_RB_RPTR_ADDR, A8XX_CP_RB_RPTR_ADDR_HI},
        {A8XX_CP_RB_RPTR_ADDR_BV, A8XX_CP_RB_RPTR_ADDR_BV_HI}, {A8XX_CP_SQE_INSTR_BASE, A8XX_CP_SQE_INSTR_BASE_HI},
        {A8XX_CP_AQE_INSTR_BASE_0, A8XX_CP_AQE_INSTR_BASE_0_HI}, {A8XX_CP_AQE_INSTR_BASE_1, A8XX_CP_AQE_INSTR_BASE_1_HI},
        {A8XX_CP_ALWAYS_ON_COUNTER, A8XX_CP_ALWAYS_ON_COUNTER_HI}, {A8XX_CP_IB1_BASE, A8XX_CP_IB1_BASE_HI},
        {A8XX_CP_IB2_BASE, A8XX_CP_IB2_BASE_HI}, {A8XX_UCHE_WRITE_THRU_BASE, A8XX_UCHE_WRITE_THRU_BASE_HI},
        {A8XX_UCHE_TRAP_BASE, A8XX_UCHE_TRAP_BASE_HI}, {A8XX_UCHE_CCHE_WRITE_THRU_BASE, A8XX_UCHE_CCHE_WRITE_THRU_BASE_HI},
        {A8XX_UCHE_CCHE_TRAP_BASE, A8XX_UCHE_CCHE_TRAP_BASE_HI},
        {A8XX_UCHE_CCHE_GC_GMEM_RANGE_MIN, A8XX_UCHE_CCHE_GC_GMEM_RANGE_MIN_HI},
        {A8XX_SP_HLSQ_GC_GMEM_RANGE_MIN, A8XX_SP_HLSQ_GC_GMEM_RANGE_MIN_HI},
        {A8XX_RBBM_SECVID_TSB_TRUSTED_BASE, A8XX_RBBM_SECVID_TSB_TRUSTED_BASE_HI},
        {A8XX_GMUCX_AO_COUNTER_LO, A8XX_GMUCX_AO_COUNTER_HI},
        {A8XX_GMUCX_POWER_COUNTER_XOCLK_L_0, A8XX_GMUCX_POWER_COUNTER_XOCLK_H_0},
        {A8XX_CX_MISC_AO_COUNTER_LO, A8XX_CX_MISC_AO_COUNTER_HI},
        {A830_SMMU_CB_TTBR0 / 4u, A830_SMMU_CB_TTBR0_HI / 4u}, {A830_SMMU_CB_TTBR1 / 4u, A830_SMMU_CB_TTBR1_HI / 4u},
        {A830_SMMU_CB_FAR / 4u, A830_SMMU_CB_FAR_HI / 4u}, {A830_SMMU_GR0_SGFAR / 4u, A830_SMMU_GR0_SGFAR_HI / 4u},
    };
    for (size_t i = 0; i < sizeof(hi_pairs) / sizeof(hi_pairs[0]); ++i)
        if (hi_pairs[i][1] != hi_pairs[i][0] + 1u)
            fail("reg64", "high word is not the next dword");
    for (unsigned n = 0; n < A8XX_RSCC_TCS_COUNT; ++n)
        if (A8XX_RSCC_TCS_DRV0_STATUS(n) != kInputTcs[n])
            fail("A8XX_RSCC_TCS_DRV0_STATUS", "differs from the power-gmu input table");
    /* 64 位失效寄存器必须 8 字节对齐（单条 str x），32 位的不能被 64 位写跨进相邻字 */
    const uint32_t tlbi64[] = {A830_SMMU_CB_TLBIVA, A830_SMMU_CB_TLBIVAA, A830_SMMU_CB_TLBIVAL, A830_SMMU_CB_TLBIVAAL};
    for (size_t i = 0; i < sizeof(tlbi64) / sizeof(tlbi64[0]); ++i)
        if (tlbi64[i] % 8u)
            fail("SMMU TLBI", "64-bit invalidation register not 8-byte aligned");
}

static void check_values(void)
{
    /* GMU 档 g = 下标 + 1：ACD 有值的档恰好是 HFI ACD 表的 enable_by_level */
    uint32_t acd = 0;
    for (unsigned i = 0; i < (unsigned)Sm8750DtGpuCommonCount; ++i)
        if (Sm8750DtGpuCommonLevels[i].acd)
            acd |= 1u << (i + 1u);
    if (acd != A830_HFI_ACD_ENABLE_BY_LEVEL)
        fail("A830_HFI_ACD_ENABLE_BY_LEVEL", "differs from the common level table");
    const Sm8750GpuLevel *initial = &Sm8750DtGpuCommonLevels[Sm8750DtGpuCommonInitial];
    if (initial->khz != 222000u || initial->bus != A830_GMU_INITIAL_BUS)
        fail("A830_GMU_INITIAL_LEVEL", "initial level is not 222 MHz / bus 2");
    if (Sm8750DtDdrKbps[A830_DDR_BOOT_LEVEL] != A830_DDR_TOP_KBPS || Sm8750DtDdrKbps[7] != A830_ACV_PERFMODE_KBPS ||
        Sm8750DtDdrKbps[6] >= A830_ACV_PERFMODE_KBPS)
        fail("DDR levels", "top level or ACV perfmode threshold differs from the generated table");
    /* HFI 头编码可逆 */
    for (uint32_t seq = 0; seq < 0x1000u; seq += 0x111u) {
        uint32_t h = A830_HFI_HDR(A830_HFI_H2F_GX_BW_PERF_VOTE, A830_HFI_MSG_ACK, A830_HFI_GX_BW_PERF_VOTE_DWORDS, seq);
        if (A830_HFI_HDR_ID(h) != A830_HFI_H2F_GX_BW_PERF_VOTE || A830_HFI_HDR_TYPE(h) != A830_HFI_MSG_ACK ||
            A830_HFI_HDR_DWORDS(h) != A830_HFI_GX_BW_PERF_VOTE_DWORDS || A830_HFI_HDR_SEQ(h) != seq || (h & A830_HFI_HDR_RECORD))
            fail("A830_HFI_HDR", "does not decode back");
    }
    /* SMR 匹配：只看有效项；mask 位不参与比较 */
    for (uint32_t sid = 0; sid < 8u; ++sid) {
        int gmu = A830_SMMU_SMR_COVERS(A830_SMMU_SMR(A830_SID_GMU, 0), sid) ? 1 : 0;
        int any = A830_SMMU_SMR_COVERS(A830_SMMU_SMR(0, 0x7FFF), sid) ? 1 : 0;
        if (gmu != (sid == A830_SID_GMU) || !any || A830_SMMU_SMR_COVERS(A830_SMMU_SMR(0, 0x7FFF) & ~A830_SMMU_SMR_VALID, sid))
            fail("A830_SMMU_SMR_COVERS", "wrong match");
    }
    /* 换算宏与 ARCH 0.3 的例子 */
    if (A830_GMU_REG_BYTE(A8XX_GMUCX_CM3_SYSRESET) != 0x47000u || A830_GPU_REG_PHYS(A8XX_GMUCX_CM3_SYSRESET) != 0x3D7E000ull ||
        A830_GMU_PHYS + A830_GMU_REG_BYTE(A8XX_GMUCX_CM3_SYSRESET) != A830_GPU_REG_PHYS(A8XX_GMUCX_CM3_SYSRESET) ||
        A830_CX_MISC_PHYS + A830_CX_MISC_REG_BYTE(A8XX_CX_MISC_AO_COUNTER_LO) != A830_GPU_REG_PHYS(A8XX_CX_MISC_AO_COUNTER_LO))
        fail("window helpers", "physical address mismatch");
}

int main(int argc, char **argv)
{
    char dir[1024];
    if (argc > 1)
        snprintf(dir, sizeof(dir), "%s", argv[1]);
    else
        default_dir(dir, sizeof(dir));
    unsigned matched = check_against_mesa(dir);
    check_layout();
    check_values();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
#if defined(__cplusplus)
    const char *language = "C++";
#else
    const char *language = "C";
#endif
    printf("PASS: A830Regs.h (%s) standalone with the sm8750 platform headers; %u/%u offsets match docs/inputs/regs "
           "(RSCC TCS +%u as KGSL), no overlaps, windows and design values asserted\n",
           language, matched, (unsigned)CHECK_COUNT, (unsigned)A8XX_RSCC_MESA_TCS_DELTA);
    return 0;
}
