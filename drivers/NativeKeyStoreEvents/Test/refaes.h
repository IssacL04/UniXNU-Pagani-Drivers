// SPDX-License-Identifier: BSD-2-Clause-Patent
// A plain FIPS-197 AES (128/192/256) for the host tests: the stand-in for the kernel's corecrypto AES behind libkern's
// aes_encrypt/aes_decrypt, and the independent half of the RFC 3394 cross-check. Checked against FIPS-197 appendix C.
#pragma once
#include <cstdint>
#include <cstring>

namespace refaes {
inline uint8_t xtime(uint8_t x) { return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1b : 0)); }
inline uint8_t mul(uint8_t a, uint8_t b) {
    uint8_t r = 0;
    while (b) { if (b & 1) r ^= a; a = xtime(a); b >>= 1; }
    return r;
}
struct Tables {
    uint8_t sbox[256], inv[256];
    Tables() {
        // The multiplicative inverse walks with the generator 3, then the affine map (FIPS-197 5.1.1).
        uint8_t p = 1, q = 1;
        do {
            p = (uint8_t)(p ^ (uint8_t)(p << 1) ^ ((p & 0x80) ? 0x1b : 0));
            q ^= (uint8_t)(q << 1); q ^= (uint8_t)(q << 2); q ^= (uint8_t)(q << 4);
            if (q & 0x80) q ^= 0x09;
            auto rotl = [](uint8_t x, int s) { return (uint8_t)((x << s) | (x >> (8 - s))); };
            sbox[p] = (uint8_t)(q ^ rotl(q, 1) ^ rotl(q, 2) ^ rotl(q, 3) ^ rotl(q, 4) ^ 0x63);
        } while (p != 1);
        sbox[0] = 0x63;
        for (int i = 0; i < 256; ++i) inv[sbox[i]] = (uint8_t)i;
    }
};
inline const Tables &tables() { static const Tables t; return t; }
// Returns the round count (10, 12, 14), or -1 for another key length. rk: 240 bytes.
inline int expand(const uint8_t *key, int keyBytes, uint8_t *rk) {
    const int nk = keyBytes / 4;
    if (keyBytes != 16 && keyBytes != 24 && keyBytes != 32) return -1;
    const int nr = nk + 6, words = 4 * (nr + 1);
    const uint8_t *s = tables().sbox;
    memcpy(rk, key, (size_t)keyBytes);
    uint8_t rcon = 1;
    for (int i = nk; i < words; ++i) {
        uint8_t t[4];
        memcpy(t, rk + 4 * (i - 1), 4);
        if (i % nk == 0) {
            const uint8_t first = t[0];
            t[0] = (uint8_t)(s[t[1]] ^ rcon); t[1] = s[t[2]]; t[2] = s[t[3]]; t[3] = s[first];
            rcon = xtime(rcon);
        } else if (nk > 6 && i % nk == 4) {
            for (auto &b : t) b = s[b];
        }
        for (int k = 0; k < 4; ++k) rk[4 * i + k] = (uint8_t)(rk[4 * (i - nk) + k] ^ t[k]);
    }
    return nr;
}
inline void addRoundKey(uint8_t st[16], const uint8_t *rk, int round) { for (int i = 0; i < 16; ++i) st[i] ^= rk[16 * round + i]; }
inline void encrypt(const uint8_t *rk, int nr, const uint8_t in[16], uint8_t out[16]) {
    const uint8_t *s = tables().sbox;
    uint8_t st[16], t[16];
    memcpy(st, in, 16);
    addRoundKey(st, rk, 0);
    for (int round = 1; round <= nr; ++round) {
        for (auto &b : st) b = s[b];
        for (int c = 0; c < 4; ++c) for (int r = 0; r < 4; ++r) t[r + 4 * c] = st[r + 4 * ((c + r) % 4)];
        if (round != nr) {
            for (int c = 0; c < 4; ++c) {
                const uint8_t *a = t + 4 * c;
                uint8_t b0 = (uint8_t)(mul(a[0], 2) ^ mul(a[1], 3) ^ a[2] ^ a[3]), b1 = (uint8_t)(a[0] ^ mul(a[1], 2) ^ mul(a[2], 3) ^ a[3]);
                uint8_t b2 = (uint8_t)(a[0] ^ a[1] ^ mul(a[2], 2) ^ mul(a[3], 3)), b3 = (uint8_t)(mul(a[0], 3) ^ a[1] ^ a[2] ^ mul(a[3], 2));
                st[4 * c] = b0; st[4 * c + 1] = b1; st[4 * c + 2] = b2; st[4 * c + 3] = b3;
            }
        } else {
            memcpy(st, t, 16);
        }
        addRoundKey(st, rk, round);
    }
    memcpy(out, st, 16);
}
inline void decrypt(const uint8_t *rk, int nr, const uint8_t in[16], uint8_t out[16]) {
    const uint8_t *inv = tables().inv;
    uint8_t st[16], t[16];
    memcpy(st, in, 16);
    addRoundKey(st, rk, nr);
    for (int round = nr - 1; round >= 0; --round) {
        for (int c = 0; c < 4; ++c) for (int r = 0; r < 4; ++r) t[r + 4 * ((c + r) % 4)] = st[r + 4 * c];
        for (int i = 0; i < 16; ++i) st[i] = inv[t[i]];
        addRoundKey(st, rk, round);
        if (round) {
            for (int c = 0; c < 4; ++c) {
                uint8_t *a = st + 4 * c;
                uint8_t b0 = (uint8_t)(mul(a[0], 14) ^ mul(a[1], 11) ^ mul(a[2], 13) ^ mul(a[3], 9));
                uint8_t b1 = (uint8_t)(mul(a[0], 9) ^ mul(a[1], 14) ^ mul(a[2], 11) ^ mul(a[3], 13));
                uint8_t b2 = (uint8_t)(mul(a[0], 13) ^ mul(a[1], 9) ^ mul(a[2], 14) ^ mul(a[3], 11));
                uint8_t b3 = (uint8_t)(mul(a[0], 11) ^ mul(a[1], 13) ^ mul(a[2], 9) ^ mul(a[3], 14));
                a[0] = b0; a[1] = b1; a[2] = b2; a[3] = b3;
            }
        }
    }
    memcpy(out, st, 16);
}
}  // namespace refaes
