#include "logger/storage_service.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "hardware/sync.h"
#include "hardware/watchdog.h"
#include "pico/stdlib.h"

#include "logger/capture_pipe.h"
#include "logger/queue.h"
#include "logger/storage.h"
#include "logger/storage_worker.h"
#include "logger/upload_bundle.h"
#include "logger/util.h"
#include "logger/writer_protocol.h"

/* ── Shared context pointer ────────────────────────────────────── */

static storage_worker_shared_t *g_svc_shared;

void logger_storage_svc_init(storage_worker_shared_t *shared) {
  g_svc_shared = shared;
}

static bool logger_storage_svc_available(void) {
  return g_svc_shared != NULL && g_svc_shared->stats.ready;
}

/* ── Submit a service request and wait for completion ────────────
 *
 * CONTRACT — read this before modifying anything below:
 *
 *   Every wrapper below passes pointers to caller-owned stack or
 *   static storage (queue structs, summary buffers, SHA-256 arrays,
 *   etc.) into svc->*_out fields.  Core 1 dereferences these pointers
 *   while executing the requested operation.
 *
 *   This is safe ONLY because submit() is synchronous: core 0 blocks
 *   in the while-loop below until core 1 sets svc->done = true.
 *   The caller's stack frame is guaranteed to outlive core 1's
 *   access because the caller hasn't returned yet.
 *
 *   DO NOT make submit() non-blocking, async, or early-return.
 *   DO NOT queue multiple service requests concurrently.
 *   Every pointer passed through svc becomes a dangling pointer
 *   the instant the calling wrapper returns.
 */

#define STORAGE_SVC_TIMEOUT_MS 30000u
#define STORAGE_SVC_FORMAT_TIMEOUT_MS 120000u
#define STORAGE_SVC_SLOW_QUEUE_TIMEOUT_MS (5u * 60u * 1000u)
#define STORAGE_SVC_BUNDLE_COMPUTE_TIMEOUT_MS (5u * 60u * 1000u)
#define STORAGE_SVC_WAIT_HEARTBEAT_MS 100u

static uint32_t
logger_storage_svc_timeout_ms(storage_service_kind_t kind) {
  switch (kind) {
  case STORAGE_SVC_QUEUE_SCAN:
  case STORAGE_SVC_QUEUE_REFRESH:
  case STORAGE_SVC_QUEUE_REBUILD:
    return STORAGE_SVC_SLOW_QUEUE_TIMEOUT_MS;
  case STORAGE_SVC_BUNDLE_COMPUTE:
    return STORAGE_SVC_BUNDLE_COMPUTE_TIMEOUT_MS;
  case STORAGE_SVC_STORAGE_FORMAT:
    return STORAGE_SVC_FORMAT_TIMEOUT_MS;
  default:
    return STORAGE_SVC_TIMEOUT_MS;
  }
}

static void logger_storage_svc_wait_heartbeat(void) {
  /* Core 0 wait strategy:
   *
   * - sleep in WFE so long storage operations do not burn a full core
   * - wake promptly when core 1 completes and signals __sev()
   * - also wake periodically via the core-0 default alarm pool so we can
   *   feed the watchdog and notice a timeout even if core 1 wedges or an
   *   expected completion event is missed
   *
   * We intentionally keep the alarm-pool dependency out of the core-1 worker
   * (see storage_worker.c), but on core 0 the default alarm pool is already
   * part of normal system operation and is the cleanest low-CPU heartbeat.
   */
  (void)best_effort_wfe_or_timeout(
      make_timeout_time_ms(STORAGE_SVC_WAIT_HEARTBEAT_MS));
}

static void logger_storage_svc_prepare(storage_service_t *svc) {
  memset(&svc->params, 0, sizeof(svc->params));
  svc->queue_out = NULL;
  svc->queue_in = NULL;
  svc->summary_out = NULL;
  svc->storage_status_out = NULL;
  svc->retention_pruned_out = NULL;
  svc->reserve_pruned_out = NULL;
  svc->reserve_met_out = NULL;
  svc->requeued_count_out = NULL;
  svc->sha256_out = NULL;
  svc->bundle_size_out = NULL;
  svc->system_log = NULL;
  svc->format_status_out = NULL;
  svc->file_exists_out = NULL;
}

static bool logger_storage_svc_submit(storage_service_kind_t kind) {
  storage_service_t *svc = &g_svc_shared->service;

  /* Debug guard: catch re-entrancy or double-submit.  If done is
   * false and kind is not NONE, a previous request is still in
   * flight — a second submit would overwrite the pointer fields
   * while core 1 is still using them. */
  assert(svc->done || svc->kind == STORAGE_SVC_NONE);

  svc->done = false;
  svc->ok = false;

  __mem_fence_release();
  svc->kind = kind;
  __mem_fence_release();

  /* Enqueue a service-request command to wake core 1 */
  logger_writer_cmd_t cmd;
  memset(&cmd, 0, sizeof(cmd));
  cmd.type = LOGGER_WRITER_SERVICE_REQUEST;

  if (!capture_cmd_ring_enqueue(g_svc_shared->pipe, &cmd)) {
    printf("[storage_svc] enqueue failed for kind=%d\n", (int)kind);
    svc->kind = STORAGE_SVC_NONE;
    return false;
  }
  __sev();

  /* Wait for core 1 to complete.
   *
   * Even if the deadline fires we must not return until core 1
   * finishes writing through the output pointers in svc — otherwise
   * a new request could overwrite them while core 1 is still active.
   * The timeout just means we report failure; the wait is mandatory.
   */
  const uint32_t deadline =
      (uint32_t)(time_us_64() / 1000ull) + logger_storage_svc_timeout_ms(kind);
  bool timed_out = false;
  while (!svc->done) {
    if (!timed_out && (uint32_t)(time_us_64() / 1000ull) >= deadline) {
      printf("[storage_svc] timeout waiting for kind=%d\n", (int)kind);
      timed_out = true;
      /* Fall through — keep waiting, don't return. */
    }
    watchdog_update();
    logger_storage_svc_wait_heartbeat();
  }
  __mem_fence_acquire();

  const bool ok = !timed_out && svc->ok;
  svc->kind = STORAGE_SVC_NONE;
  return ok;
}

/* ── Queue wrappers ────────────────────────────────────────────── */

bool logger_storage_svc_queue_load(logger_upload_queue_t *queue) {
  if (!logger_storage_svc_available()) {
    return logger_upload_queue_load(queue);
  }
  storage_service_t *svc = &g_svc_shared->service;
  logger_storage_svc_prepare(svc);
  svc->queue_out = queue;
  return logger_storage_svc_submit(STORAGE_SVC_QUEUE_LOAD);
}

bool logger_storage_svc_queue_scan(logger_upload_queue_t *queue,
                                   logger_system_log_t *system_log,
                                   const char *updated_at_utc_or_null) {
  if (!logger_storage_svc_available()) {
    return logger_upload_queue_scan(queue, system_log, updated_at_utc_or_null);
  }
  storage_service_t *svc = &g_svc_shared->service;
  logger_storage_svc_prepare(svc);
  svc->queue_out = queue;
  svc->system_log = system_log;
  logger_copy_string(svc->params.utc_only.updated_at_utc,
                     sizeof(svc->params.utc_only.updated_at_utc),
                     updated_at_utc_or_null != NULL ? updated_at_utc_or_null
                                                    : "");
  return logger_storage_svc_submit(STORAGE_SVC_QUEUE_SCAN);
}

bool logger_storage_svc_queue_write(const logger_upload_queue_t *queue) {
  if (!logger_storage_svc_available()) {
    return logger_upload_queue_write(queue);
  }
  storage_service_t *svc = &g_svc_shared->service;
  logger_storage_svc_prepare(svc);
  svc->queue_in = queue;
  return logger_storage_svc_submit(STORAGE_SVC_QUEUE_WRITE);
}

bool logger_storage_svc_queue_refresh(
    logger_system_log_t *system_log, const char *updated_at_utc_or_null,
    logger_upload_queue_summary_t *summary_out) {
  if (!logger_storage_svc_available()) {
    return logger_upload_queue_refresh_file(system_log, updated_at_utc_or_null,
                                            summary_out);
  }
  storage_service_t *svc = &g_svc_shared->service;
  logger_storage_svc_prepare(svc);
  svc->summary_out = summary_out;
  svc->system_log = system_log;
  logger_copy_string(svc->params.utc_only.updated_at_utc,
                     sizeof(svc->params.utc_only.updated_at_utc),
                     updated_at_utc_or_null != NULL ? updated_at_utc_or_null
                                                    : "");
  return logger_storage_svc_submit(STORAGE_SVC_QUEUE_REFRESH);
}

bool logger_storage_svc_queue_prune(
    logger_system_log_t *system_log, const char *updated_at_utc_or_null,
    uint64_t reserve_bytes, size_t *retention_pruned_count_out,
    size_t *reserve_pruned_count_out, bool *reserve_met_out,
    logger_upload_queue_summary_t *summary_out) {
  if (!logger_storage_svc_available()) {
    return logger_upload_queue_prune_file(
        system_log, updated_at_utc_or_null, reserve_bytes,
        retention_pruned_count_out, reserve_pruned_count_out, reserve_met_out,
        summary_out);
  }
  storage_service_t *svc = &g_svc_shared->service;
  logger_storage_svc_prepare(svc);
  svc->summary_out = summary_out;
  svc->retention_pruned_out = retention_pruned_count_out;
  svc->reserve_pruned_out = reserve_pruned_count_out;
  svc->reserve_met_out = reserve_met_out;
  svc->system_log = system_log;
  logger_copy_string(svc->params.prune.updated_at_utc,
                     sizeof(svc->params.prune.updated_at_utc),
                     updated_at_utc_or_null != NULL ? updated_at_utc_or_null
                                                    : "");
  svc->params.prune.reserve_bytes = reserve_bytes;
  return logger_storage_svc_submit(STORAGE_SVC_QUEUE_PRUNE);
}

bool logger_storage_svc_queue_rebuild(
    logger_system_log_t *system_log, const char *updated_at_utc_or_null,
    logger_upload_queue_summary_t *summary_out) {
  if (!logger_storage_svc_available()) {
    return logger_upload_queue_rebuild_file(system_log, updated_at_utc_or_null,
                                            summary_out);
  }
  storage_service_t *svc = &g_svc_shared->service;
  logger_storage_svc_prepare(svc);
  svc->summary_out = summary_out;
  svc->system_log = system_log;
  logger_copy_string(svc->params.utc_only.updated_at_utc,
                     sizeof(svc->params.utc_only.updated_at_utc),
                     updated_at_utc_or_null != NULL ? updated_at_utc_or_null
                                                    : "");
  return logger_storage_svc_submit(STORAGE_SVC_QUEUE_REBUILD);
}

bool logger_storage_svc_queue_requeue_blocked(
    logger_system_log_t *system_log, const char *updated_at_utc_or_null,
    const char *reason, size_t *requeued_count_out,
    logger_upload_queue_summary_t *summary_out) {
  if (!logger_storage_svc_available()) {
    return logger_upload_queue_requeue_blocked_file(
        system_log, updated_at_utc_or_null, reason, requeued_count_out,
        summary_out);
  }
  storage_service_t *svc = &g_svc_shared->service;
  logger_storage_svc_prepare(svc);
  svc->summary_out = summary_out;
  svc->requeued_count_out = requeued_count_out;
  svc->system_log = system_log;
  logger_copy_string(svc->params.requeue.updated_at_utc,
                     sizeof(svc->params.requeue.updated_at_utc),
                     updated_at_utc_or_null != NULL ? updated_at_utc_or_null
                                                    : "");
  logger_copy_string(svc->params.requeue.reason,
                     sizeof(svc->params.requeue.reason),
                     reason != NULL ? reason : "");
  return logger_storage_svc_submit(STORAGE_SVC_QUEUE_REQUEUE_BLOCKED);
}

/* ── Storage wrappers ──────────────────────────────────────────── */

bool logger_storage_svc_refresh(logger_storage_status_t *status) {
  if (!logger_storage_svc_available()) {
    return logger_storage_refresh(status);
  }
  storage_service_t *svc = &g_svc_shared->service;
  logger_storage_svc_prepare(svc);
  svc->storage_status_out = status;
  return logger_storage_svc_submit(STORAGE_SVC_STORAGE_REFRESH);
}

bool logger_storage_svc_self_test(void) {
  if (!logger_storage_svc_available()) {
    static const char probe_data[] = "ok\n";
    return logger_storage_write_file_atomic(LOGGER_RECOVERY_PROBE_PATH,
                                            probe_data,
                                            sizeof(probe_data) - 1u) &&
           logger_storage_remove_file(LOGGER_RECOVERY_PROBE_PATH);
  }
  logger_storage_svc_prepare(&g_svc_shared->service);
  return logger_storage_svc_submit(STORAGE_SVC_STORAGE_SELF_TEST);
}

bool logger_storage_svc_format(logger_storage_status_t *status) {
  if (!logger_storage_svc_available()) {
    return logger_storage_format(status);
  }
  storage_service_t *svc = &g_svc_shared->service;
  logger_storage_svc_prepare(svc);
  svc->format_status_out = status;
  return logger_storage_svc_submit(STORAGE_SVC_STORAGE_FORMAT);
}

bool logger_storage_svc_file_exists(const char *path) {
  if (!logger_storage_svc_available()) {
    return logger_storage_file_exists(path);
  }
  storage_service_t *svc = &g_svc_shared->service;
  logger_storage_svc_prepare(svc);
  logger_copy_string(svc->params.file_exists.path,
                     sizeof(svc->params.file_exists.path), path);
  bool exists = false;
  svc->file_exists_out = &exists;
  if (!logger_storage_svc_submit(STORAGE_SVC_FILE_EXISTS)) {
    return false;
  }
  return exists;
}

/* ── Bundle wrappers ───────────────────────────────────────────── */

bool logger_storage_svc_bundle_compute(
    const char *dir_name, const char *manifest_path, const char *journal_path,
    char out_sha256[LOGGER_UPLOAD_QUEUE_SHA256_HEX_LEN + 1],
    uint64_t *bundle_size_out) {
  if (!logger_storage_svc_available()) {
    return logger_upload_bundle_compute(dir_name, manifest_path, journal_path,
                                        out_sha256, bundle_size_out);
  }
  storage_service_t *svc = &g_svc_shared->service;
  logger_storage_svc_prepare(svc);
  logger_copy_string(svc->params.bundle.dir_name,
                     sizeof(svc->params.bundle.dir_name), dir_name);
  logger_copy_string(svc->params.bundle.manifest_path,
                     sizeof(svc->params.bundle.manifest_path), manifest_path);
  logger_copy_string(svc->params.bundle.journal_path,
                     sizeof(svc->params.bundle.journal_path), journal_path);
  svc->sha256_out = out_sha256;
  svc->bundle_size_out = bundle_size_out;
  return logger_storage_svc_submit(STORAGE_SVC_BUNDLE_COMPUTE);
}

bool logger_storage_svc_bundle_open(const char *dir_name,
                                    const char *manifest_path,
                                    const char *journal_path) {
  if (g_svc_shared == NULL) {
    return false;
  }
  if (!logger_storage_svc_available()) {
    return logger_upload_bundle_stream_open(
        &g_svc_shared->bundle_stream, dir_name, manifest_path, journal_path);
  }
  storage_service_t *svc = &g_svc_shared->service;
  logger_storage_svc_prepare(svc);
  logger_copy_string(svc->params.bundle.dir_name,
                     sizeof(svc->params.bundle.dir_name), dir_name);
  logger_copy_string(svc->params.bundle.manifest_path,
                     sizeof(svc->params.bundle.manifest_path), manifest_path);
  logger_copy_string(svc->params.bundle.journal_path,
                     sizeof(svc->params.bundle.journal_path), journal_path);
  return logger_storage_svc_submit(STORAGE_SVC_BUNDLE_OPEN);
}

bool logger_storage_svc_bundle_read(void *dst, size_t cap, size_t *len_out) {
  if (g_svc_shared == NULL) {
    if (len_out != NULL)
      *len_out = 0u;
    return false;
  }
  if (!logger_storage_svc_available()) {
    return logger_upload_bundle_stream_read(&g_svc_shared->bundle_stream, dst,
                                            cap, len_out);
  }
  storage_service_t *svc = &g_svc_shared->service;
  logger_storage_svc_prepare(svc);
  svc->params.bundle_read.dst = dst;
  svc->params.bundle_read.cap = cap;
  svc->params.bundle_read.len_out = len_out;
  return logger_storage_svc_submit(STORAGE_SVC_BUNDLE_READ);
}

void logger_storage_svc_bundle_close(void) {
  if (g_svc_shared == NULL) {
    return;
  }
  if (!logger_storage_svc_available()) {
    logger_upload_bundle_stream_close(&g_svc_shared->bundle_stream);
    return;
  }
  logger_storage_svc_prepare(&g_svc_shared->service);
  (void)logger_storage_svc_submit(STORAGE_SVC_BUNDLE_CLOSE);
}
