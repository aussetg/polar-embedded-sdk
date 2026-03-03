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

bool polar_sdk_psftp_encode_get_operation(
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
    operation.command = protocol_PbPFtpOperation_Command_GET;

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
