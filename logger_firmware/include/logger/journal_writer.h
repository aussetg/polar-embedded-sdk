#ifndef LOGGER_FIRMWARE_JOURNAL_WRITER_H
#define LOGGER_FIRMWARE_JOURNAL_WRITER_H

/*
 * Journal writer — holds an active journal.bin handle open for the
 * lifetime of a session.
 *
 * Replaces repeated open/write/sync/close with:
 *   open once → append many → sync at chunk/barrier boundaries → close on
 *   finalize.
 *
 * Size tracking:
 *   durable_size_bytes — bytes that have been f_sync'd
 *   appended_size_bytes — total bytes written through the handle
 *
 * journal_size_bytes for live.json MUST use durable_size_bytes.
 * Staged open-chunk RAM bytes are not counted here.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ff.h"

#include "logger/journal.h"
#include "logger/storage.h"

typedef struct {
  FIL file;
  bool open;
  uint64_t durable_size_bytes;
  uint64_t appended_size_bytes;
  char path[LOGGER_STORAGE_PATH_MAX];
} logger_journal_writer_t;

/* Initialize a writer to the closed/zero state. */
void logger_journal_writer_init(logger_journal_writer_t *w);

/*
 * Create a new journal.bin with file header.
 * Syncs immediately.  Writer is open on success.
 *
 * Returns false and leaves writer closed on any failure.
 */
bool logger_journal_writer_create(logger_journal_writer_t *w, const char *path,
                                  const char *session_id_hex,
                                  uint32_t boot_counter,
                                  int64_t journal_open_utc_ns);

/*
 * Open an existing journal.bin for appending (recovery path).
 * existing_size_bytes is treated as the durable baseline.
 */
bool logger_journal_writer_open_existing(logger_journal_writer_t *w,
                                         const char *path,
                                         uint64_t existing_size_bytes);

/*
 * Append a JSON record through the open handle.
 * Does NOT sync automatically — call sync() after barrier records.
 * Updates appended_size_bytes on success.
 */
bool logger_journal_writer_append_json(logger_journal_writer_t *w,
                                       logger_journal_record_type_t record_type,
                                       uint64_t record_seq,
                                       const char *json_payload);

/*
 * Append a binary record (sealed data chunk) through the open handle.
 * Does NOT sync automatically — call sync() after chunk emission.
 * Updates appended_size_bytes on success.
 */
bool logger_journal_writer_append_binary(
    logger_journal_writer_t *w, logger_journal_record_type_t record_type,
    uint64_t record_seq, const void *payload, size_t payload_len);

/*
 * Sync pending writes to durable storage.
 * Updates durable_size_bytes to match appended_size_bytes.
 */
bool logger_journal_writer_sync(logger_journal_writer_t *w);

/*
 * Sync + close.  Writer returns to closed state.
 */
bool logger_journal_writer_close(logger_journal_writer_t *w);

/*
 * Close without sync (fault/panic teardown — best-effort).
 */
void logger_journal_writer_force_close(logger_journal_writer_t *w);

/*
 * Current durable size.  Returns 0 if not open.
 */
uint64_t logger_journal_writer_durable_size(const logger_journal_writer_t *w);

/* Whether the writer has an open handle. */
bool logger_journal_writer_is_open(const logger_journal_writer_t *w);

#endif
