#include "logger/upload_bundle.h"

#include <limits.h>
#include <string.h>

#include "logger/sha256.h"
#include "logger/storage.h"
#include "logger/util.h"

#define LOGGER_UPLOAD_BUNDLE_NAME_MAX 100u

static bool
logger_upload_bundle_copy_name(char dst[LOGGER_UPLOAD_BUNDLE_NAME_MAX],
                               const char *src) {
  const size_t len = strlen(src);
  if (len == 0u || len >= LOGGER_UPLOAD_BUNDLE_NAME_MAX) {
    return false;
  }
  memcpy(dst, src, len + 1u);
  return true;
}

static int logger_upload_bundle_tar_write(mtar_t *tar, const void *data,
                                          unsigned size) {
  logger_upload_bundle_stream_t *stream =
      (logger_upload_bundle_stream_t *)tar->stream;
  if (stream == NULL || data == NULL ||
      (stream->emit_buf_len + size) > sizeof(stream->emit_buf)) {
    return MTAR_EWRITEFAIL;
  }
  memcpy(stream->emit_buf + stream->emit_buf_len, data, size);
  stream->emit_buf_len += size;
  return MTAR_ESUCCESS;
}

static int logger_upload_bundle_tar_seek(mtar_t *tar, unsigned pos) {
  return pos == tar->pos ? MTAR_ESUCCESS : MTAR_ESEEKFAIL;
}

static int logger_upload_bundle_tar_close(mtar_t *tar) {
  (void)tar;
  return MTAR_ESUCCESS;
}

static void
logger_upload_bundle_tar_init(logger_upload_bundle_stream_t *stream) {
  memset(&stream->tar, 0, sizeof(stream->tar));
  stream->tar.write = logger_upload_bundle_tar_write;
  stream->tar.seek = logger_upload_bundle_tar_seek;
  stream->tar.close = logger_upload_bundle_tar_close;
  stream->tar.stream = stream;
}

static bool
logger_upload_bundle_write_header(logger_upload_bundle_stream_t *stream,
                                  const char *name, unsigned size,
                                  unsigned mode, unsigned type) {
  mtar_header_t header;
  memset(&header, 0, sizeof(header));
  if (!logger_upload_bundle_copy_name(header.name, name)) {
    return false;
  }
  header.mode = mode;
  header.owner = 0u;
  header.size = size;
  header.mtime = 0u;
  header.type = type;
  return mtar_write_header(&stream->tar, &header) == MTAR_ESUCCESS;
}

static bool logger_upload_bundle_write_root_dir_header(
    logger_upload_bundle_stream_t *stream) {
  char path[LOGGER_STORAGE_PATH_MAX];
  if (!logger_path_join2(path, sizeof(path), stream->dir_name, "/")) {
    return false;
  }
  return logger_upload_bundle_write_header(stream, path, 0u, 0755u, MTAR_TDIR);
}

static bool logger_upload_bundle_write_manifest_header(
    logger_upload_bundle_stream_t *stream) {
  char path[LOGGER_STORAGE_PATH_MAX];
  if (!logger_path_join2(path, sizeof(path), stream->dir_name,
                         "/manifest.json")) {
    return false;
  }
  return logger_upload_bundle_write_header(
      stream, path, (unsigned)stream->manifest_len, 0644u, MTAR_TREG);
}

static bool logger_upload_bundle_write_journal_header(
    logger_upload_bundle_stream_t *stream) {
  char path[LOGGER_STORAGE_PATH_MAX];
  if (!logger_path_join2(path, sizeof(path), stream->dir_name,
                         "/journal.bin")) {
    return false;
  }
  return logger_upload_bundle_write_header(
      stream, path, (unsigned)stream->journal_size_bytes, 0644u, MTAR_TREG);
}

static bool
logger_upload_bundle_fill_emit(logger_upload_bundle_stream_t *stream) {
  stream->emit_buf_offset = 0u;
  stream->emit_buf_len = 0u;

  switch (stream->state) {
  case LOGGER_UPLOAD_BUNDLE_STATE_ROOT_DIR:
    if (!logger_upload_bundle_write_root_dir_header(stream)) {
      return false;
    }
    stream->state = LOGGER_UPLOAD_BUNDLE_STATE_MANIFEST_HEADER;
    return true;

  case LOGGER_UPLOAD_BUNDLE_STATE_MANIFEST_HEADER:
    if (!logger_upload_bundle_write_manifest_header(stream)) {
      return false;
    }
    stream->manifest_offset = 0u;
    stream->state = LOGGER_UPLOAD_BUNDLE_STATE_MANIFEST_DATA;
    return true;

  case LOGGER_UPLOAD_BUNDLE_STATE_MANIFEST_DATA: {
    const size_t remaining = stream->manifest_len - stream->manifest_offset;
    const unsigned chunk = remaining > sizeof(stream->io_buf)
                               ? (unsigned)sizeof(stream->io_buf)
                               : (unsigned)remaining;
    if (mtar_write_data(&stream->tar,
                        stream->manifest_buf + stream->manifest_offset,
                        chunk) != MTAR_ESUCCESS) {
      return false;
    }
    stream->manifest_offset += chunk;
    if (stream->manifest_offset == stream->manifest_len) {
      stream->state = LOGGER_UPLOAD_BUNDLE_STATE_JOURNAL_HEADER;
    }
    return true;
  }

  case LOGGER_UPLOAD_BUNDLE_STATE_JOURNAL_HEADER:
    if (!stream->journal_file_open) {
      if (f_open(&stream->journal_file, stream->journal_path, FA_READ) !=
          FR_OK) {
        return false;
      }
      stream->journal_file_open = true;
    }
    if (!logger_upload_bundle_write_journal_header(stream)) {
      return false;
    }
    stream->journal_bytes_streamed = 0u;
    stream->state = LOGGER_UPLOAD_BUNDLE_STATE_JOURNAL_DATA;
    return true;

  case LOGGER_UPLOAD_BUNDLE_STATE_JOURNAL_DATA: {
    const uint64_t remaining64 =
        stream->journal_size_bytes - stream->journal_bytes_streamed;
    const unsigned want = remaining64 > sizeof(stream->io_buf)
                              ? (unsigned)sizeof(stream->io_buf)
                              : (unsigned)remaining64;
    UINT read_bytes = 0u;
    if (want > 0u) {
      if (f_read(&stream->journal_file, stream->io_buf, want, &read_bytes) !=
              FR_OK ||
          read_bytes != want) {
        return false;
      }
    }
    if (mtar_write_data(&stream->tar, stream->io_buf, read_bytes) !=
        MTAR_ESUCCESS) {
      return false;
    }
    stream->journal_bytes_streamed += read_bytes;
    if (stream->journal_bytes_streamed == stream->journal_size_bytes) {
      if (stream->journal_file_open) {
        if (f_close(&stream->journal_file) != FR_OK) {
          stream->journal_file_open = false;
          return false;
        }
        stream->journal_file_open = false;
      }
      stream->state = LOGGER_UPLOAD_BUNDLE_STATE_FINALIZE;
    }
    return true;
  }

  case LOGGER_UPLOAD_BUNDLE_STATE_FINALIZE:
    if (mtar_finalize(&stream->tar) != MTAR_ESUCCESS) {
      return false;
    }
    stream->state = LOGGER_UPLOAD_BUNDLE_STATE_DONE;
    return true;

  case LOGGER_UPLOAD_BUNDLE_STATE_DONE:
  default:
    return true;
  }
}

static void
logger_upload_bundle_stream_init(logger_upload_bundle_stream_t *stream) {
  memset(stream, 0, sizeof(*stream));
  stream->state = LOGGER_UPLOAD_BUNDLE_STATE_DONE;
}

bool logger_upload_bundle_stream_open(logger_upload_bundle_stream_t *stream,
                                      const char *dir_name,
                                      const char *manifest_path,
                                      const char *journal_path) {
  logger_upload_bundle_stream_init(stream);
  logger_copy_string(stream->dir_name, sizeof(stream->dir_name), dir_name);
  logger_copy_string(stream->manifest_path, sizeof(stream->manifest_path),
                     manifest_path);
  logger_copy_string(stream->journal_path, sizeof(stream->journal_path),
                     journal_path);

  if (!logger_storage_read_file(stream->manifest_path, stream->manifest_buf,
                                LOGGER_UPLOAD_BUNDLE_MANIFEST_MAX,
                                &stream->manifest_len)) {
    return false;
  }
  stream->manifest_buf[stream->manifest_len] = '\0';
  if (!logger_storage_file_size(stream->journal_path,
                                &stream->journal_size_bytes)) {
    return false;
  }
  if ((unsigned)stream->manifest_len != stream->manifest_len ||
      stream->journal_size_bytes > UINT_MAX) {
    return false;
  }

  logger_upload_bundle_tar_init(stream);
  stream->state = LOGGER_UPLOAD_BUNDLE_STATE_ROOT_DIR;
  stream->initialized = true;
  return true;
}

bool logger_upload_bundle_stream_read(logger_upload_bundle_stream_t *stream,
                                      void *dst, size_t cap, size_t *len_out) {
  if (len_out != NULL) {
    *len_out = 0u;
  }
  if (!stream->initialized || dst == NULL || cap == 0u) {
    return stream->initialized;
  }

  uint8_t *out = (uint8_t *)dst;
  size_t written = 0u;
  while (written < cap) {
    if (stream->emit_buf_offset < stream->emit_buf_len) {
      const size_t remaining = stream->emit_buf_len - stream->emit_buf_offset;
      const size_t chunk =
          (cap - written) < remaining ? (cap - written) : remaining;
      memcpy(out + written, stream->emit_buf + stream->emit_buf_offset, chunk);
      stream->emit_buf_offset += chunk;
      written += chunk;
      continue;
    }

    if (stream->state == LOGGER_UPLOAD_BUNDLE_STATE_DONE) {
      break;
    }
    if (!logger_upload_bundle_fill_emit(stream)) {
      return false;
    }
  }

  if (len_out != NULL) {
    *len_out = written;
  }
  return true;
}

void logger_upload_bundle_stream_close(logger_upload_bundle_stream_t *stream) {
  if (stream == NULL) {
    return;
  }
  if (stream->journal_file_open) {
    (void)f_close(&stream->journal_file);
    stream->journal_file_open = false;
  }
  stream->initialized = false;
  stream->state = LOGGER_UPLOAD_BUNDLE_STATE_DONE;
}

bool logger_upload_bundle_compute(
    const char *dir_name, const char *manifest_path, const char *journal_path,
    char out_sha256[LOGGER_UPLOAD_QUEUE_SHA256_HEX_LEN + 1],
    uint64_t *bundle_size_out) {
  logger_upload_bundle_stream_t stream;
  if (!logger_upload_bundle_stream_open(&stream, dir_name, manifest_path,
                                        journal_path)) {
    return false;
  }

  logger_sha256_t sha;
  logger_sha256_init(&sha);
  uint64_t total_size = 0u;
  uint8_t chunk[256];
  bool ok = true;
  for (;;) {
    size_t len = 0u;
    if (!logger_upload_bundle_stream_read(&stream, chunk, sizeof(chunk),
                                          &len)) {
      ok = false;
      break;
    }
    if (len == 0u) {
      break;
    }
    logger_sha256_update(&sha, chunk, len);
    total_size += len;
  }
  logger_upload_bundle_stream_close(&stream);
  if (!ok) {
    return false;
  }

  logger_sha256_final_hex(&sha, out_sha256);
  if (bundle_size_out != NULL) {
    *bundle_size_out = total_size;
  }
  return true;
}
