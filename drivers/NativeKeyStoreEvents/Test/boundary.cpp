// SPDX-License-Identifier: BSD-2-Clause-Patent
// The open/answer boundaries of the real service against the host lifecycle double (not real IOKit registration): without
// sheng-aks every open is refused; with it the user client answers the handshake/bookkeeping selectors, get_system (14), lock
// state (7) and device state (17, 35, decoded the way libaks decodes it, inline and through a prepared descriptor), the volatile
// keybag's selectors (8, 10, 11, 31, 32) unless pagani-keybag=0, and refuses everything else with kIOReturnUnsupported.
#include "host.h"
#include <initializer_list>
#include <set>

using namespace host;

static void closedBoundary() {
    freshBoot();
    stateEnabled = false;
    refused = opened = 0;
    auto *device = static_cast<AppleKeyStore *>(native_keyevents_allocate());
    IOService *base = device;
    CHECK(base->start(nullptr) && device->publications == 1);
    struct Probe : IOUserClient {
        IOReturn externalMethod(uint32_t, IOExternalMethodArguments *, IOExternalMethodDispatch *, OSObject *, void *) override { return 0; }
    } sentinel;
    for (UInt32 type : {0u, 1u, 7u, 0xffffffffu}) {
        IOUserClient *out = &sentinel;
        CHECK(base->newUserClient(nullptr, nullptr, type, nullptr, &out) == kIOReturnNotReady && !out);
        out = &sentinel;
        CHECK(base->newUserClient(nullptr, nullptr, type, &out) == kIOReturnNotReady && !out);
        CHECK(base->newUserClient(nullptr, nullptr, type, nullptr, nullptr) == kIOReturnNotReady);
    }
    CHECK(refused == 12 && !opened);
    static_cast<OSObject *>(device)->release();
}

static void stateAnswers(long bootArg) {
    freshBoot();
    opened = 0;
    Service s(bootArg);
    IOUserClient *client = s.client;
    CHECK(client->started && opened == 1);
    // Lock state: one scalar in, one out.
    Result r = call(client, 7, {0}, {}, 1, 0);
    CHECK(r.rc == kIOReturnSuccess && r.scalars.size() == 1 && r.scalars[0] == 4);
    CHECK(call(client, 7, {0, 0}, {}, 1, 0).rc == kIOReturnBadArgument);
    // get_system hands out the system bag handle 0, without (as securityd calls it) or with the special handle.
    r = call(client, 14, {}, {}, 1, 0);
    CHECK(r.rc == kIOReturnSuccess && r.scalars.size() == 1 && r.scalars[0] == 0);
    CHECK(call(client, 14, {}, {}, 0, 0).rc == kIOReturnBadArgument);
    for (uint32_t sel : {0u, 4u, 5u, 9u, 12u, 13u, 26u, 27u, 55u, 101u, 102u}) {
        r = call(client, sel, {}, {}, 0, 0);
        CHECK(r.rc == kIOReturnSuccess && r.scalars.empty());
    }
    for (uint32_t want : {0u, 3u}) {
        lockState = want;
        for (uint32_t sel : {17u, 35u}) {
            r = call(client, sel, {0}, {}, 0, 0x1000);   // libaks __get_device_state: 4096 bytes, inline
            CHECK(r.rc == kIOReturnSuccess && r.out.size() > 20 && r.out.size() < 200);
            std::map<std::string, DerValue> state;
            CHECK(libaksDecode(r.out.data(), r.out.size(), state));
            CHECK(state.size() == 10 && derInteger(state["sls"]) == want && derInteger(state["ss"]) == 4 &&
                  derInteger(state["bh"]) == 0 && state.count("sgpe") && state.count("srcd"));
            // A larger output arrives as a descriptor: written only after prepare(), completed after.
            IOMemoryDescriptor descriptor;
            Result d = call(client, sel, {0}, {}, 0, 0, &descriptor);
            CHECK(d.rc == kIOReturnSuccess && d.out == r.out && descriptor.prepares == 1 && descriptor.completes == 1 && !descriptor.prepared);
            IOMemoryDescriptor tiny; tiny.length = 10;
            CHECK(call(client, sel, {0}, {}, 0, 0, &tiny).rc == kIOReturnNoSpace && !tiny.prepares);
            IOMemoryDescriptor huge; huge.length = (1u << 20) + 1;     // never wired, however little goes into it
            CHECK(call(client, sel, {0}, {}, 0, 0, &huge).rc == kIOReturnBadArgument && !huge.prepares);
            IOMemoryDescriptor unwired; unwired.prepareResult = kIOReturnVMError;
            CHECK(call(client, sel, {0}, {}, 0, 0, &unwired).rc == kIOReturnVMError && !unwired.completes);
            CHECK(call(client, sel, {}, {}, 0, 0x1000).rc == kIOReturnSuccess);
            CHECK(call(client, sel, {0}, {}, 0, 10).rc == kIOReturnNoSpace);
            CHECK(call(client, sel, {0}, {}, 1, 0x1000).rc == kIOReturnBadArgument);
        }
    }
    // A missing argument block is libaks's own "no connection" error.
    CHECK(client->externalMethod(7, nullptr) == kIOReturnError);
    // The selector table: every selector up to 300 with harmless arguments.
    const std::set<uint32_t> handshake{0, 4, 5, 9, 12, 13, 26, 27, 55, 101, 102}, state{7, 14, 17, 35}, keybag{8, 10, 11, 31, 32};
    for (uint32_t sel = 0; sel <= 300; ++sel) {
        r = call(client, sel, {0, 6}, bytes(32, 1), 1, 64);
        if (handshake.count(sel)) CHECK(r.rc == kIOReturnSuccess);
        else if (state.count(sel)) CHECK(r.rc != kIOReturnUnsupported);
        else if (keybag.count(sel)) CHECK(bootArg == 0 ? r.rc == kIOReturnUnsupported : r.rc != kIOReturnUnsupported);
        else CHECK(r.rc == kIOReturnUnsupported);
    }
    {
        std::lock_guard<std::mutex> g(callsMutex);
        CHECK(calls.size() > 300);   // Stage7Probe sees every call with its result
    }
    CHECK(client->clientClose() == kIOReturnSuccess && client->terminated);
}

static void startFailures() {
    freshBoot();
    AppleKeyStore failed;
    failed.baseOK = false;
    CHECK(!failed.start(nullptr) && !failed.publications && !failed.properties);
    for (unsigned which : {1u, 2u}) {
        freshBoot();
        AppleKeyStore propertyFailure;
        propertyFailure.failProperty = which;
        CHECK(!propertyFailure.start(nullptr));
        CHECK(!propertyFailure.publications && propertyFailure.stops == 1);
        CHECK(!gKeybag.lock && !gKeybag.mode && locksAlive == 0);   // a service that did not start leaves no keybag behind
    }
}

int main() {
    closedBoundary();
    stateAnswers(-1);   // pagani-keybag absent: on
    stateAnswers(0);    // pagani-keybag=0: the table before the keybag, exactly
    startFailures();
    CHECK(locksAlive == 0);
    printf("PASS: closed boundary by default; state mode answers the state/bookkeeping selectors (device state also through a prepared "
           "descriptor), the keybag selectors 8/10/11/31/32 only without pagani-keybag=0, and rejects every other selector up to 300\n");
    return 0;
}
