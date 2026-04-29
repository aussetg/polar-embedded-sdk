# Logger runtime architecture v1

Status: Draft (implementation target)
Last updated: 2026-04-14

Related:
- Firmware behavior spec: [`logger_firmware_v1.md`](./logger_firmware_v1.md)
- Data/storage/upload contract: [`logger_data_contract_v1.md`](./logger_data_contract_v1.md)
- Host/CLI interfaces: [`logger_host_interfaces_v1.md`](./logger_host_interfaces_v1.md)
- Recovery architecture and fault FSMs: [`logger_recovery_architecture_v1.md`](./logger_recovery_architecture_v1.md)
- Long-term update path: [`logger_update_architecture.md`](./logger_update_architecture.md)

---

## 1) Purpose

This document fills in the **implementation-shaping runtime details** that are too operational to live comfortably in the higher-level behavior and data-contract documents.

It is intended to let a coding agent implement the firmware independently from documentation alone.

This document defines:

- the runtime architecture model,
- the high-level finite state machine,
- periodic timers,
- button and boot gestures,
- fault-code taxonomy,
- recommended module decomposition.

The detailed fault-recovery and fault-clearing behavior now lives in
[`logger_recovery_architecture_v1.md`](./logger_recovery_architecture_v1.md).
If this document conflicts with it on recovery behavior, the recovery document wins.

---

## 2) Architectural constraints

### 2.1 Concurrency model

v1 MUST be implemented as a **single-process event-driven firmware** without an RTOS requirement.

The simplest correct mental model is:

- one main event loop,
- interrupt handlers kept tiny,
- BLE and Wi‑Fi managed from the main application context,
- no hidden worker threads.

### 2.2 Core placement

For v1, all timing-critical wireless work SHOULD remain on **core 0**.

The firmware SHOULD avoid clever multicore scheduling until proven necessary.

### 2.3 Radio exclusivity rule

The runtime state machine MUST enforce:

- **BLE logging active** ⇒ Wi‑Fi off
- **Wi‑Fi upload/NTP active** ⇒ BLE logging inactive

This is a hard architectural rule, not a hint.

---

## 3) High-level state machine

### 3.1 Top-level states

The top-level FSM uses the following states:

1. `BOOT`
2. `SERVICE`
3. `RECOVERY_HOLD`
4. `LOG_WAIT_H10`
5. `LOG_CONNECTING`
6. `LOG_SECURING`
7. `LOG_STARTING_STREAM`
8. `LOG_STREAMING`
9. `LOG_STOPPING`
10. `UPLOAD_PREP`
11. `UPLOAD_RUNNING`
12. `IDLE_WAITING_FOR_CHARGER`
13. `IDLE_UPLOAD_COMPLETE`

Faults are an overlay represented by latched fault metadata, not separate top-level states.

### 3.2 Top-level transition summary

#### `BOOT`

Responsibilities:

- initialize clocks, storage, persistent metadata, fault state, button state,
- evaluate provisioning completeness,
- evaluate charger/time boot rule,
- recover any interrupted active session state,
- route into service, logging, or upload/idle mode.

Transitions:

- `BOOT -> SERVICE` if service-mode boot hold requested
- `BOOT -> RECOVERY_HOLD` if provisioning is incomplete and USB/VBUS is absent
- `BOOT -> SERVICE` if provisioning is incomplete and USB/VBUS is present
- `BOOT -> UPLOAD_PREP` if charger present and local time is in `22:00–06:00`
- `BOOT -> LOG_WAIT_H10` otherwise

If the charger/time decision cannot be evaluated because the wall clock is invalid, `BOOT` MUST fall back to `LOG_WAIT_H10` rather than blocking logging.

#### `SERVICE`

Responsibilities:

- expose the USB CLI,
- allow preflight/tests,
- allow dangerous operations only after explicit unlock,
- keep BLE/Wi‑Fi inactive unless an explicit service command requests them,
- represent a human-pinned stop rather than an autonomous recovery state.

Transitions:

- `SERVICE -> RECOVERY_HOLD` when USB/VBUS is removed and unattended operation is not currently legal
- `SERVICE -> LOG_WAIT_H10` when USB/VBUS is removed and unattended logging is legal
- `SERVICE -> UPLOAD_PREP` for explicit network/upload tests or future update actions

`SERVICE` is no longer the default sink for recoverable failures.

#### `RECOVERY_HOLD`

Responsibilities:

- keep logging stopped,
- keep BLE logging inactive,
- keep Wi‑Fi inactive except for bounded recovery probes that explicitly require it,
- periodically re-evaluate blocking conditions,
- auto-clear latched faults after validated recovery,
- resume unattended operation without operator intervention when legal.

Transitions:

- `RECOVERY_HOLD -> SERVICE` when USB/VBUS is present and the active reason is `config_incomplete`
- `RECOVERY_HOLD -> LOG_WAIT_H10` when recovery succeeds and unattended logging is legal
- `RECOVERY_HOLD -> UPLOAD_PREP` when recovery succeeds and unattended upload policy is the correct next state
- `RECOVERY_HOLD -> IDLE_WAITING_FOR_CHARGER` when recovery succeeds but policy still requires waiting for charger
- `RECOVERY_HOLD -> RECOVERY_HOLD` while the blocking condition remains active

#### `LOG_WAIT_H10`

Responsibilities:

- BLE stack up,
- scan only for the bound H10,
- maintain day-level no-session attempt accounting,
- keep Wi‑Fi off.

Transitions:

- `LOG_WAIT_H10 -> LOG_CONNECTING` when the bound H10 is found
- `LOG_WAIT_H10 -> LOG_STOPPING` on study-day rollover if a session for the ending day already exists
- `LOG_WAIT_H10 -> LOG_WAIT_H10` on study-day rollover if no session exists yet and logging should continue for the new day
- `LOG_WAIT_H10 -> LOG_STOPPING` on manual long press, explicit service request, or charger-triggered end-of-day
- `LOG_WAIT_H10 -> RECOVERY_HOLD` on low-start battery block or recoverable storage fault

#### `LOG_CONNECTING`

Responsibilities:

- stop scanning,
- connect to the bound H10,
- track retry/backoff state.

Transitions:

- `LOG_CONNECTING -> LOG_SECURING` on successful BLE link establishment
- `LOG_CONNECTING -> LOG_STOPPING` on study-day rollover if a session for the ending day already exists
- `LOG_CONNECTING -> LOG_WAIT_H10` on study-day rollover if no session exists yet and logging should continue for the new day
- `LOG_CONNECTING -> LOG_STOPPING` on explicit service request
- `LOG_CONNECTING -> LOG_WAIT_H10` on failure or disconnect

Repeated security-oriented failures remain in the logging acquisition loop and are
handled by stale-bond recovery, not by a forced transition to `SERVICE`.

#### `LOG_SECURING`

Responsibilities:

- establish encrypted link,
- apply stale-bond retry logic,
- read required connection metadata.

Transitions:

- `LOG_SECURING -> LOG_STARTING_STREAM` on encrypted ready state
- `LOG_SECURING -> LOG_STOPPING` on study-day rollover if a session for the ending day already exists
- `LOG_SECURING -> LOG_WAIT_H10` on study-day rollover if no session exists yet and logging should continue for the new day
- `LOG_SECURING -> LOG_STOPPING` on explicit service request
- `LOG_SECURING -> LOG_WAIT_H10` on failure/disconnect

Repeated secure-link failures MUST trigger the stale-bond recovery policy defined
in `logger_recovery_architecture_v1.md` before any human-facing fallback is considered.

#### `LOG_STARTING_STREAM`

Responsibilities:

- discover services/chars as needed,
- enable PMD notifications,
- start ECG streaming,
- create session artifacts lazily only when real ECG streaming begins.

Transitions:

- `LOG_STARTING_STREAM -> LOG_STREAMING` when ECG packets begin arriving
- `LOG_STARTING_STREAM -> LOG_STOPPING` on study-day rollover if a session for the ending day already exists
- `LOG_STARTING_STREAM -> LOG_WAIT_H10` on study-day rollover if no session exists yet and logging should continue for the new day
- `LOG_STARTING_STREAM -> LOG_STOPPING` on explicit service request
- `LOG_STARTING_STREAM -> LOG_WAIT_H10` on start failure/disconnect

First-packet timeout and PMD-start retry behavior are part of the autonomous
logging acquisition loop, not a reason to enter `SERVICE`.

#### `LOG_STREAMING`

Responsibilities:

- append ECG PMD data into active chunks,
- seal and flush chunks on size/time boundaries,
- emit status snapshots, battery reads, markers, and gap metadata,
- keep Wi‑Fi off.

Transitions:

- `LOG_STREAMING -> LOG_WAIT_H10` on disconnect after explicit span close/gap record
- `LOG_STREAMING -> LOG_STOPPING` on manual stop, explicit service request, rollover, charger-triggered end-of-day, critical low battery, storage fault, or reboot recovery boundary

#### `LOG_STOPPING`

Responsibilities:

- close current span if any,
- close session if one exists,
- write immutable manifest if a session exists,
- update the upload queue store if a closed session was produced,
- decide next mode.

Transitions:

- `LOG_STOPPING -> UPLOAD_PREP` if upload work should start now
- `LOG_STOPPING -> LOG_WAIT_H10` if logging should resume immediately in a new day/mode
- `LOG_STOPPING -> SERVICE` if an explicit service request is pending
- `LOG_STOPPING -> RECOVERY_HOLD` if the stop reason is a recoverable blocking fault
- `LOG_STOPPING -> IDLE_WAITING_FOR_CHARGER` if uploads are queued but USB power is required before they may proceed
- `LOG_STOPPING -> IDLE_UPLOAD_COMPLETE` if logging is stopped and no upload work remains

#### `UPLOAD_PREP`

Responsibilities:

- ensure logging is fully stopped,
- bring up Wi‑Fi,
- perform one NTP sync attempt if network configuration permits it,
- allow upload work either on USB power or, for a battery-triggered manual upload, on battery when voltage policy permits it,
- decide whether there is pending queue work.

Transitions:

- `UPLOAD_PREP -> UPLOAD_RUNNING` if pending uploads exist
- `UPLOAD_PREP -> IDLE_UPLOAD_COMPLETE` if no pending uploads remain
- `UPLOAD_PREP -> LOG_WAIT_H10` if USB is removed before upload starts

#### `UPLOAD_RUNNING`

Responsibilities:

- upload all pending sessions oldest first,
- update queue state after every attempt,
- retry with backoff while charger remains present,
- on battery-triggered manual upload, perform only one best-effort queue pass.

Transitions:

- `UPLOAD_RUNNING -> IDLE_UPLOAD_COMPLETE` when queue is empty
- `UPLOAD_RUNNING -> IDLE_WAITING_FOR_CHARGER` when a battery-triggered manual upload pass ends with work still queued
- `UPLOAD_RUNNING -> LOG_WAIT_H10` immediately if USB is removed

#### `IDLE_WAITING_FOR_CHARGER`

Responsibilities:

- keep logging stopped,
- keep queued uploads intact,
- keep CLI available,
- wait for USB power before entering `UPLOAD_PREP`.

Transitions:

- `IDLE_WAITING_FOR_CHARGER -> UPLOAD_PREP` when USB power is attached
- `IDLE_WAITING_FOR_CHARGER -> SERVICE` on explicit service request

#### `IDLE_UPLOAD_COMPLETE`

Responsibilities:

- keep CLI available,
- keep fault state visible,
- keep logging stopped until a later policy event explicitly resumes it,
- do not start BLE logging while charger remains present in overnight upload/idle scenarios.

Transitions:

- `IDLE_UPLOAD_COMPLETE -> LOG_WAIT_H10` when USB is removed
- `IDLE_UPLOAD_COMPLETE -> SERVICE` on explicit service request

### 3.3 Host-visible mode mapping

The stable host-facing `status --json` response reports both a coarse `mode` and an exact `runtime_state`.

The mapping is:

| Internal state | `mode` | `runtime_state` |
|---|---|---|
| `SERVICE` | `service` | `service` |
| `RECOVERY_HOLD` | `recovery_hold` | `recovery_hold` |
| `LOG_WAIT_H10` | `logging` | `log_wait_h10` |
| `LOG_CONNECTING` | `logging` | `log_connecting` |
| `LOG_SECURING` | `logging` | `log_securing` |
| `LOG_STARTING_STREAM` | `logging` | `log_starting_stream` |
| `LOG_STREAMING` | `logging` | `log_streaming` |
| `LOG_STOPPING` | `logging` or `upload` | `log_stopping` |
| `UPLOAD_PREP` | `upload` | `upload_prep` |
| `UPLOAD_RUNNING` | `upload` | `upload_running` |
| `IDLE_WAITING_FOR_CHARGER` | `idle_waiting_for_charger` | `idle_waiting_for_charger` |
| `IDLE_UPLOAD_COMPLETE` | `idle_upload_complete` | `idle_upload_complete` |

During `LOG_STOPPING`, `mode` MUST be reported as:

- `logging` if the planned next state is `LOG_WAIT_H10`,
- `upload` if the planned next state is upload-related,
- `recovery_hold` if the planned next state is `RECOVERY_HOLD`,
- `service` if the planned next state is `SERVICE`.

---

## 4) Session, span, and no-session day handling

### 4.1 Lazy session creation rule

The session directory and `journal.bin` are created only when the first real ECG span begins.

This means:

- failed scans/connects before the first span live only in system/day summary state,
- no upload bundle exists unless at least one real ECG span exists.

### 4.2 No-session day summary

The runtime MUST maintain a day-level outcome accumulator even before a session exists.

For days with no successful ECG stream, the system log MUST emit a `no_session_day_summary` event containing at least:

- `study_day_local`
- `reason`
- whether the bound H10 was ever seen,
- whether BLE ever connected,
- whether ECG start was ever attempted.

Canonical v1 `reason` values are:

- `no_h10_seen`
- `no_h10_connect`
- `no_ecg_stream`
- `stopped_before_first_span`

### 4.3 Day-summary finalization time

No-session day summaries are finalized at whichever occurs first:

- the `04:00` study-day rollover,
- explicit end-of-day stop.

### 4.4 Rollover while no span is open

If `04:00` rollover occurs while no span is currently open:

- if no session exists yet for the ending study day, the runtime MUST finalize the no-session day summary, reset the day accumulator, and remain in logging acquisition states for the new study day,
- if a session already exists for the ending study day, the runtime MUST abandon any in-flight reconnect/start attempt, transition through `LOG_STOPPING`, close that session, and then continue in logging acquisition states for the new study day.

---

## 5) Recommended timer and backoff policy

### 5.1 Logging-mode periodic timers

The runtime SHOULD implement these periodic jobs during active logging:

| Job | Period |
|---|---:|
| watchdog kick / health poll | `1 s` |
| live-session flush | `5 s` |
| chunk seal timeout | `60 s` max open time |
| status snapshot | `5 min` |
| H10 battery read | `60 min` |

Recovery-mode probe cadence and stability windows are defined in
`logger_recovery_architecture_v1.md`.

### 5.2 H10 reconnect backoff

Recommended reconnect delay sequence for v1:

```text
1 s, 2 s, 5 s, 10 s, 30 s, 60 s, then cap at 60 s
```

The counter resets after a successful real ECG streaming start.

### 5.3 Upload retry backoff

Recommended upload retry delay sequence while USB remains present:

```text
30 s, 60 s, 5 min, 15 min, then cap at 15 min
```

The counter resets after any successful upload.

---

## 6) Button and boot gestures

### 6.1 Runtime button events

For v1 the button timing thresholds are:

- ignore press shorter than `50 ms`
- **short press:** `50 ms` to `< 700 ms`
- **long press:** `>= 2 s`

Only one runtime short-press action exists in v1:

- while logging: record a `MARKER`

Only one runtime long-press action exists in v1:

- stop logging and enter upload/queued-work logic

Outside logging-related states, runtime short and long presses are ignored in v1.

### 6.2 Boot-time service-mode gesture

If the user button is held continuously from boot for at least `2 s` but less than `10 s`, the device enters **service mode**.

### 6.3 Boot-time factory-reset gesture

If the user button is held continuously from boot for at least `10 s`, the device performs a **factory reset** and then reboots into service mode.

Factory reset clears:

- internal config,
- Wi‑Fi credentials,
- upload auth token,
- bound H10 setting,
- BLE bonds.

Factory reset does **not** delete:

- session directories on SD,
- exported files on SD,
- internal system event log,
- current/last-cleared fault history.

### 6.4 Gesture precedence

At boot, the factory-reset gesture takes precedence over service-mode entry.

### 6.5 Boot-time gesture detection algorithm

The boot path MUST implement gesture recognition as an intentional blocking decision window:

1. sample the button state as early as practical after reset,
2. if the button is not held, continue normal boot immediately,
3. if the button is held, defer mode selection while the hold duration is measured,
4. releasing before `2 s` means normal boot,
5. releasing at or after `2 s` but before `10 s` means service mode,
6. reaching `10 s` continuously held means factory reset.

This boot-hold delay is normative for v1.

---

## 7) Fault taxonomy and indicator mapping

### 7.1 Canonical fault codes

The canonical latched fault codes for v1 are:

- `config_incomplete`
- `clock_invalid`
- `low_battery_blocked_start`
- `critical_low_battery_stopped`
- `sd_missing_or_unwritable`
- `sd_write_failed`
- `sd_low_space_reserve_unmet`
- `upload_blocked_min_firmware`

Other non-latched warnings MAY exist in diagnostics, but these are the primary user-visible latched fault classes.

### 7.2 Visible blink-code mapping

The minimal visible indicator uses this mapping:

| Blinks | Fault code |
|---:|---|
| `1` | `config_incomplete` |
| `2` | `clock_invalid` |
| `3` | `low_battery_blocked_start` or `critical_low_battery_stopped` |
| `4` | any SD/storage fault |
| `5` | `upload_blocked_min_firmware` |

One blink cycle SHOULD repeat every 2 seconds while the fault remains latched.

### 7.3 Fault latching and clearing

Fault latching, precedence, validated auto-clear, and recovery FSM behavior are
defined by `logger_recovery_architecture_v1.md`.

The host CLI `fault clear` command remains a stable operator-facing
acknowledge/clear path, but it is no longer the only expected path out of a
recoverable latched fault.

Clearing or acknowledging a fault, whether automatic or host-initiated, MUST emit
a system-log event.

---

## 8) Recommended source tree/module decomposition

The exact file names are not normative, but a coding agent SHOULD decompose the implementation into modules roughly like this:

- `app_main.[ch]` — main loop and top-level state machine
- `app_state.[ch]` — top-level persistent runtime state struct
- `button.[ch]` — debouncing and gesture recognition
- `faults.[ch]` — fault codes, latching, blink policy
- `recovery.[ch]` — recovery reasons, timers, validation probes, and auto-clear policy
- `clock.[ch]` — RTC + NTP + clock-validity handling
- `battery.[ch]` — voltage thresholds and policy decisions
- `storage.[ch]` — FAT mount, free-space reserve, pruning
- `journal.[ch]` — `journal.bin` framing and chunk writing
- `session.[ch]` — session/span lifecycle and manifest generation
- `system_log.[ch]` — append-only internal-flash event log
- `queue.[ch]` — upload queue store
- `h10_link.[ch]` — scan/connect/security/disconnect policy
- `h10_ecg.[ch]` — PMD ECG stream control and packet ingest
- `upload.[ch]` — canonical tar streaming + HTTP(S) POST + ack handling, including built-in public-root validation and hostname verification for HTTPS uploads
- `service_cli.[ch]` — USB CLI surface, including staged chunked config-import buffering and commit

This layout is not required, but it is the intended shape.

---

## 9) Recovery order on unexpected reboot

On boot after watchdog reset or any unclean restart, the runtime SHOULD recover in this order:

1. read boot/reset cause and persistent fault state,
2. mount SD,
3. inspect `live.json` if present,
4. scan the active `journal.bin` to the last valid record,
5. append recovery metadata later rather than rewriting durable bytes,
6. decide whether to resume logging immediately or route into upload/service/recovery-hold mode based on charger/time/provisioning rules.

If a partially active session is recovered successfully and current policy still permits logging for that study day, subsequent data MUST continue in a new span with explicit recovery metadata.

If a partially active session is recovered successfully but current policy does **not** permit immediate logging, the firmware MUST finalize that session as interrupted and queue it for later upload rather than resuming it silently.

---

## 10) Coding-agent implementation checklist

A coding agent implementing this firmware from documentation alone should be able to proceed in this order:

1. implement persistent config + provisioning gate,
2. implement top-level FSM and boot rules,
3. implement SD mount/space/fault handling,
4. implement journal framing and append-only recovery,
5. implement H10 connect/security/start/reconnect policy,
6. implement session/span lifecycle,
7. implement upload queue + canonical tar + HTTP POST protocol,
8. implement service CLI and stable JSON commands,
9. implement watchdog/fault indication/system log,
10. add update architecture support later without changing the v1 data contract.

This document is intentionally operational. The normative storage and host-facing schemas remain in the paired spec documents.
