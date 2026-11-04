# NativeKeyStoreEvents

AppleKeyStore state client and volatile software keybag. Handles keybag and lock-state events for the sheng port and, on the 13T,
the keychain's key wrapping.

Current state: used with `sheng-aks=1` (and `offline-key-events=1`); Stage7Probe registers it only when `/chosen ephemeral-storage`
is 1, i.e. the Data volume is a RAM disk made fresh from the seed every boot.

Linked into XNU by `unixnu build kernel` (compiled by `scripts/build-stage7-driver.py NativeKeyStoreEvents` into the Stage 7
display archive).

## Selectors

| Selector | libaks 21H461 | Answer |
| --- | --- | --- |
| 0, 4, 5, 9, 12, 13, 26, 27, 55, 101, 102 | handshake, bag/assertion bookkeeping | success, no outputs |
| 7 | `aks_get_lock_state` | 4: unlocked since boot |
| 14 | `aks_get_system` | system bag handle 0 |
| 17, 35 | `aks_get_(extended_)device_state` | DER state (no passcode, unlocked) |
| 10 | `aks_wrap_key` | RFC 3394 AES-256 key wrap under the class key: len + 8 bytes, scalar out = class |
| 11 | `aks_unwrap_key` | the key back, or `0xe007c013` (decode error) when the integrity check fails |
| 31 | `aks_get_configuration`, `aks_prederived_is_enabled` | DER `{cf 0, mua 10}`: no passcode, no prederived secret |
| 8, 32 | `aks_lock_device` | success, nothing to lock without a passcode |
| any other | | `kIOReturnUnsupported` (`0xe00002c7`) |

Selectors 8, 10, 11, 31 and 32 belong to the volatile keybag and answer only while `pagani-keybag` is not 0.

## Volatile keybag (`pagani-keybag`)

- `pagani-keybag` absent or 1: on. `0`: off (the table above without 8/10/11/31/32, exactly the behaviour before the keybag).
  `2`: on, and the keychain classes 6-12 also need the caller's `com.apple.keystore.access-keychain-keys` entitlement (securityd has
  it); not yet tried on hardware.
- Keys: one 256-bit key per class with a key, drawn from `read_random` (the kernel PRNG) at the first wrap/unwrap of the boot, after
  checking that corecrypto's AES is registered and fits libkern's context and an RFC 3394 self-test (section 4.6) through the kernel
  AES. They never leave the kernel and are zeroed when the service stops or is freed.
- Classes with keys: 1-4 (A, B, C, D) and 6-11 (ak, ck, dk, aku, cku, dku). Class 12 (akpu) needs a passcode and every other class
  has no key: `kIOReturnNotFound`, which securityd reports as `errSecNotAvailable` ("No key available for class").
- Bags: 0 (device), -3 (MobileKeyBag's session alias) and -6 (securityd's system keychain handle) name the one keybag.
- Sizes: keys of 16..512 bytes in steps of 8; anything else `kIOReturnBadArgument`. Outputs too small: `kIOReturnNoSpace`.
- Out-of-line outputs (callers asking for more than 4096 bytes, e.g. the 32 KiB configuration buffer) are prepared before they are
  written and completed after; one larger than 1 MiB is refused (`kIOReturnBadArgument`) before anything is wired.
- Ramoops: failures as Stage7Probe's R lines (`key operation unavailable` with the selector) and, once per kind, U lines
  `keybag: ...` (mode, keys up, first wrap/unwrap with the process, refusals with process/bag/class/sizes, usage at 64/1024/16384).

What it is not: secure storage. There is no passcode, no hardware-bound key and nothing that protects data from code running in the
kernel or from anyone who can read the RAM (the RAM disk holds all data in the clear). It exists so that the keychain works for one
boot. It must not be combined with a persistent Data volume: per-boot keys make every keychain item of an earlier boot undecryptable
(securityd then drops those items).

## Tests

```
python3 -B test_closed_boundary.py   # lifecycle and the selector table (Test/boundary.cpp)
python3 -B test_volatile_keybag.py   # the keybag (Test/keybag.cpp)
```

Both compile the real `NativeKeyStoreEvents.cpp` against the host doubles in `Test/mock` and `Test/host.h` (a FIPS-197 reference
AES behind the libkern AES calls) with AddressSanitizer and UBSan.
