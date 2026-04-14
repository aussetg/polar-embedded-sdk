#ifndef LOGGER_FIRMWARE_UPLOAD_BUNDLE_H
#define LOGGER_FIRMWARE_UPLOAD_BUNDLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ff.h"
#include "microtar.h"

#include "logger/queue.h"
#include "logger/storage.h"

#define LOGGER_UPLOAD_BUNDLE_MANIFEST_MAX 8192u

typedef enum {
  LOGGER_UPLOAD_BUNDLE_STATE_ROOT_DIR = 0,
  LOGGER_UPLOAD_BUNDLE_STATE_MANIFEST_HEADER,
  LOGGER_UPLOAD_BUNDLE_STATE_MANIFEST_DATA,
  LOGGER_UPLOAD_BUNDLE_STATE_JOURNAL_HEADER,
  LOGGER_UPLOAD_BUNDLE_STATE_JOURNAL_DATA,
  LOGGER_UPLOAD_BUNDLE_STATE_FINALIZE,
  LOGGER_UPLOAD_BUNDLE_STATE_DONE,
} logger_upload_bundle_state_t;

typedef struct {
  bool initialized;
  bool journal_file_open;
  logger_upload_bundle_state_t state;
  char dir_name[64];
  char manifest_path[LOGGER_STORAGE_PATH_MAX];
  char journal_path[LOGGER_STORAGE_PATH_MAX];
  char manifest_buf[LOGGER_UPLOAD_BUNDLE_MANIFEST_MAX + 1u];
  size_t manifest_len;
  size_t manifest_offset;
  uint64_t journal_size_bytes;
  uint64_t journal_bytes_streamed;
  FIL journal_file;
  mtar_t tar;
  uint8_t emit_buf[1024];
  size_t emit_buf_offset;
  size_t emit_buf_len;
  uint8_t io_buf[512];
} logger_upload_bundle_stream_t;

void logger_upload_bundle_stream_init(logger_upload_bundle_stream_t *stream);
bool logger_upload_bundle_stream_open(logger_upload_bundle_stream_t *stream,
                                      const char *dir_name,
                                      const char *manifest_path,
                                      const char *journal_path);
bool logger_upload_bundle_stream_read(logger_upload_bundle_stream_t *stream,
                                      void *dst, size_t cap, size_t *len_out);
void logger_upload_bundle_stream_close(logger_upload_bundle_stream_t *stream);

bool logger_upload_bundle_compute(
    const char *dir_name, const char *manifest_path, const char *journal_path,
    char out_sha256[LOGGER_UPLOAD_QUEUE_SHA256_HEX_LEN + 1],
    uint64_t *bundle_size_out);

#endif