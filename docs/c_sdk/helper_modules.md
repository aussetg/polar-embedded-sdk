# Core helper modules

This page is the practical reference for the reusable C modules in `polar_sdk/core/include/`.

## Connect and timing helpers

### `polar_sdk_common.h`

- `polar_sdk_backoff_delay_ms(attempt_index)`
  - returns the retry backoff for a given attempt number
- `polar_sdk_service_mask_is_valid(mask, allowed_mask)`
  - validates a requested service bitmask against what the build allows

Use this for deterministic retry behavior and early configuration validation.

### `polar_sdk_connect.h`

Provides a compact connect-retry policy:

- `polar_sdk_connect_policy_t`
  - `timeout_ms`: total time budget
  - `attempt_slice_ms`: max time to spend on one attempt
- `polar_sdk_connect_state_t`
  - `start_ms`
  - `attempt_index`

Functions:

- `polar_sdk_connect_init()`
- `polar_sdk_connect_has_time_left()`
- `polar_sdk_connect_attempt_budget_ms()`
- `polar_sdk_connect_next_backoff_ms()`

This is a policy helper only. Your code still starts scans and connections.

### `polar_sdk_wait.h`

`polar_sdk_wait_until_done_or_disconnect()` is a tiny but useful primitive for async BLE operations.

You provide:

- clock,
- sleep,
- a completion predicate,
- a connected predicate.

It returns `false` if the link drops or the timeout expires before completion.

## Security helpers

### `polar_sdk_security.h`

`polar_sdk_security_request_with_retry()` handles the common pattern:

1. request pairing,
2. wait a bit,
3. re-check whether the link became secure,
4. repeat for a bounded number of rounds.

Inputs:

- `polar_sdk_security_policy_t`
  - `rounds`
  - `wait_ms_per_round`
  - `request_gap_ms`
  - `poll_ms`
- `polar_sdk_security_ops_t`
  - `is_connected`
  - `is_secure`
  - `request_pairing`
  - `sleep_ms`

Use this when ATT status indicates insufficient authentication and the protocol expects encryption.

## Runtime and link telemetry

### `polar_sdk_runtime.h`

`polar_sdk_runtime_link_t` is a shared status struct for:

- connection state,
- connection handle,
- last HCI/disconnect codes,
- connection update telemetry,
- disconnect counters.

Important helpers:

- `polar_sdk_runtime_link_init()`
- `polar_sdk_runtime_mark_attempt_failed()`
- `polar_sdk_runtime_on_connection_complete()`
- `polar_sdk_runtime_on_connection_update_complete()`
- `polar_sdk_runtime_on_disconnect()`

Use it if you want consistent operational telemetry across products and tools.

## HR parser

### `polar_sdk_hr.h`

Purpose:

- parse BLE Heart Rate Measurement notifications (`0x2A37`)
- normalize RR intervals to milliseconds
- retain the latest parsed sample in a small state struct

Main API:

- `polar_sdk_hr_reset()`
- `polar_sdk_hr_parse_measurement()`

The parser stores:

- latest BPM,
- contact flag,
- up to 4 RR intervals,
- sample sequence,
- parse error count.

## PMD control and streaming

### `polar_sdk_pmd.h`

This header covers:

- PMD opcode / measurement constants,
- start-command builders,
- control-point response parsing,
- security-related ATT status classification,
- a high-level ECG / ACC start policy.

Important functions:

- `polar_sdk_pmd_build_ecg_start_command()`
- `polar_sdk_pmd_build_acc_start_command()`
- `polar_sdk_pmd_parse_cp_response()`
- `polar_sdk_pmd_start_ecg_with_policy()`
- `polar_sdk_pmd_start_acc_with_policy()`

The start-policy helpers are especially valuable because they combine:

- security escalation,
- CCC enable,
- MTU gating,
- command write,
- response wait,
- bounded retry.

### `polar_sdk_pmd_control.h`

This is the glue between generic GATT operations and PMD-specific flow.

Important functions:

- `polar_sdk_pmd_map_notify_result()`
- `polar_sdk_pmd_enable_notify_pair()`
- `polar_sdk_pmd_start_command_and_wait()`

Use this if your application already owns the notification listeners and write primitives, but you do not want to rewrite PMD command sequencing.

### `polar_sdk_ecg.h`

Provides an ECG byte ring and PMD notification parser.

Output format pushed into the ring:

- one sample = **4 bytes**, little-endian signed `int32`
- unit = **microvolts**

Important functions:

- `polar_sdk_ecg_ring_init()`
- `polar_sdk_ecg_ring_reset()`
- `polar_sdk_ecg_ring_available()`
- `polar_sdk_ecg_ring_pop_bytes()`
- `polar_sdk_ecg_parse_pmd_notification()`

The parser currently accepts H10 ECG frame type 0.

### `polar_sdk_imu.h`

Provides an ACC byte ring and PMD notification parser.

Output format pushed into the ring:

- one sample = **6 bytes**
- layout = little-endian `int16 x`, `int16 y`, `int16 z`
- unit = **mg**

Important functions mirror ECG:

- `polar_sdk_imu_ring_init()`
- `polar_sdk_imu_ring_reset()`
- `polar_sdk_imu_ring_available()`
- `polar_sdk_imu_ring_pop_bytes()`
- `polar_sdk_imu_parse_pmd_notification()`

The parser currently accepts ACC frame type `0x01`.

## PSFTP framing and transactions

### `polar_sdk_psftp.h`

This is the low-level PSFTP/RFC60/RFC76 toolkit.

It provides:

- RFC60 request/query builders,
- RFC76 TX fragmentation state,
- RFC76 RX reassembly state,
- path-based GET / REMOVE encoders,
- H10 recording-start parameter encoder,
- directory decode helper,
- recording-status decode helper.

Important functions:

- `polar_sdk_psftp_build_rfc60_request()`
- `polar_sdk_psftp_build_rfc60_query()`
- `polar_sdk_psftp_tx_init()`
- `polar_sdk_psftp_tx_build_next_frame()`
- `polar_sdk_psftp_rx_reset()`
- `polar_sdk_psftp_rx_feed_frame()`
- `polar_sdk_psftp_encode_get_operation()`
- `polar_sdk_psftp_encode_remove_operation()`
- `polar_sdk_psftp_decode_directory()`

### `polar_sdk_psftp_runtime.h`

This header lifts PSFTP from frame-level tools to transaction-level helpers.

Two important layers exist here:

1. **prepare**
   - ensure required characteristics exist
   - ensure security if needed
   - enable notify paths
2. **transaction**
   - send a GET / query / proto payload
   - wait for full response
   - map protocol / sequence / remote errors into one result enum

Important functions:

- `polar_sdk_psftp_prepare_channels()`
- `polar_sdk_psftp_prepare_failure_is_security_related()`
- `polar_sdk_psftp_execute_get_operation()`
- `polar_sdk_psftp_execute_query_operation()`
- `polar_sdk_psftp_execute_proto_operation()`

These are the main PSFTP helpers you want in application code.

## BTstack adapter helpers

The BTstack-facing headers are best read alongside:

- `examples/pico_sdk/main.c`
- `examples/pico_sdk_psftp/main.c`

They cover:

- event normalization,
- advertisement matching,
- discovery routing,
- SM event handling,
- dispatch fan-out.

If you are integrating with BTstack, start from the examples rather than from the headers in isolation.

## Recommended adoption order

If you are adding features incrementally:

1. `polar_sdk_connect.h`
2. `polar_sdk_runtime.h`
3. `polar_sdk_hr.h`
4. `polar_sdk_pmd.h` + `polar_sdk_pmd_control.h`
5. `polar_sdk_ecg.h` / `polar_sdk_imu.h`
6. `polar_sdk_psftp.h` + `polar_sdk_psftp_runtime.h`

That sequence matches the actual complexity gradient.