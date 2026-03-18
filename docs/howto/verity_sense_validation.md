# How-to — Minimal Polar Verity Sense validation with the MicroPython package

Status: How-to
Last updated: 2026-03-18

This guide defines the **minimal second-device validation pass** for a
**Polar Verity Sense** against the current MicroPython `polar_sdk` package.

It is intentionally **capability-driven**:
do not assume H10-only features such as ECG, PSFTP, or H10 exercise recording.
Use the runtime capability surface and validate only what the device reports.

Canonical public API behavior lives in:
- [`../specs/polar_sdk_api_design.md`](../specs/polar_sdk_api_design.md)

## 1) Preconditions

Hardware / environment:
- flashed firmware includes `polar_sdk`
- Pico 2 W / RP2350 + CYW43 board connected over USB
- Polar Verity Sense powered on and advertising
- if other Polar devices are nearby, be ready to target the Verity Sense by BLE address

Recommended:
- keep the Verity Sense close to the Pico 2 W during validation
- use OpenOCD reset if the board gets into a bad state between retries

## 2) Step 1 — Identify the device

Run:

```bash
mpremote connect /dev/ttyACM1 run examples/micropython/scan_polar.py
```

Record:
- BLE address
- advertising name
- rough RSSI

If both an H10 and Verity Sense are nearby, prefer validating by explicit
address rather than name prefix.

## 3) Step 2 — Connect and dump capabilities

Use:

```bash
mpremote connect /dev/ttyACM1 run examples/micropython/capabilities_demo.py
```

If needed, edit these constants near the top of the script before running:
- `TARGET_ADDR`
- `NAME_PREFIX`

Pass:
- connect/disconnect succeeds
- `capabilities()` returns a sensible dict
- `device.model` / `device.family` identify the Verity Sense path cleanly
- `streams.kinds` is non-empty

Record these fields in your notes:
- `device`
- `streams.kinds`
- `streams.max_parallel_pmd_streams`
- `recording.supported`
- `psftp`
- `security`

## 4) Step 3 — Run one supported live stream path

Use:

```bash
mpremote connect /dev/ttyACM1 run examples/micropython/stream_probe.py
```

Edit these constants near the top of the script before running:
- `TARGET_ADDR` when you want to pin the exact device
- `KIND` to one value listed by `capabilities()["streams"]["kinds"]`

Suggested order for the first probe:
1. `hr`
2. `acc`
3. `ppg`
4. `ppi`
5. any other advertised stream kind

Pass:
- `start_stream(kind, ...)` succeeds
- at least one non-empty `read_stream(kind, ...)` result arrives
- `stop_stream(kind)` and `disconnect()` complete cleanly

## 5) Optional step — Validate a second supported stream

If `streams.max_parallel_pmd_streams >= 2`, or if you simply want more
confidence, repeat step 3 with one more supported kind.

This is especially useful if the first successful path was only `hr` and the
device also reports PMD-backed streams such as `acc`, `ppg`, or `ppi`.

## 6) What not to assume

Do **not** automatically run H10-specific checks unless the capability map says
they are supported and applicable:
- ECG demos
- H10 HR recording demo
- PSFTP list/download demos

For Verity Sense specifically, treat these as capability-gated rather than
default validation steps.

## 7) Minimal acceptance result template

Capture the result in this form:

- device address:
- device name:
- device model/family:
- firmware:
- stream kinds:
- first validated stream kind:
- first validated stream result: pass/fail
- second validated stream kind (optional):
- recording.supported:
- psftp flags:
- notable errors / caveats:

## 8) Success bar

The minimal second-device gate is satisfied when:
- the Verity Sense connects cleanly,
- `capabilities()` reports a coherent non-H10 capability surface, and
- at least one supported live stream path works through the current public API.