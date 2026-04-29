#!/usr/bin/env python3
"""Export, inspect, optionally repair, and manually upload a logger bundle.

This is a recovery tool for the service-mode debug bundle export path:

  service enter → service unlock → debug bundle open/read/close

It is intentionally explicit.  By default it only exports and inspects the
device's canonical bundle.  Repair, upload, GCS verification, and local queue
marking require flags.
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import io
import json
import subprocess
import sys
import tarfile
import tempfile
from dataclasses import dataclass
from datetime import UTC, datetime
from pathlib import Path
from typing import Any
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen

try:
    from logger_usb import LoggerCommandError
    from logger_usb import LoggerDevice
except ModuleNotFoundError:  # pragma: no cover - depends on invocation style
    from scripts.logger_usb import LoggerCommandError
    from scripts.logger_usb import LoggerDevice


SESSION_ID_LEN = 32
DEFAULT_OUTPUT_DIR = Path("build/manual_bundle_recovery")


@dataclass(frozen=True)
class BundleParts:
    root_dir: str
    manifest: dict[str, Any]
    manifest_bytes: bytes
    journal_bytes: bytes


def now_rfc3339_utc() -> str:
    return datetime.now(UTC).replace(microsecond=0).isoformat().replace("+00:00", "Z")


def sha256_hex(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def read_json_file(path: Path) -> dict[str, Any]:
    data = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(data, dict):
        raise ValueError(f"expected top-level JSON object in {path}")
    return data


def write_json_file(path: Path, data: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


def compact_json_bytes(data: dict[str, Any]) -> bytes:
    return json.dumps(data, separators=(",", ":"), ensure_ascii=False).encode("utf-8")


def require_session_id(value: str) -> str:
    if len(value) != SESSION_ID_LEN or any(ch not in "0123456789abcdefABCDEF" for ch in value):
        raise argparse.ArgumentTypeError("session_id must be 32 hexadecimal characters")
    return value.lower()


def optional_config_value(config: dict[str, Any], *path: str) -> Any:
    value: Any = config
    for key in path:
        if not isinstance(value, dict):
            return None
        value = value.get(key)
    return value


def required_string(value: Any, label: str) -> str:
    if not isinstance(value, str) or not value:
        raise ValueError(f"missing required string: {label}")
    return value


def command_ok(device: LoggerDevice, line: str, expected: str, timeout_s: float) -> dict[str, Any]:
    return device.command_ok(line, expected, timeout_s=timeout_s)


def export_bundle_from_device(port: str | None, session_id: str, output_tar: Path) -> dict[str, Any]:
    output_tar.parent.mkdir(parents=True, exist_ok=True)
    with LoggerDevice(port) as device:
        command_ok(device, "service enter", "service enter", 20.0)
        command_ok(device, "service unlock", "service unlock", 10.0)
        opened = command_ok(device, f"debug bundle open {session_id}", "debug bundle open", 30.0)
        payload = opened.get("payload") if isinstance(opened.get("payload"), dict) else {}

        total = 0
        try:
            with output_tar.open("wb") as out:
                while True:
                    response = command_ok(device, "debug bundle read", "debug bundle read", 30.0)
                    chunk_payload = response.get("payload") if isinstance(response.get("payload"), dict) else {}
                    data_b64 = chunk_payload.get("data_base64")
                    if not isinstance(data_b64, str):
                        raise RuntimeError("bundle read response did not include data_base64")
                    data = base64.b64decode(data_b64, validate=True)
                    expected_len = chunk_payload.get("len")
                    offset = chunk_payload.get("offset")
                    if expected_len != len(data):
                        raise RuntimeError(f"chunk length mismatch: got {len(data)} expected {expected_len}")
                    if offset != total:
                        raise RuntimeError(f"chunk offset mismatch: got {offset} expected {total}")
                    out.write(data)
                    total += len(data)
                    if chunk_payload.get("eof") is True:
                        break
        finally:
            try:
                device.command("debug bundle close", "debug bundle close", timeout_s=10.0)
            except (LoggerCommandError, Exception):
                pass

    bundle_size = payload.get("bundle_size_bytes")
    if isinstance(bundle_size, int) and bundle_size != total:
        raise RuntimeError(f"exported size mismatch: got {total} expected {bundle_size}")
    return {"open_payload": payload, "exported_size_bytes": total, "sha256": sha256_hex(output_tar.read_bytes())}


def read_bundle_parts(tar_path: Path) -> BundleParts:
    with tarfile.open(tar_path, "r:") as tf:
        members = {member.name: member for member in tf.getmembers()}
        roots = sorted({member.name.split("/", 1)[0] for member in members.values() if member.name and "/" in member.name})
        if len(roots) != 1:
            raise ValueError(f"expected exactly one bundle root directory in {tar_path}, got {roots}")
        root = roots[0]
        manifest_member = members.get(f"{root}/manifest.json")
        journal_member = members.get(f"{root}/journal.bin")
        if manifest_member is None or journal_member is None:
            raise ValueError("bundle must contain manifest.json and journal.bin")
        manifest_file = tf.extractfile(manifest_member)
        journal_file = tf.extractfile(journal_member)
        if manifest_file is None or journal_file is None:
            raise ValueError("failed to read bundle members")
        manifest_bytes = manifest_file.read()
        journal_bytes = journal_file.read()
    manifest = json.loads(manifest_bytes.decode("utf-8"))
    if not isinstance(manifest, dict):
        raise ValueError("manifest top-level value must be an object")
    return BundleParts(root, manifest, manifest_bytes, journal_bytes)


def inspect_bundle(tar_path: Path, *, write_dir: Path | None = None) -> dict[str, Any]:
    parts = read_bundle_parts(tar_path)
    spans = parts.manifest.get("spans") if isinstance(parts.manifest.get("spans"), list) else []
    packet_count = 0
    for span in spans:
        if isinstance(span, dict) and isinstance(span.get("packet_count"), int):
            packet_count += span["packet_count"]
    files = parts.manifest.get("files") if isinstance(parts.manifest.get("files"), list) else []
    journal_sha_manifest = None
    journal_size_manifest = None
    for file_entry in files:
        if isinstance(file_entry, dict) and file_entry.get("name") == "journal.bin":
            journal_sha_manifest = file_entry.get("sha256")
            journal_size_manifest = file_entry.get("size_bytes")
            break

    summary = {
        "tar_path": str(tar_path),
        "tar_size_bytes": tar_path.stat().st_size,
        "tar_sha256": sha256_hex(tar_path.read_bytes()),
        "root_dir": parts.root_dir,
        "manifest_size_bytes": len(parts.manifest_bytes),
        "manifest_sha256": sha256_hex(parts.manifest_bytes),
        "journal_size_bytes": len(parts.journal_bytes),
        "journal_sha256": sha256_hex(parts.journal_bytes),
        "journal_size_matches_manifest": journal_size_manifest == len(parts.journal_bytes),
        "journal_sha256_matches_manifest": journal_sha_manifest == sha256_hex(parts.journal_bytes),
        "session_id": parts.manifest.get("session_id"),
        "study_day_local": parts.manifest.get("study_day_local"),
        "logger_id": parts.manifest.get("logger_id"),
        "subject_id": parts.manifest.get("subject_id"),
        "timezone": parts.manifest.get("timezone"),
        "firmware_version": parts.manifest.get("firmware_version"),
        "packet_count": packet_count,
    }
    if write_dir is not None:
        extracted_dir = write_dir / parts.root_dir
        extracted_dir.mkdir(parents=True, exist_ok=True)
        (extracted_dir / "manifest.json").write_bytes(parts.manifest_bytes)
        (extracted_dir / "journal.bin").write_bytes(parts.journal_bytes)
        write_json_file(write_dir / "inspection.json", summary)
    return summary


def set_if_missing_or_forced(manifest: dict[str, Any], key: str, value: str | None, *, force: bool) -> list[str]:
    if not value:
        return []
    if force or not isinstance(manifest.get(key), str) or not manifest.get(key):
        manifest[key] = value
        return [key]
    return []


def repair_bundle(
    source_tar: Path,
    repaired_tar: Path,
    *,
    config: dict[str, Any] | None,
    logger_id: str | None,
    subject_id: str | None,
    timezone: str | None,
    h10_address: str | None,
    firmware_version: str | None,
    force: bool,
    reason: str,
) -> dict[str, Any]:
    parts = read_bundle_parts(source_tar)
    manifest = json.loads(json.dumps(parts.manifest))

    logger_id = logger_id or optional_config_value(config or {}, "identity", "logger_id")
    subject_id = subject_id or optional_config_value(config or {}, "identity", "subject_id")
    timezone = timezone or optional_config_value(config or {}, "time", "timezone")
    h10_address = h10_address or optional_config_value(config or {}, "recording", "bound_h10_address")

    original_values = {key: manifest.get(key) for key in ("logger_id", "subject_id", "timezone", "firmware_version")}
    repaired_fields: list[str] = []
    repaired_fields += set_if_missing_or_forced(manifest, "logger_id", logger_id, force=force)
    repaired_fields += set_if_missing_or_forced(manifest, "subject_id", subject_id, force=force)
    repaired_fields += set_if_missing_or_forced(manifest, "timezone", timezone, force=force)
    if firmware_version:
        repaired_fields += set_if_missing_or_forced(manifest, "firmware_version", firmware_version, force=force)

    config_snapshot = manifest.get("config_snapshot")
    if isinstance(config_snapshot, dict):
        if timezone and (force or not config_snapshot.get("timezone")):
            config_snapshot["timezone"] = timezone
            repaired_fields.append("config_snapshot.timezone")
        if h10_address and (force or not config_snapshot.get("bound_h10_address")):
            config_snapshot["bound_h10_address"] = h10_address
            repaired_fields.append("config_snapshot.bound_h10_address")

    h10 = manifest.get("h10")
    if isinstance(h10, dict) and h10_address and (force or not h10.get("bound_address")):
        h10["bound_address"] = h10_address
        repaired_fields.append("h10.bound_address")

    files = manifest.get("files")
    if isinstance(files, list):
        for file_entry in files:
            if isinstance(file_entry, dict) and file_entry.get("name") == "journal.bin":
                file_entry["size_bytes"] = len(parts.journal_bytes)
                file_entry["sha256"] = sha256_hex(parts.journal_bytes)

    manifest["local_repair"] = {
        "schema_version": 1,
        "repaired_at_utc": now_rfc3339_utc(),
        "reason": reason,
        "source_bundle_sha256": sha256_hex(source_tar.read_bytes()),
        "source_manifest_sha256": sha256_hex(parts.manifest_bytes),
        "original_values": original_values,
        "repaired_fields": repaired_fields,
        "note": "journal.bin is unchanged; repair performed on host for manual preservation upload",
    }

    manifest_bytes = compact_json_bytes(manifest)
    repaired_tar.parent.mkdir(parents=True, exist_ok=True)
    with tarfile.open(repaired_tar, "w", format=tarfile.USTAR_FORMAT) as tf:
        add_tar_member(tf, f"{parts.root_dir}/", b"", tarfile.DIRTYPE, 0o755)
        add_tar_member(tf, f"{parts.root_dir}/manifest.json", manifest_bytes, tarfile.REGTYPE, 0o644)
        add_tar_member(tf, f"{parts.root_dir}/journal.bin", parts.journal_bytes, tarfile.REGTYPE, 0o644)

    summary = inspect_bundle(repaired_tar)
    summary["repaired_fields"] = repaired_fields
    return summary


def add_tar_member(tf: tarfile.TarFile, name: str, data: bytes, type_: bytes, mode: int) -> None:
    info = tarfile.TarInfo(name)
    info.size = len(data) if type_ == tarfile.REGTYPE else 0
    info.type = type_
    info.mode = mode
    info.uid = 0
    info.gid = 0
    info.uname = ""
    info.gname = ""
    info.mtime = 0
    tf.addfile(info, io.BytesIO(data) if type_ == tarfile.REGTYPE else None)


def upload_bundle(tar_path: Path, config: dict[str, Any]) -> dict[str, Any]:
    parts = read_bundle_parts(tar_path)
    body = tar_path.read_bytes()
    url = required_string(optional_config_value(config, "upload", "url"), "upload.url")
    api_key = required_string(optional_config_value(config, "upload", "auth", "api_key"), "upload.auth.api_key")
    token = required_string(optional_config_value(config, "upload", "auth", "token"), "upload.auth.token")
    headers = {
        "Content-Type": "application/x-tar",
        "x-api-key": api_key,
        "Authorization": f"Bearer {token}",
        "X-Logger-Api-Version": "1",
        "X-Logger-Session-Id": required_string(parts.manifest.get("session_id"), "manifest.session_id"),
        "X-Logger-Hardware-Id": required_string(parts.manifest.get("hardware_id"), "manifest.hardware_id"),
        "X-Logger-Logger-Id": required_string(parts.manifest.get("logger_id"), "manifest.logger_id"),
        "X-Logger-Study-Day": required_string(parts.manifest.get("study_day_local"), "manifest.study_day_local"),
        "X-Logger-SHA256": sha256_hex(body),
        "X-Logger-Tar-Canonicalization-Version": str(parts.manifest.get("tar_canonicalization_version", 1)),
        "X-Logger-Manifest-Schema-Version": str(parts.manifest.get("schema_version", 1)),
    }
    req = Request(url, data=body, headers=headers, method="POST")
    try:
        with urlopen(req, timeout=120) as response:
            response_body = response.read().decode("utf-8", "replace")
            parsed = json.loads(response_body)
            if not isinstance(parsed, dict):
                raise RuntimeError("server response was not a JSON object")
            parsed["http_status"] = response.status
            return parsed
    except HTTPError as exc:
        body_text = exc.read().decode("utf-8", "replace")
        raise RuntimeError(f"upload failed HTTP {exc.code}: {body_text}") from exc
    except URLError as exc:
        raise RuntimeError(f"upload failed: {exc}") from exc


def verify_gcs_object(gcs_uri: str, expected_sha256: str, *, project: str | None) -> dict[str, Any]:
    with tempfile.TemporaryDirectory(prefix="logger-gcs-verify-") as tmpdir:
        tmp_path = Path(tmpdir) / "object.tar"
        cmd = ["gcloud", "storage", "cp", gcs_uri, str(tmp_path)]
        if project:
            cmd.append(f"--project={project}")
        subprocess.run(cmd, check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        actual = sha256_hex(tmp_path.read_bytes())
    if actual != expected_sha256:
        raise RuntimeError(f"GCS object SHA mismatch: got {actual}, expected {expected_sha256}")
    return {"gcs_uri": gcs_uri, "sha256": actual, "verified": True}


def mark_verified_on_device(port: str | None, session_id: str, receipt_id: str, uploaded_sha256: str) -> dict[str, Any]:
    with LoggerDevice(port) as device:
        command_ok(device, "service enter", "service enter", 20.0)
        command_ok(device, "service unlock", "service unlock", 10.0)
        return command_ok(
            device,
            f"debug queue mark-verified {session_id} {receipt_id} {uploaded_sha256}",
            "debug queue mark-verified",
            30.0,
        )


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("session_id", type=require_session_id)
    parser.add_argument("--port", help="serial port; auto-discovered if omitted")
    parser.add_argument("--output-dir", type=Path, default=DEFAULT_OUTPUT_DIR)
    parser.add_argument("--input-tar", type=Path, help="skip device export and use this bundle")
    parser.add_argument("--config", type=Path, help="device config JSON with upload secrets for repair/upload")
    parser.add_argument("--repair", action="store_true", help="write a repaired host-side bundle")
    parser.add_argument("--repair-force", action="store_true", help="overwrite existing manifest values, not only null/empty values")
    parser.add_argument("--repair-reason", default="closed-session manifest repaired during host-side manual recovery")
    parser.add_argument("--logger-id")
    parser.add_argument("--subject-id")
    parser.add_argument("--timezone")
    parser.add_argument("--h10-address")
    parser.add_argument("--firmware-version", help="set/repair manifest firmware_version, e.g. 0.1.0")
    parser.add_argument("--upload", action="store_true", help="POST repaired bundle if --repair, otherwise exported/input bundle")
    parser.add_argument("--verify-gcs-uri", help="download this gs:// object and verify it matches uploaded SHA256")
    parser.add_argument("--gcloud-project", help="project for gcloud storage verification")
    parser.add_argument("--mark-verified", action="store_true", help="mark device queue verified with server receipt_id after upload")
    parser.add_argument("--json", action="store_true", help="print machine-readable summary")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    config = read_json_file(args.config) if args.config else None

    session_dir = args.output_dir / args.session_id
    session_dir.mkdir(parents=True, exist_ok=True)
    exported_tar = session_dir / f"{args.session_id}.exported.tar"
    repaired_tar = session_dir / f"{args.session_id}.repaired.tar"

    summary: dict[str, Any] = {"session_id": args.session_id, "output_dir": str(session_dir), "steps": {}}

    source_tar = args.input_tar or exported_tar
    if args.input_tar is None:
        summary["steps"]["export"] = export_bundle_from_device(args.port, args.session_id, exported_tar)
    else:
        summary["steps"]["export"] = {"skipped": True, "input_tar": str(args.input_tar)}

    summary["steps"]["inspect_source"] = inspect_bundle(source_tar, write_dir=session_dir / "source")
    upload_tar = source_tar

    if args.repair:
        summary["steps"]["repair"] = repair_bundle(
            source_tar,
            repaired_tar,
            config=config,
            logger_id=args.logger_id,
            subject_id=args.subject_id,
            timezone=args.timezone,
            h10_address=args.h10_address,
            firmware_version=args.firmware_version,
            force=args.repair_force,
            reason=args.repair_reason,
        )
        inspect_bundle(repaired_tar, write_dir=session_dir / "repaired")
        upload_tar = repaired_tar
    else:
        summary["steps"]["repair"] = {"skipped": True}

    receipt_id: str | None = None
    uploaded_sha: str | None = None
    if args.upload:
        if config is None:
            raise ValueError("--upload requires --config with upload.auth.api_key and upload.auth.token")
        upload_response = upload_bundle(upload_tar, config)
        summary["steps"]["upload"] = upload_response
        receipt = upload_response.get("receipt_id")
        if isinstance(receipt, str):
            receipt_id = receipt
        sha = upload_response.get("sha256")
        if isinstance(sha, str):
            uploaded_sha = sha
        write_json_file(session_dir / "upload_receipt.json", upload_response)
    else:
        summary["steps"]["upload"] = {"skipped": True}

    if args.verify_gcs_uri:
        expected = uploaded_sha or sha256_hex(upload_tar.read_bytes())
        summary["steps"]["verify_gcs"] = verify_gcs_object(
            args.verify_gcs_uri, expected, project=args.gcloud_project
        )
    else:
        summary["steps"]["verify_gcs"] = {"skipped": True}

    if args.mark_verified:
        if not receipt_id:
            raise ValueError("--mark-verified requires --upload to return receipt_id")
        summary["steps"]["mark_verified"] = mark_verified_on_device(
            args.port, args.session_id, receipt_id, uploaded_sha or sha256_hex(upload_tar.read_bytes())
        )
    else:
        summary["steps"]["mark_verified"] = {"skipped": True}

    write_json_file(session_dir / "summary.json", summary)
    if args.json:
        print(json.dumps(summary, indent=2, ensure_ascii=False))
    else:
        print(f"source: {source_tar}")
        print(f"source_sha256: {summary['steps']['inspect_source']['tar_sha256']}")
        if args.repair:
            print(f"repaired: {repaired_tar}")
            print(f"repaired_sha256: {summary['steps']['repair']['tar_sha256']}")
        if receipt_id:
            print(f"receipt_id: {receipt_id}")
        print(f"summary: {session_dir / 'summary.json'}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"error: {exc}", file=sys.stderr)
        raise SystemExit(1)