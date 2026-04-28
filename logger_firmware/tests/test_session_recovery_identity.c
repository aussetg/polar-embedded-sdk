/*
 * Host-side tests for recovered session identity preservation.
 *
 * Compile:
 *   gcc -Wall -Wextra -Werror \
 *       -I tests/shim -I include -I ../vendors/jsmn \
 *       tests/test_session_recovery_identity.c \
 *       src/journal.c src/journal_writer.c src/json.c src/session_manifest.c \
 *       -o test_session_recovery_identity
 *
 * Run:
 *   ./test_session_recovery_identity
 */

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "ff.h"

#include "logger/clock.h"
#include "logger/journal.h"
#include "logger/journal_writer.h"
#include "logger/session.h"
#include "logger/storage.h"

static uint8_t g_file_buf[65536];
static size_t g_file_len;
static bool g_fail_open;

static void shim_reset(void) {
  memset(g_file_buf, 0, sizeof(g_file_buf));
  g_file_len = 0u;
  g_fail_open = false;
}

FRESULT f_open(FIL *fp, const char *path, unsigned int mode) {
  (void)path;
  if (g_fail_open) {
    return FR_DISK_ERR;
  }
  memset(fp, 0, sizeof(*fp));
  fp->is_open = 1;
  fp->buf = g_file_buf;
  fp->buf_cap = sizeof(g_file_buf);
  fp->buf_len = g_file_len;
  fp->pos = 0u;
  if ((mode & FA_CREATE_ALWAYS) != 0u) {
    g_file_len = 0u;
    fp->buf_len = 0u;
  }
  return FR_OK;
}

FRESULT f_write(FIL *fp, const void *data, UINT len, UINT *written) {
  if (written != NULL) {
    *written = 0u;
  }
  if (!fp->is_open || fp->buf == NULL) {
    return FR_NOT_READY;
  }
  if (fp->buf_len + len > fp->buf_cap) {
    return FR_DENIED;
  }
  memcpy(fp->buf + fp->buf_len, data, len);
  fp->buf_len += len;
  fp->fsize = (FSIZE_t)fp->buf_len;
  g_file_len = fp->buf_len;
  if (written != NULL) {
    *written = len;
  }
  fp->write_count++;
  return FR_OK;
}

FRESULT f_read(FIL *fp, void *data, UINT len, UINT *read) {
  if (read != NULL) {
    *read = 0u;
  }
  if (!fp->is_open || fp->buf == NULL) {
    return FR_NOT_READY;
  }
  const size_t available = fp->buf_len > fp->pos ? fp->buf_len - fp->pos : 0u;
  const size_t n = available < (size_t)len ? available : (size_t)len;
  if (n > 0u) {
    memcpy(data, fp->buf + fp->pos, n);
    fp->pos += n;
  }
  if (read != NULL) {
    *read = (UINT)n;
  }
  return FR_OK;
}

FRESULT f_sync(FIL *fp) {
  fp->sync_count++;
  return FR_OK;
}

FRESULT f_close(FIL *fp) {
  fp->is_open = 0;
  fp->close_count++;
  return FR_OK;
}

bool logger_storage_file_size(const char *path, uint64_t *size_bytes) {
  (void)path;
  if (size_bytes != NULL) {
    *size_bytes = (uint64_t)g_file_len;
  }
  return true;
}

bool logger_storage_truncate_file(const char *path, uint64_t size_bytes) {
  (void)path;
  if (size_bytes > sizeof(g_file_buf)) {
    return false;
  }
  g_file_len = (size_t)size_bytes;
  return true;
}

bool logger_clock_format_utc_ns_rfc3339(
    int64_t utc_ns, char out_rfc3339[LOGGER_CLOCK_RFC3339_UTC_LEN + 1]) {
  if (out_rfc3339 == NULL) {
    return false;
  }
  if (utc_ns == 1777125951000000000ll) {
    strcpy(out_rfc3339, "2026-04-25T14:05:51Z");
    return true;
  }
  strcpy(out_rfc3339, "2026-01-01T00:00:00Z");
  return true;
}

static logger_journal_scan_result_t make_scanned_session_start(void) {
  shim_reset();

  logger_journal_writer_t writer;
  logger_journal_writer_init(&writer);
  assert(logger_journal_writer_create(&writer, "journal.bin",
                                      "913d0640c032abc4093b972719c26e84", 7u,
                                      1777125951000000000ll));

  const char *session_start_json =
      "{\"schema_version\":1,\"record_type\":\"session_start\","
      "\"utc_ns\":1777125951000000000,"
      "\"session_id\":\"913d0640c032abc4093b972719c26e84\","
      "\"study_day_local\":\"2026-04-25\","
      "\"start_reason\":\"first_span_of_session\","
      "\"local\":\"2026-04-25T16:05:51+02:00\","
      "\"logger_id\":\"rp2-2\",\"subject_id\":\"aussetg\","
      "\"timezone\":\"Europe/Paris\",\"clock_state\":\"valid\"}";

  assert(logger_journal_writer_append_json(
      &writer, LOGGER_JOURNAL_RECORD_SESSION_START, 0u, session_start_json));
  assert(logger_journal_writer_close(&writer));

  logger_journal_scan_result_t scan;
  memset(&scan, 0, sizeof(scan));
  assert(logger_journal_scan("journal.bin", &scan));
  assert(scan.valid);
  return scan;
}

static void test_journal_scan_captures_session_start_identity(void) {
  logger_journal_scan_result_t scan = make_scanned_session_start();

  assert(strcmp(scan.session_id, "913d0640c032abc4093b972719c26e84") == 0);
  assert(strcmp(scan.study_day_local, "2026-04-25") == 0);
  assert(strcmp(scan.session_start_utc, "2026-04-25T14:05:51Z") == 0);
  assert(strcmp(scan.session_start_reason, "first_span_of_session") == 0);
  assert(strcmp(scan.logger_id, "rp2-2") == 0);
  assert(strcmp(scan.subject_id, "aussetg") == 0);
  assert(strcmp(scan.timezone, "Europe/Paris") == 0);
}

static void
test_recovered_manifest_uses_durable_identity_when_config_empty(void) {
  const logger_journal_scan_result_t scan = make_scanned_session_start();

  logger_persisted_state_t empty_config;
  memset(&empty_config, 0, sizeof(empty_config));

  logger_storage_status_t storage;
  memset(&storage, 0, sizeof(storage));
  storage.card_present = true;
  storage.mounted = true;
  strcpy(storage.filesystem, "fat32");

  logger_session_manifest_ctx_t mc;
  logger_session_manifest_ctx_seed_recovered(
      &mc, "b48f03f1178debb042b125552282156f", &scan, &empty_config, &storage,
      false, NULL);

  assert(strcmp(mc.hardware_id, "b48f03f1178debb042b125552282156f") == 0);
  assert(strcmp(mc.logger_id, "rp2-2") == 0);
  assert(strcmp(mc.subject_id, "aussetg") == 0);
  assert(strcmp(mc.timezone, "Europe/Paris") == 0);

  logger_persisted_state_t manifest_persisted;
  logger_session_manifest_ctx_copy_persisted(&mc, &manifest_persisted);
  assert(strcmp(manifest_persisted.config.logger_id, "rp2-2") == 0);
  assert(strcmp(manifest_persisted.config.subject_id, "aussetg") == 0);
  assert(strcmp(manifest_persisted.config.timezone, "Europe/Paris") == 0);
}

static void test_live_config_overrides_recovered_identity_when_present(void) {
  const logger_journal_scan_result_t scan = make_scanned_session_start();

  logger_persisted_state_t persisted;
  memset(&persisted, 0, sizeof(persisted));
  strcpy(persisted.config.logger_id, "rp2-new");
  strcpy(persisted.config.subject_id, "subject-new");
  strcpy(persisted.config.timezone, "UTC");

  logger_session_manifest_ctx_t mc;
  logger_session_manifest_ctx_seed_recovered(&mc, "hw", &scan, &persisted, NULL,
                                             false, NULL);

  assert(strcmp(mc.logger_id, "rp2-new") == 0);
  assert(strcmp(mc.subject_id, "subject-new") == 0);
  assert(strcmp(mc.timezone, "UTC") == 0);
}

int main(void) {
  test_journal_scan_captures_session_start_identity();
  test_recovered_manifest_uses_durable_identity_when_config_empty();
  test_live_config_overrides_recovered_identity_when_present();
  puts("test_session_recovery_identity: ok");
  return 0;
}