# polar_logger

This repository is building towards a **C-backed MicroPython module** for the **Polar H10** on **RP2** (current primary target: **RP2-1**, based on **Pimoroni Pico Plus 2 W**).

## Where is the driver?

The intended deliverable lives under:

- `polar_ble/driver/` — portable C driver (no MicroPython types)
- `polar_ble/mpy/` — MicroPython binding layer (C module glue)

At the moment the project is in a planning + investigation stage.

## Documentation

Start here:

- `docs/README.md`

## Build + tooling

- `CMakePresets.json` — canonical firmware build entrypoint (Pico 2 W + RP2-1 release/debug/workflow presets)
- `CMakeLists.txt` — repo-level build entrypoint forwarding into vendored MicroPython rp2 build
- `firmware/cmake/` — firmware build fragments (minimal Pimoroni `picographics` profile + compatibility shim)
- `examples/pico_sdk/` — standalone C probe (pico-sdk + BTstack) used for isolation testing
- `scripts/setup_hooks.sh` — one-time local git hook bootstrap (`core.hooksPath=.githooks`) for BTstack alignment guardrails

## Vendor deps

Third party sources live under `vendors/` (MicroPython, nanopb, ...).
