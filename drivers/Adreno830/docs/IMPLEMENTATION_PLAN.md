# Adreno830 实现计划

版本：2026-10-29（与 `docs/ARCHITECTURE.md` 同时编写，同日按审阅意见修订，改动清单见第 7 节；本文所有寄存器序列、取值与依据以架构文档为准，这里只写“做什么、按什么顺序、怎么验收”）。

---

## 0. 约定

### 0.1 标记

沿用架构文档 0.1 的证据等级：【实测·本机】、【实测·同 SoC】、【实测·sheng】、【源一致】、【分歧】、【推断】。本文另用两个标记：

- **〔已实测〕**：该里程碑的上机验收项在这台 PKX110 上已经观察到过（来自作者构建的事实 1–3）；我们的代码只需复现。
- **〔待实测〕**：需要本计划第一次上机才能确认。

### 0.2 路径

`A830/`、`UX/`、`DEV/`、`PD/`、`SOC/`、`SHENG/`、`LDT`、`S/` 的含义同架构文档 0.2。设备仓库指 GPU 工作树 `DEV/`，M0 先切到分支 `a830-driver`（基于 `oneplus13t-512g`）。

### 0.3 通用命令

下面的命令块在各里程碑里用编号引用。所有路径不能带空格（XNU 构建要求）。

**C1 主机测试**（M0 起由本仓库独立驱动，不再依赖 sheng 的检查脚本）：

```sh
cd /Users/xiaochoumao/unixnu-13t/Adreno830
python3 tests/run-host-tests.py --unixnu ../UniXNU \
    --firmware ../UniXNU-Pagani_SM8750-gpu/.cache/firmware/pkx110-gpu     # 没有固件时省略，相关用例跳过并打印 SKIP
```

脚本按 sheng 的 `SHENG/scripts/check-native-adreno-memory.py` 做法：`clang++ -std=c++23 -O2 -Wall -Wextra -Werror -fsanitize=address,undefined`，把 `IOKit/IOTypes.h` 等 5 个头替换为只包含 `tests/XnuMemoryFixture.h` 的桩，C 文件用 `clang -std=c17` 同样的告警与消毒器；逐字比对每个测试打印的 PASS 行；最后用真实 XNU 编译记录把全部内核对象编一遍并按架构文档 7.1.2 第 5 步核对 `nm`（没有树时打印 SKIP）。

**C2 设备输入检查**：

```sh
cd /Users/xiaochoumao/unixnu-13t/UniXNU-Pagani_SM8750-gpu
python3 scripts/prepare-pagani-gpu-inputs.py --check \
    --live-dtb ../一加13T原厂备份/设备信息/运行中设备树.dtb
python3 scripts/test-gpu-inputs.py
```

**C3 内核**：

```sh
cd /Users/xiaochoumao/unixnu-13t/UniXNU
DEV=../UniXNU-Pagani_SM8750-gpu
LM="--local-module driver:Adreno830=../Adreno830"     # DEV/local.toml 另指向本地 sm8750-common 与 Pagani-Drivers
./unixnu defconfig oneplus13t_defconfig -D $DEV
./unixnu tree -D $DEV $LM
./unixnu status --json -D $DEV $LM | python3 -c 'import json,sys; d=json.load(sys.stdin); print([m for m in d["modules"] if m["name"] in ("Adreno830","Sm8750Platform","Stage7Probe","BootProgress")])'
./unixnu kernel -D $DEV $LM
T=out/oneplus13t/ipados-17.7.11-21H461/oneplus13t/tree
xcrun llvm-ar t $T/build/adreno830/libAdreno830.a
xcrun llvm-nm --defined-only $T/build/adreno830/libAdreno830.a | grep -E ' [TWDSBV] _(adreno830_start|adreno830_status|pagani_gpu_start|native_adreno_publish|Sm8750GpuPowerOn)$'
grep -n IOGPUFamily out/oneplus13t/ipados-17.7.11-21H461/oneplus13t/logs/prelink.log     # M7 起
```

**C4 载荷与启动镜像**（每个里程碑换 `--kernel-arg` 与输出名；两条命令带同样的参数）：

```sh
ARGS="--kernel-arg pagani-gpu=1 --kernel-arg pagani-gpu-stop=N --kernel-arg pagani-gpu-status=2"
./unixnu payload -D $DEV $LM $ARGS
./unixnu bootimg -D $DEV $LM $ARGS -o out/a830-mN.img
shasum -a 256 out/a830-mN.img
```

**C5 上机**（由用户在电脑上执行；只临时启动，不刷任何分区）：

```sh
fastboot boot out/a830-mN.img
```

开机后拍下叠加层（第 10–13 行文字）与桌面；需要时再拍一张 30 s 后的。回退：重新 `fastboot boot` 上一个已知可用的镜像（M0 产物）。

**C6 用户态**（M0 起与内核工作并行推进，不上机；Mesa 全量从没在 Apple clang 下编译过，所以“C6 完整编译成功 + hostsim 的 a740/a830 两档通过”是 M7 的准入条件，而不是 M7 才开始的工作；详见 `docs/inputs/userspace/build-userspace-a830.sh`）：

```sh
cd /Users/xiaochoumao/unixnu-13t/UniXNU
export DEVELOPER_DIR=/Applications/Xcode-26.6.app/Contents/Developer
export UNIXNU_GPU_ABI_DIR=/Users/xiaochoumao/unixnu-13t/Adreno830
python3 scripts/build-turnip-darwin.py --target ios
python3 scripts/build-adreno-metal-plugin.py
python3 scripts/make-owner-trust-bundle.py
python3 scripts/add-gpu-plugin-data.py [--publish] --clients WidgetRenderer_Default,PosterBoard \
    --env build/NativeAdrenoMetal.env --prime-list build/prime-list.txt <BASE>.dbrdisk build/data-a830.dbrdisk
# C4 再加：--data-ramdisk build/data-a830.dbrdisk --owner-trust-cache build/adreno-metal-plugin/owner-trust.bundle
```

### 0.4 上机规则

1. 只用 `fastboot boot` 临时启动；不刷分区、不改 userdata（与项目记忆中的分区规则一致，任何分区操作先征得用户同意）。
2. 每次只改一个变量；同一里程碑内的多个上机步骤按表格顺序做，前一步不过不做后一步。
3. 每次上机记录：镜像 sha256、启动参数、叠加层照片、结论；写入 `DEV/porting.toml` 的 `gpu` 区（device 类证据）。
4. 失败时先看叠加层第 0 行的阶段、步骤、IOReturn、寄存器与原因码（架构文档 8.3、8.4），再决定改动；不靠猜测连改多处。

---

## 1. 实测与假设总表

| 领域 | 内容 | 状态 | 依据 |
| --- | --- | --- | --- |
| SMEM | 只读全局分区 `[0x1000,0x9E000)` 与分区表 `[0x1FF000,0x200000)`；SoC id 618 | 【实测·本机】 | 事实 1 |
| cmd-db | 0x81C60000 快照与解析 | 【实测·本机】（作者构建与触控都读过） | R2 secure-cx |
| qfprom | 读 `0x221C8138` 不中止 | 【实测·本机，间接】；取值未知 | `DEC/…pagani_gpu_start.c:108,196` |
| 上电 | GCC/GPUCC/GDSC/GPLL0/PLL0/RCG/分支，CBCR 处理规则 | 【实测·本机】 | 事实 2；`A830/platform/sm8750/Sm8750GpuPower.c` 等同实现 |
| DRV-2 | ACTIVE TCS 协议；作者的 CX/MX 最高档、CN0、DDR 第 11 档（本驱动的默认值；只有 ACV 改用 HLOS 的 BIT(0)，作者投的是 BIT(2)） | 【实测·本机】 | 事实 3；R2 secure-cx-08 |
| DRV-2 | LOW_SVS 与 DDR 第 2/3 档 | 〔待实测〕M11 单变量实验；机制已实测 | 架构 4.3.3 |
| 中断 | 336/337/710/712/112/994 接线 | 〔待实测〕全部可选，失败只记录、改轮询；706 首版不挂 | 架构 4.12 |
| AOSS QMP | 从 EL1 访问 AOP msgram 与 IPCC | 〔待实测〕M3 可选步 | 架构 4.17 |
| SCM | `GPU_INIT_REGS(0x9)` 成功 | 【实测·本机】 | 事实 3 |
| SCM | `CP_SMMU_APERTURE`、PAS 13 三次调用、WAITQ | 【实测·同 SoC】（annibale）/〔待实测〕 | `AW/…a830-command-startup.md:130,171` |
| SMMU | 作者的严格前置检查在本机失败 | 【实测·本机】 | 事实 4 |
| SMMU | ID 值、开机 S2CR/SMR、SCTLR/TCR2/ACTLR 回读行为、CB2/SHCFG=NSH 写法可行、LPAC 必须路由 | 【实测·同 SoC】 | annibale、hummer |
| SMMU | 本机 sACR、实际路由状态 | 〔待实测〕 | M1 预检 |
| GMU | 冷启动、HFI 2.7.1、投票表、OOB | 【源一致】〔待实测〕 | KGSL gen8 + Linux a8xx |
| CP | hw_init、SQE/AQE、CP_ME_INIT | 【源一致】；CP_INIT【实测·同 SoC】 | annibale |
| ZAP | PAS 13、退出安全模式 | 【源一致】；hwsched 路径【实测·同 SoC】 | annibale README §3 |
| 中断 | `armcpu_spi_attach(332)` 接线方式 | 【实测·sheng】 | `SHENG/package/GpuPowerProbe.cpp:660-713` |
| 一致性 | GPU 只读导入页 WB+IS | 【实测·sheng】（A740）；A830〔待实测〕 | V9a |
| 一致性 | GPU 写 WB+IS | A740 上会卡住；A830〔待实测〕 | V9b/V9c |
| ABI/用户态 | Turnip 认 A830、插件与 Turnip 的 ABI v2 | 静态核实（R1）；sheng 上 ABI 实测 | — |
| 用户态构建 | 插件源码 `-Werror` 语法检查 | 本机已做（Xcode 27.1/26.6）；Mesa 全量、Rust 桥未编，M0 起并行编译 | userspace 研究 |
| 显示 | DF（写合并、源映射缓存） | 代码在 `PD` 的 `fast-display` 分支，〔待实测〕 | — |
| 显示 | NGP 交接 | 【实测·sheng】；13T〔待实测〕 | — |
| 显示 | TE、自动刷新、MDP 取址、apps SMMU 上的 MDP 流 | 〔待实测〕 | M12 探针 |
| LLCC | 切片 9/11 激活 | 【源一致】〔待实测〕（Gunyah 下可写性未知） | — |
| 性能数字 | 帧时间、带宽、唤醒延迟等估算 | 【推断】 | perf 研究 |

---

## 2. 测试体系

### 2.1 本仓库主机测试（`tests/`，由 C1 驱动）

| 测试文件 | 状态 | 覆盖 | 引入里程碑 |
| --- | --- | --- | --- |
| `test_memory.cpp`、`test_smmu_memory.cpp`、`test_vm_alias.cpp`、`test_client.cpp` | 保留 | 代际无关 | M0（移植到新驱动脚本） |
| `test_vm.cpp` | 改 | 删除“每 64 次严格回读”的期望；新增“任一客户端页故障不使共享 VM 失效”；4 GiB 光圈（2053 页表、`MaxLeaves=1<<20`）与 `va=2` 回退两种拓扑 | M2、M7 |
| `test_submit.cpp` | 改 | 删除 WC 审计/采样/IB 日志的期望；新增“提交路径零缓存维护、零 TLBI”；16 个作业槽、每连接 1 个保留槽；没有在途作业的调用者在槽满或环满时都不收到 Busy（内核内等待 ≤200 ms），有在途作业的才收到 Busy；`kIOReturnNoResources` 只用于超过环容量的单次提交 | M7 |
| `test_userclient.cpp` | 改 | 窗口表（4 GiB 与 `va=2` 两套）、`wait_fence ≤ lastSubmitted` 校验、`faults` 按连接计数与 FAR→连接归属、全局设备丢失才拒绝新连接、累计打开上限取消（M7）；句柄 FIFO 隔离复用（≥4096 次分配才复用同一槽，M10） | M7、M10 |
| `test_ring.cpp` | 参数化 | Gen8：scratch 0x82C、SRAM init 0x449/0x5E8/0x19F、BV rptr 0x808、stall 表、围栏写 10 µs×200 与失败返回错误；环满时区分“等空间”与“单次提交超容量” | M4、M7 |
| `test_timer.cpp` | 参数化 | GMU AO 0x1F840/0x1F841（窗口 +0x47100/+0x47104），高-低-高 | M6 |
| `test_pwrup.cpp` | 参数化 | Gen8 三元组动态表 | M4 |
| `test_board.cpp` | 改 | 13T DT：GPU 4 个窗口、GMU 2 个窗口、`#iommu-cells=2`、SID 0/1/2/5 | M1 |
| `test_sha256.cpp` | 改 | gen80000 四个固件的长度与 sha256（无固件时 SKIP） | M0 |
| `test_gmumem.cpp`、`test_gmumap.cpp` | 改 | PREALLOC 驱动的 GMU VA 布局（4 段 CACHE 共 0x76000、持久/日志/HFI 区） | M3 |
| `test_smmuctx.cpp`、`test_gmuctx.cpp` | 改 | 注入 `A830Smmu` 编程器；CB2/槽 1/SID5/ASID3，CB0/槽 {0,3}/SID {0,1}/ASID1；CB0 4 GiB 拓扑 | M2 |
| `test_gmuconfig.cpp`、`test_gmuplatform.cpp` | 删除 | 依赖 `ShengGmu.h` | M0 |
| `test_gpu.cpp` | 替换为 `test_start.cpp` | 阶段机：每阶段失败注入、`pagani-gpu-stop`、不重试、失败不发布、S3 后失败把流压回 FAULT；sGFSR/FSR/RPTR/WPTR 残值不让任何阶段失败；S4 第一步播种 CX 计时器且每次开机只做一次；S8 默认不跑 V9b/V9c，`cohtest=1` 时超时以原因码 18 停下 | M0 起逐步扩充 |
| `test_irq.cpp`、`test_cpinit.cpp` | 替换为 `test_a830irq.cpp`、`test_a830cp.cpp` | Gen8 偏移、掩码、上半部只碰三个寄存器；SQE 0x816、AQE 0x818、`CP_SQE_CNTL` 0x815、绝不写 0x808、ME_INIT 10 个 dword；sGFSR/FSR 非零时 CP 照常启动（只记录） | M4、M6 |
| `test_sm8750_power.c` | 已完成（未提交） | 带依赖关系的寄存器模型；上电规则、NOC_FSM=ON 放行、每次读/写/延时的失败注入、断电 D1–D5（含 RCG 停靠）；配套 `mutate_sm8750_power.py` 26 个变异全部杀死 | M1、M13 |
| `test_status.cpp` | 新增 | 每种叠加层行 ≤66 字符；原因码（含 22、23）/步骤号格式；`irq_off`、`sticky`、`qmp` 字段；`seq` 只在变化时递增 | M0 |
| `test_sm8750_rpmh.c` | 新增 | cmd-db ARC/BCM 解析（含哑 gmxc、缺 gbx）；TCS 写序列、完成判据、超时不复用、DRV-2 锁；`Sm8750CmdDb` 快照只能是静态或堆对象（编译期断言不在栈上使用）；主机票编码：ACV 只用 BIT(0)、SH0/MC0/CN0 永不 valid=0（下限 x=1、y=1）、底线等于启动档时不发 TCS | M1 |
| `test_sm8750_scm.c` | 新增 | INTERRUPTED 续调（x6 透传）、WAITQ（GET_WQ_CTX/RESUME）、EBUSY 20×30 ms、`GPU_INIT_REGS` 不看 x1 且接受 -4、PAS 看 x1 | M1、M5 |
| `test_sm8750_socinfo.c` | 新增 | SMEM 模型：读可读页以外立即中止测试；分区表/分区头/条目遍历；SoC id 618；feature_code/pcode | M1 |
| `test_a830smmu.cpp` | 新增 | Gunyah 模型：开机 S2CR `0x03000018`、SMR 全 0、CB 随机值（含 CFIE=1 与非零 FSR）、ASIDPNE 不保留、TBI 强制为 1、ACTLR 在 CACHE_LOCK=1 时写不进、对 CB1 或 SHCFG=0 的 S2CR 写入模拟同步中止（断言我们从不这样写）、首选槽被占时顺延、有效路由指向 CB0/CB2 时拒绝、sGFSR 写回被忽略时不失败（只记录）、CB0/CB2 先复位再挂 710/712、未启用 bank 的中断只清 FSR 不计数、`TLBIVA/TLBIVAL` 必须单条 64 位访问而 `TLBIASID/TLBIALL` 为 32 位、LPAC 路由顺序 | M1、M2 |
| `test_a830gmufw.cpp` | 新增 | 块解析（元数据在前、垃圾 type/value 忽略、越界块报错）；本机固件 15 块的精确布局（无固件时用合成镜像） | M3 |
| `test_a830gmu.cpp` | 新增 | 寄存器模型：B2 先清中断、B6 不回读、B7 的 `0x100` 成功/`0xE00` 早退/100 ms 超时、AO bit3/bit5 与 INFO bit23 不致命、OOB 引用计数、OOB 后 GX 确认（1 ms 未上电即失败且不访问任何 GX 寄存器）、强制关断顺序（清栅栏后立即复位 CM3，先于 GBIF halt 与关 GX） | M3 |
| `test_a830hfi.cpp` | 新增 | 队列下标与环绕、0xFAFAFAFA 填充、整头匹配（去 bit19）、v5 应答、F2H ERR/DEBUG、F2H 20/110 立即以原因码 22 失败、1 s 超时与 NMI 早退、dbg 队列只在持锁线程里排空（336 上半部无条件写 `CLR=0x00808006`、从不碰队列下标）；M1/M2/M5（`{9,0,0}`，非 0 状态只记录）/M6/M8/M9 的逐字编码与头 | M3 |
| `test_a830votes.cpp` | 新增 | GX/CX/依赖票（KGSL 与 Linux 两种写法）、BW 表（x=y 与 x=0）、ACD 表 `0x0FF0`；与 `UX/kits/AdrenoFirmware/AdrenoHfiVotes.c` 在两者定义一致的输入上逐字对照 | M3 |
| `test_hwinit.cpp` | 新增 | `A830HwInit.h` 的表与钉选源一致（生成器 `--check`）；hw_init 写序列与 aperture 用法；GMEM_PROTECT 在安全退出后重写 | M4 |
| `test_a830zap.cpp` | 新增 | MDT 解析（本机文件或合成 ELF）、元数据 0xFCC 拼接、SMC 参数、失败保留资源 | M5 |
| `test_a830fence.cpp` | 新增 | 唤醒先于阻塞、超时、设备丢失；线程被中断时返回 `kIOReturnTimeout`（绝不返回 `kIOReturnAborted`，除非连接正在关闭）；等待期间不持锁、不做 MMIO（替身计数为 0） | M6 |
| `test_a830dcvs.cpp` | 新增 | 用忙度轨迹回放调频策略（升/降/空闲/提升/上限），叠加温度轨迹验证封顶与回落 | M11 |
| `test_sm8750_llcc.c` | 新增 | v6 激活序列与读回放弃规则 | M11 |
| `test_sm8750_tsens.c` | 新增 | tsens2 TM 状态寄存器解析（VALID、12 位有符号 0.1 °C）、8 个传感器取最大、读不到时上限 607 MHz | M11 |
| `test_sm8750_aossqmp.c` | 新增 | QMP 握手、64 字节消息、IPCC 门铃、超时；`val:0`（M3）与 `val:1`（M13） | M3、M13 |
| `test_a830recovery.cpp` | 新增 | 最小恢复：GBIF halt、强制关断、冷启动、hw_init、只发 SET_SECURE_MODE、fence 归位、中断重新使能、60 s 内 3 次转永久丢失 | M10 |

### 2.2 hostsim（`tests/hostsim/`，由 `UX/scripts/check-turnip-darwin-host.py` 驱动）

- M0 起并行（M7 的准入条件）：先在主机上用现有 a740 档跑通 `check-turnip-darwin-host.py`，再打 `docs/inputs/userspace/patches/adreno830-hostsim-a830.diff`，加环境变量 `HOSTSIM_GPU=a830`，切换设备信息 `{0x44050001, 12 MiB, 0x4000000, 0x1FFFFFFFFF000, 0x20000000, 0xE0000000, 16, 6, 1, 5, ClientIova, 0}` 与计时器（GMU AO）；`check-turnip-darwin-host.py` 先跑 a740、再跑 a830，覆盖 `vulkan_smoke.c`、`deferred_bo.cpp` 的 8 种模式、`pipeline_cache*.cpp`、`plugin_smoke.cpp` 与 `HOSTSIM_STALL=17` 看门狗。这样在主机上走一遍 Turnip 的 a8xx 编译与 darwin 后端的 gpu_id=830，不需要硬件。
- M7：hostsim 增加“多进程抢槽”场景：backboardd 持续提交时，另一进程在自己没有在途作业的情况下提交，断言它从不收到 Busy、也不被判设备丢失；WaitFence 被信号打断时 Turnip 继续等待而不判设备丢失。
- M10：hostsim 的软件 CP 增加“栅栏中断”模型（写 WPTR 后在另一线程延迟完成并 `thread_wakeup` 等价物），断言等待路径没有忙轮询；增加 CachedCoherent 模式；增加句柄复用场景（两个线程一边释放一边分配，配合用户态补丁 M10 断言稀疏数组项不会被清掉）。

### 2.3 设备仓库检查

- `DEV/scripts/test-gpu-inputs.py`（M1）：生成头与 DTS、本机运行时 DTB 的 GPU 子树一致；规范化摘要等于 `scripts/data/sm8750-gpu-inputs.json` 记录值。
- 现有 `test-format2.py`、`test-kernel-source.py`、`test-stage7.py` 等在每个里程碑末尾照跑（`路线图.md` 的“常用命令”）。


---

## 3. 里程碑总览

| 里程碑 | 名称 | 终点（`pagani-gpu-stop`） | 叠加层第 0 行（成功时） | 上机前提 |
| --- | --- | --- | --- | --- |
| M0 | 首次编译与空跑 | 0（只校验结构） | `GPU S0 INPUTS STOP ok 0ms` | 无 |
| M1 | 板级输入、上电、安全 CX（复现实测）+ SMMU 只读预检 | 0、1、2、3（预检） | `GPU S3 SMMU STOP ok …` | M0 |
| M2 | GPU SMMU 接管 | 3 | `GPU S3 SMMU STOP ok …` | M1 预检结论 |
| M3 | GMU 冷启动、HFI、GX 上电 | 4、5 | `GPU S5 GMU-HFI STOP ok …` | M2 |
| M4 | 命令处理器 | 6 | `GPU S6 CP STOP ok …` | M3 |
| M5 | ZAP 与退出安全模式 | 7 | `GPU S7 ZAP STOP ok …` | M4 |
| M6 | 中断、栅栏、计时器、一致性自检 | 8 | `GPU S8 FENCE STOP ok …` | M5 |
| M7 | Metal 服务发布 + 用户态第一阶段自检 | 不停 | `GPU RUN 222M …` | M6 + 用户态构建 |
| M8 | **GPU 合成桌面**（CPU 呈现） | 不停 | `GPU RUN … s<增长> f0 h0` | M7 |
| M9 | GPU 扫描输出交接（D1） | 不停 | 同上，NGP 计数增长 | M8 |
| M10 | 提交路径、内存性能与最小恢复 | 不停 | — | M9 |
| M11 | DCVS、温控、主机票降档实验、LLCC | 不停 | `GPU RUN <变化的频率> …` | M10、只读 DPU 探针（M12 步 1） |
| M12 | 120 Hz 与三缓冲翻页（D0/D2） | 不停 | — | M9（D0 可提前并行） |
| M13 | GMEM、挂死恢复、ACD、slumber、IFPC | 不停 | — | M11 |
| M14 | 实验：D3 零拷贝、大页、39 位地址、按进程页表、抢占 | — | — | M13 |

---

## 4. 里程碑详述

### M0 首次编译与空跑

**目标**：Adreno830 作为 UniXNU 组件编进 13T 内核并链接成功；不设 `pagani-gpu` 时设备行为与改动前完全相同；设 `pagani-gpu=1 pagani-gpu-stop=0` 时叠加层第 10 行出现 GPU 状态行。

**改动**

| 仓库 | 文件 | 动作 |
| --- | --- | --- |
| A830 | `NativeAdrenoService.cpp`、`NativeAdrenoGmuService.{h,cpp}`、`NativeAdrenoSmmuService.{h,cpp}`、`NativeAdrenoGmuPlatform.{h,cpp}`、`NativeAdrenoGmuConfig.{h,cpp}`、`NativeAdrenoGpu.{h,cpp}`、`Info.plist`、`tests/test_gmuconfig.cpp`、`tests/test_gmuplatform.cpp`、`tests/test_gpu.cpp` | 删除 |
| A830 | `NativeAdrenoKernelTime.h` | 删 `:4` 的 `#include "NativeAdrenoGmuPlatform.h"` |
| A830 | `NativeAdrenoAccelerator.cpp:9` | 包含改为 `../AdrenoMetal/OriginalAcceleratorABI.h` |
| A830 | `NativeAdrenoPublish.cpp` | `native_gop_handoff_call`、`native_gop_handoff_memory`、`boot_log_export_put` 的弱默认定义（返回 `kIOReturnUnsupported`、`nullptr`、-1） |
| A830 | `NativeAdrenoFirmwareDefaults.cpp` | 增加 `native_adreno_aqe_firmware_image[_size]`、`native_adreno_zap_image[_size]` 弱默认 |
| A830 | `Adreno830Board.h`、`Adreno830Status.{h,cpp}`、`Adreno830Start.{h,cpp}`、`A830Regs.h` | 新增（`A830Regs.h` 由 `docs/inputs/power-gmu/A830PowerGmuRegs.h` 与 `docs/inputs/perf/a830_perf_regs.h` 合并去重，再补 CP/SMMU 偏移） |
| A830 | `scripts/build-adreno830.py`、`tests/run-host-tests.py`、`tests/test_status.cpp`、`tests/test_start.cpp` | 新增。构建脚本用显式源文件清单：M0 起不编 A7xx 专用的 `NativeAdrenoCpInit.cpp`、`NativeAdrenoIrq.cpp`（分别在 M4、M6 被替换并删除），每个里程碑把新文件加进清单；`ar rcs` 之前先删除旧归档（照 `UX/scripts/build-arm-driver.py:119-121`），并断言归档成员与清单一一对应，否则删掉的 A7xx 目标文件会留在归档里被 `-force_load` 链进内核 |
| A830 | `README.md`、`AGENTS.md` | 改为 Adreno830 的说明（职责、构建入口、文档链接） |
| A830 | `platform/sm8750/Sm8750GpuPower.{h,c}`、`tests/test_sm8750_power.c` | 现有未提交文件纳入版本库（行为不变） |
| UX | `components.toml` | Adreno830 条目；AdrenoMetal 依赖改为 `["AdrenoPM4"]` |
| UX | `scripts/build-xnu-4k-baseline.sh:636,782` | `XNU_NATIVE_ADRENO_ARCHIVE` 覆盖 |
| DEV | 新分支 `a830-driver`（基于 `oneplus13t-512g`，带上 512 GB 的 `userdata` LBA） | — |
| DEV | `board.toml` | `include` 加 `Adreno830`、`AdrenoMetal`（加速器源码要包含 tree 里的 `drivers/AdrenoMetal/OriginalAcceleratorABI.h`，所以 M0 就要选中它；它是 payload 阶段组件，不进内核归档）；`archives` 加 `component:Adreno830`、`build-pagani-gpu.py`；`kernel_args` 暂不加 GPU 参数 |
| DEV | `configs/oneplus13t_defconfig` | `CONFIG_XNU_NATIVE_ADRENO=y`、`CONFIG_XNU_NATIVE_ADRENO_ARCHIVE="{tree}/build/adreno830/libAdreno830.a"` |
| DEV | `package/PaganiGpuBoard.cpp`（骨架）、`scripts/build-pagani-gpu.py`（支持 `--no-firmware`）、`scripts/data/sm8750-gpu-inputs.json`、`local.toml` | 新增 |
| DEV | `.cache/firmware/pkx110-gpu/` | 从 `一加13T原厂备份/GPU固件/` 复制 4 个 `gen80000_*`（不入库，确认 `.gitignore` 覆盖） |
| PD | `drivers/Stage7Probe/Stage7Probe.cpp` | 弱默认 `pagani_gpu_start` 与调用（3.1） |
| PD | `drivers/BootProgress/{BootProgress.c,PeripheralStatus.h}` | `boot_progress_gpu_rows`、`boot_progress_gpu_band` 与 1 Hz 绘制（`glyph_pixels`，只在内容变化时画；架构 8.5） |
| PD | `drivers/NativeMobileDisplay/{NativeLegacyDisplay.cpp,NativeLegacyRequest.h}` | CPU 拷贝跳过保留带（每帧只多两次内存读） |
| SOC | `dts/sm8750.dtsi` | `iommus`、SMMU `interrupts`、`qfprom` 节点（架构 7.4） |
| DEV | `dts/oneplus13t.dts` | `reserved-memory`、`&gpu`/`&gmu`/`&gpucc`/`&gpu_smmu`/`&qfprom` 打开（架构 7.3） |

**要写的函数**

```c
/* Adreno830Status.h/.cpp */
extern "C" Adreno830Status adreno830_status;
void     adreno830_status_begin(uint8_t stage);
void     adreno830_status_end(uint8_t stage, uint32_t elapsed_us);
void     adreno830_status_fail(uint8_t stage, uint16_t step, uint16_t reason, IOReturn ior,
                               uint64_t reg, uint64_t value, uint64_t expect);
unsigned adreno830_status_format(const Adreno830Status *, char rows[4][68], unsigned mode);  /* 纯函数 */
void     adreno830_status_publish(void);                                                     /* 排版、seq++、交给叠加层 */
extern "C" __attribute__((weak)) void boot_progress_gpu_rows(const char (*rows)[68], unsigned count) {}
/* Adreno830Start.h/.cpp */
extern "C" IOReturn adreno830_start(const Adreno830Board *);
static IOReturn validateBoard(const Adreno830Board *);      /* magic/version/窗口/中断/固件长度与 sha256 */
static bool     stopAfter(const Adreno830Board *, uint8_t stage);
/* DEV/package/PaganiGpuBoard.cpp */
extern "C" void pagani_gpu_start(void);
static void fillBoard(Adreno830Board *);                     /* M0：只填结构与固件指针 */
static void parseKnobs(Adreno830Board *);                    /* 3.6 的启动参数 */
/* PD BootProgress.c */
void boot_progress_gpu_rows(const char (*rows)[68], unsigned count);
int  boot_progress_gpu_band(unsigned *y0, unsigned *y1);
/* PD NativeLegacyRequest.h：带保留带的行拷贝 */
static inline int native_legacy_copy_rows_band(void *dst, uint64_t dst_bytes, uint64_t dst_stride, const void *src,
    uint64_t src_bytes, uint64_t src_stride, uint32_t width, uint32_t height, uint32_t band_y0, uint32_t band_y1);
```

**主机测试**：C1 全部通过（移植后的保留测试 + `test_status`、`test_start`、`test_sm8750_power`、`test_sha256`（gen80000））。`test_start` 覆盖：结构非法 → `kIOReturnBadArgument` 且不发布；`stop=0` → 状态 `Stopped`、`seq` 递增一次。

**并行的用户态线**（不上机，M7 的准入条件）：从 M0 起按 C6 装工具链（glslang、shaderc、spirv-tools、bison 3、Rust `aarch64-apple-ios`）、打补丁并完整编译 Mesa、metal2vulkan 桥与插件，保存日志与 `Cargo.lock`；随后跑 2.2 的 hostsim（先 a740、再 a830）。编不过时在这条线上解决（必要时装 Xcode 16.4），不阻塞 M1–M6 的内核上机。

**构建**：C1 → C3（检查 `libAdreno830.a` 含 `_adreno830_start`、`_pagani_gpu_start`（强）、弱符号齐全；`logs/component-Adreno830.log` 无告警）→ C4（两份镜像：①不加 GPU 参数；②`--kernel-arg pagani-gpu=1 --kernel-arg pagani-gpu-stop=0`）。

**上机验收**

| 步 | 镜像 | 判据 |
| --- | --- | --- |
| 0 | （若 `PD` 的 `fast-display` 改动还没上过机）只含 `fast-display`、不含本里程碑改动 | 进入桌面、触控正常；记录滑动是否明显变快（DF 收益） |
| 1 | ① | 与步 0 完全一致：锁屏、主屏、触控、按键 |
| 2 | ② | 同上，且第 10 行出现 `GPU S0 INPUTS STOP ok 0ms`；进入桌面后这一行仍清晰可见（保留带生效），滑动界面时该行不闪烁 |

**叠加层**：步 2 第 0 行如上；`pagani-gpu-status=2` 时第 1 行 `SOC - FC - BIN - FW ok`（M0 不读硬件）。

**实测/假设**：只验证编译、链接、钩子与叠加层；不涉及 GPU 硬件。

**完成标准**：两份镜像行为如上；C1、C3 全绿（`llvm-ar t` 的成员清单里没有 A7xx 目标文件）；`porting.toml` 记录镜像哈希与照片。用户态线不计入 M0 的完成标准。**回退**：不加 `pagani-gpu` 即等于无驱动。

### M1 板级输入、上电、安全 CX（复现实测）与 SMMU 只读预检

**目标**：用我们自己的代码走通作者构建已在本机越过的 S0–S2（同样的投票值，只有主机 ACV 改用 HLOS 的 BIT(0)，架构 4.3.2），并第一次读出本机 GPU SMMU 的真实状态（只读）。

**改动**

| 仓库 | 文件 | 动作 |
| --- | --- | --- |
| A830 | `platform/sm8750/Sm8750Rpmh.{h,c}` | 新增：cmd-db 快照（静态存储）与 ARC/BCM 查找、ACTIVE TCS 写 N 条、BCM 编码（GMU 带宽表与主机票分成两个函数：主机 ACV 用 BIT(0)、SH0/MC0/CN0 永不 valid=0，架构 4.3.2） |
| A830 | `platform/sm8750/Sm8750Scm.{h,c}` | 新增：SMC 封装、续调、EBUSY、`is_available`、`GPU_INIT_REGS` |
| A830 | `platform/sm8750/Sm8750SocInfo.{h,c}` | 新增：SMEM 只读解析、速度档 |
| A830 | `platform/sm8750/Sm8750GpuPower.{h,c}` | 已完成：分支判据加 NOC_FSM=ON（`bits[30:28]==2`）放行；改正头文件注释（调用方持有 mx/cx 票）；GCC AHB 回读加 10 µs；PLL0 半启动自愈；trace 指向出错寄存器；`Sm8750GpuPowerOff` |
| A830 | `A830Votes.{h,cpp}` | 新增（本里程碑只做 cmd-db 结果到 ARC/BCM 表的转换与档位表选择） |
| A830 | `A830Smmu.{h,cpp}` | 新增（本里程碑只有 `survey()`，S3 在预检后强制停下） |
| A830 | `Adreno830Start.cpp` | 实现 S0、S1、S2、S3（预检模式） |
| DEV | `scripts/prepare-pagani-gpu-inputs.py`、`package/PaganiGpuInputs.h`、`scripts/test-gpu-inputs.py`、`package/PaganiGpuBoard.cpp` | 生成器与完整填表 |
| SOC | `kits/Sm8750Platform/Rpmh.h` | DRV-2 共享锁 |
| PD | `drivers/PaganiInput/PaganiInput.cpp` | 触控投票包进锁 |

**要写的函数**

```c
/* Sm8750Rpmh.h */
struct Sm8750CmdDb { uint8_t snapshot[0x20000]; uint32_t bytes; };   /* 128 KiB：只能是静态对象或 S0 一次性堆分配，
                                                                         绝不作栈上局部变量（XNU 内核栈 16 KiB） */
int  Sm8750CmdDbCopy(const struct peripheral_io *db, struct Sm8750CmdDb *out);          /* 对齐 32 位读 */
int  Sm8750CmdDbArc(const struct Sm8750CmdDb *, const char *name, uint32_t *addr, uint16_t vlvl[16], unsigned *count);
int  Sm8750CmdDbBcm(const struct Sm8750CmdDb *, const char *name, uint32_t *addr, uint32_t *unit, uint16_t *width, uint8_t *vcd);
struct Sm8750RpmhCmd { uint32_t addr, data; };
int  Sm8750RpmhWriteActive(const struct peripheral_io *drv2, const struct Sm8750RpmhCmd *, unsigned n,
                           uint32_t wait_mask, struct rpmh_result *);                      /* 4.3.1，持锁 */
uint32_t Sm8750BcmCommand(int commit, int valid, uint32_t x, uint32_t y);
/* Sm8750Scm.h */
struct Sm8750SmcResult { int64_t x0; uint64_t x1, x2, x3; uint32_t calls, waitq, busy; };
int  Sm8750ScmCall(uint32_t fid, uint32_t arginfo, const uint64_t args[4], struct Sm8750SmcResult *);
int  Sm8750ScmIsAvailable(uint32_t svc_cmd, int *available);
int  Sm8750ScmGpuInitRegs(uint32_t gpu_req, struct Sm8750SmcResult *);                  /* 成功：x0∈{0,-4} */
/* Sm8750SocInfo.h */
int  Sm8750SocInfoRead(const struct peripheral_io *smem, uint32_t *soc_id, uint32_t *feature_code, uint16_t *pcode);
int  Sm8750SpeedBinRead(const struct peripheral_io *qfprom, uint32_t *bin);
/* A830Smmu.h */
struct A830SmmuSurvey { uint32_t idr0, idr1, idr2, scr0, sacr, sgfsr, smr[24], s2cr[24];
                        uint32_t cb_sctlr[4], cb_actlr[4], cb_tcr[4], cb_fsr[4]; uint64_t cb_ttbr0[4]; };
IOReturn A830SmmuSurvey(const ArmSmmu4KIo *, A830SmmuSurvey *);
/* Adreno830Start.cpp */
static IOReturn stageInputs(Adreno830Device *);  static IOReturn stagePower(Adreno830Device *);
static IOReturn stageSecureCx(Adreno830Device *); static IOReturn stageSmmuSurvey(Adreno830Device *);
```

**主机测试**：`test_sm8750_rpmh`、`test_sm8750_scm`、`test_sm8750_socinfo`、`test_sm8750_power`（加 NOC_FSM 用例）、`test_a830smmu`（只读预检部分）、`test_board`（13T DT）、`test_start`（S0–S3 预检的失败注入）；C2。

**构建**：C1、C2、C3；C4 生成下表的镜像。

**上机验收**（全部带 `pagani-gpu=1 pagani-gpu-status=2`；`pagani-gpu-rails`、`pagani-gpu-ddr`、`pagani-gpu-ddr-floor` 的默认值就是作者实测的票，架构 3.6、4.3.3，不必显式传）

| 步 | 额外参数 | 判据 | 状态 |
| --- | --- | --- | --- |
| 1 | `pagani-gpu-stop=0` | `GPU S0 INPUTS STOP ok`；诊断页 `SOC 618 FC <x> BIN <y> FW ok`、`ARC gfx <n> mx <n> cx <n> gmxc <n>`、`BCM SH0 … MC0 … ACV … CN0 …` | 读取〔已实测〕；解析〔待实测〕 |
| 2 | `pagani-gpu-stop=1` | `GPU S1 POWER STOP ok`；诊断页 `PWR st7 …`、`TCS <slot> st1 …`；记录 `CM3_SYSRESET` 读值（期望 1） | 〔已实测〕 |
| 3 | `pagani-gpu-stop=2` | `GPU S2 SECCX STOP ok`；`SCM x0 0 x1 <记录> n1 FUSE <记录>`（FUSE 只记录，不作判据，架构 4.4.2） | 〔已实测〕 |
| 4 | `pagani-gpu-stop=3` | `GPU S3 SMMU STOP ok`；诊断页三行：ID、`ACR … GFSR … S2C …`、CB0..CB3 状态 | 〔待实测〕：首次读出本机 SMMU |

**判读与决策**：把步 4 的照片与架构 4.5.1 的同 SoC 值逐项比对。若 `sACR.CACHE_LOCK=1`，即可解释事实 4 的 ACTLR 回读失败；若首选槽 0/1/3 的 SMR 有效，按 4.5.3 顺延；若 `sCR0.CLIENTPD=1`，或者有有效路由指向 CB0/CB2，停在 M1，重新评估 SMMU 方案。`sGFSR` 非零或写回后复读仍非零只记录：全局空间由 Gunyah 仿真，写回可能被忽略（架构 4.5.3 第 4 步），不作停机条件。

**完成标准**：四步都按判据通过；预检结果写入 `porting.toml`；本机运行时 DTB 与作者 `d00767f2…` 的 GPU 子树比对结论写入 `DEV/proprietary-files.txt` 注释。**回退**：M0 镜像。

### M2 GPU SMMU 接管

**目标**：按架构 4.5.4–4.5.6 建立 GMU 上下文（CB2/槽 1/SID5/ASID3）与 GPU 上下文（CB0/槽 0、LPAC 槽 3/SID0、SID1/ASID1，默认 4 GiB 光圈、2053 页表），CB0/CB2 复位之后挂上 710/712（可选，706 首版不挂），预留 BO 池；不启动 GMU。

**改动**

| 仓库 | 文件 | 动作 |
| --- | --- | --- |
| A830 | `A830Smmu.{h,cpp}` | `resetBank`、`claimStream`、`programContext`、`routeStream`、`attachLpac`、`grantAperture`、`contextIrq`（带“bank 已启用”标志与风暴保护）、`globalIrq`（写好不挂）、`release`；MMIO 辅助分 32 位与单条 64 位两种（TLBI 只用后者） |
| A830 | `NativeAdrenoSmmuContext.{h,cpp}` | 抽出 `NativeAdrenoSmmuProgrammer` 注入接口；保留表池准备；`MaxLeaves` 改为 `1<<20` |
| A830 | `NativeAdrenoGmuContext.{h,cpp}`、`NativeAdrenoGpuContext.{h,cpp}` | 参数化 bank/asid/slot/sid；GPU 上下文加 AQE 缓冲（`0x180000`/`0x7000`）、SQE `0x1C000`、内核缓冲特权页、TTBR1 空表；`GpuApertureBytes`、`TablePages` 由 `pagani-gpu-va` 决定（默认 4 GiB/2053，`va=2` 为 2 GiB/1027） |
| A830 | `NativeAdrenoVm.{h,cpp}` | 删除每 64 次 `ArmSmmu4KObserveStreams`；attach 时的路由核对改由 `A830Smmu::verifyRoutes` 回调提供 |
| A830 | `Adreno830Start.cpp` | 完整 S3：预检（含“无有效路由指向 CB0/CB2”）→ sGFSR 写回清除（复读只记录）→ sACR → CB2、CB0 复位（`SCTLR=0`、`FSR=0xC00001FE`）→ 挂 710/712（失败只记录）→ 预留池（`pool_mib`，默认 512）→ CB2 → CB0 → LPAC → aperture |

**要写的函数**

```c
struct A830SmmuStream { uint8_t slot, cb; uint16_t sid; };
IOReturn A830Smmu::resetBank(unsigned cb);                               /* SCTLR=0（回读须为 0）、FSR=0xC00001FE（复读只记录） */
IOReturn A830Smmu::claimStream(const A830SmmuStream &);                  /* S2CR=0x20300 → SMR=0x80000000|sid */
IOReturn A830Smmu::programContext(unsigned cb, const ArmSmmu4KContextImage &, uint32_t actlr,
                                  uint64_t ttbr1, A830SmmuReadback *);    /* 4.5.4 第 3–14 步，容忍规则在此实现 */
IOReturn A830Smmu::routeStream(const A830SmmuStream &);                  /* S2CR=0x300|cb，读回掩码校验 */
IOReturn A830Smmu::attachLpac(uint8_t slot, uint16_t sid, unsigned cb);
IOReturn A830Smmu::grantAperture(unsigned cb);                           /* SCM 0x42000C1B，失败只记录 */
void     A830Smmu::contextIrq(void *);  void A830Smmu::globalIrq(void *); /* 4.12.3 */
IOReturn A830Smmu::release(const A830SmmuStream &);                      /* S2CR=0x20300、SCTLR=0、TLBIALL */
static int a830SmmuWrite32(void *, uint32_t off, uint32_t v);            /* 非内联，约束 "r" */
static int a830SmmuWrite64(void *, uint32_t off, uint64_t v);            /* 单条 str x（TLBIVA/TLBIVAL 必须走它，架构 4.5.2） */
```

**主机测试**：`test_a830smmu`（完整，见 2.1）、`test_smmuctx`、`test_gmuctx`、`test_vm`（attach 路由核对改走回调）、`test_start`（S3 中途失败把已认领的流压回 FAULT）。

**构建**：C1、C3、C4。

**上机验收**（参数同 M1 再加 `pagani-gpu-stop=3`）

| 步 | 判据 | 状态 |
| --- | --- | --- |
| 1 | `GPU S3 SMMU STOP ok`；诊断页 `SLOT 0/3/1 S2C 00000300`、`CB0 00000167 <actlr> FSR <记录> CB2 00000167 <记录>`、`ACR … GFSR <记录>`（FSR、GFSR 只记录，不作判据） | 〔待实测〕 |
| 2 | 停在 S3 的状态保持 5 分钟：诊断页第 3 行末尾的 `F<n>`（只计已启用 bank 的 SMMU 故障，开机残值不计；故障时由下半部即时刷新）保持 0；`irq_off` 为 0（710/712 没被风暴保护关掉）；桌面与触控正常 | 〔待实测〕 |
| 3（可选，单变量） | `pagani-gpu-actlr=0x32b`：只看 ACTLR 读回值，其余判据同步 1 | 弄清 1.3 第 2 条的问题 |

**完成标准**：步 1、2 通过。**回退**：M1 步 3 的镜像。**若失败**：按诊断页的步号（架构 8.3）定位到 4.5.4 的具体一步；出现同步外部中止（屏幕停在 panic 页）时，记录 ESR/FAR 与当时写的寄存器，停下来重新评估，不连改多处。怀疑 4 GiB 拓扑时只加 `pagani-gpu-va=2` 做单变量对照。

### M3 GMU 冷启动、HFI 与 GX 上电

**目标**：S4、S5：CX 计时器播种（S4 第一步，装载 GMU 固件之前）、GMU 固件装载、B1–B8、HFI 启动消息 M1/M2/M5（`FEATURE_CTRL{9,0,0}`）/M6/M8/M9、主机 DDR 底线（默认等于启动档，不发 TCS）、`OOB_GPU` 置位与 GX 上电确认；另实现 S1 开头的 AOSS QMP 可选步（默认关）。

**改动**

| 仓库 | 文件 | 动作 |
| --- | --- | --- |
| A830 | `A830GmuFirmware.{h,cpp}` | 新增：块解析、PREALLOC 列表、TCM/DDR 装载 |
| A830 | `NativeAdrenoGmuMemory.{h,cpp}` | 改为 PREALLOC 驱动（4 段 CACHE + 持久/日志/HFI） |
| A830 | `A830Gmu.{h,cpp}` | 新增：`coldBoot`（B1–B8；致命只认 0xE00、bit15、WDOG）、`oobSet/oobClear`、`waitGxOn`（OOB 后 1 ms 内 `0x3D7DFA0[1:0]==0`，否则不碰 GX 直接失败）、`hfiIrq`（上半部只读 INFO、无条件写 `CLR=0x00808006`、排下半部）、`gmuWork`（持 HFI 锁排空 dbg 队列、处理 CM3 故障）、`aoIrq`、`forceOff`（KGSL 顺序）、`rsccSleep/rsccWake`（P1 用，先写好不调用） |
| A830 | `A830Hfi.{h,cpp}` | 新增：队列、发送、等待、F2H 处理（20/110 立即以原因码 22 失败）、dbg 排空（只在持锁线程里）、`start()`（M1、M2、M5 关闭形式、M6、M8、M9） |
| A830 | `A830Votes.{h,cpp}` | GX/CX/依赖票、BW 表、ACD 表（先不发） |
| A830 | `platform/sm8750/Sm8750AossQmp.{h,c}` | 新增：QMP 握手与 64 字节消息；本里程碑只发 `{class: gpu, res: acd, val: 0}`，`pagani-gpu-qmp=1` 时在 S1 开头执行 |
| A830 | `Adreno830Start.cpp` | S4（第一步 CX 计时器播种，每次开机一次）、S5；S5 内 GMU 冷启动最多 3 次；M9 应答后只在底线低于启动档时才调用 `Sm8750RpmhWriteActive`（默认不调用） |

**要写的函数**

```c
IOReturn A830GmuFirmware::parse(const uint8_t *, size_t, A830GmuBlocks *);
IOReturn A830GmuFirmware::load(const A830GmuBlocks &, A830GmuIo &, NativeAdrenoGmuMemory &);
IOReturn A830Gmu::coldBoot(uint32_t *init_result);                 /* B1–B8，返回失败步号 */
IOReturn A830Gmu::oobSet(uint32_t *ack_us);  void A830Gmu::oobClear();
IOReturn A830Gmu::waitGxOn(uint32_t timeout_us);                   /* 4.7.8，1 ms，失败时不访问 GX */
void     A830Gmu::hfiIrq(void *);  void A830Gmu::gmuWork();         /* 4.12.3：上半部只清不处理 */
IOReturn A830Gmu::forceOff();                                      /* 4.7.9（KGSL 顺序） */
void     a830CxTimerSeed(A830CxMisc &);                            /* 4.4.2，S4 第一步，每次开机一次 */
int      Sm8750AossQmpSendAcd(const struct peripheral_io *msgram, const struct peripheral_io *ipcc, int val);
IOReturn A830Hfi::init(volatile uint32_t *shared, uint64_t iova);  /* GMU 复位态下建表、清下标 */
IOReturn A830Hfi::send(uint8_t id, const uint32_t *payload, uint8_t dwords, A830HfiAck *);  /* 整头匹配、v5、1 s */
IOReturn A830Hfi::start(const A830VoteTables &, uint8_t init_level, uint8_t init_bus);       /* M1、M2、M5（{9,0,0}）、M6、M8、M9 */
IOReturn A830Hfi::perfVote(uint8_t level, uint8_t bus);           /* GX_BW_PERF_VOTE */
IOReturn A830Votes::build(const Sm8750CmdDb &, const Adreno830Board &, A830VoteTables *);
```

**主机测试**：`test_a830gmufw`、`test_a830gmu`、`test_a830hfi`、`test_a830votes`、`test_gmumem`、`test_gmumap`、`test_sm8750_aossqmp`（`val:0`）、`test_start`（S5 重试上限 3 次、每次之间强制关断；CX 计时器只播种一次）。

**构建**：C1、C3、C4。

**上机验收**（参数同 M1）

| 步 | 额外参数 | 判据 | 状态 |
| --- | --- | --- | --- |
| 1 | `pagani-gpu-stop=4` | `GPU S4 FWMEM STOP ok` | 软件 |
| 2 | `pagani-gpu-stop=5` | `GPU S5 GMU-HFI STOP ok`；诊断页 `GMU FW 50010019 HFI 20070001 INIT 00000100 <≤100>ms`、`HFI M1 0 M2 0 M6 0 M8 0 M9 0`（M5 的状态只记录）、`OOB <us>us GX 00000000 RPMH 0 AO <记录>`（GX 一项是门槛：OOB 后 1 ms 内 `[1:0]==0`） | 〔待实测〕 |
| 3（仅步 2 失败时） | `pagani-gpu-hfi=1`，再 `=2` | 依赖票写法、START 顺序的单变量对照 | — |
| 4（可选，单变量） | 步 2 的参数再加 `pagani-gpu-qmp=1` | S1 开头 QMP 握手与 `val:0` 消息成功（状态块 `qmp=1`），其余判据与步 2 相同；通过后把 `pagani-gpu-qmp` 默认改为 1。出现同步外部中止即说明 Gunyah 不放行，保持默认 0 并记入 9.2 第 7 条 | 〔待实测〕 |

**判读**：B7 超时且 `INIT=0` → 看 CB2 的 FSR/FAR（GMU 取指失败）与 `GMU2HOST_INTR_INFO`；`(INIT&0xE00)≠0` → NMI；HFI 某条 `st≠0` → 诊断页记录的 F2H 错误码；原因码 22 → 固件在 RB 模式下请求了主机资源（记录消息号 20 或 110），停下来评估；OOB 超时 → GMU 没应答；OOB 应答了但 GX 确认失败（原因码 6、步骤 0x21）→ GX 没上电（检查 `RPMH_POWER_STATE`、主机 mx/cx 票），这一轮没有访问任何 GX 寄存器，不会 panic。

**完成标准**：步 2 通过；同一镜像连续冷启动 3 次都通过。**回退**：M2 镜像。

### M4 命令处理器

**目标**：S6：Gen8 hw_init（架构 4.8 第 1–17 步）、SQE/AQE 基址、环、`CP_SQE_CNTL=1`、`CP_ME_INIT`，2 s 内完成。

**改动**

| 仓库 | 文件 | 动作 |
| --- | --- | --- |
| A830 | `scripts/derive-a830-hwinit.py`、`A830HwInit.h` | 新增：从钉选的 KGSL `adreno-gpulist.h`（`K/`）与 Linux `a8xx_catalog.c` 生成；`--variant kgsl\|linux`、`--check` |
| A830 | `A830Cp.{h,cpp}` | 新增：`hwInit`、`writePipe`、`loadMicrocode`、`startRing`、`startCp`、`waitIdle`、`dumpStall` |
| A830 | `NativeAdrenoRing.{h,cpp}` | `NativeAdrenoRingRegs`（Gen8）；围栏写 WPTR；删除环日志与整环清洗 |
| A830 | `NativeAdrenoPwrup.{h,cpp}`、删除 `NativeAdrenoPwrupRegs.h`、`NativeAdrenoA740HwInit.h`、`NativeAdrenoCpInit.{h,cpp}`、`tests/test_cpinit.cpp` | `buildGen8()`；A7xx 文件删除 |
| A830 | `Adreno830Start.cpp` | S6 |

**要写的函数**

```c
IOReturn A830Cp::hwInit(const A830HwInitConfig &);           /* 4.8，持 MMIO 锁，aperture 用完写回 0 */
void     A830Cp::writePipe(uint8_t pipe, uint32_t reg, uint32_t value);
IOReturn A830Cp::loadMicrocode(uint64_t sqe_iova, uint64_t aqe_iova);
IOReturn A830Cp::startRing(const NativeAdrenoGpuBuffer &ring, const NativeAdrenoGpuBuffer &memptrs);  /* 硬件 RPTR/WPTR 只记录 */
IOReturn A830Cp::startCp(uint64_t pwrup_iova, uint32_t *fence);  /* 前置只查 LPAC 路由；sGFSR/FSR 只记录；SQE_CNTL=1 + ME_INIT */
IOReturn A830Cp::waitIdle(uint32_t timeout_us);                /* 启动期 100 µs 睡眠步进 */
void     A830Cp::dumpStall(Adreno830Status *);                 /* 4.9 超时转储 */
IOReturn NativeAdrenoPwrup::buildGen8(const A830PwrupTables &, void *page, size_t bytes);
```

**主机测试**：`test_hwinit`（含生成器 `--check`）、`test_a830cp`、`test_ring`（Gen8）、`test_pwrup`（Gen8）。

**构建**：C1、C3、C4。

**上机验收**：`pagani-gpu-stop=6` → `GPU S6 CP STOP ok`；诊断页 `CP RPTR 10 WPTR 10 RBBM 00000000 HW 0`、`SQE PC … PROT 0 UNMASK 0`。〔待实测〕。失败时看 SQE PC：停在 `0xE3/0x4A` 即 LPAC 路由问题（回 M2 核对槽 3）。

**完成标准**：连续 3 次冷启动通过。**回退**：M3 镜像。

### M5 ZAP 与退出安全模式

**目标**：S7：PAS 13（三次 SMC）、`CP_SET_SECURE_MODE 0`、重写 `RB_GC_GMEM_PROTECT`、补 pwrup 表、HWCG、`OOB_GPU` 清除。

**改动**

| 仓库 | 文件 | 动作 |
| --- | --- | --- |
| A830 | `A830Zap.{h,cpp}` | 新增：`parseMdt`、`buildMetadata`、`authenticate` |
| A830 | `platform/sm8750/Sm8750Scm.c` | WAITQ 完整路径（INTID 994 中断或 1 ms 轮询 `GET_WQ_CTX`） |
| A830 | `A830Cp.cpp` | `exitSecure`、`programGmemProtect`、`patchPwrup`、`enableHwcg` |
| A830 | `Adreno830Start.cpp` | S7；`OOB_GPU` 在 S7 末尾清除 |

**要写的函数**

```c
IOReturn A830Zap::parseMdt(const uint8_t *, size_t, A830ZapImage *);         /* 1 个 PT_LOAD、hash 段、跨度 0x1000 */
IOReturn A830Zap::authenticate(const A830ZapImage &, uint64_t region, uint64_t bytes, Sm8750SmcResult out[3]);
IOReturn A830Cp::exitSecure(uint32_t timeout_us);                           /* SET_SECURE_MODE 0 + 空闲 */
void     A830Cp::enableHwcg();                                              /* P2S 超时只记录 */
IOReturn A830Cp::patchPwrup(void *page);                                    /* a8xx_patch_pwrup_reglist 等价 */
```

**主机测试**：`test_a830zap`（本机 `gen80000_zap.mbn` 与合成 ELF）、`test_sm8750_scm`（WAITQ/INTERRUPTED 场景）、`test_a830cp`（GMEM_PROTECT 在安全退出之后重写、HWCG 超时不致命）。

**上机验收**：`pagani-gpu-stop=7` → `GPU S7 ZAP STOP ok`；诊断页 `ZAP x0 0 x1 0 step 12 wq <n>`。〔待实测〕。失败：记录三次 SMC 的 `x0/x1`；屏幕停在“S7”且无响应（SMC 不返回）→ 用 `pagani-gpu-stop=6` 回到 M4 状态，标记“TZ 拒绝/挂起”，评估 13T 上 PAS 的可用性（这是 9.1 第 3 条风险）。

**完成标准**：连续 3 次冷启动通过。**回退**：M4 镜像。

### M6 中断、栅栏、计时器与一致性自检

**目标**：S8：接上 GPU 中断 332（唯一的中断硬依赖），用 `CP_MEM_WRITE` + 带 IRQ 的栅栏验证中断唤醒；挂接计时器；执行 V4、V9a（只记录）；比对 CP 常开计数与 GMU AO 计数。V9b/V9c 可能把 GPU 挂死，默认镜像不跑，只在步 2 的独立镜像里跑（架构 4.14.2）。

**改动**

| 仓库 | 文件 | 动作 |
| --- | --- | --- |
| A830 | `A830Irq.{h,cpp}` | 新增：`attach`、`primary`、`faultWork`、`retireWork`、通用风暴保护（连续无进展即设备侧屏蔽 + `armcpu_spi_disable`，架构 4.12.1）；删除 `NativeAdrenoIrq.{h,cpp}` 与 `tests/test_irq.cpp` |
| A830 | `A830Fence.{h,cpp}` | 新增：`wait`（线程被中断时返回 `kIOReturnTimeout`）、`signal`、`markLost` |
| A830 | `NativeAdrenoTimer.{h,cpp}` | GMU AO 计数器 |
| A830 | `Adreno830Start.cpp` | S8：默认自检序列 V4、V9a；`pagani-gpu-cohtest=1` 时在末尾、发布之前追加 V9b/V9c（架构 4.14.2） |
| PD | `drivers/PaganiArmCpu/ArmCpu.cpp` | 新增导出 `armcpu_spi_disable/enable`（架构 4.12.1、7.4）；本驱动弱导入，缺失时只做设备侧屏蔽 |

**要写的函数**

```c
IOReturn A830Irq::attach(uint32_t intid);                 /* armcpu_spi_attach(…, edge=false, …)，校验 readback */
void     A830Irq::primary(void *);                        /* 4.12.3，只碰 0x6A/0x61/0x62 */
IOReturn A830Fence::wait(uint32_t seq, uint64_t deadline_abs);   /* 4.13.3 */
void     A830Fence::signal();                             /* 中断里 thread_wakeup */
IOReturn A830SelfTest::memWrite(NativeAdrenoRing &, uint64_t iova, uint32_t value);
IOReturn A830SelfTest::coherence(NativeAdrenoGpuContext &, bool cohtest, A830CoherenceResult *);   /* V4、V9a；cohtest 时再加 V9b/V9c */
```

**主机测试**：`test_a830irq`（332 与其余电平中断的风暴保护、`armcpu_spi_disable` 存在与缺失两种情况、332 以外接线失败只记录、错误位只入队、上半部不碰 0x81C）、`test_a830fence`（竞态、超时、中断返回 Timeout）、`test_timer`（Gen8）、`test_start`（S8 默认不跑 V9b/V9c）。PD：`drivers/PaganiArmCpu` 的主机测试覆盖 disable/enable 只接受已 attach 的 INTID。

**上机验收**

| 步 | 镜像 | 判据 | 状态 |
| --- | --- | --- | --- |
| 1 | `pagani-gpu-stop=8` | `GPU S8 FENCE STOP ok`；诊断页 `IRQ 332 rb A003 n<≥1> wake <us>us off 00000000`、`COH V4 <p/f> V9a <p/f> V9b - V9c -`、`TS CP <hi:lo> AO <hi:lo>` | 〔待实测〕 |
| 2（独立镜像，可推迟到 M10 之前任何时候） | 步 1 再加 `pagani-gpu-cohtest=1` | 诊断页给出 V9b/V9c 的 p/f；超时时停在 `S8 … r18`、桌面走 CPU 呈现（预期内，不算失败） | 〔待实测〕 |

V4、V9a 失败只决定 M8/M10 的一致性默认值（`io_coherent=0`），不阻塞后续里程碑；V9b/V9c 只在步 2 的镜像里跑，默认镜像从不执行它们，所以它们的结果同样不阻塞 M7 以后的工作，只决定能否开放 `pagani-gpu-coherent=2`。

**完成标准**：步 1 通过：中断唤醒成立（`n` 增长、`wake` 为微秒级）；V4、V9a 结论记录；两种计数器的差值记录。步 2 不是完成条件。**回退**：M5 镜像。


### M7 Metal 服务发布与用户态第一阶段自检

**目标**：S9 发布 `NativeAdrenoGpu` 与 `AGXAccelerator`；S10 先以固定档运行（`pagani-gpu-dcvs=0`）；用户态插件、Turnip、adrenoprobed 在真机上完成第一阶段自检，但合成器仍走 CPU（种子里不放 `.publish`）。

**改动**

| 仓库 | 文件 | 动作 |
| --- | --- | --- |
| A830 | `NativeAdrenoPublish.cpp` | WaitFence 改为无锁 `A830Fence::wait` + 锁内零超时回收，线程被中断时返回 `kIOReturnTimeout`；`mount` 时 `attachStatus`；只有全局设备丢失（挂死类）才让 `newUserClient` 拒绝；取消“每进程累计 8 次打开”，只限并发；窗口表按 4 GiB 光圈（`va=2` 时旧表，架构 6.2）；`status=3` 的报告行取自 `native_adreno_metal_diag`；`Adreno830Status` 注册表属性 |
| A830 | `NativeAdrenoUserClient.cpp` | `submit` 的 `wait_fence` 只校验 `≤ lastSubmitted`；`faults` 按连接计数，SMMU 页故障按 FAR 所在窗口只记到出错的连接（架构 4.13.4）；`Query.output_bits` 用 IDR2.OAS（36）；DeviceInfo 用架构 6.4 的值（`va_size=0xE0000000`） |
| A830 | `NativeAdrenoSubmit.cpp`、`NativeAdrenoRing.cpp` | 删除每次提交的 WC 审计、采样、`publishMapped`、IB 日志、50/200 ms 停滞处理；作业槽 16 个、每连接 1 个保留槽；调用者没有在途作业时槽满或环满都在内核里不持锁等待（≤200 ms），有在途作业的才返回 Busy；环满不再返回 `kIOReturnNoResources`（架构 5.4） |
| A830 | `NativeAdrenoVm.cpp` | 一致性映射按架构 4.14.1（`io_coherent` 取 S8 的 V9a 结论，`pagani-gpu-coherent` 可覆盖） |
| A830 | `Adreno830Start.cpp` | S9：`native_adreno_publish(vm, submit, ring, clock, timer, &info, 32, oas)` + `publishMetal()`（架构 3.3 的顺序）；S10：`pagani-gpu-dcvs=0` 时只下发一次 `perfVote(perf, bus)` |
| A830 | `tests/hostsim/host_gpu_service.cpp` | `HOSTSIM_GPU=a830`（`docs/inputs/userspace/patches/adreno830-hostsim-a830.diff`） |
| UX | 用户态补丁 M1、M2、M3、M4、M5、M9（架构 5.5）；`turnip-darwin-kmd.patch` 映射 830；`adreno-compiler-inputs.json` | 按 `docs/inputs/userspace/build-userspace-a830.sh` 打补丁并重新登记 Mesa 补丁身份 |
| DEV | `board.toml` | （`AdrenoMetal` 已在 M0 选中）`[kernel.prelink] roots` 加 `com.apple.iokit.IOGPUFamily`；`kernel_args` 去掉 `pagani-platform-status=1`，加 `pagani-gpu=1`、`sheng-gpu-addon=1`、`sheng-gpu-metal-opens=0`；`size_mib=192` |
| DEV | Data 种子 | `add-gpu-plugin-data.py`（不带 `--publish`），`--env` 写 `TU_DEBUG=sysmem,nocb` |

**要写的函数**

```c
static IOReturn stageMetal(Adreno830Device *);          /* native_adreno_publish + publishMetal */
static IOReturn publishMetal(void);                      /* IOGPUFamily 载入 → 136 字节检查 → pre/postModLoad → allocate/init → 属性 → attach/register */
static IOReturn stageDcvs(Adreno830Device *);            /* M7：固定档；M11：起调频线程 */
IOReturn NativeAdrenoUserClient::validateWaitFence(uint64_t fence) const;
void     native_adreno_publish_status(const Adreno830Status *);   /* 阶段切换/故障时刷新注册表属性 */
```

**主机测试**：`test_userclient`、`test_submit`、`test_vm`、`test_start`（S9 失败不留半注册的服务）；hostsim：`HOSTSIM_GPU=a830 python3 scripts/check-turnip-darwin-host.py`（先 `python3 scripts/build-turnip-darwin.py --target host`），a740 与 a830 两档都过，含 2.2 的 M7 场景（多进程抢槽、WaitFence 被打断）。

**准入条件**：M0 起并行的用户态线已完成（C6 完整编译成功、hostsim 两档通过）。

**构建**：C1、C3（`prelink.log` 出现 IOGPUFamily）、C6（不带 `--publish`）、C4 加 `--data-ramdisk` 与 `--owner-trust-cache`。

**上机验收**（参数：`pagani-gpu=1 pagani-gpu-status=3 pagani-gpu-dcvs=0 sheng-gpu-addon=1 sheng-gpu-metal-opens=0`；主机票默认就是作者实测值）

| 判据 | 状态 |
| --- | --- |
| 第 0 行 `GPU RUN 222M b<x> g2/b2 s<n> f0 h0`；`s` 在开机后增长（自检提交） | 〔待实测〕 |
| 第 1 行（插件最近的报告行）出现过探针报告 `adrenoprob s34 e0 v4/4 …`（17 步 UC 映射 + 17 步 WC 映射、3 条 `CP_MEM_WRITE` 回读正确，`UX/drivers/AdrenoMetal/NativeAdrenoUserTest.h:120-175`）。报告行只保留最新一条，探针的行可能很快被其他进程覆盖（R1 已指出这一点），看不到时以第 3 行的打开数 `o` 与调用数 `c` 为准（探针每轮 15 次 ABI 调用，两轮再加一次 SetMetadata 共 31 次，R1） | 〔待实测〕 |
| 第 2 行出现 Turnip 冒烟 `T s0 r0 v1024 c1024 g480..544/0`，随后桥自检 `M s… c0 r0 i0 q… F0` | 〔待实测〕 |
| 第 3 行 `CLI o<2–4> c<调用数> s0 NGP 0/0/0` | 〔待实测〕 |
| 桌面仍由 CPU 合成，外观与 M0 相同；10 分钟内 `f0 h0` | 〔待实测〕 |

**判读**：`IOAcceleratorES` 不存在（插件根本不加载）时第 0 行会停在 `S9 METAL FAIL`；`prelink.log` 没有 IOGPUFamily 是最常见原因。种子与内核 UUID 不一致、owner 信任缺 CDHash 时，Data 挂载阶段失败（`native_gpu_addon*_stage/status` 应为 5/0、5/0、1/2，R2 metal-reg）。

**完成标准**：五项判据通过。**回退**：M6 镜像（不发布服务）。

### M8 GPU 合成桌面（CPU 呈现）

**目标**：backboardd 用 A830 合成桌面（发布模式），呈现仍走 DF 的 CPU 写合并拷贝。**这是“GPU 合成桌面”的首个达成点。**

**改动**：Data 种子改为 `--publish`（`.publish`=1，并写入 backboardd 的三项 CA 偏好），其余同 M7。可选：`.clients` 只列 `WidgetRenderer_Default,PosterBoard`。

**主机测试**：同 M7；hostsim `plugin_smoke.cpp` 的发布序列在 a830 档通过。

**构建**：C1、C3、C6（`add-gpu-plugin-data.py` 带 `--publish`）、C4（加 `--data-ramdisk build/data-a830.dbrdisk --owner-trust-cache build/adreno-metal-plugin/owner-trust.bundle`）。

**上机验收**（参数同 M7 再加 `pagani-gpu-perf=7`（607 MHz、bus 6），仍保留 `pagani-gpu-dcvs=0`：调频到 M11 才实现；固定在 222 MHz 会让“瓶颈在显示还是 GPU”的判断失真，annibale 实测提档后绘制耗时缩短，`AW/docs/analysis/2026-10-26-gpu-validation-lessons.md` §2。另用同一镜像以 `pagani-gpu-perf=2`（222 MHz）跑一轮对照）

| 判据 | 状态 |
| --- | --- |
| 第 1 行每 10 s 轮换显示 `PUB tex …` 与 `ST0 c<提交>/<失败> d<完成> …`；`ST0` 的提交与完成同步增长 | 〔待实测〕 |
| 滑动主屏时第 3 行的调用数与第 0 行的 `s` 持续增长（合成器确实在用 GPU） | 〔待实测〕 |
| 锁屏、主屏、应用切换、通知中心、小组件（不再是淡紫色空框：窗口已在 M7 扩为 682 MiB）、壁纸显示正确 | 〔待实测〕 |
| 连续使用 10 分钟：`f0 h0`，SMMU 中断计数不变，`irq_off` 为 0 | 〔待实测〕 |
| 记录：perf=7 与 perf=2 两轮各自的主观流畅度、插件 `Stats.lastMs`、插件缓存维护的字节数与耗时（架构 5.1），作为 5 节“M8 后”决策的依据 | 记录 |
| 可选：故障注入（存在 `NativeAdrenoMetal.diag` 时，adrenoprobed 提交一个读本窗口内未映射 IOVA 的 IB）：只有 adrenoprobed 判设备丢失（它的 `faults` 加 1），backboardd 的 `faults` 不变、桌面继续由 GPU 合成（架构 4.13.4 的故障分级） | 〔待实测〕 |

**失败处理**：花屏先加 `TU_DEBUG=noubwc` 对照（UBWC 参数不一致）。16×4 对齐、内容来自屏幕另一处的色块（图标标签阴影、控制中心材质这类 framebuffer fetch 处）是纹理路径的 CCHE 留着旧数据（GMEM 下是另一个 bin 同一位置的像素）：`patches/mesa/turnip-a830-fixes.patch` 已让 A8XX 在 UCHE 失效旁一并发 `CP_CCHE_INVALIDATE`、帧缓冲反馈的三处刷新也带上它，仍出现就截图记录，并用 `TU_DEBUG=sysmem,nocb` 与 `TU_DEBUG=gmem,nocb` 各跑一轮对照。**不要把 `FD_DEV_FEATURES=has_fs_tex_prefetch=0` 当采样异常的对策**：annibale 在 A830 上实测它消不掉这类色块，反而让控制中心背景变成纯黑（`AW/docs/progress/2026-10-26-gpu-desktop-acceleration.md:42`）；它只用于对照 FS 纹理预取引起的 GPU 页故障（这一路已由回移的 189ee824 规避，`prefetch_sam_helpers_quirk`）。怀疑 `alias.tex` 挂死时在环境文件加 `IR3_SHADER_DEBUG=noaliastex` 对照（谓词段内的已由回移的 84f09016 规避）。挂死看 `GPU LOST` 行的阶段与原因码，再看 `status=2` 的 IRQ 页；进程反复退出时在环境文件加 `MESA_LOG=syslog`，ir3 编译错误会先把着色器写进系统日志再 `abort()`。整体回退到 M7 的种子（不带 `--publish`）即恢复 CPU 合成。

**完成标准**：五项判据通过并记录到 `porting.toml`。**回退**：M7 种子。

### M9 GPU 扫描输出交接（D1）

**目标**：GPU 把合成好的帧拷进扫描缓冲，CPU 不再逐帧拷贝像素。

**改动**

| 仓库 | 文件 | 动作 |
| --- | --- | --- |
| PD | `drivers/Stage7Probe/Stage7Probe.cpp` | `sheng-fb-gpu=1` 时创建 presenter（attach 到 resource service）；`native_legacy_gpu_offer` 调 `native_gop_gpu_offer` |
| PD | `drivers/NativeMobileDisplay/NativeLegacyDisplay.cpp` | 移植 `retireSwap` 早退役；`finishGpuSwap` 路径 |
| PD | `drivers/NativeMobileDisplay/NativeGOPPresenter.cpp` | 帧映射缓存（6 项 LRU，槽号写进 `NGPSwap.reserved`） |
| UX | `scripts/build-stage7-display.py` | 对象表加入 `NativeGOPPresenter.late.o`，并把源文件加入 `build-report.json` 的源哈希 |
| PD、UX | `PD/drivers/NativeMobileDisplay/NativeGOPWire.h` 与 `UX/drivers/GopFramebuffer/NativeGOPWire.h`（同步修改）、`NativeGOPPresenter.cpp`、`UX/drivers/AdrenoMetal/GpuScanoutClient.h`、`GpuScanoutPresenter.h` | `NGPMode` 与 `NGP_VERSION=1` 一个字节都不改；新增 `NGP_METHOD_BAND(=4)` 与 16 字节的 `NGPBand {y0, y1, rows, reserved}`（`static_assert`）；插件在 `MODE` 之后调 `BAND`，失败（Unsupported/BadArgument）就按无保留带处理，否则把拷贝拆成带上、带下两个矩形（架构 8.5） |
| A830 | `NativeAdrenoPublish.cpp` | `NativeAdrenoHandoffLast` 由 0x7003 改为 0x7005（`:51,461`），放行 0x7004（BAND）与 M12 的 0x7005（WAIT_SWAP2） |
| A830 | `NativeAdrenoUserClient.cpp`、`NativeAdrenoMemory.cpp` | `BoImport` 的扫描内存特判：用 presenter 的 `IODeviceMemory` 物理段建 BO，GPU NC 只写 |
| DEV | `board.toml` | `kernel_args` 加 `sheng-fb-gpu=1` |

**要写的函数**

```c
IOReturn NativeAdrenoMemory::importDevice(IOMemoryDescriptor *scanout, uint64_t offset, size_t bytes);  /* 物理段，不 wire */
bool     NativeAdrenoUserClient::isScanoutAddress(task_t, uint64_t address, uint64_t bytes, uint64_t *offset) const;
uint32_t NativeGOPPresenter::cacheMapping(task_t, IOSurface *, IOMemoryDescriptor *);   /* 返回槽号 */
```

**主机测试**：`test_userclient`（扫描内存导入：只接受落在本任务 `0x5343` 映射内的地址、只写权限；隧道 0x7004/0x7005 被转发、0x7006 被拒）；`PD` 的 `drivers/NativeMobileDisplay/Test`（早退役状态机、保留带拆分、`BAND` 方法；旧插件只调 0–3 号方法时行为与改动前逐字节相同）；插件侧用旧内核（`HandoffLast=0x7003`）的替身验证 `BAND` 失败后按无保留带工作。

**构建**：C1、C3（检查 `libNativeLegacyDisplay.late.a` 含 `NativeGOPPresenter.late.o`）、C6（插件含 `BAND` 方法支持）、C4（加 `--kernel-arg sheng-fb-gpu=1`）。

**上机验收**（M8 参数（含 `pagani-gpu-perf=7`）+ `sheng-fb-gpu=1`）

| 判据 | 状态 |
| --- | --- |
| 第 3 行 `NGP <offers>/<done>/<fail>` 中 offers 与 done 随帧增长、fail 为 0 | 〔待实测〕 |
| 插件报告行里不再出现 `P2 ee00002c7`（交接被桩掉时的特征，R1） | 〔待实测〕 |
| 画面正确、无整屏闪烁；记录插件 `Stats.lastMs` 与主观帧率 | 〔待实测〕 |
| 拔掉 `sheng-fb-gpu=1` 能回到 M8 行为 | 〔待实测〕 |

**完成标准**：四项通过。**回退**：去掉 `sheng-fb-gpu=1`。

### M10 提交路径、内存性能与最小恢复

**目标**：每次提交 ≤20 µs、栅栏唤醒 ≤50 µs、等待时无忙等、零逐提交缓存维护（插件按方向维护）；全局设备丢失之后能“关 GX 再冷启动”自动恢复，不再要求重启整机。（4 GiB 光圈与 682 MiB 窗口已在 M2/M7 落地，`faults` 按连接计数已在 M7 落地。）

**改动**

| 仓库 | 文件 | 动作 |
| --- | --- | --- |
| A830 | `NativeAdrenoPublish.cpp`、`NativeAdrenoUserClient.{h,cpp}`、`NativeAdrenoSubmit.cpp`、`NativeAdrenoVm.cpp`、`NativeAdrenoRing.cpp` | 拆锁：每连接锁、VM 锁、环锁；邮箱轮询改事件驱动；句柄槽号按 FIFO 回收并隔离（同一槽至少经过 4096 次分配才复用），编码切换的开关默认关（架构 6.5 的三步，第 ③ 步在下面的步 3 才打开） |
| A830 | `A830Recovery.cpp`（新增）、`A830Gmu.cpp`、`A830Cp.cpp`、`Adreno830Start.cpp` | 最小恢复（架构 4.13.4）：GBIF halt → `forceOff` → S4/S5 冷启动（含 GX 上电确认；CX 未掉电，计时器不重播）→ S6 → 只发 `CP_SET_SECURE_MODE 0` 与收尾 → fence 归位 → `armcpu_spi_enable` → 重新接受提交；60 s 内第 3 次或恢复失败才转为永久丢失；V9b/V9c 超时也改走恢复 |
| A830 | `NativeAdrenoVm.cpp`、`NativeAdrenoSmmuMemory.cpp` | V4 通过时页表页改可缓存、写 PTE 后只 `dsb ishst` |
| A830 | `NativeAdrenoUserClient.cpp`、`NativeAdrenoAccelerator.cpp` | V9a 通过时默认 `io_coherent=1`；V9b+V9c 通过时允许 `pagani-gpu-coherent=2` 置 `CachedCoherent`；在 AGXAccelerator 上发布 `NativeAdrenoCoherentImports=0/1/2` |
| UX | 用户态 M6（视 V9 结论）、M7、M8（按方向维护，架构 4.14.1）、M10（Turnip `darwin_bo_finish` 先清稀疏项再入队与回收，重新登记补丁身份） | 插件与 Turnip 按内核能力开启 |

**主机测试**：`test_userclient`（锁顺序、句柄 FIFO 隔离）、`test_a830recovery`；hostsim 栅栏中断模型、CachedCoherent 模式与句柄复用场景（2.2）；`test_vm`（可缓存页表路径）。

**构建**：C1、C3、C6（按 V9 结论决定是否带用户态 M6；用户态 M8、M10 必带）、C4。

**上机验收**（M9 参数 + `pagani-gpu-status=2`；仍固定 `pagani-gpu-perf=7`）

| 步 | 内容 | 判据 | 状态 |
| --- | --- | --- | --- |
| 1 | 拆锁、事件驱动、插件按方向维护 | 诊断页 `IRQ <n> wait <n> to <≈0> late <≈0> wptr 0`（2026-10-30 起 DCVS 线程每秒存过队列周期后，这一行末尾改为队列计量 `\| q <均>/<最大>ms d<深度> s<慢>`，ARCHITECTURE 4.13.5；`wptr_retries` 仍在状态块与注册表里，判据不变）；采样的提交耗时 ≤20 µs、唤醒 ≤50 µs；插件缓存维护耗时比 M8 记录的下降；30 分钟使用无回归（`f0 h0`） | 〔待实测〕 |
| 2 | 最小恢复：`pagani-gpu-diag=1` 下注入挂死（提交一个永不结束的 `CP_WAIT_REG_MEM`） | 第 0 行依次出现 `GPU LOST …`、`GPU RECOVER 1 S6 <ms>ms`、再回到 `GPU RUN … f<+1>`；桌面在 3 s 内恢复 GPU 合成；`irq_off` 为 0 | 〔待实测〕 |
| 3 | 句柄编码切换（用户态 M10 与 FIFO 隔离都已在步 1 的镜像里跑满 30 分钟之后才打开） | 30 分钟使用无回归；Turnip 报告行无 `submit failed` 与设备丢失 | 〔待实测〕 |

**完成标准**：三步通过。**回退**：上一个镜像（各改动有独立开关时逐项回退）。

### M11 DCVS、温控、主机票降档实验与 LLCC

**目标**：按忙度调频，同时上线温度封顶；用单变量实验判断主机 DDR/CX/MX 票能否从作者实测值往下降（默认值只在实验证明 UFS、CPU 拷贝、显示都不受影响后才改）；启用 LLCC。

**前提**：只读 DPU 探针（M12 步 1，可提前并行做）已经可用，`underruns` 计数能读到；没有它，步 2–4 的“无欠载”无法判定。

**改动**

| 仓库 | 文件 | 动作 |
| --- | --- | --- |
| A830 | `A830Dcvs.{h,cpp}` | 新增：忙度采样、策略、`perfVote`、触摸/迟到帧提升接口、温度封顶（架构 4.16） |
| A830 | `platform/sm8750/Sm8750Tsens.{h,c}` | 新增：tsens2 传感器 1–8 只读采样（TM 基址 `0xC22A000`、`TM_Sn_STATUS +0xA0+4n`），读不到时 DCVS 上限 607 MHz |
| A830 | `platform/sm8750/Sm8750Llcc.{h,c}`、`A830Cp.cpp` | 切片 9/11 激活；`GBIF_SCACHE_CNTL1/0` |
| A830 | `Adreno830Start.cpp` | S10 起调频线程；`pagani-gpu-llcc` 默认改 1（验证后）；主机票按实验结论改默认值 |
| PD | `drivers/PaganiInput/PaganiInput.cpp` | 导出触摸计数（弱符号 `pagani_input_touch_events`），供 DCVS 触摸提升读取 |
| PD | `NativeLegacyDisplay.cpp` | 迟到帧通知（弱回调） |
| DEV | C4 的 `--kernel-arg` | 按下表逐步加实验参数（这些参数从不写进 `board.toml`）；步 1 起不再传 `pagani-gpu-dcvs=0`、`pagani-gpu-perf=7` |

**主机测试**：`test_a830dcvs`（含温度轨迹）、`test_sm8750_tsens`、`test_sm8750_llcc`、`test_sm8750_rpmh`（底线调整序列：任何底线都不发 valid=0 的 SH0/MC0/CN0，ACV 只用 BIT(0)）。

**构建**：C1、C3、C4（每步一个镜像，只差一个 `--kernel-arg`）。

**上机验收**（单变量顺序，每步 30 分钟稳定；步 2–4 每步都和上一步比较三项：UFS——开机到锁屏的耗时与 NativeUfs 诊断计数；CPU 拷贝——关掉 `sheng-fb-gpu` 的对照轮里 CPU 回退拷贝的帧时间；显示——MDP 欠载计数。任何一项变差就撤回该步，默认值不改）

| 步 | 参数变化 | 判据 |
| --- | --- | --- |
| 1 | 去掉 `pagani-gpu-dcvs=0` 与 `pagani-gpu-perf=7`（DCVS 与温控上线） | 诊断页 DCVS 行的档位驻留随负载变化；空闲 64 ms 内回到 160 MHz；滑动时升到 525–734 MHz；10 分钟持续负载（连续滑动或动画循环）下记录温度与档位曲线：最高温度、封顶是否按 85/95 °C 生效、有无反复振荡 |
| 2 | `pagani-gpu-ddr-floor=3`（GMU 接管后主机 DDR 底线降到第 3 级） | 无欠载（`underruns` 不增长）、无挂死；三项比较不变差 |
| 3 | 再加 `pagani-gpu-ddr=2`（启动期主机 DDR 票降到初始档 bus-min） | 同上；GMU 冷启动仍通过 |
| 4 | 再加 `pagani-gpu-rails=0`（主机 CX/MX 改 LOW_SVS） | 同上；GMU 冷启动仍通过；UFS 无超时或错误计数增长 |
| 5 | `pagani-gpu-llcc=1` | 诊断页 LLCC 切片激活位为 1；无挂死；记录帧时间变化 |

**完成标准**：步 1、5 通过；步 2–4 每步都有结论（通过的才把对应默认值改过去，未通过的保持作者实测值并在架构 4.3.3 记下原因）。把 `llcc` 默认改为 1。**回退**：逐步撤回对应参数。

### M12 120 Hz 与三缓冲翻页（D0/D2）

**目标**：TE 锁相、上报真实刷新率（目标 120 Hz）；GPU 拷进后台缓冲、DPU 翻页，无撕裂。D0 不依赖 GPU，可在 M1 之后的任意时间并行开始；其中步 1 的只读探针（欠载计数）是 M11 步 2–4 的前提，必须在 M11 之前完成。

**改动**

| 仓库 | 文件 | 动作 |
| --- | --- | --- |
| PD | `drivers/NativeMobileDisplay/DpuProbe.cpp`（新）；`drivers/BootProgress/BootProgress.c` | 只读探针（架构 5.3.3 第 1 条），`pagani-dpu-probe=1`；结果经新增的 `boot_progress_dpu_rows` 显示在文字行 14–17（与 GPU 行同样只在变化时画、有保留带） |
| PD | `NativeLegacyDisplay.cpp` | TE 锁相 vsync、`getRefreshAttributes` 用测得频率、`pagani-te=1` |
| SOC | `kits/Sm8750Platform/PeripheralIO.h` | 通用 DCS 短写/长写（只在 TE≈60 时用于切 120 Hz 时序） |
| PD | `NativeGOPPresenter.cpp`、两份 `NativeGOPWire.h`（同步修改） | `NGP_VERSION` 仍为 1、已有结构不变；新增 `NGP_METHOD_WAIT_SWAP2(=5)` 与 56 字节的 `NGPSwap2`（`NGPSwap` + `target_offset`，`static_assert`）、三缓冲状态机与 `flip()`（`pagani-fb-flip=1`）；保留带开启时插件顺带用 GPU 把 B0 的带复制到目标缓冲（架构 5.3.4） |
| UX | `drivers/AdrenoMetal/GpuScanoutClient.h`、`GpuScanoutPresenter.h` | 先调 `WAIT_SWAP2`，失败（旧显示驱动）退回 `WAIT_SWAP`；拷贝目的地加 `target_offset` |

**主机测试**：`PD/drivers/NativeMobileDisplay/Test` 增加锁相（时间戳序列回放）与翻页状态机（锁存超时、邮箱语义、回退到 B0）用例。

**构建**：C3、C6（插件先调 `WAIT_SWAP2`，失败退回 `WAIT_SWAP`）、C4（按步加 `pagani-dpu-probe=1`、`pagani-te=1`、`pagani-fb-flip=1`）。内核侧的 `HandoffLast=0x7005` 已在 M9 改好。

**上机验收**

| 步 | 内容 | 判据 |
| --- | --- | --- |
| 1 | `pagani-dpu-probe=1` | 拍下探针页：TE 频率、自动刷新配置、活动 SSPP 与地址、MDP 流的 SMMU 状态 |
| 2 | `pagani-te=1` | 上报刷新率 = 测得的 TE；CA 帧率上限随之提高；无画面异常 |
| 3 | 首次翻页试验（CPU 把 B0 复制到 B1 → flip(B1) → flip(B0)） | 帧计数持续、欠载 bit26 不变、FSR 0 |
| 4 | `pagani-fb-flip=1` | 无撕裂；呈现帧数/TE ≥0.98（连续滑动 10 s） |

**叠加层**：GPU 第 0 行保持 `GPU RUN …`；探针行（文字行 14–17）依次为 `TE <Hz> AR <autorefresh> FC <帧计数增量>/s`、`CTL<n> PIPE <活动管线> FLUSH <值>`、`SSPP <名> SRC0 <地址> STR <步长> FMT <格式>`、`MDP SID <sid> S2C <s2cr> CB <bank> SCT <sctlr> FSR <fsr>`；翻页开启后加一行 `FLIP <次数> LAT <最大/均值 ms> MISS <迟到> UR <欠载>`。

**完成标准**：四步通过。**回退**：去掉对应参数即回到 D1。

### M13 GMEM、恢复完善、ACD、slumber、IFPC

逐项进行，每项一个镜像、一个变量。叠加层：运行时 `GPU RUN …`；恢复注入后第 0 行依次出现 `GPU LOST …`、`GPU RECOVER <次数> <阶段> <ms>ms`，再回到 `GPU RUN … f<+1>`；slumber 时显示 `GPU SLP <已睡 s>s w<唤醒次数> <上次唤醒 ms>ms`（架构 8.4）。

| 子项 | 改动 | 验收 |
| --- | --- | --- |
| a GMEM | 插件环境文件去掉 `sysmem`（只留 `nocb`），再试 `TU_DEBUG=gmem` 定向用例 | 画面正确、无挂死；帧时间与带宽改善（记录） |
| b 恢复完善 | 在 M10 的最小恢复（`A830Recovery.cpp`）之上：故障注入矩阵（页故障、挂死、GMU 看门狗、WPTR 围栏写失败）、恢复与 slumber/IFPC 的交互、恢复耗时优化 | `pagani-gpu-diag=1` 下逐项注入：页故障时 CB0 中断给出 FSR/FAR/SID，只有出错的连接丢失、设备继续运行；挂死类在 3 s 内恢复；60 s 内第 3 次转永久丢失的规则生效 |
| c AHB 超时检测 | CX_MISC `0x1B12`（架构 4.4.2） | 无副作用 |
| d ACD | `Sm8750AossQmp.{h,c}`（M3 已有，这里改发 `val: 1`）+ HFI M3；上限放到 900 MHz | AOSS 握手成功；ACD 表应答状态 0；高档稳定；温度封顶仍生效 |
| e slumber（P1） | 架构 4.17；`Sm8750GpuPowerOff`；SMMU 状态恢复或 CX 保持型 | 熄屏/静止进入、触摸唤醒；记录每步耗时与 RSCC 状态（T9） |
| f IFPC（P2） | pwrup/ifpc/dyn 表、围栏写全覆盖、KEEPALIVE、`FEATURE_CTRL{9,1,0x1680}` | 功耗下降、无唤醒相关挂死 |
| g 速度档 | `pagani-gpu-speedbin=1` | 按熔丝匹配到的组放开更高档；温度策略生效 |

**主机测试**：b 项扩充 M10 的 `test_a830recovery.cpp`（注入矩阵、与 slumber/IFPC 的交互）；d 项扩充 M3 的 `test_sm8750_aossqmp.c`（`val: 1`）；e 项扩充 `test_sm8750_power.c`（断电 D1–D5）与 `test_a830gmu.cpp`（RSCC 睡眠/唤醒、`GENERAL_9` 回写）；f 项扩充 `test_pwrup.cpp`（IFPC 表）与 `test_ring.cpp`（所有 WPTR 写都走围栏写）。

**构建**：每个子项 C1、C3、C4；a 项另需 C6（只改插件环境文件，不重编二进制，不必重做信任包）。

### M14 实验

D3 零拷贝直扫（架构 5.3.5）；2 MiB 块描述符（扩展 ArmSmmu4K）；39 位输入与按需页表；按进程页表（`CP_SMMU_TABLE_UPDATE` + aperture）；抢占（`a8xx_preempt.c`）。每项先写主机模型测试，再单独上机；叠加层沿用 `status=2` 的诊断页，每项在 `MEM`/`IRQ` 行后追加自己的计数（例如 D3 的 `DIRECT <命中>/<回退>`）。

2026-10-31 设计研究（四项都先不做）之后先做了两组计量，不加启动参数、默认行为不变：抢占第 0 阶段（队列计量的作业归属：每个渲染服务器作业前面排着的外部服务时间 fw、超过 4 ms 的个数 n4，MEM 行 ` fw<最大>ms/<n4>`，架构 4.13.5、4.17）；按进程页表 P0（S3 页 ` AP<状态>` 与每次开机一条光圈注记，槽位压力与每秒上下文入口，页故障照旧按 FAR 窗口扣连接，故障时在途的作业只与之对比计数，架构 4.5.6、4.12.3）。完整数字在诊断页模式（`pagani-gpu-status=2/3`，测试镜像都用 2；默认 1 不写）下每分钟至多一条的 ramoops 计量行里（架构 8.8）。上机看：`AP1`、计量行的 `fw/n4`、`ctx`、`conn/nos/ovf/vmr`、`pf`，再决定第 1 阶段与 P1–P7；第 1 阶段的门槛只采信中断取栅栏的数据（设计配置，或 annibale 加 `pagani-gpu-irq=1`；计量行 `poll 0`、IRQ 行没有 `q~`），轮询时 fw、n4 可能偏低（架构 4.13.5）。

---

## 5. 决策点

| 时点 | 需要决定的事 | 依据 |
| --- | --- | --- |
| M1 后 | SMMU 方案是否照架构 4.5 执行；首选槽是否可用；ACTLR 取值 | 预检照片 |
| M3 中 | 依赖票与 START 顺序的写法（`pagani-gpu-hfi`）；`pagani-gpu-qmp` 是否改默认 1 | HFI 应答与 F2H 错误；QMP 可选步结果 |
| M5 中 | PAS 在本机不可用时的出路（停止并上报，不尝试直接写 `SECVID_TRUST_CNTL`） | 三次 SMC 结果 |
| M6 后 | 一致性默认值（V4、V9a；V9b/V9c 来自独立的 cohtest 镜像） | 自检结果 |
| M8 后 | 是否进入 M9（D1）或先做 M12 的 D0 | 帧时间瓶颈在显示还是 GPU：以 `pagani-gpu-perf=7` 的数据为准，222 MHz 一轮只作对照 |
| M11 中 | 主机 DDR 底线与启动档、CX/MX 档位是否离开作者实测值；温度封顶阈值；LLCC 默认 | UFS、CPU 拷贝帧时间、MDP 欠载三项对比；温度与档位曲线；稳定性 |
| M12 后 | D3 是否值得做 | 探针中 MDP 流的 SMMU 状态 |
| 任意时点 | 社区仓库改动走上游还是长期本地覆盖；Adreno830 目录项的仓库所有者 | 与作者沟通 |

## 6. 每个里程碑都要做的收尾

1. C1、C2、C3 全绿；`test-format2.py` 等设备检查照跑。
2. 记录镜像 sha256、启动参数、叠加层照片与结论到 `DEV/porting.toml`（device 证据），主机测试命令与结果（host 证据），源码提交（source 证据）。
3. 更新 `docs/ARCHITECTURE.md` 中被上机结果推翻或确认的条目（把【推断】改成【实测·本机】或修正取值），不在别处另写报告。
4. 提交到各自仓库的开发分支；不推送、不发 PR，除非用户要求。

---

## 7. 审阅修订（2026-10-29）

对抗性审阅的 23 条意见逐条核实后都成立（证据与逐条结论见架构文档第 10 节）。本计划随之做了下列修改；未照建议做的 5 处（第 5、13、15、16、19 条的落法）理由见架构 10.2。

| 审阅条目 | 本计划的改动 |
| --- | --- |
| 1 粘滞状态当门槛 | M1 删除“sGFSR 清不掉就停在 M1”，只保留 `CLIENTPD=1` 与“有效路由指向 CB0/CB2”两个停机条件；M2 验收里 GFSR、FSR 改为只记录；`test_a830smmu`、`test_a830gmu`、`test_a830cp`、`test_start` 覆盖“残值不致命” |
| 2 中断硬依赖与风暴 | M2 不挂 706；M2/M3 的 710/712/336/337 改为可选；M6 新增 PaganiArmCpu 的 `armcpu_spi_disable/enable` 与通用风暴保护，332 是唯一硬依赖；验收看 `irq_off` |
| 3 挂中断早于复位 | M2 的 S3 顺序改为“预检 → sGFSR 写回 → sACR → CB2/CB0 复位 → 挂 710/712 → 预留池 → CB2 → CB0 → LPAC → aperture”；`F<n>` 只计已启用 bank |
| 4 OOB 后不确认 GX | M3 新增 `waitGxOn`（1 ms），验收的 GX 一项改为门槛；判读加“GX 确认失败不会 panic” |
| 5 一致性实验进默认开机 | M6 默认只跑 V4、V9a；V9b/V9c 移到 `pagani-gpu-cohtest=1` 的独立镜像（M6 步 2，不是完成条件） |
| 6 句柄复用竞态 | M10 改为三步：用户态补丁 M10（Turnip 先清稀疏项）+ 内核 FIFO 隔离 → 上机 30 分钟 → 才打开编码切换（M10 步 3）；hostsim 加句柄复用场景 |
| 7 WaitFence 返回 Aborted | M6/M7：中断返回 Timeout；`test_a830fence` 与 hostsim 覆盖 |
| 8 作业槽与 Busy | M7：16 槽、每连接保留 1 槽，无在途作业不收到 Busy，环满不再返回 NoResources；`test_submit` 与 hostsim 多进程场景覆盖 |
| 9 NGP 扩展方式 | M9 改为新增 `NGP_METHOD_BAND(=4)`、`NGPMode` 不变、`HandoffLast=0x7005`、两份 `NativeGOPWire.h` 同步；M12 的 `WAIT_SWAP2` 改为 5 号方法 |
| 10 `ddr=11` 不成立 | 底线默认等于启动档；M1/M3/M7 的参数集不再带 `rails=1 ddr=11`（默认即作者实测值）；M11 步 2、3 分别单独降底线与启动档 |
| 11 主机票语义 | M1 起主机 ACV 用 BIT(0)、SH0/MC0/CN0 永不 valid=0；降档全部移到 M11 的单变量实验，每步比较 UFS、CPU 拷贝帧时间、MDP 欠载；M11 以只读 DPU 探针为前提 |
| 12 固定 222 MHz | M8–M10 的评估镜像加 `pagani-gpu-perf=7`，M8 另跑一轮 222 MHz 对照；“M8 后”决策以 607 MHz 数据为准 |
| 13 小组件判据不可达 | 4 GiB 光圈提前到 M2（S3 建表）、682 MiB 窗口提前到 M7；M10 不再含窗口扩大；`pagani-gpu-va=2` 作回退对照 |
| 14 单客户端故障拖垮桌面 | M7 起 `faults` 按连接计数、页故障按 FAR 归属；M8 加可选故障注入验收；“关 GX 再冷启动”的最小恢复提前到 M10（步 2），M13 只做完善 |
| 15 插件缓存维护 | M8 记录插件维护字节数与耗时；用户态补丁 M8 按架构 4.14.1 的分方向规则在 M10 落地，M10 验收要求维护耗时下降 |
| 16 TLBI 写宽度 | M2 增加 `a830SmmuWrite64`（单条 `str x`），`test_a830smmu` 断言 TLBIVA/TLBIVAL 64 位、TLBIASID/TLBIALL 32 位 |
| 17 HFI 上半部排空 | M3 拆成 `hfiIrq`（只清不处理）与持锁的 `gmuWork` |
| 18 强制关断顺序 | M3 的 `forceOff` 按 KGSL 顺序，`test_a830gmu` 断言 CM3 复位先于 GBIF halt 与关 GX |
| 19 ACD 状态告知 AOP | M3 新增 `Sm8750AossQmp`（`val:0`）与可选步 4，通过后 `pagani-gpu-qmp` 默认改 1；M13 只把消息改为 `val:1` |
| 20 START 后的未请求消息 | M3 的 HFI 对 F2H 20/110 立即失败（原因码 22），P0 发 `FEATURE_CTRL{9,0,0}` |
| 21 DCVS 无温控 | M11 新增 `Sm8750Tsens` 与温度封顶，步 1 验收加 10 分钟持续负载的温度与档位曲线 |
| 22 CX 计时器播种时机 | 从 M10 移到 M3：S4 第一步、装载 GMU 固件之前，每次开机一次，恢复路径不重播 |
| 23 构建与计划 | (a) C6 与 hostsim 从 M0 起并行，作为 M7 准入条件；(b) M1 步 3 的 FUSE 改为只记录；(c) `Sm8750CmdDb` 注明只能静态或堆分配，`test_sm8750_rpmh` 断言；(d) 构建脚本先删旧归档并核对成员清单（M0） |

## 8. 集成记录（2026-10-29）

M1–M13 的模块已全部编进 GPU 线内核并打成镜像，真机验证按 `docs/PROGRESS.md` 的上机清单进行。与本计划不同或补充的地方：

1. **启动参数**：新增 `pagani-gpu-slumber=0/1/2`、`pagani-gpu-ifpc=0/1`、`pagani-gpu-ahbto=0/1`，分别对应 M13 e、f、c 的单变量镜像，默认 0（P0）。
2. **测试表**（`tests/run-host-tests.py`）：`EXPECTED_FAIL` 清空（`test_vm`、`test_gmumap`、`test_submit`、`test_userclient`、`test_client` 已通过）；`RETIRED` 清空（`test_gmuconfig`、`test_gmuplatform`、`test_gpu`、`test_irq`、`test_cpinit` 随代码删除）；`KNOWN` 的 19 个测试钉住 PASS 行；`tests/test_a830regs.cpp` 改名为 `tests/test_a830headers.cpp`，与 `tests/test_a830regs.c` 区分。
3. **构建清单**（`scripts/build-adreno830.py`）：`GEN8` 为 `Adreno830Status/Start` 与 13 个 `A830*.cpp`，`PLATFORM` 为 7 个 `platform/sm8750/*.c`；`REQUIRED` 加入发布外壳、恢复、电源状态、DCVS 与显示钩子的入口；`WEAK` 加入 `armcpu_spi_*`、`pagani_input_touch_events`、`sm8750_rpmh_drv2_owner`、`boot_progress_display_status`。
4. **M7–M8 镜像**：M7 用不带 `--publish` 的种子加 `pagani-gpu-status=3`、`pagani-gpu-dcvs=0`；M8 出两张：板级默认参数（DCVS 开，开箱即用）与评估形式（`pagani-gpu-dcvs=0`、`pagani-gpu-perf=7`）。两张都用用户态构建的 owner 信任包（`--owner-trust-cache`）。
5. **尚未做**：主控补充二（annibale 第 14 节取舍表）的内核改动、`userspace-a830.md` 的用户态缺项（隔离工作流处理中）、UniXNU 中英文驱动页 `docs/drivers/adreno830.md`。
