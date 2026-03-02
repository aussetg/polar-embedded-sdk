# polar_ble/mpy

MicroPython binding layer for the Polar H10 driver.

Current contents:
- `mod_polar_ble.c` — MicroPython binding layer (now primarily using shared `runtime_link` state):
  - lifecycle + state/stats + module error types,
  - scan filtering (addr or name prefix),
  - connect timeout/backoff loop,
  - service/characteristic discovery and handle caching for HR/PMD/PSFTP,
  - disconnect reason counters,
  - thin wrappers over driver helpers for:
    - retry/backoff/service-mask validation,
    - connect-attempt timing/backoff policy,
    - transport connect adapter loop,
    - generic wait-loop helper usage,
    - runtime link transition + runtime-context lifecycle helpers,
    - normalized link-event adapter dispatch,
    - btstack->driver link-event decoding,
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
    - HR 0x2A37 parsing (`polar_ble_driver_hr`),
    - PMD ECG data parsing + byte ring (`polar_ble_driver_ecg`),
    - PMD IMU/ACC raw data parsing + byte ring (`polar_ble_driver_imu`),
    - PMD control bridge + start policy (`polar_ble_driver_pmd_control`, `polar_ble_driver_pmd_start`).
- `micropython.cmake` — user C module wiring + build flags:
  - feature flags: `POLAR_BLE_ENABLE_HR`, `POLAR_BLE_ENABLE_ECG`, `POLAR_BLE_ENABLE_PSFTP`
  - patch policy: `POLAR_BLE_VERIFY_MICROPY_PATCHES`, `POLAR_BLE_AUTO_APPLY_PATCHES`
  - build metadata injection: `POLAR_BLE_BUILD_GIT_SHA`, `POLAR_BLE_BUILD_GIT_DIRTY`, `POLAR_BLE_BUILD_PRESET`, `POLAR_BLE_BUILD_TYPE`

PSFTP data-plane operations are still pending later milestones.
