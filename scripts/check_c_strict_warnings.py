#!/usr/bin/env python3
"""Run strict warning checks for selected C translation units from compile_commands.json.

For each matching compilation entry, this tool re-runs the compiler in syntax-only mode
with extra warning flags (default: -Wall -Wextra -Werror).
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys


def to_command(entry: dict) -> list[str]:
    if "arguments" in entry and entry["arguments"]:
        return list(entry["arguments"])
    if "command" in entry and entry["command"]:
        return shlex.split(entry["command"])
    raise ValueError("compile command entry has neither 'arguments' nor 'command'")


def make_syntax_only_command(command: list[str], extra_flags: list[str]) -> list[str]:
    out: list[str] = []
    i = 0
    while i < len(command):
        arg = command[i]
        if arg == "-c":
            i += 1
            continue
        if arg == "-o" and i + 1 < len(command):
            i += 2
            continue
        out.append(arg)
        i += 1

    out.append("-fsyntax-only")
    out.extend(extra_flags)
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--compile-commands", required=True, help="Path to compile_commands.json")
    ap.add_argument("--repo-root", required=True, help="Repository root path")
    ap.add_argument(
        "--path-prefix",
        default="polar_sdk/core/src/",
        help="Relative path prefix to select files from compile_commands.json",
    )
    ap.add_argument(
        "--extra-flags",
        default="-Wall -Wextra -Werror",
        help="Extra compiler flags appended to each syntax-only command",
    )
    ap.add_argument("--verbose", action="store_true", help="Print every command before execution")
    args = ap.parse_args()

    repo_root = Path(args.repo_root).resolve()
    compile_commands_path = Path(args.compile_commands).resolve()
    path_prefix = args.path_prefix
    extra_flags = shlex.split(args.extra_flags)

    try:
        entries = json.loads(compile_commands_path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        print(f"compile_commands not found: {compile_commands_path}", file=sys.stderr)
        return 2

    matched = []
    for entry in entries:
        file_path = Path(entry["file"]).resolve()
        try:
            rel = file_path.relative_to(repo_root)
        except ValueError:
            continue
        if str(rel).startswith(path_prefix):
            matched.append(entry)

    if not matched:
        print(f"No compile commands matched prefix '{path_prefix}'", file=sys.stderr)
        return 2

    failures = 0
    print(f"strict-warnings: checking {len(matched)} translation unit(s)")
    for idx, entry in enumerate(matched, start=1):
        file_path = Path(entry["file"]).resolve()
        work_dir = Path(entry["directory"]).resolve()
        rel = file_path.relative_to(repo_root)

        base_command = to_command(entry)
        command = make_syntax_only_command(base_command, extra_flags)

        if args.verbose:
            cmd_str = " ".join(shlex.quote(arg) for arg in command)
            print(f"[{idx}/{len(matched)}] {rel}\n  {cmd_str}")
        else:
            print(f"[{idx}/{len(matched)}] {rel}")

        result = subprocess.run(
            command,
            cwd=work_dir,
            env=os.environ.copy(),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        if result.returncode != 0:
            failures += 1
            print(f"ERROR: strict warning check failed for {rel}", file=sys.stderr)
            if result.stderr:
                print(result.stderr, file=sys.stderr, end="" if result.stderr.endswith("\n") else "\n")
            if result.stdout:
                print(result.stdout, file=sys.stderr, end="" if result.stdout.endswith("\n") else "\n")

    if failures:
        print(f"strict-warnings: FAILED ({failures} file(s) with diagnostics)", file=sys.stderr)
        return 1

    print("strict-warnings: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
