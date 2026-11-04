#pragma once
// Host stand-in for g_crypto_funcs (libkern/crypto/register_crypto.h): only the two AES CBC mode descriptors the driver checks.
#include <cstddef>
struct ccmode_cbc { size_t size; size_t block_size; };
struct crypto_functions { const ccmode_cbc *ccaes_cbc_encrypt; const ccmode_cbc *ccaes_cbc_decrypt; };
typedef crypto_functions *crypto_functions_t;
extern crypto_functions_t g_crypto_funcs;
