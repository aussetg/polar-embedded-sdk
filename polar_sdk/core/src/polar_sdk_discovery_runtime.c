// SPDX-License-Identifier: MIT
#include "polar_sdk_discovery_runtime.h"

void polar_sdk_discovery_runtime_on_query_complete(
    const polar_sdk_discovery_snapshot_t *snapshot, uint8_t service_hr_bit,
    uint8_t service_ecg_bit, uint8_t service_psftp_bit, uint8_t att_status,
    uint8_t att_not_found_status, uint8_t hci_success_status,
    const polar_sdk_discovery_runtime_ops_t *ops) {
  if (snapshot == 0 || ops == 0 || ops->set_stage == 0 ||
      ops->mark_ready == 0 || ops->mark_att_fail == 0 ||
      ops->mark_hci_fail == 0 || ops->discover_hr_chars == 0 ||
      ops->discover_pmd_chars == 0 || ops->discover_psftp_chars == 0) {
    return;
  }

  polar_sdk_discovery_step_t step = polar_sdk_discovery_on_query_complete(
      snapshot, service_hr_bit, service_ecg_bit, service_psftp_bit, att_status);

  if (step.result == POLAR_SDK_DISCOVERY_RESULT_FAIL) {
    uint8_t fail_att =
        (att_status == hci_success_status) ? att_not_found_status : att_status;
    ops->mark_att_fail(ops->ctx, fail_att);
    return;
  }

  if (step.result == POLAR_SDK_DISCOVERY_RESULT_READY) {
    ops->mark_ready(ops->ctx);
    return;
  }

  ops->set_stage(ops->ctx, step.next_stage);

  polar_sdk_discovery_dispatch_ops_t dispatch_ops = {
      .ctx = ops->ctx,
      .discover_hr_chars = ops->discover_hr_chars,
      .discover_pmd_chars = ops->discover_pmd_chars,
      .discover_psftp_chars = ops->discover_psftp_chars,
  };
  uint8_t err =
      polar_sdk_discovery_dispatch_command(step.command, &dispatch_ops);
  if (err != hci_success_status) {
    ops->mark_hci_fail(ops->ctx, err);
  }
}

int polar_sdk_discovery_stage_kind(polar_sdk_discovery_stage_t stage) {
  if (stage == POLAR_SDK_DISC_STAGE_SERVICES) {
    return 1;
  }
  if (stage == POLAR_SDK_DISC_STAGE_HR_CHARS ||
      stage == POLAR_SDK_DISC_STAGE_PMD_CHARS ||
      stage == POLAR_SDK_DISC_STAGE_PSFTP_CHARS) {
    return 2;
  }
  return 0;
}
