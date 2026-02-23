// SPDX-License-Identifier: MIT
#ifndef POLAR_BLE_DRIVER_BTSTACK_GATT_ROUTE_H
#define POLAR_BLE_DRIVER_BTSTACK_GATT_ROUTE_H

#include <stdbool.h>
#include <stdint.h>

#include "polar_ble_driver_btstack_gatt.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    POLAR_BLE_DRIVER_GATT_ROUTE_NONE = 0,
    POLAR_BLE_DRIVER_GATT_ROUTE_MTU_EVENT,
    POLAR_BLE_DRIVER_GATT_ROUTE_HR_VALUE,
    POLAR_BLE_DRIVER_GATT_ROUTE_PMD_CP_VALUE,
    POLAR_BLE_DRIVER_GATT_ROUTE_PMD_DATA_VALUE,
    POLAR_BLE_DRIVER_GATT_ROUTE_HR_UNMATCHED_VALUE,
    POLAR_BLE_DRIVER_GATT_ROUTE_QUERY_COMPLETE,
} polar_ble_driver_btstack_gatt_route_kind_t;

typedef struct {
    uint16_t conn_handle;
    bool connected;

    uint16_t hr_value_handle;
    bool hr_enabled;

    uint16_t pmd_cp_value_handle;
    bool pmd_cp_listening;

    uint16_t pmd_data_value_handle;
    bool ecg_enabled;
} polar_ble_driver_btstack_gatt_route_state_t;

typedef struct {
    polar_ble_driver_btstack_gatt_route_kind_t kind;
    uint8_t query_complete_att_status;
    polar_ble_driver_btstack_mtu_event_t mtu;
    polar_ble_driver_btstack_value_event_t value;
} polar_ble_driver_btstack_gatt_route_result_t;

bool polar_ble_driver_btstack_route_gatt_event(
    uint8_t packet_type,
    uint8_t *packet,
    const polar_ble_driver_btstack_gatt_route_state_t *state,
    polar_ble_driver_btstack_gatt_route_result_t *out);

#ifdef __cplusplus
}
#endif

#endif // POLAR_BLE_DRIVER_BTSTACK_GATT_ROUTE_H
