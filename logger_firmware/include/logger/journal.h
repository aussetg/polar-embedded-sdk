#ifndef LOGGER_FIRMWARE_JOURNAL_H
#define LOGGER_FIRMWARE_JOURNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LOGGER_JOURNAL_ID_HEX_LEN 32
#define LOGGER_JOURNAL_UTC_MAX 31
#define LOGGER_JOURNAL_REASON_MAX 31
#define LOGGER_JOURNAL_MAX_SPANS 16

typedef struct {
  bool present;
  char span_id[LOGGER_JOURNAL_ID_HEX_LEN + 1];
  char start_utc[LOGGER_JOURNAL_UTC_MAX + 1];
  char end_utc[LOGGER_JOURNAL_UTC_MAX + 1];
  char start_reason[LOGGER_JOURNAL_REASON_MAX + 1];
  char end_reason[LOGGER_JOURNAL_REASON_MAX + 1];
  uint32_t packet_count;
  uint32_t first_seq_in_span;
  uint32_t last_seq_in_span;
} logger_journal_span_summary_t;

typedef struct {
  bool valid;
  bool session_closed;
  bool active_span_open;
  bool quarantined;
  bool quarantine_clock_invalid_at_start;
  bool quarantine_clock_fixed_mid_session;
  bool quarantine_clock_jump;
  bool quarantine_recovery_after_reset;
  uint64_t valid_size_bytes;
  uint64_t next_record_seq;
  uint32_t next_chunk_seq_in_session;
  uint32_t next_packet_seq_in_span;
  uint32_t span_count;
  uint32_t active_span_index;
  char session_id[LOGGER_JOURNAL_ID_HEX_LEN + 1];
  char active_span_id[LOGGER_JOURNAL_ID_HEX_LEN + 1];
  char study_day_local[11];
  char session_start_utc[LOGGER_JOURNAL_UTC_MAX + 1];
  char session_end_utc[LOGGER_JOURNAL_UTC_MAX + 1];
  char session_start_reason[LOGGER_JOURNAL_REASON_MAX + 1];
  char session_end_reason[LOGGER_JOURNAL_REASON_MAX + 1];
  logger_journal_span_summary_t spans[LOGGER_JOURNAL_MAX_SPANS];
} logger_journal_scan_result_t;

typedef enum {
  LOGGER_JOURNAL_RECORD_SESSION_START = 0x0001,
  LOGGER_JOURNAL_RECORD_SPAN_START = 0x0002,
  LOGGER_JOURNAL_RECORD_DATA_CHUNK = 0x0003,
  LOGGER_JOURNAL_RECORD_STATUS_SNAPSHOT = 0x0004,
  LOGGER_JOURNAL_RECORD_MARKER = 0x0005,
  LOGGER_JOURNAL_RECORD_GAP = 0x0006,
  LOGGER_JOURNAL_RECORD_SPAN_END = 0x0007,
  LOGGER_JOURNAL_RECORD_SESSION_END = 0x0008,
  LOGGER_JOURNAL_RECORD_RECOVERY = 0x0009,
  LOGGER_JOURNAL_RECORD_CLOCK_EVENT = 0x000A,
  LOGGER_JOURNAL_RECORD_H10_BATTERY = 0x000B,
} logger_journal_record_type_t;

bool logger_journal_create(const char *path, const char *session_id_hex,
                           uint32_t boot_counter, int64_t journal_open_utc_ns,
                           uint64_t *size_bytes_out);

bool logger_journal_append_json_record(const char *path,
                                       logger_journal_record_type_t record_type,
                                       uint64_t record_seq,
                                       const char *json_payload,
                                       uint64_t *size_bytes_out);

bool logger_journal_append_binary_record(
    const char *path, logger_journal_record_type_t record_type,
    uint64_t record_seq, const void *payload, size_t payload_len,
    uint64_t *size_bytes_out);

bool logger_journal_scan(const char *path,
                         logger_journal_scan_result_t *result);
bool logger_journal_truncate_to_valid(const char *path,
                                      uint64_t valid_size_bytes);

#endif
