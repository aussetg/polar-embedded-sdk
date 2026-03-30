# logger_firmware

Standalone pico-sdk/C firmware tree for the RP2-2 logger appliance.
Its goal is to automatically (after configuration) and independently run,
collect Polar data, and then upload to a server.

It is more or less a state machine going through:

1. `BOOT`
2. `SERVICE`
3. `LOG_WAIT_H10`
4. `LOG_CONNECTING`
5. `LOG_SECURING`
6. `LOG_STARTING_STREAM`
7. `LOG_STREAMING`
8. `LOG_STOPPING`
9. `UPLOAD_PREP`
10. `UPLOAD_RUNNING`
11. `IDLE_WAITING_FOR_CHARGER`
12. `IDLE_UPLOAD_COMPLETE`

It is currently very simple and only uses Pico SDK and the Polar Logger SDK—no RTOS or fancy stuff going on.

It is intentionally separate from:

- `polar_sdk/core/` — reusable Polar/H10 protocol and BTstack integration
- `polar_sdk/mpy/` — MicroPython binding layer
- `examples/pico_sdk/` — isolation probes and experiments
- `firmware/` — current MicroPython board/build support

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
