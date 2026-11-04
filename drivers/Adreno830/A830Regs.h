/* SPDX-License-Identifier: MIT */
/*
 * A830Regs.h：A830（Gen8，chip id 0x44050001，DT "qcom,adreno-gpu-gen8-0-1"）/ SM8750 / 一加 13T PKX110
 * 的唯一寄存器头文件。驱动的每个模块（A830Smmu、A830Gmu、A830Hfi、A830Votes、A830Cp、A830Zap、A830Irq、
 * A830Fence、A830Dcvs、Adreno830Start、参数化后的 NativeAdreno* 与主机测试）都从这里取硬件事实。
 *
 * 由下列输入合并、去重、统一命名而来（docs/ARCHITECTURE.md 0.4、2.2；IMPLEMENTATION_PLAN M0）：
 *   docs/inputs/power-gmu/A830PowerGmuRegs.h  电源、时钟、GMU、HFI、RSCC、CX_MISC、RPMh 编码、超时
 *   docs/inputs/perf/a830_perf_regs.h         中断、忙计数、LLCC、DPU、面板
 *   docs/inputs/regs/a8xx_regs.txt、a8xx_gmu_regs.txt（Mesa 寄存器 XML 导出）与 ARCHITECTURE 第 4 节用到的
 *   CP/RBBM/UCHE/GBIF/SMMU/GMU 偏移和取值。
 * 只放硬件事实（地址、偏移、位、取值、编码、超时上限），不含流程代码；流程见 ARCHITECTURE 第 4 节。
 *
 * 来源标注（每组注释里写明）：
 *   [MESA] freedreno 寄存器 XML（MIT）：a6xx.xml、a6xx_gmu.xml、adreno_pm4.xml 的 A8XX 变体
 *   [KGSL] 本机型 KGSL gen8：gen8_reg.h、adreno_gen8*.c、adreno_hfi.h、kgsl_gmu_core.h、adreno-gpulist.h（gen8_0_1）
 *   [LNX]  Linux drm/msm a8xx_gpu.c、a6xx_gmu.c、a6xx_gpu.c、a8xx_catalog.c；iommu/arm-smmu.h；soc/qcom/llcc-qcom.c；dpu1
 *   [CLK]  Linux clk/qcom gpucc-sm8750.c、gcc-sm8750.c、gxclkctl-kaanapali.c
 *   [DT]   本机运行时设备树（/tmp/claude-501/live.dts）
 *   [HW]   本机实测（PKX110）；[SOC] 同 SoC 实测（annibale、hummer）
 *   [ARCH] docs/ARCHITECTURE.md 的取值（括号内为节号）；两个来源冲突时注释写明取哪个、为什么
 *   GPL 源码（KGSL、Linux）只用来核对事实，这里的名字、组织和注释都是自写的。
 *
 * 命名与约定：
 *   A8XX_<名>         GPU 地址空间的 dword 偏移（物理地址 = 0x03D00000 + 4×dword，ARCH 0.3）。GPU 块用 Mesa A8XX
 *                     的名字；GMU 块用 KGSL gen8 的 GMUCX_/GMUAO_ 写法（Mesa 的 GMU 名字按代际混用，A8XX 版本不全）；
 *                     CX_MISC 用 KGSL 的 GPU 空间偏移（0x27800 起）。名字带 _PIPE 的寄存器按管线分组，访问前先写
 *                     A8XX_CP_APERTURE_CNTL_HOST 选管线（ARCH 4.8），用完写回 0。
 *   reg64             <名> 是低 32 位，<名>_HI 是高 32 位（低地址 +1）；SMMU 的 64 位寄存器同理（字节 +4）。
 *   A8XX_RSCC_*       GPU RSCC 块内 dword 偏移（相对 0x03D50000）。
 *   A830_SMMU_*       GPU SMMU（MMU-500，0x03DA0000）块内字节偏移、位与取值。
 *   SM8750_GCC_*、SM8750_GPUCC_*  时钟控制器块内字节偏移。
 *   A830_*            本机/本驱动专有的取值、地址布局、编码、超时；SM8750_* 为 SoC 级窗口与非 GPU 块。
 *   位字段宏           <寄存器宏名>_<字段名>；取值宏 A830_<寄存器>_VALUE 或按用途命名。
 * 电源域：A8XX_RBBM_*、A8XX_CP_*、A8XX_UCHE_*、GRAS/RB/SP/TPL1 属 GX，只能在 OOB_GPU 应答且
 *   GMUCX_GFX_PWR_CLK_STATUS[1:0]==0 之后访问（ARCH 4.7.8），否则同步外部中止；GBIF（0x3C00 起）、GMU、RSCC、
 *   CX_MISC 属 CX，S1 第 9 步之后可访问（ARCH 4.2.2）。
 * 访问宽度：全部 32 位，唯一例外是 SMMU 的 TLBIVA/TLBIVAA/TLBIVAL/TLBIVAAL，必须单条 64 位存储（ARCH 4.5.2）。
 * 只记录、不作判据（ARCH 第 10 节）：sGFSR、CB FSR、CP_RB_RPTR/WPTR 初值、CP_SQE_CNTL 读回、GMU AO bit3/bit5、
 *   GMU2HOST bit23、GPU_CX_BUSY_STATUS、CX_MISC_GFX_PWR_CLK_STATUS、SW_FUSE_VALUE；这些宏的注释里标“只记录”。
 *
 * 不在这里的东西：非上下文表、保护表、GBIF CX 表、AO HWCG 表、双三次权重、pwrup/ifpc/dyn 表在生成的 A830HwInit.h；
 *   各 bin 档位表、DDR/CNOC/GMU 档表在 platform/sm8750/Sm8750GpuLevels.h（生成，经 Adreno830Board 传入）；
 *   SMC 功能号在 platform/sm8750/Sm8750Scm.h（SM8750_SCM_*）；apps DRV-2 的 TCS 布局在 platform/sm8750/Sm8750Rpmh.h。
 *
 * 本文件可独立用 C11 与 C++17（及内核的 gnu17、gnu++2b）编译，只依赖 <stdint.h>，不定义任何对象或函数。
 */
#ifndef A830_REGS_H
#define A830_REGS_H
#include <stdint.h>

#if defined(__cplusplus)
#define A830_STATIC_ASSERT(expr, why) static_assert((expr), why)
#else
#define A830_STATIC_ASSERT(expr, why) _Static_assert((expr), why)
#endif

/* ===================================================================================================================
 * 1. 物理窗口与地址换算 [DT][ARCH 0.3、3.2]
 *    运行期一律用 Adreno830Board 传入的窗口；这里的值是生成器与主机测试比对用的期望值，也是换算宏的基准。
 * =================================================================================================================== */
#define A830_GPU_PHYS                   0x03D00000ull  /* kgsl_3d0_reg_memory：GX 寄存器与 CX 侧 GBIF */
#define A830_GPU_BYTES                  0x00040000u
#define A830_GMU_PHYS                   0x03D37000ull  /* gmu@3d37000 "gmu"：GMU CX、GMU AO（经 dword 偏移换算） */
#define A830_GMU_BYTES                  0x00068000u
#define A830_GMU_AO_PHYS                0x03D40000ull  /* "gmu_ao_blk_dec0"（KGSL 单独映射；本驱动经 gmu 窗口访问 GMUAO） */
#define A830_GMU_AO_BYTES               0x00010000u
#define A830_RSCC_PHYS                  0x03D50000ull  /* GPU 节点 "rscc" */
#define A830_RSCC_BYTES                 0x00010000u
#define A830_CX_DBGC_PHYS               0x03D61000ull  /* "cx_dbgc" */
#define A830_CX_DBGC_BYTES              0x00003000u
#define A830_GX_GDSCR_PHYS              0x03D68024ull  /* gx_clkctl_gx_gdsc 的 GDSCR [DT][CLK] */
#define A830_GX_CFG_GDSCR_PHYS          0x03D68028ull  /* CFG_GDSCR = GDSCR + 4 */
#define A830_GPUCC_PHYS                 0x03D90000ull  /* clock-controller@3d90000 */
#define A830_GPUCC_BYTES                0x00009800u
#define A830_CX_MISC_PHYS               0x03D9E000ull  /* GPU 节点 "cx_misc" */
#define A830_CX_MISC_BYTES              0x00002000u
#define A830_SMMU_PHYS                  0x03DA0000ull  /* kgsl-smmu@3da0000 */
#define A830_SMMU_BYTES                 0x00040000u
#define A830_ZAP_PHYS                   0x9B09A000ull  /* gpu-microcode@9b09a000，no-map */
#define A830_ZAP_BYTES                  0x00002000u
#define SM8750_GCC_PHYS                 0x00100000ull
#define SM8750_GCC_BYTES                0x001F4200u
#define SM8750_RSC_DRV2_PHYS            0x16520000ull  /* apps_rsc "drv-2"：tcs-offset 0xD00、tcs-distance 0x2A0 [DT][HW] */
#define SM8750_RSC_DRV2_BYTES           0x00010000u
#define SM8750_CMD_DB_PHYS              0x81C60000ull  /* aop_cmd_db_region */
#define SM8750_CMD_DB_BYTES             0x00020000u
#define SM8750_SMEM_PHYS                0x81D00000ull  /* 只允许读 [0x1000,0x9E000) 与 [0x1FF000,0x200000) [HW] */
#define SM8750_SMEM_BYTES               0x00200000u
#define SM8750_QFPROM_PHYS              0x221C8000ull  /* qfprom 中含 GPU 速度档的页 */
#define SM8750_QFPROM_BYTES             0x00001000u
#define SM8750_QFPROM_GPU_SPEED_BIN     0x00000138u    /* gpu_speed_bin@138：reg <0x138 2>，bits [8:0] */
#define SM8750_GPU_SPEED_BIN_MASK       0x000001FFu
#define SM8750_AOSS_QMP_PHYS            0x0C300000ull  /* aoss_qmp 消息区 */
#define SM8750_AOSS_QMP_BYTES           0x00000400u
#define SM8750_IPCC_PHYS                0x00406000ull  /* ipcc_mproc */
#define SM8750_IPCC_BYTES               0x00001000u
#define SM8750_TSENS2_TM_PHYS           0x0C22A000ull  /* tsens2 第一段（TM） */
#define SM8750_TSENS2_TM_BYTES          0x000001FFu

/* GPU 空间 dword 偏移的换算。GMU 与 CX_MISC 的寄存器沿用 KGSL 的 GPU 空间偏移，经各自窗口访问。 */
#define A830_GPU_REG_BYTE(dw)           (4u * (uint32_t)(dw))                              /* GPU 窗口内字节偏移 */
#define A830_GPU_REG_PHYS(dw)           (A830_GPU_PHYS + 4ull * (uint64_t)(dw))            /* 物理地址 */
#define A830_GMU_WIN_DELTA              0x00037000u    /* A830_GMU_PHYS - A830_GPU_PHYS */
#define A830_GMU_REG_BYTE(dw)           (4u * (uint32_t)(dw) - A830_GMU_WIN_DELTA)         /* gmu 窗口内字节偏移 */
#define A830_CX_MISC_WIN_DELTA          0x0009E000u    /* A830_CX_MISC_PHYS - A830_GPU_PHYS */
#define A830_CX_MISC_REG_BYTE(dw)       (4u * (uint32_t)(dw) - A830_CX_MISC_WIN_DELTA)     /* cx_misc 窗口内字节偏移 */
#define A830_RSCC_REG_BYTE(dw)          (4u * (uint32_t)(dw))                              /* rscc 窗口内字节偏移 */
#define A830_RSCC_REG_PHYS(dw)          (A830_RSCC_PHYS + 4ull * (uint64_t)(dw))
#define A830_GPU_REG_IN_WINDOW(dw)      ((uint32_t)(dw) < A830_GPU_BYTES / 4u)
#define A830_GMU_REG_IN_WINDOW(dw)      ((uint32_t)(dw) >= A830_GMU_WIN_DELTA / 4u && \
                                         (uint32_t)(dw) < (A830_GMU_WIN_DELTA + A830_GMU_BYTES) / 4u)
#define A830_CX_MISC_REG_IN_WINDOW(dw)  ((uint32_t)(dw) >= A830_CX_MISC_WIN_DELTA / 4u && \
                                         (uint32_t)(dw) < (A830_CX_MISC_WIN_DELTA + A830_CX_MISC_BYTES) / 4u)

/* ===================================================================================================================
 * 2. RBBM [MESA][KGSL gen8_reg.h]（GX 域）
 * =================================================================================================================== */
#define A8XX_RBBM_GBIF_HALT             0x0000Au       /* GX 侧 GBIF 停机；bit0 = 停 GX 客户端 */
#define A8XX_RBBM_GBIF_HALT_ACK         0x0000Bu
#define A8XX_RBBM_STATUS                0x00012u
#define A8XX_RBBM_STATUS1               0x00013u       /* 超时转储 */
#define A8XX_RBBM_GFX_STATUS            0x00015u       /* 超时转储 */
#define A8XX_RBBM_INTERFACE_HANG_INT_CNTL 0x0002Fu
#define A8XX_RBBM_INT_CLEAR_CMD         0x00061u       /* 写 1 清 */
#define A8XX_RBBM_INT_0_MASK            0x00062u
#define A8XX_RBBM_INT_0_STATUS          0x0006Au       /* 屏蔽后的状态；未屏蔽影子见 A8XX_GMUAO_RBBM_INT_UNMASKED_STATUS_SHADOW */
#define A8XX_RBBM_SW_FUSE_INT_STATUS    0x00071u
#define A8XX_RBBM_SW_FUSE_INT_MASK      0x00072u
#define A8XX_RBBM_CLOCK_CNTL_GLOBAL     0x0009Au
#define A8XX_RBBM_CGC_GLOBAL_LOAD_CMD   0x0009Bu
#define A8XX_RBBM_CGC_P2S_TRIG_CMD      0x0009Cu
#define A8XX_RBBM_CGC_P2S_STATUS        0x0009Fu
#define A8XX_RBBM_CGC_0_PC              0x0010Bu
#define A8XX_RBBM_PERFCTR_GPU_BUSY_MASKED 0x0019Eu
#define A8XX_RBBM_PERFCTR_SRAM_INIT_STATUS 0x0019Fu    /* A7XX 是 0x50F */
#define A8XX_RBBM_PERFCTR_CP(i)         (0x001B0u + 2u * (uint32_t)(i))   /* reg64 数组，14 个；CP0 由 CP_PERFCTR_CP_SEL[0]=1 计周期 */
#define A8XX_RBBM_PERFCTR_CP_COUNT      14u
#define A8XX_RBBM_PERFCTR_SRAM_INIT_CMD 0x00449u       /* A7XX 是 0x50E */
#define A8XX_RBBM_PERFCTR_CNTL          0x00460u
#define A8XX_RBBM_SLICE_PERFCTR_CNTL    0x00500u
#define A8XX_RBBM_SLICE_INTERFACE_HANG_INT_CNTL 0x0058Fu
#define A8XX_RBBM_SLICE_PERFCTR_SRAM_INIT_CMD 0x005E8u
#define A8XX_RBBM_SECVID_TRUST_CNTL     0x0F400u       /* 决不直接写：ZAP 可用时受安全保护，写即 XPU 违规（ARCH 4.11） */
#define A8XX_RBBM_SECVID_TSB_TRUSTED_BASE 0x0F800u     /* reg64 */
#define A8XX_RBBM_SECVID_TSB_TRUSTED_BASE_HI 0x0F801u
#define A8XX_RBBM_SECVID_TSB_TRUSTED_SIZE 0x0F802u
#define A8XX_RBBM_SECVID_TSB_CNTL       0x0F803u

/* RBBM_INT_0_STATUS / _MASK / INT_CLEAR_CMD 的位 [MESA A6XX_RBBM_INT_0_MASK][KGSL GEN8_INT_*] */
#define A8XX_RBBM_INT_0_GPU_IDLE        (1u << 0)
#define A8XX_RBBM_INT_0_CP_AHB_ERROR    (1u << 1)
#define A8XX_RBBM_INT_0_ATB_ASYNCFIFO_OVERFLOW (1u << 6)
#define A8XX_RBBM_INT_0_GPC_ERROR       (1u << 7)
#define A8XX_RBBM_INT_0_CP_SW           (1u << 8)
#define A8XX_RBBM_INT_0_CP_HW_ERROR     (1u << 9)
#define A8XX_RBBM_INT_0_PM4CPINTERRUPT  (1u << 15)     /* 正常事件，只计数 */
#define A8XX_RBBM_INT_0_CP_RB_DONE_TS   (1u << 17)     /* 正常事件，只计数 */
#define A8XX_RBBM_INT_0_CP_CACHE_FLUSH_TS (1u << 20)   /* 环栅栏 CP_EVENT_WRITE7(CACHE_CLEAN|IRQ) 完成 */
#define A8XX_RBBM_INT_0_ATB_BUS_OVERFLOW (1u << 22)
#define A8XX_RBBM_INT_0_HANG_DETECT     (1u << 23)
#define A8XX_RBBM_INT_0_UCHE_OOB_ACCESS (1u << 24)
#define A8XX_RBBM_INT_0_UCHE_TRAP       (1u << 25)
#define A8XX_RBBM_INT_0_TSB_WRITE_ERROR (1u << 28)
#define A8XX_RBBM_INT_0_SW_FUSE_VIOLATION (1u << 29)
/* A8XX_INT_MASK = Linux A8XX_INT_MASK = KGSL GEN8_INT_MASK（ARCH 4.8 第 16 步）：位 1,6,7,8,9,15,17,20,22,23,24,25,28,29 */
#define A8XX_INT_MASK                   0x33D283C2u
/* 错误位：INT_MASK 去掉 bit15、bit17 两个正常事件与 bit20 栅栏（ARCH 4.12.3 第 4 步，含 bit8 CP_SW；
 * docs/inputs/perf 的 A8XX_INT_ERRORS 漏了 bit8，以 ARCH 为准） */
#define A8XX_INT_ERRORS                 0x33C003C2u
#define A8XX_INT_FATAL                  (A8XX_RBBM_INT_0_CP_HW_ERROR | A8XX_RBBM_INT_0_HANG_DETECT)  /* 上半部立即降掩码 */
#define A830_INT_MASK_FENCE_ONLY        A8XX_RBBM_INT_0_CP_CACHE_FLUSH_TS   /* 致命错误后只留栅栏中断：0x00100000 */

/* RBBM_STATUS [MESA A8XX]：空闲判据 = 除 bit0 外全 0，且 RBBM_INT_0_STATUS 的 HANG_DETECT 为 0（ARCH 4.9） */
#define A8XX_RBBM_STATUS_CP_AHB_BUSY_CX_MASTER (1u << 0)
#define A8XX_RBBM_STATUS_CP_BUSY        (1u << 2)
#define A8XX_RBBM_STATUS_GPU_BUSY_IGN_AHB (1u << 23)
#define A8XX_RBBM_STATUS_BUSY_MASK      (~A8XX_RBBM_STATUS_CP_AHB_BUSY_CX_MASTER)
#define A8XX_RBBM_CGC_P2S_STATUS_TXDONE (1u << 0)      /* 轮询上限 10 µs，超时只记录（ARCH 4.11 第 11 步） */
#define A8XX_RBBM_PERFCTR_SRAM_INIT_STATUS_DONE (1u << 0)
#define A8XX_RBBM_GBIF_HALT_GX          (1u << 0)

/* hw_init 取值（ARCH 4.8）[LNX a8xx hw_init][KGSL gen8_start] */
#define A8XX_RBBM_INTERFACE_HANG_INT_CNTL_EN (1u << 30)
#define A830_HANG_DETECT_CYCLES         0x00CFFFFFu    /* KGSL gen8_0_1 hang_detect_cycles */
#define A830_RBBM_INTERFACE_HANG_INT_CNTL_VALUE 0x40CFFFFFu
#define A830_RBBM_SLICE_INTERFACE_HANG_INT_CNTL_VALUE 0x40000000u
#define A830_RBBM_PERFCTR_GPU_BUSY_MASKED_VALUE 0xFFFFFFFFu
#define A830_RBBM_PERFCTR_CNTL_VALUE    0x00000001u    /* RBBM_PERFCTR_CNTL 与 RBBM_SLICE_PERFCTR_CNTL 都写 1 */
/* HWCG（ARCH 4.11 第 11 步） */
#define A830_RBBM_CGC_0_PC_VALUE        0x00000702u

/* ===================================================================================================================
 * 3. CP [MESA][KGSL gen8_reg.h]（GX 域）
 *    注意：Gen8 的 CP_SQE_CNTL 是 0x815，0x808 在 Gen8 上是 CP_RB_RPTR_ADDR_BV（A7XX 的 0x808 才是 SQE_CNTL），决不能写 0x808
 *    当作启动 SQE（ARCH 2.2、4.9）。
 * =================================================================================================================== */
#define A8XX_CP_RB_BASE                 0x00800u       /* reg64 */
#define A8XX_CP_RB_BASE_HI              0x00801u
#define A8XX_CP_RB_CNTL                 0x00802u
#define A8XX_CP_RB_RPTR_WR              0x00803u
#define A8XX_CP_RB_RPTR_ADDR            0x00804u       /* reg64：BR 的 RPTR 影子 = memptrs+0 */
#define A8XX_CP_RB_RPTR_ADDR_HI         0x00805u
#define A8XX_CP_RB_RPTR                 0x00806u       /* 启动前读到的值只记录 */
#define A8XX_CP_RB_WPTR                 0x00807u       /* 只用围栏写（ARCH 4.13.2） */
#define A8XX_CP_RB_RPTR_ADDR_BV         0x00808u       /* reg64：BV 的 RPTR 影子 = memptrs+8 */
#define A8XX_CP_RB_RPTR_ADDR_BV_HI      0x00809u
#define A8XX_CP_RB_RPTR_BV              0x0080Au
#define A8XX_CP_SMMU_STREAM_ID_LPAC     0x00814u       /* 非上下文表第一项，写 0x101：LPAC 用 SID1 取微码（ARCH 4.5.5） */
#define A8XX_CP_SQE_CNTL                0x00815u       /* 写 1 启动 SQE；读回只记录 */
#define A8XX_CP_SQE_INSTR_BASE          0x00816u       /* reg64 */
#define A8XX_CP_SQE_INSTR_BASE_HI       0x00817u
#define A8XX_CP_AQE_INSTR_BASE_0        0x00818u       /* reg64 */
#define A8XX_CP_AQE_INSTR_BASE_0_HI     0x00819u
#define A8XX_CP_AQE_INSTR_BASE_1        0x0081Au       /* reg64：只在开 LPAC 时写，首版不写 */
#define A8XX_CP_AQE_INSTR_BASE_1_HI     0x0081Bu
#define A8XX_CP_APERTURE_CNTL_HOST      0x0081Cu       /* 主机访问 _PIPE 寄存器前选管线；中断上半部从不碰 */
#define A8XX_CP_APERTURE_CNTL_GMU       0x0081Du
#define A8XX_CP_APERTURE_CNTL_CD        0x0081Eu
#define A8XX_CP_CP2GMU_STATUS           0x00822u       /* 超时转储；bit0 IFPC */
#define A8XX_CP_SCRATCH_GLOBAL(i)       (0x0082Au + (uint32_t)(i))        /* 4 个；[2] = 0x82C 存栅栏序号（A7XX 是 0x885） */
#define A8XX_CP_SCRATCH_GLOBAL_COUNT    4u
#define A8XX_CP_SCRATCH_PIPE(i)         (0x00830u + (uint32_t)(i))        /* 5 个，按管线 */
#define A8XX_CP_SCRATCH_PIPE_COUNT      5u
#define A8XX_CP_RL_ERROR_DETAILS_0      0x00840u       /* 超时转储 */
#define A8XX_CP_RL_ERROR_DETAILS_1      0x00841u
#define A8XX_CP_APRIV_CNTL_PIPE         0x0084Du
#define A8XX_CP_PROTECT_CNTL_PIPE       0x0084Eu       /* BR、BV 各写一次 */
#define A8XX_CP_PROTECT_STATUS_PIPE     0x0084Fu
#define A8XX_CP_PROTECT_GLOBAL(i)       (0x00850u + (uint32_t)(i))        /* 64 个；表来自 A830HwInit.h */
#define A8XX_CP_PROTECT_GLOBAL_COUNT    64u
#define A8XX_CP_PROTECT_PIPE(i)         (0x008A0u + (uint32_t)(i))        /* 16 个；只写 [15]（last span，BR、BV） */
#define A8XX_CP_PROTECT_PIPE_COUNT      16u
#define A8XX_CP_HW_FAULT_STATUS_PIPE    0x008B3u
#define A8XX_CP_HW_FAULT_STATUS_MASK_PIPE 0x008B4u
#define A8XX_CP_INTERRUPT_STATUS_GLOBAL 0x008B5u
#define A8XX_CP_INTERRUPT_STATUS_MASK_GLOBAL 0x008B6u
#define A8XX_CP_INTERRUPT_STATUS_PIPE   0x008B7u
#define A8XX_CP_INTERRUPT_STATUS_MASK_PIPE 0x008B8u
#define A8XX_CP_PIPE_STATUS_PIPE        0x008B9u
#define A8XX_CP_SQE_STATUS_PIPE         0x008BBu
#define A8XX_CP_PERFCTR_CP_SEL(i)       (0x008D0u + (uint32_t)(i))        /* 21 个 */
#define A8XX_CP_PERFCTR_CP_SEL_COUNT    21u
#define A8XX_CP_ALWAYS_ON_COUNTER       0x008E7u       /* reg64；S8 与 GMU AO 计数器比对（ARCH 1.3 第 5 条） */
#define A8XX_CP_ALWAYS_ON_COUNTER_HI    0x008E8u
#define A8XX_CP_SQE_STAT_ADDR_PIPE      0x008F0u       /* 超时转储 0–15 号 */
#define A8XX_CP_SQE_STAT_DATA_PIPE      0x008F1u
#define A8XX_CP_IB1_BASE                0x00900u       /* reg64，按管线（KGSL 名 CP_IB1_BASE_LO_PIPE）；看门狗读 BR */
#define A8XX_CP_IB1_BASE_HI             0x00901u
#define A8XX_CP_IB1_REM_SIZE            0x00902u
#define A8XX_CP_IB2_BASE                0x00904u       /* reg64，按管线 */
#define A8XX_CP_IB2_BASE_HI             0x00905u
#define A8XX_CP_IB2_REM_SIZE            0x00906u

/* CP_RB_CNTL [MESA AXXX_CP_RB_CNTL]：BUFSZ = log2(环字节/8)，BLKSZ = log2(块字节/8) */
#define A8XX_CP_RB_CNTL_BUFSZ(log2_qw)  ((uint32_t)(log2_qw) & 0x3Fu)
#define A8XX_CP_RB_CNTL_BLKSZ(log2_qw)  (((uint32_t)(log2_qw) & 0x3Fu) << 8)
#define A8XX_CP_RB_CNTL_NO_UPDATE       (1u << 27)     /* 不写 RPTR 影子；本驱动不用 */
#define A830_RING_BYTES                 0x00008000u    /* 32 KiB = 8192 dword */
#define A830_RING_DWORDS                (A830_RING_BYTES / 4u)
#define A830_RING_BLOCK_BYTES           32u
#define A830_CP_RB_CNTL_VALUE           0x0000020Cu    /* BUFSZ 12、BLKSZ 2（ARCH 4.9；KGSL GEN8_CP_RB_CNTL_DEFAULT） */
#define A830_CP_SQE_CNTL_START          0x00000001u

/* CP_APERTURE_CNTL_HOST [MESA a8xx_aperture_cntl]：PIPEID [15:12]、SLICEID [18:16]、USESLICEID bit23 */
#define A8XX_CP_APERTURE_CNTL_HOST_PIPE(p) (((uint32_t)(p) & 0xFu) << 12)
#define A8XX_PIPE_NONE                  0u             /* 写 0 即清 aperture */
#define A8XX_PIPE_BR                    1u
#define A8XX_PIPE_BV                    2u
#define A8XX_PIPE_LPAC                  3u             /* hw_init 跳过 */
#define A8XX_PIPE_AQE0                  4u
#define A8XX_PIPE_AQE1                  5u
#define A8XX_PIPE_DDE_BR                6u
#define A8XX_PIPE_DDE_BV                7u

/* CP_APRIV_CNTL_PIPE [MESA a6xx_cp_apriv_cntl] */
#define A8XX_CP_APRIV_ICACHE            (1u << 0)
#define A8XX_CP_APRIV_RBFETCH           (1u << 1)
#define A8XX_CP_APRIV_RBPRIVLEVEL       (1u << 2)
#define A8XX_CP_APRIV_RBRPWB            (1u << 3)
#define A8XX_CP_APRIV_SCRATCHWT         (1u << 4)
#define A8XX_CP_APRIV_CDREAD            (1u << 5)
#define A8XX_CP_APRIV_CDWRITE           (1u << 6)
#define A830_CP_APRIV_BR_VALUE          0x0000006Fu    /* BR：ICACHE|RBFETCH|RBPRIVLEVEL|RBRPWB|CDREAD|CDWRITE */
#define A830_CP_APRIV_VALUE             0x0000000Fu    /* BV、AQE0/1、DDE_BR/BV */

/* CP_PROTECT_CNTL_PIPE [MESA a8xx_cp_protect_cntl] 与保护表项编码 [LNX A6XX_PROTECT_*] */
#define A8XX_CP_PROTECT_CNTL_ACCESS_PROT_EN (1u << 0)
#define A8XX_CP_PROTECT_CNTL_ACCESS_FAULT_ON_VIOL_EN (1u << 1)
#define A8XX_CP_PROTECT_CNTL_LAST_SPAN_INF_RANGE (1u << 3)
#define A8XX_CP_PROTECT_CNTL_HALT_SQE_RANGE 0xFFFF0000u
#define A830_CP_PROTECT_CNTL_VALUE      0xFFFF000Bu
/* 表项：起始 dword [17:0]、长度（end-start）[30:18]、bit31 = 读写都禁止（0 = 允许读） */
#define A8XX_CP_PROTECT_REG(start, len, noaccess) \
    (((uint32_t)(start) & 0x3FFFFu) | (((uint32_t)(len) & 0x1FFFu) << 18) | ((noaccess) ? (1u << 31) : 0u))
#define A830_CP_PROTECT_LAST_VALUE      0x80027C02u    /* KGSL gen8_0_0 第 63 项，同时写 CP_PROTECT_PIPE[15] */

/* 中断掩码（ARCH 4.8 第 15、16 步）[MESA A8XX_CP_GLOBAL_INT_MASK 等] */
#define A8XX_CP_GLOBAL_INT_HWFAULT(p)   (1u << ((uint32_t)(p) - 1u))       /* p = A8XX_PIPE_BR..DDE_BV */
#define A8XX_CP_GLOBAL_INT_SWFAULT(p)   (1u << ((uint32_t)(p) + 15u))
#define A830_CP_INTERRUPT_STATUS_MASK_GLOBAL_VALUE 0x007F007Fu
#define A830_CP_INTERRUPT_STATUS_MASK_PIPE_VALUE   0x07E83F7Fu   /* CSF*WRAP、OPCODE/VSD/REGPROT/ILLEGAL/SMMU/VBIF、RTWROVF、LRZ* */
#define A830_CP_HW_FAULT_STATUS_MASK_PIPE_VALUE    0x0000077Fu   /* CSF*FAULT、SQEREADBURSTOVF、EVENTENGINEOVF、UCODEERROR */
#define A830_CP_SMMU_STREAM_ID_LPAC_VALUE 0x00000101u
#define A830_CP_PERFCTR_CP_SEL0_VALUE   0x00000001u    /* CP0 恒计周期 */

/* PM4 操作码与事件 [MESA adreno_pm4.xml，A7XX- 变体；A8XX 未改] */
#define A8XX_PM4_CP_NOP                 0x10u
#define A8XX_PM4_CP_WAIT_TIMESTAMP      0x14u
#define A8XX_PM4_CP_THREAD_CONTROL      0x17u
#define A8XX_PM4_CP_RESET_CONTEXT_STATE 0x1Fu
#define A8XX_PM4_CP_WAIT_FOR_IDLE       0x26u
#define A8XX_PM4_CP_WAIT_REG_MEM        0x3Cu
#define A8XX_PM4_CP_MEM_WRITE           0x3Du
#define A8XX_PM4_CP_INDIRECT_BUFFER     0x3Fu
#define A8XX_PM4_CP_EVENT_WRITE7        0x46u
#define A8XX_PM4_CP_ME_INIT             0x48u
#define A8XX_PM4_CP_SMMU_TABLE_UPDATE   0x53u          /* 按进程页表，M14 */
#define A8XX_PM4_CP_SET_MARKER          0x65u
#define A8XX_PM4_CP_SET_SECURE_MODE     0x66u
#define A8XX_PM4_CP_MEM_TO_MEM          0x73u
#define A8XX_EVENT_CCU_INVALIDATE_DEPTH 0x18u
#define A8XX_EVENT_CCU_INVALIDATE_COLOR 0x19u
#define A8XX_EVENT_LRZ_CACHE_FLUSH      0x26u          /* A5XX/A6XX 的 LRZ_FLUSH_INVALIDATE 同号 */
#define A8XX_EVENT_CACHE_CLEAN          0x31u          /* A6XX 的 0x31 是 CACHE_INVALIDATE */
#define A8XX_EVENT_CACHE_FLUSH7         0x32u
#define A8XX_EVENT_CACHE_INVALIDATE7    0x33u
#define A8XX_CP_EVENT_WRITE7_WRITE_SRC(v) (((uint32_t)(v) & 7u) << 20)
#define A8XX_CP_EVENT_WRITE7_WRITE_DST_ONCHIP (1u << 24)
#define A8XX_CP_EVENT_WRITE7_WRITE_ENABLED (1u << 27)
#define A8XX_CP_EVENT_WRITE7_IRQ        (1u << 31)
#define A8XX_CP_THREAD_BR               1u
#define A8XX_CP_THREAD_BV               2u
#define A8XX_CP_THREAD_BOTH             3u
#define A8XX_CP_THREAD_CONCURRENT_BIN_DISABLE (1u << 27)
#define A8XX_CP_THREAD_SYNC             (1u << 31)
#define A830_CP_SET_MARKER_IFPC_ENABLE  0x00000100u    /* 每次提交末尾（Linux a7xx_submit） */
#define A830_CP_SET_MARKER_IFPC_DISABLE 0x00000101u
#define A830_FENCE_EVENT                0x08000031u    /* CACHE_CLEAN|WRITE_ENABLED：BV 栅栏（ARCH 4.10） */
#define A830_FENCE_EVENT_IRQ            0x88000031u    /* 再加 IRQ：BR 栅栏，完成后 RBBM_INT_0 bit20 */

/* CP_INIT（ARCH 4.9）：环里一次写入 10 个 dword，完成判据 CP_RB_RPTR == 10 [LNX a8xx_cp_init][KGSL] */
#define A830_CP_THREAD_CONTROL_INIT     A8XX_CP_THREAD_CONCURRENT_BIN_DISABLE   /* 0x08000000 */
#define A830_CP_ME_INIT_DWORDS          7u
#define A830_CP_ME_INIT_MULTI_CONTEXT   (1u << 0)
#define A830_CP_ME_INIT_ERROR_DETECTION (1u << 1)
#define A830_CP_ME_INIT_DEFAULT_RESET   (1u << 3)
#define A830_CP_ME_INIT_NO_PERFCTR_SAVE (1u << 6)
#define A830_CP_ME_INIT_PWRUP_LIST      (1u << 8)
#define A830_CP_ME_INIT_FEATURES        0x0000014Bu
#define A830_CP_ME_INIT_HW_CONTEXTS     0x00000003u
#define A830_CP_ME_INIT_ERROR_DETECT    0x20000000u
#define A830_CP_ME_INIT_OPERATION_MODE  0x00000002u
#define A830_CP_ME_INIT_DYN_TRIPLETS    0x80000000u    /* 动态上电表是 {偏移, 值, 管线} 三元组 */
#define A830_CP_INIT_RING_DWORDS        10u            /* THREAD_CONTROL 2 + ME_INIT 8 */

/* ===================================================================================================================
 * 4. UCHE、GMEM、UBWC（ARCH 4.8 第 4–9 步、4.14.3–4.14.4）[MESA][LNX a8xx_set_ubwc_config][KGSL]
 * =================================================================================================================== */
#define A8XX_UCHE_WRITE_THRU_BASE       0x00E06u       /* reg64 */
#define A8XX_UCHE_WRITE_THRU_BASE_HI    0x00E07u
#define A8XX_UCHE_TRAP_BASE             0x00E08u       /* reg64 */
#define A8XX_UCHE_TRAP_BASE_HI          0x00E09u
#define A8XX_UCHE_CLIENT_PF             0x00E11u
#define A8XX_UCHE_CCHE_WRITE_THRU_BASE  0x00F04u       /* reg64 */
#define A8XX_UCHE_CCHE_WRITE_THRU_BASE_HI 0x00F05u
#define A8XX_UCHE_CCHE_TRAP_BASE        0x00F06u       /* reg64 */
#define A8XX_UCHE_CCHE_TRAP_BASE_HI     0x00F07u
#define A8XX_UCHE_CCHE_GC_GMEM_RANGE_MIN 0x00F08u      /* reg64 */
#define A8XX_UCHE_CCHE_GC_GMEM_RANGE_MIN_HI 0x00F09u
#define A8XX_SP_HLSQ_GC_GMEM_RANGE_MIN  0x0AE10u       /* reg64 */
#define A8XX_SP_HLSQ_GC_GMEM_RANGE_MIN_HI 0x0AE11u
#define A8XX_GRAS_NC_MODE_CNTL          0x08700u       /* BR、BV 各写 */
#define A8XX_RB_CCU_NC_MODE_CNTL        0x08E08u       /* BR */
#define A8XX_RB_GC_GMEM_PROTECT         0x08E09u       /* BR；退出安全模式后再写一次（ARCH 4.11 第 9 步） */
#define A8XX_RB_CONTEXT_SWITCH_GMEM_SAVE_RESTORE_ENABLE 0x08E50u   /* BR */
#define A8XX_RB_CMP_NC_MODE_CNTL        0x08F00u       /* BR */
#define A8XX_SP_NC_MODE_CNTL            0x0AE02u
#define A8XX_TPL1_NC_MODE_CNTL          0x0B604u
#define A8XX_TPL1_BICUBIC_WEIGHTS_TABLE(i) (0x0B606u + (uint32_t)(i))     /* 硬件 25 个，写前 20 个；值在 A830HwInit.h */
#define A8XX_TPL1_BICUBIC_WEIGHTS_TABLE_COUNT 25u
#define A830_TPL1_BICUBIC_WEIGHTS_WRITTEN 20u

#define A830_GMEM_BASE                  0x0000000004000000ull   /* uche_gmem_alignment 64 MiB；DeviceInfo.gmem_base */
#define A830_GMEM_BYTES                 0x00C00000u    /* 12 MiB */
#define A830_UCHE_TRAP_ADDR             0x0001FFFFFFFFF000ull   /* 超出 32 位输入，天然不可映射 */
#define A830_UCHE_CLIENT_PF_VALUE       0x00000081u    /* BIT(7) | 1 */
#define A830_RB_GC_GMEM_PROTECT_VALUE   0x0C000000u
#define A830_RB_CONTEXT_SWITCH_GMEM_SAVE_RESTORE_VALUE 0x00000001u

/* UBWC 5.0（DT qcom,ubwc-mode=5；ubwc_5_0_hbb16）：DeviceInfo 必须报同一组（ARCH 4.14.3） */
#define A830_UBWC_VERSION               5u
#define A830_HIGHEST_BANK_BIT           16u
#define A830_MIN_ACCESS_LENGTH          32u            /* 所以 min_acc_len_64b = 0 */
#define A830_UBWC_SWIZZLE               6u             /* 二级、三级 swizzle 都开（bit1|bit2） */
#define A830_UBWC_MACROTILE_MODE        1u
#define A830_UBWC_HBB                   (A830_HIGHEST_BANK_BIT - 13u)   /* 3 */
#define A830_UBWC_HBB_LO                (A830_UBWC_HBB & 3u)
#define A830_UBWC_HBB_HI                (A830_UBWC_HBB >> 2)
#define A830_UBWC_MODE_TAG              4u             /* qcom_ubwc_version_tag */
#define A830_UBWC_UAVFLAGPRD_INV        2u
#define A830_UBWC_AMSBC                 1u
#define A830_UBWC_RGB565_PREDICATOR     1u
#define A830_GRAS_NC_MODE_CNTL_VALUE    0x00000060u    /* hbb<<5 */
#define A830_RB_CCU_NC_MODE_CNTL_VALUE  0x00000006u    /* hbb_hi<<3 | hbb_lo<<1 */
#define A830_RB_CMP_NC_MODE_CNTL_VALUE  0x00020006u    /* mode<<15 | rgb565<<2 | amsbc<<1 */
#define A830_SP_NC_MODE_CNTL_VALUE      0x00000026u    /* uavflagprd_inv<<4 | hbb_lo<<1 */
#define A830_TPL1_NC_MODE_CNTL_VALUE    0x00000006u    /* hbb_lo<<1 */

/* ===================================================================================================================
 * 5. GBIF（CX 侧，GPU 空间）[MESA][KGSL gen8_0_0_gbif_cx_regs]
 * =================================================================================================================== */
#define A8XX_GBIF_CX_CONFIG             0x03C00u
#define A8XX_GBIF_SCACHE_CNTL0          0x03C01u
#define A8XX_GBIF_SCACHE_CNTL1          0x03C02u
#define A8XX_GBIF_QSB_SIDE(i)           (0x03C03u + (uint32_t)(i))        /* SIDE0..3 */
#define A8XX_GBIF_QSB_SIDE0             0x03C03u
#define A8XX_GBIF_QSB_SIDE3             0x03C06u
#define A8XX_GBIF_HALT                  0x03C45u
#define A8XX_GBIF_HALT_ACK              0x03C46u
#define A8XX_GBIF_HALT_CLIENT           (1u << 0)
#define A8XX_GBIF_HALT_ARB              (1u << 1)
#define A830_GBIF_HALT_BOOT_VALUE       (1u << 3)      /* GMU 启动前写 BIT(3)（KGSL gen8_gmu_register_config） */
#define A830_GBIF_CX_CONFIG_VALUE       0x20023000u
#define A830_GBIF_QSB_SIDE_VALUE        0x00071E20u
#define A830_GMU_MRC_GBIF_QOS_VALUE     0x00000033u
/* GBIF_SCACHE：GPU 用 LLCC 切片 SCID（M11）。CNTL1 = 5 个 6 位字段；CNTL0 = [15:10]、[21:16]、[27:22] 三个字段 | BIT(8)
 * [KGSL _llc_configure_gpu_scid] */
#define A8XX_GBIF_SCACHE_CNTL1_SCID(s) \
    ((((uint32_t)(s) & 0x3Fu) << 24) | (((uint32_t)(s) & 0x3Fu) << 18) | (((uint32_t)(s) & 0x3Fu) << 12) | \
     (((uint32_t)(s) & 0x3Fu) << 6) | ((uint32_t)(s) & 0x3Fu))
#define A8XX_GBIF_SCACHE_CNTL0_SCID(s) \
    ((((uint32_t)(s) & 0x3Fu) << 10) | (((uint32_t)(s) & 0x3Fu) << 16) | (((uint32_t)(s) & 0x3Fu) << 22) | (1u << 8))
#define A830_GBIF_SCACHE_CNTL1_SCID9    0x09249249u
#define A830_GBIF_SCACHE_CNTL0_SCID9    0x02492500u

/* ===================================================================================================================
 * 6. GMU（GPU 空间 dword，经 gmu 窗口：字节偏移 = 4×dword − 0x37000）[KGSL gen8_reg.h][MESA a6xx_gmu.xml A8XX][BIN]
 * =================================================================================================================== */
#define A8XX_GMU_CM3_ITCM_START         0x1B400u       /* 物理 0x3D6D000；GMU VA [0x0,0x4000) */
#define A8XX_GMU_CM3_DTCM_START         0x1C400u       /* 物理 0x3D71000；GMU VA [0x10004000,0x10008000) */
#define A8XX_GMU_TCM_DWORDS             0x01000u       /* ITCM、DTCM 各 16 KiB */
#define A8XX_GMU_DTCM_CORE_FW_VERSION   (A8XX_GMU_CM3_DTCM_START + 0xFF8u)   /* 0x1D3F8；本机 0x50010019 */
#define A8XX_GMU_DTCM_PWR_FW_VERSION    (A8XX_GMU_CM3_DTCM_START + 0xFFAu)   /* 本机 0x50000112 */
#define A8XX_GMU_DTCM_HFI_VERSION       (A8XX_GMU_CM3_DTCM_START + 0xFFCu)   /* 本机 0x20070001 = HFI 2.7.1 */
#define A8XX_GMUCX_ICACHE_CONFIG        0x1F400u
#define A8XX_GMUCX_DCACHE_CONFIG        0x1F401u
#define A8XX_GMUCX_SYS_BUS_CONFIG       0x1F40Fu
#define A8XX_GMUCX_MRC_GBIF_QOS_CTRL    0x1F50Bu
#define A8XX_GMUCX_PWR_COL_KEEPALIVE    0x1F7E4u
#define A8XX_GMUCX_PWR_COL_PREEMPTION_KEEPALIVE 0x1F7E5u
#define A8XX_GMUCX_GFX_PWR_CLK_STATUS   0x1F7E8u       /* GX 上电确认的门槛（ARCH 4.7.8） */
#define A8XX_GMUCX_RPMH_POWER_STATE     0x1F7E9u
#define A8XX_GMUCX_CX_FAL_INTF          0x1F7ECu
#define A8XX_GMUCX_CX_FALNEXT_INTF      0x1F7EDu
#define A8XX_GMUCX_CM3_SYSRESET         0x1F800u       /* 物理 0x3D7E000，gmu 窗口 +0x47000；1 = 复位 */
#define A8XX_GMUCX_CM3_BOOT_CONFIG      0x1F801u
#define A8XX_GMUCX_WDOG_CTRL            0x1F813u
#define A8XX_GMUCX_CM3_FW_INIT_RESULT   0x1F81Cu
#define A8XX_GMUCX_CM3_CFG              0x1F82Du
#define A8XX_GMUCX_AO_COUNTER_LO        0x1F840u       /* GetTimestamp：19.2 MHz，高-低-高读（ARCH 1.3 第 5 条） */
#define A8XX_GMUCX_AO_COUNTER_HI        0x1F841u
#define A8XX_GMUCX_HFI_CTRL_STATUS      0x1F980u
#define A8XX_GMUCX_HFI_QTBL_INFO        0x1F984u
#define A8XX_GMUCX_HFI_QTBL_ADDR        0x1F985u
#define A8XX_GMUCX_HFI_CTRL_INIT        0x1F986u
#define A8XX_GMUCX_GMU2HOST_INTR_SET    0x1F990u
#define A8XX_GMUCX_GMU2HOST_INTR_CLR    0x1F991u       /* 写 1 清 */
#define A8XX_GMUCX_GMU2HOST_INTR_INFO   0x1F992u       /* 原始状态，不受 MASK 影响 */
#define A8XX_GMUCX_GMU2HOST_INTR_MASK   0x1F993u       /* 1 = 屏蔽 */
#define A8XX_GMUCX_HOST2GMU_INTR_SET    0x1F994u       /* 只写：bit0 HFI 门铃，bit28..31 OOB */
#define A8XX_GMUCX_HOST2GMU_INTR_CLR    0x1F995u
#define A8XX_GMUCX_HOST2GMU_INTR_RAW_INFO 0x1F996u
#define A8XX_GMUCX_GENERAL_8            0x1F9C8u       /* 日志：GMU VA[31:12] | (字节/4K − 1) */
#define A8XX_GMUCX_GENERAL_9            0x1F9C9u       /* 日志写指针保留值，冷启动 0 */
#define A8XX_GMUCX_GENERAL_10           0x1F9CAu       /* GMU chip id */
#define A8XX_GMUCX_GENERAL_11           0x1F9CBu       /* VRB 地址（RB 模式不用） */
#define A8XX_GMUCX_POWER_COUNTER_ENABLE 0x1FC10u       /* A7XX 的 0x1F840 在 A8XX 上是 AO 计数器，不能照搬 */
#define A8XX_GMUCX_POWER_COUNTER_SELECT_XOCLK_0 0x1FC40u
#define A8XX_GMUCX_POWER_COUNTER_SELECT_XOCLK_1 0x1FC41u
#define A8XX_GMUCX_POWER_COUNTER_XOCLK_L_0 0x1FCA0u    /* DCVS 忙计数：19.2 MHz tick，先 H 再 L 再 H */
#define A8XX_GMUCX_POWER_COUNTER_XOCLK_H_0 0x1FCA1u
#define A8XX_GMUAO_AO_INTERRUPT_EN      0x23B03u
#define A8XX_GMUAO_AO_HOST_INTERRUPT_CLR 0x23B04u
#define A8XX_GMUAO_AO_HOST_INTERRUPT_STATUS 0x23B05u
#define A8XX_GMUAO_AO_HOST_INTERRUPT_MASK 0x23B06u
#define A8XX_GMUAO_RSCC_CONTROL_REQ     0x23B07u
#define A8XX_GMUAO_RSCC_CONTROL_ACK     0x23B08u
#define A8XX_GMUAO_CGC_MODE_CNTL        0x23B09u
#define A8XX_GMUAO_CGC_DELAY_CNTL       0x23B0Au
#define A8XX_GMUAO_CGC_HYST_CNTL        0x23B0Bu
#define A8XX_GMUAO_GPU_CX_BUSY_STATUS   0x23B0Cu       /* 只记录：bit30（KGSL gen8）与 bit23（Linux）【分歧】 */
#define A8XX_GMUAO_GPU_CX_BUSY_STATUS2  0x23B0Du
#define A8XX_GMUAO_GPU_CX_BUSY_MASK     0x23B0Eu
#define A8XX_GMUAO_AHB_FENCE_CTRL       0x23B10u
#define A8XX_GMUAO_AHB_FENCE_RANGE_0    0x23B11u
#define A8XX_GMUAO_AHB_FENCE_STATUS     0x23B13u       /* WPTR 围栏写检查：gmu 窗口 +0x57C4C */
#define A8XX_GMUAO_AHB_FENCE_STATUS_CLR 0x23B14u
#define A8XX_GMUAO_RBBM_INT_UNMASKED_STATUS_SHADOW 0x23B15u   /* 诊断看它，不看屏蔽后的 RBBM_INT_0_STATUS */

/* 位与取值 */
#define A8XX_GMUCX_GFX_PWR_CLK_STATUS_GX_GDSC_OFF (1u << 0)
#define A8XX_GMUCX_GFX_PWR_CLK_STATUS_GX_CLK_OFF  (1u << 1)
#define A8XX_GMUCX_GFX_PWR_CLK_STATUS_GX_OFF      0x00000003u   /* [1:0]==0 才算 GX 开 */
#define A830_GMU_RPMH_POWER_STATE_ACTIVE 0x0u
#define A830_GMU_RPMH_POWER_STATE_IFPC  0x3u
#define A830_GMU_RPMH_POWER_STATE_SLUMBER 0xFu          /* B6 先写 0xF */
#define A830_GMU_CX_FAL_VETO            0x1u            /* CX_FAL_INTF、CX_FALNEXT_INTF 写 1 */
#define A830_GMU_CM3_SYSRESET_ASSERT    0x1u
#define A830_GMU_CM3_SYSRESET_RELEASE   0x0u
#define A830_GMU_CM3_BOOT_CONFIG_VALUE  0x00000002u
#define A830_GMU_CM3_CFG_VALUE          0x00004052u
#define A830_GMU_CM3_CFG_NMI            (1u << 9)       /* 必须为 0 */
#define A830_GMU_FW_INIT_MASK           0x000001FFu     /* core 版本 > 0x20010004 的判据 */
#define A830_GMU_FW_INIT_READY          0x00000100u
#define A830_GMU_FW_INIT_NMI_MASK       0x00000E00u     /* 非 0 = GMU 复位/NMI 中，立即失败 */
#define A830_GMU_HFI_CTRL_STATUS_READY  (1u << 0)
#define A830_GMU_HFI_QTBL_INFO_VALUE    0x00000001u
#define A830_GMU_HFI_CTRL_INIT_VALUE    0x00000001u
#define A830_GMU_CACHE_CONFIG_VALUE     0x00000001u     /* ICACHE、DCACHE、SYS_BUS_CONFIG 都写 1 */
#define A830_GMU_CHIPID                 0x08000100u     /* GENERAL_10：ADRENO_GMU_REV(8.0.1) [KGSL] = gmu_chipid [LNX] = [BIN] */
#define A830_GMU_LOG_CFG(va, bytes)     (((uint32_t)(va) & 0xFFFFF000u) | (((uint32_t)(bytes) / 0x1000u - 1u) & 0xFFu))
#define A830_GMU_PWRCNT_XOCLK0_GPU_BUSY 0x00000020u     /* SELECT_XOCLK_0 [7:0] |= 0x20 */
#define A830_GMU_PWRCNT_XOCLK1_IFPC     0x00000004u     /* SELECT_XOCLK_1 [7:0] = 0x04 */
#define A830_GMU_PWRCNT_SELECT_MASK     0x000000FFu
#define A830_GMU_POWER_COUNTER_ENABLE_VALUE 0x00000001u
#define A830_GMU_CX_BUSY_MASK_VALUE     0xFF000000u
#define A830_GMU_CX_BUSY_KGSL           (1u << 30)      /* CXGXCPUBUSYIGNAHB [KGSL gen8] */
#define A830_GMU_CX_BUSY_LINUX          (1u << 23)      /* GPUBUSYIGNAHB [LNX] */
#define A830_GMU_CGC_MODE_VALUE         0x00020000u     /* KGSL gen8_ao_hwcg_regs */
#define A830_GMU_CGC_DELAY_VALUE        0x00010111u
#define A830_GMU_CGC_HYST_VALUE         0x00005555u
#define A830_GMU_RSCC_CONTROL_SLEEP     (1u << 0)
#define A830_GMU_RSCC_CONTROL_WAKE      (1u << 1)       /* ACK 也看 bit1 */

/* GMU2HOST_INTR_*（HFI 中断 336）[KGSL adreno_hfi.h][LNX] */
#define A830_GMU2HOST_MSGQ              (1u << 0)       /* HFI 消息到达；默认不开中断，HFI 等待用睡眠轮询 */
#define A830_GMU2HOST_SIDEMSGQ          (1u << 1)
#define A830_GMU2HOST_DBGQ              (1u << 2)
#define A830_GMU2HOST_CM3_FAULT         (1u << 15)      /* KGSL：致命 */
#define A830_GMU2HOST_CM3_FAULT_LINUX   (1u << 23)      /* Linux 定义：只记录（ARCH 1.3 第 13 条） */
#define A830_GMU2HOST_OOB_MASK          0xFFFF0000u     /* bit16..31 是 OOB 应答区 */
#define A830_GMU2HOST_MASK_RUN          0xFF7F7FF9u     /* 开放 bit1、bit2、bit15、bit23 */
#define A830_GMU2HOST_IRQ_CLEAR         0x00808006u     /* 336 上半部无条件写 CLR（ARCH 4.12.3） */
#define A830_HOST2GMU_HFI_DOORBELL      (1u << 0)

/* AO_HOST_INTERRUPT_*（GMU 中断 337）[KGSL kgsl_gmu_core.h] */
#define A830_GMU_AO_WDOG_BITE           (1u << 0)       /* 致命 */
#define A830_GMU_AO_RSCC_COMP           (1u << 1)
#define A830_GMU_AO_FENCE_ERR           (1u << 3)       /* 只记录 */
#define A830_GMU_AO_DBD_WAKEUP          (1u << 4)
#define A830_GMU_AO_HOST_AHB_BUS_ERR    (1u << 5)       /* 只记录 */
#define A830_GMU_AO_MASK_RUN            0xFFFFFFD6u     /* 开放 bit0、bit3、bit5 */

/* AHB 围栏（ARCH 1.3 第 12 条：默认 KGSL 下界 0x8A0，IFPC 关时不生效） */
#define A830_AHB_FENCE_RANGE(lo)        ((1u << 31) | (0x32u << 18) | ((uint32_t)(lo) & 0x3FFFFu))
#define A830_AHB_FENCE_RANGE_KGSL       0x80C808A0u
#define A830_AHB_FENCE_RANGE_LINUX      0x80C808C0u     /* Linux a8xx = [BIN] */
#define A830_AHB_FENCE_CTRL_ALLOW       0x0u
#define A830_AHB_FENCE_STATUS_WRITEDROPPED0 (1u << 0)
#define A830_AHB_FENCE_STATUS_WRITEDROPPED1 (1u << 1)
#define A830_AHB_FENCE_STATUS_CLR_VALUE 0x00000007u

/* OOB（gen8 非 legacy 位：set = BIT(30 − 2·req)，ack = BIT(31 − req)，clear = BIT(31 − 2·req)）[KGSL][LNX][BIN]
 * req 0 = GPU，1 = perfcounter；gen8 不支持 boot_slumber、dcvs（req ≥ 6）。set/clear 写 HOST2GMU_INTR_SET，
 * ack 在 GMU2HOST_INTR_INFO，处理完写 GMU2HOST_INTR_CLR 清除。 */
#define A830_OOB_REQ_GPU                0u
#define A830_OOB_REQ_PERFCNTR           1u
#define A830_OOB_SET(req)               (1u << (30u - 2u * (uint32_t)(req)))
#define A830_OOB_ACK(req)               (1u << (31u - (uint32_t)(req)))
#define A830_OOB_CLEAR(req)             (1u << (31u - 2u * (uint32_t)(req)))
#define A830_OOB_GPU_SET                (1u << 30)
#define A830_OOB_GPU_ACK                (1u << 31)
#define A830_OOB_GPU_CLEAR              (1u << 31)
#define A830_OOB_PERFCNTR_SET           (1u << 28)
#define A830_OOB_PERFCNTR_ACK           (1u << 30)
#define A830_OOB_PERFCNTR_CLEAR         (1u << 29)

/* ===================================================================================================================
 * 7. GPU RSCC（相对 0x03D50000 的 dword）[KGSL gen8_reg.h]
 *    主机不编程 RSCC 序列器与 PDC（ARCH 4.7）；只在强制关断、slumber、RSCC 唤醒时读这些状态。
 * =================================================================================================================== */
#define A8XX_RSCC_RSC_STATUS0_DRV0      0x00004u       /* 物理 0x3D50010 */
#define A8XX_RSCC_SEQ_BUSY_DRV0         0x00101u       /* 物理 0x3D50404 */
#define A8XX_RSCC_TCS0_DRV0_STATUS      0x0034Au       /* 物理 0x3D50D28 */
#define A8XX_RSCC_TCS_STRIDE            0x000A8u
#define A8XX_RSCC_TCS_COUNT             10u
#define A8XX_RSCC_TCS_DRV0_STATUS(n)    (A8XX_RSCC_TCS0_DRV0_STATUS + A8XX_RSCC_TCS_STRIDE * (uint32_t)(n))
#define A8XX_RSCC_TCS_IDLE              (1u << 0)
/* Mesa 表（docs/inputs/regs/a8xx_gmu_regs.txt）写的是 0x346 起，那是 A6XX/A7XX 第一版的位置；A740 族与 A8XX 要再加
 * 4（Linux a6xx_gmu_rpmh_off 的 seqmem_off），等于 KGSL gen8 的 0x34A。 */
#define A8XX_RSCC_MESA_TCS_DELTA        4u
#define A830_RSCC_SLEEP_DONE            (1u << 16)      /* RSC_STATUS0_DRV0：A830 用 bit16，A840/A850 族用 bit30 [LNX] */

/* ===================================================================================================================
 * 8. CX_MISC（KGSL 的 GPU 空间 dword；经 cx_misc 窗口：字节偏移 = 4×dword − 0x9E000）[KGSL][MESA A6XX_CX_MISC]
 * =================================================================================================================== */
#define A8XX_CX_MISC_CX_AHB_AON_CNTL    0x27810u       /* 物理 0x3D9E040；..0x27814 依次 AON/GMU/CP/VBIF_SMMU/HOST（M13） */
#define A8XX_CX_MISC_CX_AHB_GMU_CNTL    0x27811u
#define A8XX_CX_MISC_CX_AHB_CP_CNTL     0x27812u
#define A8XX_CX_MISC_CX_AHB_VBIF_SMMU_CNTL 0x27813u
#define A8XX_CX_MISC_CX_AHB_HOST_CNTL   0x27814u
#define A8XX_CX_MISC_INT_CLEAR_CMD      0x27831u       /* 可选：写全 1 */
#define A8XX_CX_MISC_INT_0_MASK         0x27833u       /* 可选：只开 bit31 GPU_CC 限频事件（INTID 112） */
#define A8XX_CX_MISC_INT_0_STATUS       0x27834u
#define A8XX_CX_MISC_TCM_RET_CNTL       0x27839u       /* 只读（A830 由 AOP 管），不写 */
#define A8XX_CX_MISC_AO_COUNTER_LO      0x27880u       /* S4 第一步用 CNTVCT 播种，每次开机一次（ARCH 4.4.2） */
#define A8XX_CX_MISC_AO_COUNTER_HI      0x27881u
#define A8XX_CX_MISC_GFX_PWR_CLK_STATUS 0x27883u       /* 只记录：bit3 GX_GDSC_POWER_OFF，bit4 GX_CLK_OFF；不作门槛 */
#define A8XX_CX_MISC_SW_FUSE_VALUE      0x27C00u       /* 只记录：SCM 之后读 */
#define A8XX_CX_MISC_SW_FUSE_FREQ_LIMIT_STATUS 0x27C05u /* 只记录：[8:0] FINALFREQLIMIT，bit24 SOFTSKUDISABLED */
#define A8XX_CX_MISC_INT_GPU_CC_IRQ     (1u << 31)
#define A8XX_CX_MISC_SW_FUSE_FASTBLEND  (1u << 0)
#define A8XX_CX_MISC_SW_FUSE_LPAC       (1u << 1)
#define A8XX_CX_MISC_SW_FUSE_RAYTRACING (1u << 2)
#define A8XX_CX_MISC_GFX_PWR_CLK_STATUS_GX_GDSC_OFF (1u << 3)
#define A8XX_CX_MISC_GFX_PWR_CLK_STATUS_GX_CLK_OFF  (1u << 4)
#define A8XX_CX_MISC_FREQ_LIMIT_MASK    0x000001FFu
#define A8XX_CX_MISC_SOFT_SKU_DISABLED  (1u << 24)
/* CX_AHB_*_CNTL = ADRENO_AHB_CNTL_DEFAULT（bit8/9/11/12）| 超时 log2(周期) [4:0]。ARCH 4.4.2 写 0x1B12（超时 2^18 周期）；
 * KGSL 对 gen8_0_1（hub 200 MHz、NOC 超时 3410 µs）按公式算出 19，即 0x1B13。两者都短于 NOC 超时、长于 1 ms 下限，
 * 这里按 ARCH 取 0x1B12，M13 实现时再定（见报告的未决问题）。 */
#define A8XX_CX_MISC_CX_AHB_CNTL_DEFAULT 0x00001B00u
#define A8XX_CX_MISC_CX_AHB_CNTL_TIMEOUT(log2_cycles) ((uint32_t)(log2_cycles) & 0x1Fu)
#define A830_CX_AHB_CNTL_VALUE          0x00001B12u

/* ===================================================================================================================
 * 9. 时钟与电源：GCC（相对 0x00100000）、GPUCC（相对 0x03D90000）、GX clkctl [CLK][HW]
 *    上电序列在 platform/sm8750/Sm8750GpuPower.c（其内部常量与此一致）；这里供断电、强制关断与主机测试引用。
 * =================================================================================================================== */
#define SM8750_GCC_GPLL0_MODE           0x00000u       /* bit31 LOCK_DET；本机 L[15:0]=0x1F、ALPHA[15:0]=0x4000 = 600 MHz */
#define SM8750_GCC_GPLL0_L_VAL          0x00010u
#define SM8750_GCC_GPLL0_ALPHA_VAL      0x00014u
#define SM8750_GCC_APCS_CLOCK_BRANCH_ENA_VOTE 0x52000u /* bit15 gcc_gpu_gpll0_clk_src，bit16 gcc_gpu_gpll0_div_clk_src */
#define SM8750_GCC_APCS_GPLL_ENA_VOTE   0x52020u       /* bit0 GPLL0 投票（系统持有，不碰） */
#define SM8750_GCC_GPU_CFG_AHB_CBCR     0x71004u       /* 关键常开，只置位；带 hwcg，CLK_OFF 不可作判据 */
#define SM8750_GCC_GPU_GEMNOC_GFX_CBCR  0x71010u       /* HALT_VOTED + hwcg；本机读到 0x80000003，因 HW_CTL 放行 [HW] */
#define SM8750_GCC_DDRSS_GPU_AXI_CBCR   0x71150u       /* HALT_SKIP，只置位 */
#define SM8750_GCC_GPLL0_VOTE_GPU       ((1u << 15) | (1u << 16))
#define SM8750_GPUCC_PLL0_MODE          0x0000u        /* bit0 OUTCTRL，bit2 RESET_N，bit31 LOCK_DET */
#define SM8750_GPUCC_PLL0_OPMODE        0x0004u        /* 0 STANDBY，1 RUN */
#define SM8750_GPUCC_PLL0_L_VAL         0x0010u
#define SM8750_GPUCC_PLL0_ALPHA_VAL     0x0014u
#define SM8750_GPUCC_PLL0_USER_CTL      0x0018u        /* 使能后 |= 0x7 */
#define SM8750_GPUCC_PLL0_USER_CTL_U    0x001Cu
#define SM8750_GPUCC_PLL0_CONFIG_CTL    0x0020u
#define SM8750_GPUCC_PLL0_CONFIG_CTL_U  0x0024u
#define SM8750_GPUCC_PLL0_CONFIG_CTL_U1 0x0028u
#define SM8750_GPUCC_HLOS1_VOTE_SMMU_CBCR 0x7000u      /* HALT_VOTED */
#define SM8750_GPUCC_XO_BCR             0x9000u
#define SM8750_GPUCC_RSCC_XO_AON_CBCR   0x9004u        /* 关键：只置位，不回读 */
#define SM8750_GPUCC_CXO_AON_CBCR       0x9008u        /* 关键：只置位，置位后读到 0x80000000 [HW] */
#define SM8750_GPUCC_DEMET_CBCR         0x9010u        /* HALT_VOTED；Linux/KGSL 不开，首版沿用实测（ARCH 1.3 第 14 条） */
#define SM8750_GPUCC_GX_BCR             0x905Cu
#define SM8750_GPUCC_GX_AHB_FF_CBCR     0x9064u        /* 关键：只置位 */
#define SM8750_GPUCC_CX_BCR             0x907Cu        /* bit0=1 表示处于复位 */
#define SM8750_GPUCC_CX_GDSCR           0x9080u
#define SM8750_GPUCC_CX_CFG_GDSCR       0x9084u        /* bit16 POWER_UP_COMPLETE，bit15 POWER_DOWN_COMPLETE */
#define SM8750_GPUCC_CX_GDS_HW_CTRL     0x9094u        /* bit31 PWR_ON：上电轮询只看这里 [HW] */
#define SM8750_GPUCC_AHB_CBCR           0x90BCu        /* HALT_DELAY：置位后延时 10 µs，不轮询 */
#define SM8750_GPUCC_SLEEP_CBCR         0x90CCu        /* 关键：只置位 */
#define SM8750_GPUCC_CX_GMU_CBCR        0x90D4u
#define SM8750_GPUCC_CXO_CBCR           0x90E4u
#define SM8750_GPUCC_HUB_CX_INT_CBCR    0x90E8u
#define SM8750_GPUCC_GMU_BCR            0x9314u
#define SM8750_GPUCC_GMU_CMD_RCGR       0x9318u        /* CFG = +4；500 MHz：CFG=0x201（源 2 = PLL0_OUT_EVEN，/1） */
#define SM8750_GPUCC_CB_BCR             0x93A0u
#define SM8750_GPUCC_CB_CBCR            0x93A4u        /* 关键：只置位 */
#define SM8750_GPUCC_RSCC_HUB_AON_CBCR  0x93A8u        /* 关键：只置位 */
#define SM8750_GPUCC_FAST_HUB_BCR       0x93E4u
#define SM8750_GPUCC_HUB_AON_CBCR       0x93E8u        /* HALT_VOTED（可选，同 demet） */
#define SM8750_GPUCC_HUB_CMD_RCGR       0x93ECu        /* CFG = +4；200 MHz：CFG=0x505（源 5 = GPLL0，/3） */
#define SM8750_RCG_CFG_OFFSET           0x0004u        /* CFG_RCGR = CMD_RCGR + 4 */
#define SM8750_GPUCC_PLL0_L_VALUE       0x00440034u    /* L=0x34、CAL_L=0x44：VCO 1000 MHz，EVEN 500 MHz */
#define SM8750_GPUCC_PLL0_ALPHA_VALUE   0x00001555u
#define SM8750_GPUCC_PLL0_USER_CTL_VALUE 0x00000400u   /* EVEN 后分频 /2 */
#define SM8750_GPUCC_PLL0_USER_CTL_U_VALUE 0x00000002u
#define SM8750_GPUCC_PLL0_CONFIG_CTL_VALUE 0x19660387u
#define SM8750_GPUCC_PLL0_CONFIG_CTL_U_VALUE 0x098060A0u
#define SM8750_GPUCC_PLL0_CONFIG_CTL_U1_VALUE 0xB416CB20u
#define SM8750_GPUCC_PLL0_OUTPUTS       0x00000007u    /* USER_CTL |= 0x7 */
#define SM8750_GPUCC_GMU_RCG_CFG_500MHZ 0x00000201u
#define SM8750_GPUCC_HUB_RCG_CFG_200MHZ 0x00000505u
#define SM8750_CBCR_ENABLE              (1u << 0)
#define SM8750_CBCR_HW_CTL              (1u << 1)      /* hwcg 开：跳过 halt 检查 */
#define SM8750_CBCR_NOC_FSM_STATUS(v)   (((uint32_t)(v) >> 28) & 7u)   /* == SM8750_CBCR_NOC_FSM_ON 视为已开 */
#define SM8750_CBCR_NOC_FSM_ON          2u
#define SM8750_CBCR_CLK_OFF             (1u << 31)
#define SM8750_RCG_CMD_UPDATE           (1u << 0)
#define SM8750_RCG_CMD_ROOT_EN          (1u << 1)
#define SM8750_RCG_CMD_ROOT_OFF         (1u << 31)
#define SM8750_RCG_CFG_MASK             0x0010371Fu    /* SRC_DIV[4:0] | SRC_SEL[10:8] | MODE[13:12] | HW_CLK_CTRL[20] */
#define SM8750_GDSC_SW_COLLAPSE         (1u << 0)
#define SM8750_GDSC_HW_CONTROL          (1u << 1)
#define SM8750_GDSC_SW_OVERRIDE         (1u << 2)
#define SM8750_GDSC_RETAIN_FF_ENABLE    (1u << 11)
#define SM8750_GDSC_WAIT_FIELDS_MASK    0x00FFF000u
#define SM8750_GDSC_CX_WAIT_VALUES      0x00228000u    /* EN_REST=2、EN_FEW=2、CLK_DIS=8（gpu_cc_cx_gdsc）[CLK][HW] */
#define SM8750_GDSC_CX_WRITE_MASK       0x00FFF007u    /* 写 CX GDSCR 时的掩码（ARCH 4.2.1 第 4 步） */
#define SM8750_GDSC_PWR_ON              (1u << 31)
#define SM8750_GDSC_CFG_POWER_UP_COMPLETE (1u << 16)
#define SM8750_GDSC_CFG_POWER_DOWN_COMPLETE (1u << 15)   /* GX 强制关断时轮询 CFG_GDSCR 的这一位（ARCH 4.7.9 第 6 步） */
#define SM8750_PLL_MODE_OUTCTRL         (1u << 0)
#define SM8750_PLL_MODE_RESET_N         (1u << 2)
#define SM8750_PLL_MODE_LOCK_DET        (1u << 31)
#define SM8750_PLL_OPMODE_RUN           1u

/* ===================================================================================================================
 * 10. GPU SMMU（ARM MMU-500，0x03DA0000，块内字节偏移）[LNX arm-smmu.h][SOC][ARCH 4.5]
 *     全局空间由 Gunyah 仿真：写可能被忽略、回读只记录；SMR/S2CR 路由与 CB 的 SCTLR.M 才是功能门槛。
 * =================================================================================================================== */
#define A830_SMMU_PAGE_BYTES            0x1000u
#define A830_SMMU_GR0_SCR0              0x000u
#define A830_SMMU_GR0_SACR              0x010u
#define A830_SMMU_GR0_IDR0              0x020u
#define A830_SMMU_GR0_IDR1              0x024u
#define A830_SMMU_GR0_IDR2              0x028u
#define A830_SMMU_GR0_SGFAR             0x040u         /* 64 位：低 +0x40，高 +0x44 */
#define A830_SMMU_GR0_SGFAR_HI          0x044u
#define A830_SMMU_GR0_SGFSR             0x048u         /* 只记录：读到的值写回清除，复读也只记录 */
#define A830_SMMU_GR0_SGFSRRESTORE      0x04Cu
#define A830_SMMU_GR0_SGFSYNR0          0x050u
#define A830_SMMU_GR0_SGFSYNR1          0x054u
#define A830_SMMU_GR0_SGFSYNR2          0x058u
#define A830_SMMU_GR0_STLBGSYNC         0x070u
#define A830_SMMU_GR0_STLBGSTATUS       0x074u
#define A830_SMMU_GR0_SMR(n)            (0x800u + 4u * (uint32_t)(n))       /* 物理 0x3DA0800 + 4n */
#define A830_SMMU_GR0_S2CR(n)           (0xC00u + 4u * (uint32_t)(n))       /* 物理 0x3DA0C00 + 4n */
#define A830_SMMU_GR1                   0x1000u                              /* 物理 0x3DA1000 */
#define A830_SMMU_GR1_CBAR(n)           (A830_SMMU_GR1 + 4u * (uint32_t)(n))
#define A830_SMMU_GR1_CBFRSYNRA(n)      (A830_SMMU_GR1 + 0x400u + 4u * (uint32_t)(n))   /* 低 16 位 = 故障 SID */
#define A830_SMMU_GR1_CBA2R(n)          (A830_SMMU_GR1 + 0x800u + 4u * (uint32_t)(n))
/* 上下文 bank：CBn = (全局页数 + n) × 4 KiB，全局页数 = 2 << IDR1[30:28]；SM8750 为 32 页，CB0 0x3DC0000、CB2 0x3DC2000 */
#define A830_SMMU_GLOBAL_PAGES(idr1)    (2u << (((uint32_t)(idr1) >> 28) & 7u))
#define A830_SMMU_CB_OFF(global_pages, n) (((uint32_t)(global_pages) + (uint32_t)(n)) * A830_SMMU_PAGE_BYTES)
#define A830_SMMU_CB_SCTLR              0x000u
#define A830_SMMU_CB_ACTLR              0x004u
#define A830_SMMU_CB_RESUME             0x008u
#define A830_SMMU_CB_TCR2               0x010u
#define A830_SMMU_CB_TTBR0              0x020u         /* 64 位；SCTLR.M=0 时可拆两次 32 位写 */
#define A830_SMMU_CB_TTBR0_HI           0x024u
#define A830_SMMU_CB_TTBR1              0x028u
#define A830_SMMU_CB_TTBR1_HI           0x02Cu
#define A830_SMMU_CB_TCR                0x030u         /* 先于 TTBR 写 */
#define A830_SMMU_CB_CONTEXTIDR         0x034u
#define A830_SMMU_CB_MAIR0              0x038u
#define A830_SMMU_CB_MAIR1              0x03Cu
#define A830_SMMU_CB_PAR                0x050u
#define A830_SMMU_CB_FSR                0x058u         /* 只记录（复位时写 1 清）；中断处理按 FAULT 掩码判断 */
#define A830_SMMU_CB_FSRRESTORE         0x05Cu
#define A830_SMMU_CB_FAR                0x060u         /* 64 位 */
#define A830_SMMU_CB_FAR_HI             0x064u
#define A830_SMMU_CB_FSYNR0             0x068u
#define A830_SMMU_CB_FSYNR1             0x06Cu
#define A830_SMMU_CB_TLBIVA             0x600u         /* 64 位：必须单条 64 位存储（写即失效） */
#define A830_SMMU_CB_TLBIVAA            0x608u         /* 64 位：同上 */
#define A830_SMMU_CB_TLBIASID           0x610u         /* 32 位 */
#define A830_SMMU_CB_TLBIALL            0x618u         /* 32 位 */
#define A830_SMMU_CB_TLBIVAL            0x620u         /* 64 位：必须单条 64 位存储 */
#define A830_SMMU_CB_TLBIVAAL           0x628u         /* 64 位：同上 */
#define A830_SMMU_CB_TLBSYNC            0x7F0u
#define A830_SMMU_CB_TLBSTATUS          0x7F4u

/* sCR0 / sACR / IDR [LNX][SOC 实测值] */
#define A830_SMMU_SCR0_CLIENTPD         (1u << 0)      /* = 1 时拒绝启动（所有流旁路） */
#define A830_SMMU_SCR0_GFRE             (1u << 1)
#define A830_SMMU_SCR0_GFIE             (1u << 2)
#define A830_SMMU_SCR0_USFCFG           (1u << 10)     /* = 0 时记录警告 */
#define A830_SMMU_SACR_CACHE_LOCK       (1u << 26)     /* MMU-500：= 1 时写回清零后的值，回读只记录 */
#define A830_SMMU_IDR0_S1TS             (1u << 30)
#define A830_SMMU_IDR0_SMS              (1u << 27)
#define A830_SMMU_IDR0_CTTW             (1u << 14)
#define A830_SMMU_IDR0_NUMSMRG(v)       ((uint32_t)(v) & 0xFFu)
#define A830_SMMU_IDR1_PAGESIZE_64K     (1u << 31)     /* 必须为 0（4K 页） */
#define A830_SMMU_IDR1_NUMPAGENDXB(v)   (((uint32_t)(v) >> 28) & 7u)
#define A830_SMMU_IDR1_NUMS2CB(v)       (((uint32_t)(v) >> 16) & 0xFFu)
#define A830_SMMU_IDR1_NUMCB(v)         ((uint32_t)(v) & 0xFFu)
#define A830_SMMU_IDR2_PTFS_4K          (1u << 12)
#define A830_SMMU_IDR2_OAS(v)           (((uint32_t)(v) >> 4) & 0xFu)
#define A830_SMMU_IDR2_IAS(v)           ((uint32_t)(v) & 0xFu)            /* 第二级输入（IPA）位数，不约束第一级上下文 */
#define A830_SMMU_IDR2_UBS(v)           (((uint32_t)(v) >> 8) & 0xFu)     /* 上游总线位数 = 第一级上下文输入上限（套件按它检查） */
#define A830_SMMU_OAS_BITS(enc)         ((enc) == 0u ? 32u : (enc) == 1u ? 36u : (enc) == 2u ? 40u : \
                                         (enc) == 3u ? 42u : (enc) == 4u ? 44u : 48u)
#define A830_SMMU_OUTPUT_BITS_MAX       40u            /* NativeAdrenoGpuContext 的上限；SM8750 实际 36 */
#define A830_SMMU_SCR0_SM8750           0x002E0406u    /* [SOC] 开机值 */
#define A830_SMMU_IDR0_SM8750           0x4C017E18u    /* [SOC] S1TS、SMS、CTTW，NUMSMRG 24，无硬件 ATOS */
#define A830_SMMU_IDR1_SM8750           0x40000015u    /* [SOC] 4K 页，NUMPAGENDXB 4，NUMCB 21，NUMS2CB 0 */
#define A830_SMMU_IDR2_SM8750           0x00005511u    /* [SOC] IAS = OAS = 36 位，UBS = 48 位 */
#define A830_SMMU_SGFSR_USF             (1u << 1)      /* 未识别流（例如漏了 SID1）；结合 CP 是否挂住判断 */

/* SMR / S2CR（15 位流 ID）[LNX][ARCH 4.5.3–4.5.5] */
#define A830_SMMU_SMR_VALID             (1u << 31)
#define A830_SMMU_SMR_ID_MASK           0x00007FFFu
#define A830_SMMU_SMR(sid, mask)        (A830_SMMU_SMR_VALID | (((uint32_t)(mask) & 0x7FFFu) << 16) | ((uint32_t)(sid) & 0x7FFFu))
#define A830_SMMU_SMR_MASK_OF(smr)      (((uint32_t)(smr) >> 16) & 0x7FFFu)
#define A830_SMMU_SMR_COVERS(smr, sid)  ((((uint32_t)(smr) & A830_SMMU_SMR_VALID) != 0u) && \
    (((((uint32_t)(smr) ^ (uint32_t)(sid)) & 0x7FFFu) & ~A830_SMMU_SMR_MASK_OF(smr)) == 0u))
#define A830_SMMU_S2CR_CBNDX(v)         ((uint32_t)(v) & 0xFFu)
#define A830_SMMU_S2CR_SHCFG_NSH        (3u << 8)
#define A830_SMMU_S2CR_TYPE(v)          (((uint32_t)(v) >> 16) & 3u)
#define A830_SMMU_S2CR_TYPE_TRANS       0u
#define A830_SMMU_S2CR_TYPE_BYPASS      1u
#define A830_SMMU_S2CR_TYPE_FAULT       2u
#define A830_SMMU_S2CR_TYPE_MASK        0x00030000u
#define A830_SMMU_S2CR_PRIVCFG(v)       (((uint32_t)(v) >> 24) & 3u)
#define A830_SMMU_S2CR_FAULT_NSH        0x00020300u    /* 认领：FAULT|SHCFG=NSH；读回 (v & TYPE_MASK) == 0x20000 */
#define A830_SMMU_S2CR_TRANS_NSH(cb)    (0x00000300u | ((uint32_t)(cb) & 0xFFu))   /* 路由：translate|NSH|cb */
#define A830_SMMU_S2CR_CHECK_MASK       0x030303FFu    /* PRIVCFG|TYPE|SHCFG|CBNDX：读回按它比 */
#define A830_SMMU_S2CR_BOOT_SM8750      0x03000018u    /* [SOC][HW] 开机 24 个 S2CR 都是它（因 SMR 无效而不生效） */

/* CBAR / CBA2R / SCTLR / TCR2 / TCR / MAIR / FSR（ARCH 4.5.4 表） */
#define A830_SMMU_CBAR_TYPE_S1_TRANS_S2_BYPASS (1u << 16)
#define A830_SMMU_CBAR_S1_MEMATTR_WB    (0xFu << 12)
#define A830_SMMU_CBAR_S1_BPSHCFG_NSH   (3u << 8)
#define A830_SMMU_CBAR_VALUE            0x0001F300u    /* S1 翻译 S2 旁路、MEMATTR=WB、BPSHCFG=NSH、VMID 0 */
#define A830_SMMU_CBA2R_VA64            (1u << 0)
#define A830_SMMU_SCTLR_M               (1u << 0)
#define A830_SMMU_SCTLR_TRE             (1u << 1)
#define A830_SMMU_SCTLR_AFE             (1u << 2)
#define A830_SMMU_SCTLR_CFRE            (1u << 5)
#define A830_SMMU_SCTLR_CFIE            (1u << 6)
#define A830_SMMU_SCTLR_HUPCF           (1u << 8)
#define A830_SMMU_SCTLR_ASIDPNE         (1u << 12)     /* [SOC] 不保留 */
#define A830_SMMU_SCTLR_ENABLE          0x00001167u    /* 终止模式；中断前提 CFIE */
#define A830_SMMU_SCTLR_READBACK        0x00000167u    /* 读回容忍 bit12 为 0 */
#define A830_SMMU_TCR2_PASIZE_36        1u
#define A830_SMMU_TCR2_AS               (1u << 4)      /* 16 位 ASID */
#define A830_SMMU_TCR2_TBI0             (1u << 5)      /* [SOC] 只读为 1：读回容忍 bit5/6 */
#define A830_SMMU_TCR2_TBI1             (1u << 6)
#define A830_SMMU_TCR2_SEP_UPSTREAM     (7u << 15)
#define A830_SMMU_TCR2_VALUE            0x00038011u
#define A830_SMMU_TCR_T0SZ(n)           ((uint32_t)(n) & 0x3Fu)
#define A830_SMMU_TCR_EPD0              (1u << 7)
#define A830_SMMU_TCR_IRGN0(v)          (((uint32_t)(v) & 3u) << 8)
#define A830_SMMU_TCR_ORGN0(v)          (((uint32_t)(v) & 3u) << 10)
#define A830_SMMU_TCR_SH0(v)            (((uint32_t)(v) & 3u) << 12)
#define A830_SMMU_TCR_TG0_4K            (0u << 14)
#define A830_SMMU_TCR_T1SZ(n)           (((uint32_t)(n) & 0x3Fu) << 16)
#define A830_SMMU_TCR_A1                (1u << 22)
#define A830_SMMU_TCR_EPD1              (1u << 23)     /* GPU 运行后 CB0 的 EPD1 会被清（推测 SQE 经 aperture 写），巡检容忍 */
#define A830_SMMU_TCR_IRGN1(v)          (((uint32_t)(v) & 3u) << 24)
#define A830_SMMU_TCR_ORGN1(v)          (((uint32_t)(v) & 3u) << 26)
#define A830_SMMU_TCR_SH1(v)            (((uint32_t)(v) & 3u) << 28)
#define A830_SMMU_TCR_TG1_4K            (2u << 30)
#define A830_SMMU_RGN_WBWA              1u
#define A830_SMMU_SH_IS                 3u
#define A830_SMMU_TCR_CB0_VALUE         0xB5A03520u    /* T0SZ=T1SZ=32、WBWA、IS、4K/4K、EPD1=1（上半区空表保护） */
#define A830_SMMU_TCR_CB2_VALUE         0x80803520u    /* T0SZ=32、WBWA、IS、TG1=4K、EPD1=1 */
#define A830_SMMU_MAIR_IDX_NC           0u             /* 0x44 */
#define A830_SMMU_MAIR_IDX_WB           1u             /* 0xFF */
#define A830_SMMU_MAIR0_VALUE           0x0000FF44u    /* 与套件 ArmSmmu4KResult.mair 一致 */
#define A830_SMMU_FSR_TF                (1u << 1)
#define A830_SMMU_FSR_AFF               (1u << 2)
#define A830_SMMU_FSR_PF                (1u << 3)
#define A830_SMMU_FSR_EF                (1u << 4)
#define A830_SMMU_FSR_TLBMCF            (1u << 5)
#define A830_SMMU_FSR_TLBLKF            (1u << 6)
#define A830_SMMU_FSR_ASF               (1u << 7)
#define A830_SMMU_FSR_UUT               (1u << 8)
#define A830_SMMU_FSR_FORMAT_MASK       (3u << 9)      /* 读到 0x400 是格式字段，不是故障 */
#define A830_SMMU_FSR_SS                (1u << 30)     /* 仅 stall 模式：写 RESUME=1 终止事务 */
#define A830_SMMU_FSR_MULTI             (1u << 31)
#define A830_SMMU_FSR_FAULT_MASK        0xC00001FEu    /* 有效故障位，也是复位时写 1 清的值 */
#define A830_SMMU_RESUME_TERMINATE      1u
#define A830_SMMU_TLBSTATUS_SACTIVE     (1u << 0)
#define A830_SMMU_TLBI_ALL_VALUE        0xFFFFFFFFu    /* TLBIALL 写入值（ARCH 4.5.4 第 12 步） */
#define A830_SMMU_TLBI_PAGE_LIMIT       512u           /* ≤512 页逐页 TLBIVAL，更多用 TLBIASID（ARCH 4.6.4） */
#define A830_SMMU_TTBR(asid, pa)        ((((uint64_t)(asid) & 0xFFFFull) << 48) | ((uint64_t)(pa) & 0x0000FFFFFFFFFFFFull))
#define A830_SMMU_TLBIVA_VALUE(iova, asid) ((((uint64_t)(iova) >> 12) & 0x00000FFFFFFFFFFFull) | \
                                            (((uint64_t)(asid) & 0xFFFFull) << 48))
#define A830_SMMU_ACTLR_DEFAULT         0x00000303u    /* [SOC] 可写回；pagani-gpu-actlr 可改 */
#define A830_SMMU_ACTLR_DT              0x0000032Bu    /* DT qcom,actlr；作者写它并要求原样读回而失败 */
#define A830_SMMU_APERTURE_ARG(cb)      (0xFFFF0000u | ((uint32_t)(cb) & 0xFFFFu))   /* CP_SMMU_APERTURE 的 x2；x3..x5 = 0xFFFFFFFF */
/* 流与 bank（板级输入的期望值，ARCH 1.3 第 1、3 条）：ASID = CB 号 + 1 */
#define A830_SID_GPU                    0u             /* gfx3d_user */
#define A830_SID_LPAC                   1u             /* gfx3d_lpac：必须路由到 CB0，否则 BR/BV 卡在 PC 0xE3/0x4A */
#define A830_SID_GMU                    5u
#define A830_SMMU_SLOT_GPU              0u
#define A830_SMMU_SLOT_LPAC             3u
#define A830_SMMU_SLOT_GMU              1u
#define A830_SMMU_SLOT_RESERVED         2u             /* 留给 TZ 的安全流，顺延时跳过 */
#define A830_SMMU_CB_GPU                0u
#define A830_SMMU_CB_GMU                2u             /* 避开 CB1（annibale 写 CB1 触发同步外部中止） */
#define A830_SMMU_ASID(cb)              ((uint32_t)(cb) + 1u)

/* ===================================================================================================================
 * 11. GPU 地址空间（CB0）、PTE 与 memptrs（ARCH 4.6、4.13.1）
 * =================================================================================================================== */
#define A830_GPU_VA_SQE                 0x0000000000100000ull  /* 0x0–0xFFFFF 不映射（空指针保护） */
#define A830_GPU_VA_SQE_BYTES           0x0001C000u
#define A830_GPU_VA_AQE                 0x0000000000180000ull
#define A830_GPU_VA_AQE_BYTES           0x00007000u
#define A830_GPU_VA_RING                0x0000000000200000ull  /* 按大小对齐 */
#define A830_GPU_VA_RING_BYTES          A830_RING_BYTES
#define A830_GPU_VA_MEMPTRS             0x0000000000300000ull
#define A830_GPU_VA_PWRUP               0x0000000000301000ull
#define A830_GPU_VA_SCRATCH             0x0000000000302000ull
#define A830_GPU_VA_KERNEL_PAGE_BYTES   0x00001000u            /* memptrs、pwrup、scratch 各一页 */
#define A830_GPU_VA_GMEM                A830_GMEM_BASE         /* 0x0400_0000–0x04BF_FFFF：严禁映射 */
#define A830_GPU_VA_CLIENT_BASE         0x0000000020000000ull
#define A830_GPU_VA_CLIENT_END_4G       0x0000000100000000ull  /* pagani-gpu-va=4 与默认的退路：4 GiB 光圈（不含） */
#define A830_GPU_VA_CLIENT_END_2G       0x0000000080000000ull  /* pagani-gpu-va=2 回退 */
#define A830_GPU_VA_CLIENT_END_16G      0x0000000400000000ull  /* 默认（pagani-gpu-va=16）：34 位输入（须 IDR2.UBS ≥ 34 位，SM8750 为 48），32 个客户端窗口 */
#define A830_GPU_VA_UCHE_TRAP           A830_UCHE_TRAP_ADDR
/* PTE（4K 页描述符；ARCH 4.6.3） */
#define A830_PTE_PAGE                   0x3ull                 /* bits[1:0] = 0b11 */
#define A830_PTE_ATTRINDX(i)            (((uint64_t)(i) & 7ull) << 2)
#define A830_PTE_AP_EL0                 (1ull << 6)            /* AP[1]：非特权可访问 */
#define A830_PTE_AP_RDONLY              (1ull << 7)            /* AP[2] */
#define A830_PTE_SH_OUTER               (2ull << 8)
#define A830_PTE_SH_INNER               (3ull << 8)
#define A830_PTE_AF                     (1ull << 10)
#define A830_PTE_NG                     (1ull << 11)           /* 必须置 1 */
#define A830_PTE_PXN                    (1ull << 53)
#define A830_PTE_XN                     (1ull << 54)
#define A830_PTE_USER_RW_NC             0x0060000000000E43ull  /* WC 缓冲、GPU 写的导入页 */
#define A830_PTE_USER_RO_NC             0x0060000000000EC3ull
#define A830_PTE_KERNEL_RW_NC           0x0060000000000E03ull  /* memptrs、pwrup、scratch */
#define A830_PTE_KERNEL_RO_NC           0x0060000000000E83ull  /* ring */
#define A830_PTE_UCODE                  0x0000000000000EC3ull  /* 只读、可执行、非特权可读 */
#define A830_PTE_COHERENT_RO            0x0060000000000FC7ull  /* WB+IS */
#define A830_PTE_COHERENT_RW            0x0060000000000F47ull
/* memptrs 页（Linux struct msm_rbmemptrs 布局） */
#define A830_MEMPTRS_RPTR               0x00u                  /* BR RPTR 影子 */
#define A830_MEMPTRS_FENCE              0x04u                  /* BR 完成序号 */
#define A830_MEMPTRS_BV_RPTR            0x08u
#define A830_MEMPTRS_BV_FENCE           0x0Cu
#define A830_MEMPTRS_STATS              0x10u
/* 本机钉选固件的尺寸（只供校验与测试；哈希在 DEV/scripts/data/sm8750-gpu-inputs.json） */
#define A830_FW_HEADER_BYTES            4u                     /* SQE/AQE 文件去掉首 4 字节再拷 */
#define A830_SQE_FW_FILE_BYTES          111892u                /* gen80000_sqe.fw */
#define A830_SQE_IMAGE_BYTES            (A830_SQE_FW_FILE_BYTES - A830_FW_HEADER_BYTES)   /* 0x1B510 */
#define A830_AQE_FW_FILE_BYTES          25340u                 /* gen80000_aqe.fw */
#define A830_AQE_IMAGE_BYTES            (A830_AQE_FW_FILE_BYTES - A830_FW_HEADER_BYTES)   /* 0x62F8 */
#define A830_GMU_FW_FILE_BYTES          88896u                 /* gen80000_gmu.bin */
#define A830_ZAP_FW_FILE_BYTES          12088u                 /* gen80000_zap.mbn = 0x2F38 */
#define A830_ZAP_PAS_ID                 13u
#define A830_ZAP_MEM_SETUP_BYTES        0x00001000u            /* PAS_MEM_SETUP(13, 0x9B09A000, 0x1000) */
#define A830_ZAP_METADATA_BYTES         0x00000FCCu            /* [0,0x94) + [0x2000,0x2F38) */

/* ===================================================================================================================
 * 12. GMU 地址空间（CB2，经 SID5）与固件格式（ARCH 4.7.1–4.7.2）[KGSL gen8_gmu_parse_fw]
 * =================================================================================================================== */
#define A830_GMU_VA_ITCM                0x00000000u            /* TCM，寄存器写入，不映射 */
#define A830_GMU_ITCM_BYTES             0x00004000u
#define A830_GMU_VA_DTCM                0x10004000u            /* TCM，寄存器写入，不映射 */
#define A830_GMU_DTCM_BYTES             0x00004000u
#define A830_GMU_VA_PERSIST             0x60000000u            /* 覆盖两块 PERSIST（各 4K），只在首次分配时清零 */
#define A830_GMU_PERSIST_BYTES          0x00004000u
#define A830_GMU_VA_LOG                 0x60004000u            /* 16 KiB（Linux/作者）；KGSL 用 64 KiB */
#define A830_GMU_LOG_BYTES              0x00004000u
#define A830_GMU_VA_HFI                 0x60008000u            /* 队列表 + 3 个队列 */
#define A830_GMU_HFI_BYTES              0x00004000u
#define A830_GMU_VA_DEBUG               0x60400000u            /* 28 KiB，固件不要求，不分配 */
#define A830_GMU_FW_BLOCK_HDR_BYTES     16u                    /* {u32 addr, size, type, value} */
#define A830_GMU_FW_TYPE_PREALLOC       1u                     /* size==0 的元数据块：GMU VA addr 处要 value 字节 */
#define A830_GMU_FW_TYPE_PREALLOC_PERSIST 7u
#define A830_GMU_FW_PAD_VALUE           0xEFBEADDEu            /* 数据块的 type/value 填充值，忽略 */
#define A830_GMU_ITCM_BOOT_WORD0        0x10007F80u            /* 引导桩初始 SP；ITCM 前 4 字 0x10007F80 0x105 0x145 0x159 */
#define A830_GMU_ITCM_BOOT_WORD1        0x00000105u            /* 复位向量 */
#define A830_GMU_CORE_FW_VERSION        0x50010019u            /* 5.1.25 */
#define A830_GMU_PWR_FW_VERSION         0x50000112u
#define A830_GMU_HFI_VERSION            0x20070001u            /* HFI 2.7.1 */
#define A830_GMU_VER_MAJOR(v)           (((uint32_t)(v) >> 28) & 0xFu)
#define A830_GMU_VER_MINOR(v)           (((uint32_t)(v) >> 16) & 0xFFFu)
#define A830_GMU_VER_STEP(v)            ((uint32_t)(v) & 0xFFFFu)
#define A830_GMU_CORE_NEW_INIT_AFTER    0x20010004u            /* core 大于它：FW_INIT_RESULT 用 (v & 0x1FF) == 0x100 */
#define A830_GMU_HFI_TABLE_FORMAT_MIN   0x20060001u            /* HFI ≥ 它：H2F_MSG_TABLE 表格式 */
#define A830_GMU_HFI_V5_MINOR_ABOVE     4u                     /* HFI 小版本 > 4：FEATURE/GET/SET 应答为 v5 格式 */

/* ===================================================================================================================
 * 13. HFI（共享内存在 GMU VA 0x60008000，16 KiB）[KGSL adreno_hfi.h、adreno_gen8_hfi.c][LNX a6xx_hfi.*][ARCH 4.7.5–4.7.6]
 * =================================================================================================================== */
#define A830_HFI_HDR(id, type, dwords, seq) \
    ((((uint32_t)(seq) & 0xFFFu) << 20) | (((uint32_t)(type) & 0xFu) << 16) | \
     (((uint32_t)(dwords) & 0xFFu) << 8) | ((uint32_t)(id) & 0xFFu))
#define A830_HFI_HDR_ID(h)              ((uint32_t)(h) & 0xFFu)
#define A830_HFI_HDR_DWORDS(h)          (((uint32_t)(h) >> 8) & 0xFFu)     /* 含头，最大 255 */
#define A830_HFI_HDR_TYPE(h)            (((uint32_t)(h) >> 16) & 0xFu)
#define A830_HFI_HDR_SEQ(h)             (((uint32_t)(h) >> 20) & 0xFFFu)
#define A830_HFI_HDR_RECORD             (1u << 19)              /* 比较 ACK 头前两边都先清掉 */
#define A830_HFI_MSG_CMD                0u
#define A830_HFI_MSG_ACK                1u
/* 消息号（H2F 主机发，F2H 固件发）。不发 INIT(0)、FW_VER(1)、PERF_TBL(4)（只属 legacy 或 HFI < 2.6.1） */
#define A830_HFI_H2F_BW_VOTE_TBL        3u
#define A830_HFI_H2F_ACD_TBL            7u
#define A830_HFI_H2F_CLX_TBL            8u
#define A830_HFI_H2F_START              10u
#define A830_HFI_H2F_FEATURE_CTRL       11u
#define A830_HFI_H2F_GET_VALUE          12u
#define A830_HFI_H2F_SET_VALUE          13u
#define A830_HFI_H2F_CORE_FW_START      14u
#define A830_HFI_H2F_TABLE              15u
#define A830_HFI_H2F_GX_BW_PERF_VOTE    30u
#define A830_HFI_H2F_FW_HALT            32u
#define A830_HFI_H2F_PREPARE_SLUMBER    33u
#define A830_HFI_F2H_MEM_ALLOC          20u             /* RB 模式不支持：收到即以原因码 22 失败 */
#define A830_HFI_F2H_ERR                100u            /* {hdr, code, data[16]}，致命 */
#define A830_HFI_F2H_DEBUG              101u
#define A830_HFI_F2H_LOG_BLOCK          102u
#define A830_HFI_F2H_GMU_CNTR_REGISTER  110u            /* RB 模式不支持：同 MEM_ALLOC */
#define A830_HFI_F2H_GMU_CNTR_RELEASE   111u
#define A830_HFI_TABLE_BW_VOTE          0u
#define A830_HFI_TABLE_GPU_PERF         1u
#define A830_HFI_TABLE_ACD              3u
#define A830_HFI_FEATURE_IFPC           9u
#define A830_HFI_FEATURE_BCL            11u
#define A830_HFI_FEATURE_ACD            12u
#define A830_HFI_FEATURE_CLX            21u
#define A830_HFI_VALUE_LOG_EVENT_ON     112u
#define A830_HFI_VALUE_GMU_AB_VOTE      122u            /* GET_VALUE：v5 word2==1 表示支持 AB 投票 */
/* v5 应答（FEATURE_CTRL/GET_VALUE/SET_VALUE，HFI 小版本 > 4）：word2 返回值、word3 错误码；其他消息 word2 是错误码 */
#define A830_HFI_V5_SUCCESS             0u
#define A830_HFI_V5_ERROR_TIMEOUT       6u
#define A830_HFI_V5_ERROR_NOT_SUPPORTED 7u
#define A830_HFI_V5_ERROR_NO_ENTRY      8u              /* 未定义的消息（ARCH 4.7.5 说的“8 不支持”即此值） */
#define A830_HFI_IFPC_HYSTERESIS        0x00001680u     /* 5760 XO = 300 µs（P2 才开 IFPC） */
#define A830_HFI_INVALID_DCVS_IDX       0xFFu           /* freq = 0xFF 表示不变 */
#define A830_HFI_BW_NO_AB               0xFFFF0000u     /* bw 字段高 16 位：AB 无效 */
#define A830_HFI_BW_UNCHANGED           0xFFFF00FFu     /* bw = 它表示不变 */
#define A830_HFI_GX_BW_PERF_ACK_BLOCK   1u              /* GX_BW_PERF_VOTE 的 ack_type */
/* 消息长度（dword，含头）与设计给出的头（ARCH 4.7.6） */
#define A830_HFI_PERF_TABLE_DWORDS(n_gx, n_cx) (3u + 2u + 3u * (uint32_t)(n_gx) + 2u + 2u * (uint32_t)(n_cx))  /* 12 档 + CX 3 档 = 49 */
#define A830_HFI_BW_TABLE_DWORDS        160u            /* 1 + 5 + 6 + 2×6 + 8 + 16×8 */
#define A830_HFI_BW_MAX_CNOC_CMDS       6u
#define A830_HFI_BW_MAX_CNOC_LEVELS     2u
#define A830_HFI_BW_MAX_DDR_CMDS        8u
#define A830_HFI_BW_MAX_LEVELS          16u
#define A830_HFI_ACD_TABLE_DWORDS       37u             /* {hdr, version=1, enable_by_level, stride=1, num_levels, data[32]} */
#define A830_HFI_ACD_ENABLE_BY_LEVEL    0x00000FF0u     /* 11 档表：GMU 档 4..11 有 ACD 值 */
#define A830_HFI_FEATURE_CTRL_DWORDS    4u
#define A830_HFI_GET_VALUE_DWORDS       3u
#define A830_HFI_CORE_FW_START_DWORDS   2u
#define A830_HFI_START_DWORDS           1u
#define A830_HFI_GX_BW_PERF_VOTE_DWORDS 4u              /* {hdr, ack_type, freq, bw} */
/* 队列表（本驱动默认布局；只在首个 ACK 都收不到时改试 KGSL 布局） */
#define A830_HFI_QTBL_HDR_DWORDS        6u              /* version 0, size, qhdr0_offset 6, qhdr_size 12, num_q 3, num_active_q 3 */
#define A830_HFI_QTBL_SIZE_BYTES        168u            /* Linux 写字节数：(6 + 3×12)×4；固件不依赖 */
#define A830_HFI_QHDR_DWORDS            12u
#define A830_HFI_QHDR_STATUS            0u              /* 队列头字段下标 */
#define A830_HFI_QHDR_START_ADDR        1u
#define A830_HFI_QHDR_TYPE              2u
#define A830_HFI_QHDR_QUEUE_SIZE        3u
#define A830_HFI_QHDR_MSG_SIZE          4u
#define A830_HFI_QHDR_DROPPED           5u
#define A830_HFI_QHDR_RX_WATERMARK      6u
#define A830_HFI_QHDR_TX_WATERMARK      7u
#define A830_HFI_QHDR_RX_REQUEST        8u
#define A830_HFI_QHDR_TX_REQUEST        9u
#define A830_HFI_QHDR_READ_INDEX        10u
#define A830_HFI_QHDR_WRITE_INDEX       11u
#define A830_HFI_QUEUE_COUNT            3u
#define A830_HFI_QUEUE_DWORDS           1024u           /* 4 KiB */
#define A830_HFI_QUEUE_ID_CMD           0u
#define A830_HFI_QUEUE_ID_MSG           1u
#define A830_HFI_QUEUE_ID_DBG           2u
#define A830_HFI_QUEUE_PRIORITY         10u
#define A830_HFI_QUEUE_TYPE(prio, id)   ((((uint32_t)(prio) & 0xFFu) << 8) | ((uint32_t)(id) & 0xFFu))   /* 0xA00/0xA01/0xA02 */
#define A830_HFI_QUEUE_OFFSET(id)       (0x1000u + 0x1000u * (uint32_t)(id))     /* cmd +0x1000、msg +0x2000、dbg +0x3000 */
#define A830_HFI_KGSL_QUEUE_OFFSET(id)  (848u + 4096u * (uint32_t)(id))          /* 备选布局（KGSL） */
#define A830_HFI_QUEUE_ENABLED          1u
#define A830_HFI_FILL_WORD              0xFAFAFAFAu     /* 不足 4 dword 的倍数时填充 */

/* ===================================================================================================================
 * 14. RPMh 票的编码与 DDR 档（ARCH 4.3.2、4.7.7）[LNX tcs.h、bcm-voter.c、sm8750.c][KGSL adreno_rpmh.c、adreno_gen8_rpmh.c]
 *     发送与 cmd-db 解析在 platform/sm8750/Sm8750Rpmh.*；两种 BCM 用途（GMU 带宽表、主机票）不能混用。
 * =================================================================================================================== */
#define A830_BCM_TCS_CMD(commit, valid, x, y) \
    ((((uint32_t)(commit) & 1u) << 30) | (((uint32_t)(valid) & 1u) << 29) | \
     (((uint32_t)(x) & 0x3FFFu) << 14) | ((uint32_t)(y) & 0x3FFFu))
#define A830_BCM_VOTE_MAX               0x00003FFFu
#define A830_ARC_VOTE(pri_idx, sec_idx, vlvl) \
    ((((uint32_t)(vlvl) & 0xFFFFu) << 16) | (((uint32_t)(sec_idx) & 0xFFu) << 8) | ((uint32_t)(pri_idx) & 0xFFu))
#define A830_DEP_VOTE(cx_idx, mx_idx)   (0xFFFFC000u | (((uint32_t)(mx_idx) & 0x3Fu) << 8) | ((uint32_t)(cx_idx) & 0xFFu))  /* KGSL，默认 */
#define A830_DEP_VOTE_LINUX(cx_idx, mx_idx) (0x0FFFC000u | (((uint32_t)(mx_idx) & 0x3Fu) << 8) | ((uint32_t)(cx_idx) & 0xFFu))  /* pagani-gpu-hfi bit0 */
#define A830_DEP_VOTE_OFF               0xFFFFFFFFu     /* GMU 档 0（关）的依赖票 */
#define A830_DEP_CX_NONE                0xFFu           /* 本机 DT 没有 qcom,cx-level */
#define A830_ARC_LOW_SVS                0x40u           /* pagani-gpu-rails=0 时取首个 vlvl ≥ 它的 hlvl */
#define A830_ACV_GPU_PERFMODE           (1u << 2)       /* 只用于 GMU 带宽表（第 7 级起） */
#define A830_ACV_HLOS_ENABLE            (1u << 0)       /* 只用于主机票（Linux sm8750 bcm_acv.enable_mask） */
#define A830_ACV_PERFMODE_KBPS          10687500u       /* MHZ_TO_KBPS(2736, 4) = DDR 第 7 级 */
#define A830_CN0_VOTE_ON                0x60000001u     /* 主机 CN0 票 = GMU CNOC 表第 1 级 */
#define A830_CN0_VOTE_OFF               0x40000000u     /* 只出现在 GMU CNOC 表第 0 级，主机票里永不出现 */
#define A830_SH0_BUSWIDTH               16u             /* Linux a8xx_catalog A830 bcms */
#define A830_MC0_BUSWIDTH               4u
#define A830_DDR_LEVELS                 12u
#define A830_DDR_BOOT_LEVEL             11u             /* pagani-gpu-ddr 默认（作者实测）；底线默认等于它 */
#define A830_DDR_TOP_KBPS               18597656u       /* 第 11 级 */
#define A830_DDR_CHANNELS               4u              /* KGSL num_ddr_channels：AB 量化用 */
#define A830_GMU_COMMON_LEVELS          11u             /* 各 bin 共有的 ≤900 MHz 档 */
#define A830_GMU_INITIAL_LEVEL          2u              /* 222 MHz；GMU 档 = 11 − DT 序号，档 0 = 关 */
#define A830_GMU_INITIAL_BUS            2u
#define A830_XO_HZ                      19200000u

/* ===================================================================================================================
 * 15. SCM 取值（ARCH 4.4.1；SMC 功能号与调用层在 platform/sm8750/Sm8750Scm.h）
 * =================================================================================================================== */
#define A830_SCM_GPU_REQ_ALWAYS_EN      (1u << 0)
#define A830_SCM_GPU_REQ_BCL_EN         (1u << 1)       /* 先不请求（需要配套 HFI 表） */
#define A830_SCM_GPU_REQ_CLX_EN         (1u << 2)       /* 先不请求 */
#define A830_SCM_GPU_REQ_TSENSE_EN      (1u << 3)
#define A830_SCM_GPU_INIT_REQ           0x00000009u     /* ALWAYS_EN|TSENSE_EN；成功 x0 ∈ {0, −4}，不看 x1 [HW] */

/* ===================================================================================================================
 * 16. 中断（GIC INTID = SPI + 32；板级输入的期望值）[DT][ARCH 4.12.2]
 * =================================================================================================================== */
#define A830_IRQ_GPU                    332u            /* kgsl_3d0_irq，电平；唯一硬依赖 */
#define A830_IRQ_CX_HOST                112u            /* cx_host_irq，电平，可选 */
#define A830_IRQ_GMU_HFI                336u            /* 电平，可选 */
#define A830_IRQ_GMU                    337u            /* 电平，可选 */
#define A830_IRQ_SMMU_GLOBAL            706u            /* 首版不挂 */
#define A830_IRQ_SMMU_CB(n)             (710u + (uint32_t)(n))   /* CB0..CB3 = 710..713，电平，可选 */
#define A830_IRQ_SCM_WAITQ              994u            /* 边沿，可选 */
#define A830_IRQ_MDSS                   115u            /* 显示 TE（M12，显示侧） */
#define A830_IRQ_STORM_GPU              1000u           /* 连续无进展次数阈值：332 */
#define A830_IRQ_STORM_OTHER            64u             /* 其余电平中断 */

/* ===================================================================================================================
 * 17. 等待上限与步进（微秒）[ARCH 3.4、4.x][KGSL][LNX]；全部为睡眠轮询，启动期之外不忙等
 * =================================================================================================================== */
#define A830_T_GDSC_US                  2000u
#define A830_T_PLL_LOCK_US              1500u
#define A830_T_RCG_US                   500u
#define A830_T_BRANCH_US                500u
#define A830_T_RPMH_TCS_POLL_US         1000u           /* apps DRV-2 TCS：每 1 ms 一次，最多 2000 次 */
#define A830_T_RPMH_TCS_POLLS           2000u
#define A830_T_SCM_US                   1000000u        /* 每个 SMC 请求总预算 */
#define A830_T_SCM_EBUSY_SLEEP_US       30000u
#define A830_SCM_EBUSY_RETRIES          20u
#define A830_T_SMMU_TLBSYNC_US          10000u          /* 先自旋 10 次，再 1/2/4… µs 退避 */
#define A830_SMMU_TLBSYNC_SPINS         10u
#define A830_T_FW_INIT_US               100000u         /* B7 */
#define A830_T_HFI_INIT_US              100000u         /* B8，设计档 */
#define A830_T_HFI_INIT_ANNIBALE_US     500000u         /* B8，annibale 档（kernel-sequences 5.2 第 4 步、第 14 节第 22 行，K/adreno8_hfi.c:345） */
#define A830_T_HFI_ACK_US               1000000u        /* 每条 HFI（KGSL HFI_RSP_TIMEOUT） */
#define A830_T_OOB_US                   100000u
#define A830_T_GX_ON_US                 1000u           /* OOB 后 GX 上电确认，10 µs 步进 */
#define A830_T_GX_ON_STEP_US            10u
#define A830_T_RSCC_US                  100000u         /* RSCC 睡眠/唤醒各步 */
#define A830_T_RSCC_TCS_FORCE_OFF_US    1000u           /* 强制关断：TCS0..9 各等 ≤1 ms，超时不判失败 */
#define A830_T_RSCC_TCS_SLUMBER_US      2000u           /* slumber（P1）：各 2 ms */
#define A830_T_GX_GDSC_OFF_US           2000u           /* 强制关断第 6 步轮询 CFG_GDSCR bit15 */
#define A830_T_GBIF_HALT_US             100000u
#define A830_T_LOWEST_IDLE_US           100000u
#define A830_T_CX_IDLE_US               100000u
#define A830_T_CP_IDLE_US               2000000u        /* CP_ME_INIT、SET_SECURE_MODE 后的空闲判据 */
#define A830_T_CGC_P2S_US               10u
#define A830_T_SELFTEST_US              100000u         /* S8 栅栏中断自检、V9b/V9c 各项 */
#define A830_T_LLCC_US                  100u            /* LLCC_STATUS_READ_DELAY */
#define A830_WPTR_FENCE_RETRY_US        10u             /* WPTR 围栏写：WRITEDROPPED0 置位就 10 µs 后重写 */
#define A830_WPTR_FENCE_RETRIES         200u            /* 200 次仍失败返回错误并进入恢复 */
#define A830_WPTR_FENCE_LOG_AFTER       100u
#define A830_POLL_STEP_US               100u            /* 通用睡眠轮询步进 */
#define A830_FENCE_WAIT_SLICE_US        16000u          /* 栅栏等待的保险时钟，只防丢中断 */
#define A830_FENCE_POLL_SLICE_US        1000u           /* 332 未挂、被风暴保护关掉或退回轮询时，栅栏等待按此间隔轮询 */

/* ===================================================================================================================
 * 18. 其他 SoC 块：LLCC v6（M11）、TSENS（M11）、AOSS QMP（M3 可选）[LNX llcc-qcom.c、tsens-v2.c、qcom_aoss.c][DT][ARCH 4.15–4.17]
 *     平台层（platform/sm8750）保持自包含，可以不包含本文件；这里的值是同一组事实的唯一记录。
 * =================================================================================================================== */
#define SM8750_LLCC0_PHYS               0x24800000ull   /* 4 个通道：0x24800000、0x25800000、0x24C00000、0x25C00000，各 2 MiB */
#define SM8750_LLCC_BCAST_OR_PHYS       0x26800000ull
#define SM8750_LLCC_BCAST_AND_PHYS      0x26C00000ull
#define SM8750_LLCC_WINDOW_BYTES        0x00200000u
#define SM8750_LLCC_V6_COMMON_HW_INFO   0x64000u        /* 广播 OR 上读，期望 ≥ 0x06000000 */
#define SM8750_LLCC_V6_COMMON_STATUS0   0x6400Cu        /* llcc0 上读，[31:28] = bank 数（期望 4） */
#define SM8750_LLCC_VERSION_6           0x06000000u
#define SM8750_LLCC_NUM_BANKS(status0)  (((uint32_t)(status0) >> 28) & 0xFu)
#define SM8750_LLCC_V6_ATTR0(s)         (0x41000u + 64u * (uint32_t)(s))   /* res_ways */
#define SM8750_LLCC_V6_ATTR1(s)         (0x41008u + 64u * (uint32_t)(s))   /* bonus_ways */
#define SM8750_LLCC_V6_ATTR2(s)         (0x41010u + 64u * (uint32_t)(s))   /* cache_mode | probe<<4 | fixed<<8 | prio<<12 */
#define SM8750_LLCC_V6_ATTR3(s)         (0x41014u + 64u * (uint32_t)(s))   /* max_cap 字节 / bank 数 >> 6 */
#define SM8750_LLCC_V6_WRS_EN           0x42080u        /* 位 = 切片号 */
#define SM8750_LLCC_V6_WRS_CACHEABLE_EN 0x42088u
#define SM8750_LLCC_TRP_ACT_CTRL(s)     (0x1000u * (uint32_t)(s))          /* 写 0x3（ACTIVATE<<1|TRIG），再写 0x2 */
#define SM8750_LLCC_TRP_STATUS(s)       (0x1000u * (uint32_t)(s) + 4u)     /* AND 基址 bit4 ACT_COMPLETE；OR 基址 bit1 DEACTIVATE 清零 */
#define SM8750_LLCC_TRP_ACT_CLEAR(s)    (0x1000u * (uint32_t)(s) + 8u)     /* 写 1 */
#define SM8750_LLCC_ACT_TRIG            (1u << 0)
#define SM8750_LLCC_ACT_OPCODE_ACTIVATE (1u << 1)       /* opcode 1 << 1 */
#define SM8750_LLCC_STATUS_ACT_COMPLETE (1u << 4)
#define SM8750_LLCC_STATUS_DEACTIVATE   (1u << 1)
#define SM8750_LLCC_ATTR2_FIXED_SIZE    (1u << 8)
#define SM8750_LLCC_ATTR2_PRIORITY(p)   (((uint32_t)(p) & 7u) << 12)
#define SM8750_LLCC_ATTR3_MAX_CAP(kib, banks) (((uint32_t)(kib) * 1024u / (uint32_t)(banks)) >> 6)
#define SM8750_LLCC_SCID_CPUSS          1u              /* 5120 KiB，固件 activate_on_init（只读看状态） */
#define SM8750_LLCC_SCID_GPU            9u              /* 5632 KiB，prio 1，fixed，bonus 全开，write_scid(_cacheable)_en */
#define SM8750_LLCC_SCID_GPUHTW         11u             /* 512 KiB，prio 1，fixed，bonus 全开 */
#define SM8750_LLCC_SCID_WRCACHE        31u
#define SM8750_LLCC_GPU_MAX_CAP_KIB     5632u
#define SM8750_LLCC_GPUHTW_MAX_CAP_KIB  512u
#define SM8750_LLCC_ATTR2_GPU_VALUE     0x00001100u     /* fixed<<8 | prio(1)<<12 */
#define SM8750_LLCC_ATTR3_GPU_VALUE     0x00005800u     /* 5632 KiB / 4 bank */
#define SM8750_LLCC_ATTR3_GPUHTW_VALUE  0x00000800u
#define SM8750_TSENS_TM_SN_STATUS(n)    (0xA0u + 4u * (uint32_t)(n))       /* gpuss-0..7 = tsens2 传感器 1..8 */
#define SM8750_TSENS_STATUS_VALID       (1u << 21)
#define SM8750_TSENS_STATUS_TEMP_MASK   0x00000FFFu     /* 有符号 12 位，单位 0.1 °C */
#define SM8750_TSENS_GPU_SENSOR_FIRST   1u
#define SM8750_TSENS_GPU_SENSOR_LAST    8u
#define SM8750_AOSS_QMP_MAGIC           0x4D41494Cu     /* 描述符 +0x00 */
#define SM8750_AOSS_QMP_VERSION         1u              /* 描述符 +0x04 */
#define SM8750_AOSS_QMP_MCORE_MBOX_SIZE 0x34u           /* 描述符内字节偏移 */
#define SM8750_AOSS_QMP_MCORE_MBOX_OFFSET 0x38u
#define SM8750_AOSS_QMP_MSG_BYTES       64u             /* 零填充字符串写到 mbox+4，长度写 mbox+0（Linux QMP_MSG_LEN） */
#define SM8750_IPCC_SEND_ID             0x0Cu           /* AOSS 门铃：写 client 0 / signal 0 = 0 */

/* ===================================================================================================================
 * 19. 显示（DPU 12.0）与 PKX110 面板：本驱动不访问；供 PD 的 DpuProbe、D0/D2 翻页参考 [DPU][DT][ARCH 5.3]
 *     LDT 的 qcom,sde-kms 以 0xAE00000 为基，Linux 目录以 0xAE01000 为基，二者绝对地址一致。
 * =================================================================================================================== */
#define SM8750_MDSS_PHYS                0x0AE00000ull
#define SM8750_MDSS_HW_INTR_STATUS      0x010u          /* bit0 = MDP */
#define SM8750_MDP_PHYS                 0x0AE01000ull
#define SM8750_MDP_INTR_EN              0x010u
#define SM8750_MDP_INTR_STATUS          0x014u
#define SM8750_MDP_INTR_CLEAR           0x018u
#define SM8750_MDP_INTR_INTF1_UNDERRUN  (1u << 26)
#define SM8750_MDP_INTR_INTF1_VSYNC     (1u << 27)
#define SM8750_MDP_SSPP_VIG(n)          (0x04000u + 0x2000u * (uint32_t)(n))   /* MDP 内偏移；n=0..3：0xAE05000.. */
#define SM8750_MDP_SSPP_DMA(n)          (0x24000u + 0x2000u * (uint32_t)(n))   /* n=0..5：0xAE25000.. */
#define SM8750_SSPP_SRC_SIZE            0x000u
#define SM8750_SSPP_SRC_XY              0x008u
#define SM8750_SSPP_OUT_SIZE            0x00Cu
#define SM8750_SSPP_SRC0_ADDR           0x014u
#define SM8750_SSPP_SRC1_ADDR           0x018u
#define SM8750_SSPP_SRC_YSTRIDE0        0x024u
#define SM8750_SSPP_SRC_FORMAT          0x030u
#define SM8750_SSPP_SRC_OP_MODE         0x038u
#define SM8750_SSPP_FETCH_CONFIG        0x048u
#define SM8750_SSPP_SRC_ADDR_SW_STATUS  0x070u
#define SM8750_SSPP_SRC_XY_REC1         0x168u
#define SM8750_SSPP_SRC_SIZE_REC1       0x16Cu
#define SM8750_SSPP_MULTIRECT_OPMODE    0x170u
#define SM8750_MDP_CTL(n)               (0x15000u + 0x1000u * (uint32_t)(n))   /* CTL0 = 0xAE16000 */
#define SM8750_CTL_TOP                  0x014u
#define SM8750_CTL_FLUSH                0x018u
#define SM8750_CTL_START                0x01Cu
#define SM8750_CTL_INTF_ACTIVE          0x0F4u
#define SM8750_CTL_FETCH_PIPE_ACTIVE    0x0FCu
#define SM8750_CTL_PIPE_ACTIVE          0x12Cu          /* DPU ≥ 12 */
#define SM8750_CTL_LAYER_ACTIVE         0x130u          /* DPU ≥ 12 */
#define SM8750_CTL_FLUSH_VIG0           (1u << 0)
#define SM8750_CTL_FLUSH_VIG1           (1u << 1)
#define SM8750_CTL_FLUSH_VIG2           (1u << 2)
#define SM8750_CTL_FLUSH_VIG3           (1u << 18)
#define SM8750_CTL_FLUSH_DMA0           (1u << 11)
#define SM8750_CTL_FLUSH_DMA1           (1u << 12)
#define SM8750_CTL_FLUSH_DMA2           (1u << 24)
#define SM8750_CTL_FLUSH_DMA3           (1u << 25)
#define SM8750_CTL_FLUSH_DMA4           (1u << 13)
#define SM8750_CTL_FLUSH_DMA5           (1u << 14)
#define SM8750_MDP_INTF1                0x35000u        /* = 0xAE36000，DSI0（sde-intf-off 0x36000） */
#define SM8750_INTF_TIMING_ENGINE_EN    0x000u
#define SM8750_INTF_CONFIG              0x004u
#define SM8750_INTF_FRAME_LINE_COUNT_EN 0x0A8u
#define SM8750_INTF_FRAME_COUNT         0x0ACu
#define SM8750_INTF_LINE_COUNT          0x0B0u
#define SM8750_INTF_STATUS              0x26Cu
#define SM8750_INTF_TEAR_MDP_VSYNC_SEL  0x280u
#define SM8750_INTF_TEAR_TEAR_CHECK_EN  0x284u
#define SM8750_INTF_TEAR_SYNC_CONFIG_VSYNC 0x288u       /* [18:0] vsync_count，bit19 计数使能，bit20 外部 TE */
#define SM8750_INTF_TEAR_SYNC_CONFIG_HEIGHT 0x28Cu
#define SM8750_INTF_TEAR_VSYNC_INIT_VAL 0x294u
#define SM8750_INTF_TEAR_INT_COUNT_VAL  0x298u          /* [31:16] 读指针帧计数（每个 TE +1），[15:0] 行计数 */
#define SM8750_INTF_TEAR_START_POS      0x2A0u
#define SM8750_INTF_TEAR_RD_PTR_IRQ     0x2A4u
#define SM8750_INTF_TEAR_LINE_COUNT     0x2B0u          /* 写指针行计数 */
#define SM8750_INTF_TEAR_AUTOREFRESH_CONFIG 0x2B4u      /* bit31 使能，[15:0] 每几个 TE 传一帧 */
#define SM8750_MDP_INTF1_INTR_EN        0x351C0u        /* = 0xAE361C0 */
#define SM8750_MDP_INTF1_TEAR_INTR_EN   0x35800u        /* = 0xAE36800 */
#define SM8750_MDP_INTF1_TEAR_INTR_STATUS 0x35804u
#define SM8750_MDP_INTF1_TEAR_INTR_CLEAR 0x35808u
#define SM8750_TEAR_INTR_RD_PTR         (1u << 2)
#define SM8750_DSI0_PHYS                0x0AE94000ull
#define SM8750_DSI_INT_CTRL             0x110u          /* 状态 W1C + 使能位 */
#define PKX110_PANEL_WIDTH              1216u
#define PKX110_PANEL_HEIGHT             2640u
#define PKX110_PANEL_STRIDE             (PKX110_PANEL_WIDTH * 4u)          /* 4864；以 PE_Video.v_rowBytes 为准 */
#define PKX110_PANEL_FRAME_BYTES        (PKX110_PANEL_STRIDE * PKX110_PANEL_HEIGHT)   /* 0xC3F000 */
#define PKX110_PANEL_TRANSFER_US_120    6800u           /* qcom,mdss-mdp-transfer-time-us */
#define PKX110_SPLASH_PHYS              0xFC800000ull   /* GOP 帧缓冲 */
#define PKX110_SPLASH_BYTES             0x02B00000u     /* splash_region，可放 3 帧 */
#define PKX110_SCANOUT_BUF(k)           (PKX110_SPLASH_PHYS + (uint64_t)(k) * PKX110_PANEL_FRAME_BYTES)   /* k = 0,1,2 */

/* ===================================================================================================================
 * 20. 编译期断言：设计（ARCHITECTURE/IMPLEMENTATION_PLAN）点名的取值，以及合成值与位字段的一致性
 * =================================================================================================================== */
/* 任务与设计点名的偏移 */
A830_STATIC_ASSERT(A8XX_CP_SQE_CNTL == 0x815u, "Gen8 CP_SQE_CNTL is 0x815 (ARCH 2.2, 4.9)");
A830_STATIC_ASSERT(A8XX_CP_RB_RPTR_ADDR_BV == 0x808u, "0x808 is CP_RB_RPTR_ADDR_BV on Gen8, never SQE_CNTL");
A830_STATIC_ASSERT(A8XX_CP_SCRATCH_GLOBAL(2) == 0x82Cu, "fence scratch CP_SCRATCH_GLOBAL[2] (ARCH 4.10)");
A830_STATIC_ASSERT(A8XX_RBBM_INT_0_STATUS == 0x6Au, "RBBM_INT_0_STATUS (ARCH 4.12.3)");
A830_STATIC_ASSERT(A8XX_RBBM_INT_CLEAR_CMD == 0x61u, "RBBM_INT_CLEAR_CMD (ARCH 4.12.3)");
A830_STATIC_ASSERT(A8XX_RBBM_INT_0_MASK == 0x62u, "RBBM_INT_0_MASK (ARCH 4.12.3)");
A830_STATIC_ASSERT(A8XX_GMUCX_AO_COUNTER_LO == 0x1F840u && A8XX_GMUCX_AO_COUNTER_HI == 0x1F841u, "GMU AO counter (ARCH 1.3 #5)");
A830_STATIC_ASSERT(A8XX_GMUCX_CM3_SYSRESET == 0x1F800u, "CM3_SYSRESET (ARCH 0.3)");
A830_STATIC_ASSERT(A8XX_RBBM_PERFCTR_SRAM_INIT_CMD == 0x449u && A8XX_RBBM_SLICE_PERFCTR_SRAM_INIT_CMD == 0x5E8u &&
                   A8XX_RBBM_PERFCTR_SRAM_INIT_STATUS == 0x19Fu, "SRAM init registers (ARCH 2.2, 4.10)");
A830_STATIC_ASSERT(A8XX_CP_SQE_INSTR_BASE == 0x816u && A8XX_CP_AQE_INSTR_BASE_0 == 0x818u && A8XX_CP_AQE_INSTR_BASE_1 == 0x81Au,
                   "microcode bases (ARCH 4.9)");
A830_STATIC_ASSERT(A8XX_CP_RB_BASE == 0x800u && A8XX_CP_RB_CNTL == 0x802u && A8XX_CP_RB_RPTR_ADDR == 0x804u &&
                   A8XX_CP_RB_RPTR == 0x806u && A8XX_CP_RB_WPTR == 0x807u, "ring registers (ARCH 4.9)");
A830_STATIC_ASSERT(A8XX_CP_APERTURE_CNTL_HOST == 0x81Cu && A8XX_RBBM_GBIF_HALT == 0x0Au && A8XX_GBIF_HALT == 0x3C45u,
                   "first GX writes of hw_init (ARCH 4.7.8, 4.8 step 1)");
A830_STATIC_ASSERT(A8XX_CP_SMMU_STREAM_ID_LPAC == 0x814u && A830_CP_SMMU_STREAM_ID_LPAC_VALUE == 0x101u, "LPAC stream (ARCH 4.5.5)");
A830_STATIC_ASSERT(A8XX_RB_GC_GMEM_PROTECT == 0x8E09u && A8XX_RBBM_SECVID_TRUST_CNTL == 0xF400u, "ARCH 4.11");
A830_STATIC_ASSERT(A8XX_CP_PROTECT_GLOBAL(63) == 0x88Fu && A8XX_CP_PROTECT_PIPE(15) == 0x8AFu, "protect slots (ARCH 4.8 step 13)");
A830_STATIC_ASSERT(A8XX_TPL1_BICUBIC_WEIGHTS_TABLE(0) == 0xB606u && A8XX_CP_PERFCTR_CP_SEL(0) == 0x8D0u, "ARCH 4.8 steps 6, 8");
A830_STATIC_ASSERT(A8XX_GMUAO_AHB_FENCE_STATUS == 0x23B13u && A8XX_GMUAO_RBBM_INT_UNMASKED_STATUS_SHADOW == 0x23B15u,
                   "ARCH 4.9, 4.13.2");
A830_STATIC_ASSERT(A8XX_GMUCX_POWER_COUNTER_ENABLE == 0x1FC10u && A8XX_GMUCX_POWER_COUNTER_XOCLK_L_0 == 0x1FCA0u, "ARCH 4.16");
A830_STATIC_ASSERT(A8XX_GMU_DTCM_CORE_FW_VERSION == 0x1D3F8u, "Mesa GMU_CORE_FW_VERSION");

/* 窗口与换算（ARCH 0.3） */
A830_STATIC_ASSERT(A830_GMU_PHYS - A830_GPU_PHYS == A830_GMU_WIN_DELTA, "GMU window delta");
A830_STATIC_ASSERT(A830_CX_MISC_PHYS - A830_GPU_PHYS == A830_CX_MISC_WIN_DELTA, "CX_MISC window delta");
A830_STATIC_ASSERT(A830_GMU_REG_BYTE(A8XX_GMUCX_CM3_SYSRESET) == 0x47000u, "CM3_SYSRESET at gmu window +0x47000");
A830_STATIC_ASSERT(A830_GMU_REG_BYTE(A8XX_GMUCX_AO_COUNTER_LO) == 0x47100u &&
                   A830_GMU_REG_BYTE(A8XX_GMUCX_AO_COUNTER_HI) == 0x47104u, "AO counter at gmu window +0x47100/+0x47104");
A830_STATIC_ASSERT(A830_GMU_REG_BYTE(A8XX_GMUAO_AHB_FENCE_STATUS) == 0x57C4Cu, "AHB fence status at gmu window +0x57C4C");
A830_STATIC_ASSERT(A830_GMU_REG_IN_WINDOW(A8XX_GMU_CM3_ITCM_START) && A830_GMU_REG_IN_WINDOW(A8XX_GMUAO_RBBM_INT_UNMASKED_STATUS_SHADOW),
                   "GMU registers inside the gmu window");
A830_STATIC_ASSERT(A830_CX_MISC_REG_IN_WINDOW(A8XX_CX_MISC_CX_AHB_AON_CNTL) && A830_CX_MISC_REG_IN_WINDOW(A8XX_CX_MISC_SW_FUSE_FREQ_LIMIT_STATUS),
                   "CX_MISC registers inside the cx_misc window");
A830_STATIC_ASSERT(A830_GPU_REG_IN_WINDOW(A8XX_RBBM_SECVID_TSB_CNTL) && A830_GPU_REG_IN_WINDOW(A8XX_GBIF_HALT_ACK), "GPU window");
A830_STATIC_ASSERT(A830_GPU_REG_PHYS(A8XX_RBBM_INT_0_STATUS) == 0x03D001A8ull && A830_GPU_REG_PHYS(A8XX_RBBM_INT_CLEAR_CMD) == 0x03D00184ull &&
                   A830_GPU_REG_PHYS(A8XX_RBBM_INT_0_MASK) == 0x03D00188ull, "RBBM interrupt registers (perf input)");
/* B1–B8、OOB、强制关断里出现的物理地址（ARCH 4.7.3、4.7.8、4.7.9） */
A830_STATIC_ASSERT(A830_GPU_REG_PHYS(A8XX_GMUCX_CM3_SYSRESET) == 0x03D7E000ull, "B1");
A830_STATIC_ASSERT(A830_GPU_REG_PHYS(A8XX_GMUCX_GMU2HOST_INTR_CLR) == 0x03D7E644ull &&
                   A830_GPU_REG_PHYS(A8XX_GMUAO_AO_HOST_INTERRUPT_CLR) == 0x03D8EC10ull &&
                   A830_GPU_REG_PHYS(A8XX_GMUCX_GMU2HOST_INTR_MASK) == 0x03D7E64Cull &&
                   A830_GPU_REG_PHYS(A8XX_GMUAO_AO_HOST_INTERRUPT_MASK) == 0x03D8EC18ull, "B2");
A830_STATIC_ASSERT(A830_GPU_REG_PHYS(A8XX_GMUAO_RSCC_CONTROL_REQ) == 0x03D8EC1Cull &&
                   A830_GPU_REG_PHYS(A8XX_GMUAO_RSCC_CONTROL_ACK) == 0x03D8EC20ull &&
                   A830_RSCC_REG_PHYS(A8XX_RSCC_SEQ_BUSY_DRV0) == 0x03D50404ull, "B3");
A830_STATIC_ASSERT(A830_GPU_REG_PHYS(A8XX_GMU_CM3_ITCM_START) == 0x03D6D000ull &&
                   A830_GPU_REG_PHYS(A8XX_GMU_CM3_DTCM_START) == 0x03D71000ull, "B4");
A830_STATIC_ASSERT(A830_GPU_REG_PHYS(A8XX_GMU_DTCM_CORE_FW_VERSION) == 0x03D74FE0ull &&
                   A830_GPU_REG_PHYS(A8XX_GMU_DTCM_HFI_VERSION) == 0x03D74FF0ull, "B5");
A830_STATIC_ASSERT(A830_GPU_REG_PHYS(A8XX_GMUCX_RPMH_POWER_STATE) == 0x03D7DFA4ull &&
                   A830_GPU_REG_PHYS(A8XX_GMUCX_CX_FALNEXT_INTF) == 0x03D7DFB4ull &&
                   A830_GPU_REG_PHYS(A8XX_GMUCX_CX_FAL_INTF) == 0x03D7DFB0ull &&
                   A830_GPU_REG_PHYS(A8XX_GMUCX_CM3_FW_INIT_RESULT) == 0x03D7E070ull &&
                   A830_GPU_REG_PHYS(A8XX_GMUCX_CM3_BOOT_CONFIG) == 0x03D7E004ull &&
                   A830_GPU_REG_PHYS(A8XX_GMUCX_HFI_QTBL_ADDR) == 0x03D7E614ull &&
                   A830_GPU_REG_PHYS(A8XX_GMUCX_HFI_QTBL_INFO) == 0x03D7E610ull &&
                   A830_GPU_REG_PHYS(A8XX_GMUAO_AHB_FENCE_RANGE_0) == 0x03D8EC44ull &&
                   A830_GPU_REG_PHYS(A8XX_GMUCX_CM3_CFG) == 0x03D7E0B4ull, "B6 (1)");
A830_STATIC_ASSERT(A830_GPU_REG_PHYS(A8XX_GBIF_QSB_SIDE0) == 0x03D0F00Cull && A830_GPU_REG_PHYS(A8XX_GBIF_QSB_SIDE3) == 0x03D0F018ull &&
                   A830_GPU_REG_PHYS(A8XX_GBIF_CX_CONFIG) == 0x03D0F000ull &&
                   A830_GPU_REG_PHYS(A8XX_GMUCX_MRC_GBIF_QOS_CTRL) == 0x03D7D42Cull &&
                   A830_GPU_REG_PHYS(A8XX_GBIF_HALT) == 0x03D0F114ull &&
                   A830_GPU_REG_PHYS(A8XX_GMUCX_GENERAL_9) == 0x03D7E724ull &&
                   A830_GPU_REG_PHYS(A8XX_GMUCX_GENERAL_10) == 0x03D7E728ull &&
                   A830_GPU_REG_PHYS(A8XX_GMUCX_GENERAL_8) == 0x03D7E720ull &&
                   A830_GPU_REG_PHYS(A8XX_GMUCX_SYS_BUS_CONFIG) == 0x03D7D03Cull &&
                   A830_GPU_REG_PHYS(A8XX_GMUCX_ICACHE_CONFIG) == 0x03D7D000ull &&
                   A830_GPU_REG_PHYS(A8XX_GMUCX_DCACHE_CONFIG) == 0x03D7D004ull, "B6 (2)");
A830_STATIC_ASSERT(A830_GPU_REG_PHYS(A8XX_GMUCX_GMU2HOST_INTR_INFO) == 0x03D7E648ull &&
                   A830_GPU_REG_PHYS(A8XX_GMUAO_AO_HOST_INTERRUPT_STATUS) == 0x03D8EC14ull, "B7");
A830_STATIC_ASSERT(A830_GPU_REG_PHYS(A8XX_GMUCX_HFI_CTRL_INIT) == 0x03D7E618ull &&
                   A830_GPU_REG_PHYS(A8XX_GMUCX_HFI_CTRL_STATUS) == 0x03D7E600ull, "B8");
A830_STATIC_ASSERT(A830_RSCC_REG_PHYS(A8XX_RSCC_RSC_STATUS0_DRV0) == 0x03D50010ull, "RSCC sleep (ARCH 4.7.4)");
A830_STATIC_ASSERT(A830_GPU_REG_PHYS(A8XX_GMUCX_HOST2GMU_INTR_SET) == 0x03D7E650ull &&
                   A830_GPU_REG_PHYS(A8XX_GMUCX_GFX_PWR_CLK_STATUS) == 0x03D7DFA0ull &&
                   A830_GPU_REG_PHYS(A8XX_GMUAO_GPU_CX_BUSY_MASK) == 0x03D8EC38ull &&
                   A830_GPU_REG_PHYS(A8XX_GMUCX_POWER_COUNTER_SELECT_XOCLK_0) == 0x03D7F100ull &&
                   A830_GPU_REG_PHYS(A8XX_GMUCX_POWER_COUNTER_SELECT_XOCLK_1) == 0x03D7F104ull &&
                   A830_GPU_REG_PHYS(A8XX_GMUCX_POWER_COUNTER_ENABLE) == 0x03D7F040ull &&
                   A830_GPU_REG_PHYS(A8XX_GMUAO_CGC_MODE_CNTL) == 0x03D8EC24ull &&
                   A830_GPU_REG_PHYS(A8XX_GMUAO_CGC_DELAY_CNTL) == 0x03D8EC28ull &&
                   A830_GPU_REG_PHYS(A8XX_GMUAO_CGC_HYST_CNTL) == 0x03D8EC2Cull, "OOB and GX confirmation (ARCH 4.7.8)");
A830_STATIC_ASSERT(A830_GPU_REG_PHYS(A8XX_GMUCX_PWR_COL_KEEPALIVE) == 0x03D7DF90ull &&
                   A830_RSCC_REG_PHYS(A8XX_RSCC_TCS_DRV0_STATUS(0)) == 0x03D50D28ull &&
                   A830_GPU_REG_PHYS(A8XX_GMUAO_AHB_FENCE_STATUS_CLR) == 0x03D8EC50ull &&
                   A830_GPU_REG_PHYS(A8XX_GMUAO_AHB_FENCE_CTRL) == 0x03D8EC40ull &&
                   A830_GPU_REG_PHYS(A8XX_GBIF_HALT_ACK) == 0x03D0F118ull, "force off (ARCH 4.7.9)");
A830_STATIC_ASSERT(A8XX_RSCC_TCS_DRV0_STATUS(9) == 0x932u, "KGSL GEN8_RSCC_TCS9_DRV0_STATUS");
A830_STATIC_ASSERT(A830_GPU_REG_PHYS(A8XX_GMUAO_GPU_CX_BUSY_STATUS) == 0x03D8EC30ull &&
                   A830_GPU_REG_PHYS(A8XX_GMUCX_POWER_COUNTER_XOCLK_L_0) == 0x03D7F280ull &&
                   A830_GPU_REG_PHYS(A8XX_GMUCX_POWER_COUNTER_XOCLK_H_0) == 0x03D7F284ull &&
                   A830_GPU_REG_PHYS(A8XX_GMUAO_AHB_FENCE_STATUS) == 0x03D8EC4Cull, "ARCH 4.9, 4.16");
/* CX_MISC（ARCH 4.4.2、4.7.8） */
A830_STATIC_ASSERT(A830_GPU_REG_PHYS(A8XX_CX_MISC_SW_FUSE_VALUE) == 0x03D9F000ull &&
                   A830_GPU_REG_PHYS(A8XX_CX_MISC_SW_FUSE_FREQ_LIMIT_STATUS) == 0x03D9F014ull &&
                   A830_GPU_REG_PHYS(A8XX_CX_MISC_TCM_RET_CNTL) == 0x03D9E0E4ull &&
                   A830_GPU_REG_PHYS(A8XX_CX_MISC_AO_COUNTER_LO) == 0x03D9E200ull &&
                   A830_GPU_REG_PHYS(A8XX_CX_MISC_AO_COUNTER_HI) == 0x03D9E204ull &&
                   A830_GPU_REG_PHYS(A8XX_CX_MISC_CX_AHB_AON_CNTL) == 0x03D9E040ull &&
                   A830_GPU_REG_PHYS(A8XX_CX_MISC_CX_AHB_HOST_CNTL) == 0x03D9E050ull &&
                   A830_GPU_REG_PHYS(A8XX_CX_MISC_INT_CLEAR_CMD) == 0x03D9E0C4ull &&
                   A830_GPU_REG_PHYS(A8XX_CX_MISC_INT_0_MASK) == 0x03D9E0CCull &&
                   A830_GPU_REG_PHYS(A8XX_CX_MISC_GFX_PWR_CLK_STATUS) == 0x03D9E20Cull, "CX_MISC physical addresses");
A830_STATIC_ASSERT(A830_CX_MISC_REG_BYTE(A8XX_CX_MISC_SW_FUSE_VALUE) == 0x1000u &&
                   A830_CX_MISC_REG_BYTE(A8XX_CX_MISC_TCM_RET_CNTL) == 0x0E4u, "Linux cx_mem offsets 0x400 and 0x39");
A830_STATIC_ASSERT(A830_GX_CFG_GDSCR_PHYS == A830_GX_GDSCR_PHYS + 4u, "GX CFG_GDSCR = GDSCR + 4 (ARCH 4.7.9 step 6)");
/* SMMU 地址（ARCH 4.5.2） */
A830_STATIC_ASSERT(A830_SMMU_PHYS + A830_SMMU_GR1 == 0x03DA1000ull && A830_SMMU_PHYS + A830_SMMU_GR0_SMR(0) == 0x03DA0800ull &&
                   A830_SMMU_PHYS + A830_SMMU_GR0_S2CR(0) == 0x03DA0C00ull, "SMMU global addresses");
A830_STATIC_ASSERT(A830_SMMU_GLOBAL_PAGES(A830_SMMU_IDR1_SM8750) == 32u &&
                   A830_SMMU_PHYS + A830_SMMU_CB_OFF(A830_SMMU_GLOBAL_PAGES(A830_SMMU_IDR1_SM8750), 0) == 0x03DC0000ull &&
                   A830_SMMU_PHYS + A830_SMMU_CB_OFF(A830_SMMU_GLOBAL_PAGES(A830_SMMU_IDR1_SM8750), 2) == 0x03DC2000ull,
                   "CB0 0x3DC0000, CB2 0x3DC2000");
A830_STATIC_ASSERT(A830_SMMU_IDR1_NUMCB(A830_SMMU_IDR1_SM8750) == 21u && A830_SMMU_IDR0_NUMSMRG(A830_SMMU_IDR0_SM8750) == 24u &&
                   (A830_SMMU_IDR0_SM8750 & (A830_SMMU_IDR0_S1TS | A830_SMMU_IDR0_SMS | A830_SMMU_IDR0_CTTW)) ==
                   (A830_SMMU_IDR0_S1TS | A830_SMMU_IDR0_SMS | A830_SMMU_IDR0_CTTW) &&
                   A830_SMMU_OAS_BITS(A830_SMMU_IDR2_OAS(A830_SMMU_IDR2_SM8750)) == 36u &&
                   A830_SMMU_OAS_BITS(A830_SMMU_IDR2_UBS(A830_SMMU_IDR2_SM8750)) == 48u &&
                   (A830_SMMU_IDR2_SM8750 & A830_SMMU_IDR2_PTFS_4K) != 0u &&
                   (A830_SMMU_IDR1_SM8750 & A830_SMMU_IDR1_PAGESIZE_64K) == 0u, "SM8750 SMMU ID registers (ARCH 4.5.1)");
/* SMMU 取值（ARCH 4.5.4） */
A830_STATIC_ASSERT(A830_SMMU_S2CR_FAULT_NSH == ((A830_SMMU_S2CR_TYPE_FAULT << 16) | A830_SMMU_S2CR_SHCFG_NSH), "S2CR fault");
A830_STATIC_ASSERT(A830_SMMU_S2CR_TRANS_NSH(2) == 0x302u && A830_SMMU_S2CR_TRANS_NSH(0) == 0x300u, "S2CR routes read back 0x302/0x300");
A830_STATIC_ASSERT(A830_SMMU_S2CR_TYPE(A830_SMMU_S2CR_BOOT_SM8750) == A830_SMMU_S2CR_TYPE_TRANS &&
                   A830_SMMU_S2CR_CBNDX(A830_SMMU_S2CR_BOOT_SM8750) == 24u &&
                   A830_SMMU_S2CR_PRIVCFG(A830_SMMU_S2CR_BOOT_SM8750) == 3u, "boot S2CR translate->CB24, PRIVCFG 3");
A830_STATIC_ASSERT(A830_SMMU_SMR(A830_SID_GMU, 0) == 0x80000005u && A830_SMMU_SMR(A830_SID_LPAC, 0) == 0x80000001u &&
                   A830_SMMU_SMR_COVERS(0x80000005u, 5u) && !A830_SMMU_SMR_COVERS(0x80000005u, 1u) &&
                   A830_SMMU_SMR_COVERS(A830_SMMU_SMR(0, 7), 5u) && !A830_SMMU_SMR_COVERS(0x00000005u, 5u), "SMR encoding and match");
A830_STATIC_ASSERT(A830_SMMU_CBAR_VALUE == (A830_SMMU_CBAR_TYPE_S1_TRANS_S2_BYPASS | A830_SMMU_CBAR_S1_MEMATTR_WB |
                                            A830_SMMU_CBAR_S1_BPSHCFG_NSH), "CBAR 0x1F300");
A830_STATIC_ASSERT(A830_SMMU_SCTLR_ENABLE == (A830_SMMU_SCTLR_M | A830_SMMU_SCTLR_TRE | A830_SMMU_SCTLR_AFE | A830_SMMU_SCTLR_CFRE |
                                              A830_SMMU_SCTLR_CFIE | A830_SMMU_SCTLR_HUPCF | A830_SMMU_SCTLR_ASIDPNE) &&
                   A830_SMMU_SCTLR_READBACK == (A830_SMMU_SCTLR_ENABLE & ~A830_SMMU_SCTLR_ASIDPNE), "SCTLR 0x1167, reads 0x167");
A830_STATIC_ASSERT(A830_SMMU_TCR2_VALUE == (A830_SMMU_TCR2_SEP_UPSTREAM | A830_SMMU_TCR2_AS | A830_SMMU_TCR2_PASIZE_36) &&
                   (A830_SMMU_TCR2_VALUE | A830_SMMU_TCR2_TBI0 | A830_SMMU_TCR2_TBI1) == 0x00038071u, "TCR2 0x38011, reads 0x38071");
A830_STATIC_ASSERT(A830_SMMU_TCR_CB0_VALUE == (A830_SMMU_TCR_T0SZ(32) | A830_SMMU_TCR_IRGN0(A830_SMMU_RGN_WBWA) |
                   A830_SMMU_TCR_ORGN0(A830_SMMU_RGN_WBWA) | A830_SMMU_TCR_SH0(A830_SMMU_SH_IS) | A830_SMMU_TCR_TG0_4K |
                   A830_SMMU_TCR_T1SZ(32) | A830_SMMU_TCR_EPD1 | A830_SMMU_TCR_IRGN1(A830_SMMU_RGN_WBWA) |
                   A830_SMMU_TCR_ORGN1(A830_SMMU_RGN_WBWA) | A830_SMMU_TCR_SH1(A830_SMMU_SH_IS) | A830_SMMU_TCR_TG1_4K),
                   "CB0 TCR 0xB5A03520");
A830_STATIC_ASSERT(A830_SMMU_TCR_CB2_VALUE == (A830_SMMU_TCR_T0SZ(32) | A830_SMMU_TCR_IRGN0(A830_SMMU_RGN_WBWA) |
                   A830_SMMU_TCR_ORGN0(A830_SMMU_RGN_WBWA) | A830_SMMU_TCR_SH0(A830_SMMU_SH_IS) | A830_SMMU_TCR_EPD1 |
                   A830_SMMU_TCR_TG1_4K), "CB2 TCR 0x80803520");
A830_STATIC_ASSERT(A830_SMMU_FSR_FAULT_MASK == (A830_SMMU_FSR_MULTI | A830_SMMU_FSR_SS | A830_SMMU_FSR_UUT | A830_SMMU_FSR_ASF |
                   A830_SMMU_FSR_TLBLKF | A830_SMMU_FSR_TLBMCF | A830_SMMU_FSR_EF | A830_SMMU_FSR_PF | A830_SMMU_FSR_AFF |
                   A830_SMMU_FSR_TF) && (0x400u & A830_SMMU_FSR_FAULT_MASK) == 0u, "FSR fault mask 0xC00001FE; 0x400 is format");
A830_STATIC_ASSERT(A830_SMMU_TTBR(A830_SMMU_ASID(A830_SMMU_CB_GPU), 0x987654000ull) == 0x0001000987654000ull &&
                   A830_SMMU_ASID(A830_SMMU_CB_GMU) == 3u, "TTBR = ASID<<48 | root; GPU ASID 1, GMU ASID 3");
A830_STATIC_ASSERT(A830_SMMU_TLBIVA_VALUE(0x20001000ull, 1u) == 0x0001000000020001ull, "TLBIVAL value = iova>>12 | ASID<<48");
A830_STATIC_ASSERT(A830_SMMU_APERTURE_ARG(0) == 0xFFFF0000u, "CP_SMMU_APERTURE x2 for CB0 (ARCH 4.4.1)");
/* PTE（ARCH 4.6.3） */
A830_STATIC_ASSERT(A830_PTE_USER_RW_NC == (A830_PTE_PAGE | A830_PTE_ATTRINDX(A830_SMMU_MAIR_IDX_NC) | A830_PTE_AP_EL0 | A830_PTE_SH_OUTER |
                   A830_PTE_AF | A830_PTE_NG | A830_PTE_PXN | A830_PTE_XN), "user RW NC");
A830_STATIC_ASSERT(A830_PTE_USER_RO_NC == (A830_PTE_USER_RW_NC | A830_PTE_AP_RDONLY), "user RO NC");
A830_STATIC_ASSERT(A830_PTE_KERNEL_RW_NC == (A830_PTE_USER_RW_NC & ~A830_PTE_AP_EL0) &&
                   A830_PTE_KERNEL_RO_NC == (A830_PTE_KERNEL_RW_NC | A830_PTE_AP_RDONLY), "kernel privileged NC");
A830_STATIC_ASSERT(A830_PTE_UCODE == (A830_PTE_USER_RO_NC & ~(A830_PTE_PXN | A830_PTE_XN)), "microcode: executable");
A830_STATIC_ASSERT(A830_PTE_COHERENT_RW == (A830_PTE_PAGE | A830_PTE_ATTRINDX(A830_SMMU_MAIR_IDX_WB) | A830_PTE_AP_EL0 |
                   A830_PTE_SH_INNER | A830_PTE_AF | A830_PTE_NG | A830_PTE_PXN | A830_PTE_XN) &&
                   A830_PTE_COHERENT_RO == (A830_PTE_COHERENT_RW | A830_PTE_AP_RDONLY), "coherent WB+IS");
/* 中断与 CP 取值（ARCH 4.8、4.12） */
A830_STATIC_ASSERT(A8XX_INT_MASK == (A8XX_RBBM_INT_0_CP_AHB_ERROR | A8XX_RBBM_INT_0_ATB_ASYNCFIFO_OVERFLOW | A8XX_RBBM_INT_0_GPC_ERROR |
                   A8XX_RBBM_INT_0_CP_SW | A8XX_RBBM_INT_0_CP_HW_ERROR | A8XX_RBBM_INT_0_PM4CPINTERRUPT |
                   A8XX_RBBM_INT_0_CP_RB_DONE_TS | A8XX_RBBM_INT_0_CP_CACHE_FLUSH_TS | A8XX_RBBM_INT_0_ATB_BUS_OVERFLOW |
                   A8XX_RBBM_INT_0_HANG_DETECT | A8XX_RBBM_INT_0_UCHE_OOB_ACCESS | A8XX_RBBM_INT_0_UCHE_TRAP |
                   A8XX_RBBM_INT_0_TSB_WRITE_ERROR | A8XX_RBBM_INT_0_SW_FUSE_VIOLATION), "A8XX_INT_MASK 0x33D283C2");
A830_STATIC_ASSERT(A8XX_INT_ERRORS == (A8XX_INT_MASK & ~(A8XX_RBBM_INT_0_PM4CPINTERRUPT | A8XX_RBBM_INT_0_CP_RB_DONE_TS |
                   A8XX_RBBM_INT_0_CP_CACHE_FLUSH_TS)), "error bits = mask minus normal events and the fence");
A830_STATIC_ASSERT(A830_INT_MASK_FENCE_ONLY == 0x00100000u && (A8XX_INT_FATAL & A8XX_INT_ERRORS) == A8XX_INT_FATAL, "ARCH 4.12.3");
A830_STATIC_ASSERT(A8XX_CP_GLOBAL_INT_HWFAULT(A8XX_PIPE_BR) == (1u << 0) && A8XX_CP_GLOBAL_INT_HWFAULT(A8XX_PIPE_DDE_BV) == (1u << 6) &&
                   A8XX_CP_GLOBAL_INT_SWFAULT(A8XX_PIPE_BR) == (1u << 16) && A8XX_CP_GLOBAL_INT_SWFAULT(A8XX_PIPE_DDE_BV) == (1u << 22) &&
                   A830_CP_INTERRUPT_STATUS_MASK_GLOBAL_VALUE == (0x7Fu | (0x7Fu << 16)), "HW and SW fault bits of BR..DDE_BV");
A830_STATIC_ASSERT(A830_CP_INTERRUPT_STATUS_MASK_PIPE_VALUE == (0x7Fu | (0x3Fu << 8) | (1u << 19) | (0x3Fu << 21)),
                   "pipe interrupt mask 0x07E83F7F");
A830_STATIC_ASSERT(A830_CP_HW_FAULT_STATUS_MASK_PIPE_VALUE == (0x7Fu | (7u << 8)), "pipe HW fault mask 0x77F");
A830_STATIC_ASSERT(A830_CP_APRIV_VALUE == (A8XX_CP_APRIV_ICACHE | A8XX_CP_APRIV_RBFETCH | A8XX_CP_APRIV_RBPRIVLEVEL |
                   A8XX_CP_APRIV_RBRPWB) && A830_CP_APRIV_BR_VALUE == (A830_CP_APRIV_VALUE | A8XX_CP_APRIV_CDREAD |
                   A8XX_CP_APRIV_CDWRITE), "APRIV BR 0x6F, others 0x0F (ARCH 4.6.2, 4.8 step 15)");
A830_STATIC_ASSERT(A830_CP_PROTECT_CNTL_VALUE == (A8XX_CP_PROTECT_CNTL_ACCESS_PROT_EN | A8XX_CP_PROTECT_CNTL_ACCESS_FAULT_ON_VIOL_EN |
                   A8XX_CP_PROTECT_CNTL_LAST_SPAN_INF_RANGE | A8XX_CP_PROTECT_CNTL_HALT_SQE_RANGE), "CP_PROTECT_CNTL 0xFFFF000B");
A830_STATIC_ASSERT(A8XX_CP_PROTECT_REG(0x27C02u, 0u, 1) == A830_CP_PROTECT_LAST_VALUE, "last protect entry 0x80027C02");
A830_STATIC_ASSERT(A8XX_CP_APERTURE_CNTL_HOST_PIPE(A8XX_PIPE_BR) == 0x1000u && A8XX_CP_APERTURE_CNTL_HOST_PIPE(A8XX_PIPE_DDE_BV) == 0x7000u,
                   "aperture = pipe << 12 (ARCH 4.8)");
A830_STATIC_ASSERT(A830_CP_RB_CNTL_VALUE == (A8XX_CP_RB_CNTL_BUFSZ(12) | A8XX_CP_RB_CNTL_BLKSZ(2)) &&
                   (1u << 12) * 8u == A830_RING_BYTES && (1u << 2) * 8u == A830_RING_BLOCK_BYTES, "CP_RB_CNTL 0x20C for 32 KiB");
A830_STATIC_ASSERT(A830_CP_ME_INIT_FEATURES == (A830_CP_ME_INIT_MULTI_CONTEXT | A830_CP_ME_INIT_ERROR_DETECTION |
                   A830_CP_ME_INIT_DEFAULT_RESET | A830_CP_ME_INIT_NO_PERFCTR_SAVE | A830_CP_ME_INIT_PWRUP_LIST) &&
                   A830_CP_THREAD_CONTROL_INIT == 0x08000000u && A830_CP_INIT_RING_DWORDS == 2u + 1u + A830_CP_ME_INIT_DWORDS,
                   "CP_ME_INIT payload (ARCH 4.9)");
A830_STATIC_ASSERT(A830_FENCE_EVENT == (A8XX_EVENT_CACHE_CLEAN | A8XX_CP_EVENT_WRITE7_WRITE_ENABLED) &&
                   A830_FENCE_EVENT_IRQ == (A830_FENCE_EVENT | A8XX_CP_EVENT_WRITE7_IRQ), "fence events (ARCH 4.10)");
A830_STATIC_ASSERT(A830_RBBM_INTERFACE_HANG_INT_CNTL_VALUE == (A8XX_RBBM_INTERFACE_HANG_INT_CNTL_EN | A830_HANG_DETECT_CYCLES) &&
                   A830_RBBM_SLICE_INTERFACE_HANG_INT_CNTL_VALUE == A8XX_RBBM_INTERFACE_HANG_INT_CNTL_EN, "hang detect (ARCH 4.8 step 11)");
/* UCHE/UBWC/GBIF（ARCH 4.8、4.14、4.15） */
A830_STATIC_ASSERT(A830_GMEM_BASE == 0x4000000ull && A830_GMEM_BASE % 0x4000000ull == 0u &&
                   A830_GMEM_BASE + A830_GMEM_BYTES <= A830_GPU_VA_CLIENT_BASE, "GMEM window 0x4000000, 12 MiB");
A830_STATIC_ASSERT(A830_UCHE_CLIENT_PF_VALUE == ((1u << 7) | 1u), "UCHE_CLIENT_PF 0x81");
A830_STATIC_ASSERT(A830_UBWC_HBB == 3u && A830_GRAS_NC_MODE_CNTL_VALUE == (A830_UBWC_HBB << 5) &&
                   A830_RB_CCU_NC_MODE_CNTL_VALUE == ((A830_UBWC_HBB_HI << 3) | (A830_UBWC_HBB_LO << 1)) &&
                   A830_RB_CMP_NC_MODE_CNTL_VALUE == ((A830_UBWC_MODE_TAG << 15) | (A830_UBWC_RGB565_PREDICATOR << 2) |
                                                      (A830_UBWC_AMSBC << 1)) &&
                   A830_SP_NC_MODE_CNTL_VALUE == ((A830_UBWC_HBB_HI << 10) | (A830_UBWC_UAVFLAGPRD_INV << 4) | (A830_UBWC_HBB_LO << 1)) &&
                   A830_TPL1_NC_MODE_CNTL_VALUE == ((A830_UBWC_HBB_HI << 4) | (A830_UBWC_HBB_LO << 1)), "UBWC NC_MODE values (ARCH 4.8 step 7)");
A830_STATIC_ASSERT(A8XX_GBIF_SCACHE_CNTL1_SCID(SM8750_LLCC_SCID_GPU) == A830_GBIF_SCACHE_CNTL1_SCID9 &&
                   A8XX_GBIF_SCACHE_CNTL0_SCID(SM8750_LLCC_SCID_GPU) == A830_GBIF_SCACHE_CNTL0_SCID9, "GBIF_SCACHE for SCID 9 (ARCH 4.8 step 9a)");
A830_STATIC_ASSERT(SM8750_LLCC_ATTR3_MAX_CAP(SM8750_LLCC_GPU_MAX_CAP_KIB, 4u) == SM8750_LLCC_ATTR3_GPU_VALUE &&
                   SM8750_LLCC_ATTR3_MAX_CAP(SM8750_LLCC_GPUHTW_MAX_CAP_KIB, 4u) == SM8750_LLCC_ATTR3_GPUHTW_VALUE &&
                   SM8750_LLCC_ATTR2_GPU_VALUE == (SM8750_LLCC_ATTR2_FIXED_SIZE | SM8750_LLCC_ATTR2_PRIORITY(1)) &&
                   SM8750_LLCC_BCAST_OR_PHYS + SM8750_LLCC_V6_ATTR0(SM8750_LLCC_SCID_GPU) == 0x26841240ull,
                   "LLCC slice attributes (ARCH 4.15)");
/* GMU 取值（ARCH 4.7） */
A830_STATIC_ASSERT(A830_GMU2HOST_IRQ_CLEAR == (A830_GMU2HOST_SIDEMSGQ | A830_GMU2HOST_DBGQ | A830_GMU2HOST_CM3_FAULT |
                   A830_GMU2HOST_CM3_FAULT_LINUX) && A830_GMU2HOST_MASK_RUN == (uint32_t)~A830_GMU2HOST_IRQ_CLEAR,
                   "GMU2HOST mask 0xFF7F7FF9 opens exactly the bits 336 clears (ARCH 4.7.3 B2, 4.12.3)");
A830_STATIC_ASSERT((A830_GMU2HOST_IRQ_CLEAR & A830_GMU2HOST_OOB_MASK) == A830_GMU2HOST_CM3_FAULT_LINUX &&
                   (A830_GMU2HOST_IRQ_CLEAR & A830_OOB_GPU_ACK) == 0u, "336 never clears the OOB ack bit31");
A830_STATIC_ASSERT(A830_GMU_AO_MASK_RUN == (uint32_t)~(A830_GMU_AO_WDOG_BITE | A830_GMU_AO_FENCE_ERR | A830_GMU_AO_HOST_AHB_BUS_ERR),
                   "AO mask 0xFFFFFFD6");
A830_STATIC_ASSERT(A830_OOB_SET(A830_OOB_REQ_GPU) == A830_OOB_GPU_SET && A830_OOB_ACK(A830_OOB_REQ_GPU) == A830_OOB_GPU_ACK &&
                   A830_OOB_CLEAR(A830_OOB_REQ_GPU) == A830_OOB_GPU_CLEAR && A830_OOB_SET(A830_OOB_REQ_PERFCNTR) == A830_OOB_PERFCNTR_SET &&
                   A830_OOB_ACK(A830_OOB_REQ_PERFCNTR) == A830_OOB_PERFCNTR_ACK &&
                   A830_OOB_CLEAR(A830_OOB_REQ_PERFCNTR) == A830_OOB_PERFCNTR_CLEAR, "OOB bits (ARCH 4.7.8)");
A830_STATIC_ASSERT(A830_AHB_FENCE_RANGE(0x8A0u) == A830_AHB_FENCE_RANGE_KGSL && A830_AHB_FENCE_RANGE(0x8C0u) == A830_AHB_FENCE_RANGE_LINUX,
                   "AHB fence range (ARCH 1.3 #12)");
A830_STATIC_ASSERT(A830_GMU_LOG_CFG(A830_GMU_VA_LOG, A830_GMU_LOG_BYTES) == 0x60004003u, "GENERAL_8 = 0x60004003 (ARCH 4.7.2)");
A830_STATIC_ASSERT(A8XX_GMUCX_GFX_PWR_CLK_STATUS_GX_OFF == (A8XX_GMUCX_GFX_PWR_CLK_STATUS_GX_GDSC_OFF |
                   A8XX_GMUCX_GFX_PWR_CLK_STATUS_GX_CLK_OFF), "GX on = [1:0] == 0");
A830_STATIC_ASSERT((A830_GMU_FW_INIT_READY & ~A830_GMU_FW_INIT_MASK) == 0u && (A830_GMU_FW_INIT_NMI_MASK & A830_GMU_FW_INIT_MASK) == 0u &&
                   (A830_GMU_CM3_CFG_VALUE & A830_GMU_CM3_CFG_NMI) == 0u, "init result fields; CM3_CFG NMI bit clear");
A830_STATIC_ASSERT(A830_GMU_VER_MAJOR(A830_GMU_HFI_VERSION) == 2u && A830_GMU_VER_MINOR(A830_GMU_HFI_VERSION) == 7u &&
                   A830_GMU_VER_STEP(A830_GMU_HFI_VERSION) == 1u && A830_GMU_VER_MINOR(A830_GMU_CORE_FW_VERSION) == 1u &&
                   A830_GMU_VER_STEP(A830_GMU_CORE_FW_VERSION) == 25u && A830_GMU_CORE_FW_VERSION > A830_GMU_CORE_NEW_INIT_AFTER &&
                   A830_GMU_HFI_VERSION >= A830_GMU_HFI_TABLE_FORMAT_MIN &&
                   A830_GMU_VER_MINOR(A830_GMU_HFI_VERSION) > A830_GMU_HFI_V5_MINOR_ABOVE, "firmware versions (ARCH 4.7.3 B5)");
A830_STATIC_ASSERT(A830_GMU_VA_ITCM + A830_GMU_ITCM_BYTES == 0x4000u && A830_GMU_VA_DTCM + A830_GMU_DTCM_BYTES == 0x10008000u &&
                   A830_GMU_ITCM_BYTES == A8XX_GMU_TCM_DWORDS * 4u && A8XX_GMU_CM3_DTCM_START - A8XX_GMU_CM3_ITCM_START == A8XX_GMU_TCM_DWORDS &&
                   A830_GMU_VA_PERSIST + A830_GMU_PERSIST_BYTES == A830_GMU_VA_LOG &&
                   A830_GMU_VA_LOG + A830_GMU_LOG_BYTES == A830_GMU_VA_HFI, "GMU VA layout (ARCH 4.7.2)");
/* HFI（ARCH 4.7.5–4.7.6） */
A830_STATIC_ASSERT(A830_HFI_PERF_TABLE_DWORDS(12u, 3u) == 49u &&
                   A830_HFI_HDR(A830_HFI_H2F_TABLE, A830_HFI_MSG_CMD, 49u, 1u) == 0x0010310Fu, "M1 header 0x0010310F");
A830_STATIC_ASSERT(A830_HFI_BW_TABLE_DWORDS == 1u + 5u + A830_HFI_BW_MAX_CNOC_CMDS + A830_HFI_BW_MAX_CNOC_LEVELS * A830_HFI_BW_MAX_CNOC_CMDS +
                   A830_HFI_BW_MAX_DDR_CMDS + A830_HFI_BW_MAX_LEVELS * A830_HFI_BW_MAX_DDR_CMDS &&
                   A830_HFI_HDR(A830_HFI_H2F_BW_VOTE_TBL, A830_HFI_MSG_CMD, A830_HFI_BW_TABLE_DWORDS, 2u) == 0x0020A003u,
                   "M2 header 0x0020A003");
A830_STATIC_ASSERT((A830_HFI_HDR(A830_HFI_H2F_FEATURE_CTRL, 0u, A830_HFI_FEATURE_CTRL_DWORDS, 0u) == 0x40Bu) &&
                   (A830_HFI_HDR(A830_HFI_H2F_CORE_FW_START, 0u, A830_HFI_CORE_FW_START_DWORDS, 0u) == 0x20Eu) &&
                   (A830_HFI_HDR(A830_HFI_H2F_GET_VALUE, 0u, A830_HFI_GET_VALUE_DWORDS, 0u) == 0x30Cu) &&
                   (A830_HFI_HDR(A830_HFI_H2F_START, 0u, A830_HFI_START_DWORDS, 0u) == 0x10Au) &&
                   (A830_HFI_HDR(A830_HFI_H2F_GX_BW_PERF_VOTE, 0u, A830_HFI_GX_BW_PERF_VOTE_DWORDS, 0u) == 0x41Eu),
                   "M5/M6/M7/M8/M9 headers");
A830_STATIC_ASSERT(A830_HFI_ACD_TABLE_DWORDS == 5u + 32u && A830_HFI_ACD_ENABLE_BY_LEVEL == (((1u << 12) - 1u) & ~((1u << 4) - 1u)),
                   "ACD table 37 dwords, levels 4..11");
A830_STATIC_ASSERT(A830_HFI_QTBL_SIZE_BYTES == (A830_HFI_QTBL_HDR_DWORDS + A830_HFI_QUEUE_COUNT * A830_HFI_QHDR_DWORDS) * 4u &&
                   A830_HFI_QUEUE_TYPE(A830_HFI_QUEUE_PRIORITY, A830_HFI_QUEUE_ID_DBG) == 0xA02u &&
                   A830_HFI_QUEUE_OFFSET(A830_HFI_QUEUE_ID_DBG) + A830_HFI_QUEUE_DWORDS * 4u == A830_GMU_HFI_BYTES,
                   "HFI queue table fits the 16 KiB HFI region");
A830_STATIC_ASSERT(A830_HFI_IFPC_HYSTERESIS == A830_XO_HZ / 1000u * 300u / 1000u, "IFPC hysteresis = 300 us of XO");
/* RPMh 与 SCM（ARCH 4.3.2、4.4.1、4.7.7） */
A830_STATIC_ASSERT(A830_CN0_VOTE_ON == A830_BCM_TCS_CMD(1, 1, 0, 1) && A830_CN0_VOTE_OFF == A830_BCM_TCS_CMD(1, 0, 0, 0),
                   "CN0 votes 0x60000001 / 0x40000000");
A830_STATIC_ASSERT(A830_DEP_VOTE(A830_DEP_CX_NONE, 3u) == 0xFFFFC3FFu && A830_DEP_VOTE_LINUX(A830_DEP_CX_NONE, 3u) == 0x0FFFC3FFu,
                   "dependency vote, KGSL and Linux forms");
A830_STATIC_ASSERT(A830_ARC_VOTE(2u, 1u, 0x80u) == 0x00800102u, "ARC vote = vlvl<<16 | sec<<8 | pri");
A830_STATIC_ASSERT(A830_SCM_GPU_INIT_REQ == (A830_SCM_GPU_REQ_ALWAYS_EN | A830_SCM_GPU_REQ_TSENSE_EN), "GPU_INIT_REGS x2 = 0x9");
/* GPU VA 与固件（ARCH 4.6.2、4.9、4.11） */
A830_STATIC_ASSERT(A830_SQE_IMAGE_BYTES == 0x1B510u && A830_SQE_IMAGE_BYTES <= A830_GPU_VA_SQE_BYTES &&
                   A830_AQE_IMAGE_BYTES == 0x62F8u && A830_AQE_IMAGE_BYTES <= A830_GPU_VA_AQE_BYTES, "microcode fits its VA");
A830_STATIC_ASSERT(A830_GPU_VA_SQE + A830_GPU_VA_SQE_BYTES <= A830_GPU_VA_AQE && A830_GPU_VA_AQE + A830_GPU_VA_AQE_BYTES <= A830_GPU_VA_RING &&
                   A830_GPU_VA_RING % A830_GPU_VA_RING_BYTES == 0u && A830_GPU_VA_RING + A830_GPU_VA_RING_BYTES <= A830_GPU_VA_MEMPTRS &&
                   A830_GPU_VA_PWRUP == A830_GPU_VA_MEMPTRS + A830_GPU_VA_KERNEL_PAGE_BYTES &&
                   A830_GPU_VA_SCRATCH == A830_GPU_VA_PWRUP + A830_GPU_VA_KERNEL_PAGE_BYTES &&
                   A830_GPU_VA_SCRATCH + A830_GPU_VA_KERNEL_PAGE_BYTES <= A830_GPU_VA_GMEM, "CB0 kernel layout");
A830_STATIC_ASSERT(A830_GPU_VA_UCHE_TRAP >= 0x100000000ull && A830_GPU_VA_CLIENT_END_4G == 0x100000000ull, "trap beyond 32-bit input");
A830_STATIC_ASSERT(A830_GPU_VA_UCHE_TRAP >= A830_GPU_VA_CLIENT_END_16G && A830_GPU_VA_CLIENT_END_16G == 0x400000000ull,
                   "trap beyond the 34-bit input of pagani-gpu-va=16");
A830_STATIC_ASSERT(A830_ZAP_FW_FILE_BYTES == 0x2F38u && A830_ZAP_METADATA_BYTES == 0x94u + (0x2F38u - 0x2000u) &&
                   A830_ZAP_METADATA_BYTES <= 0x1000u && A830_ZAP_MEM_SETUP_BYTES <= A830_ZAP_BYTES, "ZAP image (ARCH 4.11)");
A830_STATIC_ASSERT(A830_MEMPTRS_BV_RPTR == 8u && A830_MEMPTRS_BV_FENCE == 12u, "memptrs layout (ARCH 4.13.1)");
/* DPU 与面板（ARCH 5.3.3–5.3.4） */
A830_STATIC_ASSERT(SM8750_MDP_PHYS + SM8750_MDP_INTF1 == 0x0AE36000ull && SM8750_MDP_PHYS + SM8750_MDP_CTL(0) == 0x0AE16000ull &&
                   SM8750_MDP_PHYS + SM8750_MDP_SSPP_VIG(0) == 0x0AE05000ull && SM8750_MDP_PHYS + SM8750_MDP_SSPP_DMA(5) == 0x0AE2F000ull &&
                   SM8750_MDP_PHYS + SM8750_MDP_INTF1_INTR_EN == 0x0AE361C0ull &&
                   SM8750_MDP_PHYS + SM8750_MDP_INTF1_TEAR_INTR_CLEAR == 0x0AE36808ull, "DPU addresses");
A830_STATIC_ASSERT(PKX110_PANEL_FRAME_BYTES == 0xC3F000u && PKX110_SCANOUT_BUF(2) == 0xFE07E000ull &&
                   PKX110_SCANOUT_BUF(3) <= PKX110_SPLASH_PHYS + PKX110_SPLASH_BYTES, "three scanout buffers fit the splash region");

/* ===================================================================================================================
 * 21. 集成时并入的补充（各模块实现时在本地“A830Regs.h additions”块里补的事实，集成步骤统一移到这里）
 *     来源同上：[MESA] a8xx_regs.txt / adreno_pm4.xml / a6xx_enums.xml，[KGSL] gen8_reg.h、adreno_hfi.h，
 *     [LNX] a8xx_gpu.c、a6xx_gmu.c、qcom_aoss.c。寄存器偏移已加进 tests/test_a830regs.c 的 Mesa 逐项比对。
 * =================================================================================================================== */
/* RBBM 状态与 SECVID（GX）[MESA][KGSL GEN8_RBBM_SECVID_TSB_STATUS_LO/HI] */
#define A8XX_RBBM_GFX_STATUS1           0x00016u
#define A8XX_RBBM_LPAC_STATUS           0x00018u
#define A8XX_RBBM_GFX_BR_STATUS         0x0001Au
#define A8XX_RBBM_GFX_BV_STATUS         0x0001Cu
#define A8XX_RBBM_MISC_STATUS           0x0001Eu
#define A8XX_RBBM_MISC_STATUS_SMMU_STALLED_ON_FAULT (1u << 0)   /* 挂死时 SMMU 正 stall：不判挂死（Linux） */
#define A8XX_RBBM_SECVID_TSB_STATUS     0x0FC00u       /* reg64：TSB 写错误地址 */
#define A8XX_RBBM_SECVID_TSB_STATUS_HI  0x0FC01u       /* [16:0] 地址 48:32，[31:23] MID */
/* CP 故障与恢复 [LNX a8xx_recover、a8xx_cp_hw_err_irq][KGSL CP_SW_*] */
#define A830_CP_SQE_CNTL_HALT           0x00000003u    /* 恢复：GX 在电时停 SQE */
#define A8XX_CP_SQE_STAT_PC             0u             /* CP_SQE_STAT_ADDR_PIPE 下标 0：SQE PC */
#define A8XX_CP_SQE_STAT_OPCODE         1u             /* 下标 1：出错的操作码 */
#define A8XX_CP_SW_FAULT_OPCODE_ERROR   (1u << 8)      /* CP_INTERRUPT_STATUS_PIPE */
#define A8XX_CP_SW_FAULT_REG_PROTECTION (1u << 10)
#define A8XX_GBIF_CX_CONFIG_CLOCK_GATING (1u << 0)     /* GMU 启动时置位；HWCG 关时清（Linux） */
/* GMU 冷启动与中断接线 [LNX a6xx_gmu_start][ARCH 4.7.9、4.12.1] */
#define A830_GMU_LEGACY_INIT_DONE       0xBABEFACEu    /* 核版本 ≤ 0x20010004 的 CM3_FW_INIT_RESULT */
#define A830_SPI_LEVEL_READBACK         0xA003u        /* armcpu_spi_attach 读回：电平、Group1、使能、优先级 0xA0 */
#define A830_SPI_READBACK_MASK          0xFF07u
#define A830_AHB_FENCE_POLLS            100u           /* IFPC：AHB_FENCE_CTRL 轮询次数，间隔 1 µs [KGSL gen8_irq_poll_fence] */
/* HFI 布局 [KGSL adreno_hfi.h][ARCH 0.3、4.7.5] */
#define A830_HFI_KGSL_QTBL_HEADERS      17u            /* KGSL HFI_QUEUE_HDR_MAX：848 = ALIGN(24 + 17×48, 16) */
#define A830_HFI_KGSL_DBG_PRIORITY      40u            /* KGSL HFI_DBG_PRI（Linux 布局三条队列都是 10） */
#define A830_HFI_MAX_PAYLOAD            254u           /* 头的 size 字段 8 位：含头 255 dword */
/* hfi_bwtable_cmd 的 dword 下标（头 = 0）与 ACD 表 [KGSL adreno_hfi.h、gen8_gmu_acd_probe] */
#define A830_HFI_BW_LEVEL_NUM           1u
#define A830_HFI_BW_CNOC_CMDS           2u
#define A830_HFI_BW_DDR_CMDS            3u
#define A830_HFI_BW_CNOC_WAIT           4u
#define A830_HFI_BW_DDR_WAIT            5u
#define A830_HFI_BW_CNOC_ADDRS          6u                                                        /* [6] */
#define A830_HFI_BW_CNOC_DATA           (A830_HFI_BW_CNOC_ADDRS + A830_HFI_BW_MAX_CNOC_CMDS)      /* [2][6] */
#define A830_HFI_BW_DDR_ADDRS           (A830_HFI_BW_CNOC_DATA + A830_HFI_BW_MAX_CNOC_LEVELS * A830_HFI_BW_MAX_CNOC_CMDS)  /* [8] */
#define A830_HFI_BW_DDR_DATA            (A830_HFI_BW_DDR_ADDRS + A830_HFI_BW_MAX_DDR_CMDS)        /* [16][8] */
#define A830_HFI_ACD_VERSION_IDX        1u
#define A830_HFI_ACD_ENABLE_IDX         2u
#define A830_HFI_ACD_STRIDE_IDX         3u
#define A830_HFI_ACD_LEVELS_IDX         4u
#define A830_HFI_ACD_DATA_IDX           5u
#define A830_HFI_ACD_TABLE_VERSION      1u
#define A830_HFI_ACD_TABLE_STRIDE       1u
/* AOSS QMP 描述符的其余偏移与链路状态 [LNX qcom_aoss.c]；平台层 Sm8750AossQmp.h 自带同值枚举，测试逐项比对 */
#define SM8750_AOSS_QMP_DESC_FEATURES   0x08u
#define SM8750_AOSS_QMP_UCORE_LINK_STATE 0x0Cu
#define SM8750_AOSS_QMP_UCORE_LINK_STATE_ACK 0x10u
#define SM8750_AOSS_QMP_UCORE_CH_STATE  0x14u
#define SM8750_AOSS_QMP_UCORE_CH_STATE_ACK 0x18u
#define SM8750_AOSS_QMP_UCORE_MBOX_SIZE 0x1Cu
#define SM8750_AOSS_QMP_UCORE_MBOX_OFFSET 0x20u
#define SM8750_AOSS_QMP_MCORE_LINK_STATE 0x24u
#define SM8750_AOSS_QMP_MCORE_LINK_STATE_ACK 0x28u
#define SM8750_AOSS_QMP_MCORE_CH_STATE  0x2Cu
#define SM8750_AOSS_QMP_MCORE_CH_STATE_ACK 0x30u
#define SM8750_AOSS_QMP_STATE_UP        0x0000FFFFu
#define SM8750_AOSS_QMP_STATE_DOWN      0xFFFF0000u
/* S8 V9c：A8XX 2D 引擎（A2D）实心填充，照 Turnip r2d 路径（tu_clear_blit.cc）[MESA a8xx_regs.txt、adreno_pm4.xml] */
#define A8XX_PM4_CP_WAIT_MEM_WRITES     0x12u
#define A8XX_PM4_CP_BLIT                0x2Cu          /* 负载：OP [3:0] */
#define A8XX_CP_BLIT_OP_SCALE           3u
#define A8XX_EVENT_CCU_CLEAN_COLOR      0x21u          /* vgt_event_type（A7XX 起） */
#define A8XX_GRAS_A2D_BLT_CNTL          0x08500u
#define A8XX_GRAS_A2D_DEST_TL           0x08505u       /* a6xx_reg_xy：X [13:0]，Y [29:16] */
#define A8XX_GRAS_A2D_DEST_BR           0x08506u
#define A8XX_RB_CCU_CACHE_CNTL          0x088E5u       /* COLOR_OFFSET [13:0]>>12、COLOR_CACHE_SIZE [15:14]、DEPTH_OFFSET [29:16]>>12、DEPTH_CACHE_SIZE [31:30] */
#define A8XX_RB_A2D_BLT_CNTL            0x08C00u
#define A8XX_RB_A2D_PIXEL_CNTL          0x08C01u
#define A8XX_RB_A2D_DEST_BUFFER_INFO    0x08C17u       /* COLOR_FORMAT [7:0]、TILE_MODE [9:8]、COLOR_SWAP [11:10] */
#define A8XX_RB_A2D_DEST_BUFFER_BASE    0x08C18u       /* reg64，64 字节对齐 */
#define A8XX_RB_A2D_DEST_BUFFER_PITCH   0x08C1Au       /* 字节 >> 6；单行缓冲为 0 */
#define A8XX_RB_A2D_CLEAR_COLOR_DW0     0x08C2Cu       /* DW0..DW3 */
#define A8XX_SP_A2D_OUTPUT_INFO         0x0A9BFu       /* HALF [0]、IFMT_TYPE [2:1]、COLOR_FORMAT [10:3]、SRGB [11]、MASK [15:12] */
#define A8XX_TPL1_A2D_BLT_CNTL          0x0B2D2u       /* RAW_COPY [0]、TYPE [31:29] */
#define A8XX_FMT6_32_UINT               0x4Bu          /* a6xx_enums.xml */
#define A8XX_R2D_INT32                  7u
#define A8XX_OUTPUT_IFMT_2D_UINT        2u
#define A8XX_TEX_2D                     1u
#define A8XX_A2D_BLT_CNTL(fmt, ifmt)    ((1u << 7) | (((uint32_t)(fmt) & 0xFFu) << 8) | (0xFu << 20) | (((uint32_t)(ifmt) & 7u) << 24))
#define A8XX_SP_A2D_OUTPUT_INFO_VALUE(type, fmt) ((((uint32_t)(type) & 3u) << 1) | (((uint32_t)(fmt) & 0xFFu) << 3) | (0xFu << 12))
#define A8XX_TPL1_A2D_BLT_CNTL_TYPE(t)  (((uint32_t)(t) & 7u) << 29)
#define A8XX_REG_XY(x, y)               (((uint32_t)(x) & 0x3FFFu) | (((uint32_t)(y) & 0x3FFFu) << 16))
/* A830 sysmem 的 CCU 布局（Mesa fd6_calc_gmem_cache_offsets，A830：6 个 CCU、12 MiB GMEM）：深度 0x870000、颜色 0x9F0000 */
#define A830_CCU_SYSMEM_DEPTH_OFFSET    0x00870000u
#define A830_CCU_SYSMEM_COLOR_OFFSET    0x009F0000u
#define A830_RB_CCU_CACHE_CNTL_SYSMEM   ((A830_CCU_SYSMEM_COLOR_OFFSET >> 12) | ((A830_CCU_SYSMEM_DEPTH_OFFSET >> 12) << 16))

A830_STATIC_ASSERT(A830_RB_CCU_CACHE_CNTL_SYSMEM == 0x087009F0u, "A8XX RB_CCU_CACHE_CNTL for sysmem");
A830_STATIC_ASSERT(A8XX_A2D_BLT_CNTL(A8XX_FMT6_32_UINT, A8XX_R2D_INT32) == 0x07F04B80u, "A2D_BLT_CNTL: solid R32_UINT, mask 0xf");
A830_STATIC_ASSERT(A830_HFI_BW_DDR_DATA + A830_HFI_BW_MAX_LEVELS * A830_HFI_BW_MAX_DDR_CMDS == A830_HFI_BW_TABLE_DWORDS,
                   "hfi_bwtable_cmd field indexes span the 160-dword table");
A830_STATIC_ASSERT(A830_HFI_KGSL_QUEUE_OFFSET(0) == ((24u + A830_HFI_KGSL_QTBL_HEADERS * 48u + 15u) & ~15u),
                   "KGSL queue table: 17 headers before the first queue");
A830_STATIC_ASSERT(A8XX_CP_SW_FAULT_OPCODE_ERROR == (1u << 8) && A8XX_RBBM_MISC_STATUS == 0x1Eu, "CP/RBBM fault bits");

#endif /* A830_REGS_H */
