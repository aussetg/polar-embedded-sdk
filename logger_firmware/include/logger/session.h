#ifndef LOGGER_FIRMWARE_SESSION_H
#define LOGGER_FIRMWARE_SESSION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "logger/battery.h"
#include "logger/clock.h"
#include "logger/config_store.h"
#include "logger/faults.h"
#include "logger/journal.h"
#include "logger/storage.h"
#include "logger/system_log.h"

#define LOGGER_SESSION_ID_HEX_LEN 32

#include "logger/capture_stats.h"
#include "logger/chunk_builder.h"
#include "logger/journal_writer.h"
#include "logger/writer_protocol.h"

void logger_session_set_capture_stats(logger_capture_stats_t *stats);

enum {
  LOGGER_SESSION_STREAM_KIND_ECG = 1u,
  LOGGER_SESSION_STREAM_KIND_ACC = 2u,
};

typedef struct logger_session_state {
  bool active;
  bool span_active;
  bool quarantined;
  bool quarantine_clock_invalid_at_start;
  bool quarantine_clock_fixed_mid_session;
  bool quarantine_clock_jump;
  bool quarantine_recovery_after_reset;
  char clock_state[8];
  char session_id[LOGGER_SESSION_ID_HEX_LEN + 1];
  char current_span_id[LOGGER_SESSION_ID_HEX_LEN + 1];
  uint32_t current_span_index;
  char study_day_local[11];
  char session_start_utc[LOGGER_JOURNAL_UTC_MAX + 1];
  char session_end_utc[LOGGER_JOURNAL_UTC_MAX + 1];
  char session_start_reason[LOGGER_JOURNAL_REASON_MAX + 1];
  char session_end_reason[LOGGER_JOURNAL_REASON_MAX + 1];
  char session_dir_name[64];
  char session_dir_path[LOGGER_STORAGE_PATH_MAX];
  char journal_path[LOGGER_STORAGE_PATH_MAX];
  char live_path[LOGGER_STORAGE_PATH_MAX];
  char manifest_path[LOGGER_STORAGE_PATH_MAX];
  /*
   * Cached raw (binary 128-bit) form of current_span_id.
   * Used by the chunk builder to avoid per-packet hex-to-bytes conversion.
   * Updated by the control core when span identity changes.
   */
  uint8_t current_span_id_raw[16];
  /*
   * Writer-side durable counters.  Assigned only when journal records
   * are actually emitted.  For the current inline path these are
   * incremented inside the writer dispatch; when the writer moves
   * to core 1, core 0 never touches these.
   */
  uint64_t next_record_seq;
  uint64_t journal_size_bytes;
  logger_journal_writer_t journal_writer;
  uint32_t next_chunk_seq_in_session;
  /* Control-core owned counters */
  uint32_t next_packet_seq_in_span;
  uint32_t span_count;
  logger_journal_span_summary_t spans[LOGGER_JOURNAL_MAX_SPANS];
  logger_chunk_builder_t chunk_builder;
} logger_session_state_t;

void logger_session_init(logger_session_state_t *session);

bool logger_session_start_debug(
    logger_session_state_t *session, logger_system_log_t *system_log,
    const char *hardware_id, const logger_persisted_state_t *persisted,
    const logger_clock_status_t *clock, const logger_battery_status_t *battery,
    const logger_storage_status_t *storage, logger_fault_code_t current_fault,
    uint32_t boot_counter, uint32_t now_ms, const char **error_code_out,
    const char **error_message_out);

bool logger_session_refresh_live(logger_session_state_t *session,
                                 const logger_clock_status_t *clock,
                                 uint32_t boot_counter, uint32_t now_ms);

bool logger_session_write_status_snapshot(
    logger_session_state_t *session, const logger_clock_status_t *clock,
    const logger_battery_status_t *battery,
    const logger_storage_status_t *storage, logger_fault_code_t current_fault,
    uint32_t boot_counter, uint32_t now_ms);

bool logger_session_write_marker(logger_session_state_t *session,
                                 const logger_clock_status_t *clock,
                                 uint32_t boot_counter, uint32_t now_ms);

bool logger_session_ensure_active_span(
    logger_session_state_t *session, logger_system_log_t *system_log,
    const char *hardware_id, const logger_persisted_state_t *persisted,
    const logger_clock_status_t *clock, const logger_storage_status_t *storage,
    const char *start_reason, const char *h10_address, bool encrypted,
    bool bonded, bool clock_jump_at_session_start, uint32_t boot_counter,
    uint32_t now_ms, const char **error_code_out,
    const char **error_message_out);

bool logger_session_append_ecg_packet(logger_session_state_t *session,
                                      const logger_clock_status_t *clock,
                                      uint64_t mono_us, const uint8_t *value,
                                      size_t value_len);

bool logger_session_append_pmd_packet(logger_session_state_t *session,
                                      const logger_clock_status_t *clock,
                                      uint16_t stream_kind, uint64_t mono_us,
                                      const uint8_t *value, size_t value_len);

/*
 * Check time-based chunk seal.  Call periodically from the main loop
 * (recommended: every ~1 s while a session is active and streaming).
 * Returns false on storage write failure.
 */
bool logger_session_seal_chunk_if_needed(logger_session_state_t *session,
                                         uint32_t now_ms);

bool logger_session_handle_disconnect(logger_session_state_t *session,
                                      const logger_clock_status_t *clock,
                                      uint32_t boot_counter, uint32_t now_ms,
                                      const char *gap_reason);

bool logger_session_handle_clock_event(logger_session_state_t *session,
                                       const logger_clock_status_t *clock,
                                       uint32_t boot_counter, uint32_t now_ms,
                                       const char *event_kind,
                                       const char *span_end_reason,
                                       int64_t delta_ns, int64_t old_utc_ns,
                                       int64_t new_utc_ns, bool split_span);

bool logger_session_append_h10_battery(logger_session_state_t *session,
                                       const logger_clock_status_t *clock,
                                       uint32_t boot_counter, uint32_t now_ms,
                                       uint8_t battery_percent,
                                       const char *read_reason);

bool logger_session_finalize(
    logger_session_state_t *session, logger_system_log_t *system_log,
    const char *hardware_id, const logger_persisted_state_t *persisted,
    const logger_clock_status_t *clock, const logger_storage_status_t *storage,
    const char *end_reason, uint32_t boot_counter, uint32_t now_ms);

bool logger_session_stop_debug(logger_session_state_t *session,
                               logger_system_log_t *system_log,
                               const char *hardware_id,
                               const logger_persisted_state_t *persisted,
                               const logger_clock_status_t *clock,
                               const logger_storage_status_t *storage,
                               uint32_t boot_counter, uint32_t now_ms);

bool logger_session_recover_on_boot(
    logger_session_state_t *session, logger_system_log_t *system_log,
    const char *hardware_id, const logger_persisted_state_t *persisted,
    const logger_clock_status_t *clock, const logger_storage_status_t *storage,
    uint32_t boot_counter, uint32_t now_ms, bool resume_allowed,
    bool *recovered_active_out, bool *closed_session_out);

#endif