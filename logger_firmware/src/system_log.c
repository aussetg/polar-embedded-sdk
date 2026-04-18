#include "logger/system_log.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "logger/util.h"

#define LOGGER_SYSTEM_LOG_MAGIC 0x31474F4Cu
#define LOGGER_SYSTEM_LOG_SCHEMA_VERSION 1u

/* Packed deliberately: this struct is a direct storage image.  It must be
 * exactly LOGGER_SYSTEM_LOG_RECORD_SIZE (512 B).  The natural alignment of
 * the uint32_t / char[] members would leave internal padding, so we pack
 * and let the static_assert below guard the exact size. */
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
  char details_json[LOGGER_SYSTEM_LOG_DETAILS_JSON_MAX + 1u];
} logger_system_log_record_t;

static_assert(sizeof(logger_system_log_record_t) ==
                  LOGGER_SYSTEM_LOG_RECORD_SIZE,
              "system log record size mismatch");
static_assert((sizeof(logger_system_log_record_t) -
               sizeof(((logger_system_log_record_t *)0)->details_json)) ==
                  LOGGER_SYSTEM_LOG_RECORD_FIXED_BYTES,
              "system log fixed record bytes mismatch");

static bool
logger_system_log_record_blank(const logger_system_log_record_t *record) {
  return record->magic == 0xffffffffu;
}

static bool
logger_system_log_record_valid(const logger_system_log_record_t *record) {
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
  return logger_crc32_ieee((const uint8_t *)&copy, sizeof(copy)) ==
         record->crc32;
}

static void logger_system_log_scan(logger_system_log_t *log) {
  log->event_count = 0u;
  log->next_record_index = 0u;
  log->next_event_seq = 1u;

  const uint32_t capacity = log->backend->capacity;
  logger_system_log_record_t record;

  for (uint32_t i = 0u; i < capacity; ++i) {
    log->backend->read_record(i, &record, sizeof(record));
    if (logger_system_log_record_blank(&record)) {
      log->next_record_index = i;
      return;
    }
    if (!logger_system_log_record_valid(&record)) {
      log->next_record_index = i;
      return;
    }
    log->event_count += 1u;
    log->next_event_seq = record.event_seq + 1u;
    log->next_record_index = i + 1u;
  }
}

void logger_system_log_init(logger_system_log_t *log,
                            const system_log_backend_t *backend,
                            uint32_t boot_counter) {
  memset(log, 0, sizeof(*log));
  log->initialized = true;
  log->boot_counter = boot_counter;
  log->backend = backend;
  log->writable = (backend != NULL);
  if (!log->writable) {
    return;
  }
  logger_system_log_scan(log);
}

void logger_system_log_refresh(logger_system_log_t *log) {
  if (log == NULL || !log->initialized || !log->writable) {
    return;
  }
  logger_system_log_scan(log);
}

bool logger_system_log_append(logger_system_log_t *log, const char *utc_or_null,
                              const char *kind,
                              logger_system_log_severity_t severity,
                              const char *details_json_or_null) {
  if (log == NULL || !log->initialized || !log->writable || kind == NULL ||
      kind[0] == '\0') {
    return false;
  }

  const uint32_t capacity = log->backend->capacity;

  if (log->next_event_seq == 0u || log->next_record_index > capacity ||
      log->event_count > capacity) {
    logger_system_log_scan(log);
  }

  if (log->next_record_index >= capacity) {
    log->backend->erase_all();
    log->event_count = 0u;
    log->next_record_index = 0u;
  }

  /* Verify the target slot is blank. */
  logger_system_log_record_t target;
  log->backend->read_record(log->next_record_index, &target, sizeof(target));
  if (!logger_system_log_record_blank(&target)) {
    log->backend->erase_all();
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
  logger_copy_string_fallback(record.utc, sizeof(record.utc), utc_or_null, "");
  logger_copy_string_fallback(record.kind, sizeof(record.kind), kind,
                              "unknown");
  logger_copy_string_fallback(record.details_json, sizeof(record.details_json),
                              details_json_or_null, "{}");
  record.crc32 = 0u;
  record.crc32 = logger_crc32_ieee((const uint8_t *)&record, sizeof(record));

  log->backend->write_record(log->next_record_index, &record, sizeof(record));

  log->next_record_index += 1u;
  if (log->event_count < capacity) {
    log->event_count += 1u;
  }
  log->next_event_seq += 1u;
  return true;
}

uint32_t logger_system_log_count(const logger_system_log_t *log) {
  return log == NULL ? 0u : log->event_count;
}

bool logger_system_log_read_event(const logger_system_log_t *log,
                                  uint32_t index,
                                  logger_system_log_event_t *event) {
  if (log == NULL || event == NULL || !log->initialized ||
      log->backend == NULL) {
    return false;
  }
  if (index >= log->backend->capacity) {
    return false;
  }

  logger_system_log_record_t record;
  log->backend->read_record(index, &record, sizeof(record));

  if (!logger_system_log_record_valid(&record)) {
    return false;
  }

  memset(event, 0, sizeof(*event));
  event->event_seq = record.event_seq;
  event->boot_counter = record.boot_counter;
  event->severity = (logger_system_log_severity_t)record.severity;
  logger_copy_string_fallback(event->utc, sizeof(event->utc), record.utc, "");
  logger_copy_string_fallback(event->kind, sizeof(event->kind), record.kind,
                              "unknown");
  logger_copy_string_fallback(event->details_json, sizeof(event->details_json),
                              record.details_json, "{}");
  return true;
}

const char *
logger_system_log_severity_name(logger_system_log_severity_t severity) {
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
