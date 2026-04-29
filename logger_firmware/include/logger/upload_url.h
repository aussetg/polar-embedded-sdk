// SPDX-License-Identifier: MIT
#ifndef LOGGER_FIRMWARE_UPLOAD_URL_H
#define LOGGER_FIRMWARE_UPLOAD_URL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LOGGER_UPLOAD_URL_HOST_MAX 128
#define LOGGER_UPLOAD_URL_PATH_MAX 192

typedef struct {
  bool https;
  char host[LOGGER_UPLOAD_URL_HOST_MAX + 1u];
  char path[LOGGER_UPLOAD_URL_PATH_MAX + 1u];
  uint16_t port;
  bool host_bracketed_literal;
} logger_upload_url_parts_t;

bool logger_upload_url_parse(const char *url, logger_upload_url_parts_t *out);

#endif