// SPDX-License-Identifier: MIT
#include "polar_sdk_runtime_context.h"

#include "polar_sdk_transport_adapter.h"

static bool polar_sdk_runtime_context_has_link_ops(
    const polar_sdk_runtime_context_link_ops_t *ops) {
  return ops != 0;
}

bool polar_sdk_runtime_context_handle_link_event(
    polar_sdk_runtime_link_t *link, uint16_t invalid_conn_handle,
    const polar_sdk_link_event_t *event, bool user_disconnect_requested,
    bool connect_intent, const polar_sdk_runtime_context_link_ops_t *ops) {
  if (link == 0 || event == 0) {
    return false;
  }

  bool connected_now = polar_sdk_transport_adapter_handle_link_event(
      link, invalid_conn_handle, event, user_disconnect_requested,
      connect_intent);

  if (event->type == POLAR_SDK_LINK_EVENT_CONN_COMPLETE) {
    if (connected_now && polar_sdk_runtime_context_has_link_ops(ops) &&
        ops->on_connected_ready) {
      ops->on_connected_ready(ops->ctx, event);
    }
    return true;
  }

  if (event->type == POLAR_SDK_LINK_EVENT_DISCONNECT) {
    if (polar_sdk_runtime_context_has_link_ops(ops) && ops->on_disconnected) {
      ops->on_disconnected(ops->ctx, event);
    }
    return true;
  }

  if (event->type == POLAR_SDK_LINK_EVENT_CONN_UPDATE_COMPLETE) {
    if (polar_sdk_runtime_context_has_link_ops(ops) &&
        ops->on_conn_update_complete) {
      ops->on_conn_update_complete(ops->ctx, event);
    }
    return true;
  }

  return false;
}

static bool polar_sdk_runtime_context_has_cleanup_ops(
    const polar_sdk_runtime_context_cleanup_ops_t *ops) {
  return ops != 0 && ops->now_ms != 0 && ops->sleep_ms != 0;
}

void polar_sdk_runtime_context_cleanup_attempt(
    polar_sdk_runtime_link_t *link, uint16_t invalid_conn_handle,
    uint32_t disconnect_wait_ms, polar_sdk_runtime_state_t recovering_state,
    const polar_sdk_runtime_context_cleanup_ops_t *ops) {
  if (link == 0 || !polar_sdk_runtime_context_has_cleanup_ops(ops)) {
    return;
  }

  if (link->state == POLAR_SDK_RUNTIME_STATE_SCANNING && ops->stop_scan) {
    (void)ops->stop_scan(ops->ctx);
  }
  if (link->state == POLAR_SDK_RUNTIME_STATE_CONNECTING &&
      ops->cancel_connect) {
    (void)ops->cancel_connect(ops->ctx);
  }

  if (link->connected && link->conn_handle != invalid_conn_handle &&
      ops->disconnect) {
    (void)ops->disconnect(ops->ctx, link->conn_handle);

    uint32_t start_ms = ops->now_ms(ops->ctx);
    while ((uint32_t)(ops->now_ms(ops->ctx) - start_ms) < disconnect_wait_ms) {
      if (ops->is_link_down && ops->is_link_down(ops->ctx)) {
        break;
      }
      ops->sleep_ms(ops->ctx, 10);
    }
  }

  if (ops->stop_all_listeners) {
    ops->stop_all_listeners(ops->ctx);
  }
  if (ops->reset_session_state) {
    ops->reset_session_state(ops->ctx);
  }

  link->connected = false;
  link->conn_handle = invalid_conn_handle;
  link->conn_update_pending = false;
  link->state = recovering_state;
}
