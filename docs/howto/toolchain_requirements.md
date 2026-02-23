# How-to — toolchain requirements (firmware build)

Status: How-to
Last updated: 2026-02-25

This project builds rp2 firmware through root `CMakePresets.json` presets.

## Required tools

- `git`
- `cmake` (>= 3.25; required for workflow presets)
- `python3`
- `arm-none-eabi-gcc` + `arm-none-eabi-g++`
- build backend (`ninja` or GNU `make`)

## Recommended host tools

- `picotool` (UF2/board convenience)
- `openocd` (Pico Probe flashing/debug)
- `mpremote` (MicroPython interaction)

## Quick sanity check

```bash
git --version
cmake --version
python3 --version
arm-none-eabi-gcc --version
arm-none-eabi-g++ --version
```

(Optional)

```bash
picotool version
openocd --version
mpremote --help
```

## Notes

- Presets are the canonical interface (`cmake --preset ...`, `cmake --build --preset ...`, `cmake --workflow --preset ...`).
- If `POLAR_VERIFY_MICROPY_PATCHES=ON` (default), configure requires `git` to inspect `vendors/micropython` patch-subject history.
- If your environment changes, clear old build dirs before retrying:

```bash
rm -rf build/mpy-rp2-*
```
