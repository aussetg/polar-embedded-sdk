#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

PATHS=(
  "vendors/btstack"
  "vendors/pico-sdk/lib/btstack"
  "vendors/micropython/lib/btstack"
  "vendors/micropython/lib/pico-sdk/lib/btstack"
)

printf "%s\n" "BTstack alignment report"
printf "%s\n" "========================"

base_hash=""
mismatch=0

for rel in "${PATHS[@]}"; do
  abs="${ROOT_DIR}/${rel}"
  if [[ ! -d "$abs/.git" && ! -f "$abs/.git" ]]; then
    printf "%-45s %s\n" "$rel" "MISSING"
    mismatch=1
    continue
  fi

  hash="$(git -C "$abs" rev-parse HEAD)"
  desc="$(git -C "$abs" describe --tags --always 2>/dev/null || true)"
  printf "%-45s %s  (%s)\n" "$rel" "$hash" "$desc"

  if [[ -z "$base_hash" ]]; then
    base_hash="$hash"
  elif [[ "$hash" != "$base_hash" ]]; then
    mismatch=1
  fi
done

if [[ $mismatch -eq 0 ]]; then
  printf "\n%s\n" "OK: all BTstack trees are aligned"
else
  printf "\n%s\n" "FAIL: BTstack trees are not aligned"
  exit 1
fi
