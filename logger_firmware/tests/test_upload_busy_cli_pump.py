#!/usr/bin/env python3
"""Static guards for the bounded upload-busy CLI pump."""

from __future__ import annotations

import re
from pathlib import Path


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> None:
    firmware_root = Path(__file__).resolve().parents[1]
    include_root = firmware_root / "include" / "logger"
    busy_h = (include_root / "busy_poll.h").read_text(encoding="utf-8")
    service_h = (include_root / "service_cli.h").read_text(encoding="utf-8")
    net_h = (include_root / "net.h").read_text(encoding="utf-8")
    upload_h = (include_root / "upload.h").read_text(encoding="utf-8")
    clock_h = (include_root / "clock.h").read_text(encoding="utf-8")

    app_main_c = (firmware_root / "src" / "app_main.c").read_text(
        encoding="utf-8"
    )
    service_c = (firmware_root / "src" / "service_cli.c").read_text(
        encoding="utf-8"
    )
    upload_c = (firmware_root / "src" / "upload.c").read_text(encoding="utf-8")
    net_c = (firmware_root / "src" / "net.c").read_text(encoding="utf-8")
    clock_c = (firmware_root / "src" / "clock.c").read_text(encoding="utf-8")

    require(
        "logger_busy_poll_t" in busy_h
        and "logger_busy_poll_run" in busy_h
        and "LOGGER_BUSY_POLL_PHASE_UPLOAD_HTTP" in busy_h,
        "busy-poll hook API must exist and cover upload HTTP phase",
    )
    for header, name in [
        (service_h, "service_cli.h"),
        (net_h, "net.h"),
        (upload_h, "upload.h"),
        (clock_h, "clock.h"),
    ]:
        require(
            "logger/busy_poll.h" in header,
            f"{name} must carry the busy-poll hook through blocking paths",
        )

    require(
        "logger_service_cli_poll_upload_busy" in service_h,
        "service CLI must expose the upload-busy bounded pump",
    )
    busy_poll_start = service_c.index("void logger_service_cli_poll_upload_busy")
    busy_poll_impl = service_c[busy_poll_start:]
    require(
        "LOGGER_SERVICE_CLI_BUSY_POLL_CHAR_BUDGET" in busy_poll_impl
        and "LOGGER_SERVICE_CLI_BUSY_POLL_LINE_BUDGET" in busy_poll_impl,
        "upload-busy CLI pump must be explicitly bounded",
    )
    require(
        "logger_service_cli_execute_upload_busy" in busy_poll_impl,
        "upload-busy pump must dispatch through the busy-only executor",
    )
    require(
        "logger_service_cli_execute(cli, app" not in busy_poll_impl,
        "upload-busy pump must not run the normal mutable CLI dispatcher",
    )

    busy_exec_start = service_c.index(
        "static void logger_service_cli_execute_upload_busy"
    )
    busy_exec_end = service_c.index(
        "static void logger_handle_provisioning_status_json", busy_exec_start
    )
    busy_exec = service_c[busy_exec_start:busy_exec_end]
    require(
        'strcmp(line, "service enter") == 0' in busy_exec
        and '"not_permitted_in_mode"' in busy_exec,
        "service enter must get an immediate not_permitted response while upload is busy",
    )
    require(
        'strcmp(line, "status --json") == 0' in busy_exec
        and "logger_handle_status_upload_busy_json" in busy_exec,
        "status --json must get a bounded upload-busy status response",
    )
    require(
        '"busy_upload"' in busy_exec,
        "other commands must receive busy_upload instead of being buffered",
    )

    require(
        "logger_app_upload_busy_poll" in app_main_c
        and "logger_service_cli_poll_upload_busy" in app_main_c,
        "app upload flow must pump upload-busy CLI responses",
    )
    require(
        re.search(
            r"logger_upload_process_session\([^;]*&busy_poll[^;]*result\)",
            app_main_c,
            re.S,
        )
        is not None,
        "automatic upload attempts must pass the busy-poll hook",
    )
    require(
        re.search(
            r"logger_upload_process_one\([^;]*&busy_poll[^;]*result\)",
            service_c,
            re.S,
        )
        is not None,
        "debug upload once must pass the busy-poll hook",
    )

    require(
        "const logger_busy_poll_t *busy_poll" in upload_c
        and re.search(
            r"logger_net_wifi_join\([^;]*busy_poll\)", upload_c, re.S
        )
        and re.search(
            r"logger_upload_http_execute\([^;]*busy_poll[^;]*http_response\)",
            upload_c,
            re.S,
        ),
        "upload process must carry the busy-poll hook through Wi-Fi and HTTP",
    )
    require(
        "LOGGER_BUSY_POLL_PHASE_WIFI_JOIN" in net_c
        and "LOGGER_BUSY_POLL_PHASE_WIFI_DHCP" in net_c,
        "Wi-Fi join/DHCP loops must run the busy-poll hook",
    )
    require(
        "LOGGER_BUSY_POLL_PHASE_UPLOAD_DNS" in upload_c
        and "LOGGER_BUSY_POLL_PHASE_UPLOAD_CONNECT" in upload_c
        and "LOGGER_BUSY_POLL_PHASE_UPLOAD_HTTP" in upload_c,
        "HTTP loop must expose DNS/connect/HTTP busy phases",
    )
    require(
        "LOGGER_BUSY_POLL_PHASE_NTP_DNS" in clock_c
        and "LOGGER_BUSY_POLL_PHASE_NTP_RESPONSE" in clock_c,
        "upload-prep NTP loops must run the busy-poll hook",
    )


if __name__ == "__main__":
    main()
