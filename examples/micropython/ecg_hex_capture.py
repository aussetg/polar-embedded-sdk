#!/usr/bin/env python3
"""Capture ECG chunks as hex over the USB serial connection.

Typical host command:

    mpremote connect /dev/ttyACM0 run :ecg_hex_capture.py > ecg_capture.txt

Data lines are emitted as:

    DATA,<host_ms>,<n_bytes>,<hex_payload>

Comment lines start with '#'.
"""

import time
import ubinascii
import polar_sdk

TARGET_ADDR = None
NAME_PREFIX = "Polar"
DURATION_S = 120
CONNECT_TIMEOUT_MS = 15000
SAMPLE_RATE = 130
READ_MAX_BYTES = 1024
READ_TIMEOUT_MS = 1000
STALE_DATA_TIMEOUT_MS = 5000
PROGRESS_EVERY_S = 10


def make_device():
    kwargs = {"required_capabilities": (polar_sdk.CAP_STREAM_ECG,)}
    if TARGET_ADDR:
        kwargs["addr"] = TARGET_ADDR
    else:
        kwargs["name_prefix"] = NAME_PREFIX
    return polar_sdk.Device(**kwargs)


h10 = make_device()

print("# polar_sdk.version=%s" % polar_sdk.version())
print("# target_addr=%s" % (TARGET_ADDR or "auto"))
print("# duration_s=%d sample_rate=%d" % (DURATION_S, SAMPLE_RATE))
print("# format=DATA,host_ms,n_bytes,hex_payload")
print("# sample_format=int32_le_microvolts")

chunk_count = 0
bytes_total = 0
start_ms = time.ticks_ms()
last_data_ms = start_ms
last_progress_ms = start_ms

try:
    h10.connect(timeout_ms=CONNECT_TIMEOUT_MS)
    h10.start_ecg(sample_rate=SAMPLE_RATE)

    while time.ticks_diff(time.ticks_ms(), start_ms) < DURATION_S * 1000:
        chunk = h10.read_ecg(max_bytes=READ_MAX_BYTES, timeout_ms=READ_TIMEOUT_MS)
        now = time.ticks_ms()

        if chunk:
            chunk_count += 1
            bytes_total += len(chunk)
            last_data_ms = now
            print(
                "DATA,%d,%d,%s"
                % (
                    time.ticks_diff(now, start_ms),
                    len(chunk),
                    ubinascii.hexlify(chunk).decode(),
                )
            )

        if time.ticks_diff(now, last_data_ms) > STALE_DATA_TIMEOUT_MS:
            print("# stale_timeout_ms=%d" % STALE_DATA_TIMEOUT_MS)
            break

        if time.ticks_diff(now, last_progress_ms) >= PROGRESS_EVERY_S * 1000:
            stats = h10.stats()
            print(
                "# progress,t_s=%d,chunks=%d,bytes=%d,avail=%d,drop=%d"
                % (
                    time.ticks_diff(now, start_ms) // 1000,
                    chunk_count,
                    bytes_total,
                    stats.get("ecg_available_bytes", -1),
                    stats.get("ecg_drop_bytes_total", -1),
                )
            )
            last_progress_ms = now

except KeyboardInterrupt:
    print("# interrupted")
except Exception as exc:
    print("# error,%s,%s" % (type(exc).__name__, exc))
finally:
    try:
        h10.stop_ecg()
    except Exception:
        pass
    h10.disconnect()
    print("# chunks=%d" % chunk_count)
    print("# bytes_total=%d" % bytes_total)
    print("# final_state=%s" % h10.state())
