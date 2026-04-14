// SPDX-License-Identifier: MIT
#include "polar_sdk_gatt_control.h"

static bool
polar_sdk_gatt_notify_ops_ready(const polar_sdk_gatt_notify_ops_t *ops) {
  return ops != 0 && ops->is_connected_ready != 0 &&
         ops->listener_active != 0 && ops->start_listener != 0 &&
         ops->stop_listener != 0 && ops->write_ccc != 0 &&
         ops->wait_complete != 0;
}

polar_sdk_gatt_notify_result_t polar_sdk_gatt_set_notify(
    const polar_sdk_gatt_notify_ops_t *ops, bool enable,
    uint8_t characteristic_properties, uint8_t notify_property_mask,
    uint8_t indicate_property_mask, uint16_t ccc_none, uint16_t ccc_notify,
    uint16_t ccc_indicate, uint8_t att_success, uint32_t timeout_ms) {
  if (!polar_sdk_gatt_notify_ops_ready(ops)) {
    return POLAR_SDK_GATT_NOTIFY_WRITE_FAILED;
  }
  if (!ops->is_connected_ready(ops->ctx)) {
    return POLAR_SDK_GATT_NOTIFY_NOT_CONNECTED;
  }

  if (enable && !ops->listener_active(ops->ctx)) {
    ops->start_listener(ops->ctx);
  }

  uint16_t ccc_cfg = ccc_none;
  if (enable) {
    if ((characteristic_properties & notify_property_mask) != 0) {
      ccc_cfg = ccc_notify;
    } else if ((characteristic_properties & indicate_property_mask) != 0) {
      ccc_cfg = ccc_indicate;
    } else {
      return POLAR_SDK_GATT_NOTIFY_NO_NOTIFY_PROP;
    }
  }

  if (ops->write_ccc(ops->ctx, ccc_cfg) != 0) {
    return POLAR_SDK_GATT_NOTIFY_WRITE_FAILED;
  }

  uint8_t att_status = att_success;
  if (!ops->wait_complete(ops->ctx, timeout_ms, &att_status)) {
    return POLAR_SDK_GATT_NOTIFY_TIMEOUT;
  }
  if (att_status != att_success) {
    return POLAR_SDK_GATT_NOTIFY_ATT_REJECTED;
  }

  if (!enable && ops->listener_active(ops->ctx)) {
    ops->stop_listener(ops->ctx);
  }

  return POLAR_SDK_GATT_NOTIFY_OK;
}
