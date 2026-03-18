# MicroPython BTstack configuration in this repo

Status: Current design note
Last updated: 2026-03-18

This repository intentionally overrides the default **MicroPython rp2 BTstack** configuration for the `polar_sdk` firmware build.

## Why this override exists

The pure pico-sdk probes in this repo were the reference point for debugging the Polar H10 transport, security, PSFTP, and recording paths.

Those probe builds used a BTstack configuration that enabled the security/crypto pieces needed for the H10 workflows we validated on real hardware.

The default MicroPython rp2 BTstack configuration is slimmer and does **not** match that probe setup closely enough. In particular, the default MPY build was able to:

- scan,
- connect,
- discover PSFTP characteristics,

but then fail when PSFTP security establishment was required.

That mismatch led to failures like:

- `SecurityError: PSFTP security setup failed`
- pairing attempts completing with a non-success status while the link reconnected
- behavior diverging from the pure C probe even on the same Pico 2 W + H10 hardware

## Repository-local solution

We do **not** modify `vendors/` for this.

Instead, we provide a repo-local header at:

- `polar_sdk/mpy/btstack_inc/btstack_config.h`

And `polar_sdk/mpy/micropython.cmake` overrides:

- `MICROPY_BLUETOOTH_BTSTACK_CONFIG_FILE`

to point directly at that repo-local header.

This gives us a project-controlled BTstack configuration without patching the vendor tree.

## What is aligned with the pico-sdk probe config

The local MPY BTstack config mirrors the working pico-sdk probe profile for the settings that matter here:

- larger ACL payload budget
- CYW43 host-flow-control settings
- matching connection / L2CAP sizing
- software AES enabled
- micro-ecc enabled

The current local config lives in:

- `polar_sdk/mpy/btstack_inc/btstack_config.h`

## Extra C sources required by that config

The default MicroPython BTstack integration does not automatically compile all crypto helpers needed by the probe-style config.

So `polar_sdk/mpy/micropython.cmake` explicitly adds:

- `vendors/micropython/lib/btstack/3rd-party/rijndael/rijndael.c`
- `vendors/micropython/lib/btstack/3rd-party/micro-ecc/uECC.c`

and their include directories.

Without those additions, enabling the matching BTstack crypto options in the config header would not be sufficient.

## Important nuance

This alignment is about making the **MicroPython firmware build behave like the working pico-sdk probe environment**.

It does **not** mean every BTstack integration detail is identical between:

- the standalone pico-sdk probe examples, and
- the vendor MicroPython btstack integration.

But it removes a major source of divergence: the build-time BTstack configuration and the crypto helpers compiled into the firmware.

Later work also aligned the persistence path itself:

- rp2 now uses BTstack TLV-backed bond storage on CYW43 builds,
- the top-of-flash TLV bank is reserved away from the MicroPython VFS region, and
- rp2 overrides pico-sdk flash-safe execution so TLV writes work when core1 is dormant.

## What this did and did not solve

This config alignment was still the right change to make, but it was **not the whole story by itself**.

An on-air comparison against the pure C probe later showed that a remaining MPY failure was centered on the runtime security sequence, not on PSFTP payload encoding.

In particular, the sniffer comparison showed:

- the probe completes the expected SMP pairing flow and then reaches encrypted PSFTP traffic,
- while the failing MPY path can show:
  - `Pairing Request`
  - `Security Request`
  - `Pairing Response`
  - then no confirm/random progression.

See:

- `docs/howto/mpy_psftp_probe_air_capture_comparison.md`

That investigation was still useful, but it was not the final root cause.

The later bond-persistence work showed that the MicroPython path also had a local-storage problem:

- BTstack TLV writes were not persisting correctly on rp2/CYW43,
- so pairing could succeed for the current runtime but fail across soft resets.

After reserving the TLV flash region and fixing rp2 flash-safe execution for dormant core1, repeated bonded reconnects across MicroPython soft resets started working.

So the practical reading is:

- **BTstack config parity was necessary cleanup**, and
- **the later rp2 TLV persistence fixes were what closed the soft-reset bond-persistence gap**.

## Files involved

- `polar_sdk/mpy/btstack_inc/btstack_config.h`
- `polar_sdk/mpy/micropython.cmake`
- probe reference configs:
  - `examples/pico_sdk/btstack_config.h`
  - `examples/pico_sdk_psftp/btstack_config.h`

## Practical rule for future work

If a BLE/security issue reproduces only in the MicroPython firmware but not in the pure pico-sdk probes, check these in order before assuming the higher-level Polar logic is wrong:

1. **BTstack config parity**
2. **local bond/TLV persistence wiring**
3. **runtime sequencing differences**