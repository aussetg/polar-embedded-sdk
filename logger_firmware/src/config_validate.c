// SPDX-License-Identifier: MIT
#include "logger/config_validate.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "logger/upload_url.h"
#include "logger/util.h"

static bool logger_config_len_within(const char *value, size_t storage_len,
                                     size_t *len_out) {
  if (value == NULL || storage_len == 0u || len_out == NULL) {
    return false;
  }
  for (size_t i = 0u; i < storage_len; ++i) {
    if (value[i] == '\0') {
      *len_out = i;
      return true;
    }
  }
  return false;
}

static bool logger_config_ascii_token_char_allowed(char ch) {
  return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
         (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' || ch == '-';
}

static bool logger_config_ascii_token_valid(const char *value,
                                            size_t storage_len,
                                            bool allow_empty) {
  size_t len = 0u;
  if (!logger_config_len_within(value, storage_len, &len)) {
    return false;
  }
  if (len == 0u) {
    return allow_empty;
  }
  for (size_t i = 0u; i < len; ++i) {
    if (!logger_config_ascii_token_char_allowed(value[i])) {
      return false;
    }
  }
  return true;
}

static bool logger_config_visible_ascii_valid(const char *value,
                                              size_t storage_len,
                                              bool allow_empty) {
  size_t len = 0u;
  if (!logger_config_len_within(value, storage_len, &len)) {
    return false;
  }
  if (len == 0u) {
    return allow_empty;
  }
  for (size_t i = 0u; i < len; ++i) {
    const unsigned char c = (unsigned char)value[i];
    if (c < 0x21u || c > 0x7eu) {
      return false;
    }
  }
  return true;
}

static void logger_config_copy_bad_field(char *dst, size_t dst_len,
                                         const char *field) {
  if (dst == NULL || dst_len == 0u) {
    return;
  }
  logger_copy_string(dst, dst_len, field != NULL ? field : "config");
}

bool logger_config_logger_id_valid(const char *value, bool allow_empty) {
  return logger_config_ascii_token_valid(value, LOGGER_CONFIG_LOGGER_ID_MAX,
                                         allow_empty);
}

bool logger_config_subject_id_valid(const char *value, bool allow_empty) {
  return logger_config_ascii_token_valid(value, LOGGER_CONFIG_SUBJECT_ID_MAX,
                                         allow_empty);
}

bool logger_config_upload_url_valid(const char *value, bool allow_empty) {
  size_t len = 0u;
  if (!logger_config_len_within(value, LOGGER_CONFIG_UPLOAD_URL_MAX, &len)) {
    return false;
  }
  if (len == 0u) {
    return allow_empty;
  }

  logger_upload_url_parts_t parts;
  return logger_upload_url_parse(value, &parts);
}

bool logger_config_upload_api_key_valid(const char *value, bool allow_empty) {
  return logger_config_visible_ascii_valid(
      value, LOGGER_CONFIG_UPLOAD_API_KEY_MAX, allow_empty);
}

bool logger_config_upload_token_valid(const char *value, bool allow_empty) {
  return logger_config_visible_ascii_valid(
      value, LOGGER_CONFIG_UPLOAD_TOKEN_MAX, allow_empty);
}

bool logger_config_upload_request_material_valid(const logger_config_t *config,
                                                 char *bad_field,
                                                 size_t bad_field_len) {
  if (config == NULL) {
    logger_config_copy_bad_field(bad_field, bad_field_len, "config");
    return false;
  }
  if (!logger_config_upload_url_valid(config->upload_url, false)) {
    logger_config_copy_bad_field(bad_field, bad_field_len, "upload_url");
    return false;
  }
  if (!logger_config_logger_id_valid(config->logger_id, false)) {
    logger_config_copy_bad_field(bad_field, bad_field_len, "logger_id");
    return false;
  }
  if (!logger_config_upload_api_key_valid(config->upload_api_key, false)) {
    logger_config_copy_bad_field(bad_field, bad_field_len, "upload_api_key");
    return false;
  }
  if (!logger_config_upload_token_valid(config->upload_token, false)) {
    logger_config_copy_bad_field(bad_field, bad_field_len, "upload_token");
    return false;
  }
  logger_config_copy_bad_field(bad_field, bad_field_len, "");
  return true;
}