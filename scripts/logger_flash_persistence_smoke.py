#!/usr/bin/env python3

"""Flash-persistence smoke test for the logger firmware.

This harness intentionally avoids OpenOCD for routine flash snapshot / restore
 work. Instead it uses picotool's BOOTSEL path for:

- saving the reserved persistence region,
- flashing a temporary service-mode firmware,
- seeding legacy/new persistence records,
- restoring the original firmware + persistence image.

The test validates three things on real hardware:

1. legacy persisted config/metadata are migrated forward on first boot,
2. config import rewrites config + system log but not hot metadata,
3. fault clear rewrites hot metadata + system log but not config.
"""

from __future__ import annotations

import argparse
import binascii
import ctypes
import json
import os
import shutil
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any

REPO_ROOT = Path(__file__).resolve().parents[1]

try:
    from logger_usb import LoggerDevice, list_serial_port_candidates, probe_logger_port
except ModuleNotFoundError:  # pragma: no cover - depends on invocation style
    from scripts.logger_usb import LoggerDevice, list_serial_port_candidates, probe_logger_port


XIP_BASE = 0x10000000
FLASH_SECTOR_SIZE = 4096
FLASH_PAGE_SIZE = 256
PICO_FLASH_SIZE_BYTES = 16 * 1024 * 1024
BTSTACK_STORAGE_OFFSET = PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE - (FLASH_SECTOR_SIZE * 2)

PERSIST_REGION_SIZE = 64 * FLASH_SECTOR_SIZE
PERSIST_REGION_OFFSET = BTSTACK_STORAGE_OFFSET - PERSIST_REGION_SIZE

CONFIG_SLOT_COUNT = 8
CONFIG_SLOT_SIZE = FLASH_SECTOR_SIZE
CONFIG_REGION_OFFSET = PERSIST_REGION_OFFSET
CONFIG_REGION_SIZE = CONFIG_SLOT_COUNT * CONFIG_SLOT_SIZE

METADATA_SLOT_COUNT = 8
METADATA_SLOT_SIZE = FLASH_SECTOR_SIZE
METADATA_REGION_SIZE = METADATA_SLOT_COUNT * METADATA_SLOT_SIZE
SYSTEM_LOG_REGION_OFFSET = CONFIG_REGION_OFFSET + CONFIG_REGION_SIZE
SYSTEM_LOG_REGION_SIZE = PERSIST_REGION_SIZE - CONFIG_REGION_SIZE - METADATA_REGION_SIZE
METADATA_REGION_OFFSET = SYSTEM_LOG_REGION_OFFSET + SYSTEM_LOG_REGION_SIZE

LEGACY_PERSIST_REGION_SIZE = 16 * FLASH_SECTOR_SIZE
LEGACY_PERSIST_REGION_OFFSET = BTSTACK_STORAGE_OFFSET - LEGACY_PERSIST_REGION_SIZE
LEGACY_CONFIG_REGION_OFFSET = LEGACY_PERSIST_REGION_OFFSET
LEGACY_METADATA_REGION_OFFSET = LEGACY_PERSIST_REGION_OFFSET + (2 * FLASH_SECTOR_SIZE) + (12 * FLASH_SECTOR_SIZE)

LOGGER_FLASH_CONFIG_MAGIC = 0x31474643
LOGGER_FLASH_CONFIG_SCHEMA_VERSION = 4
LOGGER_FLASH_METADATA_MAGIC = 0x3141544D
LOGGER_FLASH_METADATA_SCHEMA_VERSION = 1

LOGGER_FAULT_NONE = 0
LOGGER_FAULT_CONFIG_INCOMPLETE = 1
LOGGER_FAULT_CLOCK_INVALID = 2

DEFAULT_CURRENT_UF2 = REPO_ROOT / "build" / "logger_firmware_rp2_2" / "logger_appliance.uf2"
DEFAULT_OUT_ROOT = REPO_ROOT / "build" / "smoke"


class LoggerConfig(ctypes.Structure):
    _fields_ = [
        ("logger_id", ctypes.c_char * 64),
        ("subject_id", ctypes.c_char * 64),
        ("bound_h10_address", ctypes.c_char * 18),
        ("timezone", ctypes.c_char * 64),
        ("upload_url", ctypes.c_char * 192),
        ("upload_token", ctypes.c_char * 160),
        ("wifi_ssid", ctypes.c_char * 33),
        ("wifi_psk", ctypes.c_char * 65),
        ("upload_tls_mode", ctypes.c_char * 32),
        ("upload_tls_anchor_der_len", ctypes.c_uint16),
        ("upload_tls_anchor_der", ctypes.c_uint8 * 2304),
        ("upload_tls_anchor_sha256", ctypes.c_char * 65),
        ("upload_tls_anchor_subject", ctypes.c_char * 256),
    ]


class FlashConfigRecord(ctypes.Structure):
    _fields_ = [
        ("magic", ctypes.c_uint32),
        ("schema_version", ctypes.c_uint16),
        ("payload_bytes", ctypes.c_uint16),
        ("sequence", ctypes.c_uint32),
        ("crc32", ctypes.c_uint32),
        ("config", LoggerConfig),
    ]


class FlashMetadataRecord(ctypes.Structure):
    _fields_ = [
        ("magic", ctypes.c_uint32),
        ("schema_version", ctypes.c_uint16),
        ("payload_bytes", ctypes.c_uint16),
        ("sequence", ctypes.c_uint32),
        ("crc32", ctypes.c_uint32),
        ("boot_counter", ctypes.c_uint32),
        ("current_fault_code", ctypes.c_uint16),
        ("last_cleared_fault_code", ctypes.c_uint16),
        ("last_boot_firmware_version", ctypes.c_char * 32),
        ("last_boot_build_id", ctypes.c_char * 64),
    ]


@dataclass(frozen=True)
class RegionDiff:
    config_changed: int
    system_log_changed: int
    metadata_changed: int

    def as_dict(self) -> dict[str, int]:
        return {
            "config_changed": self.config_changed,
            "system_log_changed": self.system_log_changed,
            "metadata_changed": self.metadata_changed,
        }


def run_subprocess(command: list[str], *, timeout_s: float) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command,
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
        timeout=timeout_s,
        check=False,
    )


def run_picotool(
    args: list[str],
    *,
    timeout_s: float = 600.0,
    retries: int = 4,
    retry_delay_s: float = 1.5,
) -> dict[str, Any]:
    command = ["picotool", *args]
    last_completed: subprocess.CompletedProcess[str] | None = None
    retryable_markers = (
        "device returned an error: rebooting",
        "No accessible RP-series devices in BOOTSEL mode were found",
        "consider -f",
        "Failed to connect",
        "No connected devices found",
    )

    for attempt in range(1, retries + 1):
        completed = run_subprocess(command, timeout_s=timeout_s)
        if completed.returncode == 0:
            return {
                "command": command,
                "returncode": completed.returncode,
                "stdout": completed.stdout,
                "stderr": completed.stderr,
                "attempt": attempt,
            }

        last_completed = completed
        merged = (completed.stdout or "") + "\n" + (completed.stderr or "")
        if attempt < retries and any(marker in merged for marker in retryable_markers):
            time.sleep(retry_delay_s)
            continue

        raise RuntimeError(
            "picotool command failed: "
            f"command={command!r} rc={completed.returncode} "
            f"stdout={completed.stdout!r} stderr={completed.stderr!r}"
        )

    assert last_completed is not None
    raise RuntimeError(
        "picotool command failed after retries: "
        f"command={command!r} rc={last_completed.returncode}"
    )


def wait_for_logger(
    *,
    expected_mode: str | None,
    preferred_port: Path | None,
    timeout_s: float = 60.0,
) -> tuple[Path, dict[str, Any]]:
    deadline = time.monotonic() + timeout_s
    last_response: dict[str, Any] | None = None

    while time.monotonic() < deadline:
        candidates: list[Path] = []
        seen: set[Path] = set()
        if preferred_port is not None:
            candidates.append(preferred_port)
            seen.add(preferred_port)
        for port in list_serial_port_candidates():
            if port not in seen:
                candidates.append(port)
                seen.add(port)

        for port in candidates:
            try:
                probe = probe_logger_port(port, timeout_s=2.0)
            except Exception:  # noqa: BLE001
                probe = None
            if probe is None:
                continue
            last_response = probe.status_response
            if expected_mode is None or probe.status_response.get("payload", {}).get("mode") == expected_mode:
                return port, probe.status_response
        time.sleep(0.5)

    raise RuntimeError(
        f"timed out waiting for logger mode={expected_mode!r}; last_response={last_response!r}"
    )


def picotool_save_persist_region(path: Path) -> dict[str, Any]:
    path.parent.mkdir(parents=True, exist_ok=True)
    return run_picotool(
        [
            "save",
            "-r",
            hex(XIP_BASE + PERSIST_REGION_OFFSET),
            hex(XIP_BASE + PERSIST_REGION_OFFSET + PERSIST_REGION_SIZE),
            str(path),
            "-t",
            "bin",
            "-f",
        ],
        timeout_s=600.0,
    )


def picotool_load_bin(path: Path, address: int, *, force: bool) -> dict[str, Any]:
    args = ["load"]
    if force:
        args.append("-f")
    args.extend(
        [
            "--ignore-partitions",
            "-v",
            str(path),
            "-t",
            "bin",
            "-o",
            hex(address),
        ]
    )
    return run_picotool(args, timeout_s=1200.0)


def picotool_load_app_and_persistence(uf2_path: Path, persist_image_path: Path) -> dict[str, Any]:
    load_app = run_picotool(["load", "-F", "-v", str(uf2_path)], timeout_s=1200.0)
    load_persist = picotool_load_bin(persist_image_path, XIP_BASE + PERSIST_REGION_OFFSET, force=False)
    reboot = run_picotool(["reboot", "-a"], timeout_s=120.0)
    return {
        "load_app": load_app,
        "load_persist": load_persist,
        "reboot": reboot,
    }


def c_string_bytes(value: str, size: int) -> bytes:
    raw = value.encode("utf-8")
    if len(raw) >= size:
        raise ValueError(f"string too long for field size={size}: {value!r}")
    return raw


def crc32_ieee(data: bytes) -> int:
    return binascii.crc32(data) & 0xFFFFFFFF


def make_config_record(*, sequence: int, values: dict[str, str]) -> bytes:
    record = FlashConfigRecord()
    ctypes.memset(ctypes.byref(record), 0xFF, ctypes.sizeof(record))
    record.magic = LOGGER_FLASH_CONFIG_MAGIC
    record.schema_version = LOGGER_FLASH_CONFIG_SCHEMA_VERSION
    record.payload_bytes = ctypes.sizeof(record)
    record.sequence = sequence
    record.crc32 = 0
    record.config.logger_id = c_string_bytes(values.get("logger_id", ""), 64)
    record.config.subject_id = c_string_bytes(values.get("subject_id", ""), 64)
    record.config.bound_h10_address = c_string_bytes(values.get("bound_h10_address", ""), 18)
    record.config.timezone = c_string_bytes(values.get("timezone", ""), 64)
    record.config.upload_url = c_string_bytes(values.get("upload_url", ""), 192)
    record.config.upload_token = c_string_bytes(values.get("upload_token", ""), 160)
    record.config.wifi_ssid = c_string_bytes(values.get("wifi_ssid", ""), 33)
    record.config.wifi_psk = c_string_bytes(values.get("wifi_psk", ""), 65)
    record.config.upload_tls_mode = c_string_bytes(values.get("upload_tls_mode", ""), 32)
    record.config.upload_tls_anchor_sha256 = c_string_bytes(values.get("upload_tls_anchor_sha256", ""), 65)
    record.config.upload_tls_anchor_subject = c_string_bytes(values.get("upload_tls_anchor_subject", ""), 256)

    data = bytearray(bytes(record))
    data[12:16] = (0).to_bytes(4, "little")
    record.crc32 = crc32_ieee(bytes(data))
    return bytes(record)


def make_metadata_record(
    *,
    sequence: int,
    boot_counter: int,
    current_fault_code: int,
    last_cleared_fault_code: int,
    last_boot_firmware_version: str,
    last_boot_build_id: str,
) -> bytes:
    record = FlashMetadataRecord()
    ctypes.memset(ctypes.byref(record), 0xFF, ctypes.sizeof(record))
    record.magic = LOGGER_FLASH_METADATA_MAGIC
    record.schema_version = LOGGER_FLASH_METADATA_SCHEMA_VERSION
    record.payload_bytes = ctypes.sizeof(record)
    record.sequence = sequence
    record.crc32 = 0
    record.boot_counter = boot_counter
    record.current_fault_code = current_fault_code
    record.last_cleared_fault_code = last_cleared_fault_code
    record.last_boot_firmware_version = c_string_bytes(last_boot_firmware_version, 32)
    record.last_boot_build_id = c_string_bytes(last_boot_build_id, 64)

    data = bytearray(bytes(record))
    data[12:16] = (0).to_bytes(4, "little")
    record.crc32 = crc32_ieee(bytes(data))
    return bytes(record)


def record_valid_metadata(raw: bytes) -> dict[str, int | str] | None:
    if len(raw) < ctypes.sizeof(FlashMetadataRecord):
        return None
    record = FlashMetadataRecord.from_buffer_copy(raw[: ctypes.sizeof(FlashMetadataRecord)])
    if record.magic != LOGGER_FLASH_METADATA_MAGIC:
        return None
    if record.schema_version != LOGGER_FLASH_METADATA_SCHEMA_VERSION:
        return None
    if record.payload_bytes != ctypes.sizeof(FlashMetadataRecord):
        return None

    data = bytearray(bytes(raw[: ctypes.sizeof(FlashMetadataRecord)]))
    data[12:16] = (0).to_bytes(4, "little")
    if crc32_ieee(bytes(data)) != record.crc32:
        return None

    return {
        "sequence": int(record.sequence),
        "boot_counter": int(record.boot_counter),
        "current_fault_code": int(record.current_fault_code),
        "last_cleared_fault_code": int(record.last_cleared_fault_code),
        "last_boot_firmware_version": bytes(record.last_boot_firmware_version).split(b"\0", 1)[0].decode("utf-8", "replace"),
        "last_boot_build_id": bytes(record.last_boot_build_id).split(b"\0", 1)[0].decode("utf-8", "replace"),
    }


def latest_metadata_from_dump(data: bytes) -> dict[str, int | str]:
    best: dict[str, int | str] | None = None
    rel = METADATA_REGION_OFFSET - PERSIST_REGION_OFFSET
    for slot in range(METADATA_SLOT_COUNT):
        start = rel + (slot * METADATA_SLOT_SIZE)
        candidate = record_valid_metadata(data[start : start + METADATA_SLOT_SIZE])
        if candidate is None:
            continue
        candidate["slot"] = slot
        if best is None or int(candidate["sequence"]) >= int(best["sequence"]):
            best = candidate
    if best is None:
        raise RuntimeError("no valid metadata record found in persistence dump")
    return best


def compare_region_bytes(before: bytes, after: bytes) -> RegionDiff:
    if len(before) != len(after):
        raise ValueError("persistence dump length mismatch")

    config_rel = CONFIG_REGION_OFFSET - PERSIST_REGION_OFFSET
    system_log_rel = SYSTEM_LOG_REGION_OFFSET - PERSIST_REGION_OFFSET
    metadata_rel = METADATA_REGION_OFFSET - PERSIST_REGION_OFFSET

    def diff_range(start: int, size: int) -> int:
        end = start + size
        return sum(1 for lhs, rhs in zip(before[start:end], after[start:end]) if lhs != rhs)

    return RegionDiff(
        config_changed=diff_range(config_rel, CONFIG_REGION_SIZE),
        system_log_changed=diff_range(system_log_rel, SYSTEM_LOG_REGION_SIZE),
        metadata_changed=diff_range(metadata_rel, METADATA_REGION_SIZE),
    )


def build_force_service_firmware(work_dir: Path) -> Path:
    tree_root = work_dir / "force_service_tree"
    build_dir = work_dir / "force_service_build"

    if tree_root.exists():
        shutil.rmtree(tree_root)
    tree_root.mkdir(parents=True)
    shutil.copytree(REPO_ROOT / "logger_firmware", tree_root / "logger_firmware")
    for name in ("polar_sdk", "firmware", "vendors", "scripts"):
        os.symlink(REPO_ROOT / name, tree_root / name, target_is_directory=True)

    main_c = tree_root / "logger_firmware" / "src" / "main.c"
    text = main_c.read_text(encoding="utf-8")
    needle = "    const logger_boot_gesture_t boot_gesture = logger_button_detect_boot_gesture(boot_now_ms);\n"
    replacement = "    const logger_boot_gesture_t boot_gesture = LOGGER_BOOT_GESTURE_SERVICE;\n"
    if needle not in text:
        raise RuntimeError("could not patch temporary main.c for forced service boot")
    main_c.write_text(text.replace(needle, replacement), encoding="utf-8")

    run = lambda cmd, timeout: run_subprocess(cmd, timeout_s=timeout)  # noqa: E731
    configured = run(["cmake", "-S", str(tree_root / "logger_firmware"), "-B", str(build_dir)], 240.0)
    if configured.returncode != 0:
        raise RuntimeError(f"temporary service build configure failed: {configured.stderr}")
    built = run(["cmake", "--build", str(build_dir), f"-j{os.cpu_count() or 4}"], 1200.0)
    if built.returncode != 0:
        raise RuntimeError(f"temporary service build failed: {built.stderr}")

    uf2_path = build_dir / "logger_appliance.uf2"
    if not uf2_path.exists():
        raise RuntimeError(f"missing temporary service UF2: {uf2_path}")
    return uf2_path


def write_json(path: Path, document: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(document, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def configure_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=Path, help="preferred USB serial port (auto-discovery still allowed after reboots)")
    parser.add_argument("--current-uf2", type=Path, default=DEFAULT_CURRENT_UF2, help="main firmware UF2 to restore/use")
    parser.add_argument("--out-root", type=Path, default=DEFAULT_OUT_ROOT, help="artifact directory root")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = configure_parser().parse_args(argv)

    current_uf2 = args.current_uf2.resolve()
    if not current_uf2.exists():
        raise RuntimeError(f"current firmware UF2 missing: {current_uf2}")

    run_id = time.strftime("%Y%m%d_%H%M%S")
    out_dir = args.out_root.resolve() / f"flash_persistence_smoke_{run_id}"
    out_dir.mkdir(parents=True, exist_ok=True)

    report: dict[str, Any] = {
        "artifact_dir": str(out_dir),
        "ok": False,
        "restore": {"attempted": False, "ok": False},
    }

    backup_image = out_dir / "persistence_backup.bin"
    legacy_seed_image = out_dir / "legacy_seed_image.bin"
    post_migration_dump = out_dir / "post_migration.bin"
    reboot_before_dump = out_dir / "reboot_before.bin"
    reboot_after_dump = out_dir / "reboot_after.bin"
    before_import_dump = out_dir / "before_import.bin"
    after_import_dump = out_dir / "after_import.bin"
    before_fault_clear_dump = out_dir / "before_fault_clear.bin"
    after_fault_clear_dump = out_dir / "after_fault_clear.bin"
    fault_slot_path = out_dir / "fault_seed_slot.bin"

    preferred_port = args.port.resolve() if args.port is not None else None
    service_uf2 = build_force_service_firmware(out_dir)
    report["service_uf2"] = str(service_uf2)

    original_port, original_status = wait_for_logger(expected_mode=None, preferred_port=preferred_port, timeout_s=30.0)
    preferred_port = original_port
    report["device_port"] = str(original_port)
    report["original_status"] = original_status

    with LoggerDevice(original_port) as device:
        original_config = device.command_ok("config export --json", "config export")
    write_json(out_dir / "original_config_export.json", original_config)

    picotool_save_persist_region(backup_image)

    try:
        legacy_payload = original_config["payload"]
        legacy_config_values = {
            "logger_id": "legacy-migrate-logger",
            "subject_id": "legacy-migrate-subject",
            "bound_h10_address": legacy_payload["recording"]["bound_h10_address"],
            "timezone": legacy_payload["time"]["timezone"],
            "upload_url": "",
            "upload_token": "",
            "wifi_ssid": "",
            "wifi_psk": "",
            "upload_tls_mode": "",
            "upload_tls_anchor_sha256": "",
            "upload_tls_anchor_subject": "",
        }
        seeded_boot_counter = 41
        seeded_last_cleared = LOGGER_FAULT_CONFIG_INCOMPLETE

        legacy_image = bytearray(b"\xff" * PERSIST_REGION_SIZE)
        legacy_cfg_rel = LEGACY_CONFIG_REGION_OFFSET - PERSIST_REGION_OFFSET
        legacy_meta_rel = LEGACY_METADATA_REGION_OFFSET - PERSIST_REGION_OFFSET
        legacy_image[legacy_cfg_rel : legacy_cfg_rel + ctypes.sizeof(FlashConfigRecord)] = make_config_record(
            sequence=7,
            values=legacy_config_values,
        )
        legacy_image[legacy_meta_rel : legacy_meta_rel + ctypes.sizeof(FlashMetadataRecord)] = make_metadata_record(
            sequence=11,
            boot_counter=seeded_boot_counter,
            current_fault_code=LOGGER_FAULT_NONE,
            last_cleared_fault_code=seeded_last_cleared,
            last_boot_firmware_version="legacy-fw",
            last_boot_build_id="legacy-build",
        )
        legacy_seed_image.write_bytes(legacy_image)

        report["seed_legacy"] = picotool_load_app_and_persistence(service_uf2, legacy_seed_image)
        migrated_port, migrated_status = wait_for_logger(expected_mode="service", preferred_port=preferred_port, timeout_s=60.0)
        preferred_port = migrated_port
        report["migration_status"] = migrated_status

        with LoggerDevice(migrated_port) as device:
            migrated_config = device.command_ok("config export --json", "config export")
            migrated_system_log = device.command_ok("system-log export --json", "system-log export")
        write_json(out_dir / "migrated_config_export.json", migrated_config)
        write_json(out_dir / "migrated_system_log.json", migrated_system_log)
        picotool_save_persist_region(post_migration_dump)
        post_migration_bytes = post_migration_dump.read_bytes()
        migration_diff = compare_region_bytes(bytes(legacy_image), post_migration_bytes)

        migration_events = migrated_system_log["payload"]["events"]
        boot_events = [event for event in migration_events if event.get("kind") == "boot"]
        if not boot_events:
            raise RuntimeError("migration boot did not produce a boot system-log event")
        boot_event = boot_events[-1]
        if migrated_config["payload"]["identity"]["logger_id"] != legacy_config_values["logger_id"]:
            raise RuntimeError("migrated config export logger_id does not match seeded legacy value")
        if migrated_status["payload"]["fault"]["last_cleared_code"] != "config_incomplete":
            raise RuntimeError("migrated status last_cleared_code did not preserve legacy metadata")
        if boot_event.get("boot_counter") != seeded_boot_counter + 1:
            raise RuntimeError(
                f"expected migrated boot_counter {seeded_boot_counter + 1}, got {boot_event.get('boot_counter')}"
            )
        if migration_diff.config_changed <= 0 or migration_diff.metadata_changed <= 0:
            raise RuntimeError(f"legacy migration did not rewrite both config and metadata regions: {migration_diff}")
        report["migration"] = {
            "legacy_seed": {
                "logger_id": legacy_config_values["logger_id"],
                "subject_id": legacy_config_values["subject_id"],
                "boot_counter": seeded_boot_counter,
                "last_cleared_fault_code": seeded_last_cleared,
            },
            "diff": migration_diff.as_dict(),
            "boot_event": boot_event,
        }

        picotool_save_persist_region(reboot_before_dump)
        wait_for_logger(expected_mode="service", preferred_port=preferred_port, timeout_s=60.0)
        picotool_save_persist_region(reboot_after_dump)
        wait_for_logger(expected_mode="service", preferred_port=preferred_port, timeout_s=60.0)
        reboot_diff = compare_region_bytes(reboot_before_dump.read_bytes(), reboot_after_dump.read_bytes())
        report["reboot_baseline"] = reboot_diff.as_dict()

        imported_doc = json.loads(json.dumps(migrated_config["payload"]))
        imported_doc["exported_at_utc"] = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
        imported_doc["identity"]["logger_id"] = "post-import-logger"
        compact_import = json.dumps(imported_doc, separators=(",", ":"), ensure_ascii=False)

        picotool_save_persist_region(before_import_dump)
        migrated_port, _ = wait_for_logger(expected_mode="service", preferred_port=preferred_port, timeout_s=60.0)
        with LoggerDevice(migrated_port) as device:
            import_responses = device.import_config_document(compact_import)
            imported_config = device.command_ok("config export --json", "config export")
        write_json(out_dir / "config_import_responses.json", import_responses)
        write_json(out_dir / "post_import_config_export.json", imported_config)
        picotool_save_persist_region(after_import_dump)
        migrated_port, _ = wait_for_logger(expected_mode="service", preferred_port=preferred_port, timeout_s=60.0)
        import_diff = compare_region_bytes(before_import_dump.read_bytes(), after_import_dump.read_bytes())
        if imported_config["payload"]["identity"]["logger_id"] != "post-import-logger":
            raise RuntimeError("config import did not update logger_id")
        if import_diff.config_changed <= 0 or import_diff.metadata_changed != reboot_diff.metadata_changed:
            raise RuntimeError(
                "config import region diff unexpected: "
                f"diff={import_diff} reboot_baseline={reboot_diff}"
            )
        report["config_import"] = {
            "diff": import_diff.as_dict(),
            "net_after_reboot_baseline": {
                "config_changed": import_diff.config_changed - reboot_diff.config_changed,
                "system_log_changed": import_diff.system_log_changed - reboot_diff.system_log_changed,
                "metadata_changed": import_diff.metadata_changed - reboot_diff.metadata_changed,
            },
            "logger_id_after": imported_config["payload"]["identity"]["logger_id"],
        }

        metadata_after_import = latest_metadata_from_dump(after_import_dump.read_bytes())
        fault_record_bytes = make_metadata_record(
            sequence=int(metadata_after_import["sequence"]) + 1,
            boot_counter=int(metadata_after_import["boot_counter"]),
            current_fault_code=LOGGER_FAULT_CLOCK_INVALID,
            last_cleared_fault_code=int(metadata_after_import["last_cleared_fault_code"]),
            last_boot_firmware_version=str(metadata_after_import["last_boot_firmware_version"]),
            last_boot_build_id=str(metadata_after_import["last_boot_build_id"]),
        )
        target_fault_slot = (int(metadata_after_import["slot"]) + 1) % METADATA_SLOT_COUNT
        fault_slot_image = bytearray(b"\xff" * FLASH_SECTOR_SIZE)
        fault_slot_image[: len(fault_record_bytes)] = fault_record_bytes
        fault_slot_path.write_bytes(fault_slot_image)
        report["inject_fault"] = picotool_load_bin(
            fault_slot_path,
            XIP_BASE + METADATA_REGION_OFFSET + (target_fault_slot * METADATA_SLOT_SIZE),
            force=True,
        )
        fault_port, fault_status = wait_for_logger(expected_mode="service", preferred_port=preferred_port, timeout_s=60.0)
        preferred_port = fault_port
        if fault_status["payload"]["fault"]["current_code"] != "clock_invalid":
            raise RuntimeError(
                f"expected injected fault clock_invalid, got {fault_status['payload']['fault']['current_code']!r}"
            )

        picotool_save_persist_region(before_fault_clear_dump)
        fault_port, _ = wait_for_logger(expected_mode="service", preferred_port=preferred_port, timeout_s=60.0)
        with LoggerDevice(fault_port) as device:
            unlock_response = device.service_unlock()
            clear_response = device.command_ok("fault clear", "fault clear", timeout_s=10.0)
            cleared_status = device.command_ok("status --json", "status")
        write_json(out_dir / "fault_clear_unlock.json", unlock_response)
        write_json(out_dir / "fault_clear_response.json", clear_response)
        write_json(out_dir / "fault_clear_status.json", cleared_status)
        picotool_save_persist_region(after_fault_clear_dump)
        wait_for_logger(expected_mode="service", preferred_port=preferred_port, timeout_s=60.0)
        clear_diff = compare_region_bytes(before_fault_clear_dump.read_bytes(), after_fault_clear_dump.read_bytes())
        metadata_before_clear = latest_metadata_from_dump(before_fault_clear_dump.read_bytes())
        metadata_after_clear = latest_metadata_from_dump(after_fault_clear_dump.read_bytes())
        if clear_diff.config_changed != 0 or clear_diff.metadata_changed <= 0:
            raise RuntimeError(
                "fault clear region diff unexpected: "
                f"diff={clear_diff} reboot_baseline={reboot_diff}"
            )
        if cleared_status["payload"]["fault"]["latched"] is not False:
            raise RuntimeError("fault clear did not clear the latched fault")
        if cleared_status["payload"]["fault"]["last_cleared_code"] != "clock_invalid":
            raise RuntimeError("fault clear did not update last_cleared_code")
        if int(metadata_after_clear["sequence"]) <= int(metadata_before_clear["sequence"]):
            raise RuntimeError("fault clear did not advance metadata sequence")
        if int(metadata_after_clear["current_fault_code"]) != LOGGER_FAULT_NONE:
            raise RuntimeError("fault clear metadata record did not clear current_fault_code")
        if int(metadata_after_clear["last_cleared_fault_code"]) != LOGGER_FAULT_CLOCK_INVALID:
            raise RuntimeError("fault clear metadata record did not update last_cleared_fault_code")
        report["fault_clear"] = {
            "diff": clear_diff.as_dict(),
            "net_after_reboot_baseline": {
                "config_changed": clear_diff.config_changed - reboot_diff.config_changed,
                "system_log_changed": clear_diff.system_log_changed - reboot_diff.system_log_changed,
                "metadata_changed": clear_diff.metadata_changed - reboot_diff.metadata_changed,
            },
            "metadata_before": metadata_before_clear,
            "metadata_after": metadata_after_clear,
            "status_after": cleared_status,
        }

        report["ok"] = True
        return_code = 0
    except Exception as exc:  # noqa: BLE001
        report["error"] = repr(exc)
        return_code = 1
    finally:
        report["restore"]["attempted"] = True
        try:
            report["restore"]["operations"] = picotool_load_app_and_persistence(current_uf2, backup_image)
            restored_port, restored_status = wait_for_logger(expected_mode=None, preferred_port=preferred_port, timeout_s=60.0)
            report["restore"]["ok"] = True
            report["restore"]["port"] = str(restored_port)
            report["restore"]["status"] = restored_status
        except Exception as exc:  # noqa: BLE001
            report["restore"]["error"] = repr(exc)
            return_code = 1

        write_json(out_dir / "smoke_report.json", report)

    return return_code


if __name__ == "__main__":
    raise SystemExit(main())