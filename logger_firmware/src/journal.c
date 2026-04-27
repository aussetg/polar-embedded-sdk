#include "logger/journal.h"

#include <assert.h>
#include <ctype.h>
#include <string.h>

#include "ff.h"

#include "logger/clock.h"
#include "logger/json.h"
#include "logger/storage.h"
#include "logger/util.h"

#define LOGGER_JOURNAL_FILE_HEADER_BYTES 64u
#define LOGGER_JOURNAL_RECORD_HEADER_BYTES 32u
#define LOGGER_JOURNAL_FLAG_JSON 0x00000001u
#define LOGGER_JOURNAL_FLAG_BINARY 0x00000002u
#define LOGGER_JOURNAL_SCAN_PAYLOAD_CAPTURE_MAX 1023u
#define LOGGER_JOURNAL_JSON_TOKEN_MAX 64u

typedef struct {
  uint8_t header[LOGGER_JOURNAL_FILE_HEADER_BYTES];
  uint8_t record_header[LOGGER_JOURNAL_RECORD_HEADER_BYTES];
  uint8_t payload_capture[LOGGER_JOURNAL_SCAN_PAYLOAD_CAPTURE_MAX + 1u];
  uint8_t chunk[128u];
  jsmntok_t tokens[LOGGER_JOURNAL_JSON_TOKEN_MAX];
  bool in_use;
} logger_journal_scan_workspace_t;

static logger_journal_scan_workspace_t g_journal_scan_workspace;

static logger_journal_scan_workspace_t *
logger_journal_scan_workspace_acquire(void) {
  assert(!g_journal_scan_workspace.in_use);
  memset(&g_journal_scan_workspace, 0, sizeof(g_journal_scan_workspace));
  g_journal_scan_workspace.in_use = true;
  return &g_journal_scan_workspace;
}

static void logger_journal_scan_workspace_release(
    logger_journal_scan_workspace_t *workspace) {
  assert(workspace == &g_journal_scan_workspace);
  assert(g_journal_scan_workspace.in_use);
  g_journal_scan_workspace.in_use = false;
}

static uint16_t logger_u16le(const uint8_t *src) {
  return (uint16_t)src[0] | ((uint16_t)src[1] << 8);
}

static uint32_t logger_u32le(const uint8_t *src) {
  return (uint32_t)src[0] | ((uint32_t)src[1] << 8) | ((uint32_t)src[2] << 16) |
         ((uint32_t)src[3] << 24);
}

static uint64_t logger_u64le(const uint8_t *src) {
  uint64_t value = 0u;
  for (size_t i = 0u; i < 8u; ++i) {
    value |= (uint64_t)src[i] << (8u * i);
  }
  return value;
}

static void logger_bytes_to_hex_16(const uint8_t in[16],
                                   char out[LOGGER_JOURNAL_ID_HEX_LEN + 1]) {
  static const char hex[] = "0123456789abcdef";
  for (size_t i = 0u; i < 16u; ++i) {
    out[i * 2u] = hex[(in[i] >> 4) & 0x0f];
    out[(i * 2u) + 1u] = hex[in[i] & 0x0f];
  }
  out[LOGGER_JOURNAL_ID_HEX_LEN] = '\0';
}

static logger_journal_span_summary_t *
logger_journal_find_span(logger_journal_scan_result_t *result,
                         const char *span_id, uint32_t index_hint) {
  if (span_id != NULL && span_id[0] != '\0') {
    for (uint32_t i = 0u;
         i < result->span_count && i < LOGGER_JOURNAL_MAX_SPANS; ++i) {
      if (result->spans[i].present &&
          strcmp(result->spans[i].span_id, span_id) == 0) {
        return &result->spans[i];
      }
    }
  }
  if (index_hint < LOGGER_JOURNAL_MAX_SPANS) {
    if (index_hint >= result->span_count) {
      result->span_count = index_hint + 1u;
    }
    return &result->spans[index_hint];
  }
  return NULL;
}

static void logger_journal_capture_utc(const logger_json_doc_t *doc,
                                       const jsmntok_t *object_tok,
                                       char out[LOGGER_JOURNAL_UTC_MAX + 1]) {
  int64_t utc_ns = 0ll;
  if (!logger_json_object_get_int64(doc, object_tok, "utc_ns", &utc_ns) ||
      !logger_clock_format_utc_ns_rfc3339(utc_ns, out)) {
    out[0] = '\0';
  }
}

static void
logger_journal_apply_json_record(logger_journal_scan_result_t *result,
                                 logger_journal_record_type_t record_type,
                                 const logger_json_doc_t *doc,
                                 const jsmntok_t *root) {
  switch (record_type) {
  case LOGGER_JOURNAL_RECORD_SESSION_START: {
    (void)logger_json_object_copy_string(doc, root, "session_id",
                                         result->session_id,
                                         sizeof(result->session_id));
    (void)logger_json_object_copy_string(doc, root, "study_day_local",
                                         result->study_day_local,
                                         sizeof(result->study_day_local));
    (void)logger_json_object_copy_string(doc, root, "start_reason",
                                         result->session_start_reason,
                                         sizeof(result->session_start_reason));
    (void)logger_json_object_copy_string(
        doc, root, "logger_id", result->logger_id, sizeof(result->logger_id));
    (void)logger_json_object_copy_string(doc, root, "subject_id",
                                         result->subject_id,
                                         sizeof(result->subject_id));
    (void)logger_json_object_copy_string(
        doc, root, "timezone", result->timezone, sizeof(result->timezone));
    logger_journal_capture_utc(doc, root, result->session_start_utc);
    char clock_state[16];
    if (logger_json_object_copy_string(doc, root, "clock_state", clock_state,
                                       sizeof(clock_state))) {
      if (strcmp(clock_state, "invalid") == 0) {
        result->quarantined = true;
        result->quarantine_clock_invalid_at_start = true;
      } else if (strcmp(clock_state, "jumped") == 0) {
        result->quarantined = true;
        result->quarantine_clock_jump = true;
      }
    }
    break;
  }
  case LOGGER_JOURNAL_RECORD_SPAN_START: {
    char span_id[LOGGER_JOURNAL_ID_HEX_LEN + 1];
    uint32_t span_index = result->span_count;
    if (!logger_json_object_copy_string(doc, root, "span_id", span_id,
                                        sizeof(span_id))) {
      break;
    }
    (void)logger_json_object_get_uint32(doc, root, "span_index_in_session",
                                        &span_index);
    logger_journal_span_summary_t *span =
        logger_journal_find_span(result, span_id, span_index);
    if (span == NULL) {
      break;
    }
    memset(span, 0, sizeof(*span));
    span->present = true;
    logger_copy_string(span->span_id, sizeof(span->span_id), span_id);
    (void)logger_json_object_copy_string(doc, root, "start_reason",
                                         span->start_reason,
                                         sizeof(span->start_reason));
    logger_journal_capture_utc(doc, root, span->start_utc);
    result->active_span_open = true;
    result->active_span_index = span_index;
    logger_copy_string(result->active_span_id, sizeof(result->active_span_id),
                       span_id);
    result->next_packet_seq_in_span = 0u;
    if (result->span_count <= span_index) {
      result->span_count = span_index + 1u;
    }
    break;
  }
  case LOGGER_JOURNAL_RECORD_SPAN_END: {
    char span_id[LOGGER_JOURNAL_ID_HEX_LEN + 1];
    if (!logger_json_object_copy_string(doc, root, "span_id", span_id,
                                        sizeof(span_id))) {
      logger_copy_string(span_id, sizeof(span_id), result->active_span_id);
    }
    logger_journal_span_summary_t *span =
        logger_journal_find_span(result, span_id, result->active_span_index);
    if (span == NULL) {
      break;
    }
    span->present = true;
    logger_copy_string(span->span_id, sizeof(span->span_id), span_id);
    (void)logger_json_object_copy_string(
        doc, root, "end_reason", span->end_reason, sizeof(span->end_reason));
    (void)logger_json_object_get_uint32(doc, root, "packet_count",
                                        &span->packet_count);
    (void)logger_json_object_get_uint32(doc, root, "first_seq_in_span",
                                        &span->first_seq_in_span);
    (void)logger_json_object_get_uint32(doc, root, "last_seq_in_span",
                                        &span->last_seq_in_span);
    logger_journal_capture_utc(doc, root, span->end_utc);
    if (strcmp(result->active_span_id, span_id) == 0) {
      result->active_span_open = false;
      result->active_span_id[0] = '\0';
      result->next_packet_seq_in_span = 0u;
    }
    break;
  }
  case LOGGER_JOURNAL_RECORD_SESSION_END:
    logger_journal_capture_utc(doc, root, result->session_end_utc);
    (void)logger_json_object_copy_string(doc, root, "end_reason",
                                         result->session_end_reason,
                                         sizeof(result->session_end_reason));
    (void)logger_json_object_get_bool(doc, root, "quarantined",
                                      &result->quarantined);
    result->session_closed = true;
    break;
  case LOGGER_JOURNAL_RECORD_RECOVERY:
    result->quarantined = true;
    result->quarantine_recovery_after_reset = true;
    break;
  case LOGGER_JOURNAL_RECORD_CLOCK_EVENT: {
    char event_kind[24];
    if (logger_json_object_copy_string(doc, root, "event_kind", event_kind,
                                       sizeof(event_kind))) {
      if (strcmp(event_kind, "clock_fixed") == 0) {
        result->quarantined = true;
        result->quarantine_clock_fixed_mid_session = true;
      } else if (strcmp(event_kind, "clock_jump") == 0) {
        result->quarantined = true;
        result->quarantine_clock_jump = true;
      }
    }
    break;
  }
  case LOGGER_JOURNAL_RECORD_DATA_CHUNK:
  case LOGGER_JOURNAL_RECORD_STATUS_SNAPSHOT:
  case LOGGER_JOURNAL_RECORD_MARKER:
  case LOGGER_JOURNAL_RECORD_GAP:
  case LOGGER_JOURNAL_RECORD_H10_BATTERY:
    break;
  }
}

static void
logger_journal_apply_binary_record(logger_journal_scan_result_t *result,
                                   logger_journal_record_type_t record_type,
                                   const uint8_t *payload, size_t payload_len) {
  if (record_type != LOGGER_JOURNAL_RECORD_DATA_CHUNK || payload == NULL ||
      payload_len < 80u) {
    return;
  }

  char span_id[LOGGER_JOURNAL_ID_HEX_LEN + 1];
  logger_bytes_to_hex_16(payload + 8u, span_id);

  const uint32_t chunk_seq_in_session = logger_u32le(payload + 4u);
  const uint32_t packet_count = logger_u32le(payload + 24u);
  const uint32_t first_seq_in_span = logger_u32le(payload + 28u);
  const uint32_t last_seq_in_span = logger_u32le(payload + 32u);

  logger_journal_span_summary_t *span =
      logger_journal_find_span(result, span_id, result->active_span_index);
  if (span == NULL) {
    return;
  }

  if (!span->present) {
    span->present = true;
    logger_copy_string(span->span_id, sizeof(span->span_id), span_id);
  }
  if (span->packet_count == 0u) {
    span->first_seq_in_span = first_seq_in_span;
  }
  span->packet_count += packet_count;
  span->last_seq_in_span = last_seq_in_span;

  if ((chunk_seq_in_session + 1u) > result->next_chunk_seq_in_session) {
    result->next_chunk_seq_in_session = chunk_seq_in_session + 1u;
  }
  if (result->active_span_open &&
      strcmp(result->active_span_id, span_id) == 0 &&
      (last_seq_in_span + 1u) > result->next_packet_seq_in_span) {
    result->next_packet_seq_in_span = last_seq_in_span + 1u;
  }
}

bool logger_journal_scan(const char *path,
                         logger_journal_scan_result_t *result) {
  if (path == NULL || result == NULL) {
    return false;
  }

  memset(result, 0, sizeof(*result));
  logger_journal_scan_workspace_t *workspace =
      logger_journal_scan_workspace_acquire();

  FIL file;
  if (f_open(&file, path, FA_READ) != FR_OK) {
    logger_journal_scan_workspace_release(workspace);
    return false;
  }

  UINT read_bytes = 0u;
  if (f_read(&file, workspace->header, sizeof(workspace->header),
             &read_bytes) != FR_OK ||
      read_bytes != sizeof(workspace->header)) {
    f_close(&file);
    logger_journal_scan_workspace_release(workspace);
    return false;
  }
  if (memcmp(workspace->header + 0, "NOF1JNL1", 8u) != 0 ||
      logger_u16le(workspace->header + 8) != LOGGER_JOURNAL_FILE_HEADER_BYTES ||
      logger_u16le(workspace->header + 10) != 1u) {
    f_close(&file);
    logger_journal_scan_workspace_release(workspace);
    return false;
  }
  const uint32_t expect_header_crc = logger_crc32_ieee(workspace->header, 56u);
  if (logger_u32le(workspace->header + 56) != expect_header_crc) {
    f_close(&file);
    logger_journal_scan_workspace_release(workspace);
    return false;
  }

  logger_bytes_to_hex_16(workspace->header + 16, result->session_id);
  result->valid = true;
  result->valid_size_bytes = LOGGER_JOURNAL_FILE_HEADER_BYTES;

  while (true) {
    read_bytes = 0u;
    const FRESULT header_fr =
        f_read(&file, workspace->record_header,
               sizeof(workspace->record_header), &read_bytes);
    if (header_fr != FR_OK) {
      break;
    }
    if (read_bytes == 0u) {
      break;
    }
    if (read_bytes != sizeof(workspace->record_header)) {
      break;
    }
    if (memcmp(workspace->record_header + 0, "RCD1", 4u) != 0) {
      break;
    }

    const uint16_t header_bytes = logger_u16le(workspace->record_header + 4);
    const logger_journal_record_type_t record_type =
        (logger_journal_record_type_t)logger_u16le(workspace->record_header +
                                                   6);
    const uint32_t total_bytes = logger_u32le(workspace->record_header + 8);
    const uint32_t payload_bytes = logger_u32le(workspace->record_header + 12);
    const uint32_t flags = logger_u32le(workspace->record_header + 16);
    const uint32_t payload_crc = logger_u32le(workspace->record_header + 20);
    const uint64_t record_seq = logger_u64le(workspace->record_header + 24);
    if (header_bytes != LOGGER_JOURNAL_RECORD_HEADER_BYTES ||
        total_bytes < header_bytes ||
        total_bytes != (header_bytes + payload_bytes) ||
        ((flags & (LOGGER_JOURNAL_FLAG_JSON | LOGGER_JOURNAL_FLAG_BINARY)) ==
         0u) ||
        ((flags & LOGGER_JOURNAL_FLAG_JSON) != 0u &&
         (flags & LOGGER_JOURNAL_FLAG_BINARY) != 0u)) {
      break;
    }

    uint32_t running_crc = logger_crc32_begin();
    memset(workspace->payload_capture, 0, sizeof(workspace->payload_capture));
    uint32_t payload_remaining = payload_bytes;
    size_t capture_offset = 0u;
    while (payload_remaining > 0u) {
      const UINT want = payload_remaining > sizeof(workspace->chunk)
                            ? sizeof(workspace->chunk)
                            : (UINT)payload_remaining;
      read_bytes = 0u;
      if (f_read(&file, workspace->chunk, want, &read_bytes) != FR_OK ||
          read_bytes != want) {
        payload_remaining = UINT32_MAX;
        break;
      }
      running_crc = logger_crc32_update(running_crc, workspace->chunk, want);
      const size_t capture_room =
          LOGGER_JOURNAL_SCAN_PAYLOAD_CAPTURE_MAX - capture_offset;
      const size_t capture_now =
          (capture_room < (size_t)want) ? capture_room : (size_t)want;
      if (capture_now > 0u) {
        memcpy(workspace->payload_capture + capture_offset, workspace->chunk,
               capture_now);
        capture_offset += capture_now;
      }
      payload_remaining -= want;
    }
    if (payload_remaining == UINT32_MAX) {
      break;
    }
    if (logger_crc32_finish(running_crc) != payload_crc) {
      break;
    }

    workspace->payload_capture[capture_offset] = '\0';
    result->valid_size_bytes += total_bytes;
    result->next_record_seq = record_seq + 1u;
    if ((flags & LOGGER_JOURNAL_FLAG_JSON) != 0u) {
      logger_json_doc_t doc;
      if (logger_json_parse(&doc, (const char *)workspace->payload_capture,
                            capture_offset, workspace->tokens,
                            LOGGER_JOURNAL_JSON_TOKEN_MAX)) {
        const jsmntok_t *root = logger_json_root(&doc);
        if (root != NULL && root->type == JSMN_OBJECT) {
          logger_journal_apply_json_record(result, record_type, &doc, root);
        }
      }
    } else if ((flags & LOGGER_JOURNAL_FLAG_BINARY) != 0u) {
      logger_journal_apply_binary_record(
          result, record_type, workspace->payload_capture, capture_offset);
    }
  }

  const bool ok = f_close(&file) == FR_OK;
  logger_journal_scan_workspace_release(workspace);
  return ok;
}

bool logger_journal_truncate_to_valid(const char *path,
                                      uint64_t valid_size_bytes) {
  return logger_storage_truncate_file(path, valid_size_bytes);
}
