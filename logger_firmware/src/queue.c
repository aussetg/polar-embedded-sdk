#include "logger/queue.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ff.h"

#include "logger/sha256.h"
#include "logger/storage.h"

#define LOGGER_QUEUE_PATH "0:/logger/state/upload_queue.json"
#define LOGGER_QUEUE_TMP_PATH "0:/logger/state/upload_queue.json.tmp"
#define LOGGER_SESSIONS_DIR "0:/logger/sessions"
#define LOGGER_MANIFEST_READ_MAX 8192u
#define LOGGER_QUEUE_READ_MAX 49152u
#define LOGGER_QUEUE_ENTRY_JSON_MAX 1536u

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
        switch (*p) {
            case '\\': replacement = "\\\\"; break;
            case '"': replacement = "\\\""; break;
            case '\n': replacement = "\\n"; break;
            case '\r': replacement = "\\r"; break;
            case '\t': replacement = "\\t"; break;
            default: break;
        }
        if (replacement != NULL) {
            const size_t repl_len = strlen(replacement);
            if ((out + repl_len) >= dst_len) {
                break;
            }
            memcpy(dst + out, replacement, repl_len);
            out += repl_len;
        } else if (*p >= 0x20u) {
            dst[out++] = (char)*p;
        }
    }
    dst[out] = '\0';
}

static bool logger_json_extract_string(const char *json, const char *key, char *out, size_t out_len) {
    const char *start = strstr(json, key);
    if (start == NULL) {
        return false;
    }
    start += strlen(key);
    const char *end = strchr(start, '"');
    if (end == NULL) {
        return false;
    }
    size_t out_i = 0u;
    for (const char *p = start; p < end && (out_i + 1u) < out_len; ++p) {
        if (*p == '\\' && (p + 1) < end) {
            ++p;
            out[out_i++] = *p;
        } else {
            out[out_i++] = *p;
        }
    }
    out[out_i] = '\0';
    return true;
}

static bool logger_json_extract_string_or_null(const char *json, const char *field_name, char *out, size_t out_len) {
    char key[64];
    const int key_n = snprintf(key, sizeof(key), "\"%s\":\"", field_name);
    if (key_n > 0 && (size_t)key_n < sizeof(key) && logger_json_extract_string(json, key, out, out_len)) {
        return true;
    }
    const int null_key_n = snprintf(key, sizeof(key), "\"%s\":null", field_name);
    if (null_key_n > 0 && (size_t)null_key_n < sizeof(key) && strstr(json, key) != NULL) {
        out[0] = '\0';
        return true;
    }
    return false;
}

static bool logger_json_extract_bool(const char *json, const char *key, bool *value_out) {
    const char *start = strstr(json, key);
    if (start == NULL || value_out == NULL) {
        return false;
    }
    start += strlen(key);
    if (strncmp(start, "true", 4u) == 0) {
        *value_out = true;
        return true;
    }
    if (strncmp(start, "false", 5u) == 0) {
        *value_out = false;
        return true;
    }
    return false;
}

static bool logger_json_extract_uint64(const char *json, const char *key, uint64_t *value_out) {
    const char *start = strstr(json, key);
    if (start == NULL || value_out == NULL) {
        return false;
    }
    start += strlen(key);
    if (*start < '0' || *start > '9') {
        return false;
    }
    char *end = NULL;
    const unsigned long long value = strtoull(start, &end, 10);
    if (end == start) {
        return false;
    }
    *value_out = (uint64_t)value;
    return true;
}

static bool logger_json_extract_uint32(const char *json, const char *key, uint32_t *value_out) {
    uint64_t value = 0u;
    if (!logger_json_extract_uint64(json, key, &value) || value > 0xffffffffu) {
        return false;
    }
    *value_out = (uint32_t)value;
    return true;
}

static const char *logger_json_find_matching(const char *start, char open_ch, char close_ch) {
    if (start == NULL || *start != open_ch) {
        return NULL;
    }

    int depth = 0;
    bool in_string = false;
    bool escaped = false;
    for (const char *p = start; *p != '\0'; ++p) {
        const char ch = *p;
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (ch == '\\') {
                escaped = true;
            } else if (ch == '"') {
                in_string = false;
            }
            continue;
        }

        if (ch == '"') {
            in_string = true;
        } else if (ch == open_ch) {
            depth += 1;
        } else if (ch == close_ch) {
            depth -= 1;
            if (depth == 0) {
                return p;
            }
        }
    }
    return NULL;
}

static bool logger_json_find_session_object_bounds(const char *json, const char **start_out, const char **end_out) {
    const char *session = strstr(json, "\"session\":{");
    if (session == NULL) {
        return false;
    }
    session = strchr(session, '{');
    if (session == NULL) {
        return false;
    }
    const char *end = strstr(session, "},\"spans\":[");
    if (end == NULL) {
        return false;
    }
    *start_out = session;
    *end_out = end + 1;
    return true;
}

static bool logger_parse_manifest_summary(const char *json, logger_manifest_summary_t *summary) {
    memset(summary, 0, sizeof(*summary));
    if (!logger_json_extract_string(json, "\"session_id\":\"", summary->session_id, sizeof(summary->session_id)) ||
        !logger_json_extract_string(json, "\"study_day_local\":\"", summary->study_day_local, sizeof(summary->study_day_local))) {
        return false;
    }

    const char *session_obj = NULL;
    const char *session_obj_end = NULL;
    if (!logger_json_find_session_object_bounds(json, &session_obj, &session_obj_end)) {
        return false;
    }

    char session_json[512];
    const size_t session_len = (size_t)(session_obj_end - session_obj);
    if (session_len >= sizeof(session_json)) {
        return false;
    }
    memcpy(session_json, session_obj, session_len);
    session_json[session_len] = '\0';

    if (!logger_json_extract_string_or_null(session_json, "start_utc", summary->session_start_utc, sizeof(summary->session_start_utc)) ||
        !logger_json_extract_string_or_null(session_json, "end_utc", summary->session_end_utc, sizeof(summary->session_end_utc)) ||
        !logger_json_extract_bool(session_json, "\"quarantined\":", &summary->quarantined)) {
        return false;
    }
    return true;
}

static void logger_sha256_update_zeroes(logger_sha256_t *sha, size_t count) {
    static const uint8_t zeroes[512] = {0};
    while (count > 0u) {
        const size_t chunk = count > sizeof(zeroes) ? sizeof(zeroes) : count;
        logger_sha256_update(sha, zeroes, chunk);
        count -= chunk;
    }
}

static void logger_tar_fill_octal(char *dst, size_t len, uint64_t value) {
    memset(dst, '0', len);
    if (len == 0u) {
        return;
    }
    dst[len - 1u] = ' ';
    if (len >= 2u) {
        dst[len - 2u] = '\0';
    }
    char tmp[32];
    snprintf(tmp, sizeof(tmp), "%llo", (unsigned long long)value);
    const size_t tmp_len = strlen(tmp);
    const size_t copy_len = (tmp_len < (len - 2u)) ? tmp_len : (len - 2u);
    memcpy(dst + (len - 2u - copy_len), tmp + (tmp_len - copy_len), copy_len);
}

static void logger_tar_build_header(
    uint8_t header[512],
    const char *name,
    uint64_t size,
    uint32_t mode,
    char typeflag) {
    memset(header, 0, 512u);
    const size_t name_len = strlen(name);
    if (name_len < 100u) {
        memcpy(header + 0, name, name_len);
    }
    logger_tar_fill_octal((char *)header + 100, 8u, mode);
    logger_tar_fill_octal((char *)header + 108, 8u, 0u);
    logger_tar_fill_octal((char *)header + 116, 8u, 0u);
    logger_tar_fill_octal((char *)header + 124, 12u, size);
    logger_tar_fill_octal((char *)header + 136, 12u, 0u);
    memset(header + 148, ' ', 8u);
    header[156] = (uint8_t)typeflag;
    memcpy(header + 257, "ustar", 5u);
    memcpy(header + 263, "00", 2u);

    unsigned checksum = 0u;
    for (size_t i = 0u; i < 512u; ++i) {
        checksum += header[i];
    }
    logger_tar_fill_octal((char *)header + 148, 8u, checksum);
}

static bool logger_compute_canonical_bundle(
    const char *dir_name,
    const char *manifest_path,
    const char *journal_path,
    char out_sha256[LOGGER_UPLOAD_QUEUE_SHA256_HEX_LEN + 1],
    uint64_t *bundle_size_out) {
    static char manifest_buf[LOGGER_MANIFEST_READ_MAX + 1u];
    size_t manifest_len = 0u;
    if (!logger_storage_read_file(manifest_path, manifest_buf, LOGGER_MANIFEST_READ_MAX, &manifest_len)) {
        return false;
    }

    uint64_t journal_size = 0u;
    if (!logger_storage_file_size(journal_path, &journal_size)) {
        return false;
    }

    logger_sha256_t sha;
    logger_sha256_init(&sha);
    uint64_t total_size = 0u;

    uint8_t header[512];
    char path[LOGGER_STORAGE_PATH_MAX];

    if (!logger_path_join2(path, sizeof(path), dir_name, "/")) {
        return false;
    }
    logger_tar_build_header(header, path, 0u, 0755u, '5');
    logger_sha256_update(&sha, header, sizeof(header));
    total_size += sizeof(header);

    if (!logger_path_join2(path, sizeof(path), dir_name, "/manifest.json")) {
        return false;
    }
    logger_tar_build_header(header, path, manifest_len, 0644u, '0');
    logger_sha256_update(&sha, header, sizeof(header));
    logger_sha256_update(&sha, manifest_buf, manifest_len);
    total_size += sizeof(header) + manifest_len;
    const size_t manifest_pad = (512u - (manifest_len % 512u)) % 512u;
    logger_sha256_update_zeroes(&sha, manifest_pad);
    total_size += manifest_pad;

    if (!logger_path_join2(path, sizeof(path), dir_name, "/journal.bin")) {
        return false;
    }
    logger_tar_build_header(header, path, journal_size, 0644u, '0');
    logger_sha256_update(&sha, header, sizeof(header));
    total_size += sizeof(header);

    FIL file;
    if (f_open(&file, journal_path, FA_READ) != FR_OK) {
        return false;
    }
    uint8_t chunk[256];
    UINT read_bytes = 0u;
    do {
        if (f_read(&file, chunk, sizeof(chunk), &read_bytes) != FR_OK) {
            (void)f_close(&file);
            return false;
        }
        if (read_bytes > 0u) {
            logger_sha256_update(&sha, chunk, read_bytes);
            total_size += read_bytes;
        }
    } while (read_bytes == sizeof(chunk));
    if (f_close(&file) != FR_OK) {
        return false;
    }

    const size_t journal_pad = (size_t)((512u - (journal_size % 512u)) % 512u);
    logger_sha256_update_zeroes(&sha, journal_pad);
    total_size += journal_pad;

    logger_sha256_update_zeroes(&sha, 1024u);
    total_size += 1024u;

    logger_sha256_final_hex(&sha, out_sha256);
    if (bundle_size_out != NULL) {
        *bundle_size_out = total_size;
    }
    return true;
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

static bool logger_parse_queue_entry_json(const char *json, logger_upload_queue_entry_t *entry) {
    memset(entry, 0, sizeof(*entry));
    uint32_t attempt_count = 0u;

    if (!logger_json_extract_string(json, "\"session_id\":\"", entry->session_id, sizeof(entry->session_id)) ||
        !logger_json_extract_string(json, "\"study_day_local\":\"", entry->study_day_local, sizeof(entry->study_day_local)) ||
        !logger_json_extract_string(json, "\"dir_name\":\"", entry->dir_name, sizeof(entry->dir_name)) ||
        !logger_json_extract_string_or_null(json, "session_start_utc", entry->session_start_utc, sizeof(entry->session_start_utc)) ||
        !logger_json_extract_string_or_null(json, "session_end_utc", entry->session_end_utc, sizeof(entry->session_end_utc)) ||
        !logger_json_extract_string(json, "\"bundle_sha256\":\"", entry->bundle_sha256, sizeof(entry->bundle_sha256)) ||
        !logger_json_extract_uint64(json, "\"bundle_size_bytes\":", &entry->bundle_size_bytes) ||
        !logger_json_extract_bool(json, "\"quarantined\":", &entry->quarantined) ||
        !logger_json_extract_string(json, "\"status\":\"", entry->status, sizeof(entry->status)) ||
        !logger_json_extract_uint32(json, "\"attempt_count\":", &attempt_count) ||
        !logger_json_extract_string_or_null(json, "last_attempt_utc", entry->last_attempt_utc, sizeof(entry->last_attempt_utc)) ||
        !logger_json_extract_string_or_null(json, "last_failure_class", entry->last_failure_class, sizeof(entry->last_failure_class)) ||
        !logger_json_extract_string_or_null(json, "verified_upload_utc", entry->verified_upload_utc, sizeof(entry->verified_upload_utc)) ||
        !logger_json_extract_string_or_null(json, "receipt_id", entry->receipt_id, sizeof(entry->receipt_id))) {
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

static bool logger_file_write_all(FIL *file, const void *data, size_t len) {
    UINT written = 0u;
    return f_write(file, data, len, &written) == FR_OK && written == len;
}

static bool logger_file_write_cstr(FIL *file, const char *text) {
    return logger_file_write_all(file, text, strlen(text));
}

static bool logger_file_write_json_string_or_null(FIL *file, const char *value) {
    if (!logger_string_present(value)) {
        return logger_file_write_cstr(file, "null");
    }
    char escaped[512];
    logger_json_escape_into(escaped, sizeof(escaped), value);
    char quoted[544];
    const int n = snprintf(quoted, sizeof(quoted), "\"%s\"", escaped);
    return n > 0 && (size_t)n < sizeof(quoted) && logger_file_write_all(file, quoted, (size_t)n);
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

    if (strstr(queue_json, "\"schema_version\":1") == NULL) {
        return false;
    }
    if (!logger_json_extract_string_or_null(queue_json, "updated_at_utc", queue->updated_at_utc, sizeof(queue->updated_at_utc))) {
        return false;
    }

    const char *sessions_key = strstr(queue_json, "\"sessions\":[");
    if (sessions_key == NULL) {
        return false;
    }
    const char *array_start = strchr(sessions_key, '[');
    if (array_start == NULL) {
        return false;
    }
    const char *array_end = logger_json_find_matching(array_start, '[', ']');
    if (array_end == NULL) {
        return false;
    }

    const char *cursor = array_start + 1;
    while (cursor < array_end) {
        while (cursor < array_end && (*cursor == ' ' || *cursor == '\t' || *cursor == '\r' || *cursor == '\n' || *cursor == ',')) {
            ++cursor;
        }
        if (cursor >= array_end) {
            break;
        }
        if (*cursor != '{') {
            return false;
        }
        const char *object_end = logger_json_find_matching(cursor, '{', '}');
        if (object_end == NULL || object_end > array_end) {
            return false;
        }

        const size_t object_len = (size_t)(object_end - cursor + 1);
        if (object_len >= LOGGER_QUEUE_ENTRY_JSON_MAX || queue->session_count >= LOGGER_UPLOAD_QUEUE_MAX_SESSIONS) {
            return false;
        }

        char object_json[LOGGER_QUEUE_ENTRY_JSON_MAX + 1u];
        memcpy(object_json, cursor, object_len);
        object_json[object_len] = '\0';

        logger_upload_queue_entry_t entry;
        if (!logger_parse_queue_entry_json(object_json, &entry) ||
            logger_upload_queue_find_by_session_id(queue, entry.session_id) != NULL) {
            return false;
        }

        queue->sessions[queue->session_count++] = entry;
        cursor = object_end + 1;
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

        if (!logger_compute_canonical_bundle(
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
                    logger_queue_copy_mutable_fields(&entry, &previous.sessions[j]);
                    if (strcmp(entry.status, "uploading") == 0) {
                        logger_copy_string(entry.status, sizeof(entry.status), "failed");
                        logger_copy_string(entry.last_failure_class, sizeof(entry.last_failure_class), "interrupted");
                        logger_log_upload_interrupted(system_log, updated_at_utc_or_null, &entry);
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