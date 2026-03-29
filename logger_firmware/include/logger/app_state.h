#ifndef LOGGER_FIRMWARE_APP_STATE_H
#define LOGGER_FIRMWARE_APP_STATE_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    LOGGER_RUNTIME_BOOT = 0,
    LOGGER_RUNTIME_SERVICE,
    LOGGER_RUNTIME_LOG_WAIT_H10,
    LOGGER_RUNTIME_LOG_CONNECTING,
    LOGGER_RUNTIME_LOG_SECURING,
    LOGGER_RUNTIME_LOG_STARTING_STREAM,
    LOGGER_RUNTIME_LOG_STREAMING,
    LOGGER_RUNTIME_LOG_STOPPING,
    LOGGER_RUNTIME_UPLOAD_PREP,
    LOGGER_RUNTIME_UPLOAD_RUNNING,
    LOGGER_RUNTIME_IDLE_WAITING_FOR_CHARGER,
    LOGGER_RUNTIME_IDLE_UPLOAD_COMPLETE,
} logger_runtime_state_t;

typedef struct {
    logger_runtime_state_t current_state;
    logger_runtime_state_t planned_next_state;
    uint32_t boot_mono_ms;
    uint32_t entered_state_mono_ms;
    uint32_t step_counter;
    bool provisioning_complete;
    bool wall_clock_valid;
    bool charger_present;
} logger_app_state_t;

const char *logger_runtime_state_name(logger_runtime_state_t state);
const char *logger_mode_name(const logger_app_state_t *state);

bool logger_runtime_state_is_logging(logger_runtime_state_t state);
bool logger_runtime_state_is_upload(logger_runtime_state_t state);
bool logger_runtime_state_is_idle(logger_runtime_state_t state);

void logger_app_state_init(logger_app_state_t *state, uint32_t now_ms);
void logger_app_state_transition(
    logger_app_state_t *state,
    logger_runtime_state_t next_state,
    const char *reason,
    uint32_t now_ms);

#endif
