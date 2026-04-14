#include "logger/service_cli.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mbedtls/base64.h"
#include "mbedtls/x509_crt.h"

#include "pico/stdlib.h"

#include "board_config.h"
#include "logger/app_main.h"
#include "logger/json.h"
#include "logger/json_writer.h"
#include "logger/queue.h"
#include "logger/sha256.h"
#include "logger/upload.h"
#include "logger/util.h"

#ifndef LOGGER_FIRMWARE_VERSION
#define LOGGER_FIRMWARE_VERSION "0.1.0-dev"
#endif

#ifndef LOGGER_BUILD_ID
#define LOGGER_BUILD_ID "logger-fw-dev"
#endif

static const char *logger_now_utc_or_null(const logger_app_t *app) {
  return app->clock.now_utc[0] != '\0' ? app->clock.now_utc : NULL;
}

static void logger_set_last_day_outcome(logger_app_t *app,
                                        const char *study_day_local,
                                        const char *kind, const char *reason) {
  logger_copy_string(app->last_day_outcome_study_day_local,
                     sizeof(app->last_day_outcome_study_day_local),
                     study_day_local);
  logger_copy_string(app->last_day_outcome_kind,
                     sizeof(app->last_day_outcome_kind), kind);
  logger_copy_string(app->last_day_outcome_reason,
                     sizeof(app->last_day_outcome_reason), reason);
  app->last_day_outcome_valid =
      study_day_local != NULL && study_day_local[0] != '\0' && kind != NULL &&
      kind[0] != '\0' && reason != NULL && reason[0] != '\0';
}

static bool logger_parse_u8(const char *text, uint8_t *value_out) {
  if (text == NULL || value_out == NULL || text[0] == '\0') {
    return false;
  }
  char *end = NULL;
  unsigned long value = strtoul(text, &end, 10);
  if (end == NULL || *end != '\0' || value > 255u) {
    return false;
  }
  *value_out = (uint8_t)value;
  return true;
}

static bool logger_parse_bool01(const char *text, bool *value_out) {
  if (text == NULL || value_out == NULL) {
    return false;
  }
  if (strcmp(text, "0") == 0) {
    *value_out = false;
    return true;
  }
  if (strcmp(text, "1") == 0) {
    *value_out = true;
    return true;
  }
  return false;
}

static bool logger_parse_size_t_strict(const char *text, size_t *value_out) {
  if (text == NULL || value_out == NULL || text[0] == '\0') {
    return false;
  }
  char *end = NULL;
  const unsigned long value = strtoul(text, &end, 10);
  if (end == NULL || *end != '\0') {
    return false;
  }
  *value_out = (size_t)value;
  return true;
}

#define LOGGER_CONFIG_IMPORT_JSON_TOKEN_MAX 256u

static bool logger_json_token_copy_primitive(const logger_json_doc_t *doc,
                                             const jsmntok_t *tok, char *out,
                                             size_t out_len) {
  if (out == NULL || out_len == 0u) {
    return false;
  }
  out[0] = '\0';
  if (doc == NULL || tok == NULL || tok->type != JSMN_PRIMITIVE ||
      tok->start < 0 || tok->end < tok->start) {
    return false;
  }

  const size_t len = (size_t)(tok->end - tok->start);
  if ((len + 1u) > out_len) {
    return false;
  }
  memcpy(out, doc->json + tok->start, len);
  out[len] = '\0';
  return true;
}

static bool logger_json_token_get_double(const logger_json_doc_t *doc,
                                         const jsmntok_t *tok,
                                         double *value_out) {
  if (value_out == NULL) {
    return false;
  }
  char number_buf[32];
  if (!logger_json_token_copy_primitive(doc, tok, number_buf,
                                        sizeof(number_buf))) {
    return false;
  }
  char *end = NULL;
  const double value = strtod(number_buf, &end);
  if (end == NULL || *end != '\0') {
    return false;
  }
  *value_out = value;
  return true;
}

static bool logger_double_nearly_equal(double a, double b) {
  double diff = a - b;
  if (diff < 0.0) {
    diff = -diff;
  }
  return diff <= 0.0005;
}

static bool logger_json_object_has_key(const logger_json_doc_t *doc,
                                       const jsmntok_t *object_tok,
                                       const char *key) {
  return logger_json_object_get(doc, object_tok, key) != NULL;
}

static bool logger_json_object_copy_string_or_empty_required(
    const logger_json_doc_t *doc, const jsmntok_t *object_tok, const char *key,
    char *out, size_t out_len) {
  if (!logger_json_object_has_key(doc, object_tok, key)) {
    return false;
  }
  return logger_json_object_copy_string_or_null(doc, object_tok, key, out,
                                                out_len);
}

static bool logger_json_array_copy_single_string(const logger_json_doc_t *doc,
                                                 const jsmntok_t *array_tok,
                                                 char *out, size_t out_len) {
  if (out == NULL || out_len == 0u || array_tok == NULL ||
      array_tok->type != JSMN_ARRAY) {
    return false;
  }
  out[0] = '\0';
  if (array_tok->size == 0) {
    return true;
  }
  if (array_tok->size != 1) {
    return false;
  }
  const jsmntok_t *value_tok = logger_json_array_get(doc, array_tok, 0u);
  return logger_json_token_copy_string(doc, value_tok, out, out_len);
}

static bool logger_upload_url_supported(const char *url) {
  return url != NULL && (strncmp(url, "http://", 7u) == 0 ||
                         strncmp(url, "https://", 8u) == 0);
}

static bool logger_upload_url_uses_https(const char *url) {
  return url != NULL && strncmp(url, "https://", 8u) == 0;
}

static bool logger_compute_sha256_hex(
    const uint8_t *data, size_t data_len,
    char out_hex[LOGGER_CONFIG_UPLOAD_TLS_ANCHOR_SHA256_HEX_LEN + 1u]) {
  if (data == NULL || data_len == 0u || out_hex == NULL) {
    return false;
  }
  logger_sha256_t sha;
  logger_sha256_init(&sha);
  logger_sha256_update(&sha, data, data_len);
  logger_sha256_final_hex(&sha, out_hex);
  return true;
}

static bool logger_extract_ca_subject(const uint8_t *der, size_t der_len,
                                      char *subject_out,
                                      size_t subject_out_len) {
  if (der == NULL || der_len == 0u || subject_out == NULL ||
      subject_out_len == 0u) {
    return false;
  }

  mbedtls_x509_crt cert;
  mbedtls_x509_crt_init(&cert);
  const int parse_rc = mbedtls_x509_crt_parse(&cert, der, der_len);
  if (parse_rc != 0) {
    mbedtls_x509_crt_free(&cert);
    return false;
  }
  if (cert.MBEDTLS_PRIVATE(ca_istrue) == 0) {
    mbedtls_x509_crt_free(&cert);
    return false;
  }

  const int subject_len =
      mbedtls_x509_dn_gets(subject_out, subject_out_len, &cert.subject);
  mbedtls_x509_crt_free(&cert);
  return subject_len > 0 && (size_t)subject_len < subject_out_len;
}

static bool logger_parse_config_import_provisioned_anchor(
    const logger_json_doc_t *doc, const jsmntok_t *anchor_tok,
    logger_config_t *config_out, const char **error_message_out) {
  if (anchor_tok == NULL || anchor_tok->type != JSMN_OBJECT) {
    *error_message_out = "config import upload.tls.anchor is invalid";
    return false;
  }

  char anchor_format[32] = {0};
  if (!logger_json_object_copy_string(doc, anchor_tok, "format", anchor_format,
                                      sizeof(anchor_format))) {
    *error_message_out = "config import upload.tls.anchor.format is invalid";
    return false;
  }
  if (strcmp(anchor_format, LOGGER_UPLOAD_TLS_ANCHOR_FORMAT_X509_DER_BASE64) !=
      0) {
    *error_message_out =
        "config import upload.tls.anchor.format is unsupported";
    return false;
  }

  static char der_base64[LOGGER_CONFIG_UPLOAD_TLS_ANCHOR_BASE64_MAX + 1u];
  if (!logger_json_object_copy_string(doc, anchor_tok, "der_base64", der_base64,
                                      sizeof(der_base64))) {
    *error_message_out =
        "config import upload.tls.anchor.der_base64 is invalid";
    return false;
  }

  size_t der_len = 0u;
  int rc = mbedtls_base64_decode(NULL, 0u, &der_len,
                                 (const unsigned char *)der_base64,
                                 strlen(der_base64));
  if (!(rc == 0 || rc == MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL) ||
      der_len == 0u || der_len > LOGGER_CONFIG_UPLOAD_TLS_ANCHOR_DER_MAX) {
    *error_message_out = "config import upload.tls.anchor.der_base64 does not "
                         "decode to a supported certificate size";
    return false;
  }

  uint8_t der_buf[LOGGER_CONFIG_UPLOAD_TLS_ANCHOR_DER_MAX];
  rc = mbedtls_base64_decode(der_buf, sizeof(der_buf), &der_len,
                             (const unsigned char *)der_base64,
                             strlen(der_base64));
  if (rc != 0 || der_len == 0u) {
    *error_message_out =
        "config import upload.tls.anchor.der_base64 is invalid";
    return false;
  }

  char subject[LOGGER_CONFIG_UPLOAD_TLS_ANCHOR_SUBJECT_MAX] = {0};
  if (!logger_extract_ca_subject(der_buf, der_len, subject, sizeof(subject))) {
    *error_message_out =
        "config import upload.tls.anchor must be a valid CA certificate";
    return false;
  }

  char sha256_hex[LOGGER_CONFIG_UPLOAD_TLS_ANCHOR_SHA256_HEX_LEN + 1u] = {0};
  if (!logger_compute_sha256_hex(der_buf, der_len, sha256_hex)) {
    *error_message_out =
        "config import upload.tls.anchor SHA-256 computation failed";
    return false;
  }

  char expected_sha256[LOGGER_CONFIG_UPLOAD_TLS_ANCHOR_SHA256_HEX_LEN + 1u] = {
      0};
  if (logger_json_object_has_key(doc, anchor_tok, "sha256") &&
      !logger_json_object_copy_string_or_null(doc, anchor_tok, "sha256",
                                              expected_sha256,
                                              sizeof(expected_sha256))) {
    *error_message_out = "config import upload.tls.anchor.sha256 is invalid";
    return false;
  }
  if (logger_string_present(expected_sha256) &&
      strcmp(expected_sha256, sha256_hex) != 0) {
    *error_message_out =
        "config import upload.tls.anchor.sha256 does not match der_base64";
    return false;
  }

  char expected_subject[LOGGER_CONFIG_UPLOAD_TLS_ANCHOR_SUBJECT_MAX] = {0};
  if (logger_json_object_has_key(doc, anchor_tok, "subject") &&
      !logger_json_object_copy_string_or_null(doc, anchor_tok, "subject",
                                              expected_subject,
                                              sizeof(expected_subject))) {
    *error_message_out = "config import upload.tls.anchor.subject is invalid";
    return false;
  }
  if (logger_string_present(expected_subject) &&
      strcmp(expected_subject, subject) != 0) {
    *error_message_out =
        "config import upload.tls.anchor.subject does not match der_base64";
    return false;
  }

  if (!logger_config_set_provisioned_anchor_in_memory(
          config_out, der_buf, der_len, sha256_hex, subject)) {
    *error_message_out = "config import upload.tls.anchor could not be stored";
    return false;
  }
  return true;
}

static bool logger_parse_config_import_upload_tls(
    const logger_json_doc_t *doc, const jsmntok_t *upload_tok,
    bool upload_enabled, const char *upload_url, logger_config_t *config_out,
    const char **error_message_out) {
  const jsmntok_t *tls_tok = logger_json_object_get(doc, upload_tok, "tls");
  if (tls_tok == NULL) {
    if (upload_enabled && logger_upload_url_uses_https(upload_url)) {
      logger_config_set_upload_tls_public_roots_in_memory(config_out);
    } else {
      logger_config_clear_upload_tls_in_memory(config_out);
    }
    return true;
  }
  if (tls_tok->type != JSMN_OBJECT) {
    *error_message_out = "config import upload.tls is invalid";
    return false;
  }

  char tls_mode[32] = {0};
  char root_profile[64] = {0};
  if (logger_json_object_has_key(doc, tls_tok, "mode") &&
      !logger_json_object_copy_string_or_null(doc, tls_tok, "mode", tls_mode,
                                              sizeof(tls_mode))) {
    *error_message_out = "config import upload.tls.mode is invalid";
    return false;
  }
  if (logger_json_object_has_key(doc, tls_tok, "root_profile") &&
      !logger_json_object_copy_string_or_null(
          doc, tls_tok, "root_profile", root_profile, sizeof(root_profile))) {
    *error_message_out = "config import upload.tls.root_profile is invalid";
    return false;
  }

  const jsmntok_t *anchor_tok = logger_json_object_get(doc, tls_tok, "anchor");
  const bool anchor_is_null =
      anchor_tok == NULL || logger_json_token_is_null(doc, anchor_tok);
  const bool upload_https =
      upload_enabled && logger_upload_url_uses_https(upload_url);

  if (!upload_https) {
    if (logger_string_present(tls_mode) ||
        logger_string_present(root_profile) || !anchor_is_null) {
      *error_message_out = "config import upload.tls must be null for disabled "
                           "or http:// upload";
      return false;
    }
    logger_config_clear_upload_tls_in_memory(config_out);
    return true;
  }

  if (!logger_string_present(tls_mode)) {
    *error_message_out = "config import upload.tls.mode must be public_roots "
                         "or provisioned_anchor for https:// upload";
    return false;
  }
  if (strcmp(tls_mode, LOGGER_UPLOAD_TLS_MODE_PUBLIC_ROOTS) == 0) {
    if (logger_string_present(root_profile) &&
        strcmp(root_profile, LOGGER_UPLOAD_TLS_PUBLIC_ROOT_PROFILE) != 0) {
      *error_message_out = "config import upload.tls.root_profile is "
                           "unsupported by current firmware";
      return false;
    }
    if (!anchor_is_null) {
      *error_message_out =
          "config import upload.tls.anchor must be null for public_roots mode";
      return false;
    }
    logger_config_set_upload_tls_public_roots_in_memory(config_out);
    return true;
  }
  if (strcmp(tls_mode, LOGGER_UPLOAD_TLS_MODE_PROVISIONED_ANCHOR) == 0) {
    if (logger_string_present(root_profile)) {
      *error_message_out = "config import upload.tls.root_profile must be null "
                           "for provisioned_anchor mode";
      return false;
    }
    return logger_parse_config_import_provisioned_anchor(
        doc, anchor_tok, config_out, error_message_out);
  }

  *error_message_out =
      "config import upload.tls.mode is unsupported by current firmware";
  return false;
}

static bool
logger_normalize_h10_address_local(const char *src,
                                   char out[LOGGER_CONFIG_BOUND_H10_ADDR_MAX]) {
  if (src == NULL || strlen(src) != 17u) {
    return false;
  }
  for (size_t i = 0u; i < 17u; ++i) {
    const char ch = src[i];
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

static bool logger_validate_fixed_policy_string(const char *value,
                                                const char *expected) {
  return value != NULL && expected != NULL && strcmp(value, expected) == 0;
}

static bool logger_parse_config_import_document(
    logger_app_t *app, const char *json, logger_persisted_state_t *state_out,
    bool *bond_cleared_out, const char **error_message_out) {
  static jsmntok_t tokens[LOGGER_CONFIG_IMPORT_JSON_TOKEN_MAX];
  logger_json_doc_t doc;
  if (!logger_json_parse(&doc, json, strlen(json), tokens,
                         LOGGER_CONFIG_IMPORT_JSON_TOKEN_MAX)) {
    *error_message_out = "config import JSON parse failed";
    return false;
  }

  const jsmntok_t *root = logger_json_root(&doc);
  if (root == NULL || root->type != JSMN_OBJECT) {
    *error_message_out = "config import requires a top-level JSON object";
    return false;
  }

  uint32_t schema_version = 0u;
  bool secrets_included = false;
  if (!logger_json_object_get_uint32(&doc, root, "schema_version",
                                     &schema_version) ||
      schema_version != 1u) {
    *error_message_out = "config import requires schema_version 1";
    return false;
  }
  if (!logger_json_object_get_bool(&doc, root, "secrets_included",
                                   &secrets_included)) {
    *error_message_out = "config import requires secrets_included";
    return false;
  }

  char hardware_id[LOGGER_HARDWARE_ID_HEX_LEN + 1];
  if (!logger_json_object_copy_string(&doc, root, "hardware_id", hardware_id,
                                      sizeof(hardware_id))) {
    *error_message_out = "config import requires hardware_id";
    return false;
  }
  if (strcmp(hardware_id, app->hardware_id) != 0) {
    *error_message_out = "config import hardware_id does not match this device";
    return false;
  }
  if (!logger_json_object_has_key(&doc, root, "exported_at_utc")) {
    *error_message_out = "config import requires exported_at_utc";
    return false;
  }

  const jsmntok_t *identity_tok =
      logger_json_object_get(&doc, root, "identity");
  const jsmntok_t *recording_tok =
      logger_json_object_get(&doc, root, "recording");
  const jsmntok_t *time_tok = logger_json_object_get(&doc, root, "time");
  const jsmntok_t *battery_tok =
      logger_json_object_get(&doc, root, "battery_policy");
  const jsmntok_t *wifi_tok = logger_json_object_get(&doc, root, "wifi");
  const jsmntok_t *upload_tok = logger_json_object_get(&doc, root, "upload");
  if (identity_tok == NULL || identity_tok->type != JSMN_OBJECT ||
      recording_tok == NULL || recording_tok->type != JSMN_OBJECT ||
      time_tok == NULL || time_tok->type != JSMN_OBJECT ||
      battery_tok == NULL || battery_tok->type != JSMN_OBJECT ||
      wifi_tok == NULL || wifi_tok->type != JSMN_OBJECT || upload_tok == NULL ||
      upload_tok->type != JSMN_OBJECT) {
    *error_message_out =
        "config import is missing one or more required sections";
    return false;
  }

  logger_persisted_state_t imported = app->persisted;
  logger_config_init(&imported.config);

  if (!logger_json_object_copy_string_or_empty_required(
          &doc, identity_tok, "logger_id", imported.config.logger_id,
          sizeof(imported.config.logger_id)) ||
      !logger_json_object_copy_string_or_empty_required(
          &doc, identity_tok, "subject_id", imported.config.subject_id,
          sizeof(imported.config.subject_id))) {
    *error_message_out = "config import identity section is invalid";
    return false;
  }

  char imported_bound_address[LOGGER_CONFIG_BOUND_H10_ADDR_MAX] = {0};
  if (!logger_json_object_copy_string_or_empty_required(
          &doc, recording_tok, "bound_h10_address", imported_bound_address,
          sizeof(imported_bound_address))) {
    *error_message_out = "config import recording.bound_h10_address is invalid";
    return false;
  }
  if (logger_string_present(imported_bound_address)) {
    char normalized_bound_address[LOGGER_CONFIG_BOUND_H10_ADDR_MAX];
    if (!logger_normalize_h10_address_local(imported_bound_address,
                                            normalized_bound_address)) {
      *error_message_out =
          "config import recording.bound_h10_address is invalid";
      return false;
    }
    logger_copy_string(imported.config.bound_h10_address,
                       sizeof(imported.config.bound_h10_address),
                       normalized_bound_address);
  } else {
    imported.config.bound_h10_address[0] = '\0';
  }

  char rollover_local[16];
  char upload_start_local[16];
  char upload_end_local[16];
  if (!logger_json_object_copy_string(&doc, recording_tok,
                                      "study_day_rollover_local",
                                      rollover_local, sizeof(rollover_local)) ||
      !logger_json_object_copy_string(
          &doc, recording_tok, "overnight_upload_window_start_local",
          upload_start_local, sizeof(upload_start_local)) ||
      !logger_json_object_copy_string(
          &doc, recording_tok, "overnight_upload_window_end_local",
          upload_end_local, sizeof(upload_end_local))) {
    *error_message_out = "config import recording policy fields are invalid";
    return false;
  }
  if (!logger_validate_fixed_policy_string(rollover_local, "04:00:00") ||
      !logger_validate_fixed_policy_string(upload_start_local, "22:00:00") ||
      !logger_validate_fixed_policy_string(upload_end_local, "06:00:00")) {
    *error_message_out = "config import contains unsupported non-default "
                         "recording policy values";
    return false;
  }

  if (!logger_json_object_copy_string_or_empty_required(
          &doc, time_tok, "timezone", imported.config.timezone,
          sizeof(imported.config.timezone))) {
    *error_message_out = "config import time.timezone is invalid";
    return false;
  }

  const jsmntok_t *critical_tok =
      logger_json_object_get(&doc, battery_tok, "critical_stop_voltage_v");
  const jsmntok_t *low_tok =
      logger_json_object_get(&doc, battery_tok, "low_start_voltage_v");
  const jsmntok_t *off_tok =
      logger_json_object_get(&doc, battery_tok, "off_charger_upload_voltage_v");
  double critical_v = 0.0;
  double low_v = 0.0;
  double off_v = 0.0;
  if (!logger_json_token_get_double(&doc, critical_tok, &critical_v) ||
      !logger_json_token_get_double(&doc, low_tok, &low_v) ||
      !logger_json_token_get_double(&doc, off_tok, &off_v)) {
    *error_message_out = "config import battery_policy values are invalid";
    return false;
  }
  if (!logger_double_nearly_equal(critical_v, 3.5) ||
      !logger_double_nearly_equal(low_v, 3.65) ||
      !logger_double_nearly_equal(off_v, 3.85)) {
    *error_message_out =
        "config import contains unsupported non-default battery policy values";
    return false;
  }

  const jsmntok_t *allowed_ssids_tok =
      logger_json_object_get(&doc, wifi_tok, "allowed_ssids");
  const jsmntok_t *networks_tok =
      logger_json_object_get(&doc, wifi_tok, "networks");
  if (allowed_ssids_tok == NULL || allowed_ssids_tok->type != JSMN_ARRAY ||
      networks_tok == NULL || networks_tok->type != JSMN_ARRAY) {
    *error_message_out = "config import wifi section is invalid";
    return false;
  }

  char allowed_ssid[LOGGER_CONFIG_WIFI_SSID_MAX] = {0};
  if (!logger_json_array_copy_single_string(
          &doc, allowed_ssids_tok, allowed_ssid, sizeof(allowed_ssid)) ||
      networks_tok->size > 1) {
    *error_message_out =
        "config import currently supports at most one Wi-Fi network";
    return false;
  }

  char network_ssid[LOGGER_CONFIG_WIFI_SSID_MAX] = {0};
  char network_psk[LOGGER_CONFIG_WIFI_PSK_MAX] = {0};
  bool network_psk_present_marker = false;
  if (networks_tok->size == 1) {
    const jsmntok_t *network_tok =
        logger_json_array_get(&doc, networks_tok, 0u);
    if (network_tok == NULL || network_tok->type != JSMN_OBJECT ||
        !logger_json_object_copy_string(&doc, network_tok, "ssid", network_ssid,
                                        sizeof(network_ssid))) {
      *error_message_out = "config import wifi.networks[0].ssid is invalid";
      return false;
    }
    if (secrets_included &&
        !logger_json_object_has_key(&doc, network_tok, "psk")) {
      *error_message_out = "config import wifi.networks[0] requires psk when "
                           "secrets_included is true";
      return false;
    }
    if (!secrets_included &&
        !logger_json_object_has_key(&doc, network_tok, "psk_present")) {
      *error_message_out = "config import wifi.networks[0] requires "
                           "psk_present when secrets_included is false";
      return false;
    }
    if (!logger_json_object_has_key(&doc, network_tok, "psk") &&
        !logger_json_object_has_key(&doc, network_tok, "psk_present")) {
      *error_message_out =
          "config import wifi.networks[0] requires psk or psk_present";
      return false;
    }
    if (logger_json_object_has_key(&doc, network_tok, "psk") &&
        !logger_json_object_copy_string_or_empty_required(
            &doc, network_tok, "psk", network_psk, sizeof(network_psk))) {
      *error_message_out = "config import wifi.networks[0].psk is invalid";
      return false;
    }
    if (logger_json_object_has_key(&doc, network_tok, "psk_present") &&
        !logger_json_object_get_bool(&doc, network_tok, "psk_present",
                                     &network_psk_present_marker)) {
      *error_message_out =
          "config import wifi.networks[0].psk_present is invalid";
      return false;
    }
    (void)network_psk_present_marker;
  }

  if ((logger_string_present(allowed_ssid) ||
       logger_string_present(network_ssid)) &&
      strcmp(allowed_ssid, network_ssid) != 0) {
    *error_message_out = "config import requires allowed_ssids[0] to match "
                         "wifi.networks[0].ssid";
    return false;
  }
  if (logger_string_present(network_ssid)) {
    logger_copy_string(imported.config.wifi_ssid,
                       sizeof(imported.config.wifi_ssid), network_ssid);
    if (secrets_included) {
      logger_copy_string(imported.config.wifi_psk,
                         sizeof(imported.config.wifi_psk), network_psk);
    } else {
      imported.config.wifi_psk[0] = '\0';
    }
  }

  bool upload_enabled = false;
  if (!logger_json_object_get_bool(&doc, upload_tok, "enabled",
                                   &upload_enabled)) {
    *error_message_out = "config import upload.enabled is invalid";
    return false;
  }

  char upload_url[LOGGER_CONFIG_UPLOAD_URL_MAX] = {0};
  if (!logger_json_object_copy_string_or_empty_required(
          &doc, upload_tok, "url", upload_url, sizeof(upload_url))) {
    *error_message_out = "config import upload.url is invalid";
    return false;
  }
  const jsmntok_t *auth_tok = logger_json_object_get(&doc, upload_tok, "auth");
  if (auth_tok == NULL || auth_tok->type != JSMN_OBJECT) {
    *error_message_out = "config import upload.auth is invalid";
    return false;
  }

  char auth_type[24];
  char auth_api_key[LOGGER_CONFIG_UPLOAD_API_KEY_MAX] = {0};
  char auth_token[LOGGER_CONFIG_UPLOAD_TOKEN_MAX] = {0};
  bool api_key_present_marker = false;
  bool token_present_marker = false;
  if (!logger_json_object_copy_string(&doc, auth_tok, "type", auth_type,
                                      sizeof(auth_type))) {
    *error_message_out = "config import upload.auth.type is invalid";
    return false;
  }
  if (strcmp(auth_type, "none") != 0 &&
      strcmp(auth_type, "api_key_and_bearer") != 0) {
    *error_message_out =
        "config import upload.auth.type must be none or api_key_and_bearer";
    return false;
  }
  if (logger_json_object_has_key(&doc, auth_tok, "api_key") &&
      !logger_json_object_copy_string_or_empty_required(
          &doc, auth_tok, "api_key", auth_api_key, sizeof(auth_api_key))) {
    *error_message_out = "config import upload.auth.api_key is invalid";
    return false;
  }
  if (logger_json_object_has_key(&doc, auth_tok, "token") &&
      !logger_json_object_copy_string_or_empty_required(
          &doc, auth_tok, "token", auth_token, sizeof(auth_token))) {
    *error_message_out = "config import upload.auth.token is invalid";
    return false;
  }
  if (logger_json_object_has_key(&doc, auth_tok, "api_key_present") &&
      !logger_json_object_get_bool(&doc, auth_tok, "api_key_present",
                                   &api_key_present_marker)) {
    *error_message_out = "config import upload.auth.api_key_present is invalid";
    return false;
  }
  if (logger_json_object_has_key(&doc, auth_tok, "token_present") &&
      !logger_json_object_get_bool(&doc, auth_tok, "token_present",
                                   &token_present_marker)) {
    *error_message_out = "config import upload.auth.token_present is invalid";
    return false;
  }
  (void)api_key_present_marker;
  (void)token_present_marker;

  if (upload_enabled) {
    if (!logger_upload_url_supported(upload_url)) {
      *error_message_out = "config import requires upload.url to be an "
                           "absolute http:// or https:// URL";
      return false;
    }
    logger_copy_string(imported.config.upload_url,
                       sizeof(imported.config.upload_url), upload_url);
    if (!logger_parse_config_import_upload_tls(&doc, upload_tok, upload_enabled,
                                               upload_url, &imported.config,
                                               error_message_out)) {
      return false;
    }
    if (strcmp(auth_type, "api_key_and_bearer") == 0) {
      if (secrets_included) {
        if (!logger_string_present(auth_api_key)) {
          *error_message_out = "config import api_key_and_bearer auth requires "
                               "api_key when secrets_included is true";
          return false;
        }
        if (!logger_string_present(auth_token)) {
          *error_message_out = "config import api_key_and_bearer auth requires "
                               "token when secrets_included is true";
          return false;
        }
        logger_copy_string(imported.config.upload_api_key,
                           sizeof(imported.config.upload_api_key),
                           auth_api_key);
        logger_copy_string(imported.config.upload_token,
                           sizeof(imported.config.upload_token), auth_token);
      } else {
        if (!logger_json_object_has_key(&doc, auth_tok, "api_key_present")) {
          *error_message_out = "config import api_key_and_bearer auth requires "
                               "api_key_present when secrets_included is false";
          return false;
        }
        if (!logger_json_object_has_key(&doc, auth_tok, "token_present")) {
          *error_message_out = "config import api_key_and_bearer auth requires "
                               "token_present when secrets_included is false";
          return false;
        }
        imported.config.upload_api_key[0] = '\0';
        imported.config.upload_token[0] = '\0';
      }
    } else {
      *error_message_out =
          "config import enabled upload requires api_key_and_bearer auth";
      return false;
    }
  } else {
    imported.config.upload_url[0] = '\0';
    imported.config.upload_api_key[0] = '\0';
    imported.config.upload_token[0] = '\0';
    if (!logger_parse_config_import_upload_tls(&doc, upload_tok, upload_enabled,
                                               upload_url, &imported.config,
                                               error_message_out)) {
      return false;
    }
  }

  if (bond_cleared_out != NULL) {
    *bond_cleared_out = strcmp(app->persisted.config.bound_h10_address,
                               imported.config.bound_h10_address) != 0;
  }
  *state_out = imported;
  return true;
}

#define logger_json_write_string_or_null(value)                                \
  logger_json_fwrite_string_or_null(stdout, (value))

static void logger_json_begin_success(const char *command,
                                      const char *generated_at_utc) {
  fputs("{\"schema_version\":1,\"command\":", stdout);
  logger_json_write_string_or_null(command);
  fputs(",\"ok\":true,\"generated_at_utc\":", stdout);
  logger_json_write_string_or_null(generated_at_utc);
  fputs(",\"payload\":", stdout);
}

static void logger_json_begin_error(const char *command,
                                    const char *generated_at_utc,
                                    const char *code, const char *message) {
  fputs("{\"schema_version\":1,\"command\":", stdout);
  logger_json_write_string_or_null(command);
  fputs(",\"ok\":false,\"generated_at_utc\":", stdout);
  logger_json_write_string_or_null(generated_at_utc);
  fputs(",\"error\":{\"code\":", stdout);
  logger_json_write_string_or_null(code);
  fputs(",\"message\":", stdout);
  logger_json_write_string_or_null(message);
  fputs("}}\n", stdout);
  fflush(stdout);
}

static void logger_json_end_success(void) {
  fputs("}\n", stdout);
  fflush(stdout);
}

static void logger_write_required_missing_array(const logger_app_t *app) {
  bool first = true;
  if (!logger_string_present(app->persisted.config.bound_h10_address)) {
    logger_json_write_string_or_null(first ? "bound_h10_address" : NULL);
    first = false;
  }
  if (!logger_string_present(app->persisted.config.logger_id)) {
    if (!first) {
      fputs(",", stdout);
    }
    logger_json_write_string_or_null("logger_id");
    first = false;
  }
  if (!logger_string_present(app->persisted.config.subject_id)) {
    if (!first) {
      fputs(",", stdout);
    }
    logger_json_write_string_or_null("subject_id");
    first = false;
  }
  if (!logger_string_present(app->persisted.config.timezone)) {
    if (!first) {
      fputs(",", stdout);
    }
    logger_json_write_string_or_null("timezone");
  }
}

static void logger_write_required_present_array(const logger_app_t *app) {
  bool first = true;
  if (logger_string_present(app->persisted.config.bound_h10_address)) {
    logger_json_write_string_or_null(first ? "bound_h10_address" : NULL);
    first = false;
  }
  if (logger_string_present(app->persisted.config.logger_id)) {
    if (!first) {
      fputs(",", stdout);
    }
    logger_json_write_string_or_null("logger_id");
    first = false;
  }
  if (logger_string_present(app->persisted.config.subject_id)) {
    if (!first) {
      fputs(",", stdout);
    }
    logger_json_write_string_or_null("subject_id");
    first = false;
  }
  if (logger_string_present(app->persisted.config.timezone)) {
    if (!first) {
      fputs(",", stdout);
    }
    logger_json_write_string_or_null("timezone");
  }
}

static void logger_write_optional_present_array(const logger_app_t *app) {
  bool first = true;
  if (logger_string_present(app->persisted.config.upload_api_key) &&
      logger_string_present(app->persisted.config.upload_token)) {
    logger_json_write_string_or_null(first ? "upload_auth" : NULL);
    first = false;
  }
  if (logger_string_present(app->persisted.config.upload_url)) {
    if (!first) {
      fputs(",", stdout);
    }
    logger_json_write_string_or_null("upload_url");
    first = false;
  }
  if (logger_config_wifi_configured(&app->persisted.config)) {
    if (!first) {
      fputs(",", stdout);
    }
    logger_json_write_string_or_null("wifi_networks");
  }
}

static void logger_write_warnings_array(const logger_app_t *app) {
  bool first = true;
  if (!app->clock.valid) {
    logger_json_write_string_or_null(first ? "clock_invalid" : NULL);
    first = false;
  }
  if (!logger_config_upload_configured(&app->persisted.config)) {
    if (!first) {
      fputs(",", stdout);
    }
    logger_json_write_string_or_null("upload_not_configured");
    first = false;
  }
  if (!logger_config_wifi_configured(&app->persisted.config)) {
    if (!first) {
      fputs(",", stdout);
    }
    logger_json_write_string_or_null("wifi_not_configured");
  }
}

static bool logger_cli_is_service_mode(const logger_app_t *app) {
  return app->runtime.current_state == LOGGER_RUNTIME_SERVICE;
}

static bool logger_cli_is_logging_mode(const logger_app_t *app) {
  return logger_runtime_state_is_logging(app->runtime.current_state);
}

static void logger_config_import_transfer_reset(logger_service_cli_t *cli) {
  if (cli == NULL) {
    return;
  }
  cli->config_import_active = false;
  cli->config_import_expected_len = 0u;
  cli->config_import_received_len = 0u;
  cli->config_import_chunk_count = 0u;
  cli->config_import_buf[0] = '\0';
}

static bool logger_require_config_import_context(logger_service_cli_t *cli,
                                                 logger_app_t *app,
                                                 const char *command,
                                                 bool require_unlock) {
  if (logger_cli_is_logging_mode(app)) {
    logger_json_begin_error(command, logger_now_utc_or_null(app),
                            "busy_logging",
                            "config import is not permitted while logging");
    return false;
  }
  if (!logger_cli_is_service_mode(app)) {
    logger_json_begin_error(command, logger_now_utc_or_null(app),
                            "not_permitted_in_mode",
                            "config import is only allowed in service mode");
    return false;
  }
  if (require_unlock && !logger_service_cli_is_unlocked(
                            cli, to_ms_since_boot(get_absolute_time()))) {
    logger_json_begin_error(command, logger_now_utc_or_null(app),
                            "service_locked",
                            "service unlock is required before config import");
    return false;
  }
  return true;
}

static bool logger_cli_is_upload_mode(const logger_app_t *app) {
  return logger_runtime_state_is_upload(app->runtime.current_state);
}

static bool logger_cli_upload_blocked_fault_present(void) {
  logger_upload_queue_t queue;
  logger_upload_queue_summary_t summary;
  logger_upload_queue_init(&queue);
  logger_upload_queue_summary_init(&summary);
  if (!logger_upload_queue_load(&queue)) {
    return true;
  }
  logger_upload_queue_compute_summary(&queue, &summary);
  return summary.blocked_count > 0u;
}

static logger_fault_code_t
logger_cli_storage_fault_code(const logger_app_t *app) {
  if (!app->storage.card_present || !app->storage.mounted ||
      !app->storage.writable || !app->storage.logger_root_ready ||
      strcmp(app->storage.filesystem, "fat32") != 0) {
    return LOGGER_FAULT_SD_MISSING_OR_UNWRITABLE;
  }
  if (!app->storage.reserve_ok) {
    return LOGGER_FAULT_SD_LOW_SPACE_RESERVE_UNMET;
  }
  return LOGGER_FAULT_NONE;
}

static bool logger_cli_fault_condition_still_present(const logger_app_t *app) {
  switch (app->persisted.current_fault_code) {
  case LOGGER_FAULT_CONFIG_INCOMPLETE:
    return !logger_config_normal_logging_ready(&app->persisted.config);
  case LOGGER_FAULT_CLOCK_INVALID:
    return !app->clock.valid;
  case LOGGER_FAULT_LOW_BATTERY_BLOCKED_START:
    return logger_battery_low_start_blocked(&app->battery);
  case LOGGER_FAULT_CRITICAL_LOW_BATTERY_STOPPED:
    return logger_battery_is_critical(&app->battery);
  case LOGGER_FAULT_SD_MISSING_OR_UNWRITABLE:
  case LOGGER_FAULT_SD_LOW_SPACE_RESERVE_UNMET:
    return logger_cli_storage_fault_code(app) ==
           app->persisted.current_fault_code;
  case LOGGER_FAULT_SD_WRITE_FAILED:
    return logger_cli_storage_fault_code(app) != LOGGER_FAULT_NONE;
  case LOGGER_FAULT_UPLOAD_BLOCKED_MIN_FIRMWARE:
    return logger_cli_upload_blocked_fault_present();
  case LOGGER_FAULT_NONE:
  default:
    return false;
  }
}

static void logger_write_storage_card_identity(const logger_app_t *app) {
  if (!logger_string_present(app->storage.manufacturer_id)) {
    fputs("null", stdout);
    return;
  }
  fputs("{\"manufacturer_id\":", stdout);
  logger_json_write_string_or_null(app->storage.manufacturer_id);
  fputs(",\"oem_id\":", stdout);
  logger_json_write_string_or_null(app->storage.oem_id);
  fputs(",\"product_name\":", stdout);
  logger_json_write_string_or_null(app->storage.product_name);
  fputs(",\"revision\":", stdout);
  logger_json_write_string_or_null(app->storage.revision);
  fputs(",\"serial_number\":", stdout);
  logger_json_write_string_or_null(app->storage.serial_number);
  fputs("}", stdout);
}

static void logger_write_status_payload(const logger_app_t *app) {
  char study_day_local[11] = {0};
  logger_upload_queue_t queue;
  logger_upload_queue_summary_t queue_summary;
  logger_upload_queue_init(&queue);
  logger_upload_queue_summary_init(&queue_summary);
  if (logger_upload_queue_load(&queue) ||
      logger_upload_queue_scan(&queue, NULL, logger_now_utc_or_null(app))) {
    logger_upload_queue_compute_summary(&queue, &queue_summary);
  }
  const bool have_study_day =
      app->session.active
          ? true
          : (logger_runtime_state_is_logging(app->runtime.current_state) &&
             logger_clock_derive_study_day_local_observed(
                 &app->clock, app->persisted.config.timezone, study_day_local));

  fputs("{\"mode\":", stdout);
  logger_json_write_string_or_null(logger_mode_name(&app->runtime));
  fputs(",\"runtime_state\":", stdout);
  logger_json_write_string_or_null(
      logger_runtime_state_name(app->runtime.current_state));

  fputs(",\"identity\":{\"hardware_id\":", stdout);
  logger_json_write_string_or_null(app->hardware_id);
  fputs(",\"logger_id\":", stdout);
  logger_json_write_string_or_null(app->persisted.config.logger_id);
  fputs(",\"subject_id\":", stdout);
  logger_json_write_string_or_null(app->persisted.config.subject_id);
  fputs("}", stdout);

  fputs(",\"provisioning\":{\"normal_logging_ready\":", stdout);
  fputs(logger_config_normal_logging_ready(&app->persisted.config) ? "true"
                                                                   : "false",
        stdout);
  fputs(",\"required_missing\":[", stdout);
  logger_write_required_missing_array(app);
  fputs("],\"warnings\":[", stdout);
  logger_write_warnings_array(app);
  fputs("]}", stdout);

  fputs(",\"fault\":{\"latched\":", stdout);
  fputs(app->persisted.current_fault_code != LOGGER_FAULT_NONE ? "true"
                                                               : "false",
        stdout);
  fputs(",\"current_code\":", stdout);
  logger_json_write_string_or_null(
      logger_fault_code_name(app->persisted.current_fault_code));
  fputs(",\"last_cleared_code\":", stdout);
  logger_json_write_string_or_null(
      logger_fault_code_name(app->persisted.last_cleared_fault_code));
  fputs("}", stdout);

  fputs(",\"battery\":{\"voltage_mv\":", stdout);
  printf("%u", (unsigned)app->battery.voltage_mv);
  fputs(",\"estimate_pct\":", stdout);
  printf("%d", app->battery.estimate_pct);
  fputs(",\"vbus_present\":", stdout);
  fputs(app->battery.vbus_present ? "true" : "false", stdout);
  fputs(",\"critical_stop_mv\":", stdout);
  printf("%u", (unsigned)LOGGER_BATTERY_CRITICAL_STOP_MV);
  fputs(",\"low_start_mv\":", stdout);
  printf("%u", (unsigned)LOGGER_BATTERY_LOW_START_BLOCK_MV);
  fputs(",\"off_charger_upload_mv\":", stdout);
  printf("%u", (unsigned)LOGGER_BATTERY_OFF_CHARGER_UPLOAD_MIN_MV);
  fputs("}", stdout);

  fputs(",\"storage\":{\"sd_present\":", stdout);
  fputs(app->storage.card_present ? "true" : "false", stdout);
  fputs(",\"filesystem\":", stdout);
  logger_json_write_string_or_null(
      logger_string_present(app->storage.filesystem) ? app->storage.filesystem
                                                     : NULL);
  fputs(",\"free_bytes\":", stdout);
  if (app->storage.mounted) {
    printf("%llu", (unsigned long long)app->storage.free_bytes);
  } else {
    fputs("null", stdout);
  }
  fputs(",\"reserve_bytes\":", stdout);
  printf("%lu", (unsigned long)LOGGER_SD_MIN_FREE_RESERVE_BYTES);
  fputs(",\"card_identity\":", stdout);
  logger_write_storage_card_identity(app);
  fputs("}", stdout);

  fputs(",\"h10\":{\"bound_address\":", stdout);
  logger_json_write_string_or_null(app->persisted.config.bound_h10_address);
  fputs(",\"connected\":", stdout);
  fputs(app->h10.connected ? "true" : "false", stdout);
  fputs(",\"encrypted\":", stdout);
  fputs(app->h10.encrypted ? "true" : "false", stdout);
  fputs(",\"bonded\":", stdout);
  fputs(app->h10.bonded ? "true" : "false", stdout);
  fputs(",\"last_seen_address\":", stdout);
  logger_json_write_string_or_null(app->h10.last_seen_address[0] != '\0'
                                       ? app->h10.last_seen_address
                                       : NULL);
  fputs(",\"battery_percent\":", stdout);
  if (app->h10.battery_percent >= 0) {
    printf("%d", app->h10.battery_percent);
  } else {
    fputs("null", stdout);
  }
  fputs(",\"phase\":", stdout);
  logger_json_write_string_or_null(logger_h10_phase_name(app->h10.phase));
  fputs(",\"att_mtu\":", stdout);
  printf("%u", (unsigned)app->h10.att_mtu);
  fputs(",\"encryption_key_size\":", stdout);
  printf("%u", (unsigned)app->h10.encryption_key_size);
  fputs(",\"ecg_start_attempts\":", stdout);
  printf("%lu", (unsigned long)app->h10.ecg_start_attempt_count);
  fputs(",\"ecg_start_successes\":", stdout);
  printf("%lu", (unsigned long)app->h10.ecg_start_success_count);
  fputs(",\"ecg_packets\":", stdout);
  printf("%lu", (unsigned long)app->h10.ecg_packet_count);
  fputs(",\"ecg_packet_drops\":", stdout);
  printf("%lu", (unsigned long)app->h10.ecg_packet_drop_count);
  fputs(",\"acc_start_attempts\":", stdout);
  printf("%lu", (unsigned long)app->h10.acc_start_attempt_count);
  fputs(",\"acc_start_successes\":", stdout);
  printf("%lu", (unsigned long)app->h10.acc_start_success_count);
  fputs(",\"acc_packets\":", stdout);
  printf("%lu", (unsigned long)app->h10.acc_packet_count);
  fputs(",\"acc_packet_drops\":", stdout);
  printf("%lu", (unsigned long)app->h10.acc_packet_drop_count);
  fputs("}", stdout);

  fputs(",\"session\":{\"active\":", stdout);
  fputs(app->session.active ? "true" : "false", stdout);
  fputs(",\"session_id\":", stdout);
  logger_json_write_string_or_null(app->session.active ? app->session.session_id
                                                       : NULL);
  fputs(",\"study_day_local\":", stdout);
  logger_json_write_string_or_null(
      app->session.active ? app->session.study_day_local
                          : (have_study_day ? study_day_local : NULL));
  fputs(",\"span_id\":", stdout);
  logger_json_write_string_or_null(
      (app->session.active && app->session.span_active)
          ? app->session.current_span_id
          : NULL);
  fputs(",\"quarantined\":", stdout);
  fputs(app->session.active && app->session.quarantined ? "true" : "false",
        stdout);
  fputs(",\"clock_state\":", stdout);
  logger_json_write_string_or_null(app->session.active
                                       ? app->session.clock_state
                                       : logger_clock_state_name(&app->clock));
  fputs(",\"journal_size_bytes\":", stdout);
  if (app->session.active) {
    printf("%llu", (unsigned long long)app->session.journal_size_bytes);
  } else {
    fputs("null", stdout);
  }
  fputs("}", stdout);

  fputs(",\"upload_queue\":{\"pending_count\":", stdout);
  printf("%lu", (unsigned long)queue_summary.pending_count);
  fputs(",\"blocked_count\":", stdout);
  printf("%lu", (unsigned long)queue_summary.blocked_count);
  fputs(",\"oldest_pending_study_day\":", stdout);
  logger_json_write_string_or_null(
      logger_string_present(queue_summary.oldest_pending_study_day)
          ? queue_summary.oldest_pending_study_day
          : NULL);
  fputs(",\"last_failure_class\":", stdout);
  logger_json_write_string_or_null(
      logger_string_present(queue_summary.last_failure_class)
          ? queue_summary.last_failure_class
          : NULL);
  fputs("}", stdout);
  fputs(",\"last_day_outcome\":{\"study_day_local\":", stdout);
  logger_json_write_string_or_null(app->last_day_outcome_valid
                                       ? app->last_day_outcome_study_day_local
                                       : NULL);
  fputs(",\"kind\":", stdout);
  logger_json_write_string_or_null(
      app->last_day_outcome_valid ? app->last_day_outcome_kind : NULL);
  fputs(",\"reason\":", stdout);
  logger_json_write_string_or_null(
      app->last_day_outcome_valid ? app->last_day_outcome_reason : NULL);
  fputs("}", stdout);
  fputs(",\"firmware\":{\"version\":", stdout);
  logger_json_write_string_or_null(LOGGER_FIRMWARE_VERSION);
  fputs(",\"build_id\":", stdout);
  logger_json_write_string_or_null(LOGGER_BUILD_ID);
  fputs("}}", stdout);
}

static void logger_handle_status_json(logger_app_t *app) {
  logger_json_begin_success("status", logger_now_utc_or_null(app));
  logger_write_status_payload(app);
  logger_json_end_success();
}

static void logger_handle_provisioning_status_json(logger_app_t *app) {
  logger_json_begin_success("provisioning-status", logger_now_utc_or_null(app));
  fputs("{\"normal_logging_ready\":", stdout);
  fputs(logger_config_normal_logging_ready(&app->persisted.config) ? "true"
                                                                   : "false",
        stdout);
  fputs(",\"required_present\":[", stdout);
  logger_write_required_present_array(app);
  fputs("],\"required_missing\":[", stdout);
  logger_write_required_missing_array(app);
  fputs("],\"optional_present\":[", stdout);
  logger_write_optional_present_array(app);
  fputs("],\"warnings\":[", stdout);
  logger_write_warnings_array(app);
  fputs("]}", stdout);
  logger_json_end_success();
}

static void logger_handle_queue_json(logger_app_t *app) {
  logger_upload_queue_t queue;
  logger_upload_queue_init(&queue);
  if (!logger_upload_queue_load(&queue)) {
    (void)logger_upload_queue_scan(&queue, NULL, logger_now_utc_or_null(app));
  }

  logger_json_begin_success("queue", logger_now_utc_or_null(app));
  fputs("{\"schema_source\":\"upload_queue.json\",\"updated_at_utc\":", stdout);
  logger_json_write_string_or_null(logger_string_present(queue.updated_at_utc)
                                       ? queue.updated_at_utc
                                       : NULL);
  fputs(",\"sessions\":[", stdout);
  for (size_t i = 0u; i < queue.session_count; ++i) {
    const logger_upload_queue_entry_t *entry = &queue.sessions[i];
    if (i != 0u) {
      fputs(",", stdout);
    }
    fputs("{\"session_id\":", stdout);
    logger_json_write_string_or_null(entry->session_id);
    fputs(",\"study_day_local\":", stdout);
    logger_json_write_string_or_null(entry->study_day_local);
    fputs(",\"dir_name\":", stdout);
    logger_json_write_string_or_null(entry->dir_name);
    fputs(",\"session_start_utc\":", stdout);
    logger_json_write_string_or_null(entry->session_start_utc);
    fputs(",\"session_end_utc\":", stdout);
    logger_json_write_string_or_null(entry->session_end_utc);
    fputs(",\"bundle_sha256\":", stdout);
    logger_json_write_string_or_null(entry->bundle_sha256);
    fputs(",\"bundle_size_bytes\":", stdout);
    printf("%llu", (unsigned long long)entry->bundle_size_bytes);
    fputs(",\"status\":", stdout);
    logger_json_write_string_or_null(entry->status);
    fputs(",\"quarantined\":", stdout);
    fputs(entry->quarantined ? "true" : "false", stdout);
    fputs(",\"attempt_count\":", stdout);
    printf("%lu", (unsigned long)entry->attempt_count);
    fputs(",\"last_attempt_utc\":", stdout);
    logger_json_write_string_or_null(
        logger_string_present(entry->last_attempt_utc) ? entry->last_attempt_utc
                                                       : NULL);
    fputs(",\"last_failure_class\":", stdout);
    logger_json_write_string_or_null(
        logger_string_present(entry->last_failure_class)
            ? entry->last_failure_class
            : NULL);
    fputs(",\"verified_upload_utc\":", stdout);
    logger_json_write_string_or_null(
        logger_string_present(entry->verified_upload_utc)
            ? entry->verified_upload_utc
            : NULL);
    fputs(",\"receipt_id\":", stdout);
    logger_json_write_string_or_null(
        logger_string_present(entry->receipt_id) ? entry->receipt_id : NULL);
    fputs("}", stdout);
  }
  fputs("]}", stdout);
  logger_json_end_success();
}

static void logger_handle_system_log_export_json(logger_app_t *app) {
  logger_json_begin_success("system-log export", logger_now_utc_or_null(app));
  fputs("{\"schema_version\":1,\"exported_at_utc\":", stdout);
  logger_json_write_string_or_null(logger_now_utc_or_null(app));
  fputs(",\"events\":[", stdout);

  for (uint32_t i = 0u; i < logger_system_log_count(&app->system_log); ++i) {
    logger_system_log_event_t event;
    if (!logger_system_log_read_event(i, &event)) {
      continue;
    }
    if (i != 0u) {
      fputs(",", stdout);
    }
    fputs("{\"event_seq\":", stdout);
    printf("%lu", (unsigned long)event.event_seq);
    fputs(",\"utc\":", stdout);
    logger_json_write_string_or_null(
        logger_string_present(event.utc) ? event.utc : NULL);
    fputs(",\"boot_counter\":", stdout);
    printf("%lu", (unsigned long)event.boot_counter);
    fputs(",\"kind\":", stdout);
    logger_json_write_string_or_null(event.kind);
    fputs(",\"severity\":", stdout);
    logger_json_write_string_or_null(
        logger_system_log_severity_name(event.severity));
    fputs(",\"details\":", stdout);
    fputs(logger_string_present(event.details_json) ? event.details_json : "{}",
          stdout);
    fputs("}", stdout);
  }

  fputs("]}", stdout);
  logger_json_end_success();
}

static void logger_write_upload_tls_json(const logger_config_t *config) {
  const char *tls_mode = logger_config_upload_tls_mode(config);

  fputs("{\"mode\":", stdout);
  logger_json_write_string_or_null(tls_mode);
  fputs(",\"root_profile\":", stdout);
  logger_json_write_string_or_null(
      tls_mode != NULL &&
              strcmp(tls_mode, LOGGER_UPLOAD_TLS_MODE_PUBLIC_ROOTS) == 0
          ? LOGGER_UPLOAD_TLS_PUBLIC_ROOT_PROFILE
          : NULL);
  fputs(",\"anchor\":", stdout);

  if (tls_mode == NULL ||
      strcmp(tls_mode, LOGGER_UPLOAD_TLS_MODE_PROVISIONED_ANCHOR) != 0 ||
      !logger_config_upload_has_provisioned_anchor(config)) {
    fputs("null", stdout);
    fputs("}", stdout);
    return;
  }

  char der_base64[LOGGER_CONFIG_UPLOAD_TLS_ANCHOR_BASE64_MAX + 1u];
  size_t der_base64_len = 0u;
  if (mbedtls_base64_encode((unsigned char *)der_base64, sizeof(der_base64),
                            &der_base64_len, config->upload_tls_anchor_der,
                            config->upload_tls_anchor_der_len) != 0 ||
      der_base64_len >= sizeof(der_base64)) {
    fputs("null}", stdout);
    return;
  }
  der_base64[der_base64_len] = '\0';

  fputs("{\"format\":", stdout);
  logger_json_write_string_or_null(
      LOGGER_UPLOAD_TLS_ANCHOR_FORMAT_X509_DER_BASE64);
  fputs(",\"der_base64\":", stdout);
  logger_json_write_string_or_null(der_base64);
  fputs(",\"sha256\":", stdout);
  logger_json_write_string_or_null(
      logger_string_present(config->upload_tls_anchor_sha256)
          ? config->upload_tls_anchor_sha256
          : NULL);
  fputs(",\"subject\":", stdout);
  logger_json_write_string_or_null(
      logger_string_present(config->upload_tls_anchor_subject)
          ? config->upload_tls_anchor_subject
          : NULL);
  fputs("}}", stdout);
}

static void logger_handle_config_export_json(logger_app_t *app) {
  logger_json_begin_success("config export", logger_now_utc_or_null(app));
  fputs("{\"schema_version\":1,\"exported_at_utc\":", stdout);
  logger_json_write_string_or_null(logger_now_utc_or_null(app));
  fputs(",\"hardware_id\":", stdout);
  logger_json_write_string_or_null(app->hardware_id);
  fputs(",\"secrets_included\":false,\"identity\":{\"logger_id\":", stdout);
  logger_json_write_string_or_null(app->persisted.config.logger_id);
  fputs(",\"subject_id\":", stdout);
  logger_json_write_string_or_null(app->persisted.config.subject_id);
  fputs("},\"recording\":{\"bound_h10_address\":", stdout);
  logger_json_write_string_or_null(app->persisted.config.bound_h10_address);
  fputs(",\"study_day_rollover_local\":\"04:00:00\",\"overnight_upload_window_"
        "start_local\":\"22:00:00\",\"overnight_upload_window_end_local\":\"06:"
        "00:00\"}",
        stdout);
  fputs(",\"time\":{\"timezone\":", stdout);
  logger_json_write_string_or_null(app->persisted.config.timezone);
  fputs("},\"battery_policy\":{\"critical_stop_voltage_v\":3.5,\"low_start_"
        "voltage_v\":3.65,\"off_charger_upload_voltage_v\":3.85}",
        stdout);
  fputs(",\"wifi\":{\"allowed_ssids\":[", stdout);
  if (logger_string_present(app->persisted.config.wifi_ssid)) {
    logger_json_write_string_or_null(app->persisted.config.wifi_ssid);
  }
  fputs("],\"networks\":[", stdout);
  if (logger_string_present(app->persisted.config.wifi_ssid)) {
    fputs("{\"ssid\":", stdout);
    logger_json_write_string_or_null(app->persisted.config.wifi_ssid);
    fputs(",\"psk_present\":", stdout);
    fputs(logger_string_present(app->persisted.config.wifi_psk) ? "true"
                                                                : "false",
          stdout);
    fputs("}", stdout);
  }
  fputs("]},\"upload\":{\"enabled\":", stdout);
  fputs(logger_string_present(app->persisted.config.upload_url) ? "true"
                                                                : "false",
        stdout);
  fputs(",\"url\":", stdout);
  logger_json_write_string_or_null(app->persisted.config.upload_url);
  fputs(",\"auth\":{\"type\":", stdout);
  logger_json_write_string_or_null(
      logger_string_present(app->persisted.config.upload_url)
          ? "api_key_and_bearer"
          : "none");
  fputs(",\"api_key_present\":", stdout);
  fputs(logger_string_present(app->persisted.config.upload_api_key) ? "true"
                                                                    : "false",
        stdout);
  fputs(",\"token_present\":", stdout);
  fputs(logger_string_present(app->persisted.config.upload_token) ? "true"
                                                                  : "false",
        stdout);
  fputs("},\"tls\":", stdout);
  logger_write_upload_tls_json(&app->persisted.config);
  fputs("}}", stdout);
  logger_json_end_success();
}

static void logger_handle_clock_status_json(logger_app_t *app) {
  logger_json_begin_success("clock status", logger_now_utc_or_null(app));
  fputs("{\"rtc_present\":", stdout);
  fputs(app->clock.rtc_present ? "true" : "false", stdout);
  fputs(",\"valid\":", stdout);
  fputs(app->clock.valid ? "true" : "false", stdout);
  fputs(",\"lost_power\":", stdout);
  fputs(app->clock.lost_power ? "true" : "false", stdout);
  fputs(",\"battery_low\":", stdout);
  fputs(app->clock.battery_low ? "true" : "false", stdout);
  fputs(",\"state\":", stdout);
  logger_json_write_string_or_null(logger_clock_state_name(&app->clock));
  fputs(",\"now_utc\":", stdout);
  logger_json_write_string_or_null(logger_now_utc_or_null(app));
  fputs(",\"timezone\":", stdout);
  logger_json_write_string_or_null(app->persisted.config.timezone);
  fputs("}", stdout);
  logger_json_end_success();
}

static void logger_handle_preflight_json(logger_app_t *app) {
  if (logger_cli_is_logging_mode(app)) {
    logger_json_begin_error("preflight", logger_now_utc_or_null(app),
                            "busy_logging",
                            "preflight is not permitted while logging");
    return;
  }
  if (logger_cli_is_upload_mode(app)) {
    logger_json_begin_error("preflight", logger_now_utc_or_null(app),
                            "not_permitted_in_mode",
                            "preflight is not permitted during upload");
    return;
  }

  const char *rtc_result =
      !app->clock.rtc_present ? "fail" : (app->clock.valid ? "pass" : "warn");
  const char *storage_result =
      logger_storage_ready_for_logging(&app->storage) ? "pass" : "fail";
  const char *battery_result = app->battery.initialized ? "pass" : "fail";
  const char *prov_result =
      logger_config_normal_logging_ready(&app->persisted.config) ? "pass"
                                                                 : "fail";
  const char *h10_scan_result =
      logger_string_present(app->persisted.config.bound_h10_address) ? "warn"
                                                                     : "fail";

  const char *overall = "pass";
  if (strcmp(rtc_result, "fail") == 0 || strcmp(storage_result, "fail") == 0 ||
      strcmp(battery_result, "fail") == 0 || strcmp(prov_result, "fail") == 0 ||
      strcmp(h10_scan_result, "fail") == 0) {
    overall = "fail";
  } else if (strcmp(rtc_result, "warn") == 0 ||
             strcmp(h10_scan_result, "warn") == 0) {
    overall = "warn";
  }

  logger_json_begin_success("preflight", logger_now_utc_or_null(app));
  fputs("{\"overall_result\":", stdout);
  logger_json_write_string_or_null(overall);
  fputs(",\"checks\":[", stdout);
  fputs("{\"name\":\"rtc\",\"result\":", stdout);
  logger_json_write_string_or_null(rtc_result);
  fputs(",\"details\":{\"rtc_present\":", stdout);
  fputs(app->clock.rtc_present ? "true" : "false", stdout);
  fputs(",\"valid\":", stdout);
  fputs(app->clock.valid ? "true" : "false", stdout);
  fputs(",\"lost_power\":", stdout);
  fputs(app->clock.lost_power ? "true" : "false", stdout);
  fputs("}}", stdout);

  fputs(",{\"name\":\"storage\",\"result\":", stdout);
  logger_json_write_string_or_null(storage_result);
  fputs(",\"details\":{\"mounted\":", stdout);
  fputs(app->storage.mounted ? "true" : "false", stdout);
  fputs(",\"filesystem\":", stdout);
  logger_json_write_string_or_null(
      logger_string_present(app->storage.filesystem) ? app->storage.filesystem
                                                     : NULL);
  fputs(",\"free_bytes\":", stdout);
  if (app->storage.mounted) {
    printf("%llu", (unsigned long long)app->storage.free_bytes);
  } else {
    fputs("null", stdout);
  }
  fputs(",\"reserve_ok\":", stdout);
  fputs(app->storage.reserve_ok ? "true" : "false", stdout);
  fputs("}}", stdout);

  fputs(",{\"name\":\"battery_sense\",\"result\":", stdout);
  logger_json_write_string_or_null(battery_result);
  fputs(",\"details\":{\"voltage_mv\":", stdout);
  printf("%u", (unsigned)app->battery.voltage_mv);
  fputs(",\"vbus_present\":", stdout);
  fputs(app->battery.vbus_present ? "true" : "false", stdout);
  fputs("}}", stdout);

  fputs(",{\"name\":\"provisioning\",\"result\":", stdout);
  logger_json_write_string_or_null(prov_result);
  fputs(",\"details\":{\"normal_logging_ready\":", stdout);
  fputs(logger_config_normal_logging_ready(&app->persisted.config) ? "true"
                                                                   : "false",
        stdout);
  fputs("}}", stdout);

  fputs(",{\"name\":\"bound_h10_scan\",\"result\":", stdout);
  logger_json_write_string_or_null(h10_scan_result);
  fputs(",\"details\":{\"implemented\":false,\"bound_address\":", stdout);
  logger_json_write_string_or_null(app->persisted.config.bound_h10_address);
  fputs("}}]}", stdout);
  logger_json_end_success();
}

static void logger_handle_net_test_json(logger_app_t *app) {
  if (logger_cli_is_logging_mode(app)) {
    logger_json_begin_error("net-test", logger_now_utc_or_null(app),
                            "busy_logging",
                            "net-test is not permitted while logging");
    return;
  }
  if (!logger_cli_is_service_mode(app)) {
    logger_json_begin_error("net-test", logger_now_utc_or_null(app),
                            "not_permitted_in_mode",
                            "net-test is only allowed in service mode");
    return;
  }

  logger_upload_net_test_result_t result;
  const bool ok = logger_upload_net_test(&app->persisted.config, &result);

  logger_json_begin_success("net-test", logger_now_utc_or_null(app));
  fputs("{\"wifi_join\":{\"result\":", stdout);
  logger_json_write_string_or_null(result.wifi_join_result);
  fputs(",\"details\":{\"message\":", stdout);
  logger_json_write_string_or_null(result.wifi_join_details);
  fputs("}},\"dns\":{\"result\":", stdout);
  logger_json_write_string_or_null(result.dns_result);
  fputs(",\"details\":{\"message\":", stdout);
  logger_json_write_string_or_null(result.dns_details);
  fputs("}},\"tls\":{\"result\":", stdout);
  logger_json_write_string_or_null(result.tls_result);
  fputs(",\"details\":{\"message\":", stdout);
  logger_json_write_string_or_null(result.tls_details);
  fputs("}},\"upload_endpoint_reachable\":{\"result\":", stdout);
  logger_json_write_string_or_null(result.upload_endpoint_reachable_result);
  fputs(",\"details\":{\"message\":", stdout);
  logger_json_write_string_or_null(result.upload_endpoint_reachable_details);
  fputs(",\"ok\":", stdout);
  fputs(ok ? "true" : "false", stdout);
  fputs("}}}", stdout);
  logger_json_end_success();
}

static void logger_handle_service_unlock(logger_service_cli_t *cli,
                                         logger_app_t *app, uint32_t now_ms) {
  if (logger_cli_is_logging_mode(app)) {
    logger_json_begin_error("service unlock", logger_now_utc_or_null(app),
                            "busy_logging",
                            "service unlock is not permitted while logging");
    return;
  }
  if (!logger_cli_is_service_mode(app)) {
    logger_json_begin_error("service unlock", logger_now_utc_or_null(app),
                            "not_permitted_in_mode",
                            "service unlock is only allowed in service mode");
    return;
  }

  cli->unlocked = true;
  cli->unlock_deadline_mono_ms = now_ms + 60000u;
  (void)logger_system_log_append(&app->system_log, logger_now_utc_or_null(app),
                                 "service_unlock",
                                 LOGGER_SYSTEM_LOG_SEVERITY_INFO, "{}");

  logger_json_begin_success("service unlock", logger_now_utc_or_null(app));
  fputs("{\"unlocked\":true,\"expires_at_utc\":", stdout);
  logger_json_write_string_or_null(logger_now_utc_or_null(app));
  fputs(",\"ttl_seconds\":60}", stdout);
  logger_json_end_success();
}

static void logger_handle_service_enter(logger_app_t *app, uint32_t now_ms) {
  if (logger_cli_is_upload_mode(app)) {
    logger_json_begin_error("service enter", logger_now_utc_or_null(app),
                            "not_permitted_in_mode",
                            "service enter is not permitted during upload");
    return;
  }

  const bool already_in_service = logger_cli_is_service_mode(app);
  bool will_stop_logging = false;
  if (!logger_app_request_service_mode(app, now_ms, &will_stop_logging)) {
    logger_json_begin_error(
        "service enter", logger_now_utc_or_null(app), "not_permitted_in_mode",
        "service enter is not permitted in the current mode");
    return;
  }

  (void)logger_system_log_append(
      &app->system_log, logger_now_utc_or_null(app), "service_request",
      LOGGER_SYSTEM_LOG_SEVERITY_INFO,
      already_in_service
          ? "{\"source\":\"host\",\"already_in_service\":true}"
          : "{\"source\":\"host\",\"already_in_service\":false}");

  logger_json_begin_success("service enter", logger_now_utc_or_null(app));
  fputs("{\"requested\":true,\"already_in_service\":", stdout);
  fputs(already_in_service ? "true" : "false", stdout);
  fputs(",\"will_stop_logging\":", stdout);
  fputs(will_stop_logging ? "true" : "false", stdout);
  fputs(",\"mode\":", stdout);
  logger_json_write_string_or_null(logger_mode_name(&app->runtime));
  fputs(",\"runtime_state\":", stdout);
  logger_json_write_string_or_null(
      logger_runtime_state_name(app->runtime.current_state));
  fputs(",\"target_mode\":\"service\"}", stdout);
  logger_json_end_success();
}

static void logger_handle_fault_clear(logger_app_t *app) {
  if (logger_cli_is_logging_mode(app)) {
    logger_json_begin_error("fault clear", logger_now_utc_or_null(app),
                            "busy_logging",
                            "fault clear is not permitted while logging");
    return;
  }
  if (logger_cli_is_upload_mode(app)) {
    logger_json_begin_error("fault clear", logger_now_utc_or_null(app),
                            "not_permitted_in_mode",
                            "fault clear is not permitted during upload");
    return;
  }

  const logger_fault_code_t previous = app->persisted.current_fault_code;
  if (previous == LOGGER_FAULT_NONE) {
    logger_json_begin_success("fault clear", logger_now_utc_or_null(app));
    fputs("{\"cleared\":false,\"previous_code\":null}", stdout);
    logger_json_end_success();
    return;
  }
  if (logger_cli_fault_condition_still_present(app)) {
    logger_json_begin_error("fault clear", logger_now_utc_or_null(app),
                            "condition_still_present",
                            "fault condition is still present");
    return;
  }

  app->persisted.last_cleared_fault_code = previous;
  app->persisted.current_fault_code = LOGGER_FAULT_NONE;
  (void)logger_config_store_save(&app->persisted);
  (void)logger_system_log_append(&app->system_log, logger_now_utc_or_null(app),
                                 "fault_cleared",
                                 LOGGER_SYSTEM_LOG_SEVERITY_INFO, "{}");

  logger_json_begin_success("fault clear", logger_now_utc_or_null(app));
  fputs("{\"cleared\":true,\"previous_code\":", stdout);
  logger_json_write_string_or_null(logger_fault_code_name(previous));
  fputs("}", stdout);
  logger_json_end_success();
}

static void logger_handle_factory_reset(logger_service_cli_t *cli,
                                        logger_app_t *app) {
  if (logger_cli_is_logging_mode(app)) {
    logger_json_begin_error("factory-reset", logger_now_utc_or_null(app),
                            "busy_logging",
                            "factory reset is not permitted while logging");
    return;
  }
  if (!logger_cli_is_service_mode(app)) {
    logger_json_begin_error("factory-reset", logger_now_utc_or_null(app),
                            "not_permitted_in_mode",
                            "factory reset is only allowed in service mode");
    return;
  }
  if (!logger_service_cli_is_unlocked(cli,
                                      to_ms_since_boot(get_absolute_time()))) {
    logger_json_begin_error("factory-reset", logger_now_utc_or_null(app),
                            "service_locked",
                            "service unlock is required before factory reset");
    return;
  }

  (void)logger_system_log_append(
      &app->system_log, logger_now_utc_or_null(app), "factory_reset",
      LOGGER_SYSTEM_LOG_SEVERITY_WARN, "{\"source\":\"service_cli\"}");
  (void)logger_config_store_factory_reset(&app->persisted);
  cli->unlocked = false;
  app->reboot_pending = true;

  logger_json_begin_success("factory-reset", logger_now_utc_or_null(app));
  fputs("{\"factory_reset\":true,\"will_reboot\":true}", stdout);
  logger_json_end_success();
}

static void logger_handle_sd_format(logger_service_cli_t *cli,
                                    logger_app_t *app) {
  if (logger_cli_is_logging_mode(app)) {
    logger_json_begin_error("sd format", logger_now_utc_or_null(app),
                            "busy_logging",
                            "sd format is not permitted while logging");
    return;
  }
  if (!logger_cli_is_service_mode(app)) {
    logger_json_begin_error("sd format", logger_now_utc_or_null(app),
                            "not_permitted_in_mode",
                            "sd format is only allowed in service mode");
    return;
  }
  if (app->session.active) {
    logger_json_begin_error(
        "sd format", logger_now_utc_or_null(app), "not_permitted_in_mode",
        "sd format is not permitted while a session is active");
    return;
  }
  if (!logger_service_cli_is_unlocked(cli,
                                      to_ms_since_boot(get_absolute_time()))) {
    logger_json_begin_error("sd format", logger_now_utc_or_null(app),
                            "service_locked",
                            "service unlock is required before sd format");
    return;
  }

  logger_storage_status_t formatted;
  if (!logger_storage_format(&formatted)) {
    (void)logger_storage_refresh(&app->storage);
    logger_json_begin_error(
        "sd format", logger_now_utc_or_null(app), "storage_unavailable",
        "failed to format and remount the SD card as FAT32");
    return;
  }

  logger_upload_queue_summary_t queue_summary;
  if (!logger_upload_queue_refresh_file(
          &app->system_log, logger_now_utc_or_null(app), &queue_summary)) {
    (void)logger_storage_refresh(&app->storage);
    logger_json_begin_error(
        "sd format", logger_now_utc_or_null(app), "storage_unavailable",
        "formatted SD card but failed to initialize logger queue state");
    return;
  }

  (void)logger_storage_refresh(&app->storage);
  (void)queue_summary;
  (void)logger_system_log_append(
      &app->system_log, logger_now_utc_or_null(app), "sd_formatted",
      LOGGER_SYSTEM_LOG_SEVERITY_WARN,
      "{\"filesystem\":\"fat32\",\"logger_root_created\":true}");

  cli->unlocked = false;

  logger_json_begin_success("sd format", logger_now_utc_or_null(app));
  fputs("{\"formatted\":true,\"filesystem\":\"fat32\",\"logger_root_created\":"
        "true}",
        stdout);
  logger_json_end_success();
}

static void logger_handle_clock_set(logger_service_cli_t *cli,
                                    logger_app_t *app, const char *value) {
  if (logger_cli_is_logging_mode(app)) {
    logger_json_begin_error("clock set", logger_now_utc_or_null(app),
                            "busy_logging",
                            "clock set is not permitted while logging");
    return;
  }
  if (!logger_cli_is_service_mode(app)) {
    logger_json_begin_error("clock set", logger_now_utc_or_null(app),
                            "not_permitted_in_mode",
                            "clock set is only allowed in service mode");
    return;
  }
  if (!logger_service_cli_is_unlocked(cli,
                                      to_ms_since_boot(get_absolute_time()))) {
    logger_json_begin_error("clock set", logger_now_utc_or_null(app),
                            "service_locked",
                            "service unlock is required before clock set");
    return;
  }
  if (!logger_clock_set_utc(value, &app->clock)) {
    logger_json_begin_error("clock set", logger_now_utc_or_null(app),
                            "invalid_config", "invalid RFC3339 UTC timestamp");
    return;
  }
  logger_app_note_wall_clock_changed(app);
  (void)logger_system_log_append(&app->system_log, logger_now_utc_or_null(app),
                                 "clock_set", LOGGER_SYSTEM_LOG_SEVERITY_INFO,
                                 "{}");

  logger_json_begin_success("clock set", logger_now_utc_or_null(app));
  fputs("{\"applied\":true,\"now_utc\":", stdout);
  logger_json_write_string_or_null(logger_now_utc_or_null(app));
  fputs("}", stdout);
  logger_json_end_success();
}

static void logger_handle_clock_sync(logger_service_cli_t *cli,
                                     logger_app_t *app) {
  if (logger_cli_is_logging_mode(app)) {
    logger_json_begin_error("clock sync", logger_now_utc_or_null(app),
                            "busy_logging",
                            "clock sync is not permitted while logging");
    return;
  }
  if (!logger_cli_is_service_mode(app)) {
    logger_json_begin_error("clock sync", logger_now_utc_or_null(app),
                            "not_permitted_in_mode",
                            "clock sync is only allowed in service mode");
    return;
  }
  if (!logger_service_cli_is_unlocked(cli,
                                      to_ms_since_boot(get_absolute_time()))) {
    logger_json_begin_error("clock sync", logger_now_utc_or_null(app),
                            "service_locked",
                            "service unlock is required before clock sync");
    return;
  }

  logger_clock_ntp_sync_result_t result;
  const bool ok = logger_app_clock_sync_ntp(app, &result);

  logger_json_begin_success("clock sync", logger_now_utc_or_null(app));
  fputs("{\"attempted\":", stdout);
  fputs(result.attempted ? "true" : "false", stdout);
  fputs(",\"applied\":", stdout);
  fputs(ok ? "true" : "false", stdout);
  fputs(",\"previous_valid\":", stdout);
  fputs(result.previous_valid ? "true" : "false", stdout);
  fputs(",\"large_correction\":", stdout);
  fputs(result.large_correction ? "true" : "false", stdout);
  fputs(",\"correction_seconds\":", stdout);
  printf("%lld", (long long)result.correction_seconds);
  fputs(",\"server\":", stdout);
  logger_json_write_string_or_null(result.server[0] != '\0' ? result.server
                                                            : NULL);
  fputs(",\"remote_address\":", stdout);
  logger_json_write_string_or_null(
      result.remote_address[0] != '\0' ? result.remote_address : NULL);
  fputs(",\"stratum\":", stdout);
  if (result.stratum != 0u) {
    printf("%u", (unsigned)result.stratum);
  } else {
    fputs("null", stdout);
  }
  fputs(",\"previous_utc\":", stdout);
  logger_json_write_string_or_null(
      result.previous_utc[0] != '\0' ? result.previous_utc : NULL);
  fputs(",\"now_utc\":", stdout);
  logger_json_write_string_or_null(logger_now_utc_or_null(app));
  fputs(",\"message\":", stdout);
  logger_json_write_string_or_null(result.message[0] != '\0' ? result.message
                                                             : NULL);
  fputs("}", stdout);
  logger_json_end_success();
}

static void logger_apply_config_import_json(logger_service_cli_t *cli,
                                            logger_app_t *app,
                                            const char *command,
                                            const char *json,
                                            bool clear_transfer_on_success) {
  logger_persisted_state_t imported;
  bool bond_cleared = false;
  const char *error_message = "config import failed";
  if (!logger_parse_config_import_document(app, json, &imported, &bond_cleared,
                                           &error_message)) {
    logger_json_begin_error(command, logger_now_utc_or_null(app),
                            "invalid_config", error_message);
    return;
  }
  if (!logger_config_store_save(&imported)) {
    logger_json_begin_error(command, logger_now_utc_or_null(app),
                            "storage_unavailable",
                            "failed to persist imported config");
    return;
  }

  app->persisted = imported;
  if (bond_cleared) {
    app->h10.bonded = false;
    app->h10.connected = false;
    app->h10.encrypted = false;
    app->h10.last_seen_address[0] = '\0';
  }
  (void)logger_h10_set_bound_address(&app->h10,
                                     app->persisted.config.bound_h10_address);
  app->runtime.provisioning_complete =
      logger_config_normal_logging_ready(&app->persisted.config);

  char details[LOGGER_SYSTEM_LOG_DETAILS_JSON_MAX + 1];
  logger_json_object_writer_t writer;
  logger_json_object_writer_init(&writer, details, sizeof(details));
  if (logger_json_object_writer_string_field(&writer, "source",
                                             "config_import") &&
      logger_json_object_writer_bool_field(&writer, "bond_cleared",
                                           bond_cleared) &&
      logger_json_object_writer_finish(&writer)) {
    (void)logger_system_log_append(
        &app->system_log, logger_now_utc_or_null(app), "config_changed",
        LOGGER_SYSTEM_LOG_SEVERITY_INFO,
        logger_json_object_writer_data(&writer));
  }

  if (clear_transfer_on_success) {
    logger_config_import_transfer_reset(cli);
  }
  cli->unlocked = false;

  logger_json_begin_success(command, logger_now_utc_or_null(app));
  fputs("{\"applied\":true,\"normal_logging_ready\":", stdout);
  fputs(logger_config_normal_logging_ready(&app->persisted.config) ? "true"
                                                                   : "false",
        stdout);
  fputs(",\"bond_cleared\":", stdout);
  fputs(bond_cleared ? "true" : "false", stdout);
  fputs("}", stdout);
  logger_json_end_success();
}

static void logger_handle_config_import(logger_service_cli_t *cli,
                                        logger_app_t *app, const char *json) {
  if (!logger_require_config_import_context(cli, app, "config import", true)) {
    return;
  }
  if (!logger_string_present(json)) {
    logger_json_begin_error("config import", logger_now_utc_or_null(app),
                            "invalid_config",
                            "expected: config import <json>\nor use: config "
                            "import begin <bytes> / chunk / commit");
    return;
  }
  logger_apply_config_import_json(cli, app, "config import", json, true);
}

static void logger_handle_config_import_begin(logger_service_cli_t *cli,
                                              logger_app_t *app,
                                              const char *args) {
  if (!logger_require_config_import_context(cli, app, "config import begin",
                                            true)) {
    return;
  }

  char size_text[24] = {0};
  char extra[8] = {0};
  const int matched = sscanf(args, "%23s %7s", size_text, extra);
  if (matched != 1) {
    logger_json_begin_error("config import begin", logger_now_utc_or_null(app),
                            "invalid_config",
                            "expected: config import begin <total_bytes>");
    return;
  }

  size_t expected_len = 0u;
  if (!logger_parse_size_t_strict(size_text, &expected_len) ||
      expected_len == 0u) {
    logger_json_begin_error(
        "config import begin", logger_now_utc_or_null(app), "invalid_config",
        "config import begin requires a positive byte count");
    return;
  }
  if (expected_len > LOGGER_SERVICE_CLI_CONFIG_IMPORT_JSON_MAX) {
    logger_json_begin_error("config import begin", logger_now_utc_or_null(app),
                            "invalid_config",
                            "config import size exceeds transport buffer");
    return;
  }

  const bool replaced_existing = cli->config_import_active;
  logger_config_import_transfer_reset(cli);
  cli->config_import_active = true;
  cli->config_import_expected_len = expected_len;

  logger_json_begin_success("config import begin", logger_now_utc_or_null(app));
  fputs("{\"started\":true,\"expected_bytes\":", stdout);
  printf("%llu", (unsigned long long)cli->config_import_expected_len);
  fputs(",\"received_bytes\":0,\"remaining_bytes\":", stdout);
  printf("%llu", (unsigned long long)cli->config_import_expected_len);
  fputs(",\"max_bytes\":", stdout);
  printf("%u", (unsigned)LOGGER_SERVICE_CLI_CONFIG_IMPORT_JSON_MAX);
  fputs(",\"replaced_existing\":", stdout);
  fputs(replaced_existing ? "true" : "false", stdout);
  fputs("}", stdout);
  logger_json_end_success();
}

static void logger_handle_config_import_chunk(logger_service_cli_t *cli,
                                              logger_app_t *app,
                                              const char *chunk) {
  if (!logger_require_config_import_context(cli, app, "config import chunk",
                                            true)) {
    return;
  }
  if (!cli->config_import_active) {
    logger_json_begin_error(
        "config import chunk", logger_now_utc_or_null(app), "invalid_config",
        "no config import transfer is active; use config import begin first");
    return;
  }

  const size_t chunk_len = strlen(chunk);
  if (chunk_len == 0u) {
    logger_json_begin_error(
        "config import chunk", logger_now_utc_or_null(app), "invalid_config",
        "config import chunk requires a non-empty payload fragment");
    return;
  }

  if ((cli->config_import_received_len + chunk_len) >
          cli->config_import_expected_len ||
      (cli->config_import_received_len + chunk_len) >
          LOGGER_SERVICE_CLI_CONFIG_IMPORT_JSON_MAX) {
    logger_config_import_transfer_reset(cli);
    logger_json_begin_error("config import chunk", logger_now_utc_or_null(app),
                            "invalid_config",
                            "config import chunk exceeds announced transfer "
                            "size; transfer aborted");
    return;
  }

  memcpy(cli->config_import_buf + cli->config_import_received_len, chunk,
         chunk_len);
  cli->config_import_received_len += chunk_len;
  cli->config_import_buf[cli->config_import_received_len] = '\0';
  cli->config_import_chunk_count += 1u;

  logger_json_begin_success("config import chunk", logger_now_utc_or_null(app));
  fputs("{\"accepted\":true,\"chunk_bytes\":", stdout);
  printf("%llu", (unsigned long long)chunk_len);
  fputs(",\"received_bytes\":", stdout);
  printf("%llu", (unsigned long long)cli->config_import_received_len);
  fputs(",\"expected_bytes\":", stdout);
  printf("%llu", (unsigned long long)cli->config_import_expected_len);
  fputs(",\"remaining_bytes\":", stdout);
  printf("%llu", (unsigned long long)(cli->config_import_expected_len -
                                      cli->config_import_received_len));
  fputs(",\"chunk_count\":", stdout);
  printf("%lu", (unsigned long)cli->config_import_chunk_count);
  fputs(",\"complete\":", stdout);
  fputs(cli->config_import_received_len == cli->config_import_expected_len
            ? "true"
            : "false",
        stdout);
  fputs("}", stdout);
  logger_json_end_success();
}

static void logger_handle_config_import_status(logger_service_cli_t *cli,
                                               logger_app_t *app) {
  if (!logger_require_config_import_context(cli, app, "config import status",
                                            false)) {
    return;
  }

  logger_json_begin_success("config import status",
                            logger_now_utc_or_null(app));
  fputs("{\"active\":", stdout);
  fputs(cli->config_import_active ? "true" : "false", stdout);
  fputs(",\"expected_bytes\":", stdout);
  printf("%llu", (unsigned long long)cli->config_import_expected_len);
  fputs(",\"received_bytes\":", stdout);
  printf("%llu", (unsigned long long)cli->config_import_received_len);
  fputs(",\"remaining_bytes\":", stdout);
  if (cli->config_import_received_len <= cli->config_import_expected_len) {
    printf("%llu", (unsigned long long)(cli->config_import_expected_len -
                                        cli->config_import_received_len));
  } else {
    fputs("0", stdout);
  }
  fputs(",\"chunk_count\":", stdout);
  printf("%lu", (unsigned long)cli->config_import_chunk_count);
  fputs(",\"max_bytes\":", stdout);
  printf("%u", (unsigned)LOGGER_SERVICE_CLI_CONFIG_IMPORT_JSON_MAX);
  fputs(",\"ready_to_commit\":", stdout);
  fputs(cli->config_import_active && cli->config_import_received_len ==
                                         cli->config_import_expected_len
            ? "true"
            : "false",
        stdout);
  fputs("}", stdout);
  logger_json_end_success();
}

static void logger_handle_config_import_cancel(logger_service_cli_t *cli,
                                               logger_app_t *app) {
  if (!logger_require_config_import_context(cli, app, "config import cancel",
                                            false)) {
    return;
  }

  const bool had_transfer = cli->config_import_active ||
                            cli->config_import_received_len != 0u ||
                            cli->config_import_expected_len != 0u ||
                            cli->config_import_chunk_count != 0u;
  logger_config_import_transfer_reset(cli);

  logger_json_begin_success("config import cancel",
                            logger_now_utc_or_null(app));
  fputs("{\"cleared\":true,\"had_transfer\":", stdout);
  fputs(had_transfer ? "true" : "false", stdout);
  fputs("}", stdout);
  logger_json_end_success();
}

static void logger_handle_config_import_commit(logger_service_cli_t *cli,
                                               logger_app_t *app) {
  if (!logger_require_config_import_context(cli, app, "config import commit",
                                            true)) {
    return;
  }
  if (!cli->config_import_active) {
    logger_json_begin_error(
        "config import commit", logger_now_utc_or_null(app), "invalid_config",
        "no config import transfer is active; use config import begin first");
    return;
  }
  if (cli->config_import_received_len != cli->config_import_expected_len) {
    logger_json_begin_error("config import commit", logger_now_utc_or_null(app),
                            "invalid_config",
                            "config import transfer is incomplete");
    return;
  }

  cli->config_import_buf[cli->config_import_received_len] = '\0';
  logger_apply_config_import_json(cli, app, "config import commit",
                                  cli->config_import_buf, true);
}

static void
logger_handle_upload_tls_clear_provisioned_anchor(logger_service_cli_t *cli,
                                                  logger_app_t *app) {
  if (logger_cli_is_logging_mode(app)) {
    logger_json_begin_error(
        "upload tls clear-provisioned-anchor", logger_now_utc_or_null(app),
        "busy_logging",
        "upload tls clear-provisioned-anchor is not permitted while logging");
    return;
  }
  if (!logger_cli_is_service_mode(app)) {
    logger_json_begin_error(
        "upload tls clear-provisioned-anchor", logger_now_utc_or_null(app),
        "not_permitted_in_mode",
        "upload tls clear-provisioned-anchor is only allowed in service mode");
    return;
  }
  if (!logger_service_cli_is_unlocked(cli,
                                      to_ms_since_boot(get_absolute_time()))) {
    logger_json_begin_error("upload tls clear-provisioned-anchor",
                            logger_now_utc_or_null(app), "service_locked",
                            "service unlock is required before upload tls "
                            "clear-provisioned-anchor");
    return;
  }

  bool had_anchor = false;
  if (!logger_config_clear_provisioned_anchor(&app->persisted, &had_anchor)) {
    logger_json_begin_error("upload tls clear-provisioned-anchor",
                            logger_now_utc_or_null(app), "storage_unavailable",
                            "failed to clear provisioned upload TLS anchor");
    return;
  }

  char details[LOGGER_SYSTEM_LOG_DETAILS_JSON_MAX + 1];
  logger_json_object_writer_t writer;
  logger_json_object_writer_init(&writer, details, sizeof(details));
  if (logger_json_object_writer_string_field(
          &writer, "source", "upload_tls_clear_provisioned_anchor") &&
      logger_json_object_writer_bool_field(&writer, "had_anchor", had_anchor) &&
      logger_json_object_writer_finish(&writer)) {
    (void)logger_system_log_append(
        &app->system_log, logger_now_utc_or_null(app), "config_changed",
        LOGGER_SYSTEM_LOG_SEVERITY_INFO,
        logger_json_object_writer_data(&writer));
  }

  cli->unlocked = false;

  logger_json_begin_success("upload tls clear-provisioned-anchor",
                            logger_now_utc_or_null(app));
  fputs("{\"cleared\":true,\"had_anchor\":", stdout);
  fputs(had_anchor ? "true" : "false", stdout);
  fputs(",\"current_tls_mode\":", stdout);
  logger_json_write_string_or_null(
      logger_config_upload_tls_mode(&app->persisted.config));
  fputs(",\"upload_ready\":", stdout);
  fputs(logger_config_upload_ready(&app->persisted.config) ? "true" : "false",
        stdout);
  fputs("}", stdout);
  logger_json_end_success();
}

static void logger_handle_debug_config_set(logger_service_cli_t *cli,
                                           logger_app_t *app,
                                           const char *args) {
  char field[48];
  char value[256];
  field[0] = '\0';
  value[0] = '\0';
  const int matched = sscanf(args, "%47s %255[^\n]", field, value);
  if (matched < 2) {
    logger_json_begin_error("debug config set", logger_now_utc_or_null(app),
                            "invalid_config",
                            "expected: debug config set <field> <value>");
    return;
  }

  const bool upload_debug_field = strcmp(field, "wifi_ssid") == 0 ||
                                  strcmp(field, "wifi_psk") == 0 ||
                                  strcmp(field, "upload_url") == 0 ||
                                  strcmp(field, "upload_api_key") == 0 ||
                                  strcmp(field, "upload_token") == 0;
  const bool allow_in_log_wait_h10 =
      upload_debug_field &&
      app->runtime.current_state == LOGGER_RUNTIME_LOG_WAIT_H10;

  if (logger_cli_is_logging_mode(app) && !allow_in_log_wait_h10) {
    logger_json_begin_error("debug config set", logger_now_utc_or_null(app),
                            "busy_logging",
                            "config mutation is not permitted while logging");
    return;
  }
  if (!logger_cli_is_service_mode(app) && !allow_in_log_wait_h10) {
    logger_json_begin_error("debug config set", logger_now_utc_or_null(app),
                            "not_permitted_in_mode",
                            "debug config set is only allowed in service mode");
    return;
  }
  if (logger_cli_is_service_mode(app) &&
      !logger_service_cli_is_unlocked(cli,
                                      to_ms_since_boot(get_absolute_time()))) {
    logger_json_begin_error(
        "debug config set", logger_now_utc_or_null(app), "service_locked",
        "service unlock is required before debug config set");
    return;
  }

  bool ok = false;
  bool bond_cleared = false;
  if (strcmp(field, "logger_id") == 0) {
    ok = logger_config_set_logger_id(&app->persisted, value);
  } else if (strcmp(field, "subject_id") == 0) {
    ok = logger_config_set_subject_id(&app->persisted, value);
  } else if (strcmp(field, "bound_h10_address") == 0) {
    ok = logger_config_set_bound_h10_address(&app->persisted, value,
                                             &bond_cleared);
  } else if (strcmp(field, "timezone") == 0) {
    ok = logger_config_set_timezone(&app->persisted, value);
  } else if (strcmp(field, "wifi_ssid") == 0) {
    ok = logger_config_set_wifi_ssid(&app->persisted, value);
  } else if (strcmp(field, "wifi_psk") == 0) {
    ok = logger_config_set_wifi_psk(&app->persisted, value);
  } else if (strcmp(field, "upload_url") == 0) {
    ok = logger_config_set_upload_url(&app->persisted, value);
  } else if (strcmp(field, "upload_api_key") == 0) {
    ok = logger_config_set_upload_api_key(&app->persisted, value);
  } else if (strcmp(field, "upload_token") == 0) {
    ok = logger_config_set_upload_token(&app->persisted, value);
  } else {
    logger_json_begin_error("debug config set", logger_now_utc_or_null(app),
                            "invalid_config", "unknown debug config field");
    return;
  }

  if (!ok) {
    logger_json_begin_error("debug config set", logger_now_utc_or_null(app),
                            "invalid_config",
                            "failed to apply debug config field");
    return;
  }

  char details[LOGGER_SYSTEM_LOG_DETAILS_JSON_MAX + 1];
  logger_json_object_writer_t writer;
  logger_json_object_writer_init(&writer, details, sizeof(details));
  if (logger_json_object_writer_string_field(&writer, "field", field) &&
      logger_json_object_writer_finish(&writer)) {
    (void)logger_system_log_append(
        &app->system_log, logger_now_utc_or_null(app), "config_changed",
        LOGGER_SYSTEM_LOG_SEVERITY_INFO,
        logger_json_object_writer_data(&writer));
  }

  logger_json_begin_success("debug config set", logger_now_utc_or_null(app));
  fputs("{\"applied\":true,\"field\":", stdout);
  logger_json_write_string_or_null(field);
  fputs(",\"normal_logging_ready\":", stdout);
  fputs(logger_config_normal_logging_ready(&app->persisted.config) ? "true"
                                                                   : "false",
        stdout);
  fputs(",\"bond_cleared\":", stdout);
  fputs(bond_cleared ? "true" : "false", stdout);
  fputs("}", stdout);
  logger_json_end_success();
}

static void logger_handle_debug_config_clear(logger_service_cli_t *cli,
                                             logger_app_t *app,
                                             const char *args) {
  if (!logger_cli_is_service_mode(app)) {
    logger_json_begin_error(
        "debug config clear", logger_now_utc_or_null(app),
        "not_permitted_in_mode",
        "debug config clear is only allowed in service mode");
    return;
  }
  if (!logger_service_cli_is_unlocked(cli,
                                      to_ms_since_boot(get_absolute_time()))) {
    logger_json_begin_error(
        "debug config clear", logger_now_utc_or_null(app), "service_locked",
        "service unlock is required before debug config clear");
    return;
  }
  if (strcmp(args, "upload") != 0) {
    logger_json_begin_error("debug config clear", logger_now_utc_or_null(app),
                            "invalid_config",
                            "only 'debug config clear upload' is supported");
    return;
  }

  (void)logger_config_clear_upload(&app->persisted);
  (void)logger_system_log_append(
      &app->system_log, logger_now_utc_or_null(app), "config_changed",
      LOGGER_SYSTEM_LOG_SEVERITY_INFO, "{\"field\":\"upload\"}");
  logger_json_begin_success("debug config clear", logger_now_utc_or_null(app));
  fputs("{\"applied\":true,\"field\":\"upload\"}", stdout);
  logger_json_end_success();
}

static void logger_handle_debug_session_start(logger_service_cli_t *cli,
                                              logger_app_t *app,
                                              uint32_t now_ms) {
  if (!logger_cli_is_service_mode(app)) {
    logger_json_begin_error(
        "debug session start", logger_now_utc_or_null(app),
        "not_permitted_in_mode",
        "debug session start is only allowed in service mode");
    return;
  }
  if (!logger_service_cli_is_unlocked(cli, now_ms)) {
    logger_json_begin_error(
        "debug session start", logger_now_utc_or_null(app), "service_locked",
        "service unlock is required before debug session start");
    return;
  }

  const char *error_code = "internal_error";
  const char *error_message = "debug session start failed";
  if (!logger_session_start_debug(
          &app->session, &app->system_log, app->hardware_id, &app->persisted,
          &app->clock, &app->battery, &app->storage,
          app->persisted.current_fault_code, app->persisted.boot_counter,
          now_ms, &error_code, &error_message)) {
    logger_json_begin_error("debug session start", logger_now_utc_or_null(app),
                            error_code, error_message);
    return;
  }
  app->last_session_snapshot_mono_ms = now_ms;

  logger_json_begin_success("debug session start", logger_now_utc_or_null(app));
  fputs("{\"active\":true,\"session_id\":", stdout);
  logger_json_write_string_or_null(app->session.session_id);
  fputs(",\"study_day_local\":", stdout);
  logger_json_write_string_or_null(app->session.study_day_local);
  fputs("}", stdout);
  logger_json_end_success();
}

static void logger_handle_debug_session_snapshot(logger_app_t *app,
                                                 uint32_t now_ms) {
  if (!app->session.active) {
    logger_json_begin_error(
        "debug session snapshot", logger_now_utc_or_null(app),
        "not_permitted_in_mode", "no debug session is active");
    return;
  }
  if (!logger_session_write_status_snapshot(
          &app->session, &app->clock, &app->battery, &app->storage,
          app->persisted.current_fault_code, app->persisted.boot_counter,
          now_ms)) {
    logger_json_begin_error("debug session snapshot",
                            logger_now_utc_or_null(app), "storage_unavailable",
                            "failed to append status snapshot");
    return;
  }
  app->last_session_snapshot_mono_ms = now_ms;

  logger_json_begin_success("debug session snapshot",
                            logger_now_utc_or_null(app));
  fputs("{\"written\":true,\"journal_size_bytes\":", stdout);
  printf("%llu", (unsigned long long)app->session.journal_size_bytes);
  fputs("}", stdout);
  logger_json_end_success();
}

static void logger_handle_debug_session_stop(logger_app_t *app,
                                             uint32_t now_ms) {
  if (!app->session.active) {
    logger_json_begin_success("debug session stop",
                              logger_now_utc_or_null(app));
    fputs("{\"active\":false}", stdout);
    logger_json_end_success();
    return;
  }
  if (!logger_session_stop_debug(
          &app->session, &app->system_log, app->hardware_id, &app->persisted,
          &app->clock, &app->storage, app->persisted.boot_counter, now_ms)) {
    logger_json_begin_error("debug session stop", logger_now_utc_or_null(app),
                            "storage_unavailable",
                            "failed to close debug session");
    return;
  }
  logger_json_begin_success("debug session stop", logger_now_utc_or_null(app));
  fputs("{\"active\":false}", stdout);
  logger_json_end_success();
}

static bool logger_debug_require_service_unlocked(logger_service_cli_t *cli,
                                                  logger_app_t *app,
                                                  const char *command,
                                                  const char *mode_message) {
  if (!logger_cli_is_service_mode(app)) {
    logger_json_begin_error(command, logger_now_utc_or_null(app),
                            "not_permitted_in_mode", mode_message);
    return false;
  }
  if (!logger_service_cli_is_unlocked(cli,
                                      to_ms_since_boot(get_absolute_time()))) {
    logger_json_begin_error(command, logger_now_utc_or_null(app),
                            "service_locked", "service unlock is required");
    return false;
  }
  return true;
}

static void logger_handle_debug_synth_ecg(logger_service_cli_t *cli,
                                          logger_app_t *app, const char *args,
                                          uint32_t now_ms) {
  if (!logger_debug_require_service_unlocked(
          cli, app, "debug synth ecg",
          "debug synth ecg is only allowed in service mode")) {
    return;
  }
  if (!app->session.active) {
    logger_json_begin_error("debug synth ecg", logger_now_utc_or_null(app),
                            "not_permitted_in_mode",
                            "no debug session is active");
    return;
  }

  uint8_t count = 1u;
  if (args != NULL && args[0] != '\0' && !logger_parse_u8(args, &count)) {
    logger_json_begin_error("debug synth ecg", logger_now_utc_or_null(app),
                            "invalid_config",
                            "expected: debug synth ecg [count]");
    return;
  }
  if (count == 0u) {
    count = 1u;
  }

  if (!app->session.span_active) {
    const char *error_code = NULL;
    const char *error_message = NULL;
    if (!logger_session_ensure_active_span(
            &app->session, &app->system_log, app->hardware_id, &app->persisted,
            &app->clock, &app->storage, "reconnect",
            app->persisted.config.bound_h10_address, false, false, false,
            app->persisted.boot_counter, now_ms, &error_code, &error_message)) {
      logger_json_begin_error(
          "debug synth ecg", logger_now_utc_or_null(app),
          error_code != NULL ? error_code : "storage_unavailable",
          error_message != NULL ? error_message : "failed to reopen span");
      return;
    }
  }

  uint8_t packet[12] = {0};
  packet[0] = 0x00u;
  uint32_t appended = 0u;
  for (uint8_t i = 0u; i < count; ++i) {
    packet[1] = i;
    packet[2] = (uint8_t)(i + 1u);
    packet[3] = 0x34u;
    packet[4] = 0x12u;
    packet[5] = 0x78u;
    packet[6] = 0x56u;
    packet[7] = 0xbcu;
    packet[8] = 0x9au;
    packet[9] = 0xf0u;
    packet[10] = 0xdeu;
    packet[11] = 0x01u;
    if (!logger_session_append_ecg_packet(&app->session, &app->clock,
                                          ((uint64_t)now_ms * 1000ull) +
                                              ((uint64_t)i * 1000ull),
                                          packet, sizeof(packet))) {
      logger_json_begin_error("debug synth ecg", logger_now_utc_or_null(app),
                              "storage_unavailable",
                              "failed to append synthetic ECG packet");
      return;
    }
    appended += 1u;
  }

  logger_json_begin_success("debug synth ecg", logger_now_utc_or_null(app));
  fputs("{\"appended_packets\":", stdout);
  printf("%lu", (unsigned long)appended);
  fputs(",\"session_id\":", stdout);
  logger_json_write_string_or_null(app->session.session_id);
  fputs(",\"span_id\":", stdout);
  logger_json_write_string_or_null(
      app->session.span_active ? app->session.current_span_id : NULL);
  fputs(",\"journal_size_bytes\":", stdout);
  printf("%llu", (unsigned long long)app->session.journal_size_bytes);
  fputs("}", stdout);
  logger_json_end_success();
}

static void logger_handle_debug_synth_disconnect(logger_service_cli_t *cli,
                                                 logger_app_t *app,
                                                 uint32_t now_ms) {
  if (!logger_debug_require_service_unlocked(
          cli, app, "debug synth disconnect",
          "debug synth disconnect is only allowed in service mode")) {
    return;
  }
  if (!app->session.active || !app->session.span_active) {
    logger_json_begin_error("debug synth disconnect",
                            logger_now_utc_or_null(app),
                            "not_permitted_in_mode", "no active span is open");
    return;
  }
  if (!logger_session_handle_disconnect(&app->session, &app->clock,
                                        app->persisted.boot_counter, now_ms,
                                        "disconnect")) {
    logger_json_begin_error("debug synth disconnect",
                            logger_now_utc_or_null(app), "storage_unavailable",
                            "failed to append synthetic disconnect gap");
    return;
  }
  logger_json_begin_success("debug synth disconnect",
                            logger_now_utc_or_null(app));
  fputs("{\"span_active\":false,\"session_id\":", stdout);
  logger_json_write_string_or_null(app->session.session_id);
  fputs("}", stdout);
  logger_json_end_success();
}

static void logger_handle_debug_synth_h10_battery(logger_service_cli_t *cli,
                                                  logger_app_t *app,
                                                  const char *args,
                                                  uint32_t now_ms) {
  if (!logger_debug_require_service_unlocked(
          cli, app, "debug synth h10-battery",
          "debug synth h10-battery is only allowed in service mode")) {
    return;
  }
  if (!app->session.active) {
    logger_json_begin_error(
        "debug synth h10-battery", logger_now_utc_or_null(app),
        "not_permitted_in_mode", "no debug session is active");
    return;
  }

  char percent_text[16] = {0};
  char reason[16] = {0};
  if (args == NULL || sscanf(args, "%15s %15s", percent_text, reason) != 2) {
    logger_json_begin_error(
        "debug synth h10-battery", logger_now_utc_or_null(app),
        "invalid_config",
        "expected: debug synth h10-battery <percent> <connect|periodic>");
    return;
  }
  uint8_t percent = 0u;
  if (!logger_parse_u8(percent_text, &percent) || percent > 100u ||
      (strcmp(reason, "connect") != 0 && strcmp(reason, "periodic") != 0)) {
    logger_json_begin_error("debug synth h10-battery",
                            logger_now_utc_or_null(app), "invalid_config",
                            "invalid battery percent or read reason");
    return;
  }

  if (!logger_session_append_h10_battery(&app->session, &app->clock,
                                         app->persisted.boot_counter, now_ms,
                                         percent, reason)) {
    logger_json_begin_error("debug synth h10-battery",
                            logger_now_utc_or_null(app), "storage_unavailable",
                            "failed to append synthetic h10_battery record");
    return;
  }
  app->h10.battery_percent = percent;

  logger_json_begin_success("debug synth h10-battery",
                            logger_now_utc_or_null(app));
  fputs("{\"written\":true,\"battery_percent\":", stdout);
  printf("%u", (unsigned)percent);
  fputs(",\"read_reason\":", stdout);
  logger_json_write_string_or_null(reason);
  fputs("}", stdout);
  logger_json_end_success();
}

static const char *logger_classify_no_session_day_reason(
    bool seen_bound_device, bool ble_connected, bool ecg_start_attempted) {
  if (!seen_bound_device) {
    return "no_h10_seen";
  }
  if (!ble_connected) {
    return "no_h10_connect";
  }
  if (!ecg_start_attempted) {
    return "no_ecg_stream";
  }
  return "no_ecg_stream";
}

static bool logger_no_session_reason_valid(const char *reason) {
  return strcmp(reason, "no_h10_seen") == 0 ||
         strcmp(reason, "no_h10_connect") == 0 ||
         strcmp(reason, "no_ecg_stream") == 0 ||
         strcmp(reason, "stopped_before_first_span") == 0;
}

static void logger_handle_debug_synth_no_session_day(logger_service_cli_t *cli,
                                                     logger_app_t *app,
                                                     const char *args) {
  if (!logger_debug_require_service_unlocked(
          cli, app, "debug synth no-session-day",
          "debug synth no-session-day is only allowed in service mode")) {
    return;
  }
  if (app->session.active) {
    logger_json_begin_error(
        "debug synth no-session-day", logger_now_utc_or_null(app),
        "not_permitted_in_mode",
        "debug synth no-session-day requires no active session");
    return;
  }

  char mode_or_reason[32] = {0};
  char seen_text[8] = {0};
  char connected_text[8] = {0};
  char start_text[8] = {0};
  const int matched = args == NULL
                          ? 0
                          : sscanf(args, "%31s %7s %7s %7s", mode_or_reason,
                                   seen_text, connected_text, start_text);
  if (matched < 1) {
    logger_json_begin_error("debug synth no-session-day",
                            logger_now_utc_or_null(app), "invalid_config",
                            "expected: debug synth no-session-day "
                            "<reason>|auto [seen connected ecg_started]");
    return;
  }

  bool seen_bound_device = app->h10.seen_count > 0u;
  bool ble_connected = app->h10.connect_count > 0u;
  bool ecg_start_attempted = app->h10.ecg_start_attempt_count > 0u;
  const char *reason = mode_or_reason;

  if (strcmp(mode_or_reason, "auto") == 0) {
    if (matched == 4) {
      if (!logger_parse_bool01(seen_text, &seen_bound_device) ||
          !logger_parse_bool01(connected_text, &ble_connected) ||
          !logger_parse_bool01(start_text, &ecg_start_attempted)) {
        logger_json_begin_error("debug synth no-session-day",
                                logger_now_utc_or_null(app), "invalid_config",
                                "auto mode flags must be 0 or 1");
        return;
      }
    }
    reason = logger_classify_no_session_day_reason(
        seen_bound_device, ble_connected, ecg_start_attempted);
  } else if (!logger_no_session_reason_valid(reason)) {
    logger_json_begin_error("debug synth no-session-day",
                            logger_now_utc_or_null(app), "invalid_config",
                            "invalid no-session-day reason");
    return;
  }

  char study_day_local[11] = {0};
  if (!logger_clock_derive_study_day_local_observed(
          &app->clock, app->persisted.config.timezone, study_day_local)) {
    logger_json_begin_error(
        "debug synth no-session-day", logger_now_utc_or_null(app),
        "invalid_config",
        "cannot derive study_day_local from current clock/timezone");
    return;
  }

  char details[LOGGER_SYSTEM_LOG_DETAILS_JSON_MAX + 1];
  logger_json_object_writer_t writer;
  logger_json_object_writer_init(&writer, details, sizeof(details));
  if (!logger_json_object_writer_string_field(&writer, "study_day_local",
                                              study_day_local) ||
      !logger_json_object_writer_string_field(&writer, "reason", reason) ||
      !logger_json_object_writer_bool_field(&writer, "seen_bound_device",
                                            seen_bound_device) ||
      !logger_json_object_writer_bool_field(&writer, "ble_connected",
                                            ble_connected) ||
      !logger_json_object_writer_bool_field(&writer, "ecg_start_attempted",
                                            ecg_start_attempted) ||
      !logger_json_object_writer_finish(&writer)) {
    logger_json_begin_error(
        "debug synth no-session-day", logger_now_utc_or_null(app),
        "storage_unavailable",
        "failed to build synthetic no_session_day_summary details");
    return;
  }
  if (!logger_system_log_append(&app->system_log, logger_now_utc_or_null(app),
                                "no_session_day_summary",
                                LOGGER_SYSTEM_LOG_SEVERITY_INFO,
                                logger_json_object_writer_data(&writer))) {
    logger_json_begin_error(
        "debug synth no-session-day", logger_now_utc_or_null(app),
        "storage_unavailable",
        "failed to append synthetic no_session_day_summary");
    return;
  }
  logger_set_last_day_outcome(app, study_day_local, "no_session", reason);

  logger_json_begin_success("debug synth no-session-day",
                            logger_now_utc_or_null(app));
  fputs("{\"study_day_local\":", stdout);
  logger_json_write_string_or_null(study_day_local);
  fputs(",\"reason\":", stdout);
  logger_json_write_string_or_null(reason);
  fputs(",\"seen_bound_device\":", stdout);
  fputs(seen_bound_device ? "true" : "false", stdout);
  fputs(",\"ble_connected\":", stdout);
  fputs(ble_connected ? "true" : "false", stdout);
  fputs(",\"ecg_start_attempted\":", stdout);
  fputs(ecg_start_attempted ? "true" : "false", stdout);
  fputs("}", stdout);
  logger_json_end_success();
}

static bool logger_update_clock_from_rfc3339(logger_app_t *app,
                                             const char *rfc3339,
                                             uint32_t now_ms,
                                             int64_t *old_utc_ns_out,
                                             int64_t *new_utc_ns_out) {
  int64_t old_utc_ns = 0ll;
  (void)logger_clock_observed_utc_ns(&app->clock, &old_utc_ns);
  if (!logger_clock_set_utc(rfc3339, &app->clock)) {
    return false;
  }
  int64_t new_utc_ns = 0ll;
  (void)logger_clock_observed_utc_ns(&app->clock, &new_utc_ns);
  app->last_clock_observation_available = true;
  app->last_clock_observation_valid = app->clock.valid;
  app->last_clock_observation_mono_ms = now_ms;
  app->last_clock_observation_utc_ns = new_utc_ns;
  if (old_utc_ns_out != NULL) {
    *old_utc_ns_out = old_utc_ns;
  }
  if (new_utc_ns_out != NULL) {
    *new_utc_ns_out = new_utc_ns;
  }
  return true;
}

static void logger_handle_debug_synth_rollover(logger_service_cli_t *cli,
                                               logger_app_t *app,
                                               const char *rfc3339,
                                               uint32_t now_ms) {
  if (!logger_debug_require_service_unlocked(
          cli, app, "debug synth rollover",
          "debug synth rollover is only allowed in service mode")) {
    return;
  }
  if (!app->session.active) {
    logger_json_begin_error("debug synth rollover", logger_now_utc_or_null(app),
                            "not_permitted_in_mode",
                            "no debug session is active");
    return;
  }

  char old_session_id[LOGGER_SESSION_ID_HEX_LEN + 1];
  char old_study_day_local[11];
  logger_copy_string(old_session_id, sizeof(old_session_id),
                     app->session.session_id);
  logger_copy_string(old_study_day_local, sizeof(old_study_day_local),
                     app->session.study_day_local);

  if (!logger_update_clock_from_rfc3339(app, rfc3339, now_ms, NULL, NULL)) {
    logger_json_begin_error("debug synth rollover", logger_now_utc_or_null(app),
                            "invalid_config", "invalid RFC3339 UTC timestamp");
    return;
  }

  char new_study_day_local[11] = {0};
  if (!logger_clock_derive_study_day_local_observed(
          &app->clock, app->persisted.config.timezone, new_study_day_local)) {
    logger_json_begin_error(
        "debug synth rollover", logger_now_utc_or_null(app), "invalid_config",
        "cannot derive study_day_local from current clock/timezone");
    return;
  }
  if (strcmp(old_study_day_local, new_study_day_local) == 0) {
    logger_json_begin_error(
        "debug synth rollover", logger_now_utc_or_null(app), "invalid_config",
        "new timestamp does not cross the study-day boundary");
    return;
  }

  if (!logger_session_finalize(&app->session, &app->system_log,
                               app->hardware_id, &app->persisted, &app->clock,
                               &app->storage, "rollover",
                               app->persisted.boot_counter, now_ms)) {
    logger_json_begin_error("debug synth rollover", logger_now_utc_or_null(app),
                            "storage_unavailable",
                            "failed to finalize pre-rollover session");
    return;
  }
  const char *error_code = NULL;
  const char *error_message = NULL;
  if (!logger_session_ensure_active_span(
          &app->session, &app->system_log, app->hardware_id, &app->persisted,
          &app->clock, &app->storage, "rollover_continue",
          app->persisted.config.bound_h10_address, false, false, false,
          app->persisted.boot_counter, now_ms, &error_code, &error_message)) {
    logger_json_begin_error(
        "debug synth rollover", logger_now_utc_or_null(app),
        error_code != NULL ? error_code : "storage_unavailable",
        error_message != NULL ? error_message
                              : "failed to open post-rollover session");
    return;
  }
  logger_set_last_day_outcome(app, old_study_day_local, "session",
                              "session_closed");
  app->last_session_live_flush_mono_ms = now_ms;
  app->last_session_snapshot_mono_ms = now_ms;

  logger_json_begin_success("debug synth rollover",
                            logger_now_utc_or_null(app));
  fputs("{\"old_study_day_local\":", stdout);
  logger_json_write_string_or_null(old_study_day_local);
  fputs(",\"new_study_day_local\":", stdout);
  logger_json_write_string_or_null(new_study_day_local);
  fputs(",\"old_session_id\":", stdout);
  logger_json_write_string_or_null(old_session_id);
  fputs(",\"new_session_id\":", stdout);
  logger_json_write_string_or_null(app->session.session_id);
  fputs("}", stdout);
  logger_json_end_success();
}

static void logger_handle_debug_synth_clock_boundary(
    logger_service_cli_t *cli, logger_app_t *app, const char *command,
    const char *event_kind, const char *span_end_reason,
    const char *continue_reason, bool jump_at_session_start,
    const char *rfc3339, uint32_t now_ms) {
  if (!logger_debug_require_service_unlocked(
          cli, app, command,
          "synthetic clock commands are only allowed in service mode")) {
    return;
  }
  if (!app->session.active) {
    logger_json_begin_error(command, logger_now_utc_or_null(app),
                            "not_permitted_in_mode",
                            "no debug session is active");
    return;
  }

  char old_session_id[LOGGER_SESSION_ID_HEX_LEN + 1];
  char old_study_day_local[11];
  logger_copy_string(old_session_id, sizeof(old_session_id),
                     app->session.session_id);
  logger_copy_string(old_study_day_local, sizeof(old_study_day_local),
                     app->session.study_day_local);

  int64_t old_utc_ns = 0ll;
  int64_t new_utc_ns = 0ll;
  if (!logger_update_clock_from_rfc3339(app, rfc3339, now_ms, &old_utc_ns,
                                        &new_utc_ns)) {
    logger_json_begin_error(command, logger_now_utc_or_null(app),
                            "invalid_config", "invalid RFC3339 UTC timestamp");
    return;
  }

  char new_study_day_local[11] = {0};
  if (!logger_clock_derive_study_day_local_observed(
          &app->clock, app->persisted.config.timezone, new_study_day_local)) {
    logger_json_begin_error(
        command, logger_now_utc_or_null(app), "invalid_config",
        "cannot derive study_day_local from current clock/timezone");
    return;
  }
  const bool crossed_study_day =
      strcmp(old_study_day_local, new_study_day_local) != 0;

  if (!logger_session_handle_clock_event(
          &app->session, &app->clock, app->persisted.boot_counter, now_ms,
          event_kind, span_end_reason, new_utc_ns - old_utc_ns, old_utc_ns,
          new_utc_ns, true)) {
    logger_json_begin_error(
        command, logger_now_utc_or_null(app), "storage_unavailable",
        "failed to append synthetic clock boundary records");
    return;
  }

  if (crossed_study_day) {
    if (!logger_session_finalize(&app->session, &app->system_log,
                                 app->hardware_id, &app->persisted, &app->clock,
                                 &app->storage, span_end_reason,
                                 app->persisted.boot_counter, now_ms)) {
      logger_json_begin_error(command, logger_now_utc_or_null(app),
                              "storage_unavailable",
                              "failed to finalize pre-boundary session");
      return;
    }
    logger_set_last_day_outcome(app, old_study_day_local, "session",
                                "session_closed");
  }

  const char *error_code = NULL;
  const char *error_message = NULL;
  if (!logger_session_ensure_active_span(
          &app->session, &app->system_log, app->hardware_id, &app->persisted,
          &app->clock, &app->storage, continue_reason,
          app->persisted.config.bound_h10_address, false, false,
          jump_at_session_start && crossed_study_day,
          app->persisted.boot_counter, now_ms, &error_code, &error_message)) {
    logger_json_begin_error(
        command, logger_now_utc_or_null(app),
        error_code != NULL ? error_code : "storage_unavailable",
        error_message != NULL ? error_message
                              : "failed to open post-boundary span/session");
    return;
  }

  app->last_session_live_flush_mono_ms = now_ms;
  app->last_session_snapshot_mono_ms = now_ms;

  logger_json_begin_success(command, logger_now_utc_or_null(app));
  fputs("{\"crossed_study_day\":", stdout);
  fputs(crossed_study_day ? "true" : "false", stdout);
  fputs(",\"old_study_day_local\":", stdout);
  logger_json_write_string_or_null(old_study_day_local);
  fputs(",\"new_study_day_local\":", stdout);
  logger_json_write_string_or_null(new_study_day_local);
  fputs(",\"old_session_id\":", stdout);
  logger_json_write_string_or_null(old_session_id);
  fputs(",\"new_session_id\":", stdout);
  logger_json_write_string_or_null(app->session.session_id);
  fputs(",\"current_span_id\":", stdout);
  logger_json_write_string_or_null(
      app->session.span_active ? app->session.current_span_id : NULL);
  fputs("}", stdout);
  logger_json_end_success();
}

static void logger_handle_debug_queue_rebuild(logger_service_cli_t *cli,
                                              logger_app_t *app) {
  const bool allowed_in_wait_h10 =
      app->runtime.current_state == LOGGER_RUNTIME_LOG_WAIT_H10;
  if (!logger_cli_is_service_mode(app) && !allowed_in_wait_h10) {
    logger_json_begin_error(
        "debug queue rebuild", logger_now_utc_or_null(app),
        "not_permitted_in_mode",
        "debug queue rebuild is only allowed in service mode or log_wait_h10");
    return;
  }
  if (app->session.active) {
    logger_json_begin_error(
        "debug queue rebuild", logger_now_utc_or_null(app),
        "not_permitted_in_mode",
        "debug queue rebuild is not permitted while a session is active");
    return;
  }
  if (logger_cli_is_service_mode(app) &&
      !logger_service_cli_is_unlocked(cli,
                                      to_ms_since_boot(get_absolute_time()))) {
    logger_json_begin_error(
        "debug queue rebuild", logger_now_utc_or_null(app), "service_locked",
        "service unlock is required before debug queue rebuild");
    return;
  }

  logger_upload_queue_summary_t summary;
  if (!logger_upload_queue_rebuild_file(
          &app->system_log, logger_now_utc_or_null(app), &summary)) {
    logger_json_begin_error(
        "debug queue rebuild", logger_now_utc_or_null(app),
        "storage_unavailable",
        "failed to rebuild upload_queue.json from local sessions");
    return;
  }

  logger_json_begin_success("debug queue rebuild", logger_now_utc_or_null(app));
  fputs("{\"rebuilt\":true,\"updated_at_utc\":", stdout);
  logger_json_write_string_or_null(
      summary.updated_at_utc[0] != '\0' ? summary.updated_at_utc : NULL);
  fputs(",\"session_count\":", stdout);
  printf("%lu", (unsigned long)summary.session_count);
  fputs(",\"pending_count\":", stdout);
  printf("%lu", (unsigned long)summary.pending_count);
  fputs(",\"blocked_count\":", stdout);
  printf("%lu", (unsigned long)summary.blocked_count);
  fputs("}", stdout);
  logger_json_end_success();
}

static void logger_handle_debug_queue_requeue_blocked(logger_service_cli_t *cli,
                                                      logger_app_t *app) {
  const bool allowed_in_wait_h10 =
      app->runtime.current_state == LOGGER_RUNTIME_LOG_WAIT_H10;
  if (!logger_cli_is_service_mode(app) && !allowed_in_wait_h10) {
    logger_json_begin_error("debug queue requeue-blocked",
                            logger_now_utc_or_null(app),
                            "not_permitted_in_mode",
                            "debug queue requeue-blocked is only allowed in "
                            "service mode or log_wait_h10");
    return;
  }
  if (app->session.active) {
    logger_json_begin_error("debug queue requeue-blocked",
                            logger_now_utc_or_null(app),
                            "not_permitted_in_mode",
                            "debug queue requeue-blocked is not permitted "
                            "while a session is active");
    return;
  }
  if (logger_cli_is_service_mode(app) &&
      !logger_service_cli_is_unlocked(cli,
                                      to_ms_since_boot(get_absolute_time()))) {
    logger_json_begin_error(
        "debug queue requeue-blocked", logger_now_utc_or_null(app),
        "service_locked",
        "service unlock is required before debug queue requeue-blocked");
    return;
  }

  size_t requeued_count = 0u;
  logger_upload_queue_summary_t summary;
  if (!logger_upload_queue_requeue_blocked_file(
          &app->system_log, logger_now_utc_or_null(app),
          "manual_requeue_blocked", &requeued_count, &summary)) {
    logger_json_begin_error(
        "debug queue requeue-blocked", logger_now_utc_or_null(app),
        "storage_unavailable",
        "failed to rewrite blocked upload_queue.json entries as pending");
    return;
  }

  logger_json_begin_success("debug queue requeue-blocked",
                            logger_now_utc_or_null(app));
  fputs("{\"requeued_count\":", stdout);
  printf("%lu", (unsigned long)requeued_count);
  fputs(",\"updated_at_utc\":", stdout);
  logger_json_write_string_or_null(
      summary.updated_at_utc[0] != '\0' ? summary.updated_at_utc : NULL);
  fputs(",\"session_count\":", stdout);
  printf("%lu", (unsigned long)summary.session_count);
  fputs(",\"pending_count\":", stdout);
  printf("%lu", (unsigned long)summary.pending_count);
  fputs(",\"blocked_count\":", stdout);
  printf("%lu", (unsigned long)summary.blocked_count);
  fputs("}", stdout);
  logger_json_end_success();
}

static void logger_handle_debug_prune_once(logger_service_cli_t *cli,
                                           logger_app_t *app) {
  const bool allowed_in_wait_h10 =
      app->runtime.current_state == LOGGER_RUNTIME_LOG_WAIT_H10;
  if (!logger_cli_is_service_mode(app) && !allowed_in_wait_h10) {
    logger_json_begin_error(
        "debug prune once", logger_now_utc_or_null(app),
        "not_permitted_in_mode",
        "debug prune once is only allowed in service mode or log_wait_h10");
    return;
  }
  if (app->session.active) {
    logger_json_begin_error(
        "debug prune once", logger_now_utc_or_null(app),
        "not_permitted_in_mode",
        "debug prune once is not permitted while a session is active");
    return;
  }
  if (logger_cli_is_service_mode(app) &&
      !logger_service_cli_is_unlocked(cli,
                                      to_ms_since_boot(get_absolute_time()))) {
    logger_json_begin_error(
        "debug prune once", logger_now_utc_or_null(app), "service_locked",
        "service unlock is required before debug prune once");
    return;
  }

  size_t retention_pruned_count = 0u;
  size_t reserve_pruned_count = 0u;
  bool reserve_met = false;
  logger_upload_queue_summary_t summary;
  if (!logger_upload_queue_prune_file(
          &app->system_log, logger_now_utc_or_null(app),
          LOGGER_SD_MIN_FREE_RESERVE_BYTES, &retention_pruned_count,
          &reserve_pruned_count, &reserve_met, &summary)) {
    logger_json_begin_error("debug prune once", logger_now_utc_or_null(app),
                            "storage_unavailable",
                            "failed to apply upload retention/prune policy");
    return;
  }

  logger_json_begin_success("debug prune once", logger_now_utc_or_null(app));
  fputs("{\"retention_pruned_count\":", stdout);
  printf("%lu", (unsigned long)retention_pruned_count);
  fputs(",\"reserve_pruned_count\":", stdout);
  printf("%lu", (unsigned long)reserve_pruned_count);
  fputs(",\"reserve_met\":", stdout);
  fputs(reserve_met ? "true" : "false", stdout);
  fputs(",\"updated_at_utc\":", stdout);
  logger_json_write_string_or_null(
      summary.updated_at_utc[0] != '\0' ? summary.updated_at_utc : NULL);
  fputs(",\"session_count\":", stdout);
  printf("%lu", (unsigned long)summary.session_count);
  fputs(",\"pending_count\":", stdout);
  printf("%lu", (unsigned long)summary.pending_count);
  fputs(",\"blocked_count\":", stdout);
  printf("%lu", (unsigned long)summary.blocked_count);
  fputs("}", stdout);
  logger_json_end_success();
}

static void logger_handle_debug_upload_once(logger_service_cli_t *cli,
                                            logger_app_t *app) {
  const bool allowed_in_wait_h10 =
      app->runtime.current_state == LOGGER_RUNTIME_LOG_WAIT_H10;
  if (!logger_cli_is_service_mode(app) && !allowed_in_wait_h10) {
    logger_json_begin_error(
        "debug upload once", logger_now_utc_or_null(app),
        "not_permitted_in_mode",
        "debug upload once is only allowed in service mode or log_wait_h10");
    return;
  }
  if (logger_cli_is_service_mode(app) &&
      !logger_service_cli_is_unlocked(cli,
                                      to_ms_since_boot(get_absolute_time()))) {
    logger_json_begin_error(
        "debug upload once", logger_now_utc_or_null(app), "service_locked",
        "service unlock is required before debug upload once");
    return;
  }

  logger_upload_process_result_t result;
  const bool ok = logger_upload_process_one(
      &app->system_log, &app->persisted.config, app->hardware_id,
      logger_now_utc_or_null(app), &result);

  if (result.code == LOGGER_UPLOAD_PROCESS_RESULT_NOT_ATTEMPTED) {
    logger_json_begin_error("debug upload once", logger_now_utc_or_null(app),
                            "invalid_config", result.message);
    return;
  }
  if (result.code == LOGGER_UPLOAD_PROCESS_RESULT_FAILED) {
    logger_json_begin_error("debug upload once", logger_now_utc_or_null(app),
                            logger_string_present(result.failure_class)
                                ? result.failure_class
                                : "upload_failed",
                            result.message);
    return;
  }
  if (result.code == LOGGER_UPLOAD_PROCESS_RESULT_BLOCKED_MIN_FIRMWARE) {
    logger_json_begin_error("debug upload once", logger_now_utc_or_null(app),
                            "min_firmware_rejected", result.message);
    return;
  }

  logger_json_begin_success("debug upload once", logger_now_utc_or_null(app));
  fputs("{\"attempted\":", stdout);
  fputs(result.attempted ? "true" : "false", stdout);
  fputs(",\"result\":", stdout);
  logger_json_write_string_or_null(
      result.code == LOGGER_UPLOAD_PROCESS_RESULT_VERIFIED ? "verified"
      : result.code == LOGGER_UPLOAD_PROCESS_RESULT_NO_WORK
          ? "no_work"
          : (ok ? "ok" : "unknown"));
  fputs(",\"session_id\":", stdout);
  logger_json_write_string_or_null(
      logger_string_present(result.session_id) ? result.session_id : NULL);
  fputs(",\"final_status\":", stdout);
  logger_json_write_string_or_null(
      logger_string_present(result.final_status) ? result.final_status : NULL);
  fputs(",\"receipt_id\":", stdout);
  logger_json_write_string_or_null(
      logger_string_present(result.receipt_id) ? result.receipt_id : NULL);
  fputs(",\"verified_upload_utc\":", stdout);
  logger_json_write_string_or_null(
      logger_string_present(result.verified_upload_utc)
          ? result.verified_upload_utc
          : NULL);
  fputs(",\"http_status\":", stdout);
  if (result.http_status >= 0) {
    printf("%d", result.http_status);
  } else {
    fputs("null", stdout);
  }
  fputs(",\"message\":", stdout);
  logger_json_write_string_or_null(
      logger_string_present(result.message) ? result.message : NULL);
  fputs("}", stdout);
  logger_json_end_success();
}

void logger_service_cli_init(logger_service_cli_t *cli) {
  memset(cli, 0, sizeof(*cli));
}

bool logger_service_cli_is_unlocked(const logger_service_cli_t *cli,
                                    uint32_t now_ms) {
  return cli->unlocked && now_ms < cli->unlock_deadline_mono_ms;
}

static void logger_service_cli_execute(logger_service_cli_t *cli,
                                       logger_app_t *app, const char *line,
                                       uint32_t now_ms) {
  if (cli->unlocked && now_ms >= cli->unlock_deadline_mono_ms) {
    cli->unlocked = false;
  }

  if (strcmp(line, "status --json") == 0) {
    logger_handle_status_json(app);
    return;
  }
  if (strcmp(line, "provisioning-status --json") == 0) {
    logger_handle_provisioning_status_json(app);
    return;
  }
  if (strcmp(line, "queue --json") == 0) {
    logger_handle_queue_json(app);
    return;
  }
  if (strcmp(line, "preflight --json") == 0) {
    logger_handle_preflight_json(app);
    return;
  }
  if (strcmp(line, "net-test --json") == 0) {
    logger_handle_net_test_json(app);
    return;
  }
  if (strcmp(line, "config export --json") == 0) {
    logger_handle_config_export_json(app);
    return;
  }
  if (strcmp(line, "system-log export --json") == 0) {
    logger_handle_system_log_export_json(app);
    return;
  }
  if (strcmp(line, "clock status --json") == 0) {
    logger_handle_clock_status_json(app);
    return;
  }
  if (strcmp(line, "clock sync --json") == 0) {
    logger_handle_clock_sync(cli, app);
    return;
  }
  if (strcmp(line, "service enter") == 0) {
    logger_handle_service_enter(app, now_ms);
    return;
  }
  if (strcmp(line, "service unlock") == 0) {
    logger_handle_service_unlock(cli, app, now_ms);
    return;
  }
  if (strcmp(line, "fault clear") == 0) {
    logger_handle_fault_clear(app);
    return;
  }
  if (strcmp(line, "factory-reset") == 0) {
    logger_handle_factory_reset(cli, app);
    return;
  }
  if (strcmp(line, "upload tls clear-provisioned-anchor") == 0) {
    logger_handle_upload_tls_clear_provisioned_anchor(cli, app);
    return;
  }
  if (strcmp(line, "config import status") == 0) {
    logger_handle_config_import_status(cli, app);
    return;
  }
  if (strcmp(line, "config import cancel") == 0) {
    logger_handle_config_import_cancel(cli, app);
    return;
  }
  if (strcmp(line, "config import commit") == 0) {
    logger_handle_config_import_commit(cli, app);
    return;
  }
  if (strcmp(line, "config import begin") == 0) {
    logger_handle_config_import_begin(cli, app, "");
    return;
  }
  if (strncmp(line, "config import begin ", 20) == 0) {
    logger_handle_config_import_begin(cli, app, line + 20);
    return;
  }
  if (strcmp(line, "config import chunk") == 0) {
    logger_handle_config_import_chunk(cli, app, "");
    return;
  }
  if (strncmp(line, "config import chunk ", 20) == 0) {
    logger_handle_config_import_chunk(cli, app, line + 20);
    return;
  }
  if (strcmp(line, "config import") == 0) {
    logger_handle_config_import(cli, app, "");
    return;
  }
  if (strncmp(line, "config import ", 14) == 0) {
    logger_handle_config_import(cli, app, line + 14);
    return;
  }
  if (strncmp(line, "clock set ", 10) == 0) {
    logger_handle_clock_set(cli, app, line + 10);
    return;
  }
  if (strncmp(line, "debug config set ", 17) == 0) {
    logger_handle_debug_config_set(cli, app, line + 17);
    return;
  }
  if (strncmp(line, "debug config clear ", 19) == 0) {
    logger_handle_debug_config_clear(cli, app, line + 19);
    return;
  }
  if (strcmp(line, "debug session start") == 0) {
    logger_handle_debug_session_start(cli, app, now_ms);
    return;
  }
  if (strcmp(line, "debug session snapshot") == 0) {
    logger_handle_debug_session_snapshot(app, now_ms);
    return;
  }
  if (strcmp(line, "debug session stop") == 0) {
    logger_handle_debug_session_stop(app, now_ms);
    return;
  }
  if (strcmp(line, "debug queue rebuild") == 0) {
    logger_handle_debug_queue_rebuild(cli, app);
    return;
  }
  if (strcmp(line, "debug queue requeue-blocked") == 0) {
    logger_handle_debug_queue_requeue_blocked(cli, app);
    return;
  }
  if (strcmp(line, "debug prune once") == 0) {
    logger_handle_debug_prune_once(cli, app);
    return;
  }
  if (strcmp(line, "debug upload once") == 0) {
    logger_handle_debug_upload_once(cli, app);
    return;
  }
  if (strcmp(line, "debug synth ecg") == 0) {
    logger_handle_debug_synth_ecg(cli, app, "", now_ms);
    return;
  }
  if (strncmp(line, "debug synth ecg ", 16) == 0) {
    logger_handle_debug_synth_ecg(cli, app, line + 16, now_ms);
    return;
  }
  if (strcmp(line, "debug synth disconnect") == 0) {
    logger_handle_debug_synth_disconnect(cli, app, now_ms);
    return;
  }
  if (strncmp(line, "debug synth h10-battery ", 24) == 0) {
    logger_handle_debug_synth_h10_battery(cli, app, line + 24, now_ms);
    return;
  }
  if (strncmp(line, "debug synth no-session-day ", 27) == 0) {
    logger_handle_debug_synth_no_session_day(cli, app, line + 27);
    return;
  }
  if (strncmp(line, "debug synth rollover ", 21) == 0) {
    logger_handle_debug_synth_rollover(cli, app, line + 21, now_ms);
    return;
  }
  if (strncmp(line, "debug synth clock-fix ", 22) == 0) {
    logger_handle_debug_synth_clock_boundary(
        cli, app, "debug synth clock-fix", "clock_fixed", "clock_fix",
        "clock_fix_continue", false, line + 22, now_ms);
    return;
  }
  if (strncmp(line, "debug synth clock-jump ", 23) == 0) {
    logger_handle_debug_synth_clock_boundary(
        cli, app, "debug synth clock-jump", "clock_jump", "clock_jump",
        "clock_jump_continue", true, line + 23, now_ms);
    return;
  }
  if (strcmp(line, "debug reboot") == 0) {
    app->reboot_pending = true;
    logger_json_begin_success("debug reboot", logger_now_utc_or_null(app));
    fputs("{\"will_reboot\":true}", stdout);
    logger_json_end_success();
    return;
  }
  if (strcmp(line, "sd format") == 0) {
    logger_handle_sd_format(cli, app);
    return;
  }

  logger_json_begin_error(line, logger_now_utc_or_null(app), "internal_error",
                          "unknown command");
}

void logger_service_cli_poll(logger_service_cli_t *cli, logger_app_t *app,
                             uint32_t now_ms) {
  for (;;) {
    int ch = getchar_timeout_us(0);
    if (ch == PICO_ERROR_TIMEOUT) {
      break;
    }
    if (ch == '\r') {
      continue;
    }
    if (ch == '\n') {
      cli->line_buf[cli->line_len] = '\0';
      if (cli->line_len > 0u) {
        logger_service_cli_execute(cli, app, cli->line_buf, now_ms);
      }
      cli->line_len = 0u;
      continue;
    }
    if (cli->line_len + 1u >= sizeof(cli->line_buf)) {
      cli->line_len = 0u;
      logger_json_begin_error("input", logger_now_utc_or_null(app),
                              "internal_error", "input line too long");
      break;
    }
    cli->line_buf[cli->line_len++] = (char)ch;
  }
}
