# API Design Draft — Polar SDK core (C) + MicroPython API

Status: Draft (iteration v0.5)
Last updated: 2026-03-03

Related:
- Canonical implementation spec: [`micropython_polar_sdk_driver.md`](./micropython_polar_sdk_driver.md)

---

## 1) Purpose

Define a clean API strategy for **two audiences**:

1. **Embedded C developers** using `polar_sdk/core/` directly (pico-sdk style).
2. **MicroPython users** using `polar_sdk` on RP2.

Target outcome:
- support 100% of Polar BLE SDK capabilities at the **feature level**,
- while keeping each API idiomatic for its audience.

### Decisions locked in this iteration (v0.4)

- Canonical accelerometer naming is **`acc`** (not `imu`) in public APIs.
- Add **generic stream methods now** (`start_stream`, `stop_stream`, `read_stream`, `read_stream_into`) while keeping typed helpers as convenience wrappers.
- Define a unified C-facing status contract (`polar_status_t`) and a single MicroPython exception mapping table.
- Canonical MicroPython class naming is **`polar_sdk.Device`** (capability-driven, not H10-specific).
- Requirement selection is capability-oriented now (`required_capabilities`); public service-mask requirement APIs are removed.
- Old public names (`H10`, `imu`) are removed from the target API surface.
- Multi-device family support, full PMD decode coverage (including compressed/delta), and full offline-recording APIs are in scope.
- Capabilities schema, decoded stream binary envelope, recording metadata shape, and chunked-transfer semantics are specified as normative contracts in this draft.

---

## 2) Design principles (proposed)

1. **One capability model, two API styles**
   - Shared concepts (streams, settings, recording, file transfer, diagnostics).
   - Different ergonomics (embedded C vs MicroPython).

2. **C API is embedded-first (not mobile-SDK-shaped)**
   - Explicit init/config structs.
   - Caller-owned buffers.
   - Predictable memory/time behavior.
   - Return codes, no hidden allocation.

3. **MicroPython API is Pythonic-for-MCU**
   - Simple object lifecycle.
   - Clear exceptions.
   - Pull-based reads on hot data path.
   - Low-allocation options (`readinto`) for sustained streaming.

4. **Thin binding rule stays strict**
   - Protocol, parsing, policies in C core.
   - MicroPython layer only marshals arguments/results and maps errors.

5. **No compatibility shims**
   - Prefer coherent API over preserving accidental early naming.

---

## 3) Style decisions

### 3.1 Relation to official Polar BLE SDK

- Match **features and semantics**.
- Do **not** mirror mobile async/reactive API shapes 1:1.

### 3.2 Relation to Pico SDK conventions (C)

- Yes: C API should feel familiar to pico-sdk/embedded users.
- Use `*_init`, `*_start`, `*_stop`, `*_read`, `*_get_*` patterns.
- Use compact enums + structs instead of ad-hoc booleans.

### 3.3 Relation to BleakHeart conventions (MicroPython)

- Borrow naming and measurement vocabulary where useful (`ecg`, `acc`, `ppi`, etc.).
- Do not adopt asyncio queue/callback push model as primary path on MCU.
- Keep pull-first API for robustness and determinism.

---

## 4) Shared conceptual model (C + MicroPython)

### 4.1 Core nouns

- **Session**: one connected device runtime.
- **Stream**: one measurement producer (HR, ECG, ACC, PPG, PPI, GYRO, MAG, ...).
- **Stream settings**: sample rate/resolution/range/channels per stream kind.
- **Recording**: offline storage control on sensor.
- **File transfer**: PSFTP list/download/delete/fetch metadata.
- **Stats/diagnostics**: transport/security/protocol counters and last statuses.

### 4.2 Stream kind enum (shared vocabulary)

Proposed canonical kinds:
- `HR`
- `ECG`
- `ACC`
- `PPG`
- `PPI`
- `GYRO`
- `MAG`

(Exact support depends on device model and firmware; query capabilities at runtime.)

---

## 5) C API shape (proposed)

### 5.1 Top-level approach

Two levels:

1. **Protocol primitives** (already close to current `core/include/*`).
2. **Session-level façade** for host apps to reduce boilerplate and keep feature API coherent.

The façade should remain callback/ops-driven internally (for portability), with optional blocking helpers.

### 5.2 Proposed C public types

```c
typedef enum {
    POLAR_OK = 0,
    POLAR_ERR_TIMEOUT,
    POLAR_ERR_NOT_CONNECTED,
    POLAR_ERR_PROTOCOL,
    POLAR_ERR_SECURITY,
    POLAR_ERR_UNSUPPORTED,
    POLAR_ERR_OVERFLOW,
    POLAR_ERR_INVALID_ARG,
    POLAR_ERR_BUSY,
    POLAR_ERR_IO,
} polar_status_t;

typedef enum {
    POLAR_STREAM_HR = 0,
    POLAR_STREAM_ECG,
    POLAR_STREAM_ACC,
    POLAR_STREAM_PPG,
    POLAR_STREAM_PPI,
    POLAR_STREAM_GYRO,
    POLAR_STREAM_MAG,
} polar_stream_kind_t;

typedef enum {
    POLAR_STREAM_FORMAT_DEFAULT = 0,
    POLAR_STREAM_FORMAT_DECODED,
    POLAR_STREAM_FORMAT_RAW,
} polar_stream_format_t;

typedef struct {
    uint16_t sample_rate_hz;
    uint8_t resolution_bits;
    int16_t range;
    uint8_t channels;
} polar_stream_config_t;

typedef uint64_t polar_capability_mask_t;

enum {
    POLAR_CAP_STREAM_HR   = (1ull << 0),
    POLAR_CAP_STREAM_ECG  = (1ull << 1),
    POLAR_CAP_STREAM_ACC  = (1ull << 2),
    POLAR_CAP_STREAM_PPG  = (1ull << 3),
    POLAR_CAP_STREAM_PPI  = (1ull << 4),
    POLAR_CAP_STREAM_GYRO = (1ull << 5),
    POLAR_CAP_STREAM_MAG  = (1ull << 6),
    POLAR_CAP_RECORDING   = (1ull << 16),
    POLAR_CAP_PSFTP_READ  = (1ull << 17),
    POLAR_CAP_PSFTP_WRITE = (1ull << 18),
};

typedef struct {
    const char *addr;      // optional BLE address string
    const char *name_prefix; // optional scan-name filter
    polar_capability_mask_t required_capabilities;

    uint16_t ecg_ring_bytes;
    uint16_t acc_ring_bytes;
    uint16_t ppg_ring_bytes;
    uint16_t gyro_ring_bytes;
    uint16_t mag_ring_bytes;
} polar_session_config_t;

typedef struct {
    polar_capability_mask_t supported;
    polar_capability_mask_t active_requirements;
    uint8_t max_parallel_pmd_streams;
} polar_capabilities_t;
```

### 5.3 Proposed C session API

```c
polar_status_t polar_session_init(polar_session_t *s, const polar_session_config_t *cfg);
polar_status_t polar_session_connect(polar_session_t *s, uint32_t timeout_ms);
polar_status_t polar_session_disconnect(polar_session_t *s, uint32_t timeout_ms);
bool           polar_session_is_connected(const polar_session_t *s);

polar_status_t polar_session_get_capabilities(polar_session_t *s, polar_capabilities_t *out);

polar_status_t polar_stream_get_default_config(
    polar_session_t *s,
    polar_stream_kind_t kind,
    polar_stream_config_t *out);

polar_status_t polar_stream_start(
    polar_session_t *s,
    polar_stream_kind_t kind,
    polar_stream_format_t format,
    const polar_stream_config_t *cfg,
    uint32_t timeout_ms);

polar_status_t polar_stream_stop(
    polar_session_t *s,
    polar_stream_kind_t kind,
    uint32_t timeout_ms);

// byte-oriented pull (caller-owned buffer)
polar_status_t polar_stream_read(
    polar_session_t *s,
    polar_stream_kind_t kind,
    uint8_t *out,
    size_t out_len,
    size_t *out_n,
    uint32_t timeout_ms);

polar_status_t polar_stats_get(const polar_session_t *s, polar_stats_t *out);
```

### 5.4 Typed helpers (C convenience layer)

Keep typed wrappers for common use (HR/ECG/ACC), internally delegating to generic stream API:
- `polar_hr_start`, `polar_hr_read`, ...
- `polar_ecg_start`, `polar_ecg_read`, ...
- `polar_acc_start`, `polar_acc_read`, ...

### 5.5 Capability-oriented requirements (C)

```c
polar_status_t polar_session_set_required_capabilities(
    polar_session_t *s,
    polar_capability_mask_t required_mask);

polar_capability_mask_t polar_session_get_required_capabilities(
    const polar_session_t *s);
```

### 5.6 Recording + PSFTP surface (C, normative signatures)

```c
typedef struct {
    char name[96];
    uint64_t size;
    bool is_dir;
} polar_fs_entry_t;

typedef struct {
    uint32_t recording_id;
    polar_stream_kind_t kind;
    uint64_t start_time_unix_ns;
    uint64_t end_time_unix_ns;
    uint32_t sample_count;
    uint32_t bytes_total;
} polar_recording_info_t;

typedef struct {
    uint32_t handle_id;
} polar_fs_download_handle_t;

polar_status_t polar_recording_start(
    polar_session_t *s,
    polar_stream_kind_t kind,
    const polar_stream_config_t *cfg,
    uint32_t timeout_ms);

polar_status_t polar_recording_stop(
    polar_session_t *s,
    polar_stream_kind_t kind,
    uint32_t timeout_ms);

polar_status_t polar_recording_list(
    polar_session_t *s,
    polar_recording_info_t *out,
    size_t out_capacity,
    size_t *out_count,
    uint32_t timeout_ms);

polar_status_t polar_recording_delete(
    polar_session_t *s,
    uint32_t recording_id,
    uint32_t timeout_ms);

polar_status_t polar_fs_list_dir(
    polar_session_t *s,
    const char *path,
    polar_fs_entry_t *out,
    size_t out_capacity,
    size_t *out_count,
    uint32_t timeout_ms);

polar_status_t polar_fs_download(
    polar_session_t *s,
    const char *path,
    uint8_t *out,
    size_t out_capacity,
    size_t *out_len,
    uint32_t timeout_ms);

polar_status_t polar_fs_download_open(
    polar_session_t *s,
    const char *path,
    polar_fs_download_handle_t *out_handle,
    uint32_t timeout_ms);

polar_status_t polar_fs_download_read(
    polar_session_t *s,
    polar_fs_download_handle_t *handle,
    uint8_t *out,
    size_t out_capacity,
    size_t *out_len,
    bool *out_eof,
    uint32_t timeout_ms);

polar_status_t polar_fs_download_close(
    polar_session_t *s,
    polar_fs_download_handle_t *handle,
    uint32_t timeout_ms);
```

Contract notes:
- one active chunked download handle per session in v1,
- `polar_fs_download_read` returns `out_len = 0` and `out_eof = true` only at EOF,
- all transfers are bounded by caller-provided buffers.

### 5.7 Status normalization boundary (C core)

Current core modules expose several domain-specific result enums (`transport`, `pmd`, `psftp`, ...). The session façade must normalize these to `polar_status_t` before returning to callers.

Rules:
- preserve domain-specific details in stats/diagnostics fields,
- return a single `polar_status_t` to API callers,
- do not leak subsystem-specific enums through the top-level API.

---

## 6) MicroPython API shape (proposed)

### 6.1 Lifecycle and capabilities

```python
h = polar_sdk.Device(
    addr=None,
    name_prefix="Polar",
    required_capabilities=(polar_sdk.CAP_STREAM_HR, polar_sdk.CAP_STREAM_ECG),
)
h.connect(timeout_ms=10000)
h.disconnect()

h.is_connected() -> bool
h.state() -> str
h.required_capabilities() -> tuple[str, ...]
h.set_required_capabilities(caps) -> None
h.capabilities() -> dict
h.stats() -> dict
```

`connect(timeout_ms=..., *, required_capabilities=None)` may optionally override the object-level requirement set for that connection attempt.

Capability constants are string-valued and must map exactly to these values:

- `CAP_STREAM_HR` = `"stream:hr"`
- `CAP_STREAM_ECG` = `"stream:ecg"`
- `CAP_STREAM_ACC` = `"stream:acc"`
- `CAP_STREAM_PPG` = `"stream:ppg"`
- `CAP_STREAM_PPI` = `"stream:ppi"`
- `CAP_STREAM_GYRO` = `"stream:gyro"`
- `CAP_STREAM_MAG` = `"stream:mag"`
- `CAP_RECORDING` = `"recording"`
- `CAP_PSFTP_READ` = `"psftp:read"`
- `CAP_PSFTP_WRITE` = `"psftp:write"`

### 6.2 Generic stream API (primary)

```python
h.stream_default_config(kind: str) -> dict
h.start_stream(kind: str, *, format: str | None = None, **cfg) -> None
h.stop_stream(kind: str) -> None

h.read_stream(kind: str, max_bytes=1024, timeout_ms=0) -> bytes
h.read_stream_into(kind: str, buf, timeout_ms=0) -> int
```

`kind` values: `"hr"`, `"ecg"`, `"acc"`, `"ppg"`, `"ppi"`, `"gyro"`, `"mag"`.

`format` values: `"decoded"`, `"raw"`.

Format-selection rules:
- if `start_stream(..., format=None)`, stream default format from capabilities is used,
- for `hr`, v1 default format is `raw`,
- read methods use the format selected by `start_stream(...)`.

v1 HR policy:
- canonical decoded HR path is `read_hr()`,
- `start_stream("hr")`/`stop_stream("hr")` are allowed,
- `start_stream("hr", format="decoded")` is unsupported unless a compact binary HR format is explicitly specified.

### 6.3 Typed conveniences (sugar)

Keep convenience methods as aliases over generic stream API:
- `start_hr`, `stop_hr`, `read_hr`
- `start_ecg`, `stop_ecg`, `read_ecg`
- `start_acc`, `stop_acc`, `read_acc`

`imu` naming is removed from the public API.

### 6.4 Return-shape policy

- **HR**: structured tuple is acceptable for low overhead.
- **High-rate streams**: bytes/readinto for minimal allocation.
- Optional future decode helpers can live in pure Python side utilities.

### 6.5 Recording + PSFTP (MicroPython)

Proposed shape:
```python
h.recording_start(kind: str, **cfg) -> None
h.recording_stop(kind: str) -> None
h.recording_status() -> dict

h.list_dir(path: str) -> list[tuple[str, int]]
h.download(path: str, *, max_bytes=8192, timeout_ms=12000) -> bytes
h.delete(path: str, *, timeout_ms=12000) -> None

# large-transfer path (chunked)
h.download_open(path: str, *, timeout_ms=12000) -> int
h.download_read(handle: int, buf, *, timeout_ms=12000) -> int
h.download_close(handle: int) -> None
```

Recording model policy: use a stable opaque `recording_id` and normalized metadata fields.

### 6.6 Error policy (MicroPython)

Module exceptions:
- `polar_sdk.Error`
- `polar_sdk.TimeoutError`
- `polar_sdk.NotConnectedError`
- `polar_sdk.ProtocolError`
- `polar_sdk.BufferOverflowError`
- `polar_sdk.SecurityError` (new)
- `polar_sdk.UnsupportedError` (new)

Canonical mapping table from `polar_status_t`:

| C status | MicroPython exception |
|---|---|
| `POLAR_OK` | no exception |
| `POLAR_ERR_TIMEOUT` | `polar_sdk.TimeoutError` |
| `POLAR_ERR_NOT_CONNECTED` | `polar_sdk.NotConnectedError` |
| `POLAR_ERR_PROTOCOL` | `polar_sdk.ProtocolError` |
| `POLAR_ERR_OVERFLOW` | `polar_sdk.BufferOverflowError` |
| `POLAR_ERR_SECURITY` | `polar_sdk.SecurityError` |
| `POLAR_ERR_UNSUPPORTED` | `polar_sdk.UnsupportedError` |
| `POLAR_ERR_INVALID_ARG` | `ValueError` |
| `POLAR_ERR_BUSY` | `polar_sdk.Error` |
| `POLAR_ERR_IO` | `polar_sdk.Error` |

Any unmapped/unknown internal error must default to `polar_sdk.Error`.

Error detail policy: exception classes stay stable; optional structured detail fields (`code`, `op`, `att_status`, `hci_status`, `pmd_status`, `psftp_error`) may be attached when available.

---

## 7) Normative contract details

### 7.1 `capabilities()` schema (normative)

`capabilities()` returns a dict with required top-level keys:

- `schema_version: int`
- `device: dict`
- `streams: dict`
- `recording: dict`
- `psftp: dict`
- `security: dict`

Pre-beta policy:
- `schema_version == 0` while API is still intentionally breakable.

Freeze policy:
- first beta/stable release sets `schema_version == 1`.
- after that, breaking schema changes require bumping `schema_version`.

`device` required keys:
- `id: str` (opaque stable identifier for current bond/device)
- `address: str | None`
- `model: str`
- `family: str`
- `firmware: str | None`

`streams` required keys:
- `kinds: list[str]` (subset of `hr/ecg/acc/ppg/ppi/gyro/mag`)
- `max_parallel_pmd_streams: int`
- `by_kind: dict[str, dict]`

Each `streams.by_kind[kind]` requires:
- `supported: bool`
- `formats: list[str]` (`raw`, `decoded` subset)
- `default_format: str`
- `units: str`
- `decoded_sample_size_bytes: int | None`
- `config: dict` with optional keys:
  - `sample_rate_hz`, `resolution_bits`, `range`, `channels`
  - each value either `{"type": "enum", "values": [...]}`
  - or `{"type": "range", "min": int, "max": int, "step": int}`

v1 default-format rule:
- `hr.default_format` is `raw`.

`recording` required keys:
- `supported: bool`
- `kinds: list[str]`
- `features: dict` (boolean feature flags such as `trigger`, `list`, `delete`)

`psftp` required keys:
- `read: bool`
- `write: bool`
- `delete: bool`
- `chunked_download: bool`
- `max_chunk_bytes: int`

`security` required keys:
- `bonded: bool`
- `encryption_key_size: int`
- `pairing_method: str | None`

### 7.2 Decoded stream binary envelope (normative)

When a stream is started with `start_stream(..., format="decoded")`, subsequent `read_stream`/`read_stream_into` calls use this envelope:

`decoded_chunk_v1` fixed header (24 bytes, little-endian):

| Offset | Size | Type | Meaning |
|---:|---:|---|---|
| 0 | 1 | `u8` | `version` (=1) |
| 1 | 1 | `u8` | `kind_code` |
| 2 | 1 | `u8` | `unit_code` |
| 3 | 1 | `u8` | `flags` |
| 4 | 4 | `u32` | `sample_count` |
| 8 | 8 | `i64` | `t0_unix_ns` (first sample timestamp) |
| 16 | 4 | `i32` | `dt_ns` (nominal sample interval, 0 if unknown/irregular) |
| 20 | 4 | `u32` | `sample_size_bytes` |

`flags` bits:
- bit0: `t0_unix_ns` valid
- bit1: `dt_ns` valid
- bit2: gap/loss detected in source data window

Payload bytes follow immediately at offset 24:
- `payload_len = sample_count * sample_size_bytes`

`kind_code` mapping:
- `1=ecg`, `2=acc`, `3=ppg`, `4=ppi`, `5=gyro`, `6=mag`

Decoded sample layouts (payload):
- `ecg`: `<i4` repeated (`units="uV"`, `sample_size_bytes=4`)
- `acc`: `<hhh` repeated (`x,y,z`, `units="mg"`, `sample_size_bytes=6`)
- `ppg`: interleaved signed int32 channels (`units="counts"`, `sample_size_bytes=4*channels`)
- `ppi`: packed struct repeated (`sample_size_bytes=6`):
  - `hr_bpm:u8`, `ppi_ms:u16`, `error_estimate_ms:u16`, `flags:u8`
  - `flags` bits: bit0 blocker, bit1 skin_contact, bit2 skin_contact_supported
- `gyro`: `<iii` repeated (`x,y,z`, `units="mdps"`, `sample_size_bytes=12`)
- `mag`: `<iii` repeated (`x,y,z`, `units="uT"`, `sample_size_bytes=12`)

Timestamp contract:
- all timestamps are Unix epoch nanoseconds.

### 7.3 Raw stream framing contract (normative)

When a stream is started with `start_stream(..., format="raw")`, `read_stream`/`read_stream_into` return one or more framed records:
- each record is `u16_le record_len` + `record_len` bytes.

For PMD kinds, `record_len` bytes are exact PMD notification bytes (`measurement_type + timestamp + frame_type + payload`).

For `hr` raw mode, `record_len` bytes are exact 0x2A37 characteristic payload bytes.

No silent conversion from raw to decoded is allowed.

### 7.4 Recording metadata contract (normative)

`recording_list()` returns `list[dict]`, each dict containing at minimum:
- `recording_id: int`
- `kind: str`
- `start_time_unix_ns: int`
- `end_time_unix_ns: int | None`
- `sample_count: int | None`
- `bytes_total: int | None`
- `state: str` (`active` / `stopped` / `unknown`)
- `path: str | None`

`recording_status()` returns at minimum:
- `active: bool`
- `active_kind: str | None`
- `active_recording_id: int | None`

### 7.5 Chunked PSFTP transfer contract (normative)

MicroPython API:
- `download_open(path, *, timeout_ms=...) -> int` returns an opaque handle id.
- `download_read(handle, buf, *, timeout_ms=...) -> int` returns number of bytes copied into `buf`.
- `download_read(...) == 0` means EOF.
- `download_close(handle) -> None` closes handle; required for non-EOF early exits.

v1 constraints:
- one active chunked download handle per `Device` instance.
- calling `download_open` with an already-open handle raises `polar_sdk.Error`.
- invalid/closed handle raises `ValueError`.

C API follows equivalent semantics via `polar_fs_download_open/read/close`.

### 7.6 Exception detail schema (normative)

MicroPython exceptions may include `.details` (dict). When present, keys are:
- `code: str` (`POLAR_ERR_*` name)
- `op: str` (operation name, e.g. `"stream_start"`, `"download_read"`)
- `att_status: int | None`
- `hci_status: int | None`
- `pmd_status: int | None`
- `psftp_error: int | None`

Rules:
- absence of `.details` is valid,
- unknown extra keys are allowed,
- exception class selection remains governed by the mapping table in section 6.6.

---

## 8) Feature parity map (initial)

This is parity at capability level (not method name parity).

- Device info/battery: planned
- HR stream (+ RR): baseline
- PMD online streams:
  - ECG: baseline
  - ACC: baseline
  - PPG/PPI/GYRO/MAG: in scope (planned)
- PMD compressed/delta decoding: in scope (planned)
- PMD stream settings query: in scope (planned)
- Recording control: in scope (planned)
- Exercise/record retrieval via PSFTP: in progress (GET/list/download baseline)
- File mutation (delete/write/merge): in scope (planned)
- Multi-device family support via capabilities: in scope (planned)

---

## 9) Proposed immediate next steps

1. Add C public status header (`polar_status_t`) and implement subsystem-result normalization in session façade.
2. Add new MicroPython exceptions: `SecurityError`, `UnsupportedError`, wired to the canonical mapping table.
3. Implement `capabilities()` exactly per section 7.1 (including `schema_version` policy).
4. Implement canonical `Device` + generic stream APIs (`start_stream/stop_stream/read_stream/read_stream_into`) and decoded/raw contracts from sections 7.2/7.3.
5. Implement recording + PSFTP APIs (including chunked transfer semantics from sections 7.4/7.5).
6. Execute the migration plan below in order and keep this spec synchronized with landed code.

---

## 10) Migration plan (implementation order)

This plan assumes pre-beta breakability (`schema_version = 0`) and no public compatibility shims.

### Phase 1 — C public contract scaffolding

1. Add new public header(s) for the v1 API contract:
   - `polar_status_t`
   - `polar_stream_kind_t`, `polar_stream_format_t`
   - `polar_capability_mask_t` + `POLAR_CAP_*`
   - `polar_session_config_t`, `polar_capabilities_t`
2. Add session functions for capability requirements:
   - `polar_session_set_required_capabilities`
   - `polar_session_get_required_capabilities`
3. Keep existing subsystem headers internal; top-level public functions return only `polar_status_t`.

Exit criteria:
- New headers compile cleanly.
- No top-level API leaks subsystem enums.

### Phase 2 — MicroPython surface switch (public API rename)

1. Expose `polar_sdk.Device` as the only public class.
2. Remove public names `H10`, `start_imu`, `read_imu`, `stop_imu`.
3. Expose `required_capabilities()` / `set_required_capabilities(...)`.
4. Remove public service-mask requirement APIs.

Exit criteria:
- REPL/docs only show `Device` + `acc` naming.
- No public `H10`/`imu` names remain.

### Phase 3 — Capabilities schema implementation

1. Implement `capabilities()` exactly per section 7.1.
2. Emit `schema_version = 0` until first beta/stable freeze.
3. Populate per-kind `formats/default_format/units/config` entries from runtime discovery + build support.
4. Ensure `hr.default_format == "raw"`.

Exit criteria:
- `capabilities()` contains all required keys/fields with correct types.
- Capability constants map exactly to documented string values.

### Phase 4 — Stream format contracts

1. Implement decoded envelope `decoded_chunk_v1` for decoded streams.
2. Implement raw framed-record format (`u16_le len + payload`) for raw streams.
3. Enforce HR policy:
   - `start_stream("hr", format="decoded")` => `UnsupportedError` unless explicit HR decoded binary contract is added later.
4. Ensure `read_stream` and `read_stream_into` use the active stream format selected at `start_stream`.

Exit criteria:
- Binary decode/raw framing matches section 7.2/7.3 exactly.
- Per-kind units/packing match spec.

### Phase 5 — Recording + PSFTP APIs

1. Implement recording methods and metadata fields from section 7.4.
2. Implement chunked transfer methods:
   - `download_open`, `download_read`, `download_close`
3. Enforce chunked rules:
   - single active handle per `Device`,
   - `download_read == 0` means EOF,
   - invalid handle => `ValueError`.

Exit criteria:
- Large downloads work with bounded memory.
- Recording list/status contracts match spec.

### Phase 6 — Error model + details

1. Wire full `polar_status_t` to MicroPython exception mapping.
2. Add optional `.details` dict keys from section 7.6 where available.
3. Preserve stable exception class selection regardless of detail presence.

Exit criteria:
- Exception types are deterministic.
- Detail dicts (when present) use only documented keys + optional extras.

### Phase 7 — Validation and freeze prep

1. Update tests/examples/docs to capability-oriented API only.
2. Run soak + forced-disconnect + PSFTP/recording validation matrix.
3. Validate at least two Polar device families.
4. Before beta/stable: set `schema_version = 1` and freeze schema.

Exit criteria:
- Acceptance criteria in canonical spec are satisfied.
- API/docs/tests fully aligned.

---

## 11) Non-goals for this draft

- Binary compatibility commitments.
- Legacy API retention.
- Finalization of every optional field beyond the v1 minimum contracts defined in section 7.

This document is intended to converge quickly through short review iterations.
