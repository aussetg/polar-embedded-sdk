#include "logger/sha256.h"

#include <string.h>

static uint32_t logger_rotr32(uint32_t value, unsigned shift) {
    return (value >> shift) | (value << (32u - shift));
}

static uint32_t logger_be32(const uint8_t bytes[4]) {
    return ((uint32_t)bytes[0] << 24) |
           ((uint32_t)bytes[1] << 16) |
           ((uint32_t)bytes[2] << 8) |
           (uint32_t)bytes[3];
}

static void logger_put_be32(uint8_t bytes[4], uint32_t value) {
    bytes[0] = (uint8_t)(value >> 24);
    bytes[1] = (uint8_t)(value >> 16);
    bytes[2] = (uint8_t)(value >> 8);
    bytes[3] = (uint8_t)value;
}

static const uint32_t logger_sha256_k[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

static void logger_sha256_transform(logger_sha256_t *ctx, const uint8_t block[64]) {
    uint32_t w[64];
    for (size_t i = 0u; i < 16u; ++i) {
        w[i] = logger_be32(block + (i * 4u));
    }
    for (size_t i = 16u; i < 64u; ++i) {
        const uint32_t s0 = logger_rotr32(w[i - 15u], 7u) ^ logger_rotr32(w[i - 15u], 18u) ^ (w[i - 15u] >> 3u);
        const uint32_t s1 = logger_rotr32(w[i - 2u], 17u) ^ logger_rotr32(w[i - 2u], 19u) ^ (w[i - 2u] >> 10u);
        w[i] = w[i - 16u] + s0 + w[i - 7u] + s1;
    }

    uint32_t a = ctx->state[0];
    uint32_t b = ctx->state[1];
    uint32_t c = ctx->state[2];
    uint32_t d = ctx->state[3];
    uint32_t e = ctx->state[4];
    uint32_t f = ctx->state[5];
    uint32_t g = ctx->state[6];
    uint32_t h = ctx->state[7];

    for (size_t i = 0u; i < 64u; ++i) {
        const uint32_t s1 = logger_rotr32(e, 6u) ^ logger_rotr32(e, 11u) ^ logger_rotr32(e, 25u);
        const uint32_t ch = (e & f) ^ ((~e) & g);
        const uint32_t temp1 = h + s1 + ch + logger_sha256_k[i] + w[i];
        const uint32_t s0 = logger_rotr32(a, 2u) ^ logger_rotr32(a, 13u) ^ logger_rotr32(a, 22u);
        const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t temp2 = s0 + maj;

        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
    ctx->state[5] += f;
    ctx->state[6] += g;
    ctx->state[7] += h;
}

void logger_sha256_init(logger_sha256_t *ctx) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->state[0] = 0x6a09e667u;
    ctx->state[1] = 0xbb67ae85u;
    ctx->state[2] = 0x3c6ef372u;
    ctx->state[3] = 0xa54ff53au;
    ctx->state[4] = 0x510e527fu;
    ctx->state[5] = 0x9b05688cu;
    ctx->state[6] = 0x1f83d9abu;
    ctx->state[7] = 0x5be0cd19u;
}

void logger_sha256_update(logger_sha256_t *ctx, const void *data, size_t len) {
    const uint8_t *bytes = (const uint8_t *)data;
    ctx->bit_count += (uint64_t)len * 8u;

    while (len > 0u) {
        const size_t chunk = (len < (64u - ctx->block_len)) ? len : (64u - ctx->block_len);
        memcpy(ctx->block + ctx->block_len, bytes, chunk);
        ctx->block_len += chunk;
        bytes += chunk;
        len -= chunk;

        if (ctx->block_len == 64u) {
            logger_sha256_transform(ctx, ctx->block);
            ctx->block_len = 0u;
        }
    }
}

void logger_sha256_final(logger_sha256_t *ctx, uint8_t out[LOGGER_SHA256_BYTES]) {
    ctx->block[ctx->block_len++] = 0x80u;
    if (ctx->block_len > 56u) {
        while (ctx->block_len < 64u) {
            ctx->block[ctx->block_len++] = 0u;
        }
        logger_sha256_transform(ctx, ctx->block);
        ctx->block_len = 0u;
    }

    while (ctx->block_len < 56u) {
        ctx->block[ctx->block_len++] = 0u;
    }

    for (int i = 7; i >= 0; --i) {
        ctx->block[ctx->block_len++] = (uint8_t)(ctx->bit_count >> (i * 8));
    }
    logger_sha256_transform(ctx, ctx->block);

    for (size_t i = 0u; i < 8u; ++i) {
        logger_put_be32(out + (i * 4u), ctx->state[i]);
    }
}

void logger_sha256_final_hex(logger_sha256_t *ctx, char out_hex[LOGGER_SHA256_HEX_LEN + 1]) {
    static const char hex[] = "0123456789abcdef";
    uint8_t digest[LOGGER_SHA256_BYTES];
    logger_sha256_final(ctx, digest);
    for (size_t i = 0u; i < LOGGER_SHA256_BYTES; ++i) {
        out_hex[i * 2u] = hex[(digest[i] >> 4) & 0x0f];
        out_hex[(i * 2u) + 1u] = hex[digest[i] & 0x0f];
    }
    out_hex[LOGGER_SHA256_HEX_LEN] = '\0';
}