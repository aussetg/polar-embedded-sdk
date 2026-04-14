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

try:
    from logger_tls import LOGGER_PUBLIC_ROOT_PROFILE
    from logger_tls import LOGGER_TLS_MODE_PROVISIONED_ANCHOR
    from logger_tls import LOGGER_TLS_MODE_PUBLIC_ROOTS
    from logger_tls import TlsProbeResult
    from logger_tls import TlsTrustPlan
    from logger_tls import plan_logger_tls
except ModuleNotFoundError:  # pragma: no cover - import path depends on invocation style
    from scripts.logger_tls import LOGGER_PUBLIC_ROOT_PROFILE
    from scripts.logger_tls import LOGGER_TLS_MODE_PROVISIONED_ANCHOR
    from scripts.logger_tls import LOGGER_TLS_MODE_PUBLIC_ROOTS
    from scripts.logger_tls import TlsProbeResult
    from scripts.logger_tls import TlsTrustPlan
    from scripts.logger_tls import plan_logger_tls


INTERRUPTED_UPLOAD_DELAY_MS = 30_000
INTERRUPTED_UPLOAD_REQUEST_WAIT_S = 20.0
INTERRUPTED_UPLOAD_REBOOT_SETTLE_S = 1.0
POST_REBOOT_READY_TIMEOUT_S = 45.0
CONFIG_IMPORT_CHUNK_SIZE = 1200
REPO_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_FIRMWARE_ELF = REPO_ROOT / "build" / "logger_firmware_rp2_2" / "logger_appliance.elf"
DEFAULT_REQUEUE_TEMP_BUILD_ID = "logger-fw-regression-requeue"
DEFAULT_RESET_COMMAND = [
    "openocd",
    "-f",
    "interface/cmsis-dap.cfg",
    "-f",
    "target/rp2350.cfg",
    "-c",
    "adapter speed 5000; init; reset run; shutdown",
]


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
    api_key: str | None
    bearer_token: str | None
    min_firmware: str | None
    origin_upload_url: str | None = None
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
        self.write_line(step.line)
        return self.read_json(step.expected_command, timeout_s=step.timeout_s)

    def write_line(self, line: str) -> None:
        assert self.fd is not None
        os.write(self.fd, (line + "\n").encode("utf-8"))

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
        parsed = urlparse(config.origin_upload_url or config.upload_url)
        if parsed.scheme != "http":
            raise ValueError("reference upload server requires an http:// origin URL")
        if not parsed.hostname or not parsed.port:
            raise ValueError("reference upload server origin URL must include an explicit host and port")
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
        if self.config.api_key is not None:
            cmd.extend(["--api-key", self.config.api_key])
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


def verified_queue_session_ids(response: dict[str, Any]) -> list[str]:
    return [
        entry["session_id"]
        for entry in queue_entries_from_response(response)
        if entry.get("status") == "verified" and isinstance(entry.get("session_id"), str)
    ]


def system_log_events_from_response(response: dict[str, Any]) -> list[dict[str, Any]]:
    payload = payload_from_response(response)
    events = payload.get("events")
    return [event for event in events if isinstance(event, dict)] if isinstance(events, list) else []


def max_system_log_event_seq(response: dict[str, Any]) -> int:
    max_seq = 0
    for event in system_log_events_from_response(response):
        seq = event.get("event_seq")
        if isinstance(seq, int) and seq > max_seq:
            max_seq = seq
    return max_seq


def find_events_by_kind_after_seq(response: dict[str, Any], kind: str, minimum_event_seq: int) -> list[dict[str, Any]]:
    return [
        event
        for event in find_events_by_kind(response, kind)
        if isinstance(event.get("event_seq"), int) and event["event_seq"] > minimum_event_seq
    ]


def summarize_tls_probe(probe: TlsProbeResult) -> dict[str, Any]:
    return {
        "url": probe.url,
        "host": probe.host,
        "port": probe.port,
        "remote_ip": probe.remote_ip,
        "tls_version": probe.tls_version,
        "cipher_suite": probe.cipher_suite,
        "verification_source": probe.verification_source,
        "chain": [
            {
                "sha256": cert.sha256,
                "subject": cert.subject,
                "issuer": cert.issuer,
                "self_signed": cert.self_signed,
                "not_before": cert.not_before,
                "not_after": cert.not_after,
            }
            for cert in probe.chain
        ],
    }


def summarize_tls_plan(plan: TlsTrustPlan) -> dict[str, Any]:
    return {
        "mode": plan.mode,
        "root_profile": plan.root_profile,
        "reason": plan.reason,
        "selected_anchor_sha256": plan.selected_anchor_sha256,
        "selected_anchor_subject": plan.selected_anchor_subject,
    }


def build_upload_tls_document(*, upload_url: str, trust_plan: TlsTrustPlan | None) -> dict[str, Any] | None:
    parsed = urlparse(upload_url)
    if parsed.scheme != "https":
        return {"mode": None, "root_profile": None, "anchor": None}
    if trust_plan is None:
        raise ValueError("https:// upload requires a trust plan")
    if trust_plan.mode == LOGGER_TLS_MODE_PUBLIC_ROOTS:
        return {
            "mode": LOGGER_TLS_MODE_PUBLIC_ROOTS,
            "root_profile": LOGGER_PUBLIC_ROOT_PROFILE,
            "anchor": None,
        }
    if trust_plan.mode == LOGGER_TLS_MODE_PROVISIONED_ANCHOR:
        if not isinstance(trust_plan.anchor, dict):
            raise ValueError("provisioned-anchor trust plan is missing anchor material")
        anchor = {
            "format": trust_plan.anchor["format"],
            "der_base64": trust_plan.anchor["der_base64"],
            "sha256": trust_plan.anchor["sha256"],
        }
        return {
            "mode": LOGGER_TLS_MODE_PROVISIONED_ANCHOR,
            "root_profile": None,
            "anchor": anchor,
        }
    raise ValueError(f"unsupported trust-plan mode {trust_plan.mode!r}")


def build_config_import_document(
    *,
    exported_at_utc: str,
    hardware_id: str,
    logger_id: str,
    subject_id: str,
    bound_h10_address: str,
    timezone: str,
    wifi_ssid: str,
    wifi_psk: str,
    upload_url: str,
    upload_api_key: str | None,
    upload_token: str | None,
    secrets_included: bool,
    upload_tls: dict[str, Any] | None = None,
) -> dict[str, Any]:
    upload_enabled = True
    if upload_api_key is None or upload_token is None:
        raise ValueError("upload-enabled config import requires both upload_api_key and upload_token")

    upload_auth: dict[str, Any]
    if secrets_included:
        upload_auth = {"type": "api_key_and_bearer", "api_key": upload_api_key, "token": upload_token}
    else:
        upload_auth = {"type": "api_key_and_bearer", "api_key_present": True, "token_present": True}

    wifi_network: dict[str, Any] = {"ssid": wifi_ssid}
    if secrets_included:
        wifi_network["psk"] = wifi_psk
    else:
        wifi_network["psk_present"] = True

    return {
        "schema_version": 1,
        "exported_at_utc": exported_at_utc,
        "hardware_id": hardware_id,
        "secrets_included": secrets_included,
        "identity": {
            "logger_id": logger_id,
            "subject_id": subject_id,
        },
        "recording": {
            "bound_h10_address": bound_h10_address,
            "study_day_rollover_local": "04:00:00",
            "overnight_upload_window_start_local": "22:00:00",
            "overnight_upload_window_end_local": "06:00:00",
        },
        "time": {
            "timezone": timezone,
        },
        "battery_policy": {
            "critical_stop_voltage_v": 3.5,
            "low_start_voltage_v": 3.65,
            "off_charger_upload_voltage_v": 3.85,
        },
        "wifi": {
            "allowed_ssids": [wifi_ssid],
            "networks": [wifi_network],
        },
        "upload": {
            "enabled": upload_enabled,
            "url": upload_url,
            "auth": upload_auth,
            **({"tls": upload_tls} if upload_tls is not None else {}),
        },
    }


def build_config_import_command(document: dict[str, Any]) -> str:
    return "config import " + json.dumps(document, separators=(",", ":"), sort_keys=True)


def build_chunked_config_import_steps(prefix: str, document: dict[str, Any], *, chunk_size: int = CONFIG_IMPORT_CHUNK_SIZE) -> list[Step]:
    compact = json.dumps(document, separators=(",", ":"), sort_keys=True)
    steps = [
        Step(f"{prefix}_unlock", "service unlock", "service unlock"),
        Step(f"{prefix}_begin", f"config import begin {len(compact)}", "config import begin", timeout_s=10.0),
    ]
    for index in range(0, len(compact), chunk_size):
        chunk = compact[index : index + chunk_size]
        steps.append(
            Step(
                f"{prefix}_chunk_{(index // chunk_size) + 1}",
                f"config import chunk {chunk}",
                "config import chunk",
                timeout_s=10.0,
            )
        )
    steps.append(Step(f"{prefix}_status", "config import status", "config import status", timeout_s=10.0))
    steps.append(Step(f"{prefix}_commit", "config import commit", "config import commit", timeout_s=15.0))
    return steps


def validate_config_export_payload(
    response: dict[str, Any],
    *,
    hardware_id: str,
    logger_id: str,
    subject_id: str,
    bound_h10_address: str,
    timezone: str,
    wifi_ssid: str,
    upload_url: str,
    expect_wifi_psk_present: bool,
    expect_upload_api_key_present: bool,
    expect_upload_token_present: bool,
    expected_auth_type: str,
    expected_tls_mode: str | None = None,
    expected_tls_root_profile: str | None = None,
    expected_anchor_sha256: str | None = None,
) -> dict[str, Any]:
    payload = payload_from_response(response)
    if expected_tls_mode is None:
        expected_tls_mode = LOGGER_TLS_MODE_PUBLIC_ROOTS if upload_url.startswith("https://") else None
    if expected_tls_root_profile is None and expected_tls_mode == LOGGER_TLS_MODE_PUBLIC_ROOTS:
        expected_tls_root_profile = LOGGER_PUBLIC_ROOT_PROFILE

    if payload.get("schema_version") != 1:
        raise RuntimeError("config export payload schema_version must be 1")
    if payload.get("hardware_id") != hardware_id:
        raise RuntimeError("config export payload hardware_id mismatch")
    if payload.get("secrets_included") is not False:
        raise RuntimeError("config export payload must omit secrets")

    identity = payload.get("identity") if isinstance(payload.get("identity"), dict) else {}
    if identity.get("logger_id") != logger_id or identity.get("subject_id") != subject_id:
        raise RuntimeError("config export identity payload mismatch")

    recording = payload.get("recording") if isinstance(payload.get("recording"), dict) else {}
    if recording.get("bound_h10_address") != bound_h10_address:
        raise RuntimeError("config export recording.bound_h10_address mismatch")

    time_config = payload.get("time") if isinstance(payload.get("time"), dict) else {}
    if time_config.get("timezone") != timezone:
        raise RuntimeError("config export time.timezone mismatch")

    wifi = payload.get("wifi") if isinstance(payload.get("wifi"), dict) else {}
    allowed_ssids = wifi.get("allowed_ssids") if isinstance(wifi.get("allowed_ssids"), list) else []
    if allowed_ssids != [wifi_ssid]:
        raise RuntimeError(f"config export allowed_ssids mismatch: expected {[wifi_ssid]!r}, got {allowed_ssids!r}")
    networks = wifi.get("networks") if isinstance(wifi.get("networks"), list) else []
    if len(networks) != 1 or not isinstance(networks[0], dict):
        raise RuntimeError("config export wifi.networks must contain exactly one object")
    if networks[0].get("ssid") != wifi_ssid:
        raise RuntimeError("config export wifi.networks[0].ssid mismatch")
    if networks[0].get("psk_present") is not expect_wifi_psk_present:
        raise RuntimeError(
            "config export wifi.networks[0].psk_present mismatch: "
            f"expected {expect_wifi_psk_present!r}, got {networks[0].get('psk_present')!r}"
        )

    upload = payload.get("upload") if isinstance(payload.get("upload"), dict) else {}
    if upload.get("enabled") is not True or upload.get("url") != upload_url:
        raise RuntimeError("config export upload settings mismatch")
    auth = upload.get("auth") if isinstance(upload.get("auth"), dict) else {}
    if auth.get("type") != expected_auth_type:
        raise RuntimeError(
            f"config export upload.auth.type mismatch: expected {expected_auth_type!r}, got {auth.get('type')!r}"
        )
    if auth.get("api_key_present") is not expect_upload_api_key_present:
        raise RuntimeError(
            "config export upload.auth.api_key_present mismatch: "
            f"expected {expect_upload_api_key_present!r}, got {auth.get('api_key_present')!r}"
        )
    if auth.get("token_present") is not expect_upload_token_present:
        raise RuntimeError(
            "config export upload.auth.token_present mismatch: "
            f"expected {expect_upload_token_present!r}, got {auth.get('token_present')!r}"
        )

    tls = upload.get("tls") if isinstance(upload.get("tls"), dict) else {}
    if tls.get("mode") != expected_tls_mode:
        raise RuntimeError(
            f"config export upload.tls.mode mismatch: expected {expected_tls_mode!r}, got {tls.get('mode')!r}"
        )
    if tls.get("root_profile") != expected_tls_root_profile:
        raise RuntimeError(
            "config export upload.tls.root_profile mismatch: "
            f"expected {expected_tls_root_profile!r}, got {tls.get('root_profile')!r}"
        )

    anchor = tls.get("anchor")
    if expected_anchor_sha256 is None:
        if anchor is not None:
            raise RuntimeError("config export upload.tls.anchor must be null")
    else:
        if not isinstance(anchor, dict):
            raise RuntimeError("config export upload.tls.anchor must be an object")
        if anchor.get("sha256") != expected_anchor_sha256:
            raise RuntimeError(
                "config export upload.tls.anchor.sha256 mismatch: "
                f"expected {expected_anchor_sha256!r}, got {anchor.get('sha256')!r}"
            )
        if anchor.get("format") != "x509_der_base64":
            raise RuntimeError("config export upload.tls.anchor.format must be x509_der_base64")
        if not isinstance(anchor.get("der_base64"), str) or not anchor.get("der_base64"):
            raise RuntimeError("config export upload.tls.anchor.der_base64 must be present")
        if not isinstance(anchor.get("subject"), str) or not anchor.get("subject"):
            raise RuntimeError("config export upload.tls.anchor.subject must be present")

    return payload


def validate_net_test_response(
    response: dict[str, Any],
    *,
    expected_tls_mode: str,
    expected_anchor_sha256: str | None = None,
) -> dict[str, Any]:
    payload = payload_from_response(response)
    for key in ("wifi_join", "dns", "tls", "upload_endpoint_reachable"):
        entry = payload.get(key) if isinstance(payload.get(key), dict) else {}
        if entry.get("result") != "pass":
            raise RuntimeError(f"net-test expected {key}.result='pass', got {entry.get('result')!r}")

    tls = payload.get("tls") if isinstance(payload.get("tls"), dict) else {}
    details = tls.get("details") if isinstance(tls.get("details"), dict) else {}
    message = details.get("message") if isinstance(details.get("message"), str) else ""
    if expected_tls_mode == LOGGER_TLS_MODE_PUBLIC_ROOTS:
        expected_fragment = f"mode=public_roots profile={LOGGER_PUBLIC_ROOT_PROFILE}"
    else:
        expected_fragment = f"mode=provisioned_anchor sha256={expected_anchor_sha256}"
    if expected_fragment not in message:
        raise RuntimeError(
            f"net-test TLS details mismatch: expected fragment {expected_fragment!r}, got {message!r}"
        )
    return payload


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
    if scenario == "upload-interrupted-reboot":
        url = base_url
        if "force" not in query:
            url = append_or_replace_query(url, "force", "temporary_unavailable")
        if "delay_ms" not in query:
            url = append_or_replace_query(url, "delay_ms", str(INTERRUPTED_UPLOAD_DELAY_MS))
        return url
    return base_url


def http_get_json(url: str) -> dict[str, Any]:
    with urlopen(url, timeout=5.0) as response:
        payload = json.load(response)
    if not isinstance(payload, dict):
        raise RuntimeError(f"expected JSON object from {url}")
    return payload


def derive_local_ref_server_healthz_url(upload_url: str | None, *, origin_upload_url: str | None = None) -> str | None:
    target_url = origin_upload_url or upload_url
    if target_url is None:
        return None
    parsed = urlparse(target_url)
    if parsed.scheme != "http" or parsed.port is None:
        return None
    return f"http://127.0.0.1:{parsed.port}/healthz"


def derive_local_ref_server_uploads_url(upload_url: str | None, *, origin_upload_url: str | None = None) -> str | None:
    target_url = origin_upload_url or upload_url
    if target_url is None:
        return None
    parsed = urlparse(target_url)
    if parsed.scheme != "http" or parsed.port is None:
        return None
    return f"http://127.0.0.1:{parsed.port}/uploads"


def wait_for_ref_server_upload_start(healthz_url: str, *, timeout_s: float) -> dict[str, Any]:
    deadline = time.monotonic() + timeout_s
    last_doc: dict[str, Any] | None = None
    last_error: Exception | None = None
    while time.monotonic() < deadline:
        try:
            doc = http_get_json(healthz_url)
            last_doc = doc
            if doc.get("active_upload_requests", 0) >= 1 or doc.get("upload_request_count", 0) >= 1:
                return doc
        except Exception as exc:  # noqa: BLE001
            last_error = exc
        time.sleep(0.1)
    raise RuntimeError(
        "timed out waiting for reference upload server to observe an upload request; "
        f"last_doc={last_doc!r} last_error={last_error!r}"
    )


def reset_target_via_openocd() -> dict[str, Any]:
    completed = subprocess.run(
        DEFAULT_RESET_COMMAND,
        check=False,
        capture_output=True,
        text=True,
        timeout=30.0,
    )
    if completed.returncode != 0:
        raise RuntimeError(
            "target reset via openocd failed: "
            f"rc={completed.returncode} stdout={completed.stdout!r} stderr={completed.stderr!r}"
        )
    return {
        "command": DEFAULT_RESET_COMMAND,
        "returncode": completed.returncode,
        "stdout": completed.stdout,
        "stderr": completed.stderr,
    }


def candidate_serial_ports(preferred_port: Path) -> list[Path]:
    candidates: list[Path] = []
    seen: set[Path] = set()

    def add(port: Path) -> None:
        if port not in seen:
            seen.add(port)
            candidates.append(port)

    add(preferred_port)
    for port in sorted(Path("/dev").glob("ttyACM*")):
        add(port)
    return candidates


def run_subprocess(command: list[str], *, timeout_s: float, cwd: Path | None = None) -> dict[str, Any]:
    completed = subprocess.run(
        command,
        check=False,
        capture_output=True,
        text=True,
        timeout=timeout_s,
        cwd=cwd,
    )
    if completed.returncode != 0:
        raise RuntimeError(
            "subprocess failed: "
            f"command={command!r} rc={completed.returncode} "
            f"stdout={completed.stdout!r} stderr={completed.stderr!r}"
        )
    return {
        "command": command,
        "returncode": completed.returncode,
        "stdout": completed.stdout,
        "stderr": completed.stderr,
    }


def build_firmware_variant(
    *,
    firmware_version: str,
    build_id: str,
    build_dir: Path,
) -> dict[str, Any]:
    configure_command = [
        "cmake",
        "-S",
        str(REPO_ROOT / "logger_firmware"),
        "-B",
        str(build_dir),
        f"-DLOGGER_FIRMWARE_VERSION={firmware_version}",
        f"-DLOGGER_BUILD_ID={build_id}",
    ]
    build_command = [
        "cmake",
        "--build",
        str(build_dir),
        f"-j{os.cpu_count() or 4}",
    ]
    configure_result = run_subprocess(configure_command, timeout_s=180.0, cwd=REPO_ROOT)
    build_result = run_subprocess(build_command, timeout_s=1_200.0, cwd=REPO_ROOT)
    elf_path = build_dir / "logger_appliance.elf"
    if not elf_path.exists():
        raise RuntimeError(f"expected built firmware ELF at {elf_path}")
    return {
        "build_dir": str(build_dir),
        "build_id": build_id,
        "firmware_version": firmware_version,
        "elf_path": str(elf_path),
        "configure": configure_result,
        "build": build_result,
    }


def flash_firmware_via_openocd(elf_path: Path) -> dict[str, Any]:
    elf_path = elf_path.resolve()
    command = [
        "openocd",
        "-f",
        "interface/cmsis-dap.cfg",
        "-f",
        "target/rp2350.cfg",
        "-c",
        f"adapter speed 5000; program {elf_path} verify reset exit",
    ]
    result = run_subprocess(command, timeout_s=600.0, cwd=REPO_ROOT)
    result["elf_path"] = str(elf_path)
    return result


def run_steps_after_reboot(
    port: Path,
    steps: list[Step],
    *,
    timeout_s: float,
) -> tuple[str, str, list[dict[str, Any]], list[dict[str, Any]]]:
    deadline = time.monotonic() + timeout_s
    last_error: Exception | None = None
    while time.monotonic() < deadline:
        for candidate in candidate_serial_ports(port):
            try:
                with SerialJsonClient(candidate) as client:
                    drained, responses, stray = run_steps(client, steps, require_service_mode=False)
                return str(candidate), drained, responses, stray
            except Exception as exc:  # noqa: BLE001
                last_error = exc
            time.sleep(0.5)
    raise RuntimeError(f"timed out waiting for post-reboot serial recovery: {last_error}")


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


def run_steps(
    client: SerialJsonClient,
    steps: list[Step],
    *,
    require_service_mode: bool = True,
) -> tuple[str, list[dict[str, Any]], list[dict[str, Any]]]:
    drained = client.drain_input()
    responses: list[dict[str, Any]] = []
    stray_responses: list[dict[str, Any]] = []
    for index, step in enumerate(steps):
        response, stray = client.send_command(step)
        if index == 0 and require_service_mode:
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
    return scenario in {
        "config-import",
        "https-public-roots",
        "https-provisioned-anchor",
        "upload-verified",
        "upload-failed",
        "upload-blocked-min-firmware",
        "upload-interrupted-reboot",
        "firmware-change-requeue",
        "retention-prune",
    }


def default_upload_auth_args_required(scenario: str) -> bool:
    return default_wifi_args_required(scenario)


def build_scenario(
    name: str,
    start_utc: datetime,
    *,
    upload_url: str | None = None,
    wifi_ssid: str | None = None,
    wifi_psk: str | None = None,
    upload_api_key: str | None = None,
    upload_token: str | None = None,
) -> list[Step]:
    if name == "smoke":
        return [
            Step("status_before", "status --json", "status"),
            Step("queue_before", "queue --json", "queue"),
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
            Step("queue_after", "queue --json", "queue"),
            Step("system_log", "system-log export --json", "system-log export"),
            Step("status_after", "status --json", "status"),
        ]
    if name == "clock-fix":
        return [
            Step("status_before", "status --json", "status"),
            Step("queue_before", "queue --json", "queue"),
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
            Step("queue_after", "queue --json", "queue"),
            Step("system_log", "system-log export --json", "system-log export"),
            Step("status_after", "status --json", "status"),
        ]
    if name == "rollover":
        return [
            Step("status_before", "status --json", "status"),
            Step("queue_before", "queue --json", "queue"),
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
            Step("queue_after", "queue --json", "queue"),
            Step("system_log", "system-log export --json", "system-log export"),
            Step("status_after", "status --json", "status"),
        ]
    if name == "no-session-day":
        return [
            Step("status_before", "status --json", "status"),
            Step("queue_before", "queue --json", "queue"),
            Step("unlock", "service unlock", "service unlock"),
            Step("clock_set", f"clock set {format_rfc3339_utc(start_utc)}", "clock set"),
            Step("no_session_day", "debug synth no-session-day auto 1 1 0", "debug synth no-session-day"),
            Step("status_after", "status --json", "status"),
            Step("queue_after", "queue --json", "queue"),
            Step("system_log", "system-log export --json", "system-log export"),
        ]
    if name in {"upload-verified", "upload-failed", "upload-blocked-min-firmware"}:
        if upload_url is None or wifi_ssid is None or wifi_psk is None or upload_api_key is None or upload_token is None:
            raise ValueError(f"scenario {name!r} requires upload_url, wifi_ssid, wifi_psk, upload_api_key, and upload_token")

        steps = [
            Step("status_before", "status --json", "status"),
            Step("queue_before", "queue --json", "queue"),
            Step("unlock_1", "service unlock", "service unlock"),
            Step("clock_set", f"clock set {format_rfc3339_utc(start_utc)}", "clock set"),
            Step("session_start", "debug session start", "debug session start"),
            Step("ecg_1", "debug synth ecg 4", "debug synth ecg"),
            Step("session_stop", "debug session stop", "debug session stop"),
            Step("queue_mid", "queue --json", "queue"),
            Step("unlock_2", "service unlock", "service unlock"),
            Step("set_wifi_ssid", f"debug config set wifi_ssid {wifi_ssid}", "debug config set"),
            Step("set_wifi_psk", f"debug config set wifi_psk {wifi_psk}", "debug config set"),
            Step("set_upload_url", f"debug config set upload_url {upload_url}", "debug config set"),
            Step("set_upload_api_key", f"debug config set upload_api_key {upload_api_key}", "debug config set"),
            Step("set_upload_token", f"debug config set upload_token {upload_token}", "debug config set"),
        ]
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
                Step("queue_after", "queue --json", "queue"),
                Step("system_log", "system-log export --json", "system-log export"),
                Step("status_after", "status --json", "status"),
            ]
        )
        return steps
    if name == "upload-interrupted-reboot":
        if upload_url is None or wifi_ssid is None or wifi_psk is None or upload_api_key is None or upload_token is None:
            raise ValueError(f"scenario {name!r} requires upload_url, wifi_ssid, wifi_psk, upload_api_key, and upload_token")

        steps = [
            Step("status_before", "status --json", "status"),
            Step("queue_before", "queue --json", "queue"),
            Step("unlock_1", "service unlock", "service unlock"),
            Step("clock_set", f"clock set {format_rfc3339_utc(start_utc)}", "clock set"),
            Step("session_start", "debug session start", "debug session start"),
            Step("ecg_1", "debug synth ecg 4", "debug synth ecg"),
            Step("session_stop", "debug session stop", "debug session stop"),
            Step("queue_mid", "queue --json", "queue"),
            Step("unlock_2", "service unlock", "service unlock"),
            Step("set_wifi_ssid", f"debug config set wifi_ssid {wifi_ssid}", "debug config set"),
            Step("set_wifi_psk", f"debug config set wifi_psk {wifi_psk}", "debug config set"),
            Step("set_upload_url", f"debug config set upload_url {upload_url}", "debug config set"),
            Step("set_upload_api_key", f"debug config set upload_api_key {upload_api_key}", "debug config set"),
            Step("set_upload_token", f"debug config set upload_token {upload_token}", "debug config set"),
        ]
        return steps
    raise ValueError(f"unsupported scenario {name!r}")


def default_start_time(scenario: str) -> datetime:
    if scenario == "rollover":
        return parse_rfc3339_utc("2026-04-02T03:59:50Z")
    if scenario == "no-session-day":
        return parse_rfc3339_utc("2026-04-02T21:00:00Z")
    if scenario in {
        "config-import",
        "https-public-roots",
        "https-provisioned-anchor",
        "upload-verified",
        "upload-failed",
        "upload-blocked-min-firmware",
        "upload-interrupted-reboot",
        "firmware-change-requeue",
        "retention-prune",
    }:
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


def validate_interrupted_upload_recovery(
    responses: list[dict[str, Any]],
    *,
    ref_server_uploads_url: str | None,
) -> dict[str, Any]:
    queue_before = response_by_label(responses, "queue_before")
    queue_mid = response_by_label(responses, "queue_mid")
    queue_after = response_by_label(responses, "queue_after_reboot")
    eligible_before = eligible_queue_session_ids(queue_before)
    if eligible_before:
        raise RuntimeError(
            "upload-interrupted-reboot requires a clean eligible queue before starting; "
            f"found pending/failed sessions: {eligible_before}"
        )

    new_session_ids = extract_new_session_ids(queue_before, queue_mid)
    if len(new_session_ids) != 1:
        raise RuntimeError("upload-interrupted-reboot expected exactly one new closed session before upload")
    session_id = new_session_ids[0]

    upload_entry = queue_entry_map_from_response(queue_after).get(session_id)
    if upload_entry is None:
        raise RuntimeError("interrupted upload session is missing from final queue --json output")
    if upload_entry.get("status") != "failed":
        raise RuntimeError(
            "upload-interrupted-reboot expected final queue status 'failed', "
            f"got {upload_entry.get('status')!r}"
        )
    if upload_entry.get("attempt_count") != 1:
        raise RuntimeError("upload-interrupted-reboot expected attempt_count=1")
    if upload_entry.get("last_failure_class") != "interrupted":
        raise RuntimeError(
            "upload-interrupted-reboot expected last_failure_class='interrupted', "
            f"got {upload_entry.get('last_failure_class')!r}"
        )

    system_log_summary = validate_system_log_response(response_by_label(responses, "system_log_after_reboot"))
    events = find_events_by_kind(response_by_label(responses, "system_log_after_reboot"), "upload_interrupted_recovered")
    if not any(
        isinstance(event, dict)
        and isinstance(event.get("details"), dict)
        and event["details"].get("session_id") == session_id
        for event in events
    ):
        raise RuntimeError(
            "system-log export did not contain upload_interrupted_recovered for "
            f"session_id={session_id}"
        )

    ref_server_uploads = None
    if ref_server_uploads_url is not None:
        ref_server_doc = http_get_json(ref_server_uploads_url)
        uploads = ref_server_doc.get("uploads") if isinstance(ref_server_doc.get("uploads"), list) else []
        ref_server_uploads = uploads
        if any(isinstance(upload, dict) and upload.get("session_id") == session_id for upload in uploads):
            raise RuntimeError("reference server unexpectedly accepted the interrupted upload session")

    return {
        "started_session_id": session_id,
        "queue_before_session_ids": sorted(session_ids_from_queue_response(queue_before)),
        "queue_after_session_ids": sorted(session_ids_from_queue_response(queue_after)),
        "new_session_ids": new_session_ids,
        "system_log_validation": system_log_summary,
        "final_queue_entry": upload_entry,
        "ref_server_uploads": ref_server_uploads,
    }


def run_config_import_scenario(
    port: Path,
    start_utc: datetime,
    *,
    upload_url: str,
    wifi_ssid: str,
    wifi_psk: str,
    upload_api_key: str,
    upload_token: str | None,
) -> dict[str, Any]:
    with SerialJsonClient(port) as client:
        drained = client.drain_input()
        _, initial_responses, initial_stray = run_steps(
            client,
            [
                Step("status_before", "status --json", "status"),
                Step("config_export_before", "config export --json", "config export"),
                Step("system_log_before", "system-log export --json", "system-log export"),
            ],
        )

        export_before = payload_from_response(response_by_label(initial_responses, "config_export_before"))
        hardware_id = export_before.get("hardware_id")
        if not isinstance(hardware_id, str) or not hardware_id:
            raise RuntimeError("config-import scenario could not determine hardware_id")

        identity_before = export_before.get("identity") if isinstance(export_before.get("identity"), dict) else {}
        recording_before = export_before.get("recording") if isinstance(export_before.get("recording"), dict) else {}
        time_before = export_before.get("time") if isinstance(export_before.get("time"), dict) else {}

        logger_id = identity_before.get("logger_id") if isinstance(identity_before.get("logger_id"), str) and identity_before.get("logger_id") else "logger-regression"
        subject_id = identity_before.get("subject_id") if isinstance(identity_before.get("subject_id"), str) and identity_before.get("subject_id") else "subject-regression"
        primary_bound_address = (
            recording_before.get("bound_h10_address")
            if isinstance(recording_before.get("bound_h10_address"), str) and recording_before.get("bound_h10_address")
            else "24:AC:AC:05:A3:10"
        )
        alternate_bound_address = "24:AC:AC:05:A3:11" if primary_bound_address != "24:AC:AC:05:A3:11" else "24:AC:AC:05:A3:10"
        timezone = time_before.get("timezone") if isinstance(time_before.get("timezone"), str) and time_before.get("timezone") else "UTC"

        seeded_doc = build_config_import_document(
            exported_at_utc=format_rfc3339_utc(start_utc),
            hardware_id=hardware_id,
            logger_id=logger_id,
            subject_id=subject_id,
            bound_h10_address=primary_bound_address,
            timezone=timezone,
            wifi_ssid=wifi_ssid,
            wifi_psk=wifi_psk,
            upload_url=upload_url,
            upload_api_key=upload_api_key,
            upload_token=upload_token,
            secrets_included=True,
        )
        cleared_doc = build_config_import_document(
            exported_at_utc=format_rfc3339_utc(start_utc + timedelta(seconds=10)),
            hardware_id=hardware_id,
            logger_id=logger_id,
            subject_id=subject_id,
            bound_h10_address=alternate_bound_address,
            timezone=timezone,
            wifi_ssid=wifi_ssid,
            wifi_psk=wifi_psk,
            upload_url=upload_url,
            upload_api_key=upload_api_key,
            upload_token=upload_token,
            secrets_included=False,
        )
        restore_doc = build_config_import_document(
            exported_at_utc=format_rfc3339_utc(start_utc + timedelta(seconds=20)),
            hardware_id=hardware_id,
            logger_id=logger_id,
            subject_id=subject_id,
            bound_h10_address=primary_bound_address,
            timezone=timezone,
            wifi_ssid=wifi_ssid,
            wifi_psk=wifi_psk,
            upload_url=upload_url,
            upload_api_key=upload_api_key,
            upload_token=upload_token,
            secrets_included=True,
        )

        _, seed_responses, seed_stray = run_steps(
            client,
            [
                Step("unlock_seed", "service unlock", "service unlock"),
                Step("import_seed", build_config_import_command(seeded_doc), "config import", timeout_s=10.0),
                Step("config_export_seed", "config export --json", "config export"),
            ],
            require_service_mode=False,
        )
        _, clear_responses, clear_stray = run_steps(
            client,
            [
                Step("unlock_clear", "service unlock", "service unlock"),
                Step("import_clear", build_config_import_command(cleared_doc), "config import", timeout_s=10.0),
                Step("config_export_clear", "config export --json", "config export"),
            ],
            require_service_mode=False,
        )
        _, restore_responses, restore_stray = run_steps(
            client,
            [
                Step("unlock_restore", "service unlock", "service unlock"),
                Step("import_restore", build_config_import_command(restore_doc), "config import", timeout_s=10.0),
                Step("config_export_restore", "config export --json", "config export"),
                Step("reboot", "debug reboot", "debug reboot"),
            ],
            require_service_mode=False,
        )

    post_port, post_drained, post_responses, post_stray = run_steps_after_reboot(
        port,
        [
            Step("status_after_reboot", "status --json", "status", timeout_s=5.0),
            Step("config_export_after_reboot", "config export --json", "config export", timeout_s=5.0),
            Step("system_log_after_reboot", "system-log export --json", "system-log export", timeout_s=5.0),
        ],
        timeout_s=POST_REBOOT_READY_TIMEOUT_S,
    )

    responses = initial_responses + seed_responses + clear_responses + restore_responses + post_responses
    stray_responses = initial_stray + seed_stray + clear_stray + restore_stray + post_stray

    import_seed_payload = payload_from_response(response_by_label(responses, "import_seed"))
    if import_seed_payload.get("applied") is not True or import_seed_payload.get("normal_logging_ready") is not True:
        raise RuntimeError("config-import seed import did not apply a normal-logging-ready config")
    validate_config_export_payload(
        response_by_label(responses, "config_export_seed"),
        hardware_id=hardware_id,
        logger_id=logger_id,
        subject_id=subject_id,
        bound_h10_address=primary_bound_address,
        timezone=timezone,
        wifi_ssid=wifi_ssid,
        upload_url=upload_url,
        expect_wifi_psk_present=True,
        expect_upload_api_key_present=upload_api_key is not None,
        expect_upload_token_present=upload_token is not None,
        expected_auth_type="api_key_and_bearer",
    )

    import_clear_payload = payload_from_response(response_by_label(responses, "import_clear"))
    if import_clear_payload.get("applied") is not True or import_clear_payload.get("normal_logging_ready") is not True:
        raise RuntimeError("config-import clear import did not apply a normal-logging-ready config")
    if import_clear_payload.get("bond_cleared") is not True:
        raise RuntimeError("config-import clear import expected bond_cleared=true after changing bound_h10_address")
    validate_config_export_payload(
        response_by_label(responses, "config_export_clear"),
        hardware_id=hardware_id,
        logger_id=logger_id,
        subject_id=subject_id,
        bound_h10_address=alternate_bound_address,
        timezone=timezone,
        wifi_ssid=wifi_ssid,
        upload_url=upload_url,
        expect_wifi_psk_present=False,
        expect_upload_api_key_present=False,
        expect_upload_token_present=False,
        expected_auth_type="api_key_and_bearer",
    )

    import_restore_payload = payload_from_response(response_by_label(responses, "import_restore"))
    if import_restore_payload.get("applied") is not True or import_restore_payload.get("normal_logging_ready") is not True:
        raise RuntimeError("config-import restore import did not apply a normal-logging-ready config")
    if import_restore_payload.get("bond_cleared") is not True:
        raise RuntimeError("config-import restore import expected bond_cleared=true when restoring the prior bound_h10_address")
    validate_config_export_payload(
        response_by_label(responses, "config_export_restore"),
        hardware_id=hardware_id,
        logger_id=logger_id,
        subject_id=subject_id,
        bound_h10_address=primary_bound_address,
        timezone=timezone,
        wifi_ssid=wifi_ssid,
        upload_url=upload_url,
        expect_wifi_psk_present=True,
        expect_upload_api_key_present=upload_api_key is not None,
        expect_upload_token_present=upload_token is not None,
        expected_auth_type="api_key_and_bearer",
    )

    status_after_reboot = payload_from_response(response_by_label(responses, "status_after_reboot"))
    if status_after_reboot.get("mode") != "logging":
        raise RuntimeError("config-import scenario expected the device to return to logging mode after reboot")
    if status_after_reboot.get("runtime_state") != "log_wait_h10":
        raise RuntimeError("config-import scenario expected runtime_state=log_wait_h10 after reboot")
    validate_config_export_payload(
        response_by_label(responses, "config_export_after_reboot"),
        hardware_id=hardware_id,
        logger_id=logger_id,
        subject_id=subject_id,
        bound_h10_address=primary_bound_address,
        timezone=timezone,
        wifi_ssid=wifi_ssid,
        upload_url=upload_url,
        expect_wifi_psk_present=True,
        expect_upload_api_key_present=upload_api_key is not None,
        expect_upload_token_present=upload_token is not None,
        expected_auth_type="api_key_and_bearer",
    )

    system_log_before = response_by_label(responses, "system_log_before")
    system_log_after_reboot = response_by_label(responses, "system_log_after_reboot")
    system_log_summary = validate_system_log_response(system_log_after_reboot)
    config_change_events = [
        event
        for event in find_events_by_kind_after_seq(
            system_log_after_reboot,
            "config_changed",
            max_system_log_event_seq(system_log_before),
        )
        if isinstance(event.get("details"), dict) and event["details"].get("source") == "config_import"
    ]
    if len(config_change_events) < 3:
        raise RuntimeError("config-import scenario expected at least three new config_changed events from config_import")
    if not any(event["details"].get("bond_cleared") is True for event in config_change_events):
        raise RuntimeError("config-import scenario expected at least one config_changed event with bond_cleared=true")

    return {
        "scenario": "config-import",
        "port": str(port),
        "post_reboot_port": post_port,
        "start_utc": format_rfc3339_utc(start_utc),
        "effective_upload_url": upload_url,
        "drained_preamble": drained,
        "post_reboot_drained_preamble": post_drained,
        "responses": responses,
        "stray_responses": stray_responses,
        "hardware_id": hardware_id,
        "started_session_id": None,
        "queue_before_session_ids": [],
        "queue_after_session_ids": [],
        "new_session_ids": [],
        "system_log_validation": system_log_summary,
    }


def run_https_trust_scenario(
    port: Path,
    scenario: str,
    start_utc: datetime,
    *,
    upload_url: str,
    wifi_ssid: str,
    wifi_psk: str,
    upload_api_key: str,
    upload_token: str | None,
    trust_mode: str,
    spawn_ref_server: bool,
    ref_server_data_dir: Path | None,
    ref_server_origin_url: str | None,
    tls_ca_cert: Path | None,
) -> dict[str, Any]:
    parsed_upload_url = urlparse(upload_url)
    if parsed_upload_url.scheme != "https":
        raise ValueError(f"{scenario} requires an https:// upload_url")
    if spawn_ref_server and ref_server_origin_url is None:
        raise ValueError(f"{scenario} requires --ref-server-origin-url when --spawn-ref-server is used with an HTTPS upload URL")

    tls_probe, trust_plan = plan_logger_tls(upload_url, trust_mode=trust_mode, ca_cert_path=tls_ca_cert, timeout_s=15.0)
    expected_anchor_sha256 = (
        trust_plan.selected_anchor_sha256 if trust_plan.mode == LOGGER_TLS_MODE_PROVISIONED_ANCHOR else None
    )
    ref_server_uploads_url = derive_local_ref_server_uploads_url(
        upload_url,
        origin_upload_url=ref_server_origin_url,
    )

    with ExitStack() as stack:
        if spawn_ref_server:
            if ref_server_data_dir is None:
                stamp = datetime.now(UTC).strftime("%Y%m%d_%H%M%S")
                ref_server_data_dir = Path("build") / f"logger_upload_ref_server_runner_{scenario}_{stamp}"
            ref_server = stack.enter_context(
                RefServerProcess(
                    RefServerConfig(
                        upload_url=upload_url,
                        data_dir=ref_server_data_dir,
                        api_key=upload_api_key,
                        bearer_token=upload_token,
                        min_firmware=None,
                        origin_upload_url=ref_server_origin_url,
                    )
                )
            )
            ref_server_uploads_url = ref_server.uploads_url

        restore_clock_utc = format_rfc3339_utc(datetime.now(UTC))
        prune_clock_utc = format_rfc3339_utc(start_utc + timedelta(days=18))

        with SerialJsonClient(port) as client:
            drained = client.drain_input()
            _, initial_responses, initial_stray = run_steps(
                client,
                [
                    Step("status_before", "status --json", "status"),
                    Step("queue_before", "queue --json", "queue"),
                    Step("config_export_before", "config export --json", "config export"),
                    Step("system_log_before", "system-log export --json", "system-log export"),
                ],
            )

            export_before = payload_from_response(response_by_label(initial_responses, "config_export_before"))
            hardware_id = export_before.get("hardware_id")
            if not isinstance(hardware_id, str) or not hardware_id:
                raise RuntimeError(f"{scenario} could not determine hardware_id")

            identity_before = export_before.get("identity") if isinstance(export_before.get("identity"), dict) else {}
            recording_before = export_before.get("recording") if isinstance(export_before.get("recording"), dict) else {}
            time_before = export_before.get("time") if isinstance(export_before.get("time"), dict) else {}

            logger_id = (
                identity_before.get("logger_id")
                if isinstance(identity_before.get("logger_id"), str) and identity_before.get("logger_id")
                else "logger-regression"
            )
            subject_id = (
                identity_before.get("subject_id")
                if isinstance(identity_before.get("subject_id"), str) and identity_before.get("subject_id")
                else "subject-regression"
            )
            bound_h10_address = (
                recording_before.get("bound_h10_address")
                if isinstance(recording_before.get("bound_h10_address"), str) and recording_before.get("bound_h10_address")
                else "24:AC:AC:05:A3:10"
            )
            timezone = (
                time_before.get("timezone")
                if isinstance(time_before.get("timezone"), str) and time_before.get("timezone")
                else "UTC"
            )

            import_doc = build_config_import_document(
                exported_at_utc=format_rfc3339_utc(start_utc),
                hardware_id=hardware_id,
                logger_id=logger_id,
                subject_id=subject_id,
                bound_h10_address=bound_h10_address,
                timezone=timezone,
                wifi_ssid=wifi_ssid,
                wifi_psk=wifi_psk,
                upload_url=upload_url,
                upload_api_key=upload_api_key,
                upload_token=upload_token,
                secrets_included=True,
                upload_tls=build_upload_tls_document(upload_url=upload_url, trust_plan=trust_plan),
            )

            steps = [
                *build_chunked_config_import_steps("import", import_doc),
                Step("config_export_after_import", "config export --json", "config export"),
                Step("net_test_after_import", "net-test --json", "net-test", timeout_s=120.0),
                Step("unlock_clock", "service unlock", "service unlock"),
                Step("clock_set", f"clock set {format_rfc3339_utc(start_utc)}", "clock set"),
                Step("session_start", "debug session start", "debug session start"),
                Step("ecg_1", "debug synth ecg 4", "debug synth ecg"),
                Step("session_stop", "debug session stop", "debug session stop"),
                Step("queue_mid", "queue --json", "queue"),
                Step("unlock_upload", "service unlock", "service unlock"),
                Step("upload_once", "debug upload once", "debug upload once", timeout_s=120.0),
                Step("queue_after", "queue --json", "queue"),
                Step("system_log", "system-log export --json", "system-log export"),
            ]
            if trust_plan.mode == LOGGER_TLS_MODE_PROVISIONED_ANCHOR:
                steps.extend(
                    [
                        Step("unlock_clear", "service unlock", "service unlock"),
                        Step(
                            "clear_anchor",
                            "upload tls clear-provisioned-anchor",
                            "upload tls clear-provisioned-anchor",
                        ),
                        Step("config_export_after_clear", "config export --json", "config export"),
                        Step("net_test_after_clear", "net-test --json", "net-test", timeout_s=120.0),
                    ]
                )
            steps.extend(
                [
                    Step("unlock_prune_clock", "service unlock", "service unlock"),
                    Step("clock_set_prune", f"clock set {prune_clock_utc}", "clock set"),
                    Step("unlock_prune", "service unlock", "service unlock"),
                    Step("prune_once", "debug prune once", "debug prune once", timeout_s=10.0),
                    Step("queue_after_prune", "queue --json", "queue"),
                    Step("system_log_after_cleanup", "system-log export --json", "system-log export"),
                    Step("unlock_restore_clock", "service unlock", "service unlock"),
                    Step("clock_restore", f"clock set {restore_clock_utc}", "clock set"),
                    Step("reboot", "debug reboot", "debug reboot"),
                ]
            )

            _, scenario_responses, scenario_stray = run_steps(client, steps, require_service_mode=False)

        post_port, post_drained, post_responses, post_stray = run_steps_after_reboot(
            port,
            [
                Step("status_after_reboot", "status --json", "status", timeout_s=5.0),
                Step("queue_after_reboot", "queue --json", "queue", timeout_s=5.0),
                Step("config_export_after_reboot", "config export --json", "config export", timeout_s=5.0),
            ],
            timeout_s=POST_REBOOT_READY_TIMEOUT_S,
        )

        responses = initial_responses + scenario_responses + post_responses
        stray_responses = initial_stray + scenario_stray + post_stray

        import_payload = payload_from_response(response_by_label(responses, "import_commit"))
        if import_payload.get("applied") is not True or import_payload.get("normal_logging_ready") is not True:
            raise RuntimeError(f"{scenario} import did not apply a normal-logging-ready config")

        validate_config_export_payload(
            response_by_label(responses, "config_export_after_import"),
            hardware_id=hardware_id,
            logger_id=logger_id,
            subject_id=subject_id,
            bound_h10_address=bound_h10_address,
            timezone=timezone,
            wifi_ssid=wifi_ssid,
            upload_url=upload_url,
            expect_wifi_psk_present=True,
            expect_upload_api_key_present=upload_api_key is not None,
            expect_upload_token_present=upload_token is not None,
            expected_auth_type="api_key_and_bearer",
            expected_tls_mode=trust_plan.mode,
            expected_tls_root_profile=trust_plan.root_profile,
            expected_anchor_sha256=expected_anchor_sha256,
        )
        validate_net_test_response(
            response_by_label(responses, "net_test_after_import"),
            expected_tls_mode=trust_plan.mode,
            expected_anchor_sha256=expected_anchor_sha256,
        )

        upload_analysis = validate_upload_outcome(
            "upload-verified",
            responses,
            ref_server_uploads_url=ref_server_uploads_url,
        )
        session_id = upload_analysis["started_session_id"]

        prune_payload = payload_from_response(response_by_label(responses, "prune_once"))
        if int(prune_payload.get("retention_pruned_count", 0)) < 1:
            raise RuntimeError(f"{scenario} expected retention_pruned_count >= 1")
        if prune_payload.get("reserve_met") is not True:
            raise RuntimeError(f"{scenario} expected reserve_met=true")

        queue_after_prune = response_by_label(responses, "queue_after_prune")
        if session_id in session_ids_from_queue_response(queue_after_prune):
            raise RuntimeError(f"{scenario} expected the uploaded session to be pruned during cleanup")

        system_log_after_cleanup = response_by_label(responses, "system_log_after_cleanup")
        system_log_summary = validate_system_log_response(system_log_after_cleanup)
        config_changed_events = find_events_by_kind_after_seq(
            system_log_after_cleanup,
            "config_changed",
            max_system_log_event_seq(response_by_label(responses, "system_log_before")),
        )
        if not any(
            isinstance(event.get("details"), dict) and event["details"].get("source") == "config_import"
            for event in config_changed_events
        ):
            raise RuntimeError(f"{scenario} expected a config_changed event from config_import")

        clear_payload = None
        if trust_plan.mode == LOGGER_TLS_MODE_PROVISIONED_ANCHOR:
            clear_payload = payload_from_response(response_by_label(responses, "clear_anchor"))
            if clear_payload.get("cleared") is not True or clear_payload.get("had_anchor") is not True:
                raise RuntimeError(f"{scenario} expected upload tls clear-provisioned-anchor to clear an existing anchor")
            if clear_payload.get("current_tls_mode") != LOGGER_TLS_MODE_PUBLIC_ROOTS:
                raise RuntimeError(f"{scenario} expected clear-provisioned-anchor to fall back to public_roots")
            if clear_payload.get("upload_ready") is not True:
                raise RuntimeError(f"{scenario} expected upload_ready=true after clear-provisioned-anchor")

            validate_config_export_payload(
                response_by_label(responses, "config_export_after_clear"),
                hardware_id=hardware_id,
                logger_id=logger_id,
                subject_id=subject_id,
                bound_h10_address=bound_h10_address,
                timezone=timezone,
                wifi_ssid=wifi_ssid,
                upload_url=upload_url,
                expect_wifi_psk_present=True,
                expect_upload_api_key_present=upload_api_key is not None,
                expect_upload_token_present=upload_token is not None,
                expected_auth_type="api_key_and_bearer",
                expected_tls_mode=LOGGER_TLS_MODE_PUBLIC_ROOTS,
                expected_tls_root_profile=LOGGER_PUBLIC_ROOT_PROFILE,
                expected_anchor_sha256=None,
            )
            validate_net_test_response(
                response_by_label(responses, "net_test_after_clear"),
                expected_tls_mode=LOGGER_TLS_MODE_PUBLIC_ROOTS,
            )
            if not any(
                isinstance(event.get("details"), dict)
                and event["details"].get("source") == "upload_tls_clear_provisioned_anchor"
                for event in config_changed_events
            ):
                raise RuntimeError(f"{scenario} expected a config_changed event from upload_tls_clear_provisioned_anchor")

        status_after_reboot = payload_from_response(response_by_label(responses, "status_after_reboot"))
        if status_after_reboot.get("mode") != "logging":
            raise RuntimeError(f"{scenario} expected the device to return to logging mode after reboot")
        if status_after_reboot.get("runtime_state") != "log_wait_h10":
            raise RuntimeError(f"{scenario} expected runtime_state=log_wait_h10 after reboot")

        if session_id in session_ids_from_queue_response(response_by_label(responses, "queue_after_reboot")):
            raise RuntimeError(f"{scenario} expected cleanup to remove the uploaded session before reboot")

        validate_config_export_payload(
            response_by_label(responses, "config_export_after_reboot"),
            hardware_id=hardware_id,
            logger_id=logger_id,
            subject_id=subject_id,
            bound_h10_address=bound_h10_address,
            timezone=timezone,
            wifi_ssid=wifi_ssid,
            upload_url=upload_url,
            expect_wifi_psk_present=True,
            expect_upload_api_key_present=upload_api_key is not None,
            expect_upload_token_present=upload_token is not None,
            expected_auth_type="api_key_and_bearer",
            expected_tls_mode=LOGGER_TLS_MODE_PUBLIC_ROOTS,
            expected_tls_root_profile=LOGGER_PUBLIC_ROOT_PROFILE,
            expected_anchor_sha256=None,
        )

        return {
            "scenario": scenario,
            "port": str(port),
            "post_reboot_port": post_port,
            "start_utc": format_rfc3339_utc(start_utc),
            "effective_upload_url": upload_url,
            "drained_preamble": drained,
            "post_reboot_drained_preamble": post_drained,
            "responses": responses,
            "stray_responses": stray_responses,
            "started_session_id": session_id,
            "queue_before_session_ids": sorted(session_ids_from_queue_response(response_by_label(responses, "queue_before"))),
            "queue_after_session_ids": sorted(session_ids_from_queue_response(response_by_label(responses, "queue_after_reboot"))),
            "new_session_ids": [session_id],
            "system_log_validation": system_log_summary,
            "final_queue_entry": upload_analysis["final_queue_entry"],
            "ref_server_uploads": upload_analysis["ref_server_uploads"],
            "tls_probe": summarize_tls_probe(tls_probe),
            "tls_plan": summarize_tls_plan(trust_plan),
            "net_test_after_import": payload_from_response(response_by_label(responses, "net_test_after_import")),
            "net_test_after_clear": (
                payload_from_response(response_by_label(responses, "net_test_after_clear"))
                if trust_plan.mode == LOGGER_TLS_MODE_PROVISIONED_ANCHOR
                else None
            ),
            "clear_anchor": clear_payload,
            "prune_result": prune_payload,
        }


def run_firmware_change_requeue_scenario(
    port: Path,
    start_utc: datetime,
    *,
    upload_url: str,
    wifi_ssid: str,
    wifi_psk: str,
    upload_api_key: str,
    upload_token: str | None,
    spawn_ref_server: bool,
    ref_server_data_dir: Path | None,
    firmware_elf: Path,
    requeue_temp_build_id: str,
    requeue_temp_build_dir: Path | None,
) -> dict[str, Any]:
    blocked_upload_url = append_or_replace_query(upload_url, "force", "minimum_firmware")
    ref_server_uploads_url = derive_local_ref_server_uploads_url(upload_url)

    with ExitStack() as stack:
        if spawn_ref_server:
            if ref_server_data_dir is None:
                stamp = datetime.now(UTC).strftime("%Y%m%d_%H%M%S")
                ref_server_data_dir = Path("build") / f"logger_upload_ref_server_runner_firmware_change_requeue_{stamp}"
            ref_server = stack.enter_context(
                RefServerProcess(
                    RefServerConfig(
                        upload_url=upload_url,
                        data_dir=ref_server_data_dir,
                        api_key=upload_api_key,
                        bearer_token=upload_token,
                        min_firmware=None,
                    )
                )
            )
            ref_server_uploads_url = ref_server.uploads_url

        with SerialJsonClient(port) as client:
            drained = client.drain_input()
            _, setup_responses, setup_stray = run_steps(
                client,
                [
                    Step("status_before", "status --json", "status"),
                    Step("queue_before", "queue --json", "queue"),
                    Step("system_log_before", "system-log export --json", "system-log export"),
                    Step("fault_clear_before", "fault clear", "fault clear"),
                    Step("unlock_1", "service unlock", "service unlock"),
                    Step("clock_set", f"clock set {format_rfc3339_utc(start_utc)}", "clock set"),
                    Step("session_start", "debug session start", "debug session start"),
                    Step("ecg_1", "debug synth ecg 4", "debug synth ecg"),
                    Step("session_stop", "debug session stop", "debug session stop"),
                    Step("queue_mid", "queue --json", "queue"),
                    Step("unlock_2", "service unlock", "service unlock"),
                    Step("set_wifi_ssid", f"debug config set wifi_ssid {wifi_ssid}", "debug config set"),
                    Step("set_wifi_psk", f"debug config set wifi_psk {wifi_psk}", "debug config set"),
                    Step("set_upload_url", f"debug config set upload_url {blocked_upload_url}", "debug config set"),
                    Step("set_upload_api_key", f"debug config set upload_api_key {upload_api_key}", "debug config set"),
                    Step("set_upload_token", f"debug config set upload_token {upload_token}", "debug config set"),
                    Step("unlock_3", "service unlock", "service unlock"),
                    Step("upload_once", "debug upload once", "debug upload once", timeout_s=90.0, allow_error=True),
                    Step("queue_after", "queue --json", "queue"),
                    Step("system_log", "system-log export --json", "system-log export"),
                ],
            )

        blocked_analysis = validate_upload_outcome(
            "upload-blocked-min-firmware",
            setup_responses,
            ref_server_uploads_url=ref_server_uploads_url,
        )
        session_id = blocked_analysis["started_session_id"]
        status_before = payload_from_response(response_by_label(setup_responses, "status_before"))
        firmware_before = status_before.get("firmware") if isinstance(status_before.get("firmware"), dict) else {}
        firmware_version = firmware_before.get("version") if isinstance(firmware_before.get("version"), str) else None
        current_build_id = firmware_before.get("build_id") if isinstance(firmware_before.get("build_id"), str) else None
        if firmware_version is None or current_build_id is None:
            raise RuntimeError("firmware-change-requeue could not determine the current firmware identity")
        if current_build_id == requeue_temp_build_id:
            raise RuntimeError("requeue_temp_build_id must differ from the currently running build_id")

        temp_build_dir = requeue_temp_build_dir or (REPO_ROOT / "build" / "logger_firmware_requeue_variant")
        temp_build = build_firmware_variant(
            firmware_version=firmware_version,
            build_id=requeue_temp_build_id,
            build_dir=temp_build_dir,
        )
        flash_temp_result = flash_firmware_via_openocd(Path(temp_build["elf_path"]))
        time.sleep(INTERRUPTED_UPLOAD_REBOOT_SETTLE_S)

        requeue_port, requeue_drained, requeue_responses, requeue_stray = run_steps_after_reboot(
            port,
            [
                Step("status_after_requeue_boot", "status --json", "status", timeout_s=5.0),
                Step("queue_after_requeue_boot", "queue --json", "queue", timeout_s=5.0),
                Step("system_log_after_requeue_boot", "system-log export --json", "system-log export", timeout_s=5.0),
            ],
            timeout_s=POST_REBOOT_READY_TIMEOUT_S,
        )

        flash_restore_result = flash_firmware_via_openocd(firmware_elf)
        time.sleep(INTERRUPTED_UPLOAD_REBOOT_SETTLE_S)

        cleanup_steps = [Step("status_after_restore_boot", "status --json", "status", timeout_s=5.0)]
        if blocked_upload_url != upload_url:
            cleanup_steps.append(
                Step("restore_upload_url", f"debug config set upload_url {upload_url}", "debug config set", timeout_s=5.0)
            )
        cleanup_steps.extend(
            [
                Step("upload_cleanup", "debug upload once", "debug upload once", timeout_s=90.0),
                Step("queue_after_cleanup", "queue --json", "queue", timeout_s=5.0),
                Step("system_log_after_cleanup", "system-log export --json", "system-log export", timeout_s=5.0),
                Step("status_final", "status --json", "status", timeout_s=5.0),
            ]
        )
        cleanup_port, cleanup_drained, cleanup_responses, cleanup_stray = run_steps_after_reboot(
            port,
            cleanup_steps,
            timeout_s=POST_REBOOT_READY_TIMEOUT_S + 60.0,
        )

        responses = setup_responses + requeue_responses + cleanup_responses
        stray_responses = setup_stray + requeue_stray + cleanup_stray

        requeue_status = payload_from_response(response_by_label(responses, "status_after_requeue_boot"))
        if requeue_status.get("mode") != "logging":
            raise RuntimeError("firmware-change-requeue expected the reflashed device to boot into logging mode")
        requeue_firmware = requeue_status.get("firmware") if isinstance(requeue_status.get("firmware"), dict) else {}
        if requeue_firmware.get("build_id") != requeue_temp_build_id:
            raise RuntimeError("firmware-change-requeue did not boot the temporary build variant")

        requeued_entry = queue_entry_map_from_response(response_by_label(responses, "queue_after_requeue_boot")).get(session_id)
        if requeued_entry is None:
            raise RuntimeError("firmware-change-requeue could not find the blocked session after reflashing")
        if requeued_entry.get("status") != "pending":
            raise RuntimeError(
                "firmware-change-requeue expected the blocked session to become pending after reflashing, "
                f"got {requeued_entry.get('status')!r}"
            )
        if requeued_entry.get("attempt_count") != 1:
            raise RuntimeError("firmware-change-requeue expected attempt_count=1 to be preserved")
        if requeued_entry.get("last_failure_class") is not None:
            raise RuntimeError("firmware-change-requeue expected last_failure_class to be cleared after requeue")

        requeue_system_log = response_by_label(responses, "system_log_after_requeue_boot")
        requeue_system_log_summary = validate_system_log_response(requeue_system_log)
        requeue_events = [
            event
            for event in find_events_by_kind_after_seq(
                requeue_system_log,
                "upload_queue_requeued",
                max_system_log_event_seq(response_by_label(responses, "system_log")),
            )
            if isinstance(event.get("details"), dict)
        ]
        if not any(
            event["details"].get("reason") == "firmware_changed" and event["details"].get("count", 0) >= 1
            for event in requeue_events
        ):
            raise RuntimeError("firmware-change-requeue expected an upload_queue_requeued event with reason=firmware_changed")

        restore_status = payload_from_response(response_by_label(responses, "status_after_restore_boot"))
        if restore_status.get("mode") != "logging":
            raise RuntimeError("firmware-change-requeue expected the restored firmware to boot into logging mode")
        restore_firmware = restore_status.get("firmware") if isinstance(restore_status.get("firmware"), dict) else {}
        if restore_firmware.get("build_id") != current_build_id:
            raise RuntimeError("firmware-change-requeue did not restore the original firmware build_id")

        cleanup_upload = response_by_label(responses, "upload_cleanup")
        require_ok(cleanup_upload, Step("upload_cleanup", "debug upload once", "debug upload once"))
        cleanup_payload = payload_from_response(cleanup_upload)
        if cleanup_payload.get("session_id") != session_id or cleanup_payload.get("final_status") != "verified":
            raise RuntimeError("firmware-change-requeue cleanup upload did not verify the requeued session")

        cleanup_entry = queue_entry_map_from_response(response_by_label(responses, "queue_after_cleanup")).get(session_id)
        if cleanup_entry is None:
            raise RuntimeError("firmware-change-requeue cleanup could not find the session in queue_after_cleanup")
        if cleanup_entry.get("status") != "verified":
            raise RuntimeError("firmware-change-requeue cleanup expected final queue status verified")
        if not isinstance(cleanup_entry.get("receipt_id"), str):
            raise RuntimeError("firmware-change-requeue cleanup expected a receipt_id after upload verification")

        cleanup_system_log = response_by_label(responses, "system_log_after_cleanup")
        cleanup_system_log_summary = validate_system_log_response(cleanup_system_log)
        cleanup_events = find_events_by_kind_after_seq(
            cleanup_system_log,
            "upload_verified",
            max_system_log_event_seq(requeue_system_log),
        )
        if not any(
            isinstance(event.get("details"), dict) and event["details"].get("session_id") == session_id
            for event in cleanup_events
        ):
            raise RuntimeError("firmware-change-requeue cleanup expected an upload_verified event for the requeued session")

        ref_server_uploads = None
        if ref_server_uploads_url is not None:
            ref_server_doc = http_get_json(ref_server_uploads_url)
            uploads = ref_server_doc.get("uploads") if isinstance(ref_server_doc.get("uploads"), list) else []
            ref_server_uploads = uploads
            if not any(isinstance(upload, dict) and upload.get("session_id") == session_id for upload in uploads):
                raise RuntimeError("firmware-change-requeue cleanup expected the reference server to accept the requeued session")

        return {
            "scenario": "firmware-change-requeue",
            "port": str(port),
            "requeue_port": requeue_port,
            "cleanup_port": cleanup_port,
            "start_utc": format_rfc3339_utc(start_utc),
            "effective_upload_url": blocked_upload_url,
            "drained_preamble": drained,
            "post_requeue_drained_preamble": requeue_drained,
            "post_restore_drained_preamble": cleanup_drained,
            "responses": responses,
            "stray_responses": stray_responses,
            "started_session_id": session_id,
            "queue_before_session_ids": blocked_analysis["queue_before_session_ids"],
            "queue_after_session_ids": sorted(session_ids_from_queue_response(response_by_label(responses, "queue_after_cleanup"))),
            "new_session_ids": blocked_analysis["new_session_ids"],
            "system_log_validation": cleanup_system_log_summary,
            "final_queue_entry": cleanup_entry,
            "requeued_queue_entry": requeued_entry,
            "ref_server_uploads": ref_server_uploads,
            "temp_build": temp_build,
            "flash_temp_result": flash_temp_result,
            "flash_restore_result": flash_restore_result,
            "requeue_system_log_validation": requeue_system_log_summary,
        }


def run_retention_prune_scenario(
    port: Path,
    start_utc: datetime,
    *,
    upload_url: str,
    wifi_ssid: str,
    wifi_psk: str,
    upload_api_key: str,
    upload_token: str | None,
    spawn_ref_server: bool,
    ref_server_data_dir: Path | None,
) -> dict[str, Any]:
    effective_upload_url = upload_url
    ref_server_uploads_url = derive_local_ref_server_uploads_url(upload_url)

    with ExitStack() as stack:
        if spawn_ref_server:
            if ref_server_data_dir is None:
                stamp = datetime.now(UTC).strftime("%Y%m%d_%H%M%S")
                ref_server_data_dir = Path("build") / f"logger_upload_ref_server_runner_retention_prune_{stamp}"
            ref_server = stack.enter_context(
                RefServerProcess(
                    RefServerConfig(
                        upload_url=effective_upload_url,
                        data_dir=ref_server_data_dir,
                        api_key=upload_api_key,
                        bearer_token=upload_token,
                        min_firmware=None,
                    )
                )
            )
            ref_server_uploads_url = ref_server.uploads_url

        restore_clock_utc = format_rfc3339_utc(datetime.now(UTC))
        prune_clock_utc = format_rfc3339_utc(start_utc + timedelta(days=18))

        with SerialJsonClient(port) as client:
            drained = client.drain_input()
            _, responses, stray_responses = run_steps(
                client,
                [
                    Step("status_before", "status --json", "status"),
                    Step("queue_before", "queue --json", "queue"),
                    Step("system_log_before", "system-log export --json", "system-log export"),
                    Step("fault_clear_before", "fault clear", "fault clear"),
                    Step("unlock_1", "service unlock", "service unlock"),
                    Step("clock_set_start", f"clock set {format_rfc3339_utc(start_utc)}", "clock set"),
                    Step("session_start", "debug session start", "debug session start"),
                    Step("ecg_1", "debug synth ecg 4", "debug synth ecg"),
                    Step("session_stop", "debug session stop", "debug session stop"),
                    Step("queue_mid", "queue --json", "queue"),
                    Step("unlock_2", "service unlock", "service unlock"),
                    Step("set_wifi_ssid", f"debug config set wifi_ssid {wifi_ssid}", "debug config set"),
                    Step("set_wifi_psk", f"debug config set wifi_psk {wifi_psk}", "debug config set"),
                    Step("set_upload_url", f"debug config set upload_url {effective_upload_url}", "debug config set"),
                    Step("set_upload_api_key", f"debug config set upload_api_key {upload_api_key}", "debug config set"),
                    Step("set_upload_token", f"debug config set upload_token {upload_token}", "debug config set"),
                    Step("unlock_3", "service unlock", "service unlock"),
                    Step("upload_once", "debug upload once", "debug upload once", timeout_s=90.0),
                    Step("queue_after_upload", "queue --json", "queue"),
                    Step("system_log_after_upload", "system-log export --json", "system-log export"),
                    Step("unlock_4", "service unlock", "service unlock"),
                    Step("clock_set_prune", f"clock set {prune_clock_utc}", "clock set"),
                    Step("unlock_5", "service unlock", "service unlock"),
                    Step("prune_once", "debug prune once", "debug prune once", timeout_s=10.0),
                    Step("queue_after_prune", "queue --json", "queue"),
                    Step("system_log_after_prune", "system-log export --json", "system-log export"),
                    Step("unlock_6", "service unlock", "service unlock"),
                    Step("clock_restore", f"clock set {restore_clock_utc}", "clock set"),
                    Step("reboot", "debug reboot", "debug reboot"),
                ],
            )

        post_port, post_drained, post_responses, post_stray = run_steps_after_reboot(
            port,
            [
                Step("status_after_reboot", "status --json", "status", timeout_s=5.0),
                Step("queue_after_reboot", "queue --json", "queue", timeout_s=5.0),
            ],
            timeout_s=POST_REBOOT_READY_TIMEOUT_S,
        )

        responses = responses + post_responses
        stray_responses = stray_responses + post_stray

        queue_before = response_by_label(responses, "queue_before")
        queue_mid = response_by_label(responses, "queue_mid")
        queue_after_upload = response_by_label(responses, "queue_after_upload")
        if eligible_queue_session_ids(queue_before):
            raise RuntimeError("retention-prune requires a clean eligible queue before starting")
        new_session_ids = extract_new_session_ids(queue_before, queue_mid)
        if len(new_session_ids) != 1:
            raise RuntimeError("retention-prune expected exactly one new closed session before upload")
        session_id = new_session_ids[0]

        upload_response = response_by_label(responses, "upload_once")
        require_ok(upload_response, Step("upload_once", "debug upload once", "debug upload once"))
        upload_payload = payload_from_response(upload_response)
        if upload_payload.get("session_id") != session_id or upload_payload.get("final_status") != "verified":
            raise RuntimeError("retention-prune expected debug upload once to verify the newly created session")

        verified_entry = queue_entry_map_from_response(queue_after_upload).get(session_id)
        if verified_entry is None:
            raise RuntimeError("retention-prune expected the verified session to appear in queue_after_upload")
        if verified_entry.get("status") != "verified":
            raise RuntimeError("retention-prune expected queue_after_upload status=verified")

        upload_system_log = response_by_label(responses, "system_log_after_upload")
        validate_system_log_response(upload_system_log)
        upload_events = find_events_by_kind_after_seq(
            upload_system_log,
            "upload_verified",
            max_system_log_event_seq(response_by_label(responses, "system_log_before")),
        )
        if not any(
            isinstance(event.get("details"), dict) and event["details"].get("session_id") == session_id
            for event in upload_events
        ):
            raise RuntimeError("retention-prune expected an upload_verified event for the newly created session")

        ref_server_uploads = None
        if ref_server_uploads_url is not None:
            ref_server_doc = http_get_json(ref_server_uploads_url)
            uploads = ref_server_doc.get("uploads") if isinstance(ref_server_doc.get("uploads"), list) else []
            ref_server_uploads = uploads
            if not any(isinstance(upload, dict) and upload.get("session_id") == session_id for upload in uploads):
                raise RuntimeError("retention-prune expected the reference server to accept the verified session before pruning")

        prune_payload = payload_from_response(response_by_label(responses, "prune_once"))
        if int(prune_payload.get("retention_pruned_count", 0)) < 1:
            raise RuntimeError("retention-prune expected retention_pruned_count >= 1")
        if prune_payload.get("reserve_pruned_count") != 0:
            raise RuntimeError("retention-prune expected reserve_pruned_count == 0")
        if prune_payload.get("reserve_met") is not True:
            raise RuntimeError("retention-prune expected reserve_met=true")

        queue_after_prune = response_by_label(responses, "queue_after_prune")
        if session_id in session_ids_from_queue_response(queue_after_prune):
            raise RuntimeError("retention-prune expected the newly verified session to be removed from queue_after_prune")

        system_log_after_prune = response_by_label(responses, "system_log_after_prune")
        system_log_summary = validate_system_log_response(system_log_after_prune)
        pruned_events = find_events_by_kind_after_seq(
            system_log_after_prune,
            "session_pruned",
            max_system_log_event_seq(response_by_label(responses, "system_log_after_upload")),
        )
        if not any(
            isinstance(event.get("details"), dict)
            and event["details"].get("session_id") == session_id
            and event["details"].get("reason") == "retention_expired"
            for event in pruned_events
        ):
            raise RuntimeError("retention-prune expected a session_pruned event for the newly verified session")

        status_after_reboot = payload_from_response(response_by_label(responses, "status_after_reboot"))
        if status_after_reboot.get("mode") != "logging":
            raise RuntimeError("retention-prune expected the device to return to logging mode after reboot")
        if session_id in session_ids_from_queue_response(response_by_label(responses, "queue_after_reboot")):
            raise RuntimeError("retention-prune expected the pruned session to remain absent after reboot")

        return {
            "scenario": "retention-prune",
            "port": str(port),
            "post_reboot_port": post_port,
            "start_utc": format_rfc3339_utc(start_utc),
            "effective_upload_url": effective_upload_url,
            "drained_preamble": drained,
            "post_reboot_drained_preamble": post_drained,
            "responses": responses,
            "stray_responses": stray_responses,
            "started_session_id": session_id,
            "queue_before_session_ids": sorted(session_ids_from_queue_response(response_by_label(responses, "queue_before"))),
            "queue_after_session_ids": sorted(session_ids_from_queue_response(response_by_label(responses, "queue_after_reboot"))),
            "new_session_ids": [session_id],
            "system_log_validation": system_log_summary,
            "final_queue_entry": None,
            "ref_server_uploads": ref_server_uploads,
            "prune_result": prune_payload,
        }


def run_scenario(
    port: Path,
    scenario: str,
    start_utc: datetime,
    *,
    upload_url: str | None = None,
    wifi_ssid: str | None = None,
    wifi_psk: str | None = None,
    upload_api_key: str | None = None,
    upload_token: str | None = None,
    spawn_ref_server: bool = False,
    ref_server_data_dir: Path | None = None,
    ref_server_min_firmware: str | None = None,
    ref_server_origin_url: str | None = None,
    tls_ca_cert: Path | None = None,
    firmware_elf: Path = DEFAULT_FIRMWARE_ELF,
    requeue_temp_build_id: str = DEFAULT_REQUEUE_TEMP_BUILD_ID,
    requeue_temp_build_dir: Path | None = None,
) -> dict[str, Any]:
    if scenario == "config-import":
        if upload_url is None or wifi_ssid is None or wifi_psk is None:
            raise ValueError("config-import requires upload_url, wifi_ssid, and wifi_psk")
        return run_config_import_scenario(
            port,
            start_utc,
            upload_url=upload_url,
            wifi_ssid=wifi_ssid,
            wifi_psk=wifi_psk,
            upload_api_key=upload_api_key,
            upload_token=upload_token,
        )
    if scenario == "https-public-roots":
        if upload_url is None or wifi_ssid is None or wifi_psk is None:
            raise ValueError("https-public-roots requires upload_url, wifi_ssid, and wifi_psk")
        return run_https_trust_scenario(
            port,
            scenario,
            start_utc,
            upload_url=upload_url,
            wifi_ssid=wifi_ssid,
            wifi_psk=wifi_psk,
            upload_api_key=upload_api_key,
            upload_token=upload_token,
            trust_mode=LOGGER_TLS_MODE_PUBLIC_ROOTS,
            spawn_ref_server=spawn_ref_server,
            ref_server_data_dir=ref_server_data_dir,
            ref_server_origin_url=ref_server_origin_url,
            tls_ca_cert=tls_ca_cert,
        )
    if scenario == "https-provisioned-anchor":
        if upload_url is None or wifi_ssid is None or wifi_psk is None:
            raise ValueError("https-provisioned-anchor requires upload_url, wifi_ssid, and wifi_psk")
        return run_https_trust_scenario(
            port,
            scenario,
            start_utc,
            upload_url=upload_url,
            wifi_ssid=wifi_ssid,
            wifi_psk=wifi_psk,
            upload_api_key=upload_api_key,
            upload_token=upload_token,
            trust_mode=LOGGER_TLS_MODE_PROVISIONED_ANCHOR,
            spawn_ref_server=spawn_ref_server,
            ref_server_data_dir=ref_server_data_dir,
            ref_server_origin_url=ref_server_origin_url,
            tls_ca_cert=tls_ca_cert,
        )
    if scenario == "firmware-change-requeue":
        if upload_url is None or wifi_ssid is None or wifi_psk is None:
            raise ValueError("firmware-change-requeue requires upload_url, wifi_ssid, and wifi_psk")
        return run_firmware_change_requeue_scenario(
            port,
            start_utc,
            upload_url=upload_url,
            wifi_ssid=wifi_ssid,
            wifi_psk=wifi_psk,
            upload_api_key=upload_api_key,
            upload_token=upload_token,
            spawn_ref_server=spawn_ref_server,
            ref_server_data_dir=ref_server_data_dir,
            firmware_elf=firmware_elf,
            requeue_temp_build_id=requeue_temp_build_id,
            requeue_temp_build_dir=requeue_temp_build_dir,
        )
    if scenario == "retention-prune":
        if upload_url is None or wifi_ssid is None or wifi_psk is None:
            raise ValueError("retention-prune requires upload_url, wifi_ssid, and wifi_psk")
        return run_retention_prune_scenario(
            port,
            start_utc,
            upload_url=upload_url,
            wifi_ssid=wifi_ssid,
            wifi_psk=wifi_psk,
            upload_api_key=upload_api_key,
            upload_token=upload_token,
            spawn_ref_server=spawn_ref_server,
            ref_server_data_dir=ref_server_data_dir,
        )
    if scenario == "upload-interrupted-reboot":
        return run_interrupted_upload_reboot_scenario(
            port,
            scenario,
            start_utc,
            upload_url=upload_url,
            wifi_ssid=wifi_ssid,
            wifi_psk=wifi_psk,
            upload_api_key=upload_api_key,
            upload_token=upload_token,
            spawn_ref_server=spawn_ref_server,
            ref_server_data_dir=ref_server_data_dir,
            ref_server_min_firmware=ref_server_min_firmware,
        )

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
                        api_key=upload_api_key,
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
            upload_api_key=upload_api_key,
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


def run_interrupted_upload_reboot_scenario(
    port: Path,
    scenario: str,
    start_utc: datetime,
    *,
    upload_url: str | None,
    wifi_ssid: str | None,
    wifi_psk: str | None,
    upload_api_key: str | None,
    upload_token: str | None,
    spawn_ref_server: bool,
    ref_server_data_dir: Path | None,
    ref_server_min_firmware: str | None,
) -> dict[str, Any]:
    if upload_url is None or wifi_ssid is None or wifi_psk is None:
        raise ValueError("upload-interrupted-reboot requires upload_url, wifi_ssid, and wifi_psk")

    effective_upload_url = prepare_upload_url(upload_url, scenario, ref_server_min_firmware)
    ref_server_uploads_url = derive_local_ref_server_uploads_url(effective_upload_url)
    ref_server_healthz_url = derive_local_ref_server_healthz_url(effective_upload_url)

    with ExitStack() as stack:
        if spawn_ref_server:
            if ref_server_data_dir is None:
                stamp = datetime.now(UTC).strftime("%Y%m%d_%H%M%S")
                ref_server_data_dir = Path("build") / f"logger_upload_ref_server_runner_{scenario}_{stamp}"
            ref_server = stack.enter_context(
                RefServerProcess(
                    RefServerConfig(
                        upload_url=effective_upload_url,
                        data_dir=ref_server_data_dir,
                        api_key=upload_api_key,
                        bearer_token=upload_token,
                        min_firmware=ref_server_min_firmware,
                    )
                )
            )
            ref_server_uploads_url = ref_server.uploads_url
            ref_server_healthz_url = ref_server.healthz_url

        steps = build_scenario(
            scenario,
            start_utc,
            upload_url=effective_upload_url,
            wifi_ssid=wifi_ssid,
            wifi_psk=wifi_psk,
            upload_api_key=upload_api_key,
            upload_token=upload_token,
        )

        with SerialJsonClient(port) as client:
            drained, responses, stray_responses = run_steps(client, steps)
            client.drain_input()
            client.write_line("debug upload once")

            request_observed = None
            if ref_server_healthz_url is not None:
                try:
                    request_observed = wait_for_ref_server_upload_start(
                        ref_server_healthz_url,
                        timeout_s=INTERRUPTED_UPLOAD_REQUEST_WAIT_S,
                    )
                except Exception as exc:  # noqa: BLE001
                    request_observed = {"method": "sleep_fallback", "error": str(exc)}
                    time.sleep(5.0)
            else:
                request_observed = {"method": "sleep_fallback", "error": None}
                time.sleep(5.0)

        reset_result = reset_target_via_openocd()
        time.sleep(INTERRUPTED_UPLOAD_REBOOT_SETTLE_S)

        post_steps = [
            Step("status_after_reboot", "status --json", "status", timeout_s=5.0),
            Step("queue_after_reboot", "queue --json", "queue", timeout_s=5.0),
            Step("system_log_after_reboot", "system-log export --json", "system-log export", timeout_s=5.0),
        ]
        post_port, post_drained, post_responses, post_stray_responses = run_steps_after_reboot(
            port,
            post_steps,
            timeout_s=POST_REBOOT_READY_TIMEOUT_S,
        )

        all_responses = responses + post_responses
        all_stray_responses = stray_responses + post_stray_responses
        scenario_analysis = validate_interrupted_upload_recovery(
            all_responses,
            ref_server_uploads_url=ref_server_uploads_url,
        )

        return {
            "scenario": scenario,
            "port": str(port),
            "start_utc": format_rfc3339_utc(start_utc),
            "effective_upload_url": effective_upload_url,
            "drained_preamble": drained,
            "post_reboot_port": post_port,
            "post_reboot_drained_preamble": post_drained,
            "responses": all_responses,
            "stray_responses": all_stray_responses,
            "request_observed": request_observed,
            "reset_result": reset_result,
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
            "config-import",
            "https-public-roots",
            "https-provisioned-anchor",
            "smoke",
            "clock-fix",
            "rollover",
            "no-session-day",
            "upload-verified",
            "upload-failed",
            "upload-blocked-min-firmware",
            "upload-interrupted-reboot",
            "firmware-change-requeue",
            "retention-prune",
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
    parser.add_argument("--upload-url", default=None, help="HTTP or HTTPS upload URL for upload scenarios")
    parser.add_argument("--upload-api-key", default=None, help="upload API key for upload scenarios")
    parser.add_argument("--upload-token", default=None, help="upload bearer token for upload scenarios")
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
        "--ref-server-origin-url",
        default=None,
        help="local http:// origin URL for a spawned reference server when the device upload URL is tunneled HTTPS",
    )
    parser.add_argument(
        "--ref-server-min-firmware",
        default=None,
        help="minimum firmware for a spawned reference server or upload URL preparation",
    )
    parser.add_argument(
        "--tls-ca-cert",
        type=Path,
        default=None,
        help="optional CA certificate or bundle used for host-side HTTPS trust planning",
    )
    parser.add_argument(
        "--firmware-elf",
        type=Path,
        default=DEFAULT_FIRMWARE_ELF,
        help="firmware ELF used to restore the primary build after firmware-change-requeue",
    )
    parser.add_argument(
        "--requeue-temp-build-id",
        default=DEFAULT_REQUEUE_TEMP_BUILD_ID,
        help="temporary build_id used for firmware-change-requeue coverage",
    )
    parser.add_argument(
        "--requeue-temp-build-dir",
        type=Path,
        default=None,
        help="optional build directory for the temporary firmware-change-requeue variant",
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

    if default_upload_auth_args_required(args.scenario):
        missing_args = [
            name
            for name, value in (("--upload-api-key", args.upload_api_key), ("--upload-token", args.upload_token))
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
            upload_api_key=args.upload_api_key,
            upload_token=args.upload_token,
            spawn_ref_server=args.spawn_ref_server,
            ref_server_data_dir=args.ref_server_data_dir,
            ref_server_min_firmware=args.ref_server_min_firmware,
            ref_server_origin_url=args.ref_server_origin_url,
            tls_ca_cert=args.tls_ca_cert,
            firmware_elf=args.firmware_elf,
            requeue_temp_build_id=args.requeue_temp_build_id,
            requeue_temp_build_dir=args.requeue_temp_build_dir,
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
    artifact_validation_supported = args.scenario not in {"config-import", "retention-prune"}

    if args.logger_root is not None and not errors and artifact_validation_supported:
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
    elif args.logger_root is not None and not artifact_validation_supported:
        artifact_validation = {
            "ok": True,
            "skipped": True,
            "reason": f"artifact validation is not applicable to scenario {args.scenario!r}",
        }

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