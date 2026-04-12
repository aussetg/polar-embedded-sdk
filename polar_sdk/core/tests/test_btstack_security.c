// SPDX-License-Identifier: MIT
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "polar_sdk_btstack_security.h"

#define ASSERT_TRUE(expr) do { if (!(expr)) fail(__LINE__, #expr); } while (0)
#define ASSERT_EQ_INT(expected, actual) do { int _e = (expected); int _a = (actual); if (_e != _a) fail_int(__LINE__, #actual, _e, _a); } while (0)

static uint16_t g_active_handle = 0xffffu;
static uint8_t g_active_key_size = 0u;
static unsigned g_pairing_calls = 0u;
static unsigned g_auth_policy_calls = 0u;
static unsigned g_sleep_calls = 0u;
static unsigned g_promote_on_pair_call = 0u;
static uint8_t g_promoted_key_size = 16u;
static bool g_disconnect_on_sleep = false;

static void fail(int line, const char *expr) {
    fprintf(stderr, "test_btstack_security:%d assertion failed: %s\n", line, expr);
    exit(1);
}

static void fail_int(int line, const char *expr, int expected, int actual) {
    fprintf(stderr, "test_btstack_security:%d assertion failed: %s expected=%d actual=%d\n", line, expr, expected, actual);
    exit(1);
}

static void reset_fakes(void) {
    g_active_handle = 0xffffu;
    g_active_key_size = 0u;
    g_pairing_calls = 0u;
    g_auth_policy_calls = 0u;
    g_sleep_calls = 0u;
    g_promote_on_pair_call = 0u;
    g_promoted_key_size = 16u;
    g_disconnect_on_sleep = false;
}

uint8_t gap_encryption_key_size(uint16_t conn_handle) {
    return conn_handle == g_active_handle ? g_active_key_size : 0u;
}

void sm_request_pairing(uint16_t conn_handle) {
    if (conn_handle != g_active_handle) {
        return;
    }
    g_pairing_calls += 1u;
    if (g_promote_on_pair_call != 0u && g_pairing_calls >= g_promote_on_pair_call) {
        g_active_key_size = g_promoted_key_size;
    }
}

void polar_sdk_btstack_sm_configure_default_central_policy(void) {
    g_auth_policy_calls += 1u;
}

static void fake_sleep_ms(void *ctx, uint32_t ms) {
    (void)ctx;
    (void)ms;
    g_sleep_calls += 1u;
    if (g_disconnect_on_sleep) {
        g_active_handle = 0xffffu;
        g_active_key_size = 0u;
    }
}

static polar_sdk_security_policy_t test_policy(void) {
    polar_sdk_security_policy_t policy = {
        .rounds = 2u,
        .wait_ms_per_round = 30u,
        .request_gap_ms = 10u,
        .poll_ms = 10u,
    };
    return policy;
}

static void test_invalid_handle(void) {
    reset_fakes();
    polar_sdk_security_policy_t policy = test_policy();

    ASSERT_EQ_INT(0, polar_sdk_btstack_security_encryption_key_size(0xffffu, 0xffffu));
    ASSERT_TRUE(!polar_sdk_btstack_security_ready(0xffffu, 0xffffu));
    polar_sdk_btstack_security_request_pairing(0xffffu, 0xffffu);
    ASSERT_EQ_INT(
        POLAR_SDK_SECURITY_RESULT_NOT_CONNECTED,
        polar_sdk_btstack_security_ensure(&g_active_handle, 0xffffu, &policy, fake_sleep_ms, NULL));
    ASSERT_EQ_INT(0, (int)g_auth_policy_calls);
    ASSERT_EQ_INT(0, (int)g_pairing_calls);
}

static void test_request_pairing_helper(void) {
    reset_fakes();
    g_active_handle = 0x0040u;

    polar_sdk_btstack_security_request_pairing(g_active_handle, 0xffffu);

    ASSERT_EQ_INT(1, (int)g_auth_policy_calls);
    ASSERT_EQ_INT(1, (int)g_pairing_calls);
}

static void test_already_secure_link(void) {
    reset_fakes();
    g_active_handle = 0x0040u;
    g_active_key_size = 16u;
    polar_sdk_security_policy_t policy = test_policy();

    ASSERT_EQ_INT(16, polar_sdk_btstack_security_encryption_key_size(g_active_handle, 0xffffu));
    ASSERT_TRUE(polar_sdk_btstack_security_ready(g_active_handle, 0xffffu));
    ASSERT_EQ_INT(
        POLAR_SDK_SECURITY_RESULT_OK,
        polar_sdk_btstack_security_ensure(&g_active_handle, 0xffffu, &policy, fake_sleep_ms, NULL));
    ASSERT_EQ_INT(0, (int)g_auth_policy_calls);
    ASSERT_EQ_INT(0, (int)g_pairing_calls);
}

static void test_pairing_promotes_security(void) {
    reset_fakes();
    g_active_handle = 0x0040u;
    g_promote_on_pair_call = 1u;
    polar_sdk_security_policy_t policy = test_policy();

    ASSERT_EQ_INT(
        POLAR_SDK_SECURITY_RESULT_OK,
        polar_sdk_btstack_security_ensure(&g_active_handle, 0xffffu, &policy, fake_sleep_ms, NULL));
    ASSERT_EQ_INT(1, (int)g_auth_policy_calls);
    ASSERT_EQ_INT(2, (int)g_pairing_calls);
    ASSERT_EQ_INT(16, g_active_key_size);
    ASSERT_TRUE(g_sleep_calls >= 1u);
}

static void test_timeout_without_security(void) {
    reset_fakes();
    g_active_handle = 0x0040u;
    polar_sdk_security_policy_t policy = test_policy();

    ASSERT_EQ_INT(
        POLAR_SDK_SECURITY_RESULT_TIMEOUT,
        polar_sdk_btstack_security_ensure(&g_active_handle, 0xffffu, &policy, fake_sleep_ms, NULL));
    ASSERT_EQ_INT(1, (int)g_auth_policy_calls);
    ASSERT_EQ_INT(4, (int)g_pairing_calls);
    ASSERT_TRUE(g_sleep_calls >= 1u);
}

static void test_disconnect_during_wait(void) {
    reset_fakes();
    g_active_handle = 0x0040u;
    g_disconnect_on_sleep = true;
    polar_sdk_security_policy_t policy = test_policy();

    ASSERT_EQ_INT(
        POLAR_SDK_SECURITY_RESULT_NOT_CONNECTED,
        polar_sdk_btstack_security_ensure(&g_active_handle, 0xffffu, &policy, fake_sleep_ms, NULL));
    ASSERT_EQ_INT(1, (int)g_auth_policy_calls);
    ASSERT_EQ_INT(1, (int)g_pairing_calls);
    ASSERT_TRUE(g_sleep_calls >= 1u);
}

int main(void) {
    test_invalid_handle();
    test_request_pairing_helper();
    test_already_secure_link();
    test_pairing_promotes_security();
    test_timeout_without_security();
    test_disconnect_during_wait();
    puts("test_btstack_security: ok");
    return 0;
}