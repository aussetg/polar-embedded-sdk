#!/usr/bin/env python3
"""Validate closed-session artifacts and upload queue state.

This is a host-side developer tool for the logger_firmware v1 storage contract.
It validates:

- closed session directories containing ``manifest.json`` + ``journal.bin``,
- ``upload_queue.json`` structural and ordering rules,
- manifest ↔ journal consistency,
- queue ↔ local closed-session consistency,
- canonical bundle hash/size derivation for local sessions.

The tool intentionally uses only the Python standard library.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
from dataclasses import dataclass, field
from datetime import datetime
from pathlib import Path
from typing import Any

try:
    from logger_journal_validate import parse_journal
except ModuleNotFoundError:  # pragma: no cover - import path depends on invocation style
    from scripts.logger_journal_validate import parse_journal


QUEUE_STATUS_VALUES = {
    "pending",
    "uploading",
    "verified",
    "blocked_min_firmware",
    "failed",
}


def is_hex(value: Any, length: int) -> bool:
    return isinstance(value, str) and len(value) == length and all(ch in "0123456789abcdef" for ch in value)


def is_rfc3339_utc(value: Any) -> bool:
    if not isinstance(value, str) or not value.endswith("Z"):
        return False
    try:
        datetime.fromisoformat(value[:-1] + "+00:00")
    except ValueError:
        return False
    return True


def is_utc_or_null(value: Any) -> bool:
    return value is None or is_rfc3339_utc(value)


def load_json_file(path: Path) -> Any:
    return json.loads(path.read_text(encoding="utf-8"))


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        while True:
            chunk = handle.read(65536)
            if not chunk:
                break
            digest.update(chunk)
    return digest.hexdigest()


def resolve_logger_root(path: Path) -> Path | None:
    candidate = path if path.is_dir() else path.parent
    if (candidate / "state").is_dir() and (candidate / "sessions").is_dir():
        return candidate
    if (candidate / "logger" / "state").is_dir() and (candidate / "logger" / "sessions").is_dir():
        return candidate / "logger"
    return None


def _microtar_write_string(header: bytearray, start: int, width: int, value: str) -> None:
    encoded = value.encode("utf-8")
    if len(encoded) >= width:
        raise ValueError(f"tar field too long: {value!r}")
    header[start : start + len(encoded)] = encoded


def _microtar_write_octal(header: bytearray, start: int, width: int, value: int) -> None:
    encoded = format(value, "o").encode("ascii")
    if len(encoded) + 1 > width:
        raise ValueError(f"tar octal field too long for value {value}")
    header[start : start + len(encoded)] = encoded
    header[start + len(encoded)] = 0


def _microtar_header(name: str, mode: int, size: int, typeflag: str) -> bytes:
    header = bytearray(512)
    _microtar_write_string(header, 0, 100, name)
    _microtar_write_octal(header, 100, 8, mode)
    _microtar_write_octal(header, 108, 8, 0)
    _microtar_write_octal(header, 124, 12, size)
    _microtar_write_octal(header, 136, 12, 0)
    header[156] = ord(typeflag)
    checksum = sum(header) + (8 * 32)
    checksum_text = f"{checksum:06o}".encode("ascii")
    header[148:154] = checksum_text
    header[154] = 0
    header[155] = ord(" ")
    return bytes(header)


def _tar_padding(size: int) -> bytes:
    return b"\0" * ((512 - (size % 512)) % 512)


def compute_bundle_identity(dir_name: str, manifest_path: Path, journal_path: Path) -> tuple[str, int]:
    manifest_bytes = manifest_path.read_bytes()
    digest = hashlib.sha256()
    total = 0

    def update(data: bytes) -> None:
        nonlocal total
        digest.update(data)
        total += len(data)

    update(_microtar_header(f"{dir_name}/", 0o755, 0, "5"))

    update(_microtar_header(f"{dir_name}/manifest.json", 0o644, len(manifest_bytes), "0"))
    update(manifest_bytes)
    update(_tar_padding(len(manifest_bytes)))

    journal_size = journal_path.stat().st_size
    update(_microtar_header(f"{dir_name}/journal.bin", 0o644, journal_size, "0"))
    with journal_path.open("rb") as handle:
        while True:
            chunk = handle.read(65536)
            if not chunk:
                break
            update(chunk)
    update(_tar_padding(journal_size))
    update(b"\0" * 1024)
    return digest.hexdigest(), total


@dataclass
class Report:
    kind: str
    path: str
    errors: list[str] = field(default_factory=list)
    warnings: list[str] = field(default_factory=list)
    details: dict[str, Any] = field(default_factory=dict)

    def error(self, message: str) -> None:
        self.errors.append(message)

    def warn(self, message: str) -> None:
        self.warnings.append(message)

    def to_dict(self) -> dict[str, Any]:
        return {
            "kind": self.kind,
            "path": self.path,
            **self.details,
            "errors": self.errors,
            "warnings": self.warnings,
            "ok": not self.errors,
        }


def require_mapping(value: Any, report: Report, context: str) -> dict[str, Any] | None:
    if not isinstance(value, dict):
        report.error(f"{context} must be a JSON object")
        return None
    return value


def require_list(value: Any, report: Report, context: str) -> list[Any] | None:
    if not isinstance(value, list):
        report.error(f"{context} must be an array")
        return None
    return value


def require_keys(mapping: dict[str, Any], keys: list[str], report: Report, context: str) -> None:
    for key in keys:
        if key not in mapping:
            report.error(f"missing {context}.{key}")


def closed_session_dirs(logger_root: Path) -> list[Path]:
    sessions_dir = logger_root / "sessions"
    result: list[Path] = []
    if not sessions_dir.is_dir():
        return result
    for child in sorted(sessions_dir.iterdir()):
        if not child.is_dir() or child.name.endswith(".tmp"):
            continue
        if (child / "live.json").exists():
            continue
        if (child / "manifest.json").is_file() and (child / "journal.bin").is_file():
            result.append(child)
    return result


def validate_manifest_against_journal(
    report: Report,
    manifest: dict[str, Any],
    journal_summary: dict[str, Any],
) -> None:
    session = manifest.get("session")
    spans = manifest.get("spans")
    if not isinstance(session, dict) or not isinstance(spans, list):
        return

    if manifest.get("session_id") != journal_summary.get("session_id"):
        report.error("manifest.session_id does not match journal header session_id")

    session_start = journal_summary.get("session_start")
    if isinstance(session_start, dict):
        for key in ("study_day_local", "logger_id", "subject_id", "timezone"):
            if manifest.get(key) != session_start.get(key):
                report.error(f"manifest.{key} does not match journal session_start.{key}")
        if session.get("start_reason") != session_start.get("start_reason"):
            report.error("manifest.session.start_reason does not match journal session_start.start_reason")

    session_end = journal_summary.get("session_end")
    if isinstance(session_end, dict):
        if session.get("end_reason") != session_end.get("end_reason"):
            report.error("manifest.session.end_reason does not match journal session_end.end_reason")
        if session.get("span_count") != session_end.get("span_count"):
            report.error("manifest.session.span_count does not match journal session_end.span_count")
        if session.get("quarantined") != session_end.get("quarantined"):
            report.error("manifest.session.quarantined does not match journal session_end.quarantined")
        if session.get("quarantine_reasons") != session_end.get("quarantine_reasons"):
            report.error(
                "manifest.session.quarantine_reasons does not match journal session_end.quarantine_reasons"
            )

    record_counts = journal_summary.get("record_counts", {})
    if record_counts.get("session_start") != 1:
        report.error("journal must contain exactly one session_start record")
    if record_counts.get("session_end") != 1:
        report.error("journal must contain exactly one session_end record")

    span_summaries = journal_summary.get("span_summaries", {})
    span_records = journal_summary.get("span_records", {})
    if len(spans) != len(span_summaries):
        report.error("manifest span count does not match journal span count")
        return

    for index, span in enumerate(spans):
        if not isinstance(span, dict):
            continue
        span_id = span.get("span_id")
        if not isinstance(span_id, str):
            continue
        journal_span = span_summaries.get(span_id)
        if not isinstance(journal_span, dict):
            report.error(f"manifest span {span_id} is missing from journal")
            continue
        if span.get("packet_count") != journal_span.get("packet_count"):
            report.error(f"manifest span {span_id} packet_count does not match journal")
        if span.get("packet_count"):
            if span.get("first_seq_in_span") != journal_span.get("first_seq_in_span"):
                report.error(f"manifest span {span_id} first_seq_in_span does not match journal")
            if span.get("last_seq_in_span") != journal_span.get("last_seq_in_span"):
                report.error(f"manifest span {span_id} last_seq_in_span does not match journal")
        journal_span_record = span_records.get(span_id)
        if isinstance(journal_span_record, dict):
            if span.get("index_in_session") != journal_span_record.get("index_in_session"):
                report.error(f"manifest span {span_id} index_in_session does not match journal")
            if span.get("start_reason") != journal_span_record.get("start_reason"):
                report.error(f"manifest span {span_id} start_reason does not match journal")
            if span.get("end_reason") != journal_span_record.get("end_reason"):
                report.error(f"manifest span {span_id} end_reason does not match journal")
        elif index >= len(span_summaries):
            report.error(f"manifest span {span_id} has no journal span metadata")


def validate_session_dir(session_dir: Path) -> dict[str, Any]:
    report = Report(kind="session_dir", path=str(session_dir))
    manifest_path = session_dir / "manifest.json"
    journal_path = session_dir / "journal.bin"
    live_path = session_dir / "live.json"

    if not session_dir.is_dir():
        report.error("session directory does not exist")
        return report.to_dict()
    if live_path.exists():
        report.error("closed session directory must not contain live.json")
    if not manifest_path.is_file():
        report.error("manifest.json is missing")
        return report.to_dict()
    if not journal_path.is_file():
        report.error("journal.bin is missing")
        return report.to_dict()

    try:
        manifest_raw = load_json_file(manifest_path)
    except Exception as exc:  # noqa: BLE001
        report.error(f"failed to parse manifest.json: {exc}")
        return report.to_dict()

    manifest = require_mapping(manifest_raw, report, "manifest")
    if manifest is None:
        return report.to_dict()

    require_keys(
        manifest,
        [
            "schema_version",
            "session_id",
            "study_day_local",
            "logger_id",
            "subject_id",
            "hardware_id",
            "firmware_version",
            "build_id",
            "journal_format_version",
            "tar_canonicalization_version",
            "timezone",
            "session",
            "spans",
            "config_snapshot",
            "h10",
            "storage",
            "files",
            "upload_bundle",
        ],
        report,
        "manifest",
    )

    if manifest.get("schema_version") != 1:
        report.error("manifest.schema_version must be 1")
    if manifest.get("journal_format_version") != 1:
        report.error("manifest.journal_format_version must be 1")
    if manifest.get("tar_canonicalization_version") != 1:
        report.error("manifest.tar_canonicalization_version must be 1")

    session_id = manifest.get("session_id")
    study_day_local = manifest.get("study_day_local")
    if not is_hex(session_id, 32):
        report.error("manifest.session_id must be 32 lowercase hex characters")
    if not isinstance(study_day_local, str):
        report.error("manifest.study_day_local must be a string")

    expected_dir_name = None
    if isinstance(session_id, str) and isinstance(study_day_local, str):
        expected_dir_name = f"{study_day_local}__{session_id}"
        if session_dir.name != expected_dir_name:
            report.error(f"session directory name {session_dir.name!r} != expected {expected_dir_name!r}")

    session = require_mapping(manifest.get("session"), report, "manifest.session")
    spans = require_list(manifest.get("spans"), report, "manifest.spans")
    config_snapshot = require_mapping(manifest.get("config_snapshot"), report, "manifest.config_snapshot")
    h10 = require_mapping(manifest.get("h10"), report, "manifest.h10")
    storage = require_mapping(manifest.get("storage"), report, "manifest.storage")
    files = require_list(manifest.get("files"), report, "manifest.files")
    upload_bundle = require_mapping(manifest.get("upload_bundle"), report, "manifest.upload_bundle")

    if session is not None:
        require_keys(
            session,
            [
                "start_utc",
                "end_utc",
                "start_reason",
                "end_reason",
                "span_count",
                "quarantined",
                "quarantine_reasons",
            ],
            report,
            "manifest.session",
        )
        if not is_rfc3339_utc(session.get("start_utc")):
            report.error("manifest.session.start_utc must be RFC3339 UTC")
        if not is_rfc3339_utc(session.get("end_utc")):
            report.error("manifest.session.end_utc must be RFC3339 UTC")
        if not isinstance(session.get("start_reason"), str):
            report.error("manifest.session.start_reason must be a string")
        if not isinstance(session.get("end_reason"), str):
            report.error("manifest.session.end_reason must be a string")
        if not isinstance(session.get("span_count"), int) or session.get("span_count") < 0:
            report.error("manifest.session.span_count must be a non-negative integer")
        if not isinstance(session.get("quarantined"), bool):
            report.error("manifest.session.quarantined must be boolean")
        quarantine_reasons = session.get("quarantine_reasons")
        if not isinstance(quarantine_reasons, list):
            report.error("manifest.session.quarantine_reasons must be an array")
        elif session.get("quarantined") is False and quarantine_reasons:
            report.error("manifest.session.quarantine_reasons must be empty when quarantined is false")

    if spans is not None:
        seen_span_ids: set[str] = set()
        for index, span_value in enumerate(spans):
            span = require_mapping(span_value, report, f"manifest.spans[{index}]")
            if span is None:
                continue
            require_keys(
                span,
                [
                    "span_id",
                    "index_in_session",
                    "start_utc",
                    "end_utc",
                    "start_reason",
                    "end_reason",
                    "packet_count",
                    "first_seq_in_span",
                    "last_seq_in_span",
                ],
                report,
                f"manifest.spans[{index}]",
            )
            span_id = span.get("span_id")
            if not is_hex(span_id, 32):
                report.error(f"manifest.spans[{index}].span_id must be 32 lowercase hex characters")
            elif span_id in seen_span_ids:
                report.error(f"duplicate span_id in manifest.spans: {span_id}")
            else:
                seen_span_ids.add(span_id)
            if span.get("index_in_session") != index:
                report.error(f"manifest.spans[{index}].index_in_session must equal {index}")
            if not is_rfc3339_utc(span.get("start_utc")):
                report.error(f"manifest.spans[{index}].start_utc must be RFC3339 UTC")
            if not is_rfc3339_utc(span.get("end_utc")):
                report.error(f"manifest.spans[{index}].end_utc must be RFC3339 UTC")
            for key in ("start_reason", "end_reason"):
                if not isinstance(span.get(key), str):
                    report.error(f"manifest.spans[{index}].{key} must be a string")
            for key in ("packet_count", "first_seq_in_span", "last_seq_in_span"):
                if not isinstance(span.get(key), int) or span.get(key) < 0:
                    report.error(f"manifest.spans[{index}].{key} must be a non-negative integer")
        if session is not None and isinstance(session.get("span_count"), int) and session["span_count"] != len(spans):
            report.error("manifest.session.span_count must equal len(manifest.spans)")

    if config_snapshot is not None:
        require_keys(
            config_snapshot,
            [
                "bound_h10_address",
                "timezone",
                "study_day_rollover_local",
                "overnight_upload_window_start_local",
                "overnight_upload_window_end_local",
                "critical_stop_voltage_v",
                "low_start_voltage_v",
                "off_charger_upload_voltage_v",
            ],
            report,
            "manifest.config_snapshot",
        )

    if h10 is not None:
        require_keys(
            h10,
            [
                "bound_address",
                "connected_address_first",
                "model_number",
                "serial_number",
                "firmware_revision",
                "battery_percent_first",
                "battery_percent_last",
            ],
            report,
            "manifest.h10",
        )

    if storage is not None:
        require_keys(storage, ["sd_capacity_bytes", "sd_identity", "filesystem"], report, "manifest.storage")
        if storage.get("filesystem") != "fat32":
            report.error("manifest.storage.filesystem must be 'fat32'")
        if not isinstance(storage.get("sd_capacity_bytes"), int) or storage.get("sd_capacity_bytes") <= 0:
            report.error("manifest.storage.sd_capacity_bytes must be a positive integer")
        sd_identity = require_mapping(storage.get("sd_identity"), report, "manifest.storage.sd_identity")
        if sd_identity is not None:
            require_keys(
                sd_identity,
                ["manufacturer_id", "oem_id", "product_name", "revision", "serial_number"],
                report,
                "manifest.storage.sd_identity",
            )

    journal_file_entry = None
    if files is not None:
        if len(files) != 1:
            report.error("manifest.files must contain exactly one entry")
        for index, file_value in enumerate(files):
            file_entry = require_mapping(file_value, report, f"manifest.files[{index}]")
            if file_entry is None:
                continue
            require_keys(file_entry, ["name", "size_bytes", "sha256"], report, f"manifest.files[{index}]")
            if file_entry.get("name") == "journal.bin":
                journal_file_entry = file_entry
            if not isinstance(file_entry.get("size_bytes"), int) or file_entry.get("size_bytes") < 0:
                report.error(f"manifest.files[{index}].size_bytes must be a non-negative integer")
            if not is_hex(file_entry.get("sha256"), 64):
                report.error(f"manifest.files[{index}].sha256 must be 64 lowercase hex characters")
        if journal_file_entry is None:
            report.error("manifest.files must include a journal.bin entry")

    if upload_bundle is not None:
        require_keys(
            upload_bundle,
            ["format", "compression", "canonicalization_version", "root_dir_name", "file_order"],
            report,
            "manifest.upload_bundle",
        )
        if upload_bundle.get("format") != "tar":
            report.error("manifest.upload_bundle.format must be 'tar'")
        if upload_bundle.get("compression") != "none":
            report.error("manifest.upload_bundle.compression must be 'none'")
        if upload_bundle.get("canonicalization_version") != 1:
            report.error("manifest.upload_bundle.canonicalization_version must be 1")
        if upload_bundle.get("root_dir_name") != session_dir.name:
            report.error("manifest.upload_bundle.root_dir_name must match session directory name")
        if upload_bundle.get("file_order") != ["manifest.json", "journal.bin"]:
            report.error("manifest.upload_bundle.file_order must equal ['manifest.json', 'journal.bin']")

    journal_size = journal_path.stat().st_size
    journal_sha256 = sha256_file(journal_path)
    if isinstance(journal_file_entry, dict):
        if journal_file_entry.get("size_bytes") != journal_size:
            report.error("manifest.files[journal.bin].size_bytes does not match local journal.bin size")
        if journal_file_entry.get("sha256") != journal_sha256:
            report.error("manifest.files[journal.bin].sha256 does not match local journal.bin sha256")

    try:
        journal_summary = parse_journal(journal_path)
    except ValueError as exc:
        report.error(f"journal validation failed: {exc}")
        journal_summary = None
    if isinstance(journal_summary, dict):
        for warning in journal_summary.get("warnings", []):
            report.warn(f"journal: {warning}")
        for error in journal_summary.get("errors", []):
            report.error(f"journal: {error}")
        validate_manifest_against_journal(report, manifest, journal_summary)

    try:
        bundle_sha256, bundle_size_bytes = compute_bundle_identity(session_dir.name, manifest_path, journal_path)
    except ValueError as exc:
        report.error(f"failed to compute canonical bundle identity: {exc}")
        bundle_sha256 = None
        bundle_size_bytes = None

    report.details.update(
        {
            "dir_name": session_dir.name,
            "session_id": session_id,
            "study_day_local": study_day_local,
            "session_start_utc": session.get("start_utc") if isinstance(session, dict) else None,
            "session_end_utc": session.get("end_utc") if isinstance(session, dict) else None,
            "quarantined": session.get("quarantined") if isinstance(session, dict) else None,
            "bundle_sha256": bundle_sha256,
            "bundle_size_bytes": bundle_size_bytes,
        }
    )
    return report.to_dict()


def validate_queue_file(
    queue_path: Path,
    session_reports: dict[str, dict[str, Any]],
    *,
    restrict_session_ids: set[str] | None = None,
) -> dict[str, Any]:
    report = Report(kind="upload_queue", path=str(queue_path))
    if not queue_path.is_file():
        report.error("upload_queue.json is missing")
        return report.to_dict()

    try:
        queue_raw = load_json_file(queue_path)
    except Exception as exc:  # noqa: BLE001
        report.error(f"failed to parse upload_queue.json: {exc}")
        return report.to_dict()

    queue = require_mapping(queue_raw, report, "upload_queue")
    if queue is None:
        return report.to_dict()

    require_keys(queue, ["schema_version", "updated_at_utc", "sessions"], report, "upload_queue")
    if queue.get("schema_version") != 1:
        report.error("upload_queue.schema_version must be 1")
    if not is_utc_or_null(queue.get("updated_at_utc")):
        report.error("upload_queue.updated_at_utc must be RFC3339 UTC or null")

    sessions = require_list(queue.get("sessions"), report, "upload_queue.sessions")
    if sessions is None:
        return report.to_dict()

    seen_session_ids: set[str] = set()
    prev_key: tuple[str, str, str] | None = None
    entry_map: dict[str, dict[str, Any]] = {}
    validated_count = 0

    for index, entry_value in enumerate(sessions):
        entry = require_mapping(entry_value, report, f"upload_queue.sessions[{index}]")
        if entry is None:
            continue
        require_keys(
            entry,
            [
                "session_id",
                "study_day_local",
                "dir_name",
                "session_start_utc",
                "session_end_utc",
                "bundle_sha256",
                "bundle_size_bytes",
                "quarantined",
                "status",
                "attempt_count",
                "last_attempt_utc",
                "last_failure_class",
                "verified_upload_utc",
                "receipt_id",
            ],
            report,
            f"upload_queue.sessions[{index}]",
        )

        session_id = entry.get("session_id")
        if not is_hex(session_id, 32):
            report.error(f"upload_queue.sessions[{index}].session_id must be 32 lowercase hex characters")
            continue
        if session_id in seen_session_ids:
            report.error(f"duplicate queue session_id: {session_id}")
        seen_session_ids.add(session_id)
        entry_map[session_id] = entry

        if restrict_session_ids is not None and session_id not in restrict_session_ids:
            continue
        validated_count += 1

        if not isinstance(entry.get("study_day_local"), str):
            report.error(f"upload_queue.sessions[{index}].study_day_local must be a string")
        if not isinstance(entry.get("dir_name"), str):
            report.error(f"upload_queue.sessions[{index}].dir_name must be a string")
        if not is_rfc3339_utc(entry.get("session_start_utc")):
            report.error(f"upload_queue.sessions[{index}].session_start_utc must be RFC3339 UTC")
        if not is_rfc3339_utc(entry.get("session_end_utc")):
            report.error(f"upload_queue.sessions[{index}].session_end_utc must be RFC3339 UTC")
        if not is_hex(entry.get("bundle_sha256"), 64):
            report.error(f"upload_queue.sessions[{index}].bundle_sha256 must be 64 lowercase hex characters")
        if not isinstance(entry.get("bundle_size_bytes"), int) or entry.get("bundle_size_bytes") < 0:
            report.error(f"upload_queue.sessions[{index}].bundle_size_bytes must be a non-negative integer")
        if not isinstance(entry.get("quarantined"), bool):
            report.error(f"upload_queue.sessions[{index}].quarantined must be boolean")
        if entry.get("status") not in QUEUE_STATUS_VALUES:
            report.error(f"upload_queue.sessions[{index}].status has invalid value {entry.get('status')!r}")
        if not isinstance(entry.get("attempt_count"), int) or entry.get("attempt_count") < 0:
            report.error(f"upload_queue.sessions[{index}].attempt_count must be a non-negative integer")
        if not is_utc_or_null(entry.get("last_attempt_utc")):
            report.error(f"upload_queue.sessions[{index}].last_attempt_utc must be RFC3339 UTC or null")
        if not is_utc_or_null(entry.get("verified_upload_utc")):
            report.error(f"upload_queue.sessions[{index}].verified_upload_utc must be RFC3339 UTC or null")
        if entry.get("status") == "verified":
            if not is_rfc3339_utc(entry.get("verified_upload_utc")):
                report.error(f"upload_queue.sessions[{index}] verified entries must set verified_upload_utc")
            if not isinstance(entry.get("receipt_id"), str):
                report.error(f"upload_queue.sessions[{index}] verified entries must set receipt_id")
        elif entry.get("verified_upload_utc") is not None or entry.get("receipt_id") is not None:
            report.warn(
                f"upload_queue.sessions[{index}] has verified metadata while status={entry.get('status')!r}"
            )
        if entry.get("attempt_count") == 0 and entry.get("last_attempt_utc") is not None:
            report.warn(f"upload_queue.sessions[{index}] has last_attempt_utc despite attempt_count=0")

        key = (str(entry.get("study_day_local")), str(entry.get("session_start_utc")), session_id)
        if prev_key is not None and key < prev_key:
            report.error("upload_queue.sessions is not in oldest-first order")
        prev_key = key

        session_report = session_reports.get(session_id)
        if session_report is None:
            report.error(f"queue entry {session_id} has no matching local closed session")
            continue
        for field_name in (
            "study_day_local",
            "dir_name",
            "session_start_utc",
            "session_end_utc",
            "quarantined",
            "bundle_sha256",
            "bundle_size_bytes",
        ):
            if entry.get(field_name) != session_report.get(field_name):
                report.error(
                    f"queue entry {session_id} field {field_name} does not match local session artifacts"
                )

    targets = restrict_session_ids if restrict_session_ids is not None else set(session_reports)
    missing_from_queue = sorted(targets - set(entry_map))
    for session_id in missing_from_queue:
        report.error(f"local closed session {session_id} is missing from upload_queue.json")

    report.details.update(
        {
            "updated_at_utc": queue.get("updated_at_utc"),
            "session_count": len(sessions),
            "validated_session_count": validated_count,
        }
    )
    return report.to_dict()


def validate_session_selection(logger_root: Path, session_ids: set[str]) -> dict[str, Any]:
    logger_root = resolve_logger_root(logger_root) or logger_root
    sessions_dir = logger_root / "sessions"
    session_reports: dict[str, dict[str, Any]] = {}
    missing: list[str] = []
    for session_id in sorted(session_ids):
        matches = sorted(sessions_dir.glob(f"*__{session_id}"))
        if not matches:
            missing.append(session_id)
            continue
        report = validate_session_dir(matches[0])
        session_reports[session_id] = report

    queue_report = validate_queue_file(
        logger_root / "state" / "upload_queue.json",
        session_reports,
        restrict_session_ids=session_ids,
    )
    errors = [f"session {session_id} directory not found under {sessions_dir}" for session_id in missing]
    errors.extend(queue_report["errors"])
    for report in session_reports.values():
        errors.extend(report["errors"])
    warnings: list[str] = []
    warnings.extend(queue_report["warnings"])
    for report in session_reports.values():
        warnings.extend(report["warnings"])
    return {
        "kind": "session_selection",
        "path": str(logger_root),
        "selected_session_ids": sorted(session_ids),
        "missing_session_ids": missing,
        "session_reports": [session_reports[session_id] for session_id in sorted(session_reports)],
        "queue_report": queue_report,
        "errors": errors,
        "warnings": warnings,
        "ok": not errors,
    }


def validate_logger_root(logger_root: Path) -> dict[str, Any]:
    resolved_root = resolve_logger_root(logger_root)
    if resolved_root is None:
        return {
            "kind": "logger_root",
            "path": str(logger_root),
            "errors": ["path is not a logger root and does not contain ./logger"],
            "warnings": [],
            "ok": False,
        }

    session_reports_list = [validate_session_dir(session_dir) for session_dir in closed_session_dirs(resolved_root)]
    session_reports = {
        report["session_id"]: report
        for report in session_reports_list
        if isinstance(report.get("session_id"), str)
    }
    queue_report = validate_queue_file(resolved_root / "state" / "upload_queue.json", session_reports)

    errors = list(queue_report["errors"])
    warnings = list(queue_report["warnings"])
    for report in session_reports_list:
        errors.extend(report["errors"])
        warnings.extend(report["warnings"])

    return {
        "kind": "logger_root",
        "path": str(resolved_root),
        "closed_session_count": len(session_reports_list),
        "session_reports": session_reports_list,
        "queue_report": queue_report,
        "errors": errors,
        "warnings": warnings,
        "ok": not errors,
    }


def auto_validate(path: Path) -> dict[str, Any]:
    resolved_root = resolve_logger_root(path)
    if resolved_root is not None:
        return validate_logger_root(resolved_root)

    if path.is_file() and path.name in {"manifest.json", "journal.bin"}:
        return validate_session_dir(path.parent)
    if path.is_dir() and (path / "manifest.json").is_file() and (path / "journal.bin").is_file():
        return validate_session_dir(path)
    if path.is_file() and path.name == "upload_queue.json":
        root = resolve_logger_root(path.parent.parent)
        if root is not None:
            return validate_logger_root(root)
        return validate_queue_file(path, {})

    return {
        "kind": "unknown",
        "path": str(path),
        "errors": ["could not identify a logger root, session directory, or upload_queue.json path"],
        "warnings": [],
        "ok": False,
    }


def print_text(summary: dict[str, Any]) -> None:
    print(f"kind: {summary['kind']}")
    print(f"path: {summary['path']}")
    if summary["kind"] == "logger_root":
        print(f"closed_session_count: {summary.get('closed_session_count', 0)}")
        queue_report = summary.get("queue_report", {})
        print(f"queue_session_count: {queue_report.get('session_count')}")
        for report in summary.get("session_reports", []):
            print(
                f"  session {report.get('session_id')} dir={report.get('dir_name')} ok={report.get('ok')}"
            )
    elif summary["kind"] == "session_dir":
        print(f"session_id: {summary.get('session_id')}")
        print(f"study_day_local: {summary.get('study_day_local')}")
        print(f"bundle_sha256: {summary.get('bundle_sha256')}")
        print(f"bundle_size_bytes: {summary.get('bundle_size_bytes')}")
    elif summary["kind"] == "upload_queue":
        print(f"session_count: {summary.get('session_count')}")
        print(f"validated_session_count: {summary.get('validated_session_count')}")

    if summary.get("warnings"):
        print("warnings:")
        for warning in summary["warnings"]:
            print(f"  - {warning}")

    if summary.get("errors"):
        print("errors:")
        for error in summary["errors"]:
            print(f"  - {error}")
    else:
        print("errors: none")
    print(f"ok: {summary['ok']}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("path", type=Path, help="logger root, session dir, manifest.json, journal.bin, or upload_queue.json")
    parser.add_argument("--json", action="store_true", help="emit machine-readable JSON")
    args = parser.parse_args()

    summary = auto_validate(args.path)
    if args.json:
        json.dump(summary, sys.stdout, indent=2, sort_keys=True)
        sys.stdout.write("\n")
    else:
        print_text(summary)
    return 0 if summary.get("ok") else 1


if __name__ == "__main__":
    raise SystemExit(main())