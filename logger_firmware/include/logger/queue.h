#ifndef LOGGER_FIRMWARE_QUEUE_H
#define LOGGER_FIRMWARE_QUEUE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "logger/system_log.h"

#define LOGGER_UPLOAD_QUEUE_MAX_SESSIONS 64
#define LOGGER_UPLOAD_QUEUE_STATUS_MAX 24
#define LOGGER_UPLOAD_QUEUE_FAILURE_CLASS_MAX 32
#define LOGGER_UPLOAD_QUEUE_RECEIPT_ID_MAX 96
#define LOGGER_UPLOAD_QUEUE_SHA256_HEX_LEN 64
#define LOGGER_UPLOAD_QUEUE_UTC_MAX 31

typedef struct {
    char session_id[33];
    char study_day_local[11];
    char dir_name[64];
    char session_start_utc[LOGGER_UPLOAD_QUEUE_UTC_MAX + 1];
    char session_end_utc[LOGGER_UPLOAD_QUEUE_UTC_MAX + 1];
    char bundle_sha256[LOGGER_UPLOAD_QUEUE_SHA256_HEX_LEN + 1];
    uint64_t bundle_size_bytes;
    bool quarantined;
    char status[LOGGER_UPLOAD_QUEUE_STATUS_MAX + 1];
    uint32_t attempt_count;
    char last_attempt_utc[LOGGER_UPLOAD_QUEUE_UTC_MAX + 1];
    char last_failure_class[LOGGER_UPLOAD_QUEUE_FAILURE_CLASS_MAX + 1];
    char verified_upload_utc[LOGGER_UPLOAD_QUEUE_UTC_MAX + 1];
    char receipt_id[LOGGER_UPLOAD_QUEUE_RECEIPT_ID_MAX + 1];
} logger_upload_queue_entry_t;

typedef struct {
    char updated_at_utc[LOGGER_UPLOAD_QUEUE_UTC_MAX + 1];
    size_t session_count;
    logger_upload_queue_entry_t sessions[LOGGER_UPLOAD_QUEUE_MAX_SESSIONS];
} logger_upload_queue_t;

typedef struct {
    bool available;
    char updated_at_utc[LOGGER_UPLOAD_QUEUE_UTC_MAX + 1];
    uint32_t session_count;
    uint32_t pending_count;
    uint32_t blocked_count;
    char oldest_pending_study_day[11];
    char last_failure_class[LOGGER_UPLOAD_QUEUE_FAILURE_CLASS_MAX + 1];
} logger_upload_queue_summary_t;

void logger_upload_queue_init(logger_upload_queue_t *queue);
void logger_upload_queue_summary_init(logger_upload_queue_summary_t *summary);
void logger_upload_queue_compute_summary(
    const logger_upload_queue_t *queue,
    logger_upload_queue_summary_t *summary);

const logger_upload_queue_entry_t *logger_upload_queue_find_by_session_id(
    const logger_upload_queue_t *queue,
    const char *session_id);

bool logger_upload_queue_load(logger_upload_queue_t *queue);
bool logger_upload_queue_scan(
    logger_upload_queue_t *queue,
    logger_system_log_t *system_log,
    const char *updated_at_utc_or_null);
bool logger_upload_queue_write(const logger_upload_queue_t *queue);
bool logger_upload_queue_rebuild_file(
    logger_system_log_t *system_log,
    const char *updated_at_utc_or_null,
    logger_upload_queue_summary_t *summary_out);
bool logger_upload_queue_requeue_blocked_file(
    logger_system_log_t *system_log,
    const char *updated_at_utc_or_null,
    size_t *requeued_count_out,
    logger_upload_queue_summary_t *summary_out);
bool logger_upload_queue_refresh_file(
    logger_system_log_t *system_log,
    const char *updated_at_utc_or_null,
    logger_upload_queue_summary_t *summary_out);

#endif