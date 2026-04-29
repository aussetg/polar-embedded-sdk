// SPDX-License-Identifier: MIT
#include "logger/upload_url.h"

#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "lwip/ip_addr.h"

#include "logger/config_store.h"

static bool logger_url_len_within(const char *value, size_t max_storage_len,
                                  size_t *len_out) {
  if (value == NULL || max_storage_len == 0u || len_out == NULL) {
    return false;
  }
  for (size_t i = 0u; i < max_storage_len; ++i) {
    if (value[i] == '\0') {
      *len_out = i;
      return true;
    }
  }
  return false;
}

static bool logger_url_byte_allowed(char ch) {
  const unsigned char c = (unsigned char)ch;
  return c > 0x20u && c < 0x7fu && c != '#';
}

static bool logger_url_all_bytes_allowed(const char *value, size_t len) {
  for (size_t i = 0u; i < len; ++i) {
    if (!logger_url_byte_allowed(value[i])) {
      return false;
    }
  }
  return true;
}

static bool logger_url_has_valid_percent_escapes(const char *value,
                                                 size_t len) {
  for (size_t i = 0u; i < len; ++i) {
    if (value[i] != '%') {
      continue;
    }
    if ((i + 2u) >= len || !isxdigit((unsigned char)value[i + 1u]) ||
        !isxdigit((unsigned char)value[i + 2u])) {
      return false;
    }
    i += 2u;
  }
  return true;
}

static bool logger_url_copy_part(char *dst, size_t dst_len, const char *start,
                                 const char *end) {
  if (dst == NULL || dst_len == 0u || start == NULL || end < start) {
    return false;
  }
  const size_t len = (size_t)(end - start);
  if (len >= dst_len) {
    return false;
  }
  memcpy(dst, start, len);
  dst[len] = '\0';
  return true;
}

static bool logger_url_host_char_allowed(char ch) {
  return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
         (ch >= '0' && ch <= '9') || ch == '-';
}

static bool logger_url_dns_host_valid(const char *start, const char *end) {
  if (start == NULL || end <= start ||
      (size_t)(end - start) > LOGGER_UPLOAD_URL_HOST_MAX) {
    return false;
  }

  size_t label_len = 0u;
  char prev = '\0';
  for (const char *p = start; p < end; ++p) {
    const char ch = *p;
    if (ch == '.') {
      if (label_len == 0u || prev == '-') {
        return false;
      }
      label_len = 0u;
      prev = ch;
      continue;
    }
    if (!logger_url_host_char_allowed(ch)) {
      return false;
    }
    if (label_len == 0u && ch == '-') {
      return false;
    }
    label_len += 1u;
    if (label_len > 63u) {
      return false;
    }
    prev = ch;
  }

  return label_len > 0u && prev != '-';
}

static bool logger_url_ip_literal_valid(const char *start, const char *end) {
  if (start == NULL || end <= start ||
      (size_t)(end - start) > LOGGER_UPLOAD_URL_HOST_MAX) {
    return false;
  }

  bool have_colon = false;
  for (const char *p = start; p < end; ++p) {
    const char ch = *p;
    if (ch == ':') {
      have_colon = true;
      continue;
    }
    if (ch == '.' || isxdigit((unsigned char)ch)) {
      continue;
    }
    return false;
  }
  if (!have_colon) {
    return false;
  }

  char literal[LOGGER_UPLOAD_URL_HOST_MAX + 1u];
  if (!logger_url_copy_part(literal, sizeof(literal), start, end)) {
    return false;
  }
  ip_addr_t addr;
  return ipaddr_aton(literal, &addr) != 0;
}

static bool logger_url_parse_port(const char *start, const char *end,
                                  uint16_t *port_out) {
  if (start == NULL || end <= start || port_out == NULL) {
    return false;
  }

  unsigned long port = 0u;
  for (const char *p = start; p < end; ++p) {
    if (!isdigit((unsigned char)*p)) {
      return false;
    }
    port = (port * 10u) + (unsigned long)(*p - '0');
    if (port > 65535u) {
      return false;
    }
  }
  if (port == 0u) {
    return false;
  }
  *port_out = (uint16_t)port;
  return true;
}

bool logger_upload_url_parse(const char *url, logger_upload_url_parts_t *out) {
  if (out == NULL) {
    return false;
  }
  memset(out, 0, sizeof(*out));

  size_t url_len = 0u;
  if (!logger_url_len_within(url, LOGGER_CONFIG_UPLOAD_URL_MAX, &url_len) ||
      url_len == 0u || !logger_url_all_bytes_allowed(url, url_len) ||
      !logger_url_has_valid_percent_escapes(url, url_len)) {
    return false;
  }

  const char *after_scheme = NULL;
  if (strncmp(url, "http://", 7u) == 0) {
    out->https = false;
    out->port = 80u;
    after_scheme = url + 7u;
  } else if (strncmp(url, "https://", 8u) == 0) {
    out->https = true;
    out->port = 443u;
    after_scheme = url + 8u;
  } else {
    return false;
  }

  const char *const url_end = url + url_len;
  const char *const path_start =
      memchr(after_scheme, '/', (size_t)(url_end - after_scheme));
  const char *const authority_end = path_start != NULL ? path_start : url_end;
  if (authority_end == after_scheme ||
      memchr(after_scheme, '@', (size_t)(authority_end - after_scheme)) !=
          NULL) {
    return false;
  }

  const char *host_start = after_scheme;
  const char *host_end = authority_end;
  const char *port_start = NULL;
  bool bracketed_literal = false;

  if (*host_start == '[') {
    const char *const close =
        memchr(host_start, ']', (size_t)(authority_end - host_start));
    if (close == NULL || close == (host_start + 1)) {
      return false;
    }
    bracketed_literal = true;
    host_start += 1;
    host_end = close;
    if ((close + 1) < authority_end) {
      if (*(close + 1) != ':') {
        return false;
      }
      port_start = close + 2;
    }
    if (!logger_url_ip_literal_valid(host_start, host_end)) {
      return false;
    }
  } else {
    const char *const colon =
        memchr(host_start, ':', (size_t)(authority_end - host_start));
    if (colon != NULL) {
      host_end = colon;
      port_start = colon + 1;
    }
    if (!logger_url_dns_host_valid(host_start, host_end)) {
      return false;
    }
  }

  if (port_start != NULL &&
      !logger_url_parse_port(port_start, authority_end, &out->port)) {
    return false;
  }

  if (!logger_url_copy_part(out->host, sizeof(out->host), host_start,
                            host_end)) {
    return false;
  }
  out->host_bracketed_literal = bracketed_literal;

  if (path_start == NULL) {
    out->path[0] = '/';
    out->path[1] = '\0';
    return true;
  }

  if (!logger_url_copy_part(out->path, sizeof(out->path), path_start,
                            url_end)) {
    return false;
  }
  return true;
}