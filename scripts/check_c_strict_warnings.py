#!/usr/bin/env python3
"""Run strict C checks for selected translation units from compile_commands.json.

Supports two execution modes:
- syntax: re-run compiler in syntax-only mode (`-fsyntax-only`)
- compile: re-run compiler in compile mode (`-c`) writing objects to a temp dir

By default this script is used as a strict warning gate for project-owned code.
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile


def to_command(entry: dict) -> list[str]:
    if "arguments" in entry and entry["arguments"]:
        return list(entry["arguments"])
    if "command" in entry and entry["command"]:
        return shlex.split(entry["command"])
    raise ValueError("compile command entry has neither 'arguments' nor 'command'")


def is_vendor_path(path_text: str, vendor_markers: list[str]) -> bool:
    return any(marker in path_text for marker in vendor_markers)


def systemize_vendor_includes(command: list[str], vendor_markers: list[str]) -> list[str]:
    out: list[str] = []
    i = 0
    while i < len(command):
        arg = command[i]

        if arg == "-I" and i + 1 < len(command):
            include_path = command[i + 1]
            if is_vendor_path(include_path, vendor_markers):
                out.extend(["-isystem", include_path])
            else:
                out.extend(["-I", include_path])
            i += 2
            continue

        if arg.startswith("-I") and len(arg) > 2:
            include_path = arg[2:]
            if is_vendor_path(include_path, vendor_markers):
                out.extend(["-isystem", include_path])
            else:
                out.append(arg)
            i += 1
            continue

        out.append(arg)
        i += 1

    return out


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

        if arg.startswith("-o") and len(arg) > 2:
            i += 1
            continue

        out.append(arg)
        i += 1

    out.append("-fsyntax-only")
    out.extend(extra_flags)
    return out


def make_compile_command(command: list[str], extra_flags: list[str], output_path: Path) -> list[str]:
    out: list[str] = []
    i = 0
    saw_c_flag = False
    saw_output_flag = False

    while i < len(command):
        arg = command[i]

        if arg == "-c":
            saw_c_flag = True
            out.append(arg)
            i += 1
            continue

        if arg == "-o" and i + 1 < len(command):
            saw_output_flag = True
            out.extend(["-o", str(output_path)])
            i += 2
            continue

        if arg.startswith("-o") and len(arg) > 2:
            saw_output_flag = True
            out.extend(["-o", str(output_path)])
            i += 1
            continue

        out.append(arg)
        i += 1

    if not saw_c_flag:
        out.append("-c")
    if not saw_output_flag:
        out.extend(["-o", str(output_path)])

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
        "--mode",
        choices=["syntax", "compile"],
        default="syntax",
        help="Execution mode: syntax-only or compile",
    )
    ap.add_argument(
        "--extra-flags",
        default="-Wall -Wextra -Werror",
        help="Extra compiler flags appended to each command",
    )
    ap.add_argument(
        "--systemize-vendor-includes",
        action="store_true",
        help="Rewrite vendor include paths from -I to -isystem",
    )
    ap.add_argument(
        "--vendor-marker",
        action="append",
        default=["/vendors/"],
        help="Substring used to classify include paths as vendor paths (can be repeated)",
    )
    ap.add_argument("--verbose", action="store_true", help="Print every command before execution")
    args = ap.parse_args()

    repo_root = Path(args.repo_root).resolve()
    compile_commands_path = Path(args.compile_commands).resolve()
    path_prefix = args.path_prefix
    mode = args.mode
    extra_flags = shlex.split(args.extra_flags)

    try:
        entries = json.loads(compile_commands_path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        print(f"compile_commands not found: {compile_commands_path}", file=sys.stderr)
        return 2

    matched: list[dict] = []
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
    print(f"strict-c-check: checking {len(matched)} translation unit(s) [mode={mode}]")

    with tempfile.TemporaryDirectory(prefix="polar-strict-c-") as temp_dir:
        temp_dir_path = Path(temp_dir)

        for idx, entry in enumerate(matched, start=1):
            file_path = Path(entry["file"]).resolve()
            work_dir = Path(entry["directory"]).resolve()
            rel = file_path.relative_to(repo_root)

            base_command = to_command(entry)
            if args.systemize_vendor_includes:
                base_command = systemize_vendor_includes(base_command, args.vendor_marker)

            if mode == "compile":
                out_obj = temp_dir_path / f"tu_{idx}.o"
                command = make_compile_command(base_command, extra_flags, out_obj)
            else:
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
                print(f"ERROR: strict C check failed for {rel}", file=sys.stderr)
                if result.stderr:
                    print(result.stderr, file=sys.stderr, end="" if result.stderr.endswith("\n") else "\n")
                if result.stdout:
                    print(result.stdout, file=sys.stderr, end="" if result.stdout.endswith("\n") else "\n")

    if failures:
        print(f"strict-c-check: FAILED ({failures} file(s) with diagnostics)", file=sys.stderr)
        return 1

    print("strict-c-check: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
