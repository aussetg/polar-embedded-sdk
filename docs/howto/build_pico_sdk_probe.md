# How-to — build the pure pico-sdk BTstack probe (`examples/pico_sdk`)

Status: How-to
Last updated: 2026-02-24

This guide builds the standalone C probe app (no MicroPython runtime) from:

- `examples/pico_sdk/`

Use this when you want to isolate BLE behavior below the MicroPython binding/runtime layer.

## Build

From the repo root:

```bash
cmake -S examples/pico_sdk -B build/pico_sdk_probe \
  -DPICO_BOARD=pico2_w \
  -DPICO_SDK_PATH=$PICO_SDK_PATH

cmake --build build/pico_sdk_probe -j$(nproc)
```

## Flash (Pico Probe / OpenOCD)

```bash
openocd -f interface/cmsis-dap.cfg -f target/rp2350.cfg \
  -c "adapter speed 5000; program build/pico_sdk_probe/h10_btstack_probe.elf verify reset exit"
```

## Common build variants

Connection-only run (disable HR subscription):

```bash
cmake -S examples/pico_sdk -B build/pico_sdk_probe_idle \
  -DPICO_BOARD=pico2_w \
  -DPICO_SDK_PATH=$PICO_SDK_PATH \
  -DH10_ENABLE_HR=0

cmake --build build/pico_sdk_probe_idle -j$(nproc)
```

Build against MicroPython-vendored BTstack for A/B testing:

```bash
cmake -S examples/pico_sdk -B build/pico_sdk_probe_mpbtstack \
  -DPICO_BOARD=pico2_w \
  -DPICO_SDK_PATH=$PICO_SDK_PATH \
  -DPICO_BTSTACK_PATH=$PWD/vendors/micropython/lib/btstack \
  -DH10_ENABLE_HR=0

cmake --build build/pico_sdk_probe_mpbtstack -j$(nproc)
```

## BTstack 1.8 note

If you point `PICO_BTSTACK_PATH` to a BTstack 1.8 tree while using pico-sdk 2.2.0,
apply local pico-sdk patches first:

```bash
./patches/apply_pico_sdk_patches.sh --dry-run
./patches/apply_pico_sdk_patches.sh
```

(Required rename in rp2 pico_btstack integration: `hids_client.c` -> `hids_host.c`.)

## Useful CMake cache variables

- `H10_TARGET_ADDR` (default `24:AC:AC:05:A3:10`)
- `H10_ENABLE_HR` (default `1`)
- `H10_FORCE_PAIRING` (default `0`)
- `H10_POST_CONNECT_UPDATE` (default `1`)
- `H10_POST_CONNECT_CONN_INTERVAL_MIN` (default `24`)
- `H10_POST_CONNECT_CONN_INTERVAL_MAX` (default `24`)
- `H10_POST_CONNECT_CONN_LATENCY` (default `0`)
- `H10_POST_CONNECT_SUPERVISION_TIMEOUT_10MS` (default `72`)

## Serial logs

Probe logs are emitted on USB CDC (`/dev/ttyACM*`).

Device discovery:

```bash
ls -la /dev/ttyACM*
```

Capture with your preferred serial terminal/logger into `captures/`.
