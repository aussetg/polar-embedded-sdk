# Minimal "hello polar" script for polar_sdk.Device.

import polar_sdk

h10 = polar_sdk.Device(
    name_prefix="Polar",
    required_capabilities=(polar_sdk.CAP_STREAM_HR,),
)

print("polar_sdk.version() =", polar_sdk.version())
print(
    "feature flags:",
    polar_sdk.FEATURE_HR,
    polar_sdk.FEATURE_ECG,
    polar_sdk.FEATURE_PSFTP,
)
print(
    "capability constants:",
    polar_sdk.CAP_STREAM_HR,
    polar_sdk.CAP_STREAM_ECG,
    polar_sdk.CAP_PSFTP_READ,
)
print("required capabilities:", h10.required_capabilities())
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
