import polar_sdk
import time

h10 = polar_sdk.H10(name_prefix="Polar")
print("version", polar_sdk.version())

try:
    h10.connect(timeout_ms=10000)
    print("connected", h10.state())
    h10.start_hr()
    print("hr started")

    for i in range(15):
        time.sleep_ms(1000)
        s = h10.stats()
        print(
            i + 1,
            "sec",
            "state",
            h10.state(),
            "connected",
            h10.is_connected(),
            "hr_notif",
            s.get("hr_notifications_total"),
            "disc08",
            s.get("disconnect_reason_0x08_total"),
        )
        if not h10.is_connected():
            break

    print("final stats", h10.stats())
    try:
        h10.stop_hr()
    except Exception as exc:
        print("stop_hr err", type(exc).__name__, exc)
finally:
    h10.disconnect()
    print("done", h10.state())
