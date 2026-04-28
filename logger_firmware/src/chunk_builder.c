#include "logger/chunk_builder.h"

#include <string.h>

#include "pico.h"

/* ── little-endian encoding helpers ────────────────────────────── */

static inline void cb_put_u16le(uint8_t *dst, uint16_t v) {
  dst[0] = (uint8_t)v;
  dst[1] = (uint8_t)(v >> 8);
}

static inline void cb_put_u32le(uint8_t *dst, uint32_t v) {
  dst[0] = (uint8_t)v;
  dst[1] = (uint8_t)(v >> 8);
  dst[2] = (uint8_t)(v >> 16);
  dst[3] = (uint8_t)(v >> 24);
}

static inline void cb_put_u64le(uint8_t *dst, uint64_t v) {
  for (size_t i = 0u; i < 8u; ++i)
    dst[i] = (uint8_t)(v >> (8u * i));
}

static inline size_t cb_align4(size_t n) { return (n + 3u) & ~(size_t)3u; }

/* ── public API ────────────────────────────────────────────────── */

void logger_chunk_builder_init(logger_chunk_builder_t *cb, uint8_t *buf,
                               size_t buf_cap) {
  cb->buf = buf;
  cb->buf_cap = buf_cap;
  cb->has_data = false;
  cb->stream_kind = 0u;
  memset(cb->span_id_raw, 0, 16u);
  cb->packet_count = 0u;
  cb->first_seq_in_span = 0u;
  cb->last_seq_in_span = 0u;
  cb->first_mono_us = 0u;
  cb->last_mono_us = 0u;
  cb->first_utc_ns = 0;
  cb->last_utc_ns = 0;
  cb->entries_write_offset = LOGGER_CHUNK_HEADER_BYTES;
  cb->first_packet_time_ms = 0u;
}

void logger_chunk_builder_reset(logger_chunk_builder_t *cb) {
  cb->has_data = false;
  cb->stream_kind = 0u;
  memset(cb->span_id_raw, 0, 16u);
  cb->packet_count = 0u;
  cb->first_seq_in_span = 0u;
  cb->last_seq_in_span = 0u;
  cb->first_mono_us = 0u;
  cb->last_mono_us = 0u;
  cb->first_utc_ns = 0;
  cb->last_utc_ns = 0;
  cb->entries_write_offset = LOGGER_CHUNK_HEADER_BYTES;
  cb->first_packet_time_ms = 0u;
}

logger_chunk_result_t __time_critical_func(logger_chunk_builder_append)(
    logger_chunk_builder_t *cb, uint16_t stream_kind,
    const uint8_t span_id_raw[16], uint32_t seq_in_span, uint64_t mono_us,
    int64_t utc_ns, const uint8_t *value, size_t value_len, uint32_t now_ms) {
  if (value == NULL || value_len == 0u || value_len > 0xffffu) {
    return LOGGER_CHUNK_FULL;
  }

  /* If builder has data and stream/span changed, caller must seal first */
  if (cb->has_data && (cb->stream_kind != stream_kind ||
                       memcmp(cb->span_id_raw, span_id_raw, 16u) != 0)) {
    return LOGGER_CHUNK_FULL;
  }

  const size_t entry_unpadded = LOGGER_CHUNK_ENTRY_HEADER_BYTES + value_len;
  const size_t entry_len = cb_align4(entry_unpadded);
  const size_t total_after = cb->entries_write_offset + entry_len;

  /* Won't fit in buffer */
  if (total_after > cb->buf_cap) {
    return LOGGER_CHUNK_FULL;
  }

  /* Capture stream/span on first packet, update running bounds */
  if (!cb->has_data) {
    cb->stream_kind = stream_kind;
    memcpy(cb->span_id_raw, span_id_raw, 16u);
    cb->first_mono_us = mono_us;
    cb->first_utc_ns = utc_ns;
    cb->first_seq_in_span = seq_in_span;
    cb->first_packet_time_ms = now_ms;
  }
  cb->last_mono_us = mono_us;
  cb->last_utc_ns = utc_ns;
  cb->last_seq_in_span = seq_in_span;
  cb->packet_count += 1u;
  cb->has_data = true;

  /* Write entry: zero first (covers padding), then fill fields */
  uint8_t *entry = cb->buf + cb->entries_write_offset;
  memset(entry, 0, entry_len);
  cb_put_u32le(entry + 0u, seq_in_span);
  /* entry + 4: flags = 0 */
  cb_put_u64le(entry + 8u, mono_us);
  cb_put_u64le(entry + 16u, (uint64_t)utc_ns);
  cb_put_u16le(entry + 24u, (uint16_t)value_len);
  /* entry + 26: reserved = 0 */
  memcpy(entry + LOGGER_CHUNK_ENTRY_HEADER_BYTES, value, value_len);
  cb->entries_write_offset += entry_len;

  /* Target size reached? */
  if (cb->entries_write_offset >= LOGGER_CHUNK_TARGET_SIZE) {
    return LOGGER_CHUNK_SEAL;
  }
  return LOGGER_CHUNK_OK;
}

bool __time_critical_func(logger_chunk_builder_age_exceeded)(
    const logger_chunk_builder_t *cb, uint32_t now_ms) {
  if (!cb->has_data)
    return false;
  return (now_ms - cb->first_packet_time_ms) >= LOGGER_CHUNK_MAX_AGE_MS;
}

bool __time_critical_func(logger_chunk_builder_has_data)(
    const logger_chunk_builder_t *cb) {
  return cb->has_data;
}

bool __time_critical_func(logger_chunk_builder_seal)(
    logger_chunk_builder_t *cb, uint32_t chunk_seq_in_session,
    const uint8_t **payload_out, size_t *payload_len_out) {
  if (!cb->has_data)
    return false;

  const size_t entries_bytes =
      cb->entries_write_offset - LOGGER_CHUNK_HEADER_BYTES;

  /* Write the 80-byte chunk header at buffer start */
  uint8_t *h = cb->buf;
  memset(h, 0, LOGGER_CHUNK_HEADER_BYTES);

  cb_put_u16le(h + 0u, cb->stream_kind);
  cb_put_u16le(h + 2u, LOGGER_CHUNK_ENCODING_RAW_PMD_V1);
  cb_put_u32le(h + 4u, chunk_seq_in_session);
  memcpy(h + 8u, cb->span_id_raw, 16u);
  cb_put_u32le(h + 24u, cb->packet_count);
  cb_put_u32le(h + 28u, cb->first_seq_in_span);
  cb_put_u32le(h + 32u, cb->last_seq_in_span);
  /* h + 36: reserved0 = 0 */
  cb_put_u64le(h + 40u, cb->first_mono_us);
  cb_put_u64le(h + 48u, cb->last_mono_us);
  cb_put_u64le(h + 56u, (uint64_t)cb->first_utc_ns);
  cb_put_u64le(h + 64u, (uint64_t)cb->last_utc_ns);
  cb_put_u32le(h + 72u, (uint32_t)entries_bytes);
  /* h + 76: reserved1 = 0 */

  *payload_out = cb->buf;
  *payload_len_out = cb->entries_write_offset;
  return true;
}

void __time_critical_func(logger_chunk_builder_clear)(
    logger_chunk_builder_t *cb) {
  cb->has_data = false;
  cb->packet_count = 0u;
  cb->first_seq_in_span = 0u;
  cb->last_seq_in_span = 0u;
  cb->first_mono_us = 0u;
  cb->last_mono_us = 0u;
  cb->first_utc_ns = 0;
  cb->last_utc_ns = 0;
  cb->entries_write_offset = LOGGER_CHUNK_HEADER_BYTES;
  cb->first_packet_time_ms = 0u;
  /* stream_kind and span_id_raw preserved for next chunk in same span */
}
