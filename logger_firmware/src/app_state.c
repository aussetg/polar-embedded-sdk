#include "logger/app_state.h"

#include <stdio.h>
#include <string.h>

const char *logger_mode_name(const logger_app_state_t *state) {
  if (state->current_state == LOGGER_RUNTIME_SERVICE) {
    return "service";
  }
  if (state->current_state == LOGGER_RUNTIME_LOG_STOPPING) {
    return logger_runtime_state_is_logging(state->planned_next_state)
               ? "logging"
               : "upload";
  }
  if (logger_runtime_state_is_logging(state->current_state)) {
    return "logging";
  }
  if (logger_runtime_state_is_upload(state->current_state)) {
    return "upload";
  }
  if (state->current_state == LOGGER_RUNTIME_IDLE_WAITING_FOR_CHARGER) {
    return "idle_waiting_for_charger";
  }
  if (state->current_state == LOGGER_RUNTIME_IDLE_UPLOAD_COMPLETE) {
    return "idle_upload_complete";
  }
  return "service";
}

void logger_app_state_init(logger_app_state_t *state, uint32_t now_ms) {
  memset(state, 0, sizeof(*state));
  state->current_state = LOGGER_RUNTIME_BOOT;
  state->planned_next_state = LOGGER_RUNTIME_BOOT;
  state->boot_mono_ms = now_ms;
  state->entered_state_mono_ms = now_ms;
}

void logger_app_state_transition(logger_app_state_t *state,
                                 logger_runtime_state_t next_state,
                                 const char *reason, uint32_t now_ms) {
  const char *old_name = logger_runtime_state_name(state->current_state);
  const char *new_name = logger_runtime_state_name(next_state);
  const char *why = reason != NULL ? reason : "unspecified";

  printf("[logger] state %s -> %s reason=%s t=%lu ms\n", old_name, new_name,
         why, (unsigned long)now_ms);

  state->current_state = next_state;
  state->planned_next_state = next_state;
  state->entered_state_mono_ms = now_ms;
}
