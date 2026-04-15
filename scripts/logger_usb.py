#!/usr/bin/env python3
"""USB CDC transport helpers for the logger appliance host tools.

The firmware emits JSON envelopes over a raw serial console rather than a
line-oriented protocol. This module therefore waits for complete JSON objects
and ignores stray non-JSON bytes between responses.

Only the Python standard library is used.
"""

from __future__ import annotations

import glob
import json
import os
import select
import termios
import time
import tty
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Self


DEFAULT_BAUD = termios.B115200
DEFAULT_CHUNK_SIZE = 1200
DEFAULT_IMPORT_TIMEOUT_S = 20.0
DEFAULT_PORT_GLOBS = (
    "/dev/ttyACM*",
    "/dev/ttyUSB*",
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


class SerialJsonClient:
    def __init__(self, port: Path) -> None:
        self.port = Path(port)
        self.fd: int | None = None
        self._saved_termios: list[Any] | None = None

    def __enter__(self) -> Self:
        fd = os.open(self.port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        self.fd = fd
        self._saved_termios = termios.tcgetattr(fd)
        tty.setraw(fd, when=termios.TCSANOW)
        attrs = termios.tcgetattr(fd)
        attrs[4] = DEFAULT_BAUD
        attrs[5] = DEFAULT_BAUD
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

    def write_line(self, line: str) -> None:
        assert self.fd is not None
        data = (line + "\n").encode("utf-8")
        offset = 0
        while offset < len(data):
            try:
                written = os.write(self.fd, data[offset:])
            except BlockingIOError:
                select.select([], [self.fd], [], 0.1)
                continue
            if written <= 0:
                raise LoggerUsbError("short write to logger USB serial port")
            offset += written

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
            try:
                data = os.read(self.fd, 4096)
            except BlockingIOError:
                continue
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

    def send_command(self, line: str, expected_command: str, *, timeout_s: float = 5.0) -> tuple[dict[str, Any], list[dict[str, Any]]]:
        self.write_line(line)
        return self.read_json(expected_command, timeout_s=timeout_s)


def list_serial_port_candidates() -> list[Path]:
    ports: list[Path] = []
    for pattern in DEFAULT_PORT_GLOBS:
        ports.extend(Path(path) for path in glob.glob(pattern))
    return sorted(set(ports))


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
        raise LoggerDiscoveryError("no logger device responded on /dev/ttyACM* or /dev/ttyUSB*")
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
        response, _ = self.client.send_command(line, expected_command, timeout_s=timeout_s)
        return response

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