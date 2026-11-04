# PaganiBt

一加 13T（PKX110，SM8750）的蓝牙控制器（peach 的 BT 子系统，属性 `ganges`，运行固件 `BRAHMA`），走 QUP2 SE6 串口。这一版做到路线报告（`build-userspace/a830-inputs/round-1101/bluetooth.md`）的阶段 0 到阶段 4：纯函数、GENI UART、IBS、H4 过滤层、上电、握手、固件下载、HCI 校验、LE 扫描自检，以及接真 `bluetoothd` 的桥（`BtBridge.h`、下面“阶段 4”一节）。

默认关。没有 `pagani-bt=N` 时 `pagani_bt_start` 立刻返回：什么都不映射、不分配、不启动，iOS 也看不到任何蓝牙的东西。不 panic：任何失败都是 `pagani_bt_status` 里的一个步号和错误码，加一行 `[bt]` U 记录（BootProgress ramoops，`pagani-ramoops=1`），线程结束。不写存储，NVM 只改内存副本。阶段 1 到 3 线程跑完就退出，不留轮询；阶段 4 线程留下来当桥的工作线程，轮询档位见下。

**硬件状态：这里的东西都还没上过机。** 主机测试、内核编译和跟踪回放都通过；它们不能证明任何东西在手机上能工作。

## 阶段（`pagani-bt=N`）

| 步 | 动作 | 判据 | 上机前的把握来自 |
| --- | --- | --- | --- |
| 101 | 只读：GCC 投票、分支、RCG、GPLL0，串口脚、WLAN_EN、SW_CTRL，七路轨道，睡眠时钟，BT_EN | 日志完整，一个寄存器都不写 | 模型 |
| 102 | 投 `0x52010` 开 QUP2 外壳和 SE6 时钟；分支都起来才读 SE，第一次读之前等 4.5 s：上一行落到 UFS 日志，再持住写者（`pagani_panel_probe_quiet` 的 bit 2），读的时候线上没有 UFS 写 | 协议域 = UART（2），FIFO 没关 | ClockDxe 表 |
| 201 | 先等 4.5 s，持住写者（`[bt] guard rails`），睡眠时钟脚、七路 RPMh 轨道、`rfclka1`，走完放开 | S7I 不降，UFS 不报错 | 同 Wi-Fi 规则，也同 Wi-Fi 的 `rails` 段：S7I 带着 UFS |
| 202 | 先等 4.5 s，持住写者（`[bt] guard bt_en`）：芯片的负载随 `BT_EN` 落到 S7I 上。`BT_EN` 低 ≥ 54 ms，再高，等 54 ms，走完放开 | PMIC 读回，SW_CTRL 有记录 | dmesg 时序 |
| 301 | RCG 7.3728 MHz，SE 115200 8N1，内部回环（只记录），TLMM 功能 1，硬件流控，开接收 | RCG 更新完成，`ios` 有记录 | 模型、跟踪里的寄存器值 |
| 302 | 发 `0xfd`，最多 10 次，每次等 10 ms | **收到 `0xfc`** | HAL |
| 303 | 发 `31 00 f0 ff 02 05 00` | **收到 `34 00 ff …`，原样记录** | HAL（RX 在跟踪里丢了） |
| 304 | 可选：`pagani-bt=304` 时发 `01 03 0c 00` | 只记录，不作判据 | —— |
| 401 | 阶段 2、3 在上电之前先做：从分区原地读五个文件，等分区就绪最多 120 s，读不到时芯片不上电 | 大小和 FNV 哈希进日志 | 模型 + FAT 卷 |
| 402 | 可选换速：关 RFR，发码，等 20 ms，改 RCG，开 RFR | `[bt] baud switched` | 跟踪 |
| 403 | 外设：仲裁 → ELF → 取板号 → NVM → 构建信息 → 复位 → 通知 | 补丁通知状态 0 | 跟踪回放 |
| 404 | `0xfd` → TME-L 仲裁 → 取版本 → ELF → 通知 | 补丁通知状态 0 | 跟踪回放 |
| 405 | `ActivateSS(BT)` | 激活完成事件 | 跟踪回放 |
| 406 | EDL 取版本，BT ELF | 末段应答状态 0 | 跟踪回放 |
| 407 | 取板号，NVM 逐段等 | 每段应答状态 0 | 跟踪回放 |
| 408 | 版本串、附加特性、读 OTP、日志设置、CoP、`HCI_Reset` | `HCI_Reset` 成功 | 跟踪回放 |
| 409 | `Read_Local_Version`、`Read_BD_ADDR` | `0e 0000 0e 1d00 …`，地址等于 Tag 2，版本串含 `BTFW.BRAHMA` | 跟踪 |
| 501 | LE 被动扫描 60 s | 周围有设备时计数 > 0 | 无 |
| 601 | 阶段 4：再唤一次（Android 在 `HCI_Reset` 之后第一次发送之前也发 `0xfd`），串口交给桥 | `[bt] handover wake`，`[bt] bridge up` | 跟踪 |

阶段 1 到步 303，阶段 2 是 401、101..303、402..409，阶段 3 再到 501，阶段 4 走完阶段 2 之后到 601 并留在桥上。失败时 `pagani_bt_status[0]` 是负的步号，`[1]` 是错误码（`BtDownload.h` 的 `PbError`），其余状态字见 `PaganiBt.h`。

## 启动参数

只有一个参数，`pagani-bt`，所有变化都编码在值里：命令行空间紧，一个参数最省。默认关。

| 写法 | 含义 | 占用字节（含一个空格） |
| --- | --- | --- |
| `pagani-bt=1` | 阶段 1：到步 303 | 12 |
| `pagani-bt=2` | 阶段 2：115200 全程下载，约 30 s | 12 |
| `pagani-bt=3` | 阶段 3：再加 LE 扫描 | 12 |
| `pagani-bt=23`、`=33` | 阶段 2、3，下载用 3 Mbps | 13 |
| `pagani-bt=28`、`=38` | 阶段 2、3，下载用 8 Mbps | 13 |
| `pagani-bt=48` | 阶段 4：下载用 8 Mbps，之后留给 `bluetoothd`。正式用这个 | 13 |
| `pagani-bt=43`、`=4` | 阶段 4，串口 3 Mbps、115200。115200 下载要 30 s，`bluetoothd` 会先崩一两轮再接上 | 13、12 |
| `pagani-bt=40` | 阶段 4 的替身：发布给 iOS 的东西全有，不碰任何硬件，不起线程 | 13 |
| `pagani-bt=S`（S 是表里的步号，101..501） | 跑到这一步停 | 14 |

其他值都当关，写一行 `[bt] pagani-bt value not understood`。命令行上限 1020，完整包 998 字节，任何一种写法都放得下。

## 轨道规则

七路 RPMh VRM（`kPbRails`）：L3F 1.8 V、L2F 1.2 V、L8B 2.8 V、S4D 0.876 V、S5F 0.876 V、S3G 1.86 V、S7I 1.312 V，cnss2 的顺序，和 PaganiWifi 同一张表（`Test/tables.cpp` 比对两边一致）。L8B 蓝牙节点没有列，是不是必须未知，按报告一起投。走 Adreno830 平台层的 `Sm8750CmdDbVrm`、`Sm8750RpmhWriteActive`，DRV-2 锁字串行，和 Wi-Fi 同值，投票幂等。

**S7I 同时是 UFS vccq 的上级**，规则照 Wi-Fi，并且更严：

- 读不到不投：投票前读 S7I，读不到时一路都不投（步 201，`PbErrRailRead`）。Wi-Fi 有 `pagani-wifi-s7i=1` 的绕过，这里没有。
- 只升不降：只在低于 1312 mV 时投电压，绝不比读到的低。
- 总投高功率模式：设备树让它停在低模式，要 200 mA 的负载投票才进 HPM，这个内核不投负载。
- 绝不 `enable=0`：这份代码没有任何一条路径发使能 0。
- 投后读回：读不到、比原来低、掉了使能、低于 1312 mV，都停在步 201。Wi-Fi 低于所需时只告警，这里停：**任何异常都停在 `BT_EN` 之前**。

`rfclka1` 只投开。其他轨道读不到时仍投，绝不比读到的低，和 Wi-Fi 相同。

投票寄存器 `0x52010` 目前没有别的 XNU 驱动在用，仍然走锁字 `sm8750_gcc_vote_owner` 和关中断的读改写。分支没起来时绝不读 SE：读没时钟的块会挂总线。

## 下载流程和回放证明

顺序是本机 HAL（`bt-impl-qti.so`，`PeriPatchDLManager::PeriSocInit`、`PatchDLManager::SocInit`）的调用顺序，`BtDownload.h` 开头有总表。`Test/replay.c` 用手机上拷下来的真固件跑这条序列，`Test/replay-hsuart.py` 和 Android 的 hsuart 跟踪比对。固件和跟踪只在本地读，不进被跟踪路径，缺文件时测试跳过并说明。

回放结果和它的局限，都要看：

```
生成：1371 次写，336740 字节；占位 4380 字节
跟踪：HCI_Reset 之前发送记录 157 条，其中单字节 IBS 在序列里 2 条、额外 3 条
比对：记录覆盖 35977 字节（10.7%），其中逐字节比过 2706 字节
缺口：50 处共 300761 字节没有记录（环形缓冲丢失），占位段 19 条只比帧头，单字节非 IBS 记录 12 条（判别力弱）
没有被任何记录碰到的包：1181 个，其中短命令 ['3100f1ff0103']
H4：接收记录全的走成帧（截断的 31 条不喂），IBS 27，帧 126，上交 125，丢弃 1，坏包 0，杂字节 0；发送 83 包，透传 63，就地应答 20
```

- **证明了什么**：HCI_Reset 之前跟踪里的每一条发送记录，都按顺序、在包边界上、在生成流的正确偏移处吻合。偏移是累加全部包头算出来的，所以分段大小、帧头、文件顺序错一点，后面的记录就全对不上。
- **没证明什么**：每条记录只留前 21 字节，逐字节比过的只有 2706 字节；其余靠“同一个文件 + 正确偏移”推断。Tag 17、Tag 27、Tag 2 的改写点都不在可见字节里，只有 HAL 读码支持。
- **`31 00 f1 ff 01 03`（PeriReset）没有跟踪佐证**：HAL 的 `ConfigurePeriSoC` 明确发它，它落在 14.372346 和 14.373538 之间，跟踪里没有这条记录。同一阶段外设 NVM 第 3 段也丢了记录（缺口正好 251 字节），所以按记录丢失处理，序列里保留。
- **校准**：Android 那次带了 `calibration.bin`（4380 字节，手机 data 分区，这里没有）。回放用全零占位，这部分的 19 条记录只比帧头。XNU 读不到校准，发的是不带容器头的纯 NVM，这条路径 Android 只在首次开机走，跟踪里没有。
- **接收格式**：`0x34` 事件没有真机样本，跟踪在 15.11 s 之前的 RX 全丢了。`pb_peri_parse` 和控制器模型都来自 HAL 的解析代码，第一轮上机的 `[bt] rx` 行才是证据。

## 对路线报告的更正和补充

| 报告 | 实际 | 依据 |
| --- | --- | --- |
| 握手表 `GET_BUILD_INFO` 后直接到 `fd` | 中间还有 `31 00 f1 ff 01 03`（PeriReset）和等补丁通知，跟踪丢了这条记录 | HAL `ConfigurePeriSoC` |
| “每段应答 `04 0e 05 …`” | 只有 NVM 每段答；ELF（外设、TME-L、BT）不逐段等，只等最后一段（`SKIP_EVT_VSE_CC`，属性没设时 HAL 强制 3） | HAL `TlvDnldReq`，跟踪里 ELF 期间 DMA 连发 |
| “HAL 把校准拼在后面” | 还多一个容器头 `04 c7 44 00`，整个 NVM 往后错 4 字节；没有校准文件时没有这个头 | HAL `GetTlvFile`，跟踪首段 |
| 只提到 Tag 2 | HAL 还改 Tag 17 的 `data[1]` 为波特率码（BT NVM 原值 `0x11`，外设 NVM 原值 `0xfd`，都改成 `0x15`）、Tag 27 的 `data[1]` bit 0、Tag 174 的 `data[0]` bit 0；Tag 155 那条只对 hamilton（类型 7），ganges 是 8，不适用 | HAL `ReadTlvInfo`、`UpdateNewNvmFormat`、`GetSocTypeInt` |
| “发送前若对端睡眠，先 `0xFD`” | 两个方向各管各的：控制器发 `0xfe` 只表示它不再发了，主机不用重新唤醒。跟踪 15.399870 收到 `fe`，15.523526 主机直接发数据 | 跟踪，HAL `ProcessIbsCmd` |
| 产品号 33 | 控制器原始值可能是 30（`0x1e`），HAL 在 `sec_soc=brahma` 时把 30 映射成 33，判据不能写死 33 | HAL `ProcessPeriEdlPatchEvt` |
| 步 302 “收到 `0xFC`” 是 HAL 的判据 | HAL 试满 10 次没收到也当成功往下走；这里按报告的判据当失败，便于定位 | HAL `WakeUpSoC` |
| RCG、CBCR、投票位只来自主线 | pagani 的 UEFI `ClockDxe.efi` 时钟表给出同样的值和完整 16 项频率表，已核对（基址偏移 0x10000） | `PaganiBtBoard.h` |
| “`ctl_connect` 时确保固件已下载，失败则断开” | 这样会崩溃循环：传输打不开是 abort，不是报蓝牙关闭。连接里也不能等，它持着协议栈大锁。改成连接照收、命令留着，失败时撤销服务再杀进程 | `sub_10016BBC8`、`sub_1000DDAA8` 的调用路径 |
| 只提 `bluetooth` 的 MobileGestalt 键 | `MobileBluetooth` 客户端还有自己的闸门：`bluetooth` 为真、IOKit 里有服务，否则 `BTSessionAttachWithQueue` 返回 9。这是守卫收尾的依据 | `MobileBluetooth` |
| “配对数据在 `ledevices.paired.db` 和偏好” | 那只是设备列表。Link Key、LTK、IRK 都在系统钥匙串，保护类是每次开机新生成的软件钥匙包，所以持久 Data 单独不够 | `_BTKCSetData`、`NativeKeyStoreEvents/README.md` |
| `transport-speed=8000000` | 要写成 OSData，`CFDataGetBytes` 取 4 字节，低于 115200 左右的值被当 0 | `sub_10015FC40` |
| “VSC 超时 15 s” | 两层：同步等完成 15 s（`sub_100046308`），命令的期望事件 16 s（配置表 46 的 +6）。普通命令 10 s | `sub_100055E20` |

另外，HAL 对 ganges 还会尝试下发许可证文件 `cnss_softsku_peach.pfm`，跟踪里这一步没有发出任何字节，这里不做。

## 构建

`scripts/build-pagani-bt.py`（组件配方，工作目录是构建树）重放树里的 XNU 编译记录，零警告，iOS 17.0 的 ARM64 目标文件，生成 `build/pagani-bt/libPaganiBt.a`；`--append-to build/adreno830/libAdreno830.a` 加进强制加载的归档，检查平台符号在、没有重复的强符号。FAT 读取器编成 `pagani_bt_fat_*`，因为 PaganiAdsp、PaganiWifi 的副本在同一个归档里。

单独验证编译，构建树只读：

```
python3 scripts/build-pagani-bt.py --tree <树> --adreno830 ../../../Adreno830 --out <临时目录>
```

## 阶段 4：接 `bluetoothd`

证据等级同报告。下面的读码都是 21H461 的 `/usr/sbin/bluetoothd`（sha256 `3f1e4493…`）、`MobileBluetooth.framework` 和 XNU 的 `kern_control.c`，一条都没上过机。

### 接线

`pagani_bt_start` 在 `bsd_autoconf` 里同步做完这些（`PaganiBtHost.cpp`），launchd 还没起，也不碰硬件：

| 东西 | 内容 | 依据 |
| --- | --- | --- |
| 控制名 `com.apple.uart.sk.bluetooth` | `SOCK_STREAM`，不设 `CTL_FLAG_PRIVILEGED`（`bluetoothd` 以 mobile 运行）。不接 `getopt`、`setopt`：它的 `TIOCGETA`、`TIOCSETA` 不看返回值 | `sub_1000DDAA8` |
| IOService `bluetooth`，挂在 IOResources 下 | `local-mac-address`（书写顺序，线上的反序）、`transport-encoding`=1（H4）、`transport-speed`=串口波特率，都是 OSData：`bluetoothd` 用 `CFDataGetBytes` 取，给 OSNumber 会崩 | `start`、`sub_10009ABC0`、`sub_10015FC40`、`sub_10015FD98` |
| `/chosen/mac-address-bluetooth0` | MobileGestalt 的 `BluetoothAddressData`，由它派生 `bluetooth` | `inventory/ios.md` 第 3 节 |
| `/product/bluetooth-le` | MobileGestalt 的 `bluetooth-le`，有属性就算真（`0x1a02123e0` → `0x1a020fa68`），写 u32 1 | 同上 |

地址由 `/chosen/unique-chip-id` 派生（`pb_bdaddr_from_seed`），和写进 NVM Tag 2 的是同一个，每次开机相同。种子里没有 MobileGestalt 缓存，所以开机现算，不用清缓存【实测：`s7-final` 只读挂载核对】。

`bluetoothd` 这一侧要两条 `text-fixups`（`integration/text-fixups.txt`，`check-pagani-bt-host.py` 的 `fixups` 项对只读根卷核对原字节和指令）：

- F2 `sub_1002EF860` → `return 0`：不跑 `BlueTool`。`sub_1002F0534`（boot）和 `sub_1002F0DE4`（init）都因为 `sub_1002F05F4` 返回 11 走到这里，返回 0 就算引导成功。
- F3 `strcmp` 之后的 `CBZ` → `NOP`：不走 Skywalk，落到普通 `read`/`write`。

`write` 的循环见 `sub_1000DE46C`：写失败返回 1205，所以 `ctl_send` 永远收下；`read` 返回 0 只记一行 `BM3UART read failed`，读线程悄悄退出。写是流式的，`BtH4.c` 的 `pb_h4_tx_byte` 先切包。厂商命令在 `ctl_send` 里当场回，不经过工作线程和串口。

### 轮询

报告说阶段 4 打开前要把空闲轮询降下来，这里没有常驻 1 ms：有包要发时不睡，有字节来往或等命令应答时 1 ms 一轮，安静 64 ms 后 16 ms 一轮，没有客户端时 100 ms 一轮。协议栈写东西时直接叫醒工作线程。空闲时收方向的延迟最多多 16 ms，RFR 硬件流控保证不丢；控制器等 `0xfc` 能等多久没有数据，它发 `0xfd` 之后最多 16 ms 才有回音，要上机看。中断是阶段 7 的事。

### `bluetoothd` 卡住时不拖住 SpringBoard

先看它什么时候会卡，全部【读码】：

- 只有 `MobileBluetooth` 的调用是同步 XPC（`sendMessageWithReplySync`），而且只在 `BTSessionAttachWithQueue` 成功之后才有；attach 本身是异步的。这个函数开头有个闸门：`MGGetBoolAnswer("bluetooth")` 为真、不是模拟器、IOKit 里有 `marconi-bt` 或 `bluetooth` 服务，缺一个就直接返回 9，一条 XPC 都不发。CoreBluetooth 全是异步。
- `bluetoothd` 的 `start`：找不到服务就 `errx(0, "No bluetooth on this device.")`，退出码 0，launchd 不重开。
- 传输打不开（`sub_10016BBC8`）、初始化表任何一步失败（除了 Reset）都走 `sub_100107728`，直接 abort，不是优雅地报蓝牙关闭。所以**内核拒绝连接不是安全状态**，会变成崩溃循环。
- 运行中命令 10 s 没应答（配置表 46 的 +3，厂商命令 16 s）：`HCI stall detected` 然后走协处理器转储，再崩溃；启动状态下只是这一步失败。协议栈大锁连续占住约 30 s 时苹果自己的看门狗 abort（`sub_100076154`，10 s 一跳，第三跳）。

所以卡住的窗口是“已 attach 的客户端在等一个等 HCI 的 `bluetoothd`”，最坏十几秒。保护做在内核，三层：

1. **HCI 不让它等**：厂商命令当场回；发下去的命令 4 s 没有应答就判定控制器死了，比它的 10 s 先到。串口写失败、控制器报崩溃转储同样判定。
2. **它自己卡住也有上限**：连上之后 30 s 走不到初始化表最后一步（`0x0c5b`），或者连续 5 个 `bluetoothd` 走不完初始化就退了（崩溃循环），同样 trip。阶段 2 失败直接 trip。
3. **trip 之后的状态和没开阶段 4 一样**：撤掉 `bluetooth` 服务、`mac-address-bluetooth0`、`bluetooth-le`，再 SIGKILL 连着的进程，顺序不能反。同步 XPC 立刻出错返回；launchd 重开的 `bluetoothd` 在 `start` 里退出码 0 不再重开；`BTSessionAttachWithQueue` 返回 9，客户端不再碰 XPC；以后的连接一律拒绝。这次开机蓝牙作废，不重试，下一次开机重来。

控制器还没起来时连接照收，命令留在发送环里，连接里绝不等：`connect` 时它持着协议栈大锁。launchd 在开机约 14 s 才开始起守护进程【实测：上一轮 `stage15-smoke` 的时间线】，8 Mbps 的下载这时应该已经完成【推断】；如果没有，`bluetoothd` 靠自己的计时（Reset 失败它不管，第二条失败就 abort）十几秒后崩溃，launchd 重开时就接上了，这一轮不计入崩溃循环。

没盖住的：`bluetoothd` 卡在连上控制套接字之前，内核看不见。这时它的 XPC 还没服务，attach 是异步的，同步调用不存在，SpringBoard 不受影响【读码推断】。launchd 按需拉起时的忙循环有 10 s 节流。种子开了任务、内核没开阶段 4 时就是这个状态，闸门挡住了同步路径，所以两边不匹配不致命，但别这样配。

### 替身

`pagani-bt=40`：`BtStandIn.c` 当场回 HCI，不碰寄存器，不起线程，用来先单独验证 iOS 这一半（控制套接字、服务、fix-ups、MobileGestalt、SpringBoard），和上电风险完全分开。读类命令的返回值是这台手机在 Android 上答的（跟踪 15.33..15.36 s）；`Read_Local_Supported_Commands` 跟踪只留了前 14 字节，后 50 字节按全支持占位。没有连接，没有扫描结果，ACL 全丢。它只保证走完初始化，初始化之后的命令除了读类都回状态 0，能走多远要上机看。

### HID 配对的持久化依赖

现在 Data 是内存盘，每次开机从种子重建，配对重启就没。键鼠能配对、能用，只是下次开机要重配，对端设备上也要先忘记这台手机（它还留着旧的 Link Key）。要配对活过重启，下面四件都要有，缺一件就白做【读码】：

1. **持久 Data**。`PaganiPersistentData` 和 `NativeRamBlock` 的持久覆盖层都还没上机，写闪存要用户单独授权（`memory-storage.md` 第 7 节）。这是最大的一块。
2. **稳定的钥匙包**，这条最容易漏。配对密钥不在文件里，全在钥匙串：经典 Link Key、LE 的 LTK/IRK、本机地址都走 `_BTKCSetData`，`kSecClassGenericPassword`，服务名 `MobileBluetooth`，访问组 `com.apple.bluetooth`，`kSecUseSystemKeychain`，保护类 `kSecAttrAccessibleAlwaysThisDeviceOnlyPrivate`。`NativeKeyStoreEvents` 的软件钥匙包每次开机从 `read_random` 取类密钥，它的 README 明说不能和持久 Data 一起用：上一次开机的条目解不开，`securityd` 会把它们删掉。要改成从稳定的秘密派生（比如 `unique-chip-id`），否则持久了也白搭。
3. **要保留的路径**：`/var/Keychains/`（钥匙串）、`/var/keybags/`、`/var/mobile/Library/MobileBluetooth/com.apple.MobileBluetooth.ledevices.paired.db` 和 `.other.db`、`/var/mobile/Library/Preferences/` 下的 `com.apple.MobileBluetooth.devices`、`com.apple.MobileBluetooth.ledevices`、`com.apple.MobileBluetooth.SystemSettings`，以及系统组容器 `systemgroup.com.apple.bluetooth`。持久 Data 是整卷保留，这条自然满足；如果只想做窄方案，这就是清单，清单是否完整要上机后看实际写了什么文件。
4. **地址不变**：已满足，`unique-chip-id` 派生，每次开机相同。`bluetoothd` 还把本机地址写进钥匙串（`Failed to write local address to keychain`），将来换地址方案要连钥匙串一起清。

阶段 5 之前不用动这些。先证明能配对、能打字，再谈活过重启。

## 整合

阶段 0 到 3 的组件条目、`board.toml`、`Stage7Probe` 已经在主线。阶段 4 主线要做的（全部不在这个目录）：

1. `UniXNU/components.toml` 的 PaganiBt 条目按 `integration/components.toml.snippet` 更新 `function`、`source_files`、`board_dependencies`。构建配方不变，`scripts/build-pagani-bt.py` 自己列源文件。
2. 种子：`工具/edit-phone-seed-launchd.py --enable com.apple.bluetoothd --trust <种子>-owner-trust.bundle <种子>.dbrdisk <新种子>.dbrdisk`。只改启动策略，插件信任不变。任务只是被禁用，`bluetoothd` 本体在系统卷上。在 `s7-final` 的副本上试过：变的只有 `disabled.plist`，负载余量 4.9 MB。
3. 镜像：`A830_PREBUILT_SEED=<新种子>.dbrdisk A830_OWNER_TRUST=<新种子>-owner-trust.bundle A830_EXTRA_ARGS="pagani-bt=48" A830_EXTRA_FIXUPS="$FIX $(grep -v '^#' drivers/PaganiBt/integration/text-fixups.txt | tr '\n' ' ')"`，`$FIX` 是 `make-stage16.sh` 那些脚本已有的 `vt-fixups.txt`，这两条接在后面，不能替换它。`/chosen/text-fixups` 上限 32 条，算上系统配置的 4 条、Core Image、灵动岛、键盘和 `vt-fixups.txt` 的 2 条，一共 11 条。

三样要一起上：只开种子，`bluetoothd` 退出码 0，没事但没用；只开内核，任务是禁用的，客户端 attach 失败后隔一会儿重试，也没事但没用；少了 fix-ups，`bluetoothd` 过不了 `BlueTool` 或 Skywalk 那一步，反复退出【推断】。默认镜像不加参数，行为不变。

不需要新的 XNU 补丁，不改 defconfig：目标文件只引用静态链接里已有的符号（`ctl_register`、`ctl_enqueuedata`、`mbuf_*`、`proc_signal` 都在现在的 `mach.development.bcm2837` 符号表里，已核对）。固件原地读，不进种子或镜像。

## 测试

```
python3 Test/check-pagani-bt-host.py [--tree 树] [--firmware 目录] [--trace 文件] [--root 只读根卷]
python3 Test/mutate-pagani-bt.py [名字里含这段文字的变异]
```

`check-pagani-bt-host.py` 用 ASan/UBSan 跑全部测试（列表在脚本开头），`bridge-threads` 用 ThreadSanitizer，要 cc、c++、mtools。`mutate-pagani-bt.py` 对关键判据做变异，每个都必须被抓到，并列出哪些只有真实跟踪抓得到。模型是对寄存器语义和 HAL 解析代码的复述，只能证明驱动和这份理解一致。

阶段 4 的假 `bluetoothd`（`Test/bridge.c`）按反编译说话：初始化表实际发的 24 条命令，命令号由描述符表 `0x100925dc0` 核对，一次一条等应答，写按任意边界切开，收按 `sub_1000DF6E4` 的规矩查类型字节和长度。`Test/host_glue.cpp` 把 `PaganiBtHost.cpp` 编到主机上对 `Test/kstub` 的内核替身跑，走真实的回调入口（mbuf 链、被拒时的 disconnect）。回放再把跟踪里 HCI_Reset 之后的 83 包写和接收记录按时间顺序过一遍整个桥：下发 63 包逐包对上，就地回 20 条（10 种厂商命令），上交和丢弃的帧数对上。这些都证明不了 `bluetoothd` 真的走得通，它们只证明这一侧按读码的理解工作。

回放要本地的固件和跟踪，默认在 `build-userspace/a830-inputs/round-1101/bt-local/`（`btfw/`、`hsuart.trace`，这个目录不被跟踪），也可以用 `--firmware`、`--trace` 或环境变量 `PAGANI_BT_FIRMWARE`、`PAGANI_BT_TRACE` 指定。缺文件时回放跳过并说明，其余测试照跑。`fixups` 要只读根卷，默认 `build-userspace/a830-inputs/osmnt-21H461`，缺了只查格式。

## 上机轮次和判据

每轮一个目标，失败就看对应的行，不要一次放开多步。按“修完再测”的约定，这些和别的修复一起打包，不发零碎测试包。开始前按序列号或 `ro.boot.prjname` 确认设备是 13T，USB 上可能是别的手机。首测不和 `pagani-wifi` 同开：`BT_EN` 和 `WLAN_EN` 动作要隔 100 ms，两个线程之间没有共享的时间戳。U 行用 `工具/gpu-test-loop/read-ramoops.py` 读，保留 `pagani-reset-after`。

| 轮 | 参数 | 成功时的 U 行 | 失败时 |
| --- | --- | --- | --- |
| 1 | `pagani-bt=102` | `[bt] survey`、`[bt] pins`、七条 `rail … now` 都有 `mv=`；`[bt] clocks … halts=0x0`；`[bt] geni fw=0x?66 proto=0x2 if_disable=0x0`；`stopped as asked step=102` | `FAIL step=102 err=-14`：分支没起来，看 `halts` 哪一位，多半要投 QUP 的 BCM；`[bt] clocks` 之后没有下一行且机器卡住：SE 读把总线挂了，同样查 QUP BCM；`proto` 不是 2：SE 固件没加载或协议不对，查 `qupfw` |
| 2 | `pagani-bt=201` | `rail smpi7 after … mv=1312`（或更高）`en=0x80 mode=0x7`；`[bt] rfclka1 … rc=0`；能进桌面，UFS 不报错 | `FAIL step=201 err=-12`：S7I 读不到，一路没投；`err=-11 S7I below…`：投了没升上去，看读回值，可能读回不是设定值；`rfclka1 rc=-19`：cmd-db 里不是 VRM 资源；UFS 出错：S7I 规则有问题，停，不再往下 |
| 3 | `pagani-bt=202` | `bt_en low … rc=0`、`bt_en high … rc=0`；`after bt_en … low_ms≥54`，`sw_ctrl` 有变化更好 | `rc=-1`：通道不属于 apps，Gunyah 下 HLOS 没有这个 PMIC 脚的写权限，要换路；`rc=-95`：子类型不对，看 `sub=` |
| 4 | `pagani-bt=1` | `[bt] loopback … ok=1`；`[bt] uart baud=115200 ios=0x1 cts=1`；`[bt] wake fd=1 fc=1`（fd 可以多几次）；`[bt] rx n=… 34 00 ff …`；`peri version product=… soc=0x40210200`。**把 `[bt] rx` 原样抄回来，核对 `BtTlv.h` 的 0x34 布局** | `loopback ok=0` 且 `fc=0`：先修 SE 层，`loopback` 的寄存器语义本身也没验证过；`ok=1`、`cts=0`：控制器没把 CTS 放下，查电和时钟；`cts=1`、`fd=10 fc=0`：UART 对但控制器不答，按顺序查 `rfclka1`、睡眠时钟、PDC 表（`qcom,pdc_init_table`，这版没发）；`FAIL step=303`：收到 `0xfc` 没有 `0x34`，看有没有别的 `[bt] rx` 行 |
| 5（可选） | `pagani-bt=304` | `hci reset probe rc=… status=…`，只看反应 | 不作判据 |
| 6 | `pagani-bt=2` | 五行 `firmware file`，字节数 25420、741、55204、228872、13227；`peri arbitration status=0x0`；`tmel arbitration status=0x0`；`edl version soc=0x40210200`；`[bt] firmware BTFW.BRAHMA…`；`local version 0e 00 00 0e 1d 00 21 7e`；`[bt] HCI up`。全程约 30 s | `FAIL step=401`：分区或 FAT，看 `ufs=`；`step=403/404 err=-3`：等应答超时，看最后的 `[bt] rx`；`err=-4` 和 `segment refused status=`：状态码含义在 HAL `DownloadAndCmdExeStatus`（3 签名错、5 NVM Tag 错、0x15 版本错）；`step=405`：激活没完成；NVM Tag 17 写 0 是这版的推断，115200 下 NVM 阶段出错先怀疑它 |
| 7 | `pagani-bt=23` | 同上，多一行 `baud switched baud=0x2dc6c0 code=0xe` | `step=402`：换速应答没收到，检查 RCG 48 MHz 和 RFR 时序 |
| 8 | `pagani-bt=28` | 同上，`code=0x15`，全程约 2 s；`[bt] uart stats` 的 `odd_tails=0`，`junk` 很小 | `err=-2`：写超时，CTS 长期不放时看 `[bt] uart stats` 的 `m_err`、`s_err`；`err=-3` 且 `junk` 明显变大：轮询赶不上 stale，帧被补位弄坏（见已知限制），先退回 `pagani-bt=23` |
| 9 | `pagani-bt=38` | `scanning adverts=…` 每 10 s 一行；`scan done adverts=N`，周围有手机或手环时 N > 0 | 计数 0 且没有别的错：射频或天线，试把 Wi-Fi 也开着，看 L8B 或共享天线是否依赖 Wi-Fi 一侧 |

| 10 | `pagani-bt=40`，种子开 `bluetoothd`，两条 fix-ups | `[bt] stage 4 published: stand-in`；`[bt] client connected pid=…`（有这行说明 `start`、F2、F3、沙盒都过了）；一串 `[bt] vsc answered locally opcode=…`，**全部抄回来**，这是这颗芯片要回的厂商命令清单；`[bt] stack init finished ms=…`；进桌面不变慢，设置里有蓝牙开关；`diag:` 行 `bluetoothd:` 有 pid | 没有 `client connected`：`bluetoothd` 没到传输，看 `diag:` 有没有 pid、有没有反复重启，再看 `msgbuf.txt` 的 `text-fixup: bluetoothd+…` 两行在不在；`client connected` 之后反复 `client gone ready=0`：HCI 初始化没走完，替身的应答不够，看它崩在哪一步（崩溃日志里的 `HCI initialization failed` 或 `VSC stall`）；SpringBoard 卡住：**这轮最要紧的结果**，抓 `diag:` 和 stackshot，先去掉种子改动回到干净状态再分析 |
| 11 | `pagani-bt=48`，同上 | 第 8 轮的全部 U 行；`[bt] handover wake …`；`[bt] bridge up held_bytes=… client=…`；`client connected`；`stack init finished`；`[bt] bridge …` 每 10 分钟一行，`bad=0`，`out`、`up` 在涨。设置里打开蓝牙能搜到设备 | `[bt] controller refused opcode=… status=…`：控制器不认这条。初始化表里 `0x1003`、`0x0c33`、`0x0c31`、`0x0c5b`、`0x207d` Android 没发过，除了 `0x0c31` 失败 `bluetoothd` 都会 abort，真不认就要在内核代答（这次没做，没有证据）。`[bt] GUARD why=…` 后面跟着最近 16 条命令号：`why=1` 下载失败，看前面的 `FAIL`；`why=2` 串口写失败；`why=3` 命令没应答，最后一条命令号就是它；`why=4` 控制器崩了；`why=5` 初始化没走完；`why=6` 崩溃循环，看第 10 轮同样的地方。之后是 `[bt] stage 4 withdrawn`，这次开机没有蓝牙，系统应该和没开一样 |

每轮结束都有一行 `[bt] uart stats tx_timeouts=… m_err=… s_err=… odd_tails=… junk=…`。`odd_tails` 不是 0 说明 `RX_FIFO_STATUS` 真的出现过“最高位清零、字节数 1..3”，要回头看 `pb_geni_read` 的判法。

过了第 9 轮才有理由开第 11 轮。第 10 轮不碰硬件，可以先跑，也可以和前面的轮次分开跑。

## 已知限制

没有下电路径：轨道和 `BT_EN` 留到重启，设置里关蓝牙只是协议栈发 `HCI_Reset`，控制器保持加载。trip 之后也不重新上电重试。没有 IBS 空闲睡眠（状态机写了，默认关），主机方向常醒，功耗留到阶段 7。LE 扫描后线程结束，不留接收路径；阶段 4 不跑扫描自检。地址由 `/chosen/unique-chip-id` 派生，不是手机出厂地址（Modem NV 读不到）。校准数据不用。厂商命令一律回状态 0x01，有没有哪条是 `bluetoothd` 非要成功的，静态看没有，要看第 10、11 轮。配对重启就丢，见上面的持久化依赖。A2DP 还要删 `BTAudioHALPlugin` 那条 fix-up，那是阶段 6。

串口是单线程轮询：写的时候不取接收 FIFO，靠 RFR 硬件流控顶住，一个最大的包在 8 Mbps 下不到 1.5 ms。控制器要是非得发完才肯收，这条会死锁到写超时，然后 trip，这种情况没见过，只是这样写的代价。

RX 用 4 字节打包等 stale，这是主线和 `HSUartDxe` 的做法，但它们一个走中断、一个只当调试口。这里轮询，半个字出来后还没读走又收满一个字时，`RX_LAST` 说不清前面那半个字，它按整字读出来，多 1 到 3 个补位字节（模型假定是 0，真机未知）。线路空闲才会有半个字，所以补位几乎都落在帧边界，H4 解析器当杂字节跳过，`[bt] uart stats` 的 `junk` 能看到；帧中途停顿超过 stale 才会真的坏一帧。115200 时 stale 1.9 ms、收满一个字 0.35 ms、轮询约 1 ms，碰上的窗口很窄；8 Mbps 时 stale 只有 27 us，这是 `pagani-bt=28`、`=38` 要上机看的事。

## 和 HSUartDxe 的核对

pagani UEFI 的 `HSUartDxe.efi` 是这颗 SoC 上唯一走 FIFO 模式的高通 UART 驱动，用它独立核对了 `BtGeni`：全部寄存器偏移、打包 `0x4380e`/`0xc3e0e`（发送和接收都是）、字长 8、回环值 3、`TX_TRANS_CFG` 写 2 表示不用流控、`M_CMD0`/`S_CMD0` 的 `0x8000000`、FIFO 深度在 `HW_PARAM` 的 23:16、分频字段和 `115200 = 7.3728 MHz ÷ 16 ÷ 4` 都一致。换速时它也是先停接收、改时钟、再开接收。有差别的地方：

| 项 | 主线 | `HSUartDxe` | 这里 |
| --- | --- | --- | --- |
| 接收命令参数 | FIFO 路径 0，DMA 路径 `RFR_OPEN` | 0 | 0 |
| `TX_WATERMARK` | —— | 4 | 4 |
| `RX_WATERMARK` | 2 | 深度减 8 | 深度减 8 |
| `RFR_WATERMARK` | 深度减 2 | 深度减 4 | 深度减 4 |
| `RX_STALE` | 160 | 220 | 220 |
| 半个字的判法 | bit 31 且 30:28 非 0 | 只看 29:28 非 0 | 同主线，另外的情况计 `odd_tails` |

判法选主线：两者只在 bit 31 清零、字段 1..3 时不同，这时按整字取最多多出补位，补位在帧边界能跳过，按字段取却可能丢真字节。Android 跟踪里没有 stale 寄存器的值，`stale_delay` 那几行（10000、3276、540 us）是停接收前的等待时间，不是这个寄存器。
