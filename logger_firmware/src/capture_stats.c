#include "logger/capture_stats.h"

#include <string.h>

void logger_capture_stats_init(logger_capture_stats_t *stats) {
  memset(stats, 0, sizeof(*stats));
}

void logger_capture_stats_reset(logger_capture_stats_t *stats) {
  if (stats == NULL) {
    return;
  }
  /* Preserve nothing; zero everything so the next measurement window
   * starts clean.  Same effect as init but named for intent. */
  memset(stats, 0, sizeof(*stats));
}

void logger_capture_stats_observe_queue_depth(logger_capture_stats_t *stats,
                                              uint8_t depth) {
  if (stats == NULL) {
    return;
  }
  if (depth > stats->queue_depth_hwm) {
    stats->queue_depth_hwm = depth;
  }
}

void logger_capture_stats_record_session_append(logger_capture_stats_t *stats,
                                                uint32_t elapsed_us, bool ok) {
  if (stats == NULL) {
    return;
  }
  stats->session_append_count += 1u;
  if (!ok) {
    stats->session_append_fail_count += 1u;
  }
  stats->session_append_last_us = elapsed_us;
  if (elapsed_us > stats->session_append_max_us) {
    stats->session_append_max_us = elapsed_us;
  }
}

void logger_capture_stats_record_journal_append(logger_capture_stats_t *stats,
                                                bool ok) {
  if (stats == NULL) {
    return;
  }
  stats->journal_record_count += 1u;
  if (!ok) {
    stats->journal_record_fail_count += 1u;
  }
}
