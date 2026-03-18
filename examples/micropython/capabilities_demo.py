#!/usr/bin/env python3
# Connect to a Polar device and print the public capability surface.

import polar_sdk

TARGET_ADDR = None
NAME_PREFIX = "Polar"
CONNECT_TIMEOUT_MS = 20000


def make_device():
    if TARGET_ADDR:
        return polar_sdk.Device(addr=TARGET_ADDR)
    return polar_sdk.Device(name_prefix=NAME_PREFIX)


h = make_device()

try:
    print("version", polar_sdk.version())
    print("build", polar_sdk.build_info())
    print("initial state", h.state())
    print("required capabilities", h.required_capabilities())

    h.connect(timeout_ms=CONNECT_TIMEOUT_MS)
    print("connected", h.state())

    caps = h.capabilities()
    print("device", caps.get("device"))
    print("streams", caps.get("streams"))
    print("recording", caps.get("recording"))
    print("psftp", caps.get("psftp"))
    print("security", caps.get("security"))

    stream_kinds = caps.get("streams", {}).get("kinds", ())
    for kind in stream_kinds:
        try:
            print("stream default", kind, h.stream_default_config(kind))
        except Exception as exc:
            print("stream default error", kind, type(exc).__name__, exc)

    recording_kinds = caps.get("recording", {}).get("kinds", ())
    for kind in recording_kinds:
        try:
            print("recording default", kind, h.recording_default_config(kind))
        except Exception as exc:
            print("recording default error", kind, type(exc).__name__, exc)

    stats = h.stats()
    print(
        "stats subset",
        {
            "bonded": stats.get("conn_bonded"),
            "enc": stats.get("conn_encryption_key_size"),
            "last_hci_status": stats.get("last_hci_status"),
            "last_att_status": stats.get("last_att_status"),
        },
    )
finally:
    h.disconnect()
    print("done", h.state())
