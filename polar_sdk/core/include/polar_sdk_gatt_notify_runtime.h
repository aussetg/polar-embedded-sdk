// SPDX-License-Identifier: MIT
#ifndef POLAR_SDK_GATT_NOTIFY_RUNTIME_H
#define POLAR_SDK_GATT_NOTIFY_RUNTIME_H

#include <stdbool.h>
#include <stdint.h>

#include "polar_sdk_gatt_control.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    POLAR_SDK_GATT_NOTIFY_RUNTIME_OK = 0,
    POLAR_SDK_GATT_NOTIFY_RUNTIME_MISSING_CHAR,
    POLAR_SDK_GATT_NOTIFY_RUNTIME_NOT_CONNECTED,
    POLAR_SDK_GATT_NOTIFY_RUNTIME_NO_NOTIFY_PROP,
    POLAR_SDK_GATT_NOTIFY_RUNTIME_WRITE_FAILED,
    POLAR_SDK_GATT_NOTIFY_RUNTIME_TIMEOUT,
    POLAR_SDK_GATT_NOTIFY_RUNTIME_ATT_REJECTED,
} polar_sdk_gatt_notify_runtime_result_t;

typedef struct {
    const polar_sdk_gatt_notify_ops_t *ops;
    bool has_value_handle;
    bool enable;
    uint16_t properties;

    uint16_t prop_notify;
    uint16_t prop_indicate;

    uint16_t ccc_none;
    uint16_t ccc_notify;
    uint16_t ccc_indicate;

    uint8_t att_success;
    uint32_t timeout_ms;

    bool *cfg_pending;
    bool *cfg_done;
} polar_sdk_gatt_notify_runtime_args_t;

polar_sdk_gatt_notify_runtime_result_t polar_sdk_gatt_notify_runtime_set(
    const polar_sdk_gatt_notify_runtime_args_t *args);

#ifdef __cplusplus
}
#endif

#endif // POLAR_SDK_GATT_NOTIFY_RUNTIME_H
