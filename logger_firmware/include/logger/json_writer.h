#ifndef LOGGER_FIRMWARE_JSON_WRITER_H
#define LOGGER_FIRMWARE_JSON_WRITER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
  char *buf;
  size_t cap;
  size_t len;
  bool ok;
  bool first;
  bool closed;
} logger_json_object_writer_t;

void logger_json_object_writer_init(logger_json_object_writer_t *writer,
                                    char *buf, size_t cap);

bool logger_json_object_writer_string_field(logger_json_object_writer_t *writer,
                                            const char *key, const char *value);

bool logger_json_object_writer_string_or_null_field(
    logger_json_object_writer_t *writer, const char *key, const char *value);

bool logger_json_object_writer_bool_field(logger_json_object_writer_t *writer,
                                          const char *key, bool value);

bool logger_json_object_writer_uint32_field(logger_json_object_writer_t *writer,
                                            const char *key, uint32_t value);

bool logger_json_object_writer_size_field(logger_json_object_writer_t *writer,
                                          const char *key, size_t value);

bool logger_json_object_writer_int64_field(logger_json_object_writer_t *writer,
                                           const char *key, int64_t value);

bool logger_json_object_writer_finish(logger_json_object_writer_t *writer);
const char *
logger_json_object_writer_data(const logger_json_object_writer_t *writer);

#endif