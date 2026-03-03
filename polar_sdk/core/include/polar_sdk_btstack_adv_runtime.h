// SPDX-License-Identifier: MIT
#ifndef POLAR_SDK_BTSTACK_ADV_RUNTIME_H
#define POLAR_SDK_BTSTACK_ADV_RUNTIME_H

#include <stdbool.h>
#include <stdint.h>

#include "polar_sdk_btstack_scan.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    void *ctx;
    bool (*is_scanning)(void *ctx);
    void (*on_report)(void *ctx, const polar_sdk_btstack_adv_report_t *report);
    void (*on_match)(void *ctx, const polar_sdk_btstack_adv_report_t *report);
    int (*stop_scan)(void *ctx);
    int (*connect)(void *ctx, const uint8_t *addr, uint8_t addr_type);
    void (*on_connect_error)(void *ctx, int status);
} polar_sdk_btstack_adv_runtime_ops_t;

bool polar_sdk_btstack_adv_runtime_on_report(
    polar_sdk_runtime_link_t *link,
    const polar_sdk_btstack_scan_filter_t *filter,
    const polar_sdk_btstack_adv_report_t *report,
    int hci_success_status,
    const polar_sdk_btstack_adv_runtime_ops_t *ops);

#ifdef __cplusplus
}
#endif

#endif // POLAR_SDK_BTSTACK_ADV_RUNTIME_H
