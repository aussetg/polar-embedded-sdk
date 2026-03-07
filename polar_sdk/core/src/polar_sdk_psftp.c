// SPDX-License-Identifier: MIT
#include "polar_sdk_psftp.h"

#include <string.h>

#ifndef POLAR_CFG_ENABLE_PSFTP
#define POLAR_CFG_ENABLE_PSFTP (0)
#endif

#if POLAR_CFG_ENABLE_PSFTP
#include "pb_decode.h"
#include "pb_encode.h"
#include "pftp_request.pb.h"
#include "pftp_response.pb.h"
#endif

static size_t polar_sdk_psftp_min_size(size_t a, size_t b) {
    return a < b ? a : b;
}

#if POLAR_CFG_ENABLE_PSFTP
static size_t polar_sdk_psftp_bounded_strlen(const char *s, size_t max_len) {
    if (s == 0) {
        return 0;
    }

    size_t i = 0;
    while (i < max_len && s[i] != '\0') {
        i += 1;
    }
    return i;
}
#endif

size_t polar_sdk_psftp_build_rfc60_request(
    const uint8_t *payload,
    size_t payload_len,
    uint8_t *out,
    size_t out_capacity) {
    if (out == 0) {
        return 0;
    }

    if (payload_len > 0x7fffu || out_capacity < payload_len + 2u) {
        return 0;
    }

    out[0] = (uint8_t)(payload_len & 0xffu);
    out[1] = (uint8_t)((payload_len >> 8) & 0x7fu);

    if (payload_len > 0u) {
        if (payload == 0) {
            return 0;
        }
        memcpy(out + 2, payload, payload_len);
    }

    return payload_len + 2u;
}

size_t polar_sdk_psftp_build_rfc60_query(
    uint16_t query_id,
    const uint8_t *payload,
    size_t payload_len,
    uint8_t *out,
    size_t out_capacity) {
    if (out == 0) {
        return 0;
    }

    if (query_id > 0x7fffu || out_capacity < payload_len + 2u) {
        return 0;
    }

    out[0] = (uint8_t)(query_id & 0xffu);
    out[1] = (uint8_t)(((query_id >> 8) & 0x7fu) | 0x80u);

    if (payload_len > 0u) {
        if (payload == 0) {
            return 0;
        }
        memcpy(out + 2, payload, payload_len);
    }

    return payload_len + 2u;
}

void polar_sdk_psftp_tx_init(
    polar_sdk_psftp_tx_state_t *state,
    const uint8_t *stream,
    size_t stream_len) {
    if (state == 0) {
        return;
    }

    state->stream = stream;
    state->stream_len = stream_len;
    state->offset = 0;
    state->sequence = 0;
    state->next_bit = 0;
}

bool polar_sdk_psftp_tx_has_more(const polar_sdk_psftp_tx_state_t *state) {
    if (state == 0) {
        return false;
    }

    return state->offset < state->stream_len;
}

size_t polar_sdk_psftp_tx_build_next_frame(
    polar_sdk_psftp_tx_state_t *state,
    size_t frame_capacity,
    uint8_t *out_frame,
    size_t out_frame_capacity,
    bool *out_is_last) {
    if (state == 0 || out_frame == 0 || frame_capacity == 0 || out_frame_capacity == 0) {
        return 0;
    }

    if (!polar_sdk_psftp_tx_has_more(state)) {
        return 0;
    }

    if (state->stream_len > 0 && state->stream == 0) {
        return 0;
    }

    size_t max_payload = 0;
    if (frame_capacity > 1u && out_frame_capacity > 1u) {
        max_payload = polar_sdk_psftp_min_size(frame_capacity - 1u, out_frame_capacity - 1u);
    }

    size_t remaining = 0;
    if (state->offset < state->stream_len) {
        remaining = state->stream_len - state->offset;
    }

    size_t chunk_len = polar_sdk_psftp_min_size(remaining, max_payload);
    bool more = remaining > chunk_len;

    out_frame[0] = (uint8_t)((more ? 0x06u : 0x02u) | (state->next_bit & 0x01u) | ((state->sequence & 0x0fu) << 4));
    if (chunk_len > 0u) {
        memcpy(out_frame + 1, state->stream + state->offset, chunk_len);
    }

    state->sequence = (uint8_t)((state->sequence + 1u) & 0x0fu);
    state->next_bit = 1;

    if (more) {
        state->offset += chunk_len;
    } else {
        state->offset = state->stream_len + 1u;
    }

    if (out_is_last != 0) {
        *out_is_last = !more;
    }

    return chunk_len + 1u;
}

void polar_sdk_psftp_rx_reset(
    polar_sdk_psftp_rx_state_t *state,
    uint8_t *buffer,
    size_t capacity) {
    if (state == 0) {
        return;
    }

    state->buffer = buffer;
    state->capacity = capacity;
    state->length = 0;
    state->expected_sequence = 0;
    state->expecting_first = true;
    state->error_code = 0;
}

polar_sdk_psftp_rx_result_t polar_sdk_psftp_rx_feed_frame(
    polar_sdk_psftp_rx_state_t *state,
    const uint8_t *frame,
    size_t frame_len) {
    if (state == 0 || frame == 0 || frame_len == 0) {
        return POLAR_SDK_PSFTP_RX_PROTOCOL_ERROR;
    }

    uint8_t header = frame[0];
    uint8_t next = header & 0x01u;
    uint8_t status = (uint8_t)((header >> 1) & 0x03u);
    uint8_t sequence = (uint8_t)((header >> 4) & 0x0fu);

    if (sequence != state->expected_sequence) {
        return POLAR_SDK_PSFTP_RX_SEQUENCE_ERROR;
    }

    if (state->expecting_first) {
        if (next != 0u) {
            return POLAR_SDK_PSFTP_RX_PROTOCOL_ERROR;
        }
    } else if (next != 1u) {
        return POLAR_SDK_PSFTP_RX_PROTOCOL_ERROR;
    }

    state->expected_sequence = (uint8_t)((state->expected_sequence + 1u) & 0x0fu);
    state->expecting_first = false;

    if (status == 0u) {
        if (frame_len == 3u) {
            state->error_code = (uint16_t)frame[1] | ((uint16_t)frame[2] << 8);
            return POLAR_SDK_PSFTP_RX_ERROR_FRAME;
        }
        return POLAR_SDK_PSFTP_RX_PROTOCOL_ERROR;
    }

    if (status != 0x01u && status != 0x03u) {
        return POLAR_SDK_PSFTP_RX_PROTOCOL_ERROR;
    }

    size_t payload_len = frame_len - 1u;
    if (payload_len == 0u && status == 0x03u) {
        return POLAR_SDK_PSFTP_RX_PROTOCOL_ERROR;
    }

    if (payload_len > 0u) {
        if (state->buffer == 0) {
            return POLAR_SDK_PSFTP_RX_PROTOCOL_ERROR;
        }
        if (state->length + payload_len > state->capacity) {
            return POLAR_SDK_PSFTP_RX_OVERFLOW;
        }

        memcpy(state->buffer + state->length, frame + 1, payload_len);
        state->length += payload_len;
    }

    return status == 0x01u ? POLAR_SDK_PSFTP_RX_COMPLETE : POLAR_SDK_PSFTP_RX_MORE;
}

static bool polar_sdk_psftp_encode_operation(
    int command,
    const char *path,
    size_t path_len,
    uint8_t *out_payload,
    size_t out_payload_capacity,
    size_t *out_payload_len) {
    if (out_payload_len != 0) {
        *out_payload_len = 0;
    }

    if (path == 0 || out_payload == 0 || path_len > POLAR_SDK_PSFTP_MAX_PATH_BYTES) {
        return false;
    }

#if !POLAR_CFG_ENABLE_PSFTP
    (void)out_payload_capacity;
    return false;
#else
    protocol_PbPFtpOperation operation = protocol_PbPFtpOperation_init_zero;
    operation.command = command;

    if (path_len >= sizeof(operation.path)) {
        return false;
    }

    if (path_len > 0u) {
        memcpy(operation.path, path, path_len);
    }
    operation.path[path_len] = '\0';

    pb_ostream_t stream = pb_ostream_from_buffer(out_payload, out_payload_capacity);
    if (!pb_encode(&stream, protocol_PbPFtpOperation_fields, &operation)) {
        return false;
    }

    if (out_payload_len != 0) {
        *out_payload_len = stream.bytes_written;
    }

    return true;
#endif
}

bool polar_sdk_psftp_encode_get_operation(
    const char *path,
    size_t path_len,
    uint8_t *out_payload,
    size_t out_payload_capacity,
    size_t *out_payload_len) {
#if !POLAR_CFG_ENABLE_PSFTP
    (void)path;
    (void)path_len;
    (void)out_payload;
    (void)out_payload_capacity;
    if (out_payload_len != 0) {
        *out_payload_len = 0;
    }
    return false;
#else
    return polar_sdk_psftp_encode_operation(
        protocol_PbPFtpOperation_Command_GET,
        path,
        path_len,
        out_payload,
        out_payload_capacity,
        out_payload_len);
#endif
}

bool polar_sdk_psftp_encode_remove_operation(
    const char *path,
    size_t path_len,
    uint8_t *out_payload,
    size_t out_payload_capacity,
    size_t *out_payload_len) {
#if !POLAR_CFG_ENABLE_PSFTP
    (void)path;
    (void)path_len;
    (void)out_payload;
    (void)out_payload_capacity;
    if (out_payload_len != 0) {
        *out_payload_len = 0;
    }
    return false;
#else
    return polar_sdk_psftp_encode_operation(
        protocol_PbPFtpOperation_Command_REMOVE,
        path,
        path_len,
        out_payload,
        out_payload_capacity,
        out_payload_len);
#endif
}

#if POLAR_CFG_ENABLE_PSFTP
typedef struct {
    const uint8_t *data;
    size_t len;
} polar_sdk_psftp_string_view_t;

typedef struct {
    char *buffer;
    size_t capacity;
    size_t len;
    bool present;
    bool overflow;
} polar_sdk_psftp_string_decode_ctx_t;

static bool polar_sdk_psftp_encode_string_cb(
    pb_ostream_t *stream,
    const pb_field_t *field,
    void * const *arg) {
    if (stream == 0 || field == 0 || arg == 0 || *arg == 0) {
        return false;
    }

    const polar_sdk_psftp_string_view_t *view = (const polar_sdk_psftp_string_view_t *)(*arg);
    if (!pb_encode_tag_for_field(stream, field)) {
        return false;
    }
    return pb_encode_string(stream, view->data, view->len);
}

static bool polar_sdk_psftp_discard_bytes(pb_istream_t *stream, size_t len) {
    uint8_t scratch[32];
    size_t remaining = len;
    while (remaining > 0u) {
        size_t chunk = remaining < sizeof(scratch) ? remaining : sizeof(scratch);
        if (!pb_read(stream, scratch, chunk)) {
            return false;
        }
        remaining -= chunk;
    }
    return true;
}

static bool polar_sdk_psftp_decode_string_cb(
    pb_istream_t *stream,
    const pb_field_t *field,
    void **arg) {
    (void)field;

    if (stream == 0 || arg == 0 || *arg == 0) {
        return false;
    }

    polar_sdk_psftp_string_decode_ctx_t *ctx = (polar_sdk_psftp_string_decode_ctx_t *)(*arg);
    size_t len = stream->bytes_left;
    ctx->present = true;
    ctx->len = len;

    if (ctx->buffer == 0 || ctx->capacity == 0u) {
        return polar_sdk_psftp_discard_bytes(stream, len);
    }

    if (len >= ctx->capacity) {
        ctx->buffer[0] = '\0';
        ctx->overflow = true;
        return polar_sdk_psftp_discard_bytes(stream, len);
    }

    if (len > 0u && !pb_read(stream, (pb_byte_t *)ctx->buffer, len)) {
        return false;
    }

    ctx->buffer[len] = '\0';
    return true;
}
#endif

bool polar_sdk_psftp_encode_h10_start_recording_params(
    const char *recording_id,
    size_t recording_id_len,
    polar_sdk_h10_recording_sample_type_t sample_type,
    polar_sdk_h10_recording_interval_t interval,
    uint8_t *out_payload,
    size_t out_payload_capacity,
    size_t *out_payload_len) {
    if (out_payload_len != 0) {
        *out_payload_len = 0;
    }

    if (recording_id == 0 || out_payload == 0) {
        return false;
    }

    if (recording_id_len == 0u || recording_id_len > POLAR_SDK_H10_RECORDING_ID_MAX_BYTES) {
        return false;
    }

    if (sample_type != POLAR_SDK_H10_RECORDING_SAMPLE_HEART_RATE &&
        sample_type != POLAR_SDK_H10_RECORDING_SAMPLE_RR_INTERVAL) {
        return false;
    }

    if (interval != POLAR_SDK_H10_RECORDING_INTERVAL_1S &&
        interval != POLAR_SDK_H10_RECORDING_INTERVAL_5S) {
        return false;
    }

#if !POLAR_CFG_ENABLE_PSFTP
    (void)out_payload_capacity;
    return false;
#else
    polar_sdk_psftp_string_view_t recording_id_view = {
        .data = (const uint8_t *)recording_id,
        .len = recording_id_len,
    };

    protocol_PbPFtpRequestStartRecordingParams params =
        protocol_PbPFtpRequestStartRecordingParams_init_zero;
    params.sample_type = (PbSampleType)sample_type;
    params.recording_interval.has_seconds = true;
    params.recording_interval.seconds = (uint32_t)interval;
    params.sample_data_identifier.funcs.encode = polar_sdk_psftp_encode_string_cb;
    params.sample_data_identifier.arg = &recording_id_view;

    pb_ostream_t stream = pb_ostream_from_buffer(out_payload, out_payload_capacity);
    if (!pb_encode(&stream, protocol_PbPFtpRequestStartRecordingParams_fields, &params)) {
        return false;
    }

    if (out_payload_len != 0) {
        *out_payload_len = stream.bytes_written;
    }
    return true;
#endif
}

bool polar_sdk_psftp_decode_h10_recording_status_result(
    const uint8_t *payload,
    size_t payload_len,
    bool *out_recording_on,
    char *out_recording_id,
    size_t out_recording_id_capacity,
    size_t *out_recording_id_len) {
    if (out_recording_on != 0) {
        *out_recording_on = false;
    }
    if (out_recording_id_len != 0) {
        *out_recording_id_len = 0;
    }
    if (out_recording_id != 0 && out_recording_id_capacity > 0u) {
        out_recording_id[0] = '\0';
    }

    if ((payload_len > 0u && payload == 0) ||
        (out_recording_id_capacity > 0u && out_recording_id == 0)) {
        return false;
    }

#if !POLAR_CFG_ENABLE_PSFTP
    (void)payload;
    (void)payload_len;
    return false;
#else
    polar_sdk_psftp_string_decode_ctx_t id_ctx = {
        .buffer = out_recording_id,
        .capacity = out_recording_id_capacity,
        .len = 0,
        .present = false,
        .overflow = false,
    };

    protocol_PbRequestRecordingStatusResult result = protocol_PbRequestRecordingStatusResult_init_zero;
    result.sample_data_identifier.funcs.decode = polar_sdk_psftp_decode_string_cb;
    result.sample_data_identifier.arg = &id_ctx;

    pb_istream_t stream = pb_istream_from_buffer(payload, payload_len);
    if (!pb_decode(&stream, protocol_PbRequestRecordingStatusResult_fields, &result)) {
        return false;
    }
    if (id_ctx.overflow) {
        return false;
    }

    if (out_recording_on != 0) {
        *out_recording_on = result.recording_on;
    }
    if (out_recording_id_len != 0) {
        *out_recording_id_len = id_ctx.present ? id_ctx.len : 0u;
    }
    return true;
#endif
}

#if POLAR_CFG_ENABLE_PSFTP
typedef struct {
    polar_sdk_psftp_dir_entry_t *entries;
    size_t capacity;
    size_t count;
    bool overflow;
} polar_sdk_psftp_dir_decode_ctx_t;

static bool polar_sdk_psftp_decode_directory_entry_cb(
    pb_istream_t *stream,
    const pb_field_t *field,
    void **arg) {
    (void)field;

    if (arg == 0 || *arg == 0) {
        return false;
    }

    polar_sdk_psftp_dir_decode_ctx_t *ctx = (polar_sdk_psftp_dir_decode_ctx_t *)(*arg);
    if (ctx->count >= ctx->capacity) {
        ctx->overflow = true;
        return false;
    }

    protocol_PbPFtpEntry entry = protocol_PbPFtpEntry_init_zero;
    if (!pb_decode(stream, protocol_PbPFtpEntry_fields, &entry)) {
        return false;
    }

    polar_sdk_psftp_dir_entry_t *out = &ctx->entries[ctx->count];
    size_t name_len = polar_sdk_psftp_bounded_strlen(entry.name, sizeof(entry.name));
    if (name_len > POLAR_SDK_PSFTP_MAX_ENTRY_NAME_BYTES) {
        name_len = POLAR_SDK_PSFTP_MAX_ENTRY_NAME_BYTES;
    }

    if (name_len > 0u) {
        memcpy(out->name, entry.name, name_len);
    }
    out->name[name_len] = '\0';
    out->size = entry.size;

    ctx->count += 1u;
    return true;
}
#endif

polar_sdk_psftp_dir_decode_result_t polar_sdk_psftp_decode_directory(
    const uint8_t *payload,
    size_t payload_len,
    polar_sdk_psftp_dir_entry_t *out_entries,
    size_t out_entries_capacity,
    size_t *out_entry_count) {
    if (out_entry_count != 0) {
        *out_entry_count = 0;
    }

    if ((payload_len > 0u && payload == 0) || (out_entries_capacity > 0u && out_entries == 0)) {
        return POLAR_SDK_PSFTP_DIR_DECODE_INVALID_ARGS;
    }

#if !POLAR_CFG_ENABLE_PSFTP
    (void)payload;
    (void)payload_len;
    (void)out_entries;
    (void)out_entries_capacity;
    return POLAR_SDK_PSFTP_DIR_DECODE_FAILED;
#else
    polar_sdk_psftp_dir_decode_ctx_t ctx = {
        .entries = out_entries,
        .capacity = out_entries_capacity,
        .count = 0,
        .overflow = false,
    };

    protocol_PbPFtpDirectory directory = protocol_PbPFtpDirectory_init_zero;
    directory.entries.funcs.decode = polar_sdk_psftp_decode_directory_entry_cb;
    directory.entries.arg = &ctx;

    pb_istream_t stream = pb_istream_from_buffer(payload, payload_len);
    if (!pb_decode(&stream, protocol_PbPFtpDirectory_fields, &directory)) {
        if (ctx.overflow) {
            return POLAR_SDK_PSFTP_DIR_DECODE_TOO_MANY_ENTRIES;
        }
        return POLAR_SDK_PSFTP_DIR_DECODE_FAILED;
    }

    if (out_entry_count != 0) {
        *out_entry_count = ctx.count;
    }
    return POLAR_SDK_PSFTP_DIR_DECODE_OK;
#endif
}
