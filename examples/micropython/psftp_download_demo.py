import time
import polar_ble

ROOT_DIR = "/"
MAX_BYTES = 4096

h10 = polar_ble.H10(required_services=polar_ble.SERVICE_PSFTP)

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
    for name, size in entries:
        if not name.endswith("/") and size <= MAX_BYTES:
            candidates.append((name, size))

    if not candidates:
        print("no file <=", MAX_BYTES, "bytes in", ROOT_DIR)
    else:
        name, size = candidates[0]
        path = ROOT_DIR + name

        data = None
        for attempt in range(1, 4):
            try:
                print("downloading", path, "size", size, "(attempt", attempt, ")")
                data = h10.download(path, max_bytes=MAX_BYTES, timeout_ms=15000)
                break
            except Exception as exc:
                print("download failed:", type(exc), exc)
                time.sleep_ms(600)

        if data is None:
            raise RuntimeError("download failed after retries")

        print("downloaded bytes:", len(data))
        print("first 32 bytes:", data[:32])

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
    h10.disconnect()
