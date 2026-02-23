// SPDX-License-Identifier: MIT
#ifndef POLAR_BLE_DRIVER_TRANSPORT_H
#define POLAR_BLE_DRIVER_TRANSPORT_H

#include <stdbool.h>
#include <stdint.h>

#include "polar_ble_driver_connect.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    POLAR_BLE_DRIVER_TRANSPORT_ATTEMPT_READY = 0,
    POLAR_BLE_DRIVER_TRANSPORT_ATTEMPT_FAILED,
    POLAR_BLE_DRIVER_TRANSPORT_ATTEMPT_TIMEOUT,
} polar_ble_driver_transport_attempt_result_t;

typedef enum {
    POLAR_BLE_DRIVER_TRANSPORT_CONNECT_OK = 0,
    POLAR_BLE_DRIVER_TRANSPORT_CONNECT_TIMEOUT,
    POLAR_BLE_DRIVER_TRANSPORT_CONNECT_FAILED,
} polar_ble_driver_transport_connect_result_t;

typedef struct {
    void *ctx;
    uint32_t (*now_ms)(void *ctx);
    void (*sleep_ms)(void *ctx, uint32_t ms);

    bool (*start_attempt)(void *ctx);
    polar_ble_driver_transport_attempt_result_t (*wait_attempt)(void *ctx, uint32_t attempt_budget_ms);
    void (*cleanup_after_attempt)(void *ctx);
    void (*on_connect_ready)(void *ctx);
} polar_ble_driver_transport_connect_ops_t;

typedef struct {
    uint32_t attempts_total;
    uint32_t backoff_events_total;
} polar_ble_driver_transport_connect_stats_t;

polar_ble_driver_transport_connect_result_t polar_ble_driver_transport_connect_blocking(
    const polar_ble_driver_connect_policy_t *policy,
    const polar_ble_driver_transport_connect_ops_t *ops,
    polar_ble_driver_transport_connect_stats_t *out_stats);

#ifdef __cplusplus
}
#endif

#endif // POLAR_BLE_DRIVER_TRANSPORT_H
