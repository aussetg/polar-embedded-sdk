#include "logger/journal_writer.h"

#include <string.h>

#include "logger/util.h"

#define JOURNAL_FILE_HEADER_BYTES 64u
#define JOURNAL_RECORD_HEADER_BYTES 32u
#define JOURNAL_FLAG_JSON 0x00000001u
#define JOURNAL_FLAG_BINARY 0x00000002u

/* ── little-endian writers ─────────────────────────────────────── */

static void put_u16le(uint8_t *dst, uint16_t value) {
  dst[0] = (uint8_t)value;
  dst[1] = (uint8_t)(value >> 8);
}

static void put_u32le(uint8_t *dst, uint32_t value) {
  dst[0] = (uint8_t)value;
  dst[1] = (uint8_t)(value >> 8);
  dst[2] = (uint8_t)(value >> 16);
  dst[3] = (uint8_t)(value >> 24);
}

static void put_u64le(uint8_t *dst, uint64_t value) {
  for (size_t i = 0u; i < 8u; ++i) {
    dst[i] = (uint8_t)(value >> (8u * i));
  }
}

/* ── FatFS helpers ─────────────────────────────────────────────── */

static bool writer_fwrite(logger_journal_writer_t *w, const void *data,
                          size_t len) {
  if (!w->open || data == NULL) {
    return false;
  }
  UINT written = 0u;
  if (len == 0u) {
    return true;
  }
  if (f_write(&w->file, data, (UINT)len, &written) != FR_OK ||
      written != (UINT)len) {
    return false;
  }
  w->appended_size_bytes += len;
  return true;
}

/* ── public API ────────────────────────────────────────────────── */

void logger_journal_writer_init(logger_journal_writer_t *w) {
  memset(w, 0, sizeof(*w));
  w->open = false;
}

bool logger_journal_writer_create(logger_journal_writer_t *w, const char *path,
                                  const char *session_id_hex,
                                  uint32_t boot_counter,
                                  int64_t journal_open_utc_ns) {
  if (w == NULL || path == NULL || session_id_hex == NULL) {
    return false;
  }

  logger_journal_writer_init(w);
  logger_copy_string(w->path, sizeof(w->path), path);

  /* Build the 64-byte file header */
  uint8_t header[JOURNAL_FILE_HEADER_BYTES];
  memset(header, 0, sizeof(header));

  memcpy(header + 0, "NOF1JNL1", 8u);
  put_u16le(header + 8, JOURNAL_FILE_HEADER_BYTES);
  put_u16le(header + 10, 1u); /* format_version */
  put_u32le(header + 12, 0u); /* flags */

  if (!logger_hex_to_bytes_16(session_id_hex, header + 16)) {
    return false;
  }

  put_u64le(header + 32, boot_counter);
  put_u64le(header + 40, (uint64_t)journal_open_utc_ns);
  put_u32le(header + 56, logger_crc32_ieee(header, 56u));

  /* Open + write + sync in one shot */
  if (f_open(&w->file, w->path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) {
    return false;
  }

  UINT written = 0u;
  if (f_write(&w->file, header, sizeof(header), &written) != FR_OK ||
      written != sizeof(header)) {
    (void)f_close(&w->file);
    return false;
  }

  if (f_sync(&w->file) != FR_OK) {
    (void)f_close(&w->file);
    return false;
  }

  w->open = true;
  w->appended_size_bytes = JOURNAL_FILE_HEADER_BYTES;
  w->durable_size_bytes = JOURNAL_FILE_HEADER_BYTES;
  return true;
}

bool logger_journal_writer_open_existing(logger_journal_writer_t *w,
                                         const char *path,
                                         uint64_t existing_size_bytes) {
  if (w == NULL || path == NULL) {
    return false;
  }

  logger_journal_writer_init(w);
  logger_copy_string(w->path, sizeof(w->path), path);

  if (f_open(&w->file, w->path, FA_WRITE | FA_OPEN_APPEND) != FR_OK) {
    return false;
  }

  w->open = true;
  w->appended_size_bytes = existing_size_bytes;
  w->durable_size_bytes = existing_size_bytes;
  return true;
}

bool logger_journal_writer_append_json(logger_journal_writer_t *w,
                                       logger_journal_record_type_t record_type,
                                       uint64_t record_seq,
                                       const char *json_payload) {
  if (w == NULL || !w->open || json_payload == NULL) {
    return false;
  }

  const size_t payload_len = strlen(json_payload);
  uint8_t header[JOURNAL_RECORD_HEADER_BYTES];
  memset(header, 0, sizeof(header));

  memcpy(header + 0, "RCD1", 4u);
  put_u16le(header + 4, JOURNAL_RECORD_HEADER_BYTES);
  put_u16le(header + 6, (uint16_t)record_type);
  put_u32le(header + 8, (uint32_t)(JOURNAL_RECORD_HEADER_BYTES + payload_len));
  put_u32le(header + 12, (uint32_t)payload_len);
  put_u32le(header + 16, JOURNAL_FLAG_JSON);
  put_u32le(header + 20,
            logger_crc32_ieee((const uint8_t *)json_payload, payload_len));
  put_u64le(header + 24, record_seq);

  if (!writer_fwrite(w, header, sizeof(header))) {
    return false;
  }
  if (!writer_fwrite(w, json_payload, payload_len)) {
    return false;
  }
  return true;
}

bool logger_journal_writer_append_binary(
    logger_journal_writer_t *w, logger_journal_record_type_t record_type,
    uint64_t record_seq, const void *payload, size_t payload_len) {
  if (w == NULL || !w->open || payload == NULL ||
      (uint32_t)payload_len != payload_len) {
    return false;
  }

  uint8_t header[JOURNAL_RECORD_HEADER_BYTES];
  memset(header, 0, sizeof(header));

  memcpy(header + 0, "RCD1", 4u);
  put_u16le(header + 4, JOURNAL_RECORD_HEADER_BYTES);
  put_u16le(header + 6, (uint16_t)record_type);
  put_u32le(header + 8, (uint32_t)(JOURNAL_RECORD_HEADER_BYTES + payload_len));
  put_u32le(header + 12, (uint32_t)payload_len);
  put_u32le(header + 16, JOURNAL_FLAG_BINARY);
  put_u32le(header + 20,
            logger_crc32_ieee((const uint8_t *)payload, payload_len));
  put_u64le(header + 24, record_seq);

  if (!writer_fwrite(w, header, sizeof(header))) {
    return false;
  }
  if (!writer_fwrite(w, payload, payload_len)) {
    return false;
  }
  return true;
}

bool logger_journal_writer_sync(logger_journal_writer_t *w) {
  if (w == NULL || !w->open) {
    return false;
  }
  if (f_sync(&w->file) != FR_OK) {
    return false;
  }
  w->durable_size_bytes = w->appended_size_bytes;
  return true;
}

bool logger_journal_writer_close(logger_journal_writer_t *w) {
  if (w == NULL || !w->open) {
    return false;
  }
  const bool sync_ok = f_sync(&w->file) == FR_OK;
  const bool close_ok = f_close(&w->file) == FR_OK;
  if (sync_ok) {
    w->durable_size_bytes = w->appended_size_bytes;
  }
  w->open = false;
  return sync_ok && close_ok;
}

void logger_journal_writer_force_close(logger_journal_writer_t *w) {
  if (w == NULL || !w->open) {
    return;
  }
  (void)f_close(&w->file);
  w->open = false;
}

uint64_t logger_journal_writer_durable_size(const logger_journal_writer_t *w) {
  if (w == NULL || !w->open) {
    return 0u;
  }
  return w->durable_size_bytes;
}

bool logger_journal_writer_is_open(const logger_journal_writer_t *w) {
  return w != NULL && w->open;
}
