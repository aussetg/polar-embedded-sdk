// SPDX-License-Identifier: MIT
#ifndef POLAR_SDK_RUNTIME_CONTEXT_H
#define POLAR_SDK_RUNTIME_CONTEXT_H

#include <stdbool.h>
#include <stdint.h>

#include "polar_sdk_btstack_link.h"
#include "polar_sdk_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  void *ctx;
  void (*on_connected_ready)(void *ctx, const polar_sdk_link_event_t *event);
  void (*on_disconnected)(void *ctx, const polar_sdk_link_event_t *event);
  void (*on_conn_update_complete)(void *ctx,
                                  const polar_sdk_link_event_t *event);
} polar_sdk_runtime_context_link_ops_t;

bool polar_sdk_runtime_context_handle_link_event(
    polar_sdk_runtime_link_t *link, uint16_t invalid_conn_handle,
    const polar_sdk_link_event_t *event, bool user_disconnect_requested,
    bool connect_intent, const polar_sdk_runtime_context_link_ops_t *ops);

typedef struct {
  void *ctx;
  uint32_t (*now_ms)(void *ctx);
  void (*sleep_ms)(void *ctx, uint32_t ms);

  int (*stop_scan)(void *ctx);
  int (*cancel_connect)(void *ctx);
  int (*disconnect)(void *ctx, uint16_t conn_handle);
  bool (*is_link_down)(void *ctx);

  void (*stop_all_listeners)(void *ctx);
  void (*reset_session_state)(void *ctx);
} polar_sdk_runtime_context_cleanup_ops_t;

void polar_sdk_runtime_context_cleanup_attempt(
    polar_sdk_runtime_link_t *link, uint16_t invalid_conn_handle,
    uint32_t disconnect_wait_ms, polar_sdk_runtime_state_t recovering_state,
    const polar_sdk_runtime_context_cleanup_ops_t *ops);

#ifdef __cplusplus
}
#endif

#endif // POLAR_SDK_RUNTIME_CONTEXT_H
