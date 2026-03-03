// SPDX-License-Identifier: MIT
#ifndef POLAR_SDK_BTSTACK_HELPERS_H
#define POLAR_SDK_BTSTACK_HELPERS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "polar_sdk_discovery.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    POLAR_SDK_DISC_SERVICE_NONE = 0,
    POLAR_SDK_DISC_SERVICE_HR,
    POLAR_SDK_DISC_SERVICE_PMD,
    POLAR_SDK_DISC_SERVICE_PSFTP,
} polar_sdk_disc_service_kind_t;

typedef enum {
    POLAR_SDK_DISC_CHAR_NONE = 0,
    POLAR_SDK_DISC_CHAR_HR_MEAS,
    POLAR_SDK_DISC_CHAR_PMD_CP,
    POLAR_SDK_DISC_CHAR_PMD_DATA,
    POLAR_SDK_DISC_CHAR_PSFTP_MTU,
    POLAR_SDK_DISC_CHAR_PSFTP_D2H,
    POLAR_SDK_DISC_CHAR_PSFTP_H2D,
} polar_sdk_disc_char_kind_t;

bool polar_sdk_btstack_uuid128_matches_any_order(const uint8_t *candidate, const uint8_t *be_uuid);

bool polar_sdk_btstack_adv_name_matches_prefix(
    const uint8_t *adv_data,
    uint8_t adv_len,
    const uint8_t *prefix,
    size_t prefix_len);

polar_sdk_disc_service_kind_t polar_sdk_btstack_classify_service(
    uint16_t uuid16,
    const uint8_t *uuid128,
    uint16_t hr_service_uuid16,
    uint16_t psftp_service_uuid16,
    const uint8_t *pmd_service_uuid_be);

polar_sdk_disc_char_kind_t polar_sdk_btstack_classify_char(
    polar_sdk_discovery_stage_t stage,
    uint16_t uuid16,
    const uint8_t *uuid128,
    uint16_t hr_measurement_uuid16,
    const uint8_t *pmd_cp_uuid_be,
    const uint8_t *pmd_data_uuid_be,
    const uint8_t *psftp_mtu_uuid_be,
    const uint8_t *psftp_d2h_uuid_be,
    const uint8_t *psftp_h2d_uuid_be);

#ifdef __cplusplus
}
#endif

#endif // POLAR_SDK_BTSTACK_HELPERS_H
