#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${REPO_ROOT}"

PRESET="${1:-fw-rp2-1-debug}"

echo "[1/4] Fast checks"
./scripts/check_fast.sh

echo "[2/4] Vendor patch/alignment checks"
./scripts/check_vendor_patches.sh

echo "[3/4] C checks (${PRESET})"
./scripts/check_c.sh "${PRESET}"

echo "[4/4] logger_firmware C checks"
./scripts/check_logger_firmware_c.sh

echo "OK: full checks passed"
