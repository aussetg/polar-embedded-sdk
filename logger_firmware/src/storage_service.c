#include "logger/storage_service.h"

#include <stdio.h>
#include <string.h>

#include "hardware/sync.h"
#include "hardware/watchdog.h"
#include "pico/stdlib.h"

#include "logger/capture_pipe.h"
#include "logger/queue.h"
#include "logger/reset_marker.h"
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

static storage_service_state_t
logger_storage_svc_state_load_acquire(const storage_service_t *svc) {
  return (storage_service_state_t)logger_ipc_u32_load_acquire(&svc->state);
}

static void
logger_storage_svc_state_store_release(storage_service_t *svc,
                                       storage_service_state_t state) {
  logger_ipc_u32_store_release(&svc->state, (uint32_t)state);
}

static void logger_storage_svc_kind_store_relaxed(storage_service_t *svc,
                                                  storage_service_kind_t kind) {
  logger_ipc_u32_store_relaxed(&svc->kind, (uint32_t)kind);
}

static bool logger_storage_svc_available(void) {
  return g_svc_shared != NULL &&
         logger_ipc_bool_load_acquire(&g_svc_shared->storage_service_ready);
}

/* ── Submit service requests ─────────────────────────────────────
 *
 * CONTRACT — read this before modifying anything below:
 *
 *   Requests and responses live in the shared service mailbox.  Core 1 never
 *   writes through caller-owned output buffers; wrappers copy response fields
 *   back to caller storage after DONE is observed. system_log is app-lifetime
 *   shared state, not a per-request result buffer.
 *
 *   The mailbox is asynchronous and single-flight: submit_async() publishes a
 *   request and returns once it is queued; wait() consumes the completion.
 *   The public convenience wrappers below still submit+wait because their API
 *   returns typed results synchronously. Do not queue multiple service
 *   requests concurrently: the shared mailbox has one request slot and one
 *   response slot.
 */

#define STORAGE_SVC_TIMEOUT_MS 30000u
#define STORAGE_SVC_FORMAT_TIMEOUT_MS 120000u
#define STORAGE_SVC_SLOW_QUEUE_TIMEOUT_MS (5u * 60u * 1000u)
#define STORAGE_SVC_BUNDLE_COMPUTE_TIMEOUT_MS (5u * 60u * 1000u)
#define STORAGE_SVC_WAIT_HEARTBEAT_MS 100u

static uint32_t logger_storage_svc_timeout_ms(storage_service_kind_t kind) {
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
  hard_assert(logger_storage_svc_state_load_acquire(svc) ==
              STORAGE_SVC_STATE_IDLE);
  memset(&svc->params, 0, sizeof(svc->params));
  svc->system_log = NULL;
  memset(&svc->response, 0, sizeof(svc->response));
}

static bool logger_storage_svc_submit_async(storage_service_kind_t kind,
                                            uint32_t *request_seq_out) {
  storage_service_t *svc = &g_svc_shared->service;

  /* Debug guard: catch re-entrancy or double-submit. */
  hard_assert(logger_storage_svc_state_load_acquire(svc) ==
              STORAGE_SVC_STATE_IDLE);

  logger_ipc_bool_store_relaxed(&svc->ok, false);
  const uint32_t request_seq =
      logger_ipc_u32_load_relaxed(&svc->request_seq) + 1u;
  logger_ipc_u32_store_relaxed(&svc->request_seq, request_seq);
  if (request_seq_out != NULL) {
    *request_seq_out = request_seq;
  }

  logger_storage_svc_kind_store_relaxed(svc, kind);
  logger_storage_svc_state_store_release(svc, STORAGE_SVC_STATE_SUBMITTED);

  /* Enqueue a service-request command to wake core 1 */
  logger_writer_cmd_t cmd;
  memset(&cmd, 0, sizeof(cmd));
  cmd.type = LOGGER_WRITER_SERVICE_REQUEST;

  /* Preserve the same ordering contract as synchronous writer commands:
   * older PMD packets sitting in core-0 staging must reach the command ring
   * before the service request is published to core 1. */
  if (capture_staging_has_data(g_svc_shared->pipe)) {
    (void)capture_staging_drain(g_svc_shared->pipe,
                                g_svc_shared->pipe->staging.capacity);
  }

  if (!capture_cmd_ring_enqueue(g_svc_shared->pipe, &cmd)) {
    printf("[storage_svc] enqueue failed for kind=%d\n", (int)kind);
    logger_storage_svc_kind_store_relaxed(svc, STORAGE_SVC_NONE);
    logger_storage_svc_state_store_release(svc, STORAGE_SVC_STATE_IDLE);
    return false;
  }
  __sev();

  return true;
}

static bool logger_storage_svc_wait(storage_service_kind_t kind,
                                    uint32_t request_seq) {
  storage_service_t *svc = &g_svc_shared->service;

  /* Wait for core 1 to complete.
   *
   * Even though service responses are mailbox-owned now, a timeout means the
   * storage worker may be wedged inside FatFS/SD I/O. Reboot rather than
   * continuing with an unknown worker/storage state.
   */
  const uint32_t start_ms = (uint32_t)(time_us_64() / 1000ull);
  const uint32_t timeout_ms = logger_storage_svc_timeout_ms(kind);
  bool timed_out = false;
  storage_service_state_t state = STORAGE_SVC_STATE_IDLE;
  while (true) {
    const uint32_t done_seq = logger_ipc_u32_load_acquire(&svc->done_seq);
    state = logger_storage_svc_state_load_acquire(svc);
    if (done_seq == request_seq && (state == STORAGE_SVC_STATE_DONE ||
                                    state == STORAGE_SVC_STATE_FAILED)) {
      break;
    }
    const uint32_t now_ms = (uint32_t)(time_us_64() / 1000ull);
    if (!timed_out && (now_ms - start_ms) >= timeout_ms) {
      printf("[storage_svc] timeout waiting for kind=%d\n", (int)kind);
      timed_out = true;
      /* Treat this as a fatal worker liveness failure and reboot instead of
       * feeding the watchdog forever.  Store a one-shot POWMAN marker first:
       * watchdog_reboot() intentionally clears the SDK watchdog scratch magic,
       * and RP2350 watchdog chip resets clear watchdog scratch registers. */
      logger_reset_marker_record_storage_service_timeout((uint32_t)kind,
                                                         request_seq);
      watchdog_reboot(0, 0, 0);
      while (true) {
        tight_loop_contents();
      }
    }
    watchdog_update();
    logger_storage_svc_wait_heartbeat();
  }

  const bool ok = !timed_out && state == STORAGE_SVC_STATE_DONE &&
                  logger_ipc_bool_load_relaxed(&svc->ok);
  logger_storage_svc_kind_store_relaxed(svc, STORAGE_SVC_NONE);
  logger_storage_svc_state_store_release(svc, STORAGE_SVC_STATE_IDLE);
  return ok;
}

static bool logger_storage_svc_submit_and_wait(storage_service_kind_t kind) {
  uint32_t request_seq = 0u;
  if (!logger_storage_svc_submit_async(kind, &request_seq)) {
    return false;
  }
  return logger_storage_svc_wait(kind, request_seq);
}

/* ── Queue wrappers ────────────────────────────────────────────── */

bool logger_storage_svc_queue_load(logger_upload_queue_t *queue) {
  if (queue == NULL) {
    return false;
  }
  if (!logger_storage_svc_available()) {
    return logger_upload_queue_load(queue);
  }
  storage_service_t *svc = &g_svc_shared->service;
  logger_storage_svc_prepare(svc);
  if (!logger_storage_svc_submit_and_wait(STORAGE_SVC_QUEUE_LOAD)) {
    return false;
  }
  *queue = svc->response.queue;
  return true;
}

bool logger_storage_svc_queue_scan(logger_upload_queue_t *queue,
                                   logger_system_log_t *system_log,
                                   const char *updated_at_utc_or_null) {
  if (queue == NULL) {
    return false;
  }
  if (!logger_storage_svc_available()) {
    return logger_upload_queue_scan(queue, system_log, updated_at_utc_or_null);
  }
  storage_service_t *svc = &g_svc_shared->service;
  logger_storage_svc_prepare(svc);
  svc->system_log = system_log;
  logger_copy_string(svc->params.utc_only.updated_at_utc,
                     sizeof(svc->params.utc_only.updated_at_utc),
                     updated_at_utc_or_null != NULL ? updated_at_utc_or_null
                                                    : "");
  if (!logger_storage_svc_submit_and_wait(STORAGE_SVC_QUEUE_SCAN)) {
    return false;
  }
  *queue = svc->response.queue;
  return true;
}

bool logger_storage_svc_queue_write(const logger_upload_queue_t *queue) {
  if (queue == NULL) {
    return false;
  }
  if (!logger_storage_svc_available()) {
    return logger_upload_queue_write(queue);
  }
  storage_service_t *svc = &g_svc_shared->service;
  logger_storage_svc_prepare(svc);
  svc->response.queue = *queue;
  return logger_storage_svc_submit_and_wait(STORAGE_SVC_QUEUE_WRITE);
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
  svc->system_log = system_log;
  logger_copy_string(svc->params.utc_only.updated_at_utc,
                     sizeof(svc->params.utc_only.updated_at_utc),
                     updated_at_utc_or_null != NULL ? updated_at_utc_or_null
                                                    : "");
  if (!logger_storage_svc_submit_and_wait(STORAGE_SVC_QUEUE_REFRESH)) {
    return false;
  }
  if (summary_out != NULL) {
    *summary_out = svc->response.summary;
  }
  return true;
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
  svc->system_log = system_log;
  logger_copy_string(svc->params.prune.updated_at_utc,
                     sizeof(svc->params.prune.updated_at_utc),
                     updated_at_utc_or_null != NULL ? updated_at_utc_or_null
                                                    : "");
  svc->params.prune.reserve_bytes = reserve_bytes;
  if (!logger_storage_svc_submit_and_wait(STORAGE_SVC_QUEUE_PRUNE)) {
    return false;
  }
  if (retention_pruned_count_out != NULL) {
    *retention_pruned_count_out = svc->response.retention_pruned_count;
  }
  if (reserve_pruned_count_out != NULL) {
    *reserve_pruned_count_out = svc->response.reserve_pruned_count;
  }
  if (reserve_met_out != NULL) {
    *reserve_met_out = svc->response.reserve_met;
  }
  if (summary_out != NULL) {
    *summary_out = svc->response.summary;
  }
  return true;
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
  svc->system_log = system_log;
  logger_copy_string(svc->params.utc_only.updated_at_utc,
                     sizeof(svc->params.utc_only.updated_at_utc),
                     updated_at_utc_or_null != NULL ? updated_at_utc_or_null
                                                    : "");
  if (!logger_storage_svc_submit_and_wait(STORAGE_SVC_QUEUE_REBUILD)) {
    return false;
  }
  if (summary_out != NULL) {
    *summary_out = svc->response.summary;
  }
  return true;
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
  svc->system_log = system_log;
  logger_copy_string(svc->params.requeue.updated_at_utc,
                     sizeof(svc->params.requeue.updated_at_utc),
                     updated_at_utc_or_null != NULL ? updated_at_utc_or_null
                                                    : "");
  logger_copy_string(svc->params.requeue.reason,
                     sizeof(svc->params.requeue.reason),
                     reason != NULL ? reason : "");
  if (!logger_storage_svc_submit_and_wait(STORAGE_SVC_QUEUE_REQUEUE_BLOCKED)) {
    return false;
  }
  if (requeued_count_out != NULL) {
    *requeued_count_out = svc->response.requeued_count;
  }
  if (summary_out != NULL) {
    *summary_out = svc->response.summary;
  }
  return true;
}

/* ── Storage wrappers ──────────────────────────────────────────── */

bool logger_storage_svc_refresh(logger_storage_status_t *status) {
  if (status == NULL) {
    return false;
  }
  if (!logger_storage_svc_available()) {
    (void)logger_storage_refresh(status);
    return true;
  }
  storage_service_t *svc = &g_svc_shared->service;
  logger_storage_svc_prepare(svc);
  if (!logger_storage_svc_submit_and_wait(STORAGE_SVC_STORAGE_REFRESH)) {
    return false;
  }
  *status = svc->response.storage_status;
  return true;
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
  return logger_storage_svc_submit_and_wait(STORAGE_SVC_STORAGE_SELF_TEST);
}

bool logger_storage_svc_format(logger_storage_status_t *status) {
  if (!logger_storage_svc_available()) {
    return logger_storage_format(status);
  }
  storage_service_t *svc = &g_svc_shared->service;
  logger_storage_svc_prepare(svc);
  if (!logger_storage_svc_submit_and_wait(STORAGE_SVC_STORAGE_FORMAT)) {
    return false;
  }
  if (status != NULL) {
    *status = svc->response.format_status;
  }
  return true;
}

bool logger_storage_svc_file_exists(const char *path) {
  if (path == NULL) {
    return false;
  }
  if (!logger_storage_svc_available()) {
    return logger_storage_file_exists(path);
  }
  storage_service_t *svc = &g_svc_shared->service;
  logger_storage_svc_prepare(svc);
  logger_copy_string(svc->params.file_exists.path,
                     sizeof(svc->params.file_exists.path), path);
  if (!logger_storage_svc_submit_and_wait(STORAGE_SVC_FILE_EXISTS)) {
    return false;
  }
  return svc->response.file_exists;
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
  if (!logger_storage_svc_submit_and_wait(STORAGE_SVC_BUNDLE_COMPUTE)) {
    return false;
  }
  if (out_sha256 != NULL) {
    logger_copy_string(out_sha256, LOGGER_UPLOAD_QUEUE_SHA256_HEX_LEN + 1,
                       svc->response.sha256);
  }
  if (bundle_size_out != NULL) {
    *bundle_size_out = svc->response.bundle_size;
  }
  return true;
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
  return logger_storage_svc_submit_and_wait(STORAGE_SVC_BUNDLE_OPEN);
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
  if (len_out != NULL) {
    *len_out = 0u;
  }
  if (dst == NULL || cap > STORAGE_SVC_BUNDLE_READ_MAX) {
    return false;
  }
  svc->params.bundle_read.cap = cap;
  if (!logger_storage_svc_submit_and_wait(STORAGE_SVC_BUNDLE_READ)) {
    return false;
  }
  if (svc->response.bundle_read_len > cap) {
    return false;
  }
  if (svc->response.bundle_read_len != 0u) {
    memcpy(dst, svc->response.bundle_read_data, svc->response.bundle_read_len);
  }
  if (len_out != NULL) {
    *len_out = svc->response.bundle_read_len;
  }
  return true;
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
  (void)logger_storage_svc_submit_and_wait(STORAGE_SVC_BUNDLE_CLOSE);
}
