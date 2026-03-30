#include "logger/service_cli.h"

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"

#include "board_config.h"
#include "logger/app_main.h"
#include "logger/json.h"
#include "logger/queue.h"
#include "logger/upload.h"

#ifndef LOGGER_FIRMWARE_VERSION
#define LOGGER_FIRMWARE_VERSION "0.1.0-dev"
#endif

#ifndef LOGGER_BUILD_ID
#define LOGGER_BUILD_ID "logger-fw-dev"
#endif

static bool logger_string_present(const char *value) {
    return value != NULL && value[0] != '\0';
}

static const char *logger_now_utc_or_null(const logger_app_t *app) {
    return app->clock.now_utc[0] != '\0' ? app->clock.now_utc : NULL;
}

#define logger_json_write_string_or_null(value) logger_json_fwrite_string_or_null(stdout, (value))

static void logger_json_begin_success(const char *command, const char *generated_at_utc) {
    fputs("{\"schema_version\":1,\"command\":", stdout);
    logger_json_write_string_or_null(command);
    fputs(",\"ok\":true,\"generated_at_utc\":", stdout);
    logger_json_write_string_or_null(generated_at_utc);
    fputs(",\"payload\":", stdout);
}

static void logger_json_begin_error(
    const char *command,
    const char *generated_at_utc,
    const char *code,
    const char *message) {
    fputs("{\"schema_version\":1,\"command\":", stdout);
    logger_json_write_string_or_null(command);
    fputs(",\"ok\":false,\"generated_at_utc\":", stdout);
    logger_json_write_string_or_null(generated_at_utc);
    fputs(",\"error\":{\"code\":", stdout);
    logger_json_write_string_or_null(code);
    fputs(",\"message\":", stdout);
    logger_json_write_string_or_null(message);
    fputs("}}\n", stdout);
    fflush(stdout);
}

static void logger_json_end_success(void) {
    fputs("}\n", stdout);
    fflush(stdout);
}

static void logger_write_required_missing_array(const logger_app_t *app) {
    bool first = true;
    if (!logger_string_present(app->persisted.config.bound_h10_address)) {
        logger_json_write_string_or_null(first ? "bound_h10_address" : NULL);
        first = false;
    }
    if (!logger_string_present(app->persisted.config.logger_id)) {
        if (!first) {
            fputs(",", stdout);
        }
        logger_json_write_string_or_null("logger_id");
        first = false;
    }
    if (!logger_string_present(app->persisted.config.subject_id)) {
        if (!first) {
            fputs(",", stdout);
        }
        logger_json_write_string_or_null("subject_id");
        first = false;
    }
    if (!logger_string_present(app->persisted.config.timezone)) {
        if (!first) {
            fputs(",", stdout);
        }
        logger_json_write_string_or_null("timezone");
    }
}

static void logger_write_required_present_array(const logger_app_t *app) {
    bool first = true;
    if (logger_string_present(app->persisted.config.bound_h10_address)) {
        logger_json_write_string_or_null(first ? "bound_h10_address" : NULL);
        first = false;
    }
    if (logger_string_present(app->persisted.config.logger_id)) {
        if (!first) {
            fputs(",", stdout);
        }
        logger_json_write_string_or_null("logger_id");
        first = false;
    }
    if (logger_string_present(app->persisted.config.subject_id)) {
        if (!first) {
            fputs(",", stdout);
        }
        logger_json_write_string_or_null("subject_id");
        first = false;
    }
    if (logger_string_present(app->persisted.config.timezone)) {
        if (!first) {
            fputs(",", stdout);
        }
        logger_json_write_string_or_null("timezone");
    }
}

static void logger_write_optional_present_array(const logger_app_t *app) {
    bool first = true;
    if (logger_string_present(app->persisted.config.upload_token)) {
        logger_json_write_string_or_null(first ? "upload_auth" : NULL);
        first = false;
    }
    if (logger_string_present(app->persisted.config.upload_url)) {
        if (!first) {
            fputs(",", stdout);
        }
        logger_json_write_string_or_null("upload_url");
        first = false;
    }
    if (logger_config_wifi_configured(&app->persisted.config)) {
        if (!first) {
            fputs(",", stdout);
        }
        logger_json_write_string_or_null("wifi_networks");
    }
}

static void logger_write_warnings_array(const logger_app_t *app) {
    bool first = true;
    if (!app->clock.valid) {
        logger_json_write_string_or_null(first ? "clock_invalid" : NULL);
        first = false;
    }
    if (!logger_config_upload_configured(&app->persisted.config)) {
        if (!first) {
            fputs(",", stdout);
        }
        logger_json_write_string_or_null("upload_not_configured");
        first = false;
    }
    if (!logger_config_wifi_configured(&app->persisted.config)) {
        if (!first) {
            fputs(",", stdout);
        }
        logger_json_write_string_or_null("wifi_not_configured");
    }
}

static bool logger_cli_is_service_mode(const logger_app_t *app) {
    return app->runtime.current_state == LOGGER_RUNTIME_SERVICE;
}

static bool logger_cli_is_logging_mode(const logger_app_t *app) {
    return logger_runtime_state_is_logging(app->runtime.current_state);
}

static bool logger_cli_is_upload_mode(const logger_app_t *app) {
    return logger_runtime_state_is_upload(app->runtime.current_state);
}

static logger_fault_code_t logger_cli_storage_fault_code(const logger_app_t *app) {
    if (!app->storage.card_present || !app->storage.mounted || !app->storage.writable ||
        !app->storage.logger_root_ready || strcmp(app->storage.filesystem, "fat32") != 0) {
        return LOGGER_FAULT_SD_MISSING_OR_UNWRITABLE;
    }
    if (!app->storage.reserve_ok) {
        return LOGGER_FAULT_SD_LOW_SPACE_RESERVE_UNMET;
    }
    return LOGGER_FAULT_NONE;
}

static bool logger_cli_fault_condition_still_present(const logger_app_t *app) {
    switch (app->persisted.current_fault_code) {
        case LOGGER_FAULT_CONFIG_INCOMPLETE:
            return !logger_config_normal_logging_ready(&app->persisted.config);
        case LOGGER_FAULT_CLOCK_INVALID:
            return !app->clock.valid;
        case LOGGER_FAULT_LOW_BATTERY_BLOCKED_START:
            return logger_battery_low_start_blocked(&app->battery);
        case LOGGER_FAULT_CRITICAL_LOW_BATTERY_STOPPED:
            return logger_battery_is_critical(&app->battery);
        case LOGGER_FAULT_SD_MISSING_OR_UNWRITABLE:
        case LOGGER_FAULT_SD_LOW_SPACE_RESERVE_UNMET:
            return logger_cli_storage_fault_code(app) == app->persisted.current_fault_code;
        case LOGGER_FAULT_SD_WRITE_FAILED:
        case LOGGER_FAULT_UPLOAD_BLOCKED_MIN_FIRMWARE:
            return true;
        case LOGGER_FAULT_NONE:
        default:
            return false;
    }
}

static void logger_write_storage_card_identity(const logger_app_t *app) {
    if (!logger_string_present(app->storage.manufacturer_id)) {
        fputs("null", stdout);
        return;
    }
    fputs("{\"manufacturer_id\":", stdout);
    logger_json_write_string_or_null(app->storage.manufacturer_id);
    fputs(",\"oem_id\":", stdout);
    logger_json_write_string_or_null(app->storage.oem_id);
    fputs(",\"product_name\":", stdout);
    logger_json_write_string_or_null(app->storage.product_name);
    fputs(",\"revision\":", stdout);
    logger_json_write_string_or_null(app->storage.revision);
    fputs(",\"serial_number\":", stdout);
    logger_json_write_string_or_null(app->storage.serial_number);
    fputs("}", stdout);
}

static void logger_write_status_payload(const logger_app_t *app) {
    char study_day_local[11] = {0};
    logger_upload_queue_t queue;
    logger_upload_queue_summary_t queue_summary;
    logger_upload_queue_init(&queue);
    logger_upload_queue_summary_init(&queue_summary);
    if (logger_upload_queue_load(&queue) || logger_upload_queue_scan(&queue, NULL, logger_now_utc_or_null(app))) {
        logger_upload_queue_compute_summary(&queue, &queue_summary);
    }
    const bool have_study_day = app->session.active
        ? true
        : (logger_runtime_state_is_logging(app->runtime.current_state) &&
           logger_clock_derive_study_day_local_observed(&app->clock, app->persisted.config.timezone, study_day_local));

    fputs("{\"mode\":", stdout);
    logger_json_write_string_or_null(logger_mode_name(&app->runtime));
    fputs(",\"runtime_state\":", stdout);
    logger_json_write_string_or_null(logger_runtime_state_name(app->runtime.current_state));

    fputs(",\"identity\":{\"hardware_id\":", stdout);
    logger_json_write_string_or_null(app->hardware_id);
    fputs(",\"logger_id\":", stdout);
    logger_json_write_string_or_null(app->persisted.config.logger_id);
    fputs(",\"subject_id\":", stdout);
    logger_json_write_string_or_null(app->persisted.config.subject_id);
    fputs("}", stdout);

    fputs(",\"provisioning\":{\"normal_logging_ready\":", stdout);
    fputs(logger_config_normal_logging_ready(&app->persisted.config) ? "true" : "false", stdout);
    fputs(",\"required_missing\":[", stdout);
    logger_write_required_missing_array(app);
    fputs("],\"warnings\":[", stdout);
    logger_write_warnings_array(app);
    fputs("]}", stdout);

    fputs(",\"fault\":{\"latched\":", stdout);
    fputs(app->persisted.current_fault_code != LOGGER_FAULT_NONE ? "true" : "false", stdout);
    fputs(",\"current_code\":", stdout);
    logger_json_write_string_or_null(logger_fault_code_name(app->persisted.current_fault_code));
    fputs(",\"last_cleared_code\":", stdout);
    logger_json_write_string_or_null(logger_fault_code_name(app->persisted.last_cleared_fault_code));
    fputs("}", stdout);

    fputs(",\"battery\":{\"voltage_mv\":", stdout);
    printf("%u", (unsigned)app->battery.voltage_mv);
    fputs(",\"estimate_pct\":", stdout);
    printf("%d", app->battery.estimate_pct);
    fputs(",\"vbus_present\":", stdout);
    fputs(app->battery.vbus_present ? "true" : "false", stdout);
    fputs(",\"critical_stop_mv\":", stdout);
    printf("%u", (unsigned)LOGGER_BATTERY_CRITICAL_STOP_MV);
    fputs(",\"low_start_mv\":", stdout);
    printf("%u", (unsigned)LOGGER_BATTERY_LOW_START_BLOCK_MV);
    fputs(",\"off_charger_upload_mv\":", stdout);
    printf("%u", (unsigned)LOGGER_BATTERY_OFF_CHARGER_UPLOAD_MIN_MV);
    fputs("}", stdout);

    fputs(",\"storage\":{\"sd_present\":", stdout);
    fputs(app->storage.card_present ? "true" : "false", stdout);
    fputs(",\"filesystem\":", stdout);
    logger_json_write_string_or_null(logger_string_present(app->storage.filesystem) ? app->storage.filesystem : NULL);
    fputs(",\"free_bytes\":", stdout);
    if (app->storage.mounted) {
        printf("%llu", (unsigned long long)app->storage.free_bytes);
    } else {
        fputs("null", stdout);
    }
    fputs(",\"reserve_bytes\":", stdout);
    printf("%lu", (unsigned long)LOGGER_SD_MIN_FREE_RESERVE_BYTES);
    fputs(",\"card_identity\":", stdout);
    logger_write_storage_card_identity(app);
    fputs("}", stdout);

    fputs(",\"h10\":{\"bound_address\":", stdout);
    logger_json_write_string_or_null(app->persisted.config.bound_h10_address);
    fputs(",\"connected\":false,\"encrypted\":false,\"bonded\":false,\"last_seen_address\":null,\"battery_percent\":null}", stdout);

    fputs(",\"session\":{\"active\":", stdout);
    fputs(app->session.active ? "true" : "false", stdout);
    fputs(",\"session_id\":", stdout);
    logger_json_write_string_or_null(app->session.active ? app->session.session_id : NULL);
    fputs(",\"study_day_local\":", stdout);
    logger_json_write_string_or_null(app->session.active ? app->session.study_day_local : (have_study_day ? study_day_local : NULL));
    fputs(",\"span_id\":", stdout);
    logger_json_write_string_or_null((app->session.active && app->session.span_active) ? app->session.current_span_id : NULL);
    fputs(",\"quarantined\":", stdout);
    fputs(app->session.active && app->session.quarantined ? "true" : "false", stdout);
    fputs(",\"clock_state\":", stdout);
    logger_json_write_string_or_null(app->session.active ? app->session.clock_state : logger_clock_state_name(&app->clock));
    fputs(",\"journal_size_bytes\":", stdout);
    if (app->session.active) {
        printf("%llu", (unsigned long long)app->session.journal_size_bytes);
    } else {
        fputs("null", stdout);
    }
    fputs("}", stdout);

    fputs(",\"upload_queue\":{\"pending_count\":", stdout);
    printf("%lu", (unsigned long)queue_summary.pending_count);
    fputs(",\"blocked_count\":", stdout);
    printf("%lu", (unsigned long)queue_summary.blocked_count);
    fputs(",\"oldest_pending_study_day\":", stdout);
    logger_json_write_string_or_null(logger_string_present(queue_summary.oldest_pending_study_day) ? queue_summary.oldest_pending_study_day : NULL);
    fputs(",\"last_failure_class\":", stdout);
    logger_json_write_string_or_null(logger_string_present(queue_summary.last_failure_class) ? queue_summary.last_failure_class : NULL);
    fputs("}", stdout);
    fputs(",\"last_day_outcome\":{\"study_day_local\":null,\"kind\":null,\"reason\":null}", stdout);
    fputs(",\"firmware\":{\"version\":", stdout);
    logger_json_write_string_or_null(LOGGER_FIRMWARE_VERSION);
    fputs(",\"build_id\":", stdout);
    logger_json_write_string_or_null(LOGGER_BUILD_ID);
    fputs("}}", stdout);
}

static void logger_handle_status_json(logger_app_t *app) {
    logger_json_begin_success("status", logger_now_utc_or_null(app));
    logger_write_status_payload(app);
    logger_json_end_success();
}

static void logger_handle_provisioning_status_json(logger_app_t *app) {
    logger_json_begin_success("provisioning-status", logger_now_utc_or_null(app));
    fputs("{\"normal_logging_ready\":", stdout);
    fputs(logger_config_normal_logging_ready(&app->persisted.config) ? "true" : "false", stdout);
    fputs(",\"required_present\":[", stdout);
    logger_write_required_present_array(app);
    fputs("],\"required_missing\":[", stdout);
    logger_write_required_missing_array(app);
    fputs("],\"optional_present\":[", stdout);
    logger_write_optional_present_array(app);
    fputs("],\"warnings\":[", stdout);
    logger_write_warnings_array(app);
    fputs("]}", stdout);
    logger_json_end_success();
}

static void logger_handle_queue_json(logger_app_t *app) {
    logger_upload_queue_t queue;
    logger_upload_queue_init(&queue);
    if (!logger_upload_queue_load(&queue)) {
        (void)logger_upload_queue_scan(&queue, NULL, logger_now_utc_or_null(app));
    }

    logger_json_begin_success("queue", logger_now_utc_or_null(app));
    fputs("{\"schema_source\":\"upload_queue.json\",\"updated_at_utc\":", stdout);
    logger_json_write_string_or_null(logger_string_present(queue.updated_at_utc) ? queue.updated_at_utc : NULL);
    fputs(",\"sessions\":[", stdout);
    for (size_t i = 0u; i < queue.session_count; ++i) {
        const logger_upload_queue_entry_t *entry = &queue.sessions[i];
        if (i != 0u) {
            fputs(",", stdout);
        }
        fputs("{\"session_id\":", stdout);
        logger_json_write_string_or_null(entry->session_id);
        fputs(",\"study_day_local\":", stdout);
        logger_json_write_string_or_null(entry->study_day_local);
        fputs(",\"dir_name\":", stdout);
        logger_json_write_string_or_null(entry->dir_name);
        fputs(",\"session_start_utc\":", stdout);
        logger_json_write_string_or_null(entry->session_start_utc);
        fputs(",\"session_end_utc\":", stdout);
        logger_json_write_string_or_null(entry->session_end_utc);
        fputs(",\"bundle_sha256\":", stdout);
        logger_json_write_string_or_null(entry->bundle_sha256);
        fputs(",\"bundle_size_bytes\":", stdout);
        printf("%llu", (unsigned long long)entry->bundle_size_bytes);
        fputs(",\"status\":", stdout);
        logger_json_write_string_or_null(entry->status);
        fputs(",\"quarantined\":", stdout);
        fputs(entry->quarantined ? "true" : "false", stdout);
        fputs(",\"attempt_count\":", stdout);
        printf("%lu", (unsigned long)entry->attempt_count);
        fputs(",\"last_attempt_utc\":", stdout);
        logger_json_write_string_or_null(logger_string_present(entry->last_attempt_utc) ? entry->last_attempt_utc : NULL);
        fputs(",\"last_failure_class\":", stdout);
        logger_json_write_string_or_null(logger_string_present(entry->last_failure_class) ? entry->last_failure_class : NULL);
        fputs(",\"verified_upload_utc\":", stdout);
        logger_json_write_string_or_null(logger_string_present(entry->verified_upload_utc) ? entry->verified_upload_utc : NULL);
        fputs(",\"receipt_id\":", stdout);
        logger_json_write_string_or_null(logger_string_present(entry->receipt_id) ? entry->receipt_id : NULL);
        fputs("}", stdout);
    }
    fputs("]}", stdout);
    logger_json_end_success();
}

static void logger_handle_system_log_export_json(logger_app_t *app) {
    logger_json_begin_success("system-log export", logger_now_utc_or_null(app));
    fputs("{\"schema_version\":1,\"exported_at_utc\":", stdout);
    logger_json_write_string_or_null(logger_now_utc_or_null(app));
    fputs(",\"events\":[", stdout);

    for (uint32_t i = 0u; i < logger_system_log_count(&app->system_log); ++i) {
        logger_system_log_event_t event;
        if (!logger_system_log_read_event(i, &event)) {
            continue;
        }
        if (i != 0u) {
            fputs(",", stdout);
        }
        fputs("{\"event_seq\":", stdout);
        printf("%lu", (unsigned long)event.event_seq);
        fputs(",\"utc\":", stdout);
        logger_json_write_string_or_null(logger_string_present(event.utc) ? event.utc : NULL);
        fputs(",\"boot_counter\":", stdout);
        printf("%lu", (unsigned long)event.boot_counter);
        fputs(",\"kind\":", stdout);
        logger_json_write_string_or_null(event.kind);
        fputs(",\"severity\":", stdout);
        logger_json_write_string_or_null(logger_system_log_severity_name(event.severity));
        fputs(",\"details\":", stdout);
        fputs(logger_string_present(event.details_json) ? event.details_json : "{}", stdout);
        fputs("}", stdout);
    }

    fputs("]}", stdout);
    logger_json_end_success();
}

static void logger_handle_config_export_json(logger_app_t *app) {
    logger_json_begin_success("config export", logger_now_utc_or_null(app));
    fputs("{\"schema_version\":1,\"exported_at_utc\":", stdout);
    logger_json_write_string_or_null(logger_now_utc_or_null(app));
    fputs(",\"hardware_id\":", stdout);
    logger_json_write_string_or_null(app->hardware_id);
    fputs(",\"secrets_included\":false,\"identity\":{\"logger_id\":", stdout);
    logger_json_write_string_or_null(app->persisted.config.logger_id);
    fputs(",\"subject_id\":", stdout);
    logger_json_write_string_or_null(app->persisted.config.subject_id);
    fputs("},\"recording\":{\"bound_h10_address\":", stdout);
    logger_json_write_string_or_null(app->persisted.config.bound_h10_address);
    fputs(",\"study_day_rollover_local\":\"04:00:00\",\"overnight_upload_window_start_local\":\"22:00:00\",\"overnight_upload_window_end_local\":\"06:00:00\"}", stdout);
    fputs(",\"time\":{\"timezone\":", stdout);
    logger_json_write_string_or_null(app->persisted.config.timezone);
    fputs("},\"battery_policy\":{\"critical_stop_voltage_v\":3.5,\"low_start_voltage_v\":3.65,\"off_charger_upload_voltage_v\":3.85}", stdout);
    fputs(",\"wifi\":{\"allowed_ssids\":[", stdout);
    if (logger_string_present(app->persisted.config.wifi_ssid)) {
        logger_json_write_string_or_null(app->persisted.config.wifi_ssid);
    }
    fputs("],\"networks\":[", stdout);
    if (logger_string_present(app->persisted.config.wifi_ssid)) {
        fputs("{\"ssid\":", stdout);
        logger_json_write_string_or_null(app->persisted.config.wifi_ssid);
        fputs(",\"psk_present\":", stdout);
        fputs(logger_string_present(app->persisted.config.wifi_psk) ? "true" : "false", stdout);
        fputs("}", stdout);
    }
    fputs("]},\"upload\":{\"enabled\":", stdout);
    fputs(logger_string_present(app->persisted.config.upload_url) ? "true" : "false", stdout);
    fputs(",\"url\":", stdout);
    logger_json_write_string_or_null(app->persisted.config.upload_url);
    fputs(",\"auth\":{\"type\":", stdout);
    logger_json_write_string_or_null(logger_string_present(app->persisted.config.upload_token) ? "bearer" : "none");
    fputs(",\"token_present\":", stdout);
    fputs(logger_string_present(app->persisted.config.upload_token) ? "true" : "false", stdout);
    fputs("}}}", stdout);
    logger_json_end_success();
}

static void logger_handle_clock_status_json(logger_app_t *app) {
    logger_json_begin_success("clock status", logger_now_utc_or_null(app));
    fputs("{\"rtc_present\":", stdout);
    fputs(app->clock.rtc_present ? "true" : "false", stdout);
    fputs(",\"valid\":", stdout);
    fputs(app->clock.valid ? "true" : "false", stdout);
    fputs(",\"lost_power\":", stdout);
    fputs(app->clock.lost_power ? "true" : "false", stdout);
    fputs(",\"battery_low\":", stdout);
    fputs(app->clock.battery_low ? "true" : "false", stdout);
    fputs(",\"state\":", stdout);
    logger_json_write_string_or_null(logger_clock_state_name(&app->clock));
    fputs(",\"now_utc\":", stdout);
    logger_json_write_string_or_null(logger_now_utc_or_null(app));
    fputs(",\"timezone\":", stdout);
    logger_json_write_string_or_null(app->persisted.config.timezone);
    fputs("}", stdout);
    logger_json_end_success();
}

static void logger_handle_preflight_json(logger_app_t *app) {
    if (logger_cli_is_logging_mode(app)) {
        logger_json_begin_error("preflight", logger_now_utc_or_null(app), "busy_logging", "preflight is not permitted while logging");
        return;
    }
    if (logger_cli_is_upload_mode(app)) {
        logger_json_begin_error("preflight", logger_now_utc_or_null(app), "not_permitted_in_mode", "preflight is not permitted during upload");
        return;
    }

    const char *rtc_result = !app->clock.rtc_present ? "fail" : (app->clock.valid ? "pass" : "warn");
    const char *storage_result = logger_storage_ready_for_logging(&app->storage) ? "pass" : "fail";
    const char *battery_result = app->battery.initialized ? "pass" : "fail";
    const char *prov_result = logger_config_normal_logging_ready(&app->persisted.config) ? "pass" : "fail";
    const char *h10_scan_result = logger_string_present(app->persisted.config.bound_h10_address) ? "warn" : "fail";

    const char *overall = "pass";
    if (strcmp(rtc_result, "fail") == 0 || strcmp(storage_result, "fail") == 0 ||
        strcmp(battery_result, "fail") == 0 || strcmp(prov_result, "fail") == 0 ||
        strcmp(h10_scan_result, "fail") == 0) {
        overall = "fail";
    } else if (strcmp(rtc_result, "warn") == 0 || strcmp(h10_scan_result, "warn") == 0) {
        overall = "warn";
    }

    logger_json_begin_success("preflight", logger_now_utc_or_null(app));
    fputs("{\"overall_result\":", stdout);
    logger_json_write_string_or_null(overall);
    fputs(",\"checks\":[", stdout);
    fputs("{\"name\":\"rtc\",\"result\":", stdout);
    logger_json_write_string_or_null(rtc_result);
    fputs(",\"details\":{\"rtc_present\":", stdout);
    fputs(app->clock.rtc_present ? "true" : "false", stdout);
    fputs(",\"valid\":", stdout);
    fputs(app->clock.valid ? "true" : "false", stdout);
    fputs(",\"lost_power\":", stdout);
    fputs(app->clock.lost_power ? "true" : "false", stdout);
    fputs("}}", stdout);

    fputs(",{\"name\":\"storage\",\"result\":", stdout);
    logger_json_write_string_or_null(storage_result);
    fputs(",\"details\":{\"mounted\":", stdout);
    fputs(app->storage.mounted ? "true" : "false", stdout);
    fputs(",\"filesystem\":", stdout);
    logger_json_write_string_or_null(logger_string_present(app->storage.filesystem) ? app->storage.filesystem : NULL);
    fputs(",\"free_bytes\":", stdout);
    if (app->storage.mounted) {
        printf("%llu", (unsigned long long)app->storage.free_bytes);
    } else {
        fputs("null", stdout);
    }
    fputs(",\"reserve_ok\":", stdout);
    fputs(app->storage.reserve_ok ? "true" : "false", stdout);
    fputs("}}", stdout);

    fputs(",{\"name\":\"battery_sense\",\"result\":", stdout);
    logger_json_write_string_or_null(battery_result);
    fputs(",\"details\":{\"voltage_mv\":", stdout);
    printf("%u", (unsigned)app->battery.voltage_mv);
    fputs(",\"vbus_present\":", stdout);
    fputs(app->battery.vbus_present ? "true" : "false", stdout);
    fputs("}}", stdout);

    fputs(",{\"name\":\"provisioning\",\"result\":", stdout);
    logger_json_write_string_or_null(prov_result);
    fputs(",\"details\":{\"normal_logging_ready\":", stdout);
    fputs(logger_config_normal_logging_ready(&app->persisted.config) ? "true" : "false", stdout);
    fputs("}}", stdout);

    fputs(",{\"name\":\"bound_h10_scan\",\"result\":", stdout);
    logger_json_write_string_or_null(h10_scan_result);
    fputs(",\"details\":{\"implemented\":false,\"bound_address\":", stdout);
    logger_json_write_string_or_null(app->persisted.config.bound_h10_address);
    fputs("}}]}", stdout);
    logger_json_end_success();
}

static void logger_handle_net_test_json(logger_app_t *app) {
    if (logger_cli_is_logging_mode(app)) {
        logger_json_begin_error("net-test", logger_now_utc_or_null(app), "busy_logging", "net-test is not permitted while logging");
        return;
    }
    if (!logger_cli_is_service_mode(app)) {
        logger_json_begin_error("net-test", logger_now_utc_or_null(app), "not_permitted_in_mode", "net-test is only allowed in service mode");
        return;
    }

    logger_upload_net_test_result_t result;
    const bool ok = logger_upload_net_test(&app->persisted.config, &result);

    logger_json_begin_success("net-test", logger_now_utc_or_null(app));
    fputs("{\"wifi_join\":{\"result\":", stdout);
    logger_json_write_string_or_null(result.wifi_join_result);
    fputs(",\"details\":{\"message\":", stdout);
    logger_json_write_string_or_null(result.wifi_join_details);
    fputs("}},\"dns\":{\"result\":", stdout);
    logger_json_write_string_or_null(result.dns_result);
    fputs(",\"details\":{\"message\":", stdout);
    logger_json_write_string_or_null(result.dns_details);
    fputs("}},\"tls\":{\"result\":", stdout);
    logger_json_write_string_or_null(result.tls_result);
    fputs(",\"details\":{\"message\":", stdout);
    logger_json_write_string_or_null(result.tls_details);
    fputs("}},\"upload_endpoint_reachable\":{\"result\":", stdout);
    logger_json_write_string_or_null(result.upload_endpoint_reachable_result);
    fputs(",\"details\":{\"message\":", stdout);
    logger_json_write_string_or_null(result.upload_endpoint_reachable_details);
    fputs(",\"ok\":", stdout);
    fputs(ok ? "true" : "false", stdout);
    fputs("}}}", stdout);
    logger_json_end_success();
}

static void logger_handle_service_unlock(logger_service_cli_t *cli, logger_app_t *app, uint32_t now_ms) {
    if (logger_cli_is_logging_mode(app)) {
        logger_json_begin_error("service unlock", logger_now_utc_or_null(app), "busy_logging", "service unlock is not permitted while logging");
        return;
    }
    if (!logger_cli_is_service_mode(app)) {
        logger_json_begin_error("service unlock", logger_now_utc_or_null(app), "not_permitted_in_mode", "service unlock is only allowed in service mode");
        return;
    }

    cli->unlocked = true;
    cli->unlock_deadline_mono_ms = now_ms + 60000u;
    (void)logger_system_log_append(
        &app->system_log,
        logger_now_utc_or_null(app),
        "service_unlock",
        LOGGER_SYSTEM_LOG_SEVERITY_INFO,
        "{}"
    );

    logger_json_begin_success("service unlock", logger_now_utc_or_null(app));
    fputs("{\"unlocked\":true,\"expires_at_utc\":", stdout);
    logger_json_write_string_or_null(logger_now_utc_or_null(app));
    fputs(",\"ttl_seconds\":60}", stdout);
    logger_json_end_success();
}

static void logger_handle_fault_clear(logger_app_t *app) {
    if (logger_cli_is_logging_mode(app)) {
        logger_json_begin_error("fault clear", logger_now_utc_or_null(app), "busy_logging", "fault clear is not permitted while logging");
        return;
    }
    if (logger_cli_is_upload_mode(app)) {
        logger_json_begin_error("fault clear", logger_now_utc_or_null(app), "not_permitted_in_mode", "fault clear is not permitted during upload");
        return;
    }

    const logger_fault_code_t previous = app->persisted.current_fault_code;
    if (previous == LOGGER_FAULT_NONE) {
        logger_json_begin_success("fault clear", logger_now_utc_or_null(app));
        fputs("{\"cleared\":false,\"previous_code\":null}", stdout);
        logger_json_end_success();
        return;
    }
    if (logger_cli_fault_condition_still_present(app)) {
        logger_json_begin_error("fault clear", logger_now_utc_or_null(app), "condition_still_present", "fault condition is still present");
        return;
    }

    app->persisted.last_cleared_fault_code = previous;
    app->persisted.current_fault_code = LOGGER_FAULT_NONE;
    (void)logger_config_store_save(&app->persisted);
    (void)logger_system_log_append(
        &app->system_log,
        logger_now_utc_or_null(app),
        "fault_cleared",
        LOGGER_SYSTEM_LOG_SEVERITY_INFO,
        "{}"
    );

    logger_json_begin_success("fault clear", logger_now_utc_or_null(app));
    fputs("{\"cleared\":true,\"previous_code\":", stdout);
    logger_json_write_string_or_null(logger_fault_code_name(previous));
    fputs("}", stdout);
    logger_json_end_success();
}

static void logger_handle_factory_reset(logger_service_cli_t *cli, logger_app_t *app) {
    if (logger_cli_is_logging_mode(app)) {
        logger_json_begin_error("factory-reset", logger_now_utc_or_null(app), "busy_logging", "factory reset is not permitted while logging");
        return;
    }
    if (!logger_cli_is_service_mode(app)) {
        logger_json_begin_error("factory-reset", logger_now_utc_or_null(app), "not_permitted_in_mode", "factory reset is only allowed in service mode");
        return;
    }
    if (!logger_service_cli_is_unlocked(cli, to_ms_since_boot(get_absolute_time()))) {
        logger_json_begin_error("factory-reset", logger_now_utc_or_null(app), "service_locked", "service unlock is required before factory reset");
        return;
    }

    (void)logger_system_log_append(
        &app->system_log,
        logger_now_utc_or_null(app),
        "factory_reset",
        LOGGER_SYSTEM_LOG_SEVERITY_WARN,
        "{\"source\":\"service_cli\"}");
    (void)logger_config_store_factory_reset(&app->persisted);
    cli->unlocked = false;
    app->reboot_pending = true;

    logger_json_begin_success("factory-reset", logger_now_utc_or_null(app));
    fputs("{\"factory_reset\":true,\"will_reboot\":true}", stdout);
    logger_json_end_success();
}

static void logger_handle_clock_set(logger_service_cli_t *cli, logger_app_t *app, const char *value) {
    if (logger_cli_is_logging_mode(app)) {
        logger_json_begin_error("clock set", logger_now_utc_or_null(app), "busy_logging", "clock set is not permitted while logging");
        return;
    }
    if (!logger_cli_is_service_mode(app)) {
        logger_json_begin_error("clock set", logger_now_utc_or_null(app), "not_permitted_in_mode", "clock set is only allowed in service mode");
        return;
    }
    if (!logger_service_cli_is_unlocked(cli, to_ms_since_boot(get_absolute_time()))) {
        logger_json_begin_error("clock set", logger_now_utc_or_null(app), "service_locked", "service unlock is required before clock set");
        return;
    }
    if (!logger_clock_set_utc(value, &app->clock)) {
        logger_json_begin_error("clock set", logger_now_utc_or_null(app), "invalid_config", "invalid RFC3339 UTC timestamp");
        return;
    }
    (void)logger_system_log_append(
        &app->system_log,
        logger_now_utc_or_null(app),
        "clock_set",
        LOGGER_SYSTEM_LOG_SEVERITY_INFO,
        "{}"
    );

    logger_json_begin_success("clock set", logger_now_utc_or_null(app));
    fputs("{\"applied\":true,\"now_utc\":", stdout);
    logger_json_write_string_or_null(logger_now_utc_or_null(app));
    fputs("}", stdout);
    logger_json_end_success();
}

static void logger_handle_debug_config_set(logger_service_cli_t *cli, logger_app_t *app, const char *args) {
    char field[48];
    char value[256];
    field[0] = '\0';
    value[0] = '\0';
    const int matched = sscanf(args, "%47s %255[^\n]", field, value);
    if (matched < 2) {
        logger_json_begin_error("debug config set", logger_now_utc_or_null(app), "invalid_config", "expected: debug config set <field> <value>");
        return;
    }

    const bool upload_debug_field = strcmp(field, "wifi_ssid") == 0 ||
                                    strcmp(field, "wifi_psk") == 0 ||
                                    strcmp(field, "upload_url") == 0 ||
                                    strcmp(field, "upload_token") == 0;
    const bool allow_in_log_wait_h10 = upload_debug_field && app->runtime.current_state == LOGGER_RUNTIME_LOG_WAIT_H10;

    if (logger_cli_is_logging_mode(app) && !allow_in_log_wait_h10) {
        logger_json_begin_error("debug config set", logger_now_utc_or_null(app), "busy_logging", "config mutation is not permitted while logging");
        return;
    }
    if (!logger_cli_is_service_mode(app) && !allow_in_log_wait_h10) {
        logger_json_begin_error("debug config set", logger_now_utc_or_null(app), "not_permitted_in_mode", "debug config set is only allowed in service mode");
        return;
    }
    if (logger_cli_is_service_mode(app) && !logger_service_cli_is_unlocked(cli, to_ms_since_boot(get_absolute_time()))) {
        logger_json_begin_error("debug config set", logger_now_utc_or_null(app), "service_locked", "service unlock is required before debug config set");
        return;
    }

    bool ok = false;
    bool bond_cleared = false;
    if (strcmp(field, "logger_id") == 0) {
        ok = logger_config_set_logger_id(&app->persisted, value);
    } else if (strcmp(field, "subject_id") == 0) {
        ok = logger_config_set_subject_id(&app->persisted, value);
    } else if (strcmp(field, "bound_h10_address") == 0) {
        ok = logger_config_set_bound_h10_address(&app->persisted, value, &bond_cleared);
    } else if (strcmp(field, "timezone") == 0) {
        ok = logger_config_set_timezone(&app->persisted, value);
    } else if (strcmp(field, "wifi_ssid") == 0) {
        ok = logger_config_set_wifi_ssid(&app->persisted, value);
    } else if (strcmp(field, "wifi_psk") == 0) {
        ok = logger_config_set_wifi_psk(&app->persisted, value);
    } else if (strcmp(field, "upload_url") == 0) {
        ok = logger_config_set_upload_url(&app->persisted, value);
    } else if (strcmp(field, "upload_token") == 0) {
        ok = logger_config_set_upload_token(&app->persisted, value);
    } else {
        logger_json_begin_error("debug config set", logger_now_utc_or_null(app), "invalid_config", "unknown debug config field");
        return;
    }

    if (!ok) {
        logger_json_begin_error("debug config set", logger_now_utc_or_null(app), "invalid_config", "failed to apply debug config field");
        return;
    }

    char details[96];
    snprintf(details, sizeof(details), "{\"field\":\"%s\"}", field);
    (void)logger_system_log_append(
        &app->system_log,
        logger_now_utc_or_null(app),
        "config_changed",
        LOGGER_SYSTEM_LOG_SEVERITY_INFO,
        details);

    logger_json_begin_success("debug config set", logger_now_utc_or_null(app));
    fputs("{\"applied\":true,\"field\":", stdout);
    logger_json_write_string_or_null(field);
    fputs(",\"normal_logging_ready\":", stdout);
    fputs(logger_config_normal_logging_ready(&app->persisted.config) ? "true" : "false", stdout);
    fputs(",\"bond_cleared\":", stdout);
    fputs(bond_cleared ? "true" : "false", stdout);
    fputs("}", stdout);
    logger_json_end_success();
}

static void logger_handle_debug_config_clear(logger_service_cli_t *cli, logger_app_t *app, const char *args) {
    if (!logger_cli_is_service_mode(app)) {
        logger_json_begin_error("debug config clear", logger_now_utc_or_null(app), "not_permitted_in_mode", "debug config clear is only allowed in service mode");
        return;
    }
    if (!logger_service_cli_is_unlocked(cli, to_ms_since_boot(get_absolute_time()))) {
        logger_json_begin_error("debug config clear", logger_now_utc_or_null(app), "service_locked", "service unlock is required before debug config clear");
        return;
    }
    if (strcmp(args, "upload") != 0) {
        logger_json_begin_error("debug config clear", logger_now_utc_or_null(app), "invalid_config", "only 'debug config clear upload' is supported");
        return;
    }

    (void)logger_config_clear_upload(&app->persisted);
    (void)logger_system_log_append(
        &app->system_log,
        logger_now_utc_or_null(app),
        "config_changed",
        LOGGER_SYSTEM_LOG_SEVERITY_INFO,
        "{\"field\":\"upload\"}");
    logger_json_begin_success("debug config clear", logger_now_utc_or_null(app));
    fputs("{\"applied\":true,\"field\":\"upload\"}", stdout);
    logger_json_end_success();
}

static void logger_handle_debug_session_start(logger_service_cli_t *cli, logger_app_t *app, uint32_t now_ms) {
    if (!logger_cli_is_service_mode(app)) {
        logger_json_begin_error("debug session start", logger_now_utc_or_null(app), "not_permitted_in_mode", "debug session start is only allowed in service mode");
        return;
    }
    if (!logger_service_cli_is_unlocked(cli, now_ms)) {
        logger_json_begin_error("debug session start", logger_now_utc_or_null(app), "service_locked", "service unlock is required before debug session start");
        return;
    }

    const char *error_code = "internal_error";
    const char *error_message = "debug session start failed";
    if (!logger_session_start_debug(
            &app->session,
            &app->system_log,
            app->hardware_id,
            &app->persisted,
            &app->clock,
            &app->battery,
            &app->storage,
            app->persisted.current_fault_code,
            app->persisted.boot_counter,
            now_ms,
            &error_code,
            &error_message)) {
        logger_json_begin_error("debug session start", logger_now_utc_or_null(app), error_code, error_message);
        return;
    }
    app->last_session_snapshot_mono_ms = now_ms;

    logger_json_begin_success("debug session start", logger_now_utc_or_null(app));
    fputs("{\"active\":true,\"session_id\":", stdout);
    logger_json_write_string_or_null(app->session.session_id);
    fputs(",\"study_day_local\":", stdout);
    logger_json_write_string_or_null(app->session.study_day_local);
    fputs("}", stdout);
    logger_json_end_success();
}

static void logger_handle_debug_session_snapshot(logger_app_t *app, uint32_t now_ms) {
    if (!app->session.active) {
        logger_json_begin_error("debug session snapshot", logger_now_utc_or_null(app), "not_permitted_in_mode", "no debug session is active");
        return;
    }
    if (!logger_session_write_status_snapshot(
            &app->session,
            &app->clock,
            &app->battery,
            &app->storage,
            app->persisted.current_fault_code,
            app->persisted.boot_counter,
            now_ms)) {
        logger_json_begin_error("debug session snapshot", logger_now_utc_or_null(app), "storage_unavailable", "failed to append status snapshot");
        return;
    }
    app->last_session_snapshot_mono_ms = now_ms;

    logger_json_begin_success("debug session snapshot", logger_now_utc_or_null(app));
    fputs("{\"written\":true,\"journal_size_bytes\":", stdout);
    printf("%llu", (unsigned long long)app->session.journal_size_bytes);
    fputs("}", stdout);
    logger_json_end_success();
}

static void logger_handle_debug_session_stop(logger_app_t *app, uint32_t now_ms) {
    if (!app->session.active) {
        logger_json_begin_success("debug session stop", logger_now_utc_or_null(app));
        fputs("{\"active\":false}", stdout);
        logger_json_end_success();
        return;
    }
    if (!logger_session_stop_debug(
            &app->session,
            &app->system_log,
            app->hardware_id,
            &app->persisted,
            &app->clock,
            &app->storage,
            app->persisted.boot_counter,
            now_ms)) {
        logger_json_begin_error("debug session stop", logger_now_utc_or_null(app), "storage_unavailable", "failed to close debug session");
        return;
    }
    logger_json_begin_success("debug session stop", logger_now_utc_or_null(app));
    fputs("{\"active\":false}", stdout);
    logger_json_end_success();
}

static void logger_handle_debug_queue_rebuild(logger_service_cli_t *cli, logger_app_t *app) {
    const bool allowed_in_wait_h10 = app->runtime.current_state == LOGGER_RUNTIME_LOG_WAIT_H10;
    if (!logger_cli_is_service_mode(app) && !allowed_in_wait_h10) {
        logger_json_begin_error("debug queue rebuild", logger_now_utc_or_null(app), "not_permitted_in_mode", "debug queue rebuild is only allowed in service mode or log_wait_h10");
        return;
    }
    if (app->session.active) {
        logger_json_begin_error("debug queue rebuild", logger_now_utc_or_null(app), "not_permitted_in_mode", "debug queue rebuild is not permitted while a session is active");
        return;
    }
    if (logger_cli_is_service_mode(app) && !logger_service_cli_is_unlocked(cli, to_ms_since_boot(get_absolute_time()))) {
        logger_json_begin_error("debug queue rebuild", logger_now_utc_or_null(app), "service_locked", "service unlock is required before debug queue rebuild");
        return;
    }

    logger_upload_queue_summary_t summary;
    if (!logger_upload_queue_rebuild_file(&app->system_log, logger_now_utc_or_null(app), &summary)) {
        logger_json_begin_error("debug queue rebuild", logger_now_utc_or_null(app), "storage_unavailable", "failed to rebuild upload_queue.json from local sessions");
        return;
    }

    logger_json_begin_success("debug queue rebuild", logger_now_utc_or_null(app));
    fputs("{\"rebuilt\":true,\"updated_at_utc\":", stdout);
    logger_json_write_string_or_null(summary.updated_at_utc[0] != '\0' ? summary.updated_at_utc : NULL);
    fputs(",\"session_count\":", stdout);
    printf("%lu", (unsigned long)summary.session_count);
    fputs(",\"pending_count\":", stdout);
    printf("%lu", (unsigned long)summary.pending_count);
    fputs(",\"blocked_count\":", stdout);
    printf("%lu", (unsigned long)summary.blocked_count);
    fputs("}", stdout);
    logger_json_end_success();
}

static void logger_handle_debug_upload_once(logger_service_cli_t *cli, logger_app_t *app) {
    const bool allowed_in_wait_h10 = app->runtime.current_state == LOGGER_RUNTIME_LOG_WAIT_H10;
    if (!logger_cli_is_service_mode(app) && !allowed_in_wait_h10) {
        logger_json_begin_error("debug upload once", logger_now_utc_or_null(app), "not_permitted_in_mode", "debug upload once is only allowed in service mode or log_wait_h10");
        return;
    }
    if (logger_cli_is_service_mode(app) && !logger_service_cli_is_unlocked(cli, to_ms_since_boot(get_absolute_time()))) {
        logger_json_begin_error("debug upload once", logger_now_utc_or_null(app), "service_locked", "service unlock is required before debug upload once");
        return;
    }

    logger_upload_process_result_t result;
    const bool ok = logger_upload_process_one(
        &app->system_log,
        &app->persisted.config,
        app->hardware_id,
        logger_now_utc_or_null(app),
        &result);

    if (result.code == LOGGER_UPLOAD_PROCESS_RESULT_NOT_ATTEMPTED) {
        logger_json_begin_error("debug upload once", logger_now_utc_or_null(app), "invalid_config", result.message);
        return;
    }
    if (result.code == LOGGER_UPLOAD_PROCESS_RESULT_FAILED) {
        logger_json_begin_error(
            "debug upload once",
            logger_now_utc_or_null(app),
            logger_string_present(result.failure_class) ? result.failure_class : "upload_failed",
            result.message);
        return;
    }
    if (result.code == LOGGER_UPLOAD_PROCESS_RESULT_BLOCKED_MIN_FIRMWARE) {
        logger_json_begin_error("debug upload once", logger_now_utc_or_null(app), "min_firmware_rejected", result.message);
        return;
    }

    logger_json_begin_success("debug upload once", logger_now_utc_or_null(app));
    fputs("{\"attempted\":", stdout);
    fputs(result.attempted ? "true" : "false", stdout);
    fputs(",\"result\":", stdout);
    logger_json_write_string_or_null(
        result.code == LOGGER_UPLOAD_PROCESS_RESULT_VERIFIED ? "verified" :
        result.code == LOGGER_UPLOAD_PROCESS_RESULT_NO_WORK ? "no_work" :
        (ok ? "ok" : "unknown"));
    fputs(",\"session_id\":", stdout);
    logger_json_write_string_or_null(logger_string_present(result.session_id) ? result.session_id : NULL);
    fputs(",\"final_status\":", stdout);
    logger_json_write_string_or_null(logger_string_present(result.final_status) ? result.final_status : NULL);
    fputs(",\"receipt_id\":", stdout);
    logger_json_write_string_or_null(logger_string_present(result.receipt_id) ? result.receipt_id : NULL);
    fputs(",\"verified_upload_utc\":", stdout);
    logger_json_write_string_or_null(logger_string_present(result.verified_upload_utc) ? result.verified_upload_utc : NULL);
    fputs(",\"http_status\":", stdout);
    if (result.http_status >= 0) {
        printf("%d", result.http_status);
    } else {
        fputs("null", stdout);
    }
    fputs(",\"message\":", stdout);
    logger_json_write_string_or_null(logger_string_present(result.message) ? result.message : NULL);
    fputs("}", stdout);
    logger_json_end_success();
}

void logger_service_cli_init(logger_service_cli_t *cli) {
    memset(cli, 0, sizeof(*cli));
}

bool logger_service_cli_is_unlocked(const logger_service_cli_t *cli, uint32_t now_ms) {
    return cli->unlocked && now_ms < cli->unlock_deadline_mono_ms;
}

static void logger_service_cli_execute(logger_service_cli_t *cli, logger_app_t *app, const char *line, uint32_t now_ms) {
    if (cli->unlocked && now_ms >= cli->unlock_deadline_mono_ms) {
        cli->unlocked = false;
    }

    if (strcmp(line, "status --json") == 0) {
        logger_handle_status_json(app);
        return;
    }
    if (strcmp(line, "provisioning-status --json") == 0) {
        logger_handle_provisioning_status_json(app);
        return;
    }
    if (strcmp(line, "queue --json") == 0) {
        logger_handle_queue_json(app);
        return;
    }
    if (strcmp(line, "preflight --json") == 0) {
        logger_handle_preflight_json(app);
        return;
    }
    if (strcmp(line, "net-test --json") == 0) {
        logger_handle_net_test_json(app);
        return;
    }
    if (strcmp(line, "config export --json") == 0) {
        logger_handle_config_export_json(app);
        return;
    }
    if (strcmp(line, "system-log export --json") == 0) {
        logger_handle_system_log_export_json(app);
        return;
    }
    if (strcmp(line, "clock status --json") == 0) {
        logger_handle_clock_status_json(app);
        return;
    }
    if (strcmp(line, "service unlock") == 0) {
        logger_handle_service_unlock(cli, app, now_ms);
        return;
    }
    if (strcmp(line, "fault clear") == 0) {
        logger_handle_fault_clear(app);
        return;
    }
    if (strcmp(line, "factory-reset") == 0) {
        logger_handle_factory_reset(cli, app);
        return;
    }
    if (strncmp(line, "clock set ", 10) == 0) {
        logger_handle_clock_set(cli, app, line + 10);
        return;
    }
    if (strncmp(line, "debug config set ", 17) == 0) {
        logger_handle_debug_config_set(cli, app, line + 17);
        return;
    }
    if (strncmp(line, "debug config clear ", 19) == 0) {
        logger_handle_debug_config_clear(cli, app, line + 19);
        return;
    }
    if (strcmp(line, "debug session start") == 0) {
        logger_handle_debug_session_start(cli, app, now_ms);
        return;
    }
    if (strcmp(line, "debug session snapshot") == 0) {
        logger_handle_debug_session_snapshot(app, now_ms);
        return;
    }
    if (strcmp(line, "debug session stop") == 0) {
        logger_handle_debug_session_stop(app, now_ms);
        return;
    }
    if (strcmp(line, "debug queue rebuild") == 0) {
        logger_handle_debug_queue_rebuild(cli, app);
        return;
    }
    if (strcmp(line, "debug upload once") == 0) {
        logger_handle_debug_upload_once(cli, app);
        return;
    }
    if (strcmp(line, "debug reboot") == 0) {
        app->reboot_pending = true;
        logger_json_begin_success("debug reboot", logger_now_utc_or_null(app));
        fputs("{\"will_reboot\":true}", stdout);
        logger_json_end_success();
        return;
    }
    if (strcmp(line, "sd format") == 0 ||
        strncmp(line, "config import ", 14) == 0) {
        logger_json_begin_error(line, logger_now_utc_or_null(app), "internal_error", "command not implemented yet");
        return;
    }

    logger_json_begin_error(line, logger_now_utc_or_null(app), "internal_error", "unknown command");
}

void logger_service_cli_poll(logger_service_cli_t *cli, logger_app_t *app, uint32_t now_ms) {
    for (;;) {
        int ch = getchar_timeout_us(0);
        if (ch == PICO_ERROR_TIMEOUT) {
            break;
        }
        if (ch == '\r') {
            continue;
        }
        if (ch == '\n') {
            cli->line_buf[cli->line_len] = '\0';
            if (cli->line_len > 0u) {
                logger_service_cli_execute(cli, app, cli->line_buf, now_ms);
            }
            cli->line_len = 0u;
            continue;
        }
        if (cli->line_len + 1u >= sizeof(cli->line_buf)) {
            cli->line_len = 0u;
            logger_json_begin_error("input", logger_now_utc_or_null(app), "internal_error", "input line too long");
            break;
        }
        cli->line_buf[cli->line_len++] = (char)ch;
    }
}
