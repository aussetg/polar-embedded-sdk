#include "logger/config_store.h"

#include <assert.h>
#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "hardware/address_mapped.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "pico/flash.h"

#include "logger/flash_layout.h"
#include "logger/util.h"

#define LOGGER_FLASH_CONFIG_MAGIC 0x31474643u
#define LOGGER_FLASH_CONFIG_SCHEMA_VERSION 1u

#define LOGGER_FLASH_METADATA_MAGIC 0x3141544du
#define LOGGER_FLASH_METADATA_SCHEMA_VERSION 1u

extern char __flash_binary_end;

typedef struct {
  uint32_t magic;
  uint16_t schema_version;
  uint16_t payload_bytes;
  uint32_t sequence;
  uint32_t crc32;
  logger_config_t config;
} logger_flash_config_record_t;

typedef struct {
  uint32_t magic;
  uint16_t schema_version;
  uint16_t payload_bytes;
  uint32_t sequence;
  uint32_t crc32;
  uint32_t boot_counter;
  uint16_t current_fault_code;
  uint16_t last_cleared_fault_code;
  char last_boot_firmware_version[LOGGER_PERSISTED_FIRMWARE_VERSION_MAX];
  char last_boot_build_id[LOGGER_PERSISTED_BUILD_ID_MAX];
} logger_flash_metadata_record_t;

_Static_assert(sizeof(logger_flash_config_record_t) <=
                   LOGGER_FLASH_CONFIG_SLOT_SIZE,
               "config record no longer fits in a flash slot");
_Static_assert(sizeof(logger_flash_metadata_record_t) <=
                   LOGGER_FLASH_METADATA_SLOT_SIZE,
               "metadata record no longer fits in a flash slot");

typedef struct {
  uint32_t boot_counter;
  uint16_t current_fault_code;
  uint16_t last_cleared_fault_code;
  char last_boot_firmware_version[LOGGER_PERSISTED_FIRMWARE_VERSION_MAX];
  char last_boot_build_id[LOGGER_PERSISTED_BUILD_ID_MAX];
} logger_persisted_metadata_t;

typedef struct {
  bool loaded;
  bool config_valid;
  uint32_t config_sequence;
  int config_slot;
  logger_config_t config;
  bool metadata_valid;
  uint32_t metadata_sequence;
  int metadata_slot;
  logger_persisted_metadata_t metadata;
} logger_flash_store_cache_t;

static logger_flash_store_cache_t g_store;
static logger_flash_config_record_t g_config_record_workspace;
static bool g_config_record_workspace_in_use;

static logger_flash_config_record_t *logger_config_record_workspace_acquire(void) {
  assert(!g_config_record_workspace_in_use);
  g_config_record_workspace_in_use = true;
  memset(&g_config_record_workspace, 0xff, sizeof(g_config_record_workspace));
  return &g_config_record_workspace;
}

static void logger_config_record_workspace_release(
    logger_flash_config_record_t *record) {
  (void)record;
  assert(record == &g_config_record_workspace);
  assert(g_config_record_workspace_in_use);
  g_config_record_workspace_in_use = false;
}

static uint32_t logger_flash_config_slot_offset(unsigned slot) {
  return LOGGER_FLASH_CONFIG_REGION_OFFSET +
         (slot * LOGGER_FLASH_CONFIG_SLOT_SIZE);
}

static uint32_t logger_flash_metadata_slot_offset(unsigned slot) {
  return LOGGER_FLASH_METADATA_REGION_OFFSET +
         (slot * LOGGER_FLASH_METADATA_SLOT_SIZE);
}

static unsigned logger_flash_next_slot(int current_slot, unsigned slot_count) {
  if (slot_count == 0u || current_slot < 0) {
    return 0u;
  }
  return ((unsigned)current_slot + 1u) % slot_count;
}

static bool logger_upload_url_uses_https(const char *value) {
  return logger_string_present(value) && strncmp(value, "https://", 8u) == 0;
}

static bool
logger_flash_config_record_valid(const logger_flash_config_record_t *record) {
  if (record->magic != LOGGER_FLASH_CONFIG_MAGIC) {
    return false;
  }
  if (record->schema_version != LOGGER_FLASH_CONFIG_SCHEMA_VERSION) {
    return false;
  }
  if (record->payload_bytes != sizeof(logger_flash_config_record_t)) {
    return false;
  }

  uint32_t crc = logger_crc32_begin();
  const uint8_t *bytes = (const uint8_t *)record;
  const size_t crc_offset = offsetof(logger_flash_config_record_t, crc32);
  const uint32_t zero = 0u;
  crc = logger_crc32_update(crc, bytes, crc_offset);
  crc = logger_crc32_update(crc, (const uint8_t *)&zero, sizeof(zero));
  crc = logger_crc32_update(
      crc, bytes + crc_offset + sizeof(record->crc32),
      sizeof(*record) - crc_offset - sizeof(record->crc32));
  return logger_crc32_finish(crc) == record->crc32;
}

static bool logger_flash_metadata_record_valid(
    const logger_flash_metadata_record_t *record) {
  if (record->magic != LOGGER_FLASH_METADATA_MAGIC) {
    return false;
  }
  if (record->schema_version != LOGGER_FLASH_METADATA_SCHEMA_VERSION) {
    return false;
  }
  if (record->payload_bytes != sizeof(logger_flash_metadata_record_t)) {
    return false;
  }

  uint32_t crc = logger_crc32_begin();
  const uint8_t *bytes = (const uint8_t *)record;
  const size_t crc_offset = offsetof(logger_flash_metadata_record_t, crc32);
  const uint32_t zero = 0u;
  crc = logger_crc32_update(crc, bytes, crc_offset);
  crc = logger_crc32_update(crc, (const uint8_t *)&zero, sizeof(zero));
  crc = logger_crc32_update(
      crc, bytes + crc_offset + sizeof(record->crc32),
      sizeof(*record) - crc_offset - sizeof(record->crc32));
  return logger_crc32_finish(crc) == record->crc32;
}

void logger_config_clear_provisioned_anchor_in_memory(logger_config_t *config) {
  if (config == NULL) {
    return;
  }
  config->upload_tls_anchor_der_len = 0u;
  memset(config->upload_tls_anchor_der, 0,
         sizeof(config->upload_tls_anchor_der));
  memset(config->upload_tls_anchor_sha256, 0,
         sizeof(config->upload_tls_anchor_sha256));
  memset(config->upload_tls_anchor_subject, 0,
         sizeof(config->upload_tls_anchor_subject));
}

void logger_config_clear_upload_tls_in_memory(logger_config_t *config) {
  if (config == NULL) {
    return;
  }
  config->upload_tls_mode[0] = '\0';
  logger_config_clear_provisioned_anchor_in_memory(config);
}

bool logger_config_set_provisioned_anchor_in_memory(logger_config_t *config,
                                                    const uint8_t *der,
                                                    size_t der_len,
                                                    const char *sha256_hex,
                                                    const char *subject) {
  if (config == NULL || der == NULL || der_len == 0u ||
      der_len > LOGGER_CONFIG_UPLOAD_TLS_ANCHOR_DER_MAX) {
    return false;
  }
  logger_config_clear_provisioned_anchor_in_memory(config);
  memcpy(config->upload_tls_anchor_der, der, der_len);
  config->upload_tls_anchor_der_len = (uint16_t)der_len;
  logger_copy_string(config->upload_tls_mode, sizeof(config->upload_tls_mode),
                     LOGGER_UPLOAD_TLS_MODE_PROVISIONED_ANCHOR);
  logger_copy_string(config->upload_tls_anchor_sha256,
                     sizeof(config->upload_tls_anchor_sha256), sha256_hex);
  logger_copy_string(config->upload_tls_anchor_subject,
                     sizeof(config->upload_tls_anchor_subject), subject);
  return true;
}

void logger_config_set_upload_tls_public_roots_in_memory(
    logger_config_t *config) {
  if (config == NULL) {
    return;
  }
  logger_copy_string(config->upload_tls_mode, sizeof(config->upload_tls_mode),
                     LOGGER_UPLOAD_TLS_MODE_PUBLIC_ROOTS);
  logger_config_clear_provisioned_anchor_in_memory(config);
}

bool logger_config_upload_has_provisioned_anchor(
    const logger_config_t *config) {
  return config != NULL && config->upload_tls_anchor_der_len > 0u;
}

const char *logger_config_upload_tls_mode(const logger_config_t *config) {
  if (config == NULL || !logger_upload_url_uses_https(config->upload_url)) {
    return NULL;
  }
  if (strcmp(config->upload_tls_mode,
             LOGGER_UPLOAD_TLS_MODE_PROVISIONED_ANCHOR) == 0) {
    return LOGGER_UPLOAD_TLS_MODE_PROVISIONED_ANCHOR;
  }
  return LOGGER_UPLOAD_TLS_MODE_PUBLIC_ROOTS;
}

static void logger_config_sanitize_upload_tls(logger_config_t *config) {
  if (config == NULL) {
    return;
  }
  if (!logger_upload_url_uses_https(config->upload_url)) {
    logger_config_clear_upload_tls_in_memory(config);
    return;
  }
  if (strcmp(config->upload_tls_mode,
             LOGGER_UPLOAD_TLS_MODE_PROVISIONED_ANCHOR) == 0 &&
      logger_config_upload_has_provisioned_anchor(config)) {
    return;
  }
  logger_config_set_upload_tls_public_roots_in_memory(config);
}

void logger_config_init(logger_config_t *config) {
  memset(config, 0, sizeof(*config));
}

void logger_persisted_state_init(logger_persisted_state_t *state) {
  memset(state, 0, sizeof(*state));
  logger_config_init(&state->config);
  state->current_fault_code = LOGGER_FAULT_NONE;
  state->last_cleared_fault_code = LOGGER_FAULT_NONE;
}

static void logger_metadata_init(logger_persisted_metadata_t *metadata) {
  memset(metadata, 0, sizeof(*metadata));
  metadata->current_fault_code = (uint16_t)LOGGER_FAULT_NONE;
  metadata->last_cleared_fault_code = (uint16_t)LOGGER_FAULT_NONE;
}

static void logger_metadata_from_state(const logger_persisted_state_t *state,
                                       logger_persisted_metadata_t *metadata) {
  logger_metadata_init(metadata);
  metadata->boot_counter = state->boot_counter;
  metadata->current_fault_code = (uint16_t)state->current_fault_code;
  metadata->last_cleared_fault_code = (uint16_t)state->last_cleared_fault_code;
  logger_copy_string(metadata->last_boot_firmware_version,
                     sizeof(metadata->last_boot_firmware_version),
                     state->last_boot_firmware_version);
  logger_copy_string(metadata->last_boot_build_id,
                     sizeof(metadata->last_boot_build_id),
                     state->last_boot_build_id);
}

static void
logger_state_apply_metadata(logger_persisted_state_t *state,
                            const logger_persisted_metadata_t *metadata) {
  state->boot_counter = metadata->boot_counter;
  state->current_fault_code = (logger_fault_code_t)metadata->current_fault_code;
  state->last_cleared_fault_code =
      (logger_fault_code_t)metadata->last_cleared_fault_code;
  logger_copy_string(state->last_boot_firmware_version,
                     sizeof(state->last_boot_firmware_version),
                     metadata->last_boot_firmware_version);
  logger_copy_string(state->last_boot_build_id,
                     sizeof(state->last_boot_build_id),
                     metadata->last_boot_build_id);
}

static void logger_config_normalize(const logger_config_t *src,
                                    logger_config_t *dst) {
  logger_config_init(dst);
  if (src == NULL) {
    return;
  }

  logger_copy_string(dst->logger_id, sizeof(dst->logger_id), src->logger_id);
  logger_copy_string(dst->subject_id, sizeof(dst->subject_id), src->subject_id);
  logger_copy_string(dst->bound_h10_address, sizeof(dst->bound_h10_address),
                     src->bound_h10_address);
  logger_copy_string(dst->timezone, sizeof(dst->timezone), src->timezone);
  logger_copy_string(dst->upload_url, sizeof(dst->upload_url), src->upload_url);
  logger_copy_string(dst->upload_api_key, sizeof(dst->upload_api_key),
                     src->upload_api_key);
  logger_copy_string(dst->upload_token, sizeof(dst->upload_token),
                     src->upload_token);
  logger_copy_string(dst->wifi_ssid, sizeof(dst->wifi_ssid), src->wifi_ssid);
  logger_copy_string(dst->wifi_psk, sizeof(dst->wifi_psk), src->wifi_psk);
  logger_copy_string(dst->upload_tls_mode, sizeof(dst->upload_tls_mode),
                     src->upload_tls_mode);

  if (src->upload_tls_anchor_der_len <=
      LOGGER_CONFIG_UPLOAD_TLS_ANCHOR_DER_MAX) {
    dst->upload_tls_anchor_der_len = src->upload_tls_anchor_der_len;
    if (dst->upload_tls_anchor_der_len > 0u) {
      memcpy(dst->upload_tls_anchor_der, src->upload_tls_anchor_der,
             dst->upload_tls_anchor_der_len);
    }
  }
  logger_copy_string(dst->upload_tls_anchor_sha256,
                     sizeof(dst->upload_tls_anchor_sha256),
                     src->upload_tls_anchor_sha256);
  logger_copy_string(dst->upload_tls_anchor_subject,
                     sizeof(dst->upload_tls_anchor_subject),
                     src->upload_tls_anchor_subject);

  logger_config_sanitize_upload_tls(dst);
}

static bool logger_flash_layout_is_safe(void) {
  const uintptr_t binary_end_offset = (uintptr_t)&__flash_binary_end - XIP_BASE;
  return binary_end_offset <= LOGGER_FLASH_PERSIST_REGION_OFFSET;
}

static void logger_store_cache_reset(void) {
  memset(&g_store, 0, sizeof(g_store));
  g_store.config_slot = -1;
  g_store.metadata_slot = -1;
}

static void logger_store_cache_apply_to_state(
    logger_persisted_state_t *state) {
  logger_persisted_state_init(state);
  if (g_store.config_valid) {
    state->config = g_store.config;
  }
  if (g_store.metadata_valid) {
    logger_state_apply_metadata(state, &g_store.metadata);
  }
}

/*
 * The RP2040/2350 XIP base is always word-aligned, and flash sector offsets
 * (FLASH_SECTOR_SIZE = 4096) are multiples of _Alignof(uint32_t).  So the
 * cast from uint8_t* to a struct whose first member is uint32_t is safe,
 * but -Wcast-align=strict cannot see that.  Validate at compile time and
 * use an intermediate uintptr_t cast to suppress the warning.
 */
_Static_assert(
    LOGGER_FLASH_CONFIG_SLOT_SIZE % _Alignof(logger_flash_config_record_t) == 0,
    "config slot size must be aligned for logger_flash_config_record_t");
_Static_assert(
    LOGGER_FLASH_METADATA_SLOT_SIZE %
            _Alignof(logger_flash_metadata_record_t) ==
        0,
    "metadata slot size must be aligned for logger_flash_metadata_record_t");

static bool logger_flash_select_latest_config(logger_config_t *config_out,
                                              uint32_t *sequence_out,
                                              int *slot_out) {
  bool found = false;
  uint32_t best_sequence = 0u;
  int best_slot = -1;

  logger_config_init(config_out);

  for (unsigned slot = 0u; slot < LOGGER_FLASH_CONFIG_SLOT_COUNT; ++slot) {
    const uint8_t *raw =
        (const uint8_t *)(XIP_BASE + logger_flash_config_slot_offset(slot));
    const logger_flash_config_record_t *record =
        (const logger_flash_config_record_t *)(uintptr_t)raw;
    if (!logger_flash_config_record_valid(record)) {
      continue;
    }
    if (!found || record->sequence >= best_sequence) {
      logger_config_normalize(&record->config, config_out);
      best_sequence = record->sequence;
      best_slot = (int)slot;
      found = true;
    }
  }

  *sequence_out = best_sequence;
  *slot_out = best_slot;
  return found;
}

static bool logger_flash_select_latest_metadata(
    logger_persisted_metadata_t *metadata_out, uint32_t *sequence_out,
    int *slot_out) {
  bool found = false;
  uint32_t best_sequence = 0u;
  int best_slot = -1;

  logger_metadata_init(metadata_out);

  for (unsigned slot = 0u; slot < LOGGER_FLASH_METADATA_SLOT_COUNT; ++slot) {
    const uint8_t *raw =
        (const uint8_t *)(XIP_BASE + logger_flash_metadata_slot_offset(slot));
    const logger_flash_metadata_record_t *record =
        (const logger_flash_metadata_record_t *)(uintptr_t)raw;
    if (!logger_flash_metadata_record_valid(record)) {
      continue;
    }
    if (!found || record->sequence >= best_sequence) {
      logger_metadata_init(metadata_out);
      metadata_out->boot_counter = record->boot_counter;
      metadata_out->current_fault_code = record->current_fault_code;
      metadata_out->last_cleared_fault_code = record->last_cleared_fault_code;
      logger_copy_string(metadata_out->last_boot_firmware_version,
                         sizeof(metadata_out->last_boot_firmware_version),
                         record->last_boot_firmware_version);
      logger_copy_string(metadata_out->last_boot_build_id,
                         sizeof(metadata_out->last_boot_build_id),
                         record->last_boot_build_id);
      best_sequence = record->sequence;
      best_slot = (int)slot;
      found = true;
    }
  }

  *sequence_out = best_sequence;
  *slot_out = best_slot;
  return found;
}

static bool logger_config_store_refresh_cache(void) {
  logger_store_cache_reset();
  g_store.loaded = true;

  if (!logger_flash_layout_is_safe()) {
    return false;
  }

  uint32_t config_sequence = 0u;
  uint32_t metadata_sequence = 0u;
  int config_slot = -1;
  int metadata_slot = -1;
  bool have_config =
      logger_flash_select_latest_config(&g_store.config, &config_sequence,
                                        &config_slot);
  bool have_metadata = logger_flash_select_latest_metadata(
      &g_store.metadata, &metadata_sequence, &metadata_slot);

  if (have_config) {
    g_store.config_valid = true;
    g_store.config_sequence = config_sequence;
    g_store.config_slot = config_slot;
  }

  if (have_metadata) {
    g_store.metadata_valid = true;
    g_store.metadata_sequence = metadata_sequence;
    g_store.metadata_slot = metadata_slot;
  } else {
    logger_metadata_init(&g_store.metadata);
    g_store.metadata_valid = false;
    g_store.metadata_sequence = 0u;
    g_store.metadata_slot = -1;
  }

  return have_config || have_metadata;
}

bool logger_config_store_load(logger_persisted_state_t *state) {
  const bool ok = logger_config_store_refresh_cache();
  logger_store_cache_apply_to_state(state);
  return ok;
}

struct flash_op_params {
  uint32_t offset;
  const uint8_t *data;
  size_t len;
};

static void __not_in_flash_func(flash_op_erase_and_program)(void *user_data) {
  struct flash_op_params *p = (struct flash_op_params *)user_data;
  flash_range_erase(p->offset, FLASH_SECTOR_SIZE);
  flash_range_program(p->offset, p->data, FLASH_SECTOR_SIZE);
}

static bool logger_flash_program_slot(uint32_t flash_offset, const void *record,
                                      size_t record_bytes) {
  static uint8_t sector_buf[FLASH_SECTOR_SIZE];

  memset(sector_buf, 0xff, sizeof(sector_buf));
  memcpy(sector_buf, record, record_bytes);

  struct flash_op_params params = {
      .offset = flash_offset,
      .data = sector_buf,
      .len = FLASH_SECTOR_SIZE,
  };
  return flash_safe_execute(flash_op_erase_and_program, &params, 1000) ==
         PICO_OK;
}

static bool __attribute__((noinline)) logger_config_store_write_metadata(
    const logger_persisted_metadata_t *metadata) {
  logger_flash_metadata_record_t record;
  memset(&record, 0xff, sizeof(record));
  record.magic = LOGGER_FLASH_METADATA_MAGIC;
  record.schema_version = LOGGER_FLASH_METADATA_SCHEMA_VERSION;
  record.payload_bytes = sizeof(record);
  record.sequence = g_store.metadata_sequence + 1u;
  record.crc32 = 0u;
  record.boot_counter = metadata->boot_counter;
  record.current_fault_code = metadata->current_fault_code;
  record.last_cleared_fault_code = metadata->last_cleared_fault_code;
  logger_copy_string(record.last_boot_firmware_version,
                     sizeof(record.last_boot_firmware_version),
                     metadata->last_boot_firmware_version);
  logger_copy_string(record.last_boot_build_id,
                     sizeof(record.last_boot_build_id),
                     metadata->last_boot_build_id);
  record.crc32 = logger_crc32_ieee((const uint8_t *)&record, sizeof(record));

  const unsigned target_slot = logger_flash_next_slot(
      g_store.metadata_slot, LOGGER_FLASH_METADATA_SLOT_COUNT);
  if (!logger_flash_program_slot(logger_flash_metadata_slot_offset(target_slot),
                                 &record, sizeof(record))) {
    return false;
  }

  g_store.metadata_valid = true;
  g_store.metadata_sequence = record.sequence;
  g_store.metadata_slot = (int)target_slot;
  g_store.metadata = *metadata;
  return true;
}

static bool __attribute__((noinline))
logger_config_store_write_config(const logger_config_t *config) {
  logger_flash_config_record_t *record =
      logger_config_record_workspace_acquire();
  record->magic = LOGGER_FLASH_CONFIG_MAGIC;
  record->schema_version = LOGGER_FLASH_CONFIG_SCHEMA_VERSION;
  record->payload_bytes = sizeof(*record);
  record->sequence = g_store.config_sequence + 1u;
  record->crc32 = 0u;
  record->config = *config;
  record->crc32 = logger_crc32_ieee((const uint8_t *)record, sizeof(*record));

  const unsigned target_slot = logger_flash_next_slot(
      g_store.config_slot, LOGGER_FLASH_CONFIG_SLOT_COUNT);
  if (!logger_flash_program_slot(logger_flash_config_slot_offset(target_slot),
                                 record, sizeof(*record))) {
    logger_config_record_workspace_release(record);
    return false;
  }

  g_store.config_valid = true;
  g_store.config_sequence = record->sequence;
  g_store.config_slot = (int)target_slot;
  g_store.config = *config;
  logger_config_record_workspace_release(record);
  return true;
}

bool logger_config_store_save(logger_persisted_state_t *state) {
  if (!g_store.loaded) {
    (void)logger_config_store_refresh_cache();
  }
  if (!logger_flash_layout_is_safe()) {
    return false;
  }

  /*
   * Normalize in-place.  All writers maintain the zero-padding invariant;
   * the only stateful normalization is TLS mode consistency.
   */
  logger_config_sanitize_upload_tls(&state->config);

  logger_persisted_metadata_t meta;
  logger_metadata_from_state(state, &meta);

  const bool config_changed =
      memcmp(&state->config, &g_store.config, sizeof(state->config)) != 0;
  const bool metadata_changed =
      memcmp(&meta, &g_store.metadata, sizeof(meta)) != 0;
  const bool write_metadata = metadata_changed || !g_store.metadata_valid;
  const bool write_config = config_changed || !g_store.config_valid;

  if (write_metadata && !logger_config_store_write_metadata(&meta)) {
    return false;
  }
  if (write_config && !logger_config_store_write_config(&state->config)) {
    return false;
  }

  g_store.config = state->config;
  logger_state_apply_metadata(state, &meta);
  return true;
}

bool logger_config_store_factory_reset(logger_persisted_state_t *state) {
  logger_config_init(&state->config);
  return logger_config_store_save(state);
}

bool logger_config_normal_logging_ready(const logger_config_t *config) {
  return logger_string_present(config->logger_id) &&
         logger_string_present(config->subject_id) &&
         logger_string_present(config->bound_h10_address) &&
         logger_string_present(config->timezone);
}

bool logger_config_upload_configured(const logger_config_t *config) {
  return logger_string_present(config->upload_url);
}

bool logger_config_upload_ready(const logger_config_t *config) {
  if (!logger_config_upload_configured(config)) {
    return false;
  }
  const char *mode = logger_config_upload_tls_mode(config);
  if (mode != NULL &&
      strcmp(mode, LOGGER_UPLOAD_TLS_MODE_PROVISIONED_ANCHOR) == 0) {
    return logger_config_upload_has_provisioned_anchor(config);
  }
  return true;
}

bool logger_config_wifi_configured(const logger_config_t *config) {
  return logger_string_present(config->wifi_ssid) &&
         logger_string_present(config->wifi_psk);
}

static bool logger_write_if_changed(char *dst, size_t dst_len,
                                    const char *value) {
  const char *cmp = (value != NULL) ? value : "";
  if (strncmp(dst, cmp, dst_len) == 0) {
    return true;
  }
  memset(dst, 0, dst_len);
  logger_copy_string(dst, dst_len, value);
  return true;
}

static bool
logger_normalize_h10_address(const char *src,
                             char out[LOGGER_CONFIG_BOUND_H10_ADDR_MAX]) {
  if (src == NULL || strlen(src) != 17u) {
    return false;
  }
  for (size_t i = 0u; i < 17u; ++i) {
    char ch = src[i];
    if ((i % 3u) == 2u) {
      if (ch != ':') {
        return false;
      }
      out[i] = ':';
      continue;
    }
    if (!isxdigit((unsigned char)ch)) {
      return false;
    }
    out[i] = (char)toupper((unsigned char)ch);
  }
  out[17] = '\0';
  return true;
}

bool logger_config_set_logger_id(logger_persisted_state_t *state,
                                 const char *value) {
  if (!logger_write_if_changed(state->config.logger_id,
                               sizeof(state->config.logger_id), value)) {
    return false;
  }
  return logger_config_store_save(state);
}

bool logger_config_set_subject_id(logger_persisted_state_t *state,
                                  const char *value) {
  if (!logger_write_if_changed(state->config.subject_id,
                               sizeof(state->config.subject_id), value)) {
    return false;
  }
  return logger_config_store_save(state);
}

bool logger_config_set_bound_h10_address(logger_persisted_state_t *state,
                                         const char *value,
                                         bool *bond_cleared) {
  char normalized[LOGGER_CONFIG_BOUND_H10_ADDR_MAX];
  if (!logger_normalize_h10_address(value, normalized)) {
    return false;
  }
  const bool changed = strncmp(state->config.bound_h10_address, normalized,
                               sizeof(state->config.bound_h10_address)) != 0;
  logger_copy_string(state->config.bound_h10_address,
                     sizeof(state->config.bound_h10_address), normalized);
  if (bond_cleared != NULL) {
    *bond_cleared = changed;
  }
  return logger_config_store_save(state);
}

bool logger_config_set_timezone(logger_persisted_state_t *state,
                                const char *value) {
  if (!logger_write_if_changed(state->config.timezone,
                               sizeof(state->config.timezone), value)) {
    return false;
  }
  return logger_config_store_save(state);
}

bool logger_config_set_wifi_ssid(logger_persisted_state_t *state,
                                 const char *value) {
  if (!logger_write_if_changed(state->config.wifi_ssid,
                               sizeof(state->config.wifi_ssid), value)) {
    return false;
  }
  return logger_config_store_save(state);
}

bool logger_config_set_wifi_psk(logger_persisted_state_t *state,
                                const char *value) {
  if (!logger_write_if_changed(state->config.wifi_psk,
                               sizeof(state->config.wifi_psk), value)) {
    return false;
  }
  return logger_config_store_save(state);
}

bool logger_config_set_upload_url(logger_persisted_state_t *state,
                                  const char *value) {
  const char *cmp = (value != NULL) ? value : "";
  const bool url_changed = strncmp(state->config.upload_url, cmp,
                                   sizeof(state->config.upload_url)) != 0;
  if (!logger_write_if_changed(state->config.upload_url,
                               sizeof(state->config.upload_url), value)) {
    return false;
  }
  if (url_changed) {
    if (logger_upload_url_uses_https(state->config.upload_url)) {
      logger_config_set_upload_tls_public_roots_in_memory(&state->config);
    } else {
      logger_config_clear_upload_tls_in_memory(&state->config);
    }
  }
  return logger_config_store_save(state);
}

bool logger_config_set_upload_api_key(logger_persisted_state_t *state,
                                      const char *value) {
  if (!logger_write_if_changed(state->config.upload_api_key,
                               sizeof(state->config.upload_api_key), value)) {
    return false;
  }
  return logger_config_store_save(state);
}

bool logger_config_set_upload_token(logger_persisted_state_t *state,
                                    const char *value) {
  if (!logger_write_if_changed(state->config.upload_token,
                               sizeof(state->config.upload_token), value)) {
    return false;
  }
  return logger_config_store_save(state);
}

bool logger_config_clear_upload(logger_persisted_state_t *state) {
  state->config.upload_url[0] = '\0';
  state->config.upload_api_key[0] = '\0';
  state->config.upload_token[0] = '\0';
  logger_config_clear_upload_tls_in_memory(&state->config);
  return logger_config_store_save(state);
}

bool logger_config_clear_provisioned_anchor(logger_persisted_state_t *state,
                                            bool *had_anchor_out) {
  const bool had_anchor =
      logger_config_upload_has_provisioned_anchor(&state->config);
  if (had_anchor_out != NULL) {
    *had_anchor_out = had_anchor;
  }
  logger_config_clear_provisioned_anchor_in_memory(&state->config);
  if (logger_upload_url_uses_https(state->config.upload_url)) {
    logger_config_set_upload_tls_public_roots_in_memory(&state->config);
  } else {
    logger_config_clear_upload_tls_in_memory(&state->config);
  }
  return logger_config_store_save(state);
}