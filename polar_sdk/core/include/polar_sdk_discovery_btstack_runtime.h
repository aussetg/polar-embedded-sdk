// SPDX-License-Identifier: LicenseRef-BTstack
// See NOTICE for license details (non-commercial, RP2 exception available)
#ifndef POLAR_SDK_DISCOVERY_BTSTACK_RUNTIME_H
#define POLAR_SDK_DISCOVERY_BTSTACK_RUNTIME_H

#include <stdbool.h>
#include <stdint.h>

#include "btstack.h"

#include "polar_sdk_discovery.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  POLAR_SDK_DISCOVERY_BTSTACK_NONE = 0,
  POLAR_SDK_DISCOVERY_BTSTACK_SERVICE_RESULT,
  POLAR_SDK_DISCOVERY_BTSTACK_CHAR_RESULT,
} polar_sdk_discovery_btstack_result_kind_t;

typedef struct {
  polar_sdk_discovery_btstack_result_kind_t kind;
  gatt_client_service_t service;
  gatt_client_characteristic_t characteristic;
} polar_sdk_discovery_btstack_result_t;

bool polar_sdk_discovery_btstack_decode_result(
    uint8_t packet_type, uint8_t *packet, polar_sdk_discovery_stage_t stage,
    polar_sdk_discovery_btstack_result_t *out);

#ifdef __cplusplus
}
#endif

#endif // POLAR_SDK_DISCOVERY_BTSTACK_RUNTIME_H
