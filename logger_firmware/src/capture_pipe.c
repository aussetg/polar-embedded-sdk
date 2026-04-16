#include "logger/capture_pipe.h"

#include "hardware/sync.h"
#include "pico/stdlib.h"
#include <string.h>

/* ── Internal helpers ──────────────────────────────────────────── */

/*
 * Ring index convention: head and tail are uint64_t counters that grow
 * monotonically (never reset).  The actual slot index is (head % cap).
 * Occupancy is (tail - head) using unsigned arithmetic.
 *
 * The unsigned subtraction is correct across the uint64_t wrap boundary
 * as long as occupancy < 2^64, which is guaranteed since occupancy <= cap
 * (max 128).  At 280 pkt/s the counters wrap after ~2e9 years.
 */
static bool ring_full(uint64_t head, uint64_t tail, uint32_t cap) {
  return (uint32_t)(tail - head) >= cap;
}

static bool ring_empty(uint64_t head, uint64_t tail) { return head == tail; }

static uint32_t ring_occ(uint64_t head, uint64_t tail) {
  return (uint32_t)(tail - head);
}

static uint8_t occ_pct(uint32_t occ, uint32_t cap) {
  if (cap == 0u) {
    return 0u;
  }
  const uint32_t pct = (occ * 100u) / cap;
  return pct > 100u ? 100u : (uint8_t)pct;
}

/* ── Lifecycle ─────────────────────────────────────────────────── */

void capture_pipe_init(capture_pipe_t *pipe) {
  memset(pipe, 0, sizeof(*pipe));
  pipe->health = CAPTURE_HEALTHY;
}

/* ── Source staging ────────────────────────────────────────────── */

/*
 * Source staging decouples BLE callback timing from inter-core
 * ring drain timing.  The BLE callback pushes into staging; the
 * main loop drains staging into the command ring that core 1
 * consumes.
 */

bool capture_staging_push_pmd(capture_pipe_t *pipe,
                              const logger_writer_cmd_t *cmd) {
  if (pipe == NULL || cmd == NULL) {
    return false;
  }

  capture_source_staging_t *s = &pipe->staging;
  const uint32_t idx =
      s->tail & (CAPTURE_STAGING_CAPACITY - 1u); /* power-of-2 mask */

  if ((s->tail - s->head) >= CAPTURE_STAGING_CAPACITY) {
    s->overflow_count += 1u;
    pipe->telemetry.staging_overflow_count += 1u;
    return false;
  }

  s->slots[idx] = *cmd;
  __mem_fence_release(); /* publish slot data before tail */
  s->tail += 1u;

  pipe->telemetry.staging_enqueue_count += 1u;
  const uint32_t occ = (uint32_t)(s->tail - s->head);
  if (occ > pipe->telemetry.staging_occupancy_hwm) {
    pipe->telemetry.staging_occupancy_hwm = (uint8_t)occ;
  }
  pipe->telemetry.staging_occupancy_now = (uint8_t)occ;

  return true;
}

bool capture_staging_has_data(const capture_pipe_t *pipe) {
  if (pipe == NULL) {
    return false;
  }
  return pipe->staging.head != pipe->staging.tail;
}

uint32_t capture_staging_drain(capture_pipe_t *pipe, uint32_t max_entries) {
  if (pipe == NULL) {
    return 0u;
  }

  uint32_t drained = 0u;
  capture_source_staging_t *s = &pipe->staging;

  while (drained < max_entries && s->head != s->tail) {
    const uint32_t idx =
        s->head & (CAPTURE_STAGING_CAPACITY - 1u); /* power-of-2 mask */

    if (!capture_cmd_ring_enqueue(pipe, &s->slots[idx])) {
      break;
    }
    __mem_fence_acquire(); /* see slot data after checking tail */
    memset(&s->slots[idx], 0, sizeof(s->slots[idx]));
    __mem_fence_release(); /* publish slot reuse before advancing head */
    s->head += 1u;
    drained += 1u;
  }

  if (drained > 0u) {
    pipe->telemetry.staging_drain_count += drained;
    const uint32_t occ = (uint32_t)(s->tail - s->head);
    pipe->telemetry.staging_occupancy_now = (uint8_t)occ;
  }

  return drained;
}

/* ── Command ring ──────────────────────────────────────────────── */

bool capture_cmd_ring_enqueue(capture_pipe_t *pipe,
                              const logger_writer_cmd_t *cmd) {
  if (pipe == NULL || cmd == NULL) {
    return false;
  }

  capture_cmd_ring_t *r = &pipe->cmd_ring;
  if (ring_full(r->head, r->tail, CAPTURE_CMD_RING_CAPACITY)) {
    pipe->telemetry.cmd_enqueue_reject_count += 1u;
    return false;
  }

  const uint32_t idx = r->tail % CAPTURE_CMD_RING_CAPACITY;
  r->slots[idx] = *cmd;
  r->seq += 1u;
  __mem_fence_release(); /* publish slot data before tail */
  r->tail += 1u;

  pipe->telemetry.cmd_enqueue_count += 1u;
  const uint32_t occ = ring_occ(r->head, r->tail);
  if (occ > pipe->telemetry.cmd_occupancy_hwm) {
    pipe->telemetry.cmd_occupancy_hwm = (uint8_t)occ;
  }
  pipe->telemetry.cmd_occupancy_now = (uint8_t)occ;

  return true;
}

bool capture_cmd_ring_dequeue(capture_pipe_t *pipe,
                              logger_writer_cmd_t *cmd_out) {
  if (pipe == NULL || cmd_out == NULL) {
    return false;
  }

  capture_cmd_ring_t *r = &pipe->cmd_ring;
  if (ring_empty(r->head, r->tail)) {
    return false;
  }
  __mem_fence_acquire(); /* see slot data after reading tail */

  const uint32_t idx = r->head % CAPTURE_CMD_RING_CAPACITY;
  *cmd_out = r->slots[idx];
  memset(&r->slots[idx], 0, sizeof(r->slots[idx]));
  __mem_fence_release(); /* publish slot reuse before advancing head */
  r->head += 1u;

  pipe->telemetry.cmd_dequeue_count += 1u;
  const uint32_t occ = ring_occ(r->head, r->tail);
  pipe->telemetry.cmd_occupancy_now = (uint8_t)occ;

  return true;
}

uint32_t capture_cmd_ring_occupancy(const capture_pipe_t *pipe) {
  if (pipe == NULL) {
    return 0u;
  }
  /*
   * Acquire fence: head is written by core 1 (release fence in
   * dequeue).  Without this acquire, core 0 can observe a stale
   * head value, making occupancy appear higher than reality.
   * That's safe (longer spin) but architecturally wrong without
   * the fence.  tail is core 0's own word and is always current.
   */
  __mem_fence_acquire();
  return ring_occ(pipe->cmd_ring.head, pipe->cmd_ring.tail);
}

uint8_t capture_cmd_ring_occupancy_pct(const capture_pipe_t *pipe) {
  return occ_pct(capture_cmd_ring_occupancy(pipe), CAPTURE_CMD_RING_CAPACITY);
}

/* ── Event ring ────────────────────────────────────────────────── */

bool capture_event_ring_push(capture_pipe_t *pipe,
                             const capture_event_t *event) {
  if (pipe == NULL || event == NULL) {
    return false;
  }

  capture_event_ring_t *r = &pipe->event_ring;
  if (ring_full(r->head, r->tail, CAPTURE_EVENT_RING_CAPACITY)) {
    pipe->telemetry.event_push_overflow_count += 1u;
    return false;
  }

  const uint32_t idx = r->tail % CAPTURE_EVENT_RING_CAPACITY;
  r->slots[idx] = *event;
  __mem_fence_release(); /* publish slot data before tail */
  r->tail += 1u;

  pipe->telemetry.event_push_count += 1u;
  return true;
}

bool capture_event_ring_pop(capture_pipe_t *pipe, capture_event_t *out) {
  if (pipe == NULL || out == NULL) {
    return false;
  }

  capture_event_ring_t *r = &pipe->event_ring;
  if (ring_empty(r->head, r->tail)) {
    return false;
  }
  __mem_fence_acquire(); /* see slot data after reading tail */

  const uint32_t idx = r->head % CAPTURE_EVENT_RING_CAPACITY;
  *out = r->slots[idx];
  memset(&r->slots[idx], 0, sizeof(r->slots[idx]));
  __mem_fence_release(); /* publish slot reuse before advancing head */
  r->head += 1u;

  pipe->telemetry.event_pop_count += 1u;
  return true;
}

bool capture_event_ring_has_data(const capture_pipe_t *pipe) {
  if (pipe == NULL) {
    return false;
  }
  /* tail is written by core 1 (release fence in push). */
  __mem_fence_acquire();
  return !ring_empty(pipe->event_ring.head, pipe->event_ring.tail);
}

/* ── Internal: tally one popped event into telemetry ─────────── */

static void capture_pipe_tally_event(capture_pipe_t *pipe,
                                     const capture_event_t *event) {
  switch (event->kind) {
  case CAPTURE_EVENT_BARRIER_COMPLETE:
    pipe->telemetry.event_barrier_complete_count += 1u;
    break;
  case CAPTURE_EVENT_WRITE_FAILED:
    pipe->telemetry.event_write_failed_count += 1u;
    break;
  case CAPTURE_EVENT_WORKER_DISTRESS:
    pipe->telemetry.event_worker_distress_count += 1u;
    break;
  case CAPTURE_EVENT_STATS_SNAPSHOT:
    pipe->telemetry.event_stats_snapshot_count += 1u;
    break;
  }
}

/* ── High-level submit ─────────────────────────────────────────── */

bool capture_pipe_submit_pmd(capture_pipe_t *pipe,
                             const logger_writer_cmd_t *cmd) {
  return capture_staging_push_pmd(pipe, cmd);
}

bool capture_pipe_submit_cmd(capture_pipe_t *pipe,
                             logger_session_context_t *session_ctx,
                             const logger_writer_cmd_t *cmd) {
  if (pipe == NULL || session_ctx == NULL || cmd == NULL) {
    return false;
  }

  /* Drain any pending staging data first (ordering) */
  if (capture_staging_has_data(pipe)) {
    (void)capture_staging_drain(pipe, CAPTURE_STAGING_DUAL_CORE_CAPACITY);
  }

  /* Enqueue the barrier command into the command ring.
   *
   * Core 1 will drain all queued commands (both PMD packets and
   * this barrier) in order.  The ordering guarantee is that
   * everything in the ring before this barrier is executed first.
   *
   * After enqueueing, wait for the ring to drain so the barrier
   * is processed.  Core 1 signals __sev() after draining.
   *
   * We poll the ring occupancy with a timeout rather than blocking
   * indefinitely.  If the ring is stuck, the health/degraded
   * deadline machinery in the caller will catch it.
   */
  if (!capture_cmd_ring_enqueue(pipe, cmd)) {
    /* Ring is full — barrier cannot be enqueued.
     * This is a hard failure condition. */
    capture_event_t event;
    memset(&event, 0, sizeof(event));
    event.kind = CAPTURE_EVENT_WRITE_FAILED;
    event.success = false;
    capture_pipe_note_hard_failure(pipe, 0u);
    capture_event_ring_push(pipe, &event);
    return false;
  }

  /* Signal core 1 that work is available. */
  __sev();

  /*
   * Wait for core 1 to process the barrier.
   *
   * Snapshot the completion counter before the barrier was
   * enqueued, then spin until it advances — meaning core 1
   * has actually dispatched the command.
   *
   * After the counter advances, read barrier_last_ok for the
   * dispatch result.  This is reliable regardless of whether
   * the event ring overflowed.
   *
   * Also drain any event ring entries for telemetry, but do
   * not rely on them for the barrier result.
   */
  const uint32_t wait_seq = pipe->barriers_done_seq;
  const uint32_t barrier_deadline_us = 5000000u; /* 5 s */
  const uint64_t start_us = time_us_64();

  while ((time_us_64() - start_us) < barrier_deadline_us) {
    /* Drain event ring entries for telemetry (best-effort).
     * WRITE_FAILED here is redundant — the counter + flag are
     * authoritative — but drain anyway so the ring doesn't fill.
     * Tally every popped event by kind so nothing is silently lost. */
    capture_event_t event;
    while (capture_event_ring_pop(pipe, &event)) {
      capture_pipe_tally_event(pipe, &event);
    }

    /* Core 1 set barrier_last_ok before advancing barriers_done_seq.
     * The acquire fence ensures we see the result flag. */
    if (pipe->barriers_done_seq != wait_seq) {
      __mem_fence_acquire();
      return pipe->barrier_last_ok;
    }

    /* Yield until core 1 makes progress. */
    __wfe();
  }

  /* Timeout — core 1 did not drain the barrier within 5 s.
   * This is as serious as a write failure or a full ring.
   * Record it so the health/distress machinery and the caller
   * can see it through the normal sd_write_failed recovery path. */
  capture_pipe_note_hard_failure(pipe, 0u);
  {
    capture_event_t event;
    memset(&event, 0, sizeof(event));
    event.kind = CAPTURE_EVENT_WRITE_FAILED;
    event.success = false;
    capture_event_ring_push(pipe, &event);
  }
  return false;
}

/* ── Health / distress ─────────────────────────────────────────── */

capture_health_t capture_pipe_evaluate_health(capture_pipe_t *pipe,
                                              uint32_t now_ms) {
  if (pipe == NULL) {
    return CAPTURE_HEALTHY;
  }
  (void)now_ms;

  /* Hard failure is a terminal latch — no transitions possible.
   * Check first to avoid computing dead occupancy transitions
   * and firing misleading telemetry counters. */
  if (pipe->has_seen_hard_failure) {
    pipe->health = CAPTURE_HARD_FAILURE;
    pipe->telemetry.health = CAPTURE_HARD_FAILURE;
    return CAPTURE_HARD_FAILURE;
  }

  const uint8_t pct = capture_cmd_ring_occupancy_pct(pipe);
  const capture_health_t prev = pipe->health;
  capture_health_t next = prev;

  if (pct >= CAPTURE_DISTRESSED_THRESHOLD_PCT) {
    next = CAPTURE_DISTRESSED;
  } else if (prev != CAPTURE_HEALTHY && pct < CAPTURE_RECOVERED_THRESHOLD_PCT) {
    next = CAPTURE_HEALTHY;
  }

  if (next != prev) {
    if (prev == CAPTURE_HEALTHY && next == CAPTURE_DISTRESSED) {
      pipe->telemetry.distressed_enter_count += 1u;
    } else if (prev == CAPTURE_DISTRESSED && next == CAPTURE_HEALTHY) {
      pipe->telemetry.distressed_exit_count += 1u;
      /* Recovery: clear degraded deadline if no overflow occurred */
      if (pipe->staging.overflow_count == 0u) {
        pipe->degraded_deadline_start_ms = 0u;
        pipe->degraded_deadline_active = false;
      }
    }
  }

  pipe->health = next;
  pipe->telemetry.health = next;
  return next;
}

bool capture_pipe_degraded_deadline_expired(const capture_pipe_t *pipe,
                                            uint32_t now_ms) {
  if (pipe == NULL || !pipe->degraded_deadline_active) {
    return false;
  }
  if (pipe->degraded_deadline_start_ms == 0u) {
    return false;
  }
  return (now_ms - pipe->degraded_deadline_start_ms) >=
         CAPTURE_DEGRADED_DEADLINE_MS;
}

void capture_pipe_clear_degraded_deadline(capture_pipe_t *pipe) {
  if (pipe == NULL) {
    return;
  }
  pipe->degraded_deadline_start_ms = 0u;
  pipe->degraded_deadline_active = false;
}

void capture_pipe_note_hard_failure(capture_pipe_t *pipe, uint32_t now_ms) {
  if (pipe == NULL) {
    return;
  }
  pipe->has_seen_hard_failure = true;
  pipe->health = CAPTURE_HARD_FAILURE;
  pipe->telemetry.health = CAPTURE_HARD_FAILURE;
  pipe->telemetry.hard_failure_count += 1u;

  if (!pipe->degraded_deadline_active) {
    pipe->degraded_deadline_start_ms = now_ms;
    pipe->degraded_deadline_active = true;
  }
}

bool capture_pipe_needs_recovery(const capture_pipe_t *pipe, uint32_t now_ms) {
  if (pipe == NULL) {
    return false;
  }

  /* Staging overflow is an immediate recovery trigger */
  if (pipe->staging.overflow_count > 0u) {
    return true;
  }

  /* Hard failure with expired degraded deadline */
  if (pipe->has_seen_hard_failure && pipe->degraded_deadline_active &&
      pipe->degraded_deadline_start_ms != 0u &&
      (now_ms - pipe->degraded_deadline_start_ms) >=
          CAPTURE_DEGRADED_DEADLINE_MS) {
    return true;
  }

  return false;
}

/* ── Event processing ─────────────────────────────────────────── */

uint32_t capture_pipe_process_events(capture_pipe_t *pipe) {
  if (pipe == NULL) {
    return 0u;
  }

  uint32_t processed = 0u;
  capture_event_t event;

  while (capture_event_ring_pop(pipe, &event)) {
    capture_pipe_tally_event(pipe, &event);
    processed += 1u;
  }

  return processed;
}
