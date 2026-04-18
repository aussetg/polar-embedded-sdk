#ifndef LOGGER_FIRMWARE_JSON_STREAM_WRITER_H
#define LOGGER_FIRMWARE_JSON_STREAM_WRITER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/*
 * Streaming JSON writer for CLI responses.
 *
 * Writes JSON directly to a FILE* (stdout on the serial CLI).
 * A single `needs_comma` flag tracks whether the next value needs a
 * comma separator — no nesting stack is required because:
 *
 *   - `{` and `[` reset the flag  (new scope, first item needs no comma)
 *   - `}` and `]` set the flag    (closed value = parent scope saw a value)
 *   - scalar values set the flag  (just wrote a value)
 *
 * Usage:
 *   logger_json_stream_writer_t w;
 *   logger_json_stream_writer_init(&w, stdout);
 *
 *   logger_json_stream_writer_object_begin(&w);
 *   logger_json_stream_writer_field_uint32(&w, "schema_version", 1);
 *   logger_json_stream_writer_field_string_or_null(&w, "command", cmd);
 *   logger_json_stream_writer_field_bool(&w, "ok", true);
 *   logger_json_stream_writer_field_object_begin(&w, "payload");
 *     // ... payload fields ...
 *   logger_json_stream_writer_object_end(&w);   // close payload
 *   logger_json_stream_writer_object_end(&w);   // close envelope
 *   fputc('\n', w.stream);
 *   fflush(w.stream);
 */

typedef struct {
  FILE *stream;
  bool needs_comma;
} logger_json_stream_writer_t;

void logger_json_stream_writer_init(logger_json_stream_writer_t *w,
                                    FILE *stream);

/* Structure */
void logger_json_stream_writer_object_begin(
    logger_json_stream_writer_t *w); /* writes { */
void logger_json_stream_writer_object_end(
    logger_json_stream_writer_t *w); /* writes } */
void logger_json_stream_writer_array_end(
    logger_json_stream_writer_t *w); /* writes ] */

/* Object fields: "key":value */
void logger_json_stream_writer_field_string_or_null(
    logger_json_stream_writer_t *w, const char *key, const char *value);
void logger_json_stream_writer_field_bool(logger_json_stream_writer_t *w,
                                          const char *key, bool value);
void logger_json_stream_writer_field_uint32(logger_json_stream_writer_t *w,
                                            const char *key, uint32_t value);
void logger_json_stream_writer_field_uint64(logger_json_stream_writer_t *w,
                                            const char *key, uint64_t value);
void logger_json_stream_writer_field_int32(logger_json_stream_writer_t *w,
                                           const char *key, int32_t value);
void logger_json_stream_writer_field_int64(logger_json_stream_writer_t *w,
                                           const char *key, int64_t value);
void logger_json_stream_writer_field_null(logger_json_stream_writer_t *w,
                                          const char *key);
/* Write "key":<raw_json> — raw_json must be valid JSON */
void logger_json_stream_writer_field_raw(logger_json_stream_writer_t *w,
                                         const char *key, const char *raw_json);

/* Nested structures as field value: "key":{ or "key":[ */
void logger_json_stream_writer_field_object_begin(
    logger_json_stream_writer_t *w, const char *key);
void logger_json_stream_writer_field_array_begin(logger_json_stream_writer_t *w,
                                                 const char *key);

/* Array elements */
void logger_json_stream_writer_elem_string_or_null(
    logger_json_stream_writer_t *w, const char *value);
void logger_json_stream_writer_elem_object_begin(
    logger_json_stream_writer_t *w); /* writes { with comma */

#endif
