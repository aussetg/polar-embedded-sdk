#include <stdio.h>

#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"

#include "logger/app_main.h"
#include "logger/button.h"

int main(void) {
  stdio_init_all();
  setvbuf(stdout, NULL, _IONBF, 0);
  setvbuf(stderr, NULL, _IONBF, 0);
  sleep_ms(200);

  const uint32_t boot_now_ms = to_ms_since_boot(get_absolute_time());
  const logger_boot_gesture_t boot_gesture =
      logger_button_detect_boot_gesture(boot_now_ms);

  printf("[logger] booting custom logger firmware\n");

  if (cyw43_arch_init()) {
    printf("[logger] fatal: cyw43_arch_init failed\n");
    while (true) {
      sleep_ms(1000);
    }
  }

  static logger_app_t app;
  logger_app_init(&app, to_ms_since_boot(get_absolute_time()), boot_gesture);

  while (true) {
    uint32_t now_ms = to_ms_since_boot(get_absolute_time());
    logger_app_step(&app, now_ms);
    cyw43_arch_poll();

    /*
     * Sleep until the earlier of:
     *   1. The state-appropriate deadline from logger_app_max_sleep_ms()
     *   2. The next CYW43 async event (BLE notification, WiFi callback)
     *
     * During LOG_STREAMING the effective sleep is ~10 ms (driven by BLE notify
     * events). During RECOVERY_HOLD or IDLE on battery the CPU sleeps for 1–5 s
     * between wake-ups, cutting CPU power from ~15 mA to < 1 mA average.
     */
    uint32_t after_step_ms = to_ms_since_boot(get_absolute_time());
    uint32_t sleep_for_ms = logger_app_max_sleep_ms(&app, after_step_ms);
    cyw43_arch_wait_for_work_until(make_timeout_time_ms(sleep_for_ms));
  }
}
