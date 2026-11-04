// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026, UniXNU contributors. All rights reserved.
// Host stand-ins for the kernel interfaces that PaganiAudio.cpp and PaganiAudioService.cpp use, for Test/glue.cpp, which defines the
// functions and records what the kernel code did with them. Only what those two files use; signatures and the constants that matter
// (memory options, wait flags, the shared region) follow xnu-10063. The kernel archive itself never sees this directory.
#ifndef PAGANI_AUDIO_KSTUB_H
#define PAGANI_AUDIO_KSTUB_H
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <mach/mach_types.h>     // task_t, thread_t, kern_return_t, boolean_t, vm_size_t, vm_offset_t (passed through only)
#include <mach/mach_time.h>      // mach_timebase_info_data_t
#include <IOKit/IOReturn.h>      // the real kIOReturn codes
#include <IOKit/IOTypes.h>       // IOOptionBits, UInt32, kMillisecondScale

// Kernel functions whose names user space also has (or might): renamed, so the test's definitions never interpose on the C library.
#define mach_absolute_time kstub_mach_absolute_time
#define clock_timebase_info kstub_clock_timebase_info
#define absolutetime_to_nanoseconds kstub_absolutetime_to_nanoseconds
#define clock_interval_to_deadline kstub_clock_interval_to_deadline
#define kernel_thread_start kstub_kernel_thread_start
#define thread_deallocate kstub_thread_deallocate
#define thread_set_thread_name kstub_thread_set_thread_name
#define current_thread kstub_current_thread
#define kernel_task kstub_kernel_task

typedef void *event_t;
typedef int wait_result_t;
typedef void (*thread_continue_t)(void *, wait_result_t);
#define THREAD_UNINT 0x00000000

extern "C" {
uint64_t kstub_mach_absolute_time(void);
void kstub_clock_timebase_info(mach_timebase_info_data_t *info);
void kstub_absolutetime_to_nanoseconds(uint64_t abstime, uint64_t *result);
void kstub_clock_interval_to_deadline(uint32_t interval, uint32_t scale_factor, uint64_t *result);
kern_return_t kstub_kernel_thread_start(thread_continue_t continuation, void *parameter, thread_t *new_thread);
void kstub_thread_deallocate(thread_t thread);
void kstub_thread_set_thread_name(thread_t thread, const char *name);
thread_t kstub_current_thread(void);
extern task_t kstub_kernel_task;
boolean_t PE_parse_boot_argn(const char *arg_string, void *arg_ptr, int max_arg);
void IOLog(const char *format, ...) __attribute__((format(printf, 1, 2)));
int copyin(uint64_t uaddr, void *kaddr, size_t len);
void *get_task_pmap(task_t task);
}

// IOLocks.h
typedef struct KStubLock IOLock;
extern "C" {
IOLock *IOLockAlloc(void);
void IOLockLock(IOLock *lock);
void IOLockUnlock(IOLock *lock);
void IOLockWakeup(IOLock *lock, void *event, bool oneThread);
int IOLockSleepDeadline(IOLock *lock, void *event, uint64_t deadline, UInt32 interType);
}

// IOMemoryDescriptor.h, IOBufferMemoryDescriptor.h
enum {kIODirectionNone = 0x0, kIODirectionIn = 0x1, kIODirectionOut = 0x2, kIODirectionInOut = kIODirectionIn | kIODirectionOut};
typedef IOOptionBits IODirection;
enum {kIOMemoryKernelUserShared = 0x00010000};

class OSDictionary;
class OSObject {
public:
    int kstub_refs = 1;
    virtual ~OSObject() {}
    virtual void retain() const {++const_cast<OSObject *>(this)->kstub_refs;}
    virtual void release() const {if (--const_cast<OSObject *>(this)->kstub_refs == 0) delete this;}
};

class IOMemoryDescriptor : public OSObject {
public:
    static IOMemoryDescriptor *withAddressRange(uint64_t address, uint64_t length, IOOptionBits options, task_t task);
    virtual IOReturn prepare(IODirection forDirection = kIODirectionNone);
    virtual IOReturn complete(IODirection forDirection = kIODirectionNone);
};

class IOBufferMemoryDescriptor : public IOMemoryDescriptor {
public:
    void *kstub_bytes = nullptr;
    size_t kstub_size = 0;
    IOOptionBits kstub_options = 0;
    ~IOBufferMemoryDescriptor() override;
    static IOBufferMemoryDescriptor *inTaskWithOptions(task_t inTask, IOOptionBits options, vm_size_t capacity, vm_offset_t alignment = 1);
    void *getBytesNoCopy() {return kstub_bytes;}
};

// IOService.h, IOUserClient.h
class IOService : public OSObject {
public:
    IOService *kstub_provider = nullptr;
    int kstub_registered = 0, kstub_terminated = 0;
    char kstub_grant[64] = "", kstub_user_client[64] = "", kstub_backend[32] = "";
    unsigned long long kstub_abi = 0;
    virtual bool init(OSDictionary *dictionary = nullptr);
    virtual bool attach(IOService *provider);
    virtual void detach(IOService *provider);
    virtual bool start(IOService *provider);
    virtual void registerService(IOOptionBits options = 0);
    virtual bool terminate(IOOptionBits options = 0);
    bool setProperty(const char *key, const char *value);
    bool setProperty(const char *key, unsigned long long value, unsigned int bits);
    static IOService *getResourceService();
};

struct IOExternalMethodArguments {
    uint32_t version, selector;
    const uint64_t *scalarInput;
    uint32_t scalarInputCount;
    const void *structureInput;
    uint32_t structureInputSize;
};
struct IOExternalMethodDispatch;

class IOUserClient : public IOService {
public:
    virtual bool initWithTask(task_t owningTask, void *securityToken, UInt32 type, OSDictionary *properties);
    virtual IOReturn clientClose();
    virtual IOReturn clientMemoryForType(UInt32 type, IOOptionBits *options, IOMemoryDescriptor **memory);
    virtual IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *arguments, IOExternalMethodDispatch *dispatch,
                                    OSObject *target, void *reference);
};

// OSMetaClass.h: the class macros reduce to plain C++ (construction through new, casts through RTTI); registration is recorded.
class OSMetaClass {
public:
    static void *preModLoad(const char *kextID);
    static IOReturn postModLoad(void *loadHandle);
};
#define OSDeclareDefaultStructors(className) \
public:                                      \
    className() {}                           \
protected:                                   \
    ~className() override {}
#define OSDefineMetaClassAndStructors(className, superclassName)
#define OSDynamicCast(type, inst) dynamic_cast<type *>(inst)
#define OSTypeAlloc(type) new type

#endif
