# polar_ble/proto

Inputs and tooling for PSFTP/PFTP protobuf generation.

## Layout

- `options/` — local nanopb `.options` overrides (embedded bounds).
- `generate_nanopb.sh` — deterministic generator wrapper.
- Generated `*.pb.c/*.pb.h` files are emitted to `build/polar_proto/` and are **not committed**.

## Inputs

Source `.proto` files come from Polar BLE SDK:

- `sources/Android/android-communications/library/src/sdk/proto/`

Minimal required files for PSFTP list/download:

- `types.proto`
- `structures.proto`
- `pftp_error.proto`
- `pftp_notification.proto`
- `pftp_request.proto`
- `pftp_response.proto`
- `nanopb.proto`
- `google/protobuf/descriptor.proto`

## Generate

```bash
./polar_ble/proto/generate_nanopb.sh \
  /path/to/polar-ble-sdk/sources/Android/android-communications/library/src/sdk/proto
```

Optional output directory override:

```bash
./polar_ble/proto/generate_nanopb.sh \
  /path/to/polar-ble-sdk/sources/Android/android-communications/library/src/sdk/proto \
  /tmp/polar_proto_out
```

Default output:

- `build/polar_proto/`

## Notes

- Generation uses `vendors/nanopb/generator/nanopb_generator.py`.
- Local `.options` are copied next to proto inputs during generation.
- Build integration expects generated files in `build/polar_proto/` when PSFTP is enabled.
