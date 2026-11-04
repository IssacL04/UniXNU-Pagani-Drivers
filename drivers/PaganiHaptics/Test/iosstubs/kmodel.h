// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026, UniXNU contributors. All rights reserved.
// A host model of the IORegistry pieces PaganiHapticsIos.cpp uses (Test/ios.cpp): reference counted objects, OSData, entries with a
// registry name ("IOName"), a property table and parents and children per plane; IORegistryEntry::fromPath and childFromPath resolve
// "/a/b" from the plane's root by those names, as IORegistryEntry::getChildFromComponent does for entries without a location. Only
// what the switch and libMobileGestalt's lookups need; the kernel archive never sees this directory.
#ifndef PAGANI_HAPTICS_KMODEL_H
#define PAGANI_HAPTICS_KMODEL_H
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <map>
#include <string>
#include <vector>

class OSObject {
public:
    int kstub_refs = 1;
    virtual ~OSObject() {}
    void retain() const {++const_cast<OSObject *>(this)->kstub_refs;}
    void release() const {if (--const_cast<OSObject *>(this)->kstub_refs == 0) delete this;}
};

class OSData : public OSObject {
public:
    std::vector<uint8_t> bytes;
    static OSData *withBytes(const void *data, unsigned int length)
    {
        extern int kstub_alloc_fail;
        if (kstub_alloc_fail) return nullptr;
        OSData *d = new OSData;
        d->bytes.assign((const uint8_t *)data, (const uint8_t *)data + length);
        return d;
    }
    unsigned int getLength() const {return (unsigned int)bytes.size();}
    const void *getBytesNoCopy() const {return bytes.data();}
};

struct IORegistryPlane {const char *name;};

class IORegistryEntry : public OSObject {
public:
    std::string kstub_name;
    std::map<std::string, OSObject *> kstub_props;   // the property table, in key order (as the kernel's copyPropertyKeys lists them)
    std::map<const IORegistryPlane *, std::vector<IORegistryEntry *>> kstub_children;
    std::map<const IORegistryPlane *, IORegistryEntry *> kstub_parent;
    ~IORegistryEntry() override
    {
        for (auto &p : kstub_props) p.second->release();
        for (auto &c : kstub_children) for (IORegistryEntry *e : c.second) e->release();
    }
    virtual bool init() {return true;}
    void setName(const char *name) {kstub_name = name;}
    const char *getName() const {return kstub_name.c_str();}
    bool setProperty(const char *key, OSObject *value)
    {
        extern int kstub_property_fail;
        if (kstub_property_fail || !value) return false;
        value->retain();
        auto it = kstub_props.find(key);
        if (it != kstub_props.end()) it->second->release();
        kstub_props[key] = value;
        return true;
    }
    bool attachToParent(IORegistryEntry *parent, const IORegistryPlane *plane)
    {
        extern int kstub_attach_fail;
        if (kstub_attach_fail || !parent || kstub_parent.count(plane)) return false;
        retain();   // the plane keeps it
        parent->kstub_children[plane].push_back(this);
        kstub_parent[plane] = parent;
        return true;
    }
    IORegistryEntry *childFromPath(const char *path, const IORegistryPlane *plane);
    static IORegistryEntry *fromPath(const char *path, const IORegistryPlane *plane);
    static IORegistryEntry *kstub_root;   // the root of the device tree plane
};

class IOService : public IORegistryEntry {
public:
    static int kstub_live;
    IOService() {kstub_live++;}
    ~IOService() override {kstub_live--;}
};

inline IORegistryEntry *IORegistryEntry::childFromPath(const char *path, const IORegistryPlane *plane)
{
    IORegistryEntry *at = this;
    std::string rest = path;
    while (!rest.empty()) {
        if (rest[0] == '/') {rest.erase(0, 1); continue;}
        const size_t slash = rest.find('/');
        const std::string part = rest.substr(0, slash);
        rest = slash == std::string::npos ? "" : rest.substr(slash);
        IORegistryEntry *next = nullptr;
        for (IORegistryEntry *c : at->kstub_children[plane]) if (c->kstub_name == part) {next = c; break;}
        if (!next) return nullptr;
        at = next;
    }
    at->retain();
    return at;
}
inline IORegistryEntry *IORegistryEntry::fromPath(const char *path, const IORegistryPlane *plane)
{
    return kstub_root ? kstub_root->childFromPath(path, plane) : nullptr;
}

extern "C" {
void IOLog(const char *format, ...) __attribute__((format(printf, 1, 2)));
typedef int boolean_t;
boolean_t PE_parse_boot_argn(const char *name, void *value, int size);
}
#endif
