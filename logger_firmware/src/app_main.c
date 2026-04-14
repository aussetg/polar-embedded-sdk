#include "logger/app_main.h"

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
#include "logger/upload.h"

#ifndef LOGGER_FIRMWARE_VERSION
#define LOGGER_FIRMWARE_VERSION "0.1.0-dev"
#endif

#ifndef LOGGER_BUILD_ID
#define LOGGER_BUILD_ID "logger-fw-dev"
#endif

#define LOGGER_QUEUE_MAINTENANCE_INTERVAL_MS 300000u

static bool logger_app_finalize_no_session_before_stop(logger_app_t *app);

static bool logger_timezone_is_utc_like(const char *timezone) {
    return timezone != NULL &&
           (strcmp(timezone, "UTC") == 0 || strcmp(timezone, "Etc/UTC") == 0);
}

static void logger_app_copy_string(char *dst, size_t dst_len, const char *src) {
    if (dst_len == 0u) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    size_t i = 0u;
    while (src[i] != '\0' && (i + 1u) < dst_len) {
        dst[i] = src[i];
        ++i;
    }
    dst[i] = '\0';
}

static int64_t logger_app_i64_abs(int64_t value) {
    return value < 0 ? -value : value;
}

static bool logger_app_boot_identity_matches_persisted(const logger_persisted_state_t *state) {
    return strcmp(state->last_boot_firmware_version, LOGGER_FIRMWARE_VERSION) == 0 &&
           strcmp(state->last_boot_build_id, LOGGER_BUILD_ID) == 0;
}

static void logger_app_set_persisted_boot_identity(logger_persisted_state_t *state) {
    logger_app_copy_string(state->last_boot_firmware_version,
                           sizeof(state->last_boot_firmware_version),
                           LOGGER_FIRMWARE_VERSION);
    logger_app_copy_string(state->last_boot_build_id,
                           sizeof(state->last_boot_build_id),
                           LOGGER_BUILD_ID);
}

static void logger_print_boot_banner(const logger_app_t *app) {
    printf("[logger] appliance firmware\n");
    printf("[logger] board_profile=%s\n", LOGGER_BOARD_PROFILE);
    printf("[logger] board_name=%s\n", LOGGER_BOARD_NAME);
    printf("[logger] hardware_id=%s\n", app->hardware_id);
    printf("[logger] boot_counter=%lu\n", (unsigned long)app->persisted.boot_counter);
    printf("[logger] rtc=i2c%u sda=GP%u scl=GP%u addr=0x%02x\n",
           (unsigned)LOGGER_RTC_I2C_BUS,
           (unsigned)LOGGER_RTC_SDA_PIN,
           (unsigned)LOGGER_RTC_SCL_PIN,
           (unsigned)LOGGER_RTC_I2C_ADDR);
    printf("[logger] sd=spi%u miso=GP%u cs=GP%u sck=GP%u mosi=GP%u detect=GP%u optional=%u\n",
           (unsigned)LOGGER_SD_SPI_BUS,
           (unsigned)LOGGER_SD_MISO_PIN,
           (unsigned)LOGGER_SD_CS_PIN,
           (unsigned)LOGGER_SD_SCK_PIN,
           (unsigned)LOGGER_SD_MOSI_PIN,
           (unsigned)LOGGER_SD_DETECT_PIN,
           (unsigned)LOGGER_SD_DETECT_OPTIONAL);
    printf("[logger] policy rollover=%02u:00 upload_window=%02u:00-%02u:00 reserve=%luB\n",
           (unsigned)LOGGER_STUDY_DAY_ROLLOVER_HOUR_LOCAL,
           (unsigned)LOGGER_OVERNIGHT_UPLOAD_WINDOW_START_HOUR_LOCAL,
           (unsigned)LOGGER_OVERNIGHT_UPLOAD_WINDOW_END_HOUR_LOCAL,
           (unsigned long)LOGGER_SD_MIN_FREE_RESERVE_BYTES);
    printf("[logger] battery critical=%umV start_block=%umV off_charger_upload=%umV\n",
           (unsigned)LOGGER_BATTERY_CRITICAL_STOP_MV,
           (unsigned)LOGGER_BATTERY_LOW_START_BLOCK_MV,
           (unsigned)LOGGER_BATTERY_OFF_CHARGER_UPLOAD_MIN_MV);
}

static logger_fault_code_t logger_app_storage_fault_code(const logger_storage_status_t *storage) {
    if (!storage->card_present || !storage->mounted || !storage->writable ||
        !storage->logger_root_ready || strcmp(storage->filesystem, "fat32") != 0) {
        return LOGGER_FAULT_SD_MISSING_OR_UNWRITABLE;
    }
    if (!storage->reserve_ok) {
        return LOGGER_FAULT_SD_LOW_SPACE_RESERVE_UNMET;
    }
    return LOGGER_FAULT_NONE;
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

static void logger_app_refresh_observations(logger_app_t *app, uint32_t now_ms) {
    if (app->last_observation_mono_ms != 0u && (now_ms - app->last_observation_mono_ms) < 1000u) {
        return;
    }

    logger_battery_sample(&app->battery);
    logger_clock_sample(&app->clock);
    (void)logger_storage_refresh(&app->storage);
    (void)logger_h10_set_bound_address(&app->h10, app->persisted.config.bound_h10_address);

    app->runtime.charger_present = app->battery.vbus_present;
    app->runtime.wall_clock_valid = app->clock.valid;
    app->runtime.provisioning_complete = logger_config_normal_logging_ready(&app->persisted.config);
    app->last_observation_mono_ms = now_ms;
}

static bool logger_app_recover_session_if_needed(logger_app_t *app, uint32_t now_ms, bool resume_allowed) {
    bool recovered_active = false;
    bool closed_session = false;
    if (!logger_session_recover_on_boot(
            &app->session,
            &app->system_log,
            app->hardware_id,
            &app->persisted,
            &app->clock,
            &app->storage,
            app->persisted.boot_counter,
            now_ms,
            resume_allowed,
            &recovered_active,
            &closed_session)) {
        return false;
    }
    if (recovered_active) {
        app->last_session_live_flush_mono_ms = now_ms;
        app->last_session_snapshot_mono_ms = now_ms;
    }
    if (closed_session) {
        app->last_session_live_flush_mono_ms = 0u;
        app->last_session_snapshot_mono_ms = 0u;
    }
    return true;
}

static void logger_app_maybe_latch_new_fault(logger_app_t *app, logger_fault_code_t code) {
    if (code == LOGGER_FAULT_NONE || app->persisted.current_fault_code == code) {
        return;
    }

    app->persisted.current_fault_code = code;
    (void)logger_config_store_save(&app->persisted);

    char details[LOGGER_SYSTEM_LOG_DETAILS_JSON_MAX + 1];
    logger_json_object_writer_t writer;
    logger_json_object_writer_init(&writer, details, sizeof(details));
    if (!logger_json_object_writer_string_field(&writer, "code", logger_fault_code_name(code)) ||
        !logger_json_object_writer_finish(&writer)) {
        return;
    }
    (void)logger_system_log_append(
        &app->system_log,
        app->clock.now_utc[0] != '\0' ? app->clock.now_utc : NULL,
        "fault_latched",
        LOGGER_SYSTEM_LOG_SEVERITY_WARN,
        logger_json_object_writer_data(&writer));
}

static bool logger_app_local_time_evaluable(const logger_app_t *app) {
    return app->clock.valid && logger_timezone_is_utc_like(app->persisted.config.timezone);
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

static void logger_app_transition_via_stopping(
    logger_app_t *app,
    logger_runtime_state_t next_state,
    const char *reason,
    uint32_t now_ms) {
    logger_app_state_transition(&app->runtime, LOGGER_RUNTIME_LOG_STOPPING, reason, now_ms);
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

static void logger_app_begin_upload_flow(logger_app_t *app, bool manual_off_charger) {
    app->upload_manual_off_charger = manual_off_charger;
    app->upload_ntp_attempted = false;
    app->upload_next_attempt_mono_ms = 0u;
    app->upload_retry_backoff_index = 0u;
    logger_app_reset_upload_pass(app);
}

static void logger_app_begin_upload_after_stop(
    logger_app_t *app,
    bool manual_off_charger,
    const char *reason,
    uint32_t now_ms) {
    logger_app_begin_upload_flow(app, manual_off_charger);
    logger_app_transition_via_stopping(app, LOGGER_RUNTIME_UPLOAD_PREP, reason, now_ms);
}

static void logger_app_transition_idle_waiting_for_charger(
    logger_app_t *app,
    bool manual_off_charger,
    const char *reason,
    uint32_t now_ms) {
    app->upload_manual_off_charger = manual_off_charger;
    app->upload_next_attempt_mono_ms = 0u;
    app->upload_retry_backoff_index = 0u;
    app->idle_resume_on_unplug = false;
    logger_app_reset_upload_pass(app);
    logger_app_state_transition(&app->runtime, LOGGER_RUNTIME_IDLE_WAITING_FOR_CHARGER, reason, now_ms);
}

static void logger_app_transition_idle_upload_complete(
    logger_app_t *app,
    bool resume_on_unplug,
    const char *reason,
    uint32_t now_ms) {
    app->upload_next_attempt_mono_ms = 0u;
    app->upload_retry_backoff_index = 0u;
    app->idle_resume_on_unplug = resume_on_unplug;
    logger_app_reset_upload_pass(app);
    logger_app_state_transition(&app->runtime, LOGGER_RUNTIME_IDLE_UPLOAD_COMPLETE, reason, now_ms);
}

static bool logger_app_prepare_upload_pass(
    logger_app_t *app,
    logger_upload_queue_summary_t *summary_out) {
    logger_upload_queue_t queue;
    logger_upload_queue_init(&queue);
    logger_upload_queue_summary_init(summary_out);
    app->upload_pass_count = 0u;
    app->upload_pass_next_index = 0u;

    if (!logger_upload_queue_load(&queue)) {
        return false;
    }
    logger_upload_queue_compute_summary(&queue, summary_out);

    for (size_t i = 0u; i < queue.session_count; ++i) {
        const logger_upload_queue_entry_t *entry = &queue.sessions[i];
        if (strcmp(entry->status, "pending") != 0 && strcmp(entry->status, "failed") != 0) {
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
    return true;
}

static void logger_app_schedule_upload_retry(logger_app_t *app, uint32_t now_ms) {
    static const uint32_t delays_ms[] = {
        30000u,
        60000u,
        300000u,
        900000u,
    };
    const uint8_t max_index = (uint8_t)(sizeof(delays_ms) / sizeof(delays_ms[0]) - 1u);
    uint8_t delay_index = app->upload_pass_had_success ? 0u : app->upload_retry_backoff_index;
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

static bool logger_app_run_queue_maintenance(logger_app_t *app, uint32_t now_ms, bool force) {
    if (!app->storage.mounted || !app->storage.writable || !app->storage.logger_root_ready) {
        return true;
    }

    if (!force &&
        app->last_queue_maintenance_mono_ms != 0u &&
        (now_ms - app->last_queue_maintenance_mono_ms) < LOGGER_QUEUE_MAINTENANCE_INTERVAL_MS) {
        return true;
    }

    size_t retention_pruned_count = 0u;
    size_t reserve_pruned_count = 0u;
    bool reserve_met = false;
    if (!logger_upload_queue_prune_file(&app->system_log,
                                        app->clock.now_utc[0] != '\0' ? app->clock.now_utc : NULL,
                                        LOGGER_SD_MIN_FREE_RESERVE_BYTES,
                                        &retention_pruned_count,
                                        &reserve_pruned_count,
                                        &reserve_met,
                                        NULL)) {
        return false;
    }

    app->last_queue_maintenance_mono_ms = now_ms;
    if (retention_pruned_count > 0u || reserve_pruned_count > 0u || reserve_met != app->storage.reserve_ok) {
        app->last_observation_mono_ms = 0u;
        logger_app_refresh_observations(app, now_ms);
    }
    return true;
}

static bool logger_app_observed_study_day(const logger_app_t *app, char out_study_day[11]) {
    return logger_clock_derive_study_day_local_observed(&app->clock, app->persisted.config.timezone, out_study_day);
}

static void logger_app_reset_day_tracking(logger_app_t *app, const char *study_day_local) {
    logger_app_copy_string(app->current_day_study_day_local, sizeof(app->current_day_study_day_local), study_day_local);
    app->day_seen_baseline = app->h10.seen_count;
    app->day_connect_baseline = app->h10.connect_count;
    app->day_ecg_start_baseline = app->h10.ecg_start_attempt_count;
    app->day_tracking_initialized = study_day_local != NULL && study_day_local[0] != '\0';
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

    const bool same_day = app->day_tracking_initialized &&
                          strcmp(app->current_day_study_day_local, observed_study_day_local) == 0;
    const bool preserve_has_session = same_day && app->current_day_has_session;
    logger_app_reset_day_tracking(app, observed_study_day_local);
    app->current_day_has_session = preserve_has_session;
    app->pending_day_study_day_local[0] = '\0';
}

static void logger_app_log_ntp_sync_result(
    logger_app_t *app,
    const char *event_kind,
    logger_system_log_severity_t severity,
    const logger_clock_ntp_sync_result_t *result) {
    char details[LOGGER_SYSTEM_LOG_DETAILS_JSON_MAX + 1];
    logger_json_object_writer_t writer;
    logger_json_object_writer_init(&writer, details, sizeof(details));

    if (!logger_json_object_writer_string_field(&writer, "server", result->server) ||
        !logger_json_object_writer_string_field(&writer, "message", result->message) ||
        !logger_json_object_writer_bool_field(&writer, "applied", result->applied) ||
        !logger_json_object_writer_bool_field(&writer, "previous_valid", result->previous_valid) ||
        !logger_json_object_writer_bool_field(&writer, "large_correction", result->large_correction) ||
        !logger_json_object_writer_int64_field(&writer, "correction_seconds", result->correction_seconds) ||
        !logger_json_object_writer_uint32_field(&writer, "stratum", result->stratum) ||
        !logger_json_object_writer_string_field(&writer, "remote_address", result->remote_address) ||
        !logger_json_object_writer_string_field(&writer, "previous_utc", result->previous_utc) ||
        !logger_json_object_writer_string_field(&writer, "applied_utc", result->applied_utc) ||
        !logger_json_object_writer_finish(&writer)) {
        return;
    }

    (void)logger_system_log_append(
        &app->system_log,
        app->clock.now_utc[0] != '\0' ? app->clock.now_utc : NULL,
        event_kind,
        severity,
        logger_json_object_writer_data(&writer));
}

bool logger_app_clock_sync_ntp(logger_app_t *app, logger_clock_ntp_sync_result_t *result) {
    if (app == NULL || result == NULL) {
        return false;
    }

    logger_clock_ntp_sync_result_init(result);
    if (app->persisted.config.wifi_ssid[0] == '\0') {
        logger_app_copy_string(result->message,
                               sizeof(result->message),
                               "wifi network is not configured");
        return false;
    }

    int wifi_rc = 0;
    if (!logger_net_wifi_join(&app->persisted.config, &wifi_rc, NULL)) {
        snprintf(result->message,
                 sizeof(result->message),
                 "Wi-Fi join failed rc=%d",
                 wifi_rc);
        logger_app_log_ntp_sync_result(app, "ntp_sync_failed", LOGGER_SYSTEM_LOG_SEVERITY_WARN, result);
        return false;
    }

    const bool synced = logger_clock_ntp_sync(&app->clock, result, &app->clock);
    logger_net_wifi_leave();
    logger_app_note_wall_clock_changed(app);

    if (synced) {
        logger_app_log_ntp_sync_result(app, "ntp_sync", LOGGER_SYSTEM_LOG_SEVERITY_INFO, result);
        return true;
    }

    logger_app_log_ntp_sync_result(app, "ntp_sync_failed", LOGGER_SYSTEM_LOG_SEVERITY_WARN, result);
    return false;
}

bool logger_app_request_service_mode(logger_app_t *app, uint32_t now_ms, bool *will_stop_logging_out) {
    if (will_stop_logging_out != NULL) {
        *will_stop_logging_out = false;
    }
    if (app == NULL) {
        return false;
    }

    switch (app->runtime.current_state) {
        case LOGGER_RUNTIME_SERVICE:
            return true;

        case LOGGER_RUNTIME_UPLOAD_PREP:
        case LOGGER_RUNTIME_UPLOAD_RUNNING:
        case LOGGER_RUNTIME_BOOT:
            return false;

        case LOGGER_RUNTIME_IDLE_WAITING_FOR_CHARGER:
        case LOGGER_RUNTIME_IDLE_UPLOAD_COMPLETE:
            logger_app_state_transition(&app->runtime, LOGGER_RUNTIME_SERVICE, "host_service_request", now_ms);
            return true;

        case LOGGER_RUNTIME_LOG_STOPPING:
            app->runtime.planned_next_state = LOGGER_RUNTIME_SERVICE;
            logger_app_copy_string(app->stopping_end_reason, sizeof(app->stopping_end_reason), "service_entry");
            if (will_stop_logging_out != NULL) {
                *will_stop_logging_out = true;
            }
            return true;

        default:
            break;
    }

    if (!logger_runtime_state_is_logging(app->runtime.current_state)) {
        return false;
    }

    if (!app->session.active) {
        if (!app->current_day_has_session && !logger_app_finalize_no_session_before_stop(app)) {
            logger_app_maybe_latch_new_fault(app, LOGGER_FAULT_SD_WRITE_FAILED);
            logger_app_state_transition(&app->runtime, LOGGER_RUNTIME_SERVICE, "no_session_day_summary_failed", now_ms);
            return true;
        }
        logger_app_state_transition(&app->runtime, LOGGER_RUNTIME_SERVICE, "host_service_request", now_ms);
        return true;
    }

    logger_app_copy_string(app->stopping_end_reason, sizeof(app->stopping_end_reason), "service_entry");
    logger_app_transition_via_stopping(app, LOGGER_RUNTIME_SERVICE, "host_service_request", now_ms);
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

static bool logger_app_current_day_ecg_start_attempted(const logger_app_t *app) {
    return app->h10.ecg_start_attempt_count > app->day_ecg_start_baseline;
}

static void logger_app_set_last_day_outcome(
    logger_app_t *app,
    const char *study_day_local,
    const char *kind,
    const char *reason) {
    logger_app_copy_string(app->last_day_outcome_study_day_local, sizeof(app->last_day_outcome_study_day_local), study_day_local);
    logger_app_copy_string(app->last_day_outcome_kind, sizeof(app->last_day_outcome_kind), kind);
    logger_app_copy_string(app->last_day_outcome_reason, sizeof(app->last_day_outcome_reason), reason);
    app->last_day_outcome_valid = study_day_local != NULL && study_day_local[0] != '\0' &&
                                  kind != NULL && kind[0] != '\0' &&
                                  reason != NULL && reason[0] != '\0';
}

static void logger_app_set_next_span_start_reason(logger_app_t *app, const char *reason) {
    logger_app_copy_string(app->next_span_start_reason, sizeof(app->next_span_start_reason), reason);
}

static const char *logger_app_take_next_span_start_reason(logger_app_t *app, const char *fallback) {
    static char reason[sizeof(app->next_span_start_reason)];
    if (app->next_span_start_reason[0] == '\0') {
        return fallback;
    }
    logger_app_copy_string(reason, sizeof(reason), app->next_span_start_reason);
    app->next_span_start_reason[0] = '\0';
    return reason;
}

static void logger_app_set_stopping_end_reason(logger_app_t *app, const char *reason) {
    logger_app_copy_string(app->stopping_end_reason, sizeof(app->stopping_end_reason), reason);
}

static bool logger_app_finalize_no_session_day(logger_app_t *app, const char *forced_reason) {
    if (!app->day_tracking_initialized || app->current_day_has_session || app->current_day_study_day_local[0] == '\0') {
        return true;
    }

    const bool seen_bound_device = logger_app_current_day_seen_bound_device(app);
    const bool ble_connected = logger_app_current_day_ble_connected(app);
    const bool ecg_start_attempted = logger_app_current_day_ecg_start_attempted(app);

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
    if (!logger_json_object_writer_string_field(&writer, "study_day_local", app->current_day_study_day_local) ||
        !logger_json_object_writer_string_field(&writer, "reason", reason) ||
        !logger_json_object_writer_bool_field(&writer, "seen_bound_device", seen_bound_device) ||
        !logger_json_object_writer_bool_field(&writer, "ble_connected", ble_connected) ||
        !logger_json_object_writer_bool_field(&writer, "ecg_start_attempted", ecg_start_attempted) ||
        !logger_json_object_writer_finish(&writer)) {
        return false;
    }
    if (!logger_system_log_append(
            &app->system_log,
            app->clock.now_utc[0] != '\0' ? app->clock.now_utc : NULL,
            "no_session_day_summary",
            LOGGER_SYSTEM_LOG_SEVERITY_INFO,
            logger_json_object_writer_data(&writer))) {
        return false;
    }

    logger_app_set_last_day_outcome(app, app->current_day_study_day_local, "no_session", reason);
    return true;
}

static bool logger_app_finalize_no_session_before_stop(logger_app_t *app) {
    return logger_app_finalize_no_session_day(app, "stopped_before_first_span");
}

static logger_runtime_state_t logger_app_h10_target_runtime_state(const logger_app_t *app) {
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
        default:
            return LOGGER_RUNTIME_LOG_WAIT_H10;
    }
}

static const char *logger_app_session_stop_reason(const logger_app_t *app) {
    if (app->stopping_end_reason[0] != '\0') {
        return app->stopping_end_reason;
    }
    if (app->persisted.current_fault_code == LOGGER_FAULT_SD_MISSING_OR_UNWRITABLE ||
        app->persisted.current_fault_code == LOGGER_FAULT_SD_LOW_SPACE_RESERVE_UNMET ||
        app->persisted.current_fault_code == LOGGER_FAULT_SD_WRITE_FAILED) {
        return "storage_fault";
    }
    if (logger_battery_low_start_blocked(&app->battery) || logger_battery_is_critical(&app->battery)) {
        return "low_battery";
    }
    return "manual_stop";
}

static bool logger_app_handle_h10_packets(logger_app_t *app, uint32_t now_ms) {
    if (app->h10.packet_count == 0u) {
        return true;
    }

    logger_clock_sample(&app->clock);
    app->runtime.wall_clock_valid = app->clock.valid;

    logger_h10_packet_t packet;
    bool appended_any = false;
    while (logger_h10_pop_packet(&app->h10, &packet)) {
        const char *span_start_reason = logger_app_take_next_span_start_reason(
            app,
            app->session.active ? "reconnect" : "session_start");
        if (!app->session.span_active) {
            const char *error_code = NULL;
            const char *error_message = NULL;
            if (!logger_session_ensure_active_span(
                    &app->session,
                    &app->system_log,
                    app->hardware_id,
                    &app->persisted,
                    &app->clock,
                    &app->storage,
                    span_start_reason,
                    app->persisted.config.bound_h10_address,
                    app->h10.encrypted,
                    app->h10.bonded,
                    app->pending_next_session_clock_jump,
                    app->persisted.boot_counter,
                    now_ms,
                    &error_code,
                    &error_message)) {
                (void)error_code;
                (void)error_message;
                logger_app_maybe_latch_new_fault(app, LOGGER_FAULT_SD_WRITE_FAILED);
                logger_app_transition_via_stopping(app, LOGGER_RUNTIME_SERVICE, "session_span_open_failed", now_ms);
                return false;
            }
            app->pending_next_session_clock_jump = false;
            app->current_day_has_session = true;
            app->last_session_live_flush_mono_ms = now_ms;
            app->last_session_snapshot_mono_ms = now_ms;
        }

        if (!logger_session_append_pmd_packet(
                &app->session,
                &app->clock,
                packet.stream_kind,
                packet.mono_us,
                packet.value,
                packet.value_len)) {
            logger_app_maybe_latch_new_fault(app, LOGGER_FAULT_SD_WRITE_FAILED);
            logger_app_transition_via_stopping(app, LOGGER_RUNTIME_SERVICE, "session_packet_write_failed", now_ms);
            return false;
        }
        appended_any = true;
    }

    if (appended_any && app->runtime.current_state != LOGGER_RUNTIME_LOG_STREAMING) {
        logger_app_state_transition(&app->runtime, LOGGER_RUNTIME_LOG_STREAMING, "h10_pmd_packets", now_ms);
    }

    return true;
}

static bool logger_app_handle_h10_disconnect(logger_app_t *app, uint32_t now_ms) {
    if (!app->session.active || !app->session.span_active || app->h10.connected) {
        return true;
    }

    if (!logger_session_handle_disconnect(
            &app->session,
            &app->clock,
            app->persisted.boot_counter,
            now_ms,
            "disconnect")) {
        logger_app_maybe_latch_new_fault(app, LOGGER_FAULT_SD_WRITE_FAILED);
        logger_app_transition_via_stopping(app, LOGGER_RUNTIME_SERVICE, "session_disconnect_gap_failed", now_ms);
        return false;
    }
    app->last_session_live_flush_mono_ms = now_ms;
    return true;
}

static bool logger_app_handle_h10_battery_events(logger_app_t *app, uint32_t now_ms) {
    logger_h10_battery_event_t event;
    while (logger_h10_take_battery_event(&app->h10, &event)) {
        if (!app->session.active) {
            continue;
        }
        if (!logger_session_append_h10_battery(
                &app->session,
                &app->clock,
                app->persisted.boot_counter,
                now_ms,
                event.battery_percent,
                event.read_reason)) {
            logger_app_maybe_latch_new_fault(app, LOGGER_FAULT_SD_WRITE_FAILED);
            logger_app_transition_via_stopping(app, LOGGER_RUNTIME_SERVICE, "session_h10_battery_write_failed", now_ms);
            return false;
        }
    }
    return true;
}

static bool logger_app_handle_day_and_clock_boundaries(logger_app_t *app, uint32_t now_ms) {
    char observed_study_day_local[11] = {0};
    const bool have_study_day = logger_app_observed_study_day(app, observed_study_day_local);

    if (!app->day_tracking_initialized && have_study_day) {
        logger_app_reset_day_tracking(app, observed_study_day_local);
    }
    if (app->session.active) {
        app->current_day_has_session = true;
    }

    int64_t observed_utc_ns = 0ll;
    const bool have_observed_utc = logger_clock_observed_utc_ns(&app->clock, &observed_utc_ns);
    if (app->last_clock_observation_available && have_observed_utc) {
        const int64_t delta_ns = observed_utc_ns - app->last_clock_observation_utc_ns;
        const int64_t expected_delta_ns = (int64_t)(now_ms - app->last_clock_observation_mono_ms) * 1000000ll;
        const int64_t jump_error_ns = delta_ns - expected_delta_ns;

        if (!app->last_clock_observation_valid && app->clock.valid && app->session.active) {
            const bool crosses_day = have_study_day && strcmp(observed_study_day_local, app->session.study_day_local) != 0;
            if (!logger_session_handle_clock_event(
                    &app->session,
                    &app->clock,
                    app->persisted.boot_counter,
                    now_ms,
                    "clock_fixed",
                    "clock_fix",
                    delta_ns,
                    app->last_clock_observation_utc_ns,
                    observed_utc_ns,
                    true)) {
                logger_app_maybe_latch_new_fault(app, LOGGER_FAULT_SD_WRITE_FAILED);
                logger_app_transition_via_stopping(app, LOGGER_RUNTIME_SERVICE, "session_clock_fix_failed", now_ms);
                return false;
            }
            logger_app_set_next_span_start_reason(app, "clock_fix_continue");
            if (crosses_day) {
                logger_app_set_stopping_end_reason(app, "clock_fix");
                logger_app_copy_string(app->pending_day_study_day_local,
                                       sizeof(app->pending_day_study_day_local),
                                       observed_study_day_local);
                app->pending_next_session_clock_jump = false;
                logger_app_transition_via_stopping(app, LOGGER_RUNTIME_LOG_WAIT_H10, "clock_fix_new_day", now_ms);
                return false;
            }
        } else if (app->last_clock_observation_valid && !app->clock.valid && app->session.active) {
            if (!logger_session_handle_clock_event(
                    &app->session,
                    &app->clock,
                    app->persisted.boot_counter,
                    now_ms,
                    "clock_invalid",
                    NULL,
                    0ll,
                    app->last_clock_observation_utc_ns,
                    observed_utc_ns,
                    false)) {
                logger_app_maybe_latch_new_fault(app, LOGGER_FAULT_SD_WRITE_FAILED);
                logger_app_transition_via_stopping(app, LOGGER_RUNTIME_SERVICE, "session_clock_invalid_failed", now_ms);
                return false;
            }
        } else if (app->last_clock_observation_valid && app->clock.valid &&
                   logger_app_i64_abs(jump_error_ns) > (5ll * 60ll * 1000000000ll) && app->session.active) {
            const bool crosses_day = have_study_day && strcmp(observed_study_day_local, app->session.study_day_local) != 0;
            if (!logger_session_handle_clock_event(
                    &app->session,
                    &app->clock,
                    app->persisted.boot_counter,
                    now_ms,
                    "clock_jump",
                    "clock_jump",
                    jump_error_ns,
                    app->last_clock_observation_utc_ns,
                    observed_utc_ns,
                    true)) {
                logger_app_maybe_latch_new_fault(app, LOGGER_FAULT_SD_WRITE_FAILED);
                logger_app_transition_via_stopping(app, LOGGER_RUNTIME_SERVICE, "session_clock_jump_failed", now_ms);
                return false;
            }
            logger_app_set_next_span_start_reason(app, "clock_jump_continue");
            if (crosses_day) {
                logger_app_set_stopping_end_reason(app, "clock_jump");
                logger_app_copy_string(app->pending_day_study_day_local,
                                       sizeof(app->pending_day_study_day_local),
                                       observed_study_day_local);
                app->pending_next_session_clock_jump = true;
                logger_app_transition_via_stopping(app, LOGGER_RUNTIME_LOG_WAIT_H10, "clock_jump_new_day", now_ms);
                return false;
            }
        }
    }

    if (app->day_tracking_initialized && have_study_day &&
        strcmp(observed_study_day_local, app->current_day_study_day_local) != 0) {
        if (app->session.active) {
            logger_app_set_stopping_end_reason(app, "rollover");
            logger_app_set_next_span_start_reason(app, "rollover_continue");
            logger_app_copy_string(app->pending_day_study_day_local,
                                   sizeof(app->pending_day_study_day_local),
                                   observed_study_day_local);
            app->pending_next_session_clock_jump = false;
            logger_app_transition_via_stopping(app, LOGGER_RUNTIME_LOG_WAIT_H10, "study_day_rollover", now_ms);
            return false;
        }

        if (!app->current_day_has_session) {
            if (!logger_app_finalize_no_session_day(app, NULL)) {
                logger_app_maybe_latch_new_fault(app, LOGGER_FAULT_SD_WRITE_FAILED);
                logger_app_state_transition(&app->runtime, LOGGER_RUNTIME_SERVICE, "no_session_day_summary_failed", now_ms);
                return false;
            }
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

static bool logger_app_indicator_led_should_be_on(const logger_app_t *app, uint32_t now_ms) {
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

void logger_app_init(logger_app_t *app, uint32_t now_ms, logger_boot_gesture_t boot_gesture) {
    memset(app, 0, sizeof(*app));
    app->boot_gesture = boot_gesture;
    logger_app_state_init(&app->runtime, now_ms);
    logger_identity_read_hardware_id_hex(app->hardware_id);
    logger_persisted_state_init(&app->persisted);
    (void)logger_config_store_load(&app->persisted);
    app->boot_firmware_identity_changed = !logger_app_boot_identity_matches_persisted(&app->persisted);
    logger_app_set_persisted_boot_identity(&app->persisted);
    app->persisted.boot_counter += 1u;
    (void)logger_config_store_save(&app->persisted);

    logger_battery_init();
    logger_clock_init();
    logger_h10_init(&app->h10);
    logger_storage_init();
    logger_button_init(&app->button, now_ms);
    logger_service_cli_init(&app->cli);
    logger_session_init(&app->session);
    logger_system_log_init(&app->system_log, app->persisted.boot_counter);

    logger_app_refresh_observations(app, now_ms);
    logger_print_boot_banner(app);
    app->boot_banner_printed = true;

    char boot_details[LOGGER_SYSTEM_LOG_DETAILS_JSON_MAX + 1];
    logger_json_object_writer_t boot_writer;
    logger_json_object_writer_init(&boot_writer, boot_details, sizeof(boot_details));
    if (logger_json_object_writer_string_field(&boot_writer, "gesture", logger_app_boot_gesture_name(boot_gesture)) &&
        logger_json_object_writer_string_field(&boot_writer, "firmware_version", LOGGER_FIRMWARE_VERSION) &&
        logger_json_object_writer_string_field(&boot_writer, "build_id", LOGGER_BUILD_ID) &&
        logger_json_object_writer_bool_field(&boot_writer, "firmware_changed", app->boot_firmware_identity_changed) &&
        logger_json_object_writer_finish(&boot_writer)) {
    (void)logger_system_log_append(
        &app->system_log,
        app->clock.now_utc[0] != '\0' ? app->clock.now_utc : NULL,
        "boot",
        LOGGER_SYSTEM_LOG_SEVERITY_INFO,
        logger_json_object_writer_data(&boot_writer));
    }
    if (app->clock.lost_power) {
        (void)logger_system_log_append(
            &app->system_log,
            app->clock.now_utc[0] != '\0' ? app->clock.now_utc : NULL,
            "rtc_lost_power",
            LOGGER_SYSTEM_LOG_SEVERITY_WARN,
            "{}");
    }
}

static void logger_step_boot(logger_app_t *app, uint32_t now_ms) {
    logger_app_refresh_observations(app, now_ms);

    if (app->boot_gesture == LOGGER_BOOT_GESTURE_FACTORY_RESET) {
        printf("[logger] boot gesture: factory reset\n");
        (void)logger_system_log_append(
            &app->system_log,
            app->clock.now_utc[0] != '\0' ? app->clock.now_utc : NULL,
            "factory_reset",
            LOGGER_SYSTEM_LOG_SEVERITY_WARN,
            "{\"source\":\"boot_gesture\"}");
        (void)logger_config_store_factory_reset(&app->persisted);
        app->reboot_pending = true;
        logger_app_state_transition(&app->runtime, LOGGER_RUNTIME_SERVICE, "factory_reset_reboot_pending", now_ms);
        return;
    }

    if (app->storage.mounted && app->storage.writable && app->storage.logger_root_ready) {
        if (!logger_upload_queue_refresh_file(&app->system_log,
                                              app->clock.now_utc[0] != '\0' ? app->clock.now_utc : NULL,
                                              NULL)) {
            logger_app_maybe_latch_new_fault(app, LOGGER_FAULT_SD_WRITE_FAILED);
            logger_app_state_transition(&app->runtime, LOGGER_RUNTIME_SERVICE, "queue_refresh_failed", now_ms);
            return;
        }
        if (app->boot_firmware_identity_changed) {
            if (!logger_upload_queue_requeue_blocked_file(&app->system_log,
                                                          app->clock.now_utc[0] != '\0' ? app->clock.now_utc : NULL,
                                                          "firmware_changed",
                                                          NULL,
                                                          NULL)) {
                logger_app_maybe_latch_new_fault(app, LOGGER_FAULT_SD_WRITE_FAILED);
                logger_app_state_transition(&app->runtime, LOGGER_RUNTIME_SERVICE, "queue_requeue_failed", now_ms);
                return;
            }
        }
        if (!logger_app_run_queue_maintenance(app, now_ms, true)) {
            logger_app_maybe_latch_new_fault(app, LOGGER_FAULT_SD_WRITE_FAILED);
            logger_app_state_transition(&app->runtime, LOGGER_RUNTIME_SERVICE, "queue_prune_failed", now_ms);
            return;
        }
    }

    if (app->boot_gesture == LOGGER_BOOT_GESTURE_SERVICE) {
        printf("[logger] boot gesture: forced service mode\n");
        if (logger_storage_ready_for_logging(&app->storage) &&
            !logger_app_recover_session_if_needed(app, now_ms, false)) {
            logger_app_maybe_latch_new_fault(app, LOGGER_FAULT_SD_WRITE_FAILED);
            logger_app_state_transition(&app->runtime, LOGGER_RUNTIME_SERVICE, "session_recovery_failed", now_ms);
            return;
        }
        logger_app_state_transition(&app->runtime, LOGGER_RUNTIME_SERVICE, "boot_service_hold", now_ms);
        return;
    }

    if (!app->runtime.provisioning_complete) {
        if (logger_storage_ready_for_logging(&app->storage) &&
            !logger_app_recover_session_if_needed(app, now_ms, false)) {
            logger_app_maybe_latch_new_fault(app, LOGGER_FAULT_SD_WRITE_FAILED);
            logger_app_state_transition(&app->runtime, LOGGER_RUNTIME_SERVICE, "session_recovery_failed", now_ms);
            return;
        }
        logger_app_maybe_latch_new_fault(app, LOGGER_FAULT_CONFIG_INCOMPLETE);
        logger_app_state_transition(&app->runtime, LOGGER_RUNTIME_SERVICE, "config_incomplete", now_ms);
        return;
    }

    const logger_fault_code_t storage_fault = logger_app_storage_fault_code(&app->storage);
    if (storage_fault != LOGGER_FAULT_NONE) {
        logger_app_maybe_latch_new_fault(app, storage_fault);
        logger_app_state_transition(&app->runtime, LOGGER_RUNTIME_SERVICE, "storage_not_ready", now_ms);
        return;
    }

    if (logger_battery_low_start_blocked(&app->battery)) {
        if (!logger_app_recover_session_if_needed(app, now_ms, false)) {
            logger_app_maybe_latch_new_fault(app, LOGGER_FAULT_SD_WRITE_FAILED);
            logger_app_state_transition(&app->runtime, LOGGER_RUNTIME_SERVICE, "session_recovery_failed", now_ms);
            return;
        }
        logger_app_maybe_latch_new_fault(app, LOGGER_FAULT_LOW_BATTERY_BLOCKED_START);
        logger_app_transition_idle_waiting_for_charger(app, false, "low_battery_blocked_start", now_ms);
        return;
    }

    if (!app->clock.valid) {
        logger_app_maybe_latch_new_fault(app, LOGGER_FAULT_CLOCK_INVALID);
    }

    if (logger_app_should_enter_overnight_idle(app)) {
        if (!logger_app_recover_session_if_needed(app, now_ms, false)) {
            logger_app_maybe_latch_new_fault(app, LOGGER_FAULT_SD_WRITE_FAILED);
            logger_app_state_transition(&app->runtime, LOGGER_RUNTIME_SERVICE, "session_recovery_failed", now_ms);
            return;
        }
        logger_app_begin_upload_flow(app, false);
        logger_app_state_transition(&app->runtime, LOGGER_RUNTIME_UPLOAD_PREP, "charger_overnight_window", now_ms);
        return;
    }

    if (!logger_app_recover_session_if_needed(app, now_ms, true)) {
        logger_app_maybe_latch_new_fault(app, LOGGER_FAULT_SD_WRITE_FAILED);
        logger_app_state_transition(&app->runtime, LOGGER_RUNTIME_SERVICE, "session_recovery_failed", now_ms);
        return;
    }

    logger_app_state_transition(&app->runtime, LOGGER_RUNTIME_LOG_WAIT_H10, "boot_start_logging", now_ms);
}

static void logger_step_service(logger_app_t *app, uint32_t now_ms) {
    logger_app_refresh_observations(app, now_ms);
    if (!logger_app_run_queue_maintenance(app, now_ms, !app->storage.reserve_ok)) {
        logger_app_maybe_latch_new_fault(app, LOGGER_FAULT_SD_WRITE_FAILED);
        logger_app_state_transition(&app->runtime, LOGGER_RUNTIME_SERVICE, "queue_prune_failed", now_ms);
        return;
    }
    logger_h10_set_enabled(&app->h10, false);
    logger_service_cli_poll(&app->cli, app, now_ms);
    (void)logger_button_poll(&app->button, now_ms);
}

static void logger_step_logging_link_state(logger_app_t *app, uint32_t now_ms) {
    const logger_runtime_state_t step_entry_state = app->runtime.current_state;
    logger_app_refresh_observations(app, now_ms);
    if (!logger_app_run_queue_maintenance(app, now_ms, !app->storage.reserve_ok)) {
        logger_app_maybe_latch_new_fault(app, LOGGER_FAULT_SD_WRITE_FAILED);
        logger_app_transition_via_stopping(app, LOGGER_RUNTIME_SERVICE, "queue_prune_failed", now_ms);
        return;
    }
    logger_h10_set_enabled(&app->h10, true);
    logger_h10_poll(&app->h10, now_ms);
    logger_service_cli_poll(&app->cli, app, now_ms);
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
    if (!logger_app_handle_h10_disconnect(app, now_ms)) {
        return;
    }

    if (app->session.active &&
        (app->last_session_live_flush_mono_ms == 0u || (now_ms - app->last_session_live_flush_mono_ms) >= 5000u)) {
        if (!logger_session_refresh_live(&app->session, &app->clock, app->persisted.boot_counter, now_ms)) {
            logger_app_maybe_latch_new_fault(app, LOGGER_FAULT_SD_WRITE_FAILED);
            logger_app_transition_via_stopping(app, LOGGER_RUNTIME_SERVICE, "session_live_write_failed", now_ms);
            return;
        }
        app->last_session_live_flush_mono_ms = now_ms;
    }

    const logger_fault_code_t storage_fault = logger_app_storage_fault_code(&app->storage);
    if (storage_fault != LOGGER_FAULT_NONE) {
        if (!app->session.active && !app->current_day_has_session &&
            !logger_app_finalize_no_session_before_stop(app)) {
            logger_app_maybe_latch_new_fault(app, LOGGER_FAULT_SD_WRITE_FAILED);
            logger_app_state_transition(&app->runtime, LOGGER_RUNTIME_SERVICE, "no_session_day_summary_failed", now_ms);
            return;
        }
        logger_app_maybe_latch_new_fault(app, storage_fault);
        logger_app_transition_via_stopping(app, LOGGER_RUNTIME_SERVICE, "storage_fault", now_ms);
        return;
    }

    if (logger_app_should_enter_overnight_idle(app)) {
        if (!app->session.active && !app->current_day_has_session &&
            !logger_app_finalize_no_session_before_stop(app)) {
            logger_app_maybe_latch_new_fault(app, LOGGER_FAULT_SD_WRITE_FAILED);
            logger_app_state_transition(&app->runtime, LOGGER_RUNTIME_SERVICE, "no_session_day_summary_failed", now_ms);
            return;
        }
        logger_app_begin_upload_after_stop(app, false, "charger_overnight_window", now_ms);
        return;
    }

    if (logger_battery_low_start_blocked(&app->battery)) {
        if (!app->session.active && !app->current_day_has_session &&
            !logger_app_finalize_no_session_before_stop(app)) {
            logger_app_maybe_latch_new_fault(app, LOGGER_FAULT_SD_WRITE_FAILED);
            logger_app_state_transition(&app->runtime, LOGGER_RUNTIME_SERVICE, "no_session_day_summary_failed", now_ms);
            return;
        }
        logger_app_maybe_latch_new_fault(app, LOGGER_FAULT_LOW_BATTERY_BLOCKED_START);
        logger_app_transition_via_stopping(app, LOGGER_RUNTIME_IDLE_WAITING_FOR_CHARGER, "battery_dropped_below_start_threshold", now_ms);
        return;
    }

    if (app->session.active &&
        (app->last_session_snapshot_mono_ms == 0u || (now_ms - app->last_session_snapshot_mono_ms) >= 300000u)) {
        if (!logger_session_write_status_snapshot(
                &app->session,
                &app->clock,
                &app->battery,
                &app->storage,
                app->persisted.current_fault_code,
                app->persisted.boot_counter,
                now_ms)) {
            logger_app_maybe_latch_new_fault(app, LOGGER_FAULT_SD_WRITE_FAILED);
            logger_app_transition_via_stopping(app, LOGGER_RUNTIME_SERVICE, "session_snapshot_write_failed", now_ms);
            return;
        }
        app->last_session_snapshot_mono_ms = now_ms;
    }

    const logger_button_event_t event = logger_button_poll(&app->button, now_ms);
    if (event == LOGGER_BUTTON_EVENT_SHORT_PRESS) {
        if (app->session.active) {
            if (!logger_session_write_marker(
                    &app->session,
                    &app->clock,
                    app->persisted.boot_counter,
                    now_ms)) {
                logger_app_maybe_latch_new_fault(app, LOGGER_FAULT_SD_WRITE_FAILED);
                logger_app_transition_via_stopping(app, LOGGER_RUNTIME_SERVICE, "marker_write_failed", now_ms);
                return;
            }
            printf("[logger] marker recorded\n");
        } else {
            printf("[logger] marker ignored: no active session/span\n");
        }
    } else if (event == LOGGER_BUTTON_EVENT_LONG_PRESS) {
        if (!app->session.active && !app->current_day_has_session &&
            !logger_app_finalize_no_session_before_stop(app)) {
            logger_app_maybe_latch_new_fault(app, LOGGER_FAULT_SD_WRITE_FAILED);
            logger_app_state_transition(&app->runtime, LOGGER_RUNTIME_SERVICE, "no_session_day_summary_failed", now_ms);
            return;
        }
        if (app->battery.vbus_present || logger_battery_off_charger_upload_allowed(&app->battery)) {
            logger_app_begin_upload_after_stop(app,
                                               !app->battery.vbus_present,
                                               "manual_long_press",
                                               now_ms);
        } else {
            logger_app_transition_via_stopping(app,
                                               LOGGER_RUNTIME_IDLE_WAITING_FOR_CHARGER,
                                               "manual_long_press_wait_for_charger",
                                               now_ms);
        }
        return;
    }

    const logger_runtime_state_t target_state = logger_app_h10_target_runtime_state(app);
    if (target_state != app->runtime.current_state) {
        logger_app_state_transition(&app->runtime, target_state, "h10_link_phase", now_ms);
    }
}

static void logger_step_log_stopping(logger_app_t *app, uint32_t now_ms) {
    if (!logger_runtime_state_is_logging(app->runtime.planned_next_state)) {
        logger_h10_set_enabled(&app->h10, false);
    }
    if (app->session.active) {
        char closed_study_day_local[11];
        logger_app_copy_string(closed_study_day_local, sizeof(closed_study_day_local), app->session.study_day_local);
        if (!logger_session_finalize(
                &app->session,
                &app->system_log,
                app->hardware_id,
                &app->persisted,
                &app->clock,
                &app->storage,
                logger_app_session_stop_reason(app),
                app->persisted.boot_counter,
                now_ms)) {
            logger_app_maybe_latch_new_fault(app, LOGGER_FAULT_SD_WRITE_FAILED);
            logger_app_state_transition(&app->runtime, LOGGER_RUNTIME_SERVICE, "session_stop_write_failed", now_ms);
            return;
        }
        logger_app_set_last_day_outcome(app, closed_study_day_local, "session", "session_closed");
        app->last_session_live_flush_mono_ms = 0u;
        app->last_session_snapshot_mono_ms = 0u;
    }
    if (app->pending_day_study_day_local[0] != '\0') {
        logger_app_reset_day_tracking(app, app->pending_day_study_day_local);
        app->pending_day_study_day_local[0] = '\0';
    }
    app->stopping_end_reason[0] = '\0';
    logger_app_state_transition(&app->runtime, app->runtime.planned_next_state, "log_stopping_complete", now_ms);
}

static void logger_step_upload_prep(logger_app_t *app, uint32_t now_ms) {
    logger_app_refresh_observations(app, now_ms);
    if (!logger_app_run_queue_maintenance(app, now_ms, !app->storage.reserve_ok)) {
        logger_app_maybe_latch_new_fault(app, LOGGER_FAULT_SD_WRITE_FAILED);
        logger_app_state_transition(&app->runtime, LOGGER_RUNTIME_SERVICE, "queue_prune_failed", now_ms);
        return;
    }
    logger_h10_set_enabled(&app->h10, false);
    logger_service_cli_poll(&app->cli, app, now_ms);

    const logger_fault_code_t storage_fault = logger_app_storage_fault_code(&app->storage);
    if (storage_fault != LOGGER_FAULT_NONE) {
        logger_app_maybe_latch_new_fault(app, storage_fault);
        logger_app_state_transition(&app->runtime, LOGGER_RUNTIME_SERVICE, "upload_storage_fault", now_ms);
        return;
    }

    if (!app->upload_manual_off_charger && !app->battery.vbus_present) {
        logger_app_state_transition(&app->runtime, LOGGER_RUNTIME_LOG_WAIT_H10, "upload_usb_removed", now_ms);
        return;
    }
    if (app->upload_manual_off_charger && !logger_battery_off_charger_upload_allowed(&app->battery)) {
        logger_app_transition_idle_waiting_for_charger(app, true, "manual_upload_battery_too_low", now_ms);
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
        logger_app_maybe_latch_new_fault(app, LOGGER_FAULT_SD_WRITE_FAILED);
        logger_app_state_transition(&app->runtime, LOGGER_RUNTIME_SERVICE, "upload_queue_load_failed", now_ms);
        return;
    }
    (void)summary;

    if (app->upload_pass_count == 0u) {
        logger_app_transition_idle_upload_complete(app,
                                                   app->battery.vbus_present,
                                                   "upload_queue_empty",
                                                   now_ms);
        return;
    }

    app->upload_next_attempt_mono_ms = 0u;
    logger_app_state_transition(&app->runtime, LOGGER_RUNTIME_UPLOAD_RUNNING, "upload_ready", now_ms);
}

static void logger_step_upload_running(logger_app_t *app, uint32_t now_ms) {
    logger_app_refresh_observations(app, now_ms);
    if (!logger_app_run_queue_maintenance(app, now_ms, !app->storage.reserve_ok)) {
        logger_app_maybe_latch_new_fault(app, LOGGER_FAULT_SD_WRITE_FAILED);
        logger_app_state_transition(&app->runtime, LOGGER_RUNTIME_SERVICE, "queue_prune_failed", now_ms);
        return;
    }
    logger_h10_set_enabled(&app->h10, false);
    logger_service_cli_poll(&app->cli, app, now_ms);

    const logger_fault_code_t storage_fault = logger_app_storage_fault_code(&app->storage);
    if (storage_fault != LOGGER_FAULT_NONE) {
        logger_app_maybe_latch_new_fault(app, storage_fault);
        logger_app_state_transition(&app->runtime, LOGGER_RUNTIME_SERVICE, "upload_storage_fault", now_ms);
        return;
    }

    if (!app->upload_manual_off_charger && !app->battery.vbus_present) {
        logger_app_state_transition(&app->runtime, LOGGER_RUNTIME_LOG_WAIT_H10, "upload_usb_removed", now_ms);
        return;
    }
    if (app->upload_manual_off_charger && !logger_battery_off_charger_upload_allowed(&app->battery)) {
        logger_app_transition_idle_waiting_for_charger(app, true, "manual_upload_battery_too_low", now_ms);
        return;
    }

    if (app->upload_next_attempt_mono_ms != 0u) {
        if (!logger_app_deadline_reached(now_ms, app->upload_next_attempt_mono_ms)) {
            return;
        }
        app->upload_next_attempt_mono_ms = 0u;
    }

    if (app->upload_pass_count == 0u) {
        logger_upload_queue_summary_t summary;
        if (!logger_app_prepare_upload_pass(app, &summary)) {
            logger_app_maybe_latch_new_fault(app, LOGGER_FAULT_SD_WRITE_FAILED);
            logger_app_state_transition(&app->runtime, LOGGER_RUNTIME_SERVICE, "upload_queue_load_failed", now_ms);
            return;
        }
        (void)summary;
        if (app->upload_pass_count == 0u) {
            logger_app_transition_idle_upload_complete(app,
                                                       app->battery.vbus_present,
                                                       "upload_queue_empty",
                                                       now_ms);
            return;
        }
    }

    if (app->upload_pass_next_index >= app->upload_pass_count) {
        const bool pass_had_success = app->upload_pass_had_success;
        logger_upload_queue_summary_t summary;
        if (!logger_app_prepare_upload_pass(app, &summary)) {
            logger_app_maybe_latch_new_fault(app, LOGGER_FAULT_SD_WRITE_FAILED);
            logger_app_state_transition(&app->runtime, LOGGER_RUNTIME_SERVICE, "upload_queue_reload_failed", now_ms);
            return;
        }
        (void)summary;
        if (app->upload_pass_count == 0u) {
            logger_app_transition_idle_upload_complete(app,
                                                       app->battery.vbus_present,
                                                       "upload_queue_complete",
                                                       now_ms);
            return;
        }
        app->upload_pass_had_success = pass_had_success;
        if (app->upload_manual_off_charger) {
            logger_app_transition_idle_waiting_for_charger(app, true, "manual_upload_pass_complete", now_ms);
            return;
        }
        logger_app_schedule_upload_retry(app, now_ms);
        return;
    }

    const char *session_id = app->upload_pass_session_ids[app->upload_pass_next_index];
    logger_upload_process_result_t result;
    (void)logger_upload_process_session(
        &app->system_log,
        &app->persisted.config,
        app->hardware_id,
        app->clock.now_utc[0] != '\0' ? app->clock.now_utc : NULL,
        session_id,
        &result);

    if (result.code == LOGGER_UPLOAD_PROCESS_RESULT_VERIFIED) {
        app->upload_pass_had_success = true;
    } else if (result.code == LOGGER_UPLOAD_PROCESS_RESULT_BLOCKED_MIN_FIRMWARE) {
        logger_app_maybe_latch_new_fault(app, LOGGER_FAULT_UPLOAD_BLOCKED_MIN_FIRMWARE);
    } else if (result.code == LOGGER_UPLOAD_PROCESS_RESULT_NOT_ATTEMPTED) {
        logger_app_transition_idle_upload_complete(app,
                                                   app->battery.vbus_present,
                                                   "upload_not_attempted",
                                                   now_ms);
        return;
    }

    app->upload_pass_next_index += 1u;
}

static void logger_step_idle_waiting_for_charger(logger_app_t *app, uint32_t now_ms) {
    logger_app_refresh_observations(app, now_ms);
    if (!logger_app_run_queue_maintenance(app, now_ms, !app->storage.reserve_ok)) {
        logger_app_maybe_latch_new_fault(app, LOGGER_FAULT_SD_WRITE_FAILED);
        logger_app_state_transition(&app->runtime, LOGGER_RUNTIME_SERVICE, "queue_prune_failed", now_ms);
        return;
    }
    logger_h10_set_enabled(&app->h10, false);
    logger_service_cli_poll(&app->cli, app, now_ms);
    (void)logger_button_poll(&app->button, now_ms);

    if (app->battery.vbus_present) {
        logger_app_begin_upload_flow(app, false);
        logger_app_state_transition(&app->runtime, LOGGER_RUNTIME_UPLOAD_PREP, "charger_attached", now_ms);
    }
}

static void logger_step_idle_upload_complete(logger_app_t *app, uint32_t now_ms) {
    logger_app_refresh_observations(app, now_ms);
    if (!logger_app_run_queue_maintenance(app, now_ms, !app->storage.reserve_ok)) {
        logger_app_maybe_latch_new_fault(app, LOGGER_FAULT_SD_WRITE_FAILED);
        logger_app_state_transition(&app->runtime, LOGGER_RUNTIME_SERVICE, "queue_prune_failed", now_ms);
        return;
    }
    logger_h10_set_enabled(&app->h10, false);
    logger_service_cli_poll(&app->cli, app, now_ms);
    (void)logger_button_poll(&app->button, now_ms);

    if (!app->idle_resume_on_unplug && app->battery.vbus_present) {
        app->idle_resume_on_unplug = true;
    }
    if (app->idle_resume_on_unplug && !app->battery.vbus_present) {
        app->upload_manual_off_charger = false;
        logger_app_state_transition(&app->runtime, LOGGER_RUNTIME_LOG_WAIT_H10, "usb_removed_resume_logging", now_ms);
    }
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
            logger_app_state_transition(&app->runtime, LOGGER_RUNTIME_SERVICE, "scaffold_unhandled_state", now_ms);
            break;
    }

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
