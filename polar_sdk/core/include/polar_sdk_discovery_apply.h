// SPDX-License-Identifier: MIT
#ifndef POLAR_SDK_DISCOVERY_APPLY_H
#define POLAR_SDK_DISCOVERY_APPLY_H

#include <stdbool.h>
#include <stdint.h>

#include "polar_sdk_btstack_helpers.h"

#ifdef __cplusplus
extern "C" {
#endif

void polar_sdk_discovery_apply_service_kind(polar_sdk_disc_service_kind_t kind,
                                            bool *hr_service_found,
                                            bool *pmd_service_found,
                                            bool *psftp_service_found);

void polar_sdk_discovery_apply_char_kind(
    polar_sdk_disc_char_kind_t kind, uint16_t value_handle, bool *hr_char_found,
    uint16_t *hr_measurement_handle, bool *pmd_cp_char_found,
    uint16_t *pmd_cp_handle, bool *pmd_data_char_found,
    uint16_t *pmd_data_handle, uint16_t *psftp_mtu_handle,
    uint16_t *psftp_d2h_handle, uint16_t *psftp_h2d_handle);

#ifdef __cplusplus
}
#endif

#endif // POLAR_SDK_DISCOVERY_APPLY_H
