# Documentation

This repository (**polar-embedded-sdk**) is building a **C-backed MicroPython module** for Polar BLE sensors on **rp2** (current focus: **Polar H10**; current hardware target: **RP2-1**, based on **Pimoroni Pico Plus 2 W / RP2350B + CYW43**), built on top of a **Polar-specific, BTstack-backed C SDK core** (`polar_sdk/core/`).

The documentation is organised so that:
- the **spec** is the canonical source of required behavior and API,
- **protocol references** are small and vendor-sourced (Polar BLE SDK) or BLE SIG–sourced,
- **how-to** guides are procedural and repeatable,
- **known issues** are curated and actionable.

## Read this first

1. **SDK spec (canonical):** [`specs/micropython_polar_sdk.md`](./specs/micropython_polar_sdk.md)
2. **Protocol reference index:** [`reference/README.md`](./reference/README.md)
3. **Build toolchain requirements:** [`howto/toolchain_requirements.md`](./howto/toolchain_requirements.md)
4. **BTstack alignment policy (single-version target):** [`howto/btstack_version_alignment.md`](./howto/btstack_version_alignment.md)
5. **Build/flash workflow:** [`howto/build_micropython_with_polar_module.md`](./howto/build_micropython_with_polar_module.md)
6. **RP2-1 prototype hardware profile:** [`howto/rp2_1_prototype.md`](./howto/rp2_1_prototype.md)
7. **Validation procedures (HR + ECG):** [`howto/validation.md`](./howto/validation.md)
8. **Known issues / troubleshooting:** [`KNOWN_ISSUES.md`](./KNOWN_ISSUES.md)

## Docs map

- [`specs/`](./specs/) — canonical requirements and API
- [`reference/`](./reference/) — protocol facts (Polar + BLE standard services)
- [`howto/`](./howto/) — build + validation workflows
- [`KNOWN_ISSUES.md`](./KNOWN_ISSUES.md) — curated issues and mitigations
- [`CONVENTIONS.md`](./CONVENTIONS.md) — documentation policy and sourcing rules
- [`GLOSSARY.md`](./GLOSSARY.md) — acronyms/terminology
