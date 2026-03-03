#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${REPO_ROOT}"

PRESET="${1:-fw-rp2-1-debug}"

resolve_preset_binary_dir() {
  python3 - "$REPO_ROOT" "$PRESET" <<'PY'
import json
import pathlib
import sys

repo_root = pathlib.Path(sys.argv[1]).resolve()
preset_name = sys.argv[2]
presets_path = repo_root / "CMakePresets.json"

with presets_path.open("r", encoding="utf-8") as fh:
    data = json.load(fh)

presets = {p["name"]: p for p in data.get("configurePresets", [])}


def as_list(value):
    if value is None:
        return []
    if isinstance(value, list):
        return value
    return [value]


def find_binary_dir(name, seen):
    if name in seen:
        return None
    seen.add(name)
    preset = presets.get(name)
    if preset is None:
        return None
    if "binaryDir" in preset and preset["binaryDir"]:
        return preset["binaryDir"]
    for parent in as_list(preset.get("inherits")):
        bd = find_binary_dir(parent, seen)
        if bd:
            return bd
    return None

binary_dir = find_binary_dir(preset_name, set())
if not binary_dir:
    raise SystemExit(2)

binary_dir = binary_dir.replace("${fileDir}", str(repo_root)).replace("${sourceDir}", str(repo_root)).replace("${presetName}", preset_name)
print(binary_dir)
PY
}

if ! BUILD_DIR="$(resolve_preset_binary_dir)"; then
  echo "Could not resolve binaryDir for configure preset '${PRESET}' from CMakePresets.json" >&2
  exit 2
fi

COMPILE_DB_DIR="${BUILD_DIR}/rp2"
COMPILE_DB="${COMPILE_DB_DIR}/compile_commands.json"

if ! command -v clang-tidy >/dev/null 2>&1; then
  echo "clang-tidy not found in PATH" >&2
  exit 2
fi

if [[ "${POLAR_SKIP_CONFIGURE:-0}" != "1" ]]; then
  echo "[1/5] Configure preset: ${PRESET}"
  cmake --preset "${PRESET}"
else
  echo "[1/5] Configure preset: skipped (POLAR_SKIP_CONFIGURE=1)"
fi

if [[ "${POLAR_SKIP_BUILD:-0}" != "1" ]]; then
  echo "[2/5] Build preset: ${PRESET}"
  cmake --build --preset "${PRESET}"
else
  echo "[2/5] Build preset: skipped (POLAR_SKIP_BUILD=1)"
fi

if [[ ! -f "${COMPILE_DB}" ]]; then
  echo "Missing compile database: ${COMPILE_DB}" >&2
  echo "Run configure/build for preset '${PRESET}' first." >&2
  exit 2
fi

mapfile -t CORE_SOURCES < <(find "${REPO_ROOT}/polar_sdk/core/src" -type f -name '*.c' | sort)
if [[ ${#CORE_SOURCES[@]} -eq 0 ]]; then
  echo "No core C sources found under polar_sdk/core/src" >&2
  exit 2
fi

TIDY_CHECKS="${POLAR_CLANG_TIDY_CHECKS:--*,clang-analyzer-*,-clang-analyzer-security.insecureAPI.*,bugprone-*,-bugprone-easily-swappable-parameters,-bugprone-narrowing-conversions}"
TIDY_WERRORS="${POLAR_CLANG_TIDY_WERRORS:-clang-analyzer-*,bugprone-*}"
STRICT_WARNINGS_ENABLED="${POLAR_ENABLE_STRICT_WARNINGS:-1}"
STRICT_WARN_FLAGS="${POLAR_STRICT_WARN_FLAGS:--Wall -Wextra -Werror}"
HEADER_FILTER="^${REPO_ROOT}/polar_sdk/core/"

echo "[3/5] clang-tidy (core SDK sources)"
echo "       checks: ${TIDY_CHECKS}"
clang-tidy \
  -p "${COMPILE_DB_DIR}" \
  -header-filter="${HEADER_FILTER}" \
  -checks="${TIDY_CHECKS}" \
  -warnings-as-errors="${TIDY_WERRORS}" \
  "${CORE_SOURCES[@]}" \
  --quiet

if [[ "${STRICT_WARNINGS_ENABLED}" == "1" ]]; then
  echo "[4/5] strict compiler warnings (core SDK sources)"
  echo "       extra flags: ${STRICT_WARN_FLAGS}"
  python3 scripts/check_c_strict_warnings.py \
    --compile-commands "${COMPILE_DB}" \
    --repo-root "${REPO_ROOT}" \
    --path-prefix "polar_sdk/core/src/" \
    --extra-flags "${STRICT_WARN_FLAGS}"
else
  echo "[4/5] strict compiler warnings: skipped (POLAR_ENABLE_STRICT_WARNINGS=0)"
fi

echo "[5/5] OK: C checks passed"
