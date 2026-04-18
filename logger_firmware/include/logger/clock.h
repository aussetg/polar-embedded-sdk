#ifndef LOGGER_FIRMWARE_CLOCK_H
#define LOGGER_FIRMWARE_CLOCK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LOGGER_CLOCK_RFC3339_UTC_LEN 30
#define LOGGER_CLOCK_NTP_SERVER_MAX 64
#define LOGGER_CLOCK_NTP_MESSAGE_MAX 160

typedef struct {
  int year;
  int month;
  int day;
  int weekday;
  int hour;
  int minute;
  int second;
} logger_clock_datetime_t;

typedef struct {
  bool initialized;
  bool rtc_present;
  bool valid;
  bool lost_power;
  bool battery_low;
  int year;
  int month;
  int day;
  int weekday;
  int hour;
  int minute;
  int second;
  char now_utc[LOGGER_CLOCK_RFC3339_UTC_LEN + 1];
} logger_clock_status_t;

typedef struct {
  bool attempted;
  bool applied;
  bool previous_valid;
  bool large_correction;
  uint8_t stratum;
  int64_t correction_seconds;
  char server[LOGGER_CLOCK_NTP_SERVER_MAX + 1];
  char remote_address[48];
  char previous_utc[LOGGER_CLOCK_RFC3339_UTC_LEN + 1];
  char applied_utc[LOGGER_CLOCK_RFC3339_UTC_LEN + 1];
  char message[LOGGER_CLOCK_NTP_MESSAGE_MAX + 1];
} logger_clock_ntp_sync_result_t;

void logger_clock_init(void);
void logger_clock_sample(logger_clock_status_t *status);
bool logger_clock_set_utc(const char *rfc3339_utc,
                          logger_clock_status_t *status_out);
void logger_clock_ntp_sync_result_init(logger_clock_ntp_sync_result_t *result);
bool logger_clock_ntp_sync(const logger_clock_status_t *current_status,
                           logger_clock_ntp_sync_result_t *result,
                           logger_clock_status_t *status_out);

const char *logger_clock_state_name(const logger_clock_status_t *status);
bool logger_clock_observed_utc_ns(const logger_clock_status_t *status,
                                  int64_t *utc_ns_out);
bool logger_clock_format_utc_ns_rfc3339(
    int64_t utc_ns, char out_rfc3339[LOGGER_CLOCK_RFC3339_UTC_LEN + 1]);
bool logger_clock_derive_study_day_local_observed(
    const logger_clock_status_t *status, const char *timezone,
    char out_study_day[11]);

static inline const char *
logger_clock_now_utc_or_null(const logger_clock_status_t *clock) {
  return clock->now_utc[0] != '\0' ? clock->now_utc : NULL;
}

#endif
