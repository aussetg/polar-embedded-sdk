# logger_firmware

Standalone pico-sdk/C firmware tree for the RP2-2 logger appliance.

This directory is the home for the **appliance firmware** described by:

- `docs/specs/logger_firmware_v1.md`
- `docs/specs/logger_runtime_architecture_v1.md`
- `docs/specs/logger_data_contract_v1.md`
- `docs/specs/logger_host_interfaces_v1.md`
- `docs/specs/logger_update_architecture.md`

It is intentionally separate from:

- `polar_sdk/core/` — reusable Polar/H10 protocol and BTstack integration
- `polar_sdk/mpy/` — MicroPython binding layer
- `examples/pico_sdk/` — isolation probes and experiments
- `firmware/` — current MicroPython board/build support

## Current scaffold

The first slice here is deliberately small but real:

- a standalone pico-sdk build entrypoint,
- an RP2-2 appliance board-policy header,
- a tiny top-level runtime state model using the logger spec state names,
- a boot path that safely falls into `SERVICE` until provisioning/storage/clock policy is implemented,
- a visible heartbeat using the wireless-chip LED.

In other words: this tree now boots as a logger-appliance stub rather than as another probe.

## Build

The build defaults to the canonical RP2-2 board profile and reuses the existing pico-sdk board header from:

- `firmware/boards/RP2_2/pimoroni_pico_lipo2xl_w.h`

Configure + build:

```bash
cmake -S logger_firmware -B build/logger_firmware_rp2_2
cmake --build build/logger_firmware_rp2_2 -j$(nproc)
```

Artifacts appear in:

```text
build/logger_firmware_rp2_2/
```

## Initial source layout

```text
logger_firmware/
  CMakeLists.txt
  README.md
  boards/
    rp2_2/
      board_config.h
  include/
    logger/
      app_main.h
      app_state.h
  src/
    main.c
    app_main.c
    app_state.c
```

## Next implementation steps

The next durable slices should follow `docs/specs/logger_runtime_architecture_v1.md` in this rough order:

1. persistent config + provisioning gate,
2. SD mount and fail-closed storage checks,
3. session/journal append-only core,
4. H10 scan/connect/security/start logic via `polar_sdk/core/`,
5. upload queue and service CLI.
