// SPDX-License-Identifier: MIT
#ifndef POLAR_SDK_DISCOVERY_RUNTIME_H
#define POLAR_SDK_DISCOVERY_RUNTIME_H

#include <stdint.h>

#include "polar_sdk_discovery_dispatch.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    void *ctx;
    void (*set_stage)(void *ctx, polar_sdk_discovery_stage_t stage);
    void (*mark_ready)(void *ctx);
    void (*mark_att_fail)(void *ctx, uint8_t att_status);
    void (*mark_hci_fail)(void *ctx, uint8_t hci_status);

    uint8_t (*discover_hr_chars)(void *ctx);
    uint8_t (*discover_pmd_chars)(void *ctx);
    uint8_t (*discover_psftp_chars)(void *ctx);
} polar_sdk_discovery_runtime_ops_t;

void polar_sdk_discovery_runtime_on_query_complete(
    const polar_sdk_discovery_snapshot_t *snapshot,
    uint8_t service_hr_bit,
    uint8_t service_ecg_bit,
    uint8_t service_psftp_bit,
    uint8_t att_status,
    uint8_t att_not_found_status,
    uint8_t hci_success_status,
    const polar_sdk_discovery_runtime_ops_t *ops);

int polar_sdk_discovery_stage_kind(polar_sdk_discovery_stage_t stage);

#ifdef __cplusplus
}
#endif

#endif // POLAR_SDK_DISCOVERY_RUNTIME_H
