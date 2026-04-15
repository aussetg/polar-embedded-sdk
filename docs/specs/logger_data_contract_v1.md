# Logger data contract v1

Status: Draft (implementation target)
Last updated: 2026-03-29

Related:
- Firmware behavior spec: [`logger_firmware_v1.md`](./logger_firmware_v1.md)
- Stable host interfaces: [`logger_host_interfaces_v1.md`](./logger_host_interfaces_v1.md)
- Polar PMD reference: [`../reference/polar_pmd.md`](../reference/polar_pmd.md)

---

## 1) Purpose

This document defines the **stable storage and upload contract** for the custom logger firmware.

It covers:

- on-device identifiers,
- SD card layout,
- `journal.bin` framing,
- immutable `manifest.json`,
- mutable live/upload state files,
- canonical upload tar construction,
- upload API v1 acknowledgment rules.

This is the contract that host tools and the upload server rely on.

---

## 2) Version table

The following schema/version values are normative for v1:

| Artifact / interface | Version |
|---|---:|
| Journal format | `1` |
| Immutable session manifest schema | `1` |
| Live-session state schema | `1` |
| Upload-queue state schema | `1` |
| Tar canonicalization version | `1` |
| Upload API version | `1` |

All stable artifacts MUST declare their own schema/version explicitly.

### 2.1 Common representation rules

Unless a field definition explicitly says otherwise:

- all JSON files are UTF-8, no BOM, LF line endings,
- fields documented as part of a stable schema MUST be present,
- if a documented field is unavailable, its value MUST be `null` rather than omitted,
- SHA-256 values are lowercase hexadecimal strings of exactly 64 characters,
- BLE addresses MUST use uppercase hex with `:` separators, e.g. `24:AC:AC:05:A3:10`.

Timezone fields use IANA timezone identifiers such as `Europe/Paris`.

Within a declared `schema_version`, producers MAY add additional fields. Consumers MUST ignore unknown fields.

### 2.2 Time representations

This document uses two distinct time representations.

#### Integer UTC timestamps

Fields ending in `_utc_ns` or named `utc_ns` are signed 64-bit integers containing Unix epoch UTC nanoseconds.

#### Integer monotonic timestamps

Fields ending in `_mono_us`, `_us`, or named `mono_us` are unsigned integers containing microseconds since the current boot on the logger monotonic clock.

#### Human-readable UTC timestamps

JSON string fields ending in `_utc` contain UTC timestamps in RFC 3339 form with trailing `Z`.

Allowed examples:

- `2026-03-29T22:41:18Z`
- `2026-03-29T22:41:18.123456789Z`

Local offsets such as `+01:00` MUST NOT be used in stable JSON artifacts.

When the wall clock is known invalid, UTC-bearing fields still store the wall-clock reading actually observed by the logger at capture time. Those values are preserved as-is and are marked as untrusted through quarantine and clock-state metadata rather than being rewritten later.

### 2.3 Study-day label rule

`study_day_local` is derived from the configured timezone using this rule:

- if local wall-clock time is `04:00:00` or later, `study_day_local` is the local calendar date,
- if local wall-clock time is earlier than `04:00:00`, `study_day_local` is the previous local calendar date.

If session creation occurs while the wall clock is invalid, `study_day_local` is still derived from the logger's current local wall-clock reading at that moment. It is therefore best-effort only and later clock corrections MUST NOT rewrite it for an already-created session.

### 2.4 Sequence and index bases

Unless explicitly documented otherwise, v1 sequence counters and indices are **zero-based**.

This applies to at least:

- `record_seq`
- `chunk_seq_in_session`
- `seq_in_span`
- `span_index_in_session`
- manifest `spans[].index_in_session`

### 2.5 Boot counter

`boot_counter` is a persistent unsigned integer incremented once on every boot before any new journal or system-log records are emitted.

It is part of the durable provenance chain and MUST NOT reset during ordinary factory reset or config changes.

The first boot recorded by stable firmware uses `boot_counter = 1`.

---

## 3) IDs and canonical names

### 3.1 Opaque IDs

The following IDs are opaque, stable identifiers:

- `session_id`
- `span_id`

For v1 they MUST be represented externally as:

- lowercase hexadecimal,
- exactly **32 hex characters**,
- representing **128 bits**.

Generation strategy is implementation-defined, but IDs MUST be unique enough that collisions are not expected in practical study use.

For v1 the recommended generation rule is UUIDv4-quality randomness or an equivalent 128-bit CSPRNG-derived identifier.

### 3.2 Other identifiers

| Field | Meaning |
|---|---|
| `hardware_id` | immutable device-derived identifier |
| `logger_id` | configured logical logger identifier |
| `subject_id` | configured subject metadata identifier |
| `bound_h10_address` | configured H10 BLE address in canonical `AA:BB:CC:DD:EE:FF` form |
| `study_day_local` | local study-day label in `YYYY-MM-DD` form using the 04:00 rollover rule |
| `firmware_version` | semantic firmware version string |
| `build_id` | opaque build identifier string |

For v1 `hardware_id` MUST be externally represented as exactly 32 lowercase hexadecimal characters representing 128 stable device-specific bits.

The recommended derivation rule is a deterministic hash/truncation of an immutable silicon or board identifier.

For v1 `firmware_version` MUST use SemVer 2.0 string syntax.

### 3.3 Reason strings

Where this document uses reason strings, they MUST be lowercase snake_case tokens.

Example reasons:

- `manual_stop`
- `charger_after_22`
- `charger_present_when_22_arrived`
- `low_battery`
- `storage_fault`
- `rollover`
- `disconnect`
- `clock_fixed`
- `clock_jump`
- `recovery_resume`

Canonical reason-token sets used elsewhere in this document are:

- session `start_reason`: `first_span_of_session`
- session `end_reason`: `manual_stop`, `charger_after_22`, `charger_present_when_22_arrived`, `rollover`, `low_battery`, `storage_fault`, `unexpected_reboot`, `service_entry`
- span `start_reason`: `session_start`, `reconnect`, `recovery_resume`, `rollover_continue`, `clock_fix_continue`, `clock_jump_continue`
- span `end_reason`: `disconnect`, `manual_stop`, `rollover`, `low_battery`, `storage_fault`, `clock_fix`, `clock_jump`, `unexpected_reboot`
- `gap_reason`: `disconnect`, `recovery_reboot`
- no-session-day `reason`: `no_h10_seen`, `no_h10_connect`, `no_ecg_stream`, `stopped_before_first_span`

Canonical quarantine-reason tokens are:

- `clock_invalid_at_start`
- `clock_fixed_mid_session`
- `clock_jump`
- `recovery_after_reset`

---

## 4) SD directory layout

The SD card root for logger-owned files is:

```text
/logger/
```

### 4.1 Session directories

Closed and active sessions live under:

```text
/logger/sessions/<study_day_local>__<session_id>/
```

Example:

```text
/logger/sessions/2026-03-29__8d3cf8d4d4564d0f83b3d2d6bb398d2a/
```

### 4.2 Session directory contents

During an active session, the directory contains:

```text
journal.bin
live.json
```

After the session is closed successfully, the directory contains exactly the immutable uploaded artifacts:

```text
journal.bin
manifest.json
```

`live.json` MUST NOT remain in a closed immutable session directory.

### 4.3 Mutable global state on SD

Mutable non-session state lives under:

```text
/logger/state/upload_queue.json
```

### 4.4 Optional exports

Manual exports MAY be written under:

```text
/logger/exports/
```

Example:

```text
/logger/exports/system-log-2026-03-29T23-14-55Z.json
```

---

## 5) Session lifecycle invariants

1. A session directory is created only when the first real ECG or ACC span starts.
2. A day with no successful PMD stream creates **no session directory** and no upload bundle.
3. Closed session artifacts are immutable.
4. Upload/prune state MUST NOT be tracked by mutating closed session artifacts.
5. Recovery after reset is append-only; existing durable journal bytes are never rewritten.
6. Session directories MUST NEVER be renamed after creation.
7. If a later clock correction would change the derived study day for subsequent data, the current session MUST be closed and later data MUST be written into a new session.

---

## 6) `journal.bin` format

### 6.1 General rules

- Byte order is **little-endian**.
- The file is append-only.
- CRC-32 in this document means the standard reflected IEEE/ZIP CRC-32 with polynomial `0x04C11DB7`, init `0xFFFFFFFF`, xorout `0xFFFFFFFF`.
- Recovery scans from the start and stops at the first incomplete or CRC-invalid record.
- A record is invalid if any of the following are true:
  - `magic` is wrong,
  - `header_bytes` is unexpected,
  - `total_bytes < header_bytes`,
  - `total_bytes != header_bytes + payload_bytes`,
  - `payload_crc32` does not match the payload bytes actually present.
- Record payloads are either:
  - UTF-8 JSON payloads for infrequent control/event records, or
  - compact binary data-chunk payloads for hot-path ECG/ACC PMD storage.

### 6.2 Journal file header (64 bytes)

| Offset | Size | Field | Value / meaning |
|---:|---:|---|---|
| `0` | `8` | `magic` | ASCII `NOF1JNL1` |
| `8` | `2` | `header_bytes` | `64` |
| `10` | `2` | `format_version` | `1` |
| `12` | `4` | `flags` | `0` in v1 |
| `16` | `16` | `session_id_raw` | 128-bit binary form of `session_id` |
| `32` | `8` | `boot_counter_at_open` | persistent boot counter value |
| `40` | `8` | `journal_open_utc_ns` | UTC nanoseconds when journal file was opened |
| `48` | `8` | `reserved` | zero in v1 |
| `56` | `4` | `header_crc32` | CRC-32 over bytes `0..55` |
| `60` | `4` | `reserved2` | zero in v1 |

### 6.3 Common record header (32 bytes)

Every record begins with this header.

| Offset | Size | Field | Meaning |
|---:|---:|---|---|
| `0` | `4` | `magic` | ASCII `RCD1` |
| `4` | `2` | `header_bytes` | `32` in v1 |
| `6` | `2` | `record_type` | see table below |
| `8` | `4` | `total_bytes` | header + payload |
| `12` | `4` | `payload_bytes` | payload length only |
| `16` | `4` | `flags` | bit 0 = JSON payload; bit 1 = binary payload |
| `20` | `4` | `payload_crc32` | CRC-32 of payload bytes |
| `24` | `8` | `record_seq` | monotonically increasing per-journal record sequence |

Exactly one of the payload-kind bits MUST be set for every record.

### 6.4 Record types

| Type | Name | Payload kind |
|---:|---|---|
| `0x0001` | `session_start` | JSON |
| `0x0002` | `span_start` | JSON |
| `0x0003` | `data_chunk` | binary |
| `0x0004` | `status_snapshot` | JSON |
| `0x0005` | `marker` | JSON |
| `0x0006` | `gap` | JSON |
| `0x0007` | `span_end` | JSON |
| `0x0008` | `session_end` | JSON |
| `0x0009` | `recovery` | JSON |
| `0x000A` | `clock_event` | JSON |
| `0x000B` | `h10_battery` | JSON |

### 6.5 JSON record payload rules

All JSON record payloads MUST:

- be UTF-8 encoded,
- use LF line endings,
- contain exactly one top-level JSON object,
- include `schema_version: 1`,
- include `record_type`,
- include `utc_ns`,
- include `mono_us`,
- include `boot_counter`.

Required extra fields by record type:

#### `session_start`

- `session_id`
- `study_day_local`
- `logger_id`
- `subject_id`
- `timezone`
- `clock_state` (`valid`, `invalid`, `jumped`)
- `start_reason`

For v1:

- `valid` means the wall clock was considered valid at session start,
- `invalid` means the session started while the wall clock was known invalid,
- `jumped` means the session started immediately after a large accepted clock correction.

#### `span_start`

- `session_id`
- `span_id`
- `span_index_in_session`
- `start_reason`
- `h10_address`
- `encrypted` (boolean)
- `bonded` (boolean)

#### `status_snapshot`

- `session_id`
- `active_span_id`
- `battery_voltage_mv`
- `battery_estimate_pct`
- `vbus_present`
- `sd_free_bytes`
- `sd_reserve_bytes`
- `wifi_enabled`
- `quarantined`
- `fault_code`

`active_span_id` is `null` when the session is active but no span is currently open.

`fault_code` is one of the canonical fault-code strings or `null`.

#### `marker`

- `session_id`
- `span_id`
- `marker_kind` (v1 uses `generic`)

#### `gap`

- `session_id`
- `ended_span_id`
- `gap_reason`

#### `span_end`

- `session_id`
- `span_id`
- `end_reason`
- `packet_count`
- `first_seq_in_span`
- `last_seq_in_span`

#### `session_end`

- `session_id`
- `end_reason`
- `span_count`
- `quarantined`
- `quarantine_reasons`

`quarantine_reasons` uses the canonical quarantine-reason tokens defined in section 3.3.

If `quarantined` is `false`, `quarantine_reasons` MUST be an empty array.

#### `recovery`

- `session_id`
- `recovery_reason`
- `previous_reset_cause`

#### `clock_event`

- `session_id`
- `event_kind` (`clock_invalid`, `clock_fixed`, `clock_jump`)
- `delta_ns`
- `old_utc_ns`
- `new_utc_ns`

#### `h10_battery`

- `session_id`
- `span_id`
- `battery_percent`
- `read_reason` (`connect`, `periodic`)

`span_id` is the current open span when one exists, otherwise `null`.

### 6.6 `data_chunk` binary payload

#### 6.6.1 Chunk header (80 bytes)

| Offset | Size | Field | Meaning |
|---:|---:|---|---|
| `0` | `2` | `stream_kind` | `1` = ECG, `2` = ACC |
| `2` | `2` | `encoding` | `1` = `raw_pmd_notification_list_v1` |
| `4` | `4` | `chunk_seq_in_session` | monotonically increasing chunk sequence |
| `8` | `16` | `span_id_raw` | binary `span_id` |
| `24` | `4` | `packet_count` | number of packet entries in this chunk |
| `28` | `4` | `first_seq_in_span` | first packet sequence number |
| `32` | `4` | `last_seq_in_span` | last packet sequence number |
| `36` | `4` | `reserved0` | zero in v1 |
| `40` | `8` | `first_mono_us` | first packet monotonic timestamp |
| `48` | `8` | `last_mono_us` | last packet monotonic timestamp |
| `56` | `8` | `first_utc_ns` | first packet UTC timestamp |
| `64` | `8` | `last_utc_ns` | last packet UTC timestamp |
| `72` | `4` | `entries_bytes` | byte length of all packet entries following |
| `76` | `4` | `reserved1` | zero in v1 |

`entries_bytes` includes per-entry padding bytes.

#### 6.6.2 Packet entry format

Each packet entry is:

1. a fixed 28-byte entry header,
2. the raw PMD notification value bytes,
3. zero padding to the next 4-byte boundary.

Entry header layout:

| Offset | Size | Field | Meaning |
|---:|---:|---|---|
| `0` | `4` | `seq_in_span` | monotonically increasing packet sequence within span |
| `4` | `4` | `flags` | zero in v1 |
| `8` | `8` | `mono_us` | logger monotonic timestamp |
| `16` | `8` | `utc_ns` | logger UTC timestamp |
| `24` | `2` | `value_len` | raw PMD value length |
| `26` | `2` | `reserved` | zero in v1 |

The raw PMD bytes are exactly the ATT characteristic value bytes received from the PMD Data characteristic. They are not post-processed ECG sample arrays.

For v1 the order of packet entries inside a chunk MUST be strictly increasing by `seq_in_span`.

### 6.7 Chunk sealing policy

The firmware SHOULD target about **64 KiB** per chunk, but MUST seal a chunk when either condition occurs first:

- the target chunk size is reached, or
- the active chunk has been open for **60 seconds**.

An open chunk MUST also be sealed before any of the following are written:

- `span_end`
- `session_end`
- a mode transition out of active logging
- orderly shutdown or reboot handoff

Packets are never split across chunks. If a single packet entry would exceed the target chunk size, the chunk containing that packet MAY exceed the target size.

---

## 7) `manifest.json` (immutable closed-session manifest)

### 7.1 General rules

`manifest.json` is UTF-8 JSON with `schema_version: 1`.

It is written exactly once when the session is closed and then treated as immutable.

For v1 the file SHOULD be serialized in a stable minified form with deterministic key order as emitted by the firmware implementation. Host tools MUST parse it semantically and MUST NOT depend on whitespace or key order.

### 7.2 Required top-level fields

The manifest MUST contain at least:

- `schema_version`
- `session_id`
- `study_day_local`
- `logger_id`
- `subject_id`
- `hardware_id`
- `firmware_version`
- `build_id`
- `journal_format_version`
- `tar_canonicalization_version`
- `timezone`
- `session`
- `spans`
- `config_snapshot`
- `h10`
- `storage`
- `files`
- `upload_bundle`

`subject_id` remains part of the immutable session artifact for study metadata and offline analysis. Upload servers MUST treat manifest `subject_id` as metadata only and MUST derive the authenticated upload subject from the bearer token instead.

### 7.3 Required nested sections

#### `session`

Must include:

- `start_utc`
- `end_utc`
- `start_reason`
- `end_reason`
- `span_count`
- `quarantined`
- `quarantine_reasons`

If `quarantined` is `false`, `quarantine_reasons` MUST be an empty array.

#### `spans`

An array of per-span summaries. Each entry MUST include:

- `span_id`
- `index_in_session`
- `start_utc`
- `end_utc`
- `start_reason`
- `end_reason`
- `packet_count`
- `first_seq_in_span`
- `last_seq_in_span`

#### `config_snapshot`

Must include effective recording policy at session time, including at least:

- `bound_h10_address`
- `timezone`
- `study_day_rollover_local`
- `overnight_upload_window_start_local`
- `overnight_upload_window_end_local`
- `critical_stop_voltage_v`
- `low_start_voltage_v`
- `off_charger_upload_voltage_v`

#### `h10`

Must include:

- `bound_address`
- `connected_address_first`
- `model_number`
- `serial_number`
- `firmware_revision`
- `battery_percent_first`
- `battery_percent_last`

#### `storage`

Must include:

- `sd_capacity_bytes`
- `sd_identity`
- `filesystem` (`fat32`)

`sd_identity` is an object containing at least:

- `manufacturer_id`
- `oem_id`
- `product_name`
- `revision`
- `serial_number`

All `sd_identity` subfields are strings, using uppercase hexadecimal where that is the natural SD-register representation.

#### `files`

Array of immutable session payload files described by the manifest. Each entry MUST include:

- `name`
- `size_bytes`
- `sha256`

For v1 `name` is the file basename inside the session directory, not a full path.

For v1 this array contains exactly one entry for `journal.bin`.

`manifest.json` is intentionally not listed here because including the manifest's own size or hash inside the manifest would create a self-reference.

#### `upload_bundle`

Must include:

- `format` (`tar`)
- `compression` (`none`)
- `canonicalization_version`

It MUST also include:

- `root_dir_name`
- `file_order`

For v1:

- `root_dir_name` is `<study_day_local>__<session_id>`
- `file_order` is `["manifest.json", "journal.bin"]`

The canonical tar SHA-256 and tar byte size are **derived values** computed over the exact tar stream described in section 10. They are not stored inside `manifest.json` because that tar stream includes `manifest.json` itself.

### 7.4 Session close derivation order

To avoid self-reference and to keep closed-session artifacts immutable, the close path for v1 MUST proceed in this order:

1. seal and flush the final journal chunk,
2. append `span_end` and `session_end` records as needed,
3. flush `journal.bin`,
4. compute `journal.bin` size and SHA-256,
5. write final `manifest.json` containing the journal metadata and upload-bundle canonicalization metadata,
6. flush and finalize `manifest.json`,
7. compute canonical tar `bundle_sha256` and `bundle_size_bytes` from `manifest.json` + `journal.bin`,
8. write those derived tar values into `upload_queue.json`.

Once `manifest.json` is written, it MUST NOT be rewritten in v1.

---

## 8) `live.json` (mutable active-session state)

`live.json` exists only while a session is active.

Updates to `live.json` MUST use replace-by-temp-file-and-rename semantics. In-place truncation/rewrite is not allowed.

Required fields:

- `schema_version`
- `session_id`
- `study_day_local`
- `session_dir`
- `state` (`active`)
- `journal_size_bytes`
- `last_flush_utc`
- `last_flush_mono_us`
- `current_span_id`
- `current_span_index`
- `quarantined`
- `clock_state`
- `boot_counter`

`current_span_id` and `current_span_index` are `null` when the session is active but no span is currently open.

This file SHOULD be flushed more aggressively than bulk journal data so crash recovery can locate the active session quickly.

For v1 `live.json` MUST be rewritten and flushed:

- when the session is created,
- when a span starts,
- when a span ends,
- when quarantine state changes,
- at least every 5 seconds while the session remains active.

---

## 9) `upload_queue.json` (mutable queue state)

`upload_queue.json` is the mutable index of closed sessions and their upload/prune state.

Updates to `upload_queue.json` MUST use replace-by-temp-file-and-rename semantics. In-place truncation/rewrite is not allowed.

Multiple queue entries MAY share the same `study_day_local`. `session_id` is the unique queue key.

Required top-level fields:

- `schema_version`
- `updated_at_utc`
- `sessions`

Each `sessions[]` entry MUST include:

- `session_id`
- `study_day_local`
- `dir_name`
- `session_start_utc`
- `session_end_utc`
- `bundle_sha256`
- `bundle_size_bytes`
- `quarantined`
- `status`
- `attempt_count`
- `last_attempt_utc`
- `last_failure_class`
- `verified_upload_utc`
- `receipt_id`

`dir_name` is the session directory basename exactly as stored on SD.

`bundle_sha256` and `bundle_size_bytes` are derived from the canonical tar described in section 10 after `manifest.json` is finalized.

On upload attempt, the immutable closed-session files are authoritative. The firmware SHOULD recompute the canonical tar hash and size and MAY repair cached queue values if they differ.

Queue processing order is oldest first by:

1. closed-session `study_day_local`,
2. `session_start_utc`,
3. `session_id` as a final stable tie-breaker.

Allowed `status` values:

- `pending`
- `uploading`
- `verified`
- `blocked_min_firmware`
- `failed`

Entries eligible for the next upload pass are those with `status` equal to `pending` or `failed`.

Entries with `status` equal to `blocked_min_firmware` or `verified` are not eligible for upload attempts.

Required queue state transition rules:

- `pending -> uploading` when an upload attempt begins,
- `uploading -> verified` only after valid server acknowledgment,
- `uploading -> failed` on any completed failed attempt,
- `failed -> uploading` on retry,
- `uploading -> blocked_min_firmware` on explicit server minimum-version rejection,
- `blocked_min_firmware -> pending` only after firmware version changes or explicit service-side queue reset.

On boot recovery, any queue entry left in `uploading` state from a previous interrupted run MUST be normalized to:

- `status: failed`
- `last_failure_class: interrupted`

`upload_queue.json` MUST be rewritten and flushed immediately after every queue state transition.

Recommended `last_failure_class` values include:

- `wifi_join_failed`
- `dns_failed`
- `tcp_failed`
- `tls_failed`
- `http_rejected`
- `hash_mismatch`
- `server_validation_failed`
- `min_firmware_rejected`
- `interrupted`
- `local_missing`
- `local_corrupt`

---

## 10) Canonical tar bundle v1

### 10.1 Contents

The canonical uploaded tar contains exactly one top-level directory whose name matches the session directory basename:

```text
<study_day_local>__<session_id>/
```

That directory contains exactly:

- `manifest.json`
- `journal.bin`

Mutable state files such as `live.json` and `upload_queue.json` MUST NOT be included.

### 10.2 Canonicalization rules

Tar generation MUST be deterministic.

For v1:

- format: plain uncompressed tar,
- tar header format: POSIX ustar,
- file order: top-level directory, then `manifest.json`, then `journal.bin`,
- directory entry name: `<root_dir_name>/` with a trailing slash,
- file entry names: `<root_dir_name>/manifest.json` and `<root_dir_name>/journal.bin`,
- path names inside tar: no leading `./`,
- directory mode: `0755`,
- file mode: `0644`,
- uid/gid: `0`,
- uname/gname: empty,
- mtime: `0`,
- file data padded with zero bytes to 512-byte tar block boundaries,
- tar stream terminated by exactly two 512-byte zero blocks,
- no xattrs,
- no pax extensions.

The declared bundle SHA-256 is computed over the exact uploaded tar byte stream.

---

## 11) Upload API v1

### 11.1 Request

The configured upload endpoint is a single HTTP(S) URL that accepts a `POST` body containing the canonical tar stream.

Required headers:

- `Content-Type: application/x-tar`
- `Content-Length: <exact tar bytes>`
- `X-Logger-Api-Version: 1`
- `X-Logger-Session-Id: <session_id>`
- `X-Logger-Hardware-Id: <hardware_id>`
- `X-Logger-Logger-Id: <logger_id>`
- `X-Logger-Study-Day: <study_day_local>`
- `X-Logger-SHA256: <bundle sha256>`
- `X-Logger-Tar-Canonicalization-Version: 1`
- `X-Logger-Manifest-Schema-Version: 1`

If configured, the request also includes:

- `x-api-key: <api_key>`
- `Authorization: Bearer <token>`

The server MUST derive the canonical upload `subject_id` from the authenticated bearer token. If a client also sends `X-Logger-Subject-Id`, the server MUST treat it as untrusted metadata and MUST NOT use it as an authorization input.

### 11.2 Success semantics

An HTTP success means the server has already:

- read the full tar stream,
- verified the declared SHA-256,
- validated the immutable session artifacts,
- accepted the bundle into canonical storage or deduplicated it safely.

### 11.3 Duplicate uploads

The server deduplication key for v1 is the tuple:

- `session_id`
- declared bundle `sha256`

If the same valid tuple is uploaded again, the server MUST respond as success, not as a hard error.

If the same `session_id` is presented with a different declared or verified `sha256`, the server MUST reject the upload as `validation_failed`.

### 11.4 Minimum-version rejection

The server MAY reject uploads from firmware older than a configured minimum version.

### 11.5 Failure responses

On non-successful requests the server SHOULD return JSON with:

- `api_version`
- `ok: false`
- `error.code`
- `error.message`
- `error.retryable`

Recommended status-code and error-code mapping for v1:

| HTTP status | `error.code` | Meaning |
|---:|---|---|
| `400` | `malformed_request` | missing headers or invalid request framing |
| `401` | `unauthorized` | auth missing/invalid |
| `403` | `forbidden` | auth valid but not allowed |
| `413` | `body_too_large` | upload exceeds server policy |
| `422` | `validation_failed` | hash mismatch or invalid session artifacts |
| `426` | `minimum_firmware` | firmware version below server minimum |
| `500` | `server_error` | internal error |
| `503` | `temporary_unavailable` | retry later |

If the firmware receives a non-successful HTTP response without parseable JSON, it MUST still classify the attempt deterministically using transport/status evidence. At minimum:

- transport-level failures map to the corresponding queue failure class,
- HTTP responses with unreadable bodies map to `http_rejected` unless a more specific class is known,
- HTTP `426` maps to `min_firmware_rejected`.

### 11.6 Acknowledgment body

On success the server returns JSON with at least:

- `api_version`
- `ok`
- `session_id`
- `sha256`
- `size_bytes`
- `receipt_id`
- `received_at_utc`
- `deduplicated` (boolean)

Example:

```json
{
  "api_version": 1,
  "ok": true,
  "session_id": "8d3cf8d4d4564d0f83b3d2d6bb398d2a",
  "sha256": "...",
  "size_bytes": 4128768,
  "receipt_id": "rcpt_20260329_000123",
  "received_at_utc": "2026-03-29T22:41:18Z",
  "deduplicated": false
}
```

---

## 12) Recovery and pruning rules

### 12.1 Recovery

On recovery after an unclean reset:

- the journal is scanned until the first invalid/incomplete record,
- trailing incomplete bytes are ignored,
- new recovery metadata is appended later,
- existing durable journal bytes are never rewritten.

Boot recovery MUST also reconcile closed session directories against `upload_queue.json`:

- if a closed session directory contains `manifest.json` and `journal.bin` but has no queue entry, a new `pending` queue entry MUST be created,
- if a queue entry points to a missing session directory, that queue entry MUST be removed and a system-log event emitted,
- if a closed session directory exists but required immutable files are malformed, the session MUST be treated as locally corrupt and the condition logged.

Stale temp files left behind by interrupted replace-and-rename updates are not part of the stable contract. Boot recovery SHOULD ignore them and MAY delete them.

### 12.2 Pruning

Pruning removes the entire closed session directory as one unit.

Only already verified-uploaded sessions are eligible for pruning.

When a session directory is pruned, the corresponding `upload_queue.json` entry MUST be removed in the same pruning transaction before the operation is considered complete.

### 12.3 Quarantined sessions

Quarantined sessions follow the same upload and retention rules as ordinary sessions unless an explicit future policy says otherwise.

---

## 13) Relationship to host tooling

Host tools MUST treat:

- `manifest.json`,
- `journal.bin`,
- the canonical tar stream,
- the upload acknowledgment body

as the stable external contract for v1 closed-session handling.

Stable CLI/JSON schemas used to inspect and provision the device are specified separately in [`logger_host_interfaces_v1.md`](./logger_host_interfaces_v1.md).
