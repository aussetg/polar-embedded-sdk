#ifndef LOGGER_FIRMWARE_CONFIG_STORE_H
#define LOGGER_FIRMWARE_CONFIG_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "logger/faults.h"

#define LOGGER_CONFIG_LOGGER_ID_MAX 64
#define LOGGER_CONFIG_SUBJECT_ID_MAX 64
#define LOGGER_CONFIG_BOUND_H10_ADDR_MAX 18
#define LOGGER_CONFIG_TIMEZONE_MAX 64
#define LOGGER_CONFIG_UPLOAD_URL_MAX 192
#define LOGGER_CONFIG_UPLOAD_TOKEN_MAX 160
#define LOGGER_CONFIG_WIFI_SSID_MAX 33
#define LOGGER_CONFIG_WIFI_PSK_MAX 65

typedef struct {
    char logger_id[LOGGER_CONFIG_LOGGER_ID_MAX];
    char subject_id[LOGGER_CONFIG_SUBJECT_ID_MAX];
    char bound_h10_address[LOGGER_CONFIG_BOUND_H10_ADDR_MAX];
    char timezone[LOGGER_CONFIG_TIMEZONE_MAX];
    char upload_url[LOGGER_CONFIG_UPLOAD_URL_MAX];
    char upload_token[LOGGER_CONFIG_UPLOAD_TOKEN_MAX];
    char wifi_ssid[LOGGER_CONFIG_WIFI_SSID_MAX];
    char wifi_psk[LOGGER_CONFIG_WIFI_PSK_MAX];
} logger_config_t;

typedef struct {
    uint32_t boot_counter;
    logger_fault_code_t current_fault_code;
    logger_fault_code_t last_cleared_fault_code;
    logger_config_t config;
    uint32_t storage_sequence;
    int storage_slot;
    bool storage_valid;
} logger_persisted_state_t;

void logger_config_init(logger_config_t *config);
void logger_persisted_state_init(logger_persisted_state_t *state);

bool logger_config_store_load(logger_persisted_state_t *state);
bool logger_config_store_save(logger_persisted_state_t *state);
bool logger_config_store_factory_reset(logger_persisted_state_t *state);

bool logger_config_normal_logging_ready(const logger_config_t *config);
bool logger_config_upload_configured(const logger_config_t *config);
bool logger_config_wifi_configured(const logger_config_t *config);

bool logger_config_set_logger_id(logger_persisted_state_t *state, const char *value);
bool logger_config_set_subject_id(logger_persisted_state_t *state, const char *value);
bool logger_config_set_bound_h10_address(logger_persisted_state_t *state, const char *value, bool *bond_cleared);
bool logger_config_set_timezone(logger_persisted_state_t *state, const char *value);
bool logger_config_set_wifi_ssid(logger_persisted_state_t *state, const char *value);
bool logger_config_set_wifi_psk(logger_persisted_state_t *state, const char *value);
bool logger_config_set_upload_url(logger_persisted_state_t *state, const char *value);
bool logger_config_set_upload_token(logger_persisted_state_t *state, const char *value);
bool logger_config_clear_upload(logger_persisted_state_t *state);

#endif
