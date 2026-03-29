"""Simple PSFTP directory-listing demo.

Use this to confirm that PSFTP discovery, security, and directory listing are
working before you try downloads or recording control.
"""

import time
import polar_sdk

TARGET_DIR = "/"

h10 = polar_sdk.Device(required_capabilities=(polar_sdk.CAP_PSFTP_READ,))

try:
    print("connecting...")
    h10.connect(timeout_ms=15000)

    entries = None
    for attempt in range(1, 4):
        try:
            print("listing", TARGET_DIR, "(attempt", attempt, ")")
            entries = h10.list_dir(TARGET_DIR)
            break
        except Exception as exc:
            print("list_dir failed:", type(exc), exc)
            time.sleep_ms(600)

    if entries is None:
        raise RuntimeError("list_dir failed after retries")

    print("entries:", len(entries))
    for entry in entries:
        print(" ", entry)

    print("psftp stats:", {
        "tx": h10.stats().get("psftp_tx_frames_total"),
        "rx": h10.stats().get("psftp_rx_frames_total"),
        "last_error": h10.stats().get("psftp_last_error_code"),
    })
finally:
    h10.disconnect()
