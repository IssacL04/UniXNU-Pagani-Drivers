// SPDX-License-Identifier: BSD-2-Clause-Patent
// AppleKeyStore for an ephemeral offline fixture: a notification service (the public IOKit matching class used by
// MKBEventsRegister) and, only when sheng-aks=1 is set, a user client that answers the unchanged libaks/MobileKeyBag truthfully
// for this environment: no passcode, nothing locked, and (pagani-keybag, default 1) a volatile software keybag whose class keys
// come from the kernel random generator at their first use in each boot. Stage7Probe starts this service only on volatile storage
// (/chosen ephemeral-storage = 1: the Data volume is a RAM disk made from the seed every boot), which is what makes per-boot keys
// enough: nothing wrapped in one boot is read in the next. It is not secure storage: no passcode, no hardware-bound key, nothing
// that protects data from anyone who can run code in the kernel or read the RAM (the RAM disk holds all data in the clear anyway).
//
// Answers (selectors and layouts read from the unchanged target's libaks/MobileKeyBag, see docs/touch-nt36532e.md; the set that is
// enough for the system daemons on iOS 16.7 was established on hardware by github.com/dipa-studio/ios-sweet, whose notes document it):
//   0, 4, 5, 9, 12, 13, 26, 27, 55, 101, 102   succeed, no outputs (open handshake, bag/assertion/notify bookkeeping)
//   14       aks_get_system                        scalar out: system keybag handle 0 (keybagd then needs no systembag.kb)
//   7        aks_get_lock_state(device)            scalar out: bit 2 = unlocked since boot
//   17, 35   aks_get_(extended_)device_state       struct out: DER SET{ SEQUENCE{ UTF8String key, INTEGER value } ... }
//            keys ss sls sb sfa sgs sas sgpe srcd sms bh; ss = 4, sls = lock state (0 unlocked; 3 would be "no passcode").
// Volatile keybag (pagani-keybag=1 or 2; =0 restores the table above exactly). Contracts from the iOS 17.7.11 (21H461)
// AppleKeyStore.framework client and securityd's static copy of the same client (IOConnectCallMethod arguments):
//   10      aks_wrap_key(key, len, class, bag, out, &outlen, &class_out)     (_aks_wrap_key 0x202fd5090; securityd 0x100019bf0)
//           scalar in {bag, class} (int32, sign-extended); struct in = the key; scalar out {class}; struct out = wrapped key.
//           RFC 3394 AES key wrap (default IV A6A6A6A6A6A6A6A6) under the class's 256-bit key: len + 8 bytes (securityd hands a
//           40-byte buffer for its 32-byte item and metadata keys and stores the class we return as the item's actual class).
//   11      aks_unwrap_key(wrapped, len, class, bag, out, &outlen)            (_aks_unwrap_key 0x202fd51a0; securityd 0x100014c70)
//           scalar in {bag, class}; struct in = the wrapped key; no scalar out; struct out = the key (len - 8; securityd: 32 bytes).
//   31      aks_get_configuration(bag) and aks_prederived_is_enabled(bag)    (0x202fc0648, 0x202fd40dc)
//           scalar in {bag}; struct out: a 32 KiB buffer, so an out-of-line descriptor; DER SET like the device state with
//           cf (ConfigFlags) 0 and mua (MaxUnlockAttempts) 10 (SpringBoard's own default); no prederived-passcode salt (pdps), so
//           aks_prederived_is_enabled answers false, as it must without a passcode.
//   8, 32   aks_lock_device(bag, flag)                                        (0x202fd80a8; 32 when flag != 0)
//           scalar in {bag}; there is nothing to lock without a passcode: succeeds and changes nothing.
//   Bags: 0 (device, what get_system hands out), -3 (MobileKeyBag's session alias), -6 (securityd's system keychain handle) all
//   name the one keybag; any other bag: kIOReturnNotFound. Class keys: 1-4 (A B C D) and 6-11 (ak ck dk aku cku dku). Class 12
//   (akpu, only with a passcode) and every other class: kIOReturnNotFound, which securityd reports as errSecNotAvailable "No key
//   available for class", as on a phone without a passcode. A blob that fails the RFC 3394 integrity check (tampered, other class,
//   other boot): 0xe007c013, libaks's decode error, on which securityd drops the item ("can't be decrypted on this device, ever").
//   Wrong scalar counts or bags, keys outside 16..512 bytes in 8-byte steps: kIOReturnBadArgument; a too small output buffer:
//   kIOReturnNoSpace; no kernel AES or a failed self-test: kIOReturnNotReady. pagani-keybag=2 also requires the caller's
//   com.apple.keystore.access-keychain-keys entitlement (securityd has it) for the keychain classes 6-12 (kIOReturnNotPrivileged).
// Every other selector fails with kIOReturnUnsupported. Failures reach the ramoops log as Stage7Probe's R lines (selector) and, once
// per kind, as U lines ("keybag: ...") with the process, bag, class and sizes.
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <libkern/libkern.h>
#include <libkern/crypto/aes.h>
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wextern-c-compat"
#include <libkern/crypto/crypto_internal.h>
#pragma clang diagnostic pop
#include <corecrypto/cc.h>
#include <pexpert/pexpert.h>
#include <kern/task.h>
extern "C" void native_keyevents_open_refused(void);
extern "C" bool native_aks_state_enabled(void);
extern "C" uint32_t native_aks_lock_state(void);
extern "C" void native_aks_call(uint32_t selector, int32_t result);
extern "C" void native_aks_opened(void);
extern "C" void boot_progress_trace(const char *text);    // BootProgress: one U line in the ramoops log
extern "C" void read_random(void *buffer, u_int numBytes);  // the kernel random generator (corecrypto's kernel PRNG)
extern "C" void proc_selfname(char *buf, int size);

namespace {
constexpr uint32_t kSelLockState = 7, kSelGetSystem = 14, kSelDeviceState = 17, kSelExtendedDeviceState = 35;
constexpr uint32_t kSelLockDevice = 8, kSelWrapKey = 10, kSelUnwrapKey = 11, kSelGetConfiguration = 31, kSelLockDeviceFlag = 32;
constexpr uint64_t kUnlockedSinceBoot = 4;
constexpr uint32_t kStateFlags = 4;
constexpr uint64_t kSystemHandle = 0;
constexpr IOReturn kAksError = (IOReturn)0xe00002bc;   // what libaks reports itself when the connection is missing
constexpr IOReturn kAksUnsupported = (IOReturn)0xe00002c7;
constexpr IOReturn kAksBadArgument = (IOReturn)0xe00002c2;   // kIOReturnBadArgument
constexpr IOReturn kAksNotPrivileged = (IOReturn)0xe00002c1; // kIOReturnNotPrivileged
constexpr IOReturn kAksNoSpace = (IOReturn)0xe00002c4;       // kIOReturnNoSpace
constexpr IOReturn kAksNotReady = (IOReturn)0xe00002d8;      // kIOReturnNotReady
constexpr IOReturn kAksNotFound = (IOReturn)0xe00002f0;      // kIOReturnNotFound: no key for the class, no such bag
constexpr IOReturn kAksDecodeError = (IOReturn)0xe007c013;   // libaks decode error (securityd ks_crypt: drop the item)

struct DerWriter {
    uint8_t *p; size_t cap, n = 0; bool ok = true;
    void put(uint8_t b) { if (n < cap) p[n++] = b; else ok = false; }
    void len(size_t l) { if (l < 0x80) put((uint8_t)l); else { put(0x81); put((uint8_t)l); } }   // entries stay below 256 bytes
};
// Smallest two's-complement big-endian INTEGER content for a non-negative value.
size_t derIntegerContent(uint64_t v, uint8_t out[9]) {
    uint8_t raw[8];
    for (int i = 0; i < 8; ++i) raw[i] = (uint8_t)(v >> (56 - 8 * i));
    size_t start = 0;
    while (start < 7 && raw[start] == 0) ++start;
    size_t n = 0;
    if (raw[start] & 0x80) out[n++] = 0;
    while (start < 8) out[n++] = raw[start++];
    return n;
}
size_t entrySize(const char *key, uint64_t value) {
    uint8_t c[9]; size_t kl = 0; while (key[kl]) ++kl;
    size_t body = (2 + kl) + (2 + derIntegerContent(value, c));
    return 2 + body;
}
struct DerEntry { const char *key; uint64_t value; };
// DER SET of SEQUENCE{UTF8String, INTEGER}, the shape libaks's der_dict_iterate reads. Returns the length, or 0 when too small.
size_t encodeIntegerSet(const DerEntry *entries, size_t count, uint8_t *out, size_t capacity) {
    size_t total = 0;
    for (size_t i = 0; i < count; ++i) total += entrySize(entries[i].key, entries[i].value);
    DerWriter w{out, capacity};
    w.put(0x31); w.len(total);
    for (size_t e = 0; e < count; ++e) {
        uint8_t c[9]; size_t kl = 0; while (entries[e].key[kl]) ++kl;
        size_t il = derIntegerContent(entries[e].value, c);
        w.put(0x30); w.len((2 + kl) + (2 + il));
        w.put(0x0c); w.put((uint8_t)kl); for (size_t i = 0; i < kl; ++i) w.put((uint8_t)entries[e].key[i]);
        w.put(0x02); w.put((uint8_t)il); for (size_t i = 0; i < il; ++i) w.put(c[i]);
    }
    return w.ok ? w.n : 0;
}

// ---- RFC 3394 AES key wrap over the kernel's AES (libkern aes_encrypt/aes_decrypt: corecrypto's registered AES, one block) ----
constexpr size_t kClassKeyBytes = 32, kWrapMinKey = 16, kWrapMaxKey = 512, kWrapOverhead = 8;
constexpr uint8_t kWrapIV[8] = {0xa6, 0xa6, 0xa6, 0xa6, 0xa6, 0xa6, 0xa6, 0xa6};
enum WrapStatus { kWrapOk = 0, kWrapAesFailed = 1, kWrapIntegrity = 2, kWrapBadLength = 3 };
bool wrapLengthOk(size_t n) { return n >= kWrapMinKey && n <= kWrapMaxKey && n % 8 == 0; }
void xorCounter(uint8_t a[8], uint64_t t) { for (int k = 0; k < 8; ++k) a[7 - k] ^= (uint8_t)(t >> (8 * k)); }
// The kernel's AES may only be used when corecrypto registered its CBC modes and their contexts fit libkern's fixed context
// (aes_encrypt_key panics otherwise): checked here, never assumed.
bool kernelAesUsable() {
    return g_crypto_funcs && g_crypto_funcs->ccaes_cbc_encrypt && g_crypto_funcs->ccaes_cbc_decrypt &&
           g_crypto_funcs->ccaes_cbc_encrypt->block_size == 16 && g_crypto_funcs->ccaes_cbc_decrypt->block_size == 16 &&
           g_crypto_funcs->ccaes_cbc_encrypt->size <= sizeof(aes_encrypt_ctx) &&
           g_crypto_funcs->ccaes_cbc_decrypt->size <= sizeof(aes_decrypt_ctx);
}
// out: n + 8 bytes, must not overlap plain.
WrapStatus rfc3394Wrap(const uint8_t *kek, size_t kekLen, const uint8_t *plain, size_t n, uint8_t *out) {
    if (!wrapLengthOk(n) || (kekLen != 16 && kekLen != 24 && kekLen != 32)) return kWrapBadLength;
    aes_encrypt_ctx ctx;
    WrapStatus status = aes_encrypt_key(kek, (int)kekLen, &ctx) == aes_good ? kWrapOk : kWrapAesFailed;
    uint8_t a[8], in[16], b[16];
    memcpy(a, kWrapIV, 8);
    memcpy(out + 8, plain, n);
    const size_t blocks = n / 8;
    for (unsigned j = 0; j < 6 && status == kWrapOk; ++j) {
        for (size_t i = 1; i <= blocks; ++i) {
            memcpy(in, a, 8); memcpy(in + 8, out + 8 * i, 8);
            if (aes_encrypt(in, b, &ctx) != aes_good) { status = kWrapAesFailed; break; }
            memcpy(a, b, 8); xorCounter(a, (uint64_t)blocks * j + i);
            memcpy(out + 8 * i, b + 8, 8);
        }
    }
    memcpy(out, a, 8);
    if (status != kWrapOk) cc_clear(n + kWrapOverhead, out);
    cc_clear(sizeof ctx, &ctx); cc_clear(sizeof in, in); cc_clear(sizeof b, b); cc_clear(sizeof a, a);
    return status;
}
// plain: m - 8 bytes of kernel memory, the work area; cleared again unless the integrity check passed. The caller copies it out
// only after that, so an unverified key never reaches the caller's buffer.
WrapStatus rfc3394Unwrap(const uint8_t *kek, size_t kekLen, const uint8_t *wrapped, size_t m, uint8_t *plain) {
    if (m < kWrapOverhead || !wrapLengthOk(m - kWrapOverhead) || (kekLen != 16 && kekLen != 24 && kekLen != 32)) return kWrapBadLength;
    aes_decrypt_ctx ctx;
    WrapStatus status = aes_decrypt_key(kek, (int)kekLen, &ctx) == aes_good ? kWrapOk : kWrapAesFailed;
    const size_t blocks = m / 8 - 1;
    uint8_t a[8], in[16], b[16];
    memcpy(a, wrapped, 8);
    memcpy(plain, wrapped + 8, m - 8);
    for (int j = 5; j >= 0 && status == kWrapOk; --j) {
        for (size_t i = blocks; i >= 1; --i) {
            memcpy(in, a, 8); xorCounter(in, (uint64_t)blocks * (unsigned)j + i);
            memcpy(in + 8, plain + 8 * (i - 1), 8);
            if (aes_decrypt(in, b, &ctx) != aes_good) { status = kWrapAesFailed; break; }
            memcpy(a, b, 8); memcpy(plain + 8 * (i - 1), b + 8, 8);
        }
    }
    unsigned diff = 0;
    for (int k = 0; k < 8; ++k) diff |= (unsigned)(a[k] ^ kWrapIV[k]);   // no early exit
    if (status == kWrapOk && diff) status = kWrapIntegrity;
    if (status != kWrapOk) cc_clear(m - kWrapOverhead, plain);
    cc_clear(sizeof ctx, &ctx); cc_clear(sizeof in, in); cc_clear(sizeof b, b); cc_clear(sizeof a, a);
    return status;
}

// ---- The volatile keybag: one 256-bit key per class with a key, drawn at the first wrap/unwrap of the boot ----
constexpr int32_t kKeyedClasses[] = {1, 2, 3, 4, 6, 7, 8, 9, 10, 11};
constexpr size_t kClassSlots = sizeof kKeyedClasses / sizeof kKeyedClasses[0];
constexpr int32_t kClassAkpu = 12;
constexpr const char *kKeychainEntitlement = "com.apple.keystore.access-keychain-keys";
int classSlot(int32_t keyClass) {
    for (size_t i = 0; i < kClassSlots; ++i) if (kKeyedClasses[i] == keyClass) return (int)i;
    return -1;
}
bool knownBag(int32_t bag) { return bag == 0 || bag == -3 || bag == -6; }
enum KeybagState : uint32_t { kKeysAbsent = 0, kKeysReady = 1, kKeysFailed = 2 };
// Notes (U lines) written once per boot and kind.
enum NoteKind : uint32_t {
    kNoteMode = 1u << 0, kNoteReady = 1u << 1, kNoteFailed = 1u << 2, kNoteFirstWrap = 1u << 3, kNoteFirstUnwrap = 1u << 4,
    kNotePasscodeClass = 1u << 5, kNoteUnknownClass = 1u << 6, kNoteBag = 1u << 7, kNoteIntegrity = 1u << 8, kNoteSize = 1u << 9,
    kNoteSpace = 1u << 10, kNoteArguments = 1u << 11, kNoteEntitlement = 1u << 12, kNoteConfiguration = 1u << 13,
    kNoteLock = 1u << 14, kNoteOutput = 1u << 15, kNoteAes = 1u << 16, kNoteNotReady = 1u << 17,
};
// Plain data: zero-initialized, no static constructor (the archive's late initializer only registers the two metaclasses).
struct VolatileKeybag {
    uint32_t mode;      // pagani-keybag: 0 off, 1 on, 2 on and keychain classes need the entitlement
    uint32_t state;     // KeybagState
    uint32_t noted;     // NoteKind bits
    IOLock *lock;       // first-use key creation
    uint64_t wraps, unwraps, refusals;
    uint8_t keys[kClassSlots][kClassKeyBytes];
};
VolatileKeybag gKeybag;

void note(uint32_t kind, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void note(uint32_t kind, const char *fmt, ...) {
    if (kind && (__atomic_fetch_or(&gKeybag.noted, kind, __ATOMIC_RELAXED) & kind)) return;
    char line[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    boot_progress_trace(line);
}
// The usage summary at 64, 1024 and 16384 successful wraps and unwraps (the first of each has its own line).
void noteProgress() {
    const uint64_t done = __atomic_load_n(&gKeybag.wraps, __ATOMIC_RELAXED) + __atomic_load_n(&gKeybag.unwraps, __ATOMIC_RELAXED);
    if (done == 64 || done == 1024 || done == 16384)
        note(0, "keybag: %llu wraps, %llu unwraps, %llu refused", (unsigned long long)__atomic_load_n(&gKeybag.wraps, __ATOMIC_RELAXED),
             (unsigned long long)__atomic_load_n(&gKeybag.unwraps, __ATOMIC_RELAXED),
             (unsigned long long)__atomic_load_n(&gKeybag.refusals, __ATOMIC_RELAXED));
}
void wipeKeys() {
    cc_clear(sizeof gKeybag.keys, gKeybag.keys);
    __atomic_store_n(&gKeybag.state, (uint32_t)kKeysAbsent, __ATOMIC_RELEASE);
}
// RFC 3394 section 4.6 (256-bit key data, 256-bit KEK) through the kernel AES, back again, and a flipped bit refused.
bool selfTest() {
    static const uint8_t kek[32] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
                                    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f};
    static const uint8_t data[32] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
                                     0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f};
    static const uint8_t expected[40] = {0x28, 0xc9, 0xf4, 0x04, 0xc4, 0xb8, 0x10, 0xf4, 0xcb, 0xcc, 0xb3, 0x5c, 0xfb, 0x87,
                                         0xf8, 0x26, 0x3f, 0x57, 0x86, 0xe2, 0xd8, 0x0e, 0xd3, 0x26, 0xcb, 0xc7, 0xf0, 0xe7,
                                         0x1a, 0x99, 0xf4, 0x3b, 0xfb, 0x98, 0x8b, 0x9b, 0x7a, 0x02, 0xdd, 0x21};
    uint8_t wrapped[40], back[32];
    bool ok = rfc3394Wrap(kek, 32, data, 32, wrapped) == kWrapOk && !memcmp(wrapped, expected, 40) &&
              rfc3394Unwrap(kek, 32, wrapped, 40, back) == kWrapOk && !memcmp(back, data, 32);
    wrapped[17] ^= 0x01;
    ok = ok && rfc3394Unwrap(kek, 32, wrapped, 40, back) == kWrapIntegrity;
    cc_clear(sizeof wrapped, wrapped); cc_clear(sizeof back, back);
    return ok;
}
// The class keys of this boot: created once under the lock, read without it afterwards.
bool ensureKeys() {
    uint32_t state = __atomic_load_n(&gKeybag.state, __ATOMIC_ACQUIRE);
    if (state != kKeysAbsent) return state == kKeysReady;
    if (!gKeybag.lock) return false;
    IOLockLock(gKeybag.lock);
    state = __atomic_load_n(&gKeybag.state, __ATOMIC_ACQUIRE);
    if (state == kKeysAbsent) {
        const char *why = nullptr;
        if (!kernelAesUsable()) why = "no kernel AES (corecrypto CBC modes not registered)";
        else if (!selfTest()) why = "AES key wrap self-test failed";
        else {
            read_random(gKeybag.keys, (u_int)sizeof gKeybag.keys);
            // A generator that returned nothing would leave equal (zero) keys: refuse rather than wrap with them.
            for (size_t i = 0; i < kClassSlots && !why; ++i) {
                uint8_t any = 0;
                for (size_t k = 0; k < kClassKeyBytes; ++k) any |= gKeybag.keys[i][k];
                if (!any) why = "random generator returned a zero key";
                for (size_t j = 0; j < i && !why; ++j)
                    if (!memcmp(gKeybag.keys[i], gKeybag.keys[j], kClassKeyBytes)) why = "random generator repeated a key";
            }
        }
        if (why) {
            cc_clear(sizeof gKeybag.keys, gKeybag.keys);
            __atomic_store_n(&gKeybag.state, (uint32_t)kKeysFailed, __ATOMIC_RELEASE);
            note(kNoteFailed, "keybag: off for this boot: %s", why);
        } else {
            __atomic_store_n(&gKeybag.state, (uint32_t)kKeysReady, __ATOMIC_RELEASE);
            note(kNoteReady, "keybag: up: %u class keys (classes 1-4, 6-11) from the kernel random generator, RFC 3394 self-test ok",
                 (unsigned)kClassSlots);
        }
        state = __atomic_load_n(&gKeybag.state, __ATOMIC_ACQUIRE);
    }
    IOLockUnlock(gKeybag.lock);
    return state == kKeysReady;
}

// A scalar holding an int32 (libaks sign-extends; a zero-extended value is taken too).
bool scalarInt32(uint64_t v, int32_t *out) {
    const int32_t s = (int32_t)(uint32_t)v;
    if (v != (uint64_t)(int64_t)s && v != (uint64_t)(uint32_t)s) return false;
    *out = s;
    return true;
}
size_t outputCapacity(const IOExternalMethodArguments *args) {
    if (args->structureOutputDescriptor) return args->structureOutputDescriptor->getLength();
    return args->structureOutput ? args->structureOutputSize : 0;
}
// prepare() wires the caller's whole buffer, whose size the caller picks (up to 4 GiB): no answer here comes near this, libaks hands 32 KiB.
constexpr size_t kOutOfLineMax = 1u << 20;
// The struct output, inline (at most 4096 bytes asked) or through the caller's out-of-line descriptor, which must be prepared
// (wired) before writeBytes touches it: writing an unprepared user descriptor panics (IOGMD: not wired).
IOReturn writeOutput(IOExternalMethodArguments *args, const void *data, size_t n) {
    if (IOMemoryDescriptor *md = args->structureOutputDescriptor) {
        if (n > md->getLength()) return kAksNoSpace;
        if (md->getLength() > kOutOfLineMax) return kAksBadArgument;
        IOReturn rc = md->prepare();
        if (rc != kIOReturnSuccess) return rc;
        const IOByteCount written = md->writeBytes(0, data, n);
        md->complete();
        if (written != n) return kAksError;
        args->structureOutputDescriptorSize = (uint32_t)n;
        return kIOReturnSuccess;
    }
    if (!args->structureOutput || n > args->structureOutputSize) return kAksNoSpace;
    memcpy(args->structureOutput, data, n);
    args->structureOutputSize = (uint32_t)n;
    return kIOReturnSuccess;
}
}  // namespace

// DER SET of SEQUENCE{UTF8String, INTEGER}. Returns the length, or 0 when the buffer is too small.
extern "C" size_t native_aks_encode_state(uint8_t *out, size_t capacity, uint32_t lockState) {
    const DerEntry entries[] = {{"ss", kStateFlags}, {"sls", lockState}, {"sb", 0}, {"sfa", 0}, {"sgs", 0},
                                {"sas", 0}, {"sgpe", 0}, {"srcd", 0}, {"sms", 0}, {"bh", 0}};
    return encodeIntegerSet(entries, sizeof entries / sizeof entries[0], out, capacity);
}
// The configuration of a keybag without a passcode: ConfigFlags 0 (no inactivity reboot) and MaxUnlockAttempts 10 (SpringBoard's
// own default when the value is missing). No grace period, escrow, recovery, memento or prederived-passcode entries.
extern "C" size_t native_aks_encode_configuration(uint8_t *out, size_t capacity) {
    const DerEntry entries[] = {{"cf", 0}, {"mua", 10}};
    return encodeIntegerSet(entries, sizeof entries / sizeof entries[0], out, capacity);
}

class AppleKeyStoreUserClient final : public IOUserClient {
    OSDeclareDefaultStructors(AppleKeyStoreUserClient)
    uint32_t keychainEntitled = 0;   // 0 not asked yet, 1 yes, 2 no (pagani-keybag=2 only)

    static IOReturn refuse(IOReturn rc, uint32_t kind, const char *what, int32_t bag, int32_t keyClass, size_t in, size_t out) {
        __atomic_fetch_add(&gKeybag.refusals, 1, __ATOMIC_RELAXED);
        if (!(__atomic_load_n(&gKeybag.noted, __ATOMIC_RELAXED) & kind)) {
            char name[24] = {};
            proc_selfname(name, (int)sizeof name);
            note(kind, "keybag: refused %s for %s: bag %d class %d in %zu out %zu rc 0x%x", what, name, bag, keyClass, in, out,
                 (unsigned)rc);
        }
        return rc;
    }
    bool entitledForKeychain() {
        uint32_t known = __atomic_load_n(&keychainEntitled, __ATOMIC_RELAXED);
        if (!known) {
            OSObject *value = IOUserClient::copyClientEntitlement(current_task(), kKeychainEntitlement);
            known = value == kOSBooleanTrue ? 1 : 2;
            if (value) value->release();
            __atomic_store_n(&keychainEntitled, known, __ATOMIC_RELAXED);
        }
        return known == 1;
    }
    // Bag and class (selectors 10 and 11): the common checks, in the order a refusal is reported.
    IOReturn keyArguments(IOExternalMethodArguments *args, const char *what, int32_t *bag, int32_t *keyClass, const uint8_t **in,
                          size_t *inSize) {
        *bag = 0; *keyClass = 0; *in = nullptr; *inSize = 0;
        if (args->scalarInputCount != 2 || !args->scalarInput || !scalarInt32(args->scalarInput[0], bag) ||
            !scalarInt32(args->scalarInput[1], keyClass))
            return refuse(kAksBadArgument, kNoteArguments, what, *bag, *keyClass, 0, 0);
        if (!knownBag(*bag)) return refuse(kAksNotFound, kNoteBag, what, *bag, *keyClass, 0, 0);
        if (classSlot(*keyClass) < 0)
            return refuse(kAksNotFound, *keyClass == kClassAkpu ? kNotePasscodeClass : kNoteUnknownClass, what, *bag, *keyClass, 0, 0);
        if (gKeybag.mode == 2 && *keyClass >= 6 && !entitledForKeychain())
            return refuse(kAksNotPrivileged, kNoteEntitlement, what, *bag, *keyClass, 0, 0);
        // Keys are at most 520 bytes: always inline (an out-of-line input means more than 4096 bytes).
        if (args->structureInputDescriptor || !args->structureInput)
            return refuse(kAksBadArgument, kNoteSize, what, *bag, *keyClass, args->structureInputDescriptor ? 4097 : 0, 0);
        *in = static_cast<const uint8_t *>(args->structureInput);
        *inSize = args->structureInputSize;
        return kIOReturnSuccess;
    }
    IOReturn wrapKey(IOExternalMethodArguments *args) {
        int32_t bag, keyClass; const uint8_t *in; size_t n;
        IOReturn rc = keyArguments(args, "wrap", &bag, &keyClass, &in, &n);
        if (rc != kIOReturnSuccess) return rc;
        if (args->scalarOutputCount < 1 || !args->scalarOutput) return refuse(kAksBadArgument, kNoteArguments, "wrap", bag, keyClass, n, 0);
        if (!wrapLengthOk(n)) return refuse(kAksBadArgument, kNoteSize, "wrap", bag, keyClass, n, outputCapacity(args));
        if (outputCapacity(args) < n + kWrapOverhead) return refuse(kAksNoSpace, kNoteSpace, "wrap", bag, keyClass, n, outputCapacity(args));
        if (!ensureKeys()) return refuse(kAksNotReady, kNoteNotReady, "wrap (no keys)", bag, keyClass, n, 0);
        uint8_t wrapped[kWrapMaxKey + kWrapOverhead];
        if (rfc3394Wrap(gKeybag.keys[classSlot(keyClass)], kClassKeyBytes, in, n, wrapped) != kWrapOk) {
            cc_clear(sizeof wrapped, wrapped);
            return refuse(kAksError, kNoteAes, "wrap (AES)", bag, keyClass, n, 0);
        }
        rc = writeOutput(args, wrapped, n + kWrapOverhead);
        cc_clear(sizeof wrapped, wrapped);
        if (rc != kIOReturnSuccess) return refuse(rc, kNoteOutput, "wrap output", bag, keyClass, n, outputCapacity(args));
        args->scalarOutput[0] = (uint64_t)(int64_t)keyClass;
        args->scalarOutputCount = 1;
        __atomic_fetch_add(&gKeybag.wraps, 1, __ATOMIC_RELAXED);
        if (!(__atomic_load_n(&gKeybag.noted, __ATOMIC_RELAXED) & kNoteFirstWrap)) {
            char name[24] = {};
            proc_selfname(name, (int)sizeof name);
            note(kNoteFirstWrap, "keybag: first wrap by %s: bag %d class %d %zu -> %zu bytes", name, bag, keyClass, n, n + kWrapOverhead);
        }
        noteProgress();
        return kIOReturnSuccess;
    }
    IOReturn unwrapKey(IOExternalMethodArguments *args) {
        int32_t bag, keyClass; const uint8_t *in; size_t m;
        IOReturn rc = keyArguments(args, "unwrap", &bag, &keyClass, &in, &m);
        if (rc != kIOReturnSuccess) return rc;
        args->scalarOutputCount = 0;
        // Not a whole wrapped key of a length wrap can produce: it can never be decrypted (securityd then drops the item).
        if (m < kWrapOverhead || !wrapLengthOk(m - kWrapOverhead))
            return refuse(kAksDecodeError, kNoteSize, "unwrap", bag, keyClass, m, outputCapacity(args));
        if (outputCapacity(args) < m - kWrapOverhead)
            return refuse(kAksNoSpace, kNoteSpace, "unwrap", bag, keyClass, m, outputCapacity(args));
        if (!ensureKeys()) return refuse(kAksNotReady, kNoteNotReady, "unwrap (no keys)", bag, keyClass, m, 0);
        uint8_t plain[kWrapMaxKey];
        const WrapStatus status = rfc3394Unwrap(gKeybag.keys[classSlot(keyClass)], kClassKeyBytes, in, m, plain);
        if (status != kWrapOk) {
            cc_clear(sizeof plain, plain);
            return status == kWrapIntegrity ? refuse(kAksDecodeError, kNoteIntegrity, "unwrap (integrity)", bag, keyClass, m, 0)
                                            : refuse(kAksError, kNoteAes, "unwrap (AES)", bag, keyClass, m, 0);
        }
        rc = writeOutput(args, plain, m - kWrapOverhead);
        cc_clear(sizeof plain, plain);
        if (rc != kIOReturnSuccess) return refuse(rc, kNoteOutput, "unwrap output", bag, keyClass, m, outputCapacity(args));
        __atomic_fetch_add(&gKeybag.unwraps, 1, __ATOMIC_RELAXED);
        if (!(__atomic_load_n(&gKeybag.noted, __ATOMIC_RELAXED) & kNoteFirstUnwrap)) {
            char name[24] = {};
            proc_selfname(name, (int)sizeof name);
            note(kNoteFirstUnwrap, "keybag: first unwrap by %s: bag %d class %d %zu -> %zu bytes", name, bag, keyClass, m,
                 m - kWrapOverhead);
        }
        noteProgress();
        return kIOReturnSuccess;
    }
    // Selectors 31, 8 and 32 take the bag as their only scalar.
    IOReturn bagArgument(IOExternalMethodArguments *args, const char *what) {
        int32_t bag = 0;
        if (args->scalarInputCount != 1 || !args->scalarInput || !scalarInt32(args->scalarInput[0], &bag))
            return refuse(kAksBadArgument, kNoteArguments, what, bag, 0, 0, 0);
        if (!knownBag(bag)) return refuse(kAksNotFound, kNoteBag, what, bag, 0, 0, 0);
        args->scalarOutputCount = 0;
        return kIOReturnSuccess;
    }
    IOReturn configuration(IOExternalMethodArguments *args) {
        IOReturn rc = bagArgument(args, "configuration");
        if (rc != kIOReturnSuccess) return rc;
        uint8_t der[32];
        const size_t n = native_aks_encode_configuration(der, sizeof der);
        rc = n ? writeOutput(args, der, n) : kAksError;
        if (rc != kIOReturnSuccess) return refuse(rc, kNoteOutput, "configuration output", 0, 0, 0, outputCapacity(args));
        note(kNoteConfiguration, "keybag: configuration answered (%zu DER bytes: no passcode, no prederived secret)", n);
        return kIOReturnSuccess;
    }
    IOReturn lockDevice(IOExternalMethodArguments *args) {
        IOReturn rc = bagArgument(args, "lock");
        if (rc == kIOReturnSuccess) note(kNoteLock, "keybag: lock request answered: no passcode, nothing to lock");
        return rc;
    }
public:
    IOReturn clientClose() override { terminate(); return kIOReturnSuccess; }
    IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args, IOExternalMethodDispatch *,
                            OSObject *, void *) override {
        IOReturn rc = kAksUnsupported;
        const bool keybag = __atomic_load_n(&gKeybag.mode, __ATOMIC_RELAXED) != 0;
        if (!args) rc = kAksError;
        else switch (selector) {
        // Handshake right after IOServiceOpen (a failure makes libaks drop the connection and reopen on every call), and the
        // bag/assertion/notification calls that need no answer in an environment without keybags.
        case 0: case 4: case 5: case 9: case 12: case 13: case 26: case 27: case 55: case 101: case 102:
            args->scalarOutputCount = 0;
            rc = kIOReturnSuccess;
            break;
        case kSelGetSystem:
            if (args->scalarOutputCount < 1 || !args->scalarOutput) { rc = kIOReturnBadArgument; break; }
            args->scalarOutput[0] = kSystemHandle;
            args->scalarOutputCount = 1;
            rc = kIOReturnSuccess;
            break;
        case kSelLockState:
            rc = (args->scalarInputCount == 1 && args->scalarOutputCount == 1 && args->scalarOutput)
                     ? kIOReturnSuccess : kIOReturnBadArgument;
            if (rc == kIOReturnSuccess) args->scalarOutput[0] = kUnlockedSinceBoot;
            break;
        case kSelDeviceState:
        case kSelExtendedDeviceState: {
            if (args->scalarInputCount > 1 || args->scalarOutputCount != 0) { rc = kIOReturnBadArgument; break; }
            uint8_t der[160];
            size_t n = native_aks_encode_state(der, sizeof der, native_aks_lock_state());
            rc = n ? writeOutput(args, der, n) : kIOReturnNoSpace;
            break;
        }
        case kSelWrapKey: if (keybag) rc = wrapKey(args); break;
        case kSelUnwrapKey: if (keybag) rc = unwrapKey(args); break;
        case kSelGetConfiguration: if (keybag) rc = configuration(args); break;
        case kSelLockDevice: case kSelLockDeviceFlag: if (keybag) rc = lockDevice(args); break;
        default: break;
        }
        native_aks_call(selector, (int32_t)rc);
        return rc;
    }
};
OSDefineMetaClassAndStructors(AppleKeyStoreUserClient, IOUserClient)

class AppleKeyStore final : public IOService {
    OSDeclareDefaultStructors(AppleKeyStore)
    IOReturn open(task_t task, void *security, UInt32 type, OSDictionary *properties, IOUserClient **out) {
        if (out) *out = nullptr;
        if (!native_aks_state_enabled() || !out) { native_keyevents_open_refused(); return kIOReturnNotReady; }
        AppleKeyStoreUserClient *client = new AppleKeyStoreUserClient;
        if (!client) return kIOReturnNoMemory;
        if (!client->initWithTask(task, security, type, properties) || !client->attach(this)) {
            client->release(); return kIOReturnError;
        }
        if (!client->start(this)) { client->detach(this); client->release(); return kIOReturnError; }
        native_aks_opened();
        *out = client;
        return kIOReturnSuccess;
    }
    // Teardown: the class keys never outlive the service (every blob wrapped with them becomes undecryptable). stop() switches the
    // keybag off and zeroes the keys; the lock goes only with the last reference (free), when no client can still be inside it.
    static void teardown(bool releaseLock) {
        __atomic_store_n(&gKeybag.mode, 0u, __ATOMIC_RELAXED);
        wipeKeys();
        if (!releaseLock) return;
        if (IOLock *lock = gKeybag.lock) { gKeybag.lock = nullptr; IOLockFree(lock); }
    }
public:
    bool start(IOService *provider) override {
        if (!IOService::start(provider)) return false;
        uint32_t mode = 1;
        if (!PE_parse_boot_argn("pagani-keybag", &mode, sizeof mode)) mode = 1;
        if (mode > 2) mode = 1;
        if (mode && !gKeybag.lock) gKeybag.lock = IOLockAlloc();
        if (mode && !gKeybag.lock) { note(kNoteFailed, "keybag: off: no lock"); mode = 0; }
        __atomic_store_n(&gKeybag.mode, mode, __ATOMIC_RELAXED);
        note(kNoteMode, mode ? "keybag: pagani-keybag=%u: volatile class keys at first use (bags 0/-3/-6, classes 1-4 6-11)"
                             : "keybag: pagani-keybag=%u: state answers only, no key operations", mode);
        setName("Native keystore state service");
        if (!setProperty("NativeKeyStoreEvents", true) ||
            !setProperty("KeyOperationsAvailable", mode != 0)) {
            teardown(true);   // no client was ever opened
            IOService::stop(provider);
            return false;
        }
        registerService();
        return true;
    }
    void stop(IOService *provider) override { teardown(false); IOService::stop(provider); }
    void free() override { teardown(true); IOService::free(); }
    IOReturn newUserClient(task_t t, void *s, UInt32 type, OSDictionary *p, IOUserClient **out) override {
        return open(t, s, type, p, out);
    }
    IOReturn newUserClient(task_t t, void *s, UInt32 type, IOUserClient **out) override {
        return open(t, s, type, nullptr, out);
    }
};
OSDefineMetaClassAndStructors(AppleKeyStore, IOService)
extern "C" IOService *native_keyevents_allocate(void) { return new AppleKeyStore; }
