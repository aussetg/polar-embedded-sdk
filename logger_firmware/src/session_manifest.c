#include "logger/session.h"

#include <string.h>

#include "logger/util.h"

void logger_session_manifest_ctx_refresh_from_config(
    logger_session_manifest_ctx_t *mc,
    const logger_persisted_state_t *persisted) {
  if (mc == NULL || persisted == NULL) {
    return;
  }
  if (logger_string_present(persisted->config.logger_id)) {
    logger_copy_string(mc->logger_id, sizeof(mc->logger_id),
                       persisted->config.logger_id);
  }
  if (logger_string_present(persisted->config.subject_id)) {
    logger_copy_string(mc->subject_id, sizeof(mc->subject_id),
                       persisted->config.subject_id);
  }
  if (logger_string_present(persisted->config.timezone)) {
    logger_copy_string(mc->timezone, sizeof(mc->timezone),
                       persisted->config.timezone);
  }
  if (logger_string_present(persisted->config.bound_h10_address)) {
    logger_copy_string(mc->bound_h10_address, sizeof(mc->bound_h10_address),
                       persisted->config.bound_h10_address);
  }
}

void logger_session_manifest_ctx_seed_recovered(
    logger_session_manifest_ctx_t *mc, const char *hardware_id,
    const logger_journal_scan_result_t *scan,
    const logger_persisted_state_t *persisted,
    const logger_storage_status_t *storage, bool debug_session,
    logger_system_log_t *system_log) {
  if (mc == NULL) {
    return;
  }

  memset(mc, 0, sizeof(*mc));
  logger_copy_string(mc->hardware_id, sizeof(mc->hardware_id), hardware_id);
  if (scan != NULL) {
    logger_copy_string(mc->logger_id, sizeof(mc->logger_id), scan->logger_id);
    logger_copy_string(mc->subject_id, sizeof(mc->subject_id),
                       scan->subject_id);
    logger_copy_string(mc->timezone, sizeof(mc->timezone), scan->timezone);
  }
  logger_session_manifest_ctx_refresh_from_config(mc, persisted);
  if (storage != NULL) {
    mc->storage = *storage;
  }
  mc->debug_session = debug_session;
  mc->system_log = system_log;
}

void logger_session_manifest_ctx_copy_persisted(
    const logger_session_manifest_ctx_t *mc,
    logger_persisted_state_t *persisted_for_manifest) {
  if (mc == NULL || persisted_for_manifest == NULL) {
    return;
  }
  memset(persisted_for_manifest, 0, sizeof(*persisted_for_manifest));
  logger_copy_string(persisted_for_manifest->config.logger_id,
                     sizeof(persisted_for_manifest->config.logger_id),
                     mc->logger_id);
  logger_copy_string(persisted_for_manifest->config.subject_id,
                     sizeof(persisted_for_manifest->config.subject_id),
                     mc->subject_id);
  logger_copy_string(persisted_for_manifest->config.timezone,
                     sizeof(persisted_for_manifest->config.timezone),
                     mc->timezone);
  logger_copy_string(persisted_for_manifest->config.bound_h10_address,
                     sizeof(persisted_for_manifest->config.bound_h10_address),
                     mc->bound_h10_address);
}