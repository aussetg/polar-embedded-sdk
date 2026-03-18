# Minimal smoke test for the C-backed "polar_sdk" MicroPython module.
#
# This is expected to run on-device after flashing firmware that includes
# the user C module from polar_sdk/mpy.

import polar_sdk

print("polar_sdk.version() =", polar_sdk.version())
print("polar_sdk.build_info() =", polar_sdk.build_info())
print(
    "feature flags =",
    polar_sdk.FEATURE_HR,
    polar_sdk.FEATURE_ECG,
    polar_sdk.FEATURE_PSFTP,
)
print(
    "capability constants =",
    polar_sdk.CAP_STREAM_HR,
    polar_sdk.CAP_STREAM_ECG,
    polar_sdk.CAP_PSFTP_READ,
    polar_sdk.CAP_PSFTP_DELETE,
)
print("HAS_BTSTACK =", polar_sdk.HAS_BTSTACK)

h10 = polar_sdk.Device(None)
print("h10.state() =", h10.state())
print("h10.required_capabilities() =", h10.required_capabilities())
print("h10.is_connected() =", h10.is_connected())
print("h10.stats() =", h10.stats())

print(
    "error types:",
    polar_sdk.Error,
    polar_sdk.TimeoutError,
    polar_sdk.NotConnectedError,
    polar_sdk.ProtocolError,
    polar_sdk.BufferOverflowError,
)

try:
    # Keep timeout small for smoke checks.
    h10.connect(timeout_ms=200, required_capabilities=(polar_sdk.CAP_STREAM_HR,))
    print("connect() succeeded")
except polar_sdk.TimeoutError as e:
    print("connect() TimeoutError OK for smoke:", e)

h10.disconnect()
print("disconnect() OK")
print("h10.stats() after calls =", h10.stats())
