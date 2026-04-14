// SPDX-License-Identifier: LicenseRef-BTstack
// See NOTICE for license details (non-commercial, RP2 exception available)
#include "polar_sdk_btstack_link.h"

#include "btstack.h"

bool polar_sdk_btstack_decode_link_event(uint8_t packet_type, uint8_t *packet,
                                         polar_sdk_link_event_t *out_event) {
  if (packet_type != HCI_EVENT_PACKET || packet == 0 || out_event == 0) {
    return false;
  }

  uint8_t event = hci_event_packet_get_type(packet);

  if (event == HCI_EVENT_META_GAP) {
#if defined(GAP_SUBEVENT_LE_CONNECTION_COMPLETE)
    if (hci_event_gap_meta_get_subevent_code(packet) ==
        GAP_SUBEVENT_LE_CONNECTION_COMPLETE) {
      out_event->type = POLAR_SDK_LINK_EVENT_CONN_COMPLETE;
      out_event->status =
          gap_subevent_le_connection_complete_get_status(packet);
      out_event->handle =
          gap_subevent_le_connection_complete_get_connection_handle(packet);
      out_event->conn_interval =
          gap_subevent_le_connection_complete_get_conn_interval(packet);
      out_event->conn_latency =
          gap_subevent_le_connection_complete_get_conn_latency(packet);
      out_event->supervision_timeout_10ms =
          gap_subevent_le_connection_complete_get_supervision_timeout(packet);
      out_event->reason = 0;
      return true;
    }
#endif
    return false;
  }

  if (event == HCI_EVENT_LE_META) {
    uint8_t subevent = hci_event_le_meta_get_subevent_code(packet);
    if (subevent == HCI_SUBEVENT_LE_CONNECTION_COMPLETE) {
      out_event->type = POLAR_SDK_LINK_EVENT_CONN_COMPLETE;
      out_event->status =
          hci_subevent_le_connection_complete_get_status(packet);
      out_event->handle =
          hci_subevent_le_connection_complete_get_connection_handle(packet);
      out_event->conn_interval =
          hci_subevent_le_connection_complete_get_conn_interval(packet);
      out_event->conn_latency =
          hci_subevent_le_connection_complete_get_conn_latency(packet);
      out_event->supervision_timeout_10ms =
          hci_subevent_le_connection_complete_get_supervision_timeout(packet);
      out_event->reason = 0;
      return true;
    }
    if (subevent == HCI_SUBEVENT_LE_CONNECTION_UPDATE_COMPLETE) {
      out_event->type = POLAR_SDK_LINK_EVENT_CONN_UPDATE_COMPLETE;
      out_event->status =
          hci_subevent_le_connection_update_complete_get_status(packet);
      out_event->handle =
          hci_subevent_le_connection_update_complete_get_connection_handle(
              packet);
      out_event->conn_interval =
          hci_subevent_le_connection_update_complete_get_conn_interval(packet);
      out_event->conn_latency =
          hci_subevent_le_connection_update_complete_get_conn_latency(packet);
      out_event->supervision_timeout_10ms =
          hci_subevent_le_connection_update_complete_get_supervision_timeout(
              packet);
      out_event->reason = 0;
      return true;
    }
    return false;
  }

  if (event == HCI_EVENT_DISCONNECTION_COMPLETE) {
    out_event->type = POLAR_SDK_LINK_EVENT_DISCONNECT;
    out_event->status = hci_event_disconnection_complete_get_status(packet);
    out_event->handle =
        hci_event_disconnection_complete_get_connection_handle(packet);
    out_event->conn_interval = 0;
    out_event->conn_latency = 0;
    out_event->supervision_timeout_10ms = 0;
    out_event->reason = hci_event_disconnection_complete_get_reason(packet);
    return true;
  }

  return false;
}
