#ifndef LOGGER_FIRMWARE_SERVICE_CLI_H
#define LOGGER_FIRMWARE_SERVICE_CLI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "logger/busy_poll.h"

typedef struct logger_app logger_app_t;

#define LOGGER_SERVICE_CLI_LINE_MAX 2048u
#define LOGGER_SERVICE_CLI_CONFIG_IMPORT_JSON_MAX 8192u

typedef struct {
  char line_buf[LOGGER_SERVICE_CLI_LINE_MAX];
  size_t line_len;
  bool unlocked;
  uint32_t unlock_deadline_mono_ms;
  bool config_import_active;
  size_t config_import_expected_len;
  size_t config_import_received_len;
  uint32_t config_import_chunk_count;
  char config_import_buf[LOGGER_SERVICE_CLI_CONFIG_IMPORT_JSON_MAX + 1u];
} logger_service_cli_t;

void logger_service_cli_init(logger_service_cli_t *cli);
void logger_service_cli_abort_mutable_session(logger_service_cli_t *cli);
void logger_service_cli_poll(logger_service_cli_t *cli, logger_app_t *app,
                             uint32_t now_ms);
void logger_service_cli_poll_upload_busy(logger_service_cli_t *cli,
                                         logger_app_t *app, uint32_t now_ms,
                                         logger_busy_poll_phase_t phase);

#endif
