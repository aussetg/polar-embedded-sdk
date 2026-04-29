#!/usr/bin/env python3
"""Static guard for upload request identity/auth headers."""

from __future__ import annotations

from pathlib import Path


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def section_between(text: str, start: str, end: str) -> str:
    start_index = text.index(start)
    end_index = text.index(end, start_index)
    return text[start_index:end_index]


def main() -> None:
    firmware_root = Path(__file__).resolve().parents[1]
    repo_root = firmware_root.parent

    upload_c = (firmware_root / "src" / "upload.c").read_text(encoding="utf-8")
    data_contract = (
        repo_root / "docs" / "specs" / "logger_data_contract_v1.md"
    ).read_text(encoding="utf-8")
    ref_server = (repo_root / "scripts" / "logger_upload_ref_server.py").read_text(
        encoding="utf-8"
    )
    repair_upload = (
        repo_root / "scripts" / "logger_repair_upload_bundle.py"
    ).read_text(encoding="utf-8")

    for required_header in (
        "Authorization: Bearer %s\\r\\n",
        "X-Logger-Session-Id: %s\\r\\n",
        "X-Logger-Hardware-Id: %s\\r\\n",
        "X-Logger-Logger-Id: %s\\r\\n",
        "X-Logger-Study-Day: %s\\r\\n",
        "X-Logger-SHA256: %s\\r\\n",
    ):
        require(
            required_header in upload_c,
            f"missing upload header: {required_header}",
        )

    require(
        "X-Logger-Subject-Id" not in upload_c,
        "upload request must not send subject identity as a header",
    )
    require(
        "X-Logger-Subject-Id" not in repair_upload,
        "manual repair uploader must not send subject identity as a header",
    )

    request_section = section_between(
        data_contract,
        "### 11.1 Request",
        "### 11.2 Success semantics",
    )
    required_headers = section_between(
        request_section,
        "Required headers:",
        "If configured, the request also includes:",
    )
    require(
        "X-Logger-Subject-Id" not in required_headers,
        "data contract must not list X-Logger-Subject-Id as a request header",
    )
    require(
        "MUST NOT include `X-Logger-Subject-Id`" in request_section,
        "data contract must explicitly forbid the subject header",
    )
    require(
        "manifest `subject_id` as metadata only" in data_contract,
        "manifest subject_id must remain metadata-only",
    )
    require(
        'headers.get("X-Logger-Subject-Id")' in ref_server
        and "X-Logger-Subject-Id is forbidden" in ref_server,
        "reference upload server must reject subject identity headers",
    )


if __name__ == "__main__":
    main()
