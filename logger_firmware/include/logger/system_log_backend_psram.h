#ifndef LOGGER_FIRMWARE_SYSTEM_LOG_BACKEND_PSRAM_H
#define LOGGER_FIRMWARE_SYSTEM_LOG_BACKEND_PSRAM_H

#include "logger/system_log_backend.h"

/* Total byte size of the system log region at the start of PSRAM.
 * 1024 records × 512 B each = 512 KiB.
 * Shared with psram_layout.h for compile-time offset validation. */
#define PSRAM_SYSTEM_LOG_BYTE_SIZE (1024u * 512u)

/* PSRAM storage backend instance.  Defined in system_log_backend_psram.c.
 * Use this after psram_init() has succeeded. */
extern const system_log_backend_t system_log_backend_psram;

#endif
