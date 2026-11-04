// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026, UniXNU contributors. All rights reserved.
// The property mapping is ported from UniXNU-sheng package/ShengBattery.cpp (https://github.com/UniXNU/UniXNU-sheng, commit 08cde20e,
// ShengBattery::publish); the class is renamed and takes its values from PaganiBattery's core (BatteryCore.h) instead of reading the DSP
// itself. sheng's finding, kept: powerd finds every IOPMPowerSource, reads CurrentCapacity/MaxCapacity, IsCharging, ExternalConnected,
// voltage, amperage and AdapterDetails, and listens for the status change message updateStatus sends; the status bar, the lock screen's
// charging state and Settings > Battery follow from that.
//
// The battery service. PaganiBattery.cpp registers this file's class late (its static initializer, taken out of __mod_init_func by
// scripts/build-pagani-battery.py), creates the object attached to the platform expert and calls update() from its own thread only, so
// the IOPMPowerSource properties have one writer. The service is registered once the first snapshot with a known charge is in place.
// Which properties, their names and types come from BatteryProps.c (the 21H461 facts behind each one are in BatteryProps.h), so this
// file sets no key of its own: the host checks hold that list against what powerd, the status bar and Settings read.
#include <IOKit/IOLib.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/pwr_mgt/IOPM.h>
#include <IOKit/pwr_mgt/IOPMPowerSource.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSSymbol.h>
extern "C" {
#include "BatteryCore.h"
#include "BatteryProps.h"
}

// The AdapterDetails member names BatteryProps.h documents are IOPM.h's.
static_assert(__builtin_strcmp(kIOPMPSAdapterDetailsFamilyKey, PBAT_ADAPTER_FAMILY) == 0, "AdapterDetails FamilyCode");
static_assert(__builtin_strcmp(kIOPMPSAdapterDetailsWattsKey, PBAT_ADAPTER_WATTS) == 0, "AdapterDetails Watts");
static_assert(__builtin_strcmp(kIOPMPSAdapterDetailsVoltage, PBAT_ADAPTER_MV) == 0, "AdapterDetails AdapterVoltage");
static_assert(__builtin_strcmp(kIOPMPSAdapterDetailsAmperageKey, PBAT_ADAPTER_MA) == 0, "AdapterDetails Current");
static_assert(__builtin_strcmp(kIOPMPSAdapterDetailsKey, "AdapterDetails") == 0, "AdapterDetails");

class PaganiBattery : public IOPMPowerSource {
    OSDeclareDefaultStructors(PaganiBattery)

    void number(const char *key, int64_t value) {
        const OSSymbol *symbol = OSSymbol::withCString(key);
        OSNumber *n = OSNumber::withNumber(static_cast<unsigned long long>(value), 32);
        if (symbol && n) setPSProperty(symbol, n);
        OSSafeReleaseNULL(n);
        OSSafeReleaseNULL(symbol);
    }
    void flag(const char *key, bool value) {
        const OSSymbol *symbol = OSSymbol::withCString(key);
        if (symbol) setPSProperty(symbol, value ? kOSBooleanTrue : kOSBooleanFalse);
        OSSafeReleaseNULL(symbol);
    }
    void text(const char *key, const char *value) {
        const OSSymbol *symbol = OSSymbol::withCString(key);
        OSString *string = OSString::withCString(value);
        if (symbol && string) setPSProperty(symbol, string);
        OSSafeReleaseNULL(string);
        OSSafeReleaseNULL(symbol);
    }
    void adapter(const PbatSnapshot *s) {
        OSDictionary *details = OSDictionary::withCapacity(4);
        if (!details) return;
        OSNumber *family = OSNumber::withNumber(s->adapter_family, 32);
        if (family) {details->setObject(kIOPMPSAdapterDetailsFamilyKey, family); family->release();}
        if (s->external && s->adapter_mv > 0) {
            OSNumber *watts = OSNumber::withNumber(static_cast<unsigned long long>(s->adapter_watts), 32);
            OSNumber *mv = OSNumber::withNumber(static_cast<unsigned long long>(s->adapter_mv), 32);
            OSNumber *ma = OSNumber::withNumber(static_cast<unsigned long long>(s->adapter_ma), 32);
            if (watts) {details->setObject(kIOPMPSAdapterDetailsWattsKey, watts); watts->release();}
            if (mv) {details->setObject(kIOPMPSAdapterDetailsVoltage, mv); mv->release();}
            if (ma) {details->setObject(kIOPMPSAdapterDetailsAmperageKey, ma); ma->release();}
        }
        const OSSymbol *key = OSSymbol::withCString(kIOPMPSAdapterDetailsKey);
        if (key) setPSProperty(key, details);
        OSSafeReleaseNULL(key);
        details->release();
    }

public:
    // Every property through setPSProperty with its symbol: the same keys and OSNumber widths as IOPMPowerSource's own setters
    // (OSSymbols are unique, so "ExternalConnected" here is the class's externalConnectedKey), whose comparison marks what changed.
    void apply(const PbatSnapshot *s, bool first) {
        PbatProp props[PBAT_PROPS_MAX];
        const unsigned n = pagani_battery_props(s, props);
        for (unsigned i = 0; i < n; ++i) {
            const PbatProp &p = props[i];
            switch (p.type) {
            case PbpBool: flag(p.key, p.value != 0); break;
            case PbpNumber: number(p.key, p.value); break;
            case PbpString: if (p.text) text(p.key, p.text); break;
            case PbpAdapter: adapter(s); break;
            default: break;
            }
        }
        if (first) registerService();   // the properties are in place before anyone finds the service
        updateStatus();
    }
};
OSDefineMetaClassAndStructors(PaganiBattery, IOPMPowerSource)

// Called by PaganiBattery.cpp once this file's class is registered: the service, attached to the platform expert, not yet registered.
// Null on failure. The platform's attachment and this reference keep it until reboot.
extern "C" void *pagani_battery_service_create(void) {
    IOService *platform = IOService::getPlatform();
    PaganiBattery *battery = OSTypeAlloc(PaganiBattery);
    if (!platform || !battery || !battery->init() || !battery->attach(platform)) {
        if (battery) battery->release();
        return nullptr;
    }
    return battery;
}

// PaganiBattery.cpp's thread only.
extern "C" void pagani_battery_service_update(void *service, const PbatSnapshot *snapshot, int first) {
    PaganiBattery *battery = static_cast<PaganiBattery *>(service);
    if (battery && snapshot) battery->apply(snapshot, first != 0);
}
