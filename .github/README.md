> ⚠️ **This is a read-only mirror.** The canonical repository is at [git.sr.ht/~aussetg/polar-embedded-sdk](https://git.sr.ht/~aussetg/polar-embedded-sdk).
> Please open issues and submit patches there.

# polar-embedded-sdk

This repository is building towards an **unofficial embedded reimplementation of Polar BLE SDK protocols** for **microcontrollers**, with a **portable C SDK core** (Polar-specific, BTstack-backed) and a **MicroPython `rp2` binding**.

Current primary development target: **Polar H10** on **RP2** (current hardware: **RP2-1**, based on **Pimoroni Pico Plus 2 W / RP2350B + CYW43**) using **BTstack**.

## Why ?

When I started this project, I wanted (and still do) to monitor my ECG continuously, and the best solution for that happened to be the Polar H10 ECG strap.
Not only has it been validated scientifically, but it has a fairly open and extensive SDK/API and seemed to be the perfect candidate.

I therefore needed a way to log my ECG in order to analyze it later. There already exist applications on iOS (and, I suppose, Android) that use the Polar BLE SDK to record ECG, but I, for some reason, started this project under three wrong assumptions: that iOS applications weren't allowed to run in the background permanently, that buying a dedicated Android device would be more expensive than making some microcontroller-based dedicated logger, and finally that Bluetooth was a fairly simple, or at least standard, protocol, which would make things straightforward.
It turns out that I was wrong about all three:
- iOS applications can run permanently in the background to listen to Bluetooth if they implement the `bluetooth-central` execution mode. Every BLE notification will then wake the app. They can still be killed by the OS for memory management (or other) reasons, but as long as they implement proper state preservation and restoration, they will restart as if nothing happened (you will, however, have a tiny hole in the recording).
- My final logger is planned to cost much less than a cheap Android device, but it certainly did not cost less to develop. So many prototypes, debuggers, and sniffers...
- Of course Bluetooth isn't simple; that's why there is a whole Polar BLE SDK.

However, thanks to a combination of sunk-cost fallacy, not wanting to work on the things I actually should be working on, and heavy AI assistance, I still pursued this endeavor, hoping it may be useful to someone.

If you somehow are facing the same problem and this library doesn't suit your needs I highly recommend you consider just using an Android device. However if you end up using this library for Academic research I would be very grateful if you did indeed cite it.

## Where is the SDK / core?

The intended deliverable lives under:

- `polar_sdk/core/` — portable C **SDK core** (no MicroPython types). This is the Polar-specific, BTstack-backed core layer.
- `polar_sdk/mpy/` — MicroPython binding layer (C module glue)

Current status: active implementation (transport, HR, PMD ECG/IMU, and initial PSFTP read-only APIs are in-tree).

## Documentation

Start here:

- `docs/README.md`

## Build + tooling

- `CMakePresets.json` — canonical firmware build entrypoint (Pico 2 W + RP2-1 release/debug/workflow presets)
- `CMakeLists.txt` — repo-level build entrypoint forwarding into vendored MicroPython rp2 build
- `firmware/cmake/` — firmware build fragments (minimal Pimoroni `picographics` profile + compatibility shim)
- `examples/pico_sdk/` — standalone C probe (pico-sdk + BTstack) used for isolation testing
- `scripts/setup_hooks.sh` — one-time local git hook bootstrap (`core.hooksPath=.githooks`) for BTstack alignment guardrails
- `scripts/check_python.sh` — run Python lint checks (Ruff via `uvx`) on scripts + MicroPython examples
- `scripts/check_fast.sh` — quick local quality gate (python lint + BTstack header policy + docs link lint)
- `scripts/check_c.sh` — C-focused quality gate (firmware preset build + clang-tidy + strict warnings + `gcc -fanalyzer` + optional `cppcheck` on `polar_sdk/core/src`)
- `scripts/check_full.sh` — aggregate gate (`check_fast` + `check_c`)
- `patches/align_btstack_trees.sh` — align BTstack trees to pinned commit and apply local patch stacks (auto-run by firmware/probe configure, can be disabled via CMake cache vars)

## Vendor deps

Third party sources live under `vendors/` (MicroPython, nanopb, ...).

Licensing note: see [`NOTICE`](./NOTICE) for third-party license terms (notably BTstack).
