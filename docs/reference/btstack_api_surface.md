# BTstack API surface used by this SDK

Status: Reference (implementation dependency)
Last updated: 2026-03-03

This document defines the **BTstack subset we rely on** in `polar_sdk/core/` and `polar_sdk/mpy/`.

It is the BTstack counterpart to our Polar protocol references: a small, explicit dependency map that helps with upgrades, debugging, and patch review.

Version policy: see [`../howto/btstack_version_alignment.md`](../howto/btstack_version_alignment.md) (single-version target, currently BTstack 1.8).

## Primary BTstack docs

- BTstack manual root: <https://bluekitchen-gmbh.com/btstack/>
- Architecture (single-threaded, event-driven, non-blocking): <https://bluekitchen-gmbh.com/btstack/architecture/>
- Integration model (run loop/threading): <https://bluekitchen-gmbh.com/btstack/integration/>
- GAP API: <https://bluekitchen-gmbh.com/btstack/appendix/gap/>
- GATT client API: <https://bluekitchen-gmbh.com/btstack/appendix/gatt_client/>
- Security Manager API: <https://bluekitchen-gmbh.com/btstack/appendix/sm/>
- HCI API (event handler registration): <https://bluekitchen-gmbh.com/btstack/appendix/hci/>
- AD parser helpers: <https://bluekitchen-gmbh.com/btstack/appendix/ad_parser/>

Note: the BTstack website documents the current `master` manual (no per-version doc hosting). For this project, always interpret API docs together with our pinned BTstack version policy (currently 1.8).

## Architecture assumptions we depend on

From BTstack architecture/integration docs, our design assumes:

- **Single-threaded, run-loop/event driven model**.
- **No blocking BTstack calls**: operation completion is observed via async events.
- **Packet handlers are the control plane** for scan/connect/discovery/streaming/pairing state.

This is reflected in:
- `polar_sdk/core/src/polar_sdk_btstack_*.c` (event decode/routing helpers),
- `polar_sdk/mpy/mod_polar_sdk.c` (runtime orchestration and waiting loops).

## Dependency tiers

We split dependencies into two tiers to make impact/risk clear during BTstack updates.

### Tier A — SDK core hard dependency (portable core + BTstack adapters)

These are required by `polar_sdk/core/` helpers and decode/routing logic.
If these break, both MicroPython and any pure pico-sdk host integration are affected.

#### A1) Generic event packet helpers

- `HCI_EVENT_PACKET`
- `hci_event_packet_get_type`
- `hci_event_le_meta_get_subevent_code`
- `hci_event_gap_meta_get_subevent_code`

#### A2) GAP event decode surface

Used by:
- `polar_sdk/core/src/polar_sdk_btstack_scan.c`
- `polar_sdk/core/src/polar_sdk_btstack_link.c`

Constants/events:
- `GAP_EVENT_ADVERTISING_REPORT`
- `HCI_EVENT_META_GAP`, `GAP_SUBEVENT_LE_CONNECTION_COMPLETE`
- `HCI_EVENT_LE_META`, `HCI_SUBEVENT_LE_CONNECTION_COMPLETE`
- `HCI_SUBEVENT_LE_CONNECTION_UPDATE_COMPLETE`
- `HCI_EVENT_DISCONNECTION_COMPLETE`

Accessors:
- `gap_event_advertising_report_get_address`
- `gap_event_advertising_report_get_address_type`
- `gap_event_advertising_report_get_rssi`
- `gap_event_advertising_report_get_data_length`
- `gap_event_advertising_report_get_data`
- `gap_subevent_le_connection_complete_get_*`
- `hci_subevent_le_connection_complete_get_*`
- `hci_subevent_le_connection_update_complete_get_*`
- `hci_event_disconnection_complete_get_*`

#### A3) GATT event decode surface

Used by:
- `polar_sdk/core/src/polar_sdk_btstack_gatt.c`
- `polar_sdk/core/src/polar_sdk_btstack_gatt_route.c`
- `polar_sdk/core/src/polar_sdk_discovery_btstack_runtime.c`

Types:
- `gatt_client_service_t`
- `gatt_client_characteristic_t`

Constants/events:
- `GATT_EVENT_MTU`
- `ATT_EVENT_MTU_EXCHANGE_COMPLETE`
- `GATT_EVENT_SERVICE_QUERY_RESULT`
- `GATT_EVENT_CHARACTERISTIC_QUERY_RESULT`
- `GATT_EVENT_QUERY_COMPLETE`
- `GATT_EVENT_NOTIFICATION`
- `GATT_EVENT_INDICATION`

Accessors:
- `gatt_event_mtu_get_*`
- `att_event_mtu_exchange_complete_get_*`
- `gatt_event_service_query_result_get_service`
- `gatt_event_characteristic_query_result_get_characteristic`
- `gatt_event_query_complete_get_att_status`
- `gatt_event_notification_get_*`
- `gatt_event_indication_get_*`

#### A4) Security Manager decode + policy defaults

Used by:
- `polar_sdk/core/src/polar_sdk_btstack_sm.c`

Constants/events:
- `SM_EVENT_JUST_WORKS_REQUEST`
- `SM_EVENT_NUMERIC_COMPARISON_REQUEST`
- `SM_EVENT_AUTHORIZATION_REQUEST`
- `SM_EVENT_PAIRING_COMPLETE`
- `IO_CAPABILITY_NO_INPUT_NO_OUTPUT`
- `SM_AUTHREQ_BONDING`
- `SM_AUTHREQ_SECURE_CONNECTION`

Accessors/functions:
- `sm_event_*_get_*`
- `sm_set_io_capabilities`
- `sm_set_authentication_requirements`

#### A5) AD parser helpers (name filtering)

Used by:
- `polar_sdk/core/src/polar_sdk_btstack_scan.c`
- `polar_sdk/core/src/polar_sdk_btstack_helpers.c`

Types/functions:
- `ad_context_t`
- `ad_iterator_init`, `ad_iterator_has_more`, `ad_iterator_next`
- `ad_iterator_get_data_type`, `ad_iterator_get_data_len`, `ad_iterator_get_data`

Constants:
- `BLUETOOTH_DATA_TYPE_SHORTENED_LOCAL_NAME`
- `BLUETOOTH_DATA_TYPE_COMPLETE_LOCAL_NAME`

#### A6) Common constants

- `ATT_ERROR_SUCCESS`
- `ATT_ERROR_ATTRIBUTE_NOT_FOUND`
- `HCI_CON_HANDLE_INVALID`

### Tier B — MicroPython binding/runtime dependency

These are used by `polar_sdk/mpy/mod_polar_sdk.c` for orchestration, transport control, and exposed feature operations.
If these change, core decode helpers might still compile, but the Python module behavior can break.

#### B1) GAP control/runtime calls

- `gap_set_scan_params`
- `gap_start_scan`, `gap_stop_scan`
- `gap_connect`, `gap_connect_cancel`, `gap_disconnect`
- `gap_update_connection_parameters`
- `gap_encryption_key_size`, `gap_bonded`
- `gap_delete_bonding`

#### B2) GATT client operation calls

Types:
- `gatt_client_notification_t`

Functions:
- `gatt_client_init`
- `gatt_client_mtu_enable_auto_negotiation`
- `gatt_client_send_mtu_negotiation`
- `gatt_client_get_mtu`
- `gatt_client_discover_primary_services`
- `gatt_client_discover_characteristics_for_service`
- `gatt_client_write_client_characteristic_configuration`
- `gatt_client_listen_for_characteristic_value_updates`
- `gatt_client_stop_listening_for_characteristic_value_updates`
- `gatt_client_write_value_of_characteristic`

Related constants:
- `ATT_DEFAULT_MTU`
- `GATT_CLIENT_CHARACTERISTICS_CONFIGURATION_NOTIFICATION`
- `GATT_CLIENT_CHARACTERISTICS_CONFIGURATION_INDICATION`

Integration note:
- On MicroPython BTstack builds with GATT client enabled, `mp_bluetooth_init()` is expected to initialize BTstack GATT client internals.
- This module still enforces its own MTU-negotiation policy via `gatt_client_mtu_enable_auto_negotiation(false)`.

#### B3) Security Manager operation calls

- `sm_add_event_handler`, `sm_remove_event_handler`
- `sm_request_pairing`
- `sm_just_works_confirm`
- `sm_numeric_comparison_confirm`
- `sm_authorization_grant`

Current runtime policy note:
- Authorization requests are auto-granted in this module (embedded/default policy).

#### B4) HCI handler registration

- `hci_add_event_handler`, `hci_remove_event_handler`

#### B5) Misc helpers/constants in binding

- `ERROR_CODE_SUCCESS`
- `sscanf_bd_addr`

## Upgrade-sensitive checkpoints

When bumping BTstack, verify at minimum:

1. **Tier A decode compatibility**
   - Event constants and `*_get_*` accessors used in `polar_sdk/core/src/polar_sdk_btstack_*.c` still exist and match expected payload layout.
2. **LE connection-complete dual-path support**
   - `HCI_EVENT_META_GAP`/`GAP_SUBEVENT_LE_CONNECTION_COMPLETE` and `HCI_EVENT_LE_META`/`HCI_SUBEVENT_LE_CONNECTION_COMPLETE` decode paths remain valid.
3. **GATT query-complete semantics**
   - `GATT_EVENT_QUERY_COMPLETE` and `att_status` semantics unchanged.
4. **SM event payload + policy API**
   - `sm_event_*_get_*`, `sm_set_io_capabilities`, and `sm_set_authentication_requirements` unchanged.
5. **Scan-parameter API compatibility**
   - Verify `gap_set_scan_params(scan_type, interval, window, filter_policy)` signature/semantics remain stable.
6. **Tier B operation APIs**
   - GATT write/CCC/listener APIs and GAP connect/disconnect functions unchanged.

Run both validation paths after any BTstack/API change:
- `examples/pico_sdk` probe build/run
- MicroPython rp2 firmware build + Polar module validation

(See also [`../howto/btstack_version_alignment.md`](../howto/btstack_version_alignment.md) and [`../howto/validation.md`](../howto/validation.md).)
