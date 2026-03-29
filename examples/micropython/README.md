# MicroPython examples

These examples are meant to match the user-facing documentation in:

- `docs/micropython/README.md`
- `docs/micropython/quickstart.md`
- `docs/micropython/research_workflows.md`
- `docs/micropython/api_reference.md`
- `docs/micropython/data_formats.md`

General run pattern:

```bash
mpremote connect /dev/ttyACM0 fs cp examples/micropython/<script>.py :<script>.py
mpremote connect /dev/ttyACM0 run :<script>.py
```

If multiple Polar devices are nearby, first run `scan_polar.py` and then edit
`TARGET_ADDR` near the top of the script you care about.

## First steps

### `smoke_test.py`

Minimal on-device check that the `polar_sdk` module is present and importable.

### `hello_polar.py`

Smallest useful lifecycle example: create device, connect, inspect `stats()`, disconnect.

### `capabilities_demo.py`

Print the connected device’s public capability surface (`capabilities()`), stream defaults, and recording defaults.

### `scan_polar.py`

Use the built-in MicroPython `bluetooth` module to find nearby Polar advertisements and addresses.

## Research capture workflows

### `hr_csv_capture.py`

Host-captured HR CSV logger.

Typical host command:

```bash
mpremote connect /dev/ttyACM0 run :hr_csv_capture.py > hr_capture.csv
```

### `ecg_hex_capture.py`

Host-captured ECG chunk logger.

Typical host command:

```bash
mpremote connect /dev/ttyACM0 run :ecg_hex_capture.py > ecg_capture.txt
```

For the complete workflow, decoding steps, and interpretation guidance, see:

- `docs/micropython/research_workflows.md`

## Stream-specific demos

### `hr_read_demo.py`

Basic HR API demo: `start_hr()`, `read_hr()`, `stop_hr()`.

### `hr_stability_short.py`

Short HR stress/sanity check.

### `hr_passive_stats.py`

Enables HR and prints per-second counters without calling `read_hr()`.
Useful for checking whether notifications arrive at all.

### `ecg_read_demo.py`

ECG streaming demo: `start_ecg()`, `read_ecg()`, `stop_ecg()`.

### `acc_read_demo.py`

Accelerometer streaming demo: `start_acc()`, `read_acc()`, `stop_acc()`.

### `stream_probe.py`

Generic capability-driven stream probe for `hr`, `ecg`, `acc`, and future stream kinds exposed by `capabilities()`.

## PSFTP and recording demos

### `psftp_list_demo.py`

Simple PSFTP directory listing using `list_dir()`.

### `psftp_download_demo.py`

Downloads one small file into memory with `download()`.

### `psftp_chunked_download_demo.py`

Downloads one small file in pieces with `download_open()`, `download_read()`, and `download_close()`.

### `recording_hr_demo.py`

Exercises H10 HR recording control:

- `recording_default_config()`
- `recording_start()`
- `recording_status()`
- `recording_stop()`

Precondition:

- `recording_list()` should be empty before starting a new H10 HR recording
- if stopped H10 recordings already exist, delete them first with `recording_delete(recording_id)`

## Display/UI demo

### `ecg_hr_lcd_gfx_demo.py`

Renders live HR text and an ECG waveform on a supported Pimoroni PicoGraphics display.

Requirements:

- firmware includes `polar_sdk` and `picographics`
- supported display constant is available in the build

## Related docs

- `docs/KNOWN_ISSUES.md`
- `docs/howto/package_acceptance.md`
