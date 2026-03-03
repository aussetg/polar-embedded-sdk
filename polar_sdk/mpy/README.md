# polar_sdk/mpy

MicroPython binding layer for the Polar SDK.

Current contents:
- `mod_polar_sdk.c` — MicroPython binding layer (now primarily using shared `runtime_link` state):
  - lifecycle + state/stats + module error types,
  - scan filtering (addr or name prefix),
  - connect timeout/backoff loop,
  - service/characteristic discovery and handle caching for HR/PMD/PSFTP,
  - disconnect reason counters,
  - thin wrappers over SDK helpers for:
    - retry/backoff/service-mask validation,
    - connect-attempt timing/backoff policy,
    - transport connect adapter loop,
    - generic wait-loop helper usage,
    - runtime link transition + runtime-context lifecycle helpers,
    - normalized link-event adapter dispatch,
    - btstack->sdk link-event decoding,
    - btstack helper classification/matching (UUID/service/char/adv prefix),
    - btstack GATT event normalization + route classification helpers,
    - btstack scan filter/match + adv runtime helpers,
    - btstack SM event normalization + control-policy helpers,
    - btstack packet dispatch adapter,
    - discovery orchestration + runtime helpers,
    - discovery btstack runtime decode helper,
    - discovery dispatch callback adapter,
    - discovery apply helpers for found flags/handles,
    - generic GATT CCC control + notify-runtime + query-complete helper usage,
    - generic GATT write sequencing helper usage,
    - discovery progression policy,
    - HR 0x2A37 parsing (`polar_sdk_hr`),
    - PMD ECG data parsing + byte ring (`polar_sdk_ecg`),
    - PMD IMU/ACC raw data parsing + byte ring (`polar_sdk_imu`),
    - PMD control bridge + start policy (`polar_sdk_pmd_control`, `polar_sdk_pmd_start`).
- `micropython.cmake` — user C module wiring + build flags:
  - feature flags: `POLAR_ENABLE_HR`, `POLAR_ENABLE_ECG`, `POLAR_ENABLE_PSFTP`
  - patch policy: `POLAR_VERIFY_MICROPY_PATCHES`, `POLAR_AUTO_APPLY_PATCHES`
  - build metadata injection: `POLAR_BUILD_GIT_SHA`, `POLAR_BUILD_GIT_DIRTY`, `POLAR_BUILD_PRESET`, `POLAR_BUILD_TYPE`

PSFTP data-plane baseline is now integrated:
- `H10.list_dir(path)`
- `H10.download(path, *, max_bytes=..., timeout_ms=...)`

Current scope is read-only GET operations (directory listing + raw download payload).
