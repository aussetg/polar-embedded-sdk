// SPDX-License-Identifier: MIT
#include "polar_sdk_discovery_orchestrator.h"

polar_sdk_discovery_step_t polar_sdk_discovery_on_query_complete(
    const polar_sdk_discovery_snapshot_t *snapshot,
    uint8_t service_hr_bit,
    uint8_t service_ecg_bit,
    uint8_t service_psftp_bit,
    uint8_t att_status) {
    polar_sdk_discovery_step_t step = {
        .result = POLAR_SDK_DISCOVERY_RESULT_FAIL,
        .next_stage = POLAR_SDK_DISC_STAGE_IDLE,
        .command = POLAR_SDK_DISCOVERY_CMD_NONE,
    };

    if (snapshot == 0) {
        return step;
    }

    step.next_stage = snapshot->stage;

    if (att_status != 0) {
        return step;
    }

    polar_sdk_discovery_stage_t next_stage = snapshot->stage;
    polar_sdk_discovery_action_t action = polar_sdk_discovery_next_action(
        snapshot->stage,
        snapshot->required_services_mask,
        snapshot->hr_service_found,
        snapshot->pmd_service_found,
        snapshot->psftp_service_found,
        snapshot->hr_measurement_handle,
        snapshot->pmd_cp_handle,
        snapshot->pmd_data_handle,
        snapshot->psftp_mtu_handle,
        snapshot->psftp_d2h_handle,
        snapshot->psftp_h2d_handle,
        service_hr_bit,
        service_ecg_bit,
        service_psftp_bit,
        &next_stage);

    step.next_stage = next_stage;

    if (action == POLAR_SDK_DISC_ACTION_FAIL) {
        step.result = POLAR_SDK_DISCOVERY_RESULT_FAIL;
        return step;
    }
    if (action == POLAR_SDK_DISC_ACTION_READY) {
        step.result = POLAR_SDK_DISCOVERY_RESULT_READY;
        return step;
    }

    step.result = POLAR_SDK_DISCOVERY_RESULT_PROGRESS;
    if (action == POLAR_SDK_DISC_ACTION_DISCOVER_HR_CHARS) {
        step.command = POLAR_SDK_DISCOVERY_CMD_DISCOVER_HR_CHARS;
    } else if (action == POLAR_SDK_DISC_ACTION_DISCOVER_PMD_CHARS) {
        step.command = POLAR_SDK_DISCOVERY_CMD_DISCOVER_PMD_CHARS;
    } else if (action == POLAR_SDK_DISC_ACTION_DISCOVER_PSFTP_CHARS) {
        step.command = POLAR_SDK_DISCOVERY_CMD_DISCOVER_PSFTP_CHARS;
    }

    return step;
}
