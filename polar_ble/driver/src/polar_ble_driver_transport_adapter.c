// SPDX-License-Identifier: MIT
#include "polar_ble_driver_transport_adapter.h"

bool polar_ble_driver_transport_adapter_handle_link_event(
    polar_ble_driver_runtime_link_t *link,
    uint16_t invalid_conn_handle,
    const polar_ble_driver_link_event_t *event,
    bool user_disconnect_requested,
    bool connect_intent) {
    if (link == 0 || event == 0) {
        return false;
    }

    switch (event->type) {
        case POLAR_BLE_DRIVER_LINK_EVENT_CONN_COMPLETE:
            polar_ble_driver_runtime_on_connection_complete(
                link,
                invalid_conn_handle,
                event->status,
                event->handle,
                event->conn_interval,
                event->conn_latency,
                event->supervision_timeout_10ms);
            return event->status == 0;

        case POLAR_BLE_DRIVER_LINK_EVENT_CONN_UPDATE_COMPLETE:
            if (link->conn_handle == invalid_conn_handle || event->handle == link->conn_handle) {
                polar_ble_driver_runtime_on_connection_update_complete(
                    link,
                    event->status,
                    event->conn_interval,
                    event->conn_latency,
                    event->supervision_timeout_10ms);
            }
            return false;

        case POLAR_BLE_DRIVER_LINK_EVENT_DISCONNECT:
            if (link->conn_handle == invalid_conn_handle || event->handle == link->conn_handle) {
                polar_ble_driver_runtime_on_disconnect(
                    link,
                    invalid_conn_handle,
                    event->status,
                    event->reason,
                    user_disconnect_requested,
                    connect_intent);
            }
            return false;

        default:
            return false;
    }
}
