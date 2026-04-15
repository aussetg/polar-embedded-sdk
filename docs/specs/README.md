# Specs and design notes

This directory now contains two kinds of material:

1. **Current custom logger firmware specs** for the pico-sdk/C study logger.
2. **Older SDK/MicroPython design notes** kept for implementation history and cross-checking.

For current SDK usage, still start with:

- `docs/c_sdk/`
- `docs/micropython/`

For the new custom logger firmware work, start here.

## Current custom logger firmware specs

- **Behavior/product spec:** [`logger_firmware_v1.md`](./logger_firmware_v1.md)
- **Storage/upload/data contract:** [`logger_data_contract_v1.md`](./logger_data_contract_v1.md)
- **Stable host/CLI JSON interfaces:** [`logger_host_interfaces_v1.md`](./logger_host_interfaces_v1.md)
- **Runtime architecture and state machine:** [`logger_runtime_architecture_v1.md`](./logger_runtime_architecture_v1.md)
- **Recovery architecture, fault FSMs, and service/unplug behavior:** [`logger_recovery_architecture_v1.md`](./logger_recovery_architecture_v1.md)
- **Long-term update path (SD-assisted first, OTA later):** [`logger_update_architecture.md`](./logger_update_architecture.md)

## Older SDK/MicroPython design notes

These documents are retained mainly as historical design material.

- **Polar SDK core + MicroPython module (rp2 + BTstack):** [`micropython_polar_sdk_driver.md`](./micropython_polar_sdk_driver.md)
- **API design draft (C + MicroPython):** [`polar_sdk_api_design.md`](./polar_sdk_api_design.md)
- **(Optional) Upstream MicroPython/BTstack hardening proposal:** [`micropython_btstack_global_fix.md`](./micropython_btstack_global_fix.md)
