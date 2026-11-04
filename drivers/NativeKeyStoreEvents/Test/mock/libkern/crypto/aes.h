#pragma once
// Host stand-in for libkern's AES (libkern/crypto/corecrypto_aes.c): key lengths in bytes, aes_encrypt/aes_decrypt are one ECB block.
// Backed by Test/refaes.h, an independent FIPS-197 implementation; the test can make it fail or corrupt its output.
#include <cstddef>
#include <cstdint>
#define aes_good 0
#define aes_error -1
struct aes_encrypt_ctx { uint8_t roundKeys[240]; int rounds; uint32_t magic; };
struct aes_decrypt_ctx { uint8_t roundKeys[240]; int rounds; uint32_t magic; };
int aes_encrypt_key(const unsigned char *key, int key_len, aes_encrypt_ctx cx[1]);
int aes_encrypt(const unsigned char *in, unsigned char *out, aes_encrypt_ctx cx[1]);
int aes_decrypt_key(const unsigned char *key, int key_len, aes_decrypt_ctx cx[1]);
int aes_decrypt(const unsigned char *in, unsigned char *out, aes_decrypt_ctx cx[1]);
