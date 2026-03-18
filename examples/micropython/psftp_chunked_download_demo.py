#!/usr/bin/env python3
import time
import polar_sdk

ROOT_DIR = "/"
MAX_FILE_BYTES = 4096
CHUNK_BYTES = 256

h10 = polar_sdk.Device(required_capabilities=(polar_sdk.CAP_PSFTP_READ,))
handle = None

try:
    print("connecting...")
    h10.connect(timeout_ms=15000)

    entries = None
    for attempt in range(1, 4):
        try:
            entries = h10.list_dir(ROOT_DIR)
            break
        except Exception as exc:
            print("list_dir failed (attempt", attempt, "):", type(exc), exc)
            time.sleep_ms(600)

    if entries is None:
        raise RuntimeError("list_dir failed after retries")

    candidates = []
    for entry in entries:
        name = entry.get("name")
        size = entry.get("size")
        if not entry.get("is_dir") and size <= MAX_FILE_BYTES:
            candidates.append((name, size))

    if not candidates:
        raise RuntimeError("no file <= %d bytes in %s" % (MAX_FILE_BYTES, ROOT_DIR))

    name, expected_size = candidates[0]
    path = ROOT_DIR + name
    print("chunked download", path, "expected_size", expected_size)

    handle = h10.download_open(path, timeout_ms=15000)
    buf = bytearray(CHUNK_BYTES)
    chunks = 0
    total = 0
    preview = b""

    while True:
        n = h10.download_read(handle, buf, timeout_ms=15000)
        if n == 0:
            print("EOF")
            handle = None
            break
        chunk = bytes(memoryview(buf)[:n])
        if len(preview) < 32:
            need = 32 - len(preview)
            preview += chunk[:need]
        total += n
        chunks += 1

    print("chunks", chunks)
    print("total", total)
    print("preview", preview)
    if expected_size is not None:
        print("size_match", total == expected_size)

    st = h10.stats()
    print("psftp stats:", {
        "tx": st.get("psftp_tx_frames_total"),
        "rx": st.get("psftp_rx_frames_total"),
        "seq_err": st.get("psftp_rx_seq_errors_total"),
        "proto_err": st.get("psftp_protocol_errors_total"),
        "overflow_err": st.get("psftp_overflow_errors_total"),
        "last_error": st.get("psftp_last_error_code"),
    })
finally:
    if handle is not None:
        try:
            h10.download_close(handle)
        except Exception:
            pass
    h10.disconnect()
