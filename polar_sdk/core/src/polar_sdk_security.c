// SPDX-License-Identifier: MIT
#include "polar_sdk_security.h"

static bool polar_sdk_security_ops_ready(const polar_sdk_security_ops_t *ops) {
    return ops != 0 &&
        ops->is_connected != 0 &&
        ops->is_secure != 0 &&
        ops->request_pairing != 0 &&
        ops->sleep_ms != 0;
}

polar_sdk_security_result_t polar_sdk_security_request_with_retry(
    const polar_sdk_security_policy_t *policy,
    const polar_sdk_security_ops_t *ops) {
    if (policy == 0 || !polar_sdk_security_ops_ready(ops)) {
        return POLAR_SDK_SECURITY_RESULT_INVALID_ARGS;
    }

    if (ops->is_secure(ops->ctx)) {
        return POLAR_SDK_SECURITY_RESULT_OK;
    }

    size_t rounds = policy->rounds;
    uint32_t poll_ms = policy->poll_ms == 0 ? 20 : policy->poll_ms;

    for (size_t round = 0; round < rounds; ++round) {
        if (!ops->is_connected(ops->ctx)) {
            return POLAR_SDK_SECURITY_RESULT_NOT_CONNECTED;
        }

        ops->request_pairing(ops->ctx);
        if (policy->request_gap_ms > 0) {
            ops->sleep_ms(ops->ctx, policy->request_gap_ms);
        }
        ops->request_pairing(ops->ctx);

        uint32_t elapsed_ms = 0;
        while (elapsed_ms < policy->wait_ms_per_round) {
            if (!ops->is_connected(ops->ctx)) {
                return POLAR_SDK_SECURITY_RESULT_NOT_CONNECTED;
            }
            if (ops->is_secure(ops->ctx)) {
                return POLAR_SDK_SECURITY_RESULT_OK;
            }

            ops->sleep_ms(ops->ctx, poll_ms);
            elapsed_ms += poll_ms;
        }

        if (ops->is_secure(ops->ctx)) {
            return POLAR_SDK_SECURITY_RESULT_OK;
        }
    }

    if (!ops->is_connected(ops->ctx)) {
        return POLAR_SDK_SECURITY_RESULT_NOT_CONNECTED;
    }
    if (ops->is_secure(ops->ctx)) {
        return POLAR_SDK_SECURITY_RESULT_OK;
    }
    return POLAR_SDK_SECURITY_RESULT_TIMEOUT;
}
