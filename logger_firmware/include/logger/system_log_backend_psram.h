#ifndef LOGGER_FIRMWARE_SYSTEM_LOG_BACKEND_PSRAM_H
#define LOGGER_FIRMWARE_SYSTEM_LOG_BACKEND_PSRAM_H

#include "logger/system_log_backend.h"

/* PSRAM storage backend instance.  Defined in system_log_backend_psram.c.
 * Use this after psram_init() has succeeded. */
extern const system_log_backend_t system_log_backend_psram;

#endif
