// SPDX-License-Identifier: MIT
#ifndef POLAR_SDK_DISCOVERY_ORCHESTRATOR_H
#define POLAR_SDK_DISCOVERY_ORCHESTRATOR_H

#include <stdbool.h>
#include <stdint.h>

#include "polar_sdk_discovery.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  POLAR_SDK_DISCOVERY_RESULT_PROGRESS = 0,
  POLAR_SDK_DISCOVERY_RESULT_READY,
  POLAR_SDK_DISCOVERY_RESULT_FAIL,
} polar_sdk_discovery_result_t;

typedef struct {
  polar_sdk_discovery_stage_t stage;
  uint8_t required_services_mask;

  bool hr_service_found;
  bool pmd_service_found;
  bool psftp_service_found;

  uint16_t hr_measurement_handle;
  uint16_t pmd_cp_handle;
  uint16_t pmd_data_handle;
  uint16_t psftp_mtu_handle;
  uint16_t psftp_d2h_handle;
  uint16_t psftp_h2d_handle;
} polar_sdk_discovery_snapshot_t;

typedef enum {
  POLAR_SDK_DISCOVERY_CMD_NONE = 0,
  POLAR_SDK_DISCOVERY_CMD_DISCOVER_HR_CHARS,
  POLAR_SDK_DISCOVERY_CMD_DISCOVER_PMD_CHARS,
  POLAR_SDK_DISCOVERY_CMD_DISCOVER_PSFTP_CHARS,
} polar_sdk_discovery_command_t;

typedef struct {
  polar_sdk_discovery_result_t result;
  polar_sdk_discovery_stage_t next_stage;
  polar_sdk_discovery_command_t command;
} polar_sdk_discovery_step_t;

polar_sdk_discovery_step_t polar_sdk_discovery_on_query_complete(
    const polar_sdk_discovery_snapshot_t *snapshot, uint8_t service_hr_bit,
    uint8_t service_ecg_bit, uint8_t service_psftp_bit, uint8_t att_status);

#ifdef __cplusplus
}
#endif

#endif // POLAR_SDK_DISCOVERY_ORCHESTRATOR_H
