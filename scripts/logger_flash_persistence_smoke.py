#!/usr/bin/env python3

"""Current-format flash-persistence smoke test for logger firmware.

This harness exercises the current v1 config/auth persistence path on real
hardware. It deliberately does not seed old flash layouts or test migrations.

What it checks:

1. a v1 config import with Wi-Fi + upload auth applies successfully,
2. config export reports the expected v1 auth contract,
3. the persisted config survives `fault clear` and a reboot.

Use this on a disposable dev device or one you are comfortable reprovisioning.
It overwrites the active config and does not attempt to restore omitted secrets
from a prior export.
"""

from __future__ import annotations

import argparse
import json
import time
from datetime import UTC, datetime
from pathlib import Path
from typing import Any

try:
    from logger_usb import DEFAULT_CHUNK_SIZE, LoggerDevice, probe_logger_port
except ModuleNotFoundError:  # pragma: no cover - depends on invocation style
    from scripts.logger_usb import DEFAULT_CHUNK_SIZE, LoggerDevice, probe_logger_port


def now_rfc3339_utc() -> str:
    return datetime.now(UTC).replace(microsecond=0).isoformat().replace("+00:00", "Z")


def compact_json(document: dict[str, Any]) -> str:
    return json.dumps(document, separators=(",", ":"), ensure_ascii=False)


def require_mapping(parent: dict[str, Any], key: str) -> dict[str, Any]:
    value = parent.get(key)
    if not isinstance(value, dict):
        raise RuntimeError(f"expected {key!r} to be an object")
    return value


def build_import_document(
    exported_config: dict[str, Any],
    *,
    wifi_ssid: str,
    wifi_psk: str,
    upload_url: str,
    upload_api_key: str,
    upload_token: str,
) -> dict[str, Any]:
    document = dict(exported_config)
    document["exported_at_utc"] = now_rfc3339_utc()
    document["secrets_included"] = True
    document["wifi"] = {
        "allowed_ssids": [wifi_ssid],
        "networks": [{"ssid": wifi_ssid, "psk": wifi_psk}],
    }
    document["upload"] = {
        "enabled": True,
        "url": upload_url,
        "auth": {
            "type": "api_key_and_bearer",
            "api_key": upload_api_key,
            "token": upload_token,
        },
        "tls": {"mode": None, "root_profile": None, "anchor": None},
    }
    return document


def validate_export(
    payload: dict[str, Any],
    *,
    wifi_ssid: str,
    upload_url: str,
) -> dict[str, Any]:
    if payload.get("schema_version") != 1:
        raise RuntimeError("config export schema_version must be 1")
    if payload.get("secrets_included") is not False:
        raise RuntimeError("config export must omit secrets")

    wifi = require_mapping(payload, "wifi")
    networks = wifi.get("networks") if isinstance(wifi.get("networks"), list) else []
    if len(networks) != 1 or not isinstance(networks[0], dict):
        raise RuntimeError("config export wifi.networks must contain one object")
    if networks[0].get("ssid") != wifi_ssid:
        raise RuntimeError("config export wifi.networks[0].ssid mismatch")
    if networks[0].get("psk_present") is not True:
        raise RuntimeError("config export wifi.networks[0].psk_present must be true")

    upload = require_mapping(payload, "upload")
    if upload.get("enabled") is not True:
        raise RuntimeError("config export upload.enabled must be true")
    if upload.get("url") != upload_url:
        raise RuntimeError("config export upload.url mismatch")

    auth = require_mapping(upload, "auth")
    if auth.get("type") != "api_key_and_bearer":
        raise RuntimeError("config export upload.auth.type must be api_key_and_bearer")
    if auth.get("api_key_present") is not True:
        raise RuntimeError("config export upload.auth.api_key_present must be true")
    if auth.get("token_present") is not True:
        raise RuntimeError("config export upload.auth.token_present must be true")

    tls = require_mapping(upload, "tls")
    if tls.get("mode") is not None or tls.get("root_profile") is not None or tls.get("anchor") is not None:
        raise RuntimeError("config export upload.tls must be null-valued for http:// uploads")

    return {
        "schema_version": payload.get("schema_version"),
        "hardware_id": payload.get("hardware_id"),
        "wifi_ssid": wifi_ssid,
        "upload_url": upload_url,
        "upload_auth_type": auth.get("type"),
        "api_key_present": auth.get("api_key_present"),
        "token_present": auth.get("token_present"),
    }


def wait_for_logger_port(preferred_port: Path | None, *, timeout_s: float = 45.0) -> Path:
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        if preferred_port is not None and probe_logger_port(preferred_port, timeout_s=2.0) is not None:
            return preferred_port
        try:
            with LoggerDevice() as device:
                if device.port is not None:
                    return device.port
        except Exception:
            pass
        time.sleep(1.0)
    raise RuntimeError("timed out waiting for logger to reconnect after reboot")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=Path, default=None, help="logger serial port; auto-detect if omitted")
    parser.add_argument("--chunk-size", type=int, default=DEFAULT_CHUNK_SIZE, help="chunk size for config import")
    parser.add_argument("--wifi-ssid", default="smoke-wifi", help="Wi-Fi SSID to provision")
    parser.add_argument("--wifi-psk", default="smoke-password", help="Wi-Fi PSK to provision")
    parser.add_argument("--upload-url", default="http://192.0.2.1/upload", help="upload URL to provision")
    parser.add_argument("--upload-api-key", default="smoke-api-key", help="upload API key to provision")
    parser.add_argument("--upload-token", default="smoke-bearer-token", help="upload bearer token to provision")
    args = parser.parse_args()

    with LoggerDevice(args.port) as device:
        preferred_port = device.port
        export_before = device.command_ok("config export --json", "config export")
        exported_payload = export_before.get("payload")
        if not isinstance(exported_payload, dict):
            raise RuntimeError("config export did not return a payload object")

        import_document = build_import_document(
            exported_payload,
            wifi_ssid=args.wifi_ssid,
            wifi_psk=args.wifi_psk,
            upload_url=args.upload_url,
            upload_api_key=args.upload_api_key,
            upload_token=args.upload_token,
        )
        import_responses = device.import_config_document(compact_json(import_document), chunk_size=args.chunk_size)

        export_after_import = device.command_ok("config export --json", "config export")
        payload_after_import = export_after_import.get("payload")
        if not isinstance(payload_after_import, dict):
            raise RuntimeError("config export after import did not return a payload object")
        after_import_summary = validate_export(
            payload_after_import,
            wifi_ssid=args.wifi_ssid,
            upload_url=args.upload_url,
        )

        fault_clear = device.command_ok("fault clear", "fault clear")
        export_after_fault_clear = device.command_ok("config export --json", "config export")
        payload_after_fault_clear = export_after_fault_clear.get("payload")
        if not isinstance(payload_after_fault_clear, dict):
            raise RuntimeError("config export after fault clear did not return a payload object")
        after_fault_clear_summary = validate_export(
            payload_after_fault_clear,
            wifi_ssid=args.wifi_ssid,
            upload_url=args.upload_url,
        )

        reboot_response = device.command_ok("debug reboot", "debug reboot", timeout_s=10.0)

    reconnected_port = wait_for_logger_port(preferred_port)
    with LoggerDevice(reconnected_port) as device:
        export_after_reboot = device.command_ok("config export --json", "config export")
        payload_after_reboot = export_after_reboot.get("payload")
        if not isinstance(payload_after_reboot, dict):
            raise RuntimeError("config export after reboot did not return a payload object")
        after_reboot_summary = validate_export(
            payload_after_reboot,
            wifi_ssid=args.wifi_ssid,
            upload_url=args.upload_url,
        )

    print(
        json.dumps(
            {
                "ok": True,
                "port": str(reconnected_port),
                "import_final_response": import_responses[-1],
                "fault_clear": fault_clear,
                "reboot": reboot_response,
                "after_import": after_import_summary,
                "after_fault_clear": after_fault_clear_summary,
                "after_reboot": after_reboot_summary,
            },
            indent=2,
            sort_keys=True,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())