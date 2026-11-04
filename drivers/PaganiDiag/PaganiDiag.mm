// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026, UniXNU contributors. All rights reserved.
// PaganiDiag: a root daemon that checks, for the morning's acceptance runs, which features of the iOS side work (README.md). At 60, 180
// and 600 s after boot, at background priority: MobileGestalt's answers, the power sources, the HID services by usage, the IOKit services
// of the board's drivers, the keychain, the audio route, the sensors and haptics as the frameworks see them, the widget extensions as
// LaunchServices has them, the interfaces, the clock and the Data volume's free space. Each line goes to the kernel through
// kern.pagani_diag_line (phone-diag-sysctl.patch), or its spool file when that write is refused, which logs it as a BootProgress U line
// "diag: ...". What a line says and how many there are is DiagCore.cpp (a strict budget: the 32 KiB events ring is shared); this file
// collects. The process table and the key counters come from the kernel itself. Off unless boot-arg pagani-diag is not 0: the kernel
// starts this program in place of /usr/libexec/DumpPanic (argv[0] stays that path). /private/var/factory_mount/paganidiag.conf
// (optional, DiagCore Config keys) changes the defaults.
//
// With migration=1 in that file (and boot-arg pagani-diag=2, the long budget) also the passes that say why the deletable system apps do
// not install: every 30 s installd's system app states, the staged apps, the migration sentinel and the unified-log lines of the
// migration plug-in host, datamigrator, installd and installcoordinationd; on the first pass (and every eighth) the answers the
// migration's checks get.
//
// Everything that talks to another daemon or loads foreign code runs in a child (this executable with --probe), killed after a
// timeout, so a hang or a crash costs one probe, not the boot's lines:
//   paganidiag --probe mg|pwr|hid|kc|aud|sen|hap|wid   V<TAB>key<TAB>value lines (a feature round)
//   paganidiag --probe facts        V<TAB>key<TAB>value lines
//   paganidiag --probe state        S<TAB>bundle ID<TAB>state lines and SN<TAB>count, or SE<TAB>why; MIG<TAB>0|1 or MIGE<TAB>why
//   paganidiag --probe log SINCE S  L lines (DiagCore entry_line) of the entries after SINCE (seconds since 1970; 0: since boot),
//                                   P<TAB>time of the last entry seen, N<TAB>seen<TAB>kept<TAB>predicate<TAB>ms, E<TAB>error; it
//                                   stops reading 3 s before S, the seconds the daemon waits for it
//   paganidiag --print              the daemon, writing the lines to standard output instead of the kernel
#import <Foundation/Foundation.h>
#import <OSLog/OSLog.h>
#include <CoreFoundation/CoreFoundation.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <mach/mach.h>
#include <mach-o/dyld.h>
#include <net/if.h>
#include <notify.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/attr.h>
#include <sys/file.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <algorithm>
#include <map>
#include <string>
#include <vector>
#include "DiagCore.h"

extern char **environ;

// OSLogStore's factory initializer (private; +[OSLogStore storeWithURL:error:] uses it), declared so ARC treats it as an init method.
@interface OSLogStore (PaganiDiagFactory)
- (instancetype)initForFactory;
@end

// LaunchServices' records (private; CoreServices, loaded at run time): the initializers the census calls, declared for the same reason.
@interface NSObject (PaganiDiagRecords)
- (instancetype)initWithBundleIdentifier:(NSString *)identifier requireValid:(BOOL)valid error:(NSError **)error;
- (instancetype)initWithIdentifier:(NSString *)identifier error:(NSError **)error;
@end

namespace {

using namespace pagani_diag;

constexpr const char *kSelfPath = "/private/var/factory_mount/paganidiag";    // PAGANI_DIAG_DAEMON
constexpr const char *kConfigPath = "/private/var/factory_mount/paganidiag.conf";
constexpr const char *kLockPath = "/private/var/tmp/paganidiag.lock";
constexpr const char *kSpoolPath = "/private/var/db/paganidiag.spool";          // PAGANI_DIAG_SPOOL
constexpr const char *kDataVolume = "/private/var";
constexpr const char *kTimezoneLink = "/private/var/db/timezone/localtime";
constexpr double kProbeSeconds = 20;    // a feature probe's time: at background priority, a framework's first load is slow
constexpr const char *kStatePath = "/private/var/installd/Library/MobileInstallation/BackedUpState/SystemAppInstallState.plist";
constexpr const char *kForcePath = "/private/var/preferences/com.apple.demo.SADForceInstall.plist";
constexpr const char *kBundlesPath = "/private/var/containers/Bundle/Application";
constexpr const char *kStagedPath = "/private/var/staged_system_apps";
constexpr const char *kSentinelPath = "/private/var/datamigrator/migrate";
// The logging preference that keeps DataMigration's info lines (every SystemAppMigrator line is OS_LOG_TYPE_INFO, and the
// com.apple.migration subsystem inherits the default, which does not keep info): /Library/Preferences/Logging is this directory.
constexpr const char *kLogPreferenceDirectory = "/private/var/preferences/Logging/Subsystems";
constexpr const char *kLogPreferencePath = "/private/var/preferences/Logging/Subsystems/com.apple.migration.plist";
constexpr const char *kLogPreferencesChanged = "com.apple.system.logging.prefschanged";
constexpr const char *kMobilePreferences = "/private/var/mobile/Library/Preferences/com.apple.mobile.installation.plist";
constexpr const char *kRootPreferences = "/private/var/root/Library/Preferences/com.apple.mobile.installation.plist";
constexpr const char *kMobileGestalt = "/usr/lib/libMobileGestalt.dylib";
constexpr const char *kIOKit = "/System/Library/Frameworks/IOKit.framework/IOKit";
constexpr const char *kSecurity = "/System/Library/Frameworks/Security.framework/Security";
constexpr const char *kAVFAudio = "/System/Library/Frameworks/AVFAudio.framework/AVFAudio";
constexpr const char *kCoreMotion = "/System/Library/Frameworks/CoreMotion.framework/CoreMotion";
constexpr const char *kCoreHaptics = "/System/Library/Frameworks/CoreHaptics.framework/CoreHaptics";
constexpr const char *kCoreServices = "/System/Library/Frameworks/CoreServices.framework/CoreServices";
constexpr const char *kAppsDirectory = "/Applications";
constexpr const char *kWidgetPoint = "com.apple.widgetkit-extension";
constexpr const char *kKeychainItem = "org.unixnu.pagani.diag";                 // keychain-access-groups of paganidiag.entitlements
constexpr const char *kMobileInstallation = "/System/Library/PrivateFrameworks/MobileInstallation.framework/MobileInstallation";
constexpr const char *kInstalledContentLibrary = "/System/Library/PrivateFrameworks/InstalledContentLibrary.framework/InstalledContentLibrary";
// SystemAppMigrator's own copy of MIIsApplicableToCurrentDeviceFamilyWithError (exported by the bundle; in InstalledContentLibrary it is
// local, so dlsym finds it only here).
constexpr const char *kSystemAppMigrator = "/System/Library/DataClassMigrators/SystemAppMigrator.migrator/SystemAppMigrator";
constexpr const char *kLineSysctl = "kern.pagani_diag_line";
constexpr const char *kDroppedSysctl = "kern.pagani_diag_dropped";

double monotonic() { return clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) / 1e9; }

// Seconds since boot on the clock the kernel's rounds use (mach_absolute_time): the wall clock of this phone may jump.
long uptime_seconds() { return long(clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000000000ull); }

double wall() {
    struct timeval now = {};
    gettimeofday(&now, nullptr);
    return now.tv_sec + now.tv_usec / 1e6;
}

void sleep_until(double when) {
    for (double now = monotonic(); now < when; now = monotonic()) {
        const double left = when - now;
        struct timespec span = {time_t(left), long((left - time_t(left)) * 1e9)};
        nanosleep(&span, nullptr);
    }
}

double boot_time() {
    struct timeval boot = {};
    size_t size = sizeof boot;
    if (sysctlbyname("kern.boottime", &boot, &size, nullptr, 0) != 0) return 0;
    return boot.tv_sec + boot.tv_usec / 1e6;
}

std::string sysctl_string(const char *name) {
    char value[256] = {};
    size_t size = sizeof value - 1;
    if (sysctlbyname(name, value, &size, nullptr, 0) != 0) return "-";
    return value[0] ? std::string(value) : std::string("-");
}

std::string utf8(NSString *string) { return string.UTF8String ? std::string(string.UTF8String) : std::string("?"); }

// A property list or Objective-C value as one short word: numbers and booleans as digits, strings as they are, arrays as "[a,b]".
std::string describe(id value) {
    if (!value) return "-";
    if ([value isKindOfClass:[NSNumber class]]) {
        if (CFGetTypeID((__bridge CFTypeRef)value) == CFBooleanGetTypeID()) return [value boolValue] ? "1" : "0";
        return utf8([value stringValue]);
    }
    if ([value isKindOfClass:[NSString class]]) return utf8(value);
    if ([value isKindOfClass:[NSArray class]]) {
        std::string out = "[";
        for (id item in (NSArray *)value) {
            if (out.size() > 1) out += ',';
            out += describe(item);
            if (out.size() > 100) {
                out += "...";
                break;
            }
        }
        return out + "]";
    }
    if ([value isKindOfClass:[NSDictionary class]]) return "{" + std::to_string([(NSDictionary *)value count]) + "}";
    return "?" + utf8(NSStringFromClass([value class]));
}

std::string error_text(NSError *error) {
    if (!error) return "-";
    NSString *domain = error.domain ?: @"?";
    if ([domain hasSuffix:@"ErrorDomain"]) domain = [domain substringToIndex:domain.length - 11];
    return "e" + std::to_string((long)error.code) + " " + utf8(domain) + " " + utf8(error.localizedDescription ?: @"");
}

// ---- children -----------------------------------------------------------------------------------------------------------------

// A value for a probe line: tabs and line breaks (error descriptions print arrays over several lines) as spaces.
std::string one_line(std::string text) {
    for (char &c : text)
        if (c == '\t' || c == '\n' || c == '\r') c = ' ';
    return text;
}

void put(const char *key, const std::string &value) {
    printf("V\t%s\t%s\n", key, one_line(value).c_str());
    fflush(stdout);
}

std::string preference_value(const char *path, NSString *key) {
    NSDictionary *preferences = [NSDictionary dictionaryWithContentsOfFile:@(path)];
    if (!preferences) return access(path, F_OK) == 0 ? "unreadable" : "-";
    return describe(preferences[key]);
}

int probe_facts() {
    @autoreleasepool {
        // MobileGestalt, as SystemAppMigrator and InstalledContentLibrary ask it.
        void *gestalt = dlopen(kMobileGestalt, RTLD_NOW | RTLD_LOCAL);
        auto copy_answer = gestalt ? (CFTypeRef (*)(CFStringRef, CFDictionaryRef))dlsym(gestalt, "MGCopyAnswer") : nullptr;
        for (NSString *key in @[@"SupportedDeviceFamilies", @"DeviceClassNumber", @"DeviceClass", @"ProductType", @"HWModelStr", @"arm64"]) {
            const std::string name = "mg." + utf8(key);
            if (!copy_answer) {
                put(name.c_str(), "nomg");
                continue;
            }
            CFTypeRef answer = copy_answer((__bridge CFStringRef)key, nullptr);
            put(name.c_str(), describe((__bridge id)answer));
            if (answer) CFRelease(answer);
        }
        for (const char *name : {"hw.product", "hw.target", "hw.machine", "hw.model"}) put(name, sysctl_string(name));
        host_preferred_user_arch_data_t arch = {};
        mach_msg_type_number_t count = HOST_PREFERRED_USER_ARCH_COUNT;
        char text[48];
        if (host_info(mach_host_self(), HOST_PREFERRED_USER_ARCH, (host_info_t)&arch, &count) == KERN_SUCCESS)
            snprintf(text, sizeof text, "%x/%x", (unsigned)arch.cpu_type, (unsigned)arch.cpu_subtype);
        else
            snprintf(text, sizeof text, "fail");
        put("arch", text);
        // The force install list as -[MISystemAppMigrator init] reads it.
        NSError *error = nil;
        NSArray *force = [NSArray arrayWithContentsOfURL:[NSURL fileURLWithPath:@(kForcePath)] error:&error];
        if (force) {
            unsigned strings = 0;
            for (id item in force) strings += [item isKindOfClass:[NSString class]];
            put("force", "arr " + std::to_string(force.count) + " str " + std::to_string(strings));
        } else {
            put("force", error_text(error));
        }
        struct stat info = {};
        if (stat(kForcePath, &info) == 0) {
            snprintf(text, sizeof text, "%u:%u %o", (unsigned)info.st_uid, (unsigned)info.st_gid, (unsigned)(info.st_mode & 07777));
            put("force.stat", text);
        } else {
            put("force.stat", "e" + std::to_string(errno));
        }
        put("skip.mobile", preference_value(kMobilePreferences, @"SkipDeviceFamilyCheck"));
        put("skip.root", preference_value(kRootPreferences, @"SkipDeviceFamilyCheck"));
        // installd's capability check (no entitlement needed).
        void *installation = dlopen(kMobileInstallation, RTLD_NOW | RTLD_LOCAL);
        auto check = installation ? (CFDictionaryRef (*)(CFArrayRef, CFDictionaryRef))dlsym(installation, "MobileInstallationCheckCapabilitiesMatch")
                                  : nullptr;
        if (!check) {
            put("caps", "nosym");
        } else {
            CFDictionaryRef result = check((__bridge CFArrayRef)@[@"arm64"], nullptr);
            NSDictionary *answer = (__bridge NSDictionary *)result;
            if (!answer) put("caps", "nil");
            else
                put("caps", "match=" + describe(answer[@"CapabilitiesMatch"]) +
                                (answer[@"MismatchedCapabilities"] ? " mis=" + describe(answer[@"MismatchedCapabilities"]) : std::string()));
            if (result) CFRelease(result);
        }
        // The device family check last: it loads the migrator bundle and its frameworks.
        void *migrator = dlopen(kSystemAppMigrator, RTLD_NOW | RTLD_LOCAL);
        void *symbol = migrator ? dlsym(migrator, "MIIsApplicableToCurrentDeviceFamilyWithError") : nullptr;
        const char *source = "sam";
        if (!symbol && dlopen(kInstalledContentLibrary, RTLD_NOW | RTLD_LOCAL)) {
            symbol = dlsym(RTLD_DEFAULT, "MIIsApplicableToCurrentDeviceFamilyWithError");
            source = "icl";
        }
        Class configuration = NSClassFromString(@"MIDaemonConfiguration");
        id shared = [configuration respondsToSelector:@selector(sharedInstance)]
                        ? ((id (*)(Class, SEL))objc_msgSend)(configuration, @selector(sharedInstance)) : nil;
        put("fam.skip", [shared respondsToSelector:@selector(skipDeviceFamilyCheck)]
                            ? (((BOOL (*)(id, SEL))objc_msgSend)(shared, @selector(skipDeviceFamilyCheck)) ? "1" : "0") : "-");
        if (!symbol) {
            const char *why = dlerror();
            put("fam.sam", std::string("nosym ") + (why ? why : ""));
        } else {
            NSError *family_error = nil;
            const BOOL ok = ((BOOL (*)(NSArray *, NSError *__autoreleasing *))symbol)(@[@1, @2], &family_error);
            put("fam.sam", std::string(ok ? "1" : "0") + (strcmp(source, "sam") ? " icl" : "") +
                               (ok ? std::string() : " " + error_text(family_error)));
        }
    }
    return 0;
}

int probe_state() {
    @autoreleasepool {
        void *installation = dlopen(kMobileInstallation, RTLD_NOW | RTLD_LOCAL);
        auto lookup = installation ? (CFDictionaryRef (*)(CFDictionaryRef))dlsym(installation, "MobileInstallationLookupSystemAppState")
                                   : nullptr;
        if (!lookup) {
            printf("SE\tnosym\n");
        } else {
            CFDictionaryRef result = lookup(nullptr);
            NSDictionary *states = (__bridge NSDictionary *)result;
            if (!states) printf("SE\tnil\n");
            for (id key in states) {
                const std::string id_text = one_line(describe(key)), state = one_line(describe(states[key]));
                printf("S\t%s\t%s\n", id_text.c_str(), state.c_str());
            }
            if (states) printf("SN\t%lu\n", (unsigned long)states.count);
            if (result) CFRelease(result);
        }
        fflush(stdout);
        auto status = installation ? (BOOL (*)(BOOL *, NSError *__autoreleasing *))dlsym(installation,
                                                                                         "MobileInstallationGetSystemAppMigrationStatus")
                                   : nullptr;
        if (!status) {
            printf("MIGE\tnosym\n");
        } else {
            BOOL complete = NO;
            NSError *error = nil;
            if (status(&complete, &error)) printf("MIG\t%d\n", complete ? 1 : 0);
            else printf("MIGE\t%s\n", one_line(error_text(error)).c_str());
        }
        fflush(stdout);
    }
    return 0;
}

// ---- the feature probes: one child each, V lines; the keychain's one test item is all they change -----------------------------------

// A BOOL method's answer: "1", "0", or "-" when the object is nil or has no such method.
std::string bool_of(id target, const char *selector) {
    const SEL method = sel_registerName(selector);
    if (!target || ![target respondsToSelector:method]) return "-";
    return ((BOOL (*)(id, SEL))objc_msgSend)(target, method) ? "1" : "0";
}

id object_of(id target, const char *selector) {
    const SEL method = sel_registerName(selector);
    if (!target || ![target respondsToSelector:method]) return nil;
    return ((id (*)(id, SEL))objc_msgSend)(target, method);
}

int probe_mg() {
    @autoreleasepool {
        void *gestalt = dlopen(kMobileGestalt, RTLD_NOW | RTLD_LOCAL);
        auto copy_answer = gestalt ? (CFTypeRef (*)(CFStringRef, CFDictionaryRef))dlsym(gestalt, "MGCopyAnswer") : nullptr;
        if (!copy_answer) return 3;
        for (const auto &key : mg_keys()) {
            CFTypeRef answer = copy_answer((__bridge CFStringRef)@(key.first.c_str()), nullptr);
            put(("mg." + key.second).c_str(), describe((__bridge id)answer));
            if (answer) CFRelease(answer);
        }
    }
    return 0;
}

// IOPSCopyPowerSourcesInfo: powerd's view of the first power source, what the status bar and Settings show.
int probe_pwr() {
    @autoreleasepool {
        void *iokit = dlopen(kIOKit, RTLD_NOW | RTLD_LOCAL);
        auto copy_info = iokit ? (CFTypeRef (*)(void))dlsym(iokit, "IOPSCopyPowerSourcesInfo") : nullptr;
        auto copy_list = iokit ? (CFArrayRef (*)(CFTypeRef))dlsym(iokit, "IOPSCopyPowerSourcesList") : nullptr;
        auto description = iokit ? (CFDictionaryRef (*)(CFTypeRef, CFTypeRef))dlsym(iokit, "IOPSGetPowerSourceDescription") : nullptr;
        auto providing = iokit ? (CFStringRef (*)(CFTypeRef))dlsym(iokit, "IOPSGetProvidingPowerSourceType") : nullptr;
        if (!copy_info || !copy_list || !description || !providing) return 3;
        CFTypeRef info = copy_info();
        if (!info) {
            put("pwr.n", "nil");
            return 0;
        }
        put("pwr.src", describe((__bridge id)providing(info)));
        CFArrayRef list = copy_list(info);
        NSArray *sources = (__bridge NSArray *)list;
        put("pwr.n", std::to_string(sources.count));
        if (sources.count) {
            NSDictionary *source = (__bridge NSDictionary *)description(info, (__bridge CFTypeRef)sources[0]);
            put("pwr.type", describe(source[@"Type"]));
            put("pwr.cap", describe(source[@"Current Capacity"]));
            put("pwr.max", describe(source[@"Max Capacity"]));
            put("pwr.chg", describe(source[@"Is Charging"]));
            put("pwr.state", describe(source[@"Power Source State"]));
        }
        if (list) CFRelease(list);
        CFRelease(info);
    }
    return 0;
}

// The registry alone (no user client): the HID services by usage, which decide MobileGestalt's sensor answers, and the board's services.
int probe_hid() {
    @autoreleasepool {
        void *iokit = dlopen(kIOKit, RTLD_NOW | RTLD_LOCAL);
        auto matching = iokit ? (CFMutableDictionaryRef (*)(const char *))dlsym(iokit, "IOServiceMatching") : nullptr;
        auto services = iokit ? (kern_return_t (*)(mach_port_t, CFDictionaryRef, mach_port_t *))dlsym(iokit, "IOServiceGetMatchingServices") : nullptr;
        auto next = iokit ? (mach_port_t (*)(mach_port_t))dlsym(iokit, "IOIteratorNext") : nullptr;
        auto release = iokit ? (kern_return_t (*)(mach_port_t))dlsym(iokit, "IOObjectRelease") : nullptr;
        auto property = iokit ? (CFTypeRef (*)(mach_port_t, CFStringRef, CFAllocatorRef, uint32_t))dlsym(iokit, "IORegistryEntryCreateCFProperty") : nullptr;
        if (!matching || !services || !next || !release || !property) return 3;
        auto number = [&](mach_port_t service, CFStringRef key) -> long {
            CFTypeRef value = property(service, key, kCFAllocatorDefault, 0);
            id object = (__bridge id)value;
            const long out = [object isKindOfClass:[NSNumber class]] ? [object longValue] : -1;
            if (value) CFRelease(value);
            return out;
        };
        auto each = [&](const char *name, auto visit) -> long {
            mach_port_t iterator = MACH_PORT_NULL;
            if (services(MACH_PORT_NULL, matching(name), &iterator) != KERN_SUCCESS) return -1;     // takes the dictionary
            long count = 0;
            for (mach_port_t service = next(iterator); service; service = next(iterator)) {
                ++count;
                visit(service);
                release(service);
            }
            release(iterator);
            return count;
        };
        std::map<std::string, long> found;
        for (const auto &pair : hid_pairs()) found[hid_key(pair.first, pair.second)] = 0;
        const long all = each("IOHIDEventService", [&](mach_port_t service) {
            const long page = number(service, CFSTR("PrimaryUsagePage")), usage = number(service, CFSTR("PrimaryUsage"));
            if (page < 0 || usage < 0) return;
            const auto it = found.find(hid_key(unsigned(page), unsigned(usage)));
            if (it != found.end()) ++it->second;
        });
        for (const auto &item : found) put(("hid." + item.first).c_str(), std::to_string(item.second));
        put("hid.all", std::to_string(all));
        for (const auto &item : io_classes())
            put(("io." + item.second).c_str(), std::to_string(each(item.first.c_str(), [](mach_port_t) {})));
    }
    return 0;
}

// One test item through securityd: the OSStatus of adding, reading and deleting it (-25308 locked, -34018 no entitlement, ...).
int probe_kc() {
    @autoreleasepool {
        void *security = dlopen(kSecurity, RTLD_NOW | RTLD_LOCAL);
        auto add = security ? (OSStatus (*)(CFDictionaryRef, CFTypeRef *))dlsym(security, "SecItemAdd") : nullptr;
        auto copy = security ? (OSStatus (*)(CFDictionaryRef, CFTypeRef *))dlsym(security, "SecItemCopyMatching") : nullptr;
        auto remove = security ? (OSStatus (*)(CFDictionaryRef))dlsym(security, "SecItemDelete") : nullptr;
        if (!add || !copy || !remove) return 3;
        auto constant = [&](const char *name) -> NSString * {
            void *symbol = dlsym(security, name);
            return symbol ? (__bridge NSString *)*(CFStringRef *)symbol : nil;
        };
        NSString *class_key = constant("kSecClass"), *generic = constant("kSecClassGenericPassword"),
                 *service = constant("kSecAttrService"), *account = constant("kSecAttrAccount"), *group = constant("kSecAttrAccessGroup"),
                 *value = constant("kSecValueData"), *accessible = constant("kSecAttrAccessible"),
                 *after_unlock = constant("kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly"), *return_data = constant("kSecReturnData");
        if (!class_key || !generic || !service || !account || !group || !value || !accessible || !after_unlock || !return_data) return 3;
        NSString *name = @(kKeychainItem);
        NSDictionary *query = @{class_key: generic, service: name, account: @"selftest", group: name};
        NSData *secret = [NSData dataWithBytes:"pagani" length:6];
        remove((__bridge CFDictionaryRef)query);        // a test item an earlier round could not delete
        NSMutableDictionary *item = [query mutableCopy];
        item[value] = secret;
        item[accessible] = after_unlock;
        put("kc.add", std::to_string(int(add((__bridge CFDictionaryRef)item, nullptr))));
        NSMutableDictionary *read = [query mutableCopy];
        read[return_data] = @YES;
        CFTypeRef result = nullptr;
        const OSStatus got = copy((__bridge CFDictionaryRef)read, &result);
        put("kc.get", std::to_string(int(got)));
        put("kc.match", result && [(__bridge id)result isEqual:secret] ? "1" : "0");
        if (result) CFRelease(result);
        put("kc.del", std::to_string(int(remove((__bridge CFDictionaryRef)query))));
    }
    return 0;
}

// The audio route as an app sees it: the port types of the current route and the output volume.
int probe_aud() {
    @autoreleasepool {
        dlopen(kAVFAudio, RTLD_NOW | RTLD_LOCAL);
        id shared = object_of((id)NSClassFromString(@"AVAudioSession"), "sharedInstance");
        if (!shared) return 3;
        id route = object_of(shared, "currentRoute");
        auto ports = [&](const char *selector) {
            std::string out;
            for (id port in (NSArray *)object_of(route, selector)) {
                if (!out.empty()) out += ',';
                out += describe(object_of(port, "portType"));
            }
            return out.empty() ? std::string("none") : out;
        };
        put("aud.out", ports("outputs"));
        put("aud.in", ports("inputs"));
        const SEL volume = sel_registerName("outputVolume");
        if ([shared respondsToSelector:volume]) {
            char text[16];
            snprintf(text, sizeof text, "%.2f", ((float (*)(id, SEL))objc_msgSend)(shared, volume));
            put("aud.vol", text);
        }
    }
    return 0;
}

// What CoreMotion says is there (locationd's answers); no sensor is started.
int probe_sen() {
    @autoreleasepool {
        dlopen(kCoreMotion, RTLD_NOW | RTLD_LOCAL);
        Class motion = NSClassFromString(@"CMMotionManager");
        if (!motion) return 3;
        id manager = [[motion alloc] init];
        put("sen.acc", bool_of(manager, "isAccelerometerAvailable"));
        put("sen.gyr", bool_of(manager, "isGyroAvailable"));
        put("sen.mag", bool_of(manager, "isMagnetometerAvailable"));
        put("sen.dm", bool_of(manager, "isDeviceMotionAvailable"));
    }
    return 0;
}

// What CoreHaptics says the hardware can do; nothing plays.
int probe_hap() {
    @autoreleasepool {
        dlopen(kCoreHaptics, RTLD_NOW | RTLD_LOCAL);
        id capabilities = object_of((id)NSClassFromString(@"CHHapticEngine"), "capabilitiesForHardware");
        if (!capabilities) return 3;
        put("hap.hw", bool_of(capabilities, "supportsHaptics"));
        put("hap.aud", bool_of(capabilities, "supportsAudio"));
    }
    return 0;
}

// ---- the widget census (README.md "The widget census") -----------------------------------------------------------------------------

// An extension bundle's Info.plist: <bundle>/Info.plist here, <bundle>/Contents/Info.plist on a Mac.
NSDictionary *bundle_info(NSString *bundle) {
    NSDictionary *info = [NSDictionary dictionaryWithContentsOfFile:[bundle stringByAppendingPathComponent:@"Info.plist"]];
    return info ?: [NSDictionary dictionaryWithContentsOfFile:[bundle stringByAppendingPathComponent:@"Contents/Info.plist"]];
}

// The bundle identifier of a WidgetKit extension, nil for any other bundle.
NSString *widget_identifier(NSString *bundle) {
    NSDictionary *info = bundle_info(bundle);
    NSString *point = info[@"NSExtension"][@"NSExtensionPointIdentifier"] ?: info[@"EXAppExtensionAttributes"][@"EXExtensionPointIdentifier"];
    if (![point isKindOfClass:[NSString class]] || ![point isEqualToString:@(kWidgetPoint)]) return nil;
    NSString *identifier = info[@"CFBundleIdentifier"];
    return [identifier isKindOfClass:[NSString class]] ? identifier : nil;
}

// The extensions in the app bundles of /Applications, whichever volume of the union they are on: (path, identifier).
std::vector<std::pair<std::string, std::string>> widget_bundles() {
    std::vector<std::pair<std::string, std::string>> found;
    NSFileManager *files = [NSFileManager defaultManager];
    for (NSString *app in [files contentsOfDirectoryAtPath:@(kAppsDirectory) error:nil]) {
        if (![app.pathExtension isEqualToString:@"app"]) continue;
        NSString *bundle = [@(kAppsDirectory) stringByAppendingPathComponent:app];
        for (NSString *inner in @[@"PlugIns", @"Contents/PlugIns"]) {
            NSString *plugins = [bundle stringByAppendingPathComponent:inner];
            for (NSString *name in [files contentsOfDirectoryAtPath:plugins error:nil]) {
                if (![name.pathExtension isEqualToString:@"appex"]) continue;
                NSString *path = [plugins stringByAppendingPathComponent:name];
                NSString *identifier = widget_identifier(path);
                if (identifier) found.emplace_back(utf8(path), utf8(identifier));
            }
        }
    }
    return found;
}

bool on_bind_mount(const std::string &path) {
    struct statfs volume = {};
    return statfs(path.c_str(), &volume) == 0 && !strcmp(volume.f_fstypename, "bindfs");
}

// fsgetpath of a file back to its path: "0" the same path, "x" another path, "e<errno>" refused (45 ENOTSUP: no bind mount answers).
std::string path_from_id(const std::string &path) {
    auto get_path = (ssize_t (*)(char *, size_t, fsid_t *, uint64_t))dlsym(RTLD_DEFAULT, "fsgetpath");
    struct stat info = {};
    if (!get_path) return "nosym";
    if (stat(path.c_str(), &info) != 0) return "e" + std::to_string(errno);
    fsid_t volume = {{info.st_dev, 0}};
    char found[PATH_MAX] = {};
    if (get_path(found, sizeof found, &volume, info.st_ino) < 0) return "e" + std::to_string(errno);
    return path == found ? "0" : "x";
}

// The two format capabilities of the volume of /Applications that bindfs strips, as a hex mask of VOL_CAP_FMT_*, or "e<errno>".
std::string apps_capabilities() {
    struct attrlist request = {};
    request.bitmapcount = ATTR_BIT_MAP_COUNT;
    request.volattr = ATTR_VOL_INFO | ATTR_VOL_CAPABILITIES;
    struct __attribute__((packed, aligned(4))) {
        uint32_t length;
        vol_capabilities_attr_t capabilities;
    } answer = {};
    if (getattrlist(kAppsDirectory, &request, &answer, sizeof answer, 0) != 0) return "e" + std::to_string(errno);
    const unsigned both = answer.capabilities.capabilities[VOL_CAPABILITIES_FORMAT] & answer.capabilities.valid[VOL_CAPABILITIES_FORMAT] &
                          (VOL_CAP_FMT_PERSISTENTOBJECTIDS | VOL_CAP_FMT_PATH_FROM_ID);
    char text[16];
    snprintf(text, sizeof text, "%x", both);
    return text;
}

// The live processes: chronod, app extensions, widget extensions. A widget extension lives for seconds, so the kernel's status line counts
// their execs (ax); this says what runs at the round.
void live_extensions() {
    auto list = (int (*)(void *, int))dlsym(RTLD_DEFAULT, "proc_listallpids");
    auto path_of = (int (*)(int, void *, uint32_t))dlsym(RTLD_DEFAULT, "proc_pidpath");
    if (!list || !path_of) return;
    std::vector<int> pids(4096);
    const int count = list(pids.data(), int(pids.size() * sizeof(int)));
    if (count < 0) return;
    unsigned chronod = 0, extensions = 0, widgets = 0;
    for (int i = 0; i < count && i < int(pids.size()); ++i) {
        char path[4096] = {};
        if (path_of(pids[size_t(i)], path, sizeof path) <= 0) continue;
        const char *name = strrchr(path, '/');
        chronod += name && !strcmp(name, "/chronod");
        if (!strstr(path, ".appex/")) continue;
        ++extensions;
        widgets += widget_extension_path(path);
    }
    put("wk.chr", std::to_string(chronod));
    put("wk.ext", std::to_string(extensions));
    put("wk.wid", std::to_string(widgets));
}

// What LaunchServices has for one bundle, asked as chronod asks it (-[LSApplicationExtensionRecord initWithBundleIdentifier:requireValid:
// error:], ChronoCore's check before it lists an extension).
WidgetEntry widget_entry(Class records, const std::string &path, const std::string &identifier) {
    WidgetEntry entry;
    entry.in_view = on_bind_mount(path);
    NSString *name = @(identifier.c_str());
    NSError *error = nil;
    id valid = [[records alloc] initWithBundleIdentifier:name requireValid:YES error:&error];
    entry.valid = valid != nil;
    if (!valid) entry.error = error ? long(error.code) : -1;
    id record = valid ?: [[records alloc] initWithBundleIdentifier:name requireValid:NO error:nil];
    if (!record) return entry;
    entry.registered = true;
    if ([record respondsToSelector:@selector(platform)]) entry.platform = long(((unsigned (*)(id, SEL))objc_msgSend)(record, @selector(platform)));
    entry.point = describe(object_of(object_of(record, "extensionPointRecord"), "identifier")) == kWidgetPoint;
    NSURL *url = object_of(record, "URL");
    entry.elsewhere = url && utf8(url.path) != path;
    return entry;
}

int probe_wid() {
    @autoreleasepool {
        put("wk.cap", apps_capabilities());
        put("wk.fgr", path_from_id(kAppsDirectory));
        live_extensions();
        const auto bundles = widget_bundles();
        // One bundle of the view, or of the root when no view is mounted: the file ID as the kernel and as CoreFoundation answer it.
        std::string sample = bundles.empty() ? std::string() : bundles[0].first;
        for (const auto &bundle : bundles)
            if (on_bind_mount(bundle.first)) {
                sample = bundle.first;
                break;
            }
        if (!sample.empty()) {
            put("wk.fg", path_from_id(sample));
            id file_id = nil;
            [[NSURL fileURLWithPath:@(sample.c_str())] getResourceValue:&file_id forKey:@"_NSURLFileIDKey" error:nil];
            put("wk.fid", file_id ? "1" : "0");
        }
        dlopen(kCoreServices, RTLD_NOW | RTLD_LOCAL);
        Class records = NSClassFromString(@"LSApplicationExtensionRecord"), points = NSClassFromString(@"LSExtensionPointRecord");
        if (!records || !points) return 3;
        std::vector<WidgetEntry> entries;
        for (const auto &bundle : bundles) entries.push_back(widget_entry(records, bundle.first, bundle.second));
        Values values;
        census_values(entries, values);
        for (const auto &value : values) put(value.first.c_str(), value.second);
        // The extension point, and everything LaunchServices has under it (also outside /Applications).
        id point = [[points alloc] initWithIdentifier:@(kWidgetPoint) error:nil];
        if ([point respondsToSelector:@selector(platform)])
            put("wk.pt", std::to_string(((unsigned (*)(id, SEL))objc_msgSend)(point, @selector(platform))));
        const SEL enumerate = sel_registerName("enumeratorWithExtensionPointRecord:options:");
        if (point && [records respondsToSelector:enumerate]) {
            id all = ((id (*)(Class, SEL, id, unsigned long long))objc_msgSend)(records, enumerate, point, 0);
            unsigned count = 0;
            if ([all respondsToSelector:@selector(nextObject)])
                while (count < 1000 && [all nextObject]) ++count;
            put("wk.ls", std::to_string(count));
        }
    }
    return 0;
}

// The answers that need no foreign code, in the daemon itself: the clock, the Data volume, the interfaces.
void local_values(Values &values) {
    const time_t now = time(nullptr);
    struct tm local = {};
    char text[64];
    if (localtime_r(&now, &local) && strftime(text, sizeof text, "%Y-%m-%d %H:%M:%S %z", &local)) values["time.local"] = text;
    char link[PATH_MAX] = {};
    const ssize_t length = readlink(kTimezoneLink, link, sizeof link - 1);
    if (length > 0) {
        const std::string target(link, size_t(length));
        const size_t at = target.find("zoneinfo/");
        values["time.tz"] = at == std::string::npos ? target : target.substr(at + 9);
    }
    struct statfs volume = {};
    if (statfs(kDataVolume, &volume) == 0) {
        values["data.free"] = std::to_string((unsigned long long)volume.f_bavail * volume.f_bsize >> 20);
        values["data.total"] = std::to_string((unsigned long long)volume.f_blocks * volume.f_bsize >> 20);
    }
    struct ifaddrs *interfaces = nullptr;
    if (getifaddrs(&interfaces) == 0) {
        std::map<std::string, bool> seen;
        for (const struct ifaddrs *item = interfaces; item; item = item->ifa_next)
            if (item->ifa_name) seen[item->ifa_name] = seen[item->ifa_name] || (item->ifa_flags & IFF_UP);
        freeifaddrs(interfaces);
        std::string list;
        unsigned up = 0;
        for (const auto &item : seen) {
            if (!list.empty()) list += ' ';
            list += item.first + (item.second ? '+' : '-');
            up += item.second && item.first != "lo0";
        }
        values["net.list"] = list;
        values["net.up"] = std::to_string(up);
    }
}

// The system log store, or nil and why not. +[OSLogStore storeWithScope:error:] of iOS 17 (21H461 OSLog) answers only
// OSLogStoreCurrentProcessIdentifier: any other scope returns nil without an error, whatever the entitlements. So the system store is
// LoggingSupport's local store (+[OSLogEventStore localStore], prepared synchronously: /private/var/db/diagnostics plus logd's in-flight
// buffers through its admin connection, com.apple.private.logging.admin) wrapped in an OSLogStore the way +[OSLogStore
// storeWithURL:error:] wraps an archive: -initForFactory, _source the prepared OSLogEventSource, _constraint left 0, so the
// enumerator is OSLogSystemEnumerator. The scope call stays first for a system that answers it (macOS).
OSLogStore *system_log_store(std::string &why) {
    NSError *error = nil;
    OSLogStore *store = [OSLogStore storeWithScope:(OSLogStoreScope)0 error:&error];
    if (store) return store;
    why = "scope " + error_text(error);
    Class event_store = NSClassFromString(@"OSLogEventStore");
    Ivar source_ivar = class_getInstanceVariable([OSLogStore class], "_source");
    if (!event_store || !source_ivar || ![event_store respondsToSelector:@selector(localStore)] ||
        ![OSLogStore instancesRespondToSelector:@selector(initForFactory)]) {
        why += " nolocal";
        return nil;
    }
    id local = ((id (*)(Class, SEL))objc_msgSend)(event_store, @selector(localStore));
    if (![local respondsToSelector:@selector(prepareWithCompletionHandler:)]) {
        why += " nolocal";
        return nil;
    }
    __block id source = nil;
    __block NSError *prepare_error = nil;
    dispatch_semaphore_t prepared = dispatch_semaphore_create(0);
    ((void (*)(id, SEL, void (^)(id, NSError *)))objc_msgSend)(local, @selector(prepareWithCompletionHandler:), ^(id ready, NSError *failed) {
        source = ready;
        prepare_error = failed;
        dispatch_semaphore_signal(prepared);
    });
    if (dispatch_semaphore_wait(prepared, dispatch_time(DISPATCH_TIME_NOW, int64_t(10 * NSEC_PER_SEC))) != 0) {
        why += " local timeout";
        return nil;
    }
    if (!source) {
        why += " local " + error_text(prepare_error);
        return nil;
    }
    store = [[OSLogStore alloc] initForFactory];
    if (!store) {
        why += " local init";
        return nil;
    }
    object_setIvar(store, source_ivar, source);
    return store;
}

// budget: seconds the parent waits; the enumeration stops 3 s before, so what was read is written out in time.
int probe_log(double since, double budget) {
    const double enumeration = budget > 6 ? budget - 3 : 3;
    constexpr unsigned kMaxKept = 4000;
    const double start = monotonic();
    @autoreleasepool {
        NSError *error = nil;
        std::string why;
        OSLogStore *store = system_log_store(why);
        if (!store) {
            printf("E\tstore %s\n", one_line(why).c_str());
            return 3;
        }
        OSLogPosition *position = since > 0 ? [store positionWithDate:[NSDate dateWithTimeIntervalSince1970:since]]
                                            : [store positionWithTimeIntervalSinceLatestBoot:0];
        NSArray *processes = @[@"com.apple.migrationpluginwrapper", @"com.apple.datamigrator", @"datamigrator", @"installd",
                               @"installcoordinationd"];
        NSPredicate *predicate = [NSPredicate predicateWithFormat:@"process IN %@ OR subsystem == %@ OR composedMessage CONTAINS %@",
                                                                  processes, @"com.apple.migration", @"SystemApp"];
        OSLogEnumerator *entries = [store entriesEnumeratorWithOptions:0 position:position predicate:predicate error:&error];
        const int predicated = entries != nil;
        if (!entries) {
            printf("E\tpredicate %s\n", one_line(error_text(error)).c_str());
            error = nil;
            entries = [store entriesEnumeratorWithOptions:0 position:position predicate:nil error:&error];
        }
        if (!entries) {
            printf("E\tenumerator %s\n", one_line(error_text(error)).c_str());
            return 4;
        }
        unsigned long long seen = 0, kept = 0;
        double last = since;
        @try {
            for (OSLogEntry *entry in entries) {
                @autoreleasepool {
                    const double time = entry.date.timeIntervalSince1970;
                    if (time > last) last = time;
                    if (++seen % 512 == 0) {
                        printf("P\t%.6f\n", last);
                        fflush(stdout);
                    }
                    if (monotonic() - start > enumeration) {
                        printf("E\tbudget %.0f s\n", enumeration);
                        break;
                    }
                    if (time <= since || ![entry isKindOfClass:[OSLogEntryLog class]]) continue;
                    OSLogEntryLog *log = (OSLogEntryLog *)entry;
                    Entry item;
                    item.time = time;
                    item.level = int(log.level);
                    item.pid = int(log.processIdentifier);
                    item.process = utf8(log.process ?: @"?");
                    item.subsystem = log.subsystem ? utf8(log.subsystem) : std::string();
                    item.message = utf8(log.composedMessage ?: @"");
                    if (classify(item) < 0) continue;
                    printf("%s\n", entry_line(item).c_str());
                    if (++kept >= kMaxKept) {
                        printf("E\tkept %u\n", kMaxKept);
                        break;
                    }
                }
            }
        } @catch (NSException *exception) {
            printf("E\texception %s\n", one_line(utf8(exception.name ?: @"?")).c_str());
        }
        printf("P\t%.6f\nN\t%llu\t%llu\t%d\t%.0f\n", last, seen, kept, predicated, (monotonic() - start) * 1000);
        fflush(stdout);
    }
    return 0;
}

// ---- the daemon ---------------------------------------------------------------------------------------------------------------

struct Probe {
    std::vector<std::string> lines;
    bool timed_out = false;
    int status = -1;                    // exit status, or -signal
};

std::string self_path() {
    char path[PATH_MAX] = {};
    uint32_t size = sizeof path;
    if (_NSGetExecutablePath(path, &size) == 0) {
        char real[PATH_MAX] = {};
        if (realpath(path, real)) return real;
        return path;
    }
    return kSelfPath;
}

// This executable with `arguments`, its standard output split into lines; killed after timeout seconds.
Probe run_probe(const std::vector<std::string> &arguments, double timeout) {
    Probe probe;
    int pipes[2];
    if (pipe(pipes) != 0) return probe;
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, pipes[1], 1);
    posix_spawn_file_actions_addopen(&actions, 2, "/dev/null", O_WRONLY, 0);
    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_CLOEXEC_DEFAULT);
    const std::string path = self_path();
    std::vector<char *> argv;
    argv.push_back(const_cast<char *>(path.c_str()));
    for (const std::string &argument : arguments) argv.push_back(const_cast<char *>(argument.c_str()));
    argv.push_back(nullptr);
    pid_t pid = 0;
    const int spawned = posix_spawn(&pid, path.c_str(), &actions, &attributes, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attributes);
    close(pipes[1]);
    if (spawned != 0) {
        close(pipes[0]);
        probe.status = -1000 - spawned;
        return probe;
    }
    std::string buffer;
    const double deadline = monotonic() + timeout;
    bool ended = false;                 // end of file: the child has closed its output (it exits)
    for (;;) {
        const double left = deadline - monotonic();
        if (left <= 0) {
            probe.timed_out = true;
            break;
        }
        struct pollfd wait = {pipes[0], POLLIN, 0};
        const int ready = poll(&wait, 1, int(left * 1000) + 1);
        if (ready < 0 && errno != EINTR) break;
        if (ready <= 0) continue;
        char chunk[4096];
        const ssize_t got = read(pipes[0], chunk, sizeof chunk);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) {
            ended = got == 0;
            break;
        }
        if (buffer.size() < (4u << 20)) buffer.append(chunk, size_t(got));
    }
    // Killed unless it ended: at the timeout, and after a poll or read error too, so the wait below cannot hang on a stuck child.
    if (!ended) kill(pid, SIGKILL);
    close(pipes[0]);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    probe.status = WIFEXITED(status) ? WEXITSTATUS(status) : WIFSIGNALED(status) ? -WTERMSIG(status) : -1;
    size_t start = 0;
    while (start < buffer.size()) {
        size_t end = buffer.find('\n', start);
        if (end == std::string::npos) end = buffer.size();     // a last line cut by the kill is kept as it is
        probe.lines.push_back(buffer.substr(start, end - start));
        start = end + 1;
    }
    return probe;
}

// "probe <name> timeout|sig N|exit N" for a probe that did not end well, "" otherwise.
std::string probe_trouble(const char *name, const Probe &probe) {
    if (probe.timed_out) return std::string("probe ") + name + " timeout";
    if (probe.status < -999) return std::string("probe ") + name + " spawn " + std::to_string(-1000 - probe.status);
    if (probe.status < 0) return std::string("probe ") + name + " sig " + std::to_string(-probe.status);
    if (probe.status > 0) return std::string("probe ") + name + " exit " + std::to_string(probe.status);
    return std::string();
}

std::vector<std::string> split_tabs(const std::string &line) {
    std::vector<std::string> parts;
    size_t start = 0;
    for (;;) {
        const size_t tab = line.find('\t', start);
        parts.push_back(line.substr(start, tab == std::string::npos ? std::string::npos : tab - start));
        if (tab == std::string::npos) return parts;
        start = tab + 1;
    }
}

class Sender {
public:
    Sender(bool print, unsigned gap_ms) : print_(print), pacer_(gap_ms) {}

    // Writes one line; false once the kernel takes no more (no sysctl, or its budget for this boot is used). A sysctl write a policy
    // refuses goes on through the spool file, which the kernel reads every 5 s.
    bool send(const std::string &line) {
        if (closed_ || line.empty()) return !closed_;
        const std::string text = kernel_text(line);
        for (int attempt = 0; attempt < 3; ++attempt) {
            sleep_until(pacer_.next(monotonic()));
            int result = 0;
            if (print_) {
                printf("diag: %s\n", text.c_str());
                fflush(stdout);
            } else if (spool_ >= 0) {
                result = to_spool(text);
            } else {
                result = sysctlbyname(kLineSysctl, nullptr, nullptr, const_cast<char *>(text.data()), text.size()) == 0 ? 0 : errno;
                if (result == EPERM || result == EACCES) {
                    spool_ = open(kSpoolPath, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
                    if (spool_ >= 0) result = to_spool(text);
                }
            }
            pacer_.sent(monotonic());
            if (!result) {
                ++sent_;
                return true;
            }
            error_ = result;
            if (result == EAGAIN) {
                sleep_until(monotonic() + 2);
                continue;
            }
            if (result == EINVAL) return true;
            closed_ = true;     // ENOENT (no patch), ENOSPC (the budget), no spool either, or anything else
            return false;
        }
        ++dropped_;
        return true;
    }

    bool closed() const { return closed_; }
    int error() const { return error_; }
    unsigned sent() const { return sent_; }
    bool spooling() const { return spool_ >= 0; }

private:
    int to_spool(const std::string &text) {
        const std::string line = text + "\n";
        return write(spool_, line.data(), line.size()) == ssize_t(line.size()) ? 0 : errno;
    }

    bool print_;
    int spool_ = -1;
    Pacer pacer_;
    bool closed_ = false;
    int error_ = 0;
    unsigned sent_ = 0, dropped_ = 0;
};

std::string read_file(const char *path) {
    FILE *file = fopen(path, "r");
    if (!file) return std::string();
    std::string text;
    char chunk[1024];
    for (size_t got; (got = fread(chunk, 1, sizeof chunk, file)) > 0 && text.size() < 65536;) text.append(chunk, got);
    fclose(file);
    return text;
}

// The logging preference the seed brings (make-phone-seed.py --diag-daemon) or, without it, made here before the migration starts.
std::string ensure_log_preference(const Config &config) {
    if (access(kLogPreferencePath, F_OK) == 0) return "seed";
    if (!config.log_preference) return "off";
    @autoreleasepool {
        NSDictionary *preference = @{@"DEFAULT-OPTIONS": @{@"Level": @{@"Enable": @"Info", @"Persist": @"Info"}}};
        NSData *data = [NSPropertyListSerialization dataWithPropertyList:preference format:NSPropertyListXMLFormat_v1_0 options:0
                                                                   error:nil];
        NSError *error = nil;
        if (![[NSFileManager defaultManager] createDirectoryAtPath:@(kLogPreferenceDirectory) withIntermediateDirectories:YES
                                                        attributes:@{NSFilePosixPermissions: @0755} error:&error] ||
            ![data writeToFile:@(kLogPreferencePath) options:NSDataWritingAtomic error:&error]) {
            return "fail " + std::to_string((long)error.code);
        }
        chmod(kLogPreferencePath, 0644);
        notify_post(kLogPreferencesChanged);
    }
    return "made";
}

// SystemAppInstallState.plist: its size and its bundle ID -> state pairs (PassFacts.file_bytes codes).
void read_states(PassFacts &facts) {
    struct stat info = {};
    if (stat(kStatePath, &info) != 0) {
        facts.file_bytes = errno == ENOENT ? -1 : -2;
        return;
    }
    @autoreleasepool {
        NSData *data = [NSData dataWithContentsOfFile:@(kStatePath)];
        if (!data) {
            facts.file_bytes = -2;
            return;
        }
        id list = [NSPropertyListSerialization propertyListWithData:data options:NSPropertyListImmutable format:nullptr error:nil];
        if (![list isKindOfClass:[NSDictionary class]]) {
            facts.file_bytes = -3;
            return;
        }
        facts.file_bytes = long(data.length);
        for (id key in (NSDictionary *)list) {
            id value = ((NSDictionary *)list)[key];
            facts.file.emplace_back(describe(key), [value isKindOfClass:[NSNumber class]] ? [value longValue] : -1);
        }
    }
}

long count_entries(const char *path, const char *suffix) {
    DIR *directory = opendir(path);
    if (!directory) return -1;
    long count = 0;
    const size_t suffix_length = suffix ? strlen(suffix) : 0;
    while (struct dirent *entry = readdir(directory)) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        const size_t length = strlen(entry->d_name);
        if (suffix && (length < suffix_length || strcmp(entry->d_name + length - suffix_length, suffix))) continue;
        ++count;
    }
    closedir(directory);
    return count;
}

unsigned kernel_dropped() {
    unsigned value = 0;
    size_t size = sizeof value;
    return sysctlbyname(kDroppedSysctl, &value, &size, nullptr, 0) == 0 ? value : 0;
}

// One feature round: every probe in its own child, then what DiagCore makes of the answers.
void run_round(unsigned round, const Config &config, Rounds &rounds, Sender &sender) {
    const long up = uptime_seconds();
    Values values;
    for (const std::string &name : round_probes()) {
        if (std::find(config.skip.begin(), config.skip.end(), name) != config.skip.end()) continue;
        const Probe probe = run_probe({"--probe", name}, kProbeSeconds);
        for (const std::string &line : probe.lines) {
            const std::vector<std::string> parts = split_tabs(line);
            if (parts[0] == "V" && parts.size() == 3) values[parts[1]] = parts[2];
        }
        const std::string trouble = probe_trouble(name.c_str(), probe);
        if (!trouble.empty()) values["t." + name] = trouble;
    }
    local_values(values);
    for (const std::string &line : rounds.lines(round, up, values)) sender.send(line);
}

// The passes of the system app migration (migration=1).
class Passes {
public:
    Passes(const Config &config, Sender &sender, double boot, const std::string &preference)
        : config_(config), sender_(sender), boot_(boot), preference_(preference), rationer_(config) {}

    void run() {
        ++pass_;
        const unsigned pass = pass_;
        const bool facts_pass = pass == 1 || (config_.facts_every && (pass - 1) % config_.facts_every == 0);
        // The state summary first: short, and repeated every pass.
        PassFacts facts;
        facts.pass = pass;
        facts.uptime = uptime_seconds();
        read_states(facts);
        const Probe state = run_probe({"--probe", "state"}, 20);
        for (const std::string &line : state.lines) {
            const std::vector<std::string> parts = split_tabs(line);
            if (parts[0] == "S" && parts.size() == 3) {
                facts.lookup = 1;
                facts.lookup_states.emplace_back(parts[1], strtol(parts[2].c_str(), nullptr, 10));
            } else if (parts[0] == "SN") {
                facts.lookup = 1;
            } else if (parts[0] == "SE") {
                facts.lookup = parts.size() == 2 && parts[1] == "nosym" ? 0 : -1;
            } else if (parts[0] == "MIG" && parts.size() == 2) {
                facts.migration = atoi(parts[1].c_str());
            } else if (parts[0] == "MIGE") {
                facts.migration = parts.size() == 2 && parts[1] == "nosym" ? -1 : -2;
            }
        }
        if (state.timed_out || state.status != 0) facts.lookup = facts.lookup == 1 ? 1 : -2;
        facts.bundles = count_entries(kBundlesPath, nullptr);
        facts.staged = count_entries(kStagedPath, ".app");
        facts.sentinel = access(kSentinelPath, F_OK) == 0;
        facts.log = log_summary_;
        facts.kernel_dropped = kernel_dropped();
        sender_.send(pass_line(facts));
        const std::string not1 = not1_line(facts);
        if (!not1.empty() && (facts_pass || not1.substr(not1.find(' ')) != last_not1_)) sender_.send(not1);
        last_not1_ = not1.empty() ? std::string() : not1.substr(not1.find(' '));
        trouble("state", state);
        // The facts on the first pass and every facts_every passes after it.
        if (facts_pass) {
            const Probe probe = run_probe({"--probe", "facts"}, 40);
            std::map<std::string, std::string> values;
            for (const std::string &line : probe.lines) {
                const std::vector<std::string> parts = split_tabs(line);
                if (parts[0] == "V" && parts.size() == 3) values[parts[1]] = parts[2];
            }
            values["logpref"] = preference_;
            for (const std::string &line : facts_lines(values)) sender_.send(line);
            trouble("facts", probe);
        }
        // The unified log since the previous pass.
        if (config_.log && !sender_.closed()) read_log();
    }

    unsigned count() const { return pass_; }
    const Rationer &rationer() const { return rationer_; }

private:
    void trouble(const char *name, const Probe &probe) {
        const std::string line = probe_trouble(name, probe);
        if (line != troubles_[name]) sender_.send(line);    // a repeat is not written again
        troubles_[name] = line;
    }

    void read_log() {
        const double budget = config_.period > 9 ? std::min(30.0, config_.period - 3.0) : 6.0;
        char position[32], seconds[16];
        snprintf(position, sizeof position, "%.6f", since_);
        snprintf(seconds, sizeof seconds, "%.0f", budget);
        const double probe_wall = wall(), since_before = since_;
        const Probe probe = run_probe({"--probe", "log", position, seconds}, budget);
        std::vector<Entry> entries;
        std::string summary;
        bool predicated = true;
        for (const std::string &line : probe.lines) {
            Entry entry;
            if (parse_entry(line, entry)) {
                entries.push_back(std::move(entry));
                continue;
            }
            const std::vector<std::string> parts = split_tabs(line);
            if (parts[0] == "P" && parts.size() == 2) {
                since_ = std::max(since_, strtod(parts[1].c_str(), nullptr));
            } else if (parts[0] == "N" && parts.size() == 5) {
                summary = parts[1] + "/" + parts[2] + "/" + parts[4] + "ms";
                predicated = parts[3] == "1";
            } else if (parts[0] == "E" && parts.size() == 2 && !errors_sent_[parts[1]]) {
                errors_sent_[parts[1]] = true;
                sender_.send("log " + parts[1]);
            }
        }
        log_summary_ = probe.timed_out ? "to" : summary.empty() ? "e" : summary + (predicated ? "" : "/np");
        // A probe killed before it reported a position would start at the same place again and again: skip what it could not
        // read (with a predicate the enumerator yields only matches, so a slow scan never reaches the budget check).
        if (probe.timed_out && since_ == since_before && probe_wall > since_ + 1) {
            sender_.send("log skipped " + std::to_string(long(probe_wall - (since_ > 0 ? since_ : boot_))) + " s");
            since_ = probe_wall - 1;
        }
        const PassResult result = rationer_.pass(entries, boot_);
        for (const std::string &line : result.lines) sender_.send(line);
        sender_.send(suppressed_line(result));
        trouble("log", probe);
    }

    const Config &config_;
    Sender &sender_;
    double boot_;
    std::string preference_;
    Rationer rationer_;
    unsigned pass_ = 0;
    double since_ = 0;                          // the log position: seconds since 1970 of the last entry seen; 0: since boot
    std::string log_summary_, last_not1_;
    std::map<std::string, bool> errors_sent_;
    std::map<std::string, std::string> troubles_;
};

int run_daemon(bool print, const char *argv0) {
    Config config;
    std::string bad;
    // PAGANIDIAG_CONFIG names another configuration file, for a manual run with --print (launchd gives the job no such variable).
    const char *config_path = print && getenv("PAGANIDIAG_CONFIG") ? getenv("PAGANIDIAG_CONFIG") : kConfigPath;
    parse_config(read_file(config_path), config, &bad);
    // The kernel starts one instance a boot; the lock only keeps a manual run off it, so a failed open is no reason to stop.
    const int lock = open(kLockPath, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (!print && lock >= 0 && flock(lock, LOCK_EX | LOCK_NB) != 0) return 2;
    signal(SIGPIPE, SIG_IGN);
    Sender sender(print, config.gap_ms);
    Rounds rounds(config);
    const double boot = boot_time(), started = monotonic();
    const std::string preference = config.migration && config.log ? ensure_log_preference(config) : std::string("off");
    char text[200];
    snprintf(text, sizeof text, "start v2 pid %d @%ld uid %d argv0 %s cfg r%zu/%u/%u mig%d%s%s", getpid(), uptime_seconds(), (int)geteuid(),
             argv0 ? argv0 : "-", config.rounds.size(), config.budget_lines, config.budget_bytes, config.migration ? 1 : 0,
             bad.empty() ? "" : " bad ", bad.c_str());
    text[kLineMax] = '\0';
    rounds.spend(text);
    if (!sender.send(text)) return sender.error() == ENOENT ? 3 : 4;
    Passes passes(config, sender, boot, preference);
    const bool passes_on = config.migration && config.minutes > 0;
    double next_pass = started;
    unsigned round = 0;
    while (!sender.closed()) {
        const double now = monotonic();
        const long up = uptime_seconds();
        if (round < config.rounds.size() && up >= long(config.rounds[round])) {
            run_round(++round, config, rounds, sender);
            continue;
        }
        const bool passes_left = passes_on && (!passes.count() || now - started < config.minutes * 60.0);
        if (passes_left && now >= next_pass) {
            next_pass = now + config.period;
            passes.run();
            continue;
        }
        double wait = -1;
        if (round < config.rounds.size()) wait = double(long(config.rounds[round]) - up);
        if (passes_left && (wait < 0 || next_pass - now < wait)) wait = next_pass - now;
        if (wait < 0) break;
        sleep_until(now + wait);
    }
    snprintf(text, sizeof text, "end r%u p%u @%ld log %u sup %u dup %u sent %u kd %u cut %u%s", round, passes.count(), uptime_seconds(),
             passes.rationer().logged(), passes.rationer().suppressed(), passes.rationer().duplicates(), sender.sent(), kernel_dropped(),
             rounds.cut(), sender.spooling() ? " spool" : "");
    text[kLineMax] = '\0';
    sender.send(text);
    return 0;
}

}  // namespace

int main(int argc, char **argv) {
    setvbuf(stdout, nullptr, _IOFBF, 1 << 16);
    const bool print = argc >= 2 && !strcmp(argv[1], "--print");
    if (!print) {
        // Below every interactive band, the probes too: an acceptance run must not change what it measures.
        setpriority(PRIO_DARWIN_PROCESS, 0, PRIO_DARWIN_BG);
        setiopolicy_np(IOPOL_TYPE_DISK, IOPOL_SCOPE_PROCESS, IOPOL_THROTTLE);
    }
    if (argc >= 3 && !strcmp(argv[1], "--probe")) {
        if (!strcmp(argv[2], "mg")) return probe_mg();
        if (!strcmp(argv[2], "pwr")) return probe_pwr();
        if (!strcmp(argv[2], "hid")) return probe_hid();
        if (!strcmp(argv[2], "kc")) return probe_kc();
        if (!strcmp(argv[2], "aud")) return probe_aud();
        if (!strcmp(argv[2], "sen")) return probe_sen();
        if (!strcmp(argv[2], "hap")) return probe_hap();
        if (!strcmp(argv[2], "wid")) return probe_wid();
        if (!strcmp(argv[2], "facts")) return probe_facts();
        if (!strcmp(argv[2], "state")) return probe_state();
        if (!strcmp(argv[2], "log")) return probe_log(argc >= 4 ? strtod(argv[3], nullptr) : 0, argc >= 5 ? strtod(argv[4], nullptr) : 27);
        return 64;
    }
    return run_daemon(print, argc >= 1 ? argv[0] : nullptr);
}
