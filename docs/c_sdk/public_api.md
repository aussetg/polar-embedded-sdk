# Public API and status

This page separates the **documented public headers** from their **current implementation status**.

That distinction matters in this repository because the low-level helper modules are real and in active use, while the high-level session facade is still incomplete.

## Status summary

### Stable and useful today

These headers expose implemented helper APIs used by the MicroPython binding and C probes:

- `polar_sdk_common.h`
- `polar_sdk_connect.h`
- `polar_sdk_wait.h`
- `polar_sdk_security.h`
- `polar_sdk_runtime.h`
- `polar_sdk_hr.h`
- `polar_sdk_pmd.h`
- `polar_sdk_pmd_control.h`
- `polar_sdk_ecg.h`
- `polar_sdk_imu.h`
- `polar_sdk_psftp.h`
- `polar_sdk_psftp_runtime.h`
- most BTstack adapter headers under `polar_sdk/core/include/`

### Preview / scaffold API

`polar_sdk_session.h` defines the intended end-user C facade, but most operational functions are not wired yet.

Today, you should treat it as:

- useful for data types and enums,
- useful for understanding the intended high-level shape,
- **not** the recommended integration entrypoint for real firmware yet.

## `polar_sdk_status.h`

Common result enum:

- `POLAR_OK`
- `POLAR_ERR_TIMEOUT`
- `POLAR_ERR_NOT_CONNECTED`
- `POLAR_ERR_PROTOCOL`
- `POLAR_ERR_SECURITY`
- `POLAR_ERR_UNSUPPORTED`
- `POLAR_ERR_OVERFLOW`
- `POLAR_ERR_INVALID_ARG`
- `POLAR_ERR_STATE`
- `POLAR_ERR_BUSY`
- `POLAR_ERR_IO`

Use these at the boundaries of your own wrapper layer if you want a consistent error vocabulary.

## `polar_sdk_session.h`

This header defines the high-level types:

- session state enums,
- stream kind enums,
- recording kind enums,
- stream config fields,
- capability bitmasks,
- filesystem and recording metadata structs.

### Functions that are useful now

- `polar_session_config_init()`
- `polar_stream_config_init()`
- `polar_session_init()`
- `polar_session_deinit()`
- `polar_session_state()`
- `polar_session_is_connected()`
- `polar_session_set_required_capabilities()`
- `polar_session_get_required_capabilities()`
- `polar_session_get_capabilities()`
- `polar_stream_get_default_config()`
- `polar_stats_get()`

### Functions currently returning `POLAR_ERR_UNSUPPORTED`

As of the current tree, these are placeholders:

- `polar_session_connect()`
- `polar_stream_start()`
- `polar_stream_stop()`
- `polar_stream_read()`
- all recording operations
- all filesystem operations

### Recommendation

Do not build production code directly around `polar_sdk_session_*()` yet unless you are prepared to finish that facade yourself.

For real integrations, compose the helper modules directly.

## Minimal C example: helper-first style

```c
polar_sdk_connect_policy_t policy = {
    .timeout_ms = 15000,
    .attempt_slice_ms = 3500,
};

polar_sdk_connect_state_t state;
polar_sdk_connect_init(&state, now_ms());

while (polar_sdk_connect_has_time_left(&policy, &state, now_ms())) {
    uint32_t budget = polar_sdk_connect_attempt_budget_ms(&policy, &state, now_ms());
    // start scan / connect attempt here
    // wait up to budget
    // on failure:
    uint32_t backoff = polar_sdk_connect_next_backoff_ms(&policy, &state, now_ms());
    sleep_ms(backoff);
}
```

This is representative of the current C SDK style: small, explicit, composable pieces.

## Include strategy

In new code, prefer including only what you use. For example:

- HR parser only: `polar_sdk_hr.h`
- PMD start policy: `polar_sdk_pmd.h` and `polar_sdk_pmd_control.h`
- PSFTP transactions: `polar_sdk_psftp.h` and `polar_sdk_psftp_runtime.h`

Avoid pulling in the entire tree by habit. The modules are intentionally narrow.