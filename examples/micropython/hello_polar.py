# Minimal "hello polar" script for polar_sdk.H10.

import polar_sdk

h10 = polar_sdk.H10(name_prefix="Polar", required_services=polar_sdk.SERVICE_HR)

print("polar_sdk.version() =", polar_sdk.version())
print(
    "feature flags:",
    polar_sdk.FEATURE_HR,
    polar_sdk.FEATURE_ECG,
    polar_sdk.FEATURE_PSFTP,
)
print(
    "service bits:",
    polar_sdk.SERVICE_HR,
    polar_sdk.SERVICE_ECG,
    polar_sdk.SERVICE_PSFTP,
    polar_sdk.SERVICE_ALL,
)
print("required services mask:", h10.required_services())
print("initial state:", h10.state())

try:
    h10.connect(timeout_ms=10000)
    print("connected:", h10.is_connected())
    print("stats:", h10.stats())
except polar_sdk.TimeoutError as exc:
    print("connect timeout:", exc)
    print("stats:", h10.stats())
finally:
    h10.disconnect()

print("state after disconnect:", h10.state())
