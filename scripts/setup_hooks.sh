#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if [[ ! -d "${REPO_ROOT}/.git" && ! -f "${REPO_ROOT}/.git" ]]; then
  echo "Not a git working tree: ${REPO_ROOT}" >&2
  exit 1
fi

chmod +x \
  "${REPO_ROOT}/patches/check_btstack_alignment.sh" \
  "${REPO_ROOT}/.githooks/_btstack_alignment_hook.sh" \
  "${REPO_ROOT}/.githooks/pre-commit" \
  "${REPO_ROOT}/.githooks/post-checkout" \
  "${REPO_ROOT}/.githooks/post-merge" \
  "${REPO_ROOT}/.githooks/pre-push"

git -C "${REPO_ROOT}" config core.hooksPath .githooks

echo "Configured git hooks path: .githooks"
echo "Try: ./patches/check_btstack_alignment.sh"
