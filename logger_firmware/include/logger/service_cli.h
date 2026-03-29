#ifndef LOGGER_FIRMWARE_SERVICE_CLI_H
#define LOGGER_FIRMWARE_SERVICE_CLI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct logger_app logger_app_t;

typedef struct {
    char line_buf[512];
    size_t line_len;
    bool unlocked;
    uint32_t unlock_deadline_mono_ms;
} logger_service_cli_t;

void logger_service_cli_init(logger_service_cli_t *cli);
void logger_service_cli_poll(logger_service_cli_t *cli, logger_app_t *app, uint32_t now_ms);
bool logger_service_cli_is_unlocked(const logger_service_cli_t *cli, uint32_t now_ms);

#endif
