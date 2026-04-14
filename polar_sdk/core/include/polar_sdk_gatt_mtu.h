// SPDX-License-Identifier: MIT
#ifndef POLAR_SDK_GATT_MTU_H
#define POLAR_SDK_GATT_MTU_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  POLAR_SDK_GATT_MTU_RESULT_OK = 0,
  POLAR_SDK_GATT_MTU_RESULT_NOT_CONNECTED,
  POLAR_SDK_GATT_MTU_RESULT_TIMEOUT,
  POLAR_SDK_GATT_MTU_RESULT_TRANSPORT,
} polar_sdk_gatt_mtu_result_t;

typedef struct {
  void *ctx;
  bool (*is_connected)(void *ctx);

  // Return 0 on success and set *out_mtu.
  int (*read_mtu)(void *ctx, uint16_t *out_mtu);

  // Return 0 if MTU exchange request was submitted.
  int (*request_exchange)(void *ctx);

  // Return true once MTU exchange has completed.
  bool (*wait_exchange_complete)(void *ctx, uint32_t timeout_ms);

  // Return latest/negotiated ATT MTU.
  uint16_t (*current_mtu)(void *ctx);
} polar_sdk_gatt_mtu_ops_t;

polar_sdk_gatt_mtu_result_t
polar_sdk_gatt_mtu_ensure_minimum(const polar_sdk_gatt_mtu_ops_t *ops,
                                  uint16_t minimum_mtu, uint32_t timeout_ms,
                                  uint16_t *out_mtu);

#ifdef __cplusplus
}
#endif

#endif // POLAR_SDK_GATT_MTU_H
