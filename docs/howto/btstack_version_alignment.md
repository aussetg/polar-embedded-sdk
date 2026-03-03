# BTstack version alignment policy

Status: Active
Last updated: 2026-03-02

This project uses a **single BTstack version policy**.

## Policy

- We target **BTstack 1.8** everywhere.
- We do **not** support multiple BTstack versions in parallel.
- If an upstream integration breaks on 1.8, we patch that integration (no compatibility shims in the SDK core).

## Baseline snapshot

At the time of authoring of this document, on 2026-03-02, `btstack` version differed significantly between
upstream, `pico-sdk` and `micropython`:

- `vendors/pico-sdk/lib/btstack`
  - commit: `501e6d2b86e6c92bfb9c390bcf55709938e25ac1`
  - describe: `v1.6.2-1-g501e6d2b8`
- `vendors/micropython/lib/btstack`
  - commit: `77e752abd6a0992334047a48038a5a3960e5c6bc`
  - describe: `v1.5.6.2`
- `vendors/btstack` (reference checkout)
  - commit: `af0dac01d02abce42643e9dcd35e80a43a673704`
  - describe: `v1.8-1-gaf0dac01d`

As we specifically make use of `btstack` API in our SDK core code, we need to ensure that all versions of `btstack` are aligned to the same version. We achieve this by pinning all BTstack trees to version 1.8.

## Alignment approach

1. **pico-sdk side**: patch `pico_btstack` integration for BTstack 1.8 file rename:
   - `hids_client.c` -> `hids_host.c`
2. **MicroPython side**:
   - keep using `lib/btstack` (MicroPython-owned BTstack path),
   - patch MicroPython’s vendored `lib/pico-sdk` integration for the same rename
     (apply pico-sdk patch stack with `--target vendors/micropython/lib/pico-sdk`),
   - include required newer BTstack HCI event sources in extmod integration
     (`hci_event.c` and optional `hci_event_builder.c`),
   - keep local MicroPython behavior patches as a deterministic patch stack.
3. Pin all BTstack trees to 1.8 (single version).

## Patch locations

- pico-sdk integration patches: `patches/pico-sdk/`
- MicroPython local patch stack: `patches/micropython/`

## Validation rule

A version bump is considered done only when both paths pass:

- `examples/pico_sdk` build (pure pico-sdk probe)
- `vendors/micropython/ports/rp2` build (with local MicroPython patch stack)

## Current execution status (2026-03-02)

- Phase 0: **done** (baseline captured, policy documented).
- Phase 1: **done** (pico-sdk patch prepared; build verified with BTstack 1.8).
- Phase 2: **done** (MicroPython patch stack extended for BTstack 1.8; rp2 build verified).

Validation snapshots:

1. pico-sdk probe with BTstack 1.8 (patched pico-sdk integration):
   - configure/build passed (`examples/pico_sdk`)
   - confirmed compilation of `.../vendors/btstack/src/ble/gatt-service/hids_host.c`
2. MicroPython rp2 with BTstack 1.8 (`lib/btstack` replaced in temp test tree):
   - initial link failures revealed missing `hci_event.c` / `hci_event_builder.c` integration
   - after patching extmod BTstack source lists, full `firmware.elf` build passed

## Quick version checks

From repo root:

```bash
git -C vendors/pico-sdk/lib/btstack describe --tags --always
git -C vendors/micropython/lib/btstack describe --tags --always
git -C vendors/micropython/lib/pico-sdk/lib/btstack describe --tags --always
git -C vendors/btstack describe --tags --always
```

Use the helper checker at any time:

```bash
./patches/check_btstack_alignment.sh
```

## Git hook guardrails (recommended)

Git cannot auto-run project hooks on clone, so setup is a one-time local step:

```bash
./scripts/setup_hooks.sh
```

This configures `core.hooksPath=.githooks` for the local clone.

Hook behavior:

- `post-checkout` (branch changes): warning on BTstack mismatch
- `post-merge`: warning on BTstack mismatch
- `pre-push`: warning by default on BTstack mismatch

Optional strict pre-push mode (per command):

```bash
BTSTACK_ALIGNMENT_ENFORCE_PRE_PUSH=1 git push
```

Emergency bypass:

```bash
SKIP_BTSTACK_ALIGNMENT_CHECK=1 git push
```

(Use only temporarily.)

## Pin/update runbook (exact order)

This is the deterministic sequence to align all BTstack trees to 1.8.

### 0) Pick the exact BTstack commit

Recommended default: official `v1.8` tag commit.

```bash
BTSTACK_PIN=420dc137399796c88b0013ee09f157046018923e
```

(If you intentionally want a post-1.8 hotfix, set another commit and keep it documented.)

### 1) Preflight: clean state

```bash
git status --short
git -C vendors/pico-sdk status --short
git -C vendors/micropython status --short
```

All three should be clean before pinning.

### 2) Align `vendors/btstack` (reference tree)

```bash
git -C vendors/btstack fetch --tags origin
git -C vendors/btstack checkout "$BTSTACK_PIN"
```

### 3) Align `vendors/pico-sdk/lib/btstack`

```bash
git -C vendors/pico-sdk/lib/btstack fetch --tags origin
git -C vendors/pico-sdk/lib/btstack checkout "$BTSTACK_PIN"
```

Commit this pointer bump in your pico-sdk fork/branch:

```bash
git -C vendors/pico-sdk add lib/btstack
git -C vendors/pico-sdk commit -m "lib/btstack: bump to BTstack 1.8"
```

Apply local pico-sdk compatibility patch (required rename for 1.8):

```bash
./patches/apply_pico_sdk_patches.sh --dry-run
./patches/apply_pico_sdk_patches.sh
```

### 4) Align `vendors/micropython/lib/btstack`

```bash
git -C vendors/micropython/lib/btstack fetch --tags origin
git -C vendors/micropython/lib/btstack checkout "$BTSTACK_PIN"
```

Also align MicroPython’s vendored pico-sdk BTstack tree so "all BTstack copies" are consistent:

```bash
git -C vendors/micropython/lib/pico-sdk/lib/btstack fetch --tags origin
git -C vendors/micropython/lib/pico-sdk/lib/btstack checkout "$BTSTACK_PIN"
```

Apply pico-sdk patch stack to MicroPython’s vendored `lib/pico-sdk` checkout:

```bash
./patches/apply_pico_sdk_patches.sh --dry-run --target vendors/micropython/lib/pico-sdk --force
./patches/apply_pico_sdk_patches.sh --target vendors/micropython/lib/pico-sdk --force
```

Apply local MicroPython patch stack (includes extmod BTstack 1.8 integration fixes):

```bash
./patches/apply_micropython_patches.sh --dry-run --force
./patches/apply_micropython_patches.sh --force
```

Commit BTstack pointer bumps and patch commits in your MicroPython fork/branch:

```bash
git -C vendors/micropython add lib/btstack lib/pico-sdk
git -C vendors/micropython commit -m "btstack: align lib trees to 1.8 + integration fixes"
```

### 5) Validate both build paths

pico-sdk probe:

```bash
cmake -S examples/pico_sdk -B build/pico_sdk_probe_bt18 \
  -DPICO_BOARD=pimoroni_pico_plus2_w_rp2350 \
  -DPICO_SDK_PATH=$PWD/vendors/pico-sdk \
  -DPICO_BTSTACK_PATH=$PWD/vendors/btstack
cmake --build build/pico_sdk_probe_bt18 -j$(nproc)
```

MicroPython rp2:

```bash
cmake --preset fw-rp2-1
cmake --build --preset fw-rp2-1
```

### 6) Record final top-level pointers

After submodule work is committed in your forks/branches, stage top-level submodule pointers:

```bash
git add vendors/btstack vendors/pico-sdk vendors/micropython
git commit -m "btstack: align pico-sdk and micropython stacks to 1.8"
```

### 7) Final alignment check

All BTstack trees should report the same commit:

```bash
for p in \
  vendors/btstack \
  vendors/pico-sdk/lib/btstack \
  vendors/micropython/lib/btstack \
  vendors/micropython/lib/pico-sdk/lib/btstack; do
  printf "%-45s %s\n" "$p" "$(git -C "$p" rev-parse HEAD)"
done
```
