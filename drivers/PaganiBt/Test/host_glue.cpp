// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026, UniXNU contributors. All rights reserved.
// PaganiBtHost.cpp 对 Test/kstub 的内核替身：胶水里没有判断，出错都出在接线上（属性类型、字节序、先后顺序、连接被拒时的回调），这里一条条看。
// 假 bluetoothd 走真实的回调入口：ctl_connect、ctl_send（mbuf 链）、ctl_disconnect，读 ctl_enqueuedata 放进去的字节。
#include <cerrno>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include "kstub.h"
extern "C" {
#include "../BtBridge.h"
#include "../BtTlv.h"
#include "../PaganiBt.h"
#include "../PaganiBtBoard.h"
#include "../PaganiBtHost.h"
void boot_progress_trace(const char *line);
int proc_selfpid(void);
void proc_signal(int pid, int signum);
volatile int32_t pagani_bt_status[32];
}
extern "C" {
#include "check.h"
}

// ---- 替身的世界 ----
static std::vector<std::string> gEvents;
static uint64_t gClockMs = 5000;
static int gPid = 4242;
static kern_ctl_reg gReg;
static bool gRegistered;
static int gRegisterError, gEnqueueError;
static bool gAttachFails;
static std::vector<uint8_t> gSocket;
static uint32_t gFreed;
static std::vector<int> gSignals;
static std::vector<std::string> gLog;
static IORegistryEntry gChosen, gProduct;
static IOService gResources;
static std::vector<IOService *> gServices;
const IORegistryPlane *gIODTPlane = reinterpret_cast<const IORegistryPlane *>(&gChosen);

IOLock *IOLockAlloc(void) {return new IOLock();}
void IOLockLock(IOLock *lock) {CHECK(!lock->held); lock->held = 1;}
void IOLockUnlock(IOLock *lock) {CHECK(lock->held); lock->held = 0;}
int IOLockSleepDeadline(IOLock *lock, void *, uint64_t deadline, uint32_t)
{
    CHECK(lock->held);
    if (deadline > gClockMs) gClockMs = deadline;
    return 0;
}
void IOLockWakeup(IOLock *lock, void *, bool) {CHECK(lock->held);}
void *IOMallocZero(size_t bytes) {return calloc(1, bytes);}
extern "C" void IOLog(const char *, ...) {}
uint64_t mach_absolute_time(void) {return gClockMs;}
void absolutetime_to_nanoseconds(uint64_t absolute, uint64_t *ns) {*ns = absolute * 1000000;}
void clock_interval_to_deadline(uint32_t interval, uint32_t scale, uint64_t *deadline) {*deadline = gClockMs + uint64_t(interval) * scale / 1000000;}

IORegistryEntry *IORegistryEntry::fromPath(const char *path, const IORegistryPlane *plane)
{
    CHECK(plane == gIODTPlane);
    if (!strcmp(path, "/chosen")) return &gChosen;
    if (!strcmp(path, "/product")) return &gProduct;
    return nullptr;
}
bool IORegistryEntry::setProperty(const char *key, void *bytes, unsigned length)
{
    const uint8_t *p = static_cast<const uint8_t *>(bytes);
    properties[key] = std::vector<uint8_t>(p, p + length);
    return true;
}
void IORegistryEntry::removeProperty(const char *key)
{
    gEvents.push_back(std::string("remove ") + key);
    properties.erase(key);
}
bool IOService::init() {gServices.push_back(this); return true;}
bool IOService::attach(IOService *to)
{
    if (gAttachFails) return false;
    provider = to;
    return true;
}
void IOService::detach(IOService *) {provider = nullptr;}
bool IOService::start(IOService *from) {CHECK(from == provider); started = true; return true;}
void IOService::registerService()
{
    // 属性要在别人找得到这个服务之前摆好
    CHECK(properties.count("local-mac-address") && properties.count("transport-encoding") && properties.count("transport-speed"));
    registered = true;
    gEvents.push_back("register " + name);
}
bool IOService::terminate() {terminated = true; gEvents.push_back("terminate " + name); return true;}
IOService *IOService::getResourceService() {return &gResources;}

errno_t ctl_register(struct kern_ctl_reg *reg, kern_ctl_ref *ref)
{
    if (gRegisterError) return gRegisterError;
    gReg = *reg;
    gRegistered = true;
    *ref = &gReg;
    gEvents.push_back(std::string("ctl_register ") + reg->ctl_name);
    return 0;
}
errno_t ctl_deregister(kern_ctl_ref ref)
{
    CHECK(ref == &gReg);
    gRegistered = false;
    gEvents.push_back("ctl_deregister");
    return 0;
}
errno_t ctl_enqueuedata(kern_ctl_ref ref, u_int32_t, void *data, size_t len, u_int32_t flags)
{
    CHECK(ref == &gReg && flags == 0);
    if (gEnqueueError) return gEnqueueError;
    const uint8_t *p = static_cast<const uint8_t *>(data);
    gSocket.insert(gSocket.end(), p, p + len);
    return 0;
}
void mbuf_freem(mbuf_t) {gFreed++;}
int proc_selfpid(void) {return gPid;}
void proc_signal(int pid, int signum)
{
    gSignals.push_back(signum);
    gEvents.push_back("signal " + std::to_string(signum) + " pid " + std::to_string(pid));
}
void boot_progress_trace(const char *line) {gLog.emplace_back(line);}

// ---- 串口：控制器模型 ----
static bool gMute;
static std::vector<uint8_t> gWire, gFromCtrl;
extern "C" int pagani_bt_uart_write(const uint8_t *data, uint32_t bytes, unsigned)
{
    gWire.insert(gWire.end(), data, data + bytes);
    if (!gMute && data[0] == PbH4Cmd) {
        const uint8_t cc[7] = {PbH4Evt, PbEvtCommandComplete, 4, 1, data[1], data[2], 0};
        gFromCtrl.insert(gFromCtrl.end(), cc, cc + sizeof cc);
    }
    return 0;
}
extern "C" uint32_t pagani_bt_uart_read(uint8_t *data, uint32_t max)
{
    const uint32_t n = gFromCtrl.size() < max ? uint32_t(gFromCtrl.size()) : max;
    memcpy(data, gFromCtrl.data(), n);
    gFromCtrl.erase(gFromCtrl.begin(), gFromCtrl.begin() + n);
    return n;
}

static void fresh()
{
    gEvents.clear();
    gChosen.properties.clear();
    gProduct.properties.clear();
    gSocket.clear();
    gWire.clear();
    gFromCtrl.clear();
    gSignals.clear();
    gServices.clear();
    gLog.clear();
    gRegisterError = gEnqueueError = 0;
    gAttachFails = gMute = false;
    gFreed = 0;
}
static size_t position(const char *what)
{
    for (size_t i = 0; i < gEvents.size(); ++i) if (gEvents[i].find(what) == 0) return i;
    return size_t(-1);
}
// 真实 bluetoothd 的连接：sc_unit 由 kern_control 分配。
static errno_t connectAs(uint32_t unit, int pid, void **info)
{
    sockaddr_ctl sac = {7, unit};
    gPid = pid;
    *info = nullptr;
    const errno_t error = gReg.ctl_connect(&gReg, &sac, info);
    if (error && gReg.ctl_disconnect) gReg.ctl_disconnect(&gReg, unit, *info);    // ctl_connect 在失败时也调
    return error;
}
// 一包切成一条 mbuf 链，长度 0 的段也放一个：kern_control 给的链里可能有。
static void sendChain(uint32_t unit, void *info, const uint8_t *data, uint32_t bytes)
{
    mbuf_stub parts[4];
    const uint32_t a = bytes / 2;
    parts[0] = {&parts[1], data, 1};
    parts[1] = {&parts[2], data + 1, 0};
    parts[2] = {&parts[3], data + 1, a > 1 ? a - 1 : 0};
    parts[3] = {nullptr, data + (a > 1 ? a : 1), bytes - (a > 1 ? a : 1)};
    const uint32_t before = gFreed;
    CHECK(gReg.ctl_send(&gReg, unit, info, parts, 0) == 0);
    CHECK(gFreed == before + 1);        // 链由回调释放，kern_control 不管
}

static void published()
{
    fresh();
    const uint8_t wire[6] = {0x66, 0x55, 0x44, 0x33, 0x22, 0x11};
    CHECK(pagani_bt_host_publish(wire, 8000000, 0) == 0);
    // 地址写成书写顺序，线上是反的
    const std::vector<uint8_t> written = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    CHECK(gChosen.properties.at("mac-address-bluetooth0") == written);
    const std::vector<uint8_t> one = {1, 0, 0, 0};
    CHECK(gProduct.properties.at("bluetooth-le") == one);
    CHECK(gServices.size() == 1);
    IOService *s = gServices[0];
    CHECK(s->name == "bluetooth" && s->provider == &gResources && s->started && s->registered);
    CHECK(s->properties.at("local-mac-address") == written);
    // bluetoothd 用 CFDataGetBytes 取 4 字节：一定是 OSData，长度 4
    CHECK(s->properties.at("transport-encoding") == one);
    const std::vector<uint8_t> speed = {0x00, 0x12, 0x7a, 0x00};
    CHECK(s->properties.at("transport-speed") == speed);
    CHECK(s->properties.at("name").size() == sizeof "bluetooth");
    // 控制名：流式，mobile 用户能连，TIOCGETA、TIOCSETA 不接
    CHECK(!strcmp(gReg.ctl_name, "com.apple.uart.sk.bluetooth"));
    CHECK(gReg.ctl_flags == CTL_FLAG_REG_SOCK_STREAM && !(gReg.ctl_flags & CTL_FLAG_PRIVILEGED));
    CHECK(!gReg.ctl_getopt && !gReg.ctl_setopt);
    CHECK(gReg.ctl_sendsize >= 2 * 1029 && gReg.ctl_recvsize >= 16384);
    // bluetoothd 看到服务时控制名必须已经在
    CHECK(position("ctl_register") < position("register bluetooth"));
}

static void withdrawn_after_guard()
{
    published();
    void *info = nullptr;
    CHECK(connectAs(11, 501, &info) == 0 && info != nullptr);
    gMute = true;
    uint8_t p[PbPacketMax];
    const uint32_t n = pb_hci_command(p, PbOpReset, nullptr, 0);
    sendChain(11, info, p, n);
    pagani_bt_host_run(nullptr, nullptr, 0);    // 控制器不答：4 s 后守卫 trip，这里才返回
    CHECK(gWire.size() == 4 && !memcmp(gWire.data(), p, 4));
    CHECK(gChosen.properties.empty() && gProduct.properties.empty());
    CHECK(gServices[0]->terminated);
    // 先撤后杀：launchd 重开的 bluetoothd 要看到没有蓝牙的机器
    const size_t kill_at = position("signal 9 pid 501");
    CHECK(kill_at != size_t(-1));
    CHECK(position("remove mac-address-bluetooth0") < kill_at && position("remove bluetooth-le") < kill_at);
    CHECK(position("terminate bluetooth") < kill_at);
    CHECK(pagani_bt_status[20] == PbGuardCommand && pagani_bt_status[19] == PbLinkDead);
    // 之后的连接拒绝，ctl_connect 的回调走 disconnect，unitinfo 是空的
    CHECK(connectAs(12, 502, &info) == ECONNREFUSED && info == nullptr);
    bool said = false;
    for (const std::string &line : gLog) said = said || line.find("[bt] stage 4 withdrawn") == 0;
    CHECK(said);
}

static void failed_bringup()
{
    published();
    pagani_bt_host_failed();
    CHECK(gChosen.properties.empty() && gServices[0]->terminated);
    CHECK(gSignals.empty());                    // 没人连着，谁都不杀
    CHECK(pagani_bt_status[20] == PbGuardBringup);
    // 连着的时候要杀
    published();
    void *info = nullptr;
    CHECK(connectAs(21, 601, &info) == 0);
    pagani_bt_host_failed();
    CHECK(gSignals.size() == 1 && gSignals[0] == SIGKILL && position("signal 9 pid 601") != size_t(-1));
}

static void connections()
{
    published();
    void *first = nullptr, *second = nullptr;
    CHECK(connectAs(31, 701, &first) == 0);
    // 第二个被拒：ctl_connect 随后用空的 unitinfo 调 disconnect，不能把第一个断开。单元号故意相同：判据只能是 unitinfo
    CHECK(connectAs(31, 702, &second) == EBUSY && second == nullptr);
    uint8_t p[PbPacketMax];
    const uint32_t n = pb_hci_command(p, 0xfc4d, nullptr, 0);
    sendChain(31, first, p, n);
    CHECK(gSocket.size() == 7 && gSocket[4] == 0x4d && gSocket[5] == 0xfc && gSocket[6] == 0x01);
    // ENOBUFS 和 ENOMEM 留着重试，其他错误（客户端没了）丢掉
    gSocket.clear();
    gEnqueueError = ENOBUFS;
    sendChain(31, first, p, n);
    CHECK(gSocket.empty());
    gEnqueueError = 0;
    sendChain(31, first, p, n);
    CHECK(gSocket.size() == 14);
    gEnqueueError = EINVAL;
    sendChain(31, first, p, n);
    gEnqueueError = 0;
    gSocket.clear();
    sendChain(31, first, p, n);
    CHECK(gSocket.size() == 7);
    CHECK(gReg.ctl_disconnect(&gReg, 31, first) == 0);
    void *third = nullptr;
    CHECK(connectAs(33, 703, &third) == 0);
}

static void stand_in()
{
    fresh();
    const uint8_t wire[6] = {1, 2, 3, 4, 5, 6};
    CHECK(pagani_bt_host_publish(wire, 115200, 1) == 0);
    void *info = nullptr;
    CHECK(connectAs(41, 801, &info) == 0);
    uint8_t p[PbPacketMax];
    const uint32_t n = pb_hci_command(p, PbOpBdAddr, nullptr, 0);
    sendChain(41, info, p, n);
    CHECK(gSocket.size() == 13 && !memcmp(gSocket.data() + 7, wire, 6));
    CHECK(gWire.empty());
    CHECK(gServices[0]->properties.at("transport-speed") == (std::vector<uint8_t>{0x00, 0xc2, 0x01, 0x00}));
}

static void failures()
{
    // 控制名注册不上：什么都不发布
    fresh();
    gRegisterError = EEXIST;
    const uint8_t wire[6] = {1, 2, 3, 4, 5, 6};
    CHECK(pagani_bt_host_publish(wire, 8000000, 0) == -13);
    CHECK(gChosen.properties.empty() && gProduct.properties.empty() && gServices.empty());
    // 服务发布失败：键撤掉，控制名也撤掉，不留半截
    fresh();
    gAttachFails = true;
    CHECK(pagani_bt_host_publish(wire, 8000000, 0) == -14);
    CHECK(gChosen.properties.empty() && gProduct.properties.empty());
    CHECK(!gRegistered && position("ctl_deregister") != size_t(-1));
    CHECK(position("register bluetooth") == size_t(-1));
}

int main()
{
    published();
    withdrawn_after_guard();
    failed_bringup();
    connections();
    stand_in();
    failures();
    printf("PASS glue: %u checks\n", checks);
    return 0;
}
