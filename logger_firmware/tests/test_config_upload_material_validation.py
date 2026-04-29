#!/usr/bin/env python3
"""Static guards for upload/config field validation."""

from __future__ import annotations

from pathlib import Path


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> None:
    firmware_root = Path(__file__).resolve().parents[1]

    config_validate_h = (
        firmware_root / "include" / "logger" / "config_validate.h"
    ).read_text(encoding="utf-8")
    config_validate_c = (firmware_root / "src" / "config_validate.c").read_text(
        encoding="utf-8"
    )
    upload_url_c = (firmware_root / "src" / "upload_url.c").read_text(
        encoding="utf-8"
    )
    config_store_c = (firmware_root / "src" / "config_store.c").read_text(
        encoding="utf-8"
    )
    service_cli_c = (firmware_root / "src" / "service_cli.c").read_text(
        encoding="utf-8"
    )
    upload_c = (firmware_root / "src" / "upload.c").read_text(encoding="utf-8")
    json_c = (firmware_root / "src" / "json.c").read_text(encoding="utf-8")

    for symbol in (
        "logger_config_logger_id_valid",
        "logger_config_subject_id_valid",
        "logger_config_upload_url_valid",
        "logger_config_upload_api_key_valid",
        "logger_config_upload_token_valid",
        "logger_config_upload_request_material_valid",
    ):
        require(symbol in config_validate_h, f"missing validator prototype: {symbol}")
        require(symbol in config_validate_c, f"missing validator body: {symbol}")

    require(
        "c < 0x21u || c > 0x7eu" in config_validate_c,
        "upload secrets must be constrained to visible non-space ASCII",
    )
    require(
        "c > 0x20u && c < 0x7fu && c != '#'" in upload_url_c,
        "upload URLs must reject controls, whitespace, DEL, and fragments",
    )
    require(
        "memchr(after_scheme, '@'" in upload_url_c,
        "upload URLs must reject authority userinfo",
    )
    require(
        "value == 0u" in json_c,
        "JSON string copying must reject embedded NUL before config validation",
    )

    for setter in (
        "logger_config_set_logger_id",
        "logger_config_set_subject_id",
        "logger_config_set_upload_url",
        "logger_config_set_upload_api_key",
        "logger_config_set_upload_token",
    ):
        section = config_store_c[config_store_c.index(f"bool {setter}") :]
        section = section[: section.index("}\n")]
        require("_valid(" in section, f"{setter} must validate before saving")

    require(
        "logger_config_upload_request_material_valid" in upload_c,
        "upload request construction must have a final config-material guard",
    )
    require(
        "malformed_config" in upload_c,
        "invalid upload config must be reported as config-blocked",
    )
    require(
        "logger_config_logger_id_valid" in service_cli_c
        and "logger_config_upload_api_key_valid" in service_cli_c
        and "logger_config_upload_token_valid" in service_cli_c,
        "config import must validate decoded identity and auth fields",
    )


if __name__ == "__main__":
    main()