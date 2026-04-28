#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${REPO_ROOT}"

BUILD_DIR="${LOGGER_FIRMWARE_BUILD_DIR:-build/logger_firmware_check}"
BUILD_TYPE="${LOGGER_FIRMWARE_BUILD_TYPE:-Debug}"

echo "[1/3] Configure logger_firmware (${BUILD_TYPE})"
cmake -S logger_firmware -B "${BUILD_DIR}" \
  -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DLOGGER_VERIFY_VENDOR_STATE=ON \
  -DLOGGER_ERROR_ON_PROJECT_WARNINGS=ON \
  -DLOGGER_ERROR_ON_LARGE_STACK_FRAMES=ON

echo "[2/3] Build logger_firmware with project warnings as errors"
cmake --build "${BUILD_DIR}" -j"$(nproc)"

COMPILE_DB="${BUILD_DIR}/compile_commands.json"
if [[ ! -f "${COMPILE_DB}" ]]; then
  echo "Missing compile database: ${COMPILE_DB}" >&2
  exit 2
fi

echo "[3/3] Re-run project-owned logger_firmware TUs under strict compile gate"
python3 scripts/check_c_strict_warnings.py \
  --compile-commands "${COMPILE_DB}" \
  --repo-root "${REPO_ROOT}" \
  --path-prefix "logger_firmware/src/" \
  --mode compile \
  --extra-flags "-Wall -Wextra -Werror -Wconversion -Wsign-conversion -Wshadow -Wformat=2 -Wformat-truncation=2 -Wundef -Wcast-align=strict -Wmissing-prototypes -Wstrict-prototypes -Wold-style-definition -Wredundant-decls -Wnested-externs -Wwrite-strings -Wframe-larger-than=1024" \
  --systemize-vendor-includes

echo "OK: logger_firmware C checks passed"