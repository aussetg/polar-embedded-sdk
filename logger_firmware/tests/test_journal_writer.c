/*
 * Host-side tests for journal_writer durable-size accounting and state machine.
 *
 * Uses a FatFS shim (tests/shim/ff.h) to compile journal_writer.c on host.
 *
 * Compile:
 *   gcc -Wall -Wextra -Wno-sign-conversion -Wno-conversion \
 *       -I tests/shim -I include/ \
 *       tests/test_journal_writer.c src/journal_writer.c \
 *       -o test_journal_writer
 *
 * Run:
 *   ./test_journal_writer
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Pull in the shim ff.h first */
#include "ff.h"

/* Then the logger headers that journal_writer.c needs */
#include "logger/journal.h"
#include "logger/journal_writer.h"
#include "logger/storage.h"

/* ── Shim state ────────────────────────────────────────────────── */

static uint8_t g_shim_buf[65536];
static size_t g_shim_buf_used;
static int g_shim_fail_open;
static int g_shim_fail_write;
static int g_shim_fail_sync;
static int g_shim_fail_close;

static void shim_reset(void) {
  memset(g_shim_buf, 0, sizeof(g_shim_buf));
  g_shim_buf_used = 0;
  g_shim_fail_open = 0;
  g_shim_fail_write = 0;
  g_shim_fail_sync = 0;
  g_shim_fail_close = 0;
}

/* ── Shim FatFS function implementations ───────────────────────── */

FRESULT f_open(FIL *fp, const char *path, unsigned int mode) {
  (void)path;
  (void)mode;
  if (g_shim_fail_open)
    return FR_DISK_ERR;
  fp->is_open = 1;
  fp->buf = g_shim_buf;
  fp->buf_cap = sizeof(g_shim_buf);
  fp->buf_len = 0;
  fp->sync_count = 0;
  fp->write_count = 0;
  fp->close_count = 0;
  fp->fsize = 0;
  g_shim_buf_used = 0;
  return FR_OK;
}

FRESULT f_write(FIL *fp, const void *data, UINT len, UINT *written) {
  if (g_shim_fail_write) {
    *written = 0;
    return FR_DISK_ERR;
  }
  if (!fp->is_open) {
    *written = 0;
    return FR_NOT_READY;
  }
  if (fp->buf_len + len > fp->buf_cap) {
    *written = 0;
    return FR_DENIED;
  }
  memcpy(fp->buf + fp->buf_len, data, len);
  fp->buf_len += len;
  fp->fsize = (FSIZE_t)fp->buf_len;
  g_shim_buf_used = fp->buf_len;
  *written = len;
  fp->write_count++;
  return FR_OK;
}

FRESULT f_sync(FIL *fp) {
  if (g_shim_fail_sync)
    return FR_DISK_ERR;
  fp->sync_count++;
  return FR_OK;
}

FRESULT f_close(FIL *fp) {
  if (g_shim_fail_close)
    return FR_DISK_ERR;
  fp->is_open = 0;
  fp->close_count++;
  return FR_OK;
}

/* ── Stub for logger_storage_file_size (pulled by headers) ─────── */

bool logger_storage_file_size(const char *path, uint64_t *size_bytes) {
  (void)path;
  if (size_bytes)
    *size_bytes = (uint64_t)g_shim_buf_used;
  return true;
}

/* ── Tests ─────────────────────────────────────────────────────── */

static void test_init_state(void) {
  printf("  init_state...");
  logger_journal_writer_t w;
  logger_journal_writer_init(&w);

  assert(!w.open);
  assert(w.durable_size_bytes == 0);
  assert(w.appended_size_bytes == 0);
  assert(!logger_journal_writer_is_open(&w));
  assert(logger_journal_writer_durable_size(&w) == 0);

  printf(" PASS\n");
}

static void test_create_and_durable_size(void) {
  printf("  create_and_durable_size...");
  shim_reset();

  logger_journal_writer_t w;
  logger_journal_writer_init(&w);

  bool ok = logger_journal_writer_create(
      &w, "0:/test/journal.bin", "8d3cf8d4d4564d0f83b3d2d6bb398d2a", 1, 0);
  assert(ok);
  assert(w.open);
  assert(w.durable_size_bytes == 64);
  assert(w.appended_size_bytes == 64);
  assert(logger_journal_writer_is_open(&w));
  assert(logger_journal_writer_durable_size(&w) == 64);

  /* Header was actually written */
  assert(g_shim_buf_used >= 64);
  assert(memcmp(g_shim_buf, "NOF1JNL1", 8) == 0);

  /* Sync was called during create */
  assert(w.file.sync_count >= 1);

  logger_journal_writer_force_close(&w);
  assert(!logger_journal_writer_is_open(&w));

  printf(" PASS\n");
}

static void test_append_without_sync(void) {
  printf("  append_without_sync...");
  shim_reset();

  logger_journal_writer_t w;
  logger_journal_writer_init(&w);
  assert(logger_journal_writer_create(
      &w, "test", "8d3cf8d4d4564d0f83b3d2d6bb398d2a", 1, 0));

  int create_sync_count = w.file.sync_count;
  uint64_t create_size = w.appended_size_bytes; /* 64 */

  /* Append a JSON record — should NOT sync automatically */
  const char *json = "{\"test\":true}";
  bool ok = logger_journal_writer_append_json(
      &w, LOGGER_JOURNAL_RECORD_SESSION_START, 0, json);
  assert(ok);

  /* appended_size should have grown by 32 (record header) + json len */
  size_t json_len = strlen(json);
  uint64_t expected = create_size + 32 + json_len;
  assert(w.appended_size_bytes == expected);

  /* durable_size should NOT have changed (no sync) */
  assert(w.durable_size_bytes == 64);

  /* No additional sync should have happened */
  assert(w.file.sync_count == create_sync_count);

  logger_journal_writer_force_close(&w);
  printf(" PASS\n");
}

static void test_sync_updates_durable(void) {
  printf("  sync_updates_durable...");
  shim_reset();

  logger_journal_writer_t w;
  logger_journal_writer_init(&w);
  assert(logger_journal_writer_create(
      &w, "test", "8d3cf8d4d4564d0f83b3d2d6bb398d2a", 1, 0));

  const char *json = "{\"test\":true}";
  logger_journal_writer_append_json(&w, LOGGER_JOURNAL_RECORD_SESSION_START, 0,
                                    json);

  uint64_t pre_sync_appended = w.appended_size_bytes;
  assert(w.durable_size_bytes == 64);

  /* Sync catches durable up to appended */
  assert(logger_journal_writer_sync(&w));
  assert(w.durable_size_bytes == pre_sync_appended);
  assert(logger_journal_writer_durable_size(&w) == pre_sync_appended);

  logger_journal_writer_force_close(&w);
  printf(" PASS\n");
}

static void test_close_syncs_and_closes(void) {
  printf("  close_syncs_and_closes...");
  shim_reset();

  logger_journal_writer_t w;
  logger_journal_writer_init(&w);
  assert(logger_journal_writer_create(
      &w, "test", "8d3cf8d4d4564d0f83b3d2d6bb398d2a", 1, 0));

  const char *json = "{\"test\":true}";
  logger_journal_writer_append_json(&w, LOGGER_JOURNAL_RECORD_SESSION_START, 0,
                                    json);

  uint64_t expected_final = w.appended_size_bytes;

  /* Close should sync + close */
  assert(logger_journal_writer_close(&w));
  assert(!w.open);
  assert(!logger_journal_writer_is_open(&w));

  /* durable should reflect final appended size */
  assert(w.durable_size_bytes == expected_final);

  /* durable_size returns 0 when closed */
  assert(logger_journal_writer_durable_size(&w) == 0);

  printf(" PASS\n");
}

static void test_open_existing_sets_baseline(void) {
  printf("  open_existing_sets_baseline...");
  shim_reset();

  logger_journal_writer_t w;
  logger_journal_writer_init(&w);

  assert(logger_journal_writer_open_existing(&w, "test", 12345));
  assert(w.open);
  assert(w.durable_size_bytes == 12345);
  assert(w.appended_size_bytes == 12345);
  assert(logger_journal_writer_durable_size(&w) == 12345);

  logger_journal_writer_force_close(&w);
  printf(" PASS\n");
}

static void test_force_close_no_sync(void) {
  printf("  force_close_no_sync...");
  shim_reset();

  logger_journal_writer_t w;
  logger_journal_writer_init(&w);
  assert(logger_journal_writer_create(
      &w, "test", "8d3cf8d4d4564d0f83b3d2d6bb398d2a", 1, 0));

  int syncs_before = w.file.sync_count;

  logger_journal_writer_force_close(&w);
  assert(!w.open);
  assert(w.file.sync_count == syncs_before);

  printf(" PASS\n");
}

static void test_write_after_close_fails(void) {
  printf("  write_after_close_fails...");
  shim_reset();

  logger_journal_writer_t w;
  logger_journal_writer_init(&w);

  assert(!logger_journal_writer_append_json(
      &w, LOGGER_JOURNAL_RECORD_SESSION_START, 0, "{}"));
  assert(!logger_journal_writer_append_binary(
      &w, LOGGER_JOURNAL_RECORD_DATA_CHUNK, 0, "data", 4));
  assert(!logger_journal_writer_sync(&w));
  assert(!logger_journal_writer_close(&w));

  printf(" PASS\n");
}

static void test_binary_append(void) {
  printf("  binary_append...");
  shim_reset();

  logger_journal_writer_t w;
  logger_journal_writer_init(&w);
  assert(logger_journal_writer_create(
      &w, "test", "8d3cf8d4d4564d0f83b3d2d6bb398d2a", 1, 0));

  uint8_t payload[100];
  memset(payload, 0xAB, sizeof(payload));

  assert(logger_journal_writer_append_binary(
      &w, LOGGER_JOURNAL_RECORD_DATA_CHUNK, 0, payload, sizeof(payload)));

  /* 64 (header) + 32 (record header) + 100 (payload) = 196 */
  assert(w.appended_size_bytes == 196);
  assert(w.durable_size_bytes == 64);

  assert(logger_journal_writer_sync(&w));
  assert(w.durable_size_bytes == 196);

  logger_journal_writer_force_close(&w);
  printf(" PASS\n");
}

static void test_sequential_appends_accumulate(void) {
  printf("  sequential_appends_accumulate...");
  shim_reset();

  logger_journal_writer_t w;
  logger_journal_writer_init(&w);
  assert(logger_journal_writer_create(
      &w, "test", "8d3cf8d4d4564d0f83b3d2d6bb398d2a", 1, 0));

  const char *json1 = "{\"a\":1}";
  const char *json2 = "{\"b\":2}";
  uint8_t bin[10] = {0};

  logger_journal_writer_append_json(&w, LOGGER_JOURNAL_RECORD_SESSION_START, 0,
                                    json1);
  logger_journal_writer_append_json(&w, LOGGER_JOURNAL_RECORD_SPAN_START, 1,
                                    json2);
  logger_journal_writer_append_binary(&w, LOGGER_JOURNAL_RECORD_DATA_CHUNK, 2,
                                      bin, sizeof(bin));

  /* durable still at header */
  assert(w.durable_size_bytes == 64);

  uint64_t total = w.appended_size_bytes;
  assert(total ==
         64 + (32 + strlen(json1)) + (32 + strlen(json2)) + (32 + sizeof(bin)));

  /* One sync makes everything durable */
  assert(logger_journal_writer_sync(&w));
  assert(w.durable_size_bytes == total);

  logger_journal_writer_force_close(&w);
  printf(" PASS\n");
}

static void test_create_failure_leaves_closed(void) {
  printf("  create_failure_leaves_closed...");
  shim_reset();
  g_shim_fail_open = 1;

  logger_journal_writer_t w;
  logger_journal_writer_init(&w);

  bool ok = logger_journal_writer_create(
      &w, "test", "8d3cf8d4d4564d0f83b3d2d6bb398d2a", 1, 0);
  assert(!ok);
  assert(!w.open);
  assert(!logger_journal_writer_is_open(&w));

  printf(" PASS\n");
}

static void test_write_failure_doesnt_advance_size(void) {
  printf("  write_failure_doesnt_advance_size...");
  shim_reset();

  logger_journal_writer_t w;
  logger_journal_writer_init(&w);
  assert(logger_journal_writer_create(
      &w, "test", "8d3cf8d4d4564d0f83b3d2d6bb398d2a", 1, 0));
  assert(w.appended_size_bytes == 64);

  g_shim_fail_write = 1;

  bool ok = logger_journal_writer_append_json(
      &w, LOGGER_JOURNAL_RECORD_SESSION_START, 0, "{}");
  assert(!ok);
  assert(w.appended_size_bytes == 64);

  logger_journal_writer_force_close(&w);
  printf(" PASS\n");
}

static void test_sync_failure_doesnt_update_durable(void) {
  printf("  sync_failure_doesnt_update_durable...");
  shim_reset();

  logger_journal_writer_t w;
  logger_journal_writer_init(&w);
  assert(logger_journal_writer_create(
      &w, "test", "8d3cf8d4d4564d0f83b3d2d6bb398d2a", 1, 0));

  const char *json = "{\"test\":true}";
  logger_journal_writer_append_json(&w, LOGGER_JOURNAL_RECORD_SESSION_START, 0,
                                    json);

  assert(w.durable_size_bytes == 64);
  assert(w.appended_size_bytes > 64);

  g_shim_fail_sync = 1;
  assert(!logger_journal_writer_sync(&w));

  /* durable should NOT have advanced */
  assert(w.durable_size_bytes == 64);

  logger_journal_writer_force_close(&w);
  printf(" PASS\n");
}

static void test_multiple_sync_cycles(void) {
  printf("  multiple_sync_cycles...");
  shim_reset();

  logger_journal_writer_t w;
  logger_journal_writer_init(&w);
  assert(logger_journal_writer_create(
      &w, "test", "8d3cf8d4d4564d0f83b3d2d6bb398d2a", 1, 0));

  /* Simulates: append chunk → sync → append barrier JSON → sync */
  uint8_t chunk[64];
  memset(chunk, 0xCC, sizeof(chunk));

  /* Chunk 1 */
  logger_journal_writer_append_binary(&w, LOGGER_JOURNAL_RECORD_DATA_CHUNK, 0,
                                      chunk, sizeof(chunk));
  assert(logger_journal_writer_sync(&w));
  assert(w.durable_size_bytes == 64 + 32 + 64); /* header + rec hdr + payload */

  uint64_t after_chunk1 = w.durable_size_bytes;

  /* Barrier JSON */
  const char *json = "{\"marker\":true}";
  logger_journal_writer_append_json(&w, LOGGER_JOURNAL_RECORD_MARKER, 1, json);
  /* Before sync, durable is still at chunk1 boundary */
  assert(w.durable_size_bytes == after_chunk1);

  logger_journal_writer_sync(&w);
  assert(w.durable_size_bytes == after_chunk1 + 32 + strlen(json));

  logger_journal_writer_force_close(&w);
  printf(" PASS\n");
}

/* ── main ──────────────────────────────────────────────────────── */

int main(void) {
  printf("journal_writer tests:\n");
  test_init_state();
  test_create_and_durable_size();
  test_append_without_sync();
  test_sync_updates_durable();
  test_close_syncs_and_closes();
  test_open_existing_sets_baseline();
  test_force_close_no_sync();
  test_write_after_close_fails();
  test_binary_append();
  test_sequential_appends_accumulate();
  test_create_failure_leaves_closed();
  test_write_failure_doesnt_advance_size();
  test_sync_failure_doesnt_update_durable();
  test_multiple_sync_cycles();
  printf("all tests passed.\n");
  return 0;
}
