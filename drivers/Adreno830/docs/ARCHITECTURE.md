# Adreno830 驱动架构（A830 / Gen8 / SM8750 / 一加 13T PKX110）

版本：2026-10-29（首版设计，尚未编码；同日按审阅意见修订，改动清单见第 10 节）
范围：本仓库 `/Users/xiaochoumao/unixnu-13t/Adreno830`（由 UniXNU `drivers/Adreno740@5c8c145` 的 MIT 副本 `1b5e192` 演化而来）要成为一份自写的、源码级的 A830 内核 GPU 驱动，在 UniXNU（XNU，iPadOS 17.7.11 / 21H461）上经现有 AdrenoMetal 前端（metal2vulkan + Turnip）完成 GPU 合成桌面，做法与 sheng（小米平板 6S Pro，A740/SM8550）一致。不再对作者的闭源构建打二进制补丁。

配套文档：`docs/IMPLEMENTATION_PLAN.md`（里程碑、文件、函数、测试、命令、上机验收）。

---

## 0. 约定

### 0.1 证据等级（全文统一）

| 标记 | 含义 |
| --- | --- |
| 【实测·本机】 | 在这台 PKX110 上实际发生过的现象（任务给出的事实 1–4，及其直接推论） |
| 【实测·同 SoC】 | 同为 SM8750/A830 的其他真机公开记录：REDMI K90（annibale，HackiOS，已在 XNU 下用 A830 画出 iOS 桌面）、一加 Ace 5 Pro（hummer，UniXNU） |
| 【实测·sheng】 | 在 sheng（A740/SM8550）上已经跑通、且与代际无关的部分 |
| 【源一致】 | 本机型原厂 KGSL 与 Linux 上游一致，本机尚未执行 |
| 【分歧】 | 来源互相矛盾；文中给出默认选择与备选 |
| 【推断】 | 无直接来源，需要上机确认 |

### 0.2 路径缩写

| 缩写 | 实际路径 |
| --- | --- |
| `A830/` | `/Users/xiaochoumao/unixnu-13t/Adreno830` |
| `UX/` | `/Users/xiaochoumao/unixnu-13t/UniXNU`（`b962645`） |
| `DEV/` | `/Users/xiaochoumao/unixnu-13t/UniXNU-Pagani_SM8750-gpu`（GPU 工作树，当前游离在 `3c39baf`；主线分支是 `UniXNU-Pagani_SM8750` 的 `oneplus13t-512g`，`cdd273d`） |
| `PD/` | `/Users/xiaochoumao/unixnu-13t/UniXNU-Pagani-Drivers`（分支 `fast-display`，基于 `14f50fc`，有未提交改动；文中 `PD` 的行号一律以已提交的 `14f50fc` 为准，工作区里会有偏移） |
| `SOC/` | `/Users/xiaochoumao/unixnu-13t/UniXNU-sm8750-common`（`b79d675`） |
| `SHENG/`、`SM8550/` | `/Users/xiaochoumao/unixnu-13t/参考资料/UniXNU-sheng`、`参考资料/UniXNU-sm8550-common` |
| `LDT` | `/tmp/claude-501/live.dts`（本机安卓运行时设备树，来源 live.dtb sha256 `d00767f2…`） |
| `S/` | 本轮草稿目录 `/private/tmp/claude-501/-Users-xiaochoumao-Library-Application-Support-Claude-scratch-workspaces-8d57cd08-f137-4208-a8b6-3362ffa35fb5-03638cf4-7c10-4079-a7eb-13992523ab3d-scratch-2026-10-29-ff4716/5559c563-618d-4813-9848-be3768908d8c/scratchpad` |
| `K/` | `S/design/smmu-cp/dl_kgsl13t`：OnePlus `sm8750_b_16.0.0_oneplus_13t` 分支的 KGSL（本机型原厂驱动） |
| `KG16/` | `S/wf/gmu/kgsl_dl16`（KGSL gen8 GMU/HFI/RPMh） |
| `L/` | `S/design/smmu-cp/dl_linux`（Linux 主线，文件名中 `/` 换成 `_`） |
| `AW/`、`AK/`、`HL` | annibale 工作区 `S/design/smmu-cp/dl_annibale/ws`、annibale 内核 `.../kernel/xnu/files/bsd/dev`、hummer 日志 `S/wf2/smmu/dl_hummer/full_log.txt` |
| `DEC/` | `/Users/xiaochoumao/unixnu-13t/作者产物/ida/decomp`（作者 build 68 伪代码，只作硬件事实来源） |
| `R1`、`R2` | 第一轮逆向（`wf_d4f080e7-45c/journal.jsonl`）、第二轮排雷（`wf_fa803663-3e9/journal.jsonl`） |
| `docs/inputs/` | 本仓库里归档的设计输入（见 0.4） |

GPL 代码（Linux、KGSL、annibale 内核）只用来学习寄存器、取值和时序；驱动代码一律自写，不粘贴大段原文。

### 0.3 地址约定

- GPU 寄存器在 KGSL/Mesa 里是相对 `0x03D00000` 的 dword 偏移：物理地址 = `0x03D00000 + 4×dword`。
- GMU 窗口（DT `gmu@3d37000`，长 `0x68000`）内的字节偏移 = `4×dword − 0x37000`。例：`CM3_SYSRESET`（dword `0x1F800`）= 物理 `0x3D7E000` = 窗口 `+0x47000`。
- RSCC 寄存器（`0x03D50000`）、GPU SMMU（`0x03DA0000`）、GPUCC（`0x03D90000`）、GCC（`0x00100000`）一律写成物理地址或块内偏移，文中会标明。
- 所有 MMIO 为 32 位访问，映射成 Device-nGnRE（`kIOMapInhibitCache`）；需要“写完再轮询”的写之后执行 `dsb sy`。
- 中断号：INTID = SPI + 32。

### 0.4 归档的设计输入（`docs/inputs/`）

草稿目录在 `/private/tmp`，会随重启丢失，所以本次把后续实现直接要用的输入复制进了仓库：

| 文件 | 内容 |
| --- | --- |
| `docs/inputs/power-gmu/A830PowerGmuRegs.h` | 电源、时钟、GMU、HFI、RSCC、CX_MISC 的地址与取值（已用 clang 编译并做过静态断言） |
| `docs/inputs/power-gmu/gmufw.py`、`pwrlevels.py` | `gen80000_gmu.bin` 块解析、DT 档位表解析 |
| `docs/inputs/perf/a830_perf_regs.h` | 中断、忙计数、LLCC、DPU、档位与带宽表常量 |
| `docs/inputs/regs/a8xx_regs.txt`、`a8xx_gmu_regs.txt` | 由 Mesa 寄存器 XML（MIT）导出的 A8xx dword 偏移表 |
| `docs/inputs/userspace/patches/*` | 用户态补丁：构建脚本、Turnip a8xx 回移、darwin 后端、AdrenoMetal 插件、hostsim A830 设备信息 |
| `docs/inputs/userspace/build-userspace-a830.sh`、`prime-list-21H461.txt` | 用户态构建步骤、21H461 预热列表（161 项） |

M0 会把两个头文件合并成正式的 `A830Regs.h`（见 2.2）。

---

## 1. 目标、边界与总体结论

### 1.1 目标

1. **完整**：从冷启动到 GPU 合成桌面全部由本仓库与板级代码完成：CX 域上电、主机侧 RPMh 票、SCM、GPU SMMU、GMU 冷启动与 HFI、GX 上电、Gen8 硬件初始化、SQE/AQE、CP_ME_INIT、ZAP 退出安全模式、中断、栅栏、内存与一致性、LLCC、DCVS、带宽，以及显示扫描输出交接。
2. **精确**：每一步的寄存器、数值、顺序、超时都有出处；凡是本机实测过的路径按实测行为实现；回读只校验功能位，不做整字比对。
3. **快**：不重犯作者候选版的五类性能问题（每次提交整块缓存维护、屏蔽中断后忙轮询、无 LLCC、固定频率且 OOB 永不释放、每次提交的重诊断），并把 13T 显示路径从 CPU 整帧拷贝改成 GPU 交接与 DPU 翻页。
4. **ABI 不变**：`NativeAdrenoABI.h` 逐字节保持 v2，AdrenoMetal 插件与 Turnip darwin 后端不需要感知内核换了代际（只改 `DeviceInfo` 的取值）。

### 1.2 不做（首版）

- 不走 KGSL 的 HWSCHED（GMU 调度）模式，只做 RB 模式：主机写环，GMU 只管电源和频率。Linux 上游对 A830 也是 RB 模式（`L/drivers_gpu_drm_msm_adreno_a8xx_catalog.c:1535-1578` 收录 `0x44050001`）。
- 不开 IFPC、抢占、LPAC 队列、硬件栅栏、按进程页表（`CP_SMMU_TABLE_UPDATE`）、安全渲染。它们都列入后期里程碑。
- 不把 GPU 节点导出到 ADT（`DEV/dts` 中不加 `unixnu,adt-name`），避免改变 arm-io 基址（`UX/scripts/unixnu/devicetree.py:192-209`）。
- 不安装 IOKit personality（静态内核没有 personality 机制，`UX/scripts/build-xnu-4k-baseline.sh:634-635` 注释）。

### 1.3 研究间分歧的裁决

下表是本设计对五份研究中互相矛盾之处的最终取舍，后文各节都以此为准。

| # | 议题 | 各方说法 | 裁决 | 依据 |
| --- | --- | --- | --- | --- |
| 1 | GPU SMMU 槽位/上下文 | reuse：沿用 sheng（GMU bank1/slot2/SID5）；smmu-cp：GPU SID0→槽0→CB0、LPAC SID1→槽3→CB0、GMU SID5→槽1→CB2 | 采用 smmu-cp 的分配；运行时确认目标槽 SMR 无效，否则顺延到最低空闲槽并记录 | 【实测·同 SoC】annibale（`AK/adreno8_memory.c:11-16`）；作者写 CB1/S2CR=0x1 在 annibale 上触发同步外部中止（`AW/docs/progress/...gpu-initialization.md:53`） |
| 2 | ACTLR | DT 0x32B；perf：尽量写 0x32B 不回读；smmu-cp：0x303 | 默认写 `0x303`，回读只记录；写前读 `sACR`，`CACHE_LOCK`(bit26)=1 时先清；`pagani-gpu-actlr=` 留作单变量实验 | 【实测·同 SoC】0x303 可写回（annibale）；R2 指出作者从不清 CACHE_LOCK |
| 3 | ASID | sheng：GMU 1、GPU 2；annibale：等于 CB 号 | ASID = CB 号 + 1：GPU(CB0)=1，GMU(CB2)=3。现有代码拒绝 ASID 0（`A830/NativeAdrenoGpuContext.cpp:17`） | SCTLR.ASIDPNE 不保留，ASID 必须全局唯一【实测·同 SoC】 |
| 4 | GPU 输入地址位宽 | smmu-cp：39 位 + TTBR1 空表；现有 VM：2 GiB 全拓扑 Live 表 | 首版 32 位输入；S3（M2）起 CB0 直接按 4 GiB 光圈建 Live 拓扑（2053 页表）+ TTBR1 空表保护，M7 起客户端窗口按 4 GiB 划分（6.2）；`pagani-gpu-va=2` 回退到 sheng 实测的 2 GiB（1027 页）；39 位与按需页表放到 M14；2026-10-31 起默认 16 GiB 光圈（34 位输入，8209 页表，32 个客户端窗口，`pagani-gpu-va=4` 回到 4 GiB） | Live 拓扑在 39 位下需要约 1 GiB 页表，不可行；窗口扩大若晚于 M8，M8 的小组件判据不可达（第 10 节第 13 条） |
| 5 | GetTimestamp 来源 | reuse：`CP_ALWAYS_ON_COUNTER` 0x8E7；smmu-cp：GMU AO 计数器 | 读 `GMUCX_AO_COUNTER`（dword 0x1F840/0x1F841，物理 0x3D7E100/104），高-低-高读法；上机时与 0x8E7 比对 | KGSL `gen8_read_alwayson`（`K/adreno_gen8.c:2708-2725`）；CX 域，开 IFPC 后也可读 |
| 6 | 主机 CX/MX 票 | 作者：cmd-db 最高档且不撤销【实测·本机】；power-gmu：LOW_SVS；perf：先不动 | 代码默认沿用作者实测的 cmd-db 最高档、运行期不撤销（`pagani-gpu-rails=1` 为默认值）；LOW_SVS（`pagani-gpu-rails=0`，首个 vlvl≥0x40 的档）只作 M11 的单变量实验，验收要比较 UFS、CPU 拷贝帧时间与 MDP 欠载 | apps DRV-2 上每个资源只有一张票，主机写入会替换开机遗留的票，而且读不回来（annibale 写 3 后读回仍为 0，`AW/docs/progress/2026-10-25-gpu-resume-review.md` §22）；UniXNU 里 UFS（根卷）、MDP 没有自己的 CX 票；上游 gpucc required-opps 为 low_svs 只说明 GPU 自身的下限 |
| 7 | 主机 DDR 票 | 作者：第 11 档常驻；KGSL：启动期 bus-min 后 `icc_set_bw(0,0)`；perf：降到显示底线 | 默认沿用作者实测的第 11 级，底线等于启动档（即不降）；ACV 用 HLOS 的 BIT(0)（作者在 apps DRV 上投的是 GPU 专用的 BIT(2)）；SH0/MC0/CN0 是保活资源，主机票永不 valid=0、最低 x=1、y=1；降档（`pagani-gpu-ddr-floor=3`、`pagani-gpu-ddr=2`）只作 M11 单变量实验 | `KG16/adreno_gen8_gmu.c:1883-1905`；`S/wf2/secure-cx/dl_linux/drivers_interconnect_qcom_sm8750.c:1024-1029`（ACV `enable_mask=BIT(0)`）、`:1037-1040,1087-1090,1135-1138`（CN0/MC0/SH0 `keepalive`）；同目录 `drivers_interconnect_qcom_bcm-voter.c:72-77,107-113`；UniXNU 没有其他 DDR 投票者 |
| 8 | 非上下文寄存器表 | 作者用 Linux a830 变体；KGSL gen8_0_0 | 默认 KGSL `gen8_0_0_nonctxt_regs`（`K/adreno-gpulist.h:2808-2872`，本机安卓生产驱动），生成器保留 Linux 变体开关 | R1 核实作者表与 KGSL 有 3 处不同（多 0x8E06=0x2200、缺 UCHE_CACHE_WAYS、TPL1_DBG_ECO_CNTL1=0x04000724） |
| 9 | `RB_GC_GMEM_PROTECT` | smmu-cp：推迟到安全退出后 | 照 Linux：非上下文表里照常写，退出安全模式后再写一次 `0x0C000000` | `L/drivers_gpu_drm_msm_adreno_a8xx_gpu.c:376-391,826-833` |
| 10 | HFI `START` 时机 | Linux/作者：OOB 之前；KGSL 非 hwsched：OOB 之后 | OOB 之前；OOB 应答异常时改顺序做对照 | sheng 实测顺序；`L/a6xx_hfi.c:1096-1102` |
| 11 | 依赖票高 4 位 | KGSL `0xF…`；Linux `0x0…` | KGSL 写法 `0xFFFFC000\|mx<<8\|0xFF` | 本机安卓固件实际收到的就是 KGSL 写法 |
| 12 | AHB 围栏下界 | KGSL 0x8A0；Linux/作者 0x8C0 | 0x8A0；IFPC 关时不生效 | `KG16/adreno_gen8_gmu.c:861-942` |
| 13 | CM3 故障位 | KGSL bit15；Linux bit23 | 两位都在掩码里开放、在 336 的处理函数里无条件清除；只有 bit15 判致命，bit23 只记录（用来回答 9.2 第 4 条） | `KG16/adreno_hfi.h:42-48`（`HFI_IRQ_MASK` = bit1/2/15）；gen8 的 bit16–31 属于 OOB 应答区（`HFI_IRQ_OOB_MASK`） |
| 14 | demet(0x9010)、hub_aon(0x93E8) 分支 | Linux/KGSL 在 SM8750 上不开；作者 M6 开启并在实机通过 | 首版保留实测行为（置位 + 等待，HW_CTL 或 NOC_FSM=ON 即放行）；稳定后做“不开”的单变量实验 | 【实测·本机】事实 2；R2 power-tail |
| 15 | qfprom 速度档读取 | perf：可能中止 | 照读并记录：作者修补版在“GPU board inputs”阶段读过 `0x221C8138` 后进入了“GPU power”【实测·本机，间接】；档位表默认只用各 bin 共有的 ≤900 MHz 11 档，`pagani-gpu-speedbin=1` 才按熔丝扩展 | `DEC/fffffff00bfd9bd0__pagani_gpu_start.c:108,196-203` |
| 16 | 启动方式 | sheng：后台线程 + 10 s 延迟；reuse/作者：同步 | 同步执行（late probe 内，早于 launchd）；`pagani-gpu-async=1` 仅供开发 | backboardd 启动时必须已能找到 IOAcceleratorES |
| 17 | HFI 传输 | reuse：复用 `UX/kits/AdrenoFirmware/AdrenoHfi.c` | 自写 `A830Hfi`：v5 应答、整头匹配、1 s 睡眠等待、`0xE00` 早退、调试队列排空；套件不改，避免影响 sheng | `AdrenoHfiPoll` 把 word2≠0 一律判失败（`UX/kits/AdrenoFirmware/AdrenoHfi.c:91`），对 v5 消息是错的 |
| 18 | 中断下半部 | smmu-cp：IOEventSource；perf：thread_call | `thread_call`（中断上下文可调用）；栅栏等待者用 `thread_wakeup` 直接唤醒 | 不需要额外的 IOWorkLoop 对象 |
| 19 | `output_bits` | reuse：40 | 取 IDR2.OAS 实际值（SM8750 为 36），Query 报 36 | IDR2=0x5511【实测·同 SoC】；BO 分配用它限制物理地址（`A830/NativeAdrenoUserClient.cpp:200-237`） |
| 20 | 客户端窗口 | 现状：backboardd 1 GiB + 3×170.7 MiB | M7 起即用 4 GiB 光圈：backboardd 1.5 GiB + 3×682 MiB（`pagani-gpu-va=2` 回退旧表）；2026-10-30 起 12 槽：backboardd 1.5 GiB + 3×352 MiB + 8×124 MiB，海报扩展也能拿到 Metal 设备；2026-10-31 起默认 16 GiB 光圈的 32 槽：1.5 GiB + 3×512 MiB + 28×448 MiB（6.2） | R1：小组件因 170 MiB 窗口而空白；M8 要验收小组件 |

### 1.4 一句话架构

板级代码 `pagani_gpu_start()` 在 `native_legacy_display_late_probe()` 末尾同步调用 `adreno830_start(&board)`；后者按 S0–S10 的阶段机依次调用平台层（`platform/sm8750/*`：上电、RPMh、SCM、SoC 信息）、SMMU 层（`A830Smmu`）、GMU/HFI 层（`A830Gmu`、`A830Hfi`、`A830Votes`）、CP 层（`A830Cp`、`A830Zap`）、中断与栅栏层（`A830Irq`、`A830Fence`），全部成功后经保持不变的 ABI 外壳（`NativeAdrenoPublish.cpp`、`NativeAdrenoAccelerator.cpp`）发布 `NativeAdrenoGpu` 与 `AGXAccelerator`，再启动 DCVS。任何一步失败只记录阶段、步骤、IOReturn 和最后访问的寄存器，不发布，桌面退回 CPU 呈现。

---

## 2. 模块地图

### 2.1 仓库与层次

```
                         ┌──────────────────────────────────────────┐
 用户态（不改 ABI）       │ AdrenoMetal 插件 + metal2vulkan + Turnip darwin │  UX/drivers/AdrenoMetal、UX/patches/mesa
                         └───────────────▲──────────────────────────┘
                                         │ NativeAdrenoABI.h v2（选择子 1–16、NTNL 隧道、0x4147/0x4148）
 ┌───────────────────────────────────────┴──────────────────────────────────────────┐
 │ Adreno830 仓库（A830/，MIT）                                                        │
 │  ABI 外壳：NativeAdrenoPublish / NativeAdrenoAccelerator / NativeAdrenoUserClient   │
 │  通用核心：Vm / Memory / Submit / Ring / Timer / Firmware / Sha256 / HandleIndex     │
 │  Gen8 专有：A830Smmu / A830Gmu / A830Hfi / A830Votes / A830Cp / A830Zap / A830Irq   │
 │            A830Fence / A830Dcvs / A830HwInit.h（生成）/ A830Regs.h                 │
 │  编排与诊断：Adreno830Start / Adreno830Status / Adreno830Board.h                    │
 │  SoC 后端：platform/sm8750/ Sm8750GpuPower / Sm8750Rpmh / Sm8750Scm / Sm8750SocInfo │
 │            Sm8750Llcc / Sm8750AossQmp（P3）                                         │
 └───────▲───────────────────────────────▲──────────────────────────────▲───────────┘
         │ adreno830_start(board)          │ 套件                          │ 头文件
 ┌───────┴──────────────┐     ┌───────────┴─────────────┐    ┌───────────┴──────────────┐
 │ 设备仓库 DEV/         │     │ UX/kits：ArmSmmu4K、     │    │ SOC/kits/Sm8750Platform  │
 │ package/PaganiGpu*    │     │ AdrenoPM4（不改）        │    │ Rpmh.h（加 DRV-2 锁）     │
 │ scripts/build-pagani- │     └─────────────────────────┘    └──────────────────────────┘
 │ gpu.py、固件钉选      │
 └───────▲──────────────┘
         │ pagani_gpu_start()（弱默认 + 强定义）
 ┌───────┴──────────────────────────────────────────────────────────────┐
 │ PD/：Stage7Probe（调用点）、BootProgress（GPU 状态行）、               │
 │ NativeMobileDisplay（NativeGOPPresenter 交接、TE 锁相、DPU 翻页）     │
 └──────────────────────────────────────────────────────────────────────┘
```

层次规则：

- 本仓库不读 ADT、不依赖设备仓库的头文件；板级输入全部通过 `Adreno830Board`（见 3.5）传入。
- `platform/sm8750/` 是 SoC 后端，代码自包含（只依赖 `PeripheralIO` 式的读写回调），硬件验证后再整体上移到 `SOC/kits/Sm8750Platform`。目前已存在 `A830/platform/sm8750/Sm8750GpuPower.{h,c}` 与 `A830/tests/test_sm8750_power.c`（未提交；与作者 M6 实测路径的差异见 4.2.1 末尾）。
- 共享套件 `ArmSmmu4K`、`AdrenoPM4` 不改；`AdrenoFirmware` 不再依赖（理由见 1.3 第 17 条）。

### 2.2 文件级映射

“动作”一栏：保留＝原样或只改注释/包含；参数化＝结构保留、常量改为注入；替换＝删除旧文件、写新文件；删除＝不再编进内核归档；新增＝新写。

| 文件（现状） | 动作 | 新文件 / 类 | 要点 |
| --- | --- | --- | --- |
| `NativeAdrenoABI.h` | 保留 | 同名 | 逐字节不变；`UX/patches/mesa/turnip-darwin-kmd.patch:290`、`UX/scripts/build-adreno-metal-plugin.py:41,84` 直接包含 |
| `NativeAdrenoStatus.h` | 保留 + 接线 | 同名 | `mount` 时 `attachStatus`；IRQ/SMMU 致命故障 `noteFault`；复位 `noteSuspend`。现状 `GetStatus` 恒返回 Unsupported（`NativeAdrenoPublish.cpp:240-241`） |
| `NativeAdrenoUserClient.{h,cpp}` | 保留 + 小改 | 同名 | 删除提交路径上的 CPU 前序等待（`:326-341` 的 `observeFence`，改为只校验 `wait_fence ≤ lastSubmitted`）；句柄编码改动（6.5）必须等 Turnip `darwin_bo_finish` 的顺序修正落地后才切换，且槽号 FIFO 隔离复用；`faults` 按连接计数、页故障按 FAR 归属到连接（4.12.3、4.13.4；故障时在途的作业只作对比计数）；一致性映射策略见 4.14 |
| `NativeAdrenoSubmit.{h,cpp}` | 保留 + 删诊断 | 同名 | 删除 WC 审计与重设（`:163-195`）、一致性采样（`:196-213`）、`publishMapped`（`:200`）、IB 日志（`:219-225`）、50 ms TLBIALL/200 ms GBIF 停机（`:297-316`）；保留 IB 校验；作业槽改为 16 个、每个连接独占 1 个保留槽，调用者没有在途作业时不返回 Busy（5.4） |
| `NativeAdrenoVm.{h,cpp}`、`NativeAdrenoVmAlias.h`、`NativeAdrenoHandleIndex.h` | 保留 + 小改 | 同名 | 删除每 64 次 map/unmap 的 `ArmSmmu4KObserveStreams` 严格回读（`NativeAdrenoVm.cpp:73-90`；R2 metal-reg 证明它会在任何一次客户端页故障后把共享 VM 永久判死）；解除映射前按 BO 的 seqno 判断，不做全局空闲 |
| `NativeAdrenoMemory.{h,cpp}`、`NativeAdrenoCpu.cpp` | 保留 | 同名 | 13T DRAM 在 `0x9_8000_0000` 起（`DEV/dts/oneplus13t.dts:7-10`），OAS 36 位足够 |
| `NativeAdrenoSmmuMemory.{h,cpp}` | 保留 | 同名 | `coherent_walk=1`（DT `dma-coherent`，`LDT:24144`） |
| `NativeAdrenoSmmuContext.{h,cpp}` | 参数化 | 同名 | 保留表池准备；`MaxLeaves` 由 `1<<19` 改为 `1<<20`（4 GiB 光圈）；把 `program/attach` 抽成注入接口 `NativeAdrenoSmmuProgrammer`，13T 由 `A830Smmu` 实现（不再调用 `ArmSmmu4KProgramDisabledContext`/`ArmSmmu4KAttachStreams` 的严格前置条件） |
| `NativeAdrenoGmuContext.{h,cpp}` | 参数化 | 同名 | bank/asid/slot/sid 由板级给：CB2/ASID 3/槽 1/SID 5 |
| `NativeAdrenoGpuContext.{h,cpp}` | 参数化 | 同名 | `GpuSqeBytes` 0x13000→`0x1C000`；新增 AQE 缓冲（IOVA `0x180000`，`0x7000`）；内核缓冲改特权页；槽 {0,3}、SID {0,1}；新增 TTBR1 空表；池大小由板级给（默认 512 MiB）；光圈与 `TablePages` 由 `pagani-gpu-va` 决定（默认 16 GiB/8209 页，4.6.1） |
| `NativeAdrenoGmuMemory.{h,cpp}` | 参数化 | 同名 | 改为由固件 PREALLOC 元数据驱动（见 4.7.2），不再分配 16 MiB 连续 icache |
| `NativeAdrenoRing.{h,cpp}` | 参数化 | 同名 | 引入 `NativeAdrenoRingRegs`（Gen8：scratch 0x82C、SRAM init 0x449/0x5E8、状态 0x19F、BV rptr 0x808、stall 寄存器表）；WPTR 走 KGSL 式围栏写（10 µs×200）；`wait` 改由 `A830Fence` 提供，每个带栅栏的提交在 WPTR 写之前给队列计量打戳（`NativeAdrenoRingWaiter::stamp`，4.13.5）；删除 112 字环日志与整环清洗（`:296-308`）、`kick`/`drainBus` 在正常等待中的调用 |
| `NativeAdrenoTimer.{h,cpp}` | 参数化 | 同名 | 计数器改为 GMU AO 0x1F840（GMU 窗口 `+0x47100/+0x47104`），高-低-高读 |
| `NativeAdrenoPwrup.{h,cpp}` | 参数化 | 同名 | 新增 `buildGen8()`（三元组动态表，`pack()` 已支持，`:51-55`） |
| `NativeAdrenoFirmware.{h,cpp}`、`NativeAdrenoSha256.{h,cpp}` | 保留 | 同名 | 新增 gen80000 四个固件的钉选规则 |
| `NativeAdrenoFirmwareDefaults.cpp` | 参数化 | 同名 | 增加 AQE、ZAP 的弱默认定义（长度 0） |
| `NativeAdrenoKernelTime.{h,cpp}` | 保留 | 同名 | 删去 `NativeAdrenoKernelTime.h:4` 对 `NativeAdrenoGmuPlatform.h` 的包含 |
| `NativeAdrenoPublish.cpp` | 保留 + 改五处 | 同名 | ① `native_gop_handoff_call/memory` 加弱默认定义（返回 `kIOReturnUnsupported`/`nullptr`）；② `boot_log_export_put` 加弱默认（返回 -1）；③ WaitFence 改为无锁睡眠，线程被中断时返回 `kIOReturnTimeout`（4.13.3）；④ 只有挂死类故障才进入全局设备丢失并拒绝新连接，页故障只让出错的连接丢失（4.13.4）；⑤ `NativeAdrenoHandoffLast` 由 0x7003 改为 0x7005（6.3，M9） |
| `NativeAdrenoAccelerator.cpp` | 保留 | 同名 | `:9` 的包含改为 `../AdrenoMetal/OriginalAcceleratorABI.h`（`drivers/NativeMetal` 只是官方别名，`UX/scripts/unixnu/components.py:128-151`） |
| `NativeAdrenoClient.{h,cpp}`、`NativeAdrenoBoard.{h,cpp}` | 保留（只给主机测试） | 同名 | 不进内核归档 |
| `NativeAdrenoA740HwInit.h` | 替换 | `A830HwInit.h`（生成） | 由 `scripts/derive-a830-hwinit.py` 从钉选的 KGSL `adreno-gpulist.h` 与 Linux `a8xx_catalog.c` 生成：非上下文表、保护表、GBIF CX 表、AO HWCG、双三次权重、pwrup/ifpc/dyn 表 |
| `NativeAdrenoPwrupRegs.h` | 替换 | 并入 `A830HwInit.h` | — |
| `NativeAdrenoCpInit.{h,cpp}` | 替换 | `A830Cp.{h,cpp}`（类 `A830Cp`） | hw_init、微码基址、环启动、`CP_ME_INIT`、安全退出后收尾、HWCG。注意 Gen8 的 `CP_SQE_CNTL` 是 0x815，而 0x808 在 Gen8 上是 `CP_RB_RPTR_ADDR_BV`（`docs/inputs/regs/a8xx_regs.txt:310,319`） |
| `NativeAdrenoIrq.{h,cpp}` | 替换 | `A830Irq.{h,cpp}` | Gen8 偏移（0x6A/0x61/0x62）、`armcpu_spi_attach` 接线、上半部/下半部 |
| `NativeAdrenoGmuPlatform.{h,cpp}`、`NativeAdrenoGmuConfig.{h,cpp}` | 替换 | `A830Gmu.{h,cpp}` | 去掉对 `SM8550/kits/Sm8550Platform/ShengGmu.h` 的依赖；窗口集 {Gmu, GmuAo, Rscc, Gpu, CxMisc}，无 PDC |
| `NativeAdrenoGpu.{h,cpp}` | 替换 | `Adreno830Start.{h,cpp}` | 唯一编排器：`extern "C" IOReturn adreno830_start(const Adreno830Board *)` |
| `NativeAdrenoService.cpp`、`NativeAdrenoGmuService.*`、`NativeAdrenoSmmuService.*`、`Info.plist` personality | 删除 | — | sheng 上从未实例化：静态内核不装 personality，GPU 节点不进 ADT，`parseReg` 还按 FDT 大端解析（R1；reuse 研究第 1 节 D 项） |
| — | 新增 | `Adreno830Board.h` | 板级接口结构（3.5） |
| — | 新增 | `Adreno830Status.{h,cpp}` | 状态块、错误码、叠加层行、注册表快照（第 8 节） |
| — | 新增 | `A830Regs.h` | 合并 `docs/inputs/power-gmu/A830PowerGmuRegs.h`、`docs/inputs/perf/a830_perf_regs.h` 与 CP/SMMU 偏移 |
| — | 新增 | `A830Smmu.{h,cpp}` | 预检、认领、上下文编程、LPAC 路由、aperture、故障中断、关闭（4.5） |
| — | 新增 | `A830GmuFirmware.{h,cpp}` | `gen80000_gmu.bin` 块解析与装载（4.7.1） |
| — | 新增 | `A830Hfi.{h,cpp}` | HFI 队列与 Gen8 消息（4.7.5–4.7.6） |
| — | 新增 | `A830Votes.{h,cpp}` | cmd-db ARC/BCM、GX/CX/依赖票、带宽表、ACD 表（4.7.7） |
| — | 新增 | `A830Zap.{h,cpp}` | MDT 解析与 PAS 13（4.11） |
| — | 新增 | `A830Fence.{h,cpp}` | 栅栏事件与睡眠等待（4.13）；环的队列计量（4.13.5） |
| — | 新增 | `A830Dcvs.{h,cpp}` | 调频线程与策略（4.16） |
| — | 新增（M10 最小恢复，M13 完善） | `A830Recovery.cpp` | 挂死恢复（4.13.4） |
| `platform/sm8750/Sm8750GpuPower.{h,c}` | 已完成（未提交） | 同名 | 审查 P-1..P-6 已修：注释改为调用方持有 cx.lvl/mx.lvl 票；NOC_FSM=ON 放行；GCC AHB 回读加 10 µs；PLL0 半启动自愈；trace 指向出错寄存器；新增断电 `Sm8750GpuPowerOff`（4.2.3）。带依赖关系的主机模型（`tests/test_sm8750_power.c`）与 26 个变异（`tests/mutate_sm8750_power.py`）全部通过 |
| — | 新增 | `platform/sm8750/Sm8750Rpmh.{h,c}` | cmd-db ARC/BCM 查找、DRV-2 ACTIVE TCS 写 N 条命令、共享锁（4.3） |
| — | 新增 | `platform/sm8750/Sm8750Scm.{h,c}` | SMC 封装：INTERRUPTED/WAITQ/EBUSY、is_available（4.4.1） |
| — | 新增 | `platform/sm8750/Sm8750SocInfo.{h,c}` | SMEM 分区表与全局分区解析，只读可读页（4.1.2） |
| — | 新增（M11） | `platform/sm8750/Sm8750Llcc.{h,c}` | LLCC v6 切片激活（4.15） |
| — | 新增（M11） | `platform/sm8750/Sm8750Tsens.{h,c}` | gpuss 温度只读采样（4.16） |
| — | 新增（M3 可选：ACD 关闭消息；P3：开启） | `platform/sm8750/Sm8750AossQmp.{h,c}` | AOSS QMP 握手与 ACD 状态消息（4.17） |
| `tests/*.cpp` | 参数化 + 新增 | 见 `docs/IMPLEMENTATION_PLAN.md` 第 2 节 | 由本仓库 `tests/run-host-tests.py` 独立驱动 |
| — | 新增 | `scripts/build-adreno830.py` | 组件构建脚本（7.1） |
| — | 新增 | `scripts/derive-a830-hwinit.py` | 生成 `A830HwInit.h` |

### 2.3 运行期对象与所有权

| 对象（静态单例） | 所在文件 | 生命周期 | 被谁使用 |
| --- | --- | --- | --- |
| `Adreno830Device g830` | `Adreno830Start.cpp` | 开机到关机，从不释放 | 编排器、IRQ、DCVS、发布外壳 |
| `NativeAdrenoGmuContext`（CB2） | 同上 | 同上 | GMU 固件与 HFI 共享内存 |
| `NativeAdrenoGpuContext`（CB0） | 同上 | 同上 | 内核缓冲与用户 BO 的共享 VM |
| `NativeAdrenoRing`、`NativeAdrenoSubmit`、`NativeAdrenoTimer` | 同上 | 同上 | `native_adreno_publish()` 接手 |
| `A830Gmu`、`A830Hfi` | 同上 | 同上 | 电源状态切换、DCVS（HFI 全程串行，一把互斥锁） |
| `A830Irq`、`A830Fence` | 同上 | 同上 | 中断上半部、下半部线程、等待者 |
| `Adreno830Status adreno830_status` | `Adreno830Status.cpp` | 全局导出 | 叠加层、注册表、主机工具 |
| `NativeAdrenoGpuPublished`（名 `NativeAdrenoGpu`） | `NativeAdrenoPublish.cpp` | 注册后常驻 | 直通通道（`TU_DARWIN_SERVICE`） |
| `AGXAccelerator : IOAcceleratorES` | `NativeAdrenoAccelerator.cpp` | 注册后常驻 | backboardd 的 Metal 发现、`AGXDeviceUserClient` |


---

## 3. 13T 上的启动流程

### 3.1 调用链与时机

sheng 的 GPU 不靠 IOKit 匹配，13T 沿用同样的“板级显式启动”，但改成同步执行（理由见 1.3 第 16 条）：

```
bsd_init → bsd_autoconf → IOKitBSDInit()                       （UX/patches/xnu/offline-legacy-start.patch）
  └─ native_legacy_display_late_probe()                         PD/drivers/Stage7Probe/Stage7Probe.cpp:153
       ├─ startKeyState / IOHIDFamily / IOHIDEventDriver / IOHIDResource
       ├─ IOSurface 载入，等待 IOCoreSurfaceRoot
       ├─ 建 gop-display nub，NativeLegacyDisplay::start         （CPU 呈现路径就绪）
       ├─ startPeripherals(hidReady)                            （按键、触控、面板；触控在这里投 DRV-2 VRM 票）
       ├─ [新增·M9] presenter=native_gop_presenter_create()      （仅 sheng-fb-gpu=1）
       └─ [新增·M0] if (enabled("pagani-gpu")) pagani_gpu_start();
            └─ DEV/package/PaganiGpuBoard.cpp：填 Adreno830Board，调用
                 adreno830_start(&board)                        A830/Adreno830Start.cpp
                   S0…S9 同步执行；S10 启动 DCVS 线程后返回
launchd → backboardd：IOServiceMatching("IOAcceleratorES") 找到 AGXAccelerator，按 MetalPluginName 载入插件
```

- `pagani_gpu_start` 在 Stage7Probe 里有弱默认定义 `extern "C" __attribute__((weak)) void pagani_gpu_start(void) {}`。静态内核链接不接受未定义的弱引用（`A830/NativeAdrenoFirmwareDefaults.cpp:2-5` 注释），所以必须是“弱定义 + 强定义覆盖”，强定义由设备仓库的 `build-pagani-gpu.py` 追加进 `libAdreno830.a`，经 `-force_load` 链入（`UX/scripts/build-xnu-4k-baseline.sh:633-639`）。
- 调用位置在 `startPeripherals` 之后：此时 IOSurface、显示 nub 都已就绪，触控的 DRV-2 投票已经完成，GPU 的 DRV-2 投票不会与之并发（4.3.4 另有共享锁）。
- `pagani-gpu-async=1` 时，`pagani_gpu_start` 只用 `kernel_thread_start` 起一个线程执行同样的 `adreno830_start` 后立即返回（sheng 做法，`SHENG/package/GpuPowerProbe.cpp:2035-2042`）；此模式下 backboardd 很可能先于 Metal 服务启动，只用于不需要 GPU 合成的内核调试。
- 同步模式的时间预算：正常路径估计 0.3–0.6 s【推断】（主要是 BO 池预留、PAS 三次 SMC、IOGPUFamily 载入）；每个等待都有上限（3.4 表），任一超时即停止并返回，不重试。

### 3.2 设备树：节点、属性与输入来源

GPU 节点不进 ADT。驱动在运行期不解析任何设备树；所有地址、中断、流 ID、档位都在构建期从 DTS 与钉选的 `live.dtb` 生成为 `DEV/package/PaganiGpuInputs.h`，由 `PaganiGpuBoard.cpp` 填入 `Adreno830Board`。

**(A) 由 DTS 生成**（`SOC/dts/sm8750.dtsi` + `DEV/dts/oneplus13t.dts`，节点保持 `status` 只在板级打开，不加 `unixnu,adt-name`）：

| 节点 / 属性 | 值 | 用途 | 出处 |
| --- | --- | --- | --- |
| `gpu@3d00000` `reg` | `kgsl_3d0_reg_memory` 0x3D00000/0x40000；`rscc` 0x3D50000/0x10000；`cx_dbgc` 0x3D61000/0x3000；`cx_misc` 0x3D9E000/0x2000 | GPU、RSCC、CX_MISC 窗口 | `SOC/dts/sm8750.dtsi:92-103`；`LDT:10293-10294` |
| `gpu@3d00000` `interrupts` | `<0 300 4>`（`kgsl_3d0_irq`，INTID 332，电平）、`<0 80 4>`（`cx_host_irq`，INTID 112） | GPU 中断、限频事件 | `LDT:10291-10292` |
| `gpu@3d00000` `qcom,chipid`、`qcom,gpu-model` | `0x44050001`、`"Adreno830v2"` | DeviceInfo.chip_id | `UniXNU-Pagani_SM8750/dts/oneplus13t.dts:20-23`（GPU 工作树缺这两行，M0 补上） |
| `gpu@3d00000` `iommus`（新增） | `<&gpu_smmu 0 0>, <&gpu_smmu 1 0>` | SID0（gfx3d_user）、SID1（gfx3d_lpac） | `LDT:33112-33124` |
| `gmu@3d37000` `reg` | `gmu` 0x3D37000/0x68000；`gmu_ao_blk_dec0` 0x3D40000/0x10000 | GMU 窗口 | `SOC/dts/sm8750.dtsi:104-111`；`LDT:33149-33150` |
| `gmu@3d37000` `interrupts` | `hfi` SPI 304（INTID 336）、`gmu` SPI 305（INTID 337），电平 | GMU 中断 | `LDT:33147-33148` |
| `gmu@3d37000` `iommus`（新增） | `<&gpu_smmu 5 0>` | SID5 | `LDT:33140` |
| `gpucc` `reg` | 0x3D90000/0x9800 | 时钟控制器 | `SOC/dts/sm8750.dtsi:113-120` |
| `gpu_smmu` `reg`、`#iommu-cells`、`dma-coherent` | 0x3DA0000/0x40000、2、有 | SMMU 窗口与一致性页表遍历 | `SOC/dts/sm8750.dtsi:121-127` |
| `gpu_smmu` `interrupts`（新增） | 全局 SPI 674（INTID 706）；CB0..CB3 依次 SPI 678..681（INTID 710..713），电平 | SMMU 故障 | `LDT:24149` |
| `qfprom: efuse@221c8000` + `gpu-speed-bin@138`（新增） | `reg=<0x138 0x2>`，`bits=<0 9>` | 速度档 | `LDT:17534-17546` |
| `gcc` `reg` | 0x100000/0x1F4200 | GCC | `SOC/dts/sm8750.dtsi:76-81` |
| `reserved-memory`（新增，`DEV/dts`） | `cmd-db@81c60000` 0x20000；`smem@81d00000` 0x200000；`gpu-microcode@9b09a000` 0x2000（no-map） | cmd-db、SMEM、ZAP 区 | `LDT:342`、`LDT:383`、`LDT:481-485` |
| `qcom_scm` 中断（只取数） | SPI 962（INTID 994），边沿 | TZ WAITQ | `LDT:51-57` |

**(B) 只存在于 `live.dtb`、由同一生成器取出**（UniXNU 的 DTS 没有对应绑定，属于板级策略）：`qcom,gpu-pwrlevel-bins` 6 组（频率、`qcom,level`、`qcom,bus-min/freq/max`、`qcom,acd-level`、`initial-pwrlevel`，`LDT:10346` 起）、`qcom,bus-table-ddr`（12 级 kBps，`LDT:10279`）、`qcom,bus-table-cnoc`、`qcom,gmu-freq-table`（`<500000000 0x40 650000000 0x80>`，`LDT:33142`）、`qcom,gmu-perf-ddr-bw`（`0x5caf6a`）、`qcom,actlr`（`<0 0x3ff 0x32b>`，`LDT:24150`）、`qcom,ubwc-mode=5`、`qcom,min-access-length=32`（`LDT:10285-10286`）、LLCC 6 个窗口（`LDT:7346-7349`）、AOSS QMP 消息区 `0xC300000/0x400`（`LDT:873-881`，M3 的可选步）、IPCC `0x406000/0x1000`（`LDT:828-835`）、tsens2 的 TM 窗口 `0xC22A000/0x1FF`（`LDT:26121-26128`，M11）。整份运行时 DTB 每次开机都会变（`chosen` 等节点），所以生成器不钉整份文件，而是把上述 GPU 相关节点规范化成 JSON 后钉它的 sha256（记在 `DEV/scripts/data/sm8750-gpu-inputs.json`）。注意：本机 `一加13T原厂备份/设备信息/运行中设备树.dtb`（1427077 字节，sha256 `13b8b74a8dba343b96fd34910858f66501ce7ab4eb9ce2eec1787f55fc2ffe60`，即 `LDT` 的来源）与设备仓库记录的作者抓取 `d00767f2…`（`DEV/proprietary-files.txt:5`）不是同一份文件；以本机这份为准，并在 M1 比对两者 GPU 子树是否一致。

**(C) 运行期只读**（必须在设备上取）：cmd-db 中 `gfx.lvl/cx.lvl/mx.lvl/gmxc.lvl/(gbx.lvl)` 的 ARC 地址与 vlvl 表、`SH0/MC0/ACV/CN0` 的 BCM 地址与 aux；SMEM socinfo（SoC id 618、feature_code、pcode）；速度档熔丝；SMMU ID 与当前路由；GMU 固件块头；GX/CX 上电状态。

**(D) 驱动内的代际常量**：Gen8 寄存器偏移（`docs/inputs/regs/`）、hwcg/protect/nonctxt/gbif 表（`A830HwInit.h`）、GMU VA 布局、HFI 消息号、GMEM 12 MiB 与 64 MiB 对齐、HBB 16、UBWC 5。

### 3.3 IOKit 服务与对象

| 对象 | 类 / 名字 | 父节点 | 关键属性 | 何时出现 |
| --- | --- | --- | --- | --- |
| 直通服务 | `NativeAdrenoGpuPublished`，`setName("NativeAdrenoGpu")` | `IOService::getPlatform()` | （新增）`Adreno830Status`：`OSData`，只在阶段切换与故障时刷新 | S9 |
| 加速器 | `AGXAccelerator : IOAcceleratorES`（实例 136 字节、168 个虚表项） | `IOService::getPlatform()` | `MetalPluginName=NativeAdrenoMetal`；`MetalPluginClassName=NativeAdrenoMetalDevice`；`NativeSandboxGrantProcess`=`NativeAdrenoSandboxGrantList`（`NativeAdrenoPublish.h`：backboardd、iconservicesagent、WidgetRenderer_Default、PosterBoard 与 14 个会建 Metal 设备的海报扩展） | S9（“GPU Metal registered”） |
| 用户客户端 | `AGXDeviceUserClient` | 由 `newUserClient` 建立 | 开类型 0x4147（GPU）、0x4148（只做交接） | backboardd 启动后 |
| 显示交接（M9） | `NativeGOPPresenter` | `IOService::getResourceService()`（不能挂在 display-subsystem 下：sheng int-075/077 SpringBoard 崩溃，`SHENG/package/LegacyDisplayProbe.cpp:556-564`） | — | late probe，GPU 启动前 |
| GOP 显示 | `NativeLegacyDisplay`（nub `gop-display`） | `/arm-io`（DT 平面） | 不变 | 已有 |

`publishMetal` 的顺序照 sheng（`SHENG/package/GpuPowerProbe.cpp:1217-1252`）：`OSKextLoadKextWithIdentifier("com.apple.iokit.IOGPUFamily")` → 取 `IOAcceleratorES` 元类并要求 `getClassSize()==136` → `OSMetaClass::preModLoad` → 调全局化的 `__GLOBAL__sub_I_NativeAdrenoAccelerator.cpp` → `postModLoad` → 要求 `native_adreno_accelerator_instance_size()==136` → `allocate`+`init` → 设三个属性 → `attach(getPlatform())` → `registerService()`。前提：13T 的 `[kernel.prelink] roots` 必须含 `com.apple.iokit.IOGPUFamily`（现在没有，见 7.3）。

### 3.4 阶段机

`adreno830_start()` 依次执行下列阶段。每个阶段开始时写状态块（`stage`、`state=running`、开始时间），结束时写耗时；任何一步失败写 `reason/step/ioreturn/reg/value` 后立即返回，**不重试、不回滚已做的硬件动作、不发布**（例外：S3 之后的失败会把已认领的流重新压成 FAULT，见 4.5.7）。`pagani-gpu-stop=N` 让编排器在 S_N 成功后停下（状态 `stopped`），用于逐段上机。

| 阶段 | 叠加层名字 | 主要动作 | 成功判据 | 等待上限 | 证据 |
| --- | --- | --- | --- | --- | --- |
| S0 | `GPU board inputs` | 校验板级结构与 4 个固件的长度/sha256；复制 cmd-db 快照并解析 ARC/BCM；只读 SMEM 分区表与全局分区，取 socinfo；读速度档熔丝；选档位表并预编码 HFI 表 | SoC id == 618；ARC `gfx/mx/cx` 与 BCM `SH0/MC0/ACV/CN0` 全部找到 | 无等待 | cmd-db、SMEM、qfprom 读取【实测·本机】 |
| S1 | `GPU power` | （可选，`pagani-gpu-qmp=1`）经 AOSS QMP 发 ACD 关闭消息，结果只记录（4.17）；DRV-2 投 `mx.lvl`、`cx.lvl`；`Sm8750GpuPowerOn`；读 `CM3_SYSRESET` 记录 | TCS 完成；上电返回 Ok | TCS 2000×1 ms；GDSC 2000 µs；PLL 1500 µs；RCG 500 µs；分支 500 µs | 【实测·本机】 |
| S2 | `GPU secure CX` | SCM `GPU_INIT_REGS`（每次开机一次）；DRV-2 投 CN0 与 DDR 启动档；读软熔丝 | `x0∈{0,-4}`；TCS 完成 | SCM 总 1 s（EBUSY 30 ms×20）；TCS 同上 | 【实测·本机】 |
| S3 | `GPU SMMU` | 只读预检；sGFSR 写回清除（复读只记录）；处理 sACR；CB0/CB2 先复位（`SCTLR=0`、清 FSR），再挂 710/712（可选）；预留 BO 池；GMU 上下文（CB2）与 GPU 上下文（CB0）建表、编程、路由；LPAC 路由；aperture SCM | 各 S2CR 读回类型正确；SCTLR 读回 0x167（bit12 可为 0）；sGFSR、FSR 只记录，不作判据 | TLBSYNC 10 ms | 序列【实测·同 SoC】；本机未实测 |
| S4 | `GPU firmware memory` | CX 计时器播种（每次开机一次，必须在装载 GMU 固件之前，4.4.2）；装 GMU 数据块到 DDR 区；清零 HFI/日志/持久区；拷 SQE、AQE；清环、memptrs、pwrup 页 | 长度与边界校验 | 无 | 软件操作 |
| S5 | `GPU GMU HFI` | GMU 冷启动 B1–B8；HFI 启动消息 M1–M9；主机 DDR 票按底线调整（默认底线等于启动档，不变）；OOB_GPU 置位并确认 GX 已上电；写 GMU 侧计数器；挂 GMU 中断（可选，失败只记录、改轮询） | `FW_INIT_RESULT&0x1FF==0x100`；`HFI_CTRL_STATUS.bit0`；每条 ACK 状态 0；OOB 应答；OOB 后 `GFX_PWR_CLK_STATUS[1:0]==0` | 冷启动 100 ms；HFI 就绪 100 ms；每条 HFI 1 s；OOB 100 ms；GX 确认 1 ms | 【源一致】 |
| S6 | `GPU command processor` | （M11 起）LLCC 切片 9/11 激活；Gen8 hw_init（4.8 第 1–17 步，含第 9a 步 `GBIF_SCACHE`）；SQE/AQE 基址；环；`CP_SQE_CNTL=1`；`CP_ME_INIT` | `CP_RB_RPTR==10`、memptrs[0]==10、空闲判据 | 2 s（100 µs 睡眠步进） | 【源一致】；annibale CP_INIT 走通【实测·同 SoC】 |
| S7 | `GPU ZAP secure exit` | PAS 13 三次 SMC；`CP_SET_SECURE_MODE 0`；重写 `RB_GC_GMEM_PROTECT`；补 pwrup 表；HWCG；OOB_GPU 清除 | 三次 SMC `x0==0 && x1==0`；空闲判据 | PAS 每次 SMC 1 s；空闲 2 s | 【源一致】 |
| S8 | `GPU memory fence` | 清 RBBM 中断状态后挂 GPU 中断 332（掩码已在 S6 设好；332 是唯一的中断硬依赖）；`CP_MEM_WRITE` 自检 + 带 IRQ 的栅栏；一致性自检 V4、V9a（只记录）；V9b/V9c 只在 `pagani-gpu-cohtest=1` 的独立镜像里跑（4.14.2）；计时器挂接 | scratch 读回 `0x08750830`；中断计数 +1；等待者被唤醒 | 100 ms | 332 接线做法【实测·sheng】 |
| S9 | `GPU Metal service` → `GPU Metal registered` | `native_adreno_publish`；`publishMetal` | 两个服务注册成功 | IOGPUFamily 载入无显式上限（同步 kext 载入） | 【实测·sheng】 |
| S10 | `GPU DCVS` → `GPU running` | 启动调频线程（M7 为固定档）；（M11 起）主机 DDR 底线随欠载调整 | 线程启动 | — | — |

### 3.5 板级接口 `Adreno830Board.h`

```c
/* SPDX-License-Identifier: MIT */
enum { Adreno830BoardMagic = 0x41383342 /* 'A83B' */, Adreno830BoardVersion = 1 };
struct Adreno830Window { uint64_t phys, bytes; };
struct Adreno830Level {            /* 一档 GPU 频率（升序；GMU 档 0 = 关，不在表内） */
    uint32_t khz, vlvl;             /* qcom,gpu-freq / qcom,level */
    uint8_t  bus_min, bus, bus_max; /* qcom,bus-min / bus-freq / bus-max */
    uint8_t  reserved;
    uint32_t acd;                   /* qcom,acd-level，0 = 无 */
};
struct Adreno830Firmware { const uint8_t *bytes; uint64_t length; uint8_t sha256[32]; };
struct Adreno830Board {
    uint32_t magic, version;
    Adreno830Window gpu, rscc, cx_dbgc, cx_misc, gmu, gmu_ao, gpucc, gcc, smmu, cmd_db, smem, zap, qfprom, rsc_drv2,
                    aoss_msgram, ipcc, tsens_tm;                        /* 后三个：M3 的 QMP 可选步与 M11 的温控 */
    uint32_t chip_id, soc_id;                        /* 0x44050001, 618 */
    uint32_t irq_gpu, irq_cx_host, irq_hfi, irq_gmu; /* 332, 112, 336, 337 */
    uint32_t irq_smmu_global, irq_smmu_cb[4];        /* 706, 710..713 */
    uint32_t irq_scm_waitq;                          /* 994 */
    uint16_t sid_gpu, sid_lpac, sid_gmu;             /* 0, 1, 5 */
    uint8_t  slot_gpu, slot_lpac, slot_gmu;          /* 首选 0, 3, 1 */
    uint8_t  cb_gpu, cb_gmu;                         /* 0, 2 */
    uint32_t smmu_actlr;                             /* 0x303 */
    uint32_t pool_mib;                               /* 512 */
    uint32_t qfprom_speedbin_offset, speedbin_bits;  /* 0x138, 9 */
    const Adreno830Level *levels[6]; uint8_t level_count[6]; uint8_t initial_level[6];
    uint32_t bin_speed[6], bin_sku_count[6], bin_sku[6][4];
    uint32_t common_level_count;                     /* 11：各 bin 共有的 ≤900 MHz 档 */
    const uint32_t *ddr_kbps; uint8_t ddr_levels;    /* 12 级 */
    uint32_t cnoc_levels[2];                         /* {0, 100} */
    uint32_t gmu_khz[2], gmu_vlvl[2];                /* {500000, 650000}, {0x40, 0x80} */
    uint32_t ubwc_mode, highest_bank_bit, min_access_length;
    Adreno830Firmware gmu_fw, sqe_fw, aqe_fw, zap_fw;
    uint32_t flags;                                  /* Adreno830BoardFlag*：启动参数解析结果 */
    struct Adreno830Knobs { uint32_t stop, status, async, rails, ddr_boot, ddr_floor, actlr, coherent,
                            dcvs, dcvs_min_mhz, dcvs_max_mhz, perf, llcc, speedbin, acd, hfi, diag,
                            qmp, cohtest, va; } knobs;
};
extern "C" IOReturn adreno830_start(const Adreno830Board *);
```

板级 `PaganiGpuBoard.cpp` 只做三件事：用 `PaganiGpuInputs.h` 的常量填结构；用 `PE_parse_boot_argn` 解析 3.6 的启动参数填 `knobs`；调用 `adreno830_start`。所有硬件动作都在驱动或平台层里完成。

### 3.6 启动参数

| 参数 | 默认 | 含义 |
| --- | --- | --- |
| `pagani-gpu=1` | 0 | 总开关（名字与作者 metal-68 一致） |
| `pagani-gpu-stop=N` | 不停 | S_N 成功后停下，不发布 |
| `pagani-gpu-status=0/1/2/3` | 1 | 叠加层 GPU 行：0 不画；1 一行；2 一行加三行诊断页；3 一行加用户态报告页（插件最近两条报告行与客户端摘要，第 8 节） |
| `pagani-diag`（系统开关，不是本驱动的旋钮） | 0 | 非 0 时写运行期诊断行 `gpu sec`、`gpu noslot`、`gpu va past 4 GiB`（8.8、6.2），还要诊断页和日志在跑；没有标志位，不进 `Adreno830KnobTable` |
| `pagani-gpu-async=1` | 0 | 后台线程启动（只用于内核调试） |
| `pagani-gpu-rails=0/1` | 1 | 1 = 主机 CX/MX 投 cmd-db 最高档（作者实测，默认）；0 = LOW_SVS（只用于 M11 单变量实验） |
| `pagani-gpu-ddr=L` | 11 | S2 起的主机 DDR 票档位（11 = 作者实测，默认；2 = 初始档 bus-min，M11 实验） |
| `pagani-gpu-ddr-floor=L` | 等于 `pagani-gpu-ddr` | 首个 `GX_BW_PERF_VOTE` 应答后保留的主机 DDR 底线（默认等于启动档，即不降）；0 = 只留保活最小票（SH0/MC0 x=1、y=1，ACV 关），任何取值都不发 valid=0 的 SH0/MC0/CN0 |
| `pagani-gpu-pool=MiB` | 512 | 写合并 BO 池 |
| `pagani-gpu-actlr=0xNNN` | 0x303 | GPU/GMU 上下文 ACTLR |
| `pagani-gpu-coherent=0/1/2` | 由自检决定 | 0 关；1 只让 GPU 只读的导入页走一致映射（需 V9a 通过）；2 再开放 CachedCoherent（需 cohtest 镜像里 V9b、V9c 都通过） |
| `pagani-gpu-dcvs=0/1` | 1 | 0 = 固定在 `pagani-gpu-perf` 档 |
| `pagani-gpu-dcvs-min=MHz`、`pagani-gpu-dcvs-max=MHz` | 160、734 | 调频上下限（开 ACD 前上限 734；ACD 开且未显式给出时上限是表顶，734 以上由 4.16 的持续规则放行，显式给出才是硬上限）；annibale 配置未显式给出 `pagani-gpu-dcvs-max` 时上限 342（第 11 节第 14 条） |
| `pagani-gpu-perf=G` | 初始档（2，222 MHz） | DCVS 关时的 GMU 档；M8–M10 的评估镜像固定 7（607 MHz、bus 6），M8 另留一轮 2 作对照 |
| `pagani-gpu-hfi=位掩码` | 0 | 只用于 S5 失败时做对照：bit0 = 依赖票改用 Linux/作者写法（高 4 位为 0）；bit1 = `START` 改到 OOB 之后（KGSL 非 hwsched 顺序）。默认是“依赖票 KGSL 写法 + START 在 OOB 之前” |
| `pagani-gpu-llcc=0/1` | 0（M11 验证后改 1） | LLCC 切片 9/11 |
| `pagani-gpu-speedbin=0/1` | 0 | 1 = 按熔丝选表并开放 900 MHz 以上档 |
| `pagani-gpu-acd=0/1` | 0 | ACD（P3） |
| `pagani-gpu-diag=1` | 0 | 失败时额外转储更多寄存器到状态块（不在热路径） |
| `pagani-gpu-va=2/4/16` | 16 | CB0 光圈 GiB 数（2026-10-31 默认从 4 改 16，首测判据见 PROGRESS；回退写 `pagani-gpu-va=4`，或把 `Adreno830KnobDefaults` 的默认改回 4）：4 = 2053 页表、12 个客户端窗口 1.5 GiB + 3×352 MiB + 8×124 MiB（2026-10-30 前为 1.5 GiB + 3×682 MiB）；16 = 34 位输入（须 IDR2.UBS ≥ 34 位：第一级上下文的输入上限是 UBS，套件编码时按它检查，IAS 是第二级的输入位数；SM8750 为 UBS 48、IAS 36）、8209 页表（32 MiB 连续）、32 个窗口 1.5 GiB + 3×512 MiB + 27×448 MiB + 最后一个约 512 MiB（`0x1FFFE000`，到 `0x3FFFFE000`），UBS 不足 34 位或 32 MiB 页表分不到时自动退回 4（DeviceInfo 与发布外壳按实际光圈；S3 的追踪注记 `gpu aperture GiB, table pages` 在两种配置下都记下实际建成的 GiB 与页表页数，十六进制，如 `10 2011` / `4 805`）；2 = 现有 1027 页表与旧的 4 窗口表（回退用） |
| `pagani-gpu-cohtest=1` | 0 | 只用于独立的一致性测试镜像：S8 末尾、发布之前追加 V9b/V9c（4.14.2） |
| `pagani-gpu-qmp=0/1` | 0（M3 单变量通过后改 1） | S1 开头经 AOSS QMP 告诉 AOP `{class: gpu, res: acd, val: 0}`（4.17 P3 行） |
| `pagani-gpu-profile=0/1` | 1 | 启动配置（`A830Profile.h`）：1 = annibale，按 REDMI K90 在同 SoC、同一 GMU 固件上走通的序列与取值（与本文的差异见第 11 节第 14 条）；0 = 本文的设计值。两种配置都保留 13T 实测的上电与 PLL、主机 CX/MX 与 DDR 票、SCM `GPU_INIT_REGS`。S0 检查范围后立即生效，第 0 行末尾显示 `p1`/`p0` |
| `pagani-gpu-hwsched=0` | 0 | 保留位：本版本只有主机写环（RB），非 0 时 S0 停在步骤 0x49，原因码 24 |
| `pagani-gpu-trace=0/1/2` | 1（启动调试期间） | 启动期寄存器追踪（8.7）：1 = S0–S10、重跑阶段体的电源操作与恢复期间，每次寄存器访问与 SMC 在执行之前写一行进 BootProgress 的 ramoops 日志，轮询与顺序块计数压缩；2 = 每次访问都写，不压缩；0 = 关。ramoops 日志未开（`pagani-ramoops=0`）时什么都不写；大于 2 时 S0 停在步骤 0x49 |
| `pagani-gpu-maxlevel=n` | 0 | 性能开关（2026-10-30，`Adreno830Board.h`）：0 = annibale 首轮的 160/222/342 MHz 三档；n（1–16）= annibale 配置也把设计配置从 DT 生成的整张档位表交给 GMU（`A830VotesFullTable`：同样的档位、bus-min/bus/bus-max 映射、ARC 与依赖票、ACD 表，DDR 带宽行仍是 annibale 的 x = 0，首票仍是 160 MHz `{1, 1, 2}`），DCVS 上限改为 GMU 档 n（超出表即最高档），仍受 ACD 未开时 734 MHz、显式 `pagani-gpu-dcvs-max` 与 70/60 °C 温控约束；设计配置的表本来就是整张，n 只作上限。大于 16 时 S0 停在步骤 0x49 |
| `pagani-gpu-irq=0/1` | 0 | 性能开关：1 = annibale 配置在 S8 像设计配置一样挂 332，但只放开环栅栏中断（`A830IrqConfig::own_mask` = `A830_INT_MASK_FENCE_ONLY`，挂接后与每次 `restore()` 由 `A830Irq` 自己写），栅栏等待由中断唤醒、回收由中断触发；挂接被拒、或自检里栅栏中断没到 / 没唤醒等待者，就本次开机退回首轮的轮询栅栏（`A830Irq::fallBack`，`irq_off` 位 0），S8 不因此失败；这种模式下栅栏位清不掉也算无进展，风暴保护照常生效。设计配置无影响（332 本来就是硬依赖）。RUN 行末尾显示 `m<n>`、`i1`（`i0` = 已给但退回轮询） |
| `pagani-gpu-ttarget=C` | 95 | 设计配置温控闭环的目标温度（整数 °C，2026-10-30，4.16）：上限在它之上且不在下降时每 500 ms 降一档，低于它 5 °C 并保持 2 s 才升一档；annibale 配置仍用 70/60 °C 规则，不受影响。超出 50–90 时 S0 停在步骤 0x49（原因码 1，reg 为旋钮号 28，expect 为越过的界） |
| `pagani-gpu-kopt=<位>` | 63 | 内核延迟改动的开关（2026-10-31，`NativeAdrenoKopt.h`，5.4 与 8.8），每个改动一位，默认全开，`0` = 改动之前的行为（计量行也逐字节相同）；上机对比时才写，平时不占命令行：位 0（1）池 BO 放开 Memory 锁之后再清零；位 1（2）池外 BO 只在 64–128 KiB 试连续内存，失败后 5 s 内同尺寸及更大的不再试；位 2（4）表事务不重复 preflight 的别名检查；位 3（8）`NativeAdrenoMaintainOwnedPages` 每 64 页放开一次抢占；位 4（16）调用计量；位 5（32）VM 时钟（发布时接 `native_adreno_vm_now_us`，VM 的分阶段和分类耗时才有数）。超出 0–63 时 S0 停在步骤 0x49（原因码 1，reg 为旋钮号 30，expect 为 63）。这是 `Adreno830Board::flags` 放得下的最后一个旋钮（位 31 是池标志） |
| `sheng-gpu-addon=1` | — | 共享代码固定的名字：Data 挂载插件（`zzy-native-gpu-addon-mount.patch`） |
| `sheng-gpu-metal-opens=0` | 40 | 共享代码固定的名字：backboardd 打开次数不限（`A830/NativeAdrenoAccelerator.cpp:30-31`） |
| `sheng-fb-gpu=1` | 0 | 共享代码固定的名字：建立 GPU 扫描输出交接（M9） |

不再使用：`sheng-gpu-power`、`sheng-gpu-selftest`、`sheng-gpu-pool`、`sheng-gpu-zap`、`sheng-gpu-coherent`、`sheng-gpu-wc`、`sheng-gpu-perf`、`sheng-gpu-dcvs*` 及旧服务路径的参数。


---

## 4. 各模块的硬件序列

本节是实现的直接依据。寄存器名用 KGSL `gen8_reg.h`/Mesa A8XX 的写法；凡标“只记录”的回读，不一致时只写进状态块，不作为失败条件。

### 4.1 板级输入（S0）

#### 4.1.1 cmd-db（`platform/sm8750/Sm8750Rpmh.c`）

- 映射 `0x81C60000`/`0x20000`（Device），**逐个对齐 32 位读**复制成内核快照（Device 内存不能用 `memcpy`，做法同 `SHENG/package/GpuPowerProbe.cpp:1953-1960`），之后只解析快照。快照 128 KiB，放在静态存储区（或 S0 一次性 `IOMallocData` 分配、从不释放），绝不能是栈上的局部变量：XNU 内核线程栈只有 16 KiB。【实测·本机】（作者修补版与触控的 `rpmh_vrm_address` 都读过）。
- 头：`+4` magic = `0x0C0330DB`；从 `+8` 起最多 8 个 slave 头，每个 16 字节 `{u16 slv_id, u16 header_offset, u16 data_offset, u16 cnt, u16 version, u16 reserved[3]}`，`slv_id==0` 结束；数据区从字节 144 开始。
- 条目 24 字节：`id[8]`（不足补 0）、`u32 priority[2]`、`u32 addr`（+16）、`u16 len`（+20）、`u16 offset`（+22）；aux 位置 = `144 + data_offset + offset`。
- slave 类型：ARC=3、BCM=5（VRM=4，现有 `SOC/kits/Sm8750Platform/Rpmh.h:6-30` 只认 VRM）；`addr` 的 bits[18:16] 必须等于类型。
- 需要的条目：ARC `gfx.lvl`、`mx.lvl`、`cx.lvl`、`gmxc.lvl`（DT 有 `VDD_GFX_MXC_LEVEL`，应存在）、`gbx.lvl`（可选）；BCM `SH0`、`MC0`、`ACV`、`CN0`。
- ARC aux 为 `u16 vlvl[]`，下标即 hlvl；有效个数按 KGSL `adreno_rpmh_arc_cmds`：从第 1 项起遇到“前一项非 0、本项为 0”即止；除首项外全 0 视为哑资源，`gmxc` 为哑资源时当作不存在（`KG16/adreno_gen8_rpmh.c:205-209`）。
- BCM aux 8 字节：`le32 unit, le16 width, u8 vcd, u8 reserved`。

#### 4.1.2 SMEM socinfo（`platform/sm8750/Sm8750SocInfo.c`）

- 映射 `0x81D00000`/`0x200000`，但**只允许访问** `[0x1FF000,0x200000)`（分区表）与 `[0x1000,0x9E000)`（全局分区）【实测·本机：其他页同步外部中止】。作者修补版也读过页 0（`S/smemcopy.s` 从 0 复制到 0x9E000），我们的实现不依赖页 0。所有读取经一个“越界即返回错误”的访问函数，主机测试用越界即中止的模型覆盖。
- 分区表 `0x1FF000`：magic `0x434F5424`（`$TOC`）、version 1、条目数 ≤ 84；条目 48 字节，取 `host0==host1==0xFFFE` 的全局分区：`offset`（要求 8 对齐、≤0x1FF000）、`size`（≥0x20）、`cacheline`（2 的幂、≤4096）。（字段与校验照作者 `DEC/fffffff00bfd0568__Sm8750SocInfoGpuCode.c`。）
- 分区头：magic `0x54525024`（`$PRT`）、`host0|host1<<16 == 0xFFFEFFFE`、`size` 与表项一致、`offset_free_uncached≥0x20`；从 `+0x20` 起遍历非缓存条目（16 字节头：`u16 canary 0xA5A5, u16 item, u32 size, u16 padding_data, u16 padding_hdr, u32 reserved`），找 item 137。
- socinfo：`+0x04 id` 必须是 618（`0x26A`）【实测·本机】；`fmt≥0x10` 时 `+0xB0 feature_code`、`+0xB4 pcode(u16)`【推断：按 Linux `struct socinfo` 字段顺序推得】。`soc_code`：量产 `feature_code` 落在 0xF1..0xFF 之外时等于 `feature_code`，否则为 `(pcode<<16)|feature_code`（`KG16/adreno.c:941-964`）。

#### 4.1.3 速度档（`platform/sm8750/Sm8750SocInfo.c` 内）

- 映射 `0x221C8000`/`0x1000`，读 `+0x138` 的 32 位，取 `bits[8:0]`（`LDT:17542-17546`）。作者修补版在“GPU board inputs”阶段执行过这一读并进入了“GPU power”【实测·本机，间接】；取值未知，S0 记录到状态块。
- 已知映射（perf 研究，`LDT:10351-11195`）：`0xBE`→最高 900 MHz；`0xDD`→1050；`0xE8`→1100；`0xF2`→1150；sku 1/2→1100；sku 3（兜底组，含 sku 0）→1200。匹配规则按 KGSL `adreno_of_get_pwrlevels`（`KG16/adreno.c:773-845`）：顺序找第一个匹配；只有 speed-bin 的组要求相等；有 sku-codes 的组任一 sku 为 0 或等于 soc_code 即匹配。

#### 4.1.4 档位表（默认：各 bin 共有的 11 档）

逐档比对 6 组 DT 表，≤900 MHz 的 11 档频率、RPMh 电压级、ACD 值完全相同（唯一差别：bin `0xBE` 的 900 MHz 总线档为 11，其余为 10，本表取 10）。GMU 档 `g` = 11 − DT 序号；GMU 档 0 固定为“关”（vote 0、dep `0xFFFFFFFF`、0 Hz）。

| GMU 档 g | DT 序号 | MHz | vlvl（RPMh） | bus-min / bus / bus-max | ACD |
| --- | --- | --- | --- | --- | --- |
| 1 | 10 | 160 | 0x32 LOW_SVS_D3 | 2 / 2 / 2 | — |
| 2（初始） | 9 | 222 | 0x34 LOW_SVS_D2 | 2 / 2 / 2 | — |
| 3 | 8 | 342 | 0x38 LOW_SVS_D1 | 2 / 3 / 3 | — |
| 4 | 7 | 389 | 0x3C LOW_SVS_D0 | 2 / 4 / 6 | 0xC8285FFD |
| 5 | 6 | 443 | 0x40 LOW_SVS | 2 / 4 / 6 | 0xC02E5FFD |
| 6 | 5 | 525 | 0x50 LOW_SVS_L1 | 2 / 4 / 6 | 0xC02E5FFD |
| 7 | 4 | 607 | 0x80 SVS | 4 / 6 / 8 | 0xA82E5FFD |
| 8 | 3 | 660 | 0x90 SVS_L0 | 4 / 6 / 8 | 0x882C5FFD |
| 9 | 2 | 734 | 0xC0 SVS_L1 | 6 / 8 / 10 | 0xA82A5FFD |
| 10 | 1 | 832 | 0xE0 SVS_L2 | 7 / 10 / 10 | 0x882A5FFD |
| 11 | 0 | 900 | 0x100 NOM | 7 / 10 / 10 | 0x882A5FFD |

`pagani-gpu-speedbin=1` 时，按 4.1.3 匹配到的组追加 967/1050/1100/1150/1200 MHz 档（频率与 vlvl 都相同的重复档只留一个）。DDR 12 级（kBps，`LDT:10279`）：0、781250、2136718、5285156、6074218、6671875、8171875、10687500、12449218、14398437、16500000、18597656（约 0/200/547/1353/1555/1708/2092/2736/3187/3686/4224/4761 MHz）；CNOC 两级 `<0 100>`；GMU 自身两档 500 MHz/0x40、650 MHz/0x80。

### 4.2 电源与时钟（S1，`platform/sm8750/Sm8750GpuPower.c`）

#### 4.2.1 上电顺序【实测·本机】

`A830/platform/sm8750/Sm8750GpuPower.c` 已按作者 M6 的实测行为写好。主机测试 `A830/tests/test_sm8750_power.c` 的寄存器模型带依赖关系：源 → RCG → 分支、CX GDSC → 分支、`gcc_gpu_cfg_ahb` → 任何 GPU_CC 访问、RCG 锁存期间不得停源。它覆盖“PLL 不锁”“GPLL0 未锁或频率不对”“BCR 未释放”“0x9008 写不进”“gemnoc 硬件门控”“NOC_FSM”，并对每一次读、写、延时逐一注入失败。`tests/mutate_sm8750_power.py` 用 26 个变异检验这份测试，全部被杀死才算通过。步骤（GCC 偏移相对 `0x100000`，GPUCC 偏移相对 `0x3D90000`）：

1. **前置**：调用方（编排器）已经经 DRV-2 投好 `mx.lvl`、`cx.lvl`（4.3.3）。头文件注释里“GFX/MX(C) rail votes”的说法要改正：GX 轨归 GMU 管，主机只投 CX/MX。
2. **GCC AHB**：`0x71004`（`gcc_gpu_cfg_ahb_clk`，关键常开）先读；bit0 为 0 时才写，写后在 GCC 上回读一次并延时 10 µs，然后才访问 GPU_CC。同一外设上的回读保证写已完成，10 µs 与作者实测路径一致。**不查状态**（带 hwcg，CLK_OFF 不可作判据，`gcc-sm8750.c:3241`）。
3. **复位**：读 `CX_BCR 0x907C`、`GMU_BCR 0x9314`，bit0=1 时写 0 并延时 1 µs。冷启动实测为 0。
4. **CX GDSC**：`GDSCR 0x9080` 掩码 `0x00FFF007` 写 `0x00228000`（EN_REST_WAIT=2、EN_FEW_WAIT=2、CLK_DIS_WAIT=8，SW_COLLAPSE=0）→ 延时 1 µs → 轮询 `GDS_HW_CTRL 0x9094` bit31（PWR_ON）上限 2000 µs → 延时 1 µs → `GDSCR |= 0x800`（RETAIN_FF_ENABLE）。不校验 GDSCR 读回。
5. **GPLL0**：`0x52000 |= BIT15|BIT16`（`gcc_gpu_gpll0_clk_src`/`_div_clk_src`，HALT_DELAY），延时 10 µs；要求 `0x0` bit31（LOCK_DET）=1、`0x10[15:0]==0x1F`、`0x14[15:0]==0x4000`（600 MHz），否则 `UnexpectedState`。GPLL0 本体由系统投票，不碰。
6. **GPU_CC_PLL0**（VCO 1000 MHz、EVEN 500 MHz）：“已开”的判定与 Linux `trion_pll_is_enabled` 相同（OPMODE&RUN 且 OUTCTRL）。已开且频率、输出位、锁定都一致则跳过；已开但有任一项不符返回 `UnexpectedState`，trace 指向第一个不符的寄存器（比 Linux 严，有意为之：别的频率意味着别的主人）。未开（包括锁定超时留下的“RUN 且无输出”）先 `MODE&=~OUTCTRL`、`OPMODE=STANDBY`，再写 `L 0x10=0x00440034`、`ALPHA 0x14=0x1555`、`CONFIG_CTL 0x20=0x19660387`、`CONFIG_CTL_U 0x24=0x098060A0`、`CONFIG_CTL_U1 0x28=0xB416CB20`、`USER_CTL 0x18=0x00000400`、`USER_CTL_U 0x1C=0x2`，再 `MODE|=RESET_N(BIT2)`、`OPMODE=1`、轮询 `MODE` bit31 上限 1500 µs、`USER_CTL|=0x7`、`MODE|=OUTCTRL(BIT0)`。配置寄存器不做整字回读。
7. **RCG**（`clk_rcg2_shared` 流程）：先 GMU `CMD 0x9318`/`CFG 0x931C` 写 `0x201`（源 2 = PLL0_OUT_EVEN，/1 = 500 MHz），再 hub `CMD 0x93EC`/`CFG 0x93F0` 写 `0x505`（源 5 = GPLL0，/3 = 200 MHz）。每个：`CMD|=ROOT_EN(BIT1)` → 轮询 `CMD` bit31==0 上限 500 µs → `CFG=(CFG&~0x10371F)|值` → `CMD|=UPDATE` → 轮询 UPDATE 清零上限 500 µs → `CMD&=~ROOT_EN`。（R1 已纠正：0x9318 才是 GMU 时钟。）
8. **关键 CBCR**：`0x9004、0x9008、0x9064、0x90CC、0x93A4、0x93A8` 只置 bit0，**绝不读回判定**（`0x9008` 置位后读到 `0x80000000`）【实测·本机】（关键时钟列表见 `S/ref/gpucc-sm8750.c:423-430`）。
9. **分支**：`gpu_cc_ahb 0x90BC`（HALT_DELAY，置位后延时 10 µs 不查）；`0x9010 demet`、`0x90D4 cx_gmu`、`0x90E4 cxo`、`0x93E8 hub_aon`、`0x90E8 hub_cx_int`、`0x7000 hlos1_vote_gpu_smmu` 置位后等待：判据为 `CLK_OFF(bit31)==0`，或 `HW_CTL(bit1)==1` 直接放行；**新增** `bits[30:28]==2`（NOC_FSM=ON）也放行（R2 power-tail，与 Linux `clk_branch2_check_halt` 一致）；上限 500 µs、1 µs 步进；GCC `0x71150 ddrss_gpu_axi`（HALT_SKIP）只置位；GCC `0x71010 gemnoc_gfx`（HALT_VOTED+hwcg）置位后按同一判据，读到 `0x80000003` 时因 HW_CTL 放行【实测·本机】。
10. **CM3 复位门**：读 `CM3_SYSRESET`（`0x3D7E000`）只记录，期望 1；不作为 S1 的成功条件（B1 会显式写 1）。

与作者 M6 实测路径的差异（2026-10-29 审查后）：`0x71004` 只在关闭时才写，写后回读并等 10 µs；关键 CBCR 不再附加作者的通用 10 µs；分支等待增加 NOC_FSM=ON 放行；PLL0 未开时先停再配；电压票由调用方投递；RCG 先 GMU 后 hub 的顺序与作者不同。这些差异都来自 Linux 源码，未经本机实测，列入真机清单。

DT 中 GMU 节点列出的 6 个时钟（`gpucc 4/6/0/20`、`gcc 13/34`）与 SMMU 的 `gpucc 17` 都在上面；`demet` 与 `hub_aon` 不在 DT 列表里（`S/wf2/power-tail/dl/qcom,gpucc-sun.h`），首版沿用实测行为开启，稳定后做“不开”的单变量实验（1.3 第 14 条）。

#### 4.2.2 跨步依赖

步骤 2→4 必须在访问任何 GPUCC 分支之前完成；GPLL0 先于 hub RCG；PLL0 先于 GMU RCG；第 9 步完成之前不得访问 GMU、CX_MISC、SMMU、GBIF 任何寄存器（否则同步外部中止）。

#### 4.2.3 断电（P1 阶段才用，`Sm8750GpuPowerOff`，【源一致·未实测】）

D1 逆序清分支 bit0（不清关键 CBCR 与 `gcc_gpu_cfg_ahb`；只有 BRANCH_HALT 的 `cxo 0x90E4` 等 CLK_OFF=1，上限 200 µs，失败只记入 `trace.warnings`；HALT_VOTED 与 HALT_DELAY 分支等 10 µs；`0x71150` 不等）→ D1b 两个共享 RCG 按 `clk_rcg2_shared_disable` 停靠到 XO，先 hub 后 GMU：保存 CFG → `ROOT_EN` → 等根开（超时忽略，同 Linux）→ `CFG=0` → `UPDATE` 等锁存 500 µs（超时只记 warnings）→ 释放 `ROOT_EN` → 写回保存的 CFG（不锁存，下次上电的 RCG 步骤再锁存）。停靠必须在 D2、D3 停源之前：RCG 若仍锁存在 PLL0 或 GPLL0 上，停源后它和下游（包括未建模的常开 CBCR）都会失去时钟 → D2 PLL0：`MODE&=~OUTCTRL`、`USER_CTL&=~0x7`、`OPMODE=0`（保留 RESET_N 与配置）→ D3 清 `0x52000` 的 bit15/16 → D4 `GDSCR|=SW_COLLAPSE`，延时 500 µs（GDSC 为 VOTABLE，Linux 不轮询），`CFG_GDSCR 0x9084` bit15（POWER_DOWN_COMPLETE）为 0 只记 warnings（GMU 可能仍在投 CX）→ D5 主机票一律不动：CN0、SH0、MC0 是保活资源（HLOS 永不对它们发 valid=0，4.3.2），CX/MX 票还可能承载 UFS、MDP 的需要且无法读回开机遗留值（4.3.3），断电不撤销任何主机票。

### 4.3 供电轨与 RPMh 投票（S1/S2/S5，`platform/sm8750/Sm8750Rpmh.c`）

#### 4.3.1 apps DRV-2 ACTIVE TCS 协议【实测·本机】

与 `SOC/kits/Sm8750Platform/Rpmh.h:44-105` 的 `sm8750_rpmh_vrm_enable` 及作者 `vote()` 同构，推广为“写 N 条命令”：

1. `DRV2+0x00` bits[23:16] 必须为 3；`DRV2+0x0C`：`NTCS=bits[17:12]`、`NCMD=bits[31:27]` 都在 3..16。
2. TCS 块基址 `DRV2+0xD00`：`IRQ_ENABLE +0x00`、`IRQ_STATUS +0x04`、`IRQ_CLEAR +0x08`。TCS n（0..2 为 ACTIVE，DT `tcs-config` 首项 `<2 3>`）基址 `0xD00+0x2A0·n`：`WAIT_FOR_CMPL +0x20`、`CONTROL +0x24`、`STATUS +0x28`（1=空闲）、`CMD_ENABLE +0x2C`；命令 i：`MSGID +0x34+24i`、`ADDR +0x38+24i`、`DATA +0x3C+24i`、`CMD_STATUS +0x40+24i`。
3. 选空闲槽：`CONTROL/ENABLE/WAIT` 全 0、`IRQ_STATUS` 对应位 0、`STATUS==1`。
4. 清本槽 `IRQ_ENABLE` 位；逐条写 `MSGID=0x00010108`（写、要求应答、4 字节）、`ADDR`、`DATA`；`WAIT_FOR_CMPL=CMD_ENABLE=(1<<N)-1`（只对 BCM 的 commit 位等待时按 4.3.2 取掩码）；`CONTROL=0x00010000` 后写 `0x01010000` 触发。
5. 每 1 ms 轮询一次、最多 2000 次：`STATUS==1` 且每条 `CMD_STATUS` 含 `0x00010100`。
6. 收尾：`CONTROL` 写 `0x10000` 再写 0，`ENABLE=0`、`WAIT=0`，`IRQ_CLEAR=1<<n`，恢复原 `IRQ_ENABLE` 位。超时的槽保持原状、不再复用（证据留给诊断）。
7. 只写 ACTIVE 集，不碰 sleep/wake 集。

#### 4.3.2 编码

- **ARC**：`DATA = hlvl`，地址为 cmd-db 中该 ARC 的 `addr`。CX/MX 主机票：默认取表中最高档（作者实测，4.3.3）；`pagani-gpu-rails=0` 时取首个 `vlvl ≥ 0x40`（LOW_SVS）的 hlvl。升压时同一请求里先 `mx.lvl` 后 `cx.lvl`，降压反之。
- **BCM**：`BCM_TCS_CMD(commit,valid,x,y) = commit<<30 | valid<<29 | (x&0x3FFF)<<14 | (y&0x3FFF)`。commit 取“最后一个 BCM 或 `vcd[i]≠vcd[i+1]`”，`WAIT_FOR_CMPL` 取 commit 位。下面两种用途语义不同，代码里分成两个编码函数，不能混用：
  - **GMU 带宽表（HFI M2，由 GMU 经 GPU 自己的 RSC 投）**：照 Linux a8xx。非固定 BCM（`SH0` buswidth 16、`MC0` buswidth 4，`L/…a8xx_catalog.c:1558-1567`）：`peak = kBps×width/buswidth`，`y = peak×1000/unit` 夹到 [1,0x3FFF]，`x=y`（KGSL 不启用 AB 时 x=0），`kBps=0` 时 valid=0。`ACV` 为固定 BCM：kBps>0 时 `BCM_TCS_CMD(commit,1,0, kBps≥10687500 ? BIT(2) : 0)`，否则 `BCM_TCS_CMD(commit,0,0,0)`；BIT(2) 是 GPU 的 perfmode 位，只属于这张表。
  - **主机票（apps DRV-2，HLOS 身份）**：照 Linux HLOS 互连驱动（`S/wf2/secure-cx/dl_linux/drivers_interconnect_qcom_bcm-voter.c`、`…_sm8750.c`）。`SH0/MC0` 用同一公式算 y、`x=y`，但它们是保活资源（`keepalive`，`sm8750.c:1087-1090,1135-1138`）：x、y 任何时候都不小于 1，绝不发 valid=0（`bcm-voter.c:107-113`）。`ACV` 用 HLOS 的 `enable_mask=BIT(0)`（`sm8750.c:1024-1029`）：主机 DDR 票非零时 `BCM_TCS_CMD(commit,1,0,BIT(0))`，只有底线为 0 时才 `BCM_TCS_CMD(commit,0,0,0)`（ACV 不是保活资源）；主机票里永远不放 BIT(2)。作者在 apps DRV 上投的是 BIT(2)，R2 secure-cx-08 指出这只是“归属不对”，不影响 TCS 完成，所以改用 BIT(0) 不改变已实测的协议与地址。
- **CN0**：主机票固定为 `0x60000001`（x=0、y=enable_mask，作者同值，机制实测），S2 投一次后不再改写。CN0 是保活资源（`sm8750.c:1037-1040`，`bcm-voter.c:72-77`），`0x40000000`（valid=0）只出现在 GMU 带宽表的 CNOC 第 0 级，主机票里永不出现。

#### 4.3.3 主机票策略（本驱动持有，全部 ACTIVE）

| 时机 | 票 | 默认值（= 作者实测，ACV 位除外） | M11 单变量实验 |
| --- | --- | --- | --- |
| S1 开始 | `mx.lvl`、`cx.lvl` | 表中最高档（`pagani-gpu-rails=1`），运行期不撤销 | `pagani-gpu-rails=0`：LOW_SVS 对应 hlvl |
| S2（SCM 之后） | `CN0` | `0x60000001`，之后不再改写 | — |
| S2（SCM 之后） | `SH0, MC0, ACV` | 第 11 级（18597656 kBps），ACV=BIT(0) | `pagani-gpu-ddr=2`：初始档 bus-min（2136718 kBps） |
| S5（首个 `GX_BW_PERF_VOTE` 应答后） | `SH0, MC0, ACV` | 底线 = 启动档：默认不变，不发 TCS | `pagani-gpu-ddr-floor=3`：第 3 级（5285156 kBps），ACV=BIT(0)；`=0`：SH0/MC0 x=1、y=1，ACV valid=0 |
| 断电（P1） | 全部 | 不撤销（4.2.3 D5） | — |

理由：

- apps DRV-2 上每个资源只有一张票，主机写入是替换而不是叠加；开机遗留的票（UEFI/ABL 投的）读不回来：annibale 用 ACTIVE 写 CX=3 后读回仍为 0（`AW/docs/progress/2026-10-25-gpu-resume-review.md` §18、§22）。UniXNU 里 UFS（13T 的根卷就在 UFS，`DEV/board.toml` 的 `native-ufs-root=1`）、MDP 都没有自己的 CX/DDR 票，降低主机票可能连带压低它们依赖的电压或带宽。所以在拿到可信读数之前，默认保持作者在本机实测过的票值。
- 保持 HLOS 语义：Linux 的 HLOS 互连驱动对 SH0/MC0/CN0 设 `keepalive`，无人投票时也保持 x=1、y=1（`bcm-voter.c:72-77,107-113`），所以本驱动的主机票同样永不 valid=0；ACV 的 HLOS 位是 BIT(0)，BIT(2) 只属于 GPU 经 GMU 投的带宽表（4.3.2）。
- KGSL 首启在 GMU 启动期投 bus-min、HFI 之后 `icc_set_bw(0,0)`（`KG16/adreno_gen8_gmu.c:1883-1905`），前提是安卓上其他 HLOS 驱动各自持票；UniXNU 不具备这个前提。
- M11 的降档实验每次只改一个参数，验收必须同时比较三项：UFS 吞吐（开机到锁屏的耗时与 NativeUfs 诊断计数）、CPU 回退拷贝的帧时间、MDP 欠载计数（5.3.6，需要只读 DPU 探针），三项都不变差才改默认值。底线低于启动档之后，欠载计数增长时临时升到第 6 级（5.3.6）。

#### 4.3.4 DRV-2 互斥

DRV-2 同时被触控（`PD/drivers/PaganiInput/PaganiInput.cpp:158-180`）使用。M0 在 `SOC/kits/Sm8750Platform/Rpmh.h` 增加一个仅头文件的共享锁：`__attribute__((weak)) volatile uint32_t sm8750_rpmh_drv2_owner;`（多个编译单元的弱定义由链接器合并为一个），配 `sm8750_rpmh_acquire()/release()`（原子交换 + 每次 10 µs 让步，上限 2 s）。`Sm8750Rpmh.c` 与 `PaganiInput.cpp` 的投票都包在锁内。启动期两者天然串行（3.1），锁用于以后运行期的 DDR 底线调整与 slumber。

### 4.4 SCM 与 CX_MISC 一次性设置（S2/S3/S7）

#### 4.4.1 SMC 调用层（`platform/sm8750/Sm8750Scm.c`）

- 约定：SMC64 标准调用，`x0 = 0x42000000|(svc<<8)|cmd`，`x1 = arginfo`（低 4 位为参数个数，往上每 2 位一个类型：VAL=0、RO=1、RW=2、BUFVAL=3），`x2..x5` 为参数，`SMC #0`；返回 `x0` 状态、`x1..x3` 结果。所有 SMC 用一把互斥锁串行；SMC 期间不关中断。
- 续调（`L/drivers_firmware_qcom_qcom_scm-smc.c:40-152`）：`x0==1`（INTERRUPTED）→ 以 `x0=1`、`x6=上次返回的 x6` 续调；`x0==2`（WAITQ_SLEEP）→ `wq_ctx=x1, smc_call_ctx=x2`，等 TZ waitq 中断（INTID 994，边沿；中断里快调用 `GET_WQ_CTX 0xC2002403` 取 `x1=wq_ctx, x2=flags, x3=more`，唤醒等待者），醒后发 `RESUME 0x42002402`（arginfo 1，`x2=smc_call_ctx`）；中断没挂上时每 1 ms 试一次 `GET_WQ_CTX`，总上限 1 s；`x0==-12`（EBUSY）→ 睡 30 ms 重试，最多 20 次；其他非零为失败。
- 可用性：`0x42000601`（arginfo 1，`x2=0x02000000|(svc<<8)|cmd`），返回 `x1==1`。用于探测 `0x2801`、`0x0C1B`、`0x0207`，结果记录到状态块。

| 调用 | x0 | arginfo | 参数 | 成功判据 | 时机 |
| --- | --- | --- | --- | --- | --- |
| `GPU_INIT_REGS` | `0x42002801` | 1 | `x2 = 0x9`（ALWAYS_EN\|TSENSE_EN） | `x0∈{0,-4}`，**不看 x1**（作者要求 x1==0 比 Linux/KGSL 严，R2 secure-cx） | S2，每次开机一次【实测·本机】 |
| `CP_SMMU_APERTURE` | `0x42000C1B` | 4 | `x2=0xFFFF0000\|cb`（cb=0），`x3=x4=x5=0xFFFFFFFF` | `x0==0`；失败只记录（只影响以后的按进程页表） | S3 末尾 |
| `PAS_INIT_IMAGE` | `0x42000201` | `0x82` | `x2=13`，`x3=元数据物理地址` | `x0==0 && x1==0` | S7 |
| `PAS_MEM_SETUP` | `0x42000202` | 3 | `x2=13`，`x3=0x9B09A000`，`x4=0x1000` | 同上 | S7 |
| `PAS_AUTH_AND_RESET` | `0x42000205` | 1 | `x2=13` | 同上 | S7 |

BCL（BIT1）、CLX（BIT2）先不请求（KGSL gen8_0_1 请求 `0xF`，`K/adreno_gen8.c:1582-1612`，需要配套 HFI 表）。

#### 4.4.2 CX_MISC（物理 `0x3D9E000` 起）

| 时机 | 寄存器 | 动作 | 状态 |
| --- | --- | --- | --- |
| S2 SCM 之后 | `SW_FUSE_VALUE 0x3D9F000` | 读：bit0 FASTBLEND、bit1 LPAC、bit2 RAYTRACING（非零也证明 TZ 执行了 INIT_REGS，R2） | 只记录 |
| S2 | `SW_FUSE_FREQ_LIMIT_STATUS 0x3D9F014` | 读 `[8:0]` 与 bit24 | 只记录，单位待核对 |
| S2 | `CX_MISC_TCM_RET_CNTL 0x3D9E0E4` | 只读不写（A830 由 AOP 管，Linux 只对 a7xx 写） | 只记录 |
| S4 第一步（M3 起） | `AO_COUNTER_LO/HI 0x3D9E200/204` | 关中断测偏差后写入 `CNTVCT+skew`，GPU 计数与 `mach_absolute_time` 同源（`K/adreno_gen8.c:738-789`）。每次开机只播种一次，必须在装载 GMU 固件、放开 CM3 之前：KGSL 首启在 `gen8_gmu_load_fw` 之前调用（`KG16/adreno_gen8_gmu.c:1856-1858`），并用 `CX_TIMER_INITIALIZED` 标志保证只做一次（`KG16/adreno_gen8.c:745-747`）。GMU 运行中绝不改写；只有 CX 掉过电（P1 完整断电）后的唤醒，才在重新装载固件之前重播一次 | 【源一致】 |
| M13 | `CX_AHB_{AON,GMU,CP,VBIF_SMMU,HOST}_CNTL 0x3D9E040..050` | 各写 `0x1B12`（AHB 访问挂住时先得到 AHB 错误，`K/adreno_gen8.c:1276-1291`） | 【源一致】 |
| 可选 | `INT_CLEAR_CMD 0x3D9E0C4`=全 1，`INT_0_MASK 0x3D9E0CC=~BIT31` | 只开 GPU_CC 限频事件（INTID 112） | 只记录 |

### 4.5 GPU SMMU（S3，`A830Smmu.cpp`）

#### 4.5.1 本机已知与同 SoC 事实

- 基址 `0x3DA0000`/`0x40000`；`dma-coherent`；`qcom,use-3-lvl-tables`；`#iommu-cells=2`；依赖 CX GDSC、`gpu_cc_hlos1_vote_gpu_smmu_clk`（0x7000）与 TCU 带宽票（`LDT:24135-24152`）。S1 已打开这些时钟，S2 已投 DDR。
- 【实测·同 SoC】（annibale `AW/docs/progress/2026-10-25-gpu-preflight-pass.md:24-30`、hummer `HL:668-690`）：IDR0=`0x4C017E18`（S1TS、SMS、CTTW=1、NUMSMRG=24、无硬件 ATOS）；IDR1=`0x40000015`（4K 页、NUMPAGENDXB=4 即全局区 32 页、NUMCB=21、NUMS2CB=0）；IDR2=`0x5511`（IAS=OAS=36 位，UBS=48 位）；sCR0=`0x002E0406`（CLIENTPD=0、GFRE/GFIE=1、USFCFG=1）；开机 24 个 SMR 全 0、24 个 S2CR 全 `0x03000018`（translate→CB24、PRIVCFG=3，因 SMR 无效而不生效）；CB 寄存器开机为随机值、每次冷启动不同；SCTLR 写 `0x1167` 读回 `0x167`（ASIDPNE 不保留）；TCR2 写 `0x38011` 读回 `0x38071`（TBI 只读为 1）；ACTLR 写 `0x303` 读回 `0x303`；FSR 读到 `0x400` 是格式字段不是故障（有效掩码 `0xC00001FE`）；GPU 运行后 CB0 的 TCR.EPD1 会被清零（推测是 SQE 经 aperture 所写）。
- 【实测·同 SoC】写 `S2CR1=0x00000001`（CB1、SHCFG=0、当时 SMR mask 误用 0x400）触发同步外部中止 `ESR=0x96000010`；改为 CB2、SHCFG=NSH、mask 0 后写 `0x302` 成功（`AW/docs/progress/...gpu-initialization.md:53,65-67`）。三个变量同时改，原因未拆分，所以本设计同时避开 CB1、SHCFG=0 与非零 mask。
- PKX110 自己的 apps SMMU 记录：写 sCR0 被 Gunyah 忽略、BYPASS 被改成 FAULT（R2 smmu）。所以全局空间由 hypervisor 仿真。
- 作者在本机的失败（事实 4）最可能的原因（smmu-cp 与 R2 一致）：① kit `ArmSmmu4KAttachStreams` 要求目标 S2CR 精确等于 `0x00020000`（`UX/kits/ArmSmmu4K/ArmSmmu4KStreams.c:43`），而开机值是 `0x03000018`，必然返回 Busy；② ACTLR 写 `0x32B` 后要求原样读回，却从未清 `sACR.CACHE_LOCK`；③ GMU 槽写死为 2。本设计全部绕开。

#### 4.5.2 写法约束

- 所有 SMMU 访问走非内联的 MMIO 辅助函数，内联汇编约束用 `"r"` 不用 `"rZ"`，写 0 也先放进通用寄存器（hypervisor 仿真不接受 WZR 源，Linux `QCOM_DUMMY_VAL` 注释，`L/drivers_iommu_arm_arm-smmu_arm-smmu.c:44-51`）。
- 访问宽度按寄存器区分（主机测试的寄存器模型对下列偏移断言访问宽度）：
  - 配置类 64 位寄存器（TTBR0、TTBR1）只在 `SCTLR.M=0` 时写，可以拆成低、高两次 32 位写（`AK/adreno8_smmu.c:950-960`）；Linux 对 AArch64 格式用一次 `writeq`（`L/drivers_iommu_arm_arm-smmu_arm-smmu.c:636-641`），两种都可以。
  - 失效类 64 位寄存器 `TLBIVA(+0x600)`、`TLBIVAA(+0x608)`、`TLBIVAL(+0x620)`、`TLBIVAAL(+0x628)` 必须用一条 64 位存储（`str x`，源寄存器不能是 `xzr`）：写入本身就触发失效，拆成两次时第一次低 32 位写就会以高位（ASID）为 0 发出失效，真正 ASID 的条目留在 TLB 里，GPU 会继续访问已释放的页。Linux 用 `arm_smmu_cb_writeq`（`arm-smmu.c:303,322`），套件 `UX/kits/ArmSmmu4K/ArmSmmu4KProgram.c:207-211` 调 `io->write64`，所以 `A830Smmu` 交给套件的 `write64` 必须是单条 64 位访问。
  - `TLBIASID(+0x610)`、`TLBIALL(+0x618)`、`TLBSYNC(+0x7F0)`、`SCTLR`、`FSR` 等是 32 位寄存器，用 32 位写（Linux `arm_smmu_cb_write`，`arm-smmu.c:265-266`）。
- 每次配置写后 `dsb sy` 再回读。
- 地址：GR0 `0x3DA0000`；GR1 `0x3DA1000`（`CBAR(n)=+4n`、`CBFRSYNRA(n)=+0x400+4n`、`CBA2R(n)=+0x800+4n`）；`SMR(n)=0x3DA0800+4n`；`S2CR(n)=0x3DA0C00+4n`；`CBn = 0x3DA0000 + (pages+n)×0x1000`，`pages = 2<<IDR1[30:28]` → CB0 `0x3DC0000`、CB2 `0x3DC2000`。

#### 4.5.3 预检（只读；结果全部写入状态块诊断页）

1. 读 `IDR0/1/2`（`+0x20/+0x24/+0x28`）。要求 IDR0 bit30（S1TS）与 bit27（SMS）为 1；IDR1 bit31=0（4K 页）、`NUMCB=IDR1[7:0]≥4`；IDR2 bit12（4K 粒度）=1、`OAS=IDR2[7:4]≥1`。`output_bits` = OAS 编码对应位数（`{32,36,40,42,44,48}`），上限 40（`A830/NativeAdrenoGpuContext.cpp:23-28`）→ SM8750 为 36。第一级上下文的输入位数以 `UBS=IDR2[11:8]` 为上限（套件 `ArmSmmu4KEncodeContext` 按它检查；`IAS=IDR2[3:0]` 是第二级输入位数，与本驱动无关）→ SM8750 为 48，`pagani-gpu-va=16` 的 34 位输入据此判断。
2. 读 `sCR0 +0x00`：`CLIENTPD(bit0)=1` 时拒绝（所有流会旁路）；`USFCFG(bit10)=0` 时记录警告。**不写 sCR0，不做全局复位**。
3. 读 `sACR +0x10`：`CACHE_LOCK(bit26)=1` 时写回清零后的值（`L/…arm-smmu-impl.c` 的 `arm_mmu500_reset` 做法），回读只记录。
4. 读 `sGFSR +0x48`：非零时读 `sGFAR +0x40/+0x44`、`sGFSYNR0/1/2 +0x50/+0x54/+0x58` 记录，再把读到的值写回清除（同 Linux `arm_smmu_device_reset`，`L/drivers_iommu_arm_arm-smmu_arm-smmu.c:1663-1665`，只写不校验），复读值也写入状态块。**sGFSR 不作任何阶段的判据，也不作任何步骤的前置条件**：全局寄存器空间由 Gunyah 仿真（4.5.1），写回可能被忽略；它是粘滞位，残值不说明本驱动出了问题。
5. 扫描 `SMR[0..NUMSMRG-1]`：若已有有效项（bit31）覆盖 SID 0/1/5（`((smr^sid)&0x7FFF&~mask)==0`）则拒绝启动；若有任何有效 SMR 的 S2CR 以 translate 类型指向 CB0 或 CB2（`S2CR.TYPE==0` 且 `CBNDX∈{0,2}`）也拒绝启动：这两个 bank 在 4.5.4 开头就要复位，必须先确认没有别人在用（annibale 的预检同样检查目标 context，`AW/docs/progress/2026-10-25-gpu-preflight-pass.md` §3）。选槽：板级首选 {GPU 0, LPAC 3, GMU 1}，首选槽 SMR 有效时顺延到最低的 SMR 无效槽（跳过 2，留给 TZ 的安全流），结果记录。**不判断无效槽的 S2CR 取值**。
6. 读 `CB0..CB3` 的 `SCTLR/ACTLR/TCR/TTBR0/FSR` 记录（不要求 SCTLR.M=0）。

#### 4.5.4 建立一个上下文（GMU：CB2/槽 1/SID5，S3 前半；GPU：CB0/槽 0/SID0，S3 后半）

**先复位、后挂中断**：两个上下文编程之前，先对 CB2、CB0 各做一次复位：`SCTLR=0`（回读须为 0，这是功能位）→ `FSR=0xC00001FE`（写 1 清除，复读只记录），然后才挂 710/712（4.12.2）。开机时 CB 寄存器是随机值（4.5.1；annibale 读到 CB3 的 SCTLR=`0x96cc51a`，`AW/docs/progress/2026-10-25-gpu-preflight-pass.md` §3），随机的 CFIE 加随机的 FSR 会在 SPI 使能的瞬间变成一次“故障”，甚至电平风暴；annibale 也是先写 `SCTLR=0` 再写其余 CB 寄存器（`AK/adreno8_smmu.c:940`）。两个 bank 都由 4.5.3 第 5 步确认无人使用。

| 步 | 寄存器 | 值 | 回读判据 |
| --- | --- | --- | --- |
| 1 | `S2CR[slot]` | `0x00020300`（FAULT\|SHCFG=NSH） | `(v&0x30000)==0x20000` |
| 2 | `SMR[slot]` | `0x80000000\|SID`（mask 0） | 相等 |
| 3 | `CBn+0x00 SCTLR` | 0 | 为 0（进入时不要求为 0） |
| 4 | `CBn+0x04 ACTLR` | `0x303`（`pagani-gpu-actlr` 可改） | 只记录 |
| 5 | `CBA2R(n)` / `CBAR(n)` | `0x1`（VA64）/ `0x0001F300`（S1 翻译 S2 旁路、MEMATTR=WB、BPSHCFG=NSH、VMID 0） | 相等 |
| 6 | `CBn+0x10 TCR2` | `0x00038011`（SEP=7、AS=16 位 ASID、PASIZE=36 位） | 容忍 bit5/6 被置 1 |
| 7 | `CBn+0x30 TCR`（先于 TTBR） | CB0：`0xB5A03520`（T0SZ=T1SZ=32、IRGN/ORGN=WBWA、SH=IS、TG0=4K、TG1=4K、EPD1=1）；CB2：`0x80803520` | 相等（以后巡检容忍 EPD1 被清） |
| 8 | `CBn+0x20/+0x24 TTBR0` | `(ASID<<48)\|root_pa`（CB0 ASID 1，CB2 ASID 3；root 4K 对齐且 <2^36） | 相等 |
| 9 | `CBn+0x28/+0x2C TTBR1` | CB0：`(1<<48)\|empty_root_pa`（全零 4K 表页，保护 EPD1 被清后的上半区访问）；CB2：0 | 相等 |
| 10 | `CBn+0x38/+0x3C MAIR0/1` | 低 16 位 `0xFF44`（idx0 NC、idx1 WBRWA，与 kit `ArmSmmu4KResult.mair` 一致）/ 0 | 相等 |
| 11 | `CBn+0x58 FSR` | `0xC00001FE`（写 1 清零） | — |
| 12 | `CBn+0x618 TLBIALL`、`+0x7F0 TLBSYNC`，轮询 `+0x7F4` bit0==0 | `0xFFFFFFFF` | 10 ms 上限 |
| 13 | `CBn+0x00 SCTLR` | `0x00001167`（M\|TRE\|AFE\|CFRE\|CFIE\|HUPCF\|ASIDPNE；终止模式） | `0x167`，bit12 可为 0 |
| 14 | 全量回读 | CBAR、CBA2R、TCR2、TCR、TTBR0/1、MAIR0/1、SCTLR | 按上面的容忍规则 |
| 15 | `S2CR[slot]` | `0x00000300\|cb`（translate\|NSH） | `(v&0x030303FF)==0x300\|cb`（GMU 读回 `0x302`，GPU 读回 `0x300`） |
| 16 | `FSR` | — | 只记录，不作判据（此时 CB2 的 CM3 还在复位、CB0 的 GX 还没上电，正常应为 0；非零时写回清除并记入诊断页） |

CB0 上半区的实现：先试 `ArmSmmu4KBuildUpper` 以零个范围建空表（套件说明允许零叶子的拓扑）；若套件拒绝，`A830Smmu` 在 `ArmSmmu4KEncodeContext` 产出的镜像上直接补 T1SZ=32、IRGN1/ORGN1=WBWA、SH1=IS、TG1=4K，EPD1 保持 1，TTBR1 指向一页全零的表。

顺序约束：GMU 上下文必须在 GMU 固件装载前完成（GMU 代码在 DDR，经 SID5 取指，4.7.1）；GPU 上下文必须在写 `CP_SQE_CNTL=1` 之前完成；710/712 在 CB0/CB2 复位之后、第一次置 `SCTLR.CFIE`（第 13 步）之前挂好，挂接失败只记录（4.12.1）；706 首版不挂（4.12.2）。第 13 步置位后才把该 bank 标为“已启用”，此前到来的上下文中断只清 FSR、不计故障（4.12.3）。

#### 4.5.5 LPAC 流（SID1→槽 3→CB0）

CB0 生效后、`CP_SQE_CNTL=1` 之前：`S2CR[3]=0x20300` → `SMR[3]=0x80000001` → `S2CR[3]=0x300` → 回读（`AK/adreno8_smmu.c:1004-1035`）。漏掉的后果【实测·同 SoC】：BR/BV 的 SQE 卡在等 THREAD_SYNC（PC 0xE3/0x4A），RPTR 不动，更早的轮次甚至整机复位（`AW/docs/analysis/2026-10-24-a830-command-startup.md:169-171`）。LPAC 用 SID1 取微码，由非上下文寄存器 `CP_SMMU_STREAM_ID_LPAC(0x814)=0x101` 选择。

#### 4.5.6 aperture 授权

CB0 生效后调用 `CP_SMMU_APERTURE`（4.4.1）。只影响 CP 的 `CP_SMMU_TABLE_UPDATE`（按进程页表，M14），失败只记录。结果（`A830Smmu::apertureState`：0 未调用、1 授予、2 拒绝；2026-10-31 起，M14 P0）记在三处：S3 诊断页第 2 行末尾 ` AP<状态>`（`aux[3]` 第 56–58 位，8.4）；S3 末尾一条追踪注记 `g3.40 note gpu aperture <状态> x0=<x0>`（每次开机一条，x0 为十六进制；之前日志里的 `gpu aperture GiB, table pages 4 805` 是 GPU 上下文的 VA 光圈大小与页表页数，不是这次 SMC 的结果，SMC 本身的 SCM 追踪行早被事件环冲掉）；运行期计量行末尾 ` ap<状态>`（8.8，诊断页模式下每分钟至多一行，所以测试镜像的日志里总有最近的一份）。

#### 4.5.7 运行期与关闭

- 运行期不做寄存器级健康检查；上下文故障由 710/712 报告（4.12.3）。这两个中断没挂上或被风暴保护关掉时，由 DCVS 线程每秒读一次 CB0/CB2 的 FSR 与 sGFSR（只记录并写回清除，不进提交路径）。可选低频巡检（≥1 s 一次，只在诊断模式）只比对 S2CR/SMR 路由、SCTLR.M 与 TTBR0。
- 关闭/恢复：确认 CP 停止、GBIF 已 halt → `S2CR[slot]=0x20300`（保留 SMR 有效，流继续被挡）→ `SCTLR=0` → `TLBIALL`+同步 → 之后才释放页表。任何一步失败就保留全部内存直到冷重启（`AK/adreno8_smmu.c:1037-1060`）。S3 之后的启动失败也执行前两步，避免留下一个指向我们页表的活路由。

### 4.6 页表、GPU 地址空间与 PTE（`NativeAdrenoGpuContext`、`NativeAdrenoVm`）

#### 4.6.1 格式

- AArch64 stage 1、4 KiB 粒度、只用 L3 叶子（`ArmSmmu4KBuild`），CB2 输入 32 位（T0SZ=32，3 级）；CB0 默认（`pagani-gpu-va=16`）输入 34 位（设计配置 T0SZ=30，annibale 仍写 T0SZ 25，同一个从 L1 开始的根页，只用前 16 项），前提是 IDR2.UBS ≥ 34 位（第一级输入上限，SM8750 为 48；2026-10-31 前误读 IAS，SM8750 上两者都够，结论相同），否则退回 4 GiB（输入 32 位，`pagani-gpu-va=4` 同）。页表页物理地址 <2^36（XNU 内存位于 `0x9_8000_0000–0xB_0000_0000`）。
- 页表页按现有实现在开机时整块预留（Live 拓扑）：CB0 默认 16 GiB、8209 页（32 MiB 连续，比 4 GiB 多 24 MiB，`MaxLeaves` 为 `1<<22`，别名暂存固定 `1<<19` 项，与光圈无关）；`pagani-gpu-va=4` 时 4 GiB、2053 页；`pagani-gpu-va=2` 时为现有的 2 GiB、1027 页（`NativeAdrenoGpuContext.h:18,32` 的 `GpuApertureBytes`、`TablePages` 改为由板级参数决定；`NativeAdrenoSmmuContext.h:34` 的 `MaxLeaves=1<<19` 只够 2 GiB，同时改为 `1<<20`，别名检查的暂存数组随之加倍）。因 IDR0.CTTW=1、DT `dma-coherent`、TCR 内共享 WBWA，页表遍历一致：首版仍按 sheng 做法清洗 PTE；M10 在 V4 自检通过后改为 CPU 可缓存页表页、写 PTE 后只做 `dsb ishst`。
- XNU 页可能是 16 KiB，叶子仍按 4 KiB 填写（annibale 已验证）。

#### 4.6.2 CB0 地址布局（这里按 4 GiB 光圈写，客户端窗口下面的固定地址在各光圈相同；默认 16 GiB 光圈时客户端窗口止于 `0x3FFFF_E000`，`pagani-gpu-va=2` 时止于 `0x7FFF_FFFF`）

| GPU VA | 内容 | 大小 | PTE |
| --- | --- | --- | --- |
| `0x0000_0000–0x000F_FFFF` | 不映射（空指针保护） | — | — |
| `0x0010_0000` | SQE 微码（文件去掉首 4 字节，0x1B510 字节） | `0x1C000` | 只读、可执行、非特权可读 |
| `0x0018_0000` | AQE 微码（0x62F8 字节） | `0x7000` | 同上 |
| `0x0020_0000` | ring（32 KiB，按大小对齐） | `0x8000` | 特权只读 NC |
| `0x0030_0000` | memptrs（`msm_rbmemptrs` 布局） | `0x1000` | 特权读写 NC |
| `0x0030_1000` | pwrup 寄存器表 | `0x1000` | 特权读写 NC |
| `0x0030_2000` | scratch | `0x1000` | 特权读写 NC |
| `0x0400_0000–0x04BF_FFFF` | GMEM 窗口（`UCHE_CCHE_GC_GMEM_RANGE_MIN=0x4000000`，12 MiB），**严禁映射** | — | — |
| `0x2000_0000–0xFFFF_FFFF` | 客户端窗口（第 6 节；2 GiB 模式为 `0x2000_0000–0x7FFF_FFFF`） | 3.5 GiB | 用户 |
| `0x1_FFFF_FFFF_F000` | UCHE trap 地址，超出 32 位输入，天然不可映射 | — | — |

内核缓冲改为特权页（现有 `NativeAdrenoGpuContext.cpp:79` 用的是 3 或 5，没有 `Privileged`）：CP 取环、写 RPTR 影子、执行环内包都带 APRIV（BR `0x6F`、其余管线 `0x0F`）；用户 IB 是非特权访问，碰不到环与 memptrs。

#### 4.6.3 PTE 编码（4K 页描述符）

字段：bits[1:0]=0b11；AttrIndx[4:2]；AP[1]=bit6（EL0 可访问）；AP[2]=bit7（只读）；SH[9:8]（2 外共享、3 内共享）；AF=bit10；nG=bit11（必须置 1）；PXN/XN=bit53/54。

| 用途 | 值 | 对应 kit 权限 |
| --- | --- | --- |
| 用户 RW、NC（WC 缓冲、GPU 写的导入页） | `0x0060_0000_0000_0E43` | R\|W，wb=0，sh=2 |
| 用户只读 NC | `0x0060_0000_0000_0EC3` | R |
| 内核特权 RW NC（memptrs、pwrup、scratch） | `0x0060_0000_0000_0E03` | R\|W\|Priv |
| 内核特权只读 NC（ring） | `0x0060_0000_0000_0E83` | R\|Priv |
| 微码（只读、可执行、非特权可读） | `0x0000_0000_0000_0EC3` | R\|X |
| 一致性只读（WB+IS） | `0x0060_0000_0000_0FC7` | R，wb=1，sh=3 |
| 一致性读写（WB+IS） | `0x0060_0000_0000_0F47` | R\|W，wb=1，sh=3 |

硬件规则：AArch64 禁止以特权身份从 EL0 可写页取指，所以 GMU 代码页必须是特权页（annibale 曾因此报 `FSR=0x8, FAR=0x4000`，`AW/docs/progress/...gpu-initialization.md:79-83`）。

#### 4.6.4 TLB 维护

- 映射（无效→有效）：写 PTE 后 `dsb ishst`，不发 TLBI；SMMU 不缓存无效翻译。改写有效 PTE 走 break-before-make。
- 解除映射：先确认该 BO 最后使用的 seqno 已完成（读 memptrs，不等全局空闲）→ 清 PTE、`dsb ishst` → ≤512 页逐页 `TLBIVAL(+0x620，值=((iova>>12)&0xFFF_FFFF_FFFF)|ASID<<48)`（单条 64 位写，4.5.2），>512 页 `TLBIASID(+0x610)`（32 位写，值为 ASID）→ 每批一次 `TLBSYNC` → 同步完成后才释放物理页。
- 同步预算：先自旋 10 次，再按 1/2/4… µs 退避，总 10 ms 未完成判为 SMMU 故障（停止提交、保留内存、不重试）。
- 删除 build 68 的“卡 50 ms 发 TLBIALL”。


### 4.7 GMU：固件、地址空间、冷启动与 HFI（S4/S5，`A830GmuFirmware`、`A830Gmu`、`A830Hfi`、`A830Votes`）

A830 与 A740 的关键差别（power-gmu 研究，【源一致】）：

- 主机**不编程** RSCC 序列器与 PDC（由 AOP 配置；Linux `a6xx_gmu_rpmh_init` 对 a8xx 直接返回，`L/a6xx_gmu.c:692-694`；KGSL gen8 没有这类代码），也不写 `CX_MISC_TCM_RET_CNTL`。sheng 的 `SM8550/kits/Sm8550Platform/ShengGmu.c:113-122` 两件事都做了，移植时必须删掉。
- GMU 主体代码在 DDR：ITCM 里只有 0x1EC 字节引导桩（初始 SP `0x10007F80`，复位向量 `0x105`），之后从 GMU VA `0x4000` 起经 SID5/CB2 取指。CB2 是 GMU 冷启动的硬前置。

#### 4.7.1 固件格式与装载（`A830GmuFirmware`）

- 文件：`gen80000_gmu.bin`，88896 字节，sha256 `1ad8175e6cd01ea76c64d20b83600038c52dd29844b48d57b11b769b038863a5`；core `0x50010019`（5.1.25）、pwr `0x50000112`、HFI `0x20070001`（2.7.1）。与作者内嵌版本相同。`gen80600_gmu.bin` 属于 gen8_6_0（另一颗 GPU），不用。固件只在本地，不入库。
- 格式：连续的块，每块头 16 字节 `{u32 addr, u32 size, u32 type, u32 value}` 后跟 `size` 字节数据。`size==0` 为元数据块且都在数据块之前：type 1 `PREALLOC_REQ`、type 7 `PREALLOC_PERSIST_REQ` 表示 GMU VA `addr` 处需要 `value` 字节后备存储；数据块的 type/value 是填充值 `0xEFBEADDE`，忽略（KGSL `gen8_gmu_parse_fw`，`KG16/adreno_gen8_gmu.c:1267-1326`）。
- 本机文件（`docs/inputs/power-gmu/gmufw.py` 实际解析结果）：元数据 #0–#10：ITCM `0x0/0x1EC`、CACHE `0x4000/0x15824`、ITCM `0x200/0x37A8`、CACHE `0x100000/0x1B878`、`0x200000/0x1EAA0`、`0x300000/0x24548`、DTCM `0x10004000/0x2DE8`、`0x10007000/0xF84`、`0x10007FDC/0x18`、PERSIST `0x60000000/0x1000`、`0x60001000/0x1000`；数据 #11 ITCM `0x0`（0x1EC 字节，前 4 字 `0x10007F80 0x105 0x145 0x159`）、#12 CACHE `0x4000`（0x15824 字节，主代码）、#13 DTCM `0x10007F98`（0x28 字节全 0）、#14 DTCM `0x10007FDC`（版本字）；共 15 块，结束于 `0x15B40`。
- 去向：`addr∈[0,0x4000)` → 逐 dword 写 `CM3_ITCM_START 0x3D6D000 + addr`；`addr∈[0x10004000,0x10008000)` → 写 `CM3_DTCM_START 0x3D71000 + (addr−0x10004000)`；其他 → 拷到映射在该 GMU VA 的 DDR 区；落不进任何区域的块报错。每次 GMU 启动都重装（TCM 随 CX 掉电丢失），装完 `dsb sy`。

#### 4.7.2 GMU 地址空间（CB2，经 SID5）

| GMU VA | 用途 | 大小 | 属性 |
| --- | --- | --- | --- |
| `0x4000–0x1A000`、`0x100000–0x11C000`、`0x200000–0x21F000`、`0x300000–0x325000` | CACHE（按 PREALLOC 向上取整到页，共 `0x76000` = 472 KiB），首次分配清零 | 4 段 | 读写、可执行、特权；GMU 侧 NC；CPU 侧 WC |
| `0x60000000` | 持久区（覆盖两块 PERSIST），只在首次分配时清零 | 16 KiB | 读写、特权 |
| `0x60004000` | 日志，`GENERAL_8=0x60004003` | 16 KiB | 同上 |
| `0x60008000` | HFI 共享内存 | 16 KiB | 同上 |

物理上不要求连续。作者与 Linux 的 `0xFFC000` 物理连续 icache 没有必要（KGSL 逐块按 PREALLOC 预留，`gen8_gmu_process_prealloc`）。`0x60400000` 调试区固件不要求，不分配。

#### 4.7.3 冷启动握手 B1–B8（`A830Gmu::coldBoot`，【源一致·未实测】）

前置：S1 上电完成；S2 SCM 已做（仅首启）；CB2 已映射 4.7.2 全部区域；主机票已发；HFI 队列已在 GMU 复位态初始化（4.7.5）。

| 步 | 动作（物理地址） | 值 / 判据 | 上限 |
| --- | --- | --- | --- |
| B1 | `CM3_SYSRESET 0x3D7E000` | 写 1，`dsb`（冷启动本应为 1，防御性写入） | — |
| B2 | `GMU2HOST_INTR_CLR 0x3D7E644`=`0xFFFFFFFF`；`AO_HOST_INTERRUPT_CLR 0x3D8EC10`=`0xFFFFFFFF`；`GMU2HOST_INTR_MASK 0x3D7E64C`=`0xFF7F7FF9`（开放 bit1 SIDEMSGQ、bit2 DBGQ、bit15 CM3_FAULT[KGSL]、bit23 CM3_FAULT[Linux]）；`AO_HOST_INTERRUPT_MASK 0x3D8EC18`=`0xFFFFFFD6`（开放 bit0 WDOG_BITE、bit3 FENCE_ERR、bit5 HOST_AHB_BUS_ERR） | 清除前的原值先记录到状态块（作者漏了清除，残留位让首次故障检查直接失败，R1/R2）；开放的 4 个 GMU2HOST 位在 336 的处理函数里无条件清除（4.12.3） | — |
| B3 | RSCC 唤醒，**仅在上次做过 RSCC 睡眠时**：`RSCC_CONTROL_REQ 0x3D8EC1C`=BIT1 → 轮询 `RSCC_CONTROL_ACK 0x3D8EC20` bit1 → 轮询 `RSC_SEQ_BUSY_DRV0 0x3D50404`==0 → `RSCC_CONTROL_REQ=0` | 冷启动跳过 | 各 100 ms |
| B4 | 按 4.7.1 装载固件，`dsb sy`；回读 ITCM 前 4 字 | 应为 `0x10007F80 0x105 0x145 0x159`（只作通路检查） | — |
| B5 | 读 `0x3D74FE0`（core）、`0x3D74FF0`（HFI） | `0x50010019`、`0x20070001`；core > 0x20010004 → 初始化判据用 `(v&0x1FF)==0x100`；HFI ≥ 0x20060001 → 表格式；小版本 7 > 4 → FEATURE/GET/SET 应答为 v5 格式 | — |
| B6 | 寄存器配置（KGSL `gen8_gmu_register_config`，`KG16/adreno_gen8_gmu.c:861-942`）：`RPMH_POWER_STATE 0x3D7DFA4=0xF`；`CX_FALNEXT_INTF 0x3D7DFB4=1`；`CX_FAL_INTF 0x3D7DFB0=1`；`CM3_FW_INIT_RESULT 0x3D7E070=0`；`CM3_BOOT_CONFIG 0x3D7E004=2`；`HFI_QTBL_ADDR 0x3D7E614=0x60008000`；`HFI_QTBL_INFO 0x3D7E610=1`；`AHB_FENCE_RANGE_0 0x3D8EC44=0x80C808A0`；`CM3_CFG 0x3D7E0B4=0x4052`（bit9 NMI 必须为 0）；`GBIF_QSB_SIDE0..3 0x3D0F00C..018=0x00071E20`；`GBIF_CX_CONFIG 0x3D0F000=0x20023000`；`MRC_GBIF_QOS_CTRL 0x3D7D42C=0x33`；`GBIF_HALT 0x3D0F114=BIT3`；`GENERAL_9 0x3D7E724=0`（之后为保存的日志写指针）；`GENERAL_10 0x3D7E728=0x08000100`；`GENERAL_8 0x3D7E720=0x60004003`；`SYS_BUS_CONFIG 0x3D7D03C=1`；`ICACHE_CONFIG 0x3D7D000=1`；`DCACHE_CONFIG 0x3D7D004=1`；`dsb sy` | 全部不回读（作者的 11 个回读去掉，只在诊断模式记录） | — |
| B7 | `CM3_SYSRESET=0`，`dsb`；以 100 µs 睡眠步进轮询 `CM3_FW_INIT_RESULT` | `(v&0x1FF)==0x100` 成功；`(v&0xE00)≠0` 立即失败（NMI/复位中）；每轮同时看 `GMU2HOST_INTR_INFO 0x3D7E648` bit15（CM3 故障，致命）与 AO 状态 `0x3D8EC14` bit0（WDOG_BITE，致命）；INFO bit23、AO bit3（FENCE_ERR）、bit5（HOST_AHB_BUS_ERR）只记录：KGSL 对这两个 AO 位只打日志（`KG16/adreno_gen8_gmu.c:1733-1750`），R2 cp-zap `cp-health-strict` 也把作者的 `&0x29` 判为误杀 | 100 ms |
| B8 | `HFI_CTRL_INIT 0x3D7E618=1`；轮询 `HFI_CTRL_STATUS 0x3D7E600` bit0 | bit0==1 | 100 ms |

明确不做：不写 `RSCC_SEQ_MEM`、`RSCC_PDC_*`、PDC；不写 `CX_MISC_TCM_RET_CNTL`；不以 `CX_MISC_GFX_PWR_CLK_STATUS` 为门槛（Linux 只对 A850 族用它，`L/a6xx_gmu.c:138-166`）；不以 SYSRESET 读回证明“复位已生效”。失败处理：任一步失败走 4.7.9 的强制关断，S5 内最多重新冷启动 2 次（共 3 次），仍失败则停在 S5 并保留现场；不永久毒化、不无限重试。

#### 4.7.4 RSCC 睡眠（slumber 用，P1）

`CM3_SYSRESET=1` → 读 `GENERAL_9` 保存 → `RSCC_CONTROL_REQ=BIT0` → 轮询 RSCC `RSC_STATUS0_DRV0 0x3D50010` bit16（A830 用 bit16，A840/A850 族用 bit30，`L/a6xx_gmu.c:650-674`）上限 100 ms → `RSCC_CONTROL_REQ=0`。

#### 4.7.5 HFI 传输层（`A830Hfi`，【源一致；同一布局已在 sheng 的 gen7 实测】）

- 共享内存：GMU VA `0x60008000`、16 KiB，CPU WC、GMU NC。表头 dword[0..5] = `version 0, size 168, qhdr0_offset 6, qhdr_size 12, num_q 3, num_active_q 3`（Linux 写字节数，固件不依赖）；3 个队列头各 12 dword：`status=1, start_addr, type=(prio<<8)|id, queue_size=1024(dword), msg_size=0, dropped=0, rx_watermark=1, tx_watermark=1, rx_request=1, tx_request=0, read_index=0, write_index=0`；cmd 队列 id 0 prio 10 在 `+0x1000`（type `0xA00`），msg 队列 id 1 在 `+0x2000`（`0xA01`），dbg 队列 id 2 在 `+0x3000`（`0xA02`）。只在首个 ACK 都收不到时改试 KGSL 布局（队列在 `848+i·4096`，`KG16/adreno_gen8_hfi.c:183-219`）。
- 每次 GMU 启动前、GMU 仍在复位时把三个队列的读写下标清 0；GMU 运行期间绝不重置。
- 命令头：`(seq<<20)|(type<<16)|(dwords<<8)|id`，type 0 为命令，dwords 含头、最大 255，seq 12 位递增。
- 写：空闲 = `(r−w−1) mod 1024`，需 ≥ 向上取整到 4 的长度；不足 4 的倍数用 `0xFAFAFAFA` 填；`dmb oshst` 后更新 `write_index`；`dsb st` 后写门铃 `HOST2GMU_INTR_SET 0x3D7E650=1`。同一时刻只有一条未应答命令（全局 HFI 互斥锁）。
- 读：`read==write` 为空；`size=hdr[15:8]`，环绕拷出；`read=ALIGN(read+size,4) mod 1024`，`dmb` 后写回。
- ACK 匹配：`hdr[19:16]==1`（ACK），word1 为被应答的请求头，两边去掉 bit19（RECORD）后整字相等才匹配（KGSL `CMP_HFI_ACK_HDR`；Linux 只比 seq，偏弱）。
- 状态字：`FEATURE_CTRL(11)`、`GET_VALUE(12)`、`SET_VALUE(13)` 在 HFI 小版本 >4 时为 v5：word2 是返回值、word3 是错误码（0 成功，8 不支持，6 超时）；其他消息 word2 是错误码（`KG16/adreno_gen8_hfi.c:387-421`）。
- 非 ACK 消息：`F2H_MSG_ERR(100)` `{hdr, code, data[16]}` 记录后按致命处理；`F2H_MSG_DEBUG(101)` 记录后继续；`F2H_MSG_MEM_ALLOC(20)` 与 `F2H_MSG_GMU_CNTR_REGISTER(110)`（`KG16/adreno_hfi.h:468,475`）是要求主机应答的资源请求，RB 模式不支持，收到即以原因码 22 让 S5 失败退出，不等超时（同一固件在 annibale 上 START 之后发过请求 20，不应答就失败，`AW/docs/progress/2026-10-25-gpu-preflight-pass.md` §3–4；annibale 普通模式同样把这两种请求判为立即失败，`AW/docs/plan/2026-10-25-ordinary-ring-probe.md` §3）；其他未知消息记录、计数后忽略。
- dbg 队列只在持有 HFI 互斥锁的线程里排空：每条命令完成后一次，以及 336 的下半部（4.12.3）。中断上半部从不碰任何队列下标，否则会与线程侧并发修改 `read_index`（主中断上下文拿不到 2.3 规定的 HFI 互斥锁）。
- 等待应答：100 µs 睡眠步进轮询 msg 队列（或 `GMU2HOST_INTR_INFO` bit0），总上限 **1 s**（KGSL `HFI_RSP_TIMEOUT`；作者 100 ms 太短）；每轮读 `CM3_FW_INIT_RESULT`，`(v&0xE00)≠0` 立即判 NMI；每轮看 `GMU2HOST_INTR_INFO` bit15（CM3 故障，致命；bit23 只记录）；取到消息后写 `GMU2HOST_INTR_CLR=bit0`。

#### 4.7.6 HFI 启动消息（HFI 2.7.1，`A830Hfi::start`，【源一致·未实测】）

在 B8 之后依次发送；每条等匹配的 ACK 且状态为 0；任一失败走强制关断。

| 序 | 消息 | 内容 | 头（seq 从 1 起） |
| --- | --- | --- | --- |
| M1 | `H2F_MSG_TABLE(15)`，`type=HFI_TABLE_GPU_PERF(1)` | `hdr, version=0, type=1`；entry0 `{count=N_gx=12, stride=3, 12×{gx_vote, dep_vote, freq_kHz}}`（下标 0 为关）；entry1 `{count=3, stride=2, {0,0},{cx(500000),500000},{cx(650000),650000}}`，共 `13+3×12=49` dword | `0x0010310F` |
| M2 | `H2F_MSG_BW_VOTE_TBL(3)` | 160 dword：`bw_level_num=12, cnoc_cmds_num=1, ddr_cmds_num=3, cnoc_wait_bitmask=1, ddr_wait_bitmask=commit 位`；`cnoc_cmd_addrs[6]={CN0}`；`cnoc_cmd_data[2][6]={{0x40000000},{0x60000001}}`；`ddr_cmd_addrs[8]={SH0,MC0,ACV}`；`ddr_cmd_data[16][8]` 行=第 0..11 级 | `0x0020A003` |
| M3 | （P3）ACD | `FEATURE_CTRL{12,1,0}`（v5 应答）+ `H2F_MSG_ACD_TBL(7)` 37 dword `{version=1, enable_by_level=0x0FF0, stride=1, num_levels=8, data[32]}` | — |
| M5 | IFPC | P0：`FEATURE_CTRL{9,0,0}` 显式关闭（annibale 普通模式在 CORE_FW_START 之前这样发，`AK/adreno8_hfi.c:706-726`）；KGSL 在 IFPC 关时根本不发（`KG16/adreno_gen8_hfi.c:636-644`），说明固件默认就是关，所以应答状态非 0 只记录、不失败。P2：`FEATURE_CTRL{9,1,0x1680}`（300 µs 迟滞） | `(seq<<20)\|0x40B`（v5 应答） |
| M6 | `H2F_MSG_CORE_FW_START(14)` | `{handle=0}` | `(seq<<20)\|0x20E` |
| M7 | （可选）`GET_VALUE(12)` | `{type=122 GMU_AB_VOTE, subtype=0}`；v5 word2==1 表示支持 AB 投票 | `(seq<<20)\|0x30C` |
| M8 | `H2F_MSG_START(10)` | 无负载；放在 CORE_FW_START 之后、OOB 之前（1.3 第 10 条） | `(seq<<20)\|0x10A` |
| M9 | `H2F_MSG_GX_BW_PERF_VOTE(30)` | `{ack_type=1, freq=初始 GMU 档 2, bw=bus 档 2 \| 0xFFFF0000}`（字段顺序 hdr、ack_type、freq、bw） | `(seq<<20)\|0x41E` |

不发 `H2F_MSG_INIT(0)`、`FW_VER(1)`、`PERF_TBL(4)`（只属 legacy 或 HFI<2.6.1）。P0 的发送顺序为 M1、M2、M5（关闭形式）、M6、M7、M8、M9；M5 的关闭形式是唯一允许应答状态非 0 的消息。M9 应答后：按 4.3.3 调整主机 DDR 底线（默认底线等于启动档，不发 TCS），再读一次 `SW_FUSE_VALUE`。把 ACD 关闭状态经 AOSS QMP 告诉 AOP 的可选步骤在 S1 开头，见 4.17 的 P3 行与 3.6 的 `pagani-gpu-qmp`。

#### 4.7.7 投票表（`A830Votes`）

- **GX 票**（KGSL `adreno_rpmh_setup_volt_dependency_tbl`，`KG16/adreno_rpmh.c:43-89`）：主轨 `gfx.lvl`，次轨 `gmxc.lvl`（存在且非哑），否则 `mx.lvl`。目标 vlvl `v`：`pidx`=首个 `gfx[k]≥v` 的下标（找不到即失败），`cur=gfx[pidx]`；`sidx`=首个 `sec[j]≥cur` 的下标（找不到取最后一个）；`vote=(cur<<16)|(sidx<<8)|pidx`。
- **依赖票**（gen8 专有，KGSL `GEN8_DEP_VOTE_SET`，`K/…adreno_gen8_rpmh.c:57-115`）：`mx_idx`=首个 `mx[j]≥v`（找不到取最后一个）；`cx_idx=0xFF`（本机 DT 没有 `qcom,cx-level`）；`dep=0xFFFFC000|((mx_idx&0x3F)<<8)|cx_idx`。下标 0 的 dep 为 `0xFFFFFFFF`。（Linux/作者写法 `0x0FFFC000|…`，GMU 回 `F2H_MSG_ERR` 时作对照。）
- **CX 票**（GMU 自身）：`{0,0}`、`{500000 kHz, 0x40}`、`{650000 kHz, 0x80}`，主轨 `cx.lvl`、次轨 `mx.lvl`，算法同 GX。
- **带宽表**：第 i 级用 4.1.4 的 DDR kBps 与 4.3.2 的 BCM 公式编码 `{SH0, MC0, ACV}`；第 7 级起 ACV 带 perfmode BIT(2)。
- **ACD 表**：4.1.4 的 acd 列，`enable_by_level` 位 k 表示 GMU 档 k 有值（11 档表为 `0x0FF0`），data 按 k 升序。

主机测试用合成 cmd-db（含 `gmxc` 存在/哑、`gbx` 缺省）对照 KGSL 公式逐字校验；S0 在设备上把实际 vlvl 表与编码结果写进状态块诊断页，供第一次上机比对。

#### 4.7.8 OOB 与 GX 上电（S5 末尾，`A830Gmu::oobSet/oobClear`）

- `OOB_GPU` set：`HOST2GMU_INTR_SET 0x3D7E650 = 1<<30` → 轮询 `GMU2HOST_INTR_INFO 0x3D7E648` bit31（100 µs 步进，上限 100 ms）→ `GMU2HOST_INTR_CLR = 1<<31`。clear：`HOST2GMU_INTR_SET = 1<<31`，无应答。引用计数，嵌套只发一次。`perfcounter` OOB（set bit28、ack bit30、clear bit29）只在采样性能计数时用。gen8 不支持 `boot_slumber`、`dcvs` 两种 OOB（KGSL 对 req≥6 返回 -EINVAL）。
- 置位后的 **GX 上电确认（门槛）**：以 10 µs 步进、1 ms 为上限轮询 GMUCX `GFX_PWR_CLK_STATUS 0x3D7DFA0` 的 `[1:0]==0`（bit0 GDSC 关、bit1 时钟关；KGSL `gen8_gmu_gx_is_on`，`KG16/adreno_gen8_gmu.c:677-684`）。不满足就让 S5 以原因码 6、步骤 0x21 失败退出，**不访问任何 GX 寄存器**：S6 第一步就要写 `0x81C`、`0x0A`，GX 没上电时这是同步外部中止，结果是 panic 而不是“记录后停下”（R2 gmu-boot H20 正因此保留了 GMUCX 这一项）。`RPMH_POWER_STATE 0x3D7DFA4`（应为 0）与 `CX_MISC_GFX_PWR_CLK_STATUS 0x3D9E20C` 只记录：后者是作者新加的判据，任何驱动都没在 A830 上用过，R2 H20 已说明它不能作门槛。
- 同一窗口内写 GMU 侧寄存器（每次 GMU 启动后都要重写）：`GPU_CX_BUSY_MASK 0x3D8EC38=0xFF000000`；`POWER_COUNTER_SELECT_XOCLK_0 0x3D7F100 [7:0]|=0x20`；`POWER_COUNTER_SELECT_XOCLK_1 0x3D7F104 [7:0]=0x04`（IFPC 计数）；`POWER_COUNTER_ENABLE 0x3D7F040=1`；AO 时钟门控（KGSL `gen8_ao_hwcg_regs`）`CGC_MODE 0x3D8EC24=0x00020000`、`CGC_DELAY 0x3D8EC28=0x00010111`、`CGC_HYST 0x3D8EC2C=0x00005555`。
- `OOB_GPU` 在 S7 末尾清除（Linux `hw_init` 末尾、KGSL 非 hwsched 同此）。IFPC 关闭时 GMU 不会让 GX 断电，所以清除后 GX 仍在电；**不再像作者那样永不清除**。

#### 4.7.9 强制关断（故障或超时，`A830Gmu::forceOff`，【源一致·未实测】）

顺序照 KGSL `gen8_gmu_pwrctrl_suspend`（`KG16/adreno_gen8_gmu.c:1389-1430`）：先清栅栏、立即复位 CM3，再 halt GBIF、关 GX，RSCC 睡眠放在最后。CM3 先停下，后面 halt GBIF 与关 GX 期间就不会再有 GMU 发起的访问或投票与主机争抢。

1. 屏蔽 GMU 中断；`CX_FALNEXT_INTF 0x3D7DFB4=1`（防止 GEMNOC 在断电过程中先进入塌陷，KGSL 同处注释）；`PWR_COL_KEEPALIVE 0x3D7DF90=0`。
2. GPU RSCC `TCS0..9_DRV0_STATUS`（`0x3D50D28` 起，步长见 `docs/inputs/power-gmu/A830PowerGmuRegs.h`）各等 ≤1 ms，超时不判失败。
3. `AHB_FENCE_STATUS_CLR 0x3D8EC50=0x7`；`AHB_FENCE_CTRL 0x3D8EC40=0`；`dsb sy`。
4. `CM3_SYSRESET 0x3D7E000=1`；`dsb sy`。
5. `GBIF_HALT 0x3D0F114=BIT1`，等 `GBIF_HALT_ACK 0x3D0F118` bit1，上限 100 ms（SMMU 处于 stall 模式时先放行，首版为终止模式不涉及）。
6. 若 GX 仍开（`GFX_PWR_CLK_STATUS&3==0`）：主机经 DRV-2 临时投 `gfx.lvl`/`gmxc.lvl` 最低非零档 → `GX GDSCR 0x3D68024|=BIT0` → 轮询 `CFG_GDSCR 0x3D68028` bit15 上限 2000 µs → 撤掉这张临时的 GX 票（它不是 4.3.3 的常驻主机票）【推断：Linux 经 gxpd 父电源域完成】。
7. RSCC 睡眠（4.7.4；其中第一步的 `CM3_SYSRESET=1` 在第 4 步已做过，重写无害；失败只记录）；`GBIF_HALT=0`；第 4.2.3 节断电（只在 P1 以后需要）。

### 4.8 GPU 硬件初始化（S6，`A830Cp::hwInit`）

前提：S5 已置 `OOB_GPU` 并通过 GX 上电确认（4.7.8）；CB0 与 LPAC 路由已完成。写法：带管线的寄存器先写 `CP_APERTURE_CNTL_HOST 0x81C = pipe<<12`（BR=1、BV=2、LPAC=3 跳过、AQE0=4、AQE1=5、DDE_BR=6、DDE_BV=7），写完立即写回 0；整个 hw_init 持有设备 MMIO 锁，中断上半部从不碰 `0x81C`。顺序照 Linux `hw_init`（`L/drivers_gpu_drm_msm_adreno_a8xx_gpu.c:625-858`），取值与 KGSL `gen8_start`（`K/adreno_gen8.c:1301-1540`）逐项一致。dword 偏移，物理地址 = `0x3D00000 + 4×偏移`。

| 步 | 寄存器 | 值 |
| --- | --- | --- |
| 1 | `CP_APERTURE_CNTL_HOST 0x81C`；`GBIF_HALT 0x3C45`；`RBBM_GBIF_HALT 0x0A` | 0；0 后读回一次；0 后读回一次 |
| 2 | `RBBM_SECVID_TSB_CNTL 0xF803`；`TSB_TRUSTED_BASE 0xF800/0xF801`；`TSB_TRUSTED_SIZE 0xF802` | 0；0；0（不做安全渲染） |
| 3 | `RBBM_PERFCTR_GPU_BUSY_MASKED 0x19E` | `0xFFFFFFFF` |
| 4 | `UCHE_CCHE_GC_GMEM_RANGE_MIN 0xF08/0xF09`；`SP_HLSQ_GC_GMEM_RANGE_MIN 0xAE10/0xAE11` | `0x0000_0000_0400_0000` |
| 5 | `UCHE_TRAP_BASE 0xE08`、`UCHE_WRITE_THRU_BASE 0xE06`、`UCHE_CCHE_TRAP_BASE 0xF06`、`UCHE_CCHE_WRITE_THRU_BASE 0xF04`（各 64 位） | `0x0001_FFFF_FFFF_F000` |
| 6 | `RBBM_PERFCTR_CNTL 0x460`；`RBBM_SLICE_PERFCTR_CNTL 0x500`；GMU `POWER_COUNTER_SELECT_XOCLK_1 [7:0]`；`CP_PERFCTR_CP_SEL[0] 0x8D0` | 1；1；4；1 |
| 7 | UBWC（4.14.3）：`GRAS_NC_MODE_CNTL 0x8700`（BR、BV）；`RB_CCU_NC_MODE_CNTL 0x8E08`（BR）；`RB_CMP_NC_MODE_CNTL 0x8F00`（BR）；`SP_NC_MODE_CNTL 0xAE02`；`TPL1_NC_MODE_CNTL 0xB604` | `0x60`；`0x6`；`0x20006`；`0x26`；`0x6` |
| 8 | `TPL1_BICUBIC_WEIGHTS_TABLE[0..19] 0xB606..` | `0, 0x3FE05FF4, 0x3FA0EBEE, 0x3F5193ED, 0x3F0243F0, 0, 0x3FD093E8, 0x3F4133DC, 0x3EA1DFDB, 0x3E0283E0, 0x0000AC2B, 0x0000F01D, 0x00114412, 0x0021980A, 0x0051EC05, 0x0000380E, 0x3FF09001, 0x3FC10BFA, 0x3F9193F7, 0x3F7227F7` |
| 9 | `UCHE_CLIENT_PF 0xE11` | `0x81` |
| 9a | （M11，LLCC 激活成功时）`GBIF_SCACHE_CNTL1 0x3C02`、`GBIF_SCACHE_CNTL0 0x3C01` | `0x09249249`、`0x02492500`（SCID 9） |
| 10 | 非上下文表（`A830HwInit.h` ← KGSL `gen8_0_0_nonctxt_regs`，40 项，按每项 pipe 位逐管线写，PIPE_NONE 时 0x81C=0）。第一项 `CP_SMMU_STREAM_ID_LPAC 0x814=0x101` 必写；表内 `RB_GC_GMEM_PROTECT 0x8E09=0x0C000000` 照写 | 见 `K/adreno-gpulist.h:2808-2872` |
| 11 | `RBBM_INTERFACE_HANG_INT_CNTL 0x2F`；`RBBM_SLICE_INTERFACE_HANG_INT_CNTL 0x58F` | `0x40CFFFFF`（bit30 + 0xCFFFFF 周期）；`0x40000000` |
| 12 | GMU：`GPU_CX_BUSY_MASK`；`XOCLK_0 [7:0]\|=0x20`；`POWER_COUNTER_ENABLE` | 4.7.8 |
| 13 | `CP_PROTECT_CNTL_PIPE 0x84E`（BR、BV）；`CP_PROTECT_GLOBAL[i] 0x850+i`（KGSL `gen8_0_0_protected_regs` 第 0–46 项与第 63 项，值 = `start \| (end−start)<<18 \| noaccess<<31`，`K/adreno-gpulist.h:2874-2923`）；最后一项 `[63]=0x80027C02` 同时写 `CP_PROTECT_PIPE[15] 0x8AF`（BR、BV）；`[47..62]` 不写 | `0xFFFF000B` |
| 14 | `RB_CONTEXT_SWITCH_GMEM_SAVE_RESTORE_ENABLE 0x8E50`（BR） | 1（Linux 无条件写；不开抢占也无害） |
| 15 | 逐管线（BR、BV、AQE0、AQE1、DDE_BR、DDE_BV）：`CP_APRIV_CNTL_PIPE 0x84D`；`CP_INTERRUPT_STATUS_MASK_PIPE 0x8B8`；`CP_HW_FAULT_STATUS_MASK_PIPE 0x8B4`；结束 0x81C=0 | BR `0x6F`、其余 `0x0F`；`0x07E83F7F`；`0x0000077F` |
| 16 | `RBBM_INT_CLEAR_CMD 0x61`；`CP_INTERRUPT_STATUS_MASK_GLOBAL 0x8B6`；`RBBM_INT_0_MASK 0x62` | `0xFFFFFFFF`；`0x007F007F`；`0x33D283C2`（= Linux `A8XX_INT_MASK` = KGSL `GEN8_INT_MASK`）。此时 GIC 上 332 尚未使能，电平中断只在 GPU 内挂起，S8 才接线 |
| 17 | `CP_SMMU_APERTURE` SCM 已在 S3 做过；Linux 每次 `adreno_hw_init` 都调，恢复路径（4.13.4）会重做 | — |

非上下文表的版本选择见 1.3 第 8 条；生成器 `scripts/derive-a830-hwinit.py --variant kgsl|linux` 两种都能产出，默认 kgsl，并在头文件里记下来源文件的 sha256。

### 4.9 SQE/AQE、环与 CP_ME_INIT（S4/S6，`A830Cp::startCp`）

- 微码：SQE `gen80000_sqe.fw` 111892 字节（sha256 `9c8f009b…`），从偏移 4 起复制 `0x1B510` 字节到 GPU VA `0x100000`；AQE `gen80000_aqe.fw` 25340 字节（`f04cb97e…`），复制 `0x62F8` 字节到 `0x180000`（Linux `adreno_fw_create_bo` 用 `size-4`，`L/…adreno_gpu.c:641-660`）。CPU 经 WC 写入后 `dsb sy`，之后不再改。**注意**：smmu-cp 研究里写的“111476/25044 字节”是笔误，以文件实际大小为准。
- `CP_SQE_INSTR_BASE 0x816/0x817 = 0x0010_0000`；`CP_AQE_INSTR_BASE_0 0x818/0x819 = 0x0018_0000`（上游 a8xx 有 AQE 时写）；`AQE_1 0x81A` 只在开 LPAC 时写，不写。
- 环（KGSL `gen8_rb_start`，`K/adreno_gen8.c:1718-1760`）：环页填 0 → `CP_RB_RPTR_ADDR 0x804/0x805 = memptrs+0`（BR RPTR 影子）→ `CP_RB_RPTR_ADDR_BV 0x808/0x809 = memptrs+8` → `CP_RB_CNTL 0x802 = 0x20C`（32 KiB，BLKSZ 2）→ `CP_RB_BASE 0x800/0x801 = 0x0020_0000` → 读 `CP_RB_RPTR 0x806` 与 `CP_RB_WPTR 0x807` 记录（不作判据：KGSL `gen8_rb_start` 只清自己的影子与软件指针，不检查硬件指针，`K/adreno_gen8.c:1718-1760`）→ memptrs 清零。
- 启动前的功能检查只有一项：LPAC 路由读回 `0x300`（不满足即失败，4.5.5）。sGFSR 与 CB0/CB2 的 FSR 只读取记录、FSR 写回清除，**不作前置条件**：它们是粘滞位，R2 cp-zap `cp-health-strict` 证明作者把它们判为致命会误杀（例如 GMU 一次无害的越界访问在 CB 上留下的故障位）。
- `CP_SQE_CNTL 0x815 = 1`，`dsb sy`；回读只记录（它是正在运行的 SQE 的控制寄存器，读回值不保证等于写入值；作者把它列入 71 次精确回读，R2 cp-zap 判为地雷）。**不能写 0x808**（Gen8 上是 BV RPTR 地址）。
- `CP_ME_INIT`（一次写入环的 10 个 dword，`K/adreno_gen8.c:607-650`、`L/…a8xx_gpu.c:497-546`）：
  1. `TYPE7(CP_THREAD_CONTROL=0x17, 1)`，负载 `0x08000000`（CONCURRENT_BIN_DISABLE，bit27）；
  2. `TYPE7(CP_ME_INIT=0x48, 7)`，负载 `0x0000014B`（bit0 多上下文、bit1 错误检测、bit3 默认复位、bit6 抢占不存性能计数、bit8 带自旋锁的上电表）、`0x00000003`、`0x20000000`、`0x00000002`、pwrup 表地址低 `0x00301000`、高 `0`、`0x80000000`（bit31：动态表为三元组格式）。包头用 `AdrenoPM4Type7`（含奇偶校验位）。
- pwrup 页布局：`struct cpu_gpu_lock { u32 gpu_req; u32 cpu_req; u32 turn; u8 ifpc_list_len; u8 preemption_list_len; u16 dynamic_list_len; }` 后接寄存器对与三元组（`L/a6xx_gpu.h:19-37`）。`CP_ME_INIT` 时允许全 0（Linux 在 zap 之后才填；annibale 用全零页可以过）。
- 用围栏写更新 WPTR=10（4.13.2）；完成判据：`CP_RB_RPTR==10`、memptrs[0]==10、空闲判据（`RBBM_STATUS 0x12` 除 bit0 外全 0、`RBBM_INT_0_STATUS` 的 HANG_DETECT bit23 为 0）；sGFSR/FSR 只记录；上限 2 s，100 µs 睡眠步进（启动期唯一允许的轮询，此时还没接中断）。GMU 的 `GPU_CX_BUSY_STATUS 0x3D8EC30` 只记录（bit30 为 KGSL gen8 定义，bit23 为 Linux/A6XX 定义，【分歧】）。
- 超时转储（写入状态块诊断页）：`RBBM_STATUS1 0x13`、`CP_RL_ERROR_DETAILS_0/1 0x840/0x841`、`CP_CP2GMU_STATUS 0x822`；逐管线 `CP_HW_FAULT_STATUS_PIPE 0x8B3`、`CP_INTERRUPT_STATUS_PIPE 0x8B7`、`CP_PIPE_STATUS_PIPE 0x8B9`、`CP_SQE_STATUS_PIPE 0x8BB`、`CP_PROTECT_STATUS_PIPE 0x84F`；`CP_SQE_STAT_ADDR/DATA_PIPE 0x8F0/0x8F1` 的 0–15 号；`GMU_RBBM_INT_UNMASKED_STATUS 0x23B15`（`RBBM_INT_0_STATUS` 是屏蔽后的状态，诊断要看影子寄存器，R2 cp-zap）。已知根因（同 SoC）：SID1 漏路由时 BR/BV 的 PC 停在 `0xE3/0x4A`。

### 4.10 提交格式与环内包（`NativeAdrenoRing`）

- 沿用现有 A7XX framed 序列（`RingFramedFenceWords=31`），只换 A8XX 寄存器号：`CP_SCRATCH_GLOBAL[2] 0x82C`（A7XX 是 0x885）；`RBBM_PERFCTR_SRAM_INIT_CMD 0x449` 并增加 `RBBM_SLICE_PERFCTR_SRAM_INIT_CMD 0x5E8=1`；`SRAM_INIT_STATUS 0x19F`（A7XX 是 0x50E/0x50F）。
- 上下文切换前导用 Linux `a6xx_set_pagetable` 的 A7XX/A8XX 分支（`L/a6xx_gpu.c:250-345`）：`CP_WAIT_TIMESTAMP`、两次 SYNC_THREADS、`CP_RESET_CONTEXT_STATE`、LRZ 刷新、SRAM 初始化及其 `CP_WAIT_REG_MEM`。
- 每次提交末尾的栅栏（与 Linux `a7xx_submit` 相同，`L/a6xx_gpu.c:570-628`）：`TYPE4 CP_SCRATCH_GLOBAL[2]=seqno` → `THREAD_CONTROL(BR)` + `CP_EVENT_WRITE7(0x46)` 发 `CCU_INVALIDATE_DEPTH(0x18)`、`CCU_INVALIDATE_COLOR(0x19)` → `THREAD_CONTROL(BV)` + `CP_EVENT_WRITE7 {0x08000031, bv_fence lo, hi, seqno}`（CACHE_CLEAN\|WRITE_ENABLED）→ `THREAD_CONTROL(BR)` + `CP_WAIT_TIMESTAMP(0x14) {0, bv_fence lo, hi, seqno}` → `CP_EVENT_WRITE7 {0x88000031, fence lo, hi, seqno}`（再加 IRQ 位）→ `THREAD_CONTROL(BOTH)` + `CP_SET_MARKER(0x65)=0x100`。CACHE_CLEAN 在写时间戳前把 UCHE/CCU 脏数据写回内存，所以 CPU 看到 `fence==seqno` 时 GPU 写入已可见；带 IRQ 的事件完成后 `RBBM_INT_0_STATUS` bit20（`CP_CACHE_FLUSH_TS`）置位。
- 单环、单优先级；全部内核包在环里以特权身份执行；用户命令只能经 `CP_INDIRECT_BUFFER(0x3F)` 进入。

### 4.11 ZAP 与退出安全模式（S7，`A830Zap`、`A830Cp::exitSecure`）

- 输入：本机 `gen80000_zap.mbn`（12088 = `0x2F38` 字节，sha256 `1aa7af12…`，只在本地）。ELF32：ph0 PHDR `0x94` 字节；ph1 PT_LOAD 文件偏移 `0x1000`、`p_paddr 0x1000`、filesz=memsz=`0x3B4`、flags `0x08000007`（含可重定位位）；ph2 hash 段偏移 `0x2000`、长 `0xF38`、`flags[26:24]==2`。与作者内嵌版本只差 OEM ECDSA-P384 签名的 r、s（R1 已用 OPLUS 证书验证两份都有效），用本机文件。
- 保留区：`0x9B09A000/0x2000`，no-map（`LDT:481-485`）；XNU 的 4 GiB 窗口在 UEFI 内存图中不覆盖它（R1：`PIL_Reserved 0x89E00000–0xA34FFFFF`）。
- 流程（每次开机只做一次，之后只发 `CP_SET_SECURE_MODE`，Linux/KGSL 都用“已加载”标志，`L/a6xx_gpu.c:1129-1141`、`K/adreno.c:107-121`）：
  1. 校验 ELF：一个 PT_LOAD；最低地址 4K 对齐；装载跨度 `0x1000` ≤ `0x2000`；hash 段存在；元数据 ≤4 KiB。
  2. 元数据 = 文件 `[0,0x94)` + `[0x2000,0x2F38)`，共 `0xFCC` 字节，写入物理连续、4K 对齐、经 WC 别名写入的缓冲（TZ 调用期间会加 XPU 保护，CPU 侧不能有脏行，`L/…qcom_scm.c:645-660`），`dsb sy`。
  3. `PAS_INIT_IMAGE(13, 元数据 PA)`；4. `PAS_MEM_SETUP(13, 0x9B09A000, 0x1000)`；5. WC 映射 `0x9B09A000`/`0x1000`，把文件 `[0x1000,0x13B4)` 复制到 `0x9B09A000+(p_paddr−min_paddr)`，余部补 0，`dsb sy`；6. `PAS_AUTH_AND_RESET(13)`，成功后立刻解除这块 WC 映射（作者一直不解除）；7. 元数据缓冲：成功即释放，失败保留到冷重启。
  8. 环内写 `TYPE7(CP_SET_SECURE_MODE=0x66,1)` 负载 0，围栏写 WPTR，按 4.9 的空闲判据等 2 s。
  9. `RB_GC_GMEM_PROTECT 0x8E09`（BR）= `0x0C000000`，然后 0x81C=0（Linux：GMEM_PROTECT 要在转入非安全模式之后编程）。
  10. 首次：按 Linux `a8xx_patch_pwrup_reglist` 读取当前值填 pwrup 表（`L/…a8xx_gpu.c:399-475`；A830 的 ifpc/pwrup/dyn 表来自 `L/…a8xx_catalog.c:183-262`），只有 IFPC/抢占才用到。
  11. HWCG：`RBBM_CGC_0_PC 0x10B=0x702` → AO CGC 三个寄存器（4.7.8）→ `RBBM_CLOCK_CNTL_GLOBAL 0x9A=1` → `RBBM_CGC_GLOBAL_LOAD_CMD 0x9B=1` → `RBBM_CGC_P2S_TRIG_CMD 0x9C=1` → 轮询 `RBBM_CGC_P2S_STATUS 0x9F` bit0 上限 10 µs（超时只记录，作者判为致命是错的，R2 cp-zap）→ `RBBM_CLOCK_CNTL_GLOBAL=0`。
  12. `OOB_GPU` clear。
- 决不直接写 `RBBM_SECVID_TRUST_CNTL 0xF400`：ZAP 可用时它受安全保护，直接写会触发 XPU 违规（`L/…a8xx_gpu.c:797-822`）。任一 SMC 失败就停在 S7、保留资源，记录 `x0/x1`、调用次数、WAITQ 次数。
- 证据：安卓 KGSL 在安全模式下同样先写 CACHE_CLEAN 时间戳再退出（`R2 cp-zap`：`dl_up/adreno_gen8_ringbuffer.c:382-412`），所以 S6 的 CP 初始化栅栏在 ZAP 之前可用；annibale 的 hwsched 路径已走通 ZAP 认证与退出【实测·同 SoC】。


### 4.12 中断（`A830Irq`、`A830Smmu`、`A830Gmu`、`Sm8750Scm`）

#### 4.12.1 平台接口

13T 用 `PD/drivers/PaganiArmCpu/ArmCpu.cpp:804` 的 C 接口（与 `UX/drivers/ArmCpu` 同形）：

```c
extern "C" IOReturn armcpu_spi_attach(uint32_t intid, bool edge, void (*handler)(void *), void *context,
                                      uint32_t *readback, uint64_t *router) __attribute__((weak_import));
extern "C" uint64_t armcpu_spi_count(uint32_t intid) __attribute__((weak_import));
```

它把 SPI 配成 Group1、优先级 `0xA0`、指定触发方式、路由到启动核后使能；handler 在主中断上下文、EOI 之前执行，必须自己清掉电平源（`ArmCpu.cpp:161-164`）；全系统 16 个槽（`ArmCpu.cpp:165`），目前 PD 里没有其他使用者；**没有 detach，也没有 disable**（只导出 attach 与 count，`:804,836`），所以每个 SPI 在驱动生命周期内只挂一次。readback 编码：bit0 Group1、bit1 已使能、bit2 边沿、bits[15:8] 优先级；电平触发时期望 `(rb&0xFF07)==0xA003`，`router` 应等于启动核亲和值。平台没有给 SPI 发布中断控制器 nub，所以不用 `IOInterruptEventSource`（sheng 所有板级驱动都走这个接口，`SHENG/package/GpuPowerProbe.cpp:660-713`）。

M6 在 PaganiArmCpu 增加两个导出（7.4），本驱动同样弱导入：

```c
extern "C" IOReturn armcpu_spi_disable(uint32_t intid) __attribute__((weak_import)); /* 写 GICD_ICENABLER 对应位并等 RWP；保留槽位 */
extern "C" IOReturn armcpu_spi_enable(uint32_t intid) __attribute__((weak_import));  /* 写 GICD_ISENABLER；只接受已 attach 的 INTID */
```

接线失败的处理（只有 332 是硬依赖）：

- 332：函数指针为空或 attach 返回错误 → S8 以原因码 19 失败。readback 只记录，真正的判据是 S8 自检里“中断计数 +1、等待者被唤醒”（例如 Group 位读成 0 时中断根本到不了，自检自然失败）。
- 336、337、710、712、112、994：函数指针为空、attach 失败或 readback 不符，一律只记录（状态块 `irq_off` 对应位）并改用轮询：HFI 等待本来就是睡眠轮询；GMU 致命位与 CB0/CB2 的 FSR 由 DCVS 线程每秒读一次；TZ waitq 按 1 ms 轮询 `GET_WQ_CTX`（4.4.1）。
- 706（SMMU 全局故障）首版不挂（4.12.2）。

风暴保护（所有电平中断）：每个处理函数清源之后复读一次状态，“复读仍是同样的非零位”或“进入时状态为 0”都算一次无进展；连续 N 次无进展（332 为 1000，其余为 64）就先在设备侧屏蔽源（332 写 `RBBM_INT_0_MASK=0`；336/337 写对应 MASK 全 1；710/712 清 `SCTLR.CFIE`），再调用 `armcpu_spi_disable`，置 `irq_off` 位、记原因码 23，改用上面的轮询。恢复路径（4.13.4）重新初始化设备侧后，用 `armcpu_spi_enable` 重新使能。

#### 4.12.2 要挂的中断

| 源 | INTID | 触发 | 挂接时机 | 处理者 |
| --- | --- | --- | --- | --- |
| SMMU 全局 | 706 | 电平 | **首版不挂**：sGFSR 在 Gunyah 仿真的全局空间里，写回清除可能被忽略，挂上就可能是关不掉的电平风暴；annibale 在同 SoC 上一个 SMMU/GMU/GPU 中断都没挂，反而清掉了 `sCR0.GFIE`（`AK/adreno8_smmu.c:156-167`）。全局故障只在诊断读取时看 | （`A830Smmu::globalIrq` 先写好不挂，P1 之后再评估） |
| SMMU CB0 | 710 | 电平 | S3，CB0/CB2 复位（`SCTLR=0`、清 FSR）之后、第一次置 CFIE 之前；可选 | `A830Smmu::contextIrq` |
| SMMU CB2 | 712 | 电平 | 同上；可选 | 同上 |
| GMU HFI | 336 | 电平 | S5，B8 之后；可选 | `A830Gmu::hfiIrq` |
| GMU（AO） | 337 | 电平 | 同上；可选 | `A830Gmu::aoIrq` |
| GPU `kgsl_3d0_irq` | 332 | 电平 | S8；**唯一的硬依赖**（只有它在 sheng 上实测过） | `A830Irq::primary` |
| TZ waitq | 994 | 边沿 | S2，第一次 SMC 之前（可选；没挂上时按 1 ms 轮询 `GET_WQ_CTX`） | `Sm8750Scm::waitqIrq` |
| `cx_host_irq` | 112 | 电平 | 可选（只记录限频事件） | `A830Irq::cxHost` |
| MDSS（显示 TE，M12） | 115 | 电平 | 显示侧 | NativeLegacyDisplay |

#### 4.12.3 处理函数

**GPU 上半部 `A830Irq::primary`（不加锁、只碰 0x6A/0x61/0x62 三个全局寄存器、目标 ≤2 µs）**：

1. `s = RBBM_INT_0_STATUS(0x6A)`；`s≠0` 时 `RBBM_INT_CLEAR_CMD(0x61)=s`（先读后清再分派，同 Linux `a8xx_irq` 与 KGSL `gen8_irq_handler`）。
2. `pending |= s`（原子或）。
3. bit20 `CP_CACHE_FLUSH_TS`：`fence_irq_seq++`，`thread_wakeup(&A830Fence::event)`；有待回收作业时 `thread_call_enter(retire_call)`。
4. 错误位（`s & 0x33D283C2 & ~BIT20` 中的 bit1 AHB、6 ATB async、7 GPC、8 CP_SW、9 CP_HW_ERROR、22 ATB bus、23 HANG、24 UCHE OOB、25 UCHE trap、28 TSB、29 SW fuse）：bit15（PM4CPINTERRUPT）与 bit17（RB_DONE_TS）是正常事件只计数；CP_HW_ERROR 与 HANG 在上半部立刻写 `RBBM_INT_0_MASK=0x00100000`（只留栅栏中断）防止风暴，再 `thread_call_enter(fault_call)`。
5. 风暴保护：连续 1000 次 `s==0`，或清除后复读 `RBBM_INT_0_STATUS` 仍是同样的错误位，按 4.12.1 处理（写 `RBBM_INT_0_MASK=0`、`armcpu_spi_disable(332)`、记原因码 23；栅栏等待退回 16 ms 保险时钟，功能不丢）。
6. 需要切管线 aperture 的错误读取（`CP_INTERRUPT_STATUS_GLOBAL 0x8B5` → 各管线 `0x8B3/0x8B7/0x84F`，SQE_STAT）全部放在 `fault_call` 线程里、持 MMIO 锁做（Linux `a8xx_cp_hw_err_irq` 用 aperture 锁，`L/…a8xx_gpu.c:1033-1099`）。

首版 GX 常开（IFPC 关），上半部可以直接读 GPU 寄存器；以后开 IFPC 时照 KGSL：先写 GMU `KEEPALIVE`，再轮询 `AHB_FENCE_CTRL(0x23B10)==0` 最多 100×1 µs（`K/adreno_gen8.c:2243-2278,2306-2338`）。

**SMMU 上下文故障 `A830Smmu::contextIrq`（要求 SCTLR.CFIE=1）**：读 `FSR(+0x58)`，`(FSR&0xC00001FE)==0` 直接返回（bit[10:9]=0x400 是格式字段）；该 bank 还没被本驱动启用（4.5.4 第 13 步之前）时只写回清除 FSR 后返回，不记故障、不计数（开机残值）；否则读 `FAR(+0x60/+0x64)`、`FSYNR0/1(+0x68/+0x6C)`、`CBFRSYNRA(n)` 低 16 位（出故障的 SID）；记入无锁环形缓冲；写回 FSR 清除；`FSR.SS(bit30)`=1（仅 stall 模式）时写 `RESUME(+0x08)=1` 终止事务；`thread_call_enter` 通知下半部做归属：各连接的客户端窗口互不重叠（6.2），CB0 上 FAR 落在哪个窗口就只让那个连接的 `faults` 加 1（Turnip 据此只让这一个进程判设备丢失），设备继续服务。上半部在读任何寄存器之前，先经平台钩子 `fences` 读一次环的已完成栅栏（memptrs+4）与最后发出的栅栏，随故障记录交给下半部（`sampled`；M14 P0，2026-10-31）。下半部只拿它做对比、计数，不据此扣连接（`NativeAdrenoFaultVerdictFor`、`NativeAdrenoFaultNote`）：每次换客户端，上下文入口都先等上一个栅栏（排空，4.10），所以故障时在途的作业（两个栅栏之间，查 `NativeAdrenoSubmit` 记下的 64 项栅栏→客户端范围历史，`rangesInFlight`）只属于一个连接时，通常就是它的作业做的这次访问；它就是 FAR 窗口的主人记“一致”，是别的连接、或 FAR 不在任何窗口里记“不一致”，多个连接在途、没有在途作业或历史里查不到记“无法判断”，轮询发现的故障（annibale 配置不挂 710，全靠每秒轮询，没有故障时刻的栅栏）记“轮询”（`native_adreno_fault_attribution`，计量行的 `pf`，8.8）。“不一致”不能当证据扣人：终止模式加 HUPCF 下出故障的作业会照常跑完，若它恰好在上半部读栅栏前（Gunyah 下 710 的中断延迟可观）完成，而下一个在途作业属于另一个连接，在途的就只剩无辜的那一个；FAR 落在内核区的故障可能是 SQE 的无害预取（见下）；内核自用客户端与槽 0（backboardd）共用提交范围 0。所以扣连接始终按 FAR 窗口（2026-10-31 的审查 M1：曾按“唯一在途的连接”扣，会把无辜进程乃至 backboardd 判为设备丢失，已改回），计数只用来回答“作业越出自己窗口有多常见”（按进程页表 P0）。CB2（GMU）的故障，以及 CB0 上 FAR 落在内核区（`0x2000_0000` 以下）、空闲窗口或无法归属的故障，只记录并计入 `smmu_faults`，既不让任何连接丢失、也不判全局故障：R2 cp-zap 指出 SQE 的无害预取、GMU 的一次越界访问都可能留下这类故障位，它们若真的造成问题，会以挂死（看门狗、HANG_DETECT）或 GMU 致命位的形式出现（4.13.4）。终止模式加 HUPCF 下，GPU 不会因页故障挂住，故障事务读到 0 或写入被丢弃（Linux 对 adreno 的策略）。FSR 位：TF bit1、AFF bit2、PF bit3、EF bit4、TLBMCF bit5、TLBLKF bit6、ASF bit7、UUT bit8、SS bit30、MULTI bit31（`L/…arm-smmu.h:200-218`）。

**SMMU 全局故障 `A830Smmu::globalIrq`（首版不挂，4.12.2）**：诊断读取时读 `sGFSR/sGFSYNR0/1/2/sGFAR` 记录并写回；`sGFSR` bit1 USF（未识别流，例如漏了 SID1）出现在诊断页上时，结合 CP 是否挂住来判断，不单独判致命。

**GMU `A830Gmu::hfiIrq`（336）**：上半部只做三件事：读 `GMU2HOST_INTR_INFO` 存进原子变量；**无条件**写 `GMU2HOST_INTR_CLR = 0x00808006`（把掩码开放的 bit1/2/15/23 全部清掉，照 KGSL 无条件清 `HFI_IRQ_MASK`，`KG16/adreno_gen8_hfi.c:811-812`；只清“处理过的位”会让没有处理分支的 bit1 SIDEMSGQ 成为电平风暴；OOB 应答位 bit31 不在其中，由 OOB 轮询自己清）；`thread_call_enter(gmu_call)`。dbg 队列排空与 CM3 故障处理（bit15 → 全局故障与恢复；bit23 只记录）都放在持 HFI 互斥锁的下半部里（4.7.5）。**GMU `A830Gmu::aoIrq`（337）**：读 `AO_HOST_INTERRUPT_STATUS 0x3D8EC14`，同值写入 CLR `0x3D8EC10`（同 KGSL，`KG16/adreno_gen8_gmu.c:1733-1734`）；bit0 WDOG_BITE → 先屏蔽 bit0 防风暴，再经 `gmu_call` 进入恢复；bit3 FENCE_ERR → 读 `AHB_FENCE_STATUS 0x3D8EC4C` 记录；bit5 HOST_AHB_BUS_ERR → 记录。HFI 消息（bit0）默认不开中断，HFI 等待用睡眠轮询（4.7.5）。

**下半部**：`thread_call_allocate_with_options(..., THREAD_CALL_PRIORITY_KERNEL_HIGH, THREAD_CALL_OPTIONS_ONCE)` 分配 `retire_call`、`fault_call`、`gmu_call`。`thread_call_enter` 与 `thread_wakeup` 都可在中断上下文调用。栅栏等待者不经下半部，直接被 `thread_wakeup` 唤醒（4.13）。

### 4.13 栅栏、等待、挂死检测与恢复（`A830Fence`、`NativeAdrenoRing`、`NativeAdrenoPublish.cpp`）

#### 4.13.1 memptrs 页

布局同 Linux `struct msm_rbmemptrs`（`L/…msm_ringbuffer.h:30-42`）：`+0x00 rptr`、`+0x04 fence`（BR 完成序号）、`+0x08 bv_rptr`、`+0x0C bv_fence`、`+0x10` 起统计、之后 `ttbr0(u64)`、`context_idr`、`perfcntr_fence`。CPU 与 GPU 两侧都是 Normal-NC：GPU 写直达内存，CPU 读不需要失效（sheng 已这样做，`SHENG/package/GpuPowerProbe.cpp:350-353`），`acquire` 只是一条 `dsb ld`。现有 `NativeAdrenoRing` 的 +0/+4/+8/+12 用法与此一致。

#### 4.13.2 WPTR 围栏写（KGSL `gen8_fenced_write`，`K/adreno_gen8.c:652-701`）

写 `CP_RB_WPTR(0x807)` → `dsb sy` → 读 GMU `AHB_FENCE_STATUS(0x23B13，GMU 窗口 +0x57C4C)` bit0（WRITEDROPPED0）→ 置位则 `IODelay(10)` 后重写，最多 200 次（约 2 ms），超过 100 次记录，200 次仍失败返回错误并进入恢复（作者 build 68 失败仍返回成功，必须改）。IFPC 关时正常情况下第一次就成功，代价只有一次 MMIO 读。

#### 4.13.3 等待算法（取代 `IODelay(10µs)`+`health()` 轮询）

```
A830Fence::wait(seq, deadline):
  loop:
    cur = load_acquire(memptrs->fence); if ((int32_t)(cur - seq) >= 0) return OK
    if (device_lost) return kIOReturnNotResponding
    assert_wait_deadline(&event, THREAD_ABORTSAFE, min(deadline, now + 16 ms))
    cur = load_acquire(memptrs->fence); if passed → clear_wait(current_thread(), THREAD_AWAKENED); return OK
    r = thread_block(THREAD_CONTINUE_NULL)
    if (r == THREAD_INTERRUPTED)                  /* 信号或 thread_abort_safely 打断 */
        return passed ? OK : kIOReturnTimeout     /* 不能返回 Aborted，见下 */
    if (now >= deadline) return passed ? OK : kIOReturnTimeout
```

- 返回值约定：`kIOReturnTimeout` 表示“尚未完成”，被中断时也返回它；Turnip 只把 Timeout 当作未完成并按 100 ms 一片重试，其余任何返回值一律 `vk_device_set_lost`（`UX/patches/mesa/turnip-darwin-kmd.patch:438-444,449-470`）。`kIOReturnNotResponding` 只用于设备丢失；`kIOReturnAborted` 只在连接正在关闭时返回。保留 `THREAD_ABORTSAFE`（进程被杀时线程能尽快回到用户态处理 AST），不用 `THREAD_UNINT`。
- 16 ms 保险时钟只防丢中断，不靠它推进；“先 assert_wait、再复查、再 thread_block”避免丢唤醒。
- `NativeAdrenoPublish.cpp:423-445` 的 WaitFence 循环改为：无锁调用 `A830Fence::wait(fence, caller deadline)`，醒后只在 `g.lock` 内做一次零超时的 core 调用完成回收。等待期间不持任何锁、不做 MMIO、不调 `health()`/`noteStall`。直接用调用方的 `timeout_ns`（Turnip 每片 100 ms）。
- 同一环上的序号严格按序完成：提交时不再为“等更早的栅栏”做 CPU 等待（`NativeAdrenoUserClient.cpp:326-341` 改为只校验 `wait_fence ≤ lastSubmitted`），因为每个框架化提交的序言都带 SYNC_THREADS 与 CACHE_FLUSH/INVALIDATE 加 WAIT_FOR_IDLE（`NativeAdrenoRing.cpp:150-185`）。
- BO 忙闲：每个 BO 记最后使用它的 seqno，CPU 映射、释放、解除映射前只比较 fence。
- 时间戳：GetTimestamp 读 GMU AO 计数器（1.3 第 5 条），19.2 MHz，高-低-高。

#### 4.13.4 挂死检测与恢复

- 硬件：`RBBM_HANG_DETECT`（bit23，`RBBM_INTERFACE_HANG_INT_CNTL=BIT30|0xCFFFFF`）。
- 软件看门狗：有作业在途时每 500 ms 用 `thread_call` 检查一次进度（fence、`CP_RB_RPTR 0x806`、`CP_IB1_BASE 0x900/0x901`、`CP_IB1_REM_SIZE 0x902` 任一变化算有进度），连续 2 s 无进度判挂死。删除 build 68 的 50 ms TLBIALL+kick 与 200 ms GBIF 停机（R2 metal-reg：会在正常长作业上误触发，停 GBIF 却不复位）。
- 故障分级（M7 起，桌面不能因为一个客户端的页故障就永久退回 CPU 渲染）：
  - **只丢一个连接**：SMMU 上下文页故障。终止模式加 HUPCF 不会挂住 GPU（4.12.3），按 FAR 所在窗口归属到连接（4.12.3；故障时在途的作业只作对比计数，M14 P0），只给该连接的 `faults` 加 1，设备继续服务。A8xx 上的 Turnip 已知还有问题（4.14.4），这类故障在 M8–M12 期间难免。
  - **全局设备丢失**：`RBBM_INT_0_STATUS` 的 HANG_DETECT(bit23) 或 CP_HW_ERROR(bit9)、软件看门狗 2 s 无进展、GMU 致命（CM3 故障 bit15、WDOG_BITE）、WPTR 围栏写 200 次仍失败。SMMU 故障本身从不直接触发全局丢失（4.12.3）。其中只有前三种（HANG_DETECT、CP_HW_ERROR、软件看门狗，`A830FaultBlamesClient`）可能由客户端的作业造成，恢复开始时才把挂死记到 CP 所停作业的进程名上（6.2）；其余原因同样停环、给有在途作业的连接记故障，但不归责。
- 全局设备丢失（M7–M9 的处理）：`RBBM_INT_0_MASK=0` → 快照（`RBBM_STATUS 0x12/0x13`、INT 状态、RPTR/WPTR、IB1/IB2、CP 全局与各管线故障、SMMU FSR/FAR）写状态块 → 所有连接 `faults++` → 拒绝后续提交与新连接（`newUserClient` 返回错误，插件拿不到设备、该进程退回 CPU 渲染）。用户态配合补丁 `adrenometal-5c03327` 让 backboardd 在设备丢失后 0.5 s 自退，由 launchd 重启（`docs/inputs/userspace/patches/`）。
- 最小恢复（M10，`A830Recovery.cpp`；之后全局设备丢失先走恢复）：按 `L/…a8xx_gpu.c:1305-1327` halt GBIF（`RBBM_GBIF_HALT=1` 等 `RBBM_GBIF_HALT_ACK 0x0B` bit0 → `GBIF_HALT=1` 等 `0x3C46` bit0 → `GBIF_HALT=2` 等 bit1 → `GBIF_HALT=0`）→ `A830Gmu::forceOff`（4.7.9）→ S4 重装 GMU 固件（CX 未掉电，计时器不重播，4.4.2）、S5 冷启动（含 OOB 与 GX 上电确认）→ S6 完整 hw_init 与 `CP_ME_INIT` → ZAP 已驻留，只发 `CP_SET_SECURE_MODE 0` 并重写 GMEM_PROTECT、HWCG、OOB clear → RPTR/WPTR 归零、memptrs fence 设为最后发出的 seqno（唤醒所有等待者）→ 在途作业判失败、相关连接 `faults++`，`suspends++`、`recoveries++` → 用 `armcpu_spi_enable` 恢复被风暴保护关掉的中断 → 重新接受提交与连接。SMMU 上下文不动（CX 不掉电）。恢复本身失败或 60 s 内第 3 次触发，才转为永久设备丢失。
- 完善（M13）：恢复与 slumber/IFPC 的交互、故障注入矩阵、恢复耗时优化。

#### 4.13.5 队列计量（`A830Fence` 的排队计量，2026-10-30）

目的：用户看到卡顿时，判断 GPU 作业是不是在唯一的环里排队。环只有一条、没有优先级，显示的扫描输出拷贝（插件对固件扫描缓冲做的一次 Metal blit，`NativeGOPPresenter` 的 GPU 交接）排在它之前提交的所有渲染作业之后；这一节量的就是每个作业从交给 CP 到完成的时间，以及它交出时前面还有多少作业。

- **打戳**（`NativeAdrenoRing::submitWords`）：每个内核提交与框架化提交在环字写好并发布之后、屏障与 WPTR 写之前，持环锁调用 `NativeAdrenoRingWaiter::stamp`（S8 接为 `A830Fence::ringStamp`，与 `ringWaiter` 同一个 cookie）：`stamps[seqno % 64]` 存 mach 时间低 40 位、作业归属 2 位（第 40–41 位：内核 0、渲染服务器 1、外部进程 2，见下）、这次提交看到的环深度 7 位（第 42–48 位，饱和到 127）、seqno 低 14 位（第 49–62 位；槽里若不是这个序号，只能是早 64 的整数倍的序号）与有效位 63（release 原子写）；环深度 `seqno − 已完成 fence`（含它自己，至少 1）同时记成本周期的最大值，提交数加 1；带上下文入口（换客户端，或 attach、恢复之后的第一个框架化提交）的提交另计一次（`NativeAdrenoRingStampContext`）。原始包（`CP_ME_INIT`、`CP_SET_SECURE_MODE`）与被拒的提交（Busy、NoResources）不打戳；屏障或 WPTR 写失败时序号不前进，恢复后同一序号重新打戳。戳先于屏障写入，所以 CP 能完成这个作业时戳一定已在内存里；40 位时间在 19.2 MHz 下约 15 小时回绕一次，差值跨回绕仍然精确。
- **结算**：谁先看到 memptrs fence 前进，谁就按序结算“上次结算的序号”到新 fence 之间的每一个序号（一次中断合并的多个完成都算上，延迟 = 当前时间 − 戳）：栅栏中断上半部（`A830Fence::signal`，`A830Irq` 处理 bit20 时调用）；看到自己的 fence 已过的等待者；只有在没有栅栏中断可来时（`polling()`：332 未挂、被风暴保护关掉或 `fallBack` 退回轮询），才由 DCVS 线程每秒取数前先结算一次。2026-10-31 起同一时刻只有一个结算者（`q_busy_`，归属要按序看作业）：调用者先把调用数 `q_calls_` 加 1（seq_cst），再试着成为结算者；已有结算者时立即返回，结算者在放手后复查调用数，有新调用就用新读的时钟再结算一遍，所以每个完成只算一次、谁也不等谁（取代原来对 `q_seen` 的 CAS）；最后一个调用即使碰上别人正在结算而立即返回，它带来的完成也会在那个结算者返回之前算掉。结算只用原子量：无锁、无 MMIO、不分配、无浮点，每遍最多读 64 个戳，可在主中断上下文里调用。时间换算（`a830AbsToNs`）只在每秒取数时做。
- **丢戳**：在途超过 64 个时只有最新的 64 个还有戳；槽里不是这个序号的戳就记为 `missed`，不进平均值与最大值。
- **恢复**：恢复强行改写 fence 之前（`A830Fence::forceComplete`；编排器的 CP 重启 `opCpStart` 在 `NativeAdrenoRing::recover` 之前，跳到“要写入的 fence”与“最后发出的序号”中较新的一个）先调用 `queueSkip`：失败的作业算作已结算、不计量（跳转目标交给下一遍结算，结算者先读 fence 再读跳转，所以读到强行改写的 fence 的那一遍一定先跳过）；`startRing` 清零的 memptrs fence 落后于已结算序号，什么也不算。`start()` 从当时的已完成 fence 开始计量。
- **作业归属**（抢占第 0 阶段，4.17，2026-10-31）：渲染服务器（发布外壳按进程名 backboardd 标出它的每条连接，不论在哪个槽，`NativeAdrenoUserClient::setRenderServer`）的框架化提交记为 1，其余进程记为 2，内核自己的提交记为 0。结算时每个作业的服务时间 = 完成时刻 − max(上一个作业的完成时刻, 本作业的戳)；调用者的时钟早于上一遍时按上一遍的时刻算（服务时间不会为负）。一遍里看到多个完成时，第一个拿走整段时间、其余为 0：渲染服务器服务时间与延迟因此是上界，但 fw、n4 与外部服务时间可高可低（外部作业不是这一遍的第一个时记 0，排在它后面的渲染服务器作业的 fw 随之偏低；它是第一个时又把后面作业的时间算进去，fw 偏高）。有栅栏中断时一遍只跨中断延迟，误差以它为限；轮询时（`polling()`，IRQ 行 `q~`、计量行 `poll`）一遍跨一个轮询周期，这三项基本没有意义，抢占第 1 阶段的门槛只采信中断取栅栏的数据（4.17）。渲染服务器作业的 fw = 它打戳时还在环里的外部作业（从第 `seqno − 深度 + 1` 个起）的服务时间之和，减去其中最老的那个在打戳之前已经跑掉的部分；中间有作业丢了戳、或前面超过 63 个作业时 fw 记为未知（`fw_jobs` 少于 `rs_jobs`；戳里的深度饱和在 127，128 个以上同样未知，不会回绕成“前面没有作业”）。每周期给出：渲染服务器作业数与服务时间平均/最大，fw 已知的作业数与 fw 平均/最大，fw 超过 4 ms 的个数 n4，外部作业数 fj 与服务时间合计 fb，上下文入口次数。结算者自己保存最近 64 个作业的起点、服务时间与之前的外部服务累计（`q_jobs_`），每个渲染服务器作业 O(1)。
- **显示与日志**（2026-10-31）：只在 `pagani-gpu-status=2` 的运行页上画：MEM 行在客户端尾巴之前加 ` fw<最大>ms/<n4>`（本周期 fw 最大值不为 0 时，最多 13 个字符，客户端尾巴让位），IRQ 行队列尾部后加 ` sw<上下文入口>`（放得下才加）；这两行随 G 记录每秒进 ramoops。完整数字（rs、fw 的平均与最大、n4、fj、fb、上下文入口的合计与每秒最大、轮询周期数）在诊断页模式（`pagani-gpu-status=2/3`）下每分钟至多一条计量行（8.8），只在内容变化时写；默认的 `status=1` 不写。
- **周期**：DCVS 线程每秒一次（`A830Dcvs` 的状态窗口，内存行之后、DCVS 行发布之前，GPU 醒着与睡着都调用）：`A830DcvsOps::queueRow` → 编排器 `dcvsQueueRow` → `A830Fence::takeQueue` 取走本周期并开始下一周期 → `adreno830_status_queue` 保存。一个周期 `Adreno830StatusQueue`：提交数、结算数、丢戳数、平均与最长延迟（µs）、超过 16 ms（`A830FenceQueueSlowUs`）的个数、最深环深度、`polled`（本周期的完成是在没有栅栏中断时看到的，数值是上界）；2026-10-31 起还有上面的归属各项与上下文入口次数（`rs_jobs` … `contexts`），DCVS 线程存好之后再调用 `adreno830_status_meter`（8.8）。
- **显示**：周期数据不进 576 字节的状态块（布局与静态断言不变），由 `Adreno830Status.cpp` 的静态变量保存（与 `runKnobs`、`runTemp` 同样处理，`adreno830_status_reset` 清掉），只改运行页第 3 行（8.4；2026-10-31 起第 2 行还有作业归属字段，见上）：`IRQ <n> wait <n> to <n> late <n> | q <均>/<最大>ms d<深度> s<慢>`，例 `IRQ 1942 wait 5963 to 3698 late 2 | q 4/38ms d5 s3`。下一次发布（醒着时就是 DCVS 行那一次）显示它；GPU 行有变化时 BootProgress 每秒写一次 ramoops 日志，所以日志里每秒都有一份。
- **怎么读**：同一秒里 `最大` 远超 16 ms、`s` 非 0、`d` 达到 3 以上，说明作业在环里排队（扫描输出拷贝排在渲染之后），掉帧来自排队；`d` 只有 1–2 而 `最大` 很大，说明单个作业本身慢（着色器、带宽或频率，对照 DCVS 行的驻留）；`q` 正常而照样卡顿，原因不在 GPU 环里（CPU 拷贝回退、显示路径）。按秒与 FPS 行、ramoops 的 `J jitter` 事件对照。
- 主机测试：`tests/test_a830fence.cpp`（10 个用例：戳与深度、只结算一次、周期值与清零、40 位回绕、64 戳与丢戳、跳过与强制 fence、上半部/等待者/轮询取数三方、结算者的复查（仅主机测试的钩子 `a830FencePassHook` 在一遍中途送来新完成，结算者返回前用新时钟算掉它）、三方竞争 20000 个完成且最后一次调用之后没人再调；另 13 个归属用例：交错作业的服务时间、fw 与 n4、一次中断结算多个（外部作业在前 fw 偏高、渲染服务器作业在前 fw 偏低，与逐个结算对照）、内核作业在前、前面超过 63 个、128 个以上与前面有丢戳的作业、跳过、上下文入口、轮询周期、早于上一遍的调用者时钟、按归属的三方竞争）、`tests/test_ring.cpp`（打戳的时机、次数与持锁）、`tests/test_status.cpp`（行布局、最坏 66 字符、写入者）、`tests/test_a830dcvs.cpp`（每秒一次、醒睡都调、在内存行之后与 DCVS 行之前）、`tests/test_start.cpp`（接线与恢复时的跳过；SMMU 的 `fences` 钩子按“已完成、最后发出”两个输出接到栅栏与环，故障下半部把 CB0 故障的 FAR、栅栏与 `sampled` 原样交给发布外壳）。

### 4.14 内存、一致性、UBWC 与 GMEM

#### 4.14.1 映射规则（每类对象：GPU PTE / CPU 映射 / 维护）

| 对象 | GPU 侧 | CPU 侧 | 维护 |
| --- | --- | --- | --- |
| ring、memptrs、pwrup、SQE/AQE、HFI、GMU 区 | NC（特权） | WC | 只需屏障 |
| 自有 BO（池） | NC | WC（只在 BoCpuMap 时设一次属性；池页终身归驱动，不还给 VM——sheng int-100 发现页属性会回退） | 无 |
| GPU 只读的导入页（纹理、IOSurface 读源） | `io_coherent=1` 时 WB+IS；否则 NC | 保持所有者原属性 | 一致时无；不一致时提交前清洗 |
| GPU 写的导入页（CA 渲染目标） | NC | 保持所有者原属性 | 内核：仅在导入时 `dc civac` 一次，之后提交与完成都不维护（`NativeAdrenoVm.cpp:15-18`、`NativeAdrenoUserClient.cpp:280-286`）。用户态维护不能省：“GPU 拥有期间 CPU 不碰像素”并不成立，DF 呈现路径每帧用默认缓存属性的映射读这些表面（`PD` 工作区 `NativeLegacyDisplay.cpp` 的 `sourceMapping()`：`memory->map(kIOMapAnywhere\|kIOMapReadOnly)`），见下面的插件规则 |
| 扫描缓冲（splash 区） | NC 只写 | 不映射或 WC | 无 |
| 页表页 | 一致性遍历 | M10 前 WC/清洗，V4 通过后可缓存 | M10 后只 `dsb ishst` |

- 删除提交/完成路径上的 `publishMapped`/`acquireImported`（`NativeAdrenoVm.cpp:418-439`）；改为“需要维护的映射”链表，只在 `io_coherent=0` 时非空。环发布（`NativeAdrenoRing.cpp:308`）改为 `dsb st`。
- 插件的缓存维护：现在 `flushSurfaces`（`UX/drivers/AdrenoMetal/NativeBridgeMetalFrontend.mm:483-497`）在每个命令缓冲的 commit 与 finish 时，对涉及的**整个** IOSurface 做 `sys_dcache_flush`（`:1188`、`:1154`），不分读写。用户态补丁 M8（5.5）把它按方向拆开，规则是：
  - commit：GPU 以非一致方式**读**的表面做 clean（`io_coherent=1` 时，走一致映射的读源可免）；GPU **写**的表面一律 clean，因为 CPU 可能有脏行，GPU 以 NC 写入之后脏行被逐出会覆盖 GPU 的结果（只有 `pagani-gpu-coherent=2` 时可免）；
  - finish：只对 GPU **写过**的表面做 invalidate（GPU 只读的表面内存没变，不需要）；这些表面之后会被 CPU 读（DF 路径、截图等），不能跳过，只有 `pagani-gpu-coherent=2`（V9b/V9c 通过、GPU 写会让 CPU 行失效）时才可免；
  - 每次维护的字节数与耗时记入插件 Stats（已有 `gSurfaceFlushBytes`/`gTimeSurfaceFlush` 计数），经报告行显示，计入 5.1 的帧预算。
- 种子规则（2026-11-01，默认关，种子 `NativeAdrenoMetal.env` 里 `METAL2VULKAN_SURFACE_CLEAN=seed` 打开，规则和理由在插件 `SurfaceMaintenance.h`）：commit 不再清 seed 还是 0 的表面。脏行只有三个来源：内核清零和页的上一任主人，`mapLocked` 在固定页之后、GPU 看得见之前已经按物理页 clean+invalidate（内核用加速器属性 `NativeAdrenoImportClean=1` 声明这件事，`tests/test_vm.cpp` 钉住它；改这个顺序就要撤掉属性）；进程经自己的映射写，合法写必须持写锁，而 IOSurface 在 `IOSurfaceLock` 里、第一次写之前就加 seed（21H461 内核缓存的 `IOSurface::lock`，macOS 27 实测）；插件自己的 `replaceRegion`，它现在自己持写锁。因为 seed 在锁里就动，“seed 变过之后又稳定”说明不了写完没有，所以一旦离开 0 就永远走老路。没覆盖的只有不持锁的写。21H461 的 `sys_dcache_flush` 只是一条 `dsb ish`，finish 的 invalidate 实际什么都不失效，EL0 走页的只有 commit 的 clean。
- 依据：KGSL gen8_0_1 带 `ADRENO_IOCOHERENT`（`K/adreno-gpulist.h:2975-2985`）；Linux A830 带 `ADRENO_QUIRK_HAS_CACHED_COHERENT`（`L/…a8xx_catalog.c:1544`）；DT `dma-coherent`（`LDT:24144`）；IDR0.CTTW=1。KGSL 只允许 CPU 侧可缓存的内存走 `IOMMU_CACHE`，CPU 侧 WC 的内存禁止标一致（`K/kgsl_sharedmem.c:804-838`）。一致性是单向的：GPU 嗅探 CPU 缓存；CPU 不嗅探 UCHE，所以栅栏前的 CACHE_CLEAN 必不可少。
- 风险：A740 上 GPU 经 WB+IS 写入时会停在通道末尾的 CACHE_CLEAN（`NativeAdrenoUserClient.cpp:283-285`），A830 未验证。所以默认只让“GPU 只读的导入页”走一致映射。

#### 4.14.2 一致性自检（S8 默认只跑 V4、V9a 并只记录；V9b/V9c 只在独立镜像里跑；M10 起决定默认值）

- **V4 页表一致遍历**：经可缓存映射写一个新 PTE，不清洗只 `dsb ishst`，让 CP 用 `CP_MEM_TO_MEM(0x73)` 从该页复制 16 dword 到 scratch 并带栅栏；无翻译故障且数据正确即通过。
- **V9a CPU 写→GPU 读**：可缓存页由 CPU 写 64 行图样（不清洗），映射为 WB+IS 只读，CP 复制到 NC 的 scratch，8 轮全部相等。通过 → `io_coherent=1`。
- **V9b GPU 写→CPU 读**（仅 `pagani-gpu-cohtest=1`）：同页映射 RW，CPU 先读一遍使其进缓存；`CP_MEM_WRITE` + CACHE_CLEAN\|IRQ 栅栏；100 ms 内完成且 CPU 不失效直接读到新值。
- **V9c A740 挂死复现**（仅 `pagani-gpu-cohtest=1`）：一个渲染通道写 WB+IS 目标后发 CACHE_CLEAN，栅栏 100 ms 内完成。V9b、V9c 都通过才允许 `pagani-gpu-coherent=2`：GPU 写的导入页也走一致映射，并在 `DeviceInfo.flags` 置 `NativeAdrenoDeviceCachedCoherent`（ABI 已定义，`NativeAdrenoABI.h:131`），Turnip 随之提供 HOST_CACHED 内存（`docs/inputs/userspace/patches/tu_knl_darwin-a830.diff`），此时 `clientMemoryForType` 不能再固定用 InhibitCache。
- V9b、V9c 正是 A740 上“停在 CACHE_CLEAN”的复现条件（4.14.1 的风险），可能把 GPU 挂死；而 S8 同步执行、最小恢复要到 M10 才有，挂死就意味着不发布、M7 以后全部受阻。所以默认镜像不跑它们。`pagani-gpu-cohtest=1` 的独立镜像在 S8 末尾、发布之前（此时没有任何客户端，结果最干净）执行它们：每项 100 ms 超时，超时只记录结论、把 `coherent` 的上限定为 1，并让 S8 以原因码 18 停下（GPU 可能已挂住，不能发布；桌面照常走 CPU 呈现）。M10 有了最小恢复之后，超时改为先恢复、再以 `coherent≤1` 继续发布。

#### 4.14.3 UBWC（SM8750）

参数：UBWC 5.0（DT `qcom,ubwc-mode=5`；Linux `ubwc_config.c:178` 中 sm8750 为 `ubwc_5_0_hbb16`）；`highest_bank_bit=16`（KGSL gen8_0_1 也是 16）；`min-access-length=32` 所以 `mal=0`；二级、三级 swizzle 打开；AMSBC=1；rgb565 预测器=1；`rgba8888_lossless=0`；`fp16compoptdis=0`；版本标签 mode=4；`uavflagprd_inv=2`；`hbb=16−13=3`。由此得到 4.8 第 7 步的取值（`L/…a8xx_gpu.c:277-366`、`K/adreno_gen8.c:1392-1448`）。`DeviceInfo` 必须报同一组：`highest_bank_bit=16`、`ubwc_swizzle=6`、`macrotile_mode=1`、`ubwc_version=5`。不一致会花屏；上机时可用 `TU_DEBUG=noubwc` 对照。

#### 4.14.4 GMEM 与 sysmem

GMEM 12 MiB（`K/adreno-gpulist.h:2975-2990`：`gmem_size=12*SZ_1M`、`uche_gmem_alignment=SZ_64M`），GPU 窗口 `0x0400_0000` 起，`DeviceInfo.gmem_base=0x4000000`（gen8 的 Turnip 不用 gmem_base 计算地址，`tu_clear_blit.cc:1648-1653`，但必须与内核编程的 GMEM 范围一致）。A8xx 的 framebuffer fetch 首次读前还要失效 CCHE（Turnip 侧问题，`AW/docs/analysis/2026-10-26-gpu-validation-lessons.md:9-11`，已由 5.5 的 G2 处理）；GMEM 路径在 A830 上未验证，首轮上机用 `TU_DEBUG=sysmem,nocb`（经插件环境文件设置，不改代码默认）。

### 4.15 LLCC（M11，`platform/sm8750/Sm8750Llcc.c`，【源一致·未实测】）

- SM8750 使用 LLCC v6（Linux `sm8750_cfg`：`llcc_v6_reg_offset`、`skip_llcc_cfg=false`）；DT `cache-controller@24800000`：4 个通道基址，广播 OR 基址 `0x26800000`、广播 AND 基址 `0x26C00000`（`LDT:7346-7349`）。XNU 里没有 LLCC 驱动，GPU 切片从未启用（R1）。
- 切片：GPU = slice 9，max_cap 5632 KiB，priority 1，fixed_size，bonus_ways 全开，`write_scid_en`、`write_scid_cacheable_en`；GPUHTW = slice 11，512 KiB，priority 1，fixed_size，bonus_ways 全开。
- 每个切片 s 的序列（先读后写；读回不符就放弃该切片，不影响 GPU 启动）：
  0. 版本：读 `0x26800000+0x64000` 应 ≥ `0x06000000`；读 `0x24800000+0x6400C` 的 bits[31:28] 为 bank 数 `nb`（应为 4）。
  1. 属性（广播 OR）：`ATTR0 0x26841000+64s=0`；`ATTR1 (+8)=0xFFFFFFFF`；`ATTR2 (+0x10)=0x1100`（fixed<<8 \| prio<<12）；`ATTR3 (+0x14)=max_cap×1024÷nb>>6`（GPU `0x5800`、HTW `0x800`）；仅 GPU：`WRS_EN 0x26842080` 与 `WRS_CACHEABLE_EN 0x26842088` 置 bit9。已是非零的固件配置保留不动。
  2. 激活（`llcc_update_act_ctrl`，`S/design/perf/dl_llcc/llcc-qcom.c`）：`0x26800000+0x1000s` 写 `0x3`（ACTIVATE<<1 \| TRIG）再写 `0x2` → 轮询 `0x26C00000+0x1000s+4` 的 bit4（ACT_COMPLETE）上限 100 µs → 轮询 `0x26800000+0x1000s+4` 直到 DEACTIVATE 位清零上限 100 µs → 写 `0x26800000+0x1000s+8 = 1`（ACT_CLEAR）。
  3. GPU 侧：4.8 第 9a 步写 `GBIF_SCACHE_CNTL1=0x09249249`、`GBIF_SCACHE_CNTL0=0x02492500`（SCID 9；`K/adreno_gen8.c:1186-1212,1485-1486`），每次 GX 上电都重写。GPUHTW 不需要 GPU 寄存器：TCR 页表遍历属性为外部 WBWA 即命中 HTW 切片（kit `coherent_walk=1` 得到 `0x3500`）。
- 风险：Gunyah 下 EL1 能否写 LLCC 广播寄存器（Linux HLOS 在同一位置直接写，推断可写）；失败只是少了加速。先只读 `TRP_STATUS` 看固件是否已激活其他切片（CPUSS 1、WRCACHE 31）。

### 4.16 DCVS 与带宽（S10 起，`A830Dcvs`）

- **忙度来源（A8xx 变体）**：`POWER_COUNTER_XOCLK_0_H/L 0x3D7F284/0x3D7F280`，先读 H、再读 L、再读 H，H 变了重读 L；单位 19.2 MHz tick（4.7.8 已配置）。注意 sheng 在 A7xx 上用的 `0x1F840/0x1F841/0x1F844`（`SHENG/package/GpuPowerProbe.cpp:1596-1620`）在 A8xx 上是 AO 计数器，不能照搬。
- **调度**：独立内核线程，S10 启动；由显示节拍每 2 个 TE 唤醒一次（120 Hz 下 16.7 ms），没有显示节拍时 16 ms 定时。
- **策略**（默认值，可调）：`busy=Δtick/(Δt×19.2MHz)`；需求频率 = 当前×busy/0.75（2026-10-31 前 0.70）。
  - 升：busy≥0.90 → 至少升 2 档且不低于“需求×1.25”对应档；否则需求高于当前 → 升到“需求×1.1”对应的最低档。设计配置在温度压力下（滤波后的温度不低于目标减 5 °C）两条规则每个窗口都最多升 1 档，减少升到高档再被温控收回的突发（2026-10-30）。
  - 持续（2026-10-31）：上面两条规则都不能把档位推过 734 MHz（g9，`sustainKhz`）；在 g9 或以上连续 12 个窗口（约 200 ms，`sustainWindows`）busy≥0.90 才放行再升 1 档，少一个窗口重新计。已经到达的档位留给降档规则收回。`sustainKhz=0` 关掉。
  - 降：连续 4 窗口（约 67 ms）需求 ≤ 0.85×下一低档频率（2026-10-31 前 0.8）才降 1 档，734 MHz 以上不留这个余量（需求 ≤ 下一低档频率就降：否则工作量 470 被突发推到 832 MHz 后就回不来，需求 627 > 0.85×734）；刚升过档后要求 6 窗口。
  - 空闲：64 ms 无提交且环空 → 回底档（默认 160 MHz，`pagani-gpu-dcvs-min`）。
  - 唤醒提升：空闲后第一次提交至少 525 MHz（g6），保持 50 ms（sheng i560：首帧在 348 MHz 要 30–36 ms）。
  - 触摸提升：PaganiInput 的触摸计数变化 → 底线 525 MHz（g6，2026-10-31 前 607 MHz），保持到最后一次触摸后 300 ms。
  - 提升期间策略档跟随需求上升（2026-10-31）：否则唤醒或触摸提升一结束就掉回底档，接着一串饱和窗口，又是一次突发。
  - 迟到帧提升：显示驱动报告交换晚于目标 TE 且同窗口 busy>50% → 策略档 +1，保持 100 ms，不超过持续规则的上限（2026-10-31 前按当前档 +1，连续迟到帧会一档一档爬到表顶，而迟到帧多半是 CPU 的）。
  - 上限：ACD 未开时 734 MHz（g9）；ACD 开时 900 MHz 或熔丝表最高档（`pagani-gpu-dcvs-max` 显式给出时是硬上限）；再取它与下面温控闭环上限中较低者。
  - 这组默认值的依据（`runs/20261031-223924-stage1-full-1`，PKX110，ACD 开）：旧值 0.70 与 0.8 相乘，稳态 busy 约 55%，等于按 1.8 倍工作量选频：工作量（busy×MHz）约 470 的桌面停在 832–900 MHz、GPU 自己的 DDR 票到第 10 档；旧规则下饱和窗口要“需求×1.25”，等于当前×1.79，525 MHz 起一个窗口就到表顶（日志里每秒采样一次，160→900 出现 8 次、607→900 出现 9 次）。帧率不跟频率走（只看 fps≥60 的秒：300–526 MHz 96.7 fps，761–901 MHz 99.3 fps；525 MHz、busy 85–94% 的秒 83–110 fps），每秒工作量 p50/p90/p99 为 182/466/630，所以 734 MHz 够用。主机测试（`testCompositing`）是工作量模型：182 → 342 MHz，300 → 443 MHz，470 → 734 MHz（从上往下到达时可能多停一档：300 停在 525 MHz，这是 1.1 倍升档余量与 0.85 降档余量之间的回滞）。把这轮每秒工作量回放进真实调频器（窗口内起伏是假设的），734 MHz 以上的时间约 34% → 0。这些都不是上机结果。
  - 目标为什么是 0.75（2026-11-01，第一版 0.85/0.9 被审查改掉）：压掉 832/900 MHz 的是持续规则，不是目标 busy；目标再高只是把平均频率往下压，代价是单帧余量。120 Hz 一帧 8.33 ms，目标 0.85 只留 15%，比均值长 18% 的帧就错过 vsync。真机（这轮 760–1178 s，fps≥60 的 226 秒）平均帧率不随 busy 变，但 ≥115 fps 的秒占比一路下降：busy 0–60% 31/126，60–75% 9/60，75–85% 2/20，≥85% 0/20（相关性，混有 CPU 被压档）。逐帧积压模型（真实 `A830Dcvs` 加这轮工作量，45 °C，窗口内 ±30% 加周期尖峰，起伏是假设的）：旧 70/80 平均 451 MHz、734 以上 21.4%、迟到帧 1.85%、迟超一帧 0.25%；70/80 加持续规则 419 MHz、0.7%、4.20%、0.43%；75/85 加持续规则 398 MHz、0.5%、4.49%、0.52%；85/90 加持续规则 366 MHz、0.4%、6.00%、1.15%。所以 0.75 比 0.85 多花约 30 MHz，迟到超一帧少一半多；也要看到它比旧配置的迟到帧仍然多（主要是持续规则挡住了突发），上机要盯这个。`testFrameHeadroom` 钉住这条：±30% 的帧在 734 MHz 能按目标承载的全部负载下都放得进 TE，且档位不来回跳；同样的扫描用 0.85/0.9 会出现迟到帧。负载超过 550 MHz 时持续规则保持 734 MHz（busy 75–90%），余量让给温度，这一段迟到帧是预期的。
  - DDR 没有收益：主机票默认常驻第 11 档（18.6 GB/s，表顶；`ddr_boot=11`、`ddr_floor=11`，4.3.3），这轮命令行也没有 `pagani-gpu-ddr*`，G 行的 `bN` 只是 GPU 自己的票。【推测】RPMh 多方取最大，所以 GPU 档位降下来不会让实际 DDR 降频，这组默认值不能算 DDR 侧的发热收益。DDR 这条线索要单独做 `ddr_floor` 单变量实验，并先确认显示欠载处理真的生效：`hostFloorKbps` 已是表顶，`A830Dcvs.cpp` 抬底线的分支一直没走过。
- **下发**：只在档位变化时发 `GX_BW_PERF_VOTE {1, g, bus|0xFFFF0000}`，bus 取该档 DT `bus` 值，访存密集时可升到 bus-max；`freq=0xFF` 表示不变、`bw=0xFFFF00FF` 表示不变；`g` 不得为 0（KGSL `gen8_gmu_dcvs_set:1521-1523`）。HFI 全程串行，只在 DCVS 线程里发；失败只计数并保持当前档。GMU slumber 期间只记目标档，下次启动在 M9 中发出。
- **带宽**：DDR 由 GMU 按档位自己投（M2 的 12 级表）；主机票默认保持作者实测的第 11 级（4.3.3），能否降到显示底线由 M11 的单变量实验决定；底线低于启动档之后，MDP 欠载计数增长时临时把主机底线升到第 6 级并记录。可选 AB 投票（M7 返回 1 时）：`bw[31:16]=AB_kBps×65536/(18597656×4)` 夹到 [1,0xFFFE]、`[15:8]=1`（KGSL `gen8_bus_ab_quantize`，`num_ddr_channels=4`）。
- **温度（M11，与 DCVS 同时上线，`platform/sm8750/Sm8750Tsens.c`）**：gpuss-0..7（`LDT:10283` 的 `qcom,tzone-names`）是 TSENS 控制器 tsens2 的传感器 1–8（`LDT:15007-15340` 各温区的 `thermal-sensors=<0xd9 n>`）；tsens2 `reg=<0xc22a000 0x1ff 0xc224000 0x1ff>`、`phandle=<0xd9>`（`LDT:26121-26128`），按 Linux 绑定第一段是 TM、第二段是 SROT。DCVS 每个窗口只读 `TM_Sn_STATUS = 0xC22A000 + 0xA0 + 4n`（n=1..8）：bit21 VALID，`[11:0]` 为有符号的 0.1 °C 值（Linux `drivers/thermal/qcom/tsens-v2.c`：`TM_Sn_STATUS_OFF 0xa0`、LAST_TEMP[11:0]、VALID bit21，单位 deci-Celsius），取 8 个里的最大值。只读，不改 TSENS 的任何配置。annibale 启动期就超过 70 °C 并触发了温控（`AW/docs/analysis/2026-10-26-gpu-validation-lessons.md:21`）。TSENS 读不到（映射失败或 VALID 一直为 0，或曾经有效后连续 8 个窗口无效）时 DCVS 上限固定为 607 MHz。
- **设计配置的温控闭环（2026-10-30，取代原来 85 °C → 607 MHz、95 °C → 389 MHz 的阶梯封顶）**：实测（PKX110，M8 桌面，ACD 开，`pagani-cpufreq=1`）GPU 先冲到 900 MHz，随后热到 84–94 °C，被 85 °C 规则压到 607 MHz、被 95 °C 规则压到 389 MHz（DCVS 行 `160-607`、`160-389`，389 MHz 下 busy 85%），帧率在突发与重降之间大起大落；GPU 传感器在 GPU 只跑 160–389 MHz 时仍有 93–98 °C，热主要来自钉在最高档的 CPU。闭环改为围绕目标温度（`pagani-gpu-ttarget`，默认 95 °C，2026-10-30 晚从 80 °C 放宽）逐档移动上限：
  - 输入：每个窗口的最热传感器，一阶滤波（每窗口 1/4）；每 500 ms 一个趋势窗口，滤波值变化超过 +0.2 °C 为上升、低于 −0.2 °C 为下降，否则平稳。
  - 降：每个趋势窗口最多一次，滤波温度高于目标且不在下降（上升或平稳）时上限降 1 档（从实际生效的上限算起），但不低于 525 MHz（g6，保持桌面流畅的底线）。正在下降时保持，等上一步生效。
  - 升：滤波温度不高于目标减 5 °C 并持续 2 s 才升 1 档，每升一档重新计时；离开这一带就重新计时。目标与目标减 5 °C 之间保持不动（滞回带）。
  - 紧急：TSENS 区 Critical（≥105 °C 进入，低于 100 °C 才离开）时立即降到 389 MHz（g4）并保持；离开后每 500 ms 升 1 档回到底线，再交给闭环。
  - 首个读数（启动或读数恢复）：不高于目标减 5 °C 直接回到最高档；否则从当时的上限（启动时为读不到时的 607 MHz）开始闭环。
  - 主机测试（`tests/test_a830dcvs.cpp`，一阶热模型：各档满载稳态温度 g7 77 °C、g8 81 °C、900 MHz 98 °C，时间常数 1 s 与 2 s）：满载从 45 °C 起，峰值 85.2 / 88.5 °C，之后上限稳定在 607 MHz、77 °C，一档都不再动；两档稳态温度跨过滞回带时（73 / 83 °C）只在这两档之间循环，温度 73.7–80.4 °C；目标 85 °C 时稳定在 660 MHz、81 °C；目标 70 °C 时停在 525 MHz 底线。这些是模型结果，实机曲线待测。
  - 显示：运行页 DCVS 行在上下限之后加 ` T<目标><趋势>`（`?` 无可用读数、`=` 平稳、`+` 上升、`-` 下降），紧急时再加 `!`，例 `DCVS 160-607 T80+ t170/1/8/783/0/0/35/0/0/0/0`（8.4）；上限本身就是 `-` 之后的数字。
  - 与 CPU 的耦合：`adreno830_thermal_state()` 给 PaganiArmCpu 的调速器（`pagani-cpufreq=2`）返回最热传感器（0.1 °C）与标志（有可用读数、温控上限低于旋钮/ACD 上限即“受热限制”、紧急）；GPU 睡眠、恢复或丢失时标志为 0。受热限制时 CPU 域 0 不超过 2745.6 MHz、域 1 不超过 3072 MHz，最后一个触摸帧后 100 ms 内除外（紧急时触摸也不例外）；`adreno830_cpu_hint()` 返回已接受的提交数，变化时 CPU 至少保持中档（表的中间一级）100 ms。两者都是无锁读，PaganiArmCpu 带弱默认值。
  - annibale 配置不变（70/60 °C 规则，见第 11 节第 14 条），也不在 DCVS 行显示温控字段；它的 160 MHz 保持同样算作“受热限制”。

### 4.17 slumber、IFPC、ACD 的分阶段策略

| 阶段 | 内容 | 前提 |
| --- | --- | --- |
| P0（首版） | IFPC 关（HFI 显式发 `FEATURE_CTRL{9,0,0}`）；不进 slumber；CX 与 GMU 常开；GX 在 OOB 清除后仍在电（IFPC 关时 GMU 不收 GX）；空闲靠 DCVS 降到 160 MHz、GPU 总线第 2 级压功耗。注意主机 DDR 票默认保持第 11 级（4.3.3），在 M11 实验允许降档之前，DDR 不会随 GPU 空闲而下降，P0 的空闲功耗高于设计终态 | — |
| P1 slumber | 屏幕静止、无提交满 250 ms 进入（KGSL 约 80 ms、Linux 66 ms）：S1 确认空闲 → S2 OOB set、读 RBBM_INT_0_STATUS 已开放位非 0 则放弃，否则关 GPU 中断掩码、OOB clear → S3 轮询 `RPMH_POWER_STATE==idle_level`（IFPC 关为 0）≤100 ms → S4 GPU RSCC `TCS0..9` 完成（各 2 ms）→ S5 `POWER_COUNTER_ENABLE=0`、`PREPARE_SLUMBER(33){bw=默认档\|0xFFFF0000, freq=默认档}`、`AHB_FENCE_CTRL=0`、`CX_FALNEXT_INTF=1` → S6 等 `GPU_CX_BUSY_STATUS` bit30==0 ≤100 ms → S7 RSCC 睡眠 → S8 GBIF halt → S9 屏蔽并清 GMU 中断 → S10 4.2.3 断电。唤醒走 4.2.1 + SMMU 恢复（若 CX 掉电丢了状态）+ B1–B8（含 RSCC 唤醒、GENERAL_9 回写）+ HFI（缓存的表原样重发）+ OOB 与 hw_init（ZAP 走 `SET_REMOTE_STATE` 恢复，或只发 SET_SECURE_MODE），不再调用 GPU_INIT_REGS。触摸预唤醒把数毫秒的恢复藏在手指移动时间里。备选“CX 保持型”slumber：只做 S1–S9，保留 CX GDSC 与 SMMU 时钟。 | SMMU 恢复路径、ZAP 恢复路径可用；T9 实测 |
| 熄屏规则（`pagani-screenoff` 位 1；位 3 改为完全断电） | 在 P0（或 P1）之上：系统关屏（PaganiPanel 从显示的电源回调发布的 `pagani_screen_state` 位 0）且 GPU 空闲满 `screenIdleMs`（1 s，从关屏与最后一次工作中较晚者算起）进入 slumber，默认 CX 保持型（S1–S9），位 3 走完整 S1–S10；关屏期间触摸不算活动、不预唤醒、不提频，休眠中服务线程不再每个窗口轮询触摸；有提交照常唤醒，空闲 1 s 后再睡；亮屏后显示的第一个刷新节拍唤醒服务线程（最迟一个状态周期 1 s），按触摸档启动 GMU 并保持 300 ms 触摸提频（`A830WakeScreen`）；该规则下的进入或唤醒失败照常交给恢复，并在本次开机停用该规则（`stats.screenBlocked`）。板级参数 `pagani-screenoff` 的其他位属于 CPU 调速器（位 0）与面板（位 2），S0 不做范围检查 | 同 P1：slumber 与唤醒路径待真机单变量验证 |
| P2 IFPC | CP_ME_INIT 带上电恢复表（Linux `a830_pwrup_reglist`、`a830_ifpc_reglist`、`a830_dyn_pwrup_reglist`）；所有 WPTR 写走围栏写；GPU 中断服务用 KEEPALIVE；HFI `FEATURE_CTRL{9,1,0x1680}`；切换需要整个 GMU 重启；围栏下界 0x8A0/0x8C0 实测择一 | P1 稳定 |
| P3 ACD | AOSS QMP（msgram `0x0C300000`，magic `0x4D41494C`、version 1；mcore 邮箱 offset `+0x38`、size `+0x34`）link/channel 握手各 ≤1 s → 64 字节零填充字符串 `{class: gpu, res: acd, val: 1}` 写到 `mbox+4`、长度 64 写 `mbox+0` → IPCC 门铃 `0x0040600C=0`（client 0、signal 0，DT `mboxes=<&ipcc 0 0>`）→ 等长度字归零 ≤1 s；首次 GMU 启动前做一次，然后发 HFI M3。sheng 的 `package/QcomQmp.c` 可复用。开 ACD 后才放开 900 MHz（以及熔丝表的更高档）。QMP 链路本身从 M3 起就作为 S1 开头（上电之前）的可选步骤 `pagani-gpu-qmp=1` 使用，发 `{class: gpu, res: acd, val: 0}`：KGSL 首启的第一件事就是把 ACD 状态告诉 AOP（`KG16/adreno_gen8_gmu.c:1760-1777,1838`），QMP 不可用时它静默跳过（`:1767-1768`），所以不发也是受支持的状态；本机 Gunyah 下 msgram 与 IPCC 能否访问未知，访问被拒是同步外部中止、会直接 panic，因此默认关，M3 单变量验证通过后才改默认为 1；结果（成功/超时/未执行）写入状态块。P3 再在同一链路上发 `val: 1` | Gunyah 下 HLOS 能否直接用 AOSS QMP（M3 可选步回答） |
| 抢占 | 首版不开：每环上下文记录 13536 KiB（`L/…a8xx_catalog.c:1570`）；用准入控制代替：非 backboardd 连接最多 1 个在途作业，而且这个槽是该连接独占的保留槽（5.4），所以不会出现“本进程没有在途作业却收到 Busy”。第 0 阶段（2026-10-31）只计量：队列计量记下每个作业属于渲染服务器还是外部进程，给出每个渲染服务器作业前面排着的外部服务时间 fw（4.13.5），运行页 MEM 行 ` fw<最大>ms/<n4>`、计量行给出完整数字（8.8） | 实测后台作业 >4 ms 造成掉帧时再按 `a8xx_preempt.c` 实现：同一秒里以 off+copy 为主且 n4>0 的慢帧占全部慢帧 20% 以上，或用户指出掉帧的那几秒 fw 最大值超过 4 ms。门槛只采信中断取栅栏的数据（设计配置，或 annibale 配置加 `pagani-gpu-irq=1`；那一秒 IRQ 行是 `q ` 而不是 `q~`，计量行 `poll 0`）：轮询时一遍结算合并多个完成，fw 与 n4 可能偏低，给出相反的结论（4.13.5） |


---

## 5. 性能架构

### 5.1 现状与预算

**现在每帧时间花在哪里**（R1 display/cp/knobs 与 perf 研究，【实测·本机】的是现象，毫秒数为推断）：

1. 显示：CPU 每帧把 12,840,960 字节（1216×2640×4）写进 GOP 缓冲 `0xFC800000`（`LDT` `/reserved-memory/splash_region <0xfc800000 0x2b00000>`）。build 68 用 `kIOMapInhibitCache` 映射（`PD/drivers/NativeMobileDisplay/NativeLegacyDisplay.cpp:485`），该地址在 DarwinBoot 的 4 GiB 窗口外，落到 MAIR Attr3 即 Device-nGnRnE，每帧 160 万次不可合并的 8 字节写，估计 40–150 ms/帧（按 sheng 24.8 MB 用 143–285 ms 折算）。拷贝、退役、60 Hz 软件 vsync 都在同一个 IOMFB 命令门里串行。`native_legacy_gpu_offer` 恒为 false（`PD/drivers/Stage7Probe/Stage7Probe.cpp:108`），GPU 合成到不了屏幕。
2. 作者 A830 候选的 GPU 侧：`native_adreno_io_coherent` 从不为 1，导入的 IOSurface 每次提交整块清洗、每次完成清洗加失效（约 60 ms/帧，源码注释）；`RBBM_INT_0_MASK=0`，等待是 `IODelay(10µs)` + 每轮重拿全局锁 + `health()`；频率固定在初始档（多数 bin 222 MHz）；主机把 CX/MX 与 DDR 投在最高档且永不撤销；没有 LLCC；每次提交在全局锁里做 WC 重设、`publishMapped`、IB/环日志、采样。
3. 已经在做的：`PD` 的 `fast-display` 分支（未提交）把 GOP 映射改为写合并（`pagani-fb-wc`，默认 1）、缓存源 IOSurface 映射（4 项 LRU）、默认关掉前 32 帧扫描、逐帧状态只在开叠加层时画（`Stage7Probe.cpp` 的 `frameStatus`）。这就是 perf 研究的 DF 方案，本设计把它当作基线。

**120 Hz 帧预算（8.33 ms）**：面板 AA594 为 DSC 命令模式，四套时序 `sa_fhd_120/90/60`、`oa_fhd_120`，120 Hz 下传输 6.8 ms（`qcom,mdss-mdp-transfer-time-us=0x1a90`），TE 引脚 GPIO 86（`LDT:41120` 起）。CA 的 CPU 工作 ≤4 ms（不在本设计范围）；GPU 合成约 2–3 ms【推断：sheng 6.19 MP 约 4.4–5 ms，本机 3.21 MP】；GPU 把帧拷进扫描缓冲约 1.6 ms（sheng int-243 实测 24.8 MB 约 3 ms，按字节折算）；DPU 传输 6.8 ms 与下一帧渲染并行。GPU 合计约 4–5 ms < 8.33 ms，120 fps 可达。

上面的预算还没算插件的整面缓存维护：现在每个命令缓冲在 commit 与 finish 时各对涉及的整个 IOSurface 做一次 `sys_dcache_flush`（`NativeBridgeMetalFrontend.mm:483-497,1154,1188`），一个 12.8 MB 的表面一次约 1.5–3 ms【推断：按 dc civac 每秒 4–8 GB 估】；每帧涉及渲染目标与若干源表面、往往不止一个命令缓冲，合计可达 5–30 ms，足以吃掉整个预算。所以 4.14.1 的分方向维护规则（用户态补丁 M8）是 120 fps 的前提；M8 起在插件 Stats 里记录维护的字节数与耗时，作为帧预算的一项单独核算。

**验收指标**（M10–M12）：固定滑动时呈现帧数/TE 数 ≥0.98；每帧 CPU 像素拷贝为 0；每次提交内核开销 ≤20 µs；栅栏完成到等待线程运行 ≤50 µs；空闲时 GPU 在底档；DDR 不再常驻最高档（取决于 M11 降档实验的结论，4.3.3）；插件缓存维护的每帧耗时有实测记录，并与上面的预算合计后仍 <8.33 ms。参照 sheng：空闲 130 fps，延迟均值 4.9 ms、最大 14 ms（`SHENG/porting.toml:163`）。

### 5.2 按影响排序的工作

| 序 | 工作 | 依赖 | 预期收益 | 里程碑 |
| --- | --- | --- | --- | --- |
| 0 | DF：CPU 回退修补（已在 `fast-display`） | 无 | 回退路径 40–150 ms → 2–5 ms/帧【推断】 | M0 合入 |
| 1 | D1：GPU 交接拷贝 + 早退役 + 帧映射缓存 | A830 能跑 Metal | 呈现从 CPU 40–150 ms 变 GPU 约 1.6 ms 且与渲染重叠 | M9 |
| 2 | 零维护一致性规则（4.14） | GPU 可用 | 去掉约 60 ms/帧的缓存维护；不做这项 GPU 路径不会比 CPU 快 | M8 起默认 |
| 3 | DCVS（4.16） | GMU HFI | 合成频率从 222 MHz 提到按需 389–734 MHz，吞吐 2–4 倍 | M11 |
| 4 | D0：TE 锁相 + 上报 120 Hz | 只读 DPU 探针 | 帧周期 16.7→8.3 ms；相位误差 0–16.7 ms → <0.1 ms | M12（可与 GPU 并行先做） |
| 5 | 中断栅栏与无锁等待（4.13） | 332 接线 | 等待时 CPU 占用≈0，唤醒 20–50 µs | M6/M7 |
| 6 | 精简提交与拆锁（5.4） | 无 | 每次提交毫秒级+O(n) → ≤20 µs | M10 |
| 7 | D2：三缓冲 DPU 翻页 | 探针确认 MDP 能取 splash 区 | 无撕裂，拷贝与扫描解耦 | M12 |
| 8 | LLCC（4.15） | LLCC 可写 | 带宽受限通道帧时间降 5–15%，DDR 流量降 10–30%【推断】 | M11 |
| 9 | 主机票降档实验：DDR 底线、LOW_SVS（4.3.3） | DCVS、只读 DPU 探针（欠载计数） | 功耗与温升下降；前提是证明 UFS、CPU 拷贝、MDP 不受影响 | M11 |
| 10 | ACD（4.17） | AOSS QMP | 安全使用 734 MHz 以上档 | M13 |
| 11 | 2 MiB 大页 | 扩展 ArmSmmu4K | 12.8 MB 表面 3135 个 PTE → 7 个块 | M14 |
| 12 | D3：零拷贝直扫 | 控制 apps SMMU 的 MDP 流 | 每帧省 1.6 ms GPU 与约 3 GB/s 带宽 | M14 |
| 13 | slumber、IFPC、抢占 | 系统稳定 | 功耗与尾延迟 | M13+ |

### 5.3 显示扫描输出路径（`PD/drivers/NativeMobileDisplay`、`PD/drivers/Stage7Probe`）

#### 5.3.1 方案对比

| 方案 | 每帧 CPU 像素 | GPU 时间 | 120 Hz 额外带宽 | 撕裂 | 依赖 |
| --- | --- | --- | --- | --- | --- |
| DF：CPU 写合并拷贝（基线） | 2–5 ms | 0 | 约 1.5 GB/s | 有 | 无 |
| D1：GPU 拷进当前扫描缓冲 | 0 | 约 1.6 ms | 约 3.1 GB/s | 有，但比 CPU 路径轻 | A830 + 插件 |
| D2：GPU 拷进后台缓冲 + DPU 三缓冲翻页（终态主线） | 0 | 约 1.6 ms | 约 3.1 GB/s | 无 | 探针确认 MDP 能取 splash 区 |
| D3：DPU 直接扫描 CA 表面 | 0 | 0 | 0 | 无 | 在 apps SMMU 上为 MDP 流建自己的 S1 上下文 |

#### 5.3.2 D1：GPU 交接（复刻 sheng `sheng-fb-gpu`）

1. Stage7Probe：编入 `NativeGOPPresenter.cpp`（`PD/drivers/NativeMobileDisplay/NativeGOPPresenter.cpp`，与 `UX/drivers/GopFramebuffer` 同源）作为 late 对象（`UX/scripts/build-stage7-display.py:201-218` 的对象表加一项，做法同 sheng `build-gop-legacy-display.py:174-179`）；显示启动成功、`sheng-fb-gpu=1` 时 `presenter=native_gop_presenter_create()`、`native_gop_presenter_set_source(presenter, nub)`、attach 到 `IOService::getResourceService()`；`native_legacy_gpu_offer` 改为调用 `native_gop_gpu_offer(presenter, display, memory, stride, width, height)`。
2. `NativeLegacyDisplay` 移植上游 `retireSwap` 早退役（`UX/drivers/GopFramebuffer/NativeLegacyDisplay.cpp:433-443,519-540`）：offer 成功即退役该交换，`transferred` 保持到 `finishGpuSwap`，渲染与拷贝重叠。
3. 驱动侧（本仓库）已具备的接口：AGX 用户客户端的方法 `0x7000–0x7003` 不持 GPU 锁直接转发 `native_gop_handoff_call`，内存类型 `0x5343` 返回 `native_gop_handoff_memory()`（`NativeAdrenoPublish.cpp:47-62,331-334,462`）。需要新增两点：
   - **扫描内存导入特判**：`BoImport` 的用户地址若落在本任务的 `0x5343` 映射内，直接用 presenter 的 `IODeviceMemory` 物理段建 BO（该区在 XNU 内存窗口外、不是受管页，不能 wire 用户页）；GPU 映射 NC、只写。
   - **帧映射缓存**：`NativeGOPPresenter::waitSwap` 现在每帧 `createMappingInTask`（`NativeGOPPresenter.cpp:168`）且 `:176` 把 `out->reserved` 置 0；改为按 `(IOSurface*, IOMemoryDescriptor*)` 在客户端任务里缓存最多 6 个映射（retain、LRU、客户端退出时释放），槽号写进 `NGPSwap.reserved`。插件 `GpuScanoutClient.h` 已把 reserved 当映射号，`GpuScanoutPresenter.h` 的 `sourceFor` 按号缓存 4 个 GPU 导入，于是每帧不再 pin 和映射 12.8 MB。
   - **客户端结束**（2026-10-30）：插件经加速器连接（开类型 0x4148）调用交接方法，presenter 自己的用户客户端那条“结束即 `abandonSwap`”的路径对它不起作用：渲染服务在 WAIT_SWAP 与 COMPLETE_SWAP 之间死掉时，那一帧永远是“已交出”，之后的 offer 全被拒（显示改用 CPU 拷贝），死任务与它的映射也一直被引用。驱动侧现在记下第一次调用交接方法的任务（持一个任务引用），连接关闭时调用 `native_gop_handoff_closed(task)`（弱空默认，`NativeAdrenoPublish.cpp`）；presenter 定义它、对 `frames.owner` 为该任务的情况做 `abandonSwap()` 之后才生效。
4. 插件可选优化：`NBWaitInFlight` 改成“等到渲染已提交”而非“已完成”，单环顺序保证拷贝排在渲染之后，省一次 GPU 空泡与一次用户态往返（约 0.3–1 ms，推断）。

#### 5.3.3 D0：TE 锁相与 120 Hz 上报（不依赖 GPU）

1. **只读探针**（`pagani-dpu-probe=1`，结构照 `SHENG/…/DisplayTimingProbe.cpp`，系统接管显示后约 150 s 运行一次，全程只读），记录：MDSS `0xAE00000+0x10`（HW_INTR_STATUS，bit0 MDP）；MDP 顶层 `0xAE01000` 的 `+0x10 INTR_EN`、`+0x14 INTR_STATUS`、`+0x18 INTR_CLEAR`（INTF1 欠载 bit26、vsync bit27）；INTF1 `0xAE36000` 的 `TIMING_ENGINE_EN +0x0`、`FRAME_COUNT +0xAC`、`LINE_COUNT +0xB0`、`STATUS +0x26C`、`TEAR_MDP_VSYNC_SEL +0x280`、`TEAR_CHECK_EN +0x284`、`SYNC_CONFIG_VSYNC +0x288`、`SYNC_CONFIG_HEIGHT +0x28C`、`VSYNC_INIT_VAL +0x294`、`INT_COUNT_VAL +0x298`（帧[31:16]、行[15:0]）、`START_POS +0x2A0`、`RD_PTR_IRQ +0x2A4`、`TEAR_LINE_COUNT +0x2B0`、`AUTOREFRESH_CONFIG +0x2B4`（bit31 开关、低 16 位每几个 TE 传一帧），隔 1 s 读两次帧计数；CTL0..5（CTL0 `0xAE16000`）的 `TOP +0x14`、`FLUSH +0x18`、`INTF_ACTIVE +0xF4`、`PIPE_ACTIVE +0x12C`、`LAYER_ACTIVE +0x130`；10 个 SSPP（VIG0..3 `0xAE05000/7000/9000/B000`，DMA0..5 `0xAE25000/7000/9000/B000/D000/F000`）的 `SRC_SIZE +0x00`、`SRC0_ADDR +0x14`、`SRC1_ADDR +0x18`、`SRC_YSTRIDE0 +0x24`、`SRC_FORMAT +0x30`、`OP_MODE +0x38`、`MULTIRECT_OPMODE +0x170`；apps SMMU `0x15000000` 中匹配 MDP 流 `0x800/0x801/0x802/0x804`（DT `qcom,handoff-smrs`）的 SMR/S2CR 与对应 CB 的 `SCTLR/TTBR0/TCR/MAIR0/FSR`、`CBAR`（NativeUfs 已在本机做过同类读取，`SOC/kits/Sm8750Platform/SmmuTakeover.h:31-81`）。寄存器地址来源：Linux `dpu_12_0_sm8750.h`、`dpu_hw_{ctl,sspp,intf,interrupts}.c`（`S/design/perf/dl_dpu/`），以 `0xAE00000` 为基换算后与 sheng 的 SM8550 绝对地址一致。探针结果经 BootProgress 的第二组状态行显示（文字行 14–17，新增 `boot_progress_dpu_rows`，机制与 8.5 的 GPU 行相同：只在变化时画、有保留带），便于拍照。
2. **锁相**：启动时 100 ms 内每 50 µs 读一次 `+0x298`，帧字段每变一次记 mach 时间，得 TE 周期 T 与相位；`NativeLegacyDisplay::vsyncTick` 从固定 60 Hz 改为对准“预测 TE + 50 µs”唤醒，每拍读一次 `+0x298` 校验，跳变不符时 5 ms 密集采样重锁；`native_legacy_vsync_notify` 的时间戳用推算的 TE 时刻。
3. **上报**：`getRefreshAttributes`（`PD/…/NativeLegacyDisplay.cpp:215-226`）改用测得的 TE 频率（四舍五入到 120/90/60），保留 fixed。渲染服务器按 `IOMFBDisplayRefresh` 的最小间隔自定节拍（sheng int-209/210），所以上报对了 CA 的上限就从 60 变 120。
4. 若测得 TE≈60 Hz：按 DT `timing@sa_fhd_120` 的 `qcom,mdss-dsi-timing-switch-command` 发 8 个 DCS 包（`0x39 [ff 5a a5 53]`、`0x39 [85 cc e1]`、`0x39 [ff 5a a5 00]`、`0x15 [60 00]`、`0x39 [ff 5a a5 2d]`、`0x39 [83 00 00]`、`0x15 [f2 00]`、`0x39 [ff 5a a5 00]`；60 Hz 版只有 `83 01 00`、`f2 01` 不同），在一个 TE 之后、写指针行计数为 0 或 ≥2640 时发；需要把 `SOC/kits/Sm8750Platform/PeripheralIO.h` 的 `dsi_panel_command`（目前只允许 0x51/0x28/0x29）扩成通用 DCS 短写/长写。
5. **D0b（之后）真 TE 中断**：先保存并清零 MDP `INTR_EN 0xAE01010`、INTF1 `INTR_EN 0xAE361C0`，清 DSI0 `INT_CTRL 0xAE94110` 使能位（状态位仍置位，PaganiPanel 的轮询不受影响，`PeripheralIO.h:86-102`）→ 写 `0xAE36808` 清撕裂状态、`0xAE36800` 置 bit2（RD_PTR）→ `armcpu_spi_attach(115, false, teIrq, …)` → 处理函数读 `0xAE36804`、写回 `0xAE36808`、取 `mach_absolute_time()`、经自定义 IOEventSource 在显示 workloop 上调用 `native_legacy_vsync_notify`；MDSS `HW_INTR_STATUS` 出现 bit0 以外的位或连续 1000 次无 RD_PTR 位就关掉撕裂使能、退回锁相定时。
6. **熄屏**：PaganiPanel 发 `0x28` 后按 Linux `disable_autorefresh` 顺序清自动刷新 bit31（先断外部 TE、清 bit31、等写指针离开 0..2640、恢复 TE），亮屏恢复原值，省掉约 1.54 GB/s 的空转读。

#### 5.3.4 D2：三缓冲 DPU 翻页（终态主线）

- 缓冲：splash 区 `0x2B00000` 正好放三帧：`B0=0xFC800000`（当前 GOP）、`B1=0xFD43F000`、`B2=0xFE07E000`，每帧 `0xC3F000`，末尾 `0xFECBD000 < 0xFF300000`。presenter 的映射扩为覆盖三帧的 `IODeviceMemory`，`mode.bytes` 仍为一帧。
- 协议：`NGP_VERSION` 保持 1、已有结构一个字节都不改（8.5 说明原因）；新增 `NGP_METHOD_WAIT_SWAP2(=5)`（隧道方法 0x7005，内核 `NativeAdrenoHandoffLast` 已在 M9 改为 0x7005）；`NGPSwap2` 是新结构，在 `NGPSwap` 的 48 字节后追加 `uint64_t target_offset`（0、`0xC3F000`、`0x187E000`），共 56 字节并加 `static_assert`；插件先调 `WAIT_SWAP2`，返回 `kIOReturnUnsupported`/`kIOReturnBadArgument` 就退回 `WAIT_SWAP`（单缓冲 D1）；拷贝目的地改为“目标基址+偏移”（`GpuScanoutPresenter.h:57-60` 的 `copyFromBuffer` 只改偏移）。
- 状态机（只在显示 workloop 上访问翻页寄存器）：front（正在扫描）、pending（已冲刷待锁存）、free。offer 时取既非 front 也非 pending 的缓冲为目标；complete 时 `flip(b)`；新帧完成时 pending 还没锁存则邮箱语义直接改写 SSPP 地址为最新帧、旧 pending 回 free。
- `flip(b)`：对探针记录的每个活动管线写 `SRC0_ADDR=B[b]+偏移`（多矩形管线的 REC1 再写 `SRC1_ADDR`）→ `dsb sy` → `CTL_FLUSH` 写这些管线冲刷位的或（VIG0 bit0、VIG1 bit1、VIG2 bit2、VIG3 bit18、DMA0 bit11、DMA1 bit12、DMA2 bit24、DMA3 bit25、DMA4 bit13、DMA5 bit14，`dpu_hw_ctl.c:190-246`）→ 冲刷位清零即锁存（sheng i562 实测约 5.4 ms 内锁存，不需要 `CTL_START`）→ 50 ms 未锁存就写回 B0 并计数。
- 首次写入试验（画面不变）：CPU 把 B0 复制到 B1 → `flip(B1)` → 帧计数继续、欠载 bit26 不变、MDP 上下文 FSR 为 0 → 证明 MDP 能取 B1 → `flip(B0)`。CPU 回退路径只写 B0，回退前先翻回 B0 并等锁存。
- 上屏延迟 = 拷贝完成到下一个 TE（平均 4.2 ms）+ 6.8 ms 传输。

#### 5.3.5 D3：零拷贝（实验，M14）

在 apps SMMU `0x15000000` 上为 MDP 流建自己的 S1 上下文：恒等窗口覆盖 splash 区保证切换瞬间不黑屏，再开 4 个 32 MiB 槽映射 CA 表面（sheng 上游是 4 个 DirectSlot，绝不重映正在扫描或待锁存的槽，`UX/drivers/GopFramebuffer/NativeLegacyDisplay.cpp:119-206`），最后原子改写该流的 S2CR。风险：Gunyah 是否允许 HLOS 改写显示流路由；4 KiB 页下 DPU 实时读的 TLB 压力。

#### 5.3.6 欠载与带宽保护

MDP `INTR_STATUS` bit26（INTF1 欠载）计数一旦增长：主机 DDR 底线临时升到第 6 级（4.3.3）并记录；连续增长则 DCVS 把 bus 档抬到 bus-max。

### 5.4 提交路径与锁

- **预算**：每次提交 ≤20 µs CPU（sheng 实测一帧最多 24 个 IB）。
- **保留**：IB 句柄 O(1) 查表（`NativeAdrenoHandleIndex.h`）、范围与权限检查、作业对 IB 所在 BO 的引用、框架化提交的序言与栅栏。
- **删除**：WC 审计与 `reassertWriteCombined`、一致性采样、`publishMapped`、IB 日志 `peekWords`、环日志复制与整环清洗、`health()`、提交内的 CPU 前序等待。诊断只留几个自增计数，另外每 256 次提交采样一次 mach 时间。
- **WPTR**：保留围栏状态检查（一次 MMIO 读）；200 次重试仍失败就报错。
- **锁**：M8 前保持 `g.lock`（`NativeAdrenoPublish.cpp:130`）但等待不持锁（4.13.3）；M10 拆成三把：每连接一把 `IOLock`（句柄表与窗口）、VM 一把（页表更新与 TLB 同步）、环一把（写环与 WPTR，持有时间几微秒）；栅栏等待无锁；邮箱轮询线程只在 backboardd 连接下运行，且改为事件驱动（R1 knobs：1 kHz 邮箱线程是常驻开销）。各连接共用的 Memory 锁（`NativeAdrenoSubmitLockMemory`）只管缓冲池（池内 BO 的分配与归还）和导入；2026-10-31 起池外 BO（没有池、池满或给渲染服务器留的份额不够时）的连续尝试、零散回退、写合并与释放都不持它（`NativeAdrenoMemory::allocatePooled`：池外分配只碰本对象的页，统计改为原子加），别的 App 一次表面大小的池外分配（int-152：逐页改页属性 90–130 ms）不再卡住 backboardd 的 BoAlloc/BoImport/BoFree。
- **BO 分配路径（2026-10-31，`pagani-gpu-kopt`，都没有上过机，收益未测）**：这几处减少进程之间的串联等待（移植思路来自上游 Adreno740，提交号写在注释里），不改用户可见的行为：
  - 池 BO 的清零（`NativeAdrenoMemory::finishPooled`，位 0）：Memory 锁下只从空闲段表取页，清零放到锁外，页只属于这个还没发布的对象。所有进程的 BoAlloc、BoImport、BoFree 不再等别人一个大 BO 的写合并清零。连接锁仍然全程持有，所以它解开的只是跨进程串联，渲染线程自己的分配等待不变。清零完成之前 `holdForDevice` 拒绝，忘了清零或清零失败的 BO 到不了 GPU；失败时在 Memory 锁下把页还回池。
  - 池外 BO 的连续内存尝试（`contiguousAllowed`，位 1，上游 `01ed692`）：XNU 的 `vm_page_find_contiguous` 在页队列锁和空闲页锁下扫描全部空闲页，一次失败拖慢全系统的缺页。所以只有 64–128 KiB 才尝试；失败后 5 s（按时钟，上游的值，不是我们的实测）内同尺寸及更大的都不再试，更小的仍然试，并把门槛降下去；状态全局共用（`native_adreno_contig_backoff`），因为空闲内存是所有连接的，连接之间没有共用的锁，两个字的竞争最多多搜或少搜一次。调用者要求连续（`contiguous=1`）时不受限。代价：放不进池的大 BO 一律散页，每页一个跨度，靠 VM 跨度表的 `vmr` 计数观察。
  - 表事务的别名检查（位 2，上游 `a087fca`）：preflight 已经对另一类别的所有跨度查过（类别定义和重叠判定与 kit 的 `conflicting_alias` 一致），表事务改走 `ArmSmmu4KLiveMapContextBatchExclusive`，不再排序第二遍。这段在 VM 锁内，而提交持 Ring 锁等 VM 锁。
  - CPU 缓存维护（`NativeAdrenoMaintainRuns`，位 3，上游 `2c3df1e`）：`NativeAdrenoMaintainOwnedPages` 每 64 页关一次抢占，每段自己读 CTR、前后各一个 `dsb sy`，段尾的 DSB 在发出这一段的核上完成，段之间可以被抢占或迁移。之前开机写合并 1024 MiB 池和池外大 BO 都是整段关抢占。
- **作业槽与 Busy（M7 起）**：所有连接共用一个 `NativeAdrenoSubmit`（`NativeAdrenoSubmit.h:42` 的 `jobs_[8]`，`NativeAdrenoPublish.cpp:747`）。Turnip 收到 Busy 时只等本进程最老的栅栏；本进程没有在途作业时只能 `usleep(100)` 重试 64 次（约 6.4 ms）后判设备丢失，Busy 以外的任何错误也直接判设备丢失（`UX/patches/mesa/turnip-darwin-kmd.patch:1099-1127`）。因此：
  - 槽数 44（2026-10-30 晚起，原 24、16）：连接槽位（6.2 的 0–31）各独占 1 个保留槽，其余 12 个共享；
  - **调用者没有在途作业时，内核绝不返回 Busy**：它的保留槽必然空闲；若环空间不够（现有 `submitWords` 在环满时返回 `kIOReturnNoResources`，`NativeAdrenoRing.cpp:283,287`，Turnip 会直接判设备丢失），就不持任何锁、睡在栅栏事件上等空间（M10 拆锁之前也要先放开 `g.lock` 再睡，醒来重新取锁并重查），上限 200 ms，到期才返回 Busy；
  - 有在途作业的调用者在共享槽或环空间不足时返回 Busy，Turnip 会等它自己最老的栅栏再重试；`kIOReturnNoResources` 只留给“单次提交本身就超过环容量”这种不可能满足的请求；
  - 准入控制（代替抢占）：非 backboardd 连接只用自己的保留槽，即最多 1 个在途；backboardd 可以用全部共享槽；
  - 环保持 32 KiB（8192 dword）：单次提交最大约 590 dword（128 个 IB×4 + 序言 11 + 上下文复位 26 + 栅栏 31 + 余量 8，`NativeAdrenoRing.cpp:43,58-60`），能同时容纳 13 个最大提交、50 个以上的典型提交（≤24 个 IB）。
- **回收**：作业引用的 BO 由中断触发的 `retire_call` 在 fence 过后释放，提交路径不再遍历全部作业。
- **大页**（M14）：池 BO 物理连续，GPU 映射支持 2 MiB 块描述符（ArmSmmu4K 目前只有 4 KiB 叶子）。

### 5.5 用户态配合（`docs/inputs/userspace/patches/`）

| 编号 | 改动 | 依赖内核 | 状态 |
| --- | --- | --- | --- |
| M1 | 构建管线：ABI 头目录参数化（`--abi-dir`/`UNIXNU_GPU_ABI_DIR`），`NativeGOPWire.h` 锚点 include，不再生成主机录制的 A740 命令流 `TurnipReplay.h`（`build-scripts-a830.diff`） | 无 | 对 `UX b962645` `git apply --check` 通过 |
| M2 | Mesa 85fda3b 回移 5 个 gen8 修复：7ea5e51c、d286f87b、c88b6ebd（手工）、14656cee、7638039f（`turnip-a8xx-backports.patch`） | 无 | 对 85fda3b 稀疏树 `--check` 通过 |
| M3 | 插件读 bundle 旁 `NativeAdrenoMetal.env`（只接受 `TU_`/`FD_`/`IR3_`/`NIR_`/`MESA_`/`METAL2VULKAN_` 前缀），首轮 `TU_DEBUG=sysmem,nocb` | 无 | 对 AdrenoMetal ce5e19e `--check` 通过；Xcode 27.1/26.6 `-Werror` 语法通过 |
| M4 | 遥测线程与 `reportDisplayState` 只在存在 `NativeAdrenoMetal.diag` 时运行 | 无 | 同上 |
| M5 | Turnip darwin：每进程只开一次连接（`TU_DARWIN_REOPEN=1` 恢复）；`0x44050001`→gpu_id 830 | 无 | `--check` 通过 |
| M6 | IO 一致：`DeviceInfo.flags` 的 `NativeAdrenoDeviceCachedCoherent` 置位时暴露 HOST_CACHED｜HOST_COHERENT 内存类型；只有这一类型的 BO 才 BoMap `writeback=1, shareability=3`、CPU 视图用默认缓存，其余 BO（命令流、描述符、着色器、其他类型的应用内存）仍写合并、非一致，与 msm/KGSL 后端的判定相同（A740 `eed65758`、A840 `5bbeb3b8` 实测 GPU 经 WB+IS 映射写入时停在通道末尾的 CACHE_CLEAN）。`TU_DARWIN_NO_COHERENT=1` 关闭，`TU_DARWIN_COHERENT_ALL=1` 恢复“全部 BO 一致”只供对照 | V9b/V9c | 同上（ABI 头不变：只用已定义的 DeviceInfo 位）；按类型收窄已实现，hostsim 验证 |
| M7 | 已落到环栅栏的等待不在 CPU 上等（`TU_DARWIN_CPU_WAITS=1` 退回） | 单一顺序环 + 每次提交序言同步 BR/BV、尾部清缓存 | 同上 |
| M8 | 插件缓存维护按方向拆开（4.14.1 的规则）：commit 时 GPU 非一致读的源与 GPU 写的目标做 clean，finish 时只 invalidate GPU 写过的表面；内核宣告的一致性等级（AGXAccelerator 属性 `NativeAdrenoCoherentImports=0/1/2`）只免掉对应方向（1 免读源的 clean，2 再免写目标的 clean 与 invalidate）；维护字节数与耗时进 Stats（`NativeBridgeMetalFrontend.mm:483-497,1153-1154,1188`） | 等级 1 需 V9a，等级 2 需 V9b/V9c | 只有设计 |
| M9 | 设备丢失后 backboardd 0.5 s 自退（`adrenometal-5c03327-…patch`） | 无 | `--check` 通过 |
| M10 | Turnip `darwin_bo_finish`（`turnip-darwin-kmd.patch:828-876`）改顺序：先取出退休记录要用的字段并 `memset` 稀疏数组项，再入退休队列、再 `darwin_reap_locked`（同上游 msm 后端“先清项、后关 GEM”的顺序）；改完重新登记补丁身份。现在的顺序是先 reap（可能当场 BoFree 本 BO）、最后才清项，而分配侧按句柄低 32 位查稀疏数组并断言 `gem_handle==0`（`:662-664,720-722`），内核一旦复用句柄，另一线程会写进尚未清零的旧项、随后被清掉 | 内核句柄复用（6.5）的前提 | 只有设计 |
| D1 | `dsb sy`：`tu_EndCommandBuffer` 末尾（`patches/mesa/turnip-a830-fixes.patch`）与 darwin 后端每次 `NativeAdrenoMethodSubmit` 之前（`darwin_submit_chunk`）。命令流在写合并内存里，录制线程与提交线程不在同一核，双簇 gen8 实测 GPU 读到半个 `CP_INDIRECT_BUFFER` 而挂死（pandora `pandora_dsb.py`） | 无 | 已实现 |
| U1–U3 | 上游回移（`patches/mesa/turnip-a830-upstream.patch`，插在 gen8 回移之后）：99fed09b（`tu6_clear_lrz` 补 WFI）、189ee824（预取 `sam.s2en` 保持 helper，`prefetch_sam_helpers_quirk`）、84f09016（谓词段内不用 `alias.tex`，`alias_predication_quirk`，取代环境变量 `IR3_SHADER_DEBUG=noaliastex`）、c503b0a1+a08a0b93（QCTDD13523866 dummy mov）；三个怪癖写成固定版的 props 字段，可用 `FD_DEV_FEATURES=<名字>=0` 关掉 | 无 | 已实现，主机反汇编验证 |
| R1 | `ir3_context_error()` 末尾由 `UNREACHABLE("")` 改为 `abort()`（`turnip-a830-fixes.patch`）：NDEBUG 下前者是 `__builtin_unreachable()` | 无 | 已实现 |
| G2 | A8XX 的 CCHE 失效：同一批刷新里有 UCHE 失效时不再跳过 `CP_CCHE_INVALIDATE`（A7XX 不变），子通道 `feedback_invalidate` 与动态输入附件的刷新带上 CCHE 失效（`turnip-a830-fixes.patch`；annibale A830 实测 framebuffer fetch 读到另一个 GMEM bin 的像素） | 无 | 已实现，rd 转储验证（GMEM 每个 bin 一次） |

### 5.6 度量（只用计数器，不进热路径）

- 显示：呈现帧数、翻页数、锁存数、迟到翻页、锁存时间最大/均值、TE 周期与自动刷新配置、欠载次数、插件 `Stats.lastMs/totalMs/timeouts`（`GpuScanoutClient.h:28-37`）、帧映射缓存命中/淘汰。
- GPU：忙千分比与各档驻留时间（10 s 窗口）、投票次数/失败、中断次数、等待次数/超时、唤醒延迟（中断时间戳到等待者运行）、每 256 次提交采样一次提交耗时、围栏写重试次数。
- LLCC：各切片 `TRP_STATUS` 激活位。
- 插件：缓存维护的字节数与耗时（已有 `gSurfaceFlushBytes`/`gTimeSurfaceFlush`，4.14.1），按帧折算后计入 5.1 的预算。
- 中断：`irq_off`（被风暴保护关掉或没接上的中断），以及各粘滞状态的最近非零值（8.2 `sticky`）。
- 全部写进 `adreno830_status`（第 8 节），叠加层每秒最多刷新一次且只在文字变化时重画。

---

## 6. 保持不变的用户客户端 ABI（AdrenoMetal / Turnip 契约）

### 6.1 发现与属性

- 加速器类 `AGXAccelerator`，继承原厂 `IOAcceleratorES`，实例 136 字节、168 个虚表项（`UX/scripts/build-adreno740-accelerator.py` 的审计条件），attach 到平台专家；插件与守护进程用 `IOServiceMatching("IOAcceleratorES")` 找到它（`turnip-darwin-kmd.patch:386`）。
- 必须的属性：`MetalPluginName=NativeAdrenoMetal`、`MetalPluginClassName=NativeAdrenoMetalDevice`、`NativeSandboxGrantProcess`（逗号分隔，xnu 补丁 `zzw-native-adreno-sandbox-grant.patch` 只比较前 16 字节）。
- 第二个服务 `NativeAdrenoGpuPublished`（名 `NativeAdrenoGpu`，`NativeAdrenoPublish.cpp:749`），不经沙箱的直通通道（`TU_DARWIN_SERVICE=NativeAdrenoGpuPublished` 时用类型 0；但现内核只接受 0x4147/0x4148，注意不要依赖类型 0）。

### 6.2 连接

- 用户客户端类名 `AGXDeviceUserClient`（原厂沙箱配置只允许打开这个类名；`adrenoprobed` 的权利 `com.apple.security.iokit-user-client-class` 也只列它）。
- 开类型：`0x4147` GPU 连接（挂载一个核心）；`0x4148` 只做显示交接（不占槽）；`0x4147xxxx`（xxxx≠0）为进度标记，一律返回 `kIOReturnUnsupported` 且不建立连接（`0x7000–0x70FF` 文本字符、`0x7FFE/0x7FFF` 开新行、`0x8000–0xBFFF` 两个遥测字符、`0xC0rr` 选行，`NativeAdrenoPublish.cpp:574-623`）；其余类型返回 `kIOReturnUnsupported`。
- 2026-10-30 晚起 32 个槽（`NativeAdrenoMaxClients=32`，`NativeAdrenoSubmitRanges=32`、`NativeAdrenoSubmitJobs=44`）：信息 App（MobileSMS）没列进插件名单，拿不到 Metal 设备，Core Image 退回 OpenGL ES，在 `CI::GLContext::init` 跳空指针（`runs/20261030-213341-island-apps-1`），插件名单因此按类开放（`@apps @extensions`，AdrenoMetal `ClientRule.h`）。4 GiB 光圈下表不变（12 个窗口，槽 12–31 没有窗口）；`pagani-gpu-va=16`（`NativeAdrenoClientWindows16G`）：槽 0 同上，槽 1–3 自 `0x80000000` 起各 `0x20000000`（512 MiB），槽 4–31 自 `0xE0000000` 起各 `0x1C000000`（448 MiB；最后一个槽 31 是 `0x1FFFE000`，约 512 MiB，到 `0x3FFFFE000`）。没有窗口跨过 4 GiB 的整数倍（2026-11-01）：槽 4 止于 `0xFC000000`，槽 5、14、23 分别从 4、8、12 GiB 起，每条线下面 64 MiB 不属于任何窗口。地址超过 4 GiB 在 Linux 和上游是常态，单个 BO 横跨这条线没有任何运行证据，而最初的表里开机约 60 s 进槽 5 的 PhotosPosterProvider 用到 64 MiB 就会碰上。选槽（`NativeAdrenoClientSlotFor`，2026-10-31 按审查改为名单内的进程优先）：`NativeAdrenoSandboxGrantList` 列出的小类进程（海报扩展等）先取空闲小槽，小槽满了可以借空闲大槽；名单外的进程（插件按 `@apps @extensions` 放进来的 App 与扩展）只取小槽，而且只在空闲小槽多于 K 个时才取（4 GiB 表的 8 个小槽给名单留 3 个，`NativeAdrenoListedSmallKeep4G`；16 GiB 表的 28 个留 4 个，`NativeAdrenoListedSmallKeep16G`），从不占大槽；没有小窗口的 2 GiB 表只分给名单内的进程。此前 4 GiB 表（默认，也是 16 GiB 的退路）的 8 个小槽可以被 App 占满，名单里的 17 个客户端（海报扩展、图标代理、小组件）又不能借大槽，锁屏海报会因拿不到设备而黑屏。每条连接的对象上限按类（`NativeAdrenoClientObjectsFor`）：渲染服务器与内核客户端 16384，大类 8192，其余 4096（句柄索引按实际容量分配，`NativeAdrenoHandleIndex::allocate(slots)`，CPU 视图表同样按容量），32 条全满约 38 MiB 内核内存；渲染服务器按身份而不是按槽算：重启的 backboardd 在前一个进程的孤儿还占着槽 0 时落进大槽，照样是 16384、不带保留额度（`NativeAdrenoClientTermsFor`，邮箱仍只给槽 0）。渲染服务器以外的连接带保留额度（`NativeAdrenoUserClient::setReserve`）：映射时共享 VM 至少留 8192 条映射记录、32768 个页表区段给渲染服务器（`NativeAdrenoVmRenderReserve*`，VM 映射表 16384→32768），分配时缓冲池至少留四分之一（`NativeAdrenoPoolRenderShare`），不够就退到池外分配（不持 Memory 锁，5.4）；超出保留的映射返回 `kIOReturnNoResources`（`native_adreno_vm_reserve_refused` 计数），只影响那个 App。挂死归责：恢复开始时 `NativeAdrenoSubmit::halt` 给出 CP 停在其作业里的范围（已完成栅栏之后仍在途的最近那个；毒化后来不及回收的已完成作业和 fence 为 0 的毒化提交都不算，没有就不归责），不是渲染服务器就给该进程名记一次挂死，满 `NativeAdrenoClientHangLimit`（2）次后本次开机拒绝它打开（`kIOReturnNotPermitted`），以免一个 App 反复挂死把整机 GPU 拖到第 3 次恢复判死；渲染服务器从不拒绝。只有客户端作业能造成的原因才归责（`A830FaultBlamesClient`：RBBM 挂死检测、CP_HW_ERROR、软件看门狗，恢复闸门带着原因传给 `native_adreno_recovery_begin`）；GMU 故障与看门狗、HFI、WPTR 围栏写、slumber/唤醒、S8 自检是驱动自己的问题，照样停环、给有在途作业的连接记故障，但不记到碰巧在途的进程头上。每个进程名一行的记录表 64 行用满后，新名字接管一个没有活连接、没有挂死记录、打开次数最少的行（`NativeAdrenoClientRecordFor`），挂死次数不会因为表满而不再记、不再拒。沙箱：加速器另带 `NativeSandboxGrantAll=true`，只有打了可选 XNU 补丁 `zzwc-native-adreno-sandbox-grant-all.patch` 时生效（所有进程打开本加速器的用户客户端都免沙箱判定，设置属性不受影响），没有补丁时无作用。
- 槽位拒绝行（2026-10-31，`NativeAdrenoSlotLine`，`adreno830_status_diag_lines()`：`pagani-diag` 非 0、`pagani-gpu-status` 2 或 3、日志在跑）：一行最长约 600 字节、每次开机最多 40 行，没有 UFS 日志的回路镜像放不下，所以和 `gpu sec` 同一个条件。`gpu noslot <进程> <L|l|a> small <空闲小窗>/<保留数>[ +<限流丢掉的行数>] <槽>:[~]<占用者>/<连续无调用秒数>...`，`L` 大类、`l` 名单内、`a` 其他，`~` 孤儿，名字截到 10 个字符。限流：每 5 s 一行，每次开机 40 行（`NativeAdrenoSlotLogDue`）。它回答“谁占着”，以及占着的连接是否已经长时间没有调用。同样的条件下，每次开机第一次映射到 4 GiB 以上时写一行 `gpu va past 4 GiB: <进程> slot <n> iova <映射起点> end <映射终点>`（`NativeAdrenoPast4GLine`，在 BoMap 成功之后、`g.lock` 内），只有 16 GiB 光圈到得了这里。看地址不看窗口：槽 5 从 4 GiB 起，它的客户端第一次映射就过线；导入也走 BoMap，邮箱走同一条 `runCore`，没有绕过的映射路径。窗口表不再允许横跨，所以 `iova` 小于 4 GiB、`end` 大于 4 GiB 的行不该出现。
- 页故障行（2026-11-01，`A830SmmuFaultLine`，下半部 `smmuFaultSink`，`adreno830_status_diag_lines()`，每次开机 GPU 的 CB0 和其他 bank 各 8 条：没有日志统计 CB2 的故障，它有没有良性故障不知道，不能让它吃掉 CB0 的额度）：`gpu fault cb<n> far 0x… fsr 0x… syn 0x… 0x… sid 0x…`。`pf` 只有计数，分不清高 32 位丢失、越出窗口还是页没映射，FAR 能分清，这样 4 GiB 以上出错不用为了看地址再开一次机。
- 共享 VM 的页表区段上限 262144（2026-11-01，原 131072）：16 GiB 表的窗口让 Turnip 的堆变大，metal2vulkan 的块从 4/16 MiB 变成 16/32 MiB，`runs/20261031-223924` 在 12 个客户端时已有 `vmr 37`。
- 槽位与窗口（`NativeAdrenoPublish.h` 的 `NativeAdrenoClientWindows4G/2G`）：2026-10-30 起 12 个槽（原来 4 个：渲染服务器加 WidgetRenderer_Default、PosterBoard、iconservicesagent 正好占满，海报扩展拿不到 Metal 设备，Core Image 退回不存在的 OpenGL ES 就崩：`runs/20261030-144045` 打开壁纸库时 AegirPoster `status=6` 22 次、KaleidoscopePoster `status=b` 17 次）。所有连接共用一个 GPU 地址空间（一个 SMMU 上下文），窗口不能重叠，槽越多窗口越小。2026-10-31 起默认 16 GiB 光圈（上一条的表，`pagani-gpu-va=16`）；下面是 4 GiB 光圈（`pagani-gpu-va=4` 与默认的退路）：槽 0（backboardd，int-196 实测到过 934 MiB）`0x20000000+0x60000000` 不变；槽 1–3 为大窗口，自 `0x80000000` 起各 `0x16000000`（352 MiB），给长驻的 `NativeAdrenoLargeClientList`（WidgetRenderer_Default：R1 在 170 MiB 窗口里小组件空白；PosterBoard；iconservicesagent：int-198 用到 128 MiB）；槽 4–11 为小窗口，自 `0xC2000000` 起各 `0x07C00000`（124 MiB，最后一个短 8 KiB，整表仍止于 `0xFFFFE000`），给海报扩展等其余进程。选槽（`NativeAdrenoClientSlotFor`）：backboardd 取槽 0；大类进程（及 backboardd 的第二条连接）先取空闲大槽，再取小槽；名单内的小类进程先取小槽、再借大槽，名单外的进程只取小槽并给名单留 3/4 个（见上一条）；槽 0 永远不给别人。`pagani-gpu-va=2` 时退回旧表：槽 0 `0x20000000+0x40000000`，槽 1–3 `0x60000000/0x6AAA9000/0x75552000` 各 `0x0AAA9000`，槽 4–31 没有窗口、不会分出。每槽 8 个队列，即提交对象的一个范围（`NativeAdrenoSubmitRanges=32`）。各槽窗口互不重叠，SMMU 页故障据此按 FAR 归属到连接（故障时在途的作业只与 FAR 窗口对比计数，不据此扣连接，4.12.3）。`GetDeviceInfo` 返回的 `va_start/va_size` 会被替换成本连接的窗口（`NativeAdrenoUserClient::deviceInfo`）。每条连接打开时按它的对象上限分配句柄表（16384 个对象时条目 200 字节 3.1 MiB，加 FIFO 64 KiB 与句柄索引 512 KiB，约 3.7 MiB 内核内存；大类 8192、其余 4096 按比例减少；连接关闭即释放），32 条全满约 38 MiB。
- 上限：取消“非 backboardd 进程每次开机累计 8 次”的规则（`NativeAdrenoPublish.cpp:689`），只限并发连接数（R1：WidgetRenderer 每次开机最多 4 次启动能拿到 GPU，Turnip 每进程打开两次）；`sheng-gpu-metal-opens`（加速器默认 40）防的是崩溃循环，2026-10-30 起只数“该进程名上一次健康连接（活过 60 s）之后”的打开次数（`native_adreno_client_streak`），不再按整次开机累计：按开机累计时，backboardd、WidgetRenderer 正常重启二十来次之后就永久拿不到 GPU、只能 CPU 渲染。
- 被拒的打开（外来类型 1、上限、没有空槽、设备丢失、挂载失败）先从提供者上 `detach` 再释放：以前只 `release`，提供者的子链接让对象留到关机，加速器的子节点超过 1019 个后每次 `IOService::attach` 都会不可中断地停 15 s 再失败（XNU `kIOServiceBusyMax`）。
- 关闭连接（`clientClose`）最多等本连接的作业 2 s，再逐个释放对象；释放不了的对象留下，其余照样释放（`NativeAdrenoUserClient::releaseAll`，以前第一个失败就停下，后面的对象留到关机），这样的核心成为孤儿占着槽位。孤儿在每次挂载时、恢复结束时和每秒一次（DCVS 线程，GPU 醒着且不在恢复中）重试拆除：以前只在“挂载找不到空槽”时才重试，backboardd 崩溃留下的槽 0 孤儿会把它重启后的连接挤到 682 MiB 的窗口，死进程的 BO 与导入一直不放。
- CPU 视图（`clientMemoryForType` 的每 BO 描述符）：最后一个 `BoCpuUnmap` 撤掉视图与全部用户映射，邮箱路径同样处理；`BoFree` 成功后再撤一次该 token 的视图（正常路径上已经没有），保证没有用户映射活过它的物理页，复用同一 token 的新 BO 也拿不到旧视图。

### 6.3 外部方法（选择子 1–16，结构尺寸由 `NativeAdrenoABI.h:147-163` 的 `static_assert` 固定）

| 选择子 | 方法 | 输入 | 输出 |
| --- | --- | --- | --- |
| 1 | Query | — | 24（`abi_version=2`、`input_bits`、`output_bits`、`flags=HostImport`、`max_bo_bytes=1<<28`） |
| 2 | BoAlloc | 16（字节为 4K 倍数且 ≤256 MiB；`align_log2` 12..24；`contiguous` 0/1） | 8（非零句柄） |
| 3 | BoMap | 32（客户端自选 4K 对齐 IOVA，须在窗口内；`permissions` bit0 读必需、bit1 写；`writeback` 0/1；`shareability` 2/3；`reserved` 0） | 8 |
| 4 | BoFree | 8 | — （BO 在飞行时可返回 `kIOReturnBusy`） |
| 5 | Submit | 3104（32 + 24×128） | 8（非零单调 fence，Turnip 只用低 32 位）。只对已有在途作业的调用者返回 `kIOReturnBusy`；没有在途作业的调用者使用自己的保留槽，环空间不足时在内核里不持锁等待（≤200 ms），见 5.4 |
| 6 | WaitFence | 16（`timeout_ns=0` 只查一次） | — （未完成或线程被中断都返回 `kIOReturnTimeout`=`0xE00002D6`；设备丢失 `kIOReturnNotResponding`；其他错误 Turnip 一律判设备丢失，4.13.3） |
| 7 | GetTimestamp | — | 8（19.2 MHz 常开计数） |
| 8 | GetStatus | — | 16（`faults`、`suspends`，单调；`faults` 变化即上下文丢失。M7 起 `faults` 按连接计数：页故障只加到出错的连接，全局故障加到所有连接，4.13.4） |
| 9/10 | Set/GetMetadata | 80 | —/80（每对象最多 64 字节） |
| 11/12 | Create/CloseQueue | 8 | 8/— |
| 13 | BoCpuMap | 8 | 16（`token`=clientMemoryForType 类型号、`bytes`） |
| 14 | BoCpuUnmap | 16 | — |
| 15 | GetDeviceInfo | — | 64 |
| 16 | BoImport | 16（地址、字节均 4K 对齐） | 8（固定调用方自己的页，无 CPU 映射 token） |

- 隧道：输入以 `{u32 0x4E544E4C 'NTNL', u32 方法号, 负载}` 开头时可经任何被沙箱放行的选择子调用，输出为该方法的精确输出；方法号 `0xFFFFFFFF` 是探测，直接成功（Turnip 依次探测 0..255，`turnip-darwin-kmd.patch:363-375`）。最大输入 3112 字节 < 4096，仍走内联；若将来把 `NativeAdrenoAbiMaxBuffers` 加到 168 以上，内核必须改为处理 `structureInputDescriptor`。
- 非 ABI 方法号（只经隧道）：`0x7000–0x7003` 转发 `native_gop_handoff_call`（`NGP_METHOD_MODE/PRESENT/WAIT_SWAP/COMPLETE_SWAP`，`NativeGOPWire.h:12`）；M9 起加 `0x7004`（`NGP_METHOD_BAND`，读保留带）、M12 起加 `0x7005`（`NGP_METHOD_WAIT_SWAP2`，带目标偏移的取帧），内核的转发上限 `NativeAdrenoHandoffLast` 随之由 0x7003 改为 0x7005（`NativeAdrenoPublish.cpp:51,461`；不改的话 0x7004/0x7005 根本到不了显示驱动）；`0x7100` 导出，输入 `{generation,total,offset,bytes}` 后跟数据，单片 ≤3968 字节，只接受 backboardd。
- 内存类型（`clientMemoryForType`）：BO token（句柄表索引+1）→ 物理范围描述符，`kIOMapAnywhere|kIOMapInhibitCache`（客户端传 `kIOMapWriteCombineCache` 时最终为 Normal-NC；内核必须为每个 token 缓存一个视图描述符，否则 `IOConnectUnmapMemory64` 变空操作、BoFree 后 pmap 崩溃，`NativeAdrenoPublish.cpp:150-160`）；`0x5343` 扫描输出；`0xC0DE` 4 KiB 邮箱（只给 backboardd 槽 0，布局 `+0 magic 0x4D41494C`、`+4/+8` 请求/应答序号、`+12` 方法、`+16` 状态、`+20/+24` 输入/输出字节，输入在 `+0x40`、输出在 `+0x800`，各 ≤0x700）。

### 6.4 A830 的 DeviceInfo 取值

| 字段 | 值 | 依据 |
| --- | --- | --- |
| `chip_id` | `0x44050001` | DT `qcom,chipid`；Turnip 设备表精确匹配（Mesa 85fda3b `freedreno_devices.py:1353-1371`） |
| `gmem_bytes` | `0x00C00000`（12 MiB） | KGSL gen8_0_1、Linux `a8xx_catalog.c:1542`；决定 CCU 在 GMEM 中的切分，必须如实 |
| `gmem_base` | `0x04000000` | KGSL `uche_gmem_alignment=SZ_64M`；须与 4.8 第 4 步一致 |
| `uche_trap_base` | `0x0001FFFFFFFFF000` | 进入 Turnip pipeline cache UUID 并传给 ir3，须与 4.8 第 5 步一致 |
| `va_start` / `va_size` | `0x20000000` / 默认建成 16 GiB 光圈时 `0x3E0000000`，4 GiB 光圈（`pagani-gpu-va=4` 或退回）时 `0xE0000000`，`pagani-gpu-va=2` 时 `0x60000000` | 每连接被窗口覆盖；发布外壳按它选窗口表（`NativeAdrenoClientWindowsFor`） |
| `highest_bank_bit` | 16 | 4.14.3 |
| `ubwc_swizzle` | 6 | 同上 |
| `macrotile_mode` | 1（8 通道） | 同上 |
| `ubwc_version` | 5 | 同上（Turnip 不读该字段） |
| `flags` | `NativeAdrenoDeviceClientIova`（V9b/V9c 通过且 `pagani-gpu-coherent=2` 时再加 `NativeAdrenoDeviceCachedCoherent`） | 4.14.2 |
| `reserved` | 0 | — |

Query：`input_bits=32`（16 GiB 光圈时 34，`NativeAdrenoGpuContext::inputBits()`）、`output_bits=36`（IDR2.OAS）。Turnip 不读这两个值，但内核用 `output_bits` 限制 BO 的物理地址（`NativeAdrenoUserClient.cpp:200-237`）。

### 6.5 Gen8 下内核必须替用户态完成的事

- 非上下文寄存器：Turnip 在 gen8 上不写，CP_PROTECT 还会拦它（Mesa `tu_cmd_buffer.cc:2274`）。内核每次 GPU 上电、恢复后都写 40 项非上下文表，并放进上电恢复表与 IFPC 恢复表。
- CP_PROTECT 不能拦截 Turnip 每个命令流开头写的 24 项 raw magic 上下文寄存器（`GRAS_BIN_FOVEAT_XY_FDM_OFFSET+0..5`、`RB_UNKNOWN_8818..881E`、`RB_LRZ_CNTL2`、`RB_RESOLVE_CNTL_5`、`SP_UNKNOWN_AB01`、`SP_HLSQ_MODE_CNTL`、`SP_UNKNOWN_AB23`、`TPL1_PS_ROTATION_CNTL`、`TPL1_PS_SWIZZLE_CNTL`、`PC_UNKNOWN_980B`、`PC_MODE_CNTL`，`tu_cmd_buffer.cc:2159-2186`）；KGSL 的保护表满足这一点。
- 并发 resolve 只能由内核的 `RB_CCU_CNTL=0x68`（非上下文表）决定，`TU_DEBUG=noconcurrentresolves` 在 A830 上无效。
- UBWC 参数与 DeviceInfo 一致；UCHE trap/GMEM 范围与 DeviceInfo 一致。
- 每次提交前后包（`NativeAdrenoRing::emitPrologue`/`emitFramedFence`，与 Linux `a7xx_submit` 一致，两种启动配置相同）：开头 `CP_THREAD_CONTROL(SYNC_THREADS|SET_THREAD_BR)`；客户端切换时接着是 BR/BV 上下文复位入口（`CP_WAIT_TIMESTAMP`、`CP_RESET_CONTEXT_STATE`、LRZ 冲刷，两条 SRAM 初始化写之后 `CP_WAIT_REG_MEM` 等 0x19F；Adreno740 提交 c285ae8 的做法，`NativeAdrenoRing::useContext`）；然后 `CACHE_FLUSH7`、`CACHE_INVALIDATE7`、`CP_WAIT_FOR_IDLE`，再 `CP_THREAD_CONTROL(SET_THREAD_BOTH)`、`CP_SET_MARKER(0x101)`（关 IFPC），最后逐个 `CP_INDIRECT_BUFFER`。所以用户 IB 前生效的线程是 BOTH，不是 BR。结尾：BR 作废 CCU 深度与颜色缓存，BV 用 `CACHE_CLEAN` 把时间戳写到 memptrs+12，BR 用 `CP_WAIT_TIMESTAMP` 等它，再用带 IRQ 的 `CACHE_CLEAN` 把 fence 写到 memptrs+4，线程回到 BOTH，`CP_SET_MARKER` 重新允许 IFPC（Turnip 依赖“内核在每次提交末尾清 UCHE”，`tu_cmd_buffer.cc:5307-5317`）。
- 常开计数器读 GMU AO（不是 A7XX 的 0x980/0x981）。
- 句柄：Turnip 取句柄低 32 位当稀疏数组下标（每节点 512 项）；现用 `next_handle_++`（`NativeAdrenoUserClient.cpp:45`）开机越久越大、数组只增不减。目标编码是“低 32 位 = 槽号+1（≤16384）、高 32 位 = 代数”，句柄在 ABI 上本就不透明。但槽号一复用，Turnip 现有的释放顺序就有竞态：`darwin_bo_finish` 先入退休队列并 `darwin_reap_locked`（可能当场对本 BO 调 BoFree），最后才 `memset` 稀疏数组项（`turnip-darwin-kmd.patch:828-876`）；分配侧按低 32 位查稀疏数组并断言 `gem_handle==0`（`:662-664,720-722`）。另一线程拿到刚释放的槽号会写进尚未清零的旧项，随后被清掉，活 BO 的句柄变成 0，提交被拒、设备丢失。现行的单调句柄下不存在这个竞态。所以切换分三步（M10）：① 用户态补丁 M10（5.5）先改 Turnip 的顺序并重新登记补丁身份；② 内核槽位按 FIFO 回收并隔离，一个槽至少经过 4096 次分配才复用；③ 两者都在设备上跑过之后才切换编码。
- `GetStatus`：`faults` 按连接计数（M7 起），一个小组件进程的页故障不应让 backboardd 一起丢设备（4.13.4）。


---

## 7. 构建集成与 13T 设备配置

### 7.1 `Adreno830` 组件

#### 7.1.1 目录项（`UX/components.toml`，加在 Adreno740 条目之后）

```toml
[[component]]
id = "Adreno830"
kind = "driver"
function = "A830 (Gen8) kernel GPU driver"
scope = "gpu"
repository = "https://github.com/<所有者>/Adreno830.git"   # 待定（9.2 未决问题）
revision = "<提交>"
path = "drivers/Adreno830"
hardware = ["Adreno 830", "SM8750"]
os_abi = ["ipados-17.7.11-21H461"]
board_dependencies = ["GMU/SQE/AQE and signed ZAP firmware", "GPU power policy", "reserved memory", "display hand-off"]
verified_devices = []
dependencies = ["Sm8750Platform", "AdrenoPM4", "ArmSmmu4K"]
source_files = ["NativeAdrenoPublish.cpp", "NativeAdrenoAccelerator.cpp", "Adreno830Start.cpp"]
runtime_identifiers = ["NativeAdrenoABI.h", "AGXAccelerator", "AGXDeviceUserClient", "NativeAdrenoGpuPublished", "NativeAdrenoGpu", "native_adreno_publish", "adreno830_start", "pagani_gpu_start", "adreno830_status"]
build_stage = "kernel"
build = ["{component}/scripts/build-adreno830.py"]
outputs = ["build/adreno830/libAdreno830.a"]
```

`kernel.py:131-141`：`{component}` 被替换成模块的真实路径，绝对路径脚本直接执行，`cwd` 是 tree，所以脚本里 `ROOT=Path.cwd()` 并检查 `ROOT/'components.toml'` 存在。同时把 AdrenoMetal 的依赖（`UX/components.toml:101-120`）从 `["Adreno740","AdrenoPM4"]` 改为 `["AdrenoPM4"]`，否则 13T 一选 AdrenoMetal 就会带进 Adreno740 与 Sm8550Platform，`archive_plan` 随即要求 `component:Adreno740`（`kernel.py:91,116-117`）；sheng 在自己的 include 与 archives 里显式列出 Adreno740，不受影响。

#### 7.1.2 `scripts/build-adreno830.py`（本仓库）

1. 读取 XNU 编译记录：C++ 用 `build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json`，C 用 `…/bsd/DEVELOPMENT/bsd_init.o.json`；不存在则报错并提示先跑 `XNU_BOOTSTRAP=1` 的引导构建（`kernel.py:167-182`）。处理方式照 `UX/scripts/build-arm-driver.py:83-100`：换源文件与 `-o`，去掉 `-W*`、`-MD/-MP/-MF/-MJ`，加 `-Wall -Wextra -Werror -Wno-unused-parameter -Wno-c99-designator -Wno-reorder-init-list -Wno-deprecated-enum-enum-conversion -Wno-unnecessary-virtual-specifier` 与 `-I .cache/xnu-work/osfmk`。
2. 编译：按脚本内的**显式源文件清单**（不用通配，避免把只给主机用的 `NativeAdrenoClient.cpp`、`NativeAdrenoBoard.cpp` 或待删除的 A7xx 文件带进内核；`NativeAdrenoAccelerator.cpp` 走第 3 步）编 `drivers/Adreno830/` 下的 C++ 与 `platform/sm8750/` 下的 C，加上 `kits/AdrenoPM4/AdrenoPM4.c`、`kits/ArmSmmu4K/ArmSmmu4K*.c`。头文件路径 `-I drivers/Adreno830 -I drivers/Adreno830/platform/sm8750 -I kits/ArmSmmu4K -I kits/AdrenoPM4 -I kits/Sm8750Platform -I drivers/AdrenoMetal`。
3. 分阶段加速器（照 `UX/scripts/build-adreno740-accelerator.py`）：读 `scripts/data/ioaccelerator-base-abi.json`，要求 `instance_bytes=136`、`virtual_entries=168`；生成 `original-bindings.S`；`llvm-objcopy --remove-section=__DATA,__mod_init_func --globalize-symbol=__GLOBAL__sub_I_NativeAdrenoAccelerator.cpp`；确认 `__text` 与原对象一致。`native_prelink` 经 `PYTHONPATH` 导入（13T 上是 `kits/NativePrelink`，`kernel.py:127-129`），不写死路径。
4. 先删除旧归档（`archive.unlink(missing_ok=True)`，照 `UX/scripts/build-arm-driver.py:119-121`），再 `xcrun ar rcs build/adreno830/libAdreno830.a …`。不删的话，`ar r` 只替换同名成员，M4、M6 删掉的 A7xx 目标文件会一直留在归档里，并被 `-force_load` 链进内核；第 5 步的 `llvm-nm` 检查同时断言归档成员清单与显式源文件清单一一对应。
5. `llvm-nm` 检查：必须定义 `_adreno830_start`、`_adreno830_status`、`_native_adreno_publish`、`_native_adreno_new_user_client`、`___GLOBAL__sub_I_NativeAdrenoAccelerator.cpp`、`_native_adreno_accelerator_allocate`、`_native_adreno_accelerator_instance_size`、`_Sm8750GpuPowerOn`；必须是**弱定义**：`_native_gop_handoff_call`、`_native_gop_handoff_memory`、`_boot_log_export_put`、`_boot_progress_gpu_rows`、`_native_adreno_firmware_image(_size)`、`_native_adreno_sqe_firmware_image(_size)`、`_native_adreno_aqe_firmware_image(_size)`、`_native_adreno_zap_image(_size)`；不得出现 `ShengGmu*`、`NativeAdrenoGpuService`。
6. Mach-O 审计（`build-arm-driver.py:108-118`）：magic `0xfeedfacf`、CPU `0x100000c`、`MH_OBJECT`、`LC_BUILD_VERSION` 为 iOS、最低 `0x110000`；写 `build/adreno830/build-report.json`（源文件 sha256、归档 sha256）。

#### 7.1.3 设备脚本 `DEV/scripts/build-pagani-gpu.py`

编译 `kits/oneplus13t/PaganiGpuBoard.cpp`（`-I kits/oneplus13t -I drivers/Adreno830 -I kits/Sm8750Platform`）；生成 `firmware.S`：每个固件 `.p2align 12` + `.incbin`，导出 `_native_adreno_firmware_image`（GMU）、`_native_adreno_sqe_firmware_image`、`_native_adreno_aqe_firmware_image`、`_native_adreno_zap_image` 及各自 `_size`；先按 `scripts/data/sm8750-gpu-inputs.json` 校验长度与 sha256；`ar rcs` 追加进 `build/adreno830/libAdreno830.a`；检查 `_pagani_gpu_start` 已定义（强）。做法照 `SHENG/scripts/build-adreno740-kits.py:50-66`。CI 没有固件时 `--no-firmware` 只链 PaganiGpuBoard，运行时在 S0 拒绝。

固件钉选（`DEV/scripts/data/sm8750-gpu-inputs.json`；本地文件放 `DEV/.cache/firmware/pkx110-gpu/`，经 tree 的 `.cache/firmware` 链过去，`UX/scripts/unixnu/tree.py:56-62`；**固件绝不入库**）：

| 文件 | 字节 | sha256 |
| --- | --- | --- |
| `gen80000_gmu.bin` | 88896 | `1ad8175e6cd01ea76c64d20b83600038c52dd29844b48d57b11b769b038863a5` |
| `gen80000_sqe.fw` | 111892 | `9c8f009bea112d7c06dd2ccb67e889999459e53b92f0f4859f5ea0db067ad6b5` |
| `gen80000_aqe.fw` | 25340 | `f04cb97e1e629b9003a26c5670b8f7e5b7bfeca95d35b2abd581fd8672efed20` |
| `gen80000_zap.mbn` | 12088 | `1aa7af120c4eda02a372c6b193f4eae9272d92e2494300a1b9bb0d5ad0d93c43` |

（来源：`/Users/xiaochoumao/unixnu-13t/一加13T原厂备份/GPU固件/`，本次 `shasum -a 256` 与 `stat` 复核。）

### 7.2 UniXNU（`UX/`，本地分支）

| 文件 | 改动 |
| --- | --- |
| `components.toml` | 新增 Adreno830 条目；AdrenoMetal 依赖去掉 Adreno740 |
| `scripts/build-xnu-4k-baseline.sh:636` | `ADRENO_ARCHIVE=${XNU_NATIVE_ADRENO_ARCHIVE:-$ROOT/build/adreno740/libAdreno740.a}`（与 UFS、AES 的覆盖写法一致） |
| `scripts/build-xnu-4k-baseline.sh:782` | 重链判据里的 `"$ROOT/build/adreno740/libAdreno740.a"` 同样改为 `${XNU_NATIVE_ADRENO_ARCHIVE:-…}`，否则归档变了不触发重链 |
| `scripts/build-turnip-darwin.py:42`、`scripts/build-adreno-metal-plugin.py:41,84`、`scripts/check-turnip-darwin-host.py:14`、`scripts/check-turnip-pipeline-cache-host.py:58` | ABI 头目录参数化：存在 `drivers/Adreno830` 就用它，否则 `drivers/Adreno740`；两者都在时断言两个 `NativeAdrenoABI.h` 字节相同（`docs/inputs/userspace/patches/build-scripts-a830.diff`） |
| `scripts/build-stage7-display.py:201-218` | 对象表加入 `NativeGOPPresenter.late.o`（M9），并把它加入 `:219` 起的源哈希列表 |
| `patches/mesa/turnip-darwin-kmd.patch:1334-1341` | 加 `0x44050001`→830（只影响名字，设备表按 chip_id 匹配）；改完按 `save-turnip-darwin-patch.py` 重新登记补丁身份 |
| `patches/mesa/turnip-a8xx-backports.patch`（新增）、`scripts/data/adreno-compiler-inputs.json` | 回移补丁插到 `turnip-darwin-kmd.patch` 之前 |
| `scripts/add-gpu-plugin-data.py` | `--env`、`--prime-list`、`--diag`（`add-gpu-plugin-data-a830.diff`） |

开发期用本地覆盖，不改 pin：`./unixnu … --local-module driver:Adreno830=../Adreno830`；`DEV/local.toml` 按 `local.toml.example` 写 `[[source]] name="sm8750" kind="soc" local="../UniXNU-sm8750-common"` 与 `[local] drivers="../UniXNU-Pagani-Drivers/drivers"`。

### 7.3 设备仓库（`DEV/`）

先把 GPU 工作树切到主线：`git -C DEV switch -c a830-driver oneplus13t-512g`（工作树现在游离在 `3c39baf`，缺 `830e214 Pin disabled SM8750 GPU resource tree` 与 512 GB 机型的 `userdata` LBA；主线工作区 `board.toml` 里 512 GB 的 LBA 改动尚未提交，要一并带上）。

**`board.toml`**：

```toml
[components]
include = [ …现有 18 项…, "Adreno830", "AdrenoMetal" ]   # AdrenoMetal 从 M0 起就要选中：加速器源码包含 tree 里的 drivers/AdrenoMetal/OriginalAcceleratorABI.h

[kernel]
archives = [ …现有…, "component:NativeMobileDisplay", "component:Adreno830", "build-pagani-gpu.py" ]

[kernel.prelink]
roots = [ …现有…, "com.apple.iokit.IOGPUFamily" ]   # 13T 的 roots 整体覆盖了 OS profile，现在没有它，publishMetal 必失败

[payload]
size_mib = 192    # Data 种子里多了插件（约 22 MB）与 Turnip（约 17 MB）；作者用过 160/192
kernel_args = [ …现有，去掉 "pagani-platform-status=1"（每帧关中断重画两行，R1 knobs）…,
                "pagani-gpu=1", "sheng-gpu-addon=1", "sheng-gpu-metal-opens=0" ]   # M9 再加 "sheng-fb-gpu=1"
```

逐里程碑的临时参数用 `./unixnu payload … --kernel-arg NAME=VALUE`，不改 `board.toml`。

**`configs/oneplus13t_defconfig`**：`:31` 的 `# CONFIG_XNU_NATIVE_ADRENO is not set` 改为 `CONFIG_XNU_NATIVE_ADRENO=y`；新增 `CONFIG_XNU_NATIVE_ADRENO_ARCHIVE="{tree}/build/adreno830/libAdreno830.a"`；保留 `CONFIG_XNU_NATIVE_LEGACY_DISPLAY=y`；`CONFIG_XNU_OWNER_DATA_VOLUME_UUID` 按实际 Data 种子设置（文件注释已要求）。

**`dts/oneplus13t.dts`**（节点都没有 `unixnu,adt-name`，ADT 与 arm-io 不变；只作为生成器的唯一来源）：

```dts
/ {
    reserved-memory {
        #address-cells = <2>; #size-cells = <2>; ranges;
        aop_cmd_db_mem: cmd-db@81c60000 { compatible = "qcom,cmd-db"; reg = <0 0x81c60000 0 0x20000>; no-map; };
        smem_mem: smem@81d00000 { compatible = "qcom,smem"; reg = <0 0x81d00000 0 0x200000>; no-map; };
        gpu_microcode_mem: gpu-microcode@9b09a000 { reg = <0 0x9b09a000 0 0x2000>; no-map; };
    };
};
&gpu { status = "okay"; qcom,chipid = <0x44050001>; qcom,gpu-model = "Adreno830v2"; memory-region = <&gpu_microcode_mem>; };
&gmu { status = "okay"; };
&gpucc { status = "okay"; };
&gpu_smmu { status = "okay"; };
&qfprom { status = "okay"; };
```

**新文件**：

| 文件 | 内容 |
| --- | --- |
| `scripts/prepare-pagani-gpu-inputs.py` | 用 `dtc`+`fdtget` 读 DTS（A 类）与钉选 `live.dtb`（B 类，先校验 sha256），生成 `package/PaganiGpuInputs.h`；`--check` 只比对 |
| `package/PaganiGpuInputs.h` | 生成并入库：3.2 的窗口、中断（INTID=SPI+32）、SID、chip id、保留区、qfprom 偏移、6 组档位表、共有 11 档、DDR/CNOC 表、GMU 频率表、UBWC、ACTLR |
| `package/PaganiGpuBoard.cpp` | `pagani_gpu_start()` 强定义：填 `Adreno830Board`、解析启动参数、同步/异步调用 `adreno830_start` |
| `scripts/build-pagani-gpu.py` | 7.1.3 |
| `scripts/test-gpu-inputs.py` | 校验生成头与 DTS、live.dtb 一致（接入现有 `test-*.py` 体系） |
| `scripts/data/sm8750-gpu-inputs.json` | 固件钉选 |
| `porting.toml` 的 `gpu` 区 | 每个里程碑补 source/host/device 三类证据（现有 `next_action` 改为本计划） |

### 7.4 社区仓库与 SoC 仓库

| 仓库 | 文件 | 改动 | 里程碑 |
| --- | --- | --- | --- |
| `PD`（`fast-display` 分支之上） | `drivers/Stage7Probe/Stage7Probe.cpp` | 弱默认 `pagani_gpu_start`，在 `native_legacy_display_late_probe` 末尾 `if(enabled("pagani-gpu")) pagani_gpu_start();` | M0 |
| `PD` | `drivers/BootProgress/BootProgress.c`、`PeripheralStatus.h`；`drivers/NativeMobileDisplay/NativeLegacyDisplay.cpp`、`NativeLegacyRequest.h` | 新增 `boot_progress_gpu_rows()`、`boot_progress_gpu_band()` 与 GPU 行绘制；CPU 拷贝跳过保留带（第 8.5 节） | M0 |
| `PD` | `drivers/PaganiInput/PaganiInput.cpp` | 触控 DRV-2 投票包进 `sm8750_rpmh_acquire/release` | M1 |
| `PD` | `drivers/PaganiArmCpu/ArmCpu.cpp` | 新增导出 `armcpu_spi_disable`（写 GICD_ICENABLER 并等 RWP）与 `armcpu_spi_enable`（写 GICD_ISENABLER），只接受已 attach 的 INTID（4.12.1） | M6 |
| `PD` | `drivers/Stage7Probe/Stage7Probe.cpp`、`drivers/NativeMobileDisplay/NativeLegacyDisplay.cpp`、`NativeGOPPresenter.cpp`、`NativeGOPWire.h` | presenter 创建、`native_legacy_gpu_offer`、早退役、帧映射缓存、`NGP_METHOD_BAND` 保留带方法（8.5；`NGPMode` 不变）；`UX/drivers/GopFramebuffer/NativeGOPWire.h` 同步修改 | M9 |
| `PD` | `NativeLegacyDisplay.cpp`、新 `DpuProbe.cpp` | 只读 DPU 探针、TE 锁相、120 Hz 上报、三缓冲翻页 | M12 |
| `SOC` | `dts/sm8750.dtsi` | `gpu` 加 `iommus=<&gpu_smmu 0 0>, <&gpu_smmu 1 0>`；`gmu` 加 `iommus=<&gpu_smmu 5 0>`；`gpu_smmu` 加 `#global-interrupts=<1>` 与 `interrupts`（全局 SPI 674，之后 25 个上下文 SPI 678–688、422、476、574–577、660、662、665–667、669、670、700，均为高电平，`LDT:24140,24149`）；新增 `qfprom: efuse@221c8000 { compatible="qcom,sm8750-qfprom","qcom,qfprom"; reg=<0 0x221c8000 0 0x1000>; #address-cells=<1>; #size-cells=<1>; gpu_speed_bin: gpu-speed-bin@138 { reg=<0x138 0x2>; bits=<0 9>; }; status="disabled"; }`；可选 `llcc` 节点（6 个窗口） | M0 |
| `SOC` | `kits/Sm8750Platform/Rpmh.h` | 弱全局 `sm8750_rpmh_drv2_owner` + `sm8750_rpmh_acquire/release` | M1 |
| `SOC` | `kits/Sm8750Platform/` | M11 后把 `A830/platform/sm8750/*` 上移为共享实现 | M11 后 |

社区仓库的改动能否合入上游未知（9.2），在合入前一律用本地覆盖（`local.toml` 的 `[local] drivers`）构建。

### 7.5 用户态构建与暂存（13T 只能用 Data 种子）

完整步骤见 `docs/inputs/userspace/build-userspace-a830.sh`（`bash -n` 通过，未执行）。要点：

- 工具链：`export DEVELOPER_DIR=/Applications/Xcode-26.6.app/Contents/Developer`（或 27.1；插件源码两者都过了 `-Werror` 语法检查，Mesa 与 Rust 桥未实际编译）；`brew install glslang shaderc spirv-tools bison`；`rustup target add aarch64-apple-ios --toolchain 1.98.0`；PATH 里 bison 3 与 Homebrew LLVM 在前。
- Mesa 固定 `85fda3b36405bcaea8bffc64aa4f2651dc90770d`（tree `b72ff1eb…`），按 json 顺序打 10 个补丁（含回移）；metal2vulkan 固定 `720ef1c`；`UNIXNU_GPU_ABI_DIR=/Users/xiaochoumao/unixnu-13t/Adreno830` 后 `python3 scripts/build-turnip-darwin.py --target ios`、`python3 scripts/build-adreno-metal-plugin.py`。首次成功后保存 `build/adreno-metal-bridge/Cargo.lock`。
- 暂存：13T 的 `PaganiDarwinBoot` 只认 RAMDisk、PackedRAMDisk、TrustCache（`DarwinBoot.c:222-231`），没有 GpuAddon 侧车，所以插件只能写进 RAM Data 种子：`python3 scripts/add-gpu-plugin-data.py --publish --clients WidgetRenderer_Default,PosterBoard --env build/NativeAdrenoMetal.env --prime-list build/prime-list.txt <BASE>.dbrdisk build/data-a830.dbrdisk`（BASE 的卷 UUID 必须等于内核 `CONFIG_XNU_OWNER_DATA_VOLUME_UUID`）；第一阶段自检时不加 `--publish`。
- 信任：`python3 scripts/make-owner-trust-bundle.py`（原始包 sha256 `7416ca4f…`，本地有 `作者产物/build-63/original-static-system.bundle`）生成 `owner-trust.bundle`，含插件、adrenoprobed、Turnip 三个 CDHash。任何重编都要依次重做 trust.json → owner-trust.bundle → payload。
- 载荷：`./unixnu payload -D <DEV> --data-ramdisk build/data-a830.dbrdisk --owner-trust-cache build/adreno-metal-plugin/owner-trust.bundle`，再 `./unixnu bootimg -D <DEV> -o <镜像>`。内核需要 `CONFIG_XNU_TRUST_CACHE_RUNTIME`、`CONFIG_XNU_OWNER_ROOT_TRUST`、`CONFIG_XNU_OWNER_DATA_MOUNT`（13T defconfig 均已有）。
- 种子里禁用了 `iconservicesagent`、`bluetoothd`、`iomfb_fdr_loader`，`.clients` 写 iconservicesagent 不会生效；种子 `persistent=false`，管线缓存每次开机都丢，预热列表（161 项）与持久 Data 是后续工作。

---

## 8. 诊断设计（运行期零开销）

### 8.1 原则

1. **状态是数据，不是日志**：驱动只往一个固定布局的全局结构里写字段（普通存储，最多一次原子递增），不在热路径调用 `IOLog`、`boot_progress_*`、`snprintf`。例外只有两个：8.7 的启动期寄存器追踪，只在启动与电源/恢复操作的作用域内写，运行期热路径每次访问只多一次原子读；8.8 的运行期计量行，由 DCVS 线程每 10 s 至多写一条、内容不变就不写。
2. **只在变化时显示**：叠加层由 BootProgress 现有的 1 Hz `poll()` 线程绘制，且只在行文字变化时重画；**每帧路径（`boot_progress_peripheral_frame`）不画 GPU 行**。系统接管显示后每一帧都会整屏覆盖帧缓冲，所以 GPU 行所在的 4 行文字高度（“保留带”）在 GPU 叠加层开启时不被呈现路径写入（8.5），画一次就一直可见。
3. **失败时一次性取证**：故障快照（寄存器、SMMU 状态、GMU 状态）只在失败或诊断模式下采集，正常启动只记阶段耗时。
4. **可拍照判读**：用户用手机拍屏幕；一行 ≤66 字符（`1216/18=67` 格），每个字段都能对照本节的表解释。

### 8.2 状态块 `adreno830_status`（`Adreno830Status.h`，全局导出）

```c
enum A830Stage : uint8_t { A830SInputs, A830SPower, A830SSecureCx, A830SSmmu, A830SFirmware, A830SGmu,
                           A830SCp, A830SZap, A830SFence, A830SMetal, A830SDcvs, A830SRunning = 15 };
enum A830State : uint8_t { A830Idle, A830Busy, A830Done, A830Failed, A830Stopped, A830Lost, A830Recovering, A830Slumber };
struct Adreno830Status {
    uint32_t magic;                 /* 0x41383330 'A830' */
    uint16_t version, bytes;        /* 1, sizeof */
    uint8_t  stage, state;
    uint16_t step;                  /* 阶段内步骤号（见 8.3） */
    uint16_t reason, attempts;      /* A830Reason；GMU 冷启动的重试次数 */
    uint32_t ioreturn;
    uint32_t hfi_late;              /* annibale：HFI 迟到排空的累计（排空次数、消息、迟到应答、迟到 F2H 错误各 8 位，饱和到 255；原保留字） */
    uint64_t reg, value, expect;    /* 最后访问的物理地址（或 SMC 功能号、HFI 头）、读到的值、期望值 */
    uint64_t aux[6];                /* 阶段专属，见 8.4 */
    uint32_t stage_us[11], boot_ms; /* 每阶段耗时、总耗时 */
    uint32_t seq;                   /* 每次发布变化 +1，叠加层据此判断是否重画 */
    uint32_t soc_id, feature_code, speed_bin, smmu_slots;   /* 板级观测 */
    uint32_t irq_off;               /* 未接上或被风暴保护关掉的中断：bit0 332、1 336、2 337、3 710、4 712、5 112、6 994 */
    uint32_t sticky[4];             /* 只记录的粘滞状态：sGFSR、CB0 FSR、CB2 FSR、GMU AO 状态（最近一次非零值） */
    uint8_t  qmp, coh_limit;        /* QMP：0 未执行、1 成功、2 超时、3 拒绝；一致性等级上限 0/1/2 */
    uint8_t  profile, profile_set;  /* pagani-gpu-profile；S0 接受后 profile_set = 1 */
    uint32_t gmu_general11;         /* annibale：最近一次冷启动在 B6 前读到的 GENERAL_11（RB 模式应为 0；原保留字） */
    /* 运行期计数（中断、提交、DCVS 用宽松原子写） */
    uint64_t submits, retired, irqs, fence_irqs, waits, wait_timeouts, late_wakeups;
    uint32_t faults, smmu_faults, gmu_faults, hangs, recoveries, hfi_errors, wptr_retries, underruns;
    uint32_t mhz, busy_permille, gmu_level, bus_level, votes, vote_failures;
    char rows[4][68];               /* 已排好版的叠加层行，第 0 行常显，1–3 行为诊断页 */
};
extern "C" Adreno830Status adreno830_status;
```

- 写入点：编排器在每个阶段开始/结束；各模块在失败时填 `reg/value/expect/aux`；IRQ 与提交路径只做计数递增。环的队列周期（4.13.5）不在这个结构里：DCVS 线程每秒把 `Adreno830StatusQueue` 存进 `Adreno830Status.cpp` 的静态变量（发布锁下），只用于排运行页第 3 行与第 2 行的作业归属字段（2026-10-31），以及每 10 s 至多一条的计量行（8.8），结构的布局与静态断言不变。故障计数（SMMU、GMU、CP）变化时由中断下半部重新发布一次（事件驱动，不设周期刷新），所以停在某阶段时诊断页上的故障计数也是实时的。
- 发布：`adreno830_status_publish()` 排版 `rows`、`seq++`，再调用 `boot_progress_gpu_rows(rows, n)`（BootProgress 的强定义；本仓库给一个弱的空默认，便于在没有 BootProgress 的设备上链接）。只在阶段切换、失败、以及 DCVS 线程每秒一次且内容有变化时调用。
- 注册表：S9 之后，阶段切换或故障时把整个结构作为 `OSData` 设到 `NativeAdrenoGpuPublished` 的属性 `Adreno830Status`（不在热路径），`adrenoprobed` 与主机工具可直接读。
- 失败时另外调用一次 `boot_progress_display_status("GPU <阶段名>", ioreturn)`，让它进入 BootProgress 的阶段历史（30 s 无画面时的回退页会列出）。

### 8.3 原因码与步骤号

`A830Reason`：0 无；1 参数；2 固件（长度/哈希/格式）；3 SoC 信息（SMEM/SoC id）；4 cmd-db；5 超时；6 意外状态；7 功能位回读不符；8 SMC 错误；9 RPMh（TCS）；10 SMMU 路由冲突；11 SMMU 故障；12 内存不足；13 GMU NMI；14 GMU 故障（CM3/看门狗）；15 HFI 应答状态非 0；16 F2H 错误消息；17 CP 故障；18 挂死；19 中断接线（只用于 332）；20 发布失败；21 被 `pagani-gpu-stop` 停下；22 GMU 请求主机资源（F2H 20 `MEM_ALLOC` 或 110 `CNTR_REGISTER`，RB 模式不支持）；23 中断被风暴保护关闭（运行期事件，不让阶段失败，只在状态行与 `irq_off` 上体现）；24 启动参数要求本版本没有的功能（`pagani-gpu-hwsched` 非 0）。

步骤号按阶段：S0 的启动参数检查为 0x49（`pagani-gpu-hwsched` 非 0 也在这里，原因码 24）；S1 沿用 `Sm8750GpuPowerStep`（1 GCC AHB、2 复位、3 CX GDSC、4 GPLL0、5 PLL0、6 RCG、7 分支），QMP 可选步为 0，CX_AHB 超时写入与回读为 0x30（annibale 配置，或 `pagani-gpu-ahbto=1`；reg 为出错寄存器的物理地址，value 为读到的值，expect 为 `0x1B12`）；并在 `reg/value` 填 `Sm8750GpuPowerTrace` 的块偏移与值；S3 用 4.5.3/4.5.4 的步号（bank 复位为 0x0F，CB0 加 0x100、CB2 加 0x200、LPAC 加 0x300）；S4 的 CX 计时器播种为 1；S5 用 B1–B8 为 1–8、M1–M9 为 0x11–0x19、OOB 为 0x20、OOB 后 GX 上电确认为 0x21；S6 用 4.8 的步号与 0x18（annibale 的状态检查）、0x40（SQE 启动）、0x41（ME_INIT 等待）；S7 用 4.11 的 1–12；S8 的 V9b/V9c（只在 `cohtest`）为 0x29/0x2A。

### 8.4 叠加层行格式

第 0 行（`pagani-gpu-status≥1`）：

| 状态 | 格式 | 例 |
| --- | --- | --- |
| 启动中 | `GPU S<s>.<step> <名8> RUN <ms>ms` | `GPU S5.07 GMU-HFI RUN 12ms` |
| 失败 | `GPU S<s>.<step> <名8> FAIL <ioreturn> <reg>=<value> <reason>` | `GPU S5.07 GMU-HFI FAIL E00002D6 3D7E070=00000000 r5` |
| 被停下 | `GPU S<s> <名8> STOP ok <总ms>ms` | `GPU S3 SMMU STOP ok 41ms` |
| 运行 | `GPU RUN <MHz>M b<忙%> g<档>/b<总线档> s<提交数> f<故障> h<挂死>` | `GPU RUN 525M b37 g6/b4 s123456 f0 h0` |
| 设备丢失 | `GPU LOST S<s>.<step> <ioreturn> <reason> f<故障>` | `GPU LOST S8.02 E00002D8 r18 f1` |
| 恢复中（M10 起） | `GPU RECOVER <次数> <阶段> <ms>ms` | `GPU RECOVER 1 S6 85ms` |
| 休眠（P1 起） | `GPU SLP <已睡 s>s w<唤醒次数> <上次唤醒 ms>ms` | `GPU SLP 12s w37 4ms` |

名字缩写：`INPUTS`、`POWER`、`SECCX`、`SMMU`、`FWMEM`、`GMU-HFI`、`CP`、`ZAP`、`FENCE`、`METAL`、`DCVS`。

S0 接受启动配置后，第 0 行末尾在每种状态下都加 ` p1`（annibale）或 ` p0`（设计）；只有最长的 FAIL 与 RUN 行（64–66 字符）放不下，此时整段省略，不截断字段。

第 1–3 行（`pagani-gpu-status=2`，诊断页，内容随阶段）：

| 阶段 | 第 1 行 | 第 2 行 | 第 3 行 |
| --- | --- | --- | --- |
| S0 | `SOC 618 FC <feature> BIN <speedbin> FW ok` | `ARC gfx <n> mx <n> cx <n> gmxc <n/->` | `BCM SH0 <u/w> MC0 <u/w> ACV <vcd> CN0 <vcd>` |
| S1/S2 | `PWR st<step> <blk>+<off>=<val> PLL <mode>` | `SCM x0 <x0> x1 <x1> n<调用> FUSE <swfuse>` | `TCS <slot> st<status> c<cmd0> <cmd1> <cmd2>` |
| S3 | `SMMU ID <idr0> <idr1> <idr2> CR0 <scr0>` | `ACR <sacr> GFSR <gfsr> SLOT <g>/<l>/<m> S2C <s2cr0>[ AP<0/1/2>]` | `CB0 <sctlr> <actlr> FSR <fsr> CB2 <sctlr> <fsr> F<smmu_faults>` |
| S5 | `GMU FW <core> HFI <hfi> INIT <result> <ms>ms[ TCM <字数>]` | `HFI M1 <st> M2 <st> M6 <st> M8 <st> M9 <st>[ L<m>/<a>/<e>][ G11 <general11>]` | `OOB <us>us GX <pwrclk> RPMH <state> AO <ao>[ RSC <rsc>]` |
| S6/S7 | `CP RPTR <r> WPTR <w> RBBM <status> HW <fault>` | `SQE PC <br>/<bv> PROT <prot> UNMASK <shadow>` | `ZAP x0 <x0> x1 <x1> step <n> wq <n>[ L<m>/<a>/<e>]` |
| S8 | `IRQ 332 rb <readback> n<计数> wake <us>us off <irq_off>` | `COH V4 <p/f> V9a <p/f> V9b <p/f/-> V9c <p/f/->[ L<m>/<a>/<e>]`（`-` 表示默认镜像不跑） | `TS CP <hi:lo> AO <hi:lo>` |
| 运行（`status=2`） | `DCVS <最低>-<最高>[ T<目标><趋势>[!]] t<各档驻留千分比>`（温控字段只在设计配置，见下） | `MEM pool <MiB> used <MiB> imp <n> coh <0/1/2>[ vm <MiB>][ fw<最大>ms/<n4>][ sc<MiB>][ <名>:<MiB>]`（作业归属与客户端尾部见下） | `IRQ <n> wait <n> to <n> late <n> \| q <均>/<最大>ms d<深度> s<慢>[ sw<n>]`（DCVS 线程存过一个队列周期之后，4.13.5；之前为 `IRQ <n> wait <n> to <n> late <n> wptr <n>`） |
| 运行（`status=3`） | 插件最近的报告行（`native_adreno_metal_diag.report`，如 `adrenoprob s34 e0 v4/4 …`、`PUB tex …`、`ST0 c… d…`） | 插件 Turnip 阶段行（`report2`，如 `T s0 r0 v1024 c1024 g480..544/0`） | `CLI o<打开> c<调用> s<最后状态> NGP <offers>/<done>/<fail>` |

方括号内是可选字段：`TCM`（B4 回读相符的 TCM 字数）、`G11`（`gmu_general11`）、`RSC`（B3 读到的 `RSC_STATUS0_DRV0`，bit16 为睡眠）只在 S0 接受了 annibale 配置时显示，设计配置不读这些寄存器；`L<m>/<a>/<e>` 在做过 HFI 迟到排空（`hfi_late` 的排空次数非 0）后显示，依次是排空读到的消息总数、其中迟到的应答数、迟到的 F2H 错误数（开机以来累计），各显示到 99。S5 页的 `AO` 取粘滞字 `sticky[3]`，`aux[5]` 低 32 位存 RSC 状态（`Adreno830Status.h` 的页布局）。

运行页 MEM 行（2026-10-30）：`imp` 是 GPU VM 的对象数，`used` 是 BO 池已用 MiB，` vm <MiB>` 是 GPU VM 里映射的总字节（自有、池、导入、扫描输出）。之后是发布外壳每秒一次统计的客户端（`native_adreno_client_tick` → `adreno830_status_clients`，不在 576 字节的块里）：` sc<MiB>` 是所有客户端不在池里的自有 BO（池满或碎片时退回的连续/零散页：占系统内存、逐页改页属性、每页一个 GPU 段），为 0 时不显示；然后是占 GPU 内存最多的客户端 ` <进程名>:<MiB>`，持有导入时写成 ` <进程名>:<自有>+<导入>`（导入是被钉住的用户页，多为 IOSurface），名字前的 `~` 表示孤儿（连接已关闭、对象还没能释放）。名字按剩余宽度截短：拆分写法至少留 6 个字符（更短的名字整个），合计写法至少 3 个，再放不下就整段不显示；数字最大 99999，整行不超过 66 个字符。读法：`used` 不涨而 `sc` 涨，是池满或池碎片；某个客户端的 MiB 只涨不跌，是它（或它的用户态释放队列）在漏；`~` 长期不消失，说明孤儿的作业一直没完成。

运行页 DCVS 行的温控字段（2026-10-30，4.16）：设计配置的 DCVS 线程每秒在 DCVS 行之前调用 `adreno830_status_thermal(目标, 趋势, 标志)`，只保存；下一次 `adreno830_status_dcvs` 把它（`A830StatusThermal`）并进运行页 `aux[3]` 的 bit40–50：目标整数 °C 在 bit40–46（0 表示不显示）、趋势在 bit47–48（`A830StatusTrend*`）、紧急在 bit49。驻留只用 `aux[3]` 的 bit0–39（第 13–16 档），所以运行页最多显示 16 档。字段紧跟上下限：` T<目标><趋势>`，趋势 `?` 无可用读数（上限 607 MHz）、`=` 平稳、`+` 上升、`-` 下降，紧急（≥95 °C 直到低于 90 °C）再加 `!`，最长 7 个字符（` T127-!`）；驻留放不下时照旧以 `+` 截断，整行不超过 66 个字符。annibale 配置从不保存这个字段，DCVS 行与以前相同；`adreno830_status_reset` 清掉它。读法：上限（`-` 后的数字）低于 900（ACD 开）就是温控在压；`T80+` 而上限还在降是正常的追赶；上限停在 525 而 `+` 一直不变，说明热来自 GPU 之外（看 CPU 行的 `hot` 与 `cpufreq: GPU limited by heat` 日志）；出现 `!` 时这一秒是 389 MHz。

运行页第 3 行的队列尾部（4.13.5）：周期数据由 DCVS 线程每秒存一次（`adreno830_status_queue`，不在 576 字节的块里），只有 S10 与 RUN 阶段（各种状态，含 SLP）的运行页使用；`wptr` 字段从这一行去掉，仍在状态块里。四个计数各最多占 6、6、5、4 个字符：放得下时显示全数，否则按 k、M、G、T、P、E 向下取整（如 `wait 1000k`）。`<均>` 四舍五入到毫秒、`<最大>` 向上取整（所以最大显示 `16ms` 的周期没有慢完成），均 ≤999、最大 ≤9999、深度与慢个数 ≤999，最坏情况恰好 66 个字符。特殊形式：`q~4/38ms …` 表示本周期的完成是在没有栅栏中断时看到的（轮询，数值是上界）；`| q -` 表示这一秒既没有提交也没有完成；`q -/-ms d3 s0` 表示有提交但还没有任何完成（或只有丢了戳的完成）。

M14 P0 的三个字段（2026-10-31，都只在 `pagani-gpu-status=2` 的诊断页上，第 0 行不变）：S3 页第 2 行末尾 ` AP<状态>` 是 `CP_SMMU_APERTURE` 的结果（0 未调用、1 授予、2 拒绝，`aux[3]` 第 56–58 位，记下后这一行 54 个字符；4.5.6）。运行页 MEM 行在 ` vm` 之后、客户端尾巴之前的 ` fw<最大>ms/<n4>` 是本周期渲染服务器作业前面排着的外部服务时间的最大值（向上取整到毫秒，最大 9999）与其中超过 4 ms 的个数（最大 999），只在最大值不为 0 时出现（没有外部作业挡路的秒数，行与以前相同）；它最多 13 个字符，客户端尾巴让位：` sc` 放不下就整段不显示，进程名按原规则截短或不显示，整行仍不超过 66 个字符。IRQ 行队列尾部之后的 ` sw<n>` 是本周期进入新上下文（换客户端）的提交数（最大 9999），放得下整段才显示，最坏的 66 字符行不受影响。读法：某一秒 ` fw` 大于 4、同一秒 SLOW 行以 off/copy 为主，说明外部作业挡住了合成或拷贝（抢占第 1 阶段的门槛，4.17；只看 IRQ 行是 `q ` 的秒，`q~` 的秒是轮询看到的完成，fw 可能偏低，4.13.5）；` sw` 是每秒换客户端的次数，按进程页表时每次换客户端要多一次 TLB 全失效（M14 研究）。

### 8.5 BootProgress 与呈现路径的改动（`PD/drivers/BootProgress`、`PD/drivers/NativeMobileDisplay`）

现状要点（`PD/drivers/BootProgress/BootProgress.c`）：`poll()` 每秒画第 1/2/4/5 行，但 `clear_line`/`glyph` 在系统接管显示（`display_owned`）后直接返回（`:168-176,217-221`）；接管后能画到界面上的只有 `boot_progress_peripheral_frame`，它直接用 `glyph_pixels` 且每帧调用一次（`:469-500`），这正是要避免的逐帧开销。因此：

- `PeripheralStatus.h` 新增两个接口：
  - `void boot_progress_gpu_rows(const char (*rows)[68], unsigned count);`：`irq_save` 下把行复制进 `gpu_status[4][68]`、记下行数、`gpu_seq++`；`panic_state` 或 `busy` 时直接返回；本身不画。
  - `int boot_progress_gpu_band(unsigned *y0, unsigned *y1);`：返回当前保留带（有 GPU 行时为文字行 10 起、行数×`LINE`，即 `y=420..420+42×行数`；M12 有显示探针行时扩到文字行 10–17），没有任何状态行时返回 0。
- `poll()`（每秒一次）末尾：`gpu_seq≠gpu_drawn_seq` 时用 `glyph_pixels`（不受 `display_owned` 与开机图标限制）把收到的 1 或 4 行画到文字行 10–13，先把整条带涂黑，记下 `gpu_drawn_seq`。画不画、画几行完全由驱动决定：`pagani-gpu-status=0` 时驱动从不调用 `boot_progress_gpu_rows`，BootProgress 不读任何启动参数。
- CPU 呈现路径（`NativeLegacyDisplay` 的 `native_legacy_copy_rows`/`native_legacy_copy_pixels`）：每帧开头调用一次 `boot_progress_gpu_band`（两次内存读），有保留带时跳过带内的行，只拷 `[0,y0)` 与 `[y1,H)`。带内像素因此保持为叠加层内容，不需要逐帧重画；拷贝量还少了 4 行。
- GPU 呈现路径（M9 的 D1、M12 的 D2）：**不改 `NGPMode`**。`NativeGOPWire.h:17` 静态断言 `sizeof(NGPMode)==40`，显示驱动要求尺寸完全相等（`NativeGOPPresenter.cpp:216`），插件按自己头文件里的尺寸调用并要求 `mode.version==NGP_VERSION`（`UX/drivers/AdrenoMetal/GpuScanoutClient.h:13,49-52`），而同一个插件也给 sheng 用：任何一端改了尺寸或版本，另一端得到的都是 BadArgument 或“模式被拒”，根本读不到版本字段，“见到版本 1 就按无保留带处理”无法实现。所以 M9 新增方法 `NGP_METHOD_BAND(=4)`（隧道 0x7004）：无输入，输出新结构 `NGPBand {uint32_t y0, y1, rows, reserved;}`（16 字节，`static_assert`）；插件在 `MODE` 之后调一次，失败（`kIOReturnUnsupported`、`kIOReturnBadArgument`，含旧内核 `HandoffLast=0x7003` 的拒绝）就按无保留带处理。两份 `NativeGOPWire.h`（`PD/drivers/NativeMobileDisplay/`、`UX/drivers/GopFramebuffer/`）同步修改。插件 `GpuScanoutPresenter` 把一次拷贝拆成带上、带下两个矩形；D2 时 BootProgress 只画 B0（控制台映射所在的缓冲），插件在每次拷贝时顺带用 GPU 把 B0 的保留带复制到目标缓冲（约 0.8 MB，GPU 约 0.1 ms，CPU 为零）。
- 成本：稳态为零（每帧只多两次内存读）；变化时每行约 4.3 万次 32 位写（控制台映射仍是 Device-nGnRnE，R1 knobs；以后可把 `initialize_screen` 的映射改为 WCOMB）。`boot_progress_peripheral_frame` 不变；`pagani-gpu-status=0` 时没有保留带，呈现路径与现在完全相同。

### 8.6 主机与上机工具

- 主机：`tests/run-host-tests.py` 打印每个阶段模型测试的 PASS 行；`tests/test_status.cpp` 检查每种行格式 ≤66 字符、字段不越界。
- 上机：拍照对照 8.4。设备上目前没有可用的命令行，`Adreno830Status` 注册表属性主要供插件与 `adrenoprobed` 读取并写进它们的报告行（M7 起），以后有了 shell 再用 `ioreg -l -w0 -r -c NativeAdrenoGpuPublished` 直接读。

### 8.7 启动期寄存器追踪（`A830Trace.h`，`pagani-gpu-trace`，2026-10-30）

目的：真机卡死、手动复位回到安卓后，从 ramoops 日志（`工具/gpu-test-loop` 解码）的最后一条访问行直接看出卡住总线的寄存器或 SMC，不必再按 `pagani-gpu-stop` 二分。

- **作用域**：`adreno830_start` 的 S0–S10 全程；之后重跑阶段体的电源操作（`cxPowerOn/Off`、`smmuRestore`、`cxTimerSeed`、`gmuStart`、`gmuForceOff`、`rsccSleep`、`hfiSend`、`cpStart`、`secureResume`）各自一个作用域；一次恢复从闸门转为 Recovering 到 Open 或 Lost 是一个作用域（含快照、`RBBM_INT_0_MASK`、GBIF 停止这些操作之间的访问）。作用域外（运行期提交、DCVS 投票与轮询、中断上半部、`GetTimestamp`、P2 的 OOB 操作）不写行，每次访问只多一次原子读；`pagani-gpu-trace=0` 或日志未开时 SMMU/GMU/`A830Irq`/SMC 的表与原来完全相同。
- **入口**：编排器为模块绑定的全部表都经过它：自己的 `rd/wr`（GPU/GMU/CX_MISC 的字、`Sm8750GpuPower` 的 GCC/GPU_CC 回调、DRV-2、AOSS QMP 与 IPCC、LLCC、TSENS、电源状态机与恢复的窗口），包住的 SMMU io（`A830Smmu`、VM 的表 io、编排器自己的读）、GMU io（`A830Gmu`、`A830GmuFirmware`、CX 计时器与 AHB 超时，含 RSCC、GMU_AO 窗口）、`A830Irq` 的三个窗口，以及 SMC 指令（S2、S7 的 ZAP、SMMU 光圈）。cmd-db、SMEM、qfprom 与 ZAP 区是内存式读写，不写行。只在线程上下文写：中断上半部与 SCM 等待队列中断经同一张表时由 `ml_at_interrupt_context` 排除。
- **先写后访问**：每行经 `boot_progress_trace` 写进安卓 ramoops 区，返回时已在 DRAM（BootProgress 写完所有副本后 `dsb sy`）；读与 SMC 返回后再写一行结果。多线程由一把关中断的自旋锁串行，锁等太久的行计入 `drops`。
- **行格式**（地址、值、步骤、IOReturn 为不带 0x 的十六进制；计数与微秒为十进制；每行不超过 127 字符）：
  - `g<S>.<step> <块> w <物理地址> <值>[ #<n>]`（`W` 为 64 位写），`g<S>.<step> <块> r <物理地址>[ #<n>]`，读返回后 `... =<值>`（`R` 为 64 位读）；
  - `g<S>.<step> SCM c <x0> <x1> <x2> <x3>[ #<n>]`，返回后 `g<S>.<step> SCM c <x0>[ #<n>] =<x0> <x1> <x2>`；
  - 一段计数结束：`g<S>.<step> <块> <操作> <首地址> x<n>[ ..<末地址>] =<最后读到或写入的值>`；
  - 阶段：`g<S> <名> begin t=<us>`，`g<S> <名> end ok|stop <us>us`，`g<S> <名> end fail <IOReturn> s<步骤> r<原因> <reg>=<value> <us>us`；操作：`g op <名> begin`，`g op <名> end <IOReturn> <us>us`；开头 `g start p<配置> stop <阶段|-> t=<us>`，结尾 `g done <IOReturn> <us>us lines <n> drops <n>`；注记 `g<S>.<step> note <文字>[ <值>[ <值>]]`（S3 在 GPU 上下文建好后写一条 `gpu aperture GiB, table pages <GiB> <页数>`，两种配置都有，值为十六进制，16 GiB 光圈是 `10 2011`、4 GiB 是 `4 805`、2 GiB 是 `2 403`；S3 末尾授权之后再写一条 `gpu aperture <状态> x0=<x0>`，是 `CP_SMMU_APERTURE` 的结果，4.5.6，2026-10-31）。
  - S 是状态块的阶段（0–a，运行后为 f），step 是状态块的步骤（8.3）；S1 上电的 GCC/GPU_CC 访问用 `Sm8750GpuPowerStep`，正是 S1 失败时的步骤号。块名：GPU、RSCC、DBGC、CXM（CX_MISC）、GMU、GMUAO、GPUCC、GCC、SMMU、DRV2、AOSS、IPCC、TSNS、LLC0–3、LLCO、LLCA、SCM。GPU 空间寄存器的 dword 为 `(物理地址 − 0x3D00000) / 4`，对照 `A830Regs.h`。
- **压缩**（`pagani-gpu-trace=1`）：与最近 8 个键之一相同的访问只计数（键为块、操作、地址；写与 SMC 还要值与参数相同）：同一地址是轮询，下一地址（+4，64 位 +8）是顺序块。轮询的第 1–4 次、顺序块的第 1–16 个字、之后 2 的幂次与每 1024 次写一行（第 2 次起带 `#<n>`）；新键出现、状态步骤变化、阶段/操作/注记行之前写结束行 `x<n>`。每轮不超过 8 个寄存器的轮询（`A830Cp` 的 annibale 空闲判据每轮 7 个）转多少圈都只有十几行，挂在轮询里时最后一行就是那个寄存器；只有长于 16 个字的顺序块（GMU TCM 装载与 annibale 回读、24 组 SMR/S2CR 扫描）里卡死时，最后一行可能是该块里前面某个记过的字。`pagani-gpu-trace=2` 不压缩，每次访问一行。
- **安静窗口**：S4 与唤醒时的 CX 计时器播种在关中断下测量自身写入的耗时，窗口内不写行，前后各一条注记（`cx timer seed: AO_COUNTER 3d9e200/204 writes not logged`、`cx timer seed <IOReturn> <次数>`）。
- **成本与容量**：一行约几微秒（每个副本写一遍再 `dsb sy`），压缩时轮询的未记次数只多一次加锁查表。主机脚本的 S1–S3（含 301 次的 CX GDSC 轮询）压缩后 463 行，不压缩 1113 行（含 2026-10-31 的光圈结果注记）；完整启动约一千多行，追踪行与阶段、状态行同在 BootProgress 的事件环里，环会循环覆盖较早的行，但最后几百行（卡死前的访问）总在。
- **测试**：`tests/test_a830trace.cpp`（逐行格式与压缩规则；在 `test_start.cpp` 的替身上用脚本化的寄存器流量检查两种配置 S1–S3 的完整行序列、写入前落行、SMC 指令前落行、计数压缩、`=0` 与日志关闭时无行、`=3` 被 S0 拒绝、安静播种、操作与恢复作用域、两种配置下 16/4/2 GiB 光圈的 S3 注记）。

### 8.8 运行期计量行（`adreno830_status_meter`，M14 P0，2026-10-31）

目的：抢占第 0 阶段与按进程页表 P0 要的完整数字（M14 研究），而运行页每行只有 66 个字符、G 记录就是这些行的副本，放不下；又不能再加每秒一行的日志（唯一例外是下面 2026-10-31 的每秒行）。所以完整数字在诊断页模式下每分钟至多写一条 ramoops 行（U 记录，走 BootProgress 的 32 KiB 事件环；G 记录 2026-10-31 起走控制台环）。事件环要留住开机一次性的 U 行（预装应用拷贝、叠加层、音频服务）和 PaganiDiag 迁移期的 diag/SAM 行直到复位，计量行在里面只能占小头。

- **何时写**：DCVS 线程每秒在存好队列周期（4.13.5）与客户端数字（8.4）之后调用一次 `adreno830_status_meter`，把这一秒的周期并进窗口；第 60 次时，只要叠加层是诊断页（`pagani-gpu-status=2` 或 `3`，测试镜像都用 2）而且 BootProgress 的日志在跑（`boot_progress_trace_enabled`），就排出一行，与上一次写出的行不同才经 `boot_progress_trace` 写出，然后开始新窗口。默认的 `status=1`（以及 0）和日志关着时不排版、不写，窗口照样每 60 次清零：默认配置的周期性日志逐字节不变，也不需要新的启动参数；什么都不画、不发布。空闲的 GPU 写出第一行之后，只有数字变了（例如有新的槽位拒绝）才再写。审查（2026-10-31，S2）之前是不分模式、每 10 s 一条：GPU 活跃时平均值每个窗口都变，“只写变化的”不起作用，单靠计量行约 34 分钟就写满事件环，一轮 660 s 的测试要占去约三分之一；每秒的数字仍在 G 记录里（`status=2` 的 MEM、IRQ 行，控制台环，约 70 s 一圈）。
- **格式**：`gpu meter rs <n>[ <均>/<最大>ms] fw <n>[ <均>/<最大>ms] n4 <n> ext <n> <忙>ms ctx <n> <每秒最多>/s poll <n>[ conn <现在>/<峰值> nos <n> ovf <n> vmr <对象>/<区段> pf <一致>/<不一致>/<无法判断>/<轮询>][ ca …][ lk …][ im …][ vt …][ zf …][ cs …] ap<状态>`。时间以毫秒带一位小数（平均四舍五入，最大向上取）。`rs`：渲染服务器作业数与服务时间；`fw`：其中 fw 已知的作业数与 fw；`n4`：fw 超过 4 ms 的个数；`ext`：外部作业数与服务时间合计（4.13.5）；`ctx`：上下文入口合计与单秒最多；`poll`：窗口里完成是在没有栅栏中断时看到的周期数（4.13.5 的 `polled`），不为 0 时这一行的 `rs` 是上界，`fw`、`n4` 与 `ext` 的时间可能偏低，不进抢占门槛（4.17）；方括号段在发布外壳的第一次每秒统计之后出现（S9 之后）：`conn` 当前连接数与开机以来的峰值，`nos` 因没有空闲槽被拒的打开（`native_adreno_slot_pressure[1]`），`ovf` 越出窗口被拒的映射（`native_adreno_window_overflow` 各槽之和），`vmr <对象>/<区段>` 因渲染服务器保留额度被拒的映射，对象记录和页表区段分开计（`native_adreno_vm_reserve_refused` 两项，2026-11-01 起分开：抬高一个对另一个没用），`pf` 页故障时在途作业与 FAR 窗口对比的四类计数（4.12.3，只计数，扣连接仍按 FAR）；`ap` 是 `CP_SMMU_APERTURE` 结果，S3 存下之前为 `-`（4.5.6）。各计数封顶（作业与拒绝 99999，`pf` 各 9999，连接与 `poll` 99），不算调用数字最宽 186 个字符，一条记录放得下（BootProgress 每条记录 200 个字符）。例：`gpu meter rs 12000 0.9/12.5ms fw 12000 0.1/6.1ms n4 108 ext 360 1110.6ms ctx 9570 169/s poll 0 conn 12/18 nos 0 ovf 0 vmr 0/0 pf 0/0/0/0 ap1`。
- **调用数字**（2026-10-31，`NativeAdrenoMeter.h`，`pagani-gpu-kopt` 位 4，思路来自上游 `59171b2` 的逐调用记录，我们没有那个导出通道，所以只留判断第二批改动要用的窗口数字）：发布外壳每秒取走一次（`NativeAdrenoMeterTake`，放进 `Adreno830StatusClients::calls`），`adreno830_status_clients` 并进计量行的窗口，一秒一次，不重不漏。放在 `pf` 和 `ap` 之间，每一段全 0 时不写，位 4 关掉时这一行与之前逐字节相同。时间是整毫秒向上取（小于 1 ms 也显示 1），最长值封顶 9999，合计与计数封顶 99999。
  - ` ca <BoAlloc>/<BoMap>/<BoFree>/<BoImport>/<Submit>`：窗口里每类调用最长的一次，按调用者看到的算（发布外壳的 `runCore` 与 `importBo` 从进入量到返回），连接锁、Memory、VM 锁的等待、环满时的睡眠、BoFree 拆掉 CPU 视图的时间都算在内；WaitFence 不算（那是 GPU 的时间）。
  - ` lk <连接>/<Ring>/<Memory>/<VM>`：窗口里每把锁最长的一次等待。先 `IOLockTryLock`，拿到就不读时钟，只有真的争用才读两次，所以没有争用时零开销。VM 锁的等待用 VM 时钟，位 5 关掉时恒 0。
  - ` im <导入映射数>/<其中走别名检查的>/<这些映射的毫秒合计>`：非独占导入的比例和代价（`native_adreno_vm_kind_us[1..2]`）。
  - ` vt <映射毫秒合计>/<解映射毫秒合计>`：VM 表事务的总时间，也就是 VM 锁被占用的大头，需要位 5。
  - ` zf <MiB>/<毫秒合计>`：池 BO 清零的量和时间，相除就是写合并清零的实际速度。
  - ` cs <尝试>/<失败>/<池外 BO 数>`：前两个数是池外 BO 的连续内存尝试（只有 64–128 KiB 才试），第三个数是这一分钟落到池外的全部 BO，包括没有尝试、直接走散页的。第三个数远大于第一个，就是大 BO 在走散页，每页占一个 VM 跨度，这是位 1 的代价。`sc` 只在屏幕的 MEM 行上，进不了日志，完整包（`status=3`）的报告页还会盖住这一行，`vmr` 又只数带保留量的客户端，所以这是日志里唯一的信号。
  - 这些数字决定第二批改动做不做：`lk` 的连接锁大就做等围栏免锁，Memory 大就把导入的 `prepare`/`complete` 移出锁，VM 大就把 `mapLocked` 的维护移出锁；没有 `cs` 这一段说明回退路径没走到。
  - 例（活跃的一分钟，193 个字符）：`… pf 0/0/0/0 ca 12/5/2/1/1 lk 3/0/7/1 im 122/3/6 vt 39/21 zf 411/62 ap1`。最宽 309 个字符（`Adreno830StatusMeterBytes` 320），超过 200 个字符的部分 BootProgress 放进 `+` 记录，`工具/gpu-test-loop/ramoops_format.py` 会接回去。
- **前端的一秒**（2026-11-01，每秒行的 ` pl`、` vk`、` cln`）：渲染线程自己造成的卡顿（在线程上建管线、走页清表面）在 GPU 的数字里看不出来。插件的线程（`NativeAdrenoMetalPlugin.mm` `sendSeconds`，开交接连接 0x4148，不占 GPU 槽）每秒把 `FrontendSecond.h` 的 32 字节记录经隧道方法 `NativeAdrenoFrontendMethod`（0x7101，和导出方法一样不在 `NativeAdrenoABI.h` 里）发给内核，这一秒什么都没做就不发。内核只收 `backboardd` 的，没有每秒行时回 `kIOReturnUnsupported`，线程就此结束。`adreno830_status_frontend` 累加，写出一行时取走；没有写行的秒留到下一行。格式 ` pl <状态数> <ms>/<最长 ms> vk <VkPipeline 数> <ms>/<最长 ms> cln <ms>/<最长 ms>`，放在 ` top` 之前，都是 0 的括号不出现，最宽 53 字符，整行上限由 262 变为 315。
- **每秒行 `gpu sec`**（2026-10-31，`adreno830_status_meter` 同一次调用写出）：一分钟的窗口分不清尖峰（`runs/20261031-223924`：9 次 copy≥25 ms 落在哪一秒、那一秒是谁在调用，计量行回答不了）。条件是 `adreno830_status_diag_lines()`：`pagani-diag` 非 0（S0 从 `Adreno830Board::system_diag` 交给状态块）、诊断页 2 或 3、日志在跑，外加这一秒有提交或完成，或者调用数到 `Adreno830StatusSecondBurst`（1000，抓 GPU 空闲时空转的客户端）。启动参数没有再加：`pagani-diag` 是系统现有开关（卡顿探针、PaganiDiag 都用它），和 `pagani-gpu-pool` 一样在 `Adreno830ParseKnobs` 里单独读，不进旋钮表（`Adreno830KnobCount` 已到 31，`flags` 已满）。回路镜像（`loop-full` 等，`pagani-gpu-status=2`、没有 `sheng-log=1`、也没有 `pagani-diag`）只写分钟行。
  - 格式：`gpu sec rs <n>[ <最长>ms] ext <n>[ <最长>ms][ q <最长>ms d<深度>][ fw <最长>ms][ wf <最长>ms] cl <调用数>[ ca <BoAlloc>/<BoMap>/<BoFree>/<BoImport>/<Submit>][ vt <映射 ms>/<解映射 ms>][ im <导入映射数>][ pl ...][ vk ...][ cln ...][ top <进程名> <调用数> <类别><n>...]`。`rs`、`ext`：这一秒两边的作业数和最长服务时间（`ext_max_us`，`A830Fence` 新增）；`q`、`d`：提交到完成的最长时间和最深的环（`status=3` 看不到 q 行，这里补上）；`fw`：渲染作业前方的外部时间最长值；`wf`：这一秒最长的 WaitFence（`NativeAdrenoMeterRaise(&native_adreno_meter_fence)`，位 4）；`cl`：全部连接的调用数（已关闭的连接也算）；`ca`、`vt`、`im`：这一秒的调用数字；`top`：调用最多的活连接的进程名（12 个字符）、调用数和各类别（`al mp fr im su wf cp ot`，`NativeAdrenoCallClassOf`，0 的不写）。最宽 262 个字符，一个忙的秒约 120 个字符、150 字节，超过 200 个字符的部分进 `+` 记录。
  - 计数：每个连接的 `classCalls_`（放在连接对象里，关闭时竞争的调用不碰已释放的盒子），`runCore` 和 `importBo` 各一次 relaxed 加；`native_adreno_client_tick` 每秒在 `g.lock` 下取差值，选出调用最多的连接，同时维护 `CoreBox::quiet_s`（连续多少秒没有调用，给 6.2 的拒绝行用）。
  - 日志量：约 150 字节/秒（只算有 GPU 工作的秒），走事件环。【真机】这轮事件环别的类别约 134 字节/秒，加上这一行 32 KiB 约两分钟翻完，所以这一行只该用在带 UFS 日志的测试镜像上（`pagani-diag=1` 没有绑定 `sheng-log=1`，开了诊断而没开 UFS 日志时要自己知道这一点）；带 UFS 日志时前 32 个槽保住开机，其余槽做环。
- **日志量**（用真实的排版函数 `adreno830_status_format`、`adreno830_status_meter` 排出，按 BootProgress 的记录格式算：33 字节头 + 文本 + 换行；G 记录每行再加 `<组>.<行>/<行数> ` 前缀；负载按 120 Hz 合成：每秒约 240 个作业，其中 200 个渲染服务器、40 个外部，160 次切换）：
  - G 记录（控制台环）：`status=1` 只有第 0 行，一组 85 字节，M14 前后不变，51 000–102 000 字节/10 分钟（每秒 1–2 组）；`status=2` 一组由 379 字节变为 385（只有切换，IRQ 行加 ` sw160`）或 388 字节（再加 MEM 行 ` fw7ms/12`，进程名让位），即 227 400–454 800 → 231 000–465 600 字节/10 分钟（+1.6%–2.4%）；空闲的秒没有新字段，也不多出一组。
  - 计量行（事件环）：`status=1` 为 0（审查前的版本 60 条约 9 600 字节/10 分钟，最宽 12 360 字节）；`status=2` 活跃时每 10 分钟 10 条约 1 712 字节（一条约 138 个字符、172 字节），每条都最宽时 2 140 字节，空闲时只有第一条（131 字节）；单靠计量行写满 32 KiB 事件环要约 190 分钟（审查前约 34 分钟）。2026-10-31 加调用数字后活跃时一条约 193 个字符、227 字节，每 10 分钟约 2 270 字节（每条都最宽时 3 650 字节，含 `+` 记录的头），约 144 分钟写满；`pagani-reset-after=5400` 的一轮 90 条约 20 KiB。环是循环的，复位前最后的计量行总在，开机一次性的行在长测试里本来就可能被冲掉。
  - S3 的光圈注记（4.5.6）每次开机一条约 64 字节，属于启动期追踪（`pagani-gpu-trace` 默认 1 时写，默认配置也有这一条，不是周期性的）。
- 主机测试：`tests/test_status.cpp`（S3 页 `AP`、MEM 行 `fw` 与客户端尾巴让位、IRQ 行 `sw` 的确切布局与 66 字符上限（`sw` 恰好填满第 66 个字符的边界），随机块；计量行的节奏（60 个周期一条）、只写变化、`status=1/0` 不写而 `2/3` 写、日志关着不写、`poll` 计数、平均四舍五入而最大向上取、`ap-`、最宽 180 个字符、200 个随机窗口；调用数字每秒并进窗口一次、最长取大合计相加、整毫秒向上取、全 0 的段不写、最宽 297 个字符、复位忘掉窗口），`tests/test_meter.cpp`（计时哪些方法、计时器的作用域、争用时才读时钟、并发取最大、取走一次、VM 计数取差），`tests/test_a830trace.cpp`（DCVS 线程的队列操作接到计量行：默认叠加层不写、诊断页写一条；光圈注记）。

---

## 9. 风险与未决问题

### 9.1 主要风险（按严重度）

| # | 风险 | 后果 | 缓解 |
| --- | --- | --- | --- |
| 1 | 本机 GPU SMMU 的实际状态与 annibale/hummer 不同（sACR、ACTLR 可写性、S2CR 写入策略、Gunyah 对 S2CR 的改写） | S3 失败或同步外部中止 | M1 先做只读预检并拍照；S3 只写已被同 SoC 证明安全的值；单变量实验放在 CB4 等不用的银行 |
| 2 | gen80000 GMU 固件在 RB 模式下的行为（安卓只跑 HWSCHED）：依赖票高位、START 时机、v5 应答 | S5 失败 | 默认 KGSL 写法，F2H_MSG_ERR/超时时切 Linux 写法做对照；诊断页记录每条 HFI 头与状态 |
| 3 | ZAP 三次 SMC 在 13T TZ/Gunyah 下的行为（WAITQ、签名） | S7 失败或卡在 SMC | SCM 层完整实现 INTERRUPTED/WAITQ/EBUSY；SMC 卡死只能靠 `pagani-gpu-stop=6` 二分 |
| 4 | 同步启动遇到不返回的 SMC 或硬件挂死 | 卡在 launchd 之前，桌面起不来 | 每个等待有上限；`pagani-gpu=0` 或 `pagani-gpu-stop=N` 的备用启动镜像；`pagani-gpu-async=1` |
| 5 | GPU 写经一致性映射卡在 CACHE_CLEAN（A740 教训） | 设备挂死 | 默认只让 GPU 只读的导入页走一致；V9b/V9c 只在 `pagani-gpu-cohtest=1` 的独立镜像里跑，通过才扩大 |
| 6 | LPAC 路由、CB0 TCR 被 CP 改写 | CP_INIT 永不完成或整机复位 | 4.5.5 强制顺序；巡检容忍 EPD1；TTBR1 空表 |
| 7 | 降低主机票（LOW_SVS、DDR 底线）引入不稳定或欠载，或连带压低 UFS、MDP 依赖的电压与带宽 | 偶发挂死、显示欠载、存储变慢 | 默认值就是作者实测的票（CX/MX 最高档、DDR 第 11 级），SH0/MC0/CN0 永不 valid=0；M11 才逐个单变量降档，验收比较 UFS、CPU 拷贝帧时间、MDP 欠载；欠载计数自动升底线 |
| 8 | DRV-2 与触控并发投票 | TCS 槽冲突 | 共享锁（4.3.4）；启动期天然串行 |
| 9 | 显示交接与 DPU 翻页（splash 区是否在 MDP 的 SMMU 映射内、GPU 能否写 splash 区） | D1/D2 失败 | 只读探针先行；D1 失败自动回退 CPU 路径 |
| 10 | 用户态工具链（Mesa 全量在 Apple clang 21 下未编译过）与 A830 的 GMEM 路径未验证 | 插件构建失败或渲染错误 | 用户态构建与 hostsim 从 M0 起与内核并行推进，作为 M7 的准入条件（不再等到 M7 才第一次编译）；保存日志，必要时装 Xcode 16.4；首轮 `TU_DEBUG=sysmem,nocb` |
| 11 | 跨仓库协作：社区仓库（PD、SOC）与 UniXNU 目录的改动能否合入 | 构建依赖本地覆盖 | `local.toml` 与 `--local-module`；改动保持小而可审 |

### 9.2 未决问题（需要上机或决策）

1. 本机 GPU SMMU 的 IDR、sCR0、sACR（CACHE_LOCK）、SMR/S2CR、CB 状态，以及事实 4 的具体失败检查（M1 预检回答）。
2. 速度档熔丝与 feature_code 的实际值，决定能否放开 900 MHz 以上档。
3. cmd-db 中 `gfx/gmxc/mx/cx` 的 vlvl 表，是否存在 `gbx.lvl`；`SH0/MC0/ACV/CN0` 的 unit/width/vcd。
4. CM3 故障在这版固件上报在 bit15 还是 bit23；CX 忙状态看 bit30 还是 bit23。
5. `CP_ALWAYS_ON_COUNTER` 与 GMU AO 计数器是否相等（S8 记录）。
6. GPU 写经 WB+IS 映射时是否会像 A740 那样卡住（V9c）。
7. Gunyah 下 EL1 能否写 LLCC 广播寄存器、能否直接用 AOSS QMP（决定 LLCC 与 ACD）。
8. 本机 TE 实际频率、自动刷新配置、哪些 SSPP 在取 `0xFC800000`、MDP 流在 apps SMMU 上是旁路/恒等/普通翻译（决定 D2 是否要碰 SMMU）。
9. GPU 驱动降低主机 DDR 票后由谁为 CPU、UFS 与显示提供 DDR 下限（本设计默认保持作者实测的第 11 级，M11 实验决定能否降到第 3 级）。
10. Adreno830 目录项的仓库与所有者；社区仓库改动的上游去向。
11. 同步启动可接受的最长时间（若 GMU/ZAP 在本机明显更慢，要改后台线程并让插件等待 IOAcceleratorES）。
12. demet/hub_aon 两个分支能否像 Linux/KGSL 一样不开（单变量实验）。
13. UEFI/ABL 留在 apps DRV-2 上的 CX/MX/DDR 票值：ARC 读回不反映请求值（annibale §22），目前无法得知；它决定 M11 降档的安全下限。

---

## 10. 审阅修订（2026-10-29）

对抗性审阅提出 23 条意见，逐条对照源码、实测记录与 R1/R2 复核结果核实：23 条指出的问题都成立，没有整条驳回；其中 5 条（第 5、13、15、16、19 条）的修正方式没有完全照建议做，理由见 10.2。`IMPLEMENTATION_PLAN.md` 的相应修改见该文第 7 节。

### 10.1 逐条改动

| # | 结论 | 本文改动（节号） | 主要依据 |
| --- | --- | --- | --- |
| 1 | 采纳 | sGFSR、CB0/CB2 FSR、`CP_RB_RPTR/WPTR`、GMU AO bit3/bit5 一律“读 → 写 1 清 → 复读记入状态块”，不作任何阶段的判据或前置条件；致命条件只剩 `CM3_FW_INIT_RESULT` 的 0xE00/超时、GMU2HOST bit15、AO bit0、栅栏/挂死；bit23 只记录；S2CR、SCTLR、LPAC 路由、CLIENTPD 等功能位仍是门槛（3.4、4.5.3、4.5.4、4.7.3、4.9、1.3 第 13 条） | `L/drivers_iommu_arm_arm-smmu_arm-smmu.c:1663-1665`；R2 cp-zap `cp-health-strict`；`KG16/adreno_gen8_gmu.c:1733-1750` |
| 2 | 采纳 | 706 首版不挂；710/712/336/337/112/994 接线失败或回读不符只记录、改轮询；只有 332 是硬依赖；336 无条件写 `CLR=0x00808006`；所有电平中断加“连续无进展即关闭”的风暴保护；PaganiArmCpu 新增 `armcpu_spi_disable/enable`（4.12.1–4.12.3、7.4、8.2 `irq_off`） | `PD/drivers/PaganiArmCpu/ArmCpu.cpp:161-164,804,836`；`KG16/adreno_gen8_hfi.c:811-812`；`AK/adreno8_smmu.c:156-167`、`adreno8_gmu.c:576-580` |
| 3 | 采纳并补强 | 先复位 CB0/CB2（`SCTLR=0`、清 FSR）再挂 710/712；未启用 bank 的中断只清 FSR、不计故障；预检新增“没有有效路由指向 CB0/CB2”（4.5.3、4.5.4、4.12.3） | `AW/docs/progress/2026-10-25-gpu-preflight-pass.md` §3；`AK/adreno8_smmu.c:940` |
| 4 | 采纳 | OOB 应答后以 1 ms 为上限轮询 `0x3D7DFA0[1:0]==0`，不满足则 S5 失败、不碰 GX 寄存器（4.7.8、4.8、8.3 步骤 0x21） | `KG16/adreno_gen8_gmu.c:677-684`；R2 gmu-boot H20 |
| 5 | 部分采纳 | 默认 S8 只跑 `CP_MEM_WRITE`、带 IRQ 的栅栏、V4、V9a；V9b/V9c 只在 `pagani-gpu-cohtest=1` 的独立镜像里跑（3.4、3.6、4.14.2）。执行位置见 10.2 | `NativeAdrenoUserClient.cpp:283-285`（A740 挂死） |
| 6 | 采纳 | 句柄改编码分三步：先改 Turnip 释放顺序并重新登记补丁身份（用户态补丁 M10）、内核槽号 FIFO 隔离（≥4096 次分配才复用）、两者上机后再切换（2.2、5.5、6.5） | `UX/patches/mesa/turnip-darwin-kmd.patch:662-664,720-722,828-876` |
| 7 | 采纳 | 被中断且未完成时返回 `kIOReturnTimeout`，`kIOReturnAborted` 只留给连接关闭，保留 `THREAD_ABORTSAFE`（4.13.3、6.3） | `turnip-darwin-kmd.patch:438-444,449-470` |
| 8 | 采纳并补强 | 16 个作业槽、每个连接独占 1 个保留槽，没有在途作业的调用者绝不收到 Busy；另查出环满时返回的 `kIOReturnNoResources` 同样会让 Turnip 判设备丢失，一并改为“内核内等待/对有在途作业者返回 Busy”（2.2、4.17、5.4、6.3） | `NativeAdrenoSubmit.h:42`；`NativeAdrenoRing.cpp:283,287`；`turnip-darwin-kmd.patch:1099-1127` |
| 9 | 采纳 | `NGPMode` 与 `NGP_VERSION` 不变；新增 `NGP_METHOD_BAND(=4)`、`NGP_METHOD_WAIT_SWAP2(=5)`；`NativeAdrenoHandoffLast` 改为 0x7005；两份 `NativeGOPWire.h` 同步（5.3.4、6.3、7.4、8.5） | `NativeGOPWire.h:17`；`NativeGOPPresenter.cpp:216`；`GpuScanoutClient.h:13,49-52`；`NativeAdrenoPublish.cpp:51,461` |
| 10 | 采纳 | `pagani-gpu-ddr-floor` 默认等于 `pagani-gpu-ddr`；与第 11 条合并后，默认值本身就是作者实测值，M1–M10 不再需要 `rails=1 ddr=11`，M11 用显式实验参数逐个降档（3.6、4.3.3） | 3.6 与 4.3.3 原文自相矛盾 |
| 11 | 采纳 | 主机 ACV 用 BIT(0)，BIT(2) 只留在 GMU 带宽表；SH0/MC0/CN0 主机票永不 valid=0、下限 x=1、y=1；删除“全撤”与断电 D5 的撤票；默认沿用作者实测值；降档改为 M11 单变量实验，验收比较 UFS、CPU 拷贝帧时间、MDP 欠载（1.3、4.2.3、4.3.2、4.3.3、4.16、4.17、5.1、5.2、9.1、9.2） | `S/wf2/secure-cx/dl_linux/drivers_interconnect_qcom_sm8750.c:1024-1029,1037-1040,1087-1090,1135-1138`；`…bcm-voter.c:72-77,107-113`；`AW/docs/progress/2026-10-25-gpu-resume-review.md` §22；R2 secure-cx-08 |
| 12 | 采纳 | M8–M10 的评估镜像固定 `pagani-gpu-perf=7`（607 MHz、bus 6），M8 另留 222 MHz 对照（3.6） | `AW/docs/analysis/2026-10-26-gpu-validation-lessons.md` §2 |
| 13 | 采纳（落法不同） | 4 GiB 光圈从 S3（M2）起就建好，客户端窗口从 M7 起按 1.5 GiB + 3×682 MiB 划分，`pagani-gpu-va=2` 回退；M8 的小组件判据保留（1.3 第 4、20 条，4.6.1、4.6.2、6.2、6.4）。见 10.2 | R1：小组件因 170 MiB 窗口而空白 |
| 14 | 采纳 | 页故障按 FAR 所在窗口归属到连接，只丢该连接（M7 起，`faults` 按连接计数）；全局丢失只限挂死类故障；“关 GX 再冷启动”的最小恢复提前到 M10（2.2、4.12.3、4.13.4、6.3、6.5） | 终止模式加 HUPCF 不挂住 GPU（4.12.3） |
| 15 | 采纳，规则有修正 | 插件维护按方向拆开：finish 只失效 GPU 写过的表面；commit 对 GPU 非一致读的源与 GPU 写的目标做 clean；一致性等级只免掉对应方向；维护耗时计入帧预算（4.14.1、5.1、5.5）。见 10.2 | `UX/drivers/AdrenoMetal/NativeBridgeMetalFrontend.mm:483-497,1154,1188`；`PD` 工作区 `NativeLegacyDisplay.cpp` 的 `sourceMapping()` |
| 16 | 采纳，纠正一处 | `TLBIVA/TLBIVAA/TLBIVAL/TLBIVAAL` 一律单条 64 位写；拆写只允许用于 `SCTLR.M=0` 时的 TTBR；主机测试断言访问宽度（4.5.2、4.6.4）。见 10.2 | `L/…arm-smmu.c:265-266,303,322`；`UX/kits/ArmSmmu4K/ArmSmmu4KProgram.c:207-211` |
| 17 | 采纳 | 336 的上半部只读 INFO、写 CLR、`thread_call_enter`；dbg 队列排空与 CM3 故障处理放到持 HFI 互斥锁的下半部（4.7.5、4.12.3） | 2.3 规定 HFI 全程一把互斥锁 |
| 18 | 采纳 | 强制关断改为 KGSL 顺序：清栅栏 → 复位 CM3 → halt GBIF → 关 GX → RSCC 睡眠（4.7.9） | `KG16/adreno_gen8_gmu.c:1389-1430` |
| 19 | 部分采纳 | 增加 S1 开头的 QMP 可选步，发 `{class: gpu, res: acd, val: 0}` 并记录结果（2.2、3.6、4.7.6、4.17 P3 行、8.2 `qmp`）。默认值见 10.2 | `KG16/adreno_gen8_gmu.c:1760-1777,1838` |
| 20 | 采纳 | 收到 F2H 20（`MEM_ALLOC`）或 110（`GMU_CNTR_REGISTER`）立即以原因码 22 失败；P0 在 `CORE_FW_START` 前发 `FEATURE_CTRL{9,0,0}`，应答非 0 只记录（4.7.5、4.7.6、8.3） | `AW/docs/progress/2026-10-25-gpu-preflight-pass.md` §3–4；`AK/adreno8_hfi.c:706-726`；`KG16/adreno_hfi.h:468,475`；`KG16/adreno_gen8_hfi.c:636-644` |
| 21 | 采纳 | M11 与 DCVS 同时实现 gpuss TSENS 只读采样与封顶，读不到时上限 607 MHz（2.2、4.16） | `LDT:15007-15340,26121-26128`；`AW/docs/analysis/2026-10-26-gpu-validation-lessons.md:21` |
| 22 | 采纳 | CX 计时器在 S4 第一步、装载 GMU 固件之前播种一次，GMU 运行中绝不改写，实现提前到 M3（3.4、4.4.2、8.3） | `KG16/adreno_gen8_gmu.c:1856-1858`；`KG16/adreno_gen8.c:745-747` |
| 23 | 采纳 | (a) 用户态构建与 hostsim 从 M0 起并行、作为 M7 准入（9.1 第 10 条，计划 0.3/M0/M7）；(b) 计划里的“FUSE 非零”改为只记录；(c) cmd-db 快照不得放在栈上（4.1.1，计划 M1）；(d) 归档前先删旧库，并核对成员清单（7.1.2 第 4 步） | `UX/scripts/build-arm-driver.py:119-121` |

### 10.2 未照建议做的部分及理由

1. 第 5 条：审阅建议在 S9 发布之后由后台线程跑 V9b/V9c。本设计改在独立镜像的 S8 末尾、发布之前同步执行：这时还没有任何客户端，结果不受 backboardd 并发提交干扰；若真的挂死，S8 以原因码 18 停下、桌面照常走 CPU 呈现，而不是在已经发布的设备上让合成器跟着丢设备。M10 有了最小恢复之后，超时改为先恢复、再以 `coherent≤1` 继续发布。
2. 第 13 条：审阅给了“移到 M8”或“把判据移到 M10”两个选项。本设计把光圈提前到 S3（M2）、窗口提前到 M7：如果放在 M8，M8 就同时引入“发布”和“光圈”两个变量，违反计划 0.4 的单变量规则；M7 是客户端第一次使用窗口，没有旧状态需要对照。`pagani-gpu-va=2` 保留回退。
3. 第 15 条：审阅建议 commit 时“只清洗 CPU 写过、GPU 以非一致方式读取的表面”。这会漏掉 GPU 写目标上的另一种风险：CPU 缓存里若有该表面的脏行，GPU 以 NC 写入之后这些脏行被逐出，会覆盖 GPU 的结果。所以 GPU 写的表面在 commit 时也要 clean，只有一致性等级 2 才能免。finish 的规则与审阅一致。
4. 第 16 条：审阅把 `TLBIASID` 也列入“单条 `str x`”。`TLBIASID(+0x610)` 与 `TLBIALL(+0x618)` 是 32 位寄存器，Linux 用 `arm_smmu_cb_write`（32 位）写它们（`arm-smmu.c:265-266`），64 位存储会顺带写到相邻的保留字，所以仍用 32 位写；必须 64 位写的是 `TLBIVA/TLBIVAA/TLBIVAL/TLBIVAAL`。
5. 第 19 条：审阅建议把 QMP 握手作为 S1 之前的步骤先发 `val:0`。步骤与消息照做，但默认关闭（`pagani-gpu-qmp=0`），M3 单变量验证通过后才改为默认开：本机 Gunyah 下 AOP msgram（`0x0C300000`）与 IPCC 门铃能否从 EL1 访问未知，访问被拒就是同步外部中止、整机 panic；而 KGSL 在 QMP 不可用时本来就静默跳过（`KG16/adreno_gen8_gmu.c:1767-1768`），说明“不发”是受支持的状态，默认关的代价只是暂时不与安卓逐字一致。

## 11. 实现与本文的差异（集成时记录，2026-10-29）

下列差异是实现时有意做的，代码与主机测试以此为准；没有列出的地方按本文实现。

1. **接口形式**（行为不变）：
   - `A830SmmuSurvey` 自由函数与同名结构体冲突，改为方法 `A830Smmu::survey()`。
   - `a830CxTimerSeed` 返回 `IOReturn`（原写 `void`）；`A830GmuFirmware::load` 加默认参数 `what`，`A830Gmu::oobSet` 加默认参数 `request`；GMU 内存类名为 `NativeAdrenoGmuMemory`（保留 `NativeAdrenoGmuAllocator` 别名），`allocate()` 接收 `NativeAdrenoGmuPlan`。
   - `A830SelfTest::memWrite/coherence` 不再接收 `NativeAdrenoRing&`、`NativeAdrenoGpuContext&`，改由 `A830SelfTestEnv` 的提交回调与 `A830SelfTestVm` 回调注入（V4 需要 VM 内部的“只做 `dsb ishst`”模式）。
   - 中断模块的故障种类叫 `A830IrqFault*`（`A830PowerState.h` 的 `A830FaultCause` 占用了 `A830Fault*`）。
   - `NativeAdrenoUserClient::isScanoutAddress` 的任务参数为 `void *`；`NativeAdrenoSubmit::submit` 加两个带默认值的尾参 `retry_fence`、`context`；`NativeAdrenoRingIo` 加第 7 个字段 `delay_us`。
   - 显示侧的帧映射缓存 `cacheMapping` 的表面参数为 `OSObject *`，返回“槽号+1、高位代数”的映射号；新增 `native_gop_gpu_offer_frame`/`native_legacy_gpu_offer_frame`。
2. **显示与 DCVS 的钩子名**：显示驱动调用 `pagani_display_vsync(te_mach, period_ticks)`、`pagani_display_late(late_ns)`、`pagani_display_underrun(total)`（弱空默认），`A830Dcvs.cpp` 给出强定义并转到 `adreno830_dcvs_vsync/late_frame/underruns`（迟到由 ns 换成 µs）。
3. **提交路径与电源状态**（4.13、4.17）：每次提交前后 `A830PowerState::acquire/release`；转换或恢复进行中时按“环满”处理（不持锁睡 1 ms 后重试，200 ms 后 Busy），设备丢失才返回 NotResponding。成功提交后调用 `A830Fence::submitted(最后序号)`；WPTR 围栏写 200 次仍被丢弃（环中毒）时向恢复报告 `A830FaultWptrFence`。GMU AO 计数器（CX 寄存器）在 P1 完全断电时先唤醒 GPU 再读。
4. **4.11 第 11 步 P2S 超时**：照 Linux 与 KGSL，超时只计数、不再写 `RBBM_CLOCK_CNTL_GLOBAL=0`（时钟保持强制开，不在未完成的传输上撤销覆盖）；成功时照写 0。
5. **1.3 第 8 条**：KGSL 与 Linux 的 hw_init 表实际有 4 处不同（多出 `TPL1_DBG_ECO_CNTL` bit20），`A830HwInit.h` 用 KGSL 的值，差异写在生成的头文件注释里，`scripts/derive-a830-hwinit.py --check` 钉住 10 处已知差异。
6. **HFI**（4.7.6）：M7（`GET_VALUE(GMU_AB_VOTE)`）应答状态非 0 视为“不支持 AB”并继续（与 KGSL 首启一致），超时、NMI、F2H 错误仍致命；P1 的 `PREPARE_SLUMBER` 按 4.17 发 `{bus|0xFFFF0000, level}`，不附带 AB=0；ACD 表之后的 `SET_VALUE(LOG_EVENT_ON)` 不自动发送。
7. **SMMU**（4.5）：扫描输出导入映射为读写、非一致并禁止写回（ARM 页表没有只写权限）；CB0 上半区不用套件建空上半区（会把 EPD1 清 0），按 4.5.4 第 7、9 步的退路写同样的寄存器值；风暴保护只置 `irq_off` 位，不改写原因码。
8. **一致性等级**由编排器在 S8 之后算好调用 `NativeAdrenoVm::setCoherence`，VM 只执行。
9. **编排器**（3.4）：BO 池在 CB2 之前用 `NativeAdrenoMemory::reservePoolBestEffort` 逐次减半预留（下限 64 MiB）；S9 发布拆成“可能失败的准备”与“attach + 注册”两段，准备或 `native_adreno_publish` 失败时丢弃对象、不留半注册服务；电源状态机与恢复在 S8 绑定（cohtest 的 V9b/V9c 超时要能恢复），服务线程仍在 S10 启动；`pagani-gpu-acd=1` 时 S1 先经 QMP 发 `val:1`，AOP 收下才在 S5 发 HFI M3。
10. **3.6 启动参数的补充**（设计未命名，M13 单变量镜像使用；默认值即本文的 P0）：`pagani-gpu-slumber=0/1/2`（0 不睡；1 P1 完全断电；2 P1 保持 CX）、`pagani-gpu-ifpc=0/1`（P2）、`pagani-gpu-ahbto=0/1`（每次 GMU 启动设 CX_AHB 超时检测，4.4.2）。句柄编码切换不是启动参数，是全局变量 `native_adreno_slot_handles`（默认 0，按三步计划在带 M10 的 Turnip 上机后再开）。
11. **显示**（5.3）：`sheng-vsync` 只负责打开节拍，频率由新参数 `pagani-vsync`（默认 120）决定，`pagani-te=1` 时用实测 TE；TE 锁相在帧计数之外还用同一寄存器的读指针行计数校正相位（只看帧计数时检测不到“预测偏晚”的漂移，回放测试暴露）；插件除 MODE 之后取一次 BAND 外每 64 帧重取一次（探针行约 150 s 后才出现）。新参数 `pagani-fb-async=1`（写合并拷贝放到 thread call，0 退回门内拷贝）。
12. **用户态**（5.5）：A8XX 回移补丁按上游 7638039f 改写成原始 CP 包（85fda3b 没有对应的 `tu_cs` 成员函数）；桥补上 `nmb_command_buffer_metal_error_code`；环境文件 `NativeAdrenoMetal.env` 每个进程启动即读（M7 的自检也要 `TU_DEBUG=sysmem,nocb`）；Data 种子底版用卷 UUID 与 GPU 线内核配置一致的 `作者产物/build-63/ram-data/data.dbrdisk`（metal-68 种子的 UUID 不同，挂载会失败）。
13. **构建**（7.1.2）：`NativeAdrenoKernelTime.cpp` 并入继承清单；A830/平台源文件未列入 `GEN8`/`PLATFORM` 时构建直接报错；所有弱默认由所属源文件自己定义（构建脚本不再生成）；退役的 A7xx/sheng 文件已删除，名字留在禁止符号正则里。
14. **启动配置 annibale（`pagani-gpu-profile`，默认 1，`A830Profile.h`；2026-10-30）**：按 REDMI K90 移植（annibale，HackiOS；同一 SM8750/A830、同一 GMU 固件，在 XNU 下走通）已验证的序列与取值实现，范围是 annibale 提取文档 `kernel-sequences.md` 第 14 节取舍表与第 15 节首轮清单；`pagani-gpu-profile=0` 恢复本文的设计值，两种配置都有主机测试。两种配置都保留 13T 实测的部分：`Sm8750GpuPower` 的上电与 PLL、主机 CX/MX 与 DDR 票、SCM `GPU_INIT_REGS`；都走主机写环（RB）。S0 检查取值后立即设置 `adreno830_profile_value`（早于 cmd-db 与投票表），各模块运行时读同一个值，`A830Smmu` 在 S3 读一次后在对象生命周期内沿用。与本文的全部差异：
   - **开关与状态块**（3.6、8.2–8.4）：`pagani-gpu-hwsched` 是保留位，只能为 0，非 0 时 S0 停在步骤 0x49、原因码 24（`A830ReasonUnsupported`，`kIOReturnUnsupported`），这时配置尚未生效，第 0 行不带后缀。状态块的 `profile/profile_set`（偏移 186/187）让第 0 行末尾显示 ` p1`/` p0`；原 `reserved0` 改为 `hfi_late`、原 `reserved1` 改为 `gmu_general11`；S5 诊断页增加 `TCM`、`G11`、`RSC`，S5–S8 诊断页在 HFI 迟到排空后显示 `L<m>/<a>/<e>`。576 字节布局与各钉选偏移不变。
   - **S1 CX_MISC AHB 超时**（4.4.2）：HUB 到 200 MHz 后（`Sm8750GpuPowerOn` 之后）写五个 `CX_AHB_{AON,GMU,CP,VBIF_SMMU,HOST}_CNTL=0x1B12`：先读，读到全 1 视为无应答，已是 `0x1B12` 的不写，写后 `dsb` 并严格回读；失败停在 S1 步骤 0x30、原因 7，reg 为出错寄存器的物理地址，value 为读到的值，expect 为 `0x1B12`。每次唤醒在 CX 上电后再写一次（`opCxPowerOn`，失败即唤醒失败）。设计配置只在 `pagani-gpu-ahbto=1` 时写、不回读，该参数现在 S1 也写一次（原来只在 P1 唤醒后写）。
   - **S3 SMMU**（4.5、4.6.4）：两个 bank 都写 SCTLR `0x27`（无 CFIE、HUPCF、ASIDPNE，回读须为 `0x27`）、TCR `0x00803519`（T0SZ 25、EPD1）、MAIR0 `0xF404FF44`（MAIR1 0）、TTBR1 0（CB0 的保护表忽略）；ACTLR 只写 CB0（取 `pagani-gpu-actlr`，默认 `0x303`），CB2 只读、记录；ASID 仍为 CB 号 + 1。第 12 步和 `disableBank`/`release` 的失效是 `TLBIASID=ASID` 加 `TLBSYNC=1`（设计为 `TLBIALL` 加 `TLBSYNC=~0`）。VM 的 TLB I/O 不转发套件的 `TLBIVA*`、`TLBIASID`、`TLBIALL`，每个 `TLBSYNC` 改为本 bank 的 `TLBIASID` 加 `TLBSYNC=1`，硬件只收到 32 位写；映射成功后也做一次（编程器的 `sync_after_map`：`NativeAdrenoVm` 调 `ArmSmmu4KSyncContextTlb`，`all=0`），同步不完成时映射失败、VM 中毒；设计配置的映射不做 TLB 维护。sACR 从不写，sGFSR 从不写回（数值变化时才记录）；710/712 不挂，两个 bank 记入 `irq_off`，由 DCVS 线程每秒 `poll()`。
   - **S4/S5 GMU**（4.7.1–4.7.3）：B2 依次写 `AO_MASK=~0`、`GMU2HOST_MASK=~0`、`AO_CLR=~0`、`GMU2HOST_CLR=~0`，不开放任何中断，336/337 不挂，致命位由 `pollFatal` 轮询（`report().polled==3`，不置 `irq_off`）。B3 另读 `RSC_STATUS0_DRV0`（`0x3D50010`）bit16，冷启动时发现 RSC 在睡眠也先唤醒。B4 每个 TCM 块写后 `dsb` 并逐字回读，每个 DDR 段逐字节比对（S4 装 DDR 块时同样比对）；不符或读失败让该步以原因 7 失败，reg 为 TCM 物理地址或 DDR 的 GMU 地址，value 为读到的字，expect 为镜像字。B6 之前（CM3 仍在复位）读 `GENERAL_11`（`0x3D7E72C`），非 0 或读失败只置 `A830GmuWarnGeneral11` 并记入 `gmu_general11`（K90 此时不释放 CM3，我们只记录）。`AHB_FENCE_RANGE_0=0x80C808C0`（设计 `0x80C808A0`）。S4 不播种 AO 计数器。B8 先写 `GMU2HOST_CLR=~0`、`GMU2HOST_MASK=~0` 再写 `HFI_CTRL_INIT`，等待 `A830_T_HFI_INIT_ANNIBALE_US`（500 ms；设计 `A830_T_HFI_INIT_US`，100 ms）。
   - **S5 HFI 与投票表**（4.7.5–4.7.7、4.16）：性能表只取 160、222、342 MHz 三档（vlvl `0x32/0x34/0x38`，DDR 档 2/2/3，`bus_min=bus=bus_max`，ACD 为 0），连同“关”共 4 项，负载 24 字，M1 头 `0x0010190F`；板级表与之不符时 S0 拒绝（`A830VotesStepLevels`）；带宽表恒为 x=0。启动消息 M1、M2、M5、M6、M8、M9，不发 M7（AB 永不启用）；M9 即首个 `GX_BW_PERF_VOTE` 为 `{1, 1, 2}`（160 MHz 档、DDR 档 2），电源状态机与 DCVS 都从这一档开始；它和之后每次投票的 bw 只带总线档（设计带 `0xFFFF0000`）。`FEATURE_CTRL`（含 M5）严判：匹配的应答至少 4 字，data 与 error 都须为 0，不论 HFI 版本；其他应答不足 3 字即失败（原因 15，value 为实际字数，expect 为所需字数）；HFI 主版本须为 2；每轮轮询清 `GMU2HOST_INTR_INFO & (bit0|bit2)`。已入队的命令失败后，HFI 每 100 ms 再读一次 msg 与 dbg 队列，共 3 s（`A830Hfi::drainLate`；遇 CM3 故障、NMI、`F2H_MSG_ERR` 或队列损坏提前结束；读到的只记入 `late_*` 统计，失败原因不变）；编排器另在 S6–S8 失败时（流转为 FAULT、写状态块之前）和判定挂死时（`A830RecoveryOps::snapshot`，恢复写状态块之前，CX 在电且 GMU 在运行）调用它，计数进 `hfi_late`。只有 3 个 GPU 档：`pagani-gpu-perf` 大于 3 时被钳到第 3 档（342 MHz），高于 342 MHz 的投票被拒绝，要更高频率只能切回 `pagani-gpu-profile=0`。
   - **OOB**（4.7.8）：S5 置的 `OOB_GPU` 在 S7 不清，持有到下一次冷启动；唤醒与恢复中的 S7 resume 同样不清；只有 P1 进入 slumber 时额外释放 bring-up 持有的那一份（中止时不释放）。
   - **S6 hw_init 首轮列表**（4.8）：按 K90 的顺序：aperture 写 0；`GBIF_HALT`（0x3C45）、`RBBM_GBIF_HALT`（0x0A）各写 0，`dsb` 后回读须为 0（步骤 0x01，原因 7）；紧接着 `RBBM_INT_0_MASK=0`（GPU 中断全不开，所有等待都轮询，也不写 `RBBM_INT_CLEAR_CMD`）；设计第 3–15 步去掉 `UCHE_CCHE_TRAP/WRITE_THRU`（0xF06/0xF04）、0x460、0x500、`CP_PERFCTR_CP_SEL[0]`（0x8D0）、双三次 `TABLE_0`（0xB606）、0x8E50 这些多出的写；`CP_INTERRUPT_STATUS_MASK_GLOBAL=0x007F007F`；SECVID 放最后；然后 `dsb` 和状态检查（步骤 0x18，见下）。每次写 `CP_APERTURE_CNTL_HOST` 都回读比对。`TPL1_DBG_ECO_CNTL` 仍用 13T 的 `0x10100000`；第 9a 步 LLCC 两种配置都跟 `pagani-gpu-llcc`。
   - **annibale 增加的门槛**（第 10 节与 `AGENTS.md`“回读值只记录、不作门槛”在这一配置下的例外）：微码基址与环寄存器每半个写后 `dsb`、回读须相等（SQE 失败就不写 AQE）；环启动前 `CP_RB_RPTR`、`CP_RB_WPTR` 都须为 0（否则 NotReady、原因 6，所以不经 GX 掉电的恢复会被拒绝）；`CP_SQE_CNTL` 回读须为 1（原因 7，环里不写任何字）；经 `A830CpHealth` 钩子（编排器在 S6 绑定），SQE 启动前后各读一次 sGFSR（只读、不写回），启动前已有的位只记录，启动后新出现的位致命（原因 11，reg `0x03DA0048`），读失败也致命；状态检查是 `RBBM_INT_0_STATUS & 0x33C002C2`（bit23 判挂死、原因 18，其余原因 17，bit8 CP_SW 不算）、`CP_INTERRUPT_STATUS_GLOBAL & 0x007F007F`（原因 17）和 GMU 侧故障（钩子：`A830Gmu::fatalSeen`，否则只读地查一次 `GMU2HOST_INTR_INFO` bit15 与 `CM3_FW_INIT_RESULT` 的 NMI 位，全 1 视为无应答；原因 14），在 hw_init 末尾与等待的每一轮都做，有错立即失败。
   - **CP_INIT 判据 200 ms**（4.9）：最多 2000 轮，每轮之后睡 100 µs（设计 2 s）；每轮先做状态检查，`CP_RB_RPTR`、`CP_RB_RPTR_BV` 与两个 memptrs 影子须在环内（否则立即失败），`RPTR == 影子 == 10`、`GPU_CX_BUSY_STATUS` bit23 为 0、`RBBM_STATUS & ~1 == 0` 之后再查一次状态才算空闲；超时按“读指针 → CX_BUSY → RBBM_STATUS”报告哪一项没满足。`exitSecure`、`waitIdle` 用同一判据，轮数为“时限 / 100 µs”。`RBBM_INT_0_STATUS` 是屏蔽后的状态，掩码为 0 时这一半可能恒读 0（K90 同样如此），`CP_INTERRUPT_STATUS_GLOBAL` 那一半仍有效。
   - **S7**（4.11）：`finishSecure` 只做退出安全模式、重写 `RB_GC_GMEM_PROTECT`、首次填上电表，不开 HWCG；显式调用 `enableHwcg` 时触发后先 `dsb`，P2S 轮询 3 × 1 µs，读不到 TXDONE 返回超时（步骤 0x11），时钟覆盖保持打开。
   - **S8 中断 332 不作发布门槛**（4.12.2、4.14.2）：不挂 332，不调用 `A830SelfTest::run`，改为 `memWrite → timestamps → coherence → record`：CP_MEM_WRITE 以轮询到的 fence（16 ms 保险时钟）与 scratch 回读为准，只因中断判据（步骤 0x04 计数、0x05 中断唤醒）不满足不算失败；回读错、超时、一致性探测挂死仍让 S8 失败。332 未挂时 DCVS 线程每秒回收一次已完成的作业。
   - **S10 DCVS 上限 342 MHz**（4.16）：未显式给出 `pagani-gpu-dcvs-max` 时上限 342 MHz（开不开 ACD 都一样），给出即按它（ACD 关时仍不超过 734 MHz，且受上面 3 档表限制）；任一 GPU 传感器 ≥70.0 °C 或没有可用读数时降到 160 MHz，全部低于 60 °C 才立即恢复，85/95 °C 两档不用；GMU 不应答即本次开机停止调频，目标档退回最后应答的档。
   - **HWSCHED 保留**：本版本只有 RB；`pagani-gpu-hwsched` 留给 RB 在 CP_INIT 之后出现无法解释的失败时整套切换（annibale `kernel-sequences.md` 第 5.5–5.7、9.4–9.6、11.1 节），现在只接受 0。
   - **没有照搬的 K90 细节**（保留设计做法）：SMMU 同步预算 10 ms（K90 100 ms）；CB0 的 ACTLR 回读不作判据；SMMU 故障后照设计写回 FSR 继续（K90 不清 FSR、任一故障即停 GPU）；不匹配的短应答也判失败、START 期间出现其他消息即失败、HFI 150 致命、START 超时 5 s、B1 回读 `CM3_SYSRESET` bit0、B7 不因 NMI 提前退出；WPTR 围栏读到 `AHB_FENCE_STATUS==0xFFFFFFFF` 时重试满 200 次再毒化环（K90 立即失败）；S5 的 B7/B8 冷启动失败不做迟到排空（`A830Gmu` 不持有 HFI 对象）。
15. **启动期寄存器追踪**（8.7，2026-10-30）：新增 `pagani-gpu-trace`（默认 1，0 关，2 不压缩；`Adreno830KnobTrace` 为第 26 个开关，`Adreno830BoardFlagPool` 移到 bit 26）。为了行序可复现，S2 先读 `SW_FUSE_VALUE` 再读 `SW_FUSE_FREQ_LIMIT_STATUS`，三处 CB FAR 读取固定为低字在前（原来同一表达式里两次读的先后由编译器决定）。`boot_progress_trace`/`boot_progress_trace_enabled` 的弱默认在 `Adreno830Status.cpp`，构建脚本的弱定义清单随之加两项。
16. **GPU 内存与连接生命周期**（2026-10-30，冻屏排查）：
   - **BO 池的空闲段表**（`NativeAdrenoMemory.cpp`）：原表 512 项，满了以后每次释放（包括本可与邻段合并的）都被丢弃，池容量在本次开机内只减不增，而 `used` 照减，看起来池是空的；约 1000 个大小不一的活 BO 就能把池切出 500 来个空洞（主机复现：3000 个 4 KiB BO 隔一个释放再全部释放后，`used` 为 0，池却只剩 13895/16384 页可用，64 MiB 的 BO 只能退回池外）。现在空闲段按地址有序、始终合并，表有 16384 项；空洞数不超过活 BO 数 + 1，池只在活的池内 BO 少于 16383 个时供货，所以释放永远放得下。`native_adreno_pool_shape` 记空闲段数及峰值、池内活 BO 数、因上限转到池外的分配、丢失的释放（应恒为 0）。退回池外的代价：先在 VM 里找连续物理页，不行再逐页分配并逐页改页属性（int-152：一个表面大小的 BO 90–130 ms；2026-10-31 前期间还持有全局 Memory 锁，现在池外分配与释放不持锁，5.4），占用系统的固定（wired）内存，每页一个 GPU 段。
   - **连接**（6.2）：拒绝打开时解除挂接；`sheng-gpu-metal-opens` 只数崩溃循环；`releaseAll` 释放能释放的全部；孤儿每次挂载、恢复结束与每秒重试；`BoFree` 后撤视图，邮箱的 `BoCpuUnmap` 也撤视图；调用过交接方法的连接关闭时通知 presenter（`native_gop_handoff_closed`，5.3.2，弱定义清单加一项）。
   - **按连接记账**：用户客户端核心按类别记活对象字节（池内自有、池外自有、导入），发布外壳每秒汇总（MEM 行尾部，8.4），核心盒子保存打开它的进程名，孤儿也能指名。
   - **未改、只记录**：内核句柄仍是单调递增（`native_adreno_slot_handles=0`，6.5），Turnip 的 BO 稀疏数组按句柄低 32 位只增不减，每分配一个 BO 约多占 100 字节，开机越久越大；Turnip 已带 M10 的释放顺序（`turnip-darwin-kmd.patch`），按 6.5 的三步计划上机确认后把默认值改为 1 即可封顶（16384 项）。

17. **M14 P0：抢占第 0 阶段与按进程页表 P0 的计量**（2026-10-31）：
   - **接口**（行为不变）：`NativeAdrenoRingWaiter::stamp` 多一个 `flags` 参数（`NativeAdrenoRingSource*` 与 `NativeAdrenoRingStampContext`）；`NativeAdrenoRing::submitFramed` 与 `NativeAdrenoSubmit::submit` 各加一个带默认值（外部进程）的尾参 `source`，只进戳、不改环字；`NativeAdrenoUserClient::setRenderServer`；`NativeAdrenoSubmit::rangesInFlight`；`A830SmmuPlatform` 加第 9 个字段 `fences`，`A830SmmuFault` 加 `sampled/completed/issued`；`native_adreno_note_page_fault` 加三个参数；`Adreno830StatusQueue` 与 `Adreno830StatusClients` 只在末尾加字段（都不在 576 字节的状态块里，块的布局与静态断言不变）；新增 `adreno830_status_aperture`、`adreno830_status_meter`。
   - **行为**：队列计量改为同一时刻一个结算者（4.13.5）；CB0 页故障照旧按 FAR 窗口扣连接，710 上半部读到的在途作业只与之对比、分四类计数（4.12.3；审查 M1 之后，起初的“唯一在途连接记账”已撤回）；`pagani-gpu-status=2` 的诊断页多三个字段（8.4）；诊断页模式（`status=2/3`）下每分钟至多一条计量行（8.8），默认 `status=1` 的周期性日志不变；S3 末尾一条光圈注记（4.5.6）。
   - **对抗性审查后的修改**（2026-10-31）：M1 页故障记账改回按 FAR 窗口（`NativeAdrenoFaultVerdictFor` 只给判定，`NativeAdrenoFaultNote` 计数并返回 FAR 窗口的槽；“唯一在途连接”会在出故障的作业先完成、FAR 落在内核区、内核自用客户端占范围 0 这三种情形下扣到无辜进程，包括 backboardd）；S1 成批结算时 fw、n4 可能偏低，计量行加 `poll`，门槛只采信中断模式（4.13.5、4.17）；S2 计量行只在诊断页模式写、窗口改为 60 s（8.8）；S3 结算者复查有了确定性的主机测试（仅主机测试的 `a830FencePassHook`），竞争用例去掉了掩盖漏查的补调；S4 补上时钟夹紧、深度饱和、`fences` 接线与故障下半部传参、`sw` 边界、最大值向上取整、重新挂接清历史的用例。
