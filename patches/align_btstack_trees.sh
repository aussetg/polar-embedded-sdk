#!/usr/bin/env bash
set -euo pipefail

# Align all BTstack trees to a single pinned commit and re-apply local patch stacks.
#
# This script intentionally does NOT create commits inside vendor submodules.
# Canonical reproducibility comes from:
# - this pinning script,
# - patches/apply_* scripts,
# - top-level repository history.

PIN="${BTSTACK_PIN:-420dc137399796c88b0013ee09f157046018923e}"
DRY_RUN=0
FETCH=1

usage() {
  cat <<'EOF'
Usage: patches/align_btstack_trees.sh [--pin <sha>] [--dry-run] [--no-fetch]

Options:
  --pin <sha>   BTstack commit to pin all trees to
  --dry-run     Validate and print actions without mutating trees
  --no-fetch    Do not fetch tags from remotes before checkout
  -h, --help    Show this help

Environment:
  BTSTACK_PIN   Default pin if --pin is not provided
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --pin)
      if [[ $# -lt 2 ]]; then
        echo "--pin requires a value" >&2
        exit 2
      fi
      PIN="$2"
      shift 2
      ;;
    --dry-run)
      DRY_RUN=1
      shift
      ;;
    --no-fetch)
      FETCH=0
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      usage
      exit 2
      ;;
  esac
done

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${REPO_ROOT}"

PATHS=(
  "vendors/btstack"
  "vendors/pico-sdk/lib/btstack"
  "vendors/micropython/lib/btstack"
  "vendors/micropython/lib/pico-sdk/lib/btstack"
)

for p in "${PATHS[@]}"; do
  if [[ ! -d "${p}" ]]; then
    echo "Missing path: ${p}" >&2
    exit 1
  fi
  if [[ ! -d "${p}/.git" && ! -f "${p}/.git" ]]; then
    echo "Not a git checkout: ${p}" >&2
    exit 1
  fi
done

echo "BTstack pin target: ${PIN}"

action_checkout() {
  local p="$1"

  if [[ "${FETCH}" == "1" ]]; then
    if ! git -C "${p}" fetch --tags origin >/dev/null 2>&1; then
      echo "[warn] fetch failed for ${p}; continuing with local refs" >&2
    fi
  fi

  if ! git -C "${p}" rev-parse --verify "${PIN}^{commit}" >/dev/null 2>&1; then
    echo "Pinned commit not found in ${p}: ${PIN}" >&2
    exit 1
  fi

  local current
  current="$(git -C "${p}" rev-parse HEAD)"

  if [[ "${DRY_RUN}" == "1" ]]; then
    if [[ "${current}" == "${PIN}" ]]; then
      echo "[ok] ${p} already at ${PIN}"
    else
      echo "[plan] ${p}: ${current} -> ${PIN}"
    fi
    return 0
  fi

  if [[ "${current}" != "${PIN}" ]]; then
    echo "[checkout] ${p}: ${current} -> ${PIN}"
    git -C "${p}" checkout "${PIN}" >/dev/null
  else
    echo "[ok] ${p} already at ${PIN}"
  fi
}

for p in "${PATHS[@]}"; do
  action_checkout "${p}"
done

if [[ "${DRY_RUN}" == "1" ]]; then
  echo "[plan] patches/apply_pico_sdk_patches.sh --dry-run --target vendors/pico-sdk --force"
  echo "[plan] patches/apply_pico_sdk_patches.sh --dry-run --target vendors/micropython/lib/pico-sdk --force"
  echo "[plan] patches/apply_micropython_patches.sh --dry-run --force"
  echo "[plan] patches/check_btstack_alignment.sh"
  exit 0
fi

# Apply compatibility patch stacks after BTstack pinning.
# (no pre dry-run here: later patches in a series can depend on earlier ones)
./patches/apply_pico_sdk_patches.sh --target vendors/pico-sdk --force
./patches/apply_pico_sdk_patches.sh --target vendors/micropython/lib/pico-sdk --force
./patches/apply_micropython_patches.sh --force

./patches/check_btstack_alignment.sh

echo "Done. BTstack trees aligned + patch stacks applied."
