// SPDX-License-Identifier: MIT
#ifndef POLAR_BLE_DRIVER_GATT_WRITE_H
#define POLAR_BLE_DRIVER_GATT_WRITE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    POLAR_BLE_DRIVER_GATT_WRITE_OK = 0,
    POLAR_BLE_DRIVER_GATT_WRITE_NOT_CONNECTED,
    POLAR_BLE_DRIVER_GATT_WRITE_FAILED,
    POLAR_BLE_DRIVER_GATT_WRITE_TIMEOUT,
    POLAR_BLE_DRIVER_GATT_WRITE_ATT_REJECTED,
} polar_ble_driver_gatt_write_result_t;

typedef struct {
    void *ctx;
    bool (*is_connected)(void *ctx);

    // return 0 on transport success.
    int (*write_value)(void *ctx, const uint8_t *data, uint16_t len);

    // return true on query completion and set *out_att_status.
    bool (*wait_complete)(void *ctx, uint32_t timeout_ms, uint8_t *out_att_status);
} polar_ble_driver_gatt_write_ops_t;

polar_ble_driver_gatt_write_result_t polar_ble_driver_gatt_write_with_wait(
    const polar_ble_driver_gatt_write_ops_t *ops,
    const uint8_t *data,
    uint16_t len,
    uint8_t att_success,
    uint32_t timeout_ms,
    uint8_t *out_att_status);

#ifdef __cplusplus
}
#endif

#endif // POLAR_BLE_DRIVER_GATT_WRITE_H
