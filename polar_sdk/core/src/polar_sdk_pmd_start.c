// SPDX-License-Identifier: MIT
#include "polar_sdk_pmd.h"

#include "polar_sdk_security.h"

static bool polar_sdk_pmd_ops_ready(const polar_sdk_pmd_start_ops_t *ops) {
    return ops != 0 &&
        ops->is_connected != 0 &&
        ops->encryption_key_size != 0 &&
        ops->request_pairing != 0 &&
        ops->sleep_ms != 0 &&
        ops->enable_notifications != 0 &&
        ops->ensure_minimum_mtu != 0 &&
        ops->start_ecg_and_wait_response != 0;
}

static bool polar_sdk_pmd_security_is_connected(void *ctx) {
    const polar_sdk_pmd_start_ops_t *ops = (const polar_sdk_pmd_start_ops_t *)ctx;
    return ops->is_connected(ops->ctx);
}

static bool polar_sdk_pmd_security_is_secure(void *ctx) {
    const polar_sdk_pmd_start_ops_t *ops = (const polar_sdk_pmd_start_ops_t *)ctx;
    return polar_sdk_pmd_security_ready(ops->encryption_key_size(ops->ctx));
}

static void polar_sdk_pmd_security_request_pairing(void *ctx) {
    const polar_sdk_pmd_start_ops_t *ops = (const polar_sdk_pmd_start_ops_t *)ctx;
    ops->request_pairing(ops->ctx);
}

static void polar_sdk_pmd_security_sleep_ms(void *ctx, uint32_t ms) {
    const polar_sdk_pmd_start_ops_t *ops = (const polar_sdk_pmd_start_ops_t *)ctx;
    ops->sleep_ms(ops->ctx, ms);
}

static bool polar_sdk_pmd_ensure_security(
    const polar_sdk_pmd_start_policy_t *policy,
    const polar_sdk_pmd_start_ops_t *ops) {
    if (policy == 0 || ops == 0) {
        return false;
    }

    polar_sdk_security_policy_t security_policy = {
        .rounds = policy->security_rounds_per_attempt,
        .wait_ms_per_round = policy->security_wait_ms,
        .request_gap_ms = 120,
        .poll_ms = 20,
    };
    polar_sdk_security_ops_t security_ops = {
        .ctx = (void *)ops,
        .is_connected = polar_sdk_pmd_security_is_connected,
        .is_secure = polar_sdk_pmd_security_is_secure,
        .request_pairing = polar_sdk_pmd_security_request_pairing,
        .sleep_ms = polar_sdk_pmd_security_sleep_ms,
    };

    polar_sdk_security_result_t r = polar_sdk_security_request_with_retry(
        &security_policy,
        &security_ops);
    return r == POLAR_SDK_SECURITY_RESULT_OK;
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

        if (!polar_sdk_pmd_security_ready(ops->encryption_key_size(ops->ctx))) {
            (void)polar_sdk_pmd_ensure_security(policy, ops);
        }

        int notify_status = ops->enable_notifications(ops->ctx);
        if (notify_status == POLAR_SDK_PMD_OP_OK) {
            notify_ready = true;
            break;
        }

        if (notify_status > 0) {
            last_att_status = notify_status;
            if (polar_sdk_pmd_att_status_requires_security((uint8_t)notify_status)) {
                (void)polar_sdk_pmd_ensure_security(policy, ops);
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
        if (!polar_sdk_pmd_security_ready(ops->encryption_key_size(ops->ctx))) {
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
