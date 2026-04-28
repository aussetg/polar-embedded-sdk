#include "logger/session.h"
#include "logger/writer_protocol.h"

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "ff.h"
#include "hardware/sync.h"
#include "pico/rand.h"
#include "pico/stdlib.h"

#include "board_config.h"
#include "logger/capture_stats.h"
#include "logger/h10.h"
#include "logger/journal_writer.h"
#include "logger/json.h"
#include "logger/json_writer.h"
#include "logger/psram_layout.h"
#include "logger/queue.h"
#include "logger/sha256.h"
#include "logger/util.h"
#include "logger/version.h"

#define LOGGER_SESSION_JSON_MAX 1024
#define LOGGER_SESSION_LIVE_JSON_TOKEN_MAX 64u

/* Chunk assembly buffer: 128 KiB from PSRAM, no heap in the hot path */
#define LOGGER_SESSION_CHUNK_BUF_SIZE (128u * 1024u)
static uint8_t *g_chunk_buf = PSRAM_CHUNK_BUF;

static logger_capture_stats_t *g_session_stats = NULL;

typedef struct {
  char dir_name[64];
  char dir_path[LOGGER_STORAGE_PATH_MAX];
  char journal_path[LOGGER_STORAGE_PATH_MAX];
  char live_path[LOGGER_STORAGE_PATH_MAX];
  char manifest_path[LOGGER_STORAGE_PATH_MAX];
  char live_session_id[LOGGER_SESSION_ID_HEX_LEN + 1];
  logger_journal_scan_result_t scan;
  bool live_present;
  bool in_use;
} logger_session_recovery_workspace_t;

typedef struct {
  char buf[LOGGER_SESSION_JSON_MAX];
  jsmntok_t tokens[LOGGER_SESSION_LIVE_JSON_TOKEN_MAX];
  bool in_use;
} logger_session_live_json_workspace_t;

typedef struct {
  char payload[640];
  char current_span_id_json[64];
  char current_span_index_json[24];
  char last_flush_utc_json[64];
  bool in_use;
} logger_session_live_write_workspace_t;

typedef struct {
  FIL file;
  logger_sha256_t sha;
  uint8_t chunk[256];
  bool in_use;
} logger_session_sha256_workspace_t;

static logger_session_recovery_workspace_t g_session_recovery_workspace;
static logger_session_live_json_workspace_t g_session_live_json_workspace;
static logger_session_live_write_workspace_t g_session_live_write_workspace;
static logger_session_sha256_workspace_t g_session_sha256_workspace;
static logger_persisted_state_t g_session_manifest_persisted;
static bool g_session_manifest_persisted_in_use;

static logger_session_recovery_workspace_t *
logger_session_recovery_workspace_acquire(void) {
  assert(!g_session_recovery_workspace.in_use);
  memset(&g_session_recovery_workspace, 0,
         sizeof(g_session_recovery_workspace));
  g_session_recovery_workspace.in_use = true;
  return &g_session_recovery_workspace;
}

static void logger_session_recovery_workspace_release(
    logger_session_recovery_workspace_t *workspace) {
  (void)workspace;
  assert(workspace == &g_session_recovery_workspace);
  assert(g_session_recovery_workspace.in_use);
  g_session_recovery_workspace.in_use = false;
}

static logger_session_live_json_workspace_t *
logger_session_live_json_workspace_acquire(void) {
  assert(!g_session_live_json_workspace.in_use);
  memset(&g_session_live_json_workspace, 0,
         sizeof(g_session_live_json_workspace));
  g_session_live_json_workspace.in_use = true;
  return &g_session_live_json_workspace;
}

static void logger_session_live_json_workspace_release(
    logger_session_live_json_workspace_t *workspace) {
  (void)workspace;
  assert(workspace == &g_session_live_json_workspace);
  assert(g_session_live_json_workspace.in_use);
  g_session_live_json_workspace.in_use = false;
}

static logger_session_live_write_workspace_t *
logger_session_live_write_workspace_acquire(void) {
  assert(!g_session_live_write_workspace.in_use);
  memset(&g_session_live_write_workspace, 0,
         sizeof(g_session_live_write_workspace));
  g_session_live_write_workspace.in_use = true;
  return &g_session_live_write_workspace;
}

static void logger_session_live_write_workspace_release(
    logger_session_live_write_workspace_t *workspace) {
  (void)workspace;
  assert(workspace == &g_session_live_write_workspace);
  assert(g_session_live_write_workspace.in_use);
  g_session_live_write_workspace.in_use = false;
}

static logger_session_sha256_workspace_t *
logger_session_sha256_workspace_acquire(void) {
  assert(!g_session_sha256_workspace.in_use);
  memset(&g_session_sha256_workspace, 0, sizeof(g_session_sha256_workspace));
  g_session_sha256_workspace.in_use = true;
  return &g_session_sha256_workspace;
}

static void logger_session_sha256_workspace_release(
    logger_session_sha256_workspace_t *workspace) {
  (void)workspace;
  assert(workspace == &g_session_sha256_workspace);
  assert(g_session_sha256_workspace.in_use);
  g_session_sha256_workspace.in_use = false;
}

static logger_persisted_state_t *
logger_session_manifest_persisted_acquire(void) {
  assert(!g_session_manifest_persisted_in_use);
  g_session_manifest_persisted_in_use = true;
  memset(&g_session_manifest_persisted, 0,
         sizeof(g_session_manifest_persisted));
  return &g_session_manifest_persisted;
}

static void
logger_session_manifest_persisted_release(logger_persisted_state_t *persisted) {
  (void)persisted;
  assert(persisted == &g_session_manifest_persisted);
  assert(g_session_manifest_persisted_in_use);
  g_session_manifest_persisted_in_use = false;
}

void logger_session_set_capture_stats(logger_capture_stats_t *stats) {
  g_session_stats = stats;
}

void logger_session_init_buffers(void) {
  /* Zero-initialise the PSRAM chunk buffer so session init sees clean memory.
   */
  memset(g_chunk_buf, 0, LOGGER_SESSION_CHUNK_BUF_SIZE);
}

uint64_t logger_session_writer_journal_size_approx(
    const logger_session_state_t *session) {
  if (session == NULL) {
    return 0u;
  }

  /* Core 1 owns this 64-bit field.  On RP-class 32-bit cores the load is not
   * guaranteed to be single-copy atomic, so read twice with acquire fences and
   * prefer a stable sample.  This is intentionally telemetry-only while a
   * session is active; callers needing an exact value must first complete a
   * writer barrier/finalize/recovery boundary. */
  uint64_t last = 0u;
  for (uint32_t attempt = 0u; attempt < 4u; ++attempt) {
    __mem_fence_acquire();
    const uint64_t first = session->writer.journal_size_bytes;
    __mem_fence_acquire();
    const uint64_t second = session->writer.journal_size_bytes;
    if (first == second) {
      return second;
    }
    last = second;
  }
  return last;
}

static void logger_session_writer_restore_from_scan(
    logger_session_writer_state_t *writer,
    const logger_journal_scan_result_t *scan) {
  hard_assert(writer != NULL);
  hard_assert(scan != NULL);

  writer->next_record_seq = scan->next_record_seq;
  writer->journal_size_bytes = scan->valid_size_bytes;
  writer->next_chunk_seq_in_session = scan->next_chunk_seq_in_session;
  memcpy(writer->spans, scan->spans, sizeof(writer->spans));
  logger_chunk_builder_reset(&writer->chunk_builder);
}

/*
 * Central dispatch: routes writer commands through the capture pipe
 * when one is attached, or falls back to direct dispatch otherwise.
 *
 * This is the ONLY place that should call logger_writer_dispatch()
 * (aside from the pipe's own inline consumer for PMD batch drains).
 * Every session function calls session_dispatch() instead of
 * logger_writer_dispatch() directly.
 */
static bool session_dispatch(logger_session_state_t *session,
                             const logger_writer_cmd_t *cmd) {
  if (session == NULL || cmd == NULL) {
    return false;
  }
  if (session->pipe != NULL) {
    if (cmd->type == LOGGER_WRITER_APPEND_PMD_PACKET) {
      return capture_pipe_submit_pmd(session->pipe, cmd);
    }
    return capture_pipe_submit_cmd(session->pipe,
                                   (logger_session_context_t *)session, cmd);
  }
  return logger_writer_dispatch((logger_session_context_t *)session, cmd);
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
 * Writes the sealed chunk through the open writer handle and
 * syncs immediately (chunk boundary = durability boundary).
 *
 * Updates span counters only on success, so span stats always
 * reflect durable journal bytes.
 *
 * No-op when the builder is empty.
 */
static bool logger_session_seal_active_chunk(logger_session_state_t *session) {
  logger_chunk_builder_t *cb = &session->writer.chunk_builder;
  if (!logger_chunk_builder_has_data(cb)) {
    return true;
  }

  const uint32_t pkt_count = cb->packet_count;
  const uint32_t first_seq = cb->first_seq_in_span;
  const uint32_t last_seq = cb->last_seq_in_span;

  const uint8_t *payload = NULL;
  size_t payload_len = 0u;
  if (!logger_chunk_builder_seal(cb, session->writer.next_chunk_seq_in_session,
                                 &payload, &payload_len)) {
    return false;
  }
  if (!logger_journal_writer_append_binary(
          &session->writer.journal_writer, LOGGER_JOURNAL_RECORD_DATA_CHUNK,
          session->writer.next_record_seq++, payload, payload_len)) {
    /* Journal write failed — lose in-flight data, keep journal consistent */
    logger_chunk_builder_clear(cb);
    return false;
  }
  /* Chunk boundary: sync to make this durable */
  if (!logger_journal_writer_sync(&session->writer.journal_writer)) {
    logger_chunk_builder_clear(cb);
    return false;
  }

  /* Update durable size from the writer */
  session->writer.journal_size_bytes =
      logger_journal_writer_durable_size(&session->writer.journal_writer);

  /* Update span counters from the chunk we just emitted */
  if (session->current_span_index < session->span_count) {
    logger_journal_span_summary_t *span =
        &session->writer.spans[session->current_span_index];
    if (span->packet_count == 0u) {
      span->first_seq_in_span = first_seq;
    }
    span->packet_count += pkt_count;
    span->last_seq_in_span = last_seq;
  }

  session->writer.next_chunk_seq_in_session++;
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
                                   const char *now_utc, uint32_t boot_counter,
                                   uint32_t now_ms) {
  logger_session_live_write_workspace_t *workspace =
      logger_session_live_write_workspace_acquire();

  logger_json_string_literal(
      workspace->current_span_id_json, sizeof(workspace->current_span_id_json),
      session->span_active ? session->current_span_id : NULL);
  if (session->span_active) {
    snprintf(workspace->current_span_index_json,
             sizeof(workspace->current_span_index_json), "%lu",
             (unsigned long)session->current_span_index);
  } else {
    logger_copy_string(workspace->current_span_index_json,
                       sizeof(workspace->current_span_index_json), "null");
  }
  logger_json_string_literal(workspace->last_flush_utc_json,
                             sizeof(workspace->last_flush_utc_json), now_utc);

  const int n = snprintf(
      workspace->payload, sizeof(workspace->payload),
      "{\"schema_version\":1,\"session_id\":\"%s\",\"study_day_local\":\"%s\","
      "\"session_dir\":\"%s\",\"state\":\"active\",\"journal_size_bytes\":%llu,"
      "\"last_flush_utc\":%s,\"last_flush_mono_us\":%llu,\"current_span_id\":%"
      "s,\"current_span_index\":%s,\"quarantined\":%s,\"clock_state\":\"%s\","
      "\"boot_counter\":%lu}",
      session->session_id, session->study_day_local, session->session_dir_name,
      (unsigned long long)session->writer.journal_size_bytes,
      workspace->last_flush_utc_json, (unsigned long long)now_ms * 1000ull,
      workspace->current_span_id_json, workspace->current_span_index_json,
      session->quarantined ? "true" : "false", session->clock_state,
      (unsigned long)boot_counter);
  const bool ok = n > 0 && (size_t)n < sizeof(workspace->payload) &&
                  logger_storage_write_file_atomic(
                      session->live_path, workspace->payload, (size_t)n);
  logger_session_live_write_workspace_release(workspace);
  return ok;
}

static bool
logger_session_append_session_start(logger_session_state_t *session,
                                    const logger_persisted_state_t *persisted,
                                    const logger_clock_status_t *clock,
                                    uint32_t boot_counter, uint32_t now_ms) {
  logger_writer_cmd_t cmd;
  memset(&cmd, 0, sizeof(cmd));
  /* cmd.type sets the discriminant via the union's first member.
   * Every member struct's .type field overlays the same address,
   * so setting cmd.type once is sufficient. */
  cmd.type = LOGGER_WRITER_SESSION_START;
  logger_writer_session_start_t *s = &cmd.session_start;
  s->boot_counter = boot_counter;
  s->now_ms = now_ms;
  s->utc_ns = logger_session_observed_utc_ns_or_zero(clock);
  logger_copy_string(s->session_id, sizeof(s->session_id), session->session_id);
  logger_copy_string(s->study_day_local, sizeof(s->study_day_local),
                     session->study_day_local);
  logger_copy_string(s->session_start_utc, sizeof(s->session_start_utc),
                     session->session_start_utc);
  logger_copy_string(s->session_start_reason, sizeof(s->session_start_reason),
                     session->session_start_reason);
  logger_copy_string(s->clock_state, sizeof(s->clock_state),
                     session->clock_state);
  logger_copy_string(s->logger_id, sizeof(s->logger_id),
                     persisted->config.logger_id);
  logger_copy_string(s->subject_id, sizeof(s->subject_id),
                     persisted->config.subject_id);
  logger_copy_string(s->timezone, sizeof(s->timezone),
                     persisted->config.timezone);
  return session_dispatch(session, &cmd);
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
    session->span_active = false;
    session->current_span_id[0] = '\0';
    session->current_span_index = 0xffffffffu;
    memset(span, 0, sizeof(*span));
    return false;
  }

  logger_copy_string(span->start_reason, sizeof(span->start_reason),
                     start_reason);
  logger_copy_string(span->start_utc, sizeof(span->start_utc),
                     clock != NULL ? clock->now_utc : NULL);
  session->span_count += 1u;
  session->next_packet_seq_in_span = 0u;

  /* Writer: SPAN_START */
  logger_writer_cmd_t cmd;
  memset(&cmd, 0, sizeof(cmd));
  cmd.type = LOGGER_WRITER_SPAN_START;
  logger_writer_span_start_t *ss = &cmd.span_start;
  ss->boot_counter = boot_counter;
  ss->now_ms = now_ms;
  ss->utc_ns = logger_session_observed_utc_ns_or_zero(clock);
  logger_copy_string(ss->session_id, sizeof(ss->session_id),
                     session->session_id);
  logger_copy_string(ss->span_id, sizeof(ss->span_id), span->span_id);
  ss->span_index_in_session = session->current_span_index;
  logger_copy_string(ss->start_utc, sizeof(ss->start_utc), span->start_utc);
  logger_copy_string(ss->start_reason, sizeof(ss->start_reason), start_reason);
  logger_copy_string(ss->h10_address, sizeof(ss->h10_address), h10_address);
  ss->encrypted = encrypted;
  ss->bonded = bonded;

  if (!session_dispatch(session, &cmd)) {
    /* Rollback control-plane state on writer failure */
    session->span_count -= 1u;
    session->span_active = false;
    session->current_span_id[0] = '\0';
    session->current_span_index = 0xffffffffu;
    memset(span, 0, sizeof(*span));
    return false;
  }
  return true;
}

/*
 * Control-plane span close: dispatch SPAN_END through the pipe.
 *
 * Core 0 captures span metadata and constructs a SPAN_END command.
 * Core 1 handles the barrier (seal + write + sync).
 * Control-plane state is updated only on success so the two
 * never diverge.
 */
static bool logger_session_control_close_span(
    logger_session_state_t *session, const logger_clock_status_t *clock,
    const char *end_reason, uint32_t boot_counter, uint32_t now_ms,
    char ended_span_id_out[LOGGER_SESSION_ID_HEX_LEN + 1]) {
  if (!session->span_active ||
      session->current_span_index >= session->span_count) {
    return true;
  }

  logger_journal_span_summary_t *span =
      &session->spans[session->current_span_index];
  if (ended_span_id_out != NULL) {
    logger_copy_string(ended_span_id_out, LOGGER_SESSION_ID_HEX_LEN + 1,
                       span->span_id);
  }
  /* Dispatch SPAN_END to core 1.  The writer handles the barrier:
   * seal active chunk, read actual packet counts from session state
   * after sealing, write span_end record, sync. */
  logger_writer_cmd_t cmd;
  memset(&cmd, 0, sizeof(cmd));
  cmd.type = LOGGER_WRITER_SPAN_END;
  logger_writer_span_end_t *se = &cmd.span_end;
  se->boot_counter = boot_counter;
  se->now_ms = now_ms;
  se->utc_ns = logger_session_observed_utc_ns_or_zero(clock);
  logger_copy_string(se->session_id, sizeof(se->session_id),
                     session->session_id);
  logger_copy_string(se->span_id, sizeof(se->span_id), span->span_id);
  se->span_index_in_session = session->current_span_index;
  logger_copy_string(se->end_utc, sizeof(se->end_utc),
                     clock != NULL ? clock->now_utc : NULL);
  logger_copy_string(se->end_reason, sizeof(se->end_reason), end_reason);
  /* packet_count / seq fields are filled by core 1 after sealing */

  if (!session_dispatch(session, &cmd)) {
    return false;
  }

  /* Control-plane state updated only on dispatch success. */
  logger_copy_string(span->end_reason, sizeof(span->end_reason), end_reason);
  logger_copy_string(span->end_utc, sizeof(span->end_utc),
                     clock != NULL ? clock->now_utc : NULL);
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
  /* Dispatch WRITE_GAP to core 1.  The writer handles the barrier
   * (seal + write + sync) internally. */
  logger_writer_cmd_t cmd;
  memset(&cmd, 0, sizeof(cmd));
  cmd.type = LOGGER_WRITER_WRITE_GAP;
  logger_writer_write_gap_t *g = &cmd.write_gap;
  g->boot_counter = boot_counter;
  g->now_ms = now_ms;
  g->utc_ns = logger_session_observed_utc_ns_or_zero(clock);
  logger_copy_string(g->session_id, sizeof(g->session_id), session->session_id);
  logger_copy_string(g->ended_span_id, sizeof(g->ended_span_id), ended_span_id);
  logger_copy_string(g->gap_reason, sizeof(g->gap_reason),
                     gap_reason != NULL ? gap_reason : "disconnect");
  return session_dispatch(session, &cmd);
}

static bool logger_session_append_recovery(logger_session_state_t *session,
                                           const logger_clock_status_t *clock,
                                           const char *reason,
                                           uint32_t boot_counter,
                                           uint32_t now_ms) {
  logger_writer_cmd_t cmd;
  memset(&cmd, 0, sizeof(cmd));
  cmd.type = LOGGER_WRITER_WRITE_RECOVERY;
  logger_writer_write_recovery_t *r = &cmd.write_recovery;
  r->boot_counter = boot_counter;
  r->now_ms = now_ms;
  r->utc_ns = logger_session_observed_utc_ns_or_zero(clock);
  logger_copy_string(r->session_id, sizeof(r->session_id), session->session_id);
  logger_copy_string(r->recovery_reason, sizeof(r->recovery_reason), reason);
  return session_dispatch(session, &cmd);
}

static bool logger_session_append_session_end(
    logger_session_state_t *session, const logger_clock_status_t *clock,
    const char *end_reason, uint32_t boot_counter, uint32_t now_ms) {
  /* No direct seal — writer_dispatch for SESSION_END handles the barrier. */

  /* Control-plane: capture session end metadata */
  logger_copy_string(session->session_end_utc, sizeof(session->session_end_utc),
                     clock != NULL ? clock->now_utc : NULL);
  logger_copy_string(session->session_end_reason,
                     sizeof(session->session_end_reason), end_reason);

  /* Pre-serialize quarantine reasons for the command */
  char reasons_json[128];
  logger_session_quarantine_reasons_json(session, reasons_json);

  /* Writer: SESSION_END */
  logger_writer_cmd_t cmd;
  memset(&cmd, 0, sizeof(cmd));
  cmd.type = LOGGER_WRITER_SESSION_END;
  logger_writer_session_end_t *se = &cmd.session_end;
  se->boot_counter = boot_counter;
  se->now_ms = now_ms;
  se->utc_ns = logger_session_observed_utc_ns_or_zero(clock);
  logger_copy_string(se->session_id, sizeof(se->session_id),
                     session->session_id);
  logger_copy_string(se->end_reason, sizeof(se->end_reason), end_reason);
  se->span_count = session->span_count;
  se->quarantined = session->quarantined;
  logger_copy_string(se->quarantine_reasons, sizeof(se->quarantine_reasons),
                     reasons_json);

  if (!session_dispatch(session, &cmd)) {
    return false;
  }
  /* journal_size_bytes updated by dispatch (writer-side owns durable size). */
  return true;
}

static bool __attribute__((noinline))
logger_session_compute_file_sha256(const char *path,
                                   char out_hex[LOGGER_SHA256_HEX_LEN + 1],
                                   uint64_t *size_bytes_out) {
  logger_session_sha256_workspace_t *workspace =
      logger_session_sha256_workspace_acquire();
  if (f_open(&workspace->file, path, FA_READ) != FR_OK) {
    logger_session_sha256_workspace_release(workspace);
    return false;
  }
  logger_sha256_init(&workspace->sha);
  uint64_t total = 0u;
  UINT read_bytes = 0u;
  do {
    if (f_read(&workspace->file, workspace->chunk, sizeof(workspace->chunk),
               &read_bytes) != FR_OK) {
      f_close(&workspace->file);
      logger_session_sha256_workspace_release(workspace);
      return false;
    }
    if (read_bytes > 0u) {
      logger_sha256_update(&workspace->sha, workspace->chunk, read_bytes);
      total += read_bytes;
    }
  } while (read_bytes == sizeof(workspace->chunk));
  if (f_close(&workspace->file) != FR_OK) {
    logger_session_sha256_workspace_release(workspace);
    return false;
  }
  logger_sha256_final_hex(&workspace->sha, out_hex);
  if (size_bytes_out != NULL) {
    *size_bytes_out = total;
  }
  logger_session_sha256_workspace_release(workspace);
  return true;
}

static bool __attribute__((noinline)) logger_session_build_manifest(
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
    const logger_journal_span_summary_t *span = &session->writer.spans[i];
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
    const logger_persisted_state_t *persisted,
    const logger_clock_status_t *clock, const char *end_reason,
    bool debug_session, uint32_t boot_counter, uint32_t now_ms) {
  if (!session->active) {
    return false;
  }

  logger_session_recompute_quarantine(session, clock);

  /*
   * Close span through the pipe — SPAN_END barrier.
   * After this, span_active is false and span counters are finalized.
   */
  if (!logger_session_control_close_span(session, clock, end_reason,
                                         boot_counter, now_ms, NULL)) {
    return false;
  }
  /* Session end through the pipe — SESSION_END barrier. */
  if (!logger_session_append_session_end(session, clock, end_reason,
                                         boot_counter, now_ms)) {
    return false;
  }

  /*
   * Dispatch FINALIZE_SESSION through the pipe.
   *
   * Core 1 will: close journal → compute SHA-256 → write manifest →
   * remove live.json → refresh upload queue.
   *
   * Refresh config fields in manifest_ctx from the live persisted state
   * so the manifest reflects the current config, not a stale snapshot
   * from session creation.  If config changed mid-session via CLI
   * (subject_id, timezone, etc.), the manifest gets the latest values.
   *
   * hardware_id is immutable (silicon identity) — no refresh needed.
   * storage is refreshed by core 1 (SD owner) during dispatch.
   * debug_session is a creation-time property — no refresh needed.
   *
   * The ring fence in the command transport guarantees core 1 sees
   * these writes before it reads manifest_ctx during dispatch.
   */
  {
    logger_session_manifest_ctx_t *mc = &session->manifest_ctx;
    logger_session_manifest_ctx_refresh_from_config(mc, persisted);
    mc->system_log = system_log;
  }

  logger_writer_cmd_t cmd;
  memset(&cmd, 0, sizeof(cmd));
  cmd.type = LOGGER_WRITER_FINALIZE_SESSION;
  logger_writer_finalize_session_t *fs = &cmd.finalize_session;
  fs->boot_counter = boot_counter;
  fs->now_ms = now_ms;
  if (clock != NULL && clock->now_utc[0] != '\0') {
    logger_copy_string(fs->now_utc, sizeof(fs->now_utc), clock->now_utc);
  }

  if (!session_dispatch(session, &cmd)) {
    return false;
  }

  /* System log on core 0 (internal flash, not SD) */
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
    const char *hardware_id, const logger_persisted_state_t *persisted,
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
  session->writer.next_chunk_seq_in_session = 0u;
  session->next_packet_seq_in_span = 0u;
  /*
   * Set paths in session state so the writer (core 1) can read them
   * when it handles SESSION_START.  No FatFS I/O here — directory
   * creation and journal file creation happen on core 1.
   */
  if (!logger_session_set_paths(session)) {
    logger_session_set_error(error_code_out, error_message_out,
                             "storage_unavailable",
                             "session path exceeds buffer");
    logger_session_init(session);
    return false;
  }

  /*
   * Snapshot manifest context: config/storage fields that core 1
   * will need at finalize time.  Written here by core 0, read by
   * core 1 during FINALIZE_SESSION dispatch.  The ring fences
   * in the command transport provide the ordering guarantee.
   */
  {
    logger_session_manifest_ctx_t *mc = &session->manifest_ctx;
    memset(mc, 0, sizeof(*mc));
    logger_copy_string(mc->hardware_id, sizeof(mc->hardware_id), hardware_id);
    logger_copy_string(mc->logger_id, sizeof(mc->logger_id),
                       persisted->config.logger_id);
    logger_copy_string(mc->subject_id, sizeof(mc->subject_id),
                       persisted->config.subject_id);
    logger_copy_string(mc->timezone, sizeof(mc->timezone),
                       persisted->config.timezone);
    logger_copy_string(mc->bound_h10_address, sizeof(mc->bound_h10_address),
                       persisted->config.bound_h10_address);
    mc->storage = *storage;
    mc->debug_session = debug_session;
    mc->system_log = system_log;
  }

  /*
   * SESSION_START barrier creates the directory and journal file on
   * core 1 before writing the session_start record.  Paths and IDs
   * are already in session state; the acquire fence in dequeue
   * guarantees core 1 sees them.
   */
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

  /* Write live.json through the pipe (core 1 owns FatFS). */
  if (!logger_session_refresh_live(session, clock, boot_counter, now_ms)) {
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

static bool logger_session_path_exists(const char *path, bool *exists_out,
                                       bool *io_error_out) {
  if (exists_out != NULL) {
    *exists_out = false;
  }
  FILINFO info;
  memset(&info, 0, sizeof(info));
  const FRESULT fr = f_stat(path, &info);
  if (fr == FR_OK) {
    if (exists_out != NULL) {
      *exists_out = true;
    }
    return true;
  }
  if (fr == FR_NO_FILE || fr == FR_NO_PATH) {
    return true;
  }
  if (io_error_out != NULL) {
    *io_error_out = true;
  }
  return false;
}

static bool logger_session_store_recovery_paths(
    char dir_name_out[64], char dir_path_out[LOGGER_STORAGE_PATH_MAX],
    char journal_path_out[LOGGER_STORAGE_PATH_MAX],
    char live_path_out[LOGGER_STORAGE_PATH_MAX],
    char manifest_path_out[LOGGER_STORAGE_PATH_MAX], const char *dir_name) {
  if (strlen(dir_name) >= 64u) {
    return false;
  }
  logger_copy_string(dir_name_out, 64u, dir_name);
  return logger_path_join2(dir_path_out, LOGGER_STORAGE_PATH_MAX,
                           "0:/logger/sessions/", dir_name) &&
         logger_path_join2(journal_path_out, LOGGER_STORAGE_PATH_MAX,
                           dir_path_out, "/journal.bin") &&
         logger_path_join2(live_path_out, LOGGER_STORAGE_PATH_MAX, dir_path_out,
                           "/live.json") &&
         logger_path_join2(manifest_path_out, LOGGER_STORAGE_PATH_MAX,
                           dir_path_out, "/manifest.json");
}

static bool logger_session_find_recovery_paths(
    char dir_name_out[64], char dir_path_out[LOGGER_STORAGE_PATH_MAX],
    char journal_path_out[LOGGER_STORAGE_PATH_MAX],
    char live_path_out[LOGGER_STORAGE_PATH_MAX],
    char manifest_path_out[LOGGER_STORAGE_PATH_MAX], bool *live_present_out,
    bool *io_error_out) {
  if (io_error_out != NULL) {
    *io_error_out = false;
  }
  if (live_present_out != NULL) {
    *live_present_out = false;
  }
  DIR dir;
  if (f_opendir(&dir, "0:/logger/sessions") != FR_OK) {
    if (io_error_out != NULL) {
      *io_error_out = true;
    }
    return false;
  }

  bool found = false;
  bool io_error = false;
  bool found_journal_only = false;
  for (;;) {
    FILINFO info;
    memset(&info, 0, sizeof(info));
    const FRESULT fr = f_readdir(&dir, &info);
    if (fr != FR_OK) {
      io_error = true;
      break;
    }
    if (info.fname[0] == '\0') {
      break;
    }
    if ((info.fattrib & AM_DIR) == 0u || strcmp(info.fname, ".") == 0 ||
        strcmp(info.fname, "..") == 0) {
      continue;
    }

    char dir_path[LOGGER_STORAGE_PATH_MAX];
    char journal_path[LOGGER_STORAGE_PATH_MAX];
    char live_path[LOGGER_STORAGE_PATH_MAX];
    char manifest_path[LOGGER_STORAGE_PATH_MAX];
    char candidate_dir_name[64];
    if (!logger_session_store_recovery_paths(candidate_dir_name, dir_path,
                                             journal_path, live_path,
                                             manifest_path, info.fname)) {
      continue;
    }

    bool live_exists = false;
    bool journal_exists = false;
    bool manifest_exists = false;
    if (!logger_session_path_exists(live_path, &live_exists, &io_error) ||
        !logger_session_path_exists(journal_path, &journal_exists, &io_error) ||
        !logger_session_path_exists(manifest_path, &manifest_exists,
                                    &io_error)) {
      break;
    }

    if (live_exists) {
      logger_copy_string(dir_name_out, 64u, candidate_dir_name);
      logger_copy_string(dir_path_out, LOGGER_STORAGE_PATH_MAX, dir_path);
      logger_copy_string(journal_path_out, LOGGER_STORAGE_PATH_MAX,
                         journal_path);
      logger_copy_string(live_path_out, LOGGER_STORAGE_PATH_MAX, live_path);
      logger_copy_string(manifest_path_out, LOGGER_STORAGE_PATH_MAX,
                         manifest_path);
      if (live_present_out != NULL) {
        *live_present_out = true;
      }
      found = true;
      break;
    }

    if (found_journal_only || !journal_exists || manifest_exists) {
      continue;
    }
    logger_copy_string(dir_name_out, 64u, candidate_dir_name);
    logger_copy_string(dir_path_out, LOGGER_STORAGE_PATH_MAX, dir_path);
    logger_copy_string(journal_path_out, LOGGER_STORAGE_PATH_MAX, journal_path);
    logger_copy_string(live_path_out, LOGGER_STORAGE_PATH_MAX, live_path);
    logger_copy_string(manifest_path_out, LOGGER_STORAGE_PATH_MAX,
                       manifest_path);
    if (live_present_out != NULL) {
      *live_present_out = false;
    }
    found = true;
    found_journal_only = true;
  }

  if (f_closedir(&dir) != FR_OK) {
    io_error = true;
  }
  if (io_error_out != NULL) {
    *io_error_out = io_error;
  }
  return !io_error && found;
}

static bool logger_session_load_live_session_id(
    const char *live_path, char session_id_out[LOGGER_SESSION_ID_HEX_LEN + 1]) {
  logger_session_live_json_workspace_t *workspace =
      logger_session_live_json_workspace_acquire();
  size_t len = 0u;
  if (!logger_storage_read_file(live_path, workspace->buf,
                                sizeof(workspace->buf) - 1u, &len)) {
    logger_session_live_json_workspace_release(workspace);
    return false;
  }
  workspace->buf[len] = '\0';

  logger_json_doc_t doc;
  if (!logger_json_parse(&doc, workspace->buf, len, workspace->tokens,
                         LOGGER_SESSION_LIVE_JSON_TOKEN_MAX)) {
    logger_session_live_json_workspace_release(workspace);
    return false;
  }

  const jsmntok_t *root = logger_json_root(&doc);
  if (root == NULL || root->type != JSMN_OBJECT) {
    logger_session_live_json_workspace_release(workspace);
    return false;
  }

  const bool ok =
      logger_json_object_copy_string(&doc, root, "session_id", session_id_out,
                                     LOGGER_SESSION_ID_HEX_LEN + 1) &&
      strlen(session_id_out) == LOGGER_SESSION_ID_HEX_LEN;
  logger_session_live_json_workspace_release(workspace);
  return ok;
}

static bool logger_session_restore_state_from_scan(
    logger_session_state_t *session,
    const logger_session_recovery_workspace_t *workspace,
    logger_system_log_t *system_log, const logger_clock_status_t *clock) {
  logger_session_init(session);
  session->active = true;
  logger_copy_string(session->session_id, sizeof(session->session_id),
                     workspace->scan.session_id);
  logger_copy_string(session->study_day_local, sizeof(session->study_day_local),
                     workspace->scan.study_day_local);
  logger_copy_string(session->session_start_utc,
                     sizeof(session->session_start_utc),
                     workspace->scan.session_start_utc);
  logger_copy_string(session->session_end_utc, sizeof(session->session_end_utc),
                     workspace->scan.session_end_utc);
  logger_copy_string(session->session_start_reason,
                     sizeof(session->session_start_reason),
                     workspace->scan.session_start_reason);
  logger_copy_string(session->session_end_reason,
                     sizeof(session->session_end_reason),
                     workspace->scan.session_end_reason);
  logger_copy_string(session->session_dir_name,
                     sizeof(session->session_dir_name), workspace->dir_name);
  logger_copy_string(session->session_dir_path,
                     sizeof(session->session_dir_path), workspace->dir_path);
  logger_copy_string(session->journal_path, sizeof(session->journal_path),
                     workspace->journal_path);
  logger_copy_string(session->live_path, sizeof(session->live_path),
                     workspace->live_path);
  logger_copy_string(session->manifest_path, sizeof(session->manifest_path),
                     workspace->manifest_path);

  logger_session_writer_restore_from_scan(&session->writer, &workspace->scan);
  session->next_packet_seq_in_span = workspace->scan.next_packet_seq_in_span;
  session->span_count = workspace->scan.span_count;
  memcpy(session->spans, workspace->scan.spans, sizeof(session->spans));
  session->span_active = workspace->scan.active_span_open;
  session->current_span_index = workspace->scan.active_span_open
                                    ? workspace->scan.active_span_index
                                    : 0xffffffffu;
  logger_copy_string(
      session->current_span_id, sizeof(session->current_span_id),
      workspace->scan.active_span_open ? workspace->scan.active_span_id : NULL);
  if (workspace->scan.active_span_open &&
      !logger_hex_to_bytes_16(session->current_span_id,
                              session->current_span_id_raw)) {
    logger_session_log_recovery_issue(
        system_log, clock != NULL ? clock->now_utc : NULL,
        "session_recovery_failed", "{\"reason\":\"span_id_parse_failed\"}");
    return false;
  }
  session->quarantine_clock_invalid_at_start =
      workspace->scan.quarantine_clock_invalid_at_start;
  session->quarantine_clock_fixed_mid_session =
      workspace->scan.quarantine_clock_fixed_mid_session;
  session->quarantine_clock_jump = workspace->scan.quarantine_clock_jump;
  session->quarantine_recovery_after_reset =
      workspace->scan.quarantine_recovery_after_reset;
  logger_session_recompute_quarantine(session, clock);
  return true;
}

static bool logger_session_materialize_recovered_manifest(
    logger_session_state_t *session, logger_system_log_t *system_log,
    const logger_clock_status_t *clock) {
  const logger_session_manifest_ctx_t *mc = &session->manifest_ctx;

  logger_storage_status_t storage_now = mc->storage;
  (void)logger_storage_refresh(&storage_now);

  char journal_sha256[LOGGER_SHA256_HEX_LEN + 1];
  uint64_t journal_size_bytes = 0u;
  if (!logger_session_compute_file_sha256(session->journal_path, journal_sha256,
                                          &journal_size_bytes)) {
    logger_session_log_recovery_issue(
        system_log, clock != NULL ? clock->now_utc : NULL,
        "session_recovery_failed", "{\"reason\":\"journal_hash_failed\"}");
    return false;
  }

  char *manifest = session->writer.manifest_buf;
  size_t manifest_len = 0u;
  logger_persisted_state_t *persisted_for_manifest =
      logger_session_manifest_persisted_acquire();
  logger_session_manifest_ctx_copy_persisted(mc, persisted_for_manifest);
  const bool build_ok = logger_session_build_manifest(
      session, mc->hardware_id, persisted_for_manifest, &storage_now,
      journal_sha256, journal_size_bytes, manifest,
      sizeof(session->writer.manifest_buf), &manifest_len);
  logger_session_manifest_persisted_release(persisted_for_manifest);
  if (!build_ok) {
    logger_session_log_recovery_issue(
        system_log, clock != NULL ? clock->now_utc : NULL,
        "session_recovery_failed", "{\"reason\":\"manifest_build_failed\"}");
    return false;
  }

  if (!logger_storage_write_file_atomic(session->manifest_path, manifest,
                                        manifest_len)) {
    logger_session_log_recovery_issue(
        system_log, clock != NULL ? clock->now_utc : NULL,
        "session_recovery_failed", "{\"reason\":\"manifest_write_failed\"}");
    return false;
  }
  return true;
}

void logger_session_init(logger_session_state_t *session) {
  assert(session != NULL);

  /* Preserve pipe pointer across reinit */
  struct capture_pipe *const pipe = session->pipe;

  /*
   * Force-close any open journal handle before zeroing the struct.
   *
   * When the pipe is attached, the journal handle is owned by core 1.
   * Core 1 opens it during SESSION_START dispatch and closes it during
   * FINALIZE_SESSION dispatch.  By the time we reach this reinit,
   * core 1 has already closed the handle, so is_open returns false
   * and no FatFS call occurs.
   *
   * When the pipe is NULL (boot recovery, unit tests), we're on the
   * same core that opened the handle, so force_close is safe.
   *
   * Do NOT call force_close when the pipe is attached and the handle
   * appears open — that would be a FatFS call from the wrong core.
   */
  if (logger_journal_writer_is_open(&session->writer.journal_writer)) {
    if (pipe == NULL) {
      /* No pipe — we own the handle, safe to close */
      logger_journal_writer_force_close(&session->writer.journal_writer);
    }
    /* else: pipe attached — core 1 owns the handle, do not touch.
     * This should not happen in normal operation (core 1 closes
     * before dispatching the completion that leads here), but if
     * it does, skip the close to avoid a wrong-core FatFS call.
     * The handle leaks; the system is already in an error path. */
  }
  memset(session, 0, sizeof(*session));
  session->pipe = pipe;
  session->current_span_index = 0xffffffffu;
  logger_chunk_builder_init(&session->writer.chunk_builder, g_chunk_buf,
                            LOGGER_SESSION_CHUNK_BUF_SIZE);
  logger_journal_writer_init(&session->writer.journal_writer);
}

void logger_session_set_pipe(logger_session_state_t *session,
                             struct capture_pipe *pipe) {
  if (session != NULL) {
    session->pipe = pipe;
  }
}

bool logger_session_start_debug(
    logger_session_state_t *session, logger_system_log_t *system_log,
    const char *hardware_id, const logger_persisted_state_t *persisted,
    const logger_clock_status_t *clock, const logger_battery_status_t *battery,
    const logger_storage_status_t *storage, logger_fault_code_t current_fault,
    uint32_t boot_counter, uint32_t now_ms, const char **error_code_out,
    const char **error_message_out) {
  (void)battery;
  (void)current_fault;

  if (session->active) {
    logger_session_set_error(error_code_out, error_message_out,
                             "not_permitted_in_mode",
                             "debug session is already active");
    return false;
  }
  return logger_session_start_new_active_internal(
      session, system_log, hardware_id, persisted, clock, storage,
      "session_start", persisted->config.bound_h10_address, false, false, false,
      true, boot_counter, now_ms, error_code_out, error_message_out);
}

bool logger_session_refresh_live(logger_session_state_t *session,
                                 const logger_clock_status_t *clock,
                                 uint32_t boot_counter, uint32_t now_ms) {
  if (!session->active) {
    return false;
  }
  logger_session_recompute_quarantine(session, clock);

  logger_writer_cmd_t cmd;
  memset(&cmd, 0, sizeof(cmd));
  cmd.type = LOGGER_WRITER_REFRESH_LIVE;
  cmd.refresh_live.boot_counter = boot_counter;
  cmd.refresh_live.now_ms = now_ms;
  if (clock != NULL && clock->now_utc[0] != '\0') {
    logger_copy_string(cmd.refresh_live.now_utc,
                       sizeof(cmd.refresh_live.now_utc), clock->now_utc);
  }

  return session_dispatch(session, &cmd);
}

bool logger_session_write_status_snapshot(
    logger_session_state_t *session, const logger_clock_status_t *clock,
    const logger_battery_status_t *battery,
    const logger_storage_status_t *storage, logger_fault_code_t current_fault,
    uint32_t boot_counter, uint32_t now_ms) {
  if (!session->active) {
    return false;
  }

  /* Control-plane: recompute quarantine state from current clock */
  logger_session_recompute_quarantine(session, clock);

  const int64_t utc_ns = logger_session_observed_utc_ns_or_zero(clock);

  /* Construct command with all data copied in */
  logger_writer_cmd_t cmd;
  memset(&cmd, 0, sizeof(cmd));
  cmd.type = LOGGER_WRITER_WRITE_STATUS_SNAPSHOT;
  logger_writer_write_status_snapshot_t *s = &cmd.write_status_snapshot;
  s->boot_counter = boot_counter;
  s->now_ms = now_ms;
  s->utc_ns = utc_ns;
  logger_copy_string(s->session_id, sizeof(s->session_id), session->session_id);
  if (session->span_active) {
    logger_copy_string(s->active_span_id, sizeof(s->active_span_id),
                       session->current_span_id);
  }
  s->battery_voltage_mv = battery->voltage_mv;
  s->battery_estimate_pct = (int16_t)battery->estimate_pct;
  s->vbus_present = battery->vbus_present;
  s->sd_free_bytes = storage->free_bytes;
  s->sd_reserve_bytes = LOGGER_SD_MIN_FREE_RESERVE_BYTES;
  s->quarantined = session->quarantined;
  logger_copy_string(s->fault_code, sizeof(s->fault_code),
                     logger_fault_code_name(current_fault));
  if (clock != NULL && clock->now_utc[0] != '\0') {
    logger_copy_string(s->now_utc, sizeof(s->now_utc), clock->now_utc);
  }

  return session_dispatch(session, &cmd);
}

bool logger_session_write_marker(logger_session_state_t *session,
                                 const logger_clock_status_t *clock,
                                 uint32_t boot_counter, uint32_t now_ms) {
  if (!session->active || !session->span_active) {
    return false;
  }

  const int64_t utc_ns = logger_session_observed_utc_ns_or_zero(clock);

  /* Control-plane decision: session/span active. Construct command. */
  logger_writer_cmd_t cmd;
  memset(&cmd, 0, sizeof(cmd));
  cmd.type = LOGGER_WRITER_WRITE_MARKER;
  logger_writer_write_marker_t *m = &cmd.write_marker;
  m->boot_counter = boot_counter;
  m->now_ms = now_ms;
  m->utc_ns = utc_ns;
  logger_copy_string(m->session_id, sizeof(m->session_id), session->session_id);
  logger_copy_string(m->span_id, sizeof(m->span_id), session->current_span_id);

  return session_dispatch(session, &cmd);
}

bool logger_session_ensure_active_span(
    logger_session_state_t *session, logger_system_log_t *system_log,
    const char *hardware_id, const logger_persisted_state_t *persisted,
    const logger_clock_status_t *clock, const logger_storage_status_t *storage,
    const char *start_reason, const char *h10_address, bool encrypted,
    bool bonded, bool clock_jump_at_session_start, uint32_t boot_counter,
    uint32_t now_ms, const char **error_code_out,
    const char **error_message_out) {

  if (session->active && session->span_active) {
    return true;
  }
  if (!session->active) {
    return logger_session_start_new_active_internal(
        session, system_log, hardware_id, persisted, clock, storage,
        start_reason, h10_address, encrypted, bonded,
        clock_jump_at_session_start, false, boot_counter, now_ms,
        error_code_out, error_message_out);
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
      /* live.json through the pipe (core 1 owns FatFS) */
      !logger_session_refresh_live(session, clock, boot_counter, now_ms)) {
    logger_session_set_error(error_code_out, error_message_out,
                             "storage_unavailable",
                             "failed to write reconnect span records");
    return false;
  }

  return true;
}

/*
 * Prepare the stable skeleton of an APPEND_PMD_PACKET command.
 * Called once before a drain loop; per-packet fields are then set by
 * logger_session_pmd_cmd_submit() without zeroing ~320 bytes each time.
 */
void logger_session_pmd_cmd_init(logger_session_state_t *session,
                                 const logger_clock_status_t *clock,
                                 logger_writer_cmd_t *cmd) {
  memset(cmd, 0, sizeof(*cmd));
  cmd->type = LOGGER_WRITER_APPEND_PMD_PACKET;
  logger_writer_append_pmd_packet_t *p = &cmd->append_pmd_packet;
  p->now_ms = (uint32_t)to_ms_since_boot(get_absolute_time());
  (void)logger_clock_observed_utc_ns(clock, &p->utc_ns);
  memcpy(p->span_id_raw, session->current_span_id_raw, 16);
}

/*
 * Update per-packet fields on an existing command skeleton.
 * No memset — only the fields that change between packets are written.
 * The value[] tail beyond value_len is stale but unread by the consumer.
 */
bool logger_session_pmd_cmd_submit(logger_session_state_t *session,
                                   logger_writer_cmd_t *cmd,
                                   uint16_t stream_kind, uint64_t mono_us,
                                   const uint8_t *value, size_t value_len) {
  if (!session->active || !session->span_active || value == NULL ||
      value_len == 0u || value_len > LOGGER_H10_PACKET_MAX_BYTES ||
      (stream_kind != LOGGER_SESSION_STREAM_KIND_ECG &&
       stream_kind != LOGGER_SESSION_STREAM_KIND_ACC)) {
    return false;
  }

  const uint32_t t0 = (uint32_t)to_us_since_boot(get_absolute_time());

  logger_writer_append_pmd_packet_t *p = &cmd->append_pmd_packet;
  p->stream_kind = stream_kind;
  p->seq_in_span = session->next_packet_seq_in_span;
  p->mono_us = mono_us;
  p->value_len = (uint16_t)value_len;
  memcpy(p->value, value, value_len);

  const bool ok = session_dispatch(session, cmd);

  const uint32_t elapsed = (uint32_t)to_us_since_boot(get_absolute_time()) - t0;
  logger_capture_stats_record_session_append(g_session_stats, elapsed, ok);
  logger_capture_stats_record_journal_append(g_session_stats, ok);

  if (ok) {
    session->next_packet_seq_in_span += 1u;
  }
  return ok;
}

bool logger_session_append_pmd_packet(logger_session_state_t *session,
                                      const logger_clock_status_t *clock,
                                      uint16_t stream_kind, uint64_t mono_us,
                                      const uint8_t *value, size_t value_len) {
  if (!session->active || !session->span_active || value == NULL ||
      value_len == 0u || value_len > LOGGER_H10_PACKET_MAX_BYTES ||
      session->current_span_index >= session->span_count ||
      (stream_kind != LOGGER_SESSION_STREAM_KIND_ECG &&
       stream_kind != LOGGER_SESSION_STREAM_KIND_ACC)) {
    return false;
  }

  logger_writer_cmd_t cmd;
  logger_session_pmd_cmd_init(session, clock, &cmd);
  return logger_session_pmd_cmd_submit(session, &cmd, stream_kind, mono_us,
                                       value, value_len);
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

  /* Core 1 owns chunk_builder.  Core 0 must not peek at has_data or
   * first_packet_time_ms here while the writer may be appending packets.
   * Send a cheap writer-side check instead; core 1 will seal only if the
   * chunk is old enough. */
  logger_writer_cmd_t cmd;
  memset(&cmd, 0, sizeof(cmd));
  cmd.type = LOGGER_WRITER_FLUSH_BARRIER;
  cmd.flush_barrier.now_ms = now_ms;
  cmd.flush_barrier.force = false;
  return session_dispatch(session, &cmd);
}

/*
 * handle_disconnect and handle_clock_event are compound operations
 * (span_end + gap + live_refresh, or span_end + clock_event + live_refresh)
 * that need atomicity across multiple writer commands.  Each sub-command
 * is dispatched through the pipe in order, so core 1 processes them
 * sequentially.  If any sub-command fails, the function returns false
 * immediately so the caller can enter the sd_write_failed recovery path.
 *
 * Cold-path operations — no hot-path benefit from batching.
 */
bool logger_session_handle_disconnect(logger_session_state_t *session,
                                      const logger_clock_status_t *clock,
                                      uint32_t boot_counter, uint32_t now_ms,
                                      const char *gap_reason) {
  if (!session->active || !session->span_active) {
    return true;
  }

  char ended_span_id[LOGGER_SESSION_ID_HEX_LEN + 1];
  ended_span_id[0] = '\0';
  /* close_span dispatches SPAN_END through the pipe */
  if (!logger_session_control_close_span(session, clock, "disconnect",
                                         boot_counter, now_ms, ended_span_id)) {
    return false;
  }
  /* gap dispatches WRITE_GAP through the pipe */
  if (!logger_session_append_gap(session, clock, ended_span_id,
                                 gap_reason != NULL ? gap_reason : "disconnect",
                                 boot_counter, now_ms)) {
    return false;
  }
  /* live.json through the pipe */
  return logger_session_refresh_live(session, clock, boot_counter, now_ms);
}

/*
 * Clock event: dispatch WRITE_CLOCK_EVENT through the pipe.
 *
 * The writer (core 1) handles the barrier (seal + write + sync).
 * If split_span is true, the SPAN_END dispatch happens first.
 * Both are cold-path operations that now route through core 1.
 */
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

  if (strcmp(event_kind, "clock_fixed") == 0) {
    session->quarantine_clock_fixed_mid_session = true;
  } else if (strcmp(event_kind, "clock_jump") == 0) {
    session->quarantine_clock_jump = true;
  }
  logger_session_recompute_quarantine(session, clock);

  /* Close span through the pipe if requested */
  if (split_span && session->span_active) {
    if (!logger_session_control_close_span(
            session, clock,
            span_end_reason != NULL ? span_end_reason : "clock_jump",
            boot_counter, now_ms, NULL)) {
      return false;
    }
  }

  /* Dispatch WRITE_CLOCK_EVENT to core 1 */
  logger_writer_cmd_t cmd;
  memset(&cmd, 0, sizeof(cmd));
  cmd.type = LOGGER_WRITER_WRITE_CLOCK_EVENT;
  logger_writer_write_clock_event_t *ce = &cmd.write_clock_event;
  ce->boot_counter = boot_counter;
  ce->now_ms = now_ms;
  ce->utc_ns = logger_session_observed_utc_ns_or_zero(clock);
  logger_copy_string(ce->session_id, sizeof(ce->session_id),
                     session->session_id);
  logger_copy_string(ce->event_kind, sizeof(ce->event_kind), event_kind);
  ce->delta_ns = delta_ns;
  ce->old_utc_ns = old_utc_ns;
  ce->new_utc_ns = new_utc_ns;
  if (!session_dispatch(session, &cmd)) {
    return false;
  }

  /* live.json through the pipe */
  return logger_session_refresh_live(session, clock, boot_counter, now_ms);
}

bool logger_session_append_h10_battery(logger_session_state_t *session,
                                       const logger_clock_status_t *clock,
                                       uint32_t boot_counter, uint32_t now_ms,
                                       uint8_t battery_percent,
                                       const char *read_reason) {
  if (!session->active) {
    return true;
  }

  const int64_t utc_ns = logger_session_observed_utc_ns_or_zero(clock);

  logger_writer_cmd_t cmd;
  memset(&cmd, 0, sizeof(cmd));
  cmd.type = LOGGER_WRITER_WRITE_H10_BATTERY;
  logger_writer_write_h10_battery_t *b = &cmd.write_h10_battery;
  b->boot_counter = boot_counter;
  b->now_ms = now_ms;
  b->utc_ns = utc_ns;
  logger_copy_string(b->session_id, sizeof(b->session_id), session->session_id);
  if (session->span_active) {
    logger_copy_string(b->span_id, sizeof(b->span_id),
                       session->current_span_id);
  }
  b->battery_percent = battery_percent;
  logger_copy_string(b->read_reason, sizeof(b->read_reason),
                     read_reason != NULL ? read_reason : "periodic");

  return session_dispatch(session, &cmd);
}

bool logger_session_finalize(logger_session_state_t *session,
                             logger_system_log_t *system_log,
                             const logger_persisted_state_t *persisted,
                             const logger_clock_status_t *clock,
                             const char *end_reason, uint32_t boot_counter,
                             uint32_t now_ms) {
  return logger_session_finalize_internal(session, system_log, persisted, clock,
                                          end_reason != NULL ? end_reason
                                                             : "manual_stop",
                                          false, boot_counter, now_ms);
}

bool logger_session_stop_debug(logger_session_state_t *session,
                               logger_system_log_t *system_log,
                               const logger_persisted_state_t *persisted,
                               const logger_clock_status_t *clock,
                               uint32_t boot_counter, uint32_t now_ms) {
  return logger_session_finalize_internal(session, system_log, persisted, clock,
                                          "manual_stop", true, boot_counter,
                                          now_ms);
}

bool logger_session_recover_on_boot(
    logger_session_state_t *session, logger_system_log_t *system_log,
    const char *hardware_id, const logger_persisted_state_t *persisted,
    const logger_clock_status_t *clock, const logger_storage_status_t *storage,
    uint32_t boot_counter, uint32_t now_ms, bool resume_allowed,
    bool *recovered_active_out, bool *closed_session_out) {
  logger_session_recovery_workspace_t *workspace = NULL;

  if (recovered_active_out != NULL) {
    *recovered_active_out = false;
  }
  if (closed_session_out != NULL) {
    *closed_session_out = false;
  }
  if (!logger_storage_ready_for_logging(storage)) {
    return true;
  }

  workspace = logger_session_recovery_workspace_acquire();

  bool live_scan_error = false;
  if (!logger_session_find_recovery_paths(
          workspace->dir_name, workspace->dir_path, workspace->journal_path,
          workspace->live_path, workspace->manifest_path,
          &workspace->live_present, &live_scan_error)) {
    logger_session_recovery_workspace_release(workspace);
    if (live_scan_error) {
      logger_session_log_recovery_issue(
          system_log, clock != NULL ? clock->now_utc : NULL,
          "session_recovery_failed", "{\"reason\":\"live_scan_failed\"}");
      return false;
    }
    return true;
  }

  const bool have_live_session_id =
      workspace->live_present &&
      logger_session_load_live_session_id(workspace->live_path,
                                          workspace->live_session_id);

  if (!logger_journal_scan(workspace->journal_path, &workspace->scan) ||
      !workspace->scan.valid) {
    logger_session_recovery_workspace_release(workspace);
    logger_session_log_recovery_issue(
        system_log, clock != NULL ? clock->now_utc : NULL,
        "session_recovery_failed", "{\"reason\":\"journal_scan_failed\"}");
    return false;
  }
  uint64_t actual_file_size = 0u;
  if (!logger_storage_file_size(workspace->journal_path, &actual_file_size)) {
    logger_session_recovery_workspace_release(workspace);
    return false;
  }
  if (actual_file_size > workspace->scan.valid_size_bytes &&
      !logger_journal_truncate_to_valid(workspace->journal_path,
                                        workspace->scan.valid_size_bytes)) {
    logger_session_recovery_workspace_release(workspace);
    logger_session_log_recovery_issue(
        system_log, clock != NULL ? clock->now_utc : NULL,
        "session_recovery_failed", "{\"reason\":\"journal_truncate_failed\"}");
    return false;
  }
  if (!workspace->scan.saw_session_start) {
    const bool hard_failure = workspace->live_present;
    logger_session_log_recovery_issue(
        system_log, clock != NULL ? clock->now_utc : NULL,
        hard_failure ? "session_recovery_failed" : "session_recovery_skipped",
        hard_failure ? "{\"reason\":\"journal_missing_session_start\","
                       "\"live_present\":true}"
                     : "{\"reason\":\"journal_missing_session_start\","
                       "\"live_present\":false}");
    logger_session_recovery_workspace_release(workspace);
    return !hard_failure;
  }
  if (have_live_session_id &&
      strcmp(workspace->live_session_id, workspace->scan.session_id) != 0) {
    logger_session_recovery_workspace_release(workspace);
    logger_session_log_recovery_issue(
        system_log, clock != NULL ? clock->now_utc : NULL,
        "session_recovery_failed", "{\"reason\":\"live_journal_id_mismatch\"}");
    return false;
  }

  if (!logger_session_restore_state_from_scan(session, workspace, system_log,
                                              clock)) {
    logger_session_recovery_workspace_release(workspace);
    return false;
  }

  /*
   * Populate manifest context for the recovered session.
   * Needed if this session is later finalized (either the !resume_allowed
   * path below, or deferred finalization from step_boot after the worker
   * is live).  hardware_id is silicon identity; config may have changed
   * since the session was created, so we snapshot the current values.
   */
  {
    logger_session_manifest_ctx_t *mc = &session->manifest_ctx;
    logger_session_manifest_ctx_seed_recovered(mc, hardware_id,
                                               &workspace->scan, persisted,
                                               storage, false, system_log);
  }

  const bool manifest_exists =
      logger_storage_file_exists(workspace->manifest_path);
  if (manifest_exists) {
    if (workspace->live_present) {
      (void)logger_storage_remove_file(workspace->live_path);
    }
    if (!logger_upload_queue_refresh_file(
            system_log, clock != NULL ? clock->now_utc : NULL, NULL)) {
      logger_session_recovery_workspace_release(workspace);
      return false;
    }
    logger_session_init(session);
    logger_session_recovery_workspace_release(workspace);
    return true;
  }

  if (workspace->scan.session_closed) {
    if (!logger_session_materialize_recovered_manifest(session, system_log,
                                                       clock)) {
      logger_session_recovery_workspace_release(workspace);
      return false;
    }
    if (workspace->live_present) {
      (void)logger_storage_remove_file(workspace->live_path);
    }
    if (!logger_upload_queue_refresh_file(
            system_log, clock != NULL ? clock->now_utc : NULL, NULL)) {
      logger_session_recovery_workspace_release(workspace);
      return false;
    }
    if (closed_session_out != NULL) {
      *closed_session_out = true;
    }
    logger_session_init(session);
    logger_session_recovery_workspace_release(workspace);
    return true;
  }

  /* Pre-worker recovery runs on core 0 before core 1 owns writer-domain
   * state.  Open the reconstructed journal only for sessions that still need
   * appends (resume, recovery markers, or unexpected-reboot finalization). */
  if (!logger_journal_writer_open_existing(&session->writer.journal_writer,
                                           workspace->journal_path,
                                           workspace->scan.valid_size_bytes)) {
    logger_session_recovery_workspace_release(workspace);
    logger_session_log_recovery_issue(
        system_log, clock != NULL ? clock->now_utc : NULL,
        "session_recovery_failed",
        "{\"reason\":\"journal_writer_open_failed\"}");
    return false;
  }

  if (!resume_allowed) {
    if (!logger_session_finalize_internal(session, system_log, persisted, clock,
                                          "unexpected_reboot", false,
                                          boot_counter, now_ms)) {
      logger_session_recovery_workspace_release(workspace);
      return false;
    }
    if (closed_session_out != NULL) {
      *closed_session_out = true;
    }
    logger_session_recovery_workspace_release(workspace);
    return true;
  }

  char ended_span_id[LOGGER_SESSION_ID_HEX_LEN + 1];
  ended_span_id[0] = '\0';
  if (session->span_active) {
    if (!logger_session_control_close_span(session, clock, "unexpected_reboot",
                                           boot_counter, now_ms,
                                           ended_span_id)) {
      logger_session_recovery_workspace_release(workspace);
      return false;
    }
    if (!logger_session_append_gap(session, clock, ended_span_id,
                                   "recovery_reboot", boot_counter, now_ms)) {
      logger_session_recovery_workspace_release(workspace);
      return false;
    }
  }
  session->quarantine_recovery_after_reset = true;
  logger_session_recompute_quarantine(session, clock);
  if (!logger_session_append_recovery(session, clock, "unexpected_reboot",
                                      boot_counter, now_ms)) {
    logger_session_recovery_workspace_release(workspace);
    return false;
  }
  if (!logger_session_begin_span(session, clock, "recovery_resume",
                                 persisted->config.bound_h10_address, false,
                                 false, boot_counter, now_ms)) {
    logger_session_recovery_workspace_release(workspace);
    return false;
  }
  if (!logger_session_refresh_live(session, clock, boot_counter, now_ms)) {
    logger_session_recovery_workspace_release(workspace);
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
  logger_session_recovery_workspace_release(workspace);
  return true;
}

/* ═══════════════════════════════════════════════════════════════════
 * Writer protocol dispatch
 *
 * This is the boundary between control-plane decisions and
 * durable-storage actions.  During normal operation it runs on
 * core 1 (via the capture-pipe worker loop).  During boot recovery
 * (pipe == NULL), it runs inline on core 0.
 *
 * Ownership rules encoded here:
 *   - record_seq: incremented only in this function (writer-side)
 *   - chunk_seq_in_session: incremented only in this function
 *   - journal_size_bytes: updated only from writer results here
 *     (seal_active_chunk and each dispatch case that emits records)
 *   - callers MUST NOT re-read durable_size after dispatch returns;
 *     the value is already current
 *   - next_packet_seq_in_span: NEVER touched here (control-core owns)
 * ═══════════════════════════════════════════════════════════════════ */

/*
 * Writer-side helper: serialize and emit a session_start journal record.
 * All data comes from the command struct — no control-plane state needed.
 */
static bool __attribute__((noinline))
writer_emit_session_start(logger_session_state_t *session,
                          const logger_writer_session_start_t *cmd) {
  char logger_id_escaped[LOGGER_CONFIG_LOGGER_ID_MAX * 2u];
  char subject_id_escaped[LOGGER_CONFIG_SUBJECT_ID_MAX * 2u];
  char timezone_escaped[LOGGER_CONFIG_TIMEZONE_MAX * 2u];
  logger_json_escape_into(logger_id_escaped, sizeof(logger_id_escaped),
                          cmd->logger_id);
  logger_json_escape_into(subject_id_escaped, sizeof(subject_id_escaped),
                          cmd->subject_id);
  logger_json_escape_into(timezone_escaped, sizeof(timezone_escaped),
                          cmd->timezone);

  char payload[LOGGER_SESSION_JSON_MAX];
  const int n = snprintf(
      payload, sizeof(payload),
      "{\"schema_version\":1,\"record_type\":\"session_start\",\"utc_ns\":%lld,"
      "\"mono_us\":%llu,\"boot_counter\":%lu,\"session_id\":\"%s\",\"study_day_"
      "local\":\"%s\",\"logger_id\":\"%s\",\"subject_id\":\"%s\",\"timezone\":"
      "\"%s\",\"clock_state\":\"%s\",\"start_reason\":\"%s\"}",
      (long long)cmd->utc_ns, (unsigned long long)cmd->now_ms * 1000ull,
      (unsigned long)cmd->boot_counter, cmd->session_id, cmd->study_day_local,
      logger_id_escaped, subject_id_escaped, timezone_escaped, cmd->clock_state,
      cmd->session_start_reason);
  return n > 0 && (size_t)n < sizeof(payload) &&
         logger_journal_writer_append_json(&session->writer.journal_writer,
                                           LOGGER_JOURNAL_RECORD_SESSION_START,
                                           session->writer.next_record_seq++,
                                           payload) &&
         logger_journal_writer_sync(&session->writer.journal_writer);
}

/*
 * Writer-side helper: serialize and emit a span_start journal record.
 */
static bool __attribute__((noinline))
writer_emit_span_start(logger_session_state_t *session,
                       const logger_writer_span_start_t *cmd) {
  char h10_escaped[18 * 2u];
  logger_json_escape_into(h10_escaped, sizeof(h10_escaped), cmd->h10_address);

  char payload[LOGGER_SESSION_JSON_MAX];
  const int n = snprintf(
      payload, sizeof(payload),
      "{\"schema_version\":1,\"record_type\":\"span_start\",\"utc_ns\":%lld,"
      "\"mono_us\":%llu,\"boot_counter\":%lu,\"session_id\":\"%s\",\"span_id\":"
      "\"%s\",\"span_index_in_session\":%lu,\"start_reason\":\"%s\",\"h10_"
      "address\":\"%s\",\"encrypted\":%s,\"bonded\":%s}",
      (long long)cmd->utc_ns, (unsigned long long)cmd->now_ms * 1000ull,
      (unsigned long)cmd->boot_counter, cmd->session_id, cmd->span_id,
      (unsigned long)cmd->span_index_in_session, cmd->start_reason, h10_escaped,
      cmd->encrypted ? "true" : "false", cmd->bonded ? "true" : "false");
  return n > 0 && (size_t)n < sizeof(payload) &&
         logger_journal_writer_append_json(
             &session->writer.journal_writer, LOGGER_JOURNAL_RECORD_SPAN_START,
             session->writer.next_record_seq++, payload) &&
         logger_journal_writer_sync(&session->writer.journal_writer);
}

/*
 * Writer-side helper: feed a PMD packet into the chunk builder.
 * Returns false only on writer-side failure (seal failure).
 * May seal and emit a chunk if the builder is full or at target size.
 */
static bool
writer_append_pmd_packet(logger_session_state_t *session,
                         const logger_writer_append_pmd_packet_t *cmd) {
  logger_chunk_builder_t *cb = &session->writer.chunk_builder;

  logger_chunk_result_t r = logger_chunk_builder_append(
      cb, cmd->stream_kind, cmd->span_id_raw, cmd->seq_in_span, cmd->mono_us,
      cmd->utc_ns, cmd->value, cmd->value_len, cmd->now_ms);

  if (r == LOGGER_CHUNK_FULL) {
    if (!logger_session_seal_active_chunk(session)) {
      return false;
    }
    r = logger_chunk_builder_append(cb, cmd->stream_kind, cmd->span_id_raw,
                                    cmd->seq_in_span, cmd->mono_us, cmd->utc_ns,
                                    cmd->value, cmd->value_len, cmd->now_ms);
    if (r == LOGGER_CHUNK_FULL) {
      /* Single packet too large for buffer — should not happen at 64 KiB */
      return false;
    }
  }

  if (r == LOGGER_CHUNK_SEAL) {
    return logger_session_seal_active_chunk(session);
  }

  return true;
}

/*
 * Writer-side helper: emit a marker journal record.
 * Caller (dispatch) handles the barrier seal.
 */
static bool __attribute__((noinline))
writer_emit_marker(logger_session_state_t *session,
                   const logger_writer_write_marker_t *cmd) {
  char payload[LOGGER_SESSION_JSON_MAX];
  const int n = snprintf(
      payload, sizeof(payload),
      "{\"schema_version\":1,\"record_type\":\"marker\",\"utc_ns\":%lld,\"mono_"
      "us\":%llu,\"boot_counter\":%lu,\"session_id\":\"%s\",\"span_id\":\"%s\","
      "\"marker_kind\":\"generic\"}",
      (long long)cmd->utc_ns, (unsigned long long)cmd->now_ms * 1000ull,
      (unsigned long)cmd->boot_counter, cmd->session_id, cmd->span_id);
  return n > 0 && (size_t)n < sizeof(payload) &&
         logger_journal_writer_append_json(
             &session->writer.journal_writer, LOGGER_JOURNAL_RECORD_MARKER,
             session->writer.next_record_seq++, payload) &&
         logger_journal_writer_sync(&session->writer.journal_writer);
}

/*
 * Writer-side helper: emit a status_snapshot journal record.
 */
static bool __attribute__((noinline))
writer_emit_status_snapshot(logger_session_state_t *session,
                            const logger_writer_write_status_snapshot_t *cmd) {
  char active_span_id_json[64];
  char fault_code_json[64];
  logger_json_string_literal(
      active_span_id_json, sizeof(active_span_id_json),
      cmd->active_span_id[0] != '\0' ? cmd->active_span_id : NULL);
  logger_json_string_literal(fault_code_json, sizeof(fault_code_json),
                             cmd->fault_code);

  char payload[LOGGER_SESSION_JSON_MAX];
  const int n = snprintf(
      payload, sizeof(payload),
      "{\"schema_version\":1,\"record_type\":\"status_snapshot\",\"utc_ns\":%"
      "lld,\"mono_us\":%llu,\"boot_counter\":%lu,\"session_id\":\"%s\","
      "\"active_span_id\":%s,\"battery_voltage_mv\":%u,\"battery_estimate_"
      "pct\":%d,\"vbus_present\":%s,\"sd_free_bytes\":%llu,\"sd_reserve_"
      "bytes\":%lu,\"wifi_enabled\":false,\"quarantined\":%s,\"fault_code\":%"
      "s}",
      (long long)cmd->utc_ns, (unsigned long long)cmd->now_ms * 1000ull,
      (unsigned long)cmd->boot_counter, cmd->session_id, active_span_id_json,
      (unsigned)cmd->battery_voltage_mv, (int)cmd->battery_estimate_pct,
      cmd->vbus_present ? "true" : "false",
      (unsigned long long)cmd->sd_free_bytes,
      (unsigned long)cmd->sd_reserve_bytes, cmd->quarantined ? "true" : "false",
      fault_code_json);
  return n > 0 && (size_t)n < sizeof(payload) &&
         logger_journal_writer_append_json(
             &session->writer.journal_writer,
             LOGGER_JOURNAL_RECORD_STATUS_SNAPSHOT,
             session->writer.next_record_seq++, payload) &&
         logger_journal_writer_sync(&session->writer.journal_writer);
}

/*
 * Writer-side helper: emit an h10_battery journal record.
 */
static bool __attribute__((noinline))
writer_emit_h10_battery(logger_session_state_t *session,
                        const logger_writer_write_h10_battery_t *cmd) {
  char span_id_json[64];
  logger_json_string_literal(span_id_json, sizeof(span_id_json),
                             cmd->span_id[0] != '\0' ? cmd->span_id : NULL);

  char payload[LOGGER_SESSION_JSON_MAX];
  const int n = snprintf(
      payload, sizeof(payload),
      "{\"schema_version\":1,\"record_type\":\"h10_battery\",\"utc_ns\":%lld,"
      "\"mono_us\":%llu,\"boot_counter\":%lu,\"session_id\":\"%s\",\"span_id\":"
      "%s,\"battery_percent\":%u,\"read_reason\":\"%s\"}",
      (long long)cmd->utc_ns, (unsigned long long)cmd->now_ms * 1000ull,
      (unsigned long)cmd->boot_counter, cmd->session_id, span_id_json,
      (unsigned)cmd->battery_percent, cmd->read_reason);
  return n > 0 && (size_t)n < sizeof(payload) &&
         logger_journal_writer_append_json(
             &session->writer.journal_writer, LOGGER_JOURNAL_RECORD_H10_BATTERY,
             session->writer.next_record_seq++, payload) &&
         logger_journal_writer_sync(&session->writer.journal_writer);
}

/*
 * Writer-side helper: emit a gap journal record.
 */
static bool __attribute__((noinline))
writer_emit_gap(logger_session_state_t *session,
                const logger_writer_write_gap_t *cmd) {
  char payload[LOGGER_SESSION_JSON_MAX];
  const int n = snprintf(
      payload, sizeof(payload),
      "{\"schema_version\":1,\"record_type\":\"gap\",\"utc_ns\":%lld,\"mono_"
      "us\":%llu,\"boot_counter\":%lu,\"session_id\":\"%s\",\"ended_span_id\":"
      "\"%s\",\"gap_reason\":\"%s\"}",
      (long long)cmd->utc_ns, (unsigned long long)cmd->now_ms * 1000ull,
      (unsigned long)cmd->boot_counter, cmd->session_id, cmd->ended_span_id,
      cmd->gap_reason);
  return n > 0 && (size_t)n < sizeof(payload) &&
         logger_journal_writer_append_json(
             &session->writer.journal_writer, LOGGER_JOURNAL_RECORD_GAP,
             session->writer.next_record_seq++, payload) &&
         logger_journal_writer_sync(&session->writer.journal_writer);
}

/*
 * Writer-side helper: emit a clock_event journal record.
 */
static bool __attribute__((noinline))
writer_emit_clock_event(logger_session_state_t *session,
                        const logger_writer_write_clock_event_t *cmd) {
  char payload[LOGGER_SESSION_JSON_MAX];
  const int n = snprintf(
      payload, sizeof(payload),
      "{\"schema_version\":1,\"record_type\":\"clock_event\",\"utc_ns\":%lld,"
      "\"mono_us\":%llu,\"boot_counter\":%lu,\"session_id\":\"%s\",\"event_"
      "kind\":\"%s\",\"delta_ns\":%lld,\"old_utc_ns\":%lld,\"new_utc_ns\":%"
      "lld}",
      (long long)cmd->utc_ns, (unsigned long long)cmd->now_ms * 1000ull,
      (unsigned long)cmd->boot_counter, cmd->session_id, cmd->event_kind,
      (long long)cmd->delta_ns, (long long)cmd->old_utc_ns,
      (long long)cmd->new_utc_ns);
  return n > 0 && (size_t)n < sizeof(payload) &&
         logger_journal_writer_append_json(
             &session->writer.journal_writer, LOGGER_JOURNAL_RECORD_CLOCK_EVENT,
             session->writer.next_record_seq++, payload) &&
         logger_journal_writer_sync(&session->writer.journal_writer);
}

/*
 * Writer-side helper: emit a recovery journal record.
 */
static bool __attribute__((noinline))
writer_emit_recovery(logger_session_state_t *session,
                     const logger_writer_write_recovery_t *cmd) {
  char payload[LOGGER_SESSION_JSON_MAX];
  const int n = snprintf(
      payload, sizeof(payload),
      "{\"schema_version\":1,\"record_type\":\"recovery\",\"utc_ns\":%lld,"
      "\"mono_us\":%llu,\"boot_counter\":%lu,\"session_id\":\"%s\",\"recovery_"
      "reason\":\"%s\",\"previous_reset_cause\":\"unknown\"}",
      (long long)cmd->utc_ns, (unsigned long long)cmd->now_ms * 1000ull,
      (unsigned long)cmd->boot_counter, cmd->session_id, cmd->recovery_reason);
  return n > 0 && (size_t)n < sizeof(payload) &&
         logger_journal_writer_append_json(
             &session->writer.journal_writer, LOGGER_JOURNAL_RECORD_RECOVERY,
             session->writer.next_record_seq++, payload) &&
         logger_journal_writer_sync(&session->writer.journal_writer);
}

/*
 * Writer-side helper: emit a span_end journal record.
 */
static bool __attribute__((noinline))
writer_emit_span_end(logger_session_state_t *session,
                     const logger_writer_span_end_t *cmd) {
  char payload[LOGGER_SESSION_JSON_MAX];
  const int n = snprintf(
      payload, sizeof(payload),
      "{\"schema_version\":1,\"record_type\":\"span_end\",\"utc_ns\":%lld,"
      "\"mono_us\":%llu,\"boot_counter\":%lu,\"session_id\":\"%s\",\"span_id\":"
      "\"%s\",\"end_reason\":\"%s\",\"packet_count\":%lu,\"first_seq_in_span\":"
      "%lu,\"last_seq_in_span\":%lu}",
      (long long)cmd->utc_ns, (unsigned long long)cmd->now_ms * 1000ull,
      (unsigned long)cmd->boot_counter, cmd->session_id, cmd->span_id,
      cmd->end_reason, (unsigned long)cmd->packet_count,
      (unsigned long)cmd->first_seq_in_span,
      (unsigned long)cmd->last_seq_in_span);
  return n > 0 && (size_t)n < sizeof(payload) &&
         logger_journal_writer_append_json(
             &session->writer.journal_writer, LOGGER_JOURNAL_RECORD_SPAN_END,
             session->writer.next_record_seq++, payload) &&
         logger_journal_writer_sync(&session->writer.journal_writer);
}

/*
 * Writer-side helper: emit a session_end journal record.
 */
static bool __attribute__((noinline))
writer_emit_session_end(logger_session_state_t *session,
                        const logger_writer_session_end_t *cmd) {
  char payload[LOGGER_SESSION_JSON_MAX];
  const int n = snprintf(
      payload, sizeof(payload),
      "{\"schema_version\":1,\"record_type\":\"session_end\",\"utc_ns\":%lld,"
      "\"mono_us\":%llu,\"boot_counter\":%lu,\"session_id\":\"%s\",\"end_"
      "reason\":\"%s\",\"span_count\":%lu,\"quarantined\":%s,\"quarantine_"
      "reasons\":%s}",
      (long long)cmd->utc_ns, (unsigned long long)cmd->now_ms * 1000ull,
      (unsigned long)cmd->boot_counter, cmd->session_id, cmd->end_reason,
      (unsigned long)cmd->span_count, cmd->quarantined ? "true" : "false",
      cmd->quarantine_reasons);
  return n > 0 && (size_t)n < sizeof(payload) &&
         logger_journal_writer_append_json(
             &session->writer.journal_writer, LOGGER_JOURNAL_RECORD_SESSION_END,
             session->writer.next_record_seq++, payload) &&
         logger_journal_writer_sync(&session->writer.journal_writer);
}

static bool __attribute__((noinline))
logger_writer_handle_span_start(logger_session_state_t *session,
                                const logger_writer_span_start_t *cmd) {
  if (cmd->span_index_in_session >= LOGGER_JOURNAL_MAX_SPANS) {
    return false;
  }
  if (!writer_emit_span_start(session, cmd)) {
    return false;
  }

  /* Writer-owned mirror and chunk state are updated only after the durable
   * span_start record is written.  Core 0 owns the control-plane span array;
   * core 1 owns this durable/manifest mirror. */
  logger_journal_span_summary_t *span =
      &session->writer.spans[cmd->span_index_in_session];
  memset(span, 0, sizeof(*span));
  span->present = true;
  logger_copy_string(span->span_id, sizeof(span->span_id), cmd->span_id);
  logger_copy_string(span->start_utc, sizeof(span->start_utc), cmd->start_utc);
  logger_copy_string(span->start_reason, sizeof(span->start_reason),
                     cmd->start_reason);
  logger_chunk_builder_reset(&session->writer.chunk_builder);
  session->writer.journal_size_bytes =
      logger_journal_writer_durable_size(&session->writer.journal_writer);
  return true;
}

static bool __attribute__((noinline))
logger_writer_handle_span_end(logger_session_state_t *session,
                              const logger_writer_span_end_t *cmd) {
  if (cmd->span_index_in_session >= LOGGER_JOURNAL_MAX_SPANS) {
    return false;
  }
  if (!logger_session_seal_active_chunk(session))
    return false;

  const logger_journal_span_summary_t *span =
      &session->writer.spans[cmd->span_index_in_session];
  logger_writer_span_end_t real = *cmd;
  real.packet_count = span->packet_count;
  real.first_seq_in_span = span->first_seq_in_span;
  real.last_seq_in_span = span->last_seq_in_span;
  if (!writer_emit_span_end(session, &real))
    return false;
  logger_journal_span_summary_t *mutable_span =
      &session->writer.spans[cmd->span_index_in_session];
  logger_copy_string(mutable_span->end_utc, sizeof(mutable_span->end_utc),
                     cmd->end_utc);
  logger_copy_string(mutable_span->end_reason, sizeof(mutable_span->end_reason),
                     cmd->end_reason);
  session->writer.journal_size_bytes =
      logger_journal_writer_durable_size(&session->writer.journal_writer);
  return true;
}

static bool __attribute__((noinline)) logger_writer_handle_finalize_session(
    logger_session_state_t *session,
    const logger_writer_finalize_session_t *cmd) {
  const logger_session_manifest_ctx_t *mc = &session->manifest_ctx;

  if (!logger_journal_writer_close(&session->writer.journal_writer))
    return false;
  session->writer.journal_size_bytes =
      logger_journal_writer_durable_size(&session->writer.journal_writer);

  logger_storage_status_t storage_now = mc->storage;
  (void)logger_storage_refresh(&storage_now);

  char journal_sha256[LOGGER_SHA256_HEX_LEN + 1];
  uint64_t journal_size_bytes = 0u;
  if (!logger_session_compute_file_sha256(session->journal_path, journal_sha256,
                                          &journal_size_bytes)) {
    return false;
  }

  char *manifest = session->writer.manifest_buf;
  size_t manifest_len = 0u;
  logger_persisted_state_t *persisted_for_manifest =
      logger_session_manifest_persisted_acquire();
  logger_session_manifest_ctx_copy_persisted(mc, persisted_for_manifest);
  if (!logger_session_build_manifest(
          session, mc->hardware_id, persisted_for_manifest, &storage_now,
          journal_sha256, journal_size_bytes, manifest,
          sizeof(session->writer.manifest_buf), &manifest_len)) {
    logger_session_manifest_persisted_release(persisted_for_manifest);
    return false;
  }
  logger_session_manifest_persisted_release(persisted_for_manifest);

  if (!logger_storage_write_file_atomic(session->manifest_path, manifest,
                                        manifest_len)) {
    return false;
  }

  (void)logger_storage_remove_file(session->live_path);

  const char *queue_utc = cmd->now_utc[0] != '\0' ? cmd->now_utc : NULL;
  return logger_upload_queue_refresh_file(mc->system_log, queue_utc, NULL);
}

bool logger_writer_dispatch(logger_session_context_t *ctx,
                            const logger_writer_cmd_t *cmd) {
  logger_session_state_t *const session = (logger_session_state_t *)ctx;

  switch (cmd->type) {
  case LOGGER_WRITER_SESSION_START: {
    /*
     * SESSION_START is the first SD I/O for a new session.
     * Create the session directory and journal file here on core 1
     * before writing the session_start record.
     */
    if (!logger_storage_ensure_dir(session->session_dir_path))
      return false;
    if (!logger_journal_writer_create(
            &session->writer.journal_writer, session->journal_path,
            session->session_id, cmd->session_start.boot_counter,
            cmd->session_start.utc_ns))
      return false;
    if (!writer_emit_session_start(session, &cmd->session_start))
      return false;
    session->writer.journal_size_bytes =
        logger_journal_writer_durable_size(&session->writer.journal_writer);
    return true;
  }

  case LOGGER_WRITER_SPAN_START:
    return logger_writer_handle_span_start(session, &cmd->span_start);

  case LOGGER_WRITER_APPEND_PMD_PACKET:
    return writer_append_pmd_packet(session, &cmd->append_pmd_packet);

  case LOGGER_WRITER_WRITE_MARKER:
    /* Barrier: seal in-flight chunk data before metadata record */
    if (!logger_session_seal_active_chunk(session))
      return false;
    if (!writer_emit_marker(session, &cmd->write_marker))
      return false;
    session->writer.journal_size_bytes =
        logger_journal_writer_durable_size(&session->writer.journal_writer);
    return true;

  case LOGGER_WRITER_WRITE_STATUS_SNAPSHOT:
    /* Barrier */
    if (!logger_session_seal_active_chunk(session))
      return false;
    if (!writer_emit_status_snapshot(session, &cmd->write_status_snapshot))
      return false;
    session->writer.journal_size_bytes =
        logger_journal_writer_durable_size(&session->writer.journal_writer);
    /* Status snapshot also refreshes live.json */
    return logger_session_write_live_internal(
        session,
        cmd->write_status_snapshot.now_utc[0] != '\0'
            ? cmd->write_status_snapshot.now_utc
            : NULL,
        cmd->write_status_snapshot.boot_counter,
        cmd->write_status_snapshot.now_ms);

  case LOGGER_WRITER_WRITE_H10_BATTERY:
    /* Barrier */
    if (!logger_session_seal_active_chunk(session))
      return false;
    if (!writer_emit_h10_battery(session, &cmd->write_h10_battery))
      return false;
    session->writer.journal_size_bytes =
        logger_journal_writer_durable_size(&session->writer.journal_writer);
    return true;

  case LOGGER_WRITER_WRITE_GAP:
    /* Barrier: spec §6.7 lists gap as an explicit seal trigger */
    if (!logger_session_seal_active_chunk(session))
      return false;
    if (!writer_emit_gap(session, &cmd->write_gap))
      return false;
    session->writer.journal_size_bytes =
        logger_journal_writer_durable_size(&session->writer.journal_writer);
    return true;

  case LOGGER_WRITER_WRITE_CLOCK_EVENT:
    /* Barrier */
    if (!logger_session_seal_active_chunk(session))
      return false;
    if (!writer_emit_clock_event(session, &cmd->write_clock_event))
      return false;
    session->writer.journal_size_bytes =
        logger_journal_writer_durable_size(&session->writer.journal_writer);
    return true;

  case LOGGER_WRITER_WRITE_RECOVERY:
    if (!writer_emit_recovery(session, &cmd->write_recovery))
      return false;
    session->writer.journal_size_bytes =
        logger_journal_writer_durable_size(&session->writer.journal_writer);
    return true;

  case LOGGER_WRITER_SPAN_END:
    return logger_writer_handle_span_end(session, &cmd->span_end);

  case LOGGER_WRITER_SESSION_END:
    /* Barrier: seal before session_end */
    if (!logger_session_seal_active_chunk(session))
      return false;
    if (!writer_emit_session_end(session, &cmd->session_end))
      return false;
    session->writer.journal_size_bytes =
        logger_journal_writer_durable_size(&session->writer.journal_writer);
    return true;

  case LOGGER_WRITER_FINALIZE_SESSION:
    return logger_writer_handle_finalize_session(session,
                                                 &cmd->finalize_session);

  case LOGGER_WRITER_REFRESH_LIVE:
    return logger_session_write_live_internal(
        session,
        cmd->refresh_live.now_utc[0] != '\0' ? cmd->refresh_live.now_utc : NULL,
        cmd->refresh_live.boot_counter, cmd->refresh_live.now_ms);

  case LOGGER_WRITER_FLUSH_BARRIER:
    if (!cmd->flush_barrier.force &&
        !logger_chunk_builder_age_exceeded(&session->writer.chunk_builder,
                                           cmd->flush_barrier.now_ms)) {
      return true;
    }
    if (!logger_session_seal_active_chunk(session))
      return false;
    session->writer.journal_size_bytes =
        logger_journal_writer_durable_size(&session->writer.journal_writer);
    return true;

  default:
    return false;
  }
}
