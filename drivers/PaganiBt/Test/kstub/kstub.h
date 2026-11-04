/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* 给 PaganiBtHost.cpp 用的内核接口替身：host_glue.cpp 拿它把胶水编到主机上跑。只有胶水用到的那一点，行为由 host_glue.cpp 实现，它记下每一次调用的先后。
 * 签名和 kern_control.c、IOLib 一致（bsd/sys/kern_control.h、iokit/IOKit/IOLocks.h）。 */
#ifndef PAGANI_BT_KSTUB_H
#define PAGANI_BT_KSTUB_H
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <strings.h>
#include <vector>

/* IOLib */
struct IOLock {int held;};
IOLock *IOLockAlloc(void);
void IOLockLock(IOLock *lock);
void IOLockUnlock(IOLock *lock);
int IOLockSleepDeadline(IOLock *lock, void *event, uint64_t deadline, uint32_t interType);
void IOLockWakeup(IOLock *lock, void *event, bool oneThread);
void *IOMallocZero(size_t bytes);
extern "C" void IOLog(const char *format, ...);
enum {THREAD_UNINT = 0};

/* clock */
enum {kMillisecondScale = 1000000};
uint64_t mach_absolute_time(void);
void absolutetime_to_nanoseconds(uint64_t absolute, uint64_t *ns);
void clock_interval_to_deadline(uint32_t interval, uint32_t scale, uint64_t *deadline);

/* 注册表 */
class OSObject {
public:
    virtual ~OSObject() {}
    int retains = 1;
    void release() {--retains;}
};
class IORegistryPlane;
extern const IORegistryPlane *gIODTPlane;
class IORegistryEntry : public OSObject {
public:
    std::string name;
    std::map<std::string, std::vector<uint8_t>> properties;
    static IORegistryEntry *fromPath(const char *path, const IORegistryPlane *plane);
    void setName(const char *n) {name = n;}
    bool setProperty(const char *key, void *bytes, unsigned length);
    void removeProperty(const char *key);
};
class IOService : public IORegistryEntry {
public:
    IOService *provider = nullptr;
    bool started = false, registered = false, terminated = false;
    bool init();
    bool attach(IOService *to);
    void detach(IOService *from);
    bool start(IOService *from);
    void registerService();
    bool terminate();
    static IOService *getResourceService();
};
#define OSSafeReleaseNULL(p) do {if (p) (p)->release(); (p) = nullptr;} while (0)

/* kern_control */
typedef void *kern_ctl_ref;
typedef int errno_t;
typedef uint32_t u_int32_t;
#define CTL_FLAG_PRIVILEGED 0x1
#define CTL_FLAG_REG_ID_UNIT 0x2
#define CTL_FLAG_REG_SOCK_STREAM 0x4
#define MAX_KCTL_NAME 96
struct sockaddr_ctl {uint32_t sc_id, sc_unit;};
struct mbuf_stub {mbuf_stub *next; const uint8_t *data; size_t len;};
typedef mbuf_stub *mbuf_t;
typedef errno_t (*ctl_connect_func)(kern_ctl_ref, struct sockaddr_ctl *, void **);
typedef errno_t (*ctl_disconnect_func)(kern_ctl_ref, u_int32_t, void *);
typedef errno_t (*ctl_send_func)(kern_ctl_ref, u_int32_t, void *, mbuf_t, int);
typedef errno_t (*ctl_setopt_func)(kern_ctl_ref, u_int32_t, void *, int, void *, size_t);
typedef errno_t (*ctl_getopt_func)(kern_ctl_ref, u_int32_t, void *, int, void *, size_t *);
struct kern_ctl_reg {
    char ctl_name[MAX_KCTL_NAME];
    u_int32_t ctl_id, ctl_unit, ctl_flags, ctl_sendsize, ctl_recvsize;
    ctl_connect_func ctl_connect;
    ctl_disconnect_func ctl_disconnect;
    ctl_send_func ctl_send;
    ctl_setopt_func ctl_setopt;
    ctl_getopt_func ctl_getopt;
};
errno_t ctl_register(struct kern_ctl_reg *reg, kern_ctl_ref *ref);
errno_t ctl_deregister(kern_ctl_ref ref);
errno_t ctl_enqueuedata(kern_ctl_ref ref, u_int32_t unit, void *data, size_t len, u_int32_t flags);
inline mbuf_t mbuf_next(mbuf_t m) {return m->next;}
inline void *mbuf_data(mbuf_t m) {return const_cast<uint8_t *>(m->data);}
inline size_t mbuf_len(mbuf_t m) {return m->len;}
void mbuf_freem(mbuf_t m);
#endif
