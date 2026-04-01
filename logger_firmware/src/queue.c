#include "logger/queue.h"

#include <stdio.h>
#include <string.h>

#include "ff.h"

#include "logger/json.h"
#include "logger/storage.h"
#include "logger/upload_bundle.h"

#define LOGGER_QUEUE_PATH "0:/logger/state/upload_queue.json"
#define LOGGER_QUEUE_TMP_PATH "0:/logger/state/upload_queue.json.tmp"
#define LOGGER_SESSIONS_DIR "0:/logger/sessions"
#define LOGGER_MANIFEST_READ_MAX 8192u
#define LOGGER_QUEUE_READ_MAX 49152u
#define LOGGER_MANIFEST_JSON_TOKEN_MAX 512u
#define LOGGER_QUEUE_TOP_LEVEL_JSON_TOKEN_COUNT 7u
#define LOGGER_QUEUE_ENTRY_JSON_TOKEN_COUNT 29u
#define LOGGER_QUEUE_JSON_TOKEN_MAX \
    (LOGGER_QUEUE_TOP_LEVEL_JSON_TOKEN_COUNT + (LOGGER_UPLOAD_QUEUE_MAX_SESSIONS * LOGGER_QUEUE_ENTRY_JSON_TOKEN_COUNT))
#define LOGGER_UPLOAD_RETENTION_DAYS 14u
#define LOGGER_UPLOAD_RETENTION_SECONDS (LOGGER_UPLOAD_RETENTION_DAYS * 24u * 60u * 60u)

typedef struct {
    char session_id[33];
    char study_day_local[11];
    char session_start_utc[LOGGER_UPLOAD_QUEUE_UTC_MAX + 1];
    char session_end_utc[LOGGER_UPLOAD_QUEUE_UTC_MAX + 1];
    bool quarantined;
} logger_manifest_summary_t;

static void logger_copy_string(char *dst, size_t dst_len, const char *src) {
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

static bool logger_string_present(const char *value) {
    return value != NULL && value[0] != '\0';
}

static bool logger_path_join2(char *dst, size_t dst_len, const char *a, const char *b) {
    const size_t a_len = strlen(a);
    const size_t b_len = strlen(b);
    if ((a_len + b_len + 1u) > dst_len) {
        return false;
    }
    memcpy(dst, a, a_len);
    memcpy(dst + a_len, b, b_len + 1u);
    return true;
}

static bool logger_path_join3(char *dst, size_t dst_len, const char *a, const char *b, const char *c) {
    const size_t a_len = strlen(a);
    const size_t b_len = strlen(b);
    const size_t c_len = strlen(c);
    if ((a_len + b_len + c_len + 1u) > dst_len) {
        return false;
    }
    memcpy(dst, a, a_len);
    memcpy(dst + a_len, b, b_len);
    memcpy(dst + a_len + b_len, c, c_len + 1u);
    return true;
}

static bool logger_parse_manifest_summary(const char *json, logger_manifest_summary_t *summary) {
    memset(summary, 0, sizeof(*summary));

    static jsmntok_t tokens[LOGGER_MANIFEST_JSON_TOKEN_MAX];
    logger_json_doc_t doc;
    if (!logger_json_parse(&doc, json, strlen(json), tokens, LOGGER_MANIFEST_JSON_TOKEN_MAX)) {
        return false;
    }

    const jsmntok_t *root = logger_json_root(&doc);
    if (root == NULL || root->type != JSMN_OBJECT) {
        return false;
    }

    const jsmntok_t *session_tok = logger_json_object_get(&doc, root, "session");
    if (session_tok == NULL || session_tok->type != JSMN_OBJECT) {
        return false;
    }

    return logger_json_object_copy_string(&doc, root, "session_id", summary->session_id, sizeof(summary->session_id)) &&
           logger_json_object_copy_string(&doc, root, "study_day_local", summary->study_day_local, sizeof(summary->study_day_local)) &&
           logger_json_object_copy_string_or_null(&doc,
                                                  session_tok,
                                                  "start_utc",
                                                  summary->session_start_utc,
                                                  sizeof(summary->session_start_utc)) &&
           logger_json_object_copy_string_or_null(&doc,
                                                  session_tok,
                                                  "end_utc",
                                                  summary->session_end_utc,
                                                  sizeof(summary->session_end_utc)) &&
           logger_json_object_get_bool(&doc, session_tok, "quarantined", &summary->quarantined);
}

static void logger_log_local_corrupt(
    logger_system_log_t *system_log,
    const char *updated_at_utc_or_null,
    const char *dir_name,
    const char *reason) {
    if (system_log == NULL) {
        return;
    }
    char dir_escaped[160];
    char reason_escaped[96];
    logger_json_escape_into(dir_escaped, sizeof(dir_escaped), dir_name);
    logger_json_escape_into(reason_escaped, sizeof(reason_escaped), reason);
    char details[320];
    snprintf(details,
             sizeof(details),
             "{\"dir_name\":\"%s\",\"reason\":\"%s\"}",
             dir_escaped,
             reason_escaped);
    (void)logger_system_log_append(
        system_log,
        updated_at_utc_or_null,
        "local_session_corrupt",
        LOGGER_SYSTEM_LOG_SEVERITY_WARN,
        details);
}

static void logger_log_queue_missing_local(
    logger_system_log_t *system_log,
    const char *updated_at_utc_or_null,
    const logger_upload_queue_entry_t *entry) {
    if (system_log == NULL || entry == NULL) {
        return;
    }
    char dir_escaped[160];
    char session_escaped[96];
    logger_json_escape_into(dir_escaped, sizeof(dir_escaped), entry->dir_name);
    logger_json_escape_into(session_escaped, sizeof(session_escaped), entry->session_id);
    char details[320];
    snprintf(details,
             sizeof(details),
             "{\"dir_name\":\"%s\",\"session_id\":\"%s\"}",
             dir_escaped,
             session_escaped);
    (void)logger_system_log_append(
        system_log,
        updated_at_utc_or_null,
        "queue_missing_local_removed",
        LOGGER_SYSTEM_LOG_SEVERITY_WARN,
        details);
}

static void logger_log_queue_rebuilt(
    logger_system_log_t *system_log,
    const char *updated_at_utc_or_null,
    const char *reason) {
    if (system_log == NULL) {
        return;
    }
    char reason_escaped[96];
    logger_json_escape_into(reason_escaped, sizeof(reason_escaped), reason);
    char details[160];
    snprintf(details, sizeof(details), "{\"reason\":\"%s\"}", reason_escaped);
    (void)logger_system_log_append(
        system_log,
        updated_at_utc_or_null,
        "upload_queue_rebuilt",
        LOGGER_SYSTEM_LOG_SEVERITY_WARN,
        details);
}

static void logger_log_queue_requeued(
    logger_system_log_t *system_log,
    const char *updated_at_utc_or_null,
    const char *reason,
    size_t count) {
    if (system_log == NULL) {
        return;
    }
    char reason_escaped[96];
    logger_json_escape_into(reason_escaped, sizeof(reason_escaped), reason);
    char details[192];
    snprintf(details,
             sizeof(details),
             "{\"reason\":\"%s\",\"count\":%lu}",
             reason_escaped,
             (unsigned long)count);
    (void)logger_system_log_append(
        system_log,
        updated_at_utc_or_null,
        "upload_queue_requeued",
        LOGGER_SYSTEM_LOG_SEVERITY_INFO,
        details);
}

static void logger_log_session_pruned(
    logger_system_log_t *system_log,
    const char *updated_at_utc_or_null,
    const logger_upload_queue_entry_t *entry,
    const char *reason) {
    if (system_log == NULL || entry == NULL) {
        return;
    }
    char session_escaped[96];
    char dir_escaped[160];
    char reason_escaped[64];
    logger_json_escape_into(session_escaped, sizeof(session_escaped), entry->session_id);
    logger_json_escape_into(dir_escaped, sizeof(dir_escaped), entry->dir_name);
    logger_json_escape_into(reason_escaped, sizeof(reason_escaped), reason);
    char details[384];
    snprintf(details,
             sizeof(details),
             "{\"session_id\":\"%s\",\"dir_name\":\"%s\",\"reason\":\"%s\"}",
             session_escaped,
             dir_escaped,
             reason_escaped);
    (void)logger_system_log_append(
        system_log,
        updated_at_utc_or_null,
        "session_pruned",
        LOGGER_SYSTEM_LOG_SEVERITY_INFO,
        details);
}

static void logger_log_upload_interrupted(
    logger_system_log_t *system_log,
    const char *updated_at_utc_or_null,
    const logger_upload_queue_entry_t *entry) {
    if (system_log == NULL || entry == NULL) {
        return;
    }
    char session_escaped[96];
    logger_json_escape_into(session_escaped, sizeof(session_escaped), entry->session_id);
    char details[160];
    snprintf(details, sizeof(details), "{\"session_id\":\"%s\"}", session_escaped);
    (void)logger_system_log_append(
        system_log,
        updated_at_utc_or_null,
        "upload_interrupted_recovered",
        LOGGER_SYSTEM_LOG_SEVERITY_WARN,
        details);
}

static bool logger_queue_status_valid(const char *status) {
    return strcmp(status, "pending") == 0 ||
           strcmp(status, "uploading") == 0 ||
           strcmp(status, "verified") == 0 ||
           strcmp(status, "blocked_min_firmware") == 0 ||
           strcmp(status, "failed") == 0;
}

static bool logger_queue_entry_immutable_match(
    const logger_upload_queue_entry_t *a,
    const logger_upload_queue_entry_t *b) {
    return a != NULL &&
           b != NULL &&
           strcmp(a->session_id, b->session_id) == 0 &&
           strcmp(a->study_day_local, b->study_day_local) == 0 &&
           strcmp(a->dir_name, b->dir_name) == 0 &&
           strcmp(a->session_start_utc, b->session_start_utc) == 0 &&
           strcmp(a->session_end_utc, b->session_end_utc) == 0 &&
           strcmp(a->bundle_sha256, b->bundle_sha256) == 0 &&
           a->bundle_size_bytes == b->bundle_size_bytes &&
           a->quarantined == b->quarantined;
}

static void logger_queue_copy_mutable_fields(
    logger_upload_queue_entry_t *dst,
    const logger_upload_queue_entry_t *src) {
    logger_copy_string(dst->status, sizeof(dst->status), src->status);
    dst->attempt_count = src->attempt_count;
    logger_copy_string(dst->last_attempt_utc, sizeof(dst->last_attempt_utc), src->last_attempt_utc);
    logger_copy_string(dst->last_failure_class, sizeof(dst->last_failure_class), src->last_failure_class);
    logger_copy_string(dst->verified_upload_utc, sizeof(dst->verified_upload_utc), src->verified_upload_utc);
    logger_copy_string(dst->receipt_id, sizeof(dst->receipt_id), src->receipt_id);
}

static bool logger_queue_entry_dir_exists(const logger_upload_queue_entry_t *entry) {
    if (entry == NULL || !logger_string_present(entry->dir_name)) {
        return false;
    }
    char dir_path[LOGGER_STORAGE_PATH_MAX];
    if (!logger_path_join2(dir_path, sizeof(dir_path), LOGGER_SESSIONS_DIR "/", entry->dir_name)) {
        return false;
    }
    FILINFO info;
    memset(&info, 0, sizeof(info));
    return f_stat(dir_path, &info) == FR_OK && (info.fattrib & AM_DIR) != 0u;
}

static bool logger_parse_queue_entry_json(
    const logger_json_doc_t *doc,
    const jsmntok_t *object_tok,
    logger_upload_queue_entry_t *entry) {
    memset(entry, 0, sizeof(*entry));
    uint32_t attempt_count = 0u;

    if (doc == NULL || object_tok == NULL || object_tok->type != JSMN_OBJECT) {
        return false;
    }

    if (!logger_json_object_copy_string(doc, object_tok, "session_id", entry->session_id, sizeof(entry->session_id)) ||
        !logger_json_object_copy_string(doc, object_tok, "study_day_local", entry->study_day_local, sizeof(entry->study_day_local)) ||
        !logger_json_object_copy_string(doc, object_tok, "dir_name", entry->dir_name, sizeof(entry->dir_name)) ||
        !logger_json_object_copy_string_or_null(doc,
                                                object_tok,
                                                "session_start_utc",
                                                entry->session_start_utc,
                                                sizeof(entry->session_start_utc)) ||
        !logger_json_object_copy_string_or_null(doc,
                                                object_tok,
                                                "session_end_utc",
                                                entry->session_end_utc,
                                                sizeof(entry->session_end_utc)) ||
        !logger_json_object_copy_string(doc, object_tok, "bundle_sha256", entry->bundle_sha256, sizeof(entry->bundle_sha256)) ||
        !logger_json_object_get_uint64(doc, object_tok, "bundle_size_bytes", &entry->bundle_size_bytes) ||
        !logger_json_object_get_bool(doc, object_tok, "quarantined", &entry->quarantined) ||
        !logger_json_object_copy_string(doc, object_tok, "status", entry->status, sizeof(entry->status)) ||
        !logger_json_object_get_uint32(doc, object_tok, "attempt_count", &attempt_count) ||
        !logger_json_object_copy_string_or_null(doc,
                                                object_tok,
                                                "last_attempt_utc",
                                                entry->last_attempt_utc,
                                                sizeof(entry->last_attempt_utc)) ||
        !logger_json_object_copy_string_or_null(doc,
                                                object_tok,
                                                "last_failure_class",
                                                entry->last_failure_class,
                                                sizeof(entry->last_failure_class)) ||
        !logger_json_object_copy_string_or_null(doc,
                                                object_tok,
                                                "verified_upload_utc",
                                                entry->verified_upload_utc,
                                                sizeof(entry->verified_upload_utc)) ||
        !logger_json_object_copy_string_or_null(doc,
                                                object_tok,
                                                "receipt_id",
                                                entry->receipt_id,
                                                sizeof(entry->receipt_id))) {
        return false;
    }

    if (!logger_queue_status_valid(entry->status)) {
        return false;
    }

    entry->attempt_count = attempt_count;
    return true;
}

static int logger_upload_queue_entry_compare(
    const logger_upload_queue_entry_t *a,
    const logger_upload_queue_entry_t *b) {
    int cmp = strcmp(a->study_day_local, b->study_day_local);
    if (cmp != 0) {
        return cmp;
    }
    cmp = strcmp(a->session_start_utc, b->session_start_utc);
    if (cmp != 0) {
        return cmp;
    }
    return strcmp(a->session_id, b->session_id);
}

static void logger_upload_queue_sort(logger_upload_queue_t *queue) {
    for (size_t i = 1u; i < queue->session_count; ++i) {
        logger_upload_queue_entry_t value = queue->sessions[i];
        size_t j = i;
        while (j > 0u && logger_upload_queue_entry_compare(&value, &queue->sessions[j - 1u]) < 0) {
            queue->sessions[j] = queue->sessions[j - 1u];
            --j;
        }
        queue->sessions[j] = value;
    }
}

static void logger_upload_queue_remove_at(logger_upload_queue_t *queue, size_t index) {
    if (queue == NULL || index >= queue->session_count) {
        return;
    }
    for (size_t i = index + 1u; i < queue->session_count; ++i) {
        queue->sessions[i - 1u] = queue->sessions[i];
    }
    if (queue->session_count > 0u) {
        queue->session_count -= 1u;
        memset(&queue->sessions[queue->session_count], 0, sizeof(queue->sessions[queue->session_count]));
    }
}

static int64_t logger_days_from_civil(int year, unsigned month, unsigned day) {
    year -= month <= 2u;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yoe = (unsigned)(year - era * 400);
    const unsigned doy = (153u * (month + (month > 2u ? (unsigned)-3 : 9u)) + 2u) / 5u + day - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

static bool logger_parse_rfc3339_utc_seconds(const char *text, int64_t *seconds_out) {
    if (text == NULL || seconds_out == NULL || strlen(text) != 20u) {
        return false;
    }

    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    if (sscanf(text,
               "%4d-%2d-%2dT%2d:%2d:%2dZ",
               &year,
               &month,
               &day,
               &hour,
               &minute,
               &second) != 6) {
        return false;
    }
    if (month < 1 || month > 12 || day < 1 || day > 31 ||
        hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 60) {
        return false;
    }

    const int64_t days = logger_days_from_civil(year, (unsigned)month, (unsigned)day);
    *seconds_out = (days * 86400) + ((int64_t)hour * 3600) + ((int64_t)minute * 60) + (int64_t)second;
    return true;
}

static bool logger_queue_entry_retention_expired(
    const logger_upload_queue_entry_t *entry,
    const char *now_utc_or_null) {
    if (entry == NULL || strcmp(entry->status, "verified") != 0) {
        return false;
    }
    int64_t now_seconds = 0;
    int64_t verified_seconds = 0;
    if (!logger_parse_rfc3339_utc_seconds(now_utc_or_null, &now_seconds) ||
        !logger_parse_rfc3339_utc_seconds(entry->verified_upload_utc, &verified_seconds)) {
        return false;
    }
    return now_seconds >= verified_seconds &&
           (uint64_t)(now_seconds - verified_seconds) >= (uint64_t)LOGGER_UPLOAD_RETENTION_SECONDS;
}

static ssize_t logger_upload_queue_find_oldest_verified(const logger_upload_queue_t *queue) {
    if (queue == NULL) {
        return -1;
    }
    for (size_t i = 0u; i < queue->session_count; ++i) {
        if (strcmp(queue->sessions[i].status, "verified") == 0) {
            return (ssize_t)i;
        }
    }
    return -1;
}

static bool logger_remove_closed_session_dir(const char *dir_name) {
    if (!logger_string_present(dir_name)) {
        return false;
    }

    char dir_path[LOGGER_STORAGE_PATH_MAX];
    if (!logger_path_join2(dir_path, sizeof(dir_path), LOGGER_SESSIONS_DIR "/", dir_name)) {
        return false;
    }

    DIR dir;
    if (f_opendir(&dir, dir_path) != FR_OK) {
        return false;
    }

    bool ok = true;
    for (;;) {
        FILINFO info;
        memset(&info, 0, sizeof(info));
        const FRESULT fr = f_readdir(&dir, &info);
        if (fr != FR_OK) {
            ok = false;
            break;
        }
        if (info.fname[0] == '\0') {
            break;
        }
        if (strcmp(info.fname, ".") == 0 || strcmp(info.fname, "..") == 0) {
            continue;
        }
        if ((info.fattrib & AM_DIR) != 0u) {
            ok = false;
            break;
        }

        char child_path[LOGGER_STORAGE_PATH_MAX];
        if (!logger_path_join3(child_path, sizeof(child_path), dir_path, "/", info.fname) ||
            f_unlink(child_path) != FR_OK) {
            ok = false;
            break;
        }
    }
    (void)f_closedir(&dir);
    if (!ok) {
        return false;
    }
    return f_unlink(dir_path) == FR_OK;
}

static bool logger_file_write_all(FIL *file, const void *data, size_t len) {
    UINT written = 0u;
    return f_write(file, data, len, &written) == FR_OK && written == len;
}

static bool logger_file_write_cstr(FIL *file, const char *text) {
    return logger_file_write_all(file, text, strlen(text));
}

static bool logger_file_write_json_string_or_null(FIL *file, const char *value) {
    char quoted[544];
    logger_json_string_literal(quoted, sizeof(quoted), value);
    return logger_file_write_cstr(file, quoted);
}

const logger_upload_queue_entry_t *logger_upload_queue_find_by_session_id(
    const logger_upload_queue_t *queue,
    const char *session_id) {
    if (queue == NULL || !logger_string_present(session_id)) {
        return NULL;
    }
    for (size_t i = 0u; i < queue->session_count; ++i) {
        if (strcmp(queue->sessions[i].session_id, session_id) == 0) {
            return &queue->sessions[i];
        }
    }
    return NULL;
}

void logger_upload_queue_init(logger_upload_queue_t *queue) {
    memset(queue, 0, sizeof(*queue));
}

void logger_upload_queue_summary_init(logger_upload_queue_summary_t *summary) {
    memset(summary, 0, sizeof(*summary));
}

void logger_upload_queue_compute_summary(
    const logger_upload_queue_t *queue,
    logger_upload_queue_summary_t *summary) {
    logger_upload_queue_summary_init(summary);
    if (queue == NULL) {
        return;
    }

    summary->available = true;
    logger_copy_string(summary->updated_at_utc, sizeof(summary->updated_at_utc), queue->updated_at_utc);
    summary->session_count = (uint32_t)queue->session_count;

    char latest_failure_at[LOGGER_UPLOAD_QUEUE_UTC_MAX + 1] = {0};
    bool have_failure = false;
    for (size_t i = 0u; i < queue->session_count; ++i) {
        const logger_upload_queue_entry_t *entry = &queue->sessions[i];
        if (strcmp(entry->status, "blocked_min_firmware") == 0) {
            summary->blocked_count += 1u;
        }
        if (strcmp(entry->status, "pending") == 0 ||
            strcmp(entry->status, "failed") == 0 ||
            strcmp(entry->status, "uploading") == 0) {
            if (summary->pending_count == 0u) {
                logger_copy_string(summary->oldest_pending_study_day,
                                   sizeof(summary->oldest_pending_study_day),
                                   entry->study_day_local);
            }
            summary->pending_count += 1u;
        }
        if (logger_string_present(entry->last_failure_class)) {
            if (!have_failure ||
                (!logger_string_present(latest_failure_at) && logger_string_present(entry->last_attempt_utc)) ||
                strcmp(entry->last_attempt_utc, latest_failure_at) >= 0) {
                logger_copy_string(summary->last_failure_class,
                                   sizeof(summary->last_failure_class),
                                   entry->last_failure_class);
                logger_copy_string(latest_failure_at,
                                   sizeof(latest_failure_at),
                                   entry->last_attempt_utc);
                have_failure = true;
            }
        }
    }
}

bool logger_upload_queue_load(logger_upload_queue_t *queue) {
    logger_upload_queue_init(queue);

    logger_storage_status_t storage;
    (void)logger_storage_refresh(&storage);
    if (!storage.mounted) {
        return false;
    }
    if (!logger_storage_file_exists(LOGGER_QUEUE_PATH)) {
        return true;
    }

    static char queue_json[LOGGER_QUEUE_READ_MAX + 1u];
    size_t len = 0u;
    if (!logger_storage_read_file(LOGGER_QUEUE_PATH, queue_json, LOGGER_QUEUE_READ_MAX, &len)) {
        return false;
    }
    queue_json[len] = '\0';

    static jsmntok_t tokens[LOGGER_QUEUE_JSON_TOKEN_MAX];
    logger_json_doc_t doc;
    if (!logger_json_parse(&doc, queue_json, len, tokens, LOGGER_QUEUE_JSON_TOKEN_MAX)) {
        return false;
    }

    const jsmntok_t *root = logger_json_root(&doc);
    if (root == NULL || root->type != JSMN_OBJECT) {
        return false;
    }

    uint32_t schema_version = 0u;
    if (!logger_json_object_get_uint32(&doc, root, "schema_version", &schema_version) || schema_version != 1u) {
        return false;
    }

    if (!logger_json_object_copy_string_or_null(&doc, root, "updated_at_utc", queue->updated_at_utc, sizeof(queue->updated_at_utc))) {
        return false;
    }

    const jsmntok_t *sessions_tok = logger_json_object_get(&doc, root, "sessions");
    if (sessions_tok == NULL || sessions_tok->type != JSMN_ARRAY || (size_t)sessions_tok->size > LOGGER_UPLOAD_QUEUE_MAX_SESSIONS) {
        return false;
    }

    for (size_t i = 0u; i < (size_t)sessions_tok->size; ++i) {
        const jsmntok_t *entry_tok = logger_json_array_get(&doc, sessions_tok, i);
        if (entry_tok == NULL || entry_tok->type != JSMN_OBJECT || queue->session_count >= LOGGER_UPLOAD_QUEUE_MAX_SESSIONS) {
            return false;
        }
        logger_upload_queue_entry_t entry;
        if (!logger_parse_queue_entry_json(&doc, entry_tok, &entry) ||
            logger_upload_queue_find_by_session_id(queue, entry.session_id) != NULL) {
            return false;
        }

        queue->sessions[queue->session_count++] = entry;
    }

    logger_upload_queue_sort(queue);
    return true;
}

bool logger_upload_queue_scan(
    logger_upload_queue_t *queue,
    logger_system_log_t *system_log,
    const char *updated_at_utc_or_null) {
    logger_upload_queue_init(queue);

    logger_storage_status_t storage;
    (void)logger_storage_refresh(&storage);
    if (!storage.mounted) {
        return false;
    }

    logger_copy_string(queue->updated_at_utc, sizeof(queue->updated_at_utc), updated_at_utc_or_null);

    DIR dir;
    if (f_opendir(&dir, LOGGER_SESSIONS_DIR) != FR_OK) {
        return false;
    }

    for (;;) {
        FILINFO info;
        memset(&info, 0, sizeof(info));
        const FRESULT fr = f_readdir(&dir, &info);
        if (fr != FR_OK || info.fname[0] == '\0') {
            break;
        }
        if ((info.fattrib & AM_DIR) == 0u || strcmp(info.fname, ".") == 0 || strcmp(info.fname, "..") == 0) {
            continue;
        }
        if (strstr(info.fname, ".tmp") != NULL) {
            continue;
        }

        char manifest_path[LOGGER_STORAGE_PATH_MAX];
        char journal_path[LOGGER_STORAGE_PATH_MAX];
        char live_path[LOGGER_STORAGE_PATH_MAX];
        if (!logger_path_join3(manifest_path, sizeof(manifest_path), LOGGER_SESSIONS_DIR "/", info.fname, "/manifest.json") ||
            !logger_path_join3(journal_path, sizeof(journal_path), LOGGER_SESSIONS_DIR "/", info.fname, "/journal.bin") ||
            !logger_path_join3(live_path, sizeof(live_path), LOGGER_SESSIONS_DIR "/", info.fname, "/live.json")) {
            logger_log_local_corrupt(system_log, updated_at_utc_or_null, info.fname, "path_too_long");
            continue;
        }

        if (logger_storage_file_exists(live_path)) {
            continue;
        }
        if (!logger_storage_file_exists(manifest_path) || !logger_storage_file_exists(journal_path)) {
            continue;
        }
        if (queue->session_count >= LOGGER_UPLOAD_QUEUE_MAX_SESSIONS) {
            logger_log_local_corrupt(system_log, updated_at_utc_or_null, info.fname, "queue_capacity_exhausted");
            break;
        }

        static char manifest_buf[LOGGER_MANIFEST_READ_MAX + 1u];
        size_t manifest_len = 0u;
        if (!logger_storage_read_file(manifest_path, manifest_buf, LOGGER_MANIFEST_READ_MAX, &manifest_len)) {
            logger_log_local_corrupt(system_log, updated_at_utc_or_null, info.fname, "manifest_read_failed");
            continue;
        }
        manifest_buf[manifest_len] = '\0';

        logger_manifest_summary_t manifest;
        if (!logger_parse_manifest_summary(manifest_buf, &manifest)) {
            logger_log_local_corrupt(system_log, updated_at_utc_or_null, info.fname, "manifest_parse_failed");
            continue;
        }

        logger_upload_queue_entry_t *entry = &queue->sessions[queue->session_count];
        memset(entry, 0, sizeof(*entry));
        logger_copy_string(entry->session_id, sizeof(entry->session_id), manifest.session_id);
        logger_copy_string(entry->study_day_local, sizeof(entry->study_day_local), manifest.study_day_local);
        logger_copy_string(entry->dir_name, sizeof(entry->dir_name), info.fname);
        logger_copy_string(entry->session_start_utc, sizeof(entry->session_start_utc), manifest.session_start_utc);
        logger_copy_string(entry->session_end_utc, sizeof(entry->session_end_utc), manifest.session_end_utc);
        entry->quarantined = manifest.quarantined;
        logger_copy_string(entry->status, sizeof(entry->status), "pending");

        if (!logger_upload_bundle_compute(
                info.fname,
                manifest_path,
                journal_path,
                entry->bundle_sha256,
                &entry->bundle_size_bytes)) {
            logger_log_local_corrupt(system_log, updated_at_utc_or_null, info.fname, "bundle_hash_failed");
            continue;
        }

        queue->session_count += 1u;
    }

    (void)f_closedir(&dir);
    logger_upload_queue_sort(queue);
    return true;
}

bool logger_upload_queue_write(const logger_upload_queue_t *queue) {
    logger_storage_status_t storage;
    (void)logger_storage_refresh(&storage);
    if (!storage.mounted || !storage.writable) {
        return false;
    }

    FIL file;
    if (f_open(&file, LOGGER_QUEUE_TMP_PATH, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) {
        return false;
    }

    bool ok = true;
    ok = ok && logger_file_write_cstr(&file, "{\"schema_version\":1,\"updated_at_utc\":");
    ok = ok && logger_file_write_json_string_or_null(&file, queue->updated_at_utc);
    ok = ok && logger_file_write_cstr(&file, ",\"sessions\":[");

    for (size_t i = 0u; ok && i < queue->session_count; ++i) {
        const logger_upload_queue_entry_t *entry = &queue->sessions[i];
        char number_buf[32];

        ok = ok && logger_file_write_cstr(&file, i == 0u ? "{" : ",{");

        ok = ok && logger_file_write_cstr(&file, "\"session_id\":");
        ok = ok && logger_file_write_json_string_or_null(&file, entry->session_id);
        ok = ok && logger_file_write_cstr(&file, ",\"study_day_local\":");
        ok = ok && logger_file_write_json_string_or_null(&file, entry->study_day_local);
        ok = ok && logger_file_write_cstr(&file, ",\"dir_name\":");
        ok = ok && logger_file_write_json_string_or_null(&file, entry->dir_name);
        ok = ok && logger_file_write_cstr(&file, ",\"session_start_utc\":");
        ok = ok && logger_file_write_json_string_or_null(&file, entry->session_start_utc);
        ok = ok && logger_file_write_cstr(&file, ",\"session_end_utc\":");
        ok = ok && logger_file_write_json_string_or_null(&file, entry->session_end_utc);
        ok = ok && logger_file_write_cstr(&file, ",\"bundle_sha256\":");
        ok = ok && logger_file_write_json_string_or_null(&file, entry->bundle_sha256);
        ok = ok && logger_file_write_cstr(&file, ",\"bundle_size_bytes\":");
        const int bundle_n = snprintf(number_buf, sizeof(number_buf), "%llu", (unsigned long long)entry->bundle_size_bytes);
        ok = ok && bundle_n > 0 && (size_t)bundle_n < sizeof(number_buf) && logger_file_write_all(&file, number_buf, (size_t)bundle_n);
        ok = ok && logger_file_write_cstr(&file, ",\"quarantined\":");
        ok = ok && logger_file_write_cstr(&file, entry->quarantined ? "true" : "false");
        ok = ok && logger_file_write_cstr(&file, ",\"status\":");
        ok = ok && logger_file_write_json_string_or_null(&file, entry->status);
        ok = ok && logger_file_write_cstr(&file, ",\"attempt_count\":");
        const int attempt_n = snprintf(number_buf, sizeof(number_buf), "%lu", (unsigned long)entry->attempt_count);
        ok = ok && attempt_n > 0 && (size_t)attempt_n < sizeof(number_buf) && logger_file_write_all(&file, number_buf, (size_t)attempt_n);
        ok = ok && logger_file_write_cstr(&file, ",\"last_attempt_utc\":");
        ok = ok && logger_file_write_json_string_or_null(&file, entry->last_attempt_utc);
        ok = ok && logger_file_write_cstr(&file, ",\"last_failure_class\":");
        ok = ok && logger_file_write_json_string_or_null(&file, entry->last_failure_class);
        ok = ok && logger_file_write_cstr(&file, ",\"verified_upload_utc\":");
        ok = ok && logger_file_write_json_string_or_null(&file, entry->verified_upload_utc);
        ok = ok && logger_file_write_cstr(&file, ",\"receipt_id\":");
        ok = ok && logger_file_write_json_string_or_null(&file, entry->receipt_id);
        ok = ok && logger_file_write_cstr(&file, "}");
    }

    ok = ok && logger_file_write_cstr(&file, "]}");
    const FRESULT sync_fr = ok ? f_sync(&file) : FR_INT_ERR;
    const FRESULT close_fr = f_close(&file);
    if (!ok || sync_fr != FR_OK || close_fr != FR_OK) {
        (void)f_unlink(LOGGER_QUEUE_TMP_PATH);
        return false;
    }

    (void)f_unlink(LOGGER_QUEUE_PATH);
    if (f_rename(LOGGER_QUEUE_TMP_PATH, LOGGER_QUEUE_PATH) != FR_OK) {
        (void)f_unlink(LOGGER_QUEUE_TMP_PATH);
        return false;
    }
    return true;
}

bool logger_upload_queue_rebuild_file(
    logger_system_log_t *system_log,
    const char *updated_at_utc_or_null,
    logger_upload_queue_summary_t *summary_out) {
    logger_upload_queue_t rebuilt;
    if (!logger_upload_queue_scan(&rebuilt, system_log, updated_at_utc_or_null)) {
        if (summary_out != NULL) {
            logger_upload_queue_summary_init(summary_out);
        }
        return false;
    }

    logger_upload_queue_sort(&rebuilt);
    if (!logger_upload_queue_write(&rebuilt)) {
        if (summary_out != NULL) {
            logger_upload_queue_summary_init(summary_out);
        }
        return false;
    }

    logger_log_queue_rebuilt(system_log, updated_at_utc_or_null, "manual_rebuild");
    if (summary_out != NULL) {
        logger_upload_queue_compute_summary(&rebuilt, summary_out);
    }
    return true;
}

bool logger_upload_queue_requeue_blocked_file(
    logger_system_log_t *system_log,
    const char *updated_at_utc_or_null,
    const char *reason,
    size_t *requeued_count_out,
    logger_upload_queue_summary_t *summary_out) {
    if (requeued_count_out != NULL) {
        *requeued_count_out = 0u;
    }

    logger_upload_queue_t queue;
    if (!logger_upload_queue_load(&queue)) {
        if (summary_out != NULL) {
            logger_upload_queue_summary_init(summary_out);
        }
        return false;
    }

    size_t requeued_count = 0u;
    for (size_t i = 0u; i < queue.session_count; ++i) {
        logger_upload_queue_entry_t *entry = &queue.sessions[i];
        if (strcmp(entry->status, "blocked_min_firmware") != 0) {
            continue;
        }
        logger_copy_string(entry->status, sizeof(entry->status), "pending");
        entry->last_failure_class[0] = '\0';
        entry->verified_upload_utc[0] = '\0';
        entry->receipt_id[0] = '\0';
        requeued_count += 1u;
    }

    if (requeued_count > 0u) {
        logger_copy_string(queue.updated_at_utc, sizeof(queue.updated_at_utc), updated_at_utc_or_null);
        logger_upload_queue_sort(&queue);
        if (!logger_upload_queue_write(&queue)) {
            if (summary_out != NULL) {
                logger_upload_queue_summary_init(summary_out);
            }
            return false;
        }
        logger_log_queue_requeued(system_log,
                                  updated_at_utc_or_null,
                                  logger_string_present(reason) ? reason : "manual_requeue_blocked",
                                  requeued_count);
    }

    if (requeued_count_out != NULL) {
        *requeued_count_out = requeued_count;
    }
    if (summary_out != NULL) {
        logger_upload_queue_compute_summary(&queue, summary_out);
    }
    return true;
}

bool logger_upload_queue_prune_file(
    logger_system_log_t *system_log,
    const char *updated_at_utc_or_null,
    uint64_t reserve_bytes,
    size_t *retention_pruned_count_out,
    size_t *reserve_pruned_count_out,
    bool *reserve_met_out,
    logger_upload_queue_summary_t *summary_out) {
    if (retention_pruned_count_out != NULL) {
        *retention_pruned_count_out = 0u;
    }
    if (reserve_pruned_count_out != NULL) {
        *reserve_pruned_count_out = 0u;
    }
    if (reserve_met_out != NULL) {
        *reserve_met_out = false;
    }

    logger_storage_status_t storage;
    if (!logger_storage_refresh(&storage) || !storage.mounted || !storage.writable || !storage.logger_root_ready) {
        if (summary_out != NULL) {
            logger_upload_queue_summary_init(summary_out);
        }
        return false;
    }

    logger_upload_queue_t queue;
    if (!logger_upload_queue_load(&queue)) {
        if (summary_out != NULL) {
            logger_upload_queue_summary_init(summary_out);
        }
        return false;
    }

    size_t retention_pruned_count = 0u;
    size_t reserve_pruned_count = 0u;
    bool changed = false;

    for (size_t i = 0u; i < queue.session_count;) {
        if (!logger_queue_entry_retention_expired(&queue.sessions[i], updated_at_utc_or_null)) {
            ++i;
            continue;
        }

        logger_upload_queue_entry_t pruned = queue.sessions[i];
        if (!logger_remove_closed_session_dir(pruned.dir_name)) {
            if (summary_out != NULL) {
                logger_upload_queue_summary_init(summary_out);
            }
            return false;
        }
        logger_log_session_pruned(system_log, updated_at_utc_or_null, &pruned, "retention_expired");
        logger_upload_queue_remove_at(&queue, i);
        retention_pruned_count += 1u;
        changed = true;
    }

    if (!logger_storage_refresh(&storage)) {
        if (summary_out != NULL) {
            logger_upload_queue_summary_init(summary_out);
        }
        return false;
    }

    while (storage.free_bytes < reserve_bytes) {
        const ssize_t oldest_verified_index = logger_upload_queue_find_oldest_verified(&queue);
        if (oldest_verified_index < 0) {
            break;
        }

        const size_t index = (size_t)oldest_verified_index;
        logger_upload_queue_entry_t pruned = queue.sessions[index];
        if (!logger_remove_closed_session_dir(pruned.dir_name)) {
            if (summary_out != NULL) {
                logger_upload_queue_summary_init(summary_out);
            }
            return false;
        }
        logger_log_session_pruned(system_log, updated_at_utc_or_null, &pruned, "reserve_protection");
        logger_upload_queue_remove_at(&queue, index);
        reserve_pruned_count += 1u;
        changed = true;

        if (!logger_storage_refresh(&storage)) {
            if (summary_out != NULL) {
                logger_upload_queue_summary_init(summary_out);
            }
            return false;
        }
    }

    if (changed) {
        logger_copy_string(queue.updated_at_utc, sizeof(queue.updated_at_utc), updated_at_utc_or_null);
        logger_upload_queue_sort(&queue);
        if (!logger_upload_queue_write(&queue)) {
            if (summary_out != NULL) {
                logger_upload_queue_summary_init(summary_out);
            }
            return false;
        }
    }

    if (retention_pruned_count_out != NULL) {
        *retention_pruned_count_out = retention_pruned_count;
    }
    if (reserve_pruned_count_out != NULL) {
        *reserve_pruned_count_out = reserve_pruned_count;
    }
    if (reserve_met_out != NULL) {
        *reserve_met_out = storage.free_bytes >= reserve_bytes;
    }
    if (summary_out != NULL) {
        logger_upload_queue_compute_summary(&queue, summary_out);
    }
    return true;
}

bool logger_upload_queue_refresh_file(
    logger_system_log_t *system_log,
    const char *updated_at_utc_or_null,
    logger_upload_queue_summary_t *summary_out) {
    logger_upload_queue_t scanned;
    if (!logger_upload_queue_scan(&scanned, system_log, updated_at_utc_or_null)) {
        if (summary_out != NULL) {
            logger_upload_queue_summary_init(summary_out);
        }
        return false;
    }

    logger_upload_queue_t previous;
    logger_upload_queue_init(&previous);
    const bool queue_file_present = logger_storage_file_exists(LOGGER_QUEUE_PATH);
    const bool loaded_previous = logger_upload_queue_load(&previous);
    if (!loaded_previous && queue_file_present) {
        logger_log_queue_rebuilt(system_log, updated_at_utc_or_null, "load_failed");
    }

    logger_upload_queue_t merged;
    logger_upload_queue_init(&merged);
    logger_copy_string(merged.updated_at_utc, sizeof(merged.updated_at_utc), updated_at_utc_or_null);

    bool previous_seen[LOGGER_UPLOAD_QUEUE_MAX_SESSIONS];
    memset(previous_seen, 0, sizeof(previous_seen));

    for (size_t i = 0u; i < scanned.session_count; ++i) {
        logger_upload_queue_entry_t entry = scanned.sessions[i];
        if (loaded_previous) {
            for (size_t j = 0u; j < previous.session_count; ++j) {
                if (strcmp(previous.sessions[j].session_id, entry.session_id) == 0) {
                    previous_seen[j] = true;
                    if (logger_queue_entry_immutable_match(&entry, &previous.sessions[j])) {
                        logger_queue_copy_mutable_fields(&entry, &previous.sessions[j]);
                        if (strcmp(entry.status, "uploading") == 0) {
                            logger_copy_string(entry.status, sizeof(entry.status), "failed");
                            logger_copy_string(entry.last_failure_class, sizeof(entry.last_failure_class), "interrupted");
                            logger_log_upload_interrupted(system_log, updated_at_utc_or_null, &entry);
                        }
                    } else {
                        logger_log_local_corrupt(system_log,
                                                 updated_at_utc_or_null,
                                                 entry.dir_name,
                                                 "queue_entry_identity_changed");
                    }
                    break;
                }
            }
        }
        if (merged.session_count < LOGGER_UPLOAD_QUEUE_MAX_SESSIONS) {
            merged.sessions[merged.session_count++] = entry;
        }
    }

    if (loaded_previous) {
        for (size_t i = 0u; i < previous.session_count; ++i) {
            if (previous_seen[i]) {
                continue;
            }
            if (logger_queue_entry_dir_exists(&previous.sessions[i])) {
                logger_log_local_corrupt(system_log,
                                         updated_at_utc_or_null,
                                         previous.sessions[i].dir_name,
                                         "queue_entry_unreconciled");
            } else {
                logger_log_queue_missing_local(system_log, updated_at_utc_or_null, &previous.sessions[i]);
            }
        }
    }

    logger_upload_queue_sort(&merged);
    if (summary_out != NULL) {
        logger_upload_queue_compute_summary(&merged, summary_out);
    }
    return logger_upload_queue_write(&merged);
}