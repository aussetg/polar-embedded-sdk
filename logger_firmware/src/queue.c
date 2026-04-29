#include "logger/queue.h"
#include "logger/civil_date.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "ff.h"
#include "pico/stdlib.h"

#include "logger/json.h"
#include "logger/json_writer.h"
#include "logger/psram_layout.h"
#include "logger/sha256.h"
#include "logger/storage.h"
#include "logger/upload_bundle.h"
#include "logger/util.h"

/*
 * Durable A/B queue store.
 *
 * The newest valid slot is the committed queue.  Writes always target the
 * other slot, so a reset during commit cannot damage the committed copy.
 * Slot validity is header/trailer agreement + SHA-256 + JSON validation; no
 * rename/tmp/bak/rollback path is part of v1.
 */
#define LOGGER_QUEUE_SLOT_A_PATH "0:/logger/state/upload_queue.a"
#define LOGGER_QUEUE_SLOT_B_PATH "0:/logger/state/upload_queue.b"
#define LOGGER_SESSIONS_DIR "0:/logger/sessions"
#define LOGGER_MANIFEST_READ_MAX 8192u
/*
 * Bound the persisted queue by the v1 schema, not by a typical-case sample.
 * With 64 sessions, every bounded string full, and every byte escaped as
 * a six-byte JSON unicode escape, serialized JSON is ~357 KiB. 384 KiB is
 * the next clean sector-aligned ceiling and still fits easily inside the
 * 1 MiB PSRAM queue region together with tokens and queue workspaces.
 */
#define LOGGER_QUEUE_FILE_MAX (384u * 1024u)
#define LOGGER_QUEUE_SLOT_HEADER_BYTES 512u
#define LOGGER_QUEUE_SLOT_TRAILER_BYTES 512u
#define LOGGER_QUEUE_SLOT_PAYLOAD_OFFSET LOGGER_QUEUE_SLOT_HEADER_BYTES
#define LOGGER_QUEUE_SLOT_TRAILER_OFFSET                                       \
  (LOGGER_QUEUE_SLOT_PAYLOAD_OFFSET + LOGGER_QUEUE_FILE_MAX)
#define LOGGER_QUEUE_SLOT_FILE_BYTES                                           \
  (LOGGER_QUEUE_SLOT_TRAILER_OFFSET + LOGGER_QUEUE_SLOT_TRAILER_BYTES)
#define LOGGER_QUEUE_SLOT_FORMAT_VERSION 1u
#define LOGGER_MANIFEST_JSON_TOKEN_MAX 512u
#define LOGGER_QUEUE_TOP_LEVEL_JSON_TOKEN_COUNT 7u
#define LOGGER_QUEUE_ENTRY_JSON_TOKEN_COUNT 39u
#define LOGGER_QUEUE_JSON_TOKEN_MAX                                            \
  (LOGGER_QUEUE_TOP_LEVEL_JSON_TOKEN_COUNT +                                   \
   (LOGGER_UPLOAD_QUEUE_MAX_SESSIONS * LOGGER_QUEUE_ENTRY_JSON_TOKEN_COUNT))
#define LOGGER_UPLOAD_RETENTION_DAYS 14u
#define LOGGER_UPLOAD_RETENTION_SECONDS                                        \
  (LOGGER_UPLOAD_RETENTION_DAYS * 24u * 60u * 60u)

#define LOGGER_JSON_STRING_LITERAL_MAX(raw_len) (2u + (6u * (raw_len)))
#define LOGGER_DECIMAL_U64_MAX 20u
#define LOGGER_DECIMAL_U32_MAX 10u
#define LOGGER_HTTP_STATUS_DECIMAL_MAX 5u

#define LOGGER_QUEUE_FIELD_STRING_MAX(name, raw_len)                           \
  ((sizeof("\"" name "\":") - 1u) + LOGGER_JSON_STRING_LITERAL_MAX(raw_len))
#define LOGGER_QUEUE_COMMA_FIELD_STRING_MAX(name, raw_len)                     \
  ((sizeof(",\"" name "\":") - 1u) + LOGGER_JSON_STRING_LITERAL_MAX(raw_len))
#define LOGGER_QUEUE_COMMA_FIELD_RAW_MAX(name, raw_len)                        \
  ((sizeof(",\"" name "\":") - 1u) + (raw_len))

#define LOGGER_QUEUE_ENTRY_JSON_WORST_CASE_MAX                                 \
  (2u /* ",{"; first entry uses one byte less */ +                             \
   LOGGER_QUEUE_FIELD_STRING_MAX("session_id", 32u) +                          \
   LOGGER_QUEUE_COMMA_FIELD_STRING_MAX("study_day_local", 10u) +               \
   LOGGER_QUEUE_COMMA_FIELD_STRING_MAX("dir_name", 63u) +                      \
   LOGGER_QUEUE_COMMA_FIELD_STRING_MAX("session_start_utc",                    \
                                       LOGGER_UPLOAD_QUEUE_UTC_MAX) +          \
   LOGGER_QUEUE_COMMA_FIELD_STRING_MAX("session_end_utc",                      \
                                       LOGGER_UPLOAD_QUEUE_UTC_MAX) +          \
   LOGGER_QUEUE_COMMA_FIELD_STRING_MAX("bundle_sha256",                        \
                                       LOGGER_UPLOAD_QUEUE_SHA256_HEX_LEN) +   \
   LOGGER_QUEUE_COMMA_FIELD_RAW_MAX("bundle_size_bytes",                       \
                                    LOGGER_DECIMAL_U64_MAX) +                  \
   LOGGER_QUEUE_COMMA_FIELD_RAW_MAX("quarantined", 5u) +                       \
   LOGGER_QUEUE_COMMA_FIELD_STRING_MAX("status",                               \
                                       LOGGER_UPLOAD_QUEUE_STATUS_MAX) +       \
   LOGGER_QUEUE_COMMA_FIELD_RAW_MAX("attempt_count", LOGGER_DECIMAL_U32_MAX) + \
   LOGGER_QUEUE_COMMA_FIELD_STRING_MAX("last_attempt_utc",                     \
                                       LOGGER_UPLOAD_QUEUE_UTC_MAX) +          \
   LOGGER_QUEUE_COMMA_FIELD_STRING_MAX(                                        \
       "last_failure_class", LOGGER_UPLOAD_QUEUE_FAILURE_CLASS_MAX) +          \
   LOGGER_QUEUE_COMMA_FIELD_RAW_MAX("last_http_status",                        \
                                    LOGGER_HTTP_STATUS_DECIMAL_MAX) +          \
   LOGGER_QUEUE_COMMA_FIELD_STRING_MAX(                                        \
       "last_server_error_code", LOGGER_UPLOAD_QUEUE_SERVER_ERROR_CODE_MAX) +  \
   LOGGER_QUEUE_COMMA_FIELD_STRING_MAX(                                        \
       "last_server_error_message",                                            \
       LOGGER_UPLOAD_QUEUE_SERVER_ERROR_MESSAGE_MAX) +                         \
   LOGGER_QUEUE_COMMA_FIELD_STRING_MAX(                                        \
       "last_response_excerpt", LOGGER_UPLOAD_QUEUE_RESPONSE_EXCERPT_MAX) +    \
   LOGGER_QUEUE_COMMA_FIELD_STRING_MAX("verified_upload_utc",                  \
                                       LOGGER_UPLOAD_QUEUE_UTC_MAX) +          \
   LOGGER_QUEUE_COMMA_FIELD_STRING_MAX("verified_bundle_sha256",               \
                                       LOGGER_UPLOAD_QUEUE_SHA256_HEX_LEN) +   \
   LOGGER_QUEUE_COMMA_FIELD_STRING_MAX("receipt_id",                           \
                                       LOGGER_UPLOAD_QUEUE_RECEIPT_ID_MAX) +   \
   1u /* } */)

#define LOGGER_QUEUE_FILE_WORST_CASE_MAX                                       \
  ((sizeof("{\"schema_version\":1,\"updated_at_utc\":") - 1u) +                \
   LOGGER_JSON_STRING_LITERAL_MAX(LOGGER_UPLOAD_QUEUE_UTC_MAX) +               \
   (sizeof(",\"sessions\":[") - 1u) +                                          \
   (LOGGER_UPLOAD_QUEUE_MAX_SESSIONS *                                         \
    LOGGER_QUEUE_ENTRY_JSON_WORST_CASE_MAX) +                                  \
   (sizeof("]}") - 1u))

_Static_assert(LOGGER_QUEUE_FILE_WORST_CASE_MAX <= LOGGER_QUEUE_FILE_MAX,
               "upload queue file cap must cover max serialized v1 queue");
_Static_assert((LOGGER_QUEUE_FILE_MAX % 512u) == 0u,
               "upload queue file cap should be sector-aligned");
_Static_assert(LOGGER_QUEUE_SLOT_HEADER_BYTES == 512u,
               "upload queue slot header is one sector");
_Static_assert(LOGGER_QUEUE_SLOT_TRAILER_BYTES == 512u,
               "upload queue slot trailer is one sector");
_Static_assert((LOGGER_QUEUE_SLOT_FILE_BYTES % 512u) == 0u,
               "upload queue slot file should be sector-aligned");

typedef struct {
  char session_id[33];
  char study_day_local[11];
  char session_start_utc[LOGGER_UPLOAD_QUEUE_UTC_MAX + 1];
  char session_end_utc[LOGGER_UPLOAD_QUEUE_UTC_MAX + 1];
  bool quarantined;
} logger_manifest_summary_t;

/* Scratch arena for JSON parsing — queue-load and manifest-scan paths
 * are mutually exclusive in the call graph, so they share via union.
 * PSRAM-backed to free ~70 KiB of SRAM. */
typedef union {
  struct {
    FIL file;
    uint8_t header[LOGGER_QUEUE_SLOT_HEADER_BYTES];
    uint8_t trailer[LOGGER_QUEUE_SLOT_TRAILER_BYTES];
    char json[LOGGER_QUEUE_FILE_MAX + 1u];
    jsmntok_t tokens[LOGGER_QUEUE_JSON_TOKEN_MAX];
  } load;
  struct {
    char json[LOGGER_MANIFEST_READ_MAX + 1u];
    jsmntok_t tokens[LOGGER_MANIFEST_JSON_TOKEN_MAX];
  } manifest;
} queue_scratch_t;

typedef struct {
  logger_upload_queue_t queue;
  bool in_use;
} queue_tmp_workspace_t;

typedef struct {
  logger_upload_queue_t scanned;
  logger_upload_queue_t previous;
  logger_upload_queue_t merged;
  bool previous_seen[LOGGER_UPLOAD_QUEUE_MAX_SESSIONS];
  bool in_use;
} queue_op_workspace_t;

typedef struct {
  char manifest_path[LOGGER_STORAGE_PATH_MAX];
  char journal_path[LOGGER_STORAGE_PATH_MAX];
  char live_path[LOGGER_STORAGE_PATH_MAX];
  logger_manifest_summary_t manifest;
  logger_storage_status_t storage;
  DIR dir;
  FILINFO info;
  bool in_use;
} queue_scan_workspace_t;

typedef struct {
  char dir_path[LOGGER_STORAGE_PATH_MAX];
  char child_path[LOGGER_STORAGE_PATH_MAX];
  DIR dir;
  FILINFO info;
  bool in_use;
} queue_delete_workspace_t;

typedef struct {
  FIL file;
  logger_upload_queue_t probe_queue;
  bool in_use;
} queue_write_workspace_t;

typedef struct {
  char path[LOGGER_STORAGE_PATH_MAX];
  FILINFO info;
  bool in_use;
} queue_stat_workspace_t;

typedef struct {
  FIL *file;
  size_t bytes_written;
  size_t limit;
  logger_sha256_t *sha;
} logger_queue_file_writer_t;

typedef struct {
  uint32_t payload_len;
  uint64_t generation;
  uint8_t payload_sha256[LOGGER_SHA256_BYTES];
} logger_queue_slot_meta_t;

typedef enum {
  LOGGER_QUEUE_SLOT_ABSENT,
  LOGGER_QUEUE_SLOT_VALID,
  LOGGER_QUEUE_SLOT_CORRUPT,
  LOGGER_QUEUE_SLOT_IO_ERROR,
} logger_queue_slot_read_result_t;

typedef enum {
  LOGGER_UPLOAD_QUEUE_LOAD_EMPTY,
  LOGGER_UPLOAD_QUEUE_LOAD_LOADED,
  LOGGER_UPLOAD_QUEUE_LOAD_CORRUPT,
  LOGGER_UPLOAD_QUEUE_LOAD_IO_ERROR,
} logger_upload_queue_load_result_t;

typedef struct {
  const char *path;
  logger_queue_slot_read_result_t result;
  bool present;
  bool integrity_valid;
  bool queue_valid;
  uint64_t generation;
} logger_queue_slot_probe_t;

static const uint8_t LOGGER_QUEUE_SLOT_HEADER_MAGIC[16] = {
    'L', 'Q', 'S',  'L',  'O', 'T', '1',  'H',
    'D', 'R', 0x0d, 0x0a, 'A', 'B', 0x00, 0x00};
static const uint8_t LOGGER_QUEUE_SLOT_TRAILER_MAGIC[16] = {
    'L', 'Q', 'S',  'L',  'O', 'T', '1',  'T',
    'R', 'L', 0x0d, 0x0a, 'A', 'B', 0x00, 0x00};
static const uint8_t LOGGER_QUEUE_SLOT_HASH_DOMAIN[] =
    "logger.upload_queue.slot.v1";

static logger_upload_queue_load_result_t
logger_upload_queue_load_internal(logger_upload_queue_t *queue);

#define LOGGER_QUEUE_SCRATCH_ADDR (PSRAM_QUEUE_REGION_BASE)

#define LOGGER_QUEUE_PSRAM_WORKSPACE_BASE                                      \
  (LOGGER_QUEUE_SCRATCH_ADDR + sizeof(queue_scratch_t))
#define LOGGER_QUEUE_TMP_WORKSPACE_ADDR (LOGGER_QUEUE_PSRAM_WORKSPACE_BASE)
#define LOGGER_QUEUE_OP_WORKSPACE_ADDR                                         \
  (LOGGER_QUEUE_TMP_WORKSPACE_ADDR + sizeof(queue_tmp_workspace_t))
#define LOGGER_QUEUE_SCAN_WORKSPACE_ADDR                                       \
  (LOGGER_QUEUE_OP_WORKSPACE_ADDR + sizeof(queue_op_workspace_t))
#define LOGGER_QUEUE_DELETE_WORKSPACE_ADDR                                     \
  (LOGGER_QUEUE_SCAN_WORKSPACE_ADDR + sizeof(queue_scan_workspace_t))
#define LOGGER_QUEUE_WRITE_WORKSPACE_ADDR                                      \
  (LOGGER_QUEUE_DELETE_WORKSPACE_ADDR + sizeof(queue_delete_workspace_t))
#define LOGGER_QUEUE_STAT_WORKSPACE_ADDR                                       \
  (LOGGER_QUEUE_WRITE_WORKSPACE_ADDR + sizeof(queue_write_workspace_t))
#define LOGGER_QUEUE_PSRAM_WORKSPACE_END                                       \
  (LOGGER_QUEUE_STAT_WORKSPACE_ADDR + sizeof(queue_stat_workspace_t))

_Static_assert(LOGGER_QUEUE_PSRAM_WORKSPACE_END <=
                   PSRAM_QUEUE_REGION_BASE + PSRAM_QUEUE_REGION_SIZE,
               "queue PSRAM workspace exceeds reserved queue region");

static queue_scratch_t *g_queue_scratch;

static queue_tmp_workspace_t *logger_queue_tmp_workspace_ptr(void) {
  return (queue_tmp_workspace_t *)LOGGER_QUEUE_TMP_WORKSPACE_ADDR;
}

static queue_op_workspace_t *logger_queue_op_workspace_ptr(void) {
  return (queue_op_workspace_t *)LOGGER_QUEUE_OP_WORKSPACE_ADDR;
}

static queue_scan_workspace_t *logger_queue_scan_workspace_ptr(void) {
  return (queue_scan_workspace_t *)LOGGER_QUEUE_SCAN_WORKSPACE_ADDR;
}

static queue_delete_workspace_t *logger_queue_delete_workspace_ptr(void) {
  return (queue_delete_workspace_t *)LOGGER_QUEUE_DELETE_WORKSPACE_ADDR;
}

static queue_write_workspace_t *logger_queue_write_workspace_ptr(void) {
  return (queue_write_workspace_t *)LOGGER_QUEUE_WRITE_WORKSPACE_ADDR;
}

static queue_stat_workspace_t *logger_queue_stat_workspace_ptr(void) {
  return (queue_stat_workspace_t *)LOGGER_QUEUE_STAT_WORKSPACE_ADDR;
}

static void logger_queue_store_u32_le(uint8_t *dst, uint32_t value) {
  dst[0] = (uint8_t)(value & 0xffu);
  dst[1] = (uint8_t)((value >> 8) & 0xffu);
  dst[2] = (uint8_t)((value >> 16) & 0xffu);
  dst[3] = (uint8_t)((value >> 24) & 0xffu);
}

static uint32_t logger_queue_load_u32_le(const uint8_t *src) {
  return ((uint32_t)src[0]) | ((uint32_t)src[1] << 8) |
         ((uint32_t)src[2] << 16) | ((uint32_t)src[3] << 24);
}

static void logger_queue_store_u64_le(uint8_t *dst, uint64_t value) {
  for (size_t i = 0u; i < 8u; ++i) {
    dst[i] = (uint8_t)((value >> (8u * i)) & 0xffu);
  }
}

static uint64_t logger_queue_load_u64_le(const uint8_t *src) {
  uint64_t value = 0u;
  for (size_t i = 0u; i < 8u; ++i) {
    value |= ((uint64_t)src[i]) << (8u * i);
  }
  return value;
}

static void logger_queue_slot_hash_begin(logger_sha256_t *sha,
                                         uint64_t generation) {
  uint8_t generation_le[8];
  logger_queue_store_u64_le(generation_le, generation);
  logger_sha256_init(sha);
  logger_sha256_update(sha, LOGGER_QUEUE_SLOT_HASH_DOMAIN,
                       sizeof(LOGGER_QUEUE_SLOT_HASH_DOMAIN) - 1u);
  logger_sha256_update(sha, generation_le, sizeof(generation_le));
}

static void
logger_queue_slot_metadata_write(uint8_t *dst, const uint8_t magic[16],
                                 const logger_queue_slot_meta_t *meta) {
  memset(dst, 0, LOGGER_QUEUE_SLOT_HEADER_BYTES);
  memcpy(dst, magic, 16u);
  logger_queue_store_u32_le(dst + 16u, LOGGER_QUEUE_SLOT_FORMAT_VERSION);
  logger_queue_store_u32_le(dst + 20u, LOGGER_QUEUE_SLOT_HEADER_BYTES);
  logger_queue_store_u32_le(dst + 24u, LOGGER_QUEUE_SLOT_FILE_BYTES);
  logger_queue_store_u32_le(dst + 28u, LOGGER_QUEUE_SLOT_PAYLOAD_OFFSET);
  logger_queue_store_u32_le(dst + 32u, LOGGER_QUEUE_FILE_MAX);
  logger_queue_store_u32_le(dst + 36u, meta->payload_len);
  logger_queue_store_u64_le(dst + 40u, meta->generation);
  memcpy(dst + 48u, meta->payload_sha256, LOGGER_SHA256_BYTES);
}

static bool logger_queue_slot_metadata_read(const uint8_t *src,
                                            const uint8_t magic[16],
                                            logger_queue_slot_meta_t *meta) {
  if (memcmp(src, magic, 16u) != 0 ||
      logger_queue_load_u32_le(src + 16u) != LOGGER_QUEUE_SLOT_FORMAT_VERSION ||
      logger_queue_load_u32_le(src + 20u) != LOGGER_QUEUE_SLOT_HEADER_BYTES ||
      logger_queue_load_u32_le(src + 24u) != LOGGER_QUEUE_SLOT_FILE_BYTES ||
      logger_queue_load_u32_le(src + 28u) != LOGGER_QUEUE_SLOT_PAYLOAD_OFFSET ||
      logger_queue_load_u32_le(src + 32u) != LOGGER_QUEUE_FILE_MAX) {
    return false;
  }

  memset(meta, 0, sizeof(*meta));
  meta->payload_len = logger_queue_load_u32_le(src + 36u);
  meta->generation = logger_queue_load_u64_le(src + 40u);
  if (meta->payload_len > LOGGER_QUEUE_FILE_MAX || meta->generation == 0u) {
    return false;
  }
  memcpy(meta->payload_sha256, src + 48u, LOGGER_SHA256_BYTES);
  return true;
}

static bool logger_queue_slot_meta_match(const logger_queue_slot_meta_t *a,
                                         const logger_queue_slot_meta_t *b) {
  return a->payload_len == b->payload_len && a->generation == b->generation &&
         memcmp(a->payload_sha256, b->payload_sha256, LOGGER_SHA256_BYTES) == 0;
}

static bool logger_queue_file_read_exact(FIL *file, void *data, size_t len) {
  uint8_t *p = (uint8_t *)data;
  while (len > 0u) {
    const UINT chunk = (len > 4096u) ? 4096u : (UINT)len;
    UINT got = 0u;
    if (f_read(file, p, chunk, &got) != FR_OK || got != chunk) {
      return false;
    }
    p += chunk;
    len -= chunk;
  }
  return true;
}

static bool logger_queue_file_write_exact(FIL *file, const void *data,
                                          size_t len) {
  const uint8_t *p = (const uint8_t *)data;
  while (len > 0u) {
    const UINT chunk = (len > 4096u) ? 4096u : (UINT)len;
    UINT written = 0u;
    if (f_write(file, p, chunk, &written) != FR_OK || written != chunk) {
      return false;
    }
    p += chunk;
    len -= chunk;
  }
  return true;
}

/* Debug/contract tripwire for the shared core-0 temporary queue workspace.
 * This is NOT a lock. Correctness comes from the ownership invariant:
 * status/CLI/upload-control callers on core 0 may borrow it; core-1 storage
 * worker code must never touch it. Any violation is a programmer error and
 * must fail fast rather than silently serialize wrong-core access.
 */
static void logger_queue_tmp_assert_context(void) {
  hard_assert(__get_current_exception() == 0u);
  hard_assert(get_core_num() == 0u);
}

logger_upload_queue_t *logger_upload_queue_tmp_acquire(void) {
  logger_queue_tmp_assert_context();
  queue_tmp_workspace_t *workspace = logger_queue_tmp_workspace_ptr();
  assert(!workspace->in_use);
  workspace->in_use = true;
  logger_upload_queue_init(&workspace->queue);
  return &workspace->queue;
}

void logger_upload_queue_tmp_release(logger_upload_queue_t *queue) {
  logger_queue_tmp_assert_context();
  (void)queue;
  queue_tmp_workspace_t *workspace = logger_queue_tmp_workspace_ptr();
  assert(queue == &workspace->queue);
  assert(workspace->in_use);
  workspace->in_use = false;
}

static queue_op_workspace_t *logger_queue_op_workspace_acquire(void) {
  queue_op_workspace_t *workspace = logger_queue_op_workspace_ptr();
  assert(!workspace->in_use);
  workspace->in_use = true;
  logger_upload_queue_init(&workspace->scanned);
  logger_upload_queue_init(&workspace->previous);
  logger_upload_queue_init(&workspace->merged);
  memset(workspace->previous_seen, 0, sizeof(workspace->previous_seen));
  return workspace;
}

static void logger_queue_op_workspace_release(queue_op_workspace_t *workspace) {
  (void)workspace;
  queue_op_workspace_t *const expected = logger_queue_op_workspace_ptr();
  assert(workspace == expected);
  assert(expected->in_use);
  expected->in_use = false;
}

static queue_scan_workspace_t *logger_queue_scan_workspace_acquire(void) {
  queue_scan_workspace_t *workspace = logger_queue_scan_workspace_ptr();
  assert(!workspace->in_use);
  memset(workspace, 0, sizeof(*workspace));
  workspace->in_use = true;
  return workspace;
}

static void
logger_queue_scan_workspace_release(queue_scan_workspace_t *workspace) {
  (void)workspace;
  queue_scan_workspace_t *const expected = logger_queue_scan_workspace_ptr();
  assert(workspace == expected);
  assert(expected->in_use);
  expected->in_use = false;
}

static queue_delete_workspace_t *logger_queue_delete_workspace_acquire(void) {
  queue_delete_workspace_t *workspace = logger_queue_delete_workspace_ptr();
  assert(!workspace->in_use);
  memset(workspace, 0, sizeof(*workspace));
  workspace->in_use = true;
  return workspace;
}

static void
logger_queue_delete_workspace_release(queue_delete_workspace_t *workspace) {
  (void)workspace;
  queue_delete_workspace_t *const expected =
      logger_queue_delete_workspace_ptr();
  assert(workspace == expected);
  assert(expected->in_use);
  expected->in_use = false;
}

static queue_write_workspace_t *logger_queue_write_workspace_acquire(void) {
  queue_write_workspace_t *workspace = logger_queue_write_workspace_ptr();
  assert(!workspace->in_use);
  memset(workspace, 0, sizeof(*workspace));
  workspace->in_use = true;
  return workspace;
}

static void
logger_queue_write_workspace_release(queue_write_workspace_t *workspace) {
  (void)workspace;
  queue_write_workspace_t *const expected = logger_queue_write_workspace_ptr();
  assert(workspace == expected);
  assert(expected->in_use);
  expected->in_use = false;
}

static queue_stat_workspace_t *logger_queue_stat_workspace_acquire(void) {
  queue_stat_workspace_t *workspace = logger_queue_stat_workspace_ptr();
  assert(!workspace->in_use);
  memset(workspace, 0, sizeof(*workspace));
  workspace->in_use = true;
  return workspace;
}

static void
logger_queue_stat_workspace_release(queue_stat_workspace_t *workspace) {
  (void)workspace;
  queue_stat_workspace_t *const expected = logger_queue_stat_workspace_ptr();
  assert(workspace == expected);
  assert(expected->in_use);
  expected->in_use = false;
}

void logger_queue_scratch_init(void) {
  g_queue_scratch = (queue_scratch_t *)LOGGER_QUEUE_SCRATCH_ADDR;
  memset(g_queue_scratch, 0, sizeof(queue_scratch_t));
  memset(logger_queue_tmp_workspace_ptr(), 0, sizeof(queue_tmp_workspace_t));
  memset(logger_queue_op_workspace_ptr(), 0, sizeof(queue_op_workspace_t));
  memset(logger_queue_scan_workspace_ptr(), 0, sizeof(queue_scan_workspace_t));
  memset(logger_queue_delete_workspace_ptr(), 0,
         sizeof(queue_delete_workspace_t));
  memset(logger_queue_write_workspace_ptr(), 0,
         sizeof(queue_write_workspace_t));
  memset(logger_queue_stat_workspace_ptr(), 0, sizeof(queue_stat_workspace_t));
}

static bool logger_parse_manifest_summary(const char *json,
                                          logger_manifest_summary_t *summary) {
  assert(g_queue_scratch != NULL);
  memset(summary, 0, sizeof(*summary));

  logger_json_doc_t doc;
  if (!logger_json_parse(&doc, json, strlen(json),
                         g_queue_scratch->manifest.tokens,
                         LOGGER_MANIFEST_JSON_TOKEN_MAX)) {
    return false;
  }

  const jsmntok_t *root = logger_json_root(&doc);
  if (root == NULL || root->type != JSMN_OBJECT) {
    return false;
  }

  const jsmntok_t *session_tok = logger_json_object_get(&doc, root, "session");
  if (session_tok == NULL || session_tok->type != JSMN_OBJECT) {
    return false;
  }

  return logger_json_object_copy_string(&doc, root, "session_id",
                                        summary->session_id,
                                        sizeof(summary->session_id)) &&
         logger_json_object_copy_string(&doc, root, "study_day_local",
                                        summary->study_day_local,
                                        sizeof(summary->study_day_local)) &&
         logger_json_object_copy_string_or_null(
             &doc, session_tok, "start_utc", summary->session_start_utc,
             sizeof(summary->session_start_utc)) &&
         logger_json_object_copy_string_or_null(
             &doc, session_tok, "end_utc", summary->session_end_utc,
             sizeof(summary->session_end_utc)) &&
         logger_json_object_get_bool(&doc, session_tok, "quarantined",
                                     &summary->quarantined);
}

static void logger_log_local_corrupt(logger_system_log_t *system_log,
                                     const char *updated_at_utc_or_null,
                                     const char *dir_name, const char *reason) {
  if (system_log == NULL) {
    return;
  }
  char details[LOGGER_SYSTEM_LOG_DETAILS_JSON_MAX + 1];
  logger_json_object_writer_t writer;
  logger_json_object_writer_init(&writer, details, sizeof(details));
  if (!logger_json_object_writer_string_field(&writer, "dir_name", dir_name) ||
      !logger_json_object_writer_string_field(&writer, "reason", reason) ||
      !logger_json_object_writer_finish(&writer)) {
    return;
  }
  (void)logger_system_log_append(
      system_log, updated_at_utc_or_null, "local_session_corrupt",
      LOGGER_SYSTEM_LOG_SEVERITY_WARN, logger_json_object_writer_data(&writer));
}

static void
logger_log_queue_missing_local(logger_system_log_t *system_log,
                               const char *updated_at_utc_or_null,
                               const logger_upload_queue_entry_t *entry) {
  if (system_log == NULL || entry == NULL) {
    return;
  }
  char details[LOGGER_SYSTEM_LOG_DETAILS_JSON_MAX + 1];
  logger_json_object_writer_t writer;
  logger_json_object_writer_init(&writer, details, sizeof(details));
  if (!logger_json_object_writer_string_field(&writer, "dir_name",
                                              entry->dir_name) ||
      !logger_json_object_writer_string_field(&writer, "session_id",
                                              entry->session_id) ||
      !logger_json_object_writer_finish(&writer)) {
    return;
  }
  (void)logger_system_log_append(
      system_log, updated_at_utc_or_null, "queue_missing_local_removed",
      LOGGER_SYSTEM_LOG_SEVERITY_WARN, logger_json_object_writer_data(&writer));
}

static void logger_log_queue_rebuilt(logger_system_log_t *system_log,
                                     const char *updated_at_utc_or_null,
                                     const char *reason) {
  if (system_log == NULL) {
    return;
  }
  char details[LOGGER_SYSTEM_LOG_DETAILS_JSON_MAX + 1];
  logger_json_object_writer_t writer;
  logger_json_object_writer_init(&writer, details, sizeof(details));
  if (!logger_json_object_writer_string_field(&writer, "reason", reason) ||
      !logger_json_object_writer_finish(&writer)) {
    return;
  }
  (void)logger_system_log_append(
      system_log, updated_at_utc_or_null, "upload_queue_rebuilt",
      LOGGER_SYSTEM_LOG_SEVERITY_WARN, logger_json_object_writer_data(&writer));
}

static void logger_log_queue_requeued(logger_system_log_t *system_log,
                                      const char *updated_at_utc_or_null,
                                      const char *reason, size_t count) {
  if (system_log == NULL) {
    return;
  }
  char details[LOGGER_SYSTEM_LOG_DETAILS_JSON_MAX + 1];
  logger_json_object_writer_t writer;
  logger_json_object_writer_init(&writer, details, sizeof(details));
  if (!logger_json_object_writer_string_field(&writer, "reason", reason) ||
      !logger_json_object_writer_size_field(&writer, "count", count) ||
      !logger_json_object_writer_finish(&writer)) {
    return;
  }
  (void)logger_system_log_append(
      system_log, updated_at_utc_or_null, "upload_queue_requeued",
      LOGGER_SYSTEM_LOG_SEVERITY_INFO, logger_json_object_writer_data(&writer));
}

static void logger_log_session_pruned(logger_system_log_t *system_log,
                                      const char *updated_at_utc_or_null,
                                      const logger_upload_queue_entry_t *entry,
                                      const char *reason) {
  if (system_log == NULL || entry == NULL) {
    return;
  }
  char details[LOGGER_SYSTEM_LOG_DETAILS_JSON_MAX + 1];
  logger_json_object_writer_t writer;
  logger_json_object_writer_init(&writer, details, sizeof(details));
  if (!logger_json_object_writer_string_field(&writer, "session_id",
                                              entry->session_id) ||
      !logger_json_object_writer_string_field(&writer, "dir_name",
                                              entry->dir_name) ||
      !logger_json_object_writer_string_field(&writer, "reason", reason) ||
      !logger_json_object_writer_finish(&writer)) {
    return;
  }
  (void)logger_system_log_append(
      system_log, updated_at_utc_or_null, "session_pruned",
      LOGGER_SYSTEM_LOG_SEVERITY_INFO, logger_json_object_writer_data(&writer));
}

static void
logger_log_upload_interrupted(logger_system_log_t *system_log,
                              const char *updated_at_utc_or_null,
                              const logger_upload_queue_entry_t *entry) {
  if (system_log == NULL || entry == NULL) {
    return;
  }
  char details[LOGGER_SYSTEM_LOG_DETAILS_JSON_MAX + 1];
  logger_json_object_writer_t writer;
  logger_json_object_writer_init(&writer, details, sizeof(details));
  if (!logger_json_object_writer_string_field(&writer, "session_id",
                                              entry->session_id) ||
      !logger_json_object_writer_finish(&writer)) {
    return;
  }
  (void)logger_system_log_append(
      system_log, updated_at_utc_or_null, "upload_interrupted_recovered",
      LOGGER_SYSTEM_LOG_SEVERITY_WARN, logger_json_object_writer_data(&writer));
}

static void
logger_queue_entry_mark_interrupted(logger_upload_queue_entry_t *entry,
                                    logger_system_log_t *system_log,
                                    const char *updated_at_utc_or_null) {
  if (entry == NULL) {
    return;
  }
  logger_copy_string(entry->status, sizeof(entry->status), "failed");
  logger_copy_string(entry->last_failure_class,
                     sizeof(entry->last_failure_class), "interrupted");
  entry->last_http_status = 0u;
  entry->last_server_error_code[0] = '\0';
  entry->last_server_error_message[0] = '\0';
  entry->last_response_excerpt[0] = '\0';
  entry->verified_upload_utc[0] = '\0';
  entry->verified_bundle_sha256[0] = '\0';
  entry->receipt_id[0] = '\0';
  logger_log_upload_interrupted(system_log, updated_at_utc_or_null, entry);
}

static bool logger_queue_status_valid(const char *status) {
  return strcmp(status, "pending") == 0 || strcmp(status, "uploading") == 0 ||
         strcmp(status, "verified") == 0 ||
         strcmp(status, "blocked_min_firmware") == 0 ||
         strcmp(status, "nonretryable") == 0 || strcmp(status, "failed") == 0;
}

static bool logger_queue_status_is_protected(const char *status) {
  return strcmp(status, "verified") == 0 ||
         strcmp(status, "blocked_min_firmware") == 0 ||
         strcmp(status, "nonretryable") == 0;
}

static bool
logger_queue_entry_immutable_match(const logger_upload_queue_entry_t *a,
                                   const logger_upload_queue_entry_t *b) {
  return a != NULL && b != NULL && strcmp(a->session_id, b->session_id) == 0 &&
         strcmp(a->study_day_local, b->study_day_local) == 0 &&
         strcmp(a->dir_name, b->dir_name) == 0 &&
         strcmp(a->session_start_utc, b->session_start_utc) == 0 &&
         strcmp(a->session_end_utc, b->session_end_utc) == 0 &&
         strcmp(a->bundle_sha256, b->bundle_sha256) == 0 &&
         a->bundle_size_bytes == b->bundle_size_bytes &&
         a->quarantined == b->quarantined;
}

static void
logger_queue_copy_mutable_fields(logger_upload_queue_entry_t *dst,
                                 const logger_upload_queue_entry_t *src) {
  logger_copy_string(dst->status, sizeof(dst->status), src->status);
  dst->attempt_count = src->attempt_count;
  logger_copy_string(dst->last_attempt_utc, sizeof(dst->last_attempt_utc),
                     src->last_attempt_utc);
  logger_copy_string(dst->last_failure_class, sizeof(dst->last_failure_class),
                     src->last_failure_class);
  dst->last_http_status = src->last_http_status;
  logger_copy_string(dst->last_server_error_code,
                     sizeof(dst->last_server_error_code),
                     src->last_server_error_code);
  logger_copy_string(dst->last_server_error_message,
                     sizeof(dst->last_server_error_message),
                     src->last_server_error_message);
  logger_copy_string(dst->last_response_excerpt,
                     sizeof(dst->last_response_excerpt),
                     src->last_response_excerpt);
  logger_copy_string(dst->verified_upload_utc, sizeof(dst->verified_upload_utc),
                     src->verified_upload_utc);
  logger_copy_string(dst->verified_bundle_sha256,
                     sizeof(dst->verified_bundle_sha256),
                     src->verified_bundle_sha256);
  logger_copy_string(dst->receipt_id, sizeof(dst->receipt_id), src->receipt_id);
}

static void logger_queue_copy_optional_string(const logger_json_doc_t *doc,
                                              const jsmntok_t *object_tok,
                                              const char *key, char *out,
                                              size_t out_len) {
  if (out == NULL || out_len == 0u) {
    return;
  }
  out[0] = '\0';
  const jsmntok_t *tok = logger_json_object_get(doc, object_tok, key);
  if (tok == NULL || logger_json_token_is_null(doc, tok)) {
    return;
  }
  (void)logger_json_token_copy_string(doc, tok, out, out_len);
}

static void
logger_queue_copy_optional_http_status(const logger_json_doc_t *doc,
                                       const jsmntok_t *object_tok,
                                       logger_upload_queue_entry_t *entry) {
  entry->last_http_status = 0u;
  const jsmntok_t *tok =
      logger_json_object_get(doc, object_tok, "last_http_status");
  if (tok == NULL || logger_json_token_is_null(doc, tok)) {
    return;
  }
  uint32_t value = 0u;
  if (logger_json_token_get_uint32(doc, tok, &value) && value <= 999u) {
    entry->last_http_status = (uint16_t)value;
  }
}

static bool
logger_queue_entry_dir_exists(const logger_upload_queue_entry_t *entry) {
  if (entry == NULL || !logger_string_present(entry->dir_name)) {
    return false;
  }
  queue_stat_workspace_t *workspace = logger_queue_stat_workspace_acquire();
  if (!logger_path_join2(workspace->path, sizeof(workspace->path),
                         LOGGER_SESSIONS_DIR "/", entry->dir_name)) {
    logger_queue_stat_workspace_release(workspace);
    return false;
  }
  memset(&workspace->info, 0, sizeof(workspace->info));
  const bool exists = f_stat(workspace->path, &workspace->info) == FR_OK &&
                      (workspace->info.fattrib & AM_DIR) != 0u;
  logger_queue_stat_workspace_release(workspace);
  return exists;
}

static bool logger_parse_queue_entry_json(const logger_json_doc_t *doc,
                                          const jsmntok_t *object_tok,
                                          logger_upload_queue_entry_t *entry) {
  memset(entry, 0, sizeof(*entry));
  uint32_t attempt_count = 0u;

  if (doc == NULL || object_tok == NULL || object_tok->type != JSMN_OBJECT) {
    return false;
  }

  if (!logger_json_object_copy_string(doc, object_tok, "session_id",
                                      entry->session_id,
                                      sizeof(entry->session_id)) ||
      !logger_json_object_copy_string(doc, object_tok, "study_day_local",
                                      entry->study_day_local,
                                      sizeof(entry->study_day_local)) ||
      !logger_json_object_copy_string(doc, object_tok, "dir_name",
                                      entry->dir_name,
                                      sizeof(entry->dir_name)) ||
      !logger_json_object_copy_string_or_null(
          doc, object_tok, "session_start_utc", entry->session_start_utc,
          sizeof(entry->session_start_utc)) ||
      !logger_json_object_copy_string_or_null(
          doc, object_tok, "session_end_utc", entry->session_end_utc,
          sizeof(entry->session_end_utc)) ||
      !logger_json_object_copy_string(doc, object_tok, "bundle_sha256",
                                      entry->bundle_sha256,
                                      sizeof(entry->bundle_sha256)) ||
      !logger_json_object_get_uint64(doc, object_tok, "bundle_size_bytes",
                                     &entry->bundle_size_bytes) ||
      !logger_json_object_get_bool(doc, object_tok, "quarantined",
                                   &entry->quarantined) ||
      !logger_json_object_copy_string(doc, object_tok, "status", entry->status,
                                      sizeof(entry->status)) ||
      !logger_json_object_get_uint32(doc, object_tok, "attempt_count",
                                     &attempt_count) ||
      !logger_json_object_copy_string_or_null(
          doc, object_tok, "last_attempt_utc", entry->last_attempt_utc,
          sizeof(entry->last_attempt_utc)) ||
      !logger_json_object_copy_string_or_null(
          doc, object_tok, "last_failure_class", entry->last_failure_class,
          sizeof(entry->last_failure_class)) ||
      !logger_json_object_copy_string_or_null(
          doc, object_tok, "verified_upload_utc", entry->verified_upload_utc,
          sizeof(entry->verified_upload_utc)) ||
      !logger_json_object_copy_string_or_null(doc, object_tok, "receipt_id",
                                              entry->receipt_id,
                                              sizeof(entry->receipt_id))) {
    return false;
  }

  if (!logger_queue_status_valid(entry->status)) {
    return false;
  }

  entry->attempt_count = attempt_count;
  logger_queue_copy_optional_http_status(doc, object_tok, entry);
  logger_queue_copy_optional_string(doc, object_tok, "last_server_error_code",
                                    entry->last_server_error_code,
                                    sizeof(entry->last_server_error_code));
  logger_queue_copy_optional_string(doc, object_tok,
                                    "last_server_error_message",
                                    entry->last_server_error_message,
                                    sizeof(entry->last_server_error_message));
  logger_queue_copy_optional_string(doc, object_tok, "last_response_excerpt",
                                    entry->last_response_excerpt,
                                    sizeof(entry->last_response_excerpt));
  logger_queue_copy_optional_string(doc, object_tok, "verified_bundle_sha256",
                                    entry->verified_bundle_sha256,
                                    sizeof(entry->verified_bundle_sha256));
  return true;
}

static int
logger_upload_queue_entry_compare(const logger_upload_queue_entry_t *a,
                                  const logger_upload_queue_entry_t *b) {
  int cmp = strcmp(a->study_day_local, b->study_day_local);
  if (cmp != 0) {
    return cmp;
  }
  cmp = strcmp(a->session_start_utc, b->session_start_utc);
  if (cmp != 0) {
    return cmp;
  }
  return strcmp(a->session_id, b->session_id);
}

static void logger_upload_queue_sort(logger_upload_queue_t *queue) {
  for (size_t i = 1u; i < queue->session_count; ++i) {
    size_t j = i;
    while (j > 0u && logger_upload_queue_entry_compare(
                         &queue->sessions[j], &queue->sessions[j - 1u]) < 0) {
      unsigned char *const a = (unsigned char *)&queue->sessions[j];
      unsigned char *const b = (unsigned char *)&queue->sessions[j - 1u];
      for (size_t k = 0u; k < sizeof(queue->sessions[j]); ++k) {
        const unsigned char tmp = a[k];
        a[k] = b[k];
        b[k] = tmp;
      }
      --j;
    }
  }
}

static void logger_upload_queue_remove_at(logger_upload_queue_t *queue,
                                          size_t index) {
  if (queue == NULL || index >= queue->session_count) {
    return;
  }
  for (size_t i = index + 1u; i < queue->session_count; ++i) {
    queue->sessions[i - 1u] = queue->sessions[i];
  }
  if (queue->session_count > 0u) {
    queue->session_count -= 1u;
    memset(&queue->sessions[queue->session_count], 0,
           sizeof(queue->sessions[queue->session_count]));
  }
}

static bool logger_parse_rfc3339_utc_seconds(const char *text,
                                             int64_t *seconds_out) {
  if (text == NULL || seconds_out == NULL || strlen(text) != 20u) {
    return false;
  }

  int year = 0;
  int month = 0;
  int day = 0;
  int hour = 0;
  int minute = 0;
  int second = 0;
  if (sscanf(text, "%4d-%2d-%2dT%2d:%2d:%2dZ", &year, &month, &day, &hour,
             &minute, &second) != 6) {
    return false;
  }
  if (month < 1 || month > 12 || day < 1 || day > 31 || hour < 0 || hour > 23 ||
      minute < 0 || minute > 59 || second < 0 || second > 60) {
    return false;
  }

  const int64_t days = logger_days_from_civil(year, month, day);
  *seconds_out = (days * 86400) + ((int64_t)hour * 3600) +
                 ((int64_t)minute * 60) + (int64_t)second;
  return true;
}

static bool
logger_queue_entry_retention_expired(const logger_upload_queue_entry_t *entry,
                                     const char *now_utc_or_null) {
  if (entry == NULL || strcmp(entry->status, "verified") != 0) {
    return false;
  }
  int64_t now_seconds = 0;
  int64_t verified_seconds = 0;
  if (!logger_parse_rfc3339_utc_seconds(now_utc_or_null, &now_seconds) ||
      !logger_parse_rfc3339_utc_seconds(entry->verified_upload_utc,
                                        &verified_seconds)) {
    return false;
  }
  return now_seconds >= verified_seconds &&
         (uint64_t)(now_seconds - verified_seconds) >=
             (uint64_t)LOGGER_UPLOAD_RETENTION_SECONDS;
}

static int
logger_upload_queue_find_oldest_verified(const logger_upload_queue_t *queue) {
  if (queue == NULL) {
    return -1;
  }
  for (size_t i = 0u; i < queue->session_count; ++i) {
    if (strcmp(queue->sessions[i].status, "verified") == 0) {
      return (int)i;
    }
  }
  return -1;
}

static bool logger_remove_closed_session_dir(const char *dir_name) {
  if (!logger_string_present(dir_name)) {
    return false;
  }

  queue_delete_workspace_t *workspace = logger_queue_delete_workspace_acquire();

  if (!logger_path_join2(workspace->dir_path, sizeof(workspace->dir_path),
                         LOGGER_SESSIONS_DIR "/", dir_name)) {
    logger_queue_delete_workspace_release(workspace);
    return false;
  }

  if (f_opendir(&workspace->dir, workspace->dir_path) != FR_OK) {
    logger_queue_delete_workspace_release(workspace);
    return false;
  }

  bool ok = true;
  for (;;) {
    memset(&workspace->info, 0, sizeof(workspace->info));
    const FRESULT fr = f_readdir(&workspace->dir, &workspace->info);
    if (fr != FR_OK) {
      ok = false;
      break;
    }
    if (workspace->info.fname[0] == '\0') {
      break;
    }
    if (strcmp(workspace->info.fname, ".") == 0 ||
        strcmp(workspace->info.fname, "..") == 0) {
      continue;
    }
    if ((workspace->info.fattrib & AM_DIR) != 0u) {
      ok = false;
      break;
    }

    if (!logger_path_join3(workspace->child_path, sizeof(workspace->child_path),
                           workspace->dir_path, "/", workspace->info.fname) ||
        f_unlink(workspace->child_path) != FR_OK) {
      ok = false;
      break;
    }
  }
  if (f_closedir(&workspace->dir) != FR_OK) {
    ok = false;
  }
  if (!ok) {
    logger_queue_delete_workspace_release(workspace);
    return false;
  }
  const bool removed = f_unlink(workspace->dir_path) == FR_OK;
  logger_queue_delete_workspace_release(workspace);
  return removed;
}

static void __attribute__((noinline)) logger_upload_queue_merge_scanned_entries(
    logger_upload_queue_t *merged, const logger_upload_queue_t *scanned,
    const logger_upload_queue_t *previous, bool loaded_previous,
    bool *previous_seen, logger_system_log_t *system_log,
    const char *updated_at_utc_or_null) {
  for (size_t i = 0u; i < scanned->session_count; ++i) {
    if (merged->session_count >= LOGGER_UPLOAD_QUEUE_MAX_SESSIONS) {
      break;
    }

    logger_upload_queue_entry_t *entry =
        &merged->sessions[merged->session_count];
    *entry = scanned->sessions[i];

    if (loaded_previous) {
      for (size_t j = 0u; j < previous->session_count; ++j) {
        if (strcmp(previous->sessions[j].session_id, entry->session_id) == 0) {
          previous_seen[j] = true;
          if (logger_queue_entry_immutable_match(entry,
                                                 &previous->sessions[j])) {
            logger_queue_copy_mutable_fields(entry, &previous->sessions[j]);
            if (strcmp(entry->status, "uploading") == 0) {
              logger_queue_entry_mark_interrupted(entry, system_log,
                                                  updated_at_utc_or_null);
            }
          } else {
            logger_log_local_corrupt(system_log, updated_at_utc_or_null,
                                     entry->dir_name,
                                     "queue_entry_identity_changed");
            if (logger_queue_status_is_protected(
                    previous->sessions[j].status)) {
              *entry = previous->sessions[j];
            } else {
              logger_copy_string(entry->status, sizeof(entry->status),
                                 "nonretryable");
              logger_copy_string(entry->last_failure_class,
                                 sizeof(entry->last_failure_class),
                                 "local_corrupt");
              entry->last_http_status = 0u;
              entry->last_server_error_code[0] = '\0';
              entry->last_server_error_message[0] = '\0';
              entry->last_response_excerpt[0] = '\0';
              entry->verified_upload_utc[0] = '\0';
              entry->verified_bundle_sha256[0] = '\0';
              entry->receipt_id[0] = '\0';
            }
          }
          break;
        }
      }
    }

    merged->session_count += 1u;
  }
}

static void __attribute__((noinline))
logger_upload_queue_log_unreconciled_previous(
    const logger_upload_queue_t *previous, const bool *previous_seen,
    logger_system_log_t *system_log, const char *updated_at_utc_or_null) {
  for (size_t i = 0u; i < previous->session_count; ++i) {
    if (previous_seen[i]) {
      continue;
    }
    if (logger_queue_entry_dir_exists(&previous->sessions[i])) {
      logger_log_local_corrupt(system_log, updated_at_utc_or_null,
                               previous->sessions[i].dir_name,
                               "queue_entry_unreconciled");
    } else {
      logger_log_queue_missing_local(system_log, updated_at_utc_or_null,
                                     &previous->sessions[i]);
    }
  }
}

static bool logger_upload_queue_merge_scan_with_previous(
    logger_upload_queue_t *merged, const logger_upload_queue_t *scanned,
    logger_upload_queue_t *previous, bool *previous_seen,
    logger_system_log_t *system_log, const char *updated_at_utc_or_null) {
  const logger_upload_queue_load_result_t load_result =
      logger_upload_queue_load_internal(previous);
  if (load_result == LOGGER_UPLOAD_QUEUE_LOAD_IO_ERROR) {
    return false;
  }
  const bool loaded_previous = load_result == LOGGER_UPLOAD_QUEUE_LOAD_LOADED;
  if (load_result == LOGGER_UPLOAD_QUEUE_LOAD_CORRUPT) {
    logger_log_queue_rebuilt(system_log, updated_at_utc_or_null, "load_failed");
  }

  logger_copy_string(merged->updated_at_utc, sizeof(merged->updated_at_utc),
                     updated_at_utc_or_null);
  logger_upload_queue_merge_scanned_entries(merged, scanned, previous,
                                            loaded_previous, previous_seen,
                                            system_log, updated_at_utc_or_null);
  if (loaded_previous) {
    logger_upload_queue_log_unreconciled_previous(
        previous, previous_seen, system_log, updated_at_utc_or_null);
  }

  logger_upload_queue_sort(merged);
  return logger_upload_queue_write(merged);
}

static bool __attribute__((noinline))
logger_file_write_all(logger_queue_file_writer_t *writer, const void *data,
                      size_t len) {
  if (writer == NULL || writer->file == NULL || data == NULL) {
    return false;
  }
  if (writer->bytes_written > writer->limit ||
      len > (writer->limit - writer->bytes_written)) {
    return false;
  }
  if (len == 0u) {
    return true;
  }
  UINT written = 0u;
  if (f_write(writer->file, data, len, &written) != FR_OK || written != len) {
    return false;
  }
  if (writer->sha != NULL) {
    logger_sha256_update(writer->sha, data, len);
  }
  writer->bytes_written += len;
  return true;
}

static bool __attribute__((noinline))
logger_file_write_cstr(logger_queue_file_writer_t *writer, const char *text) {
  return text != NULL && logger_file_write_all(writer, text, strlen(text));
}

static bool __attribute__((noinline))
logger_file_write_json_string_or_null(logger_queue_file_writer_t *writer,
                                      const char *value) {
  if (value == NULL || value[0] == '\0') {
    return logger_file_write_cstr(writer, "null");
  }
  if (!logger_file_write_cstr(writer, "\"")) {
    return false;
  }
  const char *run = value;
  for (const unsigned char *p = (const unsigned char *)value; *p != '\0'; ++p) {
    const char *replacement = NULL;
    char unicode_buf[7];
    switch (*p) {
    case '\\':
      replacement = "\\\\";
      break;
    case '"':
      replacement = "\\\"";
      break;
    case '\b':
      replacement = "\\b";
      break;
    case '\f':
      replacement = "\\f";
      break;
    case '\n':
      replacement = "\\n";
      break;
    case '\r':
      replacement = "\\r";
      break;
    case '\t':
      replacement = "\\t";
      break;
    default:
      if (*p < 0x20u) {
        const int n = snprintf(unicode_buf, sizeof(unicode_buf), "\\u%04x", *p);
        if (n != 6) {
          return false;
        }
        replacement = unicode_buf;
      } else {
        continue;
      }
      break;
    }

    const char *escaped_at = (const char *)p;
    if (escaped_at > run &&
        !logger_file_write_all(writer, run, (size_t)(escaped_at - run))) {
      return false;
    }
    if (!logger_file_write_cstr(writer, replacement)) {
      return false;
    }
    run = escaped_at + 1u;
  }
  if (run[0] != '\0' && !logger_file_write_cstr(writer, run)) {
    return false;
  }
  return logger_file_write_cstr(writer, "\"");
}

const logger_upload_queue_entry_t *
logger_upload_queue_find_by_session_id(const logger_upload_queue_t *queue,
                                       const char *session_id) {
  if (queue == NULL || !logger_string_present(session_id)) {
    return NULL;
  }
  for (size_t i = 0u; i < queue->session_count; ++i) {
    if (strcmp(queue->sessions[i].session_id, session_id) == 0) {
      return &queue->sessions[i];
    }
  }
  return NULL;
}

void logger_upload_queue_init(logger_upload_queue_t *queue) {
  memset(queue, 0, sizeof(*queue));
}

void logger_upload_queue_summary_init(logger_upload_queue_summary_t *summary) {
  memset(summary, 0, sizeof(*summary));
}

void logger_upload_queue_compute_summary(
    const logger_upload_queue_t *queue,
    logger_upload_queue_summary_t *summary) {
  logger_upload_queue_summary_init(summary);
  if (queue == NULL) {
    return;
  }

  summary->available = true;
  logger_copy_string(summary->updated_at_utc, sizeof(summary->updated_at_utc),
                     queue->updated_at_utc);
  summary->session_count = (uint32_t)queue->session_count;

  char latest_failure_at[LOGGER_UPLOAD_QUEUE_UTC_MAX + 1] = {0};
  bool have_failure = false;
  for (size_t i = 0u; i < queue->session_count; ++i) {
    const logger_upload_queue_entry_t *entry = &queue->sessions[i];
    if (strcmp(entry->status, "blocked_min_firmware") == 0) {
      summary->blocked_count += 1u;
    }
    if (strcmp(entry->status, "pending") == 0 ||
        strcmp(entry->status, "failed") == 0 ||
        strcmp(entry->status, "uploading") == 0) {
      if (summary->pending_count == 0u) {
        logger_copy_string(summary->oldest_pending_study_day,
                           sizeof(summary->oldest_pending_study_day),
                           entry->study_day_local);
      }
      summary->pending_count += 1u;
    }
    if (logger_string_present(entry->last_failure_class)) {
      if (!have_failure ||
          (!logger_string_present(latest_failure_at) &&
           logger_string_present(entry->last_attempt_utc)) ||
          strcmp(entry->last_attempt_utc, latest_failure_at) >= 0) {
        logger_copy_string(summary->last_failure_class,
                           sizeof(summary->last_failure_class),
                           entry->last_failure_class);
        logger_copy_string(latest_failure_at, sizeof(latest_failure_at),
                           entry->last_attempt_utc);
        have_failure = true;
      }
    }
  }
}

size_t
logger_upload_queue_recover_interrupted(logger_upload_queue_t *queue,
                                        logger_system_log_t *system_log,
                                        const char *updated_at_utc_or_null) {
  if (queue == NULL) {
    return 0u;
  }

  size_t recovered_count = 0u;
  for (size_t i = 0u; i < queue->session_count; ++i) {
    logger_upload_queue_entry_t *entry = &queue->sessions[i];
    if (strcmp(entry->status, "uploading") != 0) {
      continue;
    }
    logger_queue_entry_mark_interrupted(entry, system_log,
                                        updated_at_utc_or_null);
    recovered_count += 1u;
  }
  if (recovered_count > 0u) {
    logger_copy_string(queue->updated_at_utc, sizeof(queue->updated_at_utc),
                       updated_at_utc_or_null);
  }
  return recovered_count;
}

static bool logger_upload_queue_parse_json(logger_upload_queue_t *queue,
                                           size_t len) {
  logger_upload_queue_init(queue);
  g_queue_scratch->load.json[len] = '\0';

  logger_json_doc_t doc;
  if (!logger_json_parse(&doc, g_queue_scratch->load.json, len,
                         g_queue_scratch->load.tokens,
                         LOGGER_QUEUE_JSON_TOKEN_MAX)) {
    logger_upload_queue_init(queue);
    return false;
  }

  const jsmntok_t *root = logger_json_root(&doc);
  if (root == NULL || root->type != JSMN_OBJECT) {
    logger_upload_queue_init(queue);
    return false;
  }

  uint32_t schema_version = 0u;
  if (!logger_json_object_get_uint32(&doc, root, "schema_version",
                                     &schema_version) ||
      schema_version != 1u) {
    logger_upload_queue_init(queue);
    return false;
  }

  if (!logger_json_object_copy_string_or_null(&doc, root, "updated_at_utc",
                                              queue->updated_at_utc,
                                              sizeof(queue->updated_at_utc))) {
    logger_upload_queue_init(queue);
    return false;
  }

  const jsmntok_t *sessions_tok =
      logger_json_object_get(&doc, root, "sessions");
  if (sessions_tok == NULL || sessions_tok->type != JSMN_ARRAY ||
      (size_t)sessions_tok->size > LOGGER_UPLOAD_QUEUE_MAX_SESSIONS) {
    logger_upload_queue_init(queue);
    return false;
  }

  for (size_t i = 0u; i < (size_t)sessions_tok->size; ++i) {
    const jsmntok_t *entry_tok = logger_json_array_get(&doc, sessions_tok, i);
    if (entry_tok == NULL || entry_tok->type != JSMN_OBJECT ||
        queue->session_count >= LOGGER_UPLOAD_QUEUE_MAX_SESSIONS) {
      logger_upload_queue_init(queue);
      return false;
    }
    logger_upload_queue_entry_t *entry = &queue->sessions[queue->session_count];
    if (!logger_parse_queue_entry_json(&doc, entry_tok, entry) ||
        logger_upload_queue_find_by_session_id(queue, entry->session_id) !=
            NULL) {
      logger_upload_queue_init(queue);
      return false;
    }

    queue->session_count += 1u;
  }

  logger_upload_queue_sort(queue);
  return true;
}

static logger_queue_slot_read_result_t
logger_queue_slot_read_payload(const char *path, bool *present_out,
                               logger_queue_slot_meta_t *meta_out) {
  if (present_out != NULL) {
    *present_out = false;
  }
  if (meta_out != NULL) {
    memset(meta_out, 0, sizeof(*meta_out));
  }

  assert(g_queue_scratch != NULL);
  FIL *file = &g_queue_scratch->load.file;
  FRESULT fr = f_open(file, path, FA_READ);
  if (fr == FR_NO_FILE || fr == FR_NO_PATH) {
    return LOGGER_QUEUE_SLOT_ABSENT;
  }
  if (fr != FR_OK) {
    return LOGGER_QUEUE_SLOT_IO_ERROR;
  }
  if (present_out != NULL) {
    *present_out = true;
  }

  logger_queue_slot_read_result_t result = LOGGER_QUEUE_SLOT_IO_ERROR;
  logger_queue_slot_meta_t header_meta;
  logger_queue_slot_meta_t trailer_meta;
  memset(&header_meta, 0, sizeof(header_meta));
  memset(&trailer_meta, 0, sizeof(trailer_meta));

  if (f_size(file) != (FSIZE_t)LOGGER_QUEUE_SLOT_FILE_BYTES) {
    result = LOGGER_QUEUE_SLOT_CORRUPT;
  } else if (f_lseek(file, 0u) != FR_OK ||
             !logger_queue_file_read_exact(file, g_queue_scratch->load.header,
                                           LOGGER_QUEUE_SLOT_HEADER_BYTES)) {
    result = LOGGER_QUEUE_SLOT_IO_ERROR;
  } else if (!logger_queue_slot_metadata_read(g_queue_scratch->load.header,
                                              LOGGER_QUEUE_SLOT_HEADER_MAGIC,
                                              &header_meta)) {
    result = LOGGER_QUEUE_SLOT_CORRUPT;
  } else if (f_lseek(file, LOGGER_QUEUE_SLOT_TRAILER_OFFSET) != FR_OK ||
             !logger_queue_file_read_exact(file, g_queue_scratch->load.trailer,
                                           LOGGER_QUEUE_SLOT_TRAILER_BYTES)) {
    result = LOGGER_QUEUE_SLOT_IO_ERROR;
  } else if (!logger_queue_slot_metadata_read(g_queue_scratch->load.trailer,
                                              LOGGER_QUEUE_SLOT_TRAILER_MAGIC,
                                              &trailer_meta) ||
             !logger_queue_slot_meta_match(&header_meta, &trailer_meta)) {
    result = LOGGER_QUEUE_SLOT_CORRUPT;
  } else if (f_lseek(file, LOGGER_QUEUE_SLOT_PAYLOAD_OFFSET) != FR_OK ||
             !logger_queue_file_read_exact(file, g_queue_scratch->load.json,
                                           header_meta.payload_len)) {
    result = LOGGER_QUEUE_SLOT_IO_ERROR;
  } else {
    logger_sha256_t sha;
    uint8_t digest[LOGGER_SHA256_BYTES];
    logger_queue_slot_hash_begin(&sha, header_meta.generation);
    if (header_meta.payload_len > 0u) {
      logger_sha256_update(&sha, g_queue_scratch->load.json,
                           header_meta.payload_len);
    }
    logger_sha256_final(&sha, digest);
    result =
        memcmp(digest, header_meta.payload_sha256, LOGGER_SHA256_BYTES) == 0
            ? LOGGER_QUEUE_SLOT_VALID
            : LOGGER_QUEUE_SLOT_CORRUPT;
  }

  const FRESULT close_fr = f_close(file);
  if (close_fr != FR_OK) {
    return LOGGER_QUEUE_SLOT_IO_ERROR;
  }
  if (result == LOGGER_QUEUE_SLOT_VALID && meta_out != NULL) {
    *meta_out = header_meta;
  }
  return result;
}

static bool logger_queue_slot_probe(const char *path,
                                    logger_queue_slot_probe_t *probe,
                                    logger_upload_queue_t *parse_queue) {
  memset(probe, 0, sizeof(*probe));
  probe->path = path;

  logger_queue_slot_meta_t meta;
  bool present = false;
  const logger_queue_slot_read_result_t result =
      logger_queue_slot_read_payload(path, &present, &meta);
  probe->result = result;
  probe->present = present;
  probe->integrity_valid = result == LOGGER_QUEUE_SLOT_VALID;
  probe->generation = probe->integrity_valid ? meta.generation : 0u;
  if (probe->integrity_valid && parse_queue != NULL) {
    probe->queue_valid =
        logger_upload_queue_parse_json(parse_queue, meta.payload_len);
  }
  return result != LOGGER_QUEUE_SLOT_IO_ERROR;
}

static bool logger_queue_slots_probe(logger_queue_slot_probe_t *a,
                                     logger_queue_slot_probe_t *b,
                                     logger_upload_queue_t *parse_queue) {
  return logger_queue_slot_probe(LOGGER_QUEUE_SLOT_A_PATH, a, parse_queue) &&
         logger_queue_slot_probe(LOGGER_QUEUE_SLOT_B_PATH, b, parse_queue);
}

static logger_queue_slot_read_result_t
logger_upload_queue_load_from_slot(logger_upload_queue_t *queue,
                                   const char *path) {
  bool present = false;
  logger_queue_slot_meta_t meta;
  const logger_queue_slot_read_result_t result =
      logger_queue_slot_read_payload(path, &present, &meta);
  if (result != LOGGER_QUEUE_SLOT_VALID) {
    logger_upload_queue_init(queue);
    return result;
  }
  if (!logger_upload_queue_parse_json(queue, meta.payload_len)) {
    logger_upload_queue_init(queue);
    return LOGGER_QUEUE_SLOT_CORRUPT;
  }
  return LOGGER_QUEUE_SLOT_VALID;
}

static logger_upload_queue_load_result_t
logger_upload_queue_load_internal(logger_upload_queue_t *queue) {
  logger_storage_status_t storage;
  (void)logger_storage_refresh(&storage);
  if (!storage.mounted) {
    logger_upload_queue_init(queue);
    return LOGGER_UPLOAD_QUEUE_LOAD_IO_ERROR;
  }

  logger_queue_slot_probe_t a;
  logger_queue_slot_probe_t b;
  if (!logger_queue_slots_probe(&a, &b, NULL)) {
    logger_upload_queue_init(queue);
    return LOGGER_UPLOAD_QUEUE_LOAD_IO_ERROR;
  }

  const bool any_present = a.present || b.present;
  if (!a.integrity_valid && !b.integrity_valid) {
    logger_upload_queue_init(queue);
    return any_present ? LOGGER_UPLOAD_QUEUE_LOAD_CORRUPT
                       : LOGGER_UPLOAD_QUEUE_LOAD_EMPTY;
  }

  const logger_queue_slot_probe_t *first = &a;
  const logger_queue_slot_probe_t *second = &b;
  if (b.integrity_valid &&
      (!a.integrity_valid || b.generation > a.generation)) {
    first = &b;
    second = &a;
  }

  logger_queue_slot_read_result_t load_result = LOGGER_QUEUE_SLOT_CORRUPT;
  if (first->integrity_valid) {
    load_result = logger_upload_queue_load_from_slot(queue, first->path);
    if (load_result == LOGGER_QUEUE_SLOT_VALID) {
      return LOGGER_UPLOAD_QUEUE_LOAD_LOADED;
    }
    if (load_result == LOGGER_QUEUE_SLOT_IO_ERROR) {
      logger_upload_queue_init(queue);
      return LOGGER_UPLOAD_QUEUE_LOAD_IO_ERROR;
    }
  }
  if (second->integrity_valid) {
    load_result = logger_upload_queue_load_from_slot(queue, second->path);
    if (load_result == LOGGER_QUEUE_SLOT_VALID) {
      return LOGGER_UPLOAD_QUEUE_LOAD_LOADED;
    }
    if (load_result == LOGGER_QUEUE_SLOT_IO_ERROR) {
      logger_upload_queue_init(queue);
      return LOGGER_UPLOAD_QUEUE_LOAD_IO_ERROR;
    }
  }

  logger_upload_queue_init(queue);
  return LOGGER_UPLOAD_QUEUE_LOAD_CORRUPT;
}

bool logger_upload_queue_load(logger_upload_queue_t *queue) {
  const logger_upload_queue_load_result_t result =
      logger_upload_queue_load_internal(queue);
  return result == LOGGER_UPLOAD_QUEUE_LOAD_LOADED ||
         result == LOGGER_UPLOAD_QUEUE_LOAD_EMPTY;
}

bool logger_upload_queue_scan(logger_upload_queue_t *queue,
                              logger_system_log_t *system_log,
                              const char *updated_at_utc_or_null) {
  assert(g_queue_scratch != NULL);
  queue_scan_workspace_t *scan_workspace =
      logger_queue_scan_workspace_acquire();
  logger_upload_queue_init(queue);

  (void)logger_storage_refresh(&scan_workspace->storage);
  if (!scan_workspace->storage.mounted) {
    logger_queue_scan_workspace_release(scan_workspace);
    return false;
  }

  logger_copy_string(queue->updated_at_utc, sizeof(queue->updated_at_utc),
                     updated_at_utc_or_null);

  if (f_opendir(&scan_workspace->dir, LOGGER_SESSIONS_DIR) != FR_OK) {
    logger_queue_scan_workspace_release(scan_workspace);
    return false;
  }

  bool ok = true;
  for (;;) {
    memset(&scan_workspace->info, 0, sizeof(scan_workspace->info));
    const FRESULT fr = f_readdir(&scan_workspace->dir, &scan_workspace->info);
    if (fr != FR_OK) {
      ok = false;
      break;
    }
    if (scan_workspace->info.fname[0] == '\0') {
      break;
    }
    if ((scan_workspace->info.fattrib & AM_DIR) == 0u ||
        strcmp(scan_workspace->info.fname, ".") == 0 ||
        strcmp(scan_workspace->info.fname, "..") == 0) {
      continue;
    }
    if (strstr(scan_workspace->info.fname, ".tmp") != NULL) {
      continue;
    }

    if (!logger_path_join3(scan_workspace->manifest_path,
                           sizeof(scan_workspace->manifest_path),
                           LOGGER_SESSIONS_DIR "/", scan_workspace->info.fname,
                           "/manifest.json") ||
        !logger_path_join3(scan_workspace->journal_path,
                           sizeof(scan_workspace->journal_path),
                           LOGGER_SESSIONS_DIR "/", scan_workspace->info.fname,
                           "/journal.bin") ||
        !logger_path_join3(scan_workspace->live_path,
                           sizeof(scan_workspace->live_path),
                           LOGGER_SESSIONS_DIR "/", scan_workspace->info.fname,
                           "/live.json")) {
      logger_log_local_corrupt(system_log, updated_at_utc_or_null,
                               scan_workspace->info.fname, "path_too_long");
      continue;
    }

    if (logger_storage_file_exists(scan_workspace->live_path)) {
      continue;
    }
    if (!logger_storage_file_exists(scan_workspace->manifest_path) ||
        !logger_storage_file_exists(scan_workspace->journal_path)) {
      continue;
    }
    if (queue->session_count >= LOGGER_UPLOAD_QUEUE_MAX_SESSIONS) {
      logger_log_local_corrupt(system_log, updated_at_utc_or_null,
                               scan_workspace->info.fname,
                               "queue_capacity_exhausted");
      break;
    }

    size_t manifest_len = 0u;
    if (!logger_storage_read_file(scan_workspace->manifest_path,
                                  g_queue_scratch->manifest.json,
                                  LOGGER_MANIFEST_READ_MAX, &manifest_len)) {
      logger_log_local_corrupt(system_log, updated_at_utc_or_null,
                               scan_workspace->info.fname,
                               "manifest_read_failed");
      continue;
    }
    g_queue_scratch->manifest.json[manifest_len] = '\0';

    if (!logger_parse_manifest_summary(g_queue_scratch->manifest.json,
                                       &scan_workspace->manifest)) {
      logger_log_local_corrupt(system_log, updated_at_utc_or_null,
                               scan_workspace->info.fname,
                               "manifest_parse_failed");
      continue;
    }

    logger_upload_queue_entry_t *entry = &queue->sessions[queue->session_count];
    memset(entry, 0, sizeof(*entry));
    logger_copy_string(entry->session_id, sizeof(entry->session_id),
                       scan_workspace->manifest.session_id);
    logger_copy_string(entry->study_day_local, sizeof(entry->study_day_local),
                       scan_workspace->manifest.study_day_local);
    logger_copy_string(entry->dir_name, sizeof(entry->dir_name),
                       scan_workspace->info.fname);
    logger_copy_string(entry->session_start_utc,
                       sizeof(entry->session_start_utc),
                       scan_workspace->manifest.session_start_utc);
    logger_copy_string(entry->session_end_utc, sizeof(entry->session_end_utc),
                       scan_workspace->manifest.session_end_utc);
    entry->quarantined = scan_workspace->manifest.quarantined;
    logger_copy_string(entry->status, sizeof(entry->status), "pending");

    if (!logger_upload_bundle_compute(
            scan_workspace->info.fname, scan_workspace->manifest_path,
            scan_workspace->journal_path, entry->bundle_sha256,
            &entry->bundle_size_bytes)) {
      logger_log_local_corrupt(system_log, updated_at_utc_or_null,
                               scan_workspace->info.fname,
                               "bundle_hash_failed");
      continue;
    }

    queue->session_count += 1u;
  }

  if (f_closedir(&scan_workspace->dir) != FR_OK) {
    ok = false;
  }
  logger_queue_scan_workspace_release(scan_workspace);
  if (!ok) {
    logger_upload_queue_init(queue);
    return false;
  }
  logger_upload_queue_sort(queue);
  return true;
}

static bool logger_queue_slot_prepare_file(FIL *file) {
  const FSIZE_t wanted = (FSIZE_t)LOGGER_QUEUE_SLOT_FILE_BYTES;
  const FSIZE_t size = f_size(file);
  if (size > wanted) {
    if (f_lseek(file, wanted) != FR_OK || f_truncate(file) != FR_OK) {
      return false;
    }
  } else if (size < wanted) {
    const uint8_t zero = 0u;
    if (f_lseek(file, wanted - 1u) != FR_OK ||
        !logger_queue_file_write_exact(file, &zero, sizeof(zero))) {
      return false;
    }
  }
  return true;
}

static bool
logger_queue_slot_choose_write_target(const char **path_out,
                                      uint64_t *generation_out,
                                      logger_upload_queue_t *parse_queue) {
  logger_queue_slot_probe_t a;
  logger_queue_slot_probe_t b;
  if (path_out == NULL || generation_out == NULL || parse_queue == NULL ||
      !logger_queue_slots_probe(&a, &b, parse_queue)) {
    return false;
  }

  uint64_t best_generation = 0u;
  const char *target = LOGGER_QUEUE_SLOT_A_PATH;
  /* The committed copy is the newest JSON-parse-valid slot, not merely the
   * newest header/trailer/SHA-valid slot.  A reset during write must never
   * invalidate the only parse-valid queue. */
  if (a.queue_valid && (!b.queue_valid || a.generation >= b.generation)) {
    best_generation = a.generation;
    target = LOGGER_QUEUE_SLOT_B_PATH;
  } else if (b.queue_valid) {
    best_generation = b.generation;
    target = LOGGER_QUEUE_SLOT_A_PATH;
  } else if (a.integrity_valid &&
             (!b.integrity_valid || a.generation >= b.generation)) {
    target = LOGGER_QUEUE_SLOT_A_PATH;
  } else if (b.integrity_valid) {
    target = LOGGER_QUEUE_SLOT_B_PATH;
  }
  if (best_generation == UINT64_MAX) {
    return false;
  }

  *path_out = target;
  *generation_out = best_generation + 1u;
  return true;
}

bool logger_upload_queue_write(const logger_upload_queue_t *queue) {
  if (queue == NULL ||
      queue->session_count > LOGGER_UPLOAD_QUEUE_MAX_SESSIONS) {
    return false;
  }

  logger_storage_status_t storage;
  (void)logger_storage_refresh(&storage);
  if (!storage.mounted || !storage.writable) {
    return false;
  }

  queue_write_workspace_t *workspace = logger_queue_write_workspace_acquire();
  const char *slot_path = NULL;
  uint64_t generation = 0u;
  if (!logger_queue_slot_choose_write_target(&slot_path, &generation,
                                             &workspace->probe_queue)) {
    logger_queue_write_workspace_release(workspace);
    return false;
  }

  FIL *file = &workspace->file;
  if (f_open(file, slot_path, FA_READ | FA_WRITE | FA_OPEN_ALWAYS) != FR_OK) {
    logger_queue_write_workspace_release(workspace);
    return false;
  }
  if (!logger_queue_slot_prepare_file(file)) {
    (void)f_close(file);
    logger_queue_write_workspace_release(workspace);
    return false;
  }

  assert(g_queue_scratch != NULL);
  memset(g_queue_scratch->load.header, 0, LOGGER_QUEUE_SLOT_HEADER_BYTES);
  if (f_lseek(file, 0u) != FR_OK ||
      !logger_queue_file_write_exact(file, g_queue_scratch->load.header,
                                     LOGGER_QUEUE_SLOT_HEADER_BYTES) ||
      f_lseek(file, LOGGER_QUEUE_SLOT_PAYLOAD_OFFSET) != FR_OK) {
    (void)f_close(file);
    logger_queue_write_workspace_release(workspace);
    return false;
  }

  logger_sha256_t sha;
  logger_queue_slot_hash_begin(&sha, generation);
  logger_queue_file_writer_t writer = {
      .file = file,
      .bytes_written = 0u,
      .limit = LOGGER_QUEUE_FILE_MAX,
      .sha = &sha,
  };

  bool ok = true;
  ok = ok && logger_file_write_cstr(
                 &writer, "{\"schema_version\":1,\"updated_at_utc\":");
  ok = ok &&
       logger_file_write_json_string_or_null(&writer, queue->updated_at_utc);
  ok = ok && logger_file_write_cstr(&writer, ",\"sessions\":[");

  for (size_t i = 0u; ok && i < queue->session_count; ++i) {
    const logger_upload_queue_entry_t *entry = &queue->sessions[i];
    char number_buf[32];

    ok = ok && logger_file_write_cstr(&writer, i == 0u ? "{" : ",{");

    ok = ok && logger_file_write_cstr(&writer, "\"session_id\":");
    ok =
        ok && logger_file_write_json_string_or_null(&writer, entry->session_id);
    ok = ok && logger_file_write_cstr(&writer, ",\"study_day_local\":");
    ok = ok &&
         logger_file_write_json_string_or_null(&writer, entry->study_day_local);
    ok = ok && logger_file_write_cstr(&writer, ",\"dir_name\":");
    ok = ok && logger_file_write_json_string_or_null(&writer, entry->dir_name);
    ok = ok && logger_file_write_cstr(&writer, ",\"session_start_utc\":");
    ok = ok && logger_file_write_json_string_or_null(&writer,
                                                     entry->session_start_utc);
    ok = ok && logger_file_write_cstr(&writer, ",\"session_end_utc\":");
    ok = ok &&
         logger_file_write_json_string_or_null(&writer, entry->session_end_utc);
    ok = ok && logger_file_write_cstr(&writer, ",\"bundle_sha256\":");
    ok = ok &&
         logger_file_write_json_string_or_null(&writer, entry->bundle_sha256);
    ok = ok && logger_file_write_cstr(&writer, ",\"bundle_size_bytes\":");
    const int bundle_n = snprintf(number_buf, sizeof(number_buf), "%llu",
                                  (unsigned long long)entry->bundle_size_bytes);
    ok = ok && bundle_n > 0 && (size_t)bundle_n < sizeof(number_buf) &&
         logger_file_write_all(&writer, number_buf, (size_t)bundle_n);
    ok = ok && logger_file_write_cstr(&writer, ",\"quarantined\":");
    ok = ok &&
         logger_file_write_cstr(&writer, entry->quarantined ? "true" : "false");
    ok = ok && logger_file_write_cstr(&writer, ",\"status\":");
    ok = ok && logger_file_write_json_string_or_null(&writer, entry->status);
    ok = ok && logger_file_write_cstr(&writer, ",\"attempt_count\":");
    const int attempt_n = snprintf(number_buf, sizeof(number_buf), "%lu",
                                   (unsigned long)entry->attempt_count);
    ok = ok && attempt_n > 0 && (size_t)attempt_n < sizeof(number_buf) &&
         logger_file_write_all(&writer, number_buf, (size_t)attempt_n);
    ok = ok && logger_file_write_cstr(&writer, ",\"last_attempt_utc\":");
    ok = ok && logger_file_write_json_string_or_null(&writer,
                                                     entry->last_attempt_utc);
    ok = ok && logger_file_write_cstr(&writer, ",\"last_failure_class\":");
    ok = ok && logger_file_write_json_string_or_null(&writer,
                                                     entry->last_failure_class);
    ok = ok && logger_file_write_cstr(&writer, ",\"last_http_status\":");
    if (entry->last_http_status > 0u) {
      const int http_n = snprintf(number_buf, sizeof(number_buf), "%u",
                                  (unsigned)entry->last_http_status);
      ok = ok && http_n > 0 && (size_t)http_n < sizeof(number_buf) &&
           logger_file_write_all(&writer, number_buf, (size_t)http_n);
    } else {
      ok = ok && logger_file_write_cstr(&writer, "null");
    }
    ok = ok && logger_file_write_cstr(&writer, ",\"last_server_error_code\":");
    ok = ok && logger_file_write_json_string_or_null(
                   &writer, entry->last_server_error_code);
    ok = ok &&
         logger_file_write_cstr(&writer, ",\"last_server_error_message\":");
    ok = ok && logger_file_write_json_string_or_null(
                   &writer, entry->last_server_error_message);
    ok = ok && logger_file_write_cstr(&writer, ",\"last_response_excerpt\":");
    ok = ok && logger_file_write_json_string_or_null(
                   &writer, entry->last_response_excerpt);
    ok = ok && logger_file_write_cstr(&writer, ",\"verified_upload_utc\":");
    ok = ok && logger_file_write_json_string_or_null(
                   &writer, entry->verified_upload_utc);
    ok = ok && logger_file_write_cstr(&writer, ",\"verified_bundle_sha256\":");
    ok = ok && logger_file_write_json_string_or_null(
                   &writer, entry->verified_bundle_sha256);
    ok = ok && logger_file_write_cstr(&writer, ",\"receipt_id\":");
    ok =
        ok && logger_file_write_json_string_or_null(&writer, entry->receipt_id);
    ok = ok && logger_file_write_cstr(&writer, "}");
  }

  ok = ok && logger_file_write_cstr(&writer, "]}");
  logger_queue_slot_meta_t meta;
  memset(&meta, 0, sizeof(meta));
  meta.payload_len = (uint32_t)writer.bytes_written;
  meta.generation = generation;
  logger_sha256_final(&sha, meta.payload_sha256);

  if (ok) {
    logger_queue_slot_metadata_write(g_queue_scratch->load.trailer,
                                     LOGGER_QUEUE_SLOT_TRAILER_MAGIC, &meta);
    ok = f_lseek(file, LOGGER_QUEUE_SLOT_TRAILER_OFFSET) == FR_OK &&
         logger_queue_file_write_exact(file, g_queue_scratch->load.trailer,
                                       LOGGER_QUEUE_SLOT_TRAILER_BYTES);
  }
  if (ok) {
    logger_queue_slot_metadata_write(g_queue_scratch->load.header,
                                     LOGGER_QUEUE_SLOT_HEADER_MAGIC, &meta);
    ok = f_lseek(file, 0u) == FR_OK &&
         logger_queue_file_write_exact(file, g_queue_scratch->load.header,
                                       LOGGER_QUEUE_SLOT_HEADER_BYTES);
  }

  const FRESULT sync_fr = ok ? f_sync(file) : FR_INT_ERR;
  const FRESULT close_fr = f_close(file);
  if (!ok || sync_fr != FR_OK || close_fr != FR_OK) {
    logger_queue_write_workspace_release(workspace);
    return false;
  }
  logger_queue_write_workspace_release(workspace);
  return true;
}

bool logger_upload_queue_rebuild_file(
    logger_system_log_t *system_log, const char *updated_at_utc_or_null,
    logger_upload_queue_summary_t *summary_out) {
  queue_op_workspace_t *workspace = logger_queue_op_workspace_acquire();
  logger_upload_queue_t *const scanned = &workspace->scanned;
  logger_upload_queue_t *const previous = &workspace->previous;
  logger_upload_queue_t *const rebuilt = &workspace->merged;
  bool *const previous_seen = workspace->previous_seen;

  if (!logger_upload_queue_scan(scanned, system_log, updated_at_utc_or_null)) {
    logger_queue_op_workspace_release(workspace);
    if (summary_out != NULL) {
      logger_upload_queue_summary_init(summary_out);
    }
    return false;
  }

  if (!logger_upload_queue_merge_scan_with_previous(rebuilt, scanned, previous,
                                                    previous_seen, system_log,
                                                    updated_at_utc_or_null)) {
    logger_queue_op_workspace_release(workspace);
    if (summary_out != NULL) {
      logger_upload_queue_summary_init(summary_out);
    }
    return false;
  }

  logger_log_queue_rebuilt(system_log, updated_at_utc_or_null,
                           "manual_rebuild");
  if (summary_out != NULL) {
    logger_upload_queue_compute_summary(rebuilt, summary_out);
  }
  logger_queue_op_workspace_release(workspace);
  return true;
}

bool logger_upload_queue_requeue_blocked_file(
    logger_system_log_t *system_log, const char *updated_at_utc_or_null,
    const char *reason, size_t *requeued_count_out,
    logger_upload_queue_summary_t *summary_out) {
  if (requeued_count_out != NULL) {
    *requeued_count_out = 0u;
  }

  queue_op_workspace_t *workspace = logger_queue_op_workspace_acquire();
  logger_upload_queue_t *const queue = &workspace->scanned;

  if (!logger_upload_queue_load(queue)) {
    logger_queue_op_workspace_release(workspace);
    if (summary_out != NULL) {
      logger_upload_queue_summary_init(summary_out);
    }
    return false;
  }

  size_t requeued_count = 0u;
  for (size_t i = 0u; i < queue->session_count; ++i) {
    logger_upload_queue_entry_t *entry = &queue->sessions[i];
    if (strcmp(entry->status, "blocked_min_firmware") != 0) {
      continue;
    }
    logger_copy_string(entry->status, sizeof(entry->status), "pending");
    entry->last_failure_class[0] = '\0';
    entry->last_http_status = 0u;
    entry->last_server_error_code[0] = '\0';
    entry->last_server_error_message[0] = '\0';
    entry->last_response_excerpt[0] = '\0';
    entry->verified_upload_utc[0] = '\0';
    entry->verified_bundle_sha256[0] = '\0';
    entry->receipt_id[0] = '\0';
    requeued_count += 1u;
  }

  if (requeued_count > 0u) {
    logger_copy_string(queue->updated_at_utc, sizeof(queue->updated_at_utc),
                       updated_at_utc_or_null);
    logger_upload_queue_sort(queue);
    if (!logger_upload_queue_write(queue)) {
      logger_queue_op_workspace_release(workspace);
      if (summary_out != NULL) {
        logger_upload_queue_summary_init(summary_out);
      }
      return false;
    }
    logger_log_queue_requeued(
        system_log, updated_at_utc_or_null,
        logger_string_present(reason) ? reason : "manual_requeue_blocked",
        requeued_count);
  }

  if (requeued_count_out != NULL) {
    *requeued_count_out = requeued_count;
  }
  if (summary_out != NULL) {
    logger_upload_queue_compute_summary(queue, summary_out);
  }
  logger_queue_op_workspace_release(workspace);
  return true;
}

bool logger_upload_queue_prune_file(
    logger_system_log_t *system_log, const char *updated_at_utc_or_null,
    uint64_t reserve_bytes, size_t *retention_pruned_count_out,
    size_t *reserve_pruned_count_out, bool *reserve_met_out,
    logger_upload_queue_summary_t *summary_out) {
  if (retention_pruned_count_out != NULL) {
    *retention_pruned_count_out = 0u;
  }
  if (reserve_pruned_count_out != NULL) {
    *reserve_pruned_count_out = 0u;
  }
  if (reserve_met_out != NULL) {
    *reserve_met_out = false;
  }

  logger_storage_status_t storage;
  (void)logger_storage_refresh(&storage);
  if (!storage.mounted || !storage.writable || !storage.logger_root_ready) {
    if (summary_out != NULL) {
      logger_upload_queue_summary_init(summary_out);
    }
    return false;
  }

  queue_op_workspace_t *workspace = logger_queue_op_workspace_acquire();
  logger_upload_queue_t *const queue = &workspace->scanned;

  if (!logger_upload_queue_load(queue)) {
    logger_queue_op_workspace_release(workspace);
    if (summary_out != NULL) {
      logger_upload_queue_summary_init(summary_out);
    }
    return false;
  }

  size_t retention_pruned_count = 0u;
  size_t reserve_pruned_count = 0u;
  bool changed = false;

  for (size_t i = 0u; i < queue->session_count;) {
    if (!logger_queue_entry_retention_expired(&queue->sessions[i],
                                              updated_at_utc_or_null)) {
      ++i;
      continue;
    }

    const logger_upload_queue_entry_t *const pruned = &queue->sessions[i];
    if (!logger_remove_closed_session_dir(pruned->dir_name)) {
      logger_queue_op_workspace_release(workspace);
      if (summary_out != NULL) {
        logger_upload_queue_summary_init(summary_out);
      }
      return false;
    }
    logger_log_session_pruned(system_log, updated_at_utc_or_null, pruned,
                              "retention_expired");
    logger_upload_queue_remove_at(queue, i);
    retention_pruned_count += 1u;
    changed = true;
  }

  (void)logger_storage_refresh(&storage);
  if (!storage.mounted || !storage.writable || !storage.logger_root_ready) {
    logger_queue_op_workspace_release(workspace);
    if (summary_out != NULL) {
      logger_upload_queue_summary_init(summary_out);
    }
    return false;
  }

  while (storage.free_bytes < reserve_bytes) {
    const int oldest_verified_index =
        logger_upload_queue_find_oldest_verified(queue);
    if (oldest_verified_index < 0) {
      break;
    }

    const size_t index = (size_t)oldest_verified_index;
    const logger_upload_queue_entry_t *const pruned = &queue->sessions[index];
    if (!logger_remove_closed_session_dir(pruned->dir_name)) {
      logger_queue_op_workspace_release(workspace);
      if (summary_out != NULL) {
        logger_upload_queue_summary_init(summary_out);
      }
      return false;
    }
    logger_log_session_pruned(system_log, updated_at_utc_or_null, pruned,
                              "reserve_protection");
    logger_upload_queue_remove_at(queue, index);
    reserve_pruned_count += 1u;
    changed = true;

    (void)logger_storage_refresh(&storage);
    if (!storage.mounted || !storage.writable || !storage.logger_root_ready) {
      logger_queue_op_workspace_release(workspace);
      if (summary_out != NULL) {
        logger_upload_queue_summary_init(summary_out);
      }
      return false;
    }
  }

  if (changed) {
    logger_copy_string(queue->updated_at_utc, sizeof(queue->updated_at_utc),
                       updated_at_utc_or_null);
    logger_upload_queue_sort(queue);
    if (!logger_upload_queue_write(queue)) {
      logger_queue_op_workspace_release(workspace);
      if (summary_out != NULL) {
        logger_upload_queue_summary_init(summary_out);
      }
      return false;
    }
  }

  if (retention_pruned_count_out != NULL) {
    *retention_pruned_count_out = retention_pruned_count;
  }
  if (reserve_pruned_count_out != NULL) {
    *reserve_pruned_count_out = reserve_pruned_count;
  }
  if (reserve_met_out != NULL) {
    *reserve_met_out = storage.free_bytes >= reserve_bytes;
  }
  if (summary_out != NULL) {
    logger_upload_queue_compute_summary(queue, summary_out);
  }
  logger_queue_op_workspace_release(workspace);
  return true;
}

bool logger_upload_queue_refresh_file(
    logger_system_log_t *system_log, const char *updated_at_utc_or_null,
    logger_upload_queue_summary_t *summary_out) {
  queue_op_workspace_t *workspace = logger_queue_op_workspace_acquire();
  logger_upload_queue_t *const scanned = &workspace->scanned;
  logger_upload_queue_t *const previous = &workspace->previous;
  logger_upload_queue_t *const merged = &workspace->merged;
  bool *const previous_seen = workspace->previous_seen;

  if (!logger_upload_queue_scan(scanned, system_log, updated_at_utc_or_null)) {
    logger_queue_op_workspace_release(workspace);
    if (summary_out != NULL) {
      logger_upload_queue_summary_init(summary_out);
    }
    return false;
  }

  const bool ok = logger_upload_queue_merge_scan_with_previous(
      merged, scanned, previous, previous_seen, system_log,
      updated_at_utc_or_null);
  if (summary_out != NULL) {
    logger_upload_queue_compute_summary(merged, summary_out);
  }
  logger_queue_op_workspace_release(workspace);
  return ok;
}
