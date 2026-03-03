// SPDX-License-Identifier: MIT
#include "polar_sdk_btstack_gatt_route.h"

bool polar_sdk_btstack_route_gatt_event(
    uint8_t packet_type,
    uint8_t *packet,
    const polar_sdk_btstack_gatt_route_state_t *state,
    polar_sdk_btstack_gatt_route_result_t *out) {
    if (state == 0 || out == 0) {
        return false;
    }

    out->kind = POLAR_SDK_GATT_ROUTE_NONE;
    out->query_complete_att_status = 0;

    if (polar_sdk_btstack_decode_mtu_event(packet_type, packet, &out->mtu)) {
        out->kind = POLAR_SDK_GATT_ROUTE_MTU_EVENT;
        return true;
    }

    if (polar_sdk_btstack_decode_value_event(packet_type, packet, &out->value) &&
        state->connected &&
        state->conn_handle != 0xffff &&
        out->value.handle == state->conn_handle) {
        if (out->value.value_handle == state->hr_value_handle && state->hr_enabled) {
            out->kind = POLAR_SDK_GATT_ROUTE_HR_VALUE;
            return true;
        }
        if (out->value.value_handle == state->pmd_cp_value_handle && state->pmd_cp_listening) {
            out->kind = POLAR_SDK_GATT_ROUTE_PMD_CP_VALUE;
            return true;
        }
        if (out->value.value_handle == state->pmd_data_value_handle && state->ecg_enabled) {
            out->kind = POLAR_SDK_GATT_ROUTE_PMD_DATA_VALUE;
            return true;
        }
        if (out->value.value_handle == state->psftp_mtu_value_handle && state->psftp_mtu_listening) {
            out->kind = POLAR_SDK_GATT_ROUTE_PSFTP_MTU_VALUE;
            return true;
        }
        if (out->value.value_handle == state->psftp_d2h_value_handle && state->psftp_d2h_listening) {
            out->kind = POLAR_SDK_GATT_ROUTE_PSFTP_D2H_VALUE;
            return true;
        }
        if (state->hr_enabled) {
            out->kind = POLAR_SDK_GATT_ROUTE_HR_UNMATCHED_VALUE;
            return true;
        }
        return false;
    }

    if (polar_sdk_btstack_decode_query_complete_att_status(packet_type, packet, &out->query_complete_att_status)) {
        out->kind = POLAR_SDK_GATT_ROUTE_QUERY_COMPLETE;
        return true;
    }

    return false;
}
