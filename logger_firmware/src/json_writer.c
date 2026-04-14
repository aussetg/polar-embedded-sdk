#include "logger/json_writer.h"

#include <stdio.h>
#include <string.h>

static bool logger_json_writer_append_char(logger_json_object_writer_t *writer, char ch) {
    if (writer == NULL || !writer->ok || writer->closed || writer->len >= writer->cap) {
        return false;
    }
    if ((writer->len + 1u) >= writer->cap) {
        writer->ok = false;
        return false;
    }
    writer->buf[writer->len++] = ch;
    writer->buf[writer->len] = '\0';
    return true;
}

static bool logger_json_writer_append_text(logger_json_object_writer_t *writer, const char *text) {
    if (writer == NULL || !writer->ok || writer->closed) {
        return false;
    }
    if (text == NULL) {
        text = "";
    }

    const size_t text_len = strlen(text);
    if ((writer->len + text_len) >= writer->cap) {
        writer->ok = false;
        return false;
    }

    memcpy(writer->buf + writer->len, text, text_len);
    writer->len += text_len;
    writer->buf[writer->len] = '\0';
    return true;
}

static bool logger_json_writer_append_hex4(logger_json_object_writer_t *writer, uint8_t value) {
    char escaped[7];
    const int n = snprintf(escaped, sizeof(escaped), "\\u%04x", value);
    return n == 6 && logger_json_writer_append_text(writer, escaped);
}

static bool logger_json_writer_append_quoted_string(logger_json_object_writer_t *writer, const char *value) {
    if (!logger_json_writer_append_char(writer, '"')) {
        return false;
    }

    if (value != NULL) {
        for (const unsigned char *p = (const unsigned char *)value; *p != '\0'; ++p) {
            switch (*p) {
                case '\\':
                    if (!logger_json_writer_append_text(writer, "\\\\")) {
                        return false;
                    }
                    break;
                case '"':
                    if (!logger_json_writer_append_text(writer, "\\\"")) {
                        return false;
                    }
                    break;
                case '\b':
                    if (!logger_json_writer_append_text(writer, "\\b")) {
                        return false;
                    }
                    break;
                case '\f':
                    if (!logger_json_writer_append_text(writer, "\\f")) {
                        return false;
                    }
                    break;
                case '\n':
                    if (!logger_json_writer_append_text(writer, "\\n")) {
                        return false;
                    }
                    break;
                case '\r':
                    if (!logger_json_writer_append_text(writer, "\\r")) {
                        return false;
                    }
                    break;
                case '\t':
                    if (!logger_json_writer_append_text(writer, "\\t")) {
                        return false;
                    }
                    break;
                default:
                    if (*p < 0x20u) {
                        if (!logger_json_writer_append_hex4(writer, *p)) {
                            return false;
                        }
                    } else if (!logger_json_writer_append_char(writer, (char)*p)) {
                        return false;
                    }
                    break;
            }
        }
    }

    return logger_json_writer_append_char(writer, '"');
}

static bool logger_json_object_writer_begin_field(logger_json_object_writer_t *writer, const char *key) {
    if (writer == NULL || !writer->ok || writer->closed || key == NULL || key[0] == '\0') {
        if (writer != NULL) {
            writer->ok = false;
        }
        return false;
    }

    if (!writer->first && !logger_json_writer_append_char(writer, ',')) {
        return false;
    }
    if (!logger_json_writer_append_quoted_string(writer, key)) {
        return false;
    }
    if (!logger_json_writer_append_char(writer, ':')) {
        return false;
    }

    writer->first = false;
    return true;
}

static bool logger_json_object_writer_unsigned_field(
    logger_json_object_writer_t *writer,
    const char *key,
    unsigned long long value) {
    if (!logger_json_object_writer_begin_field(writer, key)) {
        return false;
    }

    char number[32];
    const int n = snprintf(number, sizeof(number), "%llu", value);
    if (n <= 0 || (size_t)n >= sizeof(number)) {
        writer->ok = false;
        return false;
    }
    return logger_json_writer_append_text(writer, number);
}

static bool logger_json_object_writer_signed_field(
    logger_json_object_writer_t *writer,
    const char *key,
    long long value) {
    if (!logger_json_object_writer_begin_field(writer, key)) {
        return false;
    }

    char number[32];
    const int n = snprintf(number, sizeof(number), "%lld", value);
    if (n <= 0 || (size_t)n >= sizeof(number)) {
        writer->ok = false;
        return false;
    }
    return logger_json_writer_append_text(writer, number);
}

void logger_json_object_writer_init(
    logger_json_object_writer_t *writer,
    char *buf,
    size_t cap) {
    if (writer == NULL) {
        return;
    }

    writer->buf = buf;
    writer->cap = cap;
    writer->len = 0u;
    writer->ok = buf != NULL && cap >= 3u;
    writer->first = true;
    writer->closed = false;

    if (buf != NULL && cap > 0u) {
        buf[0] = '\0';
    }
    if (!writer->ok) {
        return;
    }

    (void)logger_json_writer_append_char(writer, '{');
}

bool logger_json_object_writer_string_field(
    logger_json_object_writer_t *writer,
    const char *key,
    const char *value) {
    if (!logger_json_object_writer_begin_field(writer, key)) {
        return false;
    }
    return logger_json_writer_append_quoted_string(writer, value != NULL ? value : "");
}

bool logger_json_object_writer_string_or_null_field(
    logger_json_object_writer_t *writer,
    const char *key,
    const char *value) {
    if (!logger_json_object_writer_begin_field(writer, key)) {
        return false;
    }
    if (value == NULL) {
        return logger_json_writer_append_text(writer, "null");
    }
    return logger_json_writer_append_quoted_string(writer, value);
}

bool logger_json_object_writer_bool_field(
    logger_json_object_writer_t *writer,
    const char *key,
    bool value) {
    if (!logger_json_object_writer_begin_field(writer, key)) {
        return false;
    }
    return logger_json_writer_append_text(writer, value ? "true" : "false");
}

bool logger_json_object_writer_uint32_field(
    logger_json_object_writer_t *writer,
    const char *key,
    uint32_t value) {
    return logger_json_object_writer_unsigned_field(writer, key, (unsigned long long)value);
}

bool logger_json_object_writer_uint64_field(
    logger_json_object_writer_t *writer,
    const char *key,
    uint64_t value) {
    return logger_json_object_writer_unsigned_field(writer, key, (unsigned long long)value);
}

bool logger_json_object_writer_size_field(
    logger_json_object_writer_t *writer,
    const char *key,
    size_t value) {
    return logger_json_object_writer_unsigned_field(writer, key, (unsigned long long)value);
}

bool logger_json_object_writer_int64_field(
    logger_json_object_writer_t *writer,
    const char *key,
    int64_t value) {
    return logger_json_object_writer_signed_field(writer, key, (long long)value);
}

bool logger_json_object_writer_finish(logger_json_object_writer_t *writer) {
    if (writer == NULL || !writer->ok) {
        return false;
    }
    if (writer->closed) {
        return true;
    }
    if (!logger_json_writer_append_char(writer, '}')) {
        return false;
    }
    writer->closed = true;
    return true;
}

bool logger_json_object_writer_ok(const logger_json_object_writer_t *writer) {
    return writer != NULL && writer->ok;
}

const char *logger_json_object_writer_data(const logger_json_object_writer_t *writer) {
    return writer != NULL ? writer->buf : NULL;
}