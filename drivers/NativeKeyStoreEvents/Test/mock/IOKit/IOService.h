// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test double of the IOKit surface NativeKeyStoreEvents.cpp uses: lifecycle, IOExternalMethodArguments with the field names of
// xnu's IOUserClient.h, an IOMemoryDescriptor that aborts on writes it was not prepared for (the kernel panics there: "IOGMD: not
// wired"), IOLock (a real mutex, so the concurrency test means something), and the IOReturn values of IOReturn.h.
#pragma once
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <libkern/libkern.h>
using UInt32 = uint32_t;
using IOReturn = int;
using IOByteCount = size_t;
using IOOptionBits = uint32_t;
struct task;
using task_t = struct task *;
constexpr IOReturn kIOReturnSuccess = 0;
constexpr IOReturn kIOReturnError = (IOReturn)0xe00002bc;
constexpr IOReturn kIOReturnNoMemory = (IOReturn)0xe00002bd;
constexpr IOReturn kIOReturnNotPrivileged = (IOReturn)0xe00002c1;
constexpr IOReturn kIOReturnBadArgument = (IOReturn)0xe00002c2;
constexpr IOReturn kIOReturnNoSpace = (IOReturn)0xe00002c4;
constexpr IOReturn kIOReturnUnsupported = (IOReturn)0xe00002c7;
constexpr IOReturn kIOReturnNotReady = (IOReturn)0xe00002d8;
constexpr IOReturn kIOReturnVMError = (IOReturn)0xe00002ee;
constexpr IOReturn kIOReturnNotFound = (IOReturn)0xe00002f0;

class OSDictionary {};
class OSObject {
public:
    virtual ~OSObject() = default;
    virtual void free() { delete this; }
    void release() { free(); }
};
class OSBoolean final : public OSObject {
public:
    void free() override {}   // immortal, like the kernel's two booleans
};
extern OSBoolean *kOSBooleanTrue;
extern OSBoolean *kOSBooleanFalse;

class IOMemoryDescriptor {
public:
    uint8_t bytes[0x8000];
    size_t length = sizeof bytes;
    int prepared = 0;
    unsigned prepares = 0, completes = 0;
    IOReturn prepareResult = kIOReturnSuccess;
    IOByteCount getLength() const { return length; }
    IOReturn prepare(unsigned = 0) {
        ++prepares;
        if (prepareResult != kIOReturnSuccess) return prepareResult;
        ++prepared;
        return kIOReturnSuccess;
    }
    IOReturn complete(unsigned = 0) {
        ++completes;
        if (prepared <= 0) { fprintf(stderr, "complete() without prepare()\n"); abort(); }
        --prepared;
        return kIOReturnSuccess;
    }
    IOByteCount writeBytes(IOByteCount offset, const void *p, IOByteCount n) {
        if (prepared <= 0) { fprintf(stderr, "writeBytes on an unprepared descriptor: the kernel panics (IOGMD: not wired)\n"); abort(); }
        if (offset > length || n > length - offset) { fprintf(stderr, "writeBytes past the end: assertf in DEVELOPMENT\n"); abort(); }
        memcpy(bytes + offset, p, n);
        return n;
    }
};
struct IOExternalMethodArguments {
    uint32_t version = 2;
    uint32_t selector = 0;
    const uint64_t *scalarInput = nullptr;
    uint32_t scalarInputCount = 0;
    const void *structureInput = nullptr;
    uint32_t structureInputSize = 0;
    IOMemoryDescriptor *structureInputDescriptor = nullptr;
    uint64_t *scalarOutput = nullptr;
    uint32_t scalarOutputCount = 0;
    void *structureOutput = nullptr;
    uint32_t structureOutputSize = 0;
    IOMemoryDescriptor *structureOutputDescriptor = nullptr;
    uint32_t structureOutputDescriptorSize = 0;
};
struct IOExternalMethodDispatch {};

struct IOLock { std::mutex mutex; int held = 0; };
IOLock *IOLockAlloc(void);
void IOLockFree(IOLock *lock);
void IOLockLock(IOLock *lock);
void IOLockUnlock(IOLock *lock);

class IOService;
class IOUserClient : public OSObject {
public:
    bool terminated = false, started = false;
    virtual bool initWithTask(task_t, void *, UInt32, OSDictionary *) { return true; }
    bool attach(IOService *) { return true; }
    void detach(IOService *) {}
    virtual bool start(IOService *) { started = true; return true; }
    void terminate() { terminated = true; }
    virtual IOReturn clientClose() { return kIOReturnSuccess; }
    virtual IOReturn externalMethod(uint32_t, IOExternalMethodArguments *, IOExternalMethodDispatch * = nullptr,
                                    OSObject * = nullptr, void * = nullptr) = 0;
    static OSObject *copyClientEntitlement(task_t task, const char *entitlement);
};
#define OSDeclareDefaultStructors(name)
#define OSDefineMetaClassAndStructors(name, base)
class IOService : public OSObject {
public:
    bool baseOK = true;
    unsigned properties = 0, failProperty = 0, publications = 0, stops = 0;
    bool keyOperationsAvailable = false;
    virtual bool start(IOService *) { return baseOK; }
    virtual void stop(IOService *) { ++stops; }
    void setName(const char *) {}
    bool setProperty(const char *name, bool value) {
        if (!strcmp(name, "KeyOperationsAvailable")) keyOperationsAvailable = value;
        return ++properties != failProperty;
    }
    void registerService() { ++publications; }
    virtual IOReturn newUserClient(task_t, void *, UInt32, OSDictionary *, IOUserClient **) = 0;
    virtual IOReturn newUserClient(task_t, void *, UInt32, IOUserClient **) = 0;
};
