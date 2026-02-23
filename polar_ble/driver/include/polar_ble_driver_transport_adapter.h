// SPDX-License-Identifier: MIT
#ifndef POLAR_BLE_DRIVER_TRANSPORT_ADAPTER_H
#define POLAR_BLE_DRIVER_TRANSPORT_ADAPTER_H

#include <stdbool.h>
#include <stdint.h>

#include "polar_ble_driver_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    POLAR_BLE_DRIVER_LINK_EVENT_CONN_COMPLETE = 0,
    POLAR_BLE_DRIVER_LINK_EVENT_CONN_UPDATE_COMPLETE,
    POLAR_BLE_DRIVER_LINK_EVENT_DISCONNECT,
} polar_ble_driver_link_event_type_t;

typedef struct {
    polar_ble_driver_link_event_type_t type;
    uint8_t status;
    uint16_t handle;

    // conn complete + conn update
    uint16_t conn_interval;
    uint16_t conn_latency;
    uint16_t supervision_timeout_10ms;

    // disconnect
    uint8_t reason;
} polar_ble_driver_link_event_t;

bool polar_ble_driver_transport_adapter_handle_link_event(
    polar_ble_driver_runtime_link_t *link,
    uint16_t invalid_conn_handle,
    const polar_ble_driver_link_event_t *event,
    bool user_disconnect_requested,
    bool connect_intent);

#ifdef __cplusplus
}
#endif

#endif // POLAR_BLE_DRIVER_TRANSPORT_ADAPTER_H
