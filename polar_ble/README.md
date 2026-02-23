# polar_ble/ — Polar H10 driver + MicroPython module

This directory is the intended *product output* of the repo.

- `driver/` contains the portable C driver implementation (transport + protocol engines + buffering).
- `mpy/` contains the MicroPython C module glue that exposes the Python API.
- `proto/` contains nanopb-related *inputs* we maintain (e.g. `.options`), but generated outputs should be produced into `build/` and are not committed.

Current implementation is incomplete; see `docs/` for the plan.
