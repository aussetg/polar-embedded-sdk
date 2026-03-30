#ifndef LOGGER_FIRMWARE_SHA256_H
#define LOGGER_FIRMWARE_SHA256_H

#include <stddef.h>
#include <stdint.h>

#define LOGGER_SHA256_BYTES 32u
#define LOGGER_SHA256_HEX_LEN 64u

typedef struct {
    uint32_t state[8];
    uint64_t bit_count;
    uint8_t block[64];
    size_t block_len;
} logger_sha256_t;

void logger_sha256_init(logger_sha256_t *ctx);
void logger_sha256_update(logger_sha256_t *ctx, const void *data, size_t len);
void logger_sha256_final(logger_sha256_t *ctx, uint8_t out[LOGGER_SHA256_BYTES]);
void logger_sha256_final_hex(logger_sha256_t *ctx, char out_hex[LOGGER_SHA256_HEX_LEN + 1]);

#endif