// SPDX-License-Identifier: MIT
#ifndef POLAR_BLE_DRIVER_BTSTACK_GATT_H
#define POLAR_BLE_DRIVER_BTSTACK_GATT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t handle;
    uint16_t mtu;
} polar_ble_driver_btstack_mtu_event_t;

typedef struct {
    bool notification;
    uint16_t handle;
    uint16_t value_handle;
    const uint8_t *value;
    uint16_t value_len;
} polar_ble_driver_btstack_value_event_t;

bool polar_ble_driver_btstack_decode_mtu_event(
    uint8_t packet_type,
    uint8_t *packet,
    polar_ble_driver_btstack_mtu_event_t *out);

bool polar_ble_driver_btstack_decode_value_event(
    uint8_t packet_type,
    uint8_t *packet,
    polar_ble_driver_btstack_value_event_t *out);

bool polar_ble_driver_btstack_decode_query_complete_att_status(
    uint8_t packet_type,
    uint8_t *packet,
    uint8_t *out_att_status);

bool polar_ble_driver_btstack_decode_service_query_result(
    uint8_t packet_type,
    uint8_t *packet,
    void *out_service /* gatt_client_service_t* */);

bool polar_ble_driver_btstack_decode_characteristic_query_result(
    uint8_t packet_type,
    uint8_t *packet,
    void *out_characteristic /* gatt_client_characteristic_t* */);

#ifdef __cplusplus
}
#endif

#endif // POLAR_BLE_DRIVER_BTSTACK_GATT_H
