# BTstack protocol mapping for Polar H10 features

Status: Reference
Last updated: 2026-03-03

This document explains **how our Polar features map onto BTstack protocol layers/events**.

It is not a replacement for Polar protocol docs. Instead, it connects:
- Polar feature intent (HR/PMD/PSFTP),
- BTstack protocol/event model (GAP/GATT/ATT/SM),
- our implementation shape in `polar_sdk/core/` + `polar_sdk/mpy/`.

## Upstream context pages

- Architecture: <https://bluekitchen-gmbh.com/btstack/architecture/>
- Protocols overview: <https://bluekitchen-gmbh.com/btstack/protocols/>
- GAP API: <https://bluekitchen-gmbh.com/btstack/appendix/gap/>
- GATT client API: <https://bluekitchen-gmbh.com/btstack/appendix/gatt_client/>
- Security Manager API: <https://bluekitchen-gmbh.com/btstack/appendix/sm/>

Important note:
- BTstack web docs reflect `master`; interpret details with our pinned BTstack version policy.

## Layer model we actually use

- **GAP**: scanning, connect/disconnect, connection parameter updates.
- **GATT/ATT**: service/characteristic discovery, CCC writes, value writes, notifications/indications.
- **SM**: pairing/auth policy for protected operations.
- **HCI event envelope**: event packet type + subevent decode plumbing.

We do **not** use raw HCI command authoring for feature logic; we use GAP/GATT/SM APIs and event handlers.

## Feature mapping

## 1) Transport connect/disconnect

Polar intent:
- find H10,
- connect,
- maintain link state,
- handle retries/recoveries.

BTstack path:
- scan report: `GAP_EVENT_ADVERTISING_REPORT`
- connect call: `gap_connect(...)`
- connection complete:
  - `HCI_EVENT_META_GAP` + `GAP_SUBEVENT_LE_CONNECTION_COMPLETE`, or
  - `HCI_EVENT_LE_META` + `HCI_SUBEVENT_LE_CONNECTION_COMPLETE`
- disconnect event: `HCI_EVENT_DISCONNECTION_COMPLETE`
- explicit control: `gap_connect_cancel`, `gap_disconnect`

## 2) Service/characteristic discovery

Polar intent:
- discover HR + PMD + PSFTP services/chars and cache handles.

BTstack path:
- `gatt_client_discover_primary_services`
  - per service: `GATT_EVENT_SERVICE_QUERY_RESULT`
  - end marker: `GATT_EVENT_QUERY_COMPLETE`
- `gatt_client_discover_characteristics_for_service`
  - per characteristic: `GATT_EVENT_CHARACTERISTIC_QUERY_RESULT`
  - end marker: `GATT_EVENT_QUERY_COMPLETE`

Key operational point:
- Query completion status is ATT-level (`att_status` from query-complete), not just immediate API return value.

## 3) Notifications/indications (HR, PMD CP/data, PSFTP D2H/MTU)

Polar intent:
- subscribe to characteristic updates,
- consume stream/control traffic asynchronously.

BTstack path:
- listen registration: `gatt_client_listen_for_characteristic_value_updates`
- CCC write: `gatt_client_write_client_characteristic_configuration`
- incoming values:
  - `GATT_EVENT_NOTIFICATION`
  - `GATT_EVENT_INDICATION`
- unsubscribe/teardown:
  - CCC disable + `gatt_client_stop_listening_for_characteristic_value_updates`

Important nuance for Polar PMD CP:
- CP updates can arrive via indicate path on some devices/configs.
- central logic must handle both notify and indicate semantics.

## 4) Write operations (PMD commands, PSFTP frames)

Polar intent:
- write command/request payloads and verify outcome.

BTstack path:
- write call: `gatt_client_write_value_of_characteristic`
- completion: `GATT_EVENT_QUERY_COMPLETE` with ATT status

Critical semantic from BTstack docs:
- write payload memory must remain valid until query-complete arrives.
- our write wrappers wait for completion before returning to caller.

## 5) MTU handling

Polar intent:
- ensure practical MTU before higher-volume operations.

BTstack path:
- current MTU query: `gatt_client_get_mtu`
- explicit negotiation: `gatt_client_send_mtu_negotiation`
- MTU events:
  - `GATT_EVENT_MTU`
  - `ATT_EVENT_MTU_EXCHANGE_COMPLETE`

## 6) Security/pairing for protected operations

Polar intent:
- recover from ATT security errors and proceed after pairing.

BTstack path:
- request pairing: `sm_request_pairing`
- interactive events:
  - `SM_EVENT_JUST_WORKS_REQUEST`
  - `SM_EVENT_NUMERIC_COMPARISON_REQUEST`
  - `SM_EVENT_AUTHORIZATION_REQUEST`
  - `SM_EVENT_PAIRING_COMPLETE`
- confirm/grant actions:
  - `sm_just_works_confirm`
  - `sm_numeric_comparison_confirm`
  - `sm_authorization_grant`

Operational point:
- immediate transport return codes and eventual SM/ATT outcomes are both needed for robust error handling.

## 7) Link update telemetry

Polar intent:
- track post-connect parameter changes relevant to streaming quality.

BTstack path:
- request update: `gap_update_connection_parameters`
- completion event: `HCI_SUBEVENT_LE_CONNECTION_UPDATE_COMPLETE`

## Why the BTstack protocol pages are useful for this project

For this codebase, those pages are valuable because they clarify:

1. **Event ownership and flow control model**
   - why we structure runtime around packet handlers + completion events.
2. **Where statuses come from**
   - immediate HCI/API return vs deferred ATT/SM event status.
3. **Data-path discipline**
   - buffer lifetime and “send only when state allows” semantics.
4. **Debug interpretation**
   - helping distinguish RF/link issues from host scheduling/backpressure/state-machine issues.

## Related docs in this repo

- Polar protocol details:
  - [`polar_h10_gatt.md`](./polar_h10_gatt.md)
  - [`polar_pmd.md`](./polar_pmd.md)
  - [`polar_psftp.md`](./polar_psftp.md)
- BTstack API dependency surface:
  - [`btstack_api_surface.md`](./btstack_api_surface.md)
- BTstack status triage table:
  - [`btstack_status_triage.md`](./btstack_status_triage.md)
- BTstack debugging/config flags:
  - [`../howto/btstack_debug_flags.md`](../howto/btstack_debug_flags.md)
