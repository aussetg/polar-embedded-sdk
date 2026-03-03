// SPDX-License-Identifier: MIT
#ifndef POLAR_BLE_DRIVER_BTSTACK_SM_H
#define POLAR_BLE_DRIVER_BTSTACK_SM_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    POLAR_BLE_DRIVER_SM_EVENT_NONE = 0,
    POLAR_BLE_DRIVER_SM_EVENT_JUST_WORKS_REQUEST,
    POLAR_BLE_DRIVER_SM_EVENT_NUMERIC_COMPARISON_REQUEST,
    POLAR_BLE_DRIVER_SM_EVENT_AUTHORIZATION_REQUEST,
    POLAR_BLE_DRIVER_SM_EVENT_PAIRING_COMPLETE,
} polar_ble_driver_sm_event_type_t;

typedef struct {
    polar_ble_driver_sm_event_type_t type;
    uint16_t handle;
    uint8_t status;
    uint8_t reason;
} polar_ble_driver_sm_event_t;

bool polar_ble_driver_btstack_decode_sm_event(
    uint8_t packet_type,
    uint8_t *packet,
    polar_ble_driver_sm_event_t *out_event);

bool polar_ble_driver_sm_event_matches_handle(
    const polar_ble_driver_sm_event_t *event,
    uint16_t active_handle,
    uint16_t invalid_conn_handle);

// Apply project-default Security Manager policy for central/client role.
// Keeps BLE security setup consistent across probes and MicroPython binding.
void polar_ble_driver_btstack_sm_apply_default_auth_policy(void);

#ifdef __cplusplus
}
#endif

#endif // POLAR_BLE_DRIVER_BTSTACK_SM_H
