# Documentation conventions

This repository’s `docs/` directory is intended to be the **primary guide** for implementing and validating the Polar H10 stack.

Terminology used in this repo:
- **SDK core**: the Polar-specific, BTstack-backed C core under `polar_sdk/core/`.
- **MicroPython binding**: the `polar_sdk` user module under `polar_sdk/mpy/`.

## 1) Document types (and what is “canonical”)

We separate documents by intent so it’s always clear what to trust when information conflicts.

### Specs (`docs/specs/`) — *canonical, normative*

Specs define:
- required behavior,
- public APIs,
- architecture constraints,
- acceptance criteria.

If you are implementing features, start here.

### Protocol reference (`docs/reference/`) — *normative for protocol details*

These documents contain protocol facts (UUIDs, frame layouts, error codes, etc.).

Rules:
- Prefer **direct citations** from the [Polar BLE SDK](https://github.com/polarofficial/polar-ble-sdk) for Polar-specific details.
- For standard BLE services (HR, Battery, Device Info), cite Bluetooth SIG specs.
- Keep reference docs **small, precise, and implementation-oriented**.

### How-to (`docs/howto/`) — *procedural*

Step-by-step guides for building, flashing, and validation workflows.

### Known issues (`docs/KNOWN_ISSUES.md`) — *curated and actionable*

`KNOWN_ISSUES.md` captures **confirmed, user-visible problems** and mitigations.

It is not a roadmap; it should remain factual and help debugging/validation.

## 2) Sourcing rules

- **Do not copy large chunks** of vendor code into docs.
- Always include **file paths** (and ideally function/type names) when citing vendor sources.
- When adding new protocol facts, prefer **the official Polar BLE SDK** (https://github.com/polarofficial/polar-ble-sdk).
- Keep a clear separation between:
  - **observations** (what we measured on hardware), and
  - **requirements** (what the SDK core / module must do).

## 3) Naming and dating

- Stable documents: `snake_case.md` (e.g. `micropython_polar_sdk.md`).
- Prefer a short header block in key docs:
  - `Status:` (Draft / Active / Stable)
  - `Last updated:` (YYYY-MM-DD)

## 4) Docs QA

Run link checks before/after major doc edits.
