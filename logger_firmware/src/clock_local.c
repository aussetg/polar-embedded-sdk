#include "logger/civil_date.h"
#include "logger/clock.h"
#include "logger/util.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "board_config.h"

static void logger_clock_local_write_2_digits(char *dst, unsigned value) {
  dst[0] = (char)('0' + ((value / 10u) % 10u));
  dst[1] = (char)('0' + (value % 10u));
}

static void logger_clock_local_write_4_digits(char *dst, unsigned value) {
  dst[0] = (char)('0' + ((value / 1000u) % 10u));
  dst[1] = (char)('0' + ((value / 100u) % 10u));
  dst[2] = (char)('0' + ((value / 10u) % 10u));
  dst[3] = (char)('0' + (value % 10u));
}

static void logger_clock_local_format_date(char out_study_day[11],
                                           unsigned year, unsigned month,
                                           unsigned day) {
  logger_clock_local_write_4_digits(out_study_day, year);
  out_study_day[4] = '-';
  logger_clock_local_write_2_digits(out_study_day + 5, month);
  out_study_day[7] = '-';
  logger_clock_local_write_2_digits(out_study_day + 8, day);
  out_study_day[10] = '\0';
}

static bool logger_clock_local_timezone_present(const char *timezone) {
  return timezone != NULL && timezone[0] != '\0';
}

static bool logger_clock_local_timezone_is_europe_paris(const char *timezone) {
  return timezone != NULL && strcmp(timezone, "Europe/Paris") == 0;
}

static bool logger_clock_local_is_leap_year(int year) {
  if ((year % 400) == 0) {
    return true;
  }
  if ((year % 100) == 0) {
    return false;
  }
  return (year % 4) == 0;
}

static int logger_clock_local_days_in_month(int year, int month) {
  static const int days[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month == 2 && logger_clock_local_is_leap_year(year)) {
    return 29;
  }
  if (month < 1 || month > 12) {
    return 31;
  }
  return days[month - 1];
}

static bool logger_clock_local_datetime_reasonable_parts(int year, int month,
                                                         int day, int hour,
                                                         int minute,
                                                         int second) {
  if (year < 2024 || year > 2099) {
    return false;
  }
  if (month < 1 || month > 12) {
    return false;
  }
  if (day < 1 || day > logger_clock_local_days_in_month(year, month)) {
    return false;
  }
  if (hour < 0 || hour > 23) {
    return false;
  }
  if (minute < 0 || minute > 59) {
    return false;
  }
  if (second < 0 || second > 59) {
    return false;
  }
  return true;
}

static bool
logger_clock_local_datetime_reasonable(const logger_clock_status_t *status) {
  return logger_clock_local_datetime_reasonable_parts(
      status->year, status->month, status->day, status->hour, status->minute,
      status->second);
}

static int logger_clock_local_weekday_from_date(int year, int month, int day) {
  static const int offsets[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  if (month < 3) {
    year -= 1;
  }
  int sunday_based =
      (year + year / 4 - year / 100 + year / 400 + offsets[month - 1] + day) %
      7;
  return (sunday_based + 6) % 7;
}

static bool
logger_clock_local_status_to_utc_seconds(const logger_clock_status_t *status,
                                         int64_t *utc_seconds_out) {
  if (utc_seconds_out == NULL ||
      !logger_clock_local_datetime_reasonable(status)) {
    return false;
  }
  const int64_t days =
      logger_days_from_civil(status->year, status->month, status->day);
  *utc_seconds_out = days * 86400ll + ((int64_t)status->hour * 3600ll) +
                     ((int64_t)status->minute * 60ll) + (int64_t)status->second;
  return true;
}

static int logger_clock_local_last_sunday_of_month(int year, int month) {
  const int last_day = logger_clock_local_days_in_month(year, month);
  const int weekday =
      logger_clock_local_weekday_from_date(year, month, last_day);
  /* weekday: Monday=0 ... Sunday=6.  Distance back to Sunday is 1, 2, ...,
   * 6, 0 respectively. */
  return last_day - ((weekday + 1) % 7);
}

static bool logger_clock_local_europe_paris_dst_active_utc(
    const logger_clock_status_t *status) {
  const int month = status->month;
  if (month < 3 || month > 10) {
    return false;
  }
  if (month > 3 && month < 10) {
    return true;
  }

  const int transition_day =
      logger_clock_local_last_sunday_of_month(status->year, month);
  if (month == 3) {
    if (status->day != transition_day) {
      return status->day > transition_day;
    }
    return status->hour >= 1;
  }

  /* October: CEST ends at 01:00 UTC on the last Sunday. */
  if (status->day != transition_day) {
    return status->day < transition_day;
  }
  return status->hour < 1;
}

static bool logger_clock_local_timezone_utc_offset_seconds(
    const logger_clock_status_t *utc_status, const char *timezone,
    int32_t *offset_seconds_out) {
  if (utc_status == NULL || offset_seconds_out == NULL ||
      !logger_clock_local_datetime_reasonable(utc_status)) {
    return false;
  }
  if (logger_timezone_is_utc_like(timezone)) {
    *offset_seconds_out = 0;
    return true;
  }
  if (logger_clock_local_timezone_is_europe_paris(timezone)) {
    *offset_seconds_out =
        logger_clock_local_europe_paris_dst_active_utc(utc_status) ? 7200
                                                                   : 3600;
    return true;
  }
  return false;
}

static bool
logger_clock_local_datetime_from_unix_seconds(int64_t seconds,
                                              logger_clock_datetime_t *dt_out) {
  if (dt_out == NULL) {
    return false;
  }

  int64_t days = seconds / 86400ll;
  int64_t sod = seconds % 86400ll;
  if (sod < 0) {
    sod += 86400ll;
    days -= 1;
  }

  int year = 0;
  unsigned month = 0;
  unsigned day = 0;
  logger_civil_from_days(days, &year, &month, &day);

  memset(dt_out, 0, sizeof(*dt_out));
  dt_out->year = year;
  dt_out->month = (int)month;
  dt_out->day = (int)day;
  dt_out->hour = (int)(sod / 3600ll);
  dt_out->minute = (int)((sod % 3600ll) / 60ll);
  dt_out->second = (int)(sod % 60ll);
  dt_out->weekday = logger_clock_local_weekday_from_date(
      dt_out->year, dt_out->month, dt_out->day);
  return logger_clock_local_datetime_reasonable_parts(
      dt_out->year, dt_out->month, dt_out->day, dt_out->hour, dt_out->minute,
      dt_out->second);
}

bool logger_clock_observed_local_datetime(const logger_clock_status_t *status,
                                          const char *timezone,
                                          logger_clock_datetime_t *local_out) {
  if (status == NULL || local_out == NULL ||
      !logger_clock_local_timezone_present(timezone)) {
    return false;
  }

  int64_t utc_seconds = 0;
  int32_t offset_seconds = 0;
  if (!logger_clock_local_status_to_utc_seconds(status, &utc_seconds) ||
      !logger_clock_local_timezone_utc_offset_seconds(status, timezone,
                                                      &offset_seconds)) {
    return false;
  }
  return logger_clock_local_datetime_from_unix_seconds(
      utc_seconds + (int64_t)offset_seconds, local_out);
}

bool logger_clock_observed_local_hour_in_window(
    const logger_clock_status_t *status, const char *timezone, int start_hour,
    int end_hour, bool *in_window_out) {
  if (in_window_out == NULL || start_hour < 0 || start_hour > 23 ||
      end_hour < 0 || end_hour > 23) {
    return false;
  }

  logger_clock_datetime_t local;
  if (!logger_clock_observed_local_datetime(status, timezone, &local)) {
    return false;
  }

  if (start_hour == end_hour) {
    *in_window_out = true;
  } else if (start_hour < end_hour) {
    *in_window_out = local.hour >= start_hour && local.hour < end_hour;
  } else {
    *in_window_out = local.hour >= start_hour || local.hour < end_hour;
  }
  return true;
}

static bool logger_clock_local_derive_study_day_from_fields(
    int year, int month, int day, int hour, char out_study_day[11]) {
  if (!logger_clock_local_datetime_reasonable_parts(year, month, day, hour, 0,
                                                    0)) {
    return false;
  }

  if (hour < LOGGER_STUDY_DAY_ROLLOVER_HOUR_LOCAL) {
    day -= 1;
    if (day < 1) {
      month -= 1;
      if (month < 1) {
        month = 12;
        year -= 1;
      }
      day = logger_clock_local_days_in_month(year, month);
    }
  }

  logger_clock_local_format_date(out_study_day, (unsigned)year, (unsigned)month,
                                 (unsigned)day);
  return true;
}

bool logger_clock_derive_study_day_local_observed(
    const logger_clock_status_t *status, const char *timezone,
    char out_study_day[11]) {
  if (status == NULL || out_study_day == NULL) {
    return false;
  }
  if (!logger_clock_local_timezone_present(timezone)) {
    return false;
  }
  logger_clock_datetime_t local;
  if (!logger_clock_observed_local_datetime(status, timezone, &local)) {
    return false;
  }
  return logger_clock_local_derive_study_day_from_fields(
      local.year, local.month, local.day, local.hour, out_study_day);
}