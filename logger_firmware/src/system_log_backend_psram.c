/*
 * PSRAM-backed storage backend for the system log.
 *
 * The system log occupies the first 512 KB of the 8 MB APS6404L PSRAM
 * (1024 records × 512 B each).  The APS6404L density is 64 Mbit /
 * 8M × 8 bits (AP Memory APS6404L-3SQR datasheet Rev. 2.3, p. 1;
 * ordering-code density table p. 7).  Reads and writes are plain memcpy
 * through the QMI memory-mapped window — no erase cycles, no flash lockout.
 *
 * ## XIP cache coherency on RP2350
 *
 * The RP2350 has a SINGLE shared XIP cache (xip_ctrl, 16 KB 2-way
 * set-associative) shared by both Cortex-M33 cores.  The M33 cores on
 * this SoC were built WITHOUT private L1 caches.  Both cores use the
 * same cached XIP alias here, so either core's subsequent read observes
 * the updated shared cache state regardless of which core wrote it — no
 * per-core cache maintenance is needed for this backend's cached-alias
 * memcpy traffic.
 *
 * Do not generalise this to "physical PSRAM is immediately updated".
 * RP2350's XIP cache has dirty-line clean/invalidate operations.  Any
 * future path that mixes cached aliases with uncached aliases or external
 * visibility requirements must make that cache-maintenance policy explicit.
 *
 * This is NOT the case on many other dual-core embedded chips:
 *   - STM32H745 (M7+M4): private M7 L1 D-cache, explicit clean/invalidate
 *   - i.MX RT1170 (M7+M4): same
 *   - ESP32-S3: private instruction caches, manual coherency required
 *
 * The DMB fences below (__mem_fence_release / __mem_fence_acquire) are
 * not about cross-core cache coherency.  They ensure QMI posted writes
 * are submitted to the bus before the fence completes, and that QMI
 * reads are ordered before subsequent memory operations.  The shared
 * cache handles cached-alias coherency transparently.
 */

#include "logger/system_log_backend_psram.h"
#include "logger/psram.h"
#include "logger/system_log_backend.h"

#include <string.h>

#include "hardware/sync.h"

/* System log lives at the start of PSRAM. */
#define PSRAM_SYSTEM_LOG_OFFSET 0u
#define PSRAM_SYSTEM_LOG_BASE (PSRAM_BASE + PSRAM_SYSTEM_LOG_OFFSET)
#define PSRAM_SYSTEM_LOG_CAPACITY (PSRAM_SYSTEM_LOG_BYTE_SIZE / 512u)

static void psram_sl_write_record(uint32_t index, const void *record,
                                  size_t record_bytes) {
  void *dst =
      (void *)(PSRAM_SYSTEM_LOG_BASE + (uintptr_t)(index * record_bytes));
  memcpy(dst, record, record_bytes);
  __mem_fence_release();
}

static void psram_sl_read_record(uint32_t index, void *record,
                                 size_t record_bytes) {
  const void *src =
      (const void *)(PSRAM_SYSTEM_LOG_BASE + (uintptr_t)(index * record_bytes));
  __mem_fence_acquire();
  memcpy(record, src, record_bytes);
}

static void psram_sl_erase_all(void) {
  memset((void *)PSRAM_SYSTEM_LOG_BASE, 0xFF,
         (size_t)PSRAM_SYSTEM_LOG_CAPACITY * 512u);
  __mem_fence_release();
}

const system_log_backend_t system_log_backend_psram = {
    .write_record = psram_sl_write_record,
    .read_record = psram_sl_read_record,
    .erase_all = psram_sl_erase_all,
    .capacity = PSRAM_SYSTEM_LOG_CAPACITY,
};
