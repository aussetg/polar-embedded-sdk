#ifndef LOGGER_UTIL_H
#define LOGGER_UTIL_H

/*
 * Small shared helpers used across logger firmware translation units.
 * All are static inline — the compiler folds them away at -O1 and above.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Copy src into dst with truncation and guaranteed NUL termination.
 * If src is NULL, dst is set to "".
 */
static inline void logger_copy_string(char *dst, size_t dst_len,
                                      const char *src) {
  if (dst_len == 0u) {
    return;
  }
  if (src == NULL) {
    dst[0] = '\0';
    return;
  }
  size_t i = 0u;
  while (src[i] != '\0' && (i + 1u) < dst_len) {
    dst[i] = src[i];
    ++i;
  }
  dst[i] = '\0';
}

/*
 * Like logger_copy_string but falls back to `fallback` when src is NULL or "".
 * If fallback is also NULL, dst is set to "".
 */
static inline void logger_copy_string_fallback(char *dst, size_t dst_len,
                                               const char *src,
                                               const char *fallback) {
  if (src == NULL || src[0] == '\0') {
    src = fallback;
  }
  if (dst_len == 0u) {
    return;
  }
  size_t i = 0u;
  while (src != NULL && src[i] != '\0' && (i + 1u) < dst_len) {
    dst[i] = src[i];
    ++i;
  }
  dst[i] = '\0';
}

/*
 * IEEE 802.3 CRC-32 (polynomial 0xEDB88320, bit-reversed).
 */
static inline uint32_t logger_crc32_ieee(const uint8_t *data, size_t len) {
  uint32_t crc = 0xffffffffu;
  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) {
      const uint32_t mask = (uint32_t)(-(int32_t)(crc & 1u));
      crc = (crc >> 1) ^ (0xedb88320u & mask);
    }
  }
  return crc ^ 0xffffffffu;
}

/*
 * True when value is non-NULL and not the empty string.
 */
static inline bool logger_string_present(const char *value) {
  return value != NULL && value[0] != '\0';
}

#endif /* LOGGER_UTIL_H */
