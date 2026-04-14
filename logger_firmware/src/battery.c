#include "logger/battery.h"

#include "hardware/adc.h"
#include "pico/cyw43_arch.h"

#include "board_config.h"

static bool g_battery_initialized = false;

static int logger_clamp_int(int value, int lo, int hi) {
  if (value < lo) {
    return lo;
  }
  if (value > hi) {
    return hi;
  }
  return value;
}

void logger_battery_init(void) {
  if (g_battery_initialized) {
    return;
  }

  adc_init();
  adc_gpio_init(LOGGER_BATTERY_SENSE_PIN);
  g_battery_initialized = true;
}

void logger_battery_sample(logger_battery_status_t *status) {
  if (!g_battery_initialized) {
    logger_battery_init();
  }

  const uint input = (uint)(LOGGER_BATTERY_SENSE_PIN - ADC_BASE_PIN);
  adc_select_input(input);

  uint32_t accum = 0;
  for (int i = 0; i < 8; ++i) {
    accum += adc_read();
  }
  const uint16_t raw = (uint16_t)(accum / 8u);

  // Pimoroni-style battery divider on RP2 logger boards is approximately 1/3.
  const uint32_t mv = (raw * 3300u * 3u) / 4095u;
  const int pct = (int)(((int32_t)mv - 3300) * 100 / (4200 - 3300));

  status->initialized = true;
  status->raw_adc = raw;
  status->voltage_mv = (uint16_t)mv;
  status->estimate_pct = logger_clamp_int(pct, 0, 100);
#ifdef CYW43_WL_GPIO_VBUS_PIN
  status->vbus_present = cyw43_arch_gpio_get(CYW43_WL_GPIO_VBUS_PIN);
#else
  status->vbus_present = false;
#endif
}

bool logger_battery_low_start_blocked(const logger_battery_status_t *status) {
  return !status->vbus_present &&
         status->voltage_mv < LOGGER_BATTERY_LOW_START_BLOCK_MV;
}

bool logger_battery_off_charger_upload_allowed(
    const logger_battery_status_t *status) {
  return status->voltage_mv >= LOGGER_BATTERY_OFF_CHARGER_UPLOAD_MIN_MV;
}

bool logger_battery_is_critical(const logger_battery_status_t *status) {
  return status->voltage_mv <= LOGGER_BATTERY_CRITICAL_STOP_MV;
}
