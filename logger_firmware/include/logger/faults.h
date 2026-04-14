#ifndef LOGGER_FIRMWARE_FAULTS_H
#define LOGGER_FIRMWARE_FAULTS_H

#include "logger/storage.h"
#include <stdint.h>

typedef enum {
  LOGGER_FAULT_NONE = 0,
  LOGGER_FAULT_CONFIG_INCOMPLETE,
  LOGGER_FAULT_CLOCK_INVALID,
  LOGGER_FAULT_LOW_BATTERY_BLOCKED_START,
  LOGGER_FAULT_CRITICAL_LOW_BATTERY_STOPPED,
  LOGGER_FAULT_SD_MISSING_OR_UNWRITABLE,
  LOGGER_FAULT_SD_WRITE_FAILED,
  LOGGER_FAULT_SD_LOW_SPACE_RESERVE_UNMET,
  LOGGER_FAULT_UPLOAD_BLOCKED_MIN_FIRMWARE,
} logger_fault_code_t;

const char *logger_fault_code_name(logger_fault_code_t code);
uint8_t logger_fault_blink_count(logger_fault_code_t code);
logger_fault_code_t
logger_fault_from_storage(const logger_storage_status_t *storage);

#endif
