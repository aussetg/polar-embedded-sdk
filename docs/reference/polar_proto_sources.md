# Polar protocol buffer sources (PSFTP/PFTP)

Status: Reference (vendor-sourced)
Last updated: 2026-02-24

Polar BLE SDK version: tag **6.15.0** ([GitHub](https://github.com/polarofficial/polar-ble-sdk)).

Polar PSFTP/PFTP is defined by a set of `.proto` files in the official Polar BLE SDK.

Source directory (Android SDK):

- https://github.com/polarofficial/polar-ble-sdk/tree/master/sources/Android/android-communications/library/src/sdk/proto

## Minimal set for basic PSFTP operations

For directory listing + file download you typically need:

- `types.proto`
- `structures.proto`
- `pftp_error.proto`
- `pftp_notification.proto`
- `pftp_request.proto`
- `pftp_response.proto`

And, because these protos use nanopb options extensions:

- `nanopb.proto`
- `google/protobuf/descriptor.proto` (found under `proto/google/protobuf/`)

## Exercise / recorded data decoding

If you want to decode downloaded exercise files (e.g. `BASE.BPB`, `SAMPLES.BPB`, `RR.BPB`), you’ll also need:

- `exercise_base.proto`
- `exercise_samples.proto`
- `exercise_samples2.proto`
- `exercise_rr_samples.proto`

(And potentially others depending on which files you download: routes, sleep, nightly recovery, etc.)

## Important note about this repository

Generated nanopb outputs (`*.pb.c`, `*.pb.h`) are **build artifacts** and are not currently committed.

The SDK/Module spec and task plan assume we will:
- keep `.proto` sources referenced from the Polar BLE SDK repository,
- generate nanopb C sources during development/build into a dedicated generated directory.
