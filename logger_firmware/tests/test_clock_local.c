/*
 * Host-side tests for local civil-time derivation.
 *
 * Compile:
 *   gcc -Wall -Wextra -Werror \
 *       -I include -I boards/rp2_2 \
 *       tests/test_clock_local.c src/clock_local.c \
 *       -o test_clock_local
 *
 * Run:
 *   ./test_clock_local
 */

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "logger/clock.h"

#include "board_config.h"

static logger_clock_status_t utc_status(int year, int month, int day, int hour,
                                        int minute, int second) {
  logger_clock_status_t status;
  memset(&status, 0, sizeof(status));
  status.initialized = true;
  status.rtc_present = true;
  status.valid = true;
  status.year = year;
  status.month = month;
  status.day = day;
  status.hour = hour;
  status.minute = minute;
  status.second = second;
  return status;
}

static void assert_local(const char *label, logger_clock_status_t utc, int year,
                         int month, int day, int hour, int minute, int second) {
  logger_clock_datetime_t local;
  memset(&local, 0, sizeof(local));
  assert(logger_clock_observed_local_datetime(&utc, "Europe/Paris", &local));
  if (local.year != year || local.month != month || local.day != day ||
      local.hour != hour || local.minute != minute || local.second != second) {
    fprintf(stderr,
            "%s: got %04d-%02d-%02dT%02d:%02d:%02d, expected "
            "%04d-%02d-%02dT%02d:%02d:%02d\n",
            label, local.year, local.month, local.day, local.hour, local.minute,
            local.second, year, month, day, hour, minute, second);
    assert(false);
  }
}

static void assert_window(const char *label, logger_clock_status_t utc,
                          bool expected) {
  bool in_window = false;
  assert(logger_clock_observed_local_hour_in_window(
      &utc, "Europe/Paris", LOGGER_OVERNIGHT_UPLOAD_WINDOW_START_HOUR_LOCAL,
      LOGGER_OVERNIGHT_UPLOAD_WINDOW_END_HOUR_LOCAL, &in_window));
  if (in_window != expected) {
    fprintf(stderr, "%s: got in_window=%s, expected %s\n", label,
            in_window ? "true" : "false", expected ? "true" : "false");
    assert(false);
  }
}

static void assert_study_day(const char *label, logger_clock_status_t utc,
                             const char *expected) {
  char study_day[11];
  memset(study_day, 0, sizeof(study_day));
  assert(logger_clock_derive_study_day_local_observed(&utc, "Europe/Paris",
                                                      study_day));
  if (strcmp(study_day, expected) != 0) {
    fprintf(stderr, "%s: got study_day=%s, expected %s\n", label, study_day,
            expected);
    assert(false);
  }
}

static void test_europe_paris_offsets(void) {
  assert_local("winter CET offset", utc_status(2026, 1, 15, 12, 0, 0), 2026, 1,
               15, 13, 0, 0);
  assert_local("summer CEST offset", utc_status(2026, 7, 15, 12, 0, 0), 2026, 7,
               15, 14, 0, 0);
}

static void test_europe_paris_dst_transitions(void) {
  /* 2026-03-29 is the last Sunday of March.  Clocks jump at 01:00 UTC:
   * 01:59:59 CET is followed by 03:00:00 CEST.
   */
  assert_local("March DST before transition",
               utc_status(2026, 3, 29, 0, 59, 59), 2026, 3, 29, 1, 59, 59);
  assert_local("March DST at transition", utc_status(2026, 3, 29, 1, 0, 0),
               2026, 3, 29, 3, 0, 0);

  /* 2026-10-25 is the last Sunday of October.  Clocks fall back at 01:00 UTC:
   * 02:59:59 CEST is followed by 02:00:00 CET.
   */
  assert_local("October DST before transition",
               utc_status(2026, 10, 25, 0, 59, 59), 2026, 10, 25, 2, 59, 59);
  assert_local("October DST at transition", utc_status(2026, 10, 25, 1, 0, 0),
               2026, 10, 25, 2, 0, 0);
}

static void test_overnight_upload_window(void) {
  assert_window("before upload window", utc_status(2026, 1, 15, 20, 59, 0),
                false); /* 21:59 local */
  assert_window("upload window start inclusive",
                utc_status(2026, 1, 15, 21, 0, 0), true); /* 22:00 local */
  assert_window("upload window before end", utc_status(2026, 1, 16, 4, 59, 0),
                true); /* 05:59 local */
  assert_window("upload window end exclusive", utc_status(2026, 1, 16, 5, 0, 0),
                false); /* 06:00 local */
}

static void test_study_day_rollover(void) {
  assert_study_day("before 04:00 local rollover",
                   utc_status(2026, 1, 15, 2, 59, 59), "2026-01-14");
  assert_study_day("at 04:00 local rollover", utc_status(2026, 1, 15, 3, 0, 0),
                   "2026-01-15");
  assert_study_day("rollover across year boundary",
                   utc_status(2026, 1, 1, 2, 30, 0), "2025-12-31");
}

int main(void) {
  test_europe_paris_offsets();
  test_europe_paris_dst_transitions();
  test_overnight_upload_window();
  test_study_day_rollover();
  puts("test_clock_local: ok");
  return 0;
}