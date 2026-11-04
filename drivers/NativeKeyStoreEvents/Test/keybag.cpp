// SPDX-License-Identifier: BSD-2-Clause-Patent
// The volatile keybag of NativeKeyStoreEvents.cpp on the host: the AES and RFC 3394 underneath it against published vectors, the
// IOConnectCallMethod contracts of libaks 21H461 (wrap 10, unwrap 11, configuration 31, lock 8/32) with the buffers securityd
// really hands over, round trips for every class and bag alias, refusals (class 12, unknown classes and bags, wrong sizes, short
// outputs, tampered/foreign blobs), the kernel-AES and random-generator guards, zeroization at teardown, pagani-keybag=0 and =2,
// the U lines, and concurrent first use.
#include "host.h"
#include <set>
#include <thread>

using namespace host;

static const int32_t kKeyed[] = {1, 2, 3, 4, 6, 7, 8, 9, 10, 11};
static const int32_t kBags[] = {0, -3, -6};

static void referenceAes() {
    // FIPS-197 appendix C.1-C.3.
    const auto pt = hex("00112233445566778899aabbccddeeff");
    const char *cts[] = {"69c4e0d86a7b0430d8cdb78070b4c55a", "dda97ca4864cdfe06eaf70a0ec0d7191", "8ea2b7ca516745bfeafc49904b496089"};
    for (int k = 0; k < 3; ++k) {
        const int len = 16 + 8 * k;
        std::vector<uint8_t> key(len);
        for (int i = 0; i < len; ++i) key[i] = (uint8_t)i;
        uint8_t rk[240], out[16], back[16];
        const int nr = refaes::expand(key.data(), len, rk);
        CHECK(nr == 10 + 2 * k);
        refaes::encrypt(rk, nr, pt.data(), out);
        CHECK(!memcmp(out, hex(cts[k]).data(), 16));
        refaes::decrypt(rk, nr, out, back);
        CHECK(!memcmp(back, pt.data(), 16));
    }
}

static void rfcVectors() {
    // RFC 3394 section 4.1-4.6 through the driver's wrap/unwrap over the libkern AES calls.
    struct V { const char *kek, *data, *wrapped; } vs[] = {
        {"000102030405060708090A0B0C0D0E0F", "00112233445566778899AABBCCDDEEFF", "1FA68B0A8112B447AEF34BD8FB5A7B829D3E862371D2CFE5"},
        {"000102030405060708090A0B0C0D0E0F1011121314151617", "00112233445566778899AABBCCDDEEFF",
         "96778B25AE6CA435F92B5B97C050AED2468AB8A17AD84E5D"},
        {"000102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F", "00112233445566778899AABBCCDDEEFF",
         "64E8C3F9CE0F5BA263E9777905818A2A93C8191E7D6E8AE7"},
        {"000102030405060708090A0B0C0D0E0F1011121314151617", "00112233445566778899AABBCCDDEEFF0001020304050607",
         "031D33264E15D33268F24EC260743EDCE1C6C7DDEE725A936BA814915C6762D2"},
        {"000102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F", "00112233445566778899AABBCCDDEEFF0001020304050607",
         "A8F9BC1612C68B3FF6E6F4FBE30E71E4769C8B80A32CB8958CD5D17D6B254DA1"},
        {"000102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F",
         "00112233445566778899AABBCCDDEEFF000102030405060708090A0B0C0D0E0F",
         "28C9F404C4B810F4CBCCB35CFB87F8263F5786E2D80ED326CBC7F0E71A99F43BFB988B9B7A02DD21"}};
    for (auto &v : vs) {
        const auto kek = hex(v.kek), data = hex(v.data), wrapped = hex(v.wrapped);
        std::vector<uint8_t> out(data.size() + 8), back(data.size());
        CHECK(rfc3394Wrap(kek.data(), kek.size(), data.data(), data.size(), out.data()) == kWrapOk && out == wrapped);
        CHECK(referenceWrap(kek.data(), kek.size(), data) == wrapped);
        CHECK(rfc3394Unwrap(kek.data(), kek.size(), wrapped.data(), wrapped.size(), back.data()) == kWrapOk && back == data);
        // Every single-bit change of the wrapped key fails the integrity check and leaves no plaintext behind.
        for (size_t bit = 0; bit < wrapped.size() * 8; ++bit) {
            auto bad = wrapped;
            bad[bit / 8] ^= (uint8_t)(1u << (bit % 8));
            std::vector<uint8_t> junk(data.size(), 0x77);
            CHECK(rfc3394Unwrap(kek.data(), kek.size(), bad.data(), bad.size(), junk.data()) == kWrapIntegrity);
            for (auto b : junk) CHECK(b == 0);
        }
    }
    // Lengths the wrap does not take.
    uint8_t kek[32] = {}, buf[600] = {}, out[608];
    for (size_t n : {0, 8, 15, 17, 33, 520, 600}) CHECK(rfc3394Wrap(kek, 32, buf, n, out) == kWrapBadLength);
    for (size_t m : {0, 8, 16, 23, 41, 528}) CHECK(rfc3394Unwrap(kek, 32, buf, m, out) == kWrapBadLength);
    CHECK(rfc3394Wrap(kek, 20, buf, 32, out) == kWrapBadLength);
    CHECK(selfTest());
}

static void roundTrips() {
    freshBoot();
    Service s;
    CHECK(static_cast<IOService *>(s.service)->keyOperationsAvailable && gKeybag.mode == 1 && gKeybag.lock);
    CHECK(tracesContaining("keybag: pagani-keybag=1") == 1);
    // Keys are drawn at the first use, not at start.
    CHECK(gKeybag.state == kKeysAbsent && randomCalls == 0);
    for (size_t i = 0; i < kClassSlots; ++i) for (size_t k = 0; k < kClassKeyBytes; ++k) CHECK(!gKeybag.keys[i][k]);
    const auto key = bytes(32, 0x40);
    std::map<int32_t, std::vector<uint8_t>> blobs;
    for (int32_t cls : kKeyed) {
        for (int32_t bag : kBags) {
            // securityd: 40-byte output, one scalar out (the actual class).
            Result w = call(s.client, 10, {i32(bag), i32(cls)}, key, 1, 40);
            CHECK(w.rc == kIOReturnSuccess && w.out.size() == 40 && w.scalars.size() == 1 && (int64_t)w.scalars[0] == cls);
            // The exact RFC 3394 output under this class's key: what libaks's own rfc3394_unwrap reads.
            const int slot = classSlot(cls);
            CHECK(slot >= 0 && w.out == referenceWrap(gKeybag.keys[slot], 32, key));
            if (blobs.count(cls)) CHECK(blobs[cls] == w.out);   // the bag aliases name one keybag
            blobs[cls] = w.out;
            // securityd: 32-byte output, no scalar out.
            for (int32_t other : kBags) {
                Result u = call(s.client, 11, {i32(other), i32(cls)}, w.out, 0, 32);
                CHECK(u.rc == kIOReturnSuccess && u.out == key && u.scalars.empty());
            }
            // Unwrapping as any other class fails the integrity check (libaks decode error: securityd drops the item).
            for (int32_t wrong : kKeyed) {
                if (wrong == cls) continue;
                CHECK(call(s.client, 11, {i32(bag), i32(wrong)}, w.out, 0, 32).rc == (IOReturn)0xe007c013);
            }
        }
    }
    CHECK(gKeybag.state == kKeysReady && randomCalls == 1);
    std::set<std::vector<uint8_t>> distinct;
    for (auto &b : blobs) distinct.insert(b.second);
    CHECK(distinct.size() == kClassSlots);   // ten different class keys
    // Every key length the wrap takes, inline and through descriptors (callers that hand more than 4096 bytes).
    for (size_t n = 16; n <= 512; n += 8) {
        const auto k = bytes(n, (uint8_t)n);
        Result w = call(s.client, 10, {i32(0), i32(7)}, k, 1, n + 8);
        CHECK(w.rc == kIOReturnSuccess && w.out.size() == n + 8);
        Result u = call(s.client, 11, {i32(0), i32(7)}, w.out, 0, n);
        CHECK(u.rc == kIOReturnSuccess && u.out == k);
        Result big = call(s.client, 11, {i32(0), i32(7)}, w.out, 0, 4096);   // a larger buffer is fine: the size says how much
        CHECK(big.rc == kIOReturnSuccess && big.out == k);
        IOMemoryDescriptor wd, ud;
        Result wdesc = call(s.client, 10, {i32(0), i32(7)}, k, 1, 0, &wd);
        CHECK(wdesc.rc == kIOReturnSuccess && wdesc.out == w.out && wd.prepares == 1 && wd.completes == 1 && !wd.prepared);
        Result udesc = call(s.client, 11, {i32(0), i32(7)}, w.out, 0, 0, &ud);
        CHECK(udesc.rc == kIOReturnSuccess && udesc.out == k && ud.prepares == 1 && ud.completes == 1);
    }
    // The wrap is deterministic per class key (RFC 3394 has no nonce): equal keys give equal blobs within a boot.
    CHECK(call(s.client, 10, {i32(0), i32(6)}, key, 1, 40).out == blobs[6]);
    // A zero-extended scalar (0xfffffffa) is the same bag as a sign-extended one.
    CHECK(call(s.client, 10, {0xfffffffaull, 6}, key, 1, 40).out == blobs[6]);
    // U lines: the keybag once, the first wrap and unwrap once, one usage summary (at 64 operations; about 440 ran).
    CHECK(tracesContaining("keybag: up:") == 1 && tracesContaining("keybag: first wrap by securityd: bag 0 class 1 32 -> 40 bytes") == 1 &&
          tracesContaining("keybag: first unwrap by securityd") == 1 && tracesContaining(" unwraps, ") == 1);
}

static void refusals() {
    freshBoot();
    Service s;
    const auto key = bytes(32, 3);
    Result good = call(s.client, 10, {0, 6}, key, 1, 40);
    CHECK(good.rc == kIOReturnSuccess);
    procName = "Preferences";
    // Class 12 (akpu) needs a passcode: no key, as on a phone without one (securityd: errSecNotAvailable).
    CHECK(call(s.client, 10, {0, 12}, key, 1, 40).rc == kIOReturnNotFound);
    CHECK(call(s.client, 11, {0, 12}, good.out, 0, 32).rc == kIOReturnNotFound);
    CHECK(tracesContaining("keybag: refused wrap for Preferences: bag 0 class 12") == 1);
    // Classes without a key, including the 0x20 generation variant securityd retries with and negative or huge values.
    for (int64_t cls : {0LL, 5LL, 13LL, 14LL, 31LL, 32LL, 38LL, -1LL, 0x7fffffffLL, -0x80000000LL}) {
        CHECK(call(s.client, 10, {0, (uint64_t)cls}, key, 1, 40).rc == kIOReturnNotFound);
        CHECK(call(s.client, 11, {0, (uint64_t)cls}, good.out, 0, 32).rc == kIOReturnNotFound);
    }
    CHECK(tracesContaining("class 0 ") == 1 && tracesContaining("refused") == 2);   // one line per kind, the first case
    // Bags that are not this keybag; scalars that are not an int32.
    for (int32_t bag : {1, 2, -1, -2, -4, -5, -7, 100, -1000})
        CHECK(call(s.client, 10, {i32(bag), 6}, key, 1, 40).rc == kIOReturnNotFound);
    for (uint64_t odd : {0x100000000ull, 0x80000000ffffffffull, 0xffffffff00000000ull})
        CHECK(call(s.client, 10, {odd, 6}, key, 1, 40).rc == kIOReturnBadArgument);
    CHECK(call(s.client, 10, {0, 0x100000006ull}, key, 1, 40).rc == kIOReturnBadArgument);
    // Scalar counts: wrap/unwrap take exactly two; wrap returns one.
    CHECK(call(s.client, 10, {}, key, 1, 40).rc == kIOReturnBadArgument);
    CHECK(call(s.client, 10, {0}, key, 1, 40).rc == kIOReturnBadArgument);
    CHECK(call(s.client, 10, {0, 6, 0}, key, 1, 40).rc == kIOReturnBadArgument);
    CHECK(call(s.client, 10, {0, 6}, key, 0, 40).rc == kIOReturnBadArgument);
    CHECK(call(s.client, 11, {0}, good.out, 0, 32).rc == kIOReturnBadArgument);
    // Key sizes: 16..512 in steps of 8.
    for (size_t n : {0, 1, 8, 15, 17, 31, 33, 39, 520, 1024, 4096}) {
        const IOReturn rc = call(s.client, 10, {0, 6}, bytes(n, 9), 1, 4096).rc;
        CHECK(rc == kIOReturnBadArgument);
    }
    // An out-of-line input (more than 4096 bytes) is refused without touching it.
    IOMemoryDescriptor bigIn;
    CHECK(call(s.client, 10, {0, 6}, {}, 1, 4096, nullptr, &bigIn).rc == kIOReturnBadArgument && !bigIn.prepares);
    // Outputs one byte too short.
    CHECK(call(s.client, 10, {0, 6}, key, 1, 39).rc == kIOReturnNoSpace);
    CHECK(call(s.client, 10, {0, 6}, key, 1, 0).rc == kIOReturnNoSpace);
    CHECK(call(s.client, 11, {0, 6}, good.out, 0, 31).rc == kIOReturnNoSpace);
    IOMemoryDescriptor small; small.length = 39;
    CHECK(call(s.client, 10, {0, 6}, key, 1, 0, &small).rc == kIOReturnNoSpace && !small.prepares);
    IOMemoryDescriptor unwired; unwired.prepareResult = kIOReturnVMError;
    CHECK(call(s.client, 10, {0, 6}, key, 1, 0, &unwired).rc == kIOReturnVMError && !unwired.completes);
    // An out-of-line output past 1 MiB is not wired: prepare() would wire all of it, however little is written.
    IOMemoryDescriptor huge; huge.length = (1u << 20) + 1;
    CHECK(call(s.client, 10, {0, 6}, key, 1, 0, &huge).rc == kIOReturnBadArgument && !huge.prepares);
    // Blobs that cannot be a wrapped key, tampered ones, truncated and extended ones: the decode error (securityd drops the item).
    // No blob at all is an argument error.
    CHECK(call(s.client, 11, {0, 6}, {}, 0, 600).rc == kIOReturnBadArgument);
    for (size_t m : {8, 16, 23, 41, 528}) CHECK(call(s.client, 11, {0, 6}, bytes(m, 5), 0, 600).rc == (IOReturn)0xe007c013);
    for (size_t bit = 0; bit < 320; ++bit) {
        auto bad = good.out;
        bad[bit / 8] ^= (uint8_t)(1u << (bit % 8));
        CHECK(call(s.client, 11, {0, 6}, bad, 0, 32).rc == (IOReturn)0xe007c013);
    }
    auto truncated = good.out; truncated.resize(32);
    CHECK(call(s.client, 11, {0, 6}, truncated, 0, 32).rc == (IOReturn)0xe007c013);
    auto extended = good.out; extended.insert(extended.end(), 8, 0);
    CHECK(call(s.client, 11, {0, 6}, extended, 0, 40).rc == (IOReturn)0xe007c013);
    CHECK(tracesContaining("unwrap (integrity)") == 1);
    // Every refusal reached Stage7Probe with its selector and code; the good wrap with 0.
    std::lock_guard<std::mutex> g(callsMutex);
    CHECK(calls.front().first == 10 && calls.front().second == 0);
    for (size_t i = 1; i < calls.size(); ++i) CHECK(calls[i].second != 0);
}

static void anotherBoot() {
    // A blob from an earlier boot (other class keys) never unwraps; the keys of a boot die with its service.
    freshBoot();
    std::vector<uint8_t> old;
    {
        Service s;
        old = call(s.client, 10, {0, 7}, bytes(32, 1), 1, 40).out;
        CHECK(old.size() == 40);
        CHECK(gKeybag.state == kKeysReady);
    }
    // Teardown: keys zeroed, lock freed, mode off.
    for (size_t i = 0; i < kClassSlots; ++i) for (size_t k = 0; k < kClassKeyBytes; ++k) CHECK(!gKeybag.keys[i][k]);
    CHECK(gKeybag.state == kKeysAbsent && !gKeybag.lock && !gKeybag.mode && locksAlive == 0);
    freshBoot();
    Service s;
    CHECK(call(s.client, 11, {0, 7}, old, 0, 32).rc == (IOReturn)0xe007c013);
    // The service's stop() wipes as well and switches the keybag off; the lock stays until the last reference goes (free).
    static_cast<IOService *>(s.service)->stop(nullptr);
    for (size_t i = 0; i < kClassSlots; ++i) for (size_t k = 0; k < kClassKeyBytes; ++k) CHECK(!gKeybag.keys[i][k]);
    CHECK(!gKeybag.mode && gKeybag.state == kKeysAbsent && gKeybag.lock && locksAlive == 1);
    CHECK(call(s.client, 10, {0, 7}, bytes(32, 1), 1, 40).rc == kIOReturnUnsupported);   // and leaves no key operations
}

static void configurationAndLock() {
    freshBoot();
    Service s;
    // libaks aks_get_configuration/aks_prederived_is_enabled: one scalar, a 32 KiB output, so a descriptor.
    for (int32_t bag : kBags) {
        IOMemoryDescriptor md;
        Result r = call(s.client, 31, {i32(bag)}, {}, 0, 0, &md);
        CHECK(r.rc == kIOReturnSuccess && md.prepares == 1 && md.completes == 1 && r.out.size() <= 0x8000 && r.scalars.empty());
        std::map<std::string, DerValue> config;
        CHECK(libaksDecode(r.out.data(), r.out.size(), config));
        CHECK(config.size() == 2 && derInteger(config["cf"]) == 0 && derInteger(config["mua"]) == 10);
        CHECK(!config.count("pdps") && !config.count("gp"));   // no prederived salt: aks_prederived_is_enabled is false
    }
    Result inl = call(s.client, 31, {0}, {}, 0, 4096);
    CHECK(inl.rc == kIOReturnSuccess && inl.out.size() > 10);
    CHECK(call(s.client, 31, {0}, {}, 0, 4).rc == kIOReturnNoSpace);
    IOMemoryDescriptor top; top.length = 1u << 20;            // the largest output buffer it still wires
    CHECK(call(s.client, 31, {0}, {}, 0, 0, &top).rc == kIOReturnSuccess && top.prepares == 1 && top.completes == 1);
    CHECK(call(s.client, 31, {}, {}, 0, 4096).rc == kIOReturnBadArgument);
    CHECK(call(s.client, 31, {0, 0}, {}, 0, 4096).rc == kIOReturnBadArgument);
    CHECK(call(s.client, 31, {i32(-1)}, {}, 0, 4096).rc == kIOReturnNotFound);   // e.g. AKSIdentityCopyGroupUUID's negated identity
    CHECK(tracesContaining("keybag: configuration answered") == 1);
    // aks_lock_device: selector 8, or 32 with its flag. Nothing to lock: the lock state stays "unlocked since boot".
    for (uint32_t sel : {8u, 32u}) {
        for (int32_t bag : kBags) {
            Result r = call(s.client, sel, {i32(bag)}, {}, 0, 0);
            CHECK(r.rc == kIOReturnSuccess && r.scalars.empty());
        }
        CHECK(call(s.client, sel, {i32(7)}, {}, 0, 0).rc == kIOReturnNotFound);
        CHECK(call(s.client, sel, {}, {}, 0, 0).rc == kIOReturnBadArgument);
        Result st = call(s.client, 7, {0}, {}, 1, 0);
        CHECK(st.rc == kIOReturnSuccess && st.scalars[0] == 4);
        // Class A keys stay usable after a lock request (no passcode).
        Result w = call(s.client, 10, {0, 6}, bytes(32, 8), 1, 40);
        CHECK(w.rc == kIOReturnSuccess && call(s.client, 11, {0, 6}, w.out, 0, 32).rc == kIOReturnSuccess);
    }
    CHECK(tracesContaining("keybag: lock request answered") == 1);
}

static void modes() {
    // pagani-keybag=0: every keybag selector is unsupported, as before the keybag; no lock, no key material, no AES.
    freshBoot();
    {
        Service s(0);
        CHECK(!static_cast<IOService *>(s.service)->keyOperationsAvailable && !gKeybag.lock && locksAlive == 0);
        IOMemoryDescriptor md;
        for (uint32_t sel : {8u, 10u, 11u, 31u, 32u}) CHECK(call(s.client, sel, {0, 6}, bytes(32, 1), 1, 64).rc == kIOReturnUnsupported);
        CHECK(call(s.client, 31, {0}, {}, 0, 0, &md).rc == kIOReturnUnsupported && !md.prepares);
        CHECK(randomCalls == 0 && aesKeySchedules == 0 && tracesContaining("pagani-keybag=0: state answers only") == 1);
    }
    // Values above 2 count as 1.
    freshBoot();
    { Service s(7); CHECK(gKeybag.mode == 1); }
    // pagani-keybag=2: the keychain classes need com.apple.keystore.access-keychain-keys, asked once per client.
    freshBoot();
    {
        Service s(2);
        entitlementAnswer = nullptr;
        entitlementQueries = 0;
        for (int32_t cls : {6, 7, 8, 9, 10, 11}) CHECK(call(s.client, 10, {0, (uint64_t)cls}, bytes(32, 2), 1, 40).rc == kIOReturnNotPrivileged);
        CHECK(entitlementQueries == 1);
        CHECK(call(s.client, 10, {0, 12}, bytes(32, 2), 1, 40).rc == kIOReturnNotFound);   // no key at all comes first
        for (int32_t cls : {1, 2, 3, 4}) CHECK(call(s.client, 10, {0, (uint64_t)cls}, bytes(32, 2), 1, 40).rc == kIOReturnSuccess);
        entitlementAnswer = kOSBooleanTrue;
        IOUserClient *entitled = s.newClient();
        Result w = call(entitled, 10, {0, 6}, bytes(32, 2), 1, 40);
        CHECK(w.rc == kIOReturnSuccess && call(entitled, 11, {0, 6}, w.out, 0, 32).rc == kIOReturnSuccess);
        CHECK(call(s.client, 11, {0, 6}, w.out, 0, 32).rc == kIOReturnNotPrivileged);   // the first client keeps its answer
        CHECK(entitlementQueries == 2 && tracesContaining("refused wrap for securityd: bag 0 class 6 in 0 out 0 rc 0xe00002c1") == 1);
        entitled->release();
        entitlementAnswer = kOSBooleanFalse;
        IOUserClient *falseAnswer = s.newClient();
        CHECK(call(falseAnswer, 10, {0, 6}, bytes(32, 2), 1, 40).rc == kIOReturnNotPrivileged);
        falseAnswer->release();
    }
    // Default mode never asks for entitlements.
    freshBoot();
    entitlementQueries = 0;
    { Service s; CHECK(call(s.client, 10, {0, 6}, bytes(32, 2), 1, 40).rc == kIOReturnSuccess && entitlementQueries == 0); }
    // No lock: the keybag stays off for the boot.
    freshBoot();
    lockAllocFails = true;
    {
        Service s;
        CHECK(!gKeybag.mode && !static_cast<IOService *>(s.service)->keyOperationsAvailable);
        CHECK(call(s.client, 10, {0, 6}, bytes(32, 2), 1, 40).rc == kIOReturnUnsupported && tracesContaining("keybag: off: no lock") == 1);
    }
}

static void guards() {
    // No corecrypto AES registered: refused with kIOReturnNotReady, never a call into the missing AES, once per boot.
    freshBoot();
    {
        Service s;
        g_crypto_funcs = nullptr;
        CHECK(call(s.client, 10, {0, 6}, bytes(32, 1), 1, 40).rc == kIOReturnNotReady);
        g_crypto_funcs = &functions;   // a later registration does not revive a failed boot
        CHECK(call(s.client, 11, {0, 6}, bytes(40, 1), 0, 32).rc == kIOReturnNotReady);
        CHECK(gKeybag.state == kKeysFailed && randomCalls == 0 && aesKeySchedules == 0);
        CHECK(tracesContaining("keybag: off for this boot: no kernel AES") == 1 && tracesContaining("wrap (no keys)") == 1);
    }
    // A corecrypto context larger than libkern's: aes_encrypt_key would panic, so it is never called.
    for (int which = 0; which < 2; ++which) {
        freshBoot();
        Service s;
        (which ? decryptMode : encryptMode).size = 4096;
        CHECK(call(s.client, 10, {0, 6}, bytes(32, 1), 1, 40).rc == kIOReturnNotReady && aesKeySchedules == 0);
    }
    freshBoot();
    { Service s; encryptMode.block_size = 8; CHECK(call(s.client, 10, {0, 6}, bytes(32, 1), 1, 40).rc == kIOReturnNotReady); }
    // A self-test failure (wrong AES output, failing key schedule, failing block): no keys, no wrap.
    for (int mode : {1, 2, 3}) {
        freshBoot();
        Service s;
        aesMode = mode;
        CHECK(call(s.client, 10, {0, 6}, bytes(32, 1), 1, 40).rc == kIOReturnNotReady && randomCalls == 0);
        CHECK(tracesContaining("self-test failed") == 1);
        for (size_t i = 0; i < kClassSlots; ++i) for (size_t k = 0; k < kClassKeyBytes; ++k) CHECK(!gKeybag.keys[i][k]);
    }
    // A random generator that returns zeros or repeats itself.
    for (int mode : {1, 2}) {
        freshBoot();
        Service s;
        randomMode = mode;
        CHECK(call(s.client, 10, {0, 6}, bytes(32, 1), 1, 40).rc == kIOReturnNotReady);
        CHECK(tracesContaining(mode == 1 ? "zero key" : "repeated a key") == 1);
        for (size_t i = 0; i < kClassSlots; ++i) for (size_t k = 0; k < kClassKeyBytes; ++k) CHECK(!gKeybag.keys[i][k]);
    }
    // An AES failure after the keys exist: kIOReturnError, the caller's buffer untouched (checked by call()).
    freshBoot();
    {
        Service s;
        Result w = call(s.client, 10, {0, 6}, bytes(32, 1), 1, 40);
        CHECK(w.rc == kIOReturnSuccess);
        aesMode = 2;
        CHECK(call(s.client, 10, {0, 6}, bytes(32, 1), 1, 40).rc == kIOReturnError);
        CHECK(call(s.client, 11, {0, 6}, w.out, 0, 32).rc == kIOReturnError);
        aesMode = 0;
        CHECK(call(s.client, 11, {0, 6}, w.out, 0, 32).rc == kIOReturnSuccess);
    }
}

static void concurrentFirstUse() {
    freshBoot();
    Service s;
    std::vector<IOUserClient *> clients;
    for (int i = 0; i < 8; ++i) clients.push_back(s.newClient());
    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < 200; ++i) {
                const int32_t cls = kKeyed[(t + i) % kClassSlots];
                const auto key = bytes(32, (uint8_t)(t * 31 + i));
                Result w = call(clients[t], 10, {0, (uint64_t)cls}, key, 1, 40);
                if (w.rc != kIOReturnSuccess) { ++failures; continue; }
                Result u = call(clients[t], 11, {i32(-6), (uint64_t)cls}, w.out, 0, 32);
                if (u.rc != kIOReturnSuccess || u.out != key) ++failures;
            }
        });
    }
    for (auto &th : threads) th.join();
    CHECK(failures == 0 && randomCalls == 1 && tracesContaining("keybag: up:") == 1);
    CHECK(gKeybag.wraps == 1600 && gKeybag.unwraps == 1600 && gKeybag.refusals == 0);
    // The summaries at 64 and 1024 are best effort under concurrency (two threads may pass a mark together).
    CHECK(tracesContaining(" unwraps, ") <= 2 && tracesContaining("keybag: first wrap by") == 1);
    for (auto *c : clients) c->release();
}

int main() {
    referenceAes();
    rfcVectors();
    roundTrips();
    refusals();
    anotherBoot();
    configurationAndLock();
    modes();
    guards();
    concurrentFirstUse();
    freshBoot();
    CHECK(locksAlive == 0);
    printf("PASS: RFC 3394 vectors 4.1-4.6 and 1920 tampered blobs; libaks 21H461 wrap/unwrap/configuration/lock contracts; round "
           "trips for classes 1-4 and 6-11 over bags 0/-3/-6 and 16..512-byte keys (inline and descriptors); class 12 and unknown "
           "classes/bags, wrong sizes, short outputs, foreign blobs refused; no-AES, oversize-context, self-test and random guards; "
           "zeroized teardown; pagani-keybag=0/2; concurrent first use\n");
    return 0;
}
