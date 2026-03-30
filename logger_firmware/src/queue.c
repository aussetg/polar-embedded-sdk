#include "logger/queue.h"

#include <stdio.h>
#include <string.h>

#include "ff.h"

#include "logger/sha256.h"
#include "logger/storage.h"

#define LOGGER_QUEUE_PATH "0:/logger/state/upload_queue.json"
#define LOGGER_QUEUE_TMP_PATH "0:/logger/state/upload_queue.json.tmp"
#define LOGGER_SESSIONS_DIR "0:/logger/sessions"
#define LOGGER_MANIFEST_READ_MAX 8192u

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

    for (size_t i = 0u; i < queue->session_count; ++i) {
        const logger_upload_queue_entry_t *entry = &queue->sessions[i];
        if (strcmp(entry->status, "blocked_min_firmware") == 0) {
            summary->blocked_count += 1u;
        }
        if (strcmp(entry->status, "pending") == 0 || strcmp(entry->status, "failed") == 0) {
            if (summary->pending_count == 0u) {
                logger_copy_string(summary->oldest_pending_study_day,
                                   sizeof(summary->oldest_pending_study_day),
                                   entry->study_day_local);
            }
            summary->pending_count += 1u;
        }
        if (logger_string_present(entry->last_failure_class)) {
            logger_copy_string(summary->last_failure_class,
                               sizeof(summary->last_failure_class),
                               entry->last_failure_class);
        }
    }
}

bool logger_upload_queue_scan(
    logger_upload_queue_t *queue,
    logger_system_log_t *system_log,
    const char *updated_at_utc_or_null) {
    logger_upload_queue_init(queue);

    logger_storage_status_t storage;
    (void)logger_storage_refresh(&storage);
    if (!storage.mounted || !storage.writable) {
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
    UINT written = 0u;
    const char *prefix = "{\"schema_version\":1,\"updated_at_utc\":";
    ok = ok && f_write(&file, prefix, strlen(prefix), &written) == FR_OK && written == strlen(prefix);
    if (ok) {
        char escaped[96];
        if (logger_string_present(queue->updated_at_utc)) {
            logger_json_escape_into(escaped, sizeof(escaped), queue->updated_at_utc);
            char buf[128];
            const int n = snprintf(buf, sizeof(buf), "\"%s\"", escaped);
            ok = ok && n > 0 && (size_t)n < sizeof(buf) &&
                f_write(&file, buf, (UINT)n, &written) == FR_OK && written == (UINT)n;
        } else {
            ok = ok && f_write(&file, "null", 4u, &written) == FR_OK && written == 4u;
        }
    }
    static const char sessions_prefix[] = ",\"sessions\":[";
    ok = ok && f_write(&file, sessions_prefix, sizeof(sessions_prefix) - 1u, &written) == FR_OK && written == (sizeof(sessions_prefix) - 1u);

    for (size_t i = 0u; ok && i < queue->session_count; ++i) {
        const logger_upload_queue_entry_t *entry = &queue->sessions[i];
        char s_session_id[80], s_study_day[48], s_dir_name[128], s_start[96], s_end[96], s_sha[96], s_status[64];
        char s_last_attempt[96], s_last_failure[96], s_verified[96], s_receipt[196];
        logger_json_escape_into(s_session_id, sizeof(s_session_id), entry->session_id);
        logger_json_escape_into(s_study_day, sizeof(s_study_day), entry->study_day_local);
        logger_json_escape_into(s_dir_name, sizeof(s_dir_name), entry->dir_name);
        logger_json_escape_into(s_start, sizeof(s_start), entry->session_start_utc);
        logger_json_escape_into(s_end, sizeof(s_end), entry->session_end_utc);
        logger_json_escape_into(s_sha, sizeof(s_sha), entry->bundle_sha256);
        logger_json_escape_into(s_status, sizeof(s_status), entry->status);
        logger_json_escape_into(s_last_attempt, sizeof(s_last_attempt), entry->last_attempt_utc);
        logger_json_escape_into(s_last_failure, sizeof(s_last_failure), entry->last_failure_class);
        logger_json_escape_into(s_verified, sizeof(s_verified), entry->verified_upload_utc);
        logger_json_escape_into(s_receipt, sizeof(s_receipt), entry->receipt_id);

        char entry_buf[1024];
        const int n = snprintf(
            entry_buf,
            sizeof(entry_buf),
            "%s{\"session_id\":\"%s\",\"study_day_local\":\"%s\",\"dir_name\":\"%s\",\"session_start_utc\":\"%s\",\"session_end_utc\":\"%s\",\"bundle_sha256\":\"%s\",\"bundle_size_bytes\":%llu,\"quarantined\":%s,\"status\":\"%s\",\"attempt_count\":%lu,\"last_attempt_utc\":null,\"last_failure_class\":null,\"verified_upload_utc\":null,\"receipt_id\":null}",
            i == 0u ? "" : ",",
            s_session_id,
            s_study_day,
            s_dir_name,
            s_start,
            s_end,
            s_sha,
            (unsigned long long)entry->bundle_size_bytes,
            entry->quarantined ? "true" : "false",
            s_status,
            (unsigned long)entry->attempt_count);
        ok = ok && n > 0 && (size_t)n < sizeof(entry_buf) &&
            f_write(&file, entry_buf, (UINT)n, &written) == FR_OK && written == (UINT)n;
    }

    ok = ok && f_write(&file, "]}", 2u, &written) == FR_OK && written == 2u;
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
    logger_upload_queue_t queue;
    const bool scanned = logger_upload_queue_scan(&queue, system_log, updated_at_utc_or_null);
    if (summary_out != NULL) {
        if (scanned) {
            logger_upload_queue_compute_summary(&queue, summary_out);
        } else {
            logger_upload_queue_summary_init(summary_out);
        }
    }
    if (!scanned) {
        return false;
    }
    return logger_upload_queue_write(&queue);
}