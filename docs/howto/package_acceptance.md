# How-to — MicroPython package acceptance pass

Status: How-to
Last updated: 2026-03-18

This document defines the **practical package-level acceptance pass** for the
MicroPython `polar_sdk` module.

It is intentionally broader than the older stream-soak guide in
[`validation.md`](./validation.md): the goal here is to verify the **shipped
package surface** end-to-end, not just individual HR/ECG streaming paths.

Canonical public API behavior lives in:
- [`../specs/polar_sdk_api_design.md`](../specs/polar_sdk_api_design.md)

Implementation-oriented companion spec:
- [`../specs/micropython_polar_sdk_driver.md`](../specs/micropython_polar_sdk_driver.md)

## 1) Preconditions

Hardware / environment:
- flashed firmware includes `polar_sdk`
- Pico 2 W / RP2350 + CYW43 board connected over USB
- Polar H10 available and worn for secure PMD / PSFTP / recording checks
- if testing a second Polar family, know its BLE address or advertising name

Recommended before starting:
- keep Wi‑Fi inactive during BLE validation
- use OpenOCD reset when you need a hard reset between retries
- if the target address is unknown, use `examples/micropython/scan_polar.py`

## 2) Acceptance matrix

Minimum package gate on the primary H10 path:

1. import / build-info smoke
2. `Device()` lifecycle smoke
3. HR demo pass
4. ECG demo pass
5. ACC demo pass
6. PSFTP `list_dir("/")` pass
7. PSFTP one-shot `download(...)` pass
8. PSFTP chunked `download_open/read/close` pass
9. recording `start/status/stop` pass
10. secured reconnect across repeated MicroPython soft resets

Multi-device extension gate:
- repeat at least the connect + `capabilities()` + HR/basic stream checks on a
  second supported Polar device family when hardware is available.
- the package is capability-driven, so the exact feature subset may differ by
  device.

## 3) Run order

Use the examples under `examples/micropython/`.

### Step 1 — smoke test

```bash
mpremote connect /dev/ttyACM1 run examples/micropython/smoke_test.py
```

Pass:
- module imports
- `build_info()` returns metadata
- `Device().state()` is sensible (`idle` expected before connect)
- short `connect()` attempt either succeeds or cleanly raises `TimeoutError`

### Step 2 — lifecycle / basic connect

```bash
mpremote connect /dev/ttyACM1 run examples/micropython/hello_polar.py
```

Pass:
- connect/disconnect completes cleanly
- `stats()` is readable
- `required_capabilities()` shape is sane

### Step 3 — HR path

```bash
mpremote connect /dev/ttyACM1 run examples/micropython/hr_read_demo.py
```

Pass:
- `start_hr()` succeeds
- multiple non-`None` HR reads arrive
- `stop_hr()` / `disconnect()` complete cleanly

### Step 4 — ECG path

```bash
mpremote connect /dev/ttyACM1 run examples/micropython/ecg_read_demo.py
```

Pass:
- secure PMD setup succeeds
- decoded ECG bytes continue arriving
- no unexpected disconnect during the window

### Step 5 — ACC path

```bash
mpremote connect /dev/ttyACM1 run examples/micropython/acc_read_demo.py
```

Pass:
- `start_acc()` succeeds
- decoded ACC bytes continue arriving
- ACC ring counters stay bounded

### Step 6 — PSFTP list

```bash
mpremote connect /dev/ttyACM1 run examples/micropython/psftp_list_demo.py
```

Pass:
- root listing succeeds
- entries are returned as `list[dict]`
- PSFTP counters show real TX/RX activity

### Step 7 — PSFTP one-shot download

```bash
mpremote connect /dev/ttyACM1 run examples/micropython/psftp_download_demo.py
```

Pass:
- one small file downloads successfully
- returned byte length is non-zero
- no PSFTP protocol/overflow regressions are visible in stats

### Step 8 — PSFTP chunked download

```bash
mpremote connect /dev/ttyACM1 run examples/micropython/psftp_chunked_download_demo.py
```

Pass:
- `download_open()` succeeds
- repeated `download_read()` calls return positive chunk sizes
- final `download_read()` returns `0` for EOF
- transfer completes without stale-handle issues

### Step 9 — recording control

Precondition:
- no stopped H10 recording is already stored on the device
- if `recording_list()` is non-empty, delete the old recording(s) explicitly with
  `recording_delete(recording_id)` before running this step

```bash
mpremote connect /dev/ttyACM1 run examples/micropython/recording_hr_demo.py
```

Pass:
- `recording_default_config("hr")` returns a sensible dict
- `recording_start("hr", ...)` succeeds
- `recording_status()` shows active state while recording
- `recording_stop("hr")` succeeds and status returns inactive

### Step 10 — secured reconnect across soft resets

Run a security-sensitive example multiple times without hard-resetting the
board. Prefer a PSFTP or recording example because those exercise encrypted /
bonded reconnect behavior.

Example loop:

```bash
for i in $(seq 1 5); do
  echo "== run $i =="
  mpremote connect /dev/ttyACM1 run examples/micropython/psftp_list_demo.py || break
done
```

Alternative recording-oriented loop:

```bash
for i in $(seq 1 5); do
  echo "== run $i =="
  mpremote connect /dev/ttyACM1 run examples/micropython/recording_hr_demo.py || break
done
```

Use the recording-oriented loop only when you also clear the previous stopped H10
recording between runs; otherwise H10 recording start may be rejected until the
existing recording is explicitly deleted.

Pass:
- repeated runs reconnect successfully
- secure operations continue working after MicroPython soft resets
- no regression to the earlier bond-persistence failure signatures

## 4) Multi-device extension

When a second device family is available (for example H9 or Verity Sense):

1. identify the address with `scan_polar.py` if needed
2. run a minimal `Device(addr=...)` connect/disconnect cycle
3. print `capabilities()` and confirm the returned feature map matches the
   device family
4. run at least one supported live-stream path if exposed by the capability map

This extension is about verifying that the package surface is genuinely
**capability-driven**, not H10-hardcoded at the public API layer.

## 5) Failure handling notes

If a step fails:
- capture `stats()` output from the failing example
- note whether the failure is transport, PMD, PSFTP, or recording specific
- if the board is in a bad state, use OpenOCD reset before retrying
- for PSFTP/security regressions, compare against:
  - [`mpy_psftp_probe_air_capture_comparison.md`](./mpy_psftp_probe_air_capture_comparison.md)

For deeper streaming soak work, continue using:
- [`validation.md`](./validation.md)