// SPDX-License-Identifier: MIT
#include "polar_ble_driver_pmd.h"

bool polar_ble_driver_pmd_att_status_requires_security(uint8_t att_status) {
    // ATT error codes from the BLE ATT spec.
    return att_status == 0x05 || // insufficient authentication
        att_status == 0x08 || // insufficient authorization
        att_status == 0x0C || // insufficient encryption key size
        att_status == 0x0F; // insufficient encryption
}

bool polar_ble_driver_pmd_security_ready(uint8_t encryption_key_size) {
    return encryption_key_size > 0;
}

bool polar_ble_driver_pmd_response_status_ok(uint8_t status) {
    return status == POLAR_BLE_DRIVER_PMD_RESPONSE_SUCCESS ||
        status == POLAR_BLE_DRIVER_PMD_RESPONSE_ALREADY_IN_STATE;
}

size_t polar_ble_driver_pmd_build_ecg_start_command(
    const polar_ble_driver_pmd_ecg_start_config_t *cfg,
    uint8_t *out,
    size_t out_capacity) {
    if (cfg == 0 || out == 0) {
        return 0;
    }

    // [opcode][type][settings...]
    // setting encoding: [setting_id][count=1][value LE]
    size_t needed = cfg->include_resolution ? 10u : 6u;
    if (out_capacity < needed) {
        return 0;
    }

    out[0] = POLAR_BLE_DRIVER_PMD_OPCODE_START_MEASUREMENT;
    out[1] = POLAR_BLE_DRIVER_PMD_MEASUREMENT_ECG;

    // sampleRate setting (type 0x00)
    out[2] = 0x00;
    out[3] = 0x01;
    out[4] = (uint8_t)(cfg->sample_rate & 0xffu);
    out[5] = (uint8_t)((cfg->sample_rate >> 8) & 0xffu);

    if (!cfg->include_resolution) {
        return 6u;
    }

    // resolution setting (type 0x01)
    out[6] = 0x01;
    out[7] = 0x01;
    out[8] = (uint8_t)(cfg->resolution & 0xffu);
    out[9] = (uint8_t)((cfg->resolution >> 8) & 0xffu);
    return 10u;
}

bool polar_ble_driver_pmd_parse_cp_response(
    const uint8_t *value,
    size_t value_len,
    polar_ble_driver_pmd_cp_response_t *out) {
    if (value == 0 || out == 0 || value_len < 4) {
        return false;
    }
    if (value[0] != POLAR_BLE_DRIVER_PMD_CP_RESPONSE_CODE) {
        return false;
    }

    out->opcode = value[1];
    out->measurement_type = value[2];
    out->status = value[3];
    out->has_more = value_len > 4;
    out->more = out->has_more ? value[4] : 0;
    return true;
}
