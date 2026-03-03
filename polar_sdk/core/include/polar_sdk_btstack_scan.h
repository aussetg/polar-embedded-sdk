// SPDX-License-Identifier: MIT
#ifndef POLAR_SDK_BTSTACK_SCAN_H
#define POLAR_SDK_BTSTACK_SCAN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "polar_sdk_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool use_addr;
    uint8_t addr[6];

    bool use_name_prefix;
    const uint8_t *name_prefix;
    size_t name_prefix_len;

    bool use_name_contains_pair;
    const char *name_contains_a;
    const char *name_contains_b;
} polar_sdk_btstack_scan_filter_t;

typedef struct {
    uint8_t addr[6];
    uint8_t addr_type;
    int8_t rssi;
    const uint8_t *adv_data;
    uint8_t adv_len;
} polar_sdk_btstack_adv_report_t;

bool polar_sdk_btstack_decode_adv_report(
    uint8_t packet_type,
    uint8_t *packet,
    polar_sdk_btstack_adv_report_t *out_report);

bool polar_sdk_btstack_adv_matches_filter(
    const polar_sdk_btstack_scan_filter_t *filter,
    const uint8_t *adv_addr,
    const uint8_t *adv_data,
    uint8_t adv_len);

bool polar_sdk_btstack_adv_prepare_connect(
    polar_sdk_runtime_link_t *link,
    const polar_sdk_btstack_scan_filter_t *filter,
    const uint8_t *adv_addr,
    const uint8_t *adv_data,
    uint8_t adv_len);

#ifdef __cplusplus
}
#endif

#endif // POLAR_SDK_BTSTACK_SCAN_H
