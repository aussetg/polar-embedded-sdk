#ifndef LOGGER_FIRMWARE_FLASH_SAFETY_H
#define LOGGER_FIRMWARE_FLASH_SAFETY_H

/*
 * Project flash-safe-execute policy.
 *
 * The Pico SDK lets applications provide a strong get_flash_safety_helper()
 * when the default policy is too blunt.  Our boot has two real phases:
 *
 *   1. core 0 only: core 1 is still in the reset/bootrom holding state, so
 *      a flash erase/program only needs core-0 interrupts disabled.
 *   2. multicore: once core 1 is launched, all flash erase/program operations
 *      must use pico_multicore lockout.
 *
 * Call this exactly before launching core 1 so any flash write attempted during
 * the launch window is rejected until core 1 has installed its lockout victim.
 */
void logger_flash_safety_note_core1_launching(void);

#endif
