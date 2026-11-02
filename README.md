# UniXNU-Pagani-Drivers

一加 13T（PKX110 / SM8750）当前启动链使用的驱动与内核库，集中在一个仓库维护。设备配置、构建脚本和 XNU 补丁保留在 [UniXNU-Pagani_SM8750](https://github.com/IssacL04/UniXNU-Pagani_SM8750)；UEFI 板级驱动保留在独立固件仓库。

## 模块

| 目录 | 用途与验证边界 |
| --- | --- |
| `drivers/PaganiArmDTPlatform` | ADT 平台设备注册，包含 13T 适配 |
| `drivers/PaganiArmCpu` | CPU、GIC、定时器；第 63 版已确认八核在线及每核中断记录，长期稳定性待验证 |
| `drivers/PaganiDarwinBoot` | UEFI 到 XNU 的负载与内存交接 |
| `drivers/NativeUfs` | SM8750 UFS、SMMU/DMA 和根卷读取；物理写入关闭 |
| `drivers/NativeMobileDisplay` | NativeLegacyDisplay CPU 呈现、软件刷新；GPU presenter 候选已接线，等待实机验证 |
| `drivers/PaganiInput` | S3910 LBP 触摸、PMIC 电源／音量键与 HID；第 63 版基本触摸正常，多点与边缘手势待专项验证 |
| `drivers/PaganiPanel` | DSI 亮度、显示开关及背光类接入；已通过完整构建，亮度等面板控制仍待真机验收 |
| `drivers/NativeAES` | 当前构建使用的 AES 接口，未单独验收硬件功能 |
| `drivers/NativeVirtioBlock` | 构建与存储依赖，不代表 13T 有可用的 Virtio 硬件 |
| `drivers/NativeRamBlock` | 临时可写 RAM Data，重启后丢失修改 |
| `drivers/NativeKeyStoreEvents` | 易失 KeyStore 状态兼容，不提供硬件密钥或安全存储 |
| `drivers/BootProgress` | 早期启动、控制台与 panic 显示诊断 |
| `drivers/ShengPlatformPower` | 受限 PMGR 启动接口，保留现有类名和 ABI；不等于完整电源管理 |
| `drivers/Stage7Probe` | 原版组件启动与显示交接诊断 |
| `kits/NativePrelink` | 原版组件预链接、Mach-O 与 ABI 工具 |
| `kits/PaganiVerifiedRoot` | 根卷策略与校验库 |

这 14 个驱动／诊断目录与 2 个 kit 收录当前构建所需源码。第 63 版已确认八核在线与基本触摸，面板亮度和完整输入验收仍未完成；未纳入 Adreno、Metal、网络等模块。各版真机记录见设备仓库 README 和 STAGE6.md。

## 使用和开发

设备仓库仍以 `modules/Pagani` 一个子模块固定本仓库。格式 2 在主仓 `components.toml` 中登记社区组件的来源、依赖、构建阶段和输出。为区分官方实现，ArmCpu、ArmDTPlatform、DarwinBoot 和 VerifiedRoot 的目录分别改为 PaganiArmCpu、PaganiArmDTPlatform、PaganiDarwinBoot 和 PaganiVerifiedRoot；类名、文件名、bundle ID 和导出符号保持原样。其他社区组件保留现有名称。

SM8750 的 GCC/UFS、SMMU、SPMI 观察通道和 DSI FIFO 操作由同级 `UniXNU-sm8750-common/kits/Sm8750Platform` 提供。编译器通过构建树的 `kits/` include 路径引用该接口；分区策略和外设接线由设备配置提供。

从同级 `UniXNU/` 主仓执行：

```sh
./unixnu tree -D ../UniXNU-Pagani_SM8750 --local ../UniXNU-Pagani-Drivers
./unixnu kernel -D ../UniXNU-Pagani_SM8750 --local ../UniXNU-Pagani-Drivers
```

同时修改 SoC 时增加 `--local-module soc:sm8750=../UniXNU-sm8750-common`。默认输出为主仓 `out/oneplus13t/ipados-17.7.11-21H461/oneplus13t/`。更新时先提交本仓库，再更新设备 gitlink、主仓组件元数据和设备移植记录。构建方法见[设备 README](https://github.com/IssacL04/UniXNU-Pagani_SM8750)，职责见 [AGENTS.md](AGENTS.md)。

## 来源与许可

初始导入以设备仓库 `69952a9`、基础仓库 `d9460ebbbf43dd7f21b0ef3f62e9437c35b2315c` 的准备结果为准，保留已应用的 ArmCpu、ArmDTPlatform、DarwinBoot、NativeUfs、NativeMobileDisplay 适配。未从主仓 fork 删除通用模块。

各模块原始 LICENSE 和版权声明均保留，具体许可以对应目录为准。顶层 LICENSE 沿用 UniXNU 的 BSD-2-Clause-Patent。上游来源与导入基线如下：

| 模块 | 来源 | 上游提交 |
| --- | --- | --- |
| `drivers/PaganiArmCpu` | [UniXNU/ArmCpu](https://github.com/UniXNU/ArmCpu) | `1951cc8444b340d7d8879e9418220cf29eb711d7` |
| `drivers/PaganiArmDTPlatform` | [UniXNU/ArmDTPlatform](https://github.com/UniXNU/ArmDTPlatform) | `0d544705c2e89725caabc0542bb96bc8febd8c70` |
| `drivers/PaganiDarwinBoot` | [UniXNU/DarwinBoot](https://github.com/UniXNU/DarwinBoot) | `85332b317ee80898fc78939ea64e6ca52d3837a3` |
| `drivers/NativeAES` | [UniXNU/NativeAES](https://github.com/UniXNU/NativeAES) | `a34964b8c2fec2e83c8e4f68fb7d6fb7e4b2227b` |
| `drivers/NativeKeyStoreEvents` | [UniXNU/NativeKeyStoreEvents](https://github.com/UniXNU/NativeKeyStoreEvents) | `e59bac4ad1ac8825919ae94da8440a64d8ab90b3` |
| `drivers/NativeMobileDisplay` | [UniXNU/NativeMobileDisplay](https://github.com/UniXNU/NativeMobileDisplay) | `eb9c949164826c2e52f2771eac19e4b8bd8a3dae` |
| `drivers/NativeRamBlock` | [UniXNU/NativeRamBlock](https://github.com/UniXNU/NativeRamBlock) | `7194961a668e59018323b626be4fd7ce0a91e78f` |
| `drivers/NativeUfs` | [UniXNU/NativeUfs](https://github.com/UniXNU/NativeUfs) | `af785c1777161a62931425c2faaa536ce0bebd5b` |
| `drivers/NativeVirtioBlock` | [UniXNU/NativeVirtioBlock](https://github.com/UniXNU/NativeVirtioBlock) | `98909eb9198a4360d7a43be831887cb8b74d814d` |
| `kits/NativePrelink` | [UniXNU/NativePrelink](https://github.com/UniXNU/NativePrelink) | `2b6ffb50117eee820b98aca9bd51b5c7dfb2970e` |
| `kits/PaganiVerifiedRoot` | [UniXNU/VerifiedRoot](https://github.com/UniXNU/VerifiedRoot) | `2854c61f6c4dfda870ac3d93e1f03d9181edb041` |

`BootProgress`、`ShengPlatformPower` 和 `Stage7Probe` 来自设备仓库 `69952a9`。本仓库不包含 Apple IPSW、kernelcache、设备 DTB、厂商固件、密钥、磁盘镜像或构建产物。

The SM8750 SMP candidate validates the full MPIDR affinity and uses atomic interrupt counters. BootProgress serializes normal diagnostics after VM setup, skips contended observations and preserves the first panic across CPUs without waiting for a stopped console owner. `pagani-platform-status=1` uses the existing overlay for registered/online CPU counts, per-core timer/IPI masks and RAM/free/available MiB; touch status remains separately opt-in. Host tests cover console contention and first-panic retention. On build 63, the user confirmed system startup and normal touch with `reg=8 online=8 T=ff I=ff` and 4096 MiB RAM. IPI coverage may include the enabled self-test; directed cross-core stress and long-term stability remain unqualified.

第 62 版在交换等待超时后的 slot 345 查询处触发保护。显示接管现在等待控制台当前写入结束，避免多核日志争锁使待呈现交换失去后续处理；panic 时仍立即退出。确定性回归在旧代码上复现接管失败，修复后通过。slot 345 返回本驱动的逻辑显示状态，原始调用 ABI 由主仓构建脚本核对；这不代表读到了面板硬件时序。[第 63 次完整构建](https://github.com/IssacL04/UniXNU-Pagani_SM8750/actions/runs/40520726310) 已通过；下载后的最终内核确认了 slot 345 绑定和接管重试代码，用户随后确认第 63 版进入系统且触摸正常；持续交换可靠性仍待验证。

GPU 诊断候选在 `pagani-platform-status=1` 的 CPU/RAM 两行后增加 GPU 阶段/返回码、Metal 连接与调用、提交/完成/超时，以及 CPU 显示拷贝率和耗时。GPU snapshot 使用 try-lock 并在锁忙时保留带标记的缓存；方法编号/返回值成对记录。状态采样和栅格化限制为 4 Hz，协商后的 GPU/CPU 呈现避让日志行，日志每 250 ms 只重绘变化字符；旧客户端仍每帧覆盖缓存像素，并分别显示 GPU/CPU 帧率、交接/拷贝和覆盖耗时。板级 CLK 行显示请求频率、受电源条件保护的忙闲采样及未知的活动 SP 数。失败阶段独立保留，panic 后停止采样与绘制。无 GPU 包时保留原两行。Host 控制台边界、帧率与 panic 回归通过；完整构建和实机显示待验证。

GPU 汇总候选在 A830 初始化、自检及服务注册成功后启动已有 NativeGOPPresenter，Metal 配置通过 `sheng-fb-gpu=1` 选择它。presenter 保留帧与映射至完成，并在映射失败或 handoff 客户端关闭时通知 CPU 回退；实际代码的归属、尺寸、完成和释放回归已通过。Run 78 完整内核/Metal 构建和本地镜像回读已通过；该路径尚未在 13T 上完成 GPU 呈现实测。

NativeMobileDisplay 现可记录 GPU 帧的 offer、交付、完成和放弃，并在成功完成后读取既有扫描输出映射。像素采样默认关闭；开启后每次启动最多 3 份、间隔至少 30 秒，每份最多 64×64 个原始 BGRX 像素。采样记录不声称已发生硬件显示 latch；GPU 失败和映射越界不会触发采样。主机回归覆盖生命周期、失败路径、采样限额和像素内容，新增路径尚未实机验证。
