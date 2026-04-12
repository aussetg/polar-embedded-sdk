// SPDX-License-Identifier: LicenseRef-BTstack
// See NOTICE for license details (non-commercial, RP2 exception available)
#include "polar_sdk_btstack_sm.h"

#include "btstack.h"

bool polar_sdk_btstack_decode_sm_event(
    uint8_t packet_type,
    uint8_t *packet,
    polar_sdk_sm_event_t *out_event) {
    if (packet_type != HCI_EVENT_PACKET || packet == 0 || out_event == 0) {
        return false;
    }

    out_event->type = POLAR_SDK_SM_EVENT_NONE;
    out_event->handle = 0;
    out_event->status = 0;
    out_event->reason = 0;

    uint8_t event = hci_event_packet_get_type(packet);

    if (event == SM_EVENT_JUST_WORKS_REQUEST) {
        out_event->type = POLAR_SDK_SM_EVENT_JUST_WORKS_REQUEST;
        out_event->handle = sm_event_just_works_request_get_handle(packet);
        return true;
    }
    if (event == SM_EVENT_NUMERIC_COMPARISON_REQUEST) {
        out_event->type = POLAR_SDK_SM_EVENT_NUMERIC_COMPARISON_REQUEST;
        out_event->handle = sm_event_numeric_comparison_request_get_handle(packet);
        return true;
    }
    if (event == SM_EVENT_AUTHORIZATION_REQUEST) {
        out_event->type = POLAR_SDK_SM_EVENT_AUTHORIZATION_REQUEST;
        out_event->handle = sm_event_authorization_request_get_handle(packet);
        return true;
    }
    if (event == SM_EVENT_PAIRING_COMPLETE) {
        out_event->type = POLAR_SDK_SM_EVENT_PAIRING_COMPLETE;
        out_event->handle = sm_event_pairing_complete_get_handle(packet);
        out_event->status = sm_event_pairing_complete_get_status(packet);
        out_event->reason = sm_event_pairing_complete_get_reason(packet);
        return true;
    }

    return false;
}

bool polar_sdk_sm_event_matches_handle(
    const polar_sdk_sm_event_t *event,
    uint16_t active_handle,
    uint16_t invalid_conn_handle) {
    if (event == 0) {
        return false;
    }
    return active_handle == invalid_conn_handle || event->handle == active_handle;
}

void polar_sdk_btstack_sm_configure_default_central_policy(void) {
    sm_set_io_capabilities(IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
    uint8_t auth = SM_AUTHREQ_BONDING;
#if defined(ENABLE_LE_SECURE_CONNECTIONS)
    auth |= SM_AUTHREQ_SECURE_CONNECTION;
#endif
    sm_set_authentication_requirements(auth);
}
