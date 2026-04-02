# tinyusb local patches

These patches target the nested TinyUSB checkout at `vendors/pico-sdk/lib/tinyusb`.

## Patch series

Order:
1. `0001-fatfs-enable-mkfs.patch`

## Apply

Recommended helper:

```bash
./patches/apply_tinyusb_patches.sh
```

Equivalent manual command:

```bash
git -C vendors/pico-sdk/lib/tinyusb am ../../../../patches/tinyusb/*.patch
```

Abort if needed:

```bash
git -C vendors/pico-sdk/lib/tinyusb am --abort
```

Undo after apply (drop last commit):

```bash
git -C vendors/pico-sdk/lib/tinyusb reset --hard HEAD~1
```

## What this does

- Enables `f_mkfs()` in the vendored FatFs configuration so appliance firmware
  can perform a conservative service-mode FAT32 reformat via FatFs instead of
  a custom formatter.