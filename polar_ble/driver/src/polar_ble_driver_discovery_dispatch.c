// SPDX-License-Identifier: MIT
#include "polar_ble_driver_discovery_dispatch.h"

uint8_t polar_ble_driver_discovery_dispatch_command(
    polar_ble_driver_discovery_command_t cmd,
    const polar_ble_driver_discovery_dispatch_ops_t *ops) {
    if (ops == 0) {
        return 1;
    }

    switch (cmd) {
        case POLAR_BLE_DRIVER_DISCOVERY_CMD_DISCOVER_HR_CHARS:
            return ops->discover_hr_chars ? ops->discover_hr_chars(ops->ctx) : 1;
        case POLAR_BLE_DRIVER_DISCOVERY_CMD_DISCOVER_PMD_CHARS:
            return ops->discover_pmd_chars ? ops->discover_pmd_chars(ops->ctx) : 1;
        case POLAR_BLE_DRIVER_DISCOVERY_CMD_DISCOVER_PSFTP_CHARS:
            return ops->discover_psftp_chars ? ops->discover_psftp_chars(ops->ctx) : 1;
        case POLAR_BLE_DRIVER_DISCOVERY_CMD_NONE:
        default:
            return 0;
    }
}
