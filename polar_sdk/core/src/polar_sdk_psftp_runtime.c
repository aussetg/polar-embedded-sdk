// SPDX-License-Identifier: MIT
#include "polar_sdk_psftp_runtime.h"

#include "polar_sdk_pmd.h"

static bool polar_sdk_psftp_prepare_ops_ready(const polar_sdk_psftp_prepare_ops_t *ops) {
    return ops != 0 &&
        ops->is_connected_ready != 0 &&
        ops->has_required_characteristics != 0 &&
        ops->security_ready != 0 &&
        ops->ensure_security != 0 &&
        ops->mtu_notify_enabled != 0 &&
        ops->enable_mtu_notify != 0 &&
        ops->d2h_notify_supported != 0 &&
        ops->d2h_notify_enabled != 0 &&
        ops->enable_d2h_notify != 0;
}

static int polar_sdk_psftp_map_security_result(polar_sdk_security_result_t r) {
    if (r == POLAR_SDK_SECURITY_RESULT_OK) {
        return POLAR_SDK_PSFTP_OP_OK;
    }
    if (r == POLAR_SDK_SECURITY_RESULT_NOT_CONNECTED) {
        return POLAR_SDK_PSFTP_OP_NOT_CONNECTED;
    }
    return POLAR_SDK_PSFTP_OP_TIMEOUT;
}

bool polar_sdk_psftp_prepare_failure_is_security_related(
    int prepare_status,
    bool security_ready) {
    if (prepare_status > 0 && polar_sdk_pmd_att_status_requires_security((uint8_t)prepare_status)) {
        return true;
    }

    if ((prepare_status == POLAR_SDK_PSFTP_OP_NOT_CONNECTED ||
         prepare_status == POLAR_SDK_PSFTP_OP_TIMEOUT ||
         prepare_status == POLAR_SDK_PSFTP_OP_TRANSPORT) &&
        !security_ready) {
        return true;
    }

    return false;
}

int polar_sdk_psftp_prepare_channels(
    const polar_sdk_psftp_prepare_policy_t *policy,
    const polar_sdk_psftp_prepare_ops_t *ops) {
    polar_sdk_psftp_prepare_policy_t effective = {
        .retry_security_on_att = true,
        .strict_d2h_enable = false,
    };
    if (policy != 0) {
        effective = *policy;
    }

    if (!polar_sdk_psftp_prepare_ops_ready(ops)) {
        return POLAR_SDK_PSFTP_OP_TRANSPORT;
    }

    if (!ops->is_connected_ready(ops->ctx)) {
        return POLAR_SDK_PSFTP_OP_NOT_CONNECTED;
    }

    if (!ops->has_required_characteristics(ops->ctx)) {
        return POLAR_SDK_PSFTP_OP_MISSING_CHAR;
    }

    if (!ops->security_ready(ops->ctx)) {
        int sec = polar_sdk_psftp_map_security_result(ops->ensure_security(ops->ctx));
        if (sec != POLAR_SDK_PSFTP_OP_OK) {
            return sec;
        }
    }

    if (!ops->mtu_notify_enabled(ops->ctx)) {
        int status = ops->enable_mtu_notify(ops->ctx);
        if (status > 0 && effective.retry_security_on_att &&
            polar_sdk_pmd_att_status_requires_security((uint8_t)status)) {
            int sec = polar_sdk_psftp_map_security_result(ops->ensure_security(ops->ctx));
            if (sec != POLAR_SDK_PSFTP_OP_OK) {
                return sec;
            }
            status = ops->enable_mtu_notify(ops->ctx);
        }
        if (status != POLAR_SDK_PSFTP_OP_OK) {
            return status;
        }
    }

    if (ops->d2h_notify_supported(ops->ctx) && !ops->d2h_notify_enabled(ops->ctx)) {
        int status = ops->enable_d2h_notify(ops->ctx);
        if (status > 0 && effective.retry_security_on_att &&
            polar_sdk_pmd_att_status_requires_security((uint8_t)status)) {
            int sec = polar_sdk_psftp_map_security_result(ops->ensure_security(ops->ctx));
            if (sec != POLAR_SDK_PSFTP_OP_OK) {
                return sec;
            }
            status = ops->enable_d2h_notify(ops->ctx);
        }

        if (status == POLAR_SDK_PSFTP_OP_NOT_CONNECTED) {
            return status;
        }
        if (status != POLAR_SDK_PSFTP_OP_OK && effective.strict_d2h_enable) {
            return status;
        }
    }

    return POLAR_SDK_PSFTP_OP_OK;
}

static bool polar_sdk_psftp_get_ops_ready(const polar_sdk_psftp_get_ops_t *ops) {
    return ops != 0 &&
        ops->prepare_channels != 0 &&
        ops->frame_capacity != 0 &&
        ops->write_frame != 0 &&
        ops->begin_response != 0 &&
        ops->wait_response != 0 &&
        ops->response_result != 0 &&
        ops->rx_state != 0;
}

static polar_sdk_psftp_trans_result_t polar_sdk_psftp_execute_rfc60_stream(
    const polar_sdk_psftp_get_ops_t *ops,
    const uint8_t *request_stream,
    size_t request_len,
    uint8_t *response,
    size_t response_capacity,
    uint32_t timeout_ms,
    size_t *out_response_len,
    uint16_t *out_error_code,
    int *out_prepare_status,
    int *out_write_status) {
    if (out_response_len != 0) {
        *out_response_len = 0;
    }
    if (out_error_code != 0) {
        *out_error_code = 0;
    }
    if (out_prepare_status != 0) {
        *out_prepare_status = POLAR_SDK_PSFTP_OP_OK;
    }
    if (out_write_status != 0) {
        *out_write_status = POLAR_SDK_PSFTP_OP_OK;
    }

    if (!polar_sdk_psftp_get_ops_ready(ops) || request_stream == 0 || request_len == 0u) {
        return POLAR_SDK_PSFTP_TRANS_NOTIFY_FAILED;
    }

    int prep = ops->prepare_channels(ops->ctx);
    if (out_prepare_status != 0) {
        *out_prepare_status = prep;
    }
    if (prep == POLAR_SDK_PSFTP_OP_NOT_CONNECTED) {
        return POLAR_SDK_PSFTP_TRANS_NOT_CONNECTED;
    }
    if (prep == POLAR_SDK_PSFTP_OP_MISSING_CHAR || prep == POLAR_SDK_PSFTP_OP_NO_NOTIFY_PROP) {
        return POLAR_SDK_PSFTP_TRANS_MISSING_CHARACTERISTICS;
    }
    if (prep == POLAR_SDK_PSFTP_OP_TIMEOUT) {
        return POLAR_SDK_PSFTP_TRANS_NOTIFY_TIMEOUT;
    }
    if (prep == POLAR_SDK_PSFTP_OP_TRANSPORT) {
        return POLAR_SDK_PSFTP_TRANS_NOTIFY_FAILED;
    }
    if (prep > 0) {
        return POLAR_SDK_PSFTP_TRANS_NOTIFY_ATT_REJECTED;
    }

    ops->begin_response(ops->ctx, response, response_capacity);

    uint16_t frame_capacity = ops->frame_capacity(ops->ctx);
    uint8_t frame[520];
    if (frame_capacity == 0 || frame_capacity > sizeof(frame)) {
        return POLAR_SDK_PSFTP_TRANS_ENCODE_FAILED;
    }

    polar_sdk_psftp_tx_state_t tx;
    polar_sdk_psftp_tx_init(&tx, request_stream, request_len);

    while (polar_sdk_psftp_tx_has_more(&tx)) {
        bool is_last = false;
        size_t frame_len = polar_sdk_psftp_tx_build_next_frame(
            &tx,
            frame_capacity,
            frame,
            sizeof(frame),
            &is_last);
        (void)is_last;

        if (frame_len == 0 || frame_len > UINT16_MAX) {
            return POLAR_SDK_PSFTP_TRANS_ENCODE_FAILED;
        }

        int write_status = ops->write_frame(ops->ctx, frame, (uint16_t)frame_len);
        if (out_write_status != 0) {
            *out_write_status = write_status;
        }
        if (write_status == POLAR_SDK_PSFTP_OP_NOT_CONNECTED) {
            return POLAR_SDK_PSFTP_TRANS_NOT_CONNECTED;
        }
        if (write_status == POLAR_SDK_PSFTP_OP_TIMEOUT) {
            return POLAR_SDK_PSFTP_TRANS_WRITE_TIMEOUT;
        }
        if (write_status == POLAR_SDK_PSFTP_OP_TRANSPORT) {
            return POLAR_SDK_PSFTP_TRANS_WRITE_FAILED;
        }
        if (write_status > 0) {
            return POLAR_SDK_PSFTP_TRANS_WRITE_ATT_REJECTED;
        }

        if (ops->on_tx_frame_ok != 0) {
            ops->on_tx_frame_ok(ops->ctx);
        }
    }

    if (!ops->wait_response(ops->ctx, timeout_ms)) {
        return POLAR_SDK_PSFTP_TRANS_RESPONSE_TIMEOUT;
    }

    const polar_sdk_psftp_rx_state_t *rx_state = ops->rx_state(ops->ctx);
    if (rx_state == 0) {
        return POLAR_SDK_PSFTP_TRANS_PROTOCOL_ERROR;
    }

    polar_sdk_psftp_rx_result_t rr = ops->response_result(ops->ctx);
    if (rr == POLAR_SDK_PSFTP_RX_COMPLETE) {
        if (out_response_len != 0) {
            *out_response_len = rx_state->length;
        }
        return POLAR_SDK_PSFTP_TRANS_OK;
    }

    if (rr == POLAR_SDK_PSFTP_RX_ERROR_FRAME) {
        uint16_t error_code = rx_state->error_code;
        if (out_error_code != 0) {
            *out_error_code = error_code;
        }
        if (ops->is_remote_success != 0 && ops->is_remote_success(ops->ctx, error_code)) {
            if (out_response_len != 0) {
                *out_response_len = rx_state->length;
            }
            return POLAR_SDK_PSFTP_TRANS_OK;
        }
        return POLAR_SDK_PSFTP_TRANS_REMOTE_ERROR;
    }

    if (rr == POLAR_SDK_PSFTP_RX_SEQUENCE_ERROR) {
        return POLAR_SDK_PSFTP_TRANS_SEQUENCE_ERROR;
    }
    if (rr == POLAR_SDK_PSFTP_RX_OVERFLOW) {
        return POLAR_SDK_PSFTP_TRANS_OVERFLOW;
    }

    return POLAR_SDK_PSFTP_TRANS_PROTOCOL_ERROR;
}

polar_sdk_psftp_trans_result_t polar_sdk_psftp_execute_proto_operation(
    const polar_sdk_psftp_get_ops_t *ops,
    const uint8_t *proto_payload,
    size_t proto_payload_len,
    uint8_t *response,
    size_t response_capacity,
    uint32_t timeout_ms,
    size_t *out_response_len,
    uint16_t *out_error_code,
    int *out_prepare_status,
    int *out_write_status) {
    if (!polar_sdk_psftp_get_ops_ready(ops) || proto_payload == 0 || proto_payload_len == 0) {
        return POLAR_SDK_PSFTP_TRANS_NOTIFY_FAILED;
    }

    uint8_t request_stream[POLAR_SDK_PSFTP_RUNTIME_MAX_PROTO_REQUEST_BYTES + 2u];
    size_t request_len = polar_sdk_psftp_build_rfc60_request(
        proto_payload,
        proto_payload_len,
        request_stream,
        sizeof(request_stream));
    if (request_len == 0) {
        return POLAR_SDK_PSFTP_TRANS_ENCODE_FAILED;
    }

    return polar_sdk_psftp_execute_rfc60_stream(
        ops,
        request_stream,
        request_len,
        response,
        response_capacity,
        timeout_ms,
        out_response_len,
        out_error_code,
        out_prepare_status,
        out_write_status);
}

polar_sdk_psftp_trans_result_t polar_sdk_psftp_execute_query_operation(
    const polar_sdk_psftp_get_ops_t *ops,
    uint16_t query_id,
    const uint8_t *query_payload,
    size_t query_payload_len,
    uint8_t *response,
    size_t response_capacity,
    uint32_t timeout_ms,
    size_t *out_response_len,
    uint16_t *out_error_code,
    int *out_prepare_status,
    int *out_write_status) {
    if (!polar_sdk_psftp_get_ops_ready(ops) ||
        (query_payload_len > 0u && query_payload == 0)) {
        return POLAR_SDK_PSFTP_TRANS_NOTIFY_FAILED;
    }

    uint8_t request_stream[POLAR_SDK_PSFTP_RUNTIME_MAX_PROTO_REQUEST_BYTES + 2u];
    size_t request_len = polar_sdk_psftp_build_rfc60_query(
        query_id,
        query_payload,
        query_payload_len,
        request_stream,
        sizeof(request_stream));
    if (request_len == 0) {
        return POLAR_SDK_PSFTP_TRANS_ENCODE_FAILED;
    }

    return polar_sdk_psftp_execute_rfc60_stream(
        ops,
        request_stream,
        request_len,
        response,
        response_capacity,
        timeout_ms,
        out_response_len,
        out_error_code,
        out_prepare_status,
        out_write_status);
}

polar_sdk_psftp_trans_result_t polar_sdk_psftp_execute_get_operation(
    const polar_sdk_psftp_get_ops_t *ops,
    const char *path,
    size_t path_len,
    uint8_t *response,
    size_t response_capacity,
    uint32_t timeout_ms,
    size_t *out_response_len,
    uint16_t *out_error_code,
    int *out_prepare_status,
    int *out_write_status) {
    uint8_t proto_payload[POLAR_SDK_PSFTP_RUNTIME_MAX_PROTO_REQUEST_BYTES];
    size_t proto_payload_len = 0;
    if (!polar_sdk_psftp_encode_get_operation(path, path_len, proto_payload, sizeof(proto_payload), &proto_payload_len)) {
        if (out_response_len != 0) {
            *out_response_len = 0;
        }
        if (out_error_code != 0) {
            *out_error_code = 0;
        }
        if (out_prepare_status != 0) {
            *out_prepare_status = POLAR_SDK_PSFTP_OP_OK;
        }
        if (out_write_status != 0) {
            *out_write_status = POLAR_SDK_PSFTP_OP_OK;
        }
        return POLAR_SDK_PSFTP_TRANS_ENCODE_FAILED;
    }

    return polar_sdk_psftp_execute_proto_operation(
        ops,
        proto_payload,
        proto_payload_len,
        response,
        response_capacity,
        timeout_ms,
        out_response_len,
        out_error_code,
        out_prepare_status,
        out_write_status);
}
