# Known issues

Status: Living document
Last updated: 2026-02-24

This file collects **confirmed, user-visible issues** encountered while developing the Polar H10 driver on **Pico 2 W (RP2350 + CYW43)** using **BTstack**.

Rules:
- Keep entries **actionable** (symptoms → how to confirm → mitigations).
- Avoid speculation; if a cause is uncertain, say so.
- When an issue is fixed, either remove it or move it to a short “resolved” section with a link to the fix.

Platform-level Pico 2 W BLE stability notes (CYW43 bus/IRQ coupling, WiFi contention, sleep/power-cycle caveats):
- see [`howto/pico2w_ble_stability.md`](./howto/pico2w_ble_stability.md)

## KI-01 — Link drops with disconnect reason `0x08` (Connection Timeout)

**Observed on:** Pico 2 W + Polar H10, central role, BTstack (MicroPython integration path during HR validation).

### Symptoms
- Connection stalls and then disconnects.
- The disconnect reason is consistently `0x08` (**Connection Timeout**).
- Stalls were often seen within ~10–60 seconds in some MicroPython-driven runs.

### How to confirm
- Inspect driver telemetry (`stats()`):
  - `last_disconnect_reason == 0x08`, or equivalent counter.
- If you have a sniffer capture, the on-air signature is consistent with supervision timeout expiry (no graceful LL terminate, followed by empty PDUs and then silence).

### Mitigations / best practices
- Keep high-rate BLE processing off the Python callback path. This repo’s design (BLE critical path + parsing in C, pull-based Python API) is specifically intended to mitigate timing/scheduling sensitivity.
- Use conservative connection parameters (especially a reasonable supervision timeout) and make parameter updates observable.
- For test scripts, implement a watchdog (e.g. “no HR update for N seconds → reconnect”) so long runs do not require manual intervention.

## KI-02 — Heart Rate notifications can stall (0–1 notification over ~15s)

**Observed on:** Pico 2 W + Polar H10 during early HR validation.

### Symptoms
- HR notifications can be enabled and parsed, but during some runs:
  - only a single HR notification is received, then nothing for many seconds, or
  - no notifications are received even though CCC was written.

### How to confirm
- `stats()` shows `hr_notifications_total` stuck at 0 or 1 while the link is still connected.
- RR parsing is correct when frames do arrive (see `docs/reference/ble_heart_rate_measurement.md`).

### Mitigations / best practices
- Ensure the strap has good skin contact and the H10 is actively measuring.
- Avoid heavy printing/logging in notification paths.
- If stalls correlate with `KI-01` timeout disconnects, prioritize stabilizing link/connection-parameter behavior first.

## KI-03 — PMD/ECG start can fail unless pairing/encryption completes (ATT `0x05`)

**Observed on:** Pico 2 W + Polar H10 during ECG validation.

### Symptoms
- Enabling PMD notifications (CCC write) fails with ATT status `0x05` (**Insufficient Authentication**).
- Pairing events may be observed (e.g. Just Works request), but the link sometimes remains unencrypted:
  - `conn_encryption_key_size = 0`
  - `conn_bonded = false`
- When encryption does establish, PMD CCC enable + ECG start succeeds and ECG frames can be read.

### How to confirm
- `stats()` indicates CCC write failure (ATT status 5) and encryption key size remains 0.

### Mitigations / best practices
- The driver must treat ATT `0x05` / `0x08` (insufficient auth/authorization) as a **signal to initiate pairing/encryption and retry**.
- Ensure MTU negotiation is performed before starting high-throughput PMD streams.
- If pairing is intermittent across fresh sessions, try clearing the bond state (both sides) and re-pairing.

## KI-04 — PSFTP/PFTP not yet validated end-to-end on embedded

**Status:** protocol reference exists, but end-to-end embedded implementation/validation is not complete.

### Notes
- See protocol reference: `docs/reference/polar_psftp.md`.
- Ensure nanopb generation workflow is in place before implementing PSFTP on-device.

## Resolved

### R-01 — Pico 2 W app firmware failed to boot/enumerate (early `Out of memory` panic)

**Affected path:** MicroPython rp2 build with minimal Pimoroni `picographics` extras.

**Root cause:** rp2 builds default `MICROPY_C_HEAP_SIZE=0`, so early C++ runtime allocations via wrapped `malloc` failed during startup.

**Fix:** build Pimoroni profile with non-zero C heap (preset `fw-rp2-polar-picographics` sets `MICROPY_C_HEAP_SIZE=8192`).

**Reference:** `CMakePresets.json` (`fw-rp2-polar-picographics`), `docs/howto/build_micropython_with_polar_module.md`.
