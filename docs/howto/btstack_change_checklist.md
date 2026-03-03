# How-to — BTstack change checklist (PR gate)

Status: How-to (workflow)
Last updated: 2026-03-03

Use this checklist whenever a change touches BTstack-facing code, BTstack config, or BLE transport behavior.

## Mandatory checklist

- [ ] 1) Build the pure pico-sdk BTstack probe (`examples/pico_sdk`)
- [ ] 2) Build MicroPython rp2 firmware (`fw-rp2-1`)
- [ ] 3) Run minimal connect/discover/HR smoke test on hardware

## One-command helper

From repo root:

```bash
./scripts/btstack_change_checklist.sh
```

This runs mandatory steps 1 and 2.

To also run step 3 (hardware HR smoke):

```bash
./scripts/btstack_change_checklist.sh --with-hr-smoke --port /dev/ttyACM0
```

Optional explicit H10 address:

```bash
./scripts/btstack_change_checklist.sh --with-hr-smoke \
  --port /dev/ttyACM0 \
  --addr AA:BB:CC:DD:EE:FF
```

Equivalent environment variables:
- `POLAR_MP_PORT=/dev/ttyACM0`
- `POLAR_H10_ADDR=AA:BB:CC:DD:EE:FF`

## Manual commands (equivalent)

### 1) pico-sdk probe build

```bash
PICO_SDK_PATH=$PWD/vendors/pico-sdk \
PICO_BTSTACK_PATH=$PWD/vendors/pico-sdk/lib/btstack \
cmake -S examples/pico_sdk -B build/pico_sdk_probe_btstack_check \
  -DPICO_BOARD=pimoroni_pico_plus2_w_rp2350
cmake --build build/pico_sdk_probe_btstack_check -j$(nproc)
```

### 2) MicroPython rp2 build

```bash
cmake --preset fw-rp2-1
cmake --build --preset fw-rp2-1 -j$(nproc)
```

### 3) Minimal HR smoke (hardware)

At minimum, confirm all three:
- connect succeeds,
- discovery reaches `READY`,
- at least one HR sample is received.

Reference script and expected fields: [`validation.md`](./validation.md)

## Related checks (recommended)

- BTstack API/config sanity:
  - `python scripts/diff_btstack_config.py`
- BTstack license guard:
  - `python scripts/check_btstack_license_headers.py`
- Docs link lint:
  - `python .pi/skills/lint-docs/scripts/check_docs_links.py`
