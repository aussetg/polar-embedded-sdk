#!/usr/bin/env python3
"""Drive synthetic logger scenarios over USB serial and validate resulting artifacts.

This script is meant for offline firmware validation when a live H10 is not
available. It sends service/debug commands over the logger USB CDC console,
waits for full JSON responses, and can validate:

- closed-session artifacts,
- system-log export output,
- day-boundary and no-session-day behavior,
- queue state transitions,
- optional upload flows against the reference upload server.

The tool intentionally uses only the Python standard library.
"""

from __future__ import annotations

import argparse
import json
import os
import select
import subprocess
import sys
import termios
import time
import tty
from contextlib import ExitStack
from dataclasses import dataclass
from datetime import UTC, datetime, timedelta
from pathlib import Path
from typing import Self
from typing import Any
from urllib.parse import parse_qsl, urlencode, urlparse, urlunparse
from urllib.request import urlopen

try:
    from logger_artifact_validate import resolve_logger_root, validate_session_selection
except ModuleNotFoundError:  # pragma: no cover - import path depends on invocation style
    from scripts.logger_artifact_validate import resolve_logger_root, validate_session_selection

try:
    from logger_system_log_validate import find_events_by_kind, validate_system_log_document
except ModuleNotFoundError:  # pragma: no cover - import path depends on invocation style
    from scripts.logger_system_log_validate import find_events_by_kind, validate_system_log_document


@dataclass(frozen=True)
class Step:
    label: str
    line: str
    expected_command: str
    timeout_s: float = 5.0
    allow_error: bool = False


@dataclass(frozen=True)
class RefServerConfig:
    upload_url: str
    data_dir: Path
    bearer_token: str | None
    min_firmware: str | None
    bind_host: str = "0.0.0.0"


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


class RefServerProcess:
    def __init__(self, config: RefServerConfig) -> None:
        self.config = config
        parsed = urlparse(config.upload_url)
        if parsed.scheme != "http":
            raise ValueError("reference upload server can only be spawned for http:// upload URLs")
        if not parsed.hostname or not parsed.port:
            raise ValueError("upload URL must include an explicit host and port")
        self._parsed_upload_url = parsed
        self._proc: subprocess.Popen[bytes] | None = None
        self._log_handle: Any = None
        self.log_path = config.data_dir / "server.log"

    @property
    def uploads_url(self) -> str:
        return f"http://127.0.0.1:{self._parsed_upload_url.port}/uploads"

    @property
    def healthz_url(self) -> str:
        return f"http://127.0.0.1:{self._parsed_upload_url.port}/healthz"

    def __enter__(self) -> Self:
        self.config.data_dir.mkdir(parents=True, exist_ok=True)
        script_path = Path(__file__).with_name("logger_upload_ref_server.py")
        self._log_handle = self.log_path.open("wb")
        cmd = [
            sys.executable,
            str(script_path),
            "--host",
            self.config.bind_host,
            "--port",
            str(self._parsed_upload_url.port),
            "--path",
            self._parsed_upload_url.path or "/upload",
            "--data-dir",
            str(self.config.data_dir),
        ]
        if self.config.bearer_token is not None:
            cmd.extend(["--bearer-token", self.config.bearer_token])
        if self.config.min_firmware is not None:
            cmd.extend(["--min-firmware", self.config.min_firmware])

        self._proc = subprocess.Popen(cmd, stdout=self._log_handle, stderr=subprocess.STDOUT)
        self._wait_until_healthy()
        return self

    def __exit__(self, exc_type: object, exc: object, tb: object) -> None:
        if self._proc is not None:
            self._proc.terminate()
            try:
                self._proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self._proc.kill()
                self._proc.wait(timeout=5)
        if self._log_handle is not None:
            self._log_handle.close()
        self._proc = None
        self._log_handle = None

    def _wait_until_healthy(self, *, timeout_s: float = 10.0) -> None:
        deadline = time.monotonic() + timeout_s
        last_error: Exception | None = None
        while time.monotonic() < deadline:
            if self._proc is not None and self._proc.poll() is not None:
                raise RuntimeError(
                    "reference upload server exited early; "
                    f"see log at {self.log_path}"
                )
            try:
                with urlopen(self.healthz_url, timeout=1.0) as response:
                    doc = json.load(response)
                if doc.get("ok") is True:
                    return
            except Exception as exc:  # noqa: BLE001
                last_error = exc
            time.sleep(0.1)
        raise RuntimeError(
            "timed out waiting for reference upload server healthz; "
            f"last_error={last_error!r}; log={self.log_path}"
        )


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


def payload_from_response(response: dict[str, Any]) -> dict[str, Any]:
    payload = response.get("payload")
    return payload if isinstance(payload, dict) else {}


def queue_entries_from_response(response: dict[str, Any]) -> list[dict[str, Any]]:
    payload = payload_from_response(response)
    sessions = payload.get("sessions")
    return [entry for entry in sessions if isinstance(entry, dict)] if isinstance(sessions, list) else []


def queue_entry_map_from_response(response: dict[str, Any]) -> dict[str, dict[str, Any]]:
    return {
        entry["session_id"]: entry
        for entry in queue_entries_from_response(response)
        if isinstance(entry.get("session_id"), str)
    }


def eligible_queue_session_ids(response: dict[str, Any]) -> list[str]:
    return [
        entry["session_id"]
        for entry in queue_entries_from_response(response)
        if entry.get("status") in {"pending", "failed"} and isinstance(entry.get("session_id"), str)
    ]


def append_or_replace_query(url: str, key: str, value: str) -> str:
    parsed = urlparse(url)
    query_pairs = [(k, v) for k, v in parse_qsl(parsed.query, keep_blank_values=True) if k != key]
    query_pairs.append((key, value))
    return urlunparse(parsed._replace(query=urlencode(query_pairs)))


def prepare_upload_url(base_url: str, scenario: str, ref_server_min_firmware: str | None) -> str:
    parsed = urlparse(base_url)
    query = dict(parse_qsl(parsed.query, keep_blank_values=True))
    if scenario == "upload-failed" and "force" not in query:
        return append_or_replace_query(base_url, "force", "validation_failed")
    if scenario == "upload-blocked-min-firmware" and "force" not in query and ref_server_min_firmware is None:
        return append_or_replace_query(base_url, "force", "minimum_firmware")
    return base_url


def http_get_json(url: str) -> dict[str, Any]:
    with urlopen(url, timeout=5.0) as response:
        payload = json.load(response)
    if not isinstance(payload, dict):
        raise RuntimeError(f"expected JSON object from {url}")
    return payload


def response_by_label(responses: list[dict[str, Any]], label: str) -> dict[str, Any]:
    for item in responses:
        if item.get("label") == label:
            response = item.get("response")
            if isinstance(response, dict):
                return response
    raise KeyError(label)


def validate_status_service_mode(response: dict[str, Any]) -> None:
    payload = payload_from_response(response)
    if payload.get("mode") != "service":
        raise RuntimeError("device must already be in service mode before running synthetic scenarios")


def extract_new_session_ids(queue_before: dict[str, Any], queue_after: dict[str, Any]) -> list[str]:
    return sorted(session_ids_from_queue_response(queue_after) - session_ids_from_queue_response(queue_before))


def assert_no_unexpected_error(response: dict[str, Any], step: Step) -> None:
    if not response.get("ok") and not step.allow_error:
        require_ok(response, step)


def run_steps(client: SerialJsonClient, steps: list[Step]) -> tuple[str, list[dict[str, Any]], list[dict[str, Any]]]:
    drained = client.drain_input()
    responses: list[dict[str, Any]] = []
    stray_responses: list[dict[str, Any]] = []
    for index, step in enumerate(steps):
        response, stray = client.send_command(step)
        if index == 0:
            validate_status_service_mode(response)
        assert_no_unexpected_error(response, step)
        responses.append(
            {
                "label": step.label,
                "line": step.line,
                "expected_command": step.expected_command,
                "response": response,
            }
        )
        stray_responses.extend(stray)
    return drained, responses, stray_responses


def expected_upload_error_code(scenario: str) -> str | None:
    if scenario == "upload-failed":
        return "server_validation_failed"
    if scenario == "upload-blocked-min-firmware":
        return "min_firmware_rejected"
    return None


def default_wifi_args_required(scenario: str) -> bool:
    return scenario in {"upload-verified", "upload-failed", "upload-blocked-min-firmware"}


def build_scenario(
    name: str,
    start_utc: datetime,
    *,
    upload_url: str | None = None,
    wifi_ssid: str | None = None,
    wifi_psk: str | None = None,
    upload_token: str | None = None,
) -> list[Step]:
    if name == "smoke":
        return [
            Step("status_before", "status --json", "status --json"),
            Step("queue_before", "queue --json", "queue --json"),
            Step("unlock", "service unlock", "service unlock"),
            Step("clock_set", f"clock set {format_rfc3339_utc(start_utc)}", "clock set"),
            Step("session_start", "debug session start", "debug session start"),
            Step("ecg_1", "debug synth ecg 8", "debug synth ecg"),
            Step("h10_battery", "debug synth h10-battery 87 connect", "debug synth h10-battery"),
            Step("disconnect", "debug synth disconnect", "debug synth disconnect"),
            Step("ecg_2", "debug synth ecg 4", "debug synth ecg"),
            Step(
                "clock_jump",
                f"debug synth clock-jump {format_rfc3339_utc(start_utc + timedelta(minutes=10))}",
                "debug synth clock-jump",
            ),
            Step("session_stop", "debug session stop", "debug session stop"),
            Step("queue_after", "queue --json", "queue --json"),
            Step("system_log", "system-log export --json", "system-log export"),
            Step("status_after", "status --json", "status --json"),
        ]
    if name == "clock-fix":
        return [
            Step("status_before", "status --json", "status --json"),
            Step("queue_before", "queue --json", "queue --json"),
            Step("unlock", "service unlock", "service unlock"),
            Step("clock_set", f"clock set {format_rfc3339_utc(start_utc)}", "clock set"),
            Step("session_start", "debug session start", "debug session start"),
            Step("ecg_1", "debug synth ecg 3", "debug synth ecg"),
            Step(
                "clock_fix",
                f"debug synth clock-fix {format_rfc3339_utc(start_utc + timedelta(minutes=2))}",
                "debug synth clock-fix",
            ),
            Step("ecg_2", "debug synth ecg 2", "debug synth ecg"),
            Step("session_stop", "debug session stop", "debug session stop"),
            Step("queue_after", "queue --json", "queue --json"),
            Step("system_log", "system-log export --json", "system-log export"),
            Step("status_after", "status --json", "status --json"),
        ]
    if name == "rollover":
        return [
            Step("status_before", "status --json", "status --json"),
            Step("queue_before", "queue --json", "queue --json"),
            Step("unlock", "service unlock", "service unlock"),
            Step("clock_set", f"clock set {format_rfc3339_utc(start_utc)}", "clock set"),
            Step("session_start", "debug session start", "debug session start"),
            Step("ecg_1", "debug synth ecg 3", "debug synth ecg"),
            Step(
                "rollover",
                f"debug synth rollover {format_rfc3339_utc(start_utc + timedelta(seconds=20))}",
                "debug synth rollover",
            ),
            Step("ecg_2", "debug synth ecg 2", "debug synth ecg"),
            Step("session_stop", "debug session stop", "debug session stop"),
            Step("queue_after", "queue --json", "queue --json"),
            Step("system_log", "system-log export --json", "system-log export"),
            Step("status_after", "status --json", "status --json"),
        ]
    if name == "no-session-day":
        return [
            Step("status_before", "status --json", "status --json"),
            Step("queue_before", "queue --json", "queue --json"),
            Step("unlock", "service unlock", "service unlock"),
            Step("clock_set", f"clock set {format_rfc3339_utc(start_utc)}", "clock set"),
            Step("no_session_day", "debug synth no-session-day auto 1 1 0", "debug synth no-session-day"),
            Step("status_after", "status --json", "status --json"),
            Step("queue_after", "queue --json", "queue --json"),
            Step("system_log", "system-log export --json", "system-log export"),
        ]
    if name in {"upload-verified", "upload-failed", "upload-blocked-min-firmware"}:
        if upload_url is None or wifi_ssid is None or wifi_psk is None:
            raise ValueError(f"scenario {name!r} requires upload_url, wifi_ssid, and wifi_psk")

        steps = [
            Step("status_before", "status --json", "status --json"),
            Step("queue_before", "queue --json", "queue --json"),
            Step("unlock_1", "service unlock", "service unlock"),
            Step("clock_set", f"clock set {format_rfc3339_utc(start_utc)}", "clock set"),
            Step("session_start", "debug session start", "debug session start"),
            Step("ecg_1", "debug synth ecg 4", "debug synth ecg"),
            Step("session_stop", "debug session stop", "debug session stop"),
            Step("queue_mid", "queue --json", "queue --json"),
            Step("unlock_2", "service unlock", "service unlock"),
            Step("set_wifi_ssid", f"debug config set wifi_ssid {wifi_ssid}", "debug config set"),
            Step("set_wifi_psk", f"debug config set wifi_psk {wifi_psk}", "debug config set"),
            Step("set_upload_url", f"debug config set upload_url {upload_url}", "debug config set"),
        ]
        if upload_token is not None:
            steps.append(Step("set_upload_token", f"debug config set upload_token {upload_token}", "debug config set"))
        steps.extend(
            [
                Step("unlock_3", "service unlock", "service unlock"),
                Step(
                    "upload_once",
                    "debug upload once",
                    "debug upload once",
                    timeout_s=90.0,
                    allow_error=name != "upload-verified",
                ),
                Step("queue_after", "queue --json", "queue --json"),
                Step("system_log", "system-log export --json", "system-log export"),
                Step("status_after", "status --json", "status --json"),
            ]
        )
        return steps
    raise ValueError(f"unsupported scenario {name!r}")


def default_start_time(scenario: str) -> datetime:
    if scenario == "rollover":
        return parse_rfc3339_utc("2026-04-02T03:59:50Z")
    if scenario == "no-session-day":
        return parse_rfc3339_utc("2026-04-02T21:00:00Z")
    if scenario in {"upload-verified", "upload-failed", "upload-blocked-min-firmware"}:
        return parse_rfc3339_utc("2026-04-02T14:00:00Z")
    return parse_rfc3339_utc("2026-04-02T12:00:00Z")


def validate_system_log_response(response: dict[str, Any]) -> dict[str, Any]:
    summary = validate_system_log_document(response, path="system-log export response")
    if not summary.get("ok"):
        raise RuntimeError(f"system-log validation failed: {summary['errors']}")
    return summary


def validate_session_artifacts_for_scenario(
    scenario: str,
    artifact_validation: dict[str, Any] | None,
    new_session_ids: list[str],
) -> None:
    if artifact_validation is None:
        return
    reports = artifact_validation.get("session_reports")
    if not isinstance(reports, list):
        raise RuntimeError("artifact validation did not return session_reports")
    report_map = {report.get("session_id"): report for report in reports if isinstance(report, dict)}

    if scenario == "smoke":
        report = report_map.get(new_session_ids[0])
        if report is None:
            raise RuntimeError("artifact validation is missing the smoke session report")
        if report.get("span_count") != 3:
            raise RuntimeError("smoke scenario expected span_count=3")
        if report.get("quarantined") is not True:
            raise RuntimeError("smoke scenario expected quarantined=true")
        reasons = report.get("quarantine_reasons") if isinstance(report.get("quarantine_reasons"), list) else []
        if "clock_jump" not in reasons:
            raise RuntimeError("smoke scenario expected quarantine_reasons to include clock_jump")

    if scenario == "clock-fix":
        report = report_map.get(new_session_ids[0])
        if report is None:
            raise RuntimeError("artifact validation is missing the clock-fix session report")
        if report.get("span_count") != 2:
            raise RuntimeError("clock-fix scenario expected span_count=2")
        if report.get("quarantined") is not True:
            raise RuntimeError("clock-fix scenario expected quarantined=true")
        reasons = report.get("quarantine_reasons") if isinstance(report.get("quarantine_reasons"), list) else []
        if "clock_fixed_mid_session" not in reasons:
            raise RuntimeError("clock-fix scenario expected quarantine_reasons to include clock_fixed_mid_session")

    if scenario == "rollover":
        if len(new_session_ids) != 2:
            raise RuntimeError("rollover scenario expected exactly two new sessions")
        end_reasons = {
            report_map[session_id].get("session_end_reason")
            for session_id in new_session_ids
            if session_id in report_map
        }
        if "rollover" not in end_reasons or "manual_stop" not in end_reasons:
            raise RuntimeError("rollover scenario expected one rollover-closed session and one manual_stop session")


def analyze_non_upload_scenario(
    scenario: str,
    responses: list[dict[str, Any]],
    *,
    artifact_validation: dict[str, Any] | None,
) -> dict[str, Any]:
    queue_before = response_by_label(responses, "queue_before")
    queue_after = response_by_label(responses, "queue_after")
    new_session_ids = extract_new_session_ids(queue_before, queue_after)
    system_log_summary = validate_system_log_response(response_by_label(responses, "system_log"))

    started_session_id = None
    if scenario in {"smoke", "clock-fix", "rollover"}:
        start_payload = payload_from_response(response_by_label(responses, "session_start"))
        if isinstance(start_payload.get("session_id"), str):
            started_session_id = start_payload["session_id"]

    if scenario == "smoke" and len(new_session_ids) != 1:
        raise RuntimeError("smoke scenario expected exactly one new closed session")
    if scenario == "clock-fix" and len(new_session_ids) != 1:
        raise RuntimeError("clock-fix scenario expected exactly one new closed session")
    if scenario == "rollover" and len(new_session_ids) != 2:
        raise RuntimeError("rollover scenario expected exactly two new closed sessions")
    if scenario == "no-session-day" and new_session_ids:
        raise RuntimeError("no-session-day scenario must not create new closed sessions")

    if started_session_id is not None and started_session_id not in session_ids_from_queue_response(queue_after):
        raise RuntimeError("started session_id did not appear in final queue --json output")

    if scenario == "no-session-day":
        status_after = payload_from_response(response_by_label(responses, "status_after"))
        last_day_outcome = status_after.get("last_day_outcome")
        if not isinstance(last_day_outcome, dict):
            raise RuntimeError("status --json missing last_day_outcome")
        if last_day_outcome.get("kind") != "no_session":
            raise RuntimeError("no-session-day scenario expected last_day_outcome.kind=no_session")
        if last_day_outcome.get("reason") != "no_ecg_stream":
            raise RuntimeError("no-session-day scenario expected last_day_outcome.reason=no_ecg_stream")
        events = find_events_by_kind(response_by_label(responses, "system_log"), "no_session_day_summary")
        if not events:
            raise RuntimeError("no-session-day scenario expected a no_session_day_summary system-log event")
        details = events[-1].get("details") if isinstance(events[-1], dict) else None
        if not isinstance(details, dict) or details.get("reason") != "no_ecg_stream":
            raise RuntimeError("no-session-day system-log event does not match expected reason")

    validate_session_artifacts_for_scenario(scenario, artifact_validation, new_session_ids)

    return {
        "started_session_id": started_session_id,
        "queue_before_session_ids": sorted(session_ids_from_queue_response(queue_before)),
        "queue_after_session_ids": sorted(session_ids_from_queue_response(queue_after)),
        "new_session_ids": new_session_ids,
        "system_log_validation": system_log_summary,
    }


def validate_upload_outcome(
    scenario: str,
    responses: list[dict[str, Any]],
    *,
    ref_server_uploads_url: str | None,
) -> dict[str, Any]:
    queue_before = response_by_label(responses, "queue_before")
    queue_mid = response_by_label(responses, "queue_mid")
    queue_after = response_by_label(responses, "queue_after")
    eligible_before = eligible_queue_session_ids(queue_before)
    if eligible_before:
        raise RuntimeError(
            "upload scenarios require a clean eligible queue before starting; "
            f"found pending/failed sessions: {eligible_before}"
        )

    new_session_ids = extract_new_session_ids(queue_before, queue_mid)
    if len(new_session_ids) != 1:
        raise RuntimeError("upload scenario expected exactly one new closed session before upload")
    session_id = new_session_ids[0]

    upload_response = response_by_label(responses, "upload_once")
    upload_entry = queue_entry_map_from_response(queue_after).get(session_id)
    if upload_entry is None:
        raise RuntimeError("uploaded session is missing from final queue --json output")

    expected_status = {
        "upload-verified": "verified",
        "upload-failed": "failed",
        "upload-blocked-min-firmware": "blocked_min_firmware",
    }[scenario]
    if upload_entry.get("status") != expected_status:
        raise RuntimeError(
            f"upload scenario expected final queue status {expected_status!r}, "
            f"got {upload_entry.get('status')!r}"
        )
    if upload_entry.get("attempt_count") != 1:
        raise RuntimeError("upload scenario expected attempt_count=1 for the new session")

    expected_error = expected_upload_error_code(scenario)
    if scenario == "upload-verified":
        require_ok(upload_response, Step("upload_once", "debug upload once", "debug upload once"))
        payload = payload_from_response(upload_response)
        if payload.get("session_id") != session_id:
            raise RuntimeError("debug upload once verified the wrong session_id")
        if payload.get("final_status") != "verified":
            raise RuntimeError("debug upload once did not report final_status=verified")
        if not isinstance(payload.get("receipt_id"), str):
            raise RuntimeError("verified upload response is missing receipt_id")
        if upload_entry.get("receipt_id") != payload.get("receipt_id"):
            raise RuntimeError("queue receipt_id does not match debug upload once response")
        if upload_entry.get("verified_upload_utc") is None:
            raise RuntimeError("verified upload did not populate verified_upload_utc")
    else:
        if upload_response.get("ok") is not False:
            raise RuntimeError("expected debug upload once to return an error envelope")
        error = upload_response.get("error") if isinstance(upload_response.get("error"), dict) else {}
        if expected_error is not None and error.get("code") != expected_error:
            raise RuntimeError(
                f"expected debug upload once error code {expected_error!r}, got {error.get('code')!r}"
            )
        if scenario == "upload-failed" and upload_entry.get("last_failure_class") != "server_validation_failed":
            raise RuntimeError("upload-failed scenario expected last_failure_class=server_validation_failed")
        if scenario == "upload-blocked-min-firmware" and upload_entry.get("last_failure_class") != "min_firmware_rejected":
            raise RuntimeError("upload-blocked-min-firmware expected min_firmware_rejected")

    system_log_summary = validate_system_log_response(response_by_label(responses, "system_log"))
    expected_event_kind = {
        "upload-verified": "upload_verified",
        "upload-failed": "upload_failed",
        "upload-blocked-min-firmware": "upload_blocked_min_firmware",
    }[scenario]
    events = find_events_by_kind(response_by_label(responses, "system_log"), expected_event_kind)
    if not any(
        isinstance(event, dict)
        and isinstance(event.get("details"), dict)
        and event["details"].get("session_id") == session_id
        for event in events
    ):
        raise RuntimeError(
            f"system-log export did not contain {expected_event_kind} for session_id={session_id}"
        )

    ref_server_uploads = None
    if ref_server_uploads_url is not None:
        ref_server_doc = http_get_json(ref_server_uploads_url)
        uploads = ref_server_doc.get("uploads") if isinstance(ref_server_doc.get("uploads"), list) else []
        ref_server_uploads = uploads
        present = any(
            isinstance(upload, dict) and upload.get("session_id") == session_id
            for upload in uploads
        )
        if scenario == "upload-verified" and not present:
            raise RuntimeError("reference server uploads list is missing the verified session")
        if scenario != "upload-verified" and present:
            raise RuntimeError("reference server unexpectedly accepted a non-verified upload scenario")

    return {
        "started_session_id": session_id,
        "queue_before_session_ids": sorted(session_ids_from_queue_response(queue_before)),
        "queue_after_session_ids": sorted(session_ids_from_queue_response(queue_after)),
        "new_session_ids": new_session_ids,
        "system_log_validation": system_log_summary,
        "final_queue_entry": upload_entry,
        "ref_server_uploads": ref_server_uploads,
    }


def run_scenario(
    port: Path,
    scenario: str,
    start_utc: datetime,
    *,
    upload_url: str | None = None,
    wifi_ssid: str | None = None,
    wifi_psk: str | None = None,
    upload_token: str | None = None,
    spawn_ref_server: bool = False,
    ref_server_data_dir: Path | None = None,
    ref_server_min_firmware: str | None = None,
) -> dict[str, Any]:
    effective_upload_url = upload_url
    ref_server_uploads_url = None

    with ExitStack() as stack:
        if spawn_ref_server:
            if upload_url is None:
                raise ValueError("--spawn-ref-server requires --upload-url")
            effective_upload_url = prepare_upload_url(upload_url, scenario, ref_server_min_firmware)
            if ref_server_data_dir is None:
                stamp = datetime.now(UTC).strftime("%Y%m%d_%H%M%S")
                ref_server_data_dir = Path("build") / f"logger_upload_ref_server_runner_{scenario}_{stamp}"
            ref_server = stack.enter_context(
                RefServerProcess(
                    RefServerConfig(
                        upload_url=effective_upload_url,
                        data_dir=ref_server_data_dir,
                        bearer_token=upload_token,
                        min_firmware=ref_server_min_firmware,
                    )
                )
            )
            ref_server_uploads_url = ref_server.uploads_url
        elif upload_url is not None:
            effective_upload_url = prepare_upload_url(upload_url, scenario, ref_server_min_firmware)

        steps = build_scenario(
            scenario,
            start_utc,
            upload_url=effective_upload_url,
            wifi_ssid=wifi_ssid,
            wifi_psk=wifi_psk,
            upload_token=upload_token,
        )

        client = stack.enter_context(SerialJsonClient(port))
        drained, responses, stray_responses = run_steps(client, steps)

    scenario_analysis = (
        validate_upload_outcome(scenario, responses, ref_server_uploads_url=ref_server_uploads_url)
        if scenario in {"upload-verified", "upload-failed", "upload-blocked-min-firmware"}
        else analyze_non_upload_scenario(scenario, responses, artifact_validation=None)
    )

    return {
        "scenario": scenario,
        "port": str(port),
        "start_utc": format_rfc3339_utc(start_utc),
        "effective_upload_url": effective_upload_url,
        "drained_preamble": drained,
        "responses": responses,
        "stray_responses": stray_responses,
        **scenario_analysis,
    }


def print_text(summary: dict[str, Any]) -> None:
    print(f"scenario: {summary['scenario']}")
    print(f"port: {summary['port']}")
    print(f"start_utc: {summary['start_utc']}")
    if summary.get("effective_upload_url") is not None:
        print(f"effective_upload_url: {summary['effective_upload_url']}")
    print(f"new_session_ids: {', '.join(summary['new_session_ids']) or '(none)'}")
    if summary.get("artifact_validation") is not None:
        artifact = summary["artifact_validation"]
        print(f"artifact_validation_ok: {artifact['ok']}")
        if artifact.get("missing_session_ids"):
            print(f"missing_session_ids: {', '.join(artifact['missing_session_ids'])}")
    if summary.get("system_log_validation") is not None:
        print(f"system_log_validation_ok: {summary['system_log_validation']['ok']}")
    if summary.get("final_queue_entry") is not None:
        entry = summary["final_queue_entry"]
        print(
            "final_queue_entry: "
            f"status={entry.get('status')} attempt_count={entry.get('attempt_count')} "
            f"receipt_id={entry.get('receipt_id')}"
        )
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
        choices=[
            "smoke",
            "clock-fix",
            "rollover",
            "no-session-day",
            "upload-verified",
            "upload-failed",
            "upload-blocked-min-firmware",
        ],
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
    parser.add_argument("--wifi-ssid", default=None, help="Wi-Fi SSID for upload scenarios")
    parser.add_argument("--wifi-psk", default=None, help="Wi-Fi PSK for upload scenarios")
    parser.add_argument("--upload-url", default=None, help="HTTP upload URL for upload scenarios")
    parser.add_argument("--upload-token", default=None, help="optional bearer token for upload scenarios")
    parser.add_argument(
        "--spawn-ref-server",
        action="store_true",
        help="spawn scripts/logger_upload_ref_server.py locally for upload scenarios",
    )
    parser.add_argument(
        "--ref-server-data-dir",
        type=Path,
        default=None,
        help="data dir for a spawned reference upload server",
    )
    parser.add_argument(
        "--ref-server-min-firmware",
        default=None,
        help="minimum firmware for a spawned reference server or upload URL preparation",
    )
    parser.add_argument("--json", action="store_true", help="emit machine-readable JSON")
    args = parser.parse_args()

    start_utc = parse_rfc3339_utc(args.start_utc) if args.start_utc is not None else default_start_time(args.scenario)

    if default_wifi_args_required(args.scenario):
        missing_args = [
            name
            for name, value in (("--wifi-ssid", args.wifi_ssid), ("--wifi-psk", args.wifi_psk), ("--upload-url", args.upload_url))
            if value is None
        ]
        if missing_args:
            parser.error(f"scenario {args.scenario!r} requires {' '.join(missing_args)}")

    errors: list[str] = []
    try:
        summary = run_scenario(
            args.port,
            args.scenario,
            start_utc,
            upload_url=args.upload_url,
            wifi_ssid=args.wifi_ssid,
            wifi_psk=args.wifi_psk,
            upload_token=args.upload_token,
            spawn_ref_server=args.spawn_ref_server,
            ref_server_data_dir=args.ref_server_data_dir,
            ref_server_min_firmware=args.ref_server_min_firmware,
        )
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
            else:
                try:
                    validate_session_artifacts_for_scenario(args.scenario, artifact_validation, summary["new_session_ids"])
                except Exception as exc:  # noqa: BLE001
                    errors.append(str(exc))

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