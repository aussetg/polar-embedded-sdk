# Documentation

This repository is building a **C-backed MicroPython driver** for the **Polar H10** on **rp2 / Pico 2 W (RP2350 + CYW43)**.

The documentation is organised so that:
- the **spec** is the canonical source of required behavior and API,
- **protocol references** are small and vendor-sourced (Polar BLE SDK) or BLE SIG–sourced,
- **how-to** guides are procedural and repeatable,
- **known issues** are curated and actionable.

## Read this first

1. **Driver spec (canonical):** [`specs/micropython_polar_sdk_driver.md`](./specs/micropython_polar_sdk_driver.md)
2. **Protocol reference index:** [`reference/README.md`](./reference/README.md)
3. **Build toolchain requirements:** [`howto/toolchain_requirements.md`](./howto/toolchain_requirements.md)
4. **Build/flash workflow:** [`howto/build_micropython_with_polar_module.md`](./howto/build_micropython_with_polar_module.md)
5. **Validation procedures (HR + ECG):** [`howto/validation.md`](./howto/validation.md)
6. **Known issues / troubleshooting:** [`KNOWN_ISSUES.md`](./KNOWN_ISSUES.md)

## Docs map

- [`specs/`](./specs/) — canonical requirements and API
- [`reference/`](./reference/) — protocol facts (Polar + BLE standard services)
- [`howto/`](./howto/) — build + validation workflows
- [`KNOWN_ISSUES.md`](./KNOWN_ISSUES.md) — curated issues and mitigations
- [`CONVENTIONS.md`](./CONVENTIONS.md) — documentation policy and sourcing rules
- [`GLOSSARY.md`](./GLOSSARY.md) — acronyms/terminology
