# ECG streaming demo: connect, start ECG, stream for a fixed window, stop.

import time
import polar_sdk

DURATION_S = 120
READ_MAX_BYTES = 1024
READ_TIMEOUT_MS = 1000
STATS_PERIOD_MS = 10000
STALE_DATA_TIMEOUT_MS = 5000

h10 = polar_sdk.H10(name_prefix="Polar", required_services=polar_sdk.SERVICE_ECG)
print("version", polar_sdk.version())
print("required services", h10.required_services())

bytes_total = 0
chunks_total = 0

try:
    h10.connect(timeout_ms=15000)
    print("connected", h10.state())

    h10.start_ecg(sample_rate=130)
    print("ecg started")

    start_ms = time.ticks_ms()
    last_stats_ms = start_ms
    last_data_ms = start_ms

    while time.ticks_diff(time.ticks_ms(), start_ms) < DURATION_S * 1000:
        try:
            chunk = h10.read_ecg(max_bytes=READ_MAX_BYTES, timeout_ms=READ_TIMEOUT_MS)
        except polar_sdk.NotConnectedError as exc:
            print("read stopped (not connected):", exc)
            break

        if chunk:
            chunks_total += 1
            bytes_total += len(chunk)
            last_data_ms = time.ticks_ms()

        now = time.ticks_ms()

        if time.ticks_diff(now, last_data_ms) > STALE_DATA_TIMEOUT_MS:
            print("stale ECG data timeout", STALE_DATA_TIMEOUT_MS, "ms")
            print("stats", h10.stats())
            break

        if time.ticks_diff(now, last_stats_ms) >= STATS_PERIOD_MS:
            s = h10.stats()
            elapsed_s = time.ticks_diff(now, start_ms) // 1000
            print(
                "t_s",
                elapsed_s,
                "chunks",
                chunks_total,
                "bytes",
                bytes_total,
                "avail",
                s.get("ecg_available_bytes", -1),
                "drop",
                s.get("ecg_drop_bytes_total", -1),
                "hi",
                s.get("ecg_ring_high_water", -1),
            )
            last_stats_ms = now

    print("done chunks", chunks_total, "bytes", bytes_total)
    print("final stats", h10.stats())

    try:
        h10.stop_ecg()
        print("ecg stopped")
    except polar_sdk.NotConnectedError:
        pass
finally:
    h10.disconnect()
    print("disconnected", h10.state())
