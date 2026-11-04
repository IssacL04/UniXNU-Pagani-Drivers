// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026, UniXNU contributors. All rights reserved.
// 窗口映射、RPMh、SPMI 和保持字的写法沿用 PaganiBt PaganiBt.cpp（它沿用 PaganiWifi PaganiWifi.cpp 和 UniXNU-sheng package/WifiProbe.cpp，commit 08cde20e）。
//
// PaganiCamera 的内核一侧（README.md）。pagani_camera_start（Stage7Probe，蓝牙之后）读 pagani-cam=N，没有就不碰硬件：什么都不映射、不分配、不启动。有的话起一个
// "pagani-cam" 内核线程，走到第 N 级就退出，之后不留轮询。不 panic：任何失败都是一个步号、一个错误码和一行 [cam] 日志（BootProgress 的 ramoops U 记录，pagani-ramoops=1），
// 线程结束。不写存储。
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <libkern/c++/OSData.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
extern "C" {
#include "CamBringup.h"
#include "CamPmic.h"
#include "CamUtil.h"
#include "PaganiCamera.h"
#include "Sm8750Rpmh.h"
}

// 静态链接的内核不接受未定义的弱引用：可能缺席的提供者在这里给弱定义，真的那个优先。
extern "C" {
__attribute__((weak)) void boot_progress_trace(const char *) {}
__attribute__((weak)) volatile uint32_t pagani_panel_probe_quiet;      // 不是 0 时 UfsLog.c 的写者停下，按位用（PCAM_QUIET_BIT）
volatile int32_t pagani_camera_status[16];
void pagani_camera_ios_publish(void);                                  // PaganiCameraIos.cpp
}

namespace {
volatile int32_t *const st = pagani_camera_status;
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
Window gWin[PcamWinCount], gRsc, gCmdDbWin, gSpmiCore, gSpmiCnfg, gSpmiObs;

uint32_t hwRead(void *, unsigned win, uint32_t off)
{
    if (win >= PcamWinCount || !gWin[win].va || (off & 3) || uint64_t(off) + 4 > gWin[win].size) {
        st[8] = st[8] + 1;
        return 0xffffffffu;
    }
    return rd(gWin[win], off);
}
void hwWrite(void *, unsigned win, uint32_t off, uint32_t value)
{
    if (win >= PcamWinCount || !gWin[win].va || (off & 3) || uint64_t(off) + 4 > gWin[win].size) {
        st[8] = st[8] + 1;
        return;
    }
    // 闸门在核心里已经过了一遍；这里是最后一道，核心的路径错了也写不出去。
    if (!pcam_write_allowed(win, off, value)) {
        st[7] = st[7] + 1;
        return;
    }
    wr(gWin[win], off, value);
}
void hwUdelay(void *, unsigned us)
{
    if (us >= 1000) IOSleep(us / 1000);
    else IODelay(us);
}
void hwMsleep(void *, unsigned ms) {IOSleep(ms);}

// RPMh 走 Adreno830 的 SM8750 平台层：每个请求一条命令，等完成；DRV-2 由它自己的锁字串行，和 Wi-Fi、蓝牙、GPU 共用。
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
    if (!mapWindow(gCmdDbWin, PCAM_CMD_DB, PCAM_CMD_DB_BYTES) || !mapWindow(gRsc, PCAM_RSC_DRV2, PCAM_RSC_BYTES)) return gDbState = -14;
    return gDbState = Sm8750CmdDbCopy(&kCmdDbIo, gDb);
}
void rpmhFailure(const char *resource, uint32_t address, uint32_t value, int rc, const Sm8750RpmhResult &result)
{
    char line[160];
    PcamText t;
    pcam_text_start(&t, line, sizeof line);
    pcam_text_str(&t, "[cam] rpmh ");
    pcam_text_str(&t, resource);
    pcam_text_kx(&t, "addr", address);
    pcam_text_kx(&t, "data", value);
    pcam_text_kd(&t, "rc", rc);
    pcam_text_kd(&t, "step", result.step);
    trace(line);
}
int hwArc(void *, const char *resource, uint32_t vlvl, uint32_t *hlvl)
{
    int rc = cmdDb();
    if (rc) return rc;
    Sm8750RpmhArc arc = {};
    unsigned count = 0;
    if ((rc = Sm8750CmdDbArc(gDb, resource, &arc.addr, arc.vlvl, &count))) return rc;
    arc.count = uint8_t(count);
    arc.present = 1;
    uint32_t level = 0;
    if ((rc = Sm8750RpmhArcLevel(&arc, vlvl, &level))) return rc;
    Sm8750RpmhCmd cmd = {arc.addr, level};
    Sm8750RpmhResult result = {};
    rc = Sm8750RpmhWriteActive(&kDrv2, &cmd, 1, 1, &result);
    if (rc) rpmhFailure(resource, arc.addr, level, rc, result);
    *hlvl = level;
    return rc;
}
int hwVrm(void *, const char *resource, uint32_t reg, uint32_t value, uint32_t *address)
{
    // 写错电压可能烧传感器：核心过了闸门，这里再过一遍，错的调用到不了 RPMh。
    if (!pcam_vrm_allowed(resource, reg, value)) {
        st[7] = st[7] + 1;
        return -1;
    }
    int rc = cmdDb();
    if (rc) return rc;
    uint32_t base = 0;
    if ((rc = Sm8750CmdDbVrm(gDb, resource, &base))) return rc;
    if (address) *address = base;
    Sm8750RpmhCmd cmd = {base + reg, value};
    Sm8750RpmhResult result = {};
    rc = Sm8750RpmhWriteActive(&kDrv2, &cmd, 1, 1, &result);
    if (rc) rpmhFailure(resource, base + reg, value, rc, result);
    return rc;
}

// PMIC：只经 observer 读。
uint32_t spmiCore(void *, uint32_t off) {return gSpmiCore.va && off + 4 <= gSpmiCore.size ? rd(gSpmiCore, off) : 0;}
uint32_t spmiCnfg(void *, uint32_t off) {return gSpmiCnfg.va && off + 4 <= gSpmiCnfg.size ? rd(gSpmiCnfg, off) : 7;}
uint32_t spmiObsRead(void *, uint32_t off) {return gSpmiObs.va && off + 4 <= gSpmiObs.size ? rd(gSpmiObs, off) : 0xe;}
void spmiObsWrite(void *, uint32_t off, uint32_t value) {if (gSpmiObs.va && off + 4 <= gSpmiObs.size) wr(gSpmiObs, off, value);}
void spmiDelay(void *, unsigned us) {IODelay(us);}
const PcamSpmiIo kSpmi = {nullptr, spmiCore, spmiCnfg, spmiObsRead, spmiObsWrite, spmiDelay};
int hwPmic(void *, uint16_t ppid, uint8_t *status, uint8_t *enable, uint8_t *mode, uint16_t *mv)
{
    if (!mapWindow(gSpmiCore, PCAM_SPMI_CORE, PCAM_SPMI_CORE_BYTES) || !mapWindow(gSpmiCnfg, PCAM_SPMI_CNFG, PCAM_SPMI_CNFG_BYTES) ||
        !mapWindow(gSpmiObs, PCAM_SPMI_OBS, PCAM_SPMI_OBS_BYTES))
        return -14;
    return pcam_pmic_rail(&kSpmi, ppid, status, enable, mode, mv);
}
void hwLog(void *, const char *line) {trace(line);}
void hwQuiet(void *, unsigned on)
{
    if (on) __atomic_fetch_or(&pagani_panel_probe_quiet, PCAM_QUIET_BIT, __ATOMIC_RELEASE);
    else __atomic_fetch_and(&pagani_panel_probe_quiet, ~PCAM_QUIET_BIT, __ATOMIC_RELEASE);
}
const PcamHw kHw = {nullptr, hwRead, hwWrite, hwUdelay, hwMsleep, hwArc, hwVrm, hwPmic, hwLog, hwQuiet};

PcamBringup gB;

void cameraThread(void *parameter, wait_result_t)
{
    thread_set_thread_name(current_thread(), "pagani-cam");
    gStartMs = nowMs();
    const unsigned level = unsigned(reinterpret_cast<uintptr_t>(parameter));
    char line[160];
    PcamText t;
    pcam_text_start(&t, line, sizeof line);
    pcam_text_str(&t, "[cam] thread started");
    pcam_text_kd(&t, "level", level);
    trace(line);
    for (unsigned w = 0; w < PcamWinCount; ++w)
        if (!mapWindow(gWin[w], kPcamWinBase[w], kPcamWinBytes[w])) {
            st[0] = -PcamStepSurvey;
            st[1] = -14;
            trace("[cam] FAIL a register window was not mapped");
            return;
        }
    gB.hw = &kHw;
    gB.level = level;
    const int rc = pcam_run(&gB);
    st[0] = gB.step;
    st[1] = gB.error;
    st[3] = int32_t((nowMs() - gStartMs) / 1000);
    st[4] = int32_t(gB.cci_version);
    st[5] = int32_t(gB.sensor_id);
    st[6] = int32_t(gB.sensor_address);
    st[7] = st[7] + int32_t(gB.refused);
    pcam_text_start(&t, line, sizeof line);
    pcam_text_str(&t, "[cam] thread ends");
    pcam_text_kd(&t, "step", st[0]);
    pcam_text_kd(&t, "err", st[1]);
    pcam_text_kd(&t, "rc", rc);
    pcam_text_kd(&t, "refused", st[7]);
    pcam_text_kd(&t, "outside", st[8]);
    pcam_text_kd(&t, "seconds", st[3]);
    trace(line);
}

volatile uint32_t gStarted;
}  // namespace

extern "C" void pagani_camera_start(void)
{
    // 插件的日志服务跟着 pagani-torch-ios 走：没有新的启动参数，那一项本身默认关。
    if (argument("pagani-torch-ios", 0) == 1) pagani_camera_ios_publish();
    const unsigned value = argument("pagani-cam", 0);
    if (!value) return;
    st[2] = int32_t(value);
    const unsigned level = pcam_level(value);
    if (!level) {
        trace("[cam] pagani-cam value not understood: off");
        return;
    }
    uint32_t expected = 0;
    if (!__atomic_compare_exchange_n(&gStarted, &expected, 1u, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return;
    thread_t thread = nullptr;
    if (kernel_thread_start(cameraThread, reinterpret_cast<void *>(uintptr_t(level)), &thread) != KERN_SUCCESS) {
        st[0] = -PcamStepSurvey;
        st[1] = -7;
        trace("[cam] FAIL no thread");
        return;
    }
    thread_deallocate(thread);
}
