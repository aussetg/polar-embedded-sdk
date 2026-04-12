// SPDX-License-Identifier: MIT
#include "polar_sdk_pmd.h"

static bool polar_sdk_pmd_ops_ready(const polar_sdk_pmd_start_ops_t *ops) {
    return ops != 0 &&
        ops->is_connected != 0 &&
        ops->security_ready != 0 &&
        ops->ensure_security != 0 &&
        ops->enable_notifications != 0 &&
        ops->ensure_minimum_mtu != 0 &&
        ops->start_ecg_and_wait_response != 0;
}

static polar_sdk_pmd_start_result_t polar_sdk_pmd_map_security_result(
    polar_sdk_security_result_t security_result) {
    if (security_result == POLAR_SDK_SECURITY_RESULT_OK) {
        return POLAR_SDK_PMD_START_RESULT_OK;
    }
    if (security_result == POLAR_SDK_SECURITY_RESULT_NOT_CONNECTED) {
        return POLAR_SDK_PMD_START_RESULT_NOT_CONNECTED;
    }
    return POLAR_SDK_PMD_START_RESULT_SECURITY_TIMEOUT;
}

static polar_sdk_pmd_start_result_t polar_sdk_pmd_require_security(
    const polar_sdk_pmd_start_ops_t *ops) {
    if (ops == 0) {
        return POLAR_SDK_PMD_START_RESULT_TRANSPORT_ERROR;
    }
    if (!ops->is_connected(ops->ctx)) {
        return POLAR_SDK_PMD_START_RESULT_NOT_CONNECTED;
    }
    if (ops->security_ready(ops->ctx)) {
        return POLAR_SDK_PMD_START_RESULT_OK;
    }

    polar_sdk_pmd_start_result_t mapped = polar_sdk_pmd_map_security_result(
        ops->ensure_security(ops->ctx));
    if (mapped != POLAR_SDK_PMD_START_RESULT_OK) {
        return mapped;
    }
    if (!ops->is_connected(ops->ctx)) {
        return POLAR_SDK_PMD_START_RESULT_NOT_CONNECTED;
    }
    if (!ops->security_ready(ops->ctx)) {
        return POLAR_SDK_PMD_START_RESULT_SECURITY_TIMEOUT;
    }
    return POLAR_SDK_PMD_START_RESULT_OK;
}

static polar_sdk_pmd_start_result_t polar_sdk_pmd_recover_security(
    const polar_sdk_pmd_start_ops_t *ops) {
    if (ops == 0) {
        return POLAR_SDK_PMD_START_RESULT_TRANSPORT_ERROR;
    }
    if (!ops->is_connected(ops->ctx)) {
        return POLAR_SDK_PMD_START_RESULT_NOT_CONNECTED;
    }

    polar_sdk_pmd_start_result_t mapped = polar_sdk_pmd_map_security_result(
        ops->ensure_security(ops->ctx));
    if (mapped != POLAR_SDK_PMD_START_RESULT_OK) {
        return mapped;
    }
    if (!ops->is_connected(ops->ctx)) {
        return POLAR_SDK_PMD_START_RESULT_NOT_CONNECTED;
    }
    if (!ops->security_ready(ops->ctx)) {
        return POLAR_SDK_PMD_START_RESULT_SECURITY_TIMEOUT;
    }
    return POLAR_SDK_PMD_START_RESULT_OK;
}

static polar_sdk_pmd_start_result_t polar_sdk_pmd_start_with_command(
    const polar_sdk_pmd_start_policy_t *policy,
    const polar_sdk_pmd_start_ops_t *ops,
    const uint8_t *start_cmd,
    size_t start_cmd_len,
    uint8_t *out_pmd_response_status,
    int *out_last_ccc_att_status) {
    if (policy == 0 || !polar_sdk_pmd_ops_ready(ops) || start_cmd == 0 || start_cmd_len == 0) {
        return POLAR_SDK_PMD_START_RESULT_TRANSPORT_ERROR;
    }

    if (out_pmd_response_status != 0) {
        *out_pmd_response_status = 0xff;
    }
    if (out_last_ccc_att_status != 0) {
        *out_last_ccc_att_status = 0;
    }

    if (!ops->is_connected(ops->ctx)) {
        return POLAR_SDK_PMD_START_RESULT_NOT_CONNECTED;
    }

    size_t ccc_attempts = policy->ccc_attempts == 0 ? 1 : policy->ccc_attempts;
    int last_att_status = 0;
    bool notify_ready = false;

    for (size_t i = 0; i < ccc_attempts; ++i) {
        if (!ops->is_connected(ops->ctx)) {
            return POLAR_SDK_PMD_START_RESULT_NOT_CONNECTED;
        }

        polar_sdk_pmd_start_result_t security_result = polar_sdk_pmd_require_security(ops);
        if (security_result != POLAR_SDK_PMD_START_RESULT_OK) {
            return security_result;
        }

        int notify_status = ops->enable_notifications(ops->ctx);
        if (notify_status == POLAR_SDK_PMD_OP_OK) {
            notify_ready = true;
            break;
        }

        if (notify_status > 0) {
            last_att_status = notify_status;
            if (polar_sdk_pmd_att_status_requires_security((uint8_t)notify_status)) {
                security_result = polar_sdk_pmd_recover_security(ops);
                if (security_result != POLAR_SDK_PMD_START_RESULT_OK) {
                    if (out_last_ccc_att_status != 0) {
                        *out_last_ccc_att_status = last_att_status;
                    }
                    return security_result;
                }
                continue;
            }
            if (out_last_ccc_att_status != 0) {
                *out_last_ccc_att_status = last_att_status;
            }
            return POLAR_SDK_PMD_START_RESULT_CCC_REJECTED;
        }

        if (notify_status == POLAR_SDK_PMD_OP_NOT_CONNECTED) {
            return POLAR_SDK_PMD_START_RESULT_NOT_CONNECTED;
        }
        if (notify_status == POLAR_SDK_PMD_OP_TIMEOUT) {
            if (out_last_ccc_att_status != 0) {
                *out_last_ccc_att_status = last_att_status;
            }
            return POLAR_SDK_PMD_START_RESULT_CCC_TIMEOUT;
        }
        return POLAR_SDK_PMD_START_RESULT_TRANSPORT_ERROR;
    }

    if (!notify_ready) {
        if (out_last_ccc_att_status != 0) {
            *out_last_ccc_att_status = last_att_status;
        }
        if (!ops->is_connected(ops->ctx)) {
            return POLAR_SDK_PMD_START_RESULT_NOT_CONNECTED;
        }
        if (!ops->security_ready(ops->ctx)) {
            return POLAR_SDK_PMD_START_RESULT_SECURITY_TIMEOUT;
        }
        if (last_att_status > 0) {
            return POLAR_SDK_PMD_START_RESULT_CCC_REJECTED;
        }
        return POLAR_SDK_PMD_START_RESULT_CCC_TIMEOUT;
    }

    if (ops->ensure_minimum_mtu(ops->ctx, policy->minimum_mtu) != POLAR_SDK_PMD_OP_OK) {
        if (!ops->is_connected(ops->ctx)) {
            return POLAR_SDK_PMD_START_RESULT_NOT_CONNECTED;
        }
        return POLAR_SDK_PMD_START_RESULT_MTU_FAILED;
    }

    uint8_t response_status = 0xff;
    int start_status = ops->start_ecg_and_wait_response(ops->ctx, start_cmd, start_cmd_len, &response_status);
    if (start_status != POLAR_SDK_PMD_OP_OK) {
        if (start_status == POLAR_SDK_PMD_OP_NOT_CONNECTED) {
            return POLAR_SDK_PMD_START_RESULT_NOT_CONNECTED;
        }
        if (start_status == POLAR_SDK_PMD_OP_TIMEOUT) {
            return POLAR_SDK_PMD_START_RESULT_START_TIMEOUT;
        }
        return POLAR_SDK_PMD_START_RESULT_TRANSPORT_ERROR;
    }

    if (out_pmd_response_status != 0) {
        *out_pmd_response_status = response_status;
    }
    if (!polar_sdk_pmd_response_status_ok(response_status)) {
        return POLAR_SDK_PMD_START_RESULT_START_REJECTED;
    }

    return POLAR_SDK_PMD_START_RESULT_OK;
}

polar_sdk_pmd_start_result_t polar_sdk_pmd_start_ecg_with_policy(
    const polar_sdk_pmd_start_policy_t *policy,
    const polar_sdk_pmd_start_ops_t *ops,
    uint8_t *out_pmd_response_status,
    int *out_last_ccc_att_status) {
    uint8_t start_cmd[16];
    polar_sdk_pmd_ecg_start_config_t cfg = {
        .sample_rate = policy == 0 ? 0 : policy->sample_rate,
        .include_resolution = policy != 0 && policy->include_resolution,
        .resolution = policy == 0 ? 0 : policy->resolution,
    };
    size_t start_cmd_len = polar_sdk_pmd_build_ecg_start_command(&cfg, start_cmd, sizeof(start_cmd));
    if (start_cmd_len == 0) {
        return POLAR_SDK_PMD_START_RESULT_TRANSPORT_ERROR;
    }

    return polar_sdk_pmd_start_with_command(
        policy,
        ops,
        start_cmd,
        start_cmd_len,
        out_pmd_response_status,
        out_last_ccc_att_status);
}

polar_sdk_pmd_start_result_t polar_sdk_pmd_start_acc_with_policy(
    const polar_sdk_pmd_start_policy_t *policy,
    const polar_sdk_pmd_start_ops_t *ops,
    uint8_t *out_pmd_response_status,
    int *out_last_ccc_att_status) {
    uint8_t start_cmd[20];
    polar_sdk_pmd_acc_start_config_t cfg = {
        .sample_rate = policy == 0 ? 0 : policy->sample_rate,
        .include_resolution = policy != 0 && policy->include_resolution,
        .resolution = policy == 0 ? 0 : policy->resolution,
        .include_range = policy != 0 && policy->include_range,
        .range = policy == 0 ? 0 : policy->range,
    };
    size_t start_cmd_len = polar_sdk_pmd_build_acc_start_command(&cfg, start_cmd, sizeof(start_cmd));
    if (start_cmd_len == 0) {
        return POLAR_SDK_PMD_START_RESULT_TRANSPORT_ERROR;
    }

    return polar_sdk_pmd_start_with_command(
        policy,
        ops,
        start_cmd,
        start_cmd_len,
        out_pmd_response_status,
        out_last_ccc_att_status);
}
