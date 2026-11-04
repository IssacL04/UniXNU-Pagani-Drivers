// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026, UniXNU contributors. All rights reserved.
// The iOS side of PaganiSensors (SensorsHid.h has what iOS reads): one IOHIDDevice per sensor kind, the original 21H461 IOHIDEventDriver
// on each (matched by our personalities), the events built with the original IOHIDEvent constructors and handed to the driver's own
// dispatchEvent, the way PaganiInput's touch device hands its digitizer events (drivers/PaganiInput, HidEvents.h).
//
// ABI. IOHIDDevice is the original class (the header OriginalHidDeviceABI.h and the IOHIDDevice symbols come from the qualification of
// build-pagani-peripherals.py, already linked for PaganiInput); this class overrides exactly the slots PaganiInput's does and
// newUserClient, so every IOHIDDevice symbol it needs is one that qualification bound. The constructors, IOHIDEventDriver::setProperties (driver table slot 44)
// and IOHIDEventDriver::copyEvent (slot 201) are bound by scripts/build-pagani-sensors.py only after their bytes and slots matched the
// pinned original image; each driver's table is checked again at run time before the hooks go in (an exact copy of the original table
// with slots 44 and 201 replaced, shared by the sensor drivers only; every other driver keeps the original). The class initializer is
// taken out of __mod_init_func and run by pagani_sensors_hid_setup (as PaganiInput's and PaganiBattery's are).
//
// The orientation service (SensorsHid.h has the trap it is built around): the same class, one slot more (newUserClient, qualified by the
// build like the others); the original driver makes the events from input reports (handleReport), so no constructor is bound for it and
// its hooks are not installed. PaganiSensorSpuClient is the 'ashd' user client the AppleSPULib plug-in opens: an IOUserClient of XNU's
// own, built with the tree's headers.
#include "OriginalHidDeviceABI.h"
#include "OriginalHidEventABI.h"
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOCatalogue.h>
#include <IOKit/IOCommandGate.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOWorkLoop.h>
#include <kern/clock.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSIterator.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSUnserialize.h>
#include <mach/kmod.h>
extern "C" {
#include "SensorsHid.h"
#include "SensorsKernel.h"
#include "SensorsText.h"
}

extern "C" kmod_info_t g_kernel_kmod_info;
extern "C" void boot_progress_trace(const char *);
extern "C" void pagani_sensors_hid_static_init(void) __asm__("__GLOBAL__sub_I_PaganiSensorsHid.cpp");
// Bound by scripts/build-pagani-sensors.py (absolute symbols) after the byte qualification. Data symbols: their addresses are taken
// (adrp/add, any distance) and called through a register, never with a direct branch.
extern "C" {
extern const char pagani_sensors_event_accelerometer[];   // IOHIDEvent *(AbsoluteTime, IOFixed x, y, z, type, subType, sequence, options)
extern const char pagani_sensors_event_gyro[];            // the same, type 20
extern const char pagani_sensors_event_compass[];         // the same, type 21
extern const char pagani_sensors_event_ambient_light[];   // IOHIDEvent *(AbsoluteTime, UInt32 level, ch0, ch1, ch2, ch3, options)
extern const char pagani_sensors_event_proximity[];       // IOHIDEvent *(AbsoluteTime, UInt16 mask, UInt32 level, options)
extern const char pagani_sensors_event_pressure[];        // IOHIDEvent *(AbsoluteTime, IOFixed level, UInt32 sequence, options)
extern const char pagani_sensors_driver_set_properties[]; // IOReturn IOHIDEventDriver::setProperties(OSObject *)
extern const char pagani_sensors_driver_copy_event[];     // IOHIDEvent *IOHIDEventDriver::copyEvent(type, IOHIDEvent *, options)
}

namespace {
enum : unsigned {kSetProperties = 44, kCopyEvent = 201, kDispatch = 198, kTicksPerSecond = 19200000u};

using MotionEvent = OSObject *(*)(uint64_t, int32_t, int32_t, int32_t, uint32_t, uint32_t, uint32_t, uint32_t);
using LightEvent = OSObject *(*)(uint64_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
using ProximityEvent = OSObject *(*)(uint64_t, uint16_t, uint32_t, uint32_t);
using PressureEvent = OSObject *(*)(uint64_t, int32_t, uint32_t, uint32_t);
using SetProperties = IOReturn (*)(IOService *, OSObject *);
using CopyEvent = OSObject *(*)(IOService *, uint32_t, OSObject *, uint32_t);
using DispatchEvent = void (*)(IOService *, OSObject *, uint32_t);

template <typename F> F bound(const char *symbol) {return reinterpret_cast<F>(reinterpret_cast<uintptr_t>(symbol));}

IOWorkLoop *gLoop;
IOCommandGate *gGate;
IOService *gDevice[SNS_HID_KINDS];
IOService *gDriver[SNS_HID_KINDS];               // atomic: published once bound
uintptr_t gTable[PAGANI_HID_DRIVER_SLOTS + 2];
PaganiSensorsDemandFn gDemand;
volatile int32_t gLux = -1, gProximity = -1, gProximityLevel, gOrientation = -1;
volatile uint32_t gSpuLines, gOrientLines;
uint32_t gSequence[PaganiSensorKinds];
volatile uint32_t gDispatched, gUnbound, gRestamped;
char gPersonalities[4096];
}

// The user client of type 'ashd': Open and SendCommand answer success, the commands are only logged (the orientation comes from
// here, nothing on the phone listens to a configuration). The plug-in may be opened by every process that matches the service.
class PaganiSensorSpuClient : public IOUserClient {
    OSDeclareDefaultStructors(PaganiSensorSpuClient)
public:
    IOReturn clientClose() override
    {
        terminate();
        return kIOReturnSuccess;
    }
    IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *arguments, IOExternalMethodDispatch *, OSObject *, void *) override
    {
        uint32_t command = 0;
        const uint64_t first = arguments->scalarInputCount ? arguments->scalarInput[0] : 0;
        if (sns_hid_spu_method(selector, arguments->scalarInputCount, first, &command)) return kIOReturnUnsupported;
        arguments->scalarOutputCount = 0;
        arguments->structureOutputSize = 0;
        if (__atomic_fetch_add(&gSpuLines, 1u, __ATOMIC_RELAXED) < 8) {
            char line[96];
            SntText t;
            snt_start(&t, line, sizeof line);
            snt_str(&t, "[sensors] spu");
            snt_kd(&t, "selector", selector);
            snt_kx(&t, "command", command);
            snt_kd(&t, "bytes", arguments->structureInputSize);
            boot_progress_trace(line);
        }
        if (gDemand && selector != SNS_HID_SPU_CLOSE) gDemand(SNS_HID_ORIENT, 1);   // the client wants the current state
        return kIOReturnSuccess;
    }
};
OSDefineMetaClassAndStructors(PaganiSensorSpuClient, IOUserClient)

class PaganiSensorHid : public IOHIDDevice {
    OSDeclareDefaultStructors(PaganiSensorHid)
    unsigned kind = SNS_HID_KINDS;
public:
    void select(unsigned k) {kind = k;}
    bool report(const uint8_t *bytes, uint32_t size)
    {
        auto buffer = IOBufferMemoryDescriptor::withBytes(bytes, size, kIODirectionNone);
        const IOReturn rc = buffer ? handleReport(buffer, 0, 0) : kIOReturnNoMemory;
        OSSafeReleaseNULL(buffer);
        return rc == kIOReturnSuccess;
    }
    IOReturn newUserClient(task_t owner, void *security, UInt32 type, OSDictionary *properties, IOUserClient **handler) override
    {
        if (kind != SNS_HID_ORIENT || type != SNS_HID_SPU_CLIENT) return IOHIDDevice::newUserClient(owner, security, type, properties, handler);
        PaganiSensorSpuClient *client = new PaganiSensorSpuClient;
        if (!client) return kIOReturnNoMemory;
        if (!client->initWithTask(owner, security, type, properties) || !client->attach(this)) {
            client->release();
            return kIOReturnError;
        }
        if (!client->start(this)) {
            client->detach(this);
            client->release();
            return kIOReturnError;
        }
        *handler = client;
        return kIOReturnSuccess;
    }
    IOWorkLoop *getWorkLoop() const override {return gLoop;}
    OSString *newTransportString() const override {return OSString::withCString("ADSP");}
    OSString *newProductString() const override
    {
        const SnsHidKind *k = sns_hid_kind(kind);
        return OSString::withCString(k ? k->product : "Pagani Sensor");
    }
    IOReturn newReportDescriptor(IOMemoryDescriptor **out) const override
    {
        uint32_t length = 0;
        const uint8_t *bytes = sns_hid_descriptor(kind, &length);
        if (!out || !bytes) return kIOReturnBadArgument;
        *out = IOBufferMemoryDescriptor::withBytes(bytes, length, kIODirectionNone);
        return *out ? kIOReturnSuccess : kIOReturnNoMemory;
    }
    bool start(IOService *provider) override
    {
        const SnsHidKind *k = sns_hid_kind(kind);
        if (!k || !gLoop) return false;
        setProperty("Built-In", true);
        setProperty("HIDDefaultBehavior", true);
        if (k->placement) setProperty("Placement", k->placement, 32);
        if (kind == SNS_HID_ORIENT) {
            // Before the device starts: the service must never be visible without what CoreMotion opens on it. backboardd and locationd
            // are sandboxed and their profiles know Apple's user client class: the grant of the XNU patch
            // zzwc-native-adreno-sandbox-grant-all lets them open this one.
            OSDictionary *types = OSDictionary::withCapacity(1);
            OSString *path = OSString::withCString(SNS_HID_SPU_PLUGIN_PATH);
            const bool ready = types && path && types->setObject(SNS_HID_SPU_PLUGIN_TYPE, path) && setProperty("IOCFPlugInTypes", types);
            OSSafeReleaseNULL(types);
            OSSafeReleaseNULL(path);
            if (!ready) return false;
            setProperty("NativeSandboxGrantAll", true);
        }
        return IOHIDDevice::start(provider);
    }
    void stop(IOService *provider) override {IOHIDDevice::stop(provider);}
    void free() override {IOHIDDevice::free();}
};
OSDefineMetaClassAndStructors(PaganiSensorHid, IOHIDDevice)

namespace {
unsigned kindOf(IOService *driver)
{
    for (unsigned k = 0; k < PaganiSensorKinds; ++k) if (__atomic_load_n(&gDriver[k], __ATOMIC_ACQUIRE) == driver) return k;
    return SNS_HID_KINDS;
}

// The hub's sample time when it is the mach clock (the QTimer the virtual counter runs on, 19.2 MHz) and recent, else now.
uint64_t stamp(uint64_t ticks)
{
    const uint64_t now = mach_absolute_time();
    if (ticks && ticks <= now && now - ticks < kTicksPerSecond) return ticks;
    if (ticks > now && ticks - now <= kTicksPerSecond / 20) return now;   // read a little before the hub's: now
    __atomic_fetch_add(&gRestamped, 1u, __ATOMIC_RELAXED);
    return now;
}

OSObject *makeEvent(const SnsHidEvent &e, uint64_t ts)
{
    switch (e.type) {
    case SNS_HID_EVENT_ACCELEROMETER:
        return bound<MotionEvent>(pagani_sensors_event_accelerometer)(ts, e.x, e.y, e.z, 0, 0, e.sequence, 0);
    case SNS_HID_EVENT_GYRO: return bound<MotionEvent>(pagani_sensors_event_gyro)(ts, e.x, e.y, e.z, 0, 0, e.sequence, 0);
    case SNS_HID_EVENT_COMPASS: return bound<MotionEvent>(pagani_sensors_event_compass)(ts, e.x, e.y, e.z, 0, 0, e.sequence, 0);
    case SNS_HID_EVENT_AMBIENT_LIGHT: return bound<LightEvent>(pagani_sensors_event_ambient_light)(ts, e.level, 0, 0, 0, 0, 0);
    case SNS_HID_EVENT_PROXIMITY: return bound<ProximityEvent>(pagani_sensors_event_proximity)(ts, e.mask, e.level, 0);
    case SNS_HID_EVENT_PRESSURE: return bound<PressureEvent>(pagani_sensors_event_pressure)(ts, e.x, e.sequence, 0);
    default: return nullptr;
    }
}

// ---- the hooks in the sensor drivers' table ----
IOReturn hookSetProperties(IOService *driver, OSObject *properties)
{
    OSDictionary *dict = OSDynamicCast(OSDictionary, properties);
    const unsigned kind = kindOf(driver);
    if (dict && kind < PaganiSensorKinds && gDemand) {
        static const struct {const char *name; unsigned key;} keys[] = {{"ReportInterval", SNS_HID_KEY_REPORT_INTERVAL},
                                                                        {"DetectionMode", SNS_HID_KEY_DETECTION_MODE}};
        for (const auto &key : keys) {
            OSNumber *n = OSDynamicCast(OSNumber, dict->getObject(key.name));
            uint32_t demand = 0;
            if (n && sns_hid_demand(kind, key.key, n->unsigned64BitValue(), &demand)) gDemand(kind, demand);
        }
    }
    return reinterpret_cast<SetProperties>(pagani_original_hid_driver_vtable[kSetProperties])(driver, properties);
}

// The last ambient light level and proximity state for a copy (CoreBrightness copies the current light; the original driver only
// copies keyboard, vendor and orientation events).
OSObject *hookCopyEvent(IOService *driver, uint32_t type, OSObject *matching, uint32_t options)
{
    const unsigned kind = kindOf(driver);
    if (kind == PaganiSensorLight && type == SNS_HID_EVENT_AMBIENT_LIGHT) {
        const int32_t lux = __atomic_load_n(&gLux, __ATOMIC_ACQUIRE);
        if (lux >= 0) return bound<LightEvent>(pagani_sensors_event_ambient_light)(mach_absolute_time(), (uint32_t)lux, 0, 0, 0, 0, 0);
    }
    if (kind == PaganiSensorProximity && type == SNS_HID_EVENT_PROXIMITY) {
        const int32_t mask = __atomic_load_n(&gProximity, __ATOMIC_ACQUIRE);
        if (mask >= 0)
            return bound<ProximityEvent>(pagani_sensors_event_proximity)(mach_absolute_time(), (uint16_t)mask,
                                                                         (uint32_t)__atomic_load_n(&gProximityLevel, __ATOMIC_RELAXED), 0);
    }
    return reinterpret_cast<CopyEvent>(pagani_original_hid_driver_vtable[kCopyEvent])(driver, type, matching, options);
}

IOReturn dispatchAction(OSObject *, void *driver, void *event, void *, void *)
{
    reinterpret_cast<DispatchEvent>(pagani_original_hid_driver_vtable[kDispatch])(static_cast<IOService *>(driver),
                                                                                 static_cast<OSObject *>(event), 0);
    return kIOReturnSuccess;
}

uint32_t number(IORegistryEntry *entry, const char *name)
{
    OSNumber *value = OSDynamicCast(OSNumber, entry->getProperty(name));
    return value ? value->unsigned32BitValue() : 0xffffffffu;
}

// The event driver below a device (device -> IOHIDInterface -> IOHIDEventDriver), retained; as PaganiInput's findDriver.
IOService *findDriver(IORegistryEntry *parent, unsigned depth)
{
    if (!depth) return nullptr;
    OSIterator *iterator = parent->getChildIterator(gIOServicePlane);
    if (!iterator) return nullptr;
    IOService *found = nullptr;
    while (OSObject *child = iterator->getNextObject()) {
        IORegistryEntry *entry = OSDynamicCast(IORegistryEntry, child);
        if (!entry) continue;
        if (entry->metaCast("IOHIDEventDriver")) {
            found = OSDynamicCast(IOService, entry);
            if (found) found->retain();
        } else {
            found = findDriver(entry, depth - 1);
        }
        if (found) break;
    }
    iterator->release();
    return found;
}

bool registerClass()
{
    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!handle) return false;
    pagani_sensors_hid_static_init();
    return OSMetaClass::postModLoad(handle) == kIOReturnSuccess;
}
}  // namespace

extern "C" int pagani_sensors_hid_setup(PaganiSensorsDemandFn demand, uint32_t mask)
{
    gDemand = demand;
    if (!registerClass()) return PaganiSensorsErrClass;
    gLoop = IOWorkLoop::workLoop();
    if (!gLoop) return PaganiSensorsErrNoMemory;
    const uint32_t n = sns_hid_personalities(mask, gPersonalities, sizeof gPersonalities);
    OSObject *object = n ? OSUnserializeXML(gPersonalities) : nullptr;
    OSArray *personalities = OSDynamicCast(OSArray, object);
    const bool added = personalities && gIOCatalogue && gIOCatalogue->addDrivers(personalities, false);
    OSSafeReleaseNULL(object);
    return added ? 0 : static_cast<int>(kIOReturnNoResources);
}

extern "C" int pagani_sensors_hid_publish(unsigned kind)
{
    if (kind >= SNS_HID_KINDS || !gLoop) return static_cast<int>(kIOReturnBadArgument);
    if (gDevice[kind]) return 0;
    PaganiSensorHid *device = new PaganiSensorHid;
    IOService *provider = IOService::getResourceService();
    if (!device) return static_cast<int>(kIOReturnNoMemory);
    device->select(kind);
    const bool attached = provider && device->init() && device->attach(provider);
    const bool started = attached && device->start(provider);
    if (!started) {
        if (attached) device->detach(provider);
        device->release();
        return static_cast<int>(attached ? kIOReturnError : kIOReturnNoResources);
    }
    if (!gGate) {
        gGate = IOCommandGate::commandGate(device);
        if (gGate && gLoop->addEventSource(gGate) != kIOReturnSuccess) OSSafeReleaseNULL(gGate);
    }
    gDevice[kind] = device;   // with its attachment, until reboot
    return 0;
}

extern "C" int pagani_sensors_hid_bind(unsigned kind, char *line, uint32_t size)
{
    SntText t;
    if (kind >= SNS_HID_KINDS || !gDevice[kind]) return -1;
    if (__atomic_load_n(&gDriver[kind], __ATOMIC_ACQUIRE)) return 1;
    IOService *driver = findDriver(gDevice[kind], 4);
    if (!driver) return 0;
    snt_start(&t, line, size);
    snt_str(&t, "[sensors] hid ");
    snt_str(&t, kind == SNS_HID_ORIENT ? "orientation" : sns_kind_name(kind));
    const SnsHidKind *k = sns_hid_kind(kind);
    const uint32_t page = number(driver, "PrimaryUsagePage"), usage = number(driver, "PrimaryUsage");
    snt_kx(&t, "usage", (uint64_t)page << 16 | usage);
    uintptr_t *table = *reinterpret_cast<uintptr_t **>(driver);
    const uintptr_t *original = pagani_original_hid_driver_vtable;
    const bool qualified = original[kSetProperties] == reinterpret_cast<uintptr_t>(pagani_sensors_driver_set_properties) &&
                           original[kCopyEvent] == reinterpret_cast<uintptr_t>(pagani_sensors_driver_copy_event);
    int rc = 1;
    if (!qualified) {
        snt_str(&t, " refused: the original table's slots 44/201 are not the qualified methods");
        rc = -2;
    } else if (table != original && table != gTable + 2) {
        snt_str(&t, " refused: the driver's table is not the original");
        rc = -3;
    } else if (page != k->page || usage != k->usage || driver->getProperty("Built-In") != kOSBooleanTrue) {
        snt_str(&t, " refused: not our personality's properties");
        rc = -4;
    } else if (!gGate) {
        snt_str(&t, " refused: no command gate");
        rc = -5;
    }
    if (rc < 0) {
        driver->release();
        return rc;
    }
    if (kind == SNS_HID_ORIENT) {                 // the original driver makes its events: no hook
        __atomic_store_n(&gDriver[kind], driver, __ATOMIC_RELEASE);
        snt_str(&t, " bound");
        return 1;
    }
    if (!gTable[2]) {
        memcpy(gTable, original - 2, sizeof gTable);
        gTable[2 + kSetProperties] = reinterpret_cast<uintptr_t>(&hookSetProperties);
        gTable[2 + kCopyEvent] = reinterpret_cast<uintptr_t>(&hookCopyEvent);
    }
    __atomic_store_n(&gDriver[kind], driver, __ATOMIC_RELEASE);   // the kind first: the hooks look it up
    __atomic_store_n(reinterpret_cast<uintptr_t **>(driver), gTable + 2, __ATOMIC_RELEASE);
    if (k->placement) snt_kd(&t, "placement", number(driver, "Placement"));
    snt_kd(&t, "interval_us", number(driver, "ReportInterval"));
    snt_str(&t, " bound");
    return 1;
}

extern "C" void pagani_sensors_hid_dispatch(const SnsSample *sample)
{
    SnsHidEvent e;
    if (sample->kind >= PaganiSensorKinds) return;
    if (sample->kind == PaganiSensorLight) __atomic_store_n(&gLux, sample->value[0] > 0 ? sample->value[0] : 0, __ATOMIC_RELEASE);
    if (sample->kind == PaganiSensorProximity) {
        __atomic_store_n(&gProximityLevel, sample->value[1], __ATOMIC_RELAXED);
        __atomic_store_n(&gProximity, sample->value[0] ? int32_t(SNS_HID_PROXIMITY_NEAR) : 0, __ATOMIC_RELEASE);
    }
    IOService *driver = __atomic_load_n(&gDriver[sample->kind], __ATOMIC_ACQUIRE);
    if (!driver || !gGate) {
        __atomic_fetch_add(&gUnbound, 1u, __ATOMIC_RELAXED);
        return;
    }
    if (sns_hid_event(sample, ++gSequence[sample->kind], &e)) return;
    OSObject *event = makeEvent(e, stamp(sample->timestamp));
    if (!event) return;
    gGate->runAction(dispatchAction, driver, event);
    event->release();
    __atomic_fetch_add(&gDispatched, 1u, __ATOMIC_RELAXED);
}

extern "C" void pagani_sensors_hid_orientation(int state)
{
    uint8_t bytes[2];
    PaganiSensorHid *device = OSDynamicCast(PaganiSensorHid, gDevice[SNS_HID_ORIENT]);
    if (!device || !sns_hid_orient_report(state, bytes)) return;
    __atomic_store_n(&gOrientation, state, __ATOMIC_RELEASE);
    device->report(bytes, sizeof bytes);
    __atomic_fetch_add(&gDispatched, 1u, __ATOMIC_RELAXED);
    char line[64];
    SntText t;
    snt_start(&t, line, sizeof line);
    snt_str(&t, "[sensors] orientation");
    snt_kd(&t, "state", state);
    if (__atomic_fetch_add(&gOrientLines, 1u, __ATOMIC_RELAXED) < 32) boot_progress_trace(line);
}

extern "C" void pagani_sensors_hid_orientation_replay(void)
{
    static const uint8_t none[2] = {1, 0};
    const int state = __atomic_load_n(&gOrientation, __ATOMIC_ACQUIRE);
    PaganiSensorHid *device = OSDynamicCast(PaganiSensorHid, gDevice[SNS_HID_ORIENT]);
    if (!device || state < 0) return;
    device->report(none, sizeof none);            // the handler wants the bit going from 0 to 1
    pagani_sensors_hid_orientation(state);
}

extern "C" int pagani_sensors_hid_node(const char *name)
{
    IORegistryEntry *product = IORegistryEntry::fromPath("/product", gIODTPlane);
    if (!product) return -2;
    IORegistryEntry *existing = product->childFromPath(name, gIODTPlane);
    if (existing) {
        existing->release();
        product->release();
        return 1;
    }
    IOService *node = new IOService;
    if (node && !node->init()) {
        node->release();
        node = nullptr;
    }
    if (!node) {
        product->release();
        return -12;
    }
    uint32_t n = 0;
    while (name[n]) n++;
    OSData *bytes = OSData::withBytes(name, n + 1);
    bool ok = bytes && node->setProperty("name", bytes);    // as the device tree's own nodes: the name both ways
    OSSafeReleaseNULL(bytes);
    node->setName(name);
    ok = ok && node->attachToParent(product, gIODTPlane);
    node->release();                              // the plane keeps it once attached
    product->release();
    return ok ? 0 : -5;
}

extern "C" void pagani_sensors_hid_counts(uint32_t *dispatched, uint32_t *unbound, uint32_t *restamped)
{
    *dispatched = __atomic_load_n(&gDispatched, __ATOMIC_RELAXED);
    *unbound = __atomic_load_n(&gUnbound, __ATOMIC_RELAXED);
    *restamped = __atomic_load_n(&gRestamped, __ATOMIC_RELAXED);
}
