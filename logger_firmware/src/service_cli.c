#include "logger/service_cli.h"

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"

#include "board_config.h"
#include "logger/app_main.h"

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
    return app->clock.valid ? app->clock.now_utc : NULL;
}

static void logger_json_write_escaped(const char *value) {
    for (const unsigned char *p = (const unsigned char *)value; *p != '\0'; ++p) {
        switch (*p) {
            case '\\':
                fputs("\\\\", stdout);
                break;
            case '"':
                fputs("\\\"", stdout);
                break;
            case '\b':
                fputs("\\b", stdout);
                break;
            case '\f':
                fputs("\\f", stdout);
                break;
            case '\n':
                fputs("\\n", stdout);
                break;
            case '\r':
                fputs("\\r", stdout);
                break;
            case '\t':
                fputs("\\t", stdout);
                break;
            default:
                if (*p < 0x20u) {
                    printf("\\u%04x", *p);
                } else {
                    putchar((int)*p);
                }
                break;
        }
    }
}

static void logger_json_write_string_or_null(const char *value) {
    if (!logger_string_present(value)) {
        fputs("null", stdout);
        return;
    }
    putchar('"');
    logger_json_write_escaped(value);
    putchar('"');
}

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
        if (!first) {
            fputs(",", stdout);
        }
        logger_json_write_string_or_null("bound_h10_address");
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
        if (!first) {
            fputs(",", stdout);
        }
        logger_json_write_string_or_null("bound_h10_address");
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
        if (!first) {
            fputs(",", stdout);
        }
        logger_json_write_string_or_null("upload_auth");
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
        if (!first) {
            fputs(",", stdout);
        }
        logger_json_write_string_or_null("clock_invalid");
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
        case LOGGER_FAULT_SD_WRITE_FAILED:
        case LOGGER_FAULT_SD_LOW_SPACE_RESERVE_UNMET:
        case LOGGER_FAULT_UPLOAD_BLOCKED_MIN_FIRMWARE:
            return true;
        case LOGGER_FAULT_NONE:
        default:
            return false;
    }
}

static void logger_write_status_payload(const logger_app_t *app) {
    char study_day_local[11] = {0};
    const bool have_study_day = logger_runtime_state_is_logging(app->runtime.current_state) &&
        logger_clock_derive_study_day_local(&app->clock, app->persisted.config.timezone, study_day_local);

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

    fputs(",\"storage\":{\"sd_present\":false,\"filesystem\":null,\"free_bytes\":null,\"reserve_bytes\":", stdout);
    printf("%lu", (unsigned long)LOGGER_SD_MIN_FREE_RESERVE_BYTES);
    fputs(",\"card_identity\":null}", stdout);

    fputs(",\"h10\":{\"bound_address\":", stdout);
    logger_json_write_string_or_null(app->persisted.config.bound_h10_address);
    fputs(",\"connected\":false,\"encrypted\":false,\"bonded\":false,\"last_seen_address\":null,\"battery_percent\":null}", stdout);

    fputs(",\"session\":{\"active\":false,\"session_id\":null,\"study_day_local\":", stdout);
    logger_json_write_string_or_null(have_study_day ? study_day_local : NULL);
    fputs(",\"span_id\":null,\"quarantined\":false,\"clock_state\":", stdout);
    logger_json_write_string_or_null(logger_clock_state_name(&app->clock));
    fputs(",\"journal_size_bytes\":null}", stdout);

    fputs(",\"upload_queue\":{\"pending_count\":0,\"blocked_count\":0,\"oldest_pending_study_day\":null,\"last_failure_class\":null}", stdout);
    fputs(",\"last_day_outcome\":{\"study_day_local\":null,\"kind\":null,\"reason\":null}", stdout);
    fputs(",\"firmware\":{\"version\":", stdout);
    logger_json_write_string_or_null(LOGGER_FIRMWARE_VERSION);
    fputs(",\"build_id\":", stdout);
    logger_json_write_string_or_null(LOGGER_BUILD_ID);
    fputs("}}", stdout);
}

static void logger_handle_status_json(logger_service_cli_t *cli, logger_app_t *app) {
    (void)cli;
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
    logger_json_begin_success("queue", logger_now_utc_or_null(app));
    fputs("{\"schema_source\":\"upload_queue.json\",\"updated_at_utc\":", stdout);
    logger_json_write_string_or_null(logger_now_utc_or_null(app));
    fputs(",\"sessions\":[]}", stdout);
    logger_json_end_success();
}

static void logger_handle_system_log_export_json(logger_app_t *app) {
    logger_json_begin_success("system-log export", logger_now_utc_or_null(app));
    fputs("{\"schema_version\":1,\"exported_at_utc\":", stdout);
    logger_json_write_string_or_null(logger_now_utc_or_null(app));
    fputs(",\"events\":[]}", stdout);
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

    (void)logger_config_store_factory_reset(&app->persisted);
    cli->unlocked = false;
    app->reboot_pending = true;

    logger_json_begin_success("factory-reset", logger_now_utc_or_null(app));
    fputs("{\"factory_reset\":true,\"will_reboot\":true}", stdout);
    logger_json_end_success();
}

static void logger_handle_not_implemented(const char *command, const logger_app_t *app) {
    logger_json_begin_error(command, logger_now_utc_or_null(app), "internal_error", "command not implemented yet");
}

static void logger_handle_debug_config_set(logger_service_cli_t *cli, logger_app_t *app, const char *args) {
    if (logger_cli_is_logging_mode(app)) {
        logger_json_begin_error("debug config set", logger_now_utc_or_null(app), "busy_logging", "config mutation is not permitted while logging");
        return;
    }
    if (!logger_cli_is_service_mode(app)) {
        logger_json_begin_error("debug config set", logger_now_utc_or_null(app), "not_permitted_in_mode", "debug config set is only allowed in service mode");
        return;
    }
    if (!logger_service_cli_is_unlocked(cli, to_ms_since_boot(get_absolute_time()))) {
        logger_json_begin_error("debug config set", logger_now_utc_or_null(app), "service_locked", "service unlock is required before debug config set");
        return;
    }

    char field[48];
    char value[256];
    field[0] = '\0';
    value[0] = '\0';
    const int matched = sscanf(args, "%47s %255[^\n]", field, value);
    if (matched < 2) {
        logger_json_begin_error("debug config set", logger_now_utc_or_null(app), "invalid_config", "expected: debug config set <field> <value>");
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
    logger_json_begin_success("debug config clear", logger_now_utc_or_null(app));
    fputs("{\"applied\":true,\"field\":\"upload\"}", stdout);
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
        logger_handle_status_json(cli, app);
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
    if (strcmp(line, "config export --json") == 0) {
        logger_handle_config_export_json(app);
        return;
    }
    if (strcmp(line, "system-log export --json") == 0) {
        logger_handle_system_log_export_json(app);
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
    if (strcmp(line, "preflight --json") == 0 ||
        strcmp(line, "net-test --json") == 0 ||
        strcmp(line, "sd format") == 0 ||
        strncmp(line, "config import ", 14) == 0) {
        logger_handle_not_implemented(line, app);
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
    if (strcmp(line, "debug reboot") == 0) {
        app->reboot_pending = true;
        logger_json_begin_success("debug reboot", logger_now_utc_or_null(app));
        fputs("{\"will_reboot\":true}", stdout);
        logger_json_end_success();
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
