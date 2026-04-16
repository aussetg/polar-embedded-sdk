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
