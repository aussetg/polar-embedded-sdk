// SPDX-License-Identifier: MIT
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "polar_sdk_pmd.h"

#define ASSERT_TRUE(expr) do { if (!(expr)) fail(__LINE__, #expr); } while (0)
#define ASSERT_EQ_INT(expected, actual) do { int _e = (expected); int _a = (actual); if (_e != _a) fail_int(__LINE__, #actual, _e, _a); } while (0)

typedef struct {
    bool connected;
    bool security_ready;
    polar_sdk_security_result_t ensure_security_result;
    bool security_ready_after_ensure;
    bool disconnect_on_ensure;
    size_t ensure_security_calls;

    int notify_results[4];
    size_t notify_result_count;
    size_t notify_index;
    size_t notify_calls;

    int ensure_mtu_result;
    size_t ensure_mtu_calls;

    int start_result;
    uint8_t start_status;
    size_t start_calls;
} fake_ctx_t;

static void fail(int line, const char *expr) {
    fprintf(stderr, "test_pmd_start:%d assertion failed: %s\n", line, expr);
    exit(1);
}

static void fail_int(int line, const char *expr, int expected, int actual) {
    fprintf(stderr, "test_pmd_start:%d assertion failed: %s expected=%d actual=%d\n", line, expr, expected, actual);
    exit(1);
}

static bool fake_is_connected(void *ctx) {
    fake_ctx_t *state = (fake_ctx_t *)ctx;
    return state != NULL && state->connected;
}

static bool fake_security_ready(void *ctx) {
    fake_ctx_t *state = (fake_ctx_t *)ctx;
    return state != NULL && state->connected && state->security_ready;
}

static polar_sdk_security_result_t fake_ensure_security(void *ctx) {
    fake_ctx_t *state = (fake_ctx_t *)ctx;
    state->ensure_security_calls += 1u;
    if (state->disconnect_on_ensure) {
        state->connected = false;
        state->security_ready = false;
    } else {
        state->security_ready = state->security_ready_after_ensure;
    }
    return state->ensure_security_result;
}

static int fake_enable_notifications(void *ctx) {
    fake_ctx_t *state = (fake_ctx_t *)ctx;
    state->notify_calls += 1u;
    if (state->notify_index >= state->notify_result_count) {
        return POLAR_SDK_PMD_OP_TRANSPORT;
    }
    return state->notify_results[state->notify_index++];
}

static int fake_ensure_minimum_mtu(void *ctx, uint16_t minimum_mtu) {
    (void)minimum_mtu;
    fake_ctx_t *state = (fake_ctx_t *)ctx;
    state->ensure_mtu_calls += 1u;
    return state->ensure_mtu_result;
}

static int fake_start_ecg_and_wait_response(
    void *ctx,
    const uint8_t *start_cmd,
    size_t start_cmd_len,
    uint8_t *out_status) {
    (void)start_cmd;
    (void)start_cmd_len;
    fake_ctx_t *state = (fake_ctx_t *)ctx;
    state->start_calls += 1u;
    if (out_status != NULL) {
        *out_status = state->start_status;
    }
    return state->start_result;
}

static polar_sdk_pmd_start_policy_t test_policy(void) {
    polar_sdk_pmd_start_policy_t policy = {
        .ccc_attempts = 3u,
        .minimum_mtu = 70u,
        .sample_rate = 130u,
        .include_resolution = true,
        .resolution = 14u,
        .include_range = false,
        .range = 0u,
    };
    return policy;
}

static polar_sdk_pmd_start_ops_t test_ops(fake_ctx_t *ctx) {
    polar_sdk_pmd_start_ops_t ops = {
        .ctx = ctx,
        .is_connected = fake_is_connected,
        .security_ready = fake_security_ready,
        .ensure_security = fake_ensure_security,
        .enable_notifications = fake_enable_notifications,
        .ensure_minimum_mtu = fake_ensure_minimum_mtu,
        .start_ecg_and_wait_response = fake_start_ecg_and_wait_response,
    };
    return ops;
}

static fake_ctx_t make_base_ctx(void) {
    fake_ctx_t ctx = {
        .connected = true,
        .security_ready = true,
        .ensure_security_result = POLAR_SDK_SECURITY_RESULT_OK,
        .security_ready_after_ensure = true,
        .disconnect_on_ensure = false,
        .notify_results = { POLAR_SDK_PMD_OP_OK, 0, 0, 0 },
        .notify_result_count = 1u,
        .notify_index = 0u,
        .notify_calls = 0u,
        .ensure_mtu_result = POLAR_SDK_PMD_OP_OK,
        .ensure_mtu_calls = 0u,
        .start_result = POLAR_SDK_PMD_OP_OK,
        .start_status = POLAR_SDK_PMD_RESPONSE_SUCCESS,
        .start_calls = 0u,
    };
    return ctx;
}

static void test_already_secure_success(void) {
    fake_ctx_t ctx = make_base_ctx();
    polar_sdk_pmd_start_policy_t policy = test_policy();
    polar_sdk_pmd_start_ops_t ops = test_ops(&ctx);

    int last_ccc_att = -1;
    uint8_t response_status = 0xffu;
    polar_sdk_pmd_start_result_t r = polar_sdk_pmd_start_ecg_with_policy(&policy, &ops, &response_status, &last_ccc_att);

    ASSERT_EQ_INT(POLAR_SDK_PMD_START_RESULT_OK, r);
    ASSERT_EQ_INT(0, (int)ctx.ensure_security_calls);
    ASSERT_EQ_INT(1, (int)ctx.notify_calls);
    ASSERT_EQ_INT(1, (int)ctx.ensure_mtu_calls);
    ASSERT_EQ_INT(1, (int)ctx.start_calls);
    ASSERT_EQ_INT(POLAR_SDK_PMD_RESPONSE_SUCCESS, response_status);
    ASSERT_EQ_INT(0, last_ccc_att);
}

static void test_initial_security_recovery_success(void) {
    fake_ctx_t ctx = make_base_ctx();
    ctx.security_ready = false;
    ctx.security_ready_after_ensure = true;

    polar_sdk_pmd_start_policy_t policy = test_policy();
    polar_sdk_pmd_start_ops_t ops = test_ops(&ctx);

    polar_sdk_pmd_start_result_t r = polar_sdk_pmd_start_ecg_with_policy(&policy, &ops, NULL, NULL);

    ASSERT_EQ_INT(POLAR_SDK_PMD_START_RESULT_OK, r);
    ASSERT_EQ_INT(1, (int)ctx.ensure_security_calls);
    ASSERT_EQ_INT(1, (int)ctx.notify_calls);
}

static void test_initial_security_timeout_blocks_ccc(void) {
    fake_ctx_t ctx = make_base_ctx();
    ctx.security_ready = false;
    ctx.security_ready_after_ensure = false;
    ctx.ensure_security_result = POLAR_SDK_SECURITY_RESULT_TIMEOUT;

    polar_sdk_pmd_start_policy_t policy = test_policy();
    polar_sdk_pmd_start_ops_t ops = test_ops(&ctx);

    polar_sdk_pmd_start_result_t r = polar_sdk_pmd_start_ecg_with_policy(&policy, &ops, NULL, NULL);

    ASSERT_EQ_INT(POLAR_SDK_PMD_START_RESULT_SECURITY_TIMEOUT, r);
    ASSERT_EQ_INT(1, (int)ctx.ensure_security_calls);
    ASSERT_EQ_INT(0, (int)ctx.notify_calls);
    ASSERT_EQ_INT(0, (int)ctx.ensure_mtu_calls);
    ASSERT_EQ_INT(0, (int)ctx.start_calls);
}

static void test_initial_security_ok_without_ready_is_still_timeout(void) {
    fake_ctx_t ctx = make_base_ctx();
    ctx.security_ready = false;
    ctx.security_ready_after_ensure = false;
    ctx.ensure_security_result = POLAR_SDK_SECURITY_RESULT_OK;

    polar_sdk_pmd_start_policy_t policy = test_policy();
    polar_sdk_pmd_start_ops_t ops = test_ops(&ctx);

    polar_sdk_pmd_start_result_t r = polar_sdk_pmd_start_ecg_with_policy(&policy, &ops, NULL, NULL);

    ASSERT_EQ_INT(POLAR_SDK_PMD_START_RESULT_SECURITY_TIMEOUT, r);
    ASSERT_EQ_INT(1, (int)ctx.ensure_security_calls);
    ASSERT_EQ_INT(0, (int)ctx.notify_calls);
}

static void test_security_att_recovery_success(void) {
    fake_ctx_t ctx = make_base_ctx();
    ctx.notify_results[0] = 0x0fu;
    ctx.notify_results[1] = POLAR_SDK_PMD_OP_OK;
    ctx.notify_result_count = 2u;
    ctx.ensure_security_result = POLAR_SDK_SECURITY_RESULT_OK;
    ctx.security_ready_after_ensure = true;

    polar_sdk_pmd_start_policy_t policy = test_policy();
    polar_sdk_pmd_start_ops_t ops = test_ops(&ctx);

    int last_ccc_att = -1;
    polar_sdk_pmd_start_result_t r = polar_sdk_pmd_start_ecg_with_policy(&policy, &ops, NULL, &last_ccc_att);

    ASSERT_EQ_INT(POLAR_SDK_PMD_START_RESULT_OK, r);
    ASSERT_EQ_INT(1, (int)ctx.ensure_security_calls);
    ASSERT_EQ_INT(2, (int)ctx.notify_calls);
    ASSERT_EQ_INT(0, last_ccc_att);
}

static void test_security_att_recovery_failure_reports_security_timeout(void) {
    fake_ctx_t ctx = make_base_ctx();
    ctx.notify_results[0] = 0x05u;
    ctx.notify_result_count = 1u;
    ctx.ensure_security_result = POLAR_SDK_SECURITY_RESULT_TIMEOUT;
    ctx.security_ready_after_ensure = false;

    polar_sdk_pmd_start_policy_t policy = test_policy();
    polar_sdk_pmd_start_ops_t ops = test_ops(&ctx);

    int last_ccc_att = 0;
    polar_sdk_pmd_start_result_t r = polar_sdk_pmd_start_ecg_with_policy(&policy, &ops, NULL, &last_ccc_att);

    ASSERT_EQ_INT(POLAR_SDK_PMD_START_RESULT_SECURITY_TIMEOUT, r);
    ASSERT_EQ_INT(1, (int)ctx.ensure_security_calls);
    ASSERT_EQ_INT(1, (int)ctx.notify_calls);
    ASSERT_EQ_INT(0x05, last_ccc_att);
}

static void test_security_disconnect_reports_not_connected(void) {
    fake_ctx_t ctx = make_base_ctx();
    ctx.security_ready = false;
    ctx.disconnect_on_ensure = true;
    ctx.ensure_security_result = POLAR_SDK_SECURITY_RESULT_NOT_CONNECTED;

    polar_sdk_pmd_start_policy_t policy = test_policy();
    polar_sdk_pmd_start_ops_t ops = test_ops(&ctx);

    polar_sdk_pmd_start_result_t r = polar_sdk_pmd_start_ecg_with_policy(&policy, &ops, NULL, NULL);

    ASSERT_EQ_INT(POLAR_SDK_PMD_START_RESULT_NOT_CONNECTED, r);
    ASSERT_EQ_INT(1, (int)ctx.ensure_security_calls);
    ASSERT_EQ_INT(0, (int)ctx.notify_calls);
}

static void test_true_ccc_timeout_stays_ccc_timeout(void) {
    fake_ctx_t ctx = make_base_ctx();
    ctx.notify_results[0] = POLAR_SDK_PMD_OP_TIMEOUT;
    ctx.notify_result_count = 1u;

    polar_sdk_pmd_start_policy_t policy = test_policy();
    polar_sdk_pmd_start_ops_t ops = test_ops(&ctx);

    polar_sdk_pmd_start_result_t r = polar_sdk_pmd_start_ecg_with_policy(&policy, &ops, NULL, NULL);

    ASSERT_EQ_INT(POLAR_SDK_PMD_START_RESULT_CCC_TIMEOUT, r);
    ASSERT_EQ_INT(0, (int)ctx.ensure_security_calls);
    ASSERT_EQ_INT(1, (int)ctx.notify_calls);
}

int main(void) {
    test_already_secure_success();
    test_initial_security_recovery_success();
    test_initial_security_timeout_blocks_ccc();
    test_initial_security_ok_without_ready_is_still_timeout();
    test_security_att_recovery_success();
    test_security_att_recovery_failure_reports_security_timeout();
    test_security_disconnect_reports_not_connected();
    test_true_ccc_timeout_stays_ccc_timeout();
    puts("test_pmd_start: ok");
    return 0;
}