// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026, UniXNU contributors. All rights reserved.
// Host test of the iOS haptics switch (HapticsIos.c, PaganiHapticsIos.cpp) on the IORegistry model of Test/iosstubs: for each boot
// argument case the device tree the kernel leaves, and the answers libMobileGestalt (21H461) works out from it, the way it works them out
// (MGIOKitSupport.c, read in IDA): DeviceSupportsHaptics iterates the IODeviceTree plane recursively from its root
// (IORegistryCreateIterator(kIOMainPortDefault, "IODeviceTree", kIORegistryIterateRecursively), -[MGIOKitHelper
// copyDeviceTreeStructureNext:withFirstChar:]) for a node whose IORegistryEntryGetName starts with 'h' and whose obfuscated name is
// 6S9CvPHPtzHQqKudHSfsag (handler 0x1a0229dd4); deviceSupports3RdPartyHaptics (0x1a021f1a8) and DeviceSupportsClosedLoopHaptics
// (0x1a021d38c) open IODeviceTree:/product/haptics (IORegistryEntryFromPath), list its IORegistryEntryPropertyKeys and take the key whose
// first character is 's' / 'c' and whose obfuscated name is OtnrTIhWBtyPo45BcApMTw / XKpK5v7PGzbe7igjloNBpQ; closed-loop counts when it
// is a number or 4 bytes, not 0. The obfuscation is base64(md5("MGCopyAnswer" + name)) cut at 22 characters (sub_1A020CA7C).
// Build: c++ -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined -I iosstubs -I .. ios.cpp ../HapticsIos.c -o ios
//        [run: ios MG_KEYS_TSV]  (the haptics RE's mg/haptics-keys.tsv: its "haptics" key must be the node name's hash)
#include <stdlib.h>
#include <fstream>
#include <functional>
#include <sstream>
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#include <CommonCrypto/CommonDigest.h>
#include "../PaganiHapticsIos.cpp"

static unsigned checks;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #x); abort(); } checks++; } while (0)

// ---- the kernel, as the switch sees it -------------------------------------------------------------------------------------------
int kstub_alloc_fail, kstub_property_fail, kstub_attach_fail;
int IOService::kstub_live;
IORegistryEntry *IORegistryEntry::kstub_root;
static const IORegistryPlane kDTPlane = {"IODeviceTree"}, kServicePlane = {"IOService"};
const IORegistryPlane *gIODTPlane = &kDTPlane;
static std::map<std::string, unsigned> gArgs;
static std::vector<std::string> gLines;
static unsigned gIOLogs;
static int gActuatorOffered;
extern "C" int pagani_audio_actuator_enabled(void) {return gActuatorOffered;}   // the audio service's answer (replaces HapticsIos.c's weak one)
void IOLog(const char *, ...) {gIOLogs++;}
boolean_t PE_parse_boot_argn(const char *name, void *value, int size)
{
    auto it = gArgs.find(name);
    if (it == gArgs.end()) return 0;
    CHECK(size == sizeof(unsigned));
    memcpy(value, &it->second, sizeof(unsigned));
    return 1;
}
extern "C" void boot_progress_trace(const char *line) {gLines.push_back(line);}

static IORegistryEntry *node(IORegistryEntry *parent, const char *name)
{
    IOService *e = new IOService;
    e->setName(name);
    OSData *d = OSData::withBytes(name, (unsigned)strlen(name) + 1);
    e->setProperty("name", d);
    d->release();
    if (parent) {
        CHECK(e->attachToParent(parent, gIODTPlane));
        e->release();
    }
    return e;
}

// A device tree like the 13T's: the root, /product (with the properties PaganiArmDTPlatform gives it), /arm-io with nodes, /chosen.
static void boot(bool product = true, bool haptics_there = false)
{
    kstub_alloc_fail = kstub_property_fail = kstub_attach_fail = 0;
    if (IORegistryEntry::kstub_root) IORegistryEntry::kstub_root->release();
    CHECK(IOService::kstub_live == 0);
    IORegistryEntry *root = node(nullptr, "device-tree");
    IORegistryEntry::kstub_root = root;
    if (product) {
        IORegistryEntry *p = node(root, "product");
        const uint32_t id = 0x80ac7ae0;
        OSData *d = OSData::withBytes(&id, 4);
        p->setProperty("product-id", d);
        d->release();
        if (haptics_there) node(p, "haptics");
    }
    IORegistryEntry *io = node(root, "arm-io");
    node(io, "hapt0");      // starts with 'h', another name: must not count
    node(io, "spmi");
    node(root, "chosen");
    // An entry of another plane only: never part of the device tree's answers.
    IOService *other = new IOService;
    other->setName("haptics");
    other->kstub_parent[&kServicePlane] = root;
    root->kstub_children[&kServicePlane].push_back(other);
    gArgs.clear();
    gLines.clear();
    gIOLogs = 0;
    gIosDone = 0;
    gIosCommitted = 0;
    gIosPlan = PghIosPlan();
    gActuatorOffered = 1;
    memset((void *)pagani_haptics_ios_status, 0, sizeof pagani_haptics_ios_status);
}

// ---- libMobileGestalt, as it reads the tree --------------------------------------------------------------------------------------
static std::string obfuscated(const std::string &name)
{
    const std::string text = "MGCopyAnswer" + name;
    unsigned char md5[CC_MD5_DIGEST_LENGTH];
    CC_MD5(text.data(), (CC_LONG)text.size(), md5);
    static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (int i = 0; i < 16; i += 3) {
        const uint32_t v = (uint32_t)md5[i] << 16 | (i + 1 < 16 ? (uint32_t)md5[i + 1] << 8 : 0) | (i + 2 < 16 ? md5[i + 2] : 0);
        out += table[v >> 18 & 63];
        out += table[v >> 12 & 63];
        out += i + 1 < 16 ? table[v >> 6 & 63] : '=';
        out += i + 2 < 16 ? table[v & 63] : '=';
    }
    return out.substr(0, 22);
}
static bool matches(const std::string &name, const char *hash, char first) {return !name.empty() && name[0] == first && obfuscated(name) == hash;}

static const char kNodeHash[] = "6S9CvPHPtzHQqKudHSfsag", kClosedLoopHash[] = "XKpK5v7PGzbe7igjloNBpQ",
                  kThirdPartyHash[] = "OtnrTIhWBtyPo45BcApMTw";

// IORegistryCreateIterator(root, "IODeviceTree", recursive): every descendant of the plane's root, depth first.
static bool mgDeviceSupportsHaptics(void)
{
    std::function<bool(IORegistryEntry *)> walk = [&](IORegistryEntry *e) {
        for (IORegistryEntry *c : e->kstub_children[gIODTPlane]) {
            if (matches(c->getName(), kNodeHash, 'h')) return true;
            if (walk(c)) return true;
        }
        return false;
    };
    return IORegistryEntry::kstub_root && walk(IORegistryEntry::kstub_root);
}
// IORegistryEntryFromPath("IODeviceTree:/product/haptics"), its IORegistryEntryPropertyKeys, the key by hash and first character.
static OSObject *mgProductHapticsProperty(const char *hash, char first)
{
    IORegistryEntry *e = IORegistryEntry::fromPath("/product/haptics", gIODTPlane);
    if (!e) return nullptr;
    OSObject *found = nullptr;
    for (auto &p : e->kstub_props) if (matches(p.first, hash, first)) found = p.second;
    e->release();
    return found;
}
static bool mgClosedLoop(void)
{
    const OSData *d = dynamic_cast<const OSData *>(mgProductHapticsProperty(kClosedLoopHash, 'c'));
    uint32_t v = 0;
    if (!d || d->getLength() != 4) return false;
    memcpy(&v, d->getBytesNoCopy(), 4);
    return v != 0;
}
static bool mgThirdParty(void) {return mgProductHapticsProperty(kThirdPartyHash, 's') != nullptr;}

static unsigned nodes(void)
{
    unsigned n = 0;
    std::function<void(IORegistryEntry *)> walk = [&](IORegistryEntry *e) {
        for (IORegistryEntry *c : e->kstub_children[gIODTPlane]) {n++; walk(c);}
    };
    walk(IORegistryEntry::kstub_root);
    return n;
}

// ---- tests ------------------------------------------------------------------------------------------------------------------------
static void test_hashes(const char *tsv)
{
    // The obfuscation reproduces libMobileGestalt's keys of 21H461: the answers' own (haptics-keys.tsv) and the device tree names.
    CHECK(obfuscated("DeviceSupportsHaptics") == "cBy4BcYs5YWtFHbBpt4C6A");
    CHECK(obfuscated("DeviceSupportsClosedLoopHaptics") == "gqDnklGQnpv5ilgh5uHckw");
    CHECK(obfuscated(PGH_IOS_NODE) == kNodeHash && obfuscated(PGH_IOS_CLOSED_LOOP) == kClosedLoopHash &&
          obfuscated(PGH_IOS_THIRD_PARTY) == kThirdPartyHash);
    if (tsv) {
        std::ifstream in(tsv);
        std::stringstream text;
        text << in.rdbuf();
        const std::string t = text.str();
        CHECK(!t.empty() && t.find(std::string(kNodeHash) + "\thaptics") != std::string::npos);
        CHECK(t.find("cBy4BcYs5YWtFHbBpt4C6A\tDeviceSupportsHaptics") != std::string::npos);
        CHECK(t.find("gqDnklGQnpv5ilgh5uHckw\tDeviceSupportsClosedLoopHaptics") != std::string::npos);
        CHECK(t.find("closed-loop") != std::string::npos && t.find("supports-3rd-party-haptics") != std::string::npos);
    }
}

static void test_off(void)
{
    // pagani-haptics-ios absent or 0: nothing at all, whatever else is set.
    boot();
    const unsigned before = nodes();
    gArgs = {{"pagani-audio", 1}};
    pagani_haptics_ios_publish(0, 1);
    pagani_haptics_ios_commit(1, 1);
    CHECK(nodes() == before && gLines.empty() && gIOLogs == 0 && pagani_haptics_ios_status[0] == 0 && !gIosDone && !gIosCommitted);
    CHECK(!mgDeviceSupportsHaptics() && !mgThirdParty() && !mgClosedLoop());
}

static void test_stage(unsigned ios)
{
    boot();
    const unsigned before = nodes();
    gArgs = {{"pagani-audio", 1}};
    pagani_haptics_ios_publish(ios, 1);
    // During the kernel's start only the plan: nothing in the tree, nothing in the log.
    CHECK(nodes() == before && gLines.empty() && gIOLogs == 0 && pagani_haptics_ios_status[0] == 1 && !mgDeviceSupportsHaptics());
    // Just before launchd, with the plug-in and the tunings visible.
    pagani_haptics_ios_commit(1, 1);
    CHECK(nodes() == before + 1 && gLines.size() == 1 && gIOLogs == 1);
    CHECK(pagani_haptics_ios_status[0] == (ios == 2 ? 'B' : 'A') && pagani_haptics_ios_status[1] == (ios == 2) &&
          pagani_haptics_ios_status[2] == 1);
    CHECK(gLines[0].find(ios == 2 ? "[haptics] ios stage B: /product/haptics created" : "[haptics] ios stage A: /product/haptics created") == 0);
    // The node as IODeviceTreeAlloc makes one: in the device tree plane under /product, its registry name and "name" property.
    IORegistryEntry *n = IORegistryEntry::fromPath("/product/haptics", gIODTPlane);
    CHECK(n && !strcmp(n->getName(), "haptics") && n->kstub_parent[gIODTPlane] && !strcmp(n->kstub_parent[gIODTPlane]->getName(), "product"));
    const OSData *name = dynamic_cast<const OSData *>(n->kstub_props["name"]);
    CHECK(name && name->getLength() == 8 && !memcmp(name->getBytesNoCopy(), "haptics", 8));
    CHECK(n->kstub_props.size() == (ios == 2 ? 3u : 2u) && n->kstub_refs == 2);   // the plane's reference and this lookup's
    n->release();
    // What libMobileGestalt answers from it.
    CHECK(mgDeviceSupportsHaptics() && mgThirdParty() && mgClosedLoop() == (ios == 2));
    // Once per boot: a second call changes nothing.
    pagani_haptics_ios_publish(ios, 1);
    pagani_haptics_ios_commit(1, 1);
    CHECK(nodes() == before + 1 && gLines.size() == 1);
}

static void test_commit(void)
{
    // A kernel or image that never gets to the commit (no overlay patch, sheng-gpu-addon not 1): the plan stays a plan, no node.
    boot();
    unsigned before = nodes();
    gArgs = {{"pagani-audio", 1}};
    pagani_haptics_ios_publish(2, 1);
    CHECK(nodes() == before && !mgDeviceSupportsHaptics() && !mgClosedLoop());
    // Stage B without the tunings overlay: never closed-loop (the haptic server would fail every client with -43): stage A, and said so.
    boot();
    before = nodes();
    gArgs = {{"pagani-audio", 1}};
    pagani_haptics_ios_publish(2, 1);
    pagani_haptics_ios_commit(1, 0);
    CHECK(nodes() == before + 1 && pagani_haptics_ios_status[0] == 'A' && pagani_haptics_ios_status[1] == 0 && pagani_haptics_ios_status[3] == 2);
    CHECK(gLines.size() == 1 && gLines[0].find("[haptics] ios stage A: /product/haptics created") == 0 &&
          gLines[0].find("no closed-loop: hapticengineconfig.plist is not visible") != std::string::npos);
    CHECK(mgDeviceSupportsHaptics() && mgThirdParty() && !mgClosedLoop());
    // Stage A does not ask for the tunings.
    boot();
    before = nodes();
    gArgs = {{"pagani-audio", 1}};
    pagani_haptics_ios_publish(1, 1);
    pagani_haptics_ios_commit(1, 0);
    CHECK(nodes() == before + 1 && pagani_haptics_ios_status[0] == 'A' && gLines[0].find("not visible") == std::string::npos);
    // No plug-in on the root, or the audio service not offering the actuator: no node, or VirtualAudio would wait 8 s for a device.
    for (int which = 0; which < 2; ++which) {
        boot();
        before = nodes();
        gArgs = {{"pagani-audio", 1}};
        gActuatorOffered = which == 0;
        pagani_haptics_ios_publish(2, 1);
        pagani_haptics_ios_commit(which == 1, 1);
        CHECK(nodes() == before && pagani_haptics_ios_status[0] == -6 && gLines.size() == 1 && !mgDeviceSupportsHaptics() && !mgClosedLoop());
        CHECK(gLines[0].find("[haptics] ios node not created: ") == 0);
        CHECK(gLines[0].find(which ? "does not offer the actuator" : "not on the root") != std::string::npos);
    }
    // The rule by itself.
    PghIosPlan plan, final;
    CHECK(pgh_ios_plan(2, 1, 1, &plan) == 0);
    CHECK(pgh_ios_final(&plan, 1, 1, 1, &final) == 0 && final.create && final.stage == 'B' && final.closed_loop && !final.why[0]);
    CHECK(pgh_ios_final(&plan, 1, 1, 0, &final) == 0 && final.create && final.stage == 'A' && !final.closed_loop && final.third_party);
    CHECK(pgh_ios_final(&plan, 1, 0, 1, &final) == -1 && !final.create && !final.stage);
    CHECK(pgh_ios_final(&plan, 0, 1, 1, &final) == -1 && !final.create);
    CHECK(pgh_ios_final(nullptr, 1, 1, 1, &final) == -1 && pgh_ios_final(&plan, 1, 1, 1, nullptr) == -1);
    CHECK(pgh_ios_plan(0, 1, 1, &plan) == 0 && pgh_ios_final(&plan, 1, 1, 1, &final) == 0 && !final.create);
}

static void test_refusals(void)
{
    struct Case {unsigned ios, haptics; int audio; const char *why;};
    const Case cases[] = {{1, 0, 1, "needs pagani-haptics=1"}, {1, 2, 1, "needs pagani-haptics=1"}, {2, 1, -1, "needs pagani-audio=1"},
                          {1, 1, 0, "needs pagani-audio=1"}, {3, 1, 1, "takes 1 (stage A) or 2 (stage B)"}};
    for (const Case &c : cases) {
        boot();
        const unsigned before = nodes();
        if (c.audio >= 0) gArgs = {{"pagani-audio", (unsigned)c.audio}};
        pagani_haptics_ios_publish(c.ios, c.haptics);
        CHECK(nodes() == before && pagani_haptics_ios_status[0] == -1 && gLines.size() == 1);
        CHECK(gLines[0].find("[haptics] ios node not created: ") == 0 && gLines[0].find(c.why) != std::string::npos);
        CHECK(!mgDeviceSupportsHaptics() && !mgThirdParty() && !mgClosedLoop());
    }
    // The plan itself: off is no refusal.
    PghIosPlan plan;
    CHECK(pgh_ios_plan(0, 1, 1, &plan) == 0 && !plan.create && !strcmp(plan.why, "off"));
    CHECK(pgh_ios_plan(1, 1, 1, &plan) == 0 && plan.create && plan.stage == 'A' && plan.third_party && !plan.closed_loop);
    CHECK(pgh_ios_plan(2, 1, 7, &plan) == 0 && plan.create && plan.stage == 'B' && plan.third_party && plan.closed_loop);
    CHECK(pgh_ios_plan(1, 1, 1, nullptr) == -1);
}

static void test_failures(void)
{
    // No /product: nothing created.
    boot(false);
    unsigned before = nodes();
    gArgs = {{"pagani-audio", 1}};
    pagani_haptics_ios_publish(2, 1);
    pagani_haptics_ios_commit(1, 1);
    CHECK(nodes() == before && pagani_haptics_ios_status[0] == -2 && gLines.size() == 1 && !mgDeviceSupportsHaptics());
    // A board whose device tree has its own /product/haptics: left as it is.
    boot(true, true);
    before = nodes();
    gArgs = {{"pagani-audio", 1}};
    pagani_haptics_ios_publish(2, 1);
    pagani_haptics_ios_commit(1, 1);
    CHECK(nodes() == before && pagani_haptics_ios_status[0] == -3 && gLines[0].find("already there") != std::string::npos);
    CHECK(mgDeviceSupportsHaptics() && !mgClosedLoop());   // the board's own node, untouched
    // A property or the attach fails: the tree stays as it was and the new entry is freed.
    for (int which = 0; which < 3; ++which) {
        boot();
        before = nodes();
        const int live = IOService::kstub_live;
        gArgs = {{"pagani-audio", 1}};
        kstub_alloc_fail = which == 0;
        kstub_property_fail = which == 1;
        kstub_attach_fail = which == 2;
        pagani_haptics_ios_publish(2, 1);
        pagani_haptics_ios_commit(1, 1);
        CHECK(nodes() == before && pagani_haptics_ios_status[0] == -5 && IOService::kstub_live == live && !mgDeviceSupportsHaptics());
    }
}

int main(int argc, char **argv)
{
    test_hashes(argc > 1 ? argv[1] : nullptr);
    test_off();
    test_stage(1);
    test_stage(2);
    test_commit();
    test_refusals();
    test_failures();
    if (IORegistryEntry::kstub_root) IORegistryEntry::kstub_root->release();
    IORegistryEntry::kstub_root = nullptr;
    CHECK(IOService::kstub_live == 0);
    printf("PASS ios: %u checks (stage A and B nodes as libMobileGestalt finds them, the commit before launchd, refusals, failures%s)\n", checks,
           argc > 1 ? ", haptics-keys.tsv" : "");
    return 0;
}
#pragma clang diagnostic pop
