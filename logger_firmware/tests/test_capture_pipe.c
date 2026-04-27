/*
 * Host-side tests for capture_pipe.
 *
 * Tests the SPSC command ring, event ring, source staging buffer,
 * health/distress state machine, and inline drain-and-execute.
 *
 * Compile:
 *   gcc -Wall -Wextra -Wno-sign-conversion -Wno-conversion \
 *       -Wno-unused-function -Wno-unused-parameter \
 *       -I tests/shim -I include/ \
 *       tests/test_capture_pipe.c src/capture_pipe.c \
 *       -o test_capture_pipe
 *
 * Run:
 *   ./test_capture_pipe
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "logger/capture_pipe.h"

/* ── Test ring sizing (matches the old pre-PSRAM capacities) ─── */

#define TEST_STAGING_CAPACITY 128u
#define TEST_CMD_RING_CAPACITY 64u

/* ── Test helper: allocate and init a capture_pipe with host-heap slots ─ */

static logger_writer_cmd_t *g_test_staging;
static logger_writer_cmd_t *g_test_cmd_ring;

static void test_capture_pipe_init(capture_pipe_t *pipe) {
  /* Allocate once; reuse across tests. */
  if (!g_test_staging) {
    g_test_staging =
        (logger_writer_cmd_t *)aligned_alloc(16u,
            TEST_STAGING_CAPACITY * sizeof(logger_writer_cmd_t));
    g_test_cmd_ring =
        (logger_writer_cmd_t *)aligned_alloc(16u,
            TEST_CMD_RING_CAPACITY * sizeof(logger_writer_cmd_t));
    assert(g_test_staging != NULL && g_test_cmd_ring != NULL);
  }
  memset(g_test_staging, 0, TEST_STAGING_CAPACITY * sizeof(logger_writer_cmd_t));
  memset(g_test_cmd_ring, 0, TEST_CMD_RING_CAPACITY * sizeof(logger_writer_cmd_t));
  capture_pipe_init(pipe,
                    &(capture_pipe_init_params_t){
                        .staging_slots = g_test_staging,
                        .staging_capacity = TEST_STAGING_CAPACITY,
                        .cmd_ring_slots = g_test_cmd_ring,
                        .cmd_ring_capacity = TEST_CMD_RING_CAPACITY,
                    });
}

static void test_capture_pipe_cleanup(void) {
  free(g_test_staging);
  free(g_test_cmd_ring);
  g_test_staging = NULL;
  g_test_cmd_ring = NULL;
}

/* ── Writer dispatch stub ──────────────────────────────────────── */

struct logger_session_state;

static bool g_dispatch_fail = false;

bool logger_writer_dispatch(struct logger_session_state *ctx,
                            const logger_writer_cmd_t *cmd) {
  (void)ctx;
  (void)cmd;
  return !g_dispatch_fail;
}

/* ── Helpers ───────────────────────────────────────────────────── */

static logger_writer_cmd_t make_packet_cmd(uint32_t seq) {
  logger_writer_cmd_t cmd;
  memset(&cmd, 0, sizeof(cmd));
  cmd.type = LOGGER_WRITER_APPEND_PMD_PACKET;
  cmd.append_pmd_packet.type = LOGGER_WRITER_APPEND_PMD_PACKET;
  cmd.append_pmd_packet.seq_in_span = seq;
  cmd.append_pmd_packet.stream_kind = 1;
  cmd.append_pmd_packet.value_len = 4;
  cmd.append_pmd_packet.value[0] = (uint8_t)(seq & 0xFF);
  cmd.append_pmd_packet.value[1] = 0x01;
  cmd.append_pmd_packet.value[2] = 0x02;
  cmd.append_pmd_packet.value[3] = 0x03;
  return cmd;
}

static logger_writer_cmd_t make_barrier_cmd(void) {
  logger_writer_cmd_t cmd;
  memset(&cmd, 0, sizeof(cmd));
  cmd.type = LOGGER_WRITER_FLUSH_BARRIER;
  cmd.flush_barrier.type = LOGGER_WRITER_FLUSH_BARRIER;
  cmd.flush_barrier.boot_counter = 1;
  cmd.flush_barrier.now_ms = 1000;
  return cmd;
}

/* ── Test: init ────────────────────────────────────────────────── */

static void test_init(void) {
  printf("  init...");

  capture_pipe_t pipe;
  test_capture_pipe_init(&pipe);

  assert(pipe.health == CAPTURE_HEALTHY);
  assert(pipe.hard_failure_active == false);
  assert(pipe.degraded_deadline_active == false);
  assert(pipe.telemetry.control.cmd_enqueue_count == 0u);
  assert(pipe.telemetry.control.staging_overflow_count == 0u);
  assert(capture_cmd_ring_occupancy(&pipe) == 0u);
  assert(capture_cmd_ring_occupancy_pct(&pipe) == 0u);

  /* Freshly initialized: staging is empty */
  assert(capture_staging_has_data(&pipe) == false);
  assert(capture_event_ring_has_data(&pipe) == false);

  printf(" ok\n");
}

/* ── Test: command ring enqueue/dequeue ────────────────────────── */

static void test_cmd_ring_basic(void) {
  printf("  cmd_ring_basic...");

  capture_pipe_t pipe;
  test_capture_pipe_init(&pipe);

  logger_writer_cmd_t cmd = make_packet_cmd(0);
  assert(capture_cmd_ring_enqueue(&pipe, &cmd) == true);
  assert(capture_cmd_ring_occupancy(&pipe) == 1u);
  assert(pipe.telemetry.control.cmd_enqueue_count == 1u);

  logger_writer_cmd_t out;
  assert(capture_cmd_ring_dequeue(&pipe, &out) == true);
  assert(out.type == LOGGER_WRITER_APPEND_PMD_PACKET);
  assert(out.append_pmd_packet.seq_in_span == 0u);
  assert(capture_cmd_ring_occupancy(&pipe) == 0u);
  assert(pipe.telemetry.writer.cmd_dequeue_count == 1u);

  assert(capture_cmd_ring_dequeue(&pipe, &out) == false);

  printf(" ok\n");
}

/* ── Test: command ring full ───────────────────────────────────── */

static void test_cmd_ring_full(void) {
  printf("  cmd_ring_full...");

  capture_pipe_t pipe;
  test_capture_pipe_init(&pipe);

  for (uint32_t i = 0; i < TEST_CMD_RING_CAPACITY; i++) {
    logger_writer_cmd_t cmd = make_packet_cmd(i);
    assert(capture_cmd_ring_enqueue(&pipe, &cmd) == true);
  }
  assert(capture_cmd_ring_occupancy(&pipe) == TEST_CMD_RING_CAPACITY);

  logger_writer_cmd_t cmd = make_packet_cmd(999);
  assert(capture_cmd_ring_enqueue(&pipe, &cmd) == false);
  assert(pipe.telemetry.control.cmd_enqueue_reject_count == 1u);

  logger_writer_cmd_t out;
  assert(capture_cmd_ring_dequeue(&pipe, &out) == true);
  assert(capture_cmd_ring_enqueue(&pipe, &cmd) == true);
  assert(pipe.telemetry.control.cmd_enqueue_reject_count == 1u);

  printf(" ok\n");
}

/* ── Test: staging buffer push/drain ─────────────────────────── */

static void test_staging_push_drain(void) {
  printf("  staging_push_drain...");

  assert(TEST_STAGING_CAPACITY > 0u && "test requires real staging buffer");

  capture_pipe_t pipe;
  test_capture_pipe_init(&pipe);

  /* Push via staging — lands in staging slots, not in command ring yet */
  logger_writer_cmd_t cmd = make_packet_cmd(42);
  assert(capture_staging_push_pmd(&pipe, &cmd) == true);
  assert(pipe.telemetry.control.staging_enqueue_count == 1u);
  assert(capture_staging_has_data(&pipe) == true);
  assert(capture_cmd_ring_occupancy(&pipe) == 0u);

  /* Drain moves it into the command ring */
  assert(capture_staging_drain(&pipe, 10) == 1u);
  assert(capture_staging_has_data(&pipe) == false);
  assert(capture_cmd_ring_occupancy(&pipe) == 1u);
  assert(pipe.telemetry.control.staging_drain_count == 1u);

  /* Verify the command arrived intact */
  logger_writer_cmd_t out;
  assert(capture_cmd_ring_dequeue(&pipe, &out) == true);
  assert(out.append_pmd_packet.seq_in_span == 42u);

  printf(" ok\n");
}

/* ── Test: staging fills to capacity ─────────────────────────── */

static void test_staging_fill_and_drain(void) {
  printf("  staging_fill_and_drain...");

  assert(TEST_STAGING_CAPACITY > 0u);

  capture_pipe_t pipe;
  test_capture_pipe_init(&pipe);

  /* Fill staging to capacity */
  for (uint32_t i = 0; i < TEST_STAGING_CAPACITY; i++) {
    logger_writer_cmd_t cmd = make_packet_cmd(i);
    assert(capture_staging_push_pmd(&pipe, &cmd) == true);
  }
  assert(capture_staging_has_data(&pipe) == true);

  /* One more overflows */
  logger_writer_cmd_t overflow = make_packet_cmd(999);
  assert(capture_staging_push_pmd(&pipe, &overflow) == false);
  assert(pipe.staging.overflow_count == 1u);
  assert(pipe.telemetry.control.staging_overflow_count == 1u);

  /* Drain in batches: staging (128) > command ring (64), so we
   * drain → dequeue-ring loop until staging is empty. */
  uint32_t total_dequeued = 0u;
  while (capture_staging_has_data(&pipe)) {
    capture_staging_drain(&pipe, TEST_STAGING_CAPACITY + 1u);
    while (capture_cmd_ring_occupancy(&pipe) > 0u) {
      logger_writer_cmd_t out;
      assert(capture_cmd_ring_dequeue(&pipe, &out) == true);
      assert(out.append_pmd_packet.seq_in_span == total_dequeued);
      total_dequeued += 1u;
    }
  }
  assert(total_dequeued == TEST_STAGING_CAPACITY);
  assert(capture_staging_has_data(&pipe) == false);
  assert(capture_cmd_ring_occupancy(&pipe) == 0u);

  printf(" ok\n");
}

/* ── Test: staging overflow when buffer is full ─────────────── */

static void test_staging_overflow(void) {
  printf("  staging_overflow...");

  assert(TEST_STAGING_CAPACITY > 0u);

  capture_pipe_t pipe;
  test_capture_pipe_init(&pipe);

  /* Fill staging to capacity */
  for (uint32_t i = 0; i < TEST_STAGING_CAPACITY; i++) {
    logger_writer_cmd_t cmd = make_packet_cmd(i);
    assert(capture_staging_push_pmd(&pipe, &cmd) == true);
  }

  /* Next push overflows staging */
  logger_writer_cmd_t cmd = make_packet_cmd(999);
  assert(capture_staging_push_pmd(&pipe, &cmd) == false);
  assert(pipe.staging.overflow_count == 1u);
  assert(pipe.telemetry.control.staging_overflow_count == 1u);

  /* In the real firmware, the caller detects staging overflow directly
   * and routes to sd_write_failed immediately — it does not go through
   * the degraded-deadline path.  Verify the overflow counter and the
   * failure classification. */
  assert(pipe.staging.overflow_count == 1u);
  assert(pipe.last_writer_failure == CAPTURE_WRITER_FAILURE_STAGE_OVERFLOW);

  printf(" ok\n");
}

/* ── Test: health transitions ──────────────────────────────────── */

static void test_health_transitions(void) {
  printf("  health_transitions...");

  capture_pipe_t pipe;
  test_capture_pipe_init(&pipe);

  assert(pipe.health == CAPTURE_HEALTHY);

  /* Fill to 75% (distressed threshold) */
  const uint32_t distressed_count =
      (TEST_CMD_RING_CAPACITY * CAPTURE_DISTRESSED_THRESHOLD_PCT) / 100u;
  for (uint32_t i = 0; i < distressed_count; i++) {
    logger_writer_cmd_t cmd = make_packet_cmd(i);
    assert(capture_cmd_ring_enqueue(&pipe, &cmd) == true);
  }

  capture_health_t h = capture_pipe_evaluate_health(&pipe, 1000u);
  assert(h == CAPTURE_DISTRESSED);
  assert(pipe.telemetry.control.distressed_enter_count == 1u);

  /* Drain down to 49% (below recovered threshold of 50%) */
  const uint32_t keep_count =
      (TEST_CMD_RING_CAPACITY * (CAPTURE_RECOVERED_THRESHOLD_PCT - 1u)) /
      100u;
  const uint32_t drain_count = distressed_count - keep_count;
  for (uint32_t i = 0; i < drain_count; i++) {
    logger_writer_cmd_t out;
    assert(capture_cmd_ring_dequeue(&pipe, &out) == true);
  }

  h = capture_pipe_evaluate_health(&pipe, 2000u);
  assert(h == CAPTURE_HEALTHY);
  assert(pipe.telemetry.control.distressed_exit_count == 1u);
  assert(pipe.degraded_deadline_active == false);

  printf(" ok\n");
}

/* ── Test: hard failure and degraded deadline ──────────────────── */

static void test_hard_failure_deadline(void) {
  printf("  hard_failure_deadline...");

  capture_pipe_t pipe;
  test_capture_pipe_init(&pipe);

  capture_pipe_note_hard_failure(&pipe, 1000u);
  assert(pipe.hard_failure_active == true);
  assert(pipe.health == CAPTURE_HARD_FAILURE);
  assert(pipe.degraded_deadline_active == true);
  assert(pipe.degraded_deadline_start_ms == 1000u);

  /* At t=16000 (15s after start at t=1000), deadline expired */
  assert(capture_pipe_degraded_deadline_expired(&pipe, 16000u) == true);
  assert(capture_pipe_needs_recovery(&pipe, 16000u) == true);

  /* At t=14999 (14.999s after start), not yet */
  assert(capture_pipe_degraded_deadline_expired(&pipe, 14999u) == false);

  printf(" ok\n");
}

/* ── Test: hard failure recovery before deadline ───────────────── */

static void test_hard_failure_recovery_before_deadline(void) {
  printf("  hard_failure_recovery_before_deadline...");

  capture_pipe_t pipe;
  test_capture_pipe_init(&pipe);

  /* Simulate a hard failure (e.g. a barrier timeout) */
  capture_pipe_note_hard_failure(&pipe, 1000u);
  pipe.last_writer_failure = CAPTURE_WRITER_FAILURE_ENQUEUE_TIMEOUT;
  assert(pipe.hard_failure_active == true);
  assert(pipe.degraded_deadline_active == true);
  assert(pipe.degraded_deadline_start_ms == 1000u);

  /* Ring is already empty (no staging overflow), so evaluating health
   * before the deadline expires should recover. */
  assert(pipe.staging.overflow_count == 0u);
  capture_health_t h = capture_pipe_evaluate_health(&pipe, 5000u);
  assert(h == CAPTURE_HEALTHY);
  assert(pipe.hard_failure_active == false);
  assert(pipe.degraded_deadline_active == false);
  assert(pipe.last_writer_failure == CAPTURE_WRITER_FAILURE_NONE);
  assert(pipe.telemetry.control.distressed_exit_count == 1u);

  printf(" ok\n");
}

/* ── Test: hard failure no recovery with overflow during degraded ─ */

static void test_hard_failure_no_recovery_with_overflow(void) {
  printf("  hard_failure_no_recovery_with_overflow...");

  capture_pipe_t pipe;
  test_capture_pipe_init(&pipe);

  /* Simulate a hard failure starting the degraded period */
  capture_pipe_note_hard_failure(&pipe, 1000u);
  assert(pipe.staging_overflow_at_degraded_start == 0u);

  /* Overflow occurs DURING the degraded period */
  pipe.staging.overflow_count = 1u;

  /* Ring is empty, deadline hasn't expired, but overflow during
   * this degraded period blocks recovery. */
  capture_health_t h = capture_pipe_evaluate_health(&pipe, 5000u);
  assert(h == CAPTURE_HARD_FAILURE);
  assert(pipe.hard_failure_active == true);

  /* Overflow during degraded period triggers immediate recovery */
  assert(capture_pipe_needs_recovery(&pipe, 5000u) == true);

  printf(" ok\n");
}

/* ── Test: hard failure no recovery after deadline ────────────── */

static void test_hard_failure_no_recovery_after_deadline(void) {
  printf("  hard_failure_no_recovery_after_deadline...");

  capture_pipe_t pipe;
  test_capture_pipe_init(&pipe);

  /* Hard failure with ring still occupied (so occupancy > recovered threshold)
   */
  capture_pipe_note_hard_failure(&pipe, 1000u);

  /* Fill ring to keep occupancy high */
  for (uint32_t i = 0; i < TEST_CMD_RING_CAPACITY; i++) {
    logger_writer_cmd_t cmd = make_packet_cmd(i);
    capture_cmd_ring_enqueue(&pipe, &cmd);
  }

  /* Before deadline, ring full, should stay hard failure */
  capture_health_t h = capture_pipe_evaluate_health(&pipe, 5000u);
  assert(h == CAPTURE_HARD_FAILURE);

  /* After deadline, still hard failure */
  assert(capture_pipe_needs_recovery(&pipe, 16000u) == true);

  printf(" ok\n");
}

/* ── Test: hard failure stays sticky with full ring ────────────── */

static void test_hard_failure_recovers_when_ring_drains(void) {
  printf("  hard_failure_recovers_when_ring_drains...");

  capture_pipe_t pipe;
  test_capture_pipe_init(&pipe);

  capture_pipe_note_hard_failure(&pipe, 1000u);

  /* Fill the ring so occupancy stays above recovered threshold.
   * This prevents the recovery-before-deadline path from clearing
   * the hard failure. */
  for (uint32_t i = 0; i < TEST_CMD_RING_CAPACITY; i++) {
    logger_writer_cmd_t cmd = make_packet_cmd(i);
    capture_cmd_ring_enqueue(&pipe, &cmd);
  }

  logger_writer_cmd_t out;
  while (capture_cmd_ring_dequeue(&pipe, &out)) {
    /* drain all */
  }
  /* Now ring is empty — evaluate_health should recover since
   * no overflow and deadline hasn't expired. */
  capture_health_t h = capture_pipe_evaluate_health(&pipe, 2000u);
  assert(h == CAPTURE_HEALTHY);
  assert(pipe.hard_failure_active == false);

  printf(" ok\n");
}

/* ── Test: clear degraded deadline ─────────────────────────────── */

static void test_clear_degraded_deadline(void) {
  printf("  clear_degraded_deadline...");

  capture_pipe_t pipe;
  test_capture_pipe_init(&pipe);

  capture_pipe_note_hard_failure(&pipe, 1000u);
  assert(pipe.degraded_deadline_active == true);

  capture_pipe_clear_degraded_deadline(&pipe);
  assert(pipe.degraded_deadline_active == false);
  assert(pipe.degraded_deadline_start_ms == 0u);

  printf(" ok\n");
}

/* ── Test: event ring ──────────────────────────────────────────── */

static void test_event_ring(void) {
  printf("  event_ring...");

  capture_pipe_t pipe;
  test_capture_pipe_init(&pipe);

  capture_event_t ev = {
      .kind = CAPTURE_EVENT_BARRIER_COMPLETE,
      .cmd_seq = 42u,
      .success = true,
      .aux = 0u,
  };
  assert(capture_event_ring_push(&pipe, &ev) == true);
  assert(capture_event_ring_has_data(&pipe) == true);

  capture_event_t out;
  assert(capture_event_ring_pop(&pipe, &out) == true);
  assert(out.kind == CAPTURE_EVENT_BARRIER_COMPLETE);
  assert(out.cmd_seq == 42u);
  assert(out.success == true);
  assert(capture_event_ring_has_data(&pipe) == false);

  /* Fill to capacity */
  for (uint32_t i = 0; i < CAPTURE_EVENT_RING_CAPACITY; i++) {
    capture_event_t e = {
        .kind = CAPTURE_EVENT_STATS_SNAPSHOT,
        .cmd_seq = i,
        .success = true,
        .aux = i,
    };
    assert(capture_event_ring_push(&pipe, &e) == true);
  }
  capture_event_t extra = {.kind = CAPTURE_EVENT_WRITE_FAILED, .cmd_seq = 99};
  assert(capture_event_ring_push(&pipe, &extra) == false);
  assert(pipe.telemetry.writer.event_push_overflow_count == 1u);

  printf(" ok\n");
}

/* ── Test: occupancy percentage ────────────────────────────────── */

static void test_occupancy_pct(void) {
  printf("  occupancy_pct...");

  capture_pipe_t pipe;
  test_capture_pipe_init(&pipe);

  assert(capture_cmd_ring_occupancy_pct(&pipe) == 0u);

  for (uint32_t i = 0; i < TEST_CMD_RING_CAPACITY / 2u; i++) {
    logger_writer_cmd_t cmd = make_packet_cmd(i);
    capture_cmd_ring_enqueue(&pipe, &cmd);
  }
  assert(capture_cmd_ring_occupancy_pct(&pipe) == 50u);

  for (uint32_t i = TEST_CMD_RING_CAPACITY / 2u;
       i < TEST_CMD_RING_CAPACITY; i++) {
    logger_writer_cmd_t cmd = make_packet_cmd(i);
    capture_cmd_ring_enqueue(&pipe, &cmd);
  }
  assert(capture_cmd_ring_occupancy_pct(&pipe) == 100u);

  printf(" ok\n");
}

/* ── Test: process_events ──────────────────────────────────────── */

static void test_process_events(void) {
  printf("  process_events...");

  capture_pipe_t pipe;
  test_capture_pipe_init(&pipe);

  for (uint32_t i = 0; i < 5; i++) {
    capture_event_t ev = {
        .kind = CAPTURE_EVENT_BARRIER_COMPLETE,
        .cmd_seq = i,
        .success = true,
    };
    capture_event_ring_push(&pipe, &ev);
  }

  uint32_t processed = capture_pipe_process_events(&pipe);
  assert(processed == 5u);
  assert(capture_event_ring_has_data(&pipe) == false);

  printf(" ok\n");
}

/* ── Test: wrap-around ─────────────────────────────────────────── */

static void test_wrap_around(void) {
  printf("  wrap_around...");

  capture_pipe_t pipe;
  test_capture_pipe_init(&pipe);

  for (uint32_t round = 0; round < 10; round++) {
    for (uint32_t i = 0; i < TEST_CMD_RING_CAPACITY; i++) {
      logger_writer_cmd_t cmd = make_packet_cmd(round * 100 + i);
      assert(capture_cmd_ring_enqueue(&pipe, &cmd) == true);
    }
    for (uint32_t i = 0; i < TEST_CMD_RING_CAPACITY; i++) {
      logger_writer_cmd_t out;
      assert(capture_cmd_ring_dequeue(&pipe, &out) == true);
      assert(out.append_pmd_packet.seq_in_span == round * 100 + i);
    }
  }

  assert(capture_cmd_ring_occupancy(&pipe) == 0u);
  assert(pipe.telemetry.control.cmd_enqueue_count == 10u * TEST_CMD_RING_CAPACITY);
  assert(pipe.telemetry.writer.cmd_dequeue_count == 10u * TEST_CMD_RING_CAPACITY);

  printf(" ok\n");
}

/* ── Test: high-water marks ────────────────────────────────────── */

static void test_hwm(void) {
  printf("  hwm...");

  capture_pipe_t pipe;
  test_capture_pipe_init(&pipe);

  /* Fill ring to 32 entries */
  for (uint32_t i = 0; i < 32; i++) {
    logger_writer_cmd_t cmd = make_packet_cmd(i);
    assert(capture_cmd_ring_enqueue(&pipe, &cmd) == true);
  }
  assert(pipe.telemetry.control.cmd_occupancy_hwm == 32u);

  /* Drain 16 — HWM stays at 32 */
  for (uint32_t i = 0; i < 16; i++) {
    logger_writer_cmd_t out;
    capture_cmd_ring_dequeue(&pipe, &out);
  }
  assert(pipe.telemetry.control.cmd_occupancy_hwm == 32u);
  assert(pipe.telemetry.writer.cmd_occupancy_after_dequeue == 16u);

  printf(" ok\n");
}

/* ── Test: drain_and_execute ───────────────────────────────────── */

/* ── Test: manual drain simulating what core 1 does ────────── */

/*
 * The old capture_pipe_drain_and_execute() helper is gone — core 1
 * now owns dispatch.  This test manually drains the ring and
 * advances the barrier counter, simulating the worker's drain loop
 * so we can verify ring/event/counter mechanics without core 1.
 */
static void test_barrier_counter_and_events(void) {
  printf("  barrier_counter_and_events...");

  capture_pipe_t pipe;
  test_capture_pipe_init(&pipe);

  /* Push 4 packet commands and 1 barrier */
  for (uint32_t i = 0; i < 4; i++) {
    logger_writer_cmd_t cmd = make_packet_cmd(i);
    capture_cmd_ring_enqueue(&pipe, &cmd);
  }
  logger_writer_cmd_t barrier = make_barrier_cmd();
  capture_cmd_ring_enqueue(&pipe, &barrier);

  assert(capture_cmd_ring_occupancy(&pipe) == 5u);

  /* Simulate core 1 drain: dequeue + dispatch + advance counter. */
  const uint32_t seq_before = pipe.barriers_done_seq;
  for (uint32_t i = 0; i < 5u; i++) {
    logger_writer_cmd_t cmd;
    assert(capture_cmd_ring_dequeue(&pipe, &cmd) == true);
    const bool ok = logger_writer_dispatch(NULL, &cmd);
    assert(ok);

    const bool is_barrier = cmd.type != LOGGER_WRITER_APPEND_PMD_PACKET &&
                            cmd.type != LOGGER_WRITER_REFRESH_LIVE;
    if (is_barrier) {
      pipe.barrier_last_ok = true;
      pipe.barriers_done_seq += 1u;
      capture_event_t ev;
      memset(&ev, 0, sizeof(ev));
      ev.kind = CAPTURE_EVENT_BARRIER_COMPLETE;
      ev.success = true;
      capture_event_ring_push(&pipe, &ev);
    }
  }

  assert(capture_cmd_ring_occupancy(&pipe) == 0u);

  /* Counter advanced exactly once (one barrier). */
  assert(pipe.barriers_done_seq == seq_before + 1u);
  assert(pipe.barrier_last_ok == true);

  /* One event for the barrier */
  assert(capture_event_ring_has_data(&pipe) == true);
  capture_event_t ev;
  assert(capture_event_ring_pop(&pipe, &ev) == true);
  assert(ev.kind == CAPTURE_EVENT_BARRIER_COMPLETE);
  assert(ev.success == true);

  printf(" ok\n");
}

/* ── Test: writer failure classification ─────────────────────── */

static void test_writer_failure_names(void) {
  printf("  writer_failure_names...");

  assert(capture_writer_failure_name(CAPTURE_WRITER_FAILURE_NONE) == NULL);
  assert(
      strcmp(capture_writer_failure_name(CAPTURE_WRITER_FAILURE_STAGE_OVERFLOW),
             "control_stage_overflow") == 0);
  assert(strcmp(capture_writer_failure_name(
                    CAPTURE_WRITER_FAILURE_ENQUEUE_TIMEOUT),
                "writer_enqueue_timeout") == 0);
  assert(
      strcmp(capture_writer_failure_name(CAPTURE_WRITER_FAILURE_QUEUE_OVERFLOW),
             "writer_queue_overflow") == 0);
  assert(
      strcmp(capture_writer_failure_name(CAPTURE_WRITER_FAILURE_BARRIER_FAILED),
             "writer_barrier_failed") == 0);
  assert(
      strcmp(capture_writer_failure_name(CAPTURE_WRITER_FAILURE_FLUSH_FAILED),
             "writer_flush_failed") == 0);
  assert(strcmp(capture_writer_failure_name(
                    CAPTURE_WRITER_FAILURE_FINALIZE_FAILED),
                "writer_finalize_failed") == 0);
  assert(strcmp(capture_writer_failure_name(
                    CAPTURE_WRITER_FAILURE_PACKET_WRITE_FAILED),
                "session_packet_write_failed") == 0);

  /* Out-of-range value must return NULL so callers fall through to
   * their documented fallback instead of masking corruption. */
  assert(capture_writer_failure_name((capture_writer_failure_t)99) == NULL);

  printf(" ok\n");
}

/* ── Test: staging overflow classifies as stage_overflow ─────── */

static void test_staging_overflow_classifies(void) {
  printf("  staging_overflow_classifies...");

  capture_pipe_t pipe;
  test_capture_pipe_init(&pipe);

  assert(pipe.last_writer_failure == CAPTURE_WRITER_FAILURE_NONE);

  /* Fill staging to capacity */
  for (uint32_t i = 0; i < TEST_STAGING_CAPACITY; i++) {
    logger_writer_cmd_t cmd = make_packet_cmd(i);
    assert(capture_staging_push_pmd(&pipe, &cmd) == true);
  }

  /* Next push overflows and classifies */
  logger_writer_cmd_t cmd = make_packet_cmd(999);
  assert(capture_staging_push_pmd(&pipe, &cmd) == false);
  assert(pipe.last_writer_failure == CAPTURE_WRITER_FAILURE_STAGE_OVERFLOW);

  printf(" ok\n");
}

/* ── Test: ring full during barrier classifies as queue_overflow */

static void test_barrier_ring_full_classifies(void) {
  printf("  barrier_ring_full_classifies...");

  capture_pipe_t pipe;
  test_capture_pipe_init(&pipe);

  /* Fill command ring to capacity */
  for (uint32_t i = 0; i < TEST_CMD_RING_CAPACITY; i++) {
    logger_writer_cmd_t cmd = make_packet_cmd(i);
    assert(capture_cmd_ring_enqueue(&pipe, &cmd) == true);
  }

  /* Directly test the enqueue-failure classification path.
   * capture_pipe_submit_cmd requires non-NULL session_ctx, so we
   * simulate what it does internally: attempt the enqueue and
   * check the failure class that would be set. */
  logger_writer_cmd_t barrier = make_barrier_cmd();
  assert(capture_cmd_ring_enqueue(&pipe, &barrier) == false);

  /* Now simulate what submit_cmd does on ring-full:
   * set the failure class and note hard failure. */
  pipe.last_writer_failure = CAPTURE_WRITER_FAILURE_QUEUE_OVERFLOW;
  capture_pipe_note_hard_failure(&pipe, 1000u);

  assert(pipe.last_writer_failure == CAPTURE_WRITER_FAILURE_QUEUE_OVERFLOW);
  assert(pipe.hard_failure_active == true);

  printf(" ok\n");
}

/* ── Test: init clears failure class ──────────────────────────── */

static void test_init_clears_failure(void) {
  printf("  init_clears_failure...");

  capture_pipe_t pipe;
  test_capture_pipe_init(&pipe);
  pipe.last_writer_failure = CAPTURE_WRITER_FAILURE_ENQUEUE_TIMEOUT;
  pipe.hard_failure_active = true;

  /* Re-init should clear everything */
  test_capture_pipe_init(&pipe);
  assert(pipe.last_writer_failure == CAPTURE_WRITER_FAILURE_NONE);
  assert(pipe.hard_failure_active == false);

  printf(" ok\n");
}

/* ── Test: historical overflow doesn't poison future recovery ── */

static void test_historical_overflow_allows_recovery(void) {
  printf("  historical_overflow_allows_recovery...");

  capture_pipe_t pipe;
  test_capture_pipe_init(&pipe);

  /* 1. A historical staging overflow happens (e.g. during a transient
   *    BLE burst early in the session). */
  pipe.staging.overflow_count = 3u;
  pipe.telemetry.control.staging_overflow_count = 3u;

  /* 2. Hours pass.  The pipe recovers and is healthy. */
  assert(capture_pipe_evaluate_health(&pipe, 100000u) == CAPTURE_HEALTHY);

  /* 3. A completely unrelated hard failure occurs later. */
  capture_pipe_note_hard_failure(&pipe, 200000u);
  assert(pipe.degraded_deadline_active == true);
  assert(pipe.staging_overflow_at_degraded_start == 3u);

  /* 4. No NEW overflow during this degraded period.
   *    Ring is empty.  Deadline not expired. */
  capture_health_t h = capture_pipe_evaluate_health(&pipe, 201000u);
  assert(h == CAPTURE_HEALTHY);
  assert(pipe.hard_failure_active == false);
  assert(pipe.degraded_deadline_active == false);

  /* 5. The historical overflow counter is still there for telemetry. */
  assert(pipe.staging.overflow_count == 3u);
  assert(pipe.telemetry.control.staging_overflow_count == 3u);

  /* 6. needs_recovery also respects the per-period boundary. */
  assert(capture_pipe_needs_recovery(&pipe, 201000u) == false);

  printf(" ok\n");
}

/* ── Test: new overflow during degraded period blocks recovery ── */

static void test_new_overflow_during_degraded_blocks_recovery(void) {
  printf("  new_overflow_during_degraded_blocks_recovery...");

  capture_pipe_t pipe;
  test_capture_pipe_init(&pipe);

  /* Start with a historical overflow already present */
  pipe.staging.overflow_count = 2u;
  pipe.telemetry.control.staging_overflow_count = 2u;

  /* Hard failure starts degraded period, snapshot = 2 */
  capture_pipe_note_hard_failure(&pipe, 1000u);
  assert(pipe.staging_overflow_at_degraded_start == 2u);

  /* No new overflow yet — recovery is still possible */
  assert(capture_pipe_needs_recovery(&pipe, 5000u) == false);

  /* A NEW overflow during the degraded period */
  pipe.staging.overflow_count = 3u;
  pipe.telemetry.control.staging_overflow_count = 3u;

  /* Now it's unrecoverable — overflow during degraded period */
  assert(capture_pipe_needs_recovery(&pipe, 5000u) == true);
  capture_health_t h = capture_pipe_evaluate_health(&pipe, 5000u);
  assert(h == CAPTURE_HARD_FAILURE);

  printf(" ok\n");
}

/* ── Test: writer classification for every command type ──────── */

/*
 * Tests capture_writer_classify_cmd_failure() — the single source of
 * truth for the worker-side failure classification.  This function
 * is called by storage_worker.c on core 1 when writer dispatch fails.
 * Testing it here covers the actual if/else chain that maps command
 * types to canonical sd_write_failed subreasons.
 */

static void test_classify_flush_barrier(void) {
  printf("  classify_flush_barrier...");
  assert(capture_writer_classify_cmd_failure(LOGGER_WRITER_FLUSH_BARRIER) ==
         CAPTURE_WRITER_FAILURE_FLUSH_FAILED);
  printf(" ok\n");
}

static void test_classify_finalize_session(void) {
  printf("  classify_finalize_session...");
  assert(capture_writer_classify_cmd_failure(LOGGER_WRITER_FINALIZE_SESSION) ==
         CAPTURE_WRITER_FAILURE_FINALIZE_FAILED);
  printf(" ok\n");
}

static void test_classify_append_pmd_packet(void) {
  printf("  classify_append_pmd_packet...");
  assert(capture_writer_classify_cmd_failure(LOGGER_WRITER_APPEND_PMD_PACKET) ==
         CAPTURE_WRITER_FAILURE_PACKET_WRITE_FAILED);
  printf(" ok\n");
}

static void test_classify_barrier_commands(void) {
  printf("  classify_barrier_commands...");
  /* Every non-PACKET, non-REFRESH_LIVE command is a barrier
   * and should classify as BARRIER_FAILED. */
  assert(capture_writer_classify_cmd_failure(LOGGER_WRITER_SESSION_START) ==
         CAPTURE_WRITER_FAILURE_BARRIER_FAILED);
  assert(capture_writer_classify_cmd_failure(LOGGER_WRITER_SPAN_START) ==
         CAPTURE_WRITER_FAILURE_BARRIER_FAILED);
  assert(capture_writer_classify_cmd_failure(LOGGER_WRITER_SPAN_END) ==
         CAPTURE_WRITER_FAILURE_BARRIER_FAILED);
  assert(capture_writer_classify_cmd_failure(LOGGER_WRITER_SESSION_END) ==
         CAPTURE_WRITER_FAILURE_BARRIER_FAILED);
  assert(capture_writer_classify_cmd_failure(LOGGER_WRITER_WRITE_MARKER) ==
         CAPTURE_WRITER_FAILURE_BARRIER_FAILED);
  assert(capture_writer_classify_cmd_failure(
             LOGGER_WRITER_WRITE_STATUS_SNAPSHOT) ==
         CAPTURE_WRITER_FAILURE_BARRIER_FAILED);
  assert(capture_writer_classify_cmd_failure(LOGGER_WRITER_WRITE_H10_BATTERY) ==
         CAPTURE_WRITER_FAILURE_BARRIER_FAILED);
  assert(capture_writer_classify_cmd_failure(LOGGER_WRITER_WRITE_GAP) ==
         CAPTURE_WRITER_FAILURE_BARRIER_FAILED);
  assert(capture_writer_classify_cmd_failure(LOGGER_WRITER_WRITE_CLOCK_EVENT) ==
         CAPTURE_WRITER_FAILURE_BARRIER_FAILED);
  assert(capture_writer_classify_cmd_failure(LOGGER_WRITER_WRITE_RECOVERY) ==
         CAPTURE_WRITER_FAILURE_BARRIER_FAILED);
  printf(" ok\n");
}

static void test_classify_refresh_live_unclassified(void) {
  printf("  classify_refresh_live_unclassified...");
  /* REFRESH_LIVE is intentionally not classified — non-critical,
   * no journal durability impact. */
  assert(capture_writer_classify_cmd_failure(LOGGER_WRITER_REFRESH_LIVE) ==
         CAPTURE_WRITER_FAILURE_NONE);
  printf(" ok\n");
}

static void test_classify_service_request_unclassified(void) {
  printf("  classify_service_request_unclassified...");
  /* SERVICE_REQUEST uses the shared struct, not writer dispatch.
   * Failures are handled by the service infrastructure, not the
   * writer failure classification. */
  assert(capture_writer_classify_cmd_failure(LOGGER_WRITER_SERVICE_REQUEST) ==
         CAPTURE_WRITER_FAILURE_NONE);
  printf(" ok\n");
}

/* ── Test: worker drain classification end-to-end ───────────── */

/*
 * Simulate the worker drain loop: enqueue a command, force dispatch
 * to fail, drain via the ring, and verify the failure is classified.
 *
 * This exercises the exact code path in storage_worker.c that writes
 * last_writer_failure on core 1 — but through the extracted
 * classification function, without pico-sdk dependencies.
 */
static void test_drain_classifies_flush_barrier(void) {
  printf("  drain_classifies_flush_barrier...");

  capture_pipe_t pipe;
  test_capture_pipe_init(&pipe);

  logger_writer_cmd_t cmd = make_barrier_cmd(); /* FLUSH_BARRIER */
  assert(capture_cmd_ring_enqueue(&pipe, &cmd) == true);

  g_dispatch_fail = true;

  /* Simulate the worker drain loop: dequeue + fail + classify */
  logger_writer_cmd_t dequeued;
  assert(capture_cmd_ring_dequeue(&pipe, &dequeued) == true);
  const bool ok = logger_writer_dispatch(NULL, &dequeued);
  assert(!ok);

  capture_pipe_note_hard_failure(&pipe, 1000u);
  if (pipe.last_writer_failure == CAPTURE_WRITER_FAILURE_NONE) {
    const capture_writer_failure_t classified =
        capture_writer_classify_cmd_failure(dequeued.type);
    if (classified != CAPTURE_WRITER_FAILURE_NONE) {
      pipe.last_writer_failure = classified;
    }
  }

  assert(pipe.last_writer_failure == CAPTURE_WRITER_FAILURE_FLUSH_FAILED);
  assert(pipe.hard_failure_active == true);

  g_dispatch_fail = false;
  printf(" ok\n");
}

static void test_drain_classifies_packet_write(void) {
  printf("  drain_classifies_packet_write...");

  capture_pipe_t pipe;
  test_capture_pipe_init(&pipe);

  logger_writer_cmd_t cmd = make_packet_cmd(0);
  assert(capture_cmd_ring_enqueue(&pipe, &cmd) == true);

  g_dispatch_fail = true;

  logger_writer_cmd_t dequeued;
  assert(capture_cmd_ring_dequeue(&pipe, &dequeued) == true);
  const bool ok = logger_writer_dispatch(NULL, &dequeued);
  assert(!ok);

  capture_pipe_note_hard_failure(&pipe, 1000u);
  if (pipe.last_writer_failure == CAPTURE_WRITER_FAILURE_NONE) {
    const capture_writer_failure_t classified =
        capture_writer_classify_cmd_failure(dequeued.type);
    if (classified != CAPTURE_WRITER_FAILURE_NONE) {
      pipe.last_writer_failure = classified;
    }
  }

  assert(pipe.last_writer_failure ==
         CAPTURE_WRITER_FAILURE_PACKET_WRITE_FAILED);
  assert(pipe.hard_failure_active == true);

  g_dispatch_fail = false;
  printf(" ok\n");
}

static void test_drain_classifies_finalize(void) {
  printf("  drain_classifies_finalize...");

  capture_pipe_t pipe;
  test_capture_pipe_init(&pipe);

  logger_writer_cmd_t cmd;
  memset(&cmd, 0, sizeof(cmd));
  cmd.type = LOGGER_WRITER_FINALIZE_SESSION;
  cmd.finalize_session.type = LOGGER_WRITER_FINALIZE_SESSION;
  cmd.finalize_session.boot_counter = 1;
  assert(capture_cmd_ring_enqueue(&pipe, &cmd) == true);

  g_dispatch_fail = true;

  logger_writer_cmd_t dequeued;
  assert(capture_cmd_ring_dequeue(&pipe, &dequeued) == true);
  const bool ok = logger_writer_dispatch(NULL, &dequeued);
  assert(!ok);

  capture_pipe_note_hard_failure(&pipe, 1000u);
  if (pipe.last_writer_failure == CAPTURE_WRITER_FAILURE_NONE) {
    const capture_writer_failure_t classified =
        capture_writer_classify_cmd_failure(dequeued.type);
    if (classified != CAPTURE_WRITER_FAILURE_NONE) {
      pipe.last_writer_failure = classified;
    }
  }

  assert(pipe.last_writer_failure == CAPTURE_WRITER_FAILURE_FINALIZE_FAILED);
  assert(pipe.hard_failure_active == true);

  g_dispatch_fail = false;
  printf(" ok\n");
}

static void test_drain_first_writer_wins(void) {
  printf("  drain_first_writer_wins...");

  capture_pipe_t pipe;
  test_capture_pipe_init(&pipe);

  /* Enqueue a packet command, then a barrier command */
  logger_writer_cmd_t pkt = make_packet_cmd(0);
  logger_writer_cmd_t barrier = make_barrier_cmd();
  assert(capture_cmd_ring_enqueue(&pipe, &pkt) == true);
  assert(capture_cmd_ring_enqueue(&pipe, &barrier) == true);

  g_dispatch_fail = true;

  /* Drain both — the first failure (PACKET_WRITE) should win */
  for (int i = 0; i < 2; i++) {
    logger_writer_cmd_t dequeued;
    assert(capture_cmd_ring_dequeue(&pipe, &dequeued) == true);
    const bool ok = logger_writer_dispatch(NULL, &dequeued);
    if (!ok) {
      capture_pipe_note_hard_failure(&pipe, 1000u);
      if (pipe.last_writer_failure == CAPTURE_WRITER_FAILURE_NONE) {
        const capture_writer_failure_t classified =
            capture_writer_classify_cmd_failure(dequeued.type);
        if (classified != CAPTURE_WRITER_FAILURE_NONE) {
          pipe.last_writer_failure = classified;
        }
      }
    }
  }

  /* First failure (PACKET_WRITE) should stick; barrier's classification
   * should be suppressed by the first-writer-wins guard. */
  assert(pipe.last_writer_failure ==
         CAPTURE_WRITER_FAILURE_PACKET_WRITE_FAILED);

  g_dispatch_fail = false;
  printf(" ok\n");
}

/* ── Main ──────────────────────────────────────────────────────── */

int main(void) {
  printf("capture_pipe tests (TEST_STAGING_CAPACITY=%u,"
         " TEST_CMD_RING_CAPACITY=%u):\n",
         TEST_STAGING_CAPACITY, TEST_CMD_RING_CAPACITY);

  test_init();
  test_cmd_ring_basic();
  test_cmd_ring_full();
  test_staging_push_drain();
  test_staging_fill_and_drain();
  test_staging_overflow();
  test_health_transitions();
  test_hard_failure_deadline();
  test_hard_failure_recovery_before_deadline();
  test_hard_failure_no_recovery_with_overflow();
  test_hard_failure_no_recovery_after_deadline();
  test_hard_failure_recovers_when_ring_drains();
  test_clear_degraded_deadline();
  test_event_ring();
  test_occupancy_pct();
  test_process_events();
  test_wrap_around();
  test_hwm();
  test_barrier_counter_and_events();
  test_writer_failure_names();
  test_staging_overflow_classifies();
  test_barrier_ring_full_classifies();
  test_init_clears_failure();
  test_historical_overflow_allows_recovery();
  test_new_overflow_during_degraded_blocks_recovery();
  test_classify_flush_barrier();
  test_classify_finalize_session();
  test_classify_append_pmd_packet();
  test_classify_barrier_commands();
  test_classify_refresh_live_unclassified();
  test_classify_service_request_unclassified();
  test_drain_classifies_flush_barrier();
  test_drain_classifies_packet_write();
  test_drain_classifies_finalize();
  test_drain_first_writer_wins();

  test_capture_pipe_cleanup();
  printf("  all passed\n");
  return 0;
}
