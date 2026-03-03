#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${REPO_ROOT}"

PRESET="${1:-fw-rp2-1-debug}"

echo "[1/2] Fast checks"
./scripts/check_fast.sh

echo "[2/2] C checks (${PRESET})"
./scripts/check_c.sh "${PRESET}"

echo "OK: full checks passed"
