# Logger host interfaces v1

Status: Draft (implementation target)
Last updated: 2026-04-14

Related:
- Firmware behavior: [`logger_firmware_v1.md`](./logger_firmware_v1.md)
- Data/storage/upload contract: [`logger_data_contract_v1.md`](./logger_data_contract_v1.md)
- Runtime architecture: [`logger_runtime_architecture_v1.md`](./logger_runtime_architecture_v1.md)
- Recovery architecture: [`logger_recovery_architecture_v1.md`](./logger_recovery_architecture_v1.md)

---

## 1) Purpose

This document defines the **stable host-facing interfaces** for provisioning and inspecting the logger.

The canonical host tool name in these docs is:

```text
loggerctl
```

### What is stable in v1

- logical command set,
- JSON schema envelopes,
- JSON payload schemas,
- dangerous-operation guard rules.

### What is intentionally not frozen here

- the private USB serial framing between `loggerctl` and the device,
- human-oriented pretty text output,
- transport implementation details inside the host tool.

---

## 2) General rules

### 2.1 JSON-first stability

Whenever this document says a command is stable, it means the **JSON mode** of that command is stable.

For mutating commands whose synopsis in section 3 omits `--json`, the stable machine-readable response is still the JSON response envelope from section 2.2.

Human-friendly text output MAY evolve.

### 2.2 Common JSON envelope

All JSON command responses MUST use a common envelope:

```json
{
  "schema_version": 1,
  "command": "status",
  "ok": true,
  "generated_at_utc": "2026-03-29T22:41:18Z",
  "payload": { }
}
```

Unless a command section explicitly says otherwise:

- JSON responses are UTF-8 with no BOM,
- fields documented as part of a stable schema MUST be present,
- unavailable documented values MUST be `null` rather than omitted,
- SHA-256 values are lowercase hexadecimal strings of exactly 64 characters,
- fields ending in `_utc` use RFC 3339 UTC strings with trailing `Z`,
- BLE addresses use uppercase hex with `:` separators.

Within a declared `schema_version`, devices MAY add additional fields. Host tooling MUST ignore unknown fields.

On failure:

```json
{
  "schema_version": 1,
  "command": "sd format",
  "ok": false,
  "generated_at_utc": "2026-03-29T22:41:18Z",
  "error": {
    "code": "service_locked",
    "message": "service unlock is required before formatting SD"
  }
}
```

### 2.3 Dangerous-operation guard

Dangerous operations MUST require explicit unlock, even in service mode.

Examples:

- SD formatting,
- factory reset,
- config import,
- any future firmware update command.

### 2.4 Mutability while logging

During active logging:

- read-only diagnostics are allowed,
- mutating config/provisioning actions are forbidden.

### 2.5 Exports written to SD card

When the CLI asks the device to export JSON content onto the SD card:

- config export files use the schema in section 9 directly,
- system-log export files use the schema in section 10 directly,
- the command envelope from section 2.2 is not wrapped around those on-SD export files.

### 2.6 Common command error codes

Stable v1 error codes include at least:

- `service_locked`
- `busy_logging`
- `invalid_schema`
- `invalid_config`
- `hardware_id_mismatch`
- `condition_still_present`
- `storage_unavailable`
- `network_unavailable`
- `not_permitted_in_mode`
- `internal_error`

---

## 3) Stable command set

The following logical commands are stable in v1.

| Command | Purpose |
|---|---|
| `loggerctl status --json` | overall device state summary |
| `loggerctl provisioning-status --json` | required/optional config completeness |
| `loggerctl queue --json` | closed-session upload queue state |
| `loggerctl preflight --json` | non-mutating offline health check |
| `loggerctl net-test --json` | explicit Wi‑Fi + upload endpoint reachability test |
| `loggerctl config export --json` | export config schema |
| `loggerctl config import <file>` | import config from JSON |
| `loggerctl system-log export --json` | export system event log |
| `loggerctl service enter` | request transition into service mode |
| `loggerctl service unlock` | arm dangerous operations |
| `loggerctl fault clear` | clear/acknowledge the current latched fault |
| `loggerctl sd format` | explicit SD formatting action |
| `loggerctl factory-reset` | hardware/provisioning reset action |

The device MAY expose additional debug commands, but those are not part of the stable v1 contract unless documented here.

Mode-availability rules for stable commands:

- `status`, `provisioning-status`, `queue`, `config export`, and `system-log export` are read-only and MUST be allowed in any mode.
- `preflight` is allowed in `service`, `recovery_hold`, `idle_waiting_for_charger`, and `idle_upload_complete` modes. It MUST fail with `busy_logging` in `logging` mode and with `not_permitted_in_mode` in `upload` mode.
- `net-test` is allowed only in `service` mode. It MUST fail with `busy_logging` in `logging` mode and with `not_permitted_in_mode` in any other non-service mode.
- `service enter` is allowed in `logging`, `service`, `recovery_hold`, `idle_waiting_for_charger`, and `idle_upload_complete` modes. In `logging` mode it MUST cleanly stop logging, close any active session with end reason `service_entry`, and then enter `service`. It MUST fail with `not_permitted_in_mode` in `upload` mode.
- `service unlock` is allowed only in `service` mode. It MUST fail with `busy_logging` in `logging` mode and with `not_permitted_in_mode` in any other non-service mode.
- `fault clear` is allowed in `service`, `recovery_hold`, `idle_waiting_for_charger`, and `idle_upload_complete` modes. It MUST fail with `busy_logging` in `logging` mode and with `not_permitted_in_mode` in `upload` mode.
- `config import`, `sd format`, and `factory-reset` are allowed only in `service` mode after unlock. They MUST fail with `busy_logging` in `logging` mode and with `not_permitted_in_mode` in any other non-service mode.

---

## 4) `status --json`

### 4.1 Purpose

`status --json` is the single machine-readable summary for host automation.

### 4.2 Required payload shape

```json
{
  "mode": "logging",
  "runtime_state": "log_streaming",
  "identity": { },
  "provisioning": { },
  "fault": { },
  "recovery": { },
  "battery": { },
  "storage": { },
  "h10": { },
  "session": { },
  "upload_queue": { },
  "last_day_outcome": { },
  "firmware": { }
}
```

### 4.3 Required fields

#### `mode`

One of:

- `service`
- `recovery_hold`
- `logging`
- `upload`
- `idle_waiting_for_charger`
- `idle_upload_complete`

#### `runtime_state`

One of:

- `service`
- `recovery_hold`
- `log_wait_h10`
- `log_connecting`
- `log_securing`
- `log_starting_stream`
- `log_streaming`
- `log_stopping`
- `upload_prep`
- `upload_running`
- `idle_waiting_for_charger`
- `idle_upload_complete`

#### `identity`

Must include:

- `hardware_id`
- `logger_id`
- `subject_id`

`hardware_id` is always present and uses the stable 32-hex-character representation defined in the data contract.

`logger_id` and `subject_id` are `null` until provisioned.

#### `provisioning`

Must include:

- `normal_logging_ready` (boolean)
- `required_missing` (array)
- `warnings` (array)

The token sets match `provisioning-status --json`.

These arrays MUST be returned in lexicographic order.

#### `fault`

Must include:

- `latched` (boolean)
- `current_code`
- `last_cleared_code`

`current_code` and `last_cleared_code` are fault-code strings or `null`.

The canonical v1 fault-code set is defined in the runtime architecture document.

#### `recovery`

Must include:

- `active` (boolean)
- `reason`
- `attempt_count`
- `next_attempt_ms`
- `resume_mode`
- `service_pinned_by_user` (boolean)
- `last_action`
- `last_result`

`reason`, `next_attempt_ms`, `resume_mode`, `last_action`, and `last_result` are
either strings/integers as documented by the recovery architecture or `null` when
not applicable.

If `mode` is `recovery_hold`, `recovery.active` MUST be `true`.

The canonical meanings of these fields are defined in
`logger_recovery_architecture_v1.md`.

#### `battery`

Must include:

- `voltage_mv`
- `estimate_pct`
- `vbus_present`
- `critical_stop_mv`
- `low_start_mv`
- `off_charger_upload_mv`

#### `storage`

Must include:

- `sd_present`
- `filesystem`
- `free_bytes`
- `reserve_bytes`
- `card_identity`

`filesystem` is `fat32` when mounted successfully, otherwise `null`.

`card_identity` is either `null` or an object containing at least:

- `manufacturer_id`
- `oem_id`
- `product_name`
- `revision`
- `serial_number`

All `card_identity` subfields are strings, using uppercase hexadecimal where that is the natural SD-register representation.

#### `h10`

Must include:

- `bound_address`
- `connected`
- `encrypted`
- `bonded`
- `last_seen_address`
- `battery_percent`

`bound_address` is `null` until provisioned.

`last_seen_address` and `battery_percent` are `null` until observed.

#### `session`

Must include:

- `active` (boolean)
- `session_id`
- `study_day_local`
- `span_id`
- `quarantined`
- `clock_state`
- `journal_size_bytes`

Allowed `clock_state` values are:

- `valid`
- `invalid`
- `jumped`

For status reporting:

- `valid` means current session timing is on an unbroken valid-clock path,
- `invalid` means the current session is presently running with an invalid wall clock,
- `jumped` means the current session has crossed a large accepted clock correction boundary.

If `session.active` is `false`:

- `session_id` is `null`,
- `span_id` is `null`,
- `journal_size_bytes` is `null`,
- `quarantined` is `false`.

When `session.active` is `false`, `clock_state` reports the device's current wall-clock state rather than session provenance.

If `session.active` is `true` but the logger is currently between spans, `session_id` remains non-null and `span_id` is `null`.

When `mode` is `logging` but no session has started yet for the current study day, `study_day_local` is the current intended logging-day label derived from the current wall clock.

When `mode` is not `logging` and `session.active` is `false`, `study_day_local` is `null`.

#### `upload_queue`

Must include:

- `pending_count`
- `blocked_count`
- `oldest_pending_study_day`
- `last_failure_class`

`oldest_pending_study_day` is `null` when no pending uploads exist.

`last_failure_class` is the most recent queue failure class observed across all entries, or `null` if none exists.

`pending_count` counts queue entries whose upload work is not yet complete, i.e. entries whose status is `pending`, `failed`, or `uploading`.

`blocked_count` counts entries whose status is `blocked_min_firmware`.

#### `last_day_outcome`

Must include:

- `study_day_local`
- `kind` (`session` or `no_session`)
- `reason`

All three fields are `null` until the device has finalized at least one completed study-day outcome.

Recommended `reason` values include:

- `session_closed`
- `no_h10_seen`
- `no_h10_connect`
- `no_ecg_stream`
- `stopped_before_first_span`

#### `firmware`

Must include:

- `version`
- `build_id`

`version` uses the same SemVer string format as `firmware_version` in the data contract.

---

## 5) `provisioning-status --json`

This command is a more explicit provisioning gate report.

Required payload fields:

- `normal_logging_ready`
- `required_present`
- `required_missing`
- `optional_present`
- `warnings`

`required_present`, `required_missing`, `optional_present`, and `warnings` are arrays of canonical string tokens.

These token arrays MUST be returned in lexicographic order for stable v1 output.

Required fields under `required_present` / `required_missing` include:

- `logger_id`
- `subject_id`
- `bound_h10_address`
- `timezone`

Optional/warning-oriented fields include at least:

- upload endpoint presence,
- upload auth presence,
- Wi‑Fi network presence.

Canonical `optional_present` tokens for v1 are:

- `upload_url`
- `upload_auth`
- `wifi_networks`

Canonical warning tokens for v1 are:

- `upload_not_configured`
- `wifi_not_configured`
- `clock_invalid`

---

## 6) `queue --json`

This command exposes closed-session queue state.

Required payload fields:

- `schema_source` (`upload_queue.json`)
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
- `status`
- `quarantined`
- `attempt_count`
- `last_failure_class`
- `verified_upload_utc`
- `receipt_id`

`verified_upload_utc`, `receipt_id`, and `last_failure_class` are `null` when not yet applicable.

Allowed `status` values are:

- `pending`
- `uploading`
- `verified`
- `blocked_min_firmware`
- `failed`

The `sessions` array MUST be returned in the same oldest-first order used for upload processing.

The CLI MAY also render this as a compact table in text mode, but JSON is the stable interface.

---

## 7) `preflight --json`

### 7.1 Default behavior

Default preflight is:

- non-mutating,
- offline,
- safe to run without affecting study data,
- allowed to inspect configuration and hardware state,
- allowed to scan for the bound H10,
- not allowed to pair, format, upload, or start real logging.

If the current high-level mode is `logging`, `preflight` MUST fail with `busy_logging` rather than disrupting capture.

If the current high-level mode is `upload`, `preflight` MUST fail with `not_permitted_in_mode`.

### 7.2 Required payload fields

```json
{
  "overall_result": "pass",
  "checks": [
    {
      "name": "rtc",
      "result": "pass",
      "details": { }
    }
  ]
}
```

Required `checks[].name` values:

- `rtc`
- `storage`
- `battery_sense`
- `provisioning`
- `bound_h10_scan`

Allowed result values:

- `pass`
- `warn`
- `fail`

`overall_result` uses the same value set.

`checks[].details` is a diagnostic JSON object whose exact subfields are not frozen in v1.

---

## 8) `net-test --json`

This command explicitly tests networking.

`net-test` MUST fail with `busy_logging` in `logging` mode.

`net-test` MUST also fail with `not_permitted_in_mode` unless the device is currently in `service` mode.

Required payload fields:

- `wifi_join`
- `dns`
- `tls`
- `upload_endpoint_reachable`

Each section MUST be an object containing at least:

- `result`
- `details`

Allowed `result` values are:

- `pass`
- `fail`
- `not_applicable`

If the configured upload URL uses HTTP rather than HTTPS, `tls.result` MUST be `not_applicable`.

The per-section `details` objects are diagnostic and are not frozen beyond being JSON objects.

`net-test` is intentionally separate from default preflight.

---

## 9) Config import/export schema

`loggerctl config export --json` returns the config object from this section as the command `payload` inside the common response envelope.

### 9.1 General rules

The exported/imported config file is JSON with `schema_version: 1`.

Exports omit secrets by default.

`hardware_id` is a safety field. On import, if `hardware_id` is present and does not match the current device, the import MUST fail in stable v1.

Config import is a full replacement of the persisted device configuration, not a partial merge.

### 9.2 Required top-level sections

- `schema_version`
- `exported_at_utc`
- `hardware_id`
- `secrets_included`
- `identity`
- `recording`
- `time`
- `battery_policy`
- `wifi`
- `upload`

### 9.3 Required fields

#### `identity`

- `logger_id`
- `subject_id`

`subject_id` remains required in device config as local study metadata and for immutable session artifacts. Upload authentication identity is derived from the configured bearer token, not from `identity.subject_id`.

#### `recording`

- `bound_h10_address`
- `study_day_rollover_local` (`04:00:00` in v1 default)
- `overnight_upload_window_start_local` (`22:00:00` default)
- `overnight_upload_window_end_local` (`06:00:00` default)

#### `time`

- `timezone`

For v1 `timezone` is an IANA timezone identifier string such as `Europe/Paris`.

#### `battery_policy`

- `critical_stop_voltage_v`
- `low_start_voltage_v`
- `off_charger_upload_voltage_v`

#### `wifi`

- `allowed_ssids`
- `networks`

`allowed_ssids` is the authoritative allow-list for upload-mode Wi‑Fi joins. Even if credentials for additional SSIDs exist, the logger MUST attempt upload-mode network joins only against SSIDs in `allowed_ssids`.

Each `networks[]` entry includes at least `ssid` and either:

- `psk` when secrets are included, or
- `psk_present: true` when secrets are omitted.

Network entries are keyed by exact `ssid` string.

#### `upload`

- `enabled`
- `url`
- `auth`

`tls` is optional in the stable v1 schema.

If `enabled` is `true`, `url` MUST be an absolute `http://` or `https://` URL string.
If `enabled` is `true`, `auth.type` MUST be `api_key_and_bearer`.

If `enabled` is `false`, `url` MAY be `null`.

`auth` includes at least:

- `type` (`none` or `api_key_and_bearer`)
- `api_key` and `token` when secrets are included, or
- `api_key_present: true` and `token_present: true` when secrets are omitted.

If `tls` is present, it is an object containing at least:

- `mode`
- `root_profile`
- `anchor`

For v1, the currently supported `https://` trust mode is:

- `mode: "public_roots"`
- `root_profile: "logger-public-roots-v1"`
- `anchor: null`

For disabled upload or `http://` upload, `mode`, `root_profile`, and `anchor` are all `null`.

### 9.4 Import semantics

For v1:

- a successful import atomically replaces the prior persisted config,
- if `secrets_included` is `false`, omitted secrets are cleared rather than preserved implicitly,
- `psk_present`, `api_key_present`, and `token_present` are informational export markers and do not install secrets by themselves,
- an imported `https://` upload URL MAY omit `upload.tls`; omission is interpreted as the built-in `public_roots` mode,
- if the imported bound H10 address differs from the previous bound address, the old stored bond MUST be cleared.

### 9.5 Chunked config-import transport

For larger configs that do not fit comfortably in a single CLI line, the firmware MAY also expose a staged import transport:

- `config import begin <total_bytes>`
- `config import chunk <raw_json_fragment>`
- `config import status`
- `config import commit`
- `config import cancel`

For the staged transport:

- the transferred document is still the same compact single-line JSON config object from this section,
- `begin`, `chunk`, and `commit` are service-mode and unlock gated,
- `status` and `cancel` are service-mode only,
- `commit` performs the same validation and atomic full-replacement semantics as one-line `config import <json>`.

### 9.6 `loggerctl upload configure` helper (non-normative)

`loggerctl upload configure` is a host-side convenience command layered on top of `config export`, TLS planning, and `config import`.

For enabled upload in v1, the generated config must use:

- `upload.auth.type = "api_key_and_bearer"`
- `upload.auth.api_key` together with `upload.auth.token` when secrets are included, or
- `upload.auth.api_key_present: true` together with `upload.auth.token_present: true` when secrets are omitted.

Typical usage:

- generate a config file from a live export while supplying missing secrets,
- optionally apply that generated config back onto the device,
- optionally run `net-test` after a successful apply.

Example:

```sh
loggerctl upload configure https://example.invalid/upload \
  --api-key-env LOGGER_UPLOAD_API_KEY \
  --bearer-token-env LOGGER_UPLOAD_BEARER_TOKEN \
  --wifi-ssid my-wifi \
  --wifi-psk-env LOGGER_WIFI_PSK \
  --apply --verify-net-test
```

---

## 10) `system-log export --json`

### 10.1 Purpose

Exports the internal-flash append-only system log as structured JSON.

### 10.2 Required payload shape

```json
{
  "schema_version": 1,
  "exported_at_utc": "2026-03-29T22:41:18Z",
  "events": [
    {
      "event_seq": 1,
      "utc": "2026-03-29T22:41:18Z",
      "kind": "factory_reset",
      "severity": "info",
      "details": { }
    }
  ]
}
```

Required fields per event:

- `event_seq`
- `utc`
- `boot_counter`
- `kind`
- `severity`
- `details`

`utc` MUST be present and is `null` when unknown.

For v1, `event_seq` is one-based and the `events` array is returned in ascending `event_seq` order.

`details` is a JSON object whose exact subfields are not frozen in v1.

Allowed `severity` values for v1 are:

- `info`
- `warn`
- `error`

Example event kinds include:

- `factory_reset`
- `config_changed`
- `fault_latched`
- `fault_cleared`
- `watchdog_reset`
- `no_session_day_summary`
- `rtc_lost_power`
- `service_unlock`
- `service_auto_exit_usb_removed`
- `bond_auto_cleared`
- `bond_auto_repaired`

---

## 11) Unlock and dangerous operations

### 11.1 `service unlock`

This command arms a short-lived window in which dangerous operations are accepted.

It is valid only while the device is in `service` mode.

If USB/VBUS is removed while the device is in `service` mode, the device MUST
automatically leave `service` mode, invalidate any outstanding service unlock,
discard any uncommitted staged config-import buffer, and reevaluate unattended
policy. The exact next mode depends on the current live blocking conditions and
is not encoded in the `service unlock` response itself.

For v1:

- the unlock window lasts **60 seconds**,
- the response payload MUST include `expires_at_utc`,
- the device MAY invalidate the window earlier after a dangerous command is executed.

The success payload MUST include at least:

- `unlocked: true`
- `expires_at_utc`
- `ttl_seconds`

### 11.2 `config import`

On success the payload MUST include at least:

- `applied: true`
- `normal_logging_ready`
- `bond_cleared`

If the imported config is syntactically valid but leaves required logging prerequisites absent, `applied` is still `true` and `normal_logging_ready` is `false`.

### 11.3 `sd format`

`sd format` reformats the logger SD card as a fresh FAT32 volume for logger use.

It is destructive to all session directories, exports, and queue state stored on the card.

It does **not** erase internal-flash config, internal system logs, or boot/fault history.

On success the payload MUST include at least:

- `formatted: true`
- `filesystem: "fat32"`
- `logger_root_created: true`

### 11.4 `factory-reset`

`factory-reset` clears internal provisioning state and then reboots the device.

Its destructive scope matches the runtime architecture document.

On success the payload MUST include at least:

- `factory_reset: true`
- `will_reboot: true`

### 11.5 `fault clear`

`fault clear` acknowledges and clears the currently latched fault if policy permits it.

If the underlying fault condition is still present, the command MUST fail with `condition_still_present`.

The device MAY also clear latched faults automatically after validated recovery,
as defined in `logger_recovery_architecture_v1.md`. Therefore host tooling MUST
not assume that every clear event was user-initiated.

On success the payload MUST include at least:

- `cleared: true`
- `previous_code`

If no fault is currently latched, the command SHOULD still succeed with `cleared: false` and `previous_code: null`.

### 11.6 Dangerous commands

At minimum these commands require unlock:

- `config import`
- `sd format`
- `factory-reset`

If the device is actively logging, dangerous commands MUST fail even if unlock was granted.

---

## 12) Stability statement

The following JSON surfaces are versioned/stable from v1:

- `status --json`
- `provisioning-status --json`
- `queue --json`
- `preflight --json`
- `net-test --json`
- config import/export schema
- system-log export schema

Future firmware MAY add fields, but MUST NOT silently break required v1 fields without a schema version bump.
