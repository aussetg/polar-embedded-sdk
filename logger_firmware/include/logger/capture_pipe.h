#ifndef LOGGER_FIRMWARE_CAPTURE_PIPE_H
#define LOGGER_FIRMWARE_CAPTURE_PIPE_H

/*
 * Bounded transport machinery for the capture pipeline.
 *
 * Design (from logger_capture_pipeline_v1.md):
 *
 *   BLE callback ──▶ source staging ──▶ command ring ──▶ writer consumer
 *     (core 0)        (core 0)           (core 0→1)       (core 1, later)
 *
 *   writer ──▶ event ring ──▶ core 0
 *     (core 1)    (core 1→0)
 *
 * For now, the writer consumer is inline (same core).  The pipe API
 * exists so the later core-1 move is a transport consumer swap, not a
 * redesign.
 *
 * Memory budget during inline phase:
 *   - Command ring only: 64 slots × sizeof(logger_writer_cmd_t) ≈ 20 KiB
 *   - Source staging: 0 slots (CAPTURE_STAGING_CAPACITY == 0)
 *   - Event ring: 16 × 16 bytes = 256 bytes
 *   - Total ≈ 20 KiB
 *
 * When core 1 launches, set CAPTURE_STAGING_CAPACITY to 128 and add
 * the staging slot array back.  That adds ~42 KiB but decouples the
 * BLE callback from the inter-core ring consumer.
 *
 * Constraints:
 *   - All structures are bounded, preallocated, static.
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

#define CAPTURE_CMD_RING_CAPACITY 64u
#define CAPTURE_EVENT_RING_CAPACITY 16u

/*
 * Source staging: only needed when core 1 owns the ring consumer.
 * During the inline single-core phase, capacity is 0 — push goes
 * directly to the command ring, saving ~70 KiB.
 *
 * When core 1 launches, set to 128 (~0.5 s ECG+ACC, power of 2).
 */
#define CAPTURE_STAGING_DUAL_CORE_CAPACITY 128u
#define CAPTURE_STAGING_CAPACITY 0u /* inline phase: no staging array */

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
 * On single-core inline execution these compile to a compiler
 * barrier only (no DMB emitted).  On dual-core they emit a real
 * DMB, preventing the M33 write buffer from retiring stores out
 * of order from the other core's perspective.
 */
typedef struct {
  logger_writer_cmd_t slots[CAPTURE_CMD_RING_CAPACITY];
  uint64_t head; /* consumer reads here (writer side) */
  uint64_t tail; /* producer writes here (control core) */
  uint64_t seq;  /* monotonically increasing command position */
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
  uint64_t head;
  uint64_t tail;
} capture_event_ring_t;

/* ── Source staging ────────────────────────────────────────────── */

/*
 * During inline phase (CAPTURE_STAGING_CAPACITY == 0), this struct
 * carries no slot array — just overflow accounting.  The push
 * function routes directly to the command ring.
 *
 * When CAPTURE_STAGING_CAPACITY > 0 (dual-core phase), a
 *   logger_writer_cmd_t slots[CAPTURE_STAGING_CAPACITY];
 * array must be added here.
 */
typedef struct {
  uint64_t head;
  uint64_t tail;
  uint32_t overflow_count;
} capture_source_staging_t;

/* ── Distress state ────────────────────────────────────────────── */

typedef enum {
  CAPTURE_HEALTHY = 0,
  CAPTURE_DISTRESSED,
  CAPTURE_HARD_FAILURE,
} capture_health_t;

/* ── Telemetry ─────────────────────────────────────────────────── */

typedef struct {
  /* Command ring */
  uint32_t cmd_enqueue_count;
  uint32_t cmd_enqueue_reject_count;
  uint32_t cmd_dequeue_count;
  uint8_t cmd_occupancy_hwm;
  uint8_t cmd_occupancy_now;

  /* Event ring */
  uint32_t event_push_count;
  uint32_t event_push_overflow_count;
  uint32_t event_pop_count;

  /* Source staging */
  uint32_t staging_enqueue_count;
  uint32_t staging_overflow_count;
  uint32_t staging_drain_count;
  uint8_t staging_occupancy_hwm;
  uint8_t staging_occupancy_now;

  /* Health / distress */
  capture_health_t health;
  uint32_t distressed_enter_count;
  uint32_t distressed_exit_count;
  uint32_t hard_failure_count;
} capture_pipe_telemetry_t;

/* ── Top-level capture pipe ────────────────────────────────────── */

typedef struct capture_pipe {
  capture_cmd_ring_t cmd_ring;
  capture_event_ring_t event_ring;
  capture_source_staging_t staging;

  /* Distress / degraded deadline */
  capture_health_t health;
  uint32_t degraded_deadline_start_ms;
  bool degraded_deadline_active;
  bool has_seen_hard_failure;

  /* Telemetry */
  capture_pipe_telemetry_t telemetry;
} capture_pipe_t;

/* ── Lifecycle ─────────────────────────────────────────────────── */

void capture_pipe_init(capture_pipe_t *pipe);

/* ── High-level submit (routes through staging → ring → execution) */

/*
 * Submit a PMD packet command.
 * During inline phase: goes directly to the command ring.
 * During dual-core phase: goes to staging, drained later.
 * Returns true on success, false if full/overflow.
 */
bool capture_pipe_submit_pmd(capture_pipe_t *pipe,
                             const logger_writer_cmd_t *cmd);

/*
 * Submit a barrier/control command through the pipe.
 *
 * Inline path:
 *   1. drains staging into command ring (ordering)
 *   2. drains command ring (executes all queued PMD packets)
 *   3. executes the barrier command
 *   4. returns the dispatch result
 *
 * Core-1 path (future):
 *   1. drains staging into command ring
 *   2. enqueues the barrier into the command ring
 *   3. waits for completion event from core 1
 */
bool capture_pipe_submit_cmd(capture_pipe_t *pipe,
                             logger_session_context_t *session_ctx,
                             const logger_writer_cmd_t *cmd);

/* ── Source staging (BLE callback context) ─────────────────────── */

/*
 * Submit a PMD packet command.
 * During inline phase: goes directly to the command ring.
 * During dual-core phase: goes to staging, drained later.
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

/* ── Health / distress ─────────────────────────────────────────── */

capture_health_t capture_pipe_evaluate_health(capture_pipe_t *pipe,
                                              uint32_t now_ms);
bool capture_pipe_degraded_deadline_expired(const capture_pipe_t *pipe,
                                            uint32_t now_ms);
void capture_pipe_clear_degraded_deadline(capture_pipe_t *pipe);
void capture_pipe_note_hard_failure(capture_pipe_t *pipe, uint32_t now_ms);
bool capture_pipe_needs_recovery(const capture_pipe_t *pipe, uint32_t now_ms);

/* ── Inline writer consumer (temporary, until core 1) ──────────── */

/*
 * Drain the command ring and execute each command inline via
 * logger_writer_dispatch().  Pushes completion/failure events
 * into the event ring.  Returns the number of commands executed.
 */
uint32_t capture_pipe_drain_and_execute(capture_pipe_t *pipe,
                                        logger_session_context_t *session_ctx,
                                        uint32_t max_cmds);

/*
 * Process pending events from the event ring.
 * Returns the number of events processed.
 */
uint32_t capture_pipe_process_events(capture_pipe_t *pipe);

#endif /* LOGGER_FIRMWARE_CAPTURE_PIPE_H */
