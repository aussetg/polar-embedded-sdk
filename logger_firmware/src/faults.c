#include "logger/faults.h"

#include <stddef.h>

const char *logger_fault_code_name(logger_fault_code_t code) {
    switch (code) {
        case LOGGER_FAULT_NONE:
            return NULL;
        case LOGGER_FAULT_CONFIG_INCOMPLETE:
            return "config_incomplete";
        case LOGGER_FAULT_CLOCK_INVALID:
            return "clock_invalid";
        case LOGGER_FAULT_LOW_BATTERY_BLOCKED_START:
            return "low_battery_blocked_start";
        case LOGGER_FAULT_CRITICAL_LOW_BATTERY_STOPPED:
            return "critical_low_battery_stopped";
        case LOGGER_FAULT_SD_MISSING_OR_UNWRITABLE:
            return "sd_missing_or_unwritable";
        case LOGGER_FAULT_SD_WRITE_FAILED:
            return "sd_write_failed";
        case LOGGER_FAULT_SD_LOW_SPACE_RESERVE_UNMET:
            return "sd_low_space_reserve_unmet";
        case LOGGER_FAULT_UPLOAD_BLOCKED_MIN_FIRMWARE:
            return "upload_blocked_min_firmware";
        default:
            return "unknown_fault";
    }
}

uint8_t logger_fault_blink_count(logger_fault_code_t code) {
    switch (code) {
        case LOGGER_FAULT_CONFIG_INCOMPLETE:
            return 1;
        case LOGGER_FAULT_CLOCK_INVALID:
            return 2;
        case LOGGER_FAULT_LOW_BATTERY_BLOCKED_START:
        case LOGGER_FAULT_CRITICAL_LOW_BATTERY_STOPPED:
            return 3;
        case LOGGER_FAULT_SD_MISSING_OR_UNWRITABLE:
        case LOGGER_FAULT_SD_WRITE_FAILED:
        case LOGGER_FAULT_SD_LOW_SPACE_RESERVE_UNMET:
            return 4;
        case LOGGER_FAULT_UPLOAD_BLOCKED_MIN_FIRMWARE:
            return 5;
        case LOGGER_FAULT_NONE:
        default:
            return 0;
    }
}
