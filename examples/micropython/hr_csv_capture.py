#!/usr/bin/env python3
"""Capture Heart Rate data as CSV over the USB serial connection.

Typical host command:

    mpremote connect /dev/ttyACM0 run :hr_csv_capture.py > hr_capture.csv

Output format:

    host_ms,bpm,flags,rr_count,rr0_ms,rr1_ms,rr2_ms,rr3_ms

Comment lines start with '#'.
"""

import time
import polar_sdk

TARGET_ADDR = None
NAME_PREFIX = "Polar"
DURATION_S = 300
CONNECT_TIMEOUT_MS = 15000
READ_TIMEOUT_MS = 4000


def make_device():
    kwargs = {"required_capabilities": (polar_sdk.CAP_STREAM_HR,)}
    if TARGET_ADDR:
        kwargs["addr"] = TARGET_ADDR
    else:
        kwargs["name_prefix"] = NAME_PREFIX
    return polar_sdk.Device(**kwargs)


h10 = make_device()

print("# polar_sdk.version=%s" % polar_sdk.version())
print("# target_addr=%s" % (TARGET_ADDR or "auto"))
print("# duration_s=%d" % DURATION_S)
print("host_ms,bpm,flags,rr_count,rr0_ms,rr1_ms,rr2_ms,rr3_ms")

sample_count = 0
start_ms = time.ticks_ms()

try:
    h10.connect(timeout_ms=CONNECT_TIMEOUT_MS)
    h10.start_hr()

    while time.ticks_diff(time.ticks_ms(), start_ms) < DURATION_S * 1000:
        sample = h10.read_hr(timeout_ms=READ_TIMEOUT_MS)
        if sample is None:
            continue

        now_ms = time.ticks_diff(time.ticks_ms(), start_ms)
        bpm = sample[2]
        flags = sample[3]
        rr_count = sample[4]
        rr0_ms = sample[5]
        rr1_ms = sample[6]
        rr2_ms = sample[7]
        rr3_ms = sample[8]

        print(
            "%d,%d,%d,%d,%d,%d,%d,%d"
            % (now_ms, bpm, flags, rr_count, rr0_ms, rr1_ms, rr2_ms, rr3_ms)
        )
        sample_count += 1

except KeyboardInterrupt:
    print("# interrupted")
except Exception as exc:
    print("# error,%s,%s" % (type(exc).__name__, exc))
finally:
    try:
        h10.stop_hr()
    except Exception:
        pass
    h10.disconnect()
    print("# samples=%d" % sample_count)
    print("# final_state=%s" % h10.state())
