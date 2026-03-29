# Using the SDKs

This repository exposes **two different SDK surfaces**:

1. **C SDK core** in `polar_sdk/core/`
2. **MicroPython SDK** in `polar_sdk/mpy/` as the `polar_sdk` module

They target different users.

## Which one should you use?

### Use the C SDK if you are

- integrating Polar support into existing firmware,
- already comfortable with event-driven BLE code,
- working directly with BTstack or a similar transport layer,
- willing to assemble the final application from small reusable helper modules.

The C layer is intentionally **low-level and integration-oriented**.
It gives you parsing, retry logic, discovery helpers, PMD/PSFTP framing, and reusable control-path building blocks.

It does **not** yet provide a fully wired, transport-independent, batteries-included session implementation.

### Use the MicroPython SDK if you are

- collecting data on a Pico 2 W,
- writing short scripts for experiments,
- more interested in recordings and sensor data than BLE internals,
- comfortable with editing Python examples but not with embedded C.

The MicroPython layer is intentionally **task-oriented**.
You create a `polar_sdk.Device`, connect, start a stream or recording, and read data.

## Current practical status

### C SDK

- **Implemented and usable today:** helper modules in `polar_sdk/core/include/` and `polar_sdk/core/src/`
- **Best example of real use:** `examples/pico_sdk/` and `examples/pico_sdk_psftp/`
- **High-level facade status:** `polar_sdk_session.h` exists, but most operational calls are still placeholders returning `POLAR_ERR_UNSUPPORTED`

### MicroPython SDK

- **Implemented and usable today** for:
  - connect / disconnect
  - HR streaming
  - ECG streaming
  - ACC streaming
  - PSFTP directory listing and download
  - H10 HR recording control

## Recommended reading order

If you are doing firmware integration:

1. [C SDK](./c_sdk/README.md)
2. [Architecture and integration model](./c_sdk/architecture.md)
3. [Core helper modules](./c_sdk/helper_modules.md)

If you are writing experiment scripts:

1. [MicroPython SDK](./micropython/README.md)
2. [Quick start](./micropython/quickstart.md)
3. [API reference](./micropython/api_reference.md)
4. [Data formats](./micropython/data_formats.md)