#include "logger/session.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "ff.h"
#include "pico/rand.h"
#include "pico/stdlib.h"

#include "board_config.h"
#include "logger/capture_stats.h"
#include "logger/h10.h"
#include "logger/json.h"
#include "logger/json_writer.h"
#include "logger/queue.h"
#include "logger/sha256.h"
#include "logger/util.h"
#include "logger/version.h"

#define LOGGER_SESSION_JSON_MAX 1024
#define LOGGER_SESSION_MANIFEST_MAX 8192
#define LOGGER_SESSION_LIVE_JSON_TOKEN_MAX 64u

/* Chunk assembly buffer: 64 KiB static, no heap in the hot path */
#define LOGGER_SESSION_CHUNK_BUF_SIZE (64u * 1024u)
static uint8_t g_chunk_buf[LOGGER_SESSION_CHUNK_BUF_SIZE];

static logger_capture_stats_t *g_session_stats = NULL;

void logger_session_set_capture_stats(logger_capture_stats_t *stats) {
  g_session_stats = stats;
}

typedef struct {
  char *buf;
  size_t cap;
  size_t len;
  bool ok;
} logger_sb_t;

static void logger_random_hex128(char out[LOGGER_SESSION_ID_HEX_LEN + 1]) {
  static const char hex[] = "0123456789abcdef";
  rng_128_t random128;
  get_rand_128(&random128);
  const uint8_t *bytes = (const uint8_t *)&random128;
  for (size_t i = 0u; i < 16u; ++i) {
    out[i * 2u] = hex[(bytes[i] >> 4) & 0x0f];
    out[(i * 2u) + 1u] = hex[bytes[i] & 0x0f];
  }
  out[LOGGER_SESSION_ID_HEX_LEN] = '\0';
}

static int64_t
logger_session_observed_utc_ns_or_zero(const logger_clock_status_t *clock) {
  int64_t utc_ns = 0ll;
  (void)logger_clock_observed_utc_ns(clock, &utc_ns);
  return utc_ns;
}

static void logger_sb_init(logger_sb_t *sb, char *buf, size_t cap) {
  sb->buf = buf;
  sb->cap = cap;
  sb->len = 0u;
  sb->ok = cap > 0u;
  if (cap > 0u) {
    buf[0] = '\0';
  }
}

static void logger_sb_append(logger_sb_t *sb, const char *text) {
  if (!sb->ok || text == NULL) {
    return;
  }
  const size_t text_len = strlen(text);
  if ((sb->len + text_len + 1u) > sb->cap) {
    sb->ok = false;
    return;
  }
  memcpy(sb->buf + sb->len, text, text_len + 1u);
  sb->len += text_len;
}

static void logger_sb_appendf(logger_sb_t *sb, const char *fmt, ...) {
  if (!sb->ok) {
    return;
  }
  va_list ap;
  va_start(ap, fmt);
  const int n = vsnprintf(sb->buf + sb->len, sb->cap - sb->len, fmt, ap);
  va_end(ap);
  if (n < 0 || (size_t)n >= (sb->cap - sb->len)) {
    sb->ok = false;
    return;
  }
  sb->len += (size_t)n;
}

static void logger_sb_append_json_string_or_null(logger_sb_t *sb,
                                                 const char *value) {
  char literal[256];
  logger_json_string_literal(literal, sizeof(literal), value);
  logger_sb_append(sb, literal);
}

static void
logger_session_recompute_quarantine(logger_session_state_t *session,
                                    const logger_clock_status_t *clock) {
  if (session->quarantine_clock_jump) {
    logger_copy_string(session->clock_state, sizeof(session->clock_state),
                       "jumped");
  } else if (session->quarantine_clock_invalid_at_start ||
             session->quarantine_clock_fixed_mid_session ||
             (clock != NULL && !clock->valid)) {
    logger_copy_string(session->clock_state, sizeof(session->clock_state),
                       "invalid");
  } else if (clock != NULL && clock->valid) {
    logger_copy_string(session->clock_state, sizeof(session->clock_state),
                       "valid");
  } else {
    logger_copy_string(session->clock_state, sizeof(session->clock_state),
                       "invalid");
  }
  session->quarantined = session->quarantine_clock_invalid_at_start ||
                         session->quarantine_clock_fixed_mid_session ||
                         session->quarantine_clock_jump ||
                         session->quarantine_recovery_after_reset ||
                         (clock != NULL && !clock->valid);
}

static bool logger_session_set_paths(logger_session_state_t *session) {
  const int dir_n =
      snprintf(session->session_dir_name, sizeof(session->session_dir_name),
               "%s__%s", session->study_day_local, session->session_id);
  const int dir_path_n =
      snprintf(session->session_dir_path, sizeof(session->session_dir_path),
               "0:/logger/sessions/%s", session->session_dir_name);
  const bool journal_ok =
      logger_path_join2(session->journal_path, sizeof(session->journal_path),
                        session->session_dir_path, "/journal.bin");
  const bool live_ok =
      logger_path_join2(session->live_path, sizeof(session->live_path),
                        session->session_dir_path, "/live.json");
  const bool manifest_ok =
      logger_path_join2(session->manifest_path, sizeof(session->manifest_path),
                        session->session_dir_path, "/manifest.json");
  return dir_n > 0 && (size_t)dir_n < sizeof(session->session_dir_name) &&
         dir_path_n > 0 &&
         (size_t)dir_path_n < sizeof(session->session_dir_path) && journal_ok &&
         live_ok && manifest_ok;
}

/*
 * Seal any in-flight chunk data and emit it to the journal.
 *
 * Updates span counters only on success, so span stats always
 * reflect durable journal bytes.
 *
 * No-op when the builder is empty.
 */
static bool logger_session_seal_active_chunk(logger_session_state_t *session) {
  logger_chunk_builder_t *cb = &session->chunk_builder;
  if (!logger_chunk_builder_has_data(cb)) {
    return true;
  }

  const uint32_t pkt_count = cb->packet_count;
  const uint32_t first_seq = cb->first_seq_in_span;
  const uint32_t last_seq = cb->last_seq_in_span;

  const uint8_t *payload = NULL;
  size_t payload_len = 0u;
  if (!logger_chunk_builder_seal(cb, session->next_chunk_seq_in_session,
                                 &payload, &payload_len)) {
    return false;
  }
  if (!logger_journal_append_binary_record(
          session->journal_path, LOGGER_JOURNAL_RECORD_DATA_CHUNK,
          session->next_record_seq++, payload, payload_len,
          &session->journal_size_bytes)) {
    /* Journal write failed — lose in-flight data, keep journal consistent */
    logger_chunk_builder_clear(cb);
    return false;
  }

  /* Update span counters from the chunk we just emitted */
  if (session->current_span_index < session->span_count) {
    logger_journal_span_summary_t *span =
        &session->spans[session->current_span_index];
    if (span->packet_count == 0u) {
      span->first_seq_in_span = first_seq;
    }
    span->packet_count += pkt_count;
    span->last_seq_in_span = last_seq;
  }

  session->next_chunk_seq_in_session++;
  logger_chunk_builder_clear(cb);
  return true;
}

static void
logger_session_quarantine_reasons_json(const logger_session_state_t *session,
                                       char out[128]) {
  logger_sb_t sb;
  logger_sb_init(&sb, out, 128u);
  logger_sb_append(&sb, "[");
  bool first = true;
  if (session->quarantine_clock_invalid_at_start) {
    logger_sb_append(&sb, "\"clock_invalid_at_start\"");
    first = false;
  }
  if (session->quarantine_clock_fixed_mid_session) {
    if (!first) {
      logger_sb_append(&sb, ",");
    }
    logger_sb_append(&sb, "\"clock_fixed_mid_session\"");
    first = false;
  }
  if (session->quarantine_clock_jump) {
    if (!first) {
      logger_sb_append(&sb, ",");
    }
    logger_sb_append(&sb, "\"clock_jump\"");
    first = false;
  }
  if (session->quarantine_recovery_after_reset) {
    if (!first) {
      logger_sb_append(&sb, ",");
    }
    logger_sb_append(&sb, "\"recovery_after_reset\"");
    first = false;
  }
  if (first) {
    logger_sb_append(&sb, "");
  }
  logger_sb_append(&sb, "]");
  if (!sb.ok) {
    logger_copy_string(out, 128u, "[]");
  }
}

static bool
logger_session_write_live_internal(const logger_session_state_t *session,
                                   const logger_clock_status_t *clock,
                                   uint32_t boot_counter, uint32_t now_ms) {
  char payload[640];
  char current_span_id_json[64];
  char current_span_index_json[24];
  char last_flush_utc_json[64];

  logger_json_string_literal(current_span_id_json, sizeof(current_span_id_json),
                             session->span_active ? session->current_span_id
                                                  : NULL);
  if (session->span_active) {
    snprintf(current_span_index_json, sizeof(current_span_index_json), "%lu",
             (unsigned long)session->current_span_index);
  } else {
    logger_copy_string(current_span_index_json, sizeof(current_span_index_json),
                       "null");
  }
  logger_json_string_literal(last_flush_utc_json, sizeof(last_flush_utc_json),
                             clock != NULL ? clock->now_utc : NULL);

  const int n = snprintf(
      payload, sizeof(payload),
      "{\"schema_version\":1,\"session_id\":\"%s\",\"study_day_local\":\"%s\","
      "\"session_dir\":\"%s\",\"state\":\"active\",\"journal_size_bytes\":%llu,"
      "\"last_flush_utc\":%s,\"last_flush_mono_us\":%llu,\"current_span_id\":%"
      "s,\"current_span_index\":%s,\"quarantined\":%s,\"clock_state\":\"%s\","
      "\"boot_counter\":%lu}",
      session->session_id, session->study_day_local, session->session_dir_name,
      (unsigned long long)session->journal_size_bytes, last_flush_utc_json,
      (unsigned long long)now_ms * 1000ull, current_span_id_json,
      current_span_index_json, session->quarantined ? "true" : "false",
      session->clock_state, (unsigned long)boot_counter);
  return n > 0 && (size_t)n < sizeof(payload) &&
         logger_storage_write_file_atomic(session->live_path, payload,
                                          (size_t)n);
}

static bool
logger_session_append_session_start(logger_session_state_t *session,
                                    const logger_persisted_state_t *persisted,
                                    const logger_clock_status_t *clock,
                                    uint32_t boot_counter, uint32_t now_ms) {
  char logger_id_escaped[LOGGER_CONFIG_LOGGER_ID_MAX * 2u];
  char subject_id_escaped[LOGGER_CONFIG_SUBJECT_ID_MAX * 2u];
  char timezone_escaped[LOGGER_CONFIG_TIMEZONE_MAX * 2u];
  logger_json_escape_into(logger_id_escaped, sizeof(logger_id_escaped),
                          persisted->config.logger_id);
  logger_json_escape_into(subject_id_escaped, sizeof(subject_id_escaped),
                          persisted->config.subject_id);
  logger_json_escape_into(timezone_escaped, sizeof(timezone_escaped),
                          persisted->config.timezone);

  char payload[LOGGER_SESSION_JSON_MAX];
  const int n = snprintf(
      payload, sizeof(payload),
      "{\"schema_version\":1,\"record_type\":\"session_start\",\"utc_ns\":%lld,"
      "\"mono_us\":%llu,\"boot_counter\":%lu,\"session_id\":\"%s\",\"study_day_"
      "local\":\"%s\",\"logger_id\":\"%s\",\"subject_id\":\"%s\",\"timezone\":"
      "\"%s\",\"clock_state\":\"%s\",\"start_reason\":\"%s\"}",
      (long long)logger_session_observed_utc_ns_or_zero(clock),
      (unsigned long long)now_ms * 1000ull, (unsigned long)boot_counter,
      session->session_id, session->study_day_local, logger_id_escaped,
      subject_id_escaped, timezone_escaped, session->clock_state,
      session->session_start_reason);
  return n > 0 && (size_t)n < sizeof(payload) &&
         logger_journal_append_json_record(
             session->journal_path, LOGGER_JOURNAL_RECORD_SESSION_START,
             session->next_record_seq++, payload, &session->journal_size_bytes);
}

static bool logger_session_begin_span(logger_session_state_t *session,
                                      const logger_clock_status_t *clock,
                                      const char *start_reason,
                                      const char *h10_address, bool encrypted,
                                      bool bonded, uint32_t boot_counter,
                                      uint32_t now_ms) {
  if (session->span_count >= LOGGER_JOURNAL_MAX_SPANS) {
    return false;
  }

  logger_journal_span_summary_t *span = &session->spans[session->span_count];
  memset(span, 0, sizeof(*span));
  span->present = true;
  logger_random_hex128(span->span_id);
  logger_copy_string(session->current_span_id, sizeof(session->current_span_id),
                     span->span_id);
  session->current_span_index = session->span_count;
  session->span_active = true;

  /* Cache raw span_id for chunk builder (avoids per-packet hex conversion) */
  if (!logger_hex_to_bytes_16(span->span_id, session->current_span_id_raw)) {
    session->span_count -= 1u;
    session->span_active = false;
    session->current_span_id[0] = '\0';
    session->current_span_index = 0xffffffffu;
    memset(span, 0, sizeof(*span));
    return false;
  }

  /* Reset chunk builder for new span */
  logger_chunk_builder_reset(&session->chunk_builder);

  logger_copy_string(span->start_reason, sizeof(span->start_reason),
                     start_reason);
  logger_copy_string(span->start_utc, sizeof(span->start_utc),
                     clock != NULL ? clock->now_utc : NULL);
  session->span_count += 1u;
  session->next_packet_seq_in_span = 0u;

  char h10_escaped[LOGGER_CONFIG_BOUND_H10_ADDR_MAX * 2u];
  logger_json_escape_into(h10_escaped, sizeof(h10_escaped), h10_address);

  char payload[LOGGER_SESSION_JSON_MAX];
  const int n = snprintf(
      payload, sizeof(payload),
      "{\"schema_version\":1,\"record_type\":\"span_start\",\"utc_ns\":%lld,"
      "\"mono_us\":%llu,\"boot_counter\":%lu,\"session_id\":\"%s\",\"span_id\":"
      "\"%s\",\"span_index_in_session\":%lu,\"start_reason\":\"%s\",\"h10_"
      "address\":\"%s\",\"encrypted\":%s,\"bonded\":%s}",
      (long long)logger_session_observed_utc_ns_or_zero(clock),
      (unsigned long long)now_ms * 1000ull, (unsigned long)boot_counter,
      session->session_id, span->span_id,
      (unsigned long)session->current_span_index, start_reason, h10_escaped,
      encrypted ? "true" : "false", bonded ? "true" : "false");
  if (!(n > 0 && (size_t)n < sizeof(payload)) ||
      !logger_journal_append_json_record(
          session->journal_path, LOGGER_JOURNAL_RECORD_SPAN_START,
          session->next_record_seq++, payload, &session->journal_size_bytes)) {
    session->span_count -= 1u;
    session->span_active = false;
    session->current_span_id[0] = '\0';
    session->current_span_index = 0xffffffffu;
    memset(span, 0, sizeof(*span));
    return false;
  }
  return true;
}

static bool logger_session_close_active_span(
    logger_session_state_t *session, const logger_clock_status_t *clock,
    const char *end_reason, uint32_t boot_counter, uint32_t now_ms,
    char ended_span_id_out[LOGGER_SESSION_ID_HEX_LEN + 1]) {
  if (!session->span_active ||
      session->current_span_index >= session->span_count) {
    return true;
  }

  /* Barrier: seal any in-flight chunk data before span_end */
  if (!logger_session_seal_active_chunk(session)) {
    return false;
  }

  logger_journal_span_summary_t *span =
      &session->spans[session->current_span_index];
  if (ended_span_id_out != NULL) {
    logger_copy_string(ended_span_id_out, LOGGER_SESSION_ID_HEX_LEN + 1,
                       span->span_id);
  }
  logger_copy_string(span->end_reason, sizeof(span->end_reason), end_reason);
  logger_copy_string(span->end_utc, sizeof(span->end_utc),
                     clock != NULL ? clock->now_utc : NULL);

  char payload[LOGGER_SESSION_JSON_MAX];
  const int n = snprintf(
      payload, sizeof(payload),
      "{\"schema_version\":1,\"record_type\":\"span_end\",\"utc_ns\":%lld,"
      "\"mono_us\":%llu,\"boot_counter\":%lu,\"session_id\":\"%s\",\"span_id\":"
      "\"%s\",\"end_reason\":\"%s\",\"packet_count\":%lu,\"first_seq_in_span\":"
      "%lu,\"last_seq_in_span\":%lu}",
      (long long)logger_session_observed_utc_ns_or_zero(clock),
      (unsigned long long)now_ms * 1000ull, (unsigned long)boot_counter,
      session->session_id, span->span_id, end_reason,
      (unsigned long)span->packet_count, (unsigned long)span->first_seq_in_span,
      (unsigned long)span->last_seq_in_span);
  if (!(n > 0 && (size_t)n < sizeof(payload)) ||
      !logger_journal_append_json_record(
          session->journal_path, LOGGER_JOURNAL_RECORD_SPAN_END,
          session->next_record_seq++, payload, &session->journal_size_bytes)) {
    return false;
  }

  session->span_active = false;
  session->current_span_id[0] = '\0';
  session->current_span_index = 0xffffffffu;
  return true;
}

static bool logger_session_append_gap(logger_session_state_t *session,
                                      const logger_clock_status_t *clock,
                                      const char *ended_span_id,
                                      const char *gap_reason,
                                      uint32_t boot_counter, uint32_t now_ms) {
  /* Barrier: spec §6.7 lists gap as an explicit seal trigger */
  if (!logger_session_seal_active_chunk(session)) {
    return false;
  }

  char payload[LOGGER_SESSION_JSON_MAX];
  const int n = snprintf(
      payload, sizeof(payload),
      "{\"schema_version\":1,\"record_type\":\"gap\",\"utc_ns\":%lld,\"mono_"
      "us\":%llu,\"boot_counter\":%lu,\"session_id\":\"%s\",\"ended_span_id\":"
      "\"%s\",\"gap_reason\":\"%s\"}",
      (long long)logger_session_observed_utc_ns_or_zero(clock),
      (unsigned long long)now_ms * 1000ull, (unsigned long)boot_counter,
      session->session_id, ended_span_id, gap_reason);
  return n > 0 && (size_t)n < sizeof(payload) &&
         logger_journal_append_json_record(
             session->journal_path, LOGGER_JOURNAL_RECORD_GAP,
             session->next_record_seq++, payload, &session->journal_size_bytes);
}

static bool logger_session_append_recovery(logger_session_state_t *session,
                                           const logger_clock_status_t *clock,
                                           const char *reason,
                                           uint32_t boot_counter,
                                           uint32_t now_ms) {
  char payload[LOGGER_SESSION_JSON_MAX];
  const int n = snprintf(
      payload, sizeof(payload),
      "{\"schema_version\":1,\"record_type\":\"recovery\",\"utc_ns\":%lld,"
      "\"mono_us\":%llu,\"boot_counter\":%lu,\"session_id\":\"%s\",\"recovery_"
      "reason\":\"%s\",\"previous_reset_cause\":\"unknown\"}",
      (long long)logger_session_observed_utc_ns_or_zero(clock),
      (unsigned long long)now_ms * 1000ull, (unsigned long)boot_counter,
      session->session_id, reason);
  return n > 0 && (size_t)n < sizeof(payload) &&
         logger_journal_append_json_record(
             session->journal_path, LOGGER_JOURNAL_RECORD_RECOVERY,
             session->next_record_seq++, payload, &session->journal_size_bytes);
}

static bool logger_session_append_session_end(
    logger_session_state_t *session, const logger_clock_status_t *clock,
    const char *end_reason, uint32_t boot_counter, uint32_t now_ms) {
  /* Barrier: spec §6.7 lists session_end as an explicit seal trigger */
  if (!logger_session_seal_active_chunk(session)) {
    return false;
  }

  char reasons_json[128];
  logger_session_quarantine_reasons_json(session, reasons_json);
  logger_copy_string(session->session_end_utc, sizeof(session->session_end_utc),
                     clock != NULL ? clock->now_utc : NULL);
  logger_copy_string(session->session_end_reason,
                     sizeof(session->session_end_reason), end_reason);

  char payload[LOGGER_SESSION_JSON_MAX];
  const int n = snprintf(
      payload, sizeof(payload),
      "{\"schema_version\":1,\"record_type\":\"session_end\",\"utc_ns\":%lld,"
      "\"mono_us\":%llu,\"boot_counter\":%lu,\"session_id\":\"%s\",\"end_"
      "reason\":\"%s\",\"span_count\":%lu,\"quarantined\":%s,\"quarantine_"
      "reasons\":%s}",
      (long long)logger_session_observed_utc_ns_or_zero(clock),
      (unsigned long long)now_ms * 1000ull, (unsigned long)boot_counter,
      session->session_id, end_reason, (unsigned long)session->span_count,
      session->quarantined ? "true" : "false", reasons_json);
  return n > 0 && (size_t)n < sizeof(payload) &&
         logger_journal_append_json_record(
             session->journal_path, LOGGER_JOURNAL_RECORD_SESSION_END,
             session->next_record_seq++, payload, &session->journal_size_bytes);
}

static bool
logger_session_compute_file_sha256(const char *path,
                                   char out_hex[LOGGER_SHA256_HEX_LEN + 1],
                                   uint64_t *size_bytes_out) {
  FIL file;
  if (f_open(&file, path, FA_READ) != FR_OK) {
    return false;
  }
  logger_sha256_t sha;
  logger_sha256_init(&sha);
  uint64_t total = 0u;
  uint8_t chunk[256];
  UINT read_bytes = 0u;
  do {
    if (f_read(&file, chunk, sizeof(chunk), &read_bytes) != FR_OK) {
      f_close(&file);
      return false;
    }
    if (read_bytes > 0u) {
      logger_sha256_update(&sha, chunk, read_bytes);
      total += read_bytes;
    }
  } while (read_bytes == sizeof(chunk));
  if (f_close(&file) != FR_OK) {
    return false;
  }
  logger_sha256_final_hex(&sha, out_hex);
  if (size_bytes_out != NULL) {
    *size_bytes_out = total;
  }
  return true;
}

static bool logger_session_build_manifest(
    const logger_session_state_t *session, const char *hardware_id,
    const logger_persisted_state_t *persisted,
    const logger_storage_status_t *storage, const char *journal_sha256,
    uint64_t journal_size_bytes, char *manifest_out, size_t manifest_cap,
    size_t *manifest_len_out) {
  logger_sb_t sb;
  logger_sb_init(&sb, manifest_out, manifest_cap);

  char quarantine_reasons_json[128];
  logger_session_quarantine_reasons_json(session, quarantine_reasons_json);

  logger_sb_append(&sb, "{\"schema_version\":1,\"session_id\":");
  logger_sb_append_json_string_or_null(&sb, session->session_id);
  logger_sb_append(&sb, ",\"study_day_local\":");
  logger_sb_append_json_string_or_null(&sb, session->study_day_local);
  logger_sb_append(&sb, ",\"logger_id\":");
  logger_sb_append_json_string_or_null(&sb, persisted->config.logger_id);
  logger_sb_append(&sb, ",\"subject_id\":");
  logger_sb_append_json_string_or_null(&sb, persisted->config.subject_id);
  logger_sb_append(&sb, ",\"hardware_id\":");
  logger_sb_append_json_string_or_null(&sb, hardware_id);
  logger_sb_append(&sb, ",\"firmware_version\":");
  logger_sb_append_json_string_or_null(&sb, LOGGER_FIRMWARE_VERSION);
  logger_sb_append(&sb, ",\"build_id\":");
  logger_sb_append_json_string_or_null(&sb, LOGGER_BUILD_ID);
  logger_sb_append(&sb, ",\"journal_format_version\":1,\"tar_canonicalization_"
                        "version\":1,\"timezone\":");
  logger_sb_append_json_string_or_null(&sb, persisted->config.timezone);

  logger_sb_append(&sb, ",\"session\":{\"start_utc\":");
  logger_sb_append_json_string_or_null(&sb, session->session_start_utc);
  logger_sb_append(&sb, ",\"end_utc\":");
  logger_sb_append_json_string_or_null(&sb, session->session_end_utc);
  logger_sb_append(&sb, ",\"start_reason\":");
  logger_sb_append_json_string_or_null(&sb, session->session_start_reason);
  logger_sb_append(&sb, ",\"end_reason\":");
  logger_sb_append_json_string_or_null(&sb, session->session_end_reason);
  logger_sb_appendf(
      &sb, ",\"span_count\":%lu,\"quarantined\":%s,\"quarantine_reasons\":%s}",
      (unsigned long)session->span_count,
      session->quarantined ? "true" : "false", quarantine_reasons_json);

  logger_sb_append(&sb, ",\"spans\":[");
  for (uint32_t i = 0u; i < session->span_count && sb.ok; ++i) {
    const logger_journal_span_summary_t *span = &session->spans[i];
    if (i != 0u) {
      logger_sb_append(&sb, ",");
    }
    logger_sb_append(&sb, "{\"span_id\":");
    logger_sb_append_json_string_or_null(&sb, span->span_id);
    logger_sb_appendf(
        &sb, ",\"index_in_session\":%lu,\"start_utc\":", (unsigned long)i);
    logger_sb_append_json_string_or_null(&sb, span->start_utc);
    logger_sb_append(&sb, ",\"end_utc\":");
    logger_sb_append_json_string_or_null(&sb, span->end_utc);
    logger_sb_append(&sb, ",\"start_reason\":");
    logger_sb_append_json_string_or_null(&sb, span->start_reason);
    logger_sb_append(&sb, ",\"end_reason\":");
    logger_sb_append_json_string_or_null(&sb, span->end_reason);
    logger_sb_appendf(&sb,
                      ",\"packet_count\":%lu,\"first_seq_in_span\":%lu,\"last_"
                      "seq_in_span\":%lu}",
                      (unsigned long)span->packet_count,
                      (unsigned long)span->first_seq_in_span,
                      (unsigned long)span->last_seq_in_span);
  }
  logger_sb_append(&sb, "]");

  logger_sb_append(&sb, ",\"config_snapshot\":{\"bound_h10_address\":");
  logger_sb_append_json_string_or_null(&sb,
                                       persisted->config.bound_h10_address);
  logger_sb_append(&sb, ",\"timezone\":");
  logger_sb_append_json_string_or_null(&sb, persisted->config.timezone);
  logger_sb_appendf(
      &sb,
      ",\"study_day_rollover_local\":\"%02u:00:00\",\"overnight_upload_window_"
      "start_local\":\"%02u:00:00\",\"overnight_upload_window_end_local\":\"%"
      "02u:00:00\",\"critical_stop_voltage_v\":3.5,\"low_start_voltage_v\":3."
      "65,\"off_charger_upload_voltage_v\":3.85}",
      (unsigned)LOGGER_STUDY_DAY_ROLLOVER_HOUR_LOCAL,
      (unsigned)LOGGER_OVERNIGHT_UPLOAD_WINDOW_START_HOUR_LOCAL,
      (unsigned)LOGGER_OVERNIGHT_UPLOAD_WINDOW_END_HOUR_LOCAL);

  logger_sb_append(&sb, ",\"h10\":{\"bound_address\":");
  logger_sb_append_json_string_or_null(&sb,
                                       persisted->config.bound_h10_address);
  logger_sb_append(
      &sb, ",\"connected_address_first\":null,\"model_number\":null,\"serial_"
           "number\":null,\"firmware_revision\":null,\"battery_percent_first\":"
           "null,\"battery_percent_last\":null}");

  logger_sb_appendf(&sb,
                    ",\"storage\":{\"sd_capacity_bytes\":%llu,\"sd_identity\":{"
                    "\"manufacturer_id\":",
                    (unsigned long long)storage->capacity_bytes);
  logger_sb_append_json_string_or_null(&sb, storage->manufacturer_id);
  logger_sb_append(&sb, ",\"oem_id\":");
  logger_sb_append_json_string_or_null(&sb, storage->oem_id);
  logger_sb_append(&sb, ",\"product_name\":");
  logger_sb_append_json_string_or_null(&sb, storage->product_name);
  logger_sb_append(&sb, ",\"revision\":");
  logger_sb_append_json_string_or_null(&sb, storage->revision);
  logger_sb_append(&sb, ",\"serial_number\":");
  logger_sb_append_json_string_or_null(&sb, storage->serial_number);
  logger_sb_append(&sb, "},\"filesystem\":");
  logger_sb_append_json_string_or_null(&sb, storage->filesystem);
  logger_sb_append(&sb, "}");

  logger_sb_appendf(&sb,
                    ",\"files\":[{\"name\":\"journal.bin\",\"size_bytes\":%llu,"
                    "\"sha256\":\"%s\"}]",
                    (unsigned long long)journal_size_bytes, journal_sha256);
  logger_sb_appendf(
      &sb,
      ",\"upload_bundle\":{\"format\":\"tar\",\"compression\":\"none\","
      "\"canonicalization_version\":1,\"root_dir_name\":\"%s\",\"file_order\":["
      "\"manifest.json\",\"journal.bin\"]}}",
      session->session_dir_name);

  if (!sb.ok) {
    return false;
  }
  if (manifest_len_out != NULL) {
    *manifest_len_out = sb.len;
  }
  return true;
}

static bool logger_session_finalize_internal(
    logger_session_state_t *session, logger_system_log_t *system_log,
    const char *hardware_id, const logger_persisted_state_t *persisted,
    const logger_clock_status_t *clock, const logger_storage_status_t *storage,
    const char *end_reason, bool debug_session, uint32_t boot_counter,
    uint32_t now_ms) {
  if (!session->active) {
    return false;
  }

  logger_session_recompute_quarantine(session, clock);

  if (!logger_session_close_active_span(session, clock, end_reason,
                                        boot_counter, now_ms, NULL)) {
    return false;
  }
  if (!logger_session_append_session_end(session, clock, end_reason,
                                         boot_counter, now_ms)) {
    return false;
  }

  char journal_sha256[LOGGER_SHA256_HEX_LEN + 1];
  uint64_t journal_size_bytes = 0u;
  if (!logger_session_compute_file_sha256(session->journal_path, journal_sha256,
                                          &journal_size_bytes)) {
    return false;
  }

  static char manifest[LOGGER_SESSION_MANIFEST_MAX];
  size_t manifest_len = 0u;
  if (!logger_session_build_manifest(
          session, hardware_id, persisted, storage, journal_sha256,
          journal_size_bytes, manifest, sizeof(manifest), &manifest_len)) {
    return false;
  }
  if (!logger_storage_write_file_atomic(session->manifest_path, manifest,
                                        manifest_len)) {
    return false;
  }

  (void)logger_storage_remove_file(session->live_path);
  if (!logger_upload_queue_refresh_file(
          system_log, clock != NULL ? clock->now_utc : NULL, NULL)) {
    return false;
  }

  char details[LOGGER_SYSTEM_LOG_DETAILS_JSON_MAX + 1];
  logger_json_object_writer_t writer;
  logger_json_object_writer_init(&writer, details, sizeof(details));
  if (logger_json_object_writer_bool_field(&writer, "debug", debug_session) &&
      logger_json_object_writer_string_field(&writer, "reason", end_reason) &&
      logger_json_object_writer_finish(&writer)) {
    (void)logger_system_log_append(
        system_log,
        clock != NULL && clock->now_utc[0] != '\0' ? clock->now_utc : NULL,
        "session_closed", LOGGER_SYSTEM_LOG_SEVERITY_INFO,
        logger_json_object_writer_data(&writer));
  }
  logger_session_init(session);
  return true;
}

static void logger_session_set_error(const char **error_code_out,
                                     const char **error_message_out,
                                     const char *code, const char *message) {
  if (error_code_out != NULL) {
    *error_code_out = code;
  }
  if (error_message_out != NULL) {
    *error_message_out = message;
  }
}

static bool logger_session_start_new_active_internal(
    logger_session_state_t *session, logger_system_log_t *system_log,
    const logger_persisted_state_t *persisted,
    const logger_clock_status_t *clock, const logger_storage_status_t *storage,
    const char *span_start_reason, const char *h10_address, bool encrypted,
    bool bonded, bool clock_jump_at_session_start, bool debug_session,
    uint32_t boot_counter, uint32_t now_ms, const char **error_code_out,
    const char **error_message_out) {
  logger_capture_stats_reset(g_session_stats);
  if (!logger_storage_ready_for_logging(storage)) {
    logger_session_set_error(error_code_out, error_message_out,
                             "storage_unavailable",
                             "storage is not ready for session artifacts");
    return false;
  }

  logger_session_init(session);
  session->quarantine_clock_invalid_at_start = clock != NULL && !clock->valid;
  session->quarantine_clock_jump = clock_jump_at_session_start;
  logger_session_recompute_quarantine(session, clock);

  if (!logger_clock_derive_study_day_local_observed(
          clock, persisted->config.timezone, session->study_day_local)) {
    logger_session_set_error(
        error_code_out, error_message_out, "invalid_config",
        "cannot derive study_day_local from current clock/timezone");
    return false;
  }

  logger_random_hex128(session->session_id);
  logger_copy_string(session->session_start_utc,
                     sizeof(session->session_start_utc),
                     clock != NULL ? clock->now_utc : NULL);
  logger_copy_string(session->session_start_reason,
                     sizeof(session->session_start_reason),
                     "first_span_of_session");
  session->next_chunk_seq_in_session = 0u;
  session->next_packet_seq_in_span = 0u;
  if (!logger_session_set_paths(session)) {
    logger_session_set_error(error_code_out, error_message_out,
                             "storage_unavailable",
                             "session path exceeds buffer");
    logger_session_init(session);
    return false;
  }
  if (!logger_storage_ensure_dir(session->session_dir_path)) {
    logger_session_set_error(error_code_out, error_message_out,
                             "storage_unavailable",
                             "failed to create session directory");
    logger_session_init(session);
    return false;
  }
  if (!logger_journal_create(session->journal_path, session->session_id,
                             boot_counter,
                             logger_session_observed_utc_ns_or_zero(clock),
                             &session->journal_size_bytes)) {
    logger_session_set_error(error_code_out, error_message_out,
                             "storage_unavailable",
                             "failed to create journal.bin");
    logger_session_init(session);
    return false;
  }
  if (!logger_session_append_session_start(session, persisted, clock,
                                           boot_counter, now_ms) ||
      !logger_session_begin_span(session, clock, span_start_reason, h10_address,
                                 encrypted, bonded, boot_counter, now_ms)) {
    logger_session_set_error(error_code_out, error_message_out,
                             "storage_unavailable",
                             "failed to write initial journal records");
    logger_session_init(session);
    return false;
  }

  session->active = true;
  if (!logger_session_write_live_internal(session, clock, boot_counter,
                                          now_ms)) {
    logger_session_set_error(error_code_out, error_message_out,
                             "storage_unavailable",
                             "failed to write live.json");
    logger_session_init(session);
    return false;
  }

  char details[LOGGER_SYSTEM_LOG_DETAILS_JSON_MAX + 1];
  logger_json_object_writer_t writer;
  logger_json_object_writer_init(&writer, details, sizeof(details));
  if (logger_json_object_writer_bool_field(&writer, "debug", debug_session) &&
      logger_json_object_writer_finish(&writer)) {
    (void)logger_system_log_append(
        system_log,
        clock != NULL && clock->now_utc[0] != '\0' ? clock->now_utc : NULL,
        "session_started", LOGGER_SYSTEM_LOG_SEVERITY_INFO,
        logger_json_object_writer_data(&writer));
  }
  return true;
}

static void logger_session_log_recovery_issue(logger_system_log_t *system_log,
                                              const char *utc_or_null,
                                              const char *kind,
                                              const char *details_json) {
  if (system_log == NULL) {
    return;
  }
  (void)logger_system_log_append(system_log, utc_or_null, kind,
                                 LOGGER_SYSTEM_LOG_SEVERITY_WARN,
                                 details_json != NULL ? details_json : "{}");
}

static bool logger_session_find_live_paths(
    char dir_name_out[64], char dir_path_out[LOGGER_STORAGE_PATH_MAX],
    char journal_path_out[LOGGER_STORAGE_PATH_MAX],
    char live_path_out[LOGGER_STORAGE_PATH_MAX],
    char manifest_path_out[LOGGER_STORAGE_PATH_MAX]) {
  DIR dir;
  if (f_opendir(&dir, "0:/logger/sessions") != FR_OK) {
    return false;
  }

  bool found = false;
  for (;;) {
    FILINFO info;
    memset(&info, 0, sizeof(info));
    const FRESULT fr = f_readdir(&dir, &info);
    if (fr != FR_OK || info.fname[0] == '\0') {
      break;
    }
    if ((info.fattrib & AM_DIR) == 0u || strcmp(info.fname, ".") == 0 ||
        strcmp(info.fname, "..") == 0) {
      continue;
    }

    char live_path[LOGGER_STORAGE_PATH_MAX];
    if (!logger_path_join3(live_path, sizeof(live_path), "0:/logger/sessions/",
                           info.fname, "/live.json")) {
      continue;
    }
    if (!logger_storage_file_exists(live_path)) {
      continue;
    }

    logger_copy_string(dir_name_out, 64u, info.fname);
    if (!logger_path_join2(dir_path_out, LOGGER_STORAGE_PATH_MAX,
                           "0:/logger/sessions/", info.fname) ||
        !logger_path_join2(journal_path_out, LOGGER_STORAGE_PATH_MAX,
                           dir_path_out, "/journal.bin") ||
        !logger_path_join2(live_path_out, LOGGER_STORAGE_PATH_MAX, dir_path_out,
                           "/live.json") ||
        !logger_path_join2(manifest_path_out, LOGGER_STORAGE_PATH_MAX,
                           dir_path_out, "/manifest.json")) {
      continue;
    }
    found = true;
    break;
  }

  return f_closedir(&dir) == FR_OK && found;
}

static bool logger_session_load_live_session_id(
    const char *live_path, char session_id_out[LOGGER_SESSION_ID_HEX_LEN + 1]) {
  char buf[1024];
  size_t len = 0u;
  if (!logger_storage_read_file(live_path, buf, sizeof(buf) - 1u, &len)) {
    return false;
  }
  buf[len] = '\0';

  jsmntok_t tokens[LOGGER_SESSION_LIVE_JSON_TOKEN_MAX];
  logger_json_doc_t doc;
  if (!logger_json_parse(&doc, buf, len, tokens,
                         LOGGER_SESSION_LIVE_JSON_TOKEN_MAX)) {
    return false;
  }

  const jsmntok_t *root = logger_json_root(&doc);
  if (root == NULL || root->type != JSMN_OBJECT) {
    return false;
  }

  return logger_json_object_copy_string(&doc, root, "session_id",
                                        session_id_out,
                                        LOGGER_SESSION_ID_HEX_LEN + 1) &&
         strlen(session_id_out) == LOGGER_SESSION_ID_HEX_LEN;
}

void logger_session_init(logger_session_state_t *session) {
  memset(session, 0, sizeof(*session));
  session->current_span_index = 0xffffffffu;
  logger_chunk_builder_init(&session->chunk_builder, g_chunk_buf,
                            LOGGER_SESSION_CHUNK_BUF_SIZE);
}

bool logger_session_start_debug(
    logger_session_state_t *session, logger_system_log_t *system_log,
    const char *hardware_id, const logger_persisted_state_t *persisted,
    const logger_clock_status_t *clock, const logger_battery_status_t *battery,
    const logger_storage_status_t *storage, logger_fault_code_t current_fault,
    uint32_t boot_counter, uint32_t now_ms, const char **error_code_out,
    const char **error_message_out) {
  (void)hardware_id;
  (void)battery;
  (void)current_fault;

  if (session->active) {
    logger_session_set_error(error_code_out, error_message_out,
                             "not_permitted_in_mode",
                             "debug session is already active");
    return false;
  }
  return logger_session_start_new_active_internal(
      session, system_log, persisted, clock, storage, "session_start",
      persisted->config.bound_h10_address, false, false, false, true,
      boot_counter, now_ms, error_code_out, error_message_out);
}

bool logger_session_refresh_live(logger_session_state_t *session,
                                 const logger_clock_status_t *clock,
                                 uint32_t boot_counter, uint32_t now_ms) {
  if (!session->active) {
    return false;
  }
  logger_session_recompute_quarantine(session, clock);
  return logger_session_write_live_internal(session, clock, boot_counter,
                                            now_ms);
}

bool logger_session_write_status_snapshot(
    logger_session_state_t *session, const logger_clock_status_t *clock,
    const logger_battery_status_t *battery,
    const logger_storage_status_t *storage, logger_fault_code_t current_fault,
    uint32_t boot_counter, uint32_t now_ms) {
  if (!session->active) {
    return false;
  }

  /* Barrier: seal any in-flight chunk data before metadata record */
  if (!logger_session_seal_active_chunk(session)) {
    return false;
  }

  logger_session_recompute_quarantine(session, clock);

  char active_span_id_json[64];
  char fault_code_json[64];
  logger_json_string_literal(active_span_id_json, sizeof(active_span_id_json),
                             session->span_active ? session->current_span_id
                                                  : NULL);
  logger_json_string_literal(fault_code_json, sizeof(fault_code_json),
                             logger_fault_code_name(current_fault));

  char payload[LOGGER_SESSION_JSON_MAX];
  const int n = snprintf(
      payload, sizeof(payload),
      "{\"schema_version\":1,\"record_type\":\"status_snapshot\",\"utc_ns\":%"
      "lld,\"mono_us\":%llu,\"boot_counter\":%lu,\"session_id\":\"%s\","
      "\"active_span_id\":%s,\"battery_voltage_mv\":%u,\"battery_estimate_"
      "pct\":%d,\"vbus_present\":%s,\"sd_free_bytes\":%llu,\"sd_reserve_"
      "bytes\":%lu,\"wifi_enabled\":false,\"quarantined\":%s,\"fault_code\":%"
      "s}",
      (long long)logger_session_observed_utc_ns_or_zero(clock),
      (unsigned long long)now_ms * 1000ull, (unsigned long)boot_counter,
      session->session_id, active_span_id_json, (unsigned)battery->voltage_mv,
      battery->estimate_pct, battery->vbus_present ? "true" : "false",
      (unsigned long long)storage->free_bytes,
      (unsigned long)LOGGER_SD_MIN_FREE_RESERVE_BYTES,
      session->quarantined ? "true" : "false", fault_code_json);
  if (!(n > 0 && (size_t)n < sizeof(payload)) ||
      !logger_journal_append_json_record(
          session->journal_path, LOGGER_JOURNAL_RECORD_STATUS_SNAPSHOT,
          session->next_record_seq++, payload, &session->journal_size_bytes)) {
    return false;
  }
  return logger_session_write_live_internal(session, clock, boot_counter,
                                            now_ms);
}

bool logger_session_write_marker(logger_session_state_t *session,
                                 const logger_clock_status_t *clock,
                                 uint32_t boot_counter, uint32_t now_ms) {
  if (!session->active || !session->span_active) {
    return false;
  }

  /* Barrier: seal any in-flight chunk data before marker record */
  if (!logger_session_seal_active_chunk(session)) {
    return false;
  }

  char payload[LOGGER_SESSION_JSON_MAX];
  const int n = snprintf(
      payload, sizeof(payload),
      "{\"schema_version\":1,\"record_type\":\"marker\",\"utc_ns\":%lld,\"mono_"
      "us\":%llu,\"boot_counter\":%lu,\"session_id\":\"%s\",\"span_id\":\"%s\","
      "\"marker_kind\":\"generic\"}",
      (long long)logger_session_observed_utc_ns_or_zero(clock),
      (unsigned long long)now_ms * 1000ull, (unsigned long)boot_counter,
      session->session_id, session->current_span_id);
  return n > 0 && (size_t)n < sizeof(payload) &&
         logger_journal_append_json_record(
             session->journal_path, LOGGER_JOURNAL_RECORD_MARKER,
             session->next_record_seq++, payload, &session->journal_size_bytes);
}

bool logger_session_ensure_active_span(
    logger_session_state_t *session, logger_system_log_t *system_log,
    const char *hardware_id, const logger_persisted_state_t *persisted,
    const logger_clock_status_t *clock, const logger_storage_status_t *storage,
    const char *start_reason, const char *h10_address, bool encrypted,
    bool bonded, bool clock_jump_at_session_start, uint32_t boot_counter,
    uint32_t now_ms, const char **error_code_out,
    const char **error_message_out) {
  (void)hardware_id;

  if (session->active && session->span_active) {
    return true;
  }
  if (!session->active) {
    return logger_session_start_new_active_internal(
        session, system_log, persisted, clock, storage, start_reason,
        h10_address, encrypted, bonded, clock_jump_at_session_start, false,
        boot_counter, now_ms, error_code_out, error_message_out);
  }
  if (!logger_storage_ready_for_logging(storage)) {
    logger_session_set_error(error_code_out, error_message_out,
                             "storage_unavailable",
                             "storage is not ready for session artifacts");
    return false;
  }

  logger_session_recompute_quarantine(session, clock);
  if (!logger_session_begin_span(session, clock, start_reason, h10_address,
                                 encrypted, bonded, boot_counter, now_ms) ||
      !logger_session_write_live_internal(session, clock, boot_counter,
                                          now_ms)) {
    logger_session_set_error(error_code_out, error_message_out,
                             "storage_unavailable",
                             "failed to write reconnect span records");
    return false;
  }

  return true;
}

bool logger_session_append_pmd_packet(logger_session_state_t *session,
                                      const logger_clock_status_t *clock,
                                      uint16_t stream_kind, uint64_t mono_us,
                                      const uint8_t *value, size_t value_len) {
  const uint32_t t0 = (uint32_t)to_us_since_boot(get_absolute_time());

  if (!session->active || !session->span_active || value == NULL ||
      value_len == 0u || value_len > LOGGER_H10_PACKET_MAX_BYTES ||
      session->current_span_index >= session->span_count ||
      (stream_kind != LOGGER_SESSION_STREAM_KIND_ECG &&
       stream_kind != LOGGER_SESSION_STREAM_KIND_ACC)) {
    return false;
  }

  logger_chunk_builder_t *cb = &session->chunk_builder;
  const uint32_t seq_in_span = session->next_packet_seq_in_span;
  session->next_packet_seq_in_span += 1u;

  int64_t utc_ns = logger_session_observed_utc_ns_or_zero(clock);
  const uint32_t now_ms = (uint32_t)to_ms_since_boot(get_absolute_time());

  logger_chunk_result_t r = logger_chunk_builder_append(
      cb, stream_kind, session->current_span_id_raw, seq_in_span, mono_us,
      utc_ns, value, value_len, now_ms);

  /*
   * FULL means the current chunk must be sealed first.
   * This also covers stream/span identity changes.
   */
  if (r == LOGGER_CHUNK_FULL) {
    if (!logger_session_seal_active_chunk(session)) {
      session->next_packet_seq_in_span -= 1u;
      const uint32_t elapsed =
          (uint32_t)to_us_since_boot(get_absolute_time()) - t0;
      logger_capture_stats_record_session_append(g_session_stats, elapsed,
                                                 false);
      logger_capture_stats_record_journal_append(g_session_stats, false);
      return false;
    }
    r = logger_chunk_builder_append(cb, stream_kind,
                                    session->current_span_id_raw, seq_in_span,
                                    mono_us, utc_ns, value, value_len, now_ms);
    if (r == LOGGER_CHUNK_FULL) {
      /* Single packet too large for buffer — should not happen at 64 KiB */
      session->next_packet_seq_in_span -= 1u;
      const uint32_t elapsed =
          (uint32_t)to_us_since_boot(get_absolute_time()) - t0;
      logger_capture_stats_record_session_append(g_session_stats, elapsed,
                                                 false);
      return false;
    }
  }

  /* SEAL means target size reached — emit now */
  bool ok = true;
  if (r == LOGGER_CHUNK_SEAL) {
    ok = logger_session_seal_active_chunk(session);
  }

  const uint32_t elapsed = (uint32_t)to_us_since_boot(get_absolute_time()) - t0;
  logger_capture_stats_record_session_append(g_session_stats, elapsed, ok);
  logger_capture_stats_record_journal_append(g_session_stats, ok);
  return ok;
}

bool logger_session_append_ecg_packet(logger_session_state_t *session,
                                      const logger_clock_status_t *clock,
                                      uint64_t mono_us, const uint8_t *value,
                                      size_t value_len) {
  return logger_session_append_pmd_packet(session, clock,
                                          LOGGER_SESSION_STREAM_KIND_ECG,
                                          mono_us, value, value_len);
}

bool logger_session_seal_chunk_if_needed(logger_session_state_t *session,
                                         uint32_t now_ms) {
  if (!session->active || !session->span_active) {
    return true;
  }
  if (!logger_chunk_builder_has_data(&session->chunk_builder)) {
    return true;
  }
  if (logger_chunk_builder_age_exceeded(&session->chunk_builder, now_ms)) {
    return logger_session_seal_active_chunk(session);
  }
  return true;
}

bool logger_session_handle_disconnect(logger_session_state_t *session,
                                      const logger_clock_status_t *clock,
                                      uint32_t boot_counter, uint32_t now_ms,
                                      const char *gap_reason) {
  if (!session->active || !session->span_active) {
    return true;
  }

  char ended_span_id[LOGGER_SESSION_ID_HEX_LEN + 1];
  ended_span_id[0] = '\0';
  if (!logger_session_close_active_span(session, clock, "disconnect",
                                        boot_counter, now_ms, ended_span_id)) {
    return false;
  }
  if (!logger_session_append_gap(session, clock, ended_span_id,
                                 gap_reason != NULL ? gap_reason : "disconnect",
                                 boot_counter, now_ms)) {
    return false;
  }
  return logger_session_write_live_internal(session, clock, boot_counter,
                                            now_ms);
}

bool logger_session_handle_clock_event(logger_session_state_t *session,
                                       const logger_clock_status_t *clock,
                                       uint32_t boot_counter, uint32_t now_ms,
                                       const char *event_kind,
                                       const char *span_end_reason,
                                       int64_t delta_ns, int64_t old_utc_ns,
                                       int64_t new_utc_ns, bool split_span) {
  if (!session->active || event_kind == NULL) {
    return true;
  }

  /* Barrier: seal any in-flight chunk data before clock_event */
  if (!logger_session_seal_active_chunk(session)) {
    return false;
  }

  if (strcmp(event_kind, "clock_fixed") == 0) {
    session->quarantine_clock_fixed_mid_session = true;
  } else if (strcmp(event_kind, "clock_jump") == 0) {
    session->quarantine_clock_jump = true;
  }
  logger_session_recompute_quarantine(session, clock);

  if (split_span && session->span_active) {
    if (!logger_session_close_active_span(
            session, clock,
            span_end_reason != NULL ? span_end_reason : "clock_jump",
            boot_counter, now_ms, NULL)) {
      return false;
    }
  }

  char payload[LOGGER_SESSION_JSON_MAX];
  const int n = snprintf(
      payload, sizeof(payload),
      "{\"schema_version\":1,\"record_type\":\"clock_event\",\"utc_ns\":%lld,"
      "\"mono_us\":%llu,\"boot_counter\":%lu,\"session_id\":\"%s\",\"event_"
      "kind\":\"%s\",\"delta_ns\":%lld,\"old_utc_ns\":%lld,\"new_utc_ns\":%"
      "lld}",
      (long long)logger_session_observed_utc_ns_or_zero(clock),
      (unsigned long long)now_ms * 1000ull, (unsigned long)boot_counter,
      session->session_id, event_kind, (long long)delta_ns,
      (long long)old_utc_ns, (long long)new_utc_ns);
  if (!(n > 0 && (size_t)n < sizeof(payload)) ||
      !logger_journal_append_json_record(
          session->journal_path, LOGGER_JOURNAL_RECORD_CLOCK_EVENT,
          session->next_record_seq++, payload, &session->journal_size_bytes)) {
    return false;
  }

  return logger_session_write_live_internal(session, clock, boot_counter,
                                            now_ms);
}

bool logger_session_append_h10_battery(logger_session_state_t *session,
                                       const logger_clock_status_t *clock,
                                       uint32_t boot_counter, uint32_t now_ms,
                                       uint8_t battery_percent,
                                       const char *read_reason) {
  if (!session->active) {
    return true;
  }

  /* Barrier: seal any in-flight chunk data before h10_battery record */
  if (!logger_session_seal_active_chunk(session)) {
    return false;
  }

  char span_id_json[64];
  logger_json_string_literal(span_id_json, sizeof(span_id_json),
                             session->span_active ? session->current_span_id
                                                  : NULL);

  char payload[LOGGER_SESSION_JSON_MAX];
  const int n = snprintf(
      payload, sizeof(payload),
      "{\"schema_version\":1,\"record_type\":\"h10_battery\",\"utc_ns\":%lld,"
      "\"mono_us\":%llu,\"boot_counter\":%lu,\"session_id\":\"%s\",\"span_id\":"
      "%s,\"battery_percent\":%u,\"read_reason\":\"%s\"}",
      (long long)logger_session_observed_utc_ns_or_zero(clock),
      (unsigned long long)now_ms * 1000ull, (unsigned long)boot_counter,
      session->session_id, span_id_json, (unsigned)battery_percent,
      read_reason != NULL ? read_reason : "periodic");
  return n > 0 && (size_t)n < sizeof(payload) &&
         logger_journal_append_json_record(
             session->journal_path, LOGGER_JOURNAL_RECORD_H10_BATTERY,
             session->next_record_seq++, payload, &session->journal_size_bytes);
}

bool logger_session_finalize(
    logger_session_state_t *session, logger_system_log_t *system_log,
    const char *hardware_id, const logger_persisted_state_t *persisted,
    const logger_clock_status_t *clock, const logger_storage_status_t *storage,
    const char *end_reason, uint32_t boot_counter, uint32_t now_ms) {
  return logger_session_finalize_internal(
      session, system_log, hardware_id, persisted, clock, storage,
      end_reason != NULL ? end_reason : "manual_stop", false, boot_counter,
      now_ms);
}

bool logger_session_stop_debug(logger_session_state_t *session,
                               logger_system_log_t *system_log,
                               const char *hardware_id,
                               const logger_persisted_state_t *persisted,
                               const logger_clock_status_t *clock,
                               const logger_storage_status_t *storage,
                               uint32_t boot_counter, uint32_t now_ms) {
  return logger_session_finalize_internal(
      session, system_log, hardware_id, persisted, clock, storage,
      "manual_stop", true, boot_counter, now_ms);
}

bool logger_session_recover_on_boot(
    logger_session_state_t *session, logger_system_log_t *system_log,
    const char *hardware_id, const logger_persisted_state_t *persisted,
    const logger_clock_status_t *clock, const logger_storage_status_t *storage,
    uint32_t boot_counter, uint32_t now_ms, bool resume_allowed,
    bool *recovered_active_out, bool *closed_session_out) {
  if (recovered_active_out != NULL) {
    *recovered_active_out = false;
  }
  if (closed_session_out != NULL) {
    *closed_session_out = false;
  }
  if (!logger_storage_ready_for_logging(storage)) {
    return true;
  }

  char dir_name[64];
  char dir_path[LOGGER_STORAGE_PATH_MAX];
  char journal_path[LOGGER_STORAGE_PATH_MAX];
  char live_path[LOGGER_STORAGE_PATH_MAX];
  char manifest_path[LOGGER_STORAGE_PATH_MAX];
  if (!logger_session_find_live_paths(dir_name, dir_path, journal_path,
                                      live_path, manifest_path)) {
    return true;
  }

  char live_session_id[LOGGER_SESSION_ID_HEX_LEN + 1];
  const bool have_live_session_id =
      logger_session_load_live_session_id(live_path, live_session_id);

  logger_journal_scan_result_t scan;
  if (!logger_journal_scan(journal_path, &scan) || !scan.valid) {
    logger_session_log_recovery_issue(
        system_log, clock != NULL ? clock->now_utc : NULL,
        "session_recovery_failed", "{\"reason\":\"journal_scan_failed\"}");
    return false;
  }
  uint64_t actual_file_size = 0u;
  if (!logger_storage_file_size(journal_path, &actual_file_size)) {
    return false;
  }
  if (actual_file_size > scan.valid_size_bytes &&
      !logger_journal_truncate_to_valid(journal_path, scan.valid_size_bytes)) {
    logger_session_log_recovery_issue(
        system_log, clock != NULL ? clock->now_utc : NULL,
        "session_recovery_failed", "{\"reason\":\"journal_truncate_failed\"}");
    return false;
  }
  if (have_live_session_id && strcmp(live_session_id, scan.session_id) != 0) {
    logger_session_log_recovery_issue(
        system_log, clock != NULL ? clock->now_utc : NULL,
        "session_recovery_failed", "{\"reason\":\"live_journal_id_mismatch\"}");
    return false;
  }

  if (scan.session_closed || logger_storage_file_exists(manifest_path)) {
    (void)logger_storage_remove_file(live_path);
    (void)logger_upload_queue_refresh_file(
        system_log, clock != NULL ? clock->now_utc : NULL, NULL);
    return true;
  }

  logger_session_init(session);
  session->active = true;
  logger_copy_string(session->session_id, sizeof(session->session_id),
                     scan.session_id);
  logger_copy_string(session->study_day_local, sizeof(session->study_day_local),
                     scan.study_day_local);
  logger_copy_string(session->session_start_utc,
                     sizeof(session->session_start_utc),
                     scan.session_start_utc);
  logger_copy_string(session->session_end_utc, sizeof(session->session_end_utc),
                     scan.session_end_utc);
  logger_copy_string(session->session_start_reason,
                     sizeof(session->session_start_reason),
                     scan.session_start_reason);
  logger_copy_string(session->session_end_reason,
                     sizeof(session->session_end_reason),
                     scan.session_end_reason);
  logger_copy_string(session->session_dir_name,
                     sizeof(session->session_dir_name), dir_name);
  logger_copy_string(session->session_dir_path,
                     sizeof(session->session_dir_path), dir_path);
  logger_copy_string(session->journal_path, sizeof(session->journal_path),
                     journal_path);
  logger_copy_string(session->live_path, sizeof(session->live_path), live_path);
  logger_copy_string(session->manifest_path, sizeof(session->manifest_path),
                     manifest_path);
  session->next_record_seq = scan.next_record_seq;
  session->journal_size_bytes = scan.valid_size_bytes;
  session->next_chunk_seq_in_session = scan.next_chunk_seq_in_session;
  session->next_packet_seq_in_span = scan.next_packet_seq_in_span;
  session->span_count = scan.span_count;
  memcpy(session->spans, scan.spans, sizeof(session->spans));
  session->span_active = scan.active_span_open;
  session->current_span_index =
      scan.active_span_open ? scan.active_span_index : 0xffffffffu;
  logger_copy_string(session->current_span_id, sizeof(session->current_span_id),
                     scan.active_span_open ? scan.active_span_id : NULL);
  if (scan.active_span_open &&
      !logger_hex_to_bytes_16(session->current_span_id,
                              session->current_span_id_raw)) {
    logger_session_log_recovery_issue(
        system_log, clock != NULL ? clock->now_utc : NULL,
        "session_recovery_failed", "{\"reason\":\"span_id_parse_failed\"}");
    return false;
  }
  /* Initialize chunk builder for the recovered session */
  logger_chunk_builder_reset(&session->chunk_builder);
  session->quarantine_clock_invalid_at_start =
      scan.quarantine_clock_invalid_at_start;
  session->quarantine_clock_fixed_mid_session =
      scan.quarantine_clock_fixed_mid_session;
  session->quarantine_clock_jump = scan.quarantine_clock_jump;
  session->quarantine_recovery_after_reset =
      scan.quarantine_recovery_after_reset;
  logger_session_recompute_quarantine(session, clock);

  if (!resume_allowed) {
    if (!logger_session_finalize_internal(
            session, system_log, hardware_id, persisted, clock, storage,
            "unexpected_reboot", false, boot_counter, now_ms)) {
      return false;
    }
    if (closed_session_out != NULL) {
      *closed_session_out = true;
    }
    return true;
  }

  char ended_span_id[LOGGER_SESSION_ID_HEX_LEN + 1];
  ended_span_id[0] = '\0';
  if (session->span_active) {
    if (!logger_session_close_active_span(session, clock, "unexpected_reboot",
                                          boot_counter, now_ms,
                                          ended_span_id)) {
      return false;
    }
    if (!logger_session_append_gap(session, clock, ended_span_id,
                                   "recovery_reboot", boot_counter, now_ms)) {
      return false;
    }
  }
  session->quarantine_recovery_after_reset = true;
  logger_session_recompute_quarantine(session, clock);
  if (!logger_session_append_recovery(session, clock, "unexpected_reboot",
                                      boot_counter, now_ms)) {
    return false;
  }
  if (!logger_session_begin_span(session, clock, "recovery_resume",
                                 persisted->config.bound_h10_address, false,
                                 false, boot_counter, now_ms)) {
    return false;
  }
  if (!logger_session_write_live_internal(session, clock, boot_counter,
                                          now_ms)) {
    return false;
  }

  (void)logger_system_log_append(
      system_log,
      clock != NULL && clock->now_utc[0] != '\0' ? clock->now_utc : NULL,
      "session_recovered", LOGGER_SYSTEM_LOG_SEVERITY_INFO,
      "{\"resume_allowed\":true}");
  if (recovered_active_out != NULL) {
    *recovered_active_out = true;
  }
  return true;
}
