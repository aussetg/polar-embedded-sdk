#!/usr/bin/env python3
"""Drive synthetic logger scenarios over USB serial and validate resulting artifacts.

This script is meant for offline firmware validation when a live H10 is not
available. It sends service/debug commands over the logger USB CDC console,
waits for full JSON responses, and can optionally validate the resulting SD
artifacts using ``scripts/logger_artifact_validate.py``.

The tool intentionally uses only the Python standard library.
"""

from __future__ import annotations

import argparse
import json
import os
import select
import sys
import termios
import time
import tty
from dataclasses import dataclass
from datetime import UTC, datetime, timedelta
from pathlib import Path
from typing import Self
from typing import Any

try:
    from logger_artifact_validate import resolve_logger_root, validate_session_selection
except ModuleNotFoundError:  # pragma: no cover - import path depends on invocation style
    from scripts.logger_artifact_validate import resolve_logger_root, validate_session_selection


@dataclass(frozen=True)
class Step:
    line: str
    expected_command: str
    timeout_s: float = 5.0


def format_rfc3339_utc(value: datetime) -> str:
    return value.astimezone(UTC).replace(microsecond=0).isoformat().replace("+00:00", "Z")


def parse_rfc3339_utc(value: str) -> datetime:
    return datetime.fromisoformat(value.replace("Z", "+00:00")).astimezone(UTC)


class SerialJsonClient:
    def __init__(self, port: Path) -> None:
        self.port = port
        self.fd: int | None = None
        self._saved_termios: list[Any] | None = None

    def __enter__(self) -> Self:
        fd = os.open(self.port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        self.fd = fd
        self._saved_termios = termios.tcgetattr(fd)
        tty.setraw(fd, when=termios.TCSANOW)
        attrs = termios.tcgetattr(fd)
        attrs[4] = termios.B115200
        attrs[5] = termios.B115200
        termios.tcsetattr(fd, termios.TCSANOW, attrs)
        return self

    def __exit__(self, exc_type: object, exc: object, tb: object) -> None:
        if self.fd is not None and self._saved_termios is not None:
            termios.tcsetattr(self.fd, termios.TCSANOW, self._saved_termios)
        if self.fd is not None:
            os.close(self.fd)
        self.fd = None
        self._saved_termios = None

    def drain_input(self, *, quiet_time_s: float = 0.25, max_time_s: float = 1.0) -> str:
        assert self.fd is not None
        deadline = time.monotonic() + max_time_s
        quiet_deadline = time.monotonic() + quiet_time_s
        chunks: list[bytes] = []
        while time.monotonic() < deadline:
            timeout = max(0.0, quiet_deadline - time.monotonic())
            ready, _, _ = select.select([self.fd], [], [], timeout)
            if not ready:
                break
            data = os.read(self.fd, 4096)
            if not data:
                break
            chunks.append(data)
            quiet_deadline = time.monotonic() + quiet_time_s
        return b"".join(chunks).decode("utf-8", errors="replace")

    def send_command(self, step: Step) -> tuple[dict[str, Any], list[dict[str, Any]]]:
        assert self.fd is not None
        os.write(self.fd, (step.line + "\n").encode("utf-8"))
        return self.read_json(step.expected_command, timeout_s=step.timeout_s)

    def read_json(self, expected_command: str, *, timeout_s: float) -> tuple[dict[str, Any], list[dict[str, Any]]]:
        assert self.fd is not None
        deadline = time.monotonic() + timeout_s
        in_object = False
        depth = 0
        in_string = False
        escaping = False
        buf: list[str] = []
        stray: list[dict[str, Any]] = []

        while time.monotonic() < deadline:
            ready, _, _ = select.select([self.fd], [], [], 0.1)
            if not ready:
                continue
            data = os.read(self.fd, 4096)
            if not data:
                continue
            text = data.decode("utf-8", errors="replace")
            for ch in text:
                if not in_object:
                    if ch == "{":
                        in_object = True
                        depth = 1
                        in_string = False
                        escaping = False
                        buf = [ch]
                    continue

                buf.append(ch)
                if in_string:
                    if escaping:
                        escaping = False
                    elif ch == "\\":
                        escaping = True
                    elif ch == '"':
                        in_string = False
                    continue

                if ch == '"':
                    in_string = True
                elif ch == "{":
                    depth += 1
                elif ch == "}":
                    depth -= 1
                    if depth == 0:
                        raw = "".join(buf)
                        in_object = False
                        buf = []
                        try:
                            parsed = json.loads(raw)
                        except json.JSONDecodeError:
                            continue
                        if parsed.get("command") != expected_command:
                            stray.append(parsed)
                            continue
                        return parsed, stray

        raise TimeoutError(f"timed out waiting for JSON response to {expected_command!r}")


def require_ok(response: dict[str, Any], step: Step) -> None:
    if response.get("ok"):
        return
    error = response.get("error") if isinstance(response.get("error"), dict) else {}
    code = error.get("code")
    message = error.get("message")
    raise RuntimeError(f"command {step.line!r} failed with code={code!r} message={message!r}")


def session_ids_from_queue_response(response: dict[str, Any]) -> set[str]:
    payload = response.get("payload") if isinstance(response.get("payload"), dict) else {}
    sessions = payload.get("sessions") if isinstance(payload.get("sessions"), list) else []
    session_ids: set[str] = set()
    for session in sessions:
        if isinstance(session, dict) and isinstance(session.get("session_id"), str):
            session_ids.add(session["session_id"])
    return session_ids


def build_scenario(name: str, start_utc: datetime) -> list[Step]:
    if name == "smoke":
        return [
            Step("status --json", "status --json"),
            Step("queue --json", "queue --json"),
            Step("service unlock", "service unlock"),
            Step(f"clock set {format_rfc3339_utc(start_utc)}", "clock set"),
            Step("debug session start", "debug session start"),
            Step("debug synth ecg 8", "debug synth ecg"),
            Step("debug synth h10-battery 87 connect", "debug synth h10-battery"),
            Step("debug synth disconnect", "debug synth disconnect"),
            Step("debug synth ecg 4", "debug synth ecg"),
            Step(
                f"debug synth clock-jump {format_rfc3339_utc(start_utc + timedelta(minutes=10))}",
                "debug synth clock-jump",
            ),
            Step("debug session stop", "debug session stop"),
            Step("queue --json", "queue --json"),
        ]
    if name == "rollover":
        return [
            Step("status --json", "status --json"),
            Step("queue --json", "queue --json"),
            Step("service unlock", "service unlock"),
            Step(f"clock set {format_rfc3339_utc(start_utc)}", "clock set"),
            Step("debug session start", "debug session start"),
            Step("debug synth ecg 3", "debug synth ecg"),
            Step(
                f"debug synth rollover {format_rfc3339_utc(start_utc + timedelta(seconds=20))}",
                "debug synth rollover",
            ),
            Step("debug synth ecg 2", "debug synth ecg"),
            Step("debug session stop", "debug session stop"),
            Step("queue --json", "queue --json"),
        ]
    raise ValueError(f"unsupported scenario {name!r}")


def default_start_time(scenario: str) -> datetime:
    if scenario == "rollover":
        return parse_rfc3339_utc("2026-04-02T03:59:50Z")
    return parse_rfc3339_utc("2026-04-02T12:00:00Z")


def run_scenario(port: Path, scenario: str, start_utc: datetime) -> dict[str, Any]:
    steps = build_scenario(scenario, start_utc)
    responses: list[dict[str, Any]] = []
    stray_responses: list[dict[str, Any]] = []

    with SerialJsonClient(port) as client:
        drained = client.drain_input()
        for index, step in enumerate(steps):
            response, stray = client.send_command(step)
            require_ok(response, step)
            if index == 0:
                payload = response.get("payload") if isinstance(response.get("payload"), dict) else {}
                if payload.get("mode") != "service":
                    raise RuntimeError("device must already be in service mode before running synthetic scenarios")
            responses.append(
                {
                    "line": step.line,
                    "expected_command": step.expected_command,
                    "response": response,
                }
            )
            stray_responses.extend(stray)
    queue_before = session_ids_from_queue_response(responses[1]["response"])
    queue_after = session_ids_from_queue_response(responses[-1]["response"])
    new_session_ids = sorted(queue_after - queue_before)

    start_payload = responses[4]["response"].get("payload")
    started_session_id = None
    if isinstance(start_payload, dict) and isinstance(start_payload.get("session_id"), str):
        started_session_id = start_payload["session_id"]

    if started_session_id is not None and started_session_id not in queue_after:
        raise RuntimeError("started session_id did not appear in final queue --json output")
    if scenario == "smoke" and len(new_session_ids) < 1:
        raise RuntimeError("smoke scenario did not produce any new closed session")
    if scenario == "rollover" and len(new_session_ids) < 2:
        raise RuntimeError("rollover scenario did not produce two closed sessions")

    return {
        "scenario": scenario,
        "port": str(port),
        "start_utc": format_rfc3339_utc(start_utc),
        "drained_preamble": drained,
        "responses": responses,
        "stray_responses": stray_responses,
        "started_session_id": started_session_id,
        "queue_before_session_ids": sorted(queue_before),
        "queue_after_session_ids": sorted(queue_after),
        "new_session_ids": new_session_ids,
    }


def print_text(summary: dict[str, Any]) -> None:
    print(f"scenario: {summary['scenario']}")
    print(f"port: {summary['port']}")
    print(f"start_utc: {summary['start_utc']}")
    print(f"new_session_ids: {', '.join(summary['new_session_ids']) or '(none)'}")
    if summary.get("artifact_validation") is not None:
        artifact = summary["artifact_validation"]
        print(f"artifact_validation_ok: {artifact['ok']}")
        if artifact.get("missing_session_ids"):
            print(f"missing_session_ids: {', '.join(artifact['missing_session_ids'])}")
    print(f"ok: {summary['ok']}")
    if summary.get("errors"):
        print("errors:")
        for error in summary["errors"]:
            print(f"  - {error}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=Path, required=True, help="USB CDC device, e.g. /dev/ttyACM2")
    parser.add_argument(
        "--scenario",
        choices=["smoke", "rollover"],
        default="smoke",
        help="synthetic scenario to run",
    )
    parser.add_argument(
        "--start-utc",
        default=None,
        help="override scenario start time in RFC3339 UTC form (default depends on scenario)",
    )
    parser.add_argument(
        "--logger-root",
        type=Path,
        default=None,
        help="mounted logger root or mount point containing ./logger for post-run artifact validation",
    )
    parser.add_argument("--json", action="store_true", help="emit machine-readable JSON")
    args = parser.parse_args()

    start_utc = parse_rfc3339_utc(args.start_utc) if args.start_utc is not None else default_start_time(args.scenario)

    errors: list[str] = []
    try:
        summary = run_scenario(args.port, args.scenario, start_utc)
    except Exception as exc:  # noqa: BLE001
        summary = {
            "scenario": args.scenario,
            "port": str(args.port),
            "start_utc": format_rfc3339_utc(start_utc),
            "responses": [],
            "new_session_ids": [],
        }
        errors.append(str(exc))

    artifact_validation = None
    if args.logger_root is not None and not errors:
        resolved_root = resolve_logger_root(args.logger_root)
        if resolved_root is None:
            errors.append("--logger-root is not a logger root and does not contain ./logger")
        else:
            artifact_validation = validate_session_selection(resolved_root, set(summary["new_session_ids"]))
            if not artifact_validation.get("ok"):
                errors.append("artifact validation failed")

    summary["artifact_validation"] = artifact_validation
    summary["errors"] = errors
    summary["ok"] = not errors and (
        artifact_validation is None or artifact_validation.get("ok", False)
    )

    if args.json:
        json.dump(summary, sys.stdout, indent=2, sort_keys=True)
        sys.stdout.write("\n")
    else:
        print_text(summary)
    return 0 if summary["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())