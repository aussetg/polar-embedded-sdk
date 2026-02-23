// SPDX-License-Identifier: MIT
#ifndef POLAR_BLE_DRIVER_GATT_QUERY_COMPLETE_H
#define POLAR_BLE_DRIVER_GATT_QUERY_COMPLETE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool *pending;
    bool *done;
    uint8_t *att_status;
    bool update_last_att_status;
} polar_ble_driver_gatt_query_slot_t;

bool polar_ble_driver_gatt_apply_query_complete(
    uint8_t query_complete_att_status,
    const polar_ble_driver_gatt_query_slot_t *slots,
    size_t slot_count,
    uint8_t *last_att_status);

#ifdef __cplusplus
}
#endif

#endif // POLAR_BLE_DRIVER_GATT_QUERY_COMPLETE_H
