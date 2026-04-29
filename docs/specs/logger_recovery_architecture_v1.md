# Logger recovery architecture v1

Status: Draft (implementation target)
Last updated: 2026-04-14

Related:
- Firmware behavior/product spec: [`logger_firmware_v1.md`](./logger_firmware_v1.md)
- Runtime architecture and top-level FSM: [`logger_runtime_architecture_v1.md`](./logger_runtime_architecture_v1.md)
- Stable host/CLI JSON interfaces: [`logger_host_interfaces_v1.md`](./logger_host_interfaces_v1.md)
- Data/storage/upload contract: [`logger_data_contract_v1.md`](./logger_data_contract_v1.md)
- Long-term update path: [`logger_update_architecture.md`](./logger_update_architecture.md)

Override rule:

- This document is normative for **fault handling, automatic recovery, fault clearing,
  service-mode lifetime, USB-unplug behavior, and host-visible recovery state**.
- If it conflicts with older wording in `logger_runtime_architecture_v1.md` or
  `logger_host_interfaces_v1.md`, this document wins.

---

## 1) Purpose

This document defines the recovery behavior required for the custom study logger
to operate unattended.

The central design change is:

- **`SERVICE` is for humans**.
- **`RECOVERY_HOLD` is for the machine**.

The logger MUST no longer fall into a sticky service-mode sink for failures that
are recoverable without operator help.

This document defines:

1. the distinction between human service mode and autonomous recovery hold,
2. the recovery FSM for each v1 fault class,
3. how and when latched faults clear automatically,
4. how USB/VBUS removal exits service mode,
5. the required recovery behavior for H10 security/bond failures that are not
   exposed as primary user-visible fault codes.

The key words **MUST**, **MUST NOT**, **SHOULD**, and **MAY** are normative.

---

## 2) Scope and non-goals

### 2.1 In scope

- v1 fault latching and clearing policy,
- autonomous recovery behavior,
- recovery state-machine additions,
- service-mode lifetime and auto-exit rules,
- recovery behavior for battery, storage, clock, upload-blocked, and H10
  stale-bond/security failure classes,
- host-visible recovery telemetry required to debug unattended behavior.

### 2.2 Out of scope

- changing the session/journal/upload data contract,
- adding OTA behavior,
- changing the study-day/session model,
- changing the core radio exclusivity rule,
- replacing fail-closed storage policy with best-effort logging.

---

## 3) Design goals

The logger recovery architecture MUST satisfy all of the following:

1. **No recoverable failure may require manual `fault clear` just to resume normal unattended operation.**
2. **No recoverable failure may strand the device permanently in `SERVICE`.**
3. **Storage/data-loss risks remain fail-closed.** The device MUST stop or refuse
   logging when safe local durability cannot be guaranteed.
4. **Faults remain visible.** A fault MAY be latched while recovery is in progress,
   but it MUST auto-clear once validated recovery has succeeded.
5. **Human intent is respected.** If the user explicitly enters service mode,
   the logger remains there only while USB/VBUS remains present.
6. **USB unplug exits service mode.** Removing USB/VBUS MUST cancel human-pinned
   service mode and return the device to unattended policy evaluation.
7. **Battery semantics are strict.**
   - low-start block applies only when starting/resuming logging,
   - critical-low stop applies only while already logging.
8. **Clock invalidity is not a logging blocker by itself.** It is a quarantining
   condition, not a reason to fall into service mode.
9. **H10 stale-bond/security failures are handled automatically** with bounded,
   rate-limited bond-clear and re-pair logic.

---

## 4) Core model

### 4.1 Human service mode vs autonomous recovery hold

The runtime distinguishes two different kinds of non-logging states:

#### `SERVICE`

`SERVICE` means:

- a human is expected to be present,
- the USB CLI is the primary control surface,
- BLE logging is inactive,
- Wi‑Fi is inactive unless explicitly requested by a service command,
- the device is intentionally stopped for provisioning or diagnostics.

`SERVICE` MUST be used only for:

- boot-time service hold gesture,
- explicit host `service enter`,
- explicit factory-reset flow,
- provisioning workflows that require host interaction.

`SERVICE` MUST NOT be the default sink for recoverable runtime failures.

#### `RECOVERY_HOLD`

`RECOVERY_HOLD` means:

- the logger is not actively logging,
- the logger is not awaiting a human by default,
- the runtime is periodically validating whether the blocking condition has
  cleared,
- the runtime is permitted to auto-clear the fault and resume unattended
  operation when validated recovery succeeds.

`RECOVERY_HOLD` is the required destination for recoverable blocking faults such as:

- low battery preventing start,
- critical battery stop recovery,
- SD missing/unwritable,
- SD low-space reserve unmet,
- SD write-path failures,
- config incomplete when USB/VBUS is absent and there is no human service session.

### 4.2 New top-level runtime state

The top-level FSM defined in `logger_runtime_architecture_v1.md` is extended with:

```text
RECOVERY_HOLD
```

Responsibilities:

- keep logging stopped,
- keep Wi‑Fi off unless a specific bounded recovery probe requires it,
- keep H10 disconnected,
- periodically probe the active blocking condition,
- clear the latched fault automatically when validated recovery succeeds,
- transition to the stored resume target or through ordinary unattended mode
  selection once the device is healthy again.

### 4.3 Host-visible mapping

Until `logger_host_interfaces_v1.md` is merged with this behavior, the runtime
MUST expose the following mapping:

| Internal state | `mode` | `runtime_state` |
|---|---|---|
| `RECOVERY_HOLD` | `recovery_hold` | `recovery_hold` |

The stable host-visible `mode` set is therefore extended with:

- `recovery_hold`

The stable host-visible `runtime_state` set is therefore extended with:

- `recovery_hold`

### 4.4 Recovery terms

#### Active condition

The real-world condition currently exists, for example:

- battery is still below threshold,
- SD is still missing,
- queue still contains blocked-min-firmware entries.

#### Latched fault

The user-visible persisted fault code currently shown by the device and host APIs.

The latched fault MAY remain present after the active condition has cleared,
but only until validated recovery finishes.

#### Recovery reason

The internal reason the logger is currently in `RECOVERY_HOLD`.

The recovery reason MAY match the visible fault code exactly, but it does not
have to. In particular, some internal recovery loops do not create a new primary
latched fault.

#### Resume target

The unattended mode the device intends to return to once validated recovery succeeds.

Typical resume targets are:

- normal unattended mode selection,
- `LOG_WAIT_H10`,
- `UPLOAD_PREP`,
- `IDLE_WAITING_FOR_CHARGER`.

#### Validated recovery

Recovery is considered validated only when the device has done more than merely
observe the absence of an error. It MUST have run the condition-specific probe(s)
required by this document.

Examples:

- storage must be writable and pass a self-test, not merely “card inserted”,
- battery must recover above the clear threshold with hysteresis, not merely
  twitch one ADC sample upward,
- blocked-min-firmware must be absent in a fresh queue scan, not merely assumed.

---

## 5) Common recovery lifecycle

### 5.1 Lifecycle shape

All blocking recovery paths follow the same shape:

```text
healthy
  ↓ detect condition
fault latched
  ↓ enter recovery hold or stay in current mode if non-blocking
recovery probing
  ↓ validated good
fault cleared
  ↓ resume
```

### 5.2 Fault precedence

Only one primary latched fault code is persisted at a time in v1.

When more than one condition is simultaneously true, the visible fault code MUST
use this precedence order, highest first:

1. `sd_write_failed`
2. `sd_missing_or_unwritable`
3. `sd_low_space_reserve_unmet`
4. `critical_low_battery_stopped`
5. `low_battery_blocked_start`
6. `config_incomplete`
7. `upload_blocked_min_firmware`
8. `clock_invalid`

This precedence governs only the **primary visible latched fault code**.
It does not waive the requirement to continue tracking other active conditions internally.

### 5.3 Fault latching

When a new active condition appears:

1. if it outranks the currently latched fault, it MUST replace it,
2. if it matches the currently latched fault, the runtime MUST avoid redundant
   persistence churn,
3. if it is lower-priority than the currently latched fault, it MAY be tracked
   internally without changing the visible primary code.

Every first latch or superseding latch MUST emit a system-log event.

### 5.4 Recovery probe cadence and stability windows

The implementation MUST use bounded periodic probes with hysteresis.

Required defaults for v1:

- power-related recovery probe cadence: `5 s`
- storage-missing/low-space recovery probe cadence: `10 s`
- storage-write-failed recovery probe cadence: `15 s`, then bounded backoff up to `60 s`
- clock-valid clear dwell: `5 s`
- battery clear dwell: `30 s`
- storage clear condition: two consecutive successful validation rounds at least `1 s` apart

### 5.5 Validated clear

When the active condition is no longer present and the required validation has passed:

1. `last_cleared_fault_code` MUST be updated,
2. `current_fault_code` MUST become `none`,
3. a `fault_cleared` system-log event MUST be written,
4. recovery counters/timers for that reason MUST reset,
5. the logger MUST transition to its resume target or unattended mode selection.

Auto-clear MUST use the same persisted fault fields as manual clear.

### 5.6 Manual clear remains available

The host `fault clear` command remains a valid operator acknowledgement path.

However, manual clear MUST NOT be the only way to recover from a condition that
the firmware can validate automatically.

### 5.7 Required recovery telemetry

The runtime MUST track at least:

- current recovery reason,
- recovery attempt count,
- next scheduled recovery attempt time or delay,
- resume target,
- whether the user explicitly pinned service mode,
- last recovery action/result.

Host-visible JSON requirements appear in section 10.

---

## 6) Service mode rules

### 6.1 Service mode is human-pinned

`SERVICE` is considered human-pinned when entered by:

- boot-time service hold,
- explicit host `service enter`,
- factory reset workflow,
- automatic transition into service because USB/VBUS is present and provisioning
  is incomplete.

### 6.2 USB/VBUS removal exits service mode

If USB/VBUS is removed while the logger is in `SERVICE`, the runtime MUST:

1. cancel the human service pin,
2. discard any uncommitted staged config-import buffer,
3. clear any temporary service-unlock token,
4. emit a system-log event such as `service_auto_exit_usb_removed`,
5. immediately reevaluate unattended policy.

The device MUST NOT remain in `SERVICE` solely because it was previously in
service before the cable was removed.

### 6.3 Where service mode exits to

After USB/VBUS removal from `SERVICE`, the runtime MUST reevaluate unattended
policy as if boot-time mode selection were being rerun with current live state.

Outcomes:

- if unattended logging is legal, leave `SERVICE` and proceed toward logging,
- if upload mode is currently the correct unattended policy, proceed toward upload,
- if a blocking recoverable condition remains, enter `RECOVERY_HOLD`,
- if configuration is still incomplete and unattended operation is impossible,
  enter `RECOVERY_HOLD(config_incomplete)` rather than remaining in `SERVICE`.

### 6.4 Automatic entry into service from recovery hold

If the recovery reason is `config_incomplete` and USB/VBUS becomes present,
the runtime SHOULD transition from `RECOVERY_HOLD` into `SERVICE` automatically
to expose the provisioning CLI.

For other recovery reasons, USB/VBUS attachment alone MUST NOT force entry into
`SERVICE`; the host may still request `service enter` explicitly.

### 6.5 Radios during service mode

During `SERVICE`:

- BLE logging MUST remain inactive,
- Wi‑Fi MUST remain inactive unless explicitly invoked by a service command,
- autonomous background recovery probes MUST NOT run except cheap local state
  refresh needed to evaluate unplug-exit or read-only diagnostics.

---

## 7) `RECOVERY_HOLD` rules

### 7.1 Responsibilities

While in `RECOVERY_HOLD`, the logger MUST:

- keep H10 logging inactive,
- keep Wi‑Fi inactive unless a specific bounded recovery probe requires otherwise,
- continue refreshing local observations needed for recovery,
- run the condition-specific recovery FSM,
- clear faults automatically when validated recovery succeeds,
- return to unattended operation without host intervention when possible.

### 7.2 Button behavior

Runtime short and long presses MAY be ignored in `RECOVERY_HOLD` for v1.

The recovery logic itself, not button handling, is the normal exit path.

### 7.3 Host interaction

Read-only diagnostics MUST remain available.

If USB/VBUS is present, `service enter` MUST be allowed from `RECOVERY_HOLD`.

### 7.4 Resume behavior

Unless a fault-specific section says otherwise, leaving `RECOVERY_HOLD` MUST run
the same unattended policy evaluation used by boot.

---

## 8) Fault-specific recovery FSMs

## 8.1 `config_incomplete`

### Meaning

Required provisioning fields for unattended logging are missing.

### FSM

```text
config complete
  ↓ required fields missing
CONFIG_INCOMPLETE_LATCHED
  ↓ USB/VBUS present
SERVICE
  ↓ USB/VBUS removed
RECOVERY_HOLD(config_incomplete)
  ↓ valid config committed
CLEAR
  ↓ unattended policy reevaluation
RESUME
```

### Rules

- `config_incomplete` is not autonomously repairable by the device.
- While USB/VBUS is present, the runtime SHOULD prefer `SERVICE` to make
  provisioning easy.
- While USB/VBUS is absent, the runtime MUST NOT remain in `SERVICE`; it MUST sit
  in `RECOVERY_HOLD(config_incomplete)`.
- Clear condition: a newly validated config satisfies `normal_logging_ready`.

### Resume target

- unattended mode selection

---

## 8.2 `clock_invalid`

### Meaning

The wall clock is known invalid, but logging is still allowed with quarantine.

### FSM

```text
clock valid
  ↓ invalid clock detected
CLOCK_INVALID_LATCHED
  ↓ continue normal logging or waiting behavior
PASSIVE_MONITOR
  ↓ clock valid continuously for 5 s or successful set/sync
CLEAR
  ↓ stay in current mode
RESUME
```

### Rules

- `clock_invalid` MUST NOT by itself force `SERVICE` or `RECOVERY_HOLD`.
- If logging starts while the clock is invalid, the resulting session MUST be
  quarantined exactly as already defined elsewhere.
- If the clock becomes valid mid-session, the runtime MUST perform the required
  clock-fix split behavior and MAY keep the fault latched until the valid clock
  dwell time passes.
- Clear condition:
  - clock valid for at least `5 s`, or
  - successful explicit clock set, or
  - successful NTP sync.

### Resume target

- no mode transition is required; remain in the current top-level mode.

---

## 8.3 `low_battery_blocked_start`

### Meaning

Battery is too low to begin a new logging run when USB/VBUS is absent.

### FSM

```text
ready to start
  ↓ battery < 3.65 V and no VBUS
LOW_BATTERY_BLOCKED_START_LATCHED
  ↓
RECOVERY_HOLD(power_low_start)
  ↓ VBUS present for 5 s OR battery ≥ 3.75 V for 30 s
CLEAR
  ↓ unattended policy reevaluation
RESUME
```

### Rules

- This condition applies **only when starting or resuming** logging.
- The logger MUST NOT stop an already active logging run merely because voltage
  fell below the low-start threshold.
- On detection, the runtime MUST refuse start/resume and enter `RECOVERY_HOLD`.
- Recommended clear hysteresis is `+100 mV`; therefore the clear threshold for
  the default v1 start-block level is `3.75 V`.

### Resume target

- unattended mode selection

---

## 8.4 `critical_low_battery_stopped`

### Meaning

Battery fell to the critical stop threshold during active logging.

### FSM

```text
LOGGING
  ↓ battery ≤ 3.50 V
CRITICAL_LOW_BATTERY_DETECTED
  ↓ clean emergency stop
CRITICAL_LOW_BATTERY_STOPPED_LATCHED
  ↓
RECOVERY_HOLD(power_critical)
  ↓ VBUS present for 5 s OR battery ≥ 3.70 V for 30 s
CLEAR
  ↓ unattended policy reevaluation
RESUME
```

### Rules

- This is the only battery threshold that may force-stop an active logging run.
- On detection, the runtime MUST:
  1. close the active session/span as cleanly as possible,
  2. latch `critical_low_battery_stopped`,
  3. enter `RECOVERY_HOLD`.
- The runtime MUST stop using `low_battery_blocked_start` as the visible fault
  for an in-progress active-run battery collapse.
- Recommended clear hysteresis is `+200 mV`; therefore the clear threshold for
  the default v1 critical-stop level is `3.70 V`.

### Resume target

- unattended mode selection

---

## 8.5 `sd_missing_or_unwritable`

### Meaning

Durable logging storage is absent, malformed, unmounted, unwritable, or the
logger root cannot be prepared.

### FSM

```text
storage healthy
  ↓ card missing / mount fail / not writable / bad fs / root not ready
SD_MISSING_OR_UNWRITABLE_LATCHED
  ↓ stop or refuse logging
RECOVERY_HOLD(storage_missing)
  ↓ periodic reprobe
STORAGE_VALIDATE
  ↓ mounted + writable + logger root ready + correct fs + self-test passes twice
CLEAR
  ↓ unattended policy reevaluation
RESUME
```

### Rules

- Logging MUST fail closed while this condition is active.
- Recovery probes MUST include:
  1. storage refresh,
  2. mount/remount attempt as needed,
  3. logger directory readiness,
  4. a tiny write/delete self-test in logger state storage.
- Observing only a card-detect signal is not sufficient to clear the fault.
- Clear requires two consecutive successful full validation rounds at least `1 s`
  apart.

### Resume target

- unattended mode selection

---

## 8.6 `sd_low_space_reserve_unmet`

### Meaning

The required SD free-space reserve cannot currently be maintained.

### FSM

```text
space reserve healthy
  ↓ reserve violated
SD_LOW_SPACE_LATCHED
  ↓ stop or refuse logging
RECOVERY_HOLD(storage_low_space)
  ↓ prune + refresh + reprobe
SPACE_VALIDATE
  ↓ reserve satisfied twice
CLEAR
  ↓ unattended policy reevaluation
RESUME
```

### Rules

- On entry, the runtime MUST run the normal retention/prune policy immediately.
- While the fault is active, logging MUST remain blocked.
- Recovery probes MUST include:
  1. prune attempt for eligible already-verified sessions,
  2. fresh free-space measurement,
  3. confirmation that storage remains writable and mounted.
- Clear requires the reserve to be satisfied in two consecutive successful probes.

### Resume target

- unattended mode selection

---

## 8.7 `sd_write_failed`

### Meaning

An actual write-path failure occurred on durable study storage or required queue/
recovery metadata, even if the card still appears physically present.

### Canonical internal subreasons

The runtime SHOULD classify the underlying reason using one of these tokens:

- `session_span_open_failed`
- `session_packet_write_failed`
- `session_live_write_failed`
- `session_snapshot_write_failed`
- `session_stop_write_failed`
- `queue_refresh_failed`
- `queue_prune_failed`
- `queue_load_failed`
- `queue_rebuild_failed`
- `session_recovery_failed`

These subreasons are for logs/telemetry and do not replace the primary visible
fault code.

### FSM

```text
write path healthy
  ↓ any required durable write fails
SD_WRITE_FAILED_LATCHED
  ↓ stop logging / finalize best effort
RECOVERY_HOLD(storage_write)
  ↓ remount + self-test + queue validation
WRITE_PATH_VALIDATE
  ↓ two full successful rounds
CLEAR
  ↓ unattended policy reevaluation
RESUME
```

### Rules

- This is the highest-priority storage fault in v1.
- The runtime MUST stop active logging as soon as safe after this fault is detected.
- Recovery validation MUST be stronger than for `sd_missing_or_unwritable` and MUST include:
  1. storage refresh,
  2. mount/remount success,
  3. logger directory readiness,
  4. atomic test write/remove success,
  5. queue load/refresh success.
- A mere successful mount is not enough to clear this fault.
- Boot-time active-session recovery failures that indicate uncertain journal/live
  consistency MUST map to this fault class.

### Resume target

- unattended mode selection

---

## 8.8 `upload_blocked_min_firmware`

### Meaning

The server rejected one or more queued uploads because the firmware is too old.

### FSM

```text
upload queue unblocked
  ↓ min-firmware rejection
UPLOAD_BLOCKED_MIN_FW_LATCHED
  ↓ keep local logging allowed
PASSIVE_MONITOR
  ↓ explicit queue requeue/reset or fresh queue scan shows zero blocked entries
CLEAR
  ↓ stay in current mode
RESUME
```

### Rules

- This fault MUST NOT block local logging.
- This fault does not require `RECOVERY_HOLD` by itself.
- Firmware or build changes on boot MUST NOT auto-requeue blocked entries by
  themselves. Existing closed-session manifests remain authoritative, so the
  artifact firmware version seen by the upload server does not change just
  because newer firmware is now running locally.
- Clear requires a fresh queue scan showing zero blocked-min-firmware entries.

### Resume target

- no forced mode transition is required.

---

## 9) Non-latched autonomous recovery loops

These recovery loops are critical for unattended study use but do not necessarily
present as new top-level user-visible latched fault codes.

## 9.1 Normal H10 acquisition failures

Failures in these phases:

- scan,
- connect,
- secure,
- discover,
- PMD start,
- waiting for first packets,

MUST normally remain inside the logging acquisition loop with bounded backoff.

They MUST NOT fall into `SERVICE` unless a separate blocking fault class such as
storage failure is also active.

## 9.2 Stale-bond / security recovery

### Purpose

The logger MUST recover automatically from stale or inconsistent bond state.

### FSM

```text
LOG_SECURING / LOG_STARTING_STREAM
  ↓ repeated security-ish failures
SECURITY_FAILURE_ACCUMULATING
  ↓ threshold = 3 repeated failures for the bound H10
AUTO_CLEAR_BOND
  ↓ disconnect + reconnect
AUTO_REPAIR_PAIRING
  ↓ secure link + PMD start succeed
RESET_SECURITY_FAILURE_COUNTERS
```

### What counts as a security failure

At minimum, these events MUST count toward the stale-bond threshold when they
repeat for the currently bound H10:

- pairing completes with non-success status,
- link never reaches secure/encrypted-ready within the securing timeout,
- PMD start/setup fails with an authentication/authorization signature after the
  runtime has already attempted ordinary security establishment.

### Rules

- The automatic bond clear threshold MUST be `3` repeated secure-connection failures.
- The behavior MUST be rate-limited to avoid thrashing.
- Clearing the bond MUST be logged explicitly.
- Successful secure PMD start MUST reset the counter.
- This loop MUST try to recover within logging acquisition states, not by
  dumping the device into `SERVICE`.

## 9.3 First-packet timeout after PMD start

If PMD start appears to succeed but no first ECG/ACC packets arrive before the
first-packet deadline:

- disconnect and retry with normal bounded backoff,
- do not enter `SERVICE`,
- count toward stale-bond/security recovery only if repeated evidence suggests
  the failure is security-related.

## 9.4 Queue/session boot repair

If boot-time queue refresh, queue rebuild, or active-session recovery cannot be
validated safely, the runtime MUST classify the problem under the storage-write
fault family and enter `RECOVERY_HOLD`, not sticky `SERVICE`.

---

## 10) Required host-visible recovery telemetry

`status --json` MUST gain a new top-level object:

```json
"recovery": {
  "active": true,
  "reason": "sd_write_failed",
  "attempt_count": 3,
  "next_attempt_ms": 15000,
  "resume_mode": "logging",
  "service_pinned_by_user": false,
  "last_action": "storage_self_test",
  "last_result": "failed"
}
```

Required fields:

- `active` (boolean)
- `reason` (string or `null`)
- `attempt_count` (non-negative integer)
- `next_attempt_ms` (integer or `null`)
- `resume_mode` (string or `null`)
- `service_pinned_by_user` (boolean)
- `last_action` (string or `null`)
- `last_result` (string or `null`)

Additionally:

- `mode` MUST allow `recovery_hold`
- `runtime_state` MUST allow `recovery_hold`

Until `logger_host_interfaces_v1.md` is updated, this section defines the
required host-visible behavior.

---

## 11) Required system-log events

At minimum the runtime MUST emit explicit system-log events for:

- `fault_latched`
- `fault_cleared`
- `service_auto_exit_usb_removed`
- `bond_auto_cleared`
- `bond_auto_repaired`

The details payload SHOULD include enough information to understand:

- why recovery started,
- what action was attempted,
- whether the condition was still present,
- why the fault was cleared.

---

## 12) Implementation consequences for the current codebase

This document implies the following required code changes:

1. add top-level `RECOVERY_HOLD` runtime state,
2. stop routing recoverable failures into sticky `SERVICE`,
3. split `SERVICE` from autonomous recovery behavior,
4. auto-exit `SERVICE` on USB/VBUS removal,
5. abort staged service-only mutable operations on service auto-exit,
6. add runtime fault reconciliation and auto-clear logic,
7. fix battery policy so low-start block never stops an already-active run,
8. actually latch/use `critical_low_battery_stopped`,
9. implement automatic stale-bond recovery,
10. expose host-visible recovery telemetry.

---

## 13) Definition of done for recovery behavior

The logger recovery work is not complete until all of the following are true:

1. no recoverable failure path leaves the device stuck in `SERVICE`,
2. unplugging USB/VBUS exits `SERVICE` automatically,
3. storage faults self-clear only after validated write-path recovery,
4. battery faults clear automatically with hysteresis,
5. clock-invalid clears automatically once the clock is valid again,
6. blocked-min-firmware clears automatically once the queue is no longer blocked,
7. stale-bond/security failures recover automatically after bounded retries,
8. `status --json` makes recovery state machine progress observable,
9. every automatic clear is persisted and logged,
10. unattended logging resumes without operator intervention whenever the
    blocking condition has genuinely cleared.