/*
 * Host-side tests for capture_pipe (inline single-core phase).
 *
 * During the inline phase (CAPTURE_STAGING_CAPACITY == 0), staging push
 * goes directly to the command ring and drain is a no-op.  These tests
 * verify that contract.  The dual-core staging path should be tested
 * separately when core 1 is introduced.
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
#include <string.h>

#include "logger/capture_pipe.h"

/* ── Writer dispatch stub ──────────────────────────────────────── */

struct logger_session_state;

bool logger_writer_dispatch(struct logger_session_state *ctx,
                            const logger_writer_cmd_t *cmd) {
  (void)ctx;
  (void)cmd;
  return true;
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
  capture_pipe_init(&pipe);

  assert(pipe.health == CAPTURE_HEALTHY);
  assert(pipe.has_seen_hard_failure == false);
  assert(pipe.degraded_deadline_active == false);
  assert(pipe.telemetry.cmd_enqueue_count == 0u);
  assert(pipe.telemetry.staging_overflow_count == 0u);
  assert(capture_cmd_ring_occupancy(&pipe) == 0u);
  assert(capture_cmd_ring_occupancy_pct(&pipe) == 0u);

  /* Inline phase: staging has_data is always false */
  assert(capture_staging_has_data(&pipe) == false);
  assert(capture_event_ring_has_data(&pipe) == false);

  printf(" ok\n");
}

/* ── Test: command ring enqueue/dequeue ────────────────────────── */

static void test_cmd_ring_basic(void) {
  printf("  cmd_ring_basic...");

  capture_pipe_t pipe;
  capture_pipe_init(&pipe);

  logger_writer_cmd_t cmd = make_packet_cmd(0);
  assert(capture_cmd_ring_enqueue(&pipe, &cmd) == true);
  assert(capture_cmd_ring_occupancy(&pipe) == 1u);
  assert(pipe.telemetry.cmd_enqueue_count == 1u);

  logger_writer_cmd_t out;
  assert(capture_cmd_ring_dequeue(&pipe, &out) == true);
  assert(out.type == LOGGER_WRITER_APPEND_PMD_PACKET);
  assert(out.append_pmd_packet.seq_in_span == 0u);
  assert(capture_cmd_ring_occupancy(&pipe) == 0u);
  assert(pipe.telemetry.cmd_dequeue_count == 1u);

  assert(capture_cmd_ring_dequeue(&pipe, &out) == false);

  printf(" ok\n");
}

/* ── Test: command ring full ───────────────────────────────────── */

static void test_cmd_ring_full(void) {
  printf("  cmd_ring_full...");

  capture_pipe_t pipe;
  capture_pipe_init(&pipe);

  for (uint32_t i = 0; i < CAPTURE_CMD_RING_CAPACITY; i++) {
    logger_writer_cmd_t cmd = make_packet_cmd(i);
    assert(capture_cmd_ring_enqueue(&pipe, &cmd) == true);
  }
  assert(capture_cmd_ring_occupancy(&pipe) == CAPTURE_CMD_RING_CAPACITY);

  logger_writer_cmd_t cmd = make_packet_cmd(999);
  assert(capture_cmd_ring_enqueue(&pipe, &cmd) == false);
  assert(pipe.telemetry.cmd_enqueue_reject_count == 1u);

  logger_writer_cmd_t out;
  assert(capture_cmd_ring_dequeue(&pipe, &out) == true);
  assert(capture_cmd_ring_enqueue(&pipe, &cmd) == true);
  assert(pipe.telemetry.cmd_enqueue_reject_count == 1u);

  printf(" ok\n");
}

/* ── Test: staging push goes directly to command ring (inline) ── */

static void test_staging_inline_passthrough(void) {
  printf("  staging_inline_passthrough...");

  assert(CAPTURE_STAGING_CAPACITY == 0u && "test requires inline phase");

  capture_pipe_t pipe;
  capture_pipe_init(&pipe);

  /* Push via staging — should land in command ring directly */
  logger_writer_cmd_t cmd = make_packet_cmd(42);
  assert(capture_staging_push_pmd(&pipe, &cmd) == true);
  assert(pipe.telemetry.staging_enqueue_count == 1u);

  /* Staging has_data is always false in inline mode */
  assert(capture_staging_has_data(&pipe) == false);

  /* Drain is a no-op */
  assert(capture_staging_drain(&pipe, 10) == 0u);

  /* But the command ring has the entry */
  assert(capture_cmd_ring_occupancy(&pipe) == 1u);
  logger_writer_cmd_t out;
  assert(capture_cmd_ring_dequeue(&pipe, &out) == true);
  assert(out.append_pmd_packet.seq_in_span == 42u);

  printf(" ok\n");
}

/* ── Test: staging overflow = command ring full (inline) ──────── */

static void test_staging_inline_overflow(void) {
  printf("  staging_inline_overflow...");

  capture_pipe_t pipe;
  capture_pipe_init(&pipe);

  /* Fill command ring via staging */
  for (uint32_t i = 0; i < CAPTURE_CMD_RING_CAPACITY; i++) {
    logger_writer_cmd_t cmd = make_packet_cmd(i);
    assert(capture_staging_push_pmd(&pipe, &cmd) == true);
  }

  /* Next staging push fails — counted as staging overflow */
  logger_writer_cmd_t cmd = make_packet_cmd(999);
  assert(capture_staging_push_pmd(&pipe, &cmd) == false);
  assert(pipe.staging.overflow_count == 1u);
  assert(pipe.telemetry.staging_overflow_count == 1u);

  /* Needs recovery should be true */
  assert(capture_pipe_needs_recovery(&pipe, 0u) == true);

  printf(" ok\n");
}

/* ── Test: health transitions ──────────────────────────────────── */

static void test_health_transitions(void) {
  printf("  health_transitions...");

  capture_pipe_t pipe;
  capture_pipe_init(&pipe);

  assert(pipe.health == CAPTURE_HEALTHY);

  /* Fill to 75% (distressed threshold) */
  const uint32_t distressed_count =
      (CAPTURE_CMD_RING_CAPACITY * CAPTURE_DISTRESSED_THRESHOLD_PCT) / 100u;
  for (uint32_t i = 0; i < distressed_count; i++) {
    logger_writer_cmd_t cmd = make_packet_cmd(i);
    assert(capture_cmd_ring_enqueue(&pipe, &cmd) == true);
  }

  capture_health_t h = capture_pipe_evaluate_health(&pipe, 1000u);
  assert(h == CAPTURE_DISTRESSED);
  assert(pipe.telemetry.distressed_enter_count == 1u);

  /* Drain down to 49% (below recovered threshold of 50%) */
  const uint32_t keep_count =
      (CAPTURE_CMD_RING_CAPACITY * (CAPTURE_RECOVERED_THRESHOLD_PCT - 1u)) /
      100u;
  const uint32_t drain_count = distressed_count - keep_count;
  for (uint32_t i = 0; i < drain_count; i++) {
    logger_writer_cmd_t out;
    assert(capture_cmd_ring_dequeue(&pipe, &out) == true);
  }

  h = capture_pipe_evaluate_health(&pipe, 2000u);
  assert(h == CAPTURE_HEALTHY);
  assert(pipe.telemetry.distressed_exit_count == 1u);
  assert(pipe.degraded_deadline_active == false);

  printf(" ok\n");
}

/* ── Test: hard failure and degraded deadline ──────────────────── */

static void test_hard_failure_deadline(void) {
  printf("  hard_failure_deadline...");

  capture_pipe_t pipe;
  capture_pipe_init(&pipe);

  capture_pipe_note_hard_failure(&pipe, 1000u);
  assert(pipe.has_seen_hard_failure == true);
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

/* ── Test: hard failure overrides distressed ───────────────────── */

static void test_hard_failure_sticky(void) {
  printf("  hard_failure_sticky...");

  capture_pipe_t pipe;
  capture_pipe_init(&pipe);

  capture_pipe_note_hard_failure(&pipe, 1000u);

  logger_writer_cmd_t out;
  while (capture_cmd_ring_dequeue(&pipe, &out)) {
    /* drain */
  }
  capture_health_t h = capture_pipe_evaluate_health(&pipe, 2000u);
  assert(h == CAPTURE_HARD_FAILURE);

  printf(" ok\n");
}

/* ── Test: clear degraded deadline ─────────────────────────────── */

static void test_clear_degraded_deadline(void) {
  printf("  clear_degraded_deadline...");

  capture_pipe_t pipe;
  capture_pipe_init(&pipe);

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
  capture_pipe_init(&pipe);

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
  assert(pipe.telemetry.event_push_overflow_count == 1u);

  printf(" ok\n");
}

/* ── Test: occupancy percentage ────────────────────────────────── */

static void test_occupancy_pct(void) {
  printf("  occupancy_pct...");

  capture_pipe_t pipe;
  capture_pipe_init(&pipe);

  assert(capture_cmd_ring_occupancy_pct(&pipe) == 0u);

  for (uint32_t i = 0; i < CAPTURE_CMD_RING_CAPACITY / 2u; i++) {
    logger_writer_cmd_t cmd = make_packet_cmd(i);
    capture_cmd_ring_enqueue(&pipe, &cmd);
  }
  assert(capture_cmd_ring_occupancy_pct(&pipe) == 50u);

  for (uint32_t i = CAPTURE_CMD_RING_CAPACITY / 2u;
       i < CAPTURE_CMD_RING_CAPACITY; i++) {
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
  capture_pipe_init(&pipe);

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
  capture_pipe_init(&pipe);

  for (uint32_t round = 0; round < 10; round++) {
    for (uint32_t i = 0; i < CAPTURE_CMD_RING_CAPACITY; i++) {
      logger_writer_cmd_t cmd = make_packet_cmd(round * 100 + i);
      assert(capture_cmd_ring_enqueue(&pipe, &cmd) == true);
    }
    for (uint32_t i = 0; i < CAPTURE_CMD_RING_CAPACITY; i++) {
      logger_writer_cmd_t out;
      assert(capture_cmd_ring_dequeue(&pipe, &out) == true);
      assert(out.append_pmd_packet.seq_in_span == round * 100 + i);
    }
  }

  assert(capture_cmd_ring_occupancy(&pipe) == 0u);
  assert(pipe.telemetry.cmd_enqueue_count == 10u * CAPTURE_CMD_RING_CAPACITY);
  assert(pipe.telemetry.cmd_dequeue_count == 10u * CAPTURE_CMD_RING_CAPACITY);

  printf(" ok\n");
}

/* ── Test: high-water marks ────────────────────────────────────── */

static void test_hwm(void) {
  printf("  hwm...");

  capture_pipe_t pipe;
  capture_pipe_init(&pipe);

  /* Fill ring to 32 entries */
  for (uint32_t i = 0; i < 32; i++) {
    logger_writer_cmd_t cmd = make_packet_cmd(i);
    assert(capture_cmd_ring_enqueue(&pipe, &cmd) == true);
  }
  assert(pipe.telemetry.cmd_occupancy_hwm == 32u);

  /* Drain 16 — HWM stays at 32 */
  for (uint32_t i = 0; i < 16; i++) {
    logger_writer_cmd_t out;
    capture_cmd_ring_dequeue(&pipe, &out);
  }
  assert(pipe.telemetry.cmd_occupancy_hwm == 32u);
  assert(pipe.telemetry.cmd_occupancy_now == 16u);

  printf(" ok\n");
}

/* ── Test: drain_and_execute ───────────────────────────────────── */

static void test_drain_and_execute(void) {
  printf("  drain_and_execute...");

  capture_pipe_t pipe;
  capture_pipe_init(&pipe);

  /* Push 4 packet commands and 1 barrier */
  for (uint32_t i = 0; i < 4; i++) {
    logger_writer_cmd_t cmd = make_packet_cmd(i);
    capture_cmd_ring_enqueue(&pipe, &cmd);
  }
  logger_writer_cmd_t barrier = make_barrier_cmd();
  capture_cmd_ring_enqueue(&pipe, &barrier);

  assert(capture_cmd_ring_occupancy(&pipe) == 5u);

  /* Execute all — barrier produces an event.  Pass a non-NULL ctx
   * (the dispatch stub ignores it). */
  uint32_t executed = capture_pipe_drain_and_execute(&pipe, (void *)1u, 10);
  assert(executed == 5u);
  assert(capture_cmd_ring_occupancy(&pipe) == 0u);

  /* One event for the barrier */
  assert(capture_event_ring_has_data(&pipe) == true);
  capture_event_t ev;
  assert(capture_event_ring_pop(&pipe, &ev) == true);
  assert(ev.kind == CAPTURE_EVENT_BARRIER_COMPLETE);
  assert(ev.success == true);

  printf(" ok\n");
}

/* ── Main ──────────────────────────────────────────────────────── */

int main(void) {
  printf("capture_pipe tests (inline phase, CAPTURE_STAGING_CAPACITY=%u):\n",
         CAPTURE_STAGING_CAPACITY);

  test_init();
  test_cmd_ring_basic();
  test_cmd_ring_full();
  test_staging_inline_passthrough();
  test_staging_inline_overflow();
  test_health_transitions();
  test_hard_failure_deadline();
  test_hard_failure_sticky();
  test_clear_degraded_deadline();
  test_event_ring();
  test_occupancy_pct();
  test_process_events();
  test_wrap_around();
  test_hwm();
  test_drain_and_execute();

  printf("  all passed\n");
  return 0;
}
