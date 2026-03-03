// SPDX-License-Identifier: MIT
#ifndef POLAR_SDK_BTSTACK_LINK_H
#define POLAR_SDK_BTSTACK_LINK_H

#include <stdbool.h>
#include <stdint.h>

#include "polar_sdk_transport_adapter.h"

#ifdef __cplusplus
extern "C" {
#endif

bool polar_sdk_btstack_decode_link_event(
    uint8_t packet_type,
    uint8_t *packet,
    polar_sdk_link_event_t *out_event);

#ifdef __cplusplus
}
#endif

#endif // POLAR_SDK_BTSTACK_LINK_H
