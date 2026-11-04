// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026, UniXNU contributors. All rights reserved.
// 阶段 4 面向 iOS 的一半（BtBridge.h、README.md）。这里只有内核接口的接线，判断都在 BtBridge.c，由主机测试覆盖。
//
// pagani_bt_host_publish 在 pagani_bt_start 里同步跑完（Stage7Probe 在 bsd_autoconf 里调用，domaininit 已过，launchd 还没起）：
//   控制名 com.apple.uart.sk.bluetooth：SOCK_STREAM，不设 CTL_FLAG_PRIVILEGED，bluetoothd 以 mobile 运行。getopt、setopt 不接：它的 TIOCGETA、TIOCSETA 不看返回值。
//   /chosen/mac-address-bluetooth0、/product/bluetooth-le：MobileGestalt 的 bluetooth 和 bluetooth-le，做法同 PaganiWifi 的 /chosen/mac-address-wifi0。
//   IOService 名字 bluetooth：bluetoothd 和 MobileBluetooth 都用 IOServiceNameMatching 找它，只放 ADT 节点找不到。local-mac-address、transport-encoding、
//   transport-speed 都是 OSData：bluetoothd 用 CFDataGetBytes 取，给 OSNumber 会崩。
// 守卫 trip 之后 retract() 撤掉上面三样，再 SIGKILL 连着的进程，原因写在 BtBridge.h。不 panic，失败都是一行 [bt] 日志。
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <libkern/c++/OSData.h>
#include <kern/clock.h>
#include <sys/errno.h>
#include <sys/kern_control.h>
#include <sys/kpi_mbuf.h>
#include <sys/signal.h>
extern "C" {
#include "BtBridge.h"
#include "BtTlv.h"
#include "BtUtil.h"
#include "PaganiBt.h"
#include "PaganiBtBoard.h"
#include "PaganiBtHost.h"
void boot_progress_trace(const char *line);     // BootProgress 的，或 PaganiBt.cpp 的弱定义
int proc_selfpid(void);
void proc_signal(int pid, int signum);
}

namespace {
enum : uint32_t {
    kStatsFirstMs = 60000, kStatsEveryMs = 600000,
};

PbBridge *gBridge;
IOLock *gLock;
kern_ctl_ref gControl;
IOService *gService;
uint32_t gKicked, gRoomEvent;

void say(const char *line)
{
    boot_progress_trace(line);
    IOLog("%s\n", line);
}
uint64_t nowMs()
{
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    return ns / 1000000;
}
uint64_t deadlineAfter(unsigned ms)
{
    uint64_t deadline = 0;
    clock_interval_to_deadline(ms, kMillisecondScale, &deadline);
    return deadline;
}

// ---- 桥的回调 ----
void ioLock(void *, int take)
{
    if (take) IOLockLock(gLock);
    else IOLockUnlock(gLock);
}
uint64_t ioNow(void *) {return nowMs();}
int ioUartWrite(void *, const uint8_t *data, uint32_t bytes, unsigned timeout_ms) {return pagani_bt_uart_write(data, bytes, timeout_ms);}
uint32_t ioUartRead(void *, uint8_t *data, uint32_t max) {return pagani_bt_uart_read(data, max);}
int ioClientPut(void *, uint32_t unit, const uint8_t *data, uint32_t bytes)
{
    const errno_t error = ctl_enqueuedata(gControl, unit, const_cast<uint8_t *>(data), bytes, 0);
    if (!error) return 0;
    return error == ENOBUFS || error == ENOMEM ? 1 : -1;
}
void ioKick(void *)
{
    gKicked = 1;
    IOLockWakeup(gLock, &gKicked, true);
}
void ioWaitRoom(void *, unsigned ms) {IOLockSleepDeadline(gLock, &gRoomEvent, deadlineAfter(ms), THREAD_UNINT);}
void ioWakeRoom(void *) {IOLockWakeup(gLock, &gRoomEvent, false);}
void ioLog(void *, const char *line) {say(line);}
const PbBridgeIo kIo = {nullptr, ioLock, ioNow, ioUartWrite, ioUartRead, ioClientPut, ioKick, ioWaitRoom, ioWakeRoom, ioLog};

// ---- kern_control：回调之前 kern_control 都放开了套接字锁，在 bluetoothd 的线程上，可以睡 ----
errno_t ctlConnect(kern_ctl_ref, struct sockaddr_ctl *sac, void **unitinfo)
{
    const int rc = pb_bridge_connect(gBridge, sac->sc_unit, proc_selfpid());
    if (rc == PbBridgeBusy) return EBUSY;
    if (rc) return ECONNREFUSED;
    *unitinfo = gBridge;
    return 0;
}
// 连接被拒时 ctl_connect 也会调这里，那时 unitinfo 是空的，不能动已经连着的客户端。
errno_t ctlDisconnect(kern_ctl_ref, u_int32_t unit, void *unitinfo)
{
    if (unitinfo) pb_bridge_disconnect(gBridge, unit);
    return 0;
}
errno_t ctlSend(kern_ctl_ref, u_int32_t unit, void *, mbuf_t chain, int)
{
    for (mbuf_t m = chain; m; m = mbuf_next(m))
        if (mbuf_len(m)) pb_bridge_write(gBridge, unit, static_cast<const uint8_t *>(mbuf_data(m)), uint32_t(mbuf_len(m)));
    mbuf_freem(chain);
    return 0;
}

bool registerControl()
{
    struct kern_ctl_reg control;
    bzero(&control, sizeof control);
    strlcpy(control.ctl_name, PB_CONTROL_NAME, sizeof control.ctl_name);
    control.ctl_flags = CTL_FLAG_REG_SOCK_STREAM;
    control.ctl_sendsize = PbControlSendBytes;
    control.ctl_recvsize = PbControlRecvBytes;
    control.ctl_connect = ctlConnect;
    control.ctl_disconnect = ctlDisconnect;
    control.ctl_send = ctlSend;
    return ctl_register(&control, &gControl) == 0;
}

bool putU32(IORegistryEntry *entry, const char *key, uint32_t value) {return entry->setProperty(key, &value, sizeof value);}

bool publishKeys(const uint8_t written[6])
{
    IORegistryEntry *chosen = IORegistryEntry::fromPath("/chosen", gIODTPlane);
    IORegistryEntry *product = IORegistryEntry::fromPath("/product", gIODTPlane);
    const bool ok = chosen && product && chosen->setProperty(PB_KEY_ADDRESS, const_cast<uint8_t *>(written), 6) && putU32(product, PB_KEY_LE, 1);
    OSSafeReleaseNULL(chosen);
    OSSafeReleaseNULL(product);
    return ok;
}

bool publishService(const uint8_t written[6], uint32_t baud)
{
    IOService *home = IOService::getResourceService();
    IOService *node = new IOService;
    if (node && !node->init()) {
        node->release();
        node = nullptr;
    }
    if (!node || !home) {
        OSSafeReleaseNULL(node);
        return false;
    }
    static const char name[] = PB_SERVICE_NAME;
    node->setName(PB_SERVICE_NAME);
    bool ok = node->setProperty("name", const_cast<char *>(name), sizeof name);
    ok = ok && node->setProperty("local-mac-address", const_cast<uint8_t *>(written), 6);
    ok = ok && putU32(node, "transport-encoding", PbTransportH4) && putU32(node, "transport-speed", baud);
    ok = ok && node->attach(home);
    if (ok && !node->start(home)) {
        node->detach(home);
        ok = false;
    }
    if (ok) {
        node->registerService();
        gService = node;        // 资源服务的连接留着它，retract 时用，不再 release
    } else {
        node->release();
    }
    return ok;
}

void retract()
{
    if (IORegistryEntry *chosen = IORegistryEntry::fromPath("/chosen", gIODTPlane)) {
        chosen->removeProperty(PB_KEY_ADDRESS);
        chosen->release();
    }
    if (IORegistryEntry *product = IORegistryEntry::fromPath("/product", gIODTPlane)) {
        product->removeProperty(PB_KEY_LE);
        product->release();
    }
    if (gService) {
        gService->terminate();
        gService = nullptr;
    }
}

void publishStatus()
{
    const PbBridge *b = gBridge;
    pagani_bt_status[19] = b->link;
    pagani_bt_status[20] = b->guard;
    pagani_bt_status[21] = int32_t(b->connections);
    pagani_bt_status[22] = int32_t(b->packets_out);
    pagani_bt_status[23] = int32_t(b->frames_up);
    pagani_bt_status[24] = int32_t(b->vsc_answered);
    pagani_bt_status[25] = int32_t(b->vsc_kinds);
    pagani_bt_status[26] = int32_t(b->frames_dropped);
    pagani_bt_status[27] = int32_t(b->from_client.junk + b->from_client.oversize + b->frames_bad);
    pagani_bt_status[28] = int32_t(b->put_full);
    pagani_bt_status[29] = int32_t(b->failed_starts);
    pagani_bt_status[30] = int32_t(b->refused);
    pagani_bt_status[31] = int32_t(b->tx_dropped);
}
void logStats()
{
    const PbBridge *b = gBridge;
    publishStatus();
    char line[160];
    PbText t;
    pb_text_start(&t, line, sizeof line);
    pb_text_str(&t, "[bt] bridge");
    pb_text_kd(&t, "link", b->link);
    pb_text_kd(&t, "conn", b->connections);
    pb_text_kd(&t, "out", b->packets_out);
    pb_text_kd(&t, "up", b->frames_up);
    pb_text_kd(&t, "drop", b->frames_dropped);
    pb_text_kd(&t, "vsc", b->vsc_answered);
    pb_text_kd(&t, "kinds", b->vsc_kinds);
    pb_text_kd(&t, "full", b->put_full);
    pb_text_kd(&t, "paused", b->rx_paused);
    pb_text_kd(&t, "bad", pagani_bt_status[27]);
    say(line);
}

// 撤销之后再杀：launchd 重开的 bluetoothd 看到的已经是没有蓝牙的机器。
void finish()
{
    retract();
    logStats();
    const int pid = gBridge->kill_pid;
    char line[160];
    PbText t;
    pb_text_start(&t, line, sizeof line);
    pb_text_str(&t, "[bt] stage 4 withdrawn: no bluetooth until reboot");
    pb_text_kd(&t, "why", gBridge->guard);
    pb_text_kd(&t, "pid", pid);
    say(line);
    if (pid > 0) proc_signal(pid, SIGKILL);
}

void hostSleep(unsigned ms)
{
    IOLockLock(gLock);
    if (!gKicked) IOLockSleepDeadline(gLock, &gKicked, deadlineAfter(ms), THREAD_UNINT);
    gKicked = 0;
    IOLockUnlock(gLock);
}
}  // namespace

extern "C" int pagani_bt_host_publish(const uint8_t address[6], uint32_t baud, int stand_in)
{
    uint8_t written[6];
    pb_bdaddr_written(address, written);
    gBridge = static_cast<PbBridge *>(IOMallocZero(sizeof(PbBridge)));
    gLock = IOLockAlloc();
    if (!gBridge || !gLock) return -12;
    pb_bridge_init(gBridge, &kIo, address, stand_in);
    if (!registerControl()) return -13;
    if (!publishKeys(written) || !publishService(written, baud)) {
        retract();
        (void)ctl_deregister(gControl);
        gControl = nullptr;
        return -14;
    }
    return 0;
}

extern "C" void pagani_bt_host_run(const PbH4Rx *rx, const uint8_t *rest, uint32_t rest_bytes)
{
    pb_bridge_up(gBridge, rx, rest, rest_bytes);
    uint64_t stats = nowMs() + kStatsFirstMs;
    for (;;) {
        const int ms = pb_bridge_step(gBridge);
        if (ms < 0) break;
        if (nowMs() >= stats) {
            stats = nowMs() + kStatsEveryMs;
            logStats();
        }
        if (ms) hostSleep(unsigned(ms));
    }
    finish();
}

extern "C" void pagani_bt_host_failed(void)
{
    pb_bridge_fail(gBridge);
    finish();
}
