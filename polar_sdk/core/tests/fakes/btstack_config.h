// Minimal btstack_config.h for host-side polar_sdk tests.
//
// Provides the required defines so polar_sdk_btstack_config.h's checks pass.
// The tests link against fake BTstack stubs — no real BLE stack is present.

#ifndef POLAR_SDK_CORE_TESTS_FAKE_BTSTACK_CONFIG_H
#define POLAR_SDK_CORE_TESTS_FAKE_BTSTACK_CONFIG_H

#define ENABLE_LE_CENTRAL
#define ENABLE_LE_SECURE_CONNECTIONS
#define HAVE_MBEDTLS_ECC_P256
#define HAVE_AES128

#endif
