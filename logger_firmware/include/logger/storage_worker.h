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
#include "logger/ipc_atomic.h"
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

typedef enum {
  STORAGE_SVC_STATE_IDLE = 0,
  STORAGE_SVC_STATE_SUBMITTED,
  STORAGE_SVC_STATE_RUNNING,
  STORAGE_SVC_STATE_DONE,
  STORAGE_SVC_STATE_FAILED,
} storage_service_state_t;

#define STORAGE_SVC_BUNDLE_READ_MAX 512u

typedef struct {
  logger_upload_queue_t queue;
  logger_upload_queue_summary_t summary;
  logger_storage_status_t storage_status;
  logger_storage_status_t format_status;
  size_t retention_pruned_count;
  size_t reserve_pruned_count;
  bool reserve_met;
  size_t requeued_count;
  char sha256[LOGGER_UPLOAD_QUEUE_SHA256_HEX_LEN + 1];
  uint64_t bundle_size;
  bool file_exists;
  uint8_t bundle_read_data[STORAGE_SVC_BUNDLE_READ_MAX];
  size_t bundle_read_len;
} storage_service_response_t;

/*
 * Shared service request/response struct.
 *
 * Core 0 fills in .kind/.params/.response request payloads, publishes a
 * monotonic request_seq, marks the mailbox SUBMITTED, then enqueues a
 * LOGGER_WRITER_SERVICE_REQUEST command.  Core 1 marks the request RUNNING,
 * executes it, fills in .response, publishes done_seq, and marks it DONE.
 * Core 1 never writes through caller-owned result buffers; core 0 copies
 * response fields after completion. system_log is an app-lifetime service
 * dependency, not a per-request output buffer.
 */
typedef struct {
  /*
   * Cross-core ordering is carried by typed acquire/release IPC words.  Core 0
   * publishes request payloads with a release-store to state=SUBMITTED; core 1
   * acquire-loads state before reading them.  Core 1 publishes response
   * payloads with release-stores to done_seq/state; core 0 acquire-loads
   * completion before copying response fields.
   */

  /* Request fields — core 0 writes before enqueue, core 1 reads */
  logger_ipc_u32_t kind;
  logger_ipc_u32_t state;
  logger_ipc_u32_t request_seq;
  logger_ipc_u32_t done_seq;

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
      size_t cap;
    } bundle_read;
    struct {
      char path[LOGGER_STORAGE_PATH_MAX];
    } file_exists;
  } params;

  logger_system_log_t *system_log;

  /* Response fields — owned by the shared service mailbox.  Core 0 copies
   * from here after DONE, so core 1 never writes through caller stack
   * pointers.  QUEUE_WRITE also uses response.queue as its mailbox-owned
   * request payload; it has no separate response body. */
  storage_service_response_t response;

  /* Completion — core 1 sets ok before publishing DONE/FAILED. */
  logger_ipc_bool_t ok;
} storage_service_t;

/* ── Worker state (for diagnostics) ────────────────────────────── */

/*
 * Approximate diagnostics only.  These counters are written by core 1 and may
 * be read by core 0 for status/CLI output.  Values may be stale and must never
 * drive control decisions.  Keep lifecycle/routing flags out of this struct.
 */
typedef struct {
  uint32_t commands_processed; /* core 1 writes, core 0 reads (approximate) */
  uint32_t write_failures;     /* core 1 writes, core 0 reads (approximate) */
  uint32_t idle_iterations;    /* core 1 writes, core 0 reads (approximate) */
  uint32_t wakeups;            /* core 1 writes, core 0 reads (approximate) */
} storage_worker_stats_t;

/* ── Shared state between core 0 and core 1 ────────────────────── */

typedef struct {
  /* The pipe that core 1 drains. Set by core 0 before launch. */
  capture_pipe_t *pipe;

  /* The session context that core 1 executes commands against.
   * Set by core 0 before launch. */
  logger_session_context_t *session_ctx;

  /* Handshake: core 0 waits on this before proceeding past BOOT. */
  logger_ipc_bool_t core1_lockout_ready;

  /* Worker statistics (core 1 writes, core 0 reads occasionally). */
  storage_worker_stats_t stats;

  /* Core-0-owned lifecycle/routing bit.  False during the pre-worker BOOT
   * window, when direct core-0 FatFS access is still allowed.  Set true by
   * core 0 after the worker is launched and both cores are flash-lockout-ready;
   * from then on, storage-service wrappers route post-boot SD/FatFS operations
   * through core 1.  Core 1 never reads or writes this field. */
  logger_ipc_bool_t storage_service_ready;

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

/*
 * True once BOOT has handed SD/FatFS ownership to the core-1 storage worker.
 *
 * This is a one-way lifecycle bit used for low-level ownership assertions in
 * storage code:
 *   - false: pre-worker BOOT window, direct core-0 storage access is allowed
 *   - true:  post-launch steady state, core 1 exclusively owns SD/FatFS
 *
 * It is written once on core 0 after launch completes and then remains true
 * until reboot.
 */
bool logger_storage_worker_owns_fatfs(void);

#endif /* LOGGER_FIRMWARE_STORAGE_WORKER_H */
