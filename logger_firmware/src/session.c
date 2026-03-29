#include "logger/session.h"

#include <stdio.h>
#include <string.h>

#include "pico/rand.h"

#include "board_config.h"
#include "logger/journal.h"

#define LOGGER_SESSION_JSON_MAX 768

static void logger_random_hex128(char out[LOGGER_SESSION_ID_HEX_LEN + 1]) {
    static const char hex[] = "0123456789abcdef";
    rng_128_t random128;
    get_rand_128(&random128);
    const uint8_t *bytes = (const uint8_t *)&random128;
    for (size_t i = 0u; i < 16u; ++i) {
        out[i * 2u] = hex[(bytes[i] >> 4) & 0x0f];
        out[(i * 2u) + 1u] = hex[bytes[i] & 0x0f];
    }
    out[LOGGER_SESSION_ID_HEX_LEN] = '\0';
}

static void logger_json_escape_into(char *dst, size_t dst_len, const char *src) {
    if (dst_len == 0u) {
        return;
    }
    size_t out = 0u;
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    for (const unsigned char *p = (const unsigned char *)src; *p != '\0' && (out + 1u) < dst_len; ++p) {
        const char *replacement = NULL;
        char unicode_buf[7];
        switch (*p) {
            case '\\':
                replacement = "\\\\";
                break;
            case '"':
                replacement = "\\\"";
                break;
            case '\n':
                replacement = "\\n";
                break;
            case '\r':
                replacement = "\\r";
                break;
            case '\t':
                replacement = "\\t";
                break;
            default:
                break;
        }
        if (replacement != NULL) {
            const size_t repl_len = strlen(replacement);
            if ((out + repl_len) >= dst_len) {
                break;
            }
            memcpy(dst + out, replacement, repl_len);
            out += repl_len;
            continue;
        }
        if (*p < 0x20u) {
            snprintf(unicode_buf, sizeof(unicode_buf), "\\u%04x", *p);
            if ((out + 6u) >= dst_len) {
                break;
            }
            memcpy(dst + out, unicode_buf, 6u);
            out += 6u;
            continue;
        }
        dst[out++] = (char)*p;
    }
    dst[out] = '\0';
}

static int64_t logger_session_observed_utc_ns_or_zero(const logger_clock_status_t *clock) {
    int64_t utc_ns = 0ll;
    (void)logger_clock_observed_utc_ns(clock, &utc_ns);
    return utc_ns;
}

static void logger_session_clock_state(const logger_clock_status_t *clock, char out[8], bool *quarantined_out) {
    if (clock->valid) {
        memcpy(out, "valid", 6u);
        *quarantined_out = false;
    } else {
        memcpy(out, "invalid", 8u);
        *quarantined_out = true;
    }
}

static bool logger_session_write_live(
    const logger_session_state_t *session,
    const logger_clock_status_t *clock,
    uint32_t boot_counter,
    uint32_t now_ms) {
    char payload[512];
    char current_span_id_json[64];
    char last_flush_utc_json[64];

    if (session->span_active) {
        snprintf(current_span_id_json, sizeof(current_span_id_json), "\"%s\"", session->current_span_id);
    } else {
        memcpy(current_span_id_json, "null", 5u);
    }
    if (clock->now_utc[0] != '\0') {
        snprintf(last_flush_utc_json, sizeof(last_flush_utc_json), "\"%s\"", clock->now_utc);
    } else {
        memcpy(last_flush_utc_json, "null", 5u);
    }

    snprintf(payload,
             sizeof(payload),
             "{\"schema_version\":1,\"session_id\":\"%s\",\"study_day_local\":\"%s\",\"session_dir\":\"%s\",\"state\":\"active\",\"journal_size_bytes\":%llu,\"last_flush_utc\":%s,\"last_flush_mono_us\":%llu,\"current_span_id\":%s,\"current_span_index\":%s,\"quarantined\":%s,\"clock_state\":\"%s\",\"boot_counter\":%lu}",
             session->session_id,
             session->study_day_local,
             session->session_dir_name,
             (unsigned long long)session->journal_size_bytes,
             last_flush_utc_json,
             (unsigned long long)now_ms * 1000ull,
             current_span_id_json,
             session->span_active ? "0" : "null",
             session->quarantined ? "true" : "false",
             session->clock_state,
             (unsigned long)boot_counter);
    return logger_storage_write_file_atomic(session->live_path, payload, strlen(payload));
}

void logger_session_init(logger_session_state_t *session) {
    memset(session, 0, sizeof(*session));
}

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
    const char **error_message_out) {
    (void)hardware_id;
    (void)battery;
    (void)current_fault;

    if (session->active) {
        if (error_code_out != NULL) {
            *error_code_out = "not_permitted_in_mode";
        }
        if (error_message_out != NULL) {
            *error_message_out = "debug session is already active";
        }
        return false;
    }
    if (!logger_storage_ready_for_logging(storage)) {
        if (error_code_out != NULL) {
            *error_code_out = "storage_unavailable";
        }
        if (error_message_out != NULL) {
            *error_message_out = "storage is not ready for session artifacts";
        }
        return false;
    }

    logger_session_init(session);
    logger_session_clock_state(clock, session->clock_state, &session->quarantined);
    if (!logger_clock_derive_study_day_local_observed(clock, persisted->config.timezone, session->study_day_local)) {
        if (error_code_out != NULL) {
            *error_code_out = "invalid_config";
        }
        if (error_message_out != NULL) {
            *error_message_out = "cannot derive study_day_local from current clock/timezone";
        }
        return false;
    }

    logger_random_hex128(session->session_id);
    logger_random_hex128(session->current_span_id);
    session->active = true;
    session->span_active = true;
    session->span_index_in_session = 0u;
    session->span_count = 1u;

    snprintf(session->session_dir_name,
             sizeof(session->session_dir_name),
             "%s__%s",
             session->study_day_local,
             session->session_id);
    snprintf(session->session_dir_path,
             sizeof(session->session_dir_path),
             "0:/logger/sessions/%s",
             session->session_dir_name);
    const size_t dir_path_len = strlen(session->session_dir_path);
    if ((dir_path_len + strlen("/journal.bin") + 1u) > sizeof(session->journal_path) ||
        (dir_path_len + strlen("/live.json") + 1u) > sizeof(session->live_path)) {
        logger_session_init(session);
        if (error_code_out != NULL) {
            *error_code_out = "storage_unavailable";
        }
        if (error_message_out != NULL) {
            *error_message_out = "session path exceeds buffer";
        }
        return false;
    }
    memcpy(session->journal_path, session->session_dir_path, dir_path_len);
    memcpy(session->journal_path + dir_path_len, "/journal.bin", strlen("/journal.bin") + 1u);
    memcpy(session->live_path, session->session_dir_path, dir_path_len);
    memcpy(session->live_path + dir_path_len, "/live.json", strlen("/live.json") + 1u);

    if (!logger_storage_ensure_dir(session->session_dir_path)) {
        logger_session_init(session);
        if (error_code_out != NULL) {
            *error_code_out = "storage_unavailable";
        }
        if (error_message_out != NULL) {
            *error_message_out = "failed to create session directory";
        }
        return false;
    }

    if (!logger_journal_create(
            session->journal_path,
            session->session_id,
            boot_counter,
            logger_session_observed_utc_ns_or_zero(clock),
            &session->journal_size_bytes)) {
        logger_session_init(session);
        if (error_code_out != NULL) {
            *error_code_out = "storage_unavailable";
        }
        if (error_message_out != NULL) {
            *error_message_out = "failed to create journal.bin";
        }
        return false;
    }

    char logger_id_escaped[LOGGER_CONFIG_LOGGER_ID_MAX * 2u];
    char subject_id_escaped[LOGGER_CONFIG_SUBJECT_ID_MAX * 2u];
    char timezone_escaped[LOGGER_CONFIG_TIMEZONE_MAX * 2u];
    char h10_addr_escaped[LOGGER_CONFIG_BOUND_H10_ADDR_MAX * 2u];
    logger_json_escape_into(logger_id_escaped, sizeof(logger_id_escaped), persisted->config.logger_id);
    logger_json_escape_into(subject_id_escaped, sizeof(subject_id_escaped), persisted->config.subject_id);
    logger_json_escape_into(timezone_escaped, sizeof(timezone_escaped), persisted->config.timezone);
    logger_json_escape_into(h10_addr_escaped, sizeof(h10_addr_escaped), persisted->config.bound_h10_address);

    char payload[LOGGER_SESSION_JSON_MAX];
    snprintf(payload,
             sizeof(payload),
             "{\"schema_version\":1,\"record_type\":\"session_start\",\"utc_ns\":%lld,\"mono_us\":%llu,\"boot_counter\":%lu,\"session_id\":\"%s\",\"study_day_local\":\"%s\",\"logger_id\":\"%s\",\"subject_id\":\"%s\",\"timezone\":\"%s\",\"clock_state\":\"%s\",\"start_reason\":\"first_span_of_session\"}",
             (long long)logger_session_observed_utc_ns_or_zero(clock),
             (unsigned long long)now_ms * 1000ull,
             (unsigned long)boot_counter,
             session->session_id,
             session->study_day_local,
             logger_id_escaped,
             subject_id_escaped,
             timezone_escaped,
             session->clock_state);
    if (!logger_journal_append_json_record(
            session->journal_path,
            LOGGER_JOURNAL_RECORD_SESSION_START,
            session->next_record_seq++,
            payload,
            &session->journal_size_bytes)) {
        logger_session_init(session);
        if (error_code_out != NULL) {
            *error_code_out = "storage_unavailable";
        }
        if (error_message_out != NULL) {
            *error_message_out = "failed to append session_start";
        }
        return false;
    }

    snprintf(payload,
             sizeof(payload),
             "{\"schema_version\":1,\"record_type\":\"span_start\",\"utc_ns\":%lld,\"mono_us\":%llu,\"boot_counter\":%lu,\"session_id\":\"%s\",\"span_id\":\"%s\",\"span_index_in_session\":0,\"start_reason\":\"session_start\",\"h10_address\":\"%s\",\"encrypted\":false,\"bonded\":false}",
             (long long)logger_session_observed_utc_ns_or_zero(clock),
             (unsigned long long)now_ms * 1000ull,
             (unsigned long)boot_counter,
             session->session_id,
             session->current_span_id,
             h10_addr_escaped);
    if (!logger_journal_append_json_record(
            session->journal_path,
            LOGGER_JOURNAL_RECORD_SPAN_START,
            session->next_record_seq++,
            payload,
            &session->journal_size_bytes)) {
        logger_session_init(session);
        if (error_code_out != NULL) {
            *error_code_out = "storage_unavailable";
        }
        if (error_message_out != NULL) {
            *error_message_out = "failed to append span_start";
        }
        return false;
    }

    if (!logger_session_write_live(session, clock, boot_counter, now_ms)) {
        logger_session_init(session);
        if (error_code_out != NULL) {
            *error_code_out = "storage_unavailable";
        }
        if (error_message_out != NULL) {
            *error_message_out = "failed to write live.json";
        }
        return false;
    }

    (void)logger_system_log_append(
        system_log,
        clock->now_utc[0] != '\0' ? clock->now_utc : NULL,
        "session_started",
        LOGGER_SYSTEM_LOG_SEVERITY_INFO,
        "{\"debug\":true}");
    return true;
}

bool logger_session_write_status_snapshot(
    logger_session_state_t *session,
    const logger_clock_status_t *clock,
    const logger_battery_status_t *battery,
    const logger_storage_status_t *storage,
    logger_fault_code_t current_fault,
    uint32_t boot_counter,
    uint32_t now_ms) {
    if (!session->active) {
        return false;
    }

    char active_span_id_json[64];
    if (session->span_active) {
        snprintf(active_span_id_json, sizeof(active_span_id_json), "\"%s\"", session->current_span_id);
    } else {
        memcpy(active_span_id_json, "null", 5u);
    }

    char fault_code_json[64];
    const char *fault_name = logger_fault_code_name(current_fault);
    if (fault_name != NULL) {
        snprintf(fault_code_json, sizeof(fault_code_json), "\"%s\"", fault_name);
    } else {
        memcpy(fault_code_json, "null", 5u);
    }

    char payload[LOGGER_SESSION_JSON_MAX];
    snprintf(payload,
             sizeof(payload),
             "{\"schema_version\":1,\"record_type\":\"status_snapshot\",\"utc_ns\":%lld,\"mono_us\":%llu,\"boot_counter\":%lu,\"session_id\":\"%s\",\"active_span_id\":%s,\"battery_voltage_mv\":%u,\"battery_estimate_pct\":%d,\"vbus_present\":%s,\"sd_free_bytes\":%llu,\"sd_reserve_bytes\":%lu,\"wifi_enabled\":false,\"quarantined\":%s,\"fault_code\":%s}",
             (long long)logger_session_observed_utc_ns_or_zero(clock),
             (unsigned long long)now_ms * 1000ull,
             (unsigned long)boot_counter,
             session->session_id,
             active_span_id_json,
             (unsigned)battery->voltage_mv,
             battery->estimate_pct,
             battery->vbus_present ? "true" : "false",
             (unsigned long long)storage->free_bytes,
             (unsigned long)LOGGER_SD_MIN_FREE_RESERVE_BYTES,
             session->quarantined ? "true" : "false",
             fault_code_json);

    if (!logger_journal_append_json_record(
            session->journal_path,
            LOGGER_JOURNAL_RECORD_STATUS_SNAPSHOT,
            session->next_record_seq++,
            payload,
            &session->journal_size_bytes)) {
        return false;
    }
    return logger_session_write_live(session, clock, boot_counter, now_ms);
}

bool logger_session_write_marker(
    logger_session_state_t *session,
    const logger_clock_status_t *clock,
    uint32_t boot_counter,
    uint32_t now_ms) {
    if (!session->active || !session->span_active) {
        return false;
    }

    char payload[LOGGER_SESSION_JSON_MAX];
    snprintf(payload,
             sizeof(payload),
             "{\"schema_version\":1,\"record_type\":\"marker\",\"utc_ns\":%lld,\"mono_us\":%llu,\"boot_counter\":%lu,\"session_id\":\"%s\",\"span_id\":\"%s\",\"marker_kind\":\"generic\"}",
             (long long)logger_session_observed_utc_ns_or_zero(clock),
             (unsigned long long)now_ms * 1000ull,
             (unsigned long)boot_counter,
             session->session_id,
             session->current_span_id);
    return logger_journal_append_json_record(
        session->journal_path,
        LOGGER_JOURNAL_RECORD_MARKER,
        session->next_record_seq++,
        payload,
        &session->journal_size_bytes);
}

bool logger_session_stop_debug(
    logger_session_state_t *session,
    logger_system_log_t *system_log,
    const logger_clock_status_t *clock,
    uint32_t boot_counter,
    uint32_t now_ms) {
    if (!session->active) {
        return false;
    }

    char payload[LOGGER_SESSION_JSON_MAX];
    if (session->span_active) {
        snprintf(payload,
                 sizeof(payload),
                 "{\"schema_version\":1,\"record_type\":\"span_end\",\"utc_ns\":%lld,\"mono_us\":%llu,\"boot_counter\":%lu,\"session_id\":\"%s\",\"span_id\":\"%s\",\"end_reason\":\"manual_stop\",\"packet_count\":0,\"first_seq_in_span\":0,\"last_seq_in_span\":0}",
                 (long long)logger_session_observed_utc_ns_or_zero(clock),
                 (unsigned long long)now_ms * 1000ull,
                 (unsigned long)boot_counter,
                 session->session_id,
                 session->current_span_id);
        if (!logger_journal_append_json_record(
                session->journal_path,
                LOGGER_JOURNAL_RECORD_SPAN_END,
                session->next_record_seq++,
                payload,
                &session->journal_size_bytes)) {
            return false;
        }
        session->span_active = false;
    }

    snprintf(payload,
             sizeof(payload),
             "{\"schema_version\":1,\"record_type\":\"session_end\",\"utc_ns\":%lld,\"mono_us\":%llu,\"boot_counter\":%lu,\"session_id\":\"%s\",\"end_reason\":\"manual_stop\",\"span_count\":%lu,\"quarantined\":%s,\"quarantine_reasons\":%s}",
             (long long)logger_session_observed_utc_ns_or_zero(clock),
             (unsigned long long)now_ms * 1000ull,
             (unsigned long)boot_counter,
             session->session_id,
             (unsigned long)session->span_count,
             session->quarantined ? "true" : "false",
             session->quarantined ? "[\"clock_invalid_at_start\"]" : "[]");
    if (!logger_journal_append_json_record(
            session->journal_path,
            LOGGER_JOURNAL_RECORD_SESSION_END,
            session->next_record_seq++,
            payload,
            &session->journal_size_bytes)) {
        return false;
    }

    (void)logger_storage_remove_file(session->live_path);
    (void)logger_system_log_append(
        system_log,
        clock->now_utc[0] != '\0' ? clock->now_utc : NULL,
        "session_closed",
        LOGGER_SYSTEM_LOG_SEVERITY_INFO,
        "{\"debug\":true,\"reason\":\"manual_stop\"}");
    logger_session_init(session);
    return true;
}
