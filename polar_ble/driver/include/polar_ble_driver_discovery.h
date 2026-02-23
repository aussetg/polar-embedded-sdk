// SPDX-License-Identifier: MIT
#ifndef POLAR_BLE_DRIVER_DISCOVERY_H
#define POLAR_BLE_DRIVER_DISCOVERY_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    POLAR_BLE_DRIVER_DISC_STAGE_IDLE = 0,
    POLAR_BLE_DRIVER_DISC_STAGE_SERVICES,
    POLAR_BLE_DRIVER_DISC_STAGE_HR_CHARS,
    POLAR_BLE_DRIVER_DISC_STAGE_PMD_CHARS,
    POLAR_BLE_DRIVER_DISC_STAGE_PSFTP_CHARS,
} polar_ble_driver_discovery_stage_t;

typedef enum {
    POLAR_BLE_DRIVER_DISC_ACTION_FAIL = 0,
    POLAR_BLE_DRIVER_DISC_ACTION_DISCOVER_HR_CHARS,
    POLAR_BLE_DRIVER_DISC_ACTION_DISCOVER_PMD_CHARS,
    POLAR_BLE_DRIVER_DISC_ACTION_DISCOVER_PSFTP_CHARS,
    POLAR_BLE_DRIVER_DISC_ACTION_READY,
} polar_ble_driver_discovery_action_t;

bool polar_ble_driver_discovery_required_services_present(
    uint8_t required_services_mask,
    bool hr_service_found,
    bool pmd_service_found,
    bool psftp_service_found,
    uint8_t service_hr_bit,
    uint8_t service_ecg_bit,
    uint8_t service_psftp_bit);

bool polar_ble_driver_discovery_required_handles_ready(
    uint8_t required_services_mask,
    uint16_t hr_measurement_handle,
    uint16_t pmd_cp_handle,
    uint16_t pmd_data_handle,
    uint16_t psftp_mtu_handle,
    uint16_t psftp_d2h_handle,
    uint16_t psftp_h2d_handle,
    uint8_t service_hr_bit,
    uint8_t service_ecg_bit,
    uint8_t service_psftp_bit);

polar_ble_driver_discovery_action_t polar_ble_driver_discovery_next_action(
    polar_ble_driver_discovery_stage_t stage,
    uint8_t required_services_mask,
    bool hr_service_found,
    bool pmd_service_found,
    bool psftp_service_found,
    uint16_t hr_measurement_handle,
    uint16_t pmd_cp_handle,
    uint16_t pmd_data_handle,
    uint16_t psftp_mtu_handle,
    uint16_t psftp_d2h_handle,
    uint16_t psftp_h2d_handle,
    uint8_t service_hr_bit,
    uint8_t service_ecg_bit,
    uint8_t service_psftp_bit,
    polar_ble_driver_discovery_stage_t *out_next_stage);

#ifdef __cplusplus
}
#endif

#endif // POLAR_BLE_DRIVER_DISCOVERY_H
