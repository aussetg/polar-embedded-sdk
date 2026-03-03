# polar_sdk/

This directory is the intended *product output* of the repo.

- `core/` — portable C **SDK core** (Polar-specific, BTstack-backed): transport + protocol engines + buffering.
- `mpy/` — MicroPython user C module glue that exposes the `polar_sdk` Python API.
- `proto/` — nanopb-related *inputs* we maintain (e.g. `.options`). Generated outputs are produced into `build/` and are not committed.

Current implementation is still in progress; PSFTP read-only (`list_dir`, `download`) has landed but requires on-device validation.
