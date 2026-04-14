// SPDX-License-Identifier: MIT
#ifndef POLAR_SDK_BTSTACK_DISPATCH_H
#define POLAR_SDK_BTSTACK_DISPATCH_H

#include <stdbool.h>
#include <stdint.h>

#include "polar_sdk_btstack_link.h"
#include "polar_sdk_btstack_scan.h"
#include "polar_sdk_btstack_sm.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  void *ctx;
  void (*on_adv_report)(void *ctx,
                        const polar_sdk_btstack_adv_report_t *report);
  void (*on_link_event)(void *ctx, const polar_sdk_link_event_t *event);
  void (*on_sm_event)(void *ctx, const polar_sdk_sm_event_t *event);
} polar_sdk_btstack_dispatch_ops_t;

bool polar_sdk_btstack_dispatch_event(
    uint8_t packet_type, uint8_t *packet,
    const polar_sdk_btstack_dispatch_ops_t *ops);

#ifdef __cplusplus
}
#endif

#endif // POLAR_SDK_BTSTACK_DISPATCH_H
