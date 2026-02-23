// SPDX-License-Identifier: MIT
#ifndef POLAR_BLE_DRIVER_SM_CONTROL_H
#define POLAR_BLE_DRIVER_SM_CONTROL_H

#include <stdbool.h>
#include <stdint.h>

#include "polar_ble_driver_btstack_sm.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    void *ctx;
    void (*on_just_works_request)(void *ctx, uint16_t handle);
    void (*on_numeric_comparison_request)(void *ctx, uint16_t handle);
    void (*on_authorization_request)(void *ctx, uint16_t handle);
    void (*on_pairing_complete)(void *ctx, const polar_ble_driver_sm_event_t *event);
} polar_ble_driver_sm_control_ops_t;

bool polar_ble_driver_sm_control_apply(
    const polar_ble_driver_sm_event_t *event,
    uint16_t active_conn_handle,
    uint16_t invalid_conn_handle,
    const polar_ble_driver_sm_control_ops_t *ops);

#ifdef __cplusplus
}
#endif

#endif // POLAR_BLE_DRIVER_SM_CONTROL_H
