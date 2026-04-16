#ifndef LOGGER_FIRMWARE_CAPTURE_STATS_H
#define LOGGER_FIRMWARE_CAPTURE_STATS_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Lightweight capture/storage pipeline instrumentation.
 *
 * All counters are unsigned. All latencies are in microseconds.
 * No heap allocation. Safe to leave compiled in for service/debug builds.
 * Overhead per update: one absolute_time read + a few integer ops.
 */

typedef struct {
  /* H10 ingress ring */
  uint8_t queue_depth_hwm; /* high-water mark of packet_count */

  /* Session-level append (session_append_pmd_packet) */
  uint32_t session_append_count;
  uint32_t session_append_fail_count;
  uint32_t session_append_max_us;
  uint32_t session_append_last_us;

  /* Storage-level append (storage_append_file) */
  uint32_t storage_append_count;
  uint32_t storage_append_fail_count;
  uint32_t storage_append_max_us;
  uint32_t storage_append_last_us;

  /* FatFS f_sync inside storage_append_file */
  uint32_t sync_count;
  uint32_t sync_max_us;
  uint32_t sync_last_us;

  /* Journal record emission (journal_append_*_record) */
  uint32_t journal_record_count;
  uint32_t journal_record_fail_count;
} logger_capture_stats_t;

void logger_capture_stats_init(logger_capture_stats_t *stats);
void logger_capture_stats_reset(logger_capture_stats_t *stats);

/* Observe current queue depth; tracks HWM internally. */
void logger_capture_stats_observe_queue_depth(logger_capture_stats_t *stats,
                                              uint8_t depth);

/* Record outcome of one session-level PMD append. */
void logger_capture_stats_record_session_append(logger_capture_stats_t *stats,
                                                uint32_t elapsed_us, bool ok);

/* Record outcome of one storage-level file append. */
void logger_capture_stats_record_storage_append(logger_capture_stats_t *stats,
                                                uint32_t elapsed_us, bool ok);

/* Record one f_sync latency observation. */
void logger_capture_stats_record_sync(logger_capture_stats_t *stats,
                                      uint32_t elapsed_us);

/* Record one journal record emission. */
void logger_capture_stats_record_journal_append(logger_capture_stats_t *stats,
                                                bool ok);

#endif
