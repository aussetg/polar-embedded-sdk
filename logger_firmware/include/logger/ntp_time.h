// Pure NTP timestamp helpers.
//
// NTP's on-wire seconds field is the low 32 bits of a 64-bit era-relative
// counter starting at 1900-01-01.  The low word rolls over in 2036.  Keep era
// unfolding in one testable place so SNTP sync keeps working through the
// rollover without pulling in a full time library.

#ifndef LOGGER_FIRMWARE_NTP_TIME_H
#define LOGGER_FIRMWARE_NTP_TIME_H

#include <stdbool.h>
#include <stdint.h>

#include "logger/civil_date.h"

#define LOGGER_NTP_UNIX_EPOCH_OFFSET_SECONDS 2208988800ll
#define LOGGER_NTP_ERA_SECONDS 4294967296ll

static inline int64_t logger_ntp_supported_min_unix_seconds(void) {
  return logger_days_from_civil(2024, 1, 1) * 86400ll;
}

static inline int64_t logger_ntp_supported_max_unix_seconds_exclusive(void) {
  return logger_days_from_civil(2100, 1, 1) * 86400ll;
}

static inline uint64_t logger_ntp_abs_i64_diff(int64_t a, int64_t b) {
  return a >= b ? (uint64_t)(a - b) : (uint64_t)(b - a);
}

static inline bool logger_ntp_transmit_seconds_to_unix(
    uint32_t transmit_seconds, bool have_reference_unix_seconds,
    int64_t reference_unix_seconds, int64_t *unix_seconds_out) {
  if (unix_seconds_out == NULL) {
    return false;
  }

  const int64_t min_unix = logger_ntp_supported_min_unix_seconds();
  const int64_t max_unix = logger_ntp_supported_max_unix_seconds_exclusive();

  bool found = false;
  int64_t best_unix = 0ll;
  uint64_t best_distance = UINT64_MAX;

  /* The firmware supports UTC years 2024..2099.  That span can only land in
   * NTP era 0 (until 2036-02-07) or era 1 (afterwards). */
  for (int era = 0; era <= 1; ++era) {
    const int64_t ntp_full_seconds =
        (int64_t)transmit_seconds + ((int64_t)era * LOGGER_NTP_ERA_SECONDS);
    const int64_t unix_seconds =
        ntp_full_seconds - LOGGER_NTP_UNIX_EPOCH_OFFSET_SECONDS;
    if (unix_seconds < min_unix || unix_seconds >= max_unix) {
      continue;
    }

    const uint64_t distance =
        have_reference_unix_seconds
            ? logger_ntp_abs_i64_diff(unix_seconds, reference_unix_seconds)
            : 0u;
    if (!found || distance < best_distance) {
      found = true;
      best_unix = unix_seconds;
      best_distance = distance;
    }
  }

  if (!found) {
    return false;
  }
  *unix_seconds_out = best_unix;
  return true;
}

#endif
