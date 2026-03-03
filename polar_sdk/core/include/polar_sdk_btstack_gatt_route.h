// SPDX-License-Identifier: MIT
#ifndef POLAR_SDK_BTSTACK_GATT_ROUTE_H
#define POLAR_SDK_BTSTACK_GATT_ROUTE_H

#include <stdbool.h>
#include <stdint.h>

#include "polar_sdk_btstack_gatt.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    POLAR_SDK_GATT_ROUTE_NONE = 0,
    POLAR_SDK_GATT_ROUTE_MTU_EVENT,
    POLAR_SDK_GATT_ROUTE_HR_VALUE,
    POLAR_SDK_GATT_ROUTE_PMD_CP_VALUE,
    POLAR_SDK_GATT_ROUTE_PMD_DATA_VALUE,
    POLAR_SDK_GATT_ROUTE_PSFTP_MTU_VALUE,
    POLAR_SDK_GATT_ROUTE_PSFTP_D2H_VALUE,
    POLAR_SDK_GATT_ROUTE_HR_UNMATCHED_VALUE,
    POLAR_SDK_GATT_ROUTE_QUERY_COMPLETE,
} polar_sdk_btstack_gatt_route_kind_t;

typedef struct {
    uint16_t conn_handle;
    bool connected;

    uint16_t hr_value_handle;
    bool hr_enabled;

    uint16_t pmd_cp_value_handle;
    bool pmd_cp_listening;

    uint16_t pmd_data_value_handle;
    bool ecg_enabled;

    uint16_t psftp_mtu_value_handle;
    bool psftp_mtu_listening;

    uint16_t psftp_d2h_value_handle;
    bool psftp_d2h_listening;
} polar_sdk_btstack_gatt_route_state_t;

typedef struct {
    polar_sdk_btstack_gatt_route_kind_t kind;
    uint8_t query_complete_att_status;
    polar_sdk_btstack_mtu_event_t mtu;
    polar_sdk_btstack_value_event_t value;
} polar_sdk_btstack_gatt_route_result_t;

bool polar_sdk_btstack_route_gatt_event(
    uint8_t packet_type,
    uint8_t *packet,
    const polar_sdk_btstack_gatt_route_state_t *state,
    polar_sdk_btstack_gatt_route_result_t *out);

#ifdef __cplusplus
}
#endif

#endif // POLAR_SDK_BTSTACK_GATT_ROUTE_H
