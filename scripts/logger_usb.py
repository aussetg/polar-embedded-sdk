#!/usr/bin/env python3
"""USB CDC transport helpers for the logger appliance host tools.

The firmware emits JSON envelopes over a raw serial console rather than a
line-oriented protocol. This module therefore waits for complete JSON objects
and ignores stray non-JSON bytes between responses.

Only the Python standard library is used.
"""

from __future__ import annotations

import glob
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Self

try:
    from logger_serial_json import LoggerSerialError
    from logger_serial_json import SerialJsonClient
except ModuleNotFoundError:  # pragma: no cover - import path depends on invocation style
    from scripts.logger_serial_json import LoggerSerialError
    from scripts.logger_serial_json import SerialJsonClient


DEFAULT_CHUNK_SIZE = 1200
DEFAULT_IMPORT_TIMEOUT_S = 20.0
DEFAULT_PORT_GLOBS = (
    "/dev/serial/by-id/*",
    "/dev/ttyACM*",
    "/dev/ttyUSB*",
)
KNOWN_NON_LOGGER_BY_ID_MARKERS = (
    "CMSIS-DAP",
    "Debug_Probe",
    "Pico_Probe",
    "PicoProbe",
    "proxmark",
)
LOGGER_BY_ID_MARKERS = (
    "Raspberry_Pi_Pico",
    "RP2350",
    "RP2040",
    "Pico",
)


class LoggerUsbError(RuntimeError):
    """Base class for USB transport and protocol failures."""


class LoggerDiscoveryError(LoggerUsbError):
    """Raised when logger USB auto-discovery fails."""


class LoggerProtocolError(LoggerUsbError):
    """Raised when the device does not return the expected JSON response."""


class LoggerCommandError(LoggerUsbError):
    """Raised when a device command returns an error envelope."""

    def __init__(self, response: dict[str, Any]) -> None:
        self.response = response
        error = response.get("error")
        if isinstance(error, dict):
            code = error.get("code")
            message = error.get("message")
            super().__init__(f"device command failed: {code}: {message}")
        else:
            super().__init__("device command failed")


@dataclass(frozen=True)
class LoggerPortProbe:
    port: Path
    status_response: dict[str, Any]


def list_serial_port_candidates() -> list[Path]:
    candidates: list[Path] = []
    for pattern in DEFAULT_PORT_GLOBS:
        candidates.extend(Path(path) for path in glob.glob(pattern))

    ignored_devices: set[Path] = set()
    for port in candidates:
        if not port.is_symlink() or "/dev/serial/by-id/" not in str(port):
            continue
        name = port.name
        if any(marker in name for marker in KNOWN_NON_LOGGER_BY_ID_MARKERS):
            ignored_devices.add(port.resolve(strict=False))

    ports_by_device: dict[Path, Path] = {}
    for port in sorted(candidates, key=_serial_port_preference_key):
        device = port.resolve(strict=False)
        if device in ignored_devices:
            continue
        ports_by_device.setdefault(device, port)
    return list(ports_by_device.values())


def _serial_port_preference_key(port: Path) -> tuple[int, str]:
    text = str(port)
    name = port.name
    if "/dev/serial/by-id/" in text:
        if any(marker in name for marker in LOGGER_BY_ID_MARKERS):
            return (0, text)
        return (1, text)
    if text.startswith("/dev/ttyACM"):
        return (2, text)
    if text.startswith("/dev/ttyUSB"):
        return (3, text)
    return (4, text)


def probe_logger_port(port: Path, *, timeout_s: float = 4.0) -> LoggerPortProbe | None:
    try:
        with SerialJsonClient(port) as client:
            client.drain_input()
            response, _ = client.send_command("status --json", "status", timeout_s=timeout_s)
    except Exception:
        return None
    payload = response.get("payload")
    if not isinstance(payload, dict) or "mode" not in payload or "runtime_state" not in payload:
        return None
    return LoggerPortProbe(port=port, status_response=response)


def discover_logger_port(*, timeout_s: float = 4.0) -> Path:
    probes = [probe for port in list_serial_port_candidates() if (probe := probe_logger_port(port, timeout_s=timeout_s))]
    if not probes:
        raise LoggerDiscoveryError("no logger device responded on /dev/serial/by-id/*, /dev/ttyACM*, or /dev/ttyUSB*")
    if len(probes) > 1:
        ports = ", ".join(str(probe.port) for probe in probes)
        raise LoggerDiscoveryError(f"multiple logger devices responded; use --port to choose one: {ports}")
    return probes[0].port


class LoggerDevice:
    def __init__(self, port: Path | str | None = None) -> None:
        self._configured_port = Path(port) if port is not None else None
        self.port: Path | None = self._configured_port
        self._client: SerialJsonClient | None = None

    def __enter__(self) -> Self:
        if self.port is None:
            self.port = discover_logger_port()
        self._client = SerialJsonClient(self.port)
        self._client.__enter__()
        self._client.drain_input()
        return self

    def __exit__(self, exc_type: object, exc: object, tb: object) -> None:
        if self._client is not None:
            self._client.__exit__(exc_type, exc, tb)
        self._client = None

    @property
    def client(self) -> SerialJsonClient:
        if self._client is None:
            raise LoggerUsbError("logger device is not open")
        return self._client

    def command(self, line: str, expected_command: str, *, timeout_s: float = 5.0) -> dict[str, Any]:
        try:
            response, _ = self.client.send_command(line, expected_command, timeout_s=timeout_s)
            return response
        except (LoggerSerialError, TimeoutError, OSError) as exc:
            raise LoggerUsbError(str(exc)) from exc

    def command_ok(self, line: str, expected_command: str, *, timeout_s: float = 5.0) -> dict[str, Any]:
        response = self.command(line, expected_command, timeout_s=timeout_s)
        if not response.get("ok"):
            raise LoggerCommandError(response)
        return response

    def service_unlock(self) -> dict[str, Any]:
        return self.command_ok("service unlock", "service unlock")

    def import_config_document(
        self,
        document: str,
        *,
        chunk_size: int = DEFAULT_CHUNK_SIZE,
        per_command_timeout_s: float = DEFAULT_IMPORT_TIMEOUT_S,
    ) -> list[dict[str, Any]]:
        if not document:
            raise ValueError("config import document must not be empty")
        if "\n" in document or "\r" in document:
            raise ValueError("config import document must be a single line of compact JSON")
        if chunk_size <= 0:
            raise ValueError("chunk_size must be positive")

        responses: list[dict[str, Any]] = []
        self.service_unlock()

        begin_response = self.command_ok(
            f"config import begin {len(document)}",
            "config import begin",
            timeout_s=per_command_timeout_s,
        )
        responses.append(begin_response)

        try:
            for start in range(0, len(document), chunk_size):
                chunk = document[start : start + chunk_size]
                chunk_response = self.command_ok(
                    f"config import chunk {chunk}",
                    "config import chunk",
                    timeout_s=per_command_timeout_s,
                )
                responses.append(chunk_response)

            status_response = self.command_ok(
                "config import status",
                "config import status",
                timeout_s=per_command_timeout_s,
            )
            responses.append(status_response)

            commit_response = self.command_ok(
                "config import commit",
                "config import commit",
                timeout_s=per_command_timeout_s,
            )
            responses.append(commit_response)
            return responses
        except Exception:
            try:
                cancel_response = self.command(
                    "config import cancel",
                    "config import cancel",
                    timeout_s=per_command_timeout_s,
                )
                responses.append(cancel_response)
            except Exception:
                pass
            raise