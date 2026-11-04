// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026, UniXNU contributors. All rights reserved.
// 窗口映射、RPMh、SPMI 和共享锁字的写法沿用 PaganiWifi PaganiWifi.cpp，它又沿用 UniXNU-sheng package/WifiProbe.cpp（commit 08cde20e）。
//
// PaganiBt 的内核一侧（README.md）。pagani_bt_start（Stage7Probe，Wi-Fi 之后）读 pagani-bt=N，没有就立刻返回：什么都不映射、不分配、
// 不启动。有的话起一个 "pagani-bt" 内核线程，把阶段跑完就退出，之后不留轮询：
//   阶段 1  勘察、开时钟、投轨道、BT_EN、UART、0xfd/0xfc、0x31 版本（步 101..303）
//   阶段 2  上电之前先从分区原地读固件（步 401），再走阶段 1，然后下载外设、TME-L、BT 固件和 NVM，HCI_Reset，读版本和地址（步 402..409）
//   阶段 3  LE 被动扫描 60 s，数广播（步 501）
//   阶段 4  先同步发布控制套接字和 bluetooth 服务（PaganiBtHost.cpp），再走阶段 2，然后这个线程留下来当桥的工作线程（BtBridge.h）
// 不 panic：任何失败都是一个步号、一个错误码和一行 [bt] 日志（BootProgress 的 ramoops U 记录，pagani-ramoops=1），线程结束。
// 不写存储。轨道和 BT_EN 留在最后的状态，到重启为止：没有下电路径，轨道绝不关。
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IORegistryEntry.h>
#include <libkern/c++/OSData.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
extern "C" {
#include "BtBringup.h"
#include "BtDownload.h"
#include "BtFirmware.h"
#include "BtUtil.h"
#include "PaganiBt.h"
#include "PaganiBtHost.h"
#include "Sm8750Rpmh.h"
}

// 静态链接的内核不接受未定义的弱引用：可能缺席的提供者在这里给弱定义，真的那个优先。
extern "C" {
__attribute__((weak)) void boot_progress_trace(const char *) {}
__attribute__((weak)) int native_ufs_part_find(const char *, uint32_t *, uint64_t *, uint64_t *) {return 0;}
__attribute__((weak)) int native_ufs_part_read(uint32_t, uint64_t, uint32_t, void *) {return 0;}
__attribute__((weak)) int native_ufs_part_status(void) {return -1;}
__attribute__((weak)) volatile uint32_t sm8750_gcc_vote_owner;
__attribute__((weak)) volatile uint32_t pagani_panel_probe_quiet;      // 不是 0 时 UfsLog.c 的写者停下，按位用（PB_QUIET_BIT）
boolean_t ml_set_interrupts_enabled(boolean_t enable);
volatile int32_t pagani_bt_status[32];
}

namespace {
enum : uint32_t {
    kVoteSteps = 5000,              // 50 ms，每步 10 us
    kFirmwareWaitSeconds = 120,     // 分区来得晚：NativeUfs 就绪之前的等待上限
    kScanSeconds = 60,
};

volatile int32_t *const st = pagani_bt_status;
uint64_t nowMs()
{
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    return ns / 1000000;
}
uint64_t gStartMs;
unsigned argument(const char *name, unsigned fallback)
{
    unsigned value = fallback;
    return PE_parse_boot_argn(name, &value, sizeof value) ? value : fallback;
}
void trace(const char *line)
{
    boot_progress_trace(line);
    IOLog("%s\n", line);
}
void traceKv(const char *what, const char *key1, int64_t v1, const char *key2 = nullptr, int64_t v2 = 0)
{
    char line[160];
    PbText t;
    pb_text_start(&t, line, sizeof line);
    pb_text_str(&t, "[bt] ");
    pb_text_str(&t, what);
    if (key1) pb_text_kx(&t, key1, uint64_t(v1));
    if (key2) pb_text_kx(&t, key2, uint64_t(v2));
    trace(line);
}

// ---- 窗口 ----
struct Window {
    IOMemoryMap *map;
    volatile uint8_t *va;
    uint64_t size;
};
bool mapWindow(Window &w, uint64_t base, uint64_t size)
{
    if (w.va) return true;
    IODeviceMemory *memory = IODeviceMemory::withRange(base, size);
    w.map = memory ? memory->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
    OSSafeReleaseNULL(memory);
    w.va = w.map ? reinterpret_cast<volatile uint8_t *>(w.map->getVirtualAddress()) : nullptr;
    w.size = w.va ? size : 0;
    return w.va != nullptr;
}
uint32_t rd(const Window &w, uint32_t at)
{
    const uint32_t value = *reinterpret_cast<volatile uint32_t *>(w.va + at);
    __asm__ volatile("dmb sy" ::: "memory");
    return value;
}
void wr(const Window &w, uint32_t at, uint32_t value)
{
    *reinterpret_cast<volatile uint32_t *>(w.va + at) = value;
    __asm__ volatile("dsb sy" ::: "memory");
}
Window gWin[PbWinCount], gRsc, gCmdDbWin, gSpmiCore, gSpmiCnfg, gSpmiObs, gSpmiChan;
unsigned gSpmiChanApid = ~0u;
uint32_t gBadAccess;

uint32_t hwRead(void *, unsigned win, uint32_t off)
{
    if (win >= PbWinCount || !gWin[win].va || (off & 3) || uint64_t(off) + 4 > gWin[win].size) {++gBadAccess; return 0xffffffffu;}
    return rd(gWin[win], off);
}
void hwWrite(void *, unsigned win, uint32_t off, uint32_t value)
{
    if (win >= PbWinCount || !gWin[win].va || (off & 3) || uint64_t(off) + 4 > gWin[win].size) {++gBadAccess; return;}
    wr(gWin[win], off, value);
}
void hwUdelay(void *, unsigned us)
{
    if (us >= 1000) IOSleep(us / 1000);
    else IODelay(us);
}
void hwMsleep(void *, unsigned ms) {IOSleep(ms);}
uint64_t hwNow(void *) {return nowMs();}
// 读改写的几次寄存器访问期间关中断：线程在读和写之间被抢占会写回旧位。别的持锁者最多等 50 ms。
boolean_t gVoteInterrupts;
int hwVoteLock(void *, int take)
{
    if (!take) {
        uint32_t expected = PB_GCC_OWNER;
        __atomic_compare_exchange_n(&sm8750_gcc_vote_owner, &expected, 0u, false, __ATOMIC_RELEASE, __ATOMIC_RELAXED);
        ml_set_interrupts_enabled(gVoteInterrupts);
        return 0;
    }
    for (uint32_t step = 0; step < kVoteSteps; ++step) {
        const boolean_t enabled = ml_set_interrupts_enabled(FALSE);
        uint32_t expected = 0;
        if (__atomic_compare_exchange_n(&sm8750_gcc_vote_owner, &expected, PB_GCC_OWNER, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
            gVoteInterrupts = enabled;
            return 0;
        }
        ml_set_interrupts_enabled(enabled);
        IODelay(10);
    }
    return -16;
}

// RPMh 走 Adreno830 的 SM8750 平台层：每个请求一条命令，等完成；DRV-2 由它自己的锁字串行，和 Wi-Fi、GPU 共用，投票幂等。
uint32_t ioRead(void *context, uint32_t offset) {return rd(*static_cast<Window *>(context), offset);}
void ioWrite(void *context, uint32_t offset, uint32_t value) {wr(*static_cast<Window *>(context), offset, value);}
void ioDelay(void *, unsigned us) {hwUdelay(nullptr, us);}
const Sm8750RpmhIo kDrv2 = {&gRsc, ioRead, ioWrite, ioDelay}, kCmdDbIo = {&gCmdDbWin, ioRead, ioWrite, ioDelay};
Sm8750CmdDb *gDb;
int gDbState = 1;                   // 1 没读过，0 读过，负数是拷贝的错误
int cmdDb()
{
    if (gDbState <= 0) return gDbState;
    gDb = static_cast<Sm8750CmdDb *>(IOMallocData(sizeof(Sm8750CmdDb)));
    if (!gDb) return gDbState = -12;
    if (!mapWindow(gCmdDbWin, PB_CMD_DB, PB_CMD_DB_BYTES) || !mapWindow(gRsc, PB_RSC_DRV2, PB_RSC_BYTES)) return gDbState = -14;
    gDbState = Sm8750CmdDbCopy(&kCmdDbIo, gDb);
    traceKv("cmd-db copied", "rc", gDbState, "drv2", rd(gRsc, 0));
    return gDbState;
}
int hwVrm(void *, const char *resource, uint32_t reg, uint32_t value, uint32_t *address)
{
    int rc = cmdDb();
    if (rc) return rc;
    uint32_t base = 0;
    if ((rc = Sm8750CmdDbVrm(gDb, resource, &base))) return rc;
    if (address) *address = base;
    Sm8750RpmhCmd cmd = {base + reg, value};
    Sm8750RpmhResult result = {};
    rc = Sm8750RpmhWriteActive(&kDrv2, &cmd, 1, 1, &result);
    if (rc) {
        char line[160];
        PbText t;
        pb_text_start(&t, line, sizeof line);
        pb_text_str(&t, "[bt] rpmh ");
        pb_text_str(&t, resource);
        pb_text_kx(&t, "addr", base + reg);
        pb_text_kx(&t, "data", value);
        pb_text_kd(&t, "rc", rc);
        pb_text_kd(&t, "step", result.step);
        trace(line);
    }
    return rc;
}

// PMIC：observer 读，通道页按需映射一个。
uint32_t spmiCore(void *, uint32_t off) {return gSpmiCore.va && off + 4 <= gSpmiCore.size ? rd(gSpmiCore, off) : 0;}
uint32_t spmiCnfg(void *, uint32_t off) {return gSpmiCnfg.va && off + 4 <= gSpmiCnfg.size ? rd(gSpmiCnfg, off) : 7;}
uint32_t spmiObsRead(void *, uint32_t off) {return gSpmiObs.va && off + 4 <= gSpmiObs.size ? rd(gSpmiObs, off) : 0xe;}
void spmiObsWrite(void *, uint32_t off, uint32_t value) {if (gSpmiObs.va && off + 4 <= gSpmiObs.size) wr(gSpmiObs, off, value);}
bool spmiChannel(unsigned apid)
{
    if (gSpmiChanApid == apid && gSpmiChan.va) return true;
    if (gSpmiChan.map) {gSpmiChan.map->release(); gSpmiChan = {};}
    gSpmiChanApid = apid;
    return mapWindow(gSpmiChan, PB_SPMI_CHAN + uint64_t(apid) * PB_SPMI_CHAN_STRIDE, PB_SPMI_CHAN_STRIDE);
}
uint32_t spmiChanRead(void *, unsigned apid, uint32_t off) {return spmiChannel(apid) && off + 4 <= PB_SPMI_CHAN_STRIDE ? rd(gSpmiChan, off) : 0xe;}
void spmiChanWrite(void *, unsigned apid, uint32_t off, uint32_t value)
{
    if (spmiChannel(apid) && off + 4 <= PB_SPMI_CHAN_STRIDE) wr(gSpmiChan, off, value);
}
void spmiDelay(void *, unsigned us) {IODelay(us);}
const PbSpmiIo kSpmi = {nullptr, spmiCore, spmiCnfg, spmiObsRead, spmiObsWrite, spmiChanRead, spmiChanWrite, spmiDelay};
bool spmiMapped()
{
    return mapWindow(gSpmiCore, PB_SPMI_CORE, PB_SPMI_CORE_BYTES) && mapWindow(gSpmiCnfg, PB_SPMI_CNFG, PB_SPMI_CNFG_BYTES) &&
           mapWindow(gSpmiObs, PB_SPMI_OBS, PB_SPMI_OBS_BYTES);
}
int hwPmic(void *, uint16_t ppid, uint8_t *status, uint8_t *enable, uint8_t *mode, uint16_t *mv)
{
    if (!spmiMapped()) return -14;
    return pb_pmic_rail(&kSpmi, ppid, status, enable, mode, mv);
}
int hwSleepClock(void *, int write)
{
    if (!spmiMapped()) return -14;
    PbGpioState s;
    const int rc = pb_pmic_sleep_clock(&kSpmi, write, &s);
    char line[160];
    PbText t;
    pb_text_start(&t, line, sizeof line);
    pb_text_str(&t, "[bt] pmk8550 gpio3");
    pb_text_kd(&t, "apid", s.apid);
    pb_text_kd(&t, "owner", s.owner);
    pb_text_kx(&t, "mode", s.regs[2]);
    pb_text_kx(&t, "src", s.regs[5]);
    pb_text_kx(&t, "en", s.regs[7]);
    pb_text_kd(&t, "written", s.written);
    pb_text_kd(&t, "rc", rc);
    trace(line);
    return rc;
}
int hwBtEn(void *, int level, PbGpioState *state)
{
    if (!spmiMapped()) return -14;
    return pb_pmic_bt_en(&kSpmi, level, state);
}
void hwLog(void *, const char *line) {trace(line);}
void hwQuiet(void *, unsigned on)
{
    if (on) __atomic_fetch_or(&pagani_panel_probe_quiet, PB_QUIET_BIT, __ATOMIC_RELEASE);
    else __atomic_fetch_and(&pagani_panel_probe_quiet, ~PB_QUIET_BIT, __ATOMIC_RELEASE);
}
const PbHw kHw = {nullptr, hwRead, hwWrite, hwUdelay, hwMsleep, hwNow, hwVoteLock, hwVrm, hwPmic, hwSleepClock, hwBtEn, hwLog, hwQuiet};

PbBringup gB;
PbDl gDl;

// ---- 下载的字节通路：GENI，轮询 ----
int xWrite(void *, const uint8_t *data, uint32_t bytes, unsigned timeout_ms) {return pb_geni_write(&gB.geni, data, bytes, timeout_ms);}
uint32_t xRead(void *, uint8_t *data, uint32_t max) {return pb_geni_read(&gB.geni, data, max);}
void xSleep(void *, unsigned ms) {IOSleep(ms);}
uint64_t xNow(void *) {return nowMs();}
int xRfr(void *, int ready)
{
    pb_geni_flow(&gB.geni, 1, ready);
    return 0;
}
int xBaud(void *, uint32_t baud) {return pb_uart_baud(&gB, baud);}
void xLog(void *, const char *line) {trace(line);}
const PbXport kXport = {nullptr, xWrite, xRead, xSleep, xNow, xRfr, xBaud, xLog};

// 阶段 4：下载结束后串口交给桥，同一个线程在用，不用锁。
extern "C" int pagani_bt_uart_write(const uint8_t *data, uint32_t bytes, unsigned timeout_ms) {return pb_geni_write(&gB.geni, data, bytes, timeout_ms);}
extern "C" uint32_t pagani_bt_uart_read(uint8_t *data, uint32_t max) {return pb_geni_read(&gB.geni, data, max);}

// ---- 固件：分区原地读 ----
int diskFind(void *, const char *name, uint32_t *lun, uint64_t *first, uint64_t *blocks) {return native_ufs_part_find(name, lun, first, blocks);}
int diskRead(void *, uint32_t lun, uint64_t lba, uint32_t blocks, void *dst) {return native_ufs_part_read(lun, lba, blocks, dst);}
void *fwAlloc(void *, size_t bytes) {return IOMallocData(bytes);}
void fwFree(void *, void *memory, size_t bytes) {if (memory) IOFreeData(memory, bytes);}
const PbFwIo kFwIo = {nullptr, diskFind, diskRead, fwAlloc, fwFree};
PbFw *gFw;

int firmwareRead()
{
    gFw = static_cast<PbFw *>(IOMallocData(sizeof(PbFw)));
    if (!gFw) return PbFwNoMemory;
    pb_fw_init(gFw, &kFwIo);
    int rc = PbFwNoPartition;
    unsigned failed = 0;
    for (unsigned waited = 0; waited <= kFirmwareWaitSeconds; waited += 2) {
        rc = pb_fw_load(gFw, &failed);
        if (rc != PbFwNoPartition) break;
        if (waited % 30 == 0) traceKv("waiting for the firmware partitions", "ufs", native_ufs_part_status(), "seconds", waited);
        IOSleep(2000);
    }
    if (rc) {
        traceKv("firmware not read", "rc", rc, "file", failed);
        return rc;
    }
    for (unsigned i = 0; i < PbFwCount; ++i) {
        char line[160];
        PbText t;
        pb_text_start(&t, line, sizeof line);
        pb_text_str(&t, "[bt] firmware file");
        pb_text_kd(&t, "index", i);
        pb_text_kd(&t, "bytes", gFw->files[i].size);
        pb_text_kx(&t, "fnv", gFw->files[i].hash);
        pb_text_char(&t, ' ');
        pb_text_str(&t, gFw->files[i].partition);
        trace(line);
    }
    return 0;
}

// 本机地址：/chosen/unique-chip-id 派生（BtTlv.c），手机自己的地址在 Modem NV 里，这里读不到。
uint8_t gAddress[6];
bool boardAddress()
{
    IORegistryEntry *chosen = IORegistryEntry::fromPath("/chosen", gIODTPlane);
    OSData *id = chosen ? OSDynamicCast(OSData, chosen->getProperty("unique-chip-id")) : nullptr;
    uint64_t seed = 0;
    if (id && id->getLength() >= sizeof seed) memcpy(&seed, id->getBytesNoCopy(), sizeof seed);
    OSSafeReleaseNULL(chosen);
    return pb_bdaddr_from_seed(seed, gAddress) != 0;
}

void setStep(int step, int error)
{
    st[0] = step;
    st[1] = error;
}
void publish()
{
    st[5] = int32_t((nowMs() - gStartMs) / 1000);
    st[6] = int32_t(gB.info.fw);
    st[7] = int32_t(gB.ios);
    st[8] = int32_t(gDl.ibs.acks_seen);
    st[9] = gDl.peri_status;
    st[10] = gDl.tmel_status;
    st[11] = int32_t(gDl.peri_version.soc ? gDl.peri_version.soc : gDl.edl_version.soc);
    st[12] = int32_t(gDl.peri_version.product ? gDl.peri_version.product : gDl.edl_version.product);
    st[13] = int32_t(gB.geni.tx_timeouts);
    st[14] = int32_t(gB.geni.m_sticky);
    st[15] = int32_t(gB.geni.s_sticky);
    st[16] = int32_t(gDl.adverts);
    st[17] = int32_t(gB.geni.rx_odd_tails);
    st[18] = int32_t(gDl.h4.junk);
}

int phaseOne()
{
    int rc = pb_survey(&gB);
    if (!rc) rc = pb_clocks(&gB);
    if (!rc) rc = pb_power(&gB);
    if (!rc) rc = pb_bt_enable(&gB);
    if (!rc) rc = pb_uart(&gB);
    setStep(gB.step, gB.error);
    if (rc) return rc;
    rc = pb_dl_wake(&gDl);
    if (!rc) rc = pb_dl_version(&gDl);
    if (!rc && gB.stop == PbStepHciProbe) rc = pb_dl_hci_probe(&gDl);
    setStep(gDl.step, gDl.error);
    return rc;
}

// 阶段 2 在上电之前读固件：读不到时芯片根本不上电；HAL 握手后紧接着就仲裁，控制器不该空等分区。
int firmwareStep()
{
    if (int rc = firmwareRead()) {
        setStep(-PbStepFiles, rc);
        trace("[bt] FAIL step=401: firmware not read, nothing powered");
        return PbErrFirmware;
    }
    setStep(PbStepFiles, 0);
    return gB.stop == PbStepFiles ? PbErrStopped : 0;
}

int phaseTwo()
{
    PbDlConfig &c = gDl.cfg;
    c.peri = gFw->files[PbFwPeri].data;
    c.peri_bytes = gFw->files[PbFwPeri].size;
    c.peri_nvm = gFw->files[PbFwPeriNvm].data;
    c.peri_nvm_bytes = gFw->files[PbFwPeriNvm].size;
    c.tmel = gFw->files[PbFwTmel].data;
    c.tmel_bytes = gFw->files[PbFwTmel].size;
    c.bt = gFw->files[PbFwBt].data;
    c.bt_bytes = gFw->files[PbFwBt].size;
    c.bt_nvm = gFw->files[PbFwBtNvm].data;
    c.bt_nvm_bytes = gFw->files[PbFwBtNvm].size;
    c.bdaddr = boardAddress() ? gAddress : nullptr;
    if (!c.bdaddr) trace("[bt] WARN no unique-chip-id: the NVM's placeholder address stays");
    int rc = pb_dl_boot(&gDl);
    if (!rc) rc = pb_dl_check(&gDl);
    setStep(gDl.step, gDl.error);
    return rc;
}

void btThread(void *parameter, wait_result_t)
{
    thread_set_thread_name(current_thread(), "pagani-bt");
    gStartMs = nowMs();
    const PbArg arg = *static_cast<const PbArg *>(parameter);
    st[3] = int32_t(arg.phase);
    st[4] = int32_t(arg.baud);
    traceKv("thread started", "phase", arg.phase, "stop", arg.stop);
    for (unsigned w = 0; w < PbWinCount; ++w)
        if (!mapWindow(gWin[w], kPbWinBase[w], kPbWinBytes[w])) {
            setStep(-PbStepSurvey, -14);
            trace("[bt] FAIL a register window was not mapped");
            return;
        }
    gB.hw = &kHw;
    gB.stop = arg.stop;
    PbDlConfig config = {};
    config.baud = arg.baud;
    config.stop = arg.stop;
    pb_dl_init(&gDl, &kXport, &config);
    int rc = arg.phase >= 2 ? firmwareStep() : 0;
    if (!rc) rc = phaseOne();
    if (!rc && arg.phase >= 2) rc = phaseTwo();
    if (!rc && arg.phase == 3) {
        rc = pb_dl_le_scan(&gDl, kScanSeconds);
        setStep(gDl.step, gDl.error);
    }
    if (arg.phase == 4) {
        if (!rc) {
            rc = pb_dl_handover(&gDl);
            setStep(gDl.step, gDl.error);
        }
        publish();
        if (rc) pagani_bt_host_failed();
        else pagani_bt_host_run(&gDl.h4, gDl.rx + gDl.rx_pos, gDl.rx_len - gDl.rx_pos);
    }
    publish();
    if (gBadAccess) traceKv("WARN accesses outside the windows", "count", gBadAccess);
    char line[160];
    PbText t;
    pb_text_start(&t, line, sizeof line);
    pb_text_str(&t, "[bt] uart stats");
    pb_text_kd(&t, "tx_timeouts", st[13]);
    pb_text_kx(&t, "m_err", uint32_t(st[14]));
    pb_text_kx(&t, "s_err", uint32_t(st[15]));
    pb_text_kd(&t, "odd_tails", st[17]);
    pb_text_kd(&t, "junk", st[18]);
    trace(line);
    pb_text_start(&t, line, sizeof line);
    pb_text_str(&t, rc == PbErrStopped ? "[bt] stopped as asked" : "[bt] thread ends");
    pb_text_kd(&t, "step", st[0]);
    pb_text_kd(&t, "err", st[1]);
    pb_text_kd(&t, "seconds", st[5]);
    trace(line);
}

volatile uint32_t gStarted;
PbArg gArg;
}  // namespace

extern "C" void pagani_bt_start(void)
{
    const unsigned value = argument("pagani-bt", 0);
    if (!value) return;
    st[2] = int32_t(value);
    pb_arg_parse(value, &gArg);
    if (!gArg.phase) {
        trace("[bt] pagani-bt value not understood: off");
        return;
    }
    uint32_t expected = 0;
    if (!__atomic_compare_exchange_n(&gStarted, &expected, 1u, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return;
    if (gArg.phase == 4) {
        const int rc = boardAddress() ? pagani_bt_host_publish(gAddress, gArg.baud, gArg.stand_in) : -2;
        if (rc) {
            setStep(-PbStepHandover, rc);
            traceKv("FAIL stage 4 not published: off", "rc", rc);
            return;
        }
        traceKv(gArg.stand_in ? "stage 4 published: stand-in, no hardware" : "stage 4 published", "baud", gArg.baud);
        if (gArg.stand_in) {
            setStep(PbStepHandover, 0);
            return;
        }
    }
    thread_t thread = nullptr;
    if (kernel_thread_start(btThread, &gArg, &thread) != KERN_SUCCESS) {
        setStep(-PbStepSurvey, -7);
        trace("[bt] FAIL no thread");
        return;
    }
    thread_deallocate(thread);
}
