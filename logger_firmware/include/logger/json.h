#ifndef LOGGER_FIRMWARE_JSON_H
#define LOGGER_FIRMWARE_JSON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define JSMN_HEADER
#include "jsmn.h"

typedef struct {
  const char *json;
  size_t json_len;
  jsmntok_t *tokens;
  size_t token_count;
} logger_json_doc_t;

bool logger_json_parse(logger_json_doc_t *doc, const char *json,
                       size_t json_len, jsmntok_t *tokens, size_t token_cap);

void logger_json_escape_into(char *dst, size_t dst_len, const char *src);
void logger_json_string_literal(char *dst, size_t dst_len, const char *src);
void logger_json_fwrite_escaped(FILE *stream, const char *value);

const jsmntok_t *logger_json_root(const logger_json_doc_t *doc);
const jsmntok_t *logger_json_object_get(const logger_json_doc_t *doc,
                                        const jsmntok_t *object_tok,
                                        const char *key);
const jsmntok_t *logger_json_array_get(const logger_json_doc_t *doc,
                                       const jsmntok_t *array_tok,
                                       size_t index);
bool logger_json_token_equals(const logger_json_doc_t *doc,
                              const jsmntok_t *tok, const char *literal);
bool logger_json_token_copy_string(const logger_json_doc_t *doc,
                                   const jsmntok_t *tok, char *out,
                                   size_t out_len);
bool logger_json_token_get_bool(const logger_json_doc_t *doc,
                                const jsmntok_t *tok, bool *value_out);
bool logger_json_token_get_uint32(const logger_json_doc_t *doc,
                                  const jsmntok_t *tok, uint32_t *value_out);
bool logger_json_token_get_uint64(const logger_json_doc_t *doc,
                                  const jsmntok_t *tok, uint64_t *value_out);
bool logger_json_token_get_int64(const logger_json_doc_t *doc,
                                 const jsmntok_t *tok, int64_t *value_out);
bool logger_json_token_is_null(const logger_json_doc_t *doc,
                               const jsmntok_t *tok);
bool logger_json_object_copy_string(const logger_json_doc_t *doc,
                                    const jsmntok_t *object_tok,
                                    const char *key, char *out, size_t out_len);
bool logger_json_object_copy_string_or_null(const logger_json_doc_t *doc,
                                            const jsmntok_t *object_tok,
                                            const char *key, char *out,
                                            size_t out_len);
bool logger_json_object_get_bool(const logger_json_doc_t *doc,
                                 const jsmntok_t *object_tok, const char *key,
                                 bool *value_out);
bool logger_json_object_get_uint32(const logger_json_doc_t *doc,
                                   const jsmntok_t *object_tok, const char *key,
                                   uint32_t *value_out);
bool logger_json_object_get_uint64(const logger_json_doc_t *doc,
                                   const jsmntok_t *object_tok, const char *key,
                                   uint64_t *value_out);
bool logger_json_object_get_int64(const logger_json_doc_t *doc,
                                  const jsmntok_t *object_tok, const char *key,
                                  int64_t *value_out);

#endif