# Logger update architecture

Status: Draft (long-term design note)
Last updated: 2026-03-29

Related:
- Current firmware behavior: [`logger_firmware_v1.md`](./logger_firmware_v1.md)
- Current host interfaces: [`logger_host_interfaces_v1.md`](./logger_host_interfaces_v1.md)

---

## 1) Purpose

This document captures the intended long-term update path for the custom logger firmware.

It exists so that v1 implementation choices do not paint the device into a corner.

The agreed long-term sequence is:

1. **v1:** no update implementation yet, but reserve flash layout and metadata concepts for safe A/B updates,
2. **v2:** **SD-assisted firmware updates**,
3. **later:** full **Wi‑Fi OTA** using the same A/B and rollback machinery.

---

## 2) Design goals

The update path MUST eventually provide:

- power-loss safety,
- rollback on failed boot,
- signature verification,
- no update while active logging is running,
- preservation of local study data across update attempts,
- a clear operator recovery story.

---

## 3) Non-goals

This design does not currently try to provide:

- delta/binary patch updates,
- update while logging is active,
- phone-driven updates,
- partial component hot-swaps,
- field-reconfigurable partition maps.

---

## 4) v1 reservation requirements

Even though v1 does not implement updates yet, the firmware design SHOULD reserve these conceptual flash regions:

- boot metadata
- updater / boot stub
- slot A
- slot B
- config + system-log area

The exact addresses and sizes are not frozen by this document yet, but the architecture MUST assume eventual **A/B application slots** and explicit boot metadata rather than a single in-place firmware image.

### 4.1 Practical implication

The v1 build and linker layout SHOULD avoid using all available onboard flash in ways that would make later A/B partitioning painful.

---

## 5) Update preconditions

When update support is implemented, the device MUST only begin an update when all of the following are true:

- active logging is stopped,
- BLE streaming is not in progress,
- the device is on USB power,
- session data already stored on SD is preserved,
- the update package verifies cryptographically,
- the target slot is inactive.

If pending uploads exist, policy MAY choose whether to upload first or update first, but data preservation MUST come before convenience.

---

## 6) Boot metadata model

The eventual update path requires persistent boot metadata containing at least:

- `active_slot`
- `pending_slot`
- `pending_update` (boolean)
- `trial_boot_count`
- `last_boot_status`
- `last_rollback_reason`
- `minimum_accepted_firmware_version` (optional future policy hook)

This metadata SHOULD be redundant and crash-safe.

---

## 7) SD-assisted updates (planned first update implementation)

### 7.1 Why SD-assisted first

This is the smallest safe step because it avoids introducing Wi‑Fi download complexity at the same time as slot management and rollback.

### 7.2 Intended operator flow

1. A signed update package is copied to SD or provided in service mode.
2. The device enters service/update mode.
3. The device verifies the update package.
4. The inactive slot is written.
5. Boot metadata is marked `pending_update`.
6. The device reboots into the new slot.
7. The new slot performs health confirmation.
8. If health confirmation succeeds, the new slot is committed.
9. Otherwise the boot logic rolls back to the previously active slot.

### 7.3 Minimum package contents

The long-term update package SHOULD contain:

- update manifest JSON,
- firmware image for one slot,
- cryptographic signature,
- declared version/build identifiers,
- image hash.

---

## 8) Wi‑Fi OTA (later)

Once SD-assisted updates are stable, Wi‑Fi OTA can reuse the same slot and rollback machinery.

### 8.1 Additional OTA-specific steps

1. Download signed package over HTTPS.
2. Stage package safely.
3. Verify signature and hash before writing any slot.
4. Write inactive slot.
5. Reuse the same pending/trial/commit/rollback flow as SD-assisted update.

### 8.2 OTA policy rules

OTA MUST NOT:

- start during active logging,
- rely on ambiguous partial downloads,
- delete local study data as part of the update flow.

---

## 9) Signature and integrity model

The update path SHOULD use a modern signed-manifest design.

Recommended direction:

- image hash: **SHA-256**
- signature scheme: **Ed25519**
- trust anchor: embedded public key(s)

Package verification MUST happen before the inactive slot is marked pending.

---

## 10) Trial boot and rollback

### 10.1 Trial boot

After an update writes the inactive slot, that slot becomes a **trial boot**.

### 10.2 Health confirmation

The new firmware SHOULD confirm health only after it has:

- booted stably,
- initialized critical peripherals,
- confirmed it can read config and storage state,
- reached a sane run state without immediate watchdog reset.

### 10.3 Rollback

If health confirmation is not recorded in time, or repeated watchdog resets occur, boot logic MUST roll back automatically to the last known good slot.

---

## 11) Interaction with upload and study data

Update machinery MUST preserve:

- closed session directories,
- upload queue state,
- system event log,
- config state,
- fault/update-needed evidence.

The update path MUST NOT treat session data as disposable cache.

---

## 12) Relationship to server-side compatibility policy

The upload server may eventually enforce a minimum compatible firmware version.

That policy fits naturally with this architecture:

- uploads from too-old firmware are rejected,
- the device latches update-needed state,
- the device can still continue local logging,
- the operator later updates via SD-assisted flow first, then OTA in later generations.

---

## 13) Summary

This document does not add v1 implementation work by itself.

Its job is to keep the custom logger firmware pointed at a safe long-term path:

- **A/B slots**
- **signed updates**
- **trial boot + rollback**
- **SD-assisted updates first**
- **Wi‑Fi OTA later**

That path is now part of the documented architecture, even though it is not a v1 feature yet.
