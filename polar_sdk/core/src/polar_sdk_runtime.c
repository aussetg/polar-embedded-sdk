// SPDX-License-Identifier: MIT
#include "polar_sdk_runtime.h"

void polar_sdk_runtime_link_init(
    polar_sdk_runtime_link_t *link,
    uint16_t invalid_conn_handle) {
    if (link == 0) {
        return;
    }

    link->state = POLAR_SDK_RUNTIME_STATE_IDLE;
    link->connected = false;
    link->conn_handle = invalid_conn_handle;
    link->attempt_failed = false;

    link->last_hci_status = 0;
    link->last_disconnect_status = 0;
    link->last_disconnect_reason = 0;

    link->conn_complete_total = 0;
    link->conn_update_complete_total = 0;

    link->last_conn_interval_units = 0;
    link->last_conn_latency = 0;
    link->last_conn_supervision_timeout_10ms = 0;
    link->last_conn_update_status = 0;
    link->conn_update_pending = false;

    link->disconnect_events_total = 0;
    link->disconnect_reason_0x08_total = 0;
    link->disconnect_reason_0x3e_total = 0;
    link->disconnect_reason_other_total = 0;
}

void polar_sdk_runtime_mark_attempt_failed(
    polar_sdk_runtime_link_t *link) {
    if (link == 0) {
        return;
    }
    link->attempt_failed = true;
    if (!link->connected) {
        link->state = POLAR_SDK_RUNTIME_STATE_RECOVERING;
    }
}

void polar_sdk_runtime_on_connection_complete(
    polar_sdk_runtime_link_t *link,
    uint16_t invalid_conn_handle,
    uint8_t status,
    uint16_t handle,
    uint16_t conn_interval,
    uint16_t conn_latency,
    uint16_t supervision_timeout_10ms) {
    if (link == 0) {
        return;
    }

    link->conn_complete_total += 1;
    link->last_hci_status = status;
    link->last_conn_interval_units = conn_interval;
    link->last_conn_latency = conn_latency;
    link->last_conn_supervision_timeout_10ms = supervision_timeout_10ms;
    link->last_conn_update_status = status;

    if (status != 0) {
        link->conn_update_pending = false;
        link->connected = false;
        link->conn_handle = invalid_conn_handle;
        polar_sdk_runtime_mark_attempt_failed(link);
        return;
    }

    link->connected = true;
    link->conn_handle = handle;
    link->state = POLAR_SDK_RUNTIME_STATE_DISCOVERING;
    link->attempt_failed = false;
}

void polar_sdk_runtime_on_connection_update_complete(
    polar_sdk_runtime_link_t *link,
    uint8_t status,
    uint16_t conn_interval,
    uint16_t conn_latency,
    uint16_t supervision_timeout_10ms) {
    if (link == 0) {
        return;
    }

    link->conn_update_pending = false;
    link->conn_update_complete_total += 1;
    link->last_conn_update_status = status;
    link->last_conn_interval_units = conn_interval;
    link->last_conn_latency = conn_latency;
    link->last_conn_supervision_timeout_10ms = supervision_timeout_10ms;
}

void polar_sdk_runtime_on_disconnect(
    polar_sdk_runtime_link_t *link,
    uint16_t invalid_conn_handle,
    uint8_t status,
    uint8_t reason,
    bool user_disconnect_requested,
    bool connect_intent) {
    if (link == 0) {
        return;
    }

    link->last_disconnect_status = status;
    link->last_disconnect_reason = reason;
    link->disconnect_events_total += 1;

    if (reason == 0x08) {
        link->disconnect_reason_0x08_total += 1;
    } else if (reason == 0x3e) {
        link->disconnect_reason_0x3e_total += 1;
    } else {
        link->disconnect_reason_other_total += 1;
    }

    link->conn_update_pending = false;
    link->connected = false;
    link->conn_handle = invalid_conn_handle;

    if (user_disconnect_requested) {
        link->state = POLAR_SDK_RUNTIME_STATE_IDLE;
    } else {
        link->state = POLAR_SDK_RUNTIME_STATE_RECOVERING;
        if (connect_intent) {
            link->attempt_failed = true;
        }
    }
}
