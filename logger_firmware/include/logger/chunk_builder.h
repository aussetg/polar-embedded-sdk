#ifndef LOGGER_FIRMWARE_CHUNK_BUILDER_H
#define LOGGER_FIRMWARE_CHUNK_BUILDER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * In-RAM chunk builder for v1 data_chunk assembly.
 *
 * Accumulates PMD packet entries in a caller-owned buffer.
 * On seal, produces the exact v1 data_chunk binary payload
 * ready for journal emission via logger_journal_append_binary_record.
 *
 * No heap allocation.  No I/O.  Pure computation.
 * Testable without hardware or FatFS.
 *
 * Lifecycle:
 *   init()       — once, with caller-owned buffer
 *   reset()      — discard any unsealed data (span boundary)
 *   append()     — zero or more times per chunk
 *   seal()       — finalize header, return payload
 *   clear()      — after successful journal emission
 *
 * append() returns OK / SEAL / FULL:
 *   OK   — packet added, keep accumulating
 *   SEAL — packet added, target size reached, seal recommended
 *   FULL — packet not added; seal current chunk, then retry
 *
 * The builder captures stream_kind and span_id from the first append()
 * after a reset() or clear().  Subsequent appends must match or the
 * builder returns FULL (caller seals, retries).
 */

#define LOGGER_CHUNK_HEADER_BYTES 80u
#define LOGGER_CHUNK_ENTRY_HEADER_BYTES 28u
#define LOGGER_CHUNK_ENCODING_RAW_PMD_V1 1u
#define LOGGER_CHUNK_TARGET_SIZE (64u * 1024u)
#define LOGGER_CHUNK_MAX_AGE_MS 60000u

typedef enum {
  LOGGER_CHUNK_OK = 0,   /* packet added, no seal needed yet */
  LOGGER_CHUNK_SEAL = 1, /* packet added, target size reached */
  LOGGER_CHUNK_FULL = 2, /* packet not added — seal current, then retry */
} logger_chunk_result_t;

typedef struct {
  /* Caller-provided storage (not owned) */
  uint8_t *buf;
  size_t buf_cap;

  /* Accumulated chunk state */
  bool has_data;
  uint16_t stream_kind;
  uint8_t span_id_raw[16];

  uint32_t packet_count;
  uint32_t first_seq_in_span;
  uint32_t last_seq_in_span;
  uint64_t first_mono_us;
  uint64_t last_mono_us;
  int64_t first_utc_ns;
  int64_t last_utc_ns;

  /* Write cursor: offset into buf where next entry goes */
  size_t entries_write_offset;

  /* Monotonic ms timestamp of first packet in this chunk */
  uint32_t first_packet_time_ms;
} logger_chunk_builder_t;

/* Initialize with caller-owned buffer. Call reset() before first use. */
void logger_chunk_builder_init(logger_chunk_builder_t *cb, uint8_t *buf,
                               size_t buf_cap);

/* Discard any unsealed data. Ready for a new span/stream. */
void logger_chunk_builder_reset(logger_chunk_builder_t *cb);

/*
 * Append one PMD packet entry.
 *
 * On the first append after reset()/clear(), the builder captures
 * stream_kind and span_id_raw.  Subsequent calls must match or
 * the builder returns FULL.
 *
 * now_ms: current monotonic time in milliseconds (for age tracking).
 */
logger_chunk_result_t logger_chunk_builder_append(
    logger_chunk_builder_t *cb, uint16_t stream_kind,
    const uint8_t span_id_raw[16], uint32_t seq_in_span, uint64_t mono_us,
    int64_t utc_ns, const uint8_t *value, size_t value_len, uint32_t now_ms);

/* True if chunk has been open >= 60 s. */
bool logger_chunk_builder_age_exceeded(const logger_chunk_builder_t *cb,
                                       uint32_t now_ms);

/* True if there is accumulated but unsealed data. */
bool logger_chunk_builder_has_data(const logger_chunk_builder_t *cb);

/*
 * Finalize the chunk header and return the sealed payload.
 * chunk_seq_in_session is assigned by the caller at seal time.
 * Caller must emit the payload to the journal, then call clear().
 */
bool logger_chunk_builder_seal(logger_chunk_builder_t *cb,
                               uint32_t chunk_seq_in_session,
                               const uint8_t **payload_out,
                               size_t *payload_len_out);

/* Clear after successful emission. Preserves stream_kind and span_id. */
void logger_chunk_builder_clear(logger_chunk_builder_t *cb);

#endif
