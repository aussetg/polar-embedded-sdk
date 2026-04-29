// Reset-cause marker stored in RP2350 POWMAN scratch registers.
//
// Watchdog scratch registers are reset by chip-level watchdog resets and are
// also interpreted by the bootrom/SDK.  POWMAN SCRATCH0..7 are general-purpose
// always-on registers that survive watchdog resets, which makes them the right
// place for one-shot reboot breadcrumbs.

#ifndef LOGGER_FIRMWARE_RESET_MARKER_H
#define LOGGER_FIRMWARE_RESET_MARKER_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
  LOGGER_RESET_MARKER_NONE = 0,
  LOGGER_RESET_MARKER_STORAGE_SERVICE_TIMEOUT = 1,
} logger_reset_marker_reason_t;

typedef struct {
  logger_reset_marker_reason_t reason;
  uint32_t arg0;
  uint32_t arg1;
} logger_reset_marker_t;

const char *
logger_reset_marker_reason_name(logger_reset_marker_reason_t reason);

void logger_reset_marker_clear(void);
void logger_reset_marker_record_storage_service_timeout(uint32_t service_kind,
                                                        uint32_t request_seq);
bool logger_reset_marker_consume(logger_reset_marker_t *marker_out);

#endif
