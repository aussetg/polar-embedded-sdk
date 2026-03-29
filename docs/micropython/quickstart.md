# Quick start

This page is the shortest path from "firmware flashed" to "I can read data".

## Before you start

You need:

- a firmware build that includes the `polar_sdk` module,
- a Polar H10 nearby,
- the strap worn correctly so it is actively measuring,
- a USB serial connection to the Pico.

Build/flash instructions live in:

- [`../howto/build_micropython_with_polar_module.md`](../howto/build_micropython_with_polar_module.md)

## Step 1: check that the module exists

Run `examples/micropython/smoke_test.py`.

Expected basics:

- `import polar_sdk` works
- `polar_sdk.version()` prints a version string
- `polar_sdk.build_info()` returns build metadata

## Step 2: create a device object

```python
import polar_sdk

h10 = polar_sdk.Device(
    name_prefix="Polar",
    required_capabilities=(polar_sdk.CAP_STREAM_HR,),
)
```

### What those settings mean

- `name_prefix="Polar"`
  - look for advertisements whose name starts with `Polar`
- `required_capabilities=(polar_sdk.CAP_STREAM_HR,)`
  - only accept a device that supports the feature you need

If you know the exact BLE address of your strap, you can use `addr="AA:BB:CC:DD:EE:FF"` instead.

## Step 3: connect

```python
h10.connect(timeout_ms=15000)
print(h10.state())
```

If successful, the state should become `"ready"`.

## Step 4: choose one task

### Read heart rate

```python
h10.start_hr()
sample = h10.read_hr(timeout_ms=5000)
print(sample)
```

### Read ECG

```python
h10.start_ecg(sample_rate=130)
chunk = h10.read_ecg(max_bytes=1024, timeout_ms=1000)
print(len(chunk))
```

### Read accelerometer data

```python
h10.start_acc(sample_rate=50, range=8)
chunk = h10.read_acc(max_bytes=1024, timeout_ms=1000)
print(len(chunk))
```

### List files on the device

```python
entries = h10.list_dir("/")
for entry in entries:
    print(entry)
```

## Step 5: always disconnect cleanly

```python
h10.disconnect()
```

Use `try/finally` so disconnect still happens if your script errors.

```python
try:
    h10.connect(timeout_ms=15000)
    # do work
finally:
    h10.disconnect()
```

## Choosing the right capability flag

Common ones are:

- `polar_sdk.CAP_STREAM_HR`
- `polar_sdk.CAP_STREAM_ECG`
- `polar_sdk.CAP_STREAM_ACC`
- `polar_sdk.CAP_PSFTP_READ`
- `polar_sdk.CAP_RECORDING`

Examples:

```python
# ECG only
required_capabilities=(polar_sdk.CAP_STREAM_ECG,)

# file access only
required_capabilities=(polar_sdk.CAP_PSFTP_READ,)

# recording + file access
required_capabilities=(polar_sdk.CAP_RECORDING, polar_sdk.CAP_PSFTP_READ)
```

## Typical mistakes

### `TimeoutError` on connect

Usually means one of these:

- the strap is not advertising,
- the strap is not being worn,
- the wrong device is nearby,
- the timeout is too short.

### `NotConnectedError` while reading

The BLE link dropped. Call `stats()` to inspect counters such as disconnect reasons.

### `UnsupportedError`

Your firmware build does not include that feature, or the current device does not expose it.

## Strong recommendation for researchers

Start from a working example in `examples/micropython/` and modify it gradually.

Do **not** try to write a large script from scratch on the first attempt.

For ready-to-run collection workflows, continue with:

- [Research workflows](./research_workflows.md)