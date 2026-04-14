// SPDX-License-Identifier: MIT
#ifndef POLAR_SDK_RUNTIME_H
#define POLAR_SDK_RUNTIME_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  POLAR_SDK_RUNTIME_STATE_IDLE = 0,
  POLAR_SDK_RUNTIME_STATE_SCANNING,
  POLAR_SDK_RUNTIME_STATE_CONNECTING,
  POLAR_SDK_RUNTIME_STATE_DISCOVERING,
  POLAR_SDK_RUNTIME_STATE_READY,
  POLAR_SDK_RUNTIME_STATE_RECOVERING,
} polar_sdk_runtime_state_t;

typedef struct {
  polar_sdk_runtime_state_t state;
  bool connected;
  uint16_t conn_handle;
  bool attempt_failed;

  uint8_t last_hci_status;
  uint8_t last_disconnect_status;
  uint8_t last_disconnect_reason;

  uint32_t conn_complete_total;
  uint32_t conn_update_complete_total;

  uint16_t last_conn_interval_units;
  uint16_t last_conn_latency;
  uint16_t last_conn_supervision_timeout_10ms;
  uint8_t last_conn_update_status;
  bool conn_update_pending;

  uint32_t disconnect_events_total;
  uint32_t disconnect_reason_0x08_total;
  uint32_t disconnect_reason_0x3e_total;
  uint32_t disconnect_reason_other_total;
} polar_sdk_runtime_link_t;

void polar_sdk_runtime_link_init(polar_sdk_runtime_link_t *link,
                                 uint16_t invalid_conn_handle);

void polar_sdk_runtime_mark_attempt_failed(polar_sdk_runtime_link_t *link);

void polar_sdk_runtime_on_connection_complete(
    polar_sdk_runtime_link_t *link, uint16_t invalid_conn_handle,
    uint8_t status, uint16_t handle, uint16_t conn_interval,
    uint16_t conn_latency, uint16_t supervision_timeout_10ms);

void polar_sdk_runtime_on_connection_update_complete(
    polar_sdk_runtime_link_t *link, uint8_t status, uint16_t conn_interval,
    uint16_t conn_latency, uint16_t supervision_timeout_10ms);

void polar_sdk_runtime_on_disconnect(polar_sdk_runtime_link_t *link,
                                     uint16_t invalid_conn_handle,
                                     uint8_t status, uint8_t reason,
                                     bool user_disconnect_requested,
                                     bool connect_intent);

#ifdef __cplusplus
}
#endif

#endif // POLAR_SDK_RUNTIME_H
