#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${REPO_ROOT}"

if [[ $# -gt 0 ]]; then
  uvx ruff check "$@"
  exit 0
fi

uvx ruff check \
  scripts \
  examples/micropython \
  firmware/boards/RP2_1/manifest.py \
  firmware/boards/RP2_2/manifest.py \
  firmware/boards/RP2_2/modules/pcf8523.py
