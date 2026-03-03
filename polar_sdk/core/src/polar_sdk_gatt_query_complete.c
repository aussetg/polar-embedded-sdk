// SPDX-License-Identifier: MIT
#include "polar_sdk_gatt_query_complete.h"

bool polar_sdk_gatt_apply_query_complete(
    uint8_t query_complete_att_status,
    const polar_sdk_gatt_query_slot_t *slots,
    size_t slot_count,
    uint8_t *last_att_status) {
    if (slots == 0 || slot_count == 0) {
        return false;
    }

    for (size_t i = 0; i < slot_count; ++i) {
        const polar_sdk_gatt_query_slot_t *slot = &slots[i];
        if (slot->pending == 0 || slot->done == 0 || slot->att_status == 0) {
            continue;
        }
        if (!(*slot->pending)) {
            continue;
        }

        *slot->att_status = query_complete_att_status;
        *slot->pending = false;
        *slot->done = true;

        if (slot->update_last_att_status && last_att_status) {
            *last_att_status = query_complete_att_status;
        }
        return true;
    }

    return false;
}
