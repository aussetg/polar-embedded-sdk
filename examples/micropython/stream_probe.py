#!/usr/bin/env python3
# Generic capability-driven live stream probe for non-H10 validation.

import time
import polar_sdk

TARGET_ADDR = None
NAME_PREFIX = "Polar"
KIND = "hr"  # edit after inspecting capabilities_demo.py output
FORMAT = None  # None = device default format for this kind
CONNECT_TIMEOUT_MS = 20000
READ_TIMEOUT_MS = 1000
READ_WINDOW_MS = 10000
MAX_BYTES = 1024

CAP_MAP = {
    "hr": polar_sdk.CAP_STREAM_HR,
    "ecg": polar_sdk.CAP_STREAM_ECG,
    "acc": polar_sdk.CAP_STREAM_ACC,
    "ppg": polar_sdk.CAP_STREAM_PPG,
    "ppi": polar_sdk.CAP_STREAM_PPI,
    "gyro": polar_sdk.CAP_STREAM_GYRO,
    "mag": polar_sdk.CAP_STREAM_MAG,
}


def make_device():
    required = ()
    cap = CAP_MAP.get(KIND)
    if cap is not None:
        required = (cap,)

    if TARGET_ADDR:
        return polar_sdk.Device(addr=TARGET_ADDR, required_capabilities=required)
    return polar_sdk.Device(name_prefix=NAME_PREFIX, required_capabilities=required)


h = make_device()

try:
    print("version", polar_sdk.version())
    print("required capabilities", h.required_capabilities())
    h.connect(timeout_ms=CONNECT_TIMEOUT_MS)
    print("connected", h.state())

    caps = h.capabilities()
    stream_kinds = caps.get("streams", {}).get("kinds", ())
    print("stream kinds", stream_kinds)
    if KIND not in stream_kinds:
        raise RuntimeError("stream kind is not supported by this device")

    cfg = h.stream_default_config(KIND)
    print("starting", KIND, "format", FORMAT, "cfg", cfg)
    h.start_stream(KIND, format=FORMAT, **cfg)

    chunks = 0
    total_bytes = 0
    t0 = time.ticks_ms()
    while time.ticks_diff(time.ticks_ms(), t0) < READ_WINDOW_MS:
        data = h.read_stream(KIND, max_bytes=MAX_BYTES, timeout_ms=READ_TIMEOUT_MS)
        if not data:
            print("poll timeout")
            continue
        chunks += 1
        total_bytes += len(data)
        print("chunk", chunks, "bytes", len(data), "total", total_bytes)

    print("done", {"kind": KIND, "chunks": chunks, "bytes": total_bytes})
finally:
    try:
        h.stop_stream(KIND)
    except Exception as exc:
        print("stop_stream note", type(exc).__name__, exc)
    h.disconnect()
    print("final state", h.state())
