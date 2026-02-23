#!/usr/bin/env bash
set -euo pipefail

FORCE=0
DRY_RUN=0

usage() {
  cat <<'EOF'
Apply local MicroPython format-patch series to vendors/micropython.

Usage:
  patches/apply_micropython_patches.sh [--dry-run] [--force]

Options:
  --dry-run         Validate patch applicability without applying
  --force           Apply even if vendors/micropython has local changes
  -h, --help        Show this help
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --dry-run)
      DRY_RUN=1
      shift
      ;;
    --force)
      FORCE=1
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

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SUBMODULE_DIR="$REPO_ROOT/vendors/micropython"
PATCH_DIR="$REPO_ROOT/patches/micropython/"

if [[ ! -d "$SUBMODULE_DIR/.git" && ! -f "$SUBMODULE_DIR/.git" ]]; then
  echo "Missing submodule checkout at: $SUBMODULE_DIR" >&2
  exit 1
fi

if [[ ! -d "$PATCH_DIR" ]]; then
  echo "Patch directory not found: $PATCH_DIR" >&2
  exit 1
fi

shopt -s nullglob
PATCHES=("$PATCH_DIR"/*.patch)
shopt -u nullglob

if [[ ${#PATCHES[@]} -eq 0 ]]; then
  echo "No patch files found in: $PATCH_DIR" >&2
  exit 1
fi

if [[ $FORCE -eq 0 ]]; then
  if [[ -n "$(git -C "$SUBMODULE_DIR" status --porcelain)" ]]; then
    echo "vendors/micropython has local changes. Commit/stash first, or re-run with --force." >&2
    exit 1
  fi
fi

patch_subject() {
  local patch="$1"
  sed -nE 's/^Subject: (\[PATCH[^]]*\] )?//p' "$patch" | head -n1
}

patch_state() {
  local patch="$1"
  local subject
  subject="$(patch_subject "$patch")"

  # Most reliable idempotency check: if the patch commit subject is already in history,
  # treat it as applied even if context has shifted due to later patches.
  if [[ -n "$subject" ]] && [[ -n "$(git -C "$SUBMODULE_DIR" log --fixed-strings --grep="$subject" --format=%s -n1)" ]]; then
    echo "ALREADY_APPLIED"
  elif git -C "$SUBMODULE_DIR" apply --check "$patch" >/dev/null 2>&1; then
    echo "APPLICABLE"
  elif git -C "$SUBMODULE_DIR" apply -R --check "$patch" >/dev/null 2>&1; then
    echo "ALREADY_APPLIED"
  else
    echo "CONFLICT"
  fi
}

if [[ $DRY_RUN -eq 1 ]]; then
  echo "Dry-run check of ${#PATCHES[@]} patch(es)..."
  for p in "${PATCHES[@]}"; do
    st="$(patch_state "$p")"
    case "$st" in
      APPLICABLE)
        echo "APPLICABLE: $(basename "$p")"
        ;;
      ALREADY_APPLIED)
        echo "ALREADY APPLIED: $(basename "$p")"
        ;;
      *)
        echo "CONFLICT: $(basename "$p")" >&2
        exit 1
        ;;
    esac
  done
  echo "Dry-run successful."
  exit 0
fi

echo "Applying ${#PATCHES[@]} patch(es) from $PATCH_DIR ..."
APPLY_COUNT=0
SKIP_COUNT=0
for p in "${PATCHES[@]}"; do
  st="$(patch_state "$p")"
  case "$st" in
    APPLICABLE)
      echo "Applying: $(basename "$p")"
      if ! git -C "$SUBMODULE_DIR" am "$p"; then
        echo
        echo "Patch apply failed. To abort in submodule:" >&2
        echo "  git -C vendors/micropython am --abort" >&2
        exit 1
      fi
      APPLY_COUNT=$((APPLY_COUNT + 1))
      ;;
    ALREADY_APPLIED)
      echo "Skipping already applied: $(basename "$p")"
      SKIP_COUNT=$((SKIP_COUNT + 1))
      ;;
    *)
      echo "Cannot apply (conflict): $(basename "$p")" >&2
      exit 1
      ;;
  esac
done

echo "Done. Applied: $APPLY_COUNT, skipped: $SKIP_COUNT"
