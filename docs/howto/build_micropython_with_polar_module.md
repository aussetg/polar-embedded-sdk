# How-to — build MicroPython (rp2) with the `polar_ble` user C module

Status: How-to
Last updated: 2026-03-02

This repo vendors MicroPython under `vendors/micropython/`.

The `polar_ble` MicroPython module scaffold lives under:

- `polar_ble/mpy/`

Prerequisites: [`toolchain_requirements.md`](./toolchain_requirements.md)

## Presets (current)

We currently keep **4 firmware presets**:

1. `fw-pico2w` — Pico 2 W base (polar module)
2. `fw-pico2w-picographics` — Pico 2 W + minimal Pimoroni picographics
3. `fw-rp2-1` — RP2-1 prototype (polar module + Pimoroni RV3028 `breakout_rtc` support)
4. `fw-rp2-1-debug` — RP2-1 prototype debug build (same module set)

RP2-1 presets set `MICROPY_C_HEAP_SIZE=8192` for stable startup with the included C++ user modules.

General rule: any rp2 preset that adds C++ user modules should set a non-zero `MICROPY_C_HEAP_SIZE`.

RP2-1 is the active prototype target.

## Patch handling policy

Patch handling is always configure-time auto-apply:

- `POLAR_BLE_AUTO_APPLY_PATCHES=ON`

No dedicated `*-autopatch` presets are kept.

If patching fails mid-series:

```bash
git -C vendors/micropython am --abort
```

Then resolve and retry configure.

## Build

From the repo root (RP2-1 release example):

```bash
cmake --preset fw-rp2-1
cmake --build --preset fw-rp2-1
```

RP2-1 debug:

```bash
cmake --preset fw-rp2-1-debug
cmake --build --preset fw-rp2-1-debug
```

Pico 2 W base:

```bash
cmake --preset fw-pico2w
cmake --build --preset fw-pico2w
```

Pico 2 W + picographics:

```bash
cmake --preset fw-pico2w-picographics
cmake --build --preset fw-pico2w-picographics
```

### One-command workflows

```bash
cmake --workflow --preset wf-fw-rp2-1
cmake --workflow --preset wf-fw-rp2-1-debug
cmake --workflow --preset wf-fw-pico2w
cmake --workflow --preset wf-fw-pico2w-picographics
```

## Artifacts

Stable exported artifact paths:

- `build/artifacts/fw-rp2-1/`
- `build/artifacts/fw-rp2-1-debug/`
- `build/artifacts/fw-pico2w/`
- `build/artifacts/fw-pico2w-picographics/`

Each stable artifact directory contains:

- `firmware.uf2`
- `firmware.elf`
- `build_info.txt`

## Flash (Pico Probe / OpenOCD)

Program RP2-1 release artifact via OpenOCD:

```bash
openocd -f interface/cmsis-dap.cfg -f target/rp2350.cfg \
  -c "adapter speed 5000; program build/artifacts/fw-rp2-1/firmware.elf verify reset exit"
```

## Build metadata in firmware

```python
import polar_ble
print(polar_ble.version())
print(polar_ble.build_info())
```

`build_info()` includes at least:

- `version`
- `git_sha`
- `git_dirty`
- `preset`
- `build_type`
- feature flags + btstack availability
