// AES-128 single-block encryption using mbedTLS.
//
// BTstack calls btstack_aes128_calc() when HAVE_AES128 is defined,
// using it for CMAC and other BLE Security Manager operations.
// By routing through mbedTLS (already linked for TLS), we avoid
// pulling in BTstack's bundled rijndael implementation (~2.7 KiB).

#include "btstack_crypto.h"

#include "mbedtls/aes.h"
#include <stdint.h>
#include <string.h>

void btstack_aes128_calc(const uint8_t *key, const uint8_t *plaintext,
                         uint8_t *ciphertext) {
  mbedtls_aes_context ctx;
  mbedtls_aes_init(&ctx);
  if (mbedtls_aes_setkey_enc(&ctx, key, 128) != 0 ||
      mbedtls_aes_crypt_ecb(&ctx, MBEDTLS_AES_ENCRYPT, plaintext, ciphertext) !=
          0) {
    memset(ciphertext, 0, 16u);
  }
  mbedtls_aes_free(&ctx);
}
