// SPDX-License-Identifier: MIT
#include "polar_sdk_gatt_notify_runtime.h"

polar_sdk_gatt_notify_runtime_result_t polar_sdk_gatt_notify_runtime_set(
    const polar_sdk_gatt_notify_runtime_args_t *args) {
  if (args == 0 || args->ops == 0 || !args->has_value_handle) {
    return POLAR_SDK_GATT_NOTIFY_RUNTIME_MISSING_CHAR;
  }

  if (args->properties > UINT8_MAX || args->prop_notify > UINT8_MAX ||
      args->prop_indicate > UINT8_MAX) {
    return POLAR_SDK_GATT_NOTIFY_RUNTIME_WRITE_FAILED;
  }

  uint8_t characteristic_properties = (uint8_t)args->properties;
  uint8_t notify_property_mask = (uint8_t)args->prop_notify;
  uint8_t indicate_property_mask = (uint8_t)args->prop_indicate;

  polar_sdk_gatt_notify_result_t r = polar_sdk_gatt_set_notify(
      args->ops, args->enable, characteristic_properties, notify_property_mask,
      indicate_property_mask, args->ccc_none, args->ccc_notify,
      args->ccc_indicate, args->att_success, args->timeout_ms);

  if (r == POLAR_SDK_GATT_NOTIFY_OK) {
    return POLAR_SDK_GATT_NOTIFY_RUNTIME_OK;
  }
  if (r == POLAR_SDK_GATT_NOTIFY_NOT_CONNECTED) {
    return POLAR_SDK_GATT_NOTIFY_RUNTIME_NOT_CONNECTED;
  }
  if (r == POLAR_SDK_GATT_NOTIFY_NO_NOTIFY_PROP) {
    return POLAR_SDK_GATT_NOTIFY_RUNTIME_NO_NOTIFY_PROP;
  }
  if (r == POLAR_SDK_GATT_NOTIFY_WRITE_FAILED) {
    return POLAR_SDK_GATT_NOTIFY_RUNTIME_WRITE_FAILED;
  }
  if (r == POLAR_SDK_GATT_NOTIFY_TIMEOUT) {
    if (args->cfg_pending) {
      *args->cfg_pending = false;
    }
    if (args->cfg_done) {
      *args->cfg_done = false;
    }
    return POLAR_SDK_GATT_NOTIFY_RUNTIME_TIMEOUT;
  }
  if (r == POLAR_SDK_GATT_NOTIFY_ATT_REJECTED) {
    return POLAR_SDK_GATT_NOTIFY_RUNTIME_ATT_REJECTED;
  }

  return POLAR_SDK_GATT_NOTIFY_RUNTIME_WRITE_FAILED;
}
