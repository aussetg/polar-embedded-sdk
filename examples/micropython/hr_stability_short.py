import polar_sdk
import time

h10 = polar_sdk.H10(name_prefix="Polar")
print("version", polar_sdk.version())

try:
    h10.connect(timeout_ms=10000)
    print("connected", h10.state())
    h10.start_hr()
    print("hr started")

    t0 = time.ticks_ms()
    got = 0
    while time.ticks_diff(time.ticks_ms(), t0) < 15000:
        try:
            s = h10.read_hr(timeout_ms=500)
        except polar_sdk.NotConnectedError as exc:
            print("read stopped:", exc)
            break
        if s is not None:
            got += 1

    print("samples", got)
    print("stats", h10.stats())

    try:
        h10.stop_hr()
    except polar_sdk.NotConnectedError:
        pass
finally:
    h10.disconnect()
    print("done", h10.state())
