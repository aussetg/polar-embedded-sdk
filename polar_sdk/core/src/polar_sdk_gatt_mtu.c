// SPDX-License-Identifier: MIT
#include "polar_sdk_gatt_mtu.h"

static bool polar_sdk_gatt_mtu_ops_ready(const polar_sdk_gatt_mtu_ops_t *ops) {
  return ops != 0 && ops->is_connected != 0 && ops->read_mtu != 0 &&
         ops->request_exchange != 0 && ops->wait_exchange_complete != 0 &&
         ops->current_mtu != 0;
}

polar_sdk_gatt_mtu_result_t
polar_sdk_gatt_mtu_ensure_minimum(const polar_sdk_gatt_mtu_ops_t *ops,
                                  uint16_t minimum_mtu, uint32_t timeout_ms,
                                  uint16_t *out_mtu) {
  if (!polar_sdk_gatt_mtu_ops_ready(ops)) {
    return POLAR_SDK_GATT_MTU_RESULT_TRANSPORT;
  }

  if (!ops->is_connected(ops->ctx)) {
    return POLAR_SDK_GATT_MTU_RESULT_NOT_CONNECTED;
  }

  uint16_t mtu = ops->current_mtu(ops->ctx);
  uint16_t read_mtu = mtu;
  if (ops->read_mtu(ops->ctx, &read_mtu) == 0) {
    mtu = read_mtu;
  }

  if (out_mtu) {
    *out_mtu = mtu;
  }
  if (mtu >= minimum_mtu) {
    return POLAR_SDK_GATT_MTU_RESULT_OK;
  }

  if (ops->request_exchange(ops->ctx) != 0) {
    return POLAR_SDK_GATT_MTU_RESULT_TRANSPORT;
  }

  if (!ops->wait_exchange_complete(ops->ctx, timeout_ms)) {
    return POLAR_SDK_GATT_MTU_RESULT_TIMEOUT;
  }

  mtu = ops->current_mtu(ops->ctx);
  if (out_mtu) {
    *out_mtu = mtu;
  }

  return mtu >= minimum_mtu ? POLAR_SDK_GATT_MTU_RESULT_OK
                            : POLAR_SDK_GATT_MTU_RESULT_TIMEOUT;
}
