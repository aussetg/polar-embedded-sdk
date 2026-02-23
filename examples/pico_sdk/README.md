# examples/pico_sdk probe

Minimal Pico 2W BTstack/CYW43 central probe for Polar H10.

Purpose:
- isolate behavior below MicroPython runtime/GATT wrapper,
- log low-level connection/security/update events,
- verify if link stalls still happen in a pure C app.

## Build

```bash
cmake -S examples/pico_sdk -B build/pico_sdk_probe \
  -DPICO_BOARD=pico2_w \
  -DPICO_SDK_PATH=$PICO_SDK_PATH
cmake --build build/pico_sdk_probe -j$(nproc)
```

## Flash

```bash
openocd -f interface/cmsis-dap.cfg -f target/rp2350.cfg \
  -c "adapter speed 5000; program build/pico_sdk_probe/h10_btstack_probe.elf verify reset exit"
```

## Useful CMake knobs

All knobs are cache variables (override with `-D...` at configure time):

- `H10_TARGET_ADDR` (default `24:AC:AC:05:A3:10`)
- `H10_ENABLE_HR` (default `1`)
- `H10_FORCE_PAIRING` (default `0`)
- `H10_POST_CONNECT_UPDATE` (default `1`)
- `H10_POST_CONNECT_CONN_INTERVAL_MIN` (default `24`)
- `H10_POST_CONNECT_CONN_INTERVAL_MAX` (default `24`)
- `H10_POST_CONNECT_CONN_LATENCY` (default `0`)
- `H10_POST_CONNECT_SUPERVISION_TIMEOUT_10MS` (default `72`)

Example: idle/connection-only probe (no HR subscription):

```bash
cmake -S examples/pico_sdk -B build/pico_sdk_probe_idle \
  -DPICO_BOARD=pico2_w \
  -DPICO_SDK_PATH=$PICO_SDK_PATH \
  -DH10_ENABLE_HR=0
cmake --build build/pico_sdk_probe_idle -j$(nproc)
```

## Serial capture

Output is on USB CDC (`/dev/ttyACM1` in this setup).
Capture with any serial tool or your own script into `captures/`.

## Build against MicroPython-vendored BTstack

For A/B tests against MicroPython's BTstack tree:

```bash
cmake -S examples/pico_sdk -B build/pico_sdk_probe_mpbtstack \
  -DPICO_BOARD=pico2_w \
  -DPICO_SDK_PATH=$PICO_SDK_PATH \
  -DPICO_BTSTACK_PATH=$PWD/vendors/micropython/lib/btstack \
  -DH10_ENABLE_HR=0
cmake --build build/pico_sdk_probe_mpbtstack -j$(nproc)
```
