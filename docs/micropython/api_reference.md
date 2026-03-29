# API reference

This page documents the current `polar_sdk` MicroPython API.

## Module-level items

### `polar_sdk.version()`

Returns a version string.

### `polar_sdk.build_info()`

Returns build metadata as a dictionary.

Useful fields include firmware preset, git SHA, dirty state, and build type.

### Exceptions

- `polar_sdk.Error`
- `polar_sdk.TimeoutError`
- `polar_sdk.NotConnectedError`
- `polar_sdk.ProtocolError`
- `polar_sdk.BufferOverflowError`
- `polar_sdk.SecurityError`
- `polar_sdk.UnsupportedError`

### Feature constants

- `polar_sdk.FEATURE_HR`
- `polar_sdk.FEATURE_ECG`
- `polar_sdk.FEATURE_PSFTP`

These are build-time flags. They tell you whether the firmware includes support for those features.

### Capability constants

- `polar_sdk.CAP_STREAM_HR`
- `polar_sdk.CAP_STREAM_ECG`
- `polar_sdk.CAP_STREAM_ACC`
- `polar_sdk.CAP_STREAM_PPG`
- `polar_sdk.CAP_STREAM_PPI`
- `polar_sdk.CAP_STREAM_GYRO`
- `polar_sdk.CAP_STREAM_MAG`
- `polar_sdk.CAP_RECORDING`
- `polar_sdk.CAP_PSFTP_READ`
- `polar_sdk.CAP_PSFTP_DELETE`

## `polar_sdk.Device(...)`

Constructor:

```python
polar_sdk.Device(addr=None, *, name_prefix="Polar", required_capabilities=())
```

Arguments:

- `addr`
  - exact BLE address as a string, or `None`
- `name_prefix`
  - advertisement name prefix to match, or `None`
- `required_capabilities`
  - tuple/list of capability constants

### Important note

Only one active `polar_sdk.Device` transport is supported at a time.

## Connection and state

### `state()`

Returns one of:

- `"idle"`
- `"scanning"`
- `"connecting"`
- `"discovering"`
- `"ready"`
- `"recovering"`

### `is_connected()`

Returns `True` only when the device is in the ready state.

### `connect(timeout_ms=10000, *, required_capabilities=None)`

Connects, performs discovery, and leaves the object ready for use.

Arguments:

- `timeout_ms`
  - total connection budget
- `required_capabilities`
  - optional per-call override

Raises:

- `TimeoutError` if the whole process times out
- `Error` on other connection failures

### `disconnect(*, timeout_ms=10000)`

Disconnects and clears local runtime state.

Safe to call in cleanup paths.

## Capabilities and telemetry

### `set_required_capabilities(caps)`

Sets the default capability filter for future connects.

### `required_capabilities()`

Returns the current default capability tuple.

### `capabilities()`

Requires an active connection.

Returns a dictionary describing:

- device identity,
- stream kinds and supported formats,
- recording support,
- PSFTP support,
- current security status.

This is the best way to inspect what the connected device/build combination can do.

### `stats()`

Returns a large dictionary of debug/telemetry counters.

Useful keys for everyday troubleshooting:

- `state`
- `connected`
- `connect_attempts_total`
- `connect_success_total`
- `last_disconnect_reason`
- `conn_encryption_key_size`
- `conn_bonded`
- `hr_notifications_total`
- `ecg_available_bytes`
- `ecg_drop_bytes_total`
- `acc_available_bytes`
- `psftp_last_error_code`

Use `stats()` whenever something stops working unexpectedly.

## Heart rate API

### `start_hr()`

Enables Heart Rate notifications.

### `stop_hr()`

Disables Heart Rate notifications.

### `read_hr(*, timeout_ms=0)`

Returns either:

- `None` if no new sample is available within the timeout, or
- a 9-item tuple:

```python
(
    source,
    timestamp,
    bpm,
    flags,
    rr_count,
    rr0_ms,
    rr1_ms,
    rr2_ms,
    rr3_ms,
)
```

Current meanings:

- `source`: currently always `"unknown"`
- `timestamp`: currently `None`
- `bpm`: heart rate in beats per minute
- `flags`: public flags bitfield
- `rr_count`: number of valid RR intervals in the tuple
- `rr*_ms`: RR interval values in milliseconds

## ECG API

### `start_ecg(*, sample_rate=130)`

Starts ECG streaming.

### `stop_ecg()`

Stops ECG streaming.

### `read_ecg(*, max_bytes=1024, timeout_ms=0)`

Returns a `bytes` object.

Format:

- payload is a sequence of little-endian signed `int32` ECG samples
- one sample = 4 bytes
- unit = microvolts

If no new data arrives before the timeout, returns `b""`.

## Accelerometer API

### `start_acc(*, sample_rate=50, range=8)`

Starts accelerometer streaming.

### `stop_acc()`

Stops accelerometer streaming.

### `read_acc(*, max_bytes=1024, timeout_ms=0)`

Returns a `bytes` object.

Format:

- payload is repeating triples of `x, y, z`
- each axis is little-endian signed `int16`
- one sample = 6 bytes total
- unit = mg

If no new data arrives before the timeout, returns `b""`.

## Generic stream API

The generic API is useful if you want one script shape for different stream kinds.

### `stream_default_config(kind)`

Examples:

- `stream_default_config("hr")`
- `stream_default_config("ecg")`
- `stream_default_config("acc")`

Returns a dictionary of default settings.

### `start_stream(kind, *, format=None, sample_rate_hz=None, range=None)`

Supported today:

- `kind="hr"`
- `kind="ecg"`
- `kind="acc"`

Notes:

- HR supports `decoded` and `raw`
- ECG currently supports `decoded` only
- ACC currently supports `decoded` only

### `stop_stream(kind)`

Stops the chosen stream.

### `read_stream(kind, *, max_bytes=1024, timeout_ms=0)`

Returns a `bytes` chunk in a generic framed format.

### `read_stream_into(kind, *, buf, timeout_ms=0)`

Reads the same generic framed format into a writable buffer and returns the number of bytes written.

See [Data formats](./data_formats.md) for the framed binary layout.

## PSFTP / filesystem API

These calls require a firmware build with PSFTP enabled.

### `list_dir(path)`

Arguments:

- `path` must be an absolute path like `"/"` or `"/SOME_DIR"`

Returns a list of dictionaries like:

```python
{"name": "SAMPLES.BPB", "size": 12345, "is_dir": False}
```

### `delete(path)`

Deletes a file by absolute path.

### `download(path, *, max_bytes=8192, timeout_ms=12000)`

Downloads one file into memory and returns it as `bytes`.

Use this only for relatively small files.

### `download_open(path, *, timeout_ms=12000)`

Starts a chunked download and returns a handle integer.

### `download_read(handle, buf, *, timeout_ms=12000)`

Fills a writable buffer.

Returns:

- number of bytes written,
- `0` at end-of-file.

### `download_close(handle)`

Closes the active chunked download.

### Important limitation

Only one download handle can be active at a time.

## Recording API

Current recording support is specifically for **H10 HR recording control**.

### `recording_default_config(kind)`

Currently useful for `kind="hr"`.

Returns for example:

```python
{"sample_type": "hr", "interval_s": 1}
```

### `recording_start(kind, *, sample_type=None, interval_s=1)`

For H10 HR recording:

- `kind` must be `"hr"`
- `sample_type` can be `"hr"` or `"rr"`
- `interval_s` can be `1` or `5`
- for `sample_type="rr"`, only `interval_s=1` is allowed

### `recording_stop(kind)`

Stops the chosen recording kind.

### `recording_status()`

Returns a dictionary like:

```python
{
    "active": False,
    "by_kind": {"hr": False},
    "active_kinds": [],
}
```

### `recording_list()`

Returns a list of dictionaries describing stored recordings.

Typical keys:

- `recording_id`
- `kind`
- `state`
- `bytes_total`
- `start_time_ns`
- `time_base`
- `path`

### `recording_delete(recording_id)`

Deletes a stored recording by its recording ID.

### Important practical rule

On H10, starting a new HR recording can fail if a stopped recording is still stored.
If so, list existing recordings and delete them first.

## Suggested usage patterns

### For live monitoring

- `connect()`
- `start_hr()` or `start_ecg()`
- periodic `read_*()`
- occasional `stats()`
- `stop_*()`
- `disconnect()`

### For file retrieval

- `connect()`
- `list_dir()`
- `download()` or chunked download functions
- `disconnect()`

### For H10 onboard recording

- `connect()`
- `recording_status()`
- `recording_list()`
- `recording_start()`
- later `recording_stop()`
- `recording_list()` again
- `disconnect()`