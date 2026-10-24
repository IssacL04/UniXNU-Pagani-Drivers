# UniXNU-Pagani-Drivers

一加 13T（PKX110 / SM8750）当前启动链使用的驱动与内核库，集中在一个仓库维护。设备配置、构建脚本和 XNU 补丁保留在 [UniXNU-Pagani_SM8750](https://github.com/IssacL04/UniXNU-Pagani_SM8750)；UEFI 板级驱动保留在独立固件仓库。

## 模块

| 目录 | 用途与验证边界 |
| --- | --- |
| `drivers/ArmDTPlatform` | ADT 平台设备注册，包含 13T 适配 |
| `drivers/ArmCpu` | CPU、GIC、定时器，当前只启用单核 |
| `drivers/DarwinBoot` | UEFI 到 XNU 的负载与内存交接 |
| `drivers/NativeUfs` | SM8750 UFS、SMMU/DMA 和根卷读取；物理写入关闭 |
| `drivers/NativeMobileDisplay` | NativeLegacyDisplay CPU 呈现、软件刷新；没有 SM8750 GPU 加速 |
| `drivers/NativeAES` | 当前构建使用的 AES 接口，未单独验收硬件功能 |
| `drivers/NativeVirtioBlock` | 构建与存储依赖，不代表 13T 有可用的 Virtio 硬件 |
| `drivers/NativeRamBlock` | 临时可写 RAM Data，重启后丢失修改 |
| `drivers/NativeKeyStoreEvents` | 易失 KeyStore 状态兼容，不提供硬件密钥或安全存储 |
| `drivers/BootProgress` | 早期启动、控制台与 panic 显示诊断 |
| `drivers/ShengPlatformPower` | 受限 PMGR 启动接口，保留现有类名和 ABI；不等于完整电源管理 |
| `drivers/Stage7Probe` | 原版组件启动与显示交接诊断 |
| `kits/NativePrelink` | 原版组件预链接、Mach-O 与 ABI 工具 |
| `kits/VerifiedRoot` | 根卷策略与校验库 |

这 12 个驱动/诊断目录与 2 个 kit 收录当前构建所需源码，不能据此把所有硬件功能列为已完成。未纳入尚未适配本机的 Adreno、Metal、触摸、网络等模块。第 39 版的真机记录见设备仓库 README 和 STAGE6.md；本次拆仓不改变驱动行为。

## 使用和开发

设备仓库以 `modules/Pagani` **一个 Git 子模块**固定本仓库提交，再由 `drivers/<Name>`、`kits/<Name>` 的相对符号链接提供标准模块布局。这是对[官网单模块单仓库规则](https://unixnu.com/contributing/repository)的明确例外，用于减少子仓库数量；模块名与覆盖优先级沿用官方构建器。

建议工作区：

```text
UniXNU_SM8750/
  UniXNU/                   主仓 fork
  UniXNU-Pagani_SM8750/      设备配置和构建入口
  UniXNU-Pagani-Drivers/     本仓库，独立开发检出
  UEFI-Pagani_SM8750/        固件
```

在设备仓库执行以下命令，用同级开发检出覆盖固定子模块：

```sh
./unixnu tree --local ../UniXNU-Pagani-Drivers
./unixnu build kernel payload --local ../UniXNU-Pagani-Drivers
```

省略 `--local` 时使用设备仓库固定的子模块版本。构建输出位于设备仓库 `out/`。测试和构建由设备仓库 `scripts/` 及 Actions 驱动；原模块的测试保留在各目录中。

更新时先提交并推送本仓库，再让设备仓库的 `modules/Pagani` 检出该提交，提交并推送 gitlink。仅推送本仓库不会改变设备仓库的默认构建版本。

## 来源与许可

初始导入以设备仓库 `69952a9`、基础仓库 `d9460ebbbf43dd7f21b0ef3f62e9437c35b2315c` 的准备结果为准，保留已应用的 ArmCpu、ArmDTPlatform、DarwinBoot、NativeUfs、NativeMobileDisplay 适配。未从主仓 fork 删除通用模块。

各模块原始 LICENSE 和版权声明均保留，具体许可以对应目录为准。顶层 LICENSE 沿用 UniXNU 的 BSD-2-Clause-Patent。上游来源与导入基线如下：

| 模块 | 来源 | 上游提交 |
| --- | --- | --- |
| `drivers/ArmCpu` | [UniXNU/ArmCpu](https://github.com/UniXNU/ArmCpu) | `1951cc8444b340d7d8879e9418220cf29eb711d7` |
| `drivers/ArmDTPlatform` | [UniXNU/ArmDTPlatform](https://github.com/UniXNU/ArmDTPlatform) | `0d544705c2e89725caabc0542bb96bc8febd8c70` |
| `drivers/DarwinBoot` | [UniXNU/DarwinBoot](https://github.com/UniXNU/DarwinBoot) | `85332b317ee80898fc78939ea64e6ca52d3837a3` |
| `drivers/NativeAES` | [UniXNU/NativeAES](https://github.com/UniXNU/NativeAES) | `a34964b8c2fec2e83c8e4f68fb7d6fb7e4b2227b` |
| `drivers/NativeKeyStoreEvents` | [UniXNU/NativeKeyStoreEvents](https://github.com/UniXNU/NativeKeyStoreEvents) | `e59bac4ad1ac8825919ae94da8440a64d8ab90b3` |
| `drivers/NativeMobileDisplay` | [UniXNU/NativeMobileDisplay](https://github.com/UniXNU/NativeMobileDisplay) | `eb9c949164826c2e52f2771eac19e4b8bd8a3dae` |
| `drivers/NativeRamBlock` | [UniXNU/NativeRamBlock](https://github.com/UniXNU/NativeRamBlock) | `7194961a668e59018323b626be4fd7ce0a91e78f` |
| `drivers/NativeUfs` | [UniXNU/NativeUfs](https://github.com/UniXNU/NativeUfs) | `af785c1777161a62931425c2faaa536ce0bebd5b` |
| `drivers/NativeVirtioBlock` | [UniXNU/NativeVirtioBlock](https://github.com/UniXNU/NativeVirtioBlock) | `98909eb9198a4360d7a43be831887cb8b74d814d` |
| `kits/NativePrelink` | [UniXNU/NativePrelink](https://github.com/UniXNU/NativePrelink) | `2b6ffb50117eee820b98aca9bd51b5c7dfb2970e` |
| `kits/VerifiedRoot` | [UniXNU/VerifiedRoot](https://github.com/UniXNU/VerifiedRoot) | `2854c61f6c4dfda870ac3d93e1f03d9181edb041` |

`BootProgress`、`ShengPlatformPower` 和 `Stage7Probe` 来自设备仓库 `69952a9`。本仓库不包含 Apple IPSW、kernelcache、设备 DTB、厂商固件、密钥、磁盘镜像或构建产物。
