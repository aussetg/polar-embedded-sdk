#include "logger/app_main.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "hardware/watchdog.h"
#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"

#include "board_config.h"

static bool logger_timezone_is_utc_like(const char *timezone) {
    return timezone != NULL &&
           (strcmp(timezone, "UTC") == 0 || strcmp(timezone, "Etc/UTC") == 0);
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

    app->runtime.charger_present = app->battery.vbus_present;
    app->runtime.wall_clock_valid = app->clock.valid;
    app->runtime.provisioning_complete = logger_config_normal_logging_ready(&app->persisted.config);
    app->last_observation_mono_ms = now_ms;
}

static void logger_app_maybe_latch_new_fault(logger_app_t *app, logger_fault_code_t code) {
    if (code == LOGGER_FAULT_NONE || app->persisted.current_fault_code == code) {
        return;
    }

    app->persisted.current_fault_code = code;
    (void)logger_config_store_save(&app->persisted);

    char details[96];
    snprintf(details,
             sizeof(details),
             "{\"code\":\"%s\"}",
             logger_fault_code_name(code));
    (void)logger_system_log_append(
        &app->system_log,
        app->clock.now_utc[0] != '\0' ? app->clock.now_utc : NULL,
        "fault_latched",
        LOGGER_SYSTEM_LOG_SEVERITY_WARN,
        details);
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
    app->persisted.boot_counter += 1u;
    (void)logger_config_store_save(&app->persisted);

    logger_battery_init();
    logger_clock_init();
    logger_storage_init();
    logger_button_init(&app->button, now_ms);
    logger_service_cli_init(&app->cli);
    logger_session_init(&app->session);
    logger_system_log_init(&app->system_log, app->persisted.boot_counter);

    logger_app_refresh_observations(app, now_ms);
    logger_print_boot_banner(app);
    app->boot_banner_printed = true;

    char boot_details[128];
    snprintf(boot_details,
             sizeof(boot_details),
             "{\"gesture\":\"%s\"}",
             logger_app_boot_gesture_name(boot_gesture));
    (void)logger_system_log_append(
        &app->system_log,
        app->clock.now_utc[0] != '\0' ? app->clock.now_utc : NULL,
        "boot",
        LOGGER_SYSTEM_LOG_SEVERITY_INFO,
        boot_details);
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

    if (app->boot_gesture == LOGGER_BOOT_GESTURE_SERVICE) {
        printf("[logger] boot gesture: forced service mode\n");
        logger_app_state_transition(&app->runtime, LOGGER_RUNTIME_SERVICE, "boot_service_hold", now_ms);
        return;
    }

    if (!app->runtime.provisioning_complete) {
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
        logger_app_maybe_latch_new_fault(app, LOGGER_FAULT_LOW_BATTERY_BLOCKED_START);
        logger_app_state_transition(&app->runtime, LOGGER_RUNTIME_IDLE_WAITING_FOR_CHARGER, "low_battery_blocked_start", now_ms);
        return;
    }

    if (!app->clock.valid) {
        logger_app_maybe_latch_new_fault(app, LOGGER_FAULT_CLOCK_INVALID);
    }

    if (logger_app_should_enter_overnight_idle(app)) {
        logger_app_state_transition(&app->runtime, LOGGER_RUNTIME_IDLE_UPLOAD_COMPLETE, "charger_overnight_window", now_ms);
        return;
    }

    logger_app_state_transition(&app->runtime, LOGGER_RUNTIME_LOG_WAIT_H10, "boot_start_logging", now_ms);
}

static void logger_step_service(logger_app_t *app, uint32_t now_ms) {
    logger_app_refresh_observations(app, now_ms);
    logger_service_cli_poll(&app->cli, app, now_ms);
    (void)logger_button_poll(&app->button, now_ms);
}

static void logger_step_log_wait_h10(logger_app_t *app, uint32_t now_ms) {
    logger_app_refresh_observations(app, now_ms);
    logger_service_cli_poll(&app->cli, app, now_ms);

    const logger_fault_code_t storage_fault = logger_app_storage_fault_code(&app->storage);
    if (storage_fault != LOGGER_FAULT_NONE) {
        logger_app_maybe_latch_new_fault(app, storage_fault);
        logger_app_transition_via_stopping(app, LOGGER_RUNTIME_SERVICE, "storage_fault", now_ms);
        return;
    }

    if (logger_battery_low_start_blocked(&app->battery)) {
        logger_app_maybe_latch_new_fault(app, LOGGER_FAULT_LOW_BATTERY_BLOCKED_START);
        logger_app_state_transition(&app->runtime, LOGGER_RUNTIME_IDLE_WAITING_FOR_CHARGER, "battery_dropped_below_start_threshold", now_ms);
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
        logger_app_transition_via_stopping(app, LOGGER_RUNTIME_IDLE_UPLOAD_COMPLETE, "manual_long_press", now_ms);
    }
}

static void logger_step_log_stopping(logger_app_t *app, uint32_t now_ms) {
    if (app->session.active) {
        if (!logger_session_stop_debug(
                &app->session,
                &app->system_log,
                &app->clock,
                app->persisted.boot_counter,
                now_ms)) {
            logger_app_maybe_latch_new_fault(app, LOGGER_FAULT_SD_WRITE_FAILED);
            logger_app_state_transition(&app->runtime, LOGGER_RUNTIME_SERVICE, "session_stop_write_failed", now_ms);
            return;
        }
    }
    logger_app_state_transition(&app->runtime, app->runtime.planned_next_state, "log_stopping_complete", now_ms);
}

static void logger_step_idle_common(logger_app_t *app, uint32_t now_ms) {
    logger_app_refresh_observations(app, now_ms);
    logger_service_cli_poll(&app->cli, app, now_ms);
    (void)logger_button_poll(&app->button, now_ms);
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
            logger_step_log_wait_h10(app, now_ms);
            break;

        case LOGGER_RUNTIME_LOG_STOPPING:
            logger_step_log_stopping(app, now_ms);
            break;

        case LOGGER_RUNTIME_IDLE_WAITING_FOR_CHARGER:
        case LOGGER_RUNTIME_IDLE_UPLOAD_COMPLETE:
            logger_step_idle_common(app, now_ms);
            break;

        case LOGGER_RUNTIME_UPLOAD_PREP:
            logger_app_state_transition(&app->runtime, LOGGER_RUNTIME_IDLE_UPLOAD_COMPLETE, "upload_not_implemented", now_ms);
            break;

        case LOGGER_RUNTIME_UPLOAD_RUNNING:
            logger_app_state_transition(&app->runtime, LOGGER_RUNTIME_IDLE_UPLOAD_COMPLETE, "upload_not_implemented", now_ms);
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
