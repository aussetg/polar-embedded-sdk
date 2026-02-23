# Minimal smoke test for the C-backed "polar_ble" MicroPython module.
#
# This is expected to run on-device after flashing firmware that includes
# the user C module from polar_ble/mpy.

import polar_ble

print("polar_ble.version() =", polar_ble.version())
print("polar_ble.build_info() =", polar_ble.build_info())
print(
    "feature flags =",
    polar_ble.FEATURE_HR,
    polar_ble.FEATURE_ECG,
    polar_ble.FEATURE_PSFTP,
)
print(
    "service bits =",
    polar_ble.SERVICE_HR,
    polar_ble.SERVICE_ECG,
    polar_ble.SERVICE_PSFTP,
    polar_ble.SERVICE_ALL,
)
print("HAS_BTSTACK =", polar_ble.HAS_BTSTACK)

h10 = polar_ble.H10(None, required_services=polar_ble.SERVICE_ALL)
print("h10.state() =", h10.state())
print("h10.required_services() =", h10.required_services())
print("h10.is_connected() =", h10.is_connected())
print("h10.stats() =", h10.stats())

print(
    "error types:",
    polar_ble.Error,
    polar_ble.TimeoutError,
    polar_ble.NotConnectedError,
    polar_ble.ProtocolError,
    polar_ble.BufferOverflowError,
)

try:
    # Keep timeout small for smoke checks.
    h10.connect(timeout_ms=200, required_services=polar_ble.SERVICE_HR)
    print("connect() succeeded")
except polar_ble.TimeoutError as e:
    print("connect() TimeoutError OK for smoke:", e)

h10.disconnect()
print("disconnect() OK")
print("h10.stats() after calls =", h10.stats())
