#ifndef LOGGER_FIRMWARE_BATTERY_H
#define LOGGER_FIRMWARE_BATTERY_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
  bool initialized;
  uint16_t raw_adc;
  uint16_t voltage_mv;
  int estimate_pct;
  bool vbus_present;
} logger_battery_status_t;

void logger_battery_init(void);
void logger_battery_sample(logger_battery_status_t *status);

bool logger_battery_low_start_blocked(const logger_battery_status_t *status);
bool logger_battery_off_charger_upload_allowed(
    const logger_battery_status_t *status);
bool logger_battery_is_critical(const logger_battery_status_t *status);

#endif
