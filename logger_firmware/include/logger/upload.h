#ifndef LOGGER_FIRMWARE_UPLOAD_H
#define LOGGER_FIRMWARE_UPLOAD_H

#include <stdbool.h>
#include <stdint.h>

#include "logger/config_store.h"
#include "logger/queue.h"
#include "logger/system_log.h"

#define LOGGER_UPLOAD_URL_HOST_MAX 128
#define LOGGER_UPLOAD_URL_PATH_MAX 192
#define LOGGER_UPLOAD_DETAIL_MAX 160
#define LOGGER_UPLOAD_MESSAGE_MAX 160

typedef struct {
    const char *wifi_join_result;
    char wifi_join_details[LOGGER_UPLOAD_DETAIL_MAX + 1];
    const char *dns_result;
    char dns_details[LOGGER_UPLOAD_DETAIL_MAX + 1];
    const char *tls_result;
    char tls_details[LOGGER_UPLOAD_DETAIL_MAX + 1];
    const char *upload_endpoint_reachable_result;
    char upload_endpoint_reachable_details[LOGGER_UPLOAD_DETAIL_MAX + 1];
} logger_upload_net_test_result_t;

typedef enum {
    LOGGER_UPLOAD_PROCESS_RESULT_NONE = 0,
    LOGGER_UPLOAD_PROCESS_RESULT_NO_WORK,
    LOGGER_UPLOAD_PROCESS_RESULT_VERIFIED,
    LOGGER_UPLOAD_PROCESS_RESULT_FAILED,
    LOGGER_UPLOAD_PROCESS_RESULT_BLOCKED_MIN_FIRMWARE,
    LOGGER_UPLOAD_PROCESS_RESULT_NOT_ATTEMPTED,
} logger_upload_process_result_code_t;

typedef struct {
    logger_upload_process_result_code_t code;
    bool attempted;
    int http_status;
    char session_id[33];
    char final_status[LOGGER_UPLOAD_QUEUE_STATUS_MAX + 1];
    char failure_class[LOGGER_UPLOAD_QUEUE_FAILURE_CLASS_MAX + 1];
    char receipt_id[LOGGER_UPLOAD_QUEUE_RECEIPT_ID_MAX + 1];
    char verified_upload_utc[LOGGER_UPLOAD_QUEUE_UTC_MAX + 1];
    char message[LOGGER_UPLOAD_MESSAGE_MAX + 1];
} logger_upload_process_result_t;

void logger_upload_net_test_result_init(logger_upload_net_test_result_t *result);
void logger_upload_process_result_init(logger_upload_process_result_t *result);

bool logger_upload_net_test(
    const logger_config_t *config,
    logger_upload_net_test_result_t *result);

bool logger_upload_process_one(
    logger_system_log_t *system_log,
    const logger_config_t *config,
    const char *hardware_id,
    const char *now_utc_or_null,
    logger_upload_process_result_t *result);

#endif