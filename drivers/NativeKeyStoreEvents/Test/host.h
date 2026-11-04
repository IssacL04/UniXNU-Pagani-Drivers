// SPDX-License-Identifier: BSD-2-Clause-Patent
// Shared host fixture of the NativeKeyStoreEvents tests: the kernel services the driver calls (libkern AES over refaes.h,
// read_random, cc_clear, IOLock, boot arguments, entitlements, the process name, BootProgress's U lines and Stage7Probe's hooks),
// each with the knobs the tests turn, then the driver itself (one translation unit, as in the kernel build), and the helpers
// that call it the way IOKit does and read its DER the way libaks does.
#pragma once
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include <IOKit/IOService.h>
#include <libkern/crypto/aes.h>
#include <libkern/crypto/crypto_internal.h>
#include "refaes.h"

#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); abort(); } } while (0)

namespace host {
// Stage7Probe's hooks.
bool stateEnabled;
unsigned refused, opened;
uint32_t lockState;
std::mutex callsMutex;
std::vector<std::pair<uint32_t, int32_t>> calls;
// BootProgress U lines.
std::mutex traceMutex;
std::vector<std::string> traces;
// Boot argument pagani-keybag: -1 absent.
long bootArgKeybag = -1;
// The kernel AES: 0 correct, 1 key schedule fails, 2 block fails, 3 wrong output (a flipped bit).
std::atomic<int> aesMode{0};
std::atomic<unsigned> aesKeySchedules{0};
// read_random: 0 a deterministic stream, 1 zeros, 2 the same 32 bytes again and again.
int randomMode = 0;
std::atomic<unsigned> randomCalls{0};
uint64_t randomState = 0x243f6a8885a308d3ull;
// IOLock bookkeeping.
std::atomic<int> locksAlive{0}, lockAllocs{0};
bool lockAllocFails = false;
// Entitlements and process name.
OSObject *entitlementAnswer = nullptr;
std::atomic<unsigned> entitlementQueries{0};
const char *procName = "securityd";
std::atomic<uint64_t> clears{0};

// corecrypto's CBC mode descriptors: context sizes within libkern's fixed context unless a test says otherwise.
ccmode_cbc encryptMode{sizeof(aes_encrypt_ctx) - 8, 16}, decryptMode{sizeof(aes_decrypt_ctx) - 8, 16};
crypto_functions functions{&encryptMode, &decryptMode};

size_t tracesContaining(const char *text) {
    std::lock_guard<std::mutex> g(traceMutex);
    size_t n = 0;
    for (auto &t : traces) n += t.find(text) != std::string::npos;
    return n;
}
void clearTraces() { std::lock_guard<std::mutex> g(traceMutex); traces.clear(); }
}  // namespace host

OSBoolean trueObject, falseObject;
OSBoolean *kOSBooleanTrue = &trueObject;
OSBoolean *kOSBooleanFalse = &falseObject;
crypto_functions_t g_crypto_funcs = &host::functions;

extern "C" bool native_aks_state_enabled() { return host::stateEnabled; }
extern "C" uint32_t native_aks_lock_state() { return host::lockState; }
extern "C" void native_keyevents_open_refused() { ++host::refused; }
extern "C" void native_aks_opened() { ++host::opened; }
extern "C" void native_aks_call(uint32_t selector, int32_t result) {
    std::lock_guard<std::mutex> g(host::callsMutex);
    host::calls.emplace_back(selector, result);
}
extern "C" void boot_progress_trace(const char *text) {
    std::lock_guard<std::mutex> g(host::traceMutex);
    host::traces.emplace_back(text);
}
extern "C" void read_random(void *buffer, u_int numBytes) {
    ++host::randomCalls;
    auto *p = static_cast<uint8_t *>(buffer);
    for (u_int i = 0; i < numBytes; ++i) {
        if (host::randomMode == 1) { p[i] = 0; continue; }
        if (host::randomMode == 2) { p[i] = (uint8_t)(0x5a ^ (i % 32)); continue; }
        // splitmix64
        uint64_t z = (host::randomState += 0x9e3779b97f4a7c15ull);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull; z = (z ^ (z >> 27)) * 0x94d049bb133111ebull; z ^= z >> 31;
        p[i] = (uint8_t)z;
    }
}
extern "C" void proc_selfname(char *buf, int size) { snprintf(buf, (size_t)size, "%s", host::procName); }
void cc_clear(size_t len, void *dst) {
    volatile uint8_t *p = static_cast<volatile uint8_t *>(dst);
    for (size_t i = 0; i < len; ++i) p[i] = 0;
    ++host::clears;
}
bool PE_parse_boot_argn(const char *name, void *value, int size) {
    if (strcmp(name, "pagani-keybag") || host::bootArgKeybag < 0) return false;
    CHECK(size == (int)sizeof(uint32_t));
    const uint32_t v = (uint32_t)host::bootArgKeybag;
    memcpy(value, &v, sizeof v);
    return true;
}
IOLock *IOLockAlloc(void) {
    if (host::lockAllocFails) return nullptr;
    ++host::locksAlive; ++host::lockAllocs;
    return new IOLock;
}
void IOLockFree(IOLock *lock) { CHECK(lock && !lock->held); --host::locksAlive; delete lock; }
void IOLockLock(IOLock *lock) { lock->mutex.lock(); CHECK(!lock->held); lock->held = 1; }
void IOLockUnlock(IOLock *lock) { CHECK(lock->held); lock->held = 0; lock->mutex.unlock(); }
task_t current_task(void) { static int token; return reinterpret_cast<task_t>(&token); }
OSObject *IOUserClient::copyClientEntitlement(task_t task, const char *entitlement) {
    CHECK(task == current_task() && !strcmp(entitlement, "com.apple.keystore.access-keychain-keys"));
    ++host::entitlementQueries;
    return host::entitlementAnswer;
}

// libkern AES. The kernel's aes_encrypt_key panics when corecrypto's context is larger than libkern's: so does this one.
constexpr uint32_t kCtxMagic = 0x41455321;
int aes_encrypt_key(const unsigned char *key, int key_len, aes_encrypt_ctx cx[1]) {
    CHECK(g_crypto_funcs && g_crypto_funcs->ccaes_cbc_encrypt);
    if (g_crypto_funcs->ccaes_cbc_encrypt->size > sizeof(aes_encrypt_ctx)) { fprintf(stderr, "kernel panic: inconsistent size for AES encrypt context\n"); abort(); }
    ++host::aesKeySchedules;
    if (host::aesMode == 1) return aes_error;
    cx[0].rounds = refaes::expand(key, key_len, cx[0].roundKeys);
    cx[0].magic = kCtxMagic;
    return cx[0].rounds > 0 ? aes_good : aes_error;
}
int aes_encrypt(const unsigned char *in, unsigned char *out, aes_encrypt_ctx cx[1]) {
    CHECK(cx[0].magic == kCtxMagic);
    if (host::aesMode == 2) return aes_error;
    refaes::encrypt(cx[0].roundKeys, cx[0].rounds, in, out);
    if (host::aesMode == 3) out[5] ^= 0x10;
    return aes_good;
}
int aes_decrypt_key(const unsigned char *key, int key_len, aes_decrypt_ctx cx[1]) {
    CHECK(g_crypto_funcs && g_crypto_funcs->ccaes_cbc_decrypt);
    if (g_crypto_funcs->ccaes_cbc_decrypt->size > sizeof(aes_decrypt_ctx)) { fprintf(stderr, "kernel panic: inconsistent size for AES decrypt context\n"); abort(); }
    ++host::aesKeySchedules;
    if (host::aesMode == 1) return aes_error;
    cx[0].rounds = refaes::expand(key, key_len, cx[0].roundKeys);
    cx[0].magic = kCtxMagic;
    return cx[0].rounds > 0 ? aes_good : aes_error;
}
int aes_decrypt(const unsigned char *in, unsigned char *out, aes_decrypt_ctx cx[1]) {
    CHECK(cx[0].magic == kCtxMagic);
    if (host::aesMode == 2) return aes_error;
    refaes::decrypt(cx[0].roundKeys, cx[0].rounds, in, out);
    if (host::aesMode == 3) out[5] ^= 0x10;
    return aes_good;
}

#include "../NativeKeyStoreEvents.cpp"

namespace host {
// A fresh boot: no keybag state, no U lines, no recorded calls.
void freshBoot() {
    if (gKeybag.lock) IOLockFree(gKeybag.lock);
    memset(static_cast<void *>(&gKeybag), 0, sizeof gKeybag);
    clearTraces();
    { std::lock_guard<std::mutex> g(callsMutex); calls.clear(); }
    aesMode = 0; randomMode = 0; lockAllocFails = false; entitlementAnswer = nullptr; procName = "securityd"; bootArgKeybag = -1;
    randomCalls = 0; aesKeySchedules = 0; entitlementQueries = 0;
    g_crypto_funcs = &functions;
    encryptMode = {sizeof(aes_encrypt_ctx) - 8, 16}; decryptMode = {sizeof(aes_decrypt_ctx) - 8, 16};
}
struct Service {
    AppleKeyStore *service = nullptr;
    IOUserClient *client = nullptr;
    explicit Service(long bootArg = -1, bool open = true) {
        bootArgKeybag = bootArg;
        stateEnabled = true;
        service = static_cast<AppleKeyStore *>(native_keyevents_allocate());
        CHECK(static_cast<IOService *>(service)->start(nullptr));
        if (open) CHECK(static_cast<IOService *>(service)->newUserClient(nullptr, nullptr, 0, nullptr, &client) == kIOReturnSuccess && client);
    }
    IOUserClient *newClient() {
        IOUserClient *c = nullptr;
        CHECK(static_cast<IOService *>(service)->newUserClient(nullptr, nullptr, 0, nullptr, &c) == kIOReturnSuccess && c);
        return c;
    }
    ~Service() {
        if (client) client->release();
        static_cast<OSObject *>(service)->release();
    }
};
// One IOConnectCallMethod as is_io_connect_method hands it over: inline structures up to 4096 bytes, descriptors above.
struct Result {
    IOReturn rc = 0;
    std::vector<uint64_t> scalars;
    std::vector<uint8_t> out;
};
Result call(IOUserClient *c, uint32_t selector, std::vector<uint64_t> scalars, const std::vector<uint8_t> &in, uint32_t scalarOut,
            size_t outCapacity, IOMemoryDescriptor *outDescriptor = nullptr, IOMemoryDescriptor *inDescriptor = nullptr) {
    IOExternalMethodArguments a;
    a.selector = selector;
    a.scalarInput = scalars.data();
    a.scalarInputCount = (uint32_t)scalars.size();
    a.structureInput = in.empty() && !inDescriptor ? nullptr : (inDescriptor ? nullptr : in.data());
    a.structureInputSize = inDescriptor ? 0 : (uint32_t)in.size();
    a.structureInputDescriptor = inDescriptor;
    uint64_t scalarOutput[16];
    for (auto &s : scalarOutput) s = 0xdeadbeefcafef00dull;
    a.scalarOutput = scalarOut ? scalarOutput : nullptr;
    a.scalarOutputCount = scalarOut;
    std::vector<uint8_t> inband(outCapacity <= 4096 ? outCapacity : 0, 0xa5);
    if (outDescriptor) {
        a.structureOutputDescriptor = outDescriptor;
        a.structureOutputDescriptorSize = (uint32_t)outDescriptor->getLength();
    } else if (outCapacity) {
        CHECK(outCapacity <= 4096);
        a.structureOutput = inband.data();
        a.structureOutputSize = (uint32_t)outCapacity;
    }
    Result r;
    r.rc = c->externalMethod(selector, &a);
    if (r.rc == kIOReturnSuccess) {
        r.scalars.assign(scalarOutput, scalarOutput + a.scalarOutputCount);
        if (outDescriptor) r.out.assign(outDescriptor->bytes, outDescriptor->bytes + a.structureOutputDescriptorSize);
        else if (outCapacity) r.out.assign(inband.begin(), inband.begin() + a.structureOutputSize);
    } else if (!outDescriptor && outCapacity) {
        // A refused call leaves the caller's buffer as it was.
        for (auto b : inband) CHECK(b == 0xa5);
    }
    return r;
}
std::vector<uint8_t> bytes(size_t n, uint8_t seed) {
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = (uint8_t)(seed + 7 * i);
    return v;
}
uint64_t i32(int32_t v) { return (uint64_t)(int64_t)v; }   // libaks: sxtw

// libaks's reader (_der_dict_iterate 0x202fc1778 + _dict_find_params_cb, _der_get_number, _der_get_bool, _set_cf_key): a SET
// (0x31) of SEQUENCEs, each a UTF8String key with a two-byte header, then one value TLV.
struct DerValue { uint8_t tag; std::vector<uint8_t> content; };
bool derLength(const uint8_t *p, size_t n, size_t &i, size_t &len) {
    if (i >= n) return false;
    uint8_t b = p[i++];
    if (b < 0x80) { len = b; return true; }
    size_t count = b & 0x7f;
    if (!count || count > 2 || i + count > n) return false;
    len = 0;
    while (count--) len = (len << 8) | p[i++];
    return true;
}
bool libaksDecode(const uint8_t *p, size_t n, std::map<std::string, DerValue> &out) {
    size_t i = 0, total;
    if (n < 2 || p[i++] != 0x31 || !derLength(p, n, i, total) || i + total != n) return false;
    while (i < n) {
        size_t sl;
        if (p[i++] != 0x30 || !derLength(p, n, i, sl) || i + sl > n) return false;
        const size_t end = i + sl, keyStart = i;
        size_t kl;
        if (p[i++] != 0x0c || !derLength(p, n, i, kl) || i - keyStart != 2 || i + kl > end) return false;
        std::string key((const char *)p + i, kl);
        i += kl;
        DerValue v;
        size_t vl;
        if (i >= end) return false;
        v.tag = p[i++];
        if (!derLength(p, n, i, vl) || i + vl != end) return false;
        v.content.assign(p + i, p + end);
        if (out.count(key)) return false;   // _dict_find_params_cb marks duplicates
        out[key] = v;
        i = end;
    }
    return true;
}
int64_t derInteger(const DerValue &v) {
    CHECK(v.tag == 0x02 && !v.content.empty() && v.content.size() <= 8);
    int64_t x = (int8_t)v.content[0];
    for (size_t k = 1; k < v.content.size(); ++k) x = (int64_t)(((uint64_t)x << 8) | v.content[k]);
    return x;
}
// An RFC 3394 wrap written separately from the driver's, over refaes directly (RFC 3394 section 2.2.1, index form).
std::vector<uint8_t> referenceWrap(const uint8_t *kek, size_t kekLen, const std::vector<uint8_t> &p) {
    uint8_t rk[240];
    const int nr = refaes::expand(kek, (int)kekLen, rk);
    CHECK(nr > 0 && p.size() % 8 == 0 && p.size() >= 16);
    const size_t n = p.size() / 8;
    uint64_t a = 0xa6a6a6a6a6a6a6a6ull;
    std::vector<uint64_t> r(n + 1);
    for (size_t i = 1; i <= n; ++i) { uint64_t w = 0; for (int k = 0; k < 8; ++k) w = (w << 8) | p[8 * (i - 1) + k]; r[i] = w; }
    for (uint64_t j = 0; j <= 5; ++j) {
        for (size_t i = 1; i <= n; ++i) {
            uint8_t blk[16], outb[16];
            for (int k = 0; k < 8; ++k) { blk[k] = (uint8_t)(a >> (56 - 8 * k)); blk[8 + k] = (uint8_t)(r[i] >> (56 - 8 * k)); }
            refaes::encrypt(rk, nr, blk, outb);
            uint64_t msb = 0, lsb = 0;
            for (int k = 0; k < 8; ++k) { msb = (msb << 8) | outb[k]; lsb = (lsb << 8) | outb[8 + k]; }
            a = msb ^ (n * j + i);
            r[i] = lsb;
        }
    }
    std::vector<uint8_t> c(8 * (n + 1));
    for (int k = 0; k < 8; ++k) c[k] = (uint8_t)(a >> (56 - 8 * k));
    for (size_t i = 1; i <= n; ++i) for (int k = 0; k < 8; ++k) c[8 * i + k] = (uint8_t)(r[i] >> (56 - 8 * k));
    return c;
}
std::vector<uint8_t> hex(const char *s) {
    std::vector<uint8_t> v;
    for (size_t i = 0; s[i] && s[i + 1]; i += 2) {
        unsigned b;
        CHECK(sscanf(s + i, "%2x", &b) == 1);
        v.push_back((uint8_t)b);
    }
    return v;
}
}  // namespace host
