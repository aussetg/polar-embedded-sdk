// Project-local Mbed TLS configuration for logger_firmware.
//
// We intentionally use a narrow TLS-client-oriented feature set instead of the
// full upstream default config. That keeps the dependency surface small and
// avoids pulling in unsupported POSIX/PSA storage pieces on bare metal.

#ifndef LOGGER_FIRMWARE_MBEDTLS_CONFIG_H
#define LOGGER_FIRMWARE_MBEDTLS_CONFIG_H

// Work around mbedtls sources that expect INT_MAX via indirect includes.
#include <limits.h>

#define MBEDTLS_ALLOW_PRIVATE_ACCESS
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_ENTROPY_HARDWARE_ALT
#define MBEDTLS_HAVE_TIME
#define MBEDTLS_HAVE_TIME_DATE
#define MBEDTLS_PLATFORM_MS_TIME_ALT
#define MBEDTLS_PLATFORM_MEMORY

#define MBEDTLS_AES_C
#define MBEDTLS_AES_FEWER_TABLES
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_BASE64_C
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_CIPHER_C
#define MBEDTLS_CIPHER_MODE_CBC
#define MBEDTLS_CTR_DRBG_C
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECDSA_C
#define MBEDTLS_ECP_C
#define MBEDTLS_ENTROPY_C
#define MBEDTLS_ERROR_C
#define MBEDTLS_GCM_C
#define MBEDTLS_MD_C
#define MBEDTLS_OID_C
#define MBEDTLS_PEM_PARSE_C
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C
#define MBEDTLS_PKCS1_V15
#define MBEDTLS_PLATFORM_C
#define MBEDTLS_RSA_C
#define MBEDTLS_SHA1_C

// SHA-256: software implementation for mbedtls / TLS.
//
// The RP2350 has a hardware SHA-256 engine which we *do* use directly
// (see logger/sha256.h → pico_sha256_*).  However, mbedtls TLS 1.2
// requires mbedtls_sha256_clone() for two purposes:
//
//   1) Constant-time HMAC via mbedtls_ct_hmac() in ssl_msg.c — clones
//      the running hash per byte to avoid timing side-channels.
//   2) Handshake transcript hash for Finished messages in ssl_tls.c.
//
// The RP2350 SHA-256 hardware is a single-instance streaming engine:
//   • The intermediate hash state (H0–H7) lives in internal registers
//     that cannot be read or written by software.
//   • Only the final result (SUM0–SUM7) is readable, and only after a
//     complete 64-byte block is processed.
//   • There is no way to snapshot or restore mid-computation state.
//
// pico-sdk ships sha256_alt.h + an ALT implementation in pico_mbedtls.c
// but omits mbedtls_sha256_clone() — precisely because it *cannot* be
// implemented on this hardware.  Enabling MBEDTLS_SHA256_ALT causes a
// link-time undefined reference to mbedtls_sha256_clone as soon as TLS
// is used.
//
// We keep MBEDTLS_SHA256_SMALLER (compact software lookup tables) for
// the TLS path, while logger_sha256_* routes through the hardware
// accelerator for data-integrity hashes (upload bundles, etc.).

#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA256_SMALLER

#define MBEDTLS_SHA384_C
#define MBEDTLS_SHA512_C
#define MBEDTLS_SSL_CLI_C
#define MBEDTLS_SSL_PROTO_TLS1_2
#define MBEDTLS_SSL_SERVER_NAME_INDICATION
#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_X509_CRT_PARSE_C
#define MBEDTLS_X509_USE_C

#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECP_DP_SECP384R1_ENABLED
#define MBEDTLS_ECP_DP_SECP521R1_ENABLED
#define MBEDTLS_ECP_DP_SECP256K1_ENABLED
#define MBEDTLS_ECP_DP_CURVE25519_ENABLED

#define MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_RSA_ENABLED

/*
 * TLS record buffers are intentionally smaller than the upstream/default
 * mbedTLS sizes to fit the RP2350 + lwIP SRAM heap budget.
 *
 * Important: these buffers still come from the lwIP heap in SRAM via
 * altcp_tls_mbedtls_mem.c (mem_malloc/mem_calloc), not from our PSRAM-backed
 * upload/queue workspaces. We reduced them while fixing TLS OOM during
 * handshake/setup, after moving our own large static workspaces into PSRAM.
 *
 * Compatibility note:
 * MBEDTLS_SSL_IN_CONTENT_LEN is an endpoint-specific deployment tradeoff here,
 * not a universally safe value for arbitrary TLS servers. The current upload
 * endpoint / cert chain was verified on hardware to complete the handshake with
 * a 6144-byte inbound record buffer. If the server, CDN, or certificate chain
 * changes and starts emitting larger handshake records, TLS may fail with
 * MBEDTLS_ERR_SSL_INVALID_RECORD and this value may need to increase again.
 *
 * MBEDTLS_SSL_OUT_CONTENT_LEN can be smaller because this client only sends a
 * small request header and streams the request body in modest chunks.
 */
#define MBEDTLS_SSL_IN_CONTENT_LEN 6144
#define MBEDTLS_SSL_OUT_CONTENT_LEN 2048

#endif
