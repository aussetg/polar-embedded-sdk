# polar_ble/driver

Portable C driver code (no MicroPython dependencies).

This layer is shared by:
- `polar_ble/mpy` (MicroPython binding)
- `examples/pico_sdk` (pure C harness)

Current helper coverage includes:
- transport/connect/discovery orchestration,
- BTstack SM helpers (event decode + default auth policy setup),
- HR parser state,
- PMD ECG/IMU parsing + PMD start policy,
- PSFTP RFC60/RFC76 framing + protobuf helpers (`polar_ble_driver_psftp`).
