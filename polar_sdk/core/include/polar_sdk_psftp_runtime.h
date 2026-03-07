// SPDX-License-Identifier: MIT
#ifndef POLAR_SDK_PSFTP_RUNTIME_H
#define POLAR_SDK_PSFTP_RUNTIME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "polar_sdk_psftp.h"
#include "polar_sdk_security.h"

#ifdef __cplusplus
extern "C" {
#endif

#define POLAR_SDK_PSFTP_RUNTIME_MAX_PROTO_REQUEST_BYTES (320u)

typedef enum {
    POLAR_SDK_PSFTP_OP_OK = 0,
    POLAR_SDK_PSFTP_OP_NOT_CONNECTED = -1,
    POLAR_SDK_PSFTP_OP_TIMEOUT = -2,
    POLAR_SDK_PSFTP_OP_TRANSPORT = -3,
    POLAR_SDK_PSFTP_OP_MISSING_CHAR = -4,
    POLAR_SDK_PSFTP_OP_NO_NOTIFY_PROP = -5,
} polar_sdk_psftp_op_status_t;

typedef enum {
    POLAR_SDK_PSFTP_TRANS_OK = 0,
    POLAR_SDK_PSFTP_TRANS_NOT_CONNECTED,
    POLAR_SDK_PSFTP_TRANS_MISSING_CHARACTERISTICS,
    POLAR_SDK_PSFTP_TRANS_NOTIFY_FAILED,
    POLAR_SDK_PSFTP_TRANS_NOTIFY_TIMEOUT,
    POLAR_SDK_PSFTP_TRANS_NOTIFY_ATT_REJECTED,
    POLAR_SDK_PSFTP_TRANS_ENCODE_FAILED,
    POLAR_SDK_PSFTP_TRANS_WRITE_FAILED,
    POLAR_SDK_PSFTP_TRANS_WRITE_TIMEOUT,
    POLAR_SDK_PSFTP_TRANS_WRITE_ATT_REJECTED,
    POLAR_SDK_PSFTP_TRANS_RESPONSE_TIMEOUT,
    POLAR_SDK_PSFTP_TRANS_PROTOCOL_ERROR,
    POLAR_SDK_PSFTP_TRANS_SEQUENCE_ERROR,
    POLAR_SDK_PSFTP_TRANS_OVERFLOW,
    POLAR_SDK_PSFTP_TRANS_REMOTE_ERROR,
} polar_sdk_psftp_trans_result_t;

typedef struct {
    bool retry_security_on_att;
    bool strict_d2h_enable;
} polar_sdk_psftp_prepare_policy_t;

typedef struct {
    void *ctx;

    bool (*is_connected_ready)(void *ctx);
    bool (*has_required_characteristics)(void *ctx);

    bool (*security_ready)(void *ctx);
    polar_sdk_security_result_t (*ensure_security)(void *ctx);

    bool (*mtu_notify_enabled)(void *ctx);
    int (*enable_mtu_notify)(void *ctx);

    bool (*d2h_notify_supported)(void *ctx);
    bool (*d2h_notify_enabled)(void *ctx);
    int (*enable_d2h_notify)(void *ctx);
} polar_sdk_psftp_prepare_ops_t;

int polar_sdk_psftp_prepare_channels(
    const polar_sdk_psftp_prepare_policy_t *policy,
    const polar_sdk_psftp_prepare_ops_t *ops);

// Classify whether a prepare-stage failure likely requires security recovery
// (pairing/encryption and/or reconnect) before retrying.
bool polar_sdk_psftp_prepare_failure_is_security_related(
    int prepare_status,
    bool security_ready);

typedef struct {
    void *ctx;

    // Returns polar_sdk_psftp_op_status_t or ATT status (>0).
    int (*prepare_channels)(void *ctx);

    // Return max payload bytes per RFC76 frame body.
    uint16_t (*frame_capacity)(void *ctx);

    // Returns polar_sdk_psftp_op_status_t or ATT status (>0).
    int (*write_frame)(void *ctx, const uint8_t *frame, uint16_t frame_len);

    // Optional per-frame success callback.
    void (*on_tx_frame_ok)(void *ctx);

    // Called once before first TX frame.
    void (*begin_response)(void *ctx, uint8_t *response, size_t response_capacity);

    // Wait until response complete/error or timeout.
    bool (*wait_response)(void *ctx, uint32_t timeout_ms);

    polar_sdk_psftp_rx_result_t (*response_result)(void *ctx);

    // Return the active RX state (length/error_code are read from this state).
    const polar_sdk_psftp_rx_state_t *(*rx_state)(void *ctx);

    // Optional: treat remote error frame as success for specific codes.
    bool (*is_remote_success)(void *ctx, uint16_t error_code);
} polar_sdk_psftp_get_ops_t;

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
    int *out_write_status);

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
    int *out_write_status);

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
    int *out_write_status);

#ifdef __cplusplus
}
#endif

#endif // POLAR_SDK_PSFTP_RUNTIME_H
