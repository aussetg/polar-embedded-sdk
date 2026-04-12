// SPDX-License-Identifier: MIT
#ifndef POLAR_SDK_PMD_H
#define POLAR_SDK_PMD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "polar_sdk_security.h"

#ifdef __cplusplus
extern "C" {
#endif

// PMD opcodes/measurement types used by Polar H10 paths.
enum {
    POLAR_SDK_PMD_OPCODE_GET_MEASUREMENT_SETTINGS = 0x01,
    POLAR_SDK_PMD_OPCODE_START_MEASUREMENT = 0x02,
    POLAR_SDK_PMD_OPCODE_STOP_MEASUREMENT = 0x03,
    POLAR_SDK_PMD_MEASUREMENT_ECG = 0x00,
    POLAR_SDK_PMD_MEASUREMENT_ACC = 0x02,
    POLAR_SDK_PMD_CP_RESPONSE_CODE = 0xF0,
};

enum {
    POLAR_SDK_PMD_RESPONSE_SUCCESS = 0x00,
    POLAR_SDK_PMD_RESPONSE_ALREADY_IN_STATE = 0x06,
    POLAR_SDK_PMD_RESPONSE_INVALID_SAMPLE_RATE = 0x08,
    POLAR_SDK_PMD_RESPONSE_INVALID_MTU = 0x0A,
};

typedef struct {
    uint16_t sample_rate;
    bool include_resolution;
    uint16_t resolution;
} polar_sdk_pmd_ecg_start_config_t;

typedef struct {
    uint16_t sample_rate;
    bool include_resolution;
    uint16_t resolution;
    bool include_range;
    uint16_t range;
} polar_sdk_pmd_acc_start_config_t;

typedef struct {
    uint8_t opcode;
    uint8_t measurement_type;
    uint8_t status;
    uint8_t more;
    bool has_more;
} polar_sdk_pmd_cp_response_t;

bool polar_sdk_pmd_att_status_requires_security(uint8_t att_status);
bool polar_sdk_pmd_security_ready(uint8_t encryption_key_size);
bool polar_sdk_pmd_response_status_ok(uint8_t status);

size_t polar_sdk_pmd_build_ecg_start_command(
    const polar_sdk_pmd_ecg_start_config_t *cfg,
    uint8_t *out,
    size_t out_capacity);

size_t polar_sdk_pmd_build_acc_start_command(
    const polar_sdk_pmd_acc_start_config_t *cfg,
    uint8_t *out,
    size_t out_capacity);

bool polar_sdk_pmd_parse_cp_response(
    const uint8_t *value,
    size_t value_len,
    polar_sdk_pmd_cp_response_t *out);

typedef enum {
    POLAR_SDK_PMD_START_RESULT_OK = 0,
    POLAR_SDK_PMD_START_RESULT_NOT_CONNECTED,
    POLAR_SDK_PMD_START_RESULT_SECURITY_TIMEOUT,
    POLAR_SDK_PMD_START_RESULT_CCC_REJECTED,
    POLAR_SDK_PMD_START_RESULT_CCC_TIMEOUT,
    POLAR_SDK_PMD_START_RESULT_MTU_FAILED,
    POLAR_SDK_PMD_START_RESULT_START_TIMEOUT,
    POLAR_SDK_PMD_START_RESULT_START_REJECTED,
    POLAR_SDK_PMD_START_RESULT_TRANSPORT_ERROR,
} polar_sdk_pmd_start_result_t;

typedef struct {
    size_t ccc_attempts;
    uint16_t minimum_mtu;
    uint16_t sample_rate;
    bool include_resolution;
    uint16_t resolution;
    bool include_range;
    uint16_t range;
} polar_sdk_pmd_start_policy_t;

typedef enum {
    POLAR_SDK_PMD_OP_OK = 0,
    POLAR_SDK_PMD_OP_NOT_CONNECTED = -1,
    POLAR_SDK_PMD_OP_TIMEOUT = -2,
    POLAR_SDK_PMD_OP_TRANSPORT = -3,
} polar_sdk_pmd_op_status_t;

typedef struct {
    void *ctx;
    bool (*is_connected)(void *ctx);

    // Must reflect the live transport security state, not a stale cache.
    // BTstack-backed callers should typically use polar_sdk_btstack_security_ready().
    bool (*security_ready)(void *ctx);

    // Request and wait for link security according to caller policy.
    polar_sdk_security_result_t (*ensure_security)(void *ctx);

    // 0 on success, >0 ATT status on ATT-level failure, <0 on transport errors.
    int (*enable_notifications)(void *ctx);
    int (*ensure_minimum_mtu)(void *ctx, uint16_t minimum_mtu);

    // return 0 on transport success and set *out_status to PMD CP response status.
    int (*start_ecg_and_wait_response)(
        void *ctx,
        const uint8_t *start_cmd,
        size_t start_cmd_len,
        uint8_t *out_status);
} polar_sdk_pmd_start_ops_t;

polar_sdk_pmd_start_result_t polar_sdk_pmd_start_ecg_with_policy(
    const polar_sdk_pmd_start_policy_t *policy,
    const polar_sdk_pmd_start_ops_t *ops,
    uint8_t *out_pmd_response_status,
    int *out_last_ccc_att_status);

polar_sdk_pmd_start_result_t polar_sdk_pmd_start_acc_with_policy(
    const polar_sdk_pmd_start_policy_t *policy,
    const polar_sdk_pmd_start_ops_t *ops,
    uint8_t *out_pmd_response_status,
    int *out_last_ccc_att_status);

#ifdef __cplusplus
}
#endif

#endif // POLAR_SDK_PMD_H
