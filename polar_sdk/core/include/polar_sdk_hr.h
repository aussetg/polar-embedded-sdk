// SPDX-License-Identifier: MIT
#ifndef POLAR_SDK_HR_H
#define POLAR_SDK_HR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  uint32_t sample_seq;
  uint32_t parse_errors_total;

  uint32_t last_ts_ms;
  uint16_t last_bpm;
  uint8_t last_rr_count;
  uint16_t last_rr_ms[4];
  uint8_t last_contact;
  uint8_t last_flags;
} polar_sdk_hr_state_t;

void polar_sdk_hr_reset(polar_sdk_hr_state_t *state);
bool polar_sdk_hr_parse_measurement(polar_sdk_hr_state_t *state,
                                    const uint8_t *value, size_t value_len,
                                    uint32_t now_ms);

#ifdef __cplusplus
}
#endif

#endif // POLAR_SDK_HR_H
