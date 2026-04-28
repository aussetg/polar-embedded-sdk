#include "logger/storage_worker.h"

#include <stdio.h>
#include <string.h>

#include "hardware/sync.h"
#include "pico/flash.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"

#include "logger/flash_safety.h"
#include "logger/queue.h"
#include "logger/storage.h"
#include "logger/upload_bundle.h"

static logger_ipc_bool_t g_storage_worker_owns_fatfs;

bool logger_storage_worker_owns_fatfs(void) {
  return logger_ipc_bool_load_acquire(&g_storage_worker_owns_fatfs);
}

static storage_service_state_t
storage_svc_state_load_acquire(const storage_service_t *svc) {
  return (storage_service_state_t)logger_ipc_u32_load_acquire(&svc->state);
}

static storage_service_kind_t
storage_svc_kind_load_relaxed(const storage_service_t *svc) {
  return (storage_service_kind_t)logger_ipc_u32_load_relaxed(&svc->kind);
}

static uint32_t
storage_svc_request_seq_load_relaxed(const storage_service_t *svc) {
  return logger_ipc_u32_load_relaxed(&svc->request_seq);
}

static void storage_svc_state_store_release(storage_service_t *svc,
                                            storage_service_state_t state) {
  logger_ipc_u32_store_release(&svc->state, (uint32_t)state);
}

/* The FIFO mailbox passes a pointer as a uint32_t word.
 * Safe on RP2350 (32-bit address space).  Static assert so the
 * build catches any future port to a 64-bit target. */
static_assert(sizeof(uint32_t) == sizeof(storage_worker_shared_t *),
              "pointer does not fit in one FIFO word");

/*
 * Handle a storage service request from core 0.
 *
 * This runs on core 1, which owns SD/FatFS.
 * Reads the request from shared->service, executes the requested
 * operation using the same queue.c / upload_bundle.c / storage.c
 * functions as before, writes results back to shared->service, and
 * signals completion.
 */
static void
logger_storage_worker_handle_service(storage_worker_shared_t *shared) {
  storage_service_t *svc = &shared->service;
  const storage_service_state_t submitted_state =
      storage_svc_state_load_acquire(svc);
  const storage_service_kind_t submitted_kind =
      storage_svc_kind_load_relaxed(svc);
  if (submitted_state != STORAGE_SVC_STATE_SUBMITTED ||
      submitted_kind == STORAGE_SVC_NONE) {
    logger_ipc_bool_store_relaxed(&svc->ok, false);
    logger_ipc_u32_store_release(&svc->done_seq,
                                 storage_svc_request_seq_load_relaxed(svc));
    storage_svc_state_store_release(svc, STORAGE_SVC_STATE_FAILED);
    __sev();
    return;
  }

  storage_svc_state_store_release(svc, STORAGE_SVC_STATE_RUNNING);

  const storage_service_kind_t kind = submitted_kind;
  const uint32_t request_seq = storage_svc_request_seq_load_relaxed(svc);
  bool ok = false;

  switch (kind) {
  case STORAGE_SVC_QUEUE_LOAD: {
    ok = logger_upload_queue_load(&svc->response.queue);
    break;
  }

  case STORAGE_SVC_QUEUE_SCAN: {
    const char *utc = svc->params.utc_only.updated_at_utc[0] != '\0'
                          ? svc->params.utc_only.updated_at_utc
                          : NULL;
    ok = logger_upload_queue_scan(&svc->response.queue, svc->system_log, utc);
    break;
  }

  case STORAGE_SVC_QUEUE_WRITE: {
    ok = logger_upload_queue_write(&svc->response.queue);
    break;
  }

  case STORAGE_SVC_QUEUE_REFRESH: {
    const char *utc = svc->params.utc_only.updated_at_utc[0] != '\0'
                          ? svc->params.utc_only.updated_at_utc
                          : NULL;
    ok = logger_upload_queue_refresh_file(svc->system_log, utc,
                                          &svc->response.summary);
    break;
  }

  case STORAGE_SVC_QUEUE_PRUNE: {
    const char *utc = svc->params.prune.updated_at_utc[0] != '\0'
                          ? svc->params.prune.updated_at_utc
                          : NULL;
    ok = logger_upload_queue_prune_file(
        svc->system_log, utc, svc->params.prune.reserve_bytes,
        &svc->response.retention_pruned_count,
        &svc->response.reserve_pruned_count, &svc->response.reserve_met,
        &svc->response.summary);
    break;
  }

  case STORAGE_SVC_QUEUE_REBUILD: {
    const char *utc = svc->params.utc_only.updated_at_utc[0] != '\0'
                          ? svc->params.utc_only.updated_at_utc
                          : NULL;
    ok = logger_upload_queue_rebuild_file(svc->system_log, utc,
                                          &svc->response.summary);
    break;
  }

  case STORAGE_SVC_QUEUE_REQUEUE_BLOCKED: {
    const char *utc = svc->params.requeue.updated_at_utc[0] != '\0'
                          ? svc->params.requeue.updated_at_utc
                          : NULL;
    const char *reason = svc->params.requeue.reason[0] != '\0'
                             ? svc->params.requeue.reason
                             : NULL;
    ok = logger_upload_queue_requeue_blocked_file(svc->system_log, utc, reason,
                                                  &svc->response.requeued_count,
                                                  &svc->response.summary);
    break;
  }

  case STORAGE_SVC_STORAGE_REFRESH: {
    /* A refresh is an observation, not a readiness predicate.  The status
     * payload is useful when the card is absent, low on reserve, or otherwise
     * not ready for logging, so report mailbox success as long as the worker
     * executed the refresh path and let callers inspect the status fields. */
    (void)logger_storage_refresh(&svc->response.storage_status);
    ok = true;
    break;
  }

  case STORAGE_SVC_STORAGE_SELF_TEST: {
    static const char probe_data[] = "ok\n";
    ok = logger_storage_write_file_atomic(
             LOGGER_RECOVERY_PROBE_PATH, probe_data, sizeof(probe_data) - 1u) &&
         logger_storage_remove_file(LOGGER_RECOVERY_PROBE_PATH);
    break;
  }

  case STORAGE_SVC_BUNDLE_COMPUTE: {
    ok = logger_upload_bundle_compute(
        svc->params.bundle.dir_name, svc->params.bundle.manifest_path,
        svc->params.bundle.journal_path, svc->response.sha256,
        &svc->response.bundle_size);
    break;
  }

  case STORAGE_SVC_BUNDLE_OPEN: {
    shared->bundle_stream_open = logger_upload_bundle_stream_open(
        &shared->bundle_stream, svc->params.bundle.dir_name,
        svc->params.bundle.manifest_path, svc->params.bundle.journal_path);
    ok = shared->bundle_stream_open;
    break;
  }

  case STORAGE_SVC_BUNDLE_READ: {
    if (shared->bundle_stream_open) {
      ok = logger_upload_bundle_stream_read(
          &shared->bundle_stream, svc->response.bundle_read_data,
          svc->params.bundle_read.cap, &svc->response.bundle_read_len);
    } else {
      svc->response.bundle_read_len = 0u;
      ok = false;
    }
    break;
  }

  case STORAGE_SVC_BUNDLE_CLOSE: {
    if (shared->bundle_stream_open) {
      logger_upload_bundle_stream_close(&shared->bundle_stream);
      shared->bundle_stream_open = false;
    }
    ok = true;
    break;
  }

  case STORAGE_SVC_STORAGE_FORMAT: {
    ok = logger_storage_format(&svc->response.format_status);
    break;
  }

  case STORAGE_SVC_FILE_EXISTS: {
    svc->response.file_exists =
        logger_storage_file_exists(svc->params.file_exists.path);
    ok = true;
    break;
  }

  case STORAGE_SVC_NONE:
  default:
    ok = false;
    break;
  }

  /* Signal completion to core 0.  Response fields and ok are written before
   * the release-store completion words. */
  logger_ipc_bool_store_relaxed(&svc->ok, ok);
  logger_ipc_u32_store_release(&svc->done_seq, request_seq);
  storage_svc_state_store_release(svc, STORAGE_SVC_STATE_DONE);
  __sev();
}

/* ── Core-1 worker entry and loop ──────────────────────────────── */

/*
 * Initialize flash-safe lockout on this core (core 1).
 *
 * This hooks the inter-core FIFO IRQ so that core 0 can pause us
 * during flash-safe execute (internal flash writes from config store,
 * system log, BTstack TLV, etc.).
 *
 * After this call, multicore_lockout_victim_is_initialized(1) returns
 * true, and the multicore FIFO is reserved for lockout use.
 */
static bool storage_worker_init_flash_safety(void) {
  if (!flash_safe_execute_core_init()) {
    printf("[storage_worker] core 1: flash_safe_execute_core_init FAILED\n");
    return false;
  }
  return true;
}

static void __no_inline_not_in_flash_func(storage_worker_idle_wait_poll)(
    capture_pipe_t *pipe, uint32_t max_wait_us) {
  if (pipe == NULL || max_wait_us == 0u) {
    return;
  }

  const uint64_t deadline = time_us_64() + (uint64_t)max_wait_us;
  while (capture_cmd_ring_occupancy(pipe) == 0u && time_us_64() < deadline) {
    /* Safe on core 1: no default alarm-pool dependency, no FIFO side effects,
     * bounded latency to notice newly enqueued work. */
    busy_wait_us_32(1000u);
  }
}

/*
 * Drain all available commands from the pipe and execute them.
 *
 * This is the core-1 side of the writer dispatch. Every command
 * that core 0 enqueued is dequeued and executed here on core 1,
 * which owns SD/FatFS.
 *
 * Memory ordering: the SPSC ring acquire fence in dequeue guarantees
 * we see the command payload before we observe the advanced tail.
 * The release fence after dequeue guarantees core 0 sees the slot
 * as reclaimed before it observes the advanced head.
 */
static void __no_inline_not_in_flash_func(logger_storage_worker_drain)(
    storage_worker_shared_t *shared) {
  if (shared == NULL || shared->pipe == NULL || shared->session_ctx == NULL) {
    return;
  }

  capture_pipe_t *pipe = shared->pipe;
  uint32_t processed = 0u;
  const uint32_t batch_limit = CAPTURE_CMD_RING_CAPACITY;

  while (processed < batch_limit) {
    logger_writer_cmd_t cmd;
    if (!capture_cmd_ring_dequeue(pipe, &cmd)) {
      break;
    }

    /* Service requests use the shared struct, not writer dispatch */
    if (cmd.type == LOGGER_WRITER_SERVICE_REQUEST) {
      logger_storage_worker_handle_service(shared);
      processed += 1u;
      continue;
    }

    const bool ok = logger_writer_dispatch(shared->session_ctx, &cmd);

    /*
     * Commands submitted through capture_pipe_submit_cmd() are synchronous:
     * core 0 waits for barriers_done_seq to advance before returning to the
     * session/app path.  That includes REFRESH_LIVE.  It is intentionally not
     * classified as a journal-durability barrier, but it still needs an ACK;
     * otherwise core 0 waits for a completion that core 1 will never publish
     * and falsely escalates to sd_write_failed / writer_enqueue_timeout.
     *
     * APPEND_PMD_PACKET is asynchronous and must not ACK here.  SERVICE_REQUEST
     * is handled above by the storage-service done/ok fields and must not share
     * the barrier counter, because an unrelated service ACK could satisfy a
     * concurrent synchronous writer wait before its command has run.
     */
    const bool needs_sync_ack = cmd.type != LOGGER_WRITER_APPEND_PMD_PACKET;

    if (!ok) {
      shared->stats.write_failures += 1u;
      const capture_writer_failure_t classified =
          capture_writer_classify_cmd_failure(cmd.type);
      capture_pipe_publish_writer_failure(
          pipe, classified != CAPTURE_WRITER_FAILURE_NONE
                    ? classified
                    : CAPTURE_WRITER_FAILURE_BARRIER_FAILED);

      /* Even on failure, advance the barrier counter so core 0
       * doesn't spin for the full 5 s timeout.  Set the result
       * flag first, then release-publish the counter — core 0 reads the
       * flag only after acquire-observing the counter change. */
      if (needs_sync_ack) {
        logger_ipc_bool_store_relaxed(&pipe->barrier_last_ok, false);
        logger_ipc_u32_add_fetch_release(&pipe->barriers_done_seq, 1u);
        __sev();
      }

      /* Best-effort event ring push for telemetry. */
      capture_event_t event;
      memset(&event, 0, sizeof(event));
      event.kind = CAPTURE_EVENT_WRITE_FAILED;
      event.success = false;
      capture_event_ring_push(pipe, &event);
    } else {
      if (needs_sync_ack) {
        /* Advance the completion counter so core 0 knows this
         * barrier was dispatched, then wake it. */
        logger_ipc_bool_store_relaxed(&pipe->barrier_last_ok, true);
        logger_ipc_u32_add_fetch_release(&pipe->barriers_done_seq, 1u);
        __sev();

        /* Best-effort event ring push for telemetry. */
        capture_event_t event;
        memset(&event, 0, sizeof(event));
        event.kind = CAPTURE_EVENT_BARRIER_COMPLETE;
        event.success = true;
        capture_event_ring_push(pipe, &event);
      }
    }

    processed += 1u;
  }

  shared->stats.commands_processed += processed;
}

/*
 * Core-1 entry point.
 *
 * Called exactly once by multicore_launch_core1().
 * Never returns.
 *
 * Sequence:
 *   1. Receive shared pointer from core 0 via FIFO
 *   2. Initialize flash-safe lockout on core 1
 *   3. Signal readiness to core 0 (core1_lockout_ready = true)
 *   4. Enter permanent worker loop:
 *        - drain command ring
 *        - if no work, sleep with __wfe()
 *        - repeat forever
 */
static void logger_storage_worker_entry(void) {
  /*
   * Receive the shared pointer from core 0 BEFORE initializing
   * flash-safe lockout.
   *
   * Reason: multicore_lockout_victim_init() hooks the FIFO IRQ.
   * After that, any FIFO data that is not a lockout request ID
   * would be silently consumed by the lockout ISR.  We must
   * receive our pointer first, while the FIFO is still ours.
   */
  storage_worker_shared_t *shared =
      (storage_worker_shared_t *)multicore_fifo_pop_blocking();

  /* Initialize flash-safe lockout on core 1.
   *
   * After this call, the multicore FIFO IRQ is hooked and the
   * FIFO is reserved for lockout use.  No more raw FIFO pushes
   * from either core.
   */
  if (!storage_worker_init_flash_safety()) {
    /*
     * Lockout init failed.  We cannot signal readiness — core 0
     * must treat this as a fatal launch failure.  Halt core 1
     * here so it doesn't touch SD or interfere with core 0's
     * error path.
     *
     * core1_lockout_ready stays false.  Core 0 will time out
     * its spin and log the failure.
     */
    printf("[storage_worker] core 1: halted (lockout init failed)\n");
    while (true) {
      __wfe();
    }
  }
  logger_ipc_bool_store_release(&shared->core1_lockout_ready, true);
  __sev(); /* wake core 0 if it is in __wfe */

  printf("[storage_worker] core 1: worker loop started\n");

  /* Permanent worker loop. */
  while (true) {
    /* Drain available work. */
    logger_storage_worker_drain(shared);

    /* If the ring is empty, sleep until core 0 pushes work.
     *
     * __wfe() waits for an event (SEV). Core 0 should issue
     * __sev() after enqueueing commands.
     *
     * We use a tight inner check to avoid missing events: if
     * new data arrived between the drain and the __wfe, we
     * loop immediately instead of sleeping. */
    if (capture_cmd_ring_occupancy(shared->pipe) == 0u) {
      shared->stats.idle_iterations += 1u;
      /*
       * Bounded core-1 idle wait without using the default alarm pool.
       *
       * core 0 still issues __sev() after enqueueing work, but we do not rely
       * on WFE + alarm-pool wakeups here because that path proved fragile on
       * the worker core.  Polling at 1 ms keeps wake latency low enough for
       * command dispatch while remaining simple and robust.
       */
      storage_worker_idle_wait_poll(shared->pipe, 100000u);
      shared->stats.wakeups += 1u;
    }
  }
}

/* ── Core-0 API ────────────────────────────────────────────────── */

void logger_storage_worker_init(storage_worker_shared_t *shared,
                                capture_pipe_t *pipe,
                                logger_session_context_t *session_ctx) {
  logger_ipc_bool_store_relaxed(&g_storage_worker_owns_fatfs, false);
  memset(shared, 0, sizeof(*shared));
  shared->pipe = pipe;
  shared->session_ctx = session_ctx;
  logger_ipc_bool_store_relaxed(&shared->core1_lockout_ready, false);
  logger_ipc_bool_store_relaxed(&shared->storage_service_ready, false);
  logger_ipc_u32_store_relaxed(&shared->service.kind, STORAGE_SVC_NONE);
  logger_ipc_u32_store_relaxed(&shared->service.state, STORAGE_SVC_STATE_IDLE);
  logger_ipc_u32_store_relaxed(&shared->service.request_seq, 0u);
  logger_ipc_u32_store_relaxed(&shared->service.done_seq, 0u);
  logger_ipc_bool_store_relaxed(&shared->service.ok, false);
}

bool logger_storage_worker_launch(storage_worker_shared_t *shared) {
  if (shared == NULL) {
    return false;
  }

  printf("[storage_worker] launching core 1 storage worker\n");

  /*
   * From this point onward core 1 may execute from XIP flash.  Tell the
   * project flash-safety helper before the launch so any flash write attempted
   * during the launch/lockout-init window is rejected rather than falling back
   * to the early-boot IRQ-only path.
   */
  logger_flash_safety_note_core1_launching();

  /*
   * Launch core 1 into our entry point.
   *
   * multicore_launch_core1() uses the inter-core FIFO for its own
   * handshake protocol (sends a startup sequence and waits for
   * core 1 to echo it back).  By the time this call returns,
   * the FIFO is clean and core 1 is executing our entry function.
   *
   * We must NOT push our data before launch — the launch handshake
   * drains the FIFO.
   */
  multicore_launch_core1(logger_storage_worker_entry);

  /*
   * Now that core 1 is alive and the FIFO is ours again, send
   * the shared pointer.  Core 1's entry function calls
   * multicore_fifo_pop_blocking() to receive it BEFORE it hooks
   * the FIFO IRQ for lockout duty.
   *
   * The FIFO is 4 entries deep on RP2350, so this single push
   * succeeds immediately.
   */
  multicore_fifo_push_blocking((uint32_t)shared);
  __sev();

  /*
   * Wait for core 1 to signal lockout readiness.
   *
   * If core 1's flash-safe init fails, it halts and never
   * signals.  Use a timeout so we don't spin forever.
   *
   * The acquire fence ensures we see all of core 1's
   * initialization before we proceed.
   */
  const uint32_t launch_timeout_ms = 5000u;
  const uint32_t start_ms = (uint32_t)(time_us_64() / 1000ull);
  while (!logger_ipc_bool_load_acquire(&shared->core1_lockout_ready)) {
    if (((uint32_t)(time_us_64() / 1000ull) - start_ms) >= launch_timeout_ms) {
      printf("[storage_worker] core 1 lockout timed out\n");
      return false;
    }
    (void)best_effort_wfe_or_timeout(make_timeout_time_ms(1u));
  }

  printf("[storage_worker] core 1 lockout ready confirmed\n");

  /*
   * Initialize flash-safe lockout on core 0 as well.
   * This allows core 1 (or internal flash write paths) to
   * pause core 0 during flash operations.
   */
  if (!flash_safe_execute_core_init()) {
    printf("[storage_worker] core 0: flash_safe_execute_core_init failed\n");
    return false;
  }

  printf("[storage_worker] core 0 flash-safe init done\n");

  logger_ipc_bool_store_release(&shared->storage_service_ready, true);
  logger_ipc_bool_store_release(&g_storage_worker_owns_fatfs, true);
  return true;
}
