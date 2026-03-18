# Spec — Polar SDK core + MicroPython module (C core + binding)

Status: Active development (**canonical spec**)
Last updated: 2026-03-03

Targets:
- **Primary runtime:** MicroPython `rp2` on Pico 2 W (RP2350 + CYW43), BLE stack = **BTstack**
- **SDK core portability target:** same C SDK core code reusable from pure pico-sdk/BTstack apps (see `examples/pico_sdk/`)

Related:
- API design draft: [`polar_sdk_api_design.md`](./polar_sdk_api_design.md)

Terminology (important):
- **SDK core**: `polar_sdk/core/` — Polar-specific, BTstack-backed C core.
- **MicroPython module/binding**: `polar_sdk/mpy/` — user module glue exposing the Python API.
- **Session**: one connected Polar device runtime instance.

---

## 1) Goal

Implement a robust Polar integration where BLE critical-path handling and protocol parsing run in the **C SDK core**, with a small pull-based MicroPython API on top.

Project target is now:
- feature-level parity with Polar BLE SDK capabilities,
- support for multiple Polar device families through capability discovery,
- full PMD stream support (including compressed/delta decoding paths),
- full offline-recording control surface,
- PSFTP data plane for read and mutation operations.

---

## 2) Scope snapshot (current vs target)

| Area | Status | Notes |
|---|---|---|
| Transport core (scan/connect/discovery/retry) | Implemented baseline | Capability-oriented requirement filtering supported |
| HR (0x2A37 parse + pull API) | Implemented baseline | Fixed-width tuple API |
| PMD ECG online stream | Implemented baseline | Raw frame-type-0 decode path implemented |
| PMD ACC online stream | Implemented baseline | Raw ACC frame type `0x01` implemented |
| PMD pairing/encryption retry policy | Implemented baseline | ATT auth/encryption retry behavior present |
| PMD additional stream types (PPG/PPI/GYRO/MAG/...) | In scope (planned) | Capability-gated by device |
| PMD compressed/delta frame decode | In scope (planned) | Raw + decoded formats both required |
| Multi-device family support | In scope (planned) | Runtime capability discovery is required |
| PSFTP list/download | Implemented baseline | RFC60/RFC76 + nanopb GET path |
| PSFTP mutation ops (delete/write/merge/...) | In scope (planned) | Bounded-memory operation required |
| Offline recording control surface | In scope (planned) | Start/stop/status/triggers/settings |

---

## 3) Constraints and motivation

Platform observations on Pico 2 W show timing sensitivity in MicroPython BLE orchestration under load (see `../KNOWN_ISSUES.md`, plus `../howto/pico2w_ble_stability.md`).

Therefore the design is:
- no high-rate Python IRQ callback data path,
- preallocated C buffers/rings for streaming data,
- pull-based Python reads (`read_hr`, `read_stream`, `read_stream_into`),
- conservative default ring/buffer sizing with bounded constructor-time overrides,
- no hidden heap allocation in streaming/transfer hot paths,
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
- Owns Polar-specific transport/policy state machines, protocol parsers, buffering, recording/file helpers, and reusable control helpers.
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

New transport/protocol logic must be added to `polar_sdk/core/` first.
`polar_sdk/mpy/` stays focused on:
1. Python argument marshalling,
2. invoking SDK core helpers/policies,
3. mapping SDK/core outcomes to Python exceptions,
4. exporting telemetry.

### 5.3 Runtime state model

Transport states:

`IDLE → SCANNING → CONNECTING → DISCOVERING → READY → (RECOVERING)`

`READY` feature substates are capability-driven and include:
- HR enabled/disabled,
- stream enabled/disabled per PMD kind,
- PMD CP response wait state,
- recording operation state,
- discovered handle/capability availability per required capability set.

Only one active transport instance is supported at a time in the current MicroPython integration.

### 5.4 Status normalization boundary (normative)

Core modules may use subsystem-specific result enums internally (`transport`, `pmd`, `psftp`, ...).
Public C API surfaces must normalize these to one top-level status contract (`polar_status_t`).

---

## 6) BLE security and connection behavior requirements

### 6.1 Security/pairing requirements (PMD/PSFTP/recording)

Protected Polar operations may require encrypted/authenticated links.
SDK core behavior must include:
- detecting ATT security-style failures (notably `0x05`, `0x08`, and related encryption/auth statuses),
- requesting pairing/encryption,
- retrying protected operations,
- surfacing security outcomes via `stats()` (e.g., encryption key size, bonded flag, pairing status/reason).

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

## 7) Public APIs (canonical target)

Module: `polar_sdk`

Normative C/API contract details are defined in `polar_sdk_api_design.md` sections 5.2–5.7 and 7 (capabilities schema, decoded/raw framing, recording metadata, chunked-transfer semantics, exception details) and are part of this target API.

### 7.1 C API shape (session + stream based)

The top-level C API is session-oriented and capability-driven:
- one unified status enum: `polar_status_t`,
- stream kind enum: `polar_stream_kind_t` (`HR`, `ECG`, `ACC`, `PPG`, `PPI`, `GYRO`, `MAG`, ...),
- generic stream lifecycle/read operations,
- recording + PSFTP operation groups,
- capability query APIs to determine supported device features.

### 7.2 MicroPython class and lifecycle

Main class: `polar_sdk.Device`

- `Device(addr: str | None = None, *, name_prefix: str | None = "Polar", required_capabilities: tuple[str, ...] | None = None)`
- `connect(timeout_ms=10000, *, required_capabilities: tuple[str, ...] | None = None) -> None`
  - non-`None` `required_capabilities` overrides object defaults for that connection attempt.
- `disconnect() -> None`
- `is_connected() -> bool`
- `state() -> str`
- `required_capabilities() -> tuple[str, ...]`
- `set_required_capabilities(caps: tuple[str, ...] | list[str]) -> None`
- `capabilities() -> dict`
- `stats() -> dict`

`capabilities()` must include `schema_version`.

Schema-version policy:
- pre-beta: `schema_version = 0` and schema shape may change without compatibility guarantees,
- first beta/stable release: schema is frozen at `schema_version = 1`,
- after freeze: breaking schema changes require a version bump.

`capabilities()` required top-level keys:
- `schema_version`, `device`, `streams`, `recording`, `psftp`, `security`

Public naming policy:
- old public names `H10` and `imu` are removed from the target API surface.

### 7.3 Generic stream API (canonical)

- `stream_default_config(kind: str) -> dict`
- `start_stream(kind: str, *, format: str | None = None, **cfg) -> None`
- `stop_stream(kind: str) -> None`
- `read_stream(kind: str, max_bytes=1024, timeout_ms=0) -> bytes`
- `read_stream_into(kind: str, buf, timeout_ms=0) -> int`

Canonical stream kind strings:
- `"hr"`, `"ecg"`, `"acc"`, `"ppg"`, `"ppi"`, `"gyro"`, `"mag"`

Canonical format values:
- `"decoded"` (normalized sample payload)
- `"raw"` (raw framed record path)

Format-selection rules:
- if `start_stream(..., format=None)`, stream default format from capabilities is used,
- for `hr`, v1 default format is `raw`,
- read methods use the active stream format selected by `start_stream(...)`.

Decoded format contract:
- payloads use `decoded_chunk_v1` envelope (24-byte header + packed samples),
- timestamps are Unix epoch nanoseconds,
- per-kind sample packing is fixed-width and little-endian.

Raw format contract:
- stream reads return framed records: `u16_le record_len` + `record_len` bytes,
- PMD kinds use exact PMD notification bytes as framed payload,
- HR raw mode uses exact 0x2A37 payload bytes as framed payload.

HR stream policy (v1):
- canonical decoded HR read API is `read_hr()`,
- `start_stream("hr")` / `stop_stream("hr")` are valid,
- `start_stream("hr", format="decoded")` is unsupported unless a compact binary HR stream format is explicitly specified.

Timestamp/units policy:
- outward-facing timestamps use Unix epoch,
- stream units are frozen per kind using the most common/default unit.

Decoded sample layouts (v1 target):
- `ecg`: `<i4` repeated, units `uV`
- `acc`: `<hhh` repeated (`x,y,z`), units `mg`
- `ppg`: interleaved signed int32 channels, units `counts`
- `ppi`: packed samples (`hr_bpm:u8`, `ppi_ms:u16`, `error_estimate_ms:u16`, `flags:u8`)
- `gyro`: `<iii` repeated (`x,y,z`), units `mdps`
- `mag`: `<iii` repeated (`x,y,z`), units `uT`

Concurrency policy (v1 default):
- allow `hr` + at most one PMD stream unless capability map explicitly advertises broader combinations,
- unsupported combinations raise `UnsupportedError`.

### 7.4 Typed convenience methods

Convenience wrappers over generic stream API:
- `start_hr()`, `stop_hr()`, `read_hr(timeout_ms=0) -> tuple | None`
- `start_ecg(...)`, `stop_ecg()`, `read_ecg(...) -> bytes`
- `start_acc(...)`, `stop_acc()`, `read_acc(...) -> bytes`

`acc` is canonical naming (not `imu`).

`read_hr()` return shape (current contract):
- `(ts_ms, bpm, rr_count, rr0, rr1, rr2, rr3, contact)`
- `ts_ms` is Unix epoch milliseconds.
- RR values are integer milliseconds; missing RR slots are `0`; `contact` is `0/1`.

### 7.5 Recording + PSFTP

Recording control surface (capability-gated):
- `recording_start(kind: str, **cfg) -> None`
- `recording_stop(kind: str) -> None`
- `recording_status() -> dict`
- `recording_list() -> list`
- `recording_get_settings(kind: str) -> dict`
- `recording_set_trigger(kind: str, **cfg) -> None`

Recording data model policy:
- recordings expose a stable opaque `recording_id`,
- metadata is returned via normalized dict fields,
- retrieval can be by id/path as indicated by capability map.

PSFTP/file operations (capability-gated):
- `list_dir(path: str) -> list[tuple[str, int]]`
- `download(path: str, *, max_bytes=8192, timeout_ms=12000) -> bytes`
- `delete(path: str, *, timeout_ms=12000) -> None`
- chunked-transfer path:
  - `download_open(path: str, *, timeout_ms=12000) -> int`
  - `download_read(handle: int, buf, *, timeout_ms=12000) -> int` (`0` means EOF)
  - `download_close(handle: int) -> None`
- additional write/merge/stat operations as supported by device capability map.

Chunked-transfer policy (v1):
- one active download handle per `Device` instance,
- opening while a handle is active raises `polar_sdk.Error`,
- invalid/closed handle raises `ValueError`.

### 7.6 Module functions/constants

- `version() -> str`
- Feature/platform flags (build + runtime capability gated)
- Capability constants (string-valued):
  - `CAP_STREAM_HR = "stream:hr"`
  - `CAP_STREAM_ECG = "stream:ecg"`
  - `CAP_STREAM_ACC = "stream:acc"`
  - `CAP_STREAM_PPG = "stream:ppg"`
  - `CAP_STREAM_PPI = "stream:ppi"`
  - `CAP_STREAM_GYRO = "stream:gyro"`
  - `CAP_STREAM_MAG = "stream:mag"`
  - `CAP_RECORDING = "recording"`
  - `CAP_PSFTP_READ = "psftp:read"`
  - `CAP_PSFTP_WRITE = "psftp:write"`

---

## 8) Error model

Module exception types:
- `polar_sdk.Error`
- `polar_sdk.TimeoutError`
- `polar_sdk.NotConnectedError`
- `polar_sdk.ProtocolError`
- `polar_sdk.BufferOverflowError`
- `polar_sdk.SecurityError`
- `polar_sdk.UnsupportedError`

Canonical mapping intent from `polar_status_t`:
- timeout → `TimeoutError`
- not connected/ready → `NotConnectedError`
- protocol rejection/missing required GATT path → `ProtocolError`
- bounded-memory overflow → `BufferOverflowError`
- pairing/encryption/security policy failure → `SecurityError`
- unsupported capability/operation → `UnsupportedError`
- other transport/internal failures → `Error`

Error detail policy:
- exception class names are stable,
- when available, exceptions may expose structured details such as `code`, `op`, `att_status`, `hci_status`, `pmd_status`, `psftp_error`.

Current ECG/ACC ring overflow behavior remains lossy-ring + telemetry (`*_drop_bytes_total`) for hot paths unless operation contract explicitly requires hard overflow failure.

---

## 9) Observability requirements (`stats()`)

`stats()` must remain rich enough to debug transport/security/protocol issues.
At minimum include:
- transport state + connect/disconnect counters,
- last HCI/ATT statuses,
- connection-update fields and latest negotiated params,
- security fields (encryption key size, bonded flag, SM counters),
- required/discovered handles and capability map,
- per-stream counters (notifications, frames, samples, parse errors),
- ring counters (`available`, dropped bytes, high-water) for high-rate streams,
- recording/PSFTP operation counters and last-result diagnostics.

---

## 10) Acceptance criteria

### 10.1 Transport and baseline stream acceptance
1. HR-only soak: stable for 20 minutes under normal RF conditions.
2. ECG soak: stable for 10 minutes with bounded buffering and observable drop/high-water counters.
3. ACC soak: stable for 10 minutes with bounded buffering and observable drop/high-water counters.
4. Forced disconnect: successful re-use/reconnect path without reboot.
5. No hard-crash/watchdog/reset/heap-failure during validation scripts.

Validation procedure reference: `../howto/validation.md`.

### 10.2 Expanded PMD acceptance
6. Capability query accurately reports available PMD stream kinds for tested devices.
7. Compressed/delta decode paths produce validated sample output for supported kinds/devices.
8. Raw format mode remains available for diagnostics and unsupported decode cases.

### 10.3 Recording + PSFTP acceptance
9. Recording control APIs (start/stop/status/list/trigger/settings) operate end-to-end on supported devices.
10. PSFTP list/download/delete (and other supported mutations) succeed repeatedly with bounded memory.
11. Oversize transfers raise bounded-memory overflow (`BufferOverflowError`) instead of overrunning buffers.
12. Forced-disconnect during recording/PSFTP yields clean failure and reconnect recovery.

### 10.4 Multi-device acceptance
13. At least two Polar device families are validated via the same API surface with capability gating.

---

## 11) Out of scope (current milestone)

- Legacy API compatibility shims.
- Desktop asyncio push-first API as primary interface.
- Support of targets other than RP2350 + CYW43/RM2.
