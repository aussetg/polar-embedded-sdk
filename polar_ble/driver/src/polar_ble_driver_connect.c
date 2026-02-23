// SPDX-License-Identifier: MIT
#include "polar_ble_driver_connect.h"

#include "polar_ble_driver_common.h"

static uint32_t polar_ble_driver_connect_elapsed_ms(uint32_t start_ms, uint32_t now_ms) {
    return (uint32_t)(now_ms - start_ms);
}

void polar_ble_driver_connect_init(
    polar_ble_driver_connect_state_t *state,
    uint32_t now_ms) {
    if (state == 0) {
        return;
    }
    state->start_ms = now_ms;
    state->attempt_index = 0;
}

bool polar_ble_driver_connect_has_time_left(
    const polar_ble_driver_connect_policy_t *policy,
    const polar_ble_driver_connect_state_t *state,
    uint32_t now_ms) {
    if (policy == 0 || state == 0 || policy->timeout_ms == 0) {
        return false;
    }
    return polar_ble_driver_connect_elapsed_ms(state->start_ms, now_ms) < policy->timeout_ms;
}

uint32_t polar_ble_driver_connect_attempt_budget_ms(
    const polar_ble_driver_connect_policy_t *policy,
    const polar_ble_driver_connect_state_t *state,
    uint32_t now_ms) {
    if (!polar_ble_driver_connect_has_time_left(policy, state, now_ms)) {
        return 0;
    }

    uint32_t elapsed = polar_ble_driver_connect_elapsed_ms(state->start_ms, now_ms);
    uint32_t remaining = policy->timeout_ms - elapsed;
    if (remaining == 0) {
        return 0;
    }

    if (policy->attempt_slice_ms == 0 || policy->attempt_slice_ms > remaining) {
        return remaining;
    }
    return policy->attempt_slice_ms;
}

uint32_t polar_ble_driver_connect_next_backoff_ms(
    const polar_ble_driver_connect_policy_t *policy,
    polar_ble_driver_connect_state_t *state,
    uint32_t now_ms) {
    if (!polar_ble_driver_connect_has_time_left(policy, state, now_ms)) {
        return 0;
    }

    uint32_t elapsed = polar_ble_driver_connect_elapsed_ms(state->start_ms, now_ms);
    uint32_t remaining = policy->timeout_ms - elapsed;
    if (remaining == 0) {
        return 0;
    }

    uint32_t backoff_ms = polar_ble_driver_backoff_delay_ms(state->attempt_index);
    if (backoff_ms > remaining) {
        backoff_ms = remaining;
    }

    if (backoff_ms > 0) {
        state->attempt_index += 1;
    }
    return backoff_ms;
}
