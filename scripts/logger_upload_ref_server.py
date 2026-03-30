#!/usr/bin/env python3
"""Reference upload server for the logger firmware v1 upload contract.

This is a host-side testing tool, so it lives under ``scripts/`` with the other
developer utilities. It intentionally uses only the Python standard library.

Features:

- accepts v1 logger uploads over HTTP POST,
- validates required request headers,
- validates the canonical tar layout and key manifest invariants,
- verifies body SHA-256 and journal.bin hash/size against manifest.json,
- persists accepted uploads under a local data directory,
- deduplicates by ``(session_id, sha256)`` as required by the spec,
- rejects same-session different-hash replays as ``validation_failed``,
- optionally enforces a bearer token and a minimum firmware version,
- exposes small inspection endpoints for local testing.

Useful endpoints:

- ``GET /healthz`` — simple health/config summary
- ``GET /uploads`` — accepted upload metadata
- ``POST /upload`` — normal upload endpoint by default

Failure injection is available with ``?force=...`` on the upload URL, e.g.
``/upload?force=temporary_unavailable`` or ``/upload?force=plain_503``.
"""

from __future__ import annotations

import argparse
import copy
import hashlib
import io
import json
import tarfile
import threading
from dataclasses import dataclass
from datetime import UTC, datetime
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any
from urllib.parse import parse_qs, urlparse


API_VERSION = 1
MANIFEST_SCHEMA_VERSION = 1
TAR_CANONICALIZATION_VERSION = 1
DEFAULT_PATH = "/upload"
DEFAULT_MAX_BODY_BYTES = 128 * 1024 * 1024


class UploadError(Exception):
    """Structured request error surfaced as an HTTP response."""

    def __init__(
        self,
        status: HTTPStatus,
        code: str,
        message: str,
        *,
        retryable: bool,
        plain_body: bytes | None = None,
        content_type: str = "application/json; charset=utf-8",
    ) -> None:
        super().__init__(message)
        self.status = status
        self.code = code
        self.message = message
        self.retryable = retryable
        self.plain_body = plain_body
        self.content_type = content_type


@dataclass(frozen=True)
class ServerConfig:
    host: str
    port: int
    path: str
    data_dir: Path
    max_body_bytes: int
    bearer_token: str | None
    min_firmware: str | None


@dataclass(frozen=True)
class ValidatedUpload:
    session_id: str
    study_day_local: str
    logger_id: str
    subject_id: str
    hardware_id: str
    firmware_version: str
    build_id: str
    root_dir_name: str
    bundle_sha256: str
    bundle_size_bytes: int
    journal_sha256: str
    journal_size_bytes: int
    manifest: dict[str, Any]


@dataclass(frozen=True)
class StoredUpload:
    session_id: str
    study_day_local: str
    logger_id: str
    subject_id: str
    hardware_id: str
    firmware_version: str
    build_id: str
    root_dir_name: str
    sha256: str
    size_bytes: int
    journal_sha256: str
    journal_size_bytes: int
    receipt_id: str
    received_at_utc: str
    deduplicated: bool
    tar_path: str
    manifest_path: str


class UploadStore:
    def __init__(self, data_dir: Path) -> None:
        self.data_dir = data_dir
        self.uploads_dir = data_dir / "uploads"
        self.index_path = data_dir / "index.json"
        self._lock = threading.Lock()

        self.uploads_dir.mkdir(parents=True, exist_ok=True)
        self.data_dir.mkdir(parents=True, exist_ok=True)
        self._state = self._load_state()

    def _default_state(self) -> dict[str, Any]:
        return {
            "schema_version": 1,
            "next_receipt_seq": 1,
            "uploads": [],
        }

    def _load_state(self) -> dict[str, Any]:
        if not self.index_path.exists():
            state = self._default_state()
            self._write_state(state)
            return state

        raw = json.loads(self.index_path.read_text(encoding="utf-8"))
        if raw.get("schema_version") != 1 or not isinstance(raw.get("uploads"), list):
            raise RuntimeError(f"invalid store state in {self.index_path}")
        if not isinstance(raw.get("next_receipt_seq"), int) or raw["next_receipt_seq"] < 1:
            raise RuntimeError(f"invalid receipt sequence in {self.index_path}")
        return raw

    def _write_state(self, state: dict[str, Any]) -> None:
        tmp_path = self.index_path.with_suffix(".json.tmp")
        tmp_path.write_text(json.dumps(state, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        tmp_path.replace(self.index_path)

    def list_uploads(self) -> list[dict[str, Any]]:
        with self._lock:
            return copy.deepcopy(self._state["uploads"])

    def count(self) -> int:
        with self._lock:
            return len(self._state["uploads"])

    def accept(self, validated: ValidatedUpload, body: bytes) -> StoredUpload:
        with self._lock:
            existing_same_session = [
                upload
                for upload in self._state["uploads"]
                if upload["session_id"] == validated.session_id
            ]
            for upload in existing_same_session:
                if upload["sha256"] == validated.bundle_sha256:
                    return StoredUpload(
                        session_id=upload["session_id"],
                        study_day_local=upload["study_day_local"],
                        logger_id=upload["logger_id"],
                        subject_id=upload["subject_id"],
                        hardware_id=upload["hardware_id"],
                        firmware_version=upload["firmware_version"],
                        build_id=upload["build_id"],
                        root_dir_name=upload["root_dir_name"],
                        sha256=upload["sha256"],
                        size_bytes=upload["size_bytes"],
                        journal_sha256=upload["journal_sha256"],
                        journal_size_bytes=upload["journal_size_bytes"],
                        receipt_id=upload["receipt_id"],
                        received_at_utc=upload["received_at_utc"],
                        deduplicated=True,
                        tar_path=upload["tar_path"],
                        manifest_path=upload["manifest_path"],
                    )

                raise UploadError(
                    HTTPStatus.UNPROCESSABLE_ENTITY,
                    "validation_failed",
                    "session_id already exists with a different sha256",
                    retryable=False,
                )

            receipt_seq = self._state["next_receipt_seq"]
            self._state["next_receipt_seq"] += 1
            received_at_utc = utc_now_rfc3339()
            receipt_id = f"rcpt_{datetime.now(UTC).strftime('%Y%m%d_%H%M%S')}_{receipt_seq:06d}"
            base_name = f"{validated.root_dir_name}__{validated.bundle_sha256[:12]}"
            tar_path = self.uploads_dir / f"{base_name}.tar"
            manifest_path = self.uploads_dir / f"{base_name}.manifest.json"

            tar_path.write_bytes(body)
            manifest_path.write_text(
                json.dumps(validated.manifest, indent=2, sort_keys=True) + "\n",
                encoding="utf-8",
            )

            record = {
                "session_id": validated.session_id,
                "study_day_local": validated.study_day_local,
                "logger_id": validated.logger_id,
                "subject_id": validated.subject_id,
                "hardware_id": validated.hardware_id,
                "firmware_version": validated.firmware_version,
                "build_id": validated.build_id,
                "root_dir_name": validated.root_dir_name,
                "sha256": validated.bundle_sha256,
                "size_bytes": validated.bundle_size_bytes,
                "journal_sha256": validated.journal_sha256,
                "journal_size_bytes": validated.journal_size_bytes,
                "receipt_id": receipt_id,
                "received_at_utc": received_at_utc,
                "tar_path": str(tar_path.relative_to(self.data_dir)),
                "manifest_path": str(manifest_path.relative_to(self.data_dir)),
            }
            self._state["uploads"].append(record)
            self._write_state(self._state)

            return StoredUpload(
                session_id=record["session_id"],
                study_day_local=record["study_day_local"],
                logger_id=record["logger_id"],
                subject_id=record["subject_id"],
                hardware_id=record["hardware_id"],
                firmware_version=record["firmware_version"],
                build_id=record["build_id"],
                root_dir_name=record["root_dir_name"],
                sha256=record["sha256"],
                size_bytes=record["size_bytes"],
                journal_sha256=record["journal_sha256"],
                journal_size_bytes=record["journal_size_bytes"],
                receipt_id=record["receipt_id"],
                received_at_utc=record["received_at_utc"],
                deduplicated=False,
                tar_path=record["tar_path"],
                manifest_path=record["manifest_path"],
            )


def utc_now_rfc3339() -> str:
    return datetime.now(UTC).replace(microsecond=0).isoformat().replace("+00:00", "Z")


def parse_args() -> ServerConfig:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1", help="bind address")
    parser.add_argument("--port", type=int, default=8787, help="listen port")
    parser.add_argument("--path", default=DEFAULT_PATH, help="upload endpoint path")
    parser.add_argument(
        "--data-dir",
        type=Path,
        default=Path("build/logger_upload_ref_server"),
        help="directory for accepted uploads and dedup state",
    )
    parser.add_argument(
        "--max-body-bytes",
        type=int,
        default=DEFAULT_MAX_BODY_BYTES,
        help="maximum accepted Content-Length before 413",
    )
    parser.add_argument(
        "--bearer-token",
        default=None,
        help="if set, require Authorization: Bearer <token>",
    )
    parser.add_argument(
        "--min-firmware",
        default=None,
        help="if set, reject firmware older than this version with HTTP 426",
    )
    args = parser.parse_args()

    path = args.path if args.path.startswith("/") else f"/{args.path}"
    return ServerConfig(
        host=args.host,
        port=args.port,
        path=path,
        data_dir=args.data_dir,
        max_body_bytes=args.max_body_bytes,
        bearer_token=args.bearer_token,
        min_firmware=args.min_firmware,
    )


def require_mapping(value: Any, context: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise UploadError(
            HTTPStatus.UNPROCESSABLE_ENTITY,
            "validation_failed",
            f"{context} must be a JSON object",
            retryable=False,
        )
    return value


def require_list(value: Any, context: str) -> list[Any]:
    if not isinstance(value, list):
        raise UploadError(
            HTTPStatus.UNPROCESSABLE_ENTITY,
            "validation_failed",
            f"{context} must be a JSON array",
            retryable=False,
        )
    return value


def require_string(value: Any, context: str) -> str:
    if not isinstance(value, str) or not value:
        raise UploadError(
            HTTPStatus.UNPROCESSABLE_ENTITY,
            "validation_failed",
            f"{context} must be a non-empty string",
            retryable=False,
        )
    return value


def require_int(value: Any, context: str) -> int:
    if not isinstance(value, int):
        raise UploadError(
            HTTPStatus.UNPROCESSABLE_ENTITY,
            "validation_failed",
            f"{context} must be an integer",
            retryable=False,
        )
    return value


def require_fields(obj: dict[str, Any], field_names: list[str], context: str) -> None:
    missing = [name for name in field_names if name not in obj]
    if missing:
        raise UploadError(
            HTTPStatus.UNPROCESSABLE_ENTITY,
            "validation_failed",
            f"{context} missing required fields: {', '.join(missing)}",
            retryable=False,
        )


def parse_semverish(version: str) -> tuple[int, int, int, tuple[str, ...]]:
    main, sep, suffix = version.partition("-")
    pieces = main.split(".")
    if len(pieces) != 3:
        raise ValueError(f"not semver-like: {version}")
    major, minor, patch = (int(piece) for piece in pieces)
    prerelease = tuple(filter(None, suffix.split("."))) if sep else ()
    return major, minor, patch, prerelease


def semverish_is_older(candidate: str, minimum: str) -> bool:
    c_major, c_minor, c_patch, c_pre = parse_semverish(candidate)
    m_major, m_minor, m_patch, m_pre = parse_semverish(minimum)
    c_core = (c_major, c_minor, c_patch)
    m_core = (m_major, m_minor, m_patch)
    if c_core != m_core:
        return c_core < m_core
    if c_pre == m_pre:
        return False
    if not c_pre and m_pre:
        return False
    if c_pre and not m_pre:
        return True
    return c_pre < m_pre


def require_header(headers: Any, name: str) -> str:
    value = headers.get(name)
    if value is None or not value.strip():
        raise UploadError(
            HTTPStatus.BAD_REQUEST,
            "malformed_request",
            f"missing required header: {name}",
            retryable=False,
        )
    return value.strip()


def sha256_hex(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def validate_tar_member_common(member: tarfile.TarInfo, expected_mode: int, context: str) -> None:
    if member.uid != 0 or member.gid != 0:
        raise UploadError(
            HTTPStatus.UNPROCESSABLE_ENTITY,
            "validation_failed",
            f"{context} must use uid=0 and gid=0",
            retryable=False,
        )
    if member.mtime != 0:
        raise UploadError(
            HTTPStatus.UNPROCESSABLE_ENTITY,
            "validation_failed",
            f"{context} must use mtime=0",
            retryable=False,
        )
    if (member.mode & 0o777) != expected_mode:
        raise UploadError(
            HTTPStatus.UNPROCESSABLE_ENTITY,
            "validation_failed",
            f"{context} mode must be {expected_mode:o}",
            retryable=False,
        )
    if member.uname or member.gname:
        raise UploadError(
            HTTPStatus.UNPROCESSABLE_ENTITY,
            "validation_failed",
            f"{context} must use empty uname/gname",
            retryable=False,
        )
    if member.pax_headers:
        raise UploadError(
            HTTPStatus.UNPROCESSABLE_ENTITY,
            "validation_failed",
            f"{context} must not include pax headers",
            retryable=False,
        )
    if member.name.startswith("./"):
        raise UploadError(
            HTTPStatus.UNPROCESSABLE_ENTITY,
            "validation_failed",
            f"{context} name must not start with ./",
            retryable=False,
        )


def validate_bundle(body: bytes, headers: Any, config: ServerConfig) -> ValidatedUpload:
    declared_api_version = require_header(headers, "X-Logger-Api-Version")
    if declared_api_version != str(API_VERSION):
        raise UploadError(
            HTTPStatus.BAD_REQUEST,
            "malformed_request",
            f"unsupported X-Logger-Api-Version: {declared_api_version}",
            retryable=False,
        )

    expected_session_id = require_header(headers, "X-Logger-Session-Id")
    expected_hardware_id = require_header(headers, "X-Logger-Hardware-Id")
    expected_logger_id = require_header(headers, "X-Logger-Logger-Id")
    expected_subject_id = require_header(headers, "X-Logger-Subject-Id")
    expected_study_day = require_header(headers, "X-Logger-Study-Day")
    expected_sha256 = require_header(headers, "X-Logger-SHA256").lower()
    expected_tar_version = require_header(headers, "X-Logger-Tar-Canonicalization-Version")
    expected_manifest_version = require_header(headers, "X-Logger-Manifest-Schema-Version")

    actual_sha256 = sha256_hex(body)
    if actual_sha256 != expected_sha256:
        raise UploadError(
            HTTPStatus.UNPROCESSABLE_ENTITY,
            "validation_failed",
            "declared X-Logger-SHA256 does not match request body",
            retryable=False,
        )

    if len(body) < 1024 or (len(body) % 512) != 0:
        raise UploadError(
            HTTPStatus.UNPROCESSABLE_ENTITY,
            "validation_failed",
            "tar stream must be block-aligned and end with two zero blocks",
            retryable=False,
        )
    if any(body[-1024:]):
        raise UploadError(
            HTTPStatus.UNPROCESSABLE_ENTITY,
            "validation_failed",
            "tar stream must terminate with exactly two zero blocks",
            retryable=False,
        )

    try:
        with tarfile.open(fileobj=io.BytesIO(body), mode="r:") as archive:
            members = archive.getmembers()
            if len(members) != 3:
                raise UploadError(
                    HTTPStatus.UNPROCESSABLE_ENTITY,
                    "validation_failed",
                    "canonical tar must contain exactly three members",
                    retryable=False,
                )

            root_member, manifest_member, journal_member = members
            root_dir_name = root_member.name.rstrip("/")
            if not root_member.isdir() or not root_dir_name:
                raise UploadError(
                    HTTPStatus.UNPROCESSABLE_ENTITY,
                    "validation_failed",
                    "first tar member must be the session root directory",
                    retryable=False,
                )
            validate_tar_member_common(root_member, 0o755, "root directory")

            expected_manifest_path = f"{root_dir_name}/manifest.json"
            expected_journal_path = f"{root_dir_name}/journal.bin"
            if manifest_member.name != expected_manifest_path or not manifest_member.isfile():
                raise UploadError(
                    HTTPStatus.UNPROCESSABLE_ENTITY,
                    "validation_failed",
                    "second tar member must be <root>/manifest.json",
                    retryable=False,
                )
            if journal_member.name != expected_journal_path or not journal_member.isfile():
                raise UploadError(
                    HTTPStatus.UNPROCESSABLE_ENTITY,
                    "validation_failed",
                    "third tar member must be <root>/journal.bin",
                    retryable=False,
                )
            validate_tar_member_common(manifest_member, 0o644, "manifest.json")
            validate_tar_member_common(journal_member, 0o644, "journal.bin")

            manifest_fh = archive.extractfile(manifest_member)
            journal_fh = archive.extractfile(journal_member)
            if manifest_fh is None or journal_fh is None:
                raise UploadError(
                    HTTPStatus.UNPROCESSABLE_ENTITY,
                    "validation_failed",
                    "failed to extract canonical tar members",
                    retryable=False,
                )
            manifest_bytes = manifest_fh.read()
            journal_bytes = journal_fh.read()
    except UploadError:
        raise
    except tarfile.TarError as exc:
        raise UploadError(
            HTTPStatus.UNPROCESSABLE_ENTITY,
            "validation_failed",
            f"invalid tar stream: {exc}",
            retryable=False,
        ) from exc

    if manifest_bytes.startswith(b"\xef\xbb\xbf"):
        raise UploadError(
            HTTPStatus.UNPROCESSABLE_ENTITY,
            "validation_failed",
            "manifest.json must be UTF-8 without BOM",
            retryable=False,
        )

    try:
        manifest = json.loads(manifest_bytes.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise UploadError(
            HTTPStatus.UNPROCESSABLE_ENTITY,
            "validation_failed",
            f"manifest.json is not valid UTF-8 JSON: {exc}",
            retryable=False,
        ) from exc

    manifest = require_mapping(manifest, "manifest.json")
    require_fields(
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
        "manifest.json",
    )

    schema_version = require_int(manifest["schema_version"], "manifest.schema_version")
    if schema_version != MANIFEST_SCHEMA_VERSION:
        raise UploadError(
            HTTPStatus.UNPROCESSABLE_ENTITY,
            "validation_failed",
            f"unsupported manifest schema_version: {schema_version}",
            retryable=False,
        )

    session_id = require_string(manifest["session_id"], "manifest.session_id")
    study_day_local = require_string(manifest["study_day_local"], "manifest.study_day_local")
    logger_id = require_string(manifest["logger_id"], "manifest.logger_id")
    subject_id = require_string(manifest["subject_id"], "manifest.subject_id")
    hardware_id = require_string(manifest["hardware_id"], "manifest.hardware_id")
    firmware_version = require_string(manifest["firmware_version"], "manifest.firmware_version")
    build_id = require_string(manifest["build_id"], "manifest.build_id")
    tar_version = require_int(
        manifest["tar_canonicalization_version"],
        "manifest.tar_canonicalization_version",
    )

    expected_root_dir_name = f"{study_day_local}__{session_id}"
    if root_dir_name != expected_root_dir_name:
        raise UploadError(
            HTTPStatus.UNPROCESSABLE_ENTITY,
            "validation_failed",
            "tar root directory does not match <study_day_local>__<session_id>",
            retryable=False,
        )

    session_section = require_mapping(manifest["session"], "manifest.session")
    require_fields(
        session_section,
        ["start_utc", "end_utc", "start_reason", "end_reason", "span_count", "quarantined", "quarantine_reasons"],
        "manifest.session",
    )
    require_list(manifest["spans"], "manifest.spans")
    require_mapping(manifest["config_snapshot"], "manifest.config_snapshot")
    require_mapping(manifest["h10"], "manifest.h10")
    storage_section = require_mapping(manifest["storage"], "manifest.storage")
    if storage_section.get("filesystem") != "fat32":
        raise UploadError(
            HTTPStatus.UNPROCESSABLE_ENTITY,
            "validation_failed",
            "manifest.storage.filesystem must be fat32",
            retryable=False,
        )

    files_section = require_list(manifest["files"], "manifest.files")
    if len(files_section) != 1:
        raise UploadError(
            HTTPStatus.UNPROCESSABLE_ENTITY,
            "validation_failed",
            "manifest.files must contain exactly one entry for journal.bin",
            retryable=False,
        )
    file_entry = require_mapping(files_section[0], "manifest.files[0]")
    require_fields(file_entry, ["name", "size_bytes", "sha256"], "manifest.files[0]")
    if file_entry["name"] != "journal.bin":
        raise UploadError(
            HTTPStatus.UNPROCESSABLE_ENTITY,
            "validation_failed",
            "manifest.files[0].name must be journal.bin",
            retryable=False,
        )

    journal_sha = sha256_hex(journal_bytes)
    journal_size = len(journal_bytes)
    if require_int(file_entry["size_bytes"], "manifest.files[0].size_bytes") != journal_size:
        raise UploadError(
            HTTPStatus.UNPROCESSABLE_ENTITY,
            "validation_failed",
            "manifest journal.bin size does not match tar payload",
            retryable=False,
        )
    if require_string(file_entry["sha256"], "manifest.files[0].sha256").lower() != journal_sha:
        raise UploadError(
            HTTPStatus.UNPROCESSABLE_ENTITY,
            "validation_failed",
            "manifest journal.bin sha256 does not match tar payload",
            retryable=False,
        )

    upload_bundle = require_mapping(manifest["upload_bundle"], "manifest.upload_bundle")
    require_fields(
        upload_bundle,
        ["format", "compression", "canonicalization_version", "root_dir_name", "file_order"],
        "manifest.upload_bundle",
    )
    if upload_bundle.get("format") != "tar" or upload_bundle.get("compression") != "none":
        raise UploadError(
            HTTPStatus.UNPROCESSABLE_ENTITY,
            "validation_failed",
            "manifest.upload_bundle must declare format=tar and compression=none",
            retryable=False,
        )
    if require_int(upload_bundle["canonicalization_version"], "manifest.upload_bundle.canonicalization_version") != TAR_CANONICALIZATION_VERSION:
        raise UploadError(
            HTTPStatus.UNPROCESSABLE_ENTITY,
            "validation_failed",
            "manifest.upload_bundle.canonicalization_version must be 1",
            retryable=False,
        )
    if require_string(upload_bundle["root_dir_name"], "manifest.upload_bundle.root_dir_name") != root_dir_name:
        raise UploadError(
            HTTPStatus.UNPROCESSABLE_ENTITY,
            "validation_failed",
            "manifest.upload_bundle.root_dir_name does not match tar root directory",
            retryable=False,
        )
    if require_list(upload_bundle["file_order"], "manifest.upload_bundle.file_order") != ["manifest.json", "journal.bin"]:
        raise UploadError(
            HTTPStatus.UNPROCESSABLE_ENTITY,
            "validation_failed",
            "manifest.upload_bundle.file_order must be ['manifest.json', 'journal.bin']",
            retryable=False,
        )

    if expected_manifest_version != str(schema_version):
        raise UploadError(
            HTTPStatus.UNPROCESSABLE_ENTITY,
            "validation_failed",
            "X-Logger-Manifest-Schema-Version does not match manifest.json",
            retryable=False,
        )
    if expected_tar_version != str(tar_version) or tar_version != TAR_CANONICALIZATION_VERSION:
        raise UploadError(
            HTTPStatus.UNPROCESSABLE_ENTITY,
            "validation_failed",
            "tar canonicalization version headers/manifest do not match v1",
            retryable=False,
        )

    header_checks = {
        "X-Logger-Session-Id": (expected_session_id, session_id),
        "X-Logger-Hardware-Id": (expected_hardware_id, hardware_id),
        "X-Logger-Logger-Id": (expected_logger_id, logger_id),
        "X-Logger-Subject-Id": (expected_subject_id, subject_id),
        "X-Logger-Study-Day": (expected_study_day, study_day_local),
    }
    for header_name, (declared, actual) in header_checks.items():
        if declared != actual:
            raise UploadError(
                HTTPStatus.UNPROCESSABLE_ENTITY,
                "validation_failed",
                f"{header_name} does not match manifest.json",
                retryable=False,
            )

    if config.min_firmware is not None:
        try:
            if semverish_is_older(firmware_version, config.min_firmware):
                raise UploadError(
                    HTTPStatus.UPGRADE_REQUIRED,
                    "minimum_firmware",
                    f"firmware {firmware_version} is below minimum {config.min_firmware}",
                    retryable=False,
                )
        except ValueError as exc:
            raise UploadError(
                HTTPStatus.UNPROCESSABLE_ENTITY,
                "validation_failed",
                f"unable to compare firmware versions: {exc}",
                retryable=False,
            ) from exc

    return ValidatedUpload(
        session_id=session_id,
        study_day_local=study_day_local,
        logger_id=logger_id,
        subject_id=subject_id,
        hardware_id=hardware_id,
        firmware_version=firmware_version,
        build_id=build_id,
        root_dir_name=root_dir_name,
        bundle_sha256=actual_sha256,
        bundle_size_bytes=len(body),
        journal_sha256=journal_sha,
        journal_size_bytes=journal_size,
        manifest=manifest,
    )


def make_handler(config: ServerConfig, store: UploadStore) -> type[BaseHTTPRequestHandler]:
    class Handler(BaseHTTPRequestHandler):
        server_version = "LoggerRefServer/1"
        sys_version = ""

        def log_message(self, fmt: str, *args: Any) -> None:
            timestamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
            print(f"[{timestamp}] {self.client_address[0]} {fmt % args}")

        def _send_json(self, status: HTTPStatus, payload: dict[str, Any]) -> None:
            body = (json.dumps(payload, sort_keys=True, separators=(",", ":")) + "\n").encode("utf-8")
            self.send_response(status)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def _send_plain(self, status: HTTPStatus, body: bytes, content_type: str = "text/plain; charset=utf-8") -> None:
            self.send_response(status)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def _send_error_response(self, error: UploadError) -> None:
            if error.plain_body is not None:
                self._send_plain(error.status, error.plain_body, error.content_type)
                return
            self._send_json(
                error.status,
                {
                    "api_version": API_VERSION,
                    "ok": False,
                    "error": {
                        "code": error.code,
                        "message": error.message,
                        "retryable": error.retryable,
                    },
                },
            )

        def _read_exact_body(self) -> bytes:
            raw_length = self.headers.get("Content-Length")
            if raw_length is None:
                raise UploadError(
                    HTTPStatus.BAD_REQUEST,
                    "malformed_request",
                    "missing Content-Length",
                    retryable=False,
                )
            try:
                length = int(raw_length)
            except ValueError as exc:
                raise UploadError(
                    HTTPStatus.BAD_REQUEST,
                    "malformed_request",
                    "invalid Content-Length",
                    retryable=False,
                ) from exc

            if length < 0:
                raise UploadError(
                    HTTPStatus.BAD_REQUEST,
                    "malformed_request",
                    "negative Content-Length is invalid",
                    retryable=False,
                )
            if length > config.max_body_bytes:
                raise UploadError(
                    HTTPStatus.REQUEST_ENTITY_TOO_LARGE,
                    "body_too_large",
                    f"request body exceeds max_body_bytes={config.max_body_bytes}",
                    retryable=False,
                )

            body = self.rfile.read(length)
            if len(body) != length:
                raise UploadError(
                    HTTPStatus.BAD_REQUEST,
                    "malformed_request",
                    "request body shorter than Content-Length",
                    retryable=False,
                )
            return body

        def do_GET(self) -> None:  # noqa: N802
            parsed = urlparse(self.path)
            if parsed.path == "/healthz":
                self._send_json(
                    HTTPStatus.OK,
                    {
                        "ok": True,
                        "api_version": API_VERSION,
                        "upload_path": config.path,
                        "data_dir": str(config.data_dir),
                        "stored_upload_count": store.count(),
                        "require_bearer": config.bearer_token is not None,
                        "min_firmware": config.min_firmware,
                    },
                )
                return

            if parsed.path == "/uploads":
                self._send_json(
                    HTTPStatus.OK,
                    {
                        "ok": True,
                        "api_version": API_VERSION,
                        "uploads": store.list_uploads(),
                    },
                )
                return

            self._send_json(
                HTTPStatus.NOT_FOUND,
                {
                    "ok": False,
                    "api_version": API_VERSION,
                    "error": {
                        "code": "not_found",
                        "message": f"unknown path: {parsed.path}",
                        "retryable": False,
                    },
                },
            )

        def do_POST(self) -> None:  # noqa: N802
            parsed = urlparse(self.path)
            if parsed.path != config.path:
                self._send_json(
                    HTTPStatus.NOT_FOUND,
                    {
                        "ok": False,
                        "api_version": API_VERSION,
                        "error": {
                            "code": "not_found",
                            "message": f"unknown upload path: {parsed.path}",
                            "retryable": False,
                        },
                    },
                )
                return

            try:
                if self.headers.get("Content-Type") != "application/x-tar":
                    raise UploadError(
                        HTTPStatus.BAD_REQUEST,
                        "malformed_request",
                        "Content-Type must be application/x-tar",
                        retryable=False,
                    )

                if config.bearer_token is not None:
                    expected_auth = f"Bearer {config.bearer_token}"
                    if self.headers.get("Authorization") != expected_auth:
                        raise UploadError(
                            HTTPStatus.UNAUTHORIZED,
                            "unauthorized",
                            "missing or invalid bearer token",
                            retryable=False,
                        )

                body = self._read_exact_body()
                force = parse_qs(parsed.query).get("force", [None])[0]
                if force == "plain_503":
                    raise UploadError(
                        HTTPStatus.SERVICE_UNAVAILABLE,
                        "temporary_unavailable",
                        "plain injected failure",
                        retryable=True,
                        plain_body=b"temporary_unavailable\n",
                        content_type="text/plain; charset=utf-8",
                    )
                if force == "plain_500":
                    raise UploadError(
                        HTTPStatus.INTERNAL_SERVER_ERROR,
                        "server_error",
                        "plain injected failure",
                        retryable=True,
                        plain_body=b"server_error\n",
                        content_type="text/plain; charset=utf-8",
                    )

                validated = validate_bundle(body, self.headers, config)

                injected_failures: dict[str, tuple[HTTPStatus, str, str, bool]] = {
                    "temporary_unavailable": (
                        HTTPStatus.SERVICE_UNAVAILABLE,
                        "temporary_unavailable",
                        "injected temporary-unavailable failure",
                        True,
                    ),
                    "server_error": (
                        HTTPStatus.INTERNAL_SERVER_ERROR,
                        "server_error",
                        "injected server error",
                        True,
                    ),
                    "validation_failed": (
                        HTTPStatus.UNPROCESSABLE_ENTITY,
                        "validation_failed",
                        "injected validation failure",
                        False,
                    ),
                    "minimum_firmware": (
                        HTTPStatus.UPGRADE_REQUIRED,
                        "minimum_firmware",
                        "injected minimum-firmware rejection",
                        False,
                    ),
                }
                if force in injected_failures:
                    status, code, message, retryable = injected_failures[force]
                    raise UploadError(status, code, message, retryable=retryable)

                stored = store.accept(validated, body)
                self._send_json(
                    HTTPStatus.OK,
                    {
                        "api_version": API_VERSION,
                        "ok": True,
                        "session_id": stored.session_id,
                        "sha256": stored.sha256,
                        "size_bytes": stored.size_bytes,
                        "receipt_id": stored.receipt_id,
                        "received_at_utc": stored.received_at_utc,
                        "deduplicated": stored.deduplicated,
                    },
                )
            except UploadError as exc:
                self._send_error_response(exc)
            except Exception as exc:  # pragma: no cover - defensive server boundary
                self._send_error_response(
                    UploadError(
                        HTTPStatus.INTERNAL_SERVER_ERROR,
                        "server_error",
                        f"unexpected server error: {exc}",
                        retryable=True,
                    )
                )

    return Handler


def main() -> int:
    config = parse_args()
    store = UploadStore(config.data_dir)
    handler_cls = make_handler(config, store)
    server = ThreadingHTTPServer((config.host, config.port), handler_cls)

    print("[logger-ref-server] listening")
    print(f"[logger-ref-server] bind={config.host}:{config.port}")
    print(f"[logger-ref-server] upload_path={config.path}")
    print(f"[logger-ref-server] data_dir={config.data_dir}")
    print(f"[logger-ref-server] bearer_required={config.bearer_token is not None}")
    print(f"[logger-ref-server] min_firmware={config.min_firmware}")

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\n[logger-ref-server] stopping")
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())