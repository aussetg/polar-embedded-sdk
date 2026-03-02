#!/usr/bin/env bash
set -euo pipefail

MODE="${1:-warn}" # warn|enforce
ORIGIN="${2:-hook}"

if [[ "${SKIP_BTSTACK_ALIGNMENT_CHECK:-0}" == "1" ]]; then
  exit 0
fi

REPO_ROOT="$(git rev-parse --show-toplevel 2>/dev/null || pwd)"
CHECK_SCRIPT="${REPO_ROOT}/patches/check_btstack_alignment.sh"

if [[ ! -x "${CHECK_SCRIPT}" ]]; then
  if [[ "${MODE}" == "enforce" ]]; then
    echo "[btstack-alignment:${ORIGIN}] missing checker: ${CHECK_SCRIPT}" >&2
    echo "Run from repo root: chmod +x patches/check_btstack_alignment.sh" >&2
    exit 1
  fi
  exit 0
fi

if output="$(${CHECK_SCRIPT} 2>&1)"; then
  exit 0
fi

if [[ "${MODE}" == "enforce" ]]; then
  echo "[btstack-alignment:${ORIGIN}] ERROR: BTstack trees are not aligned." >&2
  echo >&2
  echo "${output}" >&2
  echo >&2
  echo "Fix: follow docs/howto/btstack_version_alignment.md" >&2
  echo "Temp bypass (not recommended): SKIP_BTSTACK_ALIGNMENT_CHECK=1 git push ..." >&2
  exit 1
fi

echo "[btstack-alignment:${ORIGIN}] WARNING: BTstack trees are not aligned." >&2
echo >&2
echo "${output}" >&2
echo >&2
echo "Fix: follow docs/howto/btstack_version_alignment.md" >&2
exit 0
