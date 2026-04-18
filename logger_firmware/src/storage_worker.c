#include "logger/storage_worker.h"

#include <stdio.h>
#include <string.h>

#include "hardware/sync.h"
#include "pico/flash.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"

#include "logger/queue.h"
#include "logger/storage.h"
#include "logger/upload_bundle.h"

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
  const storage_service_kind_t kind = svc->kind;

  switch (kind) {
  case STORAGE_SVC_QUEUE_LOAD: {
    if (svc->queue_out != NULL) {
      svc->ok = logger_upload_queue_load(svc->queue_out);
    } else {
      svc->ok = false;
    }
    break;
  }

  case STORAGE_SVC_QUEUE_SCAN: {
    const char *utc = svc->params.utc_only.updated_at_utc[0] != '\0'
                          ? svc->params.utc_only.updated_at_utc
                          : NULL;
    if (svc->queue_out != NULL) {
      svc->ok = logger_upload_queue_scan(svc->queue_out, svc->system_log, utc);
    } else {
      svc->ok = false;
    }
    break;
  }

  case STORAGE_SVC_QUEUE_WRITE: {
    if (svc->queue_in != NULL) {
      svc->ok = logger_upload_queue_write(svc->queue_in);
    } else {
      svc->ok = false;
    }
    break;
  }

  case STORAGE_SVC_QUEUE_REFRESH: {
    const char *utc = svc->params.utc_only.updated_at_utc[0] != '\0'
                          ? svc->params.utc_only.updated_at_utc
                          : NULL;
    svc->ok = logger_upload_queue_refresh_file(svc->system_log, utc,
                                               svc->summary_out);
    break;
  }

  case STORAGE_SVC_QUEUE_PRUNE: {
    const char *utc = svc->params.prune.updated_at_utc[0] != '\0'
                          ? svc->params.prune.updated_at_utc
                          : NULL;
    svc->ok = logger_upload_queue_prune_file(
        svc->system_log, utc, svc->params.prune.reserve_bytes,
        svc->retention_pruned_out, svc->reserve_pruned_out,
        svc->reserve_met_out, svc->summary_out);
    break;
  }

  case STORAGE_SVC_QUEUE_REBUILD: {
    const char *utc = svc->params.utc_only.updated_at_utc[0] != '\0'
                          ? svc->params.utc_only.updated_at_utc
                          : NULL;
    svc->ok = logger_upload_queue_rebuild_file(svc->system_log, utc,
                                               svc->summary_out);
    break;
  }

  case STORAGE_SVC_QUEUE_REQUEUE_BLOCKED: {
    const char *utc = svc->params.requeue.updated_at_utc[0] != '\0'
                          ? svc->params.requeue.updated_at_utc
                          : NULL;
    const char *reason = svc->params.requeue.reason[0] != '\0'
                             ? svc->params.requeue.reason
                             : NULL;
    svc->ok = logger_upload_queue_requeue_blocked_file(
        svc->system_log, utc, reason, svc->requeued_count_out,
        svc->summary_out);
    break;
  }

  case STORAGE_SVC_STORAGE_REFRESH: {
    if (svc->storage_status_out != NULL) {
      svc->ok = logger_storage_refresh(svc->storage_status_out);
    } else {
      svc->ok = false;
    }
    break;
  }

  case STORAGE_SVC_STORAGE_SELF_TEST: {
    static const char probe_data[] = "ok\n";
    svc->ok =
        logger_storage_write_file_atomic(LOGGER_RECOVERY_PROBE_PATH, probe_data,
                                         sizeof(probe_data) - 1u) &&
        logger_storage_remove_file(LOGGER_RECOVERY_PROBE_PATH);
    break;
  }

  case STORAGE_SVC_BUNDLE_COMPUTE: {
    char sha256_buf[LOGGER_UPLOAD_QUEUE_SHA256_HEX_LEN + 1] = {0};
    uint64_t size_val = 0u;
    svc->ok = logger_upload_bundle_compute(
        svc->params.bundle.dir_name, svc->params.bundle.manifest_path,
        svc->params.bundle.journal_path,
        svc->sha256_out != NULL ? svc->sha256_out : sha256_buf,
        svc->bundle_size_out != NULL ? svc->bundle_size_out : &size_val);
    break;
  }

  case STORAGE_SVC_BUNDLE_OPEN: {
    shared->bundle_stream_open = logger_upload_bundle_stream_open(
        &shared->bundle_stream, svc->params.bundle.dir_name,
        svc->params.bundle.manifest_path, svc->params.bundle.journal_path);
    svc->ok = shared->bundle_stream_open;
    break;
  }

  case STORAGE_SVC_BUNDLE_READ: {
    if (shared->bundle_stream_open) {
      svc->ok = logger_upload_bundle_stream_read(
          &shared->bundle_stream, svc->params.bundle_read.dst,
          svc->params.bundle_read.cap, svc->params.bundle_read.len_out);
    } else {
      if (svc->params.bundle_read.len_out != NULL) {
        *svc->params.bundle_read.len_out = 0u;
      }
      svc->ok = false;
    }
    break;
  }

  case STORAGE_SVC_BUNDLE_CLOSE: {
    if (shared->bundle_stream_open) {
      logger_upload_bundle_stream_close(&shared->bundle_stream);
      shared->bundle_stream_open = false;
    }
    svc->ok = true;
    break;
  }

  case STORAGE_SVC_STORAGE_FORMAT: {
    svc->ok = logger_storage_format(svc->format_status_out);
    break;
  }

  case STORAGE_SVC_FILE_EXISTS: {
    const bool exists =
        logger_storage_file_exists(svc->params.file_exists.path);
    if (svc->file_exists_out != NULL) {
      *svc->file_exists_out = exists;
    }
    svc->ok = true;
    break;
  }

  case STORAGE_SVC_NONE:
  default:
    svc->ok = false;
    break;
  }

  /* Signal completion to core 0 */
  __mem_fence_release();
  svc->done = true;
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
static void logger_storage_worker_drain(storage_worker_shared_t *shared) {
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

    const bool is_barrier = cmd.type != LOGGER_WRITER_APPEND_PMD_PACKET &&
                            cmd.type != LOGGER_WRITER_REFRESH_LIVE;

    if (!ok) {
      shared->stats.write_failures += 1u;
      capture_pipe_note_hard_failure(pipe, 0u);

      /* Classify the failure by command type for telemetry.
       * first-writer-wins: only set if no earlier failure in this
       * degraded period already classified it.  Classification
       * logic lives in capture_pipe.c so it is testable without
       * pico-sdk dependencies.  See logger_recovery_architecture_v1.md
       * §8.7 canonical subreasons. */
      if (pipe->last_writer_failure == CAPTURE_WRITER_FAILURE_NONE) {
        const capture_writer_failure_t classified =
            capture_writer_classify_cmd_failure(cmd.type);
        if (classified != CAPTURE_WRITER_FAILURE_NONE) {
          pipe->last_writer_failure = classified;
        }
      }

      /* Publish failure classification before barrier counter
       * advancement or event ring push.
       *
       * For barrier commands, the release fence below before
       * barriers_done_seq would suffice on its own — but for
       * non-barrier failures (APPEND_PMD_PACKET) the only
       * subsequent release is inside capture_event_ring_push(),
       * which is best-effort and may silently fail.  Core 0 reads
       * last_writer_failure in logger_app_drain_capture_pipe()
       * without a per-field acquire fence, so we must ensure the
       * store is published here.
       *
       * This fence also covers the hard_failure_active and
       * health stores inside capture_pipe_note_hard_failure() above. */
      __mem_fence_release();

      /* Even on failure, advance the barrier counter so core 0
       * doesn't spin for the full 5 s timeout.  Set the result
       * flag first, then advance the counter — core 0 reads the
       * flag only after observing the counter change. */
      if (is_barrier) {
        pipe->barrier_last_ok = false;
        __mem_fence_release();
        pipe->barriers_done_seq += 1u;
        __mem_fence_release();
        __sev();
      }

      /* Best-effort event ring push for telemetry. */
      capture_event_t event;
      memset(&event, 0, sizeof(event));
      event.kind = CAPTURE_EVENT_WRITE_FAILED;
      event.success = false;
      capture_event_ring_push(pipe, &event);
    } else {
      if (is_barrier) {
        /* Advance the completion counter so core 0 knows this
         * barrier was dispatched, then wake it. */
        pipe->barrier_last_ok = true;
        __mem_fence_release();
        pipe->barriers_done_seq += 1u;
        __mem_fence_release();
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
  shared->core1_lockout_ready = true;
  __mem_fence_release();
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
       * Timed wait: wake on __sev() from core 0 (fast path) or
       * after 100 ms (safety net).  The timeout protects against
       * future regressions where a path enqueues without __sev().
       */
      best_effort_wfe_or_timeout(make_timeout_time_ms(100));
      shared->stats.wakeups += 1u;
    }
  }
}

/* ── Core-0 API ────────────────────────────────────────────────── */

void logger_storage_worker_init(storage_worker_shared_t *shared,
                                capture_pipe_t *pipe,
                                logger_session_context_t *session_ctx) {
  memset(shared, 0, sizeof(*shared));
  shared->pipe = pipe;
  shared->session_ctx = session_ctx;
  shared->core1_lockout_ready = false;
}

bool logger_storage_worker_launch(storage_worker_shared_t *shared) {
  if (shared == NULL) {
    return false;
  }

  printf("[storage_worker] launching core 1 storage worker\n");

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
  const uint32_t deadline_ms =
      (uint32_t)(time_us_64() / 1000ull) + launch_timeout_ms;
  while (!shared->core1_lockout_ready) {
    if ((uint32_t)(time_us_64() / 1000ull) >= deadline_ms) {
      printf("[storage_worker] core 1 lockout timed out\n");
      return false;
    }
    __wfe();
  }
  __mem_fence_acquire();

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

  shared->stats.ready = true;
  return true;
}
