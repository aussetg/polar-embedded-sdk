// SPDX-License-Identifier: LicenseRef-BTstack
// See NOTICE for license details (non-commercial, RP2 exception available)
#include "polar_sdk_btstack_gatt.h"

#include "btstack.h"

bool polar_sdk_btstack_decode_mtu_event(
    uint8_t packet_type,
    uint8_t *packet,
    polar_sdk_btstack_mtu_event_t *out) {
    if (packet_type != HCI_EVENT_PACKET || packet == 0 || out == 0) {
        return false;
    }

    uint8_t event = hci_event_packet_get_type(packet);
    if (event == GATT_EVENT_MTU) {
        out->handle = gatt_event_mtu_get_handle(packet);
        out->mtu = gatt_event_mtu_get_MTU(packet);
        return true;
    }
    if (event == ATT_EVENT_MTU_EXCHANGE_COMPLETE) {
        out->handle = att_event_mtu_exchange_complete_get_handle(packet);
        out->mtu = att_event_mtu_exchange_complete_get_MTU(packet);
        return true;
    }
    return false;
}

bool polar_sdk_btstack_decode_value_event(
    uint8_t packet_type,
    uint8_t *packet,
    polar_sdk_btstack_value_event_t *out) {
    if (packet_type != HCI_EVENT_PACKET || packet == 0 || out == 0) {
        return false;
    }

    uint8_t event = hci_event_packet_get_type(packet);
    if (event == GATT_EVENT_NOTIFICATION) {
        out->notification = true;
        out->handle = gatt_event_notification_get_handle(packet);
        out->value_handle = gatt_event_notification_get_value_handle(packet);
        out->value = gatt_event_notification_get_value(packet);
        out->value_len = gatt_event_notification_get_value_length(packet);
        return true;
    }
    if (event == GATT_EVENT_INDICATION) {
        out->notification = false;
        out->handle = gatt_event_indication_get_handle(packet);
        out->value_handle = gatt_event_indication_get_value_handle(packet);
        out->value = gatt_event_indication_get_value(packet);
        out->value_len = gatt_event_indication_get_value_length(packet);
        return true;
    }
    return false;
}

bool polar_sdk_btstack_decode_query_complete_att_status(
    uint8_t packet_type,
    uint8_t *packet,
    uint8_t *out_att_status) {
    if (packet_type != HCI_EVENT_PACKET || packet == 0 || out_att_status == 0) {
        return false;
    }
    if (hci_event_packet_get_type(packet) != GATT_EVENT_QUERY_COMPLETE) {
        return false;
    }
    *out_att_status = gatt_event_query_complete_get_att_status(packet);
    return true;
}

bool polar_sdk_btstack_decode_service_query_result(
    uint8_t packet_type,
    uint8_t *packet,
    void *out_service) {
    if (packet_type != HCI_EVENT_PACKET || packet == 0 || out_service == 0) {
        return false;
    }
    if (hci_event_packet_get_type(packet) != GATT_EVENT_SERVICE_QUERY_RESULT) {
        return false;
    }
    gatt_event_service_query_result_get_service(packet, (gatt_client_service_t *)out_service);
    return true;
}

bool polar_sdk_btstack_decode_characteristic_query_result(
    uint8_t packet_type,
    uint8_t *packet,
    void *out_characteristic) {
    if (packet_type != HCI_EVENT_PACKET || packet == 0 || out_characteristic == 0) {
        return false;
    }
    if (hci_event_packet_get_type(packet) != GATT_EVENT_CHARACTERISTIC_QUERY_RESULT) {
        return false;
    }
    gatt_event_characteristic_query_result_get_characteristic(packet, (gatt_client_characteristic_t *)out_characteristic);
    return true;
}
