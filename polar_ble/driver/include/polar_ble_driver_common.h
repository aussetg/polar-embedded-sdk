// SPDX-License-Identifier: MIT
#ifndef POLAR_BLE_DRIVER_COMMON_H
#define POLAR_BLE_DRIVER_COMMON_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint32_t polar_ble_driver_backoff_delay_ms(uint32_t attempt_index);
bool polar_ble_driver_service_mask_is_valid(uint32_t mask, uint32_t allowed_mask);

#ifdef __cplusplus
}
#endif

#endif // POLAR_BLE_DRIVER_COMMON_H
