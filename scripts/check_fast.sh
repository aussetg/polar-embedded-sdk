#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${REPO_ROOT}"

echo "[1/3] Python lint (Ruff via uvx)"
./scripts/check_python.sh

echo "[2/3] BTstack license header check"
python3 scripts/check_btstack_license_headers.py

echo "[3/3] Docs link lint"
python3 .pi/skills/lint-docs/scripts/check_docs_links.py

echo "OK: fast checks passed"
