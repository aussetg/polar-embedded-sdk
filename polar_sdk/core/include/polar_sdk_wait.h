// SPDX-License-Identifier: MIT
#ifndef POLAR_SDK_WAIT_H
#define POLAR_SDK_WAIT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  void *ctx;
  uint32_t (*now_ms)(void *ctx);
  void (*sleep_ms)(void *ctx, uint32_t ms);
  bool (*is_done)(void *ctx);
  bool (*is_connected)(void *ctx);
} polar_sdk_wait_ops_t;

bool polar_sdk_wait_until_done_or_disconnect(const polar_sdk_wait_ops_t *ops,
                                             uint32_t timeout_ms,
                                             uint32_t poll_interval_ms);

#ifdef __cplusplus
}
#endif

#endif // POLAR_SDK_WAIT_H
