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
#define LOGGER_FLASH_SCHEMA_VERSION 1u

#define LOGGER_FLASH_CONFIG_SLOT0_OFFSET LOGGER_FLASH_CONFIG_REGION_OFFSET
#define LOGGER_FLASH_CONFIG_SLOT1_OFFSET (LOGGER_FLASH_CONFIG_SLOT0_OFFSET + LOGGER_FLASH_CONFIG_SLOT_SIZE)

extern char __flash_binary_end;

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
} logger_flash_record_t;

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

static const logger_flash_record_t *logger_flash_slot_ptr(unsigned slot) {
    return (const logger_flash_record_t *)(XIP_BASE + logger_flash_slot_offset(slot));
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
    state->storage_sequence = record->sequence;
    state->storage_slot = slot;
    state->storage_valid = true;
}

static void logger_record_from_state(const logger_persisted_state_t *state, logger_flash_record_t *record) {
    memset(record, 0xff, sizeof(*record));
    record->magic = LOGGER_FLASH_MAGIC;
    record->schema_version = LOGGER_FLASH_SCHEMA_VERSION;
    record->payload_bytes = sizeof(logger_flash_record_t);
    record->sequence = state->storage_sequence;
    record->crc32 = 0u;
    record->boot_counter = state->boot_counter;
    record->current_fault_code = (uint16_t)state->current_fault_code;
    record->last_cleared_fault_code = (uint16_t)state->last_cleared_fault_code;
    record->config = state->config;
    record->crc32 = logger_crc32_ieee((const uint8_t *)record, sizeof(*record));
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

    const logger_flash_record_t *slot0 = logger_flash_slot_ptr(0);
    const logger_flash_record_t *slot1 = logger_flash_slot_ptr(1);
    const bool valid0 = logger_flash_record_valid(slot0);
    const bool valid1 = logger_flash_record_valid(slot1);

    if (valid0 && (!valid1 || slot0->sequence >= slot1->sequence)) {
        logger_state_from_record(state, slot0, 0);
        return true;
    }
    if (valid1) {
        logger_state_from_record(state, slot1, 1);
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

static bool logger_string_present(const char *value) {
    return value != NULL && value[0] != '\0';
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

bool logger_config_set_upload_url(logger_persisted_state_t *state, const char *value) {
    if (!logger_write_if_changed(state->config.upload_url, sizeof(state->config.upload_url), value)) {
        return false;
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
    return logger_config_store_save(state);
}
