# C integration cookbook

This page is a practical guide for integrating the C SDK helpers into a BTstack-based embedded application.

It is built directly from the patterns used in:

- `examples/pico_sdk/main.c`
- `examples/pico_sdk_psftp/main.c`

The goal is not to hide complexity. The goal is to show a repeatable way to wire the helper modules together.

## 1. Start with one application state struct

Both example probes keep explicit global state for:

- connection handle,
- discovered services/characteristics,
- listener registration flags,
- pending query-complete flags,
- runtime telemetry,
- feature-specific scratch buffers.

At minimum, keep these pieces together:

```c
static polar_sdk_runtime_link_t runtime_link;

static bool connected = false;
static hci_con_handle_t conn_handle = HCI_CON_HANDLE_INVALID;

static gatt_client_service_t pmd_service;
static gatt_client_characteristic_t pmd_cp_char;
static gatt_client_characteristic_t pmd_data_char;
static bool pmd_service_found = false;
static bool pmd_cp_found = false;
static bool pmd_data_found = false;
```

Recommendation:

- put all of this into a single `app_t` struct in real firmware,
- pass that struct through the SDK ops callbacks instead of using file-scope globals.

The examples use globals because they are probes.

## 2. Normalize link events early

Do not spread raw HCI event parsing all over the code.

The probe in `examples/pico_sdk/main.c` uses:

- `polar_sdk_btstack_dispatch.h`
- `polar_sdk_btstack_link.h`
- `polar_sdk_btstack_scan.h`
- `polar_sdk_btstack_adv_runtime.h`
- `polar_sdk_btstack_sm.h`

The pattern is:

1. BTstack callback receives a packet
2. adapter helper decodes it
3. your application callback updates local state and runtime telemetry

That keeps the rest of the code focused on behavior rather than packet structure.

## 3. Use `polar_sdk_connect.h` for retry windows and backoff

From `examples/pico_sdk/main.c`:

```c
static polar_sdk_connect_policy_t reconnect_policy = {
    .timeout_ms = H10_CONNECT_TIMEOUT_WINDOW_MS,
    .attempt_slice_ms = H10_CONNECT_ATTEMPT_SLICE_MS,
};
static polar_sdk_connect_state_t reconnect_state;
```

Use this policy for:

- total connect budget,
- per-attempt scan/connect budget,
- deterministic backoff between attempts.

Typical flow:

```c
polar_sdk_connect_init(&reconnect_state, now_ms);

while (polar_sdk_connect_has_time_left(&policy, &reconnect_state, now_ms)) {
    uint32_t budget = polar_sdk_connect_attempt_budget_ms(&policy, &reconnect_state, now_ms);
    // start scan/connect attempt here
    // wait for success/failure up to budget
    uint32_t backoff_ms = polar_sdk_connect_next_backoff_ms(&policy, &reconnect_state, now_ms);
    sleep_ms(backoff_ms);
}
```

## 4. Centralize disconnect cleanup

The C probes both have a strong cleanup step after disconnect.

That cleanup should always:

- clear `connected`
- invalidate the connection handle
- stop all characteristic listeners
- clear discovered handles
- clear pending GATT/query-complete flags
- reset feature-specific response state

This is not optional. BLE bugs become much harder to debug if stale handles survive reconnects.

## 5. Wrap CCC enable/disable with `polar_sdk_gatt_notify_runtime_set()`

The PMD probe demonstrates the intended pattern.

First define a tiny context struct:

```c
typedef struct {
    gatt_client_characteristic_t *chr;
    gatt_client_notification_t *notification;
    bool *listening;
} probe_pmd_notify_ctx_t;
```

Then provide the generic ops:

- `is_connected_ready`
- `listener_active`
- `start_listener`
- `stop_listener`
- `write_ccc`
- `wait_complete`

Then call the runtime helper:

```c
polar_sdk_gatt_notify_runtime_args_t args = {
    .ops = &ops,
    .has_value_handle = chr != NULL && chr->value_handle != 0,
    .enable = true,
    .properties = chr != NULL ? chr->properties : 0,
    .prop_notify = ATT_PROPERTY_NOTIFY,
    .prop_indicate = ATT_PROPERTY_INDICATE,
    .ccc_none = GATT_CLIENT_CHARACTERISTICS_CONFIGURATION_NONE,
    .ccc_notify = GATT_CLIENT_CHARACTERISTICS_CONFIGURATION_NOTIFICATION,
    .ccc_indicate = GATT_CLIENT_CHARACTERISTICS_CONFIGURATION_INDICATION,
    .att_success = ATT_ERROR_SUCCESS,
    .timeout_ms = 2000,
    .cfg_pending = &pmd_cfg_pending,
    .cfg_done = &pmd_cfg_done,
};

polar_sdk_gatt_notify_runtime_result_t r =
    polar_sdk_gatt_notify_runtime_set(&args);
```

Why this helps:

- listener registration and CCC writes stay consistent,
- ATT timeout/ATT reject cases are normalized,
- the same helper can be reused for HR, PMD, and PSFTP.

## 6. Use `polar_sdk_gatt_mtu_ensure_minimum()` instead of open-coding MTU logic

In `examples/pico_sdk/main.c`, PMD start uses a helper-backed MTU upgrade path.

You provide small callbacks:

- `read_mtu`
- `request_exchange`
- `wait_exchange_complete`
- `current_mtu`

Then call:

```c
polar_sdk_gatt_mtu_result_t r = polar_sdk_gatt_mtu_ensure_minimum(
    &mtu_ops,
    minimum_mtu,
    2000,
    &att_mtu);
```

This keeps PMD startup policy readable.

## 7. Start PMD streams with the policy helper, not hand-coded retries

This is the most important C integration pattern in `examples/pico_sdk/main.c`.

The probe builds one ops table:

```c
polar_sdk_pmd_start_ops_t ops = {
    .ctx = NULL,
    .is_connected = pmd_is_connected,
    .encryption_key_size = pmd_encryption_key_size,
    .request_pairing = pmd_request_pairing,
    .sleep_ms = pmd_sleep_ms_cb,
    .enable_notifications = pmd_enable_notifications_cb,
    .ensure_minimum_mtu = pmd_ensure_minimum_mtu_cb,
    .start_ecg_and_wait_response = pmd_start_measurement_and_wait_response_cb,
};
```

Then it declares an explicit start policy:

```c
polar_sdk_pmd_start_policy_t ecg_policy = {
    .ccc_attempts = H10_PMD_CCC_ATTEMPTS,
    .security_rounds_per_attempt = H10_PMD_SECURITY_ROUNDS,
    .security_wait_ms = H10_PMD_SECURITY_WAIT_MS,
    .minimum_mtu = H10_PMD_MIN_MTU,
    .sample_rate = H10_PMD_ECG_SAMPLE_RATE,
    .include_resolution = true,
    .resolution = H10_PMD_ECG_RESOLUTION,
    .include_range = false,
    .range = 0,
};
```

And finally starts the stream with:

```c
polar_sdk_pmd_start_result_t ecg_result = polar_sdk_pmd_start_ecg_with_policy(
    &ecg_policy,
    &ops,
    &ecg_response_status,
    &ecg_last_ccc_att_status);
```

Why you should copy this pattern:

- security escalation is built in,
- CCC retries are bounded,
- MTU gating is built in,
- PMD response waiting is built in,
- failure mode reporting is explicit.

Do the same for ACC with `polar_sdk_pmd_start_acc_with_policy()`.

## 8. Parse PMD notifications into rings, not directly into application logic

Once PMD data notifications are enabled:

- route ECG notifications to `polar_sdk_ecg_parse_pmd_notification()`
- route ACC notifications to `polar_sdk_imu_parse_pmd_notification()`

That gives you byte rings with overflow telemetry:

- available bytes,
- parse errors,
- dropped bytes,
- high-water mark.

This is much easier to debug than direct ad-hoc sample handling in your BTstack callback.

## 9. For PSFTP, split the problem into prepare and transaction

`examples/pico_sdk_psftp/main.c` shows the cleanest current pattern.

### Step 9.1: implement secure channel preparation

The probe uses `polar_sdk_psftp_prepare_channels()` with callbacks for:

- link-ready check,
- required-characteristic check,
- security readiness,
- security escalation,
- MTU notify enable,
- D2H notify enable.

Directly from the example:

```c
polar_sdk_psftp_prepare_policy_t policy = {
    .retry_security_on_att = true,
    .strict_d2h_enable = false,
};
```

And:

```c
int status = polar_sdk_psftp_prepare_channels(&policy, &ops);
```

This is the right place to deal with:

- ATT auth failures,
- missing characteristics,
- notify enable sequencing.

### Step 9.2: build one transaction ops table

The probe then fills `polar_sdk_psftp_get_ops_t` once:

```c
*ops = (polar_sdk_psftp_get_ops_t){
    .ctx = NULL,
    .prepare_channels = psftp_get_prepare_channels_cb,
    .frame_capacity = psftp_get_frame_capacity_cb,
    .write_frame = psftp_get_write_frame_cb,
    .on_tx_frame_ok = psftp_get_on_tx_frame_ok_cb,
    .begin_response = psftp_get_begin_response_cb,
    .wait_response = psftp_get_wait_response_cb,
    .response_result = psftp_get_response_result_cb,
    .rx_state = psftp_get_rx_state_cb,
    .is_remote_success = psftp_get_is_remote_success_cb,
};
```

That lets the shared runtime helper own:

- RFC60 build,
- RFC76 fragmentation,
- response reassembly,
- sequence/protocol/overflow classification.

## 10. Execute PSFTP GET and query operations through the runtime helper

The probe’s wrapper is intentionally small:

```c
polar_sdk_psftp_trans_result_t r = polar_sdk_psftp_execute_get_operation(
    &ops,
    path,
    strlen(path),
    out_buf,
    out_capacity,
    PSFTP_RESPONSE_TIMEOUT_MS,
    out_len,
    &error_code,
    &prep_status,
    &write_status);
```

And for query operations:

```c
polar_sdk_psftp_trans_result_t r = polar_sdk_psftp_execute_query_operation(
    &ops,
    query_id,
    query_payload,
    query_payload_len,
    out_buf,
    out_capacity,
    PSFTP_RESPONSE_TIMEOUT_MS,
    out_len,
    &error_code,
    &prep_status,
    &write_status);
```

That is the recommended shape for application code:

- small wrapper,
- explicit buffer ownership,
- explicit timeout,
- explicit access to prepare/write sub-status.

## 11. Use helper decoders after successful PSFTP responses

Again following `examples/pico_sdk_psftp/main.c`:

- `polar_sdk_psftp_decode_directory()` for `GET "/"`
- `polar_sdk_psftp_decode_h10_recording_status_result()` for H10 recording status
- `polar_sdk_psftp_encode_h10_start_recording_params()` to build the start-recording query payload

That means your application code stays focused on semantics instead of frame details.

## 12. Keep diagnostics explicit

The PSFTP probe keeps separate counters for:

- raw value events,
- routed PSFTP value events,
- query-complete routing,
- transmitted frames,
- received frames,
- sequence/protocol/overflow failures.

That is a very good pattern to copy.

When debugging a new transport bug, ask these questions in order:

1. Did the link connect and encrypt?
2. Were the required characteristics discovered?
3. Did CCC enable complete successfully?
4. Did TX complete?
5. Did any raw RX arrive?
6. Did the router classify RX correctly?
7. Did RFC76 reassembly complete?
8. Did the final semantic decode succeed?

The examples are structured around exactly that debugging ladder.

## 13. Suggested order for a new firmware integration

If you are integrating the SDK into your own firmware, do it in this order:

1. scanning + connect + disconnect cleanup
2. runtime telemetry (`polar_sdk_runtime_link_t`)
3. HR discovery + HR notifications
4. PMD discovery + MTU + ECG start policy
5. ECG/ACC ring parsing
6. PSFTP discovery + channel prepare
7. PSFTP GET/query wrappers
8. H10 recording control

That progression mirrors the real complexity of the stack.

## 14. What not to do

Avoid these common mistakes:

- hand-coding PMD security retry loops in multiple places,
- leaving listener state alive across disconnects,
- mixing routing logic with semantic decoding everywhere,
- using stale discovered handles after reconnect,
- writing a monolithic callback that performs discovery, security, parsing, and logging all at once.

The helper modules exist specifically to prevent that style of code.

## Related files

- `examples/pico_sdk/main.c`
- `examples/pico_sdk_psftp/main.c`
- `polar_sdk/core/include/polar_sdk_pmd.h`
- `polar_sdk/core/include/polar_sdk_psftp_runtime.h`
- `polar_sdk/core/include/polar_sdk_gatt_mtu.h`
- `polar_sdk/core/include/polar_sdk_gatt_control.h`