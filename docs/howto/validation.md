# How-to — validate the driver (HR + ECG soak tests)

Status: How-to
Last updated: 2026-02-24

This document describes **repeatable validation procedures** for the embedded Polar H10 driver.

It is intentionally procedural (not a roadmap). Acceptance criteria are defined in the spec.

## Prerequisites

- Hardware:
  - Pico 2 W (RP2350 + CYW43)
  - Polar H10 (powered / worn / good skin contact)
- Firmware built with required features enabled:
  - `-DPOLAR_ENABLE_HR=ON`
  - `-DPOLAR_ENABLE_ECG=ON`

Build instructions: [`build_micropython_with_polar_module.md`](./build_micropython_with_polar_module.md)

## General guidance

- Prefer *pull-based* loops (`read_hr`, `read_ecg`) and avoid heavy printing inside tight loops.
- During soak tests, keep WiFi inactive and avoid using `Pin('LED')` (onboard LED is on the CYW43 wireless chip). See [`pico2w_ble_stability.md`](./pico2w_ble_stability.md).
- When debugging failures, dump `h10.stats()` periodically so you can correlate behavior with counters.
- If you hit stability issues, consult [`../KNOWN_ISSUES.md`](../KNOWN_ISSUES.md).

## Test 1 — HR-only soak (20 minutes)

Goal: verify stable connection + repeated HR notifications for 20 minutes.

Suggested MicroPython script:

```python
import time
import polar_ble

h10 = polar_ble.H10(None, required_services=polar_ble.SERVICE_HR)
h10.connect(timeout_ms=15000)
h10.start_hr()

start = time.ticks_ms()
last_dump = start

while time.ticks_diff(time.ticks_ms(), start) < 20 * 60 * 1000:
    hr = h10.read_hr(timeout_ms=2000)
    if hr is not None:
        # hr = (ts_ms, bpm, rr_count, rr0, rr1, rr2, rr3, contact)
        pass

    # dump stats every ~30s
    now = time.ticks_ms()
    if time.ticks_diff(now, last_dump) > 30000:
        print(h10.stats())
        last_dump = now

h10.stop_hr()
h10.disconnect()
```

Pass criteria (high level):
- No unexpected disconnect.
- HR samples continue to arrive throughout the window.

## Test 2 — ECG soak (10 minutes)

Goal: verify PMD start succeeds reliably and ECG data can be read continuously with bounded buffering.

Suggested MicroPython script:

```python
import time
import polar_ble

h10 = polar_ble.H10(None, required_services=polar_ble.SERVICE_ECG)
h10.connect(timeout_ms=15000)

# ECG requires pairing/encryption on H10 in practice.
# Driver should handle this internally; if it fails, see KNOWN_ISSUES.
h10.start_ecg(sample_rate=130)

start = time.ticks_ms()
last_dump = start
bytes_total = 0

while time.ticks_diff(time.ticks_ms(), start) < 10 * 60 * 1000:
    chunk = h10.read_ecg(max_bytes=1024, timeout_ms=2000)
    if chunk:
        bytes_total += len(chunk)

    now = time.ticks_ms()
    if time.ticks_diff(now, last_dump) > 30000:
        s = h10.stats()
        print(s)
        print("bytes_total", bytes_total)
        last_dump = now

h10.stop_ecg()
h10.disconnect()
```

Pass criteria (high level):
- `start_ecg()` succeeds and ECG bytes continue to be returned.
- No unbounded growth; ring buffer high-water remains below capacity.

## Test 3 — Forced disconnect + recovery (manual)

Goal: confirm the driver recovers cleanly after a real link loss.

Procedure:
1. Start either HR or ECG streaming.
2. Force the H10 to power down:
   - remove it from the strap and wait ~1 minute.
3. Reattach the H10 to the strap.
4. Verify the driver can reconnect and resume streaming without a reboot.

Notes:
- Recovery behavior is part of the v1 acceptance criteria in the spec.
