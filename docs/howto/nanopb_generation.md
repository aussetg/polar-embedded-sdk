# How-to — nanopb generation

Status: Active
Last updated: 2026-03-02

This repo vendors:
- nanopb: `vendors/nanopb/`
- Polar BLE SDK proto sources: <https://github.com/polarofficial/polar-ble-sdk>

Generated outputs are build artifacts and are **not committed**.

## Generate

Use the project script:

```bash
./polar_ble/proto/generate_nanopb.sh \
  /path/to/polar-ble-sdk/sources/Android/android-communications/library/src/sdk/proto
```

Default output directory:

- `build/polar_proto/`

Optional custom output directory:

```bash
./polar_ble/proto/generate_nanopb.sh \
  /path/to/polar-ble-sdk/sources/Android/android-communications/library/src/sdk/proto \
  /tmp/polar_proto_out
```

## Inputs and options

- Local nanopb options live in `polar_ble/proto/options/`.
- Current options bound:
  - request path string size (`pftp_request.options`)
  - directory entry name size (`pftp_response.options`)

## Build integration

When `POLAR_ENABLE_PSFTP=ON`, `polar_ble/mpy/micropython.cmake` expects generated files in `build/polar_proto/` (or `POLAR_PROTO_GENERATED_DIR`) and fails configure with an actionable message if missing.
