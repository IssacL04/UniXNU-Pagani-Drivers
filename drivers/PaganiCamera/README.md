# PaganiCamera

一加 13T（PKX110，SM8750）的相机，按路线报告（`build-userspace/a830-inputs/round-1101/camera.md`）的计划做了两块，都默认关：

- **阶段 1a**：`ios/`，H6ISP.mediacapture 里同进程的 `CMIOExtensionProvider`，出带二维码的滚动测试图。不碰相机硬件，用来验证 iOS 一侧。
- **阶段 2**：内核驱动，走到经 CCI 读出长焦 S5KJN5 的 ID（`[cam] cci0 m0 id 0x38e5`）。不出帧。

**硬件状态：这里的东西都还没上过机。** 主机测试、内核编译和 iOS 插件的链接都通过；它们不能证明任何东西在手机上能工作。不知道的数字都标了，代码在读到意外值时停下。

## 阶段 1a：测试图

### 为什么这样做（都读了 21H461 的反汇编）

1. **厂商类在这台手机上是 nil，路线报告和手电筒的报告都没看到这一点。** `-[BWFigCaptureDeviceVendor initWithDefaultDeviceCreateFunction:cmioExtensionDeviceCreateFunction:cmioDiscoverySession:]`（0x1a693fcf4）在 0x1a693fde0：默认创建函数是空、又不在虚拟机里，就走 `bail`，返回 nil。虚拟机的判断是 `kern.hv_vmm_present`，arm64 上读设备树 `/defaults/vmm-present`，这个项目只有 QEMU 文档提到它，手机上是 0。所以只要 `dlsym H6ISPCaptureDeviceCreate` 失败（手电筒故意的），`+sharedCaptureDeviceVendor` 永远返回 nil。后果：外接相机出不来；**`PaganiTorch/ios/TorchPlugin.m` 替换的六个方法也一次都不会被调用，因为消息发给了 nil，系统手电筒按钮不会亮。**
2. 修法：插件在 `FigCaptureGetCaptureDeviceCreateFunction` 里 dlopen，正好在那次 init 之前（0x1a693ee7c），构造函数把 init 换掉：默认函数是空时传一个永远失败的占位函数过检查，返回后把实例变量 `_defaultDeviceCreateFunction` 写回空。之后的状态和虚拟机里没有默认设备时完全一样：`deviceIDs` 不含 Default（0x1a69aab60），`_createDevice` 打 `Cannot create device without create function!` 后返回错误（0x1a68c2258）。
3. **绕过 launchd。** `+startServiceWithProvider:`（0x21ccde8d8）和 `-[CMIOExtensionProviderServer init]`（0x21ccfcbb4）都要主 bundle 有 `CMIOExtension` 字典，mediaserverd 没有：前者抛异常，后者返回 nil。nil 是好事：`initWithSource:clientQueue:`（0x21ccde34c）和 `-[CMIOExtensionProviderContext initWithConnection:]`（0x21cd054c4）只问它 `internalDaemon`，nil 答 0，客户的相机授权直接是 3（已授权）。所以不启动服务，自己做它做的两件事：匿名监听（`xpc_connection_create(NULL, q)`、`xpc_endpoint_create`）；新连接上 `initWithConnection:` 加 `beginProviderContextRegistration:`（`-addConnection:`，0x21ccfd25c）。
4. **挂进发现会话。** 照 `completeRegistrationForBundleID:token:` 的回调（0x21cd0b5c0）：在会话自己的队列上 `initWithEndpoint:delegate:`、`setBundleID:`、放进 `_providersByBundleIdentifier`。设备到了之后 `provider:availableDevicesChanged:` → `postDeviceChangeNotification` → 厂商的 `_updateCMIOExtensionDevices`（0x1a69af35c）→ `CMIOExtensionFigCaptureDeviceCreate`，后面是苹果自己的路。
5. `-[CMIOExtensionStream sendSampleBuffer:...]`（0x21ccdbe5c）最后是 `dispatch_async` 到 clientQueue，不会和 source 里的 `dispatch_sync(帧队列)` 互锁。进程里只能有一个 `CMIOExtensionProvider`（0x21ccde3a0，第二个抛异常），mediaserverd 本来没有。
6. 流的格式：`CMIOExtensionFigCaptureStreamCreate` 只原样保留 `420f`、`420v`、`BGRA`，这里用 `420f`，1080p 和 720p 两档，默认 720p，30 fps 固定。`-[BWFigVideoCaptureDevice _initWithCaptureDevice:...]` 外接时强制读厂商和非本地化名，所以设备还给 `CMIOExtensionPropertyDeviceManufacturer`、`CMIOExtensionPropertyDeviceNameNonLocalized`：只给公开的 provider 属性会是 nil（主机测试抓到的）。

### 和手电筒共存

同一个文件 `H6ISP.mediacapture`，`TorchPlugin.m` 一个字没改，两个构造函数互不依赖：手电筒换厂商类的六个实例方法，这里换它的 init。不导出 `H6ISPCaptureDeviceCreate`。

| 种子里有什么 | 插件做什么 |
| --- | --- |
| 手电筒脚本编的插件 | 和以前一样（但见上面第 1 条：厂商类是 nil，手电筒按钮不亮） |
| 这里脚本编的插件，没有标记文件 | 手电筒加厂商类修正，没有相机 |
| 这里脚本编的插件，有标记文件 | 再建提供者，出测试图 |

标记文件是 `ngpu/mediacapture/PaganiCameraTestPattern`，叠加后在 `/System/Library/MediaCapture/PaganiCameraTestPattern`。用标记文件不用启动参数：不占命令行，而且只有带这个插件的种子才有意义。

### 重编插件、信任包

都不碰手机，脚本也不部署：

```
scripts/build-pagani-camera-plugin.py --out DIR
scripts/add-pagani-camera-data.py --plugin DIR --trust <当前信任包> --replace --test-pattern BASE.dbrdisk OUTPUT.dbrdisk
```

`--replace` 换掉 BASE 里已有的插件并把旧 CDHash 从属主模块删掉，苹果模块逐字节比对、绝不改。写出 OUTPUT、`<OUTPUT 主名>-owner-trust.bundle` 和 `.json`。镜像再加 `--data-ramdisk OUTPUT --owner-trust-cache <信任包> --kernel-arg pagani-torch=1 --kernel-arg pagani-torch-ios=1`。插件只要重编，CDHash 就变，信任包必须跟着重做。

### 判据和怎么测

打开**代码扫描器**（`com.apple.BarcodeScanner`，主屏没有图标：`SBAppTags` 是 `hidden`，入口是控制中心的 `QRCodeModule`，它的 `UIRequiredDeviceCapabilities` 只有 arm64，在设置 → 控制中心里加上），应该看到滚动彩条和中间的二维码，并识别出 `https://unixnu.com/pagani-cam-test`。mediaserverd 5 分钟不崩。相机 App 不认外接相机，这一阶段不看它。

U 行（`pagani-torch-ios=1` 时内核发布日志服务 `PaganiCameraLog`，只有 mediaserverd 能开）：`[cam] ios: log service published`、`[cam] ios: provider start rc=0`、`[cam] ios: stream start`、`[cam] ios: frames=1 dropped=0 mode=1280x720`，之后每 900 帧一行，`[cam] ios: stream stop`，一次启动最多 48 行。没有 `provider start` 这一行就是标记文件没看见（沙盒可能挡 `access`）或者厂商类没建起来。

要上机才知道的：

- mediaserverd 里 dlopen 插件的信任链，第一次（机制同 audiomxd）。
- 沙盒让不让 `access` 看标记文件；匿名 XPC 和 IOSurface 应该不受限，没验证。
- 21H461 的 CoreMediaIO 行为和 mac 27 的差别：接口逐个核对过，主机测试跑的是 mac 的实现。
- 外接相机在 iPhone 身份下的分支（`BWDeviceIsiPad` 看 DeviceClassNumber）。
- 第一次枚举可能赶不上：插件在厂商类第一次使用时才加载，提供者是异步的，第一次打开代码扫描器可能没有设备，退出再开一次。
- `VTPixelTransferSessionTransferImage`（有客户缓冲池时走它）在这台手机上能不能转。

## 阶段 2：长焦上电，读 ID

### 分级

| 级 | 步 | 动作 | 判据 |
| --- | --- | --- | --- |
| 1 | 101 | 只读 GCC 的三个分支、四个脚、三路轨道（SPMI observer） | 一个寄存器都不写，日志完整 |
| 2 | 102 | 开 GCC 的相机 AHB、XO、BIST AHB；分支都起来才读 CAMCC 和 cambistmclkcc，只读 | `[cam] camcc gdscr=...` 不是全 1 |
| 3 | 201 | 投 `mmcx.lvl`、`mxc.lvl` 到 `0x180`（TURBO）；CAMCC 常开的四个分支、titan GDSC、AHB 和 CCI 的 RCG 切 XO、分支；逐个确认状态之后读 `CCI_HW_VERSION` | `[cam] domain on ... cci_version=...` 不是全 0 或全 1 |
| 4 | 301..304 | 先等 4.5 s，持住写者，下电走完才放开。复位拉低；三路轨道（闸门）；MCLK0 19.2 MHz；CCI 脚；放开复位 20 ms；W `0xfcfc=0x4000`，R `0x0000`；不论成败下电 | **`[cam] cci0 m0 id 0x38e5`**，`[cam] rail ... off` 三行，无 NoC 报错 |

上机按 1、2、3、4 走，每一级看过再走下一级。

### 安全

- **未上电访问会挂总线。** 模型把这条写死：没开时钟的块一碰就是测试失败。CAMCC 之前查 GCC 分支，CCI 之前逐个查 GDSC、core_ahb、top_ahb、top_fast_ahb、cci_0。
- **第一次碰可能挂死的块**（步 102 的第一次读 CAMCC、BIST CC，步 201 的 GDSC 到第一次读 CCI，级 4 从第一次上轨道到下电走完）：先等 3.5 s 让上一行落到 UFS 日志，再按位持住写者（`pagani_panel_probe_quiet` 的 bit 4，`fetch_or`/`fetch_and`），再等 1 s，然后才碰。真挂住了就一直持着，不释放，定时复位和 panic 的最后快照照样写得出去。
- **电压白名单。** `pcam_vrm_allowed` 只放三个资源的三个值：`ldom5` 1800 mV、`ldom4` 2200 mV、`ldoi1` 1000 mV，就是 cam-sensor2 的 `rgltr-min-voltage` = `rgltr-max-voltage`。核心和胶水各过一遍，主机测试把 0..8191 mV 全扫过。投后读回：读到了就必须等于白名单，读到别的值就下电。开始前已经开在别的电压上：一票不投。
- **写闸门。** `pcam_write_allowed` 白名单：GCC 三个 CBCR 只许置位；CAMCC 十五个寄存器，GDSC 只开不关，RCG 只许切 XO；TLMM 只有四个脚，功能只有目标或 GPIO，电平只有复位脚；CCI 只有 master 0、queue 0。
- 不投 `mx.lvl`：GPU 把它投在表顶，DRV-2 每个资源只有一票。`mmcx.lvl`、`mxc.lvl` 投 `0x180`，是设备树 `qcom,init-voltage-level`、Android 启动时投的值，这个值是 TURBO 档（CPAS 的 `vdd-corners` 和 `vdd-corner-ahb-mapping`：`0x100` nominal，`0x180` turbo），比 CCI 要的高得多，选它是因为这一票会顶掉 UEFI 在 DRV-2 上留下的那票，显示也在 MMCX 上，不能把它投低。这两票不撤，和 GPU 的规矩一样，这次开机这两条轨一直在 TURBO。
- 不碰 `ldob14`（`cam_vaf`）：读 ID 不需要音圈，它在设备树里还挂着一个触摸节点。
- 不做 GDSC collapse：级 4 结束时 GDSC 和 AHB 分支留着，第一次 collapse 有风险，这是测试级，不值得。轨道、MCLK、脚都还原。
- 不开 PLL：CCI 和 MCLK 都走 XO。CCI 19.2 MHz 用设备树 37.5 MHz 的 400 kHz 节拍表，总线约 204 kHz，所有最小时间都只变长。
- 不用中断：SPI 0x1aa 不开，轮询 `IRQ_STATUS_0`。
- 不 panic；不写存储；线程走完就退出，不留轮询。

### 要在真机上确认的（都是“读到什么算什么”，错了停在那一级）

| 项 | 现在用的 | 来源 | 哪一级见分晓 |
| --- | --- | --- | --- |
| GCC 三个分支的开机状态 | 读 | Android 的 probe 无条件开，XNU 没跑 | 1 |
| `0xa0004` 是 cam_bist_mclk_ahb | 推断（probe 里第一个，Android 无条件写） | [KO] | 2：BIST 读得通 |
| MMCX 在 | 靠显示在线；级 3 才投票 | 推断 | 2 |
| GDSC 状态位 | `PWR_ON` 或 `CFG_GDSCR` bit 16 都算 | [U]、[DT] | 3 |
| CCI 的 AHB 要哪几个时钟 | core、top、top_fast、cci_0 | [DT] CPAS 的 clocks，没开 AXI 和 camnoc | 3 |
| `IRQ_MASK` 写了状态才锁存 | 写了 | [U] | 4 |
| CCI 的偏移和命令字 | `PaganiCameraBoard.h` | 按公开驱动 `i2c-qcom-cci.c` 的记忆写的，这台机器上没有源码，没有逐行核对 | 4：错了是超时或 NACK，不伤硬件 |
| 传感器 7 位地址 | 先 `0x2d` 后 `0x10` | 推断，设备树没有 | 4，阶段 0 能直接给答案 |
| 上电顺序和延时 | vio、vana、vdig、MCLK、放复位 20 ms | 推断，RESET 全程拉低 | 4，阶段 0 给真实时序 |
| PM8010 的 PPID 能不能读 | 读不到记 `unread`，不拦 | 可能在 spmi1 | 1 |
| `ldoi1` 的上级 | 不知道，这份代码没有碰 S7I | 设备树没有写 LDO 的输入；`ldoi1` 和 UFS 的 `ldoi2`（`qcom,vddp-ref-clk-supply`）在同一颗 PMIC 上，上级很可能是 S7I，也就是 UFS vccq 的上级。设备树让 S7I 停在低模式，这个内核不投负载 | **级 4 之前**从阶段 0 的第 8 项确认。是 S7I 的话，上轨道之前要先按 Wi-Fi、蓝牙的规矩读 S7I、投高功率模式（只升不降）：`cam_vdig` 的额定负载是 1.2 A，读 ID 只用到一小部分，但这条轨带着 UFS |

## 阶段 0：要从手机上取的

**只读就能拿到（`adb shell` 或 `adb pull`，不改任何状态，安卓下）：**

1. `/odm/lib64/camera/com.qti.sensormodule.paganiTele.bin`（Main、Front、Wide 同样）：从地址、上电时序、init 序列、EEPROM。放 `build-userspace/a830-inputs/` 下的本地目录，不进被跟踪路径。
2. `/odm/lib64/camera/com.qti.sensor.pagani*.so`：曝光增益换算。
3. `/odm/lib64/camera/com.qti.tuned.pagani*.bin`：Chromatix 调参，阶段 5 才用，先拿着。
4. `/vendor/etc/camera/*`：传感器和模组的 XML，如果有。
5. `cat /sys/kernel/debug/clk/clk_summary`（要 root）或 `/proc/device-tree`：相机没开时的基线。
6. `dmesg | grep -i -E 'cam_|cci|csiphy'`：现有日志里的 CCI 版本、CSIPHY 版本。
7. `/sys/class/regulator/*/name` 和 `state`：相机没开时那几路轨道的状态。
8. `cat /sys/kernel/debug/regulator/regulator_summary`（要 root）：`pm_v8i_l1`、`pm8010m_l4`、`pm8010m_l5` 的上级是谁。级 4 之前要知道 `pm_v8i_l1` 是不是挂在 `pm_v8i_s7` 下面，那条轨带着 UFS。

**要开调试参数（写 sysfs，要 root，开相机，用户来做）：**

1. `echo 0x... > /sys/module/camera/parameters/debug_mdl`、`debug_type`，开一次相机依次切主摄、长焦、前摄，导出 dmesg：`CCI%d_I2C_M%d_Q%d_LOAD_DATA_ADDR` 给出完整的 I²C 序列（从地址、init、stream-on），`3phase:%d, combo mode:%d` 给出 PHY 类型。这是最省事的一步，决定主摄后面要不要做 C-PHY。
2. 同一次 `clk_summary` 在相机开着时再抓一份：CCI 和 CSID 的真实频率。
3. 关掉调试参数。

**拿到之后马上能改的：** `kPcamTeleAddresses` 缩成一项；步 302 的时序换成真实值；阶段 4 的序列加载器有了输入。

## 工作量和终点

已完成：阶段 1a 和阶段 2 的代码、主机测试、变异检查。还差的（按路线报告，常态每天 3–6 轮上机，用户在场）：

| 阶段 | 内容 | 代码 | 上机轮次 | 风险 |
| --- | --- | --- | --- | --- |
| 0 | 上面的清单 | 无 | 用户操作 1 次 | 低 |
| 1a 验证 | 这里的测试图 | 已写 | 3–6 | 中 |
| 2 验证 | 这里的驱动 | 已写 | 3–6 | 中，卡住不至于坏硬件 |
| 1b | 内建假设备，进相机 App | 800–1500 行 | 8–20 | **高**，全程最不确定 |
| 3 | TPG → RDI → DDR | 约 1500 行 | 5–12 | 中高 |
| 4 | 传感器 RAW 一帧 | 约 1000 行加解析器 | 5–15 | 高 |
| 5 | 软件 ISP、AE/AWB、连续预览 | 约 2000 行 | 5–10 | 中，功耗未知 |

合计常态 2–3 个月，最乐观 6 周，1b 和 4 是可能无限拖延的两个点。

**诚实的终点是网络摄像头级别：** 能取景、能扫码、1b 成功的话能拍基础 JPEG。画质相当于没调好的网络摄像头：没有 HDR、夜景、人像、连拍、深度。录像大概率没有（系统里只有硬件编码器），HEIC 没有，前后摄不能同时，功耗明显高于安卓。进不了相机 App 时的 B 计划：自写一个小相机 App 直连内核用户客户端。这条路线的下限是确定的，上限不是“接近日用”。

## 启动参数

`pagani-cam` 一个参数，值就是级，默认关：

| 写法 | 含义 | 占用字节（含一个空格） |
| --- | --- | --- |
| `pagani-cam=1` | 勘察 | 13 |
| `pagani-cam=2` | 再读 CAMCC | 13 |
| `pagani-cam=3` | 再开电源域，读 CCI 版本 | 13 |
| `pagani-cam=4` | 再上电读 ID，下电 | 13 |

其他值都当关，写一行 `[cam] pagani-cam value not understood`。

`pagani-torch-ios=1`（手电筒的参数，这里只读）时多发布一个日志服务，让插件的进度进 U 行，不占字节。两者都没有时 `pagani_camera_start` 立刻返回：什么都不映射、不分配、不启动，类也不注册。

阶段 2 的 13 字节够放（命令行 1020，现在 998）。阶段 1a 要手电筒那两项共 34 字节，不够，要先腾 12 字节。

## 接口

- `pagani_camera_start()`：Stage7Probe 在蓝牙之后调用。
- `pagani_camera_status[16]`：`PaganiCamera.h`。
- `pagani_panel_probe_quiet` 的 bit 4。

## 构建和测试

- `scripts/build-pagani-camera.py --tree TREE --out DIR [--append-to libAdreno830.a]`：内核归档（组件配方），类初始化摘出 `__mod_init_func`。
- `scripts/build-pagani-camera-plugin.py --out DIR`、`scripts/add-pagani-camera-data.py`：插件和种子。
- `Test/check-pagani-camera-host.py [--tree TREE]`：主机检查。`provider` 跑在这台 Mac 真的 CoreMediaIO 上，帧走一圈匿名 XPC 回来，CoreImage 和 Vision 读回二维码。
- `Test/mutate-pagani-camera.py`：变异检查。
- `integration/`：Stage7Probe、board.toml、components.toml 的改动，主线合入。

主机检查不能证明任何东西在手机上能工作。

## 来源

只取事实，不含 GPL 或厂商代码：运行设备树；原厂 `vendor_boot_a.img` 里 `camcc-sun.ko`、`cambistmclkcc-sun.ko`、`gcc-sun.ko`、`pinctrl-sun.ko` 的数据段和 probe；主线 `i2c-qcom-cci.c`、`gdsc.c`、`clk-rcg2.c`、`pinctrl-msm.c` 的寄存器事实；21H461 的静态反汇编；ISO/IEC 18004。`CamUtil.h`、`CamPmic.c` 和日志写法取自 PaganiBt（同一仓库，同一许可证）。详见 `PaganiCameraBoard.h` 开头。
