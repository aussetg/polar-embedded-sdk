#ifndef LOGGER_FIRMWARE_SYSTEM_LOG_H
#define LOGGER_FIRMWARE_SYSTEM_LOG_H

#include <stdbool.h>
#include <stdint.h>

#include "logger/system_log_backend.h"

/* Record size must stay at 512 B (2 × FLASH_PAGE_SIZE) for format
 * compatibility with host-side log parsing.  Defined here instead of
 * flash_layout.h so the system log header no longer depends on flash
 * layout details. */
#define LOGGER_SYSTEM_LOG_RECORD_SIZE 512u
#define LOGGER_SYSTEM_LOG_EVENT_KIND_MAX 31
#define LOGGER_SYSTEM_LOG_RECORD_FIXED_BYTES 88u
#define LOGGER_SYSTEM_LOG_DETAILS_JSON_MAX                                     \
  (LOGGER_SYSTEM_LOG_RECORD_SIZE - LOGGER_SYSTEM_LOG_RECORD_FIXED_BYTES - 1u)

typedef enum {
  LOGGER_SYSTEM_LOG_SEVERITY_INFO = 1,
  LOGGER_SYSTEM_LOG_SEVERITY_WARN = 2,
  LOGGER_SYSTEM_LOG_SEVERITY_ERROR = 3,
} logger_system_log_severity_t;

typedef struct {
  uint32_t event_seq;
  uint32_t boot_counter;
  logger_system_log_severity_t severity;
  char utc[32];
  char kind[LOGGER_SYSTEM_LOG_EVENT_KIND_MAX + 1];
  char details_json[LOGGER_SYSTEM_LOG_DETAILS_JSON_MAX + 1];
} logger_system_log_event_t;

typedef struct {
  const system_log_backend_t *backend;
  bool initialized;
  bool writable;
  uint32_t boot_counter;
  uint32_t next_event_seq;
  uint32_t event_count;
  uint32_t next_record_index;
} logger_system_log_t;

/*
 * Initialise the system log with the given storage backend.
 *
 * backend must point to storage that is already initialised (e.g. PSRAM
 * mapped by psram_init()).  May be NULL, in which case the log is
 * initialised as read-only (all append calls become no-ops).
 */
void logger_system_log_init(logger_system_log_t *log,
                            const system_log_backend_t *backend,
                            uint32_t boot_counter);
void logger_system_log_refresh(logger_system_log_t *log);
bool logger_system_log_append(logger_system_log_t *log, const char *utc_or_null,
                              const char *kind,
                              logger_system_log_severity_t severity,
                              const char *details_json_or_null);
uint32_t logger_system_log_count(const logger_system_log_t *log);
bool logger_system_log_read_event(const logger_system_log_t *log,
                                  uint32_t index,
                                  logger_system_log_event_t *event);
const char *
logger_system_log_severity_name(logger_system_log_severity_t severity);

#endif
