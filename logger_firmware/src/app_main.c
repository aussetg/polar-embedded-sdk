#include "logger/app_main.h"

#include "logger/faults.h"
#include "logger/psram.h"
#include "logger/psram_layout.h"
#include "logger/storage_worker.h"
#include "logger/system_log_backend.h"
#include "logger/system_log_backend_psram.h"

#include "hardware/sync.h"
#include "pico/stdlib.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "hardware/watchdog.h"
#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"

#include "board_config.h"
#include "logger/json_writer.h"
#include "logger/net.h"
#include "logger/queue.h"
#include "logger/storage_service.h"
#include "logger/upload.h"
#include "logger/util.h"
#include "logger/version.h"

#define LOGGER_QUEUE_MAINTENANCE_INTERVAL_MS 300000u
#define LOGGER_RECOVERY_POWER_PROBE_INTERVAL_MS 5000u
#define LOGGER_RECOVERY_STORAGE_PROBE_INTERVAL_MS 10000u
#define LOGGER_RECOVERY_STORAGE_WRITE_PROBE_INTERVAL_MS 15000u
#define LOGGER_RECOVERY_STORAGE_WRITE_PROBE_INTERVAL_MAX_MS 60000u
#define LOGGER_RECOVERY_STORAGE_CLEAR_SUCCESS_GAP_MS 1000u
#define LOGGER_RECOVERY_CLOCK_CLEAR_DWELL_MS 5000u
#define LOGGER_RECOVERY_BATTERY_CLEAR_DWELL_MS 30000u
#define LOGGER_RECOVERY_USB_CLEAR_DWELL_MS 5000u
#define LOGGER_RECOVERY_LOW_START_CLEAR_MV 3750u
#define LOGGER_RECOVERY_CRITICAL_CLEAR_MV 3700u
#define LOGGER_BOOT_QUEUE_REFRESH_DEFER_MS 5000u
#define LOGGER_UPLOAD_BLOCKED_RECHECK_INTERVAL_MS 60000u

static bool logger_app_try_finalize_no_session_day(logger_app_t *app,
                                                   uint32_t now_ms);
static bool logger_app_should_enter_overnight_idle(const logger_app_t *app);
static void
logger_app_transition_via_stopping(logger_app_t *app,
                                   logger_runtime_state_t next_state,
                                   const char *reason, uint32_t now_ms);
static void logger_app_begin_upload_flow(logger_app_t *app,
                                         bool manual_off_charger);
static bool logger_app_run_queue_maintenance(logger_app_t *app, uint32_t now_ms,
                                             bool force);
static void logger_app_reconcile_upload_blocked_fault(logger_app_t *app,
                                                      uint32_t now_ms,
                                                      bool force);
static bool logger_app_state_allows_deferred_boot_queue_refresh(
    logger_runtime_state_t state);
static void logger_app_schedule_deferred_boot_queue_refresh(
    logger_app_t *app, uint32_t now_ms);
static void logger_app_maybe_run_deferred_boot_queue_refresh(
    logger_app_t *app, uint32_t now_ms);

static int64_t logger_app_i64_abs(int64_t value) {
  return value < 0 ? -(value + 1) + 1 : value;
}

static bool logger_app_boot_identity_matches_persisted(
    const logger_persisted_state_t *state) {
  return strcmp(state->last_boot_firmware_version, LOGGER_FIRMWARE_VERSION) ==
             0 &&
         strcmp(state->last_boot_build_id, LOGGER_BUILD_ID) == 0;
}

static void
logger_app_set_persisted_boot_identity(logger_persisted_state_t *state) {
  logger_copy_string(state->last_boot_firmware_version,
                     sizeof(state->last_boot_firmware_version),
                     LOGGER_FIRMWARE_VERSION);
  logger_copy_string(state->last_boot_build_id,
                     sizeof(state->last_boot_build_id), LOGGER_BUILD_ID);
}

static void logger_print_boot_banner(const logger_app_t *app) {
  printf("[logger] appliance firmware\n");
  printf("[logger] board_profile=%s\n", LOGGER_BOARD_PROFILE);
  printf("[logger] board_name=%s\n", LOGGER_BOARD_NAME);
  printf("[logger] hardware_id=%s\n", app->hardware_id);
  printf("[logger] boot_counter=%lu\n",
         (unsigned long)app->persisted.boot_counter);
  printf("[logger] rtc=i2c%u sda=GP%u scl=GP%u addr=0x%02x\n",
         (unsigned)LOGGER_RTC_I2C_BUS, (unsigned)LOGGER_RTC_SDA_PIN,
         (unsigned)LOGGER_RTC_SCL_PIN, (unsigned)LOGGER_RTC_I2C_ADDR);
  printf("[logger] sd=spi%u miso=GP%u cs=GP%u sck=GP%u mosi=GP%u detect=GP%u "
         "optional=%u\n",
         (unsigned)LOGGER_SD_SPI_BUS, (unsigned)LOGGER_SD_MISO_PIN,
         (unsigned)LOGGER_SD_CS_PIN, (unsigned)LOGGER_SD_SCK_PIN,
         (unsigned)LOGGER_SD_MOSI_PIN, (unsigned)LOGGER_SD_DETECT_PIN,
         (unsigned)LOGGER_SD_DETECT_OPTIONAL);
  printf("[logger] policy rollover=%02u:00 upload_window=%02u:00-%02u:00 "
         "reserve=%luB\n",
         (unsigned)LOGGER_STUDY_DAY_ROLLOVER_HOUR_LOCAL,
         (unsigned)LOGGER_OVERNIGHT_UPLOAD_WINDOW_START_HOUR_LOCAL,
         (unsigned)LOGGER_OVERNIGHT_UPLOAD_WINDOW_END_HOUR_LOCAL,
         (unsigned long)LOGGER_SD_MIN_FREE_RESERVE_BYTES);
  printf("[logger] battery critical=%umV start_block=%umV "
         "off_charger_upload=%umV\n",
         (unsigned)LOGGER_BATTERY_CRITICAL_STOP_MV,
         (unsigned)LOGGER_BATTERY_LOW_START_BLOCK_MV,
         (unsigned)LOGGER_BATTERY_OFF_CHARGER_UPLOAD_MIN_MV);
}

static const char *logger_app_boot_gesture_name(logger_boot_gesture_t gesture) {
  switch (gesture) {
  case LOGGER_BOOT_GESTURE_SERVICE:
    return "service";
  case LOGGER_BOOT_GESTURE_FACTORY_RESET:
    return "factory_reset";
  case LOGGER_BOOT_GESTURE_NONE:
  default:
    return "none";
  }
}

static void logger_app_refresh_observations(logger_app_t *app,
                                            uint32_t now_ms) {
  if (app->last_observation_mono_ms != 0u &&
      (now_ms - app->last_observation_mono_ms) < 1000u) {
    return;
  }

  logger_battery_sample(&app->battery);
  logger_clock_sample(&app->clock);
  if (app->debug_force_clock_invalid) {
    app->clock.valid = false;
    app->clock.now_utc[0] = '\0';
  }
  (void)logger_storage_svc_refresh(&app->storage);
  switch (app->debug_storage_fault) {
  case LOGGER_DEBUG_STORAGE_FAULT_MISSING:
    app->storage.card_present = false;
    app->storage.card_initialized = false;
    app->storage.mounted = false;
    app->storage.writable = false;
    app->storage.logger_root_ready = false;
    app->storage.reserve_ok = false;
    app->storage.filesystem[0] = '\0';
    break;
  case LOGGER_DEBUG_STORAGE_FAULT_LOW_SPACE:
    app->storage.reserve_ok = false;
    if (app->storage.mounted &&
        app->storage.free_bytes >= LOGGER_SD_MIN_FREE_RESERVE_BYTES) {
      app->storage.free_bytes = LOGGER_SD_MIN_FREE_RESERVE_BYTES - 1u;
    }
    break;
  case LOGGER_DEBUG_STORAGE_FAULT_WRITE_FAILED:
  case LOGGER_DEBUG_STORAGE_FAULT_NONE:
  default:
    break;
  }
  (void)logger_h10_set_bound_address(&app->h10,
                                     app->persisted.config.bound_h10_address);

  app->runtime.charger_present = app->battery.vbus_present;
  app->runtime.wall_clock_valid = app->clock.valid;
  app->runtime.provisioning_complete =
      logger_config_normal_logging_ready(&app->persisted.config);
  app->last_observation_mono_ms = now_ms;
}

static bool logger_app_recover_session_if_needed(logger_app_t *app,
                                                 uint32_t now_ms,
                                                 bool resume_allowed) {
  bool recovered_active = false;
  bool closed_session = false;
  if (!logger_session_recover_on_boot(
          &app->session, &app->system_log, app->hardware_id, &app->persisted,
          &app->clock, &app->storage, app->persisted.boot_counter, now_ms,
          resume_allowed, &recovered_active, &closed_session)) {
    return false;
  }
  if (recovered_active) {
    app->last_session_live_flush_mono_ms = now_ms;
    app->last_session_snapshot_mono_ms = now_ms;
    app->last_chunk_seal_mono_ms = now_ms;
  }
  if (closed_session) {
    app->last_session_live_flush_mono_ms = 0u;
    app->last_session_snapshot_mono_ms = 0u;
    app->last_chunk_seal_mono_ms = 0u;
  }
  return true;
}

static uint8_t logger_app_fault_precedence(logger_fault_code_t code) {
  switch (code) {
  case LOGGER_FAULT_PSRAM_INIT_FAILED:
    return 0u;
  case LOGGER_FAULT_SD_WRITE_FAILED:
    return 1u;
  case LOGGER_FAULT_SD_MISSING_OR_UNWRITABLE:
    return 2u;
  case LOGGER_FAULT_SD_LOW_SPACE_RESERVE_UNMET:
    return 3u;
  case LOGGER_FAULT_CRITICAL_LOW_BATTERY_STOPPED:
    return 4u;
  case LOGGER_FAULT_LOW_BATTERY_BLOCKED_START:
    return 5u;
  case LOGGER_FAULT_CONFIG_INCOMPLETE:
    return 6u;
  case LOGGER_FAULT_UPLOAD_BLOCKED_MIN_FIRMWARE:
    return 7u;
  case LOGGER_FAULT_CLOCK_INVALID:
    return 8u;
  case LOGGER_FAULT_NONE:
  default:
    return 255u;
  }
}

static void logger_app_maybe_latch_new_fault(logger_app_t *app,
                                             logger_fault_code_t code) {
  const logger_fault_code_t current = app->persisted.current_fault_code;
  if (code == LOGGER_FAULT_NONE || current == code) {
    return;
  }
  if (current != LOGGER_FAULT_NONE &&
      logger_app_fault_precedence(code) >=
          logger_app_fault_precedence(current)) {
    return;
  }

  app->persisted.current_fault_code = code;
  (void)logger_config_store_save(&app->persisted);

  char details[LOGGER_SYSTEM_LOG_DETAILS_JSON_MAX + 1];
  logger_json_object_writer_t writer;
  logger_json_object_writer_init(&writer, details, sizeof(details));
  if (!logger_json_object_writer_string_field(&writer, "code",
                                              logger_fault_code_name(code)) ||
      !logger_json_object_writer_finish(&writer)) {
    return;
  }
  (void)logger_system_log_append(
      &app->system_log, logger_clock_now_utc_or_null(&app->clock),
      "fault_latched", LOGGER_SYSTEM_LOG_SEVERITY_WARN,
      logger_json_object_writer_data(&writer));
}

void logger_app_clear_current_fault(logger_app_t *app, const char *source) {
  const logger_fault_code_t previous = app->persisted.current_fault_code;
  if (previous == LOGGER_FAULT_NONE) {
    return;
  }

  app->persisted.last_cleared_fault_code = previous;
  app->persisted.current_fault_code = LOGGER_FAULT_NONE;
  (void)logger_config_store_save(&app->persisted);

  char details[LOGGER_SYSTEM_LOG_DETAILS_JSON_MAX + 1];
  logger_json_object_writer_t writer;
  logger_json_object_writer_init(&writer, details, sizeof(details));
  if (!logger_json_object_writer_string_field(
          &writer, "code", logger_fault_code_name(previous)) ||
      !logger_json_object_writer_string_field(
          &writer, "source", source != NULL ? source : "auto") ||
      !logger_json_object_writer_finish(&writer)) {
    return;
  }
  (void)logger_system_log_append(
      &app->system_log, logger_clock_now_utc_or_null(&app->clock),
      "fault_cleared", LOGGER_SYSTEM_LOG_SEVERITY_INFO,
      logger_json_object_writer_data(&writer));
}

static bool logger_app_local_time_evaluable(const logger_app_t *app) {
  return app->clock.valid &&
         logger_timezone_is_utc_like(app->persisted.config.timezone);
}

static bool logger_app_should_enter_overnight_idle(const logger_app_t *app) {
  if (!app->battery.vbus_present) {
    return false;
  }
  if (!logger_app_local_time_evaluable(app)) {
    return false;
  }
  return app->clock.hour >= LOGGER_OVERNIGHT_UPLOAD_WINDOW_START_HOUR_LOCAL ||
         app->clock.hour < LOGGER_OVERNIGHT_UPLOAD_WINDOW_END_HOUR_LOCAL;
}

static logger_recovery_reason_t
logger_app_recovery_reason_from_fault(logger_fault_code_t code) {
  switch (code) {
  case LOGGER_FAULT_CONFIG_INCOMPLETE:
    return LOGGER_RECOVERY_CONFIG_INCOMPLETE;
  case LOGGER_FAULT_LOW_BATTERY_BLOCKED_START:
    return LOGGER_RECOVERY_LOW_BATTERY_BLOCKED_START;
  case LOGGER_FAULT_CRITICAL_LOW_BATTERY_STOPPED:
    return LOGGER_RECOVERY_CRITICAL_LOW_BATTERY_STOPPED;
  case LOGGER_FAULT_SD_MISSING_OR_UNWRITABLE:
    return LOGGER_RECOVERY_SD_MISSING_OR_UNWRITABLE;
  case LOGGER_FAULT_SD_LOW_SPACE_RESERVE_UNMET:
    return LOGGER_RECOVERY_SD_LOW_SPACE_RESERVE_UNMET;
  case LOGGER_FAULT_SD_WRITE_FAILED:
    return LOGGER_RECOVERY_SD_WRITE_FAILED;
  case LOGGER_FAULT_PSRAM_INIT_FAILED:
    return LOGGER_RECOVERY_PSRAM_INIT_FAILED;
  case LOGGER_FAULT_NONE:
  case LOGGER_FAULT_CLOCK_INVALID:
  case LOGGER_FAULT_UPLOAD_BLOCKED_MIN_FIRMWARE:
  default:
    return LOGGER_RECOVERY_NONE;
  }
}

static logger_fault_code_t
logger_app_fault_from_recovery_reason(logger_recovery_reason_t reason) {
  switch (reason) {
  case LOGGER_RECOVERY_CONFIG_INCOMPLETE:
    return LOGGER_FAULT_CONFIG_INCOMPLETE;
  case LOGGER_RECOVERY_LOW_BATTERY_BLOCKED_START:
    return LOGGER_FAULT_LOW_BATTERY_BLOCKED_START;
  case LOGGER_RECOVERY_CRITICAL_LOW_BATTERY_STOPPED:
    return LOGGER_FAULT_CRITICAL_LOW_BATTERY_STOPPED;
  case LOGGER_RECOVERY_SD_MISSING_OR_UNWRITABLE:
    return LOGGER_FAULT_SD_MISSING_OR_UNWRITABLE;
  case LOGGER_RECOVERY_SD_LOW_SPACE_RESERVE_UNMET:
    return LOGGER_FAULT_SD_LOW_SPACE_RESERVE_UNMET;
  case LOGGER_RECOVERY_SD_WRITE_FAILED:
    return LOGGER_FAULT_SD_WRITE_FAILED;
  case LOGGER_RECOVERY_PSRAM_INIT_FAILED:
    return LOGGER_FAULT_PSRAM_INIT_FAILED;
  case LOGGER_RECOVERY_NONE:
  default:
    return LOGGER_FAULT_NONE;
  }
}

static uint32_t
logger_app_recovery_initial_probe_interval_ms(logger_recovery_reason_t reason) {
  switch (reason) {
  case LOGGER_RECOVERY_LOW_BATTERY_BLOCKED_START:
  case LOGGER_RECOVERY_CRITICAL_LOW_BATTERY_STOPPED:
    return LOGGER_RECOVERY_POWER_PROBE_INTERVAL_MS;
  case LOGGER_RECOVERY_SD_MISSING_OR_UNWRITABLE:
  case LOGGER_RECOVERY_SD_LOW_SPACE_RESERVE_UNMET:
    return LOGGER_RECOVERY_STORAGE_PROBE_INTERVAL_MS;
  case LOGGER_RECOVERY_SD_WRITE_FAILED:
    return LOGGER_RECOVERY_STORAGE_WRITE_PROBE_INTERVAL_MS;
  case LOGGER_RECOVERY_PSRAM_INIT_FAILED:
    return 0u; /* irrevocable hardware fault */
  case LOGGER_RECOVERY_CONFIG_INCOMPLETE:
  case LOGGER_RECOVERY_NONE:
  default:
    return 0u;
  }
}

static void logger_app_recovery_set_status(logger_app_t *app,
                                           const char *action,
                                           const char *result) {
  logger_copy_string(app->recovery_last_action,
                     sizeof(app->recovery_last_action), action);
  logger_copy_string(app->recovery_last_result,
                     sizeof(app->recovery_last_result), result);
}

static void logger_app_reset_recovery_state(logger_app_t *app) {
  app->recovery_reason = LOGGER_RECOVERY_NONE;
  app->recovery_resume_state = LOGGER_RUNTIME_BOOT;
  app->recovery_attempt_count = 0u;
  app->recovery_next_attempt_mono_ms = 0u;
  app->recovery_probe_interval_ms = 0u;
  app->recovery_good_since_mono_ms = 0u;
  app->recovery_last_success_mono_ms = 0u;
  app->recovery_validation_success_count = 0u;
  app->recovery_last_action[0] = '\0';
  app->recovery_last_result[0] = '\0';
}

static void logger_app_enter_service(logger_app_t *app, const char *reason,
                                     uint32_t now_ms, bool pinned) {
  logger_app_reset_recovery_state(app);
  app->service_pinned_by_user = pinned;
  logger_app_state_transition(&app->runtime, LOGGER_RUNTIME_SERVICE, reason,
                              now_ms);
}

static void logger_app_prepare_recovery_hold(
    logger_app_t *app, logger_fault_code_t fault_code,
    logger_runtime_state_t resume_state, uint32_t now_ms) {
  const logger_recovery_reason_t reason =
      logger_app_recovery_reason_from_fault(fault_code);
  if (reason == LOGGER_RECOVERY_NONE) {
    return;
  }
  if (app->runtime.current_state != LOGGER_RUNTIME_RECOVERY_HOLD ||
      app->recovery_reason != reason) {
    logger_app_reset_recovery_state(app);
    app->recovery_reason = reason;
    app->recovery_resume_state = resume_state;
    app->recovery_probe_interval_ms =
        logger_app_recovery_initial_probe_interval_ms(reason);
    app->recovery_next_attempt_mono_ms =
        app->recovery_probe_interval_ms == 0u
            ? 0u
            : now_ms + app->recovery_probe_interval_ms;
    logger_app_recovery_set_status(app, "enter_recovery_hold", "pending");
  } else if (app->recovery_resume_state == LOGGER_RUNTIME_BOOT &&
             resume_state != LOGGER_RUNTIME_BOOT) {
    app->recovery_resume_state = resume_state;
  }
  app->service_pinned_by_user = false;
}

static void logger_app_route_blocking_fault(logger_app_t *app,
                                            logger_fault_code_t fault_code,
                                            logger_runtime_state_t resume_state,
                                            const char *reason,
                                            uint32_t now_ms) {
  if (fault_code == LOGGER_FAULT_NONE) {
    return;
  }

  logger_app_maybe_latch_new_fault(app, fault_code);
  logger_app_prepare_recovery_hold(app, fault_code, resume_state, now_ms);

  if (app->runtime.current_state == LOGGER_RUNTIME_LOG_STOPPING) {
    logger_app_state_transition(&app->runtime, LOGGER_RUNTIME_RECOVERY_HOLD,
                                reason, now_ms);
    return;
  }

  if (logger_runtime_state_is_logging(app->runtime.current_state) &&
      app->session.active) {
    logger_app_transition_via_stopping(app, LOGGER_RUNTIME_RECOVERY_HOLD,
                                       reason, now_ms);
    return;
  }

  logger_app_state_transition(&app->runtime, LOGGER_RUNTIME_RECOVERY_HOLD,
                              reason, now_ms);
}

static logger_runtime_state_t
logger_app_select_unattended_target(const logger_app_t *app,
                                    logger_fault_code_t *fault_code_out) {
  logger_fault_code_t fault_code = LOGGER_FAULT_NONE;

  /* Hardware fault: PSRAM is required for capture.  Irrevocable. */
  if (app->persisted.current_fault_code == LOGGER_FAULT_PSRAM_INIT_FAILED) {
    fault_code = LOGGER_FAULT_PSRAM_INIT_FAILED;
  }

  if (app->persisted.current_fault_code == LOGGER_FAULT_SD_WRITE_FAILED) {
    fault_code = LOGGER_FAULT_SD_WRITE_FAILED;
  }

  const logger_fault_code_t storage_fault =
      logger_fault_from_storage(&app->storage);
  if (storage_fault != LOGGER_FAULT_NONE &&
      logger_app_fault_precedence(storage_fault) <
          logger_app_fault_precedence(fault_code)) {
    fault_code = storage_fault;
  }

  if (fault_code == LOGGER_FAULT_NONE &&
      (app->persisted.current_fault_code ==
           LOGGER_FAULT_SD_MISSING_OR_UNWRITABLE ||
       app->persisted.current_fault_code ==
           LOGGER_FAULT_SD_LOW_SPACE_RESERVE_UNMET)) {
    fault_code = app->persisted.current_fault_code;
  }

  if (fault_code == LOGGER_FAULT_NONE && !app->runtime.provisioning_complete) {
    fault_code = LOGGER_FAULT_CONFIG_INCOMPLETE;
    if (fault_code_out != NULL) {
      *fault_code_out = fault_code;
    }
    return app->battery.vbus_present ? LOGGER_RUNTIME_SERVICE
                                     : LOGGER_RUNTIME_RECOVERY_HOLD;
  }

  if (fault_code == LOGGER_FAULT_NONE &&
      app->persisted.current_fault_code == LOGGER_FAULT_CONFIG_INCOMPLETE) {
    fault_code = LOGGER_FAULT_CONFIG_INCOMPLETE;
  }

  if (fault_code == LOGGER_FAULT_NONE &&
      app->persisted.current_fault_code ==
          LOGGER_FAULT_CRITICAL_LOW_BATTERY_STOPPED) {
    fault_code = LOGGER_FAULT_CRITICAL_LOW_BATTERY_STOPPED;
  }

  if (fault_code == LOGGER_FAULT_NONE &&
      app->persisted.current_fault_code ==
          LOGGER_FAULT_LOW_BATTERY_BLOCKED_START) {
    fault_code = LOGGER_FAULT_LOW_BATTERY_BLOCKED_START;
  }

  if (fault_code == LOGGER_FAULT_NONE &&
      logger_battery_low_start_blocked(&app->battery)) {
    fault_code = LOGGER_FAULT_LOW_BATTERY_BLOCKED_START;
  }

  if (fault_code_out != NULL) {
    *fault_code_out = fault_code;
  }
  if (fault_code != LOGGER_FAULT_NONE) {
    return LOGGER_RUNTIME_RECOVERY_HOLD;
  }
  if (logger_app_should_enter_overnight_idle(app)) {
    return LOGGER_RUNTIME_UPLOAD_PREP;
  }
  return LOGGER_RUNTIME_LOG_WAIT_H10;
}

static void logger_app_apply_unattended_target(logger_app_t *app,
                                               const char *reason,
                                               uint32_t now_ms) {
  logger_fault_code_t fault_code = LOGGER_FAULT_NONE;
  const logger_runtime_state_t target =
      logger_app_select_unattended_target(app, &fault_code);

  if (target == LOGGER_RUNTIME_SERVICE) {
    logger_app_maybe_latch_new_fault(app, fault_code);
    logger_app_enter_service(app, reason, now_ms, true);
    return;
  }
  if (target == LOGGER_RUNTIME_RECOVERY_HOLD) {
    logger_app_route_blocking_fault(app, fault_code, LOGGER_RUNTIME_BOOT,
                                    reason, now_ms);
    return;
  }

  app->service_pinned_by_user = false;
  logger_app_reset_recovery_state(app);
  if (target == LOGGER_RUNTIME_UPLOAD_PREP) {
    logger_app_begin_upload_flow(app, false);
  }
  logger_app_state_transition(&app->runtime, target, reason, now_ms);
}

static void logger_app_reconcile_clock_invalid_fault(logger_app_t *app,
                                                     uint32_t now_ms) {
  if (!app->clock.valid) {
    app->clock_valid_since_mono_ms = 0u;
    logger_app_maybe_latch_new_fault(app, LOGGER_FAULT_CLOCK_INVALID);
    return;
  }

  if (app->clock_valid_since_mono_ms == 0u) {
    app->clock_valid_since_mono_ms = now_ms;
  }
  if (app->persisted.current_fault_code == LOGGER_FAULT_CLOCK_INVALID &&
      (now_ms - app->clock_valid_since_mono_ms) >=
          LOGGER_RECOVERY_CLOCK_CLEAR_DWELL_MS) {
    logger_app_clear_current_fault(app, "clock_valid");
  }
}

static logger_fault_code_t
logger_app_fault_from_debug_storage_fault(logger_debug_storage_fault_t fault) {
  switch (fault) {
  case LOGGER_DEBUG_STORAGE_FAULT_MISSING:
    return LOGGER_FAULT_SD_MISSING_OR_UNWRITABLE;
  case LOGGER_DEBUG_STORAGE_FAULT_LOW_SPACE:
    return LOGGER_FAULT_SD_LOW_SPACE_RESERVE_UNMET;
  case LOGGER_DEBUG_STORAGE_FAULT_WRITE_FAILED:
    return LOGGER_FAULT_SD_WRITE_FAILED;
  case LOGGER_DEBUG_STORAGE_FAULT_NONE:
  default:
    return LOGGER_FAULT_NONE;
  }
}

static const char *
logger_app_debug_storage_fault_reason(logger_debug_storage_fault_t fault) {
  switch (fault) {
  case LOGGER_DEBUG_STORAGE_FAULT_MISSING:
    return "debug_storage_missing";
  case LOGGER_DEBUG_STORAGE_FAULT_LOW_SPACE:
    return "debug_storage_low_space";
  case LOGGER_DEBUG_STORAGE_FAULT_WRITE_FAILED:
    return "debug_storage_write_failed";
  case LOGGER_DEBUG_STORAGE_FAULT_NONE:
  default:
    return "debug_storage_clear";
  }
}

static bool logger_app_storage_self_test(const logger_app_t *app) {
  if (app != NULL &&
      app->debug_storage_fault == LOGGER_DEBUG_STORAGE_FAULT_WRITE_FAILED) {
    return false;
  }
  return logger_storage_svc_self_test();
}

static bool logger_app_validate_storage_missing_recovery(logger_app_t *app,
                                                         uint32_t now_ms) {
  app->last_observation_mono_ms = 0u;
  logger_app_refresh_observations(app, now_ms);
  if (!logger_storage_ready_for_logging(&app->storage)) {
    logger_app_recovery_set_status(app, "storage_validate", "blocked");
    return false;
  }
  if (!logger_app_storage_self_test(app)) {
    logger_app_recovery_set_status(app, "storage_self_test", "failed");
    return false;
  }
  logger_app_recovery_set_status(app, "storage_self_test", "passed");
  return true;
}

static bool logger_app_validate_storage_low_space_recovery(logger_app_t *app,
                                                           uint32_t now_ms) {
  if (!logger_app_run_queue_maintenance(app, now_ms, true)) {
    logger_app_recovery_set_status(app, "storage_prune_refresh", "failed");
    return false;
  }
  app->last_observation_mono_ms = 0u;
  logger_app_refresh_observations(app, now_ms);
  if (!logger_storage_ready_for_logging(&app->storage)) {
    logger_app_recovery_set_status(app, "storage_prune_refresh", "blocked");
    return false;
  }
  logger_app_recovery_set_status(app, "storage_prune_refresh", "passed");
  return true;
}

static bool logger_app_validate_storage_write_recovery(logger_app_t *app,
                                                       uint32_t now_ms) {
  logger_upload_queue_t *queue = NULL;

  app->last_observation_mono_ms = 0u;
  logger_app_refresh_observations(app, now_ms);
  if (!logger_storage_ready_for_logging(&app->storage)) {
    logger_app_recovery_set_status(app, "storage_validate", "blocked");
    return false;
  }
  if (!logger_app_storage_self_test(app)) {
    logger_app_recovery_set_status(app, "storage_self_test", "failed");
    return false;
  }
  queue = logger_upload_queue_tmp_acquire();
  if (!logger_storage_svc_queue_refresh(
          &app->system_log, logger_clock_now_utc_or_null(&app->clock), NULL) ||
      !logger_storage_svc_queue_load(queue)) {
    logger_upload_queue_tmp_release(queue);
    logger_app_recovery_set_status(app, "queue_refresh", "failed");
    return false;
  }
  logger_upload_queue_tmp_release(queue);
  logger_app_recovery_set_status(app, "queue_refresh", "passed");
  return true;
}

static void
logger_app_transition_via_stopping(logger_app_t *app,
                                   logger_runtime_state_t next_state,
                                   const char *reason, uint32_t now_ms) {
  logger_app_state_transition(&app->runtime, LOGGER_RUNTIME_LOG_STOPPING,
                              reason, now_ms);
  app->runtime.planned_next_state = next_state;
}

static bool logger_app_deadline_reached(uint32_t now_ms, uint32_t deadline_ms) {
  return (int32_t)(now_ms - deadline_ms) >= 0;
}

static void logger_app_reset_upload_pass(logger_app_t *app) {
  app->upload_pass_count = 0u;
  app->upload_pass_next_index = 0u;
  app->upload_pass_had_success = false;
}

static void logger_app_begin_upload_flow(logger_app_t *app,
                                         bool manual_off_charger) {
  app->upload_manual_off_charger = manual_off_charger;
  app->upload_ntp_attempted = false;
  app->upload_next_attempt_mono_ms = 0u;
  app->upload_retry_backoff_index = 0u;
  logger_app_reset_upload_pass(app);
}

static void logger_app_begin_upload_after_stop(logger_app_t *app,
                                               bool manual_off_charger,
                                               const char *reason,
                                               uint32_t now_ms) {
  logger_app_begin_upload_flow(app, manual_off_charger);
  logger_app_transition_via_stopping(app, LOGGER_RUNTIME_UPLOAD_PREP, reason,
                                     now_ms);
}

static void logger_app_transition_idle_waiting_for_charger(
    logger_app_t *app, bool manual_off_charger, const char *reason,
    uint32_t now_ms) {
  app->upload_manual_off_charger = manual_off_charger;
  app->upload_next_attempt_mono_ms = 0u;
  app->upload_retry_backoff_index = 0u;
  app->idle_resume_on_unplug = false;
  logger_app_reset_upload_pass(app);
  logger_app_state_transition(
      &app->runtime, LOGGER_RUNTIME_IDLE_WAITING_FOR_CHARGER, reason, now_ms);
}

static void logger_app_transition_idle_upload_complete(logger_app_t *app,
                                                       bool resume_on_unplug,
                                                       const char *reason,
                                                       uint32_t now_ms) {
  app->upload_next_attempt_mono_ms = 0u;
  app->upload_retry_backoff_index = 0u;
  app->idle_resume_on_unplug = resume_on_unplug;
  logger_app_reset_upload_pass(app);
  logger_app_state_transition(
      &app->runtime, LOGGER_RUNTIME_IDLE_UPLOAD_COMPLETE, reason, now_ms);
}

static bool
logger_app_prepare_upload_pass(logger_app_t *app,
                               logger_upload_queue_summary_t *summary_out) {
  logger_upload_queue_t *queue = logger_upload_queue_tmp_acquire();
  logger_upload_queue_summary_init(summary_out);
  app->upload_pass_count = 0u;
  app->upload_pass_next_index = 0u;

  if (!logger_storage_svc_queue_load(queue)) {
    logger_upload_queue_tmp_release(queue);
    return false;
  }
  logger_upload_queue_compute_summary(queue, summary_out);
  if (summary_out->blocked_count == 0u &&
      app->persisted.current_fault_code ==
          LOGGER_FAULT_UPLOAD_BLOCKED_MIN_FIRMWARE) {
    logger_app_clear_current_fault(app, "upload_queue_unblocked");
  }

  for (size_t i = 0u; i < queue->session_count; ++i) {
    const logger_upload_queue_entry_t *entry = &queue->sessions[i];
    if (strcmp(entry->status, "pending") != 0 &&
        strcmp(entry->status, "failed") != 0) {
      continue;
    }
    if (app->upload_pass_count >= LOGGER_UPLOAD_QUEUE_MAX_SESSIONS) {
      break;
    }
    memcpy(app->upload_pass_session_ids[app->upload_pass_count],
           entry->session_id,
           sizeof(app->upload_pass_session_ids[app->upload_pass_count]));
    app->upload_pass_count += 1u;
  }
  logger_upload_queue_tmp_release(queue);
  return true;
}

static void logger_app_reconcile_upload_blocked_fault(logger_app_t *app,
                                                      uint32_t now_ms,
                                                      bool force) {
  if (app == NULL) {
    return;
  }
  if (!force && app->last_upload_blocked_recheck_mono_ms != 0u &&
      (now_ms - app->last_upload_blocked_recheck_mono_ms) <
          LOGGER_UPLOAD_BLOCKED_RECHECK_INTERVAL_MS) {
    return;
  }
  if (!app->storage.mounted || !app->storage.writable ||
      !app->storage.logger_root_ready) {
    return;
  }

  logger_upload_queue_t *queue = logger_upload_queue_tmp_acquire();
  logger_upload_queue_summary_t summary;
  logger_upload_queue_summary_init(&summary);
  if (!logger_storage_svc_queue_load(queue)) {
    logger_upload_queue_tmp_release(queue);
    return;
  }

  app->last_upload_blocked_recheck_mono_ms = now_ms;
  logger_upload_queue_compute_summary(queue, &summary);
  if (summary.blocked_count == 0u) {
    logger_upload_queue_tmp_release(queue);
    if (app->persisted.current_fault_code ==
        LOGGER_FAULT_UPLOAD_BLOCKED_MIN_FIRMWARE) {
      logger_app_clear_current_fault(app, "upload_queue_unblocked");
    }
    return;
  }

  logger_upload_queue_tmp_release(queue);
  logger_app_maybe_latch_new_fault(app,
                                   LOGGER_FAULT_UPLOAD_BLOCKED_MIN_FIRMWARE);
}

static void logger_app_schedule_upload_retry(logger_app_t *app,
                                             uint32_t now_ms) {
  static const uint32_t delays_ms[] = {
      30000u,
      60000u,
      300000u,
      900000u,
  };
  const uint8_t max_index =
      (uint8_t)(sizeof(delays_ms) / sizeof(delays_ms[0]) - 1u);
  uint8_t delay_index =
      app->upload_pass_had_success ? 0u : app->upload_retry_backoff_index;
  if (delay_index > max_index) {
    delay_index = max_index;
  }
  app->upload_next_attempt_mono_ms = now_ms + delays_ms[delay_index];
  if (app->upload_pass_had_success) {
    app->upload_retry_backoff_index = 1u;
  } else if (app->upload_retry_backoff_index < max_index) {
    app->upload_retry_backoff_index += 1u;
  }
  logger_app_reset_upload_pass(app);
}

static bool logger_app_run_queue_maintenance(logger_app_t *app, uint32_t now_ms,
                                             bool force) {
  if (!app->storage.mounted || !app->storage.writable ||
      !app->storage.logger_root_ready) {
    return true;
  }

  if (!force && app->last_queue_maintenance_mono_ms != 0u &&
      (now_ms - app->last_queue_maintenance_mono_ms) <
          LOGGER_QUEUE_MAINTENANCE_INTERVAL_MS) {
    return true;
  }

  size_t retention_pruned_count = 0u;
  size_t reserve_pruned_count = 0u;
  bool reserve_met = false;
  if (!logger_storage_svc_queue_prune(
          &app->system_log, logger_clock_now_utc_or_null(&app->clock),
          LOGGER_SD_MIN_FREE_RESERVE_BYTES, &retention_pruned_count,
          &reserve_pruned_count, &reserve_met, NULL)) {
    return false;
  }

  app->last_queue_maintenance_mono_ms = now_ms;
  if (retention_pruned_count > 0u || reserve_pruned_count > 0u ||
      reserve_met != app->storage.reserve_ok) {
    app->last_observation_mono_ms = 0u;
    logger_app_refresh_observations(app, now_ms);
  }
  return true;
}

static bool logger_app_observed_study_day(const logger_app_t *app,
                                          char out_study_day[11]) {
  return logger_clock_derive_study_day_local_observed(
      &app->clock, app->persisted.config.timezone, out_study_day);
}

static void logger_app_reset_day_tracking(logger_app_t *app,
                                          const char *study_day_local) {
  logger_copy_string(app->current_day_study_day_local,
                     sizeof(app->current_day_study_day_local), study_day_local);
  app->day_seen_baseline = app->h10.seen_count;
  app->day_connect_baseline = app->h10.connect_count;
  app->day_ecg_start_baseline = app->h10.ecg_start_attempt_count;
  app->day_tracking_initialized =
      study_day_local != NULL && study_day_local[0] != '\0';
  app->current_day_has_session = app->session.active;
}

void logger_app_note_wall_clock_changed(logger_app_t *app) {
  if (app == NULL) {
    return;
  }

  app->runtime.wall_clock_valid = app->clock.valid;
  if (app->session.active) {
    return;
  }

  char observed_study_day_local[11] = {0};
  if (!logger_app_observed_study_day(app, observed_study_day_local)) {
    app->day_tracking_initialized = false;
    app->current_day_study_day_local[0] = '\0';
    app->pending_day_study_day_local[0] = '\0';
    app->current_day_has_session = false;
    return;
  }

  const bool same_day =
      app->day_tracking_initialized &&
      strcmp(app->current_day_study_day_local, observed_study_day_local) == 0;
  const bool preserve_has_session = same_day && app->current_day_has_session;
  logger_app_reset_day_tracking(app, observed_study_day_local);
  app->current_day_has_session = preserve_has_session;
  app->pending_day_study_day_local[0] = '\0';
}

void logger_app_note_explicit_clock_valid(logger_app_t *app, uint32_t now_ms,
                                          const char *clear_source) {
  if (app == NULL) {
    return;
  }

  app->debug_force_clock_invalid = false;
  logger_app_note_wall_clock_changed(app);
  if (!app->clock.valid) {
    app->clock_valid_since_mono_ms = 0u;
    return;
  }

  app->clock_valid_since_mono_ms = now_ms;
  if (app->persisted.current_fault_code == LOGGER_FAULT_CLOCK_INVALID) {
    logger_app_clear_current_fault(app, clear_source != NULL ? clear_source
                                                             : "clock_valid");
  }
}

void logger_app_debug_force_clock_invalid(logger_app_t *app, uint32_t now_ms) {
  if (app == NULL) {
    return;
  }

  app->debug_force_clock_invalid = true;
  app->clock.valid = false;
  app->clock.now_utc[0] = '\0';
  app->runtime.wall_clock_valid = false;
  app->clock_valid_since_mono_ms = 0u;
  logger_app_note_wall_clock_changed(app);
  logger_app_maybe_latch_new_fault(app, LOGGER_FAULT_CLOCK_INVALID);
  app->last_observation_mono_ms = now_ms;
}

void logger_app_debug_clear_forced_clock_invalid(logger_app_t *app,
                                                 uint32_t now_ms,
                                                 const char *clear_source) {
  if (app == NULL) {
    return;
  }

  app->debug_force_clock_invalid = false;
  app->last_observation_mono_ms = 0u;
  logger_app_refresh_observations(app, now_ms);
  if (app->clock.valid) {
    logger_app_note_explicit_clock_valid(app, now_ms, clear_source);
    return;
  }

  logger_app_note_wall_clock_changed(app);
}

void logger_app_debug_force_storage_fault(logger_app_t *app,
                                          logger_debug_storage_fault_t fault,
                                          uint32_t now_ms) {
  const logger_fault_code_t fault_code =
      logger_app_fault_from_debug_storage_fault(fault);
  if (app == NULL || fault_code == LOGGER_FAULT_NONE) {
    return;
  }

  app->debug_storage_fault = fault;
  app->last_observation_mono_ms = 0u;
  logger_app_refresh_observations(app, now_ms);
  logger_app_route_blocking_fault(app, fault_code, LOGGER_RUNTIME_BOOT,
                                  logger_app_debug_storage_fault_reason(fault),
                                  now_ms);
}

void logger_app_debug_clear_forced_storage_fault(logger_app_t *app,
                                                 uint32_t now_ms) {
  if (app == NULL) {
    return;
  }

  app->debug_storage_fault = LOGGER_DEBUG_STORAGE_FAULT_NONE;
  app->last_observation_mono_ms = 0u;
  logger_app_refresh_observations(app, now_ms);
  if (app->runtime.current_state != LOGGER_RUNTIME_RECOVERY_HOLD) {
    return;
  }

  switch (app->recovery_reason) {
  case LOGGER_RECOVERY_SD_MISSING_OR_UNWRITABLE:
  case LOGGER_RECOVERY_SD_LOW_SPACE_RESERVE_UNMET:
  case LOGGER_RECOVERY_SD_WRITE_FAILED:
    logger_app_recovery_set_status(app, "debug_storage_clear", "pending");
    app->recovery_next_attempt_mono_ms = now_ms;
    break;
  case LOGGER_RECOVERY_NONE:
  case LOGGER_RECOVERY_CONFIG_INCOMPLETE:
  case LOGGER_RECOVERY_LOW_BATTERY_BLOCKED_START:
  case LOGGER_RECOVERY_CRITICAL_LOW_BATTERY_STOPPED:
  default:
    break;
  }
}

static void
logger_app_log_ntp_sync_result(logger_app_t *app, const char *event_kind,
                               logger_system_log_severity_t severity,
                               const logger_clock_ntp_sync_result_t *result) {
  char details[LOGGER_SYSTEM_LOG_DETAILS_JSON_MAX + 1];
  logger_json_object_writer_t writer;
  logger_json_object_writer_init(&writer, details, sizeof(details));

  if (!logger_json_object_writer_string_field(&writer, "server",
                                              result->server) ||
      !logger_json_object_writer_string_field(&writer, "message",
                                              result->message) ||
      !logger_json_object_writer_bool_field(&writer, "applied",
                                            result->applied) ||
      !logger_json_object_writer_bool_field(&writer, "previous_valid",
                                            result->previous_valid) ||
      !logger_json_object_writer_bool_field(&writer, "large_correction",
                                            result->large_correction) ||
      !logger_json_object_writer_int64_field(&writer, "correction_seconds",
                                             result->correction_seconds) ||
      !logger_json_object_writer_uint32_field(&writer, "stratum",
                                              result->stratum) ||
      !logger_json_object_writer_string_field(&writer, "remote_address",
                                              result->remote_address) ||
      !logger_json_object_writer_string_field(&writer, "previous_utc",
                                              result->previous_utc) ||
      !logger_json_object_writer_string_field(&writer, "applied_utc",
                                              result->applied_utc) ||
      !logger_json_object_writer_finish(&writer)) {
    return;
  }

  (void)logger_system_log_append(
      &app->system_log, logger_clock_now_utc_or_null(&app->clock), event_kind,
      severity, logger_json_object_writer_data(&writer));
}

bool logger_app_clock_sync_ntp(logger_app_t *app,
                               logger_clock_ntp_sync_result_t *result) {
  if (app == NULL || result == NULL) {
    return false;
  }

  logger_clock_ntp_sync_result_init(result);
  if (app->persisted.config.wifi_ssid[0] == '\0') {
    logger_copy_string(result->message, sizeof(result->message),
                       "wifi network is not configured");
    return false;
  }

  int wifi_rc = 0;
  if (!logger_net_wifi_join(&app->persisted.config, &wifi_rc, NULL)) {
    snprintf(result->message, sizeof(result->message),
             "Wi-Fi join failed rc=%d", wifi_rc);
    logger_app_log_ntp_sync_result(app, "ntp_sync_failed",
                                   LOGGER_SYSTEM_LOG_SEVERITY_WARN, result);
    return false;
  }

  const bool synced = logger_clock_ntp_sync(&app->clock, result, &app->clock);
  logger_net_wifi_leave();

  if (synced) {
    logger_app_note_explicit_clock_valid(
        app, to_ms_since_boot(get_absolute_time()), "ntp_sync");
  } else {
    logger_app_note_wall_clock_changed(app);
  }

  if (synced) {
    logger_app_log_ntp_sync_result(app, "ntp_sync",
                                   LOGGER_SYSTEM_LOG_SEVERITY_INFO, result);
    return true;
  }

  logger_app_log_ntp_sync_result(app, "ntp_sync_failed",
                                 LOGGER_SYSTEM_LOG_SEVERITY_WARN, result);
  return false;
}

bool logger_app_request_service_mode(logger_app_t *app, uint32_t now_ms,
                                     bool *will_stop_logging_out) {
  if (will_stop_logging_out != NULL) {
    *will_stop_logging_out = false;
  }
  if (app == NULL) {
    return false;
  }

  switch (app->runtime.current_state) {
  case LOGGER_RUNTIME_SERVICE:
    return true;

  case LOGGER_RUNTIME_RECOVERY_HOLD:
    logger_app_enter_service(app, "host_service_request", now_ms, true);
    return true;

  case LOGGER_RUNTIME_UPLOAD_PREP:
  case LOGGER_RUNTIME_UPLOAD_RUNNING:
  case LOGGER_RUNTIME_BOOT:
    return false;

  case LOGGER_RUNTIME_IDLE_WAITING_FOR_CHARGER:
  case LOGGER_RUNTIME_IDLE_UPLOAD_COMPLETE:
    logger_app_enter_service(app, "host_service_request", now_ms, true);
    return true;

  case LOGGER_RUNTIME_LOG_STOPPING:
    app->runtime.planned_next_state = LOGGER_RUNTIME_SERVICE;
    app->service_pinned_by_user = true;
    logger_copy_string(app->stopping_end_reason,
                       sizeof(app->stopping_end_reason), "service_entry");
    if (will_stop_logging_out != NULL) {
      *will_stop_logging_out = true;
    }
    return true;

  case LOGGER_RUNTIME_LOG_WAIT_H10:
  case LOGGER_RUNTIME_LOG_CONNECTING:
  case LOGGER_RUNTIME_LOG_SECURING:
  case LOGGER_RUNTIME_LOG_STARTING_STREAM:
  case LOGGER_RUNTIME_LOG_STREAMING:
    break;
  default:
    break;
  }

  if (!logger_runtime_state_is_logging(app->runtime.current_state)) {
    return false;
  }

  if (!app->session.active) {
    if (!logger_app_try_finalize_no_session_day(app, now_ms)) {
      return true;
    }
    logger_app_enter_service(app, "host_service_request", now_ms, true);
    return true;
  }

  app->service_pinned_by_user = true;
  logger_copy_string(app->stopping_end_reason, sizeof(app->stopping_end_reason),
                     "service_entry");
  logger_app_transition_via_stopping(app, LOGGER_RUNTIME_SERVICE,
                                     "host_service_request", now_ms);
  if (will_stop_logging_out != NULL) {
    *will_stop_logging_out = true;
  }
  return true;
}

static bool logger_app_current_day_seen_bound_device(const logger_app_t *app) {
  return app->h10.seen_count > app->day_seen_baseline;
}

static bool logger_app_current_day_ble_connected(const logger_app_t *app) {
  return app->h10.connect_count > app->day_connect_baseline;
}

static bool
logger_app_current_day_ecg_start_attempted(const logger_app_t *app) {
  return app->h10.ecg_start_attempt_count > app->day_ecg_start_baseline;
}

void logger_app_set_last_day_outcome(logger_app_t *app,
                                     const char *study_day_local,
                                     const char *kind, const char *reason) {
  logger_copy_string(app->last_day_outcome_study_day_local,
                     sizeof(app->last_day_outcome_study_day_local),
                     study_day_local);
  logger_copy_string(app->last_day_outcome_kind,
                     sizeof(app->last_day_outcome_kind), kind);
  logger_copy_string(app->last_day_outcome_reason,
                     sizeof(app->last_day_outcome_reason), reason);
  app->last_day_outcome_valid =
      study_day_local != NULL && study_day_local[0] != '\0' && kind != NULL &&
      kind[0] != '\0' && reason != NULL && reason[0] != '\0';
}

static void logger_app_set_next_span_start_reason(logger_app_t *app,
                                                  const char *reason) {
  logger_copy_string(app->next_span_start_reason,
                     sizeof(app->next_span_start_reason), reason);
}

static const char *logger_app_take_next_span_start_reason(logger_app_t *app,
                                                          const char *fallback,
                                                          char *buf,
                                                          size_t buf_size) {
  if (app->next_span_start_reason[0] == '\0') {
    return fallback;
  }
  logger_copy_string(buf, buf_size, app->next_span_start_reason);
  app->next_span_start_reason[0] = '\0';
  return buf;
}

static void logger_app_set_stopping_end_reason(logger_app_t *app,
                                               const char *reason) {
  logger_copy_string(app->stopping_end_reason, sizeof(app->stopping_end_reason),
                     reason);
}

static bool logger_app_finalize_no_session_day(logger_app_t *app,
                                               const char *forced_reason) {
  if (!app->day_tracking_initialized || app->current_day_has_session ||
      app->current_day_study_day_local[0] == '\0') {
    return true;
  }

  const bool seen_bound_device = logger_app_current_day_seen_bound_device(app);
  const bool ble_connected = logger_app_current_day_ble_connected(app);
  const bool ecg_start_attempted =
      logger_app_current_day_ecg_start_attempted(app);

  const char *reason = forced_reason;
  if (reason == NULL || reason[0] == '\0') {
    if (!seen_bound_device) {
      reason = "no_h10_seen";
    } else if (!ble_connected) {
      reason = "no_h10_connect";
    } else {
      reason = "no_ecg_stream";
    }
  }

  char details[LOGGER_SYSTEM_LOG_DETAILS_JSON_MAX + 1];
  logger_json_object_writer_t writer;
  logger_json_object_writer_init(&writer, details, sizeof(details));
  if (!logger_json_object_writer_string_field(
          &writer, "study_day_local", app->current_day_study_day_local) ||
      !logger_json_object_writer_string_field(&writer, "reason", reason) ||
      !logger_json_object_writer_bool_field(&writer, "seen_bound_device",
                                            seen_bound_device) ||
      !logger_json_object_writer_bool_field(&writer, "ble_connected",
                                            ble_connected) ||
      !logger_json_object_writer_bool_field(&writer, "ecg_start_attempted",
                                            ecg_start_attempted) ||
      !logger_json_object_writer_finish(&writer)) {
    return false;
  }
  if (!logger_system_log_append(
          &app->system_log, logger_clock_now_utc_or_null(&app->clock),
          "no_session_day_summary", LOGGER_SYSTEM_LOG_SEVERITY_INFO,
          logger_json_object_writer_data(&writer))) {
    return false;
  }

  logger_app_set_last_day_outcome(app, app->current_day_study_day_local,
                                  "no_session", reason);
  return true;
}

static bool logger_app_finalize_no_session_before_stop(logger_app_t *app) {
  return logger_app_finalize_no_session_day(app, "stopped_before_first_span");
}

/* Try to finalize a no-session day.  Returns true if the caller may continue
   (either nothing to finalize, or finalize succeeded).  Returns false after
   transitioning to SERVICE on failure — the caller must return immediately. */
static bool logger_app_try_finalize_no_session_day(logger_app_t *app,
                                                   uint32_t now_ms) {
  if (app->session.active || app->current_day_has_session) {
    return true;
  }
  if (logger_app_finalize_no_session_before_stop(app)) {
    return true;
  }
  logger_app_route_blocking_fault(app, LOGGER_FAULT_SD_WRITE_FAILED,
                                  LOGGER_RUNTIME_BOOT,
                                  "no_session_day_summary_failed", now_ms);
  return false;
}

static logger_runtime_state_t
logger_app_h10_target_runtime_state(const logger_app_t *app) {
  switch (app->h10.phase) {
  case LOGGER_H10_PHASE_CONNECTING:
    return LOGGER_RUNTIME_LOG_CONNECTING;
  case LOGGER_H10_PHASE_SECURING:
    return LOGGER_RUNTIME_LOG_SECURING;
  case LOGGER_H10_PHASE_STARTING:
    return LOGGER_RUNTIME_LOG_STARTING_STREAM;
  case LOGGER_H10_PHASE_STREAMING:
    return LOGGER_RUNTIME_LOG_STREAMING;
  case LOGGER_H10_PHASE_OFF:
  case LOGGER_H10_PHASE_WAITING:
  case LOGGER_H10_PHASE_SCANNING:
    return LOGGER_RUNTIME_LOG_WAIT_H10;
  default:
    return LOGGER_RUNTIME_LOG_WAIT_H10;
  }
}

static const char *logger_app_session_stop_reason(const logger_app_t *app) {
  if (app->stopping_end_reason[0] != '\0') {
    return app->stopping_end_reason;
  }
  if (app->persisted.current_fault_code ==
          LOGGER_FAULT_SD_MISSING_OR_UNWRITABLE ||
      app->persisted.current_fault_code ==
          LOGGER_FAULT_SD_LOW_SPACE_RESERVE_UNMET ||
      app->persisted.current_fault_code == LOGGER_FAULT_SD_WRITE_FAILED) {
    return "storage_fault";
  }
  if (app->persisted.current_fault_code ==
          LOGGER_FAULT_CRITICAL_LOW_BATTERY_STOPPED ||
      logger_battery_is_critical(&app->battery)) {
    return "low_battery";
  }
  return "manual_stop";
}

static void
logger_app_log_h10_recovery_event(logger_app_t *app,
                                  logger_h10_recovery_event_t event) {
  const char *event_kind = logger_h10_recovery_event_name(event);
  if (app == NULL || event_kind == NULL) {
    return;
  }

  char details[LOGGER_SYSTEM_LOG_DETAILS_JSON_MAX + 1];
  logger_json_object_writer_t writer;
  logger_json_object_writer_init(&writer, details, sizeof(details));
  if (!logger_json_object_writer_string_or_null_field(
          &writer, "bound_address",
          app->persisted.config.bound_h10_address[0] != '\0'
              ? app->persisted.config.bound_h10_address
              : NULL) ||
      !logger_json_object_writer_string_or_null_field(
          &writer, "connected_address",
          app->h10.connected_address[0] != '\0' ? app->h10.connected_address
                                                : NULL) ||
      !logger_json_object_writer_string_or_null_field(
          &writer, "last_security_failure",
          logger_h10_security_failure_name(app->h10.last_security_failure)) ||
      !logger_json_object_writer_uint32_field(&writer, "bond_auto_clear_count",
                                              app->h10.bond_auto_clear_count) ||
      !logger_json_object_writer_uint32_field(
          &writer, "bond_auto_repair_count", app->h10.bond_auto_repair_count) ||
      !logger_json_object_writer_uint32_field(&writer, "last_pairing_status",
                                              app->h10.last_pairing_status) ||
      !logger_json_object_writer_uint32_field(&writer, "last_pairing_reason",
                                              app->h10.last_pairing_reason) ||
      !logger_json_object_writer_uint32_field(
          &writer, "last_disconnect_reason", app->h10.last_disconnect_reason) ||
      !logger_json_object_writer_uint32_field(&writer, "last_gatt_att_status",
                                              app->h10.last_gatt_att_status) ||
      !logger_json_object_writer_uint32_field(
          &writer, "last_start_response_status",
          app->h10.last_start_response_status) ||
      !logger_json_object_writer_finish(&writer)) {
    return;
  }

  (void)logger_system_log_append(
      &app->system_log, logger_clock_now_utc_or_null(&app->clock), event_kind,
      LOGGER_SYSTEM_LOG_SEVERITY_INFO, logger_json_object_writer_data(&writer));
}

static void logger_app_handle_h10_recovery_events(logger_app_t *app) {
  logger_h10_recovery_event_t event;
  while (logger_h10_take_recovery_event(&app->h10, &event)) {
    logger_app_log_h10_recovery_event(app, event);
  }
}

/*
 * Drain the capture pipe: staging → command ring → signal core 1.
 *
 * Core 0 drains source staging into the command ring.  Core 1
 * consumes the command ring and executes commands.
 *
 * This function does NOT execute commands inline — core 1 owns
 * SD/FatFS and is the exclusive writer.
 */
static bool logger_app_drain_capture_pipe(logger_app_t *app, uint32_t now_ms) {
  capture_pipe_t *pipe = &app->capture_pipe;

  /* Drain source staging into the command ring */
  if (capture_staging_has_data(pipe)) {
    (void)capture_staging_drain(pipe, pipe->staging.capacity);
  }

  /* Signal core 1 that work may be available. */
  if (capture_cmd_ring_occupancy(pipe) > 0u) {
    __sev();
  }

  /* Process any events (barrier completions, failures) */
  capture_pipe_process_events(pipe);

  /* Evaluate health */
  (void)capture_pipe_evaluate_health(pipe, now_ms);

  /* If a hard failure is active, check the deadline */
  if (pipe->hard_failure_active) {
    if (capture_pipe_needs_recovery(pipe, now_ms)) {
      const char *failure_reason =
          capture_writer_failure_name(pipe->last_writer_failure);
      logger_app_route_blocking_fault(
          app, LOGGER_FAULT_SD_WRITE_FAILED, LOGGER_RUNTIME_BOOT,
          failure_reason != NULL ? failure_reason : "writer_degraded_deadline",
          now_ms);
      return false;
    }
    return true;
  }

  return true;
}

static bool logger_app_handle_h10_packets(logger_app_t *app, uint32_t now_ms) {
  if (app->h10.packet_count == 0u) {
    return true;
  }

  logger_clock_sample(&app->clock);
  app->runtime.wall_clock_valid = app->clock.valid;

  logger_h10_packet_t packet;
  bool pushed_any = false;

  /*
   * The command skeleton is initialised once, then only per-packet fields
   * are updated inside the loop.  This avoids ~320 B of zeroing per packet
   * (~230 packets/s → ~73 KB/s of dead stores eliminated).
   *
   * span_id_raw may change when a new span is opened (first packet only).
   * In that case we re-init the skeleton to pick up the new span id.
   */
  logger_writer_cmd_t cmd;
  bool cmd_initialized = false;

  while (logger_h10_pop_packet(&app->h10, &packet)) {
    if (!app->session.span_active) {
      char reason_buf[sizeof(app->next_span_start_reason)];
      const char *span_start_reason = logger_app_take_next_span_start_reason(
          app, app->session.active ? "reconnect" : "session_start", reason_buf,
          sizeof(reason_buf));
      const char *error_code = NULL;
      const char *error_message = NULL;
      if (!logger_session_ensure_active_span(
              &app->session, &app->system_log, app->hardware_id,
              &app->persisted, &app->clock, &app->storage, span_start_reason,
              app->persisted.config.bound_h10_address, app->h10.encrypted,
              app->h10.bonded, app->pending_next_session_clock_jump,
              app->persisted.boot_counter, now_ms, &error_code,
              &error_message)) {
        (void)error_code;
        (void)error_message;
        logger_app_route_blocking_fault(app, LOGGER_FAULT_SD_WRITE_FAILED,
                                        LOGGER_RUNTIME_BOOT,
                                        "session_span_open_failed", now_ms);
        return false;
      }
      app->pending_next_session_clock_jump = false;
      app->current_day_has_session = true;
      app->last_session_live_flush_mono_ms = now_ms;
      app->last_session_snapshot_mono_ms = now_ms;
      app->last_chunk_seal_mono_ms = now_ms;
      /* (Re-)init skeleton after span open — span_id_raw has changed */
      logger_session_pmd_cmd_init(&app->session, &app->clock, &cmd);
      cmd_initialized = true;
    }

    if (!cmd_initialized) {
      logger_session_pmd_cmd_init(&app->session, &app->clock, &cmd);
      cmd_initialized = true;
    }

    if (!logger_session_pmd_cmd_submit(&app->session, &cmd, packet.stream_kind,
                                       packet.mono_us, packet.value,
                                       packet.value_len)) {
      logger_app_route_blocking_fault(app, LOGGER_FAULT_SD_WRITE_FAILED,
                                      LOGGER_RUNTIME_BOOT,
                                      "control_stage_overflow", now_ms);
      return false;
    }
    pushed_any = true;
  }

  if (pushed_any &&
      app->runtime.current_state != LOGGER_RUNTIME_LOG_STREAMING) {
    logger_app_state_transition(&app->runtime, LOGGER_RUNTIME_LOG_STREAMING,
                                "h10_pmd_packets", now_ms);
  }

  return true;
}

static bool logger_app_handle_h10_disconnect(logger_app_t *app,
                                             uint32_t now_ms) {
  if (!app->session.active || !app->session.span_active || app->h10.connected) {
    return true;
  }

  if (!logger_session_handle_disconnect(&app->session, &app->clock,
                                        app->persisted.boot_counter, now_ms,
                                        "disconnect")) {
    logger_app_route_blocking_fault(app, LOGGER_FAULT_SD_WRITE_FAILED,
                                    LOGGER_RUNTIME_BOOT,
                                    "session_disconnect_gap_failed", now_ms);
    return false;
  }
  app->last_session_live_flush_mono_ms = now_ms;
  app->last_chunk_seal_mono_ms = now_ms;
  return true;
}

static bool logger_app_handle_h10_battery_events(logger_app_t *app,
                                                 uint32_t now_ms) {
  logger_h10_battery_event_t event;
  while (logger_h10_take_battery_event(&app->h10, &event)) {
    if (!app->session.active) {
      continue;
    }
    if (!logger_session_append_h10_battery(
            &app->session, &app->clock, app->persisted.boot_counter, now_ms,
            event.battery_percent, event.read_reason)) {
      logger_app_route_blocking_fault(
          app, LOGGER_FAULT_SD_WRITE_FAILED, LOGGER_RUNTIME_BOOT,
          "session_h10_battery_write_failed", now_ms);
      return false;
    }
  }
  return true;
}

static bool logger_app_handle_day_and_clock_boundaries(logger_app_t *app,
                                                       uint32_t now_ms) {
  char observed_study_day_local[11] = {0};
  const bool have_study_day =
      logger_app_observed_study_day(app, observed_study_day_local);

  if (!app->day_tracking_initialized && have_study_day) {
    logger_app_reset_day_tracking(app, observed_study_day_local);
  }
  if (app->session.active) {
    app->current_day_has_session = true;
  }

  int64_t observed_utc_ns = 0ll;
  const bool have_observed_utc =
      logger_clock_observed_utc_ns(&app->clock, &observed_utc_ns);
  if (app->last_clock_observation_available && have_observed_utc) {
    const int64_t delta_ns =
        observed_utc_ns - app->last_clock_observation_utc_ns;
    const int64_t expected_delta_ns =
        (int64_t)(now_ms - app->last_clock_observation_mono_ms) * 1000000ll;
    const int64_t jump_error_ns = delta_ns - expected_delta_ns;

    if (!app->last_clock_observation_valid && app->clock.valid &&
        app->session.active) {
      const bool crosses_day =
          have_study_day &&
          strcmp(observed_study_day_local, app->session.study_day_local) != 0;
      if (!logger_session_handle_clock_event(
              &app->session, &app->clock, app->persisted.boot_counter, now_ms,
              "clock_fixed", "clock_fix", delta_ns,
              app->last_clock_observation_utc_ns, observed_utc_ns, true)) {
        logger_app_route_blocking_fault(app, LOGGER_FAULT_SD_WRITE_FAILED,
                                        LOGGER_RUNTIME_BOOT,
                                        "session_clock_fix_failed", now_ms);
        return false;
      }
      logger_app_set_next_span_start_reason(app, "clock_fix_continue");
      if (crosses_day) {
        logger_app_set_stopping_end_reason(app, "clock_fix");
        logger_copy_string(app->pending_day_study_day_local,
                           sizeof(app->pending_day_study_day_local),
                           observed_study_day_local);
        app->pending_next_session_clock_jump = false;
        logger_app_transition_via_stopping(app, LOGGER_RUNTIME_LOG_WAIT_H10,
                                           "clock_fix_new_day", now_ms);
        return false;
      }
    } else if (app->last_clock_observation_valid && !app->clock.valid &&
               app->session.active) {
      if (!logger_session_handle_clock_event(
              &app->session, &app->clock, app->persisted.boot_counter, now_ms,
              "clock_invalid", NULL, 0ll, app->last_clock_observation_utc_ns,
              observed_utc_ns, false)) {
        logger_app_route_blocking_fault(app, LOGGER_FAULT_SD_WRITE_FAILED,
                                        LOGGER_RUNTIME_BOOT,
                                        "session_clock_invalid_failed", now_ms);
        return false;
      }
    } else if (app->last_clock_observation_valid && app->clock.valid &&
               logger_app_i64_abs(jump_error_ns) >
                   (5ll * 60ll * 1000000000ll) &&
               app->session.active) {
      const bool crosses_day =
          have_study_day &&
          strcmp(observed_study_day_local, app->session.study_day_local) != 0;
      if (!logger_session_handle_clock_event(
              &app->session, &app->clock, app->persisted.boot_counter, now_ms,
              "clock_jump", "clock_jump", jump_error_ns,
              app->last_clock_observation_utc_ns, observed_utc_ns, true)) {
        logger_app_route_blocking_fault(app, LOGGER_FAULT_SD_WRITE_FAILED,
                                        LOGGER_RUNTIME_BOOT,
                                        "session_clock_jump_failed", now_ms);
        return false;
      }
      logger_app_set_next_span_start_reason(app, "clock_jump_continue");
      if (crosses_day) {
        logger_app_set_stopping_end_reason(app, "clock_jump");
        logger_copy_string(app->pending_day_study_day_local,
                           sizeof(app->pending_day_study_day_local),
                           observed_study_day_local);
        app->pending_next_session_clock_jump = true;
        logger_app_transition_via_stopping(app, LOGGER_RUNTIME_LOG_WAIT_H10,
                                           "clock_jump_new_day", now_ms);
        return false;
      }
    }
  }

  if (app->day_tracking_initialized && have_study_day &&
      strcmp(observed_study_day_local, app->current_day_study_day_local) != 0) {
    if (app->session.active) {
      logger_app_set_stopping_end_reason(app, "rollover");
      logger_app_set_next_span_start_reason(app, "rollover_continue");
      logger_copy_string(app->pending_day_study_day_local,
                         sizeof(app->pending_day_study_day_local),
                         observed_study_day_local);
      app->pending_next_session_clock_jump = false;
      logger_app_transition_via_stopping(app, LOGGER_RUNTIME_LOG_WAIT_H10,
                                         "study_day_rollover", now_ms);
      return false;
    }

    if (!logger_app_try_finalize_no_session_day(app, now_ms)) {
      return false;
    }
    logger_app_reset_day_tracking(app, observed_study_day_local);
  }

  if (have_observed_utc) {
    app->last_clock_observation_available = true;
    app->last_clock_observation_utc_ns = observed_utc_ns;
    app->last_clock_observation_mono_ms = now_ms;
    app->last_clock_observation_valid = app->clock.valid;
  }

  return true;
}

static bool logger_app_indicator_led_should_be_on(const logger_app_t *app,
                                                  uint32_t now_ms) {
  const logger_fault_code_t fault = app->persisted.current_fault_code;
  if (fault != LOGGER_FAULT_NONE) {
    const uint8_t blinks = logger_fault_blink_count(fault);
    const uint32_t phase = now_ms % 2000u;
    uint32_t start = 0u;
    for (uint8_t i = 0; i < blinks; ++i) {
      if (phase >= start && phase < (start + 100u)) {
        return true;
      }
      start += 250u;
    }
    return false;
  }

  const uint32_t phase_1s = now_ms % 1000u;
  switch (app->runtime.current_state) {
  case LOGGER_RUNTIME_SERVICE:
    return phase_1s < 120u;
  case LOGGER_RUNTIME_RECOVERY_HOLD:
    return phase_1s < 120u || (phase_1s >= 220u && phase_1s < 340u);
  case LOGGER_RUNTIME_LOG_WAIT_H10:
    return phase_1s < 70u || (phase_1s >= 160u && phase_1s < 230u);
  case LOGGER_RUNTIME_LOG_CONNECTING:
  case LOGGER_RUNTIME_LOG_SECURING:
  case LOGGER_RUNTIME_LOG_STARTING_STREAM:
  case LOGGER_RUNTIME_LOG_STREAMING:
    return phase_1s < 220u;
  case LOGGER_RUNTIME_UPLOAD_PREP:
  case LOGGER_RUNTIME_UPLOAD_RUNNING:
    return phase_1s < 500u;
  case LOGGER_RUNTIME_IDLE_WAITING_FOR_CHARGER:
    return (now_ms % 2000u) < 80u;
  case LOGGER_RUNTIME_IDLE_UPLOAD_COMPLETE:
    return false;
  case LOGGER_RUNTIME_LOG_STOPPING:
    return true;
  case LOGGER_RUNTIME_BOOT:
  default:
    return phase_1s < 50u;
  }
}

static void logger_app_update_indicator(logger_app_t *app, uint32_t now_ms) {
  const bool led_on = logger_app_indicator_led_should_be_on(app, now_ms);
  if (led_on == app->indicator_led_on) {
    return;
  }
  app->indicator_led_on = led_on;
  cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, led_on ? 1 : 0);
}

void logger_app_init(logger_app_t *app, uint32_t now_ms,
                     logger_boot_gesture_t boot_gesture) {
  memset(app, 0, sizeof(*app));
  app->boot_gesture = boot_gesture;
  logger_app_state_init(&app->runtime, now_ms);
  logger_identity_read_hardware_id_hex(app->hardware_id);
  logger_persisted_state_init(&app->persisted);
  (void)logger_config_store_load(&app->persisted);
  app->boot_firmware_identity_changed =
      !logger_app_boot_identity_matches_persisted(&app->persisted);
  logger_app_set_persisted_boot_identity(&app->persisted);
  app->persisted.boot_counter += 1u;
  (void)logger_config_store_save(&app->persisted);

  logger_battery_init();
  logger_clock_init();
  logger_h10_init(&app->h10);
  logger_h10_set_capture_stats(&app->h10, &app->capture_stats);
  logger_storage_init();
  logger_button_init(&app->button, now_ms);
  logger_service_cli_init(&app->cli);
  logger_session_init(&app->session);
  logger_capture_stats_init(&app->capture_stats);
  logger_session_set_capture_stats(&app->capture_stats);

  /*
   * Initialise PSRAM before any PSRAM-backed buffer touches.
   * cyw43_arch_init() already ran in main.c, so the SPI pins are claimed.
   *
   * PSRAM failure is a hardware fault — the capture pipeline, chunk
   * buffer, and queue scratch all require it.  Latch the fault and
   * skip buffer init so no PSRAM addresses are dereferenced.
   * step_boot will route to RECOVERY_HOLD.
   */
  const system_log_backend_t *log_backend = NULL;
  if (psram_init(PIMORONI_PICO_LIPO2XL_W_PSRAM_CS_PIN) > 0u) {
    log_backend = &system_log_backend_psram;
    printf("[logger] psram initialised, system log -> psram\n");
  } else {
    printf("[logger] FATAL: psram init failed — hardware fault\n");
    logger_app_maybe_latch_new_fault(app, LOGGER_FAULT_PSRAM_INIT_FAILED);
  }
  logger_system_log_init(&app->system_log, log_backend,
                         app->persisted.boot_counter);

  /* Wire PSRAM-backed slot arrays from the static layout.
   * Skip if PSRAM init failed — addresses would be unusable.
   *
   * Lifetime contract: after successful psram_init(), these fixed-layout
   * PSRAM pointers are treated as valid for the remainder of the boot.
   * capture_pipe_t stores them directly and does not track PSRAM liveness.
   * This is acceptable in v1 because PSRAM failure is handled as an
   * irrevocable hardware fault at boot; runtime PSRAM power-down/deinit is
   * not a supported mode.
   *
   * Ordering matters: maybe_latch_new_fault() above sets
   * current_fault_code, so this guard reads the fault that was
   * just latched (not a stale value). */
  if (app->persisted.current_fault_code != LOGGER_FAULT_PSRAM_INIT_FAILED) {
    capture_pipe_init(&app->capture_pipe,
                      &(capture_pipe_init_params_t){
                          .staging_slots = PSRAM_STAGING_SLOTS,
                          .staging_capacity = PSRAM_LAYOUT_STAGING_COUNT,
                          .cmd_ring_slots = PSRAM_CMD_RING_SLOTS,
                          .cmd_ring_capacity = PSRAM_LAYOUT_CMD_RING_COUNT,
                      });
    logger_session_init_buffers();
    logger_queue_scratch_init();
  }

  /*
   * Do NOT set the pipe yet.  The pipe is wired up in main.c AFTER
   * pre-worker recovery completes.  Until then, session->pipe == NULL
   * so all writer dispatch executes inline on core 0 — core 0 owns
   * FatFS exclusively during this window.
   */
  app->boot_recovery_done = false;

  logger_app_refresh_observations(app, now_ms);

  /*
   * Capture the watchdog reboot flag BEFORE any watchdog API call
   * that might touch the scratch registers.  watchdog_enable() is
   * called later in main.c; we read the flag here, early in init.
   *
   * watchdog_enable_caused_reboot() is true only when the watchdog
   * timer expired after being armed by watchdog_enable().  It returns
   * false after a deliberate watchdog_reboot() call (factory reset),
   * a power cycle, or a RUN-pin reset.
   */
  const bool wdt_timeout_reboot = watchdog_enable_caused_reboot();

  logger_print_boot_banner(app);
  if (wdt_timeout_reboot) {
    printf("[logger] *** watchdog timeout caused last reboot ***\n");
  }
  app->boot_banner_printed = true;

  char boot_details[LOGGER_SYSTEM_LOG_DETAILS_JSON_MAX + 1];
  logger_json_object_writer_t boot_writer;
  logger_json_object_writer_init(&boot_writer, boot_details,
                                 sizeof(boot_details));
  if (logger_json_object_writer_string_field(
          &boot_writer, "gesture",
          logger_app_boot_gesture_name(boot_gesture)) &&
      logger_json_object_writer_string_field(&boot_writer, "firmware_version",
                                             LOGGER_FIRMWARE_VERSION) &&
      logger_json_object_writer_string_field(&boot_writer, "build_id",
                                             LOGGER_BUILD_ID) &&
      logger_json_object_writer_bool_field(
          &boot_writer, "firmware_changed",
          app->boot_firmware_identity_changed) &&
      logger_json_object_writer_bool_field(
          &boot_writer, "watchdog_reboot", wdt_timeout_reboot) &&
      logger_json_object_writer_finish(&boot_writer)) {
    (void)logger_system_log_append(
        &app->system_log, logger_clock_now_utc_or_null(&app->clock), "boot",
        wdt_timeout_reboot ? LOGGER_SYSTEM_LOG_SEVERITY_WARN
                           : LOGGER_SYSTEM_LOG_SEVERITY_INFO,
        logger_json_object_writer_data(&boot_writer));
  }
  if (app->clock.lost_power) {
    (void)logger_system_log_append(
        &app->system_log, logger_clock_now_utc_or_null(&app->clock),
        "rtc_lost_power", LOGGER_SYSTEM_LOG_SEVERITY_WARN, "{}");
  }
}

bool logger_app_pre_worker_recovery(logger_app_t *app, uint32_t now_ms) {
  /*
   * Boot-time session recovery runs on core 0 with pipe == NULL.
   *
   * This is the ONLY remaining window where core 0 does direct SD/FatFS
   * session I/O.  session->pipe is NULL (set in main.c after this returns),
   * so all writer dispatch executes inline on core 0.  The storage worker
   * has not been launched yet — core 1 is idle or not yet started.
   *
   * Constraint: main.c MUST call this BEFORE logger_session_set_pipe()
   * and BEFORE logger_storage_worker_launch().  After the worker is live,
   * core 0 MUST NOT touch session FatFS directly.
   */
  logger_app_refresh_observations(app, now_ms);

  if (!logger_storage_ready_for_logging(&app->storage)) {
    /* No storage — nothing to recover. step_boot will handle the fault. */
    app->boot_recovery_done = true;
    return true;
  }

  /*
   * We don't know the unattended target yet (it depends on charger/time/
   * provisioning state that may change between now and step_boot), so
   * attempt resume.  If step_boot later decides logging isn't the target,
   * it will finalize the recovered session through the normal stopping path.
   */
  const bool resume_allowed = true;
  if (!logger_app_recover_session_if_needed(app, now_ms, resume_allowed)) {
    logger_app_route_blocking_fault(app, LOGGER_FAULT_SD_WRITE_FAILED,
                                    LOGGER_RUNTIME_BOOT,
                                    "session_recovery_failed", now_ms);
    app->boot_recovery_done = true;
    return false;
  }
  app->boot_recovery_done = true;
  return true;
}

typedef enum {
  LOGGER_STEP_CONTINUE,
  LOGGER_STEP_ABORTED,
} logger_step_result_t;

static logger_step_result_t logger_step_common_prologue(logger_app_t *app,
                                                        uint32_t now_ms,
                                                        bool h10_enabled) {
  logger_app_refresh_observations(app, now_ms);
  logger_app_reconcile_clock_invalid_fault(app, now_ms);
  logger_app_reconcile_upload_blocked_fault(app, now_ms, false);
  if (!logger_app_run_queue_maintenance(app, now_ms,
                                        !app->storage.reserve_ok)) {
    logger_app_route_blocking_fault(app, LOGGER_FAULT_SD_WRITE_FAILED,
                                    LOGGER_RUNTIME_BOOT, "queue_prune_failed",
                                    now_ms);
    return LOGGER_STEP_ABORTED;
  }
  logger_h10_set_enabled(&app->h10, h10_enabled);
  logger_service_cli_poll(&app->cli, app, now_ms);
  return LOGGER_STEP_CONTINUE;
}

static bool logger_app_state_allows_deferred_boot_queue_refresh(
    logger_runtime_state_t state) {
  switch (state) {
  case LOGGER_RUNTIME_SERVICE:
  case LOGGER_RUNTIME_UPLOAD_PREP:
  case LOGGER_RUNTIME_UPLOAD_RUNNING:
  case LOGGER_RUNTIME_IDLE_WAITING_FOR_CHARGER:
  case LOGGER_RUNTIME_IDLE_UPLOAD_COMPLETE:
    return true;

  case LOGGER_RUNTIME_BOOT:
  case LOGGER_RUNTIME_RECOVERY_HOLD:
  case LOGGER_RUNTIME_LOG_WAIT_H10:
  case LOGGER_RUNTIME_LOG_CONNECTING:
  case LOGGER_RUNTIME_LOG_SECURING:
  case LOGGER_RUNTIME_LOG_STARTING_STREAM:
  case LOGGER_RUNTIME_LOG_STREAMING:
  case LOGGER_RUNTIME_LOG_STOPPING:
  default:
    return false;
  }
}

static void logger_app_schedule_deferred_boot_queue_refresh(
    logger_app_t *app, uint32_t now_ms) {
  if (app == NULL || app->deferred_boot_queue_refresh_pending) {
    return;
  }

  app->deferred_boot_queue_refresh_pending = true;
  app->deferred_boot_queue_refresh_skip_logged = false;
  app->deferred_boot_queue_refresh_after_mono_ms =
      now_ms + LOGGER_BOOT_QUEUE_REFRESH_DEFER_MS;
  printf("[logger] deferring boot queue refresh by %lu ms\n",
         (unsigned long)LOGGER_BOOT_QUEUE_REFRESH_DEFER_MS);
}

static void logger_app_maybe_run_deferred_boot_queue_refresh(
    logger_app_t *app, uint32_t now_ms) {
  if (app == NULL || !app->deferred_boot_queue_refresh_pending) {
    return;
  }
  if ((int32_t)(now_ms - app->deferred_boot_queue_refresh_after_mono_ms) < 0) {
    return;
  }

  if (!logger_app_state_allows_deferred_boot_queue_refresh(
          app->runtime.current_state)) {
    if ((app->runtime.current_state == LOGGER_RUNTIME_RECOVERY_HOLD ||
         logger_runtime_state_is_logging(app->runtime.current_state)) &&
        (!app->deferred_boot_queue_refresh_skip_logged ||
         app->deferred_boot_queue_refresh_skip_state !=
             app->runtime.current_state)) {
      printf(
          "[logger] deferred boot queue refresh still pending; waiting for "
          "safe state (current_state=%s)\n",
          logger_runtime_state_name(app->runtime.current_state));
      app->deferred_boot_queue_refresh_skip_logged = true;
      app->deferred_boot_queue_refresh_skip_state = app->runtime.current_state;
    }
    return;
  }

  if (!app->storage.mounted || !app->storage.writable ||
      !app->storage.logger_root_ready) {
    return;
  }

  app->deferred_boot_queue_refresh_pending = false;
  app->deferred_boot_queue_refresh_skip_logged = false;

  printf("[logger] running deferred boot queue refresh\n");

  if (!logger_storage_svc_queue_refresh(
          &app->system_log, logger_clock_now_utc_or_null(&app->clock),
          NULL)) {
    logger_app_route_blocking_fault(app, LOGGER_FAULT_SD_WRITE_FAILED,
                                    app->runtime.current_state,
                                    "deferred_queue_refresh_failed", now_ms);
    return;
  }
  if (app->boot_firmware_identity_changed) {
    if (!logger_storage_svc_queue_requeue_blocked(
            &app->system_log, logger_clock_now_utc_or_null(&app->clock),
            "firmware_changed", NULL, NULL)) {
      logger_app_route_blocking_fault(app, LOGGER_FAULT_SD_WRITE_FAILED,
                                      app->runtime.current_state,
                                      "deferred_queue_rebuild_failed", now_ms);
      return;
    }
  }
  if (!logger_app_run_queue_maintenance(app, now_ms, true)) {
    logger_app_route_blocking_fault(app, LOGGER_FAULT_SD_WRITE_FAILED,
                                    app->runtime.current_state,
                                    "deferred_queue_prune_failed", now_ms);
    return;
  }
  logger_app_reconcile_upload_blocked_fault(app, now_ms, true);
}

static void logger_step_boot(logger_app_t *app, uint32_t now_ms) {
  logger_app_refresh_observations(app, now_ms);
  logger_app_reconcile_clock_invalid_fault(app, now_ms);

  if (app->boot_gesture == LOGGER_BOOT_GESTURE_FACTORY_RESET) {
    printf("[logger] boot gesture: factory reset\n");
    (void)logger_system_log_append(
        &app->system_log, logger_clock_now_utc_or_null(&app->clock),
        "factory_reset", LOGGER_SYSTEM_LOG_SEVERITY_WARN,
        "{\"source\":\"boot_gesture\"}");
    (void)logger_config_store_factory_reset(&app->persisted);
    app->reboot_pending = true;
    logger_app_enter_service(app, "factory_reset_reboot_pending", now_ms, true);
    return;
  }

  if (app->storage.mounted && app->storage.writable &&
      app->storage.logger_root_ready) {
    logger_app_schedule_deferred_boot_queue_refresh(app, now_ms);
  }

  if (app->boot_gesture == LOGGER_BOOT_GESTURE_SERVICE) {
    printf("[logger] boot gesture: forced service mode\n");
    /*
     * Pre-worker recovery already ran.  If it recovered an active session
     * but we're entering service mode, finalize it through the worker.
     */
    if (app->session.active) {
      if (!logger_session_finalize(
              &app->session, &app->system_log, &app->persisted, &app->clock,
              "service_entry", app->persisted.boot_counter, now_ms)) {
        logger_app_route_blocking_fault(app, LOGGER_FAULT_SD_WRITE_FAILED,
                                        LOGGER_RUNTIME_BOOT,
                                        "writer_finalize_failed", now_ms);
        return;
      }
    }
    logger_app_enter_service(app, "boot_service_hold", now_ms, true);
    return;
  }

  logger_fault_code_t fault_code = LOGGER_FAULT_NONE;
  const logger_runtime_state_t unattended_target =
      logger_app_select_unattended_target(app, &fault_code);

  /*
   * Pre-worker recovery already ran (see logger_app_pre_worker_recovery
   * in main.c).  If it recovered a session but the unattended target is
   * not logging, finalize through the worker now.
   */
  if (app->session.active && unattended_target != LOGGER_RUNTIME_LOG_WAIT_H10) {
    if (!logger_session_finalize(
            &app->session, &app->system_log, &app->persisted, &app->clock,
            "unexpected_reboot", app->persisted.boot_counter, now_ms)) {
      logger_app_route_blocking_fault(app, LOGGER_FAULT_SD_WRITE_FAILED,
                                      LOGGER_RUNTIME_BOOT,
                                      "writer_finalize_failed", now_ms);
      return;
    }
  }

  if (unattended_target == LOGGER_RUNTIME_SERVICE) {
    logger_app_maybe_latch_new_fault(app, fault_code);
    logger_app_enter_service(app, "config_incomplete", now_ms, true);
    return;
  }
  if (unattended_target == LOGGER_RUNTIME_RECOVERY_HOLD) {
    logger_app_route_blocking_fault(app, fault_code, LOGGER_RUNTIME_BOOT,
                                    "boot_recovery_hold", now_ms);
    return;
  }
  if (unattended_target == LOGGER_RUNTIME_UPLOAD_PREP) {
    logger_app_begin_upload_flow(app, false);
    logger_app_state_transition(&app->runtime, LOGGER_RUNTIME_UPLOAD_PREP,
                                "charger_overnight_window", now_ms);
    return;
  }

  logger_app_state_transition(&app->runtime, LOGGER_RUNTIME_LOG_WAIT_H10,
                              "boot_start_logging", now_ms);
}

static void logger_step_service(logger_app_t *app, uint32_t now_ms) {
  logger_app_refresh_observations(app, now_ms);
  logger_app_reconcile_clock_invalid_fault(app, now_ms);
  logger_h10_set_enabled(&app->h10, false);
  logger_service_cli_poll(&app->cli, app, now_ms);
  (void)logger_button_poll(&app->button, now_ms);

  if (!app->battery.vbus_present) {
    /* Snapshot state before abort_mutable_session clears config_import_active.
     */
    const bool config_import_active = app->cli.config_import_active;
    const uint32_t service_duration_ms =
        now_ms - app->runtime.entered_state_mono_ms;

    /* Pre-compute target so we can log where the device is headed. */
    logger_fault_code_t target_fault = LOGGER_FAULT_NONE;
    const logger_runtime_state_t target =
        logger_app_select_unattended_target(app, &target_fault);

    logger_service_cli_abort_mutable_session(&app->cli);
    app->service_pinned_by_user = false;

    logger_json_object_writer_t w;
    char details[192];
    logger_json_object_writer_init(&w, details, sizeof(details));
    (void)logger_json_object_writer_uint32_field(&w, "service_duration_ms",
                                                 service_duration_ms);
    (void)logger_json_object_writer_bool_field(&w, "config_import_active",
                                               config_import_active);
    (void)logger_json_object_writer_string_field(
        &w, "transitioning_to", logger_runtime_state_name(target));
    if (target_fault != LOGGER_FAULT_NONE) {
      (void)logger_json_object_writer_string_field(
          &w, "blocking_fault", logger_fault_code_name(target_fault));
    }
    (void)logger_json_object_writer_finish(&w);

    (void)logger_system_log_append(
        &app->system_log, logger_clock_now_utc_or_null(&app->clock),
        "service_auto_exit_usb_removed", LOGGER_SYSTEM_LOG_SEVERITY_INFO,
        logger_json_object_writer_data(&w));

    /*
     * apply_unattended_target calls select_unattended_target internally,
     * which is deterministic and cheap — the redundant call is acceptable.
     * Keeping the log *before* the transition means the event is recorded
     * while the device is still conceptually in SERVICE, which is the
     * correct causal ordering for post-mortem analysis.
     */
    logger_app_apply_unattended_target(app, "service_usb_removed", now_ms);
  }
}

static void logger_app_recovery_schedule_next(logger_app_t *app,
                                              uint32_t now_ms) {
  if (app->recovery_probe_interval_ms == 0u) {
    app->recovery_next_attempt_mono_ms = 0u;
    return;
  }
  app->recovery_next_attempt_mono_ms = now_ms + app->recovery_probe_interval_ms;
}

static void logger_app_recovery_reset_validation(logger_app_t *app) {
  app->recovery_good_since_mono_ms = 0u;
  app->recovery_last_success_mono_ms = 0u;
  app->recovery_validation_success_count = 0u;
}

static void logger_app_recovery_complete(logger_app_t *app,
                                         const char *clear_source,
                                         uint32_t now_ms) {
  const logger_fault_code_t reason_fault =
      logger_app_fault_from_recovery_reason(app->recovery_reason);
  if (reason_fault != LOGGER_FAULT_NONE &&
      app->persisted.current_fault_code == reason_fault) {
    logger_app_clear_current_fault(app, clear_source);
  }
  logger_app_apply_unattended_target(app, "recovery_cleared", now_ms);
}

static void logger_step_recovery_hold(logger_app_t *app, uint32_t now_ms) {
  logger_app_refresh_observations(app, now_ms);
  logger_app_reconcile_clock_invalid_fault(app, now_ms);
  logger_h10_set_enabled(&app->h10, false);
  logger_service_cli_poll(&app->cli, app, now_ms);

  if (app->recovery_reason == LOGGER_RECOVERY_NONE) {
    logger_app_apply_unattended_target(app, "recovery_missing_reason", now_ms);
    return;
  }

  if (app->recovery_reason == LOGGER_RECOVERY_CONFIG_INCOMPLETE) {
    if (app->battery.vbus_present) {
      logger_app_enter_service(app, "config_incomplete_usb_present", now_ms,
                               true);
      return;
    }
    if (app->runtime.provisioning_complete) {
      logger_app_recovery_set_status(app, "config_validate", "passed");
      logger_app_recovery_complete(app, "config_complete", now_ms);
      return;
    }
    logger_app_recovery_set_status(app, "config_validate", "blocked");
    return;
  }

  if (app->recovery_next_attempt_mono_ms != 0u &&
      !logger_app_deadline_reached(now_ms,
                                   app->recovery_next_attempt_mono_ms)) {
    return;
  }

  app->recovery_attempt_count += 1u;
  switch (app->recovery_reason) {
  case LOGGER_RECOVERY_LOW_BATTERY_BLOCKED_START:
  case LOGGER_RECOVERY_CRITICAL_LOW_BATTERY_STOPPED: {
    const bool usb_good = app->battery.vbus_present;
    const uint16_t clear_mv =
        app->recovery_reason == LOGGER_RECOVERY_LOW_BATTERY_BLOCKED_START
            ? LOGGER_RECOVERY_LOW_START_CLEAR_MV
            : LOGGER_RECOVERY_CRITICAL_CLEAR_MV;
    const bool voltage_good = app->battery.voltage_mv >= clear_mv;
    const bool good = usb_good || voltage_good;
    const uint32_t dwell_ms = usb_good ? LOGGER_RECOVERY_USB_CLEAR_DWELL_MS
                                       : LOGGER_RECOVERY_BATTERY_CLEAR_DWELL_MS;
    if (!good) {
      app->recovery_good_since_mono_ms = 0u;
      logger_app_recovery_set_status(app, "battery_recheck", "blocked");
      logger_app_recovery_schedule_next(app, now_ms);
      return;
    }
    if (app->recovery_good_since_mono_ms == 0u) {
      app->recovery_good_since_mono_ms = now_ms;
      logger_app_recovery_set_status(app, "battery_recheck", "stabilizing");
      logger_app_recovery_schedule_next(app, now_ms);
      return;
    }
    if ((now_ms - app->recovery_good_since_mono_ms) < dwell_ms) {
      logger_app_recovery_set_status(app, "battery_recheck", "stabilizing");
      logger_app_recovery_schedule_next(app, now_ms);
      return;
    }
    logger_app_recovery_set_status(app, "battery_recheck", "passed");
    logger_app_recovery_complete(app, "battery_recovered", now_ms);
    return;
  }

  case LOGGER_RECOVERY_SD_MISSING_OR_UNWRITABLE:
  case LOGGER_RECOVERY_SD_LOW_SPACE_RESERVE_UNMET:
  case LOGGER_RECOVERY_SD_WRITE_FAILED: {
    bool success = false;
    if (app->recovery_reason == LOGGER_RECOVERY_SD_MISSING_OR_UNWRITABLE) {
      success = logger_app_validate_storage_missing_recovery(app, now_ms);
    } else if (app->recovery_reason ==
               LOGGER_RECOVERY_SD_LOW_SPACE_RESERVE_UNMET) {
      success = logger_app_validate_storage_low_space_recovery(app, now_ms);
    } else {
      success = logger_app_validate_storage_write_recovery(app, now_ms);
    }

    if (!success) {
      logger_app_recovery_reset_validation(app);
      if (app->recovery_reason == LOGGER_RECOVERY_SD_WRITE_FAILED) {
        if (app->recovery_probe_interval_ms <
            LOGGER_RECOVERY_STORAGE_WRITE_PROBE_INTERVAL_MAX_MS) {
          app->recovery_probe_interval_ms *= 2u;
          if (app->recovery_probe_interval_ms >
              LOGGER_RECOVERY_STORAGE_WRITE_PROBE_INTERVAL_MAX_MS) {
            app->recovery_probe_interval_ms =
                LOGGER_RECOVERY_STORAGE_WRITE_PROBE_INTERVAL_MAX_MS;
          }
        }
      }
      logger_app_recovery_schedule_next(app, now_ms);
      return;
    }

    app->recovery_probe_interval_ms =
        logger_app_recovery_initial_probe_interval_ms(app->recovery_reason);
    if (app->recovery_validation_success_count == 0u ||
        (now_ms - app->recovery_last_success_mono_ms) >=
            LOGGER_RECOVERY_STORAGE_CLEAR_SUCCESS_GAP_MS) {
      app->recovery_validation_success_count += 1u;
      app->recovery_last_success_mono_ms = now_ms;
    }
    if (app->recovery_validation_success_count < 2u) {
      logger_app_recovery_schedule_next(app, now_ms);
      return;
    }
    logger_app_recovery_complete(app, "storage_validated", now_ms);
    return;
  }

  case LOGGER_RECOVERY_PSRAM_INIT_FAILED:
    /* Irrevocable hardware fault.  No recovery probe possible. */
    logger_app_recovery_set_status(app, "psram", "irrevocable");
    return;

  case LOGGER_RECOVERY_NONE:
  default:
    logger_app_apply_unattended_target(app, "recovery_unknown_reason", now_ms);
    return;
  }
}

static void logger_step_logging_link_state(logger_app_t *app, uint32_t now_ms) {
  const logger_runtime_state_t step_entry_state = app->runtime.current_state;
  if (logger_step_common_prologue(app, now_ms, true) == LOGGER_STEP_ABORTED) {
    return;
  }
  logger_h10_poll(&app->h10, now_ms);
  logger_app_handle_h10_recovery_events(app);
  if (app->runtime.current_state != step_entry_state) {
    return;
  }

  if (!logger_app_handle_day_and_clock_boundaries(app, now_ms)) {
    return;
  }
  if (!logger_app_handle_h10_battery_events(app, now_ms)) {
    return;
  }

  if (!logger_app_handle_h10_packets(app, now_ms)) {
    return;
  }
  if (!logger_app_drain_capture_pipe(app, now_ms)) {
    return;
  }
  if (!logger_app_handle_h10_disconnect(app, now_ms)) {
    return;
  }

  if (app->session.active &&
      (app->last_session_live_flush_mono_ms == 0u ||
       (now_ms - app->last_session_live_flush_mono_ms) >= 5000u)) {
    if (!logger_session_refresh_live(&app->session, &app->clock,
                                     app->persisted.boot_counter, now_ms)) {
      logger_app_route_blocking_fault(app, LOGGER_FAULT_SD_WRITE_FAILED,
                                      LOGGER_RUNTIME_BOOT,
                                      "session_live_write_failed", now_ms);
      return;
    }
    app->last_session_live_flush_mono_ms = now_ms;
  }

  /* Time-based chunk seal: ~1 s cadence while streaming.
   * Size seals happen inline in append_pmd_packet; barrier seals
   * happen before every non-data journal record.  This timer
   * covers the 60-second max-chunk-age rule from the data contract. */
  if (app->session.active &&
      (app->last_chunk_seal_mono_ms == 0u ||
       (now_ms - app->last_chunk_seal_mono_ms) >= 1000u)) {
    if (!logger_session_seal_chunk_if_needed(&app->session, now_ms)) {
      logger_app_route_blocking_fault(app, LOGGER_FAULT_SD_WRITE_FAILED,
                                      LOGGER_RUNTIME_BOOT,
                                      "writer_flush_failed", now_ms);
      return;
    }
    app->last_chunk_seal_mono_ms = now_ms;
  }

  const logger_fault_code_t storage_fault =
      logger_fault_from_storage(&app->storage);
  if (storage_fault != LOGGER_FAULT_NONE) {
    if (!logger_app_try_finalize_no_session_day(app, now_ms)) {
      return;
    }
    logger_app_route_blocking_fault(app, storage_fault, LOGGER_RUNTIME_BOOT,
                                    "storage_fault", now_ms);
    return;
  }

  if (logger_app_should_enter_overnight_idle(app)) {
    if (!logger_app_try_finalize_no_session_day(app, now_ms)) {
      return;
    }
    logger_app_begin_upload_after_stop(app, false, "charger_overnight_window",
                                       now_ms);
    return;
  }

  if (app->session.active && logger_battery_is_critical(&app->battery)) {
    logger_app_set_stopping_end_reason(app, "critical_low_battery");
    logger_app_route_blocking_fault(
        app, LOGGER_FAULT_CRITICAL_LOW_BATTERY_STOPPED, LOGGER_RUNTIME_BOOT,
        "critical_low_battery_stopped", now_ms);
    return;
  }
  if (!app->session.active && logger_battery_low_start_blocked(&app->battery)) {
    if (!logger_app_try_finalize_no_session_day(app, now_ms)) {
      return;
    }
    logger_app_route_blocking_fault(app, LOGGER_FAULT_LOW_BATTERY_BLOCKED_START,
                                    LOGGER_RUNTIME_BOOT,
                                    "low_battery_blocked_start", now_ms);
    return;
  }

  if (app->session.active &&
      (app->last_session_snapshot_mono_ms == 0u ||
       (now_ms - app->last_session_snapshot_mono_ms) >= 300000u)) {
    if (!logger_session_write_status_snapshot(
            &app->session, &app->clock, &app->battery, &app->storage,
            app->persisted.current_fault_code, app->persisted.boot_counter,
            now_ms)) {
      logger_app_route_blocking_fault(app, LOGGER_FAULT_SD_WRITE_FAILED,
                                      LOGGER_RUNTIME_BOOT,
                                      "session_snapshot_write_failed", now_ms);
      return;
    }
    app->last_session_snapshot_mono_ms = now_ms;
  }

  const logger_button_event_t event = logger_button_poll(&app->button, now_ms);
  if (event == LOGGER_BUTTON_EVENT_SHORT_PRESS) {
    if (app->session.active) {
      if (!logger_session_write_marker(&app->session, &app->clock,
                                       app->persisted.boot_counter, now_ms)) {
        logger_app_route_blocking_fault(app, LOGGER_FAULT_SD_WRITE_FAILED,
                                        LOGGER_RUNTIME_BOOT,
                                        "marker_write_failed", now_ms);
        return;
      }
      printf("[logger] marker recorded\n");
    } else {
      printf("[logger] marker ignored: no active session/span\n");
    }
  } else if (event == LOGGER_BUTTON_EVENT_LONG_PRESS) {
    if (!logger_app_try_finalize_no_session_day(app, now_ms)) {
      return;
    }
    if (app->battery.vbus_present ||
        logger_battery_off_charger_upload_allowed(&app->battery)) {
      logger_app_begin_upload_after_stop(app, !app->battery.vbus_present,
                                         "manual_long_press", now_ms);
    } else {
      logger_app_transition_via_stopping(
          app, LOGGER_RUNTIME_IDLE_WAITING_FOR_CHARGER,
          "manual_long_press_wait_for_charger", now_ms);
    }
    return;
  }

  const logger_runtime_state_t target_state =
      logger_app_h10_target_runtime_state(app);
  if (target_state != app->runtime.current_state) {
    logger_app_state_transition(&app->runtime, target_state, "h10_link_phase",
                                now_ms);
  }
}

static void logger_step_log_stopping(logger_app_t *app, uint32_t now_ms) {
  if (!logger_runtime_state_is_logging(app->runtime.planned_next_state)) {
    logger_h10_set_enabled(&app->h10, false);
  }
  if (app->session.active) {
    char closed_study_day_local[11];
    logger_copy_string(closed_study_day_local, sizeof(closed_study_day_local),
                       app->session.study_day_local);
    if (!logger_session_finalize(&app->session, &app->system_log,
                                 &app->persisted, &app->clock,
                                 logger_app_session_stop_reason(app),
                                 app->persisted.boot_counter, now_ms)) {
      logger_app_route_blocking_fault(app, LOGGER_FAULT_SD_WRITE_FAILED,
                                      LOGGER_RUNTIME_BOOT,
                                      "session_stop_write_failed", now_ms);
      return;
    }
    logger_app_set_last_day_outcome(app, closed_study_day_local, "session",
                                    "session_closed");
    app->last_session_live_flush_mono_ms = 0u;
    app->last_session_snapshot_mono_ms = 0u;
    app->last_chunk_seal_mono_ms = 0u;
  }
  if (app->pending_day_study_day_local[0] != '\0') {
    logger_app_reset_day_tracking(app, app->pending_day_study_day_local);
    app->pending_day_study_day_local[0] = '\0';
  }
  app->stopping_end_reason[0] = '\0';
  logger_app_state_transition(&app->runtime, app->runtime.planned_next_state,
                              "log_stopping_complete", now_ms);
}

static void logger_step_upload_prep(logger_app_t *app, uint32_t now_ms) {
  if (logger_step_common_prologue(app, now_ms, false) == LOGGER_STEP_ABORTED) {
    return;
  }

  const logger_fault_code_t storage_fault =
      logger_fault_from_storage(&app->storage);
  if (storage_fault != LOGGER_FAULT_NONE) {
    logger_app_route_blocking_fault(app, storage_fault, LOGGER_RUNTIME_BOOT,
                                    "upload_storage_fault", now_ms);
    return;
  }

  if (!app->upload_manual_off_charger && !app->battery.vbus_present) {
    logger_app_apply_unattended_target(app, "upload_usb_removed", now_ms);
    return;
  }
  if (app->upload_manual_off_charger &&
      !logger_battery_off_charger_upload_allowed(&app->battery)) {
    logger_app_transition_idle_waiting_for_charger(
        app, true, "manual_upload_battery_too_low", now_ms);
    return;
  }

  if (!app->upload_ntp_attempted) {
    logger_clock_ntp_sync_result_t ntp_result;
    app->upload_ntp_attempted = true;
    (void)logger_app_clock_sync_ntp(app, &ntp_result);
  }

  logger_upload_queue_summary_t summary;
  app->upload_pass_had_success = false;
  if (!logger_app_prepare_upload_pass(app, &summary)) {
    logger_app_route_blocking_fault(app, LOGGER_FAULT_SD_WRITE_FAILED,
                                    LOGGER_RUNTIME_BOOT, "queue_load_failed",
                                    now_ms);
    return;
  }
  (void)summary;

  if (app->upload_pass_count == 0u) {
    logger_app_transition_idle_upload_complete(app, app->battery.vbus_present,
                                               "upload_queue_empty", now_ms);
    return;
  }

  app->upload_next_attempt_mono_ms = 0u;
  logger_app_state_transition(&app->runtime, LOGGER_RUNTIME_UPLOAD_RUNNING,
                              "upload_ready", now_ms);
}

static void logger_step_upload_running(logger_app_t *app, uint32_t now_ms) {
  if (logger_step_common_prologue(app, now_ms, false) == LOGGER_STEP_ABORTED) {
    return;
  }

  const logger_fault_code_t storage_fault =
      logger_fault_from_storage(&app->storage);
  if (storage_fault != LOGGER_FAULT_NONE) {
    logger_app_route_blocking_fault(app, storage_fault, LOGGER_RUNTIME_BOOT,
                                    "upload_storage_fault", now_ms);
    return;
  }

  if (!app->upload_manual_off_charger && !app->battery.vbus_present) {
    logger_app_apply_unattended_target(app, "upload_usb_removed", now_ms);
    return;
  }
  if (app->upload_manual_off_charger &&
      !logger_battery_off_charger_upload_allowed(&app->battery)) {
    logger_app_transition_idle_waiting_for_charger(
        app, true, "manual_upload_battery_too_low", now_ms);
    return;
  }

  if (app->upload_next_attempt_mono_ms != 0u) {
    if (!logger_app_deadline_reached(now_ms,
                                     app->upload_next_attempt_mono_ms)) {
      return;
    }
    app->upload_next_attempt_mono_ms = 0u;
  }

  if (app->upload_pass_count == 0u) {
    logger_upload_queue_summary_t summary;
    if (!logger_app_prepare_upload_pass(app, &summary)) {
      logger_app_route_blocking_fault(app, LOGGER_FAULT_SD_WRITE_FAILED,
                                      LOGGER_RUNTIME_BOOT, "queue_load_failed",
                                      now_ms);
      return;
    }
    (void)summary;
    if (app->upload_pass_count == 0u) {
      logger_app_transition_idle_upload_complete(app, app->battery.vbus_present,
                                                 "upload_queue_empty", now_ms);
      return;
    }
  }

  if (app->upload_pass_next_index >= app->upload_pass_count) {
    const bool pass_had_success = app->upload_pass_had_success;
    logger_upload_queue_summary_t summary;
    if (!logger_app_prepare_upload_pass(app, &summary)) {
      logger_app_route_blocking_fault(app, LOGGER_FAULT_SD_WRITE_FAILED,
                                      LOGGER_RUNTIME_BOOT,
                                      "upload_queue_reload_failed", now_ms);
      return;
    }
    (void)summary;
    if (app->upload_pass_count == 0u) {
      logger_app_transition_idle_upload_complete(
          app, app->battery.vbus_present, "upload_queue_complete", now_ms);
      return;
    }
    app->upload_pass_had_success = pass_had_success;
    if (app->upload_manual_off_charger) {
      logger_app_transition_idle_waiting_for_charger(
          app, true, "manual_upload_pass_complete", now_ms);
      return;
    }
    logger_app_schedule_upload_retry(app, now_ms);
    return;
  }

  const char *session_id =
      app->upload_pass_session_ids[app->upload_pass_next_index];
  logger_upload_process_result_t result;
  (void)logger_upload_process_session(
      &app->system_log, &app->persisted.config, app->hardware_id,
      logger_clock_now_utc_or_null(&app->clock), session_id, &result);

  if (result.code == LOGGER_UPLOAD_PROCESS_RESULT_VERIFIED) {
    app->upload_pass_had_success = true;
  } else if (result.code == LOGGER_UPLOAD_PROCESS_RESULT_BLOCKED_MIN_FIRMWARE) {
    logger_app_maybe_latch_new_fault(app,
                                     LOGGER_FAULT_UPLOAD_BLOCKED_MIN_FIRMWARE);
  } else if (result.code == LOGGER_UPLOAD_PROCESS_RESULT_NOT_ATTEMPTED) {
    logger_app_transition_idle_upload_complete(app, app->battery.vbus_present,
                                               "upload_not_attempted", now_ms);
    return;
  }

  app->upload_pass_next_index += 1u;
}

static void logger_step_idle_waiting_for_charger(logger_app_t *app,
                                                 uint32_t now_ms) {
  if (logger_step_common_prologue(app, now_ms, false) == LOGGER_STEP_ABORTED) {
    return;
  }
  (void)logger_button_poll(&app->button, now_ms);

  if (app->battery.vbus_present) {
    logger_app_begin_upload_flow(app, false);
    logger_app_state_transition(&app->runtime, LOGGER_RUNTIME_UPLOAD_PREP,
                                "charger_attached", now_ms);
  }
}

static void logger_step_idle_upload_complete(logger_app_t *app,
                                             uint32_t now_ms) {
  if (logger_step_common_prologue(app, now_ms, false) == LOGGER_STEP_ABORTED) {
    return;
  }
  (void)logger_button_poll(&app->button, now_ms);

  if (!app->idle_resume_on_unplug && app->battery.vbus_present) {
    app->idle_resume_on_unplug = true;
  }
  if (app->idle_resume_on_unplug && !app->battery.vbus_present) {
    app->upload_manual_off_charger = false;
    logger_app_apply_unattended_target(app, "usb_removed_resume_logging",
                                       now_ms);
  }
}

static uint32_t logger_app_tighten_deadline(uint32_t cap_ms, uint32_t now_ms,
                                            uint32_t deadline_ms) {
  if (deadline_ms == 0u)
    return cap_ms;
  int32_t remaining = (int32_t)(deadline_ms - now_ms);
  if (remaining > 0 && (uint32_t)remaining < cap_ms) {
    return (uint32_t)remaining;
  }
  return cap_ms;
}

uint32_t logger_app_max_sleep_ms(const logger_app_t *app, uint32_t now_ms) {
  uint32_t cap_ms;

  switch (app->runtime.current_state) {
  case LOGGER_RUNTIME_BOOT:
  case LOGGER_RUNTIME_LOG_STOPPING:
    /* One-shot transitions — process immediately. */
    cap_ms = 20u;
    break;

  case LOGGER_RUNTIME_SERVICE:
    /* On USB. Keep CLI responsive. */
    cap_ms = 100u;
    break;

  case LOGGER_RUNTIME_LOG_STREAMING:
    /* 130 Hz ECG ≈ 8 ms period. CYW43 async events wake us on BLE
       notifications, but keep a 10 ms safety cap. */
    cap_ms = 10u;
    break;

  case LOGGER_RUNTIME_LOG_CONNECTING:
  case LOGGER_RUNTIME_LOG_SECURING:
  case LOGGER_RUNTIME_LOG_STARTING_STREAM:
    /* BLE active, events arrive quickly via async context. */
    cap_ms = 50u;
    break;

  case LOGGER_RUNTIME_LOG_WAIT_H10:
    /* BLE scan is active — CYW43 wakes us on advertisements.
       Cap exists so the 1 s observation refresh runs on time. */
    cap_ms = app->runtime.charger_present ? 200u : 500u;
    break;

  case LOGGER_RUNTIME_RECOVERY_HOLD:
    /* No BLE. On battery this is where the biggest savings live.
       1 000 ms cap preserves the diagnostic double-blink LED pattern
       (1 000 ms phase).  Tightened further below when a recovery
       deadline is pending. */
    cap_ms = app->runtime.charger_present ? 500u : 1000u;
    break;

  case LOGGER_RUNTIME_UPLOAD_PREP:
  case LOGGER_RUNTIME_UPLOAD_RUNNING:
    /* WiFi active, usually on USB. */
    cap_ms = 100u;
    break;

  case LOGGER_RUNTIME_IDLE_WAITING_FOR_CHARGER:
    /* No BLE. LED blinks on a 2 000 ms phase. */
    cap_ms = 2000u;
    break;

  case LOGGER_RUNTIME_IDLE_UPLOAD_COMPLETE:
    /* No BLE. LED is off. On battery, can sleep deep. */
    cap_ms = app->runtime.charger_present ? 2000u : 5000u;
    break;

  default:
    cap_ms = 100u;
    break;
  }

  /* Tighten towards the next recovery probe attempt. */
  cap_ms = logger_app_tighten_deadline(cap_ms, now_ms,
                                       app->recovery_next_attempt_mono_ms);

  /* Tighten towards the next upload retry attempt. */
  cap_ms = logger_app_tighten_deadline(cap_ms, now_ms,
                                       app->upload_next_attempt_mono_ms);

  /* Tighten towards the next session live-flush (5 s interval). */
  if (app->session.active && app->last_session_live_flush_mono_ms != 0u &&
      logger_runtime_state_is_logging(app->runtime.current_state)) {
    cap_ms = logger_app_tighten_deadline(
        cap_ms, now_ms, app->last_session_live_flush_mono_ms + 5000u);
  }

  /* Tighten towards the next chunk seal check (1 s interval). */
  if (app->session.active && app->last_chunk_seal_mono_ms != 0u &&
      logger_runtime_state_is_logging(app->runtime.current_state)) {
    cap_ms = logger_app_tighten_deadline(cap_ms, now_ms,
                                         app->last_chunk_seal_mono_ms + 1000u);
  }

  /* Absolute floor. */
  if (cap_ms < 1u)
    cap_ms = 1u;

  return cap_ms;
}

void logger_app_step(logger_app_t *app, uint32_t now_ms) {
  app->runtime.step_counter += 1u;

  switch (app->runtime.current_state) {
  case LOGGER_RUNTIME_BOOT:
    logger_step_boot(app, now_ms);
    break;

  case LOGGER_RUNTIME_SERVICE:
    logger_step_service(app, now_ms);
    break;

  case LOGGER_RUNTIME_RECOVERY_HOLD:
    logger_step_recovery_hold(app, now_ms);
    break;

  case LOGGER_RUNTIME_LOG_WAIT_H10:
  case LOGGER_RUNTIME_LOG_CONNECTING:
  case LOGGER_RUNTIME_LOG_SECURING:
  case LOGGER_RUNTIME_LOG_STARTING_STREAM:
  case LOGGER_RUNTIME_LOG_STREAMING:
    logger_step_logging_link_state(app, now_ms);
    break;

  case LOGGER_RUNTIME_LOG_STOPPING:
    logger_step_log_stopping(app, now_ms);
    break;

  case LOGGER_RUNTIME_IDLE_WAITING_FOR_CHARGER:
    logger_step_idle_waiting_for_charger(app, now_ms);
    break;

  case LOGGER_RUNTIME_IDLE_UPLOAD_COMPLETE:
    logger_step_idle_upload_complete(app, now_ms);
    break;

  case LOGGER_RUNTIME_UPLOAD_PREP:
    logger_step_upload_prep(app, now_ms);
    break;

  case LOGGER_RUNTIME_UPLOAD_RUNNING:
    logger_step_upload_running(app, now_ms);
    break;

  default:
    printf("[logger] unhandled state=%s; returning to service\n",
           logger_runtime_state_name(app->runtime.current_state));
    logger_app_enter_service(app, "scaffold_unhandled_state", now_ms, false);
    break;
  }

  logger_app_maybe_run_deferred_boot_queue_refresh(app, now_ms);

  logger_app_update_indicator(app, now_ms);

  if (app->reboot_pending) {
    printf("[logger] rebooting\n");
    sleep_ms(100);
    watchdog_reboot(0, 0, 10);
    while (true) {
      tight_loop_contents();
    }
  }
}
