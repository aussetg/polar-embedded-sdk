#include "logger/capture_pipe.h"

#include "hardware/sync.h"
#include "hardware/watchdog.h"
#include "pico/stdlib.h"
#include <string.h>

/* ── Internal helpers ──────────────────────────────────────────── */

/*
 * Ring index convention: head and tail are uint64_t counters that grow
 * monotonically (never reset).  The actual slot index is
 * (head & (cap - 1)).
 * Occupancy is (tail - head) using unsigned arithmetic.
 *
 * The unsigned subtraction is correct across the uint64_t wrap boundary
 * as long as occupancy < 2^64, which is guaranteed since occupancy <= cap
 * and cap is finite.  At 280 pkt/s the counters wrap after ~2e9 years.
 *
 * Mask indexing is correct only for non-zero power-of-two capacities.
 * capture_pipe_init() hard-asserts that invariant for both staging and the
 * command ring.
 */
static bool capture_pipe_capacity_is_power_of_two(uint32_t capacity) {
  return capacity != 0u && (capacity & (capacity - 1u)) == 0u;
}

static void capture_pipe_assert_init_params(
    const capture_pipe_init_params_t *params) {
  hard_assert(params != NULL);
  hard_assert(params->staging_slots != NULL);
  hard_assert(params->cmd_ring_slots != NULL);
  hard_assert(capture_pipe_capacity_is_power_of_two(params->staging_capacity));
  hard_assert(capture_pipe_capacity_is_power_of_two(params->cmd_ring_capacity));
}

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

/* ── Internal: set pipe health (primary + telemetry mirror) ──── */

/*
 * Every health transition MUST go through this helper so the
 * primary field and its telemetry mirror never diverge.
 */
static void capture_pipe_set_health(capture_pipe_t *pipe,
                                    capture_health_t health) {
  pipe->health = health;
  pipe->telemetry.health = health;
}

/* ── Lifecycle ─────────────────────────────────────────────────── */

void capture_pipe_init(capture_pipe_t *pipe,
                       const capture_pipe_init_params_t *params) {
  hard_assert(pipe != NULL);
  capture_pipe_assert_init_params(params);
  memset(pipe, 0, sizeof(*pipe));

  /* Wire PSRAM-backed slot arrays from caller. */
  pipe->staging.slots = params->staging_slots;
  pipe->staging.capacity = params->staging_capacity;
  pipe->cmd_ring.slots = params->cmd_ring_slots;
  pipe->cmd_ring.capacity = params->cmd_ring_capacity;

  capture_pipe_set_health(pipe, CAPTURE_HEALTHY);
  pipe->last_writer_failure = CAPTURE_WRITER_FAILURE_NONE;
}

/* ── Writer failure classification ─────────────────────────────── */

const char *capture_writer_failure_name(capture_writer_failure_t failure) {
  switch (failure) {
  case CAPTURE_WRITER_FAILURE_NONE:
    return NULL;
  case CAPTURE_WRITER_FAILURE_STAGE_OVERFLOW:
    return "control_stage_overflow";
  case CAPTURE_WRITER_FAILURE_ENQUEUE_TIMEOUT:
    return "writer_enqueue_timeout";
  case CAPTURE_WRITER_FAILURE_QUEUE_OVERFLOW:
    return "writer_queue_overflow";
  case CAPTURE_WRITER_FAILURE_BARRIER_FAILED:
    return "writer_barrier_failed";
  case CAPTURE_WRITER_FAILURE_FLUSH_FAILED:
    return "writer_flush_failed";
  case CAPTURE_WRITER_FAILURE_FINALIZE_FAILED:
    return "writer_finalize_failed";
  case CAPTURE_WRITER_FAILURE_PACKET_WRITE_FAILED:
    return "session_packet_write_failed";
  default:
    return NULL;
  }
}

capture_writer_failure_t
capture_writer_classify_cmd_failure(logger_writer_cmd_type_t cmd_type) {
  if (cmd_type == LOGGER_WRITER_FLUSH_BARRIER) {
    return CAPTURE_WRITER_FAILURE_FLUSH_FAILED;
  } else if (cmd_type == LOGGER_WRITER_FINALIZE_SESSION) {
    return CAPTURE_WRITER_FAILURE_FINALIZE_FAILED;
  } else if (cmd_type == LOGGER_WRITER_APPEND_PMD_PACKET) {
    return CAPTURE_WRITER_FAILURE_PACKET_WRITE_FAILED;
  } else {
    /* All remaining journal-visible commands are barriers.
     * REFRESH_LIVE and SERVICE_REQUEST are not classified:
     *   - REFRESH_LIVE: non-critical, no journal durability impact
     *   - SERVICE_REQUEST: uses the shared struct, handled by the
     *     service infrastructure, not the writer dispatch path */
    const bool is_classified_barrier =
        cmd_type != LOGGER_WRITER_REFRESH_LIVE &&
        cmd_type != LOGGER_WRITER_SERVICE_REQUEST;
    if (is_classified_barrier) {
      return CAPTURE_WRITER_FAILURE_BARRIER_FAILED;
    }
  }
  return CAPTURE_WRITER_FAILURE_NONE;
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
      s->tail & (s->capacity - 1u); /* power-of-2 mask */

  if ((s->tail - s->head) >= s->capacity) {
    s->overflow_count += 1u;
    pipe->telemetry.staging_overflow_count += 1u;
    /* first-writer-wins: preserve an earlier, more specific classification */
    if (pipe->last_writer_failure == CAPTURE_WRITER_FAILURE_NONE) {
      pipe->last_writer_failure = CAPTURE_WRITER_FAILURE_STAGE_OVERFLOW;
    }
    return false;
  }

  s->slots[idx] = *cmd;
  __mem_fence_release(); /* publish slot data before tail */
  s->tail += 1u;

  pipe->telemetry.staging_enqueue_count += 1u;
  const uint32_t occ = (uint32_t)(s->tail - s->head);
  if (occ > pipe->telemetry.staging_occupancy_hwm) {
    pipe->telemetry.staging_occupancy_hwm = (uint16_t)occ;
  }
  pipe->telemetry.staging_occupancy_now = (uint16_t)occ;

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
        s->head & (s->capacity - 1u); /* power-of-2 mask */

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
    pipe->telemetry.staging_occupancy_now = (uint16_t)occ;
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
  if (ring_full(r->head, r->tail, r->capacity)) {
    pipe->telemetry.cmd_enqueue_reject_count += 1u;
    return false;
  }

  const uint32_t idx = r->tail & (r->capacity - 1u);
  r->slots[idx] = *cmd;
  r->seq += 1u;
  __mem_fence_release(); /* publish slot data before tail */
  r->tail += 1u;

  pipe->telemetry.cmd_enqueue_count += 1u;
  const uint32_t occ = ring_occ(r->head, r->tail);
  if (occ > pipe->telemetry.cmd_occupancy_hwm) {
    pipe->telemetry.cmd_occupancy_hwm = (uint16_t)occ;
  }
  pipe->telemetry.cmd_occupancy_now = (uint16_t)occ;

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

  const uint32_t idx = r->head & (r->capacity - 1u);
  *cmd_out = r->slots[idx];
  memset(&r->slots[idx], 0, sizeof(r->slots[idx]));
  __mem_fence_release(); /* publish slot reuse before advancing head */
  r->head += 1u;

  pipe->telemetry.cmd_dequeue_count += 1u;
  const uint32_t occ = ring_occ(r->head, r->tail);
  pipe->telemetry.cmd_occupancy_now = (uint16_t)occ;

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
  if (pipe == NULL) {
    return 0u;
  }
  return occ_pct(capture_cmd_ring_occupancy(pipe), pipe->cmd_ring.capacity);
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
    (void)capture_staging_drain(pipe, pipe->staging.capacity);
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
    /* first-writer-wins: preserve an earlier, more specific classification */
    if (pipe->last_writer_failure == CAPTURE_WRITER_FAILURE_NONE) {
      pipe->last_writer_failure = CAPTURE_WRITER_FAILURE_QUEUE_OVERFLOW;
    }
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
      /* first-writer-wins: preserve an earlier, more specific classification */
      if (!pipe->barrier_last_ok &&
          pipe->last_writer_failure == CAPTURE_WRITER_FAILURE_NONE) {
        pipe->last_writer_failure = CAPTURE_WRITER_FAILURE_BARRIER_FAILED;
      }
      return pipe->barrier_last_ok;
    }

    /* Yield until core 1 makes progress. */
    watchdog_update();
    __wfe();
  }

  /* Timeout — core 1 did not drain the barrier within 5 s.
   * This is as serious as a write failure or a full ring.
   * Record it so the health/distress machinery and the caller
   * can see it through the normal sd_write_failed recovery path. */
  /* first-writer-wins: core 1 may have already set a more specific
   * classification (e.g. PACKET_WRITE_FAILED or FINALIZE_FAILED) before
   * this 5-second timeout expired.  Don't erase it. */
  if (pipe->last_writer_failure == CAPTURE_WRITER_FAILURE_NONE) {
    pipe->last_writer_failure = CAPTURE_WRITER_FAILURE_ENQUEUE_TIMEOUT;
  }
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

  /* Hard failure state.
   *
   * hard_failure_active is set by note_hard_failure() and cleared
   * by this function when recovery-before-deadline succeeds.
   * It is NOT a cumulative historical counter — the telemetry
   * field hard_failure_count serves that purpose.
   *
   * While active, the pipe stays in HARD_FAILURE unless ALL of
   * these are true (recovery before deadline):
   *   1. degraded deadline has NOT expired
   *   2. no staging overflow during this degraded period
   *   3. the command ring has drained below the recovered threshold
   *
   * This implements the spec requirement:
   *   "recovery before deadline may clear degraded state if no
   *    overflow occurred"
   */
  if (pipe->hard_failure_active) {
    const bool overflow_during_degraded =
        pipe->staging.overflow_count > pipe->staging_overflow_at_degraded_start;
    if (!capture_pipe_degraded_deadline_expired(pipe, now_ms) &&
        !overflow_during_degraded) {
      const uint8_t pct = capture_cmd_ring_occupancy_pct(pipe);
      if (pct < CAPTURE_RECOVERED_THRESHOLD_PCT) {
        /* Recovered before deadline with no data loss during
         * this degraded period. */
        pipe->hard_failure_active = false;
        pipe->degraded_deadline_active = false;
        pipe->degraded_deadline_start_ms = 0u;
        pipe->staging_overflow_at_degraded_start = 0u;
        pipe->last_writer_failure = CAPTURE_WRITER_FAILURE_NONE;
        capture_pipe_set_health(pipe, CAPTURE_HEALTHY);
        pipe->telemetry.distressed_exit_count += 1u;
        return CAPTURE_HEALTHY;
      }
    }
    capture_pipe_set_health(pipe, CAPTURE_HARD_FAILURE);
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
    }
  }

  capture_pipe_set_health(pipe, next);
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
  pipe->hard_failure_active = true;
  capture_pipe_set_health(pipe, CAPTURE_HARD_FAILURE);
  pipe->telemetry.hard_failure_count += 1u;

  if (!pipe->degraded_deadline_active) {
    pipe->degraded_deadline_start_ms = now_ms;
    pipe->degraded_deadline_active = true;
    /* Snapshot the cumulative staging overflow counter so we can
     * distinguish "overflow during this degraded period" from
     * "overflow at some earlier point in the session." */
    pipe->staging_overflow_at_degraded_start = pipe->staging.overflow_count;
  }
}

bool capture_pipe_needs_recovery(const capture_pipe_t *pipe, uint32_t now_ms) {
  if (pipe == NULL) {
    return false;
  }

  /* No degraded period active — nothing to recover from.
   * The cumulative overflow counter may be nonzero from historical
   * overflows, but that alone is not a recovery trigger outside
   * an active degraded window. */
  if (!pipe->degraded_deadline_active || !pipe->hard_failure_active) {
    return false;
  }

  /* Staging overflow during this degraded period is an immediate
   * recovery trigger.  Compare against the snapshot taken when the
   * degraded period started so that only overflows that happened
   * *during* the current degraded window count. */
  if (pipe->staging.overflow_count > pipe->staging_overflow_at_degraded_start) {
    return true;
  }

  /* Active degraded window with expired deadline */
  if (pipe->degraded_deadline_start_ms != 0u &&
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
