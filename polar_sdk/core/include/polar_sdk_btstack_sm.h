// SPDX-License-Identifier: MIT
#ifndef POLAR_SDK_BTSTACK_SM_H
#define POLAR_SDK_BTSTACK_SM_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  POLAR_SDK_SM_EVENT_NONE = 0,
  POLAR_SDK_SM_EVENT_JUST_WORKS_REQUEST,
  POLAR_SDK_SM_EVENT_NUMERIC_COMPARISON_REQUEST,
  POLAR_SDK_SM_EVENT_AUTHORIZATION_REQUEST,
  POLAR_SDK_SM_EVENT_PAIRING_COMPLETE,
} polar_sdk_sm_event_type_t;

typedef struct {
  polar_sdk_sm_event_type_t type;
  uint16_t handle;
  uint8_t status;
  uint8_t reason;
} polar_sdk_sm_event_t;

bool polar_sdk_btstack_decode_sm_event(uint8_t packet_type, uint8_t *packet,
                                       polar_sdk_sm_event_t *out_event);

bool polar_sdk_sm_event_matches_handle(const polar_sdk_sm_event_t *event,
                                       uint16_t active_handle,
                                       uint16_t invalid_conn_handle);

// Configure project-default Security Manager settings for central/client role.
// Call during BTstack bring-up so later pairing requests all inherit the same
// bonding + secure-connections policy.
void polar_sdk_btstack_sm_configure_default_central_policy(void);

#ifdef __cplusplus
}
#endif

#endif // POLAR_SDK_BTSTACK_SM_H
