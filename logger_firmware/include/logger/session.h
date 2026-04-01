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

typedef struct {
    bool active;
    bool span_active;
    bool quarantined;
    bool quarantine_clock_invalid_at_start;
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
    uint64_t next_record_seq;
    uint64_t journal_size_bytes;
    uint32_t next_chunk_seq_in_session;
    uint32_t next_packet_seq_in_span;
    uint32_t span_count;
    logger_journal_span_summary_t spans[LOGGER_JOURNAL_MAX_SPANS];
} logger_session_state_t;

void logger_session_init(logger_session_state_t *session);

bool logger_session_start_debug(
    logger_session_state_t *session,
    logger_system_log_t *system_log,
    const char *hardware_id,
    const logger_persisted_state_t *persisted,
    const logger_clock_status_t *clock,
    const logger_battery_status_t *battery,
    const logger_storage_status_t *storage,
    logger_fault_code_t current_fault,
    uint32_t boot_counter,
    uint32_t now_ms,
    const char **error_code_out,
    const char **error_message_out);

bool logger_session_refresh_live(
    logger_session_state_t *session,
    const logger_clock_status_t *clock,
    uint32_t boot_counter,
    uint32_t now_ms);

bool logger_session_write_status_snapshot(
    logger_session_state_t *session,
    const logger_clock_status_t *clock,
    const logger_battery_status_t *battery,
    const logger_storage_status_t *storage,
    logger_fault_code_t current_fault,
    uint32_t boot_counter,
    uint32_t now_ms);

bool logger_session_write_marker(
    logger_session_state_t *session,
    const logger_clock_status_t *clock,
    uint32_t boot_counter,
    uint32_t now_ms);

bool logger_session_ensure_active_span(
    logger_session_state_t *session,
    logger_system_log_t *system_log,
    const char *hardware_id,
    const logger_persisted_state_t *persisted,
    const logger_clock_status_t *clock,
    const logger_storage_status_t *storage,
    const char *start_reason,
    const char *h10_address,
    bool encrypted,
    bool bonded,
    uint32_t boot_counter,
    uint32_t now_ms,
    const char **error_code_out,
    const char **error_message_out);

bool logger_session_append_ecg_packet(
    logger_session_state_t *session,
    const logger_clock_status_t *clock,
    uint64_t mono_us,
    const uint8_t *value,
    size_t value_len);

bool logger_session_handle_disconnect(
    logger_session_state_t *session,
    const logger_clock_status_t *clock,
    uint32_t boot_counter,
    uint32_t now_ms,
    const char *gap_reason);

bool logger_session_finalize(
    logger_session_state_t *session,
    logger_system_log_t *system_log,
    const char *hardware_id,
    const logger_persisted_state_t *persisted,
    const logger_clock_status_t *clock,
    const logger_storage_status_t *storage,
    const char *end_reason,
    uint32_t boot_counter,
    uint32_t now_ms);

bool logger_session_stop_debug(
    logger_session_state_t *session,
    logger_system_log_t *system_log,
    const char *hardware_id,
    const logger_persisted_state_t *persisted,
    const logger_clock_status_t *clock,
    const logger_storage_status_t *storage,
    uint32_t boot_counter,
    uint32_t now_ms);

bool logger_session_recover_on_boot(
    logger_session_state_t *session,
    logger_system_log_t *system_log,
    const char *hardware_id,
    const logger_persisted_state_t *persisted,
    const logger_clock_status_t *clock,
    const logger_storage_status_t *storage,
    uint32_t boot_counter,
    uint32_t now_ms,
    bool resume_allowed,
    bool *recovered_active_out,
    bool *closed_session_out);

#endif