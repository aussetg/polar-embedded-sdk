#include <stdio.h>

#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"

#include "logger/app_main.h"
#include "logger/button.h"
#include "logger/storage_worker.h"

/* Shared state for the core-1 storage worker.  Static so it survives
 * the lifetime of the boot.  Core 0 sets this up before launch;
 * core 1 reads it from the entry function via the FIFO mailbox. */
static storage_worker_shared_t g_storage_worker_shared;

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

  /*
   * Launch core 1 storage worker.
   *
   * This is a BOOT-time, one-shot operation.  The worker runs for
   * the lifetime of this boot.  It is not torn down or relaunched
   * during mode transitions.
   *
   * Sequence:
   *   1. Wire the capture pipe and session context into shared state
   *   2. Launch core 1 (blocks until core 1 is lockout-ready)
   *   3. Initialize flash-safe lockout on core 0
   *
   * After this returns, both cores are flash-safe initialized and
   * core 1 is the exclusive SD/FatFS owner.
   */
  app.storage_worker_shared = &g_storage_worker_shared;
  logger_storage_worker_init(&g_storage_worker_shared, &app.capture_pipe,
                             (logger_session_context_t *)&app.session);
  if (!logger_storage_worker_launch(&g_storage_worker_shared)) {
    printf("[logger] fatal: storage worker launch failed\n");
    while (true) {
      sleep_ms(1000);
    }
  }

  printf("[logger] core 1 storage worker alive, entering main loop\n");

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
