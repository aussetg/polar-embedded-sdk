# Data formats

This page explains what the MicroPython read calls return.

If you are just trying to collect data, you do **not** need to understand every byte immediately.
But it helps to know what kind of data you are storing.

## `read_hr()`

`read_hr()` is the easiest API because it returns a Python tuple, not raw bytes.

Example shape:

```python
(
    "unknown",
    None,
    63,
    3,
    2,
    950,
    948,
    0,
    0,
)
```

Meaning:

- item 0: source label
- item 1: timestamp placeholder
- item 2: BPM
- item 3: flags
- item 4: how many RR values are valid
- items 5-8: RR intervals in milliseconds

## `read_ecg()`

`read_ecg()` returns a `bytes` object.

Each ECG sample is:

- **4 bytes**
- little-endian signed integer
- unit: **microvolts**

So the byte length should usually be a multiple of 4.

### Example: convert ECG bytes to Python integers

```python
def parse_ecg_chunk(chunk):
    out = []
    for i in range(0, len(chunk), 4):
        sample = int.from_bytes(chunk[i:i+4], "little", signed=True)
        out.append(sample)
    return out
```

## `read_acc()`

`read_acc()` also returns a `bytes` object.

Each accelerometer sample is:

- **6 bytes total**
- `x`: 2 bytes, signed little-endian
- `y`: 2 bytes, signed little-endian
- `z`: 2 bytes, signed little-endian
- unit: **mg**

So the byte length should usually be a multiple of 6.

### Example: convert ACC bytes to `(x, y, z)` tuples

```python
def parse_acc_chunk(chunk):
    out = []
    for i in range(0, len(chunk), 6):
        x = int.from_bytes(chunk[i:i+2], "little", signed=True)
        y = int.from_bytes(chunk[i+2:i+4], "little", signed=True)
        z = int.from_bytes(chunk[i+4:i+6], "little", signed=True)
        out.append((x, y, z))
    return out
```

## Generic `read_stream()` format

`read_stream()` and `read_stream_into()` return a **framed binary chunk**.

This is useful if you want one script to work with HR, ECG, and ACC using the same read function.

## Decoded chunk v1 layout

The chunk starts with a **28-byte header**.

### Header bytes

- byte 0: version
- byte 1: kind code
- byte 2: unit code
- byte 3: time base code
- byte 4: flags
- bytes 5-7: reserved
- bytes 8-11: sample count, little-endian `uint32`
- bytes 12-19: `t0_ns`, little-endian `int64`
- bytes 20-23: `dt_ns`, little-endian `int32`
- bytes 24-27: `sample_size_bytes`, little-endian `uint32`
- bytes 28...: payload

### Current kind codes

- `0` = HR
- `1` = ECG
- `2` = ACC

### Current unit codes

- `1` = BPM-style HR payload
- `2` = microvolts
- `3` = mg

### Current time behavior

At the moment, generated chunks use:

- `time_base_code = 0`
- no embedded timestamps in the header flags

So if you need precise wall-clock timing, you should timestamp chunks in your Python script when you read them.

## Generic HR payload in `read_stream("hr")`

For decoded HR, the payload is 12 bytes:

- bytes 0-1: BPM, `uint16`
- byte 2: public flags
- byte 3: RR count
- bytes 4-11: up to four RR intervals as little-endian `uint16`

## Example: parse `read_stream()` output

```python
def parse_stream_chunk(chunk):
    if len(chunk) < 28:
        return None

    version = chunk[0]
    kind = chunk[1]
    unit = chunk[2]
    flags = chunk[4]
    sample_count = int.from_bytes(chunk[8:12], "little")
    sample_size = int.from_bytes(chunk[24:28], "little")
    payload = chunk[28:]

    return {
        "version": version,
        "kind": kind,
        "unit": unit,
        "flags": flags,
        "sample_count": sample_count,
        "sample_size": sample_size,
        "payload": payload,
    }
```

## Storage advice

If you are doing long experiments:

- save raw chunks to files,
- save timestamps from your Python loop alongside them,
- decode later on your computer.

That is usually safer than doing heavy processing on the microcontroller.

## Sanity checks

When debugging data collection, these quick checks help:

- ECG chunk length should usually be divisible by 4
- ACC chunk length should usually be divisible by 6
- `read_hr()` should often return a non-`None` BPM if the strap contact is good
- if data stops changing, inspect `stats()`