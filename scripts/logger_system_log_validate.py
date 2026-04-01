#!/usr/bin/env python3
"""Validate logger system-log export JSON.

This validates either:

- the raw export object produced by the device, or
- the common host-command envelope whose payload is that export object.

The tool intentionally uses only the Python standard library.
"""

from __future__ import annotations

import argparse
import json
import sys
from dataclasses import dataclass, field
from datetime import datetime
from pathlib import Path
from typing import Any


SEVERITY_VALUES = {"info", "warn", "error"}


def is_rfc3339_utc(value: Any) -> bool:
    if not isinstance(value, str) or not value.endswith("Z"):
        return False
    try:
        datetime.fromisoformat(value[:-1] + "+00:00")
    except ValueError:
        return False
    return True


def is_utc_or_null(value: Any) -> bool:
    return value is None or is_rfc3339_utc(value)


def normalize_system_log_document(doc: Any) -> dict[str, Any]:
    if not isinstance(doc, dict):
        raise ValueError("system-log document must be a JSON object")
    if doc.get("command") == "system-log export" and isinstance(doc.get("payload"), dict):
        payload = doc["payload"]
        if doc.get("schema_version") != 1:
            raise ValueError("command envelope schema_version must be 1")
        if doc.get("ok") is not True:
            raise ValueError("command envelope ok must be true")
        return payload
    return doc


@dataclass
class Report:
    path: str
    errors: list[str] = field(default_factory=list)
    warnings: list[str] = field(default_factory=list)
    event_counts: dict[str, int] = field(default_factory=dict)
    event_count: int = 0
    exported_at_utc: str | None = None

    def error(self, message: str) -> None:
        self.errors.append(message)

    def warn(self, message: str) -> None:
        self.warnings.append(message)

    def count(self, kind: str) -> None:
        self.event_counts[kind] = self.event_counts.get(kind, 0) + 1

    def to_dict(self) -> dict[str, Any]:
        return {
            "path": self.path,
            "exported_at_utc": self.exported_at_utc,
            "event_count": self.event_count,
            "event_counts": dict(sorted(self.event_counts.items())),
            "errors": self.errors,
            "warnings": self.warnings,
            "ok": not self.errors,
        }


def validate_system_log_document(doc: Any, *, path: str = "<memory>") -> dict[str, Any]:
    payload = normalize_system_log_document(doc)
    report = Report(path=path)

    if not isinstance(payload, dict):
        raise ValueError("system-log payload must be a JSON object")
    if payload.get("schema_version") != 1:
        report.error("system-log schema_version must be 1")
    if not is_utc_or_null(payload.get("exported_at_utc")):
        report.error("system-log exported_at_utc must be RFC3339 UTC or null")
    report.exported_at_utc = payload.get("exported_at_utc")

    events = payload.get("events")
    if not isinstance(events, list):
        report.error("system-log events must be an array")
        return report.to_dict()

    last_event_seq: int | None = None
    for index, event_value in enumerate(events):
        if not isinstance(event_value, dict):
            report.error(f"events[{index}] must be an object")
            continue
        for key in ("event_seq", "utc", "boot_counter", "kind", "severity", "details"):
            if key not in event_value:
                report.error(f"events[{index}] missing required field {key}")

        event_seq = event_value.get("event_seq")
        if not isinstance(event_seq, int) or event_seq < 1:
            report.error(f"events[{index}].event_seq must be a positive integer")
        elif last_event_seq is not None and event_seq <= last_event_seq:
            report.error("events must be in strictly ascending event_seq order")
        else:
            last_event_seq = event_seq

        if not is_utc_or_null(event_value.get("utc")):
            report.error(f"events[{index}].utc must be RFC3339 UTC or null")

        boot_counter = event_value.get("boot_counter")
        if not isinstance(boot_counter, int) or boot_counter < 1:
            report.error(f"events[{index}].boot_counter must be a positive integer")

        kind = event_value.get("kind")
        if not isinstance(kind, str) or not kind:
            report.error(f"events[{index}].kind must be a non-empty string")
            continue
        report.count(kind)

        severity = event_value.get("severity")
        if severity not in SEVERITY_VALUES:
            report.error(f"events[{index}].severity must be one of {sorted(SEVERITY_VALUES)!r}")

        details = event_value.get("details")
        if not isinstance(details, dict):
            report.error(f"events[{index}].details must be a JSON object")
            continue

        if kind == "no_session_day_summary":
            for key in ("study_day_local", "reason", "seen_bound_device", "ble_connected", "ecg_start_attempted"):
                if key not in details:
                    report.error(f"events[{index}] no_session_day_summary missing details.{key}")
            if not isinstance(details.get("study_day_local"), str):
                report.error(f"events[{index}] no_session_day_summary details.study_day_local must be a string")
            if not isinstance(details.get("reason"), str):
                report.error(f"events[{index}] no_session_day_summary details.reason must be a string")
            for key in ("seen_bound_device", "ble_connected", "ecg_start_attempted"):
                if not isinstance(details.get(key), bool):
                    report.error(f"events[{index}] no_session_day_summary details.{key} must be boolean")

        if kind in {"upload_failed", "upload_verified", "upload_blocked_min_firmware"}:
            if not isinstance(details.get("session_id"), str):
                report.error(f"events[{index}] {kind} details.session_id must be a string")

    report.event_count = len(events)
    return report.to_dict()


def find_events_by_kind(doc: Any, kind: str) -> list[dict[str, Any]]:
    payload = normalize_system_log_document(doc)
    events = payload.get("events")
    if not isinstance(events, list):
        return []
    return [event for event in events if isinstance(event, dict) and event.get("kind") == kind]


def load_input(path: str) -> Any:
    if path == "-":
        return json.load(sys.stdin)
    return json.loads(Path(path).read_text(encoding="utf-8"))


def print_text(summary: dict[str, Any]) -> None:
    print(f"path: {summary['path']}")
    print(f"exported_at_utc: {summary['exported_at_utc']}")
    print(f"event_count: {summary['event_count']}")
    print("event_counts:")
    for kind, count in summary["event_counts"].items():
        print(f"  {kind}: {count}")
    if summary["warnings"]:
        print("warnings:")
        for warning in summary["warnings"]:
            print(f"  - {warning}")
    if summary["errors"]:
        print("errors:")
        for error in summary["errors"]:
            print(f"  - {error}")
    else:
        print("errors: none")
    print(f"ok: {summary['ok']}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("path", help="JSON file path or - for stdin")
    parser.add_argument("--json", action="store_true", help="emit machine-readable JSON summary")
    args = parser.parse_args()

    try:
        doc = load_input(args.path)
        summary = validate_system_log_document(doc, path=args.path)
    except (OSError, json.JSONDecodeError, ValueError) as exc:
        print(str(exc), file=sys.stderr)
        return 2

    if args.json:
        json.dump(summary, sys.stdout, indent=2, sort_keys=True)
        sys.stdout.write("\n")
    else:
        print_text(summary)
    return 0 if summary["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())