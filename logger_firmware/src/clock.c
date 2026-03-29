#include "logger/clock.h"

#include <stdio.h>
#include <string.h>

#include "hardware/gpio.h"
#include "hardware/i2c.h"

#include "board_config.h"

#define LOGGER_PCF8523_REG_CONTROL3 0x02
#define LOGGER_PCF8523_REG_SECONDS 0x03
#define LOGGER_PCF8523_REG_CONTROL3_BLF 0x04
#define LOGGER_PCF8523_SECONDS_OS 0x80

static bool g_clock_initialized = false;

static int logger_bcd_decode(uint8_t value) {
    return ((value >> 4) * 10) + (value & 0x0f);
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

static bool logger_clock_datetime_reasonable(const logger_clock_status_t *status) {
    if (status->year < 2024 || status->year > 2099) {
        return false;
    }
    if (status->month < 1 || status->month > 12) {
        return false;
    }
    if (status->day < 1 || status->day > logger_days_in_month(status->year, status->month)) {
        return false;
    }
    if (status->hour < 0 || status->hour > 23) {
        return false;
    }
    if (status->minute < 0 || status->minute > 59) {
        return false;
    }
    if (status->second < 0 || status->second > 59) {
        return false;
    }
    return true;
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

    uint8_t control3_reg = LOGGER_PCF8523_REG_CONTROL3;
    uint8_t control3 = 0xff;
    if (i2c_write_blocking(i2c0, LOGGER_RTC_I2C_ADDR, &control3_reg, 1, true) == 1 &&
        i2c_read_blocking(i2c0, LOGGER_RTC_I2C_ADDR, &control3, 1, false) == 1) {
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
    if (status->valid) {
        snprintf(status->now_utc, sizeof(status->now_utc),
                 "%04d-%02d-%02dT%02d:%02d:%02dZ",
                 status->year,
                 status->month,
                 status->day,
                 status->hour,
                 status->minute,
                 status->second);
    }
}

const char *logger_clock_state_name(const logger_clock_status_t *status) {
    return status->valid ? "valid" : "invalid";
}

static bool logger_timezone_is_utc_like(const char *timezone) {
    if (timezone == NULL) {
        return false;
    }
    return strcmp(timezone, "UTC") == 0 || strcmp(timezone, "Etc/UTC") == 0;
}

bool logger_clock_derive_study_day_local(
    const logger_clock_status_t *status,
    const char *timezone,
    char out_study_day[11]) {
    if (!status->valid || !logger_timezone_is_utc_like(timezone)) {
        return false;
    }

    int year = status->year;
    int month = status->month;
    int day = status->day;
    if (status->hour < 4) {
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
