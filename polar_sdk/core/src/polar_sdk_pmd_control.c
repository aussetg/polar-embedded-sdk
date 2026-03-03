// SPDX-License-Identifier: MIT
#include "polar_sdk_pmd_control.h"

int polar_sdk_pmd_map_notify_result(
    polar_sdk_gatt_notify_runtime_result_t result,
    uint8_t att_status,
    int op_ok,
    int op_not_connected,
    int op_timeout,
    int op_transport) {
    if (result == POLAR_SDK_GATT_NOTIFY_RUNTIME_OK) {
        return op_ok;
    }
    if (result == POLAR_SDK_GATT_NOTIFY_RUNTIME_NOT_CONNECTED) {
        return op_not_connected;
    }
    if (result == POLAR_SDK_GATT_NOTIFY_RUNTIME_TIMEOUT) {
        return op_timeout;
    }
    if (result == POLAR_SDK_GATT_NOTIFY_RUNTIME_ATT_REJECTED) {
        return att_status > 0 ? (int)att_status : op_transport;
    }
    return op_transport;
}

int polar_sdk_pmd_enable_notify_pair(
    const polar_sdk_pmd_notify_pair_ops_t *ops,
    int op_ok,
    int op_not_connected,
    int op_transport,
    int *out_last_att_status) {
    if (ops == 0 ||
        ops->is_connected == 0 ||
        ops->enable_cp_notify == 0 ||
        ops->enable_data_notify == 0) {
        return op_transport;
    }

    if (out_last_att_status) {
        *out_last_att_status = 0;
    }

    if (!ops->is_connected(ops->ctx)) {
        return op_not_connected;
    }

    int r = ops->enable_cp_notify(ops->ctx);
    if (r != op_ok) {
        if (r > 0 && out_last_att_status) {
            *out_last_att_status = r;
        }
        return r;
    }

    r = ops->enable_data_notify(ops->ctx);
    if (r != op_ok) {
        if (r > 0 && out_last_att_status) {
            *out_last_att_status = r;
        }
        return r;
    }

    return op_ok;
}

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
    uint8_t *out_response_status) {
    if (ops == 0 ||
        ops->is_connected == 0 ||
        ops->expect_response == 0 ||
        ops->write_command == 0 ||
        ops->wait_response == 0 ||
        cmd == 0 ||
        cmd_len == 0 ||
        cmd_len > 0xffffu) {
        return op_transport;
    }

    if (!ops->is_connected(ops->ctx)) {
        return op_not_connected;
    }

    ops->expect_response(ops->ctx, opcode, measurement_type);

    int wr = ops->write_command(ops->ctx, cmd, (uint16_t)cmd_len);
    if (wr != op_ok) {
        return wr;
    }

    uint8_t response_status = 0xff;
    if (!ops->wait_response(ops->ctx, timeout_ms, &response_status)) {
        return op_timeout;
    }

    if (out_response_status) {
        *out_response_status = response_status;
    }

    return op_ok;
}
