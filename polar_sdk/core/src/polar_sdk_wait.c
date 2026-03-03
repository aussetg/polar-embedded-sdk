// SPDX-License-Identifier: MIT
#include "polar_sdk_wait.h"

static bool polar_sdk_wait_ops_ready(const polar_sdk_wait_ops_t *ops) {
    return ops != 0 &&
        ops->now_ms != 0 &&
        ops->sleep_ms != 0 &&
        ops->is_done != 0 &&
        ops->is_connected != 0;
}

bool polar_sdk_wait_until_done_or_disconnect(
    const polar_sdk_wait_ops_t *ops,
    uint32_t timeout_ms,
    uint32_t poll_interval_ms) {
    if (!polar_sdk_wait_ops_ready(ops)) {
        return false;
    }
    if (poll_interval_ms == 0) {
        poll_interval_ms = 1;
    }

    uint32_t start_ms = ops->now_ms(ops->ctx);
    while ((uint32_t)(ops->now_ms(ops->ctx) - start_ms) < timeout_ms) {
        if (ops->is_done(ops->ctx)) {
            return true;
        }
        if (!ops->is_connected(ops->ctx)) {
            return false;
        }
        ops->sleep_ms(ops->ctx, poll_interval_ms);
    }

    return ops->is_done(ops->ctx);
}
