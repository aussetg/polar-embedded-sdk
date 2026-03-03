// SPDX-License-Identifier: MIT
#ifndef POLAR_SDK_PMD_CONTROL_H
#define POLAR_SDK_PMD_CONTROL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "polar_sdk_gatt_notify_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

int polar_sdk_pmd_map_notify_result(
    polar_sdk_gatt_notify_runtime_result_t result,
    uint8_t att_status,
    int op_ok,
    int op_not_connected,
    int op_timeout,
    int op_transport);

typedef struct {
    void *ctx;
    bool (*is_connected)(void *ctx);
    int (*enable_cp_notify)(void *ctx);
    int (*enable_data_notify)(void *ctx);
} polar_sdk_pmd_notify_pair_ops_t;

int polar_sdk_pmd_enable_notify_pair(
    const polar_sdk_pmd_notify_pair_ops_t *ops,
    int op_ok,
    int op_not_connected,
    int op_transport,
    int *out_last_att_status);

typedef struct {
    void *ctx;
    bool (*is_connected)(void *ctx);
    void (*expect_response)(void *ctx, uint8_t opcode, uint8_t measurement_type);
    int (*write_command)(void *ctx, const uint8_t *cmd, uint16_t cmd_len);
    bool (*wait_response)(void *ctx, uint32_t timeout_ms, uint8_t *out_response_status);
} polar_sdk_pmd_start_cmd_ops_t;

int polar_sdk_pmd_start_command_and_wait(
    const polar_sdk_pmd_start_cmd_ops_t *ops,
    const uint8_t *cmd,
    size_t cmd_len,
    uint8_t opcode,
    uint8_t measurement_type,
    uint32_t timeout_ms,
    int op_ok,
    int op_not_connected,
    int op_timeout,
    int op_transport,
    uint8_t *out_response_status);

#ifdef __cplusplus
}
#endif

#endif // POLAR_SDK_PMD_CONTROL_H
