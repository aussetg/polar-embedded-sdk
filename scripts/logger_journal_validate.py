#!/usr/bin/env python3
"""Validate and summarize logger_firmware journal.bin files.

This is a host-side developer tool. It parses the custom journal container,
checks CRCs and record layout, decodes JSON records, decodes binary
``data_chunk`` records, and verifies a few important v1 invariants:

- record-sequence monotonicity,
- span/data-chunk packet accounting,
- per-chunk entry sequencing,
- span_end summaries matching observed data_chunk payloads,
- required fields on a few key JSON record types.

The tool intentionally uses only the Python standard library.
"""

from __future__ import annotations

import argparse
import json
import struct
import sys
import zlib
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any


FILE_HEADER_BYTES = 64
RECORD_HEADER_BYTES = 32
FLAG_JSON = 0x00000001
FLAG_BINARY = 0x00000002

RECORD_TYPES = {
    0x0001: "session_start",
    0x0002: "span_start",
    0x0003: "data_chunk",
    0x0004: "status_snapshot",
    0x0005: "marker",
    0x0006: "gap",
    0x0007: "span_end",
    0x0008: "session_end",
    0x0009: "recovery",
    0x000A: "clock_event",
    0x000B: "h10_battery",
}


def u16le(buf: bytes, offset: int) -> int:
    return struct.unpack_from("<H", buf, offset)[0]


def u32le(buf: bytes, offset: int) -> int:
    return struct.unpack_from("<I", buf, offset)[0]


def u64le(buf: bytes, offset: int) -> int:
    return struct.unpack_from("<Q", buf, offset)[0]


def i64le(buf: bytes, offset: int) -> int:
    return struct.unpack_from("<q", buf, offset)[0]


def hex16(buf: bytes) -> str:
    if len(buf) != 16:
        raise ValueError("expected 16 bytes")
    return buf.hex()


@dataclass
class EntrySummary:
    seq_in_span: int
    mono_us: int
    utc_ns: int
    value_len: int
    value_prefix_hex: str


@dataclass
class ChunkSummary:
    chunk_seq_in_session: int
    span_id: str
    packet_count: int
    first_seq_in_span: int
    last_seq_in_span: int
    first_mono_us: int
    last_mono_us: int
    first_utc_ns: int
    last_utc_ns: int
    entries_bytes: int
    entries: list[EntrySummary] = field(default_factory=list)


@dataclass
class SpanAccumulator:
    packet_count: int = 0
    first_seq_in_span: int | None = None
    last_seq_in_span: int | None = None
    chunk_count: int = 0


@dataclass
class SpanRecord:
    span_id: str
    index_in_session: int | None = None
    start_reason: str | None = None
    end_reason: str | None = None
    packet_count_reported: int | None = None
    first_seq_in_span_reported: int | None = None
    last_seq_in_span_reported: int | None = None


@dataclass
class ValidationState:
    session_id_from_header: str
    boot_counter_at_open: int = 0
    journal_open_utc_ns: int = 0
    next_record_seq_expected: int | None = None
    spans: dict[str, SpanAccumulator] = field(default_factory=dict)
    span_records: dict[str, SpanRecord] = field(default_factory=dict)
    record_counts: dict[str, int] = field(default_factory=dict)
    chunks: list[ChunkSummary] = field(default_factory=list)
    session_start: dict[str, Any] | None = None
    session_end: dict[str, Any] | None = None
    errors: list[str] = field(default_factory=list)
    warnings: list[str] = field(default_factory=list)

    def count(self, name: str) -> None:
        self.record_counts[name] = self.record_counts.get(name, 0) + 1

    def error(self, message: str) -> None:
        self.errors.append(message)

    def warn(self, message: str) -> None:
        self.warnings.append(message)


def validate_json_record(state: ValidationState, record_name: str, payload: dict[str, Any], record_seq: int) -> None:
    if payload.get("schema_version") != 1:
        state.error(f"record_seq={record_seq}: schema_version != 1 for {record_name}")
    if payload.get("record_type") != record_name:
        state.error(f"record_seq={record_seq}: record_type mismatch for {record_name}")

    session_id = payload.get("session_id")
    if isinstance(session_id, str) and session_id != state.session_id_from_header:
        state.error(
            f"record_seq={record_seq}: session_id {session_id} != header session_id {state.session_id_from_header}"
        )

    if record_name == "session_start":
        state.session_start = payload
        for field_name in (
            "study_day_local",
            "logger_id",
            "subject_id",
            "timezone",
            "clock_state",
            "start_reason",
        ):
            if field_name not in payload:
                state.error(f"record_seq={record_seq}: missing {field_name} in session_start")

    elif record_name == "span_start":
        span_id = payload.get("span_id")
        if not isinstance(span_id, str):
            state.error(f"record_seq={record_seq}: span_start missing span_id")
            return
        acc = state.spans.setdefault(span_id, SpanAccumulator())
        span_record = state.span_records.setdefault(span_id, SpanRecord(span_id=span_id))
        if span_record.start_reason is not None:
            state.warn(f"record_seq={record_seq}: duplicate span_start for span {span_id}")
        span_record.index_in_session = payload.get("span_index_in_session")
        span_record.start_reason = payload.get("start_reason")
        if payload.get("encrypted") not in (True, False):
            state.error(f"record_seq={record_seq}: span_start encrypted must be boolean")
        if payload.get("bonded") not in (True, False):
            state.error(f"record_seq={record_seq}: span_start bonded must be boolean")
        if acc.packet_count > 0:
            state.warn(f"record_seq={record_seq}: span_start for span {span_id} after data already observed")

    elif record_name == "span_end":
        span_id = payload.get("span_id")
        if not isinstance(span_id, str):
            state.error(f"record_seq={record_seq}: span_end missing span_id")
            return
        acc = state.spans.setdefault(span_id, SpanAccumulator())
        span_record = state.span_records.setdefault(span_id, SpanRecord(span_id=span_id))
        span_record.end_reason = payload.get("end_reason")
        span_record.packet_count_reported = payload.get("packet_count")
        span_record.first_seq_in_span_reported = payload.get("first_seq_in_span")
        span_record.last_seq_in_span_reported = payload.get("last_seq_in_span")
        expected_packet_count = payload.get("packet_count")
        expected_first_seq = payload.get("first_seq_in_span")
        expected_last_seq = payload.get("last_seq_in_span")
        if expected_packet_count != acc.packet_count:
            state.error(
                f"record_seq={record_seq}: span_end packet_count={expected_packet_count} != observed {acc.packet_count}"
            )
        if acc.packet_count > 0:
            if expected_first_seq != acc.first_seq_in_span:
                state.error(
                    f"record_seq={record_seq}: span_end first_seq_in_span={expected_first_seq} != observed {acc.first_seq_in_span}"
                )
            if expected_last_seq != acc.last_seq_in_span:
                state.error(
                    f"record_seq={record_seq}: span_end last_seq_in_span={expected_last_seq} != observed {acc.last_seq_in_span}"
                )

    elif record_name == "clock_event":
        if payload.get("event_kind") not in {"clock_invalid", "clock_fixed", "clock_jump"}:
            state.error(f"record_seq={record_seq}: invalid clock_event event_kind={payload.get('event_kind')!r}")

    elif record_name == "h10_battery":
        battery_percent = payload.get("battery_percent")
        if not isinstance(battery_percent, int) or not (0 <= battery_percent <= 100):
            state.error(f"record_seq={record_seq}: invalid h10_battery battery_percent={battery_percent!r}")
        if payload.get("read_reason") not in {"connect", "periodic"}:
            state.error(f"record_seq={record_seq}: invalid h10_battery read_reason={payload.get('read_reason')!r}")

    elif record_name == "session_end":
        state.session_end = payload


def parse_data_chunk(payload: bytes, record_seq: int, state: ValidationState) -> None:
    if len(payload) < 80:
        state.error(f"record_seq={record_seq}: data_chunk payload too short ({len(payload)} bytes)")
        return

    stream_kind = u16le(payload, 0)
    encoding = u16le(payload, 2)
    chunk_seq_in_session = u32le(payload, 4)
    span_id = hex16(payload[8:24])
    packet_count = u32le(payload, 24)
    first_seq_in_span = u32le(payload, 28)
    last_seq_in_span = u32le(payload, 32)
    reserved0 = u32le(payload, 36)
    first_mono_us = u64le(payload, 40)
    last_mono_us = u64le(payload, 48)
    first_utc_ns = i64le(payload, 56)
    last_utc_ns = i64le(payload, 64)
    entries_bytes = u32le(payload, 72)
    reserved1 = u32le(payload, 76)

    if stream_kind != 1:
        state.error(f"record_seq={record_seq}: unexpected stream_kind={stream_kind}")
    if encoding != 1:
        state.error(f"record_seq={record_seq}: unexpected encoding={encoding}")
    if reserved0 != 0 or reserved1 != 0:
        state.error(f"record_seq={record_seq}: data_chunk reserved fields must be zero")
    if entries_bytes != len(payload) - 80:
        state.error(
            f"record_seq={record_seq}: entries_bytes={entries_bytes} != actual entries payload {len(payload) - 80}"
        )

    entries: list[EntrySummary] = []
    offset = 80
    last_seq_seen: int | None = None
    while offset < len(payload):
        if offset + 28 > len(payload):
            state.error(f"record_seq={record_seq}: truncated entry header at offset {offset}")
            break

        seq_in_span = u32le(payload, offset)
        flags = u32le(payload, offset + 4)
        mono_us = u64le(payload, offset + 8)
        utc_ns = i64le(payload, offset + 16)
        value_len = u16le(payload, offset + 24)
        reserved = u16le(payload, offset + 26)

        if flags != 0:
            state.error(f"record_seq={record_seq}: entry seq={seq_in_span} flags must be zero")
        if reserved != 0:
            state.error(f"record_seq={record_seq}: entry seq={seq_in_span} reserved must be zero")

        value_start = offset + 28
        value_end = value_start + value_len
        if value_end > len(payload):
            state.error(f"record_seq={record_seq}: entry seq={seq_in_span} value overruns payload")
            break

        if last_seq_seen is not None and seq_in_span <= last_seq_seen:
            state.error(
                f"record_seq={record_seq}: entry seq_in_span not strictly increasing ({last_seq_seen} -> {seq_in_span})"
            )
        last_seq_seen = seq_in_span

        padded_end = value_start + ((value_len + 3) & ~3)
        if padded_end > len(payload):
            state.error(f"record_seq={record_seq}: entry seq={seq_in_span} padded length overruns payload")
            break

        entries.append(
            EntrySummary(
                seq_in_span=seq_in_span,
                mono_us=mono_us,
                utc_ns=utc_ns,
                value_len=value_len,
                value_prefix_hex=payload[value_start:value_end][:8].hex(),
            )
        )
        offset = padded_end

    if len(entries) != packet_count:
        state.error(f"record_seq={record_seq}: packet_count={packet_count} != parsed entries {len(entries)}")
    if entries:
        if first_seq_in_span != entries[0].seq_in_span:
            state.error(
                f"record_seq={record_seq}: first_seq_in_span={first_seq_in_span} != first entry {entries[0].seq_in_span}"
            )
        if last_seq_in_span != entries[-1].seq_in_span:
            state.error(
                f"record_seq={record_seq}: last_seq_in_span={last_seq_in_span} != last entry {entries[-1].seq_in_span}"
            )
        if first_mono_us != entries[0].mono_us:
            state.error(f"record_seq={record_seq}: first_mono_us mismatch")
        if last_mono_us != entries[-1].mono_us:
            state.error(f"record_seq={record_seq}: last_mono_us mismatch")
        if first_utc_ns != entries[0].utc_ns:
            state.error(f"record_seq={record_seq}: first_utc_ns mismatch")
        if last_utc_ns != entries[-1].utc_ns:
            state.error(f"record_seq={record_seq}: last_utc_ns mismatch")

    acc = state.spans.setdefault(span_id, SpanAccumulator())
    acc.chunk_count += 1
    acc.packet_count += len(entries)
    if entries:
        if acc.first_seq_in_span is None:
            acc.first_seq_in_span = entries[0].seq_in_span
        acc.last_seq_in_span = entries[-1].seq_in_span

    state.chunks.append(
        ChunkSummary(
            chunk_seq_in_session=chunk_seq_in_session,
            span_id=span_id,
            packet_count=packet_count,
            first_seq_in_span=first_seq_in_span,
            last_seq_in_span=last_seq_in_span,
            first_mono_us=first_mono_us,
            last_mono_us=last_mono_us,
            first_utc_ns=first_utc_ns,
            last_utc_ns=last_utc_ns,
            entries_bytes=entries_bytes,
            entries=entries,
        )
    )


def parse_journal(path: Path) -> dict[str, Any]:
    data = path.read_bytes()
    if len(data) < FILE_HEADER_BYTES:
        raise ValueError(f"journal too short: {path}")

    header = data[:FILE_HEADER_BYTES]
    if header[:8] != b"NOF1JNL1":
        raise ValueError(f"invalid journal magic: {path}")
    if u16le(header, 8) != FILE_HEADER_BYTES:
        raise ValueError(f"unexpected header size in {path}")
    if u16le(header, 10) != 1:
        raise ValueError(f"unexpected journal version in {path}")

    header_crc_expect = u32le(header, 56)
    header_crc_actual = zlib.crc32(header[:56]) & 0xFFFFFFFF
    if header_crc_expect != header_crc_actual:
        raise ValueError(f"journal header CRC mismatch in {path}")

    state = ValidationState(
        session_id_from_header=hex16(header[16:32]),
        boot_counter_at_open=u64le(header, 32),
        journal_open_utc_ns=i64le(header, 40),
    )

    offset = FILE_HEADER_BYTES
    while offset < len(data):
        if offset + RECORD_HEADER_BYTES > len(data):
            state.error(f"truncated record header at offset {offset}")
            break

        record_header = data[offset : offset + RECORD_HEADER_BYTES]
        if record_header[:4] != b"RCD1":
            state.error(f"invalid record magic at offset {offset}")
            break

        header_bytes = u16le(record_header, 4)
        record_type = u16le(record_header, 6)
        total_bytes = u32le(record_header, 8)
        payload_bytes = u32le(record_header, 12)
        flags = u32le(record_header, 16)
        payload_crc_expect = u32le(record_header, 20)
        record_seq = u64le(record_header, 24)

        if header_bytes != RECORD_HEADER_BYTES:
            state.error(f"record at offset {offset} has unexpected header_bytes={header_bytes}")
            break
        if total_bytes != RECORD_HEADER_BYTES + payload_bytes:
            state.error(f"record_seq={record_seq}: inconsistent total_bytes/payload_bytes")
            break
        if flags not in (FLAG_JSON, FLAG_BINARY):
            state.error(f"record_seq={record_seq}: invalid flags=0x{flags:08x}")
            break

        payload_start = offset + RECORD_HEADER_BYTES
        payload_end = payload_start + payload_bytes
        if payload_end > len(data):
            state.error(f"record_seq={record_seq}: payload overruns file")
            break
        payload = data[payload_start:payload_end]
        payload_crc_actual = zlib.crc32(payload) & 0xFFFFFFFF
        if payload_crc_expect != payload_crc_actual:
            state.error(f"record_seq={record_seq}: payload CRC mismatch")
            break

        if state.next_record_seq_expected is not None and record_seq != state.next_record_seq_expected:
            state.error(
                f"record sequence jump: expected {state.next_record_seq_expected}, got {record_seq}"
            )
        state.next_record_seq_expected = record_seq + 1

        record_name = RECORD_TYPES.get(record_type, f"unknown_0x{record_type:04x}")
        state.count(record_name)

        if flags == FLAG_JSON:
            try:
                parsed = json.loads(payload.decode("utf-8"))
            except Exception as exc:  # noqa: BLE001
                state.error(f"record_seq={record_seq}: invalid JSON payload for {record_name}: {exc}")
            else:
                if not isinstance(parsed, dict):
                    state.error(f"record_seq={record_seq}: JSON payload for {record_name} is not an object")
                else:
                    validate_json_record(state, record_name, parsed, record_seq)
        else:
            if record_name == "data_chunk":
                parse_data_chunk(payload, record_seq, state)
            else:
                state.warn(f"record_seq={record_seq}: unexpected binary record type {record_name}")

        offset += total_bytes

    summary = {
        "path": str(path),
        "header": {
            "format_version": 1,
            "boot_counter_at_open": state.boot_counter_at_open,
            "journal_open_utc_ns": state.journal_open_utc_ns,
        },
        "session_id": state.session_id_from_header,
        "record_counts": dict(sorted(state.record_counts.items())),
        "chunk_count": len(state.chunks),
        "span_summaries": {
            span_id: {
                "packet_count": acc.packet_count,
                "first_seq_in_span": acc.first_seq_in_span,
                "last_seq_in_span": acc.last_seq_in_span,
                "chunk_count": acc.chunk_count,
            }
            for span_id, acc in sorted(state.spans.items())
        },
        "span_records": {
            span_id: {
                "index_in_session": span_record.index_in_session,
                "start_reason": span_record.start_reason,
                "end_reason": span_record.end_reason,
                "packet_count_reported": span_record.packet_count_reported,
                "first_seq_in_span_reported": span_record.first_seq_in_span_reported,
                "last_seq_in_span_reported": span_record.last_seq_in_span_reported,
            }
            for span_id, span_record in sorted(state.span_records.items())
        },
        "session_start": state.session_start,
        "session_end": state.session_end,
        "errors": state.errors,
        "warnings": state.warnings,
        "ok": not state.errors,
    }
    return summary


def print_text_summary(summary: dict[str, Any]) -> None:
    print(f"journal: {summary['path']}")
    print(f"session_id: {summary['session_id']}")
    print("record_counts:")
    for key, value in summary["record_counts"].items():
        print(f"  {key}: {value}")
    print(f"chunk_count: {summary['chunk_count']}")
    print("span_summaries:")
    for span_id, span_summary in summary["span_summaries"].items():
        print(
            "  "
            f"{span_id}: packets={span_summary['packet_count']} "
            f"seq={span_summary['first_seq_in_span']}..{span_summary['last_seq_in_span']} "
            f"chunks={span_summary['chunk_count']}"
        )

    if summary["warnings"]:
        print("warnings:")
        for warning in summary["warnings"]:
            print(f"  - {warning}")

    if summary["errors"]:
        print("errors:")
        for error in summary["errors"]:
            print(f"  - {error}")
    else:
        print("errors: none")

    print(f"ok: {summary['ok']}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("journal", type=Path, help="Path to journal.bin")
    parser.add_argument("--json", action="store_true", help="Emit machine-readable JSON summary")
    args = parser.parse_args()

    try:
        summary = parse_journal(args.journal)
    except ValueError as exc:
        print(str(exc), file=sys.stderr)
        return 2
    if args.json:
        json.dump(summary, sys.stdout, indent=2, sort_keys=True)
        sys.stdout.write("\n")
    else:
        print_text_summary(summary)
    return 0 if summary["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())