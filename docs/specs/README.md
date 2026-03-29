# Archived design notes

These documents are older design/spec material kept for implementation history and cross-checking.

They are **not** the primary user-facing documentation anymore.

For current usage, start with:

- `docs/c_sdk/`
- `docs/micropython/`

Pre-beta policy still applies: API/schema details may change intentionally until first beta/stable freeze (`schema_version = 1` in `capabilities()`).

## Index

- **Polar SDK core + MicroPython module (rp2 + BTstack):** [`micropython_polar_sdk_driver.md`](./micropython_polar_sdk_driver.md)
- **API design draft (C + MicroPython):** [`polar_sdk_api_design.md`](./polar_sdk_api_design.md)
- **(Optional) Upstream MicroPython/BTstack hardening proposal:** [`micropython_btstack_global_fix.md`](./micropython_btstack_global_fix.md)
