# Minimal "hello driver" script for polar_ble.H10.

import polar_ble

h10 = polar_ble.H10(name_prefix="Polar", required_services=polar_ble.SERVICE_HR)

print("polar_ble.version() =", polar_ble.version())
print(
    "feature flags:",
    polar_ble.FEATURE_HR,
    polar_ble.FEATURE_ECG,
    polar_ble.FEATURE_PSFTP,
)
print(
    "service bits:",
    polar_ble.SERVICE_HR,
    polar_ble.SERVICE_ECG,
    polar_ble.SERVICE_PSFTP,
    polar_ble.SERVICE_ALL,
)
print("required services mask:", h10.required_services())
print("initial state:", h10.state())

try:
    h10.connect(timeout_ms=10000)
    print("connected:", h10.is_connected())
    print("stats:", h10.stats())
except polar_ble.TimeoutError as exc:
    print("connect timeout:", exc)
    print("stats:", h10.stats())
finally:
    h10.disconnect()

print("state after disconnect:", h10.state())
