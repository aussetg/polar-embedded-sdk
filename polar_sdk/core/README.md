# polar_sdk/core (Polar SDK core)

Portable C **Polar SDK core** code (no MicroPython dependencies).

This layer is intentionally **Polar-specific** and **BTstack-backed** (it is not a generic BLE driver).

This layer is shared by:
- `polar_sdk/mpy` (MicroPython binding)
- `examples/pico_sdk` (pure C harness)

Current helper coverage includes:
- transport/connect/discovery orchestration,
- BTstack SM helpers (event decode + default auth policy setup),
- reusable security retry policy helper (`polar_sdk_security`),
- reusable ATT MTU ensure helper (`polar_sdk_gatt_mtu`),
- HR parser state,
- PMD ECG/IMU parsing + PMD start policy,
- PSFTP RFC60/RFC76 framing + protobuf helpers (`polar_sdk_psftp`),
- reusable PSFTP channel-prepare + GET transaction runtime helper (`polar_sdk_psftp_runtime`).
