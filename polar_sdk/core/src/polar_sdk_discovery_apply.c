// SPDX-License-Identifier: MIT
#include "polar_sdk_discovery_apply.h"

void polar_sdk_discovery_apply_service_kind(
    polar_sdk_disc_service_kind_t kind,
    bool *hr_service_found,
    bool *pmd_service_found,
    bool *psftp_service_found) {
    if (kind == POLAR_SDK_DISC_SERVICE_HR) {
        if (hr_service_found) {
            *hr_service_found = true;
        }
    } else if (kind == POLAR_SDK_DISC_SERVICE_PMD) {
        if (pmd_service_found) {
            *pmd_service_found = true;
        }
    } else if (kind == POLAR_SDK_DISC_SERVICE_PSFTP) {
        if (psftp_service_found) {
            *psftp_service_found = true;
        }
    }
}

void polar_sdk_discovery_apply_char_kind(
    polar_sdk_disc_char_kind_t kind,
    uint16_t value_handle,
    bool *hr_char_found,
    uint16_t *hr_measurement_handle,
    bool *pmd_cp_char_found,
    uint16_t *pmd_cp_handle,
    bool *pmd_data_char_found,
    uint16_t *pmd_data_handle,
    uint16_t *psftp_mtu_handle,
    uint16_t *psftp_d2h_handle,
    uint16_t *psftp_h2d_handle) {
    if (kind == POLAR_SDK_DISC_CHAR_HR_MEAS) {
        if (hr_char_found) {
            *hr_char_found = true;
        }
        if (hr_measurement_handle) {
            *hr_measurement_handle = value_handle;
        }
        return;
    }

    if (kind == POLAR_SDK_DISC_CHAR_PMD_CP) {
        if (pmd_cp_char_found) {
            *pmd_cp_char_found = true;
        }
        if (pmd_cp_handle) {
            *pmd_cp_handle = value_handle;
        }
        return;
    }

    if (kind == POLAR_SDK_DISC_CHAR_PMD_DATA) {
        if (pmd_data_char_found) {
            *pmd_data_char_found = true;
        }
        if (pmd_data_handle) {
            *pmd_data_handle = value_handle;
        }
        return;
    }

    if (kind == POLAR_SDK_DISC_CHAR_PSFTP_MTU) {
        if (psftp_mtu_handle) {
            *psftp_mtu_handle = value_handle;
        }
        return;
    }

    if (kind == POLAR_SDK_DISC_CHAR_PSFTP_D2H) {
        if (psftp_d2h_handle) {
            *psftp_d2h_handle = value_handle;
        }
        return;
    }

    if (kind == POLAR_SDK_DISC_CHAR_PSFTP_H2D) {
        if (psftp_h2d_handle) {
            *psftp_h2d_handle = value_handle;
        }
    }
}
