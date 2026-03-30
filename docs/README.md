# Documentation

This directory is also an **mdBook**.

- Book config: [`book.toml`](./book.toml)
- Table of contents: [`SUMMARY.md`](./SUMMARY.md)
- Local build: `mdbook build docs`
- Local serve: `mdbook serve docs --open`

This repository (**polar-embedded-sdk**) is building a **C-backed MicroPython module** for Polar BLE sensors on **rp2** (current focus: **Polar H10**; current hardware targets: **RP2-1**, based on **Pimoroni Pico Plus 2 W / RP2350B + CYW43**, and **RP2-2**, based on **Pimoroni Pico LiPo 2 XL W + Adafruit PiCowbell Adalogger / RP2350B + RM2 + PCF8523 + microSD**), built on top of a **Polar-specific, BTstack-backed C SDK core** (`polar_sdk/core/`).

The documentation is organised so that:
- the **SDK guides** are the main source of user-facing behavior and API,
- **protocol references** are small and vendor-sourced (Polar BLE SDK) or BLE SIG–sourced,
- **how-to** guides are procedural and repeatable,
- **known issues** are curated and actionable.

## Read this first

1. **SDK overview:** [`sdk_overview.md`](./sdk_overview.md)
2. **C SDK guide:** [`c_sdk/README.md`](./c_sdk/README.md)
3. **C integration cookbook:** [`c_sdk/cookbook.md`](./c_sdk/cookbook.md)
4. **MicroPython SDK guide:** [`micropython/README.md`](./micropython/README.md)
5. **Research workflows:** [`micropython/research_workflows.md`](./micropython/research_workflows.md)
6. **Protocol reference index:** [`reference/README.md`](./reference/README.md)
7. **Build toolchain requirements:** [`howto/toolchain_requirements.md`](./howto/toolchain_requirements.md)
8. **Build/flash workflow:** [`howto/build_micropython_with_polar_module.md`](./howto/build_micropython_with_polar_module.md)
9. **Validation procedures (HR + ECG):** [`howto/validation.md`](./howto/validation.md)
10. **Known issues / troubleshooting:** [`KNOWN_ISSUES.md`](./KNOWN_ISSUES.md)

## BTstack read order (for this project)

If you’re working on transport/discovery/streaming behavior, read in this order:

1. **BTstack API surface used here:** [`reference/btstack_api_surface.md`](./reference/btstack_api_surface.md)
2. **How Polar features map to BTstack protocols/events:** [`reference/btstack_protocol_mapping.md`](./reference/btstack_protocol_mapping.md)
3. **Status triage for `stats()` fields:** [`reference/btstack_status_triage.md`](./reference/btstack_status_triage.md)
4. **BTstack debug/config flags (practical shortlist):** [`howto/btstack_debug_flags.md`](./howto/btstack_debug_flags.md)
5. **BTstack change checklist (PR gate):** [`howto/btstack_change_checklist.md`](./howto/btstack_change_checklist.md)
6. **Then upstream BTstack manual pages** (architecture/how_to/protocols), interpreted with our pinned version policy in [`howto/btstack_version_alignment.md`](./howto/btstack_version_alignment.md)

## Docs map

- [`sdk_overview.md`](./sdk_overview.md) — choose between the C SDK and the MicroPython SDK
- [`c_sdk/`](./c_sdk/) — C core architecture, API status, helper-module guide, and integration cookbook
- [`micropython/`](./micropython/) — practical user documentation for the `polar_sdk` MicroPython module and research workflows
- [`specs/`](./specs/) — current custom logger firmware specs, update architecture notes, and older SDK design material
- [`reference/`](./reference/) — protocol facts (Polar + BLE standard services)
- [`howto/`](./howto/) — build + validation workflows
- [`KNOWN_ISSUES.md`](./KNOWN_ISSUES.md) — curated issues and mitigations
- [`CONVENTIONS.md`](./CONVENTIONS.md) — documentation policy and sourcing rules
- [`GLOSSARY.md`](./GLOSSARY.md) — acronyms/terminology
