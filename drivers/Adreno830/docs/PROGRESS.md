# Adreno830 进度（自主推进的断点文件）

目标：一加 13T（SM8750 / Adreno 830）上，iPadOS 17.7.11 的完整 Metal GPU 驱动，开箱即用：系统界面由 GPU 合成，画面经 GPU 交接上屏，动态调频，流畅。

依据：`docs/ARCHITECTURE.md`、`docs/IMPLEMENTATION_PLAN.md`（审稿修订后为准）。硬件事实以本机实测为最高等级。

## 规则

- **2026-10-30 18:08 用户授权**：用户出去吃饭，“现在这手机是你的，你自己测试吧，自动化吧”：等全部工作流跑完再合入，期间可自行上机自动测试（只用临时 `fastboot boot` + 定时复位回安卓，不刷写分区，不往手机存储写数据；有风险的新功能单独测、必须带定时复位）。

- **2026-10-31 17:50 用户授权（“允许写日志”）**：XNU 可以写 `unixnu` 分区末尾 32 MiB 日志窗口及其下方 256 MiB 导出区（`sheng-log=1`）。这是 10-30 18:08“不往手机存储写数据”的唯一例外，别的写入一律未授权。核对过：窗口 16769024..16777215 和导出区 16703488..16769023（分区相对 4 KiB 块）写入前全零；再往下的 1 GiB 预留区有非零数据，不得触碰。
- **2026-10-31 07:30 用户要求（去睡觉前）**：“把目前缺失的功能全部盘点，一直修到完美，完美了以后再打包；全部修完美了再测试，要不然不测试”——范围：震动、声音、壁纸、小组件、系统功能、相机、光线传感器等各类传感器、电池、信号、WiFi 等。今晚只写源码、跑主机测试、编内核、打包，**不上机**；早上用户醒来后统一测试。新硬件路径一律默认关、分阶段、带定时复位，早上按清单逐级打开。
- 只写源码，不再给作者的闭源二进制打补丁。
- 每个模块必须带主机测试，编进内核并通过编译后才算完成。
- 需要真机的验证攒成清单，等用户回来一次性测；用户不在时不重启手机。
- **2026-10-30 10:45 起撤回自动测试授权**：用户对反复重启、进不去系统非常不满。之后每次重启手机（含 `adb reboot bootloader`、`fastboot boot`）都要先问用户、得到同意再做，并且要先给用户一个稳定可用的镜像。（原记录：2026-10-29 23:30 用户明确允许自动测试：GPU 镜像出来后，可以自动走“adb 重启进 fastboot → `fastboot boot` 测试镜像 → 等回安卓 → adb 读 pstore 日志并解码”，不用用户拍照。每轮开始前在聊天里说一声；只做临时启动，绝不刷写分区。
- 不提交、不推送任何仓库，除非用户要求。
- 构建环境：`UniXNU/out-gpu`（GPU 线，设备目录 `UniXNU-Pagani_SM8750-gpu`，本地分支 `a830-driver`），`UniXNU/out`（日常线，不碰）。
  - 编译前：`export PATH="$PATH:/opt/homebrew/opt/llvm/bin" CCC_OVERRIDE_OPTIONS='# +-Wno-unnecessary-virtual-specifier'`。只压 XNU 头文件那一条告警，其余 `-Werror` 照常生效（审查 BUILD-4；2026-10-29 23:20 整核已按此编过）。
  - 重跑 `./unixnu defconfig` 会丢掉 `.config` 末尾的 `CONFIG_XNU_OWNER_DATA_VOLUME_UUID="1726EAC0-2168-4118-AFD6-19E12FE306E7"`，要手工补回。
  - `UniXNU/unixnu.local.toml` 的 `kernel` 指向作者 build-63 内核：打镜像必须显式传 `--kernel`。镜像输出放 `out-gpu`，不要写进 `out`。

- 2026-10-31 21:30 补充：`unixnu` 所在 LUN 0 的 `bDataReliability` 是 0，同一 LUN 上有 `userdata`。首次开 `sheng-log=1` 前要把这个残余风险告诉用户、由用户决定；首测后用 `build-userspace/a830-inputs/ufslog/baseline-20261031.txt` 核对窗口外没有变化。

## 2026-11-01：阶段 1.5 第二次首测、打字根因与第十六阶段集成

- **硬件观察（一加 13T，iPadOS 17.7.11，内核 `75e75d4e`，`boot-A830-阶段15-首测.img`，`工具/gpu-test-loop/runs/20261101-110201-stage15-smoke-1`）**：走满 420 s 定时复位，没有 panic。UFS 156 轮 0 写错误，窗口外基线六项不变。`gpu aperture GiB, table pages 10 2011`（16 GiB、8209 页），`gpu va past 4 GiB` 的 `iova 0x100000000`，无 `gpu fault`。`SystemOS root refs` 的 use 8、7，关机修复生效。用户在 240.8 s 真实触摸中止了脚本（`abort kb wake: real touch`）。探针第一次上机，M 359 秒、Q 218 次。亮度探针 P0–P2 跑完，P3 因 AHB 全关跳过。
- **打字根因**（工作流 `wf_5379276e-1b2`，`build-userspace/a830-inputs/round-1101/keyboard.md`、`keyboard-verify.md`，读码加运行时样本，修复未上机）：无网时 `-[AFDictationConnection forcedOfflineDictationIsAvailableForLanguage:synchronous:]` 读 `com.apple.assistant.support` 的 `Offline Dictation Status`，键不存在就每次重发 `_availabilityChanged`，观察者再调回 `dictationIsFunctional`，一个通知产生 M 个新通知。备忘录 3137→6281→12571 ms 比值 2.00，Spotlight 比值 2.98。缺 `AppleKeyboards`、`DictationLanguagesEnabled` 是放大器。Spotlight 两次被 jetsam 以 HIGHWATER 杀掉。
- **Jitter 判读**（`round-1101/jitter.md`）：调度和 IPI 不是原因，启动风暴和键盘循环之外 76.3 万次调度无一超过 50 ms，`idleq` 3011 是点采样假象。66% 的 jit 是桌面时钟 10 Hz 动画。真卡顿在本次开机第一次做某事：渲染线程 commit 里编译管线（214.4 s 卡 1 s）、新表面 `dc cvac` 逐页缺页（239 s 两次约 185 ms）。
- **内存与存储**（`round-1101/memory-storage.md`、`memory-storage-verify.md`）：物理 16 GiB（2+6+8），0x8c0000000 起连续 13 GiB，之前文档的 12 GiB 是上游另一台手机的数字。压缩器确实没开（ADT 缺 `kern.vm_compressor`，内核回落模式 8）。当前内核一步都不能超过 6 GiB：引导页表池 16 页已用满。
- **亮度**：`DISP_CC_MDSS_AHB_CLK`（0xaf080b0）读 0x88000000，64 次采样全关，这与 DSI 读全 0 强相关，A1 探针验证因果。
- **第十六阶段集成**（工作流 `wf_5a00a935-e3d` 八路实现、两路审查）：内核 `90be38ff`（128651264 字节，日志 `kernel-build-a830-integrate11.log`，0 条警告），新补丁 `phone-bootstrap-tables.patch`、`phone-compressor-thrashing.patch`、`phone-vm-page-limits.patch`；探针 ABI 2（`fg` 目标、`M mem`、`pw`、内核线程带）；WiFi 协议栈 54 个对象进内核（默认关），校准后主机重启、RDDM 读取器、`pagani-wifi-log`；`PaganiBt` 新驱动（默认关）；`pagani-battery` 按位（USB 探针、PMIC 记录）；`pagani-panel` bit 9（A1）；`sheng-vsync=<微秒>`；命令行上限 1020。种子 `s7-final` `162432f9`（打字修复加 `METAL2VULKAN_SURFACE_CLEAN=seed`），`s7b-final` `7f93054e`（F4 禁用 33 项），信任包 `4c91c038`，旧种子在 `final/pre-f3-20261101/`。
- **审查**：两路共 20 条发现，其中 3 条 storage 级（USB 探针、面板保持字、蓝牙首读没有持写者）已修，保持字改成按位。回归 `build-userspace/a830-inputs/round-1101/regress.sh` 24 项全过（Adreno830 45 通过 0 失败 1 跳过）。
- **失误**：分析途中 13T 被换下，USB 上换成 `OPD2513`（`SERIAL_OTHER_DEVICE`），两路代理在它上面跑过只读命令（`getprop`、`uptime`、`ls`、`/proc/meminfo`、`wm size`、一条 `su` 下的 `sha256sum`），结果已弃用。之后上机一律带 `--serial SERIAL_13T`。
- **第二批离线推进**（用户要求全部修好再测、先别动手机；工作流 `wf_67f5133b-985` 五路实现一路审查，`wf_11922c97-143` 声音跑批）：内核 `7052a3b6`（128741376 字节，`kernel-build-a830-integrate12.log`，0 条警告），种子 `s7-final` `8d816ac8`、`s7b-final` `6d097311`、`s7bt-final` `76546804`、`s7cam-final` `7a8ff028`，信任包 `c2cb2865`。
  - **声音和震动根因（读码，真机日志现象吻合，修复未上机）**：`pagani-haptics-ios=2` 起 VirtualAudio 延迟初始化一直是死的。路由库在 `DeviceSupportsHaptics` 时查马达设置表，J120 没有条目，`sub_2AC4DC` 抛 `Invalid Product Type`，`ExceptionBarrier` 吞掉，初始化状态停在 1。音量条能显示是因为 `vaemCurrentRouteHasVolumeControl` 读失败也算有控件。修复：`pagani-audio-srzl` 按位，bit 1 由 HAL 插件在初始化前写马达设置，bit 2 是 SPMI 丢包重发；完整包用 `=7`。插件 CDHash `7f5ab15f…`。新增 `sound-verdict.py`。
  - **亮度**：`pagani-panel` bit 8 走正式路径（谨慎档 321、滑块 4417、加熄屏 4353、不发布 4433），默认路径 6144 组差分不变。路线 B 只写方案。
  - **蓝牙**：阶段 4（`BtBridge.c`、`PaganiBtHost.cpp`、替身 `pagani-bt=40`、两条 text-fixups），`bluetoothd` 卡住时撤服务再杀进程。配对密钥在系统钥匙串，活过重启要持久 Data 加稳定钥匙包。
  - **相机**：`PaganiCamera`（`pagani-cam=1..4`）和测试图提供者。发现厂商类在这台手机上是 nil，系统手电筒按钮原本也走不通，种子链 s5 改用相机插件带上修正。级 3、4 投的是 TURBO，不是 NOM。级 4 之前要确认 `ldoi1` 的上级电源是否 S7I。
  - **WiFi**：设置页四个缺口补齐（直接切换网络、断开事件、密码错的错误码、`kCurrentNetwork` 回退），高风险段持写者 bit 3。
  - **保持字**：bit 0 面板、1 USB、2 蓝牙、3 WiFi、4 相机、5 扬声器、6 ADSP，一律 fetch_or、fetch_and。
  - **失误**：第二次种子链和内核并行，`edit-phone-seed-launchd.py` 读内核大小时文件不存在，s7 段失败，单独重跑后通过；`rounds` 段的 `FIX=` 破坏了 `harvest-a830.py` 的解析，已缩进修复。
  - **回归**：`round-1101/regress.sh` 34 项全过。全开时命令行 1058 到 1105 字节，超 1020，要分轮开或删调试项。
- **没验证**：以上修复和新路径一项都没上真机。镜像和判据见顶层 `测试清单.md`。

## 2026-11-01：渲染线程上的首次卡顿（未上机）

- jitter F3：内核的 `mapLocked` 本来就在 BoMap 时按物理页 clean+invalidate 整个导入，插件 commit 再用 EL0 `dc cvac` 走一遍只是重复，还在新表面上逐页缺页。加了种子规则（`SurfaceMaintenance.h`，默认关，`METAL2VULKAN_SURFACE_CLEAN=seed`）和加速器属性 `NativeAdrenoImportClean=1`（ARCHITECTURE 4.14.1）。主机端到端（host Turnip + 内核栈的主机副本 + 真 IOSurface，`AdrenoMetal bridge/surface_seed_host.mm`）：2048 页未碰过的表面，规则关 2050 次缺页，规则开 0 次。
- `gpu sec` 行加 ` pl`、` vk`、` cln`（ARCHITECTURE 8.8），隧道方法 0x7101，最宽 315 字符。
- 待真机：第一轮看 K 行 `S<MiB>/<次>`：`S` 的字节数应接近原来的 clean 字节数；若几乎为 0，说明 CA 的表面 seed 不是 0，规则够不着，要回头看。

## 2026-10-31 深夜：GPU 内核侧（未上机）

依据 `工具/gpu-test-loop/runs/20261031-223924-stage1-full-1`。全部只过了主机测试，没有上机验证过。

- **默认光圈改 16 GiB**（`Adreno830KnobDefaults` 的 `va=16`，不占命令行）：32 个窗口、页表多 24 MiB。唯一一次上机记录（`20261031-000053-gpu-va16-smoke-1`）日志 0 行，所以算没验证过。回退：`pagani-gpu-va=4`（16 字节，当前命令行放不下，要替换一个别的参数），或把默认改回 4 重编。UBS 不足或 32 MiB 分不到时驱动自己退回 4 GiB。
- **首测判据**（全都要看 UFS 日志）：①`g3... note gpu aperture GiB, table pages 10 2011`，不是 `4 805`；②`gpu meter` 的 `pf 0/0/0/0`、`ovf 0`，G 行 `f0 h0`；③出现 `gpu va past 4 GiB: <进程> slot <n> iova ... end ...` 这一行（看的是第一次映射，不是槽 6 打开：槽 5 跨 4 GiB，那个客户端用过约 64 MiB 地址就过线，这轮 48 s 起已有小窗客户端；要 `pagani-diag=1`），之后那个客户端、桌面、小组件画面都正常：GPU 地址超过 4 GiB 在真机上从没跑过，这是最要紧的一条，判读时错误早于这一行就和 4 GiB 无关；④`nos` 不再增长，或 `gpu noslot` 行里的小窗数确实到 0；⑤`vmr` 不比 12 个窗口时明显变多，`cs` 第三个数不飙升；⑥`fw`、`ctx` 不比这轮差（客户端变多，切换变多）。不算通过：拿到设备的 AegirPoster 仍崩（反驳代理已证明它有连接时也崩，这是另一个问题）。
- **DCVS 策略**：目标 busy 0.70→0.75，降档余量 0.8→0.85（第一版 0.85/0.9 被审查改掉，理由和数字见 ARCHITECTURE 4.16），触摸提升 607→525 MHz，734 MHz 以上要连续 12 个饱和窗口，提升期间策略档跟随需求，迟到帧提升不再连爬，734 MHz 以上降档不留余量（否则工作量 470–520 被突发推上 832 MHz 后回不来）。`pagani-gpu-dcvs-max` 默认未改（734 以上由持续规则放行，重场景还能用到 900 MHz），要硬上限时仍用它。预期和风险见 ARCHITECTURE 4.16。判据：G 行 RUN 的 832/900 MHz 采样比例从约 19% 降到接近 0，温度到 95 °C 的时间晚于 47 s，`fps≥60` 的秒帧率和 p99（这轮中位 32 ms）不变差；**`fps≥60` 的秒里 ≥115 fps 的占比不低于这轮的 18.6%（760–1178 s，42/226）**：平均帧率看不出单帧余量，这条才看得出，模型里迟到帧比旧配置多（1.85%→4.49%，主要来自持续规则），用户最在意的又是顺滑，这条掉了就先放宽持续规则。不算收益：DDR，主机票常驻第 11 档，GPU 档位降下来实际 DDR 不会降（ARCHITECTURE 4.16）。命令行放不下 A/B 参数，这一轮只有这一组默认值。
- **每秒计量行 `gpu sec`、拒绝行 `gpu noslot`、`gpu va past 4 GiB`**：ARCHITECTURE 8.8、6.2。三行都要 `pagani-diag=1`、`pagani-gpu-status` 2 或 3、日志在跑；回路镜像没有 `pagani-diag`，不写。

## 已完成

| 项 | 状态 | 证据 |
| --- | --- | --- |
| 本机从源码编译内核 | 完成 | `out/.../kernel/kernel.macho`、`out-gpu/.../kernel/kernel.macho` |
| 显示通路提速 F1（写合并、整行拷贝、映射缓存、诊断按需） | 已出镜像，待真机 | `UniXNU-Pagani-Drivers` 分支 `fast-display`；`测试镜像/boot-F1-显示提速.img` |
| F1 审查修复 | 源码完成，未出镜像 | 映射缓存改为 `NativeLegacySourceCache.h`：16 帧未用即释放、熄屏全部释放；拷贝与黑屏填充跳过 GPU 状态保留带（设计 8.5，M0 遗留项）；头文件注释改正；README 记录 `pagani-fb-wc`/`pagani-fb-scan` 与回退范围。主机测试 `Test/request-state.c`（C 与 C++）、`Test/source-cache.cpp` 通过；内核编译配方 `-Wall -Wextra -Werror` 通过 |
| `platform/sm8750/Sm8750GpuPower` | 完成，未接线 | 审查 P-1..P-6 已修；新增 `Sm8750GpuPowerOff`（含 RCG 停靠）。`tests/test_sm8750_power.c` 带依赖关系的模型 485 项检查通过；`tests/mutate_sm8750_power.py` 26 个变异全部杀死 |
| M0 构建集成 | 完成 | GPU 线内核链接了 Adreno830（`_adreno830_start`、`_pagani_gpu_start`），固件内嵌已核对；镜像 `out-gpu/a830-m0-1-nogpu.img`、`out-gpu/a830-m0-2-stop0.img`（已被下面的集成镜像取代） |
| M1 后端 RPMh、SCM、SocInfo | 完成，已进内核 | `platform/sm8750/Sm8750{Rpmh,Scm,SocInfo}.{h,c}` 与主机测试；已加入 `build-adreno830.py` 的 `PLATFORM` 清单 |
| M1–M13 全部模块的集成（2026-10-29 23:26） | 源码、主机测试、内核、镜像完成，待真机 | 13 个 Gen8 源文件与 7 个平台源文件全部进内核归档（51 个对象，27 个必需符号、19 个弱默认）；主机测试 47 通过、0 失败、0 已知失败、0 跳过（含内核对象检查）；GPU 线内核 sha256 `8e81176cce4a9dec1150b4ed5ca659a083b0b7c639473b7b682c984574ae29f2`；8 张镜像在 `测试镜像/boot-A830-*.img`（见下面的上机清单） |
| 架构与实现计划 | 完成（23 条审稿意见已并入：ARCHITECTURE 第 10 节、PLAN 第 7 节） | `docs/ARCHITECTURE.md`、`docs/IMPLEMENTATION_PLAN.md` |
| 上游内核小优化与计量（2026-10-31，第 1 阶段） | 源码、主机测试、内核对象完成，**没有上机，收益未测** | `pagani-gpu-kopt` 六位默认全开（架构 3.6、5.4、8.8）：池清零出 Memory 锁、连续内存限 64–128 KiB 并按时间退避 5 s、别名只查一次、缓存维护每 64 页放开抢占、调用计量、VM 时钟。`gpu meter` 行新增 `ca/lk/im/vt/zf/cs` 段。不动 Metal、不动 `NativeAdrenoABI.h`。报告：`build-userspace/a830-inputs/upstream/stage1/kernel-min-port.md` |

## 完美化目标清单（2026-10-30 21:30 用户要求“全部修到完美，先修简单的”）

按难易排序，每项修完要真机验证并在这里打勾：

| # | 目标 | 现状（21:30 灵动岛完整包实测） | 难度 |
| --- | --- | --- | --- |
| 1 | 音量键弹出音量条、音量可调 | 不可用（静音音频设备是否加载要看日志） | 易 |
| 2 | 预装应用（日历、邮件、天气…29 个） | 一个都没有（迁移补丁是否生效要看日志） | 易–中 |
| 3 | 键盘能正常输入（所有地方） | 键盘宽度已对上，但**任何地方都点不动、不能输入**（用户 21:31 更正：之前说的“能输密码”是锁屏数字键盘，不是普通键盘）；iPad 身份下键盘完全正常 | 中 |
| 4 | 设置页不慢、点选项不卡 | 仍很慢 | 中 |
| 5 | 不再在某些界面卡死 | 仍会卡死 | 中 |
| 6 | 控制中心样式正常 | 样式“抽风” | 中 |
| 7 | 系统默认壁纸、锁屏壁纸完美 | 自定义壁纸可用，默认壁纸不完美 | 中 |
| 8 | 电量显示准确、充电状态 | 显示不准 | 中–难（真实电量靠 ADSP 电池管理或直读电量计） |
| 9 | 手电筒 | 不支持 | 中–难（闪光灯 LED 在 PMIC 上，要 SPMI 写；iOS 侧走相机栈） |
| 10 | Plus 键改为 iPhone 15 Pro 的操作按钮 | 现为静音开关，可用 | 中 |
| 11 | 声音（扬声器、麦克风） | 无 | 难（ADSP + 音频图 + TFA9873 功放） |
| 12 | CPU 调度认得 6 个性能核 + 2 个超大核 | 8 核对称调度 | 难 |
| 13 | GPU 驱动完美（无掉帧、更多 GPU 客户端、LLCC 等） | 大部分 120 帧，偶有掉帧 | 持续 |
| 14 | 电源休眠（熄屏省电、深度空闲、挂起） | 代码已合入、默认关 | 中–难 |
| 15 | 128–256 GB 持久存储 | 设计中 | 难 |
| 16 | 相机 | 无 | 很难（CAMSS/ISP + iOS 相机栈） |
| 17 | 小组件正常显示 | 黑/空（宿主应用未安装、Core Image 无 Metal 设备会崩） | 中（随 #2 与 Metal 客户端） |
| 18 | 所有 App 都能用 Core Image/Metal（不再崩） | 未列入客户端的 App 用 Core Image 会退回 OpenGL ES 崩溃（信息 App 实测 `CI::GLContext::init` 空指针） | 中–难（更多 GPU 槽位或每进程页表） |
| 19 | USB（iOS 下连电脑、配件） | iOS 下 USB 不工作 | 难（USB 控制器驱动 + iOS USB 栈） |
| 20 | 位置服务等 iPhone 专属守护进程不崩 | routined 因缺 iPhone 专属类 abort | 易（禁用或补资源） |

## 里程碑

| 里程碑 | 状态 | 备注 |
| --- | --- | --- |
“代码完成”指：设计要求的代码已写完、主机测试通过、已编进 GPU 线内核并打进镜像；全部阶段都还没有真机结果。

| 里程碑 | 状态 | 备注 |
| --- | --- | --- |
| M0 首次编译与空跑 | 完成 | 退役文件已删（`NativeAdrenoGpu/Service/GmuService/SmmuService/CpInit/Irq/GmuPlatform/GmuConfig`、`Info.plist`）；弱默认全部由所属源文件自己定义，构建脚本不再生成；SoC dtsi 已补 iommus、SMMU 中断、qfprom、LLCC；README/AGENTS 已写；GPU 线已用本地 `NativeMobileDisplay` |
| M1 板级输入、上电、安全 CX、SMMU 只读预检 | 代码完成，待真机 | S0–S2 接线完成；`停在S3` 镜像一并验证 S1/S2 |
| M2 GPU SMMU 接管 | 代码完成，待真机 | `A830Smmu`（Gunyah 仿真模型 15 个用例） |
| M3 GMU 冷启动、HFI、GX 上电 | 代码完成，待真机 | `A830GmuFirmware`、`A830Gmu`、`A830Hfi`、`A830Votes`、`Sm8750AossQmp` |
| M4 命令处理器 | 代码完成，待真机 | `A830Cp`、生成的 `A830HwInit.h`（KGSL 变体，`--check` 通过） |
| M5 ZAP 与退出安全模式 | 代码完成，待真机 | `A830Zap`（本机 `gen80000_zap.mbn` 解析通过） |
| M6 中断、栅栏、计时器、一致性 | 代码完成，待真机 | `A830Irq`、`A830Fence`、`A830SelfTest`、GMU AO 计时器 |
| M7 Metal 服务发布 + 用户态构建 | 代码完成，待真机 | 发布外壳、用户客户端、提交路径；用户态产物与两份 Data 种子已在本机编出 |
| M8 GPU 合成桌面 | 镜像已出，待真机 | `boot-A830-M8-GPU桌面.img`（`--publish` 种子） |
| M9 GPU 扫描输出交接 D1 | 代码完成，待真机 | `sheng-fb-gpu=1` 已在板级默认参数里；`NativeAdrenoHandoffLast=0x7005` |
| M10 提交路径与内存性能 | 代码完成，待真机 | 16 个作业槽、200 ms 重试、Ring/Memory 两把锁、最小恢复 |
| M11 DCVS、带宽、LLCC | 代码完成，待真机 | `A830Dcvs`、`Sm8750Tsens`、`Sm8750Llcc`（`pagani-gpu-llcc` 默认 0）；显示的 `pagani_display_*` 钩子已接到 DCVS |
| M12 120 Hz 与三缓冲 | 代码完成，待真机 | `pagani-vsync` 默认 120；`pagani-te`、`pagani-fb-flip`、`pagani-dpu-probe` 默认 0 |
| M13 挂死恢复、ACD、slumber、IFPC | 代码完成，待真机 | 默认 P0；集成时新增 `pagani-gpu-slumber=0/1/2`、`pagani-gpu-ifpc=0/1`、`pagani-gpu-ahbto=0/1`（默认都是 0）供单变量镜像使用 |
| 按 annibale 已证做法对齐（主控补充二） | 代码完成，待真机 | `pagani-gpu-profile`（默认 1 = annibale，0 = 设计）：SMMU、GMU/HFI、CP、编排器、DCVS、电源状态，加上收尾接线（CP 健康钩子、失败与判挂时排空 GMU 迟到消息、S5 诊断字段、映射后按 ASID 失效）；全部差异见 ARCHITECTURE 第 11 节第 14 条 |
| annibale 性能开关（2026-10-30） | 代码完成，待真机 | 依据实测 `工具/gpu-test-loop/runs/20261030-113746-m8-1`（`GPU RUN 342M b99`、`DCVS 160-342`、`IRQ 0 wait 8314 to 4562 late 1792`：首轮表封顶 342 MHz 且 GPU 满载，栅栏靠 16 ms 保险时钟轮询）。`pagani-gpu-maxlevel=n`（0–16，默认 0）：annibale 配置也把设计配置的整张 DT 档位表交给 GMU（`A830VotesFullTable`，首票仍 160 MHz `{1, 1, 2}`，DDR 行仍 x = 0），DCVS 上限改为 GMU 档 n（超出表取最高档；ACD 未开最高 734 MHz；显式 `pagani-gpu-dcvs-max` 仍可压低；70/60 °C 温控保留）。`pagani-gpu-irq=1`（默认 0）：S8 挂 332，只放开环栅栏中断（`A830IrqConfig::own_mask`），挂接被拒或自检里中断没到就本次开机退回轮询（`A830Irq::fallBack`），S8 不因此失败；此模式下清不掉的栅栏位也计入风暴保护。RUN 行末尾显示 `m<n>`、`i1`（`i0` = 已给但退回轮询）。主机测试 48 通过、0 失败（新增用例在 test_start/test_status/test_a830votes/test_a830dcvs/test_a830irq 各自的 PASS 行）；内核对象零告警 |

## 上机记录

- 2026-10-29 23:20 用户同意后：`adb reboot bootloader` → `fastboot boot 测试镜像/boot-F1-显示提速.img`（sha256 前缀 `700c832a4c8103cc`），发送与启动均 OKAY。**用户反馈：明显更流畅**（2026-10-29 23:25）。结论：F1 的写合并扫描输出、整行拷贝、映射缓存在真机上有效；本机 clang 21 编译的内核能正常开机。之后所有 GPU 线镜像都带上 F1 及审查修复（缓存回收、熄屏释放、保留带）。

- 2026-10-29 23:23 用户强制重启回安卓后，只读读取 pstore，验证免拍照回路的前提：
  - `console-ramoops-0` 保留了 XNU 之前那次安卓会话的关机日志，说明 XNU 不碰 `0x880000000` 区域，内存内容扛过了“重启进 fastboot → XNU → 强制重启”。
  - 约 10% 字节有 1→0 位翻转，日志要写多份、按位 OR 合并、逐行 CRC 校验。
  - 证据和编码要求见草稿目录 `fastloop/LEAD-NOTE.md`。

- 2026-10-29 23:40 集成完成（内核 `8e81176c…`，8 张镜像，主机测试 47 项全过）。
  - 第一轮 GPU 上机：`fastboot boot 测试镜像/boot-A830-停在S3.img`（设计配置，`pagani-gpu-status=2 pagani-gpu-stop=3`），发送与启动 OKAY。等用户报告屏幕上 GPU 状态行的内容，或崩溃画面。
  - 同时在跑：annibale 配置开关 `wn5tm11ry`（`pagani-gpu-profile`，默认 1 = annibale），免拍照回路与用户态修复 `w29gl13og`。

- 2026-10-30 00:40 annibale 配置开关完成（`wn5tm11ry`）。
  - `pagani-gpu-profile`：1 为 annibale，是默认值；0 为设计值。改动覆盖 SMMU、GMU/HFI、CP、编排器、DCVS 和电源状态。
  - 驱动文档页（中英）已写，ArmCpu 告警已修。
  - 合入后修了两处测试：`test_a830headers` 的数量断言、`test_gmuctx` 固定用设计配置；设备仓库检查的第 0 行期望加上了 ` p1`。全部主机测试与设备检查通过。
  - 收尾接线工作流 `whko20ai6`（`wf_6b9309df-b14`）在跑：CP 健康钩子、失败时排空 GMU 消息、新增 GMU 诊断字段、VM 映射后按 ASID 失效、文档。
  - 手机仍停在“停在 S3”镜像（旧的设计配置），等用户看屏反馈。

- 2026-10-30 00:45 已合入两个隔离任务：
  - **免拍照回路**：`RamoopsLog.{c,h}` 写入器写 9 份（console 区 1 份、pmsg 区 8 份），解码按位投票加 CRC 修复；`pagani-reset-after` 与 `pagani-panic-reset` 在 PSCI 支持时用 SYSTEM_RESET2 热复位。板级默认加了 `pagani-ramoops=1 pagani-panic-reset=20`。主机工具在 `工具/gpu-test-loop/`（`gpu-loop.py` 必须加 `--yes` 才会重启）。合入后主机测试全过。
  - **用户态修复**：Mesa 补丁新增 `turnip-a830-upstream`、`turnip-a830-fixes`，`turnip-darwin-kmd` 重新生成（提交前 `dsb sy`；一致映射只给 HOST_CACHED|COHERENT）；补丁身份重新登记通过。`build-userspace-a830.sh` 重编通过：信任包 `d2161ea8…`，种子 `data-a830` 为 `73d90d0e…`、`data-a830-publish` 为 `aaeb3793…`。
  - 还剩 `Adreno830-docs.diff` 等收尾接线 `whko20ai6` 结束后再合。
  - 新打镜像脚本 `工具/make-a830-images.sh`：回路镜像停在 S3/S5/S7/S9，另有完整、M7、M8、S9 设计配置对照，都带定时复位；开箱即用的 GPU 桌面镜像不带定时复位。

- 2026-10-30 01:02 收尾接线 `whko20ai6` 完成（报告：草稿目录 `profile-reports/wiring.md`）；未上机、未提交。
  - S6 给 `A830Cp` 绑定健康钩子：sGFSR 经 SMMU 窗口只读、不写回；GMU 侧故障先只读地查一次 GMU2HOST bit15 与 `CM3_FW_INIT_RESULT` 的 NMI 位，再看 `A830Gmu::fatalSeen`。
  - S6–S8 失败（流转为 FAULT、写状态块之前）和判定挂死（`A830RecoveryOps::snapshot`，CX 在电且 GMU 在运行）先调用 `A830Hfi::drainLate`；计数进状态块 `hfi_late`（原 `reserved0`），S5–S8 诊断页显示 `L<m>/<a>/<e>`。
  - annibale 的 S5 诊断页加 `TCM`（B4 回读字数）、`G11`（`gmu_general11`，原 `reserved1`）、`RSC`；AO 改取粘滞字 `sticky[3]`；576 字节布局与钉选偏移不变。之后重打的 annibale 镜像停在 S5 时，诊断页比下面清单多出这三个字段。
  - annibale 下 VM 每次映射成功后做一次 `TLBIASID` + `TLBSYNC=1`（编程器新字段 `sync_after_map`）。
  - `A830_T_HFI_INIT_ANNIBALE_US`（500 ms）放进 `A830Regs.h`，GMU 的两处统一用它。
  - 文档：ARCHITECTURE 3.6、6.5（用户 IB 前生效的线程是 BOTH）、8.2–8.4、第 11 节第 14 条（annibale 的全部差异）；README；UniXNU 文档三处正文链接到 Adreno 830 驱动页（中英）。
  - 主机测试 `47 passed, 0 failed`（内核对象 51 个）；`mutate_sm8750_power.py` 26 个变异全部杀死；整包构建 PASS（`libAdreno830.a` sha256 `e293cdbb…`）；接线改动的 32 个变异全部杀死。

- 2026-10-30 01:06 新内核 `c197b7c4…`：annibale 开关、收尾接线、免拍照回路都已编入。`工具/make-a830-images.sh` 打出 9 张镜像。
- 2026-10-30 10:12 第一轮全自动回路，`loop-s3`（annibale 配置）：
  - 回路机制跑通：SYSTEM_RESET2 热复位，190 s 回到安卓，adb 读回日志（证据 `工具/gpu-test-loop/runs/20261030-101203-s3-annibale-1`）。
  - 但 pmsg 区头的 size 字段出现 1 次 0→1、5 次 1→0 翻转，Linux 丢弃了这一区，只剩 console 区 1 份，298 条记录里 297 条未核验。
  - 能辨认出的 GPU 行类似 `GPU S3.00 SMMU …`，看不出 S3 是否成功。
- 10:20 第二轮同一镜像：420 s 内没有复位，手机不在 USB 上，判断 XNU 卡死，需要用户手动复位（电源 + 音量下）。复位回安卓后用 `read-ramoops.py --label s3-annibale-2` 读日志。
- 正在跑的工作流 `weww0nrq3`（`wf_77317428-37d`）：
  - 副本改成 0xFFFC 字节：console 区放 4 份、pmsg 区放 32 份，丢一个区也能解码；
  - Adreno830 接上 `boot_progress_trace`：`pagani-gpu-trace=1` 时，每次有风险的寄存器访问先记一行，卡死后能看到最后访问的寄存器。
- 2026-10-30 追踪接线完成（工作流 `weww0nrq3` 的 trace 任务；未上机、未提交；报告：草稿目录 `loop-robustness/trace.md`）：
  - 新开关 `pagani-gpu-trace`（默认 1；2 = 每次访问一行不压缩；0 = 关；大于 2 时 S0 停在步骤 0x49）。只有 BootProgress 的 ramoops 日志在跑时才写。
  - 范围：S0–S10 全程、重跑阶段体的电源操作、整个恢复。编排器为模块绑定的所有表（自身 `rd/wr`、GCC/GPU_CC、DRV-2、AOSS/IPCC、LLCC、TSENS、包住的 SMMU io、GMU io、`A830Irq` 窗口、SMC）都在访问之前写一行，读和 SMC 返回后再写一行值；每个阶段有 begin/end 行（带耗时，失败带 IOReturn/步骤/原因/寄存器）。
  - 行格式 `g<阶段>.<步骤> <块> w|r|W|R|c <物理地址> <值>`，轮询第 1–4 次、顺序块前 16 个字、之后 2 的幂次与每 1024 次写一行，结束时写 `x<次数>`；详见 ARCHITECTURE 8.7。
  - 主机测试新增 `test_a830trace`（两种配置 S1–S3 的完整行序列、写入前落行、`=0` 与日志关闭无行等）；全部主机测试与内核归档构建（0 告警）通过。

- 10:31 用户手动复位回安卓后，读取第二轮（卡死）的日志（证据 `runs/20261030-103108-s3-annibale-2`）：
  - pmsg 区又被 Linux 丢弃（两轮都是），只剩 console 区 1 份，190 条记录全部没过校验；
  - 能辨认的最后一次心跳在日志开始后约 30 s，之后就没有记录，推断整机在 30–40 s 之间卡死，定时复位没有触发。
  - 结论：pmsg 区不可靠，必须靠 console 区的多份副本（`weww0nrq3` 正在改为 4 份）；卡在哪一步要等寄存器追踪。

- 10:40 用户要求提速，和代码改造并行开始二分测试：
  - 用现有内核打 `loop-s1`、`loop-s2`、`loop-s3`，复位定时缩到 60 s；
  - `工具/a830-campaign.py` 后台连跑，每张两轮，遇到卡死就停（后台任务 `b6d39d32m`，日志 `工具/campaign-*.log`）；
  - 判据：能否自己复位回安卓（日志改造完成前读不清内容，这个信号仍然可靠）。

- 10:45 用户强烈不满反复重启，已停掉二分连跑（`b6d39d32m`）。
  - 停止前：`loop-s1` 第 1 轮自己复位回到安卓（S1 期间没卡死），第 2 轮正在跑，到 60 s 时会自己复位。
  - 以后重启手机前必须先征得同意。
  - 稳定可用的日常镜像仍是 `测试镜像/boot-F1-显示提速.img`（已验证，明显更流畅；不含 GPU）。

- 10:55 用户要求：要么自己反复测好、别打扰他，要么一次写好。决定：
  - 测试集中在用户不用手机的时间段，全程无人值守，不中途打扰。
  - 为此要让卡死也能自动恢复：原厂设备树里有 Gunyah 虚拟看门狗（`qcom,gh-watchdog`，挂在 gunyah-hypervisor 节点下）。照 Android 的 gh-watchdog 驱动，用 HVC 超级调用设定、喂狗；XNU 卡死后看门狗咬，整机复位回安卓，不用按键。待实现。
  - 规模如实汇报：驱动源码约 26,800 行（不含测试），测试 42 个文件约 18,400 行。

- 11:00 汇总四次回路结果：`loop-s3` 两轮一好一卡，`loop-s1` 两轮一好一卡（第二轮卡在停掉连跑之后，手机不在 USB 上）。
  - S1 只做 GPU 时钟上电（与作者能跑到 SMMU 阶段的序列相同），也会卡，所以卡死未必来自 GPU 驱动，也可能来自这批镜像里其他新代码：显示通路的 M9/M12 改动、`sheng-fb-gpu=1`、日志写入器。
  - 新增对照镜像 `loop-s0`：只校验输入，完全不碰 GPU 寄存器。
  - 下一次测试顺序：先跑 `loop-s0`，判断卡死是否与 GPU 无关；再按寄存器追踪逐段定位。
  - 前提：Gunyah 看门狗自动复位（`wbscz5x5e`）、日志改为 4 份副本、寄存器追踪（`weww0nrq3`）都完成。

- 11:05 **更正**：用户反映每次都回到了安卓，但回到安卓后常要手动插拔数据线，Mac 才能识别 USB。
  - 之前判为“卡死”的两轮（`s3` 第 2 轮、`s1` 第 2 轮），很可能其实已经复位回到安卓，只是 USB 没有重新枚举。“S1 也会卡死”的结论作废。
  - 已确认：`loop-s0` 第 1 轮正常回到安卓并读回日志（证据 `runs/20261030-105430-loop-s0-c1-1`），仍只剩 console 区 1 份。
  - 两个问题要解决：
    - 日志只剩 1 份，读不清：`weww0nrq3` 正在改成 console 区放 4 份；
    - 回到安卓后 USB 不一定被识别：可让安卓的 adb 走 Wi-Fi（`persist.adb.tcp.port`，系统设置，需用户自己开），或回路脚本超时后提示用户插拔。

- 11:00 `loop-s0` 两轮都回到安卓（第 2 轮靠用户插拔数据线后识别），不碰 GPU 时系统稳定。
  - 至今 6 轮没有一轮能确认卡死，原先的卡死判断都来自 USB 没有识别。
  - 日志仍只剩 1 份、读不清。
  - 11:01 后台再跑一轮 `loop-s3`（任务 `bj4u4wua1`），请用户在 iOS 桌面出现后拍下屏幕上的 GPU 状态行，日志改造完成之前先用照片判断 S1–S3。

- 11:04 `loop-s3`（`runs/20261030-110238-loop-s3-c1-1`）回到了安卓，但这次 console 区也被 Linux 丢弃，pstore 为空，一条记录也没有。
  - 安卓上没有 `/dev/mem`（`CONFIG_DEVMEM` 未开），无法绕开 pstore 直接读内存。
  - 结论：ramoops 通道只能做到“多数时候可读”（console 区 6 次留住 5 次，pmsg 区 0 次），必须靠 console 区的多份副本。
  - 照片仍是最快的结论来源。日后若要完全可靠，可考虑写进 UFS 上的预留区（官方 sheng 的做法），但风险更高，暂不做。

- 11:20 `weww0nrq3` 完成：
  - 日志布局 3：36 份 × 0xFFFC，console 区 4 份、pmsg 区 32 份；解码器兼容旧布局。
  - 寄存器追踪 `A830Trace.h`：`pagani-gpu-trace=1` 时，每次寄存器访问和 SMC 执行前先写一行。
  - 主机测试 48 项全过。
  - 代理的发现：坏位是固定的弱单元，不是随机掉位。pmsg 区每次都因同一个 size 值 18283476 被丢；console 区偶尔也会被丢。
  - 读原始内存不可行：安卓没有 `/dev/mem`，`/proc/kcore` 读 ioremap 区只返回零。
- 11:22 用最终代码重编内核 `c6aa532a…`，重打 `loop-full`、`loop-s3`、`loop-s0`；回路镜像复位时间改为 200 s（S 阶段），完整版 300 s。60 s 太短，iOS 还停在苹果 logo，是我设错了。
- 11:10 前一轮 `loop-full`（内核 `20eb102e`）跑完后手机没出现在 USB 上，等用户确认手机状态。

- **2026-10-30 11:31 真机首次确认 GPU 驱动完整启动成功**。证据 `工具/gpu-test-loop/runs/20261030-113128-loop-full-prev`：内核 `20eb102e`，布局 3，console 区 4 份，676 条记录中 410 条核验通过。
  - GPU 状态行依次为：`GPU S8.00 FENCE RUN 357ms p1` → `GPU S9 METAL PUB 94ms` → `GPU S10.00 DCVS RUN 494ms` → `GPU RUN 160M b0 g1/b2 s0 f0 h0 r23 p1`、`DCVS 160-342`、`MEM pool 512 used 0 imp 6 coh 1`、`IRQ 0 wait 0 to 0 late 8 wptr 0`。
  - 即 annibale 配置下 S0–S10 全部通过：上电、SCM、SMMU、GMU 冷启动与 HFI、CP、ZAP、栅栏自检、Metal 发布、DCVS。GPU 以 160 MHz 运行，故障 0、挂死 0。
  - 系统运行到 289 s，按计划复位，没有卡死。之前的“卡死”都是 USB 没识别。
  - 下一步：M8 GPU 合成桌面（`loop-m8`：`--publish` 种子 + owner 信任包）。

- **2026-10-30 11:42 M8 GPU 合成桌面真机跑通**（证据 `runs/20261030-113746-m8-1`）：
  - 进得了桌面，画面正常。两个区都保住了，36 份投票，797 条记录中 796 条核验通过。
  - 状态：`GPU RUN 342M b99 g3/b3 s3295 f0 h0 r23 p1`、`DCVS 160-342 t710/0/289`、`MEM pool 512 used 0 imp 1246 coh 1`、`IRQ 0 wait 8314 to 4562 late 1792`。
  - 用户目测 10–20 帧，偶尔 30 帧。照片 IMG_8508：App Library 的图标显示为网格占位图，待查。
  - 瓶颈：GPU 忙碌 99%，卡在首轮 342 MHz 上限；栅栏靠轮询（IRQ 0）；Turnip 用 `TU_DEBUG=sysmem` 没开 GMEM。
  - 看门狗 `GunyahWdog` 已合入（补丁 0001–0003）：调用号是手机自带驱动反汇编得到的 SMC `0x86000005..8`；主机测试通过；板级默认不开，回路镜像加 `pagani-wdog=30`。
- 11:45 性能第一轮工作流 `wqlk313sb`（`wf_3fe3db8e-f9f`）：
  - 内核新增 `pagani-gpu-maxlevel`（完整 DVFS 表、放开调频上限）和 `pagani-gpu-irq=1`（annibale 配置下接中断取帧）；
  - 用户态新出 GMEM 种子（`TU_DEBUG=nocb`）和无环境变量的种子。

- 11:50 设计配置的 GPU 桌面（`loop-m8-design`，完整频率表、中断取帧），用户反馈：
  - 能进桌面，积极进展；
  - 有时明显能到 50–60 帧，流畅的地方很流畅；
  - 偶尔有明显卡顿（jitter）。
  - 用户要求屏幕上显示帧率计数器：已开工作流 `w2ca1lkqe`（`wf_1fc78036-59c`）。显示通路统计每帧上屏时间，在状态带上每秒显示 FPS、最小/最大帧率、帧时间平均/99 分位/最大值，以及 GPU 交接帧与 CPU 拷贝帧各多少；同样的内容写进 ramoops，并记录超过 50 ms 的帧间隔，用来分析卡顿。

- 11:55 `loop-m8-design` 跑完回到安卓，但这次两个 ramoops 区都被 Linux 丢弃，没有日志（证据 `runs/20261030-114903-m8-design-1`）。
  - 至此读日志的成功率约 2/3：`loop-full` 只剩 console 区，`m8` 两区都在，`m8-design` 两区全丢。
  - 帧率计数器上屏以后，即使日志丢了，也能直接看屏幕。

- 11:57 用户态 GMEM 种子完成（报告：草稿目录 `perf1/userspace.md`）：
  - `build-userspace/tree/build/data-a830-publish-gmem.dbrdisk`（`TU_DEBUG=nocb`，sha256 `c902b142…`）、`data-a830-publish-noenv.dbrdisk`（`TU_DEBUG=` 空值，`1606c2d9…`）；信任包不变（`d2161ea8…`），三个 CDHash 与现用镜像相同，内核 trust cache 无需重建。
  - 这是 A830 第一次真正走 GMEM（之前全是 sysmem）。风险：桥补丁 0016 按 A740 写回块 16x4 假设，若 A830 更大，屏幕右/下边缘可能花屏；异常时退回原种子。
  - 重做或另做变体：`build-userspace/stage-publish-env-seeds-a830.sh`（如 `prefer-gmem:'TU_DEBUG=nocb\nTU_AUTOTUNE_ALGO=prefer_gmem'`）。
- 11:57 上机 `loop-m8-design-gmem`（内核 `c6aa532a`、设计配置、GMEM 种子、300 s 后复位）。`工具/make-a830-images.sh` 新增此项。
  - 12:01 按时复位回到安卓（这次 USB 自动识别），但两个 ramoops 区又都被 Linux 丢弃，没有日志（证据 `runs/20261030-120144-m8-design-gmem-1`）。
  - 设计配置连续两次两区全丢，annibale 配置那次两区都在。推测与温度有关：GPU 跑到 734 MHz 时 DRAM 更热，复位间隙里弱单元掉得更多，区头坏了就整区被丢。对策：读屏上的帧率行为主；日志心跳里加温度；复位前先关 GPU（`Sm8750GpuPowerOff`）、撤掉带宽票，让复位更干净。
  - 12:03 用户没看清，按用户要求同一张镜像再跑一次（任务 `bqcagpp4k`）。
  - **12:06 用户反馈 GMEM 质变**：“这下真感觉六十帧往上，丝滑流畅顺滑，还可能更高帧数”，但卡顿（jitter）明显而且严重，还会卡死。照片 IMG_8509（设置 › 通用页）：
    - `GPU RUN 160M b0 g1/b2 s1932 f0 h0 p0`：没有故障、没有挂死。
    - `DCVS 160-734 t111/1/10/10/11/21/413/123/296/0/0`：10 s 内 607 MHz 占 41%、660 MHz 占 12%、顶档 734 MHz 占 30%，GPU 经常顶满。
    - `MEM pool 512 used 0 imp 780 coh 1`；`IRQ 1942 wait 5963 to 3698 late 2 wptr 0`：中断与提交约一比一，迟到唤醒只有 2 次，等 GPU 这条路已经正常。
  - 12:09 第二次也按时回到安卓，日志两区又全丢（`runs/20261030-120956-m8-design-gmem-1`）。
  - **日志丢失的定量解释**：Linux 只认区头签名 `0x43474244`（11 个 1 位）。按实测每个 1 位约 8% 的概率在复位中掉成 0，单区保住的概率约 0.92^11 ≈ 40%，两区至少保住一个约 64%，正好是之前“约三分之二”的成功率。设计配置下 GPU 到 734 MHz，叠在 SoC 上的 DRAM 更热、保持时间更短，掉位率上升，两区几乎必丢。对策：复位前 10–20 s 把 GPU 停在最低档、撤带宽票降温；长期改成写 UFS 分区，安卓下用 adb 读。
- 卡顿与卡死的待查方向（代码排查，待帧率行数据确认）：
  - 只有一条 RB 环、没有优先级：GPU 交接上屏的拷贝排在所有已提交的渲染后面，前面压了重活就晚一帧。
  - GPU 拷贝失败时退回 CPU 拷贝（`native_gpu_swap_end` 的 `bypassGpu`），一帧 143–285 ms，这是“顿一下”的量级；帧率行的 `gpu/cpu` 计数能直接看出有没有发生。
  - 顶档 734 MHz 不够：重场景单帧超过 16 ms 就掉帧；734 以上要 ACD（`pagani-gpu-acd=1`），风险较高，排后。
  - 着色器第一次编译、IOSurface 导入（`imp` 在涨）、发热降频（第 0 行加了温度）。
  - 卡死：截图时 `f0 h0`，不像 GPU 挂死；GMEM 的桥补丁 0016 若写回块假设不对，可能越界写坏内存，导致进程崩溃或画面停住。
- 12:12 上机 `loop-m8-design-gmem-dpu`（任务 `bch0cpid1`），请用户在卡顿或卡死时拍下 GPU 行和探针行（`TE … FC …/s`）。
  - 结果（`runs/20261030-121553-m8-gmem-dpu-1`）：只保住 console 区，1 位掉位率 23.5%（平时约 8%），649 条记录 379 条核验通过。
  - 桌面约 58 s 出图，`GPU RUN 525M … s20` → `s30`（62 s）→ `s32`（121 s，状态栏时钟跳分钟），`f0 h0`、`late 0`，之后桌面空闲。
  - **日志在 150.8 s 戛然而止，没有复位原因记录，手机约 160 s 就回到安卓，远早于设定的 360 s**。150 s 正是 `pagani-dpu-probe=1` 的探针线程开始读 MDSS/CTL/SSPP/SMMU 寄存器的时刻（`DpuProbe.cpp` 里 `IOSleep(150000)`）。判断：探针读到没上时钟或受保护的寄存器，总线卡死、CPU 停在读指令上，随后被看门狗冷复位（冷复位掉位更多，与 23.5% 吻合）。
  - **结论：`pagani-dpu-probe=1` 在 13T 上会卡死系统，禁用**；M12 第 1 步改为只读少数确定上电的寄存器（INTF_1 帧计数与 TE 计数），另做。`pagani-te`、`pagani-fb-flip` 也会读写 DPU，上机前先审它们碰的寄存器。
- 12:20 帧率计数器工作流完成（`wf_1fc78036-59c`，补丁草稿目录 `fps/fps.diff`）：
  - 屏幕第 9 行（GPU 行上面，保留带内）：`FPS 58 (31-60) | ft 17.2 p99 41.5 max 88.0ms | gpu 45 cpu 13`，即最近 1 秒帧数（最近 10 秒最低–最高）、帧间隔平均/99 分位/最大、走 GPU 交接与退回 CPU 拷贝的帧数。
  - 日志：`F` 每秒一条（console 环，加 ` | jit N`），`J` 每次超过 50 ms 的帧间隔一条（events 环，`jitter 88.4ms gpu at 123456ms`，限流 4 条后每 5 s 一条）。
  - `pagani-fps=0/1/2`（关 / 屏幕加日志 / 只写日志）；有 GPU 状态行时默认 1。计数点在 `NativeLegacyDisplay.cpp` 的帧完成处（CPU 拷贝、GPU 交接完成、翻页锁存），纯软件无锁环，不碰寄存器。
  - 解码器 `工具/gpu-test-loop/ramoops_format.py` 已加 `F`、`J` 两个类型，解码测试通过。
- 12:20 上机 `loop-m8-fps`（内核 `947d3bbe`：帧率行、调频开关、1 ms 轮询时间片、GPU 温度、停档的 GPU 侧），任务 `bhcjrpoyu`。
- **交接上屏的永久卡死路径（代码排查发现，已修，待真机）**：
  - 内核 `waitSwap` 为帧建客户端映射失败时返回 `kIOReturnNoMemory`，但这一帧留在“已交出”状态，永远没人完成；用户态 `serveOne` 遇到超时以外的错误就退出服务循环（`while (client.serveOne(500))`）。结果显示队列一直等这一帧，画面冻住，整个开机都不会恢复。
  - 修复（`NativeGOPPresenter.cpp`，只改内核，不用重签用户态）：映射失败时内核把这一帧判失败（`native_legacy_gpu_complete(…, kIOReturnNoMemory)`，这一帧退回 CPU 拷贝），给客户端返回 `kIOReturnTimeout`，客户端照常继续；过期的完成上报只计入 `native_gop_gpu_failures`，返回成功，不再让客户端退出。
- 复位前停档的日志侧（`RamoopsLog.c`）：1 Hz 观察者在定时复位前 15 s 调用一次 `adreno830_dcvs_park()`（弱符号空默认）。BootProgress 的 ramoops、console、gunyah-wdog 三项主机测试通过。
- **12:23 内核 `20f98c6c`**（38 s 构建，零告警）＝ `947d3bbe` + 交接修复 + 日志侧停档。重打 `loop-m8-fps`（`a70bcfa0…`）和开箱即用的 `m8-gmem`（`899c57f3…`，设计配置 + GMEM + 状态行与帧率行，不定时复位）。
- 12:27 `loop-m8-fps`（旧内核 `947d3bbe`，没有复位前停档）按时回到安卓，两区又全丢，没有日志（`runs/20261030-122738-m8-fps-1`）。
- 12:28 用户要求“接着跑新内核”：上机 `loop-m8-fps`（内核 `20f98c6c`：交接卡死修复 + 复位前 15 s 停档 + 帧率行 + 温度），任务 `b4b3m00ty`。
- 12:30 工作流 `wqw323pvx`（`wf_5ec1bd54-841`）三个代理并行，文件互不重叠，报告写到草稿目录 `jitter/`：
  - `display-timing`：显示路径逐帧计时（合成器到达间隔、交出、客户端取走、GPU 拷完、上屏），每秒在第 8 行显示最慢一帧的分解（`SLOW 88.0ms gap … off … take … copy … fb0 to0`），J 记录带同样的分解；心跳 H 加 CPU 实测主频。
  - `gpu-queue`：每次提交从入队到完成的延迟与环深度，每秒平均/最大延迟、最大深度、超过 16 ms 的次数，显示在 GPU 第 3 行。
  - `dpu-safety`：找出 150 s 卡死是读了哪类寄存器，`pagani-dpu-probe=1` 收缩为只读 INTF_1 帧计数与 TE 计数的安全模式（完整快照改成 `=3`，注明会卡死）；审查 `pagani-te`、`pagani-fb-flip` 读写的每个寄存器。
- 12:35 新内核 `20f98c6c` 这一轮按时回到安卓，两区仍全丢（`runs/20261030-123517-m8-fps-fix-1`）：复位前 15 s 停在 160 MHz 不足以保住日志。设计配置下靠 ramoops 读数据基本行不通，以屏幕上的帧率行和用户反馈为主；要稳定的日志只能改写 UFS 分区。
- 12:36 用户要求：这一波做完后完整开源，发到 GitHub 公开仓库，中英文 README 简洁讲清每个模块，尊重原作者版权（原作者 IssacL04 同意开源并会引用我们的仓库）。
  - 已查：`gh` 登录账号 `Shad0w23333`（有 repo 权限）；`Shad0w23333/Adreno830` 未被占用。原作者的 `UniXNU-Pagani-Drivers`、`UniXNU-Pagani_SM8750`、`UniXNU_Enc3l` 都是公开仓库，许可证 BSD-2-Clause-Patent（保留版权与许可声明即可再发布修改版）。`Adreno830` 起点是 UniXNU Adreno740 `5c8c145`（MIT，“NativeAdreno contributors”）。
  - 方案：驱动本体发 `Shad0w23333/Adreno830`（MIT，保留原版权行）；对原作者显示通路与 Mesa 的改动以补丁形式放进 `patches/`，保留 BSD-2-Clause-Patent 声明；不放私有 GPU 固件、原作者加密的 A830 Metal 产物、含本机路径和个人对话的工作日志。
  - 准备工作流 `w8kdofjtj`（`wf_8ae5a5be-9f3`，只读，草稿写到草稿目录 `release/`）：来源与许可证检查、隐私与密钥扫描、中英文 README 初稿。
- **约 12:52 电脑重启**：两个工作流被打断，`/private/tmp` 被清空（两个草稿目录全部丢失，包括早先代理留下的内核编译配方 `review/perf5-refute/cc.py`、`fastloop/kcc/kcc.py` 和各份报告；源码、镜像、`工具/gpu-test-loop/runs` 都在）。
  - 被打断时：`gpu-queue` 基本做完（在做最后的内核对象检查）；`display-timing` 改了一半 BootProgress（新建 `CpuClock.h`），显示驱动两个文件还没动；`dpu-safety` 刚重写完 `NativeDpu.h`。这些半成品留在磁盘上。
  - 13:00 用同一脚本续跑（`wugj624hh`，脚本加了重启说明：先读上一次代理的记录，再从磁盘上的文件接着做完）；开源准备工作流也续跑（`wohhztlmu`）。
  - 12:59 用户说上一轮没看清，重跑新内核 `loop-m8-fps`（`20f98c6c`），任务 `b1gs7vr3e`。
- **13:07 卡死第一次拿到证据**（`runs/20261030-130700-m8-fps-fix2-1`，内核 `20f98c6c`，GMEM 自动调优种子；console 区保住，566 条中 306 条核验通过）：
  - 用户反馈：仍会突然卡死，FPS 变成 0；流畅时“非常非常流畅”。照片 IMG_8511（13:05，卡死中）：`FPS 0 (0-88) | ft - | gpu 0 cpu 0`、`GPU RUN 160M b0 g1/b2 s5896 f0 h0 p0 51C`、`DCVS 160-734 t297/…/491/0/209/0/0`、`MEM pool 512 used 0 imp 1402 coh 1`、`IRQ 5904 wait 18135 to 11648 late 6 wptr 0`；画面只剩墙纸和状态栏。GPU 空闲、无故障无挂死、51 °C，不是 GPU 挂了。
  - F/J 记录：所有帧都走 GPU 交接（`cpu 0`），CPU 拷贝退回的猜想排除；流畅时最高 88 帧（平均帧间隔 11.3 ms）；多次约 500 ms 的单帧停顿（353/504/500/498 ms）；**264–328 s 约 64 s 一帧都没有**（照片就在这段里）。
  - 卡死期间系统活着（心跳正常，`fairplayd.A2` 每 10 s 拉起退出一次；308.9 s 启动 `MTLAssetUpgraderD`），空闲页从 2.3–3.1 万掉到 1353（310 s）、1157（350 s）。**328.13 s 启动 `ReportMemoryException`，328.42 s 出现 `display client enter`（显示客户端重新连上）**，之后画面恢复（350 s 回到 47–51 帧）。
  - 判断：渲染服务器（显示客户端那个进程）内存超限被系统处理、重启，卡死随之结束。只有开 GMEM（Turnip 自动调优）时才卡死，sysmem 时只有卡顿。怀疑自动调优的批次或渲染遍历史在我们的内核接口下没有回收，内存一直涨。已查：自动调优靠 GPU 往全局缓冲写 `autotune_fence`（A7XX+ 用 `CP_EVENT_WRITE7 CACHE_FLUSH_TS`），全局缓冲在 Darwin 后端是写合并映射，按理 CPU 读得到；`TU_DEBUG=gmem` 在调用自动调优之前就返回，完全绕开它。
  - 对照：强制 GMEM 种子 `data-a830-publish-gmem-forced.dbrdisk`（`TU_DEBUG=gmem,nocb`，sha256 `ae2d2515…`，信任包不变），镜像 `loop-m8-gmemforced`（`281bfbb9…`）13:11 上机（任务 `b3j71gy3c`）；sysmem 对照镜像 `loop-m8-sysmem-fps`（`5922ff59…`）备用。
- 13:11 卡顿诊断工作流的 `gpu-queue` 完成（报告草稿目录 `jitter/gpu-queue.md`）：GPU 第 3 行改为 `IRQ … late N | q 平均/最长ms d深度 s超16ms数`（`wptr` 移出该行，仍在状态块）；每次提交在 64 格数组里打戳，中断上半部算完成延迟，无锁；主机测试 48 通过、内核对象 0 告警。
- 13:18 强制 GMEM（`loop-m8-gmemforced`）用户反馈：一样突然卡顿、突然卡死，有时最高 115 帧，延迟也大。日志两区全丢（`runs/20261030-131758-m8-gmemforced-1`）。**自动调优泄漏的猜想排除**（不走自动调优照样卡死）。
- **`MEM` 行一直看不到 GPU 内存（已修）**：`dcvsMemoryRow` 把 `used` 写死为 0，`imp` 其实是 GPU VM 的对象总数。现在：
  - `used` = BO 池（开机预留的 512 MiB 连续内存，Turnip 的缓冲优先从这里分）的真实用量（`NativeAdrenoMemory::usage`）；
  - 新增 ` vm <MiB>` = GPU VM 里所有活映射的总字节（`NativeAdrenoVm::mappedBytes()`，map/unmap 时加减；运行页 `aux[4]` 第 49–63 位，对象数收窄到 15 位），不为 0 才显示；
  - XNU 把映射进进程的 IOKit 内存（iokit_mapped）记进该进程的 phys_footprint，映射进渲染服务器的 BO 越积越多，它就会按内存上限被系统处理，这和 13:07 那次的现象一致。上机时看 `used` 和 `vm` 是否一直涨。
  - 测试：`test_status` 新增 4 个写入用例（固定行不变），`test_gmumap` 检查映射字节与对象一致、全部解除后归零，`test_start` 补 `usage` 替身。
- 13:24 工作流 `wxtwryoww`（`wf_8d95e334-0fd`）查卡死根因，报告写到草稿目录 `freeze/`：`kernel-lifecycle`（内核侧 BO、导入、映射、句柄、延迟释放会随时间积累的路径，可直接修 Adreno830 并带测试）、`userspace-memory`（Darwin 后端、转译层、插件里每帧积累的内存，只读出补丁）、`stall-500ms`（内核、显示通路、用户态里约 500 ms 的超时与睡眠，判断哪个落在出帧路径上，只读出补丁）。
- 13:34 卡顿诊断工作流完成（草稿目录 `jitter/`）：
  - `display-timing`：屏幕第 8 行 `SLOW <最慢间隔> gap … off … take … copy … done … fb<N> to<N>`（合成器到达间隔、到交出、到客户端取走、到 GPU 拷完、到上屏；fb = CPU 回退，to = 客户端空等）；**冻屏时改为 `SLOW - <卡住的阶段> <时长>`**（copy = 客户端取走后没报拷完，take = 交出没人取，off = 队首没交出，idle = 合成器没送新帧，done = 拷完没收尾）。J 记录带同样的分解，F 行在慢帧或失败时追加分解与计数，H 心跳加 `cpu <MHz> on <核>`（`CpuClock.h`，依赖加法链）。修了前一次留下的重名变量（会导致编译失败）。BootProgress 三项与显示驱动 7 项主机测试通过。
  - `dpu-safety`：`NativeDpu.h` 加访问守卫，`pagani-dpu-probe=1` 只读 INTF_1 的 3 个寄存器，`=4` 完全不碰 MDSS（只读两个电源域状态），会卡死的完整快照只留在 `=3`；翻页试验与 TE 锁不再读 apps SMMU。危险排序：没上电的 CTL 1–5 与闲置管线（设备树里没有节点引用 `MDSS_CORE_INT2` 电源域）最可疑，其次 apps SMMU 显示上下文。给 `NativeLegacyDisplay.cpp` 与 README 的补丁已打上（熄屏关自动刷新改为显式 `pagani-te-ar-off=1`）。INTF_1 在 13T 上还没读过，上机先用 `=4`。
- **13:35 内核 `def497bd`**（45 s，零告警）＝ 20f98c6c + SLOW 行与逐帧计时 + GPU 队列行 + `MEM` 行真实用量与 `vm` + CPU 主频心跳 + 安全探针。重打 `loop-m8-fps`（`fbd318c6…`）、`m8-gmem`（`77c06fda…`）。13:36 上机 `loop-m8-fps`（任务 `b4oimhqf0`），请用户卡死时隔 5–10 s 拍两张。
- 13:37 开源准备工作流完成（草稿目录 `release/`：`provenance.md`、`privacy.md`、`README.md`、`README.en.md`）：
  - 可以开源；没有 GPL/APSL 文件头、没有二进制、没有任何内容来自原作者产物（只有 `Sm8750GpuPowerOn`、`Sm8750ScmCall`、`pagani_gpu_start` 等几个短名字相同）。Linux/KGSL/annibale 参考源在 `/private/tmp` 被重启清空，只做了启发式比对。
  - 必须处理：`ARCHITECTURE.md`、`IMPLEMENTATION_PLAN.md` 引用了对原作者构建的反编译与他加密版的内部做法，不原样发布（改写一份简短的公开设计说明）；`PROGRESS.md` 不发布；`docs/inputs` 只留 `regs/`；13 个脚本与测试写死本机路径；用户态脚本默认读 `作者产物/build-63`；对原作者仓库的改动只以补丁发布（各子目录版权方不同）；AdrenoMetal 子模块回移了 HuJK 的上游提交要署名。
  - 隐私：现有提交 `1b5e192` 的作者是本机用户名加个人邮箱，本机 git 全局用户名是 “DeepSeek Harness”。发布用全新仓库，提交身份 `Shad0w23333 <62667668+Shad0w23333@users.noreply.github.com>`。
- 13:39 发布导出工作流 `wjom7nwp0`（`wf_1b14521b-e87`）：在 `release/` 下写可重复运行的导出脚本（`release/make-release.sh` 调三个分脚本），生成脱敏后的 `release/Adreno830`（驱动本体、`patches/<上游>/`、`tools/gpu-test-loop`、`tools/build-userspace`、中英文 README、NOTICE），跑禁用字符串扫描与主机测试，建本地 git 仓库并提交一次；**不加远端、不推送**。驱动以后每修好一批就重跑一次导出。
- **13:43 诊断内核 `def497bd` 这一轮**（`runs/20261030-134339-m8-diag-1`，console 区保住，577 条 320 条核验通过；用户这轮基本没操作，提交数 31 → 33）：
  - **CPU 只跑约 2 GHz**：心跳实测 0、1、5 号核都是 1986 MHz（性能核最高 3532.8 MHz，超大核 4320 MHz）。正好是性能域 0 的第 9 档 1996.8 MHz，即开机默认档，UEFI 的 `CpuPerfDxe` 升档在 XNU 下没保持。渲染服务器、Metal 转译、Turnip 编码与着色器编译都受它拖累。
  - **GPU 内存稳定**：`MEM pool 512 used 414 imp 247 coh 1 vm 486`（85 s）到 156 s 基本不变（used 415、vm 487）。空闲时 GPU 缓冲不涨。
  - 系统可用页 `avail` 从 483907（70 s）降到 339377（350 s），空闲时也在降（开机后台任务陆续启动，或别的泄漏），但总量仍有数 GB，不是全局内存耗尽；卡死更像渲染服务器超过单进程上限。下一轮要用户持续操作，看 `used`/`vm` 在活动时是否涨。
- **CPU 调频（SCMI）**：安卓下 `scaling_driver` 为 `scmi`，策略 0 = CPU 0–5（384–3532.8 MHz 16 档），策略 6 = CPU 6–7（1017.6–4320 MHz 16 档）。设备树（root 只读）：CPUCP 门铃 `qcom,cpucp-v2`（rx `0x16430000`+0x4c08，tx `0x17830000`+0x300，SPI 26）；SCMI 共享内存 `0x17b4e000`（发送）、`0x17b4e200`（接收），各 0x200；`arm,scmi` 用门铃通道 0/2，协议 0x13（PERF）、0x14、0x80；CPU 节点 `clocks = <&scmi_perf 0/1>`。
  - 13:50 工作流 `wpwl0n40f`（`wf_272b077a-f1b`）：在 `PaganiArmCpu` 里写 SCMI PERF 客户端（共享内存 + 门铃，轮询完成），`pagani-cpufreq=0/1/2`（关 / 开机定在最高档并每 5 s 复查 / 按负载调频加触摸升频），状态带第 14 行显示两簇频率与档位，带假 CPUCP 的主机测试。报告写到草稿目录 `cpufreq/`。
- **13:58 ACD 上机成功**（`loop-m8-acd`，内核 `def497bd`，`runs/20261030-140451-m8-acd-1`）：用户反馈开机小卡一会儿后平均 70–80 帧、高时 100 帧以上、最高约 115 帧，提升明显；仍有轻微卡顿和卡死，但缓解。日志：`DCVS 160-900`、`GPU RUN 900M b58 g11/b10`，重度使用的 10 s 里 86% 时间在 900 MHz，最高 65 °C、平时 54–58 °C，f0 h0；有一秒 120 帧（`FPS 0 (0-120)`）。
  - `MEM pool 512 used 452–454`：**512 MiB 池已用 89%**，`imp` 1885–2084，`vm` 761–887 MiB。
  - 心跳：6 号超大核 1680 MHz（上限 4320）、0 号核 1986 MHz。
  - **XNU 只有 4 GiB 内存**（日志头 `xnu ram=0x8c007c000+0x100000000`；`board.toml` 的 `owned_memory_size = "0x100000000"`，而 `dram_size` 是 6 GiB；Data 内存盘也占其中）。原作者在 `STAGE6.md` 897–899 记录：整机 12 GiB，`0x8c0000000–0xb00000000` 连续 9 GiB，DarwinBoot 支持 256 MiB–8 GiB 的连续窗口，申请不到就停止加载；他建议先试 5 GiB、再评估 6–8 GiB。
- **14:15 卡死根因工作流完成**（`wxtwryoww`，草稿目录 `freeze/`）：
  - 内核（已直接修进 Adreno830，48 测试通过，内核对象 0 告警；补丁 `freeze/kernel-lifecycle.patch`）：
    1. **BO 池空闲段表只有 512 项，满了就丢掉释放的空间**：池只减不增，新 BO 退到池外（每个 90–130 ms、持全局锁、占常驻内存、每页一个 GPU 段），对得上约 500 ms 停顿与空闲页下降。改为 16384 项、按地址有序并始终合并。
    2. 被拒的打开没有 `detach`，加速器子节点超过 1019 个后每次打开 GPU 都会卡 15 s。
    3. 渲染服务器死在取帧与交帧之间时，那一帧永远“已交出”（内核侧加弱钩子 `native_gop_handoff_closed(task)`，显示侧补丁已在 14:20 打上）。
    4–7. 孤儿核心回收、`releaseAll` 遇错即停、每进程打开上限按开机累计、邮箱路径不撤 CPU 视图。
    8. 未改：句柄单调递增使 Turnip 的 BO 数组只增不减（建议上机确认后把 `native_adreno_slot_handles` 默认改 1）。
    - `MEM` 行末尾新增 ` sc<MiB>`（池外自有 BO）和 ` <进程>:<自有>+<导入>`（占 GPU 内存最多的进程，`~` 表示孤儿）。
  - 纠正：13:07 那次 **backboardd 没有重启**（进程号连续；`display client enter` 那条是开机以来第 12 个 IOMobileFramebuffer 客户端）。冻结形态是提交被卡住、放行一次、又卡住。
  - 用户态（补丁在 `freeze/patches/`）：Turnip `queue_submit` 有出错分支持着 `submit_mutex` 直接返回（一次出错后该进程所有提交永久阻塞）；GMEM 可见性流只增不减、重分配失败留悬空指针；metal2vulkan 备用命令池与保留屏障命令缓冲不释放；交接线程没有自动释放池；着色器预热把“没有提交”当空闲，冻结时反而全速跑；256 MiB memo 缓存。
  - 500 ms 停顿：我们的出帧路径上没有能卡 500 ms 的定时器；最可能是合成器那段时间没交帧、冷缓存现场编译管线、单环排队、内存紧张缺页。显示驱动补丁 1a/1b/1c（`waitSwap` 超时复查、退役被拒时放闩并重踢（会永久卡死的路径）、CPU 拷贝失败后重踢）已在 14:20 打上；插件补丁减少交接线程每帧的系统调用；Turnip 补丁改善 Busy 处理（等本进程最旧的未完成栅栏，5 s 总时长代替 64 次重试）。
- 14:20 打上显示驱动补丁 `stall-500ms-pd.patch`、`pd-handoff-closed.proposed.patch`（都能干净应用）。
- 14:21 工作流 `wy2d4pdb5`（`wf_69376943-500`）：按顺序打用户态补丁（Mesa → `save-turnip-darwin-patch.py` → 插件 → 转译层 0017），跑 `build-userspace-a830.sh` 重编插件与 Turnip、重做信任包与全部种子（含 gmem、noenv、gmem-forced），报告写到 `freeze/userspace-integrate.md`。
- 14:22 上机 `loop-m8-acd-pool`（内核 `def497bd` + ACD + `pagani-gpu-pool=1024` + **XNU 内存 6 GiB**，镜像 `36f6d3b3…`，载荷里 `darwinboot-memory-size = 0x180000000` 已核对；打镜像时临时改 `board.toml`，打完已恢复 4 GiB），任务 `bxd1h7hwp`。
- **14:16 XNU 6 GiB 内存窗口上机成功**（`runs/20261030-141627-m8-acd-6g-1`）：日志头 `xnu ram=0x8c007c000+0x180000000`；`MEM pool 1024 used 415 imp 267 coh 1 vm 487`（1 GiB 池预留成功）；300 s 时 `avail 751714`（4 GiB 时同期约 40 万页，多出约 1.3–1.4 GB）；f0 h0，跑满 360 s 按时复位。用户这轮基本没操作。
  - 开机初期一条 J 记录：`jitter 101.7ms gpu … gap 110 off 96.7 take 0.1 copy 102 done 0.0`，即这一帧等上一帧拷完、上一帧的 GPU 拷贝排在开机时的渲染与着色器编译后面用了 102 ms（单环排队）。
  - 已把设备仓库 `board.toml` 的 `owned_memory_size` 正式改为 `0x180000000`（6 GiB，和 `dram_size` 一致）；开箱即用镜像 `m8-gmem` 改为默认带 `pagani-gpu-acd=1`、`pagani-gpu-pool=1024`。
- 14:15 用户态修复工作流完成（`wy2d4pdb5`，报告 `freeze/userspace-integrate.md`，改前文件备份在 `freeze/integrate/backup/`）：Mesa（`turnip-a830-memory.patch` 已进 `UniXNU/patches/mesa/`，`turnip-darwin-kmd.patch` 用 `save-turnip-darwin-patch.py` 重写）、插件（交接线程自动释放池、`MemoryReport.h`、预热在内存余量不足时暂停、memo 默认 32 MiB、减少每帧系统调用）、转译层 0017（命令缓冲池上限）全部干净应用；`build-userspace-a830.sh` 1 分 28 秒完成。
  - **新信任包 `eaf328a8…`**（路径不变，只能配新种子）；种子：`data-a830-publish.dbrdisk` `c3d12f99…`、`-gmem` `3ec6454c…`、`-noenv` `5d8bdf5d…`、`-gmem-forced` `2ba78426…`、`data-a830.dbrdisk` `80ef719b…`。CDHash：插件 `d4973600…`、Turnip `2b12ec65…`、`adrenoprobed` `e27481d2…`。14:14 之前打的镜像不含这些修复。
  - 新环境变量（默认值）：`METAL2VULKAN_MEMO_MIB=32`、`METAL2VULKAN_PRIME`（开）、`METAL2VULKAN_PRIME_HEADROOM_MIB=512`、`METAL2VULKAN_PRIME_MAX_FOOTPRINT_MIB=1024`、`TU_VSC_MAX_PRIM_KB=1024`、`TU_VSC_MAX_DRAW_KB=256`、`MESA_LOG=syslog`（不设）。backboardd、PosterBoard、WidgetRenderer 每 10 s 发一行 `MEM …`。着色器缓存键变了，首次开机从头预热。
- 14:30 CPU 调频工作流完成（`wpwl0n40f`）：`PaganiArmCpu/ScmiPerf.h`（SCMI 收发与 PERF 消息，轮询完成、30 ms 超时）、`CpuFreqGovernor.h`（两种模式与调速策略）、`ArmCpu.cpp` 新增调频线程（+261 行，原代码未改）、`Test/scmi-perf.cpp`（假 CPUCP，972 项检查，ASan/UBSan/TSan 干净）。`pagani-cpufreq=1` 两域定在最高档（3532.8/4320 MHz）、每 5 s 复查；`=2` 按负载（20 ms 采样，>60% 一步到顶，<30% 每 100 ms 降一档，`pagani-cpufreq-min` 下限，触摸保持最高档到松手后 300 ms）。状态带第 14 行 `CPU max 3532/4320MHz lv15/15 ld …% thr N err N`，被固件降档时档位后加 `!`。风险：Gunyah 若没把 CPUCP 门铃或共享内存映射给 XNU，首次访问会异常（访问前先写 `cpufreq: probing CPUCP SCMI` 日志）。
- **14:31 内核 `068932a5`**（59 s，零告警）＝ def497bd + 卡死根因的内核修复 + 显示驱动补丁（1a/1b/1c 与交接连接关闭）+ CPU 调频。镜像 `loop-m8-full`（`f32fb360…`，新 GMEM 种子 `3ec6454c` + 新信任包 `eaf328a8` + ACD + 1 GiB 池 + `pagani-cpufreq=1` + 6 GiB，复位 420 s）与开箱即用 `m8-gmem`（`59994b99…`，同配置不复位）。14:32 上机 `loop-m8-full`（任务 `bwny3bnd8`）。
- **14:40 合并版 `loop-m8-full` 上机**（`runs/20261030-144045-m8-full-1`，**691 条记录 690 条核验通过**，日志第一次几乎完整）。用户：算是很流畅，还可以更好；不稳定，基线约 80 帧，卡时 40 多帧，最高 115 帧。
  - **CPU 调频真机可用**：`cpufreq: d0 … asked lv15 3532MHz`、`d1 … asked lv15 4320MHz`；心跳实测 6 号核 3921–4066 MHz（之前 1680）。CPUCP 固件会把档位压下来：性能核在 lv9–10（2227–2400 MHz）与 lv15 之间来回，超大核多在 lv13（4089 MHz），偶尔压到 2438–2649 MHz。
  - **GPU 被温控压频**：温度 84–94 °C，`DCVS 160-607`（85 °C 封顶）、`160-389`（94 °C），389 MHz 时 `b85`；IRQ 行 `q 15/50ms d4 s55`。最后的 `160-160` 是复位前停档，不是温控。
  - 内存：`MEM pool 1024 used 831 imp 3235 coh 1 vm 930–950 backboardd:511+92`（池用到 81%，backboardd 自有 BO 511 MiB、导入 92–108 MiB）。
  - SLOW/J：大停顿都是 `gap`（合成器没交帧）：`SLOW 921ms gap 846`、`809ms gap 791`、`351ms gap 346`、`234ms gap 257`，`off`/`take` 为 0，`copy` 3–80 ms。上游原因：温控降频 + 新种子着色器缓存为空、现场编译（Data 是内存盘，每次开机缓存都清空）。
  - 结论：瓶颈从频率转为发热。下一步：CPU 改按负载调频、GPU 调频按温度平滑控制；预编着色器缓存打进种子；iPhone 模式可行性调研（社区已有 iPhone X iOS 16 的移植；可选同代内核的 iPhone XS iOS 17.7）。
- 14:50 工作流 `wwgapcy8t`（`wf_00b03822-7ce`，报告写到草稿目录 `next/`）：`thermal-perf`（GPU 闭环温控：目标温度默认 80 °C、`pagani-gpu-ttarget`，一档一档升降带迟滞，95 °C 紧急降；靠近目标时只升一档；CPU 调速器在 GPU 受热限时把两簇压到中档，`adreno830_cpu_hint` 真正实现；`pagani-cpufreq=2` 调优为推荐值）、`shader-cache`（能否在 Mac 上为当前构建预编着色器/管线缓存并打进 Data 种子，做原型 `data-a830-publish-gmem-cache.dbrdisk`，不改签名二进制）、`iphone-mode`（只读调研：换 iPhone 身份与 IPSW 的步骤、工作量、风险，以及 iPadOS 内放大界面的替代方案）。
- 14:52 开箱即用镜像 `m8-gmem` 改为 `pagani-cpufreq=2`（CPU 按负载调频），重打 `105dca7b…`（内核 `068932a5`、GMEM 新种子 `3ec6454c`、ACD、1 GiB 池、6 GiB、不定时复位）。
- **15:00 用户方向**：调度要智能且激进（需要性能时才冲最高频，但要冲得快）；温控不要那么保守，95–100 °C 或参考合理值再降；不要几百上千毫秒的停顿。**不换系统镜像**，就在现有 iPadOS 上改成 iPhone 布局（界面更大、布局合理）。
  - 原厂温控阈值（安卓 `/sys/class/thermal`，root 只读）：`gpuss-0..7` 被动降频 **105 °C**、`hot` **125 °C**；`cpu-1-*`（超大核）92/105/118/120 °C 被动、125 °C hot；`cpu-0-*` 105/118/120 °C、125 °C hot。我们原来 GPU 85 °C 降到 607、94 °C 降到 389，比原厂早约 20 °C。
  - 新温控策略（`thermal-perf` 交付后改默认值）：GPU 100 °C 以下不限频，100 °C 起逐档平滑降（赶在原厂 105 °C 前），110 °C 紧急降；CPU 温控交给 CPUCP 固件；调度激进：升频快、触摸保持最高档到松手后 1 s（同官方 sheng）、降频慢。
  - **iPhone 布局（同一个 iPadOS 根，只改 ADT）**：身份在 `UniXNU/profiles/os/ipados-17.7.11-21H461/profile.toml`（根 `model = "iPad7,1"`、`target-type = "J120"`、`compatible = "J120AP"`；产品 `artwork-device-idiom = "pad"`、`artwork-device-subtype = 2732`、`artwork-scale-factor = 2`、`product-id` J120），配置按 arch → SoC → OS → `board.toml` 合并、后者覆盖。新脚本 `工具/make-a830-layout.sh scale3|phone` 临时改 `board.toml` 打镜像、结束必恢复：
    - `loop-m8-scale3`（`538dcfc8…`）：仍是 iPad 身份，`display-scale`/`artwork-scale-factor` = 3（1216x2640 → 405x880 点，界面放大 1.5 倍）。
    - `loop-m8-phone`（`91bfa802…`）：再加根 `model = "iPhone11,6"`、`target-type = "D331p"`、`artwork-device-idiom = "phone"`、`artwork-device-subtype = 2688`、产品名 iPhone XS Max、`compatible-device-fallback = "iPhone11,2"`；为音频保留 `J120AP` 与 J120 的 `product-id`。核对 ADT：iPhone11,6、phone、scale 3、J120AP。
  - 15:05 上机 `loop-m8-phone`（任务 `bhcszujgl`，内核 `068932a5`、GMEM 新种子、ACD、1 GiB 池、`pagani-cpufreq=2`、6 GiB）。
- **15:05 iPhone 布局版上机成功**（日志两区丢失；照片 IMG_8517/8518）：设置里显示 “iPhone Storage”、产品名 iPhone XS Max，界面放大。用户：调度后大幅流畅，动画过渡比原作者版本顺滑很多；设置页卡了十几秒（无伤大雅）。
  - IMG_8518（桌面）：`FPS 99 (0-120) | ft 9.3 p99 15.4 max 20.7ms`、`SLOW 20.7ms gap 4.1 off 7.6 take 0.2 copy 20.4`；`GPU RUN 389M b98 … 94C`、`DCVS 160-389`（旧温控）；`MEM pool 1024 used 616 imp 1889 vm 695 backboardd:425+67`；`IRQ … | q 12/27ms d4 s27`；`CPU 3532/4089MHz lv15/13! ld 42/8% boost 92 thr 34 err 1`。
  - IMG_8517（设置卡住时）：`SLOW - idle 10.3s`、GPU `b0`、`CPU 1152/1689MHz lv4!/3 ld 15/5%`：合成器 10 s 没交帧、CPU/GPU 都闲，是某个系统服务超时（日志里 `fairplayd.A2` 每 10 s 拉起退出一次）。
  - 问题：图标是网格占位图（iPad 模式的资源库也一样）；iPhone 模式下桌面黑底没墙纸、只显示少数 App 且有 “Waiting...”、很多预装 App 不见（iPad 版系统里部分 App 只声明支持 iPad）；容量约 1 GB 可用 500 MB（Data 是 1 GiB 内存盘）。
- 15:03 发布导出工作流完成（`wjom7nwp0`）：`release/Adreno830` 本地仓库，提交 `65fabad`（226 个文件、4.25 MiB，作者 `Shad0w23333 <62667668+Shad0w23333@users.noreply.github.com>`，UTC 时间，无远端）；`release/make-release.sh --commit` 可从最新源码全新导出、检查、测试再提交。待用户决定：补丁里组件 revision 是 40 个 0（推送后重跑）、是否保留 `AGENTS.md`、提交信息里的 Claude 署名、补丁里过时的硬件状态（`porting.toml` 等写着未上机）、GPL 参考源逐字比对未做（参考源丢失）、metal2vulkan 补丁/`gpu_abi.py` 许可证/`hostsim-a830.diff` 是否发布。
- 15:06 工作流 `we7lx1ze4`（`wf_cd2999a8-deb`，报告草稿目录 `polish/`）：`icons`（图标占位图的根因与修复，管 AdrenoMetal 与 `add-gpu-plugin-data.py`）、`phone-mode`（iPhone 模式的墙纸、隐藏/Waiting 的 App、Data 盘扩到 3 GiB、设置 10 s 卡顿；管设备仓库 ADT/Data 配置与 `make-a830-layout.sh`）。
- 用户要求：能落地的（GPU/CPU 驱动、调度、图标与小功能修复）全部做完，下次一起打包一次上机。
- 15:16 `iphone-mode` 调研完成（草稿目录 `next/iphone-mode.md`）：现有 iPadOS 上改 ADT 身份即可用 iPhone 布局（10-29 的 I/R/M5/M6 版已用 iPhone10,3/D22/2436/@3x 进过桌面；当时的白图标是图标服务被停用造成的，与身份无关）；当前身份其实是 iPad7,1/J120AP（2017 款 A10X，arm64 无指针认证）；驱动都不看身份。**键盘必须修**：iPhone 身份下 `+[TUIKeyboardLayoutFactory layoutsFileNameForDeviceClass]` 要读 `KBLayouts_iPhone.dat`，iPad 根卷里没有；`KBLayouts_iPad.dat` 已含完整手机键位，用 UniXNU 官方文本修补机制（OS profile 已有 4 条）让它固定返回 iPad 文件名：`text-fixup0=dyld_shared_cache_arm64.05,3e964e8,ffc300d1f44f01a9fd7b02a9,60b72a9000402d91c0035fd6`（未上机）。图标服务会同时查 phone/pad 素材；只有 Photo Booth 是 iPad 专属；Dock 的“电话”被跳过。换 iPhone 系统镜像不推荐（XS/XR 用户态是 arm64e，iPhone X 要移到 Darwin 22，约 4–8 周）。
- **15:30 用户要求真实日常存储 128–256 GB**（3 GiB 内存盘远远不够）。现状：官方文档 `devices/oneplus13t.md` 写明 “Physical writes and persistent Data are unsupported”，sheng 也把“验证持久 Data 写入”列为下一步；`NativeUfs.cpp` 纯只读（写请求返回 `kIOReturnNotWritable`，没有 WRITE 命令）。系统在 64 GiB 的 `unixnu` 分区（LUN 0 末尾），根镜像约 8.74 GiB。三步方案：A 在 NativeUfs 实现写入与刷盘、硬性只允许写 `unixnu` 分区范围；B 在 `unixnu` 的 APFS 容器里加持久 Data 卷（约 55 GB，Mac 上做好镜像、fastboot 刷一次 `unixnu`，需用户同意）；C 扩到 128/256 GB 要缩小安卓 userdata，会清空安卓数据，必须用户备份并明确同意。加密密钥要能跨开机保存。工作流 `w5uiky79a`（`wf_b8892e95-f02`，报告草稿目录 `storage/`）：`ufs-write`（写入路径、地址保护、主机测试，默认关、`native-ufs-write=1`）、`data-volume`（B/C 的只读设计）。合并包先用 3 GiB 内存盘过渡。
- **15:05 用户决定：开源提交不保留 Claude 署名。** 已改：`release/make-release.sh` 的提交信息去掉署名行，检查改为“提交信息里出现 co-authored-by/claude/anthropic 就报错”；本地仓库提交改写为 `cbe50812`（单个提交，作者/提交者 `Shad0w23333 <62667668+Shad0w23333@users.noreply.github.com>`，UTC 时间，无远端）。
- ACD 测试镜像 `loop-m8-acd`（内核 `20f98c6c`，`ae73cad5…`）：设计配置 + GMEM + `pagani-gpu-acd=1`。ACD 代码已完整：S1 向 AOP 发 QMP `val: 1`，S5 的 HFI M3 发 `FEATURE_CTRL{ACD}` 与 ACD 表，两边都应答后 DCVS 上限从 734 MHz 放到表顶（通用表 900 MHz），任一环节失败就保持关闭（734 MHz）。待卡死修复确认后上机。
- 本轮代码（Adreno830，主机测试已过）：
  - 栅栏等待：332 没接、被关或退回轮询时，时间片从 16 ms 改为 1 ms（`A830_FENCE_POLL_SLICE_US`，`A830Fence::polling()`），新用例 `pollingWithoutInterrupt`。
  - GPU 第 0 行末尾显示温度 ` <t>C`（运行页 `aux[0]` 第 57–63 位，`A830StatusTemp`、`adreno830_status_temperature`，DCVS 线程每秒写入），`test_status` 加 7 个用例，固定行不变。
  - 复位前停档的 GPU 侧：`adreno830_dcvs_park()` → `A830Dcvs::park()`，下一个节拍起上限和下限都压到 1 档（160 MHz 及其总线档），压过一切提频；`test_a830dcvs` 新增 `testPark`（319 项检查）。
  - Adreno830 全部主机测试 48 通过，内核对象 51 个、27 个必需与 21 个弱定义齐全。
- 12:10 性能第一轮工作流完成（报告：草稿目录 `perf1/kernel.md`）：`pagani-gpu-maxlevel=n`、`pagani-gpu-irq=1` 两个开关（annibale 配置用），主机测试 48 通过，内核对象 0 告警。建议值 `pagani-gpu-maxlevel=9 pagani-gpu-irq=1`；ACD 不开时上限仍是 734 MHz。annibale 配置的温控规则是 70 °C 压到 160 MHz，持续满载可能触发。
- 12:06 现有内核 `c6aa532a` 已编进 M12 的 `pagani-dpu-probe`、`pagani-te`、`pagani-fb-flip`，不用等新内核。新打两张探针镜像（复位 360 s）：`loop-m8-design-gmem-dpu`（`b0455a9b…`）、`loop-m8-design-dpu`（`bd641531…`）。
  - 探针开机 150 s 后启动，屏幕第 14–18 行每秒刷新，同时写进日志（`RAMOOPS_KIND_DPU`）：`TE <Hz> AR <自动刷新> FC <每秒实际送屏帧数>/s …`、CTL/PIPE、SSPP、MDP SMMU、FLIP 行。FC 就是面板这一层的真实帧率，TE 是屏幕实测刷新率（判断 60 还是 120 Hz）。
  - 上一轮 annibale 桌面日志的逐秒数据（`runs/20261030-113746-m8-1`）：操作时每秒约 110 次提交、200 次等待，其中约 55 次是睡满 16 ms 时间片才醒的迟到唤醒，即每秒近 0.9 s 在白等。这是 annibale 配置只有 10–20 帧的直接原因。
- 代码排查卡顿与帧率上限（不需要真机）：
  - **栅栏等待按 16 ms 时间片睡**（`A830Fence::wait`，`A830_FENCE_WAIT_SLICE_US`）：没有中断叫醒时，每次没完成的等待都要睡满 16 ms 才再看。annibale 配置不接 332，所以每帧被量化到 16 ms 的倍数（`late 1792` 就是这个），这是它只有 10–20 帧的主因之一；设计配置接了中断，所以快很多。设计配置下若某些提交不触发 CACHE_FLUSH_TS 中断，也会在那一帧卡 16 ms，要看 `IRQ` 行的 `late`。待办：中断关闭或没接时改用 1 ms 左右的短时间片。
  - 防风暴保护要连续 1000 次无进展才关 332，正常不会触发，排除。
  - 设计配置的调频上限是 734 MHz（ACD 没开）；温度 85 °C 降到 607 MHz、95 °C 降到 389 MHz。16 ms 一个窗口，忙 90% 以上一次升两档；空闲 64 ms 降到 160 MHz，首个提交升到 525 MHz、触摸升到 607 MHz。
  - CPU 频率：XNU 没有 CPU 调频驱动，但 13T 的 UEFI 有 `CpuPerfDxe`（`UEFI-Pagani_SM8750/Platforms/OnePlus/oneplus13tPkg/Drivers/CpuPerfDxe`），开机时经 SCMI 把八个核设到最高档，实测提速 5% 以上才保留。没有 XNU 下的实测；下一版在日志心跳里每 10 秒测一次 CPU 主频（依赖加法链 + 架构计时器），顺便看发热降频。官方 sheng 的做法是 `sheng-cpufreq=4` 按负载调频，触摸和 GPU 提交时给 CPU 升频。

- **15:50 合并测试包上机**（用户在场、要求立即测；任务 `b3me8n38q`，临时 `fastboot boot`，420 s 后自动回安卓读日志）：
  - 内核 `2774af7c`（零警告）：温控按用户要求放宽——GPU 目标 95 °C（`pagani-gpu-ttarget` 默认 95，范围 50–100），紧急降到 389 MHz 改为 105 °C 进、100 °C 出（`Sm8750ThermalCriticalDeci = 1050`，与原厂 gpuss 被动阈值一致）；含 `thermal-perf` 的闭环温控与 CPU 联动、栅栏 1 ms 轮询、温度与 vm 显示。
  - 种子 `data-a830-phone-cache.dbrdisk`（`e4560bc0`）：GMEM（`TU_DEBUG=nocb`）+ 启用 iconservicesagent 并列为 Metal 客户端（图标占位图）+ 禁用 `com.apple.fairplayd.A2`（设置页 10 s 停顿）+ 着色器缓存包 `df3fea96`（主机校验 PASS：命中 35、26 条平板身份的驱动记录）。
  - 镜像 `测试镜像/boot-A830-回路-M8桌面-iPhone布局.img`（`357ed5b3`）：iPhone11,6 / D331p 布局 + 键盘修正 `text-fixup0`（`KBLayouts_iPad.dat`），ACD、1 GiB 池、`pagani-cpufreq=2`、6 GiB 窗口。
  - build-userspace 已应用 `polish/icons-work/patch/` 的图标补丁与 fairplayd 可选补丁；`tree/scripts/add-gpu-plugin-data.py` 已同步为 UniXNU 版（含 `--enable-jobs/--disable-jobs`）。
  - 数据盘保持 192 MiB：用户同意先不扩到 3 GiB（打包内存盘会占 6 GiB 窗口的一半）。
  - 测试跟新阈值：`test_a830dcvs` 的热模型与规则用例显式用 80 °C 目标、紧急读数抬高 10 °C；`test_sm8750_tsens`、`test_start` 改期望。Adreno830 全部主机测试 48 通过，内核对象 51 个、30 个必需与 22 个弱定义齐全。
- **15:46 合并包上机结果**（`runs/20261030-154643-phone-pack-1`，742 条记录 741 条校验通过；照片 IMG_8520）：
  - 用户：**图标修好了；非常流畅，没有遇到严重 jitter**；壁纸仍然没有；声音和按键“还有很多小问题”。
  - 照片状态行（设置页、空闲）：`GPU RUN 160M … 58C`、`DCVS 160-900 T95-`、`MEM pool 1024 used 661 imp 1541 vm 738`、`CPU 3532/3513MHz lv15/11 ld 10/8% boost 27 thr 12 err 1 hot 0`——负载 10% 时两簇停在最高档，是触摸提频（一触摸就到顶、保持 300 ms）。
  - J 记录 5 条：合成器间隙 67–251 ms（`gap` 大、`copy` 不到 4 ms），是 CPU 侧/应用侧的停顿，不是 GPU。
  - 崩溃循环：**PosterBoard 每 10 s abort 两次（status=6）**，每次启动 184 ms 内崩，十来个 *PosterExtension 随之被杀（status=f）；**SpringBoard 每 5.4 s 段错误一次（status=b）**，从 295 s 起（可能是锁屏后需要海报）。根因（`polish/phone-mode.md`）：iPad 根卷只有 `~ipad` 的壁纸清单，iPhone 身份下 WallpaperKit 找不到集合清单就抛 NSInvalidArgumentException；D331p 还让 launchd 找不到 jetsam 配置。
  - CPUCP 固件把域 1 限在 4089 MHz（`asked lv15 4320MHz`）。
- 15:50 两个工作流交付：`wwgapcy8t`（温控性能、着色器缓存、iPhone 模式调研）、`we7lx1ze4`（图标、iPhone 模式完善）。合入：
  - 设备仓库 `patches/xnu/series` 末尾加 `phone-wallpaper-collections.patch`（把种子里 `/private/var/ngpu/wallpaper-collections` 只读叠到 `/Library/Wallpaper/Collections`），重编内核。
  - 布局脚本新变体 `phone-plus`（target-type J120、2 GiB Data、壁纸覆盖层、禁用 fairplayd）与 `phone-apps`（再加 `sheng-staged-apps=1` 和迁移哨兵，开机安装 29 个系统应用）；新增 `A830_RESET_AFTER` 覆盖定时复位。
  - 着色器缓存报告：13T 上开预热有害（后台编 158 个库、往内存盘写约 720 MiB、抢 CPU），下一包用关预热的合并种子 `data-a830-phone-cache-noprime.dbrdisk`（`0c2a5d96`）。
  - 用户同意：Data 先不扩 3 GiB（易失内存盘上限 2 GiB，再大要做持久 Data）。
- **16:00 上机 `phone-apps`**（任务 `bro29dt9a`）：内核 `e939a22c`（加壁纸联合视图补丁，零警告）、种子 `工具/seeds/phone-apps.dbrdisk`（`a2970eee`，底种子关预热的合并种子）、镜像 `测试镜像/boot-A830-回路-M8桌面-iPhone布局-增强-系统应用.img`（`8c878cc5`，`sheng-staged-apps=1`、`pagani-reset-after=600`、J120 + iPhone11,6）。675 s 后按定时复位回到安卓（XNU 没有卡死），但**日志两区都被 Linux 当作头部损坏丢弃**（`found existing invalid buffer`），屏幕上的结果等用户反馈。备用镜像 `phone-plus`（`7a42a961`，不装系统应用）已打好。
  - 安卓开机日志里的音频硬件：`wcd939x_slave_dlkm`、`wcd9378_slave_dlkm`（编解码器）、`wsa884x_dlkm`（SoundWire 智能功放），`q6_pdr_dlkm`（ADSP 保护域），交给 `audio-plan`。
- **16:25 第二次 `phone-apps` 上机**（`runs/20261030-163445-phone-apps-1`，日志又丢：两次 600 s 运行 Linux 都报同一个 `found existing invalid buffer, size 18807764`，420 s 那轮完好，像是长时间运行后有东西写坏了 zone 头，待崩溃取证代理交付后查 RamoopsLog）。**用户反馈**：
  - 好：比以前流畅非常多、基本稳定 120 帧（个别地方还会卡）；图标正常；电源键锁屏/亮屏完美；容量显示 2 GB、可用 1 GB、iPhone XS Max。
  - 坏：锁屏和桌面没有壁纸，Safari 自定义背景也不显示；日历、邮件等预装应用没出现；小组件全黑；音量键能用但不弹音量条；键盘能弹出但打不了字、界面比例不对（iPad 模式下键盘完全正常）；电量和充电不工作；没有声音。
- 16:40 键盘根因与修复：iPhone 身份下 TextInputUI 要读 `KBLayouts_iPhone.dat`，iPad 根卷没有；之前的 `text-fixup0` 让它读 iPad 的布局，按键几何对不上。经用户同意从 iPhone XS Max iOS 17.7（21H16）官方固件取出 `KBLayouts_iPhone.dat`（`3801e404`，2,347,928 字节；同版 `KBLayouts_Dynamic.dat` 与 21H461 一致），放在 `build-userspace/iphone-17.7-21H16/`（苹果文件，不进开源仓库）。新 XNU 补丁 `phone-keyboard-layouts.patch`（设备仓库 series 末尾）：把种子里 `/private/var/ngpu/textinputui-layouts` 只读叠到 `TextInputUI.framework`（暂存页 0xb00、addon5），并把壁纸/键盘两层叠加的阶段与结果写一行 U 记录（`overlay wallpaper stage … keyboard stage …`）。`make-phone-seed.py --keyboard`、布局脚本自动带上；`loop-m8-phone` 去掉 `text-fixup0`。
- **16:50 上机 `phone-kb`**（任务 `b3t15ox5m`）：内核 `52ec7378`（零警告）、镜像 `bcc7c90f`（phone-apps + iPhone 键盘布局 + 壁纸覆盖层，复位 420 s 以保住日志）。
- **16:52 第二次 `phone-kb` 上机，用户反馈**：和上一版没区别——键盘仍然错位、按键没反应；预装应用仍没有装上。**键盘真正根因**（`next/iphone-mode.md` 3.2）：TextInputUI 按屏幕宽度查宽度→前缀表（320、375、414…）选键盘布局；1216 像素宽的面板在 @3x 下是 405.33 点，不在表里，布局几何和屏幕对不上——换不换 `KBLayouts_iPhone.dat` 都一样。曾提议 XS Max 1242×2688 画布居中裁切，**用户否决：要按 13T 自己的 1216×2640 原生适配**。改为让键盘适配 405.33 点：在我们自己叠加的 `KBLayouts_iPhone.dat` 的宽度→前缀表里补上 405.33 点的条目（只改数据），先逆向确认 TextInputUI 是否按实际宽度缩放布局、有没有任意宽度的动态布局路径。日志第三次丢失（同一个 `size 18807764`，与运行时长无关，从 2 GiB 种子 + J120 + 叠加补丁 + 开机装应用的变体开始）。
- **17:15 工作流 `wdi0iu5je` 完成**（五项实现 + 四项独立审查，结论都是可合入；报告 `fix2/`）：
  - `gpu-slots`：GPU 客户端槽位 4→12（渲染服务器 1.5 GiB；3 个 352 MiB 大窗口给 WidgetRenderer_Default、PosterBoard、iconservicesagent；8 个 124 MiB 小窗口给其余进程），作业槽 16→24，沙盒授权加 14 个会建 Metal 设备的海报扩展；`NativeAdrenoABI.h` 字节不变；种子 `--clients` 同步（`build-userspace-a830.sh`、`stage-publish-env-seeds-a830.sh`、`check-env-seeds-a830.py`），现成基础种子 `fix2/gpu-slots-work/seeds/data-a830-phone-cache-noprime-posters.dbrdisk`（底种子是关预热的合并种子，17 个客户端）。注意：内核侧改动在 16:38 的 `52ec7378` 里就已生效（代理直接改工作树）。
  - `keys`：Plus 键（gpio97，低有效）接成响铃/静音开关（Telephony 页 0x0B/0x2E 开关状态），按键轮询 8 ms、PON 稳定时间 16 ms；board.toml 已加 `[adt.buttons.u32] function-button_ringerab = 0`。
  - `cpu-gov`：触摸只提到响应档，按利用率（类 schedutil）快速升频，空闲更快回落，GPU 出帧时中档保底；状态行静止 `lv3/3`、触摸 `lv9/8`。同样已在 `52ec7378` 里。
  - `crash-diag`：致命信号（段错误、abort 等）时在 `postsig_locked` 取出错线程的寄存器与最多 16 层帧指针链，换算成共享缓存未滑动地址/主程序偏移，写 ramoops 新记录 `X`（`pagani-crash`，默认 1，限流 30 s 合并），新 XNU 补丁 `crash-trace.patch`（series 末尾）+ BootProgress `CrashTrace.h`；`工具/gpu-test-loop/symbolize-crashes.py` 用 `fix2/crash-trace/dsc.a2s` 符号化。推断：SpringBoard 段错误固定在 exec 后 5.38–5.40 s（像某个 5 s 超时后的错误分支），PosterBoard abort 是缺 `Collections~iphone.plist`。
  - `audio-plan`：见上。
- **17:27 上机 `phone-plus`**（任务 `bto3xde5d`；内核 `a66d5d4a` 含 crash-trace、GPU 12 槽位、新 CPU 调速、Plus 键；种子 `phone-plus.dbrdisk` 底种子 `fix2/gpu-slots-work/seeds/data-a830-phone-cache-noprime-posters.dbrdisk`；镜像 `aab7f240`，不装系统应用，复位 420 s）。**用户反馈**：
  - Plus 键映射成功，能切换静音模式（用户以后想要灵动岛机型的“操作按钮”自定义功能，排在后面）。
  - 自定义壁纸（自己选照片）正常；**系统默认壁纸读不出来，桌面和锁屏仍是黑的**。
  - 键盘仍错位、明显偏大；预装应用基本没有（只有 App Store、设置、信息、照片等）。
  - 流畅，但和上一版差不多，仍有莫名的小 jitter、小掉帧和大掉帧。
- 17:37 `phone-plus` 那轮日志也丢了（`runs/20261030-173703-phone-plus-1`，同样 `size 18807764`）：不装系统应用也丢，排除 `sheng-staged-apps`；剩下的差别是 2 GiB 种子、J120、壁纸/键盘叠加补丁。先治标：`BootProgress/RamoopsLog.c` 记下两个 zone 的基址，新增 `zones_stamp()`，在每次心跳和 `reset_now`（定时复位、观察者复位、panic 复位都经过它）时把 Linux 头三个字（sig、start 0、size）重写一遍；不管运行中是谁改了 size，复位落下时头都是对的。`Test/ramoops.c` 新增 `test_header_stamp`（把两个 zone 的 size 改成 18807764，心跳后、复位前都恢复，Linux 视角读出全部记录；日志关闭时不映射不写），-O2/-O0 加 ASan/UBSan 通过；`工具/gpu-test-loop/run-tests.sh` 全部 ok（往轮重新解码结果不变）。根因（谁写了 size 字）留待日志恢复后再查。
- **17:45 上机 `phone-plus-stamp`**（任务 `bf0dkcuuv`；内核 `c878bb14` 含日志头重写；镜像 `4370e62f`）。**用户反馈（很失望）**：键盘仍错位；预装应用没变化；照片反而黑了；壁纸、掉帧、崩溃都还在；音频、音量键、“电源休眠”这些小问题都没修。教训：连续几轮诊断包让用户看不到改善，以后只在有可验证的修复时才上机。用户补充“休眠”指系统电源休眠（熄屏后整机低功耗），按键熄屏亮屏正常。
- **18:00 对照第 1 轮**（任务 `bw29hh5zk`，重放 15:46 的旧镜像 `357ed5b3`：内核 `2774af7c`、D331p、1 GiB 合并种子、`text-fixup0` 让键盘读 `KBLayouts_iPad.dat`）。用户反馈：动画流畅一点，但在设置里点选项有时卡死；**键盘位置明显更集中、更合理，但点击仍然没反应**——画出来的键和点击判定用的几何对不上；换不换布局文件都点不了，根子在 405.33 点宽度与宽度表匹配（绘制可能按实际宽度缩放，点击判定没有），交给键盘代理重点核对点击判定一侧。
  - **结果：旧镜像日志完整保住**（`runs/20261030-180230-bisect-A-old-d331p-1`，705 条全部校验通过，最后一条 419.999 s）：手机环境没变，日志丢失是新变体引起的。另：`SYSTEM_RESET2 returned -1; trying SYSTEM_RESET`。
  - 大卡顿与 PosterBoard 崩溃同步：400.47 s、400.73 s、411.08 s PosterBoard abort（status=6，十来个海报扩展随之 SIGTERM），对应 SLOW 460 ms（gap 442）、223 ms、322 ms——修好壁纸集合（PosterBoard 不再 abort）也能去掉大掉帧。398 s 8 个 WebKit 进程 SIGKILL（status=9，疑似设置里的网页视图被 jetsam），对应用户说的“设置里点选项卡死”，待查。405 s 起 GPU 锁在 160 MHz、copy 60+ ms 是复位前 15 s 的降温停档（`adreno830_dcvs_park`），正常使用没有。
- **18:12 对照第 2 轮**（任务 `bypny5u29`）：新包 `phone-plus` 只把 target-type 改回 D331p（镜像 `测试镜像/对照B-phone-plus-D331p.img`，`592ef5ca`，内核 `c878bb14`）。
- **18:15 用户关键反馈（对照第 2 轮，新包 + iPhone 键盘布局叠加 + D331p）**：键盘在**设置 App 的搜索框里能弹出、能正常输入**；只有 **Spotlight（SpringBoard 进程内托管的输入框）里不能输入**——之前“错位、按了没反应”很可能都是在 Spotlight 里试的。方向改为 SpringBoard 侧：ArtworkDeviceSubType 2688（414×896 点）与实际 405.333×880 点的几何差异、SpringBoard 的键盘焦点/触摸路由。工作流 `wds890bvs`（`wf_8db4cd8c-9c0`，报告 `sbkb/`）专门查这一点。
- **对照第 2 轮结果**（`runs/20261030-181628-bisect-B-new-d331p-1`）：新包改回 D331p **日志仍丢**，Linux 这次报 `size 2097148`（0x1FFFFC）——机型代号不是原因；错误值随配置变化，说明是运行期数据写进了日志区，不是随机掉位。DarwinBoot 把内存盘放在 XNU 窗口里设备树之后（`DarwinBoot.c` 606 行），不会压到 0x880000000。
- **18:30 对照第 3 轮**（任务 `buzs7ltl5`）：新内核 `c878bb14` + 旧 1 GiB 合并种子 `data-a830-phone-cache.dbrdisk` + D331p、无覆盖层（`测试镜像/对照C-新内核-旧种子.img`，`69abf8be`）。丢则问题在内核（首要怀疑 GPU 槽位扩容后新窗口页表/映射让 GPU 写到错误物理地址，其次 crash-trace、叠加挂载补丁、调速器/按键改动）；不丢则在新种子（2 GiB、覆盖层、海报客户端名单）。
- **对照第 3 轮：卡死**（`runs/20261030-182035-…`未生成；任务 `buzs7ltl5`）：新内核 `c878bb14` + 旧的**开预热**合并种子（1 GiB）+ D331p，1000 s 内没回安卓，adb/fastboot 都看不到——XNU 硬卡死，定时复位没触发（复位定时器是本 CPU 定时器，观察者兜底也没跑，说明可能所有 CPU 都卡在关中断状态，或 PSCI 复位本身失败）。同一内核配关预热种子（对照第 2 轮）能正常复位；旧内核配同一开预热种子（对照第 1 轮）也正常。嫌疑：新内核里在重负载/内存紧张下会卡住的改动（crash-trace 的关中断限流锁、调速器 SCMI 等待、按键 8 ms SPMI 轮询、GPU 24 作业槽），以及开预热往 1 GiB 内存盘写约 720 MiB。18:35 已推送通知用户回来后长按电源+音量下强制重启；重启后先用 `read-ramoops.py --label bisect-C-newkernel-oldseed-1` 读这一轮可能残留的日志。在此之前暂停上机。
- **18:40–19:10 会话额度用尽**，多个代理中途失败（电池、音频内核路径、灵动岛报告、存储设计、熄屏审查、SpringBoard 键盘、预装应用审查、挂起审查）。19:10 额度恢复，手机仍卡在对照第 3 轮（用户外出未重启）。
- **19:20 合入**（不依赖上机）：
  - 键盘 `keyboard-405`（审查可合入）：根因是 UIKit 只认 6 种手机屏尺寸，405.333×880 不在其中，键盘视图按布局族设计宽度 375 建立、按键却拉伸到 405.333，右侧键落在视图外；修复是纯数据——在我们叠加的 `KBLayouts_iPhone.dat` 里新增设计宽度 405.333 点的 `iPhone-PortraitChoco405` 族并加宽度表 405 → 该族（`工具/make-phone-keyboard-layouts.py`，成品 `工具/phone-keyboard/KBLayouts_iPhone.dat` `5d106d84`，苹果解码器在模拟器里解码 5446 个键盘无误）；`kb405-make-a830-layout.diff` 已打进布局脚本。
  - 预装应用：根因 `/chosen ephemeral-storage = 1`（易失 Data）让 datamigrator 跳过整个数据迁移；补丁 `phone-staged-apps-migration.patch`（只对 `com.apple.datamigrator` 且 `sheng-staged-apps=1` 时报非临时存储）+ `phone-staged-apps-trace.patch`（拷贝与迁移进度 U 行），已加进 series（迁移补丁由主会话审阅）。解压后实际拷贝 445.4 MiB。
  - 灵动岛：代理在额度用尽前已写好布局变体 `phone-island`/`phone-island-apps`（iPhone16,1、subtype 2556，岛 125×36.67 点居中包住挖孔 569,40–647,118 像素，状态栏空隙用一条 text-fixup 居中），主机测试 `工具/test-phone-island-layout.py` 24 项通过。
  - ADSP 核心（`PaganiAdsp`，`pagani-adsp=1`，默认关）与 iOS 音频服务（`PaganiAudio`，静音输出设备，`pagani-audio=1`，默认关）：两项审查都可合入；已打 Stage7Probe 补丁、登记 `components.toml`、board.toml 组件与归档、defconfig/.config 的 `CONFIG_XNU_EXTRA_LD_ARCHIVES`、series 加 `pagani-audio-addon-mount.patch`；布局脚本新增 `A830_AUDIO=1`（种子加插件、信任包并入 CDHash、启动参数 `pagani-audio=1`）；打包脚本补齐 PaganiPanel/NativeUfs/PaganiAdsp/PaganiAudio 本地模块。
  - 熄屏省电（`pagani-screenoff` 位 0 CPU、1 GPU slumber、2 面板睡眠、3 完全断电，默认 0）与 CPU 深 idle 原型（`pagani-idle=1/2`，默认关）已在工作树里；PaganiPanel 改动主会话审阅过（默认路径不变）；Adreno830 48/48、PaganiArmCpu 各测试通过。
  - 回路镜像加 `pagani-wdog=30`（Gunyah 看门狗，防无人值守卡死）。
- **19:40 新内核 `892bda08`**（零警告；含预装应用两补丁、PaganiAdsp/PaganiAudio 强定义、crash-trace）与测试包：
  - 完整包 `测试镜像/boot-A830-回路-M8桌面-iPhone15Pro灵动岛-系统应用.img`（`493792d5`）：iPhone16,1 灵动岛身份、键盘 405 族、预装应用（`sheng-staged-apps=1` + 迁移补丁）、静音音频设备（`pagani-audio=1`，音量条）、壁纸覆盖层、17 个 Metal 客户端、关预热、`pagani-reset-after=600`、`pagani-wdog=30`；种子 `工具/seeds/phone-island-apps-audio.dbrdisk`（`63188e60`）。
  - 备用包 `…iPhone15Pro灵动岛.img`（`27b55fd8`）：同上但不装系统应用。
  - 等用户回来强制重启手机后上机。
- **20:15 审计 A（`audit/hang.md`）的两条更正**：
  - 日志丢失**不是新内核造成的**：30 轮里 17 轮都是 `size 18807764`，最早 11:49，涉及多个更早的内核与 iPad 身份；被丢的是 **pmsg 区**（头在 0x880040008，正确值 0x001FFFF4），翻转模式固定（第 24 位 0→1，第 5/10/16 位 1→0），是复位期间 DRAM 保持失效；`zones_stamp` 在 PSCI 前刚写对仍然坏，说明发生在复位/引导期间。console 区（4 份副本）有时能保住。
  - “对照第 3 轮卡死”的判据只是 1000 s 看不到 adb；但 20:20 Mac 的 USB 总线上没有任何设备（没有安卓、没有 fastboot），手机确实没回安卓。
  - **确定的缺陷 H2（已修）**：SYSTEM_RESET 返回后 `halt()` 永久 wfe，而 `gunyah_wdog_before_reset` 已在 PSCI 前关掉看门狗，所以 `pagani-wdog` 兜不住。修复：`RamoopsLog.c` 新增 `reset_retry()`——SYSTEM_RESET 返回后重新上狗（`gunyah_wdog_after_failed_reset`：最短 4 s 咬、不再喂狗），并每秒重试 SYSTEM_RESET；没有计数器（或主机测试）时仍 `halt()`。主机测试：`Test/gunyah-wdog.c` 新增复位返回后上狗并咬合、无看门狗时不动作两项，`Test/ramoops.c` 更新复位文本，console、crash 测试均通过（-O2/-O0，ASan/UBSan）。
- **20:25 内核 `fcfa92ae`**（零警告，含复位重试 + 复位失败重新上狗）与重出的测试包：完整包 `测试镜像/boot-A830-回路-M8桌面-iPhone15Pro灵动岛-系统应用.img`（`b2a6e8f7`），备用包 `…iPhone15Pro灵动岛.img`（`d552b4fb`）。布局脚本修：音频种子脚本拒绝覆盖上次生成的 `.hal` 插件目录，重出前一并删除。
- **20:30 审计裁决（`audit/judge.md`）与落地的防护**：
  - 日志区两个头是复位期间 DRAM 固定弱单元掉位，不是代码写坏的；“日志丢没丢”不能再当二分信号。16:00–18:30 那几条“新变体引起/运行期数据写进日志区”的推论作废。
  - “对照第 3 轮卡死”大概率不是卡死：用 `回安卓用时 − uptime` 换算，17:08、18:16 两次“晚回”的安卓都在 fastboot boot 后约 479 s 准时开机，只是 Mac 几分钟后才看到 adb。已推送更正：用户回来先看屏幕、插拔数据线，只有停在 XNU 才长按。
  - 新内核增量里没有找到能卡死整机或写 0x880000000 的路径。
  - 已落地：G1 折中（PSCI 返回后重新上狗）、G2（每秒重试 SYSTEM_RESET）、**G3**（复位开始 5 s 后观察者仍在跑就从它的 CPU 每秒补发 SYSTEM_RESET，一条记录；`Test/ramoops.c` 新增 `test_observer_retry`）、**G6**（`gpu-loop.py`/`ramoops_device.py`：`adb_state` 区分 unauthorized/offline，定时复位 + 90 s 后每分钟 `adb kill-server`/`start-server` 并列出 USB 上的 OnePlus/Google/高通设备，回安卓后打印 Linux 开机时刻并判“准时/提前/迟到”，超时提示改为先查屏幕再插拔的清单）、G8（`RamoopsLog.c` 注释改正为 pmsg 头、复位掉位）。待做：G4（第二个复位定时器不放 CPU0）、G5（喂狗要求 CPU0 本地心跳在涨）、G7（日志改用热复位或 UFS 持久日志）。
- **21:24 上机灵动岛完整包**（任务 `bisfuye7x`，镜像 `85079de8`）。用户反馈：灵动岛出来了；左侧 Plus 键正常；预装应用一个都没有；音量条仍不出；键盘变宽了但**任何地方都不能输入**；设置页仍很慢；有地方会卡死；电量显示不准；控制中心样式抽风；没有相机、手电筒；壁纸不完美；性能还能更好。已整理进上方“完美化目标清单”。
- **21:33 灵动岛包的日志完整读回**（`runs/20261030-213341-island-apps-1`：用户长按复位，707 条全部校验、36 份副本零翻转——PMIC 按键复位能保住 DRAM，PSCI 软件复位会掉位）：
  - 看门狗真机可用（`wdog ok: … failed 0`）。
  - 崩溃取证首次抓到并符号化：**MobileSMS** SIGSEGV ×5，`CIContext initWithOptions:` → `internalContextWithEAGLContext:` → `CI::GLContext::init` → pc=0（没列进 Metal 客户端的 App 用 Core Image 就退回不存在的 OpenGL ES）；**routined** abort（`getMOSettingsManagerClass` 找不到 iPhone 专属类）；WebKit ×14、com.apple.Sensor* ×5 被 SIGKILL。
  - 开机阶段的 U 行（预装应用拷贝结果、叠加挂载、音频服务）被冲掉：GPU 状态行每秒 4 行占满 32 KiB 事件环（约 80 s 一圈）。预装应用监视线程在 361–495 s 一行都没有 → 第 10 s 就判“迁移结束”退出（哨兵不在、待装目录不存在），推断开机拷贝没成功。
  - 修：`RamoopsLog.c` 的 `area_of` 让 G 记录走控制台环；种子禁用 `com.apple.routined`；内核与诊断包重编中（任务 `bg1tvejzi`）。
  - 21:42 工作流 `wkyv0qzt1`（`wf_27c14a3b-43f`，报告 `next2/`）：`metal-for-apps`（所有 App/扩展/小组件都能拿到 Metal，槽位与插件门槛，耗尽时不崩）、`keyboard-touch`（键盘画对了却点不动）、`volume-hud`（音量条）。
- **23:40 强制安装包读回**（`runs/20261030-234014-apps-force-install-1`，22.9% 掉位，542 条 274 条校验）：**强制安装名单没有生效**——`app moves: D S>- com.apple.migrat 29` 照旧，`bundles -2`。新探针给出：`identity: hw.product iPhone16,1 cpu type 0x100000c subtype 0x1`（ARMv8，不是 arm64e）；`sais 1`，installd 状态文件存在（734 字节），十六进制转储只解出一段残片（bplist，能认出包名，看不出状态值）。要点：删除应用包的进程名截断为 `com.apple.migrat`，即 SystemAppMigrator 跑在 **`com.apple.migrationpluginwrapper`**（XPC 服务，role account mobile，带沙箱例外权限，有 `LookupSystemAppState/UpdateSystemAppState/InstallForDataMigrator` 等 installd SPI 授权）。安装函数 `installOrUpdateAppWithBundleID:` 的流程是 IX 协调器 → `IXPromisedTransferToPath`（待装路径）→ `setComplete`，日志从未出现 installcoordinationd 搬动应用包，所以要么没发起安装（状态 2/6），要么创建协调器就失败。内核侧已无法再推进，改走用户态：
  - 23:45 工作流 `w83erogbk`（`wf_e0f3ef37-84d`）：用户态诊断守护进程 `PaganiDiag`（复用 adrenoprobed 的签名、信任缓存与 launchd 覆盖启动）+ 内核只写 sysctl（写进 BootProgress U 行，限速）：开机读 MobileGestalt/MobileInstallation 的真实回答（`SupportedDeviceFamilies`、`arm64`、`MobileInstallationCheckCapabilitiesMatch`、`MIIsApplicableToCurrentDeviceFamilyWithError`）、installd 状态文件摘要、强制安装名单可读性，并从统一日志里筛迁移插件宿主、datamigrator、installd、installcoordinationd 的出错与关键提示原话。
- **23:45 其余两份审查与跟进修复**：XNU 三补丁可上机；键盘探针 3 处已修——`pagani-kbtrace=1` 只放开键盘子类 0x2bae，`=3` 才放开 0x2b87（UIKit 通用信号点，每帧都发，会拖慢动画）；摘要间隔 600 ms→4 s；`kdebug_reset`（ktrace 会话结束）后重新放开键盘点；测试桩同步，5 项主机测试全过。Metal 插件：CI 修补与名单解析核实无误；WebKit 辅助进程在 21H461 都是 `.appex`，会被 `@extensions` 命中——新底种子 `next2/metal-for-apps/pipeline/out/data-a830-phone-cache-noprime-apps-nowebkit.dbrdisk`（名单加 `!com.apple.WebKit.*`，插件 CDHash 不变）；布局脚本：相对路径的 `A830_OWNER_TRUST` 转绝对、出包前核对种子插件 CDHash 在信任包里（漏设时报错退出，已验证）。布局测试 30/30。
- **00:25 工作流 `whnl4unn2` 完成（GPU 与音频按审查修复，各带独立复核，均判可上机）**：
  - GPU（差异 `scratchpad/gpufix/adreno830-review-fixes.diff` + 复核小修 `scratchpad/review-check/review-small-fixes.diff`）：挂死只对 Hang/CpHwError/Watchdog 归责（`A830FaultBlamesClient`，闸门回调带原因，`native_adreno_recovery_begin(bool)`）；`halt()` 只选已完成栅栏之后仍在途的作业；选槽名单优先（名单外进程只在空闲小槽多于 K 时取小槽、从不占大槽，K 为 4G 表 3、16G 表 4；名单内小类进程小槽满时可借大槽；2 GiB 表只给名单内）；backboardd 按身份给 16384 对象、无保留额度；池外分配/写合并/释放移出 Memory 锁（统计改原子）；16 GiB 判据改读 IDR2.UBS；记录表满时让出无活连接、无挂死、打开最少的一行（复核改为两遍查找，避免持 g.lock 时多余遍历）；S3 后追踪注记 `gpu aperture GiB, table pages`（16G `10 2011`，4G `4 805`）；文档同步。主机测试 48/48，内核对象 51 个零告警，每项都有先失败后通过的用例。取舍：4 GiB 默认下名单外 App 同时最多 5 个拿到 GPU，第 6 个起拿不到（Core Image 退路兜底，不崩）。
  - 音频：`pagani-audio-log=2` 只前 16 轮 10 s 一轮；探针 seqlock 写端 relaxed 奇数 seq + `dmb ish`、读端 `dmb ishld`（检查脚本直接核对 arm64 指令顺序，主机/iOS 插件/内核归档三处）；路由行加 `vc=`（按 MediaExperience `(status|value)!=0` 的有效音量控件）；注释更正。胶水测试 740 项，插件新 CDHash `c88be410`。
  - 00:26 编内核（任务 `bwllgj0gl`），合入上述两项与点屏脚本、键盘探针修订版：内核 `3b4358ca`（Adreno830、PaganiAudio 组件零警告；内核里有 `gpu aperture`、`vc=%u`、`kbt `、`pagani-input-script` 等字符串）。
- **00:39 合并测试读回**（`runs/20261031-003923-combined-input-kbd-vol-1`，console 区保住，23.4% 掉位，506 条 205 条校验）——**键盘的断点找到了**：
  - `kbt SpringBoard[27] sum ev 206 set 11 dn 0/0 dr 0 up 0/0 task 4/4 in 0/0`（322 s）：SpringBoard 收到 206 个触摸事件（点屏脚本一共发 218 帧），键盘输入视图被建立 11 次（Spotlight 搜索框在 SpringBoard 进程里），但按键层 `touchDown/touchUp` 一次都没有 → 触摸到了应用、键盘也弹了，却进不了 `UIKeyboardLayoutStar`。正是用户说的“键盘画出来了、点哪里都没反应”。
  - 点屏脚本自己的 `input script:` 行一条没留下：它比预期早约 59 s 开始。原因：`mach_absolute_time` 在这块板上是硬件计数器原值（XNU `cpu_base_timebase=0`，只在睡眠唤醒时调整），计数器从 SoC 复位起算，XNU 日志开始时已约 59 s（日志头 `counter=0x43775410 hz=19200000`），所以 `pagani-input-script-at=330` 实际在日志时间约 271 s 开跑，落在事件环保留窗口（322 s 以后）之外。已改：`PaganiInput.cpp` 的 `scriptArm` 把 `at` 改为从触摸驱动启动时起算；README 同步；外设测试两行 PASS。
  - 预装应用仍是 `app moves: D S>- com.apple.migrat 29`；`identity` 行照旧；状态文件十六进制只解出几段残片。音频路由行没出现（解码损坏或没打）。
- **04:35 M14 两组计量实现完成（工作流 `wqszvkai5` 的实现步骤；未提交、未上机，待对抗性审查）**：抢占第 0 阶段 + 按进程页表 P0，不加启动参数，默认行为不变。
  - 作业归属（架构 4.13.5）：环的戳带归属（渲染服务器按进程名 backboardd 标出它的每条连接，不论在哪个槽；其余进程；内核自己）与打戳时的环深度；`A830Fence` 结算改为同一时刻一个结算者（调用数加一后试取，忙就返回，结算者放手后复查调用数再结算一遍，谁也不等），算出每个作业的服务时间和每个渲染服务器作业前面排着的外部服务时间 fw（减去最老那个外部作业在打戳前已经跑掉的部分），每周期 rs 均/最大、fw 均/最大、n4、fj、fb、上下文入口次数。`pagani-gpu-status=2` 时 MEM 行在客户端尾巴前加 ` fw<最大>ms/<n4>`（放不下时客户端尾巴整段让位），IRQ 行队列尾部后加 ` sw<n>`（放得下才加）；第 0 行不变。
  - 按进程页表 P0（架构 4.5.6、4.12.3）：S3 诊断页第 2 行末尾 ` AP<0/1/2>`，S3 末尾一条 `gpu aperture <状态> x0=<x0>` 注记（10-31 笔记里的 `gpu aperture 4 805` 是 VA 光圈大小与页表页数的注记，不是这次 SMC 的结果，SMC 的 SCM 行早被事件环冲掉）；槽位压力（并发峰值、无空槽拒绝、窗口溢出、VM 保留额度拒绝）；页故障：710 上半部先读两个栅栏，提交对象记 64 项栅栏→范围历史，故障时在途的作业只与 FAR 窗口对比，一致/不一致/无法判断/轮询四类计数，扣连接照旧按 FAR（初稿曾“只有一个连接的作业在途就记给它”，审查 M1 后改回，见 05:30 条）。
  - 日志（架构 8.8）：不加每秒新行。完整数字每 10 s 至多一条 `gpu meter rs … fw … n4 … ext … ctx … conn … nos … ovf … vmr … pf … ap<状态>`，只在内容变化且日志开着时写。按真实格式算的日志量：`status=2` 活跃时一组 G 记录 380 → 388 字节（每秒 1–2 组：228 000–456 000 → 232 800–465 600 字节/10 分钟，+2.1%）；计量行每条约 155 字节，每 10 分钟至多约 9.3 KB（最宽 12.4 KB），空闲为 0；光圈注记每次开机 64 字节。（审查 S2：这只对 `status=2` 成立，默认 `status=1` 也在写计量行；改为只在诊断页模式写、每分钟至多一条，见 05:30 条。）
  - 验证：完整主机测试（带固件）48 通过、0 失败，内核对象 51 个零告警；新增用例在 fence、ring、submit、userclient、status、smmu、trace 七个测试里（各自单独的 PASS 行）；追踪测试的钉选行随新注记更新（1112→1113 行、462→463 行、8→9 个结束行）。
  - **发现（不在本仓库，未改）**：BootProgress `RamoopsLog.c` 的 `ramoops_log_rows` 直接 `emit(AREA_EVENTS, ...)`，绕过了 21:3x 改的 `area_of`，G 记录实际仍写进 32 KiB 事件环（00:39 那轮开机 U 行又被冲掉与此一致）；要改成按 `area_of(kind, …)` 选区才生效。
- **22:13 UFS 首测通过（用户同意开 UFS 日志；硬件观察，一加 13T，iPadOS 17.7.11，`boot-A830-阶段1-首测.img`）**，证据 `工具/gpu-test-loop/runs/20261031-221300-stage1-smoke-1`。
  - **UFS**：XNU 第一次写 UFS。`ufslog open at LBA 124178176`，state 6，418 s，145 轮，20 槽，0 写错误，GPT 0/0，最长一轮 1454 us，最长命令 1048 us，之后稳定在约 900 us、170 us。定时复位后读回 5279 条（seq 1..6188），`ufslog last snapshot before the reset: timer`。Linux 在 fastboot boot 后 500 s 起来，USB 自动恢复，没有插拔线。复位类型 HARD_RESET/PS_HOLD。
  - **基线**：主 GPT、`unixnu` 块 16515072..16703487、根镜像 9147 MiB 三项哈希不变。没有写前哈希的区域：窗口上方 251 块、分区块 2341632..16515071。备份 GPT 可与 `UniXNU/out/dual_sys/applied2/gpt-backup.after` 比对（审计发现）。
  - **小组件根因证实**：U 行 `file ids 1`；46–50 s 内 `CalendarWidgetExtens`、`TipsWidget`、`FitnessWidget`、`MusicWidgets`、`WorldClockWidget`、`FindMyWidget*`、`TVWidgetExtension`、`PodcastsWidget`、`StocksWidget`、`GeneralMapsWidget`、`MailWidgetExtension`、`HomeWidget`、`WeatherWidget`、`RemindersWidgetExten`、`ShortcutsWidgetExten`、`BooksWidgetExtension` 等第一次 exec（以前 106 个扩展零次）。`WidgetRender c157/0 e0 x6 w1 f18 r494 m34`。退出都是信号 9/15（`ns=2`），没有崩溃。用户眼睛未确认。
  - **亮度没通**：`panel: report ver 0 ctrl 0 st 0 fifo 0 dma 0 trig 0`、`lanes 0 int 0 clk 0 tpg 0 ...`、`requests 0 writes 0 shown 1433 rc -19 ... stopped`。设备树确认面板 `qcom,dsi-ctrl-num=0`，地址没错；DSI0 全部读 0 且不挂死。根因调研工作流 `w3q8ibkxo`（`wf_a3886ec7-024`）。
  - **日志缺口**：missing seq 226-240、245-259、827-1034、1039-1042、1047-1242、1247-1517、2152-2239、2245-2356，开机早段环翻得比写线程快，`panel: start` 那几行在缺口里。
  - **最终审计** `wf_a806f3a3-ede`（`build-userspace/a830-inputs/ufslog/final-audit.json`）：反汇编确认 `f7f9908f` 的写闸门与源码一致，门铃写只有 `hostSubmit`（`adoptAndRead` 不可达），`reset_now` 第一个调用 `ufs_log_quiesce`。成立且待修（低）：SPMI 读失败时电源字掉 0（修法：`keys.filter.known&PAGANI_KEY_POWER` 才发布），基线范围不足。待修小项：`ufs_log_quiesce` 认领失败后不再拦新命令、暂停没有日志痕迹、`test_power_key` 注释过头。
- **2026-11-01 05:40 阶段 1.5 首测 panic，修复后做了一轮上机前审查（硬件观察，一加 13T，iPadOS 17.7.11，内核 `876e0f15`，`runs/20261101-053955-stage15-smoke-1`）**：用户点“开始首测”后，启动第 4.8 秒 `Kernel data abort. at pc 0xfffffff00ba14384`，`far 0`，在 `_strnstr`。UFS 日志 state 5，4 轮 0 写错误，20 秒后复位，100 秒回安卓，基线六项全部不变。launchd 之后的东西一项都没验证。
  - **根因**：`phone-app-exec-count.patch` 在 `process_signature` 里读 `imgp->ip_strings`，而 `exec_activate_image` 结尾的 `execargs_free` 已经把它置 NULL，launchd 第一次 exec 必崩。计数移到 `exec_activate_image` 的成功分支，用 `excpath`。旧测试反而断言了错误写法，已改成沿真实顺序检查，旧补丁回放 3 项报错。
  - **审查**（工作流 `wf_1329b8e6-1f3`，报告 `build-userspace/a830-inputs/fullrun/preflight-{exec,stall,gpu,misc,refcount,critic}.md`）：五路加一名补漏者，从镜像解出产物逐函数比较（46136 个相同、88 个有指令差异），没有新的 crash、hang 或写坏数据的问题。发现都是看不到数据或判据写错，内核反正要重编，这一批全部带上。
  - **探针**：S1 无帧无触摸的卡死几秒原来不写 M 记录，现在采样器醒着就写；S2 目标查到任务没查到线程时进程退出，会永远卡在死任务上，现在隔 `STALL_LOOKUP_MS` 重新按名字查；S3 kbd 的 task lock 忙不再当成没有线程（`boot_progress_stall_thread_times` 返回 `max + 1`）。
  - **GPU**：G1 `gpu aperture GiB` 注记在 S3 的日志断档里从没留下来过，S9 补一条；G2 新增 `gpu fault cb.. far ..`，每次开机最多 8 行，FAR 终于有日志出口；G3 16 GiB 窗口表改成不跨 4 GiB 整数倍（槽 5、14、23 从 4、8、12 GiB 起，三段 64 MiB 空隙，槽 31 约 512 MiB）；G4 VM 区段上限 131072 改 262144。
  - **面板探针**：四个阶段全部挪到键盘脚本之后（P0 266 s 起，P3 约 351 s 结束）；DSI 版本只在同一组读到 GDSC 开时才读；读未验证的块之前置 `pagani_panel_probe_quiet`，写线程补一轮后停写，读到挂死时线路上没有写命令（`UfsLog.h`）。
  - **引用计数**：`SystemOS root refs` 的 old 8、root 7 是修复后的精确账本（反汇编 APFS：每个根自带 5 个引用，注释和判据漏算了 `v_parent`），修前应为 7 和 8，判据 use 2、3 作废，决定性的是 `root use`。panic 的最后一步是回收 private-dir 释放 `v_parent`，不是 APFS 自己的 `vnode_rele(根)`，根因和修法不变。主机模型改成真机数字。
  - **新补丁** `phone-shutdown-progress.patch`：`unmount_callback` 的跳过和 `dounmount` 前后各记一行 R 记录。调用放在 `kern_exec.c`，因为 `vfs_subr.o` 不在第二遍重编名单里，宏在那里会静默变成空操作。反汇编确认 `_pagani_shutdown_progress` 是 `b _boot_progress_root`，`_unmount_callback` 调用 3 次。
  - **判据更正**：`pagani-input-script=1` 只在 Spotlight 里点 8 下，不进备忘录，首测看 Spotlight 里 `kbt` 的 dn/up；备忘录要用户手打。`NativeUfs/README.md` 说 APFS 不发 `DKIOCISSOLIDSTATE` 是错的，固态声明把 Data 的事务阈值从 256 改成 1024。
  - **增量审查**（工作流 `wf_14b082fa-91f`，报告 `fullrun/delta-{panel,gpu,xnu}.md`，内核 `aa94decf`）：没有 crash、hang、storage 问题。反汇编确认 `quiet` 字一处写两处读，窗口表 32 项不跨 4 GiB 整数倍，`ArmSmmu4KRange` 40 字节，容量翻倍常驻多 5 MiB。还没上机就顺带四处：① M 记录在最后一帧或触摸后再写 10 s（`STALL_M_TAIL_S`，“点开应用后等几秒”的卡死原来 2 s 后就停写），采样器睡着的秒报告显示 `-/-`；② P1 相位前、P3 第二次采样前放开写线程一轮（`panel_probe_flush`），P1 flush 后重读 GCC 门闩，P3 两次读的间隔变成 4.5 s；③ `vmr <对象>/<区段>` 分开，对象上限 24576 这次没动；④ `gpu fault` 行 CB0 和其他 bank 各 8 条，没有日志统计 CB2 的故障。
  - **最后复核**（工作流 `wf_aff80f9d-eee`，报告 `fullrun/final-{binary,panel}.md`，内核 `75e75d4e`）：与 `aa94decf` 逐函数比较，47029 个函数里 11 个指令不同，6 个是这四处改动，5 个是 `fault_lines` 变长后 `g830` 偏移加 8 的连带结果，新增符号只有 `_stall_m_until`。P1 flush 后的 GCC 重读和相位采样都在暂停 1 s 之后，最坏余量约 480 ms，P3 约 355 s 结束，暂停共 12 段约 12 s。没有 crash、hang、storage、wrong-data 问题。两条报告工具的 minor 已改，内核不动。留下一条 minor 不修：`panel: p0 lat` 行在 100 ms 取样前没有 flush，取样挂死且固件很快冷复位时这行到不了 UFS，面包屑还在，寄存器 1.5 s 时已经读过。
  - **回归（主机）**：Adreno830 45 过 0 败 1 跳过；UFS `run-host-tests.sh` 退出 0；`mutate.py` 166 个全部抓到（U61 的匹配文本早已失效，这次修好）；`stall`、`stall-threads`（TSan）、`stall-glue`、`test_stall.py`、`test-peripherals.py`（面板探针 59427402 项）、PaganiDiag、三个补丁测试全部通过。撤掉 S1、S2、S3 或 M 尾巴任何一处，`stall-glue` 都报错。内核 `75e75d4e`，0 条警告；镜像首测 `11503699`、完整包 `13d2b14c`，种子 `39e635cb`、信任缓存 `9a4701ef` 不变。
- **2026-11-01 01:20 阶段 1.5 集成完成，等用户同意后上机**（工作流 `wf_55c9940f-db5` 三路修复与探针、`wf_a136b636-841` 亮度探针；收尾报告 `build-userspace/a830-inputs/fullrun/stage15-{small,probe,gpu}.md`、`brightness2/probe-fixed.md`）。内核 `876e0f15`，种子 `final/s6-final.dbrdisk`（`39e635cb`），镜像 `boot-A830-阶段15-首测.img`（`a51b866c`，996 字节，`pagani-panel=1217`，定时 420 s，`pagani-input-script=1`，去掉 `native-ufs-diagnostics`）、`boot-A830-阶段15-完整包.img`（`ea7c6214`，998 字节，`pagani-panel=65`）。全部只有主机结果。
  - **修复**：`phone-checkdirs-kernproc.patch`（关机 panic，启动记录 `SystemOS root refs`，判据 use 2 和 3）；固态标志（`NativeUfs`、`NativeRamBlock`）；`powerlogHelperd` 在种子里禁用；`phone-app-exec-count.patch`（launch 提升只算 `.app` 启动）；电源键字只在确认电平时更新；复位开始后不再开新轮（`reset_coming`）。第 478 行的 `status=100` 是十六进制，即 `exit(1)`。
  - **GPU**：默认光圈 16 GiB（32 窗口，页表 10+2011 页），GPU 地址超过 4 GiB 在真机上从没跑过，`gpu va past 4 GiB` 行要重点看；调频默认 `targetBusyPermille=750`、`downMarginPermille=850`，模型里迟到帧 1.85% 到 4.49%，要看 ≥115 fps 占比不低于 18.6%；`gpu noslot`、`gpu sec` 挂在 `pagani-diag` 下。
  - **探针**：`phone-stall-probe.patch` + `BootProgress/StallProbe.h`（1029 行补丁）：调度延迟（`M`）、线程采样（`Q`，帧间隔跨 100/200/400/1000/3000/8000 ms、kbt 未结束）、8 核快照、IPI 计数；报告 `工具/gpu-test-loop/stall-report.py`。挂在 `pagani-diag=1` 下。写线程开机前 30 s 周期是否改 100 ms 见 `stage15-probe.md`。
  - **亮度探针**：`PanelProbe.h`，位 128（P0、P1）、1024（P2、P3）、2048（A0：只置 `GCC_DISP_AHB_CLK` 一位）、4096（允许首条 DCS）。`=65` 行为逐项差分不变。判读表在 `brightness2/probe-fixed.md`。
  - **回归**：UFS `run-host-tests.sh`、`test-peripherals.py`（探针 45744756 项）、Adreno830 45 通过 0 失败 1 跳过、PaganiDiag、调速器三项、三个补丁检查全部通过；UFS 变异 166 个全部抓到，T4 改成确定性（`test_counters`）。
  - **待上机顺序**：① 首测（新内核、探针、16 GiB、自动打字）；② `A830_PANEL=67` 单独验 CA；③ 完整包。ADSP 分级测试按 `adsp-review.md`。
- **23:10 完整包日志分析（工作流 `wf_5c93da18-198`，全文 `build-userspace/a830-inputs/fullrun/analysis.json`，证据 `工具/gpu-test-loop/runs/20261031-223924-stage1-full-1`）**。运行 1183 s，用户滑动关机时 panic，UFS 保存路径第一次真实触发并成功（state 5，394 轮，0 写错误，最长一轮 7118 us）。基线六项复核全部不变（含备份 GPT、窗口末 15 块、导出区全零）。Mac 上开着 i4Tools，自带 adb 抢端口，曾造成假 `CHANGED`。
  - **关机 panic**：`vnode_rele_ext ... usecount -ve : -1, v_type 8`，不是 bindfs 引入。SystemOS 联合挂到 `/` 时 `checkdirs_callback` 对 kernproc 多释放一次旧根，卸载根卷才暴露，使用中不会触发（把握约九成，缺回溯，靠排除法）。48 轮历史里这是第一次真走到关机。
  - **应用崩溃**：Maps 确定是 `NBRenderEncoder` 缺 `setDepthClipMode:`（上游有，走 Metal 合并）。Measure、Spotlight、Maps、FindMy、MobileSlideShow、AegirPoster 541–549 是 GPU 槽位用尽（4 GiB 光圈 12 窗口，小组件修好后扩展占满，nos 到 15）。AegirPoster 317/536/539 有设备也崩，原因未知。MobileNotes 未捕获异常，内容未知。
  - **Jitter**：排除 GPU 队列、驱动调用、IOSurface、BO 清零、slumber、UFS 写线程、全系统冻结。“末段三重叠加”“CPU 压档”两条解释已被反驳。无法区分“没东西画”和“卡住”，必须上内核采样探针。连续动画约 97 fps，与 GPU 频率无关。查实：块设备没声明固态（I/O 限流用 40/85/200 ms），`powerlogHelperd` 每 10 s 崩溃重启（status=100，25 次，误触发 launch 提升），GPU 47 s 撞温控、100 °C。
  - **键盘**：kbd、语言模型、音频、触感都不是去处（kin 最大 1 ms，`task` 最大 25 ms）。备忘录三次长停顿 3137、6281、12571 ms，首尾相接且精确翻倍，比值 2.002、2.001，来源未知；“主线程忙”未证明，可能是主线程在等回复。`up` 的 n² 增长是停顿的结果不是原因。`EnableMLTapTyping=false` 无效。
  - **下一包**（工作流 `stage15-fixes-and-probes`）：checkdirs 修复、固态标志、powerlogHelperd、调度延迟与线程采样探针、每秒 GPU 计量、GPU 槽位。分析报告提到的 `A830_EXTENDED_ARGS` 不存在，命令行仍只剩 2 字节。
- **22:34 完整包用户反馈（硬件观察，一加 13T，iPadOS 17.7.11，`boot-A830-阶段1-完整包.img` `3c0e7f38`）**：小组件“完美运行”，天气、日历、新闻、备忘录、音乐、股市、Tips 都能正常渲染，部分仍显示 waiting，用户判断可能是没联网。系统“丝般顺滑”，比之前顺很多，但有时轻微掉帧，也有**严重 jitter、严重卡顿，不可预测**，高负载可能卡，有时原因不明；用户要求下一步重点修这个。音量控制有显示了，但仍无声音。摄像头驱动没修好。充电状态不显示。WiFi、蓝牙仍不能用，要重点修。键盘打字依旧要卡好多秒才输入一个字符（说明 `EnableMLTapTyping=false` 没有解决）。用户提到看见布局在修（emoji 排待确认）。
- **22:16 完整包上机**（`boot-A830-阶段1-完整包.img`，`--android-timeout 6000`），等用户测。这一包亮度不会动。
- **21:30 第 1 阶段集成完成，等用户决定 UFS 写入后上机**（工作流 `wf_595fff07-849` UFS 日志、`wf_845dd3fa-973` 非 GPU 修复；报告 `build-userspace/a830-inputs/ufslog/report.md`、`build-userspace/a830-inputs/upstream/stage1/summary.md`）。内核 `f7f9908f`，种子 `final/s6-final.dbrdisk`（`684396ce`），镜像 `boot-A830-阶段1-{首测 89bfa185（420 s，997 字节）,完整包 3c0e7f38（5400 s，998 字节）,不发布背光 f3c435a1（pagani-panel=81）,无UFS日志 6c1ef203}.img`，都带 `.payload.json`。全部只有主机结果。
  - **上机前只读核对（2026-10-31 20:53，手机安卓）**：三星 KLUFG8RHHD-B0G1 rev 2301；`data_reliability=0x00`（LUN 0–5 全是 0）；`lun_write_protect=0`；`logical_block_size=0x0C`；`unixnu` 是 `sda16`，起始 859273216（512 字节扇区），绝对 4K 块 107409152，窗口 LBA 124178176，导出区 124112640。`userdata`（sda15）、`super`（sda14）、`persist`（sda2）、`metadata`（sda10）都在 LUN 0。规范对 0 的说法：写入中掉电，整个逻辑单元的数据都可能丢。报告要求这时把残余风险交给用户决定，所以上机前必须先问。
  - **基线** `build-userspace/a830-inputs/ufslog/baseline-20261031.txt`：主 GPT 前 6 块、`unixnu` 块 16515072..16703487、`unixnu` 前 9147 MiB（根镜像）、窗口加导出区 288 MiB（确认全零）。首测后全部复核，前三项必须不变。
  - **主线核实**：亲自读了 `hostScsiDir`/`hostLogCommand`/`hostLogOpen`/`LogWindow.h`/`LogInterface.h`/`UfsLog.c` 和复位路径。`hostSubmit` 只有三个调用点；运行期命令都走 `gHost.lock` 和槽 0，没有绕锁路径；UFS 流在 SMMU 上旁路，控制器本来就从同一块 DMA 内存读描述符，写方向 DMA 没有权限问题。绝对 LBA 自算与修复后一致（第一版报告的 61697792 是错的）。
  - **主线新增：电源键暂停**。长按是测试中最常见的、软件看不见的复位。`PaganiInput` 发布去抖后的物理电源键（`pagani_input_power_held`，脚本按键不算），写线程按下时补一轮，之后不开新轮，导出分块返回 -9 重来，松开恢复。`Test/ufslog.c` 的 `test_power_key`，变异 U80–U85。
  - **主线决定**：① 这一包 `pagani-panel=65`，不发真 0x28/0x29。这是它们第一次真正生效，以前熄屏靠黑帧，按“有风险的新功能单独测”放到后面单独一轮；报告说“只多 1 字节”不准，`pagani-panel=1` 在 board.toml，追加会变 1013 字节，改用新环境变量 `A830_PANEL` 替换。② 这一包就关 `EnableMLTapTyping`，推翻 merge-plan 的“等 kbt 数据”：`kbtrace` 按阶段分解，关掉后信息不损失，iPad 身份本来就走传统路径，用户每次上机都有成本。③ 受热时启动提升保持现状：`launchLevel` 已钳在温控上限内，95 °C 不提升。④ os_log 和 CacheExport 这一包不加：int-245 记录过目录填满后扫描占 45% CPU，会污染抖动判断，且 PaganiDiag 还没证实在真机运行。⑤ 去掉 `pagani-audio-mx=1` 只是少一个只读探针，不影响音量修复。
  - **回归（主机，最终源码）**：UFS `run-host-tests.sh` 全过；内核配方零新增告警；`test-peripherals.py`（含按键、亮度 124981 项、电源序列 314 项、发布顺序）全过；调速器 `scmi-perf`、`psci-idle`、`spi-gate` 全过；Adreno830 45 通过 0 失败 1 跳过；PaganiDiag `Test/run.sh` 全过；85 个 XNU 补丁打上，整核和预链接通过；`_boot_log_export_put` 是 `UfsLog.o` 强定义，`_pagani_input_power_held` 只有一份。ABI 头 `20ac9298` 未变，种子底层 `s1b-cache` 未重做，着色器包继续有效。
  - **打包疏漏（已修）**：命名镜像要连 `.payload.json` 一起拷，否则 `gpu-loop.py` 看不到 `sheng-log=1`，不读 UFS 窗口。
- **18:30 上游调研完成，定合并方案**（工作流 `wf_5aed96b8-62c`，报告 `build-userspace/a830-inputs/upstream/merge-plan.md`，试验成果 `upstream/artifacts/`）：用户说的上游是 `UniXNU/UniXNU` 组织。上游没有 A830 驱动，`feat/ipados27-gpu-parity` 已进 main，内容大多与系统版本无关。决定：不换 iPadOS 27；不整体换 Turnip 后端（我们有 `dsb sy` 和准入 Busy 处理）；ABI 头不动（动了 Turnip `LC_UUID` 变，着色器包失效）；CP 抢占、系统内存 BO、每进程页表暂不做。阶段 1 不动 Metal：`bindfs_vget`、亮度、`dot-pitch`、调速器、内核小优化、诊断加 `chronod`（工作流 `w0zg0z778`，`wf_845dd3fa-973`），另并 UFS 日志（工作流 `wwu99mesq`，`wf_595fff07-849`）。阶段 2 单独合 AdrenoMetal 到 `d6ca655`（39 提交，21 处冲突有解法，`repo2.bundle`），翻译层补丁从 16 增到 37，没上过 A830，必须单独成包、留回退。
- **16:47 第二次上机同一完整包**：用户反馈（硬件观察，一加 13T，iPadOS 17.7.11）：整体相当流畅，基本跟原生没区别，打开应用偶有小抖动；电量正常；音量键能调但不出声；键盘能用但每键反馈很慢，底部 emoji 那排小错位；小组件仍不能用；亮度调节无效；WiFi、蓝牙不能用。启动约 1850 s 后又是定时复位（`HARD_RESET`/`PS_HOLD`），用户未来得及长按，两区再次全丢，USB 未枚举，插拔线后才回 adb（证据 `runs/20261031-174545-a830-full-2`）。两轮都没拿到日志，长按不能作为唯一通道。只读确认：`/sys/kernel/reboot/mode` 是 `cold`；`unixnu` 分区是 `sda16`，64 GiB，头部 `SHNGROOT`；`sheng-log=1` 的日志窗口在其末尾 32 MiB（NativeUfs，有范围检查），待用户同意再用。上游调研工作流 `w9ynu9fmq`（`wf_5aed96b8-62c`），报告在 `build-userspace/a830-inputs/upstream/`。
- **13:15 用户同意后第一轮上机：`测试镜像/boot-A830-完整包.img`（`629cd45d`）**，`fastboot boot` 发送与启动 OKAY。手机在启动约 1880 s 后回到安卓（PMIC：`HARD_RESET`、触发源 `PS_HOLD`，不是长按），与 `pagani-reset-after=1800` 相符，说明内核撑到了定时复位，中途没有 panic 复位。pstore 两个区都被 Linux 丢弃（`found existing invalid buffer`），日志、验收诊断结果都没读回；用户现象待反馈。证据 `工具/gpu-test-loop/runs/20261031-134632-a830-full-1-1`。要拿日志须 30 分钟内长按电源 + 音量下。`--android-timeout` 要大于定时复位，这轮用 2400。
- **12:05 手机连着（安卓，电量 100%），跑了只读事实（不拉文件）**：`wifi/morning-facts.sh` 默认模式，结果 `build-userspace/a830-inputs/wifi/phone/20261031-115842/`：端点 `17cb:110e rev 1`，BAR0 大小 0x200000，安卓 32 个 MSI。**固件在 `modem_a` 的 FAT（挂在 `/vendor/firmware_mnt`）**：`image/peach/{amss20.bin,amss.bin,aux_ucode20.elf,aux_ucode.elf,Data.msc,bdwlan.e01…}`、`image/tmel_peach_20.elf`，PaganiWifi 能就地读，不占启动包；`/odm/etc/wifi/peach/{bdwlan.elf,regdb.bin}` 在 super 里的 ODM（读不到时放种子）。传感器 `sensors/phone/dumpsys-sensorservice.txt`：43 个硬件传感器，BOSCH bmi26x 加速度/陀螺仪、**AKM ak0991x 磁力计（真实存在，更正盘点“未知”）**、tcs3720 距离/前 RGB、stk37600 后光感、OPLUS 融合光感、计步；配置 `/vendor/etc/sensors/config`（sensor pack 要从这里取）、registry 723 项在 `/mnt/vendor/persist/sensors/registry/registry`。
- **12:00 全量合入完成，两张镜像就绪（未上机）**。硬件审查 `wyfwvokjv`（`wf_0f02c7ee-24a`）五项全交付：
  - **adsp-review**（`a830-inputs/adsp-review.md`，含早上分阶段参数 A1–D2）：确认 16 项，高 5：音频 SID 改共用 UFS 流 0x60 已在跑的旁路银行（`SpeakerSmmu.h`，只写一个 S2CR/SMR，sheng 编程转换型上下文会整机复位）；undo 不管 PAS 是否关掉都撤票 → 关不掉就留票；SCM 超预算记 `scm_parked` 不再发安全调用；中断接上不响时转轮询；`GET_WQ_CTX` 唤醒被 GPU 的 994 处理函数抢走 → `Sm8750Scm.c` 共享 `all_wakes`。中：`RESUME_TX` 地址错（sheng 同错，传感器指示流会停）、GENI 读前不清 RX FIFO、默认天花板 −6 → −12 dBFS（约 0.6 W）、`pagani-adsp=1` 不再隐式开电池。0xd0000000 按 UEFI 图是 DXE_Heap 空闲，上机未验证。
  - **sensors-review**（`sensors/build-report.md`）：线上格式无缺陷；补 MobileGestalt 运行时节点、接近重报、默认关的方向服务（含 `'ashd'` 与沙盒放行）；**FastRPC 反向文件服务已建成**（`--sensor-pack`）。`pagani-sensors` 改位域：1 服务、2 SUID、3 数据流与节点、+4 FastRPC、+8 方向。
  - **wifi-review**（`wifi/review.md`）：修 16 项：端点不应答时第 4 阶段 panic；DMA 窗口默认放 4 GiB 以下（`pagani-wifi-dmapa=0xc0000000`）；S7I 读不到时不再拉起芯片；`pagani-wifi-chipreset` 默认 0。`board-adt-wlan.diff` 只能和阶段 6 一起上。
  - **diag**：A 包零 `diag:` 行的根因是 Sandbox 平台策略不允许 xpcproxy 从 `/private/var/ngpu/bin` 运行（静态）；守护进程移到 `/private/var/factory_mount`，只需 `pagani-diag=1`；60/180/600 s 功能核对。`check-paganidiag-host.py` 的 seed 项失败是它在已合入的脚本上重复打差异，工具本身跑通。
  - **torch**（`torch/report.md`）：PaganiTorch 完成（50 mA，上限 200，5 分钟自动关）；**iOS 入口不需要相机栈**：发布 AppleH6CamIn + 用户客户端 + `/product/camera flash`，`/System/Library/MediaCapture/H6ISP.mediacapture` 叠加插件不导出设备创建函数，只替换厂商类 6 个方法，只有 FigFlashlight 客户拿到占位设备（`pagani-torch-ios=1`，默认关）。
  - 合入：`Sm8750Scm-shared-wakes.diff`、`adreno830-shared-locks.diff`（GPLL0 投票与 QMP 拿锁字，无竞争时一次 CAS）、Stage7Probe 加 torch、`phone-torch-overlay.patch`、新 `phone-diag-sysctl.patch`、种子与布局工具（`--sensor-pack`、`A830_DROP_ARGS`、同名参数去重、`A830_EXTRA_FIXUPS`、`A830_BUILD_TIME`）。**内核 `fd380975`**（零告警，127684608 字节，日志 `kernel-build-a830-integrate3.log`）。**种子 `final/s5-final.dbrdisk`**（`77c0ac67`，57.6 MB；s1b-cache → s2-audio → s3-full → s4-diag → s5-final，`工具/make-a830-final-seed.sh`）；信任包含 Metal `06f47e3e`、音频 `b961a1e8`、诊断 `6fd7d43e`、手电筒 `ab8693e5`、Turnip `2b12ec65`。
  - 全量回归：Adreno830 44 通过 0 失败；音频、马达、ADSP、传感器、扬声器、电池、WiFi、手电筒、平台、钥匙包、外设、面板、布局、壁纸、种子工具全过。
  - **`测试镜像/boot-A830-完整包.img`（`629cd45d`，986 字节）**：`A830_DROP_ARGS=startup_debug`、`A830_AUDIO=1 A830_SYSTEM_APPS=1 A830_BATTERY=1 A830_RESET_AFTER=1800`、HEIC 两条修补，`pagani-gpu-status=3 sheng-no-lc=1 pagani-haptics=1 pagani-haptics-ios=2 pagani-audio-srzl=1 pagani-audio-mx=1 pagani-halt=2 pagani-rtc=1 pagani-diag=1`。**`测试镜像/boot-A830-稳妥回退包.img`（`b1f848b3`，854 字节）**：同内核同种子，只 `pagani-audio=1 pagani-gpu-status=1`。测试步骤与判读见顶层 `测试清单.md`。
  - 未完成：actionbtn（逆向一半）、research、bootargs-ext（参数够用，暂不需要）；设置页慢根因未定位；App cryptex 未挂载。
- **11:50 ui-rest 完成（`wx8o5td6h`，报告 `build-userspace/a830-inputs/ui-rest/report.md`，集成说明 `ui-rest/integrate/README.md`，未上机）**：
  - **WebKit 辅助进程被杀的根因（确定）**：不是 jetsam 也不是宿主 kill，是内核 exec 路径上 AMFI 的启动约束：23 条 `status=9 ns=0`（`psignal_vfork_with_reason`，全部运行里只有它们是这个指纹），WebKit 是类别 4，约束要求 on-authorized-authapfs-volume 或 on-system-volume，而 SystemOS 经 `mount(MNT_RDONLY|MNT_UNION)` 挂载，两项都假。修复：启动参数 `sheng-no-lc=1`（`zzyc-owner-skip-launch-constraints.patch` 早就在内核里）。受影响的还有 adattributiond、HealthFollowUpExtension。**更正**：bisectA 398 s 那批来自 Safari 启动，与设置页无关；设置页慢的根因未定位。放开后的下一道关：App cryptex 未挂载，8 个任务缺程序（`com.apple.Safari.History`、webbookmarksd、webpushd、webinspectord、safarifetcherd、passwordbreachd、SafariBookmarksSyncAgent、AuthenticationServicesAgent，这也解释 Safari 启动时的 `xpcproxy status=4e00`）；种子规则 `!com.apple.WebKit.*` 让 GPU 进程没有 Metal；WebContent 将是第一个带 JIT 授权的进程。
  - **HEIC**：VCPHEVC 能服务瓦片（模拟器实测解出 `J120.HEIC`），`IOSurfaceAccelerator` 不是硬依赖，“没硬解就解不了”逆向不支持；未证实的单点是远程 videocodecd（`SeparateCodecProcess`），硬门控只有调用方传 `UseHardwareAcceleration=true`（QuickLook 等）。两条文本修补（`dyld_shared_cache_arm64.07`）：`_VTShouldRunVideoDecodersInProcess` 偏移 0x321e900 改为返回 1（绕开 videocodecd）；`_VTSelectAndCreateVideoDecoderInstanceInternal+0x360` 偏移 0x2fd6f58（“强制硬件”不再排除软件解码器）。`A830_EXTRA_FIXUPS` 带入（`ui-rest/integrate/vt-fixups.txt`）。风险：进程内解码的内存算在调用进程头上。视频不是同一堵墙（mediaplaybackd/mediaserverd 本来进程内解码），YCbCr IOSurface 经 Metal 采样这一环未上机验证。“照片反而黑了”很可能是当时 Metal 名单没有照片（@apps 22:40 才加），与 HEIC 无关（推断）。
  - 布局脚本：合入 `A830_EXTRA_FIXUPS`；新增同名参数去重（XNU 取最后一个，省 20 字节）。命令行真实上限 1000 字节（`UniXNU/scripts/unixnu/payload.py`），不是 1023。
- **11:36 默认生效改动审查通过 + 着色器缓存重做（`wx8o5td6h` 的 default-on-review、seedvars 交付；ui-rest 还在跑）**：
  - 审查结论（`build-userspace/a830-inputs/default-on-review.md`）：钥匙包、jetsam 下限、调速重试、0x52008 共享锁、键盘探针 v2（关时零行为）、Stage7Probe 弱默认、BootProgress 默认路径、AdrenoMetal 七项都没有会让手机开不了机、卡死或桌面坏掉的缺陷，可以默认开。修了两个低严重度：F1 钥匙包带外输出长度上限 1 MiB；F2 无纹理附件上残留 resolve 纹理会让整个 pass 被拒（QuartzCore 不设 resolveTexture，不影响 backboardd）。余量风险 R1：`avail` 长期低于 235000 页时用 `pagani-jetsam=256`。
  - **着色器缓存失效的回归已修**：0018 改了运行时源码 → `NMB_RUNTIME_ID` 变（`04e07824…` → `95df2826…`）→ 种子里的 m2vpack 全部失效，内存盘每次开机又是新的，界面每条管线都要现场翻译。修法：构建树 `--tree live` 重编插件（CDHash `06f47e3e…`，含 F2），`make-shader-cache-a830.py` 新增 `--replace-pack`，按新运行时重新生成（pack `0c5839b7`，166 文件；严格消费者禁止 Turnip 编译时同样 54 条管线建成，命中 35）。**今后每次改运行时源码都要重做这一步**，已写进 `工具/make-a830-final-seed.sh` 的说明。
  - 种子变体工具 `工具/edit-phone-seed-launchd.py`（31 用例）：`--auto-lock`（靠数据迁移把 `SBAutoLockTime` 转成 `maxInactivity`）、`--no-auto-updates`（MobileAsset 没有关闭开关，只能把服务器指到本机，手动下载也不通）、`--sos-off`（`com.apple.SOS` `SOSDeviceSupportedKey`）、`--language zh-Hans`、`--enable`；报告 `seedvars/report.md`。守护进程结论：现在没有一个能直接进日常包；可单变量试 PerfPowerServices、locationd（identityservicesd 必须保持禁用）、CommCenter（只为对比设置页）；bluetoothd、identityservicesd、routined、watchdogd 保持禁用，放出 watchdogd 会让内核 panic。主包只加 `--sos-off`：长按电源 + 音量取日志正好是 SOS 手势，声音通了以后会响警报。
  - 种子 `final/s3-full.dbrdisk` 57.14 MB（`工具/make-a830-final-seed.sh`：s1b-cache → s2-audio → s3-full）。内核待 ui-rest 与硬件审查后再重编。
- **11:00 音频审查完成（`wblsmu0py` 的 audio 代理，报告 `build-userspace/a830-inputs/audio2/build-report.md`，已合入）**：确认并修复 7 项：①closed-loop 原在 Stage7Probe 就写入、叠加层缺失时客户端全报 -43 → 节点改到 launchd 前 `pagani_haptics_ios_commit` 创建，看得见 `hapticengineconfig.plist` 才写闭环，否则退回 A 阶段并记日志；②节点存在但插件/服务不可用时 VirtualAudio 空等 8 s、可能丢 `'srzl'` → 服务能提供马达且插件可见才建节点；③插件把“列出设备”和“环可用”绑死 → 拆开，内核选项在就列出，无环只拒绝 IO；④首样本到马达 30–34 ms → 预缓冲 20→8 ms，估算 18–22 ms；⑤3600 mV 流能持续数秒 → 持续电平限制（高于 1260 mV 有 80 ms 额度、1/4 速度恢复，`pagani-audio-haptics-sustain-mv`）；⑥`pagani-haptics-ios` 只接受 1/2；⑦叠加层加 `pagani-haptics=1` 条件。核对通过：三段代码签名、10 个进度全局、`srzl`/`rwbu` 16 字节载荷、过滤器偏移（对照 21H461 dyld 缓存）；R2 在探针串行队列、一次性；FigVibrator 启动路径无阻塞；默认路径与当前镜像等价。**A 阶段可能完全不振**（MediaExperience 启动时只查一次 VirtualAudioDevice_Actuator，dispatch_once 不重试，早于 VirtualAudio 发布就整次开机没振动），看到 `runs=0` 直接上 `pagani-haptics-ios=2`。主机：音频 abi 77、engine 140、bridge 99、plugin 215 + actuator 110 + listed 89、glue 1626；马达 spmi 9047、core 92809、ios 195；31 变异全杀。**内核 `a00cbabd`**（10:57–10:59，零告警，127520768 字节，含 `pagani_haptics_ios_commit`、`ramoops_log_power_off`，日志 `kernel-build-a830-integrate2.log`）；种子 `final/s3-full` 已按新插件（CDHash `b961a1e8…`）重做。
- **10:57 关机/重启与系统时间完成（`wblsmu0py` 的 rtc 代理，产出 `build-userspace/a830-inputs/rtc/`，已合入、未上机）**：PaganiArmDTPlatform 覆盖 `haltRestart`、`getGMTTimeOfDay`、`setGMTTimeOfDay`、`start`（`PlatformHalt.h`、`PlatformRtc.h`，14719 项 + 59 变异）；BootProgress 新增 `ramoops_log_restart`、`ramoops_log_power_off`（`rtc/integrate/bootprogress-halt.diff` 已打）。`pagani-halt=1`：重启/面板重启走复位路径，关机走 PSCI SYSTEM_OFF（Gunyah 对 critical VM 交固件；看门狗保持 4 s 退路；安卓关机同一调用 0x84000008，真实结果要上机确认）；`pagani-halt=2`：关机也走复位（日志保得住，建议第一次用）。查实日历从 1970 起算（`IOKitInitializeTime` 的 IORTC 等待在 BCM2837 配置下编译掉，`getGMTTimeOfDay` 返回 0），且日历初始化不等平台专家（有竞争），加线程在 IOBSD 发布后补设。`pagani-rtc=1`：PMK8550 RTC（RTC_CTRL 0x6146 bit7，RDATA 0x6148–0x614b，连读两次一致才用）；`pagani-rtc-offset=<秒>`（`date +%s` 减 `/sys/class/rtc/rtc0/since_epoch`）；无偏移时下限取 `/chosen/pagani-build-time`（布局脚本 `A830_BUILD_TIME`）。iOS timed 在本移植上没有下限。回归：布局 34 项、BootProgress 四个测试、`gpu-test-loop/run-tests.sh`、变异全集 49 个全过。
- **10:58 发起硬件审查与收尾 `wyfwvokjv`**（`wf_0f02c7ee-24a`）：adsp-review（PaganiAdsp/Speaker/Battery）、sensors-review（含 FastRPC 依赖缺口、方向服务崩溃陷阱）、wifi-review（阶段 1–5 硬件安全、无协议栈时干净失败）、diag 收尾（A 包零 `diag:` 行的原因 + 分时功能核对）、torch 收尾。
- **10:25 撞周额度后恢复（用户“请再试一次”）**：09:34–09:36 所有子代理同时报 `You've hit your weekly limit`（工作流 `w7vq0fbw3`、`wii8gnlwj`、`wnkrer2zi`、`wh0jyo1c5`、`w53c8x8ez` 与 `wnpiyiyco` 的审查阶段都中断）。核对落盘：
  - **已完成**：系统修复四项（`build-userspace/a830-inputs/sysfix/`）——调速器重试（2246649 项）；钥匙包（默认开，`pagani-keybag=0` 关，RFC 3394 + 每次开机随机类密钥）；`phone-gpu-client-memlimits.patch`（开过 Metal 用户客户端的进程内存上限至少 512 MB，默认开，`pagani-jetsam=0` 关；**更正盘点**：图标服务被杀是因为“非活跃硬上限 6 MB”，不是活跃 20 MB；WidgetRenderer 98/45 是软上限，小组件黑与 jetsam 无关；GPU BO 映射不计入 phys_footprint）；键盘（**更正**：A 包的 `FPS 9 … gap 500` 是屏幕静止时的周期动画，`cand 0/0` 是探针数错路径，真实每键停顿在 kbd 的 CPU 变换器语言模型 + 内联补全束搜索 + CPU 被钉低频；种子偏好 `KeyboardInlineCompletion=false`，探针 v2 替换 `phone-keyboard-trace.patch`）。WiFi：事实 + 移植完成（`PaganiWifi`，主机测试全过），审查没跑；Ath12kWifi 栈要拉 Linux/FreeBSD 源码（下载需用户同意）且要先有固件，这版内核只编阶段 1–5。
  - **基本完成**（主机测试全过，缺收尾/审查）：audio2（路由 R1 进度探针 `pagani-audio-mx=1`、R2 `'srzl'` 补装 `pagani-audio-srzl=1`、R3、Actuator 设备、内核桥、`PaganiHapticsIos`、叠加层生成器、`phone-haptics-overlays.patch`）；PaganiSensors（1987677 + 1053 + 4747 + 519 项）；PaganiSpeaker（含 `SpeakerLimit` 软限幅、0x52008 共享锁 `gccvote` 162646 项）；PaganiBattery（`BatteryProps`）。
  - **未完成**：ui-rest、seedvars、bootargs-ext 没开始；rtc 只拉了源码；torch、diag、actionbtn 半成品；所有审查都没跑。
  - **主线合入**：`series` 加 `phone-gpu-client-memlimits.patch`、`phone-haptics-overlays.patch`，换键盘探针 v2；Stage7Probe 加 speaker/wifi/sensors 弱钩子（顺序 audio → speaker → haptics → wifi → sensors）；`components.toml`、board.toml 登记三驱动；`make-phone-seed.py` 合入 `--haptics-overlay --keyboard-haptics --keep-inline-completions --wifi-firmware`；`make-a830-layout.sh` 合入 `A830_SENSORS`、`A830_WIFI_FW`；新脚本 `工具/make-a830-final-seed.sh`（s1 → s2-audio → s3-full，可重跑）。**内核 `f38f9e4b`**（10:18–10:20，零告警，127516672 字节，日志 `kernel-build-a830-integrate1.log`）；`test-peripherals.py` 全过；种子 `final/s3-full.dbrdisk` 56.65 MB；**完整包候选 1**（`e81e5a29`，尚未审查）走通全链路：载荷未超限。启动参数 888 + 93 = 981/1023。
  - 10:17 工作流 `wblsmu0py`（音频审查修复、关机/重启 + RTC），10:25 工作流（默认生效改动审查、ui-rest、seedvars）。
- **09:16 盘点工作流 `waga00lun` 全部完成**（`wf_5b5ad5f2-d96`；`功能盘点.md` + 评审补充 33 项、更正 20 条、重叠 18 条）。要紧的新增：①**iOS 里关机/重启是坏的**（PaganiArmDTPlatform 没有 `haltRestart`、`PE_halt_restart` 为空 → 点关机后约 50 s 黑屏，30 s 关机超时 panic，再经 `pagani-panic-reset` 回安卓）；②iPhone 专属 App（计算器、指南针、电话、钱包）iPad 根卷上没有，要从 iPhone 固件取需用户同意；③拼音候选为 0 就打不出汉字（随 sysfix 键盘项）；④启动参数只剩约 135 字节（D 包 888/1023 字节，含重复的 `pagani-gpu-status`）；⑤32 KiB 事件环要给新驱动分配日志预算；⑥运行时设备树节点必须在 `*_start()` 返回前、按启动参数同步建好（launchd 的 `_LimitLoadToDeviceTree` 与 MobileGestalt 求值更早）；传感器工作流的 `ios-sensors-adt-nodes.diff` 把 accel/gyro 静态写进 board.toml，不能直接合；⑦Data 内存盘 2 GiB 会被写满（待装应用 467 MB），联网前要关自动更新下载；⑧`pagani-adsp=1` 会隐式打开 PaganiBattery（早上测试要注意耦合）；⑨钥匙串改动必须落在 `DRV/NativeKeyStoreEvents`（构建链接指向它）。本硬件做不到（D）：蜂窝/电话/短信/蜂窝数据、卫星定位（另有未验证的 MPSS+QMI LOC 喂 locationd 模拟定位的 C+ 旁路）、NFC/Apple Pay、Touch ID/Face ID、苹果原生相机、App Store 第三方应用、“嘿 Siri”、隔空投送/接力/热点/手表、UWB、红外、80 W 快充。
- **09:18 发起第二批 `wii8gnlwj`**（`wf_e50646ae-38c`）：rtc（PMK8550 RTC 只读 + 偏移/构建时刻下限 + **关机/重启修复 `pagani-halt=1`**：重启走 BootProgress 复位路径、关机走 Gunyah 允许的 PSCI SYSTEM_OFF）、seedvars（`工具/edit-phone-seed-launchd.py`：放出被禁守护进程逐个评估、自动锁屏、关软件更新与 MobileAsset 自动下载、关紧急 SOS 自动拨号）、torch（PaganiTorch：PM8550 SID1 闪光灯手电模式 50 mA、≤200 mA、5 分钟自动关；逆向 iOS 手电筒入口）、actionbtn（Plus 键 `pagani-plus-key=2` 做操作按钮，默认仍是静音开关）、**bootargs-ext**（XNU 补丁：`PE_parse_boot_argn` 在主命令行找不到时退到设备树 `/chosen/pagani-boot-args`，布局脚本新变量 `A830_EXTENDED_ARGS` 写入；解决启动参数满的问题）、research（USB、相机、蓝牙、显示扩展、SoCCP、GPS 旁路、硬件缩放服务、iPhone 专属 App 的设计笔记）。
- **09:05 用户问“修好了吗，我要完全完美”**：如实回答——已修未上机：查找、小组件、默认壁纸、马达内核驱动；在修：音量与声音路由（audio2 逆向找到根因：MediaExperience 只设一次 `'srzl'`、不检查返回、不重试，而 VirtualAudio 延迟初始化完成前拒绝一切 SetPropertyData，二者竞态时 `'srzl'` 永久丢失 → 设备过滤器停在“只收六个 UID”；修法 R2：我们的插件就在 audiomxd 进程里，三条件同时成立时把 MX 的监听器补装上；另：加 `haptics` 节点后 VA 延迟初始化最长等 8 s 找 `'pact'` 端口，必须让 `Actuator` 设备在 HAL 初始化时就发布）、触感、CPU 调速器、钥匙串、图标服务内存、键盘、WiFi、传感器；要上机调通才能完美：扬声器、传感器、充电状态（都靠从未上机的 ADSP）、WiFi；本硬件做不到：蜂窝/电话/短信、卫星定位、NFC/Apple Pay、指纹/面容、苹果原生相机（手电筒灯能点亮，但 iOS 入口挂在相机栈上）。
- **09:06 盘点完成 `功能盘点.md`**（规划阶段 08:42 写出，评审阶段在补充）：A/B/C/D 分级 + 11 个新工作流 + integrate（第 3 节）。与已在跑的对应：cpu、keychain、ui-polish 的 jetsam/候选词由 sysfix 覆盖。发起**第一批 `w7vq0fbw3`**（`wf_9cf0aef2-e7d`）：speaker（PaganiSpeaker 接线、0x52008 共享锁助手、内核测试音、软限幅）、diag（早上验收核对，低优先级不拖慢界面）、battery（IOPS 字段、无 ADSP 时的充电判定）、ui-rest（设置页 WebKit 被杀卡死、HEIC 照片软解），之后两路审查与修复。第二批待发：rtc（系统时间疑似从 1970 起）、seedvars（放出 locationd 等与自动锁屏的单变量变体）、torch、actionbtn、research。
- **08:38 发起系统修复工作流 `w53c8x8ez`**（`wf_98a7fc34-a9c`，产出 `build-userspace/a830-inputs/sysfix/`）：盘点证据里今晚能修完的四项，各自独占文件、并行：①CPU 调速器遇 SCMI BUSY（-6）整轮关闭（D2 41.849 s 起性能核停 ~2 GHz、超大核停 ~1 GHz）→ 暂时性结果重试退避、只在硬失败时关（`PaganiArmCpu`）；②钥匙串写不进：AppleKeyStore 选择子 0xa（`_aks_wrap_key`）每次开机失败 64–68 次 → 按 libaks 契约实现包/解包与 0x1f 等，类密钥每次开机随机（Data 本来就是易失内存盘）；③图标服务（J120 活跃上限约 20 MB，D 包被 jetsam）与 WidgetRenderer（98/45 MB）等 GPU 客户端的内存上限 → 找出上限来源与 GPU 内存记账，做最干净的覆盖；④键盘每键约 500 ms 卡顿与候选词为空（`cand 0/0`）→ 逆向 UIKit 键盘同步等待、kbd 预测、按键音与触感、iOS 17 变换器自动更正模型在无 ANE 机器上的行为，定位并修。之后安全/正确性两路对抗审查与修复。
- **08:30 类 Linux 复位前序列工作流 `wd64q06wo` 完成**（`wf_b5021d7c-99d`，产出 `build-userspace/a830-inputs/linux-like-reset/`：FACTS.md、TESTPLAN.md）：BootProgress 新增 `pagani-reset-seq=0x<掩码>`（0x002 清下载模式 TCSR 0x1FD9000：读/写 0/回读；0x001 关 SDI `0xC2000109(1,0)`；0x004 停 SPMI 仲裁器 `0xC2000901(0)`；0x008 停其余 CPU；0x010 OEM 热重启 `0xC3000922`；0x100 开机只读探测），顺序同 13T Linux；**不带参数时复位路径与快照逐字节相同**（decoder 两组 zone 镜像与 expected.tsv 哈希不变）。审查 6 条全修：F1 任何危险位或 `pagani-reset-cold=1` 自动带上 0x002 并以“回读必须为 0”为闸门，否则走老路径（防 XBL 进 CrashDump 或写 minidump）；S3 带掩码时看门狗先 PET+SET_TIME(2000,4000) 保持到 PSCI 前；S4 停核后保持屏蔽中断；S5 掩码只接受最后一次出现的小写 `0x`+1–8 位十六进制且无未知位，否则整体拒绝（`reset seq REFUSED`）；测试计划第 0 步“普通 adb reboot 是热还是硬”改为必做、每轮带 `pagani-reset-cold=1`。主机测试 8/8（-O2/-O0、ASan/UBSan）、16 个变异全杀、`gpu-test-loop/run-tests.sh` 通过。**今后编内核用驱动仓库的 BootProgress（不再用 06:36 快照）**；默认关，只影响日志能否保住，列为早上可选测试项。08:29–08:30 整核编译：**内核 `aa210730`**（零告警，含复位序列与马达驱动，日志 `kernel-build-a830-resetseq.log`）。
- **08:20 小组件与壁纸工作流 `w78ripr5b` 完成**（`wf_2d917d8c-814`，产出 `build-userspace/a830-inputs/{widgets,wallpaper}/`，都没上机）：
  - **小组件全黑的根因（主机复现并修复）**：我们的 Metal 对所有调用方都回答支持 Apple1–3 家族；SwiftUI 的渲染器 RenderBox（每个小组件在 WidgetRenderer_Default 里由它画）在 `RB::Device::Device` 从 Apple1 往上问，看到 Apple3 就在 `RB::ColorMode::pixel_format` 给 RBLayer 选苹果私有打包/XR 格式（不透明打包 RGB8_444_2P 542、500/502/520/522/523，带透明 550/551，BGR10_XR_sRGB 555），metal2vulkan 建不出这些纹理或带混合的管线 → 渲染编码器被拒、命令缓冲失败；RenderBox 只打一行 NSLog、不回退，而表面在提交后已插进 CAImageQueue → 渲染服务器显示一块从没写过的黑表面。主机实证（我们的前端 + metal2vulkan 跑在 MoltenVK，RBLayer 经 CARenderer 合成读回）：不修整帧全黑、4 个命令缓冲以 `MTLPixelFormatRGB8_444_2P` 被拒；修后像素与 Mac GPU 逐个相同。修复（`UniXNU/drivers/AdrenoMetal`，MIT 自有源码）：①`FamilyAnswer.h` + `NativeBridgeMetalFrontend.mm`：`supportsFamily:` 的调用方映像是 RenderBox 时对 Apple3 以上答 NO（其他调用方不变：QuartzCore 仍 Apple3、Core Image 仍 Apple2）；②桥接层补上渲染通道 `resolveTexture` 转发（多重采样解析以前不写目标：影响 SceneKit/SpriteKit/VectorKit 画的天气、地图、照片小组件与表情/头像海报），C 与 Rust 两侧 `NmbPassAttachment` 同步加字段；③客户端报告行（带进程名的首个问题行 + 点名客户端每 10 s 状态行 `c/e/x/w/f/r/m`，`pagani-gpu-status=3` 时进 G 行）。主机检查 `check-adreno-userspace-host.py` 全过（新增 family_answer_host、client_report_host），msaa_resolve_host 修前 FAIL 修后 PASS，SwiftUI ImageRenderer 5 种类小组件视图三种颜色模式像素与 Mac GPU 一致。已知旧问题：`check-native-metal-frontend-host.py` 本来就缺 `native_prime_*` 符号链接不过。
  - **默认壁纸全黑的根因（确定）**：我们叠加层两个壁纸包的 `logicalScreenClass` 写成 `"Any"`，WallpaperKit `+[WKWallpaperBundle shouldLoadWallpaperBundleAtURL:]`（0x1b970f628）用区分大小写的 `isEqualToString:` 与常量 `"any"`（`dyld_shared_cache_arm64.17` 文件偏移 0x5f28cf4：`61 6e 79 00`）比较 → 两包都被拒 → `WKDefaultWallpaperManager` 合成纯黑 “Solid Black”（`400.Solid_Black.png`），PosterBoard 把它装成 CollectionsPoster 默认海报。iOS 26.5 模拟器里苹果自己的 WallpaperKit 实测：旧包 REJECT、新包 LOADS。第二道坎：图层是 HEIC，上游 UniXNU `910a19d` 在同类无硬件 HEVC 的机器上实测 HEIC 壁纸全黑、换 PNG 就好。修复：重写 `工具/make-phone-wallpaper-overlay.py`（`"any"`、sips 转 2732² 8 位 sRGB PNG 放进包内、去掉符号链接、7570 与 7565 硬链接共用一份 10.75 MB），新增校验器 `工具/check-phone-wallpaper-overlay.py`（含 CAPackage 渲染：24 个状态、锁屏/主屏 × 亮/暗黑像素 0%）、`工具/replace-phone-wallpaper.py`（换进现成种子）、15 项主机测试；`make-phone-seed.py --wallpaper` 保留硬链接。种子：`wallpaper/seed/phone-island-apps-audio-findmy-wallpaper.dbrdisk`（“查找”插件 + 新壁纸）。**更正 01:36 条**：那时说“`DefaultWallpapers` 缺失时叠加层已提供”与事实不符——叠加层的包根本没被加载。
  - **启动载荷预算很紧**：192 MiB FAT 上限 95%，07:47 内核（127 MB）+ 新种子后只剩约 8.2 MB；之后加驱动/固件/叠加层都要算账（`unixnu payload` 超限会直接拒绝）。
  - 08:13–08:14 重编 Metal 插件（0018 + 小组件三项修复；`deploy-findmy-plugin.py --tree private`）并换进“查找+壁纸”种子：**中间种子 `build-userspace/a830-inputs/final/s1-metal2-wallpaper.dbrdisk`**（56.1 MB）+ `…-owner-trust.bundle`，插件 CDHash `3caf266d…`（核对：含 `_native_bridge_metal_family_withheld`、`_native_bridge_metal_counts`），Turnip `2b12ec65…` 不变。之后在它上面叠音频插件与触感叠加层。
- **08:00 查找与音量工作流 `wmt6uc0z9` 完成**（`wf_52372dfb-039`）：
  - **“查找”闪退已修（源码 + 主机复现 + 新种子）**：根因不是纹理格式，而是 App 小槽位的 GPU 地址窗口只有 124 MiB（4 GiB 表 4–11 号槽），Turnip 把内存堆报成 124 MiB，metal2vulkan 运行时却按固定 64 MiB 一块申请（缓冲区池与图像池分开，keep_room 还提前建块）：第一个 64 MiB 缓冲区块后只剩约 60 MiB，VectorKit 的 AlphaAtlas（R8Unorm 128×128、8 级 mip、Shared、ShaderRead）要的 64 MiB 图像块在 `darwin_allocate_iova` 失败 → OutOfMemory → nil → `abort_with_reason(2,6,"VectorKit received null texture from Metal")`。修复：桥接补丁 `UniXNU/drivers/AdrenoMetal/bridge/patches/0018-pooled-blocks-fit-the-heap.patch`（块 = min(64 MiB, 最小堆/16)，向下取 2 的幂，下限 1 MiB：App 4 MiB、大客户端 16 MiB、渲染服务器仍 64 MiB），已加进 `build-adreno-metal-bridge.py` 补丁表。主机（Adreno830 主机内核模拟 + 主机 Turnip）修复前 4 号槽四个图集全 OutOfMemory，修复后 84 个纹理全建成、窗口装下 87 MiB；运行时单测 48 个通过；新检查 `UniXNU/scripts/check-texture-atlas-host.py`。新种子/信任包 `build-userspace/a830-inputs/findmy/out/phone-island-apps-audio-findmy.{dbrdisk,json}` + `…-owner-trust.bundle`（插件 CDHash `dbcf2191…`，只换插件主程序）。风险：运行时标识变了，种子里的着色器翻译缓存失效，首次开机可能多一次管线重翻译（需要时用 `make-shader-cache-a830.py` 重做缓存包）。
  - **音量键未定案**：逆向把原因收窄到三道关口之一——①MediaExperience 没给 VirtualAudio 装序列化监听 `'srzl'`（之前 VirtualAudio 只收 Null_Device/Actuator/Halogen/Hawking/Flicker/Penrose 六个 UID，路由改变全被拒）；②监听装了但我们的设备没被收下（建设备抛异常，如独占模式 `'oink'`）；③设备和雷电口 `'ptbo'` 都在但路由没选它。交付了只读诊断（out=0 时探针 t1 四个字母给出是哪一关）和 `add-pagani-audio-data.py --replace/--drop-cdhash`（能把重编插件换进现成种子）。**按用户“修好再测”的要求，不拿诊断包上机**；08:05 发起工作流 `wnkrer2zi`（`wf_d1088d96-5b3`，产出 `build-userspace/a830-inputs/audio2/`）：逆向 audiomxd 里 MediaExperience 初始化的全部依赖（种子停用的 identityservicesd/bluetoothd/CommCenter/locationd/routined 是否卡住它）、设备创建对每个属性的要求、J120 路由库的选路条件 → 让三道关口都按构造通过；同时在同一插件里加 UID 为 `"Actuator"` 的马达设备、内核把马达环接到 PaganiHaptics（Float32 48 kHz → 8 位 FIFO、限压、静音即停）、`pagani-haptics-ios=1/2` 时内核在 IODeviceTree 建 `/product/haptics`（不改 board.toml）、Tunings/MediaExperience 两处叠加层的 XNU 补丁与生成脚本、键盘触感偏好；种子从“查找”新种子出发以保留 0018 修复。
- **07:55 震动与声音工作流 `w2voyxoe6` 完成**（`wf_f64a094f-f2b`，产出 `build-userspace/a830-inputs/haptics/`，三路都没上机）：
  - **马达内核驱动 `PaganiHaptics` 完成**（BSD-2-Clause-Patent，已拷入 `UniXNU-Pagani-Drivers/drivers/PaganiHaptics`）：PMIH010x 的 HAP530_HV（SPMI 从机 7：CFG `0xf000`、PATTERN `0xf100`、HBOOST `0xf200`；170 Hz LRA “0809”）。事实来源：一加 13T 内核分支的 `qcom-hv-haptics.c`、上游 HAP530 补丁 v4、UEFI PmicDxe 逆向、`spmi-pmic-arb.c`（GPL 只取事实）。写操作白名单只放 SID 7 的 CFG/PTN，HBOOST 与 SDAM 只读；写前核对仲裁器 v7、APID 归属 EE0、TYPE 0x15/SUBTYPE 0x0a、版本，**每次写前重读归属**；驱动电压硬上限 7450 mV、运行默认 3600 mV；单次 2 s、流 30 s、5 s 通电预算；出错即停并保持关；开机不等待。API：恒定振动、板载 15 个效果、PCM 流式写 FIFO（8–48 kHz）、停止。`pagani-haptics=1` 开、`=2` 干跑（只读：身份、归属、寄存器），`pagani-haptics-test=N` 开机振 N ms。主机测试 spmi 9047 项、core 91832 项（ASan/UBSan）、31 个安全变异全杀；已接线（Stage7Probe 弱钩子 + 调用、`components.toml`、board.toml include/archives）；**内核 `24de85e6`**（07:46–07:47，零告警，含 9 个 `pagani_haptics_*` 符号；BootProgress 仍用 06:36 快照；日志 `kernel-build-a830-haptics.log`）。默认关，镜像不加参数时行为与 `95a53442` 相同。
  - **iOS 触感链路（静态逆向，全部能靠数据/配置打通，不改苹果二进制）**：①设备树有名为 `haptics` 的节点 → MobileGestalt `DeviceSupportsHaptics`/`VibratorCapability`；`/product/haptics` 的 `closed-loop`≠0 → `DeviceSupportsClosedLoopHaptics`，这是 CoreHaptics、UIKit 二级反馈（含键盘触感）和统一振动引擎的总开关；②音频服务器里必须有 UID 恰为 `"Actuator"` 的 HAL 设备（VirtualAudio 按 UID 建 `Device_Actuator_Aspen`、`'pact'` 端口和 `VirtualAudioDevice_Actuator`），触感服务器和振动引擎只往它输出，格式 48 kHz 单声道 Float32；③按 `hw.model` 短名 J120 取数据：`/Library/Audio/Tunings/J120/Haptics/hapticengineconfig.plist`（闭环时必需，缺了 -43）、`MediaExperience.framework/J120` 的振动表是空的（D83 的表在同一 iPad 根卷上，142 个模式）。分 A 阶段（不开闭环：通知/来电/4095 振动走 LegacyVibeEngine）和 B 阶段（`closed-loop=1` + 预设叠加同包：键盘、UI、CoreHaptics 全通）。预设：苹果 D83 原版要下载（需用户同意，今晚不做），先用我们自己合成的 `draft/make-j120-haptics.py`（24 个 tap 波形、47 个事件 ID）。
  - **声音可行性**：扬声器代码大半已写（`PaganiAdsp`、`PaganiSpeaker`：两颗 TFA9873 在 `i2c@990000` 0x34/0x35、副 MI2S gpio121–124、GPR/APM 建图；`PaganiAudio` HAL 插件与内核环），13T 上一行没跑过；**最大拦路虎是 iOS 路由**（与音量键同因：`out=0 ports=0 vcsp=0`），再是 ADSP 起机。麦克风更难（WCD939x + SoundWire + codec DMA）。估计：内核测试音 1–2 周、iOS 出声再 1–2 周、麦克风再 2–4 周（含多轮上机）。
- **07:30 用户去睡觉前：全部功能盘点 + 修到完美再测**（规则见上）。新发起三个工作流，与在跑的四个并行（共七个）：
  - 盘点 `waga00lun`（`wf_5b5ad5f2-d96`）：四路并行扫描（硬件/设备树、我们的代码与可移植代码、iOS 端需求、真机证据）→ 规划（写 `功能盘点.md`，按 A 今晚可完成 / B 今晚写代码待上机 / C 数周 / D 本硬件不可能 分级，拆成互不重叠的工作流）→ 完整性评审。产出 `build-userspace/a830-inputs/inventory/`。
  - WiFi `wnpiyiyco`（`wf_b8d5697d-1df`）：sheng（小米平板 6S Pro，WCN7850，已能连 WPA2、DHCP、上网）的 WiFi 栈移植到 13T 的 WCN7880 peach（PCIe `17cb:110e`）：事实 → 新驱动 `UniXNU-Pagani-Drivers/drivers/PaganiWifi`（`pagani-wifi=N` 分阶段，默认关，固件就地读分区或放进内存种子）→ 硬件安全/正确性两路审查 → 修复。产出 `build-userspace/a830-inputs/wifi/`。
  - 传感器 `wh0jyo1c5`（`wf_35a5c8ca-fee`）：ADSP 传感器保护域（QMI 传感器客户端走我们的 IPC 路由，含它需要的文件服务）+ iOS 端 HID 服务（光线、距离、加速度、陀螺仪、指南针、气压）：事实（协议与 iOS 消费链路两路）→ 新驱动 `PaganiSensors` 与 `PaganiAdsp` 的 QRTR 客户端接口 → 两路审查 → 修复。产出 `build-userspace/a830-inputs/sensors/`。依赖 `pagani-adsp=1`（ADSP 至今没在真机跑过，早上要先过 ADSP 分阶段测试）。
  - 共享接线文件（Stage7Probe、components.toml、board.toml、defconfig、series、布局脚本）各工作流只出 diff 到各自 `integrate/`，由主线统一合入、编内核，避免互相覆盖。
- **07:20 D2 读回与壁纸线索（`runs/20261031-070836-pkgD2-widgets-wallpaper-1`，734 条 733 条校验、零翻转）**：用户专测小组件与壁纸后长按。没有任何崩溃记录（X），除 `mount_hfs status=100` 外没有异常退出，GPU `RUN 160M … f0 h0`——小组件和壁纸是“画成黑的”，不是进程崩溃。开机早段的 exec 记录被事件环冲掉，看不到 WidgetRenderer/PosterBoard 的槽位行。
  - 默认壁纸的资源事实（系统卷 `/Library/Wallpaper`）：默认清单只有 `DefaultWallpapers~ipad.plist`（集合 `62879BFA-…-081E` 壁纸 7570）；集合有 `iPadOS_17~ipad`（7570，`.ca` 分层动画 + HEIC 图层，`logicalScreenClass` 只有 `1024w-1366h@2x~ipad`/`834w-1112h@2x~ipad`）、`Lake~ipad`（同样只有 iPad 尺寸）、`Stripe` 和 `Bokeh`（`logicalScreenClass any`，程序化绘制，`Bokeh` 标 `supportedDeviceClasses iPhone`）。iPhone 身份下 iPad 尺寸的壁纸全被 `+[WKWallpaperBundle shouldLoadWallpaperBundleAtURL:]` 过滤掉。
  - 我们 10-30 做的叠加层（`工具/make-phone-wallpaper-overlay.py`）已给 iPhone 默认 ID（集合 `…0817` 壁纸 7565）配了 `logicalScreenClass Any` 的替身，图层是 iPad7,1 “Flat” 版的 `J120.HEIC`/`J120-Dark.HEIC`（`assets` 软链到系统卷）。仍黑。**首要嫌疑：HEIC 解不出来**——13T 下没有苹果的硬件 HEVC 解码器，用户自选能显示的照片多半是 JPEG/PNG；其次是 `Any` 与 WallpaperKit 常量的大小写（系统里 `Stripe`/`Bokeh` 写的是小写 `any`，常量串在共享缓存里被合并，`strings` 查不到，待逆向确认）。WallpaperKit 里还有 `WallpaperLogicalScreenClass` 偏好键和 `wk_ignoreLogicalScreenClassForWallpaperBundle`，可能能直接放开尺寸过滤。壁纸工作流（`w78ripr5b`）正在逆向 CMPhoto/VideoToolbox 的 HEIC 解码路径核实；若成立，叠加层把 HEIC 图层转成 PNG 并改写 `main.caml` 引用即可（只改我们生成的数据）。
  - 四个工作流仍在跑：复位序列 `wd64q06wo`、震动与声音 `w2voyxoe6`、查找与音量 `wmt6uc0z9`、小组件与壁纸 `w78ripr5b`。本机负载约 23（16 核）。
- **07:05 D 包长按复位读回（`runs/20261031-070114-pkgD-longpress-1`：690 条 689 条校验、零翻转、36 份副本全在——用户在场时长按是最可靠的日志通道）**：用户（很急）：小组件仍黑、壁纸没有、音量键不工作、要求快修。
  - **音量键根因**：`audio: open 1 by audiomxd`（插件已加载）；路由探针 `audio route #1 … ours=35 vold=1 run=0 vcsp=0/0 … out=0 … ports=0`——HAL 有我们的设备且有音量控件，但 VirtualAudio 的默认虚拟设备没有任何输出端口、`vcsp=0`，SpringBoard 的 `_isVolumeChangeAllowedForState` 因此拒绝改音量、不弹条。问题在路由层，不在按键。
  - **“查找”闪退根因**：pid 298 FindMy 74 s SIGABRT，符号化：`abort_with_payload` ← `ggl::MetalResourceManager::newTexture` ← `upload` ← `ggl::AlphaAtlas` ← `-[VKSharedResources _initWithDevice:]` ← `md::MapEngine` ← `-[VKMapView initWithDescriptor:]`——VectorKit 建地图的 alpha 图集纹理时我们的 Metal 返回空就中止（地图、天气地图同样会崩）。metal2vulkan 的格式表里有 A8Unorm，失败点还要查（参数组合、尺寸、槽位窗口）。
  - 其他退出：CategoriesService/HeuristicInterpreter/AppPredictionInternal 被 SIGKILL（`ns=2`），iconservicesagent 被 jetsam（`ns=1 code=7`）。小组件、壁纸相关进程在用户长按前（XNU 约 160 s）没有崩溃记录——用户试的时间太短。
  - 07:04 再上 D 包（任务 `b7jfer4qi`）请用户专测小组件与壁纸再长按；同时发起工作流 `wmt6uc0z9`（`wf_52372dfb-039`，产出 `build-userspace/a830-inputs/{findmy,volume}/`）：“查找”纹理失败的根因与修复（含把重编的插件换进现成种子与信任包的工具）、音量路由（VirtualAudio 为何不给我们的设备输出端口）的修复。符号缓存 `build-userspace/a830-inputs/dsc-21H461.a2s` 已建。
- **06:50 用户：控制中心“基本上完美，不像以前错乱”**（`DefaultModuleSettings~iphone.plist` 叠加生效，完美化 #6 完成）。**新的最高优先级：震动，其次声音**（“没有震动感觉非常奇怪，必须修好，全部要原生完美”）。
  - 震动硬件（`build-userspace/a830-inputs/dt/running.dts`，由备份的“运行中设备树.dtb”反编译）：PMIC 高压线性马达驱动 `qcom,hv-haptics@f000`，在 SPMI 总线 0（`qcom,spmi@c42d000`）的 PMIH010x（从机 7），外设 0xf000/0xf100/0xf200，FIFO 空中断；LRA 周期 0x16fa µs（约 170 Hz）、vmax 1260 mV、FIFO vmax 7450 mV、刹车码 ff 3f 1f、各 effect 的 FIFO 数据、SDAM 校准；SoundWire 触感标为不支持。→ 硬件能流式播放任意波形，可以做出 iPhone 式触感。
  - 发起工作流 `w2voyxoe6`（`wf_f64a094f-f2b`，产出 `build-userspace/a830-inputs/haptics/`）：①震动硬件与内核驱动 PaganiHaptics（SPMI 写只放行 SID 7 的 0xf0–0xf2，默认关 `pagani-haptics=1`、干跑 `=2`，主机模型测试）；②iOS 触感链路逆向（CoreHaptics/AVHaptic、系统震动、键盘触感、各道门槛：MobileGestalt、VirtualAudio 产品配置、我们的 HAL 插件要发布什么）；③扬声器/麦克风可行性（ADSP AudioReach 图、功放、配置数据、里程碑与工作量）。
- **06:45 用户确认 C 包大成功：预装应用全部出来且能正常打开**——备忘录、提醒事项、时钟、新闻、Apple TV、播客、iTunes Store、健康、天气、股票、家庭、图书、文件等都能用（完美化 #2 基本完成）。剩下：**“查找”黑屏闪退**；**小组件仍黑**；**默认壁纸仍黑**。用户要求继续把全部功能做好，重点是小组件和壁纸。
- **06:31 A 包读回（`runs/20261031-063106-pkgA-diag-cc-f2-1`，console 区保住，382 条 109 条校验）与用户反馈**：
  - 用户：有点卡；键盘“能工作了，但输入极其卡、可能不到一帧”；音量键不工作；提示、提醒事项、邮件只有“缓存图标”，点开让去 App Store 下载；电池没变（A 包本就不开）。
  - **键盘修好了一半**：Spotlight 里 `dn 18/18 dr 12 up 15/15 in 12/12`（上一轮 SpringBoard dn 0/0），F2 生效；`cand 0/0` 候选词从没回来。卡顿：末段 `FPS 9 … SLOW 497ms gap 500 … copy 0.7`、GPU 忙 9%——合成器不交帧，不是 GPU；首要嫌疑是 `pagani-kbtrace=3`（UIKit 通用信号点全局打开），其次 kbd 预测服务无响应。
  - **控制中心叠加成功**：`overlays: … controlcenter stage 5 status 0`（四个叠加层全挂上）。
  - **预装应用**：拷贝成功（4214 文件）；监视行 `bundles -2` = `/private/var/containers/Bundle/Application` 从未被创建，一个完整应用都没装上 → 迁移器校验轮“should be installed, but is actually NOT installed”→ 降级占位 + App Store（与用户看到的一致）。主程序 CDHash 都在信任包的苹果模块里（日历/邮件/提醒/提示/天气/备忘录逐一核对），所以不是“签名不在信任缓存”，是安装链路本身失败。
  - 诊断守护进程没跑起来（无任何 `diag:` 行，包括内核的顶替行；早期行可能被 `kbt` 汇总行冲掉，但每 30 s 的行一条都没有）；音量：没有 `audio route` 行。GPU 计量 `ap1 conn 4/4 nos 0 ovf 0 vmr 0`，无槽位压力、无外部作业挡路。
  - **对策（C 包）**：不再走安装链路——新 XNU 补丁 `phone-system-apps-view.patch`：`pagani-system-apps=1` 时，开机拷完待装应用后删掉迁移哨兵 `/private/var/datamigrator/migrate`（不跑迁移，就不会删副本、降级占位），再把 `/private/var/staged_system_apps` 只读叠到 `/Applications`（与系统应用无重名、无包名冲突），lsd 重建数据库时当内置应用注册；U 行 `system apps view stage %d status %d, migration sentinel %d`，`overlays:` 行加 system apps。真实编译 0 错误、57 条告警（无新增）。布局脚本新增 `A830_PREBUILT_SEED`（底种子已丢，直接用备份的现成种子与信任包）和 `A830_SYSTEM_APPS=1`；布局测试的共享缓存路径改到 `build-userspace/a830-inputs/systemos-21H461`（SystemOS 卷只读挂载），32 项全过不再跳过。
  - **C 包**：`测试镜像/boot-A830-C包-系统应用叠加.img`（`a0733dbc`，内核 `95a53442`，BootProgress 用 06:36 快照以免带进复位工作流的半成品），867 字节：系统应用叠加、音频、`pagani-diag=0`，**不开**键盘探针和点屏脚本，900 s 复位。06:39 上机（任务 `b1lq5astw`，日志 `build-userspace/a830-inputs/pkgC-run.log`）。
- **06:16 用户回来，真相：01:01 那轮没卡死，是 Mac 认不到 USB**：安卓 uptime 18538 s → 01:07:25 开机，即 fastboot boot 后约 465 s 准时定时复位回了安卓；电池 100%、USB 充电中。此后 5 个多小时 Mac 的三个 USB 控制器上都没有设备，直到用户插拔数据线（18:20 那次“卡死”同样是这种情况）。日志：两区都被 Linux 丢（硬复位掉位），读不到。
  - **PMIC 复位记录（`/sys/pmic_info/poff_reason`，第 0 条最新）**：[0] 01:07 XNU 复位 = `HARD_RESET`，触发 PS_HOLD；[1] 00:59 安卓 `adb reboot bootloader` = **`WARM_RESET`**；[2][3] 更早的 XNU 复位 = `HARD_RESET`。`download_mode=1`，`DLOAD dump type: mini`。→ 安卓自己重启是热复位（DDR 自刷新，pstore 保得住），XNU 的复位（PSCI SYSTEM_RESET 与看门狗咬合）都是硬复位（掉位的根源）。我们早已用 SMC64 `0xC4000012` 类型 0 试过 SYSTEM_RESET2，返回不支持；安卓的热复位更可能来自它关机前的序列：关 SDI（SCM `0xC2000109`，参数 1,0）、停 SPMI 仲裁器（SCM `0xC2000901`）、按需清下载模式，然后 SYSTEM_RESET——XNU 一样都没做，TZ 就当异常复位处理。下一步：照这个序列做一个默认关的“复位前序列”（用户在场时单独测，风险是进 CrashDump 需长按）。
  - 06:18 上机 A 包（任务 `bir2mglkx`，日志 `scratchpad/pkgA-run.log`）。
  - **06:16 会话重启后草稿目录被清空**（`/private/tmp/claude-501/…/scratchpad` 在 06:16 重建）。还在：`测试镜像/` 的 A/B 两包、`工具/seeds/` 的当前种子与信任包（已另存 `build-userspace/a830-inputs/2026-10-31-pkgAB/`：`phone-island-apps-audio.dbrdisk`、`…-owner-trust.bundle` 等）、各仓库里已合入的补丁与 diff（控制中心补丁已在设备仓库）。丢了：诊断守护进程构建产物 `diag/out`（源码在驱动仓库，`scripts/build-paganidiag.py` 可按位重建，CDHash 应为 `7301a805…`）；底种子 `next2/metal-for-apps/pipeline/out/data-a830-phone-cache-noprime-apps{,-nowebkit}.dbrdisk` 与 `owner-trust-apps.bundle`（要么重跑插件规则那条流水线，要么给布局脚本加“直接用现成种子+信任包”的入口）；热复位研究的代码与资料 `battery-wr/warm`（设计要点在 02:20 条）；M14 研究原文（要点在 03:40 条）；IDA 数据库、挂载的系统卷（重新挂 `UniXNU/out/ipsw/21H461/044-76040-038.dmg`）、`real-compile.py`、`analyze-run.sh`。教训：重建需要的输入一律放仓库或 `build-userspace/` 下，不放草稿目录。
- **05:42 A/B 两包按新内核重打（内核 `f638f5b0`：M14 计量 + 日志分区修复 + 电池 F1–F3 + 诊断/控制中心/F2，零错误零告警，字符串核对到 `gpu meter`、`gpu aperture`、电池重投票提示、`overlay controlcenter stage`、`diag: launch pid`；`RamoopsLog.o` 已按 04:40 的修改重编）**：
  - A 包 `测试镜像/boot-A830-A包-诊断-控制中心-键盘F2.img`（`4ef538c1`，912 字节，不开电池）；B 包 `测试镜像/boot-A830-B包-A加电池.img`（`66bb5f81`，929 字节，`pagani-battery=1`）。两包都是 `pagani-gpu-status=2`、660 s 定时复位、看门狗咬合复位。
  - 上机除 02:36 条列的项目外再看：开机一次性 U 行（`staged apps copy`、`overlays:` 含 controlcenter、`identity:`）和 `diag:` 迁移期 SAM 行这次应能留到复位（日志分区修复的验证）；每分钟至多一条 `gpu meter … poll <n> conn … pf a/b/c/d ap<状态>`（annibale 默认配置下 `poll` 接近 60，`ap` 应为 1）；状态行里 ` fw<最大>ms/<n4>` 与 ` sw<n>`；f/h 计数与 FPS/SLOW 分布和上一轮同量级，不应出现新的 `GPU LOST`。
- **05:40 M14 计量（`wqszvkai5`）完成：实现 → 审查（先修再上机）→ 修复**，主机测试 48 项全过、内核对象 51 个零告警，未上机：
  - 抢占第 0 阶段：环戳带提交者（内核/渲染服务器 backboardd/外部），每个渲染服务器作业前排的外部 GPU 时间 `fw`、超过 4 ms 的次数 `n4`；status=2 时 MEM 行追加 ` fw<最大>ms/<n4>`，IRQ 行追加 ` sw<每秒上下文入口>`。结算改为“单结算者”（调用者占不到就立即返回，从不等待），热路径只用原子量。
  - 按进程页表 P0：`CP_SMMU_APERTURE` 结果（S3 诊断页 ` AP<0/1/2>`、开机一条 `g3.40 note gpu aperture`、计量行 `ap`）；槽位压力（并发连接峰值、无空槽拒绝、窗口溢出、VM 保留拒绝）；页故障归属（故障时栅栏快照 + 64 项 seqno→范围历史）只作诊断计数 `pf 一致/不一致/无法判断/轮询`。
  - 审查的必修项 M1 已改：实现者曾按“故障时唯一在途的连接”扣连接，可能把无辜进程（含 backboardd）判成设备丢失；现在扣连接一律按 FAR 窗口，与改动前逐条相同。S1：成批结算时 `fw`/`n4` 可能偏低（文档写明，计量行加 `poll <n>`，抢占门槛只采信中断取栅栏的数据）。S2：计量行只在 status 2/3 写、60 s 窗口（10 分钟约 1.7 KB），默认 status=1 周期性日志逐字节不变。S3/S4：补测试，修复前存活的 10 个变异全部被测出。
  - `Adreno830/AGENTS.md` 的“唯一的日志是启动期追踪”放宽为也允许这条低频计量行（已认可）。
  - 重编内核中（任务 `bczax6w5m`：计量 + 日志分区修复 + 电池修复），之后重打 A/B 两包。
- **04:45 关键日志缺陷修复：GPU 状态行一直写进事件环**（计量工作流实现者发现）：`BootProgress/RamoopsLog.c` 的 `ramoops_log_rows` 写死 `emit(AREA_EVENTS, …)`，绕过了 21:3x 改好的 `area_of`（它本就规定 G 记录进控制台环）。所以每秒 4 行的状态行一直在冲 32 KiB 事件环（约 80 s 一圈），开机一次性 U 行（预装应用拷贝、叠加层、音频服务）总读不到；对下一包更致命——诊断守护进程迁移期（约 320–340 s）的 `diag:`/SAM 行到 660 s 复位时早被冲掉。已改为 `emit(area_of(kind, scratch), …)`；写入器测试两处断言按新行为更新（各区计数 boot 6/events 5/console 9；状态行刷屏只冲控制台环、事件环里的阶段记录保留），-O2/-O0 加 ASan/UBSan 通过；测试回路全套通过、19 个旧目录重解码正常。代价：控制台环（内核打印、exec/exit 行）现在和状态行共用，约 70 s 一圈。**A/B 两包要用含此修复的内核重打**（等计量工作流审查完再编，避免把未审代码带进去）。
- **05:30 M14 计量审查修复（工作流 `wqszvkai5` 的修复步骤；未提交、未上机）**：对抗性审查（结论“先修再上机”）的五条逐条对代码复核，全部成立，各带主机测试修掉：
  - M1（必须修）页故障记账：按“故障时唯一在途的连接”扣连接会冤枉无辜进程——出故障的作业在 710 上半部读栅栏之前完成（终止模式加 HUPCF 下作业照常跑完）而后面在途的是别的连接；FAR 落在内核区（SQE 无害预取，原规则谁也不扣）；内核自用客户端与槽 0 共用范围 0。上机镜像用过设计配置（`island-apps-1` 为 `pagani-gpu-profile=0`，第 0 行 f45），挡不住。改回：扣连接一律按 FAR 窗口；`NativeAdrenoFaultVerdictFor` 只给判定，发布外壳的计数本体改成头文件里的 `NativeAdrenoFaultNote`（模板，主机可测：判定计数、返回 FAR 窗口的槽，内核区为“无人”）。`test_userclient` 改为断言返回 FAR 槽、四类计数逐一正确（含未发布时）。架构 2.2 表、4.12.3、4.13.4、6.2、第 11 节与 PLAN M14 同步。
  - S1 成批结算：一遍结算看到多个完成时第一个拿走整段时间，“上界”只对 rs 与延迟成立，fw、n4、外部服务时间可能偏低（评审探针：成批 fw 0，逐个 23 ms）。文档改正；计量行加 `poll <n>`（窗口里轮询看到完成的周期数）；4.17、8.4 与 PLAN 写明第 1 阶段门槛只采信中断取栅栏的数据。`test_a830fence` 用同一组三作业把两个方向钉住。
  - S2 日志量：计量行“只写变化的”在 GPU 活跃时不起作用，默认 `status=1` 也每 10 s 一条进 32 KiB 事件环。改为只在诊断页模式（`status=2/3`，测试镜像都用 2）写、窗口 60 s。按真实格式重算（33 字节头 + 文本 + 换行，120 Hz 负载）：默认配置的周期性日志逐字节不变（计量行 0，审查前约 9 600 字节/10 分钟）；`status=2` 活跃时 10 条约 1 712 字节/10 分钟（最宽 2 140），单靠它写满事件环约 190 分钟（审查前约 34 分钟）；G 记录不变（控制台环，`status=2` 一组 379→385/388 字节，+1.6%–2.4%）；光圈注记每次开机一条约 64 字节。
  - S3 结算者复查：加仅主机测试的钩子 `a830FencePassHook`（`ADRENO830_HOST_TEST` 之外不存在，热路径不变），用例在一遍中途送来新完成并让它的“中断”撞上忙碌的结算者，断言结算者返回前已用新读的时钟算掉；两个竞争用例去掉线程收尾的补调，改由每次完成后的“中断”调用，并在最后一次 take 之前检查 `queueSeen`。
  - S4 测试缺口：补时钟早于上一遍（服务时间 0、fw 9 ms）、深度 ≥128 时 fw 未知、`fences` 两个输出的顺序与故障下半部传参（`test_start` 的 drainFaults 与页故障替身记录参数，线程调用替身能运行被 enter 的那一个）、`sw` 恰好填满第 66 字符、最大值向上取整（15401 µs → 15.5）、提交对象重新挂接后历史清空。
  - 变异复核：评审列出的存活变异 F1b、F11、F17、T3、T4、T6b、S2、S9、B5，加上新写的 F22（复查时不重读时钟），修复前全部存活，修复后全部被测出；新代码上 7 个能编译的变异（模式门槛、`poll` 计数、窗口长度、按在途连接记账、判定对调、不计数、轮询判定）全部被测出（另 3 个编译就失败）；F7（跳过时不清完成时刻）在实际时序里等价（恢复之后的作业都在跳过之后打戳），未加用例。完整主机测试（带固件）48 通过、0 失败，内核对象 51 个零告警。NativeAdrenoPublish.cpp 其余依赖 IOKit 的部分（槽位压力、每秒统计字段）仍只有内核编译检查。
- **03:40 M14 设计研究结论（`scratchpad/m14/results.txt`）：四项都“以后再做”，只先做两组小的计量**：
  - D3 零拷贝直扫：前置条件一个都没满足——13T 上显示流的 apps SMMU 状态从没读到过（唯一会读它的 `pagani-dpu-probe=3` 12:12 让整机卡死），D2 翻页阶梯一步没跑；SSPP 只有 32 位地址、XNU 内存在 4 GiB 以上，必须在 Gunyah 模拟的 apps SMMU 上开翻译型 S1（兄弟机 uke/asteroids 走通过，但“做对之后和拷贝模式差不多”）。收益小：拷贝本身 ≤0.6 ms（120 Hz 预算的 7% 以内）；对全部日志统计，26 条慢帧记录里 23 条是 `gap` 主导（合成器没交帧：应用/CPU 侧停顿、PosterBoard 崩溃、着色器现编、服务超时），只有 3 条是拷贝主导（都在 160/389 MHz 降档时）；而且 D3 必须推迟交换退役，可能更差。替代：先上 D2，插件“等已提交而非等完成”后立刻提交拷贝，打开 LLCC。
  - 按进程页表：大（P2–P7 约 2–3 周加 4–5 轮上机），先做 P0（aperture 结果上状态页与日志、槽位压力与上下文切换计数、页故障归属改进、单独验证 `pagani-gpu-va=16`），用数据决定要不要做。
  - 抢占：大（本体 2000–3000 行），先做第 0 阶段“作业归属计量”（每个渲染服务器作业前排了多少外部 GPU 时间 `fw`、超过 4 ms 的次数），可选 0b `pagani-gpu-fgcap=N`（默认 0）。
  - 页表格式（2 MiB 块、39 位按需页表）：大（4–6 个工作流日），以后。
  - 已发起工作流 `wqszvkai5`（`wf_be7e965f-bfb`）：一个实现者串行做“抢占第 0 阶段 + 按进程页表 P0”（同几个文件）、对抗性审查、修复。约束：默认行为不变、不加每秒新行（并进已有 G 记录或每 10 s/变化时）、给出日志量预算、提交/栅栏热路径只用原子量。
- **03:10 手机仍未回来；发起 M14 设计研究**（工作流 `wq0tg51e4`，`wf_1a777b73-fa6`，只读，产出 `scratchpad/m14/`）：M0–M13 代码齐（M11 LLCC 与 M13 各子项默认关、待真机，速度档 `pagani-gpu-speedbin=1` 已实现），计划里剩 M14 实验项。四个代理分别评估 D3 零拷贝直扫、按进程页表（`CP_SMMU_TABLE_UPDATE`，S3 已设 aperture）、抢占、页表格式（2 MiB 大页 + 39 位按需页表）的可行性、对“无掉帧/更多 GPU 客户端”的实际收益、实现步骤、主机与单变量真机测试、风险与工作量，结果出来后挑一两项实现（默认关）。
- **02:36 A/B 两包就绪（内核 `3a8d6515`：电池 F1–F3 修复在内，零告警，`component-PaganiBattery` PASS）**：
  - A 包 `测试镜像/boot-A830-A包-诊断-控制中心-键盘F2.img`（`d32a5412`，参数 912 字节，不开电池）：取代 01:50 的 `boot-A830-诊断守护-控制中心-键盘F2.img`（`0b468ea2`，内核 `92cbb602`，内容相同但没有电池代码）。
  - B 包 `测试镜像/boot-A830-B包-A加电池.img`（`a2955ae8`，929 字节）：A 包 + `pagani-battery=1`（无 `pagani-adsp`、无扬声器、`pagani-battery-pmic` 默认 1）。
  - **上机顺序（等用户处理好手机）**：①回安卓后后台任务 `baw9fhme8` 自动读卡死那轮的日志与 dmesg；②`gpu-loop.py --image ../../测试镜像/boot-A830-A包-诊断-控制中心-键盘F2.img --label diag-cc-f2 --android-timeout 1500 --yes`（电量门槛默认开）；③A 包正常后再跑 B 包（`--label battery`），看 `[battery] start … pmic=1`、`i2c up`、`gauge addr=0x55 rc=0`、`state published pct=…`。
- **02:20 电池/热复位工作流（`wlhbojzca`）收尾**：
  - **热复位研究：不做，假设不成立**（产出 `scratchpad/battery-wr/warm/`，资料 `warm-refs/`）。一加公开的 13T 内核（6.6.89）里 `qpnp-power-on.c` 的复位类型函数都包在 `<= 6.6.0` 条件里不编译，上游 pm8941-pwrkey 对 gen3 也不配 PS_HOLD；安卓重启根本不写 `PS_HOLD_RESET_CTL`（PON_PBS 0x852/0x853，类型 1 热/4 关机/7 硬）。复位类型由安全固件在 PSCI 里决定（TF-A 参考实现每次 SYSTEM_RESET 都设硬复位）→ XNU 写热复位多半被覆盖，写错却会关机/进 ramdump/僵住，都要人。代码（`pagani-reset-warm=0/1/2`，2 为只读干跑；身份、归属、单页写窗与命令白名单；36 个变异全被测出）与 diff 留在草稿目录，**不合进内核**；只在用户在场时按“只读核对 → 干跑 → 实写”试。研究者认为更可能的方向：安卓复位前会关 SDI（SCM `0xC2000109`）、停 SPMI 仲裁器（SCM `0xC2000901`，源码注释说 PS_HOLD 拉低时若 SPMI 事务进行中可能锁死 PMIC）、停其它核，XNU 都没做，而 PaganiInput 每 8 ms 读一次 SPMI——未实现、未验证，记为后续方向。
  - 已合入零风险的只读采集（`工具/gpu-test-loop/ramoops_device.py`）：回安卓后 `device.txt` 多出 `dmesg.pmic_pon`（pmic-pon-log 的 PON/Reset Trigger 与 Reset Type）、`pmic_info`（OPLUS 的 `/sys/pmic_info` 复位原因）、`dload`（下载模式）。对照四种复位（安卓自重启、XNU PSCI、看门狗咬合、长按）的 Reset Type 就能判断复位类型是不是掉位原因。模拟测试通过。
  - **电池修复（F1–F3 全部成立，已就地改在 `drivers/PaganiBattery`，留档 `scratchpad/battery-wr/fix/PaganiBattery-F1-F3.diff`，可 `patch -R` 撤回）**：F1 每次 I2C 传输先查 0x52008 的位 7/8/11，被别人冲掉就重投票并等分支时钟起来再碰引擎（原代码丢位后下一次传输有 7 次无时钟引擎访问，新代码 0 次），最多记 4 行；F2 失败串首行只记前 8 串，丢失/恢复行只记前 4 次，state/summary 行带 `fail=`/`lost=` 计数（模拟一天从 4321/5182 行降到 11/19 行，新用例隔轮失败一天 22 行）；F3 `pagani-battery-pmic=0/1/2` 写进 README 与头文件，start 行带 `pmic=`，主机检查新增“读取的每个启动参数都必须写进文档”。主机检查全过（i2c 7739、core 82235 项等），两种归档构建零告警。结论：可以单独无人值守跑一轮（只加 `pagani-battery=1`，不加 `pagani-adsp`/`pagani-speaker`，电量门槛保持开）。F1 的正式修法（共享锁放进 Sm8750Platform、触摸与扬声器也取锁）要改触摸路径，留到真机验证时再做。
  - 用修好的源码重编内核中（任务 `bl9p73xk8`），之后出 A/B 两包对照：A = 诊断 + 控制中心 + F2（不开电池），B = A + `pagani-battery=1`。
- **02:12 电池接线已合入**（六份 diff：`Stage7Probe.cpp`、`UniXNU/components.toml`、`board.toml`、`make-a830-layout.sh`、`make-a830-package.sh`、`test-phone-island-layout.py`；合入前用 `check/live/sha256-at-diff-time.txt` 核对过目标文件未变）。布局回归 32 项通过。真实构建验证通过：内核 `cfa6104c`（零错误零告警，`component-PaganiBattery` PASS，`libAdreno830.a` 含 7 个电池成员，`_pagani_battery_start` 为强定义、`_sm8750_gcc_vote_owner` 弱定义；未设 `pagani-battery` 时不开）；驱动源码的 F1–F3 修复由工作流第④步做，之后再编一次出电池单独测试镜像（`A830_BATTERY=1`，按约定单独一轮、带定时复位）。
- **01:57 电池工作流（`wlhbojzca`）前两项交付**：
  - 接线（`scratchpad/battery-wr/integrate/` 六份 diff，未应用）：电池目标文件追加进已强制装入的 `libAdreno830.a`（同 PaganiAdsp，不改 defconfig/.config；不要再往 `CONFIG_XNU_EXTRA_LD_ARCHIVES` 加，会重复符号）；Stage7Probe 在 ADSP 与音频启动之间无条件调 `pagani_battery_start`（在触摸 `startPeripherals` 之后）；组件登记、板级 include/archives、布局脚本 `A830_BATTERY=1` → `pagani-battery=1`（+17 字节，最坏 929/1000）、打包脚本与布局测试（32 项）。默认关已在源码与机器码两级核对（缺省只在 `pagani-adsp=1` 时开，我们的镜像都没有它）。草稿目录里链接与预链接演练成功，`kernel.macho` +96 KiB。注意：`board.toml` 不能在布局脚本运行期间打补丁（退出时会还原）。
  - 硬件审查：**可以上机**（只开电量计、不开 ADSP、不链接 PaganiSpeaker）。GCC 0x52008 位 7/8/11、RCG 0x17024、GENI 0x984000 的偏移与序列、gpio66/67 功能 1（i2chub0_se1，不在保留引脚、触摸用 48–51/161/162）、电量计唯一一次写（0x3e←0x0001，OPLUS 每次开机同样写）、SPMI 只读，均对照上游源码与 13T 运行设备树核实；所有等待有上限、无关中断循环。建议修：F1 投票锁只有电池自己用（触摸与扬声器无锁读改写，当前启动顺序下安全）；F2 电量计时好时坏时日志无上限（模拟一天 4000+ 行，会冲掉事件环）；F3 `pagani-battery-pmic`（默认 1，只读测绘）没写进参数表。第④步修复代理在热复位研究结束后开始。
- **01:56 开源发布流水线恢复全绿**（`release/make-release.sh`，不加 `--commit`，只在本地导出与检查）：自昨天 15:41（`make-shader-cache-a830.py`）与 16:44（测试回路加崩溃测试）以来没跑过，导出失败 9 处、检查失败 38 处。修：`release/scripts/export-tools-docs.sh` 按新的 `run-tests.sh` 更新改写（注释、`W`/`C` 两行改走 `PAGANI_DRIVERS`，缺测试源时整体跳过）、新增 `test_crash.py` 的默认路径与跳过、`make-shader-cache-a830.py` 的 8 处本机路径改为 `A830`/`UNIXNU`/`A830_USERSPACE_WORK`/`LLVM_BIN`；`check-release.py` 白名单加 iOS 进程 `iconservicesagent` 及其简称（36 处“agent”误报）；中英 README 的测试环境变量表补 `CRASH_TRACE_TEST`、`CRASH_A2S`、`A830_HEAT_TRACE`。第三次运行“导出与检查全部通过”（驱动主机测试 48 项、无固件 44 项、测试回路含电量门槛场景、发布目录测试前后逐字节相同）。提示项未处理：今晚新增的 XNU 补丁（`phone-*`、诊断、控制中心等）还没归入任何导出补丁，属于发布范围的决定，留待发布时一起定。
- **01:50 下一包镜像就绪，等手机恢复**：`测试镜像/boot-A830-诊断守护-控制中心-键盘F2.img`（`0b468ea2`，同内容也在 `…iPhone15Pro灵动岛-系统应用.img`）。核对：内核 `92cbb602`；参数 912 字节，含 `sheng-no-lc=1`、`pagani-reset-after=660`、`pagani-reset-wdog=1`、`pagani-wdog=30`、`sheng-staged-apps=1`、`pagani-audio=1`、`pagani-input-script=7`、`-at=300`、`pagani-kbtrace=3`；种子 `工具/seeds/phone-island-apps-audio.dbrdisk`（报告里有壁纸、键盘、控制中心 `/private/var/ngpu/controlcenterui`、诊断守护进程 CDHash `7301a805…` 与配额文件和迁移日志偏好、迁移哨兵、强制安装 28 个、键盘偏好）；板级文本修补含 F2。上机看：`diag:` 各行（launch/start/事实行/SAM 原话/`installation was cancelled for reason`）、`overlays:` 行的 controlcenter 阶段、`kbt` 按键计数、`input script:`、`audio route`。用户回来、读完卡死日志后再上机，`gpu-loop.py --android-timeout 1500`（新的电量门槛默认开）。
- **01:48 内核 `92cbb602`**（零错误零告警；含 `phone-diag-sysctl.patch`、`phone-controlcenter-settings.patch`，`strings` 核对到 `pagani_diag_line`、`diag: launch pid %d DumpPanic -> paganidiag`、`overlay controlcenter stage %d status %d` 与新的 `overlays:` 行）。打镜像中（任务 `b18zvy5eg`，日志 `scratchpad/diag-image-build.log`）：`A830_DIAG`、诊断信任包、排除 WebKit 的底种子、`A830_AUDIO=1`、`A830_RESET_AFTER=660`、`A830_EXTRA_ARGS="pagani-input-script=7 pagani-input-script-at=300 pagani-kbtrace=3"`，变体 `phone-island-apps`。
- **01:45 诊断守护进程复核完、已合入，控制中心修复已合入，内核重编中**（任务 `bj9s729wn`）：
  - 独立检查（`w83erogbk` 第二个代理）结论“修复后可上机”，就地修了两个阻断问题：①iOS 17.7 的 `+[OSLogStore storeWithScope:]` 对 scope≠1 直接返回 nil（反汇编 0x1d2f4f85c），日志探针一条统一日志都拿不到 → 新增 `system_log_store()`，回退到 LoggingSupport 的 `+[OSLogEventStore localStore]` 包成 OSLogStore；②launchd 把缓存里的系统任务一律设成启动类型 1（系统服务），AMFI 的 `LaunchTypeSystemServicePolicy` 会拒绝放在 Data 卷、只有 owner 信任的守护进程 → 布局脚本设 `A830_DIAG` 时自动加 `sheng-no-lc=1`（只对该次开机跳过启动约束；历次日志未见约束违规）。另修：探针子进程读循环出错时先 SIGKILL 再 waitpid。重建后守护进程 CDHash `7301a805…`，信任包 `scratchpad/diag/out/owner-trust-apps-diag.bundle`（`bc4016a9…`）。
  - 采纳配额建议：`scratchpad/diag/out/paganidiag.conf` = `boot_lines=60 notice_lines=15 info_lines=15`（约 16.5 KiB，不冲满 32 KiB 事件环，SAM 行与错误优先）。未解决的风险：`kern.pagani_diag_line` 写入可能被平台沙箱（`mac_system_check_sysctlbyname`）拒绝，症状是只有 `diag: launch` 和 `no line from pid N in 90 s`，退出码 4。
  - 合入：`工具/make-phone-seed.py`、`工具/make-a830-layout.sh` 打上诊断 diff（备份 `scratchpad/merge-backup/`）；设备 series 末尾加 `phone-diag-sysctl.patch`（`36b905cf…`）与 `phone-controlcenter-settings.patch`；种子脚本新增 `--controlcenter DIR`（拷到 `/private/var/ngpu/controlcenterui`，回读比对），布局脚本在 `工具/phone-controlcenter-overlay/` 存在时自动传入（`A830_CONTROLCENTER=0` 关）。布局回归 31 项通过；两补丁叠加后真实编译 0 错误、57 条告警（多出的 4 条是诊断补丁里 XNU 启动宏自带的）。
- **01:35 测试回路加电量门槛**（`工具/gpu-test-loop`）：手机这次在 USB 上完全消失，除了卡死，也可能是电池被整夜测试耗空后欠压关机（XNU 下没有 DSP 的充电管理、亮屏满负荷，回安卓的间隙又短；前几轮没记电量，无法确认）。`gpu-loop.py` 每轮重启前在安卓下读 `dumpsys battery`（只读）：低于 `--min-battery`（默认 40 %）就留在安卓充到 `--charge-to`（默认 60 %），超过 `--charge-timeout`（默认 3 小时）仍不够就以状态 5 停下、不重启；回安卓后打印并记录前后电量（`device.txt` 的 `battery`、`decode.json` 的 `timings.battery_before/after`）。模拟测试新增两个场景（30 % 先充到 65 % 再跑；一直 20 % 时重启前停下），`run-tests.sh` 全部通过，19 个已存目录重解码正常；README 排查一节同步。
- **01:15 电量不准的直接原因：`PaganiBattery` 从未接进内核**（不在 `components.toml`/`board.toml`，Stage7Probe 也没有 `pagani_battery_start` 钩子；驱动 16:40 就写好了：I2C 读 NFG8011B 电量计 + ADSP 电池管理器，带“没有电量前不发布、0 % 不报”的防护）。发起工作流 `wlhbojzca`（`wf_7b5adda3-aa3`，产出在 `scratchpad/battery-wr/`，只出 diff、不改正在合并的文件、不碰手机）：①接线 diff（Stage7Probe 钩子、组件登记、板级、defconfig、布局脚本 `A830_BATTERY=1` → `pagani-battery=1`）；②电池驱动硬件写操作的对抗性审查（GCC 0x52008 投票、RCG 0x17024、GENI 0x984000、gpio66/67）；③“复位前经 SPMI 把 PMIC PS_HOLD 复位类型设成热复位”的事实研究、设计、带模型的主机测试与风险评估（不在用户不在时上机）；④按审查结论修电池驱动。
- **01:01 F2 第二轮：手机没回来**（任务 `b3fhxuazk`，日志 `scratchpad/kbd-f2-run2.log`）：同一镜像，上一轮 497 s 准时复位；这轮 1500 s 内 USB 上始终没有任何设备（Mac 三个 USB 控制器都空）——卡在 XNU（定时复位与 Gunyah 看门狗都没把它拉回来）或已关机。01:25 已推送用户：先看屏幕、插拔数据线，只有停在 XNU 画面才长按电源+音量下。**上机暂停**，等用户处理。后台任务 `baw9fhme8` 在手机一回安卓就读日志（`read-ramoops.py --label kbd-f2-input-r2-1`，长按复位 DRAM 零翻转，应能读到卡死前的完整记录），并只读抓 dmesg 的 PON/复位行到 `scratchpad/pon-dmesg.txt`。
- **01:10 控制中心“样式抽风”根因与修复（完美化 #6，源码完成，未编进内核、未上机）**：
  - 根因：`-[CCUIModuleSettingsManager _loadSettings]`（ControlCenterUI 0x1cf19c04c）用 `[[NSBundle bundleForClass:] URLForResource:@"DefaultModuleSettings" withExtension:@"plist"]` 读模块尺寸，NSBundle 按设备类型取 `~iphone`/`~ipad` 变体；iPad 根卷只有 `DefaultModuleSettings~ipad.plist`，iPhone 身份下 URL 为空 → 设置为空 → 每个模块都按 `_CCUIMakeLayoutSizeFromDictionary` 的默认 1×1 排：连接、正在播放、亮度/音量滑块、专注模式都被挤成小方块。默认模块顺序（`+[CCSModuleSettingsProvider _defaultEnabledModuleOrder]`）是代码按设备类型和 MobileGestalt 构造的，不缺文件。
  - 修复（照键盘布局叠加的做法）：`工具/make-phone-controlcenter-overlay.py OS_ROOT OUT`（从系统卷的 `~ipad` 版生成 `DefaultModuleSettings~iphone.plist`，9 个模块尺寸，4 列网格；输出 `工具/phone-controlcenter-overlay/`，苹果派生数据，不进开源仓库）；内核补丁 `scratchpad/ccpatch/phone-controlcenter-settings.patch`（`native_gpu_addon_mount` 把 `/private/var/ngpu/controlcenterui` 只读叠到 `ControlCenterUI.framework`，复用暂存页 0x800..0x921（早先壁纸挂载用过、避开 launchd 环境表 0x400..0x6ff）；U 行 `overlay controlcenter stage %d status %d`；开机事实的 `overlays:` 行加 controlcenter）。`git apply --check` 通过；用 `kern_exec.o.json` 真实编译补丁前后都是 0 错误、53 条告警。种子选项 `--controlcenter DIR` 等诊断工作流的种子 diff 合入后再加（两者改同一脚本）。
  - 若要和真 iPhone 完全一致，可经用户同意从 iOS 17.7 iPhone 固件取 `DefaultModuleSettings~iphone.plist`（同取键盘布局的办法）；当前用 iPad 尺寸，网格相同。
  - 01:36 系统性扫描（系统卷里所有 `~ipad` 的 plist/nib/storyboard/ca/strings，找既无 `~iphone` 也无无后缀版本的）：plist 只有 7 个，缺 iPhone 版的是壁纸三项（`Collections` 已由壁纸叠加层兜底；`DefaultWallpapers` 缺失时 WallpaperKit 用内置默认 ID，叠加层已提供；`Wallpapers~ipad.plist` 指向根卷里不存在的 `Stills/iPad`，遗留文件）、控制中心（本修复）、`Widgets.framework/DefaultTodayViewArchive~ipad.plist`（旧式“今日”扩展的默认列表，不是 iOS 17 小组件变黑的原因，小组件仍取决于预装应用装上与 Metal）和监管图片映射（无关）；界面资源里只有“显示缩放”预览的 20 个 `~ipad.ca`（只影响设置里那一页的预览）、Setup.app（不运行）、CoreTelephony 本地化字符串。结论：iPhone 身份下影响全局的资源缺失只有控制中心这一处。
- **00:55 F2 第一轮读回：两区全丢**（`runs/20261031-005455-kbd-f2-input-1`）：Linux 在 fastboot boot 后 497 s 准时开机（看门狗咬合复位，XNU 活到定时复位、无 panic），但 `found existing invalid buffer, size 18545620`（pmsg 头 0x880040008 第 24 位 0→1 的固定弱单元）且 console 区签名也坏，一条记录都没有。手机无 `/proc/kcore`、`/dev/mem`（`CONFIG_PROC_KCORE`/`CONFIG_DEVMEM` 均未开），无法绕过 pstore 直接读物理内存。00:59 同一镜像重跑（任务 `b3fhxuazk`，日志 `scratchpad/kbd-f2-run2.log`）。
  - 推测：安卓自己重启前由 `qcom_pon` 把 PMIC 的 PS_HOLD 复位类型设成热复位（DDR 自刷新，pstore 保得住）；XNU 下无人设置，PSCI/看门狗复位可能走硬复位，DDR 若干秒无刷新而掉位。待下一次回安卓看 `pmic_pon_log` 打的复位类型确认，再决定是否做“复位前经 SPMI 设热复位”（要新增 SPMI 写，风险是设错会关机、需人工开机）。
- **00:56 诊断守护进程交付**（工作流 `w83erogbk` 实现者；独立检查进行中）：`UniXNU-Pagani-Drivers/drivers/PaganiDiag`，内核补丁 `scratchpad/diag/phone-diag-sysctl.patch`（series 末尾），种子/布局脚本 diff，信任包 `scratchpad/diag/out/owner-trust-apps-diag.bundle`（守护进程 CDHash `8736de04…`）。两点更正：
  - 21H461 的 launchd 正常开机只从签名缓存 `/System/Library/xpc/launchd.plist` 读 `/System/Library` 下的任务，叠加 `LaunchDaemons` 既加不了也换不了任务——**`adrenoprobed` 从未运行过**。新做法：内核在 `exec_activate_image` 里把 xpcproxy/launchd 第一次 exec `/usr/libexec/DumpPanic` 换成 `/private/var/ngpu/bin/paganidiag`（文件不存在时行为不变；`pagani-diag=0` 关）。守护进程经只写 sysctl `kern.pagani_diag_line` 写 U 行 `diag: …`（每次开机 ≤300 行、10 s ≤20 行）。
  - SystemAppMigrator 的 `_DMLogFunc` 全记成 info 级，`com.apple.migration` 默认不保留 info：种子写 `/private/var/preferences/Logging/Subsystems/com.apple.migration.plist`（Enable/Persist=Info）。
- **00:55 预装应用：排除“判不装”，问题在“装不上”**（静态核对 + 上一轮残片解码，未上机）：
  - 29 个待装应用的 `Info.plist`（`scratchpad/sbkb/osmnt/private/var/staged_system_apps`）：28 个 `UIDeviceFamily=[1,2]`、只要求 `arm64`（测距仪另要 `arkit`），只有照相亭是 `[2]`。
  - `arm64` 能力：MobileGestalt `sub_1A02153E8` = `NXFindBestFatArch(首选用户架构, {arm64, ALL})`；Mac 上同一函数对主机子类型 0/1/2/arm64e 全部返回“有”（`scratchpad/mg/fatarch.c`），内核报 `subtype 0x1` 不影响。
  - 设备族：`DeviceClassNumber` 按 `hw.product` 前缀 `iPhone` 得 1，`SupportedDeviceFamilies=[1]`，与 `[1,2]` 兼容。
  - 00:39 那轮 `app state 7–9` 残片按 bplist 解：偏移表在 0x282，字典 59 字节 = 28 个条目，28 个键（包名）共用一个值对象 `10 01` = **整数 1（安装）**。即强制安装名单已生效、期望状态是“装”，迁移器仍把 29 个从待装目录删掉（只见删除、没有改名）→ 失败点在之后的 IX 协调器 / installcoordinationd 安装环节（之前已知 installcoordinationd 一个都没搬）。`datamigrator` 每轮都在日志约 324 s 才启动、335 s 退出。
  - 迁移器逆向（`scratchpad/sysapps/sentinel.c`、`installorupdate.c`、`trustcache.c`）：`attemptToInstallSystemAppWithBundleID:…` 对期望“安装”的应用调 `installOrUpdateAppWithBundleID:appURL:isInstalled:`（`IXInitiatingAppInstallCoordinator` + `IXPromisedTransferToPath(待装副本)` + `setComplete`），**之后不论成败都 `removeItemAtURL` 删待装副本**——所以“只见删除”是正常流程，真正要看的是协调器的完成/取消回调（`%@ %@installation was cancelled for reason %@`）。装完后的校验轮：装上了但 `MISValidateSignature(TrustCacheOnly)` 不过 → `Installed version of %@ does not match the version in the trust cache. Demoting it`（降级占位符 + App Store 下载）；没装上 → `should be installed, but is actually NOT installed` 同样降级。苹果自带开关：`installd` 目录下存在 `SystemAppMigratorIgnoreAppsNotInTrustCache` 文件时跳过信任缓存校验（备用）。
  - 下一步不变：等诊断守护进程（`w83erogbk`）读出这几个进程的统一日志错误原文再修。
- **00:46 键盘修复 F2 上机**（任务 `bs0n87l29`，日志 `scratchpad/kbd-f2-run.log`，内核 `c8cb7d79`，镜像 `测试镜像/boot-A830-键盘F2-点屏.img`，`dc73e29b`）：
  - F2（keyboard-405 报告第 6 节）：`_UIKeyboardGetDerivedKeyboardForSpecificOrientation`（UIKitCore 0x18a2af038，`.03` 文件偏移 0x3503038）只认识几种手机屏幕尺寸，405.333×880 推导不出键盘，键盘视图的框架与 405 点宽的按键对不上，触摸落不到按键层。F2 把其中“360×780”那项判断改成“高 == 880.0”写进同一标志 `w21` 再跳过原 `and`，我们的屏幕走 390 点宽手机的路径。主会话反汇编核对了前后文；布局测试新增 `test_keyboard_frame_fixup`（原字节、被跳过的 `and w21,w8,w9`、0x18b33c218 处常量 880.0），31/31。布局脚本：iPhone 变体默认加进 `/chosen/text-fixups`（共 7 条），`A830_KEYBOARD_F2=0` 关。
  - 参数 `pagani-input-script=7 pagani-input-script-at=300 pagani-kbtrace=3 pagani-reset-after=420`（脚本约在日志 310 s 开跑、355 s 跑完，复位前留约 65 s 余量）。
- **00:30 上机合并测试**（任务 `bjd39dedj`，日志 `scratchpad/combined-run.log`，镜像 `测试镜像/boot-A830-合并测试-点屏键盘音量.img`，`fd955720`）：底种子排除 WebKit、强制安装名单、键盘偏好修复、音频（新插件）；参数 `pagani-input-script=7 pagani-input-script-at=330 pagani-kbtrace=3 pagani-reset-after=480`，命令行 898 字节（灵动岛修补已进 `/chosen/text-fixups`，共 6 条）。看：`input script:` 各步、Spotlight 进程的 `kbt` 计数（触摸是否到达键盘、按下/抬起/出字是否完成）、音量键后的 `audio route` 行（`vcsp`/`vc`）、控制中心下拉、预装应用状态、GPU 光圈注记 `4 805`。
- **00:02 16 GiB 光圈冒烟测试读回**（`runs/20261031-000053-gpu-va16-smoke-1`）：Linux 在 fastboot boot 后 677 s 准时开机（600 s 定时复位，与前几轮一致），说明 XNU 一直活到定时复位、没有 panic；但这次看门狗咬合复位后**两个区都被 Linux 丢弃**（前三次咬合复位都保住了 console 区），一条记录都没有，GPU/桌面是否正常无从判断。下一包合并测试先不开 `pagani-gpu-va=16`，以后单独再测。
  - 日志通道现状：长按复位零翻转；PSCI 复位两区必丢；看门狗咬合复位约 23% 掉位、约 3/4 的机会保住 console 区。可靠的长期办法是项目自带的 UFS 日志窗口（`unixnu` 分区末尾 32 MiB，安卓下 adb 只读取回），但要往手机存储写，按 18:08 授权的约定需用户同意，待用户回来决定。
- **23:55 点屏脚本合入、命令行腾空间**：
  - 工作流 `wxbxo2y60` 的 `input-script` 交付 `scratchpad/inputscript/pagani-input-script.diff`，已打进 `UniXNU-Pagani-Drivers/drivers/PaganiInput`（`InputScript.h` 引擎 + 约 60 行胶水；原目录备份 `scratchpad/inputscript/before/`）。`pagani-input-script=<mask>`（bit0 键盘：唤醒→Home→底边上滑解锁→Home→下拉 Spotlight→Shift、a×3、123、ABC、地球键→上滑关闭；bit1 音量加减各 3 次；bit2 右上角下拉控制中心、3 s 后关闭），`pagani-input-script-at=<s>`（默认 240，系统应用变体建议 330）；真手指、真按键、HID 未就绪都会中止；不设参数时真实触摸只多一次布尔判断。键位按 `iPhone-PortraitChoco405` 计算（a=(121,2019) px 等）。外设主机测试两行 PASS（含 input script timeline/keys/aborts/default off）。
  - 命令行 930 + 点屏脚本两项 + 键盘探针 = 996 字节（上限 1000），再加 `pagani-gpu-va=16` 就超。灵动岛状态栏修补从 `text-fixup0` 启动参数挪进 board.toml `[payload] text_fixups`（`/chosen/text-fixups`，内核上限 32 条，现 6 条：OS 配置 4 条 + Core Image + 灵动岛），腾出 76 字节；布局测试同步，30/30。
  - 下一包合入清单：GPU 审查修复（`whnl4unn2`）、音频修复、诊断守护进程（`w83erogbk`：内核 sysctl 补丁、`make-phone-seed.py --diag-daemon`、信任包）、点屏脚本、键盘探针修订版；参数 `pagani-input-script=7 pagani-input-script-at=330 pagani-kbtrace=3`（+ `pagani-gpu-va=16` 视冒烟结果）；底种子用排除 WebKit 的那份。
- **23:50 16 GiB 光圈冒烟测试上机**（任务 `bzp1f2y4u`，镜像 `测试镜像/boot-A830-冒烟-16G光圈.img`，`0b3b6a8e`，内核 `e623df2e` + `pagani-gpu-va=16` + 排除 WebKit 的名单，无键盘探针）：只看 GPU 在 T0SZ=30、32 MiB 连续页表下能否正常起来、桌面是否存活。
- **23:51 音频探针审查四项已修（工作流 `whnl4unn2` 的音频项，只改 `drivers/PaganiAudio`，未提交）**：①`pagani-audio-log=2` 只在前 16 轮允许 10 s 一轮，之后按 1 级的 10 min（每轮在 32 KiB 事件环里占 500 多字节：10 分钟诊断约 8.2 KB，改前约 30 KB、几乎冲满整环），诊断时可一直开着；②探针 seqlock 补两道屏障：插件奇数 seq 改 relaxed 写 + `dmb ish` 再写字段、偶数 seq release 写，内核读完字段后 `dmb ishld` 再读 seq；③`audio route` 行在 `vcsp=值/状态` 后加 `vc=`：按 21H461 反汇编核实的 `vaemCurrentRouteHasVolumeControl`（值预置 1，返回 `(status | value) != 0`），读失败也算有音量控件，去重与限额不变，README 解读同步；④ABI 注释 `ours_running` 改为 `'gone'`。主机检查全过（胶水 740 项，原 679；新增屏障检查，核对 -O2 主机编译、iOS 插件包、内核归档三处的指令顺序，旧插件包和旧内核对象都被判失败），8 个变异全部被抓到；内核归档 3 对象零告警、244 个内核导入不变。插件新 CDHash `c88be4108de3c44d3b1fca16e856b33c02961572`（23:48 出的 `phone-island-apps-audio` 种子已是这一版）；`add-pagani-audio-data.py` 实跑通过，Data 卷 UUID 不变。**内核侧（读者屏障、限额、`vc=`）要重编内核才生效**。差异与报告：`scratchpad/audiofix/`。
- **2026-10-31 00:08 GPU 驱动审查八项已修（工作流 `whnl4unn2` 的 GPU 项，只改 `Adreno830`，未提交，未上机）**：①挂死归责只认客户端作业能造成的原因：恢复闸门把故障原因带给编排器（`A830RecoveryOps::gate` 加 cause），`A830FaultBlamesClient` 只对 Hang/CpHwError/Watchdog 为真，`native_adreno_recovery_begin(charge)` 照样停环、按范围记故障，GMU、HFI、WPTR 围栏写、slumber/唤醒、自检不再记到碰巧在途的进程头上（`native_adreno_hang_culprits[4]` 计数）；`NativeAdrenoSubmit::halt` 只选已完成栅栏之后仍在途的最近作业，毒化后的已完成作业和 fence 0 的毒化提交不再被选，没有就是 UINT32_MAX；②选槽名单内优先：mount 把 `listed`（`NativeAdrenoSandboxGrantList`）传给 `NativeAdrenoClientSlotFor`，名单外的进程只在空闲小槽多于 3 个（4 GiB 表）/4 个（16 GiB 表）时取小槽、从不占大槽，名单内的小类进程小槽满了可借大槽，2 GiB 表只给名单内进程；③容量与保留额度按渲染服务器身份（`NativeAdrenoClientTermsFor`），落在大槽的重启 backboardd 也是 16384、不带保留，邮箱仍只给槽 0；④池外 BO 的分配（连续尝试、零散回退）、写合并与释放移出全局 Memory 锁（`NativeAdrenoMemory::allocatePooled` 只做池内一步，池外统计改原子加，池内分配与导入仍持锁）；⑤16 GiB 判据改读 IDR2.UBS（与套件编码器一致，SM8750 为 48）；⑥64 行进程名记录表满后接管无活连接、无挂死、打开最少的一行，挂死不再漏记漏拒，newUserClient 每次持锁重新查行；⑦S3 建好 GPU 上下文后写追踪注记 `gpu aperture GiB, table pages`（两种配置都有，16 GiB 为 `10 2011`）；⑧ARCHITECTURE 3.6/4.5/4.6.1/5.4/6.2/6.4/8.7 与头文件注释。每项都有先失败后通过的主机测试（test_submit、test_a830recovery、test_start、test_userclient、test_a830smmu、test_a830trace；修复前日志在 `scratchpad/gpufix/evidence/`）；全量主机测试 48 通过、0 失败，内核对象 51 个零告警。差异：`scratchpad/gpufix/adreno830-review-fixes.diff`。
- **23:28 上机强制安装包**（任务 `bfcjopfm1`，日志 `scratchpad/apps-force-run.log`）：内核 `e623df2e`（23:23 那次失败：`PEGetProductName` 未声明——在 `IOKit/IOPlatformExpert.h`，补丁里显式声明；顺带查明我的编译检查两次漏报的原因：借用 `kdebug.o.json` 的命令，而 `kern_exec.c` 自己的编译记录多了 `-D XNU_OWNER_DATA_MOUNT=1` 等宏，预装应用这整段代码在借用的命令下根本没编译。`scratchpad/real-compile.py` 已改为优先用文件自己的 `<name>.o.json`。另：编译失败后 XNU 目标文件残缺，需删 `startup.o.json` 强制引导编译）。镜像 `测试镜像/boot-A830-诊断-预装应用强制安装.img`（`3edbf63e`）：强制安装名单 28 个、`SkipDeviceFamilyCheck`、键盘偏好与探针、`pagani-reset-after=600`，去掉 `pagani-audio-log=2`；核对名单文件 501:20 0644、目录 root 755。同时启动工作流 `whnl4unn2`（`wf_32ab67dd-b6e`）：按审查修 GPU 驱动（挂死只对 Hang/CpHwError/Watchdog 归责、`halt()` 选作业、名单进程优先的选槽、backboardd 按身份给容量、池外分配移出内存锁、IAS→UBS、记录表淘汰、光圈追踪、文档）和音频探针（log=2 限 16 轮、seqlock 屏障、vcsp 按 MediaExperience 语义、注释），各带独立复核。
- **23:07 诊断包读回**（`runs/20261030-230713-apps-metal-diag-1`，看门狗咬合复位，console 区 22.4% 掉位，592 条 339 条校验）——**预装应用的真正断点找到了**：
  - 开机拷贝完全成功：`stage 2 dirs 6574 files 4214 bytes 466987635 failed 0 error 0 ms 2655`（445 MiB 只用 2.7 s）。
  - 迁移跑了：`migrator 337/5`（datamigrator pid 337 五次被告知“非临时存储”），哨兵被删（`sentinel 0 … (migration over)`）。
  - 但 `app moves: D S>- com.apple.migrat 29`：29 个 `.app` 全部被 datamigrator 自己删掉，没有一次改名进容器；`/private/var/containers/Bundle/Application` 不存在。
  - 对照 SystemAppMigrator 反编译：每个应用以“期望状态” 2/6 进入 `attemptToInstallSystemAppWithBundleID:…`，走“Skipping installation of %@ because it is already uninstalled”，随后 `removeItemAtURL` 删掉待装包。期望状态来自 installd（种子里没有任何 `SystemAppInstallState.plist`，开机时为空），只可能是迁移里的几道检查调用 `markHiddenAppAsUninstalled:`（置 2）：管理配置的移除名单（种子里没有任何限制，排除）、`handleAppsMissingRequiredCapabilities`（`MobileInstallationCheckCapabilitiesMatch`，29 个都只要求 `arm64`）、`handleAppsNotCompatibleWithDeviceFamily`（`MIIsApplicableToCurrentDeviceFamilyWithError`，29 个都是 `[1,2]`，Photo Booth 是 `[2]`）。
  - MobileGestalt 逆向（`scratchpad/mg/`）：`SupportedDeviceFamilies` 由 `DeviceClassNumber` 决定（1/2→`[1]`，3/9→`[1,2]`，其他→空数组），`DeviceClassNumber` 取 `sysctl hw.product`（XNU `PEGetProductName` = 设备树根 `model`，灵动岛变体是 `iPhone16,1`）的前缀；`arm64` 是 `host_info(HOST_PREFERRED_USER_ARCH)`（主核 `cpu_type/cpu_subtype`）与 arm64 的架构比较。纸面上两道都应通过，静态分析定不了是哪道判掉的，下一轮探针给出 `hw.product` 与 CPU 类型、以及 installd 状态文件原文。
  - 其他：三个叠加层都挂上了（`overlays: wallpaper stage 5 status 0, keyboard stage 5 status 0, audio stage 5 status 0`）；音频路由行没能从掉位里解出来；GPU 只有 backboardd 一个客户端（没人用手机）。
- **23:25 预装应用的修复（数据，不碰二进制）**：
  - SystemAppMigrator 的 `init` 无条件读取 `/private/var/preferences/com.apple.demo.SADForceInstall.plist`（字符串数组，零售演示机的强制安装名单），`handleForceInstalledApps` 在上述各道检查**之后**把名单里的每个可删除系统应用设为状态 1（安装）。`make-phone-seed.py --force-install-apps`（布局脚本系统应用变体默认开，`A830_FORCE_INSTALL=0` 关）写入 28 个包 ID（去掉 iPad 专属的 Photo Booth），并在 mobile 偏好 `com.apple.mobile.installation` 写 `SkipDeviceFamilyCheck=true`（`-[MIDaemonConfiguration init]` 读取，免设备类型检查）。
  - `phone-boot-facts.patch` 更新：`sais` 改查真实路径 `/private/var/installd/Library/MobileInstallation/BackedUpState/SystemAppInstallState.plist`；迁移结束 30 s 后（及之后每 240 s）把这份状态文件十六进制写进日志（`app state N: …`）；每 30 s 多一行 `identity: hw.product … cpu type … subtype …`。真实编译命令检查零错误。
- **23:20 审查工作流 `wxbxo2y60` 先到两份**：音频探针可上机，但 `pagani-audio-log=2` 会在音频 IO 3–4 分钟后冲满事件环（下一轮去掉），读写共享页协议缺两道内存屏障（次要）；GPU 驱动可上机但要修：①挂死归责不分恢复原因、`halt()` 可能选中已完成或 fence 为 0 的作业→无辜进程记满 2 次后整轮打不开设备；②4 GiB 下只有 8 个小槽，名单里的壁纸海报扩展要和所有 App 抢、又不能借大槽→壁纸可能变黑；另有 backboardd 不在槽 0 时被当普通大类、池外分配在全局内存锁内进行（backboardd 掉帧）、记录表 64 行、IAS/UBS 字段、文档等次要项。
- **22:55 上机诊断包**（任务 `bm3shwbo4`，日志 `scratchpad/apps-metal-run.log`）：
  - 内核 `53ae268e`（第三次编译成功；第二次失败另一原因：我的 `phone-boot-facts.patch` 用了内核没声明的 `strstr`，改 `strnstr`。教训：`-fsyntax-only` 检查不可靠，改用 `scratchpad/real-compile.py` 拿构建记录 `kdebug.o.json` 的真实 clang 命令编译补丁后的文件）。字符串核对：`staged apps watch`、`app moves:`、`overlays: wallpaper`、`told storage is not ephemeral`、`kbt `、`audio route #`、`NativeSandboxGrantAll`、`pagani-gpu-va`、`pagani-kbtrace` 都在。
  - 镜像 `测试镜像/boot-A830-诊断-预装应用与全App-Metal.img`（`5f1a79a2`）：底种子 `data-a830-phone-cache-noprime-apps`（插件规则 `@apps @extensions !SpringBoard`）、信任包超集、音频（探针版插件 `decef4d8`）、键盘偏好两条、迁移哨兵；设备树 `/chosen/text-fixups` 5 条（OS 配置 4 条 + Core Image）；启动参数 949 字节，含 `pagani-reset-after=600 pagani-reset-wdog=1 pagani-kbtrace=1 pagani-audio-log=2 sheng-staged-apps=1 pagani-audio=1`，**不含** `pagani-gpu-va=16`（16 GiB 光圈下一包单测）。
- **22:40 工作流 `wkyv0qzt1` 完成并合入**（报告 `next2/{metal-for-apps,keyboard-touch,volume-hud}-report.md`）：
  - `metal-for-apps`：①Core Image 退路——text-fixup 把 CoreImage `_wrapGLIsUsable`（`dyld_shared_cache_arm64.07` +0x2e2cf98）改成返回 0，拿不到 Metal 的进程 `CIContext` 返回 nil，不再进 `CI::GLContext::init` 跳空指针（MobileSMS 那 5 次崩溃）；经 board.toml `[payload] text_fixups` 下发（布局脚本连同 OS 配置原有 4 条一起写入，`A830_CI_GL_FIXUP=0` 关）。②插件客户端名单改成规则：原 17 个名字 + `@apps @extensions !SpringBoard`（新种子 `next2/metal-for-apps/pipeline/out/data-a830-phone-cache-noprime-apps.dbrdisk`，信任包超集 `owner-trust-apps.bundle`）。③内核：槽位 12→32，`pagani-gpu-va=16` 开 16 GiB 光圈（3 大 + 28 小窗口，不满足条件自动回退 4 GiB）；小类进程只取小槽；按类给对象上限；给 backboardd 留 VM 映射、页表区段与 1/4 缓冲池；挂死按卡住的作业归责，同名挂死 2 次拒绝再打开。④XNU 补丁 `zzwc-native-adreno-sandbox-grant-all.patch`：加速器带 `NativeSandboxGrantAll` 时所有进程都能免沙箱打开（否则 App 根本打不开用户客户端）。主机测试 48/48、布局测试 30/30。
  - `keyboard-touch`：静态分析没能唯一锁定，剩三个嫌疑（滑行键入状态、kbd 复判不回、出字环节）。交付只读探针 `phone-keyboard-trace.patch`（`pagani-kbtrace=1`：UIKit 键盘 kdebug 埋点变系统调用，按进程计数写 `kbt` U 行）和偏好修复工具 `keyboard-prefs.py`。合入时补丁编译失败（`thread_call_data_t`/`thread_call_setup` 是 `MACH_KERNEL_PRIVATE`，bsd 看不到），已改为 `thread_call_allocate`（返回空时下次再试），测试桩同步，5 项主机测试全过。种子默认写入两条偏好修复（`make-phone-seed.py --keyboard-prefs`，布局脚本 iPhone 变体默认开，`A830_KEYBOARD_PREFS=0` 关）：`KeyboardDisableHitTesting=true`、`KeyboardContinuousPathEnabled=false`。
  - `volume-hud`：门槛在 SpringBoard `_isVolumeChangeAllowedForState`（当前路由要有音量控件，来自 VirtualAudio 默认虚拟设备的 `'vcsp'`），没找到身份相关分支。交付只读路由探针（插件在 audiomxd 里于 20/60/150/400 s 查 HAL 与路由层，内核写 `audio route` U 行，`pagani-audio-probe=0` 关），本轮加 `pagani-audio-log=2`。
  - 22:44 第一次编内核失败（上面的 `thread_call_data_t`），XNU 目标文件残缺；22:48 第二次因缓存跳过 XNU 编译、归档步骤找不到目标文件失败；22:5x 移走 `startup.o.json` 强制完整重编。
  - 同时发起工作流 `wxbxo2y60`（`wf_3ccab1c4-0c0`）：四个只读审查（GPU 驱动大改、三个 XNU 补丁、音频探针、Metal 插件与 Core Image 修补）+ 一个实现者在副本里做触摸驱动“输入脚本注入”（`pagani-input-script`：自动滑动解锁、下拉 Spotlight、点键盘、按音量键、开关控制中心），交补丁，下一轮起键盘/音量也能全自动测。
- **22:04 看门狗咬合复位那轮读回**（`runs/20261030-220442-wdog-reset-1`，镜像 `a2f482c2`）：
  - 复位链：G5 无误咬（`wdog ok … failed 0 … withheld 0`，CPU0 心跳间隔 ≤ 1.6 s）；420 s 停喂狗、4 s 后咬合，Linux 在 fastboot boot 后 497 s 开机（准时，但比 PSCI 复位多约 18 s）。
  - 日志：Linux 保住了 console 区（4 份副本），pmsg 区照旧因头部掉位被丢；但 console 区数据 1→0 翻转 23.9%（PSCI 复位只是头部几个弱单元掉位、两区被丢；长按复位零翻转）。508 条记录 219 条校验通过。结论：咬合复位让 DRAM 长时间无刷新，数据比 PSCI 差、但至少留下一个区，所以自动测试仍用咬合复位。
  - 预装应用的一次性 U 行（拷贝结果、叠加挂载、音频）又被事件环冲掉（幸存记录只覆盖约 140–420 s）。能看到的：`datamigrator` 在 103 s、325 s、368 s、401 s 各被拉起一次，413 s 以状态 0 正常退出——是客户端反复询问“要不要迁移”，不是崩溃。
  - 核对：上机用的种子 `phone-island-apps-audio.dbrdisk` 里有哨兵 `/private/var/datamigrator/migrate`，没有 `.sheng-staged-apps` 和 `staged_system_apps`，种子没问题。
  - 推断（待证）：21:24 那轮监视线程在 361 s 前就因“哨兵没了、待装目录没了”退出。若迁移真跑了而安装全部失败，SystemAppMigrator 会删掉没装上的应用，同样表现为“待装目录没了、桌面没应用”；也可能是开机拷贝本身失败。两者要靠下一轮日志区分。
- **22:20 新诊断补丁 `phone-boot-facts.patch`**（series 末尾；`git apply --check` 通过，内核编译参数下语法检查零错误、警告数与补丁前相同）：
  - 监视线程不再在“迁移结束”时退出，持续 15 分钟，每 30 s（及有变化时）重写：拷贝结果、三个叠加层（壁纸/键盘/音频）的挂载状态、哨兵/待装/已装 bundle/安装协调待办/拷贝标记/installd 的 `SystemAppInstallState.plist`、最后一个被告知“非临时存储”的 datamigrator pid 与次数、Data 剩余空间。
  - 新增文件操作监听（kauth fileop，只在拷贝后到监视结束期间）：统计每个 `.app` 包的改名与删除，按“操作、起点、终点、进程”计数（S 待装目录、B bundle 容器、I installd、C installcoordinationd、T 临时目录、O 其他），前 12 次逐条记录，计数随每条监视行重写。这样能直接看出应用是被搬进 bundle 容器（装上了）还是被删掉、被谁删。
  - 待编：后台工作流 `wkyv0qzt1` 的 GPU 子代理正在大改 Adreno830（32 槽位、16 GiB 地址空间），现在编会混进半成品；等它收尾、审查后一起编，下一轮用 `A830_RESET_AFTER=600`。
- **22:00 工作流 `wgsqawgg0` 完成**（报告 `hard/`）：
  - `hardening`：G4（观察者第一次在非主定时器核上运行时装第二个本地复位定时器，截止 + 3 s）、G5（CPU0 每秒本地心跳；心跳与观察者都在 3 s 内活动才喂狗，CPU0 卡死约 33 s 后咬）已在工作树（`RamoopsLog.c/.h`、`GunyahWdog.c/.h` 与四个测试，-O2/-O0 8/8 通过），已编进内核 `c7044c5b`。风险：开机 IOKit 期间 CPU0 若长时间关中断，`withheld` 记录会暴露。
  - `screen-off-review`：可合入（默认 0 时四个模块零行为变化，双胞胎测试）；修了 `A830PowerState::tick` 的 `now-from` 无符号回绕（GPU 刚有活动就立即 slumber）。首测顺序：`pagani-screenoff=1` → `=3` → `=5`（面板睡眠单独测）→ `=11`，每轮带看门狗、不与 `pagani-idle` 同轮。
  - `storage`：安卓 /data（f2fs）锁定文件做 iOS 持久 Data 可行（AOSP libfiemap/DSU 同法）：128 GiB 文件 → Data 约 127.7 GiB；描述符放 unixnu 分区 A/B 槽，每 2 MiB 段首块为金丝雀；需 NativeUfs 去掉 `NATIVE_UFS_READ_ONLY` 并加写闸门；上机 4–6 次。设计与主机原型、两个未应用补丁见 `hard/storage.md`。
- **21:52 看门狗复位**：预装应用诊断包那轮按 600 s 定时由 PSCI 复位，日志照例两区全丢；用户长按复位那轮零翻转，12:12 看门狗复位那轮也读得出日志。用户要求“别让我手动弄”。新增 `pagani-reset-wdog=1`（`RamoopsLog.c` 的 `reset_now` + `GunyahWdog.c` 的 `gunyah_wdog_bite_reset`）：定时/观察者复位改为停喂狗、设最短 4 s 咬合，由虚拟机管理程序复位；10 s 内没咬才回退 PSCI。`Test/gunyah-wdog.c` 新增 `test_bite_reset`（-O2/-O0 通过）。内核 `c7044c5b`（零警告，含黑带修复），诊断包 `a2f482c2`（`pagani-reset-wdog=1`、`pagani-reset-after=420`、`pagani-overlay=0`），21:54 自动上机（日志 `scratchpad/wdog-reset-run.log`）。
- **21:38 上机预装应用诊断包**（任务 `bs0j476d5`，内核 `a68d4357`，镜像 `8072330b`）。用户：诊断文字去掉了，但原来那块还是黑的——显示路径（`NativeGOPPresenter` 的 `NGP_METHOD_BAND` → `boot_progress_gpu_band`）一直跳过“状态预留带”不拷贝。修：`status_band()` 在 `pagani-overlay=0` 时直接返回无预留带，显示整帧拷贝；`Test/console.c` 断言隐藏时 `boot_progress_gpu_band` 返回 0。下一包内核重编中（任务 `b9jm2gl3z`）。
- **21:32 诊断文字可隐藏**：用户要求去掉挡住 iOS 左上角的诊断行。`BootProgress.c` 新增 `pagani-overlay=0`（`boot_progress_start` 读取）：`clear_line`、`glyph_pixels`、`status_paint` 三个底层绘制函数在非 panic 时一个像素都不画，启动 logo 与 panic 画面照常，ramoops 日志照常；`Test/console.c` 加白盒用例（-O2/-O0 通过）；`make-a830-images.sh` 的 `loop-m8-phone` 加 `pagani-overlay=0`。等复位加固代理改完 BootProgress 再一起编内核。
- **21:23 用户回来，手机在安卓**：安卓 `uptime` 10600 s → 18:26 开机，正是对照第 3 轮（约 18:19 fastboot boot）420 s 定时复位的时刻。**对照第 3 轮没有卡死**，是准时复位回安卓后 Mac 的 USB 一直没枚举到手机（审计 H1 成立）；18:35 推送的“卡死”判断有误。残留日志读不到（两区照例被丢）。
- **20:35 内核 `0b8e72e6`**（零警告，含 G1–G3 复位加固）与测试包：完整包 `测试镜像/boot-A830-回路-M8桌面-iPhone15Pro灵动岛-系统应用.img`（`85079de8`），备用包 `…iPhone15Pro灵动岛.img`（`0b3cac7a`）。等手机回到安卓后先测完整包。
- 19:42 工作流 `wrqkstnyj`（`wf_085fc5be-c9d`，报告 `audit/`）：两位独立审计员 + 裁决，审 2774af7c 之后进内核的全部改动，找“重负载下整机卡死”和“写坏 0x880000000 日志区”的根因，给出二分顺序与可先落地的防护。
- 18:10 工作流 `wme25jq30`（`wf_c4301966-867`，报告 `island/`）：用户要灵动岛机型（不换系统）：21H461 共享缓存里有完整的 SystemAperture 代码；`island` 逆向判断条件与几何，候选 iPhone 15 Pro（iPhone16,1，iOS 17、393 点宽、有操作按钮），新布局变体 `phone-island`；`storage` 设计持久 Data（安卓 /data 里 f2fs 锁定文件，XNU 只读写其物理块，范围表护栏），这一轮只做设计与主机原型、不往手机写。
- 17:50 工作流 `wn6ycnjer`（`wf_1a6aed10-3d2`，报告 `power/`）：`screen-off`（熄屏时 CPU 封低档、GPU 空闲进 slumber、面板下电，`pagani-screenoff`）与 `suspend`（系统挂起可行性：PSCI CPU_SUSPEND 深 idle、电源键中断唤醒路径，第一阶段原型 `pagani-idle`，默认关），各带审查。
- 16:45 工作流 `w3s51ngoh`（`wf_caf10115-a05`，报告草稿目录 `adsp/`）：照 sheng（SM8550，同一 iPadOS，BSD-2-Clause-Patent）移植 ADSP 启动核心（UFS 读固件、PAS、SMP2P、GLINK、QRTR），其上做电池电源服务（pmic_glink battmgr）和音频内核路径（APM 图、两颗 TFA9873）；iOS 侧音频服务和 HAL 插件先接静音后端（让音量条和音频路由出来）。全部由启动参数开启、默认关闭。
- 15:58 工作流 `wdi0iu5je`（`wf_9ab52548-983`，报告草稿目录 `fix2/`）：`crash-diag`（致命信号时把 pc/lr/帧链写进日志 + 离线符号化）、`gpu-slots`（客户端槽位扩容，海报扩展列入 Metal 客户端）、`keys`（Plus 键接成静音开关、电源/音量行为对齐 iPhone）、`cpu-gov`（触摸只提到响应档，按利用率快速升到顶）、`audio-plan`（音频完全没有驱动，只读调研出最短出声路线）；前四项各带一个独立审查。

## 待真机验证清单

### 当前队列（2026-10-31 05:45 更新；按顺序，前一项通过再做下一项；单变量；都用 `工具/gpu-test-loop/gpu-loop.py --android-timeout 1500 --yes`，电量门槛默认开）

| # | 项目 | 镜像 / 参数 | 看什么 | 条件 |
| --- | --- | --- | --- | --- |
| 0 | 读 01:01 卡死那轮的日志与复位记录 | 后台任务 `baw9fhme8` 在手机回安卓时自动跑 `read-ramoops.py --label kbd-f2-input-r2-1`，并把 dmesg 的 PON 行存到 `scratchpad/pon-dmesg.txt` | 卡在哪一步；PMIC PON 日志里今晚各次复位的 Reset Trigger/Type；是否欠压关机（UVLO） | 用户先处理手机：看屏幕、插拔线，停在 XNU 才长按 |
| 1 | A 包：诊断守护进程、控制中心、键盘 F2、点屏脚本、M14 计量、日志分区修复 | `测试镜像/boot-A830-A包-诊断-控制中心-键盘F2.img`（`4ef538c1`，内核 `f638f5b0`） | `diag:` 各行与 SAM 原话（预装应用为什么装不上）；`overlays:` 行 controlcenter 阶段 5 状态 0；`kbt` 按键 dn/up 计数是否不再为 0；`input script:`；`audio route`；开机一次性 U 行留到复位；`gpu meter`（`ap`、`poll`、`conn`） | 无 |
| 2 | B 包：A 包 + 电量计 | `测试镜像/boot-A830-B包-A加电池.img`（`66bb5f81`） | `[battery] start … pmic=1` → `i2c up` → `gauge addr=0x55 rc=0 type=…` → `state published pct=…`；iOS 显示的电量；不应有 panic | A 包正常 |
| 3 | 16 GiB VA 光圈 | 当前布局 + `A830_EXTRA_ARGS=pagani-gpu-va=16`（需按新内核重打） | `gpu aperture`/计量行、桌面与 Metal 客户端正常、无页故障 | 单独一轮 |
| 4 | LLCC | `pagani-gpu-llcc=1` | 帧时间、带宽档、温度；无挂死 | 单独一轮 |
| 5 | 速度档 | `pagani-gpu-speedbin=1` | 熔丝匹配的组与放开的档位、温度封顶 | 单独一轮 |
| 6 | M13 各子项 | `pagani-gpu-ifpc=1`、`pagani-gpu-slumber=1`/`2`、`pagani-gpu-ahbto=1` 各一轮 | 功耗、唤醒、无挂死（PLAN M13 验收） | 每项单独一轮 |
| 7 | 电源休眠 | `pagani-screenoff`（位 0 CPU、1 GPU、2 面板、3 断电）、`pagani-idle=1/2` | 熄屏后功耗、按键唤醒 | 单独，逐位 |
| 8 | D2 翻页阶梯 | `pagani-dpu-probe=4` → `=1` → `=2` → `pagani-te=1` → `pagani-fb-flip=1`（**不要用 `=3`，12:12 整机卡死**） | 每步的探针行；翻页是否锁存 | 逐步 |
| 9 | 复位前热复位（只读干跑） | 代码在 `scratchpad/battery-wr/warm/`，**未合入内核**；先只读核对 PON 记录（第 0 项），再 `pagani-reset-warm=2` 干跑 | 干跑行里的 PON 身份、归属、PS_HOLD 原值 | **用户在场**；实写 `=1` 只在用户在场时单独跑 |
| 10 | UFS 日志窗口 | `unixnu` 分区末尾 32 MiB | 复位后日志完整保留 | **要往手机存储写，需用户同意** |
| 11 | ADSP 探针 | `pagani-adsp=1`、停在 ≤2 | 电池管理器与扬声器的前置 | 单独一轮 |

1. **F1 显示提速**（`测试镜像/boot-F1-显示提速.img`）：**已验证，用户反馈明显更流畅（2026-10-29 23:25）**。以下为当时的判读要点，留作参考：
   - 看流畅度、画面是否正常、有无撕裂或花屏。
   - 本镜像有意去掉了 `pagani-platform-status=1`：屏幕上不显示 CPU/RAM 两行属于预期，这张镜像不能用叠加层确认八核。
   - 本镜像早于审查修复：映射缓存不会释放被替换的表面（最多约 51 MB），不影响正确性。
   - 若不开机、panic、花屏或比 R 更卡：下一次启动 `测试镜像/boot-S0-本机源码内核基线.img`（R 换本机内核，只差工具链）。S0 也坏 → 工具链问题；S0 正常 → F1 本身的问题，再做一张只加 `pagani-fb-wc=0` 的对照。
   - 任何异常都可用 `测试镜像/boot-H-build63载荷.img` 恢复。
2. **M0 空跑**（`out-gpu/a830-m0-*.img`）：已被下面第 3 项的集成镜像取代，不必再测。
3. **A830 集成镜像（2026-10-29 23:26，全部同一个内核 `8e81176c…29f2`）**。按下面的顺序一张一张启动，前一张通过再启动下一张；每张开机后拍叠加层第 0–3 行（GPU 行，在屏幕顶部的状态带里，进入桌面后也保留）与桌面，30 s 后再拍一张。
   - 进入 fastboot：安卓下 `adb reboot bootloader`；XNU 卡住时强制重启后在开机画面按住音量下键进入 fastboot。`fastboot boot` 不刷任何分区，强制重启即回到安卓。
   - **恢复**：任何一张异常（不开机、panic、花屏、卡死）都可用 `fastboot boot /Users/xiaochoumao/unixnu-13t/测试镜像/boot-H-build63载荷.img` 回到已知可用的 XNU，或直接强制重启回安卓。
   - 第 0 行格式：`GPU S<s>.<step> <名> FAIL <ioreturn> <reg>=<value> r<原因码>` 表示该阶段失败（拍下四行即可定位）；`GPU S<s> <名> STOP ok <ms>ms` 表示按 `pagani-gpu-stop` 停下且之前全部成功；`GPU RUN <MHz>M b<忙%> g<档>/b<总线档> s<提交数> f<故障> h<挂死>` 表示驱动在运行。

   | 顺序 | 命令 | 屏幕上应看到 |
   | --- | --- | --- |
   | 1 | `fastboot boot /Users/xiaochoumao/unixnu-13t/测试镜像/boot-A830-停在S3.img` | 第 0 行 `GPU S3 SMMU STOP ok <ms>ms`；第 1–3 行为 S3 诊断页：`SMMU ID <idr0> <idr1> <idr2> CR0 <scr0>`、`ACR <sacr> GFSR <gfsr> SLOT 0/3/1 S2C <s2cr0>`、`CB0 <sctlr> <actlr> FSR <fsr> CB2 <sctlr> <fsr> F0`；桌面照常（CPU 合成）。验证 S1 上电（与作者实测路径的五处差异：`0x71004` 只在关闭时写、关键 CBCR 不附加通用 10 µs、分支等待接受 NOC_FSM=ON、PLL0 未开时先停再配、RCG 先 GMU 后 hub）、S2 SCM 与 DDR/CN0 票、S3 SMMU 接管 |
   | 2 | `fastboot boot /Users/xiaochoumao/unixnu-13t/测试镜像/boot-A830-停在S5.img` | 第 0 行 `GPU S5 GMU-HFI STOP ok <ms>ms`；第 1–3 行：`GMU FW <core> HFI <hfi> INIT <result> <ms>ms`（result 应为 0x100 一类成功值）、`HFI M1 0 M2 0 M6 0 M8 0 M9 0`（每条应答状态 0）、`OOB <us>us GX <pwrclk> RPMH <state> AO <ao>`（GX 为 0 表示 GX 已上电） |
   | 3 | `fastboot boot /Users/xiaochoumao/unixnu-13t/测试镜像/boot-A830-停在S7.img` | 第 0 行 `GPU S7 ZAP STOP ok <ms>ms`；第 1–3 行：`CP RPTR <r> WPTR <w> RBBM <status> HW <fault>`（CP_INIT 后 RPTR=WPTR）、`SQE PC <br>/<bv> PROT <prot> UNMASK <shadow>`、`ZAP x0 0 x1 0 step 12 wq <n>`（step 12 = 已退出安全模式） |
   | 4 | `fastboot boot /Users/xiaochoumao/unixnu-13t/测试镜像/boot-A830-停在S9.img` | 第 0 行 `GPU S9 METAL STOP ok <ms>ms`（S8 的中断与栅栏自检已通过，S8 失败会显示 `GPU S8.<step> FENCE FAIL …` 和 S8 诊断页 `IRQ 332 …`/`COH V4 …`/`TS CP …`）；第 1–3 行为 S9 的通用页 `AUX0 … AUX5 …`（发布记录的原始字）；Metal 服务已注册，S10 调频未启动 |
   | 5 | `fastboot boot /Users/xiaochoumao/unixnu-13t/测试镜像/boot-A830-完整.img` | 板级默认参数、S0–S10 全部执行：第 0 行 `GPU RUN <MHz>M b<忙%> g<档>/b<总线档> s0 f0 h0`（没有插件，提交数为 0）；桌面仍由 CPU 合成；放 3 分钟，触摸、亮度、熄屏亮屏正常、无 panic |
   | 6 | `fastboot boot /Users/xiaochoumao/unixnu-13t/测试镜像/boot-A830-M7-Metal自检.img` | M7 种子（插件 + `adrenoprobed`，不带 `--publish`）、`pagani-gpu-status=3`、`pagani-gpu-dcvs=0`：第 0 行 `GPU RUN … s<递增>`；第 1 行插件报告（如 `adrenoprob s34 e0 v4/4 …`，e0 = 自检无错误）；第 2 行 Turnip 阶段行（`T s0 r0 …`）；第 3 行 `CLI o<打开> c<调用> s0 NGP …`；桌面仍由 CPU 合成 |
   | 7 | `fastboot boot /Users/xiaochoumao/unixnu-13t/测试镜像/boot-A830-M8-GPU桌面.img` | 开箱即用的默认镜像：`--publish` 种子、板级默认参数（DCVS 开、`sheng-fb-gpu=1`）：系统界面由 GPU 合成并经 GPU 交接上屏；第 0 行 `GPU RUN <MHz>M …` 频率随负载变化、提交数持续增长、f0 h0；检查流畅度、花屏/撕裂、图标文字是否正确、控制中心是否正常 |
   | 8 | `fastboot boot /Users/xiaochoumao/unixnu-13t/测试镜像/boot-A830-M8-固定607MHz.img` | 计划 M8 的评估形式（`pagani-gpu-dcvs=0`、`pagani-gpu-perf=7` 即 607 MHz/总线档 6、`status=3`）：与第 7 张对照流畅度；第 0 行频率固定 607M |

   判读要点：
   - 某张出现 `FAIL`：后面的镜像不用再试，拍下四行，按 ARCHITECTURE 8.3 的步骤号与原因码定位；同一阶段的回退选项见 `annibale-port/kernel-sequences.md` 第 15.2 节（草稿目录）。
   - S6 的 CP_INIT 超时：看第 2 行 SQE PC，BR `0xE3`/BV `0x4A` 是 LPAC 路由问题；再试 annibale 用过的 linux-firmware SQE/AQE 作单变量对照。
   - 提交模式切换条件（主控决定）：RB 在 CP_INIT 之后（`CP_SET_SECURE_MODE(0)`、首个 IB、栅栏）出现无法解释的失败时，整套换成 HWSCHED（`annibale-port/kernel-sequences.md` 第 5.5–5.7、9.4–9.6、11.1 节）；本轮代码没有 HWSCHED。
   - S3 写 S2CR 同步外部中止（直接 panic）：说明 13T 的 Gunyah 不让 HLOS 写 GPU SMMU 全局区，这是 13T 与 K90 的根本差异，记下后停止往后测。
   - 每张镜像的完整启动参数与种子在同名 `.payload.json` 里。

## 后台任务

- 2026-10-29 用户指示：不再审计，直接写完代码。已停掉 `wf_9782e932-765`（M0/M1 审查）和 `wf_7b522c2c-be1`（M2–M7，含审查环节）；停后全部主机测试 25 通过、0 失败。
- 当前：`wf_d61a47da-6b5`（任务 `wibzfo01j`），纯实现，无审查环节：
  - 11 个代理按文件归属并行写完 M1 接线与 M2–M13：SMMU/VM、GMU 核心、HFI/投票、CP/环、ZAP/平台、中断/栅栏/自检、DCVS/恢复/电源状态、发布外壳、编排器/板级、显示通路、用户态构建。
  - 之后一个集成代理：合并寄存器补充、更新构建清单与测试表、跑全部主机测试、编 GPU 线内核（含本地显示/探针/输入/ArmCpu 模块）、打镜像到 `out-gpu` 和 `测试镜像/`、更新本文件与上机清单。
  - 各代理报告写在草稿目录 `impl-reports/`。
- 2026-10-29 晚用户指示：继续写到完，按官方源、社区适配思路和官方文档。核对结果：
  - 官方组件 HEAD 与本地基线一致：Adreno740 `5c8c145`、AdrenoMetal `ce5e19e`、ArmSmmu4K `3ba9245`、sheng `08cde20`；重构后的移植文档与本地 `UniXNU/docs` 相同。
  - 作者官方 `codex/a830-metal-20261028` 分支只有加密输入，无可读源码。
  - 社区 Gen8 适配（`zg7681/NativeAdreno-myron` 的 `a8xx` 分支，SM8850/A840）的经验：整份 Turnip 提交一次进内核（ABI 128 个缓冲，官方已含）；用户 IB 前线程选 BOTH；A8XX 复位序言写两个 SRAM 初始化；寄存器表可交叉核对。
  - 要点已写成集成步骤必读的 `impl-reports/00-lead-official-alignment.md`（草稿目录），包括官方 Metal 发布流程（`add-gpu-plugin-data.py --publish`，GPU 合成桌面）、格式 2 设备仓库与组件目录、中英文驱动文档页。
- 2026-10-29 用户要求找同为 Adreno 830 的现成驱动照抄。检索结论：
  - 唯一公开且在真机跑通 A830 的 XNU 驱动是 REDMI K90 的 annibale（HackiOS，SM8750）：内核 `iola1999/hackios-xnu-kernel`（分支 `k90-port`，`bsd/dev/adreno8*.c`，APSL 2.0）。它的桌面走 OpenGL ES（freedreno），不是 Metal。
  - UniXNU 的 hummer、ace6 都没有 GPU；HackiOS 一加 13 分支未上硬件；作者的 13T Metal 版源码加密。
  - 抄作业工作流 `wd405oygk`（`wf_5ea3f487-1de`）只读提取已验证的逐阶段序列、实测笔记和 A830 用户态补丁，写到草稿目录 `annibale-port/`。集成步骤按 `impl-reports/01-lead-annibale-homework.md` 把内核各阶段改成 annibale 已验证的做法。
- 2026-10-29 21:55 抄作业提取完成（`annibale-port/` 三份）。
  - 主控决定：提交模式仍走 RB 主线（Linux 上游 A830 即 RB；annibale 用 RB 走通到 CP_INIT），HWSCHED 留作已验证退路。
  - 第 14 节取舍表中“改成 annibale 做法”的各项、field-notes 的优先项、用户态缺项，都已交给集成步骤（`impl-reports/01-lead-annibale-homework.md`）。
- annibale 快的原因是试错回路：USB 常驻 loader、日志与截图走 USB、软件复位约 17 s、无人值守脚本，一轮几分钟。我们每轮都要用户拍屏。
  - 提速方案（待用户同意后实测）：让 XNU 把日志写进安卓 ramoops 区。只读核对结果：`mem_address=0x880000000`、`mem_size=0x240000`、`console_size=0x40000`、`ecc=0`、`mem_type=0`；上次重启后 `console-ramoops-0` 内容保留但有位翻转。
  - 写入时加冗余与校验，测完回安卓后用 adb 读 `/sys/fs/pstore`；再加定时复位，即可免拍照、自动循环。
  - XNU 的内存图要把该区域设为保留。
- 已停掉一直在等 fastboot 的旧 F1 自动启动任务（`boalamgqb`），避免抢在新镜像之前启动；F1 改动已进入 GPU 线的新构建。
- **2026-10-29 22:00 用量到限，断点在此**：
  - 已手动停掉实现工作流 `wf_d61a47da-6b5`，以免集成步骤写到一半被切断。
  - 11 个模块代理里 10 个已完成，代码在磁盘上，报告在草稿目录 `impl-reports/`。`publish-shell`（发布外壳）被中断，可能写到一半。集成步骤（内核构建、镜像）尚未开始。
  - 续跑方法：`Workflow({scriptPath: "/Users/xiaochoumao/.claude/projects/-Users-xiaochoumao-unixnu-13t/5559c563-618d-4813-9848-be3768908d8c/workflows/scripts/adreno830-implement-all-wf_d61a47da-6b5.js", resumeFromRunId: "wf_d61a47da-6b5"})`。已完成的 10 个代理直接取缓存，只重跑 `publish-shell` 和集成。
  - 集成会读 `impl-reports/00-lead-official-alignment.md` 与 `01-lead-annibale-homework.md`。
- 2026-10-29 用户说“继续”：已续跑 `wf_d61a47da-6b5`（任务 `w3w2wdn8e`）。
  - 核实 11 个模块代理全部完成。`publish-shell` 在 21:57、停止前已写完，ABI 头 sha256 与官方一致。
  - 集成步骤正在运行。
  - 用户态代理已在本机编出全部产物：iOS Turnip（含 830 与 0x44050001）、metal2vulkan 桥、`NativeAdrenoMetal.bundle`、`adrenoprobed`、owner 信任包。两份 Data 种子已暂存（M7 不带 `--publish`，M8 带 `--publish`），不需要 sudo。
- 23:15 集成步骤仍在做第一步（把各模块的寄存器补充合并进 `A830Regs.h`），内核还没重编。
  - 为缩短等待，另起隔离工作流 `w29gl13og`（`wf_ca0d2434-b9f`），只动集成不碰的文件，对已有文件的改动以补丁交付、集成后合入：
    - `fastloop`：免拍照上机回路——ramoops 双区写日志（行号 + CRC32）、`pagani-reset-after` 定时热复位（优先 PSCI SYSTEM_RESET2 warm）、adb 读取解码与循环脚本（`工具/gpu-test-loop/`，执行重启前必须加 `--yes`）；
    - `userspace-fixes`：A830 用户态缺项（`dsb sy`、noaliastex、三处上游修复、一致性映射收窄、ir3 abort、CCHE 失效），在私有副本里重编并重新暂存种子。
- 构建脚本新增 `--only`：并行开发时只用内核配方编译指定文件，不受别人未完成文件影响。
- **2026-10-29 23:26 集成步骤完成**（报告：草稿目录 `impl-reports/integration.md`）：
  - 整核用 `CCC_OVERRIDE_OPTIONS='# +-Wno-unnecessary-virtual-specifier'` 编过，不需要退回 `+-Wno-error`；GPU 线内核 sha256 `8e81176cce4a9dec1150b4ed5ca659a083b0b7c639473b7b682c984574ae29f2`。
  - 主机测试 `47 passed, 0 failed, 0 expected failures, 0 skipped`（含内核对象检查）；`mutate_sm8750_power.py` 26 个变异全部杀死；PD 各主机测试、设备仓库 `test-format2.py`、`porting-record.py check`、`test-gpu-inputs.py`（含 `--live-dtb`）、`test-peripherals.py` 全部通过。
  - 8 张镜像见上面的上机清单第 3 项；`out-gpu/a830-*.img` 是同一批镜像的英文名副本。

## 集成记录（2026-10-29）

- **寄存器补充**：各模块的 “A830Regs.h additions” 块全部移进 `A830Regs.h` 第 21 节（RBBM 状态/SECVID、CP 故障与恢复、GMU 启动常量、HFI 布局与带宽表下标、AOSS QMP 描述符、V9c 的 A2D 寄存器与取值、A830 sysmem CCU 布局）；新增的 18 个 GPU 寄存器加进 `tests/test_a830regs.c` 的 Mesa 逐项比对（现为 180/180）；`Sm8750AossQmp.h` 的自带枚举在 `test_sm8750_aossqmp.c` 里与 `A830Regs.h` 静态比对。
- **接口对齐**：
  - 显示调用的是 `pagani_display_vsync/late/underrun`（弱空默认），DCVS 导出的是 `adreno830_dcvs_vsync/late_frame/underruns`：`A830Dcvs.cpp` 加了三个强定义转发（迟到 ns→µs）。
  - 提交路径：每次提交前后 `A830PowerState::acquire/release`（P1/P2 由提交唤醒；转换或恢复中按环满处理，200 ms 内重试）；成功后 `A830Fence::submitted(最后序号)`（看门狗在无人等待时也能发现挂死）；WPTR 围栏写 200 次失败导致环中毒时 `adreno830_recovery_report(A830FaultWptrFence)`。
  - GMU AO 计数器读取（GetTimestamp）在 CX 断电的 slumber 下先唤醒 GPU（`accessBegin` 失败则 `acquire`）。
  - `A830Irq.h` 已改名为 `A830IrqFault*`，编排器与 `test_start` 里的改名宏删掉了；`armcpu_spi_*` 弱默认只留 `A830Irq.cpp` 一份。
  - IFPC：电源操作 `gmuStart` 成功后调用 `A830Irq::setIfpc(args->ifpc)`。
- **新启动参数**（设计未命名，M13 单变量镜像要用，默认值即设计的 P0）：`pagani-gpu-slumber=0/1/2`（0 关；1 P1 完全断电；2 P1 保持 CX）、`pagani-gpu-ifpc=0/1`、`pagani-gpu-ahbto=0/1`（每次 GMU 启动设 CX_AHB 超时检测）。S0 校验上限，`test_start` 覆盖越界拒绝。
- **构建脚本**：`GEN8` 13 个 A830 源文件 + 编排器/状态块，`PLATFORM` 7 个平台源文件；`NativeAdrenoKernelTime.cpp` 并入 `INHERITED`；未列入的 A830/平台源文件改为直接报错；`PENDING`（临时生成弱默认）删除；`REQUIRED` 27 项、`WEAK` 19 项（含 `armcpu_spi_*`、`pagani_input_touch_events`、`sm8750_rpmh_drv2_owner`）；`EXCLUDED` 只剩两个主机专用文件，退役名字留在 `FORBIDDEN` 正则里。
- **测试表**：`EXPECTED_FAIL`、`RETIRED` 清空；`KNOWN` 删掉 `test_irq`、`test_cpinit`，19 个已知测试钉住 PASS 行；同名的 `tests/test_a830regs.cpp` 改名为 `tests/test_a830headers.cpp`。
- **待主控另起一轮**：
  - annibale 第 14 节取舍表的改动都没有做：SMMU 上下文值（SCTLR `0x27`、TCR `0x803519`、MAIR0 `0xf404ff44`、只给 CB0 写 ACTLR）与 ASID 失效、不写 sACR/sGFSR、首轮不挂 710/712；GMU 中断全屏蔽轮询、`AHB_FENCE_RANGE_0=0x80c808c0`、RSC 睡眠先唤醒、`GENERAL_11` 检查、TCM 逐字回读、首轮不播种 AO 计数器；HFI 500 ms、首轮 4 档性能表与 `{1,1,2}`、去掉 M7、`FEATURE_CTRL` 严判；OOB 不清；S1 写五个 AHB 超时；hw_init 首轮列表与 `INT_0_MASK=0`；CP_INIT 判据与 200 ms；332 不作发布门槛；首轮 342 MHz 上限；`pagani-gpu-hwsched` 开关位。这些改动大多与设计默认冲突，适合做成一个“annibale 首轮”配置统一切换。
  - `userspace-a830.md` 的用户态缺项由隔离工作流 `w29gl13og` 处理，合入后要用 `build-userspace-a830.sh` 重做信任包与种子，再重打 M7/M8 两张镜像。
  - UniXNU 的 `docs/drivers/adreno830.md`、`docs/zh/drivers/adreno830.md` 两个驱动页还没写。

## IssacL04 integration (2026-11-04)

Imported from Shad0w23333/UniXNU-OnePlus13T at 3c514bc into the Pagani component repository. The ABI header includes the current A740/Metal constants and remains byte-identical to that header. Optional trace and pixel methods use the existing bounded trace store. NativeGOP handoff keeps both completion sizes and both status-band layouts; task ownership and mapping references survive close/failure paths. Local presenter regressions pass. Production compilation and device validation are pending; the hardware observations above refer to the source author's builds.
