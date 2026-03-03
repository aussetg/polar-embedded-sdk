// SPDX-License-Identifier: MIT
#ifndef POLAR_SDK_GATT_CONTROL_H
#define POLAR_SDK_GATT_CONTROL_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    POLAR_SDK_GATT_NOTIFY_OK = 0,
    POLAR_SDK_GATT_NOTIFY_NOT_CONNECTED,
    POLAR_SDK_GATT_NOTIFY_NO_NOTIFY_PROP,
    POLAR_SDK_GATT_NOTIFY_WRITE_FAILED,
    POLAR_SDK_GATT_NOTIFY_TIMEOUT,
    POLAR_SDK_GATT_NOTIFY_ATT_REJECTED,
} polar_sdk_gatt_notify_result_t;

typedef struct {
    void *ctx;
    bool (*is_connected_ready)(void *ctx);
    bool (*listener_active)(void *ctx);
    void (*start_listener)(void *ctx);
    void (*stop_listener)(void *ctx);

    // return 0 on success, non-zero transport error.
    int (*write_ccc)(void *ctx, uint16_t ccc_cfg);

    // return true on completion and set *out_att_status.
    bool (*wait_complete)(void *ctx, uint32_t timeout_ms, uint8_t *out_att_status);
} polar_sdk_gatt_notify_ops_t;

polar_sdk_gatt_notify_result_t polar_sdk_gatt_set_notify(
    const polar_sdk_gatt_notify_ops_t *ops,
    bool enable,
    uint8_t characteristic_properties,
    uint8_t notify_property_mask,
    uint8_t indicate_property_mask,
    uint16_t ccc_none,
    uint16_t ccc_notify,
    uint16_t ccc_indicate,
    uint8_t att_success,
    uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif // POLAR_SDK_GATT_CONTROL_H
