#include "logger/flash_safety.h"

#include <stdbool.h>
#include <stdint.h>

#include "hardware/sync.h"
#include "pico/error.h"
#include "pico/flash.h"
#include "pico/multicore.h"
#include "pico/platform.h"

#include "logger/ipc_atomic.h"

/*
 * Strong Pico SDK flash-safety helper.
 *
 * Why this exists:
 *   The SDK default cannot distinguish "pico_multicore is linked" from
 *   "core 1 is actually running".  Before multicore_launch_core1(), RP2350 core
 *   1 is dormant and cannot execute/read XIP flash, so disabling core-0 IRQs is
 *   sufficient and safe.  After launch, we require SDK multicore lockout.
 *
 * This keeps early config/BTstack flash writes legal without defining
 * PICO_FLASH_ASSUME_CORE1_SAFE, which would be unsafe after the storage worker
 * starts executing on core 1.
 */

static logger_ipc_bool_t g_core1_may_execute;
static uint32_t g_irq_state[2];
static bool g_lockout_taken[2];

void logger_flash_safety_note_core1_launching(void) {
  logger_ipc_bool_store_release(&g_core1_may_execute, true);
}

static bool logger_flash_safety_core1_may_execute(void) {
  return logger_ipc_bool_load_acquire(&g_core1_may_execute);
}

static bool logger_flash_safety_core_init_deinit(bool init) {
  if (!init) {
    return false;
  }

  multicore_lockout_victim_init();
  return true;
}

static bool logger_flash_safety_irq_only_is_safe(unsigned int core_num) {
  /* The only IRQ-only phase is early boot on core 0 before core 1 launch. */
  return core_num == 0u && !logger_flash_safety_core1_may_execute();
}

static int logger_flash_safety_enter(uint32_t timeout_ms) {
  const unsigned int core_num = get_core_num();
  const unsigned int slot = core_num & 1u;
  const bool irq_only = logger_flash_safety_irq_only_is_safe(core_num);

  g_lockout_taken[slot] = false;

  if (!irq_only) {
    const unsigned int other_core = core_num ^ 1u;
    if (!multicore_lockout_victim_is_initialized(other_core)) {
      return PICO_ERROR_NOT_PERMITTED;
    }
    if (!multicore_lockout_start_timeout_us((uint64_t)timeout_ms * 1000ull)) {
      return PICO_ERROR_TIMEOUT;
    }
    g_lockout_taken[slot] = true;
  }

  g_irq_state[slot] = save_and_disable_interrupts();
  return PICO_OK;
}

static int logger_flash_safety_exit(uint32_t timeout_ms) {
  const unsigned int slot = get_core_num() & 1u;

  restore_interrupts_from_disabled(g_irq_state[slot]);

  if (!g_lockout_taken[slot]) {
    return PICO_OK;
  }

  g_lockout_taken[slot] = false;
  return multicore_lockout_end_timeout_us((uint64_t)timeout_ms * 1000ull)
             ? PICO_OK
             : PICO_ERROR_TIMEOUT;
}

static flash_safety_helper_t g_logger_flash_safety_helper = {
    .core_init_deinit = logger_flash_safety_core_init_deinit,
    .enter_safe_zone_timeout_ms = logger_flash_safety_enter,
    .exit_safe_zone_timeout_ms = logger_flash_safety_exit,
};

flash_safety_helper_t *get_flash_safety_helper(void) {
  return &g_logger_flash_safety_helper;
}
