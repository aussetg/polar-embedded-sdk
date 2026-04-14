// SPDX-License-Identifier: MIT
#ifndef POLAR_SDK_IMU_H
#define POLAR_SDK_IMU_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  uint8_t *storage;
  uint16_t capacity;
  uint16_t head;
  uint16_t tail;
  uint16_t count;

  uint32_t notifications_total;
  uint32_t frames_total;
  uint32_t samples_total;
  uint32_t parse_errors_total;
  uint32_t drop_bytes_total;
  uint32_t ring_high_water;
} polar_sdk_imu_ring_t;

void polar_sdk_imu_ring_init(polar_sdk_imu_ring_t *ring, uint8_t *storage,
                             uint16_t capacity);
void polar_sdk_imu_ring_reset(polar_sdk_imu_ring_t *ring);
uint16_t polar_sdk_imu_ring_available(const polar_sdk_imu_ring_t *ring);
uint16_t polar_sdk_imu_ring_pop_bytes(polar_sdk_imu_ring_t *ring, uint8_t *out,
                                      uint16_t max_len);

void polar_sdk_imu_parse_pmd_notification(polar_sdk_imu_ring_t *ring,
                                          uint8_t pmd_measurement_type_acc,
                                          const uint8_t *value,
                                          size_t value_len);

#ifdef __cplusplus
}
#endif

#endif // POLAR_SDK_IMU_H
