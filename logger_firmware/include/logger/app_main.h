#ifndef LOGGER_FIRMWARE_APP_MAIN_H
#define LOGGER_FIRMWARE_APP_MAIN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "logger/app_state.h"
#include "logger/battery.h"
#include "logger/button.h"
#include "logger/capture_pipe.h"
#include "logger/capture_stats.h"
#include "logger/clock.h"
#include "logger/config_store.h"
#include "logger/faults.h"
#include "logger/h10.h"
#include "logger/identity.h"
#include "logger/queue.h"
#include "logger/service_cli.h"
#include "logger/session.h"
#include "logger/storage.h"
#include "logger/storage_worker.h"
#include "logger/system_log.h"

typedef enum {
  LOGGER_RECOVERY_NONE = 0,
  LOGGER_RECOVERY_CONFIG_INCOMPLETE,
  LOGGER_RECOVERY_LOW_BATTERY_BLOCKED_START,
  LOGGER_RECOVERY_CRITICAL_LOW_BATTERY_STOPPED,
  LOGGER_RECOVERY_SD_MISSING_OR_UNWRITABLE,
  LOGGER_RECOVERY_SD_LOW_SPACE_RESERVE_UNMET,
  LOGGER_RECOVERY_SD_WRITE_FAILED,
  LOGGER_RECOVERY_PSRAM_INIT_FAILED,
} logger_recovery_reason_t;

typedef enum {
  LOGGER_DEBUG_STORAGE_FAULT_NONE = 0,
  LOGGER_DEBUG_STORAGE_FAULT_MISSING,
  LOGGER_DEBUG_STORAGE_FAULT_LOW_SPACE,
  LOGGER_DEBUG_STORAGE_FAULT_WRITE_FAILED,
} logger_debug_storage_fault_t;

typedef enum {
  LOGGER_FAULT_CLEAR_NO_FAULT = 0,
  LOGGER_FAULT_CLEAR_CLEARED,
  LOGGER_FAULT_CLEAR_CONDITION_PRESENT,
  LOGGER_FAULT_CLEAR_NOT_CLEARABLE,
} logger_fault_clear_result_t;

static inline const char *
logger_debug_storage_fault_name(logger_debug_storage_fault_t fault) {
  switch (fault) {
  case LOGGER_DEBUG_STORAGE_FAULT_MISSING:
    return "sd_missing_or_unwritable";
  case LOGGER_DEBUG_STORAGE_FAULT_LOW_SPACE:
    return "sd_low_space_reserve_unmet";
  case LOGGER_DEBUG_STORAGE_FAULT_WRITE_FAILED:
    return "sd_write_failed";
  case LOGGER_DEBUG_STORAGE_FAULT_NONE:
  default:
    return NULL;
  }
}

static inline const char *
logger_recovery_reason_name(logger_recovery_reason_t reason) {
  switch (reason) {
  case LOGGER_RECOVERY_CONFIG_INCOMPLETE:
    return "config_incomplete";
  case LOGGER_RECOVERY_LOW_BATTERY_BLOCKED_START:
    return "low_battery_blocked_start";
  case LOGGER_RECOVERY_CRITICAL_LOW_BATTERY_STOPPED:
    return "critical_low_battery_stopped";
  case LOGGER_RECOVERY_SD_MISSING_OR_UNWRITABLE:
    return "sd_missing_or_unwritable";
  case LOGGER_RECOVERY_SD_LOW_SPACE_RESERVE_UNMET:
    return "sd_low_space_reserve_unmet";
  case LOGGER_RECOVERY_SD_WRITE_FAILED:
    return "sd_write_failed";
  case LOGGER_RECOVERY_PSRAM_INIT_FAILED:
    return "psram_init_failed";
  case LOGGER_RECOVERY_NONE:
  default:
    return NULL;
  }
}

typedef struct logger_app {
  logger_app_state_t runtime;
  logger_recovery_reason_t recovery_reason;
  logger_boot_gesture_t boot_gesture;
  logger_button_t button;
  logger_battery_status_t battery;
  logger_clock_status_t clock;
  logger_persisted_state_t persisted;
  logger_h10_state_t h10;
  logger_storage_status_t storage;
  logger_system_log_t system_log;
  logger_session_state_t session;
  logger_capture_stats_t capture_stats;
  capture_pipe_t capture_pipe;
  logger_service_cli_t cli;
  storage_worker_shared_t *storage_worker_shared;
  char hardware_id[LOGGER_HARDWARE_ID_HEX_LEN + 1];
  char current_day_study_day_local[11];
  char pending_day_study_day_local[11];
  char last_day_outcome_study_day_local[11];
  char last_day_outcome_kind[16];
  char last_day_outcome_reason[32];
  char recovery_last_action[32];
  char recovery_last_result[32];
  char next_span_start_reason[32];
  char stopping_end_reason[32];
  uint32_t last_observation_mono_ms;
  uint32_t last_session_live_flush_mono_ms;
  uint32_t last_session_snapshot_mono_ms;
  uint32_t last_chunk_seal_mono_ms;
  uint32_t day_seen_baseline;
  uint32_t day_connect_baseline;
  uint32_t day_ecg_start_baseline;
  uint32_t clock_valid_since_mono_ms;
  uint32_t last_clock_observation_mono_ms;
  uint32_t last_queue_maintenance_mono_ms;
  uint32_t last_upload_blocked_recheck_mono_ms;
  uint32_t deferred_boot_queue_refresh_after_mono_ms;
  uint32_t recovery_next_attempt_mono_ms;
  uint32_t recovery_probe_interval_ms;
  uint32_t recovery_good_since_mono_ms;
  uint32_t recovery_last_success_mono_ms;
  uint32_t recovery_attempt_count;
  uint32_t recovery_validation_success_count;
  uint32_t upload_next_attempt_mono_ms;
  int64_t last_trusted_clock_utc_ns;
  uint8_t upload_retry_backoff_index;
  uint8_t upload_pass_count;
  uint8_t upload_pass_next_index;
  char upload_pass_session_ids[LOGGER_UPLOAD_QUEUE_MAX_SESSIONS][33];
  bool upload_manual_off_charger;
  bool upload_ntp_attempted;
  bool upload_pass_had_success;
  bool service_pinned_by_user;
  bool debug_force_clock_invalid;
  logger_debug_storage_fault_t debug_storage_fault;
  bool idle_resume_on_unplug;
  bool day_tracking_initialized;
  bool current_day_has_session;
  bool last_day_outcome_valid;
  bool pending_next_session_clock_jump;
  bool last_clock_observation_available;
  bool last_trusted_clock_utc_available;
  bool last_clock_observation_valid;
  bool indicator_led_on;
  bool boot_banner_printed;
  bool boot_firmware_identity_changed;
  bool deferred_boot_queue_refresh_pending;
  bool deferred_boot_queue_refresh_skip_logged;
  bool boot_recovery_done;
  bool reboot_pending;
  logger_runtime_state_t deferred_boot_queue_refresh_skip_state;
  logger_runtime_state_t recovery_resume_state;
} logger_app_t;

void logger_app_clear_current_fault(logger_app_t *app, const char *source);
logger_fault_clear_result_t
logger_app_manual_clear_current_fault(logger_app_t *app, uint32_t now_ms,
                                      logger_fault_code_t *previous_code_out);
void logger_app_init(logger_app_t *app, uint32_t now_ms,
                     logger_boot_gesture_t boot_gesture);

/*
 * Run boot-time session recovery on core 0, BEFORE the storage worker
 * is launched.
 *
 * This is the ONLY remaining path where core 0 does direct SD/FatFS
 * session I/O.  It runs with session->pipe == NULL so all writer
 * commands execute inline on core 0.  The constraint is enforced by
 * main.c: this is called after logger_app_init() (pipe not set yet)
 * but before logger_session_set_pipe() + logger_storage_worker_launch().
 *
 * Returns true on success.  On failure, latches sd_write_failed and
 * returns false; the caller must NOT launch the worker in that case
 * (or can — the fault will be handled by step_boot routing to
 * RECOVERY_HOLD).
 */
bool logger_app_pre_worker_recovery(logger_app_t *app, uint32_t now_ms);
void logger_app_set_last_day_outcome(logger_app_t *app,
                                     const char *study_day_local,
                                     const char *kind, const char *reason);
void logger_app_note_wall_clock_changed(logger_app_t *app);
void logger_app_note_explicit_clock_valid(logger_app_t *app, uint32_t now_ms,
                                          const char *clear_source);
void logger_app_debug_force_clock_invalid(logger_app_t *app, uint32_t now_ms);
void logger_app_debug_clear_forced_clock_invalid(logger_app_t *app,
                                                 uint32_t now_ms,
                                                 const char *clear_source);
void logger_app_debug_force_storage_fault(logger_app_t *app,
                                          logger_debug_storage_fault_t fault,
                                          uint32_t now_ms);
void logger_app_debug_clear_forced_storage_fault(logger_app_t *app,
                                                 uint32_t now_ms);
bool logger_app_clock_sync_ntp(logger_app_t *app,
                               logger_clock_ntp_sync_result_t *result);
bool logger_app_request_service_mode(logger_app_t *app, uint32_t now_ms,
                                     bool *will_stop_logging_out);
void logger_app_step(logger_app_t *app, uint32_t now_ms);

/*
 * Returns the maximum safe sleep duration in milliseconds given the current
 * application state, power source, and pending deadlines.
 *
 * The main loop should call cyw43_arch_wait_for_work_until() with this value
 * instead of a fixed sleep_ms().  The actual sleep may be shorter if CYW43
 * async events (BLE notifications, WiFi callbacks) arrive before the deadline.
 *
 * Principles:
 *   - Active streaming (130 Hz ECG): 10 ms, same as the old fixed delay.
 *   - BLE active but idle (scan, connect, secure): 50–500 ms.
 *     CYW43 async events wake the CPU immediately on BLE data.
 *   - No BLE, on battery (recovery, idle): 1–5 s.  Tightened to the next
 *     recovery/upload deadline when one is pending.
 *   - On USB (service, upload): shorter caps for CLI responsiveness.
 */
uint32_t logger_app_max_sleep_ms(const logger_app_t *app, uint32_t now_ms);

#endif
