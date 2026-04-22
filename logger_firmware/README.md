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

## Persistence

The appliance keeps its hot-path study data on the SD card.

- `journal.bin`, manifests, queue state, and other log-like artifacts live on FAT32 SD storage.
- Onboard flash is reserved for small internal state only:
  - provisioned config + TLS material,
  - boot/fault metadata,
  - the sparse internal system log.

The internal flash layout deliberately separates cold config from hotter boot/fault metadata so ordinary boots and fault latching do not rewrite the large config/certificate blob.

The reserved internal-flash region is intentionally generous:

- config/TLS and boot/fault metadata each use multi-slot append-only sector rotation for simple wear spreading,
- the system log uses larger fixed records so detail payloads do not have to be squeezed into tiny JSON fragments,
- everything remains raw-flash and sequence/CRC based rather than pulling in a filesystem for a handful of structured records.

## PSRAM layout

The RP2-2 board gives us 8 MiB of PSRAM. The source of truth for the compile-time
layout is `include/logger/psram_layout.h`.

Current map:

- `0x11000000 .. 0x1107ffff` — system log ring (`512 KiB`)
- `0x11080000 .. 0x1120ffff` — fixed-layout writer buffers:
  - staging slots
  - command ring slots
  - chunk buffer
- `0x11210000 .. 0x1130ffff` — reserved queue region (`1 MiB`)
  - queue scratch JSON arena
  - queue tmp/op/scan/delete workspaces
- `0x11310000 .. 0x1132ffff` — reserved upload region (`128 KiB`)
  - HTTP request / response / process workspaces
- `0x11330000 .. 0x1134ffff` — reserved upload-bundle region (`128 KiB`)
  - shared bundle stream workspace

Why this is centralized:

- queue, upload, and bundle code all use PSRAM-backed scratch/workspace state;
- those regions are reserved in one header so different translation units cannot
  silently overlap by inventing their own offsets;
- each user then proves locally with `_Static_assert(...)` that it stays inside
  its assigned region.

The layout is intentionally roomy. SRAM pressure, not raw PSRAM capacity, is the
real constraint in the HTTPS/upload path, so we prefer simple fixed regions with
slack over tight packing.

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
