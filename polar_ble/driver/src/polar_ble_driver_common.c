// SPDX-License-Identifier: MIT
#include "polar_ble_driver_common.h"

uint32_t polar_ble_driver_backoff_delay_ms(uint32_t attempt_index) {
    // 200, 400, 800, 1600, 2000, 2000, ...
    if (attempt_index > 4) {
        return 2000;
    }
    uint32_t d = 200u << attempt_index;
    return d > 2000 ? 2000 : d;
}

bool polar_ble_driver_service_mask_is_valid(uint32_t mask, uint32_t allowed_mask) {
    return (mask & ~allowed_mask) == 0;
}
