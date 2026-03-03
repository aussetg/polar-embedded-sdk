// SPDX-License-Identifier: MIT
#ifndef POLAR_BLE_DRIVER_PSFTP_H
#define POLAR_BLE_DRIVER_PSFTP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define POLAR_BLE_DRIVER_PSFTP_MAX_PATH_BYTES (255u)
#define POLAR_BLE_DRIVER_PSFTP_MAX_ENTRY_NAME_BYTES (95u)
#define POLAR_BLE_DRIVER_PSFTP_PFTP_AIR_PACKET_LOST_ERROR (303u)

typedef enum {
    POLAR_BLE_DRIVER_PSFTP_RX_MORE = 0,
    POLAR_BLE_DRIVER_PSFTP_RX_COMPLETE,
    POLAR_BLE_DRIVER_PSFTP_RX_ERROR_FRAME,
    POLAR_BLE_DRIVER_PSFTP_RX_SEQUENCE_ERROR,
    POLAR_BLE_DRIVER_PSFTP_RX_PROTOCOL_ERROR,
    POLAR_BLE_DRIVER_PSFTP_RX_OVERFLOW,
} polar_ble_driver_psftp_rx_result_t;

typedef struct {
    const uint8_t *stream;
    size_t stream_len;
    size_t offset;
    uint8_t sequence;
    uint8_t next_bit;
} polar_ble_driver_psftp_tx_state_t;

typedef struct {
    uint8_t *buffer;
    size_t capacity;
    size_t length;
    uint8_t expected_sequence;
    bool expecting_first;
    uint16_t error_code;
} polar_ble_driver_psftp_rx_state_t;

typedef struct {
    char name[POLAR_BLE_DRIVER_PSFTP_MAX_ENTRY_NAME_BYTES + 1u];
    uint64_t size;
} polar_ble_driver_psftp_dir_entry_t;

typedef enum {
    POLAR_BLE_DRIVER_PSFTP_DIR_DECODE_OK = 0,
    POLAR_BLE_DRIVER_PSFTP_DIR_DECODE_INVALID_ARGS,
    POLAR_BLE_DRIVER_PSFTP_DIR_DECODE_TOO_MANY_ENTRIES,
    POLAR_BLE_DRIVER_PSFTP_DIR_DECODE_FAILED,
} polar_ble_driver_psftp_dir_decode_result_t;

size_t polar_ble_driver_psftp_build_rfc60_request(
    const uint8_t *payload,
    size_t payload_len,
    uint8_t *out,
    size_t out_capacity);

void polar_ble_driver_psftp_tx_init(
    polar_ble_driver_psftp_tx_state_t *state,
    const uint8_t *stream,
    size_t stream_len);

bool polar_ble_driver_psftp_tx_has_more(const polar_ble_driver_psftp_tx_state_t *state);

size_t polar_ble_driver_psftp_tx_build_next_frame(
    polar_ble_driver_psftp_tx_state_t *state,
    size_t frame_capacity,
    uint8_t *out_frame,
    size_t out_frame_capacity,
    bool *out_is_last);

void polar_ble_driver_psftp_rx_reset(
    polar_ble_driver_psftp_rx_state_t *state,
    uint8_t *buffer,
    size_t capacity);

polar_ble_driver_psftp_rx_result_t polar_ble_driver_psftp_rx_feed_frame(
    polar_ble_driver_psftp_rx_state_t *state,
    const uint8_t *frame,
    size_t frame_len);

bool polar_ble_driver_psftp_encode_get_operation(
    const char *path,
    size_t path_len,
    uint8_t *out_payload,
    size_t out_payload_capacity,
    size_t *out_payload_len);

polar_ble_driver_psftp_dir_decode_result_t polar_ble_driver_psftp_decode_directory(
    const uint8_t *payload,
    size_t payload_len,
    polar_ble_driver_psftp_dir_entry_t *out_entries,
    size_t out_entries_capacity,
    size_t *out_entry_count);

#ifdef __cplusplus
}
#endif

#endif // POLAR_BLE_DRIVER_PSFTP_H
