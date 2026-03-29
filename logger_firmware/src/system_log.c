#include "logger/system_log.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "hardware/address_mapped.h"
#include "hardware/flash.h"
#include "hardware/sync.h"

#include "logger/flash_layout.h"

#define LOGGER_SYSTEM_LOG_MAGIC 0x31474F4Cu
#define LOGGER_SYSTEM_LOG_SCHEMA_VERSION 1u

extern char __flash_binary_end;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t schema_version;
    uint16_t record_bytes;
    uint32_t crc32;
    uint32_t event_seq;
    uint32_t boot_counter;
    uint8_t severity;
    uint8_t reserved[3];
    char utc[32];
    char kind[32];
    char details_json[168];
} logger_system_log_record_t;

static_assert(sizeof(logger_system_log_record_t) == LOGGER_FLASH_SYSTEM_LOG_RECORD_SIZE, "system log record must fit one flash page");

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

static bool logger_flash_layout_is_safe(void) {
    const uintptr_t binary_end_offset = (uintptr_t)&__flash_binary_end - XIP_BASE;
    return binary_end_offset <= LOGGER_FLASH_PERSIST_REGION_OFFSET;
}

static const logger_system_log_record_t *logger_system_log_record_ptr(uint32_t index) {
    return (const logger_system_log_record_t *)(XIP_BASE + LOGGER_FLASH_SYSTEM_LOG_REGION_OFFSET +
                                                (index * LOGGER_FLASH_SYSTEM_LOG_RECORD_SIZE));
}

static bool logger_system_log_record_blank(const logger_system_log_record_t *record) {
    return record->magic == 0xffffffffu;
}

static bool logger_system_log_record_valid(const logger_system_log_record_t *record) {
    if (record->magic != LOGGER_SYSTEM_LOG_MAGIC) {
        return false;
    }
    if (record->schema_version != LOGGER_SYSTEM_LOG_SCHEMA_VERSION) {
        return false;
    }
    if (record->record_bytes != sizeof(*record)) {
        return false;
    }

    logger_system_log_record_t copy = *record;
    copy.crc32 = 0u;
    return logger_crc32_ieee((const uint8_t *)&copy, sizeof(copy)) == record->crc32;
}

static void logger_copy_string(char *dst, size_t dst_len, const char *src, const char *fallback) {
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

static void logger_system_log_scan(logger_system_log_t *log) {
    log->event_count = 0u;
    log->next_record_index = 0u;
    log->next_event_seq = 1u;

    for (uint32_t i = 0u; i < LOGGER_FLASH_SYSTEM_LOG_RECORD_CAPACITY; ++i) {
        const logger_system_log_record_t *record = logger_system_log_record_ptr(i);
        if (logger_system_log_record_blank(record)) {
            log->next_record_index = i;
            return;
        }
        if (!logger_system_log_record_valid(record)) {
            log->next_record_index = i;
            return;
        }
        log->event_count += 1u;
        log->next_event_seq = record->event_seq + 1u;
        log->next_record_index = i + 1u;
    }
}

void logger_system_log_init(logger_system_log_t *log, uint32_t boot_counter) {
    memset(log, 0, sizeof(*log));
    log->initialized = true;
    log->boot_counter = boot_counter;
    log->writable = logger_flash_layout_is_safe();
    if (!log->writable) {
        return;
    }
    logger_system_log_scan(log);
}

static void logger_system_log_erase_region(void) {
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(LOGGER_FLASH_SYSTEM_LOG_REGION_OFFSET, LOGGER_FLASH_SYSTEM_LOG_REGION_SIZE);
    restore_interrupts(ints);
}

bool logger_system_log_append(
    logger_system_log_t *log,
    const char *utc_or_null,
    const char *kind,
    logger_system_log_severity_t severity,
    const char *details_json_or_null) {
    if (log == NULL || !log->initialized || !log->writable || kind == NULL || kind[0] == '\0') {
        return false;
    }

    if (log->next_record_index >= LOGGER_FLASH_SYSTEM_LOG_RECORD_CAPACITY) {
        logger_system_log_erase_region();
        log->event_count = 0u;
        log->next_record_index = 0u;
    }

    const uint32_t target_offset = LOGGER_FLASH_SYSTEM_LOG_REGION_OFFSET +
        (log->next_record_index * LOGGER_FLASH_SYSTEM_LOG_RECORD_SIZE);
    const logger_system_log_record_t *target = (const logger_system_log_record_t *)(XIP_BASE + target_offset);
    if (!logger_system_log_record_blank(target)) {
        logger_system_log_erase_region();
        log->event_count = 0u;
        log->next_record_index = 0u;
    }

    logger_system_log_record_t record;
    memset(&record, 0xff, sizeof(record));
    record.magic = LOGGER_SYSTEM_LOG_MAGIC;
    record.schema_version = LOGGER_SYSTEM_LOG_SCHEMA_VERSION;
    record.record_bytes = sizeof(record);
    record.event_seq = log->next_event_seq;
    record.boot_counter = log->boot_counter;
    record.severity = (uint8_t)severity;
    logger_copy_string(record.utc, sizeof(record.utc), utc_or_null, "");
    logger_copy_string(record.kind, sizeof(record.kind), kind, "unknown");
    logger_copy_string(record.details_json, sizeof(record.details_json), details_json_or_null, "{}");
    record.crc32 = 0u;
    record.crc32 = logger_crc32_ieee((const uint8_t *)&record, sizeof(record));

    uint32_t ints = save_and_disable_interrupts();
    flash_range_program(target_offset, (const uint8_t *)&record, sizeof(record));
    restore_interrupts(ints);

    log->next_record_index += 1u;
    if (log->event_count < LOGGER_FLASH_SYSTEM_LOG_RECORD_CAPACITY) {
        log->event_count += 1u;
    }
    log->next_event_seq += 1u;
    return true;
}

uint32_t logger_system_log_count(const logger_system_log_t *log) {
    return log == NULL ? 0u : log->event_count;
}

bool logger_system_log_read_event(uint32_t index, logger_system_log_event_t *event) {
    if (event == NULL || index >= LOGGER_FLASH_SYSTEM_LOG_RECORD_CAPACITY) {
        return false;
    }
    const logger_system_log_record_t *record = logger_system_log_record_ptr(index);
    if (!logger_system_log_record_valid(record)) {
        return false;
    }

    memset(event, 0, sizeof(*event));
    event->event_seq = record->event_seq;
    event->boot_counter = record->boot_counter;
    event->severity = (logger_system_log_severity_t)record->severity;
    logger_copy_string(event->utc, sizeof(event->utc), record->utc, "");
    logger_copy_string(event->kind, sizeof(event->kind), record->kind, "unknown");
    logger_copy_string(event->details_json, sizeof(event->details_json), record->details_json, "{}");
    return true;
}

const char *logger_system_log_severity_name(logger_system_log_severity_t severity) {
    switch (severity) {
        case LOGGER_SYSTEM_LOG_SEVERITY_INFO:
            return "info";
        case LOGGER_SYSTEM_LOG_SEVERITY_WARN:
            return "warn";
        case LOGGER_SYSTEM_LOG_SEVERITY_ERROR:
            return "error";
        default:
            return "info";
    }
}
