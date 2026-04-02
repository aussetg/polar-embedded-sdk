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
#define LOGGER_CONFIG_UPLOAD_TLS_MODE_MAX 32
#define LOGGER_CONFIG_UPLOAD_TLS_ANCHOR_DER_MAX 2304
#define LOGGER_CONFIG_UPLOAD_TLS_ANCHOR_BASE64_MAX ((((LOGGER_CONFIG_UPLOAD_TLS_ANCHOR_DER_MAX) + 2u) / 3u) * 4u)
#define LOGGER_CONFIG_UPLOAD_TLS_ANCHOR_SHA256_HEX_LEN 64
#define LOGGER_CONFIG_UPLOAD_TLS_ANCHOR_SUBJECT_MAX 256
#define LOGGER_PERSISTED_FIRMWARE_VERSION_MAX 32
#define LOGGER_PERSISTED_BUILD_ID_MAX 64

#define LOGGER_UPLOAD_TLS_MODE_PUBLIC_ROOTS "public_roots"
#define LOGGER_UPLOAD_TLS_MODE_PROVISIONED_ANCHOR "provisioned_anchor"
#define LOGGER_UPLOAD_TLS_PUBLIC_ROOT_PROFILE "logger-public-roots-v1"
#define LOGGER_UPLOAD_TLS_ANCHOR_FORMAT_X509_DER_BASE64 "x509_der_base64"

typedef struct {
    char logger_id[LOGGER_CONFIG_LOGGER_ID_MAX];
    char subject_id[LOGGER_CONFIG_SUBJECT_ID_MAX];
    char bound_h10_address[LOGGER_CONFIG_BOUND_H10_ADDR_MAX];
    char timezone[LOGGER_CONFIG_TIMEZONE_MAX];
    char upload_url[LOGGER_CONFIG_UPLOAD_URL_MAX];
    char upload_token[LOGGER_CONFIG_UPLOAD_TOKEN_MAX];
    char wifi_ssid[LOGGER_CONFIG_WIFI_SSID_MAX];
    char wifi_psk[LOGGER_CONFIG_WIFI_PSK_MAX];
    char upload_tls_mode[LOGGER_CONFIG_UPLOAD_TLS_MODE_MAX];
    uint16_t upload_tls_anchor_der_len;
    uint8_t upload_tls_anchor_der[LOGGER_CONFIG_UPLOAD_TLS_ANCHOR_DER_MAX];
    char upload_tls_anchor_sha256[LOGGER_CONFIG_UPLOAD_TLS_ANCHOR_SHA256_HEX_LEN + 1u];
    char upload_tls_anchor_subject[LOGGER_CONFIG_UPLOAD_TLS_ANCHOR_SUBJECT_MAX];
} logger_config_t;

typedef struct {
    uint32_t boot_counter;
    logger_fault_code_t current_fault_code;
    logger_fault_code_t last_cleared_fault_code;
    logger_config_t config;
    char last_boot_firmware_version[LOGGER_PERSISTED_FIRMWARE_VERSION_MAX];
    char last_boot_build_id[LOGGER_PERSISTED_BUILD_ID_MAX];
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
bool logger_config_upload_ready(const logger_config_t *config);
bool logger_config_wifi_configured(const logger_config_t *config);
const char *logger_config_upload_tls_mode(const logger_config_t *config);
bool logger_config_upload_has_provisioned_anchor(const logger_config_t *config);
void logger_config_set_upload_tls_public_roots_in_memory(logger_config_t *config);
void logger_config_clear_provisioned_anchor_in_memory(logger_config_t *config);
void logger_config_clear_upload_tls_in_memory(logger_config_t *config);
bool logger_config_set_provisioned_anchor_in_memory(
    logger_config_t *config,
    const uint8_t *der,
    size_t der_len,
    const char *sha256_hex,
    const char *subject);

bool logger_config_set_logger_id(logger_persisted_state_t *state, const char *value);
bool logger_config_set_subject_id(logger_persisted_state_t *state, const char *value);
bool logger_config_set_bound_h10_address(logger_persisted_state_t *state, const char *value, bool *bond_cleared);
bool logger_config_set_timezone(logger_persisted_state_t *state, const char *value);
bool logger_config_set_wifi_ssid(logger_persisted_state_t *state, const char *value);
bool logger_config_set_wifi_psk(logger_persisted_state_t *state, const char *value);
bool logger_config_set_upload_url(logger_persisted_state_t *state, const char *value);
bool logger_config_set_upload_token(logger_persisted_state_t *state, const char *value);
bool logger_config_clear_upload(logger_persisted_state_t *state);
bool logger_config_clear_provisioned_anchor(logger_persisted_state_t *state, bool *had_anchor_out);

#endif
