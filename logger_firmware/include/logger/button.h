#ifndef LOGGER_FIRMWARE_BUTTON_H
#define LOGGER_FIRMWARE_BUTTON_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
  LOGGER_BOOT_GESTURE_NONE = 0,
  LOGGER_BOOT_GESTURE_SERVICE,
  LOGGER_BOOT_GESTURE_FACTORY_RESET,
} logger_boot_gesture_t;

typedef enum {
  LOGGER_BUTTON_EVENT_NONE = 0,
  LOGGER_BUTTON_EVENT_SHORT_PRESS,
  LOGGER_BUTTON_EVENT_LONG_PRESS,
} logger_button_event_t;

typedef struct {
  bool debounced_pressed;
  bool raw_pressed;
  bool long_reported;
  uint32_t raw_changed_mono_ms;
  uint32_t press_started_mono_ms;
} logger_button_t;

void logger_button_init(logger_button_t *button, uint32_t now_ms);
logger_button_event_t logger_button_poll(logger_button_t *button,
                                         uint32_t now_ms);
logger_boot_gesture_t logger_button_detect_boot_gesture(uint32_t now_ms);
bool logger_button_is_pressed_raw(void);

#endif
