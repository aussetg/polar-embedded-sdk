// SPDX-License-Identifier: MIT
#include "polar_ble_driver_pmd.h"

static bool polar_ble_driver_pmd_ops_ready(const polar_ble_driver_pmd_start_ops_t *ops) {
    return ops != 0 &&
        ops->is_connected != 0 &&
        ops->encryption_key_size != 0 &&
        ops->request_pairing != 0 &&
        ops->sleep_ms != 0 &&
        ops->enable_notifications != 0 &&
        ops->ensure_minimum_mtu != 0 &&
        ops->start_ecg_and_wait_response != 0;
}

static bool polar_ble_driver_pmd_wait_for_security(
    const polar_ble_driver_pmd_start_ops_t *ops,
    uint32_t timeout_ms) {
    uint32_t elapsed = 0;
    while (elapsed < timeout_ms) {
        if (!ops->is_connected(ops->ctx)) {
            return false;
        }
        if (polar_ble_driver_pmd_security_ready(ops->encryption_key_size(ops->ctx))) {
            return true;
        }
        ops->sleep_ms(ops->ctx, 20);
        elapsed += 20;
    }
    return ops->is_connected(ops->ctx) &&
        polar_ble_driver_pmd_security_ready(ops->encryption_key_size(ops->ctx));
}

static bool polar_ble_driver_pmd_ensure_security(
    const polar_ble_driver_pmd_start_policy_t *policy,
    const polar_ble_driver_pmd_start_ops_t *ops) {
    if (polar_ble_driver_pmd_security_ready(ops->encryption_key_size(ops->ctx))) {
        return true;
    }

    for (size_t r = 0; r < policy->security_rounds_per_attempt; ++r) {
        if (!ops->is_connected(ops->ctx)) {
            return false;
        }
        ops->request_pairing(ops->ctx);
        ops->sleep_ms(ops->ctx, 120);
        ops->request_pairing(ops->ctx);

        if (polar_ble_driver_pmd_wait_for_security(ops, policy->security_wait_ms)) {
            return true;
        }
    }

    return polar_ble_driver_pmd_security_ready(ops->encryption_key_size(ops->ctx));
}

polar_ble_driver_pmd_start_result_t polar_ble_driver_pmd_start_ecg_with_policy(
    const polar_ble_driver_pmd_start_policy_t *policy,
    const polar_ble_driver_pmd_start_ops_t *ops,
    uint8_t *out_pmd_response_status,
    int *out_last_ccc_att_status) {
    if (policy == 0 || !polar_ble_driver_pmd_ops_ready(ops)) {
        return POLAR_BLE_DRIVER_PMD_START_RESULT_TRANSPORT_ERROR;
    }

    if (out_pmd_response_status != 0) {
        *out_pmd_response_status = 0xff;
    }
    if (out_last_ccc_att_status != 0) {
        *out_last_ccc_att_status = 0;
    }

    if (!ops->is_connected(ops->ctx)) {
        return POLAR_BLE_DRIVER_PMD_START_RESULT_NOT_CONNECTED;
    }

    size_t ccc_attempts = policy->ccc_attempts == 0 ? 1 : policy->ccc_attempts;
    int last_att_status = 0;
    bool notify_ready = false;

    for (size_t i = 0; i < ccc_attempts; ++i) {
        if (!ops->is_connected(ops->ctx)) {
            return POLAR_BLE_DRIVER_PMD_START_RESULT_NOT_CONNECTED;
        }

        if (!polar_ble_driver_pmd_security_ready(ops->encryption_key_size(ops->ctx))) {
            (void)polar_ble_driver_pmd_ensure_security(policy, ops);
        }

        int notify_status = ops->enable_notifications(ops->ctx);
        if (notify_status == POLAR_BLE_DRIVER_PMD_OP_OK) {
            notify_ready = true;
            break;
        }

        if (notify_status > 0) {
            last_att_status = notify_status;
            if (polar_ble_driver_pmd_att_status_requires_security((uint8_t)notify_status)) {
                (void)polar_ble_driver_pmd_ensure_security(policy, ops);
                continue;
            }
            if (out_last_ccc_att_status != 0) {
                *out_last_ccc_att_status = last_att_status;
            }
            return POLAR_BLE_DRIVER_PMD_START_RESULT_CCC_REJECTED;
        }

        if (notify_status == POLAR_BLE_DRIVER_PMD_OP_NOT_CONNECTED) {
            return POLAR_BLE_DRIVER_PMD_START_RESULT_NOT_CONNECTED;
        }
        if (notify_status == POLAR_BLE_DRIVER_PMD_OP_TIMEOUT) {
            if (out_last_ccc_att_status != 0) {
                *out_last_ccc_att_status = last_att_status;
            }
            return POLAR_BLE_DRIVER_PMD_START_RESULT_CCC_TIMEOUT;
        }
        return POLAR_BLE_DRIVER_PMD_START_RESULT_TRANSPORT_ERROR;
    }

    if (!notify_ready) {
        if (out_last_ccc_att_status != 0) {
            *out_last_ccc_att_status = last_att_status;
        }
        if (!ops->is_connected(ops->ctx)) {
            return POLAR_BLE_DRIVER_PMD_START_RESULT_NOT_CONNECTED;
        }
        if (!polar_ble_driver_pmd_security_ready(ops->encryption_key_size(ops->ctx))) {
            return POLAR_BLE_DRIVER_PMD_START_RESULT_SECURITY_TIMEOUT;
        }
        if (last_att_status > 0) {
            return POLAR_BLE_DRIVER_PMD_START_RESULT_CCC_REJECTED;
        }
        return POLAR_BLE_DRIVER_PMD_START_RESULT_CCC_TIMEOUT;
    }

    if (ops->ensure_minimum_mtu(ops->ctx, policy->minimum_mtu) != POLAR_BLE_DRIVER_PMD_OP_OK) {
        if (!ops->is_connected(ops->ctx)) {
            return POLAR_BLE_DRIVER_PMD_START_RESULT_NOT_CONNECTED;
        }
        return POLAR_BLE_DRIVER_PMD_START_RESULT_MTU_FAILED;
    }

    uint8_t start_cmd[16];
    polar_ble_driver_pmd_ecg_start_config_t cfg = {
        .sample_rate = policy->sample_rate,
        .include_resolution = policy->include_resolution,
        .resolution = policy->resolution,
    };
    size_t start_cmd_len = polar_ble_driver_pmd_build_ecg_start_command(&cfg, start_cmd, sizeof(start_cmd));
    if (start_cmd_len == 0) {
        return POLAR_BLE_DRIVER_PMD_START_RESULT_TRANSPORT_ERROR;
    }

    uint8_t response_status = 0xff;
    int start_status = ops->start_ecg_and_wait_response(ops->ctx, start_cmd, start_cmd_len, &response_status);
    if (start_status != POLAR_BLE_DRIVER_PMD_OP_OK) {
        if (start_status == POLAR_BLE_DRIVER_PMD_OP_NOT_CONNECTED) {
            return POLAR_BLE_DRIVER_PMD_START_RESULT_NOT_CONNECTED;
        }
        if (start_status == POLAR_BLE_DRIVER_PMD_OP_TIMEOUT) {
            return POLAR_BLE_DRIVER_PMD_START_RESULT_START_TIMEOUT;
        }
        return POLAR_BLE_DRIVER_PMD_START_RESULT_TRANSPORT_ERROR;
    }

    if (out_pmd_response_status != 0) {
        *out_pmd_response_status = response_status;
    }
    if (!polar_ble_driver_pmd_response_status_ok(response_status)) {
        return POLAR_BLE_DRIVER_PMD_START_RESULT_START_REJECTED;
    }

    return POLAR_BLE_DRIVER_PMD_START_RESULT_OK;
}
