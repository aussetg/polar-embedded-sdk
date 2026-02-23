// SPDX-License-Identifier: MIT
#include "polar_ble_driver_gatt_write.h"

static bool polar_ble_driver_gatt_write_ops_ready(const polar_ble_driver_gatt_write_ops_t *ops) {
    return ops != 0 &&
        ops->is_connected != 0 &&
        ops->write_value != 0 &&
        ops->wait_complete != 0;
}

polar_ble_driver_gatt_write_result_t polar_ble_driver_gatt_write_with_wait(
    const polar_ble_driver_gatt_write_ops_t *ops,
    const uint8_t *data,
    uint16_t len,
    uint8_t att_success,
    uint32_t timeout_ms,
    uint8_t *out_att_status) {
    if (!polar_ble_driver_gatt_write_ops_ready(ops) || data == 0 || len == 0) {
        return POLAR_BLE_DRIVER_GATT_WRITE_FAILED;
    }
    if (out_att_status) {
        *out_att_status = att_success;
    }

    if (!ops->is_connected(ops->ctx)) {
        return POLAR_BLE_DRIVER_GATT_WRITE_NOT_CONNECTED;
    }

    if (ops->write_value(ops->ctx, data, len) != 0) {
        return POLAR_BLE_DRIVER_GATT_WRITE_FAILED;
    }

    uint8_t att_status = att_success;
    if (!ops->wait_complete(ops->ctx, timeout_ms, &att_status)) {
        return POLAR_BLE_DRIVER_GATT_WRITE_TIMEOUT;
    }
    if (out_att_status) {
        *out_att_status = att_status;
    }
    if (att_status != att_success) {
        return POLAR_BLE_DRIVER_GATT_WRITE_ATT_REJECTED;
    }

    return POLAR_BLE_DRIVER_GATT_WRITE_OK;
}
