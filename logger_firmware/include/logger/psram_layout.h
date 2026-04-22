#ifndef LOGGER_FIRMWARE_PSRAM_LAYOUT_H
#define LOGGER_FIRMWARE_PSRAM_LAYOUT_H

#include <stddef.h>
#include <stdint.h>

#include "logger/psram.h"
#include "logger/system_log_backend_psram.h"
#include "logger/writer_protocol.h"

/*
 * Static PSRAM layout — all offsets known at compile time.
 *
 * The system log (512 KB at PSRAM_BASE + 0) is allocated directly by
 * system_log_backend_psram.c and is not part of this struct.
 *
 * ASCII map of the current reserved PSRAM address space:
 *
 *   PSRAM_BASE = 0x11000000
 *
 *   0x11000000  +-------------------------------------------------------+
 *               | system log ring (512 KiB)                             |
 *   0x11080000  +-------------------------------------------------------+
 *               | fixed-layout writer buffers                           |
 *               |   - staging slots                                     |
 *               |   - command ring slots                                |
 *               |   - chunk buffer                                      |
 *   0x11210000  +-------------------------------------------------------+
 *               | queue region (1 MiB)                                  |
 *               |   - queue scratch JSON arena                          |
 *               |   - tmp / op / scan / delete workspaces              |
 *   0x11310000  +-------------------------------------------------------+
 *               | upload region (128 KiB)                               |
 *               |   - HTTP request / response / process workspaces      |
 *   0x11330000  +-------------------------------------------------------+
 *               | upload-bundle region (128 KiB)                        |
 *               |   - shared bundle stream workspace                    |
 *   0x11350000  +-------------------------------------------------------+
 *
 * The remaining regions are owned centrally here so file-local PSRAM users
 * (queue.c, upload.c, upload_bundle.c) cannot silently overlap via ad hoc
 * manual offsets. Individual translation units may sub-allocate only within
 * their assigned region and must prove that with local _Static_assert checks.
 *
 * This struct is never instantiated — it exists purely for compile-time
 * offset arithmetic via offsetof / sizeof.
 */

#define PSRAM_LAYOUT_REGION_ALIGN (64u * 1024u)
#define PSRAM_LAYOUT_ALIGN_UP(value, align)                                   \
  (((value) + ((align) - 1u)) & ~((align) - 1u))

#define PSRAM_LAYOUT_STAGING_COUNT  4096u
#define PSRAM_LAYOUT_CMD_RING_COUNT 256u
#define PSRAM_LAYOUT_CHUNK_BUF_SIZE (128u * 1024u)

#define PSRAM_QUEUE_REGION_SIZE         (1024u * 1024u)
#define PSRAM_UPLOAD_REGION_SIZE        (128u * 1024u)
#define PSRAM_UPLOAD_BUNDLE_REGION_SIZE (128u * 1024u)

_Static_assert(PSRAM_LAYOUT_STAGING_COUNT != 0u &&
                   (PSRAM_LAYOUT_STAGING_COUNT &
                    (PSRAM_LAYOUT_STAGING_COUNT - 1u)) == 0u,
               "PSRAM_LAYOUT_STAGING_COUNT must be a non-zero power of 2");
_Static_assert(PSRAM_LAYOUT_CMD_RING_COUNT != 0u &&
                   (PSRAM_LAYOUT_CMD_RING_COUNT &
                    (PSRAM_LAYOUT_CMD_RING_COUNT - 1u)) == 0u,
               "PSRAM_LAYOUT_CMD_RING_COUNT must be a non-zero power of 2");

_Static_assert((PSRAM_LAYOUT_REGION_ALIGN &
                (PSRAM_LAYOUT_REGION_ALIGN - 1u)) == 0u,
               "PSRAM layout alignment must be a power of 2");

/* First byte after the system log region. */
#define PSRAM_LAYOUT_BASE (PSRAM_BASE + PSRAM_SYSTEM_LOG_BYTE_SIZE)

typedef struct {
  logger_writer_cmd_t staging_slots[PSRAM_LAYOUT_STAGING_COUNT];
  logger_writer_cmd_t cmd_ring_slots[PSRAM_LAYOUT_CMD_RING_COUNT];
  uint8_t chunk_buf[PSRAM_LAYOUT_CHUNK_BUF_SIZE];
} psram_layout_t;

/*
 * Make the fixed-layout alignment contract explicit: offsetof(...) already
 * accounts for any compiler-inserted padding, and these asserts prove the
 * resulting PSRAM addresses remain safely aligned.
 */
_Static_assert((PSRAM_LAYOUT_BASE % _Alignof(logger_writer_cmd_t)) == 0u,
               "PSRAM layout base misaligns writer command slots");
_Static_assert((offsetof(psram_layout_t, cmd_ring_slots) %
                _Alignof(logger_writer_cmd_t)) == 0u,
               "cmd_ring_slots offset misaligned");
_Static_assert((offsetof(psram_layout_t, chunk_buf) % 8u) == 0u,
               "chunk_buf offset must remain 8-byte aligned");

/* Pointers to each PSRAM-backed fixed-layout buffer. */
#define PSRAM_STAGING_SLOTS                                                     \
  ((logger_writer_cmd_t *)(PSRAM_LAYOUT_BASE +                                  \
                           offsetof(psram_layout_t, staging_slots)))

#define PSRAM_CMD_RING_SLOTS                                                    \
  ((logger_writer_cmd_t *)(PSRAM_LAYOUT_BASE +                                  \
                           offsetof(psram_layout_t, cmd_ring_slots)))

#define PSRAM_CHUNK_BUF                                                         \
  ((uint8_t *)(PSRAM_LAYOUT_BASE + offsetof(psram_layout_t, chunk_buf)))

/* First byte after the fixed-layout struct, rounded up to a region boundary. */
#define PSRAM_LAYOUT_FIXED_END_OFFSET                                           \
  PSRAM_LAYOUT_ALIGN_UP(PSRAM_SYSTEM_LOG_BYTE_SIZE + sizeof(psram_layout_t),    \
                        PSRAM_LAYOUT_REGION_ALIGN)

#define PSRAM_QUEUE_REGION_OFFSET (PSRAM_LAYOUT_FIXED_END_OFFSET)
#define PSRAM_QUEUE_REGION_BASE   (PSRAM_BASE + PSRAM_QUEUE_REGION_OFFSET)

#define PSRAM_UPLOAD_REGION_OFFSET                                              \
  PSRAM_LAYOUT_ALIGN_UP(PSRAM_QUEUE_REGION_OFFSET + PSRAM_QUEUE_REGION_SIZE,    \
                        PSRAM_LAYOUT_REGION_ALIGN)
#define PSRAM_UPLOAD_REGION_BASE (PSRAM_BASE + PSRAM_UPLOAD_REGION_OFFSET)

#define PSRAM_UPLOAD_BUNDLE_REGION_OFFSET                                       \
  PSRAM_LAYOUT_ALIGN_UP(PSRAM_UPLOAD_REGION_OFFSET + PSRAM_UPLOAD_REGION_SIZE,  \
                        PSRAM_LAYOUT_REGION_ALIGN)
#define PSRAM_UPLOAD_BUNDLE_REGION_BASE                                         \
  (PSRAM_BASE + PSRAM_UPLOAD_BUNDLE_REGION_OFFSET)

#define PSRAM_LAYOUT_RESERVED_END_OFFSET                                        \
  (PSRAM_UPLOAD_BUNDLE_REGION_OFFSET + PSRAM_UPLOAD_BUNDLE_REGION_SIZE)

_Static_assert(PSRAM_SYSTEM_LOG_BYTE_SIZE + sizeof(psram_layout_t) <=
                   PSRAM_QUEUE_REGION_OFFSET,
               "fixed PSRAM layout overlaps queue region");
_Static_assert(PSRAM_QUEUE_REGION_OFFSET + PSRAM_QUEUE_REGION_SIZE <=
                   PSRAM_UPLOAD_REGION_OFFSET,
               "queue region overlaps upload region");
_Static_assert(PSRAM_UPLOAD_REGION_OFFSET + PSRAM_UPLOAD_REGION_SIZE <=
                   PSRAM_UPLOAD_BUNDLE_REGION_OFFSET,
               "upload region overlaps bundle region");
_Static_assert(PSRAM_LAYOUT_RESERVED_END_OFFSET <= PSRAM_SIZE,
               "PSRAM layout exceeds mapped PSRAM");

#endif /* LOGGER_FIRMWARE_PSRAM_LAYOUT_H */
