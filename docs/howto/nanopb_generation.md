# How-to — nanopb generation (planned clean workflow)

Status: How-to (planned)
Last updated: 2026-02-24

This repo vendors:
- nanopb: `vendors/nanopb/`
- Polar BLE SDK: https://github.com/polarofficial/polar-ble-sdk (contains the `.proto` sources)

The project previously had `proto/`, `options/`, and `generated/` directories at the repo root. Those were **deleted intentionally** to restart clean.

This document describes the intended *from-scratch* workflow to re-generate protobuf bindings when PSFTP/PFTP implementation begins.

## Recommended generated layout (not committed)

When implementation starts, generate into something like:

- `build/polar_proto/` *(or `src/_generated/` if you prefer, but still usually not committed)*

Example:

```
build/polar_proto/
  pftp_request.pb.c
  pftp_request.pb.h
  ...
```

## Proto source location

Proto files live here:

- https://github.com/polarofficial/polar-ble-sdk/tree/master/sources/Android/android-communications/library/src/sdk/proto

See also: [`../reference/polar_proto_sources.md`](../reference/polar_proto_sources.md)

## Generation script (outline)

A future script should:

1. Copy (or reference via `-I`) the Polar `.proto` directory.
2. Provide nanopb `.options` files (max_size/max_count) for embedded constraints.
3. Run `nanopb_generator.py` to emit `*.pb.c/h`.

The nanopb generator entrypoint in this repo is:

- `vendors/nanopb/generator/nanopb_generator.py`

## Notes / pitfalls

- Many Polar protos are **proto2** and require explicit `max_size` / `max_count` for nanopb.
- Some messages contain very large repeated fields (exercise samples). Plan for:
  - streaming decode (nanopb callbacks), or
  - a “download raw bytes” approach and decode on a host.

This is intentionally left as *documentation only* for now (no generation scripts added yet).
