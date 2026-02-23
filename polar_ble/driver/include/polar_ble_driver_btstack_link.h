// SPDX-License-Identifier: MIT
#ifndef POLAR_BLE_DRIVER_BTSTACK_LINK_H
#define POLAR_BLE_DRIVER_BTSTACK_LINK_H

#include <stdbool.h>
#include <stdint.h>

#include "polar_ble_driver_transport_adapter.h"

#ifdef __cplusplus
extern "C" {
#endif

bool polar_ble_driver_btstack_decode_link_event(
    uint8_t packet_type,
    uint8_t *packet,
    polar_ble_driver_link_event_t *out_event);

#ifdef __cplusplus
}
#endif

#endif // POLAR_BLE_DRIVER_BTSTACK_LINK_H
