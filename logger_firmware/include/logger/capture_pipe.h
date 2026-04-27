#ifndef LOGGER_FIRMWARE_CAPTURE_PIPE_H
#define LOGGER_FIRMWARE_CAPTURE_PIPE_H

/*
 * Bounded transport machinery for the capture pipeline.
 *
 * Design (from logger_capture_pipeline_v1.md):
 *
 *   BLE callback ──▶ source staging ──▶ command ring ──▶ writer consumer
 *     (core 0)        (core 0)           (core 0→1)       (core 1)
 *
 *   writer ──▶ event ring ──▶ core 0
 *     (core 1)    (core 1→0)
 *
 * Core 1 owns the command ring consumer and executes all writer
 * dispatch.  Core 0 never calls logger_writer_dispatch() directly.
 *
 * Memory budget (PSRAM-backed, see psram_layout.h):
 *   - Command ring: 256 slots × sizeof(logger_writer_cmd_t)
 *   - Source staging: 4096 slots × sizeof(logger_writer_cmd_t)
 *   - Event ring: 16 × 16 bytes = 256 bytes (SRAM)
 *
 * Constraints:
 *   - All structures are bounded and preallocated from PSRAM at init.
 *   - No heap allocation anywhere in the hot path.
 *   - No multicore FIFO for PMD payload transport.
 *   - Max single enqueue wait on core 0: 1 ms.
 *   - Distressed at >= 75% occupancy.
 *   - Recovered below 50%.
 *   - First hard failure starts a 15 s degraded deadline.
 *   - If staging overflows or deadline expires => sd_write_failed path.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "logger/writer_protocol.h"

/* ── Ring sizing ───────────────────────────────────────────────── */

#define CAPTURE_CMD_RING_CAPACITY 256u
#define CAPTURE_EVENT_RING_CAPACITY 16u

/*
 * Source staging capacity.
 *
 * 4096 slots (~22 s at 180 pkt/s, power of 2).
 * PSRAM-backed — large enough to absorb any SD card stall.
 * The BLE callback pushes into staging; the main loop drains
 * staging into the command ring that core 1 consumes.
 */
#define CAPTURE_STAGING_CAPACITY 4096u

/* ── Distress thresholds ───────────────────────────────────────── */

#define CAPTURE_DISTRESSED_THRESHOLD_PCT 75u
#define CAPTURE_RECOVERED_THRESHOLD_PCT 50u
#define CAPTURE_DEGRADED_DEADLINE_MS 15000u
#define CAPTURE_MAX_ENQUEUE_WAIT_US 1000u /* 1 ms */

/* ── Command ring ──────────────────────────────────────────────── */

/*
 * MEMORY ORDERING
 *
 * The ring uses __mem_fence_release()/__mem_fence_acquire() (both
 * are __dmb on Cortex-M33) at the SPSC boundary:
 *
 *   Producer:  store slot data, then release fence, then store tail.
 *              Guarantees the consumer sees the payload before it
 *              observes the updated tail.
 *
 *   Consumer:  load tail, then acquire fence, then load slot data.
 *              Then release fence before storing head, so the
 *              producer sees the slot as reclaimed before it
 *              observes the advanced head.
 *
 * On single-core execution these compile to a compiler barrier
 * only (no DMB emitted).  On dual-core they emit a real DMB,
 * preventing the M33 write buffer from retiring stores out of
 * order from the other core's perspective.
 */
typedef struct {
  logger_writer_cmd_t *slots; /* PSRAM-backed, set at init */
  uint32_t capacity;         /* power of 2 */
  uint32_t head; /* consumer reads here (writer side) */
  uint32_t tail; /* producer writes here (control core) */
  uint32_t seq;  /* monotonically increasing command position */
} capture_cmd_ring_t;

/* ── Event ring (writer → control core) ────────────────────────── */

typedef enum {
  CAPTURE_EVENT_BARRIER_COMPLETE = 0,
  CAPTURE_EVENT_WRITE_FAILED,
  CAPTURE_EVENT_WORKER_DISTRESS,
  CAPTURE_EVENT_STATS_SNAPSHOT,
} capture_event_kind_t;

typedef struct {
  capture_event_kind_t kind;
  uint32_t cmd_seq;
  bool success;
  uint32_t aux;
} capture_event_t;

/*
 * Same memory-ordering protocol as capture_cmd_ring_t above.
 * Producer is core 1 (writer), consumer is core 0 (control).
 */
typedef struct {
  capture_event_t slots[CAPTURE_EVENT_RING_CAPACITY];
  uint32_t head;
  uint32_t tail;
} capture_event_ring_t;

/* ── Source staging ────────────────────────────────────────────── */

/*
 * Source staging: decouples BLE callback timing from inter-core
 * ring drain timing.
 *
 * During the dual-core phase, a real staging buffer sits between
 * the BLE callback (which pushes to staging) and the main loop
 * (which drains staging into the command ring that core 1 consumes).
 */
typedef struct {
  logger_writer_cmd_t *slots; /* PSRAM-backed, set at init */
  uint32_t capacity;         /* power of 2 */
  uint32_t head;
  uint32_t tail;
  uint32_t overflow_count;
} capture_source_staging_t;

/* ── Distress state ────────────────────────────────────────────── */

typedef enum {
  CAPTURE_HEALTHY = 0,
  CAPTURE_DISTRESSED,
  CAPTURE_HARD_FAILURE,
} capture_health_t;

/* ── Writer failure classification ─────────────────────────────── */

/*
 * Canonical subreasons for the sd_write_failed fault path.
 *
 * These match the tokens listed in logger_recovery_architecture_v1.md §8.7.
 * They are for logs/telemetry and do not replace the primary visible
 * fault code (which is always sd_write_failed).
 *
 * Set by the capture pipe and storage worker at the point of failure.
 * Read by app_main when routing into the sd_write_failed recovery path
 * and when logging fault telemetry.
 */
typedef enum {
  CAPTURE_WRITER_FAILURE_NONE = 0,
  CAPTURE_WRITER_FAILURE_STAGE_OVERFLOW,      /* control_stage_overflow */
  CAPTURE_WRITER_FAILURE_ENQUEUE_TIMEOUT,     /* writer_enqueue_timeout */
  CAPTURE_WRITER_FAILURE_QUEUE_OVERFLOW,      /* writer_queue_overflow */
  CAPTURE_WRITER_FAILURE_BARRIER_FAILED,      /* writer_barrier_failed */
  CAPTURE_WRITER_FAILURE_FLUSH_FAILED,        /* writer_flush_failed */
  CAPTURE_WRITER_FAILURE_FINALIZE_FAILED,     /* writer_finalize_failed */
  CAPTURE_WRITER_FAILURE_PACKET_WRITE_FAILED, /* session_packet_write_failed */
} capture_writer_failure_t;

const char *capture_writer_failure_name(capture_writer_failure_t failure);

/*
 * Classify a writer command failure by command type.
 *
 * Returns the canonical subreason for the sd_write_failed fault path
 * given the command type that failed during dispatch on core 1.
 * Returns CAPTURE_WRITER_FAILURE_NONE for non-classifiable commands
 * (REFRESH_LIVE) and unknown types.
 *
 * This is the single source of truth for the worker-side failure
 * classification — storage_worker.c calls it instead of maintaining
 * its own if/else chain, so the logic is testable from the host-side
 * test suite without pico-sdk dependencies.
 */
capture_writer_failure_t
capture_writer_classify_cmd_failure(logger_writer_cmd_type_t cmd_type);

/* ── Telemetry ─────────────────────────────────────────────────── */

typedef struct {
  /* Command ring producer side (core 0). */
  uint32_t cmd_enqueue_count;
  uint32_t cmd_enqueue_reject_count;
  uint16_t cmd_occupancy_hwm;
  uint16_t cmd_occupancy_after_enqueue;

  /* Event ring consumer side (core 0). */
  uint32_t event_pop_count;

  /* Per-kind event tally (core 0 side, from event ring drains).
   * Every popped event increments the matching counter regardless
   * of where the drain happens (barrier wait or main loop). */
  uint32_t event_barrier_complete_count;
  uint32_t event_write_failed_count;
  uint32_t event_worker_distress_count;
  uint32_t event_stats_snapshot_count;

  /* Source staging */
  uint32_t staging_enqueue_count;
  uint32_t staging_overflow_count;
  uint32_t staging_drain_count;
  uint16_t staging_occupancy_hwm;
  uint16_t staging_occupancy_now;

  /* Health/distress telemetry. Correctness state lives directly on
   * capture_pipe_t; these are counters only. */
  uint32_t distressed_enter_count;
  uint32_t distressed_exit_count;
  uint32_t hard_failure_count;
} capture_pipe_control_telemetry_t;

typedef struct {
  /* Command ring consumer side (core 1). */
  uint32_t cmd_dequeue_count;
  uint16_t cmd_occupancy_after_dequeue;

  /* Event ring producer side (core 1). */
  uint32_t event_push_count;
  uint32_t event_push_overflow_count;
} capture_pipe_writer_telemetry_t;

typedef struct {
  /* Diagnostic-only counters split by writer core. Each sub-struct is written
   * by exactly one core; reads from the other core are approximate status/CLI
   * telemetry and must not drive control decisions. */
  capture_pipe_control_telemetry_t control;
  capture_pipe_writer_telemetry_t writer;
} capture_pipe_telemetry_t;

/* ── Top-level capture pipe ────────────────────────────────────── */

typedef struct capture_pipe {
  capture_cmd_ring_t cmd_ring;
  capture_event_ring_t event_ring;
  capture_source_staging_t staging;

  /* Reliable synchronous-command completion signal.
   *
   * Core 1 advances barriers_done_seq after dispatching any command submitted
   * through capture_pipe_submit_cmd() — whether it succeeded or failed.  PMD
   * packets are asynchronous and do not advance it; storage-service requests
   * use their own done/ok handshake and must not advance it.
   *
   * Core 0 snapshots the counter before enqueueing the synchronous command,
   * then spins until it advances.
   *
   * barrier_last_ok holds the result of the most recent synchronous command
   * dispatch.  Core 0 reads it only after seeing
   * the counter advance.
   *
   * These fields are immune to event-ring overflow.
   *
   * volatile because core 1 writes and core 0 reads without
   * a lock; the fences in the ring ops provide ordering. */
  volatile uint32_t barriers_done_seq;
  volatile bool barrier_last_ok;

  /* Reliable writer-failure publication channel.
   * Producer: core 1. Consumer/owner of health state: core 0.
   * First unobserved failure wins: while writer_failure_seq differs from
   * writer_failure_observed_seq, core 1 must not overwrite
   * writer_failure_kind. */
  volatile uint32_t writer_failure_seq;
  volatile capture_writer_failure_t writer_failure_kind;
  volatile uint32_t writer_failure_observed_seq;

  /* Distress / degraded deadline */
  capture_health_t health;
  capture_writer_failure_t last_writer_failure;
  uint32_t degraded_deadline_start_ms;
  uint32_t staging_overflow_at_degraded_start;
  bool degraded_deadline_active;
  bool hard_failure_active;

  /* Telemetry */
  capture_pipe_telemetry_t telemetry;
} capture_pipe_t;

/* ── Lifecycle ─────────────────────────────────────────────────── */

typedef struct {
  /* Caller-owned backing storage for the lifetime of the pipe.
   *
   * capture_pipe_t does not own these buffers and does not track
   * liveness.  The caller must guarantee that both slot arrays remain
   * valid and writable until the pipe is no longer used.
   *
   * In logger_firmware v1 these normally point into the fixed PSRAM
   * layout after successful psram_init().  Runtime PSRAM deinit /
   * power-down is not supported; if that ever changes, pipe users must
   * be reworked before these raw pointers can remain safe.
   *
   * Both capacities must be non-zero powers of 2. capture_pipe_init()
   * hard-asserts this invariant because the ring/staging index math uses
   * mask indexing rather than modulo.
   */
  logger_writer_cmd_t *staging_slots;
  uint32_t staging_capacity;
  logger_writer_cmd_t *cmd_ring_slots;
  uint32_t cmd_ring_capacity;
} capture_pipe_init_params_t;

void capture_pipe_init(capture_pipe_t *pipe,
                       const capture_pipe_init_params_t *params);

/* ── High-level submit (routes through staging → ring → execution) */

/*
 * Submit a PMD packet command into source staging.
 * Returns true on success, false if full/overflow.
 */
bool capture_pipe_submit_pmd(capture_pipe_t *pipe,
                             const logger_writer_cmd_t *cmd);

/*
 * Submit a barrier/control command through the pipe.
 *
 *   1. drains staging into command ring (ordering)
 *   2. enqueues the barrier into the command ring
 *   3. signals core 1 (__sev)
 *   4. waits for core 1 to process the command
 */
bool capture_pipe_submit_cmd(capture_pipe_t *pipe,
                             logger_session_context_t *session_ctx,
                             const logger_writer_cmd_t *cmd);

/* ── Source staging (BLE callback context) ─────────────────────── */

/*
 * Push a PMD packet into source staging.
 * Returns true on success, false if full/overflow.
 */
bool capture_staging_push_pmd(capture_pipe_t *pipe,
                              const logger_writer_cmd_t *cmd);
bool capture_staging_has_data(const capture_pipe_t *pipe);
uint32_t capture_staging_drain(capture_pipe_t *pipe, uint32_t max_entries);

/* ── Command ring (control core → writer) ──────────────────────── */

bool capture_cmd_ring_enqueue(capture_pipe_t *pipe,
                              const logger_writer_cmd_t *cmd);
bool capture_cmd_ring_dequeue(capture_pipe_t *pipe,
                              logger_writer_cmd_t *cmd_out);
uint32_t capture_cmd_ring_occupancy(const capture_pipe_t *pipe);
uint8_t capture_cmd_ring_occupancy_pct(const capture_pipe_t *pipe);

/* ── Event ring (writer → control core) ────────────────────────── */

bool capture_event_ring_push(capture_pipe_t *pipe,
                             const capture_event_t *event);
bool capture_event_ring_pop(capture_pipe_t *pipe, capture_event_t *out);
bool capture_event_ring_has_data(const capture_pipe_t *pipe);
void capture_pipe_publish_writer_failure(capture_pipe_t *pipe,
                                         capture_writer_failure_t failure);

/* ── Health / distress ─────────────────────────────────────────── */

capture_health_t capture_pipe_evaluate_health(capture_pipe_t *pipe,
                                              uint32_t now_ms);
bool capture_pipe_degraded_deadline_expired(const capture_pipe_t *pipe,
                                            uint32_t now_ms);
void capture_pipe_clear_degraded_deadline(capture_pipe_t *pipe);
void capture_pipe_note_hard_failure(capture_pipe_t *pipe, uint32_t now_ms);
bool capture_pipe_needs_recovery(const capture_pipe_t *pipe, uint32_t now_ms);

/* ── Event processing ─────────────────────────────────────────── */

/*
 * Process pending events from the event ring.
 * Returns the number of events processed.
 */
uint32_t capture_pipe_process_events(capture_pipe_t *pipe);

#endif /* LOGGER_FIRMWARE_CAPTURE_PIPE_H */
