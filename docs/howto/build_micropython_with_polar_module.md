# How-to — build MicroPython (rp2) with the `polar_ble` user C module

Status: How-to
Last updated: 2026-02-25

This repo vendors MicroPython under `vendors/micropython/`.

> Important: patching `vendors/micropython` is an explicit bootstrap step.

The `polar_ble` MicroPython module scaffold lives under:

- `polar_ble/mpy/`

Prerequisites: [`toolchain_requirements.md`](./toolchain_requirements.md)

## Build (CMake presets workflow)

From the repo root:

1) Apply local MicroPython patches (explicit bootstrap)

```bash
./patches/apply_micropython_patches.sh
```

Dry-run check only:

```bash
./patches/apply_micropython_patches.sh --dry-run
```

2) Configure + build firmware with presets

```bash
cmake --preset fw-rp2-polar
cmake --build --preset fw-rp2-polar
```

or

```bash
cmake --preset fw-rp2-polar-picographics
cmake --build --preset fw-rp2-polar-picographics
```

### One-command workflows

```bash
cmake --workflow --preset wf-fw-rp2-polar
cmake --workflow --preset wf-fw-rp2-polar-picographics
```

## Preset matrix

Release profiles:
- `fw-rp2-polar`
- `fw-rp2-polar-picographics`

Debug profiles:
- `fw-rp2-polar-debug`
- `fw-rp2-polar-picographics-debug`

Explicit configure-time patch auto-apply profiles:
- `fw-rp2-polar-autopatch`
- `fw-rp2-polar-picographics-autopatch`

(These mutate `vendors/micropython` and will fail if that submodule has local changes.)

## Artifacts

Inner build outputs:
- `build/mpy-rp2-polar_ble/rp2/firmware.uf2`
- `build/mpy-rp2-polar_ble/rp2/firmware.elf`
- `build/mpy-rp2-polar-picographics/rp2/firmware.uf2`
- `build/mpy-rp2-polar-picographics/rp2/firmware.elf`

Stable exported artifact paths:
- `build/artifacts/fw-rp2-polar_ble/`
- `build/artifacts/fw-rp2-polar-picographics/`
- `build/artifacts/fw-rp2-polar-debug/`
- `build/artifacts/fw-rp2-polar-picographics-debug/`

Each stable artifact directory contains:
- `firmware.uf2`
- `firmware.elf`
- `build_info.txt`

## Patch verification / optional auto-apply

Configure for `polar_ble/mpy` verifies patch-series presence by default:

- `POLAR_BLE_VERIFY_MICROPY_PATCHES=ON` (default)

If required patch subjects are missing from `vendors/micropython` history,
configure fails with an actionable error.

For explicit configure-time patch mutation, use an autopatch preset or override:

```bash
cmake --preset fw-rp2-polar -DPOLAR_BLE_AUTO_APPLY_PATCHES=ON
```

## Build metadata in firmware

The module exposes build metadata:

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

## Notes

- Presets are defined in `CMakePresets.json` at repo root and drive a repo-level CMake entrypoint (`CMakeLists.txt`) that forwards into the vendored rp2 port.
- Patches are tracked under `patches/micropython/*.patch`.
- If patching fails mid-series, run `git -C vendors/micropython am --abort`, resolve, and retry.
- `fw-rp2-polar-picographics` profiles set `MICROPY_C_HEAP_SIZE=8192` to avoid early startup OOM panics from C++ runtime allocations.
- Pimoroni `picographics` uses a local compile-time compatibility shim at `firmware/cmake/pimoroni_mp_compat.h` for newer MicroPython `mp_handle_pending(...)` signature changes.
- BLE stability on Pico 2 W is sensitive to Pico SDK / CYW43-driver versions; prefer this repo’s vendored MicroPython baseline. See [`pico2w_ble_stability.md`](./pico2w_ble_stability.md).

## Validate on device

After flashing the resulting firmware, run:

```python
import polar_ble
print(polar_ble.version())
print(polar_ble.build_info())
print("HAS_BTSTACK:", polar_ble.HAS_BTSTACK)

h10 = polar_ble.H10(None)
print(h10.state())
print(h10.is_connected())
print(h10.stats())
```
