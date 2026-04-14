// SPDX-License-Identifier: MIT
#include "polar_sdk_discovery.h"

static bool polar_sdk_need(uint8_t required_services_mask, uint8_t bit) {
  return (required_services_mask & bit) != 0;
}

bool polar_sdk_discovery_required_services_present(
    uint8_t required_services_mask, bool hr_service_found,
    bool pmd_service_found, bool psftp_service_found, uint8_t service_hr_bit,
    uint8_t service_ecg_bit, uint8_t service_psftp_bit) {
  if (polar_sdk_need(required_services_mask, service_hr_bit) &&
      !hr_service_found) {
    return false;
  }
  if (polar_sdk_need(required_services_mask, service_ecg_bit) &&
      !pmd_service_found) {
    return false;
  }
  if (polar_sdk_need(required_services_mask, service_psftp_bit) &&
      !psftp_service_found) {
    return false;
  }
  return true;
}

bool polar_sdk_discovery_required_handles_ready(
    uint8_t required_services_mask, uint16_t hr_measurement_handle,
    uint16_t pmd_cp_handle, uint16_t pmd_data_handle, uint16_t psftp_mtu_handle,
    uint16_t psftp_d2h_handle, uint16_t psftp_h2d_handle,
    uint8_t service_hr_bit, uint8_t service_ecg_bit,
    uint8_t service_psftp_bit) {
  if (polar_sdk_need(required_services_mask, service_hr_bit) &&
      hr_measurement_handle == 0) {
    return false;
  }
  if (polar_sdk_need(required_services_mask, service_ecg_bit) &&
      (pmd_cp_handle == 0 || pmd_data_handle == 0)) {
    return false;
  }
  if (polar_sdk_need(required_services_mask, service_psftp_bit) &&
      (psftp_mtu_handle == 0 || psftp_d2h_handle == 0 ||
       psftp_h2d_handle == 0)) {
    return false;
  }
  return true;
}

polar_sdk_discovery_action_t polar_sdk_discovery_next_action(
    polar_sdk_discovery_stage_t stage, uint8_t required_services_mask,
    bool hr_service_found, bool pmd_service_found, bool psftp_service_found,
    uint16_t hr_measurement_handle, uint16_t pmd_cp_handle,
    uint16_t pmd_data_handle, uint16_t psftp_mtu_handle,
    uint16_t psftp_d2h_handle, uint16_t psftp_h2d_handle,
    uint8_t service_hr_bit, uint8_t service_ecg_bit, uint8_t service_psftp_bit,
    polar_sdk_discovery_stage_t *out_next_stage) {
  if (out_next_stage != 0) {
    *out_next_stage = stage;
  }

  bool need_hr = polar_sdk_need(required_services_mask, service_hr_bit);
  bool need_ecg = polar_sdk_need(required_services_mask, service_ecg_bit);
  bool need_psftp = polar_sdk_need(required_services_mask, service_psftp_bit);

  bool have_required_services = polar_sdk_discovery_required_services_present(
      required_services_mask, hr_service_found, pmd_service_found,
      psftp_service_found, service_hr_bit, service_ecg_bit, service_psftp_bit);

  bool have_required_handles = polar_sdk_discovery_required_handles_ready(
      required_services_mask, hr_measurement_handle, pmd_cp_handle,
      pmd_data_handle, psftp_mtu_handle, psftp_d2h_handle, psftp_h2d_handle,
      service_hr_bit, service_ecg_bit, service_psftp_bit);

  switch (stage) {
  case POLAR_SDK_DISC_STAGE_SERVICES:
    if (!have_required_services) {
      return POLAR_SDK_DISC_ACTION_FAIL;
    }
    if (need_hr && hr_service_found) {
      if (out_next_stage != 0) {
        *out_next_stage = POLAR_SDK_DISC_STAGE_HR_CHARS;
      }
      return POLAR_SDK_DISC_ACTION_DISCOVER_HR_CHARS;
    }
    if (need_ecg && pmd_service_found) {
      if (out_next_stage != 0) {
        *out_next_stage = POLAR_SDK_DISC_STAGE_PMD_CHARS;
      }
      return POLAR_SDK_DISC_ACTION_DISCOVER_PMD_CHARS;
    }
    if (need_psftp && psftp_service_found) {
      if (out_next_stage != 0) {
        *out_next_stage = POLAR_SDK_DISC_STAGE_PSFTP_CHARS;
      }
      return POLAR_SDK_DISC_ACTION_DISCOVER_PSFTP_CHARS;
    }
    return POLAR_SDK_DISC_ACTION_READY;

  case POLAR_SDK_DISC_STAGE_HR_CHARS:
    if (need_ecg && pmd_service_found) {
      if (out_next_stage != 0) {
        *out_next_stage = POLAR_SDK_DISC_STAGE_PMD_CHARS;
      }
      return POLAR_SDK_DISC_ACTION_DISCOVER_PMD_CHARS;
    }
    if (need_psftp && psftp_service_found) {
      if (out_next_stage != 0) {
        *out_next_stage = POLAR_SDK_DISC_STAGE_PSFTP_CHARS;
      }
      return POLAR_SDK_DISC_ACTION_DISCOVER_PSFTP_CHARS;
    }
    return have_required_handles ? POLAR_SDK_DISC_ACTION_READY
                                 : POLAR_SDK_DISC_ACTION_FAIL;

  case POLAR_SDK_DISC_STAGE_PMD_CHARS:
    if (need_psftp && psftp_service_found) {
      if (out_next_stage != 0) {
        *out_next_stage = POLAR_SDK_DISC_STAGE_PSFTP_CHARS;
      }
      return POLAR_SDK_DISC_ACTION_DISCOVER_PSFTP_CHARS;
    }
    return have_required_handles ? POLAR_SDK_DISC_ACTION_READY
                                 : POLAR_SDK_DISC_ACTION_FAIL;

  case POLAR_SDK_DISC_STAGE_PSFTP_CHARS:
    return have_required_handles ? POLAR_SDK_DISC_ACTION_READY
                                 : POLAR_SDK_DISC_ACTION_FAIL;

  case POLAR_SDK_DISC_STAGE_IDLE:
  default:
    return POLAR_SDK_DISC_ACTION_FAIL;
  }
}
