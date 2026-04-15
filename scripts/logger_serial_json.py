#!/usr/bin/env python3
"""Shared raw-serial JSON transport helpers for logger host tools."""

from __future__ import annotations

import json
import os
import select
import termios
import time
import tty
from pathlib import Path
from typing import Any
from typing import Self


DEFAULT_BAUD = termios.B115200
DEFAULT_COMMAND_DRAIN_QUIET_S = 0.15
DEFAULT_COMMAND_DRAIN_MAX_S = 0.75


class LoggerSerialError(RuntimeError):
    """Raised for shared logger serial transport failures."""


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
        termios.tcflush(fd, termios.TCIFLUSH)
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
            try:
                data = os.read(self.fd, 4096)
            except BlockingIOError:
                continue
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
                raise LoggerSerialError("short write to logger USB serial port")
            offset += written
        termios.tcdrain(self.fd)

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

        stray_commands = [str(item.get("command")) for item in stray[-8:]]
        raise TimeoutError(
            f"timed out waiting for JSON response to {expected_command!r}; "
            f"recent stray commands={stray_commands}"
        )

    def send_command(self, line: str, expected_command: str, *, timeout_s: float = 5.0) -> tuple[dict[str, Any], list[dict[str, Any]]]:
        self.drain_input(
            quiet_time_s=DEFAULT_COMMAND_DRAIN_QUIET_S,
            max_time_s=DEFAULT_COMMAND_DRAIN_MAX_S,
        )
        self.write_line(line)
        return self.read_json(expected_command, timeout_s=timeout_s)