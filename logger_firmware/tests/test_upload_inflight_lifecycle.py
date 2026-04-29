#!/usr/bin/env python3
"""Static guards for durable upload in-flight queue semantics."""

from __future__ import annotations

from pathlib import Path


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> None:
    firmware_root = Path(__file__).resolve().parents[1]
    upload_c = (firmware_root / "src" / "upload.c").read_text(encoding="utf-8")
    queue_c = (firmware_root / "src" / "queue.c").read_text(encoding="utf-8")
    queue_h = (
        firmware_root / "include" / "logger" / "queue.h"
    ).read_text(encoding="utf-8")
    app_main_c = (firmware_root / "src" / "app_main.c").read_text(
        encoding="utf-8"
    )

    require(
        upload_c.count(
            'logger_copy_string(entry->status, sizeof(entry->status), "uploading")'
        )
        == 1,
        "only logger_upload_mark_inflight_attempt may write status=uploading",
    )

    process_start = upload_c.index("static bool logger_upload_process_selected")
    process = upload_c[process_start:]
    require(
        process.index("logger_net_wifi_join")
        < process.index("logger_upload_mark_inflight_attempt")
        < process.index("logger_upload_http_execute"),
        "uploading must be committed after Wi-Fi preflight and before HTTP execute",
    )
    require(
        process.index("const int request_n = snprintf")
        < process.index("logger_upload_mark_inflight_attempt"),
        "HTTP request construction must complete before status=uploading",
    )
    require(
        process.index("logger_storage_svc_bundle_open")
        < process.index("logger_upload_mark_inflight_attempt")
        < process.index("logger_upload_http_execute"),
        "bundle stream must be open before committing the in-flight marker",
    )
    mark_failure = process[
        process.index("if (!logger_upload_mark_inflight_attempt") : process.index(
            "logger_upload_http_response_t *http_response"
        )
    ]
    require(
        "logger_storage_svc_bundle_close();" in mark_failure
        and "logger_net_wifi_leave();" in mark_failure,
        "failed in-flight marker persist must close the bundle stream and leave Wi-Fi",
    )

    require(
        "logger_upload_apply_local_attempt_failure" in upload_c,
        "pre-inflight local failures must use the durable local-failure helper",
    )
    require(
        process.index("logger_upload_queue_recover_interrupted")
        < process.index("logger_upload_queue_find_eligible_session"),
        "direct upload calls must recover stale uploading entries before selection",
    )
    require(
        "logger_upload_queue_recover_interrupted" in queue_h,
        "queue API must expose interrupted upload recovery",
    )
    require(
        "logger_queue_entry_mark_interrupted" in queue_c
        and '"interrupted"' in queue_c
        and "verified_bundle_sha256[0] = '\\0'" in queue_c,
        "interrupted recovery must set failed/interrupted and clear verification fields",
    )
    require(
        app_main_c.index("logger_upload_queue_recover_interrupted")
        < app_main_c.index("logger_upload_queue_compute_summary"),
        "upload pass snapshots must recover stale uploading entries before summary/eligibility",
    )


if __name__ == "__main__":
    main()