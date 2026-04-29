# Custom logger firmware v1 specification

Status: Draft (implementation target)
Last updated: 2026-04-14

Related:
- Data/storage/upload contract: [`logger_data_contract_v1.md`](./logger_data_contract_v1.md)
- Stable host/CLI interfaces: [`logger_host_interfaces_v1.md`](./logger_host_interfaces_v1.md)
- Runtime architecture and state machine: [`logger_runtime_architecture_v1.md`](./logger_runtime_architecture_v1.md)
- Recovery architecture, fault FSMs, and service/unplug behavior: [`logger_recovery_architecture_v1.md`](./logger_recovery_architecture_v1.md)
- Long-term update path: [`logger_update_architecture.md`](./logger_update_architecture.md)
- RP2-2 hardware profile: [`../howto/rp2_2_prototype.md`](../howto/rp2_2_prototype.md)
- BLE stability notes for Pico-class CYW43/RM2 targets: [`../howto/pico2w_ble_stability.md`](../howto/pico2w_ble_stability.md)
- Polar PMD reference: [`../reference/polar_pmd.md`](../reference/polar_pmd.md)

---

## 1) Purpose

This document defines the target behavior for the **custom pico-sdk/C logger firmware** used for the N-of-1 hypersomnia study.

This firmware is a **logger appliance**, not a general-purpose sensor demo and not a thin variant of the current MicroPython path.

Its job is to:

1. collect raw Polar H10 ECG reliably during ordinary waking life,
2. timestamp it credibly,
3. represent gaps and failures explicitly,
4. survive crashes, disconnects, and temporary network outages,
5. upload closed daily sessions safely later.

The key words **MUST**, **MUST NOT**, **SHOULD**, and **MAY** are normative.

---

## 2) Scope

### In scope for v1

- custom firmware for **RP2-2**,
- a single bound **Polar H10**,
- **ECG + ACC**,
- raw PMD capture plus logger metadata,
- RTC-backed UTC timestamps,
- SD-backed durable local storage,
- end-of-day Wi‑Fi upload,
- USB provisioning/service CLI,
- stable JSON host interfaces,
- fault handling and recovery behavior.

### Out of scope for v1

- concurrent BLE logging + Wi‑Fi upload,
- USB mass-storage mode,
- encryption at rest on the SD card,
- OTA update implementation,
- phone app or BLE-based configuration UX.

---

## 3) Product summary

The v1 logger is defined by the following product decisions:

- **Primary hardware target:** `RP2-2`
- **On-device source of truth:** raw **PMD data packets + logger metadata**
- **Primary streams:** ECG + ACC
- **Session unit:** one **study day**
- **Study day rollover:** **04:00 local time**
- **Upload trigger policy:** charger after **22:00 local**, or manual long-press fallback
- **Overnight charger boot window:** **22:00–06:00 local** enters upload/idle mode instead of logging
- **Storage policy:** durable SD storage is mandatory; fail closed otherwise
- **Retention after verified upload:** **14 days**
- **Offline backlog target:** at least **14 days** of still-unuploaded sessions
- **Wi‑Fi during logging:** fully off

---

## 4) Hardware target and assumptions

The canonical v1 target is **RP2-2** as documented in [`../howto/rp2_2_prototype.md`](../howto/rp2_2_prototype.md).

The firmware assumes the presence of:

- RP2350 + onboard wireless,
- external RTC,
- microSD storage,
- battery voltage sensing,
- USB/VBUS sensing,
- a user button,
- a simple visible fault indicator.

The firmware MUST treat this hardware profile as the primary implementation target.

---

## 5) Required provisioning state

Normal unattended logging is allowed only when all of the following are configured:

- `logger_id`
- `subject_id`
- a bound H10 BLE address
- a configured local timezone rule

Upload settings are **not** mandatory for local logging. Missing upload settings MUST be surfaced as a **warning state**, not as a blocker for local capture.

If the device is partially provisioned, it MUST expose **service/provisioning mode** when USB/VBUS is present and MUST otherwise remain out of normal unattended logging until provisioning is complete.

---

## 6) Operating modes

The logger has six top-level operating modes and one fault overlay.

### 6.1 Service / provisioning mode

Used for:

- provisioning,
- config import/export,
- staged chunked config import for large provisioning payloads,
- preflight checks,
- network tests,
- SD formatting,
- factory reset,
- system-log export,
- diagnostics.

### 6.2 Active logging mode

Used for:

- scanning for the bound H10,
- connecting and securing the link,
- starting ECG/ACC streaming,
- persisting chunked journal data,
- recording markers, gaps, and status snapshots.

### 6.3 Recovery-hold mode

Used when:

- logging is intentionally stopped by a recoverable blocking condition,
- the logger is autonomously waiting for that condition to clear,
- the logger is periodically validating recovery without assuming a human is present.

Recovery-hold mode is distinct from service/provisioning mode. It is the normal
destination for recoverable unattended faults such as storage and battery
blocking conditions.

### 6.4 Upload mode

Used for:

- enabling Wi‑Fi,
- NTP correction when network configuration permits it,
- uploading closed session bundles,
- retrying pending uploads with backoff.

### 6.5 Idle/waiting-for-charger mode

Used when:

- logging is stopped,
- uploads are queued,
- the device is waiting for USB power before uploads may proceed.

### 6.6 Idle/upload-complete mode

Used when:

- the device is on USB power,
- logging is stopped,
- all pending uploads have completed or no upload can currently proceed.

### 6.7 Fault overlay

Faults are an overlay, not a separate top-level mode. A fault MAY coexist with service, recovery-hold, idle, upload, or logging behavior depending on severity.

Visible fault indication MUST be **latched** until cleared or acknowledged, including automatic validated clear performed by firmware recovery policy.

---

## 7) Boot and mode-selection rules

### 7.1 Service-mode entry

The device MUST enter service mode when any of the following are true:

- provisioning is incomplete and USB/VBUS is present,
- the user forces service mode via button-hold at boot,
- the device is otherwise directed into service mode by explicit recovery logic.

### 7.2 Normal unattended boot rules

If provisioning is complete:

1. **Boot on charger during 22:00–06:00 local:** enter upload/idle mode.
2. **Boot on charger outside 22:00–06:00 local:** start normal logging.
3. **Boot off charger:** start normal logging.

If the local-time decision cannot be evaluated because the wall clock is invalid, charger/time boot gating MUST NOT block logging. In that case, if provisioning is otherwise complete and no service-mode gesture is active, the device MUST boot into normal logging and quarantine any resulting session data.

### 7.3 Unplug behavior

If the device is in overnight upload/idle mode and USB power is removed, it MUST immediately transition back to normal logging mode, regardless of whether the local time is before 06:00.

If the device is in service mode and USB/VBUS is removed, it MUST automatically
leave service mode and reevaluate unattended policy. It MUST NOT remain in
service mode solely because it was previously placed there by a human while USB
power was present.

---

## 8) H10 ownership and connection policy

### 8.1 Bound sensor model

Each logger is bound to exactly **one** H10 address.

The firmware MUST:

- connect only to the configured bound address,
- persist BLE bonding information across reboots,
- prefer continuity and unattended recovery over aggressive user prompting.

### 8.2 Reconnect policy

If the H10 is unavailable or disconnects, the logger MUST keep retrying for the remainder of the logging day using bounded backoff.

### 8.3 Stale-bond recovery

If secure connection establishment fails repeatedly and the stored bond appears stale, the logger MUST:

- count such failures per study day,
- automatically clear the bond and re-pair only after **3** repeated secure-connection failures,
- rate-limit that behavior to avoid thrashing.

### 8.4 What counts as a real session

A study day produces a session bundle only if the device achieves a **successful PMD stream** and therefore records at least one real ECG or ACC span.

The following do **not** produce a session bundle by themselves:

- only scanning,
- only failed connects,
- only encrypted connects with no successful PMD start,
- only PMD control-path attempts with no real ECG or ACC packets.

Days with no successful PMD stream MUST still be summarized in the internal system log.

---

## 9) Session and span model

### 9.1 Study day

The study day is defined in local time with a **04:00** rollover boundary.

The local study-day label MUST be derived from the configured timezone rule, not from a fixed UTC offset.

### 9.2 Session creation

Session artifacts MUST be created **lazily**, only when the first real ECG or ACC span starts.

Under ordinary uninterrupted use there is typically one session per study day. However, if logging is explicitly stopped and later restarted before the next `04:00` rollover, the new capture period MUST become a **new session** with:

- the same `study_day_local`,
- a new `session_id`,
- fresh session start/end metadata.

### 9.3 Span boundaries

Every reconnect boundary MUST create a new span.

The logger MUST treat the following as mandatory new-span events:

- H10 disconnect/reconnect,
- 04:00 study-day rollover,
- crash recovery continuation,
- clock-fix split after invalid time,
- accepted large clock jump,
- explicit stop/start transitions.

### 9.4 Gap handling

Gaps MUST be explicit. They MUST NOT be silently bridged.

### 9.5 Rollover behavior during continuous wear

If the H10 remains connected across 04:00 local time, the logger MUST:

1. end the current session,
2. end the current span,
3. start a new session for the new study day,
4. start a new span in that new session,
5. preserve continuity metadata that makes the split explicit.

### 9.6 Rollover behavior while no span is open

If `04:00` local occurs while the logger is between spans:

- if a session for the ending study day already exists, that session MUST be closed at rollover and any in-flight reconnect/start attempt MUST be abandoned,
- if no session exists yet for the ending study day, the device MUST finalize the no-session day summary and continue attempting logging for the new study day without creating a synthetic empty session.

### 9.7 Session boundaries on clock correction

If a clock fix or accepted large clock jump would change the derived `study_day_local` for subsequent data, the firmware MUST:

1. close the current span,
2. close the current session,
3. preserve the original session exactly as already recorded,
4. start a new session for the corrected study day before writing later ECG data.

If the corrected time does **not** change `study_day_local`, the boundary is a new span inside the existing session.

---

## 10) Time policy

### 10.1 Canonical stored timestamps

All stored timestamps for session artifacts MUST use **UTC**.

Local timezone information MUST be stored separately as metadata.

### 10.2 Clock sources

- The external RTC is the primary wall clock.
- NTP correction SHOULD be attempted when Wi‑Fi is already enabled in upload/service mode and network configuration permits it.
- Wi‑Fi MUST NOT be enabled solely to get time during active logging.

### 10.3 Invalid clock behavior

If RTC time is invalid at boot:

- the device MUST still log if all non-clock prerequisites for logging are satisfied,
- stored UTC timestamps and `study_day_local` MUST use the wall-clock reading actually observed by the logger at capture time, even though it is known invalid,
- those stored values MUST be preserved as originally recorded and MUST NOT be rewritten later,
- resulting session data MUST be marked **quarantined**,
- the fault/clock status MUST be visible in diagnostics and manifests.

### 10.4 Clock-fix split

If a previously invalid clock becomes valid later, the firmware MUST split cleanly at that point rather than rewriting earlier timestamps.

### 10.5 Large NTP correction policy

A correction larger than **5 minutes** is a **large clock jump**.

If an NTP result passes source sanity checks and proposes a large correction, the logger MUST apply it and then:

- be explicitly logged,
- create a fresh boundary before subsequent data is considered cleanly timed,
- mark timing provenance clearly in the session metadata.

If the resulting corrected local time crosses the study-day boundary, the boundary MUST be a new session, not merely a new span.

---

## 11) User input and visible indication

### 11.1 Short press

A short button press during active logging MUST record a generic timestamped `MARKER` event.

### 11.2 Long press

A long button press MUST stop active logging immediately and move the device into upload/queued-work behavior.

If USB power is absent, upload MAY proceed only if battery state satisfies the off-charger upload threshold defined below.

Long-press behavior is only defined in logging-related modes. In service and idle modes it MUST be ignored unless a future document defines otherwise.

### 11.3 Visible indication

v1 requires only a **minimal visible fault indicator**.

The indicator:

- MAY be a single LED with blink codes,
- MUST support latched fault display,
- MUST persist fault state across reboots.

Rich UI is intentionally out of scope for v1.

Exact button/boot gesture timing is defined in [`logger_runtime_architecture_v1.md`](./logger_runtime_architecture_v1.md).

---

## 12) Power policy

### 12.1 Battery thresholds

Internal battery decisions use voltage thresholds.

Configured v1 thresholds:

- **Critical stop:** `3.50 V`
- **Low-start block:** `3.65 V`
- **Off-charger upload allowed:** `3.85 V`

The firmware MAY expose percent-like estimates for UX/diagnostics, but voltage thresholds are authoritative.

### 12.2 Start-block rule

If the device is about to start a new logging run and battery voltage is below `3.65 V`, it MUST refuse to start and raise a low-battery fault.

USB power overrides this low-start block.

### 12.3 Critical low-battery rule

If battery voltage reaches `3.50 V` during active logging, the logger MUST perform a clean emergency stop and preserve the day’s captured data.

### 12.4 Long-press off charger

If a long press stops logging while USB power is absent:

- if battery is at or above `3.85 V`, upload MAY proceed on battery,
- otherwise, the device MUST queue uploads and wait idle for USB power.

For v1, off-charger upload triggered by long press MUST perform at most one upload pass over the current queue. It MUST NOT remain in indefinite retry/backoff on battery power.

---

## 13) Storage and durability policy

### 13.1 Storage medium

The primary logging store is a host-readable **FAT32 microSD card**.

### 13.2 Fail-closed rule

If durable SD storage is unavailable, malformed, unwritable, or cannot maintain the required free-space reserve, the device MUST refuse or stop logging.

### 13.3 Write-failure handling

If SD writes begin failing during active logging, the firmware MUST retry briefly for up to **15 seconds**. If the problem persists, it MUST emergency-stop logging and latch a storage fault.

### 13.4 Crash-loss budget

The design MAY tolerate up to **60 seconds** of not-yet-durable recent data loss on sudden crash or power loss.

### 13.5 Free-space reserve

The logger MUST protect a minimum free-space reserve of **512 MB**.

If space is low, it MUST early-prune the oldest sessions that have already been successfully verified-uploaded before allowing the reserve to be violated.

If the reserve still cannot be maintained after eligible pruning, the device MUST fault and stop/refuse logging.

### 13.6 Retention

Sessions that were verified-uploaded MUST be retained locally for **14 days**, unless early-pruned sooner to protect the minimum reserve.

---

## 14) Upload policy

### 14.1 Wi‑Fi concurrency rule

Wi‑Fi MUST remain fully off during active BLE logging.

### 14.2 When upload mode begins

Upload mode begins when logging is no longer active and one of the following occurs:

- USB power is connected after **22:00 local**,
- the device is already on USB power and **22:00 local** arrives,
- a manual long press ends logging,
- the device boots on USB power during the **22:00–06:00** overnight window.

Reaching **22:00 local** while USB power is absent does **not** stop logging by itself in v1.

### 14.3 Upload queue processing

Upload mode MUST try to process **all pending closed sessions**, oldest first.

While USB power is present, retries use the normal upload backoff policy.

When upload mode was entered by an off-charger long press, the firmware MUST make a single best-effort pass over the queue and then stop retrying until USB power is later attached.

When `upload.url` uses `https://`, the firmware MUST verify the server certificate chain against the built-in public root profile `logger-public-roots-v1` and MUST perform hostname verification against the URL host. For backward-compatible configs, omitting explicit upload TLS settings for an HTTPS URL is interpreted as this built-in `public_roots` mode.

Before constructing an upload request, firmware MUST validate the configured
URL, `logger_id`, API key, and bearer token using the stable v1 config grammar.
Values containing NUL, CR, LF, tab, other control bytes, DEL, URL userinfo, or
URL fragments are malformed config. Firmware MUST block upload for malformed
config; it MUST NOT trim, escape, or send those values.

### 14.4 Upload success semantics

A session is considered uploaded only when the server has:

- received the canonical tar stream,
- verified the declared hash,
- validated the immutable session artifacts,
- returned a positive acknowledgment.

### 14.5 Ambiguous failures and retries

Retries MUST be safe and idempotent. Re-uploading an already accepted session MUST be treated by the server as a success.

Automatic retries are only for failures that can plausibly clear without
changing the immutable session artifact: transport failures, temporary server
failures, or server responses explicitly marked retryable. Hard per-session
rejections such as validation failure, body too large, duplicate/conflict
rejection, acknowledgment hash mismatch, or local immutable artifact corruption
MUST be persisted as `nonretryable` queue entries. `nonretryable` entries MUST
not be retried until an explicit service-side requeue action moves them back to
`pending`.

### 14.6 Firmware-version rejection

If the server rejects upload because firmware is too old, the logger MUST:

- keep the session queued,
- stop retrying that upload automatically,
- require an explicit queue requeue/reset before that session is retried,
- latch an update-needed fault,
- continue allowing new local logging.

Reflashing newer firmware later does not change the immutable
closed-session `manifest.json` for already-recorded sessions, so an old blocked
session does not become uploadable solely because the running firmware changed.

### 14.7 Unplug during upload

If USB power is removed during upload mode, the logger MUST abort upload work and return immediately to logging mode.

### 14.8 After successful overnight upload

If all pending uploads complete while USB power remains present, the device MUST stay in idle/upload-complete mode until unplugged or explicitly serviced.

If upload mode was entered by an off-charger long press and all pending uploads complete successfully, the device MUST remain idle until reboot, explicit service action, or a later USB/VBUS transition changes policy.

---

## 15) Periodic telemetry requirements

During active logging the firmware MUST record:

- a general status snapshot at least every **5 minutes**,
- H10 battery level on every connect/reconnect,
- H10 battery level periodically every **60 minutes** thereafter.

For v1 these are journal/session artifacts once a session exists. Before the first real ECG or ACC span of the day, the device MAY track equivalent diagnostics internally or in the system log, but no session journal exists yet.

Snapshots SHOULD include battery state, VBUS state, free SD space, and other lightweight diagnostic counters useful for post-hoc triage.

At minimum every status snapshot MUST include battery state, VBUS state, free SD space, reserve threshold, and current fault code.

---

## 16) Quarantine and fault policy

### 16.1 Quarantine conditions

Sessions MAY be uploaded automatically even when quarantined.

Quarantine reasons include, at minimum:

- invalid clock at session start,
- clock fixed mid-session,
- accepted large clock jump,
- recovery after unclean shutdown,
- other conditions that preserve data but weaken timing confidence.

### 16.2 Fault persistence

Current fault code MUST survive reboot until cleared or acknowledged.

In v1 the normal explicit operator clear/acknowledge path is the host CLI `fault clear` command.

However, recoverable faults MUST also be eligible for automatic validated clear
by firmware policy as defined in [`logger_recovery_architecture_v1.md`](./logger_recovery_architecture_v1.md).

The canonical latched fault code taxonomy and blink-code mapping are defined in [`logger_runtime_architecture_v1.md`](./logger_runtime_architecture_v1.md).

### 16.3 System log

Major lifecycle events, resets, provisioning changes, no-session day summaries, and fault acknowledgments MUST be written to a separate append-only system log in internal flash.

---

## 17) Non-goals for v1

v1 intentionally does **not** attempt to provide:

- on-device analytics,
- feature extraction instead of raw capture,
- arbitrary multi-sensor pairing,
- background Wi‑Fi during logging,
- encrypted SD files,
- OTA implementation,
- phone-app UX,
- compatibility shims for older design ideas.

---

## 18) Minimum acceptance criteria

The firmware is not ready for study use until all of the following are true:

1. It can auto-start logging on RP2-2 without operator babysitting.
2. It binds to one H10 and reconnects unattended.
3. It records raw ECG and ACC PMD data to SD durably.
4. Every disconnect/reconnect becomes an explicit gap and new span.
5. 04:00 rollover creates a new session and new span cleanly.
6. Invalid-clock runs are preserved but clearly quarantined.
7. Sudden reset recovery is append-only and leaves prior durable data intact.
8. Wi‑Fi remains off during active BLE logging.
9. Closed session uploads are safe to retry idempotently.
10. Successfully uploaded sessions are retained for 14 days, then pruned safely.
11. The logger faults visibly on SD/storage conditions that would otherwise cause silent data loss.
12. The host/CLI JSON interfaces are stable enough for repeatable tooling.
13. Days with no successful PMD stream still appear in the internal system log as explicit missingness.
14. Recoverable failed states clear automatically once validated recovery succeeds, without requiring operator babysitting.
15. Removing USB/VBUS exits service mode automatically and returns the device to unattended policy evaluation.

This document defines the firmware behavior. File formats, manifests, upload tar construction, and upload API details are defined in [`logger_data_contract_v1.md`](./logger_data_contract_v1.md).
