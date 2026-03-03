// SPDX-License-Identifier: MIT
#include "polar_sdk_discovery_btstack_runtime.h"

#include "polar_sdk_btstack_gatt.h"
#include "polar_sdk_discovery_runtime.h"

bool polar_sdk_discovery_btstack_decode_result(
    uint8_t packet_type,
    uint8_t *packet,
    polar_sdk_discovery_stage_t stage,
    polar_sdk_discovery_btstack_result_t *out) {
    if (out == 0) {
        return false;
    }

    out->kind = POLAR_SDK_DISCOVERY_BTSTACK_NONE;

    int stage_kind = polar_sdk_discovery_stage_kind(stage);
    if (stage_kind == 1) {
        if (polar_sdk_btstack_decode_service_query_result(packet_type, packet, &out->service)) {
            out->kind = POLAR_SDK_DISCOVERY_BTSTACK_SERVICE_RESULT;
            return true;
        }
        return false;
    }

    if (stage_kind == 2) {
        if (polar_sdk_btstack_decode_characteristic_query_result(packet_type, packet, &out->characteristic)) {
            out->kind = POLAR_SDK_DISCOVERY_BTSTACK_CHAR_RESULT;
            return true;
        }
        return false;
    }

    return false;
}
