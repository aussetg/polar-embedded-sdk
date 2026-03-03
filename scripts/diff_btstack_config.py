#!/usr/bin/env python3
"""Diff relevant #define values between two BTstack config headers.

Default comparison:
- left:  examples/pico_sdk/btstack_config.h
- right: vendors/micropython/ports/rp2/btstack_inc/btstack_config.h

This is a light-weight helper for debugging behavior differences between
probe and MicroPython builds.
"""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
import argparse
import re
import sys


DEFINE_RE = re.compile(r"^\s*#\s*define\s+([A-Za-z_][A-Za-z0-9_]*)(?:\s+(.*))?$")
INCLUDE_QUOTED_RE = re.compile(r'^\s*#\s*include\s+"([^"]+)"\s*$')


@dataclass
class MacroVal:
    value: str
    source: Path


def strip_inline_comment(value: str) -> str:
    if "//" in value:
        value = value.split("//", 1)[0]
    return value.strip()


def parse_header(path: Path, include_dirs: list[Path], seen: set[Path], macros: dict[str, MacroVal]) -> None:
    path = path.resolve()
    if path in seen:
        return
    seen.add(path)

    if not path.exists():
        return

    text = path.read_text(encoding="utf-8", errors="ignore")
    for line in text.splitlines():
        m_inc = INCLUDE_QUOTED_RE.match(line)
        if m_inc:
            inc_name = m_inc.group(1)
            candidates = [path.parent / inc_name] + [d / inc_name for d in include_dirs]
            for c in candidates:
                if c.exists():
                    parse_header(c, include_dirs, seen, macros)
                    break
            continue

        m_def = DEFINE_RE.match(line)
        if not m_def:
            continue

        name = m_def.group(1)
        raw_val = m_def.group(2) or "1"
        val = strip_inline_comment(raw_val)
        macros[name] = MacroVal(val if val else "1", path)


def load_macros(root: Path, start: Path) -> dict[str, MacroVal]:
    include_dirs = [
        start.parent,
        root / "vendors/micropython/extmod/btstack",
        root / "vendors/micropython/ports/rp2/btstack_inc",
        root / "examples/pico_sdk",
        root / "examples/pico_sdk_psftp",
    ]
    macros: dict[str, MacroVal] = {}
    parse_header(start, include_dirs, set(), macros)
    return macros


def fmt_macro(name: str, m: MacroVal, root: Path) -> str:
    try:
        src = m.source.relative_to(root)
    except ValueError:
        src = m.source
    return f"{name} = {m.value}  ({src})"


def main() -> int:
    repo_root = Path(__file__).resolve().parent.parent

    ap = argparse.ArgumentParser()
    ap.add_argument(
        "--left",
        default="examples/pico_sdk/btstack_config.h",
        help="left config header (default: %(default)s)",
    )
    ap.add_argument(
        "--right",
        default="vendors/micropython/ports/rp2/btstack_inc/btstack_config.h",
        help="right config header (default: %(default)s)",
    )
    ap.add_argument(
        "--show-same",
        action="store_true",
        help="also show macros with identical value",
    )
    args = ap.parse_args()

    left_path = (repo_root / args.left).resolve()
    right_path = (repo_root / args.right).resolve()

    if not left_path.exists() or not right_path.exists():
        print("Config path not found:")
        print(f"- left:  {left_path}")
        print(f"- right: {right_path}")
        return 2

    left = load_macros(repo_root, left_path)
    right = load_macros(repo_root, right_path)

    names = sorted(set(left) | set(right))
    only_left: list[str] = []
    only_right: list[str] = []
    changed: list[str] = []
    same: list[str] = []

    for n in names:
        l = left.get(n)
        r = right.get(n)
        if l is None:
            only_right.append(n)
        elif r is None:
            only_left.append(n)
        elif l.value != r.value:
            changed.append(n)
        else:
            same.append(n)

    print("BTstack config diff")
    print(f"left : {left_path.relative_to(repo_root)}")
    print(f"right: {right_path.relative_to(repo_root)}")
    print()
    print(f"summary: changed={len(changed)} only_left={len(only_left)} only_right={len(only_right)} same={len(same)}")

    if changed:
        print("\nChanged values:")
        for n in changed:
            print(f"- {fmt_macro(n, left[n], repo_root)}")
            print(f"  {fmt_macro(n, right[n], repo_root)}")

    if only_left:
        print("\nOnly in left:")
        for n in only_left:
            print(f"- {fmt_macro(n, left[n], repo_root)}")

    if only_right:
        print("\nOnly in right:")
        for n in only_right:
            print(f"- {fmt_macro(n, right[n], repo_root)}")

    if args.show_same and same:
        print("\nSame:")
        for n in same:
            print(f"- {fmt_macro(n, left[n], repo_root)}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
