#include "logger/app_state.h"

#include <stdio.h>
#include <string.h>

const char *logger_runtime_state_name(logger_runtime_state_t state) {
    switch (state) {
        case LOGGER_RUNTIME_BOOT:
            return "boot";
        case LOGGER_RUNTIME_SERVICE:
            return "service";
        case LOGGER_RUNTIME_LOG_WAIT_H10:
            return "log_wait_h10";
        case LOGGER_RUNTIME_LOG_CONNECTING:
            return "log_connecting";
        case LOGGER_RUNTIME_LOG_SECURING:
            return "log_securing";
        case LOGGER_RUNTIME_LOG_STARTING_STREAM:
            return "log_starting_stream";
        case LOGGER_RUNTIME_LOG_STREAMING:
            return "log_streaming";
        case LOGGER_RUNTIME_LOG_STOPPING:
            return "log_stopping";
        case LOGGER_RUNTIME_UPLOAD_PREP:
            return "upload_prep";
        case LOGGER_RUNTIME_UPLOAD_RUNNING:
            return "upload_running";
        case LOGGER_RUNTIME_IDLE_WAITING_FOR_CHARGER:
            return "idle_waiting_for_charger";
        case LOGGER_RUNTIME_IDLE_UPLOAD_COMPLETE:
            return "idle_upload_complete";
        default:
            return "unknown";
    }
}

bool logger_runtime_state_is_logging(logger_runtime_state_t state) {
    switch (state) {
        case LOGGER_RUNTIME_LOG_WAIT_H10:
        case LOGGER_RUNTIME_LOG_CONNECTING:
        case LOGGER_RUNTIME_LOG_SECURING:
        case LOGGER_RUNTIME_LOG_STARTING_STREAM:
        case LOGGER_RUNTIME_LOG_STREAMING:
        case LOGGER_RUNTIME_LOG_STOPPING:
            return true;
        default:
            return false;
    }
}

bool logger_runtime_state_is_upload(logger_runtime_state_t state) {
    return state == LOGGER_RUNTIME_UPLOAD_PREP || state == LOGGER_RUNTIME_UPLOAD_RUNNING;
}

bool logger_runtime_state_is_idle(logger_runtime_state_t state) {
    return state == LOGGER_RUNTIME_IDLE_WAITING_FOR_CHARGER ||
           state == LOGGER_RUNTIME_IDLE_UPLOAD_COMPLETE;
}

const char *logger_mode_name(const logger_app_state_t *state) {
    if (state->current_state == LOGGER_RUNTIME_SERVICE) {
        return "service";
    }
    if (state->current_state == LOGGER_RUNTIME_LOG_STOPPING) {
        return logger_runtime_state_is_logging(state->planned_next_state) ? "logging" : "upload";
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

void logger_app_state_transition(
    logger_app_state_t *state,
    logger_runtime_state_t next_state,
    const char *reason,
    uint32_t now_ms) {
    const char *old_name = logger_runtime_state_name(state->current_state);
    const char *new_name = logger_runtime_state_name(next_state);
    const char *why = reason != NULL ? reason : "unspecified";

    printf("[logger] state %s -> %s reason=%s t=%lu ms\n",
           old_name,
           new_name,
           why,
           (unsigned long)now_ms);

    state->current_state = next_state;
    state->planned_next_state = next_state;
    state->entered_state_mono_ms = now_ms;
}
