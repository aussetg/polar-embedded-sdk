// SPDX-License-Identifier: MIT
#include "polar_ble_driver_hr.h"

#include <string.h>

static uint16_t polar_ble_driver_hr_rr_raw_to_ms(uint16_t rr_raw) {
    return (uint16_t)((rr_raw * 1000u + 512u) / 1024u);
}

void polar_ble_driver_hr_reset(polar_ble_driver_hr_state_t *state) {
    if (state == 0) {
        return;
    }
    memset(state, 0, sizeof(*state));
}

bool polar_ble_driver_hr_parse_measurement(
    polar_ble_driver_hr_state_t *state,
    const uint8_t *value,
    size_t value_len,
    uint32_t now_ms) {
    if (state == 0 || value == 0 || value_len < 2) {
        if (state != 0) {
            state->parse_errors_total += 1;
        }
        return false;
    }

    uint8_t flags = value[0];
    size_t offset = 1;
    uint16_t bpm = 0;

    if ((flags & 0x01u) != 0) {
        if (value_len < 3) {
            state->parse_errors_total += 1;
            return false;
        }
        bpm = (uint16_t)value[offset] | ((uint16_t)value[offset + 1] << 8);
        offset += 2;
    } else {
        bpm = value[offset];
        offset += 1;
    }

    uint8_t contact = 0;
    if ((flags & 0x06u) == 0x06u) {
        contact = 1;
    }

    if ((flags & 0x08u) != 0) {
        if (offset + 1 >= value_len) {
            state->parse_errors_total += 1;
            return false;
        }
        offset += 2;
    }

    uint8_t rr_count = 0;
    uint16_t rr_ms[4] = {0, 0, 0, 0};
    if ((flags & 0x10u) != 0) {
        while (offset + 1 < value_len && rr_count < 4) {
            uint16_t rr_raw = (uint16_t)value[offset] | ((uint16_t)value[offset + 1] << 8);
            offset += 2;
            rr_ms[rr_count] = polar_ble_driver_hr_rr_raw_to_ms(rr_raw);
            rr_count += 1;
        }
    }

    state->last_ts_ms = now_ms;
    state->last_bpm = bpm;
    state->last_contact = contact;
    state->last_flags = flags;
    state->last_rr_count = rr_count;
    for (size_t i = 0; i < 4; ++i) {
        state->last_rr_ms[i] = rr_ms[i];
    }

    state->sample_seq += 1;
    return true;
}
