// SPDX-License-Identifier: MIT
#ifndef POLAR_SDK_BTSTACK_CONFIG_H
#define POLAR_SDK_BTSTACK_CONFIG_H

// Compile-time requirements for btstack_config.h when using the Polar SDK
// with the BTstack integration layer.
//
// This header does NOT define anything — btstack_config.h is owned by the
// firmware project.  It only validates that the required options are present
// and produces clear #error messages if not.

#include "btstack_config.h"

// ── Role ────────────────────────────────────────────────────────────────
// The SDK is a GATT client that connects to Polar sensors.
#if !defined(ENABLE_LE_CENTRAL)
#error "Polar SDK requires ENABLE_LE_CENTRAL in btstack_config.h"
#endif

// ── Pairing ─────────────────────────────────────────────────────────────
// Polar sensors set the LE Secure Connections bit in their pairing response.
// Legacy pairing works as a downgrade but SC is the expected path.
#if !defined(ENABLE_LE_SECURE_CONNECTIONS)
#error "Polar SDK requires ENABLE_LE_SECURE_CONNECTIONS in btstack_config.h (the H10 expects SC pairing)"
#endif

// ── Crypto backend ──────────────────────────────────────────────────────
// SC pairing needs an ECC-P256 implementation.  The SDK recommends mbedtls
// (already linked for TLS on most platforms) to avoid bundling micro-ecc.
//
//   HAVE_MBEDTLS_ECC_P256  — use mbedtls ECP (recommended)
//   ENABLE_MICRO_ECC_P256  — use bundled micro-ecc (alternative)
//   ENABLE_MICRO_ECC_FOR_LE_SECURE_CONNECTIONS — legacy alias for above
//
#if !defined(HAVE_MBEDTLS_ECC_P256) && !defined(ENABLE_MICRO_ECC_P256) && !defined(ENABLE_MICRO_ECC_FOR_LE_SECURE_CONNECTIONS)
#error "Polar SDK requires an ECC-P256 backend for LE Secure Connections. Define HAVE_MBEDTLS_ECC_P256 (recommended) or ENABLE_MICRO_ECC_FOR_LE_SECURE_CONNECTIONS in btstack_config.h."
#endif

// AES-128 is needed for CMAC (used by the Security Manager).  Either provide
// your own via HAVE_AES128 or let BTstack bundle rijndael via
// ENABLE_SOFTWARE_AES128.
#if !defined(HAVE_AES128) && !defined(ENABLE_SOFTWARE_AES128)
#error "Polar SDK requires an AES-128 backend for the BLE Security Manager. Define HAVE_AES128 or ENABLE_SOFTWARE_AES128 in btstack_config.h."
#endif

// Don't allow both — BTstack will also check but the error message here is
// more discoverable.
#if defined(HAVE_AES128) && defined(ENABLE_SOFTWARE_AES128)
#error "HAVE_AES128 and ENABLE_SOFTWARE_AES128 are mutually exclusive in btstack_config.h."
#endif

#if defined(HAVE_MBEDTLS_ECC_P256) && defined(ENABLE_MICRO_ECC_P256)
#error "HAVE_MBEDTLS_ECC_P256 and ENABLE_MICRO_ECC_P256 are mutually exclusive in btstack_config.h."
#endif

#endif // POLAR_SDK_BTSTACK_CONFIG_H
