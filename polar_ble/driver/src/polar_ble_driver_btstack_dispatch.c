// SPDX-License-Identifier: MIT
#include "polar_ble_driver_btstack_dispatch.h"

bool polar_ble_driver_btstack_dispatch_event(
    uint8_t packet_type,
    uint8_t *packet,
    const polar_ble_driver_btstack_dispatch_ops_t *ops) {
    if (ops == 0) {
        return false;
    }

    polar_ble_driver_btstack_adv_report_t adv_report;
    if (polar_ble_driver_btstack_decode_adv_report(packet_type, packet, &adv_report)) {
        if (ops->on_adv_report) {
            ops->on_adv_report(ops->ctx, &adv_report);
        }
        return true;
    }

    polar_ble_driver_link_event_t link_event;
    if (polar_ble_driver_btstack_decode_link_event(packet_type, packet, &link_event)) {
        if (ops->on_link_event) {
            ops->on_link_event(ops->ctx, &link_event);
        }
        return true;
    }

    polar_ble_driver_sm_event_t sm_event;
    if (polar_ble_driver_btstack_decode_sm_event(packet_type, packet, &sm_event)) {
        if (ops->on_sm_event) {
            ops->on_sm_event(ops->ctx, &sm_event);
        }
        return true;
    }

    return false;
}
