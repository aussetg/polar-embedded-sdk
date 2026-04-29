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
#include "logger/capture_stats.h"
#include "logger/config_validate.h"
#include "logger/faults.h"
#include "logger/json.h"
#include "logger/json_stream_writer.h"
#include "logger/json_writer.h"
#include "logger/queue.h"
#include "logger/sha256.h"
#include "logger/storage_service.h"
#include "logger/upload.h"
#include "logger/util.h"
#include "logger/version.h"

static bool logger_service_cli_is_unlocked(const logger_service_cli_t *cli,
                                           uint32_t now_ms);

static logger_persisted_state_t g_service_cli_imported_state;
static uint8_t
    g_service_cli_anchor_der_buf[LOGGER_CONFIG_UPLOAD_TLS_ANCHOR_DER_MAX];
static char g_service_cli_anchor_der_base64_buf
    [LOGGER_CONFIG_UPLOAD_TLS_ANCHOR_BASE64_MAX + 1u];
static char g_service_cli_details_json[LOGGER_SYSTEM_LOG_DETAILS_JSON_MAX + 1u];
static logger_upload_net_test_result_t g_service_cli_net_test_result;
static logger_upload_process_result_t g_service_cli_upload_process_result;
static logger_system_log_event_t g_service_cli_system_log_event;
static char g_service_cli_mark_verified_manifest_path[LOGGER_STORAGE_PATH_MAX];
static char g_service_cli_mark_verified_journal_path[LOGGER_STORAGE_PATH_MAX];

#define LOGGER_SERVICE_BUNDLE_EXPORT_CHUNK_BYTES STORAGE_SVC_BUNDLE_READ_MAX
#define LOGGER_SERVICE_CLI_BUSY_POLL_CHAR_BUDGET 96u
#define LOGGER_SERVICE_CLI_BUSY_POLL_LINE_BUDGET 1u

typedef struct {
  bool open;
  char session_id[LOGGER_SESSION_ID_HEX_LEN + 1];
  char dir_name[64];
  char manifest_path[LOGGER_STORAGE_PATH_MAX];
  char journal_path[LOGGER_STORAGE_PATH_MAX];
  uint64_t bundle_size_bytes;
  uint64_t offset;
  uint8_t chunk[LOGGER_SERVICE_BUNDLE_EXPORT_CHUNK_BYTES];
  char
      chunk_base64[((LOGGER_SERVICE_BUNDLE_EXPORT_CHUNK_BYTES + 2u) / 3u) * 4u +
                   1u];
} logger_service_bundle_export_t;

static logger_service_bundle_export_t g_service_bundle_export;

typedef struct {
  char imported_bound_address[LOGGER_CONFIG_BOUND_H10_ADDR_MAX];
  char normalized_bound_address[LOGGER_CONFIG_BOUND_H10_ADDR_MAX];
  char rollover_local[16];
  char upload_start_local[16];
  char upload_end_local[16];
  char allowed_ssid[LOGGER_CONFIG_WIFI_SSID_MAX];
  char network_ssid[LOGGER_CONFIG_WIFI_SSID_MAX];
  char network_psk[LOGGER_CONFIG_WIFI_PSK_MAX];
  char upload_url[LOGGER_CONFIG_UPLOAD_URL_MAX];
  char auth_type[24];
  char tls_mode[32];
  char root_profile[64];
  char auth_api_key[LOGGER_CONFIG_UPLOAD_API_KEY_MAX];
  char auth_token[LOGGER_CONFIG_UPLOAD_TOKEN_MAX];
  char anchor_format[32];
  char anchor_der_base64[LOGGER_CONFIG_UPLOAD_TLS_ANCHOR_BASE64_MAX + 1u];
  char anchor_subject[LOGGER_CONFIG_UPLOAD_TLS_ANCHOR_SUBJECT_MAX];
  char anchor_sha256_hex[LOGGER_CONFIG_UPLOAD_TLS_ANCHOR_SHA256_HEX_LEN + 1u];
  char anchor_expected_sha256[LOGGER_CONFIG_UPLOAD_TLS_ANCHOR_SHA256_HEX_LEN +
                              1u];
  char anchor_expected_subject[LOGGER_CONFIG_UPLOAD_TLS_ANCHOR_SUBJECT_MAX];
} logger_service_cli_config_import_workspace_t;

static logger_service_cli_config_import_workspace_t
    g_service_cli_config_import_workspace;

static const char *logger_upload_blocked_reason_hint(void) {
  return "blocked by closed-session manifest firmware_version, not current "
         "running firmware";
}

static const char *logger_upload_blocked_retry_hint(void) {
  return "reflash alone will not unblock old sessions; use debug queue "
         "requeue-blocked only after server-side minimum changes";
}

static const char *logger_upload_nonretryable_retry_hint(void) {
  return "automatic retry is disabled for this session; inspect diagnostics "
         "and use debug queue requeue-nonretryable only after repair";
}

static const char *logger_upload_queue_summary_blocked_reason_hint(
    const logger_upload_queue_summary_t *summary) {
  if (summary == NULL || summary->blocked_count == 0u) {
    return NULL;
  }
  return logger_upload_blocked_reason_hint();
}

static const char *logger_upload_queue_summary_blocked_retry_hint(
    const logger_upload_queue_summary_t *summary) {
  if (summary == NULL || summary->blocked_count == 0u) {
    return NULL;
  }
  return logger_upload_blocked_retry_hint();
}

static const char *logger_upload_queue_entry_status_detail(
    const logger_upload_queue_entry_t *entry) {
  if (entry == NULL) {
    return NULL;
  }
  if (logger_string_present(entry->last_server_error_message)) {
    return entry->last_server_error_message;
  }
  if (logger_string_present(entry->last_response_excerpt)) {
    return entry->last_response_excerpt;
  }
  if (strcmp(entry->status, "blocked_min_firmware") == 0) {
    return logger_upload_blocked_reason_hint();
  }
  if (strcmp(entry->status, "nonretryable") == 0) {
    return "hard upload rejection or local artifact problem";
  }
  return NULL;
}

static const char *
logger_upload_queue_entry_retry_hint(const logger_upload_queue_entry_t *entry) {
  if (entry == NULL) {
    return NULL;
  }
  if (strcmp(entry->status, "blocked_min_firmware") == 0) {
    return logger_upload_blocked_retry_hint();
  }
  if (strcmp(entry->status, "nonretryable") == 0) {
    return logger_upload_nonretryable_retry_hint();
  }
  return NULL;
}

static bool logger_cli_reason_token_valid(const char *reason) {
  if (!logger_string_present(reason) ||
      strlen(reason) > LOGGER_UPLOAD_QUEUE_RESPONSE_EXCERPT_MAX) {
    return false;
  }
  for (size_t i = 0u; reason[i] != '\0'; ++i) {
    const char ch = reason[i];
    if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_')) {
      return false;
    }
  }
  return true;
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
  logger_service_cli_config_import_workspace_t *const workspace =
      &g_service_cli_config_import_workspace;

  if (anchor_tok == NULL || anchor_tok->type != JSMN_OBJECT) {
    *error_message_out = "config import upload.tls.anchor is invalid";
    return false;
  }

  workspace->anchor_format[0] = '\0';
  if (!logger_json_object_copy_string(doc, anchor_tok, "format",
                                      workspace->anchor_format,
                                      sizeof(workspace->anchor_format))) {
    *error_message_out = "config import upload.tls.anchor.format is invalid";
    return false;
  }
  if (strcmp(workspace->anchor_format,
             LOGGER_UPLOAD_TLS_ANCHOR_FORMAT_X509_DER_BASE64) != 0) {
    *error_message_out =
        "config import upload.tls.anchor.format is unsupported";
    return false;
  }

  workspace->anchor_der_base64[0] = '\0';
  if (!logger_json_object_copy_string(doc, anchor_tok, "der_base64",
                                      workspace->anchor_der_base64,
                                      sizeof(workspace->anchor_der_base64))) {
    *error_message_out =
        "config import upload.tls.anchor.der_base64 is invalid";
    return false;
  }

  size_t der_len = 0u;
  int rc = mbedtls_base64_decode(
      NULL, 0u, &der_len, (const unsigned char *)workspace->anchor_der_base64,
      strlen(workspace->anchor_der_base64));
  if (!(rc == 0 || rc == MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL) ||
      der_len == 0u || der_len > LOGGER_CONFIG_UPLOAD_TLS_ANCHOR_DER_MAX) {
    *error_message_out = "config import upload.tls.anchor.der_base64 does not "
                         "decode to a supported certificate size";
    return false;
  }

  rc = mbedtls_base64_decode(
      g_service_cli_anchor_der_buf, sizeof(g_service_cli_anchor_der_buf),
      &der_len, (const unsigned char *)workspace->anchor_der_base64,
      strlen(workspace->anchor_der_base64));
  if (rc != 0 || der_len == 0u) {
    *error_message_out =
        "config import upload.tls.anchor.der_base64 is invalid";
    return false;
  }

  workspace->anchor_subject[0] = '\0';
  if (!logger_extract_ca_subject(g_service_cli_anchor_der_buf, der_len,
                                 workspace->anchor_subject,
                                 sizeof(workspace->anchor_subject))) {
    *error_message_out =
        "config import upload.tls.anchor must be a valid CA certificate";
    return false;
  }

  workspace->anchor_sha256_hex[0] = '\0';
  if (!logger_compute_sha256_hex(g_service_cli_anchor_der_buf, der_len,
                                 workspace->anchor_sha256_hex)) {
    *error_message_out =
        "config import upload.tls.anchor SHA-256 computation failed";
    return false;
  }

  workspace->anchor_expected_sha256[0] = '\0';
  if (logger_json_object_has_key(doc, anchor_tok, "sha256") &&
      !logger_json_object_copy_string_or_null(
          doc, anchor_tok, "sha256", workspace->anchor_expected_sha256,
          sizeof(workspace->anchor_expected_sha256))) {
    *error_message_out = "config import upload.tls.anchor.sha256 is invalid";
    return false;
  }
  if (logger_string_present(workspace->anchor_expected_sha256) &&
      strcmp(workspace->anchor_expected_sha256, workspace->anchor_sha256_hex) !=
          0) {
    *error_message_out =
        "config import upload.tls.anchor.sha256 does not match der_base64";
    return false;
  }

  workspace->anchor_expected_subject[0] = '\0';
  if (logger_json_object_has_key(doc, anchor_tok, "subject") &&
      !logger_json_object_copy_string_or_null(
          doc, anchor_tok, "subject", workspace->anchor_expected_subject,
          sizeof(workspace->anchor_expected_subject))) {
    *error_message_out = "config import upload.tls.anchor.subject is invalid";
    return false;
  }
  if (logger_string_present(workspace->anchor_expected_subject) &&
      strcmp(workspace->anchor_expected_subject, workspace->anchor_subject) !=
          0) {
    *error_message_out =
        "config import upload.tls.anchor.subject does not match der_base64";
    return false;
  }

  if (!logger_config_set_provisioned_anchor_in_memory(
          config_out, g_service_cli_anchor_der_buf, der_len,
          workspace->anchor_sha256_hex, workspace->anchor_subject)) {
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

  logger_service_cli_config_import_workspace_t *const workspace =
      &g_service_cli_config_import_workspace;
  char *const tls_mode = workspace->tls_mode;
  char *const root_profile = workspace->root_profile;
  tls_mode[0] = '\0';
  root_profile[0] = '\0';
  if (logger_json_object_has_key(doc, tls_tok, "mode") &&
      !logger_json_object_copy_string_or_null(doc, tls_tok, "mode", tls_mode,
                                              sizeof(workspace->tls_mode))) {
    *error_message_out = "config import upload.tls.mode is invalid";
    return false;
  }
  if (logger_json_object_has_key(doc, tls_tok, "root_profile") &&
      !logger_json_object_copy_string_or_null(
          doc, tls_tok, "root_profile", root_profile,
          sizeof(workspace->root_profile))) {
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

static bool
logger_parse_config_import_document(const logger_app_t *app, const char *json,
                                    logger_persisted_state_t *state_out,
                                    bool *bond_cleared_out,
                                    const char **error_message_out) {
  logger_service_cli_config_import_workspace_t *const workspace =
      &g_service_cli_config_import_workspace;
  static jsmntok_t tokens[LOGGER_CONFIG_IMPORT_JSON_TOKEN_MAX];
  logger_json_doc_t doc;
  memset(workspace, 0, sizeof(*workspace));
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

  *state_out = app->persisted;
  logger_config_init(&state_out->config);

  if (!logger_json_object_copy_string_or_empty_required(
          &doc, identity_tok, "logger_id", state_out->config.logger_id,
          sizeof(state_out->config.logger_id)) ||
      !logger_json_object_copy_string_or_empty_required(
          &doc, identity_tok, "subject_id", state_out->config.subject_id,
          sizeof(state_out->config.subject_id))) {
    *error_message_out = "config import identity section is invalid";
    return false;
  }
  if (!logger_config_logger_id_valid(state_out->config.logger_id, true) ||
      !logger_config_subject_id_valid(state_out->config.subject_id, true)) {
    *error_message_out = "config import identity values are invalid";
    return false;
  }

  if (!logger_json_object_copy_string_or_empty_required(
          &doc, recording_tok, "bound_h10_address",
          workspace->imported_bound_address,
          sizeof(workspace->imported_bound_address))) {
    *error_message_out = "config import recording.bound_h10_address is invalid";
    return false;
  }
  if (logger_string_present(workspace->imported_bound_address)) {
    if (!logger_normalize_h10_address_local(
            workspace->imported_bound_address,
            workspace->normalized_bound_address)) {
      *error_message_out =
          "config import recording.bound_h10_address is invalid";
      return false;
    }
    logger_copy_string(state_out->config.bound_h10_address,
                       sizeof(state_out->config.bound_h10_address),
                       workspace->normalized_bound_address);
  } else {
    state_out->config.bound_h10_address[0] = '\0';
  }

  if (!logger_json_object_copy_string(
          &doc, recording_tok, "study_day_rollover_local",
          workspace->rollover_local, sizeof(workspace->rollover_local)) ||
      !logger_json_object_copy_string(&doc, recording_tok,
                                      "overnight_upload_window_start_local",
                                      workspace->upload_start_local,
                                      sizeof(workspace->upload_start_local)) ||
      !logger_json_object_copy_string(
          &doc, recording_tok, "overnight_upload_window_end_local",
          workspace->upload_end_local, sizeof(workspace->upload_end_local))) {
    *error_message_out = "config import recording policy fields are invalid";
    return false;
  }
  if (!logger_validate_fixed_policy_string(workspace->rollover_local,
                                           "04:00:00") ||
      !logger_validate_fixed_policy_string(workspace->upload_start_local,
                                           "22:00:00") ||
      !logger_validate_fixed_policy_string(workspace->upload_end_local,
                                           "06:00:00")) {
    *error_message_out = "config import contains unsupported non-default "
                         "recording policy values";
    return false;
  }

  if (!logger_json_object_copy_string_or_empty_required(
          &doc, time_tok, "timezone", state_out->config.timezone,
          sizeof(state_out->config.timezone))) {
    *error_message_out = "config import time.timezone is invalid";
    return false;
  }
  if (logger_string_present(state_out->config.timezone) &&
      !logger_timezone_supported(state_out->config.timezone)) {
    *error_message_out = "config import time.timezone is unsupported";
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

  if (!logger_json_array_copy_single_string(&doc, allowed_ssids_tok,
                                            workspace->allowed_ssid,
                                            sizeof(workspace->allowed_ssid)) ||
      networks_tok->size > 1) {
    *error_message_out =
        "config import currently supports at most one Wi-Fi network";
    return false;
  }

  bool network_psk_present_marker = false;
  if (networks_tok->size == 1) {
    const jsmntok_t *network_tok =
        logger_json_array_get(&doc, networks_tok, 0u);
    if (network_tok == NULL || network_tok->type != JSMN_OBJECT ||
        !logger_json_object_copy_string(&doc, network_tok, "ssid",
                                        workspace->network_ssid,
                                        sizeof(workspace->network_ssid))) {
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
            &doc, network_tok, "psk", workspace->network_psk,
            sizeof(workspace->network_psk))) {
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

  if ((logger_string_present(workspace->allowed_ssid) ||
       logger_string_present(workspace->network_ssid)) &&
      strcmp(workspace->allowed_ssid, workspace->network_ssid) != 0) {
    *error_message_out = "config import requires allowed_ssids[0] to match "
                         "wifi.networks[0].ssid";
    return false;
  }
  if (logger_string_present(workspace->network_ssid)) {
    logger_copy_string(state_out->config.wifi_ssid,
                       sizeof(state_out->config.wifi_ssid),
                       workspace->network_ssid);
    if (secrets_included) {
      logger_copy_string(state_out->config.wifi_psk,
                         sizeof(state_out->config.wifi_psk),
                         workspace->network_psk);
    } else {
      state_out->config.wifi_psk[0] = '\0';
    }
  }

  bool upload_enabled = false;
  if (!logger_json_object_get_bool(&doc, upload_tok, "enabled",
                                   &upload_enabled)) {
    *error_message_out = "config import upload.enabled is invalid";
    return false;
  }

  if (!logger_json_object_copy_string_or_empty_required(
          &doc, upload_tok, "url", workspace->upload_url,
          sizeof(workspace->upload_url))) {
    *error_message_out = "config import upload.url is invalid";
    return false;
  }
  if (logger_string_present(workspace->upload_url) &&
      !logger_config_upload_url_valid(workspace->upload_url, false)) {
    *error_message_out = "config import upload.url is invalid";
    return false;
  }
  const jsmntok_t *auth_tok = logger_json_object_get(&doc, upload_tok, "auth");
  if (auth_tok == NULL || auth_tok->type != JSMN_OBJECT) {
    *error_message_out = "config import upload.auth is invalid";
    return false;
  }

  bool api_key_present_marker = false;
  bool token_present_marker = false;
  if (!logger_json_object_copy_string(&doc, auth_tok, "type",
                                      workspace->auth_type,
                                      sizeof(workspace->auth_type))) {
    *error_message_out = "config import upload.auth.type is invalid";
    return false;
  }
  if (strcmp(workspace->auth_type, "none") != 0 &&
      strcmp(workspace->auth_type, "api_key_and_bearer") != 0) {
    *error_message_out =
        "config import upload.auth.type must be none or api_key_and_bearer";
    return false;
  }
  if (logger_json_object_has_key(&doc, auth_tok, "api_key") &&
      !logger_json_object_copy_string_or_empty_required(
          &doc, auth_tok, "api_key", workspace->auth_api_key,
          sizeof(workspace->auth_api_key))) {
    *error_message_out = "config import upload.auth.api_key is invalid";
    return false;
  }
  if (logger_json_object_has_key(&doc, auth_tok, "token") &&
      !logger_json_object_copy_string_or_empty_required(
          &doc, auth_tok, "token", workspace->auth_token,
          sizeof(workspace->auth_token))) {
    *error_message_out = "config import upload.auth.token is invalid";
    return false;
  }
  if (!logger_config_upload_api_key_valid(workspace->auth_api_key, true)) {
    *error_message_out = "config import upload.auth.api_key is invalid";
    return false;
  }
  if (!logger_config_upload_token_valid(workspace->auth_token, true)) {
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
    if (!logger_config_upload_url_valid(workspace->upload_url, false)) {
      *error_message_out = "config import requires upload.url to be an "
                           "absolute http:// or https:// URL without "
                           "control characters, userinfo, or fragments";
      return false;
    }
    logger_copy_string(state_out->config.upload_url,
                       sizeof(state_out->config.upload_url),
                       workspace->upload_url);
    if (!logger_parse_config_import_upload_tls(
            &doc, upload_tok, upload_enabled, workspace->upload_url,
            &state_out->config, error_message_out)) {
      return false;
    }
    if (strcmp(workspace->auth_type, "api_key_and_bearer") == 0) {
      if (secrets_included) {
        if (!logger_string_present(workspace->auth_api_key)) {
          *error_message_out = "config import api_key_and_bearer auth requires "
                               "api_key when secrets_included is true";
          return false;
        }
        if (!logger_string_present(workspace->auth_token)) {
          *error_message_out = "config import api_key_and_bearer auth requires "
                               "token when secrets_included is true";
          return false;
        }
        logger_copy_string(state_out->config.upload_api_key,
                           sizeof(state_out->config.upload_api_key),
                           workspace->auth_api_key);
        logger_copy_string(state_out->config.upload_token,
                           sizeof(state_out->config.upload_token),
                           workspace->auth_token);
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
        state_out->config.upload_api_key[0] = '\0';
        state_out->config.upload_token[0] = '\0';
      }
    } else {
      *error_message_out =
          "config import enabled upload requires api_key_and_bearer auth";
      return false;
    }
  } else {
    state_out->config.upload_url[0] = '\0';
    state_out->config.upload_api_key[0] = '\0';
    state_out->config.upload_token[0] = '\0';
    if (!logger_parse_config_import_upload_tls(
            &doc, upload_tok, upload_enabled, workspace->upload_url,
            &state_out->config, error_message_out)) {
      return false;
    }
  }

  if (bond_cleared_out != NULL) {
    *bond_cleared_out = strcmp(app->persisted.config.bound_h10_address,
                               state_out->config.bound_h10_address) != 0;
  }
  return true;
}

/* ------------------------------------------------------------------ */
/*  CLI response envelope (streaming JSON writer)                     */
/* ------------------------------------------------------------------ */

typedef logger_json_stream_writer_t jsw;

static void jsw_ok(jsw *w, const char *command, const char *utc) {
  logger_json_stream_writer_init(w, stdout);
  logger_json_stream_writer_object_begin(w);
  logger_json_stream_writer_field_uint32(w, "schema_version", 1u);
  logger_json_stream_writer_field_string_or_null(w, "command", command);
  logger_json_stream_writer_field_bool(w, "ok", true);
  logger_json_stream_writer_field_string_or_null(w, "generated_at_utc", utc);
  logger_json_stream_writer_field_object_begin(w, "payload");
}

static void jsw_end(jsw *w) {
  logger_json_stream_writer_object_end(w);
  logger_json_stream_writer_object_end(w);
  fputc('\n', w->stream);
  fflush(w->stream);
}

static void jsw_err(jsw *w, const char *command, const char *utc,
                    const char *code, const char *message) {
  logger_json_stream_writer_init(w, stdout);
  logger_json_stream_writer_object_begin(w);
  logger_json_stream_writer_field_uint32(w, "schema_version", 1u);
  logger_json_stream_writer_field_string_or_null(w, "command", command);
  logger_json_stream_writer_field_bool(w, "ok", false);
  logger_json_stream_writer_field_string_or_null(w, "generated_at_utc", utc);
  logger_json_stream_writer_field_object_begin(w, "error");
  logger_json_stream_writer_field_string_or_null(w, "code", code);
  logger_json_stream_writer_field_string_or_null(w, "message", message);
  logger_json_stream_writer_object_end(w);
  logger_json_stream_writer_object_end(w);
  fputc('\n', w->stream);
  fflush(w->stream);
}

/* ------------------------------------------------------------------ */
/*  Array element helpers                                             */
/* ------------------------------------------------------------------ */

static void logger_write_required_field_array(jsw *w, const logger_app_t *app,
                                              bool present) {
  static const char *names[] = {"bound_h10_address", "logger_id", "subject_id"};
  const char *values[] = {
      app->persisted.config.bound_h10_address,
      app->persisted.config.logger_id,
      app->persisted.config.subject_id,
  };
  for (size_t i = 0u; i < 3u; i++) {
    if (logger_string_present(values[i]) == present) {
      logger_json_stream_writer_elem_string_or_null(w, names[i]);
    }
  }
  if (logger_timezone_supported(app->persisted.config.timezone) == present) {
    logger_json_stream_writer_elem_string_or_null(w, "timezone");
  }
}

static void logger_write_optional_present_array(jsw *w,
                                                const logger_app_t *app) {
  if (logger_string_present(app->persisted.config.upload_api_key) &&
      logger_string_present(app->persisted.config.upload_token)) {
    logger_json_stream_writer_elem_string_or_null(w, "upload_auth");
  }
  if (logger_string_present(app->persisted.config.upload_url)) {
    logger_json_stream_writer_elem_string_or_null(w, "upload_url");
  }
  if (logger_config_wifi_configured(&app->persisted.config)) {
    logger_json_stream_writer_elem_string_or_null(w, "wifi_networks");
  }
}

static void logger_write_warnings_array(jsw *w, const logger_app_t *app) {
  if (!app->clock.valid) {
    logger_json_stream_writer_elem_string_or_null(w, "clock_invalid");
  }
  if (!logger_config_upload_configured(&app->persisted.config)) {
    logger_json_stream_writer_elem_string_or_null(w, "upload_not_configured");
  }
  if (!logger_config_wifi_configured(&app->persisted.config)) {
    logger_json_stream_writer_elem_string_or_null(w, "wifi_not_configured");
  }
  if (logger_string_present(app->persisted.config.timezone) &&
      !logger_timezone_supported(app->persisted.config.timezone)) {
    logger_json_stream_writer_elem_string_or_null(w, "timezone_unsupported");
  }
}

/* ------------------------------------------------------------------ */
/*  Mode / guard helpers                                              */
/* ------------------------------------------------------------------ */

static bool logger_cli_is_service_mode(const logger_app_t *app) {
  return app->runtime.current_state == LOGGER_RUNTIME_SERVICE;
}

static bool logger_cli_is_recovery_hold_mode(const logger_app_t *app) {
  return app->runtime.current_state == LOGGER_RUNTIME_RECOVERY_HOLD;
}

static bool logger_cli_is_logging_mode(const logger_app_t *app) {
  return logger_runtime_state_is_logging(app->runtime.current_state);
}

static bool logger_cli_require_service(const logger_app_t *app,
                                       const char *command) {
  if (logger_cli_is_logging_mode(app)) {
    jsw w;
    jsw_err(&w, command, logger_clock_now_utc_or_null(&app->clock),
            "busy_logging", "not permitted while logging");
    return false;
  }
  if (!logger_cli_is_service_mode(app)) {
    jsw w;
    jsw_err(&w, command, logger_clock_now_utc_or_null(&app->clock),
            "not_permitted_in_mode", "only allowed in service mode");
    return false;
  }
  return true;
}

static bool logger_cli_require_service_unlocked(const logger_service_cli_t *cli,
                                                const logger_app_t *app,
                                                const char *command) {
  if (!logger_cli_require_service(app, command)) {
    return false;
  }
  if (!logger_service_cli_is_unlocked(cli,
                                      to_ms_since_boot(get_absolute_time()))) {
    jsw w;
    jsw_err(&w, command, logger_clock_now_utc_or_null(&app->clock),
            "service_locked", "service unlock is required");
    return false;
  }
  return true;
}

static bool logger_debug_require_service_unlocked(
    const logger_service_cli_t *cli, const logger_app_t *app,
    const char *command, const char *mode_message) {
  if (!logger_cli_is_service_mode(app)) {
    jsw w;
    jsw_err(&w, command, logger_clock_now_utc_or_null(&app->clock),
            "not_permitted_in_mode", mode_message);
    return false;
  }
  if (!logger_service_cli_is_unlocked(cli,
                                      to_ms_since_boot(get_absolute_time()))) {
    jsw w;
    jsw_err(&w, command, logger_clock_now_utc_or_null(&app->clock),
            "service_locked", "service unlock is required");
    return false;
  }
  return true;
}

static bool logger_debug_require_service_or_recovery_hold_unlocked(
    const logger_service_cli_t *cli, const logger_app_t *app,
    const char *command, const char *mode_message) {
  if (!logger_cli_is_service_mode(app) &&
      !logger_cli_is_recovery_hold_mode(app)) {
    jsw w;
    jsw_err(&w, command, logger_clock_now_utc_or_null(&app->clock),
            "not_permitted_in_mode", mode_message);
    return false;
  }
  if (!logger_service_cli_is_unlocked(cli,
                                      to_ms_since_boot(get_absolute_time()))) {
    jsw w;
    jsw_err(&w, command, logger_clock_now_utc_or_null(&app->clock),
            "service_locked", "service unlock is required");
    return false;
  }
  return true;
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

void logger_service_cli_abort_mutable_session(logger_service_cli_t *cli) {
  if (cli == NULL) {
    return;
  }
  cli->unlocked = false;
  cli->unlock_deadline_mono_ms = 0u;
  logger_config_import_transfer_reset(cli);
}

static bool
logger_require_config_import_context(const logger_service_cli_t *cli,
                                     const logger_app_t *app,
                                     const char *command, bool require_unlock) {
  if (logger_cli_is_logging_mode(app)) {
    jsw w;
    jsw_err(&w, command, logger_clock_now_utc_or_null(&app->clock),
            "busy_logging", "config import is not permitted while logging");
    return false;
  }
  if (!logger_cli_is_service_mode(app)) {
    jsw w;
    jsw_err(&w, command, logger_clock_now_utc_or_null(&app->clock),
            "not_permitted_in_mode",
            "config import is only allowed in service mode");
    return false;
  }
  if (require_unlock && !logger_service_cli_is_unlocked(
                            cli, to_ms_since_boot(get_absolute_time()))) {
    jsw w;
    jsw_err(&w, command, logger_clock_now_utc_or_null(&app->clock),
            "service_locked",
            "service unlock is required before config import");
    return false;
  }
  return true;
}

static bool logger_cli_is_upload_mode(const logger_app_t *app) {
  return logger_runtime_state_is_upload(app->runtime.current_state);
}

/* ------------------------------------------------------------------ */
/*  JSON payload writers                                              */
/* ------------------------------------------------------------------ */

static void logger_write_storage_card_identity(jsw *w,
                                               const logger_app_t *app) {
  logger_json_stream_writer_field_string_or_null(w, "manufacturer_id",
                                                 app->storage.manufacturer_id);
  logger_json_stream_writer_field_string_or_null(w, "oem_id",
                                                 app->storage.oem_id);
  logger_json_stream_writer_field_string_or_null(w, "product_name",
                                                 app->storage.product_name);
  logger_json_stream_writer_field_string_or_null(w, "revision",
                                                 app->storage.revision);
  logger_json_stream_writer_field_string_or_null(w, "serial_number",
                                                 app->storage.serial_number);
}

static void logger_write_status_payload(jsw *w, const logger_app_t *app) {
  char study_day_local[11] = {0};
  const uint32_t now_ms = to_ms_since_boot(get_absolute_time());
  logger_upload_queue_t *queue = logger_upload_queue_tmp_acquire();
  logger_upload_queue_summary_t queue_summary;
  logger_upload_queue_summary_init(&queue_summary);
  if (logger_storage_svc_queue_load(queue) ||
      logger_storage_svc_queue_scan(
          queue, NULL, logger_clock_now_utc_or_null(&app->clock))) {
    logger_upload_queue_compute_summary(queue, &queue_summary);
  }
  const bool have_study_day =
      app->session.active
          ? true
          : (logger_runtime_state_is_logging(app->runtime.current_state) &&
             logger_clock_derive_study_day_local_observed(
                 &app->clock, app->persisted.config.timezone, study_day_local));

  logger_json_stream_writer_field_string_or_null(
      w, "mode", logger_mode_name(&app->runtime));
  logger_json_stream_writer_field_string_or_null(
      w, "runtime_state",
      logger_runtime_state_name(app->runtime.current_state));

  logger_json_stream_writer_field_object_begin(w, "identity");
  logger_json_stream_writer_field_string_or_null(w, "hardware_id",
                                                 app->hardware_id);
  logger_json_stream_writer_field_string_or_null(
      w, "logger_id", app->persisted.config.logger_id);
  logger_json_stream_writer_field_string_or_null(
      w, "subject_id", app->persisted.config.subject_id);
  logger_json_stream_writer_object_end(w);

  logger_json_stream_writer_field_object_begin(w, "boot");
  logger_json_stream_writer_field_uint32(w, "boot_counter",
                                         app->persisted.boot_counter);
  logger_json_stream_writer_field_bool(w, "firmware_changed",
                                       app->boot_firmware_identity_changed);
  logger_json_stream_writer_field_bool(w, "watchdog_reset",
                                       app->boot_watchdog_reset);
  logger_json_stream_writer_field_bool(w, "watchdog_timeout_reboot",
                                       app->boot_watchdog_timeout_reboot);
  logger_json_stream_writer_field_bool(w, "watchdog_forced_reboot",
                                       app->boot_watchdog_forced_reboot);
  logger_json_stream_writer_field_string_or_null(
      w, "reset_marker",
      logger_reset_marker_reason_name(app->boot_reset_marker_reason));
  logger_json_stream_writer_field_bool(w, "storage_service_timeout",
                                       app->boot_storage_service_timeout_reset);
  if (app->boot_storage_service_timeout_reset) {
    logger_json_stream_writer_field_uint32(
        w, "storage_service_kind", app->boot_storage_service_timeout_kind);
    logger_json_stream_writer_field_uint32(
        w, "storage_service_request_seq",
        app->boot_storage_service_timeout_request_seq);
  } else {
    logger_json_stream_writer_field_null(w, "storage_service_kind");
    logger_json_stream_writer_field_null(w, "storage_service_request_seq");
  }
  logger_json_stream_writer_object_end(w);

  logger_json_stream_writer_field_object_begin(w, "provisioning");
  logger_json_stream_writer_field_bool(
      w, "normal_logging_ready",
      logger_config_normal_logging_ready(&app->persisted.config));
  logger_json_stream_writer_field_array_begin(w, "required_missing");
  logger_write_required_field_array(w, app, false);
  logger_json_stream_writer_array_end(w);
  logger_json_stream_writer_field_array_begin(w, "warnings");
  logger_write_warnings_array(w, app);
  logger_json_stream_writer_array_end(w);
  logger_json_stream_writer_object_end(w);

  logger_json_stream_writer_field_object_begin(w, "fault");
  logger_json_stream_writer_field_bool(
      w, "latched", app->persisted.current_fault_code != LOGGER_FAULT_NONE);
  logger_json_stream_writer_field_string_or_null(
      w, "current_code",
      logger_fault_code_name(app->persisted.current_fault_code));
  logger_json_stream_writer_field_string_or_null(
      w, "last_cleared_code",
      logger_fault_code_name(app->persisted.last_cleared_fault_code));
  logger_json_stream_writer_object_end(w);

  logger_json_stream_writer_field_object_begin(w, "recovery");
  logger_json_stream_writer_field_bool(
      w, "active", app->runtime.current_state == LOGGER_RUNTIME_RECOVERY_HOLD);
  logger_json_stream_writer_field_string_or_null(
      w, "reason", logger_recovery_reason_name(app->recovery_reason));
  logger_json_stream_writer_field_uint32(w, "attempt_count",
                                         app->recovery_attempt_count);
  if (app->recovery_reason == LOGGER_RECOVERY_NONE) {
    logger_json_stream_writer_field_null(w, "next_attempt_ms");
  } else if (app->recovery_next_attempt_mono_ms == 0u ||
             logger_mono_ms_deadline_reached(
                 now_ms, app->recovery_next_attempt_mono_ms)) {
    logger_json_stream_writer_field_uint32(w, "next_attempt_ms", 0u);
  } else {
    logger_json_stream_writer_field_uint32(
        w, "next_attempt_ms", app->recovery_next_attempt_mono_ms - now_ms);
  }
  logger_json_stream_writer_field_string_or_null(
      w, "exit_policy",
      logger_recovery_exit_policy_name(app->recovery_exit_policy));
  logger_json_stream_writer_field_bool(w, "service_pinned_by_user",
                                       app->service_pinned_by_user);
  logger_json_stream_writer_field_string_or_null(
      w, "last_action",
      app->recovery_last_action[0] != '\0' ? app->recovery_last_action : NULL);
  logger_json_stream_writer_field_string_or_null(
      w, "last_result",
      app->recovery_last_result[0] != '\0' ? app->recovery_last_result : NULL);
  logger_json_stream_writer_object_end(w);

  logger_json_stream_writer_field_object_begin(w, "battery");
  logger_json_stream_writer_field_uint32(w, "voltage_mv",
                                         app->battery.voltage_mv);
  logger_json_stream_writer_field_int32(w, "estimate_pct",
                                        app->battery.estimate_pct);
  logger_json_stream_writer_field_bool(w, "vbus_present",
                                       app->battery.vbus_present);
  logger_json_stream_writer_field_uint32(w, "critical_stop_mv",
                                         LOGGER_BATTERY_CRITICAL_STOP_MV);
  logger_json_stream_writer_field_uint32(w, "low_start_mv",
                                         LOGGER_BATTERY_LOW_START_BLOCK_MV);
  logger_json_stream_writer_field_uint32(
      w, "off_charger_upload_mv", LOGGER_BATTERY_OFF_CHARGER_UPLOAD_MIN_MV);
  logger_json_stream_writer_object_end(w);

  logger_json_stream_writer_field_object_begin(w, "storage");
  logger_json_stream_writer_field_bool(w, "sd_present",
                                       app->storage.card_present);
  logger_json_stream_writer_field_bool(w, "mounted", app->storage.mounted);
  logger_json_stream_writer_field_bool(w, "writable", app->storage.writable);
  logger_json_stream_writer_field_bool(w, "logger_root_ready",
                                       app->storage.logger_root_ready);
  logger_json_stream_writer_field_bool(w, "reserve_ok",
                                       app->storage.reserve_ok);
  logger_json_stream_writer_field_string_or_null(
      w, "filesystem",
      logger_string_present(app->storage.filesystem) ? app->storage.filesystem
                                                     : NULL);
  if (app->storage.mounted) {
    logger_json_stream_writer_field_uint64(w, "free_bytes",
                                           app->storage.free_bytes);
  } else {
    logger_json_stream_writer_field_null(w, "free_bytes");
  }
  logger_json_stream_writer_field_uint32(w, "reserve_bytes",
                                         LOGGER_SD_MIN_FREE_RESERVE_BYTES);
  if (!logger_string_present(app->storage.manufacturer_id)) {
    logger_json_stream_writer_field_null(w, "card_identity");
  } else {
    logger_json_stream_writer_field_object_begin(w, "card_identity");
    logger_write_storage_card_identity(w, app);
    logger_json_stream_writer_object_end(w);
  }
  logger_json_stream_writer_object_end(w);

  logger_json_stream_writer_field_object_begin(w, "h10");
  logger_json_stream_writer_field_string_or_null(
      w, "bound_address", app->persisted.config.bound_h10_address);
  logger_json_stream_writer_field_bool(w, "connected", app->h10.connected);
  logger_json_stream_writer_field_bool(w, "encrypted", app->h10.encrypted);
  logger_json_stream_writer_field_bool(w, "bonded", app->h10.bonded);
  logger_json_stream_writer_field_string_or_null(
      w, "last_seen_address",
      app->h10.last_seen_address[0] != '\0' ? app->h10.last_seen_address
                                            : NULL);
  if (app->h10.battery_percent >= 0) {
    logger_json_stream_writer_field_int32(w, "battery_percent",
                                          app->h10.battery_percent);
  } else {
    logger_json_stream_writer_field_null(w, "battery_percent");
  }
  logger_json_stream_writer_field_string_or_null(
      w, "phase", logger_h10_phase_name(app->h10.phase));
  logger_json_stream_writer_field_uint32(w, "att_mtu", app->h10.att_mtu);
  logger_json_stream_writer_field_uint32(w, "encryption_key_size",
                                         app->h10.encryption_key_size);
  logger_json_stream_writer_field_string_or_null(
      w, "last_security_failure",
      logger_h10_security_failure_name(app->h10.last_security_failure));
  logger_json_stream_writer_field_uint32(w, "security_failures",
                                         app->h10.security_failure_count);
  logger_json_stream_writer_field_bool(w, "bond_repair_in_progress",
                                       app->h10.bond_repair_in_progress);
  logger_json_stream_writer_field_uint32(w, "bond_auto_clears",
                                         app->h10.bond_auto_clear_count);
  logger_json_stream_writer_field_uint32(w, "bond_auto_repairs",
                                         app->h10.bond_auto_repair_count);
  logger_json_stream_writer_field_uint32(w, "last_pairing_status",
                                         app->h10.last_pairing_status);
  logger_json_stream_writer_field_uint32(w, "last_pairing_reason",
                                         app->h10.last_pairing_reason);
  logger_json_stream_writer_field_uint32(
      w, "ecg_start_attempts", (uint32_t)app->h10.ecg_start_attempt_count);
  logger_json_stream_writer_field_uint32(
      w, "ecg_start_successes", (uint32_t)app->h10.ecg_start_success_count);
  logger_json_stream_writer_field_uint32(w, "ecg_packets",
                                         (uint32_t)app->h10.ecg_packet_count);
  logger_json_stream_writer_field_uint32(
      w, "ecg_packet_drops", (uint32_t)app->h10.ecg_packet_drop_count);
  logger_json_stream_writer_field_uint32(
      w, "acc_start_attempts", (uint32_t)app->h10.acc_start_attempt_count);
  logger_json_stream_writer_field_uint32(
      w, "acc_start_successes", (uint32_t)app->h10.acc_start_success_count);
  logger_json_stream_writer_field_uint32(w, "acc_packets",
                                         (uint32_t)app->h10.acc_packet_count);
  logger_json_stream_writer_field_uint32(
      w, "acc_packet_drops", (uint32_t)app->h10.acc_packet_drop_count);
  logger_json_stream_writer_object_end(w);

  logger_json_stream_writer_field_object_begin(w, "session");
  logger_json_stream_writer_field_bool(w, "active", app->session.active);
  logger_json_stream_writer_field_string_or_null(
      w, "session_id", app->session.active ? app->session.session_id : NULL);
  logger_json_stream_writer_field_string_or_null(
      w, "study_day_local",
      app->session.active ? app->session.study_day_local
                          : (have_study_day ? study_day_local : NULL));
  logger_json_stream_writer_field_string_or_null(
      w, "span_id",
      (app->session.active && app->session.span_active)
          ? app->session.current_span_id
          : NULL);
  logger_json_stream_writer_field_bool(
      w, "quarantined", app->session.active && app->session.quarantined);
  logger_json_stream_writer_field_string_or_null(
      w, "clock_state",
      app->session.active ? app->session.clock_state
                          : logger_clock_state_name(&app->clock));
  if (app->session.active) {
    logger_json_stream_writer_field_uint64(
        w, "journal_size_bytes",
        logger_session_writer_journal_size_approx(&app->session));
  } else {
    logger_json_stream_writer_field_null(w, "journal_size_bytes");
  }
  logger_json_stream_writer_object_end(w);

  logger_json_stream_writer_field_object_begin(w, "upload_queue");
  logger_json_stream_writer_field_uint32(w, "pending_count",
                                         queue_summary.pending_count);
  logger_json_stream_writer_field_uint32(w, "blocked_count",
                                         queue_summary.blocked_count);
  logger_json_stream_writer_field_string_or_null(
      w, "oldest_pending_study_day",
      logger_string_present(queue_summary.oldest_pending_study_day)
          ? queue_summary.oldest_pending_study_day
          : NULL);
  logger_json_stream_writer_field_string_or_null(
      w, "last_failure_class",
      logger_string_present(queue_summary.last_failure_class)
          ? queue_summary.last_failure_class
          : NULL);
  logger_json_stream_writer_field_string_or_null(
      w, "blocked_reason",
      logger_upload_queue_summary_blocked_reason_hint(&queue_summary));
  logger_json_stream_writer_field_string_or_null(
      w, "blocked_retry_hint",
      logger_upload_queue_summary_blocked_retry_hint(&queue_summary));
  logger_json_stream_writer_object_end(w);

  logger_json_stream_writer_field_object_begin(w, "last_day_outcome");
  logger_json_stream_writer_field_string_or_null(
      w, "study_day_local",
      app->last_day_outcome_valid ? app->last_day_outcome_study_day_local
                                  : NULL);
  logger_json_stream_writer_field_string_or_null(
      w, "kind",
      app->last_day_outcome_valid ? app->last_day_outcome_kind : NULL);
  logger_json_stream_writer_field_string_or_null(
      w, "reason",
      app->last_day_outcome_valid ? app->last_day_outcome_reason : NULL);
  logger_json_stream_writer_object_end(w);

  logger_json_stream_writer_field_object_begin(w, "firmware");
  logger_json_stream_writer_field_string_or_null(w, "version",
                                                 LOGGER_FIRMWARE_VERSION);
  logger_json_stream_writer_field_string_or_null(w, "build_id",
                                                 LOGGER_BUILD_ID);
  logger_json_stream_writer_object_end(w);

  logger_upload_queue_tmp_release(queue);
}

static void logger_handle_status_json(const logger_app_t *app) {
  jsw w;
  jsw_ok(&w, "status", logger_clock_now_utc_or_null(&app->clock));
  logger_write_status_payload(&w, app);
  jsw_end(&w);
}

static void
logger_handle_status_upload_busy_json(const logger_app_t *app,
                                      logger_busy_poll_phase_t phase) {
  jsw w;
  jsw_ok(&w, "status", logger_clock_now_utc_or_null(&app->clock));
  logger_json_stream_writer_field_string_or_null(
      &w, "mode", logger_mode_name(&app->runtime));
  logger_json_stream_writer_field_string_or_null(
      &w, "runtime_state",
      logger_runtime_state_name(app->runtime.current_state));
  logger_json_stream_writer_field_object_begin(&w, "upload_busy");
  logger_json_stream_writer_field_bool(&w, "active", true);
  logger_json_stream_writer_field_string_or_null(
      &w, "phase", logger_busy_poll_phase_name(phase));
  logger_json_stream_writer_field_string_or_null(&w, "command_handling",
                                                 "bounded_busy_pump");
  logger_json_stream_writer_object_end(&w);
  jsw_end(&w);
}

/*
 * Bounded upload-busy dispatcher.
 *
 * This deliberately does not call logger_service_cli_execute(): upload owns
 * the queue workspace, bundle stream, Wi-Fi, and TLS client while this pump is
 * active.  Only immediate, non-mutating busy responses are allowed here.
 */
static void logger_service_cli_execute_upload_busy(
    logger_service_cli_t *cli, logger_app_t *app, const char *line,
    uint32_t now_ms, logger_busy_poll_phase_t phase) {
  if (cli->unlocked &&
      logger_mono_ms_deadline_reached(now_ms, cli->unlock_deadline_mono_ms)) {
    cli->unlocked = false;
  }

  if (strcmp(line, "status --json") == 0) {
    logger_handle_status_upload_busy_json(app, phase);
    return;
  }

  if (strcmp(line, "service enter") == 0) {
    jsw w;
    jsw_err(&w, "service enter", logger_clock_now_utc_or_null(&app->clock),
            "not_permitted_in_mode",
            "service enter is not permitted while upload is in progress");
    return;
  }

  jsw w;
  jsw_err(&w, line, logger_clock_now_utc_or_null(&app->clock), "busy_upload",
          "upload is in progress; retry after the current upload attempt "
          "completes");
}

static void logger_handle_provisioning_status_json(logger_app_t *app) {
  jsw w;
  jsw_ok(&w, "provisioning-status", logger_clock_now_utc_or_null(&app->clock));
  logger_json_stream_writer_field_bool(
      &w, "normal_logging_ready",
      logger_config_normal_logging_ready(&app->persisted.config));
  logger_json_stream_writer_field_array_begin(&w, "required_present");
  logger_write_required_field_array(&w, app, true);
  logger_json_stream_writer_array_end(&w);
  logger_json_stream_writer_field_array_begin(&w, "required_missing");
  logger_write_required_field_array(&w, app, false);
  logger_json_stream_writer_array_end(&w);
  logger_json_stream_writer_field_array_begin(&w, "optional_present");
  logger_write_optional_present_array(&w, app);
  logger_json_stream_writer_array_end(&w);
  logger_json_stream_writer_field_array_begin(&w, "warnings");
  logger_write_warnings_array(&w, app);
  logger_json_stream_writer_array_end(&w);
  jsw_end(&w);
}

static void logger_handle_queue_json(logger_app_t *app) {
  logger_upload_queue_t *queue = logger_upload_queue_tmp_acquire();
  if (!logger_storage_svc_queue_load(queue)) {
    (void)logger_storage_svc_queue_scan(
        queue, NULL, logger_clock_now_utc_or_null(&app->clock));
  }

  jsw w;
  jsw_ok(&w, "queue", logger_clock_now_utc_or_null(&app->clock));
  logger_json_stream_writer_field_string_or_null(&w, "schema_source",
                                                 "upload_queue_slots_v1");
  logger_json_stream_writer_field_string_or_null(
      &w, "updated_at_utc",
      logger_string_present(queue->updated_at_utc) ? queue->updated_at_utc
                                                   : NULL);
  logger_json_stream_writer_field_array_begin(&w, "sessions");
  for (size_t i = 0u; i < queue->session_count; ++i) {
    const logger_upload_queue_entry_t *entry = &queue->sessions[i];
    logger_json_stream_writer_elem_object_begin(&w);
    logger_json_stream_writer_field_string_or_null(&w, "session_id",
                                                   entry->session_id);
    logger_json_stream_writer_field_string_or_null(&w, "study_day_local",
                                                   entry->study_day_local);
    logger_json_stream_writer_field_string_or_null(&w, "dir_name",
                                                   entry->dir_name);
    logger_json_stream_writer_field_string_or_null(&w, "session_start_utc",
                                                   entry->session_start_utc);
    logger_json_stream_writer_field_string_or_null(&w, "session_end_utc",
                                                   entry->session_end_utc);
    logger_json_stream_writer_field_string_or_null(&w, "bundle_sha256",
                                                   entry->bundle_sha256);
    logger_json_stream_writer_field_uint64(&w, "bundle_size_bytes",
                                           entry->bundle_size_bytes);
    logger_json_stream_writer_field_string_or_null(&w, "status", entry->status);
    logger_json_stream_writer_field_bool(&w, "quarantined", entry->quarantined);
    logger_json_stream_writer_field_uint32(&w, "attempt_count",
                                           (uint32_t)entry->attempt_count);
    logger_json_stream_writer_field_string_or_null(
        &w, "last_attempt_utc",
        logger_string_present(entry->last_attempt_utc) ? entry->last_attempt_utc
                                                       : NULL);
    logger_json_stream_writer_field_string_or_null(
        &w, "last_failure_class",
        logger_string_present(entry->last_failure_class)
            ? entry->last_failure_class
            : NULL);
    if (entry->last_http_status > 0u) {
      logger_json_stream_writer_field_int32(&w, "last_http_status",
                                            (int32_t)entry->last_http_status);
    } else {
      logger_json_stream_writer_field_null(&w, "last_http_status");
    }
    logger_json_stream_writer_field_string_or_null(
        &w, "last_server_error_code",
        logger_string_present(entry->last_server_error_code)
            ? entry->last_server_error_code
            : NULL);
    logger_json_stream_writer_field_string_or_null(
        &w, "last_server_error_message",
        logger_string_present(entry->last_server_error_message)
            ? entry->last_server_error_message
            : NULL);
    logger_json_stream_writer_field_string_or_null(
        &w, "last_response_excerpt",
        logger_string_present(entry->last_response_excerpt)
            ? entry->last_response_excerpt
            : NULL);
    logger_json_stream_writer_field_string_or_null(
        &w, "verified_upload_utc",
        logger_string_present(entry->verified_upload_utc)
            ? entry->verified_upload_utc
            : NULL);
    logger_json_stream_writer_field_string_or_null(
        &w, "verified_bundle_sha256",
        logger_string_present(entry->verified_bundle_sha256)
            ? entry->verified_bundle_sha256
            : NULL);
    logger_json_stream_writer_field_string_or_null(
        &w, "receipt_id",
        logger_string_present(entry->receipt_id) ? entry->receipt_id : NULL);
    logger_json_stream_writer_field_string_or_null(
        &w, "status_detail", logger_upload_queue_entry_status_detail(entry));
    logger_json_stream_writer_field_string_or_null(
        &w, "retry_hint", logger_upload_queue_entry_retry_hint(entry));
    logger_json_stream_writer_object_end(&w);
  }
  logger_json_stream_writer_array_end(&w);
  jsw_end(&w);
  logger_upload_queue_tmp_release(queue);
}

static void logger_handle_system_log_export_json(logger_app_t *app) {
  logger_system_log_refresh(&app->system_log);

  jsw w;
  jsw_ok(&w, "system-log export", logger_clock_now_utc_or_null(&app->clock));
  logger_json_stream_writer_field_uint32(&w, "schema_version", 1u);
  logger_json_stream_writer_field_string_or_null(
      &w, "exported_at_utc", logger_clock_now_utc_or_null(&app->clock));
  logger_json_stream_writer_field_array_begin(&w, "events");

  for (uint32_t i = 0u; i < logger_system_log_count(&app->system_log); ++i) {
    logger_system_log_event_t *event = &g_service_cli_system_log_event;
    if (!logger_system_log_read_event(&app->system_log, i, event)) {
      continue;
    }
    logger_json_stream_writer_elem_object_begin(&w);
    logger_json_stream_writer_field_uint32(&w, "event_seq", event->event_seq);
    logger_json_stream_writer_field_string_or_null(
        &w, "utc", logger_string_present(event->utc) ? event->utc : NULL);
    logger_json_stream_writer_field_uint32(&w, "boot_counter",
                                           event->boot_counter);
    logger_json_stream_writer_field_string_or_null(&w, "kind", event->kind);
    logger_json_stream_writer_field_string_or_null(
        &w, "severity", logger_system_log_severity_name(event->severity));
    logger_json_stream_writer_field_raw(
        &w, "details",
        logger_string_present(event->details_json) ? event->details_json
                                                   : "{}");
    logger_json_stream_writer_object_end(&w);
  }

  logger_json_stream_writer_array_end(&w);
  jsw_end(&w);
}

static void logger_write_upload_tls_json(jsw *w,
                                         const logger_config_t *config) {
  const char *tls_mode = logger_config_upload_tls_mode(config);

  logger_json_stream_writer_field_object_begin(w, "tls");
  logger_json_stream_writer_field_string_or_null(w, "mode", tls_mode);
  logger_json_stream_writer_field_string_or_null(
      w, "root_profile",
      tls_mode != NULL &&
              strcmp(tls_mode, LOGGER_UPLOAD_TLS_MODE_PUBLIC_ROOTS) == 0
          ? LOGGER_UPLOAD_TLS_PUBLIC_ROOT_PROFILE
          : NULL);

  if (tls_mode == NULL ||
      strcmp(tls_mode, LOGGER_UPLOAD_TLS_MODE_PROVISIONED_ANCHOR) != 0 ||
      !logger_config_upload_has_provisioned_anchor(config)) {
    logger_json_stream_writer_field_null(w, "anchor");
    logger_json_stream_writer_object_end(w);
    return;
  }

  size_t der_base64_len = 0u;
  if (mbedtls_base64_encode(
          (unsigned char *)g_service_cli_anchor_der_base64_buf,
          sizeof(g_service_cli_anchor_der_base64_buf), &der_base64_len,
          config->upload_tls_anchor_der,
          config->upload_tls_anchor_der_len) != 0 ||
      der_base64_len >= sizeof(g_service_cli_anchor_der_base64_buf)) {
    logger_json_stream_writer_field_null(w, "anchor");
    logger_json_stream_writer_object_end(w);
    return;
  }
  g_service_cli_anchor_der_base64_buf[der_base64_len] = '\0';

  logger_json_stream_writer_field_object_begin(w, "anchor");
  logger_json_stream_writer_field_string_or_null(
      w, "format", LOGGER_UPLOAD_TLS_ANCHOR_FORMAT_X509_DER_BASE64);
  logger_json_stream_writer_field_string_or_null(
      w, "der_base64", g_service_cli_anchor_der_base64_buf);
  logger_json_stream_writer_field_string_or_null(
      w, "sha256",
      logger_string_present(config->upload_tls_anchor_sha256)
          ? config->upload_tls_anchor_sha256
          : NULL);
  logger_json_stream_writer_field_string_or_null(
      w, "subject",
      logger_string_present(config->upload_tls_anchor_subject)
          ? config->upload_tls_anchor_subject
          : NULL);
  logger_json_stream_writer_object_end(w);
  logger_json_stream_writer_object_end(w);
}

static void logger_handle_config_export_json(logger_app_t *app) {
  jsw w;
  jsw_ok(&w, "config export", logger_clock_now_utc_or_null(&app->clock));
  logger_json_stream_writer_field_uint32(&w, "schema_version", 1u);
  logger_json_stream_writer_field_string_or_null(
      &w, "exported_at_utc", logger_clock_now_utc_or_null(&app->clock));
  logger_json_stream_writer_field_string_or_null(&w, "hardware_id",
                                                 app->hardware_id);
  logger_json_stream_writer_field_bool(&w, "secrets_included", false);

  logger_json_stream_writer_field_object_begin(&w, "identity");
  logger_json_stream_writer_field_string_or_null(
      &w, "logger_id", app->persisted.config.logger_id);
  logger_json_stream_writer_field_string_or_null(
      &w, "subject_id", app->persisted.config.subject_id);
  logger_json_stream_writer_object_end(&w);

  logger_json_stream_writer_field_object_begin(&w, "recording");
  logger_json_stream_writer_field_string_or_null(
      &w, "bound_h10_address", app->persisted.config.bound_h10_address);
  logger_json_stream_writer_field_string_or_null(&w, "study_day_rollover_local",
                                                 "04:00:00");
  logger_json_stream_writer_field_string_or_null(
      &w, "overnight_upload_window_start_local", "22:00:00");
  logger_json_stream_writer_field_string_or_null(
      &w, "overnight_upload_window_end_local", "06:00:00");
  logger_json_stream_writer_object_end(&w);

  logger_json_stream_writer_field_object_begin(&w, "time");
  logger_json_stream_writer_field_string_or_null(
      &w, "timezone", app->persisted.config.timezone);
  logger_json_stream_writer_object_end(&w);

  logger_json_stream_writer_field_object_begin(&w, "battery_policy");
  logger_json_stream_writer_field_raw(&w, "critical_stop_voltage_v", "3.5");
  logger_json_stream_writer_field_raw(&w, "low_start_voltage_v", "3.65");
  logger_json_stream_writer_field_raw(&w, "off_charger_upload_voltage_v",
                                      "3.85");
  logger_json_stream_writer_object_end(&w);

  logger_json_stream_writer_field_object_begin(&w, "wifi");
  logger_json_stream_writer_field_array_begin(&w, "allowed_ssids");
  if (logger_string_present(app->persisted.config.wifi_ssid)) {
    logger_json_stream_writer_elem_string_or_null(
        &w, app->persisted.config.wifi_ssid);
  }
  logger_json_stream_writer_array_end(&w);
  logger_json_stream_writer_field_array_begin(&w, "networks");
  if (logger_string_present(app->persisted.config.wifi_ssid)) {
    logger_json_stream_writer_elem_object_begin(&w);
    logger_json_stream_writer_field_string_or_null(
        &w, "ssid", app->persisted.config.wifi_ssid);
    logger_json_stream_writer_field_bool(
        &w, "psk_present",
        logger_string_present(app->persisted.config.wifi_psk));
    logger_json_stream_writer_object_end(&w);
  }
  logger_json_stream_writer_array_end(&w);
  logger_json_stream_writer_object_end(&w);

  logger_json_stream_writer_field_object_begin(&w, "upload");
  logger_json_stream_writer_field_bool(
      &w, "enabled", logger_string_present(app->persisted.config.upload_url));
  logger_json_stream_writer_field_string_or_null(
      &w, "url", app->persisted.config.upload_url);
  logger_json_stream_writer_field_object_begin(&w, "auth");
  logger_json_stream_writer_field_string_or_null(
      &w, "type",
      logger_string_present(app->persisted.config.upload_url)
          ? "api_key_and_bearer"
          : "none");
  logger_json_stream_writer_field_bool(
      &w, "api_key_present",
      logger_string_present(app->persisted.config.upload_api_key));
  logger_json_stream_writer_field_bool(
      &w, "token_present",
      logger_string_present(app->persisted.config.upload_token));
  logger_json_stream_writer_object_end(&w);
  logger_write_upload_tls_json(&w, &app->persisted.config);
  logger_json_stream_writer_object_end(&w);

  jsw_end(&w);
}

static void logger_handle_clock_status_json(logger_app_t *app) {
  jsw w;
  jsw_ok(&w, "clock status", logger_clock_now_utc_or_null(&app->clock));
  logger_json_stream_writer_field_bool(&w, "rtc_present",
                                       app->clock.rtc_present);
  logger_json_stream_writer_field_bool(&w, "valid", app->clock.valid);
  logger_json_stream_writer_field_bool(&w, "lost_power", app->clock.lost_power);
  logger_json_stream_writer_field_bool(&w, "battery_low",
                                       app->clock.battery_low);
  logger_json_stream_writer_field_string_or_null(
      &w, "state", logger_clock_state_name(&app->clock));
  logger_json_stream_writer_field_string_or_null(
      &w, "now_utc", logger_clock_now_utc_or_null(&app->clock));
  logger_json_stream_writer_field_string_or_null(
      &w, "timezone", app->persisted.config.timezone);
  jsw_end(&w);
}

static void logger_handle_preflight_json(logger_app_t *app) {
  if (logger_cli_is_logging_mode(app)) {
    jsw w;
    jsw_err(&w, "preflight", logger_clock_now_utc_or_null(&app->clock),
            "busy_logging", "preflight is not permitted while logging");
    return;
  }
  if (logger_cli_is_upload_mode(app)) {
    jsw w;
    jsw_err(&w, "preflight", logger_clock_now_utc_or_null(&app->clock),
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

  jsw w;
  jsw_ok(&w, "preflight", logger_clock_now_utc_or_null(&app->clock));
  logger_json_stream_writer_field_string_or_null(&w, "overall_result", overall);
  logger_json_stream_writer_field_array_begin(&w, "checks");

  /* rtc */
  logger_json_stream_writer_elem_object_begin(&w);
  logger_json_stream_writer_field_string_or_null(&w, "name", "rtc");
  logger_json_stream_writer_field_string_or_null(&w, "result", rtc_result);
  logger_json_stream_writer_field_object_begin(&w, "details");
  logger_json_stream_writer_field_bool(&w, "rtc_present",
                                       app->clock.rtc_present);
  logger_json_stream_writer_field_bool(&w, "valid", app->clock.valid);
  logger_json_stream_writer_field_bool(&w, "lost_power", app->clock.lost_power);
  logger_json_stream_writer_object_end(&w);
  logger_json_stream_writer_object_end(&w);

  /* storage */
  logger_json_stream_writer_elem_object_begin(&w);
  logger_json_stream_writer_field_string_or_null(&w, "name", "storage");
  logger_json_stream_writer_field_string_or_null(&w, "result", storage_result);
  logger_json_stream_writer_field_object_begin(&w, "details");
  logger_json_stream_writer_field_bool(&w, "mounted", app->storage.mounted);
  logger_json_stream_writer_field_string_or_null(
      &w, "filesystem",
      logger_string_present(app->storage.filesystem) ? app->storage.filesystem
                                                     : NULL);
  if (app->storage.mounted) {
    logger_json_stream_writer_field_uint64(&w, "free_bytes",
                                           app->storage.free_bytes);
  } else {
    logger_json_stream_writer_field_null(&w, "free_bytes");
  }
  logger_json_stream_writer_field_bool(&w, "reserve_ok",
                                       app->storage.reserve_ok);
  logger_json_stream_writer_object_end(&w);
  logger_json_stream_writer_object_end(&w);

  /* battery_sense */
  logger_json_stream_writer_elem_object_begin(&w);
  logger_json_stream_writer_field_string_or_null(&w, "name", "battery_sense");
  logger_json_stream_writer_field_string_or_null(&w, "result", battery_result);
  logger_json_stream_writer_field_object_begin(&w, "details");
  logger_json_stream_writer_field_uint32(&w, "voltage_mv",
                                         app->battery.voltage_mv);
  logger_json_stream_writer_field_bool(&w, "vbus_present",
                                       app->battery.vbus_present);
  logger_json_stream_writer_object_end(&w);
  logger_json_stream_writer_object_end(&w);

  /* provisioning */
  logger_json_stream_writer_elem_object_begin(&w);
  logger_json_stream_writer_field_string_or_null(&w, "name", "provisioning");
  logger_json_stream_writer_field_string_or_null(&w, "result", prov_result);
  logger_json_stream_writer_field_object_begin(&w, "details");
  logger_json_stream_writer_field_bool(
      &w, "normal_logging_ready",
      logger_config_normal_logging_ready(&app->persisted.config));
  logger_json_stream_writer_object_end(&w);
  logger_json_stream_writer_object_end(&w);

  /* bound_h10_scan */
  logger_json_stream_writer_elem_object_begin(&w);
  logger_json_stream_writer_field_string_or_null(&w, "name", "bound_h10_scan");
  logger_json_stream_writer_field_string_or_null(&w, "result", h10_scan_result);
  logger_json_stream_writer_field_object_begin(&w, "details");
  logger_json_stream_writer_field_bool(&w, "implemented", false);
  logger_json_stream_writer_field_string_or_null(
      &w, "bound_address", app->persisted.config.bound_h10_address);
  logger_json_stream_writer_object_end(&w);
  logger_json_stream_writer_object_end(&w);

  logger_json_stream_writer_array_end(&w);
  jsw_end(&w);
}

static void logger_handle_net_test_json(logger_app_t *app) {
  if (!logger_cli_require_service(app, "net-test")) {
    return;
  }

  logger_upload_net_test_result_t *result = &g_service_cli_net_test_result;
  const bool ok = logger_upload_net_test(&app->persisted.config, result);

  jsw w;
  jsw_ok(&w, "net-test", logger_clock_now_utc_or_null(&app->clock));

  logger_json_stream_writer_field_object_begin(&w, "wifi_join");
  logger_json_stream_writer_field_string_or_null(&w, "result",
                                                 result->wifi_join_result);
  logger_json_stream_writer_field_object_begin(&w, "details");
  logger_json_stream_writer_field_string_or_null(&w, "message",
                                                 result->wifi_join_details);
  logger_json_stream_writer_object_end(&w);
  logger_json_stream_writer_object_end(&w);

  logger_json_stream_writer_field_object_begin(&w, "dns");
  logger_json_stream_writer_field_string_or_null(&w, "result",
                                                 result->dns_result);
  logger_json_stream_writer_field_object_begin(&w, "details");
  logger_json_stream_writer_field_string_or_null(&w, "message",
                                                 result->dns_details);
  logger_json_stream_writer_object_end(&w);
  logger_json_stream_writer_object_end(&w);

  logger_json_stream_writer_field_object_begin(&w, "tls");
  logger_json_stream_writer_field_string_or_null(&w, "result",
                                                 result->tls_result);
  logger_json_stream_writer_field_object_begin(&w, "details");
  logger_json_stream_writer_field_string_or_null(&w, "message",
                                                 result->tls_details);
  logger_json_stream_writer_object_end(&w);
  logger_json_stream_writer_object_end(&w);

  logger_json_stream_writer_field_object_begin(&w, "upload_endpoint_reachable");
  logger_json_stream_writer_field_string_or_null(
      &w, "result", result->upload_endpoint_reachable_result);
  logger_json_stream_writer_field_object_begin(&w, "details");
  logger_json_stream_writer_field_string_or_null(
      &w, "message", result->upload_endpoint_reachable_details);
  logger_json_stream_writer_field_bool(&w, "ok", ok);
  logger_json_stream_writer_object_end(&w);
  logger_json_stream_writer_object_end(&w);

  jsw_end(&w);
}

static void logger_handle_service_unlock(logger_service_cli_t *cli,
                                         logger_app_t *app, uint32_t now_ms) {
  if (!logger_cli_require_service(app, "service unlock")) {
    return;
  }

  cli->unlocked = true;
  cli->unlock_deadline_mono_ms = now_ms + 60000u;
  (void)logger_system_log_append(
      &app->system_log, logger_clock_now_utc_or_null(&app->clock),
      "service_unlock", LOGGER_SYSTEM_LOG_SEVERITY_INFO, "{}");

  jsw w;
  jsw_ok(&w, "service unlock", logger_clock_now_utc_or_null(&app->clock));
  logger_json_stream_writer_field_bool(&w, "unlocked", true);
  logger_json_stream_writer_field_string_or_null(
      &w, "expires_at_utc", logger_clock_now_utc_or_null(&app->clock));
  logger_json_stream_writer_field_uint32(&w, "ttl_seconds", 60u);
  jsw_end(&w);
}

static void logger_handle_service_enter(logger_app_t *app, uint32_t now_ms) {
  const bool already_in_service = logger_cli_is_service_mode(app);
  bool will_stop_logging = false;
  if (!logger_app_request_service_mode(app, now_ms, &will_stop_logging)) {
    jsw w;
    jsw_err(&w, "service enter", logger_clock_now_utc_or_null(&app->clock),
            "not_permitted_in_mode",
            "service enter is not permitted in the current mode");
    return;
  }

  (void)logger_system_log_append(
      &app->system_log, logger_clock_now_utc_or_null(&app->clock),
      "service_request", LOGGER_SYSTEM_LOG_SEVERITY_INFO,
      already_in_service
          ? "{\"source\":\"host\",\"already_in_service\":true}"
          : "{\"source\":\"host\",\"already_in_service\":false}");

  jsw w;
  jsw_ok(&w, "service enter", logger_clock_now_utc_or_null(&app->clock));
  logger_json_stream_writer_field_bool(&w, "requested", true);
  logger_json_stream_writer_field_bool(&w, "already_in_service",
                                       already_in_service);
  logger_json_stream_writer_field_bool(&w, "will_stop_logging",
                                       will_stop_logging);
  logger_json_stream_writer_field_string_or_null(
      &w, "mode", logger_mode_name(&app->runtime));
  logger_json_stream_writer_field_string_or_null(
      &w, "runtime_state",
      logger_runtime_state_name(app->runtime.current_state));
  logger_json_stream_writer_field_string_or_null(&w, "target_mode", "service");
  jsw_end(&w);
}

static void logger_handle_fault_clear(logger_app_t *app, uint32_t now_ms) {
  if (logger_cli_is_logging_mode(app)) {
    jsw w;
    jsw_err(&w, "fault clear", logger_clock_now_utc_or_null(&app->clock),
            "busy_logging", "fault clear is not permitted while logging");
    return;
  }
  if (logger_cli_is_upload_mode(app)) {
    jsw w;
    jsw_err(&w, "fault clear", logger_clock_now_utc_or_null(&app->clock),
            "not_permitted_in_mode",
            "fault clear is not permitted during upload");
    return;
  }

  logger_fault_code_t previous = LOGGER_FAULT_NONE;
  const logger_fault_clear_result_t result =
      logger_app_manual_clear_current_fault(app, now_ms, &previous);
  if (result == LOGGER_FAULT_CLEAR_NO_FAULT) {
    jsw w;
    jsw_ok(&w, "fault clear", logger_clock_now_utc_or_null(&app->clock));
    logger_json_stream_writer_field_bool(&w, "cleared", false);
    logger_json_stream_writer_field_null(&w, "previous_code");
    jsw_end(&w);
    return;
  }
  if (result == LOGGER_FAULT_CLEAR_CONDITION_PRESENT) {
    jsw w;
    jsw_err(&w, "fault clear", logger_clock_now_utc_or_null(&app->clock),
            "condition_still_present", "fault condition is still present");
    return;
  }
  if (result == LOGGER_FAULT_CLEAR_NOT_CLEARABLE) {
    jsw w;
    jsw_err(&w, "fault clear", logger_clock_now_utc_or_null(&app->clock),
            "not_clearable", "fault cannot be cleared manually");
    return;
  }

  jsw w;
  jsw_ok(&w, "fault clear", logger_clock_now_utc_or_null(&app->clock));
  logger_json_stream_writer_field_bool(&w, "cleared", true);
  logger_json_stream_writer_field_string_or_null(
      &w, "previous_code", logger_fault_code_name(previous));
  jsw_end(&w);
}

static void logger_handle_factory_reset(logger_service_cli_t *cli,
                                        logger_app_t *app) {
  if (!logger_cli_require_service_unlocked(cli, app, "factory-reset")) {
    return;
  }

  (void)logger_system_log_append(
      &app->system_log, logger_clock_now_utc_or_null(&app->clock),
      "factory_reset", LOGGER_SYSTEM_LOG_SEVERITY_WARN,
      "{\"source\":\"service_cli\"}");
  (void)logger_config_store_factory_reset(&app->persisted);
  cli->unlocked = false;
  app->reboot_pending = true;

  jsw w;
  jsw_ok(&w, "factory-reset", logger_clock_now_utc_or_null(&app->clock));
  logger_json_stream_writer_field_bool(&w, "factory_reset", true);
  logger_json_stream_writer_field_bool(&w, "will_reboot", true);
  jsw_end(&w);
}

static void logger_handle_sd_format(logger_service_cli_t *cli,
                                    logger_app_t *app) {
  if (!logger_cli_require_service_unlocked(cli, app, "sd format")) {
    return;
  }
  if (app->session.active) {
    jsw w;
    jsw_err(&w, "sd format", logger_clock_now_utc_or_null(&app->clock),
            "not_permitted_in_mode",
            "sd format is not permitted while a session is active");
    return;
  }

  logger_storage_status_t formatted;
  if (!logger_storage_svc_format(&formatted)) {
    (void)logger_storage_svc_refresh(&app->storage);
    jsw w;
    jsw_err(&w, "sd format", logger_clock_now_utc_or_null(&app->clock),
            "storage_unavailable",
            "failed to format and remount the SD card as FAT32");
    return;
  }

  logger_upload_queue_summary_t queue_summary;
  if (!logger_storage_svc_queue_refresh(
          &app->system_log, logger_clock_now_utc_or_null(&app->clock),
          &queue_summary)) {
    (void)logger_storage_svc_refresh(&app->storage);
    jsw w;
    jsw_err(&w, "sd format", logger_clock_now_utc_or_null(&app->clock),
            "storage_unavailable",
            "formatted SD card but failed to initialize logger queue state");
    return;
  }

  (void)logger_storage_svc_refresh(&app->storage);
  (void)queue_summary;
  (void)logger_system_log_append(
      &app->system_log, logger_clock_now_utc_or_null(&app->clock),
      "sd_formatted", LOGGER_SYSTEM_LOG_SEVERITY_WARN,
      "{\"filesystem\":\"fat32\",\"logger_root_created\":true}");

  cli->unlocked = false;

  jsw w;
  jsw_ok(&w, "sd format", logger_clock_now_utc_or_null(&app->clock));
  logger_json_stream_writer_field_bool(&w, "formatted", true);
  logger_json_stream_writer_field_string_or_null(&w, "filesystem", "fat32");
  logger_json_stream_writer_field_bool(&w, "logger_root_created", true);
  jsw_end(&w);
}

static void logger_handle_clock_set(const logger_service_cli_t *cli,
                                    logger_app_t *app, const char *value) {
  if (!logger_cli_require_service_unlocked(cli, app, "clock set")) {
    return;
  }
  if (!logger_clock_set_utc(value, &app->clock)) {
    jsw w;
    jsw_err(&w, "clock set", logger_clock_now_utc_or_null(&app->clock),
            "invalid_config", "invalid RFC3339 UTC timestamp");
    return;
  }
  logger_app_note_explicit_clock_valid(
      app, to_ms_since_boot(get_absolute_time()), "clock_set");
  (void)logger_system_log_append(
      &app->system_log, logger_clock_now_utc_or_null(&app->clock), "clock_set",
      LOGGER_SYSTEM_LOG_SEVERITY_INFO, "{}");

  jsw w;
  jsw_ok(&w, "clock set", logger_clock_now_utc_or_null(&app->clock));
  logger_json_stream_writer_field_bool(&w, "applied", true);
  logger_json_stream_writer_field_string_or_null(
      &w, "now_utc", logger_clock_now_utc_or_null(&app->clock));
  jsw_end(&w);
}

static void logger_handle_clock_sync(logger_service_cli_t *cli,
                                     logger_app_t *app) {
  if (!logger_cli_require_service_unlocked(cli, app, "clock sync")) {
    return;
  }

  logger_clock_ntp_sync_result_t result;
  const bool ok = logger_app_clock_sync_ntp(app, &result);

  jsw w;
  jsw_ok(&w, "clock sync", logger_clock_now_utc_or_null(&app->clock));
  logger_json_stream_writer_field_bool(&w, "attempted", result.attempted);
  logger_json_stream_writer_field_bool(&w, "applied", ok);
  logger_json_stream_writer_field_bool(&w, "previous_valid",
                                       result.previous_valid);
  logger_json_stream_writer_field_bool(&w, "large_correction",
                                       result.large_correction);
  logger_json_stream_writer_field_int64(&w, "correction_seconds",
                                        result.correction_seconds);
  logger_json_stream_writer_field_string_or_null(
      &w, "server", result.server[0] != '\0' ? result.server : NULL);
  logger_json_stream_writer_field_string_or_null(
      &w, "remote_address",
      result.remote_address[0] != '\0' ? result.remote_address : NULL);
  if (result.stratum != 0u) {
    logger_json_stream_writer_field_uint32(&w, "stratum", result.stratum);
  } else {
    logger_json_stream_writer_field_null(&w, "stratum");
  }
  logger_json_stream_writer_field_string_or_null(
      &w, "previous_utc",
      result.previous_utc[0] != '\0' ? result.previous_utc : NULL);
  logger_json_stream_writer_field_string_or_null(
      &w, "now_utc", logger_clock_now_utc_or_null(&app->clock));
  logger_json_stream_writer_field_string_or_null(
      &w, "message", result.message[0] != '\0' ? result.message : NULL);
  jsw_end(&w);
}

static void logger_handle_debug_synth_clock_invalid(logger_service_cli_t *cli,
                                                    logger_app_t *app,
                                                    uint32_t now_ms) {
  if (!logger_debug_require_service_unlocked(
          cli, app, "debug synth clock-invalid",
          "synthetic clock commands are only allowed in service mode")) {
    return;
  }

  logger_app_debug_force_clock_invalid(app, now_ms);

  jsw w;
  jsw_ok(&w, "debug synth clock-invalid",
         logger_clock_now_utc_or_null(&app->clock));
  logger_json_stream_writer_field_bool(&w, "forced", true);
  logger_json_stream_writer_field_bool(&w, "clock_valid", app->clock.valid);
  logger_json_stream_writer_field_string_or_null(
      &w, "fault_code",
      logger_fault_code_name(app->persisted.current_fault_code));
  jsw_end(&w);
}

static void logger_handle_debug_synth_clock_valid(logger_service_cli_t *cli,
                                                  logger_app_t *app,
                                                  uint32_t now_ms) {
  if (!logger_debug_require_service_unlocked(
          cli, app, "debug synth clock-valid",
          "synthetic clock commands are only allowed in service mode")) {
    return;
  }

  logger_app_debug_clear_forced_clock_invalid(app, now_ms, "debug_clock_valid");

  jsw w;
  jsw_ok(&w, "debug synth clock-valid",
         logger_clock_now_utc_or_null(&app->clock));
  logger_json_stream_writer_field_bool(&w, "forced", false);
  logger_json_stream_writer_field_bool(&w, "clock_valid", app->clock.valid);
  logger_json_stream_writer_field_string_or_null(
      &w, "fault_code",
      logger_fault_code_name(app->persisted.current_fault_code));
  jsw_end(&w);
}

static void logger_handle_debug_synth_storage_fault(
    logger_service_cli_t *cli, logger_app_t *app,
    logger_debug_storage_fault_t fault, const char *command, uint32_t now_ms) {
  if (!logger_debug_require_service_or_recovery_hold_unlocked(
          cli, app, command,
          "synthetic storage commands are only allowed in service mode or "
          "recovery_hold")) {
    return;
  }

  logger_app_debug_force_storage_fault(app, fault, now_ms);

  jsw w;
  jsw_ok(&w, command, logger_clock_now_utc_or_null(&app->clock));
  logger_json_stream_writer_field_string_or_null(
      &w, "forced_fault", logger_debug_storage_fault_name(fault));
  logger_json_stream_writer_field_string_or_null(
      &w, "mode", logger_mode_name(&app->runtime));
  logger_json_stream_writer_field_string_or_null(
      &w, "runtime_state",
      logger_runtime_state_name(app->runtime.current_state));
  logger_json_stream_writer_field_string_or_null(
      &w, "fault_code",
      logger_fault_code_name(app->persisted.current_fault_code));
  jsw_end(&w);
}

static void logger_handle_debug_synth_storage_valid(logger_service_cli_t *cli,
                                                    logger_app_t *app,
                                                    uint32_t now_ms) {
  if (!logger_debug_require_service_or_recovery_hold_unlocked(
          cli, app, "debug synth storage-valid",
          "synthetic storage commands are only allowed in service mode or "
          "recovery_hold")) {
    return;
  }

  logger_app_debug_clear_forced_storage_fault(app, now_ms);

  jsw w;
  jsw_ok(&w, "debug synth storage-valid",
         logger_clock_now_utc_or_null(&app->clock));
  logger_json_stream_writer_field_null(&w, "forced_fault");
  logger_json_stream_writer_field_string_or_null(
      &w, "mode", logger_mode_name(&app->runtime));
  logger_json_stream_writer_field_string_or_null(
      &w, "runtime_state",
      logger_runtime_state_name(app->runtime.current_state));
  logger_json_stream_writer_field_string_or_null(
      &w, "fault_code",
      logger_fault_code_name(app->persisted.current_fault_code));
  jsw_end(&w);
}

static void __attribute__((noinline))
logger_service_cli_append_config_changed_event(logger_app_t *app,
                                               bool bond_cleared) {
  logger_json_object_writer_t writer;
  logger_json_object_writer_init(&writer, g_service_cli_details_json,
                                 sizeof(g_service_cli_details_json));
  if (logger_json_object_writer_string_field(&writer, "source",
                                             "config_import") &&
      logger_json_object_writer_bool_field(&writer, "bond_cleared",
                                           bond_cleared) &&
      logger_json_object_writer_finish(&writer)) {
    (void)logger_system_log_append(
        &app->system_log, logger_clock_now_utc_or_null(&app->clock),
        "config_changed", LOGGER_SYSTEM_LOG_SEVERITY_INFO,
        logger_json_object_writer_data(&writer));
  }
}

static void __attribute__((noinline)) logger_service_cli_write_config_import_ok(
    const logger_app_t *app, const char *command, bool bond_cleared) {
  jsw w;
  jsw_ok(&w, command, logger_clock_now_utc_or_null(&app->clock));
  logger_json_stream_writer_field_bool(&w, "applied", true);
  logger_json_stream_writer_field_bool(
      &w, "normal_logging_ready",
      logger_config_normal_logging_ready(&app->persisted.config));
  logger_json_stream_writer_field_bool(&w, "bond_cleared", bond_cleared);
  jsw_end(&w);
}

static void logger_apply_config_import_json(logger_service_cli_t *cli,
                                            logger_app_t *app,
                                            const char *command,
                                            const char *json,
                                            bool clear_transfer_on_success) {
  bool bond_cleared = false;
  const char *error_message = "config import failed";
  if (!logger_parse_config_import_document(app, json,
                                           &g_service_cli_imported_state,
                                           &bond_cleared, &error_message)) {
    jsw w;
    jsw_err(&w, command, logger_clock_now_utc_or_null(&app->clock),
            "invalid_config", error_message);
    return;
  }
  if (!logger_config_store_save(&g_service_cli_imported_state)) {
    jsw w;
    jsw_err(&w, command, logger_clock_now_utc_or_null(&app->clock),
            "storage_unavailable", "failed to persist imported config");
    return;
  }

  app->persisted = g_service_cli_imported_state;
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

  logger_service_cli_append_config_changed_event(app, bond_cleared);

  if (clear_transfer_on_success) {
    logger_config_import_transfer_reset(cli);
  }
  cli->unlocked = false;

  logger_service_cli_write_config_import_ok(app, command, bond_cleared);
}

static void logger_handle_config_import(logger_service_cli_t *cli,
                                        logger_app_t *app, const char *json) {
  if (!logger_require_config_import_context(cli, app, "config import", true)) {
    return;
  }
  if (!logger_string_present(json)) {
    jsw w;
    jsw_err(&w, "config import", logger_clock_now_utc_or_null(&app->clock),
            "invalid_config",
            "expected: config import <json>\nor use: config "
            "import begin <bytes> / chunk / commit");
    return;
  }
  logger_apply_config_import_json(cli, app, "config import", json, true);
}

static void logger_handle_config_import_begin(logger_service_cli_t *cli,
                                              const logger_app_t *app,
                                              const char *args) {
  if (!logger_require_config_import_context(cli, app, "config import begin",
                                            true)) {
    return;
  }

  char size_text[24] = {0};
  char extra[8] = {0};
  const int matched = sscanf(args, "%23s %7s", size_text, extra);
  if (matched != 1) {
    jsw w;
    jsw_err(&w, "config import begin",
            logger_clock_now_utc_or_null(&app->clock), "invalid_config",
            "expected: config import begin <total_bytes>");
    return;
  }

  size_t expected_len = 0u;
  if (!logger_parse_size_t_strict(size_text, &expected_len) ||
      expected_len == 0u) {
    jsw w;
    jsw_err(&w, "config import begin",
            logger_clock_now_utc_or_null(&app->clock), "invalid_config",
            "config import begin requires a positive byte count");
    return;
  }
  if (expected_len > LOGGER_SERVICE_CLI_CONFIG_IMPORT_JSON_MAX) {
    jsw w;
    jsw_err(&w, "config import begin",
            logger_clock_now_utc_or_null(&app->clock), "invalid_config",
            "config import size exceeds transport buffer");
    return;
  }

  const bool replaced_existing = cli->config_import_active;
  logger_config_import_transfer_reset(cli);
  cli->config_import_active = true;
  cli->config_import_expected_len = expected_len;

  jsw w;
  jsw_ok(&w, "config import begin", logger_clock_now_utc_or_null(&app->clock));
  logger_json_stream_writer_field_bool(&w, "started", true);
  logger_json_stream_writer_field_uint64(&w, "expected_bytes",
                                         cli->config_import_expected_len);
  logger_json_stream_writer_field_uint64(&w, "received_bytes", 0u);
  logger_json_stream_writer_field_uint64(&w, "remaining_bytes",
                                         cli->config_import_expected_len);
  logger_json_stream_writer_field_uint32(
      &w, "max_bytes", LOGGER_SERVICE_CLI_CONFIG_IMPORT_JSON_MAX);
  logger_json_stream_writer_field_bool(&w, "replaced_existing",
                                       replaced_existing);
  jsw_end(&w);
}

static void logger_handle_config_import_chunk(logger_service_cli_t *cli,
                                              const logger_app_t *app,
                                              const char *chunk) {
  if (!logger_require_config_import_context(cli, app, "config import chunk",
                                            true)) {
    return;
  }
  if (!cli->config_import_active) {
    jsw w;
    jsw_err(
        &w, "config import chunk", logger_clock_now_utc_or_null(&app->clock),
        "invalid_config",
        "no config import transfer is active; use config import begin first");
    return;
  }

  const size_t chunk_len = strlen(chunk);
  if (chunk_len == 0u) {
    jsw w;
    jsw_err(&w, "config import chunk",
            logger_clock_now_utc_or_null(&app->clock), "invalid_config",
            "config import chunk requires a non-empty payload fragment");
    return;
  }

  if ((cli->config_import_received_len + chunk_len) >
          cli->config_import_expected_len ||
      (cli->config_import_received_len + chunk_len) >
          LOGGER_SERVICE_CLI_CONFIG_IMPORT_JSON_MAX) {
    logger_config_import_transfer_reset(cli);
    jsw w;
    jsw_err(&w, "config import chunk",
            logger_clock_now_utc_or_null(&app->clock), "invalid_config",
            "config import chunk exceeds announced transfer "
            "size; transfer aborted");
    return;
  }

  memcpy(cli->config_import_buf + cli->config_import_received_len, chunk,
         chunk_len);
  cli->config_import_received_len += chunk_len;
  cli->config_import_buf[cli->config_import_received_len] = '\0';
  cli->config_import_chunk_count += 1u;

  jsw w;
  jsw_ok(&w, "config import chunk", logger_clock_now_utc_or_null(&app->clock));
  logger_json_stream_writer_field_bool(&w, "accepted", true);
  logger_json_stream_writer_field_uint64(&w, "chunk_bytes",
                                         (uint64_t)chunk_len);
  logger_json_stream_writer_field_uint64(
      &w, "received_bytes", (uint64_t)cli->config_import_received_len);
  logger_json_stream_writer_field_uint64(
      &w, "expected_bytes", (uint64_t)cli->config_import_expected_len);
  logger_json_stream_writer_field_uint64(
      &w, "remaining_bytes",
      (uint64_t)(cli->config_import_expected_len -
                 cli->config_import_received_len));
  logger_json_stream_writer_field_uint32(
      &w, "chunk_count", (uint32_t)cli->config_import_chunk_count);
  logger_json_stream_writer_field_bool(&w, "complete",
                                       cli->config_import_received_len ==
                                           cli->config_import_expected_len);
  jsw_end(&w);
}

static void logger_handle_config_import_status(logger_service_cli_t *cli,
                                               const logger_app_t *app) {
  if (!logger_require_config_import_context(cli, app, "config import status",
                                            false)) {
    return;
  }

  jsw w;
  jsw_ok(&w, "config import status", logger_clock_now_utc_or_null(&app->clock));
  logger_json_stream_writer_field_bool(&w, "active", cli->config_import_active);
  logger_json_stream_writer_field_uint64(
      &w, "expected_bytes", (uint64_t)cli->config_import_expected_len);
  logger_json_stream_writer_field_uint64(
      &w, "received_bytes", (uint64_t)cli->config_import_received_len);
  if (cli->config_import_received_len <= cli->config_import_expected_len) {
    logger_json_stream_writer_field_uint64(
        &w, "remaining_bytes",
        (uint64_t)(cli->config_import_expected_len -
                   cli->config_import_received_len));
  } else {
    logger_json_stream_writer_field_uint64(&w, "remaining_bytes", 0u);
  }
  logger_json_stream_writer_field_uint32(
      &w, "chunk_count", (uint32_t)cli->config_import_chunk_count);
  logger_json_stream_writer_field_uint32(
      &w, "max_bytes", LOGGER_SERVICE_CLI_CONFIG_IMPORT_JSON_MAX);
  logger_json_stream_writer_field_bool(&w, "ready_to_commit",
                                       cli->config_import_active &&
                                           cli->config_import_received_len ==
                                               cli->config_import_expected_len);
  jsw_end(&w);
}

static void logger_handle_config_import_cancel(logger_service_cli_t *cli,
                                               const logger_app_t *app) {
  if (!logger_require_config_import_context(cli, app, "config import cancel",
                                            false)) {
    return;
  }

  const bool had_transfer = cli->config_import_active ||
                            cli->config_import_received_len != 0u ||
                            cli->config_import_expected_len != 0u ||
                            cli->config_import_chunk_count != 0u;
  logger_config_import_transfer_reset(cli);

  jsw w;
  jsw_ok(&w, "config import cancel", logger_clock_now_utc_or_null(&app->clock));
  logger_json_stream_writer_field_bool(&w, "cleared", true);
  logger_json_stream_writer_field_bool(&w, "had_transfer", had_transfer);
  jsw_end(&w);
}

static void logger_handle_config_import_commit(logger_service_cli_t *cli,
                                               logger_app_t *app) {
  if (!logger_require_config_import_context(cli, app, "config import commit",
                                            true)) {
    return;
  }
  if (!cli->config_import_active) {
    jsw w;
    jsw_err(
        &w, "config import commit", logger_clock_now_utc_or_null(&app->clock),
        "invalid_config",
        "no config import transfer is active; use config import begin first");
    return;
  }
  if (cli->config_import_received_len != cli->config_import_expected_len) {
    jsw w;
    jsw_err(&w, "config import commit",
            logger_clock_now_utc_or_null(&app->clock), "invalid_config",
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
  if (!logger_cli_require_service_unlocked(
          cli, app, "upload tls clear-provisioned-anchor")) {
    return;
  }

  bool had_anchor = false;
  if (!logger_config_clear_provisioned_anchor(&app->persisted, &had_anchor)) {
    jsw w;
    jsw_err(&w, "upload tls clear-provisioned-anchor",
            logger_clock_now_utc_or_null(&app->clock), "storage_unavailable",
            "failed to clear provisioned upload TLS anchor");
    return;
  }

  logger_json_object_writer_t writer;
  logger_json_object_writer_init(&writer, g_service_cli_details_json,
                                 sizeof(g_service_cli_details_json));
  if (logger_json_object_writer_string_field(
          &writer, "source", "upload_tls_clear_provisioned_anchor") &&
      logger_json_object_writer_bool_field(&writer, "had_anchor", had_anchor) &&
      logger_json_object_writer_finish(&writer)) {
    (void)logger_system_log_append(
        &app->system_log, logger_clock_now_utc_or_null(&app->clock),
        "config_changed", LOGGER_SYSTEM_LOG_SEVERITY_INFO,
        logger_json_object_writer_data(&writer));
  }

  cli->unlocked = false;

  jsw w;
  jsw_ok(&w, "upload tls clear-provisioned-anchor",
         logger_clock_now_utc_or_null(&app->clock));
  logger_json_stream_writer_field_bool(&w, "cleared", true);
  logger_json_stream_writer_field_bool(&w, "had_anchor", had_anchor);
  logger_json_stream_writer_field_string_or_null(
      &w, "current_tls_mode",
      logger_config_upload_tls_mode(&app->persisted.config));
  logger_json_stream_writer_field_bool(
      &w, "upload_ready", logger_config_upload_ready(&app->persisted.config));
  jsw_end(&w);
}

static bool logger_parse_debug_config_set_args(const char *args, char *field,
                                               size_t field_len,
                                               const char **value_out) {
  if (args == NULL || field == NULL || field_len == 0u || value_out == NULL) {
    return false;
  }

  const char *p = args;
  while (*p != '\0' && isspace((unsigned char)*p)) {
    ++p;
  }
  const char *field_start = p;
  while (*p != '\0' && !isspace((unsigned char)*p)) {
    ++p;
  }
  const size_t parsed_field_len = (size_t)(p - field_start);
  while (*p != '\0' && isspace((unsigned char)*p)) {
    ++p;
  }
  if (parsed_field_len == 0u || parsed_field_len >= field_len || *p == '\0') {
    field[0] = '\0';
    *value_out = NULL;
    return false;
  }

  memcpy(field, field_start, parsed_field_len);
  field[parsed_field_len] = '\0';
  *value_out = p;
  return true;
}

static void logger_handle_debug_config_set(const logger_service_cli_t *cli,
                                           logger_app_t *app,
                                           const char *args) {
  char field[48];
  const char *value = NULL;
  field[0] = '\0';
  if (!logger_parse_debug_config_set_args(args, field, sizeof(field), &value)) {
    jsw w;
    jsw_err(&w, "debug config set", logger_clock_now_utc_or_null(&app->clock),
            "invalid_config", "expected: debug config set <field> <value>");
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
    jsw w;
    jsw_err(&w, "debug config set", logger_clock_now_utc_or_null(&app->clock),
            "busy_logging", "config mutation is not permitted while logging");
    return;
  }
  if (!logger_cli_is_service_mode(app) && !allow_in_log_wait_h10) {
    jsw w;
    jsw_err(&w, "debug config set", logger_clock_now_utc_or_null(&app->clock),
            "not_permitted_in_mode",
            "debug config set is only allowed in service mode");
    return;
  }
  if (logger_cli_is_service_mode(app) &&
      !logger_service_cli_is_unlocked(cli,
                                      to_ms_since_boot(get_absolute_time()))) {
    jsw w;
    jsw_err(&w, "debug config set", logger_clock_now_utc_or_null(&app->clock),
            "service_locked",
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
    jsw w;
    jsw_err(&w, "debug config set", logger_clock_now_utc_or_null(&app->clock),
            "invalid_config", "unknown debug config field");
    return;
  }

  if (!ok) {
    jsw w;
    jsw_err(&w, "debug config set", logger_clock_now_utc_or_null(&app->clock),
            "invalid_config", "failed to apply debug config field");
    return;
  }

  logger_json_object_writer_t writer;
  logger_json_object_writer_init(&writer, g_service_cli_details_json,
                                 sizeof(g_service_cli_details_json));
  if (logger_json_object_writer_string_field(&writer, "field", field) &&
      logger_json_object_writer_finish(&writer)) {
    (void)logger_system_log_append(
        &app->system_log, logger_clock_now_utc_or_null(&app->clock),
        "config_changed", LOGGER_SYSTEM_LOG_SEVERITY_INFO,
        logger_json_object_writer_data(&writer));
  }

  jsw w;
  jsw_ok(&w, "debug config set", logger_clock_now_utc_or_null(&app->clock));
  logger_json_stream_writer_field_bool(&w, "applied", true);
  logger_json_stream_writer_field_string_or_null(&w, "field", field);
  logger_json_stream_writer_field_bool(
      &w, "normal_logging_ready",
      logger_config_normal_logging_ready(&app->persisted.config));
  logger_json_stream_writer_field_bool(&w, "bond_cleared", bond_cleared);
  jsw_end(&w);
}

static void logger_handle_debug_config_clear(const logger_service_cli_t *cli,
                                             logger_app_t *app,
                                             const char *args) {
  if (!logger_debug_require_service_unlocked(
          cli, app, "debug config clear",
          "debug config clear is only allowed in service mode")) {
    return;
  }
  if (strcmp(args, "upload") != 0) {
    jsw w;
    jsw_err(&w, "debug config clear", logger_clock_now_utc_or_null(&app->clock),
            "invalid_config", "only 'debug config clear upload' is supported");
    return;
  }

  (void)logger_config_clear_upload(&app->persisted);
  (void)logger_system_log_append(
      &app->system_log, logger_clock_now_utc_or_null(&app->clock),
      "config_changed", LOGGER_SYSTEM_LOG_SEVERITY_INFO,
      "{\"field\":\"upload\"}");
  jsw w;
  jsw_ok(&w, "debug config clear", logger_clock_now_utc_or_null(&app->clock));
  logger_json_stream_writer_field_bool(&w, "applied", true);
  logger_json_stream_writer_field_string_or_null(&w, "field", "upload");
  jsw_end(&w);
}

static void logger_handle_debug_session_start(const logger_service_cli_t *cli,
                                              logger_app_t *app,
                                              uint32_t now_ms) {
  if (!logger_debug_require_service_unlocked(
          cli, app, "debug session start",
          "debug session start is only allowed in service mode")) {
    return;
  }

  const char *error_code = "internal_error";
  const char *error_message = "debug session start failed";
  if (!logger_session_start_debug(
          &app->session, &app->system_log, app->hardware_id, &app->persisted,
          &app->clock, &app->battery, &app->storage,
          app->persisted.current_fault_code, app->persisted.boot_counter,
          now_ms, &error_code, &error_message)) {
    jsw w;
    jsw_err(&w, "debug session start",
            logger_clock_now_utc_or_null(&app->clock), error_code,
            error_message);
    return;
  }
  app->last_session_snapshot_mono_ms = now_ms;

  jsw w;
  jsw_ok(&w, "debug session start", logger_clock_now_utc_or_null(&app->clock));
  logger_json_stream_writer_field_bool(&w, "active", true);
  logger_json_stream_writer_field_string_or_null(&w, "session_id",
                                                 app->session.session_id);
  logger_json_stream_writer_field_string_or_null(&w, "study_day_local",
                                                 app->session.study_day_local);
  jsw_end(&w);
}

static void logger_handle_debug_session_snapshot(logger_app_t *app,
                                                 uint32_t now_ms) {
  if (!app->session.active) {
    jsw w;
    jsw_err(&w, "debug session snapshot",
            logger_clock_now_utc_or_null(&app->clock), "not_permitted_in_mode",
            "no debug session is active");
    return;
  }
  if (!logger_session_write_status_snapshot(
          &app->session, &app->clock, &app->battery, &app->storage,
          app->persisted.current_fault_code, app->persisted.boot_counter,
          now_ms)) {
    jsw w;
    jsw_err(&w, "debug session snapshot",
            logger_clock_now_utc_or_null(&app->clock), "storage_unavailable",
            "failed to append status snapshot");
    return;
  }
  app->last_session_snapshot_mono_ms = now_ms;

  jsw w;
  jsw_ok(&w, "debug session snapshot",
         logger_clock_now_utc_or_null(&app->clock));
  logger_json_stream_writer_field_bool(&w, "written", true);
  logger_json_stream_writer_field_uint64(
      &w, "journal_size_bytes",
      logger_session_writer_journal_size_approx(&app->session));
  jsw_end(&w);
}

static void logger_handle_debug_session_stop(logger_app_t *app,
                                             uint32_t now_ms) {
  if (!app->session.active) {
    jsw w;
    jsw_ok(&w, "debug session stop", logger_clock_now_utc_or_null(&app->clock));
    logger_json_stream_writer_field_bool(&w, "active", false);
    jsw_end(&w);
    return;
  }
  if (!logger_session_stop_debug(&app->session, &app->system_log,
                                 &app->persisted, &app->clock,
                                 app->persisted.boot_counter, now_ms)) {
    jsw w;
    jsw_err(&w, "debug session stop", logger_clock_now_utc_or_null(&app->clock),
            "storage_unavailable", "failed to close debug session");
    return;
  }
  jsw w;
  jsw_ok(&w, "debug session stop", logger_clock_now_utc_or_null(&app->clock));
  logger_json_stream_writer_field_bool(&w, "active", false);
  jsw_end(&w);
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
    jsw w;
    jsw_err(&w, "debug synth ecg", logger_clock_now_utc_or_null(&app->clock),
            "not_permitted_in_mode", "no debug session is active");
    return;
  }

  uint8_t count = 1u;
  if (args != NULL && args[0] != '\0' && !logger_parse_u8(args, &count)) {
    jsw w;
    jsw_err(&w, "debug synth ecg", logger_clock_now_utc_or_null(&app->clock),
            "invalid_config", "expected: debug synth ecg [count]");
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
      jsw w;
      jsw_err(&w, "debug synth ecg", logger_clock_now_utc_or_null(&app->clock),
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
      jsw w;
      jsw_err(&w, "debug synth ecg", logger_clock_now_utc_or_null(&app->clock),
              "storage_unavailable", "failed to append synthetic ECG packet");
      return;
    }
    appended += 1u;
  }

  /* Flush all queued packets through core 1 with a barrier.
   * capture_pipe_submit_cmd() drains staging, enqueues the
   * barrier, and waits for core 1 to process everything —
   * no magic sleep needed. */
  logger_writer_cmd_t flush_cmd;
  memset(&flush_cmd, 0, sizeof(flush_cmd));
  flush_cmd.flush_barrier.type = LOGGER_WRITER_FLUSH_BARRIER;
  flush_cmd.flush_barrier.boot_counter = app->persisted.boot_counter;
  flush_cmd.flush_barrier.now_ms = now_ms;
  flush_cmd.flush_barrier.force = true;
  capture_pipe_submit_cmd(&app->capture_pipe,
                          (logger_session_context_t *)&app->session,
                          &flush_cmd);

  jsw w;
  jsw_ok(&w, "debug synth ecg", logger_clock_now_utc_or_null(&app->clock));
  logger_json_stream_writer_field_uint32(&w, "appended_packets", appended);
  logger_json_stream_writer_field_string_or_null(&w, "session_id",
                                                 app->session.session_id);
  logger_json_stream_writer_field_string_or_null(
      &w, "span_id",
      app->session.span_active ? app->session.current_span_id : NULL);
  logger_json_stream_writer_field_uint64(
      &w, "journal_size_bytes",
      logger_session_writer_journal_size_approx(&app->session));
  jsw_end(&w);
}

static void
logger_handle_debug_synth_disconnect(const logger_service_cli_t *cli,
                                     logger_app_t *app, uint32_t now_ms) {
  if (!logger_debug_require_service_unlocked(
          cli, app, "debug synth disconnect",
          "debug synth disconnect is only allowed in service mode")) {
    return;
  }
  if (!app->session.active || !app->session.span_active) {
    jsw w;
    jsw_err(&w, "debug synth disconnect",
            logger_clock_now_utc_or_null(&app->clock), "not_permitted_in_mode",
            "no active span is open");
    return;
  }
  if (!logger_session_handle_disconnect(&app->session, &app->clock,
                                        app->persisted.boot_counter, now_ms,
                                        "disconnect")) {
    jsw w;
    jsw_err(&w, "debug synth disconnect",
            logger_clock_now_utc_or_null(&app->clock), "storage_unavailable",
            "failed to append synthetic disconnect gap");
    return;
  }
  jsw w;
  jsw_ok(&w, "debug synth disconnect",
         logger_clock_now_utc_or_null(&app->clock));
  logger_json_stream_writer_field_bool(&w, "span_active", false);
  logger_json_stream_writer_field_string_or_null(&w, "session_id",
                                                 app->session.session_id);
  jsw_end(&w);
}

static void logger_handle_debug_h10_inject_stale_bond(logger_service_cli_t *cli,
                                                      logger_app_t *app,
                                                      uint32_t now_ms) {
  (void)cli;
  if (!logger_cli_is_logging_mode(app) ||
      app->runtime.current_state == LOGGER_RUNTIME_LOG_STOPPING) {
    jsw w;
    jsw_err(&w, "debug h10 inject-stale-bond",
            logger_clock_now_utc_or_null(&app->clock), "not_permitted_in_mode",
            "debug h10 inject-stale-bond is only allowed while logging");
    return;
  }

  bool restart_requested = false;
  if (!logger_h10_debug_arm_stale_bond_injection(&app->h10, now_ms,
                                                 &restart_requested)) {
    jsw w;
    jsw_err(&w, "debug h10 inject-stale-bond",
            logger_clock_now_utc_or_null(&app->clock), "not_permitted_in_mode",
            "H10 stale-bond injection requires an active logging target");
    return;
  }

  jsw w;
  jsw_ok(&w, "debug h10 inject-stale-bond",
         logger_clock_now_utc_or_null(&app->clock));
  logger_json_stream_writer_field_bool(&w, "armed", true);
  logger_json_stream_writer_field_bool(&w, "restart_requested",
                                       restart_requested);
  logger_json_stream_writer_field_string_or_null(
      &w, "phase", logger_h10_phase_name(app->h10.phase));
  jsw_end(&w);
}

static void
logger_handle_debug_synth_h10_battery(const logger_service_cli_t *cli,
                                      logger_app_t *app, const char *args,
                                      uint32_t now_ms) {
  if (!logger_debug_require_service_unlocked(
          cli, app, "debug synth h10-battery",
          "debug synth h10-battery is only allowed in service mode")) {
    return;
  }
  if (!app->session.active) {
    jsw w;
    jsw_err(&w, "debug synth h10-battery",
            logger_clock_now_utc_or_null(&app->clock), "not_permitted_in_mode",
            "no debug session is active");
    return;
  }

  char percent_text[16] = {0};
  char reason[16] = {0};
  if (args == NULL || sscanf(args, "%15s %15s", percent_text, reason) != 2) {
    jsw w;
    jsw_err(&w, "debug synth h10-battery",
            logger_clock_now_utc_or_null(&app->clock), "invalid_config",
            "expected: debug synth h10-battery <percent> <connect|periodic>");
    return;
  }
  uint8_t percent = 0u;
  if (!logger_parse_u8(percent_text, &percent) || percent > 100u ||
      (strcmp(reason, "connect") != 0 && strcmp(reason, "periodic") != 0)) {
    jsw w;
    jsw_err(&w, "debug synth h10-battery",
            logger_clock_now_utc_or_null(&app->clock), "invalid_config",
            "invalid battery percent or read reason");
    return;
  }

  if (!logger_session_append_h10_battery(&app->session, &app->clock,
                                         app->persisted.boot_counter, now_ms,
                                         percent, reason)) {
    jsw w;
    jsw_err(&w, "debug synth h10-battery",
            logger_clock_now_utc_or_null(&app->clock), "storage_unavailable",
            "failed to append synthetic h10_battery record");
    return;
  }
  app->h10.battery_percent = percent;

  jsw w;
  jsw_ok(&w, "debug synth h10-battery",
         logger_clock_now_utc_or_null(&app->clock));
  logger_json_stream_writer_field_bool(&w, "written", true);
  logger_json_stream_writer_field_uint32(&w, "battery_percent",
                                         (uint32_t)percent);
  logger_json_stream_writer_field_string_or_null(&w, "read_reason", reason);
  jsw_end(&w);
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

static void
logger_handle_debug_synth_no_session_day(const logger_service_cli_t *cli,
                                         logger_app_t *app, const char *args) {
  if (!logger_debug_require_service_unlocked(
          cli, app, "debug synth no-session-day",
          "debug synth no-session-day is only allowed in service mode")) {
    return;
  }
  if (app->session.active) {
    jsw w;
    jsw_err(&w, "debug synth no-session-day",
            logger_clock_now_utc_or_null(&app->clock), "not_permitted_in_mode",
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
    jsw w;
    jsw_err(&w, "debug synth no-session-day",
            logger_clock_now_utc_or_null(&app->clock), "invalid_config",
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
        jsw w;
        jsw_err(&w, "debug synth no-session-day",
                logger_clock_now_utc_or_null(&app->clock), "invalid_config",
                "auto mode flags must be 0 or 1");
        return;
      }
    }
    reason = logger_classify_no_session_day_reason(
        seen_bound_device, ble_connected, ecg_start_attempted);
  } else if (!logger_no_session_reason_valid(reason)) {
    jsw w;
    jsw_err(&w, "debug synth no-session-day",
            logger_clock_now_utc_or_null(&app->clock), "invalid_config",
            "invalid no-session-day reason");
    return;
  }

  char study_day_local[11] = {0};
  if (!logger_clock_derive_study_day_local_observed(
          &app->clock, app->persisted.config.timezone, study_day_local)) {
    jsw w;
    jsw_err(&w, "debug synth no-session-day",
            logger_clock_now_utc_or_null(&app->clock), "invalid_config",
            "cannot derive study_day_local from current clock/timezone");
    return;
  }

  logger_json_object_writer_t writer;
  logger_json_object_writer_init(&writer, g_service_cli_details_json,
                                 sizeof(g_service_cli_details_json));
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
    jsw w;
    jsw_err(&w, "debug synth no-session-day",
            logger_clock_now_utc_or_null(&app->clock), "storage_unavailable",
            "failed to build synthetic no_session_day_summary details");
    return;
  }
  if (!logger_system_log_append(
          &app->system_log, logger_clock_now_utc_or_null(&app->clock),
          "no_session_day_summary", LOGGER_SYSTEM_LOG_SEVERITY_INFO,
          logger_json_object_writer_data(&writer))) {
    jsw w;
    jsw_err(&w, "debug synth no-session-day",
            logger_clock_now_utc_or_null(&app->clock), "storage_unavailable",
            "failed to append synthetic no_session_day_summary");
    return;
  }
  logger_app_set_last_day_outcome(app, study_day_local, "no_session", reason);

  jsw w;
  jsw_ok(&w, "debug synth no-session-day",
         logger_clock_now_utc_or_null(&app->clock));
  logger_json_stream_writer_field_string_or_null(&w, "study_day_local",
                                                 study_day_local);
  logger_json_stream_writer_field_string_or_null(&w, "reason", reason);
  logger_json_stream_writer_field_bool(&w, "seen_bound_device",
                                       seen_bound_device);
  logger_json_stream_writer_field_bool(&w, "ble_connected", ble_connected);
  logger_json_stream_writer_field_bool(&w, "ecg_start_attempted",
                                       ecg_start_attempted);
  jsw_end(&w);
}

static bool logger_update_clock_from_rfc3339(logger_app_t *app,
                                             const char *rfc3339,
                                             uint32_t now_ms,
                                             int64_t *old_utc_ns_out,
                                             int64_t *new_utc_ns_out) {
  int64_t old_utc_ns = 0ll;
  (void)logger_clock_valid_utc_ns(&app->clock, &old_utc_ns);
  if (!logger_clock_set_utc(rfc3339, &app->clock)) {
    return false;
  }
  int64_t new_utc_ns = 0ll;
  const bool have_new_utc = logger_clock_valid_utc_ns(&app->clock, &new_utc_ns);
  app->last_clock_observation_available = true;
  app->last_trusted_clock_utc_available = have_new_utc;
  app->last_clock_observation_valid = app->clock.valid;
  app->last_clock_observation_mono_ms = now_ms;
  app->last_trusted_clock_utc_ns = new_utc_ns;
  if (old_utc_ns_out != NULL) {
    *old_utc_ns_out = old_utc_ns;
  }
  if (new_utc_ns_out != NULL) {
    *new_utc_ns_out = new_utc_ns;
  }
  return true;
}

static void logger_handle_debug_synth_rollover(const logger_service_cli_t *cli,
                                               logger_app_t *app,
                                               const char *rfc3339,
                                               uint32_t now_ms) {
  if (!logger_debug_require_service_unlocked(
          cli, app, "debug synth rollover",
          "debug synth rollover is only allowed in service mode")) {
    return;
  }
  if (!app->session.active) {
    jsw w;
    jsw_err(&w, "debug synth rollover",
            logger_clock_now_utc_or_null(&app->clock), "not_permitted_in_mode",
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
    jsw w;
    jsw_err(&w, "debug synth rollover",
            logger_clock_now_utc_or_null(&app->clock), "invalid_config",
            "invalid RFC3339 UTC timestamp");
    return;
  }

  char new_study_day_local[11] = {0};
  if (!logger_clock_derive_study_day_local_observed(
          &app->clock, app->persisted.config.timezone, new_study_day_local)) {
    jsw w;
    jsw_err(&w, "debug synth rollover",
            logger_clock_now_utc_or_null(&app->clock), "invalid_config",
            "cannot derive study_day_local from current clock/timezone");
    return;
  }
  if (strcmp(old_study_day_local, new_study_day_local) == 0) {
    jsw w;
    jsw_err(&w, "debug synth rollover",
            logger_clock_now_utc_or_null(&app->clock), "invalid_config",
            "new timestamp does not cross the study-day boundary");
    return;
  }

  if (!logger_session_finalize(&app->session, &app->system_log, &app->persisted,
                               &app->clock, "rollover",
                               app->persisted.boot_counter, now_ms)) {
    jsw w;
    jsw_err(&w, "debug synth rollover",
            logger_clock_now_utc_or_null(&app->clock), "storage_unavailable",
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
    jsw w;
    jsw_err(&w, "debug synth rollover",
            logger_clock_now_utc_or_null(&app->clock),
            error_code != NULL ? error_code : "storage_unavailable",
            error_message != NULL ? error_message
                                  : "failed to open post-rollover session");
    return;
  }
  logger_app_set_last_day_outcome(app, old_study_day_local, "session",
                                  "session_closed");
  app->last_session_live_flush_mono_ms = now_ms;
  app->last_session_snapshot_mono_ms = now_ms;

  jsw w;
  jsw_ok(&w, "debug synth rollover", logger_clock_now_utc_or_null(&app->clock));
  logger_json_stream_writer_field_string_or_null(&w, "old_study_day_local",
                                                 old_study_day_local);
  logger_json_stream_writer_field_string_or_null(&w, "new_study_day_local",
                                                 new_study_day_local);
  logger_json_stream_writer_field_string_or_null(&w, "old_session_id",
                                                 old_session_id);
  logger_json_stream_writer_field_string_or_null(&w, "new_session_id",
                                                 app->session.session_id);
  jsw_end(&w);
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
    jsw w;
    jsw_err(&w, command, logger_clock_now_utc_or_null(&app->clock),
            "not_permitted_in_mode", "no debug session is active");
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
    jsw w;
    jsw_err(&w, command, logger_clock_now_utc_or_null(&app->clock),
            "invalid_config", "invalid RFC3339 UTC timestamp");
    return;
  }

  char new_study_day_local[11] = {0};
  if (!logger_clock_derive_study_day_local_observed(
          &app->clock, app->persisted.config.timezone, new_study_day_local)) {
    jsw w;
    jsw_err(&w, command, logger_clock_now_utc_or_null(&app->clock),
            "invalid_config",
            "cannot derive study_day_local from current clock/timezone");
    return;
  }
  const bool crossed_study_day =
      strcmp(old_study_day_local, new_study_day_local) != 0;

  if (!logger_session_handle_clock_event(
          &app->session, &app->clock, app->persisted.boot_counter, now_ms,
          event_kind, span_end_reason, new_utc_ns - old_utc_ns, old_utc_ns,
          new_utc_ns, true)) {
    jsw w;
    jsw_err(&w, command, logger_clock_now_utc_or_null(&app->clock),
            "storage_unavailable",
            "failed to append synthetic clock boundary records");
    return;
  }

  if (crossed_study_day) {
    if (!logger_session_finalize(&app->session, &app->system_log,
                                 &app->persisted, &app->clock, span_end_reason,
                                 app->persisted.boot_counter, now_ms)) {
      jsw w;
      jsw_err(&w, command, logger_clock_now_utc_or_null(&app->clock),
              "storage_unavailable", "failed to finalize pre-boundary session");
      return;
    }
    logger_app_set_last_day_outcome(app, old_study_day_local, "session",
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
    jsw w;
    jsw_err(&w, command, logger_clock_now_utc_or_null(&app->clock),
            error_code != NULL ? error_code : "storage_unavailable",
            error_message != NULL
                ? error_message
                : "failed to open post-boundary span/session");
    return;
  }

  app->last_session_live_flush_mono_ms = now_ms;
  app->last_session_snapshot_mono_ms = now_ms;

  jsw w;
  jsw_ok(&w, command, logger_clock_now_utc_or_null(&app->clock));
  logger_json_stream_writer_field_bool(&w, "crossed_study_day",
                                       crossed_study_day);
  logger_json_stream_writer_field_string_or_null(&w, "old_study_day_local",
                                                 old_study_day_local);
  logger_json_stream_writer_field_string_or_null(&w, "new_study_day_local",
                                                 new_study_day_local);
  logger_json_stream_writer_field_string_or_null(&w, "old_session_id",
                                                 old_session_id);
  logger_json_stream_writer_field_string_or_null(&w, "new_session_id",
                                                 app->session.session_id);
  logger_json_stream_writer_field_string_or_null(
      &w, "current_span_id",
      app->session.span_active ? app->session.current_span_id : NULL);
  jsw_end(&w);
}

static void logger_handle_debug_queue_rebuild(logger_service_cli_t *cli,
                                              logger_app_t *app) {
  const bool allowed_in_wait_h10 =
      app->runtime.current_state == LOGGER_RUNTIME_LOG_WAIT_H10;
  if (!logger_cli_is_service_mode(app) && !allowed_in_wait_h10) {
    jsw w;
    jsw_err(
        &w, "debug queue rebuild", logger_clock_now_utc_or_null(&app->clock),
        "not_permitted_in_mode",
        "debug queue rebuild is only allowed in service mode or log_wait_h10");
    return;
  }
  if (app->session.active) {
    jsw w;
    jsw_err(&w, "debug queue rebuild",
            logger_clock_now_utc_or_null(&app->clock), "not_permitted_in_mode",
            "debug queue rebuild is not permitted while a session is active");
    return;
  }
  if (logger_cli_is_service_mode(app) &&
      !logger_service_cli_is_unlocked(cli,
                                      to_ms_since_boot(get_absolute_time()))) {
    jsw w;
    jsw_err(&w, "debug queue rebuild",
            logger_clock_now_utc_or_null(&app->clock), "service_locked",
            "service unlock is required before debug queue rebuild");
    return;
  }

  logger_upload_queue_summary_t summary;
  if (!logger_storage_svc_queue_rebuild(
          &app->system_log, logger_clock_now_utc_or_null(&app->clock),
          &summary)) {
    jsw w;
    jsw_err(&w, "debug queue rebuild",
            logger_clock_now_utc_or_null(&app->clock), "storage_unavailable",
            "failed to rebuild upload queue store from local sessions");
    return;
  }

  jsw w;
  jsw_ok(&w, "debug queue rebuild", logger_clock_now_utc_or_null(&app->clock));
  logger_json_stream_writer_field_bool(&w, "rebuilt", true);
  logger_json_stream_writer_field_string_or_null(
      &w, "updated_at_utc",
      summary.updated_at_utc[0] != '\0' ? summary.updated_at_utc : NULL);
  logger_json_stream_writer_field_uint32(&w, "session_count",
                                         (uint32_t)summary.session_count);
  logger_json_stream_writer_field_uint32(&w, "pending_count",
                                         (uint32_t)summary.pending_count);
  logger_json_stream_writer_field_uint32(&w, "blocked_count",
                                         (uint32_t)summary.blocked_count);
  jsw_end(&w);
}

static void logger_handle_debug_queue_requeue_blocked(logger_service_cli_t *cli,
                                                      logger_app_t *app) {
  const bool allowed_in_wait_h10 =
      app->runtime.current_state == LOGGER_RUNTIME_LOG_WAIT_H10;
  if (!logger_cli_is_service_mode(app) && !allowed_in_wait_h10) {
    jsw w;
    jsw_err(&w, "debug queue requeue-blocked",
            logger_clock_now_utc_or_null(&app->clock), "not_permitted_in_mode",
            "debug queue requeue-blocked is only allowed in "
            "service mode or log_wait_h10");
    return;
  }
  if (app->session.active) {
    jsw w;
    jsw_err(&w, "debug queue requeue-blocked",
            logger_clock_now_utc_or_null(&app->clock), "not_permitted_in_mode",
            "debug queue requeue-blocked is not permitted "
            "while a session is active");
    return;
  }
  if (logger_cli_is_service_mode(app) &&
      !logger_service_cli_is_unlocked(cli,
                                      to_ms_since_boot(get_absolute_time()))) {
    jsw w;
    jsw_err(&w, "debug queue requeue-blocked",
            logger_clock_now_utc_or_null(&app->clock), "service_locked",
            "service unlock is required before debug queue requeue-blocked");
    return;
  }

  size_t requeued_count = 0u;
  logger_upload_queue_summary_t summary;
  if (!logger_storage_svc_queue_requeue_blocked(
          &app->system_log, logger_clock_now_utc_or_null(&app->clock),
          "manual_requeue_blocked", &requeued_count, &summary)) {
    jsw w;
    jsw_err(&w, "debug queue requeue-blocked",
            logger_clock_now_utc_or_null(&app->clock), "storage_unavailable",
            "failed to rewrite blocked upload queue entries as pending");
    return;
  }

  jsw w;
  jsw_ok(&w, "debug queue requeue-blocked",
         logger_clock_now_utc_or_null(&app->clock));
  logger_json_stream_writer_field_uint32(&w, "requeued_count",
                                         (uint32_t)requeued_count);
  logger_json_stream_writer_field_string_or_null(
      &w, "updated_at_utc",
      summary.updated_at_utc[0] != '\0' ? summary.updated_at_utc : NULL);
  logger_json_stream_writer_field_uint32(&w, "session_count",
                                         (uint32_t)summary.session_count);
  logger_json_stream_writer_field_uint32(&w, "pending_count",
                                         (uint32_t)summary.pending_count);
  logger_json_stream_writer_field_uint32(&w, "blocked_count",
                                         (uint32_t)summary.blocked_count);
  jsw_end(&w);
}

static bool logger_manual_receipt_id_valid(const char *receipt_id) {
  if (!logger_string_present(receipt_id)) {
    return false;
  }

  for (const unsigned char *p = (const unsigned char *)receipt_id; *p != '\0';
       ++p) {
    if (*p < 0x21u || *p == 0x7fu || isspace(*p)) {
      return false;
    }
  }
  return true;
}

static void logger_queue_entry_mark_verified_manual(
    logger_upload_queue_entry_t *entry, const char *receipt_id,
    const char *verified_sha256, const char *verified_utc) {
  logger_copy_string(entry->status, sizeof(entry->status), "verified");
  logger_copy_string(entry->receipt_id, sizeof(entry->receipt_id), receipt_id);
  logger_copy_string(entry->verified_bundle_sha256,
                     sizeof(entry->verified_bundle_sha256), verified_sha256);
  logger_copy_string(entry->verified_upload_utc,
                     sizeof(entry->verified_upload_utc), verified_utc);
  entry->last_failure_class[0] = '\0';
  entry->last_http_status = 0u;
  entry->last_server_error_code[0] = '\0';
  entry->last_server_error_message[0] = '\0';
  entry->last_response_excerpt[0] = '\0';
}

static void logger_handle_debug_queue_mark_verified(logger_service_cli_t *cli,
                                                    logger_app_t *app,
                                                    const char *args) {
  const char *command = "debug queue mark-verified";
  if (!logger_cli_require_service_unlocked(cli, app, command)) {
    return;
  }

  char session_id[LOGGER_SESSION_ID_HEX_LEN + 1];
  char receipt_id[LOGGER_UPLOAD_QUEUE_RECEIPT_ID_MAX + 1];
  char uploaded_sha256[LOGGER_UPLOAD_QUEUE_SHA256_HEX_LEN + 1];
  memset(session_id, 0, sizeof(session_id));
  memset(receipt_id, 0, sizeof(receipt_id));
  memset(uploaded_sha256, 0, sizeof(uploaded_sha256));

  const char *space = args != NULL ? strchr(args, ' ') : NULL;
  const char *sha_space = space != NULL ? strchr(space + 1u, ' ') : NULL;
  if (space == NULL || sha_space == NULL ||
      (size_t)(space - args) != LOGGER_SESSION_ID_HEX_LEN ||
      (size_t)(sha_space - (space + 1u)) == 0u ||
      (size_t)(sha_space - (space + 1u)) > LOGGER_UPLOAD_QUEUE_RECEIPT_ID_MAX ||
      strlen(sha_space + 1u) != LOGGER_UPLOAD_QUEUE_SHA256_HEX_LEN) {
    jsw w;
    jsw_err(&w, command, logger_clock_now_utc_or_null(&app->clock),
            "invalid_argument",
            "expected: debug queue mark-verified <session_id> <receipt_id> "
            "<uploaded_sha256>");
    return;
  }
  memcpy(session_id, args, LOGGER_SESSION_ID_HEX_LEN);
  memcpy(receipt_id, space + 1u, (size_t)(sha_space - (space + 1u)));
  receipt_id[sha_space - (space + 1u)] = '\0';
  logger_copy_string(uploaded_sha256, sizeof(uploaded_sha256), sha_space + 1u);

  if (!logger_manual_receipt_id_valid(receipt_id)) {
    jsw w;
    jsw_err(&w, command, logger_clock_now_utc_or_null(&app->clock),
            "invalid_argument",
            "receipt_id must be non-empty printable text without whitespace");
    return;
  }

  uint8_t session_id_bytes[16];
  if (!logger_hex_to_bytes_16(session_id, session_id_bytes)) {
    jsw w;
    jsw_err(&w, command, logger_clock_now_utc_or_null(&app->clock),
            "invalid_argument", "session_id must be 32 hexadecimal chars");
    return;
  }
  for (size_t i = 0u; i < LOGGER_SESSION_ID_HEX_LEN; ++i) {
    session_id[i] = (char)tolower((unsigned char)session_id[i]);
  }
  uint8_t uploaded_sha_prefix[16];
  uint8_t uploaded_sha_suffix[16];
  if (!logger_hex_to_bytes_16(uploaded_sha256, uploaded_sha_prefix) ||
      !logger_hex_to_bytes_16(uploaded_sha256 + 32u, uploaded_sha_suffix)) {
    jsw w;
    jsw_err(&w, command, logger_clock_now_utc_or_null(&app->clock),
            "invalid_argument", "uploaded_sha256 must be 64 hexadecimal chars");
    return;
  }
  for (size_t i = 0u; i < LOGGER_UPLOAD_QUEUE_SHA256_HEX_LEN; ++i) {
    uploaded_sha256[i] = (char)tolower((unsigned char)uploaded_sha256[i]);
  }

  logger_upload_queue_t *queue = logger_upload_queue_tmp_acquire();
  if (!logger_storage_svc_queue_load(queue)) {
    logger_upload_queue_tmp_release(queue);
    jsw w;
    jsw_err(&w, command, logger_clock_now_utc_or_null(&app->clock),
            "storage_unavailable", "failed to load upload queue store");
    return;
  }

  logger_upload_queue_entry_t *entry = NULL;
  for (size_t i = 0u; i < queue->session_count; ++i) {
    if (strcmp(queue->sessions[i].session_id, session_id) == 0) {
      entry = &queue->sessions[i];
      break;
    }
  }
  if (entry == NULL) {
    logger_upload_queue_tmp_release(queue);
    jsw w;
    jsw_err(&w, command, logger_clock_now_utc_or_null(&app->clock), "not_found",
            "session_id is not present in upload queue");
    return;
  }

  if (strcmp(entry->status, "verified") == 0 &&
      (strcmp(entry->receipt_id, receipt_id) != 0 ||
       strcmp(entry->verified_bundle_sha256, uploaded_sha256) != 0)) {
    logger_upload_queue_tmp_release(queue);
    jsw w;
    jsw_err(&w, command, logger_clock_now_utc_or_null(&app->clock),
            "already_verified",
            "session is already verified with a different receipt or hash");
    return;
  }
  if (strcmp(entry->status, "verified") == 0) {
    logger_upload_queue_summary_t summary;
    logger_upload_queue_compute_summary(queue, &summary);
    logger_upload_queue_tmp_release(queue);

    jsw w;
    jsw_ok(&w, command, logger_clock_now_utc_or_null(&app->clock));
    logger_json_stream_writer_field_string_or_null(&w, "session_id",
                                                   session_id);
    logger_json_stream_writer_field_string_or_null(&w, "status", "verified");
    logger_json_stream_writer_field_bool(&w, "already_verified", true);
    logger_json_stream_writer_field_string_or_null(&w, "receipt_id",
                                                   receipt_id);
    logger_json_stream_writer_field_string_or_null(&w, "uploaded_sha256",
                                                   uploaded_sha256);
    logger_json_stream_writer_field_uint32(&w, "pending_count",
                                           summary.pending_count);
    jsw_end(&w);
    return;
  }

  const bool paths_ok =
      logger_path_join3(g_service_cli_mark_verified_manifest_path,
                        sizeof(g_service_cli_mark_verified_manifest_path),
                        "0:/logger/sessions/", entry->dir_name,
                        "/manifest.json") &&
      logger_path_join3(g_service_cli_mark_verified_journal_path,
                        sizeof(g_service_cli_mark_verified_journal_path),
                        "0:/logger/sessions/", entry->dir_name, "/journal.bin");
  if (!paths_ok) {
    logger_upload_queue_tmp_release(queue);
    jsw w;
    jsw_err(&w, command, logger_clock_now_utc_or_null(&app->clock),
            "artifact_path_invalid",
            "failed to build canonical bundle paths for session");
    return;
  }

  char local_sha256[LOGGER_UPLOAD_QUEUE_SHA256_HEX_LEN + 1];
  uint64_t local_size_bytes = 0u;
  if (!logger_storage_svc_bundle_compute(
          entry->dir_name, g_service_cli_mark_verified_manifest_path,
          g_service_cli_mark_verified_journal_path, local_sha256,
          &local_size_bytes)) {
    logger_upload_queue_tmp_release(queue);
    jsw w;
    jsw_err(&w, command, logger_clock_now_utc_or_null(&app->clock),
            "artifact_unavailable",
            "failed to recompute canonical upload bundle");
    return;
  }

  if (strcmp(local_sha256, entry->bundle_sha256) != 0 ||
      local_size_bytes != entry->bundle_size_bytes) {
    logger_upload_queue_tmp_release(queue);
    jsw w;
    jsw_err(&w, command, logger_clock_now_utc_or_null(&app->clock),
            "queue_artifact_mismatch",
            "local canonical bundle no longer matches upload queue entry");
    return;
  }

  if (strcmp(uploaded_sha256, local_sha256) != 0) {
    logger_upload_queue_tmp_release(queue);
    jsw w;
    jsw_err(&w, command, logger_clock_now_utc_or_null(&app->clock),
            "artifact_hash_mismatch",
            "uploaded_sha256 does not match the local canonical bundle");
    return;
  }

  const char *now_utc = logger_clock_now_utc_or_null(&app->clock);
  logger_queue_entry_mark_verified_manual(entry, receipt_id, local_sha256,
                                          now_utc);
  logger_copy_string(queue->updated_at_utc, sizeof(queue->updated_at_utc),
                     now_utc);

  if (!logger_storage_svc_queue_write(queue)) {
    logger_upload_queue_tmp_release(queue);
    jsw w;
    jsw_err(&w, command, logger_clock_now_utc_or_null(&app->clock),
            "storage_unavailable", "failed to write upload queue store");
    return;
  }

  logger_upload_queue_summary_t summary;
  logger_upload_queue_compute_summary(queue, &summary);
  logger_upload_queue_tmp_release(queue);

  logger_json_object_writer_t writer;
  logger_json_object_writer_init(&writer, g_service_cli_details_json,
                                 sizeof(g_service_cli_details_json));
  if (logger_json_object_writer_string_field(&writer, "session_id",
                                             session_id) &&
      logger_json_object_writer_string_field(&writer, "receipt_id",
                                             receipt_id) &&
      logger_json_object_writer_string_field(&writer, "uploaded_sha256",
                                             uploaded_sha256) &&
      logger_json_object_writer_string_field(&writer, "local_sha256",
                                             local_sha256) &&
      logger_json_object_writer_string_field(&writer, "source",
                                             "host_manual_upload") &&
      logger_json_object_writer_finish(&writer)) {
    (void)logger_system_log_append(
        &app->system_log, logger_clock_now_utc_or_null(&app->clock),
        "queue_mark_verified", LOGGER_SYSTEM_LOG_SEVERITY_WARN,
        logger_json_object_writer_data(&writer));
  }

  jsw w;
  jsw_ok(&w, command, logger_clock_now_utc_or_null(&app->clock));
  logger_json_stream_writer_field_string_or_null(&w, "session_id", session_id);
  logger_json_stream_writer_field_string_or_null(&w, "status", "verified");
  logger_json_stream_writer_field_bool(&w, "already_verified", false);
  logger_json_stream_writer_field_string_or_null(&w, "receipt_id", receipt_id);
  logger_json_stream_writer_field_string_or_null(&w, "uploaded_sha256",
                                                 uploaded_sha256);
  logger_json_stream_writer_field_string_or_null(&w, "local_sha256",
                                                 local_sha256);
  logger_json_stream_writer_field_uint64(&w, "bundle_size_bytes",
                                         local_size_bytes);
  logger_json_stream_writer_field_uint32(&w, "pending_count",
                                         summary.pending_count);
  jsw_end(&w);
}

static void logger_handle_debug_queue_mark_nonretryable(
    logger_service_cli_t *cli, logger_app_t *app, const char *args) {
  const char *command = "debug queue mark-nonretryable";
  if (!logger_cli_require_service_unlocked(cli, app, command)) {
    return;
  }

  char session_id[LOGGER_SESSION_ID_HEX_LEN + 1];
  char reason[LOGGER_UPLOAD_QUEUE_RESPONSE_EXCERPT_MAX + 1];
  memset(session_id, 0, sizeof(session_id));
  memset(reason, 0, sizeof(reason));

  const char *space = args != NULL ? strchr(args, ' ') : NULL;
  if (space == NULL || (size_t)(space - args) != LOGGER_SESSION_ID_HEX_LEN ||
      strlen(space + 1u) > LOGGER_UPLOAD_QUEUE_RESPONSE_EXCERPT_MAX) {
    jsw w;
    jsw_err(
        &w, command, logger_clock_now_utc_or_null(&app->clock),
        "invalid_argument",
        "expected: debug queue mark-nonretryable <session_id> <reason_token>");
    return;
  }
  memcpy(session_id, args, LOGGER_SESSION_ID_HEX_LEN);
  logger_copy_string(reason, sizeof(reason), space + 1u);
  if (!logger_cli_reason_token_valid(reason)) {
    jsw w;
    jsw_err(&w, command, logger_clock_now_utc_or_null(&app->clock),
            "invalid_argument",
            "reason_token must be lowercase snake_case and <=160 chars");
    return;
  }

  uint8_t session_id_bytes[16];
  if (!logger_hex_to_bytes_16(session_id, session_id_bytes)) {
    jsw w;
    jsw_err(&w, command, logger_clock_now_utc_or_null(&app->clock),
            "invalid_argument", "session_id must be 32 hexadecimal chars");
    return;
  }

  logger_upload_queue_t *queue = logger_upload_queue_tmp_acquire();
  if (!logger_storage_svc_queue_load(queue)) {
    logger_upload_queue_tmp_release(queue);
    jsw w;
    jsw_err(&w, command, logger_clock_now_utc_or_null(&app->clock),
            "storage_unavailable", "failed to load upload queue store");
    return;
  }

  logger_upload_queue_entry_t *entry = NULL;
  for (size_t i = 0u; i < queue->session_count; ++i) {
    if (strcmp(queue->sessions[i].session_id, session_id) == 0) {
      entry = &queue->sessions[i];
      break;
    }
  }
  if (entry == NULL) {
    logger_upload_queue_tmp_release(queue);
    jsw w;
    jsw_err(&w, command, logger_clock_now_utc_or_null(&app->clock), "not_found",
            "session_id is not present in upload queue");
    return;
  }
  if (strcmp(entry->status, "verified") == 0) {
    logger_upload_queue_tmp_release(queue);
    jsw w;
    jsw_err(&w, command, logger_clock_now_utc_or_null(&app->clock),
            "not_permitted_in_status",
            "verified sessions cannot be marked nonretryable");
    return;
  }

  logger_copy_string(entry->status, sizeof(entry->status), "nonretryable");
  logger_copy_string(entry->last_failure_class,
                     sizeof(entry->last_failure_class), "manual_nonretryable");
  entry->last_http_status = 0u;
  entry->last_server_error_code[0] = '\0';
  entry->last_server_error_message[0] = '\0';
  logger_copy_string(entry->last_response_excerpt,
                     sizeof(entry->last_response_excerpt), reason);
  entry->verified_upload_utc[0] = '\0';
  entry->verified_bundle_sha256[0] = '\0';
  entry->receipt_id[0] = '\0';
  logger_copy_string(queue->updated_at_utc, sizeof(queue->updated_at_utc),
                     logger_clock_now_utc_or_null(&app->clock));

  if (!logger_storage_svc_queue_write(queue)) {
    logger_upload_queue_tmp_release(queue);
    jsw w;
    jsw_err(&w, command, logger_clock_now_utc_or_null(&app->clock),
            "storage_unavailable", "failed to write upload queue store");
    return;
  }

  logger_upload_queue_summary_t summary;
  logger_upload_queue_compute_summary(queue, &summary);
  logger_upload_queue_tmp_release(queue);

  logger_json_object_writer_t writer;
  logger_json_object_writer_init(&writer, g_service_cli_details_json,
                                 sizeof(g_service_cli_details_json));
  if (logger_json_object_writer_string_field(&writer, "session_id",
                                             session_id) &&
      logger_json_object_writer_string_field(&writer, "reason", reason) &&
      logger_json_object_writer_string_field(&writer, "source", "host") &&
      logger_json_object_writer_finish(&writer)) {
    (void)logger_system_log_append(
        &app->system_log, logger_clock_now_utc_or_null(&app->clock),
        "queue_mark_nonretryable", LOGGER_SYSTEM_LOG_SEVERITY_WARN,
        logger_json_object_writer_data(&writer));
  }

  jsw w;
  jsw_ok(&w, command, logger_clock_now_utc_or_null(&app->clock));
  logger_json_stream_writer_field_string_or_null(&w, "session_id", session_id);
  logger_json_stream_writer_field_string_or_null(&w, "status", "nonretryable");
  logger_json_stream_writer_field_string_or_null(&w, "reason", reason);
  logger_json_stream_writer_field_uint32(&w, "pending_count",
                                         summary.pending_count);
  jsw_end(&w);
}

static void logger_handle_debug_queue_requeue_nonretryable(
    logger_service_cli_t *cli, logger_app_t *app, const char *args) {
  const char *command = "debug queue requeue-nonretryable";
  if (!logger_cli_require_service_unlocked(cli, app, command)) {
    return;
  }

  char target[LOGGER_SESSION_ID_HEX_LEN + 1];
  char reason[LOGGER_UPLOAD_QUEUE_RESPONSE_EXCERPT_MAX + 1];
  memset(target, 0, sizeof(target));
  memset(reason, 0, sizeof(reason));

  const char *space = args != NULL ? strchr(args, ' ') : NULL;
  const size_t target_len = space != NULL ? (size_t)(space - args) : 0u;
  if (space == NULL || target_len == 0u ||
      target_len > LOGGER_SESSION_ID_HEX_LEN ||
      strlen(space + 1u) > LOGGER_UPLOAD_QUEUE_RESPONSE_EXCERPT_MAX) {
    jsw w;
    jsw_err(&w, command, logger_clock_now_utc_or_null(&app->clock),
            "invalid_argument",
            "expected: debug queue requeue-nonretryable <session_id|all> "
            "<reason_token>");
    return;
  }
  memcpy(target, args, target_len);
  target[target_len] = '\0';
  logger_copy_string(reason, sizeof(reason), space + 1u);

  if (!logger_cli_reason_token_valid(reason)) {
    jsw w;
    jsw_err(&w, command, logger_clock_now_utc_or_null(&app->clock),
            "invalid_argument",
            "reason_token must be lowercase snake_case and <=160 chars");
    return;
  }

  const bool all = strcmp(target, "all") == 0;
  if (!all) {
    if (target_len != LOGGER_SESSION_ID_HEX_LEN) {
      jsw w;
      jsw_err(&w, command, logger_clock_now_utc_or_null(&app->clock),
              "invalid_argument",
              "target must be 'all' or a 32-char session_id");
      return;
    }
    uint8_t session_id_bytes[16];
    if (!logger_hex_to_bytes_16(target, session_id_bytes)) {
      jsw w;
      jsw_err(&w, command, logger_clock_now_utc_or_null(&app->clock),
              "invalid_argument", "session_id must be 32 hexadecimal chars");
      return;
    }
    for (size_t i = 0u; i < LOGGER_SESSION_ID_HEX_LEN; ++i) {
      target[i] = (char)tolower((unsigned char)target[i]);
    }
  }

  logger_upload_queue_t *queue = logger_upload_queue_tmp_acquire();
  if (!logger_storage_svc_queue_load(queue)) {
    logger_upload_queue_tmp_release(queue);
    jsw w;
    jsw_err(&w, command, logger_clock_now_utc_or_null(&app->clock),
            "storage_unavailable", "failed to load upload queue store");
    return;
  }

  bool target_found = all;
  bool target_wrong_status = false;
  size_t requeued_count = 0u;
  for (size_t i = 0u; i < queue->session_count; ++i) {
    logger_upload_queue_entry_t *entry = &queue->sessions[i];
    if (!all && strcmp(entry->session_id, target) != 0) {
      continue;
    }
    target_found = true;
    if (strcmp(entry->status, "nonretryable") != 0) {
      target_wrong_status = !all;
      if (!all) {
        break;
      }
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
    if (!all) {
      break;
    }
  }

  if (!target_found) {
    logger_upload_queue_tmp_release(queue);
    jsw w;
    jsw_err(&w, command, logger_clock_now_utc_or_null(&app->clock), "not_found",
            "session_id is not present in upload queue");
    return;
  }
  if (target_wrong_status) {
    logger_upload_queue_tmp_release(queue);
    jsw w;
    jsw_err(&w, command, logger_clock_now_utc_or_null(&app->clock),
            "not_permitted_in_status", "selected session is not nonretryable");
    return;
  }

  if (requeued_count > 0u) {
    logger_copy_string(queue->updated_at_utc, sizeof(queue->updated_at_utc),
                       logger_clock_now_utc_or_null(&app->clock));
    if (!logger_storage_svc_queue_write(queue)) {
      logger_upload_queue_tmp_release(queue);
      jsw w;
      jsw_err(&w, command, logger_clock_now_utc_or_null(&app->clock),
              "storage_unavailable", "failed to write upload queue store");
      return;
    }
  }

  logger_upload_queue_summary_t summary;
  logger_upload_queue_compute_summary(queue, &summary);
  logger_upload_queue_tmp_release(queue);

  logger_json_object_writer_t writer;
  logger_json_object_writer_init(&writer, g_service_cli_details_json,
                                 sizeof(g_service_cli_details_json));
  if (logger_json_object_writer_string_field(&writer, "target", target) &&
      logger_json_object_writer_string_field(&writer, "reason", reason) &&
      logger_json_object_writer_uint32_field(&writer, "requeued_count",
                                             (uint32_t)requeued_count) &&
      logger_json_object_writer_finish(&writer)) {
    (void)logger_system_log_append(
        &app->system_log, logger_clock_now_utc_or_null(&app->clock),
        "queue_requeue_nonretryable", LOGGER_SYSTEM_LOG_SEVERITY_WARN,
        logger_json_object_writer_data(&writer));
  }

  jsw w;
  jsw_ok(&w, command, logger_clock_now_utc_or_null(&app->clock));
  logger_json_stream_writer_field_string_or_null(&w, "target", target);
  logger_json_stream_writer_field_string_or_null(&w, "reason", reason);
  logger_json_stream_writer_field_uint32(&w, "requeued_count",
                                         (uint32_t)requeued_count);
  logger_json_stream_writer_field_uint32(&w, "pending_count",
                                         summary.pending_count);
  jsw_end(&w);
}

static void logger_service_bundle_export_reset(void) {
  if (g_service_bundle_export.open) {
    logger_storage_svc_bundle_close();
  }
  memset(&g_service_bundle_export, 0, sizeof(g_service_bundle_export));
}

static bool logger_cli_require_service_unlocked_for_bundle(
    logger_service_cli_t *cli, logger_app_t *app, const char *command) {
  if (!logger_cli_require_service(app, command)) {
    return false;
  }
  if (!logger_service_cli_is_unlocked(cli,
                                      to_ms_since_boot(get_absolute_time()))) {
    jsw w;
    jsw_err(&w, command, logger_clock_now_utc_or_null(&app->clock),
            "service_locked",
            "service unlock is required before bundle export");
    return false;
  }
  return true;
}

static void logger_handle_debug_bundle_open(logger_service_cli_t *cli,
                                            logger_app_t *app,
                                            const char *session_id) {
  const char *command = "debug bundle open";
  if (!logger_cli_require_service_unlocked_for_bundle(cli, app, command)) {
    return;
  }
  uint8_t session_id_bytes[16];
  if (!logger_hex_to_bytes_16(session_id, session_id_bytes)) {
    jsw w;
    jsw_err(&w, command, logger_clock_now_utc_or_null(&app->clock),
            "invalid_argument", "expected 32 lowercase hexadecimal session_id");
    return;
  }

  logger_service_bundle_export_reset();

  logger_upload_queue_t *queue = logger_upload_queue_tmp_acquire();
  if (!logger_storage_svc_queue_load(queue)) {
    (void)logger_storage_svc_queue_scan(
        queue, NULL, logger_clock_now_utc_or_null(&app->clock));
  }
  const logger_upload_queue_entry_t *entry =
      logger_upload_queue_find_by_session_id(queue, session_id);
  if (entry == NULL) {
    logger_upload_queue_tmp_release(queue);
    jsw w;
    jsw_err(&w, command, logger_clock_now_utc_or_null(&app->clock), "not_found",
            "session_id is not present in upload queue");
    return;
  }

  const bool paths_ok =
      logger_path_join3(g_service_bundle_export.manifest_path,
                        sizeof(g_service_bundle_export.manifest_path),
                        "0:/logger/sessions/", entry->dir_name,
                        "/manifest.json") &&
      logger_path_join3(g_service_bundle_export.journal_path,
                        sizeof(g_service_bundle_export.journal_path),
                        "0:/logger/sessions/", entry->dir_name, "/journal.bin");
  if (paths_ok && logger_storage_svc_bundle_open(
                      entry->dir_name, g_service_bundle_export.manifest_path,
                      g_service_bundle_export.journal_path)) {
    g_service_bundle_export.open = true;
    logger_copy_string(g_service_bundle_export.session_id,
                       sizeof(g_service_bundle_export.session_id),
                       entry->session_id);
    logger_copy_string(g_service_bundle_export.dir_name,
                       sizeof(g_service_bundle_export.dir_name),
                       entry->dir_name);
    g_service_bundle_export.bundle_size_bytes = entry->bundle_size_bytes;
  }
  logger_upload_queue_tmp_release(queue);

  if (!g_service_bundle_export.open) {
    jsw w;
    jsw_err(&w, command, logger_clock_now_utc_or_null(&app->clock),
            "storage_unavailable", "failed to open canonical upload bundle");
    return;
  }

  jsw w;
  jsw_ok(&w, command, logger_clock_now_utc_or_null(&app->clock));
  logger_json_stream_writer_field_string_or_null(
      &w, "session_id", g_service_bundle_export.session_id);
  logger_json_stream_writer_field_string_or_null(
      &w, "dir_name", g_service_bundle_export.dir_name);
  logger_json_stream_writer_field_uint64(
      &w, "bundle_size_bytes", g_service_bundle_export.bundle_size_bytes);
  logger_json_stream_writer_field_uint32(
      &w, "chunk_size_bytes", LOGGER_SERVICE_BUNDLE_EXPORT_CHUNK_BYTES);
  jsw_end(&w);
}

static void logger_handle_debug_bundle_read(logger_service_cli_t *cli,
                                            logger_app_t *app) {
  const char *command = "debug bundle read";
  if (!logger_cli_require_service_unlocked_for_bundle(cli, app, command)) {
    return;
  }
  if (!g_service_bundle_export.open) {
    jsw w;
    jsw_err(&w, command, logger_clock_now_utc_or_null(&app->clock), "not_open",
            "open a bundle before reading it");
    return;
  }

  size_t len = 0u;
  if (!logger_storage_svc_bundle_read(g_service_bundle_export.chunk,
                                      sizeof(g_service_bundle_export.chunk),
                                      &len)) {
    logger_service_bundle_export_reset();
    jsw w;
    jsw_err(&w, command, logger_clock_now_utc_or_null(&app->clock),
            "storage_unavailable", "failed to read canonical upload bundle");
    return;
  }

  size_t encoded_len = 0u;
  if (mbedtls_base64_encode(
          (unsigned char *)g_service_bundle_export.chunk_base64,
          sizeof(g_service_bundle_export.chunk_base64), &encoded_len,
          g_service_bundle_export.chunk, len) != 0) {
    logger_service_bundle_export_reset();
    jsw w;
    jsw_err(&w, command, logger_clock_now_utc_or_null(&app->clock),
            "internal_error", "failed to base64-encode bundle chunk");
    return;
  }
  g_service_bundle_export.chunk_base64[encoded_len] = '\0';

  const uint64_t offset = g_service_bundle_export.offset;
  char session_id[LOGGER_SESSION_ID_HEX_LEN + 1];
  logger_copy_string(session_id, sizeof(session_id),
                     g_service_bundle_export.session_id);
  g_service_bundle_export.offset += len;
  const bool eof = len == 0u;

  jsw w;
  jsw_ok(&w, command, logger_clock_now_utc_or_null(&app->clock));
  logger_json_stream_writer_field_string_or_null(&w, "session_id", session_id);
  logger_json_stream_writer_field_uint64(&w, "offset", offset);
  logger_json_stream_writer_field_uint32(&w, "len", (uint32_t)len);
  logger_json_stream_writer_field_bool(&w, "eof", eof);
  logger_json_stream_writer_field_string_or_null(
      &w, "data_base64", g_service_bundle_export.chunk_base64);
  jsw_end(&w);

  if (eof) {
    logger_service_bundle_export_reset();
  }
}

static void logger_handle_debug_bundle_close(logger_service_cli_t *cli,
                                             logger_app_t *app) {
  const char *command = "debug bundle close";
  if (!logger_cli_require_service_unlocked_for_bundle(cli, app, command)) {
    return;
  }
  logger_service_bundle_export_reset();
  jsw w;
  jsw_ok(&w, command, logger_clock_now_utc_or_null(&app->clock));
  logger_json_stream_writer_field_bool(&w, "closed", true);
  jsw_end(&w);
}

static void logger_handle_debug_prune_once(logger_service_cli_t *cli,
                                           logger_app_t *app) {
  const bool allowed_in_wait_h10 =
      app->runtime.current_state == LOGGER_RUNTIME_LOG_WAIT_H10;
  if (!logger_cli_is_service_mode(app) && !allowed_in_wait_h10) {
    jsw w;
    jsw_err(&w, "debug prune once", logger_clock_now_utc_or_null(&app->clock),
            "not_permitted_in_mode",
            "debug prune once is only allowed in service mode or log_wait_h10");
    return;
  }
  if (app->session.active) {
    jsw w;
    jsw_err(&w, "debug prune once", logger_clock_now_utc_or_null(&app->clock),
            "not_permitted_in_mode",
            "debug prune once is not permitted while a session is active");
    return;
  }
  if (logger_cli_is_service_mode(app) &&
      !logger_service_cli_is_unlocked(cli,
                                      to_ms_since_boot(get_absolute_time()))) {
    jsw w;
    jsw_err(&w, "debug prune once", logger_clock_now_utc_or_null(&app->clock),
            "service_locked",
            "service unlock is required before debug prune once");
    return;
  }

  size_t retention_pruned_count = 0u;
  size_t reserve_pruned_count = 0u;
  bool reserve_met = false;
  logger_upload_queue_summary_t summary;
  if (!logger_storage_svc_queue_prune(
          &app->system_log, logger_clock_now_utc_or_null(&app->clock),
          LOGGER_SD_MIN_FREE_RESERVE_BYTES, &retention_pruned_count,
          &reserve_pruned_count, &reserve_met, &summary)) {
    jsw w;
    jsw_err(&w, "debug prune once", logger_clock_now_utc_or_null(&app->clock),
            "storage_unavailable",
            "failed to apply upload retention/prune policy");
    return;
  }

  jsw w;
  jsw_ok(&w, "debug prune once", logger_clock_now_utc_or_null(&app->clock));
  logger_json_stream_writer_field_uint32(&w, "retention_pruned_count",
                                         (uint32_t)retention_pruned_count);
  logger_json_stream_writer_field_uint32(&w, "reserve_pruned_count",
                                         (uint32_t)reserve_pruned_count);
  logger_json_stream_writer_field_bool(&w, "reserve_met", reserve_met);
  logger_json_stream_writer_field_string_or_null(
      &w, "updated_at_utc",
      summary.updated_at_utc[0] != '\0' ? summary.updated_at_utc : NULL);
  logger_json_stream_writer_field_uint32(&w, "session_count",
                                         (uint32_t)summary.session_count);
  logger_json_stream_writer_field_uint32(&w, "pending_count",
                                         (uint32_t)summary.pending_count);
  logger_json_stream_writer_field_uint32(&w, "blocked_count",
                                         (uint32_t)summary.blocked_count);
  jsw_end(&w);
}

typedef struct {
  logger_service_cli_t *cli;
  logger_app_t *app;
} logger_service_cli_upload_busy_hook_ctx_t;

static void
logger_service_cli_upload_busy_hook(void *ctx, logger_busy_poll_phase_t phase) {
  logger_service_cli_upload_busy_hook_ctx_t *hook_ctx =
      (logger_service_cli_upload_busy_hook_ctx_t *)ctx;
  if (hook_ctx == NULL || hook_ctx->cli == NULL || hook_ctx->app == NULL) {
    return;
  }
  logger_service_cli_poll_upload_busy(hook_ctx->cli, hook_ctx->app,
                                      to_ms_since_boot(get_absolute_time()),
                                      phase);
}

static void logger_handle_debug_upload_once(logger_service_cli_t *cli,
                                            logger_app_t *app) {
  const bool allowed_in_wait_h10 =
      app->runtime.current_state == LOGGER_RUNTIME_LOG_WAIT_H10;
  if (!logger_cli_is_service_mode(app) && !allowed_in_wait_h10) {
    jsw w;
    jsw_err(
        &w, "debug upload once", logger_clock_now_utc_or_null(&app->clock),
        "not_permitted_in_mode",
        "debug upload once is only allowed in service mode or log_wait_h10");
    return;
  }
  if (logger_cli_is_service_mode(app) &&
      !logger_service_cli_is_unlocked(cli,
                                      to_ms_since_boot(get_absolute_time()))) {
    jsw w;
    jsw_err(&w, "debug upload once", logger_clock_now_utc_or_null(&app->clock),
            "service_locked",
            "service unlock is required before debug upload once");
    return;
  }

  logger_upload_process_result_t *result = &g_service_cli_upload_process_result;
  logger_service_cli_upload_busy_hook_ctx_t busy_ctx = {.cli = cli, .app = app};
  const logger_busy_poll_t busy_poll = {
      .ctx = &busy_ctx,
      .poll = logger_service_cli_upload_busy_hook,
  };
  const bool ok = logger_upload_process_one(
      &app->system_log, &app->persisted.config, app->hardware_id,
      logger_clock_now_utc_or_null(&app->clock), &busy_poll, result);

  if (result->code == LOGGER_UPLOAD_PROCESS_RESULT_NOT_ATTEMPTED) {
    jsw w;
    jsw_err(&w, "debug upload once", logger_clock_now_utc_or_null(&app->clock),
            "invalid_config", result->message);
    return;
  }
  if (result->code == LOGGER_UPLOAD_PROCESS_RESULT_PERSIST_FAILED) {
    jsw w;
    jsw_err(&w, "debug upload once", logger_clock_now_utc_or_null(&app->clock),
            "storage_unavailable", result->message);
    return;
  }
  if (result->code == LOGGER_UPLOAD_PROCESS_RESULT_CONFIG_BLOCKED) {
    jsw w;
    jsw_err(&w, "debug upload once", logger_clock_now_utc_or_null(&app->clock),
            logger_string_present(result->failure_class)
                ? result->failure_class
                : "upload_config_blocked",
            result->message);
    return;
  }
  if (result->code == LOGGER_UPLOAD_PROCESS_RESULT_NONRETRYABLE) {
    jsw w;
    jsw_err(&w, "debug upload once", logger_clock_now_utc_or_null(&app->clock),
            logger_string_present(result->failure_class) ? result->failure_class
                                                         : "nonretryable",
            result->message);
    return;
  }
  if (result->code == LOGGER_UPLOAD_PROCESS_RESULT_FAILED) {
    jsw w;
    jsw_err(&w, "debug upload once", logger_clock_now_utc_or_null(&app->clock),
            logger_string_present(result->failure_class) ? result->failure_class
                                                         : "upload_failed",
            result->message);
    return;
  }
  if (result->code == LOGGER_UPLOAD_PROCESS_RESULT_BLOCKED_MIN_FIRMWARE) {
    jsw w;
    jsw_err(&w, "debug upload once", logger_clock_now_utc_or_null(&app->clock),
            "min_firmware_rejected", result->message);
    return;
  }

  jsw w;
  jsw_ok(&w, "debug upload once", logger_clock_now_utc_or_null(&app->clock));
  logger_json_stream_writer_field_bool(&w, "attempted", result->attempted);
  logger_json_stream_writer_field_string_or_null(
      &w, "result",
      result->code == LOGGER_UPLOAD_PROCESS_RESULT_VERIFIED ? "verified"
      : result->code == LOGGER_UPLOAD_PROCESS_RESULT_NO_WORK
          ? "no_work"
          : (ok ? "ok" : "unknown"));
  logger_json_stream_writer_field_string_or_null(
      &w, "session_id",
      logger_string_present(result->session_id) ? result->session_id : NULL);
  logger_json_stream_writer_field_string_or_null(
      &w, "final_status",
      logger_string_present(result->final_status) ? result->final_status
                                                  : NULL);
  logger_json_stream_writer_field_string_or_null(
      &w, "receipt_id",
      logger_string_present(result->receipt_id) ? result->receipt_id : NULL);
  logger_json_stream_writer_field_string_or_null(
      &w, "verified_upload_utc",
      logger_string_present(result->verified_upload_utc)
          ? result->verified_upload_utc
          : NULL);
  if (result->http_status >= 0) {
    logger_json_stream_writer_field_int32(&w, "http_status",
                                          result->http_status);
  } else {
    logger_json_stream_writer_field_null(&w, "http_status");
  }
  logger_json_stream_writer_field_string_or_null(
      &w, "server_error_code",
      logger_string_present(result->server_error_code)
          ? result->server_error_code
          : NULL);
  logger_json_stream_writer_field_string_or_null(
      &w, "server_error_message",
      logger_string_present(result->server_error_message)
          ? result->server_error_message
          : NULL);
  logger_json_stream_writer_field_string_or_null(
      &w, "response_excerpt",
      logger_string_present(result->response_excerpt) ? result->response_excerpt
                                                      : NULL);
  logger_json_stream_writer_field_string_or_null(
      &w, "message",
      logger_string_present(result->message) ? result->message : NULL);
  jsw_end(&w);
}

void logger_service_cli_init(logger_service_cli_t *cli) {
  memset(cli, 0, sizeof(*cli));
}

static bool logger_service_cli_is_unlocked(const logger_service_cli_t *cli,
                                           uint32_t now_ms) {
  return cli->unlocked &&
         !logger_mono_ms_deadline_reached(now_ms, cli->unlock_deadline_mono_ms);
}

static void logger_handle_capture_stats_json(const logger_app_t *app) {
  const logger_capture_stats_t *s = &app->capture_stats;
  jsw w;
  jsw_ok(&w, "capture stats", logger_clock_now_utc_or_null(&app->clock));

  logger_json_stream_writer_field_object_begin(&w, "h10_queue");
  logger_json_stream_writer_field_uint32(&w, "depth_hwm",
                                         (uint32_t)s->queue_depth_hwm);
  logger_json_stream_writer_field_uint32(&w, "queue_capacity",
                                         LOGGER_H10_PACKET_QUEUE_DEPTH);
  logger_json_stream_writer_field_uint32(
      &w, "ecg_packet_drops", (uint32_t)app->h10.ecg_packet_drop_count);
  logger_json_stream_writer_field_uint32(
      &w, "acc_packet_drops", (uint32_t)app->h10.acc_packet_drop_count);
  logger_json_stream_writer_object_end(&w);

  logger_json_stream_writer_field_object_begin(&w, "session_append");
  logger_json_stream_writer_field_uint32(&w, "count", s->session_append_count);
  logger_json_stream_writer_field_uint32(&w, "fail_count",
                                         s->session_append_fail_count);
  logger_json_stream_writer_field_uint32(&w, "max_us",
                                         s->session_append_max_us);
  logger_json_stream_writer_field_uint32(&w, "last_us",
                                         s->session_append_last_us);
  logger_json_stream_writer_object_end(&w);

  logger_json_stream_writer_field_object_begin(&w, "storage_append");
  logger_json_stream_writer_field_uint32(&w, "count", s->storage_append_count);
  logger_json_stream_writer_field_uint32(&w, "fail_count",
                                         s->storage_append_fail_count);
  logger_json_stream_writer_field_uint32(&w, "max_us",
                                         s->storage_append_max_us);
  logger_json_stream_writer_field_uint32(&w, "last_us",
                                         s->storage_append_last_us);
  logger_json_stream_writer_object_end(&w);

  logger_json_stream_writer_field_object_begin(&w, "sync");
  logger_json_stream_writer_field_uint32(&w, "count", s->sync_count);
  logger_json_stream_writer_field_uint32(&w, "max_us", s->sync_max_us);
  logger_json_stream_writer_field_uint32(&w, "last_us", s->sync_last_us);
  logger_json_stream_writer_object_end(&w);

  logger_json_stream_writer_field_object_begin(&w, "journal");
  logger_json_stream_writer_field_uint32(&w, "record_count",
                                         s->journal_record_count);
  logger_json_stream_writer_field_uint32(&w, "record_fail_count",
                                         s->journal_record_fail_count);
  logger_json_stream_writer_object_end(&w);

  jsw_end(&w);
}

static void logger_service_cli_execute(logger_service_cli_t *cli,
                                       logger_app_t *app, const char *line,
                                       uint32_t now_ms) {
  if (cli->unlocked &&
      logger_mono_ms_deadline_reached(now_ms, cli->unlock_deadline_mono_ms)) {
    cli->unlocked = false;
  }

  if (strcmp(line, "status --json") == 0) {
    logger_handle_status_json(app);
    return;
  }
  if (strcmp(line, "capture stats --json") == 0) {
    logger_handle_capture_stats_json(app);
    return;
  }
  if (strcmp(line, "capture stats reset") == 0) {
    logger_capture_stats_reset((logger_capture_stats_t *)&app->capture_stats);
    jsw w;
    jsw_ok(&w, "capture stats reset",
           logger_clock_now_utc_or_null(&app->clock));
    logger_json_stream_writer_field_bool(&w, "reset", true);
    jsw_end(&w);
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
    logger_handle_fault_clear(app, now_ms);
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
  if (strncmp(line, "debug queue mark-verified ", 26) == 0) {
    logger_handle_debug_queue_mark_verified(cli, app, line + 26);
    return;
  }
  if (strncmp(line, "debug queue requeue-nonretryable ", 33) == 0) {
    logger_handle_debug_queue_requeue_nonretryable(cli, app, line + 33);
    return;
  }
  if (strncmp(line, "debug queue mark-nonretryable ", 30) == 0) {
    logger_handle_debug_queue_mark_nonretryable(cli, app, line + 30);
    return;
  }
  if (strncmp(line, "debug bundle open ", 18) == 0) {
    logger_handle_debug_bundle_open(cli, app, line + 18);
    return;
  }
  if (strcmp(line, "debug bundle read") == 0) {
    logger_handle_debug_bundle_read(cli, app);
    return;
  }
  if (strcmp(line, "debug bundle close") == 0) {
    logger_handle_debug_bundle_close(cli, app);
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
  if (strcmp(line, "debug h10 inject-stale-bond") == 0) {
    logger_handle_debug_h10_inject_stale_bond(cli, app, now_ms);
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
  if (strcmp(line, "debug synth clock-invalid") == 0) {
    logger_handle_debug_synth_clock_invalid(cli, app, now_ms);
    return;
  }
  if (strcmp(line, "debug synth clock-valid") == 0) {
    logger_handle_debug_synth_clock_valid(cli, app, now_ms);
    return;
  }
  if (strcmp(line, "debug synth storage-missing") == 0) {
    logger_handle_debug_synth_storage_fault(
        cli, app, LOGGER_DEBUG_STORAGE_FAULT_MISSING,
        "debug synth storage-missing", now_ms);
    return;
  }
  if (strcmp(line, "debug synth storage-low-space") == 0) {
    logger_handle_debug_synth_storage_fault(
        cli, app, LOGGER_DEBUG_STORAGE_FAULT_LOW_SPACE,
        "debug synth storage-low-space", now_ms);
    return;
  }
  if (strcmp(line, "debug synth storage-write-failed") == 0) {
    logger_handle_debug_synth_storage_fault(
        cli, app, LOGGER_DEBUG_STORAGE_FAULT_WRITE_FAILED,
        "debug synth storage-write-failed", now_ms);
    return;
  }
  if (strcmp(line, "debug synth storage-valid") == 0) {
    logger_handle_debug_synth_storage_valid(cli, app, now_ms);
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
    jsw w;
    jsw_ok(&w, "debug reboot", logger_clock_now_utc_or_null(&app->clock));
    logger_json_stream_writer_field_bool(&w, "will_reboot", true);
    jsw_end(&w);
    return;
  }
  if (strcmp(line, "sd format") == 0) {
    logger_handle_sd_format(cli, app);
    return;
  }

  jsw w;
  jsw_err(&w, line, logger_clock_now_utc_or_null(&app->clock), "internal_error",
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
      const bool have_line = cli->line_len > 0u;
      cli->line_len = 0u;
      if (have_line) {
        logger_service_cli_execute(cli, app, cli->line_buf, now_ms);
      }
      continue;
    }
    if (cli->line_len + 1u >= sizeof(cli->line_buf)) {
      cli->line_len = 0u;
      jsw w;
      jsw_err(&w, "input", logger_clock_now_utc_or_null(&app->clock),
              "internal_error", "input line too long");
      break;
    }
    cli->line_buf[cli->line_len++] = (char)ch;
  }
}

void logger_service_cli_poll_upload_busy(logger_service_cli_t *cli,
                                         logger_app_t *app, uint32_t now_ms,
                                         logger_busy_poll_phase_t phase) {
  size_t chars = 0u;
  size_t lines = 0u;
  while (chars < LOGGER_SERVICE_CLI_BUSY_POLL_CHAR_BUDGET &&
         lines < LOGGER_SERVICE_CLI_BUSY_POLL_LINE_BUDGET) {
    int ch = getchar_timeout_us(0);
    if (ch == PICO_ERROR_TIMEOUT) {
      break;
    }
    chars += 1u;
    if (ch == '\r') {
      continue;
    }
    if (ch == '\n') {
      cli->line_buf[cli->line_len] = '\0';
      const bool have_line = cli->line_len > 0u;
      cli->line_len = 0u;
      if (have_line) {
        logger_service_cli_execute_upload_busy(cli, app, cli->line_buf, now_ms,
                                               phase);
        lines += 1u;
      }
      continue;
    }
    if (cli->line_len + 1u >= sizeof(cli->line_buf)) {
      cli->line_len = 0u;
      jsw w;
      jsw_err(&w, "input", logger_clock_now_utc_or_null(&app->clock),
              "internal_error", "input line too long");
      break;
    }
    cli->line_buf[cli->line_len++] = (char)ch;
  }
}
