#ifndef LOGGER_FIRMWARE_SYSTEM_LOG_BACKEND_H
#define LOGGER_FIRMWARE_SYSTEM_LOG_BACKEND_H

#include <stddef.h>
#include <stdint.h>

/*
 * Storage backend interface for the system log.
 *
 * Each backend provides read/write/erase primitives indexed by record
 * number.  The system log layer handles record format, CRC, and
 * scanning — the backend is a dumb byte store.
 *
 * record_bytes is always LOGGER_SYSTEM_LOG_RECORD_SIZE (512 B).
 */
typedef struct {
  /* Write a record at the given index. */
  void (*write_record)(uint32_t index, const void *record, size_t record_bytes);

  /* Read a record at the given index into the caller's buffer. */
  void (*read_record)(uint32_t index, void *record, size_t record_bytes);

  /* Erase the entire log region. After this call, all indices
   * should read back as blank (0xFF bytes). */
  void (*erase_all)(void);

  /* Maximum number of records this backend can hold. */
  uint32_t capacity;
} system_log_backend_t;

#endif
