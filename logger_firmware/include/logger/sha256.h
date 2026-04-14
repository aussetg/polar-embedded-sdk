#ifndef LOGGER_FIRMWARE_SHA256_H
#define LOGGER_FIRMWARE_SHA256_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pico/sha256.h"

#define LOGGER_SHA256_BYTES SHA256_RESULT_BYTES
#define LOGGER_SHA256_HEX_LEN 64u

typedef struct {
  pico_sha256_state_t state;
  bool active;
} logger_sha256_t;

void logger_sha256_init(logger_sha256_t *ctx);
void logger_sha256_update(logger_sha256_t *ctx, const void *data, size_t len);
void logger_sha256_final(logger_sha256_t *ctx,
                         uint8_t out[LOGGER_SHA256_BYTES]);
void logger_sha256_final_hex(logger_sha256_t *ctx,
                             char out_hex[LOGGER_SHA256_HEX_LEN + 1]);

#endif