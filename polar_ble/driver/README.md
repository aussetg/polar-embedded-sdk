# polar_ble/driver

Portable C driver code (no MicroPython dependencies).

This driver layer is intentionally portable and shared by:
- `polar_ble/mpy` (MicroPython binding)
- `examples/pico_sdk` (pure C harness)
- 
Current runtime integration:
- `polar_ble/mpy/mod_polar_ble.c` uses shared driver helpers for:
  - connect attempt scheduling,
  - discovery progression,
  - HR parse state,
  - ECG parse/ring buffering,
  - IMU/ACC parse/ring buffering,
  - PMD start policy callbacks.
- `examples/pico_sdk/main.c` uses shared driver helpers for connect scheduling, runtime link transitions, and PMD start policy.
