#ifndef LOGGER_FIRMWARE_STORAGE_WORKER_H
#define LOGGER_FIRMWARE_STORAGE_WORKER_H

/*
 * Core-1 storage worker — permanent background loop that owns
 * SD/FatFS and drains the capture-pipe command ring.
 *
 * Lifecycle (from logger_runtime_architecture_v1.md §2.2, §4.4):
 *   - launched once during BOOT via multicore_launch_core1()
 *   - stays alive until reboot
 *   - may sleep idle but is not torn down/relaunched during normal
 *     mode transitions
 *
 * Flash safety (from logger_capture_pipeline_v1.md §10):
 *   - both runtime cores call flash_safe_execute_core_init()
 *   - worker launch is not complete until both cores are lockout-ready
 *   - multicore FIFO remains reserved for SDK lockout use
 */

#include <stdbool.h>
#include <stdint.h>

#include "logger/capture_pipe.h"
#include "logger/queue.h"
#include "logger/storage.h"
#include "logger/upload_bundle.h"

/* ── Storage service request types ─────────────────────────────── */

typedef enum {
  STORAGE_SVC_NONE = 0,
  STORAGE_SVC_STORAGE_REFRESH,
  STORAGE_SVC_STORAGE_SELF_TEST,
  STORAGE_SVC_QUEUE_LOAD,
  STORAGE_SVC_QUEUE_SCAN,
  STORAGE_SVC_QUEUE_WRITE,
  STORAGE_SVC_QUEUE_REFRESH,
  STORAGE_SVC_QUEUE_PRUNE,
  STORAGE_SVC_QUEUE_REBUILD,
  STORAGE_SVC_QUEUE_REQUEUE_BLOCKED,
  STORAGE_SVC_BUNDLE_COMPUTE,
  STORAGE_SVC_BUNDLE_OPEN,
  STORAGE_SVC_BUNDLE_READ,
  STORAGE_SVC_BUNDLE_CLOSE,
  STORAGE_SVC_STORAGE_FORMAT,
  STORAGE_SVC_FILE_EXISTS,
} storage_service_kind_t;

/*
 * Shared service request/response struct.
 *
 * Core 0 fills in .kind and .params, then enqueues a
 * LOGGER_WRITER_SERVICE_REQUEST command.  Core 1 reads the request,
 * executes it, fills in .result, sets .done = true.
 *
 * The queue_out pointer lets core 1 write directly into a caller-
 * supplied queue struct on core 0's stack (safe because core 0 is
 * blocked waiting for .done).  Same pattern for storage_status_out.
 *
 * For BUNDLE_READ, core 0 provides dst/cap pointers; core 1 writes
 * the data and len_out.
 */
typedef struct {
  /*
   * No field is volatile — inter-core ordering relies entirely on
   * explicit memory fences (__mem_fence_release / __mem_fence_acquire)
   * paired with __sev() / __wfe().  This is the standard pattern for
   * shared-memory IPC on single-issue ARM cores: fences are sufficient
   * because volatile only prevents compiler reordering/reload
   * elision, which the fences already guarantee.
   */

  /* Request fields — core 0 writes before enqueue, core 1 reads */
  storage_service_kind_t kind;

  union {
    struct {
      char updated_at_utc[LOGGER_UPLOAD_QUEUE_UTC_MAX + 1];
    } utc_only; /* QUEUE_SCAN, QUEUE_REFRESH, QUEUE_REBUILD */
    struct {
      char updated_at_utc[LOGGER_UPLOAD_QUEUE_UTC_MAX + 1];
      uint64_t reserve_bytes;
    } prune;
    struct {
      char updated_at_utc[LOGGER_UPLOAD_QUEUE_UTC_MAX + 1];
      char reason[64];
    } requeue;
    struct {
      char dir_name[64];
      char manifest_path[LOGGER_STORAGE_PATH_MAX];
      char journal_path[LOGGER_STORAGE_PATH_MAX];
    } bundle;
    struct {
      void *dst;
      size_t cap;
      size_t *len_out;
    } bundle_read;
    struct {
      char path[LOGGER_STORAGE_PATH_MAX];
    } file_exists;
  } params;

  /* Pointers to caller-supplied output buffers.
   *
   * THESE ARE DANGLING POINTERS after the wrapper returns.
   * Safe only because logger_storage_svc_submit() is synchronous
   * and blocks core 0 until .done — see the CONTRACT comment in
   * storage_service.c for the full invariant.
   *
   * All fields are cleared to NULL at the start of each submit()
   * so stale pointers from a prior request can't be dereferenced.
   */
  logger_upload_queue_t *queue_out;
  const logger_upload_queue_t *queue_in; /* for QUEUE_WRITE */
  logger_upload_queue_summary_t *summary_out;
  logger_storage_status_t *storage_status_out;
  size_t *retention_pruned_out;
  size_t *reserve_pruned_out;
  bool *reserve_met_out;
  size_t *requeued_count_out;
  char *sha256_out;
  uint64_t *bundle_size_out;
  logger_system_log_t *system_log;
  logger_storage_status_t *format_status_out;
  bool *file_exists_out;

  /* Completion — core 1 sets done = true after filling results */
  bool done;
  bool ok;
} storage_service_t;

/* ── Worker state (for diagnostics) ────────────────────────────── */

/*
 * Cross-core access policy (no fences, by design):
 *
 *   counters_*  — written by core 1 only, read by core 0 for
 *                 host diagnostics.  Values may be stale when
 *                 read.  Acceptable: these are approximate
 *                 telemetry, never used for correctness decisions.
 *
 *   ready       — written by core 0 once after launch completes.
 *                 Core 1 never reads it.  The flag exists so
 *                 core 0 / host tooling can confirm the worker
 *                 reached steady state.
 *
 * Do not add fields that feed back into control decisions without
 * also adding acquire/release fences at the read/write sites.
 */
typedef struct {
  uint32_t commands_processed; /* core 1 writes, core 0 reads (approximate) */
  uint32_t write_failures;     /* core 1 writes, core 0 reads (approximate) */
  uint32_t idle_iterations;    /* core 1 writes, core 0 reads (approximate) */
  uint32_t wakeups;            /* core 1 writes, core 0 reads (approximate) */
  bool ready;                  /* core 0 writes once; core 1 must not read */
} storage_worker_stats_t;

/* ── Shared state between core 0 and core 1 ────────────────────── */

typedef struct {
  /* The pipe that core 1 drains. Set by core 0 before launch. */
  capture_pipe_t *pipe;

  /* The session context that core 1 executes commands against.
   * Set by core 0 before launch. */
  logger_session_context_t *session_ctx;

  /* Handshake: core 0 waits on this before proceeding past BOOT. */
  volatile bool core1_lockout_ready;

  /* Worker statistics (core 1 writes, core 0 reads occasionally). */
  storage_worker_stats_t stats;

  /* Service request channel for non-logging SD operations. */
  storage_service_t service;

  /* Bundle stream state — owned by core 1, used only during upload. */
  logger_upload_bundle_stream_t bundle_stream;
  bool bundle_stream_open;
} storage_worker_shared_t;

/* ── API (called from core 0) ──────────────────────────────────── */

/*
 * Prepare the shared state for the storage worker.
 * Must be called on core 0 before logger_storage_worker_launch().
 */
void logger_storage_worker_init(storage_worker_shared_t *shared,
                                capture_pipe_t *pipe,
                                logger_session_context_t *session_ctx);

/*
 * Launch core 1 and wait for it to become fully ready.
 *
 * This blocks until:
 *   1. core 1 is executing the worker loop
 *   2. flash_safe_execute_core_init() has been called on core 1
 *   3. multicore_lockout_victim_init() has been called on core 1
 *   4. core1_lockout_ready is true
 *
 * Then calls flash_safe_execute_core_init() on core 0.
 *
 * Must be called exactly once, during BOOT.
 * Must not be called from an interrupt context.
 *
 * Returns true on success, false on failure (should not happen
 * with correct pico-sdk setup).
 */
bool logger_storage_worker_launch(storage_worker_shared_t *shared);

/* ── API (called from core 1, internal) ────────────────────────── */

/*
 * Core-1 entry point. Called by pico-sdk's multicore launch machinery.
 * Never returns.
 */
void logger_storage_worker_entry(void);

/*
 * The main worker loop body, separated for testability.
 * Processes commands from the ring until the pipe is empty,
 * then returns.  Callers should invoke this in a loop with
 * idle sleep between iterations.
 */
void logger_storage_worker_drain(storage_worker_shared_t *shared);

#endif /* LOGGER_FIRMWARE_STORAGE_WORKER_H */
