#include "logger/journal.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "logger/storage.h"

#define LOGGER_JOURNAL_FILE_HEADER_BYTES 64u
#define LOGGER_JOURNAL_RECORD_HEADER_BYTES 32u
#define LOGGER_JOURNAL_FLAG_JSON 0x00000001u

static uint32_t logger_crc32_ieee(const uint8_t *data, size_t len) {
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

static void logger_put_u16le(uint8_t *dst, uint16_t value) {
    dst[0] = (uint8_t)value;
    dst[1] = (uint8_t)(value >> 8);
}

static void logger_put_u32le(uint8_t *dst, uint32_t value) {
    dst[0] = (uint8_t)value;
    dst[1] = (uint8_t)(value >> 8);
    dst[2] = (uint8_t)(value >> 16);
    dst[3] = (uint8_t)(value >> 24);
}

static void logger_put_u64le(uint8_t *dst, uint64_t value) {
    for (size_t i = 0u; i < 8u; ++i) {
        dst[i] = (uint8_t)(value >> (8u * i));
    }
}

static bool logger_hex_nibble(char ch, uint8_t *value) {
    if (ch >= '0' && ch <= '9') {
        *value = (uint8_t)(ch - '0');
        return true;
    }
    ch = (char)toupper((unsigned char)ch);
    if (ch >= 'A' && ch <= 'F') {
        *value = (uint8_t)(10 + (ch - 'A'));
        return true;
    }
    return false;
}

static bool logger_hex_to_bytes_16(const char *hex, uint8_t out[16]) {
    if (hex == NULL || strlen(hex) != 32u) {
        return false;
    }
    for (size_t i = 0u; i < 16u; ++i) {
        uint8_t hi = 0u;
        uint8_t lo = 0u;
        if (!logger_hex_nibble(hex[i * 2u], &hi) || !logger_hex_nibble(hex[(i * 2u) + 1u], &lo)) {
            return false;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

bool logger_journal_create(
    const char *path,
    const char *session_id_hex,
    uint32_t boot_counter,
    int64_t journal_open_utc_ns,
    uint64_t *size_bytes_out) {
    uint8_t header[LOGGER_JOURNAL_FILE_HEADER_BYTES];
    memset(header, 0, sizeof(header));

    memcpy(header + 0, "NOF1JNL1", 8u);
    logger_put_u16le(header + 8, LOGGER_JOURNAL_FILE_HEADER_BYTES);
    logger_put_u16le(header + 10, 1u);
    logger_put_u32le(header + 12, 0u);

    if (!logger_hex_to_bytes_16(session_id_hex, header + 16)) {
        return false;
    }

    logger_put_u64le(header + 32, boot_counter);
    logger_put_u64le(header + 40, (uint64_t)journal_open_utc_ns);
    logger_put_u32le(header + 56, logger_crc32_ieee(header, 56u));

    return logger_storage_write_file_atomic(path, header, sizeof(header)) &&
           logger_storage_file_size(path, size_bytes_out);
}

bool logger_journal_append_json_record(
    const char *path,
    logger_journal_record_type_t record_type,
    uint64_t record_seq,
    const char *json_payload,
    uint64_t *size_bytes_out) {
    if (json_payload == NULL) {
        return false;
    }

    const size_t payload_len = strlen(json_payload);
    uint8_t header[LOGGER_JOURNAL_RECORD_HEADER_BYTES];
    memset(header, 0, sizeof(header));

    memcpy(header + 0, "RCD1", 4u);
    logger_put_u16le(header + 4, LOGGER_JOURNAL_RECORD_HEADER_BYTES);
    logger_put_u16le(header + 6, (uint16_t)record_type);
    logger_put_u32le(header + 8, (uint32_t)(LOGGER_JOURNAL_RECORD_HEADER_BYTES + payload_len));
    logger_put_u32le(header + 12, (uint32_t)payload_len);
    logger_put_u32le(header + 16, LOGGER_JOURNAL_FLAG_JSON);
    logger_put_u32le(header + 20, logger_crc32_ieee((const uint8_t *)json_payload, payload_len));
    logger_put_u64le(header + 24, record_seq);

    if (!logger_storage_append_file(path, header, sizeof(header), size_bytes_out)) {
        return false;
    }
    return logger_storage_append_file(path, json_payload, payload_len, size_bytes_out);
}
