"""Basic HR read demo.

Connects to a Polar device, enables HR notifications, reads a small number of
samples, and then stops cleanly.
"""

import polar_sdk

SAMPLES = 10
MAX_EMPTY = 20

h10 = polar_sdk.Device(name_prefix="Polar")
print("version", polar_sdk.version())

try:
    h10.connect(timeout_ms=10000)
    print("connected state", h10.state())
    print(
        "handles",
        h10.stats().get("hr_measurement_handle"),
        h10.stats().get("pmd_cp_handle"),
        h10.stats().get("psftp_mtu_handle"),
    )

    started = False
    try:
        h10.start_hr()
        started = True
        print("hr started")
    except (
        polar_sdk.TimeoutError,
        polar_sdk.NotConnectedError,
        polar_sdk.ProtocolError,
    ) as exc:
        print("start_hr failed:", type(exc).__name__, exc)

    got = 0
    empty = 0
    while started and got < SAMPLES and empty < MAX_EMPTY:
        try:
            sample = h10.read_hr(timeout_ms=4000)
        except polar_sdk.NotConnectedError as exc:
            print("read_hr stopped:", exc)
            break
        if sample is None:
            print("no sample")
            empty += 1
            continue
        print("hr", sample)
        got += 1
        empty = 0

    if empty >= MAX_EMPTY:
        print("stopping after", MAX_EMPTY, "consecutive empty reads")

    try:
        h10.stop_hr()
        print("hr stopped")
    except polar_sdk.NotConnectedError:
        pass

    print("stats", h10.stats())
finally:
    h10.disconnect()
    print("disconnected", h10.state())
