#include "logger/config_store.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "hardware/address_mapped.h"
#include "hardware/flash.h"
#include "hardware/sync.h"

#include "logger/flash_layout.h"

#define LOGGER_FLASH_MAGIC 0x31474643u
#define LOGGER_FLASH_SCHEMA_VERSION 3u

#define LOGGER_FLASH_CONFIG_SLOT0_OFFSET LOGGER_FLASH_CONFIG_REGION_OFFSET
#define LOGGER_FLASH_CONFIG_SLOT1_OFFSET (LOGGER_FLASH_CONFIG_SLOT0_OFFSET + LOGGER_FLASH_CONFIG_SLOT_SIZE)

extern char __flash_binary_end;

typedef struct {
    char logger_id[LOGGER_CONFIG_LOGGER_ID_MAX];
    char subject_id[LOGGER_CONFIG_SUBJECT_ID_MAX];
    char bound_h10_address[LOGGER_CONFIG_BOUND_H10_ADDR_MAX];
    char timezone[LOGGER_CONFIG_TIMEZONE_MAX];
    char upload_url[LOGGER_CONFIG_UPLOAD_URL_MAX];
    char upload_token[LOGGER_CONFIG_UPLOAD_TOKEN_MAX];
    char wifi_ssid[LOGGER_CONFIG_WIFI_SSID_MAX];
    char wifi_psk[LOGGER_CONFIG_WIFI_PSK_MAX];
} logger_flash_config_v1_v2_t;

typedef struct {
    uint32_t magic;
    uint16_t schema_version;
    uint16_t payload_bytes;
    uint32_t sequence;
    uint32_t crc32;
    uint32_t boot_counter;
    uint16_t current_fault_code;
    uint16_t last_cleared_fault_code;
    logger_flash_config_v1_v2_t config;
} logger_flash_record_v1_t;

typedef struct {
    uint32_t magic;
    uint16_t schema_version;
    uint16_t payload_bytes;
    uint32_t sequence;
    uint32_t crc32;
    uint32_t boot_counter;
    uint16_t current_fault_code;
    uint16_t last_cleared_fault_code;
    logger_flash_config_v1_v2_t config;
    char last_boot_firmware_version[LOGGER_PERSISTED_FIRMWARE_VERSION_MAX];
    char last_boot_build_id[LOGGER_PERSISTED_BUILD_ID_MAX];
} logger_flash_record_v2_t;

typedef struct {
    uint32_t magic;
    uint16_t schema_version;
    uint16_t payload_bytes;
    uint32_t sequence;
    uint32_t crc32;
    uint32_t boot_counter;
    uint16_t current_fault_code;
    uint16_t last_cleared_fault_code;
    logger_config_t config;
    char last_boot_firmware_version[LOGGER_PERSISTED_FIRMWARE_VERSION_MAX];
    char last_boot_build_id[LOGGER_PERSISTED_BUILD_ID_MAX];
} logger_flash_record_t;

_Static_assert(sizeof(logger_flash_record_t) <= LOGGER_FLASH_CONFIG_SLOT_SIZE,
               "config record no longer fits in a flash slot");

typedef struct {
    bool valid;
    uint32_t sequence;
    logger_persisted_state_t state;
    int slot;
} logger_flash_slot_state_t;

static uint32_t logger_crc32_ieee(const uint8_t *data, size_t len) {
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            const uint32_t mask = (uint32_t)(-(int32_t)(crc & 1u));
            crc = (crc >> 1) ^ (0xedb88320u & mask);
        }
    }
    return crc ^ 0xffffffffu;
}

static uint32_t logger_flash_slot_offset(unsigned slot) {
    return slot == 0u ? LOGGER_FLASH_CONFIG_SLOT0_OFFSET : LOGGER_FLASH_CONFIG_SLOT1_OFFSET;
}

static bool logger_flash_record_v1_valid(const logger_flash_record_v1_t *record) {
    if (record->magic != LOGGER_FLASH_MAGIC) {
        return false;
    }
    if (record->schema_version != 1u) {
        return false;
    }
    if (record->payload_bytes != sizeof(logger_flash_record_v1_t)) {
        return false;
    }

    logger_flash_record_v1_t copy = *record;
    copy.crc32 = 0u;
    const uint32_t crc = logger_crc32_ieee((const uint8_t *)&copy, sizeof(copy));
    return crc == record->crc32;
}

static bool logger_flash_record_v2_valid(const logger_flash_record_v2_t *record) {
    if (record->magic != LOGGER_FLASH_MAGIC) {
        return false;
    }
    if (record->schema_version != 2u) {
        return false;
    }
    if (record->payload_bytes != sizeof(logger_flash_record_v2_t)) {
        return false;
    }

    logger_flash_record_v2_t copy = *record;
    copy.crc32 = 0u;
    const uint32_t crc = logger_crc32_ieee((const uint8_t *)&copy, sizeof(copy));
    return crc == record->crc32;
}

static bool logger_flash_record_valid(const logger_flash_record_t *record) {
    if (record->magic != LOGGER_FLASH_MAGIC) {
        return false;
    }
    if (record->schema_version != LOGGER_FLASH_SCHEMA_VERSION) {
        return false;
    }
    if (record->payload_bytes != sizeof(logger_flash_record_t)) {
        return false;
    }

    logger_flash_record_t copy = *record;
    copy.crc32 = 0u;
    const uint32_t crc = logger_crc32_ieee((const uint8_t *)&copy, sizeof(copy));
    return crc == record->crc32;
}

static void logger_copy_string(char *dst, size_t dst_len, const char *src) {
    if (dst_len == 0u) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    size_t n = 0u;
    while (src[n] != '\0' && n + 1u < dst_len) {
        dst[n] = src[n];
        ++n;
    }
    dst[n] = '\0';
}

static bool logger_string_present(const char *value) {
    return value != NULL && value[0] != '\0';
}

static bool logger_upload_url_uses_https(const char *value) {
    return logger_string_present(value) && strncmp(value, "https://", 8u) == 0;
}

static void logger_copy_legacy_config(logger_config_t *dst, const logger_flash_config_v1_v2_t *src) {
    logger_config_init(dst);
    logger_copy_string(dst->logger_id, sizeof(dst->logger_id), src->logger_id);
    logger_copy_string(dst->subject_id, sizeof(dst->subject_id), src->subject_id);
    logger_copy_string(dst->bound_h10_address, sizeof(dst->bound_h10_address), src->bound_h10_address);
    logger_copy_string(dst->timezone, sizeof(dst->timezone), src->timezone);
    logger_copy_string(dst->upload_url, sizeof(dst->upload_url), src->upload_url);
    logger_copy_string(dst->upload_token, sizeof(dst->upload_token), src->upload_token);
    logger_copy_string(dst->wifi_ssid, sizeof(dst->wifi_ssid), src->wifi_ssid);
    logger_copy_string(dst->wifi_psk, sizeof(dst->wifi_psk), src->wifi_psk);
}

void logger_config_clear_provisioned_anchor_in_memory(logger_config_t *config) {
    if (config == NULL) {
        return;
    }
    config->upload_tls_anchor_der_len = 0u;
    memset(config->upload_tls_anchor_der, 0, sizeof(config->upload_tls_anchor_der));
    memset(config->upload_tls_anchor_sha256, 0, sizeof(config->upload_tls_anchor_sha256));
    memset(config->upload_tls_anchor_subject, 0, sizeof(config->upload_tls_anchor_subject));
}

void logger_config_clear_upload_tls_in_memory(logger_config_t *config) {
    if (config == NULL) {
        return;
    }
    config->upload_tls_mode[0] = '\0';
    logger_config_clear_provisioned_anchor_in_memory(config);
}

bool logger_config_set_provisioned_anchor_in_memory(
    logger_config_t *config,
    const uint8_t *der,
    size_t der_len,
    const char *sha256_hex,
    const char *subject) {
    if (config == NULL || der == NULL || der_len == 0u || der_len > LOGGER_CONFIG_UPLOAD_TLS_ANCHOR_DER_MAX) {
        return false;
    }
    logger_config_clear_provisioned_anchor_in_memory(config);
    memcpy(config->upload_tls_anchor_der, der, der_len);
    config->upload_tls_anchor_der_len = (uint16_t)der_len;
    logger_copy_string(config->upload_tls_mode, sizeof(config->upload_tls_mode), LOGGER_UPLOAD_TLS_MODE_PROVISIONED_ANCHOR);
    logger_copy_string(config->upload_tls_anchor_sha256,
                       sizeof(config->upload_tls_anchor_sha256),
                       sha256_hex);
    logger_copy_string(config->upload_tls_anchor_subject,
                       sizeof(config->upload_tls_anchor_subject),
                       subject);
    return true;
}

void logger_config_set_upload_tls_public_roots_in_memory(logger_config_t *config) {
    if (config == NULL) {
        return;
    }
    logger_copy_string(config->upload_tls_mode,
                       sizeof(config->upload_tls_mode),
                       LOGGER_UPLOAD_TLS_MODE_PUBLIC_ROOTS);
    logger_config_clear_provisioned_anchor_in_memory(config);
}

bool logger_config_upload_has_provisioned_anchor(const logger_config_t *config) {
    return config != NULL && config->upload_tls_anchor_der_len > 0u;
}

const char *logger_config_upload_tls_mode(const logger_config_t *config) {
    if (config == NULL || !logger_upload_url_uses_https(config->upload_url)) {
        return NULL;
    }
    if (strcmp(config->upload_tls_mode, LOGGER_UPLOAD_TLS_MODE_PROVISIONED_ANCHOR) == 0) {
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
    if (strcmp(config->upload_tls_mode, LOGGER_UPLOAD_TLS_MODE_PROVISIONED_ANCHOR) == 0 &&
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
    state->storage_slot = -1;
}

static void logger_state_from_record(logger_persisted_state_t *state, const logger_flash_record_t *record, int slot) {
    state->boot_counter = record->boot_counter;
    state->current_fault_code = (logger_fault_code_t)record->current_fault_code;
    state->last_cleared_fault_code = (logger_fault_code_t)record->last_cleared_fault_code;
    state->config = record->config;
    logger_config_sanitize_upload_tls(&state->config);
    logger_copy_string(state->last_boot_firmware_version,
                       sizeof(state->last_boot_firmware_version),
                       record->last_boot_firmware_version);
    logger_copy_string(state->last_boot_build_id,
                       sizeof(state->last_boot_build_id),
                       record->last_boot_build_id);
    state->storage_sequence = record->sequence;
    state->storage_slot = slot;
    state->storage_valid = true;
}

static void logger_state_from_record_v2(
    logger_persisted_state_t *state,
    const logger_flash_record_v2_t *record,
    int slot) {
    state->boot_counter = record->boot_counter;
    state->current_fault_code = (logger_fault_code_t)record->current_fault_code;
    state->last_cleared_fault_code = (logger_fault_code_t)record->last_cleared_fault_code;
    logger_copy_legacy_config(&state->config, &record->config);
    logger_config_sanitize_upload_tls(&state->config);
    logger_copy_string(state->last_boot_firmware_version,
                       sizeof(state->last_boot_firmware_version),
                       record->last_boot_firmware_version);
    logger_copy_string(state->last_boot_build_id,
                       sizeof(state->last_boot_build_id),
                       record->last_boot_build_id);
    state->storage_sequence = record->sequence;
    state->storage_slot = slot;
    state->storage_valid = true;
}

static void logger_state_from_record_v1(
    logger_persisted_state_t *state,
    const logger_flash_record_v1_t *record,
    int slot) {
    state->boot_counter = record->boot_counter;
    state->current_fault_code = (logger_fault_code_t)record->current_fault_code;
    state->last_cleared_fault_code = (logger_fault_code_t)record->last_cleared_fault_code;
    logger_copy_legacy_config(&state->config, &record->config);
    logger_config_sanitize_upload_tls(&state->config);
    state->last_boot_firmware_version[0] = '\0';
    state->last_boot_build_id[0] = '\0';
    state->storage_sequence = record->sequence;
    state->storage_slot = slot;
    state->storage_valid = true;
}

static void logger_record_from_state(const logger_persisted_state_t *state, logger_flash_record_t *record) {
    logger_persisted_state_t sanitized = *state;
    logger_config_sanitize_upload_tls(&sanitized.config);

    memset(record, 0xff, sizeof(*record));
    record->magic = LOGGER_FLASH_MAGIC;
    record->schema_version = LOGGER_FLASH_SCHEMA_VERSION;
    record->payload_bytes = sizeof(logger_flash_record_t);
    record->sequence = sanitized.storage_sequence;
    record->crc32 = 0u;
    record->boot_counter = sanitized.boot_counter;
    record->current_fault_code = (uint16_t)sanitized.current_fault_code;
    record->last_cleared_fault_code = (uint16_t)sanitized.last_cleared_fault_code;
    record->config = sanitized.config;
    logger_copy_string(record->last_boot_firmware_version,
                       sizeof(record->last_boot_firmware_version),
                       sanitized.last_boot_firmware_version);
    logger_copy_string(record->last_boot_build_id,
                       sizeof(record->last_boot_build_id),
                       sanitized.last_boot_build_id);
    record->crc32 = logger_crc32_ieee((const uint8_t *)record, sizeof(*record));
}

static bool logger_flash_slot_load(unsigned slot, logger_flash_slot_state_t *out) {
    memset(out, 0, sizeof(*out));
    out->slot = (int)slot;

    const uint8_t *raw = (const uint8_t *)(XIP_BASE + logger_flash_slot_offset(slot));
    const uint32_t magic = *(const uint32_t *)raw;
    const uint16_t schema_version = *(const uint16_t *)(raw + 4u);

    if (magic != LOGGER_FLASH_MAGIC) {
        return false;
    }

    logger_persisted_state_init(&out->state);
    if (schema_version == 1u) {
        const logger_flash_record_v1_t *record = (const logger_flash_record_v1_t *)raw;
        if (!logger_flash_record_v1_valid(record)) {
            return false;
        }
        logger_state_from_record_v1(&out->state, record, (int)slot);
        out->sequence = record->sequence;
        out->valid = true;
        return true;
    }

    if (schema_version == 2u) {
        const logger_flash_record_v2_t *record = (const logger_flash_record_v2_t *)raw;
        if (!logger_flash_record_v2_valid(record)) {
            return false;
        }
        logger_state_from_record_v2(&out->state, record, (int)slot);
        out->sequence = record->sequence;
        out->valid = true;
        return true;
    }

    if (schema_version == LOGGER_FLASH_SCHEMA_VERSION) {
        const logger_flash_record_t *record = (const logger_flash_record_t *)raw;
        if (!logger_flash_record_valid(record)) {
            return false;
        }
        logger_state_from_record(&out->state, record, (int)slot);
        out->sequence = record->sequence;
        out->valid = true;
        return true;
    }

    return false;
}

static bool logger_flash_layout_is_safe(void) {
    const uintptr_t binary_end_offset = (uintptr_t)&__flash_binary_end - XIP_BASE;
    return binary_end_offset <= LOGGER_FLASH_PERSIST_REGION_OFFSET;
}

bool logger_config_store_load(logger_persisted_state_t *state) {
    logger_persisted_state_init(state);
    if (!logger_flash_layout_is_safe()) {
        return false;
    }

    logger_flash_slot_state_t slot0;
    logger_flash_slot_state_t slot1;
    const bool valid0 = logger_flash_slot_load(0u, &slot0);
    const bool valid1 = logger_flash_slot_load(1u, &slot1);

    if (valid0 && (!valid1 || slot0.sequence >= slot1.sequence)) {
        *state = slot0.state;
        return true;
    }
    if (valid1) {
        *state = slot1.state;
        return true;
    }

    state->storage_valid = false;
    return false;
}

bool logger_config_store_save(logger_persisted_state_t *state) {
    if (!logger_flash_layout_is_safe()) {
        return false;
    }

    logger_flash_record_t record;
    logger_config_sanitize_upload_tls(&state->config);
    state->storage_sequence += 1u;
    logger_record_from_state(state, &record);

    const unsigned target_slot = (state->storage_slot == 0) ? 1u : 0u;
    const uint32_t flash_offset = logger_flash_slot_offset(target_slot);

    static uint8_t sector_buf[FLASH_SECTOR_SIZE];
    memset(sector_buf, 0xff, sizeof(sector_buf));
    memcpy(sector_buf, &record, sizeof(record));

    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(flash_offset, FLASH_SECTOR_SIZE);
    flash_range_program(flash_offset, sector_buf, FLASH_SECTOR_SIZE);
    restore_interrupts(ints);

    state->storage_slot = (int)target_slot;
    state->storage_valid = true;
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
    if (mode != NULL && strcmp(mode, LOGGER_UPLOAD_TLS_MODE_PROVISIONED_ANCHOR) == 0) {
        return logger_config_upload_has_provisioned_anchor(config);
    }
    return true;
}

bool logger_config_wifi_configured(const logger_config_t *config) {
    return logger_string_present(config->wifi_ssid) && logger_string_present(config->wifi_psk);
}

static bool logger_write_if_changed(char *dst, size_t dst_len, const char *value) {
    char tmp[LOGGER_CONFIG_UPLOAD_URL_MAX];
    if (dst_len > sizeof(tmp)) {
        return false;
    }
    memset(tmp, 0, sizeof(tmp));
    logger_copy_string(tmp, dst_len, value);
    if (strncmp(dst, tmp, dst_len) == 0) {
        return true;
    }
    memcpy(dst, tmp, dst_len);
    return true;
}

static bool logger_normalize_h10_address(const char *src, char out[LOGGER_CONFIG_BOUND_H10_ADDR_MAX]) {
    if (src == NULL || strlen(src) != 17u) {
        return false;
    }
    for (size_t i = 0; i < 17u; ++i) {
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

bool logger_config_set_logger_id(logger_persisted_state_t *state, const char *value) {
    if (!logger_write_if_changed(state->config.logger_id, sizeof(state->config.logger_id), value)) {
        return false;
    }
    return logger_config_store_save(state);
}

bool logger_config_set_subject_id(logger_persisted_state_t *state, const char *value) {
    if (!logger_write_if_changed(state->config.subject_id, sizeof(state->config.subject_id), value)) {
        return false;
    }
    return logger_config_store_save(state);
}

bool logger_config_set_bound_h10_address(logger_persisted_state_t *state, const char *value, bool *bond_cleared) {
    char normalized[LOGGER_CONFIG_BOUND_H10_ADDR_MAX];
    if (!logger_normalize_h10_address(value, normalized)) {
        return false;
    }
    const bool changed = strncmp(state->config.bound_h10_address, normalized, sizeof(state->config.bound_h10_address)) != 0;
    logger_copy_string(state->config.bound_h10_address, sizeof(state->config.bound_h10_address), normalized);
    if (bond_cleared != NULL) {
        *bond_cleared = changed;
    }
    return logger_config_store_save(state);
}

bool logger_config_set_timezone(logger_persisted_state_t *state, const char *value) {
    if (!logger_write_if_changed(state->config.timezone, sizeof(state->config.timezone), value)) {
        return false;
    }
    return logger_config_store_save(state);
}

bool logger_config_set_wifi_ssid(logger_persisted_state_t *state, const char *value) {
    if (!logger_write_if_changed(state->config.wifi_ssid, sizeof(state->config.wifi_ssid), value)) {
        return false;
    }
    return logger_config_store_save(state);
}

bool logger_config_set_wifi_psk(logger_persisted_state_t *state, const char *value) {
    if (!logger_write_if_changed(state->config.wifi_psk, sizeof(state->config.wifi_psk), value)) {
        return false;
    }
    return logger_config_store_save(state);
}

bool logger_config_set_upload_url(logger_persisted_state_t *state, const char *value) {
    char previous_url[LOGGER_CONFIG_UPLOAD_URL_MAX];
    logger_copy_string(previous_url, sizeof(previous_url), state->config.upload_url);
    if (!logger_write_if_changed(state->config.upload_url, sizeof(state->config.upload_url), value)) {
        return false;
    }
    if (strncmp(previous_url, state->config.upload_url, sizeof(previous_url)) != 0) {
        if (logger_upload_url_uses_https(state->config.upload_url)) {
            logger_config_set_upload_tls_public_roots_in_memory(&state->config);
        } else {
            logger_config_clear_upload_tls_in_memory(&state->config);
        }
    }
    return logger_config_store_save(state);
}

bool logger_config_set_upload_token(logger_persisted_state_t *state, const char *value) {
    if (!logger_write_if_changed(state->config.upload_token, sizeof(state->config.upload_token), value)) {
        return false;
    }
    return logger_config_store_save(state);
}

bool logger_config_clear_upload(logger_persisted_state_t *state) {
    state->config.upload_url[0] = '\0';
    state->config.upload_token[0] = '\0';
    logger_config_clear_upload_tls_in_memory(&state->config);
    return logger_config_store_save(state);
}

bool logger_config_clear_provisioned_anchor(logger_persisted_state_t *state, bool *had_anchor_out) {
    const bool had_anchor = logger_config_upload_has_provisioned_anchor(&state->config);
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
