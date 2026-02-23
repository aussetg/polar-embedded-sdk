// SPDX-License-Identifier: LicenseRef-BTstack
// See NOTICE for license details (non-commercial, RP2 exception available)
#include "polar_ble_driver_btstack_helpers.h"

#include <string.h>

#include "btstack.h"

bool polar_ble_driver_btstack_uuid128_matches_any_order(const uint8_t *candidate, const uint8_t *be_uuid) {
    if (candidate == 0 || be_uuid == 0) {
        return false;
    }
    if (memcmp(candidate, be_uuid, 16) == 0) {
        return true;
    }
    for (size_t i = 0; i < 16; ++i) {
        if (candidate[i] != be_uuid[15 - i]) {
            return false;
        }
    }
    return true;
}

bool polar_ble_driver_btstack_adv_name_matches_prefix(
    const uint8_t *adv_data,
    uint8_t adv_len,
    const uint8_t *prefix,
    size_t prefix_len) {
    if (prefix_len == 0) {
        return true;
    }
    if (adv_data == 0 || prefix == 0) {
        return false;
    }

    ad_context_t context;
    for (ad_iterator_init(&context, adv_len, adv_data); ad_iterator_has_more(&context); ad_iterator_next(&context)) {
        uint8_t data_type = ad_iterator_get_data_type(&context);
        uint8_t data_len = ad_iterator_get_data_len(&context);
        const uint8_t *data = ad_iterator_get_data(&context);

        if (data_type == BLUETOOTH_DATA_TYPE_SHORTENED_LOCAL_NAME ||
            data_type == BLUETOOTH_DATA_TYPE_COMPLETE_LOCAL_NAME) {
            if (data_len >= prefix_len && memcmp(data, prefix, prefix_len) == 0) {
                return true;
            }
        }
    }
    return false;
}

polar_ble_driver_disc_service_kind_t polar_ble_driver_btstack_classify_service(
    uint16_t uuid16,
    const uint8_t *uuid128,
    uint16_t hr_service_uuid16,
    uint16_t psftp_service_uuid16,
    const uint8_t *pmd_service_uuid_be) {
    if (hr_service_uuid16 != 0 && uuid16 == hr_service_uuid16) {
        return POLAR_BLE_DRIVER_DISC_SERVICE_HR;
    }
    if (psftp_service_uuid16 != 0 && uuid16 == psftp_service_uuid16) {
        return POLAR_BLE_DRIVER_DISC_SERVICE_PSFTP;
    }
    if (uuid16 == 0 && polar_ble_driver_btstack_uuid128_matches_any_order(uuid128, pmd_service_uuid_be)) {
        return POLAR_BLE_DRIVER_DISC_SERVICE_PMD;
    }
    return POLAR_BLE_DRIVER_DISC_SERVICE_NONE;
}

polar_ble_driver_disc_char_kind_t polar_ble_driver_btstack_classify_char(
    polar_ble_driver_discovery_stage_t stage,
    uint16_t uuid16,
    const uint8_t *uuid128,
    uint16_t hr_measurement_uuid16,
    const uint8_t *pmd_cp_uuid_be,
    const uint8_t *pmd_data_uuid_be,
    const uint8_t *psftp_mtu_uuid_be,
    const uint8_t *psftp_d2h_uuid_be,
    const uint8_t *psftp_h2d_uuid_be) {
    if (stage == POLAR_BLE_DRIVER_DISC_STAGE_HR_CHARS) {
        return uuid16 == hr_measurement_uuid16 ? POLAR_BLE_DRIVER_DISC_CHAR_HR_MEAS : POLAR_BLE_DRIVER_DISC_CHAR_NONE;
    }

    if (stage == POLAR_BLE_DRIVER_DISC_STAGE_PMD_CHARS) {
        if (uuid16 == 0 && polar_ble_driver_btstack_uuid128_matches_any_order(uuid128, pmd_cp_uuid_be)) {
            return POLAR_BLE_DRIVER_DISC_CHAR_PMD_CP;
        }
        if (uuid16 == 0 && polar_ble_driver_btstack_uuid128_matches_any_order(uuid128, pmd_data_uuid_be)) {
            return POLAR_BLE_DRIVER_DISC_CHAR_PMD_DATA;
        }
        return POLAR_BLE_DRIVER_DISC_CHAR_NONE;
    }

    if (stage == POLAR_BLE_DRIVER_DISC_STAGE_PSFTP_CHARS) {
        if (uuid16 == 0 && polar_ble_driver_btstack_uuid128_matches_any_order(uuid128, psftp_mtu_uuid_be)) {
            return POLAR_BLE_DRIVER_DISC_CHAR_PSFTP_MTU;
        }
        if (uuid16 == 0 && polar_ble_driver_btstack_uuid128_matches_any_order(uuid128, psftp_d2h_uuid_be)) {
            return POLAR_BLE_DRIVER_DISC_CHAR_PSFTP_D2H;
        }
        if (uuid16 == 0 && polar_ble_driver_btstack_uuid128_matches_any_order(uuid128, psftp_h2d_uuid_be)) {
            return POLAR_BLE_DRIVER_DISC_CHAR_PSFTP_H2D;
        }
    }

    return POLAR_BLE_DRIVER_DISC_CHAR_NONE;
}
