// SPDX-License-Identifier: MIT
#include "polar_sdk_ecg.h"

#include <string.h>

static void polar_sdk_ecg_ring_push_bytes(polar_sdk_ecg_ring_t *ring, const uint8_t *data, uint16_t len) {
    if (ring == 0 || ring->storage == 0 || ring->capacity == 0 || data == 0 || len == 0) {
        return;
    }

    if (len >= ring->capacity) {
        uint16_t skip = len - ring->capacity;
        data += skip;
        len = ring->capacity;
        ring->drop_bytes_total += skip;

        if (ring->count > 0) {
            ring->drop_bytes_total += ring->count;
            ring->tail = ring->head;
            ring->count = 0;
        }
    }

    uint16_t free_bytes = ring->capacity - ring->count;
    if (len > free_bytes) {
        uint16_t drop = len - free_bytes;
        ring->tail = (uint16_t)((ring->tail + drop) % ring->capacity);
        ring->count -= drop;
        ring->drop_bytes_total += drop;
    }

    uint16_t first = ring->capacity - ring->head;
    if (first > len) {
        first = len;
    }
    memcpy(&ring->storage[ring->head], data, first);
    uint16_t second = len - first;
    if (second > 0) {
        memcpy(&ring->storage[0], data + first, second);
    }

    ring->head = (uint16_t)((ring->head + len) % ring->capacity);
    ring->count += len;
    if (ring->count > ring->ring_high_water) {
        ring->ring_high_water = ring->count;
    }
}

void polar_sdk_ecg_ring_init(
    polar_sdk_ecg_ring_t *ring,
    uint8_t *storage,
    uint16_t capacity) {
    if (ring == 0) {
        return;
    }
    memset(ring, 0, sizeof(*ring));
    ring->storage = storage;
    ring->capacity = capacity;
}

void polar_sdk_ecg_ring_reset(polar_sdk_ecg_ring_t *ring) {
    if (ring == 0) {
        return;
    }
    ring->head = 0;
    ring->tail = 0;
    ring->count = 0;
}

uint16_t polar_sdk_ecg_ring_available(const polar_sdk_ecg_ring_t *ring) {
    return ring == 0 ? 0 : ring->count;
}

uint16_t polar_sdk_ecg_ring_pop_bytes(
    polar_sdk_ecg_ring_t *ring,
    uint8_t *out,
    uint16_t max_len) {
    if (ring == 0 || out == 0 || max_len == 0) {
        return 0;
    }

    uint16_t n = ring->count;
    if (n > max_len) {
        n = max_len;
    }
    if (n == 0) {
        return 0;
    }

    uint16_t first = ring->capacity - ring->tail;
    if (first > n) {
        first = n;
    }
    memcpy(out, &ring->storage[ring->tail], first);
    uint16_t second = n - first;
    if (second > 0) {
        memcpy(out + first, &ring->storage[0], second);
    }

    ring->tail = (uint16_t)((ring->tail + n) % ring->capacity);
    ring->count -= n;
    return n;
}

void polar_sdk_ecg_parse_pmd_notification(
    polar_sdk_ecg_ring_t *ring,
    uint8_t pmd_measurement_type_ecg,
    const uint8_t *value,
    size_t value_len) {
    if (ring == 0 || value == 0) {
        return;
    }
    if (value_len < 10) {
        ring->parse_errors_total += 1;
        return;
    }

    if (value[0] != pmd_measurement_type_ecg) {
        return;
    }

    uint8_t frame_type_byte = value[9];
    bool compressed = (frame_type_byte & 0x80u) != 0;
    uint8_t frame_type = frame_type_byte & 0x7fu;
    if (compressed || frame_type != 0) {
        ring->parse_errors_total += 1;
        return;
    }

    const uint8_t *payload = value + 10;
    size_t payload_len = value_len - 10;
    if (payload_len == 0) {
        return;
    }

    uint16_t sample_count = (uint16_t)(payload_len / 3u);
    if ((payload_len % 3u) != 0u) {
        ring->parse_errors_total += 1;
    }

    for (uint16_t i = 0; i < sample_count; ++i) {
        uint16_t o = (uint16_t)(i * 3u);
        int32_t sample = (int32_t)((uint32_t)payload[o] | ((uint32_t)payload[o + 1] << 8) | ((uint32_t)payload[o + 2] << 16));
        if ((sample & 0x00800000) != 0) {
            sample |= (int32_t)0xff000000;
        }

        uint8_t packed[4];
        packed[0] = (uint8_t)(sample & 0xff);
        packed[1] = (uint8_t)((sample >> 8) & 0xff);
        packed[2] = (uint8_t)((sample >> 16) & 0xff);
        packed[3] = (uint8_t)((sample >> 24) & 0xff);
        polar_sdk_ecg_ring_push_bytes(ring, packed, 4);
    }

    ring->notifications_total += 1;
    ring->frames_total += 1;
    ring->samples_total += sample_count;
}
