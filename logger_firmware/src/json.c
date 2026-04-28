#include "jsmn.h"

#include "logger/json.h"

#include <stdlib.h>
#include <string.h>

static size_t logger_json_token_index(const logger_json_doc_t *doc,
                                      const jsmntok_t *tok) {
  return (size_t)(tok - doc->tokens);
}

static size_t logger_json_subtree_span(const logger_json_doc_t *doc,
                                       size_t index) {
  if (doc == NULL || index >= doc->token_count) {
    return 0u;
  }

  const jsmntok_t *tok = &doc->tokens[index];
  size_t span = 1u;
  switch (tok->type) {
  case JSMN_OBJECT: {
    size_t child = index + 1u;
    for (int i = 0; i < tok->size; ++i) {
      if ((child + 1u) >= doc->token_count) {
        return 0u;
      }
      const size_t value_span = logger_json_subtree_span(doc, child + 1u);
      if (value_span == 0u) {
        return 0u;
      }
      span += 1u + value_span;
      child += 1u + value_span;
    }
    return span;
  }
  case JSMN_ARRAY: {
    size_t child = index + 1u;
    for (int i = 0; i < tok->size; ++i) {
      const size_t child_span = logger_json_subtree_span(doc, child);
      if (child_span == 0u) {
        return 0u;
      }
      span += child_span;
      child += child_span;
    }
    return span;
  }
  case JSMN_STRING:
  case JSMN_PRIMITIVE:
  case JSMN_UNDEFINED:
  default:
    return span;
  }
}

static bool logger_json_hex4_decode(const char *src, uint32_t *value_out) {
  if (src == NULL || value_out == NULL) {
    return false;
  }

  uint32_t value = 0u;
  for (size_t i = 0u; i < 4u; ++i) {
    const char ch = src[i];
    value <<= 4u;
    if (ch >= '0' && ch <= '9') {
      value |= (uint32_t)(ch - '0');
    } else if (ch >= 'a' && ch <= 'f') {
      value |= (uint32_t)(10 + (ch - 'a'));
    } else if (ch >= 'A' && ch <= 'F') {
      value |= (uint32_t)(10 + (ch - 'A'));
    } else {
      return false;
    }
  }

  *value_out = value;
  return true;
}

static bool logger_json_append_byte(char *out, size_t out_len, size_t *out_i,
                                    uint8_t value) {
  if (out == NULL || out_i == NULL || (*out_i + 1u) >= out_len) {
    return false;
  }
  out[*out_i] = (char)value;
  *out_i += 1u;
  return true;
}

static bool logger_json_append_utf8(char *out, size_t out_len, size_t *out_i,
                                    uint32_t codepoint) {
  if (codepoint <= 0x7fu) {
    return logger_json_append_byte(out, out_len, out_i, (uint8_t)codepoint);
  }
  if (codepoint <= 0x7ffu) {
    return logger_json_append_byte(
               out, out_len, out_i,
               (uint8_t)(0xc0u | ((codepoint >> 6u) & 0x1fu))) &&
           logger_json_append_byte(out, out_len, out_i,
                                   (uint8_t)(0x80u | (codepoint & 0x3fu)));
  }
  if (codepoint <= 0xffffu) {
    return logger_json_append_byte(
               out, out_len, out_i,
               (uint8_t)(0xe0u | ((codepoint >> 12u) & 0x0fu))) &&
           logger_json_append_byte(
               out, out_len, out_i,
               (uint8_t)(0x80u | ((codepoint >> 6u) & 0x3fu))) &&
           logger_json_append_byte(out, out_len, out_i,
                                   (uint8_t)(0x80u | (codepoint & 0x3fu)));
  }
  if (codepoint <= 0x10ffffu) {
    return logger_json_append_byte(
               out, out_len, out_i,
               (uint8_t)(0xf0u | ((codepoint >> 18u) & 0x07u))) &&
           logger_json_append_byte(
               out, out_len, out_i,
               (uint8_t)(0x80u | ((codepoint >> 12u) & 0x3fu))) &&
           logger_json_append_byte(
               out, out_len, out_i,
               (uint8_t)(0x80u | ((codepoint >> 6u) & 0x3fu))) &&
           logger_json_append_byte(out, out_len, out_i,
                                   (uint8_t)(0x80u | (codepoint & 0x3fu)));
  }
  return false;
}

bool logger_json_escape_into(char *dst, size_t dst_len, const char *src) {
  if (dst == NULL || dst_len == 0u) {
    return false;
  }

  size_t out = 0u;
  dst[0] = '\0';
  if (src == NULL) {
    return true;
  }

  for (const unsigned char *p = (const unsigned char *)src; *p != '\0'; ++p) {
    const char *replacement = NULL;
    switch (*p) {
    case '\\':
      replacement = "\\\\";
      break;
    case '"':
      replacement = "\\\"";
      break;
    case '\b':
      replacement = "\\b";
      break;
    case '\f':
      replacement = "\\f";
      break;
    case '\n':
      replacement = "\\n";
      break;
    case '\r':
      replacement = "\\r";
      break;
    case '\t':
      replacement = "\\t";
      break;
    default:
      break;
    }
    if (replacement != NULL) {
      const size_t repl_len = strlen(replacement);
      if ((out + repl_len) >= dst_len) {
        dst[0] = '\0';
        return false;
      }
      memcpy(dst + out, replacement, repl_len);
      out += repl_len;
      continue;
    }
    if (*p < 0x20u) {
      char unicode_buf[7];
      const int n = snprintf(unicode_buf, sizeof(unicode_buf), "\\u%04x", *p);
      if (n != 6 || (out + 6u) >= dst_len) {
        dst[0] = '\0';
        return false;
      }
      memcpy(dst + out, unicode_buf, 6u);
      out += 6u;
      continue;
    }
    if ((out + 1u) >= dst_len) {
      dst[0] = '\0';
      return false;
    }
    dst[out++] = (char)*p;
  }

  dst[out] = '\0';
  return true;
}

bool logger_json_string_literal(char *dst, size_t dst_len, const char *src) {
  if (dst == NULL || dst_len == 0u) {
    return false;
  }
  dst[0] = '\0';

  if (src == NULL || src[0] == '\0') {
    const char null_literal[] = "null";
    if (sizeof(null_literal) > dst_len) {
      return false;
    }
    memcpy(dst, null_literal, sizeof(null_literal));
    return true;
  }

  if (dst_len < 3u) {
    return false;
  }

  dst[0] = '"';
  /* Reserve 2 bytes: closing quote + NUL. escape_into writes up to
     (dst_len - 2) - 1 chars plus its own NUL, which we overwrite. */
  if (!logger_json_escape_into(dst + 1u, dst_len - 2u, src)) {
    dst[0] = '\0';
    return false;
  }

  const size_t escaped_len = strlen(dst + 1u);
  dst[1u + escaped_len] = '"';
  dst[2u + escaped_len] = '\0';
  return true;
}

void logger_json_fwrite_escaped(FILE *stream, const char *value) {
  if (stream == NULL || value == NULL) {
    return;
  }

  for (const unsigned char *p = (const unsigned char *)value; *p != '\0'; ++p) {
    switch (*p) {
    case '\\':
      fputs("\\\\", stream);
      break;
    case '"':
      fputs("\\\"", stream);
      break;
    case '\b':
      fputs("\\b", stream);
      break;
    case '\f':
      fputs("\\f", stream);
      break;
    case '\n':
      fputs("\\n", stream);
      break;
    case '\r':
      fputs("\\r", stream);
      break;
    case '\t':
      fputs("\\t", stream);
      break;
    default:
      if (*p < 0x20u) {
        fprintf(stream, "\\u%04x", *p);
      } else {
        fputc((int)*p, stream);
      }
      break;
    }
  }
}

bool logger_json_parse(logger_json_doc_t *doc, const char *json,
                       size_t json_len, jsmntok_t *tokens, size_t token_cap) {
  if (doc == NULL || json == NULL || tokens == NULL || token_cap == 0u) {
    return false;
  }

  jsmn_parser parser;
  jsmn_init(&parser);
  const int rc =
      jsmn_parse(&parser, json, json_len, tokens, (unsigned int)token_cap);
  if (rc <= 0) {
    memset(doc, 0, sizeof(*doc));
    return false;
  }

  doc->json = json;
  doc->json_len = json_len;
  doc->tokens = tokens;
  doc->token_count = (size_t)rc;
  return true;
}

const jsmntok_t *logger_json_root(const logger_json_doc_t *doc) {
  if (doc == NULL || doc->token_count == 0u) {
    return NULL;
  }
  return &doc->tokens[0];
}

bool logger_json_token_equals(const logger_json_doc_t *doc,
                              const jsmntok_t *tok, const char *literal) {
  if (doc == NULL || tok == NULL || literal == NULL || tok->start < 0 ||
      tok->end < tok->start) {
    return false;
  }
  const size_t tok_len = (size_t)(tok->end - tok->start);
  return strlen(literal) == tok_len &&
         strncmp(doc->json + tok->start, literal, tok_len) == 0;
}

const jsmntok_t *logger_json_object_get(const logger_json_doc_t *doc,
                                        const jsmntok_t *object_tok,
                                        const char *key) {
  if (doc == NULL || object_tok == NULL || key == NULL ||
      object_tok->type != JSMN_OBJECT) {
    return NULL;
  }

  size_t child = logger_json_token_index(doc, object_tok) + 1u;
  for (int i = 0; i < object_tok->size; ++i) {
    if ((child + 1u) >= doc->token_count) {
      return NULL;
    }
    const jsmntok_t *key_tok = &doc->tokens[child];
    const jsmntok_t *value_tok = &doc->tokens[child + 1u];
    if (key_tok->type != JSMN_STRING) {
      return NULL;
    }
    if (logger_json_token_equals(doc, key_tok, key)) {
      return value_tok;
    }
    const size_t value_span = logger_json_subtree_span(doc, child + 1u);
    if (value_span == 0u) {
      return NULL;
    }
    child += 1u + value_span;
  }
  return NULL;
}

const jsmntok_t *logger_json_array_get(const logger_json_doc_t *doc,
                                       const jsmntok_t *array_tok,
                                       size_t index) {
  if (doc == NULL || array_tok == NULL || array_tok->type != JSMN_ARRAY ||
      index >= (size_t)array_tok->size) {
    return NULL;
  }

  size_t child = logger_json_token_index(doc, array_tok) + 1u;
  for (size_t i = 0u; i < index; ++i) {
    const size_t child_span = logger_json_subtree_span(doc, child);
    if (child_span == 0u) {
      return NULL;
    }
    child += child_span;
  }
  if (child >= doc->token_count) {
    return NULL;
  }
  return &doc->tokens[child];
}

bool logger_json_token_copy_string(const logger_json_doc_t *doc,
                                   const jsmntok_t *tok, char *out,
                                   size_t out_len) {
  if (out == NULL || out_len == 0u) {
    return false;
  }
  out[0] = '\0';
  if (doc == NULL || tok == NULL || tok->type != JSMN_STRING ||
      tok->start < 0 || tok->end < tok->start) {
    return false;
  }

  size_t out_i = 0u;
  for (int pos = tok->start; pos < tok->end; ++pos) {
    const char ch = doc->json[pos];
    if (ch != '\\') {
      if (!logger_json_append_byte(out, out_len, &out_i, (uint8_t)ch)) {
        out[0] = '\0';
        return false;
      }
      continue;
    }

    if ((pos + 1) >= tok->end) {
      out[0] = '\0';
      return false;
    }

    ++pos;
    switch (doc->json[pos]) {
    case '"':
      if (!logger_json_append_byte(out, out_len, &out_i, '"')) {
        out[0] = '\0';
        return false;
      }
      break;
    case '\\':
      if (!logger_json_append_byte(out, out_len, &out_i, '\\')) {
        out[0] = '\0';
        return false;
      }
      break;
    case '/':
      if (!logger_json_append_byte(out, out_len, &out_i, '/')) {
        out[0] = '\0';
        return false;
      }
      break;
    case 'b':
      if (!logger_json_append_byte(out, out_len, &out_i, '\b')) {
        out[0] = '\0';
        return false;
      }
      break;
    case 'f':
      if (!logger_json_append_byte(out, out_len, &out_i, '\f')) {
        out[0] = '\0';
        return false;
      }
      break;
    case 'n':
      if (!logger_json_append_byte(out, out_len, &out_i, '\n')) {
        out[0] = '\0';
        return false;
      }
      break;
    case 'r':
      if (!logger_json_append_byte(out, out_len, &out_i, '\r')) {
        out[0] = '\0';
        return false;
      }
      break;
    case 't':
      if (!logger_json_append_byte(out, out_len, &out_i, '\t')) {
        out[0] = '\0';
        return false;
      }
      break;
    case 'u': {
      if ((pos + 4) >= tok->end) {
        out[0] = '\0';
        return false;
      }
      uint32_t codepoint = 0u;
      if (!logger_json_hex4_decode(doc->json + pos + 1, &codepoint)) {
        out[0] = '\0';
        return false;
      }
      pos += 4;

      if (codepoint >= 0xd800u && codepoint <= 0xdbffu) {
        if ((pos + 6) >= tok->end || doc->json[pos + 1] != '\\' ||
            doc->json[pos + 2] != 'u') {
          out[0] = '\0';
          return false;
        }
        uint32_t low = 0u;
        if (!logger_json_hex4_decode(doc->json + pos + 3, &low) ||
            low < 0xdc00u || low > 0xdfffu) {
          out[0] = '\0';
          return false;
        }
        codepoint =
            0x10000u + (((codepoint - 0xd800u) << 10u) | (low - 0xdc00u));
        pos += 6;
      } else if (codepoint >= 0xdc00u && codepoint <= 0xdfffu) {
        out[0] = '\0';
        return false;
      }

      if (!logger_json_append_utf8(out, out_len, &out_i, codepoint)) {
        out[0] = '\0';
        return false;
      }
      break;
    }
    default:
      out[0] = '\0';
      return false;
    }
  }

  out[out_i] = '\0';
  return true;
}

bool logger_json_token_get_bool(const logger_json_doc_t *doc,
                                const jsmntok_t *tok, bool *value_out) {
  if (doc == NULL || tok == NULL || value_out == NULL ||
      tok->type != JSMN_PRIMITIVE) {
    return false;
  }
  if (logger_json_token_equals(doc, tok, "true")) {
    *value_out = true;
    return true;
  }
  if (logger_json_token_equals(doc, tok, "false")) {
    *value_out = false;
    return true;
  }
  return false;
}

bool logger_json_token_get_uint32(const logger_json_doc_t *doc,
                                  const jsmntok_t *tok, uint32_t *value_out) {
  uint64_t value = 0u;
  if (!logger_json_token_get_uint64(doc, tok, &value) || value > 0xffffffffu ||
      value_out == NULL) {
    return false;
  }
  *value_out = (uint32_t)value;
  return true;
}

bool logger_json_token_get_uint64(const logger_json_doc_t *doc,
                                  const jsmntok_t *tok, uint64_t *value_out) {
  if (doc == NULL || tok == NULL || value_out == NULL ||
      tok->type != JSMN_PRIMITIVE || tok->start < 0 || tok->end < tok->start) {
    return false;
  }
  const size_t len = (size_t)(tok->end - tok->start);
  if (len == 0u || len >= 32u) {
    return false;
  }
  char buf[32];
  memcpy(buf, doc->json + tok->start, len);
  buf[len] = '\0';
  char *end = NULL;
  const unsigned long long value = strtoull(buf, &end, 10);
  if (end == buf || *end != '\0') {
    return false;
  }
  *value_out = (uint64_t)value;
  return true;
}

bool logger_json_token_get_int64(const logger_json_doc_t *doc,
                                 const jsmntok_t *tok, int64_t *value_out) {
  if (doc == NULL || tok == NULL || value_out == NULL ||
      tok->type != JSMN_PRIMITIVE || tok->start < 0 || tok->end < tok->start) {
    return false;
  }
  const size_t len = (size_t)(tok->end - tok->start);
  if (len == 0u || len >= 32u) {
    return false;
  }
  char buf[32];
  memcpy(buf, doc->json + tok->start, len);
  buf[len] = '\0';
  char *end = NULL;
  const long long value = strtoll(buf, &end, 10);
  if (end == buf || *end != '\0') {
    return false;
  }
  *value_out = (int64_t)value;
  return true;
}

bool logger_json_token_is_null(const logger_json_doc_t *doc,
                               const jsmntok_t *tok) {
  return doc != NULL && tok != NULL && tok->type == JSMN_PRIMITIVE &&
         logger_json_token_equals(doc, tok, "null");
}

bool logger_json_object_copy_string(const logger_json_doc_t *doc,
                                    const jsmntok_t *object_tok,
                                    const char *key, char *out,
                                    size_t out_len) {
  return logger_json_token_copy_string(
      doc, logger_json_object_get(doc, object_tok, key), out, out_len);
}

bool logger_json_object_copy_string_or_null(const logger_json_doc_t *doc,
                                            const jsmntok_t *object_tok,
                                            const char *key, char *out,
                                            size_t out_len) {
  if (out == NULL || out_len == 0u) {
    return false;
  }
  out[0] = '\0';
  const jsmntok_t *tok = logger_json_object_get(doc, object_tok, key);
  if (tok == NULL) {
    return false;
  }
  if (logger_json_token_is_null(doc, tok)) {
    return true;
  }
  return logger_json_token_copy_string(doc, tok, out, out_len);
}

bool logger_json_object_get_bool(const logger_json_doc_t *doc,
                                 const jsmntok_t *object_tok, const char *key,
                                 bool *value_out) {
  return logger_json_token_get_bool(
      doc, logger_json_object_get(doc, object_tok, key), value_out);
}

bool logger_json_object_get_uint32(const logger_json_doc_t *doc,
                                   const jsmntok_t *object_tok, const char *key,
                                   uint32_t *value_out) {
  return logger_json_token_get_uint32(
      doc, logger_json_object_get(doc, object_tok, key), value_out);
}

bool logger_json_object_get_uint64(const logger_json_doc_t *doc,
                                   const jsmntok_t *object_tok, const char *key,
                                   uint64_t *value_out) {
  return logger_json_token_get_uint64(
      doc, logger_json_object_get(doc, object_tok, key), value_out);
}

bool logger_json_object_get_int64(const logger_json_doc_t *doc,
                                  const jsmntok_t *object_tok, const char *key,
                                  int64_t *value_out) {
  return logger_json_token_get_int64(
      doc, logger_json_object_get(doc, object_tok, key), value_out);
}
