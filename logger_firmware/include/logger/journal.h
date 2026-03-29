#ifndef LOGGER_FIRMWARE_JOURNAL_H
#define LOGGER_FIRMWARE_JOURNAL_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    LOGGER_JOURNAL_RECORD_SESSION_START = 0x0001,
    LOGGER_JOURNAL_RECORD_SPAN_START = 0x0002,
    LOGGER_JOURNAL_RECORD_DATA_CHUNK = 0x0003,
    LOGGER_JOURNAL_RECORD_STATUS_SNAPSHOT = 0x0004,
    LOGGER_JOURNAL_RECORD_MARKER = 0x0005,
    LOGGER_JOURNAL_RECORD_GAP = 0x0006,
    LOGGER_JOURNAL_RECORD_SPAN_END = 0x0007,
    LOGGER_JOURNAL_RECORD_SESSION_END = 0x0008,
    LOGGER_JOURNAL_RECORD_RECOVERY = 0x0009,
    LOGGER_JOURNAL_RECORD_CLOCK_EVENT = 0x000A,
    LOGGER_JOURNAL_RECORD_H10_BATTERY = 0x000B,
} logger_journal_record_type_t;

bool logger_journal_create(
    const char *path,
    const char *session_id_hex,
    uint32_t boot_counter,
    int64_t journal_open_utc_ns,
    uint64_t *size_bytes_out);

bool logger_journal_append_json_record(
    const char *path,
    logger_journal_record_type_t record_type,
    uint64_t record_seq,
    const char *json_payload,
    uint64_t *size_bytes_out);

#endif
