// One-shot reset markers backed by RP2350 POWMAN SCRATCH registers.

#include "logger/reset_marker.h"

#include <stddef.h>
#include <string.h>

#include "hardware/structs/powman.h"
#include "hardware/sync.h"

/* RP2350 POWMAN SCRATCH0..7 are unprotected 32-bit RW registers at offsets
 * 0xb0..0xcc.  Unlike WATCHDOG SCRATCH0..7, they are not reset by a
 * watchdog-triggered chip reset, and the bootrom does not interpret them.
 *
 * Layout:
 *   scratch[0] magic, written last
 *   scratch[1] format version
 *   scratch[2] reason
 *   scratch[3] arg0
 *   scratch[4] arg1
 *   scratch[5] checksum
 */
#define LOGGER_RESET_MARKER_MAGIC 0x4c47524du /* "LGRM" */
#define LOGGER_RESET_MARKER_VERSION 1u
#define LOGGER_RESET_MARKER_CHECK_XOR 0xa57a5c3au

static uint32_t logger_reset_marker_checksum(uint32_t version, uint32_t reason,
                                             uint32_t arg0, uint32_t arg1) {
  return LOGGER_RESET_MARKER_MAGIC ^ version ^ reason ^ arg0 ^ arg1 ^
         LOGGER_RESET_MARKER_CHECK_XOR;
}

static bool logger_reset_marker_reason_valid(uint32_t reason) {
  switch ((logger_reset_marker_reason_t)reason) {
  case LOGGER_RESET_MARKER_STORAGE_SERVICE_TIMEOUT:
    return true;
  case LOGGER_RESET_MARKER_NONE:
  default:
    return false;
  }
}

const char *
logger_reset_marker_reason_name(logger_reset_marker_reason_t reason) {
  switch (reason) {
  case LOGGER_RESET_MARKER_STORAGE_SERVICE_TIMEOUT:
    return "storage_service_timeout";
  case LOGGER_RESET_MARKER_NONE:
  default:
    return NULL;
  }
}

void logger_reset_marker_clear(void) {
  for (size_t i = 0u; i < 6u; ++i) {
    powman_hw->scratch[i] = 0u;
  }
  __mem_fence_release();
}

static void logger_reset_marker_record(logger_reset_marker_reason_t reason,
                                       uint32_t arg0, uint32_t arg1) {
  const uint32_t reason_u32 = (uint32_t)reason;
  const uint32_t checksum = logger_reset_marker_checksum(
      LOGGER_RESET_MARKER_VERSION, reason_u32, arg0, arg1);

  powman_hw->scratch[0] = 0u;
  powman_hw->scratch[1] = LOGGER_RESET_MARKER_VERSION;
  powman_hw->scratch[2] = reason_u32;
  powman_hw->scratch[3] = arg0;
  powman_hw->scratch[4] = arg1;
  powman_hw->scratch[5] = checksum;
  __mem_fence_release();
  powman_hw->scratch[0] = LOGGER_RESET_MARKER_MAGIC;
  __mem_fence_release();
}

void logger_reset_marker_record_storage_service_timeout(uint32_t service_kind,
                                                        uint32_t request_seq) {
  logger_reset_marker_record(LOGGER_RESET_MARKER_STORAGE_SERVICE_TIMEOUT,
                             service_kind, request_seq);
}

bool logger_reset_marker_consume(logger_reset_marker_t *marker_out) {
  if (marker_out != NULL) {
    memset(marker_out, 0, sizeof(*marker_out));
  }

  if (powman_hw->scratch[0] != LOGGER_RESET_MARKER_MAGIC) {
    return false;
  }

  const uint32_t version = powman_hw->scratch[1];
  const uint32_t reason = powman_hw->scratch[2];
  const uint32_t arg0 = powman_hw->scratch[3];
  const uint32_t arg1 = powman_hw->scratch[4];
  const uint32_t checksum = powman_hw->scratch[5];

  logger_reset_marker_clear();

  if (version != LOGGER_RESET_MARKER_VERSION ||
      !logger_reset_marker_reason_valid(reason) ||
      checksum != logger_reset_marker_checksum(version, reason, arg0, arg1)) {
    return false;
  }

  if (marker_out != NULL) {
    marker_out->reason = (logger_reset_marker_reason_t)reason;
    marker_out->arg0 = arg0;
    marker_out->arg1 = arg1;
  }
  return true;
}
