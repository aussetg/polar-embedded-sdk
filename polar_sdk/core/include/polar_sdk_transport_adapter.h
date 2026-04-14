// SPDX-License-Identifier: MIT
#ifndef POLAR_SDK_TRANSPORT_ADAPTER_H
#define POLAR_SDK_TRANSPORT_ADAPTER_H

#include <stdbool.h>
#include <stdint.h>

#include "polar_sdk_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  POLAR_SDK_LINK_EVENT_CONN_COMPLETE = 0,
  POLAR_SDK_LINK_EVENT_CONN_UPDATE_COMPLETE,
  POLAR_SDK_LINK_EVENT_DISCONNECT,
} polar_sdk_link_event_type_t;

typedef struct {
  polar_sdk_link_event_type_t type;
  uint8_t status;
  uint16_t handle;

  // conn complete + conn update
  uint16_t conn_interval;
  uint16_t conn_latency;
  uint16_t supervision_timeout_10ms;

  // disconnect
  uint8_t reason;
} polar_sdk_link_event_t;

bool polar_sdk_transport_adapter_handle_link_event(
    polar_sdk_runtime_link_t *link, uint16_t invalid_conn_handle,
    const polar_sdk_link_event_t *event, bool user_disconnect_requested,
    bool connect_intent);

#ifdef __cplusplus
}
#endif

#endif // POLAR_SDK_TRANSPORT_ADAPTER_H
