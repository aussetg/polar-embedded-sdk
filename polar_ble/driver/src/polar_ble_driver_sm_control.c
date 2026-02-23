// SPDX-License-Identifier: MIT
#include "polar_ble_driver_sm_control.h"

bool polar_ble_driver_sm_control_apply(
    const polar_ble_driver_sm_event_t *event,
    uint16_t active_conn_handle,
    uint16_t invalid_conn_handle,
    const polar_ble_driver_sm_control_ops_t *ops) {
    if (event == 0 || ops == 0) {
        return false;
    }
    if (!polar_ble_driver_sm_event_matches_handle(event, active_conn_handle, invalid_conn_handle)) {
        return false;
    }

    switch (event->type) {
        case POLAR_BLE_DRIVER_SM_EVENT_JUST_WORKS_REQUEST:
            if (ops->on_just_works_request) {
                ops->on_just_works_request(ops->ctx, event->handle);
            }
            return true;

        case POLAR_BLE_DRIVER_SM_EVENT_NUMERIC_COMPARISON_REQUEST:
            if (ops->on_numeric_comparison_request) {
                ops->on_numeric_comparison_request(ops->ctx, event->handle);
            }
            return true;

        case POLAR_BLE_DRIVER_SM_EVENT_AUTHORIZATION_REQUEST:
            if (ops->on_authorization_request) {
                ops->on_authorization_request(ops->ctx, event->handle);
            }
            return true;

        case POLAR_BLE_DRIVER_SM_EVENT_PAIRING_COMPLETE:
            if (ops->on_pairing_complete) {
                ops->on_pairing_complete(ops->ctx, event);
            }
            return true;

        default:
            return false;
    }
}
