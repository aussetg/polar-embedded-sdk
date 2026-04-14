// SPDX-License-Identifier: MIT
#ifndef POLAR_SDK_BTSTACK_SECURITY_H
#define POLAR_SDK_BTSTACK_SECURITY_H

#include <stdbool.h>
#include <stdint.h>

#include "polar_sdk_security.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*polar_sdk_btstack_security_sleep_ms_fn_t)(void *ctx,
                                                         uint32_t ms);

// Return the live encryption key size reported by BTstack for the active link.
// Callers should prefer this over cached security-event state.
uint8_t
polar_sdk_btstack_security_encryption_key_size(uint16_t conn_handle,
                                               uint16_t invalid_conn_handle);

bool polar_sdk_btstack_security_ready(uint16_t conn_handle,
                                      uint16_t invalid_conn_handle);

// Apply the project-default Security Manager policy and issue a pairing
// request for the active link if one exists.
void polar_sdk_btstack_security_request_pairing(uint16_t conn_handle,
                                                uint16_t invalid_conn_handle);

// Apply the project-default BTstack Security Manager policy, request pairing,
// and wait for live link security to become ready according to the supplied
// retry policy.
polar_sdk_security_result_t polar_sdk_btstack_security_ensure(
    const uint16_t *conn_handle, uint16_t invalid_conn_handle,
    const polar_sdk_security_policy_t *policy,
    polar_sdk_btstack_security_sleep_ms_fn_t sleep_ms, void *sleep_ctx);

#ifdef __cplusplus
}
#endif

#endif // POLAR_SDK_BTSTACK_SECURITY_H