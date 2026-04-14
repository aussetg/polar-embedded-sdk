#include "logger/json_stream_writer.h"
#include "logger/json.h"

#include <stdio.h>
#include <string.h>

/*
 * Write a comma separator if the previous token was a value.
 * Called before every new field, element, or raw value.
 */
static void ljsw_comma(logger_json_stream_writer_t *w) {
  if (w->needs_comma) {
    fputc(',', w->stream);
  }
}

/*
 * Write "key": — assumes key is ASCII / JSON-safe (always true here:
 * every key is a C string literal from the caller).
 */
static void ljsw_key(logger_json_stream_writer_t *w, const char *key) {
  ljsw_comma(w);
  fputc('"', w->stream);
  fputs(key, w->stream);
  fputs("\":", w->stream);
}

void logger_json_stream_writer_init(logger_json_stream_writer_t *w,
                                    FILE *stream) {
  w->stream = stream;
  w->needs_comma = false;
}

/* ---- structure ---- */

void logger_json_stream_writer_object_begin(logger_json_stream_writer_t *w) {
  ljsw_comma(w);
  fputc('{', w->stream);
  w->needs_comma = false;
}

void logger_json_stream_writer_object_end(logger_json_stream_writer_t *w) {
  fputc('}', w->stream);
  w->needs_comma = true;
}

void logger_json_stream_writer_array_begin(logger_json_stream_writer_t *w) {
  ljsw_comma(w);
  fputc('[', w->stream);
  w->needs_comma = false;
}

void logger_json_stream_writer_array_end(logger_json_stream_writer_t *w) {
  fputc(']', w->stream);
  w->needs_comma = true;
}

/* ---- object fields ---- */

void logger_json_stream_writer_field_string_or_null(
    logger_json_stream_writer_t *w, const char *key, const char *value) {
  ljsw_key(w, key);
  if (value == NULL || value[0] == '\0') {
    fputs("null", w->stream);
  } else {
    fputc('"', w->stream);
    logger_json_fwrite_escaped(w->stream, value);
    fputc('"', w->stream);
  }
  w->needs_comma = true;
}

void logger_json_stream_writer_field_bool(logger_json_stream_writer_t *w,
                                          const char *key, bool value) {
  ljsw_key(w, key);
  fputs(value ? "true" : "false", w->stream);
  w->needs_comma = true;
}

void logger_json_stream_writer_field_uint32(logger_json_stream_writer_t *w,
                                            const char *key, uint32_t value) {
  ljsw_key(w, key);
  fprintf(w->stream, "%lu", (unsigned long)value);
  w->needs_comma = true;
}

void logger_json_stream_writer_field_uint64(logger_json_stream_writer_t *w,
                                            const char *key, uint64_t value) {
  ljsw_key(w, key);
  fprintf(w->stream, "%llu", (unsigned long long)value);
  w->needs_comma = true;
}

void logger_json_stream_writer_field_int32(logger_json_stream_writer_t *w,
                                           const char *key, int32_t value) {
  ljsw_key(w, key);
  fprintf(w->stream, "%ld", (long)value);
  w->needs_comma = true;
}

void logger_json_stream_writer_field_int64(logger_json_stream_writer_t *w,
                                           const char *key, int64_t value) {
  ljsw_key(w, key);
  fprintf(w->stream, "%lld", (long long)value);
  w->needs_comma = true;
}

void logger_json_stream_writer_field_size(logger_json_stream_writer_t *w,
                                          const char *key, size_t value) {
  ljsw_key(w, key);
  fprintf(w->stream, "%lu", (unsigned long)value);
  w->needs_comma = true;
}

void logger_json_stream_writer_field_null(logger_json_stream_writer_t *w,
                                          const char *key) {
  ljsw_key(w, key);
  fputs("null", w->stream);
  w->needs_comma = true;
}

void logger_json_stream_writer_field_raw(logger_json_stream_writer_t *w,
                                         const char *key,
                                         const char *raw_json) {
  ljsw_key(w, key);
  fputs(raw_json != NULL ? raw_json : "null", w->stream);
  w->needs_comma = true;
}

/* ---- nested structures as field value ---- */

void logger_json_stream_writer_field_object_begin(
    logger_json_stream_writer_t *w, const char *key) {
  ljsw_key(w, key);
  fputc('{', w->stream);
  w->needs_comma = false;
}

void logger_json_stream_writer_field_array_begin(logger_json_stream_writer_t *w,
                                                 const char *key) {
  ljsw_key(w, key);
  fputc('[', w->stream);
  w->needs_comma = false;
}

/* ---- array elements ---- */

void logger_json_stream_writer_elem_string_or_null(
    logger_json_stream_writer_t *w, const char *value) {
  ljsw_comma(w);
  if (value == NULL || value[0] == '\0') {
    fputs("null", w->stream);
  } else {
    fputc('"', w->stream);
    logger_json_fwrite_escaped(w->stream, value);
    fputc('"', w->stream);
  }
  w->needs_comma = true;
}

void logger_json_stream_writer_elem_object_begin(
    logger_json_stream_writer_t *w) {
  ljsw_comma(w);
  fputc('{', w->stream);
  w->needs_comma = false;
}
