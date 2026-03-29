# MicroPython SDK

The MicroPython SDK is the user-facing `polar_sdk` module built from `polar_sdk/mpy/mod_polar_sdk.c`.

This documentation assumes you are a researcher or experimenter who wants to:

- connect to a Polar H10 from a Pico 2 W,
- read heart rate, ECG, or accelerometer data,
- list or download files from the sensor,
- control H10 onboard HR recording,
- do all of that with as little BLE knowledge as possible.

## The basic idea

You usually only need four steps:

1. create a `polar_sdk.Device`
2. connect
3. start a stream or recording
4. read data or check status

## Smallest useful example

```python
import polar_sdk

h10 = polar_sdk.Device(
    name_prefix="Polar",
    required_capabilities=(polar_sdk.CAP_STREAM_HR,),
)

try:
    h10.connect(timeout_ms=15000)
    h10.start_hr()
    sample = h10.read_hr(timeout_ms=5000)
    print(sample)
finally:
    h10.disconnect()
```

## What this module is good at

- easy connect/disconnect
- simple experiment scripts
- on-device data collection
- low Python complexity because BLE-critical work stays in C

## What this module is not

- it is not CPython on your laptop
- it is not a general BLE toolkit
- it currently supports one active `polar_sdk.Device` transport at a time

## Recommended reading order

1. [Quick start](./quickstart.md)
2. [Research workflows](./research_workflows.md)
3. [API reference](./api_reference.md)
4. [Data formats](./data_formats.md)

## Examples

Real scripts live in `examples/micropython/`.

Start with:

- `examples/micropython/hello_polar.py`
- `examples/micropython/hr_read_demo.py`
- `examples/micropython/ecg_read_demo.py`
- `examples/micropython/hr_csv_capture.py`
- `examples/micropython/ecg_hex_capture.py`
- `examples/micropython/psftp_list_demo.py`
- `examples/micropython/recording_hr_demo.py`

## One important practical rule

Keep your Python loop simple.

Avoid printing too much or doing expensive work every time data arrives. A good pattern is:

- read data,
- append or save it,
- print short progress messages only once every few seconds.

That keeps the system more stable during long recordings.