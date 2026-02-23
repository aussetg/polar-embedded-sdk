// SPDX-License-Identifier: MIT
#include "polar_ble_driver_btstack_adv_runtime.h"

bool polar_ble_driver_btstack_adv_runtime_on_report(
    polar_ble_driver_runtime_link_t *link,
    const polar_ble_driver_btstack_scan_filter_t *filter,
    const polar_ble_driver_btstack_adv_report_t *report,
    int hci_success_status,
    const polar_ble_driver_btstack_adv_runtime_ops_t *ops) {
    if (filter == 0 || report == 0 || ops == 0 ||
        ops->is_scanning == 0 ||
        ops->stop_scan == 0 ||
        ops->connect == 0) {
        return false;
    }

    if (!ops->is_scanning(ops->ctx)) {
        return false;
    }

    if (ops->on_report) {
        ops->on_report(ops->ctx, report);
    }

    if (!polar_ble_driver_btstack_adv_prepare_connect(link, filter, report->addr, report->adv_data, report->adv_len)) {
        return true;
    }

    if (ops->on_match) {
        ops->on_match(ops->ctx, report);
    }

    (void)ops->stop_scan(ops->ctx);
    int err = ops->connect(ops->ctx, report->addr, report->addr_type);
    if (err != hci_success_status && ops->on_connect_error) {
        ops->on_connect_error(ops->ctx, err);
    }

    return true;
}
