# Spec — Polar H10 SDK core + MicroPython module (C core + binding)

Status: Active development (**canonical spec**)
Last updated: 2026-02-24

Targets:
- **Primary runtime:** MicroPython `rp2` on Pico 2 W (RP2350 + CYW43), BLE stack = **BTstack**
- **SDK core portability target:** same C SDK core code reusable from pure pico-sdk/BTstack apps (see `examples/pico_sdk/`)

Terminology (important):
- **SDK core**: `polar_sdk/core/` — Polar-specific, BTstack-backed C core.
- **MicroPython module/binding**: `polar_sdk/mpy/` — user module glue exposing the Python API.

---

## 1) Goal

Implement a robust Polar H10 integration where BLE critical-path handling and protocol parsing run in the **C SDK core**, with a small pull-based Python API exposed by the MicroPython module.

Primary capabilities for the current task:
- reliable connect/disconnect + retry/recovery behavior,
- Heart Rate (standard HR service) streaming,
- ECG streaming via PMD (raw frame type 0 path),
- strong diagnostics/observability via `stats()`.

PSFTP/PFTP read-only data-plane (`list_dir`, `download`) is now implemented in the baseline (validation still ongoing).

---

## 2) Scope snapshot (current vs planned)

| Area | Status | Notes |
|---|---|---|
| Transport core (scan/connect/discovery/retry) | Implemented baseline | Required-service mask supported |
| HR (0x2A37 parse + pull API) | Implemented baseline | Fixed-width tuple API |
| PMD ECG start/stop + data parse | Implemented baseline | Raw PMD frame type 0 only |
| PMD pairing/encryption retry policy | Implemented baseline | Handles ATT auth failures with retry logic |
| PSFTP service/char discovery handles | Implemented baseline | Data-plane API not exposed yet |
| PSFTP list/download API | Implemented baseline (read-only) | RFC60/RFC76 + nanopb GET path |
| IMU stream API (PMD ACC raw) | Implemented baseline | Raw PMD ACC frame type `0x01` (int16 x/y/z) |

---

## 3) Constraints and motivation

Platform observations on Pico 2 W show timing sensitivity in MicroPython BLE orchestration under load (see `../KNOWN_ISSUES.md`, plus `../howto/pico2w_ble_stability.md`).

Therefore the design is:
- no high-rate Python IRQ callback data path,
- preallocated C buffers/rings for streaming data,
- pull-based Python reads (`read_hr`, `read_ecg`, `read_imu`),
- explicit counters/state for debugging and validation.

---

## 4) Canonical protocol references

- UUIDs/services/chars: `../reference/polar_h10_gatt.md`
- Heart Rate Measurement format: `../reference/ble_heart_rate_measurement.md`
- PMD protocol details: `../reference/polar_pmd.md`
- PSFTP/PFTP framing/details: `../reference/polar_psftp.md`
- Proto source inventory: `../reference/polar_proto_sources.md`

---

## 5) Architecture (normative)

### 5.1 Layering and ownership

**Layer A — Polar SDK core (`polar_sdk/core/`)**
- Owns Polar-specific transport/policy state machines, protocol parsers, buffering, and reusable control helpers.
- Must not depend on MicroPython types/headers.
- Exposes callback-driven interfaces so host integrations provide platform operations (time, sleep, GATT ops, etc.).

**Layer B — host/stack adapters (still inside `polar_sdk/core/`)**
- BTstack decode/classification/dispatch helpers (`polar_sdk_btstack_*`, discovery/runtime adapters, SM control helpers).
- Shared by both MicroPython and pure pico-sdk hosts.

**Layer C — MicroPython binding (`polar_sdk/mpy/mod_polar_sdk.c`)**
- Thin glue only: argument parsing, object lifecycle, exception mapping, and exposure of Python API.
- No duplicate protocol parsing/state-machine logic that belongs in Layer A/B.

**Layer D — pure pico-sdk usage (`examples/pico_sdk/`)**
- Uses the same SDK core components without MicroPython.

### 5.2 “Thin binding” rule

New transport/protocol logic should be added to `polar_sdk/core/` first.
`polar_sdk/mpy/` should stay focused on:
1. Python argument marshalling,
2. invoking SDK core helpers/policies,
3. mapping SDK/core outcomes to Python exceptions,
4. exporting telemetry.

### 5.3 Runtime state model

Transport states:

`IDLE → SCANNING → CONNECTING → DISCOVERING → READY → (RECOVERING)`

`READY` feature substates (host-managed flags/counters):
- HR enabled/disabled,
- ECG enabled/disabled,
- PMD CP response wait state,
- discovery/handle availability per required service mask.

Only one active transport instance is supported at a time in the current MicroPython integration.

---

## 6) BLE security and connection behavior requirements

### 6.1 Security/pairing requirements (PMD)

PMD operations on H10 may require encrypted/authenticated links.
SDK core behavior must include:
- detecting ATT security-style failures (notably `0x05`, `0x08`, and related encryption/auth statuses),
- requesting pairing/encryption,
- retrying protected operations (especially PMD CCC enable),
- surfacing security outcomes via `stats()` (e.g., encryption key size, bonded flag, pairing counters).

See `../KNOWN_ISSUES.md` (KI-03).

### 6.2 Post-connect parameter update

After connect, request a connection-parameter update suitable for sustained streaming.

Current MicroPython baseline parameters:
- interval min: 24 units (30 ms)
- interval max: 40 units (50 ms)
- latency: 0
- supervision timeout: 600 units (6 s)

`connect()` should wait a bounded settle window for update completion before returning success.
Latest negotiated/update status must be observable in `stats()`.

---

## 7) Public Python API (canonical current baseline)

Module: `polar_sdk`

Main class: `polar_sdk.H10`

### 7.1 Lifecycle / transport
- `H10(addr: str | None = None, *, name_prefix: str | None = "Polar", required_services: int = <enabled-feature mask>)`
- `connect(timeout_ms=10000, *, required_services=-1) -> None`
  - `required_services=-1` keeps current object mask.
- `disconnect() -> None`
- `is_connected() -> bool`
- `state() -> str`
- `required_services() -> int`
- `set_required_services(mask: int) -> None`

### 7.2 Heart Rate
- `start_hr() -> None`
- `stop_hr() -> None`
- `read_hr(timeout_ms=0) -> tuple | None`

Return shape:
- `(ts_ms, bpm, rr_count, rr0, rr1, rr2, rr3, contact)`
- RR values are integer milliseconds.
- Missing RR slots are `0`.
- `contact` is `0/1`.

### 7.3 ECG (PMD)
- `start_ecg(sample_rate=130) -> None`
- `stop_ecg() -> None`
- `read_ecg(max_bytes=1024, timeout_ms=0) -> bytes`

ECG byte payload format:
- packed little-endian signed `int32` samples (`<i4`),
- each sample is sign-extended from PMD ECG raw 24-bit payload,
- returned lengths are 4-byte aligned.

### 7.4 IMU (PMD ACC)
- `start_imu(sample_rate=50, range=8) -> None`
- `stop_imu() -> None`
- `read_imu(max_bytes=1024, timeout_ms=0) -> bytes`

IMU byte payload format:
- packed little-endian signed `int16` triples (`<hhh`) per sample,
- order is `(x, y, z)` in milli-g,
- returned lengths are 6-byte aligned.

### 7.5 PSFTP (read-only)
- `list_dir(path: str) -> list[tuple[str, int]]`
- `download(path: str, *, max_bytes=8192, timeout_ms=12000) -> bytes`

Current scope is GET-only request flow over PSFTP MTU characteristic.

### 7.6 Stream helpers
- `start_streams(*, ecg=False, imu=False, hr=False) -> None`
- `stop_streams(*, ecg=True, imu=True, hr=True) -> None`

### 7.7 Diagnostics
- `stats() -> dict`
- `version() -> str` (module function)

### 7.8 Module constants
- Feature flags: `FEATURE_HR`, `FEATURE_ECG`, `FEATURE_PSFTP`
- Platform flag: `HAS_BTSTACK`
- Service-mask bits: `SERVICE_HR`, `SERVICE_ECG`, `SERVICE_PSFTP`, `SERVICE_ALL`

### 7.9 Deferred (not currently part of implemented API)
- PSFTP query/notification host APIs
- PSFTP write/remove/merge APIs
- `set_log_level(...)`

---

## 8) Error model

Module exception types:
- `polar_sdk.Error`
- `polar_sdk.TimeoutError`
- `polar_sdk.NotConnectedError`
- `polar_sdk.ProtocolError`
- `polar_sdk.BufferOverflowError`

Mapping intent:
- connection attempt timeout → `TimeoutError`
- operation without active ready link → `NotConnectedError`
- ATT/PMD protocol rejection or missing required GATT path → `ProtocolError`
- transport/internal failures not fitting above → `Error`

Current ECG overflow behavior is lossy-ring with telemetry (`ecg_drop_bytes_total`), not an exception in the hot path.

---

## 9) Observability requirements (`stats()`)

`stats()` must remain rich enough to debug KI-01/KI-02/KI-03 class failures.
At minimum include:
- transport state + connect/disconnect counters,
- last HCI/ATT statuses,
- connection-update fields and latest negotiated params,
- security fields (encryption key size, bonded flag, SM counters),
- required/discovered handles for HR/PMD/PSFTP,
- HR counters + parser counters,
- ECG ring counters (`available`, parse errors, dropped bytes, high-water).

---

## 10) Acceptance criteria

### 10.1 Current task acceptance (current baseline)
1. HR-only soak: stable for 20 minutes under normal RF conditions.
2. ECG soak: stable for 10 minutes with bounded buffering and observable drop/high-water counters.
3. Forced disconnect: successful re-use/reconnect path without reboot.
4. No hard-crash/watchdog/reset/heap-failure during validation scripts.

Validation procedure reference: `../howto/validation.md`.

### 10.2 PSFTP validation acceptance (current implementation)
5. PSFTP list/download end-to-end succeeds repeatedly with the nanopb workflow in place.
6. Oversize `download(max_bytes=...)` path raises bounded-memory overflow (`BufferOverflowError`) instead of overrunning buffers.
7. Forced-disconnect during PSFTP yields clean failure and reconnect recovery.

Current caveat (to remove in follow-up): in some intensive MicroPython repeat loops, PSFTP can enter a pairing-failed state (`sm_last_pairing_status=19`, `enc=0`) that currently requires board reset for recovery.

---

## 11) Out of scope (current milestone)

- Multi-device family support beyond Polar H10 focus.
- Full PMD compressed/delta frame decoding and all measurement types.
- Full offline-recording control surface.
- Legacy API compatibility shims.
- Support of targets other than RP2350 + CYW43/RM2
