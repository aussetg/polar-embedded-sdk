// SPDX-License-Identifier: MIT
#ifndef POLAR_SDK_PSFTP_H
#define POLAR_SDK_PSFTP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define POLAR_SDK_PSFTP_MAX_PATH_BYTES (255u)
#define POLAR_SDK_PSFTP_MAX_ENTRY_NAME_BYTES (95u)
#define POLAR_SDK_H10_RECORDING_ID_MAX_BYTES (64u)
#define POLAR_SDK_PSFTP_PFTP_AIR_PACKET_LOST_ERROR (303u)

#define POLAR_SDK_PSFTP_QUERY_REQUEST_START_RECORDING (14u)
#define POLAR_SDK_PSFTP_QUERY_REQUEST_STOP_RECORDING (15u)
#define POLAR_SDK_PSFTP_QUERY_REQUEST_RECORDING_STATUS (16u)

typedef enum {
    POLAR_SDK_H10_RECORDING_SAMPLE_HEART_RATE = 1,
    POLAR_SDK_H10_RECORDING_SAMPLE_RR_INTERVAL = 16,
} polar_sdk_h10_recording_sample_type_t;

typedef enum {
    POLAR_SDK_H10_RECORDING_INTERVAL_1S = 1,
    POLAR_SDK_H10_RECORDING_INTERVAL_5S = 5,
} polar_sdk_h10_recording_interval_t;

typedef enum {
    POLAR_SDK_PSFTP_RX_MORE = 0,
    POLAR_SDK_PSFTP_RX_COMPLETE,
    POLAR_SDK_PSFTP_RX_ERROR_FRAME,
    POLAR_SDK_PSFTP_RX_SEQUENCE_ERROR,
    POLAR_SDK_PSFTP_RX_PROTOCOL_ERROR,
    POLAR_SDK_PSFTP_RX_OVERFLOW,
} polar_sdk_psftp_rx_result_t;

typedef struct {
    const uint8_t *stream;
    size_t stream_len;
    size_t offset;
    uint8_t sequence;
    uint8_t next_bit;
} polar_sdk_psftp_tx_state_t;

typedef struct {
    uint8_t *buffer;
    size_t capacity;
    size_t length;
    uint8_t expected_sequence;
    bool expecting_first;
    uint16_t error_code;
} polar_sdk_psftp_rx_state_t;

typedef struct {
    char name[POLAR_SDK_PSFTP_MAX_ENTRY_NAME_BYTES + 1u];
    uint64_t size;
} polar_sdk_psftp_dir_entry_t;

typedef enum {
    POLAR_SDK_PSFTP_DIR_DECODE_OK = 0,
    POLAR_SDK_PSFTP_DIR_DECODE_INVALID_ARGS,
    POLAR_SDK_PSFTP_DIR_DECODE_TOO_MANY_ENTRIES,
    POLAR_SDK_PSFTP_DIR_DECODE_FAILED,
} polar_sdk_psftp_dir_decode_result_t;

size_t polar_sdk_psftp_build_rfc60_request(
    const uint8_t *payload,
    size_t payload_len,
    uint8_t *out,
    size_t out_capacity);

size_t polar_sdk_psftp_build_rfc60_query(
    uint16_t query_id,
    const uint8_t *payload,
    size_t payload_len,
    uint8_t *out,
    size_t out_capacity);

void polar_sdk_psftp_tx_init(
    polar_sdk_psftp_tx_state_t *state,
    const uint8_t *stream,
    size_t stream_len);

bool polar_sdk_psftp_tx_has_more(const polar_sdk_psftp_tx_state_t *state);

size_t polar_sdk_psftp_tx_build_next_frame(
    polar_sdk_psftp_tx_state_t *state,
    size_t frame_capacity,
    uint8_t *out_frame,
    size_t out_frame_capacity,
    bool *out_is_last);

void polar_sdk_psftp_rx_reset(
    polar_sdk_psftp_rx_state_t *state,
    uint8_t *buffer,
    size_t capacity);

polar_sdk_psftp_rx_result_t polar_sdk_psftp_rx_feed_frame(
    polar_sdk_psftp_rx_state_t *state,
    const uint8_t *frame,
    size_t frame_len);

bool polar_sdk_psftp_encode_get_operation(
    const char *path,
    size_t path_len,
    uint8_t *out_payload,
    size_t out_payload_capacity,
    size_t *out_payload_len);

bool polar_sdk_psftp_encode_remove_operation(
    const char *path,
    size_t path_len,
    uint8_t *out_payload,
    size_t out_payload_capacity,
    size_t *out_payload_len);

bool polar_sdk_psftp_encode_h10_start_recording_params(
    const char *recording_id,
    size_t recording_id_len,
    polar_sdk_h10_recording_sample_type_t sample_type,
    polar_sdk_h10_recording_interval_t interval,
    uint8_t *out_payload,
    size_t out_payload_capacity,
    size_t *out_payload_len);

bool polar_sdk_psftp_decode_h10_recording_status_result(
    const uint8_t *payload,
    size_t payload_len,
    bool *out_recording_on,
    char *out_recording_id,
    size_t out_recording_id_capacity,
    size_t *out_recording_id_len);

polar_sdk_psftp_dir_decode_result_t polar_sdk_psftp_decode_directory(
    const uint8_t *payload,
    size_t payload_len,
    polar_sdk_psftp_dir_entry_t *out_entries,
    size_t out_entries_capacity,
    size_t *out_entry_count);

#ifdef __cplusplus
}
#endif

#endif // POLAR_SDK_PSFTP_H
