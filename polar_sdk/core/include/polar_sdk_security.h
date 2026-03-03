// SPDX-License-Identifier: MIT
#ifndef POLAR_SDK_SECURITY_H
#define POLAR_SDK_SECURITY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    POLAR_SDK_SECURITY_RESULT_OK = 0,
    POLAR_SDK_SECURITY_RESULT_NOT_CONNECTED,
    POLAR_SDK_SECURITY_RESULT_TIMEOUT,
    POLAR_SDK_SECURITY_RESULT_INVALID_ARGS,
} polar_sdk_security_result_t;

typedef struct {
    size_t rounds;
    uint32_t wait_ms_per_round;
    uint32_t request_gap_ms;
    uint32_t poll_ms;
} polar_sdk_security_policy_t;

typedef struct {
    void *ctx;
    bool (*is_connected)(void *ctx);
    bool (*is_secure)(void *ctx);
    void (*request_pairing)(void *ctx);
    void (*sleep_ms)(void *ctx, uint32_t ms);
} polar_sdk_security_ops_t;

polar_sdk_security_result_t polar_sdk_security_request_with_retry(
    const polar_sdk_security_policy_t *policy,
    const polar_sdk_security_ops_t *ops);

#ifdef __cplusplus
}
#endif

#endif // POLAR_SDK_SECURITY_H
