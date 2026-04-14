#include "logger/button.h"

#include "hardware/gpio.h"
#include "pico/stdlib.h"

#include "board_config.h"

#define LOGGER_BUTTON_DEBOUNCE_MS 30u
#define LOGGER_BUTTON_SHORT_MIN_MS 50u
#define LOGGER_BUTTON_SHORT_MAX_MS 700u
#define LOGGER_BUTTON_LONG_MIN_MS 2000u
#define LOGGER_BUTTON_FACTORY_RESET_MS 10000u

static bool g_button_gpio_initialized = false;

static void logger_button_gpio_init_once(void) {
  if (g_button_gpio_initialized) {
    return;
  }

  gpio_init(LOGGER_BUTTON_PIN);
  gpio_set_dir(LOGGER_BUTTON_PIN, GPIO_IN);
  gpio_pull_up(LOGGER_BUTTON_PIN);
  g_button_gpio_initialized = true;
}

bool logger_button_is_pressed_raw(void) {
  logger_button_gpio_init_once();
  const bool raw_level = gpio_get(LOGGER_BUTTON_PIN);
  return LOGGER_BUTTON_ACTIVE_LOW ? !raw_level : raw_level;
}

void logger_button_init(logger_button_t *button, uint32_t now_ms) {
  logger_button_gpio_init_once();
  button->debounced_pressed = logger_button_is_pressed_raw();
  button->raw_pressed = button->debounced_pressed;
  button->long_reported = false;
  button->raw_changed_mono_ms = now_ms;
  button->press_started_mono_ms = button->debounced_pressed ? now_ms : 0u;
}

logger_boot_gesture_t logger_button_detect_boot_gesture(uint32_t now_ms) {
  logger_button_gpio_init_once();

  if (!logger_button_is_pressed_raw()) {
    return LOGGER_BOOT_GESTURE_NONE;
  }

  for (;;) {
    sleep_ms(10);
    const uint32_t elapsed = to_ms_since_boot(get_absolute_time()) - now_ms;
    const bool still_pressed = logger_button_is_pressed_raw();

    if (!still_pressed) {
      if (elapsed >= LOGGER_BUTTON_LONG_MIN_MS) {
        return LOGGER_BOOT_GESTURE_SERVICE;
      }
      return LOGGER_BOOT_GESTURE_NONE;
    }

    if (elapsed >= LOGGER_BUTTON_FACTORY_RESET_MS) {
      return LOGGER_BOOT_GESTURE_FACTORY_RESET;
    }
  }
}

logger_button_event_t logger_button_poll(logger_button_t *button,
                                         uint32_t now_ms) {
  const bool raw_pressed = logger_button_is_pressed_raw();
  if (raw_pressed != button->raw_pressed) {
    button->raw_pressed = raw_pressed;
    button->raw_changed_mono_ms = now_ms;
  }

  if ((now_ms - button->raw_changed_mono_ms) < LOGGER_BUTTON_DEBOUNCE_MS) {
    if (button->debounced_pressed && !button->long_reported &&
        (now_ms - button->press_started_mono_ms) >= LOGGER_BUTTON_LONG_MIN_MS) {
      button->long_reported = true;
      return LOGGER_BUTTON_EVENT_LONG_PRESS;
    }
    return LOGGER_BUTTON_EVENT_NONE;
  }

  if (button->debounced_pressed != button->raw_pressed) {
    button->debounced_pressed = button->raw_pressed;
    if (button->debounced_pressed) {
      button->press_started_mono_ms = now_ms;
      button->long_reported = false;
      return LOGGER_BUTTON_EVENT_NONE;
    }

    const uint32_t duration_ms = now_ms - button->press_started_mono_ms;
    if (duration_ms >= LOGGER_BUTTON_SHORT_MIN_MS &&
        duration_ms < LOGGER_BUTTON_SHORT_MAX_MS) {
      return LOGGER_BUTTON_EVENT_SHORT_PRESS;
    }
    return LOGGER_BUTTON_EVENT_NONE;
  }

  if (button->debounced_pressed && !button->long_reported &&
      (now_ms - button->press_started_mono_ms) >= LOGGER_BUTTON_LONG_MIN_MS) {
    button->long_reported = true;
    return LOGGER_BUTTON_EVENT_LONG_PRESS;
  }

  return LOGGER_BUTTON_EVENT_NONE;
}
