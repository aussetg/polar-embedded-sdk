// SPDX-License-Identifier: MIT
#ifndef LOGGER_FIRMWARE_CONFIG_VALIDATE_H
#define LOGGER_FIRMWARE_CONFIG_VALIDATE_H

#include <stdbool.h>
#include <stddef.h>

#include "logger/config_store.h"

bool logger_config_logger_id_valid(const char *value, bool allow_empty);
bool logger_config_subject_id_valid(const char *value, bool allow_empty);
bool logger_config_upload_url_valid(const char *value, bool allow_empty);
bool logger_config_upload_api_key_valid(const char *value, bool allow_empty);
bool logger_config_upload_token_valid(const char *value, bool allow_empty);

bool logger_config_upload_request_material_valid(const logger_config_t *config,
                                                 char *bad_field,
                                                 size_t bad_field_len);

#endif