#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${REPO_ROOT}"

echo "[1/5] BTstack tree alignment"
./patches/check_btstack_alignment.sh

echo "[2/5] pico-sdk patch stack"
./patches/apply_pico_sdk_patches.sh --dry-run --target vendors/pico-sdk --force

echo "[3/5] MicroPython vendored pico-sdk patch stack"
./patches/apply_pico_sdk_patches.sh --dry-run --target vendors/micropython/lib/pico-sdk --force

echo "[4/5] TinyUSB nested patch stack"
./patches/apply_tinyusb_patches.sh --dry-run --force

echo "[5/5] MicroPython patch stack"
./patches/apply_micropython_patches.sh --dry-run --force

echo "OK: vendor patch/alignment checks passed"