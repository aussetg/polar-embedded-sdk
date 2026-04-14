#include "logger/sha256.h"

#include <string.h>

#include "pico/error.h"

void logger_sha256_init(logger_sha256_t *ctx) {
  memset(ctx, 0, sizeof(*ctx));
  const int rc =
      pico_sha256_start_blocking(&ctx->state, SHA256_BIG_ENDIAN, false);
  hard_assert(rc == PICO_OK);
  ctx->active = true;
}

void logger_sha256_update(logger_sha256_t *ctx, const void *data, size_t len) {
  hard_assert(ctx != NULL);
  hard_assert(ctx->active);
  pico_sha256_update_blocking(&ctx->state, (const uint8_t *)data, len);
}

void logger_sha256_final(logger_sha256_t *ctx,
                         uint8_t out[LOGGER_SHA256_BYTES]) {
  hard_assert(ctx != NULL);
  hard_assert(out != NULL);
  hard_assert(ctx->active);

  sha256_result_t result;
  pico_sha256_finish(&ctx->state, &result);
  memcpy(out, result.bytes, LOGGER_SHA256_BYTES);
  ctx->active = false;
}

void logger_sha256_final_hex(logger_sha256_t *ctx,
                             char out_hex[LOGGER_SHA256_HEX_LEN + 1]) {
  static const char hex[] = "0123456789abcdef";
  uint8_t digest[LOGGER_SHA256_BYTES];
  logger_sha256_final(ctx, digest);
  for (size_t i = 0u; i < LOGGER_SHA256_BYTES; ++i) {
    out_hex[i * 2u] = hex[(digest[i] >> 4) & 0x0f];
    out_hex[(i * 2u) + 1u] = hex[digest[i] & 0x0f];
  }
  out_hex[LOGGER_SHA256_HEX_LEN] = '\0';
}
