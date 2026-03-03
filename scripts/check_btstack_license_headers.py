#!/usr/bin/env python3
"""Check BTstack license headers for source files that include btstack.h.

Rule enforced:
- Any .c/.h file that includes "btstack.h" must start with:
  1) // SPDX-License-Identifier: LicenseRef-BTstack
  2) // See NOTICE for license details (non-commercial, RP2 exception available)

Exit codes:
- 0: OK
- 1: violations found
"""

from __future__ import annotations

from pathlib import Path
import sys


EXPECTED_LINE_1 = "// SPDX-License-Identifier: LicenseRef-BTstack"
EXPECTED_LINE_2 = "// See NOTICE for license details (non-commercial, RP2 exception available)"


def iter_source_files(root: Path):
    excluded_roots = {
        ".git",
        "build",
        "vendors",
        ".agent",
        ".cache",
        ".ruff_cache",
        ".idea",
    }
    for path in root.rglob("*"):
        if not path.is_file():
            continue
        if path.suffix not in {".c", ".h"}:
            continue
        rel = path.relative_to(root)
        if any(part in excluded_roots for part in rel.parts):
            continue
        yield path


def main() -> int:
    repo_root = Path(__file__).resolve().parent.parent
    violations: list[str] = []
    checked = 0

    for path in iter_source_files(repo_root):
        text = path.read_text(encoding="utf-8", errors="ignore")
        if '#include "btstack.h"' not in text:
            continue

        checked += 1
        lines = text.splitlines()
        rel = path.relative_to(repo_root)

        if len(lines) < 2:
            violations.append(f"{rel}: file too short to contain required BTstack license header")
            continue

        if lines[0].strip() != EXPECTED_LINE_1:
            violations.append(
                f"{rel}: first line must be '{EXPECTED_LINE_1}'"
            )
        if lines[1].strip() != EXPECTED_LINE_2:
            violations.append(
                f"{rel}: second line must be '{EXPECTED_LINE_2}'"
            )

    if violations:
        print("BTstack license header check FAILED:")
        for v in violations:
            print(f"- {v}")
        return 1

    print(f"OK: checked {checked} source file(s) including btstack.h")
    return 0


if __name__ == "__main__":
    sys.exit(main())
