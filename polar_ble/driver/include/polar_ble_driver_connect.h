// SPDX-License-Identifier: MIT
#ifndef POLAR_BLE_DRIVER_CONNECT_H
#define POLAR_BLE_DRIVER_CONNECT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t timeout_ms;
    uint32_t attempt_slice_ms;
} polar_ble_driver_connect_policy_t;

typedef struct {
    uint32_t start_ms;
    uint32_t attempt_index;
} polar_ble_driver_connect_state_t;

void polar_ble_driver_connect_init(
    polar_ble_driver_connect_state_t *state,
    uint32_t now_ms);

bool polar_ble_driver_connect_has_time_left(
    const polar_ble_driver_connect_policy_t *policy,
    const polar_ble_driver_connect_state_t *state,
    uint32_t now_ms);

uint32_t polar_ble_driver_connect_attempt_budget_ms(
    const polar_ble_driver_connect_policy_t *policy,
    const polar_ble_driver_connect_state_t *state,
    uint32_t now_ms);

uint32_t polar_ble_driver_connect_next_backoff_ms(
    const polar_ble_driver_connect_policy_t *policy,
    polar_ble_driver_connect_state_t *state,
    uint32_t now_ms);

#ifdef __cplusplus
}
#endif

#endif // POLAR_BLE_DRIVER_CONNECT_H
