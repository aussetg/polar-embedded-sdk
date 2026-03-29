#!/usr/bin/env python3
"""H10 onboard HR recording-control demo.

The script checks current recording state, refuses to proceed if old stopped
recordings are still stored, starts a short HR recording, then stops it.
"""

import time
import polar_sdk

RECORD_SECONDS = 5

h10 = polar_sdk.Device(
    name_prefix="Polar",
    required_capabilities=(
        polar_sdk.CAP_RECORDING,
        polar_sdk.CAP_PSFTP_READ,
    ),
)

try:
    print("version", polar_sdk.version())
    h10.connect(timeout_ms=20000)
    print("connected", h10.state())
    print("recording default", h10.recording_default_config("hr"))
    print("recordings before", h10.recording_list())

    status = h10.recording_status()
    print("initial status", status)

    if status.get("by_kind", {}).get("hr"):
        print("stopping existing HR recording first")
        h10.recording_stop("hr")
        print("status after pre-stop", h10.recording_status())

    remaining = h10.recording_list()
    if remaining:
        raise RuntimeError(
            "existing H10 recording(s) present; delete explicitly with recording_delete() before running this demo"
        )

    print("starting HR recording")
    h10.recording_start("hr", sample_type="hr", interval_s=1)

    for sec in range(RECORD_SECONDS):
        time.sleep_ms(1000)
        print("t", sec + 1, "status", h10.recording_status())

    print("stopping HR recording")
    h10.recording_stop("hr")
    print("final status", h10.recording_status())
    print(
        "pairing fields",
        {
            "bonded": h10.stats().get("conn_bonded"),
            "enc": h10.stats().get("conn_encryption_key_size"),
            "sm_status": h10.stats().get("sm_last_pairing_status"),
            "sm_reason": h10.stats().get("sm_last_pairing_reason"),
        },
    )
finally:
    h10.disconnect()
    print("done", h10.state())
