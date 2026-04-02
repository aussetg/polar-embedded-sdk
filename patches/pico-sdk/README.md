# pico-sdk local patches

These patches target the `vendors/pico-sdk` submodule.

## Patch series

Order:
1. `0001-rp2-common-pico-btstack-rename-hids-client-to-hids-host.patch`
2. `0002-rp2-common-pico-mbedtls-drop-psa-storage-sources.patch`

## Apply

Recommended helper:

```bash
./patches/apply_pico_sdk_patches.sh
```

Apply to MicroPython vendored pico-sdk as well:

```bash
./patches/apply_pico_sdk_patches.sh --target vendors/micropython/lib/pico-sdk
```

Equivalent manual command:

```bash
git -C vendors/pico-sdk am ../../patches/pico-sdk/*.patch
```

Abort if needed:

```bash
git -C vendors/pico-sdk am --abort
```

Undo after apply (drop last commit):

```bash
git -C vendors/pico-sdk reset --hard HEAD~1
```

## What this does

- Makes `src/rp2_common/pico_btstack` compatible with BTstack 1.8 rename:
  - `src/ble/gatt-service/hids_client.c`
  - -> `src/ble/gatt-service/hids_host.c`
- Updates both CMake and Bazel source lists.
- Drops the rp2 pico_mbedtls PSA storage source files that currently require
  missing vendored headers and are not needed by the appliance TLS client path.
