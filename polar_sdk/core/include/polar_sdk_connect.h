// SPDX-License-Identifier: MIT
#ifndef POLAR_SDK_CONNECT_H
#define POLAR_SDK_CONNECT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t timeout_ms;
    uint32_t attempt_slice_ms;
} polar_sdk_connect_policy_t;

typedef struct {
    uint32_t start_ms;
    uint32_t attempt_index;
} polar_sdk_connect_state_t;

void polar_sdk_connect_init(
    polar_sdk_connect_state_t *state,
    uint32_t now_ms);

bool polar_sdk_connect_has_time_left(
    const polar_sdk_connect_policy_t *policy,
    const polar_sdk_connect_state_t *state,
    uint32_t now_ms);

uint32_t polar_sdk_connect_attempt_budget_ms(
    const polar_sdk_connect_policy_t *policy,
    const polar_sdk_connect_state_t *state,
    uint32_t now_ms);

uint32_t polar_sdk_connect_next_backoff_ms(
    const polar_sdk_connect_policy_t *policy,
    polar_sdk_connect_state_t *state,
    uint32_t now_ms);

#ifdef __cplusplus
}
#endif

#endif // POLAR_SDK_CONNECT_H
