#ifndef LOGGER_FIRMWARE_STORAGE_SERVICE_H
#define LOGGER_FIRMWARE_STORAGE_SERVICE_H

/*
 * Storage service — core-0 wrappers that route SD/FatFS operations
 * through the core-1 storage worker.
 *
 * After BOOT, core 0 MUST NOT call queue.c, upload_bundle.c, or
 * storage.c mutating functions directly.  Instead, core 0 calls the
 * storage_service wrappers below.  The underlying mailbox is asynchronous and
 * single-flight:
 *
 *   1. fills in a shared request struct,
 *   2. enqueues a LOGGER_WRITER_SERVICE_REQUEST command,
 *   3. core 1 executes and publishes a monotonic completion sequence,
 *   4. the synchronous convenience wrappers wait and copy typed results.
 *
 * The actual SD/FatFS work happens on core 1 using the same
 * queue.c / upload_bundle.c / storage.c functions as before.
 *
 * BOOT-only exceptions (before the worker is launched) continue
 * to call the underlying functions directly.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "logger/queue.h"
#include "logger/storage.h"
#include "logger/storage_worker.h"
#include "logger/system_log.h"

/* ── Queue operations ──────────────────────────────────────────── */

bool logger_storage_svc_queue_load(logger_upload_queue_t *queue);

bool logger_storage_svc_queue_scan(logger_upload_queue_t *queue,
                                   logger_system_log_t *system_log,
                                   const char *updated_at_utc_or_null);

bool logger_storage_svc_queue_write(const logger_upload_queue_t *queue);

bool logger_storage_svc_queue_refresh(
    logger_system_log_t *system_log, const char *updated_at_utc_or_null,
    logger_upload_queue_summary_t *summary_out);

bool logger_storage_svc_queue_prune(logger_system_log_t *system_log,
                                    const char *updated_at_utc_or_null,
                                    uint64_t reserve_bytes,
                                    size_t *retention_pruned_count_out,
                                    size_t *reserve_pruned_count_out,
                                    bool *reserve_met_out,
                                    logger_upload_queue_summary_t *summary_out);

bool logger_storage_svc_queue_rebuild(
    logger_system_log_t *system_log, const char *updated_at_utc_or_null,
    logger_upload_queue_summary_t *summary_out);

bool logger_storage_svc_queue_requeue_blocked(
    logger_system_log_t *system_log, const char *updated_at_utc_or_null,
    const char *reason, size_t *requeued_count_out,
    logger_upload_queue_summary_t *summary_out);

/* ── Storage operations ────────────────────────────────────────── */

bool logger_storage_svc_refresh(logger_storage_status_t *status);

bool logger_storage_svc_self_test(void);

bool logger_storage_svc_format(logger_storage_status_t *status);

bool logger_storage_svc_file_exists(const char *path);

/* ── Bundle operations ─────────────────────────────────────────── */

bool logger_storage_svc_bundle_compute(
    const char *dir_name, const char *manifest_path, const char *journal_path,
    char out_sha256[LOGGER_UPLOAD_QUEUE_SHA256_HEX_LEN + 1],
    uint64_t *bundle_size_out);

bool logger_storage_svc_bundle_open(const char *dir_name,
                                    const char *manifest_path,
                                    const char *journal_path);

bool logger_storage_svc_bundle_read(void *dst, size_t cap, size_t *len_out);

void logger_storage_svc_bundle_close(void);

/* ── Configuration ─────────────────────────────────────────────── */

/*
 * Wire the storage service to its shared context.
 * Must be called once during BOOT, after the worker is launched.
 */
void logger_storage_svc_init(storage_worker_shared_t *shared);

#endif /* LOGGER_FIRMWARE_STORAGE_SERVICE_H */
