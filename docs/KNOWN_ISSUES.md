# Known issues

Status: Living document
Last updated: 2026-03-18

This file collects **confirmed, user-visible issues** encountered while developing the Polar H10 stack (SDK core + MicroPython module) on **Pico 2 W (RP2350 + CYW43)** using **BTstack**.

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
- Inspect telemetry (`stats()`), which is exposed by the MicroPython module but largely sourced from the SDK core:
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
- The SDK core (and therefore the MicroPython module) must treat ATT `0x05` / `0x08` (insufficient auth/authorization) as a **signal to initiate pairing/encryption and retry**.
- Ensure MTU negotiation is performed before starting high-throughput PMD streams.
- If pairing is intermittent across fresh sessions, try clearing the bond state (both sides) and re-pairing.

## KI-04 — PSFTP can fail if BTstack SM auth policy is not initialized consistently

**Observed on:** Pico 2 W + Polar H10 during PSFTP list/download validation.

### Symptoms
- PSFTP operations can appear to fail for transport reasons (timeouts / missing responses), even after discovery+CCC success.
- Typical bad signatures include:
  - write-request completion timeout,
  - no routed PSFTP RX (`rx=0`) after request TX,
  - intermittent behavior across fresh sessions.

### How to confirm
- Check whether the integration path sets BTstack SM defaults before operation:
  - IO capabilities: `IO_CAPABILITY_NO_INPUT_NO_OUTPUT`
  - auth requirements: `SM_AUTHREQ_BONDING | SM_AUTHREQ_SECURE_CONNECTION`
- If these are missing/inconsistent, PSFTP reliability can degrade.

### Mitigations / best practices
- Initialize SM auth policy consistently in all entry points (MicroPython binding + probes/examples).
- Use the shared helper in this repo:
  - `polar_sdk_btstack_sm_configure_default_central_policy()`
- Keep reconnect-on-security-failure behavior and explicit diagnostics in place for regressions.

### Current state
- Dedicated C PSFTP probe (`examples/pico_sdk_psftp`) is stable after auth-policy alignment, with repeated successful `list_dir("/")` + `download("/DEVICE.BPB")` runs.
- The earlier MicroPython sticky pairing-failed state observed during repeated PSFTP runs is now addressed by the rp2 BTstack TLV persistence fixes; see `Resolved` R-02 below.

### Investigation references
- `docs/reference/polar_psftp.md`
- `docs/howto/mpy_psftp_probe_air_capture_comparison.md`

## KI-05 — H10 HR recording start is rejected while a stopped H10 recording is still stored

**Observed on:** Polar H10 exercise-recording path via the MicroPython `polar_sdk` module.

### Symptoms
- `recording_start("hr", ...)` fails when the sensor already contains a stopped H10 recording.
- The older/raw device-visible failure was:
  - `ProtocolError: PSFTP error code 106`
- The current MicroPython behavior intentionally converts this into a clearer user-facing error:
  - `Error: delete existing recording before starting a new one`

### How to confirm
- `recording_status()` reports inactive.
- `recording_list()` is non-empty and shows at least one stopped H10 recording.
- Starting a new HR recording fails until the existing stopped recording is deleted.

### Mitigations / best practices
- Before starting a new H10 HR recording, check `recording_list()`.
- If a stopped H10 recording is present, delete it explicitly with:
  - `recording_delete(recording_id)`
- The acceptance/demo flow now treats an empty recording list as a precondition instead of deleting user/device data implicitly.

### Current state
- This behavior is currently treated as a shipped limitation / known issue for the MicroPython package.
- Unique recording ID generation is working; the remaining rejection is not explained by simple ID collision.

## Vendor-reported device issues (Polar; informational)

These issues are reported by Polar (SDK/device docs) and are included here for awareness.
They are **not necessarily defects in this repository**.

Source:
- Polar BLE SDK — `documentation/KnownIssues.md`:
  https://github.com/polarofficial/polar-ble-sdk/blob/master/documentation/KnownIssues.md
  (checked: 2026-03-02)

## Polar Verity Sense (Polar-reported)

### PVS-01 — `requestStreamSettings()` reports incorrect PPG sample rate (normal mode)
- **Firmware:** starting from 1.1.5
- **Feature:** PPG stream
- **Problem:** reading PPG settings can return **135 Hz** even though the actual stream is **55 Hz**.
- **Workaround:**
  - Streaming still works if `startOhrStreaming` is requested with 135 Hz; received data is sampled at 55 Hz.
  - In SDK mode, settings are reported correctly.
- **Fix:** fixed in Verity Sense firmware **2.1.0**.

### PVS-02 — Battery level not updated while charging over USB
- **Firmware:** fixed from 2.2.6
- **Feature:** battery status
- **Problem:** while charging, reported battery level can stay frozen at the value from charge-start.
- **Workaround:** unplug charger; battery level reporting resumes correctly.

### PVS-03 — Stream timestamps do not reflect `setLocalTime` until reboot
- **Firmware:** all
- **Feature:** stream timestamps
- **Problem:** changing local time via `setLocalTime` does not affect emitted stream timestamps until restart.
- **Workaround:** power cycle the device once after setting time.

## Polar H10 (Polar-reported)

### PH10-01 — Internal recording read can be interrupted if sensor is removed from strap
- **Firmware:** all
- **Feature:** stored internal recording
- **Problem:** H10 disconnects BLE after ~45 s when removed from strap; this can abort a long memory-read session.
- **Workaround:** keep H10 attached to strap and worn during recording download.

### PH10-02 — ECG/ACC streaming must be explicitly terminated by the client
- **Firmware:** all
- **Feature:** terminate data streaming
- **Problem:** if the client does not explicitly stop streaming, H10 can remain active until battery removal/depletion (even if removed from strap).
- **Workaround:** always terminate streaming/connection from the client before removing sensor from strap.

## Polar OH1 (Polar-reported)

### POH1-01 — Stream timestamps do not reflect `setLocalTime` until reboot
- **Firmware:** all
- **Feature:** stream timestamps
- **Problem:** changing local time via `setLocalTime` does not affect emitted stream timestamps until restart.
- **Workaround:** power cycle the device once after setting time.

### POH1-02 — `requestStreamSettings()` reports incorrect PPG sample rate
- **Firmware:** all
- **Feature:** PPG stream
- **Problem:** settings can report **130 Hz** while actual PPG stream is **135 Hz**.
- **Workaround:** starting OHR with 130 Hz still yields correctly sampled 135 Hz data.

## Resolved

### R-01 — rp2 firmware failed to boot/enumerate (early `Out of memory` panic)

**Affected path:** MicroPython rp2 builds that include C++ user modules.

**Root cause:** rp2 builds default `MICROPY_C_HEAP_SIZE=0`, so early C++ runtime allocations via wrapped `malloc` can fail during startup.

**Fix:** set non-zero C heap on affected presets (currently `MICROPY_C_HEAP_SIZE=8192` on `fw-pico2w-picographics`, `fw-rp2-1`, and `fw-rp2-1-debug`).

**Reference:** `CMakePresets.json`, `docs/howto/build_micropython_with_polar_module.md`.

### R-02 — rp2 BTstack bond persistence was lost across MicroPython soft resets

**Affected path:** Pico 2 W / rp2 / CYW43 MicroPython builds using BTstack TLV-backed bond storage.

**Symptoms before fix:**
- pairing could succeed for the current runtime,
- but the local bond database did not survive soft reset,
- repeated secured reconnects/PSFTP runs could fall back into `enc=0`, `conn_bonded=false`, and pairing-failure signatures.

**Root cause:** two issues combined:
- the default BTstack TLV bank overlapped the top-of-flash MicroPython filesystem region, and
- pico-sdk flash-safe execution rejected BTstack TLV flash writes on rp2 when core1 was dormant.

**Fix:**
- reserve the rp2 top-of-flash BTstack TLV bank away from VFS, and
- override rp2 flash-safe execution so BTstack TLV writes are allowed when core1 is dormant and still use multicore lockout when core1 is active.

**Result:** TLV headers and bond entries now persist in flash, and repeated bonded reconnects across MicroPython soft resets work on the Pico 2 W test path.

**References:**
- `patches/micropython/0012-ports-rp2-reserve-top-flash-for-btstack-tlv-bank.patch`
- `patches/micropython/0013-ports-rp2-override-pico-flash-safety-for-dormant-core1.patch`
- `docs/howto/mpy_psftp_probe_air_capture_comparison.md`
