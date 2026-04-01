#ifndef LOGGER_FIRMWARE_APP_MAIN_H
#define LOGGER_FIRMWARE_APP_MAIN_H

#include <stdbool.h>
#include <stdint.h>

#include "logger/app_state.h"
#include "logger/battery.h"
#include "logger/button.h"
#include "logger/clock.h"
#include "logger/config_store.h"
#include "logger/h10.h"
#include "logger/identity.h"
#include "logger/queue.h"
#include "logger/session.h"
#include "logger/service_cli.h"
#include "logger/storage.h"
#include "logger/system_log.h"

typedef struct logger_app {
    logger_app_state_t runtime;
    logger_boot_gesture_t boot_gesture;
    logger_button_t button;
    logger_battery_status_t battery;
    logger_clock_status_t clock;
    logger_persisted_state_t persisted;
    logger_h10_state_t h10;
    logger_storage_status_t storage;
    logger_system_log_t system_log;
    logger_session_state_t session;
    logger_service_cli_t cli;
    char hardware_id[LOGGER_HARDWARE_ID_HEX_LEN + 1];
    char current_day_study_day_local[11];
    char pending_day_study_day_local[11];
    char last_day_outcome_study_day_local[11];
    char last_day_outcome_kind[16];
    char last_day_outcome_reason[32];
    char next_span_start_reason[32];
    char stopping_end_reason[32];
    uint32_t last_observation_mono_ms;
    uint32_t last_session_live_flush_mono_ms;
    uint32_t last_session_snapshot_mono_ms;
    uint32_t day_seen_baseline;
    uint32_t day_connect_baseline;
    uint32_t day_ecg_start_baseline;
    uint32_t last_clock_observation_mono_ms;
    uint32_t last_queue_maintenance_mono_ms;
    uint32_t upload_next_attempt_mono_ms;
    int64_t last_clock_observation_utc_ns;
    uint8_t upload_retry_backoff_index;
    uint8_t upload_pass_count;
    uint8_t upload_pass_next_index;
    char upload_pass_session_ids[LOGGER_UPLOAD_QUEUE_MAX_SESSIONS][33];
    bool upload_manual_off_charger;
    bool upload_pass_had_success;
    bool idle_resume_on_unplug;
    bool day_tracking_initialized;
    bool current_day_has_session;
    bool last_day_outcome_valid;
    bool pending_next_session_clock_jump;
    bool last_clock_observation_available;
    bool last_clock_observation_valid;
    bool indicator_led_on;
    bool boot_banner_printed;
    bool boot_firmware_identity_changed;
    bool reboot_pending;
} logger_app_t;

void logger_app_init(logger_app_t *app, uint32_t now_ms, logger_boot_gesture_t boot_gesture);
void logger_app_step(logger_app_t *app, uint32_t now_ms);

#endif
