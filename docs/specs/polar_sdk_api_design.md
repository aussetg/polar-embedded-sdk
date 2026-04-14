# API Design Specification — Polar SDK core (C) + MicroPython API

Status: Draft (iteration v0.7)
Last updated: 2026-03-06

Related:
- Canonical implementation spec: [`micropython_polar_sdk_driver.md`](./micropython_polar_sdk_driver.md)

---

## 1) Purpose

This document defines the target public API for two audiences:

1. **Embedded C developers** using `polar_sdk/core/` directly.
2. **MicroPython users** using `polar_sdk` on RP2.

The target outcome is:
- feature-level coverage for targeted Polar device families (initially H10, H9, and Verity Sense), with device-specific availability expressed through runtime capabilities,
- an embedded-first C API with explicit ownership and bounded memory behavior,
- a MicroPython API that is coherent and predictable on MCU targets,
- no public compatibility shims for older accidental names.

This document is the **public contract**. Rollout planning and migration notes are intentionally excluded.

---

## 2) Design principles

1. **One capability model, two API idioms**
   - Shared concepts across C and MicroPython.
   - Audience-appropriate ergonomics for each surface.

2. **Embedded-first C API**
   - Caller-owned storage.
   - Caller-owned output buffers.
   - Explicit init / deinit.
   - No hidden allocation in the public contract.

3. **Pythonic-for-MCU MicroPython API**
   - Small object lifecycle.
   - Clear exception behavior.
   - Pull-based reads on the hot path.
   - Explicit low-allocation paths.

4. **Thin binding discipline**
   - Protocol, parsing, stream framing, and policy live in the C core.
   - The MicroPython layer marshals arguments, bytes, small dicts/tuples, and exceptions.

5. **Capability-first public surface**
   - Query device/runtime capabilities.
   - Require capabilities explicitly when desired.
   - No public service-mask APIs.

6. **No compatibility shims**
   - Public `acc`, not `imu`.
   - Public `polar_sdk.Device`, not `H10`.

---

## 3) Normative conventions

The key words **MUST**, **MUST NOT**, **SHOULD**, and **MAY** are normative.

### 3.1 Canonical ordering

Canonical stream-kind order:
1. `hr`
2. `ecg`
3. `acc`
4. `ppg`
5. `ppi`
6. `gyro`
7. `mag`
8. `temperature`
9. `skin_temp`

This order is used for:
- capability tuples,
- `streams.kinds` (using the live-stream subset),
- `recording.kinds`,
- `active_kinds`,
- dict insertion order when dicts are keyed by kind string.

### 3.2 Stable vs intentionally flexible parts

This document freezes:
- method names and arguments,
- status/exception behavior,
- binary stream framing,
- capabilities schema,
- recording/filesystem metadata schema,
- handle / EOF / timeout semantics.

This document does **not** freeze diagnostics payloads such as `stats()` / `polar_stats_get(...)`.

---

## 4) Shared conceptual model

### 4.1 Core nouns

- **Session**: one connected device runtime.
- **Stream**: one live measurement producer (`hr`, `ecg`, `acc`, `ppg`, `ppi`, `gyro`, `mag`).
- **Stream config**: requested per-kind settings such as sample rate or range.
- **Recording**: on-device offline recording control and retrieval.
- **Raw filesystem path**: device-native PSFTP path.
- **Recording ID**: opaque stable identifier for a logical recording.
- **Live stream**: open-ended producer; it has no EOF concept.
- **Finite transfer**: file or recording retrieval; it has EOF semantics.

### 4.2 Stream kinds and codes

| Kind | MicroPython string | C enum | `decoded_chunk_v1.kind_code` |
|---|---|---|---:|
| Heart rate | `"hr"` | `POLAR_STREAM_HR` | 0 |
| ECG | `"ecg"` | `POLAR_STREAM_ECG` | 1 |
| Accelerometer | `"acc"` | `POLAR_STREAM_ACC` | 2 |
| PPG | `"ppg"` | `POLAR_STREAM_PPG` | 3 |
| PPI | `"ppi"` | `POLAR_STREAM_PPI` | 4 |
| Gyroscope | `"gyro"` | `POLAR_STREAM_GYRO` | 5 |
| Magnetometer | `"mag"` | `POLAR_STREAM_MAG` | 6 |

Recordings use a related but distinct kind namespace.
Recordable kinds are not required to equal live-stream kinds.

Known recording kind strings in v1 are:
- `"hr"`
- `"ecg"`
- `"acc"`
- `"ppg"`
- `"ppi"`
- `"gyro"`
- `"mag"`
- `"temperature"`
- `"skin_temp"`

Devices expose the supported subset via `recording.kinds`.

### 4.3 Capability identifiers

Capability strings MUST match exactly:

- `CAP_STREAM_HR` = `"stream:hr"`
- `CAP_STREAM_ECG` = `"stream:ecg"`
- `CAP_STREAM_ACC` = `"stream:acc"`
- `CAP_STREAM_PPG` = `"stream:ppg"`
- `CAP_STREAM_PPI` = `"stream:ppi"`
- `CAP_STREAM_GYRO` = `"stream:gyro"`
- `CAP_STREAM_MAG` = `"stream:mag"`
- `CAP_RECORDING` = `"recording"`
- `CAP_PSFTP_READ` = `"psftp:read"`
- `CAP_PSFTP_DELETE` = `"psftp:delete"`

### 4.4 Time model

Public time data is **source-time-first**.

Canonical time bases:
- `unknown`
- `polar_device`
- `unix`

`polar_device` means Polar device time with epoch `2000-01-01T00:00:00Z`.
It is **not** inherently trustworthy wall-clock time.

Rules:
- Canonical fields use `*_ns + time_base`.
- Unix-normalized fields such as `*_unix_ns` are optional convenience fields only.
- Absence of a trustworthy Unix-normalized value is valid.

### 4.5 Live streams vs finite transfers

This distinction is intentional:

- **Live stream reads**
  - `b""` / `0` means no complete data by deadline.
  - There is no EOF concept.

- **Finite transfer reads**
  - `0` means EOF, and only EOF.
  - Timeout before data and before EOF is an error.

### 4.6 Raw paths vs recording IDs

A raw filesystem path:
- is a device-native PSFTP path,
- uses `/` separators,
- MUST be absolute,
- is used with `list_dir`, `download`, and `delete`.

A recording ID:
- is an opaque stable identifier for a logical recording,
- MAY be path-like,
- MUST NOT be assumed to be a raw filesystem path,
- is used with logical recording metadata and control APIs.

---

## 5) C API

### 5.1 Ownership and lifetime

`polar_session_t` is a **public concrete struct defined in the header**.
Callers own its storage and may place it in static storage, on the stack, or inside a larger application struct.

Rules:
- Callers MUST initialize via `polar_session_init(...)` before use.
- Callers MUST eventually call `polar_session_deinit(...)`.
- Callers MUST treat `polar_session_t` fields as private implementation details.
- No public API in this document performs hidden heap allocation as part of its contract.

`polar_stats_t` is a diagnostics type defined in the header. Its detailed shape is intentionally implementation-defined.

### 5.2 Public C types

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
    POLAR_ERR_STATE,
    POLAR_ERR_BUSY,
    POLAR_ERR_IO,
} polar_status_t;

typedef enum {
    POLAR_SESSION_STATE_IDLE = 0,
    POLAR_SESSION_STATE_SCANNING,
    POLAR_SESSION_STATE_CONNECTING,
    POLAR_SESSION_STATE_DISCOVERING,
    POLAR_SESSION_STATE_READY,
    POLAR_SESSION_STATE_RECOVERING,
} polar_session_state_t;

typedef enum {
    POLAR_STREAM_HR = 0,
    POLAR_STREAM_ECG,
    POLAR_STREAM_ACC,
    POLAR_STREAM_PPG,
    POLAR_STREAM_PPI,
    POLAR_STREAM_GYRO,
    POLAR_STREAM_MAG,
} polar_stream_kind_t;

typedef uint32_t polar_stream_kind_mask_t;

enum {
    POLAR_STREAM_KIND_MASK_HR   = (1u << POLAR_STREAM_HR),
    POLAR_STREAM_KIND_MASK_ECG  = (1u << POLAR_STREAM_ECG),
    POLAR_STREAM_KIND_MASK_ACC  = (1u << POLAR_STREAM_ACC),
    POLAR_STREAM_KIND_MASK_PPG  = (1u << POLAR_STREAM_PPG),
    POLAR_STREAM_KIND_MASK_PPI  = (1u << POLAR_STREAM_PPI),
    POLAR_STREAM_KIND_MASK_GYRO = (1u << POLAR_STREAM_GYRO),
    POLAR_STREAM_KIND_MASK_MAG  = (1u << POLAR_STREAM_MAG),
};

typedef enum {
    POLAR_RECORDING_KIND_HR = 0,
    POLAR_RECORDING_KIND_ECG,
    POLAR_RECORDING_KIND_ACC,
    POLAR_RECORDING_KIND_PPG,
    POLAR_RECORDING_KIND_PPI,
    POLAR_RECORDING_KIND_GYRO,
    POLAR_RECORDING_KIND_MAG,
    POLAR_RECORDING_KIND_TEMPERATURE,
    POLAR_RECORDING_KIND_SKIN_TEMP,
} polar_recording_kind_t;

typedef uint32_t polar_recording_kind_mask_t;

enum {
    POLAR_RECORDING_KIND_MASK_HR          = (1u << POLAR_RECORDING_KIND_HR),
    POLAR_RECORDING_KIND_MASK_ECG         = (1u << POLAR_RECORDING_KIND_ECG),
    POLAR_RECORDING_KIND_MASK_ACC         = (1u << POLAR_RECORDING_KIND_ACC),
    POLAR_RECORDING_KIND_MASK_PPG         = (1u << POLAR_RECORDING_KIND_PPG),
    POLAR_RECORDING_KIND_MASK_PPI         = (1u << POLAR_RECORDING_KIND_PPI),
    POLAR_RECORDING_KIND_MASK_GYRO        = (1u << POLAR_RECORDING_KIND_GYRO),
    POLAR_RECORDING_KIND_MASK_MAG         = (1u << POLAR_RECORDING_KIND_MAG),
    POLAR_RECORDING_KIND_MASK_TEMPERATURE = (1u << POLAR_RECORDING_KIND_TEMPERATURE),
    POLAR_RECORDING_KIND_MASK_SKIN_TEMP   = (1u << POLAR_RECORDING_KIND_SKIN_TEMP),
};

typedef enum {
    POLAR_STREAM_FORMAT_DEFAULT = 0,
    POLAR_STREAM_FORMAT_DECODED,
    POLAR_STREAM_FORMAT_RAW,
} polar_stream_format_t;

typedef enum {
    POLAR_TIME_BASE_UNKNOWN = 0,
    POLAR_TIME_BASE_POLAR_DEVICE,
    POLAR_TIME_BASE_UNIX,
} polar_time_base_t;

typedef enum {
    POLAR_RECORDING_STATE_UNKNOWN = 0,
    POLAR_RECORDING_STATE_ACTIVE,
    POLAR_RECORDING_STATE_STOPPED,
} polar_recording_state_t;

typedef enum {
    POLAR_STREAM_CFG_SAMPLE_RATE_HZ  = (1u << 0),
    POLAR_STREAM_CFG_RESOLUTION_BITS = (1u << 1),
    POLAR_STREAM_CFG_RANGE           = (1u << 2),
    POLAR_STREAM_CFG_CHANNELS        = (1u << 3),
} polar_stream_config_field_t;

typedef struct {
    uint32_t fields;
    uint16_t sample_rate_hz;
    uint8_t resolution_bits;
    int16_t range;
    uint8_t channels;
} polar_stream_config_t;

typedef struct {
    size_t stream_ring_bytes;
    size_t transfer_bytes;
} polar_buffer_config_t;

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
    POLAR_CAP_PSFTP_DELETE = (1ull << 18),
};

#define POLAR_FS_NAME_MAX_BYTES       96
#define POLAR_PATH_MAX_BYTES          128
#define POLAR_RECORDING_ID_MAX_BYTES  128

typedef struct {
    const char *addr;        // optional BLE address string
    const char *name_prefix; // optional scan-name prefix filter
    polar_capability_mask_t required_capabilities;
    polar_buffer_config_t buffers;
} polar_session_config_t;

typedef struct {
    polar_capability_mask_t supported;
    polar_capability_mask_t active_requirements;
    uint8_t max_parallel_pmd_streams;
} polar_capabilities_t;

typedef struct {
    polar_recording_kind_mask_t active_kinds;
} polar_recording_status_t;

typedef struct {
    char name[POLAR_FS_NAME_MAX_BYTES];
    uint64_t size;
    bool is_dir;
} polar_fs_entry_t;

typedef struct {
    char recording_id[POLAR_RECORDING_ID_MAX_BYTES];
    polar_recording_kind_t kind;
    polar_recording_state_t state;
    polar_time_base_t time_base;
    uint64_t start_time_ns;
    uint64_t end_time_ns;
    uint64_t bytes_total;
    uint32_t sample_count;
    bool has_start_time;
    bool has_end_time;
    bool has_bytes_total;
    bool has_sample_count;
    char path[POLAR_PATH_MAX_BYTES]; // optional representative raw path, may be empty
} polar_recording_info_t;

typedef struct {
    uint32_t handle_id;
} polar_fs_download_handle_t;
```

### 5.3 Public C API

```c
void polar_session_config_init(polar_session_config_t *cfg);
void polar_stream_config_init(polar_stream_config_t *cfg);

polar_status_t polar_session_init(polar_session_t *s, const polar_session_config_t *cfg);
void           polar_session_deinit(polar_session_t *s);

polar_session_state_t polar_session_state(const polar_session_t *s);
bool                  polar_session_is_connected(const polar_session_t *s);

polar_status_t polar_session_connect(polar_session_t *s, uint32_t timeout_ms);
polar_status_t polar_session_disconnect(polar_session_t *s, uint32_t timeout_ms);

polar_status_t polar_session_set_required_capabilities(
    polar_session_t *s,
    polar_capability_mask_t required_mask);

polar_capability_mask_t polar_session_get_required_capabilities(
    const polar_session_t *s);

polar_status_t polar_session_get_capabilities(
    polar_session_t *s,
    polar_capabilities_t *out);

polar_status_t polar_stream_get_default_config(
    polar_session_t *s,
    polar_stream_kind_t kind,
    polar_stream_config_t *out);

polar_status_t polar_stream_start(
    polar_session_t *s,
    polar_stream_kind_t kind,
    polar_stream_format_t format,
    const polar_stream_config_t *cfg);

polar_status_t polar_stream_stop(
    polar_session_t *s,
    polar_stream_kind_t kind);

polar_status_t polar_stream_read(
    polar_session_t *s,
    polar_stream_kind_t kind,
    uint8_t *out,
    size_t out_len,
    size_t *out_n,
    uint32_t timeout_ms);

polar_status_t polar_recording_get_default_config(
    polar_session_t *s,
    polar_recording_kind_t kind,
    polar_stream_config_t *out);

polar_status_t polar_recording_start(
    polar_session_t *s,
    polar_recording_kind_t kind,
    const polar_stream_config_t *cfg);

polar_status_t polar_recording_stop(
    polar_session_t *s,
    polar_recording_kind_t kind);

polar_status_t polar_recording_status(
    polar_session_t *s,
    polar_recording_status_t *out);

polar_status_t polar_recording_list(
    polar_session_t *s,
    polar_recording_info_t *out,
    size_t out_capacity,
    size_t *out_count);

polar_status_t polar_recording_delete(
    polar_session_t *s,
    const char *recording_id);

polar_status_t polar_fs_list_dir(
    polar_session_t *s,
    const char *path,
    polar_fs_entry_t *out,
    size_t out_capacity,
    size_t *out_count);

polar_status_t polar_fs_delete(
    polar_session_t *s,
    const char *path);

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
    polar_fs_download_handle_t *handle);

polar_status_t polar_stats_get(const polar_session_t *s, polar_stats_t *out);
```

A convenience layer MAY expose typed wrappers such as `polar_hr_*`, `polar_ecg_*`, and `polar_acc_*`, but the session/stream API above is the normative C surface.

### 5.4 C semantics

#### Global argument validation

Unless explicitly stated otherwise:
- required pointer arguments MUST be non-NULL,
- required string arguments MUST be non-NULL,
- violations return `POLAR_ERR_INVALID_ARG`.

Explicit exceptions:
- `polar_session_init(s, NULL)` is allowed and means use default session config,
- `cfg == NULL` for `polar_stream_start(...)` means use default live-stream config,
- `cfg == NULL` for `polar_recording_start(...)` means use default recording config.

#### Initialization and state

- `polar_session_config_init(...)` MUST initialize defaults:
  - `addr = NULL`
  - `name_prefix = NULL`
  - `required_capabilities = 0`
  - `buffers.stream_ring_bytes = 0`
  - `buffers.transfer_bytes = 0`
- `polar_session_init(...)` MUST copy config values and any strings needed after return.
- `polar_session_deinit(...)` MUST be idempotent and MUST invalidate active transfer handles and local live-stream state.
- `polar_session_connect(...)` on an already-ready session succeeds as a no-op.
- `polar_session_disconnect(...)` on an idle session succeeds as a no-op.
- Only `POLAR_SESSION_STATE_READY` guarantees normal stream / recording / filesystem operations are valid.
- `polar_session_is_connected(...)` MUST be equivalent to `polar_session_state(...) == POLAR_SESSION_STATE_READY` in v1.

Changing required capabilities while connected affects the **next** connection, not the current one.

#### Serialized control operations

Control operations and finite-transfer operations are serialized per session.

Control operations in v1 are:
- `polar_stream_start(...)`
- `polar_stream_stop(...)`
- `polar_recording_get_default_config(...)`
- `polar_recording_start(...)`
- `polar_recording_stop(...)`
- `polar_recording_status(...)`
- `polar_recording_list(...)`
- `polar_recording_delete(...)`
- `polar_fs_list_dir(...)`
- `polar_fs_delete(...)`

Rules:
- If a control operation or finite-transfer operation is already in progress, starting another returns `POLAR_ERR_BUSY`.
- Control operations use bounded implementation-defined internal timeouts.
- If a control operation exceeds its internal timeout, it returns `POLAR_ERR_TIMEOUT`.
- Operation failure MUST leave the session reusable by retry, disconnect, or reconnect.

#### Stream and recording config

Rules for `polar_stream_config_t`:
- `cfg == NULL` means use defaults.
- `cfg != NULL && cfg->fields == 0` also means use defaults.
- Each bit in `fields` declares an explicitly requested field.
- Requesting a field unsupported for the chosen kind returns `POLAR_ERR_INVALID_ARG`.

`polar_stream_get_default_config(...)` MUST:
- write only fields valid for the chosen live-stream kind,
- set `out->fields` to exactly those valid keys.

`polar_recording_get_default_config(...)` MUST:
- query recording settings independently of live-stream defaults,
- write only fields valid for the chosen recording kind,
- set `out->fields` to exactly those valid keys.

Recording defaults and accepted recording config keys are independent from live-stream defaults and accepted live-stream config keys, even when the kind names are similar.

`polar_stream_start(...)` semantics:
- `POLAR_STREAM_FORMAT_DEFAULT` means use the default format from capabilities.
- If the kind is already active with the same effective format/config, succeed as a no-op.
- If the kind is already active with a different effective format/config, return `POLAR_ERR_BUSY`.

`polar_stream_stop(...)` on an inactive kind succeeds as a no-op.

`polar_recording_start(...)` semantics:
- If the kind is already active with the same effective config, succeed as a no-op.
- If the kind is already active with a different effective config, return `POLAR_ERR_BUSY`.
- If starting another kind would exceed the effective maximum parallel recording-kind limit, return `POLAR_ERR_BUSY`.

`polar_recording_stop(...)` on an inactive kind succeeds as a no-op.

#### Buffer budgets

`polar_buffer_config_t` defines session-global internal staging budgets:
- `stream_ring_bytes`: shared live-stream budget
- `transfer_bytes`: shared filesystem / metadata staging budget

Rules:
- `0` means implementation default.
- Values below implementation minimums return `POLAR_ERR_INVALID_ARG` at init.
- Budgets are internal staging limits, not logical object-size limits.
- Effective limits MUST be reflected by capabilities.

#### Fixed-size strings and optional values

Rules:
- All fixed-size public strings MUST be NUL-terminated.
- Size limits include the terminator.
- Absent optional strings are represented by `buf[0] = '\0'`.
- If a name, path, or recording ID does not fit, the operation MUST fail with `POLAR_ERR_OVERFLOW`.
- Silent truncation is forbidden.

For `polar_recording_info_t`:
- if `has_start_time == false`, `start_time_ns MUST be 0`,
- if `has_end_time == false`, `end_time_ns MUST be 0`,
- if `has_bytes_total == false`, `bytes_total MUST be 0`,
- if `has_sample_count == false`, `sample_count MUST be 0`,
- if no timestamp fields are present, `time_base SHOULD be POLAR_TIME_BASE_UNKNOWN`.

#### Live-stream reads

`polar_stream_read(...)` is a live-stream operation.

Rules:
- `out`, `out_n`, and `out_len > 0` are required.
- `timeout_ms = 0` means immediate poll.
- `POLAR_OK` with `*out_n > 0` means one or more complete atomic units were returned.
- `POLAR_OK` with `*out_n == 0` means no complete atomic unit was available by the deadline.
- There is no EOF concept.
- If the stream is not active, return `POLAR_ERR_STATE`.
- If disconnected while waiting, return `POLAR_ERR_NOT_CONNECTED`.
- If `out_len` cannot hold one full atomic unit, return `POLAR_ERR_OVERFLOW`.

Atomic-unit rules:
- decoded mode: never split a sample,
- raw mode: never split a framed raw record.

#### Filesystem downloads

Finite transfers in v1 are raw filesystem downloads only:
- `polar_fs_download(...)`
- `polar_fs_download_open/read/close(...)`

One-shot rules:
- `out`, `out_len`, and `out_capacity > 0` are required.
- Transfer is all-or-error.
- If the object does not fit in `out_capacity`, return `POLAR_ERR_OVERFLOW`.
- No truncation and no partial-success return is allowed.

Chunked rules:
- `polar_fs_download_open(...)` requires `timeout_ms > 0`.
- `polar_fs_download_read(...)` requires `out`, `out_len`, `out_eof`, and `out_capacity > 0`.
- `timeout_ms = 0` means immediate poll.
- `*out_len == 0 && *out_eof == true` means EOF, and only EOF.
- Timeout before bytes and before EOF returns `POLAR_ERR_TIMEOUT`.
- Reaching EOF automatically closes the handle.
- Explicit close is required for non-EOF early exit.
- Invalid or already-closed handles return `POLAR_ERR_INVALID_ARG`.

v1 permits exactly **one active chunked finite-transfer handle per session**.

While a chunked finite-transfer handle is open:
- `polar_fs_download_read(...)` and `polar_fs_download_close(...)` on that handle are allowed,
- all other control operations and finite-transfer operations return `POLAR_ERR_BUSY`.

#### Robustness and recovery

Rules:
- Unexpected disconnect MUST invalidate active transfer handles and local live-stream state.
- BLE disconnect MUST NOT be interpreted as stopping on-device offline recording.
- Reconnect MUST re-discover remote state; implementations MUST NOT assume prior local state still mirrors the device.
- Cleanup after error, disconnect, or timeout MUST be idempotent.

`polar_stats_get(...)` is public, but `polar_stats_t` is intentionally implementation-defined.

---

## 6) MicroPython API

### 6.1 Module surface

Public module name:
- `polar_sdk`

The only public device class is:
- `polar_sdk.Device`

Old names such as `H10` and `imu` are not part of the target surface.

Public exception classes:
- `polar_sdk.Error`
- `polar_sdk.TimeoutError`
- `polar_sdk.NotConnectedError`
- `polar_sdk.ProtocolError`
- `polar_sdk.BufferOverflowError`
- `polar_sdk.SecurityError`
- `polar_sdk.UnsupportedError`

Capability constants are string-valued and MUST match section 4.3 exactly.

### 6.2 `Device` lifecycle

```python
h = polar_sdk.Device(
    addr=None,
    name_prefix="Polar",
    required_capabilities=(),
)

h.connect(timeout_ms=10000, *, required_capabilities=None) -> None
h.disconnect(timeout_ms=10000) -> None

h.is_connected() -> bool
h.state() -> str
h.required_capabilities() -> tuple[str, ...]
h.set_required_capabilities(caps) -> None
h.capabilities() -> dict
h.stats() -> dict
```

Rules:
- `required_capabilities=()` means no requirement filter.
- Constructor and `set_required_capabilities(...)` accept `tuple[str, ...]`, `list[str]`, or `None`.
- `set_required_capabilities(None)` clears requirements back to the empty set.
- Unknown capability strings raise `ValueError`.
- Capability strings unavailable in the current build raise `ValueError`.
- Duplicate capability entries are ignored.
- `required_capabilities()` returns a deduplicated tuple in canonical order.

`connect(...)` override semantics:
- `required_capabilities=None` means use the object-level stored requirement set.
- A non-`None` override applies only to that connect attempt.
- The stored object-level requirement set remains unchanged.

Connection rules:
- `connect(...)` requires `timeout_ms > 0`.
- `disconnect(...)` requires `timeout_ms > 0`.
- `connect(...)` on an already-ready object succeeds as a no-op.
- `disconnect(...)` on an idle object succeeds as a no-op.

Control/transfer serialization rules:
- control operations and finite-transfer operations are serialized per device,
- control operations use bounded implementation-defined internal timeouts instead of public `timeout_ms` arguments,
- if a control operation or finite-transfer operation is already in progress, another such operation raises `polar_sdk.Error`,
- operation failure MUST leave the object reusable via retry, disconnect, or reconnect.

### 6.3 State strings

`state()` returns one of these exact lowercase strings:
- `"idle"`
- `"scanning"`
- `"connecting"`
- `"discovering"`
- `"ready"`
- `"recovering"`

Rules:
- Only `"ready"` guarantees normal stream / recording / filesystem operations are valid.
- `is_connected()` MUST be equivalent to `state() == "ready"` in v1.

### 6.4 `capabilities()` schema (normative)

`capabilities()` requires `state() == "ready"`. Otherwise it raises `polar_sdk.NotConnectedError`.

Top-level keys:
- `schema_version: int`
- `device: dict`
- `streams: dict`
- `recording: dict`
- `psftp: dict`
- `security: dict`

Versioning policy:
- Current firmware uses `schema_version == 1`.
- Breaking schema changes require an intentional version bump.

#### `device`

Required keys:
- `id: str | None`
- `address: str | None`
- `model: str | None`
- `family: str | None`
- `firmware: str | None`

#### `streams`

Required keys:
- `kinds: list[str]`
- `max_parallel_pmd_streams: int`
- `by_kind: dict[str, dict]`

Rules:
- `kinds` lists supported live-stream kinds only, in canonical order.
- `by_kind` MUST contain exactly the same keys as `kinds`, inserted in canonical order.

Each `streams.by_kind[kind]` requires:
- `formats: list[str]` (`"decoded"`, `"raw"` subset)
- `default_format: str`
- `units: str | None`
- `decoded_sample_size_bytes: int | None`

Optional key:
- `config: dict`

`config` rules:
- If the kind accepts no configuration keys, `config` MUST be omitted.
- If the kind accepts configuration keys, `config` MUST be present.
- `config` MUST describe **every** accepted keyword for `start_stream(kind, **cfg)`.

Descriptor shapes:
- fixed: `{"type": "fixed", "value": int}`
- enum: `{"type": "enum", "values": [int, ...]}`
- range: `{"type": "range", "min": int, "max": int, "step": int}`

`stream_default_config(kind)` MUST return exactly the accepted config keys for that kind, populated with defaults.

v1 default-format rule:
- `streams.by_kind["hr"]["default_format"] == "decoded"`

#### `recording`

Required keys:
- `supported: bool`
- `kinds: list[str]`
- `max_parallel_kinds: int`
- `features: dict`
- `by_kind: dict[str, dict]`

Rules:
- `kinds` lists recordable kinds only, in canonical order.
- `kinds` MAY include recording-only kinds such as `"temperature"` and `"skin_temp"`.
- `by_kind` MUST contain exactly the same keys as `kinds`, inserted in canonical order.
- Multiple recording kinds MAY be active simultaneously, subject to `max_parallel_kinds`.
- `features` contains only public-API feature flags, each boolean:
  - `control`
  - `status`
  - `list`
  - `delete`

Each `recording.by_kind[kind]` requires:
- `units: str | None`

Optional key:
- `config: dict`

`config` rules:
- If the kind accepts no recording configuration keys, `config` MUST be omitted.
- If the kind accepts recording configuration keys, `config` MUST be present.
- `config` MUST describe **every** accepted keyword for `recording_start(kind, **cfg)`.

Descriptor shapes are identical to `streams.by_kind[kind].config`.

`recording_default_config(kind)` MUST return exactly the accepted recording config keys for that kind, populated with defaults.

#### `psftp`

Required keys:
- `read: bool`
- `delete: bool`
- `chunked_download: bool`
- `max_chunk_bytes: int`

`max_chunk_bytes` is the effective maximum bytes a chunked finite transfer can place into a sufficiently large user buffer.

#### `security`

Required keys:
- `bonded: bool`
- `encryption_key_size: int`
- `pairing_method: str | None`

If known, `pairing_method` SHOULD use one of:
- `"just_works"`
- `"numeric_comparison"`
- `"passkey"`

### 6.5 Generic stream API

```python
h.stream_default_config(kind: str) -> dict

h.start_stream(kind: str, *, format: str | None = None, **cfg) -> None
h.stop_stream(kind: str) -> None

h.read_stream(kind: str, max_bytes=1024, timeout_ms=0) -> bytes
h.read_stream_into(kind: str, buf, timeout_ms=0) -> int
```

`kind` values:
- `"hr"`, `"ecg"`, `"acc"`, `"ppg"`, `"ppi"`, `"gyro"`, `"mag"`

`format` values:
- `"decoded"`, `"raw"`

Rules:
- `stream_default_config(kind)` requires `state() == "ready"`.
- `format=None` means use the default format from `capabilities()`.
- Unknown kinds raise `ValueError`.
- Unsupported formats raise `polar_sdk.UnsupportedError`.
- Unknown config keys raise `ValueError`.
- Config keys not accepted for that kind raise `ValueError`.

Start/stop rules:
- starting an already-active kind with the same effective format/config succeeds as a no-op,
- starting an already-active kind with a different effective format/config raises `polar_sdk.Error`,
- stopping an inactive kind succeeds as a no-op.

`start_stream(...)` and `stop_stream(...)` are control operations and therefore use bounded implementation-defined internal timeouts.

Live-read rules:
- `max_bytes > 0` is required for `read_stream`.
- `buf` for `read_stream_into` MUST be writable and have length `> 0`.
- `timeout_ms = 0` means immediate poll.
- `read_stream(...) -> b""` means no complete atomic unit by deadline.
- `read_stream_into(...) -> 0` means no complete atomic unit by deadline.
- There is no EOF concept for live streams.
- Reading an inactive stream raises `polar_sdk.Error`.
- Disconnect during wait raises `polar_sdk.NotConnectedError`.
- Too-small output capacity for one atomic unit raises `polar_sdk.BufferOverflowError`.

Atomic-unit rules:
- decoded mode never splits a sample,
- raw mode never splits a framed raw record.

### 6.6 Typed convenience wrappers

Typed methods are **convenience wrappers over the same underlying stream machinery**.
They are not required to be byte-for-byte aliases of the generic stream methods.

Unless explicitly overridden by return-shape rules, typed convenience wrappers inherit the same connection, inactivity, timeout, and overflow semantics as the corresponding generic stream operations for that kind and format.

#### HR

```python
h.start_hr() -> None
h.stop_hr() -> None
h.read_hr(timeout_ms=0) -> tuple | None
```

Rules:
- `start_hr()` is equivalent to `start_stream("hr", format="decoded")`.
- `stop_hr()` is equivalent to `stop_stream("hr")`.
- `read_hr(...)` returns `None` if no new HR sample arrives by deadline.
- `read_hr(...)` requires decoded HR mode to be active; otherwise it raises `polar_sdk.Error`.

Tuple layout:
- `(time_base, t0_ns, bpm, flags, rr_count, rr_ms0, rr_ms1, rr_ms2, rr_ms3)`

Rules:
- `time_base` is one of `"unknown"`, `"polar_device"`, or `"unix"`.
- `t0_ns` is `int | None`; `None` means HR sample timing is unavailable or undefined.
- `read_hr(...)` intentionally exposes decoded HR sample fields and minimal timing metadata, not the full generic decoded-chunk envelope.

#### ECG

```python
h.start_ecg(**cfg) -> None
h.stop_ecg() -> None
h.read_ecg(max_bytes=1024, timeout_ms=0) -> bytes
```

Rules:
- `start_ecg(**cfg)` is equivalent to `start_stream("ecg", format="decoded", **cfg)`.
- `stop_ecg()` is equivalent to `stop_stream("ecg")`.
- `read_ecg(...)` returns decoded payload bytes only, using the ECG sample layout from section 7.1 and no generic envelope.
- `read_ecg(...) -> b""` means no complete data by deadline.
- `read_ecg(...)` requires decoded ECG mode to be active; otherwise it raises `polar_sdk.Error`.

#### ACC

```python
h.start_acc(**cfg) -> None
h.stop_acc() -> None
h.read_acc(max_bytes=1024, timeout_ms=0) -> bytes
```

Rules mirror ECG convenience behavior, using the ACC sample layout from section 7.1.

### 6.7 Recording and filesystem API

```python
h.recording_default_config(kind: str) -> dict
h.recording_start(kind: str, **cfg) -> None
h.recording_stop(kind: str) -> None
h.recording_status() -> dict
h.recording_list() -> list[dict]
h.recording_delete(recording_id: str) -> None

h.list_dir(path: str) -> list[dict]
h.download(path: str, *, max_bytes=8192, timeout_ms=12000) -> bytes
h.delete(path: str) -> None

h.download_open(path: str, *, timeout_ms=12000) -> int
h.download_read(handle: int, buf, *, timeout_ms=12000) -> int
h.download_close(handle: int) -> None
```

Rules:
- All recording/filesystem methods require `state() == "ready"`.
- Raw filesystem `path` arguments MUST be absolute device-native paths using `/` separators.
- `path` and `recording_id` arguments MUST be non-empty strings.
- `recording_default_config(kind)` returns defaults for recording control, not live-stream control.
- `recording_start(kind, **cfg)` accepts the keys described by `capabilities()["recording"]["by_kind"][kind]["config"]` when present.
- Recording config defaults and accepted keys are independent from live-stream defaults and accepted keys.
- Recording kinds are the values exposed by `capabilities()["recording"]["kinds"]` and MAY include recording-only kinds such as `"temperature"` and `"skin_temp"`.
- Unknown or unsupported recording kinds raise `ValueError` or `polar_sdk.UnsupportedError` as appropriate.
- `recording_stop(kind)` on an inactive kind succeeds as a no-op.
- Starting an already-active recording kind with the same effective config succeeds as a no-op.
- Starting an already-active recording kind with a different effective config raises `polar_sdk.Error`.
- Starting another kind when that would exceed `capabilities()["recording"]["max_parallel_kinds"]` raises `polar_sdk.Error`.

Recording / filesystem separation:
- `delete(path)` is raw filesystem-path deletion.
- `recording_delete(recording_id)` is logical-recording-aware deletion.
- `download(path)` retrieves a raw filesystem object.
- v1 does not define a public logical-recording retrieval API.

Finite-transfer rules:
- `max_bytes > 0` is required for one-shot downloads.
- one-shot downloads are all-or-error,
- if the object does not fit in `max_bytes`, raise `polar_sdk.BufferOverflowError`,
- no truncation is allowed.

Chunked finite-transfer rules:
- `buf` length MUST be `> 0`.
- `timeout_ms = 0` means immediate poll.
- `*_download_read(...) == 0` means EOF, and only EOF.
- timeout before bytes and before EOF raises `polar_sdk.TimeoutError`.
- reaching EOF automatically closes the handle.
- explicit `*_download_close(...)` is required for non-EOF early exit.
- invalid or closed handles raise `ValueError`.

v1 handle rule:
- one active chunked finite-transfer handle per `Device` instance,
- opening another while one is active raises `polar_sdk.Error`.

While a chunked download handle is open:
- `download_read(...)` and `download_close(...)` on that handle are allowed,
- all other control operations and finite-transfer operations raise `polar_sdk.Error`.

`stats()` returns implementation-defined diagnostics and is not schema-frozen by this document.

### 6.8 Error policy

Canonical mapping from `polar_status_t`:

| C status | MicroPython exception |
|---|---|
| `POLAR_OK` | no exception |
| `POLAR_ERR_TIMEOUT` | `polar_sdk.TimeoutError` |
| `POLAR_ERR_NOT_CONNECTED` | `polar_sdk.NotConnectedError` |
| `POLAR_ERR_PROTOCOL` | `polar_sdk.ProtocolError` |
| `POLAR_ERR_SECURITY` | `polar_sdk.SecurityError` |
| `POLAR_ERR_UNSUPPORTED` | `polar_sdk.UnsupportedError` |
| `POLAR_ERR_OVERFLOW` | `polar_sdk.BufferOverflowError` |
| `POLAR_ERR_INVALID_ARG` | `ValueError` |
| `POLAR_ERR_STATE` | `polar_sdk.Error` |
| `POLAR_ERR_BUSY` | `polar_sdk.Error` |
| `POLAR_ERR_IO` | `polar_sdk.Error` |

Unknown internal failures default to `polar_sdk.Error`.

Optional exception detail schema:
- `.details` MAY be attached as a dict
- when present, known keys are:
  - `code: str`
  - `op: str`
  - `att_status: int | None`
  - `hci_status: int | None`
  - `pmd_status: int | None`
  - `psftp_error: int | None`
- unknown extra keys are allowed

---

## 7) Binary and metadata contracts

### 7.1 Decoded stream binary envelope (`decoded_chunk_v1`)

When a stream is active in decoded mode, `read_stream` / `read_stream_into` use this envelope.

Fixed header: **28 bytes**, little-endian.

| Offset | Size | Type | Meaning |
|---:|---:|---|---|
| 0 | 1 | `u8` | `version` (=1) |
| 1 | 1 | `u8` | `kind_code` |
| 2 | 1 | `u8` | `unit_code` |
| 3 | 1 | `u8` | `time_base_code` |
| 4 | 1 | `u8` | `flags` |
| 5 | 3 | `u8[3]` | reserved (=0) |
| 8 | 4 | `u32` | `sample_count` |
| 12 | 8 | `i64` | `t0_ns` |
| 20 | 4 | `i32` | `dt_ns` |
| 24 | 4 | `u32` | `sample_size_bytes` |

Payload length is `sample_count * sample_size_bytes`.

Rules:
- `sample_count >= 1`
- if `flags bit0` is clear, `t0_ns` MUST be 0
- if `flags bit1` is clear, `dt_ns` MUST be 0
- reserved bytes MUST be 0

`flags` bits:
- bit0: `t0_ns` valid
- bit1: `dt_ns` valid
- bit2: source data gap/loss detected in this chunk
- bits 3..7: reserved, must be 0

`kind_code` mapping:
- `0 = hr`
- `1 = ecg`
- `2 = acc`
- `3 = ppg`
- `4 = ppi`
- `5 = gyro`
- `6 = mag`

`time_base_code` mapping:
- `0 = unknown`
- `1 = polar_device`
- `2 = unix`

`unit_code` mapping:
- `1 = bpm`
- `2 = uV`
- `3 = mg`
- `4 = counts`
- `5 = ms`
- `6 = mdps`
- `7 = uT`

Decoded payload layouts:
- `hr` (`sample_size_bytes = 12`, `unit_code = 1`)
  - `bpm:u16`
  - `flags:u8`
  - `rr_count:u8`
  - `rr_ms0:u16`, `rr_ms1:u16`, `rr_ms2:u16`, `rr_ms3:u16`
  - `flags` bits: bit0 contact_detected, bit1 contact_supported
  - `rr_count` range is `0..4`
  - unused `rr_msN` values MUST be 0
  - `dt_ns` SHOULD be 0 unless a meaningful regular interval is known
- `ecg` (`sample_size_bytes = 4`, `unit_code = 2`): repeated `<i32>`
- `acc` (`sample_size_bytes = 6`, `unit_code = 3`): repeated `<i16, i16, i16>` for `x, y, z`
- `ppg` (`sample_size_bytes = 4 * channels`, `unit_code = 4`): repeated interleaved signed int32 channel values
- `ppi` (`sample_size_bytes = 6`, `unit_code = 5`)
  - `hr_bpm:u8`, `ppi_ms:u16`, `error_estimate_ms:u16`, `flags:u8`
  - `flags` bits: bit0 blocker, bit1 skin_contact, bit2 skin_contact_supported
  - `dt_ns` SHOULD be 0 for irregular/event-driven PPI data
- `gyro` (`sample_size_bytes = 12`, `unit_code = 6`): repeated `<i32, i32, i32>` for `x, y, z`
- `mag` (`sample_size_bytes = 12`, `unit_code = 7`): repeated `<i32, i32, i32>` for `x, y, z`

### 7.2 Raw stream framing contract

When a stream is active in raw mode, `read_stream` / `read_stream_into` return one or more framed raw records.

Each raw record is:
- `u16_le record_len`
- followed by `record_len` payload bytes

Payload meaning:
- PMD kinds: exact PMD notification bytes (`measurement_type + timestamp + frame_type + payload`)
- HR raw mode: exact `0x2A37` characteristic payload bytes

Rules:
- one call may return multiple complete framed records,
- framed records MUST NOT be split across calls,
- if the caller buffer cannot hold one full framed record, the read fails with overflow.

### 7.3 File listing contract

`list_dir(path)` returns `list[dict]`.
Each dict contains at minimum:
- `name: str`
- `size: int`
- `is_dir: bool`

Rules:
- `name` is the basename only
- `name` MUST NOT contain path separators
- `size` is file size in bytes for files
- `size == 0` for directories
- `is_dir` is authoritative
- additional keys are allowed
- entry ordering is unspecified

### 7.4 Recording metadata contract

`recording_list()` returns `list[dict]`.
Each dict contains at minimum:
- `recording_id: str`
- `kind: str`
- `state: str` (`"active"`, `"stopped"`, `"unknown"`)
- `bytes_total: int | None`
- `start_time_ns: int | None`
- `time_base: str` (`"polar_device"`, `"unix"`, `"unknown"`)

Optional richer fields:
- `end_time_ns: int | None`
- `sample_count: int | None`
- `start_time_unix_ns: int | None`
- `end_time_unix_ns: int | None`
- `path: str | None`

Rules:
- `recording_list()` is intentionally shallow and cheap
- minimum fields MUST NOT require decoding full recording contents
- `recording_id` is the canonical public identifier for a logical recording
- `kind` is any value from `capabilities()["recording"]["kinds"]` and is not required to be a live-stream kind
- `path`, when present, is an optional representative raw filesystem path for debugging or low-level inspection
- `path` is not guaranteed to be a complete logical-recording retrieval path
- implementations MAY merge split backing files into one logical recording entry when that matches device/vendor semantics
- if `start_time_ns` is `None`, `time_base` SHOULD be `"unknown"`

`recording_status()` returns a dict with at minimum:
- `active: bool`
- `by_kind: dict[str, bool]`

Rules:
- `by_kind` MUST contain exactly the kinds listed by `capabilities()["recording"]["kinds"]`
- `active` is `True` iff any `by_kind` value is true
- more than one `by_kind` entry MAY be true simultaneously, subject to `capabilities()["recording"]["max_parallel_kinds"]`

Optional convenience field:
- `active_kinds: list[str]`

### 7.5 Finite transfer contract

This section applies to raw filesystem downloads only.

One-shot rules:
- transfer is all-or-error
- object larger than `max_bytes` raises overflow
- no truncation
- no partial successful return

Chunked rules:
- exactly one chunked finite-transfer handle may be active per session/device in v1
- `*_download_read(...) == 0` means EOF, and only EOF
- timeout before data and before EOF is a timeout error
- EOF automatically closes the handle
- explicit close is required for non-EOF early exit
- invalid or already-closed handles are invalid-argument errors

---

## 8) Non-goals

This document does not promise:
- binary ABI stability,
- compatibility shims for older names,
- a frozen diagnostics schema for `stats()`,
- public filesystem upload / generic write APIs,
- public typed logical-recording retrieval APIs,
- mirroring the exact asynchronous shape of the mobile Polar SDKs.

The goal is a coherent, release-worthy API contract for embedded C and MicroPython, not a direct copy of the mobile SDK surface.