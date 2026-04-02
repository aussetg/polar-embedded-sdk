#!/usr/bin/env python3
"""Host-side CLI for the logger appliance.

This tool speaks the private USB serial framing used by the current firmware but
keeps the stable v1 command names and JSON-first behavior exposed to users.

It also provides host-assisted HTTPS endpoint provisioning so the device can be
configured for either:

- built-in curated public roots, or
- one provisioned CA anchor.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from contextlib import nullcontext
from dataclasses import dataclass
from datetime import UTC, datetime
from pathlib import Path
from typing import Any
from urllib.parse import urlparse

try:
    from logger_tls import LOGGER_TLS_MODE_PROVISIONED_ANCHOR
    from logger_tls import LOGGER_TLS_MODE_PUBLIC_ROOTS
    from logger_tls import LoggerTlsError
    from logger_tls import TlsProbeResult
    from logger_tls import TlsTrustPlan
    from logger_tls import plan_logger_tls
except ModuleNotFoundError:  # pragma: no cover - depends on invocation style
    from scripts.logger_tls import LOGGER_TLS_MODE_PROVISIONED_ANCHOR
    from scripts.logger_tls import LOGGER_TLS_MODE_PUBLIC_ROOTS
    from scripts.logger_tls import LoggerTlsError
    from scripts.logger_tls import TlsProbeResult
    from scripts.logger_tls import TlsTrustPlan
    from scripts.logger_tls import plan_logger_tls

try:
    from logger_usb import DEFAULT_CHUNK_SIZE
    from logger_usb import LoggerCommandError
    from logger_usb import LoggerDevice
    from logger_usb import LoggerDiscoveryError
    from logger_usb import LoggerUsbError
except ModuleNotFoundError:  # pragma: no cover - depends on invocation style
    from scripts.logger_usb import DEFAULT_CHUNK_SIZE
    from scripts.logger_usb import LoggerCommandError
    from scripts.logger_usb import LoggerDevice
    from scripts.logger_usb import LoggerDiscoveryError
    from scripts.logger_usb import LoggerUsbError


CONFIG_TOP_LEVEL_KEYS = (
    "schema_version",
    "exported_at_utc",
    "hardware_id",
    "secrets_included",
    "identity",
    "recording",
    "time",
    "battery_policy",
    "wifi",
    "upload",
)
TRUST_MODE_ALIASES = {
    "auto": "auto",
    "public-roots": LOGGER_TLS_MODE_PUBLIC_ROOTS,
    "public_roots": LOGGER_TLS_MODE_PUBLIC_ROOTS,
    "provisioned-anchor": LOGGER_TLS_MODE_PROVISIONED_ANCHOR,
    "provisioned_anchor": LOGGER_TLS_MODE_PROVISIONED_ANCHOR,
}


@dataclass(frozen=True)
class DeviceCommandSpec:
    line: str
    expected_command: str
    timeout_s: float = 5.0


def now_rfc3339_utc() -> str:
    return datetime.now(UTC).replace(microsecond=0).isoformat().replace("+00:00", "Z")


def print_json(document: dict[str, Any]) -> None:
    print(json.dumps(document, indent=2, sort_keys=True))


def read_json_file(path: Path) -> dict[str, Any]:
    data = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(data, dict):
        raise ValueError(f"expected top-level JSON object in {path}")
    return data


def write_json_file(path: Path, document: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(document, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


def compact_json(document: dict[str, Any]) -> str:
    return json.dumps(document, separators=(",", ":"), ensure_ascii=False)


def getenv_optional(name: str | None) -> str | None:
    if name is None:
        return None
    value = os.environ.get(name)
    if value is None:
        raise ValueError(f"environment variable {name!r} is not set")
    return value


def resolve_secret_argument(value: str | None, env_name: str | None) -> str | None:
    if value is not None and env_name is not None:
        raise ValueError("specify either the direct value or the environment variable, not both")
    return value if value is not None else getenv_optional(env_name)


def normalize_trust_mode(value: str) -> str:
    normalized = TRUST_MODE_ALIASES.get(value)
    if normalized is None:
        raise ValueError(f"unsupported trust mode: {value}")
    return normalized


def require_mapping(parent: dict[str, Any], key: str) -> dict[str, Any]:
    value = parent.get(key)
    if not isinstance(value, dict):
        raise ValueError(f"config field {key!r} must be an object")
    return value


def sanitize_export_summary(config: dict[str, Any]) -> dict[str, Any]:
    identity = config.get("identity") if isinstance(config.get("identity"), dict) else {}
    wifi = config.get("wifi") if isinstance(config.get("wifi"), dict) else {}
    upload = config.get("upload") if isinstance(config.get("upload"), dict) else {}
    auth = upload.get("auth") if isinstance(upload.get("auth"), dict) else {}
    tls = sanitize_tls_object(upload.get("tls"))

    psk_present = False
    ssids: list[str] = []
    networks = wifi.get("networks")
    if isinstance(networks, list):
        for network in networks:
            if not isinstance(network, dict):
                continue
            ssid = network.get("ssid")
            if isinstance(ssid, str):
                ssids.append(ssid)
            if isinstance(network.get("psk"), str) or network.get("psk_present") is True:
                psk_present = True

    token_present = isinstance(auth.get("token"), str) or auth.get("token_present") is True
    return {
        "schema_version": config.get("schema_version"),
        "hardware_id": config.get("hardware_id"),
        "identity": {
            "logger_id": identity.get("logger_id"),
            "subject_id": identity.get("subject_id"),
        },
        "wifi": {
            "allowed_ssids": wifi.get("allowed_ssids"),
            "configured_ssids": ssids,
            "psk_present": psk_present,
        },
        "upload": {
            "enabled": upload.get("enabled"),
            "url": upload.get("url"),
            "auth": {
                "type": auth.get("type"),
                "token_present": token_present,
            },
            "tls": tls,
        },
    }


def ensure_config_shape(config: dict[str, Any]) -> None:
    missing = [key for key in CONFIG_TOP_LEVEL_KEYS if key not in config]
    if missing:
        raise ValueError(f"config is missing required top-level keys: {', '.join(missing)}")
    if config.get("schema_version") != 1:
        raise ValueError("config schema_version must be 1")


def sanitize_tls_object(value: Any) -> dict[str, Any] | None:
    if not isinstance(value, dict):
        return None
    anchor = value.get("anchor")
    sanitized_anchor: dict[str, Any] | None
    if isinstance(anchor, dict):
        sanitized_anchor = {
            key: anchor.get(key)
            for key in ("format", "sha256", "subject")
            if key in anchor
        }
    else:
        sanitized_anchor = None
    return {
        "mode": value.get("mode"),
        "root_profile": value.get("root_profile"),
        "anchor": sanitized_anchor,
    }


def find_hidden_secret_markers(config: dict[str, Any]) -> list[str]:
    markers: list[str] = []

    wifi = require_mapping(config, "wifi")
    networks = wifi.get("networks")
    if isinstance(networks, list):
        for index, network in enumerate(networks):
            if isinstance(network, dict) and network.get("psk_present") is True and "psk" not in network:
                markers.append(f"wifi.networks[{index}].psk")

    upload = require_mapping(config, "upload")
    auth = require_mapping(upload, "auth")
    if auth.get("token_present") is True and "token" not in auth:
        markers.append("upload.auth.token")

    return markers


def apply_wifi_settings(config: dict[str, Any], *, ssid: str | None, psk: str | None) -> dict[str, Any]:
    wifi = require_mapping(config, "wifi")
    networks = wifi.get("networks")
    if not isinstance(networks, list):
        raise ValueError("config wifi.networks must be an array")
    if len(networks) > 1:
        raise ValueError("current firmware supports exactly one Wi-Fi network entry")

    existing = networks[0] if networks else {}
    if existing and not isinstance(existing, dict):
        raise ValueError("config wifi.networks[0] must be an object")
    network = dict(existing)

    effective_ssid = ssid or network.get("ssid")
    if effective_ssid is None and psk is not None:
        raise ValueError("--wifi-psk requires an SSID via --wifi-ssid or existing config")

    if effective_ssid is not None:
        network["ssid"] = effective_ssid
        wifi["allowed_ssids"] = [effective_ssid]
    elif not isinstance(wifi.get("allowed_ssids"), list):
        wifi["allowed_ssids"] = []

    if psk is not None:
        network["psk"] = psk
        network.pop("psk_present", None)

    if network:
        if "ssid" not in network:
            raise ValueError("Wi-Fi network entry is missing ssid")
        wifi["networks"] = [network]
    else:
        wifi["networks"] = []

    return {
        "ssid": network.get("ssid"),
        "psk_present": isinstance(network.get("psk"), str) or network.get("psk_present") is True,
        "allowed_ssids": wifi.get("allowed_ssids"),
    }


def apply_upload_auth_settings(
    config: dict[str, Any],
    *,
    bearer_token: str | None,
    clear_bearer_token: bool,
) -> dict[str, Any]:
    upload = require_mapping(config, "upload")
    auth = require_mapping(upload, "auth")

    if clear_bearer_token and bearer_token is not None:
        raise ValueError("--clear-bearer-token cannot be combined with --bearer-token")
    if clear_bearer_token:
        upload["auth"] = {"type": "none"}
    elif bearer_token is not None:
        upload["auth"] = {"type": "bearer", "token": bearer_token}
    else:
        upload["auth"] = dict(auth)

    final_auth = require_mapping(upload, "auth")
    return {
        "type": final_auth.get("type"),
        "token_present": isinstance(final_auth.get("token"), str) or final_auth.get("token_present") is True,
    }


def apply_upload_endpoint_settings(
    config: dict[str, Any],
    *,
    url: str,
    trust_plan: TlsTrustPlan | None,
) -> dict[str, Any]:
    upload = require_mapping(config, "upload")
    upload["enabled"] = True
    upload["url"] = url

    parsed = urlparse(url)
    if parsed.scheme == "https":
        if trust_plan is None:
            raise ValueError("HTTPS upload configuration requires a TLS trust plan")
        upload["tls"] = {
            "mode": trust_plan.mode,
            "root_profile": trust_plan.root_profile,
            "anchor": trust_plan.anchor,
        }
    elif parsed.scheme == "http":
        upload["tls"] = {"mode": None, "root_profile": None, "anchor": None}
    else:
        raise ValueError("upload URL must use http:// or https://")

    return {
        "enabled": upload.get("enabled"),
        "url": upload.get("url"),
        "tls": sanitize_tls_object(upload.get("tls")),
    }


def finalize_secrets_flag(config: dict[str, Any]) -> bool:
    wifi = require_mapping(config, "wifi")
    networks = wifi.get("networks")
    upload = require_mapping(config, "upload")
    auth = require_mapping(upload, "auth")
    has_wifi_secret = any(isinstance(network, dict) and isinstance(network.get("psk"), str) for network in networks or [])
    has_token_secret = isinstance(auth.get("token"), str)
    config["secrets_included"] = has_wifi_secret or has_token_secret
    return bool(config["secrets_included"])


def sanitize_tls_plan(plan: TlsTrustPlan | None) -> dict[str, Any] | None:
    if plan is None:
        return None
    return {
        "mode": plan.mode,
        "root_profile": plan.root_profile,
        "reason": plan.reason,
        "selected_anchor_sha256": plan.selected_anchor_sha256,
        "selected_anchor_subject": plan.selected_anchor_subject,
    }


def sanitize_tls_probe(probe: TlsProbeResult | None) -> dict[str, Any] | None:
    if probe is None:
        return None
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


def handle_device_command(args: argparse.Namespace) -> int:
    with LoggerDevice(args.port) as device:
        response = device.command(
            args.command_spec.line,
            args.command_spec.expected_command,
            timeout_s=args.command_spec.timeout_s,
        )
        if getattr(args, "output", None) is not None and response.get("ok"):
            payload = response.get("payload")
            if not isinstance(payload, dict):
                raise ValueError("device response did not include an object payload to export")
            write_json_file(args.output, payload)
        print_json(response)
        return 0 if response.get("ok") else 1


def handle_config_import(args: argparse.Namespace) -> int:
    config = read_json_file(args.path)
    ensure_config_shape(config)
    compact = compact_json(config)

    with LoggerDevice(args.port) as device:
        responses = device.import_config_document(compact, chunk_size=args.chunk_size)
        summary = {
            "port": str(device.port),
            "config_path": str(args.path),
            "document_bytes": len(compact),
            "chunk_size": args.chunk_size,
            "responses": responses,
            "final_response": responses[-1],
        }
    print_json(summary)
    return 0


def load_base_config(device: LoggerDevice | None, path: Path | None) -> tuple[dict[str, Any], str]:
    if path is not None:
        config = read_json_file(path)
        ensure_config_shape(config)
        return config, f"file:{path}"
    if device is None:
        raise ValueError("a live device is required when --config is omitted")
    response = device.command_ok("config export --json", "config export")
    payload = response.get("payload")
    if not isinstance(payload, dict):
        raise ValueError("config export returned an invalid payload")
    ensure_config_shape(payload)
    return payload, "device:config-export"


def handle_upload_configure(args: argparse.Namespace) -> int:
    wifi_psk = resolve_secret_argument(args.wifi_psk, args.wifi_psk_env)
    bearer_token = resolve_secret_argument(args.bearer_token, args.bearer_token_env)

    parsed = urlparse(args.url)
    if parsed.scheme not in {"http", "https"} or not parsed.netloc:
        raise ValueError("upload URL must be an absolute http:// or https:// URL")
    if parsed.scheme == "http" and args.ca_cert is not None:
        raise ValueError("--ca-cert is only valid for https:// upload URLs")

    device_cm: Any = LoggerDevice(args.port) if args.apply or args.config is None else nullcontext(None)
    with device_cm as maybe_device:
        device = maybe_device if isinstance(maybe_device, LoggerDevice) else None
        config, source = load_base_config(device, args.config)

        probe: TlsProbeResult | None = None
        trust_plan: TlsTrustPlan | None = None
        if parsed.scheme == "https":
            probe, trust_plan = plan_logger_tls(
                args.url,
                trust_mode=normalize_trust_mode(args.trust_mode),
                ca_cert_path=args.ca_cert,
                timeout_s=args.timeout,
            )
        elif normalize_trust_mode(args.trust_mode) != "auto":
            raise ValueError("--trust-mode is only meaningful for https:// upload URLs")

        config["exported_at_utc"] = now_rfc3339_utc()
        wifi_summary = apply_wifi_settings(config, ssid=args.wifi_ssid, psk=wifi_psk)
        auth_summary = apply_upload_auth_settings(config, bearer_token=bearer_token, clear_bearer_token=args.clear_bearer_token)
        upload_summary = apply_upload_endpoint_settings(config, url=args.url, trust_plan=trust_plan)
        secrets_included = finalize_secrets_flag(config)
        hidden_markers = find_hidden_secret_markers(config)

        output_path: str | None = None
        if args.output is not None:
            write_json_file(args.output, config)
            output_path = str(args.output)

        apply_summary: dict[str, Any] | None = None
        post_apply_export: dict[str, Any] | None = None
        net_test_response: dict[str, Any] | None = None
        if args.apply:
            if device is None:
                raise ValueError("internal error: missing device handle for apply")
            if hidden_markers:
                raise ValueError(
                    "refusing to apply config that still contains hidden-secret markers from export-only data: "
                    + ", ".join(hidden_markers)
                    + "; provide the secret values explicitly"
                )
            responses = device.import_config_document(compact_json(config), chunk_size=args.chunk_size)
            apply_summary = {
                "responses": responses,
                "final_response": responses[-1],
            }
            export_response = device.command_ok("config export --json", "config export")
            export_payload = export_response.get("payload")
            if not isinstance(export_payload, dict):
                raise ValueError("config export after apply returned an invalid payload")
            post_apply_export = sanitize_export_summary(export_payload)
            if args.verify_net_test:
                net_test_response = device.command("net-test --json", "net-test", timeout_s=args.timeout)

    summary = {
        "config_source": source,
        "output_path": output_path,
        "applied": args.apply,
        "port": str(device.port) if device is not None else None,
        "upload_url": args.url,
        "wifi": wifi_summary,
        "auth": auth_summary,
        "upload": upload_summary,
        "secrets_included": secrets_included,
        "hidden_secret_markers": hidden_markers,
        "import_ready": not hidden_markers,
        "tls_probe": sanitize_tls_probe(probe),
        "tls_plan": sanitize_tls_plan(trust_plan),
        "post_apply_config": post_apply_export,
        "apply": apply_summary,
        "net_test": net_test_response,
    }
    print_json(summary)
    return 0 if net_test_response is None or net_test_response.get("ok") else 1


def add_json_flag(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--json", action="store_true", help="accepted for compatibility; JSON is always printed")


def add_device_command_parser(
    subparsers: argparse._SubParsersAction[argparse.ArgumentParser],
    name: str,
    *,
    help_text: str,
    spec: DeviceCommandSpec,
) -> argparse.ArgumentParser:
    parser = subparsers.add_parser(name, help=help_text)
    add_json_flag(parser)
    parser.set_defaults(handler=handle_device_command, command_spec=spec)
    return parser


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=Path, help="logger USB serial device (auto-detect if omitted)")
    subparsers = parser.add_subparsers(dest="command", required=True)

    add_device_command_parser(
        subparsers,
        "status",
        help_text="read the stable overall status summary",
        spec=DeviceCommandSpec("status --json", "status"),
    )
    add_device_command_parser(
        subparsers,
        "provisioning-status",
        help_text="read the provisioning completeness summary",
        spec=DeviceCommandSpec("provisioning-status --json", "provisioning-status"),
    )
    add_device_command_parser(
        subparsers,
        "queue",
        help_text="read the upload queue summary",
        spec=DeviceCommandSpec("queue --json", "queue"),
    )
    add_device_command_parser(
        subparsers,
        "preflight",
        help_text="run the firmware preflight checks",
        spec=DeviceCommandSpec("preflight --json", "preflight", timeout_s=15.0),
    )
    add_device_command_parser(
        subparsers,
        "net-test",
        help_text="run Wi-Fi and upload endpoint reachability checks",
        spec=DeviceCommandSpec("net-test --json", "net-test", timeout_s=120.0),
    )
    add_device_command_parser(
        subparsers,
        "factory-reset",
        help_text="issue factory-reset",
        spec=DeviceCommandSpec("factory-reset", "factory-reset", timeout_s=20.0),
    )

    config_parser = subparsers.add_parser("config", help="export or import persisted config")
    config_subparsers = config_parser.add_subparsers(dest="config_command", required=True)
    config_export = add_device_command_parser(
        config_subparsers,
        "export",
        help_text="read the config JSON object",
        spec=DeviceCommandSpec("config export --json", "config export"),
    )
    config_export.add_argument("--output", type=Path, help="write the config payload object to a JSON file")

    config_import = config_subparsers.add_parser("import", help="import a config JSON file using staged transport")
    config_import.add_argument("path", type=Path, help="path to the raw config JSON object")
    config_import.add_argument("--chunk-size", type=int, default=DEFAULT_CHUNK_SIZE, help="chunk size for staged import")
    add_json_flag(config_import)
    config_import.set_defaults(handler=handle_config_import)

    service_parser = subparsers.add_parser("service", help="service-mode operations")
    service_subparsers = service_parser.add_subparsers(dest="service_command", required=True)
    add_device_command_parser(
        service_subparsers,
        "unlock",
        help_text="arm the dangerous-operation window",
        spec=DeviceCommandSpec("service unlock", "service unlock"),
    )

    fault_parser = subparsers.add_parser("fault", help="fault operations")
    fault_subparsers = fault_parser.add_subparsers(dest="fault_command", required=True)
    add_device_command_parser(
        fault_subparsers,
        "clear",
        help_text="clear the current latched fault if allowed",
        spec=DeviceCommandSpec("fault clear", "fault clear", timeout_s=10.0),
    )

    sd_parser = subparsers.add_parser("sd", help="SD card operations")
    sd_subparsers = sd_parser.add_subparsers(dest="sd_command", required=True)
    add_device_command_parser(
        sd_subparsers,
        "format",
        help_text="reformat the SD card for logger use",
        spec=DeviceCommandSpec("sd format", "sd format", timeout_s=60.0),
    )

    system_log_parser = subparsers.add_parser("system-log", help="system log operations")
    system_log_subparsers = system_log_parser.add_subparsers(dest="system_log_command", required=True)
    system_log_export = add_device_command_parser(
        system_log_subparsers,
        "export",
        help_text="read the structured system log export",
        spec=DeviceCommandSpec("system-log export --json", "system-log export", timeout_s=15.0),
    )
    system_log_export.add_argument("--output", type=Path, help="write the system-log payload object to a JSON file")

    upload_parser = subparsers.add_parser("upload", help="upload and HTTPS trust operations")
    upload_subparsers = upload_parser.add_subparsers(dest="upload_command", required=True)

    configure_parser = upload_subparsers.add_parser(
        "configure",
        help="host-assisted upload endpoint provisioning and optional config apply",
    )
    configure_parser.add_argument("url", help="absolute http:// or https:// upload URL")
    configure_parser.add_argument("--config", type=Path, help="base config JSON file; default is live config export")
    configure_parser.add_argument("--output", type=Path, help="write the generated config JSON to a file")
    configure_parser.add_argument("--apply", action="store_true", help="import the generated config onto the device")
    configure_parser.add_argument("--verify-net-test", action="store_true", help="run net-test after a successful apply")
    configure_parser.add_argument("--chunk-size", type=int, default=DEFAULT_CHUNK_SIZE, help="chunk size for staged import")
    configure_parser.add_argument("--timeout", type=float, default=30.0, help="TLS probe and net-test timeout in seconds")
    configure_parser.add_argument(
        "--trust-mode",
        choices=tuple(TRUST_MODE_ALIASES),
        default="auto",
        help="HTTPS trust planning policy",
    )
    configure_parser.add_argument("--ca-cert", type=Path, help="CA certificate or bundle for private HTTPS endpoints")
    configure_parser.add_argument("--wifi-ssid", help="set or replace the single configured Wi-Fi SSID")
    wifi_psk_group = configure_parser.add_mutually_exclusive_group()
    wifi_psk_group.add_argument("--wifi-psk", help="set the Wi-Fi PSK directly")
    wifi_psk_group.add_argument("--wifi-psk-env", help="read the Wi-Fi PSK from an environment variable")
    token_group = configure_parser.add_mutually_exclusive_group()
    token_group.add_argument("--bearer-token", help="set the upload bearer token directly")
    token_group.add_argument("--bearer-token-env", help="read the bearer token from an environment variable")
    configure_parser.add_argument(
        "--clear-bearer-token",
        action="store_true",
        help="replace bearer auth with auth.type=none",
    )
    add_json_flag(configure_parser)
    configure_parser.set_defaults(handler=handle_upload_configure)

    tls_parser = upload_subparsers.add_parser("tls", help="upload TLS trust helpers")
    tls_subparsers = tls_parser.add_subparsers(dest="upload_tls_command", required=True)
    add_device_command_parser(
        tls_subparsers,
        "clear-provisioned-anchor",
        help_text="erase the provisioned upload CA anchor from the device",
        spec=DeviceCommandSpec(
            "upload tls clear-provisioned-anchor",
            "upload tls clear-provisioned-anchor",
            timeout_s=15.0,
        ),
    )

    return parser


def main() -> int:
    parser = build_arg_parser()
    args = parser.parse_args()
    try:
        return args.handler(args)
    except LoggerCommandError as exc:
        print_json(exc.response)
        return 1
    except (FileNotFoundError, LoggerDiscoveryError, LoggerTlsError, LoggerUsbError, ValueError, json.JSONDecodeError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())