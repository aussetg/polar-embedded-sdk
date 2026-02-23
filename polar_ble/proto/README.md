# polar/proto

Inputs for PSFTP/PFTP protobuf generation.

- `options/` will contain nanopb `.options` files we maintain.
- `.proto` files are sourced from the [Polar BLE SDK](https://github.com/polarofficial/polar-ble-sdk) (`sources/Android/android-communications/library/src/sdk/proto/`).

Generated `*.pb.c/h` outputs should be produced into `build/` and are not committed.
