#include "logger/clock.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "hardware/gpio.h"
#include "hardware/i2c.h"

#include "board_config.h"

#define LOGGER_PCF8523_REG_CONTROL1 0x00
#define LOGGER_PCF8523_REG_CONTROL3 0x02
#define LOGGER_PCF8523_REG_SECONDS 0x03
#define LOGGER_PCF8523_REG_CONTROL1_STOP 0x20
#define LOGGER_PCF8523_REG_CONTROL3_PM_MASK 0xe0
#define LOGGER_PCF8523_REG_CONTROL3_BLF 0x04
#define LOGGER_PCF8523_SECONDS_OS 0x80

static bool g_clock_initialized = false;

static int logger_bcd_decode(uint8_t value) {
    return ((value >> 4) * 10) + (value & 0x0f);
}

static uint8_t logger_bcd_encode(int value) {
    return (uint8_t)(((value / 10) << 4) | (value % 10));
}

static bool logger_is_leap_year(int year) {
    if ((year % 400) == 0) {
        return true;
    }
    if ((year % 100) == 0) {
        return false;
    }
    return (year % 4) == 0;
}

static int logger_days_in_month(int year, int month) {
    static const int days[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (month == 2 && logger_is_leap_year(year)) {
        return 29;
    }
    if (month < 1 || month > 12) {
        return 31;
    }
    return days[month - 1];
}

static bool logger_clock_datetime_reasonable_parts(
    int year,
    int month,
    int day,
    int hour,
    int minute,
    int second) {
    if (year < 2024 || year > 2099) {
        return false;
    }
    if (month < 1 || month > 12) {
        return false;
    }
    if (day < 1 || day > logger_days_in_month(year, month)) {
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

static bool logger_clock_datetime_reasonable(const logger_clock_status_t *status) {
    return logger_clock_datetime_reasonable_parts(
        status->year,
        status->month,
        status->day,
        status->hour,
        status->minute,
        status->second);
}

static bool logger_timezone_is_utc_like(const char *timezone) {
    if (timezone == NULL) {
        return false;
    }
    return strcmp(timezone, "UTC") == 0 || strcmp(timezone, "Etc/UTC") == 0;
}

static bool logger_timezone_present(const char *timezone) {
    return timezone != NULL && timezone[0] != '\0';
}

static int logger_weekday_from_date(int year, int month, int day) {
    static const int offsets[] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    if (month < 3) {
        year -= 1;
    }
    int sunday_based = (year + year / 4 - year / 100 + year / 400 + offsets[month - 1] + day) % 7;
    return (sunday_based + 6) % 7;
}

static int64_t logger_days_from_civil(int year, int month, int day) {
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yoe = (unsigned)(year - era * 400);
    const unsigned doy = (153u * (unsigned)(month + (month > 2 ? -3 : 9)) + 2u) / 5u + (unsigned)day - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return (int64_t)(era * 146097 + (int)doe - 719468);
}

static bool logger_clock_status_to_observed_utc_ns(const logger_clock_status_t *status, int64_t *utc_ns_out) {
    if (!logger_clock_datetime_reasonable(status)) {
        return false;
    }
    const int64_t days = logger_days_from_civil(status->year, status->month, status->day);
    const int64_t seconds = days * 86400ll +
        ((int64_t)status->hour * 3600ll) +
        ((int64_t)status->minute * 60ll) +
        (int64_t)status->second;
    *utc_ns_out = seconds * 1000000000ll;
    return true;
}

static bool logger_clock_read_reg(uint8_t reg, uint8_t *value) {
    if (i2c_write_blocking(i2c0, LOGGER_RTC_I2C_ADDR, &reg, 1, true) != 1) {
        return false;
    }
    return i2c_read_blocking(i2c0, LOGGER_RTC_I2C_ADDR, value, 1, false) == 1;
}

static bool logger_clock_write_reg(uint8_t reg, uint8_t value) {
    uint8_t buf[2] = { reg, value };
    return i2c_write_blocking(i2c0, LOGGER_RTC_I2C_ADDR, buf, sizeof(buf), false) == (int)sizeof(buf);
}

static bool logger_clock_update_reg(uint8_t reg, uint8_t mask, uint8_t value) {
    uint8_t current = 0u;
    if (!logger_clock_read_reg(reg, &current)) {
        return false;
    }
    current = (uint8_t)((current & ~mask) | (value & mask));
    return logger_clock_write_reg(reg, current);
}

static void logger_clock_format_now(logger_clock_status_t *status) {
    snprintf(status->now_utc, sizeof(status->now_utc),
             "%04d-%02d-%02dT%02d:%02d:%02dZ",
             status->year,
             status->month,
             status->day,
             status->hour,
             status->minute,
             status->second);
}

void logger_clock_init(void) {
    if (g_clock_initialized) {
        return;
    }

    i2c_init(i2c0, 400000u);
    gpio_set_function(LOGGER_RTC_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(LOGGER_RTC_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(LOGGER_RTC_SDA_PIN);
    gpio_pull_up(LOGGER_RTC_SCL_PIN);
    g_clock_initialized = true;
}

void logger_clock_sample(logger_clock_status_t *status) {
    memset(status, 0, sizeof(*status));
    logger_clock_init();
    status->initialized = true;

    uint8_t reg = LOGGER_PCF8523_REG_SECONDS;
    uint8_t data[7];
    int rc = i2c_write_blocking(i2c0, LOGGER_RTC_I2C_ADDR, &reg, 1, true);
    if (rc != 1) {
        status->rtc_present = false;
        return;
    }
    rc = i2c_read_blocking(i2c0, LOGGER_RTC_I2C_ADDR, data, sizeof(data), false);
    if (rc != (int)sizeof(data)) {
        status->rtc_present = false;
        return;
    }

    uint8_t control3 = 0xffu;
    if (logger_clock_read_reg(LOGGER_PCF8523_REG_CONTROL3, &control3)) {
        status->battery_low = (control3 & LOGGER_PCF8523_REG_CONTROL3_BLF) != 0;
    }

    status->rtc_present = true;
    status->lost_power = (data[0] & LOGGER_PCF8523_SECONDS_OS) != 0;
    status->second = logger_bcd_decode((uint8_t)(data[0] & 0x7f));
    status->minute = logger_bcd_decode((uint8_t)(data[1] & 0x7f));
    status->hour = logger_bcd_decode((uint8_t)(data[2] & 0x3f));
    status->day = logger_bcd_decode((uint8_t)(data[3] & 0x3f));
    status->weekday = data[4] & 0x07;
    status->month = logger_bcd_decode((uint8_t)(data[5] & 0x1f));
    status->year = 2000 + logger_bcd_decode(data[6]);

    status->valid = !status->lost_power && logger_clock_datetime_reasonable(status);
    if (logger_clock_datetime_reasonable(status)) {
        logger_clock_format_now(status);
    }
}

bool logger_clock_set_utc(const char *rfc3339_utc, logger_clock_status_t *status_out) {
    if (rfc3339_utc == NULL || strlen(rfc3339_utc) != 20u) {
        return false;
    }

    logger_clock_datetime_t dt;
    memset(&dt, 0, sizeof(dt));
    if (sscanf(rfc3339_utc,
               "%4d-%2d-%2dT%2d:%2d:%2dZ",
               &dt.year,
               &dt.month,
               &dt.day,
               &dt.hour,
               &dt.minute,
               &dt.second) != 6) {
        return false;
    }
    if (!logger_clock_datetime_reasonable_parts(dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second)) {
        return false;
    }

    logger_clock_init();
    dt.weekday = logger_weekday_from_date(dt.year, dt.month, dt.day);

    uint8_t buf[8];
    buf[0] = LOGGER_PCF8523_REG_SECONDS;
    buf[1] = logger_bcd_encode(dt.second) & 0x7fu;
    buf[2] = logger_bcd_encode(dt.minute) & 0x7fu;
    buf[3] = logger_bcd_encode(dt.hour) & 0x3fu;
    buf[4] = logger_bcd_encode(dt.day) & 0x3fu;
    buf[5] = (uint8_t)(dt.weekday & 0x07);
    buf[6] = logger_bcd_encode(dt.month) & 0x1fu;
    buf[7] = logger_bcd_encode(dt.year - 2000);

    if (!logger_clock_update_reg(LOGGER_PCF8523_REG_CONTROL3, LOGGER_PCF8523_REG_CONTROL3_PM_MASK, 0u)) {
        return false;
    }
    if (i2c_write_blocking(i2c0, LOGGER_RTC_I2C_ADDR, buf, sizeof(buf), false) != (int)sizeof(buf)) {
        return false;
    }
    if (!logger_clock_update_reg(LOGGER_PCF8523_REG_CONTROL1, LOGGER_PCF8523_REG_CONTROL1_STOP, 0u)) {
        return false;
    }

    if (status_out != NULL) {
        logger_clock_sample(status_out);
    }
    return true;
}

const char *logger_clock_state_name(const logger_clock_status_t *status) {
    return status->valid ? "valid" : "invalid";
}

bool logger_clock_observed_utc_ns(const logger_clock_status_t *status, int64_t *utc_ns_out) {
    if (status == NULL || utc_ns_out == NULL) {
        return false;
    }
    return logger_clock_status_to_observed_utc_ns(status, utc_ns_out);
}

static bool logger_clock_derive_study_day_from_fields(
    int year,
    int month,
    int day,
    int hour,
    char out_study_day[11]) {
    if (!logger_clock_datetime_reasonable_parts(year, month, day, hour, 0, 0)) {
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
            day = logger_days_in_month(year, month);
        }
    }

    snprintf(out_study_day, 11, "%04d-%02d-%02d", year, month, day);
    return true;
}

bool logger_clock_derive_study_day_local(
    const logger_clock_status_t *status,
    const char *timezone,
    char out_study_day[11]) {
    if (!status->valid || !logger_timezone_is_utc_like(timezone)) {
        return false;
    }
    return logger_clock_derive_study_day_from_fields(
        status->year,
        status->month,
        status->day,
        status->hour,
        out_study_day);
}

bool logger_clock_derive_study_day_local_observed(
    const logger_clock_status_t *status,
    const char *timezone,
    char out_study_day[11]) {
    if (status == NULL || out_study_day == NULL) {
        return false;
    }
    if (!logger_timezone_is_utc_like(timezone) && !logger_timezone_present(timezone)) {
        return false;
    }
    if (!logger_clock_datetime_reasonable(status)) {
        return false;
    }
    return logger_clock_derive_study_day_from_fields(
        status->year,
        status->month,
        status->day,
        status->hour,
        out_study_day);
}
