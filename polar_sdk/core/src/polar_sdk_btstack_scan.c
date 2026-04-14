// SPDX-License-Identifier: LicenseRef-BTstack
// See NOTICE for license details (non-commercial, RP2 exception available)
#include "polar_sdk_btstack_scan.h"

#include <string.h>

#include "btstack.h"

#include "polar_sdk_btstack_helpers.h"

static bool polar_sdk_btstack_adv_name_contains_pair(const uint8_t *adv_data,
                                                     uint8_t adv_len,
                                                     const char *a,
                                                     const char *b) {
  if (adv_data == 0 || a == 0 || b == 0) {
    return false;
  }

  ad_context_t context;
  for (ad_iterator_init(&context, adv_len, adv_data);
       ad_iterator_has_more(&context); ad_iterator_next(&context)) {
    uint8_t data_type = ad_iterator_get_data_type(&context);
    uint8_t data_len = ad_iterator_get_data_len(&context);
    const uint8_t *data = ad_iterator_get_data(&context);

    if (data_type == BLUETOOTH_DATA_TYPE_SHORTENED_LOCAL_NAME ||
        data_type == BLUETOOTH_DATA_TYPE_COMPLETE_LOCAL_NAME) {
      char name[48];
      uint8_t n = data_len < (sizeof(name) - 1) ? data_len : (sizeof(name) - 1);
      memcpy(name, data, n);
      name[n] = '\0';
      return strstr(name, a) != 0 && strstr(name, b) != 0;
    }
  }
  return false;
}

bool polar_sdk_btstack_decode_adv_report(
    uint8_t packet_type, uint8_t *packet,
    polar_sdk_btstack_adv_report_t *out_report) {
  if (packet_type != HCI_EVENT_PACKET || packet == 0 || out_report == 0) {
    return false;
  }
  if (hci_event_packet_get_type(packet) != GAP_EVENT_ADVERTISING_REPORT) {
    return false;
  }

  gap_event_advertising_report_get_address(packet, out_report->addr);
  out_report->addr_type = gap_event_advertising_report_get_address_type(packet);
  out_report->rssi = (int8_t)gap_event_advertising_report_get_rssi(packet);
  out_report->adv_len = gap_event_advertising_report_get_data_length(packet);
  out_report->adv_data = gap_event_advertising_report_get_data(packet);
  return true;
}

bool polar_sdk_btstack_adv_matches_filter(
    const polar_sdk_btstack_scan_filter_t *filter, const uint8_t *adv_addr,
    const uint8_t *adv_data, uint8_t adv_len) {
  if (filter == 0 || adv_addr == 0) {
    return false;
  }

  if (filter->use_addr) {
    return memcmp(adv_addr, filter->addr, 6) == 0;
  }

  if (filter->use_name_prefix) {
    return polar_sdk_btstack_adv_name_matches_prefix(
        adv_data, adv_len, filter->name_prefix, filter->name_prefix_len);
  }

  if (filter->use_name_contains_pair) {
    return polar_sdk_btstack_adv_name_contains_pair(
        adv_data, adv_len, filter->name_contains_a, filter->name_contains_b);
  }

  return true;
}

bool polar_sdk_btstack_adv_prepare_connect(
    polar_sdk_runtime_link_t *link,
    const polar_sdk_btstack_scan_filter_t *filter, const uint8_t *adv_addr,
    const uint8_t *adv_data, uint8_t adv_len) {
  if (!polar_sdk_btstack_adv_matches_filter(filter, adv_addr, adv_data,
                                            adv_len)) {
    return false;
  }
  if (link != 0) {
    link->state = POLAR_SDK_RUNTIME_STATE_CONNECTING;
  }
  return true;
}
