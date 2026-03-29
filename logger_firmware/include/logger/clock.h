#ifndef LOGGER_FIRMWARE_CLOCK_H
#define LOGGER_FIRMWARE_CLOCK_H

#include <stdbool.h>

#define LOGGER_CLOCK_RFC3339_UTC_LEN 30

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

void logger_clock_init(void);
void logger_clock_sample(logger_clock_status_t *status);

const char *logger_clock_state_name(const logger_clock_status_t *status);
bool logger_clock_derive_study_day_local(
    const logger_clock_status_t *status,
    const char *timezone,
    char out_study_day[11]);

#endif
