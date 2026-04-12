// SPDX-License-Identifier: MIT
#include "polar_sdk_btstack_security.h"

#include "polar_sdk_btstack_sm.h"

#include "btstack.h"

typedef struct {
    const uint16_t *conn_handle;
    uint16_t invalid_conn_handle;
    polar_sdk_btstack_security_sleep_ms_fn_t sleep_ms;
    void *sleep_ctx;
} polar_sdk_btstack_security_ctx_t;

static uint16_t polar_sdk_btstack_security_current_handle(
    const polar_sdk_btstack_security_ctx_t *state) {
    if (state == 0 || state->conn_handle == 0) {
        return state == 0 ? 0u : state->invalid_conn_handle;
    }
    return *state->conn_handle;
}

uint8_t polar_sdk_btstack_security_encryption_key_size(
    uint16_t conn_handle,
    uint16_t invalid_conn_handle) {
    if (conn_handle == invalid_conn_handle) {
        return 0u;
    }
    return gap_encryption_key_size(conn_handle);
}

bool polar_sdk_btstack_security_ready(
    uint16_t conn_handle,
    uint16_t invalid_conn_handle) {
    return polar_sdk_btstack_security_encryption_key_size(conn_handle, invalid_conn_handle) > 0u;
}

void polar_sdk_btstack_security_request_pairing(
    uint16_t conn_handle,
    uint16_t invalid_conn_handle) {
    if (conn_handle == invalid_conn_handle) {
        return;
    }
    polar_sdk_btstack_sm_configure_default_central_policy();
    sm_request_pairing(conn_handle);
}

static bool polar_sdk_btstack_security_is_connected_cb(const void *ctx) {
    const polar_sdk_btstack_security_ctx_t *state = (const polar_sdk_btstack_security_ctx_t *)ctx;
    return state != 0 &&
        polar_sdk_btstack_security_current_handle(state) != state->invalid_conn_handle;
}

static bool polar_sdk_btstack_security_is_secure_cb(const void *ctx) {
    const polar_sdk_btstack_security_ctx_t *state = (const polar_sdk_btstack_security_ctx_t *)ctx;
    return state != 0 && polar_sdk_btstack_security_ready(
        polar_sdk_btstack_security_current_handle(state),
        state->invalid_conn_handle);
}

static void polar_sdk_btstack_security_request_pairing_cb(const void *ctx) {
    const polar_sdk_btstack_security_ctx_t *state = (const polar_sdk_btstack_security_ctx_t *)ctx;
    uint16_t conn_handle = polar_sdk_btstack_security_current_handle(state);
    if (state == 0 || conn_handle == state->invalid_conn_handle) {
        return;
    }
    sm_request_pairing(conn_handle);
}

static void polar_sdk_btstack_security_sleep_ms_cb(const void *ctx, uint32_t ms) {
    const polar_sdk_btstack_security_ctx_t *state = (const polar_sdk_btstack_security_ctx_t *)ctx;
    if (state == 0 || state->sleep_ms == 0) {
        return;
    }
    state->sleep_ms(state->sleep_ctx, ms);
}

polar_sdk_security_result_t polar_sdk_btstack_security_ensure(
    const uint16_t *conn_handle,
    uint16_t invalid_conn_handle,
    const polar_sdk_security_policy_t *policy,
    polar_sdk_btstack_security_sleep_ms_fn_t sleep_ms,
    void *sleep_ctx) {
    if (policy == 0 || sleep_ms == 0) {
        return POLAR_SDK_SECURITY_RESULT_INVALID_ARGS;
    }
    if (conn_handle == 0 || *conn_handle == invalid_conn_handle) {
        return POLAR_SDK_SECURITY_RESULT_NOT_CONNECTED;
    }
    if (polar_sdk_btstack_security_ready(*conn_handle, invalid_conn_handle)) {
        return POLAR_SDK_SECURITY_RESULT_OK;
    }

    polar_sdk_btstack_security_ctx_t ctx = {
        .conn_handle = conn_handle,
        .invalid_conn_handle = invalid_conn_handle,
        .sleep_ms = sleep_ms,
        .sleep_ctx = sleep_ctx,
    };
    polar_sdk_security_ops_t ops = {
        .ctx = &ctx,
        .is_connected = polar_sdk_btstack_security_is_connected_cb,
        .is_secure = polar_sdk_btstack_security_is_secure_cb,
        .request_pairing = polar_sdk_btstack_security_request_pairing_cb,
        .sleep_ms = polar_sdk_btstack_security_sleep_ms_cb,
    };

    polar_sdk_btstack_sm_configure_default_central_policy();
    return polar_sdk_security_request_with_retry(policy, &ops);
}